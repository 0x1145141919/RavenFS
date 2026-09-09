/* ═══════════════════════════════════════════════════════════════════
 * test_pwrite_big.cpp — pwrite 真盘规模阶梯（step4：大文件 + 超多 extent）
 *
 * 被测合同（pwrite_pipeline.md §4 在真盘几何上的量级验证）：
 *   B1  超大 direct 文件：fresh mkfs 低址连续区顺序写 GB 级（默认 8GB，
 *       可参数化冲 64GB）——direct 单 run 无尺寸上限 + 逐 chunk 尾贴保形；
 *       全量读回校验（真盘才有意义的规模）。
 *   B2  超多 extent：骨架（direct 1 块 + 拦路虎 → 洞写撞墙物化上树 2 记录）
 *       → 洞写 ×N（每 8KB 写 4KB，逻辑洞隔 1 块 → 无锚纯插永不融合）→
 *       树滚到 N 条记录（默认 10^5，可冲 10^6；叶序 337 → 10^5 记录 ≈ 300
 *       叶 2~3 层树，分裂/预算池/平衡在真实规模滚动）。
 *       断言：audit 结构不变量 + records==N + 账本 == N + 节点×2（零泄漏）
 *       + 全量写块读回。
 *
 * 验证策略：同 smoke（测试侧 hack helper，不信任接口互验）。树审计
 * audit_run 抄 test_extent_fusion（DFS 结构不变量 + 兄弟链序）。
 *
 * 环境纪律：<device> 外部先 mkfs（跑法同 smoke）。B1 先占低址连续区，
 * B2 骨架锚 B1 尾（hint 低址序确定性，D4 实证同款）。
 *
 * 用法：ravenfs_juv_pwrite_big <device> [b1_gb] [b2_records]
 *       默认 b1_gb=8 b2_records=100000；b1_gb=0 跳过 B1，b2_records=0 跳过 B2。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "../include/ravenfs/inode_cache_radix.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#include "../include/ravenfs/bitmap_slices.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
static constexpr uint64_t FS_BLOCK = 4096;
static constexpr uint64_t INLINE_MAX = 160;

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK: return "RC_OK"; case RC_ERR_OPEN: return "RC_ERR_OPEN";
    case RC_ERR_MAGIC: return "RC_ERR_MAGIC"; case RC_ERR_VERSION: return "RC_ERR_VERSION";
    case RC_ERR_CRC: return "RC_ERR_CRC"; case RC_ERR_BITMAP_SELF: return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST: return "RC_ERR_EXIST"; case RC_ERR_NOT_FOUND: return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK: return "RC_ERR_NO_BLOCK"; case RC_ERR_NO_INODE: return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN: return "RC_ERR_NAME_LEN"; case RC_ERR_IO: return "RC_ERR_IO";
    case RC_ERR_STATE: return "RC_ERR_STATE"; case RC_ERR_NOT_EMPTY: return "RC_ERR_NOT_EMPTY";
    case RC_ERR_NOT_SUPPORTED: return "RC_ERR_NOT_SUPPORTED";
    case RC_ERR_OVERLAP: return "RC_ERR_OVERLAP";
    default: return "RC_???";
    }
}

static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.logical_processor_count = 1;
    return mnt;
}

static uint64_t make_reg(RavenFS_juvenile &fs, const char *name) {
    if (fs.root_dir_insert_dentry(name, INODE_TYPE_REGULAR) != RC_OK) return PTR_INVALID;
    uint64_t inum = 0;
    if (fs.root_dir_lookup(name, &inum) != RC_OK) return PTR_INVALID;
    return inum;
}

static uint64_t free_blocks(RavenFS_juvenile &fs) {
    return fs.bitmap_mgr->block_free_total();
}

static uint8_t pat_byte(uint64_t seed, uint64_t abs_off) {
    uint64_t x = seed + abs_off;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint8_t)(x >> 24);
}

struct ino_snap_t {
    uint64_t size, data_root;
    uint64_t direct_bit;
    int shape;
};
static ino_snap_t snap(RavenFS_juvenile &fs, uint64_t inum) {
    InodeSession s(fs.inode_cache_mgr, inum);
    ino_snap_t t; memset(&t, 0, sizeof(t));
    if (!s.inode) return t;
    t.size = s.inode->size;
    t.data_root = s.inode->data_root;
    t.direct_bit = s.inode->is_data_root_direct_bit;
    t.shape = (int)fs.ftype(s.inode);
    return t;
}
static const char *shape_name(int sh) {
    return sh == 0 ? "inline" : sh == 1 ? "direct" : sh == 2 ? "extent" : "???";
}

/* 区间取样（抄 fusion） */
static bool range_sample(RavenFS_juvenile &fs, uint64_t root, uint64_t base,
                         uint64_t len, std::vector<extent_t> &out) {
    out.clear();
    bptree_path_t trace;
    if (fs.extent_Bptree_walk(root, &trace, base) != RC_OK) return false;
    const uint64_t leaf = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;
    RavenFS_juvenile::extent_iterator it(&fs, leaf, glb);
    if (it.iterator_erro_flag) return false;
    if (glb == RavenFS_juvenile::EXTENT_LEAF_GLB_NONE) {
        ++it;
        if (it.iterator_erro_flag) return true;
    } else if (!hit) {
        ++it;
    }
    const uint64_t end = base + len;
    extent_t e;
    while (!it.iterator_erro_flag) {
        if (!it.current_extent(e)) break;
        if (e.fblkbase >= end) break;
        if (e.fblkbase + e.length > base) out.push_back(e);
        ++it;
    }
    return true;
}

