# MPC asynchrone avec core set via consensus externe

Prototype académique de calcul multipartite sécurisé (MPC) branché sur **MP-SPDZ** sans modifier les sources de MP-SPDZ.

Enchaînement :

1. Les fournisseurs envoient des valeurs (fichiers + preuve BLAKE2b).
2. Le **consensus** décide le **core set** (entrées valides).
3. Le **bridge** prépare `Player-Data` pour **semi2k**, compile le `.mpc` et lance **`semi2k-party.x`** sur ce core set.

## Objectifs du prototype

- Simuler des absences (provider non soumis).
- Valider intégrité et authenticité des contributions côté consensus (preuve BLAKE2b + option ACK Ed25519).
- Démo MPC : somme, moyenne, etc. sur le core set.
- Montrer l’intégration MP-SPDZ (fichiers `Player-Data`, `compile.py`, exécution en ligne semi2k).

## Architecture

![Architecture hybride MPC avec core set](./others/architecture.png)

### Répertoire de travail (important)

Les binaires (`data_provider`, `consensus`, `spdz_bridge`) résolvent **`inputs/`**, **`core_set.txt`**, **`third_party/MP-SPDZ/`**, **`logs/`**, **`provider_secrets/`** par rapport au **répertoire courant** (`fs::current_path()` / `getcwd`).  
Il faut donc **toujours les lancer depuis la racine du dépôt**, pas depuis `build/`.

### Zone 1 — Data providers (asynchrone)

| Élément | Détail |
|--------|--------|
| Code | `node/src/data_provider.cpp` |
| Binaire | `./build/node/data_provider <id> <valeur> [--computation-nodes N]` |
| Défaut | `--computation-nodes` vaut **3** si omis |
| Sortie | `inputs/provider_<id>.txt` (id, valeur masquée, nonce, preuve BLAKE2b) |
| Secrets | Parts additives de \(s_i\) dans `provider_secrets/` (un fichier `.secret` par partie MPC) |

Variable optionnelle : `MPC_PROVIDER_SECRET` (sinon secret de démo partagé avec le consensus).

### Zone 2 — Consensus

| Élément | Détail |
|--------|--------|
| Code | `consensus/src/consensus.cpp`, `consensus/src/ack_crypto_tool.cpp` (signatures ACK) |
| Binaire | `./build/consensus/consensus [min_inputs] [--acks-dir … --k … …]` |
| Sortie | `core_set.txt` (un identifiant de provider par ligne) |

Sans `--acks-dir` : validation des preuves sur `inputs/` et besoin d’**au moins `min_inputs`** entrées valides. Avec `--acks-dir` : filtrage additionnel par ACK (seuil `--k`, fenêtre `--timeout-seconds`, etc.).

### Zone 3 — Bridge MP-SPDZ (semi2k uniquement)

| Élément | Détail |
|--------|--------|
| Code | `spdz_bridge/src/spdz_bridge.cpp`, `spdz_bridge/src/semi2k_prep.cpp` |
| Binaire | `./build/spdz_bridge/spdz_bridge [--computation-nodes N] [chemin/vers/programme.mpc]` |
| Défaut programme | `programs/sum.mpc` (chemin relatif à la racine du dépôt) |
| Backend | **Uniquement semi2k** (\(\mathbb{Z}/2^{64}\mathbb{Z}\)) — pas d’option `--backend` |

Le bridge :

- lit `core_set.txt` et les `inputs/provider_*.txt` ;
- écrit sous `third_party/MP-SPDZ/Player-Data/` : `Public-Masked-Values` et `Input-P*-0` ;
- exécute `python3 compile.py -R 64 …` dans `third_party/MP-SPDZ` en pointant vers le fichier `.mpc` du dépôt ;
- lance **`third_party/MP-SPDZ/semi2k-party.x`** pour chaque partie (ports `-pn`, hôte `localhost` pour \(p>0\)).

Résultat attendu dans `logs/player_0.log` : lignes `SUM=…` ou `RESULT=…` ; le bridge affiche alors `MP-SPDZ result: …`.

