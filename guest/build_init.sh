#!/bin/bash
# guest/build_init.sh — Compile init.c (PID 1) with musl for RISC-V
#
# Produces: /workspace/vm/init — static RISC-V binary
#
# Requires: musl cross-toolchain at /opt/musl-riscv64
#           (run build_musl.sh first if missing)
#
set -euo pipefail

MUSL_PREFIX="/opt/musl-riscv64"
MUSL_GCC="/usr/local/bin/riscv64-musl-gcc"
SRC="/workspace/guest/init.c"
OUT="/workspace/vm/init"

if [ ! -f "$MUSL_GCC" ]; then
    echo "ERROR: musl-gcc wrapper not found at $MUSL_GCC"
    echo "       Run build_musl.sh first"
    exit 1
fi

echo "=== Compiling init.c (musl static) ==="
$MUSL_GCC -static -Os -o "$OUT" "$SRC"
riscv64-linux-gnu-strip "$OUT"
file "$OUT"
ls -la "$OUT"
echo "Done: $OUT"
