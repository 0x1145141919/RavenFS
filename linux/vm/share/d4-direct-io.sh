#!/bin/bash
# d4-direct-io.sh — D-4「O_DIRECT（iomap DIO）」VM 内验收（root 执行）
#
# 用法:
#   ./d4-direct-io.sh [挂载点]
#   默认挂载点: /mnt/rfs
#
# 口径（用户 2026-10-23 拍板）:
#   · DIO 写：pos / 长度 / 用户缓冲 全部 4K 对齐，否则 -EINVAL（先预检，不脏分配）；
#   · 读：iomap 默认（bdev logical block，通常 512）；
#   · 读写均 IOMAP_DIO_FORCE_WAIT（同步完成）；
#   · DIO 写仅 -ENOTBLK 回退 buffered（XFS/ext4 同构）；
#   · O_APPEND 且 EOF 非 4K 对齐 → -EINVAL（已知口径，本脚本只 INFO 记录）。
#
# 本脚本自包含: 定位 deadbeef 盘 → insmod → mkfs --v2.0 → mount → 测试 →
#              umount → audit → dmesg 扫描；失败也会尽可能 umount/清理。
#
# 前提: guest Arch Linux，root；/host/mkfs.ravenfs；可选 cc/gcc、fallocate。
#       不需要网络。cc 缺失时 SKIP 需要 C 的用例（见各段 SKIP 打印）。
#
# 覆盖矩阵（编号对应用户验收单）:
#   1  open(O_DIRECT) 成功                2  4K 对齐 DIO 写 + DIO/buffered 三方一致
#   3  对齐/非对齐矩阵                    4  inline 文件 DIO 读 + 迁移
#   5  稀疏文件洞读 0 + 中段 DIO 写       6  extent 树文件多段 DIO 覆盖写
#   7  direct 跨 EOF 增长（direct→extent）8  O_APPEND + O_DIRECT
#   9  sync/umount/mount 持久化           10 并发冒烟（DIO 写 + buffered 读）
#   11 回归审计（audit_v20/v13）+ dmesg   12 统计面（/proc/fs/ravenfs/*/stats）
# 注: 输出顺序为 ... 10 → 12（卸载前采样 stats）→ 11（卸载后审计）→ dmesg。

set -u
export LC_ALL=C

MNT="${1:-/mnt/rfs}"
TMP="/tmp/d4.$$"
ERRF="$TMP/dd.err"
DEV=""
T=""
MKFS=/host/mkfs.ravenfs
HELPER=""
HAVE_CC=0
MOUNTED=0

PASS_N=0
FAIL_N=0
SKIP_N=0
FAIL=0

mkdir -p "$TMP" || exit 2

cleanup() {
	local rc=$?

	cd / 2>/dev/null
	if [ -n "${T:-}" ] && [ -d "$T" ]; then
		rm -rf "$T" 2>/dev/null
	fi
	# 只卸载本脚本挂上的实例，绝不动脚本启动前已存在的挂载
	if [ "${MOUNTED:-0}" -eq 1 ] && mountpoint -q "$MNT" 2>/dev/null; then
		sync 2>/dev/null
		umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null
	fi
	rm -rf "$TMP" 2>/dev/null
	exit "$rc"
}
trap cleanup EXIT

check() { # $1=case $2=expected $3=actual
	if [ "$2" = "$3" ]; then
		echo "PASS  $1  ($3)"
		PASS_N=$((PASS_N + 1))
	else
		echo "FAIL  $1  expected=$2 actual=$3"
		FAIL_N=$((FAIL_N + 1))
		FAIL=1
	fi
}

skip() { # $1=case [$2=reason]
	SKIP_N=$((SKIP_N + 1))
	if [ -n "${2:-}" ]; then
		echo "SKIP  $1  ($2)"
	else
		echo "SKIP  $1"
	fi
}

info() { echo "INFO  $*"; }

size_of() { stat -c %s "$1" 2>/dev/null || echo MISSING; }
sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
cmpm() { cmp -s "$1" "$2" && echo MATCH || echo MISMATCH; }

errno_token() { # $1=rc $2=stderr file → OK/EINVAL/EOPNOTSUPP/ENOSPC/rc=N:msg
	local rc="$1" f="$2" last

	if [ "$rc" -eq 0 ]; then
		echo OK
		return
	fi
	last=$(tail -1 "$f" 2>/dev/null)
	if grep -qi 'invalid argument' "$f" 2>/dev/null; then
		echo EINVAL
	elif grep -qi 'operation not supported' "$f" 2>/dev/null; then
		echo EOPNOTSUPP
	elif grep -qi 'no space left' "$f" 2>/dev/null; then
		echo ENOSPC
	else
		echo "rc=$rc:${last:-?}"
	fi
}

DD_RC=0
DD_TOK=OK
dd_run() { # dd "$@"；结果写 DD_RC/DD_TOK
	DD_RC=0
	dd "$@" 2>"$ERRF" || DD_RC=$?
	DD_TOK=$(errno_token "$DD_RC" "$ERRF")
	if [ "$DD_RC" -ne 0 ]; then
		echo "      dd: $(tail -1 "$ERRF" 2>/dev/null)"
	fi
}

