/* ═══════════════════════════════════════════════════════════════════
 * test_punch_truncate_smoke.cpp — direct_file_punch_hole / direct_file_trunacate 发烟
 * （规格 punch_truncate_pread_spec.md §5/§6 + §10 测试规划）
 *
 * 被测合同（punch）：
 *   P3 offset≥size → RC_OK no-op；P2 len==0 → RC_OK；溢出 → RC_ERR_STATE
 *   P4 clamp_end=min(off+len, round_up(size))；P5/P6 整块域释放 + 边残块保留清零
 *   §5.2 形态分流：inline 就地 memset；direct 整块域空保 direct / 非空物化 extent；
 *     extent 逐记录 loose（部分生效可接受）
 *   KEEP_SIZE：洞后 size 恒不变；洞读 0、洞外数据保留
 *
 * 被测合同（truncate）：
 *   T7 等值 no-op；增大（§6.5 P4）：inline≤160 补零 / inline>160+direct 空写骨架 /
 *     extent 纯 size 推进；缩小（§6.3）：≤160 强制回迁 inline、direct 缩 run、
 *     extent 尾 loose + 出口坍缩判定（单条覆盖 [0,newceil) → direct）
 *
 * 验证器 = 可信内部原语组合（ftype/InodeSession/range_sample/read_block/位图账本），
 * 与 pwrite/pread 同策略（不做接口互验）；内容用 (seed, 绝对偏移) 派生模式比对 +
 * 洞区比对 0，preaad 就位后升级互验（P7 未拍，草案保持）。
 *
 * 用法：ravenfs_juv_punch_truncate_smoke <device>
 * 跑法：mkfs.ravenfs <device> -f --inode-count=262144 && \
 *       ravenfs_juv_punch_truncate_smoke <device>
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
#include <utility>

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
    case RC_ERR_OVERLAP: return "RC_ERR_OVERLAP"; case RC_ERR_IS_DIR: return "RC_ERR_IS_DIR";
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
static result_code_t pw(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len, uint64_t seed) {
    std::vector<uint8_t> buf(len); fill_buf(buf.data(), len, seed, off);
    return fs.direct_file_pwrite(inum, off, len, buf.data());
}
static result_code_t pr(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len,
                        std::vector<uint8_t> &out, uint64_t &got) {
    out.assign(len, 0); got = 0;
    return fs.direct_file_pread(inum, off, len, out.data(), &got);
}
static bool chk_seed(const uint8_t *buf, uint64_t len, uint64_t off, uint64_t seed) {
    for (uint64_t i = 0; i < len; i++) if (buf[i] != pat_byte(seed, off + i)) return false;
    return true;
}
static bool chk_zero(const uint8_t *buf, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) if (buf[i] != 0) return false;
    return true;
}
/* 全文件读回 + 校验 [a,b) 为零、其余为 pattern（字节级）；返回 rc */
static result_code_t verify_range(RavenFS_juvenile &fs, uint64_t inum, uint64_t size,
                                  uint64_t a, uint64_t b, uint64_t seed,
                                  bool &ok) {
    std::vector<uint8_t> out; uint64_t got = 0;
    result_code_t rc = pr(fs, inum, 0, size, out, got);
    if (rc != RC_OK || got != size) { ok = false; return rc; }
    /* [0,a) pattern */
    ok = chk_seed(out.data(), a, 0, seed);
    if (!ok) return RC_OK;
    /* [a,b) zero */
    ok = chk_zero(out.data() + a, b - a);
    if (!ok) return RC_OK;
    /* [b,size) pattern */
    ok = chk_seed(out.data() + b, size - b, b, seed);
    return RC_OK;
}

