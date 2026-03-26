# Architecture du système

Cette section décrit l’architecture globale du prototype et le rôle de chaque
composant.

---

## Vue d’ensemble

Le système est composé de quatre blocs principaux :

1. **Provider (non fiable)**
2. **Share verifiers (un par `party_index`)**
3. **Consensus d’admission (autorité de core set)**
4. **Exécution MPC (bridge + MP-SPDZ)**

Le consensus d’admission est volontairement **externalisé** afin de ne pas modifier
l’implémentation interne de MP-SPDZ.

---

## 1. Provider et share verifiers (`node/`, `consensus/`)

Cette phase produit l'evidence nécessaire à l'admission.

Provider (`node/src/data_provider.cpp`) :
- produit `inputs/provider_<id>.txt` (masked wire + preuve d'auth BLAKE2b keyed),
- produit `inputs/provider_<id>_manifest.json` (engagements/digests publics pour vérification locale),
- produit `inputs/provider_<id>_type_proof.json` (evidence de type, vérifiée dans le consensus),
- génère `provider_secrets/provider_<id>_share_<p>.secret` pour chaque `party_index p` (le secret complet `s_i` n'est jamais écrit en clair).

Share verifier (`consensus/src/share_verifier.cpp`) :
- vérifie uniquement sa share locale `provider_<id>_share_<party_index>.secret`,
- vérifie la cohérence avec le manifest public pour cet `party_index`,
- produit un ACK signé pour l'admission.

---

## 2. Service de consensus (`consensus/`)

Le consensus est simulé par un service centralisé.

Responsabilités :
- vérification des preuves provider (preuve BLAKE2b keyed, wire canonique),
- vérification directe du `type_proof` via la factory de backends (`consensus/src/type_proof.cpp`),
- en mode ACK : vérification des signatures et des champs de liaison des ACKs produits par les share verifiers,
- imposition d’une couverture complète des `party_index` requis avant d’admettre un provider,
- écriture du `core_set.txt` (décision d'admission).

Ce composant ne réalise **pas** un consensus byzantin complet.
Il s’agit d’un modèle simplifié et réaliste pour un prototype.

---

## 3. MP-SPDZ (`third_party/MP-SPDZ`)

MP-SPDZ est utilisé comme **boîte noire** pour le calcul MPC.

Hypothèses conservées :
- communication synchrone,
- ensemble fixe de participants,
- absence de pannes pendant l’exécution.

Ces hypothèses sont satisfaites en lançant MP-SPDZ uniquement
après la décision du core set.

---

## Pont MP-SPDZ (`spdz_bridge/`)

Ce module assure la liaison entre la phase asynchrone et MP-SPDZ :

- lecture du `core_set.txt`,
- préparation des entrées MP-SPDZ à partir des evidence déjà admises (masked values + shares locales),
- génération des fichiers de configuration MP-SPDZ,
- compilation des programmes `.mpc`,
- lancement des processus MP-SPDZ avec les bons identifiants.

Le bridge est volontairement **execution-only** : il ne revalide pas `type_proof` ni la logique d'admission.

---

## Séparation des responsabilités

Cette architecture permet :
- une modularité claire,
- une implémentation réaliste,
- une compréhension isolée de chaque difficulté :
  - asynchronisme avant le calcul,
  - MPC classique pendant le calcul.