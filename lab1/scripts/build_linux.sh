#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="${1:-$PWD}"
BRANCH="${2:-main}"
BUILD_DIR="$REPO_DIR/build"

echo "[1/4] Updating repo..."
git -C "$REPO_DIR" fetch --all || true
if git -C "$REPO_DIR" switch "$BRANCH" 2>/dev/null; then :; else git -C "$REPO_DIR" checkout -B "$BRANCH"; fi
git -C "$REPO_DIR" pull --ff-only || true

echo "[2/4] Configuring..."
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug

echo "[3/4] Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[4/4] Running..."
exec "$BUILD_DIR/bin/hello"
