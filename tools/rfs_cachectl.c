// SPDX-License-Identifier: GPL-2.0
/*
 * rfs_cachectl.c —— RavenFS mdata 缓存强淘汰用户态小工具
 *
 * 用法：
 *   rfs_cachectl [-q] [-v] <mountpoint> pressure
 *   rfs_cachectl [-q] [-v] <mountpoint> lowload [age]
 *
 * 在挂载点内任意文件/目录上 open(O_RDONLY) + ioctl(RAVENFS_IOC_CACHE_SQUEEZE)。
 * 需要 root（CAP_SYS_ADMIN）；只读挂载返回 -EROFS。
 *
 * age 语法（仅 LOWLOAD 使用；PRESSURE 忽略 TTL）：
 *   <num>s / <num>ms / <num>us / <num>ns / <num>m / <num>h   相对年龄，
 *       工具内部换算成内核要求的 CLOCK_MONOTONIC **绝对截止时间戳**：
 *           cutoff = now_monotonic_ns - age_ns
 *   0 | all | none      不做 TTL 过滤（ttl_ns = 0，等价回收全部可回收行）
 *   <num>               裸绝对 cutoff（ns），兼容旧接口 / 脚本直接传时间戳
 *   省略 age            默认 2s（低载档的保守默认）
 *
 * 成效验证（ioctl 前后各抓一次 /proc/fs/ravenfs/<s_id>/stats）：
 *   md_resident_del_bytes  增量 = 本次窗口内归还的 8K 节点缓冲字节
 *   md_table_del_bytes     增量 = 本次窗口内释放的翻译表结构字节
 *   md_reap_strong         增量 = 强淘汰成功回收的行数（内核计数）
 * 工具同时打印 resident/table 的**在驻量**（add - del）前后对比。
 * 注意：这两个 del 计数是累计量，窗口内若有并发 TTL reap/drop，增量会略大于
 * 本次 ioctl 的贡献；以 ioctl 返回的 rows_reaped / table_bytes_freed 为准，
 * stats 增量用于独立交叉验证（两者相等说明窗口内无并发淘汰）。
 *
 * 编译（仓库根目录）：
 *   gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I include/linux \
 *       tools/rfs_cachectl.c -o rfs_cachectl
 * 安装：
 *   make -C tools install            # 默认 PREFIX=/usr → /usr/bin
 *
 * 示例：
 *   rfs_cachectl /mnt/ravenfs pressure          # 压力档（无视 TTL）
 *   rfs_cachectl /mnt/ravenfs lowload 2s        # 只回收空闲超过 2s 的冷行
 *   rfs_cachectl /mnt/ravenfs lowload 0         # 不按 TTL 过滤（暖行也回收）
 *   rfs_cachectl /mnt/ravenfs lowload 1770000000000000000  # 裸 cutoff
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#include "ravenfs_mdata_ioctl.h"

/* 与 include/linux/ravenfs.h 的 RAVENFS_SUPER_MAGIC 一致（"RAVN"）。
 * 仅用于 statfs 的「是不是 ravenfs 挂载」提示，不参与 ioctl 参数。 */
#define RAVENFS_SUPER_MAGIC_VALUE	0x5241564EUL
/* 与内核 RAVENFS_MDATA_NODE_BYTES 一致：每行一份 8K 节点缓冲。 */
#define RAVENFS_MDATA_NODE_BYTES	8192ULL
/* LOWLOAD 省略 age 时的默认相对年龄（保守：2s 冷）。 */
#define DEFAULT_LOWLOAD_AGE_NS		(2ULL * 1000000000ULL)

/* /proc/fs/ravenfs/<s_id>/stats 中本工具关心的字段。 */
struct md_stats {
	unsigned long long reap_strong;
	unsigned long long resident_add_bytes;
	unsigned long long resident_del_bytes;
	unsigned long long table_add_bytes;
	unsigned long long table_del_bytes;
	unsigned long long table_free;
	int valid;
};