c_tok() { # $1=helper stdout $2=helper rc
	local out="$1" rc="$2"

	if [ "$rc" -eq 0 ]; then
		echo OK
		return
	fi
	case "$out" in
	*"errno=22"*) echo EINVAL ;;
	*"errno=95"*) echo EOPNOTSUPP ;;
	*"errno=28"*) echo ENOSPC ;;
	*"errno=13"*) echo EACCES ;;
	*) echo "rc=$rc:${out:-?}" ;;
	esac
}

need() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "缺少工具: $1" >&2
		exit 2
	}
}

# ─────────────────────────────────────────────────────────────
# 环境检查
# ─────────────────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ] || {
	echo "must run as root" >&2
	exit 2
}

for t in dd cmp sha256sum stat truncate sync mountpoint insmod umount dmesg; do
	need "$t"
done

# 按 QEMU serial=deadbeef 定位 ravenfs 盘
for d in /dev/nvme*n1; do
	[ -e "$d" ] || continue
	s=$(tr -d ' \n' <"/sys/block/$(basename "$d")/device/serial" 2>/dev/null)
	[ "$s" = deadbeef ] && DEV="$d"
done
if [ -z "$DEV" ]; then
	echo "NO_DISK (serial=deadbeef 未找到)" >&2
	exit 2
fi
[ -x "$MKFS" ] || {
	echo "no $MKFS" >&2
	exit 2
}

# C helper（open 探针 / pwrite pos 非对齐 / punch 兜底 / append 精确 errno）
for c in cc gcc; do
	command -v "$c" >/dev/null 2>&1 || continue
	cat >"$TMP/dio_helper.c" <<'CEOF'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/falloc.h>

static void *abuf(size_t len)
{
	void *p = NULL;

	if (posix_memalign(&p, 4096, len) != 0) {
		fprintf(stderr, "posix_memalign(%zu) failed\n", len);
		exit(9);
	}
	memset(p, 0x5a, len);
	return p;
}

int main(int argc, char **argv)
{
	const char *cmd, *path;
	long long off;
	size_t len;
	void *b;
	ssize_t n;
	int fd, flags = O_RDWR | O_DIRECT;

	if (argc < 3) {
		fprintf(stderr, "usage: %s open|pwrite|append|punch|pread <path> [off] [len]\n",
			argv[0]);
		return 9;
	}
	cmd = argv[1];
	path = argv[2];
	off = argc > 3 ? atoll(argv[3]) : 0;
	len = argc > 4 ? (size_t)atoll(argv[4]) : 4096;

	if (!strcmp(cmd, "open")) {
		fd = open(path, flags);
		if (fd < 0) {
			printf("errno=%d %s\n", errno, strerror(errno));
			return 1;
		}
		printf("OK\n");
		close(fd);
		return 0;
	}

	if (!strcmp(cmd, "punch")) {
		fd = open(path, O_RDWR);
		if (fd < 0) {
			printf("errno=%d %s\n", errno, strerror(errno));
			return 1;
		}
		if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			      (off_t)off, (off_t)len) < 0) {
			printf("errno=%d %s\n", errno, strerror(errno));
			close(fd);
			return 1;
		}
		printf("OK\n");
		close(fd);
		return 0;
	}

	if (!strcmp(cmd, "append"))
		flags |= O_APPEND;
	else if (strcmp(cmd, "pwrite") && strcmp(cmd, "pread")) {
		fprintf(stderr, "bad cmd %s\n", cmd);
		return 9;
	}

	fd = open(path, flags);
	if (fd < 0) {
		printf("errno=%d %s\n", errno, strerror(errno));
		return 1;
	}
	b = abuf(len);
	if (!strcmp(cmd, "pwrite"))
		n = pwrite(fd, b, len, (off_t)off);
	else if (!strcmp(cmd, "append"))
		n = write(fd, b, len);
	else
		n = pread(fd, b, len, (off_t)off);
	free(b);
	if (n < 0) {
		printf("errno=%d %s\n", errno, strerror(errno));
		close(fd);
		return 1;
	}
	printf("ret=%zd\n", n);
	close(fd);
	return 0;
}
CEOF
	if "$c" -O0 -o "$TMP/dio_helper" "$TMP/dio_helper.c"; then
		HELPER="$TMP/dio_helper"
		HAVE_CC=1
		break
	fi
done
if [ "$HAVE_CC" -eq 0 ]; then
	info "C helper 不可用（无 cc/gcc 或编译失败）→ 依赖 C 的用例将 SKIP"
fi

# dmesg 基线（insmod 之前）
DMESG_BASE=$(dmesg 2>/dev/null | wc -l)

echo "=== D-4 O_DIRECT 验收: dev=$DEV mnt=$MNT ==="
info "kernel=$(uname -r)  cc=$([ "$HAVE_CC" -eq 1 ] && echo yes || echo no)  tmp=$TMP"

