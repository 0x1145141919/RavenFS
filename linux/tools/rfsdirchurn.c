// SPDX-License-Identifier: GPL-2.0
/*
 * rfsdirchurn.c —— RavenFS 名字堆(name_heap)「逐组精准爆破」用户态压测
 *
 * 用法:  rfsdirchurn <ravenfs-dir> [ROUNDS]
 *
 * ── 为什么重写（2026-09-27）──────────────────────────────────────────────
 * 旧版是「泛参数 churn」：BASE COUNT NAMELEN SEED [CYCLES …]。问题：
 *   · 参数随手给 → 可能在**短名**（≤40B，压根不走 name_heap）上白跑，
 *     也可能名字长度不落在任何粒度档的边界，测不出堆的上限/碎片行为；
 *   · 不校验被测路径**确是 ravenfs**（在 ext4/tmpfs 上跑完一脸茫然）；
 *   · 不记内存基线、删完只采一次 → 分不清「正常释放」还是「泄漏/滞后」。
 *
 * 重写原则（构造集硬编码，入参只留一个路径）：
 *   1) 入参仅 1 个待测目录；其余常量全部编译期内定。
 *   2) 先 statfs() 校验 `f_type == RAVENFS_SUPER_MAGIC`（"RAVN"）——非 ravenfs 直接拒。
 *      并要求该目录**为空**（否则条目计数与堆边界都会失准）。
 *   3) 构造集 = name_heap 全部 **5 个粒度组逐一精准打穿**（镜像 `linux/name_heap.c`
 *      的 `rfs_heap_gran_log2` / `rfs_heap_bits_log2` / `rfs_heap_group_*`）。
 *      目录 inode 内联 10 个 heap controller（每档一组），**同一目录**内依次把每组灌满：
 *        G 粒度  名字长  尾长  units  逐槽容量(bit/units)
 *        0  8B    80B    40    5     13107 + 26214 + 52428  =  91749   ← 跨槽碎片台阶
 *        1  64B  104B    64    1     65536 + 131072        = 196608
 *        2 128B  168B   128    1     65536 + 131072        = 196608
 *        3 256B  296B   256    1     65536 + 131072        = 196608
 *        4 512B  552B   512    1     65536                 =  65536
 *        合计 = 747109 条（= 需 inode 数）
 *      「精准」= 每组插 cap+1 条，断言**恰好 cap 成功 + 1 条 ENOSPC**（ENOSPC 由
 *      跨槽碎片决定，非盘满）。G0 的 91749 是逐槽 floor 而非 458752/5=91750，此即碎片台阶。
 *      G0 量级（≈1456 叶）还会**越过内裂边界**（叶→内节点→根升长），顺带压 `rfs_apply_up`。
 *   4) **每组**的插入序、删除序各自独立乱序（PCA RNG，固定种子 → 可复现）。
 *   5) 压测前记基线：/proc/meminfo 的 MemAvailable/MemFree/Slab + statfs bfree/ffree；
 *      并预检 statfs.f_files ≥ 所需 inode（不足 → 提示重 mkfs，不白跑）。
 *   6) **每组删净**（readdir 回空）后 syncfs，断言 bfree/ffree 精确回基线 —— 定位到组的泄漏。
 *   7) 末轮删净后按 ≥2 个时间点（默认 +2s / +6s / +12s）采样内存：
 *      逐点回升=释放滞后，持续低于基线=真泄漏。
 *   8) 跑 ROUNDS 轮（默认 2）：第 2 轮能否再次灌满 = 目录变空时
 *      `ravenfs_dir_name_heap_release` 是否真归还（历史缺陷正是「第二轮整体 ENOSPC」）。
 *
 * ⚠ 需要 inode ≥ 747117：4G 盘请 `mkfs.ravenfs --inode-ratio=1`；默认 ratio=8 需 ≥24.5G。
 * 宿主 `gcc -static` 编译后投递 guest；目标目录在 ravenfs 挂载点内（自建空目录）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>

/* ── 硬编码：name_heap 几何（镜像 linux/name_heap.c；改动务必同步）────────── */
/* gran_log2[10] = {3,3,3, 6,6, 7,7, 8,8, 9}     → 8B/64B/128B/256B/512B */
/* bits_log2[10] = {16,17,18, 16,17, 16,17, 16,17, 16} → 2^16/2^17/2^18 bit */
static const unsigned char HEAP_BITS_LOG2[10] = { 16, 17, 18, 16, 17, 16, 17, 16, 17, 16 };
static const unsigned char GROUP_LO[5] = { 0, 3, 5, 7, 9 };
static const unsigned char GROUP_HI[5] = { 3, 5, 7, 9, 10 };
static const char *const GROUP_TAG[5] = { "8B", "64B", "128B", "256B", "512B" };

