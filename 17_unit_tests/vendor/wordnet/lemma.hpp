#pragma once
#include <cstddef>
#include <cstdint>

namespace ir {
void lemmatize(char* word, size_t capacity);
uint32_t lemma_dictionary_id();
}
