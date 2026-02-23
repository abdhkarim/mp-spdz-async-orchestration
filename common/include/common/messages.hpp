#pragma once

#include "types.hpp"
#include <string>
#include <vector>

/**
 * @file messages.hpp
 * @brief Définition des messages échangés entre nodes et consensus.
 *
 * Principe : on définit ici le "contrat" du protocole.
 * Tous les modules (node/consensus/spdz_bridge) utilisent les mêmes structures.
 *
 * IMPORTANT :
 * - Ces structs sont des "conteneurs" (pas de logique).
 * - La sérialisation (JSON) sera dans serialize.hpp/.cpp.
 */

enum class MsgType {
    SHARE,     // Un participant envoie une part (share) à un autre
    ACK,       // Preuve signée "j'ai reçu ce share"
    CORE_SET,  // Décision finale : qui participe réellement au MPC
    ERROR_MSG  // Message d'erreur (optionnel, utile pour debug)
};

struct ShareMsg {
    MsgType type = MsgType::SHARE;

    SessionId session;  // identifie l'exécution
    RoundId round = 0;

    PartyId from = -1;  // émetteur du share
    PartyId to = -1;    // destinataire du share

    // Contenu du share. Pour un proto : base64 (car JSON-friendly).
    std::string payload_b64;
};

struct AckMsg {
    MsgType type = MsgType::ACK;

    SessionId session;
    RoundId round = 0;

    PartyId from = -1; // "from" = l'émetteur original du share (celui qui a envoyé le share)
    PartyId to = -1;   // "to"   = celui qui a reçu le share et qui signe l'ACK

    // hash( payload || session || round || from || to )
    // -> permet de lier la signature à EXACTEMENT ce share pour cette session.
    std::string share_hash_hex;

    // Signature du receveur (PartyId = to), encodée en base64.
    std::string signature_b64;

    // (Optionnel) identifiant de clé publique si vous gérez plusieurs clés.
    std::string pubkey_id;
};

struct CoreSetMsg {
    MsgType type = MsgType::CORE_SET;

    SessionId session;
    RoundId round = 0;

    // Liste des participants acceptés pour lancer MP-SPDZ.
    std::vector<PartyId> core_set;

    // Explication utile pour logs/debug (ex: "k=3 valid acks per sender")
    std::string reason;

    // Optionnel : signature du consensus (si vous voulez signer la décision)
    std::string signature_b64;
};