/* 构造集：每组一个定长名字（尾长取该档带、units 见 rfs_heap_pick） */
struct plan {
	unsigned name_len;
};
static const struct plan PLAN[5] = {
	{ 80 },		/* tail 40 → 8B 档, units 5 */
	{ 104 },	/* tail 64 → 64B 档, units 1 */
	{ 168 },	/* tail 128 → 128B 档, units 1 */
	{ 296 },	/* tail 256 → 256B 档, units 1 */
	{ 552 },	/* tail 512 → 512B 档, units 1（=RAVENFS_MAX_NAME_LEN） */
};

#define RAVENFS_SUPER_MAGIC	0x5241564EUL	/* "RAVN"，与 include/magics.h 同源 */
#define DENTRY_INLINE_NAME_MAX	40u
#define MAX_NL			552u

#define DEFAULT_ROUNDS	2
/* 删净后内存采样点（秒）；≥2 个，观察释放滞后 + 泄漏 */
static const int POST_SAMPLE_SEC[] = { 2, 6, 12 };
#define N_POST_SAMPLE	((int)(sizeof(POST_SAMPLE_SEC) / sizeof(POST_SAMPLE_SEC[0])))

/* ── name_heap 归属判定镜像（rfs_heap_pick）─────────────────────────────── */
static int heap_pick(unsigned tail, unsigned *units, unsigned *gran_log2)
{
	if (tail <= 40)  { *units = (tail + 7) / 8; *gran_log2 = 3; return 0; }
	if (tail <= 64)  { *units = 1; *gran_log2 = 6; return 1; }
	if (tail <= 128) { *units = 1; *gran_log2 = 7; return 2; }
	if (tail <= 256) { *units = 1; *gran_log2 = 8; return 3; }
	*units = 1; *gran_log2 = 9; return 4;
}
/* 逐槽 floor(bits/units) 求和 = 该组精确容量（跨槽碎片即体现在此） */
static long group_cap(int g, unsigned units)
{
	long cap = 0;
	int s;

	for (s = GROUP_LO[g]; s < GROUP_HI[g]; s++)
		cap += (1L << HEAP_BITS_LOG2[s]) / (long)units;
	return cap;
}

