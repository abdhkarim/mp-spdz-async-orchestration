#!/usr/bin/env python3
"""Small Tkinter GUI wrapper that runs the three binaries produced by the CMake
build.  The purpose is purely demonstrative: the user can press buttons and
see the output of each stage without dropping to a shell.

Usage:
    python demo_gui.py

The script assumes that the build directory has been created and contains the
executables in subfolders (e.g. build\node\data_provider, build\consensus\consensus,
build\spdz_bridge\spdz_bridge).  On Windows the GUI will use backslashes in
commands; adjust the command string if you run on another platform.
"""

import os
import subprocess
import tkinter as tk
from tkinter import scrolledtext, messagebox

# where CMake writes its outputs
BUILD_DIR = os.path.join(os.getcwd(), "build")


def run_cmd(cmd: str) -> str:
    """Run command in the build directory and capture stdout+stderr."""
    try:
        output = subprocess.check_output(cmd, shell=True, cwd=BUILD_DIR, stderr=subprocess.STDOUT)
        return output.decode(errors="ignore")
    except subprocess.CalledProcessError as e:
        return e.output.decode(errors="ignore")


def provider():
    id_ = entry_id.get().strip()
    val = entry_val.get().strip()
    malformed = var_malformed.get()
    if not id_ or not val:
        messagebox.showwarning("Input", "Please fill both ID and value fields.")
        return
    cmd = f"./node/data_provider {id_} {val}"
    if malformed:
        cmd += " --malformed"
    append(f"$ {cmd}\n")
    append(run_cmd(cmd) + "\n")


def consensus():
    append("$ ./consensus/consensus\n")
    append(run_cmd("./consensus/consensus") + "\n")


def bridge():
    append("$ ./spdz_bridge/spdz_bridge\n")
    append(run_cmd("./spdz_bridge/spdz_bridge") + "\n")


def append(text: str) -> None:
    txt.insert(tk.END, text)
    txt.see(tk.END)


root = tk.Tk()
root.title("MP-SPDZ Demo")

frm = tk.Frame(root, padx=10, pady=10)
frm.pack(fill=tk.BOTH, expand=True)

# provider controls
lbl_id = tk.Label(frm, text="Provider ID:")
lbl_id.grid(row=0, column=0, sticky=tk.W)
entry_id = tk.Entry(frm, width=5)
entry_id.grid(row=0, column=1, sticky=tk.W)

lbl_val = tk.Label(frm, text="Value:")
lbl_val.grid(row=0, column=2, sticky=tk.W)
entry_val = tk.Entry(frm, width=10)
entry_val.grid(row=0, column=3, sticky=tk.W)

var_malformed = tk.IntVar()
chk = tk.Checkbutton(frm, text="malformed", variable=var_malformed)
chk.grid(row=0, column=4, padx=5)

btn_provider = tk.Button(frm, text="Run provider", command=provider)
btn_provider.grid(row=0, column=5, padx=5)

# consensus/bridge buttons
btn_consensus = tk.Button(frm, text="Run consensus", command=consensus)
btn_consensus.grid(row=1, column=0, columnspan=2, pady=5)

btn_bridge = tk.Button(frm, text="Run bridge", command=bridge)
btn_bridge.grid(row=1, column=2, columnspan=2, pady=5)

# output area
txt = scrolledtext.ScrolledText(frm, width=80, height=20, wrap=tk.WORD)
txt.grid(row=2, column=0, columnspan=6, pady=10)

root.mainloop()
