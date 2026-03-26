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
cmake --build build -j4 --target data_provider consensus ack_crypto_tool share_verifier spdz_bridge

# ─── Section 1: ACK / asynchrony attack scenarios ───────────────────────────

log "1) ACK / asynchrony attack scenarios"

SCENARIOS=(
  "normal:ok"
  "insufficient-acks:fail"
  "replay-ack:ok"
  "hash-mismatch:fail"
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

# ─── Section 7: Type proof admission (provider-side; consensus-verified) ──

log "7) Type proof admission — negative + positive cases"

clean_workspace

PROVIDER_ID=1
PROVIDER_VALUE=10
COMPUTATION_NODES=3
SESSION_ID="typeproof-full-system"
ROUND_ID_BASE=5000
PROTOCOL_VERSION="1"
SCHEMA_ID="semi2k-wire-v1"
MIN_INPUTS=1

CONSENSUS_BIN="./build/consensus/consensus"
DATA_PROVIDER_BIN="./build/node/data_provider"
SHARE_VERIFIER_BIN="./build/consensus/share_verifier"
ACK_CRYPTO_TOOL_BIN="./build/consensus/ack_crypto_tool"

mutate_type_proof() {
  local scenario="$1"
  local expected_schema_id="$2"

  local tp_path="inputs/provider_${PROVIDER_ID}_type_proof.json"

  # Provider generates a correct artifact; we reconstruct/modify it depending on scenario.
  case "${scenario}" in
    typeproof-absent)
      rm -f "${tp_path}" || true
      ;;
    typeproof-invalid-json)
      echo "{" > "${tp_path}"
      ;;
    typeproof-provider-id-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["provider_id"] = ${PROVIDER_ID}+1
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-nonce-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["nonce"] = "00000000000000000000000000000000"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-schema-id-mismatch)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["schema_id"] = "wrong-schema"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-masked-wire-digest-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["masked_wire_digest"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-share-manifest-id-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["share_manifest_id"] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-mask-commitment-leaves-digest-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["mask_commitment_leaves_digest"] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-id-incoherent)
      python3 - <<PY
from pathlib import Path
import json
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))
d["type_proof_id"] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-blob-backend-reject)
      python3 - <<PY
from pathlib import Path
import json, hashlib
p = Path("inputs/provider_${PROVIDER_ID}_type_proof.json")
d = json.load(p.open("r", encoding="utf-8"))

wrong_blob = "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
d["type_proof_blob"] = wrong_blob

# Recompute type_proof_id consistently with the consensus stub formula.
msg = (
  f"provider_id=${PROVIDER_ID};nonce={d['nonce']};schema_id=${SCHEMA_ID};"
  f"type_proof_system={d['type_proof_system']};type_proof_vk_id={d['type_proof_vk_id']};"
  f"type_proof_blob={wrong_blob}"
)
d["type_proof_id"] = hashlib.blake2b(msg.encode("utf-8"), digest_size=32).hexdigest()

p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
      ;;
    typeproof-positive)
      # keep generated artifact as-is
      ;;
    *)
      echo "Unknown typeproof scenario: ${scenario}" >&2
      exit 2
      ;;
  esac
}

