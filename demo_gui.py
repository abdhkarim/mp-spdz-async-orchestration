#!/usr/bin/env python3
"""Interface graphique Tkinter pour la démo MP-SPDZ.

Fonctionnalités :
- appels subprocess plus sûrs
- exécution asynchrone pour éviter de bloquer l'interface
- mode natif + option WSL sous Windows
- vérification des entrées provider ID / value
- boutons : provider, consensus, bridge, scénario complet, reset
- petit résumé du résultat (core set + SUM)
"""

import os
import queue
import re
import shlex
import subprocess
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, scrolledtext


# Racine du projet = dossier courant
PROJECT_ROOT = Path.cwd()
# Dossier build où les exécutables / fichiers sont lancés
BUILD_DIR = PROJECT_ROOT / "build"

# regex pour vérifier les entrées
ID_REGEX = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
INT_REGEX = re.compile(r"^[+-]?\d{1,64}$")


def to_wsl_path(path: Path) -> str:
    """Convertit un chemin Windows en chemin WSL."""
    text = str(path)

    # si le chemin est du style C:\...
    if re.match(r"^[A-Za-z]:\\", text):
        drive = text[0].lower()
        rest = text[2:].replace("\\", "/")
        return f"/mnt/{drive}{rest}"

    return text.replace("\\", "/")


def validate_provider_inputs(provider_id: str, value: str) -> tuple[bool, str]:
    # vérifie si l'id du provider est valide
    if not ID_REGEX.fullmatch(provider_id):
        return False, "Provider ID invalide (1-32, alnum, _ ou -)."

    # vérifie si la valeur est bien un entier signé
    if not INT_REGEX.fullmatch(value):
        return False, "Value invalide (entier signé, max 64 caractères)."

    return True, ""


