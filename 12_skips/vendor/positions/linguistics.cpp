#include "linguistics.hpp"

namespace ir::pos {
void normalize(char* word, size_t capacity, uint32_t flags) {
    if (flags & 4)
        ir::lemmatize(word, capacity);
    else if (flags & 1)
        ir::stem(word);
}

} // namespace ir::pos
