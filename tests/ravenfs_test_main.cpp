#include "ravenfs_god.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

/* ═══════════════════════════════════════════════════════════════
 * RavenFS 上帝类 — Phase 1 测试
 *
 * 用法:
 *   ./ravenfs_test <device>
 *
 * 前置: device 必须已由 mkfs.ravenfs 格式化
 *
 * 错误码 (mkdir): -EEXIST, -ENOENT, -ENOTDIR, -ENAMETOOLONG, -ENOSPC
 * 错误码 (rmdir): -ENOENT, -ENOTDIR, -ENOTEMPTY, -EBUSY
 * ═══════════════════════════════════════════════════════════════ */

static int failures = 0;
static int tests    = 0;

#define CHECK(cond, fmt, ...) do {                              \
    tests++;                                                    \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL [%d] " fmt "\n",              \
                tests, ##__VA_ARGS__);                          \
        failures++;                                             \
    } else {                                                    \
        fprintf(stderr, "  PASS [%d] " fmt "\n",              \
                tests, ##__VA_ARGS__);                          \
    }                                                           \
} while(0)

/* 成功: 返回 0 */
#define CHECK_OK(expr, fmt, ...) \
    CHECK((expr) == 0, fmt " (rc=0)", ##__VA_ARGS__)

/* 失败: 返回指定 -errno */
#define CHECK_ERR(expr, expected_err, fmt, ...) do {            \
    int __rc = (expr);                                          \
    CHECK(__rc == (expected_err),                               \
          fmt " → got %d, want %d", ##__VA_ARGS__,              \
          __rc, (expected_err));                                \
} while(0)

static void print_separator(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════\n");
}

static int find_entry(const RavenFS::DirEntryInfo *entries, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0)
            return i;
    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * Test 1: mount + root readdir
 * ═══════════════════════════════════════════════════════════════ */
