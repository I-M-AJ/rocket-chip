#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Run Rocket Chip RV64GC Linux (`fw_payload.elf`) on Spike or Verilato
# Usage:
#   ./linux_boot/run_linux_sim.sh spike       # ~1 second ISA functional check
#   ./linux_boot/run_linux_sim.sh verilator   # Cycle-accurate RTL simulation
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PAYLOAD="${PAYLOAD:-$SCRIPT_DIR/build/fw_payload.elf}"
CONFIG="${CONFIG:-freechips.rocketchip.system.DefaultConfig}"
MODE="${1:-verilator}"

if [ ! -f "$PAYLOAD" ]; then
    echo "ERROR: $PAYLOAD not found. Run ./linux_boot/build_linux_payload.sh first."
    exit 1
fi

if [ "$MODE" = "spike" ]; then
    echo "=== Running $PAYLOAD in Spike (RV64GC functional simulator) ==="
    if command -v spike >/dev/null 2>&1; then
        SPIKE_BIN="spike"
    else
        SPIKE_BIN="$(find /nix/store -maxdepth 3 -path '*-spike-*/bin/spike' 2>/dev/null | head -n 1)"
    fi
    if [ -z "${SPIKE_BIN:-}" ]; then
        echo "ERROR: spike not found. Run inside 'nix develop'."
        exit 1
    fi
    exec "$SPIKE_BIN" --isa=rv64imafdc_zicsr_zifencei -m0x0c000000:0x04000000,0x80000000:0x10000000 "$PAYLOAD"

elif [ "$MODE" = "verilator" ]; then
    EMULATOR="$REPO_ROOT/out/emulator/freechips.rocketchip.system.TestHarness/${CONFIG}/verilator/elf.dest/emulator"
    if [ ! -x "$EMULATOR" ]; then
        echo "Emulator binary not found at $EMULATOR — building via 'make emulator CONFIG=$CONFIG'..."
        cd "$REPO_ROOT"
        nix --experimental-features 'nix-command flakes' develop -c make emulator CONFIG="$CONFIG"
    fi
    echo "=== Running $PAYLOAD on Cycle-Accurate Verilator Rocket Chip ($CONFIG) ==="
    exec "$EMULATOR" -c "$PAYLOAD"

else
    echo "Usage: $0 [spike|verilator]"
    exit 1
fi
