/* ═══════════════════════════════════════════════════════════════════════════
 * m44_attrcmp.cpp — M4.4 **host 侧属性断言小工具**（独立于 ko / 独立于 audit_v1.2）
 *
 * 直接离线解析 v1.2 镜像的 inode 字段，对一份属性清单逐项断言：
 *   kind / size / mode（含 suid/sgid/sticky）/ uid / gid / mtime / atime / ctime（含 ns）。
 * 供两类自证：
 *   1) host 侧无 guest 自证：`m44_attrcmp m44.img m44.img.expect`（初始靶子属性 == 声明）；
 *   2) guest 操作后把镜像带出，`m44_attrcmp out.img m44.img.wexpect`（属性已**落盘**保留）。
 *
 * 清单格式 = `mkstates_a` 产物（`.expect` / `.wexpect`）：
 *   A <inode> <kind:reg|dir|lnk> <size> <mode4oct> <uid> <gid> \
 *     <mtime_s> <mtime_ns> <atime_s> <atime_ns> <ctime_s|-> <ctime_ns> <path>
 * （ctime = -1 → 不断言）
 *
 * 纪律：**只读**（O_RDONLY）；不复用 audit_v1.2/readback 代码（自证独立）；不依赖 ko。
 * 用法： ./m44_attrcmp <image> <attrsfile>       退出：0=全一致；1=有 BAD；2=用法/IO
 * ═══════════════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include "magics.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static constexpr uint64_t BLK = 4096;
static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

struct Req {
    uint64_t inode;
    std::string kind;
    uint64_t size, mode, uid, gid;
    int64_t  mtime; int64_t mns;
    int64_t  atime; int64_t ans;
    int64_t  ctime; int64_t cns;      /* == -1 → 跳过 */
    std::string path;
};

