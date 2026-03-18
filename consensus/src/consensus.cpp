#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sodium.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Représente une entrée provider déjà validée syntaxiquement.
struct ProviderInput {
    int id = -1;
    long long value = 0;
    std::string masked_value_str;  // Pour les valeurs masquées, garde la chaîne complète
    std::string nonce;
    std::string proof;
};

// Parse un entier strict (la chaîne entière doit être numérique).
std::optional<long long> parse_integer(const std::string& s) {
    try {
        size_t idx = 0;
        const long long v = std::stoll(s, &idx);
        if (idx != s.size()) {
            return std::nullopt;
        }
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::string to_hex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string compute_proof(const std::string& id,
                          const std::string& value,
                          const std::string& nonce,
                          const std::string& secret) {
    const std::string message = "id=" + id + ";value=" + value + ";nonce=" + nonce;
    unsigned char digest[crypto_generichash_BYTES] = {0};
    crypto_generichash_state state;
    crypto_generichash_init(&state,
                            reinterpret_cast<const unsigned char*>(secret.data()),
                            secret.size(),
                            sizeof(digest));
    crypto_generichash_update(&state,
                              reinterpret_cast<const unsigned char*>(message.data()),
                              message.size());
    crypto_generichash_final(&state, digest, sizeof(digest));
    return to_hex(digest, sizeof(digest));
}

// Parse un fichier provider au format strict :
//   id=<entier>
//   value=<entier>
//   nonce=<hex>
//   proof=<hex blake2b clé-partagée>
// Toute ligne manquante ou supplémentaire rend le fichier invalide.
std::optional<ProviderInput> parse_provider_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::nullopt;
    }

    std::string line1;
    std::string line2;
    std::string line3;
    std::string line4;
    std::string extra;
    if (!std::getline(in, line1) || !std::getline(in, line2) ||
        !std::getline(in, line3) || !std::getline(in, line4)) {
        return std::nullopt;
    }
    if (std::getline(in, extra)) {
        return std::nullopt;
    }

    const std::string id_prefix = "id=";
    const std::string value_prefix = "value=";
    const std::string masked_value_prefix = "masked_value=";
    const std::string nonce_prefix = "nonce=";
    const std::string proof_prefix = "proof=";
    
    // Support both old format (value=) and new format (masked_value=)
    bool is_masked = (line2.rfind(masked_value_prefix, 0) == 0);
    bool is_plain = (line2.rfind(value_prefix, 0) == 0);
    
    if (line1.rfind(id_prefix, 0) != 0 || (!is_masked && !is_plain) ||
        line3.rfind(nonce_prefix, 0) != 0 || line4.rfind(proof_prefix, 0) != 0) {
        return std::nullopt;
    }

    const auto id = parse_integer(line1.substr(id_prefix.size()));
    
    // Extract value based on detected format
    size_t value_offset = is_masked ? masked_value_prefix.size() : value_prefix.size();
    std::string value_str = line2.substr(value_offset);
    
    std::optional<long long> value;
    std::string masked_value_str;
    if (is_masked) {
        // Pour les valeurs masquées, on garde la chaîne complète
        masked_value_str = value_str;
        // On ne parse pas comme entier pour les masquées
        value = 0;  // valeur dummy
    } else {
        // Pour les valeurs plain, on parse comme entier
        value = parse_integer(value_str);
    }
    
    if (!id || (!is_masked && !value)) {
        return std::nullopt;
    }

    ProviderInput parsed;
    parsed.id = static_cast<int>(*id);
    parsed.value = is_masked ? 0 : *value;  // valeur dummy pour masquées
    parsed.masked_value_str = masked_value_str;
    parsed.nonce = line3.substr(nonce_prefix.size());
    parsed.proof = line4.substr(proof_prefix.size());
    return parsed;
}

