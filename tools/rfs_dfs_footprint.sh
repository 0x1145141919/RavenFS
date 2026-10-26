#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# rfs_dfs_footprint.sh —— 量化一次 DFS 遍历在 RavenFS mdata 缓存里留下的
# 「表结构足迹」（RADIX 静态基数树，不含 8K 行数据缓冲）。
#
# 原理：
#   1. 遍历前读 /proc/fs/ravenfs/<s_id>/stats；
#   2. find -xdev（GNU find 本身就是深度优先）走一遍目标目录；
#   3. 再读 stats，计算
#        Δlive_table = (md_table_add_bytes - md_table_del_bytes) 的前后差
#        Δlive_res   = (md_resident_add_bytes - md_resident_del_bytes) 前后差
#      Δlive_table 就是这次遍历新增、且**尚未被摘除**的表结构字节；
#   4. 等 TTL reap 把 8K 行数据回收后，再读一次：行数据会归零，但表结构
#      按 v1 契约（普通 reap 不摘表）保持不变，从而把两类足迹分离。
#
# 用法：
#   ./rfs_dfs_footprint.sh [选项] <挂载点或目录>
#
#   -n, --max-entries N   最多统计 N 个目录项（默认 0 = 全量）
#   -t, --max-seconds N   最多遍历 N 秒（默认 0 = 不限）
#   -w, --wait N          遍历后等待 N 秒观察 TTL reap（默认 8；
#                         TTL=2s + 扫描周期 4s，需 >6s 才会退行数据）
#       --no-idle         不等 TTL，直接报遍历后快照
#       --squeeze         遍历后调用 rfs_cachectl pressure 回收（需 root）
#       --stats PATH      手动指定 stats 文件（默认自动定位）
#   -h, --help
#
# 环境：
#   STATS=/proc/fs/ravenfs/nvme1n1/stats   同 --stats
#   RFS_CACHECTL=/usr/bin/rfs_cachectl      同 PATH 查找
#
# 注意：
#   · 表结构足迹 ≈ 每目录项 4~10 KiB（叶表 8 KiB / 128 槽 + 上层内部表），
#     全量遍历大卷前先确认可用内存；建议先 -n 20000 抽样外推。
#   · 普通 TTL reap 只归还 8K 行数据、不摘空表；要真正退还表结构需
#     PRESSURE 强淘汰（--squeeze 或 rfs_cachectl <mnt> pressure）。
#   · 只读 stats／metadata，不需要 root；--squeeze 才需要 CAP_SYS_ADMIN。

set -euo pipefail

PROG=$(basename "$0")
ROOT=""
MAX_ENTRIES=0
MAX_SECONDS=0
WAIT=8
DO_IDLE=1
DO_SQUEEZE=0
STATS="${STATS:-}"

usage() {
	sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

die() { echo "$PROG: $*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
	case "$1" in
	-n|--max-entries) MAX_ENTRIES="${2:-}"; shift 2 ;;
	-t|--max-seconds) MAX_SECONDS="${2:-}"; shift 2 ;;
	-w|--wait)        WAIT="${2:-}"; shift 2 ;;
	--no-idle)        DO_IDLE=0; shift ;;
	--squeeze)        DO_SQUEEZE=1; shift ;;
	--stats)          STATS="${2:-}"; shift 2 ;;
	-h|--help)        usage 0 ;;
	-*)               die "未知选项 '$1'（-h 看用法）" ;;
	*)
		[ -z "$ROOT" ] || die "只能指定一个目录"
		ROOT="$1"; shift ;;
	esac
done

[ -n "$ROOT" ] || usage 2
[ -d "$ROOT" ] || die "不是目录: $ROOT"
ROOT=$(realpath "$ROOT")
case "$MAX_ENTRIES$MAX_SECONDS$WAIT" in
	*[!0-9]*) die "-n/-t/-w 必须是非负整数" ;;
esac

