#include "output.hpp"
#include "search.hpp"

namespace {
size_t number(const char* text, bool allow_zero) {
    size_t value = 0;
    ir::require(*text, "Expected integer");

    for (; *text; ++text) {
        ir::require(*text >= '0' && *text <= '9' && value <= (SIZE_MAX - size_t(*text - '0')) / 10,
                    "Invalid integer");
        value = value * 10 + size_t(*text - '0');
    }
    ir::require(value || allow_zero, "Limit must be positive");
    return value;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("boolean INDEX [LIMIT [OFFSET]] < queries.txt\nOperators: implicit AND, &&, ||, !, "
             "parentheses; ! > AND > OR");
        return 0;
    }

    try {
        ir::require(argc >= 2 && argc <= 4, "Usage: boolean INDEX [LIMIT [OFFSET]] < queries.txt");
        ir::Index index(argv[1]);
        size_t limit = argc >= 3 ? number(argv[2], false) : 50;
        size_t offset = argc == 4 ? number(argv[3], true) : 0;
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
    }
}