static const char *type_name(uint32_t t) {
    switch (t) {
    case INODE_TYPE_FIFO:     return "fifo";
    case INODE_TYPE_CHR:      return "chr";
    case INODE_TYPE_DIR:      return "dir";
    case INODE_TYPE_BLK:      return "blk";
    case INODE_TYPE_REGULAR:  return "reg";
    case INODE_TYPE_SOFT_SYM: return "lnk";
    case INODE_TYPE_SOCK:     return "sock";
    default:                  return "?";
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <image> <attrsfile>\n", argv[0]);
        return 2;
    }
    /* 读清单 */
    std::vector<Req> reqs;
    {
        FILE *fp = fopen(argv[2], "r");
        if (!fp) { perror("open attrs"); return 2; }
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            if (line[0] != 'A') continue;
            Req r;
            char kind[16], cbuf[32], path[2048];
            unsigned long long inode = 0, size = 0, mode = 0, uid = 0, gid = 0;
            long long mns = 0, ans = 0, cns = 0;
            char mtime[32], atime[32];
            if (sscanf(line, "A %llu %15s %llu %llo %llu %llu %31s %lld %31s %lld %31s %lld %2047s",
                       &inode, kind, &size, &mode, &uid, &gid, mtime, &mns, atime, &ans, cbuf, &cns, path) != 13) {
                fprintf(stderr, "m44_attrcmp: 解析失败: %s", line);
                fclose(fp); return 2;
            }
            r.inode = inode; r.kind = kind; r.size = size; r.mode = mode;
            r.uid = uid; r.gid = gid;
            r.mtime = (strcmp(mtime, "-1") == 0) ? -1 : (int64_t)strtoull(mtime, nullptr, 10); r.mns = mns;
            r.atime = (strcmp(atime, "-1") == 0) ? -1 : (int64_t)strtoull(atime, nullptr, 10); r.ans = ans;
            r.ctime = (strcmp(cbuf, "-1") == 0) ? -1 : (int64_t)strtoull(cbuf, nullptr, 10);
            r.cns = cns; r.path = path;
            reqs.push_back(r);
        }
        fclose(fp);
    }

    struct stat st;
    if (stat(argv[1], &st) < 0) { perror("stat image"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open image"); return 2; }

    auto rd = [&](void *buf, size_t n, uint64_t off) {
        uint8_t *p = (uint8_t *)buf; size_t d = 0;
        while (d < n) { ssize_t r = pread(fd, p + d, n - d, (off_t)(off + d)); if (r <= 0) return false; d += (size_t)r; }
        return true;
    };

    RavenSuperblock sb;
    if (!rd(&sb, sizeof(sb), 0)) { fprintf(stderr, "m44_attrcmp: 读 SB 失败\n"); close(fd); return 2; }
    if (sb.magic != SUPERBLOCK_MAGIC) { fprintf(stderr, "m44_attrcmp: SB magic 不符\n"); close(fd); return 2; }
    uint64_t total_blocks = sb.total_blocks, inode_count = sb.inode_array_count;
    uint64_t bb = (total_blocks + 32767) / 32768;
    uint64_t ib = (inode_count * 256 + 4095) / 4096;
    uint64_t inode_array_base = (total_blocks - bb) - ib;

    printf("=== m44_attrcmp (host, readonly) : %s vs %s ===\n", argv[1], argv[2]);
    printf("geo total_blocks=%llu inode_count=%llu inode_array_base=%llu\n",
           (unsigned long long)total_blocks, (unsigned long long)inode_count,
           (unsigned long long)inode_array_base);

    int nbad = 0;
    for (const Req &r : reqs) {
        if (r.inode >= inode_count) { printf("[BAD] %-16s inode %llu 越界\n", r.path.c_str(), (unsigned long long)r.inode); nbad++; continue; }
        raven_Inode_t ino;
        if (!rd(&ino, 256, blkoff(inode_array_base) + r.inode * 256)) {
            printf("[BAD] %-16s 读 inode 失败\n", r.path.c_str()); nbad++; continue;
        }
        uint32_t type = RFS_INO1_GET_TYPE(ino.word1);
        std::string tkind = type_name(type);
        uint32_t mode   = RFS_MODE_GET(ino.mode_xattr);
        bool ok = true;
        auto chk = [&](bool cond, const char *what) {
            if (!cond) { ok = false; printf("  [DIFF] %-16s %s\n", r.path.c_str(), what); }
        };
        if (tkind != r.kind) { ok = false; printf("  [DIFF] %-16s kind 盘=%s 期望=%s\n", r.path.c_str(), tkind.c_str(), r.kind.c_str()); }
        if (ino.size != r.size)     { ok = false; printf("  [DIFF] %-16s size 盘=%llu 期望=%llu\n", r.path.c_str(), (unsigned long long)ino.size, (unsigned long long)r.size); }
        if (mode != r.mode)         { ok = false; printf("  [DIFF] %-16s mode 盘=%04o 期望=%04o\n", r.path.c_str(), mode, (unsigned)r.mode); }
        if (ino.uid != r.uid)       { ok = false; printf("  [DIFF] %-16s uid 盘=%u 期望=%llu\n", r.path.c_str(), ino.uid, (unsigned long long)r.uid); }
        if (ino.gid != r.gid)       { ok = false; printf("  [DIFF] %-16s gid 盘=%u 期望=%llu\n", r.path.c_str(), ino.gid, (unsigned long long)r.gid); }
        if (r.mtime != -1 && (ino.mtime != (uint64_t)r.mtime || (int64_t)ino.m_ns_stamp != r.mns)) {
            ok = false; printf("  [DIFF] %-16s mtime 盘=%llu.%u 期望=%lld.%lld\n", r.path.c_str(),
                               (unsigned long long)ino.mtime, ino.m_ns_stamp, (long long)r.mtime, (long long)r.mns);
        }
        if (r.atime != -1 && (ino.atime != (uint64_t)r.atime || (int64_t)ino.a_ns_stamp != r.ans)) {
            ok = false; printf("  [DIFF] %-16s atime 盘=%llu.%u 期望=%lld.%lld\n", r.path.c_str(),
                               (unsigned long long)ino.atime, ino.a_ns_stamp, (long long)r.atime, (long long)r.ans);
        }
        if (r.ctime != -1 && (ino.ctime != (uint64_t)r.ctime || (int64_t)ino.c_ns_stamp != r.cns)) {
            ok = false; printf("  [DIFF] %-16s ctime 盘=%llu.%u 期望=%lld.%lld\n", r.path.c_str(),
                               (unsigned long long)ino.ctime, ino.c_ns_stamp, (long long)r.ctime, (long long)r.cns);
        }
        (void)chk;
        if (ok) {
            printf("[OK ] %-16s inode=%-4llu %s size=%-8llu mode=%04o uid=%u gid=%u "
                   "m=%llu at=%llu\n",
                   r.path.c_str(), (unsigned long long)r.inode, tkind.c_str(),
                   (unsigned long long)ino.size, mode, ino.uid, ino.gid,
                   (unsigned long long)ino.mtime, (unsigned long long)ino.atime);
        } else {
            nbad++;
        }
    }
    close(fd);
    printf("m44_attrcmp: RESULT: %s（%zu 靶子, BAD=%d）\n", nbad == 0 ? "PASS" : "FAIL", reqs.size(), nbad);
    return nbad == 0 ? 0 : 1;
}