# ─────────────────────────────────────────────────────────────
# insmod / mkfs / mount
# ─────────────────────────────────────────────────────────────
if mountpoint -q "$MNT" 2>/dev/null; then
	info "已有挂载 $MNT，先卸载"
	sync
	umount "$MNT" || {
		echo "cannot umount existing $MNT" >&2
		exit 2
	}
fi

KO=/home/raven/rfs/linux/ravenfs.ko
[ -f "$KO" ] || KO=/rfssrc/linux/ravenfs.ko

if grep -qw ravenfs /proc/filesystems; then
	info "ravenfs 已注册，跳过 insmod（模块来源未知）"
else
	[ -f "$KO" ] || {
		echo "no ravenfs.ko ($KO)" >&2
		exit 2
	}
	insmod "$KO" || {
		echo "INSMOD_FAIL: $KO"
		dmesg | tail -20
		exit 2
	}
	info "insmod $KO"
fi
grep -qw ravenfs /proc/filesystems || {
	echo "ravenfs not registered" >&2
	exit 2
}

echo "=== mkfs --v2.0 -f $DEV ==="
"$MKFS" --v2.0 -f "$DEV" >"$TMP/mkfs.log" 2>&1 || {
	echo "MKFS_FAIL"
	tail -20 "$TMP/mkfs.log"
	exit 2
}

echo "=== mount ==="
mkdir -p "$MNT"
mount -t ravenfs "$DEV" "$MNT" 2>"$TMP/mount.err" || {
	echo "MOUNT_FAIL: $(tail -1 "$TMP/mount.err" 2>/dev/null)"
	dmesg | tail -30
	exit 2
}
MOUNTED=1

T="$MNT/.d4.$$"
mkdir -p "$T" || {
	echo "cannot create $T"
	exit 2
}
info "测试目录 $T"

# ─────────────────────────────────────────────────────────────
# [1] open(O_DIRECT) 成功
# ─────────────────────────────────────────────────────────────
echo
echo "=== [1] open(O_DIRECT) 成功 ==="
F1="$T/f_open"
dd_run if=/dev/zero of="$F1" bs=4k count=1 oflag=direct status=none
check "1-a dd open(O_DIRECT)+4K 写" "OK" "$DD_TOK"
check "1-b size=4096" "4096" "$(size_of "$F1")"

if [ "$HAVE_CC" -eq 1 ]; then
	out=$("$HELPER" open "$F1" 2>&1)
	rc=$?
	check "1-c open(O_RDWR|O_DIRECT) 探针" "OK" "$(c_tok "$out" "$rc")"
else
	skip "1-c open(O_RDWR|O_DIRECT) 探针" "无 cc；1-a 已代偿证明 open 可用"
fi

# ─────────────────────────────────────────────────────────────
# [2] 4K 对齐 DIO 写 + DIO 读回 + buffered 读回，三方一致
# ─────────────────────────────────────────────────────────────
echo
echo "=== [2] 对齐 DIO 写 64K，DIO/buffered 双读三方比对 ==="
SRC2="$TMP/src2"
F2="$T/f_dio_64k"
dd if=/dev/urandom of="$SRC2" bs=64k count=1 status=none

dd_run if="$SRC2" of="$F2" bs=64k count=1 oflag=direct status=none
check "2-a DIO 写 64K" "OK" "$DD_TOK"
check "2-b size=65536" "65536" "$(size_of "$F2")"

dd_run if="$F2" of="$TMP/out2.dio" iflag=direct bs=4k status=none
check "2-c DIO 读 64K" "OK" "$DD_TOK"
check "2-d DIO 读长度" "65536" "$(size_of "$TMP/out2.dio")"
check "2-e DIO 读 cmp" "MATCH" "$(cmpm "$SRC2" "$TMP/out2.dio")"

dd_run if="$F2" of="$TMP/out2.buf" bs=4k status=none
check "2-f buffered 读 64K" "OK" "$DD_TOK"
check "2-g buffered 读长度" "65536" "$(size_of "$TMP/out2.buf")"
check "2-h buffered 读 cmp" "MATCH" "$(cmpm "$SRC2" "$TMP/out2.buf")"

S2=$(sha "$SRC2")
info "sha256(src)=$S2"
check "2-i sha256 DIO==src" "$S2" "$(sha "$TMP/out2.dio")"
check "2-j sha256 buffered==src" "$S2" "$(sha "$TMP/out2.buf")"

# ─────────────────────────────────────────────────────────────
# [3] 对齐矩阵: 512B / pos=512 必须 EINVAL；4K/8K/64K 必须成功
# ─────────────────────────────────────────────────────────────
echo
echo "=== [3] 对齐矩阵 ==="
F3="$T/f_badalign"
: >"$F3"
dd_run if=/dev/zero of="$F3" bs=512 count=1 oflag=direct conv=notrunc status=none
check "3-a DIO bs=512（长度不对齐）→ EINVAL" "EINVAL" "$DD_TOK"
check "3-b 失败后 size 仍为 0（无脏分配）" "0" "$(size_of "$F3")"

