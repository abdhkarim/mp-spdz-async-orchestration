#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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

struct AckEvidence {
    std::string session_id;
    int round_id = 0;
    int provider_id = -1;
    int computation_node_id = -1;
    std::string input_hash;
    long long timestamp_unix_ms = 0;
    std::string signature;
};

struct AckValidationResult {
    bool ok = false;
    std::string reason;
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

std::optional<std::string> read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

std::optional<std::string> parse_json_string_field(const std::string& json,
                                                   const std::string& field) {
    const std::regex re("\"" + field + "\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
        return std::nullopt;
    }
    return m[1].str();
}

std::optional<long long> parse_json_int_field(const std::string& json,
                                              const std::string& field) {
    const std::regex re("\"" + field + R"("\s*:\s*(-?\d+))");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
        return std::nullopt;
    }
    return parse_integer(m[1].str());
}

std::optional<AckEvidence> parse_ack_file(const fs::path& ack_path) {
    const auto content_opt = read_text_file(ack_path);
    if (!content_opt) {
        return std::nullopt;
    }
    const std::string& json = *content_opt;

    const auto session_id = parse_json_string_field(json, "session_id");
    const auto round_id = parse_json_int_field(json, "round_id");
    const auto provider_id = parse_json_int_field(json, "provider_id");
    const auto computation_node_id = parse_json_int_field(json, "computation_node_id");
    const auto input_hash = parse_json_string_field(json, "input_hash");
    const auto timestamp_unix_ms = parse_json_int_field(json, "timestamp_unix_ms");
    const auto signature = parse_json_string_field(json, "signature");

    if (!session_id || !round_id || !provider_id || !computation_node_id ||
        !input_hash || !timestamp_unix_ms || !signature) {
        return std::nullopt;
    }

    AckEvidence ack;
    ack.session_id = *session_id;
    ack.round_id = static_cast<int>(*round_id);
    ack.provider_id = static_cast<int>(*provider_id);
    ack.computation_node_id = static_cast<int>(*computation_node_id);
    ack.input_hash = *input_hash;
    ack.timestamp_unix_ms = *timestamp_unix_ms;
    ack.signature = *signature;
    return ack;
}

std::optional<std::string> hash_file_blake2b_hex(const fs::path& file_path) {
    const auto content_opt = read_text_file(file_path);
    if (!content_opt) {
        return std::nullopt;
    }

    const std::string& content = *content_opt;
    unsigned char digest[32] = {0};  // 64 hex chars
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, sizeof(digest));
    crypto_generichash_update(&state,
                              reinterpret_cast<const unsigned char*>(content.data()),
                              content.size());
    crypto_generichash_final(&state, digest, sizeof(digest));
    return to_hex(digest, sizeof(digest));
}

