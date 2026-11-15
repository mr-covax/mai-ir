#include "tokenizer.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

struct Statistics {
    std::uint64_t tokens = 0;
    std::uint64_t total_length = 0;
};

std::string read_input(const std::string& path) {
    if (path == "-") {
        std::string input{std::istreambuf_iterator<char>(std::cin), {}};

        if (std::cin.bad()) {
            throw std::runtime_error("Failed to read stdin");
        }

        return input;
    }

    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Cannot open input: " + path);
    }

    std::string input{std::istreambuf_iterator<char>(file), {}};

    if (file.bad()) {
        throw std::runtime_error("Failed to read input: " + path);
    }

    return input;
}

void print_statistics(const Statistics& stats, std::size_t bytes, double seconds) {
    const auto average = stats.tokens ? static_cast<double>(stats.total_length) / stats.tokens : 0;
    const auto per_kib = bytes ? seconds * 1024 / bytes : 0;
    const auto throughput = seconds > 0 ? bytes / (seconds * 1024 * 1024) : 0;

    std::cout << std::setprecision(17) << "{\"tokens\":" << stats.tokens
              << ",\"total_length\":" << stats.total_length << ",\"average_length\":" << average
              << ",\"input_bytes\":" << bytes << ",\"elapsed_seconds\":" << seconds
              << ",\"seconds_per_kib\":" << per_kib << ",\"mib_per_second\":" << throughput
              << "}\n";
}

}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    bool stats_only = false;
    bool positional = false;
    bool has_path = false;
    std::string path = "-";

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];

            if (!positional && argument == "--help") {
                std::cout << "Usage: tokenize [--stats-only] [--] [FILE|-]\n";
                return 0;
            }

            if (!positional && argument == "--stats-only") {
                stats_only = true;
            } else if (!positional && argument == "--") {
                positional = true;
            } else if (!positional && argument.starts_with("--")) {
                throw std::runtime_error("Unknown option: " + argument);
            } else if (has_path) {
                throw std::runtime_error("Expected at most one input file");
            } else {
                path = argument;
                has_path = true;
            }
        }

        const auto input = read_input(path);
        Statistics stats;
        const auto started = std::chrono::steady_clock::now();

        tokenizer::tokenize(input, [&](const tokenizer::Token& token) {
            ++stats.tokens;
            stats.total_length += token.length;

            if (!stats_only) {
                std::cout << token.text << '\n';
            }
        });

        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        if (stats_only) {
            print_statistics(stats, input.size(), elapsed);
        }

        std::cout.flush();

        if (!std::cout) {
            throw std::runtime_error("Failed to write output");
        }

    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
