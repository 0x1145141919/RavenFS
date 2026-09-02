/* ═══════════════════════════════════════════════════════════════════
 * test_extent_walk.cpp — extent_Bptree_walk 左闭右开语义专项测试
 *
 * 手工铺一棵双层 extent 树（绕过 inode，直接 alloc 块 + 填节点）：
 *
 *   根内部 (is_root=1, entry=2)
 *     hints[0].key = 40  → 左叶子 [10,20) [30,40)
 *     hints[1].key = 90  → 右叶子 [50,60) [70,90)
 *
 * 覆盖用例（半开区间核心语义）：
 *   1. 区间内命中（起点/中间/末尾-1）
 *   2. 开边界不命中（target == fblkbase+length）
 *   3. 空洞不命中（GLB 是插入锚点）
 *   4. target == 左子树开上界(40) → 必须落右子树（严格上界路由）
 *   5. 最左击穿（target < 全树最小键）→ 左叶子 GLB=NONE
 *   6. 最右击穿（target ≥ 全树最大开上界 90）→ 最右叶子末尾
 *   7. target == 全树最大开上界 90 → 不命中（半开）
 *
 * 用法：ravenfs_juv_extent_walk <device>   （已 mkfs 的 ravenfs v1.1 盘/镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = false;
    mnt.param = {};
    mnt.logical_processor_count = 1;
    return mnt;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static void fill_extent(extent_t *e, uint64_t fblkbase, uint64_t length, uint64_t phys) {
    e->fblkbase = fblkbase;
    e->length = length;
    e->block_base.ptr = phys;
    e->block_base.free_to_use = 0;
}

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK:             return "RC_OK";
    case RC_ERR_OPEN:       return "RC_ERR_OPEN";
    case RC_ERR_MAGIC:      return "RC_ERR_MAGIC";
    case RC_ERR_VERSION:    return "RC_ERR_VERSION";
    case RC_ERR_CRC:        return "RC_ERR_CRC";
    case RC_ERR_BITMAP_SELF:return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST:      return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND:  return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK:   return "RC_ERR_NO_BLOCK";
    case RC_ERR_NO_INODE:   return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN:   return "RC_ERR_NAME_LEN";
    case RC_ERR_IO:         return "RC_ERR_IO";
    case RC_ERR_STATE:      return "RC_ERR_STATE";
    default:                return "RC_???";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }

    uint64_t rc;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { printf("mount failed rc=%lu\n", rc); return 1; }

    /* ── 铺树：根内部 + 左/右叶子（各 2 块，2^1 对齐） ── */
    uint64_t root_blk = fs.block_alloc_hard(2, 1);
    uint64_t leafL_blk = fs.block_alloc_hard(2, 1);
    uint64_t leafR_blk = fs.block_alloc_hard(2, 1);
    if (root_blk == PTR_INVALID || leafL_blk == PTR_INVALID || leafR_blk == PTR_INVALID) {
        printf("block alloc failed\n"); return 1;
    }
    printf("nodes: root=%lu leafL=%lu leafR=%lu\n", root_blk, leafL_blk, leafR_blk);

    /* 左叶子：[10,20) [30,40)；兄弟链：无前驱，后继=右叶子 */
    {
        mdata_piece_session s(fs.mdata_piece_mgr, leafL_blk);
        auto *L = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        L->meta.magics[0] = BPTREE_MAGIC1; L->meta.magics[1] = BPTREE_MAGIC2;
        L->meta.magics[2] = BPTREE_MAGIC3; L->meta.magics[3] = BPTREE_MAGIC4;
        L->meta.parent_ptr.entry_count   = 2;
        L->meta.parent_ptr.is_inner_node = 0;
        L->meta.parent_ptr.is_root_node  = 0;
        L->meta.parent_ptr.ptr           = root_blk;
        L->meta.ptrs[1].ptr = PTR_INVALID;   /* 链表头：无前驱 */
        L->meta.ptrs[2].ptr = leafR_blk;            /* 后继 = 右叶子 */
        fill_extent(&L->extents[0], 10, 10, 100);
        fill_extent(&L->extents[1], 30, 10, 200);
        s.dirty();
    }
    /* 右叶子：[50,60) [70,90)；兄弟链：前驱=左叶子，无后继（链表尾） */
    {
        mdata_piece_session s(fs.mdata_piece_mgr, leafR_blk);
        auto *R = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        R->meta.magics[0] = BPTREE_MAGIC1; R->meta.magics[1] = BPTREE_MAGIC2;
        R->meta.magics[2] = BPTREE_MAGIC3; R->meta.magics[3] = BPTREE_MAGIC4;
        R->meta.parent_ptr.entry_count   = 2;
        R->meta.parent_ptr.is_inner_node = 0;
        R->meta.parent_ptr.is_root_node  = 0;
        R->meta.parent_ptr.ptr           = root_blk;
        R->meta.ptrs[1].ptr = leafL_blk;            /* 前驱 = 左叶子 */
        R->meta.ptrs[2].ptr = PTR_INVALID;   /* 链表尾：无后继 */
        fill_extent(&R->extents[0], 50, 10, 300);
        fill_extent(&R->extents[1], 70, 20, 400);
        s.dirty();
    }
    /* 根内部：key = 子树最大开上界 */
    {
        mdata_piece_session s(fs.mdata_piece_mgr, root_blk);
        auto *I = (Bptree_extent_inner_node_t *)s.data;
        memset(s.data, 0, 8192);
        I->meta.magics[0] = BPTREE_MAGIC1; I->meta.magics[1] = BPTREE_MAGIC2;
        I->meta.magics[2] = BPTREE_MAGIC3; I->meta.magics[3] = BPTREE_MAGIC4;
        I->meta.parent_ptr.entry_count   = 2;
        I->meta.parent_ptr.is_inner_node = 1;
        I->meta.parent_ptr.is_root_node  = 1;
        I->meta.parent_ptr.ptr           = 777;   /* 根 → inode（walk 不校验值） */
        I->extents[0].key = 40;  I->extents[0].sub_node_ptr.ptr = leafL_blk;
        I->extents[0].sub_node_ptr.free_to_use = 0;
        I->extents[1].key = 90;  I->extents[1].sub_node_ptr.ptr = leafR_blk;
        I->extents[1].sub_node_ptr.free_to_use = 0;
        s.dirty();
    }

    /* ── 用例表：target → 期望(叶子块, GLB, hit) ── */
    struct tc { uint64_t t; uint64_t leaf; uint16_t glb; bool hit; const char *desc; };
    const tc cases[] = {
        { 0,   leafL_blk, 0xFFFF, false, "最左击穿：target<全树最小键 → 左叶子 GLB=NONE" },
        { 10,  leafL_blk, 0,      true,  "区间起点命中 [10,20)" },
        { 15,  leafL_blk, 0,      true,  "区间中间命中 [10,20)" },
        { 19,  leafL_blk, 0,      true,  "区间末尾-1 命中 [10,20)" },
        { 20,  leafL_blk, 0,      false, "开边界 20 ∉ [10,20)（半开不命中，GLB 锚点=0）" },
        { 25,  leafL_blk, 0,      false, "空洞 [20,30) 不命中" },
        { 30,  leafL_blk, 1,      true,  "第二区间起点命中 [30,40)" },
        { 39,  leafL_blk, 1,      true,  "第二区间末尾-1 命中 [30,40)" },
        { 40,  leafR_blk, 0xFFFF, false, "target==左子树开上界 → 落右叶子（严格上界路由），GLB=NONE" },
        { 45,  leafR_blk, 0xFFFF, false, "空洞 [40,50) → 右叶子 GLB=NONE（最左插入）" },
        { 50,  leafR_blk, 0,      true,  "右叶子首区间起点命中 [50,60)" },
        { 60,  leafR_blk, 0,      false, "开边界 60 ∉ [50,60)" },
        { 65,  leafR_blk, 0,      false, "空洞 [60,70) 不命中" },
        { 70,  leafR_blk, 1,      true,  "右叶子次区间起点命中 [70,90)" },
        { 85,  leafR_blk, 1,      true,  "右叶子次区间中间命中" },
        { 89,  leafR_blk, 1,      true,  "右叶子次区间末尾-1 命中" },
        { 90,  leafR_blk, 1,      false, "target==全树最大开上界 → 击穿落最右叶子，但不命中（半开）" },
        { 1000, leafR_blk, 1,     false, "全局右击穿 → 最右叶子末尾锚点 GLB=1" },
    };

    for (const tc &c : cases) {
        bptree_path_t trace{};
        result_code_t r = fs.extent_Bptree_walk(root_blk, &trace, c.t);
        bool ok = (r == RC_OK)
               && (trace.blocks[trace.depth - 1] == c.leaf)
               && (trace.child_idx[trace.depth - 1] == c.glb)
               && (trace.is_leaf_hit == c.hit);
        CHECK(ok, "target=%-5lu → leaf=%s glb=%s hit=%s | %s",
              c.t,
              (r == RC_OK && trace.blocks[trace.depth - 1] == c.leaf) ? "ok" : "WRONG_LEAF",
              (r == RC_OK && trace.child_idx[trace.depth - 1] == c.glb) ? "ok" : "WRONG_GLB",
              (r == RC_OK && trace.is_leaf_hit == c.hit) ? "ok" : "WRONG_HIT",
              c.desc);
        if (!ok) {
            printf("        got: rc=%s leaf=%lu glb=%u hit=%d\n",
                   rc_name(r), r == RC_OK ? trace.blocks[trace.depth - 1] : 0,
                   r == RC_OK ? trace.child_idx[trace.depth - 1] : 0,
                   r == RC_OK ? trace.is_leaf_hit : 0);
        }
    }

    /* ══════════════════ extent_iterator 用例 ══════════════════ */
    printf("\n-- extent_iterator --\n");

    /* 单叶子根树（第三棵）：extent [100,120)，is_root=1，无兄弟 */
    uint64_t solo_blk = fs.block_alloc_hard(2, 1);
    {
        mdata_piece_session s(fs.mdata_piece_mgr, solo_blk);
        auto *S = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        S->meta.magics[0] = BPTREE_MAGIC1; S->meta.magics[1] = BPTREE_MAGIC2;
        S->meta.magics[2] = BPTREE_MAGIC3; S->meta.magics[3] = BPTREE_MAGIC4;
        S->meta.parent_ptr.entry_count   = 1;
        S->meta.parent_ptr.is_inner_node = 0;
        S->meta.parent_ptr.is_root_node  = 1;
        S->meta.parent_ptr.ptr           = 888;
        fill_extent(&S->extents[0], 100, 20, 500);
        s.dirty();
    }

    /* ── 正向游走：左叶子 idx0 起步，一路 next_gap/++ 到树最右 ── */
    {
        RavenFS_juvenile::extent_iterator it(&fs, leafL_blk, 0);
        extent_t e{};
        RavenFS_juvenile::gap_desicritor_t g{};

        g = it.next_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 20 && g.gap_len == 10,
              "叶内 gap [20,30) | base=%lu len=%lu", g.interval_base_logical_block, g.gap_len);

        ++it;
        CHECK(!it.iterator_erro_flag && it.current_extent(e) && e.fblkbase == 30 && e.length == 10,
              "++ → 左叶子 idx1 = [30,40)");

        g = it.next_gap();   /* 叶子末项跨界 */
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 40 && g.gap_len == 10,
              "跨界 gap [40,50) | base=%lu len=%lu", g.interval_base_logical_block, g.gap_len);

        ++it;   /* 跨到右叶子 */
        CHECK(!it.iterator_erro_flag && it.leaf_blk() == leafR_blk && it.index() == 0 &&
              it.current_extent(e) && e.fblkbase == 50, "++ 跨界 → 右叶子 idx0 = [50,60)");

        g = it.next_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 60 && g.gap_len == 10,
              "叶内 gap [60,70) | base=%lu len=%lu", g.interval_base_logical_block, g.gap_len);

        ++it;
        CHECK(!it.iterator_erro_flag && it.current_extent(e) && e.fblkbase == 70 && e.length == 20,
              "++ → 右叶子 idx1 = [70,90)");

        g = it.next_gap();   /* 链表尾 */
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::tail_infinite && g.interval_base_logical_block == 90,
              "树最右 next_gap → tail_infinite base=90 | type=%d", (int)g.type);

        ++it;   /* 最右末尾再 ++ → erro，位置不变 */
        CHECK(it.iterator_erro_flag && it.leaf_blk() == leafR_blk && it.index() == 1,
              "最右再 ++ → erro，位置保持 (leafR,1)");
    }

    /* ── 反向游走：从右叶子 idx1 一路 prev_gap/-- 回树最左 ── */
    {
        RavenFS_juvenile::extent_iterator it(&fs, leafR_blk, 1);
        extent_t e{};
        RavenFS_juvenile::gap_desicritor_t g{};

        g = it.prev_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 60 && g.gap_len == 10,
              "叶内 prev_gap [60,70) | base=%lu len=%lu", g.interval_base_logical_block, g.gap_len);

        --it;
        CHECK(!it.iterator_erro_flag && it.current_extent(e) && e.fblkbase == 50, "-- → 右叶子 idx0 = [50,60)");

        g = it.prev_gap();   /* 叶子首项跨界 */
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 40 && g.gap_len == 10,
              "跨界 prev_gap [40,50) | base=%lu len=%lu", g.interval_base_logical_block, g.gap_len);

        --it;   /* 跨回左叶子 */
        CHECK(!it.iterator_erro_flag && it.leaf_blk() == leafL_blk && it.index() == 1 &&
              it.current_extent(e) && e.fblkbase == 30, "-- 跨界 → 左叶子 idx1 = [30,40)");

        g = it.prev_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::normal && g.interval_base_logical_block == 20 && g.gap_len == 10,
              "叶内 prev_gap [20,30)");

        --it;
        CHECK(!it.iterator_erro_flag && it.current_extent(e) && e.fblkbase == 10, "-- → 左叶子 idx0 = [10,20)");

        g = it.prev_gap();   /* 链表头 */
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::prev_noexist,
              "树最左 prev_gap → prev_noexist | type=%d", (int)g.type);

        --it;   /* 最左再 -- → erro */
        CHECK(it.iterator_erro_flag && it.leaf_blk() == leafL_blk && it.index() == 0,
              "最左再 -- → erro，位置保持 (leafL,0)");
    }

    /* ── 虚拟前位（GLB=NONE 落点）── */
    {
        RavenFS_juvenile::extent_iterator it(&fs, leafR_blk, 0xFFFF);
        extent_t e{};
        RavenFS_juvenile::gap_desicritor_t g{};

        CHECK(!it.current_extent(e) && !it.iterator_erro_flag,
              "虚拟前位 current_extent → false 且非错误");
        g = it.next_gap();
        CHECK(it.iterator_erro_flag, "虚拟前位 next_gap → erro");

        RavenFS_juvenile::extent_iterator it2(&fs, leafR_blk, 0xFFFF);
        ++it2;   /* 虚拟位 ++ → 首项 */
        CHECK(!it2.iterator_erro_flag && it2.index() == 0 && it2.leaf_blk() == leafR_blk &&
              it2.current_extent(e) && e.fblkbase == 50, "虚拟前位 ++ → 右叶子 idx0 = [50,60)");

        RavenFS_juvenile::extent_iterator it3(&fs, leafR_blk, 0xFFFF);
        --it3;
        CHECK(it3.iterator_erro_flag, "虚拟前位 -- → erro（只允许 ++）");
    }

    /* ── 单叶子根树（is_root=1）：无兄弟边界三态 ── */
    {
        RavenFS_juvenile::extent_iterator it(&fs, solo_blk, 0);
        extent_t e{};
        RavenFS_juvenile::gap_desicritor_t g{};

        g = it.next_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::tail_infinite && g.interval_base_logical_block == 120,
              "单叶子 next_gap → tail_infinite base=120 | type=%d", (int)g.type);

        g = it.prev_gap();
        CHECK(!it.iterator_erro_flag && g.type == RavenFS_juvenile::prev_noexist,
              "单叶子 prev_gap → prev_noexist | type=%d", (int)g.type);

        ++it;
        CHECK(it.iterator_erro_flag && it.index() == 0, "单叶子唯一 extent 再 ++ → erro");

        RavenFS_juvenile::extent_iterator it2(&fs, solo_blk, 0);
        --it2;
        CHECK(it2.iterator_erro_flag, "单叶子 idx0 -- → erro");
    }

    /* ── 后缀 ++/-- 返回旧位置 ── */
    {
        RavenFS_juvenile::extent_iterator it(&fs, leafL_blk, 0);
        RavenFS_juvenile::extent_iterator old = it++;
        CHECK(old.index() == 0 && it.index() == 1, "后缀 ++ 返回旧位置 (0→1)");
        old = it--;
        CHECK(old.index() == 1 && it.index() == 0, "后缀 -- 返回旧位置 (1→0)");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
