// rfs_freeruns.cpp — RavenFS v2.0 **空闲区间（free run）dump**（只读）
//
// 目的：按磁盘契约读 block bitmap，枚举**连续空闲块区间**（bit=0 的极大 run），
//       给出「总量 / 区间数 / 最大区间 / Top-N / 长度直方图」——
//       用于调研「是否存在超大连续空闲空间可回收/消化碎片」。
//
// 契约依据（v2.0，与 mkfs/audit 逐式一致）：
//   · FS_block = 4096 B；FS_block[b] 字节偏移 = (world_base_lba + b) * lba_size
//     （world_base_lba = 4096 / lba_size ⇒ 恒等于 4096*(b+1)，见 mkfs/main.cpp:161/199）
//   · Primary SB @ 字节 0，4096B，magic[0]=RFS_SB_MAGIC_W0，version=RAVENFS_VERSION
//   · 几何推导：bitmap_blocks = ceil(total_blocks / 32768)
//               bitmap_start  = total_blocks - bitmap_blocks
//     块位图位于 bitmap_start，尺寸 bitmap_blocks*4096 B（mkfs_v20.cpp:129）
//   · 位序：LSB-first（block i → byte[i/8] 的 bit[i%8]）；**1 = 占用，0 = 空闲**
//     （mkfs_v20.cpp:90 bm_mark_range；元数据/位图自身/backup/尾部 pad 全置 1）
//
// 纪律：**只读**（O_RDONLY，无任何写路径）；日志走 stdout。
//
// 编译： g++ -std=c++17 -Wall -Wextra -O2 -I../../include rfs_freeruns.cpp -o rfs_freeruns
// 运行： ./rfs_freeruns <image-or-device> [--top N] [--min-blocks N] [--json] [--all]
// 退出码： 0=正常 2=用法/IO/契约错误

#include <disk_struct_v2.0.h>
#include <magics.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#ifndef BLKSSZGET
#define BLKSSZGET _IOR(0x12, 104, int)
#endif
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

static constexpr uint64_t FS_BLOCK = 4096;
/* v2.0 低块确定性布局（mkfs_v20.cpp）
 *   [0,2) 根 dentry / [2,66) inode 面 direct 段 / [66,67) 扁 fr → META_END = 66+1 = 67
 *   （注：mkfs_v20.cpp 头部注释写 [66,68)/“68”，与 FR_BLOCKS=1 的实现不符，此处以实现为准） */
static constexpr uint64_t META_END = 67;   /* = FR_BLOCK(66) + FR_BLOCKS(1) */

struct Run { uint64_t start; uint64_t len; };

static const char *g_prog = "rfs_freeruns";

static void die(const char *msg) {
    fprintf(stderr, "%s: error: %s\n", g_prog, msg);
    exit(2);
}

static void usage(void) {
    fprintf(stderr,
        "usage: %s <image-or-device> [options]\n"
        "  --top N         show N largest free runs (default 20; 0 = all)\n"
        "  --min-blocks N  only list runs with len >= N blocks (default 1)\n"
        "  --all           list every free run (implies --top 0 --min-blocks 1)\n"
        "  --json          machine-readable output\n"
        "  --quiet         summary only (no run list / no histogram)\n"
        "\n"
        "Reads the v2.0 block bitmap and dumps contiguous free (bit=0) runs.\n"
        "Read-only; block = 4096 B; bitmap = geometric tail region.\n",
        g_prog);
}

