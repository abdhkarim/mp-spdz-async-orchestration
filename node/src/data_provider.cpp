/**
 * data_provider.cpp
 *
 * Role: Each data provider holds a private value x_i.
 *
 * Confidentiality model:
 *   1. Provider generates s_i locally (libsodium CSPRNG).
 *   2. Provider splits s_i into N additive shares over Z/2^64:
 *          s_i = share_0 + share_1 + ... + share_{N-1}  (mod 2^64)
 *      and writes each share to:
 *          provider_secrets/provider_<id>_share_<p>.secret
 *   3. Provider computes masked_value = x_i - s_i and submits it publicly.
 *
 * The bridge never holds s_i in full — it only reads the per-party share
 * files and routes each share_p to computation node p.  No single entity
 * other than the provider itself can reconstruct x_i.
 *
 * Usage: ./data_provider <id> <value> --computation-nodes <N>
 */
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sodium.h>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;


// =============================================================================
// Section 1 – Logging
// =============================================================================
//
// A tiny logger that prepends a timestamp and a severity tag to every message.
// It writes to stdout for INFO/WARN/DEBUG and to stderr for ERROR.

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    void set_level(LogLevel level) {
        min_level_ = level;
    }

    void debug(const std::string& msg) { log(LogLevel::DEBUG, "[DEBUG]", msg); }
    void info (const std::string& msg) { log(LogLevel::INFO,  "[INFO ]", msg); }
    void warn (const std::string& msg) { log(LogLevel::WARN,  "[WARN ]", msg); }
    void error(const std::string& msg) { log(LogLevel::ERROR, "[ERROR]", msg); }

private:
    LogLevel min_level_ = LogLevel::INFO;

    void log(LogLevel level, const char* tag, const std::string& msg) {
        if (level < min_level_) return;

        // Build timestamp string
        const auto now  = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const std::tm* tm_info = std::localtime(&time);

        std::ostringstream line;
        line << "[" << std::put_time(tm_info, "%Y-%m-%d %H:%M:%S") << "] "
             << tag << " " << msg;

        if (level == LogLevel::ERROR) {
            std::cerr << line.str() << "\n";
        } else {
            std::cout << line.str() << "\n";
        }
    }
};

// Global logger instance used throughout the file.
static Logger g_logger;


// =============================================================================
// Section 2 – Big-number arithmetic (arbitrary-precision decimal strings)
// =============================================================================
//
// MP-SPDZ secrets are 64-bit unsigned integers, so x_i - s_i can go very
// negative and overflow any standard integer type. We handle subtraction on
// decimal strings to stay exact.

// Remove leading zeros from a digit string (e.g. "007" -> "7", "0" -> "0").
static std::string strip_leading_zeros(const std::string& digits) {
    size_t first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return "0";
    return digits.substr(first_nonzero);
}

// Compare two non-negative digit strings by magnitude.
// Returns -1, 0, or +1 (like strcmp semantics).
static int compare_unsigned(const std::string& a, const std::string& b) {
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return  1;
    return a.compare(b);   // same length: lexicographic == numeric
}

// Add two non-negative digit strings and return the result as a digit string.
static std::string add_unsigned(const std::string& a, const std::string& b) {
    std::string result;
    int carry = 0;
    int i = static_cast<int>(a.size()) - 1;
    int j = static_cast<int>(b.size()) - 1;

    while (i >= 0 || j >= 0 || carry > 0) {
        int digit_a = (i >= 0) ? (a[i--] - '0') : 0;
        int digit_b = (j >= 0) ? (b[j--] - '0') : 0;
        int sum     = digit_a + digit_b + carry;
        result.push_back('0' + (sum % 10));
        carry = sum / 10;
    }

    std::reverse(result.begin(), result.end());
    return strip_leading_zeros(result);
}

// Subtract a smaller-or-equal non-negative digit string from a larger one.
// Precondition: |larger| >= |smaller| (magnitude).
static std::string subtract_unsigned(const std::string& larger,
                                     const std::string& smaller) {
    std::string result;
    int borrow = 0;
    int i = static_cast<int>(larger.size())  - 1;
    int j = static_cast<int>(smaller.size()) - 1;

    while (i >= 0) {
        int digit_l = larger[i--] - '0';
        int digit_s = (j >= 0) ? (smaller[j--] - '0') : 0;
        int diff    = digit_l - digit_s - borrow;
        if (diff < 0) { diff += 10; borrow = 1; }
        else          { borrow = 0; }
        result.push_back('0' + diff);
    }

    std::reverse(result.begin(), result.end());
    return strip_leading_zeros(result);
}

