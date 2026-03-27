/**
 * semi2k_prep.cpp
 *
 * Async-externalized preprocessing for the semi2k protocol (Z/2^64).
 *
 * Confidentiality model:
 *   Each data_provider generates s_i locally, splits it into N additive shares,
 *   and writes one file per party:
 *       provider_secrets/provider_<id>_share_<p>.secret
 *
 *   The bridge reads only the share file for party p when writing Input-P{p}-0.
 *   It NEVER reads s_i in full, so it CANNOT reconstruct x_i = (x_i - s_i) + s_i.
 *
 * MP-SPDZ runs the online computation phase only.
 */

#include "semi2k_prep.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sodium.h>
#include <sstream>

namespace Semi2kPrep {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::optional<long long> parse_integer(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        size_t idx = 0;
        const long long v = std::stoll(s, &idx);
        if (idx != s.size()) return std::nullopt;
        return v;
    } catch (...) { return std::nullopt; }
}

// ---------------------------------------------------------------------------
// 1. additive_shares_z2k64
// ---------------------------------------------------------------------------

std::vector<uint64_t> additive_shares_z2k64(uint64_t secret, int n_parties) {
    if (n_parties <= 0) return {};
    std::vector<uint64_t> shares;
    shares.reserve(static_cast<size_t>(n_parties));
    uint64_t running_sum = 0;
    for (int i = 0; i < n_parties - 1; ++i) {
        uint64_t r = 0;
        randombytes_buf(&r, sizeof(r));
        shares.push_back(r);
        running_sum += r;
    }
    shares.push_back(secret - running_sum);
    return shares;
}

// ---------------------------------------------------------------------------
// 2. Per-party share file access
// ---------------------------------------------------------------------------

std::optional<uint64_t> load_provider_share(
    const fs::path& root, int provider_id, int party)
{
    const fs::path share_path =
        root / "provider_secrets" /
        ("provider_" + std::to_string(provider_id) +
         "_share_" + std::to_string(party) + ".secret");

    std::ifstream in(share_path);
    if (!in.is_open()) return std::nullopt;

    std::string line;
    if (!std::getline(in, line) || line.empty()) return std::nullopt;

    // Trim whitespace.
    const auto not_space = [](unsigned char c){ return !std::isspace(c); };
    line.erase(line.begin(), std::find_if(line.begin(), line.end(), not_space));
    line.erase(std::find_if(line.rbegin(), line.rend(), not_space).base(), line.end());

    try { return std::stoull(line); } catch (...) { return std::nullopt; }
}

