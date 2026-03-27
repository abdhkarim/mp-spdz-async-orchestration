/**
 * semi2k_prep.hpp
 *
 * Async-externalized preprocessing for the semi2k protocol (Z/2^64).
 *
 * Confidentiality model:
 *   Each data_provider generates s_i locally, splits it into N additive shares
 *   over Z/2^64, and writes one share file per computation node:
 *       provider_secrets/provider_<id>_share_<p>.secret
 *
 *   The bridge reads only the per-party share files — it never holds s_i in
 *   full and cannot reconstruct x_i = (x_i - s_i) + s_i.
 *
 * Public API:
 *   Semi2kPrep::load_provider_share()  — read one party’s share for one provider
 *   Semi2kPrep::prepare_player_data()  — build Player-Data/Input-P*-0 and
 *                                        Player-Data/Public-Masked-Values
 *   Semi2kPrep::additive_shares_z2k64() — helper for share generation
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace Semi2kPrep {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// One validated provider entry used by prepare_player_data.
// Masked wire and per-party share files are required (no plaintext-only path).
struct ProviderEntry {
    int         id            = -1;
    std::string masked_value; // (x_i - s_i) as decimal string (may be negative)
};

// per_party_shares[provider_id][party_idx] = uint64_t share
using ShareMatrix = std::unordered_map<int, std::vector<uint64_t>>;

// ---------------------------------------------------------------------------
// 1. Per-party share file access
// ---------------------------------------------------------------------------

/**
 * Read the share of s_i for party `party` written by provider `provider_id`.
 * File: <root>/provider_secrets/provider_<id>_share_<party>.secret
 *
 * Returns the uint64_t share value, or std::nullopt if missing/unparseable.
 */
std::optional<uint64_t> load_provider_share(
    const fs::path& root, int provider_id, int party);

/**
 * Load per-party shares for all providers whose share files exist under
 * <root>/provider_secrets/.  Builds a ShareMatrix indexed by provider id.
 * Only share files for `party` are read.
 */
ShareMatrix load_party_shares(const fs::path& root, int party, int n_parties);

// ---------------------------------------------------------------------------
// 2. Player-Data preparation
// ---------------------------------------------------------------------------

/**
 * For each provider in `selected`:
 *   a. Reads share_p(s_i) from the per-party share files (bridge never sees s_i).
 *   b. Writes the per-party input files:
 *        <mp_spdz_root>/Player-Data/Input-P{p}-0
 *      Each line is the share of s_i owned by party p.
 *   c. Writes the public masked-value file:
 *        <mp_spdz_root>/Player-Data/Public-Masked-Values
 *
 * Returns true on success, false on any failure.
 */
bool prepare_player_data(
    const fs::path&           mp_spdz_root,
    const std::vector<ProviderEntry>& selected,
    const fs::path&           secrets_root,
    int                       n_parties);

// ---------------------------------------------------------------------------
// 3. Helpers
// ---------------------------------------------------------------------------

/**
 * Generate additive shares of `secret` over Z/2^64.
 * Returns a vector of n_parties uint64_t values whose sum equals secret (mod 2^64).
 * Uses libsodium randombytes_buf.
 */
std::vector<uint64_t> additive_shares_z2k64(uint64_t secret, int n_parties);

} // namespace Semi2kPrep
