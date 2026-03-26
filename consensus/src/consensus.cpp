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

#include "type_proof.hpp"

namespace fs = std::filesystem;

// Représente une entrée provider déjà validée syntaxiquement.
struct ProviderInput {
    int id = -1;
    long long value = 0;
    std::string masked_value_str;  // Pour les valeurs masquées, garde la chaîne complète
    std::string wire_value_str;    // Raw wire integer string (masked_value or value=...)
    std::string nonce;
    std::string proof;
};

struct AckEvidence {
    // Two ACK formats exist in this repo:
    // 1) "share_verifier" ACKs (modern): include share commitments/digests and
    //    are signed over a long binding message.
    // 2) "async_orchestrator" ACKs (legacy): only bind to (session, round,
    //    provider_id, cn_id, input_hash, timestamp) and don't include share
    //    commitment fields.
    bool is_legacy_ack = false;
    std::string session_id;
    std::string protocol_version;
    int round_id = 0;
    std::string schema_id;
    int provider_id = -1;
    std::string nonce;
    std::string input_hash; // legacy ACK only
    int party_index = -1;
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;
    std::string share_file_digest;
    std::string share_commitment;
    std::string share_commitment_r;
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

// Canonical wire validation for signed base-10 integers:
// - no leading '+'
// - no whitespace
// - no leading zeros except the exact string "0"
// - "-0" is rejected (must be "0")
static bool is_canonical_signed_decimal_wire(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    }
    if (s[0] == '+') return false;
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (s == "0") return true;
    if (s[0] == '-') {
        if (s.size() < 2) return false;
        if (s[1] == '0') return false; // reject "-01", "-00", ...
        for (size_t i = 1; i < s.size(); ++i) if (!is_digit(s[i])) return false;
        return true;
    }
    // positive
    if (s[0] == '0') return false; // reject leading zeros
    for (char c : s) if (!is_digit(c)) return false;
    return true;
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
    parsed.wire_value_str = value_str;
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

    // ---------------------------
    // Try modern ACK first
    // ---------------------------
    const auto session_id = parse_json_string_field(json, "session_id");
    const auto protocol_version = parse_json_string_field(json, "protocol_version");
    const auto round_id = parse_json_int_field(json, "round_id");
    const auto schema_id = parse_json_string_field(json, "schema_id");
    const auto provider_id = parse_json_int_field(json, "provider_id");
    const auto nonce = parse_json_string_field(json, "nonce");
    const auto party_index = parse_json_int_field(json, "party_index");
    const auto share_manifest_id = parse_json_string_field(json, "share_manifest_id");
    const auto mask_commitment_leaves_digest = parse_json_string_field(json, "mask_commitment_leaves_digest");
    const auto share_file_digest = parse_json_string_field(json, "share_file_digest");
    const auto share_commitment = parse_json_string_field(json, "share_commitment");
    const auto share_commitment_r = parse_json_string_field(json, "share_commitment_r");
    const auto timestamp_unix_ms = parse_json_int_field(json, "timestamp_unix_ms");
    const auto signature = parse_json_string_field(json, "signature");

    if (session_id && protocol_version && round_id && schema_id && provider_id && nonce && party_index &&
        share_manifest_id && mask_commitment_leaves_digest && share_file_digest && share_commitment &&
        share_commitment_r && timestamp_unix_ms && signature) {
        AckEvidence ack;
        ack.is_legacy_ack = false;
        ack.session_id = *session_id;
        ack.protocol_version = *protocol_version;
        ack.round_id = static_cast<int>(*round_id);
        ack.schema_id = *schema_id;
        ack.provider_id = static_cast<int>(*provider_id);
        ack.nonce = *nonce;
        ack.party_index = static_cast<int>(*party_index);
        ack.share_manifest_id = *share_manifest_id;
        ack.mask_commitment_leaves_digest = *mask_commitment_leaves_digest;
        ack.share_file_digest = *share_file_digest;
        ack.share_commitment = *share_commitment;
        ack.share_commitment_r = *share_commitment_r;
        ack.timestamp_unix_ms = *timestamp_unix_ms;
        ack.signature = *signature;
        return ack;
    }