class App:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("MP-SPDZ Demo v2")

        # file pour envoyer des messages du thread vers l'interface
        self.msg_queue: queue.Queue[tuple[str, str]] = queue.Queue()

        # permet de savoir si une commande tourne déjà
        self.busy = False

        # option WSL activée par défaut si on est sur Windows
        self.var_use_wsl = tk.IntVar(value=1 if os.name == "nt" else 0)

        # variables affichées dans l'interface
        self.var_status = tk.StringVar(value="Idle")
        self.var_result = tk.StringVar(value="SUM: -")
        self.var_core_set = tk.StringVar(value="Core set: -")

        self._build_ui()
        # boucle qui regarde régulièrement la file de messages
        self.root.after(100, self._drain_queue)

    def _build_ui(self) -> None:
        # frame principal
        frm = tk.Frame(self.root, padx=10, pady=10)
        frm.pack(fill=tk.BOTH, expand=True)

        # case à cocher pour choisir WSL
        tk.Checkbutton(frm, text="Run commands in WSL", variable=self.var_use_wsl).grid(
            row=0, column=0, columnspan=2, sticky=tk.W
        )

        # champ provider id
        tk.Label(frm, text="Provider ID:").grid(row=1, column=0, sticky=tk.W)
        self.entry_id = tk.Entry(frm, width=10)
        self.entry_id.grid(row=1, column=1, sticky=tk.W)
        self.entry_id.insert(0, "1")

        # champ value
        tk.Label(frm, text="Value:").grid(row=1, column=2, sticky=tk.W)
        self.entry_val = tk.Entry(frm, width=12)
        self.entry_val.grid(row=1, column=3, sticky=tk.W)
        self.entry_val.insert(0, "10")

        # arguments supplémentaires pour consensus
        tk.Label(frm, text="Consensus args:").grid(row=2, column=0, sticky=tk.W)
        self.entry_consensus_args = tk.Entry(frm, width=18)
        self.entry_consensus_args.grid(row=2, column=1, sticky=tk.W)
        self.entry_consensus_args.insert(0, "3 --clean-inputs")

        # arguments supplémentaires pour bridge
        tk.Label(frm, text="Bridge args:").grid(row=2, column=2, sticky=tk.W)
        self.entry_bridge_args = tk.Entry(frm, width=22)
        self.entry_bridge_args.grid(row=2, column=3, columnspan=2, sticky=tk.W)
        self.entry_bridge_args.insert(0, "--backend player-online")

        # petite aide visuelle pour l'utilisateur
        tk.Label(
            frm,
            text="Ex: consensus='2' | bridge='--backend semi2k --computation-nodes 3'",
            fg="gray",
            anchor="w",
            justify=tk.LEFT,
        ).grid(row=6, column=0, columnspan=5, sticky=tk.W, padx=2)

        # boutons des différentes actions
        self.btn_provider = tk.Button(frm, text="Run provider", command=self.on_provider)
        self.btn_provider.grid(row=3, column=0, pady=6, sticky=tk.W)

        self.btn_consensus = tk.Button(frm, text="Run consensus", command=self.on_consensus)
        self.btn_consensus.grid(row=3, column=1, pady=6, sticky=tk.W)

        self.btn_bridge = tk.Button(frm, text="Run bridge", command=self.on_bridge)
        self.btn_bridge.grid(row=3, column=2, pady=6, sticky=tk.W)

        self.btn_scenario = tk.Button(frm, text="Run full scenario", command=self.on_full_scenario)
        self.btn_scenario.grid(row=3, column=3, pady=6, sticky=tk.W)

        self.btn_reset = tk.Button(frm, text="Reset workspace", command=self.on_reset)
        self.btn_reset.grid(row=3, column=4, pady=6, sticky=tk.W)

        # labels d'état et de résultat
        tk.Label(frm, textvariable=self.var_status, fg="blue").grid(row=4, column=0, columnspan=3, sticky=tk.W)
        tk.Label(frm, textvariable=self.var_core_set).grid(row=4, column=3, sticky=tk.W)
        tk.Label(frm, textvariable=self.var_result).grid(row=4, column=4, sticky=tk.W)

        # zone de texte scrollable pour afficher les sorties
        self.txt = scrolledtext.ScrolledText(frm, width=110, height=24, wrap=tk.WORD)
        self.txt.grid(row=5, column=0, columnspan=5, pady=8)

    def append(self, text: str) -> None:
        # ajoute du texte dans la console de l'interface
        self.txt.insert(tk.END, text)
        self.txt.see(tk.END)

    def set_busy(self, value: bool) -> None:
        # active / désactive les boutons pendant l'exécution
        self.busy = value
        state = tk.DISABLED if value else tk.NORMAL

        for btn in [self.btn_provider, self.btn_consensus, self.btn_bridge, self.btn_scenario, self.btn_reset]:
            btn.configure(state=state)

        self.var_status.set("Running..." if value else "Idle")

    def _native_run(self, args: list[str]) -> tuple[int, str]:
        # lance la commande normalement sur l'OS courant
        completed = subprocess.run(
            args,
            cwd=BUILD_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        return completed.returncode, completed.stdout

    def _wsl_run(self, args: list[str]) -> tuple[int, str]:
        # transforme le chemin build en chemin compréhensible par WSL
        build_wsl = to_wsl_path(BUILD_DIR)

        # on construit la commande bash de manière safe avec shlex.quote
        command = "cd " + shlex.quote(build_wsl) + " && " + " ".join(shlex.quote(a) for a in args)

        completed = subprocess.run(
            ["wsl", "-e", "bash", "-lc", command],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        return completed.returncode, completed.stdout

    def run_cmd(self, args: list[str], title: str) -> tuple[int, str]:
        # affiche la commande lancée dans la zone texte
        self.append(f"$ {' '.join(args)}\n")

        # choisit entre exécution WSL ou native
        if self.var_use_wsl.get():
            rc, out = self._wsl_run(args)
        else:
            rc, out = self._native_run(args)

        # affiche la sortie de la commande
        self.append(out + ("\n" if not out.endswith("\n") else ""))
        self.append(f"[{title}] exit={rc}\n\n")

        # met à jour le résumé si on trouve les infos dans la sortie
        self._update_summary_from_output(out)
        return rc, out

    def _update_summary_from_output(self, out: str) -> None:
        # récupère la somme si elle apparait dans la sortie
        m_sum = re.search(r"MP-SPDZ result: SUM=([-]?\d+)", out)
        if m_sum:
            self.var_result.set(f"SUM: {m_sum.group(1)}")

        # récupère le core set si présent
        m_core = re.search(r"Core set decided with \d+ provider\(s\):\s*(.*)", out)
        if m_core:
            self.var_core_set.set(f"Core set: {m_core.group(1).strip()}")

    def _drain_queue(self) -> None:
        # cette fonction lit les messages envoyés par le thread worker
        try:
            while True:
                kind, payload = self.msg_queue.get_nowait()

                if kind == "done":
                    self.set_busy(False)
                elif kind == "error":
                    self.set_busy(False)
                    messagebox.showerror("Execution error", payload)

        except queue.Empty:
            # normal si la file est vide
            pass
        finally:
            # on relance la vérification toutes les 100 ms
            self.root.after(100, self._drain_queue)

    def _run_async(self, fn) -> None:
        # évite de lancer plusieurs commandes en même temps
        if self.busy:
            messagebox.showinfo("Busy", "Une commande est déjà en cours.")
            return

        # vérifie que le dossier build existe
        if not BUILD_DIR.exists():
            messagebox.showerror("Build missing", f"Build directory not found: {BUILD_DIR}")
            return

        self.set_busy(True)

        def worker() -> None:
            # fonction exécutée dans un thread séparé
            try:
                fn()
                self.msg_queue.put(("done", ""))
            except Exception as exc:  # noqa: BLE001
                self.msg_queue.put(("error", str(exc)))

        # thread daemon pour ne pas bloquer la fermeture du programme
        threading.Thread(target=worker, daemon=True).start()

    def on_provider(self) -> None:
        # récupère les valeurs saisies
        provider_id = self.entry_id.get().strip()
        value = self.entry_val.get().strip()

        # validation avant exécution
        ok, err = validate_provider_inputs(provider_id, value)
        if not ok:
            messagebox.showwarning("Input", err)
            return

        def task() -> None:
            args = ["./node/data_provider", provider_id, value]
            self.run_cmd(args, "provider")

        self._run_async(task)

    def on_consensus(self) -> None:
        # récupère les args tapés dans le champ
        extra = self.entry_consensus_args.get().strip()
        extra_args = shlex.split(extra) if extra else []

        def task() -> None:
            self.run_cmd(["./consensus/consensus", *extra_args], "consensus")

        self._run_async(task)

    def on_bridge(self) -> None:
        extra = self.entry_bridge_args.get().strip()
        extra_args = shlex.split(extra) if extra else []

        def task() -> None:
            self.run_cmd(["./spdz_bridge/spdz_bridge", *extra_args], "bridge")

        self._run_async(task)

    def on_reset(self) -> None:
        # demande confirmation avant de supprimer
        if not messagebox.askyesno("Confirm", "Supprimer inputs/logs/core_set.txt ?"):
            return

        def task() -> None:
            if self.var_use_wsl.get():
                # reset via bash quand on utilise WSL
                self.run_cmd(["bash", "-lc", "rm -rf inputs logs core_set.txt && mkdir -p inputs logs"], "reset")
            else:
                # reset manuel en natif Windows
                inputs = BUILD_DIR / "inputs"
                logs = BUILD_DIR / "logs"
                core = BUILD_DIR / "core_set.txt"

                if inputs.exists():
                    subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", str(inputs)], check=False)

                if logs.exists():
                    subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", str(logs)], check=False)

                if core.exists():
                    core.unlink(missing_ok=True)

                inputs.mkdir(parents=True, exist_ok=True)
                logs.mkdir(parents=True, exist_ok=True)
                self.append("[reset] Workspace reset done\n\n")

        self._run_async(task)

    def on_full_scenario(self) -> None:
        def task() -> None:
            # remet les résultats à zéro avant le scénario
            self.var_result.set("SUM: -")
            self.var_core_set.set("Core set: -")

            # on simule plusieurs providers valides
            self.run_cmd(["./node/data_provider", "1", "10"], "provider")
            self.run_cmd(["./node/data_provider", "2", "3"], "provider")
            self.run_cmd(["./node/data_provider", "3", "1"], "provider")

            # on lance ensuite le consensus
            rc_consensus, _ = self.run_cmd(["./consensus/consensus"], "consensus")
            if rc_consensus != 0:
                self.append("[scenario] Stopped: consensus failed\n\n")
                return

            # si consensus ok, on lance le bridge
            rc_bridge, _ = self.run_cmd(["./spdz_bridge/spdz_bridge"], "bridge")
            if rc_bridge == 0:
                self.var_status.set("Scenario completed ")
            else:
                self.var_status.set("Scenario completed with errors ")

        self._run_async(task)


def main() -> None:
    # point d'entrée du programme
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