/* ════════════════════ P 组：punch_hole ════════════════════ */
static void group_P(RavenFS_juvenile &fs) {
    printf("── P: punch_hole（三形态 × 段化 × 大小不变）──\n");

    /* P0: inline punch */
    {
        const uint64_t a = make_reg(fs, "pa");
        CHECK(a != PTR_INVALID, "P0: 建 REG");
        if (a == PTR_INVALID) return;
        CHECK(pw(fs, a, 0, 100, 0x21) == RC_OK, "P0: 写 100B");
        CHECK(snap(fs, a).shape == 0, "P0: 形态 inline");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_punch_hole(a, 20, 60) == RC_OK, "P0: punch [20,80)");
        CHECK(snap(fs, a).size == 100, "P0: size 不变=100");
        CHECK(free_blocks(fs) == fb, "P0: inline 无块释放");
        bool ok = false;
        verify_range(fs, a, 100, 20, 80, 0x21, ok);
        CHECK(ok, "P0: [0,20)pattern [20,80)0 [80,100)pattern");
    }

    /* P1: direct punch 单块内（无整块域 → 保 direct） */
    {
        const uint64_t b = make_reg(fs, "pa1");
        CHECK(b != PTR_INVALID, "P1: 建 REG");
        if (b == PTR_INVALID) return;
        CHECK(pw(fs, b, 0, 5000, 0x31) == RC_OK, "P1: 写 5000B");
        ino_snap_t sb = snap(fs, b);
        CHECK(sb.shape == 1 && sb.size == 5000, "P1: 形态 direct 5000");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_punch_hole(b, 100, 200) == RC_OK, "P1: punch [100,300) 单块内");
        ino_snap_t sa = snap(fs, b);
        CHECK(sa.shape == 1 && sa.size == 5000, "P1: 保 direct size 不变");
        CHECK(free_blocks(fs) == fb, "P1: 无整块释放");
        bool ok = false;
        verify_range(fs, b, 5000, 100, 300, 0x31, ok);
        CHECK(ok, "P1: [0,100)pattern [100,300)0 [300,5000)pattern");
    }

    /* P2: direct punch 整块域 → 物化 extent（跨块释放） */
    {
        const uint64_t c = make_reg(fs, "pa2");
        CHECK(c != PTR_INVALID, "P2: 建 REG");
        if (c == PTR_INVALID) return;
        CHECK(pw(fs, c, 0, 5 * FS_BLOCK, 0x41) == RC_OK, "P2: 写 5 块");   /* 20000B, direct */
        ino_snap_t sc = snap(fs, c);
        CHECK(sc.shape == 1 && sc.size == 5 * FS_BLOCK, "P2: 形态 direct 5 块");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_punch_hole(c, FS_BLOCK, 2 * FS_BLOCK) == RC_OK, "P2: punch [1,3) 块");
        ino_snap_t sa = snap(fs, c);
        CHECK(sa.shape == 2, "P2: 形态物化 extent");
        CHECK(sa.size == 5 * FS_BLOCK, "P2: size 不变=5 块");
        CHECK(free_blocks(fs) == fb, "P2: 净 0（释放 2 数据块 − 树根 2 块）");
        std::vector<extent_t> rv;
        if (range_sample(fs, sa.data_root, 0, 5, rv) && rv.size() == 2) {
            CHECK(rv[0].fblkbase == 0 && rv[0].length == 1, "P2: 段1 [0,1)");
            CHECK(rv[1].fblkbase == 3 && rv[1].length == 2, "P2: 段2 [3,5)");
        } else CHECK(false, "P2: 树 2 条");
        bool ok = false;
        verify_range(fs, c, 5 * FS_BLOCK, FS_BLOCK, 3 * FS_BLOCK, 0x41, ok);
        CHECK(ok, "P2: [0,1)pattern [1,3)0 [3,5)pattern");
    }

    /* P3: extent punch （逐记录 loose）—— 先用 punch 造 extent（pw 会并 direct） */
    {
        const uint64_t d = make_reg(fs, "pa3");
        CHECK(d != PTR_INVALID, "P3: 建 REG");
        if (d == PTR_INVALID) return;
        /* 写 5 块 → direct；打洞 [1,3) → extent [0,1)+[3,5) */
        CHECK(pw(fs, d, 0, 5 * FS_BLOCK, 0x51) == RC_OK, "P3: 写 5 块 direct");
        CHECK(fs.direct_file_punch_hole(d, 1 * FS_BLOCK, 2 * FS_BLOCK) == RC_OK,
              "P3: punch [1,3) → extent");
        ino_snap_t sd = snap(fs, d);
        CHECK(sd.shape == 2 && sd.size == 5 * FS_BLOCK, "P3: 形态 extent 5 块");
        const uint64_t fb = free_blocks(fs);
        /* punch [1,4)：洞越 [1,3) 洞 + [3,4) 整块释放 → [3,5) 缩为 [4,5) */
        CHECK(fs.direct_file_punch_hole(d, FS_BLOCK, 3 * FS_BLOCK) == RC_OK, "P3: punch [1,4)");
        ino_snap_t sa = snap(fs, d);
        CHECK(sa.shape == 2 && sa.size == 5 * FS_BLOCK, "P3: size 不变 extent");
        CHECK(free_blocks(fs) == fb + 1, "P3: 释放 [3,4) 1 块");
        std::vector<extent_t> rv;
        CHECK(range_sample(fs, sa.data_root, 0, 5, rv) && rv.size() == 2, "P3: 树 2 条");
        if (rv.size() == 2) {
            CHECK(rv[0].fblkbase == 0 && rv[0].length == 1, "P3: 段1 [0,1)");
            CHECK(rv[1].fblkbase == 4 && rv[1].length == 1, "P3: 段2 [4,5)");
        }
        bool ok = false;
        verify_range(fs, d, 5 * FS_BLOCK, FS_BLOCK, 4 * FS_BLOCK, 0x51, ok);
        CHECK(ok, "P3: [0,1)pattern [1,4)0 [4,5)pattern");
    }

    /* P4: punch 边界/受理（dir→IS_DIR / sym→STATE / 越 EOF no-op / 溢出 state） */
    {
        std::vector<uint8_t> out; uint64_t got = 0;
        /* root dir → IS_DIR */
        CHECK(fs.direct_file_punch_hole(0, 0, 1) == RC_ERR_IS_DIR, "P4: dir → RC_ERR_IS_DIR");
        /* symlink → STATE */
        if (fs.root_dir_insert_dentry("ps", INODE_TYPE_SOFT_SYM) == RC_OK) {
            uint64_t s = 0;
            if (fs.root_dir_lookup("ps", &s) == RC_OK)
                CHECK(fs.direct_file_punch_hole(s, 0, 1) == RC_ERR_STATE, "P4: SOFT_SYM → RC_ERR_STATE");
        }
        /* 越 EOF no-op */
        const uint64_t e = make_reg(fs, "pa4");
        CHECK(e != PTR_INVALID, "P4: 建 REG");
        if (e == PTR_INVALID) return;
        CHECK(pw(fs, e, 0, 100, 0x61) == RC_OK, "P4: 写 100B");
        CHECK(fs.direct_file_punch_hole(e, 100, 50) == RC_OK, "P4: offset≥size → RC_OK no-op");
        CHECK(snap(fs, e).size == 100, "P4: size 不变");
        CHECK(fs.direct_file_punch_hole(e, 0, 0) == RC_OK, "P4: len==0 → RC_OK");
        CHECK(fs.direct_file_punch_hole(e, ~0ULL - 20, 40) == RC_ERR_STATE, "P4: 溢出 → RC_ERR_STATE");
        bool ok = false;
        verify_range(fs, e, 100, 150, 160, 0x61, ok);   /* [150,160) 空区间 out 但 [0,100) pattern */
        uint64_t got2 = 0; std::vector<uint8_t> o2;
        pr(fs, e, 0, 100, o2, got2);
        CHECK(got2 == 100 && chk_seed(o2.data(), 100, 0, 0x61), "P4: 全读仍 == pattern");
    }
}

