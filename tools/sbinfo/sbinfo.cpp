/* ═══════════════════════════════════════════════════════════════════════════
 * sbinfo — RavenFS superblock 只读查看器（host 侧）
 *
 * 定位（2026-09-29）：用户态此前只有 mkfs（写盘）+ audit（全盘深审），中间缺
 * 「只看 superblock」的轻查询——`dumpe2fs -h` 之于 ext4。秒开、只读。
 *
 * 多盘面支持（2026-10-05 拆 TU）：
 *   v1.x 与 v2.0 的 `RavenSuperblock` **类型重名且布局不同**，不能同 TU include。
 *   故本文件只做「读 4K → 按 SB magic 分派」：
 *     · magic word0 == "RAVN"(v1.x)         → sbinfo_v1.cpp
 *     · magic word0 == "RAVENFS\0"(v2.0)    → sbinfo_v20.cpp
 *   共享设施（结果簿 / crc32 / 小工具）在 sbinfo_common.{hpp,cpp}。
 *
 * 纪律：只读（无 O_WRONLY / 无 pwrite）；结果走 stdout，致命走 stderr；
 *      退出码 0 = 全部检查通过，1 = 有 BAD，2 = 用法 / IO 错误。
 *
 * 编译： make   （或 g++ -Wall -Wextra -O2 -g -I../../include \
 *                     sbinfo.cpp sbinfo_common.cpp sbinfo_v1.cpp sbinfo_v20.cpp -o sbinfo）
 * 运行： ./sbinfo <image-or-device>
 *        ./sbinfo --probe <dev>      （udev 探针：blkid 风格 ID_FS_*）
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "sbinfo_common.hpp"
#include <magics.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifndef BLKGETSIZE64
#  define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

/* ═══════════════════ 探针模式：--probe <dev> ═══════════════════
 * udev IMPORT{program}：快、静默、无副作用、恒 exit 0。非 RavenFS → 无输出。 */
static int probe_mode(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t raw[FS_BLOCK_BYTES];
    bool got = pread_full(fd, raw, sizeof raw, 0);
    close(fd);
    if (!got) return 0;

    uint64_t w0 = 0;
    memcpy(&w0, raw, sizeof w0);
    if (w0 == SUPERBLOCK_MAGIC)         sbinfo_v1_probe(raw);
    else if (w0 == SUPERBLOCK_MAGIC_V20_W0) sbinfo_v20_probe(raw);
    return 0;   /* 恒 0：udev 只看输出 */
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

    printf("=== RavenFS superblock info (readonly) : %s ===\n", path);
    printf("device: %s, %llu bytes, lba=%llu\n\n",
           is_blk ? "block" : "file",
           (unsigned long long)dev_bytes, (unsigned long long)lba_size);

    uint64_t w0 = 0;
    memcpy(&w0, raw.data(), sizeof w0);
    if (w0 == SUPERBLOCK_MAGIC) {
        sbinfo_v1_dump(fd, path, dev_bytes, lba_size, raw.data());
    } else if (w0 == SUPERBLOCK_MAGIC_V20_W0) {
        sbinfo_v20_dump(fd, path, dev_bytes, lba_size, raw.data());
    } else {
        /* 非 RavenFS：给指向性诊断（本工具存在的直接动因） */
        bad("magic = %#llx，既非 v1.x \"RAVN\"(%#llx) 也非 v2.0 \"RAVENFS\"(%#llx)",
            (unsigned long long)w0,
            (unsigned long long)SUPERBLOCK_MAGIC,
            (unsigned long long)SUPERBLOCK_MAGIC_V20_W0);
        uint16_t ext_magic = 0;
        if (pread_full(fd, &ext_magic, 2, EXT4_MAGIC_OFF) && ext_magic == EXT4_SB_MAGIC) {
            printf("       → 偏移 0x438 检出 ext 系 magic 0x%04x：这盘是 ext2/3/4，"
                   "dumpe2fs 才是正主\n", EXT4_SB_MAGIC);
        } else {
            printf("       → 字节 0 不是 RavenFS 超级块（也未检出 ext4 magic）；"
                   "未格式化？格式化工具用错盘？\n");
        }
    }

    close(fd);
    return sbinfo_finish();
}
