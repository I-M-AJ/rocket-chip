#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Build & Run Bare-Metal Custom Anomaly IP Test on Verilator AnomalySoCConfig
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG="${CONFIG:-freechips.rocketchip.system.AnomalySoCConfig}"
EMULATOR="$REPO_ROOT/out/emulator/freechips.rocketchip.system.TestHarness/${CONFIG}/verilator/elf.dest/emulator"
DTS_FILE="$REPO_ROOT/out/emulator/freechips.rocketchip.system.TestHarness/${CONFIG}/generator/elaborate.dest/${CONFIG}.dts"

echo "=== [1/3] Compiling RV64 bare-metal test (custom_ip_test.elf) ==="
if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    CROSS_PREFIX="riscv64-unknown-elf-"
elif command -v riscv64-none-elf-gcc >/dev/null 2>&1; then
    CROSS_PREFIX="riscv64-none-elf-"
elif command -v riscv64-unknown-linux-gnu-gcc >/dev/null 2>&1; then
    CROSS_PREFIX="riscv64-unknown-linux-gnu-"
else
    echo "ERROR: No riscv64 GCC found in PATH."
    exit 1
fi
make -C "$SCRIPT_DIR" CROSS_COMPILE="$CROSS_PREFIX" all

if [ ! -x "$EMULATOR" ]; then
    echo "ERROR: Emulator binary not found at:"
    echo "  $EMULATOR"
    echo "Build it first inside 'nix develop' with:"
    echo "  make emulator CONFIG=$CONFIG"
    exit 1
fi

if [ -f "$DTS_FILE" ]; then
    echo ""
    echo "=== [2/3] Checking generated Device Tree node for custom-anomaly-ip ==="
    grep -A 7 "custom-anomaly-ip" "$DTS_FILE" || true
fi

echo ""
echo "=== [3/3] Running custom_ip_test.elf on Verilator ($CONFIG) ==="
"$EMULATOR" -c "$SCRIPT_DIR/custom_ip_test.elf"
