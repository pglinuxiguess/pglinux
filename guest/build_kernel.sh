#!/bin/bash
# guest/build_kernel.sh — Build Linux kernel for RISC-V
#
# Produces: /workspace/vm/kernel.bin
#
# Requires: riscv64-linux-gnu-gcc, flex, bison, bc, libelf-dev
#
set -euo pipefail

KERNEL_VERSION="${KERNEL_VERSION:-6.1}"
BUILD_DIR="/build/linux-${KERNEL_VERSION}"
OUT="/workspace/vm/kernel.bin"

# Download only if not already present
if [ ! -d "$BUILD_DIR" ]; then
    echo "=== Downloading Linux ${KERNEL_VERSION} ==="
    cd /build
    wget -q "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz"
    tar xf "linux-${KERNEL_VERSION}.tar.xz"
    rm "linux-${KERNEL_VERSION}.tar.xz"
fi

echo "=== Building Linux ${KERNEL_VERSION} for RISC-V ==="
cd "$BUILD_DIR"

# Start from tinyconfig
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- tinyconfig

# Enable everything PostgreSQL + Debian needs
CONFIGS=(
    # Core
    64BIT SMP MMU MULTIUSER BINFMT_ELF BINFMT_SCRIPT FPU
    # IPC (PostgreSQL)
    SYSVIPC POSIX_MQUEUE FUTEX EPOLL SIGNALFD TIMERFD EVENTFD AIO
    ADVISE_SYSCALLS FHANDLE POSIX_TIMERS
    # Console
    TTY SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_EARLYCON SERIAL_OF_PLATFORM PRINTK
    # Device tree
    OF OF_EARLY_FLATTREE
    # VirtIO block device
    BLOCK VIRTIO_MENU VIRTIO VIRTIO_MMIO VIRTIO_BLK
    # Filesystems
    EXT2_FS EXT4_FS PROC_FS PROC_SYSCTL SYSFS TMPFS DEVTMPFS DEVTMPFS_MOUNT
    # Initrd (keep for fallback, but primary boot is from disk)
    BLK_DEV_INITRD RD_GZIP
    # Networking (unix sockets for PG)
    NET INET UNIX
    # RISC-V timer/IRQ
    RISCV_TIMER RISCV_SBI IRQCHIP RISCV_INTC SIFIVE_PLIC
    GENERIC_CLOCKEVENTS HIGH_RES_TIMERS RISCV_ISA_C
)

for cfg in "${CONFIGS[@]}"; do
    scripts/config --enable "$cfg"
done
scripts/config --set-val HZ 100

# Disable security/modules (not needed)
scripts/config --disable SECURITY
scripts/config --disable AUDIT
scripts/config --disable COMPAT
scripts/config --disable MODULES

# Resolve any missing dependencies
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig

echo "--- Config verification ---"
grep 'CONFIG_MULTIUSER\|CONFIG_VIRTIO_BLK\|CONFIG_EXT4\|CONFIG_DEVTMPFS\|CONFIG_BLOCK' .config | head -10

# Build
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j$(nproc) Image
cp arch/riscv/boot/Image "$OUT"

echo "=== Kernel built ==="
ls -la "$OUT"
