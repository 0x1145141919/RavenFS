/* ═══════════════════════════════════════════════════════════════════
 * test_pwrite_smoke.cpp — direct_file_pwrite 五段管线发烟（step1）
 *
 * 被测合同（Docs/pwrite_pipeline.md §4 + §11 待办 4 的集成首测）：
 *   A  inline→direct  保形路径：hard 连续候选成功（1 段 0 起始到尾）→
 *      phase1.5 保 direct → 跳 phase2 → phase3 → phase4 direct inode。
 *      顺带压 direct 稳态：覆盖写零新段（base 不动）+ 增长跨块尾贴保 direct。
 *   B  inline→extent  上树路径：写域整 hard 失败 → 降级裁剪 soft 裂段
 *      多段 → 物化建树（空根叶 + 逐段 map）→ phase4 留树。
 *   C  extent→extent  map-only 四段管线跑全（extent 旧态免判别直出）：
 *      洞写锚左邻 phys 尾 → map 左并（count 不变）/ 洞中段无锚纯插（+1 记录）/
 *      覆盖写零新段不碰树 / 尾增长锚尾贴 → 左并。树节点 alloc 只在物化
 *      建树期发生（不满叶零预算）→ 场景窗不会被 allocator 抢（分两阶段开窗）。
 *   D  direct 边界：覆盖跨块零新段 / 增长不跨块零新段（纯 size 推进）/
 *      **尾占领失败 → direct→extent 物化上树**（拦路虎占旧 run 尾后块；断言
 *      物化零搬移：记录1 phys == 原 base）/ 尾贴多块增长保 direct。
 *
 * 验证策略（09-09 拍板：audit = 测试侧 hack helper，不信任接口互验）：
 *   - 验证器 = 可信内部原语组合：#define private public 通道访问
 *     ftype / InodeSession / extent_Bptree_walk+iterator（range_sample）/
 *     read_block / bitmap 账本 —— 全部有专项测试背书，不实现 pread，
 *     pread 不做 pwrite 的校验器（未测接口互验 = 两未知数）。
 *   - 内容校验 = 逐逻辑块直读盘面，与 (seed, 绝对字节偏移) 派生模式比对；
 *     direct 公式 / extent 树映射给出 phys，读不走任何 FS 业务读接口。
 *
 * 环境纪律：<device> 需外部先 mkfs（镜像/真盘通用；跑法见仓库惯例）。
 * 场景独立性：B 组 claim-all 雕塑前不依赖 A 组盘面（claim 自记录自由区
 * 起点 C0，断言全用相对量）。
 *
 * 用法：ravenfs_juv_pwrite_smoke <device>
 * 跑法：mkfs.ravenfs <device> -f --inode-count=262144 && \
 *       ravenfs_juv_pwrite_smoke <device>
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession 完整类型 */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_session（树审计） */
#include "../include/ravenfs/bitmap_slices.hpp"       /* bitmap_mgr free 审计 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

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

/* ── 挂载（mmap 后端；镜像/真盘由外部 mkfs）── */
static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.logical_processor_count = 1;   /* 单 slice：分配确定性 */
    return mnt;
}

/* ── 正门建 REG 文件 ── */
static uint64_t make_reg(RavenFS_juvenile &fs, const char *name) {
    if (fs.root_dir_insert_dentry(name, INODE_TYPE_REGULAR) != RC_OK) return PTR_INVALID;
    uint64_t inum = 0;
    if (fs.root_dir_lookup(name, &inum) != RC_OK) return PTR_INVALID;
    return inum;
}

/* ── 审计读数 ── */
static uint64_t free_blocks(RavenFS_juvenile &fs) {
    return fs.bitmap_mgr->block_free_total();
}

/* inode 快照 + 形态（hack 通道：ftype / 枚举 private→public） */
struct ino_snap_t {
    uint64_t size, data_root;
    uint64_t direct_bit;
    int shape;   /* 0=inline 1=direct 2=extent */
};
static ino_snap_t snap(RavenFS_juvenile &fs, uint64_t inum) {
    InodeSession s(fs.inode_cache_mgr, inum);
    ino_snap_t t;
    memset(&t, 0, sizeof(t));
    if (!s.inode) return t;
    raven_Inode_t *ino = s.inode;
    t.size = ino->size;
    t.data_root = ino->data_root;
    t.direct_bit = ino->is_data_root_direct_bit;
    t.shape = (int)fs.ftype(ino);
    return t;
}
static const char *shape_name(int sh) {
    return sh == 0 ? "inline" : sh == 1 ? "direct" : sh == 2 ? "extent" : "???";
}