// Compute a - b for arbitrary signed decimal strings (e.g. "-123", "456").
// Returns the result as a signed decimal string.
std::string subtract_strings(const std::string& a, const std::string& b) {
    // --- Parse sign and magnitude for each operand ---
    auto parse = [](const std::string& s, bool& negative, std::string& magnitude) {
        if (s.empty()) throw std::invalid_argument("empty number string");
        size_t start = 0;
        negative = false;
        if (s[0] == '-') { negative = true;  start = 1; }
        if (s[0] == '+') { negative = false; start = 1; }
        if (start >= s.size()) throw std::invalid_argument("number has no digits");
        for (size_t k = start; k < s.size(); ++k)
            if (!std::isdigit(static_cast<unsigned char>(s[k])))
                throw std::invalid_argument("non-digit character: " + s);
        magnitude = strip_leading_zeros(s.substr(start));
        if (magnitude == "0") negative = false;   // avoid "-0"
    };

    bool a_neg, b_neg;
    std::string a_mag, b_mag;
    parse(a, a_neg, a_mag);
    parse(b, b_neg, b_mag);

    // --- a - b  ==  a + (-b) ---
    // Flip b's sign and delegate to signed addition logic.
    bool b_flipped = !b_neg;   // negate b

    // Signed addition: a_neg/a_mag  +  b_flipped/b_mag
    bool  result_neg;
    std::string result_mag;

    if (a_neg == b_flipped) {
        // Same sign: magnitudes add, sign is common sign.
        result_mag = add_unsigned(a_mag, b_mag);
        result_neg = a_neg;
    } else {
        // Different signs: subtract smaller magnitude from larger.
        int cmp = compare_unsigned(a_mag, b_mag);
        if (cmp == 0) return "0";
        if (cmp > 0) {
            result_mag = subtract_unsigned(a_mag, b_mag);
            result_neg = a_neg;
        } else {
            result_mag = subtract_unsigned(b_mag, a_mag);
            result_neg = b_flipped;
        }
    }

    if (result_mag == "0") return "0";
    return (result_neg ? "-" : "") + result_mag;
}


// =============================================================================
// Section 3 – Input validation
// =============================================================================

// Provider ID: alphanumeric + underscore/hyphen, max 32 characters.
bool validate_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            return false;
    return true;
}

// Value: plain integer, no float, no scientific notation, max 64 characters.
bool validate_value(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (char c : value)
        if (c == '.' || c == 'e' || c == 'E') return false;
    try   { std::stoll(value); return true; }
    catch (...) { return false; }
}


// =============================================================================
// Section 4 – Cryptographic proof (BLAKE2b via libsodium)
// =============================================================================
//
// The proof is a keyed BLAKE2b hash over the string "id=…;value=…;nonce=…".
// The key is the shared auth secret (MPC_PROVIDER_SECRET env variable).
// The consensus node recomputes this hash to verify the file was produced by
// a legitimate provider and has not been modified.

std::string to_hex(const unsigned char* data, size_t len) {
    static const char* hex_chars = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += hex_chars[(data[i] >> 4) & 0x0F];
        out += hex_chars[ data[i]       & 0x0F];
    }
    return out;
}

static std::vector<unsigned char> from_hex_to_bytes(const std::string& hex) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<unsigned char> out;
    if (hex.size() % 2 != 0) return out;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return {};
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

