#include "config.hpp"
#include "index.hpp"
#include "output.hpp"

#include <filesystem>
#include <unistd.h>

namespace {
constexpr size_t mebibyte = 1024 * 1024;

bool use_skips(const char* mode) {
    ir::require(!strcmp(mode, "skip") || !strcmp(mode, "linear"), "Expected skip or linear");
    return !strcmp(mode, "skip");
}

ir::pos::Options search_options(const ir::IndexConfig& config) {
    const auto& engine = config.get("search_engine");
    ir::require(engine == "search" || engine == "rank", "Expected search or rank");

    ir::pos::Options options;
    options.ranked = options.fuzzy = engine == "rank";
    options.limit = config.number("search_limit", 1, SIZE_MAX);
    options.offset = config.number("search_offset", 0, SIZE_MAX);
    options.use_skips = use_skips(config.get("traversal").c_str());

    return options;
}

void build_index(const char* corpus, const char* output, const char* codec, uint64_t memory,
                 uint32_t stride, const char* mode) {
    ir::require(!strcmp(codec, "raw") || !strcmp(codec, "vb"), "Codec must be raw or vb");
    ir::require(!strcmp(mode, "exact") || !strcmp(mode, "stem") || !strcmp(mode, "lemma"),
                "Invalid normalization");
    ir::require(memory && memory <= SIZE_MAX / mebibyte, "Invalid memory budget");

    uint32_t normalization = 0;
    if (!strcmp(mode, "stem"))
        normalization = 1;
    else if (!strcmp(mode, "lemma"))
        normalization = 4;

    ir::pos::build(corpus, output, normalization, !strcmp(codec, "vb"), size_t(memory * mebibyte),
                   stride);
}

int search_from_stdin(const char* index_path, ir::pos::Options options) {
    ir::pos::Index index(index_path);
    char* query = nullptr;
    size_t capacity = 0;
    int status = 0;
    ssize_t length = 0;

    while ((length = getline(&query, &capacity, stdin)) >= 0) {
        try {
            ir::require(memchr(query, 0, size_t(length)) == nullptr, "NUL in query");
            ir::pos::search(index, query, options);
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

    return status;
}

template <class Action> void save_output(const std::filesystem::path& path, Action action) {
    ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");

    std::unique_ptr<FILE, int (*)(FILE*)> output(fopen(path.c_str(), "wb"), &fclose);
    if (!output)
        throw std::runtime_error("Cannot open output: " + path.string());

    const int saved = dup(STDOUT_FILENO);
    ir::require(saved != -1, "Cannot save stdout");

    if (dup2(fileno(output.get()), STDOUT_FILENO) == -1) {
        close(saved);
        throw ir::Error{"Cannot redirect stdout"};
    }

    try {
        action();
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
    } catch (...) {
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
        throw;
    }

    const bool restored = dup2(saved, STDOUT_FILENO) != -1;
    close(saved);
    ir::require(restored, "Cannot restore stdout");
}

void measure_ranking(const ir::IndexConfig& config) {
    const auto& mode = config.get("mode");
    const auto repeats = unsigned(config.number("repeats", 1, 1000));
    const std::filesystem::path directory(config.get("experiment_output"));
    const auto queries = std::filesystem::canonical(config.get("queries"));
    const auto& codec = config.get("codec");
    const auto stride = uint32_t(config.number("skip_stride", 0, UINT32_MAX));
    auto options = search_options(config);

    if (directory.empty() || directory == "." || directory == ".." || directory.has_root_path())
        throw std::runtime_error("Invalid experiment output: " + directory.string());

    // Measurement must never build an index or remove prepared artifacts.
    const auto path = config.get("output");
    ir::pos::Index index(path.c_str());
    ir::require(codec == "vb" || codec == "raw", "Codec must be raw or vb");
    ir::require(mode == "lemma" || mode == "stem" || mode == "exact", "Invalid normalization");
    const uint32_t flags = (codec == "vb" ? 2u : 0u) |
                           (mode == "lemma" ? 4u : mode == "stem" ? 1u : 0u);
    ir::require(index.header.flags == flags && index.header.stride == stride,
                "Index does not match ranking experiment config");
    std::filesystem::create_directories(directory);

    for (const std::string engine : {"search", "rank"}) {
        options.ranked = options.fuzzy = engine == "rank";

        save_output(directory / (engine + ".jsonl"), [&] {
            std::ifstream input(queries);
            if (!input)
                throw std::runtime_error("Cannot open queries: " + queries.string());

            std::string line;

            while (std::getline(input, line)) {
                ir::require(line.find('\0') == std::string::npos, "NUL in query");
                ir::pos::search(index, line.c_str(), options);
            }

            if (!input.eof() || input.bad())
                throw std::runtime_error("Cannot read queries: " + queries.string());
        });
    }

    save_output(directory / "benchmark.json",
                [&] { ir::pos::benchmark(index, queries.c_str(), options, repeats); });
}
} // namespace

int main(int argc, char** argv) {
    try {
        ir::require(argc == 2, "Usage: positional build|search|measure");

        int status = 0;

        if (!strcmp(argv[1], "build")) {
            const ir::IndexConfig config;

            build_index(
                config.get("corpus").c_str(), config.get("output").c_str(),
                config.get("codec").c_str(), config.number("memory_mib", 1, SIZE_MAX / mebibyte),
                uint32_t(config.number("skip_stride", 0, UINT32_MAX)), config.get("mode").c_str());
        } else if (!strcmp(argv[1], "measure")) {
            measure_ranking(ir::IndexConfig{});
        } else if (!strcmp(argv[1], "search")) {
            const ir::IndexConfig config;
            status = search_from_stdin(config.get("output").c_str(), search_options(config));
        } else {
            throw ir::Error{"Usage: positional build|search|measure"};
        }

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
