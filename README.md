# MPC Asynchrone avec Core Set via Consensus Externe

Ce dépôt contient un prototype académique de calcul multipartite sécurisé (MPC)
intégrant MP-SPDZ sans modifier son code source.

Le but est de montrer un enchaînement simple et fonctionnel :
1. des fournisseurs envoient des valeurs,
2. un consensus centralisé décide le core set (participants valides),
3. MP-SPDZ exécute la somme uniquement sur ce core set.

## Objectifs du prototype

- Simuler des crashes (participant absent).
- Simuler des comportements malveillants simples (fichier mal formé).
- Produire une démo minimale de calcul MPC (addition).
- Illustrer les difficultés d’intégration MP-SPDZ (offline/online, fichiers, lancement).

## Architecture
![Architecture hybride MPC avec core set](./others/architecture.png)
### `node/` (data providers)
- Exécutable `data_provider`.
- Écrit un fichier `inputs/provider_<id>.txt`.
- Format valide attendu :
  - `id=<id>`
  - `value=<valeur>`

### `consensus/`
- Exécutable `consensus`.
- Attend un timeout fixe.
- Lit `inputs/`, valide les fichiers.
- Exclut les fichiers mal formés.
- Écrit `core_set.txt` (un id par ligne).

### `spdz_bridge/`
- Exécutable `spdz_bridge`.
- Lit `core_set.txt`.
- Génère les entrées MP-SPDZ dans `third_party/MP-SPDZ/Player-Data/Input-P*-0`.
- Compile `programs/sum.mpc`.
- Tente de lancer MP-SPDZ online.
- En cas d’échec runtime, affiche une somme de fallback calculée localement.

### `programs/sum.mpc`
- Lit `N` entrées secrètes (`N = taille du core set`).
- Calcule la somme.
- Révèle `SUM=<résultat>`.

## Correspondance avec l'architecture (Zone 1 / 2 / 3)

Cette section fait le lien direct avec le schéma "Architecture Hybride: Server-Side MPC avec Consensus (Core Set)".

### Zone 1 : Data Providers (asynchrone / instable)

- Composant : `node/src/data_provider.cpp`
- Exécutable : `./build/node/data_provider <id> <value> [--malformed]`
- Rôle :
  - envoi d'une contribution individuelle,
  - crash simulé si le provider n'est pas lancé,
  - comportement malveillant simulé via `--malformed`.
- Trace concrète : fichiers `inputs/provider_<id>.txt`.

### Zone 2 : Arbitre / Consensus (frontière de synchronisation)

- Composant : `consensus/src/consensus.cpp`
- Exécutable : `./build/consensus/consensus [timeout_seconds]`
- Rôle :
  - attend la fenêtre de collecte (timeout),
  - valide le format des preuves/entrées reçues,
  - exclut les entrées absentes ou invalides,
  - décide le `core set`.
- Trace concrète : fichier `core_set.txt` (un identifiant valide par ligne).

### Zone 3 : Computation Nodes (synchrone / stable)

- Composants :
  - `spdz_bridge/src/spdz_bridge.cpp`
  - `programs/sum.mpc`
  - runtime MP-SPDZ dans `third_party/MP-SPDZ`
- Rôle :
  - conversion du core set en parties MP-SPDZ actives,
  - préparation des fichiers `Player-Data/Input-P*-0`,
  - compilation/exécution du programme MPC,
  - calcul synchrone sur les seules parties retenues.
- Trace concrète :
  - logs d'exécution,
  - sortie `SUM=<valeur>`.

## Arborescence

```text
common/        code partagé C++
node/          data providers
consensus/     décision du core set
spdz_bridge/   interface vers MP-SPDZ
programs/      programmes MPC (.mpc)
docs/          documentation
third_party/   MP-SPDZ (submodule, non modifié)
```

## Prérequis

- macOS/Linux avec CMake et compilateur C++20.
- MP-SPDZ présent dans `third_party/MP-SPDZ`.
- Pour l’exécution complète MP-SPDZ (offline+online) :
  - `Player-Online.x`
  - `Fake-Offline.x`
- `grep` suffit pour les vérifications (pas besoin de `rg`).

## Compilation du prototype C++

Depuis la racine du projet :

```bash
cmake -S . -B build
cmake --build build -j4
```

## Démo rapide (crash + malveillant + somme)

Depuis la racine du projet :

```bash
rm -rf inputs core_set.txt logs

# providers valides
./build/node/data_provider 1 7
./build/node/data_provider 2 15

# ne pas lancer provider 3 -> crash simulé

# provider malveillant (format invalide)
./build/node/data_provider 4 999 --malformed

# consensus (attente 3 secondes)
./build/consensus/consensus 3

# bridge (préparation inputs MP-SPDZ + compilation programme)
./build/spdz_bridge/spdz_bridge
```

Résultat attendu côté consensus :
- `core_set.txt` contient `1` et `2`.
- le provider mal formé est ignoré.

## Exécution MP-SPDZ complète (offline + online)

Depuis `third_party/MP-SPDZ` :

```bash
# une seule fois si nécessaire
echo 'MY_CFLAGS += -DINSECURE' >> CONFIG.mine
make clean
make -j4 Fake-Offline.x Player-Online.x

# génération des certificats + preprocessing
rm -rf Player-Data/2-p-128
./Scripts/setup-online.sh 2 128 0 10000

# vérification clé
ls -l Player-Data/2-p-128/Params-Data

# exécution online du programme compilé (sum-2)
./Scripts/run-online.sh sum-2 -N 2
```

Résultat attendu :
- affichage terminal : `SUM=22`

Vérification dans les logs MP-SPDZ :

```bash
grep -R "SUM=" logs
```

## Dépannage

- Erreur `no modulus in Player-Data//2-p-128/Params-Data` :
  - `Fake-Offline.x` n’a pas généré correctement le preprocessing.
  - Refaire `make ... Fake-Offline.x` puis `./Scripts/setup-online.sh ...`.

- Erreur `You are trying to use insecure benchmarking functionality` :
  - ajouter `MY_CFLAGS += -DINSECURE` dans `CONFIG.mine`,
  - puis `make clean` et recompiler.

- Erreur `zsh: no matches found` sur un glob :
  - utiliser `ls logs` puis `grep -R "SUM=" logs`.

## Limites

- Consensus centralisé simulé (pas de vrai consensus byzantin).
- Communication simplifiée par fichiers.
- Objectif pédagogique/prototype, pas production.