/* ════════════════════ T 组：truncate ════════════════════ */
static void group_T(RavenFS_juvenile &fs) {
    printf("── T: truncate（等值/缩小三出口/增大 P4）──\n");

    /* T0: 等值 no-op */
    {
        const uint64_t a = make_reg(fs, "ta");
        CHECK(a != PTR_INVALID, "T0: 建 REG");
        if (a == PTR_INVALID) return;
        CHECK(pw(fs, a, 0, 100, 0x71) == RC_OK, "T0: 写 100B");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_trunacate(a, 100) == RC_OK, "T0: ==size no-op");
        CHECK(snap(fs, a).size == 100 && free_blocks(fs) == fb, "T0: size 不变 无释放");
    }

    /* T1: inline 缩小（size≤160）*/
    {
        const uint64_t b = make_reg(fs, "ta1");
        CHECK(b != PTR_INVALID, "T1: 建 REG");
        if (b == PTR_INVALID) return;
        CHECK(pw(fs, b, 0, 100, 0x72) == RC_OK, "T1: 写 100B");
        CHECK(fs.direct_file_trunacate(b, 40) == RC_OK, "T1: shrink 100→40");
        ino_snap_t s = snap(fs, b);
        CHECK(s.shape == 0 && s.size == 40, "T1: inline size=40");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, b, 0, 40, out, got);
        CHECK(got == 40 && chk_seed(out.data(), 40, 0, 0x72), "T1: [0,40) pattern");
    }

    /* T2: direct 缩小（>160，保 direct，释放尾块）*/
    {
        const uint64_t c = make_reg(fs, "ta2");
        CHECK(c != PTR_INVALID, "T2: 建 REG");
        if (c == PTR_INVALID) return;
        CHECK(pw(fs, c, 0, 5 * FS_BLOCK, 0x73) == RC_OK, "T2: 写 5 块");
        ino_snap_t sc = snap(fs, c);
        CHECK(sc.shape == 1, "T2: 形态 direct");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_trunacate(c, 2 * FS_BLOCK) == RC_OK, "T2: shrink 5→2 块");
        ino_snap_t sa = snap(fs, c);
        CHECK(sa.shape == 1 && sa.size == 2 * FS_BLOCK, "T2: 保 direct size=2 块");
        CHECK(free_blocks(fs) == fb + 3, "T2: 释放 3 尾块");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, c, 0, 2 * FS_BLOCK, out, got);
        CHECK(got == 2 * FS_BLOCK && chk_seed(out.data(), 2 * FS_BLOCK, 0, 0x73), "T2: [0,2块) pattern");
    }

    /* T3: extent 缩小 → 收敛坍缩 direct（单条覆盖 [0,newceil)）*/
    {
        const uint64_t d = make_reg(fs, "ta3");
        CHECK(d != PTR_INVALID, "T3: 建 REG");
        if (d == PTR_INVALID) return;
        CHECK(pw(fs, d, 0, 5 * FS_BLOCK, 0x74) == RC_OK, "T3: 写 5 块 direct");
        /* 打洞 [1,3) → 物化 extent [0,1)+[3,5) */
        CHECK(fs.direct_file_punch_hole(d, 1 * FS_BLOCK, 2 * FS_BLOCK) == RC_OK,
              "T3: punch [1,3) → extent");
        ino_snap_t sd = snap(fs, d);
        CHECK(sd.shape == 2 && sd.size == 5 * FS_BLOCK, "T3: 形态 extent 5 块");
        /* 缩到 1 块 → loose [1,5) → 仅剩 [0,1) → 坍缩 direct */
        CHECK(fs.direct_file_trunacate(d, 1 * FS_BLOCK) == RC_OK, "T3: shrink 5→1 块");
        ino_snap_t sa = snap(fs, d);
        CHECK(sa.shape == 1 && sa.size == 1 * FS_BLOCK, "T3: 坍缩 direct size=1 块");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, d, 0, 1 * FS_BLOCK, out, got);
        CHECK(got == 1 * FS_BLOCK && chk_seed(out.data(), 1 * FS_BLOCK, 0, 0x74), "T3: [0,1块) pattern");
    }

    /* T4: 强制回迁 inline（size>160 → ≤160）*/
    {
        const uint64_t e = make_reg(fs, "ta4");
        CHECK(e != PTR_INVALID, "T4: 建 REG");
        if (e == PTR_INVALID) return;
        CHECK(pw(fs, e, 0, 5000, 0x75) == RC_OK, "T4: 写 5000B");
        CHECK(snap(fs, e).shape == 1, "T4: 形态 direct");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_trunacate(e, 120) == RC_OK, "T4: shrink 5000→120");
        ino_snap_t sa = snap(fs, e);
        CHECK(sa.shape == 0 && sa.size == 120, "T4: 强制回迁 inline size=120");
        CHECK(free_blocks(fs) == fb + 2, "T4: 释放 2 块（5000/4096=2 块）");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, e, 0, 120, out, got);
        CHECK(got == 120 && chk_seed(out.data(), 120, 0, 0x75), "T4: [0,120) pattern");
    }

    /* T5: 缩到 0（强制回迁 inline 空文件）*/
    {
        const uint64_t f = make_reg(fs, "ta5");
        CHECK(f != PTR_INVALID, "T5: 建 REG");
        if (f == PTR_INVALID) return;
        CHECK(pw(fs, f, 0, 5000, 0x76) == RC_OK, "T5: 写 5000B");
        CHECK(fs.direct_file_trunacate(f, 0) == RC_OK, "T5: shrink 5000→0");
        ino_snap_t sa = snap(fs, f);
        CHECK(sa.shape == 0 && sa.size == 0, "T5: inline size=0");
    }

    /* T6: extent 缩小留树（多段 / 保留 >1 记录）*/
    {
        const uint64_t g = make_reg(fs, "ta6");
        CHECK(g != PTR_INVALID, "T6: 建 REG");
        if (g == PTR_INVALID) return;
        CHECK(pw(fs, g, 0, 5 * FS_BLOCK, 0x77) == RC_OK, "T6: 写 5 块 direct");
        /* 打洞 [2,4) → extent [0,2)+[4,5) */
        CHECK(fs.direct_file_punch_hole(g, 2 * FS_BLOCK, 2 * FS_BLOCK) == RC_OK,
              "T6: punch [2,4) → extent");
        ino_snap_t sg = snap(fs, g);
        CHECK(sg.shape == 2 && sg.size == 5 * FS_BLOCK, "T6: extent 5 块");
        const uint64_t fb = free_blocks(fs);
        /* 缩到 3 块 → loose [3,5) → 移除 [4,5)（洞 [3,4) 留着）→ 留树 */
        CHECK(fs.direct_file_trunacate(g, 3 * FS_BLOCK) == RC_OK, "T6: shrink 5→3 块");
        ino_snap_t sa = snap(fs, g);
        CHECK(sa.shape == 2 && sa.size == 3 * FS_BLOCK, "T6: 留树 extent size=3 块");
        CHECK(free_blocks(fs) == fb + 1, "T6: 释放 [4,5) 1 数据块");
        std::vector<extent_t> rv;
        CHECK(range_sample(fs, sa.data_root, 0, 3, rv) && rv.size() == 1, "T6: 树 1 条 [0,2)");
        if (rv.size() == 1)
            CHECK(rv[0].fblkbase == 0 && rv[0].length == 2, "T6: 段 [0,2)");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, g, 0, 2 * FS_BLOCK, out, got);
        CHECK(got == 2 * FS_BLOCK && chk_seed(out.data(), 2 * FS_BLOCK, 0, 0x77), "T6: [0,2块) pattern");
    }

    /* T7: 增大（inline≤160 补零 / inline>160 空写 / direct 增长 / extent 纯推进）*/
    {
        const uint64_t h = make_reg(fs, "ta7a");
        CHECK(h != PTR_INVALID, "T7: 建 REG");
        if (h == PTR_INVALID) return;
        CHECK(pw(fs, h, 0, 100, 0x78) == RC_OK, "T7: 写 100B inline");
        CHECK(fs.direct_file_trunacate(h, 150) == RC_OK, "T7a: grow 100→150");
        ino_snap_t sa = snap(fs, h);
        CHECK(sa.shape == 0 && sa.size == 150, "T7a: inline grow size=150");
        std::vector<uint8_t> out; uint64_t got = 0;
        pr(fs, h, 0, 150, out, got);
        CHECK(got == 150 && chk_seed(out.data(), 100, 0, 0x78) && chk_zero(out.data() + 100, 50),
              "T7a: [0,100)pattern [100,150)0");

        /* inline>160 → 空写（df_pipeline） */
        CHECK(fs.direct_file_trunacate(h, 2 * FS_BLOCK) == RC_OK, "T7b: grow 150→2块");
        ino_snap_t sb = snap(fs, h);
        CHECK(sb.shape == 1 && sb.size == 2 * FS_BLOCK, "T7b: 迁出 direct 2 块");
        pr(fs, h, 0, 2 * FS_BLOCK, out, got);
        CHECK(got == 2 * FS_BLOCK && chk_seed(out.data(), 100, 0, 0x78) &&
              chk_zero(out.data() + 100, 2 * FS_BLOCK - 100),
              "T7b: [0,100)pattern [100,2块)0（[100,150) 是此前增长区）");

        /* extent 纯 size 推进 */
        const uint64_t i = make_reg(fs, "ta7c");
        CHECK(i != PTR_INVALID, "T7: 建 REG");
        if (i == PTR_INVALID) return;
        CHECK(pw(fs, i, 0, 5 * FS_BLOCK, 0x79) == RC_OK, "T7c: 写 5 块 direct");
        /* 打洞 [2,3) → extent [0,2)+[3,5) */
        CHECK(fs.direct_file_punch_hole(i, 2 * FS_BLOCK, 1 * FS_BLOCK) == RC_OK,
              "T7c: punch [2,3) → extent");
        ino_snap_t si = snap(fs, i);
        CHECK(si.shape == 2 && si.size == 5 * FS_BLOCK, "T7c: extent 5 块");
        const uint64_t fb = free_blocks(fs);
        CHECK(fs.direct_file_trunacate(i, 6 * FS_BLOCK) == RC_OK, "T7c: grow 5→6 块");
        ino_snap_t sis = snap(fs, i);
        CHECK(sis.shape == 2 && sis.size == 6 * FS_BLOCK, "T7c: extent 纯推进 size=6 块");
        CHECK(free_blocks(fs) == fb, "T7c: extent 零分配");
        std::vector<uint8_t> o2; uint64_t g2 = 0;
        pr(fs, i, 5 * FS_BLOCK, 1 * FS_BLOCK, o2, g2);
        CHECK(g2 == 1 * FS_BLOCK && chk_zero(o2.data(), 1 * FS_BLOCK), "T7c: 新域 [5,6) 读 0");
    }

    /* T8: 受理（dir/sym）*/
    {
        CHECK(fs.direct_file_trunacate(0, 10) == RC_ERR_IS_DIR, "T8: dir → RC_ERR_IS_DIR");
        uint64_t s = 0;
        if (fs.root_dir_lookup("ps", &s) == RC_OK)
            CHECK(fs.direct_file_trunacate(s, 10) == RC_ERR_STATE, "T8: SOFT_SYM → RC_ERR_STATE");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 1; }
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 1; }
    uint64_t rc = 0;
    RavenFS_juvenile fs(mnt, &rc);
    if (rc != RC_OK || !fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    group_P(fs);
    group_T(fs);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