run_typeproof_case() {
  local scenario="$1"
  local expected_admit="$2"  # "ok" or "fail"

  clean_workspace

  # Generate provider evidence + shares + manifest + provider-side type_proof.
  run_cmd "${TMP_DIR}/typeproof_provider_${scenario}.log" \
    "${DATA_PROVIDER_BIN}" "${PROVIDER_ID}" "${PROVIDER_VALUE}" \
    --computation-nodes "${COMPUTATION_NODES}"

  # Mutate/remove type_proof artifact.
  mutate_type_proof "${scenario}" "${SCHEMA_ID}"

  # Prepare per-party verifier ACK keys.
  case_dir="${TMP_DIR}/typeproof_${scenario}"
  acks_dir="${case_dir}/acks"
  cn_keys_dir="${case_dir}/cn_keys"
  mkdir -p "${acks_dir}" "${cn_keys_dir}"

  for cn_id in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/cn_key_${cn_id}.log" \
      "${ACK_CRYPTO_TOOL_BIN}" gen-keypair \
      "${cn_keys_dir}/cn_${cn_id}.pub.hex" \
      "${cn_keys_dir}/cn_${cn_id}.sec.hex"
  done

  # Create local share ACKs (distributed verifiers).
  for party_index in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/share_ack_p${party_index}.log" \
      "${SHARE_VERIFIER_BIN}" \
        --session-id "${SESSION_ID}" \
        --round-id "${ROUND_ID}" \
        --protocol-version "${PROTOCOL_VERSION}" \
        --schema-id "${SCHEMA_ID}" \
        --provider-id "${PROVIDER_ID}" \
        --party-index "${party_index}" \
        --inputs-dir "${REPO_ROOT}/inputs" \
        --provider-secrets-dir "${REPO_ROOT}/provider_secrets" \
        --share-manifest-path "${REPO_ROOT}/inputs/provider_${PROVIDER_ID}_manifest.json" \
        --cn-keys-dir "${cn_keys_dir}" \
        --acks-out-dir "${acks_dir}"
  done

  # Run consensus admission.
  set +e
  run_cmd "${case_dir}/consensus.log" \
    "${CONSENSUS_BIN}" "${MIN_INPUTS}" \
      --acks-dir "${acks_dir}" \
      --num-parties "${COMPUTATION_NODES}" \
      --session-id "${SESSION_ID}" \
      --round-id "${ROUND_ID}" \
      --timeout-seconds 0 \
      --artifacts-dir "${case_dir}/artifacts" \
      --cn-keys-dir "${cn_keys_dir}" \
      --schema-id "${SCHEMA_ID}"
  rc=$?
  set -e

  if [[ "${expected_admit}" == "ok" ]]; then
    if [[ ${rc} -eq 0 ]] \
      && [[ -f core_set.txt ]] \
      && assert_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "typeproof:${scenario}" "PASS" "provider admitted"
      mark_pass
    else
      append_summary "typeproof:${scenario}" "FAIL" "expected admission (rc=${rc})"
      mark_fail
    fi
  else
    # Negative: provider must not be admitted.
    if [[ ! -f core_set.txt ]] || assert_not_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "typeproof:${scenario}" "PASS" "provider rejected"
      mark_pass
    else
      append_summary "typeproof:${scenario}" "FAIL" "provider unexpectedly admitted"
      mark_fail
    fi
  fi
}

SCENARIOS_TYPEPROOF=(
  "typeproof-positive:ok"
  "typeproof-absent:fail"
  "typeproof-invalid-json:fail"
  "typeproof-provider-id-incoherent:fail"
  "typeproof-nonce-incoherent:fail"
  "typeproof-schema-id-mismatch:fail"
  "typeproof-masked-wire-digest-incoherent:fail"
  "typeproof-share-manifest-id-incoherent:fail"
  "typeproof-mask-commitment-leaves-digest-incoherent:fail"
  "typeproof-id-incoherent:fail"
  "typeproof-blob-backend-reject:fail"
)

for item in "${SCENARIOS_TYPEPROOF[@]}"; do
  scenario="${item%%:*}"
  expected="${item##*:}"
  ROUND_ID=$((ROUND_ID_BASE + RANDOM % 10000))
  if [[ "${expected}" == "ok" ]]; then
    run_typeproof_case "${scenario}" "ok"
  else
    run_typeproof_case "${scenario}" "fail"
  fi
done

# ─── Section 8: Semantic type proof backend (non-ZK) ─────────────────────────
#
# This section validates that the semantic backend is wired end-to-end:
# - provider generates semantic-schema-v1 type_proof_blob (base64(JSON))
# - consensus loads the authoritative schema from schemas/type_registry.json
# - consensus verifies structure/semantic constraints for:
#   int64, bool, fixed_point(scale), vector, tuple, record, recursive compositions
#
log "8) Semantic type proof backend — structured types (int64/bool/fixed_point/vector/tuple/record/recursive)"

