#include "../include/ravenfs/ravenfs.hpp"
#include "test_common.hpp"

/* 测试函数声明 */
void test_mkdir_single(RavenFS &fs);
void test_mkdir_nested(RavenFS &fs);
void test_rmdir(RavenFS &fs);
void test_edge_cases(RavenFS &fs);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }

    fprintf(stderr,
        "\n"
        "  ╔══════════════════════════════════════════╗\n"
        "  ║     RavenFS Phase 1 — VFS Cache Test    ║\n"
        "  ╚══════════════════════════════════════════╝\n"
        "  Device: %s\n\n", argv[1]);

    uint64_t rc;
    RavenFS fs(argv[1], &rc);
    if (rc) {
        fprintf(stderr, "Mount failed: %lu\n", rc);
        return 1;
    }

    test_mkdir_single(fs);
    test_mkdir_nested(fs);
    test_rmdir(fs);
    test_edge_cases(fs);

    print_summary();
    return g_failures > 0 ? 1 : 0;
}
