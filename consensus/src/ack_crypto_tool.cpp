#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sodium.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

std::optional<std::string> read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::string content;
    if (!std::getline(in, content)) return std::nullopt;
    return content;
}

bool write_text_file(const fs::path& path, const std::string& line) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << line << "\n";
    return out.good();
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium\n";
        return 1;
    }
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  ack_crypto_tool gen-keypair <public_out> <secret_out>\n"
                  << "  ack_crypto_tool sign <secret_key_file> <message>\n"
                  << "  ack_crypto_tool verify <public_key_file> <message> <signature_hex>\n";
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "gen-keypair") {
        if (argc != 4) {
            std::cerr << "Usage: ack_crypto_tool gen-keypair <public_out> <secret_out>\n";
            return 1;
        }
        unsigned char pk[crypto_sign_PUBLICKEYBYTES] = {0};
        unsigned char sk[crypto_sign_SECRETKEYBYTES] = {0};
        crypto_sign_keypair(pk, sk);

        if (!write_text_file(argv[2], to_hex(pk, sizeof(pk))) ||
            !write_text_file(argv[3], to_hex(sk, sizeof(sk)))) {
            std::cerr << "Failed to write key files\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "sign") {
        if (argc != 4) {
            std::cerr << "Usage: ack_crypto_tool sign <secret_key_file> <message>\n";
            return 1;
        }
        const auto sk_hex_opt = read_text_file(argv[2]);
        if (!sk_hex_opt) {
            std::cerr << "Failed to read secret key file\n";
            return 1;
        }
        const auto sk_bytes_opt = from_hex(*sk_hex_opt);
        if (!sk_bytes_opt || sk_bytes_opt->size() != crypto_sign_SECRETKEYBYTES) {
            std::cerr << "Invalid secret key format\n";
            return 1;
        }

        const std::string message = argv[3];
        unsigned char sig[crypto_sign_BYTES] = {0};
        if (crypto_sign_detached(sig, nullptr,
                                 reinterpret_cast<const unsigned char*>(message.data()),
                                 message.size(),
                                 sk_bytes_opt->data()) != 0) {
            std::cerr << "Signing failed\n";
            return 1;
        }
        std::cout << to_hex(sig, sizeof(sig)) << "\n";
        return 0;
    }

    if (cmd == "verify") {
        if (argc != 5) {
            std::cerr << "Usage: ack_crypto_tool verify <public_key_file> <message> <signature_hex>\n";
            return 1;
        }
        const auto pk_hex_opt = read_text_file(argv[2]);
        if (!pk_hex_opt) {
            std::cerr << "Failed to read public key file\n";
            return 1;
        }
        const auto pk_opt = from_hex(*pk_hex_opt);
        const auto sig_opt = from_hex(argv[4]);
        if (!pk_opt || !sig_opt ||
            pk_opt->size() != crypto_sign_PUBLICKEYBYTES ||
            sig_opt->size() != crypto_sign_BYTES) {
            std::cerr << "Invalid public key or signature format\n";
            return 1;
        }
        const std::string message = argv[3];
        const int rc = crypto_sign_verify_detached(
            sig_opt->data(),
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size(),
            pk_opt->data());
        std::cout << (rc == 0 ? "OK" : "FAIL") << "\n";
        return (rc == 0) ? 0 : 2;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    return 1;
}
