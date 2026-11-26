#pragma once
#include "base.hpp"

namespace ir {

struct Header {
    uint32_t flags;
    uint64_t documents, terms, tokens, bytes;
    uint64_t dictionary, strings, postings, offsets, metadata, size;
};

struct Term {
    uint64_t text, postings, frequency;
    uint32_t length, documents;
};

struct Index {
    std::unique_ptr<FILE, int (*)(FILE*)> file_owner{nullptr, &fclose};
    FILE* file = nullptr;
    Header header;
    explicit Index(const char* path);
    Term term(uint64_t id);
    bool find(const char* text, Term& found);
    void document(uint32_t id, char*& url, char*& title);
};

void build(const char* input, const char* output, bool stemming, size_t memory);
void statistics(const char* path);
void frequencies(const char* path);
void search(Index& index, const char* query, size_t limit, Index* exact = nullptr);

}
