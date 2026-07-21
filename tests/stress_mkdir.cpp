#include "Userspace_God.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ═══════════════════════════════════════════════════════════════
 * 压测：在 /stress_test 下创建 65536 个子目录
 *
 * 用法:
 *   ./stress_mkdir <device>
 *
 * 前置: device 已由 mkfs.ravenfs 格式化
 * 任何 mkdir 错误立即停机（返回 1）
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const int N = 65536;

    fprintf(stderr, "stress_mkdir: mounting %s\n", device);

    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) {
        fprintf(stderr, "FAIL: mount returned code=%lu\n", (unsigned long)rc);
        return 1;
    }

    /* 创建根测试目录 */
    int ret = fs.mkdir("/stress_test");
    if (ret != 0) {
        fprintf(stderr, "FAIL: mkdir /stress_test returned %d\n", ret);
        return 1;
    }
    fprintf(stderr, "stress_mkdir: created /stress_test\n");

    /* 批量创建子目录 */
    char path[128];
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof(path), "/stress_test/dir_%05d", i);
        ret = fs.mkdir(path);
        if (ret != 0) {
            fprintf(stderr, "FAIL: mkdir %s returned %d at count=%d\n",
                    path, ret, i);
            return 1;
        }
        if (i % 8192 == 0 && i > 0)
            fprintf(stderr, "  created %d directories...\n", i);
    }

    fprintf(stderr, "SUCCESS: created %d directories under /stress_test\n", N);
    return 0;
}
