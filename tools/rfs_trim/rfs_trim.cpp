/* ═══════════════════════════════════════════════════════════════════════════
 * rfs_trim — RavenFS 离线裁剪（trim/discard）工具（host 侧，不挂载）
 *
 * 动机（2026-09-30）：RavenFS 跑在 thin-provisioning 卷 / 稀疏镜像 / SSD 上时，
 * 空闲 FS_block 仍占着底层物理空间。本工具在 **FS 卸载后**，按 **block 位图**
 * 机械地把空闲 FS_block 区间裁掉还给底层：
 *   · 块设备   → ioctl(BLKDISCARD)（内核 TRIM/discard，仅完整 discard 单元）
 *   · 镜像文件 → fallocate(PUNCH_HOLE|KEEP_SIZE)（真打洞 → 稀疏文件）
 *
 * 分工（前提）：「位图与文件树是否一致」属 audit（tools/audit_v1.2）辖区——本工具
 * **假定 audit 通过**，只做机械执行；SB 解析 / 派生布局公式照抄 tools/sbinfo
 * （mkfs calc_geometry 同式），自包含、不复用 god 库、不挂载。
 *
 * 安全闸（跑前必过，不过就拒）：
 *   block 位图 bit [inode_bm_base, total_blocks) 必须**全 1**——自描述三段
 *   （inode 位图 / inode 数组 / block 位图自身；mkfs format_fs 恒 mark_range），
 *   出现 0 即位图损坏或几何不可信（同 mkfs RC_ERR_BITMAP_SELF 判据）→ 拒跑。
 *   过闸后只统计 [0, total_blocks) 的 0 游程（全 1 尾巴保证游程都在 inode_bm_base 之下）。
 *
 * 对齐纪律：只裁**完整 discard 单元**。块设备对齐取
 *   /sys/block/<dev>/queue/{discard_granularity,discard_alignment}
 *   （拿不到保守 4096/0），按 queue/max_discard_bytes（缺省 ≤1MiB）切批；
 *   镜像文件恒 4096/0。纯计算（位图 → 按 granularity 对齐的段列表）抽成
 *   plan_trim() 纯函数（--selftest-plan 可单测，不碰盘）。
 *
 * 纪律：默认 **dry-run**（O_RDONLY + 只报告），--apply 才写；stdout = 报告，
 *      stderr = 致命错误；退出码 0 = 成功，1 = 校验拒跑 / 裁剪失败，2 = 用法 / IO。
 *
 * 编译： g++ -std=c++17 -Wall -Wextra -O2 -g -I../../include rfs_trim.cpp -o rfs_trim
 * 运行： ./rfs_trim <image-or-device>            # DRY-RUN（默认，只报告）
 *       ./rfs_trim <image-or-device> --apply    # 真裁剪（FS 必须已卸载！）
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <disk_struct_v1.1.hpp>
#include <magics.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>

#ifndef BLKGETSIZE64
#  define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#  define BLKSSZGET _IO(0x12, 104)
#endif
#ifndef BLKDISCARD
#  define BLKDISCARD _IO(0x12, 119)
#endif
#ifndef FALLOC_FL_KEEP_SIZE
#  define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#  define FALLOC_FL_PUNCH_HOLE 0x02
#endif

static constexpr uint64_t FS_BLOCK_BYTES       = 4096;
static constexpr uint64_t DEFAULT_MAX_DISCARD  = 1ull << 20;   /* 1MiB 缺省批 */
static constexpr uint32_t EXT4_SB_MAGIC        = 0xEF53;       /* ext 系 sbi_magic @ 0x438 */
static constexpr off_t    EXT4_MAGIC_OFF       = 0x438;