Si `semi2k-party.x` est absent ou si la compilation / MPC échoue, la sortie peut **ne pas** contenir `MP-SPDZ result:` (voir dépannage).

### Module `common/`

Bibliothèque statique partagée : types réseau, sérialisation, utilitaires crypto (`common/src/*.cpp`, headers sous `common/include/common/`). Liée par `consensus`, `node` et le bridge.

### Programmes MPC (`programs/`)

| Fichier | Rôle |
|---------|------|
| `sum.mpc` | Somme des entrées → `SUM=…` |
| `avg.mpc` | Moyenne → `RESULT=…` |
| `triple_sum.mpc` | Somme triple → `RESULT=…` |
| `parity_sum.mpc` | Parité de la somme → `RESULT=…` |

Le masquage et les parts ne passent pas par un `issue_secrets.mpc` : ils sont gérés en C++ (`data_provider`, `semi2k_prep`).

## Arborescence (principale)

```text
mp-spdz-async-orchestration/
├── CMakeLists.txt
├── LICENSE
├── demo_gui.py              # interface Tkinter optionnelle (lancer depuis la racine du dépôt)
├── common/                  # bibliothèque partagée
├── node/                    # data_provider
├── consensus/               # consensus + ack_crypto_tool
├── spdz_bridge/             # bridge semi2k
├── programs/                # .mpc (sum, avg, triple_sum, parity_sum)
├── scripts/                 # orchestration, validation complète
├── schemas/                 # schémas JSON (ACK, core set, justification)
├── docs/                    # architecture, protocole, notes
├── others/                  # assets (ex. architecture.png)
├── third_party/MP-SPDZ/     # sous-module Git — compiler semi2k-party.x
├── .gitmodules
├── build/                   # sortie CMake (généré ; souvent ignoré par git)
├── inputs/                  # généré à l’exécution
├── logs/                    # logs des parties semi2k
├── provider_secrets/        # parts par provider (généré)
├── core_set.txt             # sortie consensus (généré)
└── artifacts/               # JSON (mode ACK / orchestrateur)
```

## Prérequis

- **CMake** ≥ 3.20, compilateur **C++20**
- **pkg-config** et **libsodium**
- En-têtes **Boost.Multiprecision** (`boost/multiprecision/cpp_int.hpp`) — ex. paquet `libboost-dev` (Debian/Ubuntu)
- **Python 3** (pour `third_party/MP-SPDZ/compile.py`)
- **MP-SPDZ** dans `third_party/MP-SPDZ` : initialiser le sous-module puis compiler **`semi2k-party.x`**

Initialisation du sous-module :

```bash
git submodule update --init --recursive
```

La chaîne **Player-Online.x / Fake-Offline.x** du tutoriel amont MP-SPDZ **n’est pas** utilisée pour le flux semi2k de ce dépôt (préparation dans `semi2k_prep` + binaire en ligne).

## Compiler le projet C++

À la **racine** du dépôt :

```bash
cmake -S . -B build
cmake --build build -j4
```

Cibles : `data_provider`, `consensus`, `ack_crypto_tool`, `spdz_bridge`.

## Compiler MP-SPDZ (obligatoire pour le bridge)

```bash
cd third_party/MP-SPDZ
# Dépendances : voir README MP-SPDZ (make, g++, libsodium, etc.)
make -j4 semi2k-party.x
cd ../..
test -f third_party/MP-SPDZ/semi2k-party.x && echo OK
```

Sans ce fichier, le message `semi2k-party.x not found` apparaît et il n’y a pas de ligne `MP-SPDZ result:`.

## Windows + WSL

Préférer **WSL** pour bash, CMake et MP-SPDZ.

`scripts/run_bridge_wsl.sh` configure le build puis exécute **`spdz_bridge` avec la racine du dépôt comme répertoire courant** (indispensable pour trouver `third_party/` et `programs/`).

Exemple (adapter le chemin) :

