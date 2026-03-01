#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: ./data_provider <id> <value> [--malformed]\n";
        return 1;
    }

    const std::string id = argv[1];
    const std::string value = argv[2];
    const bool malformed = (argc == 4 && std::string(argv[3]) == "--malformed");

    fs::path inputs_dir = fs::current_path() / "inputs";
    fs::create_directories(inputs_dir);

    const fs::path output_file = inputs_dir / ("provider_" + id + ".txt");
    std::ofstream out(output_file);
    if (!out.is_open()) {
        std::cerr << "Failed to open output file: " << output_file << "\n";
        return 1;
    }

    if (malformed) {
        out << "this_file_is_malformed\n";
    } else {
        out << "id=" << id << "\n";
        out << "value=" << value << "\n";
    }

    std::cout << "Provider " << id << " wrote input file: " << output_file << "\n";
    return 0;
}
