#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Build Minimal RV64GC Linux + OpenSBI Single-ELF Payload (`fw_payload.elf`)
# for Rocket Chip `DefaultConfig` (Spike & Verilator `emulator`)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="${WORK_DIR:-$HOME/rocket_linux_build}"
OUT_DIR="$SCRIPT_DIR/build"

# Load Nix profile if available
if [ -f /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh ]; then
    # shellcheck disable=SC1091
    source /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
fi
export PATH="$HOME/.nix-profile/bin:$PATH"

CROSS="${CROSS_COMPILE:-riscv64-unknown-linux-gnu-}"
if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
    echo "ERROR: ${CROSS}gcc not found on PATH."
    echo "Install via: nix profile add nixpkgs#pkgsCross.riscv64.buildPackages.gcc"
    exit 1
fi

mkdir -p "$WORK_DIR" "$OUT_DIR"

echo "=== [1/5] Cloning OpenSBI v1.5 & Linux v6.6 (if not cached in $WORK_DIR) ==="
if [ ! -d "$WORK_DIR/opensbi" ]; then
    git clone --depth 1 --branch v1.5 https://github.com/riscv-software-src/opensbi.git "$WORK_DIR/opensbi"
fi
if [ ! -d "$WORK_DIR/linux" ]; then
    git clone --depth 1 --branch v6.6 https://github.com/torvalds/linux.git "$WORK_DIR/linux"
fi

echo "=== [2/5] Compiling Device Tree (rocket_linux.dtb) ==="
dtc -I dts -O dtb -o "$WORK_DIR/rocket_linux.dtb" "$SCRIPT_DIR/rocket_linux.dts"
cp -f "$WORK_DIR/rocket_linux.dtb" "$OUT_DIR/rocket_linux.dtb"

echo "=== [3/5] Building freestanding PID-1 /init via Linux nolibc ==="
"${CROSS}gcc" -Os -fno-ident -fno-asynchronous-unwind-tables \
    -nostdlib -static \
    -include "$WORK_DIR/linux/tools/include/nolibc/nolibc.h" \
    -o "$WORK_DIR/init" "$SCRIPT_DIR/init.c" -lgcc
"${CROSS}strip" "$WORK_DIR/init"
ls -lh "$WORK_DIR/init"

cat > "$WORK_DIR/initramfs_list.txt" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
dir /proc 0755 0 0
dir /sys 0755 0 0
file /init $WORK_DIR/init 0755 0 0
EOF

echo "=== [4/5] Configuring & Compiling Minimal RV64GC Sv39 Linux Kernel ==="
cd "$WORK_DIR/linux"
if [ ! -f "$WORK_DIR/linux/arch/riscv/boot/Image" ] || [ "${REBUILD_KERNEL:-0}" = "1" ]; then
make ARCH=riscv CROSS_COMPILE="$CROSS" tinyconfig

./scripts/config \
    --disable WERROR \
    --enable 64BIT \
    --enable MMU \
    --enable RISCV_ISA_C \
    --enable FPU \
    --enable RISCV_SBI \
    --enable RISCV_SBI_V01 \
    --enable SERIAL_EARLYCON \
    --enable SERIAL_EARLYCON_RISCV_SBI \
    --enable TTY \
    --enable HVC_DRIVER \
    --enable HVC_RISCV_SBI \
    --enable PRINTK \
    --enable EARLY_PRINTK \
    --enable COMMON_CLK \
    --enable TIMER_OF \
    --enable RISCV_TIMER \
    --enable CLINT_TIMER \
    --enable SIFIVE_PLIC \
    --enable BLK_DEV_INITRD \
    --enable INITRAMFS_COMPRESSION_NONE \
    --set-str INITRAMFS_SOURCE "$WORK_DIR/initramfs_list.txt" \
    --enable BINFMT_ELF \
    --enable BINFMT_SCRIPT \
    --enable PROC_FS \
    --enable SYSFS \
    --enable DEVTMPFS \
    --enable TMPFS \
    --enable FUTEX \
    --enable MULTIUSER \
    --enable UIO \
    --enable UIO_PDRV_GENIRQ \
    --disable RISCV_MISALIGNED \
    --disable RISCV_PROBE_UNALIGNED_ACCESS

make ARCH=riscv CROSS_COMPILE="$CROSS" olddefconfig
echo "--- Active Key Kernel Configs ---"
grep -E "CONFIG_(64BIT|MMU|RISCV_ISA_C|FPU|RISCV_SBI|HVC_RISCV_SBI|SERIAL_EARLYCON_RISCV_SBI|RISCV_TIMER|SIFIVE_PLIC|BLK_DEV_INITRD|INITRAMFS_SOURCE|BINFMT_ELF|PROC_FS)=" .config || true

make ARCH=riscv CROSS_COMPILE="$CROSS" KCFLAGS="-Wno-error" -j"$(nproc)" Image
fi
ls -lh "$WORK_DIR/linux/arch/riscv/boot/Image"

echo "=== [5/5] Building OpenSBI fw_payload.elf (OpenSBI + DTB + Linux + Initramfs) ==="
cd "$WORK_DIR/opensbi"
make PLATFORM=generic CROSS_COMPILE="$CROSS" clean
make PLATFORM=generic \
     CROSS_COMPILE="$CROSS" \
     CC="${CROSS}gcc -std=gnu11 -Wno-error" \
     FW_TEXT_START=0x80000000 \
     FW_PAYLOAD=y \
     FW_PAYLOAD_OFFSET=0x200000 \
     FW_PAYLOAD_PATH="$WORK_DIR/linux/arch/riscv/boot/Image" \
     FW_FDT_PATH="$WORK_DIR/rocket_linux.dtb" \
     -j"$(nproc)"

cp -f "$WORK_DIR/opensbi/build/platform/generic/firmware/fw_payload.elf" "$OUT_DIR/fw_payload.elf"
echo "=== SUCCESS: Built $OUT_DIR/fw_payload.elf ==="
ls -lh "$OUT_DIR/fw_payload.elf"
"${CROSS}nm" "$OUT_DIR/fw_payload.elf" | grep -E "tohost|fromhost"