/* extent 树记录数（叶链遍历；轻量审计，抄 phase1 惯例） */
static uint64_t tree_records(RavenFS_juvenile &fs, uint64_t root) {
    uint64_t blk = root, total = 0;
    for (int d = 0; d < 64; d++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) return PTR_INVALID;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_extent_node(m)) return PTR_INVALID;
        if (!m->parent_ptr.is_inner_node) {
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            total += (uint16_t)leaf->meta.parent_ptr.entry_count;
            blk = leaf->meta.ptrs[2].ptr;
            if (blk == PTR_INVALID_52) return total;
            continue;
        }
        auto *inner = (Bptree_extent_inner_node_t *)s.data;
        if (inner->meta.parent_ptr.entry_count == 0) return PTR_INVALID;
        blk = inner->extents[0].sub_node_ptr.ptr;
    }
    return PTR_INVALID;
}

/* ── 区间取样：与 [base, base+len) 相交的整条记录（抄 fusion 工具）── */
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

/* ── 内容模式：(seed, 绝对字节偏移) 派生，写/读共用同一公式 ── */
static uint8_t pat_byte(uint64_t seed, uint64_t abs_off) {
    uint64_t x = seed + abs_off;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint8_t)(x >> 24);
}
static void fill_buf(uint8_t *buf, uint64_t len, uint64_t seed, uint64_t base_off) {
    for (uint64_t i = 0; i < len; i++) buf[i] = pat_byte(seed, base_off + i);
}

/* ── 逻辑块 → phys 解析（direct 公式 / extent 树映射；洞 → false）── */
static bool resolve_phys(RavenFS_juvenile &fs, raven_Inode_t *ino,
                         uint64_t blk, uint64_t &phys) {
    int sh = (int)fs.ftype(ino);
    if (sh == 1) {   /* direct：base + 逻辑块号 */
        if (ino->data_root == PTR_INVALID_52) return false;
        phys = ino->data_root + blk;
        return true;
    }
    if (sh == 2) {   /* extent：walk+iterator 求覆盖记录 */
        std::vector<extent_t> recs;
        if (!range_sample(fs, ino->data_root, blk, 1, recs)) return false;
        for (const extent_t &e : recs)
            if (e.fblkbase <= blk && blk < e.fblkbase + e.length) {
                phys = e.block_base.ptr + (blk - e.fblkbase);
                return true;
            }
        return false;   /* 洞 */
    }
    return false;       /* inline：无外置块 */
}

/* ── 读回校验：[off, off+len) 逐字节 == pat(seed)。可信原语组合。── */
static void verify_range(RavenFS_juvenile &fs, uint64_t inum,
                         uint64_t off, uint64_t len, uint64_t seed, const char *tag) {
    InodeSession s(fs.inode_cache_mgr, inum);
    if (!s.inode) { CHECK(false, "%s: inode 不可读", tag); return; }
    raven_Inode_t *ino = s.inode;
    const int sh = (int)fs.ftype(ino);

    if (sh == 0) {   /* inline：内容在 inode 内联区 */
        if (off + len > INLINE_MAX) { CHECK(false, "%s: inline 越界读", tag); return; }
        bool ok = true;
        for (uint64_t i = 0; i < len; i++)
            if (ino->inline_data[off + i] != pat_byte(seed, off + i)) { ok = false; break; }
        CHECK(ok, "%s: inline 内容逐字节 == 模式 (%lluB @%llu)", tag,
              (unsigned long long)len, (unsigned long long)off);
        return;
    }

    const uint64_t first = off >> 12, last = (off + len + 4095) >> 12;
    uint8_t buf[FS_BLOCK];
    uint64_t bad = 0;
    for (uint64_t blk = first; blk < last && bad == 0; blk++) {
        uint64_t phys = 0;
        if (!resolve_phys(fs, ino, blk, phys)) {
            CHECK(false, "%s: 逻辑块 %llu 无 phys 映射（不该是洞）", tag,
                  (unsigned long long)blk);
            return;
        }
        if (!fs.block_test(phys)) {
            CHECK(false, "%s: phys %llu 位图未占用", tag, (unsigned long long)phys);
            return;
        }
        fs.read_block(phys, buf);
        const uint64_t lo = blk * FS_BLOCK < off ? off - blk * FS_BLOCK : 0;
        const uint64_t hi = blk * FS_BLOCK + FS_BLOCK < off + len
                                ? FS_BLOCK
                                : (off + len) - blk * FS_BLOCK;
        for (uint64_t i = lo; i < hi; i++)
            if (buf[i] != pat_byte(seed, blk * FS_BLOCK + i)) { bad++; break; }
    }
    CHECK(bad == 0, "%s: 逐块读回 == 模式 (%lluB @%llu, %s, %llu 块)",
          tag, (unsigned long long)len, (unsigned long long)off, shape_name(sh),
          (unsigned long long)(last - first));
}

