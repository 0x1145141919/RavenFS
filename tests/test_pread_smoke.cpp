/* ═══════════════════════════════════════════════════════════════════
 * test_pread_smoke.cpp — direct_file_pread 发烟（规格 punch_truncate_pread_spec.md §4）
 *
 * 被测合同：
 *   R1 len==0 → RC_OK *out=0（零长读）
 *   R2 offset≥size → RC_OK *out=0（越界读不是错误）
 *   R3 EOF 短读：*out = min(len, size-offset)（正常结果）
 *   R4 只读 [0,size)：盘上 [size,块上界) 残留永不拷出
 *   R5 洞读 0；不分配不脏不扩文件
 *   R6 offset+len 溢出 → RC_ERR_STATE
 *   受理 = REG only：DIR → RC_ERR_IS_DIR；SOFT_SYM → RC_ERR_STATE
 *
 * 形态：H=inline / I=direct / J=extent（双段 phys 断）/ K=extent 带大洞 / L=防御受理
 * 验证：pread 读回 + 与写入 pattern（pat_byte(seed, abs_off)）比对；洞段比对 0。
 *   自洽验证模式（写入 seed 已知，读回比对期望）——pread 本体即被测对象。
 *
 * 用法：ravenfs_juv_pread_smoke <device>
 * 跑法：mkfs.ravenfs <device> -f --inode-count=262144 && \
 *       ravenfs_juv_pread_smoke <device>
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#include "../include/ravenfs/bitmap_slices.hpp"

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
    case RC_ERR_IS_DIR: return "RC_ERR_IS_DIR";
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

static uint64_t free_blocks(RavenFS_juvenile &fs) { return fs.bitmap_mgr->block_free_total(); }

struct ino_snap_t { uint64_t size, data_root, direct_bit; int shape; };
static ino_snap_t snap(RavenFS_juvenile &fs, uint64_t inum) {
    InodeSession s(fs.inode_cache_mgr, inum);
    ino_snap_t t; memset(&t, 0, sizeof(t));
    if (!s.inode) return t;
    raven_Inode_t *ino = s.inode;
    t.size = ino->size; t.data_root = ino->data_root; t.direct_bit = ino->is_data_root_direct_bit;
    t.shape = (int)fs.ftype(ino);
    return t;
}
static const char *shape_name(int sh) {
    return sh == 0 ? "inline" : sh == 1 ? "direct" : sh == 2 ? "extent" : "???";
}

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
    if (glb == RavenFS_juvenile::EXTENT_LEAF_GLB_NONE) { ++it; if (it.iterator_erro_flag) return true; }
    else if (!hit) ++it;
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

/* pwrite 写（模式 seed） */
static result_code_t pw(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len, uint64_t seed) {
    std::vector<uint8_t> buf(len); fill_buf(buf.data(), len, seed, off);
    return fs.direct_file_pwrite(inum, off, len, buf.data());
}
/* pread 读：out 刚好容纳 len；返回 rc, got 出参 */
static result_code_t pr(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len,
                        std::vector<uint8_t> &out, uint64_t &got) {
    out.assign(len, 0);
    got = 0;
    return fs.direct_file_pread(inum, off, len, out.data(), &got);
}

/* 校验 buf[0, len)（绝对偏移 off 起）与 pattern 一致；buf 可为整段或偏移段 */
static bool chk_seed(const uint8_t *buf, uint64_t len, uint64_t off, uint64_t seed) {
    for (uint64_t i = 0; i < len; i++)
        if (buf[i] != pat_byte(seed, off + i)) return false;
    return true;
}
/* 校验 buf[0, len) 为 0 */
static bool chk_zero(const uint8_t *buf, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) if (buf[i] != 0) return false;
    return true;
}

static void drain_free(RavenFS_juvenile &fs) { while (fs.block_alloc_hard(1, 0) != PTR_INVALID) {} }
static void claim_all(RavenFS_juvenile &fs, uint64_t *out_first, uint64_t *out_total) {
    *out_first = PTR_INVALID; *out_total = 0;
    const uint64_t sizes[] = {2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1};
    for (uint64_t s : sizes) for (;;) {
        uint64_t b = fs.block_alloc_hard(s, 0);
        if (b == PTR_INVALID) break;
        if (*out_first == PTR_INVALID) *out_first = b;
        *out_total += s;
    }
}

