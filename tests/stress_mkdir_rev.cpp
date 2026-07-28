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
 *   4. 支持 --until N 在 N 次后停止（用于二分定位）
 *   5. 支持 --order-file 导出插入顺序
 *   6. 支持 --validate 在结束后校验 B+tree
 * ═══════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <device> [options]\n"
        "Options:\n"
        "  --seed <n>           random seed (default: 42)\n"
        "  --until <n>          stop after n mkdir (default: 65536)\n"
        "  --order-file <path>  save insertion order to file\n"
        "  --validate           validate dentry tree after insertion\n"
        , prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    int seed = 42;
    int until_n = 65536;
    const char *order_file = nullptr;
    bool do_validate = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            until_n = atoi(argv[++i]);
            if (until_n <= 0 || until_n > 65536) until_n = 65536;
        } else if (strcmp(argv[i], "--order-file") == 0 && i + 1 < argc) {
            order_file = argv[++i];
        } else if (strcmp(argv[i], "--validate") == 0) {
            do_validate = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

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
    int N = 65536;
    std::vector<std::string> names;
    names.reserve(N);
    for (int i = 0; i < N; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/stress_test/dir_%05d", i);
        names.push_back(path);
    }

    /* 打乱（固定种子确保可复现） */
    {
        std::mt19937 g(seed);
        std::shuffle(names.begin(), names.end(), g);
    }
    fprintf(stderr, "stress_mkdir_rev: shuffled %d names (seed=%d, until=%d)\n",
            N, seed, until_n);

    /* 保存插入顺序 */
    if (order_file) {
        FILE *fp = fopen(order_file, "w");
        if (!fp) {
            fprintf(stderr, "FAIL: cannot write %s\n", order_file);
            return 1;
        }
        for (int i = 0; i < N; i++)
            fprintf(fp, "%s\n", names[i].c_str());
        fclose(fp);
        fprintf(stderr, "stress_mkdir_rev: order saved to %s\n", order_file);
    }

    /* 批量创建子目录 */
    int err_count = 0;
    int limit = until_n < N ? until_n : N;
    for (int i = 0; i < limit; i++) {
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

    fprintf(stderr, "stress_mkdir_rev: created %d directories under /stress_test (shuffled order)\n", limit);

    if (do_validate) {
        fprintf(stderr, "stress_mkdir_rev: validating dentry tree...\n");
        int e = fs.validate_dentry_tree();
        fprintf(stderr, "stress_mkdir_rev: validate %s (%d errors)\n",
                e == 0 ? "PASS" : "FAIL", e);
        if (e > 0) return 1;
    }

    return 0;
}
