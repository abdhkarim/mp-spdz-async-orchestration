#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

SUMMARY_FILE="${REPO_ROOT}/backend_test_summary.txt"
TMP_DIR="${REPO_ROOT}/.tmp_full_test_runs"
mkdir -p "${TMP_DIR}"
rm -f "${SUMMARY_FILE}"

log() {
  echo "[full-test] $*"
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
  local content
  content="$(<"${file}")"
  [[ "${content}" == *"${needle}"* ]]
}

assert_not_contains() {
  local needle="$1"
  local file="$2"
  local content
  content="$(<"${file}")"
  [[ "${content}" != *"${needle}"* ]]
}

SCENARIOS=(
  "normal:ok"
  "insufficient-acks:fail"
  "replay-ack:ok"
  "hash-mismatch:ok"
  "stale-ack:fail"
)

BACKENDS=(
  "semi2k"
  "semi"
  "shamir"
  "replicated-ring"
  "replicated-field"
  "player-online"
)

# All demo .mpc circuits tested against every backend (same Fig.2 inputs + hello_mpc).
MPC_PROGRAMS=(
  "sum"
  "avg"
  "triple_sum"
  "parity_sum"
  "hello_mpc"
)

pass_count=0
fail_count=0

mark_pass() {
  pass_count=$((pass_count + 1))
}

mark_fail() {
  fail_count=$((fail_count + 1))
}

clean_workspace() {
  rm -rf inputs logs artifacts core_set.txt
  mkdir -p inputs logs artifacts
  mkdir -p "${TMP_DIR}"
}

log "Building binaries..."
cmake --build build -j4 --target data_provider consensus ack_crypto_tool spdz_bridge

log "1) ACK/asynchrony attack scenarios"
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
    --backend semi2k \
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

log "2) Provider tampering test (no malformed label)"
clean_workspace
run_cmd "${TMP_DIR}/tamper_provider_setup.log" ./build/node/data_provider 1 11
run_cmd "${TMP_DIR}/tamper_provider_setup2.log" ./build/node/data_provider 2 22
run_cmd "${TMP_DIR}/tamper_provider_setup3.log" ./build/node/data_provider 3 33
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
run_cmd "${TMP_DIR}/tamper_provider_consensus.log" ./build/consensus/consensus 3
tamper_rc=$?
set -e
if [[ ${tamper_rc} -ne 0 ]] && assert_contains "invalid cryptographic proof" "${TMP_DIR}/tamper_provider_consensus.log"; then
  append_summary "tampering:provider-file" "PASS" "proof mismatch rejected"
  mark_pass
else
  append_summary "tampering:provider-file" "FAIL" "tampering not rejected as expected"
  mark_fail
fi

log "3) Late provider arrival test (asynchrony)"
clean_workspace
run_cmd "${TMP_DIR}/late_provider_1.log" ./build/node/data_provider 1 5
run_cmd "${TMP_DIR}/late_provider_2.log" ./build/node/data_provider 2 6
run_cmd "${TMP_DIR}/late_provider_consensus.log" ./build/consensus/consensus 2
run_cmd "${TMP_DIR}/late_provider_3.log" ./build/node/data_provider 3 7
if [[ -f core_set.txt ]] && assert_contains "1" core_set.txt && assert_contains "2" core_set.txt && assert_not_contains "3" core_set.txt; then
  append_summary "asynchrony:late-provider" "PASS" "late input excluded from decided core set"
  mark_pass
else
  append_summary "asynchrony:late-provider" "FAIL" "core set handling unexpected"
  mark_fail
fi

log "4) Backend × program matrix (each backend runs each programs/*.mpc demo)"
export MPC_PROVIDER_SECRET="${MPC_PROVIDER_SECRET:-mpc-demo-secret}"
for backend in "${BACKENDS[@]}"; do
  for prog in "${MPC_PROGRAMS[@]}"; do
    clean_workspace
    run_cmd "${TMP_DIR}/bm_${backend}_${prog}_p1.log" ./build/node/data_provider 1 7
    run_cmd "${TMP_DIR}/bm_${backend}_${prog}_p2.log" ./build/node/data_provider 2 15
    run_cmd "${TMP_DIR}/bm_${backend}_${prog}_p3.log" ./build/node/data_provider 3 20
    run_cmd "${TMP_DIR}/bm_${backend}_${prog}_consensus.log" ./build/consensus/consensus 3

    set +e
    run_cmd "${TMP_DIR}/bm_${backend}_${prog}_bridge.log" ./build/spdz_bridge/spdz_bridge --backend "${backend}" --computation-nodes 3 "${REPO_ROOT}/programs/${prog}.mpc"
    bridge_rc=$?
    set -e

    key="backend:${backend}:${prog}"
    if [[ ${bridge_rc} -eq 0 ]] && assert_contains "MP-SPDZ result:" "${TMP_DIR}/bm_${backend}_${prog}_bridge.log"; then
      if [[ "${prog}" == "hello_mpc" ]] && ! assert_contains "MP-SPDZ result: 3" "${TMP_DIR}/bm_${backend}_${prog}_bridge.log"; then
        append_summary "${key}" "FAIL" "hello_mpc expected MP-SPDZ result: 3"
        mark_fail
        continue
      fi
      append_summary "${key}" "PASS" "MPC run succeeded"
      mark_pass
    elif assert_contains "Backend runtime not found" "${TMP_DIR}/bm_${backend}_${prog}_bridge.log"; then
      append_summary "${key}" "FAIL" "runtime missing in environment"
      mark_fail
    else
      append_summary "${key}" "FAIL" "bridge failed (rc=${bridge_rc})"
      mark_fail
    fi
  done
done

echo "" >> "${SUMMARY_FILE}"
echo "TOTAL_PASS=${pass_count}" >> "${SUMMARY_FILE}"
echo "TOTAL_FAIL=${fail_count}" >> "${SUMMARY_FILE}"

log "Done. Summary: ${SUMMARY_FILE}"
cat "${SUMMARY_FILE}"

if [[ ${fail_count} -gt 0 ]]; then
  exit 1
fi