static bool pedersen_commit_u64(unsigned char out_point[crypto_core_ristretto255_BYTES],
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

static void hash_to_scalar(unsigned char out_scalar[crypto_core_ristretto255_SCALARBYTES],
                           const std::string& msg) {
    unsigned char h[64] = {0};
    crypto_generichash(h, sizeof(h),
                       reinterpret_cast<const unsigned char*>(msg.data()),
                       msg.size(),
                       nullptr, 0);
    crypto_core_ristretto255_scalar_reduce(out_scalar, h);
}

static std::string hex32(const unsigned char s[crypto_core_ristretto255_SCALARBYTES]) {
    return to_hex(s, crypto_core_ristretto255_SCALARBYTES);
}

static std::string hex_point(const unsigned char p[crypto_core_ristretto255_BYTES]) {
    return to_hex(p, crypto_core_ristretto255_BYTES);
}

// OR-proof that Pedersen commitment C opens to either m0 or m1 (as scalars).
// Transcript: e (full Fiat–Shamir challenge), e1 (second split component), s0, s1 (scalars hex).
struct OrProof {
    std::string e_hex;
    std::string e1_hex;
    std::string s0_hex;
    std::string s1_hex;
};

static OrProof prove_commitment_one_of_two(const unsigned char C[crypto_core_ristretto255_BYTES],
                                          const unsigned char r_open[crypto_core_ristretto255_SCALARBYTES],
                                          bool actual_is_m0,
                                          const unsigned char m0_scalar[crypto_core_ristretto255_SCALARBYTES],
                                          const unsigned char m1_scalar[crypto_core_ristretto255_SCALARBYTES],
                                          const unsigned char G[crypto_core_ristretto255_BYTES],
                                          const unsigned char H[crypto_core_ristretto255_BYTES],
                                          const std::string& domain) {
    // Implementation of a standard Schnorr OR proof on the statement:
    //   know r such that C - m*G = r*H for m in {m0,m1}.
    // We simulate the non-chosen branch.
    const bool is_m0 = actual_is_m0;

    auto scalar_add = [](unsigned char out[32], const unsigned char a[32], const unsigned char b[32]) {
        crypto_core_ristretto255_scalar_add(out, a, b);
    };
    auto scalar_sub = [](unsigned char out[32], const unsigned char a[32], const unsigned char b[32]) {
        crypto_core_ristretto255_scalar_sub(out, a, b);
    };
    auto scalar_mul = [](unsigned char out[32], const unsigned char a[32], const unsigned char b[32]) {
        crypto_core_ristretto255_scalar_mul(out, a, b);
    };

    auto point_mul = [](unsigned char out[32], const unsigned char s[32], const unsigned char P[32]) {
        crypto_scalarmult_ristretto255(out, s, P);
    };

    auto point_sub = [](unsigned char out[32], const unsigned char A[32], const unsigned char B[32]) {
        crypto_core_ristretto255_sub(out, A, B);
    };

    unsigned char m0G[32]; (void)crypto_scalarmult_ristretto255(m0G, m0_scalar, G);
    unsigned char m1G[32]; (void)crypto_scalarmult_ristretto255(m1G, m1_scalar, G);

    unsigned char C_minus_m0G[32]; point_sub(C_minus_m0G, C, m0G);
    unsigned char C_minus_m1G[32]; point_sub(C_minus_m1G, C, m1G);

    // Simulated branch scalars (e_sim, s_sim) chosen at random.
    unsigned char e_sim[32]; crypto_core_ristretto255_scalar_random(e_sim);
    unsigned char s_sim[32]; crypto_core_ristretto255_scalar_random(s_sim);

    // Real branch witness commitment: a_real = w*H.
    unsigned char w[32]; crypto_core_ristretto255_scalar_random(w);
    unsigned char a_real[32]; point_mul(a_real, w, H);

    // Simulated branch commitment: a_sim = s_sim*H + e_sim*(C - m_sim*G).
    unsigned char sH[32]; point_mul(sH, s_sim, H);
    unsigned char eX[32];
    point_mul(eX, e_sim, is_m0 ? C_minus_m1G : C_minus_m0G); // simulate opposite branch
    unsigned char a_sim[32]; crypto_core_ristretto255_add(a_sim, sH, eX);

    // Fiat-Shamir challenge e = H(domain | C | a0 | a1).
    unsigned char e[32];
    std::string transcript = domain + "|" + hex_point(C) + "|";
    if (is_m0) transcript += hex_point(a_real) + "|" + hex_point(a_sim);
    else       transcript += hex_point(a_sim) + "|" + hex_point(a_real);
    hash_to_scalar(e, transcript);

    // Split e into e0+e1 = e where the simulated branch uses e_sim.
    unsigned char e0[32] = {0};
    unsigned char e1[32] = {0};
    if (is_m0) {
        // simulated is branch 1
        std::memcpy(e1, e_sim, 32);
        scalar_sub(e0, e, e1);
    } else {
        // simulated is branch 0
        std::memcpy(e0, e_sim, 32);
        scalar_sub(e1, e, e0);
    }

    // Responses:
    // s_real = w + e_real * r_open
    unsigned char e_real[32];
    std::memcpy(e_real, is_m0 ? e0 : e1, 32);
    unsigned char e_r[32]; scalar_mul(e_r, e_real, r_open);
    unsigned char s_real[32]; scalar_add(s_real, w, e_r);

    unsigned char s0[32] = {0};
    unsigned char s1[32] = {0};
    if (is_m0) {
        std::memcpy(s0, s_real, 32);
        std::memcpy(s1, s_sim, 32);
    } else {
        std::memcpy(s0, s_sim, 32);
        std::memcpy(s1, s_real, 32);
    }

    OrProof out;
    // Full challenge e is unchanged after the split into e0/e1; consensus checks H(transcript)==e.
    out.e_hex = hex32(e);
    out.e1_hex = hex32(e1);
    out.s0_hex = hex32(s0);
    out.s1_hex = hex32(s1);
    return out;
}

static uint64_t parse_signed_decimal_mod2_64(const std::string& s) {
    // Parse arbitrary-length signed decimal string and return value mod 2^64.
    // Accepts canonical wire format already enforced elsewhere.
    bool neg = false;
    size_t i = 0;
    if (!s.empty() && s[0] == '-') { neg = true; i = 1; }
    uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        uint64_t d = static_cast<uint64_t>(c - '0');
        __uint128_t tmp = static_cast<__uint128_t>(acc) * 10 + d;
        acc = static_cast<uint64_t>(tmp); // mod 2^64 by truncation
    }
    if (!neg) return acc;
    // two's complement: (-acc) mod 2^64
    return static_cast<uint64_t>(0) - acc;
}

// proof-real-v1 encodes only one carry term {0, 2^64}.  The integer sum of share
// limbs V can be s + k*2^64 (k in {0,1,2}); consensus checks group arithmetic
// with that V.  Resample until (masked + V - x) is 0 or 2^64 (exact multiple).
static bool proof_real_carry_coefficient_ok(const std::vector<uint64_t>& shares,
                                            const std::string& masked_value,
                                            uint64_t x_u64) {
    const uint64_t masked_u64 = parse_signed_decimal_mod2_64(masked_value);
    __int128_t V = 0;
    for (uint64_t sh : shares) V += static_cast<__int128_t>(sh);
    const __int128_t diff =
        static_cast<__int128_t>(masked_u64) + V - static_cast<__int128_t>(x_u64);
    if (diff < 0) return false;
    const auto du = static_cast<unsigned __int128>(diff);
    const unsigned __int128 uq = static_cast<unsigned __int128>(1) << 64;
    if (du % uq != 0) return false;
    const unsigned __int128 t = du / uq;
    return t <= 1;
}

static void delete_provider_share_files(const std::string& provider_id, int n_parties) {
    const fs::path secrets_dir = fs::current_path() / "provider_secrets";
    for (int p = 0; p < n_parties; ++p) {
        const fs::path sp =
            secrets_dir / ("provider_" + provider_id + "_share_" + std::to_string(p) + ".secret");
        std::error_code ec;
        fs::remove(sp, ec);
    }
}

// Unkeyed BLAKE2b-256 (64 hex chars) over ASCII bytes.
static std::string blake2b_hex32(const std::string& msg) {
    unsigned char digest[32] = {0};
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, sizeof(digest));
    crypto_generichash_update(&state,
                              reinterpret_cast<const unsigned char*>(msg.data()),
                              msg.size());
    crypto_generichash_final(&state, digest, sizeof(digest));
    return to_hex(digest, sizeof(digest));
}

