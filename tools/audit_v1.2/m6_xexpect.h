#pragma once
/*
 * ═══════════════════════════════════════════════════════════════════════════
 * m6_xexpect.h — M6 xattr 期望清单共享件（host 侧；producer/consumer 共用）
 *
 * 生产端：mkstates_x.cpp      消费端：m6_xattr_dump.cpp
 * 目的：把「每个 inode 应有的 xattr（命名空间 index / 后缀 / 值）」解耦为一份可交换清单，
 *       令 host 侧独立解析器（m6_xattr_dump verify）与生产端用同一真值。
 *
 * 与 M4.1 内容清单（m41_expect.h）**同构**：值以 XXH64(seed=0) 指纹断言（值可 opaque）。
 *
 * 清单格式（纯文本；`#` 注释；path / name 不含空白）：
 *   # ravenfs-m6-xexpect v1
 *   X <path> <inode> <type:0|1> <n_entries>      # type 0 = 无 xattr（ptr 哨兵）；1 = 有块
 *   E <index> <name|-> <size> <xxh64hex>          # name "-" = 空后缀（ACL）；size 可 0
 *   ...
 *
 * 命名空间 index 抄 ext4（见 disk_struct_v1.2.h RFS_XATTR_INDEX_*）：
 *   user=1, posix_acl_access=2, posix_acl_default=3, trusted=4, security=6, system=7
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <xxhash.h>

struct XExpectEntry {
    uint8_t     index;      /* 命名空间 index */
    std::string name;       /* 后缀（ACL 为空串） */
    uint32_t    size;       /* 值长（字节） */
    uint64_t    hash;       /* XXH64(value, seed=0) */
};

struct XExpectIno {
    std::string path;       /* 人类可读路径（清单键） */
    uint64_t    inode;
    uint32_t    type;       /* 0 = no-xattr；1 = xattr 块 */
    std::vector<XExpectEntry> ents;
};

static inline uint64_t xexp_hash(const void *p, size_t n) {
    return (uint64_t)XXH64(p, n, 0);
}

static inline bool m6_write_xexpect(const char *path, const std::vector<XExpectIno> &v) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "# ravenfs-m6-xexpect v1\n");
    fprintf(fp, "# X <path> <inode> <type:0|1> <n_entries>\n");
    fprintf(fp, "# E <index> <name|-> <size> <xxh64hex>\n");
    for (const XExpectIno &x : v) {
        fprintf(fp, "X %s %llu %u %zu\n", x.path.c_str(),
                (unsigned long long)x.inode, x.type, x.ents.size());
        for (const XExpectEntry &e : x.ents)
            fprintf(fp, "E %u %s %u %016llx\n", e.index,
                    e.name.empty() ? "-" : e.name.c_str(),
                    e.size, (unsigned long long)e.hash);
    }
    fclose(fp);
    return true;
}

static inline bool m6_read_xexpect(const char *path, std::vector<XExpectIno> &v) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char line[4096];
    XExpectIno *cur = nullptr;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == 0) continue;
        if (line[0] == 'X') {
            char nm[2048] = {0};
            unsigned long long ino = 0; unsigned ty = 0, ne = 0;
            if (sscanf(line, "X %2047s %llu %u %u", nm, &ino, &ty, &ne) >= 4) {
                XExpectIno x;
                x.path = nm; x.inode = ino; x.type = ty;
                v.push_back(x); cur = &v.back();
            } else { fclose(fp); return false; }
        } else if (line[0] == 'E') {
            if (!cur) { fclose(fp); return false; }
            unsigned idx = 0, sz = 0; char nm[2048] = {0}; char hex[32] = {0};
            if (sscanf(line, "E %u %2047s %u %31s", &idx, nm, &sz, hex) >= 4) {
                XExpectEntry e;
                e.index = (uint8_t)idx;
                e.name  = (strcmp(nm, "-") == 0) ? std::string() : std::string(nm);
                e.size  = sz;
                e.hash  = strtoull(hex, nullptr, 16);
                cur->ents.push_back(e);
            } else { fclose(fp); return false; }
        }
    }
    fclose(fp);
    return true;
}
