// SPDX-License-Identifier: GPL-2.0
/*
 * rfs_cachectl.c —— RavenFS 缓存强淘汰用户态小工具（可选，不入 Makefile/CMake）
 *
 * 用法：
 *   rfs_cachectl <file-or-dir> lowload|pressure [ttl_cutoff_ns]
 *
 * 在挂载点内任意文件/目录上 open(O_RDONLY) + ioctl(RAVENFS_IOC_CACHE_SQUEEZE)。
 * 需要 root（CAP_SYS_ADMIN）；只读挂载返回 -EROFS。
 *
 * ttl_cutoff_ns 与 UAPI 一致：是 **CLOCK_MONOTONIC 绝对截止时间戳（ns）**，
 * 不是「空闲时长」；0 = 不做 TTL 过滤。需要按年龄筛选时由调用方先算
 * `now_monotonic_ns - age_ns`（例：python3 -c
 * 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC) - 2000000000)'）。
 *
 * 编译（仓库根目录）：
 *   gcc -std=gnu11 -Wall -Wextra -Werror -I include/linux \
 *       tools/rfs_cachectl.c -o rfs_cachectl
 *
 * 示例：
 *   rfs_cachectl /mnt/ravenfs lowload 0      # 不按 TTL 过滤
 *   rfs_cachectl /mnt/ravenfs pressure       # 压力档（无视 TTL）
 *   rfs_cachectl /mnt/ravenfs lowload "$cutoff"  # cutoff = now_mono - 2s
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ravenfs_mdata_ioctl.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s <file-or-dir> lowload|pressure [ttl_cutoff_ns]\n"
		"  lowload   reclaim rows idle longer than ttl cutoff\n"
		"            (absolute CLOCK_MONOTONIC ns; 0 = no TTL filter)\n"
		"  pressure  reclaim all reclaimable rows, ignore ttl\n",
		prog);
}

int main(int argc, char **argv)
{
	struct ravenfs_cache_squeeze_arg arg = { 0 };
	const char *path, *gear_str, *ttl_str;
	unsigned long long ttl = 0;
	char *end = NULL;
	int fd, ret;

	if (argc < 3 || argc > 4) {
		usage(argv[0]);
		return 2;
	}

	path = argv[1];
	gear_str = argv[2];
	ttl_str = (argc == 4) ? argv[3] : NULL;

	if (!strcmp(gear_str, "pressure")) {
		arg.gear = RAVENFS_CACHE_SQUEEZE_PRESSURE;
	} else if (!strcmp(gear_str, "lowload")) {
		arg.gear = RAVENFS_CACHE_SQUEEZE_LOWLOAD;
	} else {
		fprintf(stderr, "invalid gear '%s' (want lowload|pressure)\n",
			gear_str);
		usage(argv[0]);
		return 2;
	}

	if (ttl_str) {
		errno = 0;
		ttl = strtoull(ttl_str, &end, 0);
		if (errno || end == ttl_str || *end != '\0') {
			fprintf(stderr, "invalid ttl_ns '%s'\n", ttl_str);
			return 2;
		}
	}
	arg.ttl_ns = ttl;
	arg.flags = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return 1;
	}

	ret = ioctl(fd, RAVENFS_IOC_CACHE_SQUEEZE, &arg);
	if (ret < 0) {
		fprintf(stderr, "ioctl(%s): %s\n", path, strerror(errno));
		/* 失败也可能带回 partial 结果（例如 -EIO 的脏行写回失败）。*/
	}

	printf("gear=%s ttl_cutoff_ns=%llu rows_reaped=%llu tables_freed=%llu "
	       "table_bytes_freed=%llu write_errors=%llu\n",
	       gear_str, (unsigned long long)arg.ttl_ns,
	       (unsigned long long)arg.rows_reaped,
	       (unsigned long long)arg.tables_freed,
	       (unsigned long long)arg.table_bytes_freed,
	       (unsigned long long)arg.write_errors);

	close(fd);
	return ret < 0 ? 1 : 0;
}