    // ---------------------------
    // Fallback: legacy ACK format
    // (generated by scripts/async_orchestrator.py)
    //
    // Fields:
    //  - session_id, round_id, provider_id
    //  - computation_node_id (maps to party_index)
    //  - input_hash
    //  - timestamp_unix_ms, signature
    // ---------------------------
    const auto legacy_session_id = parse_json_string_field(json, "session_id");
    const auto legacy_round_id = parse_json_int_field(json, "round_id");
    const auto legacy_provider_id = parse_json_int_field(json, "provider_id");
    const auto legacy_party_index = parse_json_int_field(json, "computation_node_id");
    const auto legacy_input_hash = parse_json_string_field(json, "input_hash");
    const auto legacy_timestamp_unix_ms = parse_json_int_field(json, "timestamp_unix_ms");
    const auto legacy_signature = parse_json_string_field(json, "signature");

    if (!legacy_session_id || !legacy_round_id || !legacy_provider_id || !legacy_party_index ||
        !legacy_input_hash || !legacy_timestamp_unix_ms || !legacy_signature) {
        return std::nullopt;
    }

    AckEvidence ack;
    ack.is_legacy_ack = true;
    ack.session_id = *legacy_session_id;
    ack.protocol_version.clear();
    ack.round_id = static_cast<int>(*legacy_round_id);
    ack.schema_id.clear();
    ack.provider_id = static_cast<int>(*legacy_provider_id);
    ack.nonce.clear();
    ack.input_hash = *legacy_input_hash;
    ack.party_index = static_cast<int>(*legacy_party_index);
    ack.timestamp_unix_ms = *legacy_timestamp_unix_ms;
    ack.signature = *legacy_signature;
    // Remaining modern-only fields stay empty.
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

// Unkeyed BLAKE2b-256 hex digest (64 chars), over ASCII bytes.
static std::string blake2b_hex32_unkeyed(const std::string& msg) {
    unsigned char digest[32] = {0};
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, sizeof(digest));
    crypto_generichash_update(&state,
                              reinterpret_cast<const unsigned char*>(msg.data()),
                              msg.size());
    crypto_generichash_final(&state, digest, sizeof(digest));
    return to_hex(digest, sizeof(digest));
}

struct ProviderManifestEvidence {
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;
    int num_parties = -1;
    std::vector<std::string> share_file_digests_by_party;
    std::vector<std::string> share_commitments_by_party;
    std::vector<std::string> share_commitment_rs_by_party;
};

struct ProviderManifestMinimal {
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;
    int num_parties = -1;
};

