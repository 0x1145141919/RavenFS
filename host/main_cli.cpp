/* ═══════════════════════════════════════════════════════
 * RavenFS CLI 测试入口
 *
 * 裸 CLI 直接调 God Class，不经过 FUSE。
 * 用法：
 *   ./ravenfs_cli /tmp/test.img mkfs
 *   ./ravenfs_cli /tmp/test.img create /hello.txt
 *   ./ravenfs_cli /tmp/test.img ls /
 *   ./ravenfs_cli /tmp/test.img read /hello.txt
 * ═══════════════════════════════════════════════════════ */

#include "ravenfs/ravenfs.h"
#include "io_backend_posix.cpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <image> <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  mkfs                Format the image\n"
        "  create <path>       Create a new file\n"
        "  mkdir <path>        Create a directory\n"
        "  ls <path>           List directory contents\n"
        "  write <path> <data> Write text to file\n"
        "  read <path>         Read file contents\n"
        "  cat <path>          Alias for read\n"
        "  stat <path>         Show file metadata\n"
        , prog);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char* img_path = argv[1];
    const char* cmd = argv[2];

    PosixIOBackend io(img_path);
    RavenFS fs;
    Visitor who{0, 0}; /* root */

    if (strcmp(cmd, "mkfs") == 0) {
        /* 默认 64MB，后续支持 --size 参数 */
        if (!io.resize(64 * 1024 * 1024 / 4096)) {
            fprintf(stderr, "Failed to resize image\n");
            return 1;
        }
        if (!fs.mkfs(&io)) {
            fprintf(stderr, "mkfs failed\n");
            return 1;
        }
        printf("mkfs done: %s\n", img_path);
        return 0;
    }

    /* 其他命令需要先 mount */
    if (!fs.mount(&io)) {
        fprintf(stderr, "Mount failed (not formatted?)\n");
        return 1;
    }

    if (strcmp(cmd, "create") == 0 || strcmp(cmd, "touch") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        if (!fs.create_file(who, argv[3])) {
            fprintf(stderr, "create failed: %s\n", argv[3]);
            return 1;
        }
        printf("created: %s\n", argv[3]);

    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        if (!fs.mkdir(who, argv[3])) {
            fprintf(stderr, "mkdir failed: %s\n", argv[3]);
            return 1;
        }
        printf("mkdir: %s\n", argv[3]);

    } else if (strcmp(cmd, "ls") == 0) {
        const char* path = argc >= 4 ? argv[3] : "/";
        RavenFS::DirEntry entries[256];
        int n = fs.readdir(path, entries, 256);
        if (n < 0) {
            fprintf(stderr, "readdir failed: %s\n", path);
            return 1;
        }
        for (int i = 0; i < n; i++) {
            printf("%c %s\n",
                   entries[i].file_type == 1 ? 'd' : 'f',
                   entries[i].name);
        }

    } else if (strcmp(cmd, "write") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        FileID f = fs.open(who, argv[3]);
        if (f == INVALID_FILE_ID) {
            fprintf(stderr, "open failed: %s\n", argv[3]);
            return 1;
        }
        const char* data = argv[4];
        if (!fs.write(f, (void*)data, strlen(data), 0)) {
            fprintf(stderr, "write failed\n");
            fs.close(f);
            return 1;
        }
        fs.close(f);
        printf("wrote %zu bytes to %s\n", strlen(data), argv[3]);

    } else if (strcmp(cmd, "read") == 0 || strcmp(cmd, "cat") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        FileID f = fs.open(who, argv[3]);
        if (f == INVALID_FILE_ID) {
            fprintf(stderr, "open failed: %s\n", argv[3]);
            return 1;
        }
        char buf[4096];
        ssize_t n = fs.read(f, buf, sizeof(buf) - 1, 0) ? 4096 : -1;
        if (n < 0) {
            fprintf(stderr, "read failed\n");
            fs.close(f);
            return 1;
        }
        buf[n] = '\0';
        printf("%s", buf);
        fs.close(f);

    } else if (strcmp(cmd, "stat") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        FileID f = fs.open(who, argv[3]);
        if (f == INVALID_FILE_ID) {
            fprintf(stderr, "open failed: %s\n", argv[3]);
            return 1;
        }
        printf("TODO: stat for %s\n", argv[3]);
        fs.close(f);

    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}
