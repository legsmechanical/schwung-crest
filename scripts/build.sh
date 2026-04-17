#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="crest-builder"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
MODULE_ID="crest"

cd "$REPO_ROOT"

# ── Inside Docker (second-pass) or no Docker: compile directly ──────────────
if [ "${IN_DOCKER:-0}" = "1" ] || ! command -v docker >/dev/null 2>&1; then
    if command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
        CC="${CROSS_PREFIX}gcc"
    else
        echo "WARNING: cross-compiler not found, falling back to host gcc"
        CC="gcc"
    fi

    mkdir -p build

    "$CC" -std=c11 -O3 -g -shared -fPIC \
        src/dsp/crest.c \
        -o build/dsp.so \
        -lm

    echo "Compiled build/dsp.so"

    # ── Assemble dist package ────────────────────────────────────────────────
    mkdir -p "dist/${MODULE_ID}/wavetables/factory"

    cp src/module.json  "dist/${MODULE_ID}/module.json"
    cp src/ui.js        "dist/${MODULE_ID}/ui.js"
    cp build/dsp.so     "dist/${MODULE_ID}/dsp.so"
    cp wavetables/factory/default.wav "dist/${MODULE_ID}/wavetables/factory/default.wav"

    cd dist
    tar -czf "${MODULE_ID}-module.tar.gz" "${MODULE_ID}/"
    cd "$REPO_ROOT"

    echo "Package: dist/${MODULE_ID}-module.tar.gz"
    echo "Done."
    exit 0
fi

# ── Host: build inside Docker, then deploy from host ────────────────────────
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "Building Docker image $IMAGE_NAME..."
    docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
fi

docker run --rm \
    -v "$REPO_ROOT:/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    -e IN_DOCKER=1 \
    "$IMAGE_NAME" \
    ./scripts/build.sh

# Deploy from the host (has SSH keys and network access to Move)
if [ "${SKIP_INSTALL:-0}" != "1" ]; then
    "$SCRIPT_DIR/install.sh"
fi
