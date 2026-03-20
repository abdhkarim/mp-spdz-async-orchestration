#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using boost::multiprecision::cpp_int;

// Entrée validée d'un provider retenu pour le core set.
struct ProviderInput {
    int id = -1;
    long long value = 0;
    std::string masked_value_str;  // Pour les valeurs masquées
};

struct BridgeConfig {
    std::string backend_name = "player-online";
    fs::path program_path;
    int computation_nodes = -1;  // -1 => use selected providers count
};

// Famille de backend MP-SPDZ — détermine la compilation et le runtime.
enum class BackendFamily {
    Ring2k,           // semi2k : anneau Z / 2^k
    Field,            // semi   : corps premier
    OnlineField,      // player-online : corps premier avec pre-processing
    Shamir,           // shamir : secret sharing de Shamir
    ReplicatedRing,   // replicated-ring : partage répliqué sur anneau
    ReplicatedField,  // replicated-field : partage répliqué sur corps
    Specialized
};

// Descripteur d'un backend : métadonnées + pointeurs vers les fonctions propres
// au backend (construction de commande, parsing du résultat).
struct BackendAdapter {
    std::string name;         // Identifiant textuel
    std::string binary_name;  // Nom de l'exécutable MP-SPDZ
    BackendFamily family = BackendFamily::Specialized;

    bool requires_preprocessing = false;  // Nécessite Fake-Offline.x
    bool requires_ssl           = false;  // Nécessite des certificats SSL
    int  min_parties            = 2;      // Nombre minimum de parties

    // Construit la commande shell pour lancer une partie.
    std::string (*build_command)(
        const fs::path& mp_spdz_root,
        const fs::path& backend_binary,
        const std::string& compiled_program_name,
        int party, int n_parties, int port_base,
        const fs::path& log_path);

    // Extrait le résultat depuis le log de la partie 0.
    std::optional<std::string> (*parse_result)(const fs::path& log_path);
};

