#!/bin/bash
# guest/build_musl.sh — Build musl libc cross-toolchain for RISC-V
#
# One-time setup. Produces:
#   /opt/musl-riscv64/           — musl sysroot (headers + static lib)
#   /usr/local/bin/riscv64-musl-gcc — wrapper script
#
# Requires: riscv64-linux-gnu-gcc (apt: gcc-riscv64-linux-gnu)
#
set -euo pipefail

MUSL_VERSION="${MUSL_VERSION:-1.2.5}"
PREFIX="/opt/musl-riscv64"
CROSS="riscv64-linux-gnu-"

if [ -f "$PREFIX/lib/libc.a" ] && [ -f "/usr/local/bin/riscv64-musl-gcc" ]; then
    echo "musl already built at $PREFIX — skipping"
    exit 0
fi

echo "=== Building musl ${MUSL_VERSION} for riscv64 ==="

cd /tmp
wget -q "https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
tar xf "musl-${MUSL_VERSION}.tar.gz"
cd "musl-${MUSL_VERSION}"

./configure \
    --host=riscv64-linux-gnu \
    --prefix="$PREFIX" \
    --disable-shared \
    CROSS_COMPILE="${CROSS}" \
    CC="${CROSS}gcc"
make -j$(nproc)
make install

cd /tmp && rm -rf "musl-${MUSL_VERSION}" "musl-${MUSL_VERSION}.tar.gz"

# Create wrapper script
cat > /usr/local/bin/riscv64-musl-gcc << 'WRAPPER'
#!/bin/sh
exec riscv64-linux-gnu-gcc \
    -specs /opt/musl-riscv64/lib/musl-gcc.specs \
    -nostdinc \
    -isystem /opt/musl-riscv64/include \
    "$@"
WRAPPER
chmod +x /usr/local/bin/riscv64-musl-gcc

echo "=== musl installed at $PREFIX ==="
ls -la "$PREFIX/lib/libc.a"
