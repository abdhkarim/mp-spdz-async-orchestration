/**
 * spdz_bridge.cpp
 *
 * Bridge between the async orchestration layer and MP-SPDZ (semi2k backend).
 *
 * Architecture:
 *   - Each data_provider generates its own masking secret s_i locally.
 *   - Consensus selects the core set of providers.
 *   - This bridge reads provider-generated secrets, builds additive shares
 *     over Z/2^64, writes Player-Data for semi2k-party.x, then runs the
 *     online computation phase.
 *
 * MP-SPDZ is invoked ONLY for the online computation (no secret generation,
 * no Fake-Offline preprocessing — all externalized).
 *
 * Usage:
 *   ./spdz_bridge [--computation-nodes N] [program_path]
 */

#include <boost/multiprecision/cpp_int.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <unordered_map>
#include <vector>

// Async-externalized semi2k preprocessing (replaces issue_secrets.mpc + Fake-Offline.x).
#include "semi2k_prep.hpp"

namespace fs = std::filesystem;
using boost::multiprecision::cpp_int;

// =============================================================================
// Types
// =============================================================================

// Validated provider entry from inputs/provider_<id>.txt.
struct ProviderInput {
    int         id               = -1;
    long long   value            = 0;
    std::string masked_value_str; // (x_i - s_i) as decimal string
};

// Bridge runtime configuration.
struct BridgeConfig {
    fs::path program_path;
    int      computation_nodes = -1;  // -1 => use selected providers count
};

// =============================================================================
// Utilities
// =============================================================================

// Shell-safe single-quote wrapping.
std::string quote_shell(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

// Strict integer parse — rejects partial matches.
std::optional<long long> parse_integer(const std::string& s) {
    try {
        size_t idx = 0;
        const long long v = std::stoll(s, &idx);
        if (idx != s.size()) return std::nullopt;
        return v;
    } catch (...) { return std::nullopt; }
}

// Run a shell command and normalise its exit code.
int run_shell_command(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
#ifdef WEXITSTATUS
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
#endif
    return rc;
}

// =============================================================================
// Provider file parsing
// =============================================================================

// Parse inputs/provider_<id>.txt (4-line format: id, masked_value, nonce, proof).
std::optional<ProviderInput> parse_provider_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::string line1, line2, line3, line4, extra;
    if (!std::getline(in, line1) || !std::getline(in, line2)) return std::nullopt;

    const bool has_line3 = static_cast<bool>(std::getline(in, line3));
    const bool has_line4 = has_line3 && static_cast<bool>(std::getline(in, line4));
    if (has_line3 != has_line4) return std::nullopt;
    if (std::getline(in, extra)) return std::nullopt;  // Reject extra lines.

    const std::string id_prefix           = "id=";
    const std::string masked_value_prefix = "masked_value=";

    if (line1.rfind(id_prefix, 0) != 0 || line2.rfind(masked_value_prefix, 0) != 0)
        return std::nullopt;

    const auto id = parse_integer(line1.substr(id_prefix.size()));
    if (!id) return std::nullopt;

    const std::string masked_value_str = line2.substr(masked_value_prefix.size());

    if (has_line3) {
        if (line3.rfind("nonce=", 0) != 0 || line4.rfind("proof=", 0) != 0)
            return std::nullopt;
    }

    ProviderInput p;
    p.id               = static_cast<int>(*id);
    p.value            = 0;  // Not used — bridge reconstructs via (x-s)+s.
    p.masked_value_str = masked_value_str;
    return p;
}

// =============================================================================
// Core set + input loading
// =============================================================================

std::vector<int> read_core_set(const fs::path& core_set_path) {
    std::vector<int> ids;
    std::ifstream in(core_set_path);
    if (!in.is_open()) return ids;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto id = parse_integer(line);
        if (id) ids.push_back(static_cast<int>(*id));
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
        const fs::path f = inputs_dir / ("provider_" + std::to_string(id) + ".txt");
        const auto parsed = parse_provider_file(f);
        if (!parsed || parsed->id != id) {
            std::cerr << "Missing or malformed provider file for id " << id << "\n";
            return std::nullopt;
        }
        selected.push_back(*parsed);
    }
    return selected;
}

// =============================================================================
// semi2k-party.x command builder
// =============================================================================

