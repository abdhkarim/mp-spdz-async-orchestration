#!/usr/bin/env python3
"""
Data Provider - Tkinter GUI for MP-SPDZ Async Orchestration Demo

This graphical interface allows manual control over the demonstration workflow:
- Run providers with custom ID and values
- Simulate malicious providers with --malformed flag
- Trigger consensus filtering
- Execute SPDZ bridge with MPC computation

Features:
  - Real-time output display in scrollable text area
  - Interactive buttons for each phase (providers, consensus, bridge)
  - Input validation and error handling
  - Clean separation of concerns (logging, execution, UI)

Usage:
  python3 demo_gui.py

The GUI assumes:
  - build/ directory exists with compiled binaries
  - Execution context from project root
  - build/ contains: node/data_provider, consensus/consensus, spdz_bridge/spdz_bridge
"""

import os
import subprocess
import tkinter as tk
from tkinter import scrolledtext, messagebox

# Reference to build directory (where CMake outputs executables)
BUILD_DIR = os.path.join(os.getcwd(), "build")


def run_cmd(cmd: str) -> str:
    """
    Execute a shell command and capture its output.
    
    Args:
        cmd: Command string to execute (e.g., "./node/data_provider 1 42")
        
    Returns:
        Command output as string (stdout + stderr combined)
        
    Raises:
        subprocess.CalledProcessError if command fails
    """
    try:
        output = subprocess.check_output(cmd, shell=True, cwd=BUILD_DIR, stderr=subprocess.STDOUT)
        return output.decode(errors="ignore")
    except subprocess.CalledProcessError as e:
        return e.output.decode(errors="ignore")


def provider():
    """
    Handle 'Run provider' button click.
    
    Validates input fields, constructs command with optional --malformed flag,
    executes provider binary, and displays output.
    """
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
    """
    Handle 'Run consensus' button click.
    
    Executes consensus module to filter malformed providers and decide core set.
    Waits 10 seconds for async collection before validation.
    """
    append("$ ./consensus/consensus\n")
    append(run_cmd("./consensus/consensus") + "\n")


def bridge():
    """
    Handle 'Run bridge' button click.
    
    Executes SPDZ bridge orchestrator.
    - If MP-SPDZ is available: compiles and runs secure MPC
    - If MP-SPDZ missing: returns fallback sum of validated inputs
    """
    append("$ ./spdz_bridge/spdz_bridge\n")
    append(run_cmd("./spdz_bridge/spdz_bridge") + "\n")


def append(text: str) -> None:
    """
    Append text to output area and auto-scroll to end.
    
    Args:
        text: Text to append to the scrolled text widget
    """
    txt.insert(tk.END, text)
    txt.see(tk.END)


# Create main window
root = tk.Tk()
root.title("MP-SPDZ Demo")
root.geometry("900x600")

# Frame for input controls
frm = tk.Frame(root, padx=10, pady=10)
frm.pack(fill=tk.BOTH, expand=True)

# Provider ID input
lbl_id = tk.Label(frm, text="Provider ID:")
lbl_id.grid(row=0, column=0, sticky=tk.W)
entry_id = tk.Entry(frm, width=5)
entry_id.grid(row=0, column=1, sticky=tk.W)

# Provider value input
lbl_val = tk.Label(frm, text="Value:")
lbl_val.grid(row=0, column=2, sticky=tk.W, padx=(20, 0))
entry_val = tk.Entry(frm, width=10)
entry_val.grid(row=0, column=3, sticky=tk.W)

# Malformed checkbox
var_malformed = tk.IntVar()
chk = tk.Checkbutton(frm, text="malformed", variable=var_malformed)
chk.grid(row=0, column=4, padx=5)

# Run provider button
btn_provider = tk.Button(frm, text="Run provider", command=provider)
btn_provider.grid(row=0, column=5, padx=5)

# Consensus and Bridge buttons
btn_consensus = tk.Button(frm, text="Run consensus", command=consensus)
btn_consensus.grid(row=1, column=0, columnspan=2, pady=5)

btn_bridge = tk.Button(frm, text="Run bridge", command=bridge)
btn_bridge.grid(row=1, column=2, columnspan=2, pady=5)

# Output text area (scrollable)
txt = scrolledtext.ScrolledText(frm, width=100, height=25, wrap=tk.WORD)
txt.grid(row=2, column=0, columnspan=6, pady=10, sticky=tk.NSEW)

# Configure grid to expand
frm.rowconfigure(2, weight=1)
frm.columnconfigure(5, weight=1)

root.mainloop()