F3P="$T/f_badpos"
: >"$F3P"
if [ "$HAVE_CC" -eq 1 ]; then
	out=$("$HELPER" pwrite "$F3P" 512 4096 2>&1)
	rc=$?
	tok=$(c_tok "$out" "$rc")
	[ "$tok" = EINVAL ] || echo "      helper: $out"
	check "3-c pwrite off=512 len=4096 → EINVAL" "EINVAL" "$tok"
	check "3-d 失败后 size 仍为 0（无脏分配）" "0" "$(size_of "$F3P")"
else
	skip "3-c pwrite off=512 len=4096 → EINVAL" "无 cc，无法构造 4K 对齐用户缓冲"
	skip "3-d 失败后 size 仍为 0" "依赖 3-c"
fi

F4K="$T/f_4k"
dd_run if=/dev/zero of="$F4K" bs=4k count=1 oflag=direct status=none
check "3-e DIO 4K 对齐写成功" "OK" "$DD_TOK"
check "3-f size=4096" "4096" "$(size_of "$F4K")"

F8K="$T/f_8k"
dd_run if=/dev/zero of="$F8K" bs=8k count=1 oflag=direct status=none
check "3-g DIO 8K 对齐写成功" "OK" "$DD_TOK"
check "3-h size=8192" "8192" "$(size_of "$F8K")"

F64K="$T/f_64k"
dd_run if=/dev/zero of="$F64K" bs=64k count=1 oflag=direct status=none
check "3-i DIO 64K 对齐写成功" "OK" "$DD_TOK"
check "3-j size=65536" "65536" "$(size_of "$F64K")"

# ─────────────────────────────────────────────────────────────
# [4] inline 文件: DIO 读到 100 字节；4K DIO 写触发迁移
# ─────────────────────────────────────────────────────────────
echo
echo "=== [4] inline（100B）DIO 读 + inline→direct/extent 迁移 ==="
ISRC="$TMP/inline100"
{
	head -c 50 /dev/zero | tr '\0' 'A'
	head -c 50 /dev/zero | tr '\0' 'B'
} >"$ISRC"

F4="$T/f_inline"
dd if="$ISRC" of="$F4" bs=100 count=1 status=none
check "4-a 建立 inline 文件（buffered 100B）" "100" "$(size_of "$F4")"

dd_run if="$F4" of="$TMP/inline.dio" iflag=direct bs=4k count=1 status=none
check "4-b DIO 读 inline rc" "OK" "$DD_TOK"
check "4-c DIO 读 inline 长度" "100" "$(size_of "$TMP/inline.dio")"
check "4-d DIO 读 inline 内容" "MATCH" "$(cmpm "$ISRC" "$TMP/inline.dio")"

sync
BLK4="$TMP/blk4"
dd if=/dev/urandom of="$BLK4" bs=4k count=1 status=none
dd_run if="$BLK4" of="$F4" bs=4k count=1 oflag=direct conv=notrunc status=none
check "4-e 4K DIO 写（inline→direct/extent 迁移）" "OK" "$DD_TOK"
check "4-f 迁移后 size=4096" "4096" "$(size_of "$F4")"

dd_run if="$F4" of="$TMP/inline2.dio" iflag=direct bs=4k status=none
check "4-g 迁移后 DIO 读 cmp" "MATCH" "$(cmpm "$BLK4" "$TMP/inline2.dio")"
dd_run if="$F4" of="$TMP/inline2.buf" bs=4k status=none
check "4-h 迁移后 buffered 读 cmp" "MATCH" "$(cmpm "$BLK4" "$TMP/inline2.buf")"
info "4 迁移后 st_blocks=$(stat -c %b "$F4" 2>/dev/null || echo '?')"

# ─────────────────────────────────────────────────────────────
# [5] 稀疏文件: 1MiB 洞读 0；中段 4K DIO 写
# ─────────────────────────────────────────────────────────────
echo
echo "=== [5] 1MiB 文件：洞读 0 + 中段 4K DIO 写 ==="
F5="$T/f_sparse"
SZ5=1048576
EXP5="$TMP/exp5"
BLK5="$TMP/blk5"

truncate -s "$SZ5" "$F5"
check "5-a truncate size=1MiB" "$SZ5" "$(size_of "$F5")"
info "5  初始 st_blocks=$(stat -c %b "$F5" 2>/dev/null || echo '?')"
truncate -s "$SZ5" "$EXP5"
dd if=/dev/urandom of="$BLK5" bs=4k count=1 status=none
sync	# truncate 的补零可能还在页缓存里，DIO 读前先落盘

dd_run if="$F5" of="$TMP/sparse0.dio" iflag=direct bs=4k status=none
check "5-b DIO 全量读 1MiB" "OK" "$DD_TOK"
check "5-c 读长度=1MiB" "$SZ5" "$(size_of "$TMP/sparse0.dio")"
check "5-d 洞内容全 0" "0" "$(tr -d '\000' <"$TMP/sparse0.dio" | wc -c | tr -d ' ')"

