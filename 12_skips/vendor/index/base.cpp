#include "base.hpp"
#include <sys/types.h>

namespace ir {

void require(bool condition, const char* message) {
    if (!condition)
        throw Error{message};
}

uint32_t read32(FILE* file) {
    unsigned char bytes[4];
    require(fread(bytes, 1, 4, file) == 4, "Truncated file or read error");
    uint32_t value = 0;

    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(bytes[i]) << (i * 8);
    return value;
}

uint64_t read64(FILE* file) {
    uint64_t low = read32(file), high = read32(file);
    return low | (high << 32);
}

void write32(FILE* file, uint32_t value) {
    unsigned char bytes[4];

    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = value >> (i * 8);
    require(fwrite(bytes, 1, 4, file) == 4, "Write error");
}

void write64(FILE* file, uint64_t value) {
    write32(file, uint32_t(value));
    write32(file, uint32_t(value >> 32));
}

char* read_text(FILE* file, uint64_t length, uint64_t end) {
    uint64_t position = uint64_t(ftello(file));
    require(length < SIZE_MAX && position <= end && length <= end - position,
            "Invalid string length");
    char* text = static_cast<char*>(malloc(size_t(length) + 1));
    require(text != nullptr, "Out of memory");

    try {
        require(fread(text, 1, size_t(length), file) == size_t(length),
                "Truncated file or read error");
        text[length] = 0;
        require(memchr(text, 0, size_t(length)) == nullptr, "NUL in text");
    } catch (...) {
        free(text);
        throw;
    }

    return text;
}

}
