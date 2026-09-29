/* ═══════════════════════════════════════════════════════════════════════════
 * sbinfo — RavenFS superblock 只读查看器（host 侧）
 *
 * 动机（2026-09-29）：用户态此前只有 mkfs（写盘）+ audit（全盘递归深审），
 * 中间缺一档「只看 superblock」的轻查询——`dumpe2fs -h` 之于 ext4 那种。
 * 本工具填这个空：秒开、只读、不碰 bitmap/inode/dentry（那是 audit 的辖区），
 * 盘不是 RavenFS 时给出**指向性诊断**（比如检测 0x438 处 ext4 magic，
 * 直接告诉用户「这盘是 ext4，dumpe2fs 才是正主」——本次起因就是拿
 * dumpe2fs 读 RavenFS 盘报「找不到超级块」）。
 *
 * 契约真值来源：
 *   · include/disk_struct_v1.2.h  （RavenSuperblock 4096B，packed；偏移全走结构体）
 *   · include/magics.h            （SUPERBLOCK_MAGIC = 0x5241564E "RAVN"）
 *   · mkfs/mkfs.cpp               （crc32 同多项式 0xEDB88320；build_superblock 字段语义；
 *                                   block_to_offset：FS_block[b] 字节 = world_base_lba*lba + b*4096）
 *   · tools/audit_v1.2/audit_v1.2.cpp（crc 校验口径：原始 4096B 拷贝、置零 [112,116)；
 *                                   backup 链游走的哨兵与护栏）
 *   · Docs/disk_layout_v1.1.md §15（四时间戳语义：0 哨兵 = 从未挂载/从未干净卸载）
 *
 * 设计取舍（与 audit 的分工）：
 *   · 只做 SB 层诊断 → 不读位图、不做全盘一致性（audit 已覆盖，重复实现只会漂移）；
 *   · 版本 2(v1.1)/3(v1.2) 均接受并**标注盘面**（两版 SB 字节布局全等，见 mkfs.hpp 头注），
 *     版本既非 2 也非 3 才判坏——查看器职责是「告诉我盘上是什么」，不是替挂载路径拒盘；
 *   · crc32 / magic / backup 链 / 设备尺寸 仍判 BAD（这些是 SB 自身完整性）。
 *
 * 纪律：只读（无 O_WRONLY / 无 pwrite）；日志走 stdout，致命走 stderr；
 *      退出码 0 = 全部检查通过，1 = 有 BAD，2 = 用法 / IO 错误。
 *
 * 编译： g++ -Wall -Wextra -O2 -g -I../../include sbinfo.cpp -o sbinfo
 * 运行： ./sbinfo <image-or-device>     （裸盘读需 root：sudo ./sbinfo /dev/nvme0n1）
 *       ./sbinfo --probe <dev>          （udev 探针模式：吐 blkid 风格 ID_FS_*；见下）
 *
 * ── --probe（2026-09-29 补）──
 *   动机：系统盘/数据盘 nvme0↔nvme1 枚举顺序会翻转，硬编码 /dev/nvmeXn1 的
 *   .mount 会挂错盘暴毙。UUID 路线要求内核/udev 侧有 /dev/disk/by-uuid/<uuid>，
 *   但 libblkid 不认识 ravenfs → 该软链从不生成 → fstab/.mount 的 UUID= 无处解。
 *   本模式以 blkid 风格吐 KEY=VALUE 供 udev IMPORT{program} 吸收，配合
 *   /etc/udev/rules.d/70-ravenfs.rules 手动补出 by-uuid/by-label 链。
 *   契约：仅 O_RDONLY 读 4KiB；非 RavenFS/打不开 → 静默 exit 0（无输出=不设变量）。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <disk_struct_v1.2.h>
#include <magics.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifndef BLKGETSIZE64
#  define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

static constexpr uint64_t FS_BLOCK_BYTES = 4096;
static constexpr uint32_t EXT4_SB_MAGIC   = 0xEF53;   /* ext 系 sbi_magic @ 0x438 */
static constexpr off_t    EXT4_MAGIC_OFF  = 0x438;

