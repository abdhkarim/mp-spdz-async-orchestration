#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sodium.h>
#include <string>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================================
// Logging System
// ============================================================================
enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
   public:
    Logger() = default;

    void set_level(LogLevel level) { min_level = level; }

    void log(LogLevel level, const std::string& msg) {
        if (level < min_level) return;

        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const std::tm* timeinfo = std::localtime(&time);

        std::stringstream ss;
        ss << "[" << std::put_time(timeinfo, "%Y-%m-%d %H:%M:%S") << "] ";

        switch (level) {
            case LogLevel::DEBUG:
                ss << "[DEBUG] ";
                break;
            case LogLevel::INFO:
                ss << "[INFO ] ";
                break;
            case LogLevel::WARN:
                ss << "[WARN ] ";
                break;
            case LogLevel::ERROR:
                ss << "[ERROR] ";
                break;
        }
        ss << msg;

        if (level == LogLevel::ERROR) {
            std::cerr << ss.str() << "\n";
        } else {
            std::cout << ss.str() << "\n";
        }
    }

    void info(const std::string& msg) { log(LogLevel::INFO, msg); }
    void warn(const std::string& msg) { log(LogLevel::WARN, msg); }
    void error(const std::string& msg) { log(LogLevel::ERROR, msg); }
    void debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }

   private:
    LogLevel min_level = LogLevel::INFO;
};

static Logger g_logger;

// ============================================================================
// Utility Functions
// ============================================================================
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

// Validate provider ID: alphanumeric, no path traversal
bool validate_id(const std::string& id) {
    if (id.empty() || id.length() > 32) return false;
    for (char c : id) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

// Validate value: numeric or numeric-like
bool validate_value(const std::string& value) {
    if (value.empty() || value.length() > 64) return false;
    
    // Reject floating point and scientific notation
    for (char c : value) {
        if (c == '.' || c == 'e' || c == 'E') return false;
    }
    
    try {
        std::stoll(value);  // attempts to parse as integer
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Cryptographic Functions
// ============================================================================
std::string compute_proof(const std::string& id,
                          const std::string& value,
                          const std::string& nonce,
                          const std::string& secret) {
    const std::string message = "id=" + id + ";value=" + value + ";nonce=" + nonce;
    unsigned char digest[crypto_generichash_BYTES] = {0};
    crypto_generichash_state state;

    if (crypto_generichash_init(&state,
                                reinterpret_cast<const unsigned char*>(secret.data()),
                                secret.size(),
                                sizeof(digest)) != 0) {
        throw std::runtime_error("crypto_generichash_init failed");
    }

    if (crypto_generichash_update(&state,
                                   reinterpret_cast<const unsigned char*>(message.data()),
                                   message.size()) != 0) {
        throw std::runtime_error("crypto_generichash_update failed");
    }

    if (crypto_generichash_final(&state, digest, sizeof(digest)) != 0) {
        throw std::runtime_error("crypto_generichash_final failed");
    }

    return to_hex(digest, sizeof(digest));
}

// ============================================================================
// Main Provider Logic
// ============================================================================
int write_provider_file(const std::string& id,
                        const std::string& value,
                        bool malformed,
                        const std::string& secret) {
    try {
        // Create inputs directory
        fs::path inputs_dir = fs::current_path() / "inputs";
        fs::create_directories(inputs_dir);
        g_logger.debug("Created/verified inputs directory: " + inputs_dir.string());

        const fs::path output_file = inputs_dir / ("provider_" + id + ".txt");

        // Open output file
        std::ofstream out(output_file);
        if (!out.is_open()) {
            g_logger.error("Failed to open output file: " + output_file.string());
            return 1;
        }
        g_logger.debug("Opened output file: " + output_file.string());

        if (malformed) {
            g_logger.warn("Writing MALFORMED data for provider " + id +
                         " (simulated attack)");
            out << "this_file_is_malformed\n";
        } else {
            // Generate cryptographic proof
            unsigned char nonce_raw[16] = {0};
            randombytes_buf(nonce_raw, sizeof(nonce_raw));
            const std::string nonce = to_hex(nonce_raw, sizeof(nonce_raw));

            g_logger.debug("Generated nonce: " + nonce.substr(0, 8) + "...");

            const std::string proof = compute_proof(id, value, nonce, secret);
            g_logger.debug("Computed cryptographic proof");

            out << "id=" << id << "\n";
            out << "value=" << value << "\n";
            out << "nonce=" << nonce << "\n";
            out << "proof=" << proof << "\n";

            g_logger.info("Valid signature generated for provider " + id +
                         " with value " + value);
        }

        if (!out.good()) {
            g_logger.error("File write error for: " + output_file.string());
            return 1;
        }

        out.close();
        g_logger.info("Provider " + id + " wrote input file: " + output_file.string());
        return 0;

    } catch (const std::exception& e) {
        g_logger.error("Exception in write_provider_file: " + std::string(e.what()));
        return 1;
    }
}

// ============================================================================
// Entry Point
// ============================================================================
int main(int argc, char* argv[]) {
    try {
        // Parse arguments
        if (argc < 3 || argc > 4) {
            std::cerr << "Usage: " << argv[0] << " <id> <value> [--malformed]\n";
            std::cerr << "  <id>    : provider identifier (alphanumeric, max 32 chars)\n";
            std::cerr << "  <value> : numeric value to share (max 64 chars)\n";
            std::cerr << "  --malformed : optional flag to simulate a malicious provider\n";
            return 1;
        }

        const std::string id = argv[1];
        const std::string value = argv[2];
        const bool malformed = (argc == 4 && std::string(argv[3]) == "--malformed");

        g_logger.info("=== Data Provider Initialization ===");
        g_logger.debug("ID: " + id + ", Value: " + value +
                      (malformed ? ", Mode: MALFORMED" : ", Mode: NORMAL"));

        // Validate inputs
        if (!validate_id(id)) {
            g_logger.error("Invalid provider ID: '" + id +
                          "' (must be alphanumeric, 1-32 chars)");
            return 1;
        }

        if (!validate_value(value)) {
            g_logger.error("Invalid value: '" + value +
                          "' (must be numeric, max 64 chars)");
            return 1;
        }

        g_logger.debug("Input validation passed");

        // Initialize libsodium
        if (sodium_init() < 0) {
            g_logger.error("Failed to initialize libsodium");
            return 1;
        }
        g_logger.debug("Libsodium initialized");

        // Get shared secret
        const std::string secret = []() {
            const char* env = std::getenv("MPC_PROVIDER_SECRET");
            if (env && *env) {
                return std::string(env);
            }
            return std::string("mpc-demo-secret");
        }();

        if (std::string(secret) == "mpc-demo-secret") {
            g_logger.warn("Using default secret; set MPC_PROVIDER_SECRET for production");
        } else {
            g_logger.debug("Using custom secret from environment");
        }

        // Write provider file
        int result = write_provider_file(id, value, malformed, secret);
        if (result == 0) {
            g_logger.info("=== Provider execution successful ===\n");
        } else {
            g_logger.error("=== Provider execution failed ===\n");
        }

        return result;

    } catch (const std::exception& e) {
        g_logger.error("Unhandled exception: " + std::string(e.what()));
        return 1;
    }
}
