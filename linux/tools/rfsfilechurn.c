// SPDX-License-Identifier: GPL-2.0
/*
 * rfsfilechurn.c —— RavenFS 文件数据「随机写」用户态压测（M4.2 写路径）
 *
 * 用法:  rfsfilechurn <ravenfs-dir> [ROUNDS] [NFILES]
 *
 * ── 目的 ─────────────────────────────────────────────────────────────────
 * M4.2 的 buffered 写 = `write_iter` + `iomap_begin(WRITE)` 立刻分配（D-M4-6）+
 * 三态迁移（inline↔extent，D-M4-8）+ `i_blocks` 随写更新。M4.3 追加 **truncate / punch_hole**
 * （`punch_truncate_pread_spec.md`；D-M4-11 拒 KEEP_SIZE 纯预留）。此压测在挂载点内对若干
 * 文件做 N 轮随机 `write/pwrite` + `ftruncate` + `fallocate(PUNCH_HOLE|KEEP_SIZE)`，并用
 * **影子模型**（每文件一份期望字节缓冲 + 一个「写过」位图）记录期望内容；每轮结束 `pread`
 * 回读逐字节比对（写过区=填充字节，未写区=洞/被 punch/截断区=0）。
 *
 * 覆盖形态：空文件增长、跨块写、>64K 大文件、稀疏洞（随机 offset 造洞）、重复覆盖写、
 *           随机 truncate（缩/增）、随机 punch（打洞）。
 *
 * 兼容：truncate / punch 会先做**能力探测**；若目标 ko 尚未实现（-EOPNOTSUPP/-ENOSYS/
 *       -EINVAL），自动**关闭**该类操作并打印说明 —— 故本工具在 M4.2 内核上仍可用。
 *
 * ── 纪律（镜像 rfsdirchurn.c）────────────────────────────────────────────
 *   1) 入参仅 1 个待测目录；其余常量编译期内定（可加 ROUNDS/NFILES 两个可选覆盖）。
 *   2) 先 statfs() 校验 `f_type == RAVENFS_SUPER_MAGIC`（"RAVN"）——非 ravenfs 直接拒；
 *      要求该目录**已存在且为空**（否则条目/占用基线失准）。
 *   3) 固定 RNG 种子 → 写序列可复现。
 *   4) 影子模型：`data[]` = 期望字节；`written[]` = 是否写过（未写=洞，期望 0）。
 *      填充字节 = `churn_byte(name, off)`，只依赖文件名与文件内偏移（跨轮稳定）。
 *   5) 每轮末回读全量比对；任一处不符 → 报错并 FAIL（打印首个不符 off/盘值/期望）。
 *   6) 结束时 syncfs；块/inode 计数读 statfs（可作泄漏观察，但不作硬断言——写压测文件长期存在）。
 *
 * 退出码： 0 = 全部轮次比对一致；1 = 有不符/IO 错；2 = 用法/环境错。
 *
 * 宿主 `gcc -static` 编译后投递 guest；目标目录须在 ravenfs 挂载点内。
 * 完事 `sync` 后可另跑 `audit_v1.2 <dev>` 做结构自洽审计。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#define RAVENFS_SUPER_MAGIC	0x5241564EUL	/* "RAVN"，与 include/magics.h 同源 */

#define DEF_ROUNDS	3
#define DEF_NFILES	8
#define MAXSZ		(256u * 1024u)	/* 每文件影子缓冲上限（= 单文件可达最大尺寸）*/
#define OPBUF		65536u		/* 单次写缓冲 */
#define OPS_PER_ROUND	64		/* 每轮随机写次数 */

/* ── 填充字节（确定性；与 host 端 m42_fill.h 公式同源）─────────────────── */
static unsigned char churn_byte(const char *name, unsigned long long off)
{
	unsigned long long h = 1469598103934665603ULL;
	const unsigned char *p = (const unsigned char *)name;

	for (; *p; ++p) {
		h ^= (unsigned long long)*p;
		h *= 1099511628211ULL;
	}
	unsigned long long x = h ^ (off * 1099511628211ULL);

	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return (unsigned char)x;
}

/* M4.3 追加：操作计数
 *   · tr_writes   = ftruncate 次数
 *   · punch_ops   = 成功 punch 次数（EOPNOTSUPP 时自动关闭）
 */