/* ─────────────────────────── 结果簿 ─────────────────────────── */
static int n_ok  = 0;
static int n_bad = 0;
static int n_warn = 0;
static void ok  (const char *fmt, ...) __attribute__((format(printf,1,2)));
static void bad (const char *fmt, ...) __attribute__((format(printf,1,2)));
static void warn(const char *fmt, ...) __attribute__((format(printf,1,2)));
#include <cstdarg>
static void vreport(const char *tag, const char *fmt, va_list ap) {
    printf("%-6s ", tag);
    vprintf(fmt, ap);
    printf("\n");
}
static void ok(const char *fmt, ...)   { n_ok++;  va_list ap; va_start(ap, fmt); vreport("OK",   fmt, ap); va_end(ap); }
static void bad(const char *fmt, ...)  { n_bad++; va_list ap; va_start(ap, fmt); vreport("BAD",  fmt, ap); va_end(ap); }
static void warn(const char *fmt, ...) { n_warn++; va_list ap; va_start(ap, fmt); vreport("WARN", fmt, ap); va_end(ap); }

/* ═══════════════════════════ CRC32（与 mkfs.cpp / audit 逐位一致） ═══════════════════════════ */
static uint32_t crc32_tab[256];
static bool     crc32_tab_init = false;
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_tab[i] = c;
    }
    crc32_tab_init = true;
}
static uint32_t crc32_calc(const void *data, size_t len) {
    if (!crc32_tab_init) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        c = crc32_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* SB crc32 校验口径（同 audit）：原始 4096B 拷贝 → 置零 crc32 字段 [112,116) → 全长重算 */
static constexpr size_t CRC_OFF = 112;   /* offsetof(RavenSuperblock, crc32) */
static uint32_t sb_crc32(const uint8_t *raw) {
    std::vector<uint8_t> tmp(raw, raw + FS_BLOCK_BYTES);
    memset(tmp.data() + CRC_OFF, 0, 4);
    return crc32_calc(tmp.data(), tmp.size());
}

/* ─────────────────────────── 小工具 ─────────────────────────── */
static bool pread_full(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; off += r; n -= (size_t)r;
    }
    return true;
}

static std::string fmt_time(uint64_t epoch) {
    if (epoch == 0) return "never (0)";
    time_t t = (time_t)epoch;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return "?";
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    char out[96];
    snprintf(out, sizeof out, "%s (%llu)", buf, (unsigned long long)epoch);
    return out;
}

static std::string fmt_bytes(uint64_t bytes) {
    static const char *unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    char buf[64];
    snprintf(buf, sizeof buf, "%.2f %s", v, unit[u]);
    return buf;
}

