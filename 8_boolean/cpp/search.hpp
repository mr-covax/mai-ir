#pragma once
#include "index.hpp"
#include "query.hpp"

namespace ir {

struct SearchContext {
    Index& index;
    uint64_t postings_read = 0;
};

List lookup(const Node& node, void* context);
void boolean_search(Index& index, const char* text, size_t limit, size_t offset);

}