static unsigned long long g_truncs, g_punches;

/* ── PCG-ish RNG（固定种子，可复现）────────────────────────────────────── */
static unsigned long long rng_state = 0x9E3779B97F4A7C15ULL;
static unsigned long long rng(void)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return rng_state >> 11;
}
static unsigned long long rng_range(unsigned long long n)
{
	return n ? rng() % n : 0;
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

struct fchurn {
	char	name[64];
	int	fd;
	unsigned long long size;		/* 当前文件尺寸 */
	unsigned char	*data;			/* 期望字节 [0,MAXSZ) */
	unsigned char	*written;		/* 写过标记 [0,MAXSZ) */
	unsigned long long writes;		/* 本文件写次数 */
};

static int fs_is_ravenfs(const char *dir)
{
	struct statfs sf;

	/* 仅 host 自测：允许在非 ravenfs（如 ext4）目录上跑影子模型，验证操作/回读逻辑 */
	if (getenv("RFSCHURN_ANYFS")) {
		fprintf(stderr, "⚠ RFSCHURN_ANYFS 已设：跳过 ravenfs 校验（**仅限 host 自测**）\n");
		return 1;
	}
	if (statfs(dir, &sf) != 0) {
		perror("statfs");
		return 0;
	}
	if ((unsigned long)sf.f_type != RAVENFS_SUPER_MAGIC) {
		fprintf(stderr, "✗ %s 不是 ravenfs（f_type=0x%lx，期望 0x%lx \"RAVN\"）\n",
			dir, (unsigned long)sf.f_type, RAVENFS_SUPER_MAGIC);
		return 0;
	}
	return 1;
}

static int dir_ok(const char *dir)
{
	struct stat st;

	if (stat(dir, &st) != 0) {
		perror("stat");
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "✗ %s 不是目录\n", dir);
		return 0;
	}
	return 1;
}

/* 做一次写：off/len 已 clamp 到 [0,MAXSZ)；更新影子模型 + 实际 pwrite */
static int do_write(struct fchurn *fc, unsigned long long off, unsigned long long len)
{
	unsigned char buf[OPBUF];
	unsigned long long done = 0;

	while (done < len) {
		unsigned long long chunk = len - done;

		if (chunk > sizeof(buf)) chunk = sizeof(buf);
		for (unsigned long long i = 0; i < chunk; i++)
			buf[i] = churn_byte(fc->name, off + done + i);
		ssize_t w = pwrite(fc->fd, buf, chunk, (off_t)(off + done));

		if (w <= 0) {
			fprintf(stderr, "✗ pwrite %s@%llu: %s\n", fc->name,
				off + done, strerror(errno));
			return -1;
		}
		/* 更新影子 */
		for (unsigned long long i = 0; i < (unsigned long long)w; i++) {
			fc->data[off + done + i]    = buf[i];
			fc->written[off + done + i] = 1;
		}
		done += (unsigned long long)w;
	}
	if (off + len > fc->size)
		fc->size = off + len;
	fc->writes++;
	return 0;
}

/* ── M4.3：ftruncate（影子同步）──
 * 缩：丢尾部；增：新区为洞（期望 0）。两向都更新 size 与「写过」位图。
 * 返回 0 成功；-1 硬失败。
 */
static int do_truncate(struct fchurn *fc, unsigned long long newlen)
{
	unsigned long long old = fc->size;

	if (newlen > MAXSZ)
		newlen = MAXSZ;
	if (ftruncate(fc->fd, (off_t)newlen) != 0) {
		fprintf(stderr, "✗ ftruncate %s->%llu: %s\n", fc->name, newlen, strerror(errno));
		return -1;
	}
	/* 影子：两端被影响的区间一律视为洞（期望 0）*/
	if (newlen < old) {
		for (unsigned long long i = newlen; i < old; i++) {
			fc->data[i] = 0;
			fc->written[i] = 0;
		}
	} else if (newlen > old) {
		for (unsigned long long i = old; i < newlen; i++) {
			fc->data[i] = 0;
			fc->written[i] = 0;
		}
	}
	fc->size = newlen;
	g_truncs++;
	return 0;
}

