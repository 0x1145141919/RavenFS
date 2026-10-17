// rfs_slice_fr.cpp — RavenFS v2.0 **分片空闲区间（per-slice free-range）trace dump**（只读）
//
// 目的：按 **上帝类 / ko 通用的分片算法**（见下「契约依据」）把 block 位图切成
//       slice_count 个分片，**逐片**枚举该片内的极大连续空闲区间（bit=0 的 run，
//       已按片边界夹断），以**结构化 trace** 输出到 stdout，供外部重定向 + 事后挖掘。
//
// 为什么单列这个工具：分片算法**不落盘**（盘上只有整张 block 位图 + 挂载时重建），
//       但它**持久地影响**后续每一次分配（片内 best-fit、跨片只靠 preferred 轮转、
//       释放按 `base/per` 定位到唯一片）。位图形状看不出「分片后每片长什么样」，
//       本工具就是补这块——把「分片视图」变成可 diff、可回归、可结构化分析的日志。
//
// ── 契约依据（与 god / ko **逐式一致**）────────────────────────────────
//   · 分片数 n 推导（god `juvenile_god/fs_allocator_router.cpp:166` fs_allocator_t ctor
//     ≡ ko `linux/ravenfs_allocator_router.c:196` rfs_allocator_init；block 组单组，v2.0
//     inode 已是数据面不参与切片）：
//       1) 2^n 上界：n = 1; while (n < cpu && n < 2^30) n <<= 1;
//       2) 满片数约束：blk_cap = nbits / msb; cap = blk_cap ?: 1; if (n > cap) n = cap;
//       3) 末片非空迭代：per_b = ceil(ceil(nbits/n)/msb)*msb;
//          若 (n-1)*per_b < nbits 则停，否则 n >>= 1（n==1 兜底）。
//       DEFRAG 模式（god SLICES_MODE_DEFRAG）在③之后**强制 n = 1**（全片视角）。
//   · 每片几何（god `setup_slices` `:79` ≡ ko `setup_group` `:126`）：
//       per = ((nbits + n - 1)/n + msb - 1)/msb*msb;（0 → msb）
//       片 i：begin = i*per，cnt = min(per, nbits - begin)。
//   · 常量：msb = `min_slice_bits` 默认 32768（`RFS_IV_SLICE_MIN_BITS`
//     `include/linux/ravenfs_allocator_top.h:68`；god `fs_allocator.hpp:343`）；
//     nbits = bitmap_blocks × 32768（`RAVENFS_BITS_PER_BLOCK` `include/linux/ravenfs.h:62`
//     × `linux/allocator_adapter.c:778`）；1 bit = 1 FS_block。
//
// ── cpu 数：显式参数优先，缺省回退机器全局 ──────────────────────────
//   `--cpu N` 给定 → 用之（无状态、可复现、几何 A/B 的基准）；
//   未给定 → 取**机器全局**可配置 CPU 数（`sysconf(_SC_NPROCESSORS_CONF)`，语义对齐 ko
//   `allocator_adapter.c` 的 `num_possible_cpus()`），再退 online 数、再退 1。
//   无论哪种来源，trace 均带 `cpu_src=` 字段 → 事后可判几何来自「参数」还是「宿主」。
//
// ── 磁盘契约（v2.0，与 mkfs_v20.cpp / audit 逐式一致）──────────────────
//   · FS_block = 4096 B；FS_block[b] 字节偏移 = (world_base_lba + b) × lba_size。
//   · Primary SB @ 字节 0，magic[0]=RFS_SB_MAGIC_W0，version=RAVENFS_VERSION。
//   · bitmap_blocks = ceil(total/32768)；bitmap_start = total − bitmap_blocks；
//     位序 LSB-first；**1 = 占用，0 = 空闲**。
//
// ── 数据流纪律 ────────────────────────────────────────────────────
//   · 只读（O_RDONLY，无写路径）；**结构化 trace 一律 stdout**（外部重定向即可），
//     诊断/错误一律 stderr。
//
// 编译： g++ -std=c++17 -Wall -Wextra -O2 -I../../include rfs_slice_fr.cpp -o rfs_slice_fr
// 运行： ./rfs_slice_fr <image-or-device> [--cpu N] [--msb N] [--defrag]
//                              [--top N] [--quiet] [--json]
// 退出码： 0=正常  2=用法/IO/契约错误
//
// ── trace schema（每行 `[TRACE][<tag>] key=value ...`）─────────────
//   [TRACE][BSLICE-GEO] cpu cpu_src msb nbits total_blocks bitmap_start bitmap_blocks
//                        slices per_slice_bits defrag
//   [TRACE][BSLICE]     idx begin bits end free used fr max_len min_len
//   [TRACE][BSLICE-FR]  idx ord base len end          (base/len = 全局块号/块数)
//   [TRACE][BSLICE-SUM] cpu cpu_src msb slices per_slice_bits fr_total free_total
//                        max_len covered_bits covered_blocks
//   （`idx` = 片号 [0,slices)；`ord` = 该片内 fr 序 [0,fr)；`end = base+len`）