// Protège une chaîne pour un usage sûr dans une commande shell.
std::string quote_shell(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// Parse strict d'entier ; rejette les chaînes partiellement numériques.
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

std::optional<cpp_int> parse_cpp_int(const std::string& s) {
    // Rejette une chaîne vide.
    if (s.empty()) {
        return std::nullopt;
    }

    // Autorise un signe optionnel au début (+ ou -).
    size_t start = 0;
    if (s[0] == '+' || s[0] == '-') {
        start = 1;
    }

    // Si la chaîne est uniquement un signe, c'est invalide.
    if (start >= s.size()) {
        return std::nullopt;
    }

    // Vérifie que tous les caractères restants sont des chiffres.
    for (size_t i = start; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return std::nullopt;
        }
    }

    // Conversion en grand entier multiprécision.
    // En cas d'échec, on renvoie std::nullopt.
    try {
        return cpp_int(s);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> make_additive_shares(const std::string& secret, int n_parties) {
    // Résultat: une part additive par partie, au format texte.
    std::vector<std::string> shares;
    shares.reserve(static_cast<size_t>(n_parties));

    // Parse du secret source en entier multiprécision.
    const auto secret_int = parse_cpp_int(secret);
    if (!secret_int) {
        return shares;
    }

    // Générateur pseudo-aléatoire pour les n_parties - 1 premières parts.
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<long long> dist(-(1LL << 60), (1LL << 60));

    // Somme partielle des parts déjà tirées aléatoirement.
    cpp_int partial_sum = 0;

    // On tire n_parties - 1 parts aléatoires.
    // Ces parts peuvent être négatives, ce qui est acceptable en partage additif.
    for (int i = 0; i < n_parties - 1; ++i) {
        const long long r = dist(gen);
        shares.push_back(std::to_string(r));
        partial_sum += r;
    }

    // Dernière part calculée pour garantir:
    // sum(shares) == secret_int
    const cpp_int last = *secret_int - partial_sum;
    shares.push_back(last.convert_to<std::string>());

    // Exemple (3 parties):
    //  s = a0 + a1 + a2
    //  a0,a1 aléatoires ; a2 = s - (a0 + a1)
    return shares;
}

// Parse strict d'un fichier provider (même format que dans consensus).
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
    if (!std::getline(in, line1) || !std::getline(in, line2)) {
        return std::nullopt;
    }
    // Compatibilité :
    // - ancien format: 2 lignes (id, value)
    // - nouveau format: 4 lignes (id, value, nonce, proof)
    const bool has_line3 = static_cast<bool>(std::getline(in, line3));
    const bool has_line4 = has_line3 && static_cast<bool>(std::getline(in, line4));
    if (has_line3 != has_line4) {
        return std::nullopt;
    }
    if (std::getline(in, extra)) {
        return std::nullopt;
    }

    const std::string id_prefix = "id=";
    const std::string value_prefix = "value=";
    const std::string masked_value_prefix = "masked_value=";
    
    // Support both old format (value=) and new format (masked_value=)
    bool is_masked = (line2.rfind(masked_value_prefix, 0) == 0);
    bool is_plain = (line2.rfind(value_prefix, 0) == 0);
    
    if (line1.rfind(id_prefix, 0) != 0 || (!is_masked && !is_plain)) {
        return std::nullopt;
    }

    const auto id = parse_integer(line1.substr(id_prefix.size()));
    
    // Extract value based on detected format
    size_t value_offset = is_masked ? masked_value_prefix.size() : value_prefix.size();
    std::string value_str = line2.substr(value_offset);
    
    std::optional<long long> value;
    if (is_masked) {
        // Pour les valeurs masquées, on garde la valeur masquée telle quelle
        // L'extraction x = (x-s) + s sera faite par MP-SPDZ
        value = 0;  // Valeur dummy, on n'en a pas besoin
    } else {
        value = parse_integer(value_str);
    }
    
    if (!id || !value) {
        return std::nullopt;
    }

    if (has_line3) {
        const std::string nonce_prefix = "nonce=";
        const std::string proof_prefix = "proof=";
        if (line3.rfind(nonce_prefix, 0) != 0 || line4.rfind(proof_prefix, 0) != 0) {
            return std::nullopt;
        }
    }

    ProviderInput parsed;
    parsed.id = static_cast<int>(*id);
    parsed.value = *value;
    if (is_masked) {
        parsed.masked_value_str = value_str;
    }
    return parsed;
}

// Exécute une commande shell et normalise son code de retour.
int run_shell_command(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc == -1) {
        return -1;
    }
#ifdef WEXITSTATUS
    if (WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
#endif
    return rc;
}

std::optional<BridgeConfig> parse_bridge_args(int argc, char** argv, const fs::path& root) {
    BridgeConfig config;
    config.program_path = root / "programs" / "sum.mpc";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--backend") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --backend\n";
                return std::nullopt;
            }
            config.backend_name = argv[++i];
            continue;
        }

        if (arg == "--computation-nodes") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --computation-nodes\n";
                return std::nullopt;
            }
            const auto parsed_nodes = parse_integer(argv[++i]);
            if (!parsed_nodes || *parsed_nodes <= 0) {
                std::cerr << "Invalid value for --computation-nodes\n";
                return std::nullopt;
            }
            config.computation_nodes = static_cast<int>(*parsed_nodes);
            continue;
        }

        fs::path candidate = arg;
        config.program_path = candidate.is_absolute() ? candidate : (root / candidate);
    }

    config.program_path = fs::absolute(config.program_path);
    return config;
}

// =============================================================================
// Constructeurs de commandes backend (un par famille)
// =============================================================================

// player-online : numéro de partie en premier argument positionnel.
std::string build_command_player_online(
    const fs::path& mp_spdz_root, const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party, int n_parties, int port_base, const fs::path& log_path)
{
    return "cd " + quote_shell(mp_spdz_root.string()) + " && " +
           quote_shell(backend_binary.string()) + " " + std::to_string(party) +
           " " + compiled_program_name +
           " -N " + std::to_string(n_parties) +
           " -pn " + std::to_string(port_base) +
           " -h localhost > " + quote_shell(log_path.string()) + " 2>&1";
}

// Backends avec -p et -N (semi2k, semi).
std::string build_command_party_style_with_N(
    const fs::path& mp_spdz_root, const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party, int n_parties, int port_base, const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";
    return "cd " + quote_shell(mp_spdz_root.string()) + " && " +
           quote_shell(backend_binary.string()) +
           " -p " + std::to_string(party) +
           " -N " + std::to_string(n_parties) +
           " -pn " + std::to_string(port_base) +
           host_arg + " " + compiled_program_name +
           " > " + quote_shell(log_path.string()) + " 2>&1";
}

// Backends sans -N (replicated-ring, replicated-field).
std::string build_command_party_style_no_N(
    const fs::path& mp_spdz_root, const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party, int /*n_parties*/, int port_base, const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";
    return "cd " + quote_shell(mp_spdz_root.string()) + " && " +
           quote_shell(backend_binary.string()) +
           " -p " + std::to_string(party) +
           " -pn " + std::to_string(port_base) +
           host_arg + " " + compiled_program_name +
           " > " + quote_shell(log_path.string()) + " 2>&1";
}

