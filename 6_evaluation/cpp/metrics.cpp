#include "metrics.hpp"
#include "arrays.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ir {

Scores metrics(const unsigned* grades, size_t count, const unsigned* ideal, size_t ideal_count,
               size_t cutoff) {
    Scores result{};
    double idcg = 0, continuation = 1;

    for (size_t i = 0; i < cutoff; ++i) {
        unsigned grade = i < count ? grades[i] : 0;
        require(grade <= 3, "Relevance must be 0..3");

        double gain = double((1u << grade) - 1);
        result.precision += grade >= 2 ? 1 : 0;
        result.dcg += gain / log2(double(i + 2));

        double satisfaction = gain / 8;
        result.err += continuation * satisfaction / double(i + 1);
        continuation *= 1 - satisfaction;

        if (i < ideal_count) {
            require(ideal[i] <= 3, "Relevance must be 0..3");
            idcg += double((1u << ideal[i]) - 1) / log2(double(i + 2));
        }
    }

    require(cutoff > 0, "Cutoff must be positive");
    result.precision /= double(cutoff);
    result.ndcg = idcg ? result.dcg / idcg : 0;
    return result;
}

namespace {

struct Query {
    const char* id;
};

struct Judgment {
    size_t query;
    const char* url;
    unsigned grade;
};

struct Run {
    size_t system, query;
    unsigned rank;
    const char* url;
};

struct Evaluation {
    Array<Query> queries;
    Array<Judgment> judgments;
    Array<Run> runs;
    Array<const char*> systems;
    Array<char*> rows;

    ~Evaluation() {
        for (size_t i = 0; i < rows.size; ++i)
            free(rows[i]);
    }

    size_t query(const char* id) const {
        for (size_t i = 0; i < queries.size; ++i)
            if (!strcmp(queries[i].id, id))
                return i;

        throw Error{"Unknown query ID"};
    }

    size_t system(const char* name) {
        for (size_t i = 0; i < systems.size; ++i)
            if (!strcmp(systems[i], name))
                return i;

        systems.push(name);
        return systems.size - 1;
    }