#include <disk_struct_v2.0.h>
#include <magics.h>

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

static constexpr uint64_t FS_BLOCK    = 4096;
static constexpr uint64_t BITS_BLOCK  = 32768;    /* RAVENFS_BITS_PER_BLOCK */
static constexpr uint64_t MSB_DEFAULT = 32768;    /* RFS_IV_SLICE_MIN_BITS */

static const char *g_prog = "rfs_slice_fr";

static void die(const char *msg) {
    fprintf(stderr, "%s: error: %s\n", g_prog, msg);
    exit(2);
}

static void usage(void) {
    fprintf(stderr,
        "usage: %s <image-or-device> [options]\n"
        "  --cpu N        logical CPU count fed to the SHARED sharding geometry\n"
        "                 (omitted = machine-global count via sysconf(_SC_NPROCESSORS_CONF),\n"
        "                  mirroring ko num_possible_cpus(); then online, then 1)\n"
        "  --msb N        min_slice_bits in BITS (slice lower bound; default 32768)\n"
        "  --defrag       SLICES_MODE_DEFRAG: force a single slice (offline view)\n"
        "  --top N        per-slice FR line cap (0 = all; default 0)\n"
        "  --quiet        GEO + per-slice + SUM only (no FR lines)\n"
        "  --json         machine-readable stream instead of [TRACE] lines\n"
        "\n"
        "Splits the v2.0 block bitmap into the god/ko-common slices and dumps\n"
        "each slice's maximal free (bit=0) intervals, clipped to slice bounds.\n"
        "Read-only; stdout = structured trace; block = 4096 B; 1 bit = 1 FS_block.\n",
        g_prog);
}

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

/* ── 共享分片几何（god ctor ≡ ko rfs_allocator_init；见文件头「契约依据」）── */
struct slice_geo {
    uint32_t cpu_slots;
    uint64_t slice_count;
    uint64_t per_slice_bits;
};

static slice_geo derive_geometry(uint64_t nbits, uint32_t cpu_count, uint64_t msb,
                                 bool defrag) {
    slice_geo g{};
    g.cpu_slots = cpu_count ? cpu_count : 1;
    if (msb == 0) msb = MSB_DEFAULT;

    /* 1) 2^n 上界（仿 FPA；0 → cpu_slots 兜底单槽）*/
    uint64_t n = 1;
    while (n < g.cpu_slots && n < (1ULL << 30)) n <<= 1;

    /* 2) 满片数约束：每片 ≥ msb 位（v2.0 仅 block 组）*/
    uint64_t blk_cap = nbits / msb;
    uint64_t cap = blk_cap ? blk_cap : 1;
    if (n > cap) n = cap;

    /* 3) 末片非空迭代 */
    for (;;) {
        uint64_t per_b = ((nbits + n - 1) / n + msb - 1) / msb * msb;
        if (per_b == 0) per_b = msb;
        if ((n - 1) * per_b < nbits) break;
        if (n == 1) break;
        n >>= 1;
    }

    /* DEFRAG：god ctor 在③之后强制单分片（全片视角）*/
    if (defrag) n = 1;

    g.slice_count = n;

    /* setup_slices/setup_group：用**最终 n** 重算 per（与上面 per_b 同式）*/
    uint64_t per = ((nbits + n - 1) / n + msb - 1) / msb * msb;
    if (per == 0) per = msb;
    g.per_slice_bits = per;
    return g;
}

/* CPU 数来源：显式参数优先；缺省取机器全局（对齐 num_possible_cpus()）。*/
static uint32_t resolve_cpu(bool cpu_seen, uint32_t cpu_param, const char **src_out) {
    if (cpu_seen && cpu_param) { *src_out = "param"; return cpu_param; }

    long c = sysconf(_SC_NPROCESSORS_CONF);
    if (c > 0) { *src_out = "host-conf"; return (uint32_t)c; }
    long o = sysconf(_SC_NPROCESSORS_ONLN);
    if (o > 0) { *src_out = "host-onln"; return (uint32_t)o; }
    *src_out = "fallback1";
    return 1;
}