// Shamir (même format que no_N, mais famille distincte).
std::string build_command_shamir(
    const fs::path& mp_spdz_root, const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party, int /*n_parties*/, int port_base, const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";
    return "cd " + quote_shell(mp_spdz_root.string()) + " && " +
           quote_shell(backend_binary.string()) +
           " -p " + std::to_string(party) +
           " -pn " + std::to_string(port_base) +
           host_arg + " " + compiled_program_name +
           " > " + quote_shell(log_path.string()) + " 2>&1";
}

// =============================================================================
// Registre des backends (repris de dev et étendu)
// =============================================================================

// Retourne le nom textuel d'une famille de backend.
std::string backend_family_name(BackendFamily family) {
    switch (family) {
        case BackendFamily::Ring2k:          return "ring-2k";
        case BackendFamily::Field:           return "field";
        case BackendFamily::OnlineField:     return "online-field";
        case BackendFamily::Shamir:          return "shamir";
        case BackendFamily::ReplicatedRing:  return "replicated-ring";
        case BackendFamily::ReplicatedField: return "replicated-field";
        case BackendFamily::Specialized:     return "specialized";
    }
    return "unknown";
}

// Retourne l'extrait de résultat (SUM= ou RESULT=) de la partie 0.
std::optional<std::string> parse_result_from_log(const fs::path& log_path);  // forward decl

// Registre statique de tous les backends supportés.
const std::vector<BackendAdapter>& backend_registry() {
    static const std::vector<BackendAdapter> backends = {
        { "player-online",    "Player-Online.x",            BackendFamily::OnlineField,      true,  true,  2, build_command_player_online,      parse_result_from_log },
        { "semi2k",           "semi2k-party.x",             BackendFamily::Ring2k,           false, false, 2, build_command_party_style_with_N, parse_result_from_log },
        { "semi",             "semi-party.x",               BackendFamily::Field,            false, false, 2, build_command_party_style_with_N, parse_result_from_log },
        { "shamir",           "shamir-party.x",             BackendFamily::Shamir,           false, true,  3, build_command_shamir,             parse_result_from_log },
        { "replicated-ring",  "replicated-ring-party.x",    BackendFamily::ReplicatedRing,   false, true,  3, build_command_party_style_no_N,   parse_result_from_log },
        { "replicated-field", "replicated-field-party.x",   BackendFamily::ReplicatedField,  false, true,  3, build_command_party_style_no_N,   parse_result_from_log },
    };
    return backends;
}

// Cherche un backend par nom dans le registre. Retourne nullptr si introuvable.
const BackendAdapter* find_backend_adapter(const std::string& name) {
    for (const auto& b : backend_registry())
        if (b.name == name) return &b;
    return nullptr;
}

// Affiche la liste des backends supportés sur stderr.
void print_supported_backends() {
    std::cerr << "Supported backends:\n";
    for (const auto& b : backend_registry()) {
        std::cerr << "  - " << b.name
                  << " [family="        << backend_family_name(b.family)
                  << ", preprocessing=" << (b.requires_preprocessing ? "yes" : "no")
                  << ", ssl="           << (b.requires_ssl           ? "yes" : "no")
                  << ", min_parties="   << b.min_parties
                  << "]\n";
    }
}

// Retourne le chemin de l'exécutable d'un backend.
fs::path get_backend_binary(const fs::path& mp_spdz_root, const BackendAdapter& backend) {
    return mp_spdz_root / backend.binary_name;
}

// Retourne les flags de compilation additionnels.
// Les backends sur anneau 2^k nécessitent -R 64 pour un registre 64 bits.
std::string backend_compile_flags(const BackendAdapter& backend) {
    if (backend.family == BackendFamily::Ring2k ||
        backend.family == BackendFamily::ReplicatedRing) {
        return "-R 64";
    }
    return "";
}

// Vérifie que le nombre de parties respecte les contraintes du backend.
bool validate_backend_party_count(const BackendAdapter& backend, int n_parties) {
    if (n_parties < 2) {
        std::cerr << "At least 2 computation nodes are required.\n";
        return false;
    }
    if (n_parties < backend.min_parties) {
        std::cerr << "Backend " << backend.name
                  << " requires at least " << backend.min_parties
                  << " parties, but " << n_parties << " requested.\n";
        return false;
    }
    return true;
}

