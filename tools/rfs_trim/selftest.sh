#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════
# rfs_trim 自测（不联网、不 sudo、只碰 $TMP 临时目录）
#
# 覆盖：
#   1) --selftest-plan   纯函数（位图 → granularity 对齐段列表）离线单测
#   2) 文件模式端到端    16MiB 稀疏镜像 + 合法 SB + block 位图：
#                       dry-run 只报告不打洞 → --apply 真打洞（st_blocks 下降、
#                       占用区字节不变、报告数字与构造一致）→ 再 --apply 幂等
#   3) 拒跑             尾巴（自描述三段）非全 1 → exit 1 且不打洞
#   4) 边角             用法 exit 2 / --help exit 0 / 非 RavenFS 魔数 exit 1
# 用法：./selftest.sh   （或 make check）
# ═══════════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")"

PASS=0; FAIL=0
ok() { printf 'ok    %s\n' "$*"; PASS=$((PASS + 1)); }
ng() { printf 'FAIL  %s\n' "$*"; FAIL=$((FAIL + 1)); }
expect_eq() { # desc got want
    if [ "$2" = "$3" ]; then ok "$1 → $2"; else ng "$1: got='$2' want='$3'"; fi
}
expect_lt() { # desc got limit
    if [ "$2" -lt "$3" ] 2>/dev/null; then ok "$1 → $2 < $3"; else ng "$1: got='$2' not < '$3'"; fi
}
expect_grep() { # desc pattern file
    if grep -Eq "$2" "$3"; then ok "$1"; else ng "$1（$2 未命中 $3）"; fi
}

# ── 0. 构建（-Wall -Wextra，0 error 0 warn；有告警则 make 输出会被下面断言抓住）──
MAKE_OUT=$(make 2>&1) || { echo "$MAKE_OUT"; ng "make 构建"; exit 1; }
if echo "$MAKE_OUT" | grep -qiE 'warning|error'; then
    echo "$MAKE_OUT"; ng "make 应 0 error 0 warn"
else
    ok "make 构建（0 error 0 warn）"
fi

# ── 1. 纯函数单测 ──
if ./rfs_trim --selftest-plan > /tmp/rfs_trim_plan.$$ 2>&1; then
    ok "--selftest-plan 纯函数 5 例全过"
else
    cat /tmp/rfs_trim_plan.$$; ng "--selftest-plan"
fi
rm -f /tmp/rfs_trim_plan.$$

command -v python3 >/dev/null 2>&1 || { echo "selftest: 需要 python3 构造测试镜像"; exit 1; }

