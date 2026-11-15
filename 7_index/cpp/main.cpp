#include "index.hpp"

namespace {

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

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("ir build CORPUS OUTPUT [exact|stem] [MEMORY_MIB]\nir stats INDEX\nir frequencies "
                 "INDEX\nir search INDEX [LIMIT [EXACT_INDEX]] < queries.txt");
        } else if (argc >= 4 && argc <= 6 && !strcmp(argv[1], "build")) {
            const char* mode = argc >= 5 ? argv[4] : "exact";
            ir::require(!strcmp(mode, "exact") || !strcmp(mode, "stem"),
                        "Mode must be exact or stem");
            size_t memory = argc == 6 ? positive(argv[5]) : 64;
            ir::require(memory <= SIZE_MAX / (1024 * 1024), "Memory budget too large");
            ir::build(argv[2], argv[3], !strcmp(mode, "stem"), memory * 1024 * 1024);
        } else if (argc == 3 && !strcmp(argv[1], "stats")) {
            ir::statistics(argv[2]);
        } else if (argc == 3 && !strcmp(argv[1], "frequencies")) {
            ir::frequencies(argv[2]);
        } else if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "search")) {
            ir::Index index(argv[2]);
            size_t limit = argc >= 4 ? positive(argv[3]) : 5;
            ir::Index* exact = argc == 5 ? new ir::Index(argv[4]) : nullptr;
            char* query = nullptr;
            size_t capacity = 0;
            ssize_t length;

            while ((length = getline(&query, &capacity, stdin)) >= 0) {
                try {
                    ir::require(memchr(query, 0, size_t(length)) == nullptr, "NUL in query");
                    ir::search(index, query, limit, exact);
                } catch (...) {
                    free(query);
                    delete exact;
                    throw;
                }
            }
            free(query);
            delete exact;
            ir::require(!ferror(stdin), "Read error");
        } else {
            throw ir::Error{"Invalid arguments; see ir --help"};
        }
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
        return 0;
    } catch (const ir::Error& error) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
}
