#!/usr/bin/env bash
# full_system_validation_wsl.sh
#
# Full-coverage integration test for the async MPC orchestration system.
# Covers:
#   1) ACK / asynchrony attack scenarios (5 scenarios via async_orchestrator.py)
#   2) Provider tampering detection (BLAKE2b proof mismatch)
#   3) Late-provider asynchrony (provider arrives after consensus decided)
#   4) Masking confidentiality check (bridge file contains no plain values)
#   5) MPC program matrix — sum, avg, triple_sum, parity_sum with result verification
#   6) Provider crash simulation (one provider absent from core set)
#
# Run from repo root (WSL):
#   bash scripts/full_system_validation_wsl.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

SUMMARY_FILE="${REPO_ROOT}/backend_test_summary.txt"
TMP_DIR="${REPO_ROOT}/.tmp_full_test_runs"
mkdir -p "${TMP_DIR}"
rm -f "${SUMMARY_FILE}"

# ─── Helpers ────────────────────────────────────────────────────────────────

log() {
  echo ""
  echo "══════════════════════════════════════════════════════"
  echo "[full-test] $*"
  echo "══════════════════════════════════════════════════════"
}

append_summary() {
  local component="$1"
  local status="$2"
  local details="$3"
  echo "${component} | ${status} | ${details}" >> "${SUMMARY_FILE}"
}

run_cmd() {
  local outfile="$1"
  shift
  "$@" >"${outfile}" 2>&1
}

assert_contains() {
  local needle="$1"
  local file="$2"
  [[ "$(<"${file}")" == *"${needle}"* ]]
}

assert_not_contains() {
  local needle="$1"
  local file="$2"
  [[ "$(<"${file}")" != *"${needle}"* ]]
}

assert_str_contains() {
  local needle="$1"
  local haystack="$2"
  [[ "${haystack}" == *"${needle}"* ]]
}

pass_count=0
fail_count=0

mark_pass() { pass_count=$((pass_count + 1)); }
mark_fail() { fail_count=$((fail_count + 1)); }

clean_workspace() {
  # Wipe all per-round state including provider secrets (each provider generates fresh ones).
  rm -rf inputs logs artifacts core_set.txt provider_secrets
  mkdir -p inputs logs artifacts provider_secrets
  mkdir -p "${TMP_DIR}"
}

# ─── Build ───────────────────────────────────────────────────────────────────

log "Building all binaries"
cmake --build build -j4 --target data_provider consensus ack_crypto_tool spdz_bridge

# ─── Section 1: ACK / asynchrony attack scenarios ───────────────────────────

log "1) ACK / asynchrony attack scenarios"

SCENARIOS=(
  "normal:ok"
  "insufficient-acks:fail"
  "replay-ack:ok"
  "hash-mismatch:ok"
  "stale-ack:fail"
)

for item in "${SCENARIOS[@]}"; do
  scenario="${item%%:*}"
  expected="${item##*:}"
  out="${TMP_DIR}/scenario_${scenario}.log"

  set +e
  run_cmd "${out}" python3 scripts/async_orchestrator.py \
    --clean \
    --session-id "full-${scenario}" \
    --round-id 1 \
    --providers 1:10,2:20,3:30,4:40,5:50 \
    --computation-nodes 3 \
    --k-acks 2 \
    --ack-nodes 3 \
    --ack-timeout-seconds 2 \
    --scenario "${scenario}"
  rc=$?
  set -e

  if [[ "${expected}" == "ok" && ${rc} -eq 0 ]]; then
    append_summary "scenario:${scenario}" "PASS" "expected success"
    mark_pass
  elif [[ "${expected}" == "fail" && ${rc} -ne 0 ]]; then
    append_summary "scenario:${scenario}" "PASS" "expected rejection"
    mark_pass
  else
    append_summary "scenario:${scenario}" "FAIL" "unexpected exit code ${rc}"
    mark_fail
  fi
done

# ─── Section 2: Provider tampering detection ────────────────────────────────

log "2) Provider tampering — BLAKE2b proof mismatch"

clean_workspace
run_cmd "${TMP_DIR}/tamper_p1.log" ./build/node/data_provider 1 11 --computation-nodes 3
run_cmd "${TMP_DIR}/tamper_p2.log" ./build/node/data_provider 2 22 --computation-nodes 3
run_cmd "${TMP_DIR}/tamper_p3.log" ./build/node/data_provider 3 33 --computation-nodes 3

# Corrupt provider 2's masked_value after it has been written (proof becomes invalid).
python3 - <<'PY'
from pathlib import Path
p = Path("inputs/provider_2.txt")
lines = p.read_text().splitlines()
for i, line in enumerate(lines):
    if line.startswith("masked_value="):
        lines[i] = "masked_value=999999999999"
        break
