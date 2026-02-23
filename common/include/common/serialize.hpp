#pragma once

#include "messages.hpp"
#include <string>

/**
 * @file serialize.hpp
 * @brief Conversion des messages C++ <-> JSON (string).
 *
 * Pourquoi ?
 * - Le réseau (HTTP/Zenoh) transporte généralement du texte/bytes.
 * - JSON est simple à debugger (lisible à l'oeil).
 */

// Encode en JSON string
std::string to_json(const ShareMsg& msg);
std::string to_json(const AckMsg& msg);
std::string to_json(const CoreSetMsg& msg);

// Decode depuis JSON string (lève std::runtime_error si invalide)
ShareMsg from_json_share(const std::string& s);
AckMsg from_json_ack(const std::string& s);
CoreSetMsg from_json_coreset(const std::string& s);