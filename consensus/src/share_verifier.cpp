#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <sodium.h>

namespace fs = std::filesystem;

static std::string to_hex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

static std::optional<std::vector<unsigned char>> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

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

static std::string trim_ascii_ws(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

static std::optional<std::string> read_text_file_one_line(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    return line;
}

static std::optional<std::string> read_text_file_all(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

struct ProviderEvidence {
    std::string provider_id_str;  // from provider file "id=<...>"
    std::string masked_value;
    std::string nonce;
};

static std::optional<ProviderEvidence> parse_provider_file(const fs::path& provider_path) {
    std::ifstream in(provider_path);
    if (!in.is_open()) return std::nullopt;
    std::string line1, line2, line3, line4, extra;
    if (!std::getline(in, line1) || !std::getline(in, line2) ||
        !std::getline(in, line3) || !std::getline(in, line4)) {
        return std::nullopt;
    }
    if (std::getline(in, extra)) return std::nullopt; // reject extra lines

    if (line1.rfind("id=", 0) != 0) return std::nullopt;
    if (line2.rfind("masked_value=", 0) != 0) return std::nullopt;
    if (line3.rfind("nonce=", 0) != 0) return std::nullopt;

    ProviderEvidence ev;
    ev.provider_id_str = line1.substr(std::string("id=").size());
    ev.masked_value = line2.substr(std::string("masked_value=").size());
    ev.nonce = line3.substr(std::string("nonce=").size());
    return ev;
}

static std::optional<std::string> parse_json_string_field(const std::string& json,
                                                          const std::string& field) {
    const std::regex re("\"" + field + "\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return std::nullopt;
    return m[1].str();
}

static std::optional<long long> parse_json_int_field(const std::string& json,
                                                      const std::string& field) {
    const std::regex re("\"" + field + R"("\s*:\s*(-?\d+))");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return std::nullopt;
    try {
        return std::stoll(m[1].str());
    } catch (...) {
        return std::nullopt;
    }
}

struct ManifestEvidence {
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;
    int num_parties = -1;
    std::string expected_share_file_digest;
    std::string expected_share_leaf_digest;
    std::string expected_share_commitment;
    std::string expected_share_commitment_r;
};

static std::optional<ManifestEvidence> parse_manifest_for_party_index(
    const std::string& manifest_json,
    int party_index) {
    ManifestEvidence me;
    const auto smid = parse_json_string_field(manifest_json, "share_manifest_id");
    const auto mask_digest = parse_json_string_field(manifest_json, "mask_commitment_leaves_digest");
    const auto n_opt = parse_json_int_field(manifest_json, "num_parties");
    if (!smid || !mask_digest || !n_opt) return std::nullopt;
    me.share_manifest_id = *smid;
    me.mask_commitment_leaves_digest = *mask_digest;
    me.num_parties = static_cast<int>(*n_opt);

    // Extract the party object containing this party_index.
    // Note: this assumes the manifest is generated by this repo with stable formatting.
    // Avoid raw-string delimiters that make it easy to accidentally introduce
    // an unmatched '(' into the regex.
    const std::string re_str =
        "\"party_index\"\\s*:\\s*" + std::to_string(party_index) +
        "\\s*,\\s*\"share_file_digest\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
        "\\s*,\\s*\"share_leaf_digest\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
        "\\s*,\\s*\"share_commitment\"\\s*:\\s*\"([a-fA-F0-9]{64})\""
        "\\s*,\\s*\"share_commitment_r\"\\s*:\\s*\"([a-fA-F0-9]{64})\"";
    const std::regex re(re_str);
    std::smatch m;
    if (!std::regex_search(manifest_json, m, re)) return std::nullopt;
    me.expected_share_file_digest = m[1].str();
    me.expected_share_leaf_digest = m[2].str();
    me.expected_share_commitment = m[3].str();
    me.expected_share_commitment_r = m[4].str();
    return me;
}

static std::optional<uint64_t> parse_share_file_u64(const fs::path& share_path) {
    auto content_opt = read_text_file_all(share_path);
    if (!content_opt) return std::nullopt;
    const std::string trimmed = trim_ascii_ws(*content_opt);
    if (trimmed.empty()) return std::nullopt;
    try {
        unsigned long long v = std::stoull(trimmed, nullptr, 10);
        return static_cast<uint64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

static std::string ack_signing_message(const std::string& protocol_version,
                                         int round_id,
                                         const std::string& schema_id,
                                         int provider_id,
                                         const std::string& nonce,
                                         int party_index,
                                         const std::string& share_manifest_id,
                                         const std::string& mask_commitment_leaves_digest,
                                         const std::string& share_file_digest,
                                         const std::string& share_commitment,
                                         const std::string& share_commitment_r,
                                         long long timestamp_unix_ms) {
    // Keep this string stable; signatures bind exactly to it.
    return protocol_version + "|" +
           std::to_string(round_id) + "|" +
           schema_id + "|" +
           std::to_string(provider_id) + "|" +
           nonce + "|" +
           std::to_string(party_index) + "|" +
           share_manifest_id + "|" +
           mask_commitment_leaves_digest + "|" +
           share_file_digest + "|" +
           share_commitment + "|" +
           share_commitment_r + "|" +
           std::to_string(timestamp_unix_ms);
}

static std::optional<std::string> sign_with_cn_secret_key_hex(const fs::path& cn_secret_key_file,
                                                                const std::string& message) {
    const auto sk_hex_opt = read_text_file_all(cn_secret_key_file);
    if (!sk_hex_opt) return std::nullopt;
    const std::string sk_hex = trim_ascii_ws(*sk_hex_opt);
    const auto sk_bytes_opt = from_hex(sk_hex);
    if (!sk_bytes_opt) return std::nullopt;
    if (sk_bytes_opt->size() != crypto_sign_SECRETKEYBYTES) return std::nullopt;

    unsigned char sig[crypto_sign_BYTES] = {0};
    if (crypto_sign_detached(sig, nullptr,
                              reinterpret_cast<const unsigned char*>(message.data()),
                              message.size(),
                              sk_bytes_opt->data()) != 0) {
        return std::nullopt;
    }
    return to_hex(sig, sizeof(sig));
}

static void write_json_ack(const fs::path& out_path,
                             const std::string& session_id,
                             const std::string& protocol_version,
                             int round_id,
                             const std::string& schema_id,
                             int provider_id,
                             const std::string& nonce,
                             int party_index,
                             const std::string& share_manifest_id,
                             const std::string& mask_commitment_leaves_digest,
                             const std::string& share_file_digest,
                             const std::string& share_commitment,
                             const std::string& share_commitment_r,
                             long long timestamp_unix_ms,
                             const std::string& signature_hex) {
    std::ofstream out(out_path, std::ios::trunc);
    if (!out.is_open()) return;
    out << "{\n";
    out << "  \"session_id\": \"" << session_id << "\",\n";
    out << "  \"protocol_version\": \"" << protocol_version << "\",\n";
    out << "  \"round_id\": " << round_id << ",\n";
    out << "  \"schema_id\": \"" << schema_id << "\",\n";
    out << "  \"provider_id\": " << provider_id << ",\n";
    out << "  \"nonce\": \"" << nonce << "\",\n";
    out << "  \"party_index\": " << party_index << ",\n";
    out << "  \"share_manifest_id\": \"" << share_manifest_id << "\",\n";
    out << "  \"mask_commitment_leaves_digest\": \"" << mask_commitment_leaves_digest << "\",\n";
    out << "  \"share_file_digest\": \"" << share_file_digest << "\",\n";
    out << "  \"share_commitment\": \"" << share_commitment << "\",\n";
    out << "  \"share_commitment_r\": \"" << share_commitment_r << "\",\n";
    out << "  \"timestamp_unix_ms\": " << timestamp_unix_ms << ",\n";
    out << "  \"signature\": \"" << signature_hex << "\"\n";
    out << "}\n";
}

static void scalar_from_u64(unsigned char out_scalar[crypto_core_ristretto255_SCALARBYTES],
                            uint64_t v) {
    std::memset(out_scalar, 0, crypto_core_ristretto255_SCALARBYTES);
    for (int i = 0; i < 8; ++i) {
        out_scalar[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
    }
}

static void point_from_domain(unsigned char out_point[crypto_core_ristretto255_BYTES],
                              const std::string& domain) {
    unsigned char h[64] = {0};
    crypto_generichash(h, sizeof(h),
                       reinterpret_cast<const unsigned char*>(domain.data()),
                       domain.size(),
                       nullptr, 0);
    crypto_core_ristretto255_from_hash(out_point, h);
}

static bool pedersen_commit_u64_with_r(
    unsigned char out_point[crypto_core_ristretto255_BYTES],
    uint64_t v,
    const unsigned char r_scalar[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char G[crypto_core_ristretto255_BYTES],
    const unsigned char H[crypto_core_ristretto255_BYTES]) {
    unsigned char v_scalar[crypto_core_ristretto255_SCALARBYTES];
    scalar_from_u64(v_scalar, v);
    unsigned char a[crypto_core_ristretto255_BYTES];
    unsigned char b[crypto_core_ristretto255_BYTES];
    if (crypto_scalarmult_ristretto255(a, v_scalar, G) != 0) return false;
    if (crypto_scalarmult_ristretto255(b, r_scalar, H) != 0) return false;
    crypto_core_ristretto255_add(out_point, a, b);
    return true;
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium\n";
        return 1;
    }

    // Required args (kept verbose for clarity/stability):
    //  --session-id <...>
    //  --round-id <int>
    //  --protocol-version <str>
    //  --schema-id <str>
    //  --provider-id <int>
    //  --party-index <int>
    //  --inputs-dir <path>
    //  --provider-secrets-dir <path>
    //  --share-manifest-path <path>
    //  --cn-keys-dir <path>
    //  --acks-out-dir <path>
    // Optional:
    //  --timestamp-unix-ms <int> (for deterministic tests)

    std::string session_id = "demo-session";
    int round_id = 0;
    std::string protocol_version = "1";
    std::string schema_id = "semi2k-wire-v1";
    int provider_id = -1;
    int party_index = -1;
    fs::path inputs_dir = fs::current_path() / "inputs";
    fs::path provider_secrets_dir = fs::current_path() / "provider_secrets";
    fs::path share_manifest_path;
    fs::path cn_keys_dir = fs::current_path() / "artifacts" / "cn_keys";
    fs::path acks_out_dir = fs::current_path() / "artifacts" / "acks";
    std::optional<long long> timestamp_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after " << flag << "\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--session-id") {
            auto v = need_value(arg);
            if (!v) return 1;
            session_id = *v;
            continue;
        }
        if (arg == "--round-id") {
            auto v = need_value(arg);
            if (!v) return 1;
            round_id = std::stoi(*v);
            continue;
        }
        if (arg == "--protocol-version") {
            auto v = need_value(arg);
            if (!v) return 1;
            protocol_version = *v;
            continue;
        }
        if (arg == "--schema-id") {
            auto v = need_value(arg);
            if (!v) return 1;
            schema_id = *v;
            continue;
        }
        if (arg == "--provider-id") {
            auto v = need_value(arg);
            if (!v) return 1;
            provider_id = std::stoi(*v);
            continue;
        }
        if (arg == "--party-index") {
            auto v = need_value(arg);
            if (!v) return 1;
            party_index = std::stoi(*v);
            continue;
        }
        if (arg == "--inputs-dir") {
            auto v = need_value(arg);
            if (!v) return 1;
            inputs_dir = *v;
            continue;
        }
        if (arg == "--provider-secrets-dir") {
            auto v = need_value(arg);
            if (!v) return 1;
            provider_secrets_dir = *v;
            continue;
        }
        if (arg == "--share-manifest-path") {
            auto v = need_value(arg);
            if (!v) return 1;
            share_manifest_path = *v;
            continue;
        }
        if (arg == "--cn-keys-dir") {
            auto v = need_value(arg);
            if (!v) return 1;
            cn_keys_dir = *v;
            continue;
        }
        if (arg == "--acks-out-dir") {
            auto v = need_value(arg);
            if (!v) return 1;
            acks_out_dir = *v;
            continue;
        }
        if (arg == "--timestamp-unix-ms") {
            auto v = need_value(arg);
            if (!v) return 1;
            timestamp_override = std::stoll(*v);
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return 1;
    }

    if (provider_id <= 0 || party_index < 0 || share_manifest_path.empty()) {
        std::cerr << "Missing required args: provider-id/party-index/share-manifest-path\n";
        return 1;
    }

    if (!fs::exists(share_manifest_path)) {
        std::cerr << "Missing share manifest: " << share_manifest_path << "\n";
        return 1;
    }

    const fs::path provider_file = inputs_dir / ("provider_" + std::to_string(provider_id) + ".txt");
    const auto provider_ev_opt = parse_provider_file(provider_file);
    if (!provider_ev_opt) {
        std::cerr << "Missing/malformed provider evidence: " << provider_file << "\n";
        return 1;
    }
    const ProviderEvidence provider_ev = *provider_ev_opt;

    const auto manifest_json_opt = read_text_file_all(share_manifest_path);
    if (!manifest_json_opt) {
        std::cerr << "Cannot read share manifest: " << share_manifest_path << "\n";
        return 1;
    }

    const auto manifest_ev_opt = parse_manifest_for_party_index(*manifest_json_opt, party_index);
    if (!manifest_ev_opt) {
        std::cerr << "Manifest missing evidence for party " << party_index << "\n";
        return 1;
    }
    const ManifestEvidence manifest_ev = *manifest_ev_opt;

    // Compute canonical expected share_manifest_id from provider evidence and party coverage size.
    // This is public and consensus can recompute the same way.
    const std::string manifest_input =
        "provider_id=" + provider_ev.provider_id_str +
        ";nonce=" + provider_ev.nonce +
        ";masked_value=" + provider_ev.masked_value +
        ";num_parties=" + std::to_string(manifest_ev.num_parties);
    const std::string expected_share_manifest_id = blake2b_hex32_unkeyed(manifest_input);
    if (expected_share_manifest_id != manifest_ev.share_manifest_id) {
        std::cerr << "Manifest share_manifest_id mismatch (evidence tampering?)\n";
        return 1;
    }

    // Load ONLY the local share file for this party_index.
    const fs::path share_path =
        provider_secrets_dir /
        ("provider_" + std::to_string(provider_id) +
         "_share_" + std::to_string(party_index) + ".secret");
    const auto share_u64_opt = parse_share_file_u64(share_path);
    if (!share_u64_opt) {
        std::cerr << "Missing/local share file unreadable: " << share_path << "\n";
        return 1;
    }
    const uint64_t share_u64 = *share_u64_opt;
    const std::string share_value_dec = std::to_string(share_u64);

    const std::string computed_share_file_digest =
        blake2b_hex32_unkeyed("share_file_value=" + share_value_dec);

    const std::string leaf_digest_input =
        "provider_id=" + provider_ev.provider_id_str +
        ";nonce=" + provider_ev.nonce +
        ";masked_value=" + provider_ev.masked_value +
        ";party_index=" + std::to_string(party_index) +
        ";share_value=" + share_value_dec;
    const std::string computed_share_leaf_digest =
        blake2b_hex32_unkeyed(leaf_digest_input);

    if (computed_share_file_digest != manifest_ev.expected_share_file_digest) {
        std::cerr << "share_file_digest mismatch: local share tampering\n";
        return 1;
    }
    if (computed_share_leaf_digest != manifest_ev.expected_share_leaf_digest) {
        std::cerr << "share_leaf_digest mismatch: local share / masked payload mismatch\n";
        return 1;
    }

    // Commitment-level binding:
    // manifest provides (share_commitment, share_commitment_r) and we recompute
    // the Pedersen commitment point from (local share_u64, share_commitment_r).
    const auto share_commitment_bytes_opt = from_hex(manifest_ev.expected_share_commitment);
    const auto share_commitment_r_bytes_opt = from_hex(manifest_ev.expected_share_commitment_r);
    if (!share_commitment_bytes_opt || !share_commitment_r_bytes_opt) {
        std::cerr << "Invalid hex for share_commitment/share_commitment_r in manifest\n";
        return 1;
    }
    if (share_commitment_bytes_opt->size() != crypto_core_ristretto255_BYTES ||
        share_commitment_r_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES) {
        std::cerr << "Invalid size for share_commitment/share_commitment_r in manifest\n";
        return 1;
    }

    unsigned char Gp[crypto_core_ristretto255_BYTES];
    unsigned char Hp[crypto_core_ristretto255_BYTES];
    point_from_domain(Gp, "mpc:pedersen:G:v1");
    point_from_domain(Hp, "mpc:pedersen:H:v1");

    unsigned char r_scalar[crypto_core_ristretto255_SCALARBYTES] = {0};
    std::memcpy(r_scalar,
                share_commitment_r_bytes_opt->data(),
                crypto_core_ristretto255_SCALARBYTES);

    unsigned char C_recomputed[crypto_core_ristretto255_BYTES];
    if (!pedersen_commit_u64_with_r(C_recomputed, share_u64, r_scalar, Gp, Hp)) {
        std::cerr << "Failed to recompute share Pedersen commitment\n";
        return 1;
    }
    if (std::memcmp(C_recomputed,
                     share_commitment_bytes_opt->data(),
                     crypto_core_ristretto255_BYTES) != 0) {
        std::cerr << "share_commitment mismatch: manifest commitments not consistent with local share\n";
        return 1;
    }

    long long timestamp_ms = 0;
    if (timestamp_override) {
        timestamp_ms = *timestamp_override;
    } else {
        timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    }

    const std::string message = ack_signing_message(
        protocol_version, round_id, schema_id, provider_id, provider_ev.nonce,
        party_index, manifest_ev.share_manifest_id,
        manifest_ev.mask_commitment_leaves_digest, computed_share_file_digest,
        manifest_ev.expected_share_commitment, manifest_ev.expected_share_commitment_r,
        timestamp_ms);

    const fs::path cn_secret_file = cn_keys_dir / ("cn_" + std::to_string(party_index) + ".sec.hex");
    const auto signature_opt = sign_with_cn_secret_key_hex(cn_secret_file, message);
    if (!signature_opt) {
        std::cerr << "Failed signing ACK (missing key?)\n";
        return 1;
    }

    fs::create_directories(acks_out_dir);
    const fs::path out_path =
        acks_out_dir / ("ack_p" + std::to_string(provider_id) + "_party" + std::to_string(party_index) + ".json");
    write_json_ack(out_path,
                   session_id, protocol_version, round_id, schema_id,
                   provider_id, provider_ev.nonce, party_index,
                   manifest_ev.share_manifest_id,
                   manifest_ev.mask_commitment_leaves_digest,
                   computed_share_file_digest,
                   manifest_ev.expected_share_commitment,
                   manifest_ev.expected_share_commitment_r,
                   timestamp_ms,
                   *signature_opt);

    return 0;
}

