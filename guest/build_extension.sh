#!/bin/bash
# guest/build_extension.sh — Cross-compile the linuxsql_vm extension for RISC-V

set -euo pipefail

PG_VERSION="18.1"
INSTALL_PREFIX="/opt/pgsql-riscv64"
WORKSPACE="/workspace"

CC="riscv64-linux-gnu-gcc"

echo "=== Building linuxsql_vm Extension for guest ==="
cd "$WORKSPACE/extension"

# Build strictly against the cross-compiled RISC-V PostgreSQL installation
export PG_CONFIG="$INSTALL_PREFIX/bin/pg_config"

# We must bypass the automated MacOS specific Makefile logic, so we compile it manually here
$CC -Wall -O2 -fPIC -shared \
    -I$INSTALL_PREFIX/include/server \
    -DSLJIT_CONFIG_AUTO=1 \
    -o linuxsql_vm.so \
    rv64_pgext.c rv64_cpu.c rv64_mmu.c rv64_fpu.c rv64_bus.c rv64_virtio.c rv64_virtio_net.c rv64_sljit.c sljit_src/sljitLir.c \
    -lm

echo "=== Extension built: linuxsql_vm.so ==="
ls -l linuxsql_vm.so