static void usage(const char *prog)
{
	fprintf(stderr,
"usage: %s [-q] [-v] <mountpoint> pressure\n"
"       %s [-q] [-v] <mountpoint> lowload [age]\n"
"\n"
"  pressure     回收所有可回收行（无视 TTL，模拟内存压力）\n"
"  lowload      只回收 acc_stamp < cutoff 的冷行；省略 age 默认 2s\n"
"\n"
"  age: <num>s|ms|us|ns|m|h   相对年龄（工具自动换算 CLOCK_MONOTONIC cutoff）\n"
"       0|all|none           不做 TTL 过滤\n"
"       <num>                裸绝对 cutoff（ns，兼容旧接口）\n"
"\n"
"  -q  quiet      只打印错误\n"
"  -v  verbose    额外打印 ioctl 前后的原始 stats 快照\n"
"\n"
"成效核对：ioctl 前后各读一次 /proc/fs/ravenfs/<s_id>/stats，\n"
"用 md_resident_del_bytes / md_table_del_bytes 增量报告两类内存节约。\n",
		prog, prog);
}

/* CLOCK_MONOTONIC 纳秒。 */
static unsigned long long mono_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (unsigned long long)ts.tv_sec * 1000000000ULL +
	       (unsigned long long)ts.tv_nsec;
}

