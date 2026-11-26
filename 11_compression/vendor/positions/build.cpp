#include "index.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <future>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ir::pos {
namespace {

using Text = std::unique_ptr<char, decltype(&free)>;

std::string prepare_document(const char* text, size_t size, uint32_t normalization) {
    struct Tokens {
        uint32_t normalization;
        std::string bytes;
        std::string long_word;

        void append(const char* token, size_t length) {
            require(length < UINT32_MAX, "Term too long");
            if (normalization) {
                char local[512];
                if (length < sizeof(local)) {
                    memcpy(local, token, length);
                    local[length] = 0;
                    normalize(local, sizeof(local), normalization);
                    bytes.append(local);
                } else {
                    long_word.assign(token, length);
                    normalize(long_word.data(), length + 1, normalization);
                    bytes.append(long_word.c_str());
                }
            } else {
                bytes.append(token, length);
            }
            bytes.push_back('\0');
        }
    };

    Tokens result{normalization, {}, {}};
    result.bytes.reserve(size);
    tokenize(
        text, size,
        [](const char* token, size_t length, void* context) {
            static_cast<Tokens*>(context)->append(token, length);
        },
        &result);
    return std::move(result.bytes);
}

class DocumentWorkers {
    using Task = std::packaged_task<std::string()>;

    std::mutex mutex;
    std::condition_variable available;
    std::deque<Task> queue;
    std::vector<std::thread> threads;
    bool stopping = false;

    void work() {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                task = std::move(queue.front());
                queue.pop_front();
            }
            task();
        }
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        available.notify_all();
        for (auto& thread : threads)
            thread.join();
    }

public:
    explicit DocumentWorkers(unsigned count) {
        threads.reserve(count);
        try {
            for (unsigned i = 0; i < count; ++i)
                threads.emplace_back([this] { work(); });
        } catch (...) {
            stop();
            throw;
        }
    }

    ~DocumentWorkers() {
        stop();
    }

    std::future<std::string> submit(Text text, size_t size, uint32_t normalization) {
        Task task([text = std::move(text), size, normalization]() mutable {
            // Release the source bytes as soon as tokenization finishes.
            const auto source = std::move(text);
            return prepare_document(source.get(), size, normalization);
        });
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(task));
        }
        available.notify_one();
        return result;
    }
};

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

    std::string file(const char* name) const {
        return std::string(path) + '/' + name;
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
};

struct Occurrence {
    const char* text;
    uint32_t document, position;
};

int compare(const Occurrence& a, const Occurrence& b) {
    int order = strcmp(a.text, b.text);
    if (order)
        return order;
    if (a.document != b.document)
        return a.document < b.document ? -1 : 1;
    return a.position < b.position ? -1 : a.position > b.position;
}

constexpr size_t io_block_size = 1024 * 1024;

struct BlockWriter {
    FILE* file;
    std::unique_ptr<unsigned char[]> buffer{new unsigned char[io_block_size]};
    uint64_t written = 0;
    size_t used = 0;

    explicit BlockWriter(FILE* output) : file(output) {}

    uint64_t offset() const {
        return written + used;
    }

    void flush() {
        require(fwrite(buffer.get(), 1, used, file) == used, "Write error");
        written += used;
        used = 0;
    }

    void write(const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size) {
            if (used == io_block_size)
                flush();
            const size_t count = std::min(size, io_block_size - used);
            memcpy(buffer.get() + used, bytes, count);
            used += count;
            bytes += count;
            size -= count;
        }
    }

    void integer(uint32_t value, bool compressed = true) {
        unsigned char bytes[5];
        size_t length = 0;
        if (compressed) {
            while (value >= 128) {
                bytes[length++] = static_cast<unsigned char>((value & 127) | 128);
                value >>= 7;
            }
            bytes[length++] = static_cast<unsigned char>(value);
        } else {
            for (; length < 4; ++length)
                bytes[length] = static_cast<unsigned char>(value >> (length * 8));
        }
        write(bytes, length);
    }
};

struct RunWriter {
    std::unique_ptr<FILE, int (*)(FILE*)> file_owner{nullptr, &fclose};
    BlockWriter output;
    std::string previous_word;
    uint32_t document = 0, position = 0;

    explicit RunWriter(const char* path)
        : file_owner(fopen(path, "wb"), &fclose), output(file_owner.get()) {
        require(file_owner != nullptr, "Cannot open file");
    }

