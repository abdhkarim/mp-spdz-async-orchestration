/**
 * data_provider.cpp
 *
 * Role: Each data provider holds a private value x_i. Before sending it to
 * the MPC servers, it masks it as (x_i - s_i), where s_i is a per-provider
 * secret issued by MP-SPDZ (stored in provider_secrets/provider_<id>.secret).
 *
 * The provider also computes a cryptographic proof (BLAKE2b) over the masked
 * value so that the consensus node can verify the data has not been tampered
 * with in transit.
 *
 * Usage: ./data_provider <id> <value> [--malformed]
 *   --malformed : simulate a malicious provider (writes a corrupted file)
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sodium.h>
#include <sstream>
#include <string>

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
// Section 5 – Per-provider secret (s_i)
// =============================================================================
//
// The bridge runs MP-SPDZ to generate one random secret s_i per provider and
// writes them to provider_secrets/provider_<id>.secret (one file per provider).
// This function reads s_i for the given provider ID.

std::optional<std::string> read_provider_secret(const std::string& provider_id) {
    const fs::path secret_file =
        fs::current_path() / "provider_secrets" / ("provider_" + provider_id + ".secret");

    std::ifstream in(secret_file);
    if (!in.is_open()) {
        g_logger.warn("Provider secret file not found at " + secret_file.string() +
                      " — run the SPDZ bridge first to issue secrets");
        return std::nullopt;
    }

    std::string secret;
    if (!std::getline(in, secret)) {
        g_logger.error("Secret for provider " + provider_id + " is missing in " +
                       secret_file.string());
        return std::nullopt;
    }

    const auto not_space = [](unsigned char c){ return !std::isspace(c); };
    secret.erase(secret.begin(),
                 std::find_if(secret.begin(), secret.end(), not_space));
    secret.erase(std::find_if(secret.rbegin(), secret.rend(), not_space).base(),
                 secret.end());

    if (secret.empty()) {
        g_logger.error("Secret for provider " + provider_id + " is empty in " +
                       secret_file.string());
        return std::nullopt;
    }

    return secret;
}


// =============================================================================
// Section 6 – Writing the provider file
// =============================================================================
//
// Creates  build/inputs/provider_<id>.txt  with four fields:
//   id             – provider identity
//   masked_value   – x_i - s_i  (the value masked by the SPDZ-issued secret)
//   nonce          – random 16-byte hex string (replay protection)
//   proof          – BLAKE2b authentication tag

int write_provider_file(const std::string& id,
                        const std::string& value,
                        bool malformed,
                        const std::string& auth_secret) {
    // --- Prepare output path ---
    const fs::path inputs_dir  = fs::current_path() / "inputs";
    fs::create_directories(inputs_dir);
    const fs::path output_file = inputs_dir / ("provider_" + id + ".txt");

    std::ofstream out(output_file);
    if (!out.is_open()) {
        g_logger.error("Cannot open output file: " + output_file.string());
        return 1;
    }

    // --- Malformed mode: simulate a corrupted / attack provider ---
    if (malformed) {
        out << "this_file_is_malformed\n";
        g_logger.warn("Wrote MALFORMED file for provider " + id +
                      " (attack simulation)");
        return 0;
    }

    // --- Compute masked value: x_i - s_i ---
    const auto secret_opt = read_provider_secret(id);
    std::string masked_value = value;   // fallback: send plain value if no secret

    if (secret_opt) {
        try {
            masked_value = subtract_strings(value, *secret_opt);
            g_logger.info("Provider " + id + ": masked " + value +
                          " → " + masked_value + " (x - s_i)");
        } catch (const std::exception& e) {
            g_logger.warn("Masking failed (" + std::string(e.what()) +
                          "), falling back to plain value");
            masked_value = value;
        }
    } else {
        g_logger.info("No secret available for provider " + id +
                      " — using plain value");
    }

    // --- Random nonce for replay protection ---
    unsigned char nonce_raw[16] = {};
    randombytes_buf(nonce_raw, sizeof(nonce_raw));
    const std::string nonce = to_hex(nonce_raw, sizeof(nonce_raw));

    // --- Cryptographic proof over the masked value ---
    const std::string proof = compute_proof(id, masked_value, nonce, auth_secret);

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
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <id> <value> [--malformed]\n"
                  << "  <id>        provider identifier (alphanumeric, max 32 chars)\n"
                  << "  <value>     integer value to share (max 64 chars)\n"
                  << "  --malformed simulate a malicious provider\n";
        return 1;
    }

    const std::string id       = argv[1];
    const std::string value    = argv[2];
    const bool        malformed = (argc == 4 && std::string(argv[3]) == "--malformed");

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
    const int result = write_provider_file(id, value, malformed, auth_secret);

    if (result == 0)
        g_logger.info("=== Provider " + id + " finished successfully ===");
    else
        g_logger.error("=== Provider " + id + " failed ===");

    return result;
}
