#!/bin/bash
# guest/build_all.sh — Orchestrate the full RISC-V guest build
#
# This runs on the HOST (macOS). It manages a persistent Docker
# container and runs individual build scripts inside it.
#
# Usage:
#   ./guest/build_all.sh          # full build
#   ./guest/build_all.sh init     # rebuild only init.c
#   ./guest/build_all.sh rootfs   # rebuild only rootfs image
#   ./guest/build_all.sh shell    # interactive shell in builder
#
set -euo pipefail

DOCKER="/usr/local/bin/docker"
CONTAINER="linuxsql-builder"
IMAGE="debian:sid"
WORKSPACE="$(cd "$(dirname "$0")/.." && pwd)"

# ---- Ensure Docker is running ----
if ! $DOCKER info >/dev/null 2>&1; then
    echo "Docker is not running. Start Docker Desktop and try again."
    exit 1
fi

# ---- Create or reuse persistent builder container ----
if ! $DOCKER ps -a --format '{{.Names}}' | grep -q "^${CONTAINER}$"; then
    echo "=== Creating builder container ==="
    $DOCKER run -d \
        --name "$CONTAINER" \
        --platform linux/amd64 \
        -v "$WORKSPACE:/workspace" \
        "$IMAGE" \
        sleep infinity

    # One-time setup: install cross-compilation toolchain
    echo "=== Installing toolchain (one-time) ==="
    $DOCKER exec "$CONTAINER" bash -c '
        apt-get update -qq && apt-get install -y --no-install-recommends \
            build-essential \
            gcc-riscv64-linux-gnu \
            g++-riscv64-linux-gnu \
            wget ca-certificates \
            bison flex bc libelf-dev \
            e2fsprogs \
            debootstrap qemu-user-static \
            file cpio \
        && rm -rf /var/lib/apt/lists/*
        mkdir -p /build /out
    '
    echo "=== Toolchain installed ==="
elif ! $DOCKER ps --format '{{.Names}}' | grep -q "^${CONTAINER}$"; then
    echo "=== Starting stopped builder container ==="
    $DOCKER start "$CONTAINER"
fi

run_in_builder() {
    $DOCKER exec "$CONTAINER" bash -c "$1"
}

# ---- Handle subcommands ----
CMD="${1:-all}"

case "$CMD" in
    shell)
        echo "=== Interactive shell in builder ==="
        $DOCKER exec -it "$CONTAINER" bash
        exit 0
        ;;
    musl)
        echo "=== Building musl ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_musl.sh
        ;;
    init)
        echo "=== Building init.c ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_init.sh
        ;;
    kernel)
        echo "=== Building kernel ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_kernel.sh
        ;;
    postgres|pg)
        echo "=== Building PostgreSQL ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_postgres.sh
        ;;
    rootfs)
        echo "=== Building rootfs ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_rootfs.sh
        ;;
    extension|ext)
        echo "=== Building Extension ==="
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_extension.sh
        ;;
    all)
        echo "=== Full build ==="
        echo ""
        echo "[1/6] musl toolchain..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_musl.sh
        echo ""
        echo "[2/6] init.c..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_init.sh
        echo ""
        echo "[3/6] kernel..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_kernel.sh
        echo ""
        echo "[4/6] PostgreSQL..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_postgres.sh
        echo ""
        echo "[5/6] Extension..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_extension.sh
        echo ""
        echo "[6/6] rootfs..."
        $DOCKER exec "$CONTAINER" bash /workspace/guest/build_rootfs.sh
        echo ""
        echo "=== Full build complete ==="
        echo "Artifacts:"
        ls -lh "$WORKSPACE/vm/kernel.bin" "$WORKSPACE/vm/rootfs.img" "$WORKSPACE/vm/init" 2>/dev/null || true
        ;;
    *)
        echo "Usage: $0 [all|shell|musl|init|kernel|postgres|extension|rootfs]"
        exit 1
        ;;
esac