    void finish() {
        output.flush();
        require(fflush(file_owner.get()) == 0, "Write error");
    }

    void add(const Occurrence& entry) {
        bool same = !strcmp(previous_word.c_str(), entry.text);
        if (!same) {
            size_t length = strlen(entry.text);
            require(length < UINT32_MAX, "Term too long");
            output.integer(uint32_t(length + 1));
            output.write(entry.text, length);
            previous_word.assign(entry.text, length);
            document = position = 0;
        } else
            output.integer(0);
        require(entry.document >= document, "Unsorted run document");
        output.integer(entry.document - document);
        if (entry.document != document)
            position = 0;
        require(entry.position >= position, "Unsorted run position");
        output.integer(entry.position - position);
        document = entry.document;
        position = entry.position;
    }
};

struct RunReader {
    std::unique_ptr<FILE, int (*)(FILE*)> file_owner{nullptr, &fclose};
    FILE* file = nullptr;
    uint64_t unread = 0;
    std::array<unsigned char, io_block_size> buffer;
    size_t cursor = 0, buffered = 0;
    std::string word;
    uint32_t document = 0, position = 0;
    Occurrence current{nullptr, 0, 0};

    explicit RunReader(const char* path)
        : file_owner(fopen(path, "rb"), &fclose), file(file_owner.get()) {
        require(file != nullptr, "Cannot open file");
        struct stat info{};
        require(fstat(fileno(file), &info) == 0 && info.st_size >= 0, "Cannot stat file");
        unread = uint64_t(info.st_size);
    }

    void refill() {
        require(unread, "Truncated sorted run");
        buffered = size_t(std::min<uint64_t>(unread, buffer.size()));
        require(fread(buffer.data(), 1, buffered, file) == buffered,
                "Truncated file or read error");
        unread -= buffered;
        cursor = 0;
    }

    uint32_t integer() {
        uint32_t result = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            if (cursor == buffered)
                refill();
            const auto byte = buffer[cursor++];
            require(shift != 28 || byte <= 15, "Variable-byte integer overflow");
            result |= uint32_t(byte & 127) << shift;
            if (!(byte & 128)) {
                require(!shift || byte, "Noncanonical variable-byte integer");
                return result;
            }
        }
        throw Error{"Variable-byte integer overflow"};
    }

    bool next() {
        if (cursor == buffered && !unread)
            return false;
        const uint32_t length = integer();
        if (length) {
            const size_t size = length - 1;
            require(size && size <= unread + buffered - cursor, "Invalid string length");
            word.resize(size);
            for (size_t copied = 0; copied < size;) {
                if (cursor == buffered)
                    refill();
                const size_t count = std::min(size - copied, buffered - cursor);
                memcpy(word.data() + copied, buffer.data() + cursor, count);
                cursor += count;
                copied += count;
            }
            require(!memchr(word.data(), 0, size), "NUL in text");
            document = position = 0;
        }

        require(!word.empty(), "Missing term in sorted run");
        uint32_t gap = integer();
        uint64_t id = uint64_t(document) + gap;
        require(id <= UINT32_MAX, "Run document gap overflow");
        if (gap)
            position = 0;
        uint64_t at = uint64_t(position) + integer();
        require(at <= UINT32_MAX, "Run position gap overflow");
        document = uint32_t(id);
        position = uint32_t(at);
        current = {word.c_str(), document, position};
        return true;
    }
};

template <class Sink>
void merge_runs(const std::vector<std::string>& runs, size_t first, size_t last, Sink& sink) {
    std::vector<std::unique_ptr<RunReader>> readers;
    std::vector<RunReader*> heap;
    readers.reserve(last - first);
    heap.reserve(last - first);
    for (size_t i = first; i < last; ++i) {
        auto reader = std::make_unique<RunReader>(runs[i].c_str());
        if (reader->next())
            heap.push_back(reader.get());
        readers.push_back(std::move(reader));
    }

    const auto later = [](const RunReader* a, const RunReader* b) {
        return compare(a->current, b->current) > 0;
    };
    std::make_heap(heap.begin(), heap.end(), later);
    while (!heap.empty()) {
        RunReader* first = heap.front();
        sink.add(first->current);
        if (!first->next()) {
            first->file_owner.reset();
            heap.front() = heap.back();
            heap.pop_back();
        }
        // Only the head changed, so it can only move down the heap.
        for (size_t parent = 0, child = 1; child < heap.size(); child = 2 * parent + 1) {
            if (child + 1 < heap.size() && later(heap[child], heap[child + 1]))
                ++child;
            if (!later(heap[parent], heap[child]))
                break;
            std::swap(heap[parent], heap[child]);
            parent = child;
        }
    }
}

