# Protocole d’admission et core set (implémentation actuelle)

Ce document décrit **uniquement** le protocole tel qu’il est implémenté dans ce dépôt. Il ne reprend pas d’anciens schémas génériques ni des pistes non codées.

Pour le découpage des composants, voir aussi [`architecture.md`](architecture.md). Pour l’interface et les backends de `type_proof`, voir [`type_proof_layer_interface.md`](type_proof_layer_interface.md).

---

## Objectif : admission au `core_set`

Le **core set** est l’ensemble des identifiants de **fournisseurs de données** (*providers*) jugés **admissibles** pour enchaîner avec le calcul MPC (semi2k). L’admission vérifie, pour chaque candidat :

- l’authenticité / intégrité de la soumission provider (preuve keyed BLAKE2b) ;
- la cohérence du manifeste public avec la preuve provider ;
- la validité de l’artefact `type_proof` **au niveau du consensus** (pas de famille `type_ack`) ;
- en mode ACK (obligatoire dans l’implémentation actuelle) : la présence d’ACKs **signés** couvrant **tous** les `party_index` requis pour ce tour.

Le consensus écrit la décision dans `core_set.txt` (et des JSON d’audit sous `artifacts/`). Le pont MP-SPDZ ne refait **pas** ces vérifications : il suppose le core set déjà décidé.

---

## Enchaînement réel des composants

Flux implémenté :

```text
data_provider → share_verifier → consensus → (optionnel) spdz_bridge → (optionnel) MP-SPDZ (semi2k)
```

1. **`data_provider`** (`node/`) : produit les fichiers d’entrée et les secrets de partage locaux.
2. **`share_verifier`** (`consensus/`, binaire dédié) : pour un couple `(provider_id, party_index)`, charge **uniquement** la share locale et le manifeste ; vérifie la cohérence ; émet un **ACK signé** (clé CN du `party_index`).
3. **`consensus`** : service **centralisé** et **fichier-local** ; c’est la seule étape qui applique la politique d’admission complète (preuves provider, manifeste, `type_proof`, ACKs, session, anti-replay, fenêtre de temps optionnelle).
4. **`spdz_bridge`** : **exécution uniquement** — lit `core_set.txt`, prépare `Player-Data` pour **semi2k**, compile le `.mpc` choisi, lance `semi2k-party.x`. Aucun rôle d’admission, pas de re-vérification sémantique des preuves.

---

## Rôles détaillés

### `data_provider`

- Écrit `inputs/provider_<id>.txt` : identifiant, fil masqué (`masked_value=…`), `nonce`, preuve BLAKE2b keyed (secret partagé avec le consensus, ex. `MPC_PROVIDER_SECRET`).
- Écrit `inputs/provider_<id>_manifest.json` : engagements publics et métadonnées de partage (dont `share_manifest_id`, `mask_commitment_leaves_digest`, `num_parties`, entrées par `party_index`).
- Écrit `inputs/provider_<id>_type_proof.json` : preuve / evidence de type (backend selon `type_proof_system` — voir `consensus` + `type_proof.cpp`).
- Génère `provider_secrets/provider_<id>_share_<p>.secret` pour chaque indice de partie `p` prévu.

### `share_verifier`

- **Ne vérifie que** la share locale `provider_<id>_share_<party_index>.secret` et sa cohérence avec le manifeste pour ce `party_index` (digests, engagements).
- **Ne décide pas** l’admission au core set.
- Produit un fichier ACK JSON dans le répertoire `--acks-out-dir` (convention de nommage du type `ack_p<id>_party<party_index>.json`).

Il **n’existe pas** de mécanisme `type_ack` : la validité de type est vérifiée dans `consensus`, pas via un ACK séparé.

### `consensus`

- Entrées principales : répertoire `inputs/`, répertoire `--acks-dir`, paramètres de session (`--session-id`, `--round-id`, `--protocol-version`, `--schema-id`), taille de couverture `--num-parties` (ou `--k`), quorum `min_inputs`, clés CN `--cn-keys-dir`, `--timeout-seconds` optionnel, `--artifacts-dir`.
- Sorties : `core_set.txt` ; `artifacts/core_set.json` ; `artifacts/justification.json` (acceptés / rejetés avec raisons côté ACK quand applicable).

**Comportement important :** sans `--acks-dir`, le binaire **refuse de s’exécuter** — le mode « admission sans ACK » n’est pas disponible dans l’état actuel du code.

### `spdz_bridge`

- Lit `core_set.txt` et les fichiers provider / secrets déjà présents.
- Prépare les entrées semi2k et invoque `compile.py` puis `semi2k-party.x`.
- **Backend MPC uniquement semi2k** dans ce dépôt.

---

## Artefacts produits