run_semantic_case() {
  local case_name="$1"
  local schema_id="$2"
  local provider_value="$3"
  local typed_json="$4"           # can be empty
  local expected_admit="$5"       # "ok" or "fail"

  clean_workspace

  SESSION_ID="semantic-${case_name}"
  ROUND_ID=$((6000 + RANDOM % 10000))

  # Generate provider evidence + shares + manifest + semantic type_proof.
  if [[ -n "${typed_json}" ]]; then
    run_cmd "${TMP_DIR}/semantic_provider_${case_name}.log" \
      ./build/node/data_provider "${PROVIDER_ID}" "${provider_value}" \
        --computation-nodes "${COMPUTATION_NODES}" \
        --schema-id "${schema_id}" \
        --type-proof-system "semantic-schema-v1" \
        --typed-json "${typed_json}"
  else
    run_cmd "${TMP_DIR}/semantic_provider_${case_name}.log" \
      ./build/node/data_provider "${PROVIDER_ID}" "${provider_value}" \
        --computation-nodes "${COMPUTATION_NODES}" \
        --schema-id "${schema_id}" \
        --type-proof-system "semantic-schema-v1"
  fi

  case_dir="${TMP_DIR}/semantic_${case_name}"
  acks_dir="${case_dir}/acks"
  cn_keys_dir="${case_dir}/cn_keys"
  mkdir -p "${acks_dir}" "${cn_keys_dir}"

  # Prepare per-party verifier ACK keys.
  for cn_id in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/cn_key_${cn_id}.log" \
      "${ACK_CRYPTO_TOOL_BIN}" gen-keypair \
      "${cn_keys_dir}/cn_${cn_id}.pub.hex" \
      "${cn_keys_dir}/cn_${cn_id}.sec.hex"
  done

  # Create local share ACKs (distributed verifiers).
  for party_index in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/share_ack_p${party_index}.log" \
      "${SHARE_VERIFIER_BIN}" \
        --session-id "${SESSION_ID}" \
        --round-id "${ROUND_ID}" \
        --protocol-version "${PROTOCOL_VERSION}" \
        --schema-id "${schema_id}" \
        --provider-id "${PROVIDER_ID}" \
        --party-index "${party_index}" \
        --inputs-dir "${REPO_ROOT}/inputs" \
        --provider-secrets-dir "${REPO_ROOT}/provider_secrets" \
        --share-manifest-path "${REPO_ROOT}/inputs/provider_${PROVIDER_ID}_manifest.json" \
        --cn-keys-dir "${cn_keys_dir}" \
        --acks-out-dir "${acks_dir}"
  done

  # Run consensus admission (ACK mode + semantic schema id).
  set +e
  run_cmd "${case_dir}/consensus.log" \
    "${CONSENSUS_BIN}" "${MIN_INPUTS}" \
      --acks-dir "${acks_dir}" \
      --num-parties "${COMPUTATION_NODES}" \
      --session-id "${SESSION_ID}" \
      --round-id "${ROUND_ID}" \
      --timeout-seconds 0 \
      --artifacts-dir "${case_dir}/artifacts" \
      --cn-keys-dir "${cn_keys_dir}" \
      --schema-id "${schema_id}"
  rc=$?
  set -e

  if [[ "${expected_admit}" == "ok" ]]; then
    if [[ ${rc} -eq 0 ]] \
      && [[ -f core_set.txt ]] \
      && assert_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "semantic:${case_name}" "PASS" "provider admitted (schema=${schema_id})"
      mark_pass
    else
      append_summary "semantic:${case_name}" "FAIL" "expected admission (schema=${schema_id}, rc=${rc})"
      mark_fail
    fi
  else
    if [[ ! -f core_set.txt ]] || assert_not_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "semantic:${case_name}" "PASS" "provider rejected (schema=${schema_id})"
      mark_pass
    else
      append_summary "semantic:${case_name}" "FAIL" "provider unexpectedly admitted (schema=${schema_id})"
      mark_fail
    fi
  fi
}

# int64
run_semantic_case "int64_positive" "int64-v1" "123" "" "ok"

# bool
run_semantic_case "bool_positive" "bool-v1" "1" "" "ok"
run_semantic_case "bool_violation" "bool-v1" "2" "" "fail"

# fixed_point(scale=2) encoded as {"unscaled": <int>}
run_semantic_case "fixed_point_positive" "fixed_point_s2_v1" "12345" "" "ok"
run_semantic_case "fixed_point_struct_violation" "fixed_point_s2_v1" "12345" "\"oops\"" "fail"

# vector
run_semantic_case "vector_positive" "vector_int64_len3_v1" "0" "[1,2,3]" "ok"
run_semantic_case "vector_len_violation" "vector_int64_len3_v1" "0" "[1,2]" "fail"

# tuple
run_semantic_case "tuple_positive" "tuple_bool_int64_v1" "0" "[1,42]" "ok"
run_semantic_case "tuple_arity_violation" "tuple_bool_int64_v1" "0" "[1]" "fail"

# record
run_semantic_case "record_positive" "record_ab_v1" "0" "{\"a\":7,\"b\":1}" "ok"
run_semantic_case "record_extra_field_violation" "record_ab_v1" "0" "{\"a\":7,\"b\":1,\"c\":0}" "fail"