struct Name {
    char* text;
    uint32_t length, rank;
};

struct Record {
    uint32_t term, document, position;
};

uint64_t hash_word(const char* word, size_t length) {
    uint64_t value = 14695981039346656037ull;

    for (size_t i = 0; i < length; ++i) {
        value ^= static_cast<unsigned char>(word[i]);
        value *= 1099511628211ull;
    }

    return value;
}

struct Builder {
    Workspace workspace;
    Array<Record> entries;
    Array<Name> names;
    std::vector<std::string> runs;
    uint32_t* table = nullptr;
    size_t table_size = 0, budget, used = 0;
    uint32_t document = 0, position = 0;
    unsigned serial = 0, blocks = 0;
    uint64_t tokens = 0;

    Builder(const char* output, size_t memory) : workspace(output), budget(memory) {}

    ~Builder() {
        for (size_t i = 0; i < names.size; ++i)
            free(names[i].text);

        free(table);
    }

    size_t slot(const char* text, size_t length) const {
        size_t at = size_t(hash_word(text, length)) & (table_size - 1);

        while (table[at]) {
            const auto& name = names[table[at] - 1];
            if (name.length == length && !memcmp(name.text, text, length))
                break;
            at = (at + 1) & (table_size - 1);
        }

        return at;
    }

    void grow() {
        size_t capacity = table_size ? table_size * 2 : 32;
        require(capacity > table_size && capacity <= SIZE_MAX / sizeof(uint32_t),
                "Dictionary too large");
        auto* replacement = static_cast<uint32_t*>(malloc(capacity * sizeof(uint32_t)));
        require(replacement != nullptr, "Out of memory");
        memset(replacement, 0, capacity * sizeof(uint32_t));
        free(table);
        table = replacement;
        table_size = capacity;

        for (size_t i = 0; i < names.size; ++i)
            table[slot(names[i].text, names[i].length)] = uint32_t(i + 1);
    }

    std::string name() {
        char name[40];
        snprintf(name, sizeof(name), "run-%u", serial++);
        return workspace.file(name);
    }

    void flush() {
        if (!entries.size)
            return;
        Array<uint32_t> order;

        for (size_t i = 0; i < names.size; ++i)
            order.push(uint32_t(i));
        sort(order.data, order.size,
             [&](uint32_t a, uint32_t b) { return strcmp(names[a].text, names[b].text) < 0; });

        for (size_t i = 0; i < order.size; ++i)
            names[order[i]].rank = uint32_t(i);

        for (size_t i = 0; i < entries.size; ++i)
            entries[i].term = names[entries[i].term].rank;
        sort(entries.data, entries.size, [](const Record& a, const Record& b) {
            if (a.term != b.term)
                return a.term < b.term;
            return a.document != b.document ? a.document < b.document : a.position < b.position;
        });
        auto path = name();
        RunWriter file(path.c_str());

        for (size_t i = 0; i < entries.size; ++i) {
            const auto& entry = entries[i];
            file.add({names[order[entry.term]].text, entry.document, entry.position});
        }
        file.finish();

        for (size_t i = 0; i < names.size; ++i)
            free(names[i].text);
        names.size = 0;
        entries.size = 0;
        used = 0;
        memset(table, 0, table_size * sizeof(uint32_t));
        runs.push_back(std::move(path));
        ++blocks;
    }

    void add_word(const char* text, size_t length) {
        require(position < UINT32_MAX, "Document has too many tokens");
        if (!table_size)
            grow();
        size_t at = slot(text, length);
        bool present = table[at] != 0;
        size_t cost = sizeof(Record) * 2 + (present ? 0 : length + 1 + sizeof(Name) * 2 + 24);
        if (used + cost > budget) {
            flush();
            at = slot(text, length);
            present = false;
        }
        if (!present) {
            if ((names.size + 1) * 3 > table_size * 2) {
                grow();
                at = slot(text, length);
            }

            require(names.size < UINT32_MAX, "Block dictionary too large");
            names.push({nullptr, uint32_t(length), 0});
            names[names.size - 1].text = strndup(text, length);
            require(names[names.size - 1].text != nullptr, "Out of memory");
            table[at] = uint32_t(names.size);
            used += length + 1 + sizeof(Name) * 2 + 24;
        }
        entries.push({table[at] - 1, document, position++});
        ++tokens;
        used += sizeof(Record) * 2;
    }