# ── 定位 /proc/fs/ravenfs/<s_id>/stats ─────────────────────────────
resolve_stats() {
	local src s_id dev link

	if [ -n "$STATS" ]; then
		[ -r "$STATS" ] || die "stats 不可读: $STATS"
		return 0
	fi

	# 首选 findmnt：SOURCE = /dev/nvme1n1 → s_id = nvme1n1
	if command -v findmnt >/dev/null 2>&1; then
		src=$(findmnt -n -o SOURCE --target "$ROOT" 2>/dev/null | head -1 || true)
		if [ -n "$src" ]; then
			s_id=$(basename "$src")
			if [ -r "/proc/fs/ravenfs/$s_id/stats" ]; then
				STATS="/proc/fs/ravenfs/$s_id/stats"
				return 0
			fi
		fi
	fi

	# 回退：mountinfo（按 maj:min 查 /sys/dev/block）
	read -r dev src < <(awk -v target="$ROOT" '
		{
			sep = 0
			for (i = 6; i <= NF; i++)
				if ($i == "-") { sep = i; break }
			if (!sep || $(sep + 1) != "ravenfs")
				next
			mp = $5
			if (target == mp || mp == "/" ||
			    index(target, mp "/") == 1)
				print $3, $(sep + 2)
		}' /proc/self/mountinfo | tail -1)
	if [ -n "${dev:-}" ]; then
		src=$(readlink -f "/sys/dev/block/$dev" 2>/dev/null || true)
		s_id=$(basename "${src:-}")
		if [ -n "$s_id" ] && [ -r "/proc/fs/ravenfs/$s_id/stats" ]; then
			STATS="/proc/fs/ravenfs/$s_id/stats"
			return 0
		fi
	fi

	# 最后兜底：/proc/fs/ravenfs 下唯一条目
	local entries
	entries=$(find /proc/fs/ravenfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
	if [ "$entries" -eq 1 ]; then
		s_id=$(basename "$(find /proc/fs/ravenfs -mindepth 1 -maxdepth 1 -type d)")
		STATS="/proc/fs/ravenfs/$s_id/stats"
		[ -r "$STATS" ] || die "stats 不可读: $STATS"
		return 0
	fi

	die "无法定位 /proc/fs/ravenfs/<s_id>/stats（可用 --stats 指定）"
}

# ── 读一次快照：字段顺序固定，便于 read ────────────────────────────
# node_load resident_add resident_del table_add table_del
# leaf_alloc internal_alloc table_free reap retry
read_snap() {
	local f=$STATS
	awk '
		{ v[$1] = $2 }
		END {
			printf "%s %s %s %s %s %s %s %s %s %s\n",
				v["md_node_load"] + 0,
				v["md_resident_add_bytes"] + 0,
				v["md_resident_del_bytes"] + 0,
				v["md_table_add_bytes"] + 0,
				v["md_table_del_bytes"] + 0,
				v["md_table_alloc_leaf"] + 0,
				v["md_table_alloc_internal"] + 0,
				v["md_table_free"] + 0,
				v["md_reap"] + 0,
				v["md_retry"] + 0
		}' "$f"
}

live_table()  { echo $(( $4 - $5 )); }   # 依赖 read_snap 的变量名
live_res()    { echo $(( $2 - $3 )); }

mib() { awk -v b="$1" 'BEGIN { printf "%.2f MiB", b / 1048576 }'; }
kib_per() {
	awk -v b="$1" -v n="$2" 'BEGIN {
		if (n <= 0) { print "0 B"; exit }
		printf "%.2f KiB", (b / n) / 1024
	}'
}

resolve_stats

log "== RavenFS DFS 表结构足迹 =="
log "root   : $ROOT"
log "stats  : $STATS"

# shellcheck disable=SC2046
set -- $(read_snap)
B_NODE=$1 B_RADD=$2 B_RDEL=$3 B_TADD=$4 B_TDEL=$5
B_LEAF=$6 B_INT=$7 B_FREE=$8 B_REAP=$9 B_RETRY=${10}
B_TLIVE=$((B_TADD - B_TDEL))
B_RLIVE=$((B_RADD - B_RDEL))

log "before : node_load=$B_NODE  resident=$B_RLIVE B  table=$B_TLIVE B ($(mib "$B_TLIVE"))"
log "         leaf_alloc=$B_LEAF internal_alloc=$B_INT table_free=$B_FREE reap=$B_REAP"

# 安全提示：全量前粗估（目录项 * 4~10 KiB）
if [ "$MAX_ENTRIES" -eq 0 ] && [ "$MAX_SECONDS" -eq 0 ]; then
	USED=$(( $(stat -f -c '%c' "$ROOT") - $(stat -f -c '%d' "$ROOT") ))
	if [ "$USED" -gt 100000 ]; then
		log "warn   : 该挂载约 $USED 个 inode；表结构按「已打开的 leaf 窗口」计费"
		log "         （每窗口 8 KiB / 128 个节点），冷启动全量遍历可能到百 MiB~GB 级。"
		log "         建议先 -n N 抽样，或用 --squeeze 清空后再测冷启动足迹。"
	fi
fi

T0=$(date +%s%N)
TRUNC=0
COUNT_LINE=$(mktemp "${TMPDIR:-/tmp}/rfs_dfs_count.XXXXXX")
trap 'rm -f "$COUNT_LINE"' EXIT

# GNU find 默认就是深度优先（pre-order）；-xdev 不跨挂载。
# head 关闭管道 → find 收到 SIGPIPE 自行停止，实现条目上限。
FIND=(find "$ROOT" -xdev -printf '%y %d\n')
if [ "$MAX_SECONDS" -gt 0 ]; then
	FIND=(timeout "$MAX_SECONDS" "${FIND[@]}")
fi

log "DFS    : start (max_entries=$MAX_ENTRIES max_seconds=$MAX_SECONDS)"
set +o pipefail
if [ "$MAX_ENTRIES" -gt 0 ]; then
	"${FIND[@]}" 2>/dev/null | head -n "$MAX_ENTRIES" |
		awk '{ n++; t[$1]++; if ($2 > d) d = $2 }
		     END { printf "%d %d %d %d %d %d\n",
			   n, t["d"], t["f"], t["l"],
			   n - t["d"] - t["f"] - t["l"], d }' >"$COUNT_LINE"
else
	"${FIND[@]}" 2>/dev/null |
		awk '{ n++; t[$1]++; if ($2 > d) d = $2 }
		     END { printf "%d %d %d %d %d %d\n",
			   n, t["d"], t["f"], t["l"],
			   n - t["d"] - t["f"] - t["l"], d }' >"$COUNT_LINE"
fi
FIND_RC=$?
set -o pipefail
[ "$FIND_RC" -eq 124 ] && TRUNC=1
[ "$MAX_SECONDS" -gt 0 ] || true

read -r N_ENT N_DIR N_FILE N_LINK N_OTHER N_DEPTH <"$COUNT_LINE"
[ -n "${N_ENT:-}" ] || N_ENT=0
T1=$(date +%s%N)
ELAPSED_MS=$(( (T1 - T0) / 1000000 ))
[ "$MAX_ENTRIES" -gt 0 ] && [ "$N_ENT" -ge "$MAX_ENTRIES" ] && TRUNC=1

# shellcheck disable=SC2046
set -- $(read_snap)
A_NODE=$1 A_RADD=$2 A_RDEL=$3 A_TADD=$4 A_TDEL=$5
A_LEAF=$6 A_INT=$7 A_FREE=$8 A_REAP=$9 A_RETRY=${10}
A_TLIVE=$((A_TADD - A_TDEL))
A_RLIVE=$((A_RADD - A_RDEL))

D_NODE=$((A_NODE - B_NODE))
D_LEAF=$((A_LEAF - B_LEAF))
D_INT=$((A_INT - B_INT))
D_FREE=$((A_FREE - B_FREE))
D_REAP=$((A_REAP - B_REAP))
D_TLIVE=$((A_TLIVE - B_TLIVE))
D_RLIVE=$((A_RLIVE - B_RLIVE))

log "DFS    : done elapsed=${ELAPSED_MS}ms truncated=$TRUNC"
log "entries: $N_ENT (dirs=$N_DIR files=$N_FILE links=$N_LINK other=$N_OTHER) maxdepth=$N_DEPTH"
log "after  : node_load=$A_NODE  resident=$A_RLIVE B ($(mib "$A_RLIVE"))  table=$A_TLIVE B ($(mib "$A_TLIVE"))"
log "delta  : nodes=+$D_NODE  reap=+$D_REAP  leaf=+$D_LEAF  internal=+$D_INT  table_free=+$D_FREE"
log "         resident=+$(mib "$D_RLIVE")  table=+$(mib "$D_TLIVE")  ($D_TLIVE B)"

if [ "$D_NODE" -gt 0 ]; then
	log "ratio  : table $(kib_per "$D_TLIVE" "$D_NODE")/node, $(kib_per "$D_TLIVE" "$N_ENT")/entry"
	if [ "$D_TLIVE" -eq 0 ]; then
		log "note   : 本次没有新开 leaf 窗口（节点落进已存在的叶表）；"
		log "         非零足迹见 before/after 的 live table，属于更早遍历的累计。"
	fi
fi
if [ "$N_ENT" -gt 0 ]; then
	log "         table footprint by type: leaf +$D_LEAF x 8 KiB, internal +$D_INT"
	log "         ≈ $(kib_per "$D_TLIVE" "$N_ENT") per entry; 1M entries ≈ $(awk -v b="$D_TLIVE" -v n="$N_ENT" 'BEGIN{printf "%.1f GiB", (b/n)*1000000/1073741824}')"
fi

# ── TTL reap 后再看：行数据会归零，表结构应保持 ────────────────────
if [ "$DO_IDLE" -eq 1 ] && [ "$WAIT" -gt 0 ]; then
	sleep "$WAIT"
	# shellcheck disable=SC2046
	set -- $(read_snap)
	I_RADD=$2 I_RDEL=$3 I_TADD=$4 I_TDEL=$5
	I_RLIVE=$((I_RADD - I_RDEL))
	I_TLIVE=$((I_TADD - I_TDEL))
	log "idle   : +${WAIT}s 后 node_load=$(($1))  resident=$I_RLIVE B ($(mib "$I_RLIVE"))  table=$I_TLIVE B ($(mib "$I_TLIVE"))"
	log "         （普通 reap 只退 8K 行；表结构只有 PRESSURE 强淘汰才摘）"
fi

# ── 可选：强淘汰回收并报告实际退还 ─────────────────────────────────
if [ "$DO_SQUEEZE" -eq 1 ]; then
	[ "$(id -u)" -eq 0 ] || die "--squeeze 需要 root（CAP_SYS_ADMIN）"
	CTL="${RFS_CACHECTL:-}"
	if [ -z "$CTL" ] && command -v rfs_cachectl >/dev/null 2>&1; then
		CTL=$(command -v rfs_cachectl)
	fi
	if [ -z "$CTL" ] && [ -x "$(dirname "$0")/rfs_cachectl" ]; then
		CTL="$(dirname "$0")/rfs_cachectl"
	fi
	[ -n "$CTL" ] && [ -x "$CTL" ] || die "未找到 rfs_cachectl（设 RFS_CACHECTL=）"

	# shellcheck disable=SC2046
	set -- $(read_snap)
	Q_TLIVE=$(( $4 - $5 ))
	Q_RLIVE=$(( $2 - $3 ))
	log "squeeze: $CTL $ROOT pressure"
	"$CTL" "$ROOT" pressure || true
	# shellcheck disable=SC2046
	set -- $(read_snap)
	Z_TLIVE=$(( $4 - $5 ))
	Z_RLIVE=$(( $2 - $3 ))
	log "reclaim: resident $Q_RLIVE B -> $Z_RLIVE B (退还 $((Q_RLIVE - Z_RLIVE)) B)"
	log "         table    $Q_TLIVE B -> $Z_TLIVE B (退还 $((Q_TLIVE - Z_TLIVE)) B, $(mib "$((Q_TLIVE - Z_TLIVE))"))"
fi