/* 单逻辑块 → phys（direct 公式 / extent 记录；洞 → false） */
static bool resolve_phys(RavenFS_juvenile &fs, raven_Inode_t *ino,
                         uint64_t blk, uint64_t &phys) {
    int sh = (int)fs.ftype(ino);
    if (sh == 1) {
        if (ino->data_root == PTR_INVALID_52) return false;
        phys = ino->data_root + blk;
        return true;
    }
    if (sh == 2) {
        std::vector<extent_t> recs;
        if (!range_sample(fs, ino->data_root, blk, 1, recs)) return false;
        for (const extent_t &e : recs)
            if (e.fblkbase <= blk && blk < e.fblkbase + e.length) {
                phys = e.block_base.ptr + (blk - e.fblkbase);
                return true;
            }
        return false;
    }
    return false;
}

/* 区间校验（逐块 resolve + read_block + pat 比对）。返回坏块数。 */
static uint64_t verify_blocks(RavenFS_juvenile &fs, uint64_t inum,
                              uint64_t first_blk, uint64_t blk_count,
                              uint64_t seed, uint64_t size_bytes) {
    InodeSession s(fs.inode_cache_mgr, inum);
    if (!s.inode) return blk_count;
    raven_Inode_t *ino = s.inode;
    uint8_t buf[FS_BLOCK];
    uint64_t bad = 0;
    for (uint64_t k = 0; k < blk_count && bad == 0; k++) {
        const uint64_t blk = first_blk + k;
        uint64_t phys = 0;
        if (!resolve_phys(fs, ino, blk, phys)) { bad++; continue; }
        fs.read_block(phys, buf);
        const uint64_t lo = blk * FS_BLOCK;
        const uint64_t hi = lo + FS_BLOCK < size_bytes ? lo + FS_BLOCK : size_bytes;
        for (uint64_t i = lo; i < hi; i++)
            if (buf[i - lo] != pat_byte(seed, i)) { bad++; break; }
    }
    return bad;
}

/* ═══════════ 树完整性审计（DFS，抄 test_extent_fusion） ═══════════ */
struct audit_t {
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0, bad = 0;
};
static uint64_t audit_node(RavenFS_juvenile &fs, audit_t &a, uint64_t blk,
                           uint64_t expect_parent, bool expect_root,
                           uint64_t inum, uint64_t depth) {
    if (depth > 64) { a.bad++; return 0; }
    mdata_piece_session s(fs.mdata_piece_mgr, blk);
    if (s.index_error() || !s) { a.bad++; return 0; }
    Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
    if (!is_extent_node(meta)) { a.bad++; return 0; }
    if (meta->parent_ptr.is_root_node != expect_root) { a.bad++; return 0; }
    if (expect_root) { if (meta->parent_ptr.ptr != inum) a.bad++; }
    else { if (meta->parent_ptr.ptr != expect_parent) a.bad++; }
    a.nodes++;
    const uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
    if (!meta->parent_ptr.is_inner_node) {
        a.leaves++;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        a.records += cnt;
        if (cnt > 337) a.bad++;
        if (!expect_root && (cnt < 168 || cnt == 0)) a.bad++;
        for (uint16_t i = 0; i < cnt; i++) {
            const extent_t e = leaf->extents[i];
            if (i > 0) {
                const extent_t p = leaf->extents[i - 1];
                if (p.fblkbase + p.length > e.fblkbase) a.bad++;
            }
        }
        return cnt > 0 ? leaf->extents[cnt - 1].fblkbase +
                             leaf->extents[cnt - 1].length
                       : 0;
    }
    a.inner++;
    auto *inner = (Bptree_extent_inner_node_t *)s.data;
    if (cnt == 0) a.bad++;
    if (cnt > 337) a.bad++;
    if (expect_root && cnt == 1) a.bad++;
    if (!expect_root && cnt < 168) a.bad++;
    uint64_t prev_key = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        const extent_hint_t h = inner->extents[i];
        if (h.sub_node_ptr.ptr == 0 || h.sub_node_ptr.ptr == PTR_INVALID) a.bad++;
        if (i > 0 && h.key <= prev_key) a.bad++;
        prev_key = h.key;
        uint64_t child_max = audit_node(fs, a, h.sub_node_ptr.ptr, blk, false,
                                        inum, depth + 1);
        if (h.key != child_max) a.bad++;
    }
    return prev_key;
}