/* volume_name / uuid 打印（定长字段可能不带 NUL） */
static std::string fmt_label(const uint8_t *name, size_t cap) {
    size_t len = 0;
    while (len < cap && name[len]) len++;
    std::string s;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = name[i];
        s += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    return s;
}
static std::string fmt_uuid(const uint8_t u[16]) {
    char buf[37];
    snprintf(buf, sizeof buf,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
             u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
    return buf;
}

/* ═══════════════════ udev/blkid 探针模式：--probe <dev> ═══════════════════
 * 见文件头。udev IMPORT{program} 调用入口：必须快、静默、无副作用、恒 exit 0。 */
static void probe_enc(std::string &s) {   /* ID_FS_*_ENC：文件名安全化 */
    for (char &c : s) {
        unsigned char u = (unsigned char)c;
        const bool safe = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                          (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        if (!safe) c = '_';
    }
}
static int probe_mode(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;                              /* 打不开 → 静默 */
    uint8_t raw[FS_BLOCK_BYTES];
    bool got = pread_full(fd, raw, sizeof raw, 0);
    close(fd);
    if (!got) return 0;
    RavenSuperblock sb;
    memcpy(&sb, raw, sizeof sb);
    if (sb.magic != SUPERBLOCK_MAGIC) return 0;        /* 非 RavenFS → 静默 */
    if (sb.version != 2 && sb.version != 3) return 0;
    std::string uuid  = fmt_uuid(sb.uuid);
    std::string label = fmt_label(sb.volume_name, sizeof sb.volume_name);
    printf("ID_FS_TYPE=ravenfs\n");
    printf("ID_FS_USAGE=filesystem\n");
    printf("ID_FS_VERSION=%s\n", sb.version == 3 ? "1.2" : "1.1");
    printf("ID_FS_UUID=%s\n", uuid.c_str());
    printf("ID_FS_UUID_ENC=%s\n", uuid.c_str());
    if (!label.empty()) {
        printf("ID_FS_LABEL=%s\n", label.c_str());
        probe_enc(label);
        printf("ID_FS_LABEL_ENC=%s\n", label.c_str());
    }
    return 0;                                          /* 恒 0：udev 只看输出 */
}

/* ═══════════════════════════ main ═══════════════════════════ */
int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--probe") == 0)
        return probe_mode(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-or-device>\n       %s --probe <dev>\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 2; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 2; }
    const bool is_blk = S_ISBLK(st.st_mode);
    if (!is_blk && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: %s 既不是块设备也不是普通文件\n", path);
        close(fd); return 2;
    }

    /* 设备尺寸 + LBA（lba 只用于换算 world_base_lba → 字节基址；块设备 ioctl 失败不致命，
     * 回退假设 base = world_base_lba * 4096 的前提会破坏 → 直接按 mkfs 契约 4096 起算并 WARN） */
    uint64_t dev_bytes = 0;
    uint64_t lba_size  = 4096;
    if (is_blk) {
        if (ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) { perror("BLKGETSIZE64"); close(fd); return 2; }
        int lba = 0;
        if (ioctl(fd, BLKSSZGET, &lba) == 0 && lba > 0) lba_size = (uint64_t)lba;
        else warn("BLKSSZGET 失败，LBA 按 4096 猜测（world 基址换算可能不准）");
    } else {
        dev_bytes = (uint64_t)st.st_size;
    }

    /* ── Primary SB @ 设备字节 0 ── */
    std::vector<uint8_t> raw(FS_BLOCK_BYTES);
    if (!pread_full(fd, raw.data(), FS_BLOCK_BYTES, 0)) {
        fprintf(stderr, "error: 读取 superblock 失败\n");
        close(fd); return 2;
    }
    RavenSuperblock sb;
    memcpy(&sb, raw.data(), sizeof sb);

    printf("=== RavenFS superblock info (readonly) : %s ===\n", path);
    printf("device: %s, %llu bytes, lba=%llu\n\n",
           is_blk ? "block" : "file",
           (unsigned long long)dev_bytes, (unsigned long long)lba_size);

    /* ── magic：RavenFS 不中时给指向性诊断（本工具存在的直接动因） ── */
    if (sb.magic == SUPERBLOCK_MAGIC) {
        ok("magic = \"RAVN\" (%#llx)", (unsigned long long)sb.magic);
    } else {
        bad("magic = %#llx，不是 \"RAVN\" (%#llx)",
            (unsigned long long)sb.magic, (unsigned long long)SUPERBLOCK_MAGIC);
        uint16_t ext_magic = 0;
        if (pread_full(fd, &ext_magic, 2, EXT4_MAGIC_OFF) && ext_magic == EXT4_SB_MAGIC) {
            printf("       → 偏移 0x438 检出 ext 系 magic 0x%04x：这盘是 ext2/3/4，"
                   "dumpe2fs 才是正主（RavenFS SB magic \"RAVN\" 在字节 0）\n",
                   EXT4_SB_MAGIC);
        } else {
            printf("       → 字节 0 不是 RavenFS 超级块（也未检出 ext4 magic）；"
                   "未格式化？格式化工具用错盘？\n");
        }
        close(fd);
        printf("\nchecks: %d ok, %d bad, %d warn\n", n_ok, n_bad, n_warn);
        return 1;
    }

    /* ── version：2/3 均为合法盘面（SB 布局全等，仅语义分叉），标注即可 ── */
    const char *vname =
        sb.version == 2 ? "v1.1（god 盘面）" :
        sb.version == 3 ? "v1.2" : "未知版本";
    if (sb.version == 2 || sb.version == 3)
        ok("version = %u (%s)", sb.version, vname);
    else
        bad("version = %u，既非 2(v1.1) 也非 3(v1.2)", sb.version);

    /* ── crc32 ── */
    {
        uint32_t stored = sb.crc32;
        uint32_t calc   = sb_crc32(raw.data());
        if (stored == calc) ok("crc32 = %#010x（重算一致）", stored);
        else                bad("crc32 = %#010x，重算 = %#010x（不一致）", stored, calc);
    }

    /* ── 核心字段 ── */
    printf("\n-- 字段 --\n");
    printf("volume label     : \"%s\"\n", fmt_label(sb.volume_name, sizeof sb.volume_name).c_str());
    printf("uuid             : %s\n", fmt_uuid(sb.uuid).c_str());
    printf("world_base_lba   : %llu  (lba=%llu → FS 世界字节基址 %llu)",
           (unsigned long long)sb.world_base_lba, (unsigned long long)lba_size,
           (unsigned long long)(sb.world_base_lba * lba_size));
    if (sb.world_base_lba * lba_size == FS_BLOCK_BYTES) printf("  [mkfs 契约: 4096]\n");
    else printf("  [偏离 mkfs 契约 4096]\n");
    printf("total_blocks     : %llu  (≈ %s)\n",
           (unsigned long long)sb.total_blocks,
           fmt_bytes(sb.total_blocks * FS_BLOCK_BYTES).c_str());
    printf("inode_array_count: %llu\n", (unsigned long long)sb.inode_array_count);
    printf("root_inode       : %llu%s\n", (unsigned long long)sb.root_inode,
           sb.root_inode == 16 ? "  [v1.2]" : (sb.root_inode == 0 ? "  [v1.1]" : "  [?!]"));

    printf("\n-- 卷生命周期时间戳 --\n");
    printf("born   : %s\n", fmt_time(sb.born_time).c_str());
    printf("mount  : %s\n", fmt_time(sb.mount_time).c_str());
    printf("umount : %s\n", fmt_time(sb.umount_time).c_str());
    printf("modify : %s\n", fmt_time(sb.modify_time).c_str());
    /* §15 语义：0 哨兵 = 从未挂载/从未干净卸载；崩溃态 = mount 新而 umount 旧 */
    if (sb.mount_time == 0 && sb.umount_time == 0)
        printf("state  : 从未挂载（或挂载路径未回写时间戳）\n");
    else if (sb.umount_time == 0 && sb.mount_time != 0)
        warn("state  : 有 mount 无 umount → 仍挂载中，或上次**非正常关机**（umount 0 哨兵）");
    else if (sb.mount_time > sb.umount_time)
        warn("state  : mount(%llu) > umount(%llu) → 上次非正常关机",
             (unsigned long long)sb.mount_time, (unsigned long long)sb.umount_time);
    else
        printf("state  : 干净卸载历史（umount ≥ mount）\n");

    /* ── 派生几何（与 mkfs calc_geometry 逐式一致；audit 同源） ── */
    printf("\n-- 派生几何（mkfs calc_geometry 同式） --\n");
    if (sb.total_blocks == 0 || sb.inode_array_count == 0) {
        bad("total_blocks/inode_array_count 为 0，几何不可信");
    } else {
        uint64_t total = sb.total_blocks, ic = sb.inode_array_count;
        uint64_t bb = (total + 32767) / 32768;
        uint64_t bitmap_start = total - bb;
        uint64_t ib  = (ic * 256 + 4095) / 4096;
        uint64_t iab = bitmap_start - ib;
        uint64_t ibb = (ic + 32767) / 32768;
        uint64_t ibm = iab - ibb;
        if (ibm <= 1) bad("inode_bitmap_base = %llu ≤ 1，几何非法（设备太小？）",
                          (unsigned long long)ibm);
        else {
            printf("block bitmap     : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)bitmap_start, (unsigned long long)total,
                   (unsigned long long)bb);
            printf("inode array      : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)iab, (unsigned long long)bitmap_start,
                   (unsigned long long)ib);
            printf("inode bitmap     : [%llu, %llu)  (+%llu blocks)\n",
                   (unsigned long long)ibm, (unsigned long long)iab,
                   (unsigned long long)ibb);
        }

        /* 设备尺寸交叉验证（audit 同判据） */
        uint64_t expect = FS_BLOCK_BYTES * (total + 1);
        if (dev_bytes == expect)
            ok("device bytes = 4096*(total_blocks+1) = %llu",
               (unsigned long long)expect);
        else
            warn("device bytes %llu ≠ 4096*(total_blocks+1) = %llu（SB total_blocks 与盘尺寸不符）",
                 (unsigned long long)dev_bytes, (unsigned long long)expect);
    }

    /* ── Backup SB 链 ── */
    printf("\n-- Backup SB 链（count=%u, head=backup_next） --\n", sb.backup_count);
    const uint64_t world_byte_base = sb.world_base_lba * lba_size;
    if (sb.backup_next == PTR_INVALID) {
        if (sb.backup_count == 0) ok("无 backup SB（backup_count=0）");
        else bad("backup_next = PTR_INVALID 但 backup_count = %u", sb.backup_count);
    } else {
        uint64_t blk = sb.backup_next, prev_blk = PTR_INVALID;
        uint32_t guard = 0;
        uint32_t max_iter = sb.backup_count + 8;      /* 同 audit 护栏 */
        bool saw_dup = false;
        while (blk != PTR_INVALID && blk < sb.total_blocks && guard < max_iter) {
            off_t off = (off_t)(world_byte_base + blk * FS_BLOCK_BYTES);
            std::vector<uint8_t> braw(FS_BLOCK_BYTES);
            if (!pread_full(fd, braw.data(), FS_BLOCK_BYTES, off)) {
                bad("backup[%u] blk=%llu 读取失败（off=%llu）",
                    guard, (unsigned long long)blk, (unsigned long long)off);
                break;
            }
            RavenSuperblock bsb;
            memcpy(&bsb, braw.data(), sizeof bsb);

            bool magic_ok = (bsb.magic == SUPERBLOCK_MAGIC);
            uint32_t bcalc = sb_crc32(braw.data());
            bool crc_ok   = (bsb.crc32 == bcalc);
            bool link_ok  = (bsb.backup_prev == prev_blk);

            if (magic_ok && crc_ok && link_ok)
                ok("backup[%u] blk=%llu off=%llu  magic✓ crc✓ prev链✓ (prev=%s next=%s)",
                   guard, (unsigned long long)blk, (unsigned long long)off,
                   bsb.backup_prev == PTR_INVALID ? "-" :
                       std::to_string(bsb.backup_prev).c_str(),
                   bsb.backup_next == PTR_INVALID ? "-" :
                       std::to_string(bsb.backup_next).c_str());
            else {
                bad("backup[%u] blk=%llu  magic%s crc%s prev链%s（存=%s 重算=%s prev=%llu 期望=%llu）",
                    guard, (unsigned long long)blk,
                    magic_ok ? "✓" : "✗", crc_ok ? "✓" : "✗", link_ok ? "✓" : "✗",
                    magic_ok ? "" : "≠RAVN ",
                    magic_ok ? (crc_ok ? "" : "不一致") : "",
                    (unsigned long long)bsb.backup_prev,
                    (unsigned long long)prev_blk);
            }

            if (bsb.backup_next == blk) { saw_dup = true; break; }   /* 自环 */
            prev_blk = blk;
            blk = bsb.backup_next;
            guard++;
        }
        if (saw_dup) bad("backup 链自环（next == self）");
        else if (blk != PTR_INVALID && guard >= max_iter)
            bad("backup 链超长（> %u 节点）或成环，已截断", max_iter);

        uint32_t walked = guard + (blk == PTR_INVALID ? 1 : 0);
        /* 计数核对：链上实际节点数 vs backup_count */
        uint32_t nodes = 0;
        {
            uint64_t b = sb.backup_next;
            uint32_t g = 0;
            while (b != PTR_INVALID && b < sb.total_blocks && g < max_iter) {
                std::vector<uint8_t> tmp(FS_BLOCK_BYTES);
                if (!pread_full(fd, tmp.data(), FS_BLOCK_BYTES,
                                (off_t)(world_byte_base + b * FS_BLOCK_BYTES))) break;
                RavenSuperblock t;
                memcpy(&t, tmp.data(), sizeof t);
                nodes++; g++;
                if (t.backup_next == b) break;
                b = t.backup_next;
            }
        }
        if (nodes == sb.backup_count)
            ok("backup 节点数 = backup_count = %u", nodes);
        else
            warn("backup 节点数 = %u ≠ backup_count = %u", nodes, sb.backup_count);
        (void)walked;
    }

    printf("\nchecks: %d ok, %d bad, %d warn\n", n_ok, n_bad, n_warn);
    close(fd);
    return n_bad ? 1 : 0;
}
