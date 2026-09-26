/* ═══════════════════════════════════════════════════════════════════════════
 * dents_dump.cpp — getdents64 续读压测/枚举器（纯 libc，无第三方依赖）
 *
 * 用途：在 guest 里跑，对挂载点某目录用**小缓冲** getdents64 反复续读，
 *       强制内核多次填充/续读路径；逐条打印，末尾给出计数汇总。
 *
 * 用法： ./dents_dump <dir> [bufsize]      （bufsize 默认 4096；可给 256 等小值）
 *
 * 输出：
 *   每行  "<ino> <d_type> <name>"（原始 d_type 数值；含 ". / .."）
 *   末尾  "COUNT=<n> UNIQUE=<n> DUP=<n> MISSING=<n>"
 *
 * 判重/判漏语义：
 *   COUNT   = 第一遍（bufsize 指定的小缓冲）返回的条目总数
 *   UNIQUE  = 第一遍名字去重后的数量
 *   DUP     = COUNT - UNIQUE（同名重复出现 = 续读重复 bug）
 *   MISSING = 第二遍（大缓冲 1MiB 作参照）独有、而第一遍未见的名字数
 *             （=0 表示小缓冲续读未漏条目；>0 高度可疑 = 跨缓冲漏读）
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

static std::vector<std::string> enum_dir(const char *path, size_t bufsize, bool verbose) {
    std::vector<std::string> names;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { fprintf(stderr, "error: open %s: %s\n", path, strerror(errno)); exit(2); }

    std::vector<char> buf(bufsize);
    for (;;) {
        long nread = syscall(SYS_getdents64, fd, buf.data(), buf.size());
        if (nread < 0) { fprintf(stderr, "error: getdents64: %s\n", strerror(errno)); close(fd); exit(2); }
        if (nread == 0) break;
        long bpos = 0;
        while (bpos < nread) {
            linux_dirent64 *d = (linux_dirent64 *)(buf.data() + bpos);
            if (d->d_reclen == 0) break;               /* 防御：0 长度 = 死循环 */
            bpos += d->d_reclen;
            if (verbose)
                printf("%llu %u %s\n",
                       (unsigned long long)d->d_ino, (unsigned)d->d_type, d->d_name);
            names.emplace_back(d->d_name);
        }
    }
    close(fd);
    return names;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <dir> [bufsize]\n", argv[0]);
        return 2;
    }
    size_t bufsize = (argc == 3) ? (size_t)strtoull(argv[2], nullptr, 0) : 4096;
    if (bufsize < 32) bufsize = 32;

    /* 第一遍：指定（小）缓冲，逐条打印 */
    std::vector<std::string> names = enum_dir(argv[1], bufsize, true);

    std::unordered_set<std::string> seen;
    for (const std::string &s : names) seen.insert(s);

    /* 第二遍：大缓冲作参照（续读边界几乎不出现） */
    std::vector<std::string> ref = enum_dir(argv[1], 1u << 20, false);
    std::unordered_set<std::string> refset;
    for (const std::string &s : ref) refset.insert(s);

    size_t missing = 0;
    for (const std::string &s : refset)
        if (!seen.count(s)) missing++;

    printf("COUNT=%zu UNIQUE=%zu DUP=%zu MISSING=%zu\n",
           names.size(), seen.size(), names.size() - seen.size(), missing);
    return 0;
}