struct Run { uint64_t base; uint64_t len; };

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *path = argv[1];

    uint32_t cpu_param = 0;
    bool cpu_seen = false;
    uint64_t msb = MSB_DEFAULT;
    bool defrag = false, json = false, quiet = false;
    uint64_t top = 0;                       /* 0 = all */

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](void) -> const char * {
            if (i + 1 >= argc) die("missing option value");
            return argv[++i];
        };
        if (a == "--cpu")            { cpu_param = (uint32_t)strtoul(need(), nullptr, 0); cpu_seen = true; }
        else if (a == "--msb")       msb = strtoull(need(), nullptr, 0);
        else if (a == "--defrag")    defrag = true;
        else if (a == "--top")       top = strtoull(need(), nullptr, 0);
        else if (a == "--quiet")     quiet = true;
        else if (a == "--json")      json = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "%s: unknown option '%s'\n", g_prog, a.c_str()); usage(); return 2; }
    }
    if (msb == 0 || (msb & (msb - 1)) != 0)
        die("--msb must be a non-zero power of two (bits)");

    const char *cpu_src = "param";
    const uint32_t cpu = resolve_cpu(cpu_seen, cpu_param, &cpu_src);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 2; }

    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat failed");
    bool is_blk = S_ISBLK(st.st_mode);
    uint64_t lba_size = FS_BLOCK;
    if (is_blk) {
        int ss = 0;
        if (ioctl(fd, BLKSSZGET, &ss) == 0 && ss > 0) lba_size = (uint64_t)ss;
    }

    RavenSuperblock sb;
    pread_full(fd, &sb, sizeof(sb), 0);
    if (sb.magic[0] != RFS_SB_MAGIC_W0) {
        fprintf(stderr, "%s: error: bad superblock magic (0x%016" PRIx64 " != 0x%016" PRIx64 ")\n",
                g_prog, (uint64_t)sb.magic[0], (uint64_t)RFS_SB_MAGIC_W0);
        close(fd); return 2;
    }
    if (sb.version != RAVENFS_VERSION) {
        fprintf(stderr, "%s: error: not a v2.0 volume (version=%u)\n", g_prog, sb.version);
        close(fd); return 2;
    }

    const uint64_t total_blocks   = sb.total_blocks;
    const uint64_t world_base_lba = sb.world_base_lba;
    if (total_blocks == 0) die("total_blocks == 0 (corrupt SB)");

    const uint64_t base_byte     = world_base_lba * lba_size;
    const uint64_t bitmap_blocks = (total_blocks + BITS_BLOCK - 1) / BITS_BLOCK;
    const uint64_t bitmap_start  = total_blocks - bitmap_blocks;
    const uint64_t bm_bytes      = bitmap_blocks * FS_BLOCK;
    const uint64_t nbits         = bitmap_blocks * BITS_BLOCK;   /* 分配器视角的位数 */

    std::vector<uint8_t> bm(bm_bytes);
    pread_full(fd, bm.data(), bm_bytes, base_byte + bitmap_start * FS_BLOCK);

    slice_geo geo = derive_geometry(nbits, cpu, msb, defrag);
    if (geo.slice_count == 0) die("slice_count == 0 (geometry underflow)");

    auto bit_used = [&](uint64_t i) -> bool {
        return (bm[i >> 3] >> (i & 7)) & 1u;
    };

    uint64_t sum_fr = 0, sum_free = 0, sum_max = 0, covered = 0;

    if (json) {
        printf("{\n");
        printf("  \"path\": \"%s\",\n", path);
        printf("  \"cpu\": %u,\n", cpu);
        printf("  \"cpu_src\": \"%s\",\n", cpu_src);
        printf("  \"msb\": %" PRIu64 ",\n", msb);
        printf("  \"nbits\": %" PRIu64 ",\n", nbits);
        printf("  \"total_blocks\": %" PRIu64 ",\n", total_blocks);
        printf("  \"bitmap_start\": %" PRIu64 ",\n", bitmap_start);
        printf("  \"bitmap_blocks\": %" PRIu64 ",\n", bitmap_blocks);
        printf("  \"slices\": %" PRIu64 ",\n", geo.slice_count);
        printf("  \"per_slice_bits\": %" PRIu64 ",\n", geo.per_slice_bits);
        printf("  \"defrag\": %u,\n", defrag ? 1u : 0u);
        printf("  \"slice_fr\": [\n");
    } else {
        printf("[TRACE][BSLICE-GEO] cpu=%u cpu_src=%s msb=%" PRIu64 " nbits=%" PRIu64
               " total_blocks=%" PRIu64 " bitmap_start=%" PRIu64 " bitmap_blocks=%" PRIu64
               " slices=%" PRIu64 " per_slice_bits=%" PRIu64 " defrag=%u\n",
               cpu, cpu_src, msb, nbits, total_blocks, bitmap_start, bitmap_blocks,
               geo.slice_count, geo.per_slice_bits, defrag ? 1u : 0u);
    }

    for (uint64_t idx = 0; idx < geo.slice_count; idx++) {
        const uint64_t begin = idx * geo.per_slice_bits;
        if (begin >= nbits) break;
        uint64_t cnt = nbits - begin;
        if (cnt > geo.per_slice_bits) cnt = geo.per_slice_bits;
        const uint64_t end = begin + cnt;
        covered += cnt;

        /* 逐位扫本片的极大 0-run（片内夹断）*/
        std::vector<Run> fr;
        uint64_t free_cnt = 0;
        uint64_t cur_start = 0;
        bool cur = false;
        for (uint64_t i = begin; i < end; i++) {
            if (!bit_used(i)) {
                if (!cur) { cur = true; cur_start = i; }
            } else if (cur) {
                fr.push_back({cur_start, i - cur_start});
                free_cnt += i - cur_start;
                cur = false;
            }
        }
        if (cur) { fr.push_back({cur_start, end - cur_start}); free_cnt += end - cur_start; }

        uint64_t max_len = 0, min_len = 0;
        for (const Run &r : fr) {
            if (r.len > max_len) max_len = r.len;
            if (min_len == 0 || r.len < min_len) min_len = r.len;
        }

        sum_fr += fr.size();
        sum_free += free_cnt;
        if (max_len > sum_max) sum_max = max_len;

        if (json) {
            printf("    {\"idx\": %" PRIu64 ", \"begin\": %" PRIu64 ", \"bits\": %" PRIu64
                   ", \"end\": %" PRIu64 ", \"free\": %" PRIu64 ", \"used\": %" PRIu64
                   ", \"fr\": %zu, \"max_len\": %" PRIu64 ", \"min_len\": %" PRIu64 ", \"frs\": [",
                   idx, begin, cnt, end, free_cnt, cnt - free_cnt, fr.size(), max_len, min_len);
            if (!quiet) {
                size_t shown = 0;
                for (const Run &r : fr) {
                    if (top && shown >= top) break;
                    printf("%s{\"base\": %" PRIu64 ", \"len\": %" PRIu64 "}",
                           shown ? ", " : "", r.base, r.len);
                    shown++;
                }
            }
            printf("]}%s\n", (idx + 1 == geo.slice_count) ? "" : ",");
            continue;
        }

        printf("[TRACE][BSLICE] idx=%" PRIu64 " begin=%" PRIu64 " bits=%" PRIu64
               " end=%" PRIu64 " free=%" PRIu64 " used=%" PRIu64 " fr=%zu max_len=%" PRIu64
               " min_len=%" PRIu64 "\n",
               idx, begin, cnt, end, free_cnt, cnt - free_cnt, fr.size(), max_len, min_len);

        if (!quiet) {
            size_t shown = 0;
            for (const Run &r : fr) {
                if (top && shown >= top) break;
                printf("[TRACE][BSLICE-FR] idx=%" PRIu64 " ord=%zu base=%" PRIu64
                       " len=%" PRIu64 " end=%" PRIu64 "\n",
                       idx, shown, r.base, r.len, r.base + r.len);
                shown++;
            }
        }
    }

    if (json) {
        printf("  ],\n");
        printf("  \"sum\": {\"cpu\": %u, \"cpu_src\": \"%s\", \"slices\": %" PRIu64
               ", \"per_slice_bits\": %" PRIu64 ", \"fr_total\": %" PRIu64
               ", \"free_total\": %" PRIu64 ", \"max_len\": %" PRIu64
               ", \"covered_bits\": %" PRIu64 "}\n}\n",
               cpu, cpu_src, geo.slice_count, geo.per_slice_bits, sum_fr, sum_free,
               sum_max, covered);
    } else {
        printf("[TRACE][BSLICE-SUM] cpu=%u cpu_src=%s msb=%" PRIu64 " slices=%" PRIu64
               " per_slice_bits=%" PRIu64 " fr_total=%" PRIu64 " free_total=%" PRIu64
               " max_len=%" PRIu64 " covered_bits=%" PRIu64 " covered_blocks=%" PRIu64 "\n",
               cpu, cpu_src, msb, geo.slice_count, geo.per_slice_bits, sum_fr, sum_free,
               sum_max, covered, covered);
    }

    close(fd);
    return 0;
}
