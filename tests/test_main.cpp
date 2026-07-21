#include "test_common.hpp"

/* 测试函数声明（定义在 test_mkdir_rmdir.cpp） */
void test_mount_readdir(const char *device);
void test_mkdir_single(const char *device);
void test_mkdir_nested(const char *device);
void test_rmdir(const char *device);
void test_edge_cases(const char *device);
void test_error_codes();

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

    print_summary();
    return g_failures > 0 ? 1 : 0;
}
