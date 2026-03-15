#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR" || exit 1

PROGRAM="${1:-programs/sum.mpc}"

BACKENDS=(
  semi2k
  semi
  shamir
  replicated-ring
  replicated-field
  player-online
)

BRIDGE="./build/spdz_bridge/spdz_bridge"
PROVIDER="./build/node/data_provider"
CONSENSUS="./build/consensus/consensus"
RESULTS_FILE="backend_test_summary.txt"

print_line() {
  printf '%*s\n' "${COLUMNS:-80}" '' | tr ' ' '-'
}

prepare_env() {
  rm -rf inputs logs core_set.txt
  mkdir -p inputs logs
  export MPC_PROVIDER_SECRET=mpc-demo-secret

  "$PROVIDER" 1 7 >/dev/null 2>&1 || return 1
  "$PROVIDER" 2 15 >/dev/null 2>&1 || return 1
  "$PROVIDER" 3 20 >/dev/null 2>&1 || return 1

  "$CONSENSUS" 3 >/dev/null 2>&1 || return 1
  return 0
}

collect_logs_text() {
  local combined=""
  if [[ -d logs ]]; then
    local f
    for f in logs/player_*.log; do
      if [[ -f "$f" ]]; then
        combined+=$'\n'
        combined+="===== ${f} ====="
        combined+=$'\n'
        combined+="$(cat "$f")"
        combined+=$'\n'
      fi
    done
  fi
  printf "%s" "$combined"
}

detect_cause_from_text() {
  local text="$1"

  if grep -qi "requires at least" <<<"$text"; then
    echo "Not enough parties for this backend"
  elif grep -qi "Cannot access Player-Data/P[0-9].*\.pem" <<<"$text"; then
    echo "SSL certificates missing"
  elif grep -qi "setup-ssl.sh" <<<"$text"; then
    echo "SSL material not initialized"
  elif grep -qi "no modulus in .*Params-Data" <<<"$text"; then
    echo "Missing or invalid Params-Data / preprocessing"
  elif grep -qi "Purging preprocessed data because something is wrong" <<<"$text"; then
    echo "Invalid preprocessing data"
  elif grep -qi "Running secrets generation because no suitable material" <<<"$text"; then
    echo "Advanced preprocessing or crypto setup required"
  elif grep -qi "Backend runtime not found" <<<"$text"; then
    echo "Backend binary not built in MP-SPDZ"
  elif grep -qi "MP-SPDZ compilation failed" <<<"$text"; then
    echo "MPC program compilation failed"
  elif grep -qi "Could not parse RESULT" <<<"$text"; then
    echo "Execution finished without parsable RESULT"
  elif grep -qi "No such file or directory" <<<"$text"; then
    echo "Missing file or script"
  elif grep -qi "Some MP-SPDZ parties failed" <<<"$text"; then
    echo "At least one MPC party failed"
  else
    echo "Unknown cause; inspect stdout/stderr and logs"
  fi
}

run_one_backend() {
  local backend="$1"

  local tmp_out tmp_err
  tmp_out="$(mktemp)"
  tmp_err="$(mktemp)"

  if ! prepare_env; then
    echo "$backend | FAIL | Environment preparation failed" | tee -a "$RESULTS_FILE"
    rm -f "$tmp_out" "$tmp_err"
    return
  fi

  "$BRIDGE" --backend "$backend" "$PROGRAM" >"$tmp_out" 2>"$tmp_err"
  local rc=$?

  local stdout_text stderr_text logs_text merged_text cause
  stdout_text="$(cat "$tmp_out")"
  stderr_text="$(cat "$tmp_err")"
  logs_text="$(collect_logs_text)"
  merged_text="${stdout_text}"$'\n'"${stderr_text}"$'\n'"${logs_text}"

  if [[ $rc -eq 0 ]]; then
    echo "$backend | PASS | OK" | tee -a "$RESULTS_FILE"
  else
    cause="$(detect_cause_from_text "$merged_text")"
    echo "$backend | FAIL | $cause" | tee -a "$RESULTS_FILE"

    print_line
    echo "Backend: $backend"
    echo "Cause: $cause"
    echo
    echo "--- stdout ---"
    cat "$tmp_out"
    echo
    echo "--- stderr ---"
    cat "$tmp_err"
    echo
    echo "--- logs ---"
    if [[ -n "$logs_text" ]]; then
      printf "%s\n" "$logs_text"
    else
      echo "No player logs found."
    fi
    print_line
  fi

  rm -f "$tmp_out" "$tmp_err"
}

main() {
  : > "$RESULTS_FILE"

  if [[ ! -x "$BRIDGE" ]]; then
    echo "Bridge binary not found: $BRIDGE"
    echo "Build first with: cmake --build build -j\"$(nproc)\""
    exit 1
  fi

  if [[ ! -x "$PROVIDER" || ! -x "$CONSENSUS" ]]; then
    echo "Provider or consensus binaries are missing."
    exit 1
  fi

  if [[ ! -f "$PROGRAM" ]]; then
    echo "Program not found: $PROGRAM"
    exit 1
  fi

  print_line
  echo "Testing validated backends with program: $PROGRAM"
  print_line

  local b
  for b in "${BACKENDS[@]}"; do
    run_one_backend "$b"
  done

  print_line
  echo "Summary written to: $RESULTS_FILE"
  cat "$RESULTS_FILE"
  print_line
}

main "$@"