#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

cmake --build build -j4 --target data_provider consensus ack_crypto_tool spdz_bridge

python3 scripts/async_orchestrator.py "$@"