    unsigned grade(size_t query, const char* url) const {
        for (size_t i = 0; i < judgments.size; ++i)
            if (judgments[i].query == query && !strcmp(judgments[i].url, url))
                return judgments[i].grade;

        throw Error{"Unjudged search result; complete qrels before evaluation"};
    }
};

size_t read_rows(Evaluation& evaluation, const char* path) {
    FILE* input = fopen(path, "rb");
    require(input != nullptr, "Cannot open TSV file");
    const size_t first = evaluation.rows.size;
    char* row = nullptr;
    size_t capacity = 0;

    try {
        ssize_t length = 0;
        while ((length = getline(&row, &capacity, input)) >= 0) {
            require(memchr(row, 0, size_t(length)) == nullptr, "NUL in TSV row");
            while (length > 0 && (row[length - 1] == '\n' || row[length - 1] == '\r'))
                row[--length] = 0;

            evaluation.rows.push(row);
            row = nullptr;
            capacity = 0;
        }

        require(!ferror(input), "Cannot read TSV file");
    } catch (...) {
        free(row);
        fclose(input);
        throw;
    }

    free(row);
    require(fclose(input) == 0, "Cannot close TSV file");
    return first;
}

unsigned number(const char* text) {
    require(*text != 0, "Missing integer");
    unsigned value = 0;

    for (; *text; ++text) {
        require(*text >= '0' && *text <= '9' && value <= (UINT_MAX - unsigned(*text - '0')) / 10,
                "Invalid integer");
        value = value * 10 + unsigned(*text - '0');
    }

    return value;
}

void split_fields(char* row, char** parts, size_t expected) {
    size_t count = 1;
    parts[0] = row;

    for (char* p = row; *p; ++p)
        if (*p == '\t') {
            require(count < expected, "Too many TSV fields");
            *p = 0;
            parts[count++] = p + 1;
        }

    require(count == expected, "Wrong TSV field count");

    for (size_t i = 0; i < count; ++i)
        require(*parts[i], "Empty TSV field");
}

bool judgment_before(const Judgment& left, const Judgment& right) {
    if (left.query != right.query)
        return left.query < right.query;
    return strcmp(left.url, right.url) < 0;
}

void sort_judgments(Array<Judgment>& judgments) {
    for (size_t i = 1; i < judgments.size; ++i) {
        Judgment value = judgments[i];
        size_t place = i;
        while (place > 0 && judgment_before(value, judgments[place - 1])) {
            judgments[place] = judgments[place - 1];
            --place;
        }
        judgments[place] = value;
    }
}

bool run_before(const Run& left, const Run& right) {
    if (left.system != right.system)
        return left.system < right.system;
    if (left.query != right.query)
        return left.query < right.query;
    return left.rank < right.rank;
}

void sort_runs(Array<Run>& runs) {
    for (size_t i = 1; i < runs.size; ++i) {
        Run value = runs[i];
        size_t place = i;
        while (place > 0 && run_before(value, runs[place - 1])) {
            runs[place] = runs[place - 1];
            --place;
        }
        runs[place] = value;
    }
}

void sort_grades(Array<unsigned>& grades) {
    for (size_t i = 1; i < grades.size; ++i) {
        unsigned value = grades[i];
        size_t place = i;
        while (place > 0 && value > grades[place - 1]) {
            grades[place] = grades[place - 1];
            --place;
        }
        grades[place] = value;
    }
}

void print_string(const char* text) {
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

void print_scores(Scores scores) {
    printf("{\"P\":%.9f,\"DCG\":%.9f,\"NDCG\":%.9f,\"ERR\":%.9f}", scores.precision, scores.dcg,
           scores.ndcg, scores.err);
}

} // namespace

void evaluate(const char* query_path, const char* judgment_path, const char* run_path) {
    Evaluation e;
    size_t first = read_rows(e, query_path);
    for (size_t row = first; row < e.rows.size; ++row) {
        char* parts[2];
        split_fields(e.rows[row], parts, 2);

        for (size_t i = 0; i < e.queries.size; ++i)
            require(strcmp(e.queries[i].id, parts[0]), "Duplicate query ID");

        e.queries.push({parts[0]});
    }

    require(e.queries.size, "No queries");

    first = read_rows(e, judgment_path);
    for (size_t row = first; row < e.rows.size; ++row) {
        char* parts[3];
        split_fields(e.rows[row], parts, 3);

        size_t query = e.query(parts[0]);
        unsigned grade = number(parts[2]);

        require(grade <= 3, "Relevance must be 0..3");
        e.judgments.push({query, parts[1], grade});
    }

    sort_judgments(e.judgments);

    for (size_t i = 1; i < e.judgments.size; ++i)
        require(e.judgments[i - 1].query != e.judgments[i].query ||
                    strcmp(e.judgments[i - 1].url, e.judgments[i].url),
                "Duplicate judgment");

    first = read_rows(e, run_path);
    for (size_t row = first; row < e.rows.size; ++row) {
        char* parts[4];
        split_fields(e.rows[row], parts, 4);

        unsigned rank = number(parts[2]);

        require(rank <= 5, "Run ranks must be 0..5");
        require(rank != 0 || !strcmp(parts[3], "-"), "Empty result must use rank 0 and URL '-'");
        e.runs.push({e.system(parts[0]), e.query(parts[1]), rank, parts[3]});
    }

    require(e.systems.size, "No systems in runs");

    sort_runs(e.runs);

    for (size_t i = 0; i < e.runs.size; ++i) {
        const auto& r = e.runs[i];
        bool same = i && e.runs[i - 1].system == r.system && e.runs[i - 1].query == r.query;
        if (!r.rank) {
            require(!same && (i + 1 == e.runs.size || e.runs[i + 1].system != r.system ||
                              e.runs[i + 1].query != r.query),
                    "Empty result mixed with ranked results");
            continue;
        }

        require(r.rank == (same ? e.runs[i - 1].rank + 1 : 1), "Noncontiguous or duplicate ranks");

        for (size_t j = i + 1 - r.rank; j < i; ++j)
            require(strcmp(r.url, e.runs[j].url), "Duplicate result URL");

        e.grade(r.query, r.url);
    }

    printf("{\"query_count\":%zu,\"relevant_minimum\":2,\"max_grade\":3,\"systems\":{",
           e.queries.size);
    const size_t cutoffs[] = {1, 3, 5};

    for (size_t system = 0; system < e.systems.size; ++system) {
        if (system)
            putchar(',');
        print_string(e.systems[system]);
        printf(":{\"queries\":{");
        Scores sums[3]{};

        for (size_t query = 0; query < e.queries.size; ++query) {
            Array<unsigned> ideal;

            for (size_t i = 0; i < e.judgments.size; ++i)
                if (e.judgments[i].query == query)
                    ideal.push(e.judgments[i].grade);

            require(ideal.size, "Query has no judgments");
            sort_grades(ideal);

            unsigned grades[5]{};
            size_t count = 0;
            bool seen = false;

            for (size_t i = 0; i < e.runs.size; ++i)
                if (e.runs[i].system == system && e.runs[i].query == query) {
                    seen = true;
                    if (e.runs[i].rank)
                        grades[count++] = e.grade(query, e.runs[i].url);
                }

            require(seen, "Missing query response; use rank 0 and URL '-' for an empty result");

            if (query)
                putchar(',');
            print_string(e.queries[query].id);
            printf(":{");

            for (size_t c = 0; c < 3; ++c) {
                Scores score = metrics(grades, count, ideal.data, ideal.size, cutoffs[c]);
                sums[c].precision += score.precision;
                sums[c].dcg += score.dcg;
                sums[c].ndcg += score.ndcg;
                sums[c].err += score.err;
                if (c)
                    putchar(',');
                printf("\"%zu\":", cutoffs[c]);
                print_scores(score);
            }
            putchar('}');
        }
        printf("},\"macro\":{");

        for (size_t c = 0; c < 3; ++c) {
            Scores score = sums[c];
            score.precision /= e.queries.size;
            score.dcg /= e.queries.size;
            score.ndcg /= e.queries.size;
            score.err /= e.queries.size;
            if (c)
                putchar(',');
            printf("\"%zu\":", cutoffs[c]);
            print_scores(score);
        }
        printf("}}");
    }
    puts("}}");
}

} // namespace ir
