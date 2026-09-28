#include "mkfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <device>\n"
        "\n"
        "Options:\n"
        "  --inode-ratio=N    FS_blocks per inode (default: 8)\n"
        "  --inode-count=N    Direct inode count (overrides --inode-ratio)\n"
        "  --inodes=N         Alias for --inode-count\n"
        "  --backups=N        Backup Superblock count (default: 3)\n"
        "  --label=NAME       Volume label (max 48 bytes)\n"
        "  --uuid=UUID        Explicit UUID (auto-generated if omitted)\n"
        "  --v1.1             Write v1.1 disk format (version=2, root inode 0)\n"
        "                     for juvenile god / tests (default: v1.2)\n"
        "  -f, --force        Overwrite existing filesystem\n"
        "  -v, --verbose      Print detailed layout info\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  success\n"
        "  1  parameter error\n"
        "  2  device is mounted\n"
        "  3  existing filesystem (use -f)\n"
        "  4  unsupported device\n"
        "  5  device too small or misaligned\n"
        "  6  access error\n",
        prog);
}

int main(int argc, char *argv[]) {
    MkfsParams params{};
    params.inode_ratio = 8;
    params.backups = 3;
    uuid_clear(params.uuid);

    static struct option long_opts[] = {
        {"inode-ratio", required_argument, 0, 'r'},
        {"inode-count", required_argument, 0, 'n'},
        {"inodes",      required_argument, 0, 'n'},
        {"backups",     required_argument, 0, 'b'},
        {"label",       required_argument, 0, 'l'},
        {"uuid",        required_argument, 0, 'u'},
        {"v1.1",        no_argument,       0, '1'},
        {"force",       no_argument,       0, 'f'},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "fvh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'r': {
            char *end = nullptr;
            params.inode_ratio = strtoull(optarg, &end, 0);
            if (*end || params.inode_ratio == 0) {
                fprintf(stderr, "invalid --inode-ratio: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'n': {
            char *end = nullptr;
            params.inode_count = strtoull(optarg, &end, 0);
            if (*end || params.inode_count == 0) {
                fprintf(stderr, "invalid --inode-count: %s\n", optarg);
                return 1;
            }
            params.inode_count_explicit = true;
            break;
        }
        case 'b': {
            char *end = nullptr;
            unsigned long n = strtoul(optarg, &end, 0);
            if (*end) {
                fprintf(stderr, "invalid --backups: %s\n", optarg);
                return 1;
            }
            params.backups = (uint32_t)n;
            break;
        }
        case 'l':
            if (strlen(optarg) >= sizeof(params.label)) {
                fprintf(stderr, "label too long (max 48 bytes)\n");
                return 1;
            }
            strncpy(params.label, optarg, sizeof(params.label));
            break;
        case 'u':
            if (uuid_parse(optarg, params.uuid) != 0) {
                fprintf(stderr, "invalid UUID format\n");
                return 1;
            }
            break;
        case '1':
            params.v11 = true;      /* --v1.1：写 v1.1 盘（god/测试团队） */
            break;
        case 'f':
            params.force = true;
            break;
        case 'v':
            params.verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: <device> is required\n");
        return 1;
    }
    params.device = argv[optind];

    /* ─── 设备检测 ─── */
    DevInfo dev{};
    if (get_device_info(params.device, &dev) < 0)
        return 6;

    /* ─── 打开设备 ─── */
    int fd = open(params.device, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 6;
    }

    if (dev.type == DEV_REGULAR) {
        // File: LBA_size=4096, multiplier=1, world_base_lba=1 (skip past Primary SB at byte 0)
        params.lba_size      = 4096;
        params.multiplier    = 1;
        params.world_base_lba = 1;

        if (dev.bytes % 4096 != 0) {
            fprintf(stderr, "error: file size %lu not 4096-aligned\n",
                    (unsigned long)dev.bytes);
            close(fd);
            return 5;
        }
    } else {
        /* ─── ioctl 硬件探针 ─── */
        DevType hw = probe_hardware(fd, params.device);
        if (hw == DEV_UNKNOWN) {
            fprintf(stderr,
                "error: %s is not a supported hardware device.\n"
                "       Only NVMe and SATA drives are accepted.\n",
                params.device);
            close(fd);
            return 4;
        }
        dev.type = hw;

        if (dev.is_mounted) {
            fprintf(stderr, "error: %s is mounted\n", params.device);
            close(fd);
            return 2;
        }

        // 实测 LBA_size（ioctl BLKSSZGET）
        params.lba_size = get_lba_size(fd, hw);
        if (params.lba_size == 0) { close(fd); return 5; }

        params.multiplier    = 4096 / params.lba_size;

        // 块设备统一模型：设备 fd offset 0 = Primary SB
        // FS_block 世界从设备 fd offset 4096 开始
        params.world_base_lba = params.multiplier;
    }

    /* ─── 已有 FS 检测 ─── */
    if (!params.force && check_existing_fs(fd)) {
        close(fd);
        return 3;
    }

    /* ─── inode count 确定 ─── */
    uint64_t total_blocks = dev.bytes / 4096 - 1;
    if (!params.inode_count_explicit) {
        // inode_count = ceil(total_blocks / inode_ratio)，再 16 向上取整
        uint64_t raw = (total_blocks + params.inode_ratio - 1) / params.inode_ratio;
        params.inode_count = ((raw + 15) / 16) * 16;
    } else if (params.inode_count % 16 != 0) {
        // 直指定的 inode_count 也 16 向上取整
        params.inode_count = ((params.inode_count + 15) / 16) * 16;
    }

    /* ─── 几何计算 ─── */
    FsGeometry geo = calc_geometry(total_blocks, params.inode_count);

    if (geo.inode_array_bitmap_base <= 1) {
        fprintf(stderr,
            "error: device too small for %lu inodes\n",
            (unsigned long)params.inode_count);
        close(fd);
        return 5;
    }

    if (params.verbose) {
        fprintf(stderr,
            "device:          %s\n"
            "type:            %s\n"
            "bytes:           %lu\n"
            "total_blocks:    %lu\n"
            "inode_ratio:     %lu\n"
            "inode_count:     %lu\n"
            "backups:         %u\n"
            "lba_size:        %lu\n"
            "multiplier:      %lu\n"
            "world_base_lba:  %lu\n"
            "\n"
            "metadata layout (FS_block world):\n"
            "  bitmap:        [%lu, %lu)  (%lu blocks)\n"
            "  inode array:   [%lu, %lu)  (%lu blocks)\n"
            "  inode bitmap:  [%lu, %lu)  (%lu blocks)\n",
            params.device,
            dev.type == DEV_BLOCK_NVME ? "NVMe" :
                dev.type == DEV_BLOCK_ATA ? "ATA" : "file",
            (unsigned long)dev.bytes,
            (unsigned long)geo.total_blocks,
            (unsigned long)params.inode_ratio,
            (unsigned long)params.inode_count,
            (unsigned)params.backups,
            (unsigned long)params.lba_size,
            (unsigned long)params.multiplier,
            (unsigned long)params.world_base_lba,
            (unsigned long)geo.bitmap_start,
            (unsigned long)geo.total_blocks,
            (unsigned long)geo.bitmap_blocks,
            (unsigned long)geo.inode_array_base,
            (unsigned long)geo.bitmap_start,
            (unsigned long)geo.inode_blocks,
            (unsigned long)geo.inode_array_bitmap_base,
            (unsigned long)geo.inode_array_base,
            (unsigned long)geo.inode_bitmap_blocks);
    }

    /* ─── 生成 UUID ─── */
    if (uuid_is_null(params.uuid))
        uuid_generate(params.uuid);

    /* ─── 执行 ─── */
    int ret = format_fs(fd, params, geo);
    close(fd);

    if (ret == 0) {
        char uuid_str[37];
        uuid_unparse(params.uuid, uuid_str);
        printf("mkfs.ravenfs: done  device=%s  format=%s  blocks=%lu  "
               "ratio=%lu  inodes=%lu  backups=%u  uuid=%s\n",
               params.device,
               params.v11 ? "v1.1" : "v1.2",
               (unsigned long)geo.total_blocks,
               (unsigned long)params.inode_ratio,
               (unsigned long)params.inode_count,
               (unsigned)params.backups,
               uuid_str);
    }

    return ret;
}
