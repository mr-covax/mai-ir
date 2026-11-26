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

void build_raw_index(const ir::IndexConfig& config) {
    const std::filesystem::path directory(config.get("experiment_output"));

    if (directory.empty() || directory == "." || directory == ".." || directory.has_root_path())
        throw std::runtime_error("Invalid experiment output: " + directory.string());

    const auto path = directory / "raw.idx";
    ir::require(!std::filesystem::exists(path) && !std::filesystem::is_symlink(path),
                "Raw index already exists; use measure-raw, not build-raw");
    const auto corpus = std::filesystem::canonical(config.get("corpus"));
    const auto& mode = config.get("mode");
    const auto memory = config.number("memory_mib", 1, SIZE_MAX / mebibyte);
    // Explicit preparation must not erase a ready index or unrelated experiment files.
    std::filesystem::create_directories(directory);

    save_output(directory / "raw-build.json",
                [&] { build_index(corpus.c_str(), path.c_str(), "raw", memory, 0, mode.c_str()); });
}

void measure_index(const ir::IndexConfig& config, bool compressed) {
    const std::filesystem::path directory(config.get("experiment_output"));

    if (directory.empty() || directory == "." || directory == ".." || directory.has_root_path())
        throw std::runtime_error("Invalid experiment output: " + directory.string());

    std::filesystem::create_directories(directory);

    const std::string codec = compressed ? "vb" : "raw";
    const auto path =
        compressed ? std::filesystem::path(config.get("output")) : directory / "raw.idx";
    const auto queries = std::filesystem::canonical(config.get("queries"));
    const auto repeats = unsigned(config.number("repeats", 1, 1000));

    uint32_t expected_flags = compressed ? 2 : 0;
    const auto& mode = config.get("mode");

    if (mode == "stem")
        expected_flags |= 1;
    else if (mode == "lemma")
        expected_flags |= 4;
    else
        ir::require(mode == "exact", "Invalid normalization");

    ir::pos::Index index(path.c_str());
    ir::require(index.header.flags == expected_flags && index.header.stride == 0,
                "Index does not match compression experiment config");

    struct Range {
        const char* name;
        uint32_t minimum, maximum;
    };

    const Range ranges[] = {
        {"all", 0, UINT32_MAX}, {"rare", 1, 2}, {"medium", 3, 19}, {"common", 20, UINT32_MAX}};

    ir::pos::Options options;
    options.use_skips = false;

    save_output(directory / (codec + "-statistics.json"),
                [&] { ir::pos::statistics(path.c_str()); });

    for (auto [name, minimum, maximum] : ranges)
        save_output(directory / (codec + "-" + name + ".json"),
                    [&] { ir::pos::scan(index, minimum, maximum); });

    save_output(directory / (codec + "-search.json"),
                [&] { ir::pos::benchmark(index, queries.c_str(), options, repeats); });
}
} // namespace

int main(int argc, char** argv) {
    try {
        ir::require(argc == 2, "Usage: positional build|search|build-raw|measure-raw|measure-vb");

        int status = 0;

        if (!strcmp(argv[1], "build")) {
            const ir::IndexConfig config;
            build_index(
                config.get("corpus").c_str(), config.get("output").c_str(),
                config.get("codec").c_str(), config.number("memory_mib", 1, SIZE_MAX / mebibyte),
                uint32_t(config.number("skip_stride", 0, UINT32_MAX)), config.get("mode").c_str());
        } else if (!strcmp(argv[1], "build-raw")) {
            build_raw_index(ir::IndexConfig{});
        } else if (!strcmp(argv[1], "measure-raw")) {
            measure_index(ir::IndexConfig{}, false);
        } else if (!strcmp(argv[1], "measure-vb")) {
            measure_index(ir::IndexConfig{}, true);
        } else if (!strcmp(argv[1], "search")) {
            const ir::IndexConfig config;
            status = search_from_stdin(config.get("output").c_str(), search_options(config));
        } else {
            throw ir::Error{"Usage: positional build|search|build-raw|measure-raw|measure-vb"};
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