    template <class Sink> void merge(Sink& sink) {
        constexpr size_t fan_in = 128;
        flush();
        while (runs.size() > fan_in) {
            std::vector<std::string> next;
            for (size_t first = 0; first < runs.size(); first += fan_in) {
                const size_t last = std::min(first + fan_in, runs.size());
                if (last - first == 1) {
                    next.push_back(std::move(runs[first]));
                    continue;
                }
                auto path = name();
                RunWriter out(path.c_str());
                merge_runs(runs, first, last, out);
                out.finish();
                for (size_t i = first; i < last; ++i)
                    require(unlink(runs[i].c_str()) == 0, "Cannot remove sorted run");
                next.push_back(std::move(path));
            }
            runs.swap(next);
        }
        merge_runs(runs, 0, runs.size(), sink);
        for (const auto& path : runs)
            require(unlink(path.c_str()) == 0, "Cannot remove sorted run");
        runs.clear();
    }
};

struct Inverter {
    struct Frequency {
        uint32_t document, count;
    };

    FILE* dictionary;
    FILE* strings;
    BlockWriter postings;
    FILE* skips;
    Header& header;
    double* norms;
    Term term;
    char* word = nullptr;
    uint32_t document = 0, previous = 0;
    Array<uint32_t> positions;
    Array<Frequency> frequencies;
    uint64_t block_offset = 0;
    uint32_t block_prior = 0, block_ordinal = 0;

    Inverter(FILE* dictionary_file, FILE* strings_file, FILE* postings_file, FILE* skips_file,
             Header& index_header, double* document_norms)
        : dictionary(dictionary_file), strings(strings_file), postings(postings_file),
          skips(skips_file), header(index_header), norms(document_norms) {}

    ~Inverter() {
        free(word);
    }

    void block() {
        if (!header.stride || term.documents == block_ordinal)
            return;
        write32(skips, previous);
        write32(skips, block_prior);
        write64(skips, block_offset);
        write32(skips, block_ordinal);
        write32(skips, term.documents - block_ordinal);
        ++term.blocks;
    }

    void posting() {
        if (!positions.size)
            return;
        bool compressed = header.flags & 2;
        if (header.stride && term.documents % header.stride == 0) {
            block_offset = postings.offset() - term.postings;
            block_prior = previous;
            block_ordinal = term.documents;
        }
        const auto count = uint32_t(positions.size);
        postings.integer(compressed ? document - previous : document, compressed);
        postings.integer(count, compressed);
        uint32_t last = 0;

        for (size_t i = 0; i < positions.size; ++i) {
            postings.integer(compressed ? positions[i] - last : positions[i], compressed);
            last = positions[i];
        }
        previous = document;
        frequencies.push({document, count});
        ++term.documents;
        term.frequency += positions.size;
        if (header.stride && term.documents % header.stride == 0)
            block();
        positions.size = 0;
    }

    void finish() {
        if (!word)
            return;
        posting();
        if (header.stride && term.documents % header.stride)
            block();
        term.bytes = postings.offset() - term.postings;
        write_term(dictionary, term);
        double idf = log((double(header.documents) + 1) / (term.documents + 1)) + 1;
        for (size_t i = 0; i < frequencies.size; ++i) {
            const auto& frequency = frequencies[i];
            double weight = (1 + log(double(frequency.count))) * idf;
            norms[frequency.document] += weight * weight;
        }
        frequencies.size = 0;
        free(word);
        word = nullptr;
    }

    void add(const Occurrence& occurrence) {
        if (!word || strcmp(word, occurrence.text)) {
            finish();
            word = strdup(occurrence.text);
            require(word != nullptr, "Out of memory");
            term = {};
            term.text = uint64_t(ftello(strings));
            term.postings = postings.offset();
            term.skips = uint64_t(ftello(skips));
            term.length = uint32_t(strlen(word));
            require(fwrite(word, 1, term.length, strings) == term.length, "Write error");
            previous = 0;
            block_ordinal = 0;
            ++header.terms;
            document = occurrence.document;
        } else if (document != occurrence.document) {
            posting();
            document = occurrence.document;
        }

        require(!positions.size || positions[positions.size - 1] < occurrence.position,
                "Duplicate occurrence");
        positions.push(occurrence.position);
    }
};