/* pread n bytes at off, retry on short read */
static void pread_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, (off_t)(off + done));
        if (r < 0) die("pread failed");
        if (r == 0) die("unexpected EOF while reading device/image");
        done += (size_t)r;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *path = argv[1];

    uint64_t top = 20, min_blocks = 1;
    bool json = false, quiet = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](void) -> const char* {
            if (i + 1 >= argc) die("missing option value");
            return argv[++i];
        };
        if (a == "--top")            top = strtoull(need(), nullptr, 0);
        else if (a == "--min-blocks") min_blocks = strtoull(need(), nullptr, 0);
        else if (a == "--all")       { top = 0; min_blocks = 1; }
        else if (a == "--json")      json = true;
        else if (a == "--quiet")     quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "%s: unknown option '%s'\n", g_prog, a.c_str()); usage(); return 2; }
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 2; }

    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat failed");
    bool is_blk = S_ISBLK(st.st_mode);
    uint64_t dev_bytes = (uint64_t)st.st_size;
    if (is_blk) {
        uint64_t sz = 0;
        if (ioctl(fd, BLKGETSIZE64, &sz) == 0) dev_bytes = sz;
    }
    uint64_t lba_size = FS_BLOCK;
    if (is_blk) {
        int ss = 0;
        if (ioctl(fd, BLKSSZGET, &ss) == 0 && ss > 0) lba_size = (uint64_t)ss;
    }

    /* ── Primary Superblock @ byte 0 ── */
    RavenSuperblock sb;
    pread_full(fd, &sb, sizeof(sb), 0);
    if (sb.magic[0] != RFS_SB_MAGIC_W0) {
        fprintf(stderr, "%s: error: bad superblock magic (0x%016" PRIx64 " != 0x%016" PRIx64 ")\n",
                g_prog, (uint64_t)sb.magic[0], (uint64_t)RFS_SB_MAGIC_W0);
        close(fd);
        return 2;
    }
    if (sb.version != RAVENFS_VERSION) {
        fprintf(stderr, "%s: error: not a v2.0 volume (version=%u)\n", g_prog, sb.version);
        close(fd);
        return 2;
    }

    const uint64_t total_blocks = sb.total_blocks;
    const uint64_t world_base_lba = sb.world_base_lba;
    if (total_blocks == 0) die("total_blocks == 0 (corrupt SB)");

    const uint64_t base_byte = world_base_lba * lba_size;   /* FS_block[0] 的字节偏移 */
    /* FS_block[b] 字节偏移 = base_byte + b*4096 */

    /* ── 几何：bitmap = 尾部推导区间 ── */
    const uint64_t bitmap_blocks = (total_blocks + 32767) / 32768;
    const uint64_t bitmap_start  = total_blocks - bitmap_blocks;
    const uint64_t bm_bytes      = bitmap_blocks * FS_BLOCK;

    std::vector<uint8_t> bm(bm_bytes);
    pread_full(fd, bm.data(), bm_bytes, base_byte + bitmap_start * FS_BLOCK);

    /* ── 枚举空闲 run（bit=0），仅扫 [0, total_blocks) ── */
    std::vector<Run> runs;
    uint64_t total_free = 0;
    uint64_t cur_start = 0;
    bool cur = false;
    auto close_run = [&](uint64_t end) {
        if (cur) { runs.push_back({cur_start, end - cur_start}); total_free += end - cur_start; cur = false; }
    };
    for (uint64_t i = 0; i < total_blocks; i++) {
        bool used = (bm[i >> 3] >> (i & 7)) & 1u;
        if (!used) {
            if (!cur) { cur = true; cur_start = i; }
        } else {
            if (cur) close_run(i);
        }
    }
    close_run(total_blocks);

    uint64_t max_run = 0, max_start = 0, max_end = 0;
    for (const Run &r : runs) if (r.len > max_run) { max_run = r.len; max_start = r.start; max_end = r.start + r.len; }

    if (json) {
        printf("{\n");
        printf("  \"path\": \"%s\",\n", path);
        printf("  \"block_bytes\": %" PRIu64 ",\n", FS_BLOCK);
        printf("  \"world_base_lba\": %" PRIu64 ",\n", world_base_lba);
        printf("  \"lba_size\": %" PRIu64 ",\n", lba_size);
        printf("  \"base_byte\": %" PRIu64 ",\n", base_byte);
        printf("  \"total_blocks\": %" PRIu64 ",\n", total_blocks);
        printf("  \"bitmap_start\": %" PRIu64 ",\n", bitmap_start);
        printf("  \"bitmap_blocks\": %" PRIu64 ",\n", bitmap_blocks);
        printf("  \"total_free_blocks\": %" PRIu64 ",\n", total_free);
        printf("  \"total_free_bytes\": %" PRIu64 ",\n", total_free * FS_BLOCK);
        printf("  \"free_runs\": %zu,\n", runs.size());
        printf("  \"max_run_blocks\": %" PRIu64 ",\n", max_run);
        printf("  \"max_run_start\": %" PRIu64 ",\n", max_start);
        printf("  \"runs\": [");
        size_t shown = 0;
        std::vector<Run> sorted = runs;
        std::sort(sorted.begin(), sorted.end(), [](const Run &a, const Run &b){ return a.len > b.len; });
        for (const Run &r : sorted) {
            if (r.len < min_blocks) continue;
            if (top && shown >= top) break;
            printf("%s\n    {\"start\": %" PRIu64 ", \"len\": %" PRIu64 "}", shown ? "," : "", r.start, r.len);
            shown++;
        }
        printf("%s],\n", shown ? "\n  " : "");
        printf("  \"listed\": %zu\n", shown);
        printf("}\n");
        close(fd);
        return 0;
    }

    /* ── 文本输出 ── */
    const double gb = (double)FS_BLOCK / (1024.0 * 1024.0 * 1024.0);
    printf("RavenFS v2.0 free-interval dump (read-only)\n");
    printf("  source        : %s%s\n", path, is_blk ? " (block device)" : " (image)");
    printf("  device bytes  : %" PRIu64 " (%.2f GiB)\n", dev_bytes, (double)dev_bytes / (1024.0*1024.0*1024.0));
    printf("  block / lba   : %" PRIu64 " B / %" PRIu64 " B\n", FS_BLOCK, lba_size);
    printf("  world_base_lba: %" PRIu64 "   FS_block[0] @ byte %" PRIu64 "\n", world_base_lba, base_byte);
    printf("  total_blocks  : %" PRIu64 " (%.2f GiB of FS world)\n",
           total_blocks, (double)total_blocks * gb);
    printf("  bitmap        : [%" PRIu64 ", %" PRIu64 ")  %" PRIu64 " blocks (%.2f MiB)\n",
           bitmap_start, bitmap_start + bitmap_blocks, bitmap_blocks,
           (double)bm_bytes / (1024.0 * 1024.0));
    printf("  meta reserved : [0, %" PRIu64 ")  (dentry/plane/fr; 1 = used)\n", META_END);
    printf("\n");
    printf("  total free    : %" PRIu64 " blocks = %.3f GiB\n", total_free, (double)total_free * gb);
    printf("  free ratio    : %.4f%%\n", 100.0 * (double)total_free / (double)total_blocks);
    printf("  free runs     : %zu   (contiguous 0-bit intervals)\n", runs.size());
    printf("  largest run   : [%" PRIu64 ", %" PRIu64 ")  %" PRIu64 " blocks = %.3f GiB\n",
           max_start, max_end, max_run, (double)max_run * gb);
    if (total_free) {
        printf("  mean run len  : %.2f blocks   (max/mean = %.1fx)\n",
               (double)total_free / (double)runs.size(),
               (double)max_run / ((double)total_free / (double)runs.size()));
    }
    printf("\n");

    if (!quiet) {
        std::vector<Run> sorted = runs;
        std::sort(sorted.begin(), sorted.end(), [](const Run &a, const Run &b){ return a.len > b.len; });

        printf("  ── free runs by length%s ──\n", top ? " (top N)" : " (all)");
        printf("   %-5s %-14s %-14s %-12s %s\n", "#", "start_blk", "end_blk", "blocks", "size");
        size_t shown = 0;
        for (const Run &r : sorted) {
            if (r.len < min_blocks) continue;
            if (top && shown >= top) break;
            printf("   %-5zu %-14" PRIu64 " %-14" PRIu64 " %-12" PRIu64 " %.3f GiB\n",
                   shown + 1, r.start, r.start + r.len, r.len, (double)r.len * gb);
            shown++;
        }
        uint64_t ge = 0; for (const Run &r : sorted) if (r.len >= min_blocks) ge++;
        if (shown < ge) printf("   ... (%" PRIu64 " more runs >= %" PRIu64 " blocks)\n", ge - shown, min_blocks);
        printf("\n");

        /* 直方图：log2 桶（长度 1..） */
        printf("  ── free-run length histogram (log2 buckets) ──\n");
        const int MAXB = 40;
        uint64_t cnt[MAXB], blks[MAXB];
        memset(cnt, 0, sizeof(cnt)); memset(blks, 0, sizeof(blks));
        for (const Run &r : runs) {
            int b = 0; uint64_t v = r.len; while (v >>= 1) b++;
            if (b >= MAXB) b = MAXB - 1;
            cnt[b]++; blks[b] += r.len;
        }
        printf("   %-22s %-10s %-14s %s\n", "len range", "runs", "blocks", "of free");
        for (int b = 0; b < MAXB; b++) {
            if (!cnt[b]) continue;
            uint64_t lo = 1ULL << b;
            uint64_t hi = (b + 1 >= 64) ? ~0ULL : ((1ULL << (b + 1)) - 1);
            printf("   [%-9" PRIu64 " .. %-9" PRIu64 "] %-10" PRIu64 " %-14" PRIu64 " %.2f%%\n",
                   lo, hi, cnt[b], blks[b], total_free ? 100.0 * (double)blks[b] / (double)total_free : 0.0);
        }
    }

    close(fd);
    return 0;
}
