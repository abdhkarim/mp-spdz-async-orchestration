#!/usr/bin/env python3
"""
Fournisseur de Données - Interface Graphique Tkinter pour Démonstration d'Orchestration Asynchrone MP-SPDZ

Cette interface graphique permet le contrôle manuel du workflow de démonstration :
- Exécuter des fournisseurs avec ID et valeurs personnalisés
- Simuler des fournisseurs malveillants avec le drapeau --malformed
- Déclencher le filtrage de consensus
- Exécuter le pont SPDZ avec calcul MPC

Fonctionnalités :
  - Affichage en temps réel de la sortie dans une zone de texte déroulante
  - Boutons interactifs pour chaque phase (fournisseurs, consensus, pont)
  - Validation des entrées et gestion d'erreurs
  - Séparation claire des préoccupations (logging, exécution, UI)

Utilisation :
  python3 demo_gui.py

L'interface graphique suppose :
  - Le répertoire build/ existe avec les binaires compilés
  - Contexte d'exécution depuis la racine du projet
  - build/ contient : node/data_provider, consensus/consensus, spdz_bridge/spdz_bridge
"""

import os
import subprocess
import tkinter as tk
from tkinter import scrolledtext, messagebox

# Référence au répertoire build (où CMake génère les exécutables)
BUILD_DIR = os.path.join(os.getcwd(), "build")


def run_cmd(cmd: str) -> str:
    """
    Exécute une commande shell et capture sa sortie.
    
    Args:
        cmd: Chaîne de commande à exécuter (ex. "./node/data_provider 1 42")
        
    Returns:
        Sortie de la commande sous forme de chaîne (stdout + stderr combinés)
        
    Raises:
        subprocess.CalledProcessError si la commande échoue
    """
    try:
        output = subprocess.check_output(cmd, shell=True, cwd=BUILD_DIR, stderr=subprocess.STDOUT)
        return output.decode(errors="ignore")
    except subprocess.CalledProcessError as e:
        return e.output.decode(errors="ignore")


def provider():
    """
    Gère le clic sur le bouton 'Exécuter fournisseur'.
    
    Valide les champs d'entrée, construit la commande avec le drapeau --malformed optionnel,
    exécute le binaire fournisseur, et affiche la sortie.
    """
    id_ = entry_id.get().strip()
    val = entry_val.get().strip()
    malformed = var_malformed.get()
    
    if not id_ or not val:
        messagebox.showwarning("Entrée", "Veuillez remplir les champs ID et valeur.")
        return
    
    cmd = f"./node/data_provider {id_} {val}"
    if malformed:
        cmd += " --malformed"
    
    append(f"$ {cmd}\n")
    append(run_cmd(cmd) + "\n")


def consensus():
    """
    Gère le clic sur le bouton 'Exécuter consensus'.
    
    Exécute le module consensus pour filtrer les fournisseurs malformés et décider de l'ensemble de base.
    Attend 10 secondes pour la collecte asynchrone avant validation.
    """
    append("$ ./consensus/consensus\n")
    append(run_cmd("./consensus/consensus") + "\n")


def bridge():
    """
    Gère le clic sur le bouton 'Exécuter pont'.
    
    Exécute l'orchestrateur du pont SPDZ.
    - Si MP-SPDZ est disponible : compile et exécute le MPC sécurisé
    - Si MP-SPDZ manquant : retourne la somme de repli des entrées validées
    """
    append("$ ./spdz_bridge/spdz_bridge\n")
    append(run_cmd("./spdz_bridge/spdz_bridge") + "\n")


def append(text: str) -> None:
    """
    Ajoute du texte à la zone de sortie et fait défiler automatiquement jusqu'à la fin.
    
    Args:
        text: Texte à ajouter au widget de texte déroulant
    """
    txt.insert(tk.END, text)
    txt.see(tk.END)


# Créer la fenêtre principale
root = tk.Tk()
root.title("Démo MP-SPDZ")
root.geometry("900x600")

# Cadre pour les contrôles d'entrée
frm = tk.Frame(root, padx=10, pady=10)
frm.pack(fill=tk.BOTH, expand=True)

# Entrée ID du fournisseur
lbl_id = tk.Label(frm, text="ID Fournisseur :")
lbl_id.grid(row=0, column=0, sticky=tk.W)
entry_id = tk.Entry(frm, width=5)
entry_id.grid(row=0, column=1, sticky=tk.W)

# Entrée valeur du fournisseur
lbl_val = tk.Label(frm, text="Valeur :")
lbl_val.grid(row=0, column=2, sticky=tk.W, padx=(20, 0))
entry_val = tk.Entry(frm, width=10)
entry_val.grid(row=0, column=3, sticky=tk.W)

# Case à cocher malformé
var_malformed = tk.IntVar()
chk = tk.Checkbutton(frm, text="malformé", variable=var_malformed)
chk.grid(row=0, column=4, padx=5)

# Bouton exécuter fournisseur
btn_provider = tk.Button(frm, text="Exécuter fournisseur", command=provider)
btn_provider.grid(row=0, column=5, padx=5)

# Boutons Consensus et Pont
btn_consensus = tk.Button(frm, text="Exécuter consensus", command=consensus)
btn_consensus.grid(row=1, column=0, columnspan=2, pady=5)

btn_bridge = tk.Button(frm, text="Exécuter pont", command=bridge)
btn_bridge.grid(row=1, column=2, columnspan=2, pady=5)

# Zone de texte de sortie (déroulante)
txt = scrolledtext.ScrolledText(frm, width=100, height=25, wrap=tk.WORD)
txt.grid(row=2, column=0, columnspan=6, pady=10, sticky=tk.NSEW)

# Configurer la grille pour étendre
frm.rowconfigure(2, weight=1)
frm.columnconfigure(5, weight=1)

root.mainloop()
