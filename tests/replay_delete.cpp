#include "ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <set>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define CHECKPOINT_ARR { 10000, 20000, 30000, 40000, 50000 }

static int g_cps[] = CHECKPOINT_ARR;

static bool need_validate(int i) {
    for (int cp : g_cps)
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
        "Usage: %s <device> <order_file> [options]\n"
        "Replay a deletion sequence previously saved by stress_delete --order-file\n"
        "\n"
        "Options:\n"
        "  --probe <seq1,seq2,...>   全量 dump B+tree 到文件（删除前插桩）\n"
        "  --dump-dir <path>          dump 目录（默认 dentry_dumps）\n"
        , prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *order_file = argv[2];

    /* 解析 probe 参数 */
    std::set<int> probe_set;
    const char *dump_dir = "dentry_dumps";

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
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
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 读取删除顺序文件 */
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
    fprintf(stderr, "replay_delete: loaded %d entries from %s\n", N, order_file);

    if (!probe_set.empty()) {
        fprintf(stderr, "replay_delete: probe at seq");
        for (int s : probe_set) fprintf(stderr, " %d", s);
        fprintf(stderr, "\n");
        mkdir(dump_dir, 0755);
    }

    if (N == 0) {
        fprintf(stderr, "replay_delete: nothing to replay\n");
        return 0;
    }

    fprintf(stderr, "replay_delete: mounting %s\n", device);
    uint64_t rc;
    RavenFS fs(device, &rc);
    if (rc) {
        fprintf(stderr, "FAIL: mount code=%lu\n", (unsigned long)rc);
        return 1;
    }

    /* baseline */
    {
        int e = silent_validate(fs);
        if (e > 0) { fprintf(stderr, "FAIL: baseline validate %d errors\n", e); return 1; }
        fprintf(stderr, "  baseline validate OK\n");
    }

    int err_count = 0;
    for (int idx = 0; idx < N; idx++) {
        const char *full = sequence[idx].c_str();

        /* 插桩：删除前全量 dump + 校验 routing key */
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

    fprintf(stderr, "REPLAY done: %d dirs, %d errors\n", N, err_count);

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
