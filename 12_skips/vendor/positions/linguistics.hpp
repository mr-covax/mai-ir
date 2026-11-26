#pragma once
#include "base.hpp"
#include "lemma.hpp"

namespace ir::pos {
void normalize(char* word, size_t capacity, uint32_t flags);
}