/* ─────────────────────────── 报告器（sbinfo 同款三档） ─────────────────────────── */
static int n_ok = 0, n_warn = 0;
static void report(const char *tag, const char *fmt, va_list ap) {
    printf("%-5s ", tag);
    vprintf(fmt, ap);
    printf("\n");
}
static void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ok(const char *fmt, ...)   { n_ok++;  va_list ap; va_start(ap, fmt); report("OK", fmt, ap);   va_end(ap); }
static void warn(const char *fmt, ...) { n_warn++; va_list ap; va_start(ap, fmt); report("WARN", fmt, ap); va_end(ap); }
static void bad(const char *fmt, ...)           { va_list ap; va_start(ap, fmt); report("BAD", fmt, ap);  va_end(ap); }

static std::string fmt_bytes(uint64_t bytes) {
    static const char *unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    char buf[64];
    snprintf(buf, sizeof buf, "%.2f %s", v, unit[u]);
    return buf;
}

static bool pread_full(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; off += r; n -= (size_t)r;
    }
    return true;
}

/* ═══════════════════ 派生几何（mkfs calc_geometry / sbinfo 同式） ═══════════════════ */
struct Geo {
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t bitmap_blocks;      /* block 位图块数 = ceil(total/32768) */
    uint64_t bitmap_start;       /* block 位图 = [bitmap_start, total) */
    uint64_t inode_blocks;       /* inode 数组块数 = ceil(ic*256/4096) */
    uint64_t inode_array_base;   /* inode 数组 = [inode_array_base, bitmap_start) */
    uint64_t inode_bm_blocks;    /* inode 位图块数 = ceil(ic/32768) */
    uint64_t inode_bm_base;      /* inode 位图 = [inode_bm_base, inode_array_base) */
};

static bool calc_geometry(uint64_t total, uint64_t ic, Geo *g, const char **why) {
    if (total == 0 || ic == 0) { *why = "total_blocks/inode_array_count 为 0，几何不可信"; return false; }
    g->total_blocks   = total;
    g->inode_count    = ic;
    g->bitmap_blocks  = (total + 32767) / 32768;
    g->bitmap_start   = total - g->bitmap_blocks;
    g->inode_blocks   = (ic * 256 + 4095) / 4096;
    if (g->inode_blocks > g->bitmap_start) { *why = "inode 数组比 block 位图起点还大，设备太小"; return false; }
    g->inode_array_base = g->bitmap_start - g->inode_blocks;
    g->inode_bm_blocks  = (ic + 32767) / 32768;
    if (g->inode_bm_blocks > g->inode_array_base) { *why = "inode 位图比 inode 数组起点还大，设备太小"; return false; }
    g->inode_bm_base = g->inode_array_base - g->inode_bm_blocks;
    if (g->inode_bm_base <= 1) { *why = "inode_bitmap_base ≤ 1，几何非法（设备太小？）"; return false; }
    return true;
}

/* ═══════════════════ 纯函数：位图 → 按 discard 单元对齐的裁剪段列表 ═══════════════════
 * 输入：bmp（block 位图原字节，LSB-first：bit i = bmp[i/8] >> (i%8) & 1，同 mkfs/god）、
 *      nbits（有效位数 = total_blocks）、world_base（FS_block[0] 的绝对字节偏移）、
 *      gran/align（discard 粒度与对齐，绝对字节空间）。
 * 输出：TrimPlan——逻辑空闲统计 + 只含**完整 discard 单元**的段列表（绝对字节）。
 * 纯计算：不碰盘、不碰全局状态，可在 --selftest-plan 里离线单测。 */
struct TrimRange {
    uint64_t start = 0;   /* 绝对字节 */
    uint64_t len   = 0;
};
struct TrimPlan {
    uint64_t free_blocks      = 0;   /* [0,nbits) 内 0 的个数 */
    uint64_t free_bytes       = 0;   /* = free_blocks * 4096 */
    uint64_t raw_segments     = 0;   /* 原始 0 游程段数 */
    uint64_t trimmable_bytes  = 0;   /* 对齐后可裁字节 */
    uint64_t segments         = 0;   /* 对齐后段数 */
    std::vector<TrimRange> segs;
};