/* ═══════════════════════════════════════════════════════════════
 * B1：超大 direct 文件（顺序 chunk 写，尾贴保 direct）
 * ═══════════════════════════════════════════════════════════════ */
static void run_B1(RavenFS_juvenile &fs, uint64_t gb) {
    printf("── B1: 超大 direct（%llu GB 顺序写）──\n", (unsigned long long)gb);
    const uint64_t a = make_reg(fs, "big_direct");
    if (a == PTR_INVALID) { CHECK(false, "B1: 建文件失败"); return; }
    const uint64_t total = gb * (1ULL << 30);
    const uint64_t CHUNK = 32ULL << 20;   /* 32MB/次 */
    const uint64_t rounds = total / CHUNK;
    std::vector<uint8_t> buf(CHUNK);
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t r = 0; r < rounds; r++) {
        for (uint64_t i = 0; i < CHUNK; i++)
            buf[i] = pat_byte(0xB1, r * CHUNK + i);
        result_code_t rc = fs.direct_file_pwrite(a, r * CHUNK, CHUNK, buf.data());
        if (rc != RC_OK) { CHECK(false, "B1: chunk %llu rc=%s", (unsigned long long)r, rc_name(rc)); return; }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ws = std::chrono::duration<double>(t1 - t0).count();
    ino_snap_t s = snap(fs, a);
    CHECK(s.shape == 1 && s.direct_bit == 1, "B1: 全程保 direct got %s",
          shape_name(s.shape));
    CHECK(s.size == total, "B1: size == %llu GB", (unsigned long long)gb);
    printf("  写耗时 %.1fs（%.2f GB/s）base=%llu\n", ws,
           gb / ws, (unsigned long long)s.data_root);

    /* 全量读回（逐块 resolve + 比对） */
    auto t2 = std::chrono::steady_clock::now();
    const uint64_t blocks = total / FS_BLOCK;
    uint64_t bad = 0;
    const uint64_t STEP = 4096;   /* 进度打印粒度 */
    for (uint64_t k = 0; k < blocks && bad == 0; k += STEP) {
        uint64_t n = blocks - k < STEP ? blocks - k : STEP;
        bad += verify_blocks(fs, a, k, n, 0xB1, total);
    }
    auto t3 = std::chrono::steady_clock::now();
    double rs = std::chrono::duration<double>(t3 - t2).count();
    CHECK(bad == 0, "B1: 全量读回 %llu GB == 模式", (unsigned long long)gb);
    printf("  读回耗时 %.1fs（%.2f GB/s）\n", rs, gb / rs);
}

/* ═══════════════════════════════════════════════════════════════
 * B2：超多 extent（骨架上树后洞写 ×N 纯插）
 * 骨架：b 写 [0,1) direct → 拦路虎占 base+1 → 写 [2,3) 增长撞墙 →
 *       物化上树 2 记录（洞 [1,2)）。此后每 8KB 写 4KB：写段起点
 *       (2i) 与左邻记录终点 (2i-1) 隔 1 块洞 → 无锚纯插永不融合。
 * ═══════════════════════════════════════════════════════════════ */