static void test_mount_readdir(const char *device) {
    print_separator("Test 1: mount + root readdir");
    uint64_t rc;
    RavenFS fs(device, &rc);
    CHECK(rc == RAVENFS_OK, "mount code=%lu", (unsigned long)rc);
    if (rc) return;

    fs.dump_info();

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/", entries, 64);
    CHECK(n >= 2, "root has >= 2 entries (got %d)", n);

    if (n >= 2) {
        int dot = find_entry(entries, n, ".");
        int dotdot = find_entry(entries, n, "..");
        CHECK(dot >= 0,     "root has '.' entry (index %d)", dot);
        CHECK(dotdot >= 0,  "root has '..' entry (index %d)", dotdot);

        fprintf(stderr, "  root entries:\n");
        for (int i = 0; i < n; i++)
            fprintf(stderr, "    [%d] inode=%lu  name='%s'\n",
                    i, (unsigned long)entries[i].inode, entries[i].name);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Test 2: mkdir single level
 * ═══════════════════════════════════════════════════════════════ */
static void test_mkdir_single(const char *device) {
    print_separator("Test 2: mkdir single level");
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) return;

    CHECK_OK(fs.mkdir("/tmp"),     "mkdir /tmp");
    CHECK_OK(fs.mkdir("/etc"),     "mkdir /etc");
    CHECK_OK(fs.mkdir("/var"),     "mkdir /var");
    CHECK_OK(fs.mkdir("/home"),    "mkdir /home");
    CHECK_OK(fs.mkdir("/mnt"),     "mkdir /mnt");

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/", entries, 64);
    CHECK(n >= 6, "root has >= 6 entries (got %d)", n);
    CHECK(find_entry(entries, n, "tmp")  >= 0, "found 'tmp' in root");
    CHECK(find_entry(entries, n, "etc")  >= 0, "found 'etc' in root");
    CHECK(find_entry(entries, n, "var")  >= 0, "found 'var' in root");
    CHECK(find_entry(entries, n, "home") >= 0, "found 'home' in root");
    CHECK(find_entry(entries, n, "mnt")  >= 0, "found 'mnt' in root");

    CHECK_ERR(fs.mkdir("/tmp"), -EEXIST, "mkdir /tmp again");
    CHECK_ERR(fs.mkdir("/var"), -EEXIST, "mkdir /var again");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 3: nested mkdir
 * ═══════════════════════════════════════════════════════════════ */
static void test_mkdir_nested(const char *device) {
    print_separator("Test 3: nested mkdir");
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) return;

    CHECK_OK(fs.mkdir("/tmp/sub1"),      "mkdir /tmp/sub1");
    CHECK_OK(fs.mkdir("/tmp/sub2"),      "mkdir /tmp/sub2");
    CHECK_OK(fs.mkdir("/tmp/sub1/a"),    "mkdir /tmp/sub1/a");
    CHECK_OK(fs.mkdir("/tmp/sub1/b"),    "mkdir /tmp/sub1/b");
    CHECK_OK(fs.mkdir("/home/user"),     "mkdir /home/user");
    CHECK_OK(fs.mkdir("/home/user/docs"), "mkdir /home/user/docs");

    RavenFS::DirEntryInfo entries[64];

    int n = fs.readdir("/tmp/sub1", entries, 64);
    CHECK(n >= 3, "/tmp/sub1 has >= 3 entries (got %d)", n);
    CHECK(find_entry(entries, n, ".")  >= 0, "/tmp/sub1 has '.'");
    CHECK(find_entry(entries, n, "..") >= 0, "/tmp/sub1 has '..'");
    CHECK(find_entry(entries, n, "a")  >= 0, "/tmp/sub1 has 'a'");
    CHECK(find_entry(entries, n, "b")  >= 0, "/tmp/sub1 has 'b'");

    int n2 = fs.readdir("/home/user/docs", entries, 64);
    CHECK(n2 >= 2, "/home/user/docs has >= 2 entries (got %d)", n2);
    CHECK(find_entry(entries, n2, "..") >= 0, "/home/user/docs has '..'");

    int dot_idx = find_entry(entries, n2, ".");
    if (dot_idx >= 0) {
        uint64_t my_inum;
        CHECK(fs.path_resolve("/home/user/docs", &my_inum) == 0,
              "resolve /home/user/docs");
        CHECK(entries[dot_idx].inode == my_inum,
              "'.' inode == self (%lu == %lu)",
              (unsigned long)entries[dot_idx].inode,
              (unsigned long)my_inum);
    }

    /* 不存在的父路径 */
    CHECK_ERR(fs.mkdir("/nonexistent/sub"), -ENOENT,
              "mkdir on nonexistent parent");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 4: rmdir
 * ═══════════════════════════════════════════════════════════════ */
static void test_rmdir(const char *device) {
    print_separator("Test 4: rmdir");
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) return;

    CHECK_OK(fs.rmdir("/mnt"), "rmdir /mnt (empty)");

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/", entries, 64);
    CHECK(find_entry(entries, n, "mnt") < 0, "/mnt gone from root");

    CHECK_OK(fs.rmdir("/home/user/docs"), "rmdir /home/user/docs (empty)");
    int n2 = fs.readdir("/home/user", entries, 64);
    CHECK(find_entry(entries, n2, "docs") < 0, "docs gone from /home/user");

    CHECK_ERR(fs.rmdir("/tmp"), -ENOTEMPTY, "rmdir /tmp (non-empty)");
    CHECK_ERR(fs.rmdir("/"),    -EBUSY,     "rmdir / (root)");

    CHECK_ERR(fs.rmdir("/nonexistent"), -ENOENT, "rmdir nonexistent");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 5: edge cases + error code cross-check
 * ═══════════════════════════════════════════════════════════════ */
static void test_edge_cases(const char *device) {
    print_separator("Test 5: edge cases");
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) return;

    /* 创建-删除-再创建 */
    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge");
    CHECK_ERR(fs.mkdir("/test_edge"), -EEXIST, "mkdir /test_edge again");
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge");
    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge after delete");
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge again");

    /* readdir 不存在路径 */
    CHECK(fs.readdir("/no_exist", nullptr, 0) < 0,
          "readdir nonexistent → negative");

    /* 父组件不存在的路径 */
    CHECK_ERR(fs.mkdir("/a/b/c"), -ENOENT, "mkdir with nonexistent mid");
    CHECK_ERR(fs.rmdir("/a/b/c"), -ENOENT, "rmdir with nonexistent mid");

    /* readdir 非目录 → -ENOTDIR（目前没有文件可测，留空） */
}

/* ═══════════════════════════════════════════════════════════════
 * Test 6: error code verification for mount
 * ═══════════════════════════════════════════════════════════════ */
static void test_error_codes() {
    print_separator("Test 6: error codes");

    uint64_t rc;
    RavenFS fs("/dev/no_such_device", &rc);
    CHECK(rc == RAVENFS_ERR_OPEN, "nonexistent → ERR_OPEN (%lu)",
          (unsigned long)rc);

    /* null res_code 不应崩 */
    RavenFS fs2("/dev/no_such_device", nullptr);
    CHECK(1, "null res_code → no crash");
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device>\n"
                        "  e.g. %s /dev/nvme1n1\n"
                        "  or   %s /tmp/ravenfs.img\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *device = argv[1];

    fprintf(stderr,
        "\n"
        "  ██████╗  █████╗ ██╗   ██╗███████╗███╗   ██╗███████╗███████╗\n"
        "  ██╔══██╗██╔══██╗██║   ██║██╔════╝████╗  ██║██╔════╝██╔════╝\n"
        "  ██████╔╝███████║╚██╗ ██╔╝█████╗  ██╔██╗ ██║█████╗  ███████╗\n"
        "  ██╔══██╗██╔══██║ ╚████╔╝ ██╔══╝  ██║╚██╗██║██╔══╝  ╚════██║\n"
        "  ██║  ██║██║  ██║  ╚██╔╝  ███████╗██║ ╚████║██║     ███████║\n"
        "  ╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝  ╚═══╝╚═╝     ╚══════╝\n"
        "  Phase 1 — mkdir / rmdir\n\n"
        "  Device: %s\n\n", device);

    test_mount_readdir(device);
    test_mkdir_single(device);
    test_mkdir_nested(device);
    test_rmdir(device);
    test_edge_cases(device);
    test_error_codes();

    fprintf(stderr,
        "\n═══ Results: %d tests, %d failures ═══\n\n",
        tests, failures);

    return failures > 0 ? 1 : 0;
}