dd_run if="$BLK5" of="$F5" bs=4k seek=128 count=1 oflag=direct conv=notrunc status=none
check "5-e 中段（off=512KiB）4K DIO 写" "OK" "$DD_TOK"
dd if="$BLK5" of="$EXP5" bs=4k seek=128 count=1 conv=notrunc status=none

dd_run if="$F5" of="$TMP/sparse1.dio" iflag=direct bs=4k status=none
check "5-f 写后 DIO 全量读 cmp 期望（洞 0 + 写入区）" "MATCH" "$(cmpm "$EXP5" "$TMP/sparse1.dio")"
dd_run if="$F5" of="$TMP/sparse1.buf" bs=4k status=none
check "5-g 写后 buffered 全量读 cmp 期望" "MATCH" "$(cmpm "$EXP5" "$TMP/sparse1.buf")"
check "5-h size 仍为 1MiB" "$SZ5" "$(size_of "$F5")"

# ─────────────────────────────────────────────────────────────
# [6] extent 树文件: punch 物化多段 → buffered 随机写 → DIO 覆盖写
# ─────────────────────────────────────────────────────────────
echo
echo "=== [6] extent 树：punch 物化 + buffered 多段写 + DIO 随机覆盖 ==="
F6="$T/f_extent"
SH6="$TMP/shadow6"
SZ6=262144

truncate -s "$SZ6" "$F6"
check "6-a truncate size=256KiB" "$SZ6" "$(size_of "$F6")"
info "6  初始 st_blocks=$(stat -c %b "$F6" 2>/dev/null || echo '?')（direct 基线）"
truncate -s "$SZ6" "$SH6"
sync	# 先把 truncate 补零落盘，再 punch / 造 extent

PUNCHED=0
PUNCH_TOOL=""
if command -v fallocate >/dev/null 2>&1; then
	PUNCH_TOOL=fallocate
elif [ "$HAVE_CC" -eq 1 ]; then
	PUNCH_TOOL=helper
fi
if [ -z "$PUNCH_TOOL" ]; then
	skip "6-b punch 3×4K（direct→extent 物化）" "无 fallocate 且无 cc"
else
	for b in 4 9 20; do
		if [ "$PUNCH_TOOL" = fallocate ]; then
			fallocate -p -o $((b * 4096)) -l 4096 "$F6" 2>"$TMP/punch.err"
			rc=$?
		else
			out=$("$HELPER" punch "$F6" $((b * 4096)) 4096 2>&1)
			rc=$?
			[ "$rc" -ne 0 ] && echo "      helper punch blk=$b: $out"
		fi
		if [ "$rc" -eq 0 ]; then
			PUNCHED=$((PUNCHED + 1))
		elif [ "$PUNCH_TOOL" = fallocate ]; then
			echo "      fallocate -p blk=$b: $(tail -1 "$TMP/punch.err" 2>/dev/null)"
		fi
	done
	check "6-b punch 3×4K（direct→extent 物化）" "3" "$PUNCHED"
fi
if [ "$PUNCHED" -eq 0 ]; then
	info "6  punch 不可用 → extent 树前提未建立；后续仅验证 DIO/buffered 覆盖写一致性"
fi
check "6-c punch 后 size 不变" "$SZ6" "$(size_of "$F6")"
info "6  punch 后 st_blocks=$(stat -c %b "$F6" 2>/dev/null || echo '?')"

# punch 出来的洞必须读 0（blk=9 之后会被 DIO 覆盖写入）
if [ "$PUNCHED" -gt 0 ]; then
	dd_run if="$F6" of="$TMP/punched9" iflag=direct bs=4k skip=9 count=1 status=none
	check "6-c2 punch 后 blk=9 DIO 读 rc" "OK" "$DD_TOK"
	check "6-c3 punch 后 blk=9 DIO 读全 0" "0" \
		"$(tr -d '\000' <"$TMP/punched9" | wc -c | tr -d ' ')"
else
	skip "6-c2 punch 后 blk=9 DIO 读 rc" "punch 未成功，无洞可读"
	skip "6-c3 punch 后 blk=9 DIO 读全 0" "punch 未成功，无洞可读"
fi

# buffered 随机 4K 写：先填两个洞（4/20），再覆盖若干已映射块；9 留作洞
for b in 4 20 0 7 15 30; do
	gen="$TMP/b6_$b"
	dd if=/dev/urandom of="$gen" bs=4k count=1 status=none
	dd_run if="$gen" of="$F6" bs=4k seek="$b" count=1 conv=notrunc status=none
	check "6-d buffered 写 blk=$b" "OK" "$DD_TOK"
	dd if="$gen" of="$SH6" bs=4k seek="$b" count=1 conv=notrunc status=none
done

