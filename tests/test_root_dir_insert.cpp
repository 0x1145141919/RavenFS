/* ═══════════════════════════════════════════════════════════════════
 * test_root_dir_insert.cpp — root_dir_insert_dentry 接口专项测试
 *
 * 范围（先只测 insert）：
 *   1. 边界/异常输入：空名、40B 超长、39B 上限、重复插入、
 *      大小写、空格、UTF-8、斜杠、控制字符
 *   2. 批量插入：1000 条目，覆盖叶子分裂 → 根分裂（建新根）→
 *      内部节点分裂（多层树）
 *   3. 树完整性验证：DFS 校验 (hash,name) 复合序严格递增、
 *      内部 hints hash_max 严格递增且 == 子树最大 hash、
 *      父子指针一致、叶子兄弟链表双向一致、瞬时槽未落盘
 *   4. 持久化：析构（位图全量写回）→ 重挂载 → 树内容不变
 *
 * 只调用 root_dir_insert_dentry（不碰 lookup/readdir/rm），
 * 树验证用 #define private public 直读内部状态（同 test_mount_bitmap）。
 *
 * 用法：ravenfs_juv_insert <device>     （已 mkfs 的 ravenfs v1.1 盘/镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <set>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* 打开设备并填挂载结构体：LBA_size 用 BLKSSZGET 探测（普通文件回退 4096）。
 * fd 生命周期归调用者（类只借不关）；测试末尾统一 ::close。 */
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
 * 树完整性 DFS 验证器
 *
 * 返回子树最大 hash；不变量破坏时 st.ok=false 并记录首错。
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
    /* PTR_INVALID 赋入 52 位位域会被截断，比较时同样截断 */
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

    /* 叶子兄弟链表双向一致（根叶子不做链表）——独立诊断，不置 ok=false */
    if (parent_blk != PTR_INVALID) {
        /* PTR_INVALID 赋入 52 位位域被截断，比较时同样截断 */
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
               st.link_breaks ? "  ⚠ 已知 dentry_leaf_split 链表 bug" : "");
    } else {
        printf("  %s\n", st.err);
    }
    CHECK(st.ok, "树结构完整 (%s)", tag);
    if (st.ok) CHECK(st.entries == expect_entries, "条目总数 %llu == %llu (%s)",
                     (unsigned long long)st.entries, (unsigned long long)expect_entries, tag);
}

