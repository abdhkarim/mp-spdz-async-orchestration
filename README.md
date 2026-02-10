# MPC Asynchrone avec Core Set via Consensus Externe

Ce dépôt contient un prototype de **calcul multipartite sécurisé (MPC) asynchrone**
basé sur **MP-SPDZ**, tolérant aux pannes de participants.

L’objectif est de permettre l’exécution d’un calcul MPC même lorsque certains
participants sont absents, déconnectés ou crashent, grâce à une **phase
d’accord préalable** sur l’ensemble des participants actifs (appelé *core set*).

Le projet adopte une **architecture hybride** :
- une phase **asynchrone externe** pour décider qui participe,
- suivie d’une exécution **MPC synchrone classique** avec MP-SPDZ.

---

## Idée principale

Dans les implémentations MPC classiques, tous les participants doivent être
présents et synchronisés avant le début du calcul.  
Dans un environnement réel (edge computing, federated learning, systèmes
distribués), cette hypothèse est souvent irréaliste.

Notre contribution consiste à :
1. collecter des preuves cryptographiques de réception des données,
2. utiliser un organe de consensus externe (simulé) pour décider du *core set*,
3. lancer MP-SPDZ uniquement avec les participants valides.

---

## Structure du projet

common/        → types, messages, crypto partagés
node/          → nœud MPC (phase asynchrone + lancement MP-SPDZ)
consensus/     → service central de décision du core set
spdz_bridge/   → génération et lancement de la configuration MP-SPDZ
programs/      → programmes MP-SPDZ (.mpc)
configs/       → configurations de démo
scripts/       → scripts utilitaires (setup, démo, crashs)
docs/          → documentation du protocole et de l’architecture
tests/         → tests unitaires
third_party/   → dépendances externes (MP-SPDZ en submodule)

---

## Flux global

1. Les nœuds MPC échangent leurs données (phase asynchrone).
2. Chaque réception valide produit une preuve cryptographique.
3. Le service de consensus collecte et vérifie les preuves.
4. Le *core set* est décidé et diffusé.
5. MP-SPDZ est lancé uniquement avec ce core set.

---

## Objectif pédagogique

Ce projet vise à démontrer :
- la compréhension des limites du MPC synchrone,
- la gestion de l’asynchronisme et des pannes,
- l’intégration d’un framework MPC existant dans une architecture distribuée réaliste