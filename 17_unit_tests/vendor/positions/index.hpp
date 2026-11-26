#pragma once
#include "base.hpp"
#include "linguistics.hpp"
#include "query.hpp"

namespace ir::pos {

constexpr uint64_t header_size = 144;
constexpr uint64_t term_size = 56;
constexpr uint64_t skip_size = 24;

struct Header {
    uint32_t flags = 0;
    uint64_t documents = 0, terms = 0, tokens = 0, bytes = 0;
    uint64_t dictionary = header_size, strings = 0, postings = 0, offsets = 0, metadata = 0,
             size = 0;
    uint64_t lengths = 0, skips = 0, norms = 0, fingerprint = 14695981039346656037ull;
    uint32_t stride = 0, dictionary_id = 0;
};

struct Term {
    uint64_t text = 0, postings = 0, bytes = 0, skips = 0, frequency = 0;
    uint32_t length = 0, documents = 0, blocks = 0;
};

void write_header(FILE* file, const Header& header);
void write_term(FILE* file, const Term& term);
uint32_t decode(FILE* file, bool compressed, uint64_t end);
void encode(FILE* file, uint32_t value, bool compressed);
double read_double(FILE* file);
void write_double(FILE* file, double value);
void append(FILE* output, FILE* input);

struct Index {
    std::unique_ptr<FILE, int (*)(FILE*)> file_owner{nullptr, &fclose};
    FILE* file = nullptr;
    Header header;
    Array<uint32_t> lengths;
    Array<double> norms;
    explicit Index(const char* path);
    Term term(uint64_t id);
    bool find(const char* text, Term& found);
    void document(uint32_t id, char*& url, char*& title);
};

struct Counters {
    uint64_t records = 0, positions = 0, skip_reads = 0, jumps = 0;
};

struct Cursor {
    Index& index;
    Term term;
    Counters& counters;
    Array<uint32_t> positions;
    uint64_t next_offset = 0;
    uint32_t seen = 0, previous = 0, document = 0;
    bool valid = false;

    Cursor(Index& source, Term entry, Counters& counts)
        : index(source), term(entry), counters(counts) {}

    bool next();
    bool advance(uint32_t target, bool use_skips);
};

void build(const char* input, const char* output, uint32_t normalization, bool compressed,
           size_t memory, uint32_t stride);
void statistics(const char* path);

struct Options {
    size_t limit = 50, offset = 0;
    bool ranked = false, use_skips = true, fuzzy = false;
};

struct Search {
    Index& index;
    Options options;
    Counters counters;
    uint64_t comparisons = 0;
    Array<Term> query_terms;

    explicit Search(Index& value, Options settings = {}) : index(value), options(settings) {}

    List lookup(const Node& node);
    List conjunction(const Node& node);
    List run(const Node* node);
    List phrase(const Node& node);
    void collect(const Node* node, bool positive = true);
};

struct Hit {
    uint32_t document;
    double score;
};

void compute(Index& index, const char* text, Options options, Array<Hit>& hits, Counters& counts,
             uint64_t& comparisons);
void search(Index& index, const char* text, Options options);
void benchmark(Index& index, const char* queries, Options options, unsigned repeats);
void scan(Index& index, uint32_t minimum, uint32_t maximum);

} // namespace ir::pos
