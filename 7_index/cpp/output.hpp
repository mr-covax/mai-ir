#pragma once

#include <cstdio>

namespace ir {

inline void print_json_string(const char* text) {
    putchar('"');

    for (auto p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 32)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

}
