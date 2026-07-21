/* ═══════════════════════════════════════════════════════════════
 * ravenfs.fsck — 文件系统检查工具
 *
 * 两种模式：
 *   1. 基本 fsck（默认）：superblock 验证、bitmap 一致性、backup 链、根 inode/dentry
 *   2. --tree：递归打印目录树（需链接 ravenfs_god）
 *
 * 用法:
 *   ravenfs.fsck <device>
 *   ravenfs.fsck <device> --tree [output_file]
 * ═══════════════════════════════════════════════════════════════ */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>

/* ─── god 类（--tree 模式必需） ─── */
#include "Userspace_God.hpp"
/* Userspace_God.hpp 已定义 FsGeometry，fsck.hpp 中同名定义通过 guard 跳过 */
#define RAVENFS_FSGEOMETRY_DEFINED

#include "fsck.hpp"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <device>\n"
        "\n"
        "Read-only filesystem inspector for RavenFS.\n"
        "\n"
        "Options:\n"
        "  -v, --verbose      More detailed output (basic fsck mode)\n"
        "  -t, --tree [file]  Print directory tree to stdout or file\n"
        "  -d, --validate-dentry  Full dentry B+tree validation from root\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  no errors\n"
        "  1  filesystem errors found\n"
        "  2  access / read error\n",
        prog);
}

/* ═══════════════════════════════════════════════════════════════
 * --tree 模式（依赖 ravenfs_god）
 * ═══════════════════════════════════════════════════════════════ */

/* ─── 递归打印目录树 ─── */
static void print_tree(RavenFS &fs, uint64_t dir_inode, FILE *out,
                       std::vector<bool> &prefixes)
{
    RavenFS::DirEntryInfo entries[128];
    int n = fs.readdir_inum(dir_inode, entries, 128);
    if (n < 0) return;

    /* 收集非 . 和 .. 的子条目 */
    std::vector<std::pair<uint64_t, std::string>> children;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, ".") == 0 ||
            strcmp(entries[i].name, "..") == 0)
            continue;
        children.push_back({entries[i].inode, entries[i].name});
    }

    for (size_t i = 0; i < children.size(); i++) {
        bool last = (i == children.size() - 1);
        uint64_t child_inum = children[i].first;
        const std::string &name = children[i].second;

        /* 前缀 */
        for (size_t j = 0; j < prefixes.size(); j++)
            fprintf(out, "%s", prefixes[j] ? "│   " : "    ");
        fprintf(out, "%s── %s", last ? "└" : "├", name.c_str());

        int typ = fs.inode_type(child_inum);
        if (typ == INODE_TYPE_DIR) {
            fprintf(out, "/\n");
            prefixes.push_back(!last);
            print_tree(fs, child_inum, out, prefixes);
            prefixes.pop_back();
        } else {
            fprintf(out, "\n");
        }
    }
}

static int run_tree(const char *device, const char *outfile) {
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) {
        fprintf(stderr, "ravenfs.fsck: mount failed (code=%lu)\n",
                (unsigned long)rc);
        return 1;
    }

    FILE *out = stdout;
    bool close_out = false;
    if (outfile) {
        out = fopen(outfile, "w");
        if (!out) {
            perror("ravenfs.fsck: fopen");
            return 1;
        }
        close_out = true;
    }

    fprintf(out, "/\n");
    std::vector<bool> prefixes;
    print_tree(fs, 0, out, prefixes);

    if (close_out)
        fclose(out);

    fprintf(stderr, "ravenfs.fsck: done\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * 主函数
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    bool verbose = false;
    bool do_tree = false;
    bool do_dentry_validate = false;
    const char *outfile = nullptr;

    static struct option long_opts[] = {
        {"verbose",          no_argument,       0, 'v'},
        {"tree",             optional_argument, 0, 't'},
        {"validate-dentry",  no_argument,       0, 'd'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    /* 先 getopt_long 解析 */
    int opt;
    while ((opt = getopt_long(argc, argv, "vt::dh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'v': verbose = true; break;
        case 't':
            do_tree = true;
            if (optarg)
                outfile = optarg;
            break;
        case 'd': do_dentry_validate = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 2;
        }
    }

    /* 兼容旧式 --tree [file] 参数风格（带空格） */
    for (int i = optind + 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0) {
            do_tree = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outfile = argv[i + 1];
                /* 后移已消费的参数 */
                argc--;
                for (int j = i + 1; j < argc; j++)
                    argv[j] = argv[j + 1];
            }
            argc--;
            for (int j = i; j < argc; j++)
                argv[j] = argv[j + 1];
            break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: <device> is required\n");
        return 2;
    }

    const char *device = argv[optind];

    if (do_tree) {
        return run_tree(device, outfile);
    }

    /* ─── 打开设备 ─── */
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    /* ─── Dentry 树校验模式（走上帝类缓存） ─── */
    if (do_dentry_validate) {
        close(fd);  // 关闭原始 fd，改走上帝类 mount
        uint64_t rc;
        RavenFS fs(device, &rc);
        if (rc) {
            fprintf(stderr, "ERROR: mount failed (code=%lu)\n", (unsigned long)rc);
            return 2;
        }
        int ret = fs.validate_dentry_tree();
        fs.unmount();
        return ret;
    }

    /* ─── 基本 fsck 模式 ─── */
    uint64_t lba_size = fsck_probe_lba_size(fd);
    if (lba_size == 0) { close(fd); return 2; }

    int ret = fsck_dump(fd, verbose, lba_size);
    close(fd);
    return ret;
}
