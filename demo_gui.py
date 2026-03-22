#!/usr/bin/env python3
"""Tkinter GUI for the async MPC orchestration prototype.

Tabs:
  - Manual: Zone 1 (providers) → Zone 2 (file consensus) → Zone 3 (semi2k bridge)
  - ACK orchestrator: scripts/async_orchestrator.py (ACKs, artifacts, CN keys)
  - Scenarios: tampering, late provider, masking, crash (+ optional MPC matrix)
  - Integration: full_system_validation_wsl.sh

Run from the repository root (where build/, programs/, scripts/ live).
"""

from __future__ import annotations

import os
import queue
import re
import shlex
import shutil
import subprocess
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, scrolledtext, ttk

PROJECT_ROOT = Path.cwd()
BUILD_DIR = PROJECT_ROOT / "build"
PROGRAMS_DIR = PROJECT_ROOT / "programs"
SCRIPTS_DIR = PROJECT_ROOT / "scripts"
RUN_CWD = PROJECT_ROOT

ID_REGEX = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
INT_REGEX = re.compile(r"^[+-]?\d{1,64}$")

ORCH_SCENARIOS = (
    "normal",
    "insufficient-acks",
    "replay-ack",
    "hash-mismatch",
    "stale-ack",
)


def to_wsl_path(path: Path) -> str:
    text = str(path)
    if re.match(r"^[A-Za-z]:\\", text):
        drive = text[0].lower()
        rest = text[2:].replace("\\", "/")
        return f"/mnt/{drive}{rest}"
    return text.replace("\\", "/")


def validate_provider_inputs(provider_id: str, value: str) -> tuple[bool, str]:
    if not ID_REGEX.fullmatch(provider_id):
        return False, "Invalid provider ID (1–32 chars: letters, digits, _ or -)."
    if not INT_REGEX.fullmatch(value):
        return False, "Invalid value (signed integer, max 64 characters)."
    return True, ""


def discover_mpc_programs() -> list[Path]:
    if not PROGRAMS_DIR.is_dir():
        return []
    return sorted(PROGRAMS_DIR.glob("*.mpc"))


