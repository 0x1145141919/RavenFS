#!/bin/bash
set -euo pipefail

BUILD_DIR="$(cd "$(dirname "$0")/../build" && pwd)"
MKFS="${BUILD_DIR}/mkfs/mkfs.ravenfs"
STRESS_REV="${BUILD_DIR}/tests/ravenfs_stress_rev"
STRESS_DELETE="${BUILD_DIR}/tests/ravenfs_stress_delete"
REPLAY="${BUILD_DIR}/tests/ravenfs_replay_delete"

UUID="11111111-2222-3333-4444-555555555555"
MODE="full"
ORDER_FILE=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] <device>

三板斧全自动管线：
  full:   mkfs → stress_mkdir_rev → stress_delete (乱序)
  replay: mkfs → stress_mkdir_rev → replay_delete (复盘)

Options:
  --uuid <uuid>         UUID for mkfs (default: $UUID)
  --replay <order.txt>  回放模式，用此前保存的 order file 复盘
  -h, --help            Show this help
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uuid)     UUID="$2"; shift 2 ;;
        --replay)   MODE="replay"; ORDER_FILE="$2"; shift 2 ;;
        -h|--help)  usage ;;
        -*)
            echo "Unknown option: $1"
            usage
            ;;
        *)
            if [[ -z "${DEVICE:-}" ]]; then
                DEVICE="$1"
            else
                echo "Unexpected argument: $1"
                usage
            fi
            shift
            ;;
    esac
done

if [[ -z "${DEVICE:-}" ]]; then
    echo "Error: <device> is required"
    usage
fi

for bin in "$MKFS" "$STRESS_REV" "$STRESS_DELETE" "$REPLAY"; do
    if [[ ! -x "$bin" ]]; then
        echo "Error: not found or not executable: $bin"
        echo "       Run 'cmake --build build' first."
        exit 1
    fi
done

if [[ "$MODE" == "replay" && -z "$ORDER_FILE" ]]; then
    echo "Error: --replay requires an order file path"
    usage
fi

run_phase0() {
    echo "═══════════════════════════════════════════════"
    echo "  Phase 0: mkfs.ravenfs --uuid=$UUID"
    echo "═══════════════════════════════════════════════"
    "$MKFS" --uuid="$UUID" -f "$DEVICE"
}

run_phase1() {
    echo "═══════════════════════════════════════════════"
    echo "  Phase 1: stress_mkdir_rev (65536 dirs)"
    echo "═══════════════════════════════════════════════"
    "$STRESS_REV" "$DEVICE"
}


if [[ "$MODE" == "full" ]]; then
    run_phase0
    run_phase1

    echo "═══════════════════════════════════════════════"
    echo "  Phase 2: stress_delete (乱序 + 保存 order)"
    echo "═══════════════════════════════════════════════"
    set +e
    "$STRESS_DELETE" "$DEVICE"
    RC=$?
    set -e

    if [[ $RC -eq 0 ]]; then
        echo ""
        echo "  ✓ 全部通过"
        exit 0
    else
        echo ""
        echo "  ✗ 失败 (exit=$RC)"
        echo ""
        echo "  复盘命令:"
        echo "    $(basename "$0") --replay delete_order.txt $DEVICE"
        exit $RC
    fi

elif [[ "$MODE" == "replay" ]]; then
    run_phase0
    run_phase1

    echo "═══════════════════════════════════════════════"
    echo "  Phase 2: replay_delete ($ORDER_FILE)"
    echo "═══════════════════════════════════════════════"
    "$REPLAY" "$DEVICE" "$ORDER_FILE"
fi
