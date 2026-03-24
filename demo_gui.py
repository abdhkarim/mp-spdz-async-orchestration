#!/usr/bin/env python3
"""
Interface graphique de démonstration pour le pipeline MP-SPDZ orchestré de façon asynchrone.

But de cette interface :
- lancer facilement les vraies étapes du projet ;
- montrer visuellement le chemin providers → consensus → bridge → MP-SPDZ ;
- afficher les logs réels pour suivre ce qui s’exécute.

Structure attendue :
repo/
  demo_gui_visual.py
  build/node/data_provider
  build/consensus/consensus
  build/spdz_bridge/spdz_bridge
  programs/sum.mpc
"""

from __future__ import annotations

import os
import platform
import sys
import shlex
import shutil
import subprocess
import threading
from pathlib import Path
import tkinter as tk
from tkinter import messagebox, ttk

# Chemins principaux du projet.
REPO_ROOT = Path(__file__).resolve().parent
BUILD_DIR = REPO_ROOT / "build"
IS_WINDOWS = platform.system().lower().startswith("win")

# Couleurs de l’interface.
BG = "#0f172a"
PANEL = "#111827"
CARD = "#1f2937"
CARD_ALT = "#0b1220"
TEXT = "#e5e7eb"
MUTED = "#9ca3af"
ACCENT = "#38bdf8"
SUCCESS = "#22c55e"
WARN = "#f59e0b"
ERROR = "#ef4444"
IDLE = "#334155"
LINE = "#475569"

# Polices utilisées dans la GUI.
FONT = ("Segoe UI", 10)
FONT_BOLD = ("Segoe UI Semibold", 10)
FONT_TITLE = ("Segoe UI Semibold", 18)
FONT_BIG = ("Segoe UI Semibold", 13)
FONT_MONO = ("Consolas", 10)

# Taille de référence du schéma pipeline sur le canvas (scaling = grandissement / réduction).
_PIPELINE_LAYOUT_W = 760.0
_PIPELINE_LAYOUT_H = 430.0


