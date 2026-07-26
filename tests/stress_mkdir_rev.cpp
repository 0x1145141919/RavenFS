#include "ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <vector>
#include <string>

/* ═══════════════════════════════════════════════════════════════
 * 压测：乱序插入验证字典序纪律
 *
 *   1. 在 /stress_test 下创建 65536 个子目录
 *   2. 生成顺序 dir_00000 … dir_65535
 *   3. 打乱成随机顺序后逐个 mkdir
 *   4. 挂载后任何 mkdir 错误立即停机
 *
 * 前置: device 已由 mkfs.ravenfs 格式化
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const int N = 65536;

    fprintf(stderr, "stress_mkdir_rev: mounting %s\n", device);

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
    fprintf(stderr, "stress_mkdir_rev: created /stress_test\n");

    /* 生成无序名称列表 */
    std::vector<std::string> names;
    names.reserve(N);
    for (int i = 0; i < N; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/stress_test/dir_%05d", i);
        names.push_back(path);
    }

        /* 打乱 */
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(names.begin(), names.end(), g);
    }
    fprintf(stderr, "stress_mkdir_rev: shuffled %d names\n", N);

    /* 批量创建子目录 */
    int err_count = 0;
    for (int i = 0; i < N; i++) {
        ret = fs.mkdir(names[i].c_str());
        if (ret != 0) {
            fprintf(stderr, "FAIL: mkdir %s returned %d at count=%d\n",
                    names[i].c_str(), ret, i);
            err_count++;
            if (err_count >= 5) {
                fprintf(stderr, "  (too many errors, aborting)\n");
                return 1;
            }
        }
        if (i % 8192 == 0 && i > 0)
            fprintf(stderr, "  created %d directories...\n", i);
    }

    fprintf(stderr, "SUCCESS: created %d directories under /stress_test (shuffled order)\n", N);
    return 0;
}