# DIO 随机覆盖写：blk=9 是洞（触发分配），2/31 是已映射块
sync
for b in 2 9 31; do
	gen="$TMP/b6_$b"
	dd if=/dev/urandom of="$gen" bs=4k count=1 status=none
	dd_run if="$gen" of="$F6" bs=4k seek="$b" count=1 oflag=direct conv=notrunc status=none
	if [ "$b" = 9 ]; then
		check "6-e DIO 写洞 blk=9（分配 + 写入）" "OK" "$DD_TOK"
	else
		check "6-e DIO 覆盖写 blk=$b" "OK" "$DD_TOK"
	fi
	dd if="$gen" of="$SH6" bs=4k seek="$b" count=1 conv=notrunc status=none
done

sync
dd_run if="$F6" of="$TMP/ext.dio" iflag=direct bs=4k status=none
check "6-f DIO 全量读 rc" "OK" "$DD_TOK"
check "6-g DIO 全量 vs 影子模型" "MATCH" "$(cmpm "$SH6" "$TMP/ext.dio")"
dd_run if="$F6" of="$TMP/ext.buf" bs=4k status=none
check "6-h buffered 全量读 rc" "OK" "$DD_TOK"
check "6-i buffered 全量 vs 影子模型" "MATCH" "$(cmpm "$SH6" "$TMP/ext.buf")"
check "6-j sha256 DIO==buffered" "$(sha "$TMP/ext.dio")" "$(sha "$TMP/ext.buf")"
check "6-k size=256KiB" "$SZ6" "$(size_of "$F6")"

# ─────────────────────────────────────────────────────────────
# [7] direct 跨 EOF 增长: 4K（direct）→ off=8192 4K DIO → direct→extent + 中间洞
# ─────────────────────────────────────────────────────────────
echo
echo "=== [7] direct 跨 EOF 增长（off=8192，中间 4K 洞） ==="
F7="$T/f_grow"
B7A="$TMP/blk7a"
B7B="$TMP/blk7b"
dd if=/dev/urandom of="$B7A" bs=4k count=1 status=none
dd if=/dev/urandom of="$B7B" bs=4k count=1 status=none

dd if="$B7A" of="$F7" bs=4k count=1 status=none
check "7-a 顺序 buffered 写 4K（落 direct）" "4096" "$(size_of "$F7")"
sync

dd_run if="$B7B" of="$F7" bs=4k seek=2 count=1 oflag=direct conv=notrunc status=none
check "7-b off=8192 4K DIO 写" "OK" "$DD_TOK"
check "7-c size=12288" "12288" "$(size_of "$F7")"
info "7  st_blocks=$(stat -c %b "$F7" 2>/dev/null || echo '?')（含 extent 节点）"

EXP7="$TMP/exp7"
truncate -s 12288 "$EXP7"
dd if="$B7A" of="$EXP7" bs=4k seek=0 count=1 conv=notrunc status=none
dd if="$B7B" of="$EXP7" bs=4k seek=2 count=1 conv=notrunc status=none

dd_run if="$F7" of="$TMP/grow.b1.dio" iflag=direct bs=4k skip=1 count=1 status=none
check "7-d 中间块 [4096,8192) DIO 读 rc" "OK" "$DD_TOK"
check "7-d2 中间块 [4096,8192) DIO 读全 0" "0" "$(tr -d '\000' <"$TMP/grow.b1.dio" | wc -c | tr -d ' ')"
dd_run if="$F7" of="$TMP/grow.b1.buf" bs=4k skip=1 count=1 status=none
check "7-e 中间块 buffered 读 rc" "OK" "$DD_TOK"
check "7-e2 中间块 buffered 读全 0" "0" "$(tr -d '\000' <"$TMP/grow.b1.buf" | wc -c | tr -d ' ')"

dd_run if="$F7" of="$TMP/grow.dio" iflag=direct bs=4k status=none
check "7-f DIO 全量读 vs 期望" "MATCH" "$(cmpm "$EXP7" "$TMP/grow.dio")"
dd_run if="$F7" of="$TMP/grow.buf" bs=4k status=none
check "7-g buffered 全量读 vs 期望" "MATCH" "$(cmpm "$EXP7" "$TMP/grow.buf")"

# ─────────────────────────────────────────────────────────────
# [8] O_APPEND + O_DIRECT
# ─────────────────────────────────────────────────────────────
echo
echo "=== [8] O_APPEND + O_DIRECT ==="
F8="$T/f_append"
A1="$TMP/a1"
A2="$TMP/a2"
dd if=/dev/urandom of="$A1" bs=4k count=1 status=none
dd if=/dev/urandom of="$A2" bs=4k count=1 status=none
dd if="$A1" of="$F8" bs=4k count=1 status=none
sync

dd_run if="$A2" of="$F8" bs=4k count=1 oflag=direct,append conv=notrunc status=none
check "8-a EOF 4K 对齐时 DIO append 成功" "OK" "$DD_TOK"
check "8-b append 后 size=8192" "8192" "$(size_of "$F8")"

cat "$A1" "$A2" >"$TMP/exp8"
dd_run if="$F8" of="$TMP/app.dio" iflag=direct bs=4k status=none
check "8-c append 结果 DIO 读 cmp" "MATCH" "$(cmpm "$TMP/exp8" "$TMP/app.dio")"
dd_run if="$F8" of="$TMP/app.buf" bs=4k status=none
check "8-d append 结果 buffered 读 cmp" "MATCH" "$(cmpm "$TMP/exp8" "$TMP/app.buf")"