bool has_preprocessing_data(const fs::path& mp_spdz_root, int n_parties) {
    const fs::path params_file =
        mp_spdz_root / "Player-Data" /
        (std::to_string(n_parties) + "-p-128") /
        "Params-Data";
    return fs::exists(params_file);
}

int ensure_preprocessing_data(const fs::path& mp_spdz_root, int n_parties) {
    if (has_preprocessing_data(mp_spdz_root, n_parties)) {
        return 0;
    }

    const fs::path fake_offline_binary = mp_spdz_root / "Fake-Offline.x";
    if (!fs::exists(fake_offline_binary)) {
        std::cerr << "Missing Fake-Offline.x; cannot generate preprocessing data for "
                  << n_parties << " parties\n";
        return 1;
    }

    // Prime par défaut suggéré par MP-SPDZ pour 128-bit prime field.
    const std::string default_prime = "170141183460469231731687303715885907969";
    const std::string cmd =
        "cd " + quote_shell(mp_spdz_root.string()) + " && " +
        quote_shell(fake_offline_binary.string()) + " " +
        std::to_string(n_parties) + " -P " + default_prime;

    std::cout << "Generating MP-SPDZ preprocessing for " << n_parties
              << " parties...\n";
    const int rc = run_shell_command(cmd);
    if (rc != 0) {
        std::cerr << "Failed to generate preprocessing data (Fake-Offline.x exit code "
                  << rc << ")\n";
        return rc;
    }
    return 0;
}

// =============================================================================
// Gestion SSL (reprise de la branche dev)
// =============================================================================

// Supprime les anciens fichiers SSL (.pem, .key) dans Player-Data/.
bool delete_old_ssl_material(const fs::path& mp_spdz_root) {
    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    if (!fs::exists(player_data_dir)) return true;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(player_data_dir, ec)) {
        if (ec) { std::cerr << "Failed to iterate Player-Data for SSL cleanup.\n"; return false; }
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext == ".pem" || ext == ".key") {
            std::error_code remove_ec;
            fs::remove(entry.path(), remove_ec);
            if (remove_ec) {
                std::cerr << "Failed to remove old SSL file: " << entry.path() << "\n";
                return false;
            }
        }
    }
    return true;
}

// Régénère les certificats SSL pour n_parties via Scripts/setup-ssl.sh.
bool regenerate_ssl_material(const fs::path& mp_spdz_root, int n_parties) {
    if (!delete_old_ssl_material(mp_spdz_root)) return false;
    const std::string cmd =
        "cd " + quote_shell(mp_spdz_root.string()) +
        " && Scripts/setup-ssl.sh " + std::to_string(n_parties);
    std::cout << "Regenerating SSL certificates for " << n_parties << " parties...\n";
    return run_shell_command(cmd) == 0;
}

// Prépare l'environnement complet d'un backend (SSL + pre-processing).
bool ensure_backend_environment(
    const BackendAdapter& backend,
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    int n_parties)
{
    // Vérifie que l'exécutable backend existe.
    if (!fs::exists(backend_binary)) {
        std::cerr << "Backend runtime not found: " << backend_binary << "\n";
        return false;
    }
    // Backends SSL : regénère les certificats.
    if (backend.requires_ssl) {
        if (!regenerate_ssl_material(mp_spdz_root, n_parties)) {
            std::cerr << "Failed to regenerate SSL material for " << backend.name << "\n";
            return false;
        }
    }
    // Backends avec pre-processing : génère via Fake-Offline.x.
    if (backend.requires_preprocessing) {
        if (ensure_preprocessing_data(mp_spdz_root, n_parties) != 0) {
            std::cerr << "Failed to generate preprocessing for " << backend.name << "\n";
            return false;
        }
    }
    return true;
}

// =============================================================================
// Helpers de compilation et d'exécution (repris de dev)
// =============================================================================

// Compile un programme .mpc avec paramètres et flags optionnels.
// Nom résultant : <stem>-<n_parties>-<n_selected>
bool compile_program(
    const fs::path& mp_spdz_root,
    const fs::path& program_path,
    int n_parties, int n_selected,
    const std::string& extra_flags)
{
    const std::string compile_cmd =
        "cd " + quote_shell(mp_spdz_root.string()) + " && python3 compile.py " +
        (extra_flags.empty() ? "" : (extra_flags + " ")) +
        quote_shell(program_path.string()) +
        " " + std::to_string(n_parties) +
        " " + std::to_string(n_selected);
    std::cout << "Compiling " << program_path.filename().string() << " with MP-SPDZ...\n";
    return run_shell_command(compile_cmd) == 0;
}