| Artefact | Rôle |
|----------|------|
| `inputs/provider_<id>.txt` | Preuve d’authenticité sur le fil masqué + nonce. |
| `inputs/provider_<id>_manifest.json` | Manifeste public de partage (liaison ACK / type_proof). |
| `inputs/provider_<id>_type_proof.json` | Evidence de type (vérifiée dans `consensus`). |
| `provider_secrets/provider_<id>_share_<p>.secret` | Share additive locale pour la partie `p`. |
| Fichiers dans `--acks-dir` | ACKs signés par `share_verifier` (une entrée par `(provider_id, party_index)` attendu). |
| `core_set.txt` | Liste des IDs provider admis (un par ligne). |
| `artifacts/core_set.json` | Snapshot JSON de la décision (session, round, `k_required`, timeouts, providers). |
| `artifacts/justification.json` | Détail acceptation / rejet (raisons, fichiers ACK cités). |

Les schémas JSON de référence sont sous `schemas/` (`ack.schema.json`, `core_set.schema.json`, `justification.schema.json`).

---

## Vérifications effectuées dans `consensus`

### Sur chaque fichier `provider_<id>.txt` candidat

- Format strict (quatre lignes, `id=`, `masked_value=` ou `value=`, `nonce=`, `proof=`).
- Preuve BLAKE2b keyed : `proof` doit correspondre au calcul attendu sur `(id, valeur fil, nonce)` avec le secret provider/consensus.
- **Fil entier canonique** : la chaîne décimale signée doit respecter les règles de normalisation (pas d’espace, pas de `+`, zéros de tête interdits sauf `"0"`, etc.).

### Manifeste et liaison provider → manifeste

- Lecture minimale du manifeste ; `num_parties` ≥ seuil de couverture ACK requis.
- **`share_manifest_id`** : recalcul depuis les champs publics du fichier provider (`provider_id`, `nonce`, `masked_value`, `num_parties`) et comparaison au manifeste (défense contre altération du manifeste).

### `type_proof`

- Chargement de `provider_<id>_type_proof.json` et vérification via la couche `type_proof` (liaison `provider_id`, `nonce`, digest du fil masqué, `schema_id` attendu par le tour, cohérence avec `share_manifest_id` et `mask_commitment_leaves_digest` du manifeste).
- Passage au backend enregistré (`stub-typeproof-v1`, `semantic-schema-v1`, `proof-real-v1`, etc.) — voir le code et [`type_proof_layer_interface.md`](type_proof_layer_interface.md) pour les garanties réelles de chaque backend.

**Il n’y a pas** de vérification de type distribuée via un second type d’ACK : tout passe par cette vérification **dans** `consensus`.

### Mode ACK (obligatoire)

Pour chaque fichier ACK dans `--acks-dir` (nom `ack_*.json`) :

- Cohérence **session** : `session_id`, `round_id`, `protocol_version`, `schema_id` avec les paramètres du consensus.
- Le `provider_id` doit être parmi les candidats ayant déjà passé les vérifications provider + type_proof.
- `party_index` dans `[0, k_required)`.
- **Signature** Ed25519 : message signé = concaténation déterministe des champs (voir `ack_signing_message` dans `consensus.cpp`) ; clé publique attendue `cn_<party_index>.pub.hex` dans `--cn-keys-dir`.
- **Anti-replay** : au plus un ACK valide par couple `(provider_id, party_index)`.
- **Timeout optionnel** : si `--timeout-seconds` > 0, le `timestamp_unix_ms` de l’ACK doit tomber dans la fenêtre `[now - timeout, now + petite marge]`.
- **Liaison manifeste** : `share_manifest_id`, `mask_commitment_leaves_digest`, digests de fichier de share et engagements par `party_index` doivent correspondre au manifeste dérivé du provider.

### Règle de couverture ACK

Pour qu’un provider soit **retenu** après la phase ACK, il doit exister un ACK valide pour **chaque** `party_index` de `0` à `k_required - 1` (couverture complète). Sinon le provider est exclu du core set (avec raison enregistrée dans `justification.json` si applicable).

---

## Règle d’admission effective (quorum)

1. On considère les providers qui passent **toutes** les étapes : fichier provider, manifeste lié, `type_proof` valide.
2. Parmi eux, on ne garde que ceux avec **couverture ACK complète** pour `k_required` indices.
3. On exige **au moins `min_inputs`** providers ainsi retenus. Sinon le consensus **échoue** (pas de `core_set.txt`, code de retour non nul) ; des artefacts JSON peuvent quand même être écrits pour tracer l’échec.

L’ordre des IDs dans `core_set.txt` suit l’itération sur les fichiers `inputs/` (puis tri / déduplication défensive dans le code).

---

## Ce que le protocole implémente / n’implémente pas

**Implémenté :**

- Admission par preuves cryptographiques côté fichier (BLAKE2b keyed, signatures ACK).
- Vérification centralisée du `type_proof` sans `type_ack`.
- Politique ACK : couverture complète des parties, anti-replay, paramètres de session, timeout optionnel sur horodatage.
- Pont d’exécution semi2k découplé de l’admission.

**Non implémenté (hors scope du code actuel) :**

- Consensus réparti ou tolérant aux fautes byzantines entre plusieurs nœuds.
- Transport réseau authentifié entre providers, verifiers et consensus (tout est **local filesystem**).
- Autres backends MPC que **semi2k** dans ce dépôt.
- Ré-vérification des preuves dans `spdz_bridge` (volontairement absente).
- Mécanisme `type_ack` : **inexistant** dans le flux.
