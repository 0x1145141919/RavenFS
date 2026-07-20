#include "fsck.hpp"

#include <cstdio>
#include <cstdlib>

#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <device>\n"
        "\n"
        "Read-only filesystem inspector for RavenFS.\n"
        "\n"
        "Options:\n"
        "  -v, --verbose      More detailed output\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  no errors\n"
        "  1  filesystem errors found\n"
        "  2  access / read error\n",
        prog);
}

int main(int argc, char *argv[]) {
    bool verbose = false;

    static struct option long_opts[] = {
        {"verbose",  no_argument, 0, 'v'},
        {"help",     no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "vh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'v': verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 2;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: <device> is required\n");
        return 2;
    }

    const char *device = argv[optind];

    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    // 实测 LBA_size（BLKSSZGET），失败则退出
    uint64_t lba_size = fsck_probe_lba_size(fd);
    if (lba_size == 0) { close(fd); return 2; }

    int ret = fsck_dump(fd, verbose, lba_size);
    close(fd);
    return ret;
}
