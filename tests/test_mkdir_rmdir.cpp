#include "ravenfs/ravenfs.hpp"
#include "test_common.hpp"

int g_tests     = 0;
int g_failures  = 0;

void print_separator(const char *title) {
    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  %s\n", title);
    fprintf(stderr, "═══════════════════════════════════════════\n");
}

int find_entry(const RavenFS::DirEntryInfo *entries, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0)
            return i;
    return -1;
}

void print_summary() {
    fprintf(stderr, "\n═══ Results: %d tests, %d failures ═══\n\n",
            g_tests, g_failures);
}

/* ═══════════════════════════════════════════════════════════════
 * Test 1: mount + root readdir
 * ═══════════════════════════════════════════════════════════════ */
void test_mount_readdir(const char *device) {
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
void test_mkdir_single(const char *device) {
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
void test_mkdir_nested(const char *device) {
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

    CHECK_ERR(fs.mkdir("/nonexistent/sub"), -ENOENT,
              "mkdir on nonexistent parent");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 4: rmdir
 * ═══════════════════════════════════════════════════════════════ */
void test_rmdir(const char *device) {
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
void test_edge_cases(const char *device) {
    print_separator("Test 5: edge cases");
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) return;

    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge");
    CHECK_ERR(fs.mkdir("/test_edge"), -EEXIST, "mkdir /test_edge again");
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge");
    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge after delete");
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge again");

    CHECK(fs.readdir("/no_exist", nullptr, 0) < 0,
          "readdir nonexistent → negative");

    CHECK_ERR(fs.mkdir("/a/b/c"), -ENOENT, "mkdir with nonexistent mid");
    CHECK_ERR(fs.rmdir("/a/b/c"), -ENOENT, "rmdir with nonexistent mid");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 6: error code verification for mount
 * ═══════════════════════════════════════════════════════════════ */
void test_error_codes() {
    print_separator("Test 6: error codes");

    uint64_t rc;
    RavenFS fs("/dev/no_such_device", &rc);
    CHECK(rc == RAVENFS_ERR_OPEN, "nonexistent → ERR_OPEN (%lu)",
          (unsigned long)rc);

    RavenFS fs2("/dev/no_such_device", nullptr);
    CHECK(1, "null res_code → no crash");
}