/* ── pwrite 正门直调（inode_idx 版，测试对象本体）── */
static result_code_t pw(RavenFS_juvenile &fs, uint64_t inum,
                        uint64_t off, uint64_t len, uint64_t seed) {
    std::vector<uint8_t> buf(len);
    fill_buf(buf.data(), len, seed, off);
    return fs.direct_file_pwrite(inum, off, len, buf.data());
}

/* ═══════════════════════════════════════════════════════════════
 * A 组：inline → direct 保形 + direct 稳态（覆盖零新段 / 尾贴增长）
 * 前置：fresh mkfs 后低址连续自由（外部保证）。
 * ═══════════════════════════════════════════════════════════════ */
static void group_A(RavenFS_juvenile &fs) {
    printf("── A: inline→direct 保形 + direct 稳态 ──\n");
    const uint64_t a = make_reg(fs, "a");
    CHECK(a != PTR_INVALID, "A0: 建 REG 文件 a");
    if (a == PTR_INVALID) return;

    ino_snap_t s0 = snap(fs, a);
    CHECK(s0.shape == 0 && s0.size == 0, "A0: 初始 inline 空（size=0）");

    /* A1: 5000B 整域 hard 成功 → 保 direct，2 块 */
    uint64_t fb = free_blocks(fs);
    CHECK(pw(fs, a, 0, 5000, 0xA1) == RC_OK, "A1: pwrite 5000B RC_OK");
    ino_snap_t s1 = snap(fs, a);
    CHECK(s1.shape == 1 && s1.direct_bit == 1, "A1: 形态 direct（direct_bit=1）got %s",
          shape_name(s1.shape));
    CHECK(s1.size == 5000 && s1.data_root != PTR_INVALID_52,
          "A1: size=5000 data_root 有效");
    CHECK(fb - free_blocks(fs) == 2, "A1: 位图账本 2 块（域 [0,2)）");
    verify_range(fs, a, 0, 5000, 0xA1, "A1 读回");
    const uint64_t base_a = s1.data_root;

    /* A2: 覆盖写 [100,3100) 零新段，base 不动，size 不变 */
    fb = free_blocks(fs);
    CHECK(pw(fs, a, 100, 3000, 0xA2) == RC_OK, "A2: 覆盖写 3000B RC_OK");
    ino_snap_t s2 = snap(fs, a);
    CHECK(s2.shape == 1 && s2.data_root == base_a && s2.size == 5000,
          "A2: 仍 direct、base 不动、size 不变");
    CHECK(fb - free_blocks(fs) == 0, "A2: 覆盖写零新段（账本 0）");
    verify_range(fs, a, 0, 100, 0xA1, "A2 读回[0,100) 旧 seed");
    verify_range(fs, a, 100, 3000, 0xA2, "A2 读回[100,3100) 新 seed");
    verify_range(fs, a, 3100, 1900, 0xA1, "A2 读回[3100,5000) 旧 seed");

    /* A3: 增长跨块 [5000,10100)：尾贴占领成功 → 保 direct，base 不动，+1 块 */
    fb = free_blocks(fs);
    CHECK(pw(fs, a, 5000, 5100, 0xA3) == RC_OK, "A3: 增长 5100B RC_OK");
    ino_snap_t s3 = snap(fs, a);
    CHECK(s3.shape == 1 && s3.data_root == base_a && s3.size == 10100,
          "A3: 尾贴保 direct、base 不动、size=10100");
    CHECK(fb - free_blocks(fs) == 1, "A3: 账本 +1 块（域扩到 3 块）");
    verify_range(fs, a, 5000, 5100, 0xA3, "A3 读回新域");
    verify_range(fs, a, 100, 3000, 0xA2, "A3 读回旧覆盖段");
}

