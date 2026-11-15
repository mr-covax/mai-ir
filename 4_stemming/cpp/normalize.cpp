#include "error.hpp"
#include "lemma.hpp"

namespace {
void emit(const char* token, size_t length, void* opaque) {
    uint32_t mode = *static_cast<uint32_t*>(opaque);
    size_t capacity = length + 1;
    if (mode == 4 && capacity < 256)
        capacity = 256;
    char* word = static_cast<char*>(malloc(capacity));
    ir::require(word != nullptr, "Out of memory");
    memcpy(word, token, length);
    word[length] = 0;

    try {
        if (mode == 4)
            ir::lemmatize(word, capacity);
        else if (mode == 1)
            ir::stem(word);
        puts(word);
    } catch (...) {
        free(word);
        throw;
    }
    free(word);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("normalize [lemma|stem|exact] < text.txt");
        return 0;
    }

    try {
        ir::require(argc <= 2, "Usage: normalize [lemma|stem|exact] < text.txt");
        const char* mode = argc == 2 ? argv[1] : "lemma";
        ir::require(!strcmp(mode, "lemma") || !strcmp(mode, "stem") || !strcmp(mode, "exact"),
                    "Unknown normalization mode");
        uint32_t flags = !strcmp(mode, "lemma") ? 4u : !strcmp(mode, "stem") ? 1u : 0u;
        char* text = nullptr;
        size_t capacity = 0;
        ssize_t length;

        while ((length = getline(&text, &capacity, stdin)) >= 0) {
            try {
                ir::require(memchr(text, 0, size_t(length)) == nullptr, "NUL in input");
                ir::tokenize(text, strlen(text), emit, &flags);
            } catch (...) {
                free(text);
                throw;
            }
        }
        free(text);
        ir::require(!ferror(stdin), "Read error");
        ir::require(!ferror(stdout) && fflush(stdout) == 0, "Output error");
        return 0;
    } catch (const ir::Error& error) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
}