std::optional<std::vector<unsigned char>> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const char h = hex[2 * i];
        const char l = hex[2 * i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(h);
        const int lo = nibble(l);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

std::string trim_ascii_ws(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ack_signing_message(const AckEvidence& ack) {
    return ack.session_id + "|" +
           std::to_string(ack.round_id) + "|" +
           std::to_string(ack.provider_id) + "|" +
           std::to_string(ack.computation_node_id) + "|" +
           ack.input_hash + "|" +
           std::to_string(ack.timestamp_unix_ms);
}

bool verify_ack_signature(const AckEvidence& ack, const fs::path& cn_keys_dir) {
    const fs::path pk_file =
        cn_keys_dir / ("cn_" + std::to_string(ack.computation_node_id) + ".pub.hex");
    const auto pk_hex_opt = read_text_file(pk_file);
    if (!pk_hex_opt) return false;
    const auto pk_opt = from_hex(trim_ascii_ws(*pk_hex_opt));
    const auto sig_opt = from_hex(trim_ascii_ws(ack.signature));
    if (!pk_opt || !sig_opt) return false;
    if (pk_opt->size() != crypto_sign_PUBLICKEYBYTES ||
        sig_opt->size() != crypto_sign_BYTES) {
        return false;
    }

    const std::string msg = ack_signing_message(ack);
    return crypto_sign_verify_detached(
               sig_opt->data(),
               reinterpret_cast<const unsigned char*>(msg.data()),
               msg.size(),
               pk_opt->data()) == 0;
}

void write_core_set_json(const fs::path& path,
                         const std::string& session_id,
                         int round_id,
                         int k_required,
                         int timeout_seconds,
                         const std::vector<int>& provider_ids) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << "{\n"
        << "  \"session_id\": \"" << session_id << "\",\n"
        << "  \"round_id\": " << round_id << ",\n"
        << "  \"decided_at_unix_ms\": " << now_ms << ",\n"
        << "  \"k_required\": " << k_required << ",\n"
        << "  \"timeout_seconds\": " << timeout_seconds << ",\n"
        << "  \"provider_ids\": [";
    for (size_t i = 0; i < provider_ids.size(); ++i) {
        out << provider_ids[i];
        if (i + 1 < provider_ids.size()) out << ", ";
    }
    out << "]\n}\n";
}

void write_justification_json(const fs::path& path,
                              const std::string& session_id,
                              int round_id,
                              const std::vector<int>& accepted,
                              const std::map<int, std::string>& rejected_reasons,
                              const std::map<int, int>& distinct_acks_by_provider,
                              const std::map<int, std::vector<std::string>>& evidence_files) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << "{\n"
        << "  \"session_id\": \"" << session_id << "\",\n"
        << "  \"round_id\": " << round_id << ",\n"
        << "  \"accepted\": [\n";
    for (size_t i = 0; i < accepted.size(); ++i) {
        out << "    {\"provider_id\": " << accepted[i]
            << ", \"reason\": \"k_of_n_valid_acks\""
            << ", \"distinct_ack_count\": " << (distinct_acks_by_provider.count(accepted[i]) ? distinct_acks_by_provider.at(accepted[i]) : 0)
            << ", \"ack_files\": [";
        if (evidence_files.count(accepted[i])) {
            const auto& files = evidence_files.at(accepted[i]);
            for (size_t k = 0; k < files.size(); ++k) {
                out << "\"" << files[k] << "\"";
                if (k + 1 < files.size()) out << ", ";
            }
        }
        out << "]}";
        if (i + 1 < accepted.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n"
        << "  \"rejected\": [\n";
    size_t idx = 0;
    for (const auto& [provider_id, reason] : rejected_reasons) {
        out << "    {\"provider_id\": " << provider_id
            << ", \"reason\": \"" << reason << "\""
            << ", \"distinct_ack_count\": " << (distinct_acks_by_provider.count(provider_id) ? distinct_acks_by_provider.at(provider_id) : 0)
            << ", \"ack_files\": [";
        if (evidence_files.count(provider_id)) {
            const auto& files = evidence_files.at(provider_id);
            for (size_t k = 0; k < files.size(); ++k) {
                out << "\"" << files[k] << "\"";
                if (k + 1 < files.size()) out << ", ";
            }
        }
        out << "]}";
        if (idx + 1 < rejected_reasons.size()) out << ",";
        out << "\n";
        ++idx;
    }
    out << "  ]\n}\n";
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
    bool ack_mode = false;
    fs::path acks_dir;
    int k_required = 2;
    std::string session_id = "demo-session";
    int round_id = 0;
    int timeout_seconds = 0;
    fs::path artifacts_dir = fs::current_path() / "artifacts";
    fs::path cn_keys_dir = fs::current_path() / "artifacts" / "cn_keys";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--clean-inputs") {
            clean_inputs = true;
            continue;
        }
        if (arg == "--acks-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --acks-dir\n";
                return 1;
            }
            acks_dir = argv[++i];
            ack_mode = true;
            continue;
        }
        if (arg == "--k") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --k\n";
                return 1;
            }
            const auto parsed = parse_integer(argv[++i]);
            if (!parsed || *parsed <= 0) {
                std::cerr << "Invalid value for --k\n";
                return 1;
            }
            k_required = static_cast<int>(*parsed);
            continue;
        }
        if (arg == "--session-id") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --session-id\n";
                return 1;
            }
            session_id = argv[++i];
            continue;
        }
        if (arg == "--round-id") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --round-id\n";
                return 1;
            }
            const auto parsed = parse_integer(argv[++i]);
            if (!parsed || *parsed < 0) {
                std::cerr << "Invalid value for --round-id\n";
                return 1;
            }
            round_id = static_cast<int>(*parsed);
            continue;
        }
        if (arg == "--timeout-seconds") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --timeout-seconds\n";
                return 1;
            }
            const auto parsed = parse_integer(argv[++i]);
            if (!parsed || *parsed < 0) {
                std::cerr << "Invalid value for --timeout-seconds\n";
                return 1;
            }
            timeout_seconds = static_cast<int>(*parsed);
            continue;
        }
        if (arg == "--artifacts-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --artifacts-dir\n";
                return 1;
            }
            artifacts_dir = argv[++i];
            continue;
        }
        if (arg == "--cn-keys-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --cn-keys-dir\n";
                return 1;
            }
            cn_keys_dir = argv[++i];
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
    std::map<int, std::string> rejected_reasons;
    std::map<int, int> distinct_acks_by_provider;
    std::map<int, std::vector<std::string>> evidence_files_by_provider;

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

    if (ack_mode) {
        if (!fs::exists(acks_dir) || !fs::is_directory(acks_dir)) {
            std::cerr << "ACK mode enabled but acks directory not found: " << acks_dir << "\n";
            return 1;
        }

        std::map<int, std::set<int>> distinct_acks_by_provider_set;
        std::set<std::pair<int, int>> seen_provider_cn;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const std::regex ack_filename_regex(R"(ack_.*\.json)");
        for (const auto& entry : fs::directory_iterator(acks_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            if (!std::regex_match(filename, ack_filename_regex)) continue;

            const auto ack_opt = parse_ack_file(entry.path());
            if (!ack_opt) continue;
            const AckEvidence& ack = *ack_opt;

            if (ack.session_id != session_id || ack.round_id != round_id) continue;
            if (!verify_ack_signature(ack, cn_keys_dir)) {
                rejected_reasons[ack.provider_id] = "invalid_signature";
                continue;
            }

            if (timeout_seconds > 0) {
                const long long min_allowed = now_ms - static_cast<long long>(timeout_seconds) * 1000LL;
                if (ack.timestamp_unix_ms < min_allowed || ack.timestamp_unix_ms > now_ms + 5000) {
                    rejected_reasons[ack.provider_id] = "ack_outside_timeout_window";
                    continue;
                }
            }

            const fs::path provider_file = inputs_dir / ("provider_" + std::to_string(ack.provider_id) + ".txt");
            const auto hash_opt = hash_file_blake2b_hex(provider_file);
            if (!hash_opt || *hash_opt != ack.input_hash) {
                rejected_reasons[ack.provider_id] = "ack_hash_mismatch_or_missing_provider_file";
                continue;
            }

            const std::pair<int, int> replay_key{ack.provider_id, ack.computation_node_id};
            if (seen_provider_cn.count(replay_key) != 0) {
                rejected_reasons[ack.provider_id] = "ack_replay_detected";
                continue;
            }
            seen_provider_cn.insert(replay_key);
            distinct_acks_by_provider_set[ack.provider_id].insert(ack.computation_node_id);
            evidence_files_by_provider[ack.provider_id].push_back(entry.path().filename().string());
        }

        for (const auto& [provider_id, cn_ids] : distinct_acks_by_provider_set) {
            distinct_acks_by_provider[provider_id] = static_cast<int>(cn_ids.size());
        }

        std::vector<int> ack_selected;
        for (int provider_id : core_set_ids) {
            const int n_distinct_acks =
                (distinct_acks_by_provider.count(provider_id) ? distinct_acks_by_provider[provider_id] : 0);
            if (n_distinct_acks >= k_required) {
                ack_selected.push_back(provider_id);
            } else {
                if (rejected_reasons.count(provider_id) == 0) {
                    rejected_reasons[provider_id] = "insufficient_distinct_acks";
                }
            }
        }
        core_set_ids = ack_selected;
    }

    // Déduplication défensive (utile si un même id apparaît plusieurs fois).
    std::sort(core_set_ids.begin(), core_set_ids.end());
    core_set_ids.erase(std::unique(core_set_ids.begin(), core_set_ids.end()), core_set_ids.end());

    if (static_cast<int>(core_set_ids.size()) < min_inputs) {
        if (ack_mode) {
            fs::create_directories(artifacts_dir);
            write_core_set_json(artifacts_dir / "core_set.json",
                                session_id, round_id, k_required, timeout_seconds, core_set_ids);
            write_justification_json(artifacts_dir / "justification.json",
                                     session_id, round_id, core_set_ids, rejected_reasons,
                                     distinct_acks_by_provider, evidence_files_by_provider);
            std::cout << "Wrote ACK-based artifacts in " << artifacts_dir << "\n";
        }
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

    if (ack_mode) {
        fs::create_directories(artifacts_dir);
        write_core_set_json(artifacts_dir / "core_set.json",
                            session_id, round_id, k_required, timeout_seconds, core_set_ids);
        write_justification_json(artifacts_dir / "justification.json",
                                 session_id, round_id, core_set_ids, rejected_reasons,
                                 distinct_acks_by_provider, evidence_files_by_provider);
        std::cout << "Wrote ACK-based artifacts in " << artifacts_dir << "\n";
    }

    return 0;
}
