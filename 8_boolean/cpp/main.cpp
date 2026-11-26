#include "config.hpp"
#include "output.hpp"
#include "search.hpp"

#include <optional>

namespace {
constexpr size_t mebibyte = 1024 * 1024;

size_t number(const char* text, bool allow_zero) {
    size_t value = 0;
    ir::require(*text, "Expected integer");

    for (; *text; ++text) {
        ir::require(*text >= '0' && *text <= '9' && value <= (SIZE_MAX - size_t(*text - '0')) / 10,
                    "Invalid integer");
        value = value * 10 + size_t(*text - '0');
    }
    ir::require(value || allow_zero, "Expected positive integer");
    return value;
}

void build(const char* corpus, const char* output, const char* mode, size_t memory) {
    ir::require(!strcmp(mode, "exact") || !strcmp(mode, "stem"), "Mode must be exact or stem");
    ir::require(memory <= SIZE_MAX / mebibyte, "Memory budget too large");
    ir::build(corpus, output, !strcmp(mode, "stem"), memory * mebibyte);
    ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("boolean build | boolean search (read ./config)\n"
             "boolean build CORPUS OUTPUT exact|stem MEMORY_MIB\n"
             "boolean INDEX [LIMIT [OFFSET]] < queries.txt\nOperators: implicit AND, &&, ||, !, "
             "parentheses; ! > AND > OR");
        return 0;
    }

    try {
        if (argc == 2 && !strcmp(argv[1], "build")) {
            const ir::IndexConfig config;
            build(config.get("corpus").c_str(), config.get("output").c_str(),
                  config.get("mode").c_str(), config.number("memory_mib", 1, SIZE_MAX / mebibyte));
            return 0;
        }
        if (argc == 6 && !strcmp(argv[1], "build")) {
            build(argv[2], argv[3], argv[4], number(argv[5], false));
            return 0;
        }
        ir::require(argc >= 2 && argc <= 4, "Usage: boolean INDEX [LIMIT [OFFSET]] < queries.txt");
        std::optional<ir::IndexConfig> config;
        if (argc == 2 && !strcmp(argv[1], "search"))
            config.emplace();
        ir::Index index(config ? config->get("output").c_str() : argv[1]);
        size_t limit = config ? config->number("search_limit", 1, SIZE_MAX)
                              : argc >= 3 ? number(argv[2], false) : 50;
        size_t offset = config ? config->number("search_offset", 0, SIZE_MAX)
                               : argc == 4 ? number(argv[3], true) : 0;
        int status = 0;
        char* query = nullptr;
        size_t capacity = 0;
        ssize_t length;

        while ((length = getline(&query, &capacity, stdin)) >= 0) {
            try {
                ir::require(memchr(query, 0, size_t(length)) == nullptr, "NUL in query");
                ir::boolean_search(index, query, limit, offset);
            } catch (const ir::Error& error) {
                printf("{\"total\":0,\"results\":[],\"error\":");
                ir::print_json_string(error.message);
                puts("}");
                fprintf(stderr, "%s\n", error.message);
                status = 1;
            }
        }
        free(query);
        ir::require(!ferror(stdin), "Read error");
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
        return status;
    } catch (const ir::Error& error) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
