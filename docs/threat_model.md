# Threat model (prototype `mp-spdz-async-orchestration`)

Ce document décrit le modèle de menaces **aligné sur l’implémentation actuelle** du dépôt. Il vise la clarté pour un prototype de recherche / démonstration, **pas** une certification de produit.

---

## 1. Périmètre (scope)

- **Inclus** : génération des artefacts provider (`inputs/`, `provider_secrets/`), production d’ACKs par `share_verifier`, **admission centralisée** par `consensus`, décision `core_set.txt` et JSON d’audit, enchaînement optionnel avec `spdz_bridge` et **MP-SPDZ semi2k** en local ou environnement contrôlé (ex. WSL).
- **Exclu** : sécurité interne complète de la bibliothèque **MP-SPDZ** amont (on s’appuie sur ses hypothèses et sa doc) ; sécurité du système d’exploitation, du compilateur, et de Python utilisé par `compile.py`.

---

## 2. Actifs à protéger

| Actif | Description |
|-------|-------------|
| **Intégrité des entrées MPC** | Que seules des valeurs ayant passé la politique d’admission soient injectées dans le calcul semi2k. |
| **Authenticité des soumissions provider** | Preuve keyed BLAKE2b liant identité déclarée, nonce et fil masqué. |
| **Cohérence du partage secret** | Les shares utilisées par les parties MPC correspondent aux engagements / manifeste vérifiés via la chaîne ACK. |
| **Traçabilité de la décision** | `core_set.txt`, `artifacts/core_set.json`, `justification.json`, fichiers ACK pour expliquer acceptation / rejet. |
| **Clés de signature ACK** | Matériel `cn_*` utilisé pour signer / vérifier les ACKs (intégrité des fichiers clé sur disque). |
| **Secret partagé provider–consensus** | `MPC_PROVIDER_SECRET` (ou défaut de démo) pour la preuve BLAKE2b. |

La **confidentialité** des clairs individuels vis-à-vis du consensus n’est **pas** assurée au sens MPC complet : le consensus ne reconstruit pas les secrets en clair dans le scénario nominal, mais le modèle de confiance (qui voit quoi sur disque) reste celui d’un **prototype local**.

---

## 3. Acteurs et frontières de confiance

| Acteur | Rôle | Confiance |
|--------|------|-----------|
| **Provider** | Produit fichiers `inputs/` et `provider_secrets/`. | **Non fiable** pour l’énoncé des valeurs ; doit être contenu par preuves et politique d’admission. |
| **Share verifier (par `party_index`)** | Vérifie **sa** share et émet un ACK signé. | Modélisé comme honnête pour la démo ; en production, chaque partie contrôlerait sa clé CN. |
| **Consensus (`consensus`)** | Point unique qui applique la politique d’admission. | **Totalement de confiance** dans ce prototype : il lit tous les fichiers et impose la règle finale. |
| **spdz_bridge** | Prépare et lance semi2k. | Doit être exécuté dans un environnement de confiance ; il ne refait pas l’admission. |
| **Parties semi2k** | Calcul MPC synchrone classique. | Honnêtes-mais-curieux ou malveillantes selon le scénario MPC standard ; **hors** du périmètre d’une preuve BFT du script shell. |

**Frontière** : tout ce qui est « au-delà » du processus `consensus` qui écrit `core_set.txt` est considéré comme **non protégé** par le protocole d’admission (ex. modification manuelle de `core_set.txt` avant le bridge).

---

## 4. Modèle d’attaquant

- Peut **lire et modifier** des fichiers dans `inputs/`, `artifacts/`, répertoires ACK, **s’il a accès au même système de fichiers** que le prototype (même utilisateur, partage SMB mal configuré, etc.).
- Peut tenter de **rejouer** des ACKs, de **forger** des fichiers provider, de **mélanger** des sessions (`session_id` / `round_id`), ou de **voler** des clés CN / le secret `MPC_PROVIDER_SECRET`.
- **Ne** modèle **pas** une coalition de parties MPC capable de casser semi2k ou libsodium : on reste au niveau **logique admission + fichiers**.

Ce n’est **pas** un modèle « Byzantine distribué » sur plusieurs nœuds indépendants : le consensus n’est **pas** répliqué.

---

## 5. Hypothèses de confiance