/* ════════════════════ H 组：inline 读 ════════════════════ */
static void group_H(RavenFS_juvenile &fs) {
    printf("── H: inline 读（短读/越界/零长/溢出）──\n");
    const uint64_t a = make_reg(fs, "ha");
    CHECK(a != PTR_INVALID, "H0: 建 REG 文件 ha");
    if (a == PTR_INVALID) return;
    CHECK(pw(fs, a, 0, 100, 0xA1) == RC_OK, "H0: 写 100B RC_OK");
    CHECK(snap(fs, a).shape == 0, "H0: 形态 inline");

    std::vector<uint8_t> out; uint64_t got = 0;
    result_code_t r = pr(fs, a, 0, 100, out, got);
    CHECK(r == RC_OK && got == 100, "H1: 全读 100B out=%llu rc=%s",
          (unsigned long long)got, rc_name(r));
    CHECK(chk_seed(out.data(), got, 0, 0xA1), "H1: 内容 == pattern");

    r = pr(fs, a, 50, 100, out, got);
    CHECK(r == RC_OK && got == 50, "H2: EOF 短读 out=%llu（size=100 偏移 50）",
          (unsigned long long)got);
    CHECK(chk_seed(out.data(), got, 50, 0xA1), "H2: 短读内容");

    r = pr(fs, a, 100, 50, out, got);
    CHECK(r == RC_OK && got == 0, "H3: offset==size → 0 字节（非错误）");
    r = pr(fs, a, 200, 10, out, got);
    CHECK(r == RC_OK && got == 0, "H4: offset>size → 0 字节");
    r = pr(fs, a, 0, 0, out, got);
    CHECK(r == RC_OK && got == 0, "H5: 零长读 0 字节");
    r = pr(fs, a, 90, 30, out, got);
    CHECK(r == RC_OK && got == 10, "H6: 跨 EOF clamp out=%llu（10B）", (unsigned long long)got);
    r = pr(fs, a, ~0ULL - 20, 40, out, got);
    CHECK(r == RC_ERR_STATE, "H7: offset+len 溢出 → RC_ERR_STATE");

    /* inline 覆写后读回 */
    CHECK(pw(fs, a, 40, 60, 0xA2) == RC_OK, "H8: 覆写 [40,100) 60B RC_OK");
    r = pr(fs, a, 0, 100, out, got);
    CHECK(r == RC_OK && got == 100, "H8: 全读 100B");
    CHECK(chk_seed(out.data(), 40, 0, 0xA1) && chk_seed(out.data() + 40, 60, 40, 0xA2),
          "H8: [0,40)旧 pattern + [40,100)新 pattern");
}

/* ════════════════════ I 组：direct 读 ════════════════════ */
static void group_I(RavenFS_juvenile &fs) {
    printf("── I: direct 读（块内偏移/跨块/clamp/越界）──\n");
    const uint64_t b = make_reg(fs, "ia");
    CHECK(b != PTR_INVALID, "I0: 建 REG 文件 ia");
    if (b == PTR_INVALID) return;
    CHECK(pw(fs, b, 0, 5000, 0xB1) == RC_OK, "I0: 写 5000B RC_OK");
    ino_snap_t s = snap(fs, b);
    CHECK(s.shape == 1 && s.size == 5000, "I0: 形态 direct size=5000");

    std::vector<uint8_t> out; uint64_t got = 0;
    /* 全读 */
    result_code_t r = pr(fs, b, 0, 5000, out, got);
    CHECK(r == RC_OK && got == 5000 && chk_seed(out.data(), got, 0, 0xB1), "I1: 全读 5000B == pattern");
    /* 块内偏移 */
    r = pr(fs, b, 100, 3000, out, got);
    CHECK(r == RC_OK && got == 3000 && chk_seed(out.data(), got, 100, 0xB1), "I2: [100,3100) 块内偏移");
    /* 跨块边界 [4090,4200) */
    r = pr(fs, b, 4090, 110, out, got);
    CHECK(r == RC_OK && got == 110 && chk_seed(out.data(), got, 4090, 0xB1), "I3: 跨块 [4090,4200)");
    /* 跨 3 块 [4000,8500) */
    r = pr(fs, b, 4000, 4500, out, got);
    CHECK(r == RC_OK && got == 1000 && chk_seed(out.data(), got, 4000, 0xB1), "I4: [4000,8500) 跨块 clamp->1000 (EOF)");
    /* EOF 短读 */
    r = pr(fs, b, 4990, 100, out, got);
    CHECK(r == RC_OK && got == 10 && chk_seed(out.data(), got, 4990, 0xB1), "I5: EOF 短读 10B");
    /* 越界 */
    r = pr(fs, b, 5000, 10, out, got);
    CHECK(r == RC_OK && got == 0, "I6: offset==size → 0");
    r = pr(fs, b, 6000, 10, out, got);
    CHECK(r == RC_OK && got == 0, "I7: offset>size → 0");

    /* 覆盖写后读（读路径校验覆盖） */
    CHECK(pw(fs, b, 100, 3000, 0xB2) == RC_OK, "I8: 覆盖写 [100,3100)");
    r = pr(fs, b, 0, 5000, out, got);
    CHECK(r == RC_OK && got == 5000, "I8: 全读 5000B");
    CHECK(chk_seed(out.data(), 100, 0, 0xB1), "I8: [0,100) 旧 pattern");
    CHECK(chk_seed(out.data() + 100, 3000, 100, 0xB2), "I8: [100,3100) 新 pattern");
    CHECK(chk_seed(out.data() + 3100, 1900, 3100, 0xB1), "I8: [3100,5000) 旧 pattern");
}