/* ═══════════════════════════════════════════════════════════════
 * B 组：inline → extent（hard 整败 → soft 裂段 2 条 → 上树留树）
 * 前置：main 已 claim-all 榨干全域（free==0），本组自开双窗。
 * 收尾：清空剩余自由 → C 组窗从干净盘面开始（无 B 残留 run 污染低址序）。
 * ═══════════════════════════════════════════════════════════════ */
static void group_B(RavenFS_juvenile &fs, uint64_t C0) {
    printf("── B: inline→extent 上树（碎片裂段降级）──\n");
    const uint64_t b = make_reg(fs, "b");
    CHECK(b != PTR_INVALID, "B0: 建 REG 文件 b");
    if (b == PTR_INVALID) return;

    CHECK(pw(fs, b, 0, 100, 0xB1) == RC_OK, "B1: inline 写 100B RC_OK");
    ino_snap_t s1 = snap(fs, b);
    CHECK(s1.shape == 0 && s1.size == 100, "B1: 仍 inline（size=100）");
    verify_range(fs, b, 0, 100, 0xB1, "B1 inline 读回");

    /* 双窗雕塑：自由 = [C0, C0+120) ∪ [C0+400, C0+520)（main claim 后 free==0） */
    fs.block_free(C0, 120);
    fs.block_free(C0 + 400, 120);
    CHECK(free_blocks(fs) == 240, "B2: 双窗雕塑 free==240");

    /* B3: 201 块整域（823296B）→ hard(201) 败 → soft 裂 120+81 两段 → 上树 */
    const uint64_t W = 201 * FS_BLOCK;
    uint64_t fb = free_blocks(fs);
    CHECK(pw(fs, b, 0, W, 0xB3) == RC_OK, "B3: pwrite 823296B RC_OK");
    ino_snap_t s3 = snap(fs, b);
    CHECK(s3.shape == 2 && s3.direct_bit == 0, "B3: 形态 extent（direct_bit=0）got %s",
          shape_name(s3.shape));
    CHECK(s3.size == W && s3.data_root != PTR_INVALID_52,
          "B3: size=823296 data_root=树根");
    /* 树审计：单层根叶 2 记录 */
    const uint64_t recs = tree_records(fs, s3.data_root);
    CHECK(recs == 2, "B3: 树记录 == 2（裂段两段）got %llu",
          (unsigned long long)recs);
    /* 映射审计：精确两条 [0,120)@C0 [120,201)@C0+400 */
    std::vector<extent_t> recv;
    CHECK(range_sample(fs, s3.data_root, 0, W / FS_BLOCK, recv) && recv.size() == 2,
          "B3: range_sample 2 条记录");
    if (recv.size() == 2) {
        const extent_t &e0 = recv[0], &e1 = recv[1];
        CHECK(e0.fblkbase == 0 && e0.length == 120 && e0.block_base.ptr == C0,
              "B3: 段1 [0,120) phys=C0");
        CHECK(e1.fblkbase == 120 && e1.length == 81 && e1.block_base.ptr == C0 + 400,
              "B3: 段2 [120,201) phys=C0+400");
    }
    /* 账本：数据 201 + 空根叶 2 */
    CHECK(fb - free_blocks(fs) == 203, "B3: 账本 203（数据 201 + 树 2）got %llu",
          (unsigned long long)(fb - free_blocks(fs)));
    /* 全文件逐块读回（走 extent 映射） */
    verify_range(fs, b, 0, W, 0xB3, "B3 读回 201 块整域");

    /* B4 清场：占掉全部剩余自由（B 窗余量）→ C 组窗从干净盘面开始 */
    while (fs.block_alloc_hard(1, 0) != PTR_INVALID) {}
    CHECK(free_blocks(fs) == 0, "B4: 清场 free==0");
}

/* ═══════════════════════════════════════════════════════════════
 * C 组：extent → extent（map-only 四段管线跑全：物化态无、逐段 map、
 *       覆盖零新段不碰树、出口留树）
 * 舞台：main claim 后本组自开窗。构造窗（物化 1 + 写域 300 + 树 3）
 *       与场景窗（C1 锚 1 + C2 纯插 1 + C4 尾 10）分离——树节点 alloc
 *       只在构造期 phase2 发生，场景期零新树（不满叶零预算）→ 场景窗
 *       不会被 allocator 抢。构造期让树吃独立窗 D（最小自由 run 在
 *       写域之后），数据尾 C0+830 保持 claim 占用 → C4 窗后开即得。
 * ═══════════════════════════════════════════════════════════════ */