p.write_text("\n".join(lines) + "\n")
PY

set +e
run_cmd "${TMP_DIR}/tamper_consensus.log" ./build/consensus/consensus 3
tamper_rc=$?
set -e

if [[ ${tamper_rc} -ne 0 ]] && assert_contains "invalid cryptographic proof" "${TMP_DIR}/tamper_consensus.log"; then
  append_summary "tampering:provider-file" "PASS" "proof mismatch rejected by consensus"
  mark_pass
else
  append_summary "tampering:provider-file" "FAIL" "tampering was not rejected"
  mark_fail
fi

# ─── Section 3: Late-provider asynchrony ────────────────────────────────────

log "3) Late-provider asynchrony — late input excluded from core set"

clean_workspace
run_cmd "${TMP_DIR}/late_p1.log" ./build/node/data_provider 1 5 --computation-nodes 3
run_cmd "${TMP_DIR}/late_p2.log" ./build/node/data_provider 2 6 --computation-nodes 3
# Consensus decides with just 2 providers (quorum met).
run_cmd "${TMP_DIR}/late_consensus.log" ./build/consensus/consensus 2
# Provider 3 arrives after consensus — must NOT appear in core_set.txt.
run_cmd "${TMP_DIR}/late_p3.log" ./build/node/data_provider 3 7 --computation-nodes 3

if [[ -f core_set.txt ]] \
  && assert_contains "1" core_set.txt \
  && assert_contains "2" core_set.txt \
  && assert_not_contains "3" core_set.txt; then
  append_summary "asynchrony:late-provider" "PASS" "late input correctly excluded from core set"
  mark_pass
else
  append_summary "asynchrony:late-provider" "FAIL" "core set handling unexpected"
  mark_fail
fi

# ─── Section 4: Masking confidentiality check ───────────────────────────────

log "4) Masking confidentiality — bridge inputs never contain plain values"

clean_workspace
export MPC_PROVIDER_SECRET="${MPC_PROVIDER_SECRET:-mpc-demo-secret}"
run_cmd "${TMP_DIR}/conf_p1.log" ./build/node/data_provider 1 42 --computation-nodes 3
run_cmd "${TMP_DIR}/conf_p2.log" ./build/node/data_provider 2 42 --computation-nodes 3
run_cmd "${TMP_DIR}/conf_p3.log" ./build/node/data_provider 3 42 --computation-nodes 3
run_cmd "${TMP_DIR}/conf_consensus.log" ./build/consensus/consensus 3

# Check: none of the provider input files contain "masked_value=42" (the plain value).
# If masking worked, the stored value must differ from 42.
# Also verify that per-party share files were created (s_i was split at the provider side).
conf_ok=true
for id in 1 2 3; do
  if grep -q "^masked_value=42$" "inputs/provider_${id}.txt" 2>/dev/null; then
    conf_ok=false
    break
  fi
  # Check share files exist (provider split s_i into 3 shares — bridge never sees full s_i).
  for p in 0 1 2; do
    if [[ ! -f "provider_secrets/provider_${id}_share_${p}.secret" ]]; then
      conf_ok=false
      break 2
    fi
  done
done

if [[ "${conf_ok}" == "true" ]]; then
  append_summary "confidentiality:masking" "PASS" "no plain values; per-party share files verified"
  mark_pass
else
  append_summary "confidentiality:masking" "FAIL" "plain value 42 found unmasked in a provider input file"
  mark_fail
fi

# ─── Section 5: MPC program matrix with result verification ─────────────────

log "5) MPC program matrix — semi2k × {sum, avg, triple_sum, parity_sum}"
# Providers: 1:7, 2:15, 3:20  => sum=42, avg=14, triple_sum=126, parity_sum=42%2=0

declare -A EXPECTED_RESULTS
EXPECTED_RESULTS["sum"]="42"
EXPECTED_RESULTS["avg"]="14"
EXPECTED_RESULTS["triple_sum"]="126"
EXPECTED_RESULTS["parity_sum"]="0"

MPC_PROGRAMS=( "sum" "avg" "triple_sum" "parity_sum" )

