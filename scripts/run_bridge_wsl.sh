#!/usr/bin/env bash
# Build from repo root, then run spdz_bridge with cwd = repo root (required:
# bridge resolves inputs/, core_set.txt, third_party/MP-SPDZ via current_path()).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"

echo "[WSL] Repo root : ${REPO_ROOT}"
echo "[WSL] Build dir : ${BUILD_DIR}"

mkdir -p "${BUILD_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j

echo "[WSL] Running spdz_bridge from repo root..."
cd "${REPO_ROOT}"
exec "${BUILD_DIR}/spdz_bridge/spdz_bridge" "$@"
