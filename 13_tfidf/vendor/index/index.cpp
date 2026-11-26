#include "index.hpp"
#include "output.hpp"
#include <ctime>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ir {
namespace {

constexpr size_t header_size = 96;
constexpr uint32_t max_text = 64 * 1024 * 1024;

struct Workspace {
    char* path;

    explicit Workspace(const char* output) {
        path = static_cast<char*>(malloc(strlen(output) + 16));
        require(path != nullptr, "Out of memory");
        sprintf(path, "%s.tmp-XXXXXX", output);
        if (!mkdtemp(path)) {
            free(path);
            throw Error{"Cannot create temporary directory"};
        }
    }

    ~Workspace() {
        DIR* directory = opendir(path);
        if (directory) {
            while (auto entry = readdir(directory)) {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;
                unlinkat(dirfd(directory), entry->d_name, 0);
            }
            closedir(directory);
        }
        rmdir(path);
        free(path);
    }

    char* file(const char* name) const {
        char* result = static_cast<char*>(malloc(strlen(path) + strlen(name) + 2));
        require(result != nullptr, "Out of memory");
        sprintf(result, "%s/%s", path, name);
        return result;
    }
};

struct Pair {
    char* text;
    uint32_t document;
    uint64_t frequency;
};

int compare(const Pair& a, const Pair& b) {
    int order = strcmp(a.text, b.text);
    return order ? order : a.document < b.document ? -1 : a.document > b.document;
}

void write_pair(FILE* file, const Pair& pair) {
    size_t length = strlen(pair.text);
    require(length <= UINT32_MAX, "Term too long");
    write32(file, uint32_t(length));
    write32(file, pair.document);
    write64(file, pair.frequency);
    require(fwrite(pair.text, 1, length, file) == length, "Write error");
}

bool read_pair(FILE* file, Pair& pair, uint64_t end) {
    free(pair.text);
    pair.text = nullptr;
    if (uint64_t(ftello(file)) == end)
        return false;
    uint32_t length = read32(file);
    pair.document = read32(file);
    pair.frequency = read64(file);
    pair.text = read_text(file, length, end);
    return true;
}

struct Builder {
    Workspace work;
    Array<Pair> block;
    Array<char*> runs;
    size_t used = 0, budget;
    uint32_t document = 0;
    uint64_t tokens = 0;
    bool stemming;
    unsigned serial = 0;
    unsigned blocks = 0;

    Builder(const char* output, size_t memory, bool normalize)
        : work(output), budget(memory), stemming(normalize) {}

    ~Builder() {
        for (size_t i = 0; i < block.size; ++i)
            free(block[i].text);

        for (auto i = size_t(0); i < runs.size; ++i)
            free(runs[i]);
    }

    char* name() {
        char suffix[40];
        snprintf(suffix, sizeof(suffix), "run-%u", serial++);
        return work.file(suffix);
    }

    void flush() {
        if (!block.size)
            return;
        sort(block.data, block.size,
             [](const Pair& a, const Pair& b) { return compare(a, b) < 0; });
        char* path = name();
        std::unique_ptr<FILE, int (*)(FILE*)> file_owner(fopen(path, "wb"), &fclose);
        FILE* file = file_owner.get();
        require(file != nullptr, "Cannot open file");

        for (size_t i = 0; i < block.size;) {
            size_t end = i + 1;

            while (end < block.size && compare(block[i], block[end]) == 0) {
                block[i].frequency += block[end].frequency;
                ++end;
            }
            write_pair(file, block[i]);

            for (; i < end; ++i) {
                free(block[i].text);
                block[i].text = nullptr;
            }
        }
        require(fflush(file) == 0, "Write error");
        block.size = 0;
        used = 0;
        runs.push(path);
        ++blocks;
    }

    void add(const char* text, size_t length) {
        require(length <= UINT32_MAX, "Term too long");
        if (used + length + 1 + sizeof(Pair) * 2 > budget)
            flush();
        char* normalized = strndup(text, length);
        require(normalized != nullptr, "Out of memory");
        try {
            if (stemming)
                stem(normalized);
            block.push({normalized, document, 1});
        } catch (...) {
            free(normalized);
            throw;
        }
        used += length + 1 + sizeof(Pair) * 2;
        ++tokens;
    }

