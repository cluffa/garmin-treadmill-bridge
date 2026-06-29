#!/usr/bin/env bash
# build.sh — build and optionally flash a board
#
# Usage:
#   ./build.sh xiao-c6              # build only
#   ./build.sh xiao-c6 flash        # build + flash (auto-detects port)
#   ./build.sh heltec-v3 flash /dev/cu.usbserial-0001
#
# Requires: ESP-IDF installed at ~/esp/esp-idf, ccache (brew install ccache)

set -e

BOARD="${1:?Usage: ./build.sh <board> [flash] [port]}"
ACTION="${2:-build}"
PORT="${3:-}"

IDF_PY=~/.espressif/python_env/idf5.5_py3.13_env/bin/python
RISCV=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin
XTENSA=~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin
export PATH="$XTENSA:$RISCV:$PATH"

BOARD_DIR="$(dirname "$0")/boards/$BOARD"
[ -d "$BOARD_DIR" ] || { echo "Unknown board: $BOARD"; exit 1; }

echo "==> Building $BOARD..."
"$IDF_PY" ~/esp/esp-idf/tools/idf.py --ccache -C "$BOARD_DIR" build

if [ "$ACTION" = "flash" ]; then
    # Auto-detect port if not given
    if [ -z "$PORT" ]; then
        PORT=$(ls /dev/cu.usbmodem* /dev/cu.SLAB_USB* /dev/cu.usbserial* 2>/dev/null | head -1)
        [ -n "$PORT" ] || { echo "No serial port found. Specify one: ./build.sh $BOARD flash /dev/cu.xxx"; exit 1; }
    fi

    # Release any process holding the port
    lsof "$PORT" 2>/dev/null | awk 'NR>1{print $2}' | xargs kill 2>/dev/null || true
    sleep 0.5

    echo "==> Flashing $BOARD to $PORT..."
    cd "$BOARD_DIR/build"
    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python \
        ~/esp/esp-idf/components/esptool_py/esptool/esptool.py \
        -p "$PORT" -b 460800 \
        --before default_reset --after hard_reset \
        write_flash "@flash_args"
fi