/* ════════════════════ J 组：extent 读（双段 phys 断 + 带洞）═══ */
static void group_J(RavenFS_juvenile &fs, uint64_t C0) {
    printf("── J: extent 读（双段边界 + 大洞 + 洞读 0）──\n");

    /* J 文件 j：extent 2 条逻辑连续、phys 断（[0,120)@C0 [120,201)@C0+400）*/
    const uint64_t j = make_reg(fs, "ja");
    CHECK(j != PTR_INVALID, "J0: 建 REG 文件 ja");
    if (j == PTR_INVALID) return;
    CHECK(pw(fs, j, 0, 100, 0xC1) == RC_OK, "J0: 写 100B");
    fs.block_free(C0, 120); fs.block_free(C0 + 400, 120);
    CHECK(free_blocks(fs) == 240, "J0: 双窗雕塑 free==240");
    const uint64_t W = 201 * FS_BLOCK;
    CHECK(pw(fs, j, 0, W, 0xC2) == RC_OK, "J1: 写 201 块（hard 败 → 裂 120+81 两段）RC_OK");
    ino_snap_t sj = snap(fs, j);
    CHECK(sj.shape == 2 && sj.size == W, "J1: 形态 extent size=%llu", (unsigned long long)sj.size);
    CHECK(tree_records(fs, sj.data_root) == 2, "J1: 树记录 2");
    std::vector<extent_t> rv;
    const uint64_t dCnt = W / FS_BLOCK;
    if (range_sample(fs, sj.data_root, 0, dCnt, rv) && rv.size() == 2) {
        CHECK(rv[0].fblkbase == 0 && rv[0].length == 120 && rv[0].block_base.ptr == C0, "J1: 段1");
        CHECK(rv[1].fblkbase == 120 && rv[1].length == 81 && rv[1].block_base.ptr == C0 + 400, "J1: 段2");
    } else CHECK(false, "J1: range_sample 2 条");

    std::vector<uint8_t> out; uint64_t got = 0;
    /* 全文件 201 块读（双段边界 [120,130) 处） */
    result_code_t r = pr(fs, j, 0, W, out, got);
    CHECK(r == RC_OK && got == W, "J2: 全读 201 块 out=%llu rc=%s", (unsigned long long)got, rc_name(r));
    CHECK(chk_seed(out.data(), got, 0, 0xC2), "J2: 全文件 == pattern");
    /* 跨段边界 [119*4096, 122*4096) */
    r = pr(fs, j, 119 * FS_BLOCK, 3 * FS_BLOCK, out, got);
    CHECK(r == RC_OK && got == 3 * FS_BLOCK && chk_seed(out.data(), got, 119 * FS_BLOCK, 0xC2),
          "J3: 跨段边界读 [119,122) 块");

    /* 文件 k：extent 带大洞 [1,200)（式同 pwrite C 组 C2）
     * 先 drain 榨干 J1 残留（J1 的树根/剩余 free 落在 C0+481..+520，区段
     * 与 J5 窗重叠风险）→ 从干净 free==0 重新开窗。 */
    drain_free(fs);
    CHECK(free_blocks(fs) == 0, "J5: drain 残留 free==0");
    const uint64_t k = make_reg(fs, "ka");
    CHECK(k != PTR_INVALID, "J5: 建 REG 文件 ka");
    if (k == PTR_INVALID) return;
    CHECK(pw(fs, k, 0, 100, 0xD1) == RC_OK, "J5: 写 100B");
    fs.block_free(C0 + 520, 1); fs.block_free(C0 + 530, 300); fs.block_free(C0 + 900, 3);
    const uint64_t KOFF = 200 * FS_BLOCK, KW = 300 * FS_BLOCK;
    CHECK(pw(fs, k, KOFF, KW, 0xD2) == RC_OK, "J5: 写 [200,500) 块（洞 [1,200)）RC_OK");
    ino_snap_t sk = snap(fs, k);
    CHECK(sk.shape == 2 && sk.size == KOFF + KW, "J5: 形态 extent size=%llu",
          (unsigned long long)sk.size);
    CHECK(range_sample(fs, sk.data_root, 0, 500, rv) && rv.size() == 2, "J5: 树 2 条");

    /* 洞区 [1,200) 全 0（读 0 语义） */
    r = pr(fs, k, FS_BLOCK, (200 - 1) * FS_BLOCK, out, got);
    CHECK(r == RC_OK && got == (200 - 1) * FS_BLOCK, "J5: 读洞 [1,200) out=%llu",
          (unsigned long long)got);
    CHECK(chk_zero(out.data(), got), "J5: 洞区全 0");
    /* 记录 [0,1)：首 100B 旧 pattern（迁移内容），其余到块界 0 */
    r = pr(fs, k, 0, FS_BLOCK, out, got);
    CHECK(r == RC_OK && got == FS_BLOCK, "J5: 读首块 [0,1)");
    CHECK(chk_seed(out.data(), 100, 0, 0xD1), "J5: 首块前 100B == 旧 pattern（迁移）");
    CHECK(chk_zero(out.data() + 100, FS_BLOCK - 100), "J5: 首块 [100,4096) == 0");
    /* 记录 [200,500) 段 */ 
    r = pr(fs, k, KOFF, KW, out, got);
    CHECK(r == RC_OK && got == KW && chk_seed(out.data(), got, KOFF, 0xD2), "J6: 写域段 == pattern");
    /* 跨界：洞右缘 [199*4096, 201*4096) 洞尾 0 + 记录头 pattern */
    r = pr(fs, k, 199 * FS_BLOCK, 2 * FS_BLOCK, out, got);
    CHECK(r == RC_OK && got == 2 * FS_BLOCK, "J7: 洞右缘跨界");
    CHECK(chk_zero(out.data(), FS_BLOCK), "J7: [199,200) 洞尾 0");
    CHECK(chk_seed(out.data() + FS_BLOCK, FS_BLOCK, KOFF, 0xD2), "J7: [200,201) 记录头 pattern");

    /* EOF 短读 + 越界（extent 态） */
    r = pr(fs, k, KOFF + KW - 8, 100, out, got);
    CHECK(r == RC_OK && got == 8 && chk_seed(out.data(), got, KOFF + KW - 8, 0xD2), "J8: extent EOF 短读 8B");
    r = pr(fs, k, KOFF + KW, 10, out, got);
    CHECK(r == RC_OK && got == 0, "J8: extent offset==size → 0");
}

