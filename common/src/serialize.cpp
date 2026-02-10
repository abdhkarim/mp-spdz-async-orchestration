#include "common/serialize.hpp"

/*
 * Implémentation à venir :
 * - sérialisation JSON des messages (ShareMsg, AckMsg, CoreSetMsg)
 * - désérialisation depuis JSON
 *
 * Pour l’instant : stub pour que le projet compile.
 */

std::string to_json(const ShareMsg&) {
    return "{}";
}

std::string to_json(const AckMsg&) {
    return "{}";
}

std::string to_json(const CoreSetMsg&) {
    return "{}";
}

ShareMsg from_json_share(const std::string&) {
    return {};
}

AckMsg from_json_ack(const std::string&) {
    return {};
}

CoreSetMsg from_json_coreset(const std::string&) {
    return {};
}