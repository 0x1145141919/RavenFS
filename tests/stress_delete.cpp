#include "ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>

#define CHECKPOINT_ARR { 10000, 20000, 30000, 40000, 50000 }
#define CHECKPOINT_FINE_ARR { }

static int g_cps[] = CHECKPOINT_ARR;
static int g_cp_fine[] = CHECKPOINT_FINE_ARR;

static int enum_dir(RavenFS &fs, const char *path,
                    std::vector<std::string> &out)
{
    std::vector<RavenFS::DirEntryInfo> buf(65536 + 2);
    int n = fs.readdir(path, buf.data(), (int)buf.size());
    if (n < 0) return n;

    out.clear();
    for (int i = 0; i < n; i++) {
        if (strcmp(buf[i].name, ".") == 0) continue;
        if (strcmp(buf[i].name, "..") == 0) continue;
        out.push_back(buf[i].name);
    }
    return (int)out.size();
}

static bool need_validate(int i) {
    for (int cp : g_cps)
        if (i == cp) return true;
    for (int cp : g_cp_fine)
        if (i == cp) return true;
    return false;
}

static int silent_validate(RavenFS &fs) {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    dup2(null_fd, STDOUT_FILENO);
    close(null_fd);

    int err = fs.validate_dentry_tree();

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    return err;
}

static int loud_validate(RavenFS &fs, int seq, const char *label) {
    fprintf(stderr, "\n═══ validate at %s (seq=%d) ═══\n\n", label, seq);
    return fs.validate_dentry_tree();
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <device> [options]\n"
        "Options:\n"
        "  --sequential         delete in readdir order (default: shuffled)\n"
        "  --seed <n>           random seed for shuffle (default: 42)\n"
        "  --order-file <path>  path to save/read deletion order (default: delete_order.txt)\n"
        , prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    bool sequential = false;
    int seed = 42;
    const char *order_file = "delete_order.txt";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--sequential") == 0) {
            sequential = true;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--order-file") == 0 && i + 1 < argc) {
            order_file = argv[++i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "stress_delete: mounting %s\n", device);
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) {
        fprintf(stderr, "FAIL: mount code=%lu\n", (unsigned long)rc);
        return 1;
    }

    std::vector<std::string> names;
    int N = enum_dir(fs, "/stress_test", names);
    if (N < 0) {
        fprintf(stderr, "FAIL: readdir /stress_test returned %d\n", N);
        return 1;
    }
    fprintf(stderr, "stress_delete: found %d entries\n", N);
    if (N == 0) {
        fprintf(stderr, "stress_delete: nothing to delete\n");
        return 0;
    }

    /* 生成完整路径列表，names 天然是 B+tree 字典序 */
    std::vector<std::string> full_paths;
    full_paths.reserve(N);
    for (int i = 0; i < N; i++) {
        char full[256];
        snprintf(full, sizeof(full), "/stress_test/%s", names[i].c_str());
        full_paths.push_back(full);
    }

    /* 删除顺序：默认乱序并保存到文件 */
    std::vector<int> order(N);
    for (int i = 0; i < N; i++) order[i] = i;

    if (!sequential) {
        std::mt19937 g(seed);
        std::shuffle(order.begin(), order.end(), g);

        FILE *fp = fopen(order_file, "w");
        if (!fp) {
            fprintf(stderr, "FAIL: cannot write %s\n", order_file);
            return 1;
        }
        for (int i = 0; i < N; i++)
            fprintf(fp, "%s\n", full_paths[order[i]].c_str());
        fclose(fp);
        fprintf(stderr, "stress_delete: shuffled (seed=%d), order saved to %s\n",
                seed, order_file);
    } else {
        fprintf(stderr, "stress_delete: sequential delete\n");
    }

    /* baseline */
    {
        int e = silent_validate(fs);
        if (e > 0) { fprintf(stderr, "FAIL: baseline validate %d errors\n", e); return 1; }
        fprintf(stderr, "  baseline validate OK\n");
    }

    int err_count = 0;
    for (int idx = 0; idx < N; idx++) {
        int i = sequential ? idx : order[idx];
        const char *full = full_paths[i].c_str();

        int ret = fs.rmdir(full);
        if (ret != 0) {
            fprintf(stderr, "\nRMDIR FAIL [%d/%d]: %s returned %d\n",
                    idx, N, full, ret);
            loud_validate(fs, idx, "post-rmdir-fail");
            err_count++;
            if (err_count >= 5) return 1;
            continue;
        }

        if (need_validate(idx)) {
            int e = silent_validate(fs);
            if (e > 0) {
                fprintf(stderr, "\nVALIDATE FAIL [%d/%d]: %d errors\n", idx, N, e);
                loud_validate(fs, idx, "post-validate-fail");
                return 1;
            }
            fprintf(stderr, "  cp %d validate OK\n", idx);
        }

        if (idx % 2000 == 0 && idx > 0)
            fprintf(stderr, "  progress %d/%d\n", idx, N);
    }

    fprintf(stderr, "DELETE done: %d dirs, %d errors\n", N, err_count);

    RavenFS::DirEntryInfo entries[64];
    int n = fs.readdir("/stress_test", entries, 64);
    if (n <= 2)
        fprintf(stderr, "VERIFY: /stress_test has %d entries (. + ..)\n", n);
    else {
        fprintf(stderr, "FAIL: /stress_test still has %d entries (expected <=2)\n", n);
        return 1;
    }

    if (fs.rmdir("/stress_test") != 0) {
        fprintf(stderr, "FAIL: rmdir /stress_test\n");
        return 1;
    }
    fprintf(stderr, "  removed /stress_test\n");

    {
        int e = silent_validate(fs);
        fprintf(stderr, "final validate: %s (%d errors)\n",
                e == 0 ? "PASS" : "FAIL", e);
    }

    fprintf(stderr, "SUCCESS\n");
    return 0;
}