for prog in "${MPC_PROGRAMS[@]}"; do
  clean_workspace
  run_cmd "${TMP_DIR}/prog_${prog}_p1.log" ./build/node/data_provider 1 7 --computation-nodes 3
  run_cmd "${TMP_DIR}/prog_${prog}_p2.log" ./build/node/data_provider 2 15 --computation-nodes 3
  run_cmd "${TMP_DIR}/prog_${prog}_p3.log" ./build/node/data_provider 3 20 --computation-nodes 3
  run_cmd "${TMP_DIR}/prog_${prog}_consensus.log" ./build/consensus/consensus 3

  set +e
  run_cmd "${TMP_DIR}/prog_${prog}_bridge.log" \
    ./build/spdz_bridge/spdz_bridge \
    --computation-nodes 3 \
    "${REPO_ROOT}/programs/${prog}.mpc"
  bridge_rc=$?
  set -e

  key="semi2k:${prog}"

  if [[ ${bridge_rc} -ne 0 ]]; then
    if assert_contains "semi2k-party.x not found" "${TMP_DIR}/prog_${prog}_bridge.log"; then
      append_summary "${key}" "FAIL" "semi2k-party.x missing — build MP-SPDZ first"
    else
      append_summary "${key}" "FAIL" "bridge failed (rc=${bridge_rc})"
    fi
    mark_fail
    continue
  fi

  # Extract result value from "MP-SPDZ result: <N>"
  result_line=$(grep "MP-SPDZ result:" "${TMP_DIR}/prog_${prog}_bridge.log" || true)
  result_val=$(echo "${result_line}" | grep -oP '(?<=MP-SPDZ result: )\S+' || true)
  expected="${EXPECTED_RESULTS[${prog}]}"

  if [[ "${result_val}" == "${expected}" ]]; then
    append_summary "${key}" "PASS" "result=${result_val} (expected ${expected})"
    mark_pass
  elif [[ -z "${result_val}" ]]; then
    append_summary "${key}" "FAIL" "no result line in output"
    mark_fail
  else
    append_summary "${key}" "FAIL" "wrong result=${result_val} (expected ${expected})"
    mark_fail
  fi
done

# ─── Section 6: Provider crash simulation ───────────────────────────────────

log "6) Provider crash simulation — absent provider excluded from core set"

clean_workspace
# Providers 1 and 2 submit; provider 3 "crashes" (never submits).
run_cmd "${TMP_DIR}/crash_p1.log" ./build/node/data_provider 1 100 --computation-nodes 2
run_cmd "${TMP_DIR}/crash_p2.log" ./build/node/data_provider 2 200 --computation-nodes 2
# Consensus quorum = 2 → decides without provider 3.
run_cmd "${TMP_DIR}/crash_consensus.log" ./build/consensus/consensus 2

if [[ -f core_set.txt ]] \
  && assert_contains "1" core_set.txt \
  && assert_contains "2" core_set.txt \
  && assert_not_contains "3" core_set.txt; then

  # Bridge should compute sum = 300 for providers 1+2.
  set +e
  run_cmd "${TMP_DIR}/crash_bridge.log" \
    ./build/spdz_bridge/spdz_bridge --computation-nodes 2
  crash_rc=$?
  set -e

  if [[ ${crash_rc} -eq 0 ]]; then
    crash_result=$(grep "MP-SPDZ result:" "${TMP_DIR}/crash_bridge.log" | grep -oP '(?<=MP-SPDZ result: )\S+' || true)
    if [[ "${crash_result}" == "300" ]]; then
      append_summary "crash:provider-absent" "PASS" "core set={1,2}, result=300 correct"
      mark_pass
    elif [[ -z "${crash_result}" ]]; then
      append_summary "crash:provider-absent" "FAIL" "no result after crash simulation"
      mark_fail
    else
      append_summary "crash:provider-absent" "FAIL" "wrong result=${crash_result} (expected 300)"
      mark_fail
    fi
  else
    if assert_contains "semi2k-party.x not found" "${TMP_DIR}/crash_bridge.log"; then
      append_summary "crash:provider-absent" "FAIL" "semi2k-party.x missing"
    else
      append_summary "crash:provider-absent" "FAIL" "bridge failed (rc=${crash_rc})"
    fi
    mark_fail
  fi
else
  append_summary "crash:provider-absent" "FAIL" "core set did not exclude crashed provider"
  mark_fail
fi

# ─── Final summary ───────────────────────────────────────────────────────────

echo "" >> "${SUMMARY_FILE}"
echo "TOTAL_PASS=${pass_count}" >> "${SUMMARY_FILE}"
echo "TOTAL_FAIL=${fail_count}" >> "${SUMMARY_FILE}"

log "Done — results"
cat "${SUMMARY_FILE}"

if [[ ${fail_count} -gt 0 ]]; then
  echo ""
  echo "[full-test] FAILED: ${fail_count} test(s) failed."
  exit 1
fi

echo ""
echo "[full-test] ALL ${pass_count} TESTS PASSED."
