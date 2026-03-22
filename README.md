# MPC asynchrone avec core set via consensus externe

Prototype académique de calcul multipartite sécurisé (MPC) branché sur **MP-SPDZ** sans modifier les sources de MP-SPDZ.

Enchaînement :

1. Les fournisseurs envoient des valeurs (fichiers + preuve BLAKE2b).
2. Le **consensus** décide le **core set** (entrées valides).
3. Le **bridge** prépare `Player-Data` pour **semi2k**, compile le `.mpc` et lance **`semi2k-party.x`** sur ce core set.

## Objectifs du prototype

- Simuler des absences (provider non soumis).
- Valider intégrité et authenticité des contributions côté consensus (preuve + option ACK).
- Démo MPC : somme, moyenne, etc. sur le core set.
- Montrer l’intégration MP-SPDZ (fichiers `Player-Data`, compilation `compile.py`, exécution en ligne).

## Architecture

![Architecture hybride MPC avec core set](./others/architecture.png)

### Zone 1 — Data providers (asynchrone)

| Élément | Détail |
|--------|--------|
| Code | `node/src/data_provider.cpp` |
| Binaire | `./build/node/data_provider <id> <valeur> [--computation-nodes N]` |
| Défaut | `--computation-nodes` vaut **3** si omis |
| Sortie | `inputs/provider_<id>.txt` (id, valeur masquée, nonce, preuve BLAKE2b) |
| Secret | Partage additif de \(s_i\) dans `provider_secrets/` (un fichier de part par partie MPC) |

Variable optionnelle : `MPC_PROVIDER_SECRET` (sinon secret de démo partagé).

### Zone 2 — Consensus

| Élément | Détail |
|--------|--------|
| Code | `consensus/src/consensus.cpp` |
| Binaire | `./build/consensus/consensus [min_inputs] [options ACK…]` |
| Sortie | `core_set.txt` (un identifiant de provider par ligne) |

Sans `--acks-dir`, le consensus valide les preuves sur les fichiers `inputs/` et exige **au moins `min_inputs` entrées valides** pour écrire `core_set.txt`. Avec `--acks-dir`, mode ACK (seuil `--k`, etc.).

### Zone 3 — Bridge MP-SPDZ (semi2k uniquement)

| Élément | Détail |
|--------|--------|
| Code | `spdz_bridge/src/spdz_bridge.cpp`, `spdz_bridge/src/semi2k_prep.cpp` |
| Binaire | `./build/spdz_bridge/spdz_bridge [--computation-nodes N] [chemin/vers/programme.mpc]` |
| Défaut programme | `programs/sum.mpc` |
| Backend | **Uniquement semi2k** (anneau \(\mathbb{Z}/2^{64}\mathbb{Z}\)) — pas de sélection `--backend` |

Le bridge :

- lit `core_set.txt` et les `inputs/provider_*.txt` ;
- écrit `Player-Data/Public-Masked-Values` et les entrées par partie (`Input-P*-0`) via la préparation semi2k externe ;
- appelle `python3 compile.py -R 64 …` dans `third_party/MP-SPDZ` ;
- lance **`third_party/MP-SPDZ/semi2k-party.x`** pour chaque partie.

Résultat attendu dans `logs/player_0.log` : lignes `SUM=…` ou `RESULT=…` ; le bridge affiche alors `MP-SPDZ result: …`.

En cas d’échec de compilation ou d’exécution, ou si `semi2k-party.x` est absent, un message de secours peut s’afficher **sans** ligne `MP-SPDZ result:` (voir dépannage).

### Programmes MPC (`programs/`)

| Fichier | Rôle |
|---------|------|
| `sum.mpc` | Somme des entrées → `SUM=…` |
| `avg.mpc` | Moyenne → `RESULT=…` |
| `triple_sum.mpc` | Somme triple → `RESULT=…` |
| `parity_sum.mpc` | Parité de la somme → `RESULT=…` |

Il n’y a **pas** de `issue_secrets.mpc` dans ce dépôt : le masquage et les parts sont gérés côté C++ (`data_provider`, `semi2k_prep`).

## Arborescence (principale)

```text
mp-spdz-async-orchestration/
├── CMakeLists.txt
├── common/                 # code partagé
├── node/                   # data_provider
├── consensus/              # consensus (+ ack_crypto_tool)
├── spdz_bridge/            # bridge semi2k
├── programs/               # .mpc (sum, avg, triple_sum, parity_sum)
├── scripts/                # orchestration, tests intégration
├── schemas/                # schémas JSON (ACK, core set, justification)
├── docs/                   # notes d’architecture / protocole
├── others/                 # assets (ex. architecture.png)
├── third_party/MP-SPDZ/    # clone MP-SPDZ (à compiler : semi2k-party.x)
├── build/                  # sortie CMake (généré)
├── inputs/                 # généré à l’exécution
├── logs/                   # logs parties MP-SPDZ
├── provider_secrets/       # parts par provider (généré)
├── core_set.txt            # sortie consensus (généré)
└── artifacts/              # JSON d’orchestration (mode ACK)
```

## Prérequis

