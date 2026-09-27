// SPDX-License-Identifier: GPL-2.0
/*
 * rfsdirparallel.c —— RavenFS **并行性**压测：N 个目录下并发乱序 mkdir/rmdir
 *
 * 用法:  rfsdirparallel <ravenfs-root> [NWORKERS] [OPS_PER_WORKER]
 *        默认 NWORKERS=6, OPS_PER_WORKER=20000
 *
 * 目的：看**并行性**——VFS 对**同一父目录**的改操作经父 i_rwsem 串行；
 *   **不同目录**应可并行（ravenfs 侧：不同 dentry 树 / 不同 mdata 分片）。
 *   每个 worker 独占一个子目录 <root>/dN，独立**乱序** churn：
 *   维护 LIVE 个存活名，每步随机「建新 / 删旧」（= 真正乱序的 mkdir/rmdir 混合）。
 *
 * 判读：
 *   · 并行良好：总墙钟 ≈ 单 worker 耗时（有富余核时）；各 worker 自身耗时接近。
 *   · 串行：    总墙钟 ≈ Σ 各 worker 耗时（线性叠加）。
 *   → 跑 1 worker 与 6 worker 各一次横向对比即得并行度。
 *
 * 纪律：
 *   1) statfs 校验 f_type == RAVENFS_SUPER_MAGIC（"RAVN"），非 ravenfs 直接拒。
 *   2) 要求 <root> 为空（否则计数失准）。
 *   3) 结束断言各 worker 子目录已删净（readdir 回空）；打印 created/removed/err。
 *
 * 宿主 `gcc -O2 -static -pthread` 编译后投递 guest；<root> 在 ravenfs 挂载点内。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#ifndef RAVENFS_SUPER_MAGIC
#define RAVENFS_SUPER_MAGIC 0x5241564EUL	/* "RAVN" */
#endif

#define LIVE		512		/* 每 worker 存活名上限（决定 rmdir 空间） */
#define NAME_BYTES	24

static char		g_root[4000];
static int		g_nw;
static long		g_ops;
static pthread_barrier_t g_bar;
static volatile long	g_created, g_removed, g_benign, g_err;

struct worker {
	int		id;
	double		secs;
	long		done;
	pthread_t	t;
};

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* xorshift64：可复现（种子按 worker id） */
static inline uint64_t rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static long dir_entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	long n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

static void *worker(void *arg)
{
	struct worker *w = arg;
	char dir[4096];
	char path[4096];
	char (*live)[NAME_BYTES] = calloc(LIVE, NAME_BYTES);
	long nlive = 0;
	uint64_t s = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(w->id + 1) * 0x100000001B3ULL);
	double t0;

	snprintf(dir, sizeof dir, "%s/d%d", g_root, w->id);
	snprintf(path, sizeof path, "%s", dir);

	pthread_barrier_wait(&g_bar);		/* 发令：尽量同时起跑 */
	t0 = now_s();

	for (long i = 0; i < g_ops; i++) {
		uint64_t r = rng(&s);
		int do_create = (nlive < LIVE) && (nlive == 0 || (r & 1));

		if (do_create) {
			snprintf(live[nlive], NAME_BYTES, "f%09llu",
				 (unsigned long long)(rng(&s) % 1000000000ULL));
			snprintf(path, sizeof path, "%s/%s", dir, live[nlive]);
			if (mkdir(path, 0777) == 0) {
				nlive++;
				__sync_fetch_and_add(&g_created, 1);
			} else if (errno == EEXIST) {
				__sync_fetch_and_add(&g_benign, 1);	/* 随机重名：无害 */
			} else {
				__sync_fetch_and_add(&g_err, 1);
			}
		} else if (nlive > 0) {
			long k = (long)(rng(&s) % (uint64_t)nlive);
			snprintf(path, sizeof path, "%s/%s", dir, live[k]);
			if (rmdir(path) == 0) {
				nlive--;
				memcpy(live[k], live[nlive], NAME_BYTES);	/* swap-remove */
				__sync_fetch_and_add(&g_removed, 1);
			} else {
				__sync_fetch_and_add(&g_err, 1);
			}
		} else {
			__sync_fetch_and_add(&g_benign, 1);
		}
	}
	w->secs = now_s() - t0;
	w->done = nlive;

	/* 清尾：把自己目录删净 */
	for (long k = 0; k < nlive; k++) {
		snprintf(path, sizeof path, "%s/%s", dir, live[k]);
		if (rmdir(path) == 0)
			__sync_fetch_and_add(&g_removed, 1);
	}
	free(live);
	return NULL;
}