static uint64_t align_up_unit(uint64_t x, uint64_t gran, uint64_t align) {
    if (x <= align) return align;
    uint64_t r = (x - align) % gran;
    return r ? x + (gran - r) : x;
}
static uint64_t align_dn_unit(uint64_t x, uint64_t gran, uint64_t align) {
    if (x <= align) return align;
    return x - (x - align) % gran;
}

static TrimPlan plan_trim(const uint8_t *bmp, uint64_t nbits, uint64_t world_base,
                          uint64_t gran, uint64_t align) {
    TrimPlan p;
    if (gran == 0) gran = FS_BLOCK_BYTES;
    uint64_t i = 0;
    while (i < nbits) {
        if ((bmp[i >> 3] >> (i & 7)) & 1) { i++; continue; }
        uint64_t b0 = i;
        while (i < nbits && (((bmp[i >> 3] >> (i & 7)) & 1) == 0)) i++;
        uint64_t b1 = i;
        p.free_blocks   += b1 - b0;
        p.raw_segments  += 1;
        /* 0 游程 [b0,b1) → 绝对字节 → 只保留完整 discard 单元 */
        uint64_t rs = world_base + b0 * FS_BLOCK_BYTES;
        uint64_t re = world_base + b1 * FS_BLOCK_BYTES;
        uint64_t s  = align_up_unit(rs, gran, align);
        uint64_t e  = align_dn_unit(re, gran, align);
        if (s < e) {
            p.segs.push_back(TrimRange{s, e - s});
            p.trimmable_bytes += e - s;
        }
    }
    p.free_bytes  = p.free_blocks * FS_BLOCK_BYTES;
    p.segments    = (uint64_t)p.segs.size();
    return p;
}