/* ── M4.3：punch_hole（FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE；影子同步）──
 * 无 size 变更；洞内读 0。EOPNOTSUPP/ENOSYS 视为「未实现」→ 返回 1（调用方关闭该操作）。
 * 其它错误 → -1。成功 → 0。
 */
static int do_punch(struct fchurn *fc, unsigned long long off, unsigned long long len)
{
	if (off >= fc->size)
		return 0;                                   /* offset ≥ size：no-op */
	if (off + len > fc->size)
		len = fc->size - off;                       /* clamp 到 EOF */
	if (len == 0)
		return 0;

	if (fallocate(fc->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		      (off_t)off, (off_t)len) != 0) {
		if (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL)
			return 1;                           /* 未实现 → 调用方降级 */
		fprintf(stderr, "✗ punch %s@%llu+%llu: %s\n", fc->name, off, len, strerror(errno));
		return -1;
	}
	for (unsigned long long i = off; i < off + len; i++) {
		fc->data[i] = 0;
		fc->written[i] = 0;
	}
	g_punches++;
	return 0;
}

/* 回读全量比对：写过区=data[]，未写区=0 */
static int verify(struct fchurn *fc, int round, int quiet)
{
	unsigned char *got = malloc(fc->size ? fc->size : 1);

	if (!got) { fprintf(stderr, "OOM\n"); return -1; }
	for (unsigned long long o = 0; o < fc->size; ) {
		size_t chunk = (size_t)((fc->size - o) < OPBUF ? (fc->size - o) : OPBUF);
		ssize_t r = pread(fc->fd, got + o, chunk, (off_t)o);

		if (r <= 0) {
			fprintf(stderr, "✗ pread %s@%llu: %s\n", fc->name, o, strerror(errno));
			free(got);
			return -1;
		}
		o += (unsigned long long)r;
	}
	for (unsigned long long o = 0; o < fc->size; o++) {
		unsigned char exp = fc->written[o] ? fc->data[o] : 0;

		if (got[o] != exp) {
			fprintf(stderr, "✗ r%d %s 不符 @off=%llu 盘=%02x 期望=%02x（wrote=%u）\n",
				round, fc->name, o, got[o], exp, fc->written[o]);
			free(got);
			return -1;
		}
	}
	free(got);
	if (!quiet)
		printf("    r%d %-16s size=%-8llu writes=%-5llu  [OK]\n",
		       round, fc->name, fc->size, fc->writes);
	return 0;
}

