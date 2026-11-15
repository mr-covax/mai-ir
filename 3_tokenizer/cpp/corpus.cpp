#include "tokenizer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void read(std::ifstream& file, char* output, std::size_t size) {
    if (!file.read(output, static_cast<std::streamsize>(size)))
        throw std::runtime_error("Truncated corpus stream");
}

std::uint32_t read32(std::ifstream& file) {
    unsigned char data[4];
    read(file, reinterpret_cast<char*>(data), 4);
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 | std::uint32_t(data[2]) << 16 |
           std::uint32_t(data[3]) << 24;
}

std::uint64_t read64(std::ifstream& file) {
    std::uint64_t low = read32(file), high = read32(file);
    return low | high << 32;
}

std::string text(std::ifstream& file, std::uint64_t length) {
    if (length > 64 * 1024 * 1024)
        throw std::runtime_error("Document field exceeds 64 MiB");
    std::string result(static_cast<std::size_t>(length), '\0');
    read(file, result.data(), result.size());
    if (result.find('\0') != std::string::npos)
        throw std::runtime_error("NUL in corpus field");
    return result;
}

void fingerprint(std::uint64_t& hash, const std::string& value) {
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    hash *= 1099511628211ull;
}

std::uint64_t number(const char* value) {
    if (!*value)
        throw std::runtime_error("Missing limit");
    std::uint64_t result = 0;

    for (; *value; ++value) {
        if (*value < '0' || *value > '9' || result > (UINT64_MAX - unsigned(*value - '0')) / 10)
            throw std::runtime_error("Invalid limit");
        result = result * 10 + unsigned(*value - '0');
    }

    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("tokenize_stream CORPUS_BIN [--frequencies FILE] [LIMIT]");
            return 0;
        }
        if (argc < 2 || argc > 5)
            throw std::runtime_error("Usage: tokenize_stream CORPUS_BIN [--frequencies FILE] [LIMIT]");
        const char* corpus = nullptr;
        const char* frequencies = nullptr;
        std::uint64_t limit = UINT64_MAX;

        for (int index = 1; index < argc; ++index) {
            if (!strcmp(argv[index], "--frequencies")) {
                if (index + 1 >= argc)
                    throw std::runtime_error("--frequencies requires a path");
                frequencies = argv[++index];
            } else if (!strncmp(argv[index], "--", 2)) {
                throw std::runtime_error("Unknown option");
            } else if (!corpus) {
                corpus = argv[index];
            } else if (limit == UINT64_MAX) {
                limit = number(argv[index]);
            } else {
                throw std::runtime_error("Unexpected argument");
            }
        }
        std::ifstream file(corpus, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open corpus stream");
        char magic[8];
        read(file, magic, 8);
        if (memcmp(magic, "IRDOC001", 8))
            throw std::runtime_error("Invalid corpus stream");
        std::uint64_t documents = 0, bytes = 0, tokens = 0, length = 0,
                      hash = 14695981039346656037ull;
        struct Counters {
            std::uint64_t frequency;
            std::uint32_t documents;
        };
        std::unordered_map<std::string, Counters> terms;
        std::unordered_set<const std::string*> unique;
        double seconds = 0;
        auto started = std::chrono::steady_clock::now();

        while (documents < limit && file.peek() != std::char_traits<char>::eof()) {
            auto a = read32(file), b = read32(file);
            auto size = read64(file);
            auto url = text(file, a);
            auto title = text(file, b);
            auto input = text(file, size);
            fingerprint(hash, url);
            fingerprint(hash, title);
            fingerprint(hash, input);
            auto begin = std::chrono::steady_clock::now();
            unique.clear();
            tokenizer::tokenize(input, [&](const tokenizer::Token& token) {
                ++tokens;
                length += token.length;

                if (frequencies) {
                    auto inserted = terms.try_emplace(token.text, Counters{0, 0});
                    ++inserted.first->second.frequency;
                    unique.insert(&inserted.first->first);
                }
            });

            for (const std::string* term : unique)
                ++terms[*term].documents;
            seconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            ++documents;
            bytes += size;
        }
        if (file.bad())
            throw std::runtime_error("Corpus read error");
        if (frequencies) {
            std::vector<const std::pair<const std::string, Counters>*> rows;
            rows.reserve(terms.size());

            for (const auto& entry : terms)
                rows.push_back(&entry);
            std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
                return a->first < b->first;
            });
            std::ofstream output(frequencies, std::ios::binary);
            if (!output)
                throw std::runtime_error("Cannot open frequencies output");

            for (const auto* row : rows)
                output << row->first << '\t' << row->second.frequency << '\t'
                       << row->second.documents << '\n';
            if (!output.flush())
                throw std::runtime_error("Frequencies write error");
        }
        double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << std::setprecision(17) << "{\"documents\":" << documents
                  << ",\"input_bytes\":" << bytes << ",\"tokens\":" << tokens
                  << ",\"total_length\":" << length
                  << ",\"average_length\":" << (tokens ? double(length) / tokens : 0)
                  << ",\"tokenization_seconds\":" << seconds << ",\"wall_seconds\":" << wall
                  << ",\"seconds_per_kib\":" << (bytes ? seconds * 1024 / bytes : 0)
                  << ",\"mib_per_second\":" << (seconds ? bytes / (seconds * 1024 * 1024) : 0)
                  << ",\"corpus_fingerprint\":\"" << std::hex << hash << "\",\"terms\":" << std::dec
                  << terms.size() << "}\n";
        if (!std::cout.flush())
            throw std::runtime_error("Output error");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