static void group_C(RavenFS_juvenile &fs, uint64_t C0) {
    printf("── C: extent→extent（map-only 全管线）──\n");
    const uint64_t c = make_reg(fs, "c");
    CHECK(c != PTR_INVALID, "C0: 建 REG 文件 c");
    if (c == PTR_INVALID) return;

    CHECK(pw(fs, c, 0, 100, 0xC1) == RC_OK, "C1: inline 写 100B RC_OK");
    ino_snap_t s1 = snap(fs, c);
    CHECK(s1.shape == 0 && s1.size == 100, "C1: 仍 inline（size=100）");

    /* ── 构造窗（stage 1）：物化 [0,1) 吃 C0+520；写域 300 吃 [C0+530,830)；
     * 树 2 块吃 D=[C0+900,903)（低址序：A/B 尽后首个自由 run）── */
    fs.block_free(C0 + 520, 1);
    fs.block_free(C0 + 530, 300);
    fs.block_free(C0 + 900, 3);

    /* C2: 写 [200,500)（819200B 起 1228800B）→ 迁出域 [0,500) hard 必败
     * （无 ≥500 连续 run）→ 降级：物化 [0,1) + 写域 [200,500)，洞 [1,200) 留空
     * → 上树。期望记录 = [0,1)@C0+520 + [200,500)@C0+530。 */
    const uint64_t W = 300 * FS_BLOCK;
    const uint64_t OFF = 200 * FS_BLOCK;
    uint64_t fb = free_blocks(fs);
    CHECK(pw(fs, c, OFF, W, 0xC2) == RC_OK, "C2: pwrite 300 块迁出 RC_OK");
    ino_snap_t s2 = snap(fs, c);
    CHECK(s2.shape == 2 && s2.direct_bit == 0, "C2: 形态 extent got %s",
          shape_name(s2.shape));
    CHECK(s2.size == OFF + W, "C2: size=%llu", (unsigned long long)(OFF + W));
    const uint64_t recs2 = tree_records(fs, s2.data_root);
    CHECK(recs2 == 2, "C2: 树记录 2（[0,1) + [200,500)）got %llu",
          (unsigned long long)recs2);
    std::vector<extent_t> rv;
    CHECK(range_sample(fs, s2.data_root, 0, 500, rv) && rv.size() == 2,
          "C2: range_sample 2 条");
    if (rv.size() == 2) {
        CHECK(rv[0].fblkbase == 0 && rv[0].length == 1 && rv[0].block_base.ptr == C0 + 520,
              "C2: 记录1 [0,1)@C0+520");
        CHECK(rv[1].fblkbase == 200 && rv[1].length == 300 &&
                  rv[1].block_base.ptr == C0 + 530,
              "C2: 记录2 [200,500)@C0+530");
    }
    CHECK(fb - free_blocks(fs) == 303, "C2: 账本 303（物化 1 + 数据 300 + 树 2）got %llu",
          (unsigned long long)(fb - free_blocks(fs)));
    verify_range(fs, c, 0, 100, 0xC1, "C2 读回物化旧内容[0,100)");
    verify_range(fs, c, OFF, W, 0xC2, "C2 读回写域 300 块");

    /* ── 场景窗（stage 2）：树已在位，零新树 → 窗不会被抢 ── */

    /* C3: 洞写 [1,2) 锚 = 物化记录 phys 尾（C0+521）→ 紧贴占领 → map 左并
     * [0,1)→[0,2)。记录数不变（左并 count 不变）。 */
    fs.block_free(C0 + 521, 1);
    fb = free_blocks(fs);
    CHECK(pw(fs, c, 1 * FS_BLOCK, FS_BLOCK, 0xC3) == RC_OK, "C3: 洞写 [1,2) RC_OK");
    ino_snap_t s3 = snap(fs, c);
    CHECK(tree_records(fs, s3.data_root) == 2, "C3: 左并记录数不变（2）");
    CHECK(range_sample(fs, s3.data_root, 0, 2, rv) && rv.size() == 1,
          "C3: [0,2) 合成单条");
    if (rv.size() == 1)
        CHECK(rv[0].fblkbase == 0 && rv[0].length == 2 &&
                  rv[0].block_base.ptr == C0 + 520 &&
                  fs.block_test(rv[0].block_base.ptr + 1),
              "C3: [0,2)@C0+520 连续 2 块（左并）");
    CHECK(fb - free_blocks(fs) == 1, "C3: 账本 +1");
    verify_range(fs, c, 1 * FS_BLOCK, FS_BLOCK, 0xC3, "C3 读回洞写块");

    /* C4: 洞中段写 [100,101) 无左邻可锚 → soft 纯插新记录（记录 2→3） */
    fb = free_blocks(fs);
    CHECK(pw(fs, c, 100 * FS_BLOCK, FS_BLOCK, 0xC4) == RC_OK, "C4: 洞中段写 RC_OK");
    ino_snap_t s4 = snap(fs, c);
    CHECK(tree_records(fs, s4.data_root) == 3, "C4: 纯插记录 2→3");
    CHECK(fb - free_blocks(fs) == 1, "C4: 账本 +1");
    verify_range(fs, c, 100 * FS_BLOCK, FS_BLOCK, 0xC4, "C4 读回纯插块");

    /* C5: 覆盖写 [210,260) ⊂ 记录 [200,500)：零新段、记录数不变、不碰树 */
    fb = free_blocks(fs);
    CHECK(pw(fs, c, 210 * FS_BLOCK, 50 * FS_BLOCK, 0xC5) == RC_OK, "C5: 覆盖写 RC_OK");
    ino_snap_t s5 = snap(fs, c);
    CHECK(s5.size == OFF + W, "C5: size 不变");
    CHECK(tree_records(fs, s5.data_root) == 3, "C5: 覆盖零新段记录 3");
    CHECK(fb - free_blocks(fs) == 0, "C5: 账本 0");
    verify_range(fs, c, 210 * FS_BLOCK, 50 * FS_BLOCK, 0xC5, "C5 读回覆盖段");

    /* C6: 尾增长 [500,510) 锚 = 记录 [200,500) phys 尾（C0+830，stage2 开窗）→
     * 紧贴 → map 左并扩 [200,510)。记录数不变（左并 count 不变）。 */
    fs.block_free(C0 + 830, 10);
    fb = free_blocks(fs);
    CHECK(pw(fs, c, 500 * FS_BLOCK, 10 * FS_BLOCK, 0xC6) == RC_OK, "C6: 尾增长 RC_OK");
    ino_snap_t s6 = snap(fs, c);
    CHECK(s6.size == 510 * FS_BLOCK, "C6: size=510 块");
    CHECK(tree_records(fs, s6.data_root) == 3, "C6: 尾增长左并记录 3");
    CHECK(range_sample(fs, s6.data_root, 200, 310, rv) && rv.size() == 1,
          "C6: [200,510) 合成单条");
    if (rv.size() == 1)
        CHECK(rv[0].fblkbase == 200 && rv[0].length == 310 &&
                  rv[0].block_base.ptr == C0 + 530,
              "C6: [200,510)@C0+530（尾贴左并）");
    CHECK(fb - free_blocks(fs) == 10, "C6: 账本 +10");
    verify_range(fs, c, 500 * FS_BLOCK, 10 * FS_BLOCK, 0xC6, "C6 读回增长段");
    verify_range(fs, c, 0, 100, 0xC1, "C6 全链抽查[0,100)");
    verify_range(fs, c, 200 * FS_BLOCK, 10 * FS_BLOCK, 0xC2, "C6 全链抽查[200,210)");
    verify_range(fs, c, 260 * FS_BLOCK, 40 * FS_BLOCK, 0xC2, "C6 全链抽查[260,300)");
    verify_range(fs, c, 210 * FS_BLOCK, 50 * FS_BLOCK, 0xC5, "C6 全链抽查覆盖段[210,260)");
}

