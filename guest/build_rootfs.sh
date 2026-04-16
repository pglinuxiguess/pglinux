#!/bin/bash
# guest/build_rootfs.sh — Create a Debian riscv64 rootfs disk image
#
# NO mount required — uses debootstrap --foreign into a directory,
# then mke2fs -d to package it as ext4. Works in unprivileged containers.
#
# Produces: /workspace/vm/rootfs.img
#
set -euo pipefail

ROOTFS_SIZE_MB=512
ROOTFS_IMG="/workspace/vm/rootfs.img"
ROOTFS_DIR="/tmp/rootfs"
PG_INSTALL="/opt/pgsql-riscv64"

echo "=== Building Debian riscv64 rootfs (${ROOTFS_SIZE_MB}MB) ==="

# Clean previous
rm -rf "$ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR"

# ---- Step 1: Debootstrap (foreign = no chroot needed) ----
echo "[1/5] Running debootstrap --foreign (riscv64)..."
debootstrap \
    --arch=riscv64 \
    --foreign \
    --variant=minbase \
    --include=dash,coreutils,sed,grep,gawk,findutils,procps,hostname \
    sid \
    "$ROOTFS_DIR" \
    http://deb.debian.org/debian

# Second stage completes the package configuration.
# With qemu-user-static registered via binfmt_misc, chroot to riscv64 works.
echo "[1/5] Running debootstrap second stage..."
if [ -f /usr/bin/qemu-riscv64-static ]; then
    cp /usr/bin/qemu-riscv64-static "$ROOTFS_DIR/usr/bin/"
    chroot "$ROOTFS_DIR" /debootstrap/debootstrap --second-stage 2>&1 | tail -5
    rm -f "$ROOTFS_DIR/usr/bin/qemu-riscv64-static"
    echo "  Second stage complete ✓"
else
    echo "  WARN: qemu-user-static not available, skipping second stage"
    echo "  (rootfs will have unpacked but unconfigured packages)"
fi

# ---- Step 2: Configure ----
echo "[2/5] Configuring rootfs..."

# /etc basics
echo "linuxsql-vm" > "$ROOTFS_DIR/etc/hostname"
cat > "$ROOTFS_DIR/etc/hosts" << 'EOF'
127.0.0.1 localhost linuxsql-vm
EOF
echo "nameserver 10.0.2.3" > "$ROOTFS_DIR/etc/resolv.conf"

# Users
if [ -f "$ROOTFS_DIR/etc/passwd" ]; then
    grep -q '^postgres:' "$ROOTFS_DIR/etc/passwd" || \
        echo "postgres:x:70:70:PostgreSQL:/data/pgdata:/bin/dash" >> "$ROOTFS_DIR/etc/passwd"
    grep -q '^postgres:' "$ROOTFS_DIR/etc/group" || \
        echo "postgres:x:70:" >> "$ROOTFS_DIR/etc/group"
else
    echo "root:x:0:0:root:/root:/bin/dash" > "$ROOTFS_DIR/etc/passwd"
    echo "root:x:0:" > "$ROOTFS_DIR/etc/group"
    echo "postgres:x:70:70:PostgreSQL:/data/pgdata:/bin/dash" >> "$ROOTFS_DIR/etc/passwd"
    echo "postgres:x:70:" >> "$ROOTFS_DIR/etc/group"
fi

# Shell profile for vm_exec prompt detection
mkdir -p "$ROOTFS_DIR/etc/profile.d"
cat > "$ROOTFS_DIR/etc/profile.d/linuxsql.sh" << 'EOF'
export PS1="linuxsql# "
export PATH=/usr/local/pgsql/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export PAGER=cat
export LANG=C
EOF