static std::optional<ProviderManifestEvidence> parse_provider_manifest(
    const fs::path& manifest_path,
    int expected_num_parties) {
    const auto content_opt = read_text_file(manifest_path);
    if (!content_opt) return std::nullopt;
    const std::string& json = *content_opt;

    const auto share_manifest_id = parse_json_string_field(json, "share_manifest_id");
    const auto mask_commitment_leaves_digest = parse_json_string_field(json, "mask_commitment_leaves_digest");
    const auto n_opt = parse_json_int_field(json, "num_parties");
    if (!share_manifest_id || !mask_commitment_leaves_digest || !n_opt) return std::nullopt;

    const int num_parties = static_cast<int>(*n_opt);
    ProviderManifestEvidence ev;
    ev.share_manifest_id = *share_manifest_id;
    ev.mask_commitment_leaves_digest = *mask_commitment_leaves_digest;
    ev.num_parties = num_parties;
    ev.share_file_digests_by_party.assign(static_cast<size_t>(expected_num_parties), "");
    ev.share_commitments_by_party.assign(static_cast<size_t>(expected_num_parties), "");
    ev.share_commitment_rs_by_party.assign(static_cast<size_t>(expected_num_parties), "");

    for (int p = 0; p < expected_num_parties; ++p) {
        // We rely on the manifest being generated by this repo with stable formatting.
        const std::string re_str =
            "\"party_index\"\\s*:\\s*" + std::to_string(p) +
            "\\s*,\\s*\"share_file_digest\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
            "\\s*,\\s*\"share_leaf_digest\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
            "\\s*,\\s*\"share_commitment\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
            "\\s*,\\s*\"share_commitment_r\"\\s*:\\s*\"([a-fA-F0-9]{64})\"";
        const std::regex re(re_str);
        std::smatch m;
        if (!std::regex_search(json, m, re)) return std::nullopt;
        ev.share_file_digests_by_party[static_cast<size_t>(p)] = m[1].str();
        ev.share_commitments_by_party[static_cast<size_t>(p)] = m[3].str();
        ev.share_commitment_rs_by_party[static_cast<size_t>(p)] = m[4].str();
    }

    return ev;
}

static std::optional<ProviderManifestMinimal> parse_provider_manifest_minimal(
    const fs::path& manifest_path) {
    const auto content_opt = read_text_file(manifest_path);
    if (!content_opt) return std::nullopt;
    const std::string& json = *content_opt;

    const auto share_manifest_id = parse_json_string_field(json, "share_manifest_id");
    const auto mask_commitment_leaves_digest =
        parse_json_string_field(json, "mask_commitment_leaves_digest");
    const auto n_opt = parse_json_int_field(json, "num_parties");
    if (!share_manifest_id || !mask_commitment_leaves_digest || !n_opt) return std::nullopt;

    ProviderManifestMinimal out;
    out.share_manifest_id = *share_manifest_id;
    out.mask_commitment_leaves_digest = *mask_commitment_leaves_digest;
    out.num_parties = static_cast<int>(*n_opt);
    return out;
}


static std::string compute_share_manifest_id_from_provider_input(
    const ProviderInput& provider,
    int expected_num_parties) {
    const std::string masked_wire =
        provider.masked_value_str.empty() ? std::to_string(provider.value) : provider.masked_value_str;
    const std::string manifest_input =
        "provider_id=" + std::to_string(provider.id) +
        ";nonce=" + provider.nonce +
        ";masked_value=" + masked_wire +
        ";num_parties=" + std::to_string(expected_num_parties);
    return blake2b_hex32_unkeyed(manifest_input);
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
    // Signatures bind exactly to the fields below (byte-for-byte stability
    // is important for acceptance).
    if (ack.is_legacy_ack) {
        // Must match scripts/async_orchestrator.py::ack_signing_message().
        return ack.session_id + "|" +
               std::to_string(ack.round_id) + "|" +
               std::to_string(ack.provider_id) + "|" +
               std::to_string(ack.party_index) + "|" +
               ack.input_hash + "|" +
               std::to_string(ack.timestamp_unix_ms);
    }

    return ack.protocol_version + "|" +
           std::to_string(ack.round_id) + "|" +
           ack.schema_id + "|" +
           std::to_string(ack.provider_id) + "|" +
           ack.nonce + "|" +
           std::to_string(ack.party_index) + "|" +
           ack.share_manifest_id + "|" +
           ack.mask_commitment_leaves_digest + "|" +
           ack.share_file_digest + "|" +
           ack.share_commitment + "|" +
           ack.share_commitment_r + "|" +
           std::to_string(ack.timestamp_unix_ms);
}

