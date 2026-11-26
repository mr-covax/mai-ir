#include "index.hpp"
#include "output.hpp"

#include <cmath>
#include <ctime>

namespace ir::pos {
namespace {

struct TermCollector {
    Index& index;
    Array<Term>& entries;
    bool missing = false;
    size_t count = 0;

    TermCollector(Index& source, Array<Term>& output) : index(source), entries(output) {}
};

void add_query_term(const char* text, size_t length, void* opaque) {
    auto& context = *static_cast<TermCollector*>(opaque);
    char word[4 * max_query_bytes + 1];
    require(length < sizeof(word), "Query term too long");

    memcpy(word, text, length);
    word[length] = 0;
    normalize(word, sizeof(word), context.index.header.flags);

    Term term;
    bool found = context.index.find(word, term);
    ++context.count;

    if (!found) {
        context.missing = true;
        return;
    }
    context.entries.push(term);
}

bool find_terms(Index& index, const char* text, Array<Term>& output) {
    TermCollector context(index, output);
    tokenize(text, strlen(text), add_query_term, &context);
    return context.count && !context.missing;
}

bool is_term_conjunction(const Node& node) {
    if (node.operation == Operation::term)
        return true;
    return node.operation == Operation::conjunction && is_term_conjunction(*node.left) &&
           is_term_conjunction(*node.right);
}

bool gather_terms(Index& index, const Node& node, Array<Term>& terms) {
    if (node.operation == Operation::term)
        return find_terms(index, node.text, terms);

    bool left_found = gather_terms(index, *node.left, terms);
    bool right_found = gather_terms(index, *node.right, terms);
    return left_found && right_found;
}

size_t find_position(const Array<uint32_t>& positions, uint64_t value) {
    size_t a = 0, b = positions.size;

    while (a < b) {
        size_t middle = a + (b - a) / 2;
        if (positions[middle] < value)
            a = middle + 1;
        else
            b = middle;
    }

    return a;
}

struct CursorArray {
    Array<Cursor*> items;