static void run_B2(RavenFS_juvenile &fs, uint64_t n) {
    printf("── B2: 超多 extent（洞写 %llu 条记录）──\n", (unsigned long long)n);
    const uint64_t b = make_reg(fs, "big_ext");
    if (b == PTR_INVALID) { CHECK(false, "B2: 建文件失败"); return; }
    uint8_t buf[FS_BLOCK];

    /* 骨架 */
    for (uint64_t i = 0; i < FS_BLOCK; i++) buf[i] = pat_byte(0xB2, i);
    CHECK(fs.direct_file_pwrite(b, 0, FS_BLOCK, buf) == RC_OK, "B2: 骨架 [0,1) RC_OK");
    ino_snap_t s0 = snap(fs, b);
    const uint64_t base = s0.data_root;
    CHECK(s0.shape == 1, "B2: 骨架 direct（base=%llu）", (unsigned long long)base);
    const uint64_t blocker = fs.block_alloc_hard(1, 0);
    CHECK(blocker == base + 1, "B2: 拦路虎占 base+1（got %llu）",
          (unsigned long long)blocker);
    for (uint64_t i = 0; i < FS_BLOCK; i++) buf[i] = pat_byte(0xB2, 2 * FS_BLOCK + i);
    CHECK(fs.direct_file_pwrite(b, 2 * FS_BLOCK, FS_BLOCK, buf) == RC_OK,
          "B2: 骨架 [2,3) 撞墙 RC_OK");
    ino_snap_t s1 = snap(fs, b);
    CHECK(s1.shape == 2 && s1.direct_bit == 0 && s1.size == 3 * FS_BLOCK,
          "B2: 骨架物化上树 extent（size=3 块）got %s", shape_name(s1.shape));

    /* 洞写 ×(n-2)（i=2..n-1），每写块 [2i, 2i+1) */
    uint64_t fb = free_blocks(fs);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t writes_ok = 0;
    for (uint64_t i = 2; i < n; i++) {
        const uint64_t off = (2 * i) * FS_BLOCK;
        for (uint64_t k = 0; k < FS_BLOCK; k++) buf[k] = pat_byte(0xB2, off + k);
        result_code_t rc = fs.direct_file_pwrite(b, off, FS_BLOCK, buf);
        if (rc != RC_OK) { CHECK(false, "B2: 洞写 %llu rc=%s", (unsigned long long)i, rc_name(rc)); return; }
        writes_ok++;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ws = std::chrono::duration<double>(t1 - t0).count();
    ino_snap_t s2 = snap(fs, b);
    const uint64_t size_bytes = (2 * n - 1) * FS_BLOCK;
    CHECK(s2.size == size_bytes, "B2: size == %llu 块", (unsigned long long)(2 * n - 1));
    CHECK(writes_ok == n - 2, "B2: 洞写全部 RC_OK（%llu 次）",
          (unsigned long long)(n - 2));

    /* 树结构审计 + 记录数精确 */
    audit_t aud;
    audit_node(fs, aud, s2.data_root, 0, true, b, 0);
    CHECK(aud.bad == 0, "B2: 树结构不变量绿（nodes=%llu inner=%llu leaves=%llu）",
          (unsigned long long)aud.nodes, (unsigned long long)aud.inner,
          (unsigned long long)aud.leaves);
    CHECK(aud.records == n, "B2: 树记录精确 == %llu（got %llu）",
          (unsigned long long)n, (unsigned long long)aud.records);
    /* 账本：数据 n 块 + 树节点 aud.nodes × 2 块/节点（骨架后起算） */
    const uint64_t expect_used = (n - 2) + (aud.nodes - 1) * 2;
    const uint64_t used = fb - free_blocks(fs);
    CHECK(used == expect_used, "B2: 账本零泄漏（used=%llu 期望=%llu 树节点=%llu）",
          (unsigned long long)used, (unsigned long long)expect_used,
          (unsigned long long)aud.nodes);
    printf("  洞写耗时 %.1fs（%.0f 次/s）；树 %llu 节点 %llu 叶；记录 %llu\n",
           ws, (n - 2) / ws, (unsigned long long)aud.nodes,
           (unsigned long long)aud.leaves, (unsigned long long)aud.records);

    /* 全量写块读回（偶数逻辑块，逐块 resolve） */
    auto t2 = std::chrono::steady_clock::now();
    uint64_t bad = 0;
    for (uint64_t i = 0; i < n && bad == 0; i++) {
        const uint64_t blk = 2 * i;
        uint64_t phys = 0;
        InodeSession s(fs.inode_cache_mgr, b);
        if (!resolve_phys(fs, s.inode, blk, phys)) { bad++; continue; }
        fs.read_block(phys, buf);
        const uint64_t lo = blk * FS_BLOCK;
        for (uint64_t k = 0; k < FS_BLOCK; k++)
            if (buf[k] != pat_byte(0xB2, lo + k)) { bad++; break; }
    }
    auto t3 = std::chrono::steady_clock::now();
    double rs = std::chrono::duration<double>(t3 - t2).count();
    CHECK(bad == 0, "B2: %llu 个写块全量读回 == 模式", (unsigned long long)n);
    printf("  读回耗时 %.1fs（%llu 块 resolve）\n", rs, (unsigned long long)n);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <device> [b1_gb] [b2_records]\n", argv[0]);
        return 1;
    }
    uint64_t b1_gb = argc > 2 ? strtoull(argv[2], nullptr, 10) : 8;
    uint64_t b2_n  = argc > 3 ? strtoull(argv[3], nullptr, 10) : 100000;
    printf("device=%s b1_gb=%llu b2_records=%llu\n",
           argv[1], (unsigned long long)b1_gb, (unsigned long long)b2_n);

    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 1; }
    uint64_t rc = 0;
    RavenFS_juvenile fs(mnt, &rc);
    if (rc != RC_OK || !fs.is_mounted()) {
        printf("mount failed rc=%s\n", rc_name(rc));
        return 1;
    }

    if (b1_gb > 0) run_B1(fs, b1_gb);
    if (b2_n > 1) run_B2(fs, b2_n);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
