#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct ProviderInput {
    int id = -1;
    long long value = 0;
};

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

int main(int argc, char* argv[]) {
    int timeout_seconds = 10;
    if (argc == 2) {
        const auto parsed_timeout = parse_integer(argv[1]);
        if (!parsed_timeout || *parsed_timeout < 0) {
            std::cerr << "Invalid timeout value: " << argv[1] << "\n";
            return 1;
        }
        timeout_seconds = static_cast<int>(*parsed_timeout);
    } else if (argc > 2) {
        std::cerr << "Usage: ./consensus [timeout_seconds]\n";
        return 1;
    }

    std::cout << "Consensus waiting " << timeout_seconds << " seconds for provider inputs...\n";
    std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));

    const fs::path inputs_dir = fs::current_path() / "inputs";
    const fs::path core_set_file = fs::current_path() / "core_set.txt";
    std::vector<int> core_set_ids;

    if (!fs::exists(inputs_dir)) {
        std::cout << "Inputs directory not found. Writing empty core set.\n";
    } else {
        const std::regex filename_regex(R"(provider_(\d+)\.txt)");
        for (const auto& entry : fs::directory_iterator(inputs_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(filename, match, filename_regex)) {
                continue;
            }

            const auto parsed_file_id = parse_integer(match[1].str());
            if (!parsed_file_id) {
                continue;
            }

            const auto parsed = parse_provider_file(entry.path());
            if (!parsed || parsed->id != *parsed_file_id) {
                std::cout << "Ignoring malformed provider file: " << entry.path() << "\n";
                continue;
            }

            core_set_ids.push_back(parsed->id);
        }
    }

    std::sort(core_set_ids.begin(), core_set_ids.end());
    core_set_ids.erase(std::unique(core_set_ids.begin(), core_set_ids.end()), core_set_ids.end());

    std::ofstream out(core_set_file);
    if (!out.is_open()) {
        std::cerr << "Failed to write core set file: " << core_set_file << "\n";
        return 1;
    }

    for (const int id : core_set_ids) {
        out << id << "\n";
    }

    std::cout << "Core set decided with " << core_set_ids.size() << " provider(s): ";
    for (size_t i = 0; i < core_set_ids.size(); ++i) {
        std::cout << core_set_ids[i];
        if (i + 1 < core_set_ids.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "\n";
    std::cout << "Wrote " << core_set_file << "\n";

    return 0;
}