/* 清场：占掉全部剩余自由（场景间隔离，D 组窗从干净盘面开始） */
static void drain_free(RavenFS_juvenile &fs) {
    while (fs.block_alloc_hard(1, 0) != PTR_INVALID) {}
}

/* ═══════════════════════════════════════════════════════════════
 * D 组：direct 边界条件（路径 4/5）
 *   D1  inline→direct（低址连续，3 块 @Wd）
 *   D2  direct 覆盖跨块边界（[4000,8500) 越块 0/1/2 界，全旧 run 内）→ 零新段
 *   D3  direct 增长不跨块（[9000,9400) 仍在块 2 内）→ 零新段纯 size 推进
 *   D4  **direct→extent**：拦路虎占旧 run 尾后块（C0+1503）→ 尾占领失败 →
 *       新段 phys 断 → 物化上树：断言记录1 phys == 原 base（零搬移）
 *   D5  direct 尾贴多块增长保 direct（50 块）
 * ═══════════════════════════════════════════════════════════════ */
static void group_D(RavenFS_juvenile &fs, uint64_t C0) {
    printf("── D: direct 边界（覆盖跨块 / 增长零新段 / 尾败上树 / 尾贴多块）──\n");
    drain_free(fs);
    CHECK(free_blocks(fs) == 0, "D0: 清场 free==0");

    const uint64_t d = make_reg(fs, "d");
    CHECK(d != PTR_INVALID, "D0: 建 REG 文件 d");
    if (d == PTR_INVALID) return;

    /* 窗：Wd=3（d 数据） Wb=1（拦路虎） Wg=2（增长段） Wt=3（树，align 余量） */
    fs.block_free(C0 + 1500, 3);
    fs.block_free(C0 + 1503, 1);
    fs.block_free(C0 + 1600, 2);
    fs.block_free(C0 + 1700, 3);

    /* D1: 9000B（3 块）→ direct @C0+1500 */
    uint64_t fb = free_blocks(fs);
    CHECK(pw(fs, d, 0, 9000, 0xD1) == RC_OK, "D1: pwrite 9000B RC_OK");
    ino_snap_t s1 = snap(fs, d);
    CHECK(s1.shape == 1 && s1.direct_bit == 1 && s1.size == 9000 &&
              s1.data_root == C0 + 1500,
          "D1: direct @C0+1500 size=9000");
    CHECK(fb - free_blocks(fs) == 3, "D1: 账本 3 块");

    /* D2: 覆盖跨块边界 [4000,8500)（越块 0/1/2 界，全旧 run 内）→ 零新段 */
    fb = free_blocks(fs);
    CHECK(pw(fs, d, 4000, 4500, 0xD2) == RC_OK, "D2: 覆盖跨块 RC_OK");
    ino_snap_t s2 = snap(fs, d);
    CHECK(s2.shape == 1 && s2.data_root == C0 + 1500 && s2.size == 9000,
          "D2: 仍 direct base 不动 size 不变");
    CHECK(fb - free_blocks(fs) == 0, "D2: 覆盖零新段");

    /* D3: 增长不跨块 [9000,9400)（块 2 内）→ 零新段纯 size 推进 */
    fb = free_blocks(fs);
    CHECK(pw(fs, d, 9000, 400, 0xD3) == RC_OK, "D3: 增长不跨块 RC_OK");
    ino_snap_t s3 = snap(fs, d);
    CHECK(s3.shape == 1 && s3.data_root == C0 + 1500 && s3.size == 9400,
          "D3: 仍 direct base 不动 size=9400");
    CHECK(fb - free_blocks(fs) == 0, "D3: 增长零新段（纯 size 推进）");

    /* D4: 拦路虎占旧 run 尾 C0+1503 → 增长 2 块 → 尾占领失败 → 物化上树 */
    const uint64_t blocker = fs.block_alloc_hard(1, 0);
    CHECK(blocker == C0 + 1503, "D4: 拦路虎精确占 C0+1503（got %llu）",
          (unsigned long long)blocker);
    fb = free_blocks(fs);
    CHECK(pw(fs, d, 9400, 8192, 0xD4) == RC_OK, "D4: 增长 2 块 RC_OK");
    ino_snap_t s4 = snap(fs, d);
    CHECK(s4.shape == 2 && s4.direct_bit == 0 && s4.size == 17592,
          "D4: 尾败物化 → extent（size=17592）got %s", shape_name(s4.shape));
    const uint64_t recs4 = tree_records(fs, s4.data_root);
    CHECK(recs4 == 2, "D4: 树记录 2（旧区 + 新段）got %llu", (unsigned long long)recs4);
    std::vector<extent_t> rv;
    CHECK(range_sample(fs, s4.data_root, 0, 5, rv) && rv.size() == 2, "D4: sample 2 条");
    if (rv.size() == 2) {
        CHECK(rv[0].fblkbase == 0 && rv[0].length == 3 &&
                  rv[0].block_base.ptr == C0 + 1500,
              "D4: 记录1 [0,3)@原 base（物化零搬移）");
        CHECK(rv[1].fblkbase == 3 && rv[1].length == 2 &&
                  rv[1].block_base.ptr == C0 + 1600,
              "D4: 记录2 [3,5)@C0+1600（phys 断，永居 extent）");
    }
    CHECK(fb - free_blocks(fs) == 4, "D4: 账本 4（数据 2 + 树 2）got %llu",
          (unsigned long long)(fb - free_blocks(fs)));
    verify_range(fs, d, 0, 4000, 0xD1, "D4 读回[0,4000)");
    verify_range(fs, d, 4000, 4500, 0xD2, "D4 读回覆盖段");
    verify_range(fs, d, 8500, 500, 0xD1, "D4 读回[8500,9000)");
    verify_range(fs, d, 9000, 400, 0xD3, "D4 读回[9000,9400)");
    verify_range(fs, d, 9400, 8192, 0xD4, "D4 读回增长段（越记录 1/2 边界）");

    /* D5: 文件 e 尾贴多块增长保 direct（50 块） */
    drain_free(fs);
    const uint64_t e = make_reg(fs, "e");
    CHECK(e != PTR_INVALID, "D5: 建 REG 文件 e");
    if (e == PTR_INVALID) return;
    fs.block_free(C0 + 1800, 100);
    CHECK(pw(fs, e, 0, FS_BLOCK, 0xD5a) == RC_OK, "D5: e 写 1 块 RC_OK");
    ino_snap_t se = snap(fs, e);
    CHECK(se.shape == 1 && se.data_root == C0 + 1800, "D5: e direct @C0+1800");
    fb = free_blocks(fs);
    CHECK(pw(fs, e, FS_BLOCK, 50 * FS_BLOCK, 0xD5b) == RC_OK, "D5: 尾贴增长 50 块 RC_OK");
    ino_snap_t s5 = snap(fs, e);
    CHECK(s5.shape == 1 && s5.data_root == C0 + 1800 && s5.size == 51 * FS_BLOCK,
          "D5: 尾贴 50 块保 direct（base 不动 size=51 块）");
    CHECK(fb - free_blocks(fs) == 50, "D5: 账本 +50");
    verify_range(fs, e, 0, FS_BLOCK, 0xD5a, "D5 读回首块");
    verify_range(fs, e, FS_BLOCK, 50 * FS_BLOCK, 0xD5b, "D5 读回增长 50 块");
}

