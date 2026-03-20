#!/usr/bin/env python3
"""
External asynchronous orchestrator (Phase 1).

This script centralizes round control and artifacts while delegating computation
to existing binaries:
  - build/node/data_provider
  - build/consensus/consensus
  - build/spdz_bridge/spdz_bridge

It implements an explicit state machine:
  COLLECTING -> DECIDED -> PREPARED -> RUNNING -> DONE/FAILED
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List


@dataclass(frozen=True)
class ProviderValue:
    provider_id: int
    value: int


class OrchestratorError(RuntimeError):
    pass


def run_command(args: List[str], cwd: Path, env: dict | None = None) -> None:
    print(f"[cmd] {' '.join(args)}")
    result = subprocess.run(args, cwd=str(cwd), env=env)
    if result.returncode != 0:
        raise OrchestratorError(f"Command failed ({result.returncode}): {' '.join(args)}")


def run_command_capture(args: List[str], cwd: Path, env: dict | None = None) -> str:
    result = subprocess.run(args, cwd=str(cwd), env=env, capture_output=True, text=True)
    if result.returncode != 0:
        raise OrchestratorError(
            f"Command failed ({result.returncode}): {' '.join(args)}\n{result.stderr.strip()}"
        )
    return result.stdout.strip()


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def compute_file_hash(path: Path) -> str:
    return hashlib.blake2b(path.read_bytes(), digest_size=32).hexdigest()


def ack_signing_message(
    session_id: str,
    round_id: int,
    provider_id: int,
    computation_node_id: int,
    input_hash: str,
    timestamp_unix_ms: int,
) -> str:
    return (
        f"{session_id}|{round_id}|{provider_id}|{computation_node_id}|"
        f"{input_hash}|{timestamp_unix_ms}"
    )


def read_core_set(core_set_path: Path) -> List[int]:
    if not core_set_path.exists():
        return []
    provider_ids: List[int] = []
    for line in core_set_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        provider_ids.append(int(line))
    return sorted(set(provider_ids))


def parse_provider_values(raw: str) -> List[ProviderValue]:
    pairs: List[ProviderValue] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        parts = item.split(":")
        if len(parts) != 2:
            raise OrchestratorError(
                f"Invalid provider pair '{item}'. Expected format: id:value"
            )
        provider_id = int(parts[0].strip())
        value = int(parts[1].strip())
        if provider_id <= 0:
            raise OrchestratorError("Provider id must be >= 1")
        pairs.append(ProviderValue(provider_id=provider_id, value=value))
    if not pairs:
        raise OrchestratorError("No providers specified")
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one async-orchestrated MPC round (Phase 1)."
    )
    parser.add_argument(
        "--providers",
        required=True,
        help="Comma-separated id:value list, e.g. 1:10,2:20,3:30",
    )
    parser.add_argument(
        "--backend",
        default="semi2k",
        help="MP-SPDZ backend passed to spdz_bridge (default: semi2k)",
    )
    parser.add_argument(
        "--computation-nodes",
        type=int,
        default=3,
        help="Number of MP-SPDZ computation nodes (default: 3)",
    )
    parser.add_argument(
        "--session-id",
        default="demo-session",
        help="Logical session id used in artifacts (default: demo-session)",
    )
    parser.add_argument(
        "--k-acks",
        type=int,
        default=2,
        help="Distinct CN ACK threshold per provider (default: 2)",
    )
    parser.add_argument(
        "--ack-nodes",
        type=int,
        default=0,
        help="How many CN IDs emit ACKs (0 => computation_nodes)",
    )
    parser.add_argument(
        "--ack-timeout-seconds",
        type=int,
        default=0,
        help="Consensus ACK freshness window in seconds (0 disables time check)",
    )
    parser.add_argument(
        "--scenario",
        choices=["normal", "insufficient-acks", "replay-ack", "hash-mismatch", "stale-ack"],
        default="normal",
        help="Generate a deterministic negative-path test scenario",
    )
    parser.add_argument(
        "--round-id",
        type=int,
        default=0,
        help="Logical round id used in artifacts (default: 0)",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove inputs/core_set/logs/artifacts before run",
    )
    args = parser.parse_args()

    if args.computation_nodes < 2:
        raise OrchestratorError("--computation-nodes must be >= 2")
    if args.k_acks < 1:
        raise OrchestratorError("--k-acks must be >= 1")
    if args.ack_timeout_seconds < 0:
        raise OrchestratorError("--ack-timeout-seconds must be >= 0")

    repo_root = Path(__file__).resolve().parents[1]
    build_dir = repo_root / "build"
    artifacts_dir = repo_root / "artifacts"
    inputs_dir = repo_root / "inputs"
    logs_dir = repo_root / "logs"
    core_set_path = repo_root / "core_set.txt"

    providers = parse_provider_values(args.providers)
    min_inputs = len(providers)
    now_ms = int(time.time() * 1000)
    ack_nodes = args.computation_nodes if args.ack_nodes == 0 else args.ack_nodes
    if ack_nodes < args.k_acks:
        raise OrchestratorError("--ack-nodes must be >= --k-acks")

    state = "COLLECTING"
    print(f"[state] {state}")

    if args.clean:
        for path in (inputs_dir, logs_dir, artifacts_dir):
            if path.exists():
                shutil.rmtree(path)
        if core_set_path.exists():
            core_set_path.unlink()

    inputs_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)
    artifacts_dir.mkdir(parents=True, exist_ok=True)

    run_meta = {
        "session_id": args.session_id,
        "round_id": args.round_id,
        "started_at_unix_ms": now_ms,
        "backend": args.backend,
        "computation_nodes": args.computation_nodes,
        "providers": [p.__dict__ for p in providers],
    }
    write_json(artifacts_dir / "run_meta.json", run_meta)

    for p in providers:
        run_command(
            [str(build_dir / "node" / "data_provider"), str(p.provider_id), str(p.value)],
            cwd=repo_root,
            env=os.environ.copy(),
        )

    acks_dir = artifacts_dir / "acks"
    cn_keys_dir = artifacts_dir / "cn_keys"
    acks_dir.mkdir(parents=True, exist_ok=True)
    cn_keys_dir.mkdir(parents=True, exist_ok=True)

    ack_crypto_tool = build_dir / "consensus" / "ack_crypto_tool"
    if not ack_crypto_tool.exists():
        raise OrchestratorError(
            f"Missing {ack_crypto_tool}. Rebuild with target ack_crypto_tool."
        )

    for cn_id in range(ack_nodes):
        pub_file = cn_keys_dir / f"cn_{cn_id}.pub.hex"
        sec_file = cn_keys_dir / f"cn_{cn_id}.sec.hex"
        if not (pub_file.exists() and sec_file.exists()):
            run_command(
                [
                    str(ack_crypto_tool),
                    "gen-keypair",
                    str(pub_file),
                    str(sec_file),
                ],
                cwd=repo_root,
                env=os.environ.copy(),
            )

    for p in providers:
        provider_file = inputs_dir / f"provider_{p.provider_id}.txt"
        input_hash = compute_file_hash(provider_file)
        provider_ack_nodes = ack_nodes
        if args.scenario == "insufficient-acks" and p.provider_id == providers[-1].provider_id:
            provider_ack_nodes = max(0, args.k_acks - 1)
        for cn_id in range(provider_ack_nodes):
            ts = int(time.time() * 1000)
            if args.scenario == "stale-ack" and p.provider_id == providers[-1].provider_id:
                ts = ts - max(1, args.ack_timeout_seconds + 5) * 1000
            ack_hash = input_hash
            if args.scenario == "hash-mismatch" and p.provider_id == providers[-1].provider_id and cn_id == 0:
                ack_hash = "0" * 64
            ack_payload = {
                "session_id": args.session_id,
                "round_id": args.round_id,
                "provider_id": p.provider_id,
                "computation_node_id": cn_id,
                "input_hash": ack_hash,
                "timestamp_unix_ms": ts,
            }
            sec_file = cn_keys_dir / f"cn_{cn_id}.sec.hex"
            msg = ack_signing_message(
                args.session_id,
                args.round_id,
                p.provider_id,
                cn_id,
                ack_hash,
                ts,
            )
            signature = run_command_capture(
                [str(ack_crypto_tool), "sign", str(sec_file), msg],
                cwd=repo_root,
                env=os.environ.copy(),
            )
            ack_payload["signature"] = signature
            write_json(
                acks_dir / f"ack_p{p.provider_id}_cn{cn_id}.json",
                ack_payload,
            )
            if args.scenario == "replay-ack" and p.provider_id == providers[-1].provider_id and cn_id == 0:
                write_json(
                    acks_dir / f"ack_p{p.provider_id}_cn{cn_id}_replay.json",
                    ack_payload,
                )

    state = "DECIDED"
    print(f"[state] {state}")
    run_command(
        [
            str(build_dir / "consensus" / "consensus"),
            str(min_inputs),
            "--acks-dir",
            str(acks_dir),
            "--k",
            str(args.k_acks),
            "--session-id",
            args.session_id,
            "--round-id",
            str(args.round_id),
            "--timeout-seconds",
            str(args.ack_timeout_seconds),
            "--artifacts-dir",
            str(artifacts_dir),
            "--cn-keys-dir",
            str(cn_keys_dir),
        ],
        cwd=repo_root,
        env=os.environ.copy(),
    )

    core_set = read_core_set(core_set_path)
    if not core_set:
        raise OrchestratorError("Consensus produced an empty core_set.txt")

    state = "PREPARED"
    print(f"[state] {state}")

    state = "RUNNING"
    print(f"[state] {state}")
    run_command(
        [
            str(build_dir / "spdz_bridge" / "spdz_bridge"),
            "--backend",
            args.backend,
            "--computation-nodes",
            str(args.computation_nodes),
        ],
        cwd=repo_root,
        env=os.environ.copy(),
    )

    state = "DONE"
    print(f"[state] {state}")
    print("[ok] Round completed. Artifacts written to artifacts/")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OrchestratorError as exc:
        print(f"[state] FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
