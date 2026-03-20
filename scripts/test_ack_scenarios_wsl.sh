#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

cmake --build build -j4 --target data_provider consensus ack_crypto_tool spdz_bridge

run_case() {
  local scenario="$1"
  local expect_rejected="$2"
  echo "=== scenario: ${scenario} ==="
  python3 scripts/async_orchestrator.py \
    --clean \
    --session-id "ack-${scenario}" \
    --round-id 1 \
    --providers 1:10,2:20,3:30,4:40,5:50 \
    --backend semi2k \
    --computation-nodes 3 \
    --k-acks 2 \
    --ack-nodes 3 \
    --ack-timeout-seconds 2 \
    --scenario "${scenario}" || true

  python3 - <<'PY'
import json
from pathlib import Path
p = Path("artifacts/justification.json")
if not p.exists():
    print("justification.json missing")
else:
    data = json.loads(p.read_text())
    print("accepted:", len(data.get("accepted", [])), "rejected:", len(data.get("rejected", [])))
    for item in data.get("rejected", []):
        print(" - provider", item.get("provider_id"), "reason:", item.get("reason"))
PY
  echo
}

run_case normal no
run_case insufficient-acks yes
run_case replay-ack yes
run_case hash-mismatch yes
run_case stale-ack yes