    char* merge() {
        flush();
        if (!runs.size) {
            char* path = name();
            std::unique_ptr<FILE, int (*)(FILE*)> empty_owner(fopen(path, "wb"), &fclose);
            FILE* empty = empty_owner.get();
            require(empty != nullptr, "Cannot open file");
            runs.push(path);
        }

        while (runs.size > 1) {
            size_t next = 0;

            for (size_t i = 0; i < runs.size; i += 2) {
                if (i + 1 == runs.size) {
                    runs[next++] = runs[i];
                    continue;
                }

                char* path = name();
                std::unique_ptr<FILE, int (*)(FILE*)> a_owner(fopen(runs[i], "rb"), &fclose);
                FILE* a = a_owner.get();
                require(a != nullptr, "Cannot open file");
                struct stat a_info{};
                require(fstat(fileno(a), &a_info) == 0 && a_info.st_size >= 0, "Cannot stat file");
                uint64_t a_size = uint64_t(a_info.st_size);
                std::unique_ptr<FILE, int (*)(FILE*)> b_owner(fopen(runs[i + 1], "rb"), &fclose);
                FILE* b = b_owner.get();
                require(b != nullptr, "Cannot open file");
                struct stat b_info{};
                require(fstat(fileno(b), &b_info) == 0 && b_info.st_size >= 0, "Cannot stat file");
                uint64_t b_size = uint64_t(b_info.st_size);
                std::unique_ptr<FILE, int (*)(FILE*)> output_owner(fopen(path, "wb"), &fclose);
                FILE* output = output_owner.get();
                require(output != nullptr, "Cannot open file");
                Pair left{nullptr, 0, 0}, right{nullptr, 0, 0};
                bool has_left = read_pair(a, left, a_size), has_right = read_pair(b, right, b_size);

                while (has_left || has_right) {
                    int order = has_left && has_right ? compare(left, right) : has_left ? -1 : 1;
                    if (order == 0) {
                        left.frequency += right.frequency;
                        write_pair(output, left);
                        has_left = read_pair(a, left, a_size);
                        has_right = read_pair(b, right, b_size);
                    } else if (order < 0) {
                        write_pair(output, left);
                        has_left = read_pair(a, left, a_size);
                    } else {
                        write_pair(output, right);
                        has_right = read_pair(b, right, b_size);
                    }
                }
                require(fflush(output) == 0, "Write error");
                free(left.text);
                free(right.text);
                unlink(runs[i]);
                unlink(runs[i + 1]);
                free(runs[i]);
                free(runs[i + 1]);
                runs[i] = nullptr;
                runs[i + 1] = nullptr;
                runs[next++] = path;
            }
            runs.size = next;
        }

        return runs[0];
    }
};

void append(FILE* output, FILE* input) {
    require(fseeko(input, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");
    char buffer[65536];
    struct stat input_info{};
    require(fstat(fileno(input), &input_info) == 0 && input_info.st_size >= 0, "Cannot stat file");
    uint64_t input_size = uint64_t(input_info.st_size);
    uint64_t remaining = input_size;

    while (remaining) {
        size_t size = remaining < sizeof(buffer) ? size_t(remaining) : sizeof(buffer);
        require(fread(buffer, 1, size, input) == size, "Truncated file or read error");
        require(fwrite(buffer, 1, size, output) == size, "Write error");
        remaining -= size;
    }
}

void write_term(FILE* file, const Term& term) {
    write64(file, term.text);
    write64(file, term.postings);
    write64(file, term.frequency);
    write32(file, term.length);
    write32(file, term.documents);
}

void write_header(FILE* file, const Header& h) {
    require(fseeko(file, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");
    require(fwrite("IRIDX001", 1, 8, file) == 8, "Write error");
    write32(file, 1);
    write32(file, h.flags);
    write64(file, h.documents);
    write64(file, h.terms);
    write64(file, h.tokens);
    write64(file, h.bytes);
    write64(file, h.dictionary);
    write64(file, h.strings);
    write64(file, h.postings);
    write64(file, h.offsets);
    write64(file, h.metadata);
    write64(file, h.size);
}

struct Query {
    Array<char*> words;
    bool stemming;

    explicit Query(bool normalize) : stemming(normalize) {}

    ~Query() {
        for (size_t i = 0; i < words.size; ++i)
            free(words[i]);
    }
};

}

void build(const char* input, const char* output, bool stemming, size_t memory) {
    struct stat status;
    require(lstat(output, &status) != 0, "Output already exists");
    Builder builder(output, memory, stemming);
    std::unique_ptr<FILE, int (*)(FILE*)> stream_owner(fopen(input, "rb"), &fclose);
    FILE* stream = stream_owner.get();
    require(stream != nullptr, "Cannot open file");
    struct stat stream_info{};
    require(fstat(fileno(stream), &stream_info) == 0 && stream_info.st_size >= 0,
            "Cannot stat file");
    uint64_t stream_size = uint64_t(stream_info.st_size);
    char magic[8];
    require(fread(magic, 1, 8, stream) == 8, "Truncated file or read error");
    require(memcmp(magic, "IRDOC001", 8) == 0, "Invalid corpus stream");
    char* paths[6];
    const char* names[] = {"documents", "offsets", "dictionary", "strings", "postings", "index"};

    for (size_t i = 0; i < 6; ++i)
        paths[i] = builder.work.file(names[i]);
    std::unique_ptr<FILE, int (*)(FILE*)> documents_owner(fopen(paths[0], "w+b"), &fclose);
    FILE* documents = documents_owner.get();
    require(documents != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> offsets_owner(fopen(paths[1], "w+b"), &fclose);
    FILE* offsets = offsets_owner.get();
    require(offsets != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> dictionary_owner(fopen(paths[2], "w+b"), &fclose);
    FILE* dictionary = dictionary_owner.get();
    require(dictionary != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> strings_owner(fopen(paths[3], "w+b"), &fclose);
    FILE* strings = strings_owner.get();
    require(strings != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> postings_owner(fopen(paths[4], "w+b"), &fclose);
    FILE* postings = postings_owner.get();
    require(postings != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> index_owner(fopen(paths[5], "w+b"), &fclose);
    FILE* index = index_owner.get();
    require(index != nullptr, "Cannot open file");
    uint64_t bytes = 0;
    const auto start = clock();
    timespec wall_start{};
    require(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0, "Cannot read clock");

    while (uint64_t(ftello(stream)) < stream_size) {
        require(builder.document < UINT32_MAX, "Too many documents");
        uint32_t url_size = read32(stream), title_size = read32(stream);
        uint64_t text_size = read64(stream);
        require(url_size && url_size <= max_text && title_size <= max_text && text_size <= max_text,
                "Invalid document size");
        char* url = read_text(stream, url_size, stream_size);
        char* title = read_text(stream, title_size, stream_size);
        char* text = read_text(stream, text_size, stream_size);
        write64(offsets, uint64_t(ftello(documents)));
        write32(documents, url_size);
        write32(documents, title_size);
        require(fwrite(url, 1, url_size, documents) == url_size, "Write error");
        require(fwrite(title, 1, title_size, documents) == title_size, "Write error");

        try {
            tokenize(
                text, size_t(text_size),
                [](const char* token, size_t length, void* context) {
                    static_cast<Builder*>(context)->add(token, length);
                },
                &builder);
        } catch (...) {
            free(url);
            free(title);
            free(text);
            throw;
        }
        free(url);
        free(title);
        free(text);
        bytes += text_size;
        ++builder.document;
    }
    std::unique_ptr<FILE, int (*)(FILE*)> merged_owner(fopen(builder.merge(), "rb"), &fclose);
    FILE* merged = merged_owner.get();
    require(merged != nullptr, "Cannot open file");
    struct stat merged_info{};
    require(fstat(fileno(merged), &merged_info) == 0 && merged_info.st_size >= 0,
            "Cannot stat file");
    uint64_t merged_size = uint64_t(merged_info.st_size);
    Pair pair{nullptr, 0, 0};
    char* current = nullptr;
    Term term{};
    uint64_t terms = 0;

    while (read_pair(merged, pair, merged_size)) {
        if (!current || strcmp(current, pair.text)) {
            if (current)
                write_term(dictionary, term);
            free(current);
            current = strdup(pair.text);
            require(current != nullptr, "Out of memory");
            term = {uint64_t(ftello(strings)), uint64_t(ftello(postings)), 0,
                    uint32_t(strlen(current)), 0};
            require(fwrite(current, 1, term.length, strings) == term.length, "Write error");
            ++terms;
        }
        term.frequency += pair.frequency;
        ++term.documents;
        write32(postings, pair.document);
    }
    if (current)
        write_term(dictionary, term);
    free(current);
    free(pair.text);
    char zeros[header_size]{};
    require(fwrite(zeros, 1, sizeof(zeros), index) == sizeof(zeros), "Write error");
    Header h{stemming ? 1u : 0u,
             builder.document,
             terms,
             builder.tokens,
             bytes,
             header_size,
             0,
             0,
             0,
             0,
             0};
    append(index, dictionary);
    h.strings = uint64_t(ftello(index));
    append(index, strings);
    h.postings = uint64_t(ftello(index));
    append(index, postings);
    h.offsets = uint64_t(ftello(index));
    append(index, offsets);
    h.metadata = uint64_t(ftello(index));
    append(index, documents);
    h.size = uint64_t(ftello(index));
    write_header(index, h);
    require(fflush(index) == 0, "Write error");
    require(link(paths[5], output) == 0, "Cannot publish index; destination may already exist");

    for (char* path : paths)
        free(path);
    timespec wall_end{};
    require(clock_gettime(CLOCK_MONOTONIC, &wall_end) == 0, "Cannot read clock");
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read resource usage");
    double seconds = double(wall_end.tv_sec - wall_start.tv_sec) +
                     double(wall_end.tv_nsec - wall_start.tv_nsec) / 1e9;
    printf("{\"documents\":%u,\"terms\":%llu,\"tokens\":%llu,\"text_bytes\":%llu,\"index_bytes\":%"
           "llu,\"blocks\":%u,\"merge_files\":%u,\"cpu_seconds\":%.6f,\"elapsed_seconds\":%.6f,"
           "\"seconds_per_document\":%.9f,\"seconds_per_kib\":%.9f,\"peak_rss_kib\":%ld}\n",
           builder.document, (unsigned long long)terms, (unsigned long long)builder.tokens,
           (unsigned long long)bytes, (unsigned long long)h.size, builder.blocks,
           builder.serial - builder.blocks, double(clock() - start) / CLOCKS_PER_SEC, seconds,
           builder.document ? seconds / builder.document : 0, bytes ? seconds * 1024 / bytes : 0,
           usage.ru_maxrss);
}

Index::Index(const char* path)
    : file_owner(fopen(path, "rb"), &fclose), file(file_owner.get()), header{} {
    require(file != nullptr, "Cannot open file");
    struct stat file_info{};
    require(fstat(fileno(file), &file_info) == 0 && file_info.st_size >= 0, "Cannot stat file");
    uint64_t file_size = uint64_t(file_info.st_size);

    char magic[8];
    require(fread(magic, 1, 8, file) == 8, "Truncated file or read error");
    require(memcmp(magic, "IRIDX001", 8) == 0 && read32(file) == 1, "Unsupported index format");
    header.flags = read32(file);
    header.documents = read64(file);
    header.terms = read64(file);
    header.tokens = read64(file);
    header.bytes = read64(file);
    header.dictionary = read64(file);
    header.strings = read64(file);
    header.postings = read64(file);
    header.offsets = read64(file);
    header.metadata = read64(file);
    header.size = read64(file);
    require(header.flags <= 1 && header.documents <= UINT32_MAX &&
                header.dictionary == header_size && header.size == file_size &&
                header.dictionary <= header.strings && header.strings <= header.postings &&
                header.postings <= header.offsets && header.offsets <= header.metadata &&
                header.metadata <= header.size &&
                header.terms == (header.strings - header.dictionary) / 32 &&
                (header.strings - header.dictionary) % 32 == 0 &&
                header.metadata - header.offsets == header.documents * 8 &&
                (header.offsets - header.postings) % 4 == 0,
            "Invalid index header");
}

Term Index::term(uint64_t id) {
    require(id < header.terms, "Invalid term ID");
    require(fseeko(file, static_cast<off_t>(header.dictionary + id * 32), SEEK_SET) == 0,
            "Cannot seek file");
    Term t{};
    t.text = read64(file);
    t.postings = read64(file);
    t.frequency = read64(file);
    t.length = read32(file);
    t.documents = read32(file);
    require(t.length && t.text <= header.postings - header.strings &&
                t.length <= header.postings - header.strings - t.text &&
                t.postings <= header.offsets - header.postings &&
                uint64_t(t.documents) * 4 <= header.offsets - header.postings - t.postings &&
                t.documents <= header.documents && t.frequency >= t.documents,
            "Invalid dictionary entry");
    return t;
}

bool Index::find(const char* text, Term& found) {
    uint64_t low = 0, high = header.terms;

    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        Term entry = term(middle);
        require(fseeko(file, static_cast<off_t>(header.strings + entry.text), SEEK_SET) == 0,
                "Cannot seek file");
        char* value = read_text(file, entry.length, header.postings);
        int order = strcmp(text, value);
        free(value);
        if (!order) {
            found = entry;
            return true;
        }
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }

    return false;
}

void Index::document(uint32_t id, char*& url, char*& title) {
    require(id < header.documents, "Invalid document ID");
    require(fseeko(file, static_cast<off_t>(header.offsets + uint64_t(id) * 8), SEEK_SET) == 0,
            "Cannot seek file");
    uint64_t offset = read64(file);
    require(offset <= header.size - header.metadata && header.size - header.metadata - offset >= 8,
            "Invalid document offset");
    require(fseeko(file, static_cast<off_t>(header.metadata + offset), SEEK_SET) == 0,
            "Cannot seek file");
    uint32_t url_size = read32(file), title_size = read32(file);
    require(url_size && url_size <= max_text && title_size <= max_text,
            "Invalid document metadata");
    url = read_text(file, url_size, header.size);
    try {
        title = read_text(file, title_size, header.size);
    } catch (...) {
        free(url);
        url = nullptr;
        throw;
    }
}

namespace {

void match(Index& index, const char* query, Array<uint32_t>& result) {
    Query q(index.header.flags & 1);
    tokenize(
        query, strlen(query),
        [](const char* text, size_t length, void* context) {
            auto& q = *static_cast<Query*>(context);
            char* value = strndup(text, length);
            require(value != nullptr, "Out of memory");
            try {
                if (q.stemming)
                    stem(value);
                q.words.push(value);
            } catch (...) {
                free(value);
                throw;
            }
        },
        &q);
    Array<Term> terms;
    bool missing = false;

    for (size_t i = 0; i < q.words.size; ++i) {
        Term term{};
        if (index.find(q.words[i], term))
            terms.push(term);
        else
            missing = true;
    }
    sort(terms.data, terms.size,
         [](const Term& a, const Term& b) { return a.documents < b.documents; });
    if (!missing && terms.size) {
        require(fseeko(index.file, static_cast<off_t>(index.header.postings + terms[0].postings),
                       SEEK_SET) == 0,
                "Cannot seek file");

        for (uint32_t i = 0; i < terms[0].documents; ++i) {
            uint32_t id = read32(index.file);
            require(id < index.header.documents && (!i || id > result[i - 1]),
                    "Invalid posting list");
            result.push(id);
        }

        for (size_t i = 1; i < terms.size && result.size; ++i) {
            require(fseeko(index.file,
                           static_cast<off_t>(index.header.postings + terms[i].postings),
                           SEEK_SET) == 0,
                    "Cannot seek file");
            size_t cursor = 0, kept = 0;
            uint32_t previous = 0;

            for (uint32_t j = 0; j < terms[i].documents; ++j) {
                uint32_t id = read32(index.file);
                require(id < index.header.documents && (!j || id > previous),
                        "Invalid posting list");
                previous = id;

                while (cursor < result.size && result[cursor] < id)
                    ++cursor;
                if (cursor < result.size && result[cursor] == id)
                    result[kept++] = id;
            }
            result.size = kept;
        }
    }
}

}

void search(Index& index, const char* query, size_t limit, Index* exact) {
    Array<uint32_t> result;
    match(index, query, result);
    Array<uint32_t> preferred;
    Array<uint32_t> ordered;
    if (exact) {
        require(index.header.flags == 1 && exact->header.flags == 0 &&
                    index.header.documents == exact->header.documents &&
                    index.header.bytes == exact->header.bytes &&
                    index.header.tokens == exact->header.tokens,
                "Incompatible exact and stem indexes");
        match(*exact, query, preferred);

        for (size_t i = 0; i < preferred.size; ++i) {
            char *a = nullptr, *b = nullptr, *title_a = nullptr, *title_b = nullptr;
            index.document(preferred[i], a, title_a);
            exact->document(preferred[i], b, title_b);
            bool same = !strcmp(a, b);
            free(a);
            free(b);
            free(title_a);
            free(title_b);
            require(same, "Indexes refer to different documents");
            ordered.push(preferred[i]);
        }

        size_t cursor = 0;

        for (size_t i = 0; i < result.size; ++i) {
            while (cursor < preferred.size && preferred[cursor] < result[i])
                ++cursor;
            if (cursor == preferred.size || preferred[cursor] != result[i])
                ordered.push(result[i]);
        }

        require(ordered.size == result.size, "Exact results are not a subset of stem results");

        for (size_t i = 0; i < result.size; ++i)
            result[i] = ordered[i];
    }
    printf("{\"total\":%zu,\"results\":[", result.size);

    for (size_t i = 0; i < result.size && i < limit; ++i) {
        char *url = nullptr, *title = nullptr;
        index.document(result[i], url, title);
        if (i)
            putchar(',');
        printf("{\"id\":%u,\"url\":", result[i]);
        print_json_string(url);
        printf(",\"title\":");
        print_json_string(title);
        putchar('}');
        free(url);
        free(title);
    }
    puts("]}");
}

void statistics(const char* path) {
    Index index(path);
    uint64_t length = 0;

    for (uint64_t i = 0; i < index.header.terms; ++i) {
        auto term = index.term(i);
        require(
            fseeko(index.file, static_cast<off_t>(index.header.strings + term.text), SEEK_SET) == 0,
            "Cannot seek file");
        char* text = read_text(index.file, term.length, index.header.postings);

        for (uint32_t j = 0; j < term.length; ++j)
            if ((static_cast<unsigned char>(text[j]) & 0xc0) != 0x80)
                ++length;
        free(text);
    }

    const auto& h = index.header;
    printf("{\"documents\":%llu,\"terms\":%llu,\"tokens\":%llu,\"text_bytes\":%llu,\"index_bytes\":"
           "%llu,\"average_term_length\":%.9f,\"stemming\":%s}\n",
           (unsigned long long)h.documents, (unsigned long long)h.terms,
           (unsigned long long)h.tokens, (unsigned long long)h.bytes, (unsigned long long)h.size,
           h.terms ? double(length) / h.terms : 0, h.flags ? "true" : "false");
}

void frequencies(const char* path) {
    Index index(path);

    for (uint64_t i = 0; i < index.header.terms; ++i) {
        auto t = index.term(i);
        require(fseeko(index.file, static_cast<off_t>(index.header.strings + t.text), SEEK_SET) ==
                    0,
                "Cannot seek file");
        char* text = read_text(index.file, t.length, index.header.postings);
        printf("%s\t%llu\t%u\n", text, (unsigned long long)t.frequency, t.documents);
        free(text);
    }
}

}