// Builds the shell command to launch one semi2k party.
// semi2k uses: -p <party> -N <n_parties> [-h localhost] <program>
std::string build_semi2k_command(
    const fs::path& mp_spdz_root,
    const fs::path& binary,
    const std::string& compiled_program,
    int party, int n_parties, int port_base,
    const fs::path& log_path)
{
    const std::string host_arg = (party == 0) ? "" : " -h localhost";
    return "cd " + quote_shell(mp_spdz_root.string()) + " && " +
           quote_shell(binary.string()) +
           " -p " + std::to_string(party) +
           " -N " + std::to_string(n_parties) +
           " -pn " + std::to_string(port_base) +
           host_arg + " " + compiled_program +
           " > " + quote_shell(log_path.string()) + " 2>&1";
}

// =============================================================================
// MP-SPDZ compile + run
// =============================================================================

// Compiles a .mpc program for semi2k (ring Z/2^64, flag -R 64).
bool compile_program(
    const fs::path& mp_spdz_root,
    const fs::path& program_path,
    int n_parties, int n_selected)
{
    const std::string cmd =
        "cd " + quote_shell(mp_spdz_root.string()) +
        " && python3 compile.py -R 64 " +
        quote_shell(program_path.string()) +
        " " + std::to_string(n_parties) +
        " " + std::to_string(n_selected);
    std::cout << "Compiling " << program_path.filename().string() << " with MP-SPDZ...\n";
    return run_shell_command(cmd) == 0;
}

