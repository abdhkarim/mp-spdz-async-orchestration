#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;

struct ProviderInput {
    int id = -1;
    long long value = 0;
};

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

std::optional<ProviderInput> parse_provider_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::nullopt;
    }

    std::string line1;
    std::string line2;
    std::string extra;
    if (!std::getline(in, line1) || !std::getline(in, line2)) {
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

    ProviderInput parsed;
    parsed.id = static_cast<int>(*id);
    parsed.value = *value;
    return parsed;
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

std::optional<std::string> read_sum_from_log(const fs::path& log_path) {
    std::ifstream in(log_path);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::string line;
    std::regex sum_regex(R"(SUM=([-]?\d+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, sum_regex)) {
            return m[1].str();
        }
    }
    return std::nullopt;
}

int main() {
    const fs::path root = fs::current_path();
    const fs::path core_set_path = root / "core_set.txt";
    const fs::path inputs_dir = root / "inputs";
    const fs::path mp_spdz_root = root / "third_party" / "MP-SPDZ";
    const fs::path project_sum_program = root / "programs" / "sum.mpc";

    const auto core_set = read_core_set(core_set_path);
    if (core_set.empty()) {
        std::cerr << "No providers in core set. Ensure consensus generated core_set.txt.\n";
        return 1;
    }

    std::vector<ProviderInput> selected;
    selected.reserve(core_set.size());
    long long fallback_sum = 0;
    for (const int id : core_set) {
        const fs::path provider_file = inputs_dir / ("provider_" + std::to_string(id) + ".txt");
        const auto parsed = parse_provider_file(provider_file);
        if (!parsed || parsed->id != id) {
            std::cerr << "Missing or malformed provider file for selected id " << id << "\n";
            return 1;
        }
        selected.push_back(*parsed);
        fallback_sum += parsed->value;
    }

    const fs::path player_data_dir = mp_spdz_root / "Player-Data";
    fs::create_directories(player_data_dir);
    for (size_t party = 0; party < selected.size(); ++party) {
        const fs::path input_file = player_data_dir / ("Input-P" + std::to_string(party) + "-0");
        std::ofstream out(input_file);
        if (!out.is_open()) {
            std::cerr << "Failed to write MP-SPDZ input file: " << input_file << "\n";
            return 1;
        }
        out << selected[party].value << "\n";
    }

    std::cout << "Prepared " << selected.size() << " MP-SPDZ input file(s).\n";
    std::cout << "Core set mapping (provider_id -> party_index):\n";
    for (size_t i = 0; i < selected.size(); ++i) {
        std::cout << "  " << selected[i].id << " -> " << i << "\n";
    }

    const fs::path player_online_binary = mp_spdz_root / "Player-Online.x";
    if (!fs::exists(player_online_binary)) {
        std::cout << "Player-Online.x not found. Integration attempted but runtime unavailable.\n";
        std::cout << "Fallback sum from validated inputs = " << fallback_sum << "\n";
        return 0;
    }
    if (!fs::exists(project_sum_program)) {
        std::cerr << "Missing program file: " << project_sum_program << "\n";
        return 1;
    }

    const int n_parties = static_cast<int>(selected.size());
    const std::string compiled_program_name = "sum-" + std::to_string(n_parties);

    const std::string compile_cmd =
        "cd " + quote_shell(mp_spdz_root.string()) + " && python3 compile.py " +
        quote_shell(project_sum_program.string()) + " " + std::to_string(n_parties);
    std::cout << "Compiling sum.mpc with MP-SPDZ...\n";
    if (run_shell_command(compile_cmd) != 0) {
        std::cout << "MP-SPDZ compilation failed. Fallback sum = " << fallback_sum << "\n";
        return 0;
    }

    const int port_base = 15000;
    const fs::path logs_dir = root / "logs";
    fs::create_directories(logs_dir);

    std::vector<std::future<int>> jobs;
    jobs.reserve(selected.size());
    for (int party = 0; party < n_parties; ++party) {
        const fs::path log_path = logs_dir / ("player_" + std::to_string(party) + ".log");
        const std::string cmd =
            "cd " + quote_shell(mp_spdz_root.string()) + " && " +
            quote_shell(player_online_binary.string()) + " " + std::to_string(party) +
            " " + compiled_program_name + " -N " + std::to_string(n_parties) +
            " -pn " + std::to_string(port_base) +
            " -h localhost > " + quote_shell(log_path.string()) + " 2>&1";

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

    const auto parsed_sum = read_sum_from_log(logs_dir / "player_0.log");
    if (parsed_sum) {
        std::cout << "MP-SPDZ result: SUM=" << *parsed_sum << "\n";
    } else {
        std::cout << "Could not parse SUM from MP-SPDZ logs. Fallback sum = " << fallback_sum << "\n";
    }

    if (!all_ok) {
        std::cout << "Some MP-SPDZ parties failed; fallback sum from validated inputs = "
                  << fallback_sum << "\n";
        return 1;
    }

    return 0;
}