void add_to_fingerprint(Header& header, const char* text) {
    do {
        header.fingerprint ^= static_cast<unsigned char>(*text);
        header.fingerprint *= 1099511628211ull;
    } while (*text++);
}

void index_documents(FILE* source, uint64_t source_size, FILE* metadata, FILE* offsets,
                     FILE* lengths, uint32_t normalization, Header& header, Builder& builder) {
    struct PendingDocument {
        size_t size;
        std::future<std::string> tokens;
    };

    const unsigned cpus = std::thread::hardware_concurrency();
    // Leave room for the indexing thread and for a second build on the same machine.
    const unsigned worker_count = std::clamp(cpus / 2, 2u, 4u) - 1;
    constexpr size_t pending_limit = 32 * 1024 * 1024;
    DocumentWorkers workers(worker_count);
    std::deque<PendingDocument> pending;
    size_t pending_bytes = 0;
    const auto commit = [&] {
        auto& document = pending.front();
        const auto tokens = document.tokens.get();
        builder.position = 0;

        const char* word = tokens.data();
        const char* end = word + tokens.size();

        while (word < end) {
            const size_t length = strlen(word);
            builder.add_word(word, length);
            word += length + 1;
        }

        write32(lengths, builder.position);
        ++builder.document;
        pending_bytes -= document.size;
        pending.pop_front();
    };

    while (uint64_t(ftello(source)) < source_size) {
        require(uint64_t(builder.document) + pending.size() < UINT32_MAX, "Too many documents");
        const uint32_t url_size = read32(source), title_size = read32(source);
        const uint64_t size = read64(source);
        constexpr uint64_t max_field = 64 * 1024 * 1024;
        require(url_size && url_size <= max_field && title_size <= max_field && size <= max_field,
                "Invalid document size");

        while (!pending.empty() &&
               (pending.size() >= worker_count * 4 || pending_bytes + size > pending_limit))
            commit();

        const Text url(read_text(source, url_size, source_size), &free);
        const Text title(read_text(source, title_size, source_size), &free);
        Text body(read_text(source, size, source_size), &free);

        add_to_fingerprint(header, url.get());
        add_to_fingerprint(header, title.get());
        add_to_fingerprint(header, body.get());

        write64(offsets, uint64_t(ftello(metadata)));
        write32(metadata, url_size);
        write32(metadata, title_size);
        require(fwrite(url.get(), 1, url_size, metadata) == url_size, "Write error");
        require(fwrite(title.get(), 1, title_size, metadata) == title_size, "Write error");

        pending.push_back(
            {size_t(size), workers.submit(std::move(body), size_t(size), normalization)});
        pending_bytes += size_t(size);
        header.bytes += size;
    }

    while (!pending.empty())
        commit();
}

} // namespace