```powershell
wsl -e bash -lc "cd /chemin/vers/mp-spdz-async-orchestration && ./scripts/run_bridge_wsl.sh --computation-nodes 2"
```

Pas d’option `--backend` : le bridge ne prend que `--computation-nodes` et un chemin optionnel vers un `.mpc`.

### Interface graphique optionnelle

```bash
python3 demo_gui.py
```

À lancer **depuis la racine du dépôt** après compilation. Les commandes utilisent `./build/...` avec `cwd` = racine du projet.

## Démo rapide (2 providers, 3ᵉ absent)

Le consensus doit avoir **`min_inputs` = 2** si seuls les providers 1 et 2 sont lancés. Utiliser **`--computation-nodes 2`** pour aligner le partage de secrets et le MPC sur deux parties.

```bash
export MPC_PROVIDER_SECRET="mpc-demo-secret"   # optionnel

rm -rf inputs logs artifacts core_set.txt provider_secrets
mkdir -p inputs logs artifacts provider_secrets

./build/node/data_provider 1 7 --computation-nodes 2
./build/node/data_provider 2 15 --computation-nodes 2

./build/consensus/consensus 2

./build/spdz_bridge/spdz_bridge --computation-nodes 2
```

Ne pas appeler `consensus 3` avec seulement deux fichiers provider valides : échec (« pas assez d’entrées ») et pas de `core_set.txt`.

## Orchestration asynchrone (`async_orchestrator.py`)

`scripts/run_async_round_wsl.sh` compile les binaires nécessaires puis appelle l’orchestrateur. **Il n’y a pas d’option `--backend`** (le champ `backend: semi2k` dans `artifacts/run_meta.json` est informatif).

```bash
./scripts/run_async_round_wsl.sh \
  --clean \
  --session-id demo-session \
  --round-id 0 \
  --providers 1:10,2:20,3:30,4:40,5:50 \
  --k-acks 2 \
  --ack-nodes 3 \
  --ack-timeout-seconds 2 \
  --computation-nodes 3
```

Artefacts : `artifacts/run_meta.json`, `artifacts/core_set.json`, `artifacts/justification.json`, `artifacts/acks/`, `artifacts/cn_keys/`. Schémas : `schemas/*.schema.json`.

## Tests d’intégration complets

```bash
bash scripts/full_system_validation_wsl.sh
```

Résumé : `backend_test_summary.txt`. Journaux détaillés : `.tmp_full_test_runs/`.  
Les étapes MPC nécessitent **`semi2k-party.x`** et Python opérationnel. Un échec du type « no result line in output » arrive souvent si le binaire MP-SPDZ manque **ou** si le bridge retourne 0 sans ligne `MP-SPDZ result:` (comportement actuel du script).

## Dépannage

- **`semi2k-party.x not found`** : compiler MP-SPDZ dans `third_party/MP-SPDZ` ; vérifier que le sous-module est bien présent (`git submodule update --init --recursive`).
- **Chemins incorrects (`third_party` introuvable)** : lancer les binaires depuis la **racine du dépôt**, pas depuis `build/`.
- **Pas de `MP-SPDZ result:`** : lire `logs/player_0.log` et la sortie du bridge (`MP-SPDZ compilation failed`, `Could not parse result`, etc.).
- **`compile.py` échoue** : dépendances Python / environnement MP-SPDZ.
- **Consensus sans `core_set.txt`** : moins de `min_inputs` entrées valides que demandé.
- **Erreur de compilation C++** `std::vector` / en-tête manquant sur une autre machine : inclure explicitement `<vector>` dans les fichiers qui utilisent `std::vector` (certains compilateurs n’incluent pas transitivement cet en-tête).
- Détails **MP-SPDZ amont** (modulus, `INSECURE`, etc.) : [README MP-SPDZ](https://github.com/data61/MP-SPDZ).

## Limites

- Consensus centralisé par fichiers (pas de consensus byzantin réaliste).
- Prototype pédagogique, pas destiné à la production.