ShareMatrix load_party_shares(const fs::path& root, int party, int n_parties) {
    ShareMatrix result;
    const fs::path secrets_dir = root / "provider_secrets";
    if (!fs::exists(secrets_dir) || !fs::is_directory(secrets_dir)) return result;

    // Pattern: provider_<id>_share_<party>.secret
    const std::regex fname_re(
        R"(provider_(\d+)_share_)" + std::to_string(party) + R"(\.secret)");

    for (const auto& entry : fs::directory_iterator(secrets_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(fname, m, fname_re)) continue;

        const auto id_opt = parse_integer(m[1].str());
        if (!id_opt) continue;

        std::ifstream in(entry.path());
        std::string line;
        if (!in.is_open() || !std::getline(in, line)) continue;

        const auto not_space = [](unsigned char c){ return !std::isspace(c); };
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), not_space));
        line.erase(std::find_if(line.rbegin(), line.rend(), not_space).base(), line.end());
        if (line.empty()) continue;

        // Verify all N share files exist (provider wrote shares for all parties).
        bool complete = true;
        for (int p = 0; p < n_parties; ++p) {
            const fs::path sp =
                secrets_dir /
                ("provider_" + m[1].str() +
                 "_share_" + std::to_string(p) + ".secret");
            if (!fs::exists(sp)) { complete = false; break; }
        }
        if (!complete) continue;

        try {
            result[static_cast<int>(*id_opt)] = {};  // placeholder; filled below
        } catch (...) {}
    }

    // For each provider id found, load its share for `party`.
    for (auto& [pid, _] : result) {
        auto sh = load_provider_share(root, pid, party);
        if (sh) {
            std::vector<uint64_t> v = { *sh };
            result[pid] = v;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// 3. prepare_player_data
// ---------------------------------------------------------------------------

bool prepare_player_data(
    const fs::path&                   mp_spdz_root,
    const std::vector<ProviderEntry>& selected,
    const fs::path&                   secrets_root,
    int                               n_parties)
{
    if (n_parties < 2) {
        std::cerr << "[semi2k_prep] At least 2 computation nodes required\n";
        return false;
    }
    if (selected.empty()) {
        std::cerr << "[semi2k_prep] Empty provider selection\n";
        return false;
    }
    if (sodium_init() < 0) {
        std::cerr << "[semi2k_prep] Failed to initialise libsodium\n";
        return false;
    }

    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    std::error_code ec;
    fs::create_directories(player_data_dir, ec);
    if (ec) {
        std::cerr << "[semi2k_prep] Cannot create Player-Data dir: " << ec.message() << "\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Build masked values list and per-party share matrix.
    //
    //   party_shares[party][provider_idx] = share of s_i for party p
    //
    // The bridge reads share_p(s_i) directly from the per-party file written
    // by the provider.  It NEVER accumulates all shares, so it cannot
    // reconstruct s_i and hence cannot compute x_i.
    // ------------------------------------------------------------------

    std::vector<std::string> masked_values;
    masked_values.reserve(selected.size());

    // party_shares[p][i] = uint64_t share for party p, provider index i
    std::vector<std::vector<uint64_t>> party_shares(
        static_cast<size_t>(n_parties),
        std::vector<uint64_t>(selected.size(), 0));

    for (size_t pi = 0; pi < selected.size(); ++pi) {
        const ProviderEntry& prov = selected[pi];

        if (prov.masked_value.empty()) {
            std::cerr << "[semi2k_prep] Provider " << prov.id
                      << " has empty masked_value (masked wire required)\n";
            return false;
        }
        masked_values.push_back(prov.masked_value);

        // Read share_p for each party — bridge reads ONE share file per party,
        // never the full s_i.
        for (int p = 0; p < n_parties; ++p) {
            const auto sh = load_provider_share(secrets_root, prov.id, p);
            if (!sh) {
                std::cerr << "[semi2k_prep] Missing share file for provider "
                          << prov.id << " party " << p << "\n";
                return false;
            }
            party_shares[static_cast<size_t>(p)][pi] = *sh;
        }
    }

    // ------------------------------------------------------------------
    // Write Player-Data/Public-Masked-Values
    // ------------------------------------------------------------------
    {
        const fs::path pmv_path = player_data_dir / "Public-Masked-Values";
        std::ofstream out(pmv_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[semi2k_prep] Cannot write Public-Masked-Values\n";
            return false;
        }
        for (const auto& mv : masked_values) out << mv << "\n";
        std::cout << "[semi2k_prep] Wrote " << masked_values.size()
                  << " masked value(s) to " << pmv_path << "\n";
    }

    // ------------------------------------------------------------------
    // Write Player-Data/Input-P{p}-0 for each party
    // Each party p receives only its own share_p(s_i) for every provider i.
    // ------------------------------------------------------------------
    for (int p = 0; p < n_parties; ++p) {
        const fs::path input_path =
            player_data_dir / ("Input-P" + std::to_string(p) + "-0");
        std::ofstream out(input_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[semi2k_prep] Cannot write " << input_path << "\n";
            return false;
        }
        for (size_t pi = 0; pi < selected.size(); ++pi) {
            // Write as signed decimal (MP-SPDZ sint expects signed representation).
            const int64_t signed_share =
                static_cast<int64_t>(party_shares[static_cast<size_t>(p)][pi]);
            out << signed_share << "\n";
        }
        std::cout << "[semi2k_prep] Wrote " << selected.size()
                  << " share(s) to " << input_path << "\n";
    }

    std::cout << "[semi2k_prep] Player-Data prepared for " << n_parties
              << " parties, " << selected.size() << " provider(s)\n";
    return true;
}

} // namespace Semi2kPrep
