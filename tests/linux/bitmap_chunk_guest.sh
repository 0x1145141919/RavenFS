#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# bitmap_chunk_guest.sh — Linux 侧位图（CHUNK 后端）验收 harness · guest 工作段
#
# 用途（03 §10 端到端验收，测试团队 Phase B）：
#   build    编译 ravenfs.ko（guest 内可写副本，vermagic 对齐）+ 备 mkfs
#   load <0|1> [order]   insmod（bitmap_backing=1 → CHUNK；0 → FULL 控制组）
#   mkfs     按 QEMU serial 认盘（deadbeef=试验盘），fresh mkfs（**v1.2 默认**）
#   mount    挂载 + 基线记录（statfs f_bfree/f_files）
#   stress   分配/释放/文件建删压测（中型文件 + inode churn + 截断/追加 +
#            ENOSPC 往返）→ 落盘清单（size+md5）+ statfs 快照
#   ttl      sync + 等一个 TTL 周期（脏 chunk 落盘，dmesg 抓 bm_err）
#   umount   卸载（收尾落盘）
#   audit    audit_v1.2 离线审计（盘态 == 预期，无双分配/孤儿）
#   verify   重挂载 → 清单/校验和/statfs 三重比对（★ 重算计数 == 内存计数）
#   unload   卸模块
#   all <0|1> [order]    一键全流程
#
# 判据（每条打印 [BITMAP-ACCEPT] PASS/FAIL）：
#   C1 stress 后清单可复现（文件数/大小/md5 全等）
#   C2 ★ statfs：卸载前内存计数 == 重挂载后盘上重算计数（§10.6 计数保真；
#        重复 free 虚高 / 漏清 都会在此现形）
#   C3 ★ ENOSPC 往返：写满 → 删净 → f_bfree 回到写满前基线（不虚高不漏）
#   C4 audit_v1.2 全绿（位图/extent 树/名字堆无孤儿无双分配）
#   C5 dmesg 无 ravenfs error（bm_err=0 / wb_err=0 / 无 BUG/Oops）
#   C6 inode churn 往返：f_files 用后回落
#
# 2026-09-28 · Docs/runtime_tech/03_chunk_backed_slices.md（§10 验收）
# 写范围：tests/linux/（新）。本脚本只在 **guest 内**执行（raven，NOPASSWD sudo）。
# ═══════════════════════════════════════════════════════════════════
set -u

SRC="${RFS_SRC:-/rfssrc}"
MNT="${MNT:-/mnt/rfs}"
STATE="${STATE:-/tmp/bm-accept}"
BUILD="${BUILD:-/tmp/rfs-bm-build}"
N_SMALL="${N_SMALL:-800}"          # inode churn 文件数
FILL_CAP_MB="${FILL_CAP_MB:-33000}" # ENOSPC 填充上限（防失控；32G 盘内）
TTL_WAIT="${TTL_WAIT:-35}"         # TTL 周期 30s + 余量

PASS=0; FAIL=0
log()  { echo "[harness] $*"; }
ck()   { # ck <ok?> <描述>
    if [ "$1" = 1 ]; then PASS=$((PASS+1)); echo "[BITMAP-ACCEPT] PASS: $2"
    else FAIL=$((FAIL+1)); echo "[BITMAP-ACCEPT] FAIL: $2"; fi
}
die()  { echo "[harness][FATAL] $*"; echo "[BITMAP-ACCEPT] FAIL: $*"; exit 2; }
need_dev() {
    [ -n "${DEV:-}" ] && return 0
    for d in /sys/block/nvme*n1; do
        s=$(tr -d '[:space:]' < "$d/device/serial" 2>/dev/null || true)
        if [ "$s" = deadbeef ]; then DEV="/dev/$(basename "$d")"; return 0; fi
    done
    return 1
}
# stat -f 口径（df --output 无 f_bfree 字段——2026-09-28 实测）：
#   %f = f_bfree 空闲块（★ C2/C3 计数保真判据口径）；%S = 块大小
#   %c/%d = inode 总数/空闲数 → used = %c - %d
free_blocks()  { stat -f -c %f "$MNT" 2>/dev/null; }
inode_used()   { local t d; t=$(stat -f -c %c "$MNT"); d=$(stat -f -c %d "$MNT"); echo $((t - d)); }
dmesg_s()      { sudo dmesg "$@"; }   # raven 无 CAP_SYSLOG → 必须 sudo（实测）