/* ── claim-all：把剩余空闲块全 claim（自由区连续 → 首块+总量即其范围）── */
static void claim_all(RavenFS_juvenile &fs, uint64_t *out_first, uint64_t *out_total) {
    *out_first = PTR_INVALID;
    *out_total = 0;
    const uint64_t sizes[] = {2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1};
    for (uint64_t s : sizes) {
        for (;;) {
            uint64_t b = fs.block_alloc_hard(s, 0);
            if (b == PTR_INVALID) break;
            if (*out_first == PTR_INVALID) *out_first = b;
            *out_total += s;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <device>\n", argv[0]);
        return 1;
    }
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 1; }
    uint64_t rc = 0;
    RavenFS_juvenile fs(mnt, &rc);
    if (rc != RC_OK || !fs.is_mounted()) {
        printf("mount failed rc=%s\n", rc_name(rc));
        return 1;
    }

    group_A(fs);
    (void)audit_clean(fs, "pwrite-before-exhaust");

    /* 全局舞台：A 组用低址连续自由跑 direct 后，claim-all 榨干全域；
     * B/C 组各自从 C0 起开窗（相对断言，互不依赖盘面历史）。 */
    uint64_t C0 = PTR_INVALID, total = 0;
    claim_all(fs, &C0, &total);
    CHECK(free_blocks(fs) == 0, "claim: 榨干 free==0");
    CHECK(C0 != PTR_INVALID && total >= 1500, "claim: 自由区充足（total=%llu）",
          (unsigned long long)total);
    if (C0 == PTR_INVALID || total < 1500) {
        printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    group_B(fs, C0);
    group_C(fs, C0);
    group_D(fs, C0);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
