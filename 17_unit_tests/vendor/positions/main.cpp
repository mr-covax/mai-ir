#include "config.hpp"
#include "index.hpp"
#include "output.hpp"

namespace {
constexpr size_t mebibyte = 1024 * 1024;

void build(const char* corpus, const char* output, std::string_view codec, uint64_t memory,
           uint32_t stride, std::string_view mode) {
    ir::require(codec == "raw" || codec == "vb", "Codec must be raw or vb");
    ir::require(mode == "exact" || mode == "stem" || mode == "lemma", "Invalid normalization");
    ir::require(memory && memory <= SIZE_MAX / mebibyte, "Invalid memory budget");
    const uint32_t normalization = mode == "lemma" ? 4u : mode == "stem" ? 1u : 0u;
    ir::pos::build(corpus, output, normalization, codec == "vb", size_t(memory * mebibyte), stride);
}
} // namespace

int main(int argc, char** argv) {
    try {
        ir::require(argc == 2 && !strcmp(argv[1], "build"), "Usage: positional build");

        const ir::IndexConfig config;
        build(config.get("corpus").c_str(), config.get("output").c_str(), config.get("codec"),
              config.number("memory_mib", 1, SIZE_MAX / mebibyte),
              uint32_t(config.number("skip_stride", 0, UINT32_MAX)), config.get("mode"));

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