std::string compute_proof(const std::string& id,
                          const std::string& value,
                          const std::string& nonce,
                          const std::string& auth_secret) {
    const std::string message = "id=" + id + ";value=" + value + ";nonce=" + nonce;

    unsigned char digest[crypto_generichash_BYTES] = {};
    crypto_generichash_state state;

    if (crypto_generichash_init(&state,
            reinterpret_cast<const unsigned char*>(auth_secret.data()),
            auth_secret.size(),
            sizeof(digest)) != 0)
        throw std::runtime_error("crypto_generichash_init failed");

    if (crypto_generichash_update(&state,
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size()) != 0)
        throw std::runtime_error("crypto_generichash_update failed");

    if (crypto_generichash_final(&state, digest, sizeof(digest)) != 0)
        throw std::runtime_error("crypto_generichash_final failed");

    return to_hex(digest, sizeof(digest));
}


// =============================================================================
// Section 5 – Per-provider secret splitting  — provider-side share generation
// =============================================================================
//
// The provider generates s_i and IMMEDIATELY splits it into N additive shares
// over Z/2^64.  Only the per-party share files are written to disk:
//
//   provider_secrets/provider_<id>_share_<p>.secret   (one file per party)
//
// The full s_i is NEVER written to disk.  The bridge reads only share files
// and routes share_p to computation node p — no single process other than
// this provider can reconstruct s_i (and hence x_i).
//
// Share equation:  s_i = share_0 + share_1 + ... + share_{N-1}  (mod 2^64)
// Idempotent: if all N share files already exist and are non-empty, the
// existing shares are returned unchanged.

struct SplitSecret {
    uint64_t             full_secret;  // s_i (held only in memory, never written)
    std::vector<uint64_t> shares;      // share_p for p in [0, N)
};