# recursive (nullable tail)
run_semantic_case "recursive_positive_null_tail" "recursive_list_node_v1" "0" "{\"head\":1,\"tail\":null}" "ok"

# ─── Section 9: proof-real-v1 backend (commitments + OR-proofs) ─────────

log "9) proof-real-v1 backend — carry + bool OR proofs"

run_proof_real_v1_case() {
  local case_name="$1"
  local expected_admit="$2"  # "ok" or "fail"
  local schema_id="$3"
  local provider_value="$4"

  clean_workspace

  PROVIDER_ID=1
  PROVIDER_VALUE="${provider_value}"
  COMPUTATION_NODES=3
  SESSION_ID="proofreal-${case_name}"
  ROUND_ID=$((7500 + RANDOM % 10000))
  PROTOCOL_VERSION="1"
  SCHEMA_ID="${schema_id}"

  CONSENSUS_BIN="./build/consensus/consensus"
  DATA_PROVIDER_BIN="./build/node/data_provider"
  SHARE_VERIFIER_BIN="./build/consensus/share_verifier"
  ACK_CRYPTO_TOOL_BIN="./build/consensus/ack_crypto_tool"

  MIN_INPUTS=1

  # Generate provider evidence + shares + manifest + proof-real-v1 type_proof.
  run_cmd "${TMP_DIR}/proofreal_provider_${case_name}.log" \
    "${DATA_PROVIDER_BIN}" "${PROVIDER_ID}" "${PROVIDER_VALUE}" \
      --computation-nodes "${COMPUTATION_NODES}" \
      --schema-id "${SCHEMA_ID}" \
      --type-proof-system "proof-real-v1"

  # Mutate the proof artifact depending on the scenario.
  mutate_proof_real_v1() {
    local scenario="$1"
    local tp_path="inputs/provider_${PROVIDER_ID}_type_proof.json"

    python3 - <<PY
import base64, json, hashlib
from pathlib import Path

tp_path = Path("${tp_path}")
d = json.load(tp_path.open("r", encoding="utf-8"))

assert d["type_proof_system"] == "proof-real-v1"
blob = d["type_proof_blob"]

obj = json.loads(base64.b64decode(blob).decode("utf-8"))

if "${scenario}" == "proofreal-carry-or-tampered" or "${scenario}" == "proofreal-carry-or-tampered-int64":
    s0 = obj["carry_or"]["s0"]
    obj["carry_or"]["s0"] = ("1" if s0[0] == "0" else "0") + s0[1:]

elif "${scenario}" == "proofreal-bool-or-tampered":
    s0 = obj["bool_or"]["s0"]
    obj["bool_or"]["s0"] = ("1" if s0[0] == "0" else "0") + s0[1:]

elif "${scenario}" == "proofreal-x-randomizer-tampered-int64":
    xr = obj["x_randomizer"]
    obj["x_randomizer"] = ("1" if xr[0] != "1" else "2") + xr[1:]

elif "${scenario}" == "proofreal-binding-mismatch-int64":
    md = d["masked_wire_digest"]
    d["masked_wire_digest"] = ("0" if md[0] != "0" else "1") + md[1:]

elif "${scenario}" == "proofreal-blob-invalid":
    d["type_proof_blob"] = "deadbeef"
else:
    raise SystemExit("unknown scenario")

if "${scenario}" != "proofreal-blob-invalid":
    new_blob_json = json.dumps(obj, separators=(",",":"))
    d["type_proof_blob"] = base64.b64encode(new_blob_json.encode("utf-8")).decode("utf-8")

provider_id_numeric = str(${PROVIDER_ID})
nonce = d["nonce"]
schema_id = d["schema_id"]
type_proof_system = d["type_proof_system"]
type_proof_vk_id = d["type_proof_vk_id"]
type_proof_blob = d["type_proof_blob"]

msg = f"provider_id={provider_id_numeric};nonce={nonce};schema_id={schema_id};type_proof_system={type_proof_system};type_proof_vk_id={type_proof_vk_id};type_proof_blob={type_proof_blob}"
d["type_proof_id"] = hashlib.blake2b(msg.encode("utf-8"), digest_size=32).hexdigest()

tp_path.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY
  }

  case "${case_name}" in
    proofreal-positive|proofreal-int64-positive|proofreal-fixed_point-positive|proofreal-vector-structure-invalid)
      ;;
    proofreal-carry-or-tampered|proofreal-bool-or-tampered|proofreal-blob-invalid|proofreal-carry-or-tampered-int64|proofreal-x-randomizer-tampered-int64|proofreal-binding-mismatch-int64)
      mutate_proof_real_v1 "${case_name}"
      ;;
    *)
      echo "Unknown proofreal scenario: ${case_name}" >&2
      exit 2
      ;;
  esac

  case_dir="${TMP_DIR}/proofreal_${case_name}"
  acks_dir="${case_dir}/acks"
  cn_keys_dir="${case_dir}/cn_keys"
  mkdir -p "${acks_dir}" "${cn_keys_dir}"

  # Prepare per-party verifier ACK keys.
  for cn_id in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/cn_key_${cn_id}.log" \
      "${ACK_CRYPTO_TOOL_BIN}" gen-keypair \
      "${cn_keys_dir}/cn_${cn_id}.pub.hex" \
      "${cn_keys_dir}/cn_${cn_id}.sec.hex"
  done

  # Create local share ACKs.
  for party_index in $(seq 0 $((COMPUTATION_NODES - 1))); do
    run_cmd "${case_dir}/share_ack_p${party_index}.log" \
      "${SHARE_VERIFIER_BIN}" \
        --session-id "${SESSION_ID}" \
        --round-id "${ROUND_ID}" \
        --protocol-version "${PROTOCOL_VERSION}" \
        --schema-id "${SCHEMA_ID}" \
        --provider-id "${PROVIDER_ID}" \
        --party-index "${party_index}" \
        --inputs-dir "${REPO_ROOT}/inputs" \
        --provider-secrets-dir "${REPO_ROOT}/provider_secrets" \
        --share-manifest-path "${REPO_ROOT}/inputs/provider_${PROVIDER_ID}_manifest.json" \
        --cn-keys-dir "${cn_keys_dir}" \
        --acks-out-dir "${acks_dir}"
  done

  # Run consensus admission (ACK mode).
  set +e
  run_cmd "${case_dir}/consensus.log" \
    "${CONSENSUS_BIN}" "${MIN_INPUTS}" \
      --acks-dir "${acks_dir}" \
      --num-parties "${COMPUTATION_NODES}" \
      --session-id "${SESSION_ID}" \
      --round-id "${ROUND_ID}" \
      --timeout-seconds 0 \
      --artifacts-dir "${case_dir}/artifacts" \
      --cn-keys-dir "${cn_keys_dir}" \
      --schema-id "${SCHEMA_ID}"
  rc=$?
  set -e

  if [[ "${expected_admit}" == "ok" ]]; then
    if [[ ${rc} -eq 0 ]] \
      && [[ -f core_set.txt ]] \
      && assert_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "proofreal:${case_name}" "PASS" "provider admitted"
      mark_pass
    else
      append_summary "proofreal:${case_name}" "FAIL" "expected admission (rc=${rc})"
      mark_fail
    fi
  else
    if [[ ! -f core_set.txt ]] || assert_not_contains "${PROVIDER_ID}" core_set.txt; then
      append_summary "proofreal:${case_name}" "PASS" "provider rejected"
      mark_pass
    else
      append_summary "proofreal:${case_name}" "FAIL" "provider unexpectedly admitted"
      mark_fail
    fi
  fi
}

run_proof_real_v1_case "proofreal-positive" "ok" "bool-v1" "1"
run_proof_real_v1_case "proofreal-carry-or-tampered" "fail" "bool-v1" "1"
run_proof_real_v1_case "proofreal-bool-or-tampered" "fail" "bool-v1" "1"
run_proof_real_v1_case "proofreal-blob-invalid" "fail" "bool-v1" "1"

run_proof_real_v1_case "proofreal-int64-positive" "ok" "int64-v1" "123"
run_proof_real_v1_case "proofreal-fixed_point-positive" "ok" "fixed_point_s2_v1" "12345"

run_proof_real_v1_case "proofreal-carry-or-tampered-int64" "fail" "int64-v1" "123"
run_proof_real_v1_case "proofreal-x-randomizer-tampered-int64" "fail" "int64-v1" "123"
run_proof_real_v1_case "proofreal-binding-mismatch-int64" "fail" "int64-v1" "123"
run_proof_real_v1_case "proofreal-vector-structure-invalid" "fail" "vector_int64_len3_v1" "1"


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