/* ════════════════════ L 组：受理 / 防御 ════════════════════ */
static void group_L(RavenFS_juvenile &fs) {
    printf("── L: 受理（dir→IS_DIR / symlink→STATE）──\n");
    std::vector<uint8_t> out; uint64_t got = 0;

    /* root inode = 0 = DIR → RC_ERR_IS_DIR */
    result_code_t r = pr(fs, 0, 0, 1, out, got);
    CHECK(r == RC_ERR_IS_DIR, "L1: root dir → RC_ERR_IS_DIR got %s", rc_name(r));

    /* SOFT_SYM → RC_ERR_STATE */
    if (fs.root_dir_insert_dentry("ls", INODE_TYPE_SOFT_SYM) == RC_OK) {
        uint64_t inum = 0;
        if (fs.root_dir_lookup("ls", &inum) == RC_OK) {
            r = pr(fs, inum, 0, 1, out, got);
            CHECK(r == RC_ERR_STATE, "L2: SOFT_SYM → RC_ERR_STATE got %s", rc_name(r));
        } else CHECK(false, "L2: symlink lookup 失败");
    } else CHECK(false, "L2: 建 symlink 失败");
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 1; }
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 1; }
    uint64_t rc = 0;
    RavenFS_juvenile fs(mnt, &rc);
    if (rc != RC_OK || !fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    group_H(fs);
    group_I(fs);

    uint64_t C0 = PTR_INVALID, total = 0;
    (void)audit_clean(fs, "pread-before-exhaust");
    claim_all(fs, &C0, &total);
    CHECK(free_blocks(fs) == 0, "claim: 榨干 free==0");
    CHECK(C0 != PTR_INVALID && total >= 1500, "claim: 自由区充足 total=%llu",
          (unsigned long long)total);
    if (C0 == PTR_INVALID || total < 1500) {
        printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    group_J(fs, C0);
    group_L(fs);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