int main(int argc, char **argv)
{
	struct statfs sf;
	struct worker *ws;
	double t0, wall, ops_s;
	long total_created, total_removed, total_err, total_benign;
	int i, bad = 0;

	if (argc < 2) {
		fprintf(stderr, "用法: %s <ravenfs-root> [NWORKERS=6] [OPS_PER_WORKER=20000]\n", argv[0]);
		return 2;
	}
	snprintf(g_root, sizeof g_root, "%s", argv[1]);
	g_nw = (argc > 2) ? atoi(argv[2]) : 6;
	g_ops = (argc > 3) ? atol(argv[3]) : 20000;
	if (g_nw < 1 || g_nw > 256 || g_ops < 1) {
		fprintf(stderr, "参数非法\n");
		return 2;
	}

	if (statfs(g_root, &sf) != 0) {
		fprintf(stderr, "statfs(%s) 失败: %s\n", g_root, strerror(errno));
		return 1;
	}
	if ((unsigned long)sf.f_type != RAVENFS_SUPER_MAGIC) {
		fprintf(stderr, "拒绝：%s 非 ravenfs（f_type=0x%lx, 期望 0x%lx）\n",
			g_root, (unsigned long)sf.f_type, RAVENFS_SUPER_MAGIC);
		return 1;
	}
	if (dir_entries(g_root) != 0) {
		fprintf(stderr, "拒绝：%s 非空\n", g_root);
		return 1;
	}

	ws = calloc(g_nw, sizeof(*ws));
	pthread_barrier_init(&g_bar, NULL, g_nw);
	for (i = 0; i < g_nw; i++) {
		char d[4096];

		ws[i].id = i;
		snprintf(d, sizeof d, "%s/d%d", g_root, i);
		if (mkdir(d, 0777) != 0) {
			fprintf(stderr, "mkdir %s 失败: %s\n", d, strerror(errno));
			return 1;
		}
	}

	t0 = now_s();
	for (i = 0; i < g_nw; i++)
		pthread_create(&ws[i].t, NULL, worker, &ws[i]);
	for (i = 0; i < g_nw; i++)
		pthread_join(ws[i].t, NULL);
	wall = now_s() - t0;

	total_created = g_created;
	total_removed = g_removed;
	total_err = g_err;
	total_benign = g_benign;
	ops_s = (double)(total_created + total_removed) / wall;

	printf("=== rfsdirparallel: workers=%d ops/worker=%ld ===\n", g_nw, g_ops);
	for (i = 0; i < g_nw; i++)
		printf("  worker[%d]: secs=%.3f 残留=%ld\n", ws[i].id, ws[i].secs, ws[i].done);
	printf("TOTAL wall=%.3fs  created=%ld removed=%ld benign=%ld err=%ld\n",
	       wall, total_created, total_removed, total_benign, total_err);
	printf("aggregate ops/s = %.0f\n", ops_s);

	/* ratio：wall / max(worker 自身耗时)。≈1 并行好；≈workers 完全串行 */
	{
		double mx = 0.0;

		for (i = 0; i < g_nw; i++)
			if (ws[i].secs > mx)
				mx = ws[i].secs;
		printf("        wall/worker_max = %.2f  (≈1 并行良好；≈%d 完全串行)\n",
		       mx > 0 ? wall / mx : 0.0, g_nw);
	}

	for (i = 0; i < g_nw; i++) {
		char d[4096];
		long n;

		snprintf(d, sizeof d, "%s/d%d", g_root, i);
		n = dir_entries(d);
		if (n != 0) {
			printf("  !! d%d 未删净: %ld\n", i, n);
			bad = 1;
		}
		rmdir(d);
	}
	printf("leftover(root)=%ld  %s\n", dir_entries(g_root), bad ? "RESULT=BAD" : "RESULT=OK");
	pthread_barrier_destroy(&g_bar);
	free(ws);
	return bad ? 1 : 0;
}
