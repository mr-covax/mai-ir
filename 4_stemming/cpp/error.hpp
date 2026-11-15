#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ir {

struct Error {
    const char* message;
};

inline void require(bool condition, const char* message) {
    if (!condition)
        throw Error{message};
}

void tokenize(const char* text, size_t length, void (*emit)(const char*, size_t, void*),
              void* context);
void stem(char* word);

}
