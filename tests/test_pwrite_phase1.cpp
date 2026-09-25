/* ═══════════════════════════════════════════════════════════════════
 * test_pwrite_phase1.cpp — dfprealloc_and_plan（pwrite 五段管线 phase1）专项
 *
 * 被测合同（Docs/pwrite_pipeline.md §5/§5.1 + 09-09 拍板）：
 *   1. 输入 inode 内存映像指针（InodeSession 作用域），纯读零副作用；
 *   2. 输出 affect_extents = 纯新段：fblkbase 升序 / 互不重叠 / 与现映射
 *      零相交 / length=块数 / block_base 恒有效（位图真占用）；
 *   3. 三初始态分派：inline 迁出（direct 候选 hard 整域 → 降级物化∪写域
 *      洞留空）、direct 增长（锚定旧 run 物理尾）、extent 逐洞（起点紧贴
 *      左邻记录终点 → 锚 = 左邻 phys 尾）；
 *   4. 碎片部分满足 = 榨 soft 裂段；唯一失败面 = 某轮一块都榨不出 →
 *      RC_ERR_NO_BLOCK + 整账本逆序回滚（位图账本零残留）。
 *
 * 与纯树测试的差异：phase1 真碰位图 → 测试里一切既有映射的 phys 必须
 * 真实分配（block_alloc_*），断言才建立在位图真相上；用 block_test 校验
 * 占用、bitmap_mgr->block_free_total() 校验账本。
 *
 * 场景独立性纪律：每个成功场景校验后把该场景分配的段 block_free 释放
 * （锚定块/软分块回归自由），否则跨场景的锚定断言会被前序场景的占用
 * 污染（allocator hint 单调只进不退 → 已释放的锚点区不会被新分配复用，
 * 锚定断言保持确定性）。
 *
 * 环境：真盘镜像（mkfs.ravenfs -f），每次运行前重 mkfs（一次性镜像，
 * 数据块不做清理回收——仓库惯例）。mount 用 mmap 后端（is_device_direct
 * =false），inode/mdata 走 canvas，状态写后立即可见。
 *
 * 组布局：
 *   A inline 态（豁免/迁出 direct 候选）
 *   B direct 态（覆盖零新段/增长锚定贴尾/锚被占降级）
 *   C extent 态（覆盖空/洞锚定/洞中段无锚/尾增长锚定/跨记录多洞段/截断）
 *   D 用尽与回滚（claim-all + 双窗口雕塑 → 整败/碎片裂段成功/两段降级
 *      洞留空/extent 用尽不碰树）
 *
 * 用法：ravenfs_juv_pwrite_phase1 <device>
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession 完整类型 */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_session 完整类型 */
#include "../include/ravenfs/bitmap_slices.hpp"       /* bitmap_slices_mgr_t（free 审计） */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <list>
#include <vector>
#include <algorithm>
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
    case RC_ERR_OVERLAP: return "RC_ERR_OVERLAP";
    default: return "RC_???";
    }
}

/* ── 挂载（mmap 后端；镜像由外部 mkfs 好）── */
static linux_userspace_mount_t make_mount(const char *path, uint32_t cpu_count = 1) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.logical_processor_count = cpu_count;
    return mnt;
}

/* ── 正门建 REG 文件：insert + lookup 拿 inum ── */
static uint64_t make_reg(RavenFS_juvenile &fs, const char *name) {
    if (fs.root_dir_insert_dentry(name, INODE_TYPE_REGULAR) != RC_OK) return PTR_INVALID;
    uint64_t inum = 0;
    if (fs.root_dir_lookup(name, &inum) != RC_OK) return PTR_INVALID;
    return inum;
}

/* ── 审计读数 / 快照 ── */
static uint64_t free_blocks(RavenFS_juvenile &fs) {
    return fs.bitmap_mgr->block_free_total();
}
struct ino_snap_t { uint64_t size, data_root; uint64_t direct_bit; };
static ino_snap_t snap(RavenFS_juvenile &fs, uint64_t inum) {
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino_snap_t s; s.size = ino.size; s.data_root = ino.data_root;
    s.direct_bit = ino.is_data_root_direct_bit;
    return s;
}
static bool snap_eq(const ino_snap_t &a, const ino_snap_t &b) {
    return a.size == b.size && a.data_root == b.data_root && a.direct_bit == b.direct_bit;
}

