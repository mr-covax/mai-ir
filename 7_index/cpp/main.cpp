#include "config.hpp"
#include "index.hpp"

#include <optional>

namespace {

constexpr size_t mebibyte = 1024 * 1024;

size_t positive(const char* value) {
    ir::require(*value, "Expected positive integer");
    size_t result = 0;

    for (; *value; ++value) {
        ir::require(*value >= '0' && *value <= '9' &&
                        result <= (SIZE_MAX - size_t(*value - '0')) / 10,
                    "Invalid integer");
        result = result * 10 + size_t(*value - '0');
    }
    ir::require(result, "Expected positive integer");
    return result;
}

void build(const char* corpus, const char* output, const char* mode, size_t memory) {
    ir::require(!strcmp(mode, "exact") || !strcmp(mode, "stem"), "Mode must be exact or stem");
    ir::require(memory <= SIZE_MAX / mebibyte, "Memory budget too large");
    ir::build(corpus, output, !strcmp(mode, "stem"), memory * mebibyte);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("ir build | ir search (read ./config)\n"
                 "ir build CORPUS OUTPUT [exact|stem] [MEMORY_MIB]\nir stats INDEX\nir frequencies "
                 "INDEX\nir search INDEX [LIMIT [EXACT_INDEX]] < queries.txt");
        } else if (argc == 2 && !strcmp(argv[1], "build")) {
            const ir::IndexConfig config;
            build(config.get("corpus").c_str(), config.get("output").c_str(),
                  config.get("mode").c_str(), config.number("memory_mib", 1, SIZE_MAX / mebibyte));
        } else if (argc >= 4 && argc <= 6 && !strcmp(argv[1], "build")) {
            const char* mode = argc >= 5 ? argv[4] : "exact";
            size_t memory = argc == 6 ? positive(argv[5]) : 64;
            build(argv[2], argv[3], mode, memory);
        } else if (argc == 3 && !strcmp(argv[1], "stats")) {
            ir::statistics(argv[2]);
        } else if (argc == 3 && !strcmp(argv[1], "frequencies")) {
            ir::frequencies(argv[2]);
        } else if (argc >= 2 && argc <= 5 && !strcmp(argv[1], "search")) {
            std::optional<ir::IndexConfig> config;
            if (argc == 2)
                config.emplace();
            ir::Index index(config ? config->get("output").c_str() : argv[2]);
            size_t limit = config ? config->number("search_limit", 1, SIZE_MAX)
                                  : argc >= 4 ? positive(argv[3]) : 5;
            const char* exact_path = config ? config->get("exact_index", true).c_str()
                                            : argc == 5 ? argv[4] : "";
            std::unique_ptr<ir::Index> exact;
            if (*exact_path)
                exact = std::make_unique<ir::Index>(exact_path);
            char* query = nullptr;
            size_t capacity = 0;
            ssize_t length;

            while ((length = getline(&query, &capacity, stdin)) >= 0) {
                try {
                    ir::require(memchr(query, 0, size_t(length)) == nullptr, "NUL in query");
                    ir::search(index, query, limit, exact.get());
                } catch (...) {
                    free(query);
                    throw;
                }
            }
            free(query);
            ir::require(!ferror(stdin), "Read error");
        } else {
            throw ir::Error{"Invalid arguments; see ir --help"};
        }
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
        return 0;
    } catch (const ir::Error& error) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
