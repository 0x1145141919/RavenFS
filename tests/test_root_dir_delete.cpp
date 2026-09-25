/* ═══════════════════════════════════════════════════════════════════
 * test_root_dir_delete.cpp — root_dir_rm_dentry 接口专项测试
 *
 * 范围（只测 delete，插入仅作为造数据手段）：
 *   1. 边界/异常输入：删除不存在条目、40B 超长名、39B 上限、
 *      空名、空目录删除、重复删除、删除后 lookup 确认消失
 *   2. 批量删除：插入 batch 条 → 正向顺序删（左→右，触发右邻居
 *      合并/抹匀）→ 反向删（右→左，触发左邻居合并/抹匀）→
 *      删空收缩（树回到根叶子）
 *   3. 树完整性验证：DFS 校验 (hash,name) 复合序严格递增、
 *      内部 hints hash_max 严格递增且 == 子树最大 hash、
 *      父子指针一致、叶子兄弟链表双向一致、瞬时槽未落盘
 *   4. 持久化：析构（位图全量写回）→ 重挂载 → 树内容不变
 *
 * 只调用 root_dir_insert_dentry / root_dir_rm_dentry（lookup 仅确认
 * 删除效果），树验证用 #define private public 直读内部状态。
 *
 * 用法：ravenfs_juv_delete <device> [batch_count=5000] [del_count=0]
 *   del_count=0 → 全部删除；>0 → 只删前 del_count 条后验证退出
 *   （复播二分模式：定位删除路径上第一个坏点）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* verify 同步点：flush_all 需完整类型 */
#include "../include/ravenfs/mdata_piece_caches.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <set>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

using Clock = std::chrono::steady_clock;

/* 打开设备并填挂载结构体：LBA_size 用 BLKSSZGET 探测（普通文件回退 4096）。
 * fd 生命周期归调用者（类只借不关）；测试末尾统一 ::close。
 * direct=false → mmap 后端（缓存全 offload）；direct=true → 位图全量内存，
 * 缓存按 use_*_cache 开关选 self_reliat/no_cache（编排决策树）。 */
static linux_userspace_mount_t make_mount(const char *path, bool direct,
                                          bool use_inode_cache, bool use_block_cache) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, direct ? O_RDWR | O_DIRECT : O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = direct;
    mnt.param.if_will_use_inode_cache = use_inode_cache;
    mnt.param.if_will_use_mdata_piece_cache = use_block_cache;
    mnt.logical_processor_count = 1;
    return mnt;
}

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

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

/* ═══════════════════════════════════════════════════════════════
 * 树完整性 DFS 验证器（与 insert 测试同源）
 * ═══════════════════════════════════════════════════════════════ */
struct TreeStat {
    bool        ok = true;
    char        err[256] = {0};
    uint64_t    leaf_count = 0, inner_count = 0, entries = 0;
    uint64_t    invalid_inode_ptr = 0;   /* inode_ptr.ptr == PTR_INVALID 条目数 */
    uint64_t    link_breaks = 0;         /* 叶子链表断裂数（独立诊断，不置 ok=false） */
    uint64_t    link_checked = 0;        /* 已检查的 prev/next 边数 */
    std::set<uint64_t> visited;
};

static void verify_fail(TreeStat &st, const char *fmt, ...) {
    if (!st.ok) return;
    st.ok = false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.err, sizeof(st.err), fmt, ap);
    va_end(ap);
}

