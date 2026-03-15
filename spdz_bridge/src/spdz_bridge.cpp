#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

// =========================
// Data structures
// =========================

struct ProviderInput {
    int id = -1;
    long long value = 0;
};

struct BridgeConfig {
    std::string backend_name = "semi2k";
    fs::path program_path;
    int computation_nodes = -1; // -1 => use selected providers count
};

enum class BackendFamily {
    Ring2k,
    Field,
    OnlineField,
    Shamir,
    ReplicatedRing,
    ReplicatedField,
    Specialized
};

struct BackendAdapter {
    std::string name;
    std::string binary_name;
    BackendFamily family = BackendFamily::Specialized;

    bool requires_preprocessing = false;
    bool requires_ssl = false;
    bool supports_unencrypted = false;
    bool uses_default_player_data_inputs = true;

    int min_parties = 2;

    bool (*prepare_inputs)(
        const fs::path& mp_spdz_root,
        const std::vector<ProviderInput>& selected,
        int n_parties);

    bool (*check_ready)(
        const fs::path& mp_spdz_root,
        const fs::path& backend_binary);

    std::string (*build_command)(
        const fs::path& mp_spdz_root,
        const fs::path& backend_binary,
        const std::string& compiled_program_name,
        int party,
        int n_parties,
        int port_base,
        const fs::path& log_path);

    std::optional<std::string> (*parse_result)(
        const fs::path& log_path);
};

// =========================
// Generic helpers
// =========================

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

std::string backend_family_name(BackendFamily family) {
    switch (family) {
        case BackendFamily::Ring2k: return "ring-2k";
        case BackendFamily::Field: return "field";
        case BackendFamily::OnlineField: return "online-field";
        case BackendFamily::Shamir: return "shamir";
        case BackendFamily::ReplicatedRing: return "replicated-ring";
        case BackendFamily::ReplicatedField: return "replicated-field";
        case BackendFamily::Specialized: return "specialized";
    }
    return "unknown";
}

// =========================
// Provider input parsing
// =========================

std::optional<ProviderInput> parse_provider_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::nullopt;
    }

    std::string line1, line2, line3, line4, extra;
    if (!std::getline(in, line1) || !std::getline(in, line2)) {
        return std::nullopt;
    }

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

    if (line1.rfind(id_prefix, 0) != 0 || line2.rfind(value_prefix, 0) != 0) {
        return std::nullopt;
    }

    const auto id = parse_integer(line1.substr(id_prefix.size()));
    const auto value = parse_integer(line2.substr(value_prefix.size()));
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
    return parsed;
}

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

// =========================
// Result parsing
// =========================

std::optional<std::string> parse_result_default(const fs::path& log_path) {
    std::ifstream in(log_path);
    if (!in.is_open()) {
        return std::nullopt;
    }

    std::string line;
    std::regex result_regex(R"(RESULT=(.*))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, result_regex)) {
            return m[1].str();
        }
    }

    return std::nullopt;
}

// =========================
// Argument parsing
// =========================

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
        } else if (arg == "--computation-nodes") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --computation-nodes\n";
                return std::nullopt;
            }
            const auto parsed = parse_integer(argv[++i]);
            if (!parsed || *parsed <= 0) {
                std::cerr << "Invalid value for --computation-nodes\n";
                return std::nullopt;
            }
            config.computation_nodes = static_cast<int>(*parsed);
        } else {
            fs::path candidate = arg;
            config.program_path = candidate.is_absolute() ? candidate : (root / candidate);
        }
    }

    config.program_path = fs::absolute(config.program_path);
    return config;
}

// =========================
// Generic backend helpers
// =========================

bool prepare_inputs_default(
    const fs::path& mp_spdz_root,
    const std::vector<ProviderInput>& selected,
    int n_parties)
{
    if (n_parties != static_cast<int>(selected.size())) {
        std::cerr
            << "Current bridge input mode only supports one clear input owner per MPC party.\n"
            << "Secure remapping where #computation_nodes != #selected_data_providers is not\n"
            << "implemented yet because it requires a proper secret-sharing / resharing layer.\n";
        return false;
    }

    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    fs::create_directories(player_data_dir);

    for (size_t party = 0; party < selected.size(); ++party) {
        const fs::path input_file =
            player_data_dir / ("Input-P" + std::to_string(party) + "-0");
        std::ofstream out(input_file);
        if (!out.is_open()) {
            std::cerr << "Failed to write MP-SPDZ input file: " << input_file << "\n";
            return false;
        }
        out << selected[party].value << "\n";
    }

    return true;
}

bool check_ready_default(
    const fs::path& /*mp_spdz_root*/,
    const fs::path& backend_binary)
{
    if (!fs::exists(backend_binary)) {
        std::cerr << "Backend runtime not found: " << backend_binary << "\n";
        return false;
    }
    return true;
}