# EOF 非 4K 对齐（100B inline）：已知口径 -EINVAL，只记录
F8B="$T/f_append_inline"
head -c 100 /dev/zero | tr '\0' 'C' >"$TMP/inlineC"
dd if="$TMP/inlineC" of="$F8B" bs=100 count=1 status=none
sync
dd_run if="$A2" of="$F8B" bs=4k count=1 oflag=direct,append conv=notrunc status=none
info "8-e O_APPEND+O_DIRECT 于 EOF=100（非 4K 对齐）: rc=$DD_RC token=$DD_TOK size=$(size_of "$F8B")"
if [ "$DD_TOK" = EINVAL ]; then
	info "      符合已知口径 -EINVAL（不计 PASS/FAIL）"
else
	info "      注意：与已知口径不同（仅记录，不计 FAIL）"
fi
if [ "$HAVE_CC" -eq 1 ]; then
	out=$("$HELPER" append "$F8B" 4096 2>&1)
	rc=$?
	info "8-f helper O_APPEND+O_DIRECT 追加 4K: $(c_tok "$out" "$rc") size=$(size_of "$F8B")"
fi

# ─────────────────────────────────────────────────────────────
# [9] 持久化: sync + umount + mount + 双读比对
# ─────────────────────────────────────────────────────────────
echo
echo "=== [9] 持久化（sync → umount → mount） ==="
sync
cd / 2>/dev/null

umount "$MNT" 2>"$TMP/um.err"
rc=$?
[ "$rc" -ne 0 ] && echo "      umount: $(tail -1 "$TMP/um.err" 2>/dev/null)"
check "9-a umount" "0" "$rc"
[ "$rc" -eq 0 ] && MOUNTED=0

mount -t ravenfs "$DEV" "$MNT" 2>"$TMP/mnt2.err"
rc=$?
[ "$rc" -ne 0 ] && echo "      mount: $(tail -1 "$TMP/mnt2.err" 2>/dev/null)"
check "9-b remount" "0" "$rc"
if [ "$rc" -ne 0 ]; then
	echo "remount 失败，无法继续持久化验证"
	dmesg | tail -30
	exit 1
fi
MOUNTED=1
check "9-c 测试目录仍在" "yes" "$([ -d "$T" ] && echo yes || echo no)"

dd_run if="$F2" of="$TMP/p2.dio" iflag=direct bs=4k status=none
check "9-d 重挂后 DIO 读 f_dio_64k cmp" "MATCH" "$(cmpm "$SRC2" "$TMP/p2.dio")"
dd_run if="$F2" of="$TMP/p2.buf" bs=4k status=none
check "9-e 重挂后 buffered 读 f_dio_64k cmp" "MATCH" "$(cmpm "$SRC2" "$TMP/p2.buf")"

dd_run if="$F6" of="$TMP/p6.dio" iflag=direct bs=4k status=none
check "9-f 重挂后 DIO 读 extent 文件 cmp" "MATCH" "$(cmpm "$SH6" "$TMP/p6.dio")"
dd_run if="$F6" of="$TMP/p6.buf" bs=4k status=none
check "9-g 重挂后 buffered 读 extent 文件 cmp" "MATCH" "$(cmpm "$SH6" "$TMP/p6.buf")"

# ─────────────────────────────────────────────────────────────
# [10] 并发冒烟: 一进程 DIO 写，另一进程 buffered 读
# ─────────────────────────────────────────────────────────────
echo
echo "=== [10] 并发冒烟（DIO 写 + buffered 读） ==="
F10="$T/f_conc"
SH10="$TMP/shadow10"
SZ10=131072
truncate -s "$SZ10" "$F10"
truncate -s "$SZ10" "$SH10"
sync	# 补零落盘，避免 DIO 写期间和脏页纠缠
for i in $(seq 0 15); do
	dd if=/dev/urandom of="$TMP/c10_$i" bs=4k count=1 status=none
done
: >"$TMP/conc.err"

(
	for r in 1 2; do
		for i in $(seq 0 15); do
			dd if="$TMP/c10_$i" of="$F10" bs=4k seek="$i" count=1 \
				conv=notrunc oflag=direct status=none 2>>"$TMP/conc.err" ||
				echo "WRITE_FAIL blk=$i r=$r" >>"$TMP/conc.err"
		done
	done
) &
WPID=$!
(
	for r in $(seq 1 40); do
		dd if="$F10" of=/dev/null bs=4k status=none 2>>"$TMP/conc.err" ||
			echo "READ_FAIL r=$r" >>"$TMP/conc.err"
	done
) &
RPID=$!
wait "$WPID"
WRC=$?
wait "$RPID"
RRC=$?