// Launches all n_parties semi2k processes in parallel and waits for them.
bool run_semi2k_parties(
    const fs::path& mp_spdz_root,
    const fs::path& binary,
    const std::string& compiled_program,
    int n_parties, int port_base,
    const fs::path& logs_dir)
{
    std::vector<std::future<int>> jobs;
    jobs.reserve(static_cast<size_t>(n_parties));
    for (int party = 0; party < n_parties; ++party) {
        const fs::path log = logs_dir / ("player_" + std::to_string(party) + ".log");
        const std::string cmd = build_semi2k_command(
            mp_spdz_root, binary, compiled_program,
            party, n_parties, port_base, log);
        jobs.push_back(std::async(std::launch::async,
            [cmd]() { return run_shell_command(cmd); }));
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

// Extracts "SUM=<value>" or "RESULT=<value>" from party 0's log.
std::optional<std::string> parse_result_from_log(const fs::path& log_path) {
    std::ifstream in(log_path);
    if (!in.is_open()) return std::nullopt;
    std::string line;
    std::regex result_regex(R"(RESULT=(.*))");
    std::regex sum_regex(R"(SUM=([-]?\d+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, result_regex)) return m[1].str();
        if (std::regex_search(line, m, sum_regex))    return m[1].str();
    }
    return std::nullopt;
}

// =============================================================================
// CLI parsing
// =============================================================================

std::optional<BridgeConfig> parse_args(int argc, char** argv, const fs::path& root) {
    BridgeConfig cfg;
    cfg.program_path = root / "programs" / "sum.mpc";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--computation-nodes") {
            if (i + 1 >= argc) { std::cerr << "Missing value after --computation-nodes\n"; return std::nullopt; }
            const auto v = parse_integer(argv[++i]);
            if (!v || *v <= 0) { std::cerr << "Invalid --computation-nodes value\n"; return std::nullopt; }
            cfg.computation_nodes = static_cast<int>(*v);
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return std::nullopt;
        }

        // Positional: program path.
        fs::path candidate = arg;
        cfg.program_path = candidate.is_absolute() ? candidate : (root / candidate);
    }

    cfg.program_path = fs::absolute(cfg.program_path);
    return cfg;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    const fs::path root         = fs::current_path();
    const fs::path inputs_dir   = root / "inputs";
    const fs::path core_set_path= root / "core_set.txt";
    const fs::path mp_spdz_root = root / "third_party" / "MP-SPDZ";
    const fs::path logs_dir     = root / "logs";
    fs::create_directories(logs_dir);

    // ── CLI ──────────────────────────────────────────────────────────────────
    const auto cfg_opt = parse_args(argc, argv, root);
    if (!cfg_opt) {
        std::cerr << "Usage: ./spdz_bridge [--computation-nodes N] [program_path]\n"
                  << "  --computation-nodes N  number of semi2k computation nodes (default: #providers)\n"
                  << "  program_path           path to .mpc program (default: programs/sum.mpc)\n";
        return 1;
    }
    const BridgeConfig& cfg = *cfg_opt;

    if (!fs::exists(cfg.program_path)) {
        std::cerr << "Missing program file: " << cfg.program_path << "\n";
        return 1;
    }

    // ── Core set ─────────────────────────────────────────────────────────────
    const auto core_set = read_core_set(core_set_path);
    if (core_set.empty()) {
        std::cout << "No providers in core set. Populate core_set.txt to run a program.\n";
        return 0;
    }

    // ── Load validated provider inputs ───────────────────────────────────────
    const auto selected_opt = load_selected_inputs(inputs_dir, core_set);
    if (!selected_opt) return 1;
    const std::vector<ProviderInput>& selected = *selected_opt;

    const int n_parties = (cfg.computation_nodes > 0)
                        ? cfg.computation_nodes
                        : static_cast<int>(selected.size());
    if (n_parties < 2) {
        std::cerr << "semi2k requires at least 2 computation nodes.\n";
        return 1;
    }

    // ── Build Player-Data (externalized semi2k preprocessing) ────────────────
    // The bridge reads per-party share files written by each provider.
    // It NEVER reads s_i in full — each party p only gets its own share_p(s_i).
    std::vector<Semi2kPrep::ProviderEntry> prep_entries;
    prep_entries.reserve(selected.size());
    for (const auto& p : selected) {
        Semi2kPrep::ProviderEntry e;
        e.id = p.id;
        if (!p.masked_value_str.empty()) {
            // Provider used masking: verify share files exist before proceeding.
            const auto sh0 = Semi2kPrep::load_provider_share(root, p.id, 0);
            if (sh0) {
                e.masked_value = p.masked_value_str;  // (x_i - s_i)
                e.plain_value  = "";
            } else {
                // No share files — provider fell back to plain value.
                e.masked_value = "";
                e.plain_value  = p.masked_value_str;
            }
        } else {
            e.masked_value = "";
            e.plain_value  = p.masked_value_str;
        }
        prep_entries.push_back(std::move(e));
    }

    std::string fallback_sum_str;
    if (!Semi2kPrep::prepare_player_data(
            mp_spdz_root, prep_entries, root, n_parties, fallback_sum_str))
        return 1;

    // ── Verify semi2k binary ──────────────────────────────────────────────────
    const fs::path semi2k_binary = mp_spdz_root / "semi2k-party.x";
    if (!fs::exists(semi2k_binary)) {
        std::cout << "semi2k-party.x not found at " << semi2k_binary << "\n"
                  << "Fallback plaintext sum = " << fallback_sum_str << "\n";
        return 0;
    }

    // ── Compile .mpc program ──────────────────────────────────────────────────
    // Compiled name convention used by compile.py: <stem>-<n_parties>-<n_selected>
    const std::string compiled_name =
        cfg.program_path.stem().string() +
        "-" + std::to_string(n_parties) +
        "-" + std::to_string(selected.size());

    if (!compile_program(mp_spdz_root, cfg.program_path, n_parties,
                         static_cast<int>(selected.size()))) {
        std::cout << "MP-SPDZ compilation failed. Fallback sum = " << fallback_sum_str << "\n";
        return 0;
    }

    // ── Run semi2k online computation ─────────────────────────────────────────
    constexpr int port_base = 15000;
    const bool run_ok = run_semi2k_parties(
        mp_spdz_root, semi2k_binary, compiled_name,
        n_parties, port_base, logs_dir);

    // ── Parse and print result ────────────────────────────────────────────────
    const auto result = parse_result_from_log(logs_dir / "player_0.log");
    if (result) {
        std::cout << "MP-SPDZ result: " << *result << "\n";
    } else {
        std::cout << "Could not parse result from MP-SPDZ log. "
                  << "Fallback sum = " << fallback_sum_str << "\n";
    }

    if (!run_ok) {
        std::cout << "Some semi2k parties failed; "
                  << "fallback sum = " << fallback_sum_str << "\n";
        return 1;
    }

    return 0;
}