// Lance toutes les parties d'un backend en parallèle et attend leur terminaison.
bool run_backend_parties(
    const BackendAdapter& backend,
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int n_parties, int port_base,
    const fs::path& logs_dir)
{
    std::vector<std::future<int>> jobs;
    jobs.reserve(static_cast<size_t>(n_parties));
    for (int party = 0; party < n_parties; ++party) {
        // Chaque partie MP-SPDZ tourne en parallèle, avec un log dédié.
        const fs::path log_path = logs_dir / ("player_" + std::to_string(party) + ".log");
        const std::string cmd = backend.build_command(
            mp_spdz_root, backend_binary, compiled_program_name,
            party, n_parties, port_base, log_path);
        jobs.push_back(std::async(std::launch::async, [cmd]() { return run_shell_command(cmd); }));
    }
    bool all_ok = true;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const int rc = jobs[i].get();
        if (rc != 0) {
            std::cerr << "Party " << i << " exited with code " << rc
                      << " (see logs/player_" << i << ".log)\n";
            all_ok = false;
        }
    }
    return all_ok;
}

// Lit core_set.txt (un identifiant par ligne), puis trie/déduplique.
std::vector<int> read_core_set(const fs::path& core_set_path) {
    std::vector<int> ids;
    std::ifstream in(core_set_path);
    if (!in.is_open()) {
        return ids;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto id = parse_integer(line);
        if (id) {
            ids.push_back(static_cast<int>(*id));
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Charge et valide les fichiers de tous les providers du core set.
std::optional<std::vector<ProviderInput>> load_selected_inputs(
    const fs::path& inputs_dir,
    const std::vector<int>& core_set)
{
    std::vector<ProviderInput> selected;
    selected.reserve(core_set.size());
    for (const int id : core_set) {
        const fs::path provider_file = inputs_dir / ("provider_" + std::to_string(id) + ".txt");
        const auto parsed = parse_provider_file(provider_file);
        if (!parsed || parsed->id != id) {
            std::cerr << "Missing or malformed provider file for selected id " << id << "\n";
            return std::nullopt;
        }
        selected.push_back(*parsed);
    }
    return selected;
}

fs::path provider_secret_file_path(const fs::path& root, int provider_id) {
    return root / "provider_secrets" / ("provider_" + std::to_string(provider_id) + ".secret");
}

std::unordered_map<int, std::string> read_provider_secrets(const fs::path& root) {
    std::unordered_map<int, std::string> secrets;
    const fs::path secrets_dir = root / "provider_secrets";
    if (!fs::exists(secrets_dir) || !fs::is_directory(secrets_dir)) {
        return secrets;
    }

    for (const auto& entry : fs::directory_iterator(secrets_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (filename.rfind("provider_", 0) != 0 || filename.size() <= 16 ||
            filename.substr(filename.size() - 7) != ".secret") {
            continue;
        }

        const std::string id_str = filename.substr(9, filename.size() - 16);
        const auto id = parse_integer(id_str);
        if (!id) {
            continue;
        }

        std::ifstream in(entry.path());
        std::string secret;
        if (!in.is_open() || !std::getline(in, secret)) {
            continue;
        }

        if (!id || secret.empty()) {
            continue;
        }

        secret.erase(secret.begin(), std::find_if(secret.begin(), secret.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        secret.erase(std::find_if(secret.rbegin(), secret.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), secret.end());

        if (!secret.empty()) {
            secrets[static_cast<int>(*id)] = secret;
        }
    }

    return secrets;
}

int issue_provider_secrets(const fs::path& root, int max_provider_id) {
    const fs::path mp_spdz_root = root / "third_party" / "MP-SPDZ";
    const fs::path issue_program = root / "programs" / "issue_secrets.mpc";
    const fs::path secrets_dir = root / "provider_secrets";

    fs::create_directories(secrets_dir);

    {
        bool complete = true;
        for (int id = 1; id <= max_provider_id; ++id) {
            const fs::path secret_path = provider_secret_file_path(root, id);
            if (!fs::exists(secret_path)) {
                complete = false;
                break;
            }
            std::ifstream in(secret_path);
            std::string line;
            if (!in.is_open() || !std::getline(in, line) || line.empty()) {
                complete = false;
                break;
            }
        }
        if (complete) {
            return 0;
        }
    }

    if (!fs::exists(issue_program)) {
        std::cerr << "Missing MPC issuer program: " << issue_program << "\n";
        return 1;
    }

    const int issuance_parties = []() {
        const char* env = std::getenv("MPC_ISSUANCE_PARTIES");
        if (!env || !*env) {
            return 3;
        }
        try {
            const int v = std::stoi(env);
            return (v > 0) ? v : 3;
        } catch (...) {
            return 3;
        }
    }();

    const std::string compiled_program_name = "issue_secrets-" + std::to_string(max_provider_id);
    const std::string compile_cmd =
        "cd " + quote_shell(mp_spdz_root.string()) + " && python3 compile.py " +
        quote_shell(issue_program.string()) + " " + std::to_string(max_provider_id);

    std::cout << "Compiling issue_secrets.mpc with MP-SPDZ...\n";
    if (run_shell_command(compile_cmd) != 0) {
        std::cerr << "Failed to compile issue_secrets.mpc\n";
        return 1;
    }

    if (ensure_preprocessing_data(mp_spdz_root, issuance_parties) != 0) {
        std::cerr << "Failed to prepare preprocessing data for issuance phase\n";
        return 1;
    }

    const fs::path player_online_binary = mp_spdz_root / "Player-Online.x";
    if (!fs::exists(player_online_binary)) {
        std::cerr << "Player-Online.x not found; cannot issue provider secrets with MP-SPDZ\n";
        return 1;
    }

    const fs::path logs_dir = root / "logs";
    fs::create_directories(logs_dir);
    const int port_base = 15100;

    std::vector<std::future<int>> jobs;
    jobs.reserve(static_cast<size_t>(issuance_parties));
    for (int party = 0; party < issuance_parties; ++party) {
        const fs::path log_path = logs_dir / ("issuer_player_" + std::to_string(party) + ".log");
        const std::string cmd =
            "cd " + quote_shell(mp_spdz_root.string()) + " && " +
            quote_shell(player_online_binary.string()) + " " + std::to_string(party) +
            " " + compiled_program_name + " -N " + std::to_string(issuance_parties) +
            " -pn " + std::to_string(port_base) +
            " -h localhost > " + quote_shell(log_path.string()) + " 2>&1";

        jobs.push_back(std::async(std::launch::async, [cmd]() { return run_shell_command(cmd); }));
    }

    bool all_ok = true;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const int rc = jobs[i].get();
        if (rc != 0) {
            std::cerr << "Secret issuer party " << i << " exited with code " << rc
                      << " (see logs/issuer_player_" << i << ".log)\n";
            all_ok = false;
        }
    }

    if (!all_ok) {
        return 1;
    }

    std::unordered_map<int, std::string> issued;
    {
        std::ifstream in(logs_dir / "issuer_player_0.log");
        if (!in.is_open()) {
            std::cerr << "Cannot open issuer log for parsing\n";
            return 1;
        }

        std::string line;
        std::regex secret_regex(R"(PROVIDER_SECRET_(\d+)=([-]?\d+))");
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_search(line, m, secret_regex)) {
                const auto id_opt = parse_integer(m[1].str());
                if (id_opt) {
                    issued[static_cast<int>(*id_opt)] = m[2].str();
                }
            }
        }
    }

    if (static_cast<int>(issued.size()) < max_provider_id) {
        std::cerr << "MP-SPDZ did not emit all provider secrets (got " << issued.size()
                  << ", expected " << max_provider_id << ")\n";
        return 1;
    }

    for (int id = 1; id <= max_provider_id; ++id) {
        const fs::path secret_path = provider_secret_file_path(root, id);
        std::ofstream out(secret_path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "Failed to write provider secret file: " << secret_path << "\n";
            return 1;
        }
        out << issued[id] << "\n";
        out.close();

        std::error_code ec;
        fs::permissions(secret_path,
                        fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace,
                        ec);
    }

    std::cout << "Issued per-provider masking secrets with MP-SPDZ in " << secrets_dir
              << " for provider ids 1.." << max_provider_id << "\n";
    return 0;
}

// =============================================================================
// Préparation sécurisée des entrées MP-SPDZ
// =============================================================================

// Prépare les entrées pour MP-SPDZ selon le protocole de partage additif :
//   1. Écrit les valeurs masquées (x−s) dans Player-Data/Public-Masked-Values
//   2. Pour chaque partie p, écrit ses parts de secret dans Player-Data/Input-Pp-0
//
// Ce flux garantit qu'aucune partie ne voit la valeur brute d'un provider.
bool prepare_secure_inputs(
    const fs::path& mp_spdz_root,
    const std::vector<ProviderInput>& selected,
    const std::unordered_map<int, std::string>& provider_secrets,
    int n_parties,
    cpp_int& fallback_sum_out)
{
    if (n_parties <= 0) {
        std::cerr << "Secure input mapping requires a strictly positive number of computation nodes\n";
        return false;
    }

    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    fs::create_directories(player_data_dir);

    // Collecte les valeurs masquées et calcule la somme de fallback.
    std::vector<std::string> masked_values;
    masked_values.reserve(selected.size());
    fallback_sum_out = 0;

    for (const auto& provider : selected) {
        masked_values.push_back(
            provider.masked_value_str.empty()
                ? std::to_string(provider.value)
                : provider.masked_value_str);

        const auto it = provider_secrets.find(provider.id);
        if (it == provider_secrets.end()) {
            std::cerr << "Missing issued secret for provider id " << provider.id << "\n";
            return false;
        }
        if (!provider.masked_value_str.empty()) {
            // fallback = (x−s) + s = x
            const auto mi = parse_cpp_int(provider.masked_value_str);
            const auto si = parse_cpp_int(it->second);
            if (mi && si) fallback_sum_out += (*mi + *si);
        } else {
            fallback_sum_out += provider.value;
        }
    }

    // Matrice party_secret_shares[partie][provider] : share de s_i pour la partie p.
    std::vector<std::vector<std::string>> party_secret_shares(
        static_cast<size_t>(n_parties), std::vector<std::string>(selected.size()));

    for (size_t pi = 0; pi < selected.size(); ++pi) {
        const int prov_id = selected[pi].id;
        const auto sit = provider_secrets.find(prov_id);
        if (sit == provider_secrets.end()) {
            std::cerr << "Missing issued secret for provider " << prov_id << "\n";
            return false;
        }
        // Découpe s_i en n parts additives : s_i = a0 + ... + a_{n-1}
        const auto shares = make_additive_shares(sit->second, n_parties);
        if (static_cast<int>(shares.size()) != n_parties) {
            std::cerr << "Failed to generate additive shares for provider " << prov_id << "\n";
            return false;
        }
        for (int party = 0; party < n_parties; ++party)
            party_secret_shares[static_cast<size_t>(party)][pi] = shares[static_cast<size_t>(party)];
    }

    // Écrit les valeurs masquées publiques dans Public-Masked-Values.
    {
        std::ofstream out(player_data_dir / "Public-Masked-Values");
        if (!out.is_open()) {
            std::cerr << "Failed to write Public-Masked-Values\n";
            return false;
        }
        for (const auto& mv : masked_values) out << mv << "\n";
    }

    // Écrit les parts secrètes de chaque partie dans Input-Pp-0.
    // La partie p reçoit [share(p, s_0), ..., share(p, s_{n-1})].
    for (int party = 0; party < n_parties; ++party) {
        const fs::path input_file = player_data_dir / ("Input-P" + std::to_string(party) + "-0");
        std::ofstream out(input_file);
        if (!out.is_open()) {
            std::cerr << "Failed to write MP-SPDZ input file: " << input_file << "\n";
            return false;
        }
        for (size_t pi = 0; pi < selected.size(); ++pi)
            out << party_secret_shares[static_cast<size_t>(party)][pi] << "\n";
    }

    return true;
}

// Extrait "SUM=<valeur>" ou "RESULT=<...>" depuis un log MP-SPDZ.
// Cette fonction sert aussi de parse_result dans les BackendAdapter.
std::optional<std::string> parse_result_from_log(const fs::path& log_path) {
    std::ifstream in(log_path);
    if (!in.is_open()) return std::nullopt;
    std::string line;
    std::regex sum_regex(R"(SUM=([-]?\d+))");
    std::regex result_regex(R"(RESULT=(.*))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, sum_regex))    return m[1].str();
        if (std::regex_search(line, m, result_regex)) return m[1].str();
    }
    return std::nullopt;
}

int main(int argc, char** argv) {
    // ── Racines de travail ────────────────────────────────────────────────────
    const fs::path root          = fs::current_path();
    const fs::path core_set_path = root / "core_set.txt";
    const fs::path inputs_dir    = root / "inputs";
    const fs::path mp_spdz_root  = root / "third_party" / "MP-SPDZ";
    const fs::path logs_dir      = root / "logs";
    fs::create_directories(logs_dir);

    // ── Parsing des arguments ─────────────────────────────────────────────────
    const auto bridge_config_opt = parse_bridge_args(argc, argv, root);
    if (!bridge_config_opt) {
        std::cerr << "Usage: ./spdz_bridge [--backend <name>] [--computation-nodes N] [program_path]\n";
        print_supported_backends();
        return 1;
    }
    const BridgeConfig& config = *bridge_config_opt;

    // ── Dispatch vers l'adaptateur backend ───────────────────────────────────
    const BackendAdapter* backend = find_backend_adapter(config.backend_name);
    if (!backend) {
        std::cerr << "Unknown backend: " << config.backend_name << "\n";
        print_supported_backends();
        return 1;
    }

    std::cout << "Backend        : " << config.backend_name                           << "\n"
              << "Family         : " << backend_family_name(backend->family)          << "\n"
              << "Preprocessing  : " << (backend->requires_preprocessing ? "yes" : "no") << "\n"
              << "SSL            : " << (backend->requires_ssl ? "yes" : "no")           << "\n"
              << "Min parties    : " << backend->min_parties                          << "\n";

    // ── Gestion des secrets de providers ─────────────────────────────────────
    const int max_provider_id = []() {
        const char* env = std::getenv("MPC_MAX_PROVIDER_ID");
        if (!env || !*env) return 32;
        try {
            const int v = std::stoi(env);
            return (v > 0) ? v : 32;
        } catch (...) { return 32; }
    }();

    if (issue_provider_secrets(root, max_provider_id) != 0) return 1;
    const auto provider_secrets = read_provider_secrets(root);

    // ── Lecture du core set ───────────────────────────────────────────────────
    const auto core_set = read_core_set(core_set_path);
    if (core_set.empty()) {
        std::cout << "No providers in core set. Secrets are issued and ready in provider_secrets/.\n";
        return 0;
    }

    // ── Chargement des entrées sélectionnées ──────────────────────────────────
    const auto selected_opt = load_selected_inputs(inputs_dir, core_set);
    if (!selected_opt) return 1;
    const std::vector<ProviderInput>& selected = *selected_opt;

    const int n_parties =
        (config.computation_nodes > 0)
            ? config.computation_nodes
            : static_cast<int>(selected.size());

    if (!validate_backend_party_count(*backend, n_parties)) return 1;

    // ── Préparation des entrées sécurisées (shares + masked values) ───────────
    cpp_int fallback_sum = 0;
    if (!prepare_secure_inputs(mp_spdz_root, selected, provider_secrets, n_parties, fallback_sum))
        return 1;

    // ── Vérification du binaire backend ──────────────────────────────────────
    const fs::path backend_binary = get_backend_binary(mp_spdz_root, *backend);
    if (!fs::exists(backend_binary)) {
        std::cout << "Backend runtime not found: " << backend_binary << "\n";
        std::cout << "Fallback sum from validated inputs = "
                  << fallback_sum.convert_to<std::string>() << "\n";
        return 0;
    }
    if (!fs::exists(config.program_path)) {
        std::cerr << "Missing program file: " << config.program_path << "\n";
        return 1;
    }

    // ── Environnement backend (SSL, preprocessing) ────────────────────────────
    if (!ensure_backend_environment(*backend, mp_spdz_root, backend_binary, n_parties)) {
        std::cout << "Environment setup failed. Fallback sum = "
                  << fallback_sum.convert_to<std::string>() << "\n";
        return 0;
    }

    // ── Compilation du programme MPC ──────────────────────────────────────────
    const std::string compiled_program_name =
        "sum-" + std::to_string(n_parties) + "-" + std::to_string(selected.size());

    if (!compile_program(mp_spdz_root, config.program_path, n_parties,
                         static_cast<int>(selected.size()),
                         backend_compile_flags(*backend))) {
        std::cout << "MP-SPDZ compilation failed. Fallback sum = "
                  << fallback_sum.convert_to<std::string>() << "\n";
        return 0;
    }

    // ── Exécution des parties MPC ─────────────────────────────────────────────
    const int port_base = 15000;
    const bool run_ok = run_backend_parties(
        *backend, mp_spdz_root, backend_binary,
        compiled_program_name, n_parties, port_base, logs_dir);

    // ── Lecture du résultat ───────────────────────────────────────────────────
    const auto parsed_sum = backend->parse_result(logs_dir / "player_0.log");
    if (parsed_sum) {
        std::cout << "MP-SPDZ result: SUM=" << *parsed_sum << "\n";
    } else {
        std::cout << "Could not parse SUM from MP-SPDZ logs. Fallback sum = "
                  << fallback_sum.convert_to<std::string>() << "\n";
    }

    std::cout << "Per-provider secrets are managed in provider_secrets/ (bridge-issued).\n";

    if (!run_ok) {
        std::cout << "Some MP-SPDZ parties failed; fallback sum from validated inputs = "
                  << fallback_sum.convert_to<std::string>() << "\n";
        return 1;
    }

    return 0;
}