bool check_ready_player_online(
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary)
{
    if (!fs::exists(backend_binary)) {
        std::cerr << "Backend runtime not found: " << backend_binary << "\n";
        return false;
    }

    const fs::path fake_offline = mp_spdz_root / "Fake-Offline.x";
    if (!fs::exists(fake_offline)) {
        std::cerr << "Missing Fake-Offline.x required for player-online preprocessing: "
                  << fake_offline << "\n";
        return false;
    }

    return true;
}

// =========================
// SSL setup helpers
// =========================

bool delete_old_ssl_material(const fs::path& mp_spdz_root) {
    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    if (!fs::exists(player_data_dir)) {
        return true;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(player_data_dir, ec)) {
        if (ec) {
            std::cerr << "Failed to iterate Player-Data for SSL cleanup.\n";
            return false;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        const fs::path p = entry.path();
        const std::string ext = p.extension().string();
        if (ext == ".pem" || ext == ".key") {
            std::error_code remove_ec;
            fs::remove(p, remove_ec);
            if (remove_ec) {
                std::cerr << "Failed to remove old SSL file: " << p << "\n";
                return false;
            }
        }
    }

    return true;
}

bool regenerate_ssl_material(const fs::path& mp_spdz_root, int n_parties) {
    if (!delete_old_ssl_material(mp_spdz_root)) {
        return false;
    }

    const std::string cmd =
        "cd " + quote_shell(mp_spdz_root.string()) +
        " && Scripts/setup-ssl.sh " + std::to_string(n_parties);

    std::cout << "Regenerating SSL certificates for " << n_parties << " parties...\n";
    return run_shell_command(cmd) == 0;
}

bool setup_player_online_preprocessing(const fs::path& mp_spdz_root, int n_parties) {
    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    fs::create_directories(player_data_dir);

    const std::string modulus = "170141183460469231731687303715885907969";

    const std::string cmd =
        "cd " + quote_shell(mp_spdz_root.string()) +
        " && ./Fake-Offline.x " + std::to_string(n_parties) +
        " -P " + modulus;

    std::cout << "Generating preprocessing for player-online with Fake-Offline.x...\n";
    const int rc = run_shell_command(cmd);

    if (rc != 0) {
        std::cerr
            << "Fake-Offline.x failed for player-online.\n"
            << "Expected command form:\n"
            << "  ./Fake-Offline.x <nparties> -P <prime>\n";
        return false;
    }

    return true;
}

bool ensure_backend_environment(
    const BackendAdapter& backend,
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    int n_parties)
{
    if (!backend.check_ready(mp_spdz_root, backend_binary)) {
        return false;
    }

    if (backend.requires_ssl) {
        if (!regenerate_ssl_material(mp_spdz_root, n_parties)) {
            std::cerr << "Failed to regenerate SSL material for backend "
                      << backend.name << "\n";
            return false;
        }
    }

    if (backend.name == "player-online") {
        if (!setup_player_online_preprocessing(mp_spdz_root, n_parties)) {
            std::cerr << "Failed to generate preprocessing for backend "
                      << backend.name << "\n";
            return false;
        }
    }

    return true;
}

// =========================
// Backend-specific command builders
// =========================

std::string build_command_party_style_with_N(
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party,
    int n_parties,
    int port_base,
    const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";

    return
        "cd " + quote_shell(mp_spdz_root.string()) + " && " +
        quote_shell(backend_binary.string()) +
        " -p " + std::to_string(party) +
        " -N " + std::to_string(n_parties) +
        " -pn " + std::to_string(port_base) +
        host_arg + " " +
        compiled_program_name +
        " > " + quote_shell(log_path.string()) + " 2>&1";
}

std::string build_command_party_style_no_N(
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party,
    int /*n_parties*/,
    int port_base,
    const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";

    return
        "cd " + quote_shell(mp_spdz_root.string()) + " && " +
        quote_shell(backend_binary.string()) +
        " -p " + std::to_string(party) +
        " -pn " + std::to_string(port_base) +
        host_arg + " " +
        compiled_program_name +
        " > " + quote_shell(log_path.string()) + " 2>&1";
}

std::string build_command_shamir(
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party,
    int /*n_parties*/,
    int port_base,
    const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";

    return
        "cd " + quote_shell(mp_spdz_root.string()) + " && " +
        quote_shell(backend_binary.string()) +
        " -p " + std::to_string(party) +
        " -pn " + std::to_string(port_base) +
        host_arg + " " +
        compiled_program_name +
        " > " + quote_shell(log_path.string()) + " 2>&1";
}

std::string build_command_player_online(
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int party,
    int n_parties,
    int port_base,
    const fs::path& log_path)
{
    return
        "cd " + quote_shell(mp_spdz_root.string()) + " && " +
        quote_shell(backend_binary.string()) + " " + std::to_string(party) +
        " " + compiled_program_name +
        " -N " + std::to_string(n_parties) +
        " -pn " + std::to_string(port_base) +
        " -h localhost" +
        " > " + quote_shell(log_path.string()) + " 2>&1";
}

// =========================
// Backend registry
// =========================

const std::vector<BackendAdapter>& backend_registry() {
    static const std::vector<BackendAdapter> backends = {
        {
            "semi2k", "semi2k-party.x", BackendFamily::Ring2k,
            false, false, false, true, 2,
            prepare_inputs_default, check_ready_default,
            build_command_party_style_with_N, parse_result_default
        },
        {
            "semi", "semi-party.x", BackendFamily::Field,
            false, false, false, true, 2,
            prepare_inputs_default, check_ready_default,
            build_command_party_style_with_N, parse_result_default
        },
        {
            "shamir", "shamir-party.x", BackendFamily::Shamir,
            false, true, false, true, 3,
            prepare_inputs_default, check_ready_default,
            build_command_shamir, parse_result_default
        },
        {
            "replicated-ring", "replicated-ring-party.x", BackendFamily::ReplicatedRing,
            false, true, false, true, 3,
            prepare_inputs_default, check_ready_default,
            build_command_party_style_no_N, parse_result_default
        },
        {
            "replicated-field", "replicated-field-party.x", BackendFamily::ReplicatedField,
            false, true, false, true, 3,
            prepare_inputs_default, check_ready_default,
            build_command_party_style_no_N, parse_result_default
        },
        {
            "player-online", "Player-Online.x", BackendFamily::OnlineField,
            true, true, false, true, 2,
            prepare_inputs_default, check_ready_player_online,
            build_command_player_online, parse_result_default
        }
    };
    return backends;
}

const BackendAdapter* find_backend_adapter(const std::string& backend_name) {
    const auto& backends = backend_registry();
    for (const auto& backend : backends) {
        if (backend.name == backend_name) {
            return &backend;
        }
    }
    return nullptr;
}

fs::path get_backend_binary(const fs::path& mp_spdz_root, const BackendAdapter& backend) {
    return mp_spdz_root / backend.binary_name;
}

void print_supported_backends() {
    std::cerr << "Supported backends are:\n";
    for (const auto& entry : backend_registry()) {
        std::cerr << "  - " << entry.name
                  << " [family=" << backend_family_name(entry.family)
                  << ", preprocessing=" << (entry.requires_preprocessing ? "yes" : "no")
                  << ", ssl=" << (entry.requires_ssl ? "yes" : "no")
                  << ", min_parties=" << entry.min_parties
                  << "]\n";
    }
}

// =========================
// Program compilation
// =========================

bool compile_program(
    const fs::path& mp_spdz_root,
    const fs::path& program_path,
    int n_parties)
{
    const std::string compile_cmd =
        "cd " + quote_shell(mp_spdz_root.string()) + " && python3 compile.py " +
        quote_shell(program_path.string()) + " " + std::to_string(n_parties);

    std::cout << "Compiling program " << program_path.filename().string()
              << " with MP-SPDZ...\n";

    return run_shell_command(compile_cmd) == 0;
}

// =========================
// Backend constraints
// =========================

bool validate_backend_party_count(const BackendAdapter& backend, int n_parties) {
    if (n_parties < 2) {
        std::cerr << "At least 2 computation nodes are required to preserve any MPC meaning.\n";
        return false;
    }

    if (n_parties < backend.min_parties) {
        std::cerr << "Backend " << backend.name
                  << " requires at least " << backend.min_parties
                  << " parties, but requested computation nodes = " << n_parties << ".\n";
        return false;
    }

    return true;
}

bool validate_input_security_model(
    const std::vector<ProviderInput>& selected,
    int n_parties)
{
    if (static_cast<int>(selected.size()) != n_parties) {
        std::cerr
            << "Unsafe / unsupported configuration in current bridge mode:\n"
            << "selected data providers = " << selected.size()
            << ", computation nodes = " << n_parties << ".\n"
            << "Current bridge writes clear inputs directly to MP-SPDZ parties.\n"
            << "Supporting #providers != #computation_nodes securely requires\n"
            << "a real secret-sharing / resharing layer, which is not implemented yet.\n";
        return false;
    }

    return true;
}

// =========================
// Runtime execution
// =========================

bool run_backend_parties(
    const BackendAdapter& backend,
    const fs::path& mp_spdz_root,
    const fs::path& backend_binary,
    const std::string& compiled_program_name,
    int n_parties,
    int port_base,
    const fs::path& logs_dir)
{
    std::vector<std::future<int>> jobs;
    jobs.reserve(static_cast<size_t>(n_parties));

    for (int party = 0; party < n_parties; ++party) {
        const fs::path log_path = logs_dir / ("player_" + std::to_string(party) + ".log");

        const std::string cmd = backend.build_command(
            mp_spdz_root,
            backend_binary,
            compiled_program_name,
            party,
            n_parties,
            port_base,
            log_path);

        if (cmd.empty()) {
            std::cerr << "Failed to build backend command for backend "
                      << backend.name << "\n";
            return false;
        }

        jobs.push_back(std::async(std::launch::async, [cmd]() {
            return run_shell_command(cmd);
        }));
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

// =========================
// Main
// =========================

int main(int argc, char** argv) {
    const fs::path root = fs::current_path();
    const fs::path core_set_path = root / "core_set.txt";
    const fs::path inputs_dir = root / "inputs";
    const fs::path mp_spdz_root = root / "third_party" / "MP-SPDZ";
    const fs::path logs_dir = root / "logs";

    const auto parsed_config = parse_bridge_args(argc, argv, root);
    if (!parsed_config) {
        return 1;
    }
    const BridgeConfig config = *parsed_config;

    const BackendAdapter* backend = find_backend_adapter(config.backend_name);
    if (!backend) {
        std::cerr << "Unknown backend: " << config.backend_name << "\n";
        print_supported_backends();
        return 1;
    }

    if (!fs::exists(config.program_path)) {
        std::cerr << "Missing program file: " << config.program_path << "\n";
        return 1;
    }

    const auto core_set = read_core_set(core_set_path);
    if (core_set.empty()) {
        std::cerr << "No providers in core set. Ensure consensus generated core_set.txt.\n";
        return 1;
    }

    const auto selected_opt = load_selected_inputs(inputs_dir, core_set);
    if (!selected_opt) {
        return 1;
    }
    const std::vector<ProviderInput> selected = *selected_opt;

    const int n_selected_providers = static_cast<int>(selected.size());
    const int n_parties =
        (config.computation_nodes > 0) ? config.computation_nodes : n_selected_providers;

    if (!validate_backend_party_count(*backend, n_parties)) {
        return 1;
    }

    if (!validate_input_security_model(selected, n_parties)) {
        return 1;
    }

    const fs::path backend_binary = get_backend_binary(mp_spdz_root, *backend);
    if (!ensure_backend_environment(*backend, mp_spdz_root, backend_binary, n_parties)) {
        return 1;
    }

    if (!backend->prepare_inputs(mp_spdz_root, selected, n_parties)) {
        std::cerr << "Failed to prepare backend inputs for backend "
                  << backend->name << "\n";
        return 1;
    }

    std::cout << "Backend: " << backend->name
              << " [family=" << backend_family_name(backend->family)
              << ", preprocessing=" << (backend->requires_preprocessing ? "yes" : "no")
              << ", ssl=" << (backend->requires_ssl ? "yes" : "no")
              << ", min_parties=" << backend->min_parties
              << "]\n";

    std::cout << "Selected data providers: " << n_selected_providers << "\n";
    std::cout << "Computation nodes: " << n_parties << "\n";
    std::cout << "Prepared " << n_parties << " MP-SPDZ input file(s).\n";
    std::cout << "Core set mapping (provider_id -> party_index, current clear-input demo mode):\n";
    for (size_t i = 0; i < selected.size(); ++i) {
        std::cout << "  " << selected[i].id << " -> " << i << "\n";
    }

    const std::string program_base = config.program_path.stem().string();
    const std::string compiled_program_name = program_base + "-" + std::to_string(n_parties);

    if (!compile_program(mp_spdz_root, config.program_path, n_parties)) {
        std::cerr << "MP-SPDZ compilation failed for backend "
                  << backend->name << "\n";
        return 1;
    }

    fs::create_directories(logs_dir);
    const int port_base = 15000;

    const bool run_ok = run_backend_parties(
        *backend,
        mp_spdz_root,
        backend_binary,
        compiled_program_name,
        n_parties,
        port_base,
        logs_dir);

    const auto parsed_result = backend->parse_result(logs_dir / "player_0.log");
    if (parsed_result) {
        std::cout << "MP-SPDZ result: RESULT=" << *parsed_result << "\n";
    } else {
        std::cerr << "Could not parse RESULT from MP-SPDZ logs.\n";
    }

    if (!run_ok) {
        std::cerr << "Some MP-SPDZ parties failed.\n";
        return 1;
    }

    if (!parsed_result) {
        return 1;
    }

    return 0;
}