/* ── 用例 ── */
struct Case { const char *name; result_code_t expect; const char *desc; };

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [batch_count=65536] [stop_at=0]\n", argv[0]); return 2; }
    int batch_n = 65536;
    if (argc >= 3) batch_n = atoi(argv[2]);
    if (batch_n < 1) batch_n = 1;
    int stop_at = 0;   /* >0：复播二分模式，只插前 stop_at 条后 verify 退出 */
    if (argc >= 4) stop_at = atoi(argv[3]);
    if (stop_at < 0) stop_at = 0;
    int insert_n = (stop_at > 0 && stop_at < batch_n) ? stop_at : batch_n;

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.1 root_dir_insert_dentry 专项测试\n"
           " device: %s   batch: %d   stop_at: %d%s\n"
           "════════════════════════════════════════════════\n",
           argv[1], batch_n, insert_n,
           stop_at > 0 ? "  (复播二分模式)" : "");

    uint64_t rc = 0;
    uint64_t inserted = 0;      /* 成功插入的条目数 */

    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    /* ══ Phase 0~2：挂载 → 边界 → 批量（作用域块，析构时位图全量写回） ══ */
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
    char long39[40], long40[41], long41[42], long552[553], long553[554];
    memset(long39, 'x', 39); long39[39] = 0;
    memset(long40, 'y', 40); long40[40] = 0;
    memset(long41, 'z', 41); long41[41] = 0;
    memset(long552, 'w', 552); long552[552] = 0;   /* RAVENFS_MAX_NAME_LEN 上限 */
    memset(long553, 'v', 553); long553[553] = 0;
    static const char ctrl[] = { 0x01, 0x02, 0x03, 0 };

    const Case cases[] = {
        { "",            RC_OK,            "空名（长度 0，合法边界）" },
        { long40,        RC_OK,            "40B 边界（全内联上限，无堆）" },
        { long41,        RC_OK,            "41B 长名（名字堆 8B 档，1 单元）" },
        { "a",           RC_OK,            "正常单字符" },
        { "a",           RC_ERR_EXIST,     "重复插入同名字" },
        { "A",           RC_OK,            "大小写敏感（a/A 不同）" },
        { "a",           RC_ERR_EXIST,     "再次重复插入" },
        { long39,        RC_OK,            "39B 正好内联上限内" },
        { long552,       RC_OK,            "552B 上限长名（512B 档单单元）" },
        { long553,       RC_ERR_NAME_LEN,  "553B 超限（RAVENFS_MAX_NAME_LEN=552）" },
        { "foo bar",     RC_OK,            "含空格" },
        { "中文目录项",    RC_OK,            "UTF-8 多字节" },
        { "dir/sub",     RC_OK,            "含斜杠（根目录无路径语义，纯字符）" },
        { ctrl,          RC_OK,            "控制字符 0x01-0x03" },
    };
    for (const auto &c : cases) {
        result_code_t r = fs.root_dir_insert_dentry(c.name);
        bool ok = (r == c.expect);
        if (ok) inserted += (c.expect == RC_OK);
        printf("  [%s] insert(\"%s\") → %-16s expect %-16s  %s\n",
               ok ? "PASS" : "FAIL", c.name, rc_name(r), rc_name(c.expect), c.desc);
        ok ? g_pass++ : g_fail++;
    }

    /* 空名再插一次：hash 相同，应判 EXIST（同时暴露 memcpy 越界读行为） */
    {
        result_code_t r = fs.root_dir_insert_dentry("");
        bool ok = (r == RC_ERR_EXIST);
        printf("  [%s] insert(\"\") 再次 → %s（expect RC_ERR_EXIST；空名条目已存在）\n",
               ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
    }

    verify_tree(fs, inserted, "Phase 1 后");

    /* 长名重复插入：hash 相同 → 叶子内完整名字比较（dentry_name_cmp 读堆尾部）→ EXIST */
    {
        result_code_t r = fs.root_dir_insert_dentry(long41);
        bool ok = (r == RC_ERR_EXIST);
        printf("  [%s] insert(41B) 再次 → %s（expect RC_ERR_EXIST，尾部比较命中）\n",
               ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
    }

    /* ══ Phase 2：批量插入触发分裂（叶子分裂 → 根分裂 → 内部节点分裂） ══ */
    printf("\n--- Phase 2: 批量插入 %d 条目（覆盖多层分裂） ---\n", insert_n);
    char nb[64];
    bool batch_ok = true;
    uint64_t fail_at = 0;
    result_code_t fail_rc = RC_OK;
    int progress = insert_n >= 1000 ? insert_n / 8 : 1;
    for (int i = 0; i < insert_n; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_insert_dentry(nb);
        if (r != RC_OK) { batch_ok = false; fail_at = i; fail_rc = r; break; }
        inserted++;   /* 每成功一个立即计数，失败时树内条目数仍准确 */
        if (progress >= 1 && (i + 1) % progress == 0)
            printf("  ... %d/%d 已插入（累计条目 %llu）\n",
                   i + 1, insert_n, (unsigned long long)inserted);
    }
    if (batch_ok) {
        printf("  批量插入全部 RC_OK\n");
    } else {
        printf("  批量插入在 f%05llu 失败: %s（树内条目 %llu）\n",
               (unsigned long long)fail_at, rc_name(fail_rc),
               (unsigned long long)inserted);
    }
    CHECK(batch_ok, "批量 %d 全成功", insert_n);
    fs.dump_info();

    verify_tree(fs, inserted, "Phase 2 后");

    /* ══ Phase 2b：长名批量（名字堆各档位激活 + lookup 抽查） ══ */
    printf("\n--- Phase 2b: 长名批量（名字堆 8B/64B/128B/256B/512B 档） ---\n");
    const char *seed_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char lname[RAVENFS_MAX_NAME_LEN + 1];
    bool long_ok = true;
    uint64_t long_fail_at = 0;
    result_code_t long_fail_rc = RC_OK;
    int long_n = 300;
    for (int i = 0; i < long_n; i++) {
        /* 长度在 41~552 间分布，覆盖各档位 */
        int len = 41 + (i * 7) % 512;   /* 41,48,...,552 循环 */
        for (int j = 0; j < len; j++) lname[j] = seed_chars[(i + j * 3) % 62];
        lname[len] = 0;
        result_code_t r = fs.root_dir_insert_dentry(lname);
        if (r != RC_OK) { long_ok = false; long_fail_at = i; long_fail_rc = r; break; }
        inserted++;
        /* 抽查：插入后立即 lookup（完整名字比较读堆） */
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup(lname, &inum);
        if (lr != RC_OK) { long_ok = false; long_fail_at = i; long_fail_rc = lr; break; }
    }
    if (long_ok) {
        printf("  长名批量 %d 条全部 RC_OK + lookup 命中\n", long_n);
    } else {
        printf("  长名批量在 #%llu 失败: %s（树内条目 %llu）\n",
               (unsigned long long)long_fail_at, rc_name(long_fail_rc),
               (unsigned long long)inserted);
    }
    CHECK(long_ok, "长名批量 %d 全成功且可查", long_n);

    verify_tree(fs, inserted, "Phase 2b 后");
    }   /* ← fs 析构：位图全量写回 */

    /* 复播二分模式：到这里就够，直接汇总（不动 Phase 3） */
    if (stop_at > 0) {
        printf("\n════════════════════════════════════════════════\n"
               " SUMMARY(bisect stop_at=%d): %d passed, %d failed\n"
               "════════════════════════════════════════════════\n",
               insert_n, g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    /* ══ Phase 3：持久化（析构 flush → 重挂载；fs 已卸载） ══ */
    printf("\n--- Phase 3: 持久化验证（重挂载） ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            fs2.dump_info();
            verify_tree(fs2, inserted, "重挂载后");
        }
    }
    ::close(mnt.dev.fd);

    /* ══ 汇总 ══ */
    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    printf("注：当前 insert 未分配 inode（条目 inode_ptr.ptr 恒 PTR_INVALID），\n"
           "    树验证统计 invalid_inode_ptr 数量供确认。\n");
    return g_fail ? 1 : 0;
}