1. Le processus `consensus` s’exécute sur une machine / compte où l’attaquant ne peut pas **modifier silencieusement** les binaires ou les fichiers d’entrée **pendant** une exécution légitime — ou bien l’attaquant est limité à des scénarios déjà couverts (rejets, pas de `core_set.txt`).
2. Les clés publiques CN sous `--cn-keys-dir` correspondent aux clés secrètes utilisées par les `share_verifier` honnêtes.
3. Le secret `MPC_PROVIDER_SECRET` (si utilisé) est partagé **seulement** entre provider et consensus dans le scénario voulu.
4. MP-SPDZ semi2k est utilisé comme boîte noire correcte pour la phase en ligne, sous ses propres hypothèses (nombre de parties, réseau synchrone entre processus lancés par le bridge, etc.).

---

## 6. Objectifs de sécurité (ce que le code cherche à garantir)

- **S1 — Intégrité admission** : un ID n’apparaît dans `core_set.txt` que si les vérifications implémentées (preuve provider, manifeste lié, `type_proof`, ACKs avec couverture complète, paramètres de session) ont réussi.
- **S2 — Non-répudiation partielle des ACKs** : signature Ed25519 sur un message déterministe ; anti-replay par `(provider_id, party_index)`.
- **S3 — Liaison forte** : ACKs et `type_proof` liés aux mêmes digests de manifeste / fil masqué que le consensus recalcule ou contrôle.
- **S4 — Fenêtre temporelle optionnelle** : si activée, exclusion des ACKs trop anciens par rapport à l’horloge du processus consensus.

---

## 7. Non-objectifs / hors scope

- **Consensus distribué tolérant aux fautes** entre plusieurs instances `consensus`.
- **Protection contre un administrateur malveillant** sur la machine qui exécute consensus et bridge.
- **Anonymat** ou **privacy** des providers entre eux au-delà de ce que semi2k assure pendant le calcul.
- **Garantie formelle** sur les backends `type_proof` autres que ce que chaque backend implémente (ex. `semantic-schema-v1` n’est pas un ZK complet — voir README et `type_proof_layer_interface.md`).
- **Sécurité réseau** entre machines distantes (pas de TLS / authentification de transport dans ce dépôt).

---

## 8. Mitigations actuelles (dans le code)

| Menace | Mitigation implémentée |
|--------|-------------------------|
| Fichier provider altéré | Preuve BLAKE2b keyed ; fil décimal canonique. |
| Manifeste incohérent avec le provider | Recalcul de `share_manifest_id` depuis le fichier provider. |
| Type / schéma arbitraire | `type_proof` vérifié dans `consensus` avec `schema_id` attendu pour le tour. |
| ACK forgé ou issu d’une autre session | Vérification signature + champs session / protocole / schéma + liaison manifeste + nonce provider. |
| Double soumission ACK | Détection de replay `(provider_id, party_index)`. |
| ACK trop ancien | Option `--timeout-seconds` sur `timestamp_unix_ms`. |
| Admission non justifiée | JSON `justification.json` avec raisons de rejet. |

---

## 9. Risques résiduels

| Risque | Commentaire |
|--------|-------------|
| **Confiance centralisée dans `consensus`** | Un compromis du processus ou du disque équivalent à « tout accepter ou tout falsifier » avant écriture des artefacts. |
| **Prototype single-machine** | Pas de séparation physique entre rôles ; un utilisateur peut simuler toutes les parties et toutes les clés. |
| **Horloge** | La fenêtre timeout dépend de l’horloge locale du consensus ; pas de synchronisation NTP modélisée. |
| **Secrets par défaut** | `mpc-demo-secret` si variable d’environnement absente — inacceptable en production. |
| **Semantique `type_proof`** | Selon le backend, la garantie peut être limitée à de la validation de registre ou des preuves d’engagement prototypes, pas une ZK complète. |
| **Bridge** | Si `core_set.txt` est modifié manuellement après le consensus, le bridge ne le détecte pas. |

---

## 10. Synthèse

Ce dépôt implémente une **chaîne d’admission explicite et vérifiable sur fichiers**, avec **consensus centralisé** et **calcul MPC semi2k externalisé**. Il ne constitue **pas** un système distribué byzantin complet. Toute utilisation au-delà de la recherche ou de la démo doit reprendre ce modèle de menaces et renforcer séparation des rôles, gestion des secrets, et déploiement réseau selon le contexte.
