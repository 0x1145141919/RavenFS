// bitmap_hotspot.cpp — RavenFS 双位图分片热点可视化（只读）
//
// 目的：验证 03 设计假设「分配推进集中在每片 hint 邻域（顺序局部性）」——
//       把块位图 / inode 位图按 分片(slice) × chunk 两级切格，逐格 popcount
//       出置位密度，终端字符热图 + 可选 SVG 输出，并给出集中度量化指标。
//
// 依据：Docs/runtime_tech/03_chunk_backed_slices.md
//       · chunk_bits = 32768 << order   （order=4 → 64K 字节 → 524288 位）
//       · slice_bits = 32768 × k        （默认 16M 位 = 32768×512；块位图 32 片）
//   几何/解析与 tools/audit_v1.2 逐式一致（mkfs calc_geometry）。
//
// 纪律：只读（O_RDONLY，无任何写路径）；日志 stdout，SVG 经 --svg 显式指定。
//
// 编译： g++ -Wall -Wextra -O2 -Iinclude tools/bitmap_hotspot.cpp -o bitmap_hotspot
// 运行： ./bitmap_hotspot <image-or-device> [--order N] [--slice-bits N]
//                        [--cells N] [--top N] [--color] [--svg out.svg]
// 退出码： 0=正常 2=用法/IO 错误

#include <disk_struct_v1.2.h>
#include <magics.h>

#include <algorithm>
#include <cmath>
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

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

static constexpr uint64_t FS_BLOCK_BYTES = 4096;

/* ── CRC32（同 mkfs/audit：反射 0xEDB88320） ── */
static uint32_t crc32_tab[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_tab[i] = c;
    }
}
static uint32_t crc32_calc(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc32_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static inline uint64_t blkoff(uint64_t b) { return FS_BLOCK_BYTES * (b + 1); }

static bool pread_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

static uint64_t popcount_buf(const uint8_t *p, size_t nbytes) {
    uint64_t s = 0;
    size_t i = 0;
    for (; i + 8 <= nbytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        s += (uint64_t)__builtin_popcountll(w);
    }
    for (; i < nbytes; i++) s += (uint64_t)__builtin_popcount(p[i]);
    return s;
}

/* 密度 → 档位字符（0..9），按本图 max 自动拉伸，凸显局部热点 */
static const char RAMP[] = " .:-=+*#%@";
static int ramp_char(double d) {
    if (d < 0) d = 0;
    if (d > 1) d = 1;
    int k = (int)(d * 9.999);
    return k > 9 ? 9 : k;
}

/* SVG 配色：冷 #16202f → 中 #f0a030 → 热 #e03030 */
static void svg_color(double t, char *out) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double r, g, b;
    if (t < 0.5) { double u = t * 2;
        r = 0x16 + u * (0xf0 - 0x16); g = 0x20 + u * (0xa0 - 0x20); b = 0x2f + u * (0x30 - 0x2f);
    } else { double u = (t - 0.5) * 2;
        r = 0xf0 + u * (0xe0 - 0xf0); g = 0xa0 + u * (0x30 - 0xa0); b = 0x30 + u * (0x30 - 0x30);
    }
    sprintf(out, "#%02x%02x%02x", (int)r, (int)g, (int)b);
}

/* ── 单个位图的扫描与呈现 ── */
struct BitmapView {
    const char *name;
    uint64_t total_bits;
    uint64_t dev_base;          /* 位图区域设备字节偏移（blkoff(base)） */
    uint64_t chunk_bits;
    uint64_t slice_cells;       /* 每片多少 chunk（≥1） */
    std::vector<uint64_t> chunk_set;
    std::vector<uint64_t> chunk_tot;
};

static bool scan_bitmap(int fd, BitmapView &v) {
    uint64_t nchunks = (v.total_bits + v.chunk_bits - 1) / v.chunk_bits;
    v.chunk_set.assign(nchunks, 0);
    v.chunk_tot.assign(nchunks, 0);

    std::vector<uint8_t> buf;
    for (uint64_t c = 0; c < nchunks; c++) {
        uint64_t bit_lo = c * v.chunk_bits;
        uint64_t bit_hi = std::min(v.total_bits, bit_lo + v.chunk_bits);
        uint64_t byte_lo = bit_lo / 8;
        uint64_t byte_hi = (bit_hi + 7) / 8;
        buf.resize((size_t)(byte_hi - byte_lo));
        if (!pread_full(fd, buf.data(), buf.size(), v.dev_base + byte_lo)) {
            fprintf(stderr, "error: pread %s chunk %llu failed\n", v.name, (unsigned long long)c);
            return false;
        }
        uint64_t set = popcount_buf(buf.data(), buf.size());
        /* 尾部非整字节：屏蔽超出 total_bits 的位（chunk_bits 恒 4096 倍数 → 头部无需掩码） */
        if (bit_hi % 8) {
            uint64_t last = bit_hi / 8 - byte_lo;
            unsigned keep = (unsigned)(bit_hi % 8);
            uint8_t mask = (uint8_t)((1u << keep) - 1);
            set -= (uint64_t)__builtin_popcount(buf[last] & (uint8_t)~mask);
        }
        v.chunk_set[c] = set;
        v.chunk_tot[c] = bit_hi - bit_lo;
    }
    return true;
}

