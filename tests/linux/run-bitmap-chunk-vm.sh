#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# run-bitmap-chunk-vm.sh — Linux 侧位图验收 harness · 宿主编排段（Phase B）
#
# 职责：确保 VM 起来 → 投递 guest 工作段（tests/linux/bitmap_chunk_guest.sh）
#       → 分 pass 驱动全流程 → 日志收拢到 linux/vm/test-runs/<ts>-bitmap-chunk/
#
# 用法：
#   run-bitmap-chunk-vm.sh            # 默认：CHUNK(backing=1, order=4) + FULL 控制组
#   RUN=1  run-bitmap-chunk-vm.sh     # 只跑 CHUNK
#   RUN=0  run-bitmap-chunk-vm.sh     # 只跑 FULL
#   ORDER=8 RUN=1 run-bitmap-chunk-vm.sh   # CHUNK order 变体
#   N_SMALL=800 FILL_CAP_MB=33000 TTL_WAIT=35  # 透传 guest 段参数
#
# 前置（已由 Docs/linux/ravenfs-vm-env-notes.md 落地）：
#   linux/vm/disks/{root-ext4,root-ravenfs}.qcow2 + esp.img；guest raven 免密 sudo；
#   宿主 ssh key 已装 guest；试验盘 serial=deadbeef（**mkfs 会抹掉它**）。
#
# 判据输出：grep '\[BITMAP-ACCEPT\]' <pass日志>（PASS/FAIL 逐条 + 汇总）。
# 2026-09-28 · 测试团队 Phase B · 写范围 tests/linux/（vm/ 仅落日志）
# ═══════════════════════════════════════════════════════════════════
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
VM="$REPO/linux/vm"
SSHPORT="${SSHPORT:-2222}"
SSH_OPTS=(-p "$SSHPORT" -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/tmp/kh_vm
          -o ConnectTimeout=5 -o BatchMode=yes)
SSHCMD=(ssh "${SSH_OPTS[@]}" raven@localhost)
SCPCMD=(scp -P "$SSHPORT" -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/tmp/kh_vm
        -o BatchMode=yes)
RUN="${RUN:-both}"      # both | 1 (CHUNK) | 0 (FULL)
ORDER="${ORDER:-4}"

TS="$(date +%Y%m%d-%H%M%S)"
OUT="$VM/test-runs/$TS-bitmap-chunk"
mkdir -p "$OUT"

log()  { echo "[orchestrator] $*"; }
die()  { echo "[orchestrator][FATAL] $*"; exit 2; }

ssh_ok() { "${SSHCMD[@]}" true >/dev/null 2>&1; }

ensure_vm() {
    if ssh_ok; then log "VM 已在跑（:$SSHPORT）"; return 0; fi
    log "启动 VM（run-vm.sh daily）…"
    ( cd "$VM" && setsid nohup ./run-vm.sh daily > "$OUT/qemu.log" 2>&1 & )
    local i
    for i in $(seq 1 90); do
        sleep 2
        if ssh_ok; then log "VM ssh 就绪（$((i*2))s）"; return 0; fi
    done
    die "VM 未在 180s 内就绪（见 $OUT/qemu.log）"
}

# —— 简化：直接逐相调用（guest 段已打进 scp 的脚本）——
pass_run() {
    local b="$1" L="$OUT/pass$1.log"
    : > "$L"
    local -a phases
    phases=("load $b $ORDER" mkfs mount stress ttl umount audit verify unload)
    for ph in "${phases[@]}"; do
        log "pass$b: $ph"
        if ! "${SSHCMD[@]}" "bash /tmp/bm-accept.sh $ph" >> "$L" 2>&1; then
            log "pass$b: '$ph' 退出非零（日志见 pass$b.log，继续收集）"
        fi
        # 关键相失败即停（mkfs/load/mount 挂不上就没必要往下走）
        if [ "$ph" = mkfs ]   && ! grep -q "fresh mkfs" "$L"; then die "pass$b mkfs 失败（见 pass$b.log）"; fi
        if [ "$ph" = load ]   && ! grep -q "loaded:" "$L"; then die "pass$b load 失败"; fi
        if [ "$ph" = mount ]  && ! grep -q "mounted" "$L";   then die "pass$b mount 失败"; fi
        if [ "$ph" = verify ] && grep -qF "FATAL" "$L"; then log "pass$b verify 中断"; fi
    done
    local p f
    p=$(grep -c '\[BITMAP-ACCEPT\] PASS' "$L" || true)
    f=$(grep -c '\[BITMAP-ACCEPT\] FAIL' "$L" || true)
    echo "SUMMARY pass(backing=$b order=$ORDER): PASS=$p FAIL=$f" | tee -a "$OUT/SUMMARY.txt"
    grep '\[BITMAP-ACCEPT\]' "$L" | sed 's/^/  /' | tee -a "$OUT/SUMMARY.txt"
}

log "输出目录: $OUT"
ensure_vm

log "投递 guest 段 → /tmp/bm-accept.sh"
"${SCPCMD[@]}" "$REPO/tests/linux/bitmap_chunk_guest.sh" raven@localhost:/tmp/bm-accept.sh \
    || die "投递失败"
"${SSHCMD[@]}" 'chmod +x /tmp/bm-accept.sh && rm -rf /tmp/bm-accept && mkdir -p /tmp/bm-accept' || die "chmod 失败"

log "一次性 guest 构建（ko + mkfs 备份）"
if ! "${SSHCMD[@]}" 'bash /tmp/bm-accept.sh build' > "$OUT/build.log" 2>&1; then
    tail -20 "$OUT/build.log"
    die "guest 构建失败（见 $OUT/build.log）"
fi
tail -5 "$OUT/build.log"

case "$RUN" in
    both) pass_run 1; pass_run 0 ;;
    1)    pass_run 1 ;;
    0)    pass_run 0 ;;
    *) die "RUN ∈ {both,1,0}" ;;
esac

echo; echo "══ 最终汇总（grep BITMAP-ACCEPT 全量）══"
cat "$OUT/SUMMARY.txt" 2>/dev/null || true
TOT_F=$(grep -h '\[BITMAP-ACCEPT\] FAIL' "$OUT"/pass*.log 2>/dev/null | wc -l)
log "总 FAIL=$TOT_F；日志目录 $OUT"
[ "$TOT_F" -eq 0 ]
