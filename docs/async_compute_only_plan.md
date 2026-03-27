# Statut et feuille de route : MPC asynchrone et compute externalisé

Ce document remplace l’ancien « plan spéculatif » : il reflète **l’état du dépôt** et les suites **plausibles** pour un prototype de recherche. L’objectif architectural reste : **MP-SPDZ (semi2k) comme moteur de calcul**, le reste (admission, preuves, orchestration fichier) **à l’extérieur** de MP-SPDZ.

---

## Déjà implémenté

- **Découplage** : le nombre de soumissions provider ne fixe pas directement la topologie MP-SPDZ ; le **core set** est décidé **avant** l’exécution MPC.
- **Chaîne d’admission réelle** :  
  `data_provider` → `share_verifier` (ACK signé par partie) → **`consensus`** (seule autorité d’admission sur fichiers) → optionnellement `spdz_bridge` → `semi2k-party.x`.
- **Preuve provider** : BLAKE2b keyed + contrôle de **fil décimal canonique** pour le masqué.
- **Manifeste** : liaison `share_manifest_id` recalculée depuis les champs publics du provider ; binding utilisé pour ACKs et `type_proof`.
- **`type_proof`** : vérifié **directement dans `consensus`** (backends dans `consensus/src/type_proof.cpp`) — **pas** de `type_ack`.
- **ACKs** : le binaire `consensus` **exige** `--acks-dir` ; vérification des signatures (clés CN), cohérence avec manifeste et provider, **anti-replay** `(provider_id, party_index)`, **fenêtre temporelle** optionnelle (`--timeout-seconds`), **couverture complète** des `party_index` `0 … k_required-1`.
- **Artefacts** : `core_set.txt`, `artifacts/core_set.json`, `artifacts/justification.json` ; ACKs dans le répertoire fourni à `--acks-dir`.
- **`spdz_bridge`** : **exécution uniquement** — préparation `Player-Data`, `compile.py`, lancement **semi2k** ; pas d’admission.
- **Interface graphique** : `project_gui.py` enchaîne les mêmes binaires que la ligne de commande.
- **Validation** : scripts du dossier `scripts/` (ex. validation bout-en-bout sous WSL).

---

## Partiellement implémenté / prototype

- **Orchestration** : il n’y a pas une **machine d’états unique** officielle qui piloterait tout le cycle de vie ; l’enchaînement est assuré par **scripts**, la **GUI**, ou des commandes manuelles. Les états type « COLLECTING → DECIDED → RUNNING » sont **conceptuels**, pas un module unique dans le dépôt.
- **Schémas JSON** : fichiers sous `schemas/` ; l’alignement strict schéma ↔ tous les champs runtime peut encore être resserré selon les besoins d’audit.
- **`type_proof`** : plusieurs backends coexistent (`stub-typeproof-v1`, `semantic-schema-v1`, `proof-real-v1`). Seuls certains offrent une garantie cryptographique forte ; le dépôt reste **expérimental** sur la couche sémantique (voir README et `type_proof_layer_interface.md`).

---

## Limites actuelles du prototype

- **`consensus` centralisé** : un seul processus lit `inputs/` et `--acks-dir` ; pas de protocole BFT multi-nœuds.
- **Pas de réseau** : hypothèse typique **machine unique** ou dossiers partagés ; pas de canal authentifié end-to-end entre entités distantes dans le code.
- **Secrets de démo** : valeur par défaut partagée pour la preuve provider si `MPC_PROVIDER_SECRET` est absent (documenté dans le README).
- **MPC** : **semi2k uniquement** dans ce pont ; pas d’alternative `--backend` dans `spdz_bridge`.
- **Disponibilité / reprise** : peu ou pas de politique automatisée de relance après échec MPC ou crash (hors ce que permettent scripts et opérateur).

---

## Prochaines étapes réalistes

1. **Orchestrateur explicite** (script ou petit service) avec états nommés et journalisation unique pour démos et CI — sans imposer encore un déploiement distribué.
2. **Supervision du calcul** : timeouts, capture d’erreurs `semi2k-party.x`, stratégie de retry documentée.
3. **Durcissement des secrets** : rotation, séparation des secrets provider vs consensus, moins de valeurs par défaut en environnement « sérieux ».
4. **Évolution de `type_proof`** : choix de backend et de registre de schémas (`schemas/type_registry.json`) alignés sur un scénario cible (ZK / SNARK complet hors scope immédiat pour ce dépôt).
5. **Scénarios d’échec** : démos automatisées (provider manquant, ACK incomplet, timeout) déjà partiellement couvertes par les scripts ; extension possible pour la reprise après échec MPC.

Ces pistes **complètent** le prototype actuel ; elles ne remettent pas en cause le fait que l’**admission par ACK** et le **pont execution-only** sont **déjà en place** dans le code.