static void print_view(const BitmapView &v, int cells_per_row, int top_n, bool color) {
    uint64_t total_set = 0, n = v.chunk_set.size();
    double maxd = 0;
    for (uint64_t i = 0; i < n; i++) {
        total_set += v.chunk_set[i];
        double d = (double)v.chunk_set[i] / (double)v.chunk_tot[i];
        if (d > maxd) maxd = d;
    }
    double glob = (double)total_set / (double)v.total_bits;

    /* 集中度：top10% chunk 承载份额 + 变异系数 */
    std::vector<uint64_t> sorted(v.chunk_set.begin(), v.chunk_set.end());
    std::sort(sorted.begin(), sorted.end(), std::greater<uint64_t>());
    uint64_t k = std::max<uint64_t>(1, (n + 9) / 10);
    uint64_t top_set = 0;
    for (uint64_t i = 0; i < k && i < n; i++) top_set += sorted[i];
    double share = total_set ? (double)top_set / (double)total_set : 0;
    double mean = n ? (double)total_set / (double)n : 0, var = 0;
    if (n > 1 && mean > 0) {
        for (uint64_t i = 0; i < n; i++) {
            double d = (double)v.chunk_set[i] - mean;
            var += d * d;
        }
        var /= (double)(n - 1);
    }
    double cv = mean > 0 ? std::sqrt(var) / mean : 0;

    uint64_t slice_bits_eff = v.slice_cells * v.chunk_bits;
    uint64_t nslices = (v.total_bits + slice_bits_eff - 1) / slice_bits_eff;

    printf("\n── %s ──\n", v.name);
    printf("  bits=%llu  chunk=%llu位(%.0fK)  chunks=%llu  |  slice=%llu位(%llu cells)  slices=%llu\n",
           (unsigned long long)v.total_bits,
           (unsigned long long)v.chunk_bits, v.chunk_bits / 1024.0,
           (unsigned long long)n,
           (unsigned long long)slice_bits_eff, (unsigned long long)v.slice_cells,
           (unsigned long long)nslices);
    printf("  置位合计=%llu (%.3f%%)  格密度max=%.1f%%\n",
           (unsigned long long)total_set, glob * 100, maxd * 100);
    printf("  [集中度] top%.0f%%格承载 %.1f%% 置位（均匀=%.0f%%）  CV=%.2f\n",
           ceil(k * 100.0 / n), share * 100, ceil(k * 100.0 / n), cv);

    /* 分片表 */
    printf("  %-5s %-28s %12s %8s\n", "slice", "bit range", "set", "density");
    for (uint64_t s = 0; s < nslices; s++) {
        uint64_t lo = s * slice_bits_eff, hi = std::min(v.total_bits, lo + slice_bits_eff);
        uint64_t set = 0;
        for (uint64_t c = s * v.slice_cells; c < (s + 1) * v.slice_cells && c < n; c++)
            set += v.chunk_set[c];
        double d = (double)set / (double)(hi - lo);
        char bar[33];
        int fill = (int)(d * 32 + 0.5);
        for (int i = 0; i < 32; i++) bar[i] = i < fill ? '#' : '.';
        bar[32] = 0;
        printf("  %-5llu [%11llu,%11llu) %12llu %7.2f%% %s\n",
               (unsigned long long)s, (unsigned long long)lo, (unsigned long long)hi,
               (unsigned long long)set, d * 100, bar);
    }

    /* chunk 热图（自动拉伸） */
    printf("  热图（行=%d格，| = 片界；字符按本图 max 拉伸，空格=0，@=%.1f%%=格密度max）：\n",
           cells_per_row, maxd * 100);
    for (uint64_t i = 0; i < n; i++) {
        if (i % (uint64_t)cells_per_row == 0) printf("  %5llu ", (unsigned long long)i);
        double d = v.chunk_tot[i] ? (double)v.chunk_set[i] / (double)v.chunk_tot[i] : 0;
        int ch = ramp_char(maxd > 0 ? d / maxd : 0);
        if (color) {
            int lvl = 232 + (int)((double)ch / 9.0 * 23.0);   /* 232..255 灰阶 → 前景字符 */
            printf("\033[38;5;%dm%c\033[0m", lvl, RAMP[ch]);
        } else {
            putchar(RAMP[ch]);
        }
        bool end = (i % (uint64_t)cells_per_row == (uint64_t)cells_per_row - 1);
        bool slice_end = (v.slice_cells && ((i + 1) % v.slice_cells == 0));
        bool last = (i == n - 1);
        if (slice_end && !last) { printf("|"); }
        if (end || last) printf("\n");
    }

    /* 热点榜 */
    std::vector<uint64_t> idx(n);
    for (uint64_t i = 0; i < n; i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](uint64_t a, uint64_t b) {
        return (double)v.chunk_set[a] / v.chunk_tot[a] > (double)v.chunk_set[b] / v.chunk_tot[b];
    });
    printf("  Top %d 热点 chunk：\n", top_n);
    for (int i = 0; i < top_n && i < (int)n; i++) {
        uint64_t c = idx[i];
        uint64_t bit_lo = c * v.chunk_bits;
        double d = (double)v.chunk_set[c] / (double)v.chunk_tot[c];
        printf("    #%d chunk=%llu  bits[%llu,%llu)  set=%llu/%llu  %.1f%%\n",
               i + 1, (unsigned long long)c,
               (unsigned long long)bit_lo, (unsigned long long)(bit_lo + v.chunk_tot[c]),
               (unsigned long long)v.chunk_set[c], (unsigned long long)v.chunk_tot[c], d * 100);
    }
}