check "10-a DIO 写进程 rc" "0" "$WRC"
check "10-b buffered 读进程 rc" "0" "$RRC"
check "10-c 无 I/O 错误" "empty" "$([ -s "$TMP/conc.err" ] && echo nonempty || echo empty)"
if [ -s "$TMP/conc.err" ]; then
	sed -n '1,5p' "$TMP/conc.err" | sed 's/^/      /'
fi
info "10  writer=32×4K DIO，reader=40 次全文件 buffered 读"

for i in $(seq 0 15); do
	dd if="$TMP/c10_$i" of="$SH10" bs=4k seek="$i" count=1 conv=notrunc status=none
done
sync
dd_run if="$F10" of="$TMP/conc.dio" iflag=direct bs=4k status=none
check "10-d 最终内容（DIO 读）vs 影子" "MATCH" "$(cmpm "$SH10" "$TMP/conc.dio")"

# 并发读可能把旧页重新塞回 page cache；先丢干净页再做 buffered 比对
sync
echo 3 >/proc/sys/vm/drop_caches 2>/dev/null
dd_run if="$F10" of="$TMP/conc.buf" bs=4k status=none
check "10-e 最终内容（buffered 读，drop_caches 后）vs 影子" "MATCH" "$(cmpm "$SH10" "$TMP/conc.buf")"

# ─────────────────────────────────────────────────────────────
# [12] 统计面（卸载前采样；对应验收单第 12 项）
# ─────────────────────────────────────────────────────────────
echo
echo "=== [12] 统计面: /proc/fs/ravenfs/*/stats（卸载前） ==="
SID=$(ls /proc/fs/ravenfs 2>/dev/null | head -1)
if [ -n "$SID" ] && [ -r "/proc/fs/ravenfs/$SID/stats" ]; then
	STATS="/proc/fs/ravenfs/$SID/stats"
	info "SID=$SID"
	awk '$1 == "op_read_bytes" || $1 == "op_write_bytes" ||
	     $1 == "op_read_iter"  || $1 == "op_write_iter" ||
	     $1 == "op_fsync"      || $1 == "io_read_bytes" ||
	     $1 == "io_write_bytes" || $1 == "err_enospc" {
		printf "      %-16s %s\n", $1, $2
	}' "$STATS"
	info "stats 尾部:"
	tail -4 "$STATS" | sed 's/^/      /'
else
	info "无 /proc/fs/ravenfs/*/stats（D-11 未启用？）"
fi

# ─────────────────────────────────────────────────────────────
# [11] 回归审计 + dmesg（对应验收单第 11 项）
# ─────────────────────────────────────────────────────────────
echo
echo "=== [11] 回归审计（audit_v20/v13）+ dmesg ==="
rm -rf "$T"
T=""
sync
cd / 2>/dev/null

umount "$MNT" 2>"$TMP/um2.err"
rc=$?
[ "$rc" -ne 0 ] && echo "      umount: $(tail -1 "$TMP/um2.err" 2>/dev/null)"
check "11-a 最终 umount" "0" "$rc"
if [ "$rc" -ne 0 ]; then
	echo "umount 失败，审计不可信，跳过"
	dmesg | tail -30
	exit 1
fi
MOUNTED=0

AUD=""
if [ -x /host/audit_v20 ]; then
	AUD=/host/audit_v20
elif [ -x /host/audit_v13 ]; then
	AUD=/host/audit_v13
fi
if [ -n "$AUD" ]; then
	"$AUD" "$DEV" >"$TMP/audit.log" 2>&1
	ARC=$?
	info "audit $(basename "$AUD") rc=$ARC（磁盘格式 v2.0 期望 audit_v20）"
	tail -12 "$TMP/audit.log" | sed 's/^/      /'
else
	info "未找到 /host/audit_v20 或 /host/audit_v13，跳过审计"
fi

# dmesg 只扫本次运行新增的日志（基线在 insmod 之前采样）
NEW=$(dmesg 2>/dev/null | tail -n +"$((DMESG_BASE + 1))")
HITS=$(printf '%s\n' "$NEW" | grep -cE 'BUG:|Oops|panic|WARNING:|Call Trace')
check "dmesg 新增日志无 BUG/Oops/panic/WARNING/Call Trace" "0" "$HITS"
if [ "$HITS" -gt 0 ]; then
	printf '%s\n' "$NEW" |
		grep -E 'BUG:|Oops|panic|WARNING:|Call Trace' |
		tail -20 | sed 's/^/      /'
fi
RING_HITS=$(dmesg 2>/dev/null | grep -cE 'BUG:|Oops|panic|WARNING:|Call Trace')
info "dmesg 全环命中=$RING_HITS（含历史日志，仅供参考）"

# ─────────────────────────────────────────────────────────────
# 汇总
# ─────────────────────────────────────────────────────────────
TOTAL=$((PASS_N + FAIL_N))
echo
if [ "$FAIL" -eq 0 ]; then
	echo "RESULT: PASS $PASS_N/$TOTAL (SKIP=$SKIP_N)"
else
	echo "RESULT: FAIL $PASS_N/$TOTAL (SKIP=$SKIP_N)"
fi
exit "$FAIL"
