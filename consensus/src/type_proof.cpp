#include "type_proof.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <cstring>
#include <iostream>
#include <regex>
#include <sodium.h>
#include <stdexcept>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace type_proof {

// ---------------------------------------------------------------------------
// Local helpers (kept in this module to avoid cross-file coupling)
// ---------------------------------------------------------------------------

static std::optional<std::string> read_text_file_all(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
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

static std::string compute_masked_wire_digest(const ProviderEvidenceView& provider) {
    return blake2b_hex32_unkeyed(provider.masked_wire);
}

static std::string compute_expected_type_proof_blob(const ProviderEvidenceView& provider) {
    return blake2b_hex32_unkeyed(
        "type_proof_blob:provider_id=" + std::to_string(provider.provider_id) +
        ";nonce=" + provider.nonce +
        ";masked_wire=" + provider.masked_wire);
}

static std::string compute_expected_type_proof_id(const ProviderEvidenceView& provider,
                                                  const std::string& expected_schema_id,
                                                  const Evidence& tp) {
    return blake2b_hex32_unkeyed(
        "provider_id=" + std::to_string(provider.provider_id) +
        ";nonce=" + provider.nonce +
        ";schema_id=" + expected_schema_id +
        ";type_proof_system=" + tp.type_proof_system +
        ";type_proof_vk_id=" + tp.type_proof_vk_id +
        ";type_proof_blob=" + tp.type_proof_blob);
}

static std::optional<std::vector<unsigned char>> from_hex(const std::string& hex) {
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

static void hash_to_scalar(unsigned char out_scalar[crypto_core_ristretto255_SCALARBYTES],
                            const std::string& msg) {
    unsigned char h[64] = {0};
    crypto_generichash(h, sizeof(h),
                       reinterpret_cast<const unsigned char*>(msg.data()),
                       msg.size(),
                       nullptr, 0);
    crypto_core_ristretto255_scalar_reduce(out_scalar, h);
}

static std::optional<uint64_t> parse_signed_decimal_mod2_64(const std::string& s) {
    // Parse arbitrary-length signed decimal string and return value mod 2^64.
    bool neg = false;
    size_t i = 0;
    if (!s.empty() && s[0] == '-') { neg = true; i = 1; }
    uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        __uint128_t tmp = static_cast<__uint128_t>(acc) * 10 + d;
        acc = static_cast<uint64_t>(tmp); // mod 2^64 by truncation
    }
    if (!neg) return acc;
    return static_cast<uint64_t>(0) - acc;
}

static bool scalar_is_all_zero(const unsigned char s[crypto_core_ristretto255_SCALARBYTES]) {
    for (size_t i = 0; i < crypto_core_ristretto255_SCALARBYTES; ++i) {
        if (s[i] != 0) return false;
    }
    return true;
}

// OR-proof verification (same construction as prove_commitment_one_of_two in data_provider.cpp).
// The prover outputs the full Fiat–Shamir challenge e alongside the second split component e1
// (so e = e0 + e1 in the scalar field with e0 the first branch challenge). Given e and e1,
// e0 = e - e1 is unique; we reconstruct a0,a1 for each layout and check hash(domain|C|a0|a1) == e.
// A prior fixed-point-only verifier could fail to converge even for honestly generated proofs
// because the map e0 ↦ H(·) − e1 is not contractive from typical seeds.
static bool or_proof_verify_layout(const unsigned char C[crypto_core_ristretto255_BYTES],
                                   const unsigned char e_full[crypto_core_ristretto255_SCALARBYTES],
                                   const unsigned char e1_proof[crypto_core_ristretto255_SCALARBYTES],
                                   const unsigned char s0[crypto_core_ristretto255_SCALARBYTES],
                                   const unsigned char s1[crypto_core_ristretto255_SCALARBYTES],
                                   const std::string& domain,
                                   const unsigned char m0_scalar[crypto_core_ristretto255_SCALARBYTES],
                                   const unsigned char m1_scalar[crypto_core_ristretto255_SCALARBYTES],
                                   const unsigned char G[crypto_core_ristretto255_BYTES],
                                   const unsigned char H[crypto_core_ristretto255_BYTES],
                                   bool layout_is_m0) {
    unsigned char e0_proof[crypto_core_ristretto255_SCALARBYTES];
    crypto_core_ristretto255_scalar_sub(e0_proof, e_full, e1_proof);

    unsigned char m0G[crypto_core_ristretto255_BYTES];
    unsigned char m1G[crypto_core_ristretto255_BYTES];
    // 0*G is the identity point (32 zero bytes) on Ristretto255; some libsodium builds reject n=0 in scalarmult.
    if (scalar_is_all_zero(m0_scalar)) {
        std::memset(m0G, 0, crypto_core_ristretto255_BYTES);
    } else if (crypto_scalarmult_ristretto255(m0G, m0_scalar, G) != 0) {
        return false;
    }
    if (crypto_scalarmult_ristretto255(m1G, m1_scalar, G) != 0) return false;

    unsigned char C_minus_m0G[crypto_core_ristretto255_BYTES];
    unsigned char C_minus_m1G[crypto_core_ristretto255_BYTES];
    crypto_core_ristretto255_sub(C_minus_m0G, C, m0G);
    crypto_core_ristretto255_sub(C_minus_m1G, C, m1G);

    unsigned char s0H[crypto_core_ristretto255_BYTES];
    unsigned char s1H[crypto_core_ristretto255_BYTES];
    if (crypto_scalarmult_ristretto255(s0H, s0, H) != 0) return false;
    if (crypto_scalarmult_ristretto255(s1H, s1, H) != 0) return false;

    unsigned char a0[crypto_core_ristretto255_BYTES];
    unsigned char a1[crypto_core_ristretto255_BYTES];

    if (layout_is_m0) {
        unsigned char e0_Cminus[crypto_core_ristretto255_BYTES];
        if (crypto_scalarmult_ristretto255(e0_Cminus, e0_proof, C_minus_m0G) != 0) return false;
        crypto_core_ristretto255_sub(a0, s0H, e0_Cminus);

        unsigned char e1_Cminus[crypto_core_ristretto255_BYTES];
        if (crypto_scalarmult_ristretto255(e1_Cminus, e1_proof, C_minus_m1G) != 0) return false;
        crypto_core_ristretto255_add(a1, s1H, e1_Cminus);
    } else {
        unsigned char e0_Cminus[crypto_core_ristretto255_BYTES];
        if (crypto_scalarmult_ristretto255(e0_Cminus, e0_proof, C_minus_m0G) != 0) return false;
        crypto_core_ristretto255_add(a0, s0H, e0_Cminus);

        unsigned char e1_Cminus[crypto_core_ristretto255_BYTES];
        if (crypto_scalarmult_ristretto255(e1_Cminus, e1_proof, C_minus_m1G) != 0) return false;
        crypto_core_ristretto255_sub(a1, s1H, e1_Cminus);
    }

    const std::string transcript =
        domain + "|" + to_hex(C, crypto_core_ristretto255_BYTES) + "|" +
        to_hex(a0, crypto_core_ristretto255_BYTES) + "|" +
        to_hex(a1, crypto_core_ristretto255_BYTES);

    unsigned char e_candidate[crypto_core_ristretto255_SCALARBYTES];
    hash_to_scalar(e_candidate, transcript);
    // Compare as field elements (canonical 32-byte encodings can differ for the same scalar mod L
    // if one side came from hex decode without reduction).
    unsigned char diff[crypto_core_ristretto255_SCALARBYTES];
    crypto_core_ristretto255_scalar_sub(diff, e_candidate, e_full);
    static const unsigned char zero_sc[crypto_core_ristretto255_SCALARBYTES] = {0};
    return sodium_memcmp(diff, zero_sc, crypto_core_ristretto255_SCALARBYTES) == 0;
}

static std::optional<bool> verify_or_proof_open_one_of_two(
    const unsigned char C[crypto_core_ristretto255_BYTES],
    const unsigned char e_full[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char e1[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char s0[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char s1[crypto_core_ristretto255_SCALARBYTES],
    const std::string& domain,
    const unsigned char m0_scalar[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char m1_scalar[crypto_core_ristretto255_SCALARBYTES],
    const unsigned char G[crypto_core_ristretto255_BYTES],
    const unsigned char H[crypto_core_ristretto255_BYTES]) {
    const bool ok_m0 =
        or_proof_verify_layout(C, e_full, e1, s0, s1, domain, m0_scalar, m1_scalar, G, H, true);
    const bool ok_m1 =
        or_proof_verify_layout(C, e_full, e1, s0, s1, domain, m0_scalar, m1_scalar, G, H, false);

    if (ok_m0 && !ok_m1) return true;
    if (!ok_m0 && ok_m1) return false;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Backend interface + implementations
// ---------------------------------------------------------------------------

class VerifierBackend {
public:
    virtual ~VerifierBackend() = default;
    virtual bool verify(const ProviderEvidenceView& provider,
                        const ManifestMinimalView& manifest,
                        const Evidence& tp,
                        const std::string& expected_schema_id) const = 0;
};

class StubBackend final : public VerifierBackend {
public:
    bool verify(const ProviderEvidenceView& provider,
                const ManifestMinimalView& /*manifest*/,
                const Evidence& tp,
                const std::string& expected_schema_id) const override {
        if (tp.type_proof_system != "stub-typeproof-v1") return false;
        if (tp.type_proof_vk_id != "stub-vk-v1") return false;
        if (tp.schema_id != expected_schema_id) return false;

        const std::string expected_blob = compute_expected_type_proof_blob(provider);
        if (tp.type_proof_blob != expected_blob) return false;

        const std::string expected_id =
            compute_expected_type_proof_id(provider, expected_schema_id, tp);
        if (tp.type_proof_id != expected_id) return false;

        return true;
    }
};

// Base64 decode helper (libsodium).
static std::optional<std::string> base64_decode(const std::string& b64) {
    if (b64.empty()) return std::nullopt;
    std::vector<unsigned char> bin(b64.size()); // upper bound
    size_t bin_len = 0;
    if (sodium_base642bin(bin.data(), bin.size(),
                          b64.c_str(), b64.size(),
                          nullptr, &bin_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(bin.data()), bin_len);
}

static std::optional<std::string> base64_encode(const std::string& s) {
    if (s.empty()) return std::string();
    const size_t max_len = sodium_base64_ENCODED_LEN(s.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(max_len, '\0');
    sodium_bin2base64(out.data(), out.size(),
                      reinterpret_cast<const unsigned char*>(s.data()), s.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    // libsodium NUL-terminates; trim.
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser for type registry + type_proof_blob
// (enough for null/bool/int/string/array/object)
// ---------------------------------------------------------------------------

struct Json;
using JsonObject = std::unordered_map<std::string, Json>;
using JsonArray = std::vector<Json>;

struct Json {
    using Value = std::variant<std::nullptr_t, bool, long long, std::string, JsonArray, JsonObject>;
    Value v;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_int() const { return std::holds_alternative<long long>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_array() const { return std::holds_alternative<JsonArray>(v); }
    bool is_object() const { return std::holds_alternative<JsonObject>(v); }

    const JsonArray* as_array() const { return std::get_if<JsonArray>(&v); }
    const JsonObject* as_object() const { return std::get_if<JsonObject>(&v); }
    const std::string* as_string() const { return std::get_if<std::string>(&v); }
    const long long* as_int() const { return std::get_if<long long>(&v); }
    const bool* as_bool() const { return std::get_if<bool>(&v); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    Json parse() {
        skip_ws();
        Json out = parse_value();
        skip_ws();
        if (pos_ != s_.size()) throw std::runtime_error("trailing_json");
        return out;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { ++pos_; continue; }
            break;
        }
    }

    char peek() const { return (pos_ < s_.size()) ? s_[pos_] : '\0'; }
    char get() { return (pos_ < s_.size()) ? s_[pos_++] : '\0'; }

    void expect(char c) {
        if (get() != c) throw std::runtime_error("expected_char");
    }

    Json parse_value() {
        char c = peek();
        if (c == '{') return Json{parse_object()};
        if (c == '[') return Json{parse_array()};
        if (c == '"') return Json{parse_string()};
        if (c == 't') { consume("true"); return Json{true}; }
        if (c == 'f') { consume("false"); return Json{false}; }
        if (c == 'n') { consume("null"); return Json{nullptr}; }
        if (c == '-' || (c >= '0' && c <= '9')) return Json{parse_int()};
        throw std::runtime_error("invalid_json_value");
    }

    void consume(const char* word) {
        for (size_t i = 0; word[i]; ++i) {
            if (get() != word[i]) throw std::runtime_error("invalid_literal");
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < s_.size()) {
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        // Minimal unicode escape support: accept \u00XX only.
                        if (pos_ + 4 > s_.size()) throw std::runtime_error("bad_unicode_escape");
                        const std::string hex = s_.substr(pos_, 4);
                        pos_ += 4;
                        auto hexval = [](char h) -> int {
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                            return -1;
                        };
                        int v0 = hexval(hex[0]), v1 = hexval(hex[1]), v2 = hexval(hex[2]), v3 = hexval(hex[3]);
                        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) throw std::runtime_error("bad_unicode_escape");
                        int code = (v0 << 12) | (v1 << 8) | (v2 << 4) | v3;
                        if (code <= 0x7F) out.push_back(static_cast<char>(code));
                        else throw std::runtime_error("unsupported_unicode_escape");
                        break;
                    }
                    default:
                        throw std::runtime_error("bad_escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    long long parse_int() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (!(peek() >= '0' && peek() <= '9')) throw std::runtime_error("bad_int");
        while (peek() >= '0' && peek() <= '9') ++pos_;
        const std::string token = s_.substr(start, pos_ - start);
        try {
            size_t idx = 0;
            long long v = std::stoll(token, &idx);
            if (idx != token.size()) throw std::runtime_error("bad_int");
            return v;
        } catch (...) {
            throw std::runtime_error("bad_int");
        }
    }

    JsonArray parse_array() {
        expect('[');
        skip_ws();
        JsonArray arr;
        if (peek() == ']') { get(); return arr; }
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            char c = get();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("bad_array_sep");
        }
        return arr;
    }

    JsonObject parse_object() {
        expect('{');
        skip_ws();
        JsonObject obj;
        if (peek() == '}') { get(); return obj; }
        while (true) {
            skip_ws();
            if (peek() != '"') throw std::runtime_error("bad_object_key");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            char c = get();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("bad_object_sep");
        }
        return obj;
    }
};

static std::optional<Json> parse_json(const std::string& s) {
    try {
        return JsonParser(s).parse();
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Schema representation + verifier
// ---------------------------------------------------------------------------

struct Schema;
using SchemaPtr = std::shared_ptr<Schema>;

struct Schema {
    std::string kind; // int64, bool, fixed_point, vector, tuple, record, ref
    // fixed_point
    int scale = 0;
    std::string encoding; // e.g. unscaled_int64
    // vector
    std::optional<int> length;
    SchemaPtr elem;
    // tuple
    std::vector<SchemaPtr> elems;
    // record
    std::vector<std::pair<std::string, SchemaPtr>> fields; // preserve order
    // ref
    std::string ref_schema_id;
    bool nullable = false;
};

static std::optional<int> get_int_field(const JsonObject& o, const std::string& k) {
    auto it = o.find(k);
    if (it == o.end()) return std::nullopt;
    if (!it->second.is_int()) return std::nullopt;
    return static_cast<int>(*it->second.as_int());
}

static std::optional<std::string> get_string_field(const JsonObject& o, const std::string& k) {
    auto it = o.find(k);
    if (it == o.end()) return std::nullopt;
    if (!it->second.is_string()) return std::nullopt;
    return *it->second.as_string();
}

static const Json* get_field(const JsonObject& o, const std::string& k) {
    auto it = o.find(k);
    if (it == o.end()) return nullptr;
    return &it->second;
}

static std::optional<SchemaPtr> parse_schema_node(const Json& j,
                                                  const JsonObject& registry_schemas,
                                                  std::vector<std::string>& stack);

static std::optional<SchemaPtr> parse_ref_schema(const std::string& schema_id,
                                                 const JsonObject& registry_schemas,
                                                 std::vector<std::string>& stack) {
    // detect recursion cycles (allowed) by stack; we still allow, but prevent infinite expansion.
    for (const auto& s : stack) {
        if (s == schema_id) {
            // Represent recursive ref as ref node; resolution happens at verify time.
            auto sc = std::make_shared<Schema>();
            sc->kind = "ref";
            sc->ref_schema_id = schema_id;
            sc->nullable = false;
            return sc;
        }
    }
    auto it = registry_schemas.find(schema_id);
    if (it == registry_schemas.end()) return std::nullopt;
    stack.push_back(schema_id);
    auto parsed = parse_schema_node(it->second, registry_schemas, stack);
    stack.pop_back();
    return parsed;
}

static std::optional<SchemaPtr> parse_schema_node(const Json& j,
                                                  const JsonObject& registry_schemas,
                                                  std::vector<std::string>& stack) {
    if (!j.is_object()) return std::nullopt;
    const auto* o = j.as_object();
    auto kind_opt = get_string_field(*o, "kind");
    if (!kind_opt) return std::nullopt;

    auto sc = std::make_shared<Schema>();
    sc->kind = *kind_opt;

    if (sc->kind == "int64" || sc->kind == "bool") {
        return sc;
    }
    if (sc->kind == "fixed_point") {
        auto scale_opt = get_int_field(*o, "scale");
        auto enc_opt = get_string_field(*o, "encoding");
        if (!scale_opt || !enc_opt) return std::nullopt;
        sc->scale = *scale_opt;
        sc->encoding = *enc_opt;
        return sc;
    }
    if (sc->kind == "vector") {
        if (const Json* lenj = get_field(*o, "length")) {
            if (!lenj->is_int()) return std::nullopt;
            sc->length = static_cast<int>(*lenj->as_int());
        }
        const Json* elemj = get_field(*o, "elem");
        if (!elemj) return std::nullopt;
        auto elem_sc = parse_schema_node(*elemj, registry_schemas, stack);
        if (!elem_sc) return std::nullopt;
        sc->elem = *elem_sc;
        return sc;
    }
    if (sc->kind == "tuple") {
        const Json* elemsj = get_field(*o, "elems");
        if (!elemsj || !elemsj->is_array()) return std::nullopt;
        for (const auto& ej : *elemsj->as_array()) {
            auto esc = parse_schema_node(ej, registry_schemas, stack);
            if (!esc) return std::nullopt;
            sc->elems.push_back(*esc);
        }
        return sc;
    }
    if (sc->kind == "record") {
        const Json* fieldsj = get_field(*o, "fields");
        if (!fieldsj || !fieldsj->is_object()) return std::nullopt;
        for (const auto& [fname, fnode] : *fieldsj->as_object()) {
            auto fsc = parse_schema_node(fnode, registry_schemas, stack);
            if (!fsc) return std::nullopt;
            sc->fields.push_back({fname, *fsc});
        }
        return sc;
    }
    if (sc->kind == "ref") {
        auto sid_opt = get_string_field(*o, "schema_id");
        if (!sid_opt) return std::nullopt;
        sc->ref_schema_id = *sid_opt;
        if (const Json* nj = get_field(*o, "nullable")) {
            if (!nj->is_bool()) return std::nullopt;
            sc->nullable = *nj->as_bool();
        }
        return sc;
    }
    return std::nullopt;
}

static std::optional<SchemaPtr> load_schema_from_registry(const fs::path& registry_path,
                                                          const std::string& schema_id) {
    const auto content_opt = read_text_file_all(registry_path);
    if (!content_opt) return std::nullopt;
    const auto root_opt = parse_json(*content_opt);
    if (!root_opt || !root_opt->is_object()) return std::nullopt;
    const auto* root = root_opt->as_object();
    const Json* schemasj = get_field(*root, "schemas");
    if (!schemasj || !schemasj->is_object()) return std::nullopt;
    const auto& schemas = *schemasj->as_object();

    auto it = schemas.find(schema_id);
    if (it == schemas.end()) return std::nullopt;
    std::vector<std::string> stack;
    return parse_schema_node(it->second, schemas, stack);
}

static bool verify_value_against_schema(const Json& value,
                                       const SchemaPtr& schema,
                                       const fs::path& registry_path,
                                       int depth,
                                       std::string* out_reason) {
    if (!schema) return false;
    if (depth > 64) { if (out_reason) *out_reason = "semantic_max_depth"; return false; }

    if (schema->kind == "int64") {
        if (!value.is_int()) { if (out_reason) *out_reason = "semantic_expected_int64"; return false; }
        // value already in long long range by parser; treat as int64.
        return true;
    }
    if (schema->kind == "bool") {
        if (!value.is_int()) { if (out_reason) *out_reason = "semantic_expected_bool_int"; return false; }
        const long long v = *value.as_int();
        if (v != 0 && v != 1) { if (out_reason) *out_reason = "semantic_bool_out_of_range"; return false; }
        return true;
    }
    if (schema->kind == "fixed_point") {
        // encoding: unscaled_int64 => JSON must be object {"unscaled": <int64>}
        if (schema->encoding != "unscaled_int64") { if (out_reason) *out_reason = "semantic_fixed_point_unknown_encoding"; return false; }
        if (!value.is_object()) { if (out_reason) *out_reason = "semantic_expected_fixed_point_object"; return false; }
        const auto* o = value.as_object();
        auto it = o->find("unscaled");
        if (it == o->end() || !it->second.is_int()) { if (out_reason) *out_reason = "semantic_fixed_point_missing_unscaled"; return false; }
        (void)schema->scale; // scale used by consumer; semantic check here is structural.
        return true;
    }
    if (schema->kind == "vector") {
        if (!value.is_array()) { if (out_reason) *out_reason = "semantic_expected_vector_array"; return false; }
        const auto* arr = value.as_array();
        if (schema->length && static_cast<int>(arr->size()) != *schema->length) {
            if (out_reason) *out_reason = "semantic_vector_length_mismatch";
            return false;
        }
        for (const auto& el : *arr) {
            if (!verify_value_against_schema(el, schema->elem, registry_path, depth + 1, out_reason)) return false;
        }
        return true;
    }
    if (schema->kind == "tuple") {
        if (!value.is_array()) { if (out_reason) *out_reason = "semantic_expected_tuple_array"; return false; }
        const auto* arr = value.as_array();
        if (arr->size() != schema->elems.size()) { if (out_reason) *out_reason = "semantic_tuple_arity_mismatch"; return false; }
        for (size_t i = 0; i < arr->size(); ++i) {
            if (!verify_value_against_schema((*arr)[i], schema->elems[i], registry_path, depth + 1, out_reason)) return false;
        }
        return true;
    }
    if (schema->kind == "record") {
        if (!value.is_object()) { if (out_reason) *out_reason = "semantic_expected_record_object"; return false; }
        const auto* obj = value.as_object();
        // Require exact field set (no missing, no extra).
        if (obj->size() != schema->fields.size()) { if (out_reason) *out_reason = "semantic_record_field_set_mismatch"; return false; }
        for (const auto& [fname, fsc] : schema->fields) {
            auto it = obj->find(fname);
            if (it == obj->end()) { if (out_reason) *out_reason = "semantic_record_missing_field"; return false; }
            if (!verify_value_against_schema(it->second, fsc, registry_path, depth + 1, out_reason)) return false;
        }
        return true;
    }
    if (schema->kind == "ref") {
        if (schema->nullable && value.is_null()) return true;
        // Resolve referenced schema from registry (lazy).
        auto ref_sc = load_schema_from_registry(registry_path, schema->ref_schema_id);
        if (!ref_sc) { if (out_reason) *out_reason = "semantic_unknown_ref_schema"; return false; }
        return verify_value_against_schema(value, *ref_sc, registry_path, depth + 1, out_reason);
    }

    if (out_reason) *out_reason = "semantic_unknown_schema_kind";
    return false;
}

class SemanticSchemaBackend final : public VerifierBackend {
public:
    bool verify(const ProviderEvidenceView& provider,
                const ManifestMinimalView& manifest,
                const Evidence& tp,
                const std::string& expected_schema_id) const override {
        (void)provider;
        (void)manifest;

        if (tp.type_proof_system != "semantic-schema-v1") return false;
        if (tp.type_proof_vk_id != "type-registry-v1") return false;
        if (tp.schema_id != expected_schema_id) return false;

        // Decode blob: base64(JSON)
        const auto decoded_opt = base64_decode(tp.type_proof_blob);
        if (!decoded_opt) return false;
        const auto blob_json_opt = parse_json(*decoded_opt);
        if (!blob_json_opt || !blob_json_opt->is_object()) return false;
        const auto* blob_obj = blob_json_opt->as_object();
        const Json* value = get_field(*blob_obj, "value");
        if (!value) return false;

        // Load authoritative schema from registry on disk.
        const fs::path registry_path = fs::current_path() / "schemas" / "type_registry.json";
        const auto schema_opt = load_schema_from_registry(registry_path, expected_schema_id);
        if (!schema_opt) return false;

        std::string reason;
        if (!verify_value_against_schema(*value, *schema_opt, registry_path, 0, &reason)) {
            return false;
        }

        // Integrity: type_proof_id must match the binding hash.
        const std::string expected_id = compute_expected_type_proof_id(
            ProviderEvidenceView{tp.provider_id, tp.nonce, ""}, expected_schema_id, tp);
        // ProviderEvidenceView above doesn't carry masked_wire here; compute_expected_type_proof_id doesn't need it.
        if (tp.type_proof_id != expected_id) return false;

        return true;
    }
};

class ProofRealV1Backend final : public VerifierBackend {
public:
    bool verify(const ProviderEvidenceView& provider,
                const ManifestMinimalView& /*manifest*/,
                const Evidence& tp,
                const std::string& expected_schema_id) const override {
        if (tp.type_proof_system != "proof-real-v1") return false;
        if (tp.schema_id != expected_schema_id) return false;

        // Integrity: type_proof_id must match the binding hash.
        const std::string expected_id =
            compute_expected_type_proof_id(provider, expected_schema_id, tp);
        if (tp.type_proof_id != expected_id) return false;

        // proof-real-v1 in this repo only supports scalar kinds that map to a single
        // u64 witness: int64, bool, and fixed_point (with unscaled_int64 encoding).
        // Composite kinds (vector/tuple/record/ref) are rejected in admission.
        const fs::path registry_path = fs::current_path() / "schemas" / "type_registry.json";
        const auto schema_opt = load_schema_from_registry(registry_path, expected_schema_id);
        if (!schema_opt) return false;
        const auto& schema = *schema_opt;
        if (schema->kind != "int64" && schema->kind != "bool" && schema->kind != "fixed_point") {
            return false;
        }
        if (schema->kind == "fixed_point" && schema->encoding != "unscaled_int64") {
            return false;
        }

        // Decode blob: base64(JSON)
        const auto decoded_opt = base64_decode(tp.type_proof_blob);
        if (!decoded_opt) return false;
        const auto blob_json_opt = parse_json(*decoded_opt);
        if (!blob_json_opt || !blob_json_opt->is_object()) return false;
        const auto* blob_obj = blob_json_opt->as_object();

        const auto* proof_system_field = get_field(*blob_obj, "proof_system");
        if (!proof_system_field || !proof_system_field->is_string()) return false;
        if (*proof_system_field->as_string() != "proof-real-v1") return false;

        const auto x_commitment_opt = get_string_field(*blob_obj, "x_commitment");
        const auto carry_commitment_opt = get_string_field(*blob_obj, "carry_commitment");
        const auto x_randomizer_opt = get_string_field(*blob_obj, "x_randomizer");
        if (!x_commitment_opt || !carry_commitment_opt || !x_randomizer_opt) return false;

        auto xC_bytes_opt = from_hex(*x_commitment_opt);
        auto x_r_bytes_opt = from_hex(*x_randomizer_opt);
        auto carryC_bytes_opt = from_hex(*carry_commitment_opt);
        if (!xC_bytes_opt || xC_bytes_opt->size() != crypto_core_ristretto255_BYTES ||
            !x_r_bytes_opt || x_r_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
            !carryC_bytes_opt || carryC_bytes_opt->size() != crypto_core_ristretto255_BYTES) {
            return false;
        }

        unsigned char C_x[crypto_core_ristretto255_BYTES];
        unsigned char r_x[crypto_core_ristretto255_SCALARBYTES];
        unsigned char C_t[crypto_core_ristretto255_BYTES];
        std::memcpy(C_x, xC_bytes_opt->data(), crypto_core_ristretto255_BYTES);
        std::memcpy(r_x, x_r_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
        std::memcpy(C_t, carryC_bytes_opt->data(), crypto_core_ristretto255_BYTES);

        // Load Pedersen generators (must match provider).
        unsigned char Gp[crypto_core_ristretto255_BYTES];
        unsigned char Hp[crypto_core_ristretto255_BYTES];
        point_from_domain(Gp, "mpc:pedersen:G:v1");
        point_from_domain(Hp, "mpc:pedersen:H:v1");

        // Pedersen opening decomposition:
        //   C_x = x*G + r_x*H  =>  x*G = C_x - r_x*H
        unsigned char rH[crypto_core_ristretto255_BYTES];
        unsigned char xG[crypto_core_ristretto255_BYTES];
        crypto_scalarmult_ristretto255(rH, r_x, Hp);
        crypto_core_ristretto255_sub(xG, C_x, rH);

        // -------------------------------------------------------------------
        // carry_or: commitment C_t opens to {0, 2^64}
        // -------------------------------------------------------------------
        const Json* carry_or_j = get_field(*blob_obj, "carry_or");
        if (!carry_or_j || !carry_or_j->is_object()) return false;
        const auto* carry_or_obj = carry_or_j->as_object();

        const auto carry_e_opt = get_string_field(*carry_or_obj, "e");
        const auto carry_e1_opt = get_string_field(*carry_or_obj, "e1");
        const auto carry_s0_opt = get_string_field(*carry_or_obj, "s0");
        const auto carry_s1_opt = get_string_field(*carry_or_obj, "s1");
        if (!carry_e_opt || !carry_e1_opt || !carry_s0_opt || !carry_s1_opt) return false;

        auto e_full_bytes_opt = from_hex(*carry_e_opt);
        auto e1_bytes_opt = from_hex(*carry_e1_opt);
        auto s0_bytes_opt = from_hex(*carry_s0_opt);
        auto s1_bytes_opt = from_hex(*carry_s1_opt);
        if (!e_full_bytes_opt || e_full_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
            !e1_bytes_opt || e1_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
            !s0_bytes_opt || s0_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
            !s1_bytes_opt || s1_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES) {
            return false;
        }

        unsigned char e_full_carry[crypto_core_ristretto255_SCALARBYTES];
        unsigned char e1[crypto_core_ristretto255_SCALARBYTES];
        unsigned char s0[crypto_core_ristretto255_SCALARBYTES];
        unsigned char s1[crypto_core_ristretto255_SCALARBYTES];
        std::memcpy(e_full_carry, e_full_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
        std::memcpy(e1, e1_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
        std::memcpy(s0, s0_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
        std::memcpy(s1, s1_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);

        unsigned char m0_carry[crypto_core_ristretto255_SCALARBYTES];
        unsigned char m1_carry[crypto_core_ristretto255_SCALARBYTES];
        std::memset(m0_carry, 0, crypto_core_ristretto255_SCALARBYTES);
        std::memset(m1_carry, 0, crypto_core_ristretto255_SCALARBYTES);
        // m1_carry = 2^64 (little-endian, byte index 8).
        m1_carry[8] = 1;

        const std::string carry_domain = "proof-real-v1:carry";
        const auto carry_is_m0_opt =
            verify_or_proof_open_one_of_two(C_t, e_full_carry, e1, s0, s1, carry_domain,
                                             m0_carry, m1_carry, Gp, Hp);
        if (!carry_is_m0_opt) {
            std::cerr << "[proof-real-v1] carry_or verification failed\n";
            return false;
        }

        unsigned char carry_scalar[crypto_core_ristretto255_SCALARBYTES];
        if (*carry_is_m0_opt) {
            std::memset(carry_scalar, 0, crypto_core_ristretto255_SCALARBYTES);
        } else {
            std::memcpy(carry_scalar, m1_carry, crypto_core_ristretto255_SCALARBYTES);
        }

        // -------------------------------------------------------------------
        // bool_or (only when schema is bool-v1)
        // -------------------------------------------------------------------
        unsigned char x_scalar[crypto_core_ristretto255_SCALARBYTES];
        if (expected_schema_id == "bool-v1") {
            const Json* bool_or_j = get_field(*blob_obj, "bool_or");
            if (!bool_or_j || !bool_or_j->is_object()) return false;
            const auto* bool_or_obj = bool_or_j->as_object();

            const auto bool_e_opt = get_string_field(*bool_or_obj, "e");
            const auto bool_e1_opt = get_string_field(*bool_or_obj, "e1");
            const auto bool_s0_opt = get_string_field(*bool_or_obj, "s0");
            const auto bool_s1_opt = get_string_field(*bool_or_obj, "s1");
            if (!bool_e_opt || !bool_e1_opt || !bool_s0_opt || !bool_s1_opt) return false;

            auto bool_e_full_bytes_opt = from_hex(*bool_e_opt);
            auto bool_e1_bytes_opt = from_hex(*bool_e1_opt);
            auto bool_s0_bytes_opt = from_hex(*bool_s0_opt);
            auto bool_s1_bytes_opt = from_hex(*bool_s1_opt);
            if (!bool_e_full_bytes_opt || bool_e_full_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
                !bool_e1_bytes_opt || bool_e1_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
                !bool_s0_bytes_opt || bool_s0_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES ||
                !bool_s1_bytes_opt || bool_s1_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES) {
                return false;
            }

            unsigned char x_e_full[crypto_core_ristretto255_SCALARBYTES];
            unsigned char x_e1[crypto_core_ristretto255_SCALARBYTES];
            unsigned char x_s0[crypto_core_ristretto255_SCALARBYTES];
            unsigned char x_s1[crypto_core_ristretto255_SCALARBYTES];
            std::memcpy(x_e_full, bool_e_full_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
            std::memcpy(x_e1, bool_e1_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
            std::memcpy(x_s0, bool_s0_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);
            std::memcpy(x_s1, bool_s1_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);

            unsigned char m0_bool[crypto_core_ristretto255_SCALARBYTES];
            unsigned char m1_bool[crypto_core_ristretto255_SCALARBYTES];
            std::memset(m0_bool, 0, crypto_core_ristretto255_SCALARBYTES);
            std::memset(m1_bool, 0, crypto_core_ristretto255_SCALARBYTES);
            scalar_from_u64(m1_bool, 1ULL);

            const std::string bool_domain = "proof-real-v1:bool";
            const auto x_is_m0_opt =
                verify_or_proof_open_one_of_two(C_x, x_e_full, x_e1, x_s0, x_s1, bool_domain,
                                                 m0_bool, m1_bool, Gp, Hp);
            if (!x_is_m0_opt) {
                std::cerr << "[proof-real-v1] bool_or verification failed\n";
                return false;
            }

            if (*x_is_m0_opt) {
                std::memset(x_scalar, 0, crypto_core_ristretto255_SCALARBYTES);
            } else {
                std::memcpy(x_scalar, m1_bool, crypto_core_ristretto255_SCALARBYTES);
            }
            // Cross-check: xG derived from x_randomizer must equal x_scalar*G.
            unsigned char xG_check[crypto_core_ristretto255_BYTES];
            crypto_scalarmult_ristretto255(xG_check, x_scalar, Gp);
            if (std::memcmp(xG_check, xG, crypto_core_ristretto255_BYTES) != 0) return false;
        }

        // -------------------------------------------------------------------
        // Commitment equation on the G-component:
        //   x*G + carry*G == masked*G + sum_p (share_p*G)
        // where share_p*G is derived from manifest:
        //   share_p*G = share_commitment_p - share_commitment_r_p * H
        // -------------------------------------------------------------------
        const auto masked_u64_opt = parse_signed_decimal_mod2_64(provider.masked_wire);
        if (!masked_u64_opt) {
            std::cerr << "[proof-real-v1] failed to parse masked_u64\n";
            return false;
        }

        unsigned char masked_scalar[crypto_core_ristretto255_SCALARBYTES];
        scalar_from_u64(masked_scalar, *masked_u64_opt);

        // Load full manifest from disk to get commitments.
        const fs::path manifest_path =
            fs::current_path() / "inputs" /
            ("provider_" + std::to_string(provider.provider_id) + "_manifest.json");
        const auto manifest_content_opt = read_text_file_all(manifest_path);
        if (!manifest_content_opt) return false;
        const auto manifest_json_opt = parse_json(*manifest_content_opt);
        if (!manifest_json_opt || !manifest_json_opt->is_object()) return false;
        const auto* manifest_obj = manifest_json_opt->as_object();

        const Json* parties_j = get_field(*manifest_obj, "parties");
        if (!parties_j || !parties_j->is_array()) return false;
        const auto* parties_arr = parties_j->as_array();
        if (!parties_arr || parties_arr->empty()) return false;

        // Sum share_p*G in the group.  libsodium rejects scalar 0 in
        // crypto_scalarmult_ristretto255, so we cannot seed the accumulator with
        // 0*G; start from the first shareG instead.
        unsigned char total_share_G[crypto_core_ristretto255_BYTES];
        bool have_total_share = false;

        for (const auto& pj : *parties_arr) {
            if (!pj.is_object()) return false;
            const auto* po = pj.as_object();
            const auto share_commitment_opt = get_string_field(*po, "share_commitment");
            const auto share_commitment_r_opt = get_string_field(*po, "share_commitment_r");
            if (!share_commitment_opt || !share_commitment_r_opt) return false;

            auto shareC_bytes_opt = from_hex(*share_commitment_opt);
            auto r_bytes_opt = from_hex(*share_commitment_r_opt);
            if (!shareC_bytes_opt || shareC_bytes_opt->size() != crypto_core_ristretto255_BYTES ||
                !r_bytes_opt || r_bytes_opt->size() != crypto_core_ristretto255_SCALARBYTES) {
                return false;
            }

            unsigned char shareC[crypto_core_ristretto255_BYTES];
            unsigned char r_scalar_p[crypto_core_ristretto255_SCALARBYTES];
            std::memcpy(shareC, shareC_bytes_opt->data(), crypto_core_ristretto255_BYTES);
            std::memcpy(r_scalar_p, r_bytes_opt->data(), crypto_core_ristretto255_SCALARBYTES);

            unsigned char rH[crypto_core_ristretto255_BYTES];
            crypto_scalarmult_ristretto255(rH, r_scalar_p, Hp);

            unsigned char shareG[crypto_core_ristretto255_BYTES];
            crypto_core_ristretto255_sub(shareG, shareC, rH);

            if (!have_total_share) {
                std::memcpy(total_share_G, shareG, crypto_core_ristretto255_BYTES);
                have_total_share = true;
            } else {
                crypto_core_ristretto255_add(total_share_G, total_share_G, shareG);
            }
        }
        if (!have_total_share) return false;

        unsigned char carryG[crypto_core_ristretto255_BYTES];
        unsigned char maskedG[crypto_core_ristretto255_BYTES];
        crypto_scalarmult_ristretto255(carryG, carry_scalar, Gp);
        crypto_scalarmult_ristretto255(maskedG, masked_scalar, Gp);

        unsigned char leftG[crypto_core_ristretto255_BYTES];
        unsigned char rightG[crypto_core_ristretto255_BYTES];
        crypto_core_ristretto255_add(leftG, xG, carryG);
        crypto_core_ristretto255_add(rightG, maskedG, total_share_G);

        const bool ok = std::memcmp(leftG, rightG, crypto_core_ristretto255_BYTES) == 0;
        if (!ok) std::cerr << "[proof-real-v1] commitment equation mismatch\n";
        return ok;
    }
};

class UnknownBackend final : public VerifierBackend {
public:
    bool verify(const ProviderEvidenceView&,
                const ManifestMinimalView&,
                const Evidence&,
                const std::string&) const override {
        return false;
    }
};

static const VerifierBackend& backend_for_system(const std::string& type_proof_system) {
    static const StubBackend stub;
    static const SemanticSchemaBackend semantic;
    static const ProofRealV1Backend proof_real;
    static const UnknownBackend unknown;
    if (type_proof_system == "stub-typeproof-v1") return stub;
    if (type_proof_system == "semantic-schema-v1") return semantic;
    if (type_proof_system == "proof-real-v1") return proof_real;
    return unknown;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<Evidence> parse_evidence_file(const std::string& json) {
    const auto provider_id_opt = parse_json_int_field(json, "provider_id");
    const auto nonce_opt = parse_json_string_field(json, "nonce");
    const auto schema_id_opt = parse_json_string_field(json, "schema_id");
    const auto masked_wire_digest_opt = parse_json_string_field(json, "masked_wire_digest");
    const auto share_manifest_id_opt = parse_json_string_field(json, "share_manifest_id");
    const auto mask_commitment_leaves_digest_opt =
        parse_json_string_field(json, "mask_commitment_leaves_digest");

    const auto type_proof_system_opt = parse_json_string_field(json, "type_proof_system");
    const auto type_proof_vk_id_opt = parse_json_string_field(json, "type_proof_vk_id");
    const auto type_proof_blob_opt = parse_json_string_field(json, "type_proof_blob");
    const auto type_proof_id_opt = parse_json_string_field(json, "type_proof_id");

    if (!provider_id_opt || !nonce_opt || !schema_id_opt || !masked_wire_digest_opt ||
        !share_manifest_id_opt || !mask_commitment_leaves_digest_opt ||
        !type_proof_system_opt || !type_proof_vk_id_opt || !type_proof_blob_opt || !type_proof_id_opt) {
        return std::nullopt;
    }

    Evidence ev;
    ev.provider_id = static_cast<int>(*provider_id_opt);
    ev.nonce = *nonce_opt;
    ev.schema_id = *schema_id_opt;
    ev.masked_wire_digest = *masked_wire_digest_opt;
    ev.share_manifest_id = *share_manifest_id_opt;
    ev.mask_commitment_leaves_digest = *mask_commitment_leaves_digest_opt;
    ev.type_proof_system = *type_proof_system_opt;
    ev.type_proof_vk_id = *type_proof_vk_id_opt;
    ev.type_proof_blob = *type_proof_blob_opt;
    ev.type_proof_id = *type_proof_id_opt;
    return ev;
}

std::optional<Evidence> load_evidence_file(const std::string& path) {
    const auto content_opt = read_text_file_all(fs::path(path));
    if (!content_opt) return std::nullopt;
    return parse_evidence_file(*content_opt);
}

bool verify_for_provider(const ProviderEvidenceView& provider,
                         const ManifestMinimalView& manifest,
                         const Evidence& tp,
                         const std::string& expected_schema_id,
                         std::string* out_reason) {
    if (tp.provider_id != provider.provider_id) {
        if (out_reason) *out_reason = "type_proof_provider_id_mismatch";
        return false;
    }
    if (tp.nonce != provider.nonce) {
        if (out_reason) *out_reason = "type_proof_nonce_mismatch";
        return false;
    }
    if (tp.schema_id != expected_schema_id) {
        if (out_reason) *out_reason = "type_proof_schema_id_not_authoritative";
        return false;
    }
    if (tp.masked_wire_digest != compute_masked_wire_digest(provider)) {
        if (out_reason) *out_reason = "type_proof_masked_wire_digest_mismatch";
        return false;
    }
    if (tp.share_manifest_id != manifest.share_manifest_id) {
        if (out_reason) *out_reason = "type_proof_share_manifest_id_mismatch";
        return false;
    }
    if (tp.mask_commitment_leaves_digest != manifest.mask_commitment_leaves_digest) {
        if (out_reason) *out_reason = "type_proof_mask_commitment_leaves_digest_mismatch";
        return false;
    }

    const VerifierBackend& backend = backend_for_system(tp.type_proof_system);
    if (!backend.verify(provider, manifest, tp, expected_schema_id)) {
        if (out_reason) *out_reason = "type_proof_backend_verification_failed";
        return false;
    }
    return true;
}

} // namespace type_proof