# ── 9p 挂载（重启后不自动挂；见 ravenfs-vm-env-notes §6 重启坑） ──
ensure_9p() {
    if ! mountpoint -q /rfssrc; then
        sudo mkdir -p /rfssrc
        sudo mount -t 9p -o trans=virtio,version=9p2000.L,ro rfssrc /rfssrc \
            || die "9p rfssrc（源码）挂载失败"
        log "9p rfssrc 已挂（ro 源码）"
    fi
    if ! mountpoint -q /host; then
        sudo mkdir -p /host
        sudo mount -t 9p -o trans=virtio,version=9p2000.L hostshare /host || true
    fi
}

# ── build：可写副本编译（9p 只读；头 ../include 需一并拷贝） ──
phase_build() {
    ensure_9p
    log "build: 拷源码 → $BUILD（guest 内编译，内核 $(uname -r)）"
    rm -rf "$BUILD"; mkdir -p "$BUILD/linux" "$BUILD/include"
    # ⚠️ 只拷 kbuild 需要的文件：整树 cp 会把 linux/vm（36G qcow2！）灌进 guest /tmp
    #    —— vm-env-notes §6 的 9p 大坑，cp 同款（2026-09-28 实测踩过）。
    cp -f "$SRC/linux/Makefile" "$SRC/linux"/*.c "$SRC/linux"/*.h "$BUILD/linux/" \
        || die "拷贝 linux/ 源失败"
    cp -rf "$SRC/include/." "$BUILD/include/" || die "拷贝 include/ 失败"
    make -C "/lib/modules/$(uname -r)/build" M="$BUILD/linux" modules > "$STATE/build.log" 2>&1
    local rc=$?
    tail -5 "$STATE/build.log"
    [ $rc -eq 0 ] && [ -f "$BUILD/linux/ravenfs.ko" ] || die "ko 编译失败（见 $STATE/build.log）"
    cp -f "$SRC/mkfs/mkfs.ravenfs" "$BUILD/mkfs.ravenfs" && chmod +x "$BUILD/mkfs.ravenfs"
    log "ko OK: $(modinfo -F filename "$BUILD/linux/ravenfs.ko" 2>/dev/null) srcversion=$(modinfo -F srcversion "$BUILD/linux/ravenfs.ko" 2>/dev/null)"
    ck 1 "C0 guest 内 ko 编译成功"
}

# ── load <backing> [order] ──
phase_load() {
    local backing="${1:-1}" order="${2:-4}"
    if grep -qw ravenfs /proc/filesystems; then sudo rmmod ravenfs || die "rmmod 旧模块失败"; fi
    sudo insmod "$BUILD/linux/ravenfs.ko" bitmap_backing="$backing" bitmap_chunk_order="$order" \
        || die "insmod 失败（backing=$backing order=$order）"
    grep -qw ravenfs /proc/filesystems || die "模块未注册 /proc/filesystems"
    log "loaded: bitmap_backing=$backing bitmap_chunk_order=$order"
    echo "$backing" > "$STATE/backing"; echo "$order" > "$STATE/order"
    dmesg_s | tail -5
}

# ── mkfs（v1.2 默认；按 serial 认盘） ──
phase_mkfs() {
    need_dev || die "找不到 serial=deadbeef 的试验盘"
    log "mkfs: $DEV（v1.2 默认）"
    mount | grep -q "^$DEV" && die "目标盘仍被挂载，先 umount"
    sudo "$BUILD/mkfs.ravenfs" -f "$DEV" > "$STATE/mkfs.log" 2>&1 \
        || die "mkfs 失败（见 $STATE/mkfs.log）：$(tail -1 "$STATE/mkfs.log")"
    tail -3 "$STATE/mkfs.log"
    ck 1 "C0 fresh mkfs（v1.2）@ $DEV"
}

# ── mount + 基线 ──
phase_mount() {
    need_dev || die "无目标盘"
    sudo mkdir -p "$MNT"
    sudo mount -t ravenfs "$DEV" "$MNT" || die "mount 失败"
    stat -f -c 'fsID=%t' "$MNT" | grep -qi 5241564e || log "警告: statfs ID 非 RAVN"
    df -B1 "$MNT" > "$STATE/df.baseline" 2>/dev/null || df "$MNT" > "$STATE/df.baseline"
    free_blocks > "$STATE/free.baseline"
    log "mounted；baseline free_blocks=$(cat "$STATE/free.baseline")"
    dmesg_s | tail -3
    ck 1 "C0 CHUNK/FULL 挂载成功"
}

# ── stress：分配/释放/文件建删压测 ──
phase_stress() {
    [ -d "$MNT" ] || die "未挂载"
    local W="$MNT/bmtest"
    sudo mkdir -p "$W"
    sudo chown "$(id -u):$(id -g)" "$W" || die "bmtest chown 失败"

    # A. 中型文件建/写（block 分配）×16
    log "stress A: 16 × 中型文件"
    local i
    for i in $(seq 1 16); do
        dd if=/dev/urandom of="$W/a$i.bin" bs=64K count=$(( (i % 8) + 1 )) status=none \
            || die "A 写 a$i.bin 失败"
    done

    # B. inode churn：N_SMALL 建 + 隔一删一（inode 分配/释放往返）
    log "stress B: $N_SMALL 小文件建删"
    for i in $(seq 1 "$N_SMALL"); do echo "x$i" > "$W/n$i" || die "B 建 n$i 失败"; done
    local used1; used1=$(inode_used)
    for i in $(seq 2 2 "$N_SMALL"); do rm -f "$W/n$i" || die "B 删 n$i 失败"; done
    local used2; used2=$(inode_used)
    echo "$used1 $used2" > "$STATE/inode.churn"
    log "inode used: churn 中 $used1 → 删半后 $used2"

    # C. 追加/截断/覆写（扩缩分配）
    log "stress C: 追加/截断/覆写"
    dd if=/dev/urandom of="$W/a1.bin" bs=64K count=4 conv=notrunc status=none
    truncate -s 1024K "$W/a2.bin" || die "C truncate 增失败"
    truncate -s 37K   "$W/a3.bin"  || die "C truncate 缩失败"
    dd if=/dev/urandom of="$W/a4.bin" bs=64K count=8 conv=notrunc status=none

    sync
    # D. ENOSPC 往返（★ C3 计数保真判据）
    local f0 fmin f1
    f0=$(free_blocks)
    log "stress D: ENOSPC 填充（cap=${FILL_CAP_MB}M）"
    dd if=/dev/zero of="$W/fill.bin" bs=1M count="$FILL_CAP_MB" 2>"$STATE/fill.log" || true
    fmin=$(free_blocks)
    log "fill 后 f_bfree=$fmin（前 $f0）"
    rm -f "$W/fill.bin"
    sync
    f1=$(free_blocks)
    echo "$f0 $fmin $f1" > "$STATE/enospc.roundtrip"
    log "删净后 f_bfree=$f1"

    # E. 清单（size + md5）+ statfs 快照
    ( cd "$MNT" && find bmtest -type f ! -name 'fill.bin' | LC_ALL=C sort | \
        while read -r f; do printf '%s %s %s\n' "$f" "$(stat -c%s "$f")" "$(md5sum "$f" | cut -d' ' -f1)"; done ) \
        > "$STATE/manifest.stress"
    df -B1 "$MNT" > "$STATE/df.stress"
    free_blocks > "$STATE/free.stress"
    log "manifest: $(wc -l < "$STATE/manifest.stress") 项；free_blocks=$(cat "$STATE/free.stress")"
    ck 1 "C1a stress 全程零错误（写/建/删/截断/ENOSPC）"
}

# ── ttl：sync + 一个 TTL 周期 ──
phase_ttl() {
    sync
    log "TTL 等待 ${TTL_WAIT}s（脏 chunk 落盘）"
    sleep "$TTL_WAIT"
    dmesg_s | grep -E "ravenfs: ttl|bm_err" | tail -4 > "$STATE/ttl.dmesg" || true
    cat "$STATE/ttl.dmesg"
    ck 1 "C0 TTL 周期等待完成"
}

# ── umount ──
phase_umount() {
    free_blocks > "$STATE/free.preumount"
    df -B1 "$MNT" > "$STATE/df.preumount"
    sync
    sudo umount "$MNT" || die "umount 失败"
    log "umounted"
    ck 1 "C0 卸载成功（收尾落盘）"
}

# ── audit：audit_v1.2 离线审计 ──
phase_audit() {
    need_dev || die "无目标盘"
    ensure_9p
    log "audit_v1.2 $DEV"
    if [ -x "$SRC/tools/audit_v1.2/audit_v1.2" ]; then
        sudo "$SRC/tools/audit_v1.2/audit_v1.2" "$DEV" > "$STATE/audit.log" 2>&1
        local rc=$?
        tail -8 "$STATE/audit.log"
        [ $rc -eq 0 ] && ck 1 "C4 audit_v1.2 全绿" || ck 0 "C4 audit_v1.2 失败 rc=$rc（见 $STATE/audit.log）"
    else
        ck 0 "C4 audit_v1.2 不可执行（$SRC/tools/audit_v1.2）"
    fi
}

# ── verify：重挂载 + 三重比对 ──
phase_verify() {
    need_dev || die "无目标盘"
    sudo mount -t ravenfs "$DEV" "$MNT" || die "重挂载失败"

    # C1 清单比对
    ( cd "$MNT" && find bmtest -type f ! -name 'fill.bin' | LC_ALL=C sort | \
        while read -r f; do printf '%s %s %s\n' "$f" "$(stat -c%s "$f")" "$(md5sum "$f" | cut -d' ' -f1)"; done ) \
        > "$STATE/manifest.remount"
    if cmp -s "$STATE/manifest.stress" "$STATE/manifest.remount"; then
        ck 1 "C1 重挂载清单全等（文件数=$(wc -l < "$STATE/manifest.stress")，大小+md5 逐项一致）"
    else
        ck 0 "C1 重挂载清单不等（diff 见下）"
        diff "$STATE/manifest.stress" "$STATE/manifest.remount" | head -10
    fi

    # C2 ★ 内存计数（stress 时） == 盘上重算（重挂载后）
    local fs freem freep
    fs=$(free_blocks); freep=$(cat "$STATE/free.preumount"); freem=$(cat "$STATE/free.stress")
    log "f_bfree: stress时=$freem 卸载前=$freep 重挂载=$fs"
    if [ "$fs" = "$freep" ] && [ "$freep" = "$freem" ]; then
        ck 1 "C2 statfs 计数三态全等（stress=卸载前=重挂载=$fs）"
    else
        ck 0 "C2 statfs 计数失配（stress=$freem 卸载前=$freep 重挂载=$fs）→ §10.6 计数保真违例"
    fi

    # C3 ENOSPC 往返（f0 应 == f1 == 重挂载值）
    read -r c_f0 c_fmin c_f1 < "$STATE/enospc.roundtrip"
    if [ "$c_f1" = "$c_f0" ] && [ "$c_f1" = "$fs" ]; then
        ck 1 "C3 ENOSPC 往返计数归零差（前=$c_f0 满=$c_fmin 后=$c_f1 重挂=$fs）"
    else
        ck 0 "C3 ENOSPC 往返计数不归位（前=$c_f0 后=$c_f1 重挂=$fs）→ 虚高/漏清"
    fi

    # C6 inode 往返：重挂载 used == churn 删半后
    read -r u1 u2 < "$STATE/inode.churn"
    local u3; u3=$(inode_used)
    if [ "$u3" = "$u2" ]; then
        ck 1 "C6 inode 计数往返（churn $u1→删半 $u2 → 重挂 $u3）"
    else
        ck 0 "C6 inode 计数失配（删半 $u2 vs 重挂 $u3）"
    fi

    # C5 dmesg 无错误
    if dmesg_s | tail -200 | grep -E "ravenfs.*(BUG|Oops|error|ERR|-EIO|bm_err=[1-9])" | grep -v "ttl:" | head -5 | grep -q .; then
        dmesg_s | tail -200 | grep -E "ravenfs.*(BUG|Oops|error|ERR|-EIO|bm_err=[1-9])" | grep -v "ttl:" | head -5
        ck 0 "C5 dmesg 出现 ravenfs 错误行"
    else
        ck 1 "C5 dmesg 无 ravenfs 错误（bm_err/wb_err 未见非零）"
    fi
    dmesg_s | grep -E "ravenfs: ttl" | tail -2 > "$STATE/ttl.final" || true

    sudo umount "$MNT" || log "verify 收尾 umount 失败（忽略）"
}

phase_unload() {
    grep -qw ravenfs /proc/filesystems && sudo rmmod ravenfs && log "rmmod ok" || true
}

phase_all() {
    local backing="${1:-1}" order="${2:-4}"
    mkdir -p "$STATE"
    phase_build
    phase_load "$backing" "$order"
    phase_mkfs
    phase_mount
    phase_stress
    phase_ttl
    phase_umount
    phase_audit
    phase_verify
    phase_unload
    echo "[BITMAP-ACCEPT] SUMMARY backing=$backing order=${2:-4}: PASS=$PASS FAIL=$FAIL"
    [ "$FAIL" = 0 ]
}

cmd="${1:-}"
case "$cmd" in
    build)  mkdir -p "$STATE"; phase_build ;;
    load)   shift; phase_load "$@" ;;
    mkfs)   phase_mkfs ;;
    mount)  phase_mount ;;
    stress) phase_stress ;;
    ttl)    phase_ttl ;;
    umount) phase_umount ;;
    audit)  phase_audit ;;
    verify) phase_verify ;;
    unload) phase_unload ;;
    all)    shift; phase_all "$@" ;;
    *)      echo "usage: $0 build|load <0|1> [order]|mkfs|mount|stress|ttl|umount|audit|verify|unload|all <0|1> [order]"; exit 64 ;;
esac
