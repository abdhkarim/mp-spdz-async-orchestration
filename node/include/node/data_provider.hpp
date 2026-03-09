#pragma once

#include <string>

// Validate provider ID: alphanumeric, no path traversal
bool validate_id(const std::string& id);

// Validate value: numeric or numeric-like
bool validate_value(const std::string& value);

// Convert binary data to hexadecimal string
std::string to_hex(const unsigned char* data, size_t len);

// Compute cryptographic proof using libsodium
std::string compute_proof(const std::string& id,
                          const std::string& value,
                          const std::string& nonce,
                          const std::string& secret);