/* ── 工具 ─────────────────────────────────────────────────────────────── */
static unsigned long long rng_state = 0x9E3779B97F4A7C15ULL;
static unsigned long long rng(void)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return rng_state >> 11;
}
static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
/* 组内定长名字：前 40B 内联，其余走堆；前缀含组号防跨组撞名 */
static void mkname(char *b, int g, unsigned long long i, unsigned nl)
{
	int h = snprintf(b, nl + 1, "%c%012llu", (char)('a' + g), i);
	int k;

	for (k = h; k < (int)nl; k++)
		b[k] = (char)('a' + (int)((i + (unsigned)k + (unsigned)g) % 26));
	b[nl] = 0;
}
static void shuffle(unsigned long long *a, long n)
{
	long i;

	for (i = n - 1; i > 0; i--) {
		long j = (long)(rng() % (unsigned long long)(i + 1));
		unsigned long long t = a[i];

		a[i] = a[j];
		a[j] = t;
	}
}
static void do_syncfs(const char *base)
{
	int fd = open(base, O_RDONLY | O_DIRECTORY);

	if (fd >= 0) {
		syncfs(fd);
		close(fd);
	}
}
static long mem_kb(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256];
	size_t kl = strlen(key);
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, kl)) {
			sscanf(line + kl, ": %ld", &v);
			break;
		}
	}
	fclose(f);
	return v;
}
static void mem_report(const char *tag)
{
	long av = mem_kb("MemAvailable"), fr = mem_kb("MemFree"), sl = mem_kb("Slab");

	printf("  [mem] %-12s MemAvailable=%ldMB MemFree=%ldMB Slab=%ldMB\n", tag,
	       av >= 0 ? av / 1024 : -1, fr >= 0 ? fr / 1024 : -1, sl >= 0 ? sl / 1024 : -1);
}
static long count_entries(const char *base)
{
	DIR *d = opendir(base);
	struct dirent *e;
	long n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			n++;
	}
	closedir(d);
	return n;
}

struct fssnap {
	unsigned long long bfree, ffree, blocks, files;
};

static int fs_snap(const char *base, struct fssnap *s)
{
	struct statfs st;

	if (statfs(base, &st) != 0)
		return -1;
	s->blocks = (unsigned long long)st.f_blocks;
	s->bfree = (unsigned long long)st.f_bfree;
	s->files = (unsigned long long)st.f_files;
	s->ffree = (unsigned long long)st.f_ffree;
	return 0;
}

