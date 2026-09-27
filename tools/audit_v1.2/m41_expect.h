#pragma once
/*
 * ═══════════════════════════════════════════════════════════════════════════
 * m41_expect.h — M4.1 靶子「期望内容清单」共享件（host 侧，producer/consumer 共用）
 *
 * 生产端：mkstates.cpp  消费端：readback.cpp
 * 目的：把「每文件应有内容」与内容生成规则解耦为一份可交换的清单，
 *       令 host 侧（readback verify / dump）与 guest 侧（cat/dd + cmp）用同一真值。
 *
 * 内容生成（确定性）：fill_byte(name, off) —— 只依赖文件名与文件内偏移，
 *   生产端按此写盘、消费端按此重建期望字节；两侧必须字节一致 → 本头文件唯一实现。
 *
 * 清单格式（纯文本；`#` 注释；名字不含空白）：
 *   # ravenfs-m4.1-expect v1
 *   F <name> <inode> <size> <xxh64hex>
 *   S <off> <len> <D|Z>            # D=按 fill_byte 生成；Z=全 0（空洞）
 *   ...                            # segs 有序、无缝、恰好覆盖 [0,size)
 * 或者（外部原样内容，如真实 ELF）：
 *   F <name> <inode> <size> <xxh64hex>
 *   RAW <path>                     # 期望内容 = path 文件原样字节（host 路径）
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <xxhash.h>

/* ── 确定性内容字节生成（生产/消费两侧唯一实现，勿改公式）── */
static inline uint8_t m41_fill_byte(const char *name, uint64_t off) {
    uint64_t h = 1469598103934665603ULL;              /* FNV-1a offset basis */
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    uint64_t x = h ^ (off * 1099511628211ULL);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;         /* murmur3 finalizer */
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint8_t)x;
}

/* kind：'D' = 数据（fill_byte）  'Z' = 零（空洞） */
struct M41Seg {
    uint64_t off;
    uint64_t len;
    char     kind;
};

struct M41File {
    std::string         name;
    uint64_t            inode;
    uint64_t            size;
    std::vector<M41Seg> segs;
    std::string         src;    /* 非空 = RAW：期望内容 = src 文件原样字节（如真实 ELF） */
};

/* 重建某文件的期望内容（RAW 读 src；否则按 segments 生成） */
static inline void m41_build(const M41File &f, std::vector<uint8_t> &out) {
    if (!f.src.empty()) {
        out.clear();
        FILE *fp = fopen(f.src.c_str(), "rb");
        if (fp) {
            uint8_t buf[65536]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.insert(out.end(), buf, buf + n);
            fclose(fp);
        }
        return;
    }
    out.assign((size_t)f.size, 0);
    for (const M41Seg &s : f.segs) {
        if (s.kind != 'D') continue;
        for (uint64_t i = 0; i < s.len; i++)
            out[(size_t)(s.off + i)] = m41_fill_byte(f.name.c_str(), s.off + i);
    }
}

/* 内容 XXH64(seed=0) */
static inline uint64_t m41_hash(const M41File &f) {
    std::vector<uint8_t> b;
    m41_build(f, b);
    return (uint64_t)XXH64(b.data(), b.size(), 0);
}

/* ── 写清单 ── */
static inline bool m41_write_expect(const char *path, const std::vector<M41File> &files) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "# ravenfs-m4.1-expect v1\n");
    for (const M41File &fl : files) {
        fprintf(fp, "F %s %llu %llu %016llx\n",
                fl.name.c_str(),
                (unsigned long long)fl.inode,
                (unsigned long long)fl.size,
                (unsigned long long)m41_hash(fl));
        if (!fl.src.empty())
            fprintf(fp, "RAW %s\n", fl.src.c_str());
        else
            for (const M41Seg &s : fl.segs)
                fprintf(fp, "S %llu %llu %c\n",
                        (unsigned long long)s.off, (unsigned long long)s.len, s.kind);
    }
    fclose(fp);
    return true;
}

/* ── 读清单 ── */
static inline bool m41_read_expect(const char *path, std::vector<M41File> &files) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char line[4096];
    M41File *cur = nullptr;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == 0) continue;
        if (line[0] == 'F') {
            char nm[2048]; unsigned long long ino = 0, sz = 0; char hex[32] = {0};
            if (sscanf(line, "F %2047s %llu %llu %31s", nm, &ino, &sz, hex) >= 4) {
                M41File fl;
                fl.name  = nm;
                fl.inode = ino;
                fl.size  = sz;
                files.push_back(fl);
                cur = &files.back();
            } else {
                fclose(fp); return false;
            }
        } else if (line[0] == 'S') {
            if (!cur) { fclose(fp); return false; }
            unsigned long long off = 0, len = 0; char k = 0;
            if (sscanf(line, "S %llu %llu %c", &off, &len, &k) >= 3)
                cur->segs.push_back(M41Seg{ (uint64_t)off, (uint64_t)len, k });
        } else if (strncmp(line, "RAW ", 4) == 0) {
            if (!cur) { fclose(fp); return false; }
            char p[2048];
            if (sscanf(line, "RAW %2047s", p) == 1) cur->src = p;
        }
    }
    fclose(fp);
    return true;
}