- **CMake** ≥ 3.20, compilateur **C++20**
- **pkg-config** et **libsodium** (preuves BLAKE2b)
- **Python 3** (invocation de `compile.py` dans MP-SPDZ)
- Copie de **MP-SPDZ** dans `third_party/MP-SPDZ`, puis compilation du binaire **`semi2k-party.x`** (indispensable pour une vraie exécution MPC via le bridge)

La chaîne **Player-Online.x / Fake-Offline.x** décrite dans la doc amont MP-SPDZ **n’est pas** utilisée par ce prototype pour le flux semi2k courant (préparation externalisée dans le bridge).

## Compiler le projet C++

À la racine du dépôt :

```bash
cmake -S . -B build
cmake --build build -j4
```

Cibles utiles : `data_provider`, `consensus`, `ack_crypto_tool`, `spdz_bridge`.

## Compiler MP-SPDZ (obligatoire pour le bridge)

À partir de la racine du dépôt, après avoir cloné [MP-SPDZ](https://github.com/data61/MP-SPDZ) dans `third_party/MP-SPDZ` :

```bash
cd third_party/MP-SPDZ
# Suivre README MP-SPDZ pour dépendances (g++, make, libsodium, etc.)
make -j4 semi2k-party.x
```

Vérification :

```bash
test -f third_party/MP-SPDZ/semi2k-party.x && echo OK
```

Sans ce fichier, le bridge affiche `semi2k-party.x not found` et ne produit pas `MP-SPDZ result:`.

## Windows + WSL

Développer et exécuter sous **WSL** pour la cohérence avec CMake, bash et MP-SPDZ.

Exemple depuis PowerShell (adapter le chemin) :

```powershell
wsl -e bash -lc "cd /chemin/vers/mp-spdz-async-orchestration && ./scripts/run_bridge_wsl.sh"
```

`run_bridge_wsl.sh` configure `build/`, compile, puis lance `spdz_bridge` avec les **mêmes arguments** que le binaire (pas de `--backend`). Exemple :

```powershell
wsl -e bash -lc "cd /chemin/vers/mp-spdz-async-orchestration && ./scripts/run_bridge_wsl.sh --computation-nodes 2"
```

## Démo rapide (2 providers, « crash » du 3ᵉ)

Le consensus doit demander **2** entrées si seuls les providers 1 et 2 sont lancés. Aligner le nombre de parties MPC avec `--computation-nodes 2`.

```bash
export MPC_PROVIDER_SECRET="mpc-demo-secret"   # optionnel

rm -rf inputs logs artifacts core_set.txt provider_secrets
mkdir -p inputs logs artifacts provider_secrets

./build/node/data_provider 1 7 --computation-nodes 2
./build/node/data_provider 2 15 --computation-nodes 2
# provider 3 volontairement absent

./build/consensus/consensus 2

./build/spdz_bridge/spdz_bridge --computation-nodes 2
```

Attendu : `core_set.txt` contient `1` et `2` ; sortie du bridge avec `MP-SPDZ result:` cohérente avec la somme des valeurs du core set (si `semi2k-party.x` est compilé).

**Ne pas** utiliser `./build/consensus/consensus 3` avec seulement deux fichiers provider : le consensus échoue (pas assez d’entrées) et ne crée pas `core_set.txt`.

## Orchestration asynchrone (script Python)

Le script `scripts/run_async_round_wsl.sh` construit les binaires puis appelle `async_orchestrator.py`. Il n’existe **pas** d’option `--backend` : le méta-fichier indique `semi2k` à titre informatif.

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

Artefacts typiques : `artifacts/run_meta.json`, `artifacts/core_set.json`, `artifacts/justification.json`, `artifacts/acks/`, `artifacts/cn_keys/`.

Schémas : `schemas/*.schema.json`.

## Tests d’intégration complets

```bash
bash scripts/full_system_validation_wsl.sh
```

Rapport : `backend_test_summary.txt`.  
Les parties MPC du script exigent **`semi2k-party.x`** et Python fonctionnel ; sinon les tests semi2k peuvent échouer avec « no result line in output » alors que la cause réelle est souvent l’absence du binaire MP-SPDZ (voir logs dans `.tmp_full_test_runs/`).

## Dépannage

- **`semi2k-party.x not found`** : compiler MP-SPDZ (`make semi2k-party.x`) dans `third_party/MP-SPDZ`.
- **Pas de ligne `MP-SPDZ result:`** : vérifier `logs/player_0.log`, la sortie du bridge (`MP-SPDZ compilation failed`, `Could not parse result`, etc.) et la présence de `SUM=` / `RESULT=`.
- **Échec de `compile.py`** : dépendances Python MP-SPDZ, chemins, droits d’exécution.
- **Consensus ne crée pas `core_set.txt`** : le nombre d’entrées valides est inférieur au seuil `min_inputs` — ajuster `min_inputs` ou fournir plus de providers.
- Erreurs **MP-SPDZ amont** (modulus, `INSECURE`, etc.) : se reporter au [README MP-SPDZ](https://github.com/data61/MP-SPDZ) pour les tutoriels **Player-Online** / **Fake-Offline** si vous les utilisez en dehors de ce flux semi2k.

## Limites

- Consensus centralisé par fichiers (pas de consensus byzantin réaliste).
- Prototype pédagogique, pas destiné à la production.