/* ═══════════════════ --selftest-plan：纯函数离线单测（不碰盘） ═══════════════════ */
static int selftest_plan(void) {
    int fails = 0;
    auto chk = [&](bool cond, const char *msg) {
        if (cond) printf("ok    %s\n", msg);
        else      { printf("FAIL  %s\n", msg); fails++; }
    };

    /* 1) 全 1 位图 → 0 空闲 0 段 */
    {
        std::vector<uint8_t> b(4096, 0xFF);
        TrimPlan p = plan_trim(b.data(), 4096 * 8, 4096, 4096, 0);
        chk(p.free_bytes == 0 && p.segments == 0 && p.raw_segments == 0,
            "全 1 位图 → 空闲 0 / 段 0");
    }
    /* 2) 常规：world=4096, gran=4096 → 游程整段可裁（4096 对齐天然满足） */
    {
        std::vector<uint8_t> b(4096, 0xFF);
        for (uint64_t i = 8; i < 24; i++) b[i / 8] &= (uint8_t)~(1u << (i % 8));
        TrimPlan p = plan_trim(b.data(), 4096 * 8, 4096, 4096, 0);
        chk(p.free_blocks == 16 && p.free_bytes == 16 * 4096 &&
            p.raw_segments == 1 && p.segments == 1 &&
            p.trimmable_bytes == 16 * 4096 &&
            p.segs[0].start == 4096 + 8 * 4096 && p.segs[0].len == 16 * 4096,
            "gran=4096 → 16 blocks 游程完整保留 [4096+32768, +65536)");
    }
    /* 3) 粒度放大：gran=8192 → 两端各裁掉半格（对齐损耗 8192B） */
    {
        std::vector<uint8_t> b(4096, 0xFF);
        for (uint64_t i = 8; i < 24; i++) b[i / 8] &= (uint8_t)~(1u << (i % 8));
        TrimPlan p = plan_trim(b.data(), 4096 * 8, 4096, 8192, 0);
        /* raw [36864,102400) → [40960,98304) = 57344 */
        chk(p.free_bytes == 65536 && p.segments == 1 &&
            p.trimmable_bytes == 57344 &&
            p.segs[0].start == 40960 && p.segs[0].len == 57344,
            "gran=8192 → 段收窄至完整 8K 单元（57344 = 65536-8192 损耗）");
    }
    /* 4) 游程比粒度小 → 整段被对齐吃掉（只裁完整单元的铁律） */
    {
        std::vector<uint8_t> b(4096, 0xFF);
        for (uint64_t i = 0; i < 4; i++) b[i / 8] &= (uint8_t)~(1u << (i % 8));
        TrimPlan p = plan_trim(b.data(), 4096 * 8, 4096, 1ull << 20, 0);
        chk(p.free_bytes == 4 * 4096 && p.raw_segments == 1 &&
            p.segments == 0 && p.trimmable_bytes == 0,
            "gran=1MiB 且游程跨不进单元 → 可裁 0（拒裁部分单元）");
    }
    /* 5) world 基址不对齐：world=512, gran=4096 → 段两头裁齐 */
    {
        std::vector<uint8_t> b(4096, 0xFF);
        for (uint64_t i = 0; i < 2; i++) b[i / 8] &= (uint8_t)~(1u << (i % 8));
        TrimPlan p = plan_trim(b.data(), 4096 * 8, 512, 4096, 0);
        /* raw [512, 8704) → [4096, 8192) = 4096 */
        chk(p.free_bytes == 8192 && p.segments == 1 &&
            p.trimmable_bytes == 4096 &&
            p.segs[0].start == 4096 && p.segs[0].len == 4096,
            "world_base=512 → 两头对齐裁齐（8192 → 4096 可裁）");
    }
    printf("selftest-plan: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

/* ═══════════════════ 块设备 discard 参数（sysfs；拿不到保守 4096/0） ═══════════════════ */
struct DiscardCfg {
    bool     is_blk;
    uint64_t gran;        /* discard_granularity */
    uint64_t align;       /* discard_alignment */
    uint64_t max_batch;   /* 单次 discard 上限字节 */
    std::string queue_dir;
};

static bool sysfs_read_u64(const std::string &dir, const char *name, uint64_t *out) {
    if (dir.empty()) return false;
    std::string p = dir + "/" + name;
    FILE *f = fopen(p.c_str(), "r");
    if (!f) return false;
    unsigned long long v = 0;
    bool got = (fscanf(f, "%llu", &v) == 1);
    fclose(f);
    if (got) *out = (uint64_t)v;
    return got;
}

/* /sys/dev/block/<maj>:<min> → realpath → …/block/<dev>(/<part>)/queue；
 * 分区无自己的 queue 时退父目录（queue 参数盘/分区同值）。 */
static std::string find_queue_dir(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISBLK(st.st_mode)) return "";
    char link[64];
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u",
             major(st.st_rdev), minor(st.st_rdev));
    char real[PATH_MAX];
    if (!realpath(link, real)) return "";
    std::string p(real);
    if (access((p + "/queue").c_str(), R_OK) == 0) return p + "/queue";
    size_t slash = p.rfind('/');
    if (slash == std::string::npos) return "";
    std::string parent = p.substr(0, slash);
    if (access((parent + "/queue").c_str(), R_OK) == 0) return parent + "/queue";
    return "";
}

static DiscardCfg probe_discard(int fd, bool is_blk) {
    DiscardCfg c{};
    c.is_blk    = is_blk;
    c.gran      = FS_BLOCK_BYTES;          /* 保守缺省 4096 */
    c.align     = 0;
    c.max_batch = DEFAULT_MAX_DISCARD;     /* 缺省 ≤1MiB */
    if (!is_blk) return c;                 /* 镜像文件：恒 4096/0，fallocate 不切批 */
    c.queue_dir = find_queue_dir(fd);
    uint64_t v = 0;
    if (sysfs_read_u64(c.queue_dir, "discard_granularity", &v) && v > 0) c.gran = v;
    if (sysfs_read_u64(c.queue_dir, "discard_alignment",   &v))          c.align = v;
    if (sysfs_read_u64(c.queue_dir, "max_discard_bytes",   &v) && v > 0) c.max_batch = v;
    if (c.gran == 0) c.gran = FS_BLOCK_BYTES;
    /* 批必须是完整单元的整数倍，否则切批切出半个 discard 单元 */
    if (c.max_batch % c.gran) c.max_batch -= c.max_batch % c.gran;
    if (c.max_batch < c.gran) c.max_batch = c.gran;
    return c;
}

