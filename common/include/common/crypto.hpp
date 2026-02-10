#pragma once

#include <string>
#include <vector>

/**
 * @file crypto.hpp
 * @brief Petites fonctions crypto (hash + signature) via libsodium.
 *
 * Objectif : fournir une API SIMPLE à utiliser pour le reste du projet.
 * On ne veut pas que node/ ou consensus/ manipulent directement libsodium partout.
 */

namespace crypto {

// Bytes utilitaires
using Bytes = std::vector<unsigned char>;

// Convertit bytes <-> hex/base64 (on implémentera ça dans bytes.hpp/.cpp)
std::string to_hex(const Bytes& data);
Bytes from_hex(const std::string& hex);

std::string to_base64(const Bytes& data);
Bytes from_base64(const std::string& b64);

// Hash BLAKE2b (32 bytes par défaut)
Bytes hash32(const Bytes& data);

// Signature Ed25519
// - sign() retourne la signature (64 bytes) sous forme Bytes
Bytes sign_ed25519(const Bytes& message, const Bytes& secret_key);

// - verify() vérifie la signature avec la clé publique
bool verify_ed25519(const Bytes& message, const Bytes& signature, const Bytes& public_key);

} // namespace crypto