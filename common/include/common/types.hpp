#pragma once

#include <cstdint>
#include <string>

/**
 * @file types.hpp
 * @brief Types de base partagés par tous les modules.
 *
 * Pourquoi c'est important ?
 * - On veut que tout le projet parle le même langage : mêmes IDs, mêmes types.
 * - Ça évite des bugs débiles (ex: confondre party_id et round_id).
 */

// Identifiant d'un participant (ex: 0,1,2,3...).
using PartyId = std::int32_t;

// Identifiant de session (ex: "S1234"). Sert à éviter les replays et à isoler une exécution.
using SessionId = std::string;

// Numéro de round (si on fait plusieurs tentatives/collectes).
using RoundId = std::int32_t;