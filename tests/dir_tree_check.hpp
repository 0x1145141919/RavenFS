#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * dir_tree_check.hpp — 目录顶层测试（insert/delete）共享装置（v1.3）
 *
 * 收录两测试共通部分：挂载构造、结果码名、CHECK 计数、dentry 树 DFS 校验器。
 * 目标：消灭 test_root_dir_insert/delete 之间重复的 ~150 行校验器，并让
 * v1.1→v1.2 掩码迁移只在一处发生（避免两文件再次分叉漂移）。
 *
 * 包含顺序约定：调用方先
 *     #define private public
 *     #include "../include/ravenfs/ravenfs_v1.1.hpp"
 *     #undef private
 * 再 include 本头。本头在 private 重定义之外拉缓存/审计完整类型
 * （inode_cache_radix.hpp / mdata_piece_caches.hpp / audit_check.hpp）。
 *
 * 2026-10-05 v1.3 迁移：位域成员访问（parent_ptr.entry_count / .is_root_node /
 * .is_inner_node / .ptr、sub_ptr.ptr、ptrs[j].ptr、inode_ptr.ptr）→ 掩码访问器
 * （RFS_PP_COUNT / RFS_PP_IS_ROOT / RFS_PP_IS_INNER / RFS_PP_PTR / RFS_PTR_ADDR）；
 * Dentry_t → ddentry_t。
 *
 * 校验不变量（对齐 disk_layout v1.1 §10）：
 *   · 叶内 (hash,name) 复合序严格递增；
 *   · 内部 hints hash_max 严格递增且 == 子树最大 hash；
 *   · 根/非根标志与 parent_ptr 一致；
 *   · 非根叶兄弟链双向自洽（根叶不做链）。
 * 注意：复合序比较只读内联 40B（不读名字数据面尾部）；测试用长名前缀互异，
 * 不触发「同 hash 且前 40B 相同」的歧义，故无需 owner 目录上下文。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/inode_cache_radix.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#include "audit_check.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* ── 计数 + CHECK（两测试共用一份，避免各自定义分叉） ── */
inline int g_pass = 0, g_fail = 0;
#ifndef CHECK
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)
#endif

/* ── 打开设备并填挂载结构体 ──
 * LBA_size 用 BLKSSZGET 探测（普通文件回退 4096）。fd 生命周期归调用者。
 * direct=false → mmap 后端（缓存全 offload；可达性审计前提）；
 * direct=true  → 按 use_*_cache 选 self_reliat / no_cache。 */
inline linux_userspace_mount_t make_mount(const char *path, bool direct = false,
                                          bool use_inode_cache = false,
                                          bool use_block_cache = false)
{
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, direct ? O_RDWR | O_DIRECT : O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = direct;
    mnt.param = {};
    mnt.param.if_will_use_inode_cache = use_inode_cache;
    mnt.param.if_will_use_mdata_piece_cache = use_block_cache;
    mnt.logical_processor_count = 1;
    return mnt;
}

inline const char *rc_name(result_code_t rc)
{
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
    case RC_ERR_NOT_EMPTY:  return "RC_ERR_NOT_EMPTY";
    case RC_ERR_NOT_SUPPORTED: return "RC_ERR_NOT_SUPPORTED";
    case RC_ERR_NAME_INVALID:  return "RC_ERR_NAME_INVALID";
    default:                return "RC_???";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * dentry 树完整性 DFS 校验器
 *
 * dir_verify_node 返回子树最大 hash；破坏不变量时 st.ok=false 并记录首错。
 * 依赖调用方以 `#define private public` 引入的 8192B 节点类型与 read_buff。
 * ═══════════════════════════════════════════════════════════════════ */
struct DirTreeStat {
    bool        ok = true;
    char        err[256] = {0};
    uint64_t    leaf_count = 0, inner_count = 0, entries = 0;
    uint64_t    invalid_inode_ptr = 0;   /* inode_ptr.addr == PTR_INVALID(52) 条目数 */
    uint64_t    link_breaks = 0;         /* 叶子链表断裂数（独立诊断，不置 ok=false） */
    uint64_t    link_checked = 0;        /* 已检查的 prev/next 边数 */
    std::set<uint64_t> visited;
};

inline void dir_verify_fail(DirTreeStat &st, const char *fmt, ...)
{
    if (!st.ok) return;
    st.ok = false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.err, sizeof(st.err), fmt, ap);
    va_end(ap);
}

