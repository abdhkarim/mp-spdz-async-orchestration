#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sodium.h>
#include <string>

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

int main(int argc, char* argv[]) {
    // Mode normal: ./data_provider <id> <value>
    // Mode malveillant simulé: ./data_provider <id> <value> --malformed
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: ./data_provider <id> <value> [--malformed]\n";
        return 1;
    }

    const std::string id = argv[1];
    const std::string value = argv[2];
    const bool malformed = (argc == 4 && std::string(argv[3]) == "--malformed");
    const std::string secret = []() {
        const char* env = std::getenv("MPC_PROVIDER_SECRET");
        if (env && *env) {
            return std::string(env);
        }
        return std::string("mpc-demo-secret");
    }();

    if (sodium_init() < 0) {
        std::cerr << "Failed to initialize libsodium\n";
        return 1;
    }

    // Chaque provider écrit un fichier indépendant ; pas de socket réseau ici.
    fs::path inputs_dir = fs::current_path() / "inputs";
    fs::create_directories(inputs_dir);

    const fs::path output_file = inputs_dir / ("provider_" + id + ".txt");
    std::ofstream out(output_file);
    if (!out.is_open()) {
        std::cerr << "Failed to open output file: " << output_file << "\n";
        return 1;
    }

    if (malformed) {
        // Contenu volontairement invalide pour tester le filtrage consensus.
        out << "this_file_is_malformed\n";
    } else {
        // Format signé attendu par consensus (preuve cryptographique simple).
        // Si besoin, le secret partagé peut être injecté via MPC_PROVIDER_SECRET.
        unsigned char nonce_raw[16] = {0};
        randombytes_buf(nonce_raw, sizeof(nonce_raw));
        const std::string nonce = to_hex(nonce_raw, sizeof(nonce_raw));
        const std::string proof = compute_proof(id, value, nonce, secret);

        out << "id=" << id << "\n";
        out << "value=" << value << "\n";
        out << "nonce=" << nonce << "\n";
        out << "proof=" << proof << "\n";
    }

    std::cout << "Provider " << id << " wrote input file: " << output_file << "\n";
    return 0;
}
