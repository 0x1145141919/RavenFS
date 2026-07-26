#include "../include/ravenfs/ravenfs.hpp"
#include "test_common.hpp"
#include "debug_dump.hpp"

int g_tests    = 0;
int g_failures = 0;

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
 * Test 1: mkdir single level + root readdir
 * ═══════════════════════════════════════════════════════════════ */
void test_mkdir_single(RavenFS &fs) {
    print_separator("Test 1: mkdir single level");

    CHECK_OK(fs.mkdir("/tmp"),     "mkdir /tmp");
    debug_dump("after mkdir /tmp", fs);
    CHECK_OK(fs.mkdir("/etc"),     "mkdir /etc");
    debug_dump("after mkdir /etc", fs);
    CHECK_OK(fs.mkdir("/var"),     "mkdir /var");
    debug_dump("after mkdir /var", fs);
    CHECK_OK(fs.mkdir("/home"),    "mkdir /home");
    debug_dump("after mkdir /home", fs);
    CHECK_OK(fs.mkdir("/mnt"),     "mkdir /mnt");
    debug_dump("after mkdir /mnt", fs);

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/", entries, 64);
    debug_dump("after readdir /", fs);
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
 * Test 2: nested mkdir
 * ═══════════════════════════════════════════════════════════════ */
void test_mkdir_nested(RavenFS &fs) {
    print_separator("Test 2: nested mkdir");

    CHECK_OK(fs.mkdir("/tmp/sub1"),      "mkdir /tmp/sub1");
    debug_dump("after mkdir /tmp/sub1", fs);
    CHECK_OK(fs.mkdir("/tmp/sub2"),      "mkdir /tmp/sub2");
    debug_dump("after mkdir /tmp/sub2", fs);
    CHECK_OK(fs.mkdir("/tmp/sub1/a"),    "mkdir /tmp/sub1/a");
    debug_dump("after mkdir /tmp/sub1/a", fs);
    CHECK_OK(fs.mkdir("/tmp/sub1/b"),    "mkdir /tmp/sub1/b");
    debug_dump("after mkdir /tmp/sub1/b", fs);
    CHECK_OK(fs.mkdir("/home/user"),     "mkdir /home/user");
    debug_dump("after mkdir /home/user", fs);
    CHECK_OK(fs.mkdir("/home/user/docs"), "mkdir /home/user/docs");
    debug_dump("after mkdir /home/user/docs", fs);

    RavenFS::DirEntryInfo entries[64];

    int n = fs.readdir("/tmp/sub1", entries, 64);
    debug_dump("after readdir /tmp/sub1", fs);

    CHECK(n >= 3, "/tmp/sub1 has >= 3 entries (got %d)", n);
    CHECK(find_entry(entries, n, ".")  >= 0, "/tmp/sub1 has '.'");
    CHECK(find_entry(entries, n, "..") >= 0, "/tmp/sub1 has '..'");
    CHECK(find_entry(entries, n, "a")  >= 0, "/tmp/sub1 has 'a'");
    CHECK(find_entry(entries, n, "b")  >= 0, "/tmp/sub1 has 'b'");

    int n2 = fs.readdir("/home/user/docs", entries, 64);
    debug_dump("after readdir /home/user/docs", fs);
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
 * Test 3: rmdir
 * ═══════════════════════════════════════════════════════════════ */
void test_rmdir(RavenFS &fs) {
    print_separator("Test 3: rmdir");

    debug_dump("before rmdir /mnt", fs);
    CHECK_OK(fs.rmdir("/mnt"), "rmdir /mnt (empty)");
    debug_dump("after rmdir /mnt", fs);

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/", entries, 64);
    debug_dump("after readdir / (post-rmdir)", fs);
    CHECK(find_entry(entries, n, "mnt") < 0, "/mnt gone from root");

    debug_dump("before rmdir /home/user/docs", fs);
    CHECK_OK(fs.rmdir("/home/user/docs"), "rmdir /home/user/docs (empty)");
    debug_dump("after rmdir /home/user/docs", fs);
    int n2 = fs.readdir("/home/user", entries, 64);
    CHECK(find_entry(entries, n2, "docs") < 0, "docs gone from /home/user");

    debug_dump("before rmdir /tmp (non-empty)", fs);
    CHECK_ERR(fs.rmdir("/tmp"), -ENOTEMPTY, "rmdir /tmp (non-empty)");
    debug_dump("after rmdir /tmp (non-empty)", fs);
    CHECK_ERR(fs.rmdir("/"),    -EBUSY,     "rmdir / (root)");

    CHECK_ERR(fs.rmdir("/nonexistent"), -ENOENT, "rmdir nonexistent");
}

/* ═══════════════════════════════════════════════════════════════
 * Test 4: edge cases + error code cross-check
 * ═══════════════════════════════════════════════════════════════ */
void test_edge_cases(RavenFS &fs) {
    print_separator("Test 4: edge cases");

    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge");
    debug_dump("after mkdir /test_edge", fs);
    CHECK_ERR(fs.mkdir("/test_edge"), -EEXIST, "mkdir /test_edge again");
    debug_dump("after mkdir /test_edge (EEXIST)", fs);
    debug_dump("before rmdir /test_edge", fs);
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge");
    debug_dump("after rmdir /test_edge", fs);
    CHECK_OK(fs.mkdir("/test_edge"),       "mkdir /test_edge after delete");
    debug_dump("after mkdir /test_edge (recreate)", fs);
    CHECK_OK(fs.rmdir("/test_edge"),       "rmdir /test_edge again");
    debug_dump("after rmdir /test_edge (final)", fs);

    CHECK(fs.readdir("/no_exist", nullptr, 0) < 0,
          "readdir nonexistent → negative");

    CHECK_ERR(fs.mkdir("/a/b/c"), -ENOENT, "mkdir with nonexistent mid");
    CHECK_ERR(fs.rmdir("/a/b/c"), -ENOENT, "rmdir with nonexistent mid");
}