inline uint64_t dir_verify_node(RavenFS_juvenile &fs, uint64_t blk,
                                uint64_t parent_blk, DirTreeStat &st)
{
    if (!st.ok) return 0;
    if (blk == PTR_INVALID) { dir_verify_fail(st, "child ptr PTR_INVALID"); return 0; }
    if (st.visited.count(blk)) { dir_verify_fail(st, "cycle at blk %llu", (unsigned long long)blk); return 0; }
    st.visited.insert(blk);

    alignas(Bptree_ddentry_leaf_node_t) uint8_t buf[8192];
    if (fs.read_buff(blk, 2, buf) != RC_OK) { dir_verify_fail(st, "read_buff blk %llu failed", (unsigned long long)blk); return 0; }

    Bptree_meta_t *meta = (Bptree_meta_t *)buf;
    if (!is_dentry_node(meta)) { dir_verify_fail(st, "blk %llu bad magic", (unsigned long long)blk); return 0; }

    uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    if (cnt > 126) { dir_verify_fail(st, "blk %llu entry_count=%u >126 (瞬时槽落盘?)", (unsigned long long)blk, cnt); return 0; }

    if (parent_blk == PTR_INVALID) {        /* 根 */
        if (!RFS_PP_IS_ROOT(meta->parent_ptr)) { dir_verify_fail(st, "root blk %llu is_root_node=0", (unsigned long long)blk); return 0; }
    } else {                                /* 非根 */
        if (RFS_PP_IS_ROOT(meta->parent_ptr)) { dir_verify_fail(st, "blk %llu 非根却标 root", (unsigned long long)blk); return 0; }
        if (RFS_PP_PTR(meta->parent_ptr) != parent_blk) { dir_verify_fail(st, "blk %llu parent_ptr=%llu != 实际父 %llu",
                                                              (unsigned long long)blk,
                                                              (unsigned long long)RFS_PP_PTR(meta->parent_ptr),
                                                              (unsigned long long)parent_blk); return 0; }
    }

    if (RFS_PP_IS_INNER(meta->parent_ptr)) {
        st.inner_count++;
        auto *inner = (Bptree_ddentry_inner_node_t *)buf;
        uint64_t prev = 0;
        for (uint16_t i = 0; i < cnt; i++) {
            if (i > 0 && inner->hints[i].hash_max <= prev) {
                dir_verify_fail(st, "blk %llu hints[%u].hash_max=%llu 非严格递增", (unsigned long long)blk, i,
                            (unsigned long long)inner->hints[i].hash_max); return 0;
            }
            prev = inner->hints[i].hash_max;
            uint64_t child_max = dir_verify_node(fs, RFS_PTR_ADDR(inner->hints[i].sub_ptr), blk, st);
            if (st.ok && inner->hints[i].hash_max != child_max) {
                dir_verify_fail(st, "blk %llu hints[%u].hash_max=%llu != 子树最大 %llu",
                            (unsigned long long)blk, i,
                            (unsigned long long)inner->hints[i].hash_max,
                            (unsigned long long)child_max); return 0;
            }
        }
        return cnt ? inner->hints[cnt - 1].hash_max : 0;
    }

    /* ── 叶子 ── */
    st.leaf_count++;
    auto *leaf = (Bptree_ddentry_leaf_node_t *)buf;
    if (cnt == 0) {
        if (parent_blk != PTR_INVALID) { dir_verify_fail(st, "非根叶子 blk %llu 为空", (unsigned long long)blk); return 0; }
        return 0;
    }
    uint64_t prev_hash = 0;
    char prev_name[DENTRY_INLINE_NAME_MAX];
    memset(prev_name, 0, sizeof(prev_name));
    /* addr 域 52 位；比较须同样截断（64 位 PTR_INVALID 恒不相等） */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & RFS_MASK52;
    for (uint16_t i = 0; i < cnt; i++) {
        const ddentry_t &e = leaf->entries[i];
        if (RFS_PTR_ADDR(e.inode_ptr) == PTR_INVALID_52) st.invalid_inode_ptr++;
        if (i > 0) {
            bool le = (e.hash < prev_hash) ||
                      (e.hash == prev_hash && memcmp(e.name, prev_name, DENTRY_INLINE_NAME_MAX) <= 0);
            if (le) {
                dir_verify_fail(st, "叶子 blk %llu entries[%u] 复合序非严格递增", (unsigned long long)blk, i); return 0;
            }
        }
        prev_hash = e.hash;
        memcpy(prev_name, e.name, DENTRY_INLINE_NAME_MAX);
    }
    st.entries += cnt;

    /* 叶子兄弟链表双向一致（根叶子不做链表）——独立诊断，不置 ok=false */
    if (parent_blk != PTR_INVALID) {
        uint64_t prev = RFS_PTR_ADDR(leaf->meta.ptrs[1]);
        uint64_t next = RFS_PTR_ADDR(leaf->meta.ptrs[2]);
        if (prev == blk) {
            st.link_breaks++;   /* prev 自指 = 根叶子 ".." 残留 */
        } else if (prev != PTR_INVALID_52) {
            st.link_checked++;
            alignas(Bptree_ddentry_leaf_node_t) uint8_t pb[8192];
            if (fs.read_buff(prev, 2, pb) != RC_OK) { st.link_breaks++; }
            else {
                auto *pm = (Bptree_meta_t *)pb;
                if (RFS_PP_IS_INNER(pm->parent_ptr)) { st.link_breaks++; }
                else {
                    auto *pleaf = (Bptree_ddentry_leaf_node_t *)pb;
                    if (RFS_PTR_ADDR(pleaf->meta.ptrs[2]) != blk) st.link_breaks++;
                }
            }
        }
        if (next == blk) {
            st.link_breaks++;
        } else if (next != PTR_INVALID_52) {
            st.link_checked++;
            alignas(Bptree_ddentry_leaf_node_t) uint8_t nb[8192];
            if (fs.read_buff(next, 2, nb) != RC_OK) { st.link_breaks++; }
            else {
                auto *nm = (Bptree_meta_t *)nb;
                if (RFS_PP_IS_INNER(nm->parent_ptr)) { st.link_breaks++; }
                else {
                    auto *nleaf = (Bptree_ddentry_leaf_node_t *)nb;
                    if (RFS_PTR_ADDR(nleaf->meta.ptrs[1]) != blk) st.link_breaks++;
                }
            }
        }
    }
    return leaf->entries[cnt - 1].hash;
}