    ~CursorArray() {
        for (size_t i = 0; i < items.size; ++i)
            delete items[i];
    }
};

double now() {
    timespec time{};
    require(clock_gettime(CLOCK_MONOTONIC, &time) == 0, "Cannot read clock");
    return double(time.tv_sec) + double(time.tv_nsec) / 1e9;
}

bool fuzzy_query(const char* text) {
    return !strpbrk(text, "&|!()\"") && !strstr(text, "«") && !strstr(text, "»");
}

} // namespace

void compute(Index& index, const char* text, Options options, Array<Hit>& hits, Counters& counts,
             uint64_t& comparisons) {
    Query query(text, options.ranked && options.fuzzy && fuzzy_query(text));
    Search searcher(index, options);
    List result = searcher.run(query.root);

    for (size_t i = 0; i < result.size(); ++i)
        hits.push({result[i], 0});

    if (options.ranked && hits.size) {
        searcher.collect(query.root);
        double query_norm = 0;

        for (size_t i = 0; i < searcher.query_terms.size; ++i) {
            Term term = searcher.query_terms[i];
            double idf = log((double(index.header.documents) + 1) / (term.documents + 1)) + 1;
            query_norm += idf * idf;
            Cursor cursor(index, term, searcher.counters);

            for (size_t j = 0; j < hits.size; ++j) {
                if (!cursor.advance(hits[j].document, options.use_skips))
                    break;
                if (cursor.document == hits[j].document)
                    hits[j].score += (1 + log(double(cursor.positions.size))) * idf * idf;
            }
        }
        query_norm = sqrt(query_norm);

        for (size_t i = 0; i < hits.size; ++i) {
            double norm = index.norms[hits[i].document] * query_norm;
            hits[i].score = norm ? hits[i].score / norm : 0;
        }
        sort(hits.data, hits.size, [](const Hit& a, const Hit& b) {
            return a.score != b.score ? a.score > b.score : a.document < b.document;
        });
    }

    counts = searcher.counters;
    comparisons = searcher.comparisons;
}

List Search::conjunction(const Node& node) {
    Array<Term> terms;
    if (!gather_terms(index, node, terms) || !terms.size)
        return List{};

    sort(terms.data, terms.size,
         [](const Term& a, const Term& b) { return a.documents < b.documents; });
    List result;
    Cursor first(index, terms[0], counters);

    while (first.next())
        result.push(first.document);

    for (size_t i = 1; i < terms.size && result.size(); ++i) {
        if (terms[i].text == terms[i - 1].text)
            continue;
        Cursor cursor(index, terms[i], counters);
        size_t kept = 0;

        for (size_t j = 0; j < result.size(); ++j) {
            ++comparisons;
            if (!cursor.advance(result[j], options.use_skips))
                break;
            if (cursor.document == result[j])
                result.values[kept++] = result[j];
        }
        result.values.size = kept;
    }

    return result;
}

List Search::lookup(const Node& node) {
    if (node.operation == Operation::phrase)
        return phrase(node);
    return conjunction(node);
}

List Search::run(const Node* node) {
    if (!node)
        return List{};
    if (is_term_conjunction(*node))
        return conjunction(*node);
    if (node->operation == Operation::phrase)
        return phrase(*node);

    List a = run(node->left);
    if (node->operation == Operation::negation)
        return complement(a, index.header.documents);

    List b = run(node->right);
    if (node->operation == Operation::conjunction)
        return intersection(a, b, 0, &comparisons);
    require(node->operation == Operation::disjunction, "Invalid query operation");
    return unite(a, b);
}

List Search::phrase(const Node& node) {
    Array<Term> terms;
    List result;
    if (!find_terms(index, node.text, terms) || !terms.size)
        return result;

    CursorArray cursors;
    size_t pivot = 0;

    for (size_t i = 0; i < terms.size; ++i) {
        cursors.items.push(new Cursor(index, terms[i], counters));
        if (terms[i].documents < terms[pivot].documents)
            pivot = i;
    }

    Cursor& anchor = *cursors.items[pivot];
    anchor.next();

    while (anchor.valid) {
        uint32_t document = anchor.document, target = document;
        bool exhausted = false;

        for (size_t i = 0; i < cursors.items.size; ++i) {
            if (i == pivot)
                continue;
            if (!cursors.items[i]->advance(document, options.use_skips)) {
                exhausted = true;
                break;
            }
            if (cursors.items[i]->document > target)
                target = cursors.items[i]->document;
        }
        if (exhausted)
            break;

        if (target != document) {
            anchor.advance(target, options.use_skips);
            continue;
        }

        bool found = false;
        const auto& first = cursors.items[0]->positions;

        for (size_t i = 0; i < first.size && !found; ++i) {
            uint64_t start = first[i], last = start;
            bool matches = true;

            for (size_t j = 1; j < cursors.items.size; ++j) {
                const auto& positions = cursors.items[j]->positions;
                uint64_t expected = node.distance == UINT32_MAX ? start + j : last + 1;
                size_t at = find_position(positions, expected);
                if (at == positions.size ||
                    (node.distance == UINT32_MAX && positions[at] != expected)) {
                    matches = false;
                    break;
                }
                last = positions[at];
                if (node.distance != UINT32_MAX && last - start > node.distance) {
                    matches = false;
                    break;
                }
            }
            found = matches;
        }
        if (found)
            result.push(document);

        anchor.next();
    }

    return result;
}

void Search::collect(const Node* node, bool positive) {
    if (!node)
        return;
    if (node->operation == Operation::negation) {
        collect(node->left, !positive);
        return;
    }
    if (node->operation == Operation::term || node->operation == Operation::phrase) {
        if (!positive)
            return;

        Array<Term> terms;
        find_terms(index, node->text, terms);

        for (size_t i = 0; i < terms.size; ++i) {
            bool duplicate = false;

            for (size_t j = 0; j < query_terms.size; ++j)
                duplicate = duplicate || query_terms[j].text == terms[i].text;
            if (!duplicate)
                query_terms.push(terms[i]);
        }

        return;
    }

    collect(node->left, positive);
    collect(node->right, positive);
}

void search(Index& index, const char* text, Options options) {
    double started = now();
    Array<Hit> hits;
    Counters counters;
    uint64_t comparisons = 0;

    compute(index, text, options, hits, counters, comparisons);
    double elapsed = now() - started;

    printf(
        "{\"total\":%zu,\"offset\":%zu,\"elapsed_seconds\":%.9f,\"postings_read\":%llu,\"positions_"
        "read\":%llu,\"skip_reads\":%llu,\"jumps\":%llu,\"comparisons\":%llu,\"results\":[",
        hits.size, options.offset, elapsed, (unsigned long long)counters.records,
        (unsigned long long)counters.positions, (unsigned long long)counters.skip_reads,
        (unsigned long long)counters.jumps, (unsigned long long)comparisons);
    size_t count = 0;

    for (size_t i = options.offset; i < hits.size && count < options.limit; ++i, ++count) {
        char* url = nullptr;
        char* title = nullptr;

        index.document(hits[i].document, url, title);

        if (count)
            putchar(',');
        printf("{\"id\":%u,\"url\":", hits[i].document);
        print_json_string(url);
        printf(",\"title\":");
        print_json_string(title);
        if (options.ranked)
            printf(",\"score\":%.12f", hits[i].score);
        putchar('}');

        free(url);
        free(title);
    }
    puts("]}");
}

void benchmark(Index& index, const char* query_file, Options options, unsigned repeats) {
    std::unique_ptr<FILE, int (*)(FILE*)> input_owner(fopen(query_file, "rb"), &fclose);
    FILE* input = input_owner.get();
    require(input != nullptr, "Cannot open file");

    Array<char*> queries;
    char* text = nullptr;
    size_t capacity = 0;
    uint64_t checksum = 14695981039346656037ull, total_records = 0, total_jumps = 0;
    Array<double> times;

    try {
        ssize_t length;
        while ((length = getline(&text, &capacity, input)) >= 0) {
            require(memchr(text, 0, size_t(length)) == nullptr, "NUL in query");
            queries.push(text);
            text = nullptr;
            capacity = 0;
        }
        free(text);
        text = nullptr;
        require(!ferror(input), "Read error");
        require(queries.size && repeats > 0 && repeats <= 1000, "Invalid benchmark size");

        for (unsigned repeat = 0; repeat <= repeats; ++repeat) {
            for (size_t i = 0; i < queries.size; ++i) {
                Array<Hit> hits;
                Counters counts;
                uint64_t comparisons = 0;
                double started = now();
                compute(index, queries[i], options, hits, counts, comparisons);
                double seconds = now() - started;
                if (!repeat)
                    continue;
                times.push(seconds);
                total_records += counts.records;
                total_jumps += counts.jumps;
                checksum ^= hits.size;
                checksum *= 1099511628211ull;

                for (size_t j = 0; j < hits.size; ++j) {
                    checksum ^= hits[j].document;
                    checksum *= 1099511628211ull;
                }
            }
        }
    } catch (...) {
        free(text);
        for (size_t i = 0; i < queries.size; ++i)
            free(queries[i]);
        throw;
    }

    double sum = 0;

    for (size_t i = 0; i < times.size; ++i)
        sum += times[i];
    sort(times.data, times.size, [](double a, double b) { return a < b; });
    double median = times.size % 2 ? times[times.size / 2]
                                   : (times[times.size / 2 - 1] + times[times.size / 2]) / 2;
    printf("{\"queries\":%zu,\"repeats\":%u,\"warmup_passes\":1,\"mean_seconds\":%.9f,\"median_"
           "seconds\":%.9f,\"postings_read\":%llu,\"jumps\":%llu,\"result_checksum\":\"%016llx\","
           "\"use_skips\":%s}\n",
           queries.size, repeats, sum / times.size, median, (unsigned long long)total_records,
           (unsigned long long)total_jumps, (unsigned long long)checksum,
           options.use_skips ? "true" : "false");

    for (size_t i = 0; i < queries.size; ++i)
        free(queries[i]);
}

void scan(Index& index, uint32_t minimum, uint32_t maximum) {
    Counters counters;
    uint64_t terms = 0, bytes = 0, checksum = 0;
    double started = now();

    for (uint64_t i = 0; i < index.header.terms; ++i) {
        Term term = index.term(i);
        if (term.documents < minimum || term.documents > maximum)
            continue;
        ++terms;
        bytes += term.bytes;
        Cursor cursor(index, term, counters);

        while (cursor.next()) {
            checksum += cursor.document;

            for (size_t j = 0; j < cursor.positions.size; ++j)
                checksum += cursor.positions[j];
        }
    }

    double elapsed = now() - started;
    printf("{\"terms\":%llu,\"coordinate_bytes\":%llu,\"postings\":%llu,\"positions\":%llu,"
           "\"elapsed_seconds\":%.9f,\"checksum\":%llu}\n",
           (unsigned long long)terms, (unsigned long long)bytes,
           (unsigned long long)counters.records, (unsigned long long)counters.positions, elapsed,
           (unsigned long long)checksum);
}

} // namespace ir::pos
