// SPDX-License-Identifier: GPL-2.0
/*
 * mkfs_common.cpp —— mkfs 设备探测 / 已有 FS 检测 / LBA 探测 / 格式名（共用底座）。
 *
 * 盘面写出口现**只保留 v2.0**（mkfs_v20.cpp，盘面头 disk_struct_v2.0.h）。v1.1/v1.2/
 * v1.3 写出口（固定 inode 数组 + inode 位图）已随 v1.x 盘面头一并退役删除。
 */
#include "mkfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>

/* ═══════════════════════════════════════════════════════════════
 * 硬件探针
 * ═══════════════════════════════════════════════════════════════ */
static int try_open_parent(const char *device_path) {
    struct stat st;
    if (stat(device_path, &st) < 0 || !S_ISBLK(st.st_mode))
        return -1;

    char link[256];
    snprintf(link, sizeof(link),
             "/sys/dev/block/%d:%d",
             major(st.st_rdev), minor(st.st_rdev));

    char target[256];
    ssize_t len = readlink(link, target, sizeof(target) - 1);
    if (len < 0) return -1;
    target[len] = '\0';

    const char *devname = strrchr(target, '/');
    if (!devname) return -1;
    devname++;

    char parent[256];
    strncpy(parent, devname, sizeof(parent) - 1);
    char *p = nullptr;

    if (strncmp(parent, "nvme", 4) == 0) {
        p = strrchr(parent, 'p');
        if (p && p[1] >= '0' && p[1] <= '9') *p = '\0';
    } else {
        p = parent + strlen(parent);
        while (p > parent && p[-1] >= '0' && p[-1] <= '9') p--;
        *p = '\0';
    }

    if (strcmp(parent, devname) == 0) return -1;

    char devpath[512];
    snprintf(devpath, sizeof(devpath), "/dev/%s", parent);
    return open(devpath, O_RDONLY);
}

static bool probe_nvme(int fd) {
    // NVME_IOCTL_ID returns nsid (>0) on success, -1 on error
    return ioctl(fd, NVME_IOCTL_ID, 0) > 0;
}

static bool probe_ata(int fd) {
    struct hd_driveid id;
    return ioctl(fd, HDIO_GET_IDENTITY, &id) == 0;
}

DevType probe_hardware(int fd, const char *device_path) {
    if (probe_nvme(fd)) return DEV_BLOCK_NVME;
    if (probe_ata(fd))  return DEV_BLOCK_ATA;

    int parent_fd = try_open_parent(device_path);
    if (parent_fd >= 0) {
        DevType t = DEV_UNKNOWN;
        if (probe_nvme(parent_fd))         t = DEV_BLOCK_NVME;
        else if (probe_ata(parent_fd))     t = DEV_BLOCK_ATA;
        close(parent_fd);
        return t;
    }

    return DEV_UNKNOWN;
}

/* ═══════════════════════════════════════════════════════════════
 * 设备信息
 * ═══════════════════════════════════════════════════════════════ */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#define BLKSSZGET  _IO(0x12, 104)
#endif

static bool is_mounted(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode))
        return false;

    char link_buf[256];
    snprintf(link_buf, sizeof(link_buf),
             "/sys/dev/block/%d:%d",
             major(st.st_rdev), minor(st.st_rdev));
    char real[256];
    ssize_t len = readlink(link_buf, real, sizeof(real) - 1);
    if (len < 0) return false;
    real[len] = '\0';

    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return false;

    char line[1024];
    bool mounted = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned int mj = 0, mn = 0;
        char mp[256] = {};
        int n = sscanf(line,
            "%*d %*d %*u:%*u / %255s %*s %*s %*s %u:%u",
            mp, &mj, &mn);
        if (n >= 3 && (int)major(st.st_rdev) == (int)mj
            && (int)minor(st.st_rdev) == (int)mn) {
            mounted = true;
            break;
        }
    }
    fclose(f);
    return mounted;
}

int get_device_info(const char *path, DevInfo *info) {
    struct stat st;
    if (stat(path, &st) < 0) {
        perror("stat");
        return -1;
    }
    memset(info, 0, sizeof(*info));

    if (S_ISBLK(st.st_mode)) {
        int bfd = open(path, O_RDONLY);
        if (bfd < 0) { perror("open"); return -1; }
        if (ioctl(bfd, BLKGETSIZE64, &info->bytes) < 0) {
            perror("BLKGETSIZE64"); close(bfd); return -1;
        }
        close(bfd);
        info->is_mounted = is_mounted(path);
        info->type = DEV_UNKNOWN;
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        info->type  = DEV_REGULAR;
        info->bytes = (uint64_t)st.st_size;
        return 0;
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * LBA_size 探测（ioctl）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t get_lba_size(int fd, DevType type) {
    if (type == DEV_REGULAR) return 4096;

    int sz = 0;
    if (ioctl(fd, BLKSSZGET, &sz) != 0 || sz <= 0) {
        fprintf(stderr, "error: BLKSSZGET failed to determine LBA_size\n");
        return 0;
    }
    if (4096 % (unsigned)sz != 0) {
        fprintf(stderr, "error: LBA_size %d does not divide 4096\n", sz);
        return 0;
    }
    return (uint64_t)sz;
}

/* ═══════════════════════════════════════════════════════════════
 * 已有 FS 检测
 * ═══════════════════════════════════════════════════════════════ */
#define EXT4_MAGIC_OFFSET  0x438
#define EXT4_MAGIC_VALUE   0xEF53

bool check_existing_fs(int fd) {
    uint64_t raven_magic = 0;
    pread(fd, &raven_magic, 8, 0);
    if (raven_magic == SUPERBLOCK_MAGIC) {
        fprintf(stderr, "RavenFS superblock found (v1.x \"RAVN\") — use -f to overwrite\n");
        return true;
    }
    if (raven_magic == SUPERBLOCK_MAGIC_V20_W0) {   /* v2.0 magic[0] 前缀 "RAVENFS\0" */
        fprintf(stderr, "RavenFS superblock found (v2.0 \"RAVENFS\") — use -f to overwrite\n");
        return true;
    }

    uint16_t ext4_magic = 0;
    pread(fd, &ext4_magic, 2, EXT4_MAGIC_OFFSET);
    if (ext4_magic == EXT4_MAGIC_VALUE) {
        fprintf(stderr, "ext4 filesystem detected — use -f to overwrite\n");
        return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════
 * 格式名
 * ═══════════════════════════════════════════════════════════════ */
const char *mkfs_fmt_name(MkfsFormat f) {
    switch (f) {
    case MKFS_FMT_V2_0: return "v2.0";
    }
    return "v2.0";
}