/* ═══════════════════ 挂载检查（--apply 前置；块设备按 maj:min，防 by-uuid 别名） ═══════════════════ */
static bool is_mounted(const char *path, const struct stat *st) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return false;
    bool found = false;
    char line[4096];
    while (!found && fgets(line, sizeof line, f)) {
        char src[512];
        if (sscanf(line, "%511s", src) != 1) continue;
        if (strcmp(src, path) == 0) { found = true; break; }
        struct stat ms;
        if (stat(src, &ms) == 0) {
            if (S_ISBLK(ms.st_mode) && ms.st_rdev == st->st_rdev) found = true;
            else if (S_ISREG(ms.st_mode) && S_ISREG(st->st_mode) &&
                     ms.st_dev == st->st_dev && ms.st_ino == st->st_ino) found = true;
        }
    }
    fclose(f);
    return found;
}

/* ═══════════════════ 裁剪执行（--apply；块设备 BLKDISCARD 切批 / 文件 punch） ═══════════════════
 * 返回失败批数（0 = 全部成功）。所有段/批均已在 plan_trim 里对齐过。 */
static uint64_t apply_plan(int wfd, const TrimPlan &plan, const DiscardCfg &cfg,
                           uint64_t dev_bytes, uint64_t *clamped_bytes) {
    const uint64_t printed_cap = 8;   /* 每段只打一行，失败必打，防刷屏 */
    uint64_t fails = 0, printed = 0;
    uint64_t n = (uint64_t)plan.segs.size();

    for (uint64_t si = 0; si < n; si++) {
        TrimRange r = plan.segs[si];
        uint64_t seg_start = r.start, seg_end = r.start + r.len;

        if (!cfg.is_blk && seg_end > dev_bytes) {   /* 几何 vs 文件尺寸不符：截到尾 */
            if (seg_start >= dev_bytes) continue;
            *clamped_bytes += seg_end - dev_bytes;
            seg_end = dev_bytes;
        }

        uint64_t done = 0;
        uint64_t pos  = seg_start;
        while (pos < seg_end) {
            uint64_t chunk = std::min(seg_end - pos, cfg.max_batch);
            if (cfg.is_blk && chunk % cfg.gran) chunk -= chunk % cfg.gran;
            if (chunk == 0) break;

            int rc;
            if (cfg.is_blk) {
                uint64_t args[2] = { pos, chunk };      /* {start, len} 绝对字节 */
                rc = ioctl(wfd, BLKDISCARD, args);
            } else {
                rc = fallocate(wfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                               (off_t)pos, (off_t)chunk);
            }
            if (rc == 0) { done += chunk; pos += chunk; }
            else {
                fails++;
                fprintf(stderr, "error: 段 %llu [%llu, +%llu) 批 [%llu, +%llu) %s 失败: %s\n",
                        (unsigned long long)(si + 1),
                        (unsigned long long)seg_start, (unsigned long long)r.len,
                        (unsigned long long)pos, (unsigned long long)chunk,
                        cfg.is_blk ? "BLKDISCARD" : "punch_hole", strerror(errno));
                break;   /* 该段放弃，继续下一段 */
            }
        }

        if (printed < printed_cap) {
            printf("apply 段 %llu/%llu: [%llu, +%llu) → %s 掉 %llu B%s\n",
                   (unsigned long long)(si + 1), (unsigned long long)n,
                   (unsigned long long)seg_start, (unsigned long long)r.len,
                   cfg.is_blk ? "discard" : "punch",
                   (unsigned long long)done,
                   done == r.len ? " ✓" : " ✗（部分）");
            printed++;
        } else if (printed == printed_cap && si + 1 == n) {
            printf("apply …（段数 %llu，后续段略）\n", (unsigned long long)n);
            printed++;
        }
    }
    return fails;
}