std::optional<SplitSecret> generate_or_load_provider_shares(
    const std::string& provider_id, int n_parties)
{
    const fs::path secrets_dir = fs::current_path() / "provider_secrets";
    fs::create_directories(secrets_dir);

    // Build share file paths.
    std::vector<fs::path> share_paths;
    for (int p = 0; p < n_parties; ++p) {
        share_paths.push_back(
            secrets_dir / ("provider_" + provider_id +
                           "_share_" + std::to_string(p) + ".secret"));
    }

    // --- Idempotent: try to load all existing share files ---
    {
        std::vector<uint64_t> existing_shares;
        bool all_ok = true;
        for (const auto& sp : share_paths) {
            std::ifstream in(sp);
            std::string line;
            if (!in.is_open() || !std::getline(in, line) || line.empty()) {
                all_ok = false;
                break;
            }
            try {
                existing_shares.push_back(std::stoull(line));
            } catch (...) {
                all_ok = false;
                break;
            }
        }
        if (all_ok && static_cast<int>(existing_shares.size()) == n_parties) {
            // Reconstruct s_i in memory only for masking.
            uint64_t s_full = 0;
            for (auto sh : existing_shares) s_full += sh;
            g_logger.info("Provider " + provider_id +
                          ": loaded " + std::to_string(n_parties) + " existing shares");
            return SplitSecret{s_full, existing_shares};
        }
    }

    // --- Generate fresh s_i and split into N shares ---
    uint64_t s_i = 0;
    randombytes_buf(&s_i, sizeof(s_i));

    std::vector<uint64_t> shares;
    shares.reserve(static_cast<size_t>(n_parties));
    uint64_t running_sum = 0;
    for (int p = 0; p < n_parties - 1; ++p) {
        uint64_t r = 0;
        randombytes_buf(&r, sizeof(r));
        shares.push_back(r);
        running_sum += r;  // mod 2^64 via unsigned overflow
    }
    shares.push_back(s_i - running_sum);  // last share closes the sum

    // Write one share file per party (owner read/write only).
    for (int p = 0; p < n_parties; ++p) {
        std::ofstream out(share_paths[static_cast<size_t>(p)], std::ios::trunc);
        if (!out.is_open()) {
            g_logger.error("Cannot write share file: " + share_paths[static_cast<size_t>(p)].string());
            return std::nullopt;
        }
        out << shares[static_cast<size_t>(p)] << "\n";
        out.close();

        std::error_code ec;
        fs::permissions(share_paths[static_cast<size_t>(p)],
                        fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
    }

    g_logger.info("Provider " + provider_id + ": generated s_i, split into " +
                  std::to_string(n_parties) + " share files (s_i never written to disk)");
    return SplitSecret{s_i, shares};
}


// =============================================================================
// Section 6 – Writing the provider file
// =============================================================================
//
// Creates  inputs/provider_<id>.txt  with four fields:
//   id             – provider identity
//   masked_value   – x_i - s_i  (value masked by the provider-split secret)
//   nonce          – random 16-byte hex string (replay protection)
//   proof          – BLAKE2b authentication tag
//
// Confidentiality guarantee: s_i is generated and split into N shares entirely
// by this provider process.  The bridge reads only per-party share files and
// routes share_p to computation node p.  No entity other than this provider
// can reconstruct s_i or x_i.

static std::string base64_encode(const std::string& s) {
    const size_t max_len = sodium_base64_ENCODED_LEN(s.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(max_len, '\0');
    sodium_bin2base64(out.data(), out.size(),
                      reinterpret_cast<const unsigned char*>(s.data()), s.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

int write_provider_file(const std::string& id,
                        const std::string& value,
                        const std::string& auth_secret,
                        int n_parties,
                        const std::string& schema_id,
                        const std::string& type_proof_system,
                        const std::string& type_proof_vk_id,
                        const std::optional<std::string>& typed_json_opt) {
    // --- Prepare output path ---
    const fs::path inputs_dir  = fs::current_path() / "inputs";
    fs::create_directories(inputs_dir);
    const fs::path output_file = inputs_dir / ("provider_" + id + ".txt");

    std::ofstream out(output_file);
    if (!out.is_open()) {
        g_logger.error("Cannot open output file: " + output_file.string());
        return 1;
    }

    // --- Generate s_i and split into N per-party shares (required; no plain fallback) ---
    std::optional<SplitSecret> split;
    std::string masked_value;

    auto parse_x_u64 = [&value]() -> uint64_t {
        long long x_ll = 0;
        try { x_ll = std::stoll(value); } catch (...) { x_ll = 0; }
        return static_cast<uint64_t>(static_cast<int64_t>(x_ll));
    };

    if (type_proof_system == "proof-real-v1") {
        constexpr int k_max_attempts = 128;
        bool accepted = false;
        for (int attempt = 0; attempt < k_max_attempts; ++attempt) {
            if (attempt > 0) delete_provider_share_files(id, n_parties);
            auto s_opt = generate_or_load_provider_shares(id, n_parties);
            if (!s_opt) {
                g_logger.error("Provider " + id +
                               ": could not generate or load per-party shares — aborting");
                return 1;
            }
            std::string mv;
            try {
                mv = subtract_strings(value, std::to_string(s_opt->full_secret));
            } catch (const std::exception& e) {
                g_logger.warn("Masking attempt failed (" + std::string(e.what()) + "), resampling shares");
                continue;
            }
            if (!proof_real_carry_coefficient_ok(s_opt->shares, mv, parse_x_u64()))
                continue;
            split = std::move(s_opt);
            masked_value = std::move(mv);
            accepted = true;
            break;
        }
        if (!accepted) {
            g_logger.error("Provider " + id +
                           ": proof-real-v1 could not sample shares compatible with carry OR — aborting");
            return 1;
        }
        g_logger.info("Provider " + id + ": masked " + value +
                      " → " + masked_value + " (x - s_i, s_i split into " +
                      std::to_string(n_parties) + " shares)");
    } else {
        split = generate_or_load_provider_shares(id, n_parties);
        if (!split) {
            g_logger.error("Provider " + id +
                           ": could not generate or load per-party shares — aborting");
            return 1;
        }
        try {
            masked_value = subtract_strings(value, std::to_string(split->full_secret));
            g_logger.info("Provider " + id + ": masked " + value +
                          " → " + masked_value + " (x - s_i, s_i split into " +
                          std::to_string(n_parties) + " shares)");
        } catch (const std::exception& e) {
            g_logger.error("Masking failed (" + std::string(e.what()) + ") — aborting");
            return 1;
        }
    }

    // --- Random nonce for replay protection ---
    unsigned char nonce_raw[16] = {};
    randombytes_buf(nonce_raw, sizeof(nonce_raw));
    const std::string nonce = to_hex(nonce_raw, sizeof(nonce_raw));

    // Provider->consensus will use `masked_wire_digest` to bind `type_proof`
    // to the canonical decimal masked wire string used by the admission layer.
    // For the prototype, we use unkeyed BLAKE2b-256 hex digests (64 hex chars).
    const std::string masked_wire_digest = blake2b_hex32(masked_value);

    // --- Cryptographic proof over the masked value ---
    const std::string proof = compute_proof(id, masked_value, nonce, auth_secret);

    // --- Public share manifest (commitments/digests) ---
    // This is required for decentralized per-party verification: each
    // computation-node verifier can validate its own local share file
    // without seeing other parties' shares.
    {
        // share_manifest_id binds the provider evidence without requiring
        // any raw share material.
        const std::string manifest_input =
            "provider_id=" + id +
            ";nonce=" + nonce +
            ";masked_value=" + masked_value +
            ";num_parties=" + std::to_string(n_parties);
        const std::string share_manifest_id = blake2b_hex32(manifest_input);

        std::vector<std::string> leaf_digests;
        leaf_digests.reserve(static_cast<size_t>(n_parties));

        // Precompute per-party digests deterministically.
        std::vector<std::string> share_file_digests;
        std::vector<std::string> share_leaf_digests;
        std::vector<std::string> share_commitments_hex;
        std::vector<std::string> share_commitment_r_hex;
        share_file_digests.reserve(static_cast<size_t>(n_parties));
        share_leaf_digests.reserve(static_cast<size_t>(n_parties));
        share_commitments_hex.reserve(static_cast<size_t>(n_parties));
        share_commitment_r_hex.reserve(static_cast<size_t>(n_parties));

        unsigned char G[crypto_core_ristretto255_BYTES];
        unsigned char H[crypto_core_ristretto255_BYTES];
        point_from_domain(G, "mpc:pedersen:G:v1");
        point_from_domain(H, "mpc:pedersen:H:v1");

        for (int p = 0; p < n_parties; ++p) {
            const std::string share_value_dec = std::to_string(split->shares[static_cast<size_t>(p)]);
            const std::string share_file_digest = blake2b_hex32("share_file_value=" + share_value_dec);

            // Leaf digest binds: provider evidence + local share value.
            const std::string leaf_digest_input =
                "provider_id=" + id +
                ";nonce=" + nonce +
                ";masked_value=" + masked_value +
                ";party_index=" + std::to_string(p) +
                ";share_value=" + share_value_dec;
            const std::string share_leaf_digest = blake2b_hex32(leaf_digest_input);

            share_file_digests.push_back(share_file_digest);
            share_leaf_digests.push_back(share_leaf_digest);
            leaf_digests.push_back(share_leaf_digest);

            // Pedersen commitment to the local share (v = share_u64, r random scalar).
            unsigned char r_scalar[crypto_core_ristretto255_SCALARBYTES];
            crypto_core_ristretto255_scalar_random(r_scalar);
            unsigned char C[crypto_core_ristretto255_BYTES];
            if (!pedersen_commit_u64(C,
                                     split->shares[static_cast<size_t>(p)],
                                     r_scalar, G, H)) {
                g_logger.error("Failed to compute share Pedersen commitment");
                return 1;
            }
            share_commitments_hex.push_back(to_hex(C, sizeof(C)));
            share_commitment_r_hex.push_back(to_hex(r_scalar, sizeof(r_scalar)));
        }

        std::string concat;
        concat.reserve(static_cast<size_t>(n_parties) * 80);
        for (int p = 0; p < n_parties; ++p) {
            concat += leaf_digests[static_cast<size_t>(p)];
            concat.push_back(';');
        }
        const std::string mask_commitment_leaves_digest = blake2b_hex32("mask_commitment|" + concat);

        const fs::path manifest_path =
            inputs_dir / ("provider_" + id + "_manifest.json");
        std::ofstream m_out(manifest_path, std::ios::trunc);
        if (!m_out.is_open()) {
            g_logger.error("Cannot write manifest: " + manifest_path.string());
            return 1;
        }

        m_out << "{\n";
        m_out << "  \"num_parties\": " << n_parties << ",\n";
        m_out << "  \"share_manifest_id\": \"" << share_manifest_id << "\",\n";
        m_out << "  \"mask_commitment_leaves_digest\": \"" << mask_commitment_leaves_digest << "\",\n";
        m_out << "  \"parties\": [\n";
        for (int p = 0; p < n_parties; ++p) {
            m_out << "    {\n";
            m_out << "      \"party_index\": " << p << ",\n";
            m_out << "      \"share_file_digest\": \"" << share_file_digests[static_cast<size_t>(p)] << "\",\n";
            m_out << "      \"share_leaf_digest\": \"" << share_leaf_digests[static_cast<size_t>(p)] << "\",\n";
            m_out << "      \"share_commitment\": \"" << share_commitments_hex[static_cast<size_t>(p)] << "\",\n";
            m_out << "      \"share_commitment_r\": \"" << share_commitment_r_hex[static_cast<size_t>(p)] << "\"\n";
            m_out << "    }";
            if (p + 1 < n_parties) m_out << ",";
            m_out << "\n";
        }
        m_out << "  ]\n";
        m_out << "}\n";

        if (!m_out.good()) {
            g_logger.error("Write error for manifest: " + manifest_path.string());
            return 1;
        }
        g_logger.info("Provider " + id + ": wrote share manifest " + manifest_path.string());

        // ---------------------------------------------------------------------
        // Type proof provider-side artifact (stub for now)
        //
        // This artifact is consumed directly by `consensus` (admission layer).
        // The semantic proof backend is intentionally stubbed: the goal is to
        // set up a clean interface + binding chain without moving logic into
        // the bridge.
        // ---------------------------------------------------------------------
        std::string provider_id_numeric;
        try {
            provider_id_numeric = std::to_string(std::stoll(id));
        } catch (...) {
            g_logger.warn("Provider " + id +
                          ": cannot parse provider id as integer; skipping type_proof artifact");
            // Continue: the provider input file is still written, but it will
            // be rejected by consensus due to type_proof parse/binding failure.
            provider_id_numeric.clear();
        }

        if (!provider_id_numeric.empty()) {
            const std::string protocol_version = "1";

            std::string type_proof_blob;
            if (type_proof_system == "stub-typeproof-v1") {
                type_proof_blob =
                    blake2b_hex32("type_proof_blob:provider_id=" + provider_id_numeric +
                                  ";nonce=" + nonce +
                                  ";masked_wire=" + masked_value);
            } else if (type_proof_system == "semantic-schema-v1") {
                // NOTE: This is NOT a ZK proof. It is a semantic evidence blob
                // that consensus can validate against an authoritative schema.
                // The blob is base64(JSON) to keep provider_<id>_type_proof.json simple.
                std::string value_json;
                if (typed_json_opt) {
                    // Allow explicit test cases / composite types by injecting a JSON value directly.
                    value_json = *typed_json_opt;
                } else if (schema_id == "bool-v1" || schema_id == "int64-v1" || schema_id == "semi2k-wire-v1") {
                    value_json = value; // integer JSON literal
                } else if (schema_id == "fixed_point_s2_v1") {
                    value_json = std::string("{\"unscaled\":") + value + "}";
                } else {
                    g_logger.error("schema_id '" + schema_id +
                                   "' requires --typed-json for semantic-schema-v1");
                    return 1;
                }
                const std::string blob_json =
                    std::string("{\"schema_id\":\"") + schema_id + "\",\"value\":" + value_json + "}";
                type_proof_blob = base64_encode(blob_json);
            } else if (type_proof_system == "proof-real-v1") {
                // Strong(er) prototype: Pedersen commitments + OR proofs + linear relation in commitments.
                // This proves linkage between masked_value and the per-party share commitments (but not a full SNARK).
                // It relies on share_verifier signing the share commitment fields in ACKs (Option A).

                // Compute masked element modulo 2^64 from the signed decimal wire.
                const uint64_t masked_u64 = parse_signed_decimal_mod2_64(masked_value);

                // Compute x as uint64_t two's complement from the provider int64 input.
                long long x_ll = 0;
                try { x_ll = std::stoll(value); } catch (...) { x_ll = 0; }
                const int64_t x_i64 = static_cast<int64_t>(x_ll);
                const uint64_t x_u64 = static_cast<uint64_t>(x_i64);

                // Integer sum of share limbs (matches consensus commitment aggregation).
                __uint128_t V128 = 0;
                for (auto sh : split->shares) V128 += static_cast<uint64_t>(sh);

                // Carry term t in {0, 2^64} for the equation over integers:
                // x_u64 + t*2^64 = masked_u64 + V128  (exact in ℤ, t ∈ {0,1} after resampling).
                const __uint128_t rhs = static_cast<__uint128_t>(masked_u64) + V128;
                const bool carry = (rhs >= (static_cast<__uint128_t>(1) << 64));

                // Commitments:
                unsigned char Gp[crypto_core_ristretto255_BYTES];
                unsigned char Hp[crypto_core_ristretto255_BYTES];
                point_from_domain(Gp, "mpc:pedersen:G:v1");
                point_from_domain(Hp, "mpc:pedersen:H:v1");

                unsigned char r_x[crypto_core_ristretto255_SCALARBYTES];
                crypto_core_ristretto255_scalar_random(r_x);
                unsigned char C_x[crypto_core_ristretto255_BYTES];
                if (!pedersen_commit_u64(C_x, x_u64, r_x, Gp, Hp)) {
                    g_logger.error("Failed to compute x commitment");
                    return 1;
                }

                // Commitment to carry term: commit to 0 or 2^64 via OR-proof.
                // Since 2^64 doesn't fit u64, we encode it as scalar with bit 64 set in little-endian 32 bytes.
                unsigned char r_t[crypto_core_ristretto255_SCALARBYTES];
                crypto_core_ristretto255_scalar_random(r_t);
                unsigned char C_t[crypto_core_ristretto255_BYTES];
                // Build scalar for t: 0 or 2^64.
                unsigned char t_scalar[crypto_core_ristretto255_SCALARBYTES];
                std::memset(t_scalar, 0, sizeof(t_scalar));
                if (carry) {
                    t_scalar[8] = 1; // 2^64 in little-endian scalar bytes
                }
                unsigned char a[crypto_core_ristretto255_BYTES];
                unsigned char b[crypto_core_ristretto255_BYTES];
                if (crypto_scalarmult_ristretto255(a, t_scalar, Gp) != 0) return 1;
                if (crypto_scalarmult_ristretto255(b, r_t, Hp) != 0) return 1;
                crypto_core_ristretto255_add(C_t, a, b);

                // OR proof for carry commitment in {0, 2^64}.
                const uint64_t m0 = 0ULL;
                const uint64_t m1 = 0ULL; // placeholder; encoded in scalar via (bit 64) for 2^64
                (void)m1;
                unsigned char zero_scalar[crypto_core_ristretto255_SCALARBYTES];
                std::memset(zero_scalar, 0, sizeof(zero_scalar));
                unsigned char two64_scalar[crypto_core_ristretto255_SCALARBYTES];
                std::memset(two64_scalar, 0, sizeof(two64_scalar));
                two64_scalar[8] = 1; // 2^64

                OrProof carry_pf = prove_commitment_one_of_two(
                    C_t, r_t,
                    !carry, // if carry==false => t=0
                    zero_scalar, two64_scalar,
                    Gp, Hp,
                    "proof-real-v1:carry");

                // Optional OR proof for x commitment if schema bool.
                std::optional<OrProof> bool_pf;
                if (schema_id == "bool-v1") {
                    unsigned char one_scalar[crypto_core_ristretto255_SCALARBYTES];
                    scalar_from_u64(one_scalar, 1ULL);
                    bool_pf = prove_commitment_one_of_two(
                        C_x, r_x,
                        (x_u64 == 0ULL),
                        zero_scalar, one_scalar,
                        Gp, Hp,
                        "proof-real-v1:bool");
                }

                const std::string blob_json =
                    std::string("{\"version\":1,") +
                    "\"proof_system\":\"proof-real-v1\"," +
                    "\"x_commitment\":\"" + to_hex(C_x, sizeof(C_x)) + "\"," +
                    "\"x_randomizer\":\"" + to_hex(r_x, sizeof(r_x)) + "\"," +
                    "\"carry_commitment\":\"" + to_hex(C_t, sizeof(C_t)) + "\"," +
                    "\"carry_or\":{\"e\":\"" + carry_pf.e_hex + "\",\"e1\":\"" + carry_pf.e1_hex + "\",\"s0\":\"" + carry_pf.s0_hex + "\",\"s1\":\"" + carry_pf.s1_hex + "\"}" +
                    (bool_pf ? (std::string(",\"bool_or\":{\"e\":\"") + bool_pf->e_hex + "\",\"e1\":\"" + bool_pf->e1_hex + "\",\"s0\":\"" + bool_pf->s0_hex + "\",\"s1\":\"" + bool_pf->s1_hex + "\"}") : std::string("")) +
                    "}";
                type_proof_blob = base64_encode(blob_json);
            } else {
                g_logger.error("Unknown --type-proof-system: " + type_proof_system);
                return 1;
            }

            const std::string type_proof_id =
                blake2b_hex32("provider_id=" + provider_id_numeric +
                              ";nonce=" + nonce +
                              ";schema_id=" + schema_id +
                              ";type_proof_system=" + type_proof_system +
                              ";type_proof_vk_id=" + type_proof_vk_id +
                              ";type_proof_blob=" + type_proof_blob);

            const fs::path type_proof_path =
                inputs_dir / ("provider_" + provider_id_numeric + "_type_proof.json");
            std::ofstream tp_out(type_proof_path, std::ios::trunc);
            if (!tp_out.is_open()) {
                g_logger.error("Cannot write type proof artifact: " + type_proof_path.string());
                return 1;
            }

            tp_out << "{\n";
            tp_out << "  \"provider_id\": " << provider_id_numeric << ",\n";
            tp_out << "  \"protocol_version\": \"" << protocol_version << "\",\n";
            tp_out << "  \"schema_id\": \"" << schema_id << "\",\n";
            tp_out << "  \"nonce\": \"" << nonce << "\",\n";
            tp_out << "  \"masked_wire_digest\": \"" << masked_wire_digest << "\",\n";
            tp_out << "  \"share_manifest_id\": \"" << share_manifest_id << "\",\n";
            tp_out << "  \"mask_commitment_leaves_digest\": \"" << mask_commitment_leaves_digest << "\",\n";
            tp_out << "  \"type_proof_system\": \"" << type_proof_system << "\",\n";
            tp_out << "  \"type_proof_vk_id\": \"" << type_proof_vk_id << "\",\n";
            tp_out << "  \"type_proof_blob\": \"" << type_proof_blob << "\",\n";
            tp_out << "  \"type_proof_id\": \"" << type_proof_id << "\"\n";
            tp_out << "}\n";

            if (!tp_out.good()) {
                g_logger.error("Write error for type proof artifact: " + type_proof_path.string());
                return 1;
            }

            g_logger.info("Provider " + id + ": wrote type proof artifact " + type_proof_path.string());
        }
    }

    // --- Write the file ---
    out << "id="           << id           << "\n"
        << "masked_value=" << masked_value << "\n"
        << "nonce="        << nonce        << "\n"
        << "proof="        << proof        << "\n";

    if (!out.good()) {
        g_logger.error("Write error for file: " + output_file.string());
        return 1;
    }

    g_logger.info("Provider " + id + " wrote file: " + output_file.string());
    return 0;
}


// =============================================================================
// Section 7 – Entry point
// =============================================================================

int main(int argc, char* argv[]) {
    // --- Parse command-line arguments ---
    // Usage: ./data_provider <id> <value> [--computation-nodes <N>]
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <id> <value> [--computation-nodes <N>] "
                  << "[--schema-id <schema_id>] [--type-proof-system <system>] [--typed-json <json>]\n"
                  << "  <id>                  provider identifier (alphanumeric, max 32 chars)\n"
                  << "  <value>               integer value to share (max 64 chars)\n"
                  << "  --computation-nodes N  number of semi2k computation nodes (default: 3)\n";
        return 1;
    }

    const std::string id    = argv[1];
    const std::string value = argv[2];

    // Parse optional flags.
    int n_parties = 3;  // default
    std::string schema_id = "semi2k-wire-v1";
    std::string type_proof_system = "stub-typeproof-v1";
    std::string type_proof_vk_id = "stub-vk-v1";
    std::optional<std::string> typed_json_opt;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--computation-nodes" && i + 1 < argc) {
            try {
                n_parties = std::stoi(argv[++i]);
                if (n_parties < 2) {
                    std::cerr << "--computation-nodes must be >= 2\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Invalid --computation-nodes value\n";
                return 1;
            }
        }
        if (std::string(argv[i]) == "--schema-id" && i + 1 < argc) {
            schema_id = argv[++i];
        }
        if (std::string(argv[i]) == "--type-proof-system" && i + 1 < argc) {
            type_proof_system = argv[++i];
            if (type_proof_system == "semantic-schema-v1") {
                type_proof_vk_id = "type-registry-v1";
            }
        }
        if (std::string(argv[i]) == "--typed-json" && i + 1 < argc) {
            typed_json_opt = std::string(argv[++i]);
        }
    }

    g_logger.info("=== Data Provider " + id + " starting ===");

    // --- Validate inputs ---
    if (!validate_id(id)) {
        g_logger.error("Invalid ID '" + id + "' (alphanumeric + _/-, max 32 chars)");
        return 1;
    }
    if (!validate_value(value)) {
        g_logger.error("Invalid value '" + value + "' (integer, max 64 chars)");
        return 1;
    }

    // --- Initialise libsodium ---
    if (sodium_init() < 0) {
        g_logger.error("Failed to initialise libsodium");
        return 1;
    }

    // --- Read auth secret from environment (used for BLAKE2b proof) ---
    const char* env_secret = std::getenv("MPC_PROVIDER_SECRET");
    const std::string auth_secret = (env_secret && *env_secret)
                                    ? std::string(env_secret)
                                    : "mpc-demo-secret";
    if (auth_secret == "mpc-demo-secret")
        g_logger.warn("Using default auth secret — set MPC_PROVIDER_SECRET for production");

    // --- Write the provider input file ---
    const int result = write_provider_file(id, value, auth_secret, n_parties,
                                           schema_id, type_proof_system, type_proof_vk_id,
                                           typed_json_opt);

    if (result == 0)
        g_logger.info("=== Provider " + id + " finished successfully ===");
    else
        g_logger.error("=== Provider " + id + " failed ===");

    return result;
}
