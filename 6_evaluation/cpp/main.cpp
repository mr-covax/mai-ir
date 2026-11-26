#include "metrics.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("evaluate QUERIES_TSV QRELS_TSV RUNS_TSV");
        return 0;
    }

    try {
        ir::require(argc == 4, "Usage: evaluate QUERIES_TSV QRELS_TSV RUNS_TSV");
        ir::evaluate(argv[1], argv[2], argv[3]);
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
        return 0;
    } catch (const ir::Error& error) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
}
