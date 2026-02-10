# Architecture du système

Cette section décrit l’architecture globale du prototype et le rôle de chaque
composant.

---

## Vue d’ensemble

Le système est composé de trois blocs principaux :

1. **Nœuds MPC**
2. **Organe de consensus externe**
3. **Moteur MPC (MP-SPDZ)**

Le consensus est volontairement **externalisé** afin de ne pas modifier
l’implémentation interne de MP-SPDZ.

---

## 1. Nœuds MPC (`node/`)

Chaque nœud représente un participant potentiel au calcul MPC.

Responsabilités :
- participation à la phase asynchrone de collecte,
- réception des données des autres participants,
- génération de preuves de réception,
- communication avec le service de consensus,
- lancement ou participation à l’exécution MP-SPDZ si inclus dans le core set.

Les nœuds peuvent :
- être lents,
- se déconnecter,
- crasher.

Le système doit continuer à fonctionner malgré ces pannes.

---

## 2. Service de consensus (`consensus/`)

Le consensus est simulé par un service centralisé.

Responsabilités :
- collecte des preuves cryptographiques,
- vérification de leur authenticité et validité,
- application d’une règle de décision pour déterminer le core set,
- diffusion de la décision aux nœuds MPC.

Ce composant ne réalise **pas** un consensus byzantin complet.
Il s’agit d’un modèle simplifié et réaliste pour un prototype.

---

## 3. MP-SPDZ (`third_party/mp-spdz`)

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

- génération des fichiers de configuration MP-SPDZ,
- compilation des programmes `.mpc`,
- lancement des processus MP-SPDZ avec les bons identifiants.

---

## Séparation des responsabilités

Cette architecture permet :
- une modularité claire,
- une implémentation réaliste,
- une compréhension isolée de chaque difficulté :
  - asynchronisme avant le calcul,
  - MPC classique pendant le calcul.