class VisualDemoGUI(tk.Tk):
    def __init__(self) -> None:
        # Initialisation générale de la fenêtre.
        super().__init__()
        self.title("Async MPC Demo GUI — Visual Pipeline")
        self._apply_screen_fit_geometry()
        self.configure(bg=BG)
        self.resizable(True, True)

        # Variables affichées dans les cartes d’état.
        self.var_result = tk.StringVar(value="Result: waiting")
        self.var_core = tk.StringVar(value="Core set: waiting")
        self.var_status = tk.StringVar(value="Status: idle")
        self.var_wsl = tk.BooleanVar(value=False)

        # Ces dictionnaires servent à garder les éléments dessinés dans le schéma.
        self.pipeline_items: dict[str, int] = {}
        self.pipeline_labels: dict[str, int] = {}
        self.pipeline_lines: dict[str, int] = {}
        self.provider_visual_slots = [1, 2, 3]

        # Construction de l’interface et état initial.
        self._build_style()
        self._build_ui()
        self._draw_pipeline()
        self._set_stage("provider1", "idle")
        self._set_stage("provider2", "idle")
        self._set_stage("provider3", "idle")
        self._set_stage("consensus", "idle")
        self._set_stage("bridge", "idle")
        self._set_stage("mpspdz", "idle")
        self._check_build_available(initial=True)

    def _apply_screen_fit_geometry(self) -> None:
        """Ouvre la fenêtre en fonction de l’écran (pas de taille figée 1920×1080)."""
        self.update_idletasks()
        sw = max(1, int(self.winfo_screenwidth()))
        sh = max(1, int(self.winfo_screenheight()))
        margin_x, margin_y = 32, 80

        usable_w = max(640, sw - margin_x)
        usable_h = max(480, sh - margin_y)

        w = int(usable_w * 0.92)
        h = int(usable_h * 0.90)
        w = max(720, min(w, usable_w))
        h = max(520, min(h, usable_h))

        if w > sw - 8:
            w = sw - 8
        if h > sh - 8:
            h = sh - 8

        x = max(0, (sw - w) // 2)
        y = max(0, (sh - h) // 12)
        self.geometry(f"{w}x{h}+{x}+{y}")

        # Minimum redimensionnable : jamais plus grand que la fenêtre d’ouverture ni que l’écran.
        min_w = max(560, min(840, sw - margin_x))
        min_h = max(400, min(640, sh - margin_y))
        min_w = max(480, min(min_w, w))
        min_h = max(360, min(min_h, h))
        self.minsize(min_w, min_h)

    # ---------- UI ----------
    def _build_style(self) -> None:
        # Configure un style sombre simple pour les widgets ttk.
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TEntry", fieldbackground="#0b1220", foreground=TEXT, insertcolor=TEXT)
        style.configure("TCheckbutton", background=PANEL, foreground=TEXT)

    def _build_ui(self) -> None:
        # Construit les grandes zones de la fenêtre.
        root = tk.Frame(self, bg=BG)
        root.pack(fill="both", expand=True, padx=16, pady=16)
        root.grid_columnconfigure(0, weight=0)
        root.grid_columnconfigure(1, weight=1)
        root.grid_rowconfigure(1, weight=1)
        root.grid_rowconfigure(2, weight=1)

        header = tk.Frame(root, bg=BG)
        header.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 12))
        tk.Label(header, text="Async MPC Demo", font=FONT_TITLE, bg=BG, fg=TEXT).pack(anchor="w")
        tk.Label(
            header,
            text="Visual explanation of providers → consensus → bridge → MP-SPDZ, with the real binaries running underneath.",
            font=FONT,
            bg=BG,
            fg=MUTED,
        ).pack(anchor="w", pady=(4, 0))

        self.sidebar = tk.Frame(root, bg=PANEL, bd=0, highlightthickness=1, highlightbackground="#263244")
        self.sidebar.grid(row=1, column=0, rowspan=2, sticky="nsew", padx=(0, 12))
        self.sidebar.grid_columnconfigure(0, weight=1)

        self.main_top = tk.Frame(root, bg=BG)
        self.main_top.grid(row=1, column=1, sticky="nsew")
        self.main_top.grid_columnconfigure(0, weight=3)
        self.main_top.grid_columnconfigure(1, weight=2)
        self.main_top.grid_rowconfigure(0, weight=1)

        self.main_bottom = tk.Frame(root, bg=BG)
        self.main_bottom.grid(row=2, column=1, sticky="nsew", pady=(12, 0))
        self.main_bottom.grid_columnconfigure(0, weight=1)
        self.main_bottom.grid_rowconfigure(0, weight=1)

        self._build_sidebar()
        self._build_visual_panel()
        self._build_status_cards()
        self._build_log_panel()

    def _section(self, parent: tk.Widget, title: str, subtitle: str | None = None) -> tk.Frame:
        # Crée une petite section avec titre et sous-titre.
        wrap = tk.Frame(parent, bg=PANEL)
        wrap.pack(fill="x", padx=14, pady=(12, 0))
        tk.Label(wrap, text=title, font=FONT_BIG, bg=PANEL, fg=TEXT).pack(anchor="w")
        if subtitle:
            tk.Label(wrap, text=subtitle, font=FONT, bg=PANEL, fg=MUTED, justify="left", wraplength=360).pack(anchor="w", pady=(2, 8))
        return wrap

    def _labeled_entry(self, parent: tk.Widget, label: str, help_text: str, default: str, width: int = 28) -> tk.Entry:
        # Crée un champ texte avec un label et une aide courte.
        frame = tk.Frame(parent, bg=PANEL)
        frame.pack(fill="x", pady=(0, 10))
        tk.Label(frame, text=label, font=FONT_BOLD, bg=PANEL, fg=TEXT).pack(anchor="w")
        entry = tk.Entry(frame, width=width, bg="#0b1220", fg=TEXT, insertbackground=TEXT, relief="flat", font=FONT)
        entry.pack(fill="x", pady=(5, 4), ipady=7)
        entry.insert(0, default)
        tk.Label(frame, text=help_text, font=("Segoe UI", 9), bg=PANEL, fg=MUTED, wraplength=360, justify="left").pack(anchor="w")
        return entry

    def _build_sidebar(self) -> None:
        # Barre latérale scrollable : tout le contenu peut dépasser la hauteur de fenêtre
        # (évite que les boutons « Actions » soient coupés sur petits écrans).
        shell = tk.Frame(self.sidebar, bg=PANEL)
        shell.pack(fill="both", expand=True)
        shell.grid_rowconfigure(0, weight=1)
        shell.grid_columnconfigure(0, weight=1)

        self._sidebar_canvas = tk.Canvas(
            shell,
            bg=PANEL,
            highlightthickness=0,
            bd=0,
        )
        vsb = tk.Scrollbar(
            shell,
            orient="vertical",
            command=self._sidebar_canvas.yview,
            bg=CARD_ALT,
            troughcolor=PANEL,
            activebackground=LINE,
            width=14,
            borderwidth=0,
            highlightthickness=0,
        )
        self._sidebar_canvas.configure(yscrollcommand=vsb.set)
        self._sidebar_body = tk.Frame(self._sidebar_canvas, bg=PANEL)
        self._sidebar_body_id = self._sidebar_canvas.create_window(
            (0, 0), window=self._sidebar_body, anchor="nw"
        )

        def _sync_sidebar_scroll(_event: object | None = None) -> str:
            self._sidebar_canvas.configure(scrollregion=self._sidebar_canvas.bbox("all"))
            return ""

        def _stretch_sidebar_inner(event: tk.Event) -> str:
            self._sidebar_canvas.itemconfigure(self._sidebar_body_id, width=event.width)
            return ""

        self._sidebar_body.bind("<Configure>", lambda e: _sync_sidebar_scroll())
        self._sidebar_canvas.bind("<Configure>", _stretch_sidebar_inner)

        self._sidebar_canvas.grid(row=0, column=0, sticky="nsew")
        vsb.grid(row=0, column=1, sticky="ns")

        def _sidebar_contains(widget: tk.Misc | None) -> bool:
            while widget is not None:
                if widget in (self._sidebar_body, self._sidebar_canvas, vsb):
                    return True
                widget = widget.master  # type: ignore[assignment]

            return False

        def _on_sidebar_wheel(event: tk.Event) -> None:
            if not _sidebar_contains(event.widget):
                return
            if getattr(event, "delta", 0):
                self._sidebar_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

        def _on_sidebar_linux_up(event: tk.Event) -> None:
            if _sidebar_contains(event.widget):
                self._sidebar_canvas.yview_scroll(-1, "units")

        def _on_sidebar_linux_down(event: tk.Event) -> None:
            if _sidebar_contains(event.widget):
                self._sidebar_canvas.yview_scroll(1, "units")

        self.bind_all("<MouseWheel>", _on_sidebar_wheel, add="+")
        if sys.platform.startswith("linux"):
            self.bind_all("<Button-4>", _on_sidebar_linux_up, add="+")
            self.bind_all("<Button-5>", _on_sidebar_linux_down, add="+")

        body = self._sidebar_body
        sec = self._section(
            body,
            "Input controls",
            "Fill the values below, then run a single step or the whole demo. The visual diagram on the right will animate the flow.",
        )
        self.entry_provider_id = self._labeled_entry(sec, "Provider ID", "Example: 1, 2, or 3. This is the sender node identifier.", "1")
        self.entry_value = self._labeled_entry(sec, "Provider value", "Example: 10. This is the private input contributed by that provider.", "10")
        self.entry_computation_nodes = self._labeled_entry(sec, "Computation nodes (N)", "Usually 3. Must match the bridge / MP-SPDZ party count.", "3")
        self.entry_consensus_args = self._labeled_entry(sec, "Consensus args", "Example: 2 --clean-inputs. Here 2 means the minimum number of valid inputs required.", "2 --clean-inputs")
        self.entry_bridge_args = self._labeled_entry(sec, "Bridge args", "Main example: programs/sum.mpc. Do not put --backend here; the current bridge CLI no longer accepts it.", "programs/sum.mpc")

        options = self._section(body, "Runtime")
        chk = tk.Checkbutton(
            options,
            text="Run commands in WSL",
            variable=self.var_wsl,
            bg=PANEL,
            fg=TEXT,
            selectcolor=CARD_ALT,
            activebackground=PANEL,
            activeforeground=TEXT,
            font=FONT,
            highlightthickness=0,
        )
        chk.pack(anchor="w", pady=(0, 8))
        tk.Label(
            options,
            text="Enable this when your binaries were built inside WSL/Linux and you launch the GUI from Windows.",
            bg=PANEL,
            fg=MUTED,
            font=("Segoe UI", 9),
            wraplength=360,
            justify="left",
        ).pack(anchor="w")

        actions = self._section(
            body,
            "Actions",
            "Single-step buttons help explain the pipeline. The full scenario launches provider 1 = 10, provider 2 = 3, consensus with quorum 2, then the bridge.",
        )
        btn_row1 = tk.Frame(actions, bg=PANEL)
        btn_row1.pack(fill="x", pady=(2, 8))
        self._button(btn_row1, "Run provider", self.run_provider, accent=False).pack(side="left", fill="x", expand=True)
        self._button(btn_row1, "Run consensus", self.run_consensus, accent=False).pack(side="left", fill="x", expand=True, padx=(8, 0))

        btn_row2 = tk.Frame(actions, bg=PANEL)
        btn_row2.pack(fill="x", pady=(0, 8))
        self._button(btn_row2, "Run bridge", self.run_bridge, accent=False).pack(side="left", fill="x", expand=True)
        self._button(btn_row2, "Reset workspace", self.reset_workspace, accent=False).pack(side="left", fill="x", expand=True, padx=(8, 0))

        self._button(actions, "Run full scenario", self.run_full_scenario, accent=True).pack(fill="x", pady=(2, 4))

        # Petite légende pour comprendre les couleurs du schéma.
        notes = self._section(body, "How to read the picture")
        legend = tk.Frame(notes, bg=PANEL)
        legend.pack(fill="x")
        self._legend_item(legend, IDLE, "Idle")
        self._legend_item(legend, WARN, "Running")
        self._legend_item(legend, SUCCESS, "Success")
        self._legend_item(legend, ERROR, "Error")
        tk.Label(
            notes,
            text="Providers send validated inputs to Consensus. Consensus writes the core set. Bridge prepares MP-SPDZ Player-Data, then MP-SPDZ computes the secure result.",
            bg=PANEL,
            fg=MUTED,
            font=("Segoe UI", 9),
            wraplength=360,
            justify="left",
        ).pack(anchor="w", pady=(8, 0))

    def _legend_item(self, parent: tk.Widget, color: str, text: str) -> None:
        # Affiche un rond coloré avec son label.
        row = tk.Frame(parent, bg=PANEL)
        row.pack(anchor="w", pady=2)
        tk.Canvas(row, width=14, height=14, bg=PANEL, highlightthickness=0).pack(side="left")
        c = row.winfo_children()[-1]
        c.create_oval(2, 2, 12, 12, fill=color, outline=color)
        tk.Label(row, text=text, bg=PANEL, fg=TEXT, font=("Segoe UI", 9)).pack(side="left", padx=(6, 0))

    def _button(self, parent: tk.Widget, text: str, command, accent: bool) -> tk.Button:
        # Crée un bouton avec le style visuel du projet.
        return tk.Button(
            parent,
            text=text,
            command=command,
            bg=ACCENT if accent else CARD,
            fg="#08111d" if accent else TEXT,
            activebackground="#67e8f9" if accent else "#2d3a4d",
            activeforeground="#08111d" if accent else TEXT,
            relief="flat",
            bd=0,
            padx=14,
            pady=11,
            font=FONT_BOLD,
            cursor="hand2",
        )

    def _build_visual_panel(self) -> None:
        # Zone centrale qui affiche le pipeline visuel.
        panel = tk.Frame(self.main_top, bg=PANEL, highlightthickness=1, highlightbackground="#263244")
        panel.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        tk.Label(panel, text="Visual pipeline", bg=PANEL, fg=TEXT, font=FONT_BIG).pack(anchor="w", padx=14, pady=(12, 0))
        tk.Label(panel, text="Nodes light up as the real commands execute. This makes the dataflow easier to explain in a demo.", bg=PANEL, fg=MUTED, font=FONT).pack(anchor="w", padx=14, pady=(2, 8))

        # Hauteur initiale liée à l’écran ; le canvas grandit avec la grille (expand=True).
        _sh = int(self.winfo_screenheight())
        _canvas_h = max(280, min(560, int(_sh * 0.22)))
        self.canvas = tk.Canvas(panel, bg=CARD_ALT, highlightthickness=0, height=_canvas_h)
        self.canvas.pack(fill="both", expand=True, padx=14, pady=(0, 14))
        self.canvas.bind("<Configure>", lambda _e: self._draw_pipeline())

    def _build_status_cards(self) -> None:
        # Cartes à droite pour afficher les infos importantes.
        side = tk.Frame(self.main_top, bg=BG)
        side.grid(row=0, column=1, sticky="nsew")
        side.grid_rowconfigure(3, weight=1)
        side.grid_columnconfigure(0, weight=1)

        self._info_card(side, "Result", self.var_result, 0)
        self._info_card(side, "Core set", self.var_core, 1)
        self._info_card(side, "Status", self.var_status, 2)

        help_card = tk.Frame(side, bg=PANEL, highlightthickness=1, highlightbackground="#263244")
        help_card.grid(row=3, column=0, sticky="nsew")
        tk.Label(help_card, text="Quick explanation", bg=PANEL, fg=TEXT, font=FONT_BIG).pack(anchor="w", padx=14, pady=(12, 0))
        bullets = (
            "1) Providers mask their values and write validated input files.\n\n"
            "2) Consensus chooses which providers are accepted into the core set.\n\n"
            "3) The bridge prepares MP-SPDZ Player-Data from that validated set.\n\n"
            "4) MP-SPDZ runs the secure computation and outputs the result."
        )
        tk.Label(help_card, text=bullets, bg=PANEL, fg=MUTED, font=FONT, justify="left", wraplength=390).pack(anchor="w", padx=14, pady=(8, 14))

    def _info_card(self, parent: tk.Widget, title: str, var: tk.StringVar, row: int) -> None:
        # Carte simple : titre + valeur dynamique.
        card = tk.Frame(parent, bg=PANEL, highlightthickness=1, highlightbackground="#263244")
        card.grid(row=row, column=0, sticky="ew", pady=(0, 12))
        tk.Label(card, text=title, bg=PANEL, fg=MUTED, font=FONT).pack(anchor="w", padx=14, pady=(10, 0))
        tk.Label(card, textvariable=var, bg=PANEL, fg=TEXT, font=FONT_BIG, wraplength=390, justify="left").pack(anchor="w", padx=14, pady=(4, 12))

    def _build_log_panel(self) -> None:
        # Zone de logs pour voir stdout/stderr des commandes lancées.
        panel = tk.Frame(self.main_bottom, bg=PANEL, highlightthickness=1, highlightbackground="#263244")
        panel.grid(row=0, column=0, sticky="nsew")
        tk.Label(panel, text="Execution log", bg=PANEL, fg=TEXT, font=FONT_BIG).pack(anchor="w", padx=14, pady=(12, 0))
        tk.Label(panel, text="This is the real stdout/stderr of the tools. Useful both for debugging and for showing what actually ran.", bg=PANEL, fg=MUTED, font=FONT).pack(anchor="w", padx=14, pady=(2, 8))

        self.log = tk.Text(panel, bg="#020617", fg=TEXT, insertbackground=TEXT, relief="flat", font=FONT_MONO, wrap="word")
        self.log.pack(fill="both", expand=True, padx=14, pady=(0, 14))
        self.log.tag_configure("cmd", foreground=ACCENT)
        self.log.tag_configure("ok", foreground=SUCCESS)
        self.log.tag_configure("err", foreground=ERROR)
        self.log.tag_configure("warn", foreground=WARN)

    # ---------- visual pipeline ----------
    def _draw_pipeline(self) -> None:
        # Redessine tout le schéma du pipeline (mise à l’échelle selon la taille réelle du canvas).
        c = self.canvas
        c.delete("all")
        raw_w = c.winfo_width()
        raw_h = c.winfo_height()
        if raw_w <= 1 or raw_h <= 1:
            return

        scale = min(raw_w / _PIPELINE_LAYOUT_W, raw_h / _PIPELINE_LAYOUT_H)
        off_x = (raw_w - _PIPELINE_LAYOUT_W * scale) / 2.0
        off_y = (raw_h - _PIPELINE_LAYOUT_H * scale) / 2.0

        def pt(x: float, y: float) -> tuple[float, float]:
            return (off_x + x * scale, off_y + y * scale)

        base_positions = {
            "provider1": (120, 110),
            "provider2": (120, 235),
            "provider3": (120, 360),
            "consensus": (370, 235),
            "bridge": (610, 170),
            "mpspdz": (610, 315),
        }
        positions = {k: pt(xy[0], xy[1]) for k, xy in base_positions.items()}

        self.pipeline_lines.clear()
        self.pipeline_items.clear()
        self.pipeline_labels.clear()

        # Lignes de circulation des données.
        self._draw_line("p1_cons", positions["provider1"], positions["consensus"], "validated input", scale)
        self._draw_line("p2_cons", positions["provider2"], positions["consensus"], "validated input", scale)
        self._draw_line("p3_cons", positions["provider3"], positions["consensus"], "optional / missing", scale)
        self._draw_line("cons_bridge", positions["consensus"], positions["bridge"], "core_set.txt", scale)
        self._draw_line("bridge_mps", positions["bridge"], positions["mpspdz"], "Player-Data + run", scale)

        # Nœuds du pipeline (rayons proportionnels).
        self._draw_node("provider1", *positions["provider1"], 72 * scale, "Provider 1", "Private input\nmask + write file", scale)
        self._draw_node("provider2", *positions["provider2"], 72 * scale, "Provider 2", "Private input\nmask + write file", scale)
        self._draw_node("provider3", *positions["provider3"], 72 * scale, "Provider 3", "Optional / late\ncan be ignored", scale)
        self._draw_node("consensus", *positions["consensus"], 84 * scale, "Consensus", "Validate inputs\nselect core set", scale)
        self._draw_node("bridge", *positions["bridge"], 84 * scale, "Bridge", "Prepare MP-SPDZ\nPlayer-Data", scale)
        self._draw_node("mpspdz", *positions["mpspdz"], 84 * scale, "MP-SPDZ", "Secure compute\nreturn result", scale)

        tx, ty = pt(610, 55)
        fz = max(7, min(11, int(round(10 * scale))))
        c.create_text(tx, ty, text="Secure computation zone", fill=MUTED, font=("Segoe UI", fz, "italic"))

        bx0, by0 = pt(500, 90)
        bx1, by1 = pt(720, 395)
        dash_pat = (max(3, int(round(6 * scale))), max(2, int(round(4 * scale))))
        c.create_rectangle(bx0, by0, bx1, by1, outline="#234156", dash=dash_pat)

    def _draw_line(
        self,
        key: str,
        p1: tuple[float, float],
        p2: tuple[float, float],
        text: str,
        scale: float,
    ) -> None:
        # Dessine une flèche entre deux composants.
        c = self.canvas
        x1, y1 = p1
        x2, y2 = p2
        inset = 76.0 * scale
        outset = 90.0 * scale
        lw = max(1.0, 3.0 * scale)
        line = c.create_line(
            x1 + inset,
            y1,
            x2 - outset,
            y2,
            fill=LINE,
            width=lw,
            arrow=tk.LAST,
            smooth=True,
        )
        tx = (x1 + x2) / 2 + 5.0 * scale
        ty = (y1 + y2) / 2 - 18.0 * scale
        fs = max(7, min(10, int(round(9 * scale))))
        lbl = c.create_text(tx, ty, text=text, fill=MUTED, font=("Segoe UI", fs))
        self.pipeline_lines[key] = line
        self.pipeline_labels[key] = lbl

    def _draw_node(
        self,
        key: str,
        x: float,
        y: float,
        r: float,
        title: str,
        subtitle: str,
        scale: float,
    ) -> None:
        # Dessine un nœud du schéma avec son titre.
        c = self.canvas
        squash = 10.0 * scale
        ow = max(1, int(round(2 * scale)))
        oval = c.create_oval(
            x - r,
            y - r + squash,
            x + r,
            y + r - squash,
            fill=IDLE,
            outline="#5b6b80",
            width=ow,
        )
        title_fs = max(8, min(11, int(round(10 * scale))))
        sub_fs = max(7, min(9, int(round(9 * scale))))
        c.create_text(x, y - 12.0 * scale, text=title, fill=TEXT, font=("Segoe UI Semibold", title_fs))
        c.create_text(x, y + 20.0 * scale, text=subtitle, fill="#d1d5db", font=("Segoe UI", sub_fs), justify="center")
        self.pipeline_items[key] = oval

    def _set_stage(self, stage: str, state: str) -> None:
        # Change la couleur d’un bloc selon son état.
        if stage not in self.pipeline_items:
            return
        fill = {"idle": IDLE, "running": WARN, "success": SUCCESS, "error": ERROR}.get(state, IDLE)
        outline = {"idle": "#5b6b80", "running": "#fbbf24", "success": "#86efac", "error": "#fca5a5"}.get(state, "#5b6b80")
        self.canvas.itemconfig(self.pipeline_items[stage], fill=fill, outline=outline)
        self.update_idletasks()

    # ---------- helpers ----------
    def append(self, text: str, tag: str | None = None) -> None:
        # Ajoute du texte dans le log et scroll automatiquement.
        self.log.insert("end", text, tag or "")
        self.log.see("end")
        self.update_idletasks()

    def set_status(self, text: str) -> None:
        # Met à jour le statut général.
        self.var_status.set(f"Status: {text}")
        self.update_idletasks()

    def get_computation_nodes(self) -> int:
        # Récupère N et vérifie que c’est un entier positif.
        try:
            n = int(self.entry_computation_nodes.get().strip())
            if n <= 0:
                raise ValueError
            return n
        except ValueError:
            raise ValueError("Computation nodes (N) must be a positive integer.")

    def to_wsl_path(self, path: Path) -> str:
        # Convertit un chemin Windows vers le format WSL.
        path = path.resolve()
        s = str(path).replace("\\", "/")
        if len(s) > 1 and s[1] == ":":
            drive = s[0].lower()
            return f"/mnt/{drive}{s[2:]}"
        return s

    def _binary_candidates(self) -> list[Path]:
        # Liste des binaires que la GUI doit trouver.
        suffix = ".exe" if IS_WINDOWS and not self.var_wsl.get() else ""
        return [
            BUILD_DIR / "node" / f"data_provider{suffix}",
            BUILD_DIR / "consensus" / f"consensus{suffix}",
            BUILD_DIR / "spdz_bridge" / f"spdz_bridge{suffix}",
        ]

    def _check_build_available(self, initial: bool = False) -> bool:
        # Vérifie que les exécutables compilés existent bien.
        missing = [str(p) for p in self._binary_candidates() if not p.exists()]
        if not missing:
            return True
        msg = "Missing build artifacts:\n\n" + "\n".join(missing) + "\n\nBuild first with CMake from the repository root."
        if not initial:
            messagebox.showerror("Build missing", msg)
        self.set_status("build missing")
        return False

    def _native_run(self, args: list[str]) -> tuple[int, str]:
        # Lance une commande localement depuis la racine du dépôt.
        self.append(f"$ (cwd={REPO_ROOT}) {' '.join(args)}\n", "cmd")
        proc = subprocess.run(
            args,
            cwd=str(REPO_ROOT),
            text=True,
            capture_output=True,
            shell=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.append(output)
        return proc.returncode, output

    def _wsl_run(self, args: list[str]) -> tuple[int, str]:
        # Lance une commande dans WSL.
        repo_wsl = shlex.quote(self.to_wsl_path(REPO_ROOT))
        cmd = "cd " + repo_wsl + " && " + " ".join(shlex.quote(a) for a in args)
        shown = f"$ (cwd={REPO_ROOT}) {' '.join(args)}\n"
        self.append(shown, "cmd")
        proc = subprocess.run(
            ["wsl", "-e", "bash", "-lc", cmd],
            text=True,
            capture_output=True,
            shell=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.append(output)
        return proc.returncode, output

    def run_logged(self, args: list[str], kind: str, stage: str | None = None) -> tuple[int, str]:
        # Lance une commande, met à jour le schéma, puis écrit le résultat dans le log.
        if stage:
            self._set_stage(stage, "running")
        runner = self._wsl_run if self.var_wsl.get() else self._native_run
        rc, output = runner(args)
        tag = "ok" if rc == 0 else "err"
        self.append(f"[{kind}] exit={rc}\n\n", tag)
        if stage:
            self._set_stage(stage, "success" if rc == 0 else "error")
        self._parse_summary(output)
        return rc, output

    def _parse_summary(self, text: str) -> None:
        # Cherche dans les logs le core set et le résultat final.
        for line in text.splitlines():
            if "Core set decided" in line:
                self.var_core.set(f"Core set: {line.strip()}")
            elif "MP-SPDZ result:" in line:
                self.var_result.set(f"Result: {line.split('MP-SPDZ result:', 1)[1].strip()}")
            elif "Fallback sum =" in line:
                self.var_result.set(f"Result: {line.split('Fallback sum =', 1)[1].strip()} (fallback)")
            elif "Fallback plaintext sum =" in line:
                self.var_result.set(f"Result: {line.split('Fallback plaintext sum =', 1)[1].strip()} (fallback)")

    def run_in_thread(self, fn) -> None:
        # Lance une tâche dans un thread pour ne pas bloquer l’interface.
        threading.Thread(target=fn, daemon=True).start()

    # ---------- actions ----------
    def run_provider(self) -> None:
        # Lance un provider avec les valeurs saisies.
        if not self._check_build_available():
            return
        def task():
            try:
                pid = int(self.entry_provider_id.get().strip())
                value = self.entry_value.get().strip()
                n = self.get_computation_nodes()
            except ValueError as e:
                messagebox.showerror("Invalid input", str(e))
                return

            stage = f"provider{pid}" if pid in (1, 2, 3) else None
            self.set_status(f"running provider {pid}")
            rc, _ = self.run_logged(["build/node/data_provider", str(pid), value, "--computation-nodes", str(n)], kind="provider", stage=stage)
            self.set_status("provider completed" if rc == 0 else "provider failed")
        self.run_in_thread(task)

    def run_consensus(self) -> None:
        # Lance le consensus avec les arguments saisis.
        if not self._check_build_available():
            return
        def task():
            args = shlex.split(self.entry_consensus_args.get().strip())
            self.set_status("running consensus")
            rc, _ = self.run_logged(["build/consensus/consensus"] + args, kind="consensus", stage="consensus")
            self.set_status("consensus completed" if rc == 0 else "consensus failed")
        self.run_in_thread(task)

    def run_bridge(self) -> None:
        # Lance le bridge puis reflète l’activité MP-SPDZ dans le schéma.
        if not self._check_build_available():
            return
        def task():
            try:
                n = self.get_computation_nodes()
            except ValueError as e:
                messagebox.showerror("Invalid input", str(e))
                return
            extra = shlex.split(self.entry_bridge_args.get().strip())
            self.set_status("running bridge + MP-SPDZ")
            self._set_stage("bridge", "running")
            self._set_stage("mpspdz", "idle")
            rc, output = self.run_logged(["build/spdz_bridge/spdz_bridge"] + extra + ["--computation-nodes", str(n)], kind="bridge", stage="bridge")
            if "Compiling" in output or "MP-SPDZ result:" in output or "semi2k-party.x" in output:
                self._set_stage("mpspdz", "success" if rc == 0 else "error")
            self.set_status("bridge completed" if rc == 0 else "bridge failed")
        self.run_in_thread(task)

    def reset_workspace(self) -> None:
        # Supprime les fichiers générés pour repartir d’un état propre.
        def task():
            self.set_status("resetting workspace")
            self.var_result.set("Result: waiting")
            self.var_core.set("Core set: waiting")
            for stage in ["provider1", "provider2", "provider3", "consensus", "bridge", "mpspdz"]:
                self._set_stage(stage, "idle")
            if self.var_wsl.get():
                cmd = "rm -rf inputs logs core_set.txt provider_secrets artifacts && mkdir -p inputs logs"
                repo_wsl = shlex.quote(self.to_wsl_path(REPO_ROOT))
                shown = "$ (cwd={}) reset workspace\n".format(REPO_ROOT)
                self.append(shown, "cmd")
                proc = subprocess.run(["wsl", "-e", "bash", "-lc", f"cd {repo_wsl} && {cmd}"], text=True, capture_output=True)
                out = (proc.stdout or "") + (proc.stderr or "")
                self.append(out)
                self.append(f"[reset] exit={proc.returncode}\n\n", "ok" if proc.returncode == 0 else "err")
                self.set_status("workspace reset" if proc.returncode == 0 else "reset failed")
                return

            for target in [REPO_ROOT / "inputs", REPO_ROOT / "logs", REPO_ROOT / "provider_secrets", REPO_ROOT / "artifacts"]:
                if target.exists():
                    shutil.rmtree(target, ignore_errors=True)
            core = REPO_ROOT / "core_set.txt"
            if core.exists():
                try:
                    core.unlink()
                except OSError:
                    pass
            (REPO_ROOT / "inputs").mkdir(parents=True, exist_ok=True)
            (REPO_ROOT / "logs").mkdir(parents=True, exist_ok=True)
            self.append(f"$ (cwd={REPO_ROOT}) reset workspace\n", "cmd")
            self.append("Removed inputs, logs, core_set.txt, provider_secrets, artifacts\n")
            self.append("[reset] exit=0\n\n", "ok")
            self.set_status("workspace reset")
        self.run_in_thread(task)

    def run_full_scenario(self) -> None:
        # Joue une démo complète : 2 providers, consensus, puis bridge.
        if not self._check_build_available():
            return
        def task():
            try:
                n = self.get_computation_nodes()
            except ValueError as e:
                messagebox.showerror("Invalid input", str(e))
                return

            self.set_status("running full scenario")
            self.var_result.set("Result: running")
            self.var_core.set("Core set: running")
            for stage in ["provider1", "provider2", "provider3", "consensus", "bridge", "mpspdz"]:
                self._set_stage(stage, "idle")

            # Scénario prédéfini pour la démo.
            steps = [
                (["build/node/data_provider", "1", "10", "--computation-nodes", str(n)], "provider", "provider1"),
                (["build/node/data_provider", "2", "3", "--computation-nodes", str(n)], "provider", "provider2"),
                (["build/consensus/consensus", "2", "--clean-inputs"], "consensus", "consensus"),
            ]
            for args, kind, stage in steps:
                rc, _ = self.run_logged(args, kind=kind, stage=stage)
                if rc != 0:
                    self.set_status(f"full scenario failed during {kind}")
                    return

            self._set_stage("provider3", "idle")
            self._set_stage("bridge", "running")
            rc, out = self.run_logged(
                ["build/spdz_bridge/spdz_bridge", "programs/sum.mpc", "--computation-nodes", str(n)],
                kind="bridge",
                stage="bridge",
            )
            if "Compiling" in out or "MP-SPDZ result:" in out or "semi2k-party.x" in out:
                self._set_stage("mpspdz", "success" if rc == 0 else "error")
            self.set_status("full scenario completed" if rc == 0 else "full scenario failed")
        self.run_in_thread(task)


def main() -> None:
    # Point d’entrée de l’application.
    app = VisualDemoGUI()
    app.mainloop()


if __name__ == "__main__":
    main()