bool verify_ack_signature(const AckEvidence& ack, const fs::path& cn_keys_dir) {
    const fs::path pk_file =
        cn_keys_dir / ("cn_" + std::to_string(ack.party_index) + ".pub.hex");
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
            << ", \"reason\": \"full_party_index_coverage\""
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
    int k_required = 0;  // In ACK mode: required coverage size (#party indices).
    std::string session_id = "demo-session";
    int round_id = 0;
    std::string protocol_version = "1";
    std::string schema_id = "semi2k-wire-v1";
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
        if (arg == "--num-parties") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --num-parties\n";
                return 1;
            }
            const auto parsed = parse_integer(argv[++i]);
            if (!parsed || *parsed <= 0) {
                std::cerr << "Invalid value for --num-parties\n";
                return 1;
            }
            k_required = static_cast<int>(*parsed);
            continue;
        }
        if (arg == "--protocol-version") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --protocol-version\n";
                return 1;
            }
            protocol_version = argv[++i];
            continue;
        }
        if (arg == "--schema-id") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --schema-id\n";
                return 1;
            }
            schema_id = argv[++i];
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
            const std::regex provider_txt_regex(R"(provider_(\d+)\.txt)");
            const std::regex provider_manifest_json_regex(R"(provider_(\d+)_manifest\.json)");
            const std::regex provider_type_proof_json_regex(R"(provider_(\d+)_type_proof\.json)");
        const auto now = fs::file_time_type::clock::now();
        constexpr auto stale_grace = std::chrono::seconds(30);
        size_t removed_count = 0;

        for (const auto& entry : fs::directory_iterator(inputs_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            std::smatch match;
            const bool match_any =
                std::regex_match(filename, match, provider_txt_regex) ||
                std::regex_match(filename, match, provider_manifest_json_regex) ||
                std::regex_match(filename, match, provider_type_proof_json_regex);
            if (!match_any) continue;

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

            // Canonical masked wire validation: this is a purely syntactic check
            // on the decimal integer string.
            if (!is_canonical_signed_decimal_wire(parsed->wire_value_str)) {
                std::cout << "Ignoring provider file with non-canonical integer wire: "
                          << entry.path() << "\n";
                continue;
            }

            // Type proof layer: semantic type validity remains consensus-verified.
            // This is provider-side (no type_ack) and consumed directly here.
            {
                const fs::path manifest_path =
                    inputs_dir / ("provider_" + std::to_string(parsed->id) + "_manifest.json");
                const fs::path type_proof_path =
                    inputs_dir / ("provider_" + std::to_string(parsed->id) + "_type_proof.json");

                const auto manifest_min_opt = parse_provider_manifest_minimal(manifest_path);
                if (!manifest_min_opt) {
                    std::cout << "Ignoring provider: missing/invalid manifest for type_proof "
                              << entry.path() << "\n";
                    continue;
                }
                // In ACK mode, `k_required` is an ACK coverage threshold, not
                // necessarily the total number of parties in the provider manifest.
                // We only need the manifest to contain at least the required
                // party indices [0..k_required-1].
                if (ack_mode && manifest_min_opt->num_parties < k_required) {
                    std::cout << "Ignoring provider: manifest num_parties mismatch for type_proof "
                              << entry.path() << "\n";
                    continue;
                }

                // Binding defense: ensure provider evidence -> manifest share_manifest_id
                // (this is re-computable from provider file only).
                {
                    const std::string expected_smid =
                        compute_share_manifest_id_from_provider_input(*parsed,
                                                                        manifest_min_opt->num_parties);
                    if (expected_smid != manifest_min_opt->share_manifest_id) {
                        std::cout << "Ignoring provider: manifest share_manifest_id mismatch for type_proof "
                                  << entry.path() << "\n";
                        continue;
                    }
                }

                const auto tp_opt = type_proof::load_evidence_file(type_proof_path.string());
                if (!tp_opt) {
                    std::cout << "Ignoring provider: missing/invalid type_proof artifact: "
                              << type_proof_path << "\n";
                    continue;
                }

                std::string reason;
                type_proof::ProviderEvidenceView provider_view;
                provider_view.provider_id = parsed->id;
                provider_view.nonce = parsed->nonce;
                provider_view.masked_wire = parsed->wire_value_str;

                type_proof::ManifestMinimalView manifest_view;
                manifest_view.share_manifest_id = manifest_min_opt->share_manifest_id;
                manifest_view.mask_commitment_leaves_digest = manifest_min_opt->mask_commitment_leaves_digest;

                if (!type_proof::verify_for_provider(
                        provider_view, manifest_view, *tp_opt, schema_id, &reason)) {
                    std::cout << "Ignoring provider: type_proof verification failed (" << reason
                              << "): " << entry.path() << "\n";
                    continue;
                }
            }

            core_set_ids.push_back(parsed->id);
        }
    }

    if (ack_mode) {
        if (!fs::exists(acks_dir) || !fs::is_directory(acks_dir)) {
            std::cerr << "ACK mode enabled but acks directory not found: " << acks_dir << "\n";
            return 1;
        }
        if (k_required <= 0) {
            std::cerr << "ACK mode enabled but --num-parties (required coverage size) not provided.\n";
            return 1;
        }

        const std::set<int> candidate_providers(core_set_ids.begin(), core_set_ids.end());
        if (candidate_providers.empty()) {
            std::cerr << "ACK mode enabled but no candidate providers after provider-input validation.\n";
            return 1;
        }

        std::map<int, std::set<int>> distinct_acks_by_provider_set;
        std::set<std::pair<int, int>> seen_provider_party;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::map<int, ProviderInput> provider_ev_cache;
        std::map<int, ProviderManifestEvidence> manifest_ev_cache;

        auto get_provider_ev = [&](int provider_id) -> std::optional<ProviderInput> {
            if (provider_ev_cache.count(provider_id)) return provider_ev_cache[provider_id];
            const fs::path provider_file = inputs_dir / ("provider_" + std::to_string(provider_id) + ".txt");
            const auto parsed = parse_provider_file(provider_file);
            if (!parsed) return std::nullopt;
            if (parsed->id != provider_id) return std::nullopt;
            provider_ev_cache[provider_id] = *parsed;
            return provider_ev_cache[provider_id];
        };

        auto get_manifest_ev = [&](int provider_id) -> std::optional<ProviderManifestEvidence> {
            if (manifest_ev_cache.count(provider_id)) return manifest_ev_cache[provider_id];
            const fs::path manifest_path = inputs_dir / ("provider_" + std::to_string(provider_id) + "_manifest.json");
            const auto parsed = parse_provider_manifest(manifest_path, k_required);
            if (!parsed) return std::nullopt;

            // Defense against manifest tampering: recompute share_manifest_id
            // from provider input evidence (public fields only).
            const auto provider_opt = get_provider_ev(provider_id);
            if (!provider_opt) return std::nullopt;
            const std::string expected_smid =
                compute_share_manifest_id_from_provider_input(*provider_opt, parsed->num_parties);
            if (expected_smid != parsed->share_manifest_id) return std::nullopt;

            manifest_ev_cache[provider_id] = *parsed;
            return manifest_ev_cache[provider_id];
        };

        const std::regex ack_filename_regex(R"(ack_.*\.json)");
        for (const auto& entry : fs::directory_iterator(acks_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            if (!std::regex_match(filename, ack_filename_regex)) continue;

            const auto ack_opt = parse_ack_file(entry.path());
            if (!ack_opt) continue;
            const AckEvidence& ack = *ack_opt;

            if (ack.session_id != session_id || ack.round_id != round_id) continue;
            if (!ack.is_legacy_ack &&
                (ack.protocol_version != protocol_version || ack.schema_id != schema_id)) continue;
            if (!candidate_providers.count(ack.provider_id)) continue;
            if (ack.party_index < 0 || ack.party_index >= k_required) continue;

            const auto provider_ev_opt = get_provider_ev(ack.provider_id);
            if (!provider_ev_opt) {
                rejected_reasons[ack.provider_id] = "missing_provider_evidence";
                continue;
            }

            if (!ack.is_legacy_ack) {
                if (ack.nonce != provider_ev_opt->nonce) {
                    rejected_reasons[ack.provider_id] = "ack_nonce_mismatch";
                    continue;
                }
            } else {
                // Legacy ACKs sign the raw provider file hash.
                const fs::path provider_path = inputs_dir / ("provider_" + std::to_string(ack.provider_id) + ".txt");
                const auto expected_hash_opt = hash_file_blake2b_hex(provider_path);
                if (!expected_hash_opt || *expected_hash_opt != ack.input_hash) {
                    rejected_reasons[ack.provider_id] = "ack_input_hash_mismatch";
                    continue;
                }
            }

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

            if (!ack.is_legacy_ack) {
                const auto manifest_ev_opt = get_manifest_ev(ack.provider_id);
                if (!manifest_ev_opt) {
                    rejected_reasons[ack.provider_id] = "missing_or_invalid_provider_manifest";
                    continue;
                }

                if (ack.share_manifest_id != manifest_ev_opt->share_manifest_id) {
                    rejected_reasons[ack.provider_id] = "ack_share_manifest_id_mismatch";
                    continue;
                }
                if (ack.mask_commitment_leaves_digest != manifest_ev_opt->mask_commitment_leaves_digest) {
                    rejected_reasons[ack.provider_id] = "ack_mask_commitment_digest_mismatch";
                    continue;
                }
                if (ack.share_file_digest !=
                    manifest_ev_opt->share_file_digests_by_party[static_cast<size_t>(ack.party_index)]) {
                    rejected_reasons[ack.provider_id] = "ack_share_file_digest_mismatch";
                    continue;
                }
                if (ack.share_commitment !=
                    manifest_ev_opt->share_commitments_by_party[static_cast<size_t>(ack.party_index)]) {
                    rejected_reasons[ack.provider_id] = "ack_share_commitment_mismatch";
                    continue;
                }
                if (ack.share_commitment_r !=
                    manifest_ev_opt->share_commitment_rs_by_party[static_cast<size_t>(ack.party_index)]) {
                    rejected_reasons[ack.provider_id] = "ack_share_commitment_r_mismatch";
                    continue;
                }
            }

            const std::pair<int, int> replay_key{ack.provider_id, ack.party_index};
            if (seen_provider_party.count(replay_key) != 0) {
                rejected_reasons[ack.provider_id] = "ack_replay_detected";
                continue;
            }
            seen_provider_party.insert(replay_key);
            distinct_acks_by_provider_set[ack.provider_id].insert(ack.party_index);
            evidence_files_by_provider[ack.provider_id].push_back(entry.path().filename().string());
        }

        for (const auto& [provider_id, party_ids] : distinct_acks_by_provider_set) {
            distinct_acks_by_provider[provider_id] = static_cast<int>(party_ids.size());
        }

        std::vector<int> ack_selected;
        for (int provider_id : core_set_ids) {
            const auto it = distinct_acks_by_provider_set.find(provider_id);
            if (it == distinct_acks_by_provider_set.end()) {
                if (rejected_reasons.count(provider_id) == 0) rejected_reasons[provider_id] = "missing_any_ack";
                continue;
            }
            const auto& covered_parties = it->second;
            bool coverage_ok = true;
            std::string missing_parties;
            for (int p = 0; p < k_required; ++p) {
                if (covered_parties.count(p) == 0) {
                    coverage_ok = false;
                    if (!missing_parties.empty()) missing_parties += ",";
                    missing_parties += std::to_string(p);
                }
            }
            if (coverage_ok) ack_selected.push_back(provider_id);
            else if (rejected_reasons.count(provider_id) == 0) rejected_reasons[provider_id] = "missing_party_indices:" + missing_parties;
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
