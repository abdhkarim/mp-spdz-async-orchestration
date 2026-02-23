# Protocole asynchrone et core set

Ce document décrit le protocole implémenté pour décider du core set
avant l’exécution du calcul MPC.

---

## Objectif du protocole

Déterminer un ensemble de participants fiables (*core set*) tels que :
- chacun est suffisamment présent et actif,
- les données nécessaires au calcul ont été correctement reçues,
- le calcul MPC peut être lancé sans blocage.

---

## Phase 1 : collecte asynchrone

Chaque participant :
1. envoie ses données aux autres participants,
2. reçoit des données des autres,
3. produit une preuve cryptographique pour chaque réception valide.

Cette phase est **asynchrone** :
- aucun ordre global,
- aucun temps limite strict,
- tolérance aux crashs.

---

## Preuves de réception

Une preuve atteste qu’un participant affirme avoir reçu correctement une donnée.

Une preuve contient :
- identifiant de session,
- identités de l’émetteur et du receveur,
- hash de la donnée reçue,
- signature cryptographique du receveur.

Propriétés assurées :
- authenticité,
- intégrité,
- non-répudiation.

---

## Phase 2 : décision du core set

Le service de consensus :
1. collecte les preuves,
2. vérifie leur validité,
3. applique une règle de décision.

Exemple de règle :
> Un participant est inclus dans le core set s’il dispose
> d’au moins *k* preuves valides provenant de participants distincts.

Cette règle est paramétrable.

---

## Phase 3 : diffusion

Une fois le core set décidé :
- il est diffusé à tous les nœuds,
- seuls les membres du core set poursuivent l’exécution.

---

## Phase 4 : calcul MPC

Les participants du core set lancent MP-SPDZ
avec une configuration cohérente.

Le calcul MPC est alors exécuté de manière classique et synchrone.