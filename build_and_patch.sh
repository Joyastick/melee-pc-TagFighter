#!/usr/bin/env bash
# One-command rebuild: compiles the mod, repacks it into a bootable ISO next
# to a real Melee ISO, and (optionally) launches Dolphin with it.
#
# Usage:
#   ./build_and_patch.sh [/path/to/retail.iso] [--launch]
#
# If the ISO path is omitted, reuses the one baked in below.
set -euo pipefail
cd "$(dirname "$0")"

DEFAULT_ISO="/c/Users/aldri/Documents/Smash/NKit 1.4 + GameCube Partitions/NKit/Processed/GameCube/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"
DOLPHIN="/c/Users/aldri/Downloads/dolphin-2606a-x64/Dolphin-x64/Dolphin.exe"
PATCH_SCRIPT="$(dirname "$0")/tools/patch_iso.py"
OUT_ISO="tag_melee.iso"

ISO="$DEFAULT_ISO"
LAUNCH=0
for arg in "$@"; do
    case "$arg" in
        --launch) LAUNCH=1 ;;
        *) ISO="$arg" ;;
    esac
done

echo "== configuring (--non-matching) =="
python configure.py --non-matching

echo "== building =="
ninja

echo "== patching main.dol into a fresh ISO =="
rm -f "$OUT_ISO"
python3 "$PATCH_SCRIPT" "$ISO" build/GALE01/main.dol "$OUT_ISO"

echo "== done: $OUT_ISO =="

if [ "$LAUNCH" -eq 1 ]; then
    powershell -Command "Get-Process -Name Dolphin -ErrorAction SilentlyContinue | Stop-Process -Force" || true
    sleep 1
    "$DOLPHIN" -e "$OUT_ISO" &
    echo "Launched Dolphin with $OUT_ISO"
fi
