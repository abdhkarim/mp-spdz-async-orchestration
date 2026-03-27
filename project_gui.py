"""
Project GUI (from scratch)
--------------------------
Front-end for the repo's real workflow:
  data_provider -> share_verifier (ACK mandatory) -> consensus (ACK mandatory)
  -> optional spdz_bridge -> optional MP-SPDZ

No dependency on any orchestrator script.
"""

from __future__ import annotations

import json
import os
import queue
import shlex
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Optional

import tkinter as tk
from tkinter import filedialog, messagebox, ttk


REPO_ROOT = Path(__file__).resolve().parent


def is_windows() -> bool:
    return os.name == "nt"


class WorkflowFlowchart(ttk.Frame):
    """
    Global, persistent workflow visualization.
    Renders a simple flowchart:
      Provider(s) -> ACK generation -> Consensus -> Optional bridge -> Optional MP-SPDZ
    Each node can be in: idle | running | success | failed (strong visual color coding).
    """

    def __init__(self, master: tk.Misc):
        super().__init__(master)
        self.canvas = tk.Canvas(self, height=110, highlightthickness=0, bg="white")
        self.canvas.pack(fill="both", expand=True)

        self.case_var = tk.StringVar(value="")
        self.case_label = ttk.Label(self, textvariable=self.case_var)
        self.case_label.place(x=12, y=8)

        self.steps: list[tuple[str, str, bool]] = [
            ("providers", "Provider(s)", False),
            ("ack", "ACK generation", False),
            ("consensus", "Consensus", False),
            ("bridge", "Optional bridge", True),
            ("mp_spdz", "Optional MP-SPDZ", True),
        ]
        self._state: dict[str, str] = {k: "idle" for k, _t, _opt in self.steps}

        self._node_items: dict[str, dict[str, int]] = {}
        self._arrow_items: list[int] = []

        self.canvas.bind("<Configure>", lambda _e: self._redraw())
        self._redraw()

    def reset(self) -> None:
        for k, _t, _opt in self.steps:
            self._state[k] = "idle"
        self.set_case("")
        self._apply_colors()

    def set_case(self, name: str) -> None:
        name = (name or "").strip()
        self.case_var.set(f"Running: {name}" if name else "")

    def set_step_state(self, step_key: str, state: str) -> None:
        if step_key not in self._state:
            return
        # Backward/forward compatibility: some parts of the repo/tests may
        # use `succeeded` instead of `success`.
        if state not in {"idle", "running", "success", "succeeded", "failed", "skipped"}:
            return
        self._state[step_key] = state
        self._apply_colors()

    def _colors_for(self, step_key: str) -> tuple[str, str, str]:
        # (fill, outline, text)
        state = self._state.get(step_key, "idle")
        if state == "idle":
            return ("#f2f4f7", "#c8d0da", "#1f2937")
        if state == "skipped":
            return ("#f8fafc", "#cbd5e1", "#6b7280")
        if state == "running":
            return ("#fff3cd", "#f0ad4e", "#7a4b00")
        if state in {"success", "succeeded"}:
            return ("#d1fae5", "#10b981", "#065f46")
        if state == "failed":
            return ("#fee2e2", "#ef4444", "#7f1d1d")
        return ("#f2f4f7", "#c8d0da", "#1f2937")

    def _redraw(self) -> None:
        self.canvas.delete("all")
        self._node_items.clear()
        self._arrow_items.clear()

        w = max(900, int(self.canvas.winfo_width() or 0))
        h = int(self.canvas.winfo_height() or 110)

        padding_x = 18
        top = 34
        node_h = 52
        gap = 18

        n = len(self.steps)
        node_w = int((w - padding_x * 2 - gap * (n - 1)) / n)
        node_w = max(140, min(230, node_w))

        # Center horizontally with fixed node_w and gap.
        total_w = n * node_w + (n - 1) * gap
        start_x = int((w - total_w) / 2)

        # Arrows + nodes
        for i, (step_key, title, is_optional) in enumerate(self.steps):
            x0 = start_x + i * (node_w + gap)
            y0 = top
            x1 = x0 + node_w
            y1 = y0 + node_h

            fill, outline, text_color = self._colors_for(step_key)
            width = 2 if self._state.get(step_key) in {"running", "failed"} else 1

            rect = self.canvas.create_rectangle(x0, y0, x1, y1, fill=fill, outline=outline, width=width)
            if is_optional:
                # Dashed outline effect: draw an extra dashed rectangle on top.
                self.canvas.create_rectangle(x0, y0, x1, y1, fill="", outline=outline, width=2, dash=(5, 3))

            label = self.canvas.create_text(
                int((x0 + x1) / 2),
                int((y0 + y1) / 2) - 6,
                text=title,
                fill=text_color,
                font=("Segoe UI", 10, "bold") if is_windows() else ("TkDefaultFont", 10, "bold"),
            )

            sub = self.canvas.create_text(
                int((x0 + x1) / 2),
                int((y0 + y1) / 2) + 14,
                text=self._state.get(step_key, "idle").upper(),
                fill=text_color,
                font=("Segoe UI", 9) if is_windows() else ("TkDefaultFont", 9),
            )

            self._node_items[step_key] = {"rect": rect, "label": label, "sub": sub}

            if i < n - 1:
                ax0 = x1 + 4
                ay = int((y0 + y1) / 2)
                ax1 = x1 + gap - 4
                arr = self.canvas.create_line(
                    ax0,
                    ay,
                    ax1,
                    ay,
                    arrow=tk.LAST,
                    fill="#9aa4b2",
                    width=2,
                )
                self._arrow_items.append(arr)

        # Light baseline bar (visual cohesion)
        self.canvas.create_line(padding_x, top + node_h + 14, w - padding_x, top + node_h + 14, fill="#e5e7eb")

    def _apply_colors(self) -> None:
        for step_key, items in self._node_items.items():
            fill, outline, text_color = self._colors_for(step_key)
            width = 2 if self._state.get(step_key) in {"running", "failed"} else 1
            self.canvas.itemconfig(items["rect"], fill=fill, outline=outline, width=width)
            self.canvas.itemconfig(items["label"], fill=text_color)
            self.canvas.itemconfig(items["sub"], fill=text_color, text=self._state.get(step_key, "idle").upper())


def now_unix_ms() -> int:
    return int(time.time() * 1000)


def ensure_dirs(*paths: Path) -> None:
    for p in paths:
        p.mkdir(parents=True, exist_ok=True)