/* ═══════════════════════════════════ 用法 ═══════════════════════════════════ */
static void usage(FILE *out, const char *argv0) {
    fprintf(out,
        "usage: %s <image-or-device> [--apply]\n"
        "  RavenFS 离线裁剪：按 block 位图把空闲 FS_block 还给底层\n"
        "  默认 dry-run（只读只报告）；--apply 才执行（FS 必须已卸载）\n"
        "  其它：--selftest-plan（纯函数单测，不碰盘）  -h/--help\n"
        "  退出码：0 成功 · 1 校验拒跑/裁剪失败 · 2 用法/IO 错误\n",
        argv0);
}

/* ═══════════════════════════════════ main ═══════════════════════════════════ */
int main(int argc, char **argv) {
    const char *path = nullptr;
    bool apply = false;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--apply") == 0) apply = true;
        else if (strcmp(argv[i], "--selftest-plan") == 0) return selftest_plan();
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]); return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: 未知选项 %s\n", argv[i]);
            usage(stderr, argv[0]); return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(stderr, argv[0]); return 2;
        }
    }
    if (!path) { usage(stderr, argv[0]); return 2; }

    /* ── 1. 判型打开（读用 O_RDONLY；--apply 的写 fd 后开，保证 dry-run 零写） ── */
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) { fprintf(stderr, "error: 打开 %s: %s\n", path, strerror(errno)); return 2; }
    struct stat st;
    if (fstat(rfd, &st) < 0) { perror("fstat"); close(rfd); return 2; }
    const bool is_blk = S_ISBLK(st.st_mode);
    if (!is_blk && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: %s 既不是块设备也不是普通文件\n", path);
        close(rfd); return 2;
    }

    /* ── 2. 设备尺寸 + LBA（块设备 BLKSSZGET，失败回退 4096；文件恒 4096） ── */
    uint64_t dev_bytes = 0;
    uint64_t lba_size  = FS_BLOCK_BYTES;
    if (is_blk) {
        if (ioctl(rfd, BLKGETSIZE64, &dev_bytes) < 0) {
            fprintf(stderr, "error: BLKGETSIZE64 %s: %s\n", path, strerror(errno));
            close(rfd); return 2;
        }
        int lba = 0;
        if (ioctl(rfd, BLKSSZGET, &lba) == 0 && lba > 0) lba_size = (uint64_t)lba;
        else warn("BLKSSZGET 失败，LBA 按 4096 回退（world 基址换算可能不准）");
    } else {
        dev_bytes = (uint64_t)st.st_size;   /* 镜像文件 LBA 恒 4096（mkfs 4096 契约） */
    }

    /* ── 3. 读 SB（LBA0 = 字节 0，4096B）：magic 校验 + 核心字段 ── */
    std::vector<uint8_t> raw(FS_BLOCK_BYTES);
    if (!pread_full(rfd, raw.data(), FS_BLOCK_BYTES, 0)) {
        fprintf(stderr, "error: 读取 superblock 失败\n");
        close(rfd); return 2;
    }
    RavenSuperblock sb;
    memcpy(&sb, raw.data(), sizeof sb);

    printf("=== rfs_trim — RavenFS 离线裁剪（%s） : %s ===\n",
           apply ? "APPLY" : "DRY-RUN", path);

    if (sb.magic != SUPERBLOCK_MAGIC) {
        bad("magic = %#llx，不是 \"RAVN\" (%#llx)",
            (unsigned long long)sb.magic, (unsigned long long)SUPERBLOCK_MAGIC);
        uint16_t ext_magic = 0;
        if (pread_full(rfd, &ext_magic, 2, EXT4_MAGIC_OFF) && ext_magic == EXT4_SB_MAGIC)
            printf("     → 偏移 0x438 检出 ext 系 magic 0x%04x：这盘是 ext2/3/4，不是 RavenFS\n",
                   EXT4_SB_MAGIC);
        close(rfd); return 1;
    }
    if (sb.version != 2 && sb.version != 3) {
        bad("version = %u，既非 2(v1.1) 也非 3(v1.2)，SB 布局无保证 → 拒跑", sb.version);
        close(rfd); return 1;
    }

    const uint64_t world_base = sb.world_base_lba * lba_size;
    if (world_base != FS_BLOCK_BYTES)
        warn("world_base_lba*lba = %llu，偏离 mkfs 4096 契约（SB 应恰占 world 之外的首 4K）",
             (unsigned long long)world_base);

    /* 溢出护栏：world 末端必须可表示 */
    if (sb.total_blocks > (UINT64_MAX - world_base) / FS_BLOCK_BYTES) {
        bad("total_blocks=%llu 溢出 world 末端 → 几何不可信，拒跑",
            (unsigned long long)sb.total_blocks);
        close(rfd); return 1;
    }
    const uint64_t world_end = world_base + sb.total_blocks * FS_BLOCK_BYTES;

    printf("device: %s, %llu bytes (≈ %s), lba=%llu | world_base_lba=%llu → 基址 %llu\n",
           is_blk ? "block" : "file",
           (unsigned long long)dev_bytes, fmt_bytes(dev_bytes).c_str(),
           (unsigned long long)lba_size,
           (unsigned long long)sb.world_base_lba, (unsigned long long)world_base);
    printf("SB: magic \"RAVN\" ✓ version=%u total_blocks=%llu inode_array_count=%llu\n",
           sb.version, (unsigned long long)sb.total_blocks,
           (unsigned long long)sb.inode_array_count);

    if (world_end > dev_bytes) {
        bad("world 末端 %llu > 设备 %llu（SB 与盘尺寸不符）→ 拒跑",
            (unsigned long long)world_end, (unsigned long long)dev_bytes);
        close(rfd); return 1;
    }
    if (world_end < dev_bytes)
        warn("world 末端 %llu < 设备 %llu（尾部 %llu B 不在 world 内）",
             (unsigned long long)world_end, (unsigned long long)dev_bytes,
             (unsigned long long)(dev_bytes - world_end));

    /* ── 4. 派生布局（mkfs calc_geometry 同式） ── */
    Geo geo{};
    const char *why = "";
    if (!calc_geometry(sb.total_blocks, sb.inode_array_count, &geo, &why)) {
        bad("%s → 拒跑", why);
        close(rfd); return 1;
    }
    printf("布局: block 位图=[%llu,%llu) inode 数组=[%llu,%llu) inode 位图=[%llu,%llu)\n",
           (unsigned long long)geo.bitmap_start, (unsigned long long)geo.total_blocks,
           (unsigned long long)geo.inode_array_base, (unsigned long long)geo.bitmap_start,
           (unsigned long long)geo.inode_bm_base, (unsigned long long)geo.inode_array_base);

    /* ── 5. 读 block 位图：byte = world_base_lba*lba + bitmap_start*4096 ── */
    const off_t bm_off = (off_t)(world_base + geo.bitmap_start * FS_BLOCK_BYTES);
    const size_t bm_len = (size_t)geo.bitmap_blocks * (size_t)FS_BLOCK_BYTES;
    std::vector<uint8_t> bmp(bm_len);
    if (!pread_full(rfd, bmp.data(), bm_len, bm_off)) {
        fprintf(stderr, "error: 读取 block 位图失败（off=%llu, len=%zu）\n",
                (unsigned long long)bm_off, bm_len);
        close(rfd); return 2;
    }

    /* ── 6. 安全前置：自描述三段 bit [inode_bm_base, total) 必须全 1 ── */
    for (uint64_t b = geo.inode_bm_base; b < geo.total_blocks; b++) {
        if (((bmp[b >> 3] >> (b & 7)) & 1) == 0) {
            bad("block 位图自描述三段 [%llu, %llu) 非全 1（首个 0 = block %llu）"
                "→ 位图/几何不可信，拒跑",
                (unsigned long long)geo.inode_bm_base,
                (unsigned long long)geo.total_blocks, (unsigned long long)b);
            close(rfd); return 1;
        }
    }
    ok("自描述三段 [%llu, %llu) 共 %llu bit 全 1（inode 位图/数组/block 位图自占）",
       (unsigned long long)geo.inode_bm_base, (unsigned long long)geo.total_blocks,
       (unsigned long long)(geo.total_blocks - geo.inode_bm_base));

    /* ── 7. discard 参数 + 纯函数规划 ── */
    DiscardCfg cfg = probe_discard(rfd, is_blk);
    if (cfg.is_blk)
        printf("discard: gran=%llu align=%llu max_batch=%llu（%s）\n",
               (unsigned long long)cfg.gran, (unsigned long long)cfg.align,
               (unsigned long long)cfg.max_batch,
               cfg.queue_dir.empty() ? "sysfs 不可得，保守缺省 4096/0/1MiB" : cfg.queue_dir.c_str());
    else
        printf("discard: 镜像文件 → fallocate(PUNCH_HOLE|KEEP_SIZE)，gran=4096 align=0\n");

    TrimPlan plan = plan_trim(bmp.data(), geo.total_blocks, world_base, cfg.gran, cfg.align);

    /* ── 8. 裁剪报告 ── */
    printf("\n-- 裁剪报告 --\n");
    printf("FS 覆盖字节      : %llu (%llu blocks × 4096B)\n",
           (unsigned long long)(geo.total_blocks * FS_BLOCK_BYTES),
           (unsigned long long)geo.total_blocks);
    printf("逻辑空闲字节     : %llu (%llu blocks)\n",
           (unsigned long long)plan.free_bytes, (unsigned long long)plan.free_blocks);
    printf("对齐后可裁字节   : %llu\n", (unsigned long long)plan.trimmable_bytes);
    double loss = plan.free_bytes ?
        100.0 * (double)(plan.free_bytes - plan.trimmable_bytes) / (double)plan.free_bytes : 0.0;
    printf("损耗比           : %.2f%%（逻辑空闲中被 discard 对齐吃掉、裁不动的部分）\n", loss);
    printf("段数             : 原始 %llu → 对齐后 %llu\n",
           (unsigned long long)plan.raw_segments, (unsigned long long)plan.segments);

    if (!apply) {
        printf("\n[dry-run] 只报告未动手；加 --apply 才执行裁剪。\n");
        printf("\nchecks: %d ok, %d warn\n", n_ok, n_warn);
        close(rfd);
        return 0;
    }

    /* ── 9. --apply：卸载检查 → 写 fd → 执行 ── */
    if (is_mounted(path, &st)) {
        bad("%s 仍在 /proc/mounts 里（挂载中）→ 双写者风险，先 umount 再 --apply", path);
        close(rfd); return 1;
    }
    int wfd = open(path, O_RDWR);
    if (wfd < 0) {
        fprintf(stderr, "error: --apply 需要可写打开 %s: %s\n", path, strerror(errno));
        close(rfd); return 2;
    }

    if (plan.segments == 0) {
        printf("\napply: 无可裁剪段（空闲为 0 或全部被对齐吃掉）\n");
        close(wfd); close(rfd);
        return 0;
    }

    printf("\n-- apply（%s） --\n", cfg.is_blk ? "BLKDISCARD" : "fallocate punch_hole");
    uint64_t clamped = 0;
    uint64_t fails = apply_plan(wfd, plan, cfg, dev_bytes, &clamped);
    if (clamped)
        warn("有 %llu B 超出设备尾部被截去（world 与盘尺寸不符）",
             (unsigned long long)clamped);
    fsync(wfd);

    printf("\napply: 裁掉 %llu B / %llu 段 / 失败 %llu 批\n",
           (unsigned long long)(plan.trimmable_bytes - clamped),
           (unsigned long long)plan.segments, (unsigned long long)fails);
    printf("checks: %d ok, %d warn\n", n_ok, n_warn);

    close(wfd);
    close(rfd);
    return fails ? 1 : 0;
}