static void fmt_bytes(unsigned long long bytes, char *buf, size_t len)
{
	if (bytes >= 1024ULL * 1024ULL)
		snprintf(buf, len, "%llu B (%.2f MiB)",
			 bytes, (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024ULL)
		snprintf(buf, len, "%llu B (%.2f KiB)",
			 bytes, (double)bytes / 1024.0);
	else
		snprintf(buf, len, "%llu B", bytes);
}

/*
 * 解析 age。返回 0 成功；-1 语法错误。
 * is_relative=true 表示 ns 是相对年龄，调用方需换算 cutoff。
 */
static int parse_age(const char *s, unsigned long long *ns, bool *is_relative)
{
	unsigned long long mult = 0;
	char *end = NULL;

	if (!strcmp(s, "all") || !strcmp(s, "none")) {
		*ns = 0;
		*is_relative = true;
		return 0;
	}

	errno = 0;
	*ns = strtoull(s, &end, 0);
	if (errno || end == s)
		return -1;

	if (*end == '\0') {
		/* 纯数字：裸绝对 cutoff；0 是特例（0 = 不做 TTL 过滤），
		 * 按相对 0 处理以便输出显示为 none。 */
		*is_relative = (*ns == 0);
		return 0;
	}

	if (!strcmp(end, "ns"))
		mult = 1ULL;
	else if (!strcmp(end, "us"))
		mult = 1000ULL;
	else if (!strcmp(end, "ms"))
		mult = 1000000ULL;
	else if (!strcmp(end, "s"))
		mult = 1000000000ULL;
	else if (!strcmp(end, "m"))
		mult = 60ULL * 1000000000ULL;
	else if (!strcmp(end, "h"))
		mult = 3600ULL * 1000000000ULL;
	else
		return -1;

	if (mult != 1ULL && *ns > ULLONG_MAX / mult)
		return -1;
	*ns *= mult;
	*is_relative = true;
	return 0;
}

static void fmt_age(unsigned long long ns, char *buf, size_t len)
{
	if (ns == 0) {
		snprintf(buf, len, "none (no TTL filter)");
		return;
	}
	if (ns % (3600ULL * 1000000000ULL) == 0)
		snprintf(buf, len, "%lluh", ns / (3600ULL * 1000000000ULL));
	else if (ns % (60ULL * 1000000000ULL) == 0)
		snprintf(buf, len, "%llum", ns / (60ULL * 1000000000ULL));
	else if (ns % 1000000000ULL == 0)
		snprintf(buf, len, "%llus", ns / 1000000000ULL);
	else if (ns % 1000000ULL == 0)
		snprintf(buf, len, "%llums", ns / 1000000ULL);
	else if (ns % 1000ULL == 0)
		snprintf(buf, len, "%lluus", ns / 1000ULL);
	else
		snprintf(buf, len, "%lluns", ns);
}

/* mountinfo 的 path 字段按内核规则转义：\040 \011 \012 \134。 */
static void unescape_mountinfo(char *s)
{
	char *r = s, *w = s;

	while (*r) {
		if (r[0] == '\\' && isdigit((unsigned char)r[1]) &&
		    isdigit((unsigned char)r[2]) && isdigit((unsigned char)r[3])) {
			int v = (r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0');

			*w++ = (char)v;
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

/* path 是否等于 mountpoint，或位于其下（"挂在里面"也接受）。 */
static bool path_under(const char *path, const char *mp)
{
	size_t n;

	if (!strcmp(mp, "/"))
		return true;
	n = strlen(mp);
	if (strncmp(path, mp, n) != 0)
		return false;
	return path[n] == '\0' || path[n] == '/';
}

static bool stats_file_exists(const char *s_id)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/proc/fs/ravenfs/%s/stats", s_id);
	return access(path, R_OK) == 0;
}

/*
 * 由挂载点定位 /proc/fs/ravenfs/<s_id>/stats。
 * s_id 在 get_tree_bdev 路径下 = 底层块设备名（如 nvme1n1 / sda1）。
 * 步骤：/proc/self/mountinfo 找最长前缀匹配的 ravenfs 挂载 →
 *       源设备 basename / sysfs 设备名 → 最后单条目兜底。
 */
static int resolve_stats_path(const char *mountpoint, char *out, size_t len)
{
	char real[PATH_MAX];
	char line[PATH_MAX * 2];
	char best_mp[PATH_MAX] = "";
	char best_dev[64] = "";
	char best_src[PATH_MAX] = "";
	FILE *fp;
	bool saw_ravenfs = false;

	if (!realpath(mountpoint, real))
		snprintf(real, sizeof(real), "%s", mountpoint);

	fp = fopen("/proc/self/mountinfo", "r");
	if (!fp) {
		fprintf(stderr, "rfs_cachectl: open /proc/self/mountinfo: %s\n",
			strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *tok[64];
		int ntok = 0, i, dash = -1;
		char *p = line;

		for (i = 0; i < 64; i++) {
			while (*p == ' ' || *p == '\n')
				p++;
			if (!*p)
				break;
			tok[ntok++] = p;
			while (*p && *p != ' ' && *p != '\n')
				p++;
			if (*p)
				*p++ = '\0';
		}
		if (ntok < 7)
			continue;
		for (i = 6; i < ntok; i++) {
			if (!strcmp(tok[i], "-")) {
				dash = i;
				break;
			}
		}
		if (dash < 0 || dash + 2 >= ntok)
			continue;
		if (strcmp(tok[dash + 1], "ravenfs") != 0)
			continue;
		saw_ravenfs = true;

		unescape_mountinfo(tok[4]);
		if (!path_under(real, tok[4]))
			continue;
		if (strlen(tok[4]) < strlen(best_mp))
			continue;

		snprintf(best_mp, sizeof(best_mp), "%s", tok[4]);
		snprintf(best_dev, sizeof(best_dev), "%s", tok[2]);
		snprintf(best_src, sizeof(best_src), "%s", tok[dash + 2]);
	}
	fclose(fp);

	if (!best_mp[0]) {
		if (saw_ravenfs) {
			fprintf(stderr,
				"rfs_cachectl: %s 不在任何 ravenfs 挂载点内\n",
				real);
			/* 路径明确不属于 ravenfs：不做单条目兜底，避免把
			 * stats 张冠李戴。 */
			return -1;
		}
		fprintf(stderr,
			"rfs_cachectl: 没有 ravenfs 挂载（检查 /proc/filesystems）\n");
		/* 单条目兜底：容器/命名空间里 mountinfo 可能不可见。 */
	}

	if (best_mp[0]) {
		const char *base = strrchr(best_src, '/');
		char sys[PATH_MAX], link[PATH_MAX];
		char sbid[256];
		ssize_t n;

		base = base ? base + 1 : best_src;
		if (base[0] && strlen(base) < sizeof(sbid)) {
			snprintf(sbid, sizeof(sbid), "%s", base);
			if (stats_file_exists(sbid)) {
				snprintf(out, len, "/proc/fs/ravenfs/%s/stats",
					 sbid);
				return 0;
			}
		}
		snprintf(sys, sizeof(sys), "/sys/dev/block/%s", best_dev);
		n = readlink(sys, link, sizeof(link) - 1);
		if (n > 0) {
			const char *sb;

			link[n] = '\0';
			sb = strrchr(link, '/');
			sb = sb ? sb + 1 : link;
			if (sb[0] && strlen(sb) < sizeof(sbid)) {
				snprintf(sbid, sizeof(sbid), "%s", sb);
				if (stats_file_exists(sbid)) {
					snprintf(out, len,
						 "/proc/fs/ravenfs/%s/stats",
						 sbid);
					return 0;
				}
			}
		}
	}

	/* 最后兜底：/proc/fs/ravenfs 下只有一个条目时直接用它。 */
	{
		DIR *d = opendir("/proc/fs/ravenfs");
		struct dirent *de;
		char only[256] = "";
		int count = 0;

		if (d) {
			while ((de = readdir(d))) {
				if (!strcmp(de->d_name, ".") ||
				    !strcmp(de->d_name, ".."))
					continue;
				if (count == 0)
					snprintf(only, sizeof(only), "%s",
						 de->d_name);
				count++;
			}
			closedir(d);
		}
		if (count == 1 && stats_file_exists(only)) {
			fprintf(stderr,
				"rfs_cachectl: warning: 无法由挂载点定位 s_id，唯一候选 %s\n",
				only);
			snprintf(out, len, "/proc/fs/ravenfs/%s/stats", only);
			return 0;
		}
	}

	return -1;
}

static int read_stats(const char *path, struct md_stats *st)
{
	char line[512];
	FILE *fp;

	memset(st, 0, sizeof(*st));
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char name[128];
		unsigned long long val;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%127s %llu", name, &val) != 2)
			continue;

		if (!strcmp(name, "md_reap_strong"))
			st->reap_strong = val;
		else if (!strcmp(name, "md_resident_add_bytes"))
			st->resident_add_bytes = val;
		else if (!strcmp(name, "md_resident_del_bytes"))
			st->resident_del_bytes = val;
		else if (!strcmp(name, "md_table_add_bytes"))
			st->table_add_bytes = val;
		else if (!strcmp(name, "md_table_del_bytes"))
			st->table_del_bytes = val;
		else if (!strcmp(name, "md_table_free"))
			st->table_free = val;
	}
	fclose(fp);
	st->valid = 1;
	return 0;
}

static void dump_stats(const char *tag, const struct md_stats *st)
{
	fprintf(stderr,
		"  [%s] reap_strong=%llu resident_add=%llu resident_del=%llu "
		"table_add=%llu table_del=%llu table_free=%llu\n",
		tag, st->reap_strong, st->resident_add_bytes,
		st->resident_del_bytes, st->table_add_bytes,
		st->table_del_bytes, st->table_free);
}

static unsigned long long live_bytes(unsigned long long add,
				     unsigned long long del)
{
	return add > del ? add - del : 0;
}

int main(int argc, char **argv)
{
	struct ravenfs_cache_squeeze_arg arg = { 0 };
	struct md_stats before, after;
	const char *path = NULL, *gear_str = NULL, *age_str = NULL;
	char stats_path[PATH_MAX] = "";
	bool quiet = false, verbose = false, is_relative = false;
	bool have_stats = false, have_age = false;
	unsigned long long age_ns = DEFAULT_LOWLOAD_AGE_NS;
	unsigned long long now, cutoff;
	int fd, ret, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
			quiet = true;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else if (!path) {
			path = argv[i];
		} else if (!gear_str) {
			gear_str = argv[i];
		} else if (!age_str) {
			age_str = argv[i];
		} else {
			fprintf(stderr, "rfs_cachectl: 多余参数 '%s'\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (!path || !gear_str) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(gear_str, "pressure") || !strcmp(gear_str, "0") ||
	    !strcmp(gear_str, "p")) {
		arg.gear = RAVENFS_CACHE_SQUEEZE_PRESSURE;
		gear_str = "pressure";
	} else if (!strcmp(gear_str, "lowload") || !strcmp(gear_str, "1") ||
		   !strcmp(gear_str, "l")) {
		arg.gear = RAVENFS_CACHE_SQUEEZE_LOWLOAD;
		gear_str = "lowload";
	} else {
		fprintf(stderr, "rfs_cachectl: 非法档位 '%s'（pressure|lowload）\n",
			gear_str);
		return 2;
	}

	if (age_str) {
		if (parse_age(age_str, &age_ns, &is_relative) != 0) {
			fprintf(stderr, "rfs_cachectl: 非法 age '%s'\n", age_str);
			return 2;
		}
		have_age = true;
	}

	now = mono_ns();
	if (arg.gear == RAVENFS_CACHE_SQUEEZE_LOWLOAD) {
		if (!have_age) {
			/* 省略 age：默认 2s 冷行。 */
			is_relative = true;
			age_ns = DEFAULT_LOWLOAD_AGE_NS;
		}
		if (is_relative && age_ns != 0) {
			cutoff = now > age_ns ? now - age_ns : 0;
			/* now < age_ns 只可能发生在开机后极早期，cutoff 取 0
			 * 会变成「不过滤」；这里保守取 1，避免误回收热行。 */
			if (cutoff == 0)
				cutoff = 1;
			arg.ttl_ns = cutoff;
		} else if (is_relative) {
			arg.ttl_ns = 0;	/* all/none */
		} else {
			arg.ttl_ns = age_ns;	/* 裸 cutoff 原样透传 */
		}
	} else {
		arg.ttl_ns = 0;		/* PRESSURE 忽略 TTL */
	}
	arg.flags = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "rfs_cachectl: open(%s): %s\n", path,
			strerror(errno));
		return 1;
	}

	/* statfs 提示：非 ravenfs 时直接点明，避免把 -ENOTTY 误读成参数问题。 */
	{
		struct statfs sfs;

		if (fstatfs(fd, &sfs) == 0 &&
		    (unsigned long)sfs.f_type != RAVENFS_SUPER_MAGIC_VALUE)
			fprintf(stderr,
				"rfs_cachectl: warning: %s 的 fs type=0x%lx 不是 ravenfs（0x%lx）\n",
				path, (unsigned long)sfs.f_type,
				RAVENFS_SUPER_MAGIC_VALUE);
	}

	if (resolve_stats_path(path, stats_path, sizeof(stats_path)) == 0) {
		if (read_stats(stats_path, &before) == 0)
			have_stats = true;
		else
			fprintf(stderr, "rfs_cachectl: warning: 读取 %s 失败: %s\n",
				stats_path, strerror(errno));
	} else {
		fprintf(stderr,
			"rfs_cachectl: warning: 未找到 /proc/fs/ravenfs/<s_id>/stats，"
			"只报告 ioctl 返回值\n");
	}

	if (have_stats && verbose) {
		fprintf(stderr, "rfs_cachectl: stats=%s\n", stats_path);
		dump_stats("before", &before);
	}

	ret = ioctl(fd, RAVENFS_IOC_CACHE_SQUEEZE, &arg);
	if (ret < 0)
		fprintf(stderr, "rfs_cachectl: ioctl(%s): %s\n", path,
			strerror(errno));
	/* 失败也可能带回 partial 结果（例如 -EIO 的脏行写回失败）。 */

	if (have_stats && read_stats(stats_path, &after) == 0 && verbose)
		dump_stats("after ", &after);

	if (!quiet) {
		if (arg.gear == RAVENFS_CACHE_SQUEEZE_LOWLOAD) {
			char relbuf[64];

			fmt_age(is_relative ? age_ns : 0, relbuf,
				sizeof(relbuf));
			if (is_relative)
				printf("rfs_cachectl: path=%s gear=%s age=%s "
				       "ttl_cutoff_ns=%llu (now=%llu)\n",
				       path, gear_str, relbuf,
				       (unsigned long long)arg.ttl_ns, now);
			else
				printf("rfs_cachectl: path=%s gear=%s "
				       "ttl_cutoff_ns=%llu (raw cutoff)\n",
				       path, gear_str,
				       (unsigned long long)arg.ttl_ns);
		} else {
			printf("rfs_cachectl: path=%s gear=%s (ttl ignored)\n",
			       path, gear_str);
		}

		printf("  ioctl: rows_reaped=%llu tables_freed=%llu "
		       "table_bytes_freed=%llu write_errors=%llu\n",
		       (unsigned long long)arg.rows_reaped,
		       (unsigned long long)arg.tables_freed,
		       (unsigned long long)arg.table_bytes_freed,
		       (unsigned long long)arg.write_errors);
	}

	if (have_stats) {
		unsigned long long d_res_del, d_tab_del, d_strong, d_tab_free;
		unsigned long long rows_by_bytes, live_res_b, live_res_a;
		unsigned long long live_tab_b, live_tab_a;
		char res_delta[64], tab_delta[64];
		char res_before[64], res_after[64];
		char tab_before[64], tab_after[64];

		d_res_del = after.resident_del_bytes - before.resident_del_bytes;
		d_tab_del = after.table_del_bytes - before.table_del_bytes;
		d_strong = after.reap_strong - before.reap_strong;
		d_tab_free = after.table_free - before.table_free;

		rows_by_bytes = d_res_del / RAVENFS_MDATA_NODE_BYTES;
		live_res_b = live_bytes(before.resident_add_bytes,
					before.resident_del_bytes);
		live_res_a = live_bytes(after.resident_add_bytes,
					after.resident_del_bytes);
		live_tab_b = live_bytes(before.table_add_bytes,
					before.table_del_bytes);
		live_tab_a = live_bytes(after.table_add_bytes,
					after.table_del_bytes);

		fmt_bytes(d_res_del, res_delta, sizeof(res_delta));
		fmt_bytes(d_tab_del, tab_delta, sizeof(tab_delta));
		fmt_bytes(live_res_b, res_before, sizeof(res_before));
		fmt_bytes(live_res_a, res_after, sizeof(res_after));
		fmt_bytes(live_tab_b, tab_before, sizeof(tab_before));
		fmt_bytes(live_tab_a, tab_after, sizeof(tab_after));

		if (!quiet) {
			printf("  %s: 8K node buffers: %s (%llu rows x %llu B)\n",
			       ret < 0 ? "observed (ioctl failed)" : "saved",
			       res_delta, rows_by_bytes,
			       RAVENFS_MDATA_NODE_BYTES);
			printf("         table structs  : %s (%llu tables freed)\n",
			       tab_delta, d_tab_free);
			printf("  live : node buffers %s -> %s\n",
			       res_before, res_after);
			printf("         table structs  %s -> %s\n",
			       tab_before, tab_after);
			printf("  check: Δreap_strong=%llu Δresident/8K=%llu "
			       "Δtable_del=%llu B Δtable_free=%llu  %s\n",
			       d_strong, rows_by_bytes, d_tab_del, d_tab_free,
			       ret < 0 ?
			       "(ioctl 失败，增量仅反映窗口内并发活动)" :
			       (d_res_del == arg.rows_reaped *
					 RAVENFS_MDATA_NODE_BYTES &&
				d_tab_del == arg.table_bytes_freed) ?
			       "(与 ioctl 结果一致)" :
			       "(窗口内有并发淘汰/回填，差值正常)");
		}
	}

	close(fd);
	return ret < 0 ? 1 : 0;
}