static uint64_t verify_node(RavenFS_juvenile &fs, uint64_t blk,
                            uint64_t parent_blk, TreeStat &st) {
    if (!st.ok) return 0;
    if (blk == PTR_INVALID) { verify_fail(st, "child ptr PTR_INVALID"); return 0; }
    if (st.visited.count(blk)) { verify_fail(st, "cycle at blk %llu", (unsigned long long)blk); return 0; }
    st.visited.insert(blk);

    alignas(Bptree_dentry_leaf_node_t) uint8_t buf[8192];
    if (fs.read_buff(blk, 2, buf) != RC_OK) { verify_fail(st, "read_buff blk %llu failed", (unsigned long long)blk); return 0; }

    Bptree_meta_t *meta = (Bptree_meta_t *)buf;
    if (!is_dentry_node(meta)) { verify_fail(st, "blk %llu bad magic", (unsigned long long)blk); return 0; }

    uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
    if (cnt > 126) { verify_fail(st, "blk %llu entry_count=%u >126 (瞬时槽落盘?)", (unsigned long long)blk, cnt); return 0; }

    if (parent_blk == PTR_INVALID) {        /* 根 */
        if (!meta->parent_ptr.is_root_node) { verify_fail(st, "root blk %llu is_root_node=0", (unsigned long long)blk); return 0; }
    } else {                                /* 非根 */
        if (meta->parent_ptr.is_root_node) { verify_fail(st, "blk %llu 非根却标 root", (unsigned long long)blk); return 0; }
        if (meta->parent_ptr.ptr != parent_blk) { verify_fail(st, "blk %llu parent_ptr=%llu != 实际父 %llu",
                                                              (unsigned long long)blk,
                                                              (unsigned long long)meta->parent_ptr.ptr,
                                                              (unsigned long long)parent_blk); return 0; }
    }

    if (meta->parent_ptr.is_inner_node) {
        st.inner_count++;
        auto *inner = (Bptree_dentry_inner_node_t *)buf;
        uint64_t prev = 0;
        for (uint16_t i = 0; i < cnt; i++) {
            if (i > 0 && inner->hints[i].hash_max <= prev) {
                verify_fail(st, "blk %llu hints[%u].hash_max=%llu 非严格递增", (unsigned long long)blk, i,
                            (unsigned long long)inner->hints[i].hash_max); return 0;
            }
            prev = inner->hints[i].hash_max;
            uint64_t child_max = verify_node(fs, inner->hints[i].sub_ptr.ptr, blk, st);
            if (st.ok && inner->hints[i].hash_max != child_max) {
                verify_fail(st, "blk %llu hints[%u].hash_max=%llu != 子树最大 %llu",
                            (unsigned long long)blk, i,
                            (unsigned long long)inner->hints[i].hash_max,
                            (unsigned long long)child_max); return 0;
            }
        }
        return cnt ? inner->hints[cnt - 1].hash_max : 0;
    }

    /* ── 叶子 ── */
    st.leaf_count++;
    auto *leaf = (Bptree_dentry_leaf_node_t *)buf;
    if (cnt == 0) {
        if (parent_blk != PTR_INVALID) { verify_fail(st, "非根叶子 blk %llu 为空", (unsigned long long)blk); return 0; }
        return 0;
    }
    uint64_t prev_hash = 0;
    char prev_name[40];
    memset(prev_name, 0, sizeof(prev_name));
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    for (uint16_t i = 0; i < cnt; i++) {
        const Dentry_t &e = leaf->entries[i];
        if (e.inode_ptr.ptr == PTR_INVALID_52) st.invalid_inode_ptr++;
        if (i > 0) {
            bool le = (e.hash < prev_hash) ||
                      (e.hash == prev_hash && memcmp(e.name, prev_name, 40) <= 0);
            if (le) {
                verify_fail(st, "叶子 blk %llu entries[%u] 复合序非严格递增", (unsigned long long)blk, i); return 0;
            }
        }
        prev_hash = e.hash;
        memcpy(prev_name, e.name, 40);
    }
    st.entries += cnt;

    /* 叶子兄弟链表双向一致（根叶子不做链表）——独立诊断 */
    if (parent_blk != PTR_INVALID) {
        const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
        uint64_t prev = leaf->meta.ptrs[1].ptr;
        uint64_t next = leaf->meta.ptrs[2].ptr;
        if (prev == blk) {
            st.link_breaks++;   /* prev 自指 = 根叶子 ".." 残留 */
        } else if (prev != PTR_INVALID_52) {
            st.link_checked++;
            alignas(Bptree_dentry_leaf_node_t) uint8_t pb[8192];
            if (fs.read_buff(prev, 2, pb) != RC_OK) { st.link_breaks++; }
            else {
                auto *pm = (Bptree_meta_t *)pb;
                if (pm->parent_ptr.is_inner_node) { st.link_breaks++; }
                else {
                    auto *pleaf = (Bptree_dentry_leaf_node_t *)pb;
                    if (pleaf->meta.ptrs[2].ptr != blk) st.link_breaks++;
                }
            }
        }
        if (next == blk) {
            st.link_breaks++;
        } else if (next != PTR_INVALID_52) {
            st.link_checked++;
            alignas(Bptree_dentry_leaf_node_t) uint8_t nb[8192];
            if (fs.read_buff(next, 2, nb) != RC_OK) { st.link_breaks++; }
            else {
                auto *nm = (Bptree_meta_t *)nb;
                if (nm->parent_ptr.is_inner_node) { st.link_breaks++; }
                else {
                    auto *nleaf = (Bptree_dentry_leaf_node_t *)nb;
                    if (nleaf->meta.ptrs[1].ptr != blk) st.link_breaks++;
                }
            }
        }
    }
    return leaf->entries[cnt - 1].hash;
}

