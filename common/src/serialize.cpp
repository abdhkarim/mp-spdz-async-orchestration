#include "common/serialize.hpp"

/*
 * Stub de sérialisation.
 *
 * Pourquoi garder ce stub :
 * - le prototype de démo courant n'échange pas encore ces structures en JSON,
 * - l'interface est déjà figée pour intégrer facilement nlohmann/json plus tard.
 *
 * Valeur de retour "{}" :
 * - permet de compiler et lier sans introduire de dépendance réseau/JSON
 *   dans cette étape.
 */

std::string to_json(const ShareMsg&) {
    // Placeholder minimal ; sera remplacé par une vraie sérialisation.
    return "{}";
}

std::string to_json(const AckMsg&) {
    // Placeholder minimal ; sera remplacé par une vraie sérialisation.
    return "{}";
}

std::string to_json(const CoreSetMsg&) {
    // Placeholder minimal ; sera remplacé par une vraie sérialisation.
    return "{}";
}

ShareMsg from_json_share(const std::string&) {
    // Retourne un message vide tant que le parsing JSON n'est pas implémenté.
    return {};
}

AckMsg from_json_ack(const std::string&) {
    // Retourne un message vide tant que le parsing JSON n'est pas implémenté.
    return {};
}

CoreSetMsg from_json_coreset(const std::string&) {
    // Retourne un message vide tant que le parsing JSON n'est pas implémenté.
    return {};
}