/* ── SVG 输出：两幅网格热图 ── */
static void svg_view(FILE *f, const BitmapView &v, double *y, int cells_per_row) {
    uint64_t n = v.chunk_set.size();
    double maxd = 0;
    for (uint64_t i = 0; i < n; i++) {
        double d = (double)v.chunk_set[i] / (double)v.chunk_tot[i];
        if (d > maxd) maxd = d;
    }
    const double cell = 14, gap = 2;
    int rows = (int)((n + cells_per_row - 1) / cells_per_row);

    fprintf(f, "<text x=\"10\" y=\"%.0f\" fill=\"#e8e8f0\" font-size=\"16\" font-family=\"monospace\">%s  (置位密度/chunk, 自动刻度 max=%.1f%%)</text>\n",
            *y + 4, v.name, maxd * 100);
    *y += 16;
    for (uint64_t i = 0; i < n; i++) {
        int r = (int)(i / cells_per_row), c = (int)(i % cells_per_row);
        double d = v.chunk_tot[i] ? (double)v.chunk_set[i] / (double)v.chunk_tot[i] : 0;
        char col[8];
        svg_color(maxd > 0 ? d / maxd : 0, col);
        double x = 48 + c * (cell + gap), yy = *y + r * (cell + gap);
        if (c == 0) {
            fprintf(f, "<text x=\"42\" y=\"%.0f\" fill=\"#889\" font-size=\"10\" text-anchor=\"end\" font-family=\"monospace\">%llu</text>",
                    yy + cell - 3, (unsigned long long)i);
        }
        uint64_t slice_idx = v.slice_cells ? i / v.slice_cells : 0;
        fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" fill=\"%s\"><title>chunk %llu · s%llu: %.2f%%</title></rect>\n",
                x, yy, cell, cell, col, (unsigned long long)i, (unsigned long long)slice_idx, d * 100);
        if (v.slice_cells && ((i + 1) % v.slice_cells == 0) && c != cells_per_row - 1 && i != n - 1) {
            fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" stroke=\"#556\" stroke-width=\"1\"/>\n",
                    x + cell + gap / 2, yy - 1, x + cell + gap / 2, yy + cell + 1);
        }
    }
    *y += rows * (cell + gap) + 26;
}

