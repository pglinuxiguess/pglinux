#!/bin/bash
# guest/build_postgres.sh — Cross-compile PostgreSQL 18.1 for RISC-V
#
# Produces: /opt/pgsql-riscv64/{bin,share,lib}
#
# This build uses glibc dynamic linking (the cross-toolchain default).
# The Debian rootfs provides glibc shared libs at runtime.
#
# Patches applied:
#   - pg_stack_depth.patch: hardcode stack depth (our kernel returns 0 for RLIMIT_STACK)
#   - pg_initdb_signal_debug.patch: extra debugging for initdb signal handling
#   - pg_builtin_ext.c: static extension registry for plpgsql
#   - pg_dfmgr_builtin.sh: patches dfmgr.c to use the builtin registry
#   - pg_strdup_diag.patch: diagnostic for NULL pg_strdup calls
#   - functioncmds.c NaN fix: workaround for emulator FPU prorows bug
#   - guc_tables.c: adjust max_stack_depth default
#
set -euo pipefail

PG_VERSION="${PG_VERSION:-18.1}"
BUILD_DIR="/build/postgresql-${PG_VERSION}"
INSTALL_PREFIX="/opt/pgsql-riscv64"
WORKSPACE="/workspace"

CC="riscv64-linux-gnu-gcc"

# ---- Step 1: Download PG source ----
if [ ! -d "$BUILD_DIR" ]; then
    echo "=== Downloading PostgreSQL ${PG_VERSION} ==="
    cd /build
    wget -q "https://ftp.postgresql.org/pub/source/v${PG_VERSION}/postgresql-${PG_VERSION}.tar.bz2"
    tar xf "postgresql-${PG_VERSION}.tar.bz2"
    rm "postgresql-${PG_VERSION}.tar.bz2"
fi

# ---- Step 2: Configure ----
echo "=== Configuring PostgreSQL ==="
cd "$BUILD_DIR"

# Clean if previously configured
[ -f src/Makefile.global ] && make distclean 2>/dev/null || true

$CC --version | head -1

./configure \
    --host=riscv64-linux-gnu \
    --build=$(uname -m)-linux-gnu \
    --prefix="$INSTALL_PREFIX" \
    --without-readline \
    --without-zlib \
    --without-icu \
    --without-openssl \
    --without-libxml \
    --without-libxslt \
    --without-gssapi \
    --without-ldap \
    --without-pam \
    --without-systemd \
    --without-perl \
    --without-python \
    --without-tcl \
    --disable-thread-safety \
    CC="$CC" \
    CFLAGS="-Os" \
    LDFLAGS_EX="-static"

echo "=== Configure done ==="

# ---- Step 3: Apply patches ----
echo "=== Applying patches ==="

patch -p1 < "$WORKSPACE/guest/pg_stack_depth.patch"
patch -p1 < "$WORKSPACE/guest/pg_initdb_signal_debug.patch"
patch -p1 < "$WORKSPACE/guest/pg_strdup_diag.patch" || echo "WARN: pg_strdup_diag.patch failed (may already be applied)"

# initdb verbose logging
sed -i 's/exit_on_error=true -c log_checkpoints=false/exit_on_error=true -c log_checkpoints=false -c log_error_verbosity=verbose/' \
    src/bin/initdb/initdb.c

# FPU NaN workaround (prorows)
sed -i "s/if (prorows < 0)/if (isnan(prorows)) prorows = -1; if (prorows < 0)/" \
    src/backend/commands/functioncmds.c

# Stack depth GUC default
sed -i 's/100, 100, MAX_KILOBYTES,/2048, 0, MAX_KILOBYTES,/' \
    src/backend/utils/misc/guc_tables.c

# Builtin extension registry
cp "$WORKSPACE/guest/pg_builtin_ext.c" src/backend/utils/fmgr/pg_builtin_ext.c
sh "$WORKSPACE/guest/pg_dfmgr_builtin.sh" "$BUILD_DIR"

echo "=== Patches applied ==="

# ---- Step 4: Build ----
echo "=== Building PostgreSQL ==="

# Build everything — with dynamic linking, .so builds should work
make -j$(nproc) -k 2>&1 | tail -20
echo "=== make pass done ==="

# Verify plpgsql objects exist
ls src/pl/plpgsql/src/*.o && echo "plpgsql objects OK" || echo "WARN: plpgsql objects missing"

# ---- Step 5: Re-link postgres with plpgsql built-in ----
echo "=== Re-linking postgres with builtin plpgsql ==="

# Compile the builtin extension registry
$CC -c -Os -I src/include \
    src/backend/utils/fmgr/pg_builtin_ext.c \
    -o src/backend/utils/fmgr/pg_builtin_ext.o

echo "src/backend/utils/fmgr/pg_builtin_ext.o" >> src/backend/utils/fmgr/objfiles.txt

# Add plpgsql objects as LOCALOBJS
if ! grep -q 'pl_comp.o' src/backend/Makefile; then
    sed -i '/^OBJS = /i LOCALOBJS += ../pl/plpgsql/src/pl_comp.o ../pl/plpgsql/src/pl_exec.o ../pl/plpgsql/src/pl_funcs.o ../pl/plpgsql/src/pl_gram.o ../pl/plpgsql/src/pl_handler.o ../pl/plpgsql/src/pl_scanner.o' \
        src/backend/Makefile
fi

rm -f src/backend/postgres
make -C src/backend postgres 2>&1 | tail -10
file src/backend/postgres
echo "=== postgres re-linked ==="

# ---- Step 6: Install ----
echo "=== Installing ==="
make install -k 2>&1 | tail -10
riscv64-linux-gnu-strip "$INSTALL_PREFIX/bin/"* 2>/dev/null || true

# Ensure postgresql.conf.sample exists
[ -f "$INSTALL_PREFIX/share/postgresql.conf.sample" ] || \
    cp src/backend/utils/misc/postgresql.conf.sample "$INSTALL_PREFIX/share/" 2>/dev/null || \
    echo "# Minimal postgresql.conf.sample for initdb" > "$INSTALL_PREFIX/share/postgresql.conf.sample"

echo ""
echo "=== PostgreSQL ${PG_VERSION} installed ==="
ls -la "$INSTALL_PREFIX/bin/"
file "$INSTALL_PREFIX/bin/postgres"
file "$INSTALL_PREFIX/bin/initdb"