TMP=$(mktemp -d "${RFS_TRIM_TESTDIR:-/tmp}/rfs_trim_selftest.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

# ── 测试几何（16MiB 镜像；公式同 mkfs calc_geometry）──
#   total_blocks=4095（world_base_lba=1 → world 占满 16MiB-4K）
#   bitmap_blocks=1 → block 位图 [4094,4095)
#   inode_array=64 → 数组 4 块 [4090,4094)，位图 1 块 [4089,4090)
#   空闲游程 = FS_block [1000,3000) → 逻辑空闲 2000 块
export TOT=4095 IC=64 WBL=1 FREE_LO=1000 FREE_HI=3000
EXP_FS=$((TOT * 4096))                 # 16773120
EXP_FREE=$(( (FREE_HI - FREE_LO) * 4096 ))  # 8192000

# ── 2. 生成器 / 校验器（python3，heredoc 落盘到 $TMP）──
cat > "$TMP/rfstest.py" <<'PYEOF'
import os, struct, sys

TOT = int(os.environ["TOT"]); IC = int(os.environ["IC"]); WBL = int(os.environ["WBL"])
FLO = int(os.environ["FREE_LO"]); FHI = int(os.environ["FREE_HI"])
FSZ = 16 * 1024 * 1024
WORLD = WBL * 4096                      # FS_block[0] 绝对字节
PAT = bytes(((i * 7 + 13) & 0xFF) for i in range(4096))   # 每 4096B 一循环的填充图案

def geometry():
    bb = (TOT + 32767) // 32768
    bitmap_start = TOT - bb
    ib = (IC * 256 + 4095) // 4096
    iab = bitmap_start - ib
    ibb = (IC + 32767) // 32768
    ibm = iab - ibb
    return bitmap_start, iab, ibm

def make_sb():
    sb = bytearray(4096)
    struct.pack_into("<Q", sb, 0,  0x5241564E)   # magic "RAVN"
    struct.pack_into("<I", sb, 8,  2)            # version 2 = v1.1
    struct.pack_into("<Q", sb, 16, WBL)          # world_base_lba
    struct.pack_into("<Q", sb, 24, TOT)          # total_blocks
    struct.pack_into("<Q", sb, 32, IC)           # inode_array_count
    return bytes(sb)

def make_bm(mode):
    bm = bytearray(b"\xff" * 4096)               # LSB-first，同 mkfs BitmapBuf::mark
    for b in range(FLO, FHI):                    # 空闲游程 [1000,3000)
        bm[b // 8] &= ~(1 << (b % 8))
    if mode == "badtail":
        _, _, ibm = geometry()
        b = ibm + 1                              # 自描述三段内（inode 数组区）挖一个 0
        bm[b // 8] &= ~(1 << (b % 8))
    return bytes(bm)

def gen(img, mode):
    bitmap_start, _, _ = geometry()
    with open(img, "wb") as f:
        f.truncate(FSZ)
        for _ in range(FSZ // 4096):
            f.write(PAT)                         # 全盘填图案（先于 SB/位图）
        f.seek(0); f.write(make_sb())
        f.seek(WORLD + bitmap_start * 4096); f.write(make_bm(mode))
        f.flush(); os.fsync(f.fileno())

def verify(img, phase):                          # phase: pre（未打洞）| post（已打洞）
    bitmap_start, _, _ = geometry()
    bm_off = WORLD + bitmap_start * 4096
    free_lo = WORLD + FLO * 4096
    free_len = (FHI - FLO) * 4096
    with open(img, "rb") as f:
        raw = f.read()
    bad = []
    if len(raw) != FSZ:                          bad.append("文件尺寸被改")
    if raw[0:4096] != make_sb():                 bad.append("SB 字节被改")
    if raw[WORLD:free_lo] != PAT * FLO:          bad.append("占用头区 [0,1000) 字节被改")
    if raw[WORLD + FHI * 4096:bm_off] != PAT * (bitmap_start - FHI):
                                                 bad.append("占用尾区 [3000,4094) 字节被改")
    if raw[bm_off:bm_off + 4096] != make_bm("ok"): bad.append("block 位图字节被改")
    hole = raw[free_lo:free_lo + free_len]
    if phase == "pre":
        if hole != PAT * (FHI - FLO):            bad.append("空闲区字节被改（应仍是填充图案）")
    else:
        if hole != b"\0" * free_len:             bad.append("空闲区未被真正打洞（非全 0）")
    if bad:
        print("verify[%s] FAIL: %s" % (phase, "; ".join(bad)))
        return 1
    print("verify[%s] ok: SB/占用区/位图不变，空闲区%s" %
          (phase, "仍是图案（未打洞）" if phase == "pre" else "全 0（真打洞）"))
    return 0

def main():
    if sys.argv[1] == "gen":     gen(sys.argv[2], sys.argv[3]); return 0
    if sys.argv[1] == "verify":  return verify(sys.argv[2], sys.argv[3])
    return 2

sys.exit(main())
PYEOF

IMG="$TMP/raven.img"
BAD="$TMP/badtail.img"

# ── 3. 构造正常镜像 ──
if python3 "$TMP/rfstest.py" gen "$IMG" ok; then ok "构造 16MiB 测试镜像"; else ng "构造镜像"; fi
BEFORE=$(stat -c %b "$IMG")
expect_eq "构造后 st_blocks（全填满）" "$BEFORE" "32768"

# ── 4. dry-run（默认）：只报告、不打洞 ──
./rfs_trim "$IMG" > "$TMP/dry.txt" 2>&1
expect_eq "dry-run 退出码" "$?" "0"
expect_grep "dry-run 标记"  '\[dry-run\] 只报告未动手' "$TMP/dry.txt"
expect_grep "报告: FS 覆盖字节"      "FS 覆盖字节 +: $EXP_FS"            "$TMP/dry.txt"
expect_grep "报告: 逻辑空闲字节"     "逻辑空闲字节 +: $EXP_FREE \(2000 blocks\)" "$TMP/dry.txt"
expect_grep "报告: 对齐后可裁字节"   "对齐后可裁字节 +: $EXP_FREE"        "$TMP/dry.txt"
expect_grep "报告: 损耗比 0.00%"     "损耗比 +: 0\.00%"                  "$TMP/dry.txt"
expect_grep "报告: 段数 1→1"         "段数 +: 原始 1 → 对齐后 1"          "$TMP/dry.txt"
expect_grep "报告: 自描述三段全 1"   "自描述三段 \[4089, 4095\) .*全 1"   "$TMP/dry.txt"
expect_eq "dry-run 后 st_blocks 未变" "$(stat -c %b "$IMG")" "$BEFORE"
if python3 "$TMP/rfstest.py" verify "$IMG" pre >/dev/null; then
    ok "dry-run 后镜像字节未动（空闲区仍图案、未打洞）"
else
    ng "dry-run 后镜像字节被改"
fi

# ── 5. --apply：真打洞 ──
./rfs_trim "$IMG" --apply > "$TMP/apply.txt" 2>&1
expect_eq "apply 退出码" "$?" "0"
expect_grep "apply: 段 1/1 裁掉"    'apply 段 1/1: \[4100096, \+8192000\)' "$TMP/apply.txt"
expect_grep "apply: 汇总"            "apply: 裁掉 $EXP_FREE B / 1 段 / 失败 0 批" "$TMP/apply.txt"
if grep -q '\[dry-run\]' "$TMP/apply.txt"; then ng "apply 输出不应含 dry-run 标记"; else ok "apply 输出不含 dry-run 标记"; fi
AFTER=$(stat -c %b "$IMG")
expect_lt "apply 后 st_blocks 下降（真打洞）" "$AFTER" "$BEFORE"
expect_eq "apply 后 st_blocks 精确值（32768-8192000/512）" "$AFTER" "16768"
if python3 "$TMP/rfstest.py" verify "$IMG" post; then
    ok "apply 后占用区字节不变 + 空闲区全 0"
else
    ng "apply 后字节校验"
fi

# ── 6. 幂等：再 --apply 一次（洞已存在，仍成功且不再变化）──
./rfs_trim "$IMG" --apply > "$TMP/apply2.txt" 2>&1
expect_eq "二次 apply 退出码" "$?" "0"
expect_eq "二次 apply 后 st_blocks 不再变" "$(stat -c %b "$IMG")" "$AFTER"

# ── 7. 尾巴（自描述三段 [4089,4095)）非全 1 → 拒跑且不打洞 ──
python3 "$TMP/rfstest.py" gen "$BAD" badtail
BAD_BEFORE=$(stat -c %b "$BAD")
./rfs_trim "$BAD" --apply > "$TMP/bad.txt" 2>&1
expect_eq "尾巴非全 1 → 拒跑退出码" "$?" "1"
expect_grep "拒跑理由" '非全 1.*拒跑' "$TMP/bad.txt"
expect_eq "拒跑后未打洞（st_blocks 未变）" "$(stat -c %b "$BAD")" "$BAD_BEFORE"

# ── 8. 边角：用法 / 帮助 / 非 RavenFS 魔数 ──
./rfs_trim > /dev/null 2>&1
expect_eq "无参数 → 用法 exit 2" "$?" "2"
./rfs_trim --help > /dev/null 2>&1
expect_eq "--help → exit 0" "$?" "0"
head -c 4096 /dev/zero > "$TMP/zeros.img"
./rfs_trim "$TMP/zeros.img" --apply > "$TMP/zeros.txt" 2>&1
expect_eq "非 RavenFS（零块）→ 拒跑 exit 1" "$?" "1"
expect_grep "魔数诊断" '不是 "RAVN"' "$TMP/zeros.txt"

# ── 汇总 ──
echo "----------------------------------------"
echo "selftest: PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" -ne 0 ]; then exit 1; fi
echo "selftest: ALL PASSED"