static void verify_tree(RavenFS_juvenile &fs, uint64_t expect_entries, const char *tag) {
    /* self_reliat 是写回缓存：session 修改未落盘前直读盘会假阳性。
     * 同步点 flush_all（脏写回驻留 + 非脏丢弃；offload=msync / nocache=无操作，均无害）。
     */
    if (fs.inode_cache_mgr) fs.inode_cache_mgr->flush_all();
    if (fs.mdata_piece_mgr) fs.mdata_piece_mgr->flush_all();
    TreeStat st;
    uint64_t root_max = verify_node(fs, fs.root_dentry_blk, PTR_INVALID, st);
    (void)root_max;
    printf("=== verify_tree(%s): %s ===\n", tag, st.ok ? "OK" : "FAIL");
    if (st.ok) {
        printf("  leaf=%llu  inner=%llu  entries=%llu  (expect %llu)  inode_ptr_invalid=%llu\n"
               "  链表诊断: 检查 %llu 边, 断裂 %llu 处%s\n",
               (unsigned long long)st.leaf_count,
               (unsigned long long)st.inner_count,
               (unsigned long long)st.entries,
               (unsigned long long)expect_entries,
               (unsigned long long)st.invalid_inode_ptr,
               (unsigned long long)st.link_checked,
               (unsigned long long)st.link_breaks,
               st.link_breaks ? "  ⚠ 链表断裂" : "");
    } else {
        printf("  %s\n", st.err);
    }
    CHECK(st.ok, "树结构完整 (%s)", tag);
    if (st.ok) CHECK(st.entries == expect_entries, "条目总数 %llu == %llu (%s)",
                     (unsigned long long)st.entries, (unsigned long long)expect_entries, tag);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [batch_count=5000] [del_count=0] [mode=offload|nocache|selfreliat]\n", argv[0]); return 2; }
    int batch_n = 5000;
    if (argc >= 3) batch_n = atoi(argv[2]);
    if (batch_n < 1) batch_n = 1;
    int del_count = 0;   /* 0 = 删全部；>0 = 复播二分，只删前 del_count 条 */
    if (argc >= 4) del_count = atoi(argv[3]);
    if (del_count < 0) del_count = 0;
    /* 后端模式：offload（非直写 → mmap 全 offload）/ nocache（直写 → 双 no_cache）/
     * selfreliat（直写 → 双 self_reliat 用户态缓存） */
    bool mode_direct = false, mode_use_icache = false, mode_use_bcache = false;
    if (argc >= 5 && strcmp(argv[4], "nocache") == 0) {
        mode_direct = true;   /* 直写 + 双 no_cache */
    } else if (argc >= 5 && strcmp(argv[4], "selfreliat") == 0) {
        mode_direct = true;
        mode_use_icache = true;   /* 直写 + 双 self_reliat（编排决策树落 self_reliat 后端） */
        mode_use_bcache = true;
    } else {
        mode_direct = false;  /* 非直写 → mmap 后端，缓存全 offload（开关被忽略） */
    }

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.1 root_dir_rm_dentry 专项测试\n"
           " device: %s   batch: %d   del: %s   mode: %s\n"
           "════════════════════════════════════════════════\n",
           argv[1], batch_n,
           del_count > 0 ? "bisect（只删前若干条）" : "全部删除",
           mode_direct ? (mode_use_icache ? "selfreliat（直写+双self_reliat）"
                                          : "nocache（直写+双no_cache）")
                       : "offload（mmap后端）");

    uint64_t rc = 0;
    uint64_t inserted = 0;

    linux_userspace_mount_t mnt = make_mount(argv[1], mode_direct, mode_use_icache, mode_use_bcache);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
    RavenFS_juvenile fs(mnt, &rc);
    CHECK(fs.is_mounted(), "mount rc=%s", rc_name(rc));
    if (!fs.is_mounted()) {
        printf("挂载失败，测试中止（先确认已 mkfs）\n");
        return 1;
    }
    fs.dump_info();

    /* ══ Phase 1：边界 / 异常输入 ══ */
    printf("\n--- Phase 1: 边界与异常 ---\n");
    char long39[40], long40[41], long41[42];
    memset(long39, 'x', 39); long39[39] = 0;
    memset(long40, 'y', 40); long40[40] = 0;
    memset(long41, 'z', 41); long41[41] = 0;

    /* 1a. 空目录上删：RC_ERR_NOT_FOUND */
    {
        result_code_t r = fs.root_dir_rm_dentry("ghost", false);
        printf("  [%s] rm(\"ghost\") 空目录 → %s（expect RC_ERR_NOT_FOUND）\n",
               r == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r));
        r == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
    }

    /* 1b. 先插一条再删的常规路径 */
    {
        result_code_t ri = fs.root_dir_insert_dentry("alpha", INODE_TYPE_REGULAR);
        CHECK(ri == RC_OK, "insert(\"alpha\") → %s", rc_name(ri));
        if (ri == RC_OK) inserted++;
        result_code_t r = fs.root_dir_rm_dentry("alpha", false);
        bool ok = (r == RC_OK);
        printf("  [%s] rm(\"alpha\") → %s（expect RC_OK）\n", ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
        if (ok) inserted--;
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup("alpha", &inum);
        printf("  [%s] lookup(\"alpha\") → %s（expect RC_ERR_NOT_FOUND）\n",
               lr == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(lr));
        lr == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
        /* 再删一次：已消失 → NOT_FOUND */
        result_code_t r2 = fs.root_dir_rm_dentry("alpha", false);
        printf("  [%s] rm(\"alpha\") 二次 → %s（expect RC_ERR_NOT_FOUND）\n",
               r2 == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r2));
        r2 == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
    }

    /* 1c. 超长名（长度检查在 walk 之前）：>552B 才 NAME_LEN，40/41B 已合法 */
    {
        char long553[554];
        memset(long553, 'v', 553); long553[553] = 0;
        result_code_t r = fs.root_dir_rm_dentry(long553, false);
        printf("  [%s] rm(553B) → %s（expect RC_ERR_NAME_LEN）\n",
               r == RC_ERR_NAME_LEN ? "PASS" : "FAIL", rc_name(r));
        r == RC_ERR_NAME_LEN ? g_pass++ : g_fail++;
        result_code_t r2 = fs.root_dir_rm_dentry(long40, false);   /* 合法但不存在 → NOT_FOUND */
        printf("  [%s] rm(40B) → %s（expect RC_ERR_NOT_FOUND，合法长度不存在）\n",
               r2 == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r2));
        r2 == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
        result_code_t r3 = fs.root_dir_rm_dentry(long41, false);
        printf("  [%s] rm(41B) → %s（expect RC_ERR_NOT_FOUND）\n",
               r3 == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r3));
        r3 == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
    }

    /* 1d. 39B 上限名：插→删→再删 */
    {
        result_code_t ri = fs.root_dir_insert_dentry(long39, INODE_TYPE_REGULAR);
        CHECK(ri == RC_OK, "insert(39B) → %s", rc_name(ri));
        if (ri == RC_OK) inserted++;
        result_code_t r = fs.root_dir_rm_dentry(long39, false);
        bool ok = (r == RC_OK);
        printf("  [%s] rm(39B) → %s（expect RC_OK）\n", ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
        if (ok) inserted--;
    }

    /* 1e. 空名：不插则 NOT_FOUND（与 insert 测试对称，insert 空名合法） */
    {
        result_code_t r = fs.root_dir_rm_dentry("", false);
        printf("  [%s] rm(\"\") 未插空名 → %s（expect RC_ERR_NOT_FOUND）\n",
               r == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r));
        r == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
    }

    verify_tree(fs, inserted, "Phase 1 后");

    /* ══ Phase 2：批量插入 → 批量删除 ══ */
    printf("\n--- Phase 2: 插入 %d 条造数据 ---\n", batch_n);
    char nb[64];
    bool batch_ok = true;
    const auto t_insert0 = Clock::now();
    int progress = batch_n >= 1000 ? batch_n / 8 : 1;
    for (int i = 0; i < batch_n; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_insert_dentry(nb, INODE_TYPE_REGULAR);
        if (r != RC_OK) { batch_ok = false; printf("  insert f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted++;
        if (progress >= 1 && (i + 1) % progress == 0)
            printf("  ... %d/%d 已插入（累计 %llu）\n", i + 1, batch_n, (unsigned long long)inserted);
    }
    const auto t_insert1 = Clock::now();
    const double insert_sec = std::chrono::duration<double>(t_insert1 - t_insert0).count();
    CHECK(batch_ok, "批量插入 %d 全成功", batch_n);
    printf("  ⏱ 插入阶段: %.3f s（%.0f ops/s）\n", insert_sec, insert_sec > 0 ? batch_n / insert_sec : 0);
    fs.dump_info();
    verify_tree(fs, inserted, "批量插入后");

    /* ── 删除阶段 ── */
    int del_target = (del_count > 0) ? del_count : batch_n;
    if (del_target > batch_n) del_target = batch_n;
    /* 默认模式：正向删前半、反向删后半，左右邻居的合并/抹匀都覆盖；
     * 单方向删光则树直接收缩回根叶子（收缩路径同样被验证）。 */
    int fwd_target = (del_count > 0) ? del_target : batch_n / 2;
    if (fwd_target < 0) fwd_target = 0;

    /* 2a. 正向删（左→右）：欠载时看右邻居 */
    printf("\n--- Phase 2a: 正向删除 %d 条（左→右） ---\n", fwd_target);
    const auto t_fwd0 = Clock::now();
    progress = fwd_target >= 1000 ? fwd_target / 8 : 1;
    bool del_ok = true;
    int del_done = 0;
    for (int i = 0; i < fwd_target; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_rm_dentry(nb, false);
        if (r != RC_OK) { del_ok = false; printf("  rm f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted--;
        del_done++;
        if (progress >= 1 && (del_done) % progress == 0)
            printf("  ... %d/%d 已删（剩余 %llu）\n", del_done, fwd_target, (unsigned long long)inserted);
    }
    const auto t_fwd1 = Clock::now();
    const double fwd_sec = std::chrono::duration<double>(t_fwd1 - t_fwd0).count();
    CHECK(del_ok, "正向删除 %d 全成功", del_done);
    printf("  ⏱ 正向删: %.3f s（%.0f ops/s）\n", fwd_sec, fwd_sec > 0 ? del_done / fwd_sec : 0);
    verify_tree(fs, inserted, "正向删除后");

    /* 复播二分模式：到这里就够 */
    if (del_count > 0) {
        printf("\n════════════════════════════════════════════════\n"
               " SUMMARY(bisect del=%d): %d passed, %d failed\n"
               "════════════════════════════════════════════════\n",
               del_done, g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    /* 2b. 反向删（右→左）：欠载时看左邻居；删空收缩 */
    printf("\n--- Phase 2b: 反向删除 %d 条（右→左） ---\n", batch_n - del_done);
    int rev_target = batch_n - del_done;
    const auto t_rev0 = Clock::now();
    progress = rev_target >= 1000 ? rev_target / 8 : 1;
    bool rev_ok = true;
    int rev_done = 0;
    for (int i = batch_n - 1; i >= del_done; i--) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_rm_dentry(nb, false);
        if (r != RC_OK) { rev_ok = false; printf("  rm f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted--;
        rev_done++;
        if (progress >= 1 && rev_done % progress == 0)
            printf("  ... %d/%d 已删（剩余 %llu）\n", rev_done, rev_target, (unsigned long long)inserted);
    }
    const auto t_rev1 = Clock::now();
    const double rev_sec = std::chrono::duration<double>(t_rev1 - t_rev0).count();
    CHECK(rev_ok, "反向删除 %d 全成功", rev_done);
    printf("  ⏱ 反向删: %.3f s（%.0f ops/s）\n", rev_sec, rev_sec > 0 ? rev_done / rev_sec : 0);
    verify_tree(fs, inserted, "反向删除后");
    printf("  删空后剩余条目: %llu\n", (unsigned long long)inserted);
    CHECK(inserted == 0, "全部删空（剩余 0）");
    fs.dump_info();
    CHECK(audit_clean(fs, "delete-empty") == 0, "audit_inode 全净 (delete 删空)");

    /* 2c. 删空后再删 → NOT_FOUND */
    {
        result_code_t r = fs.root_dir_rm_dentry("f00000", false);
        printf("  [%s] rm(\"f00000\") 删空后 → %s（expect RC_ERR_NOT_FOUND）\n",
               r == RC_ERR_NOT_FOUND ? "PASS" : "FAIL", rc_name(r));
        r == RC_ERR_NOT_FOUND ? g_pass++ : g_fail++;
    }
    }   /* ← fs 析构：位图全量写回 */

    /* ══ Phase 3：持久化（析构 flush → 重挂载） ══ */
    printf("\n--- Phase 3: 持久化验证（重挂载） ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            fs2.dump_info();
            verify_tree(fs2, inserted, "重挂载后");
            /* 删空后树应收缩回根叶子：leaf=1 inner=0 */
            TreeStat st;
            verify_node(fs2, fs2.root_dentry_blk, PTR_INVALID, st);
            if (st.ok) {
                CHECK(st.leaf_count == 1 && st.inner_count == 0,
                      "删空收缩: leaf=%llu inner=%llu (expect 1/0)",
                      (unsigned long long)st.leaf_count,
                      (unsigned long long)st.inner_count);
            }
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    printf("注：insert 已分配真实 inode，rm 时 inode_free 释放 inode 槽；\n"
           "    删除路径同时验证 inode 位图随插删增减。\n");
    return g_fail ? 1 : 0;
}