int main(int argc, char **argv) {
    const char *path = nullptr, *svg_path = nullptr;
    uint64_t order = 4, slice_bits = 16777216;
    int cells = 64, top_n = 10;
    bool color = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *o) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s 缺参数\n", o); exit(2); }
            return argv[++i];
        };
        if (a == "--order") order = strtoull(need("--order"), nullptr, 0);
        else if (a == "--slice-bits") slice_bits = strtoull(need("--slice-bits"), nullptr, 0);
        else if (a == "--cells") cells = atoi(need("--cells"));
        else if (a == "--top") top_n = atoi(need("--top"));
        else if (a == "--svg") svg_path = need("--svg");
        else if (a == "--color") color = true;
        else if (a[0] == '-') { fprintf(stderr, "error: 未知选项 %s\n", a.c_str()); return 2; }
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s <image-or-device> [--order N] [--slice-bits N] [--cells N] [--top N] [--color] [--svg out.svg]\n", argv[0]);
        return 2;
    }
    if (order > 8) { fprintf(stderr, "error: --order 须 0..8\n"); return 2; }
    if (cells < 8 || cells > 256) cells = 64;
    if (top_n < 1) top_n = 10;
    crc32_init();

    struct stat st;
    if (stat(path, &st) < 0) { fprintf(stderr, "error: stat %s failed\n", path); return 2; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "error: open %s (O_RDONLY) failed\n", path); return 2; }

    /* ── SB ── */
    uint8_t sbraw[FS_BLOCK_BYTES];
    if (!pread_full(fd, sbraw, sizeof(sbraw), 0)) { fprintf(stderr, "error: read SB failed\n"); return 2; }
    RavenSuperblock sb;
    memcpy(&sb, sbraw, sizeof(sb));

    printf("=== RavenFS bitmap hotspot (readonly) : %s ===\n", path);
    if (sb.magic != SUPERBLOCK_MAGIC) { fprintf(stderr, "error: SB magic=%#llx != RAVN\n", (unsigned long long)sb.magic); return 2; }
    if (sb.version != RAVENFS_VERSION) { fprintf(stderr, "error: SB version=%u != %u\n", sb.version, RAVENFS_VERSION); return 2; }
    {
        uint32_t stored = sb.crc32;
        sbraw[112] = sbraw[113] = sbraw[114] = sbraw[115] = 0;
        uint32_t calc = crc32_calc(sbraw, sizeof(sbraw));
        printf("SB: magic=RAVN version=%u crc32=%#08x %s\n", sb.version, stored,
               calc == stored ? "✓" : "✗（继续，仅供参考）");
    }

    uint64_t total_blocks = sb.total_blocks;
    uint64_t inode_count  = sb.inode_array_count;
    uint64_t bb = (total_blocks + 32767) / 32768;
    uint64_t bitmap_start = total_blocks - bb;
    uint64_t ib = (inode_count * 256 + 4095) / 4096;
    uint64_t inode_array_base = bitmap_start - ib;
    uint64_t ibb = (inode_count + 32767) / 32768;
    uint64_t inode_bm_base = inode_array_base - ibb;

    printf("geo: total_blocks=%llu inode_count=%llu  bitmap[%llu,+%llu) inode_bm[%llu,+%llu)\n",
           (unsigned long long)total_blocks, (unsigned long long)inode_count,
           (unsigned long long)bitmap_start, (unsigned long long)bb,
           (unsigned long long)inode_bm_base, (unsigned long long)ibb);
    printf("params: chunk_order=%llu → chunk_bits=%llu(%lluK)  slice_bits=%llu\n",
           (unsigned long long)order, (unsigned long long)(32768ULL << order),
           (unsigned long long)((32768ULL << order) / 1024), (unsigned long long)slice_bits);

    uint64_t chunk_bits = 32768ULL << order;
    if (slice_bits < chunk_bits) slice_bits = chunk_bits;
    slice_bits -= slice_bits % chunk_bits;      /* 对齐到 chunk 边界 */
    uint64_t slice_cells = slice_bits / chunk_bits;

    BitmapView blk{ "块位图 (block bitmap)", total_blocks, blkoff(bitmap_start), chunk_bits, slice_cells, {}, {} };
    BitmapView ino{ "inode 位图 (inode bitmap)", inode_count, blkoff(inode_bm_base), chunk_bits, slice_cells, {}, {} };
    if (!scan_bitmap(fd, blk) || !scan_bitmap(fd, ino)) { close(fd); return 2; }

    print_view(blk, cells, top_n, color);
    print_view(ino, cells, top_n, color);

    if (svg_path) {
        FILE *f = fopen(svg_path, "w");
        if (!f) { fprintf(stderr, "error: open %s for write failed\n", svg_path); close(fd); return 2; }
        double y = 40;
        uint64_t nb = blk.chunk_set.size(), ni = ino.chunk_set.size();
        int rowsb = (int)((nb + cells - 1) / cells), rowsi = (int)((ni + cells - 1) / cells);
        double H = 40 + 16 + rowsb * 16 + 30 + 16 + rowsi * 16 + 30;
        fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" style=\"background:#0d1117\">\n", (double)(cells * 16 + 200), H);
        fprintf(f, "<text x=\"10\" y=\"24\" fill=\"#e8e8f0\" font-size=\"18\" font-family=\"monospace\">RavenFS bitmap hotspot — %s</text>\n", path);
        svg_view(f, blk, &y, cells);
        svg_view(f, ino, &y, cells);
        fprintf(f, "</svg>\n");
        fclose(f);
        printf("\nSVG → %s\n", svg_path);
    }

    close(fd);
    return 0;
}