int main(int argc, char* argv[]) {
    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium\n";
        return 1;
    }
    const std::string secret = []() {
        const char* env = std::getenv("MPC_PROVIDER_SECRET");
        if (env && *env) {
            return std::string(env);
        }
        return std::string("mpc-demo-secret");
    }();

    // Arguments:
    //   ./consensus [min_inputs] [--clean-inputs]
    //   ./consensus [--clean-inputs] [min_inputs]
    int min_inputs = 3;
    bool clean_inputs = false;
    bool min_inputs_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--clean-inputs") {
            clean_inputs = true;
            continue;
        }

        if (!min_inputs_set) {
            const auto parsed_min_inputs = parse_integer(arg);
            if (!parsed_min_inputs || *parsed_min_inputs <= 0) {
                std::cerr << "Invalid argument: " << arg << "\n";
                std::cerr << "Usage: ./consensus [min_inputs] [--clean-inputs]\n";
                return 1;
            }
            min_inputs = static_cast<int>(*parsed_min_inputs);
            min_inputs_set = true;
            continue;
        }

        std::cerr << "Unexpected argument: " << arg << "\n";
        std::cerr << "Usage: ./consensus [min_inputs] [--clean-inputs]\n";
        return 1;
    }

    // Le consensus décide immédiatement sur les entrées disponibles.
    std::cout << "Consensus quorum policy: need at least " << min_inputs
              << " valid input(s) to decide a core set.\n";

    const fs::path inputs_dir = fs::current_path() / "inputs";
    const fs::path core_set_file = fs::current_path() / "core_set.txt";
    std::vector<int> core_set_ids;

    if (clean_inputs && fs::exists(inputs_dir)) {
        // Nettoyage défensif: supprime uniquement les fichiers provider_*.txt obsolètes.
        // Les fichiers récents (potentiellement de l'exécution courante) sont conservés.
        const std::regex filename_regex(R"(provider_(\d+)\.txt)");
        const auto now = fs::file_time_type::clock::now();
        constexpr auto stale_grace = std::chrono::seconds(30);
        size_t removed_count = 0;

        for (const auto& entry : fs::directory_iterator(inputs_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(filename, match, filename_regex)) {
                continue;
            }

            std::error_code ec;
            const auto last_write = fs::last_write_time(entry.path(), ec);
            if (ec) {
                continue;
            }

            if ((now - last_write) > stale_grace) {
                fs::remove(entry.path(), ec);
                if (!ec) {
                    ++removed_count;
                }
            }
        }

        if (removed_count > 0) {
            std::cout << "Cleaned " << removed_count << " stale provider input file(s).\n";
        }
    }

    if (!fs::exists(inputs_dir)) {
        std::cout << "Inputs directory not found. Writing empty core set.\n";
    } else {
        // On n'analyse que les noms de fichiers conformes à provider_<id>.txt.
        const std::regex filename_regex(R"(provider_(\d+)\.txt)");
        for (const auto& entry : fs::directory_iterator(inputs_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(filename, match, filename_regex)) {
                continue;
            }

            const auto parsed_file_id = parse_integer(match[1].str());
            if (!parsed_file_id) {
                continue;
            }

            const auto parsed = parse_provider_file(entry.path());
            if (!parsed || parsed->id != *parsed_file_id) {
                // Un fichier mal formé ou incohérent est traité comme participant invalide.
                std::cout << "Ignoring malformed provider file: " << entry.path() << "\n";
                continue;
            }
            const std::string expected_proof = compute_proof(
                std::to_string(parsed->id), 
                parsed->masked_value_str.empty() ? std::to_string(parsed->value) : parsed->masked_value_str,
                parsed->nonce, secret);
            if (expected_proof != parsed->proof) {
                std::cout << "Ignoring provider file with invalid cryptographic proof: "
                          << entry.path() << "\n";
                continue;
            }

            core_set_ids.push_back(parsed->id);
        }
    }

    // Déduplication défensive (utile si un même id apparaît plusieurs fois).
    std::sort(core_set_ids.begin(), core_set_ids.end());
    core_set_ids.erase(std::unique(core_set_ids.begin(), core_set_ids.end()), core_set_ids.end());

    if (static_cast<int>(core_set_ids.size()) < min_inputs) {
        if (fs::exists(core_set_file)) {
            fs::remove(core_set_file);
        }
        std::cerr << "Not enough valid inputs to decide core set (have "
                  << core_set_ids.size() << ", need " << min_inputs << ").\n";
        std::cerr << "No core_set.txt produced.\n";
        return 1;
    }

    std::ofstream out(core_set_file);
    if (!out.is_open()) {
        std::cerr << "Failed to write core set file: " << core_set_file << "\n";
        return 1;
    }

    for (const int id : core_set_ids) {
        // Le core set est écrit dans un format simple : un id par ligne.
        out << id << "\n";
    }

    std::cout << "Core set decided with " << core_set_ids.size() << " provider(s): ";
    for (size_t i = 0; i < core_set_ids.size(); ++i) {
        std::cout << core_set_ids[i];
        if (i + 1 < core_set_ids.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "\n";
    std::cout << "Wrote " << core_set_file << "\n";

    return 0;
}
