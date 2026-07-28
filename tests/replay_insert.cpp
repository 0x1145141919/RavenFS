#include "ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <set>
#include <sys/stat.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <device> <order_file> [options]\n"
        "Replay an insertion sequence saved by stress_mkdir_rev --order-file\n"
        "\n"
        "Options:\n"
        "  --until <n>           stop after n mkdir (default: all)\n"
        "  --probe <n1,n2,...>   validate + dump at these counts (before mkdir at that count)\n"
        "  --dump-dir <path>     dump directory (default: insert_dumps)\n"
        "  --validate            validate dentry tree after all insertions\n"
        , prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *order_file = argv[2];

    int until_n = 65536;
    const char *dump_dir = "insert_dumps";
    bool do_validate = false;
    std::set<int> probe_set;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            until_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            while (*p) {
                char *end;
                long seq = strtol(p, &end, 10);
                if (end == p) break;
                probe_set.insert((int)seq);
                p = end;
                if (*p == ',') p++;
            }
        } else if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--validate") == 0) {
            do_validate = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 读取插入顺序文件 */
    FILE *fp = fopen(order_file, "r");
    if (!fp) {
        fprintf(stderr, "FAIL: cannot open %s\n", order_file);
        return 1;
    }

    std::vector<std::string> sequence;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] == '\0')
            continue;
        sequence.push_back(line);
    }
    fclose(fp);

    int N = (int)sequence.size();
    fprintf(stderr, "replay_insert: loaded %d entries from %s\n", N, order_file);

    if (!probe_set.empty()) {
        fprintf(stderr, "replay_insert: probe at count");
        for (int s : probe_set) fprintf(stderr, " %d", s);
        fprintf(stderr, "\n");
        mkdir(dump_dir, 0755);
    }

    if (N == 0) {
        fprintf(stderr, "replay_insert: nothing to insert\n");
        return 0;
    }

    fprintf(stderr, "replay_insert: mounting %s\n", device);
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) {
        fprintf(stderr, "FAIL: mount code=%lu\n", (unsigned long)rc);
        return 1;
    }

    /* 创建根测试目录 */
    int ret = fs.mkdir("/stress_test");
    if (ret != 0) {
        fprintf(stderr, "FAIL: mkdir /stress_test returned %d\n", ret);
        return 1;
    }

    int limit = until_n < N ? until_n : N;
    int err_count = 0;

    for (int idx = 0; idx < limit; idx++) {
        const char *full = sequence[idx].c_str();

        /* 插桩：插入前校验 */
        if (probe_set.find(idx) != probe_set.end()) {
            char dump_path[512];
            snprintf(dump_path, sizeof(dump_path), "%s/dump_%06d.txt", dump_dir, idx);
            FILE *df = fopen(dump_path, "w");
            if (df) {
                fprintf(stderr, "  probe %d: dumping dentry tree to %s\n", idx, dump_path);
                fs.dump_dentry_tree(df);
                fclose(df);
            }
            fprintf(stderr, "  probe %d: validate\n", idx);
            fs.validate_dentry_tree();
        }

        ret = fs.mkdir(full);
        if (ret != 0) {
            fprintf(stderr, "\nMKDIR FAIL [%d/%d]: %s returned %d\n",
                    idx, limit, full, ret);
            err_count++;
            if (err_count >= 5) {
                fprintf(stderr, "  (too many errors, aborting)\n");
                return 1;
            }
            continue;
        }

        if (idx % 8192 == 0 && idx > 0)
            fprintf(stderr, "  created %d directories...\n", idx);
    }

    fprintf(stderr, "INSERT done: %d/%d dirs, %d errors\n", limit, N, err_count);

    if (do_validate) {
        fprintf(stderr, "replay_insert: validating dentry tree...\n");
        int e = fs.validate_dentry_tree();
        fprintf(stderr, "replay_insert: validate %s (%d errors)\n",
                e == 0 ? "PASS" : "FAIL", e);
        if (e > 0) return 1;
    }

    return 0;
}