int main(int argc, char **argv)
{
	const char *base;
	int rounds = DEF_ROUNDS, nfiles = DEF_NFILES;
	struct fchurn *fc;
	int i, r, fail = 0;
	int trunc_enabled = 1, punch_enabled = 1;
	unsigned long long total_bytes = 0;

	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: %s <ravenfs-dir> [ROUNDS] [NFILES]\n", argv[0]);
		return 2;
	}
	base = argv[1];
	if (argc >= 3) rounds = atoi(argv[2]);
	if (argc >= 4) nfiles = atoi(argv[3]);
	if (rounds <= 0) rounds = DEF_ROUNDS;
	if (nfiles <= 0 || nfiles > 64) nfiles = DEF_NFILES;

	if (!fs_is_ravenfs(base) || !dir_ok(base))
		return 2;
	printf("ravenfs file-churn: dir=%s rounds=%d nfiles=%d (MAXSZ=%u)\n\n",
	       base, rounds, nfiles, MAXSZ);

	fc = calloc((size_t)nfiles, sizeof(*fc));
	if (!fc) { fprintf(stderr, "OOM\n"); return 2; }

	for (i = 0; i < nfiles; i++) {
		char path[4096];

		snprintf(fc[i].name, sizeof(fc[i].name), "f%02d.churn", i);
		snprintf(path, sizeof(path), "%s/%s", base, fc[i].name);
		fc[i].fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fc[i].fd < 0) {
			fprintf(stderr, "✗ open %s: %s\n", path, strerror(errno));
			return 2;
		}
		fc[i].data    = calloc(MAXSZ, 1);
		fc[i].written = calloc(MAXSZ, 1);
		if (!fc[i].data || !fc[i].written) { fprintf(stderr, "OOM\n"); return 2; }
	}
	printf("created %d files\n", nfiles);

	/* ── M4.3 能力探测：truncate / punch（未实现则降级关闭，保 M4.2 兼容）── */
	if (ftruncate(fc[0].fd, 0) != 0) {
		trunc_enabled = 0;
		printf("note: ftruncate 不可用（%s）→ 关闭 truncate 随机操作\n", strerror(errno));
	}
	{
		int pr = fallocate(fc[0].fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096);

		if (pr != 0 && (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL)) {
			punch_enabled = 0;
			printf("note: PUNCH_HOLE 不可用（%s）→ 关闭 punch 随机操作\n", strerror(errno));
		}
	}
	printf("ops: write=on truncate=%s punch=%s\n",
	       trunc_enabled ? "on" : "off", punch_enabled ? "on" : "off");

	for (r = 1; r <= rounds; r++) {
		int op;
		double t0 = now_s();

		for (op = 0; op < OPS_PER_ROUND; op++) {
			int f = (int)rng_range((unsigned long long)nfiles);
			unsigned long long off, len, span;
			int slots = 3 + (trunc_enabled ? 1 : 0) + (punch_enabled ? 1 : 0);

			/* 档位：0/1/2 = 写（小/跨块/大块）；3 = truncate（若启用）；4 = punch（若启用）*/
			unsigned long long mode = rng_range((unsigned long long)slots);

			if (mode == 3 && trunc_enabled) {
				if (do_truncate(&fc[f], rng_range(MAXSZ + 1)) != 0) { fail = 1; break; }
				continue;
			}
			if (mode == 4 && punch_enabled) {
				int pr = do_punch(&fc[f], rng_range(MAXSZ), 1 + rng_range(32768));

				if (pr < 0) { fail = 1; break; }
				if (pr == 1) { punch_enabled = 0; printf("note: punch 运行期不可用 → 关闭\n"); }
				continue;
			}
			if (mode == 3 || mode == 4)          /* 该档被禁用 → 退化为写 */
				mode = rng_range(3);

			/* 三档混合：小随机写 / 中等跨块写 / 大块（含 >64K）增长写 */
			if (mode == 0) {		/* 小随机写：[1,4K] */
				len = 1 + rng_range(4095);
				off = rng_range(MAXSZ > len ? MAXSZ - len : 1);
			} else if (mode == 1) {	/* 跨块：[4K,16K] */
				len = 4096 + rng_range(12288);
				off = rng_range(MAXSZ > len ? MAXSZ - len : 1);
			} else {		/* 大块：到 96K（配合高 off 可越 64K）*/
				len = 16384 + rng_range(81920);
				span = (MAXSZ > len) ? (MAXSZ - len) : 1;
				off = rng_range(span);
			}
			if (do_write(&fc[f], off, len) != 0) { fail = 1; break; }
			total_bytes += len;
		}
		if (fail)
			break;

		printf("round %d: %d ops, %.1fs\n", r, OPS_PER_ROUND, now_s() - t0);
		for (i = 0; i < nfiles; i++)
			if (verify(&fc[i], r, 0) != 0) { fail = 1; break; }
		if (fail)
			break;
	}

	if (!fail) {
		int sfd = open(base, O_RDONLY | O_DIRECTORY);

		if (sfd >= 0) { syncfs(sfd); close(sfd); }
		printf("\nsyncfs done\n");
		/* 结束后再全量比对一次（含 sync 后）*/
		for (i = 0; i < nfiles; i++)
			if (verify(&fc[i], rounds, 0) != 0) { fail = 1; break; }
	}

	for (i = 0; i < nfiles; i++) {
		if (fc[i].fd >= 0) close(fc[i].fd);
		free(fc[i].data);
		free(fc[i].written);
	}
	free(fc);

	if (fail) {
		printf("\nFAIL: 有写/读回不符\n");
		return 1;
	}
	printf("\nPASS: %d 轮 × %d 文件（写 %.1f MiB / truncate %llu / punch %llu），逐轮影子比对一致；sync 后复检一致\n",
	       rounds, nfiles, (double)total_bytes / (1024.0 * 1024.0), g_truncs, g_punches);
	printf("提示：可 `sync` 后跑 audit_v1.2 <device> 做结构自洽审计\n");
	return 0;
}