/* self_reliat 是写回缓存：session 修改未落盘前直读盘会假阳性。
 * 同步点 flush_all（脏写回驻留 + 非脏丢弃；offload=msync / nocache=无操作，均无害）。 */
inline void dir_flush_caches(RavenFS_juvenile &fs)
{
    if (fs.inode_cache_mgr) fs.inode_cache_mgr->flush_all();
    if (fs.mdata_piece_mgr) fs.mdata_piece_mgr->flush_all();
}

/* 对 root 目录树做一次完整校验 + 条目数对账（内部 CHECK）。
 * 返回 DirTreeStat 供调用方追加检查（如 leaf/inner 计数）。 */
inline DirTreeStat dir_verify_tree(RavenFS_juvenile &fs, uint64_t expect_entries, const char *tag)
{
    dir_flush_caches(fs);
    DirTreeStat st;
    uint64_t root_max = dir_verify_node(fs, fs.root_dentry_blk, PTR_INVALID, st);
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
    return st;
}

/* ── 长名生成器（41..RAVENFS_MAX_NAME_LEN，前缀互异、片内/跨片/多 tail 覆盖） ── */
inline void dir_make_long_name(char *out, int idx)
{
    static const char seed[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int len = 41 + (idx * 7) % 512;   /* 41,48,...,552 循环 */
    for (int j = 0; j < len; j++) out[j] = seed[(idx + j * 3) % 62];
    out[len] = '\0';
}

/* ── 唯一长名生成器（大规范压测用；idx 全域不碰撞）──
 * dir_make_long_name 的名字由 (idx mod 62, idx mod 512) 决定，按 CRT 周期
 * lcm(62,512)=15872 循环——1M 级会撞名（重复插入 → EXIST）。本生成器把 idx
 * 以 base62 定宽（8 字节）写进名字最前 8B 作为唯一锚，其后用确定 pattern
 * 填到 len；长度 41..552 循环。熵足够覆盖 >62^8 个 idx，且前缀唯一。
 * 生成的名字只含 [0-9A-Za-z]，天然不含 '/'。 */
inline void dir_make_unique_long_name(char *out, uint64_t idx)
{
    static const char seed[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const int len = 41 + (int)((idx * 7) % 512);   /* 41..552 */
    uint64_t v = idx;
    for (int j = 7; j >= 0; --j) { out[j] = seed[v % 62]; v /= 62; }   /* 唯一锚 */
    for (int j = 8; j < len; j++) out[j] = seed[(idx + (uint64_t)j * 3) % 62];
    out[len] = '\0';
}
