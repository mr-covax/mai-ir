#include "search.hpp"
#include "output.hpp"
#include <ctime>

namespace ir {
namespace {

struct Atom {
    SearchContext& context;
    List result;
    bool first = true;

    explicit Atom(SearchContext& value) : context(value) {}
};

void emit(const char* text, size_t length, void* opaque) {
    auto& atom = *static_cast<Atom*>(opaque);
    char word[4 * max_query_bytes + 1];
    require(length < sizeof(word), "Query term too long");
    memcpy(word, text, length);
    word[length] = 0;
    if (atom.context.index.header.flags & 1)
        stem(word);
    Term term{};
    bool found = atom.context.index.find(word, term);
    List ids;
    if (found) {
        auto& index = atom.context.index;
        require(fseeko(index.file, static_cast<off_t>(index.header.postings + term.postings),
                       SEEK_SET) == 0,
                "Cannot seek file");

        for (uint32_t i = 0; i < term.documents; ++i) {
            uint32_t id = read32(index.file);
            require(id < index.header.documents && (!i || ids[i - 1] < id), "Invalid posting list");
            ids.push(id);
        }
        atom.context.postings_read += term.documents;
    }
    if (atom.first)
        atom.result = static_cast<List&&>(ids);
    else
        atom.result = intersection(atom.result, ids);
    atom.first = false;
}

}

List lookup(const Node& node, void* context) {
    require(node.operation != Operation::phrase,
            "Phrase search requires a positional index from laboratory 10");
    Atom atom(*static_cast<SearchContext*>(context));
    tokenize(node.text, strlen(node.text), emit, &atom);
    return static_cast<List&&>(atom.result);
}

void boolean_search(Index& index, const char* text, size_t limit, size_t offset) {
    timespec start{}, finish{};
    require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "Cannot read clock");
    Query query(text);
    SearchContext context{index};
    uint64_t comparisons = 0;
    List ids = evaluate(query.root, index.header.documents, lookup, &context, 0, &comparisons);
    require(clock_gettime(CLOCK_MONOTONIC, &finish) == 0, "Cannot read clock");
    double seconds =
        double(finish.tv_sec - start.tv_sec) + double(finish.tv_nsec - start.tv_nsec) / 1e9;
    printf("{\"total\":%zu,\"offset\":%zu,\"elapsed_seconds\":%.9f,\"postings_read\":%llu,"
           "\"comparisons\":%llu,\"results\":[",
           ids.size(), offset, seconds, (unsigned long long)context.postings_read,
           (unsigned long long)comparisons);
    size_t count = 0;

    for (size_t i = offset; i < ids.size() && count < limit; ++i, ++count) {
        char *url = nullptr, *title = nullptr;
        index.document(ids[i], url, title);
        if (count)
            putchar(',');
        printf("{\"id\":%u,\"url\":", ids[i]);
        print_json_string(url);
        printf(",\"title\":");
        print_json_string(title);
        putchar('}');
        free(url);
        free(title);
    }
    puts("]}");
}

}