void build(const char* input, const char* output, uint32_t normalization, bool compressed,
           size_t memory, uint32_t stride) {
    struct stat status;
    require(lstat(output, &status) != 0, "Output already exists");
    Builder builder(output, memory);
    const char* names[] = {"metadata", "offsets", "dictionary", "strings",
                           "postings", "skips",   "lengths",    "index"};
    std::array<std::string, std::size(names)> paths;

    for (size_t i = 0; i < 8; ++i)
        paths[i] = builder.workspace.file(names[i]);

    std::unique_ptr<FILE, int (*)(FILE*)> metadata_owner(fopen(paths[0].c_str(), "w+b"), &fclose);
    FILE* metadata = metadata_owner.get();
    require(metadata != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> offsets_owner(fopen(paths[1].c_str(), "w+b"), &fclose);
    FILE* offsets = offsets_owner.get();
    require(offsets != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> dictionary_owner(fopen(paths[2].c_str(), "w+b"), &fclose);
    FILE* dictionary = dictionary_owner.get();
    require(dictionary != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> strings_owner(fopen(paths[3].c_str(), "w+b"), &fclose);
    FILE* strings = strings_owner.get();
    require(strings != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> postings_owner(fopen(paths[4].c_str(), "w+b"), &fclose);
    FILE* postings = postings_owner.get();
    require(postings != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> skips_owner(fopen(paths[5].c_str(), "w+b"), &fclose);
    FILE* skips = skips_owner.get();
    require(skips != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> lengths_owner(fopen(paths[6].c_str(), "w+b"), &fclose);
    FILE* lengths = lengths_owner.get();
    require(lengths != nullptr, "Cannot open file");

    std::unique_ptr<FILE, int (*)(FILE*)> out_owner(fopen(paths[7].c_str(), "w+b"), &fclose);
    FILE* out = out_owner.get();
    require(out != nullptr, "Cannot open file");

    constexpr size_t source_buffer_size = 4 * 1024 * 1024;
    std::unique_ptr<char[]> source_buffer(new char[source_buffer_size]);
    std::unique_ptr<FILE, int (*)(FILE*)> source_owner(fopen(input, "rb"), &fclose);
    FILE* source = source_owner.get();
    require(source != nullptr, "Cannot open file");
    require(setvbuf(source, source_buffer.get(), _IOFBF, source_buffer_size) == 0,
            "Cannot buffer corpus stream");

    struct stat source_info{};
    require(fstat(fileno(source), &source_info) == 0 && source_info.st_size >= 0,
            "Cannot stat file");
    uint64_t source_size = uint64_t(source_info.st_size);
    char magic[8];
    require(fread(magic, 1, 8, source) == 8, "Truncated file or read error");
    require(!memcmp(magic, "IRDOC001", 8), "Invalid corpus stream");

    Header header;
    header.flags = normalization | (compressed ? 2u : 0u);
    header.dictionary_id = (normalization & 4) ? lemma_dictionary_id() : 0;
    header.stride = stride;

    timespec start{}, end{};
    require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "Cannot read clock");

    index_documents(source, source_size, metadata, offsets, lengths, normalization, header,
                    builder);
    header.documents = builder.document;
    header.tokens = builder.tokens;
    double* norms = static_cast<double*>(calloc(size_t(header.documents) + 1, sizeof(double)));
    require(norms != nullptr, "Out of memory");

    try {
        Inverter inverter(dictionary, strings, postings, skips, header, norms);
        builder.merge(inverter);
        inverter.finish();
        inverter.postings.flush();

        char zeros[header_size]{};
        require(fwrite(zeros, 1, header_size, out) == header_size, "Write error");

        append(out, dictionary);
        header.strings = uint64_t(ftello(out));
        append(out, strings);
        header.postings = uint64_t(ftello(out));
        append(out, postings);
        header.skips = uint64_t(ftello(out));
        append(out, skips);
        header.offsets = uint64_t(ftello(out));
        append(out, offsets);
        header.metadata = uint64_t(ftello(out));
        append(out, metadata);
        header.lengths = uint64_t(ftello(out));
        append(out, lengths);
        header.norms = uint64_t(ftello(out));

        for (uint64_t i = 0; i < header.documents; ++i)
            write_double(out, sqrt(norms[i]));

        header.size = uint64_t(ftello(out));
        write_header(out, header);
        require(fflush(out) == 0, "Write error");
        require(link(paths[7].c_str(), output) == 0, "Cannot publish positional index");
    } catch (...) {
        free(norms);

        throw;
    }
    free(norms);

    require(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "Cannot read clock");
    double seconds = double(end.tv_sec - start.tv_sec) + double(end.tv_nsec - start.tv_nsec) / 1e9;
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Cannot read process memory");

    printf(
        "{\"documents\":%llu,\"terms\":%llu,\"positions\":%llu,\"text_bytes\":%llu,\"index_bytes\":"
        "%llu,\"coordinate_bytes\":%llu,\"skip_bytes\":%llu,\"blocks\":%u,\"elapsed_seconds\":%.9f,"
        "\"kib_per_second\":%.6f,\"peak_rss_kib\":%ld,\"compressed\":%s,\"skip_stride\":%u}\n",
        (unsigned long long)header.documents, (unsigned long long)header.terms,
        (unsigned long long)header.tokens, (unsigned long long)header.bytes,
        (unsigned long long)header.size, (unsigned long long)(header.skips - header.postings),
        (unsigned long long)(header.offsets - header.skips), builder.blocks, seconds,
        seconds ? header.bytes / (seconds * 1024) : 0, usage.ru_maxrss,
        compressed ? "true" : "false", stride);
}

} // namespace ir::pos