# ---- Step 3: Install PostgreSQL ----
echo "[3/5] Installing PostgreSQL..."
if [ -d "$PG_INSTALL" ]; then
    mkdir -p "$ROOTFS_DIR/usr/local/pgsql"
    cp -a "$PG_INSTALL"/* "$ROOTFS_DIR/usr/local/pgsql/"
    echo "  PG installed from $PG_INSTALL"

    # Install the cross-compiled extension
    if [ -f "/workspace/extension/linuxsql_vm.so" ]; then
        mkdir -p "$ROOTFS_DIR/usr/local/pgsql/lib"
        mkdir -p "$ROOTFS_DIR/usr/local/pgsql/share/extension"
        cp "/workspace/extension/linuxsql_vm.so" "$ROOTFS_DIR/usr/local/pgsql/lib/"
        cp "/workspace/extension/linuxsql_vm.control" "$ROOTFS_DIR/usr/local/pgsql/share/extension/"
        cp "/workspace/extension/linuxsql_vm--1.0.sql" "$ROOTFS_DIR/usr/local/pgsql/share/extension/"
        echo "  linuxsql_vm natively installed into guest PostgreSQL"
    else
        echo "  WARN: linuxsql_vm.so not found!"
    fi

    # Copy binary assets into the guest so L3 can boot!
    mkdir -p "$ROOTFS_DIR/vm"
    cp "/workspace/vm/fw_jump.bin" "$ROOTFS_DIR/vm/" || true
    cp "/workspace/vm/kernel.bin" "$ROOTFS_DIR/vm/" || true
    cp "/workspace/vm/linuxsql.dtb" "$ROOTFS_DIR/vm/" || true
    cp "/workspace/vm/initramfs.cpio.gz" "$ROOTFS_DIR/vm/" || true
    # Note: we do NOT copy rootfs.img into itself to avoid recursion bloat.
    # L2 will use /dev/vda as the rootfs asset.
else
    echo "  WARN: $PG_INSTALL not found — run build_postgres.sh first"
fi

# PG data directories
mkdir -p "$ROOTFS_DIR/data/pgdata"
mkdir -p "$ROOTFS_DIR/data/pgrun"
mkdir -p "$ROOTFS_DIR/data/pglog"
chown -R 70:70 "$ROOTFS_DIR/data" 2>/dev/null || true

# ---- Step 4: Install init binary ----
echo "[4/5] Installing init..."
if [ -f "/workspace/vm/init" ]; then
    cp /workspace/vm/init "$ROOTFS_DIR/init"
    chmod 755 "$ROOTFS_DIR/init"
    echo "  /init installed"
else
    echo "  WARN: /workspace/vm/init not found — run build_init.sh first"
fi

# Ensure glibc shared libs from cross-toolchain are present
echo "[4/5] Checking glibc..."
if [ -f "$ROOTFS_DIR/lib/riscv64-linux-gnu/libc.so.6" ] || \
   [ -f "$ROOTFS_DIR/lib/libc.so.6" ]; then
    echo "  glibc found ✓"
else
    echo "  Copying glibc from cross-toolchain..."
    LIB_SRC="/usr/riscv64-linux-gnu/lib"
    LIB_DST="$ROOTFS_DIR/lib/riscv64-linux-gnu"
    mkdir -p "$LIB_DST" "$ROOTFS_DIR/lib"
    # Dynamic linker must be at the expected path
    cp -a "$LIB_SRC/ld-linux-riscv64-lp64d.so.1" "$ROOTFS_DIR/lib/" 2>/dev/null || true
    for lib in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1; do
        cp -a "$LIB_SRC/$lib" "$LIB_DST/" 2>/dev/null || true
    done
fi

# ---- Step 5: Package as ext4 image ----
echo "[5/5] Creating ext4 image..."

# Count inodes needed (files + dirs + margin)
INODE_COUNT=$(find "$ROOTFS_DIR" | wc -l)
INODE_COUNT=$((INODE_COUNT + 1000))  # margin

mke2fs -t ext4 -L rootfs -d "$ROOTFS_DIR" \
    -N "$INODE_COUNT" \
    -r 1 -b 4096 \
    "$ROOTFS_IMG" "${ROOTFS_SIZE_MB}M"

echo ""
echo "=== Rootfs build complete ==="
echo "  Image: $ROOTFS_IMG ($(du -h "$ROOTFS_IMG" | cut -f1))"
echo "  Files: $(find "$ROOTFS_DIR" -type f | wc -l)"
echo "  Dirs:  $(find "$ROOTFS_DIR" -type d | wc -l)"
ls -la "$ROOTFS_DIR/bin/dash" 2>/dev/null && echo "  /bin/dash: ✓" || echo "  /bin/dash: MISSING"
ls -la "$ROOTFS_DIR/bin/sh" 2>/dev/null && echo "  /bin/sh: ✓" || echo "  /bin/sh: MISSING"
ls -la "$ROOTFS_DIR/init" 2>/dev/null && echo "  /init: ✓" || echo "  /init: MISSING"

# Cleanup
rm -rf "$ROOTFS_DIR"
echo "Done: $ROOTFS_IMG"