int main(int argc, char **argv)
{
	const char *base;
	int rounds = DEFAULT_ROUNDS;
	struct stat st;
	struct statfs sf;
	struct fssnap base_fs, end_fs;
	int g, i, r, fs_counters_ok = 1;
	long total = 0, peak = 0; /* total=各组之和；peak=同一时刻并发占用（逐组即最大值） */
	char *name, *path;
	unsigned long long *slot_map, *created;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <ravenfs-dir> [ROUNDS]\n", argv[0]);
		return 2;
	}
	base = argv[1];
	if (argc == 3)
		rounds = atoi(argv[2]);

	/* ── 1. 校验：确是 ravenfs 上的空目录 ────────────────────────────── */
	if (stat(base, &st) != 0) {
		perror("stat");
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "✗ %s 不是目录\n", base);
		return 1;
	}
	if (statfs(base, &sf) != 0) {
		perror("statfs");
		return 1;
	}
	if ((unsigned long)sf.f_type != RAVENFS_SUPER_MAGIC) {
		fprintf(stderr, "✗ %s 不是 ravenfs（f_type=0x%lx，期望 0x%lx \"RAVN\"）\n",
			base, (unsigned long)sf.f_type, RAVENFS_SUPER_MAGIC);
		return 1;
	}
	if (count_entries(base) != 0) {
		fprintf(stderr, "✗ %s 非空；精准爆破要求干净的空目录\n", base);
		return 1;
	}
	printf("ravenfs dir OK: %s (f_type=0x%lx)\n\n", base, (unsigned long)sf.f_type);

	/* ── 2. 由几何推导各组精确台阶 ───────────────────────────────────── */
	printf("plan（同一目录内 5 组逐一打穿；name_heap 几何镜像 name_heap.c）:\n");
	for (g = 0; g < 5; g++) {
		unsigned tail = PLAN[g].name_len - DENTRY_INLINE_NAME_MAX;
		unsigned units, gran;
		int pg = heap_pick(tail, &units, &gran);
		long cap = group_cap(g, units);
		char per[128];
		int s, off = 0;

		if (pg != g || units * (1u << gran) != tail) {
			fprintf(stderr, "✗ 构造集自检失败 G%d（name=%u → group=%d units=%u gran=%u）\n",
				g, PLAN[g].name_len, pg, units, gran);
			return 1;
		}
		for (s = GROUP_LO[g]; s < GROUP_HI[g]; s++)
			off += snprintf(per + off, sizeof(per) - off, "%s%ld",
					s == GROUP_LO[g] ? "" : " + ",
					(1L << HEAP_BITS_LOG2[s]) / (long)units);
		printf("  G%d %-4s name=%3uB tail=%3u units=%u  = %-24s = %6ld\n",
		       g, GROUP_TAG[g], PLAN[g].name_len, tail, units, per, cap);
		total += cap;
		if (cap > peak)
			peak = cap;
	}
	printf("→ 五组合计 %ld 条；逐组爆破的**并发峰值** = %ld（另加 1 个宿主目录）\n",
	       total, peak);

	/* ── 3. 基线 + inode 预检 ───────────────────────────────────────── */
	/* ⚠ statfs 读的是**盘上位图**（ioctl/sb_bread，只有 sync_fs 才落盘）→
	   先把当前挂载态刷盘，否则「基线」漏掉尚未落盘的分配（典型：刚 mkdir 的
	   测试目录自身 = 1 inode + 2 块）→ 首轮 syncfs 后凭空出现 Δ-2 块/Δ-1 inode 假泄漏。 */
	{
		int sfd = open(base, O_RDONLY | O_DIRECTORY);
		if (sfd >= 0) { syncfs(sfd); close(sfd); }
	}
	if (fs_snap(base, &base_fs) != 0) {
		perror("statfs(baseline)");
		return 1;
	}
	if (base_fs.blocks == 0 && base_fs.files == 0) {
		fs_counters_ok = 0;
		fprintf(stderr, "⚠ statfs 未提供块/inode 计数 → 跳过块级泄漏断言，仅靠内存采样\n");
	}
	if (base_fs.files > 0 && (long)base_fs.files < peak + 64) {
		fprintf(stderr, "✗ inode 不足：fs 共 %llu，并发峰值需 ≥ %ld → 重新 mkfs（--inode-ratio 调小）\n",
			base_fs.files, peak + 64);
		return 1;
	}
	printf("\nbaseline: bfree=%llu ffree=%llu (blocks=%llu files=%llu)\n",
	       base_fs.bfree, base_fs.ffree, base_fs.blocks, base_fs.files);
	mem_report("baseline");

	name = malloc(MAX_NL + 1);
	path = malloc(strlen(base) + MAX_NL + 2);
	if (!name || !path) {
		fprintf(stderr, "OOM\n");
		return 1;
	}

	/* ── 4. ROUNDS 轮 × 5 组：乱序插入 → 乱序删除 → 回基线 ────────────── */
	for (r = 0; r < rounds; r++) {
		for (g = 0; g < 5; g++) {
			unsigned tail = PLAN[g].name_len - DENTRY_INLINE_NAME_MAX;
			unsigned units, gran;
			long cap, ok = 0, enospc = 0, other = 0, ncreated = 0, del_ok = 0, del_fail = 0, k;
			double t0;

			heap_pick(tail, &units, &gran);
			cap = group_cap(g, units);

			slot_map = malloc(sizeof(*slot_map) * (cap + 1));
			created = malloc(sizeof(*created) * (cap + 1));
			if (!slot_map || !created) {
				fprintf(stderr, "OOM\n");
				return 1;
			}
			for (k = 0; k <= cap; k++)
				slot_map[k] = (unsigned long long)k;

			shuffle(slot_map, cap + 1);	/* 插入乱序 */
			t0 = now_s();
			for (k = 0; k <= cap; k++) {
				mkname(name, g, slot_map[k], PLAN[g].name_len);
				sprintf(path, "%s/%s", base, name);
				if (mkdir(path, 0755) == 0) {
					ok++;
					created[ncreated++] = slot_map[k];
				} else if (errno == ENOSPC) {
					enospc++;
				} else {
					if (other < 3)
						printf("  MKDIR unexpect e=%d(%s)\n", errno, strerror(errno));
					other++;
				}
			}
			printf("\nr%d G%d %-4s INSERT: ok=%ld ENOSPC=%ld other=%ld (%.1fs)\n",
			       r + 1, g, GROUP_TAG[g], ok, enospc, other, now_s() - t0);
			if (ok != cap || enospc != 1 || other != 0) {
				fprintf(stderr, "✗ G%d 边界不符：期望 ok=%ld ENOSPC=1 other=0\n", g, cap);
				return 1;
			}
			mem_report("peak");

			shuffle(created, ncreated);	/* 删除乱序（独立于插入序） */
			t0 = now_s();
			for (k = 0; k < ncreated; k++) {
				mkname(name, g, created[k], PLAN[g].name_len);
				sprintf(path, "%s/%s", base, name);
				if (rmdir(path) == 0)
					del_ok++;
				else {
					if (del_fail < 3)
						printf("  RMDIR fail e=%d(%s)\n", errno, strerror(errno));
					del_fail++;
				}
			}
			printf("r%d G%d %-4s DELETE: ok=%ld fail=%ld (%.1fs)\n",
			       r + 1, g, GROUP_TAG[g], del_ok, del_fail, now_s() - t0);
			free(slot_map);
			free(created);
			if (del_fail != 0)
				return 1;

			do_syncfs(base);
			if (count_entries(base) != 0) {
				fprintf(stderr, "✗ 删后目录非空（readdir=%ld）\n", count_entries(base));
				return 1;
			}
			if (fs_counters_ok) {
				const int keep[] = { 0, 2 }; /* 至少 2 个时间点：即时 + 2s（等 TTL/滞后释放） */
				struct fssnap s;
				int ci, back = 0;

				for (ci = 0; ci < 2 && !back; ci++) {
					if (ci)
						sleep(keep[1]);
					if (fs_snap(base, &s) != 0)
						break;
					back = (s.bfree == base_fs.bfree && s.ffree == base_fs.ffree);
					printf("r%d G%d %-4s AFTER%s: bfree=%llu (Δ%+lld) ffree=%llu (Δ%+lld)\n",
					       r + 1, g, GROUP_TAG[g], ci ? "+2s" : "   ", s.bfree,
					       (long long)(s.bfree - base_fs.bfree), s.ffree,
					       (long long)(s.ffree - base_fs.ffree));
				}
				if (!back) {
					fprintf(stderr, "✗ G%d 未回基线（含 2s 滞后观察）→ 该组疑似泄漏\n", g);
					return 1;
				}
			}
		}
	}

	/* ── 5. 删净后内存多点采样（滞后 vs 泄漏）────────────────────────── */
	printf("\npost-delete memory watch（距删净；回升=释放滞后，持续低于基线=泄漏）:\n");
	{
		double mark = now_s();

		for (i = 0; i < N_POST_SAMPLE; i++) {
			char tag[32];
			double remain = POST_SAMPLE_SEC[i] - (now_s() - mark);

			if (remain > 0)
				usleep((useconds_t)(remain * 1e6));
			snprintf(tag, sizeof(tag), "t+%ds", POST_SAMPLE_SEC[i]);
			mem_report(tag);
		}
	}

	if (fs_counters_ok && fs_snap(base, &end_fs) == 0) {
		printf("\nfinal:    bfree=%llu ffree=%llu\n", end_fs.bfree, end_fs.ffree);
		if (end_fs.bfree != base_fs.bfree || end_fs.ffree != base_fs.ffree) {
			fprintf(stderr, "✗ 终局未回基线 → 资源泄漏\n");
			return 1;
		}
	}
	printf("\nPASS: %d 轮 × 5 组 × (%ld 条长名 mkdir + 全 rmdir)，%s，目录回空\n",
	       rounds, total, fs_counters_ok ? "块/inode 精确回基线" : "内存采样见上");
	return 0;
}
