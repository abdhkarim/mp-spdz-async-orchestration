# Async MPC Plan (Externalize All But Computation)

## Goal

Move to an architecture where MP-SPDZ is used only as the computation engine, while all asynchronous workflow is handled externally.

Target split:

- External plane: intake, buffering, ACK evidence, consensus, orchestration, failure policy.
- MP-SPDZ plane: run the selected computation on the decided batch.

## Current Status

- Done: `inputs` count is decoupled from `computation_nodes`.
- Done: input selection (core set) is outside MP-SPDZ.
- Missing:
  - explicit CN-signed ACK evidence (`k-of-n`, timeout, anti-replay),
  - auditable decision artifacts (`core_set.json`, `justification.json`),
  - a single orchestration state machine controlling the full session lifecycle.

## Execution Plan

### Phase 1 - Foundation (this iteration)

1. Introduce machine-readable artifacts:
   - `artifacts/acks/*.json`
   - `artifacts/core_set.json`
   - `artifacts/justification.json`
2. Define JSON schemas for ACK, core set, and justification.
3. Add an external orchestrator script with explicit states:
   - `COLLECTING -> DECIDED -> PREPARED -> RUNNING -> DONE/FAILED`
4. Keep current C++ binaries (`data_provider`, `consensus`, `spdz_bridge`) as execution backends.

Deliverable:
- One command launches a full round and writes auditable artifacts.

### Phase 2 - ACK-aware consensus

1. Add CN ACK emission (simulated first, then real).
2. Make consensus consume ACK artifacts and enforce:
   - distinct CNs,
   - `>= k` valid ACKs before deadline,
   - anti-replay checks.
3. Produce `core_set.json` and `justification.json` directly from consensus.

Deliverable:
- Core set decisions are justified by explicit ACK evidence.

### Phase 3 - Compute robustness orchestration

1. Add run supervision:
   - process monitoring,
   - timeout,
   - controlled stop.
2. Add rerun policy:
   - re-decide compute set if needed,
   - deterministic relaunch flow.

Deliverable:
- Crash/no-response during compute produces controlled recovery behavior.

### Phase 4 - Compute-only PoC aligned with docs

1. Implement two-program PoC:
   - `input_store.mpc` (prepare persistent MPC memory),
   - `compute_from_mem.mpc` with `-m old`.
2. Trigger compute only after external consensus marks batch as ready.

Deliverable:
- Demonstrable separation between preparation and computation phases.

## Acceptance Criteria

- Inputs can arrive asynchronously and be buffered externally.
- Core set is decided with explicit artifacts and reproducible logic.
- MP-SPDZ runs with fixed computation nodes, independent of input count.
- End-to-end run logs show state transitions and final result.
- Failure path (at least one) is handled without silent blocking.

## Short-Term Next Actions

1. Use `scripts/async_orchestrator.py` for all demos.
2. Start storing ACK artifacts under `artifacts/acks/`.
3. Extend consensus to read ACKs and write JSON decision artifacts.
4. Add one scripted crash scenario for compute rerun demonstration.