/* ── phase1 生产同款调用姿势：InodeSession 作用域取映像指针传参 ── */
static result_code_t p1(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len,
                        std::list<extent_t> &out) {
    InodeSession s(fs.inode_cache_mgr, inum);
    if (!s.inode) return RC_ERR_STATE;
    return fs.dfprealloc_and_plan(s.inode, off, len, out);
}

/* 场景收尾：释放本场景分配段（场景独立性纪律，见头注） */
static void free_plan(RavenFS_juvenile &fs, const std::list<extent_t> &plan) {
    for (const extent_t &e : plan)
        fs.block_free(e.block_base.ptr, e.length);
}

/* ── 输出校验：不变量逐条 + 覆盖并集与期望逻辑区间集合比对 + 位图账本 ──
 * expect = {fb,len} 升序集合；plan 因 phys 断点可能多条（逻辑相邻合并后比对）。 */
static void verify_plan(RavenFS_juvenile &fs, const std::list<extent_t> &plan,
                        const std::vector<std::pair<uint64_t, uint64_t>> &expect,
                        uint64_t free_before, const char *tag) {
    uint64_t sum = 0, prev_end = 0;
    bool first = true, ok = true;
    for (const extent_t &e : plan) {
        if (e.length == 0) ok = false;
        if (e.block_base.ptr == PTR_INVALID_52) ok = false;
        if (!first && e.fblkbase < prev_end) ok = false;   /* 逆序或重叠 */
        for (uint64_t i = 0; i < e.length; i++)
            if (!fs.block_test(e.block_base.ptr + i)) ok = false;   /* 未占用 */
        sum += e.length;
        prev_end = e.fblkbase + e.length;
        first = false;
    }
    CHECK(ok, "%s: 不变量绿（升序/不重叠/len≥1/phys 有效+真占用）", tag);

    std::vector<std::pair<uint64_t, uint64_t>> cov;
    for (const extent_t &e : plan) cov.push_back({e.fblkbase, e.length});
    std::sort(cov.begin(), cov.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (auto &c : cov) {
        if (!merged.empty() && merged.back().first + merged.back().second == c.first)
            merged.back().second += c.second;
        else
            merged.push_back(c);
    }
    bool same = merged.size() == expect.size();
    if (same) {
        for (size_t i = 0; i < merged.size(); i++)
            if (merged[i] != expect[i]) { same = false; break; }
    }
    CHECK(same, "%s: 覆盖 == 期望（%zu 条区间）", tag, expect.size());

    CHECK(free_before - free_blocks(fs) == sum,
          "%s: 位图账本（alloc %llu == 期望 %llu）",
          tag, (unsigned long long)(free_before - free_blocks(fs)),
          (unsigned long long)sum);
}

/* ── 状态构造 ── */

static void set_inline(RavenFS_juvenile &fs, uint64_t inum, uint64_t size, uint8_t fill) {
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino.size = size;
    ino.is_data_root_direct_bit = 0;
    ino.data_root = PTR_INVALID;
    memset(ino.inline_data, fill, sizeof(ino.inline_data));
    fs.inode_write(inum, &ino);
}

/* direct 态：真分配连续 run → inode 直指（调用方保证 ceil(size/4K)==blocks） */
static uint64_t set_direct(RavenFS_juvenile &fs, uint64_t inum, uint64_t blocks, uint64_t size) {
    const uint64_t base = fs.block_alloc_hard(blocks, 0);
    if (base == PTR_INVALID) return PTR_INVALID;
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino.size = size;
    ino.data_root = base;
    ino.is_data_root_direct_bit = 1;
    fs.inode_write(inum, &ino);
    return base;
}

/* extent 态：建空根叶（2 块真分配）→ inode size>160 / direct_bit=0 */
static uint64_t set_extent_root(RavenFS_juvenile &fs, uint64_t inum, uint64_t fsize) {
    const uint64_t root = fs.block_alloc_hard(2, 1);
    if (root == PTR_INVALID) return PTR_INVALID;
    {
        mdata_piece_session s(fs.mdata_piece_mgr, root);
        if (s.index_error() || !s) return PTR_INVALID;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        leaf->meta.magics[0] = BPTREE_MAGIC1;
        leaf->meta.magics[1] = BPTREE_MAGIC2;
        leaf->meta.magics[2] = BPTREE_MAGIC3;
        leaf->meta.magics[3] = BPTREE_MAGIC4;
        leaf->meta.parent_ptr.entry_count   = 0;
        leaf->meta.parent_ptr.is_inner_node = 0;
        leaf->meta.parent_ptr.is_root_node  = 1;
        leaf->meta.parent_ptr.ptr           = inum;   /* "." 审计锚 */
        leaf->meta.ptrs[0].ptr = PTR_INVALID;
        leaf->meta.ptrs[1].ptr = PTR_INVALID;
        leaf->meta.ptrs[2].ptr = PTR_INVALID;
        s.dirty();
    }
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino.size = fsize;
    ino.data_root = root;
    ino.is_data_root_direct_bit = 0;
    fs.inode_write(inum, &ino);
    return root;
}

/* extent 态 map 一条记录：真分配 phys run → map（返回 phys；失败 PTR_INVALID） */
static uint64_t map_rec(RavenFS_juvenile &fs, uint64_t &root_blk, uint64_t inum,
                        uint64_t fb, uint64_t len) {
    const uint64_t phys = fs.block_alloc_hard(len, 0);
    if (phys == PTR_INVALID) return PTR_INVALID;
    extent_t e;
    e.fblkbase = fb;
    e.length = len;
    e.block_base.ptr = phys;
    e.block_base.free_to_use = 0;
    if (fs.extent_map_range(root_blk, inum, e) != RC_OK) return PTR_INVALID;
    return phys;
}

/* 隔离垫片：分配后释放一段，hint 推过 → 后续记录 phys 不贴前一条
 * （贴邻会毁掉"锚定 phys == 左邻尾"断言前提：左邻尾即被占）。 */
static void spacer(RavenFS_juvenile &fs, uint64_t blocks) {
    const uint64_t b = fs.block_alloc_hard(blocks, 0);
    if (b != PTR_INVALID) fs.block_free(b, blocks);
}

/* extent 态叶链记录数（轻量审计：phase1 失败不得碰树） */
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
    linux_userspace_mount_t mnt = make_mount(argv[1], 1);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 1; }
    uint64_t rc = 0;
    RavenFS_juvenile fs(mnt, &rc);
    if (rc != RC_OK || !fs.is_mounted()) {
        printf("mount failed rc=%s\n", rc_name(rc));
        return 1;
    }

    /* ══════════ A：inline 态 ══════════ */
    printf("\n-- A: inline 态 --\n");
    {
        const uint64_t f1 = make_reg(fs, "f_a1");
        const uint64_t f2 = make_reg(fs, "f_a2");
        CHECK(f1 != PTR_INVALID && f2 != PTR_INVALID, "A0: 正门建 REG x2");
        set_inline(fs, f2, 150, 0xAB);
        std::list<extent_t> plan;
        uint64_t fb;

        /* A1 零长写 → 空表 */
        rc = p1(fs, f1, 100, 0, plan);
        CHECK(rc == RC_OK && plan.empty(), "A1: len=0 → RC_OK + 空表");
        /* A2 纯内联写（size0 文件 off100 len50 → newsize 150 ≤160）→ 空表 */
        rc = p1(fs, f1, 100, 50, plan);
        CHECK(rc == RC_OK && plan.empty(), "A2: off100 len50（≤160）→ 豁免空表");
        /* A3 inline→inline 覆盖（size150 文件 off0 len10）→ 空表 */
        rc = p1(fs, f2, 0, 10, plan);
        CHECK(rc == RC_OK && plan.empty(), "A3: size150 off0 len10 → 豁免空表");

        /* A4 迁出 direct 候选（单块）：off0 len2000 → [0,1) */
        ino_snap_t s0 = snap(fs, f1);
        fb = free_blocks(fs);
        rc = p1(fs, f1, 0, 2000, plan);
        CHECK(rc == RC_OK && plan.size() == 1 && plan.front().fblkbase == 0,
              "A4: 迁出 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{0, 1}}, fb, "A4 [0,1)");
        CHECK(snap_eq(s0, snap(fs, f1)), "A4: inode 未动（纯读）");
        free_plan(fs, plan);
        CHECK(snap_eq(s0, snap(fs, f1)), "A4: 释放后 inode 仍未动");

        /* A5 迁出多块整域：off0 len10000 → [0,3) */
        fb = free_blocks(fs);
        rc = p1(fs, f1, 0, 10000, plan);
        verify_plan(fs, plan, {{0, 3}}, fb, "A5 [0,3)");
        free_plan(fs, plan);

        /* A6 稀疏远写：off100000 len100 → [0,25)（direct 占位语义） */
        fb = free_blocks(fs);
        rc = p1(fs, f1, 100000, 100, plan);
        verify_plan(fs, plan, {{0, 25}}, fb, "A6 稀疏远写 [0,25)");
        free_plan(fs, plan);

        /* A7 size150 文件跨 160 迁出：off150 len100 → newsize250 → [0,1)（物化块） */
        fb = free_blocks(fs);
        rc = p1(fs, f2, 150, 100, plan);
        verify_plan(fs, plan, {{0, 1}}, fb, "A7 250B 迁出 [0,1)");
        free_plan(fs, plan);
    }

    /* ══════════ B：direct 态 ══════════ */
    printf("\n-- B: direct 态 --\n");
    {
        const uint64_t d1 = make_reg(fs, "f_b1");
        const uint64_t d2 = make_reg(fs, "f_b2");
        CHECK(d1 != PTR_INVALID && d2 != PTR_INVALID, "B0: 正门建 REG x2");
        const uint64_t B1 = set_direct(fs, d1, 3, 9000);   /* run [B1,B1+3)，oldceil=3 */
        CHECK(B1 != PTR_INVALID, "B0: direct 构造 d1 base=%llu", (unsigned long long)B1);
        std::list<extent_t> plan;
        uint64_t fb;

        /* B1 覆盖写（纯区内）→ 空表 */
        rc = p1(fs, d1, 100, 1000, plan);
        CHECK(rc == RC_OK && plan.empty(), "B1: 覆盖写 → 空表 rc=%s", rc_name(rc));

        /* B2 增长但未跨块界（end=9000==size → newceil==oldceil）→ 空表 */
        rc = p1(fs, d1, 5000, 4000, plan);
        CHECK(rc == RC_OK && plan.empty(), "B2: 增长不跨块界 → 空表 rc=%s", rc_name(rc));

        /* B3 增长跨块界 + 锚定贴尾：off8192 len5000 → newceil4，段 [3,1)@B1+3 */
        fb = free_blocks(fs);
        rc = p1(fs, d1, 8192, 5000, plan);
        CHECK(rc == RC_OK && plan.size() == 1 && plan.front().fblkbase == 3 &&
              plan.front().length == 1 && plan.front().block_base.ptr == B1 + 3,
              "B3: 增长锚定贴尾 → {3,1,B1+3} rc=%s", rc_name(rc));
        CHECK(free_blocks(fs) == fb - 1, "B3: 位图账本 -1");
        free_plan(fs, plan);

        /* B4 锚被占 → 降级 soft 别处（phys != B2+3 确定性：该块已被占） */
        const uint64_t B2 = set_direct(fs, d2, 3, 9000);
        CHECK(B2 != PTR_INVALID, "B0: direct 构造 d2");
        uint64_t r = 99;
        CHECK(fs.block_occupy_soft(B2 + 3, 1, &r) == B2 + 3 && r == 0,
              "B4: 先占尾块 B2+3");
        fb = free_blocks(fs);
        rc = p1(fs, d2, 8192, 5000, plan);
        CHECK(rc == RC_OK && plan.size() == 1 && plan.front().fblkbase == 3 &&
              plan.front().length == 1 && plan.front().block_base.ptr != B2 + 3,
              "B4: 锚被占 → soft 别处 {3,1} phys=%llu rc=%s",
              (unsigned long long)(plan.empty() ? 0 : plan.front().block_base.ptr),
              rc_name(rc));
        CHECK(free_blocks(fs) == fb - 1, "B4: 位图账本 -1");
        free_plan(fs, plan);
    }

    /* ══════════ C：extent 态 ══════════ */
    printf("\n-- C: extent 态 --\n");
    {
        const uint64_t e1 = make_reg(fs, "f_c1");
        CHECK(e1 != PTR_INVALID, "C0: 正门建 REG");
        uint64_t root = set_extent_root(fs, e1, 8 * FS_BLOCK);   /* size 8 块 */
        CHECK(root != PTR_INVALID, "C0: extent 根叶");
        const uint64_t P = map_rec(fs, root, e1, 0, 2);
        spacer(fs, 64);                            /* 隔离：Q 不贴 P */
        const uint64_t Q = map_rec(fs, root, e1, 5, 1);
        CHECK(P != PTR_INVALID && Q != PTR_INVALID, "C0: map 记录 [0,2)@P [5,6)@Q");
        CHECK(Q != P + 2 && Q != P + 1, "C0: 隔离生效（Q=%llu 不贴 P run）",
              (unsigned long long)Q);
        CHECK(tree_records(fs, root) == 2, "C0: 树 2 条记录");
        std::list<extent_t> plan;
        uint64_t fb;

        /* C1 覆盖写（区内）→ 空表 */
        rc = p1(fs, e1, 100, 1000, plan);
        CHECK(rc == RC_OK && plan.empty(), "C1: 区内覆盖 → 空表 rc=%s", rc_name(rc));
        rc = p1(fs, e1, 100, 6000, plan);
        CHECK(rc == RC_OK && plan.empty(), "C1b: 覆盖至记录末 → 空表 rc=%s", rc_name(rc));

        /* C2 洞头写（gap [2,5) 头，段起点==左邻终点）→ 锚定 P+2 */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 8192, 4096, plan);         /* D=[2,3) */
        CHECK(rc == RC_OK, "C2: 洞头写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{2, 1}}, fb, "C2 [2,1)");
        if (plan.size() == 1)
            CHECK(plan.front().block_base.ptr == P + 2, "C2: 锚定左邻尾 P+2");
        free_plan(fs, plan);

        /* C3 洞中段起点（block 3，左邻终点 2 ≠ 3）→ 无锚 soft（只断覆盖） */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 12288, 4096, plan);        /* D=[3,4) */
        CHECK(rc == RC_OK, "C3: 洞中段写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{3, 1}}, fb, "C3 [3,1)");
        free_plan(fs, plan);

        /* C4 尾增长锚定：off=block6 → [6,1)@Q+1（左邻 [5,6) 终点==6） */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 24576, 4096, plan);        /* D=[6,7) */
        CHECK(rc == RC_OK, "C4: 尾增长写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{6, 1}}, fb, "C4 [6,1)");
        if (plan.size() == 1)
            CHECK(plan.front().block_base.ptr == Q + 1, "C4: 锚定左邻尾 Q+1");
        free_plan(fs, plan);

        /* C5 跨记录长写 D=[0,9)：洞 [2,5) 锚 P+2 + 尾 [6,9) 锚 Q+1；记录区跳过 */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 0, 9 * FS_BLOCK, plan);
        CHECK(rc == RC_OK, "C5: 跨记录长写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{2, 3}, {6, 3}}, fb, "C5 [2,5)+[6,9)");
        if (plan.size() == 2) {
            auto it = plan.begin();
            CHECK(it->fblkbase == 2 && it->block_base.ptr == P + 2, "C5: 首段锚 P+2");
            ++it;
            CHECK(it->fblkbase == 6 && it->block_base.ptr == Q + 1, "C5: 次段锚 Q+1");
        }
        free_plan(fs, plan);

        /* C6 D 被下一记录截断：[2,4)（gap 只写前半，[4,5) 留空） */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 8192, 8192, plan);         /* D=[2,4) */
        CHECK(rc == RC_OK, "C6: 洞截断写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{2, 2}}, fb, "C6 [2,2)");
        if (plan.size() == 1)
            CHECK(plan.front().block_base.ptr == P + 2, "C6: 锚定 P+2");
        free_plan(fs, plan);
    }

    /* ══════════ D：用尽与回滚 ══════════ */
    printf("\n-- D: 用尽与回滚 --\n");
    {
        const uint64_t f_big = make_reg(fs, "f_d_big");    /* inline size0 */
        const uint64_t f_frac = make_reg(fs, "f_d_frac");  /* inline size0 */
        const uint64_t e1 = make_reg(fs, "f_d_e1");
        CHECK(f_big != PTR_INVALID && f_frac != PTR_INVALID && e1 != PTR_INVALID,
              "D0: 正门建 REG x3");
        uint64_t root = set_extent_root(fs, e1, 8 * FS_BLOCK);
        CHECK(root != PTR_INVALID, "D0: extent 根叶");
        const uint64_t P = map_rec(fs, root, e1, 0, 2);
        spacer(fs, 64);
        const uint64_t Q = map_rec(fs, root, e1, 5, 1);
        CHECK(P != PTR_INVALID && Q != PTR_INVALID, "D0: map 记录");
        CHECK(tree_records(fs, root) == 2, "D0: 基线 2 条记录");

        /* claim-all → 双窗口雕塑：自由区 = [C0, C0+120) ∪ [C0+200, C0+320) */
        uint64_t C0 = PTR_INVALID, total = 0;
        claim_all(fs, &C0, &total);
        CHECK(free_blocks(fs) == 0, "D1: claim-all 后 free==0");
        CHECK(C0 != PTR_INVALID && total >= 400, "D1: 自由区 ≥400 块（total=%llu）",
              (unsigned long long)total);
        fs.block_free(C0, 120);
        fs.block_free(C0 + 200, 120);
        CHECK(free_blocks(fs) == 240, "D1: 双窗口雕塑 free==240");
        std::list<extent_t> plan;
        uint64_t fb;

        /* D2 整败：domain 300 > 总自由 240 → NO_BLOCK + 整账本回滚 */
        fb = free_blocks(fs);
        ino_snap_t s0 = snap(fs, f_big);
        rc = p1(fs, f_big, 0, 300 * FS_BLOCK, plan);
        CHECK(rc == RC_ERR_NO_BLOCK && plan.empty(), "D2: 用尽 → NO_BLOCK + 空表 rc=%s",
              rc_name(rc));
        CHECK(free_blocks(fs) == fb, "D2: 回滚零残留（free 复原）");
        CHECK(snap_eq(s0, snap(fs, f_big)), "D2: inode 未动");

        /* D3 碎片裂段成功：domain 150 ≤ 240、但无 ≥150 连续 run →
         * hard 败 → soft 榨 120+30 两条（phys 断，coalesce 不合并） */
        fb = free_blocks(fs);
        rc = p1(fs, f_frac, 0, 150 * FS_BLOCK, plan);
        CHECK(rc == RC_OK && plan.size() == 2, "D3: 碎片裂段 2 条 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{0, 150}}, fb, "D3 覆盖 [0,150)");
        if (plan.size() == 2) {
            auto it = plan.begin();
            CHECK(it->fblkbase == 0 && it->block_base.ptr == C0, "D3: 首段吃窗 A");
            ++it;
            CHECK(it->fblkbase == 120 && it->block_base.ptr == C0 + 200,
                  "D3: 次段吃窗 B（phys 断不合并）");
        }

        /* D4 两段降级（off 中段起）：写域 [200,201) + 物化 [0,1)，洞 [1,200) 留空。
         * 此时自由只剩窗 B 余量 90 → hard(201) 必败 → 降级路径。 */
        fb = free_blocks(fs);
        rc = p1(fs, f_frac, 200 * FS_BLOCK, 100, plan);
        CHECK(rc == RC_OK, "D4: 两段降级 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{0, 1}, {200, 1}}, fb, "D4 物化+写域（洞留空）");
        CHECK(plan.size() == 2, "D4: 恰 2 段（[0,1) + [200,1)）");

        /* D5 extent 态用尽：释放锚点块 Q+1/P+2 保持可锚；domain 300 → 先锚后榨 → 败 */
        fs.block_free(Q + 1, 1);
        fs.block_free(P + 2, 1);
        fb = free_blocks(fs);
        rc = p1(fs, e1, 24576, 300 * FS_BLOCK, plan);   /* D=[6,306)：尾洞段 */
        CHECK(rc == RC_ERR_NO_BLOCK && plan.empty(), "D5: extent 用尽 → NO_BLOCK rc=%s",
              rc_name(rc));
        CHECK(free_blocks(fs) == fb, "D5: 回滚零残留");
        CHECK(tree_records(fs, root) == 2, "D5: 树未动（仍 2 条记录）");

        /* D6 失败后健康：锚定洞写恢复可用（P+2 已被回滚释放） */
        fb = free_blocks(fs);
        rc = p1(fs, e1, 8192, 4096, plan);
        CHECK(rc == RC_OK, "D6: 回滚后锚定写 rc=%s", rc_name(rc));
        verify_plan(fs, plan, {{2, 1}}, fb, "D6 [2,1)");
        if (plan.size() == 1)
            CHECK(plan.front().block_base.ptr == P + 2, "D6: 锚定 P+2");
    }

    (void)audit_clean(fs, "pwrite-phase1-final");
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
