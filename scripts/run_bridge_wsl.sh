#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"

echo "[WSL] Repo root : ${REPO_ROOT}"
echo "[WSL] Build dir : ${BUILD_DIR}"

mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"
cmake ..
cmake --build . -j

echo "[WSL] Running spdz_bridge..."
./spdz_bridge/spdz_bridge "$@"