def open_in_file_manager(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(str(path))
    if is_windows():
        os.startfile(str(path))  # noqa: S606,S607 - intended on Windows
        return
    if shutil.which("xdg-open"):
        subprocess.Popen(["xdg-open", str(path)], cwd=str(REPO_ROOT))
        return
    if shutil.which("open"):
        subprocess.Popen(["open", str(path)], cwd=str(REPO_ROOT))
        return
    raise RuntimeError("No known file manager opener (xdg-open/open).")


def wsl_bash_command(repo_root: Path, bash_cmd: str) -> list[str]:
    # On Windows: run via wsl.exe and convert Windows path to WSL path.
    # On Linux/WSL: run bash directly (there is no wsl.exe inside WSL).
    if not is_windows():
        return ["bash", "-lc", f"cd {shlex.quote(str(repo_root))} && {bash_cmd}"]

    return ["wsl", "-e", "bash", "-lc", f'cd "$(wslpath {shlex.quote(str(repo_root))})" && {bash_cmd}']


@dataclass(frozen=True)
class RunSpec:
    title: str
    argv: list[str]
    cwd: Path


class ProcessRunner:
    def __init__(self, on_line: Callable[[str], None], on_state: Callable[[str], None]):
        self._on_line = on_line
        self._on_state = on_state
        self._proc: Optional[subprocess.Popen] = None
        self._lock = threading.Lock()

    def is_running(self) -> bool:
        with self._lock:
            return self._proc is not None and self._proc.poll() is None

    def cancel(self) -> None:
        with self._lock:
            proc = self._proc
        if not proc or proc.poll() is not None:
            return
        self._on_state("Cancelling…")
        try:
            if is_windows():
                proc.terminate()
            else:
                proc.terminate()
        except Exception:
            pass

    def run_async(self, spec: RunSpec, on_exit: Optional[Callable[[int], None]] = None) -> None:
        if self.is_running():
            raise RuntimeError("A process is already running. Cancel it first.")

        def _target() -> None:
            self._on_state(f"Running: {spec.title}")
            self._on_line(f"$ (cwd={spec.cwd}) " + " ".join(map(shlex.quote, spec.argv)) + "\n")
            try:
                proc = subprocess.Popen(
                    spec.argv,
                    cwd=str(spec.cwd),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=1,
                    universal_newlines=True,
                )
            except FileNotFoundError as e:
                self._on_line(f"\nERROR: executable not found: {e}\n")
                self._on_state("Failed (executable not found)")
                return
            except Exception as e:
                self._on_line(f"\nERROR: failed to start process: {e}\n")
                self._on_state("Failed to start")
                return

            with self._lock:
                self._proc = proc

            try:
                assert proc.stdout is not None
                for line in proc.stdout:
                    self._on_line(line)
            finally:
                # Important: `poll()` can be None if the stdout pipe closes
                # before the process fully exits. Use `wait()` so the GUI
                # always receives an exit event (fixes steps stuck in `running`).
                rc = proc.wait()
                with self._lock:
                    self._proc = None
                self._on_state(f"Exited with code {rc}")
                if on_exit is not None:
                    try:
                        on_exit(int(rc))
                    except Exception:
                        # Never let callback errors crash the runner thread.
                        pass

        threading.Thread(target=_target, daemon=True).start()


class App(ttk.Frame):
    def __init__(self, master: tk.Tk):
        super().__init__(master)
        self.master = master

        self.q: "queue.Queue[tuple[str, str]]" = queue.Queue()
        self._step_sequence: list[dict[str, object]] = []
        self._step_sequence_idx: int = 0
        self._sequence_active: bool = False

        self.status_var = tk.StringVar(value="Idle")
        self.use_wsl_var = tk.BooleanVar(value=True if is_windows() else False)
        self.auto_configure_build_var = tk.BooleanVar(value=True)

        # Common workflow settings
        self.schema_id_var = tk.StringVar(value="semi2k-wire-v1")
        self.protocol_version_var = tk.StringVar(value="1")
        self.session_id_var = tk.StringVar(value="gui-session")
        self.round_id_var = tk.IntVar(value=0)
        self.computation_nodes_var = tk.IntVar(value=3)
        self.min_inputs_var = tk.IntVar(value=2)
        self.timeout_seconds_var = tk.IntVar(value=0)

        # Paths
        self.build_dir_var = tk.StringVar(value=str(REPO_ROOT / "build"))
        self.inputs_dir = REPO_ROOT / "inputs"
        self.provider_secrets_dir = REPO_ROOT / "provider_secrets"
        self.logs_dir = REPO_ROOT / "logs"
        self.artifacts_dir = REPO_ROOT / "artifacts"
        self.core_set_path = REPO_ROOT / "core_set.txt"
        self.validation_summary_path = REPO_ROOT / "backend_test_summary.txt"
        self.validation_runs_dir = REPO_ROOT / ".tmp_full_test_runs"

        # ACK dirs (configurable per run)
        # Use repo-relative paths so WSL commands work reliably.
        self.acks_dir_var = tk.StringVar(value=str(Path("artifacts") / "gui_acks"))
        self.cn_keys_dir_var = tk.StringVar(value=str(Path("artifacts") / "gui_cn_keys"))

        # MPC program
        self.program_path_var = tk.StringVar(value=str(REPO_ROOT / "programs" / "sum.mpc"))
        self.enable_bridge_var = tk.BooleanVar(value=False)

        # Providers list (simple table-like inputs)
        self.providers_text = tk.StringVar(value="1=7\n2=15\n3=20")

        self._build_ui()

        self.runner = ProcessRunner(
            on_line=lambda s: self.q.put(("line", s)),
            on_state=lambda s: self.q.put(("state", s)),
        )
        self.after(50, self._drain_queue)

    # ---------- UI ----------
    def _build_ui(self) -> None:
        self.master.title("MP-SPDZ Async Orchestration — Project GUI")
        self.master.minsize(1100, 700)

        style = ttk.Style()
        try:
            style.theme_use("vista" if is_windows() else "clam")
        except Exception:
            pass

        self.pack(fill="both", expand=True)

        top = ttk.Frame(self)
        top.pack(fill="x", padx=12, pady=(12, 6))

        left = ttk.Frame(top)
        left.pack(side="left", fill="x", expand=True)

        ttk.Label(left, text="Repo root").grid(row=0, column=0, sticky="w")
        ttk.Label(left, text=str(REPO_ROOT)).grid(row=0, column=1, sticky="w", padx=(8, 0))

        ttk.Checkbutton(left, text="Use WSL (recommended on Windows)", variable=self.use_wsl_var).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(4, 0)
        )
        ttk.Checkbutton(left, text="Auto-configure build/ if missing", variable=self.auto_configure_build_var).grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(4, 0)
        )

        ttk.Label(left, text="Build dir").grid(row=3, column=0, sticky="w", pady=(6, 0))
        build_entry = ttk.Entry(left, textvariable=self.build_dir_var, width=60)
        build_entry.grid(row=3, column=1, sticky="we", padx=(8, 0), pady=(6, 0))
        left.columnconfigure(1, weight=1)

        ttk.Label(left, text="Status").grid(row=4, column=0, sticky="w", pady=(6, 0))
        ttk.Label(left, textvariable=self.status_var).grid(row=4, column=1, sticky="w", padx=(8, 0), pady=(6, 0))

        right = ttk.Frame(top)
        right.pack(side="right", fill="y")
        ttk.Button(right, text="Cancel running process", command=self._cancel).pack(fill="x")
        ttk.Button(right, text="Open repo root", command=lambda: self._safe_open(REPO_ROOT)).pack(fill="x", pady=(6, 0))
        ttk.Button(right, text="Open artifacts/", command=lambda: self._safe_open(self.artifacts_dir)).pack(
            fill="x", pady=(6, 0)
        )
        ttk.Button(right, text="Open inputs/", command=lambda: self._safe_open(self.inputs_dir)).pack(fill="x", pady=(6, 0))
        ttk.Button(right, text="Open logs/", command=lambda: self._safe_open(self.logs_dir)).pack(fill="x", pady=(6, 0))

        # Global, persistent flowchart visualization (always visible regardless of tab).
        flow_wrap = ttk.Frame(self)
        flow_wrap.pack(fill="x", padx=12, pady=(0, 6))
        self.flowchart = WorkflowFlowchart(flow_wrap)
        self.flowchart.pack(fill="x")

        mid = ttk.Panedwindow(self, orient="horizontal")
        mid.pack(fill="both", expand=True, padx=12, pady=6)

        self.notebook = ttk.Notebook(mid)
        mid.add(self.notebook, weight=2)

        log_frame = ttk.Frame(mid)
        mid.add(log_frame, weight=3)

        self._build_tabs()
        self._build_log_panel(log_frame)

    def _build_tabs(self) -> None:
        self.tab_build = ttk.Frame(self.notebook)
        self.tab_pipeline = ttk.Frame(self.notebook)
        self.tab_full = ttk.Frame(self.notebook)
        self.tab_execution = ttk.Frame(self.notebook)
        self.tab_outputs = ttk.Frame(self.notebook)

        self.notebook.add(self.tab_build, text="Build / Setup")
        self.notebook.add(self.tab_pipeline, text="Manual Pipeline")
        self.notebook.add(self.tab_full, text="Full-cycle Run")
        self.notebook.add(self.tab_execution, text="Execution / Testing")
        self.notebook.add(self.tab_outputs, text="Outputs / Files")

        self._tab_build_ui()
        self._tab_pipeline_ui()
        self._tab_full_ui()
        self._tab_execution_ui()
        self._tab_outputs_ui()

    def _build_log_panel(self, parent: ttk.Frame) -> None:
        header = ttk.Frame(parent)
        header.pack(fill="x", pady=(0, 6))
        ttk.Label(header, text="Live logs (stdout/stderr)").pack(side="left")
        ttk.Button(header, text="Clear", command=self._log_clear).pack(side="right")

        self.log_text = tk.Text(parent, wrap="none", height=30)
        self.log_text.pack(fill="both", expand=True)

        yscroll = ttk.Scrollbar(self.log_text, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=yscroll.set)
        yscroll.pack(side="right", fill="y")

        xscroll = ttk.Scrollbar(parent, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(xscrollcommand=xscroll.set)
        xscroll.pack(fill="x")

        self.log_text.configure(font=("Consolas", 10) if is_windows() else ("TkFixedFont", 10))

    # ---------- Tabs ----------
    def _tab_build_ui(self) -> None:
        f = self.tab_build
        f.columnconfigure(1, weight=1)

        ttk.Label(f, text="CMake configure").grid(row=0, column=0, sticky="w", padx=10, pady=(10, 4))
        self.cmake_args_var = tk.StringVar(value="-S . -B build")
        ttk.Entry(f, textvariable=self.cmake_args_var).grid(row=0, column=1, sticky="we", padx=10, pady=(10, 4))
        ttk.Button(f, text="Configure", command=self._cmake_configure).grid(row=0, column=2, padx=10, pady=(10, 4))

        ttk.Label(f, text="CMake build").grid(row=1, column=0, sticky="w", padx=10, pady=4)
        self.cmake_build_args_var = tk.StringVar(
            value="--build build -j4 --target data_provider consensus ack_crypto_tool share_verifier spdz_bridge"
        )
        ttk.Entry(f, textvariable=self.cmake_build_args_var).grid(row=1, column=1, sticky="we", padx=10, pady=4)
        ttk.Button(f, text="Build", command=self._cmake_build).grid(row=1, column=2, padx=10, pady=4)

        ttk.Separator(f).grid(row=2, column=0, columnspan=3, sticky="we", padx=10, pady=10)

        ttk.Label(f, text="Clean workspace (generated artifacts)").grid(row=3, column=0, sticky="w", padx=10, pady=4)
        ttk.Button(f, text="Clean inputs/logs/artifacts/core_set/provider_secrets", command=self._clean_workspace).grid(
            row=3, column=1, sticky="w", padx=10, pady=4
        )
        ttk.Button(f, text="Clean build/ (danger)", command=self._clean_build).grid(row=3, column=2, padx=10, pady=4)

        ttk.Separator(f).grid(row=4, column=0, columnspan=3, sticky="we", padx=10, pady=10)

        ttk.Label(
            f,
            text="Note: binaries must be executed with CWD = repo root (paths are resolved relative to current directory).",
        ).grid(row=5, column=0, columnspan=3, sticky="w", padx=10, pady=(0, 10))

    def _tab_pipeline_ui(self) -> None:
        f = self.tab_pipeline
        f.columnconfigure(1, weight=1)

        row = 0
        ttk.Label(f, text="Workflow settings").grid(row=row, column=0, sticky="w", padx=10, pady=(10, 4))
        row += 1

        grid = ttk.Frame(f)
        grid.grid(row=row, column=0, columnspan=3, sticky="we", padx=10)
        for i in range(8):
            grid.columnconfigure(i, weight=1)

        def add_labeled(col: int, label: str, var: tk.Variable, width: int = 18) -> None:
            ttk.Label(grid, text=label).grid(row=0, column=col, sticky="w")
            ttk.Entry(grid, textvariable=var, width=width).grid(row=1, column=col, sticky="we", padx=(0, 10))

        add_labeled(0, "schema_id", self.schema_id_var, 20)
        add_labeled(1, "protocol_version", self.protocol_version_var, 14)
        add_labeled(2, "session_id", self.session_id_var, 18)
        add_labeled(3, "round_id", self.round_id_var, 10)
        add_labeled(4, "num_parties", self.computation_nodes_var, 10)
        add_labeled(5, "min_inputs", self.min_inputs_var, 10)
        add_labeled(6, "timeout_seconds", self.timeout_seconds_var, 12)

        row += 1
        ttk.Separator(f).grid(row=row, column=0, columnspan=3, sticky="we", padx=10, pady=10)
        row += 1

        ttk.Label(f, text="Providers (one per line: provider_id=value)").grid(
            row=row, column=0, sticky="w", padx=10, pady=(0, 4)
        )
        row += 1
        providers = tk.Text(f, height=7, wrap="none")
        providers.grid(row=row, column=0, columnspan=2, sticky="nsew", padx=10)
        providers.insert("1.0", self.providers_text.get())
        f.rowconfigure(row, weight=1)
        f.columnconfigure(1, weight=1)

        def sync_providers_out() -> None:
            self.providers_text.set(providers.get("1.0", "end").strip())

        providers.bind("<KeyRelease>", lambda _e: sync_providers_out())
        providers.bind("<FocusOut>", lambda _e: sync_providers_out())

        buttons = ttk.Frame(f)
        buttons.grid(row=row, column=2, sticky="ns", padx=(0, 10))

        ttk.Button(buttons, text="Run provider(s)", command=self._run_providers).pack(fill="x", pady=(0, 6))
        ttk.Separator(buttons).pack(fill="x", pady=6)

        ttk.Label(buttons, text="ACK evidence").pack(anchor="w")
        ttk.Button(buttons, text="Gen CN keys", command=self._gen_cn_keys).pack(fill="x", pady=(4, 4))
        ttk.Button(buttons, text="Generate ACKs", command=self._gen_acks).pack(fill="x", pady=(0, 6))
        ttk.Button(buttons, text="Run consensus (ACK mandatory)", command=self._run_consensus).pack(fill="x")

        row += 1
        ttk.Separator(f).grid(row=row, column=0, columnspan=3, sticky="we", padx=10, pady=10)
        row += 1

        bridge = ttk.Labelframe(f, text="Optional: bridge / MP-SPDZ")
        bridge.grid(row=row, column=0, columnspan=3, sticky="we", padx=10, pady=(0, 10))
        bridge.columnconfigure(1, weight=1)

        ttk.Checkbutton(bridge, text="Enable bridge step", variable=self.enable_bridge_var).grid(
            row=0, column=0, sticky="w", padx=10, pady=8
        )
        ttk.Label(bridge, text="Program (.mpc)").grid(row=1, column=0, sticky="w", padx=10, pady=(0, 8))
        ttk.Entry(bridge, textvariable=self.program_path_var).grid(row=1, column=1, sticky="we", padx=10, pady=(0, 8))
        ttk.Button(bridge, text="Browse", command=self._browse_program).grid(row=1, column=2, padx=10, pady=(0, 8))
        ttk.Button(bridge, text="Run bridge now", command=self._run_bridge).grid(
            row=2, column=0, padx=10, pady=(0, 10), sticky="w"
        )
        ttk.Label(
            bridge,
            text="Bridge reads `core_set.txt` and prepares `third_party/MP-SPDZ/Player-Data/` then runs semi2k.",
        ).grid(row=2, column=1, columnspan=2, sticky="w", padx=10, pady=(0, 10))

    def _tab_full_ui(self) -> None:
        f = self.tab_full
        f.columnconfigure(0, weight=1)

        ttk.Label(
            f,
            text="Run the whole workflow (no orchestrator): providers → ACKs → consensus (ACK mandatory) → optional bridge",
        ).grid(row=0, column=0, sticky="w", padx=10, pady=(10, 8))

        ttk.Button(f, text="Run full-cycle now", command=self._run_full_cycle).grid(
            row=1, column=0, sticky="w", padx=10, pady=(0, 10)
        )

        ttk.Label(
            f,
            text="Tip: keep `min_inputs` aligned with number of providers you actually run; consensus will reject otherwise.",
        ).grid(row=2, column=0, sticky="w", padx=10, pady=(0, 10))

    def _tab_execution_ui(self) -> None:
        """
        Unified execution/testing tab.
        Replaces the old split between:
          - Validation / Tests
          - Simulation / Scenarios
        """
        f = self.tab_execution
        f.columnconfigure(0, weight=1)

        header = ttk.Frame(f)
        header.grid(row=0, column=0, sticky="we", padx=10, pady=(10, 6))
        header.columnconfigure(1, weight=1)

        ttk.Label(
            header,
            text="One control center: choose a single execution target or run the full validation suite.",
        ).grid(row=0, column=0, sticky="w")

        # --- Controls ---
        controls = ttk.LabelFrame(f, text="Execution target")
        controls.grid(row=1, column=0, sticky="we", padx=10, pady=(0, 10))
        controls.columnconfigure(1, weight=1)

        self.execution_var = tk.StringVar(value="normal")
        self.execution_targets: dict[str, Optional[str]] = {
            "normal": "ack-attack:normal",
            "insufficient ACKs": "ack-attack:insufficient-acks",
            "stale ACK": "ack-attack:stale-ack",
            "replay ACK": "ack-attack:replay-ack",
            "tampered ACK": "ack-attack:tampered-ack",
            "late / missing provider": "late-provider",
            "provider crash simulation": "crash-provider",
            "provider tampering (file/proof mismatch)": "tampering",
            "MPC matrix": "mpc-matrix",
            "Type proof admission": "typeproof",
            "Semantic type proof backend": "semantic",
            "Proof-real backend": "proofreal",
            "Full validation suite": None,
        }

        display_values = list(self.execution_targets.keys())

        ttk.Label(controls, text="Target").grid(row=0, column=0, sticky="w", padx=10, pady=8)
        ttk.Combobox(
            controls,
            textvariable=self.execution_var,
            values=display_values,
            state="readonly",
            width=46,
        ).grid(row=0, column=1, sticky="we", padx=10, pady=8)

        ttk.Button(
            controls,
            text="Run selected target",
            command=self._run_selected_execution_target,
        ).grid(row=0, column=2, sticky="w", padx=10, pady=8)

        btns = ttk.Frame(f)
        btns.grid(row=2, column=0, sticky="w", padx=10, pady=(0, 10))
        ttk.Button(btns, text="Open backend_test_summary.txt", command=self._open_validation_summary).pack(
            side="left", padx=(0, 8)
        )
        ttk.Button(btns, text="Open .tmp_full_test_runs/", command=self._open_validation_runs).pack(side="left")

        ttk.Label(
            f,
            text="Tip: the global flowchart above updates live during execution.",
        ).grid(row=3, column=0, sticky="w", padx=10, pady=(0, 6))

    def _flow_reset(self) -> None:
        self.flowchart.reset()

    def _flow_case(self, name: str) -> None:
        self.flowchart.set_case(name)

    def _flow_step(self, step_key: str, state: str) -> None:
        self.flowchart.set_step_state(step_key, state)

    def _flow_steps(self, step_keys: Iterable[str], state: str) -> None:
        for k in step_keys:
            self._flow_step(k, state)

    def _run_shelllike_with_flow(
        self,
        title: str,
        command: str,
        flow_keys: list[str],
        case_name: Optional[str] = None,
    ) -> None:
        """
        Run one shell-like command and drive the flowchart directly (no markers required).
        """
        if self.runner.is_running():
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return

        if case_name:
            self._flow_reset()
            self._flow_case(case_name)

        self._flow_steps(flow_keys, "running")

        def _on_exit(rc: int) -> None:
            self.q.put(("flow_exit", json.dumps({"rc": rc, "flow_keys": flow_keys})))

        if self.use_wsl_var.get():
            argv = wsl_bash_command(REPO_ROOT, command)
        else:
            argv = ["bash", "-lc", f"cd {shlex.quote(str(REPO_ROOT))} && {command}"]

        spec = RunSpec(title=title, argv=argv, cwd=REPO_ROOT)
        self.runner.run_async(spec, on_exit=_on_exit)

    def _start_step_sequence(self, case_name: str, steps: list[dict[str, object]]) -> None:
        """
        Start a GUI-driven step sequence. Each step is a dict:
          { "title": str, "command": str, "flow_keys": list[str], "enabled": bool }
        """
        if self.runner.is_running() or self._sequence_active:
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return
        self._sequence_active = True
        self._step_sequence = steps
        self._step_sequence_idx = 0
        self._flow_reset()
        self._flow_case(case_name)
        self._run_next_sequence_step()

    def _run_next_sequence_step(self) -> None:
        if not self._sequence_active:
            return
        if self._step_sequence_idx >= len(self._step_sequence):
            self._sequence_active = False
            return

        step = self._step_sequence[self._step_sequence_idx]
        title = str(step["title"])
        command = str(step["command"])
        flow_keys = list(step.get("flow_keys", []))  # type: ignore[arg-type]
        enabled = bool(step.get("enabled", True))

        if not enabled:
            self._flow_steps(flow_keys, "skipped")
            self._step_sequence_idx += 1
            self._run_next_sequence_step()
            return

        self._flow_steps(flow_keys, "running")

        def _on_exit(rc: int) -> None:
            self.q.put(("seq_exit", json.dumps({"rc": rc, "flow_keys": flow_keys})))

        # Sequence steps are always shell-like (single process per step).
        if self.use_wsl_var.get():
            argv = wsl_bash_command(REPO_ROOT, command)
        else:
            argv = ["bash", "-lc", f"cd {shlex.quote(str(REPO_ROOT))} && {command}"]
        spec = RunSpec(title=title, argv=argv, cwd=REPO_ROOT)
        self.runner.run_async(spec, on_exit=_on_exit)

    def _handle_gui_markers(self, line: str) -> bool:
        """
        Parses backend markers to drive the global flowchart visualization.

        Markers:
          - `GUI_CASE:<name>`
          - `GUI_STEP:<step_key>:<start|success|failed>`
        """
        s = (line or "").strip()
        if not s:
            return False

        if s.startswith("GUI_CASE:"):
            case_name = s[len("GUI_CASE:") :].strip()
            self._flow_case(case_name)
            self._flow_reset()
            self._flow_case(case_name)
            return True

        if s.startswith("GUI_STEP:"):
            rest = s[len("GUI_STEP:") :]
            parts = rest.split(":", 2)
            if len(parts) >= 2:
                step_key = parts[0].strip()
                step_state = parts[1].strip()
                if step_state == "start":
                    self._flow_step(step_key, "running")
                elif step_state in {"success", "succeeded"}:
                    self._flow_step(step_key, "succeeded")
                elif step_state == "failed":
                    self._flow_step(step_key, "failed")
                return True

        return False

    def _tab_outputs_ui(self) -> None:
        f = self.tab_outputs
        f.columnconfigure(0, weight=1)

        ttk.Label(f, text="Quick access to produced files/folders").grid(
            row=0, column=0, sticky="w", padx=10, pady=(10, 6)
        )

        grid = ttk.Frame(f)
        grid.grid(row=1, column=0, sticky="w", padx=10)

        items = [
            ("core_set.txt", self.core_set_path),
            ("inputs/", self.inputs_dir),
            ("provider_secrets/", self.provider_secrets_dir),
            ("artifacts/", self.artifacts_dir),
            ("logs/", self.logs_dir),
            ("backend_test_summary.txt", self.validation_summary_path),
        ]
        for r, (label, path) in enumerate(items):
            ttk.Button(grid, text=f"Open {label}", command=lambda p=path: self._safe_open(p)).grid(
                row=r, column=0, sticky="w", pady=4
            )
            ttk.Label(grid, text=str(path)).grid(row=r, column=1, sticky="w", padx=(10, 0))

    # ---------- Logging ----------
    def _log_clear(self) -> None:
        self.log_text.delete("1.0", "end")

    def _log_append(self, s: str) -> None:
        self.log_text.insert("end", s)
        self.log_text.see("end")

    def _drain_queue(self) -> None:
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "line":
                    if not self._handle_gui_markers(payload):
                        self._log_append(payload)
                elif kind == "state":
                    self.status_var.set(payload)
                elif kind == "flow_exit":
                    d = json.loads(payload)
                    rc = int(d.get("rc", 1))
                    flow_keys = list(d.get("flow_keys", []))
                    self._flow_steps(flow_keys, "succeeded" if rc == 0 else "failed")
                elif kind == "seq_exit":
                    d = json.loads(payload)
                    rc = int(d.get("rc", 1))
                    flow_keys = list(d.get("flow_keys", []))
                    self._flow_steps(flow_keys, "succeeded" if rc == 0 else "failed")
                    if rc != 0:
                        self._sequence_active = False
                    else:
                        self._step_sequence_idx += 1
                        self._run_next_sequence_step()
                else:
                    self._log_append(payload)
        except queue.Empty:
            pass
        self.after(50, self._drain_queue)

    # ---------- Command helpers ----------
    def _run(self, title: str, argv: list[str]) -> None:
        spec = RunSpec(title=title, argv=argv, cwd=REPO_ROOT)
        self.runner.run_async(spec)

    def _run_shelllike(self, title: str, command: str) -> None:
        if self.use_wsl_var.get():
            argv = wsl_bash_command(REPO_ROOT, command)
        else:
            # PowerShell/cmd portability is tricky; we run via Python only for direct binaries.
            # For complex shell fragments, prefer WSL.
            argv = ["bash", "-lc", f"cd {shlex.quote(str(REPO_ROOT))} && {command}"]
        self._run(title, argv)

    def _ensure_configured_prefix(self) -> str:
        """
        Returns a bash prefix that ensures build/ exists and is configured.
        This avoids errors like: 'build is not a directory' after cleaning.
        """
        if not self.auto_configure_build_var.get():
            return ""
        # If build dir or CMakeCache is missing, configure.
        return '([ -f build/CMakeCache.txt ] || cmake -S . -B build) && '

    def _bash_path(self, p: Path) -> str:
        """
        Produce a bash-usable path string.
        - If p is under repo root, return a repo-relative posix path.
        - Otherwise (rare), fall back to wslpath conversion in WSL mode.
        """
        try:
            rel = p.resolve().relative_to(REPO_ROOT.resolve())
            return shlex.quote(rel.as_posix())
        except Exception:
            # Outside repo root
            if self.use_wsl_var.get():
                return f'$(wslpath {shlex.quote(str(p))})'
            return shlex.quote(str(p))

    def _bash_user_path(self, s: str) -> str:
        """
        Convert a user-provided path string to a bash-usable path.
        - Repo-relative / under repo root => repo-relative posix path
        - Windows absolute path (e.g. D:\\... or D:/...) in WSL mode => $(wslpath 'D:\\...')
        - Otherwise => shell-quoted as provided
        """
        s = (s or "").strip()
        if not s:
            return "''"
        # Windows absolute (drive letter)
        if len(s) >= 3 and s[1] == ":" and (s[2] == "\\" or s[2] == "/"):
            if self.use_wsl_var.get():
                # wslpath expects Windows-style; normalize to backslashes for safety
                win = s.replace("/", "\\")
                return f'$(wslpath {shlex.quote(win)})'
            return shlex.quote(s)
        # Try interpret as path
        try:
            p = Path(s)
            # If it's absolute on this OS or looks like it, try repo-relative mapping
            if p.is_absolute():
                return self._bash_path(p)
        except Exception:
            pass
        return shlex.quote(s.replace("\\", "/"))

    def _cancel(self) -> None:
        self.runner.cancel()

    def _safe_open(self, path: Path) -> None:
        try:
            open_in_file_manager(path)
        except Exception as e:
            messagebox.showerror("Open failed", str(e))

    # ---------- Build / Setup ----------
    def _cmake_configure(self) -> None:
        args = self.cmake_args_var.get().strip()
        if not args:
            messagebox.showerror("Invalid", "CMake args are empty.")
            return
        # Use WSL for build on Windows by default (libsodium/pkg-config).
        cmd = f"cmake {args}"
        self._run_shelllike("CMake configure", cmd)

    def _cmake_build(self) -> None:
        args = self.cmake_build_args_var.get().strip()
        if not args:
            messagebox.showerror("Invalid", "CMake build args are empty.")
            return
        # Auto-configure if build/ was deleted.
        cmd = f"{self._ensure_configured_prefix()}cmake {args}"
        self._run_shelllike("CMake build", cmd)

    def _clean_workspace(self) -> None:
        if not messagebox.askyesno("Confirm", "Delete generated workspace folders/files?"):
            return
        # Also clean GUI-specific artifacts dirs (may be user-customized).
        gui_extra_targets: list[Path] = []
        try:
            gui_acks_dir = (REPO_ROOT / Path(self.acks_dir_var.get())).resolve()
            gui_extra_targets.append(gui_acks_dir)
        except Exception:
            pass
        try:
            gui_cn_keys_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
            gui_extra_targets.append(gui_cn_keys_dir)
        except Exception:
            pass

        targets = [
            self.inputs_dir,
            self.logs_dir,
            self.artifacts_dir,
            self.provider_secrets_dir,
            self.core_set_path,
            self.validation_summary_path,
            self.validation_runs_dir,
        ]
        # De-dup while preserving order.
        seen: set[Path] = set()
        all_targets: list[Path] = []
        for p in targets + gui_extra_targets:
            if p in seen:
                continue
            seen.add(p)
            all_targets.append(p)
        for p in all_targets:
            try:
                if p.is_dir():
                    shutil.rmtree(p)
                elif p.exists():
                    p.unlink()
            except Exception as e:
                messagebox.showwarning("Clean warning", f"Failed to remove {p}: {e}")
        self._log_append("Workspace cleaned (folders deleted; will be recreated on demand).\n")

    def _clean_build(self) -> None:
        build_dir = Path(self.build_dir_var.get()).resolve()
        if not build_dir.exists():
            messagebox.showinfo("Build clean", f"{build_dir} does not exist.")
            return
        if not messagebox.askyesno("Confirm", f"Delete build dir?\n\n{build_dir}"):
            return
        try:
            shutil.rmtree(build_dir)
            self._log_append(f"Deleted {build_dir}\n")
        except Exception as e:
            messagebox.showerror("Failed", str(e))

    # ---------- Pipeline steps ----------
    def _parse_providers(self) -> list[tuple[int, int]]:
        lines = [ln.strip() for ln in self.providers_text.get().splitlines() if ln.strip()]
        out: list[tuple[int, int]] = []
        for ln in lines:
            if "=" not in ln:
                raise ValueError(f"Invalid provider line (expected id=value): {ln}")
            a, b = ln.split("=", 1)
            out.append((int(a.strip()), int(b.strip())))
        if not out:
            raise ValueError("No providers configured.")
        return out

    def _bin(self, rel: str) -> Path:
        # IMPORTANT: binaries resolve paths relative to CWD (= repo root), but the executable lives in build/.
        return (REPO_ROOT / "build" / rel).resolve()

    def _run_providers(self) -> None:
        providers = self._parse_providers()
        n = int(self.computation_nodes_var.get())
        # Run sequentially in a single WSL bash command so logs remain linear, and CWD is correct.
        parts: list[str] = []
        for pid, val in providers:
            parts.append(f'./build/node/data_provider {pid} {val} --computation-nodes {n}')
        cmd = self._ensure_configured_prefix() + " && ".join(parts)
        self._run_shelllike_with_flow("Run provider(s)", cmd, flow_keys=["providers"], case_name="Manual: providers")

    def _gen_cn_keys(self) -> None:
        cn_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
        n = int(self.computation_nodes_var.get())
        ensure_dirs(cn_dir)
        cn_dir_bash = self._bash_path(cn_dir)
        parts: list[str] = [f'rm -rf {cn_dir_bash} && mkdir -p {cn_dir_bash}']
        for cn_id in range(n):
            pub = cn_dir / f"cn_{cn_id}.pub.hex"
            sec = cn_dir / f"cn_{cn_id}.sec.hex"
            parts.append(
                f'./build/consensus/ack_crypto_tool gen-keypair {self._bash_path(pub)} {self._bash_path(sec)}'
            )
        cmd = self._ensure_configured_prefix() + " && ".join(parts)
        self._run_shelllike_with_flow("Generate CN keypairs", cmd, flow_keys=["ack"], case_name="Manual: ACK prep")

    def _gen_acks(self) -> None:
        providers = self._parse_providers()
        acks_dir = (REPO_ROOT / Path(self.acks_dir_var.get())).resolve()
        cn_keys_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
        ensure_dirs(acks_dir, cn_keys_dir)
        n = int(self.computation_nodes_var.get())

        sess = self.session_id_var.get().strip()
        if not sess:
            messagebox.showerror("Invalid", "session_id is empty.")
            return
        rid = int(self.round_id_var.get())
        schema = self.schema_id_var.get().strip()
        proto = self.protocol_version_var.get().strip()

        acks_dir_bash = self._bash_path(acks_dir)
        cn_keys_dir_bash = self._bash_path(cn_keys_dir)
        parts: list[str] = [f'rm -rf {acks_dir_bash} && mkdir -p {acks_dir_bash}']
        for pid, _val in providers:
            for party in range(n):
                parts.append(
                    " ".join(
                        [
                            "./build/consensus/share_verifier",
                            f"--session-id {shlex.quote(sess)}",
                            f"--round-id {rid}",
                            f"--protocol-version {shlex.quote(proto)}",
                            f"--schema-id {shlex.quote(schema)}",
                            f"--provider-id {pid}",
                            f"--party-index {party}",
                            "--inputs-dir inputs",
                            "--provider-secrets-dir provider_secrets",
                            f'--share-manifest-path {shlex.quote(f"inputs/provider_{pid}_manifest.json")}',
                            f"--cn-keys-dir {cn_keys_dir_bash}",
                            f"--acks-out-dir {acks_dir_bash}",
                        ]
                    )
                )
        cmd = self._ensure_configured_prefix() + " && ".join(parts)
        self._run_shelllike_with_flow(
            "Generate ACKs (share_verifier)",
            cmd,
            flow_keys=["ack"],
            case_name="Manual: ACK generation",
        )

    def _run_consensus(self) -> None:
        acks_dir = (REPO_ROOT / Path(self.acks_dir_var.get())).resolve()
        cn_keys_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
        artifacts_dir = self.artifacts_dir
        ensure_dirs(acks_dir, cn_keys_dir, artifacts_dir)

        sess = self.session_id_var.get().strip()
        rid = int(self.round_id_var.get())
        schema = self.schema_id_var.get().strip()
        proto = self.protocol_version_var.get().strip()
        min_inputs = int(self.min_inputs_var.get())
        n = int(self.computation_nodes_var.get())
        timeout_s = int(self.timeout_seconds_var.get())

        acks_dir_bash = self._bash_path(acks_dir)
        cn_keys_dir_bash = self._bash_path(cn_keys_dir)
        artifacts_dir_bash = self._bash_path(artifacts_dir)
        # ACK mandatory by always passing --acks-dir
        cmd = self._ensure_configured_prefix() + " ".join(
            [
                "./build/consensus/consensus",
                str(min_inputs),
                f"--acks-dir {acks_dir_bash}",
                f"--num-parties {n}",
                f"--session-id {shlex.quote(sess)}",
                f"--round-id {rid}",
                f"--timeout-seconds {timeout_s}",
                f"--artifacts-dir {artifacts_dir_bash}",
                f"--cn-keys-dir {cn_keys_dir_bash}",
                f"--protocol-version {shlex.quote(proto)}",
                f"--schema-id {shlex.quote(schema)}",
            ]
        )
        self._run_shelllike_with_flow(
            "Consensus (ACK mandatory)",
            cmd,
            flow_keys=["consensus"],
            case_name="Manual: consensus",
        )

    def _browse_program(self) -> None:
        p = filedialog.askopenfilename(
            title="Select .mpc program",
            initialdir=str(REPO_ROOT / "programs"),
            filetypes=[("MPC programs", "*.mpc"), ("All files", "*.*")],
        )
        if p:
            self.program_path_var.set(p)

    def _run_bridge(self) -> None:
        n = int(self.computation_nodes_var.get())
        program = self.program_path_var.get().strip()
        if program:
            # Accept absolute or repo-relative; bridge expects a path and runs compile.py inside MP-SPDZ.
            program_bash = self._bash_user_path(program)
            cmd = (
                self._ensure_configured_prefix()
                + f"./build/spdz_bridge/spdz_bridge --computation-nodes {n} {program_bash}"
            )
        else:
            cmd = self._ensure_configured_prefix() + f"./build/spdz_bridge/spdz_bridge --computation-nodes {n}"
        self._run_shelllike_with_flow(
            "spdz_bridge (optional)",
            cmd,
            flow_keys=["bridge", "mp_spdz"],
            case_name="Manual: bridge / MP-SPDZ",
        )

    # ---------- Full-cycle ----------
    def _run_full_cycle(self) -> None:
        if self.runner.is_running():
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return

        # Run as explicit GUI-tracked steps so the global flowchart can update stage-by-stage.
        providers = self._parse_providers()
        n = int(self.computation_nodes_var.get())
        sess = self.session_id_var.get().strip()
        rid = int(self.round_id_var.get())
        schema = self.schema_id_var.get().strip()
        proto = self.protocol_version_var.get().strip()
        min_inputs = int(self.min_inputs_var.get())
        timeout_s = int(self.timeout_seconds_var.get())

        acks_dir = (REPO_ROOT / Path(self.acks_dir_var.get())).resolve()
        cn_keys_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
        acks_dir_bash = self._bash_path(acks_dir)
        cn_keys_dir_bash = self._bash_path(cn_keys_dir)
        artifacts_dir_bash = self._bash_path(self.artifacts_dir)
        ensure_dirs(self.inputs_dir, self.logs_dir, self.artifacts_dir, self.provider_secrets_dir)

        # Step 1: providers
        provider_cmds: list[str] = []
        if self.auto_configure_build_var.get():
            provider_cmds.append("([ -f build/CMakeCache.txt ] || cmake -S . -B build)")
        for pid, val in providers:
            provider_cmds.append(f'./build/node/data_provider {pid} {val} --computation-nodes {n}')

        # Step 2: CN keys
        cn_steps: list[str] = [f"rm -rf {cn_keys_dir_bash} || true", f"mkdir -p {cn_keys_dir_bash}"]
        for cn_id in range(n):
            pub = cn_keys_dir / f"cn_{cn_id}.pub.hex"
            sec = cn_keys_dir / f"cn_{cn_id}.sec.hex"
            cn_steps.append(f"./build/consensus/ack_crypto_tool gen-keypair {self._bash_path(pub)} {self._bash_path(sec)}")

        # Step 3: ACK generation
        ack_steps: list[str] = [f"rm -rf {acks_dir_bash} || true", f"mkdir -p {acks_dir_bash}"]
        for pid, _val in providers:
            for party in range(n):
                ack_steps.append(
                    " ".join(
                        [
                            "./build/consensus/share_verifier",
                            f"--session-id {shlex.quote(sess)}",
                            f"--round-id {rid}",
                            f"--protocol-version {shlex.quote(proto)}",
                            f"--schema-id {shlex.quote(schema)}",
                            f"--provider-id {pid}",
                            f"--party-index {party}",
                            "--inputs-dir inputs",
                            "--provider-secrets-dir provider_secrets",
                            f'--share-manifest-path {shlex.quote(f"inputs/provider_{pid}_manifest.json")}',
                            f"--cn-keys-dir {cn_keys_dir_bash}",
                            f"--acks-out-dir {acks_dir_bash}",
                        ]
                    )
                )

        # Step 4: consensus
        consensus_cmd = " ".join(
            [
                "./build/consensus/consensus",
                str(min_inputs),
                f"--acks-dir {acks_dir_bash}",
                f"--num-parties {n}",
                f"--session-id {shlex.quote(sess)}",
                f"--round-id {rid}",
                f"--timeout-seconds {timeout_s}",
                f"--artifacts-dir {artifacts_dir_bash}",
                f"--cn-keys-dir {cn_keys_dir_bash}",
                f"--protocol-version {shlex.quote(proto)}",
                f"--schema-id {shlex.quote(schema)}",
            ]
        )

        bridge_enabled = bool(self.enable_bridge_var.get())
        program = self.program_path_var.get().strip()
        if program:
            bridge_cmd = f"./build/spdz_bridge/spdz_bridge --computation-nodes {n} {self._bash_user_path(program)}"
        else:
            bridge_cmd = f"./build/spdz_bridge/spdz_bridge --computation-nodes {n}"

        seq: list[dict[str, object]] = [
            {"title": "Full-cycle: providers", "command": " && ".join(provider_cmds), "flow_keys": ["providers"], "enabled": True},
            {"title": "Full-cycle: CN keys", "command": " && ".join(cn_steps), "flow_keys": ["ack"], "enabled": True},
            {"title": "Full-cycle: ACK generation", "command": " && ".join(ack_steps), "flow_keys": ["ack"], "enabled": True},
            {"title": "Full-cycle: consensus", "command": self._ensure_configured_prefix() + consensus_cmd, "flow_keys": ["consensus"], "enabled": True},
            {"title": "Full-cycle: bridge / MP-SPDZ", "command": self._ensure_configured_prefix() + bridge_cmd, "flow_keys": ["bridge", "mp_spdz"], "enabled": bridge_enabled},
        ]
        if not bridge_enabled:
            # Show intentional non-execution as skipped.
            self._flow_steps(["bridge", "mp_spdz"], "skipped")

        self._start_step_sequence("Full-cycle workflow", seq)

    # ---------- Validation ----------
    def _run_full_validation(self) -> None:
        self._run_validation_target(None)

    def _run_selected_execution_target(self) -> None:
        """
        Executes either a single validation scenario (via `--only`) or the full suite.
        """
        if self.runner.is_running():
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return

        choice = (self.execution_var.get() or "").strip()
        if choice not in self.execution_targets:
            messagebox.showerror("Invalid", f"Unknown target: {choice}")
            return

        only_key = self.execution_targets[choice]
        self._flow_reset()

        if only_key is None:
            self._run_validation_target(None)
            return

        self._run_validation_target(only_key)

    def _run_validation_target(self, only_key: Optional[str]) -> None:
        """
        Runs the real validation script, optionally restricted via `--only <key>`.
        """
        if self.runner.is_running():
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return

        # Only force WSL on Windows; when running inside WSL/Linux there is no `wsl` executable.
        if is_windows():
            self.use_wsl_var.set(True)
        # Reset flowchart; validation scripts will additionally drive states via GUI_STEP markers.
        self._flow_reset()
        self._flow_case(f"validation{': ' + only_key if only_key else ''}")

        if only_key:
            title = f"Validation: {only_key}"
            safe_only = shlex.quote(only_key)
            cmd = f"bash scripts/full_system_validation_wsl.sh --only {safe_only}"
        else:
            title = "Full validation suite"
            cmd = "bash scripts/full_system_validation_wsl.sh"

        self._run_shelllike(title, cmd)

    def _open_validation_summary(self) -> None:
        if not self.validation_summary_path.exists():
            messagebox.showinfo("Not found", "backend_test_summary.txt not found yet. Run validation first.")
            return
        self._safe_open(self.validation_summary_path)

    def _open_validation_runs(self) -> None:
        if not self.validation_runs_dir.exists():
            messagebox.showinfo("Not found", ".tmp_full_test_runs/ not found yet. Run validation first.")
            return
        self._safe_open(self.validation_runs_dir)

    # ---------- Scenarios ----------
    def _run_selected_scenario(self) -> None:
        messagebox.showinfo(
            "Deprecated",
            "The old Simulation/Validation split has been removed. Use the `Execution / Testing` tab instead.",
        )

    def _run_scenario(self, scenario: str) -> None:
        """
        Implements scenarios directly via binaries:
          - normal: complete ACK coverage
          - insufficient_acks: drop one party ACK for provider 5
          - stale_ack: one ACK timestamp older than timeout window
          - replay_ack: duplicate a signed ACK under a different filename
          - tampered_ack: mutate one ACK after signing (signature must fail)
        """
        if self.runner.is_running():
            messagebox.showwarning("Busy", "A process is already running. Cancel it first.")
            return

        n = int(self.computation_nodes_var.get())
        if n < 2:
            messagebox.showerror("Invalid", "num_parties must be >= 2 for scenarios.")
            return

        # Use fixed provider set for scenarios (like the validation script).
        providers = [(1, 10), (2, 20), (3, 30), (4, 40), (5, 50)]
        sess = f"gui-{scenario}"
        rid = max(1, int(self.round_id_var.get()))
        schema = self.schema_id_var.get().strip()
        proto = self.protocol_version_var.get().strip()

        acks_dir = (REPO_ROOT / Path(self.acks_dir_var.get())).resolve()
        cn_keys_dir = (REPO_ROOT / Path(self.cn_keys_dir_var.get())).resolve()
        acks_dir_bash = self._bash_path(acks_dir)
        cn_keys_dir_bash = self._bash_path(cn_keys_dir)
        artifacts_dir_bash = self._bash_path(self.artifacts_dir)

        # timeout: used for stale_ack only (keep others at 0)
        timeout_s = 2 if scenario == "stale_ack" else 0

        steps: list[str] = []
        steps.append("rm -rf inputs logs artifacts core_set.txt provider_secrets || true")
        steps.append("mkdir -p inputs logs artifacts provider_secrets")
        if self.auto_configure_build_var.get():
            steps.append("([ -f build/CMakeCache.txt ] || cmake -S . -B build)")

        for pid, val in providers:
            steps.append(f'./build/node/data_provider {pid} {val} --computation-nodes {n}')

        steps.append(f"rm -rf {acks_dir_bash} {cn_keys_dir_bash} || true")
        steps.append(f"mkdir -p {acks_dir_bash} {cn_keys_dir_bash}")
        for cn_id in range(n):
            pub = cn_keys_dir / f"cn_{cn_id}.pub.hex"
            sec = cn_keys_dir / f"cn_{cn_id}.sec.hex"
            steps.append(
                f"./build/consensus/ack_crypto_tool gen-keypair {self._bash_path(pub)} {self._bash_path(sec)}"
            )

        # ACK generation with optional timestamp override
        ts_ok = now_unix_ms()
        ts_stale = ts_ok - (10_000)
        for pid, _val in providers:
            for party in range(n):
                if scenario == "insufficient_acks" and pid == 5 and party == (n - 1):
                    continue
                ts = ts_stale if (scenario == "stale_ack" and pid == 5 and party == 0) else ts_ok
                steps.append(
                    " ".join(
                        [
                            "./build/consensus/share_verifier",
                            f"--session-id {shlex.quote(sess)}",
                            f"--round-id {rid}",
                            f"--protocol-version {shlex.quote(proto)}",
                            f"--schema-id {shlex.quote(schema)}",
                            f"--provider-id {pid}",
                            f"--party-index {party}",
                            "--inputs-dir inputs",
                            "--provider-secrets-dir provider_secrets",
                            f'--share-manifest-path {shlex.quote(f"inputs/provider_{pid}_manifest.json")}',
                            f"--cn-keys-dir {cn_keys_dir_bash}",
                            f"--acks-out-dir {acks_dir_bash}",
                            f"--timestamp-unix-ms {ts}",
                        ]
                    )
                )

        # Post-processing mutations for replay/tamper.
        if scenario == "replay_ack":
            steps.append(
                f'cp -f {shlex.quote(str(acks_dir / "ack_p5_party0.json"))} '
                f'{shlex.quote(str(acks_dir / "ack_p5_party0_replay.json"))} || true'
            )
        if scenario == "tampered_ack":
            # Mutate share_file_digest after signing -> signature check must fail.
            ack_path = acks_dir / "ack_p5_party0.json"
            py = (
                "python3 - <<'PY'\n"
                "import json\n"
                "from pathlib import Path\n"
                f"p = Path({json.dumps(str(ack_path))})\n"
                "if p.exists():\n"
                "    d = json.loads(p.read_text(encoding='utf-8'))\n"
                "    d['share_file_digest'] = '0' * 64\n"
                "    p.write_text(json.dumps(d, indent=2) + '\\n', encoding='utf-8')\n"
                "PY"
            )
            steps.append(py)

        # Run consensus (ACK mandatory).
        steps.append(
            " ".join(
                [
                    "./build/consensus/consensus",
                    "5",
                    f"--acks-dir {acks_dir_bash}",
                    f"--num-parties {n}",
                    f"--session-id {shlex.quote(sess)}",
                    f"--round-id {rid}",
                    f"--timeout-seconds {timeout_s}",
                    f"--artifacts-dir {artifacts_dir_bash}",
                    f"--cn-keys-dir {cn_keys_dir_bash}",
                    f"--protocol-version {shlex.quote(proto)}",
                    f"--schema-id {shlex.quote(schema)}",
                ]
            )
        )

        self._run_shelllike(f"Scenario: {scenario}", " && ".join(steps))


def main() -> None:
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()