class App:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("MPC async orchestration — demo")
        self.root.minsize(760, 560)
        self.msg_queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.busy = False

        self.var_use_wsl = tk.IntVar(value=1 if os.name == "nt" else 0)
        self.var_status = tk.StringVar(value="Idle")
        self.var_result = tk.StringVar(value="MPC result: —")
        self.var_core_set = tk.StringVar(value="Core set: —")
        self.var_clean_inputs = tk.IntVar(value=1)
        self.var_bridge_extra = tk.StringVar(value="")

        # Orchestrator
        self.var_orch_providers = tk.StringVar(value="1:10,2:20,3:30,4:40,5:50")
        self.var_orch_session = tk.StringVar(value="demo-gui")
        self.var_orch_round = tk.StringVar(value="0")
        self.var_orch_clean = tk.IntVar(value=1)

        self._mpc_files = discover_mpc_programs()
        self._buttons: list[ttk.Button] = []

        self._build_ui()
        self.root.after(100, self._drain_queue)

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=8)
        outer.pack(fill=tk.BOTH, expand=True)

        top = ttk.Frame(outer)
        top.pack(fill=tk.X, pady=(0, 4))
        ttk.Checkbutton(
            top,
            text="Run commands in WSL (required on Windows if build lives in WSL)",
            variable=self.var_use_wsl,
        ).pack(anchor=tk.W)
        ttk.Label(
            top,
            text="CWD must be the repository root. Architecture: Zone 1 = providers, Zone 2 = consensus, Zone 3 = semi2k bridge.",
            foreground="#444",
        ).pack(anchor=tk.W)

        nb = ttk.Notebook(outer)
        nb.pack(fill=tk.BOTH, expand=True, pady=4)

        self._tab_manual = ttk.Frame(nb, padding=6)
        self._tab_orch = ttk.Frame(nb, padding=6)
        self._tab_scen = ttk.Frame(nb, padding=6)
        self._tab_integ = ttk.Frame(nb, padding=6)
        nb.add(self._tab_manual, text="Manual (zones 1→3)")
        nb.add(self._tab_orch, text="ACK orchestrator")
        nb.add(self._tab_scen, text="Security scenarios")
        nb.add(self._tab_integ, text="Integration")

        self._build_tab_manual(self._tab_manual)
        self._build_tab_orchestrator(self._tab_orch)
        self._build_tab_scenarios(self._tab_scen)
        self._build_tab_integration(self._tab_integ)

        stat = ttk.Frame(outer)
        stat.pack(fill=tk.X, pady=4)
        ttk.Label(stat, textvariable=self.var_status, foreground="#0066cc").pack(side=tk.LEFT, padx=(0, 12))
        ttk.Label(stat, textvariable=self.var_core_set).pack(side=tk.LEFT, padx=(0, 12))
        ttk.Label(stat, textvariable=self.var_result).pack(side=tk.LEFT)

        self.txt = scrolledtext.ScrolledText(outer, width=96, height=20, wrap=tk.WORD, font=("Consolas", 9))
        self.txt.pack(fill=tk.BOTH, expand=True, pady=4)

    def _build_tab_manual(self, tab: ttk.Frame) -> None:
        ttk.Label(
            tab,
            text="File-based consensus (no ACK JSON). Use the ACK tab for CN ACKs + artifacts.",
            foreground="#555",
        ).pack(anchor=tk.W, pady=(0, 8))

        lf_p = ttk.LabelFrame(tab, text="Zone 1 — Data provider", padding=8)
        lf_p.pack(fill=tk.X, pady=4)
        row0 = ttk.Frame(lf_p)
        row0.pack(fill=tk.X)
        ttk.Label(row0, text="Provider ID:").pack(side=tk.LEFT, padx=(0, 6))
        self.entry_id = ttk.Entry(row0, width=8)
        self.entry_id.pack(side=tk.LEFT, padx=(0, 14))
        self.entry_id.insert(0, "1")
        ttk.Label(row0, text="Value:").pack(side=tk.LEFT, padx=(0, 6))
        self.entry_val = ttk.Entry(row0, width=12)
        self.entry_val.pack(side=tk.LEFT, padx=(0, 14))
        self.entry_val.insert(0, "10")
        ttk.Label(row0, text="Computation parties:").pack(side=tk.LEFT, padx=(0, 6))
        self.spin_parties = ttk.Spinbox(row0, from_=2, to=16, width=4)
        self.spin_parties.pack(side=tk.LEFT)
        self.spin_parties.delete(0, tk.END)
        self.spin_parties.insert(0, "3")
        self._btn_provider = ttk.Button(lf_p, text="Run provider", command=self.on_provider)
        self._btn_provider.pack(anchor=tk.W, pady=(8, 0))
        self._buttons.append(self._btn_provider)

        lf_c = ttk.LabelFrame(tab, text="Zone 2 — Consensus (core set, file proofs)", padding=8)
        lf_c.pack(fill=tk.X, pady=4)
        row1 = ttk.Frame(lf_c)
        row1.pack(fill=tk.X)
        ttk.Label(row1, text="Minimum valid inputs:").pack(side=tk.LEFT, padx=(0, 6))
        self.spin_quorum = ttk.Spinbox(row1, from_=1, to=32, width=4)
        self.spin_quorum.pack(side=tk.LEFT, padx=(0, 14))
        self.spin_quorum.delete(0, tk.END)
        self.spin_quorum.insert(0, "3")
        ttk.Checkbutton(row1, text="Clean stale provider files (--clean-inputs)", variable=self.var_clean_inputs).pack(
            side=tk.LEFT
        )
        self._btn_consensus = ttk.Button(lf_c, text="Run consensus", command=self.on_consensus)
        self._btn_consensus.pack(anchor=tk.W, pady=(8, 0))
        self._buttons.append(self._btn_consensus)

        lf_b = ttk.LabelFrame(tab, text="Zone 3 — semi2k bridge", padding=8)
        lf_b.pack(fill=tk.X, pady=4)
        row2 = ttk.Frame(lf_b)
        row2.pack(fill=tk.X)
        ttk.Label(row2, text="Program:").pack(side=tk.LEFT, padx=(0, 6))
        displays = [f"{p.stem}  ({p.name})" for p in self._mpc_files]
        self.cmb_program = ttk.Combobox(
            row2,
            width=40,
            state="readonly" if displays else "disabled",
            values=displays or ("(no programs/*.mpc)",),
        )
        self.cmb_program.pack(side=tk.LEFT, padx=(0, 10))
        if displays:
            idx = next((i for i, p in enumerate(self._mpc_files) if p.stem == "sum"), 0)
            self.cmb_program.current(idx)
        else:
            self.cmb_program.current(0)
        ttk.Label(row2, text="Extra args:").pack(side=tk.LEFT, padx=(0, 4))
        ttk.Entry(row2, textvariable=self.var_bridge_extra, width=28).pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._btn_bridge = ttk.Button(lf_b, text="Run bridge", command=self.on_bridge)
        self._btn_bridge.pack(anchor=tk.W, pady=(8, 0))
        self._buttons.append(self._btn_bridge)

        lf_q = ttk.LabelFrame(tab, text="Quick: happy path", padding=8)
        lf_q.pack(fill=tk.X, pady=4)
        qr = ttk.Frame(lf_q)
        qr.pack(fill=tk.X)
        self._btn_full = ttk.Button(
            qr,
            text="3 providers → consensus → bridge (uses settings above)",
            command=self.on_full_scenario,
        )
        self._btn_full.pack(side=tk.LEFT, padx=(0, 8))
        self._btn_reset = ttk.Button(tab, text="Reset workspace", command=self.on_reset)
        self._btn_reset.pack(anchor=tk.E, pady=4)
        self._buttons.extend([self._btn_full, self._btn_reset])

    def _build_tab_orchestrator(self, tab: ttk.Frame) -> None:
        ttk.Label(
            tab,
            text="Runs scripts/async_orchestrator.py: ACK generation (ack_crypto_tool), consensus with --acks-dir, then bridge. "
            "Requires build/consensus/ack_crypto_tool.",
            foreground="#333",
            wraplength=700,
        ).pack(anchor=tk.W, pady=(0, 8))

        gf = ttk.Frame(tab)
        gf.pack(fill=tk.X)
        ttk.Label(gf, text="Providers (id:value,…):").grid(row=0, column=0, sticky=tk.W, padx=2, pady=4)
        ttk.Entry(gf, textvariable=self.var_orch_providers, width=52).grid(row=0, column=1, columnspan=3, sticky=tk.EW, pady=4)
        ttk.Label(gf, text="Session ID:").grid(row=1, column=0, sticky=tk.W, padx=2, pady=4)
        ttk.Entry(gf, textvariable=self.var_orch_session, width=24).grid(row=1, column=1, sticky=tk.W, pady=4)
        ttk.Label(gf, text="Round ID:").grid(row=1, column=2, sticky=tk.E, padx=8)
        ttk.Entry(gf, textvariable=self.var_orch_round, width=8).grid(row=1, column=3, sticky=tk.W)

        self.spin_orch_cn = ttk.Spinbox(gf, from_=2, to=16, width=4)
        self.spin_orch_k = ttk.Spinbox(gf, from_=1, to=16, width=4)
        self.spin_orch_ackn = ttk.Spinbox(gf, from_=1, to=16, width=4)
        self.spin_orch_timeout = ttk.Spinbox(gf, from_=0, to=3600, width=4)
        self.cmb_orch_scenario = ttk.Combobox(gf, values=ORCH_SCENARIOS, state="readonly", width=18)

        ttk.Label(gf, text="Computation nodes:").grid(row=2, column=0, sticky=tk.W, pady=4)
        self.spin_orch_cn.grid(row=2, column=1, sticky=tk.W, pady=4)
        self.spin_orch_cn.delete(0, tk.END)
        self.spin_orch_cn.insert(0, "3")
        ttk.Label(gf, text="k-acks:").grid(row=2, column=2, sticky=tk.E, padx=8)
        self.spin_orch_k.grid(row=2, column=3, sticky=tk.W)
        self.spin_orch_k.delete(0, tk.END)
        self.spin_orch_k.insert(0, "2")

        ttk.Label(gf, text="ACK nodes:").grid(row=3, column=0, sticky=tk.W, pady=4)
        self.spin_orch_ackn.grid(row=3, column=1, sticky=tk.W)
        self.spin_orch_ackn.delete(0, tk.END)
        self.spin_orch_ackn.insert(0, "3")
        ttk.Label(gf, text="ACK timeout (s):").grid(row=3, column=2, sticky=tk.E, padx=8)
        self.spin_orch_timeout.grid(row=3, column=3, sticky=tk.W)
        self.spin_orch_timeout.delete(0, tk.END)
        self.spin_orch_timeout.insert(0, "2")

        ttk.Label(gf, text="Scenario:").grid(row=4, column=0, sticky=tk.W, pady=4)
        self.cmb_orch_scenario.grid(row=4, column=1, sticky=tk.W)
        self.cmb_orch_scenario.set("normal")

        ttk.Checkbutton(gf, text="--clean (wipe inputs/logs/artifacts before run)", variable=self.var_orch_clean).grid(
            row=5, column=0, columnspan=2, sticky=tk.W, pady=4
        )

        gf.columnconfigure(1, weight=1)

        self._btn_orch = ttk.Button(tab, text="Run ACK orchestrator round", command=self.on_run_orchestrator)
        self._btn_orch.pack(anchor=tk.W, pady=10)
        self._buttons.append(self._btn_orch)

    def _build_tab_scenarios(self, tab: ttk.Frame) -> None:
        ttk.Label(
            tab,
            text="Preset flows matching scripts/full_system_validation_wsl.sh (subset). "
            "Each run resets inputs/logs/core_set unless noted.",
            foreground="#333",
            wraplength=720,
        ).pack(anchor=tk.W, pady=(0, 8))

        def add_scenario(title: str, desc: str, cmd) -> None:
            lf = ttk.LabelFrame(tab, text=title, padding=8)
            lf.pack(fill=tk.X, pady=6)
            ttk.Label(lf, text=desc, foreground="#555", wraplength=700).pack(anchor=tk.W)
            b = ttk.Button(lf, text="Run", command=cmd)
            b.pack(anchor=tk.W, pady=(6, 0))
            self._buttons.append(b)

        add_scenario(
            "Tampering — BLAKE2b proof mismatch (Zone 2 rejects)",
            "Three honest providers, then masked_value of provider 2 is corrupted; consensus must exit non-zero "
            "and report invalid proof.",
            self.on_scenario_tampering,
        )
        add_scenario(
            "Late provider — excluded from core set",
            "Providers 1 & 2, consensus with min=2, then provider 3 submits; core_set.txt must list only 1 and 2.",
            self.on_scenario_late_provider,
        )
        add_scenario(
            "Masking — no plaintext in inputs (Zone 1)",
            "Same secret MPC_PROVIDER_SECRET; three providers with value 42; checks masked_value≠plain and share files exist.",
            self.on_scenario_masking,
        )
        add_scenario(
            "Crash — absent provider",
            "Only providers 1 & 2 (parties=2), quorum 2, bridge sum expects 300.",
            self.on_scenario_crash,
        )
        add_scenario(
            "MPC programs — smoke all programs/*.mpc",
            "For each program: fresh workspace, providers 7,15,20, consensus 3, bridge. Takes several minutes.",
            self.on_scenario_mpc_matrix,
        )

    def _build_tab_integration(self, tab: ttk.Frame) -> None:
        ttk.Label(
            tab,
            text="Runs the full bash harness: ACK scenarios, tampering, late provider, masking, semi2k matrix, crash. "
            "Requires bash, cmake-built targets, MP-SPDZ semi2k-party.x, and typically WSL on Windows.",
            foreground="#333",
            wraplength=720,
        ).pack(anchor=tk.W, pady=(0, 8))
        self._btn_fullsuite = ttk.Button(
            tab,
            text="Run scripts/full_system_validation_wsl.sh",
            command=self.on_run_full_validation,
        )
        self._btn_fullsuite.pack(anchor=tk.W, pady=8)
        self._buttons.append(self._btn_fullsuite)

    @staticmethod
    def _label_for_mpc(p: Path) -> str:
        return f"{p.stem}  ({p.name})"

    def _parties_str(self) -> str:
        return self.spin_parties.get().strip() or "3"

    def _selected_program_posix(self) -> str | None:
        if not self._mpc_files:
            return None
        i = self.cmb_program.current()
        if i < 0 or i >= len(self._mpc_files):
            return self._mpc_files[0].relative_to(PROJECT_ROOT).as_posix()
        return self._mpc_files[i].relative_to(PROJECT_ROOT).as_posix()

    def append(self, text: str) -> None:
        self.txt.insert(tk.END, text)
        self.txt.see(tk.END)

    def set_busy(self, value: bool) -> None:
        self.busy = value
        state = "disabled" if value else "normal"
        for btn in self._buttons:
            btn.configure(state=state)
        self.var_status.set("Running…" if value else "Idle")

    def _native_run(self, args: list[str], env: dict[str, str] | None = None) -> tuple[int, str]:
        e = os.environ.copy()
        if env:
            e.update(env)
        completed = subprocess.run(
            args,
            cwd=RUN_CWD,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            env=e,
        )
        return completed.returncode, completed.stdout

    def _wsl_run(self, args: list[str], env: dict[str, str] | None = None) -> tuple[int, str]:
        repo_wsl = to_wsl_path(RUN_CWD)
        prefix = ""
        if env:
            for k, v in env.items():
                prefix += f"export {k}={shlex.quote(v)}; "
        inner = " ".join(shlex.quote(a) for a in args)
        command = prefix + "cd " + shlex.quote(repo_wsl) + " && " + inner
        completed = subprocess.run(
            ["wsl", "-e", "bash", "-lc", command],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        return completed.returncode, completed.stdout

    def run_cmd(
        self,
        args: list[str],
        title: str,
        env: dict[str, str] | None = None,
    ) -> tuple[int, str]:
        self.append(f"$ {' '.join(args)}\n")
        if self.var_use_wsl.get():
            rc, out = self._wsl_run(args, env=env)
        else:
            rc, out = self._native_run(args, env=env)
        self.append(out + ("\n" if not out.endswith("\n") else ""))
        self.append(f"[{title}] exit={rc}\n\n")
        self._update_summary_from_output(out)
        return rc, out

    def _update_summary_from_output(self, out: str) -> None:
        m_sum = re.search(
            r"MP-SPDZ result:\s*(?:(?:SUM|RESULT)=)?\s*([-]?\d+)",
            out,
        )
        if m_sum:
            self.var_result.set(f"MPC result: {m_sum.group(1)}")
        m_core = re.search(r"Core set decided with \d+ provider\(s\):\s*(.*)", out)
        if m_core:
            self.var_core_set.set(f"Core set: {m_core.group(1).strip()}")

    def _drain_queue(self) -> None:
        try:
            while True:
                kind, payload = self.msg_queue.get_nowait()
                if kind == "done":
                    self.set_busy(False)
                elif kind == "error":
                    self.set_busy(False)
                    messagebox.showerror("Execution error", payload)
        except queue.Empty:
            pass
        finally:
            self.root.after(100, self._drain_queue)

    def _run_async(self, fn) -> None:
        if self.busy:
            messagebox.showinfo("Busy", "A command is already running.")
            return
        if not BUILD_DIR.exists():
            messagebox.showerror("Build missing", f"Build directory not found:\n{BUILD_DIR}")
            return
        self.set_busy(True)

        def worker() -> None:
            try:
                fn()
                self.msg_queue.put(("done", ""))
            except Exception as exc:  # noqa: BLE001
                self.msg_queue.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def _clean_workspace_dirs(self) -> None:
        for name in ("inputs", "logs", "artifacts", "provider_secrets"):
            p = RUN_CWD / name
            if p.exists():
                shutil.rmtree(p, ignore_errors=True)
        core = RUN_CWD / "core_set.txt"
        if core.exists():
            core.unlink()
        (RUN_CWD / "inputs").mkdir(parents=True, exist_ok=True)
        (RUN_CWD / "logs").mkdir(parents=True, exist_ok=True)
        (RUN_CWD / "artifacts").mkdir(parents=True, exist_ok=True)
        (RUN_CWD / "provider_secrets").mkdir(parents=True, exist_ok=True)
        self.append("[clean] Workspace directories reset (local).\n\n")

    def _tamper_provider_masked_value(self, provider_id: int, fake: str = "999999999999") -> None:
        p = RUN_CWD / "inputs" / f"provider_{provider_id}.txt"
        if not p.is_file():
            raise FileNotFoundError(f"Expected provider file: {p}")
        lines = p.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if line.startswith("masked_value="):
                lines[i] = f"masked_value={fake}"
                break
        else:
            raise ValueError("masked_value= line not found")
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.append(f"[tamper] Set masked_value={fake} in {p.name}\n\n")

    # --- Manual handlers ---
    def on_provider(self) -> None:
        provider_id = self.entry_id.get().strip()
        value = self.entry_val.get().strip()
        ok, err = validate_provider_inputs(provider_id, value)
        if not ok:
            messagebox.showwarning("Input", err)
            return
        n = self._parties_str()

        def task() -> None:
            self.run_cmd(
                ["./build/node/data_provider", provider_id, value, "--computation-nodes", n],
                "provider",
            )

        self._run_async(task)

    def on_consensus(self) -> None:
        try:
            min_v = int(self.spin_quorum.get().strip())
            if min_v < 1:
                raise ValueError
        except ValueError:
            messagebox.showwarning("Input", "Minimum valid inputs must be a positive integer.")
            return

        def task() -> None:
            args: list[str] = ["./build/consensus/consensus", str(min_v)]
            if self.var_clean_inputs.get():
                args.append("--clean-inputs")
            self.run_cmd(args, "consensus")

        self._run_async(task)

    def on_bridge(self) -> None:
        prog = self._selected_program_posix()
        if not prog:
            messagebox.showerror("Programs", f"No .mpc files under:\n{PROGRAMS_DIR}")
            return
        n = self._parties_str()
        extra = self.var_bridge_extra.get().strip()
        extra_args = shlex.split(extra) if extra else []

        def task() -> None:
            self.run_cmd(
                ["./build/spdz_bridge/spdz_bridge", "--computation-nodes", n, prog, *extra_args],
                "bridge",
            )

        self._run_async(task)

    def on_reset(self) -> None:
        if not messagebox.askyesno("Confirm", "Delete inputs/, logs/, core_set.txt, provider_secrets/, artifacts/?"):
            return

        def task() -> None:
            if self.var_use_wsl.get():
                self.run_cmd(
                    [
                        "bash",
                        "-lc",
                        "rm -rf inputs logs core_set.txt provider_secrets artifacts && mkdir -p inputs logs artifacts provider_secrets",
                    ],
                    "reset",
                )
            else:
                self._clean_workspace_dirs()

        self._run_async(task)

    def on_full_scenario(self) -> None:
        prog = self._selected_program_posix()
        if not prog:
            messagebox.showerror("Programs", f"No .mpc files under:\n{PROGRAMS_DIR}")
            return
        try:
            min_v = int(self.spin_quorum.get().strip())
        except ValueError:
            min_v = 3
        n = self._parties_str()
        extra = self.var_bridge_extra.get().strip()
        extra_bridge = shlex.split(extra) if extra else []

        def task() -> None:
            self.var_result.set("MPC result: —")
            self.var_core_set.set("Core set: —")
            for pid, val in (("1", "10"), ("2", "3"), ("3", "1")):
                self.run_cmd(
                    ["./build/node/data_provider", pid, val, "--computation-nodes", n],
                    "provider",
                )
            cargs: list[str] = ["./build/consensus/consensus", str(min_v)]
            if self.var_clean_inputs.get():
                cargs.append("--clean-inputs")
            rc_c, _ = self.run_cmd(cargs, "consensus")
            if rc_c != 0:
                self.append("[scenario] Stopped at consensus.\n\n")
                return
            self.run_cmd(
                ["./build/spdz_bridge/spdz_bridge", "--computation-nodes", n, prog, *extra_bridge],
                "bridge",
            )

        self._run_async(task)

    def _require_ack_tool(self) -> bool:
        tool = BUILD_DIR / "consensus" / "ack_crypto_tool"
        if not tool.exists():
            messagebox.showerror(
                "Missing binary",
                f"ack_crypto_tool not found:\n{tool}\n\nBuild with: cmake --build build --target ack_crypto_tool",
            )
            return False
        return True

    def on_run_orchestrator(self) -> None:
        if not self._require_ack_tool():
            return
        prov = self.var_orch_providers.get().strip()
        if not prov:
            messagebox.showwarning("Input", "Set --providers.")
            return
        try:
            cn = int(self.spin_orch_cn.get().strip())
            k = int(self.spin_orch_k.get().strip())
            ackn = int(self.spin_orch_ackn.get().strip())
            to = int(self.spin_orch_timeout.get().strip())
            rid = int(self.var_orch_round.get().strip())
        except ValueError:
            messagebox.showwarning("Input", "Numeric fields must be integers.")
            return
        session = self.var_orch_session.get().strip() or "demo-gui"
        scen = self.cmb_orch_scenario.get().strip() or "normal"

        def task() -> None:
            args = [
                "python3",
                "scripts/async_orchestrator.py",
                "--providers",
                prov,
                "--session-id",
                session,
                "--round-id",
                str(rid),
                "--computation-nodes",
                str(cn),
                "--k-acks",
                str(k),
                "--ack-nodes",
                str(ackn),
                "--ack-timeout-seconds",
                str(to),
                "--scenario",
                scen,
            ]
            if self.var_orch_clean.get():
                args.append("--clean")
            self.run_cmd(args, "orchestrator")

        self._run_async(task)

    def on_scenario_tampering(self) -> None:
        def task() -> None:
            self._clean_workspace_dirs()
            n = "3"
            self.run_cmd(["./build/node/data_provider", "1", "11", "--computation-nodes", n], "provider")
            self.run_cmd(["./build/node/data_provider", "2", "22", "--computation-nodes", n], "provider")
            self.run_cmd(["./build/node/data_provider", "3", "33", "--computation-nodes", n], "provider")
            self._tamper_provider_masked_value(2)
            rc, out = self.run_cmd(["./build/consensus/consensus", "3"], "consensus")
            ok = rc != 0 and "invalid cryptographic proof" in out
            self.append(f"[scenario tampering] Expected rejection: {'OK' if ok else 'CHECK MANUALLY'}\n\n")

        self._run_async(task)

    def on_scenario_late_provider(self) -> None:
        def task() -> None:
            self._clean_workspace_dirs()
            n = "3"
            self.run_cmd(["./build/node/data_provider", "1", "5", "--computation-nodes", n], "provider")
            self.run_cmd(["./build/node/data_provider", "2", "6", "--computation-nodes", n], "provider")
            self.run_cmd(["./build/consensus/consensus", "2"], "consensus")
            self.run_cmd(["./build/node/data_provider", "3", "7", "--computation-nodes", n], "provider")
            cs = RUN_CWD / "core_set.txt"
            text = cs.read_text(encoding="utf-8") if cs.is_file() else ""
            has1 = "1" in text.split()
            has2 = "2" in text.split()
            has3 = "3" in text.split()
            ok = has1 and has2 and not has3
            self.append(
                f"[scenario late-provider] core_set.txt:\n{text}\n"
                f"Expect {{1,2}} without 3: {'OK' if ok else 'FAIL'}\n\n"
            )

        self._run_async(task)

    def on_scenario_masking(self) -> None:
        def task() -> None:
            self._clean_workspace_dirs()
            secret = os.environ.get("MPC_PROVIDER_SECRET", "mpc-demo-secret")
            env = {"MPC_PROVIDER_SECRET": secret}
            n = "3"
            for pid in ("1", "2", "3"):
                self.run_cmd(
                    ["./build/node/data_provider", pid, "42", "--computation-nodes", n],
                    "provider",
                    env=env,
                )
            self.run_cmd(["./build/consensus/consensus", "3"], "consensus", env=env)
            bad = False
            for pid in ("1", "2", "3"):
                p = RUN_CWD / "inputs" / f"provider_{pid}.txt"
                if p.is_file():
                    for line in p.read_text(encoding="utf-8").splitlines():
                        if line.strip() == "masked_value=42":
                            bad = True
            shares_ok = True
            for pid in ("1", "2", "3"):
                for party in (0, 1, 2):
                    sp = RUN_CWD / "provider_secrets" / f"provider_{pid}_share_{party}.secret"
                    if not sp.is_file():
                        shares_ok = False
            self.append(
                f"[scenario masking] No plaintext masked_value=42: {not bad}; share files present: {shares_ok}\n"
                f"Overall: {'OK' if (not bad and shares_ok) else 'FAIL'}\n\n"
            )

        self._run_async(task)

    def on_scenario_crash(self) -> None:
        def task() -> None:
            self._clean_workspace_dirs()
            self.run_cmd(["./build/node/data_provider", "1", "100", "--computation-nodes", "2"], "provider")
            self.run_cmd(["./build/node/data_provider", "2", "200", "--computation-nodes", "2"], "provider")
            self.run_cmd(["./build/consensus/consensus", "2"], "consensus")
            _, out = self.run_cmd(["./build/spdz_bridge/spdz_bridge", "--computation-nodes", "2"], "bridge")
            m = re.search(
                r"MP-SPDZ result:\s*(?:(?:SUM|RESULT)=)?\s*([-]?\d+)",
                out,
            )
            val = m.group(1) if m else None
            self.append(f"[scenario crash] Expected MPC result 300: got {val} ({'OK' if val == '300' else 'check MP-SPDZ'})\n\n")

        self._run_async(task)

    def on_scenario_mpc_matrix(self) -> None:
        if not self._mpc_files:
            messagebox.showerror("Programs", "No programs/*.mpc found.")
            return
        if not messagebox.askyesno("Confirm", "This runs several MPC compilations and may take a long time. Continue?"):
            return

        expected = {"sum": "42", "avg": "14", "triple_sum": "126", "parity_sum": "0"}

        def task() -> None:
            for p in self._mpc_files:
                stem = p.stem
                exp = expected.get(stem)
                self._clean_workspace_dirs()
                self.run_cmd(
                    ["./build/node/data_provider", "1", "7", "--computation-nodes", "3"],
                    "provider",
                )
                self.run_cmd(
                    ["./build/node/data_provider", "2", "15", "--computation-nodes", "3"],
                    "provider",
                )
                self.run_cmd(
                    ["./build/node/data_provider", "3", "20", "--computation-nodes", "3"],
                    "provider",
                )
                self.run_cmd(["./build/consensus/consensus", "3"], "consensus")
                _, out = self.run_cmd(
                    [
                        "./build/spdz_bridge/spdz_bridge",
                        "--computation-nodes",
                        "3",
                        p.relative_to(PROJECT_ROOT).as_posix(),
                    ],
                    f"bridge-{stem}",
                )
                m = re.search(r"MP-SPDZ result:\s*(\S+)", out)
                got = m.group(1).split("=")[-1] if m else ""
                # normalize RESULT=42 / 42
                got_digits = re.sub(r"^(?:SUM|RESULT)=", "", got)
                ok = exp is None or got_digits == exp
                self.append(f"[matrix {stem}] expected {exp}, parsed '{got_digits}': {'OK' if ok else 'CHECK'}\n\n")

        self._run_async(task)

    def on_run_full_validation(self) -> None:
        if os.name == "nt" and not self.var_use_wsl.get():
            messagebox.showwarning(
                "WSL",
                "On Windows, enable 'Run commands in WSL' so bash can run scripts/full_system_validation_wsl.sh.",
            )
            return
        if not messagebox.askyesno("Confirm", "Run the full validation script? This can take many minutes."):
            return

        def task() -> None:
            self.run_cmd(["bash", "scripts/full_system_validation_wsl.sh"], "full_validation")

        self._run_async(task)


def main() -> None:
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
