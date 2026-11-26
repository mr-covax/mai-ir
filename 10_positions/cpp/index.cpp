#include "index.hpp"
#include <cmath>

namespace ir::pos {

void write_header(FILE* file, const Header& header) {
    require(fseeko(file, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");
    require(fwrite("IRPOS003", 1, 8, file) == 8, "Write error");
    write32(file, 3);
    write32(file, header.flags);
    write64(file, header.documents);
    write64(file, header.terms);
    write64(file, header.tokens);
    write64(file, header.bytes);
    write64(file, header.dictionary);
    write64(file, header.strings);
    write64(file, header.postings);
    write64(file, header.offsets);
    write64(file, header.metadata);
    write64(file, header.size);
    write64(file, header.lengths);
    write64(file, header.skips);
    write64(file, header.norms);
    write64(file, header.fingerprint);
    write32(file, header.stride);
    write32(file, header.dictionary_id);
    write64(file, header_size);
}

void write_term(FILE* file, const Term& term) {
    write64(file, term.text);
    write64(file, term.postings);
    write64(file, term.bytes);
    write64(file, term.skips);
    write64(file, term.frequency);
    write32(file, term.length);
    write32(file, term.documents);
    write32(file, term.blocks);
    write32(file, 0);
}

uint32_t decode(FILE* file, bool compressed, uint64_t end) {
    require(uint64_t(ftello(file)) <= end, "Posting offset outside section");
    if (!compressed) {
        require(end - uint64_t(ftello(file)) >= 4, "Truncated posting integer");
        return read32(file);
    }

    uint32_t result = 0;

    for (unsigned shift = 0; shift <= 28; shift += 7) {
        require(uint64_t(ftello(file)) < end, "Truncated variable-byte integer");
        unsigned char byte = 0;
        require(fread(&byte, 1, 1, file) == 1, "Truncated file or read error");
        require(shift != 28 || byte <= 15, "Variable-byte integer overflow");
        result |= uint32_t(byte & 127) << shift;
        if (!(byte & 128)) {
            require(!shift || byte, "Noncanonical variable-byte integer");
            return result;
        }
    }
    throw Error{"Variable-byte integer overflow"};
}

void encode(FILE* file, uint32_t value, bool compressed) {
    if (!compressed) {
        write32(file, value);
        return;
    }
    do {
        unsigned char byte = value & 127;
        value >>= 7;
        if (value)
            byte |= 128;
        require(fwrite(&byte, 1, 1, file) == 1, "Write error");
    } while (value);
}

double read_double(FILE* file) {
    uint64_t bits = read64(file);
    double value = 0;
    static_assert(sizeof(value) == sizeof(bits));
    memcpy(&value, &bits, sizeof(value));
    require(std::isfinite(value) && value >= 0, "Invalid vector norm");
    return value;
}

void write_double(FILE* file, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(value));
    write64(file, bits);
}

void append(FILE* output, FILE* input) {
    require(fseeko(input, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");

    char buffer[65536];
    struct stat input_info{};
    require(fstat(fileno(input), &input_info) == 0 && input_info.st_size >= 0, "Cannot stat file");
    uint64_t input_size = uint64_t(input_info.st_size);
    uint64_t remaining = input_size;

    while (remaining) {
        size_t count = remaining < sizeof(buffer) ? size_t(remaining) : sizeof(buffer);
        require(fread(buffer, 1, count, input) == count, "Truncated file or read error");
        require(fwrite(buffer, 1, count, output) == count, "Write error");
        remaining -= count;
    }
}

Index::Index(const char* path) : file_owner(fopen(path, "rb"), &fclose), file(file_owner.get()) {
    require(file != nullptr, "Cannot open file");

    struct stat file_info{};
    require(fstat(fileno(file), &file_info) == 0 && file_info.st_size >= 0, "Cannot stat file");
    uint64_t file_size = uint64_t(file_info.st_size);
    char magic[8];
    require(fread(magic, 1, 8, file) == 8, "Truncated file or read error");

    bool current_version = !memcmp(magic, "IRPOS003", 8);
    require((current_version || !memcmp(magic, "IRPOS002", 8)) &&
                read32(file) == (current_version ? 3u : 2u),
            "Unsupported positional index");

    Header& data = header;
    data.flags = read32(file);
    data.documents = read64(file);
    data.terms = read64(file);
    data.tokens = read64(file);
    data.bytes = read64(file);
    data.dictionary = read64(file);
    data.strings = read64(file);
    data.postings = read64(file);
    data.offsets = read64(file);
    data.metadata = read64(file);
    data.size = read64(file);
    data.lengths = read64(file);
    data.skips = read64(file);
    data.norms = read64(file);
    data.fingerprint = read64(file);
    data.stride = read32(file);
    data.dictionary_id = read32(file);

    require(read64(file) == header_size &&
                data.dictionary_id == ((data.flags & 4) ? lemma_dictionary_id() : 0),
            "Unsupported positional header extension");
    require(data.flags <= 7 && (data.flags & 5) != 5 && data.documents <= UINT32_MAX &&
                data.dictionary == header_size && data.dictionary <= data.strings &&
                data.strings <= data.postings && data.postings <= data.skips &&
                data.skips <= data.offsets && data.offsets <= data.metadata &&
                data.metadata <= data.lengths && data.lengths <= data.norms &&
                data.norms <= data.size && data.size == file_size &&
                (data.strings - data.dictionary) % term_size == 0 &&
                (data.strings - data.dictionary) / term_size == data.terms &&
                data.metadata - data.offsets == data.documents * 8 &&
                data.norms - data.lengths == data.documents * 4 &&
                data.size - data.norms == data.documents * 8,
            "Invalid positional index header");

    require(fseeko(file, static_cast<off_t>(data.lengths), SEEK_SET) == 0, "Cannot seek file");

    for (uint64_t i = 0; i < data.documents; ++i)
        lengths.push(read32(file));
    require(fseeko(file, static_cast<off_t>(data.norms), SEEK_SET) == 0, "Cannot seek file");

    for (uint64_t i = 0; i < data.documents; ++i)
        norms.push(read_double(file));
}

Term Index::term(uint64_t id) {
    require(id < header.terms, "Invalid term ID");
    require(fseeko(file, static_cast<off_t>(header.dictionary + id * term_size), SEEK_SET) == 0,
            "Cannot seek file");

    Term entry;
    entry.text = read64(file);
    entry.postings = read64(file);
    entry.bytes = read64(file);
    entry.skips = read64(file);
    entry.frequency = read64(file);
    entry.length = read32(file);
    entry.documents = read32(file);
    entry.blocks = read32(file);

    require(read32(file) == 0 && entry.length && entry.documents &&
                entry.documents <= header.documents && entry.frequency >= entry.documents &&
                entry.text <= header.postings - header.strings &&
                entry.length <= header.postings - header.strings - entry.text &&
                entry.postings <= header.skips - header.postings &&
                entry.bytes <= header.skips - header.postings - entry.postings &&
                entry.skips <= header.offsets - header.skips &&
                uint64_t(entry.blocks) * skip_size <= header.offsets - header.skips - entry.skips &&
                entry.blocks ==
                    (header.stride ? (uint64_t(entry.documents) + header.stride - 1) / header.stride
                                   : 0),
            "Invalid positional dictionary entry");

    return entry;
}

bool Index::find(const char* text, Term& result) {
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
            result = entry;
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
    require(offset <= header.lengths - header.metadata &&
                header.lengths - header.metadata - offset >= 8,
            "Invalid direct index offset");
    require(fseeko(file, static_cast<off_t>(header.metadata + offset), SEEK_SET) == 0,
            "Cannot seek file");

    uint32_t url_size = read32(file);
    uint32_t title_size = read32(file);

    require(url_size && uint64_t(url_size) + title_size <= header.lengths - uint64_t(ftello(file)),
            "Invalid direct index strings");

    url = read_text(file, url_size, header.lengths);
    try {
        title = read_text(file, title_size, header.lengths);
    } catch (...) {
        free(url);
        url = nullptr;
        throw;
    }
}

bool Cursor::next() {
    if (seen == term.documents) {
        valid = false;
        return false;
    }
    auto& f = index.file;
    uint64_t start = index.header.postings + term.postings, end = start + term.bytes;
    require(next_offset < term.bytes, "Truncated posting list");
    require(fseeko(f, static_cast<off_t>(start + next_offset), SEEK_SET) == 0, "Cannot seek file");
    bool compressed = index.header.flags & 2;
    uint32_t value = decode(f, compressed, end);
    uint64_t id = compressed ? uint64_t(previous) + value : value;
    require(id < index.header.documents && (!seen || id > previous), "Invalid document gap");
    document = uint32_t(id);
    uint32_t frequency = decode(f, compressed, end);
    require(frequency && frequency <= index.lengths[document], "Invalid term frequency");
    positions.size = 0;
    uint32_t last = 0;

    for (uint32_t i = 0; i < frequency; ++i) {
        value = decode(f, compressed, end);
        uint64_t position = compressed ? uint64_t(last) + value : value;
        require(position < index.lengths[document] && (!i || position > last),
                "Invalid positional gap");
        positions.push(uint32_t(position));
        last = uint32_t(position);
    }
    next_offset = uint64_t(ftello(f)) - start;
    previous = document;
    ++seen;
    valid = true;
    ++counters.records;
    counters.positions += frequency;
    require(seen != term.documents || next_offset == term.bytes, "Trailing bytes in posting list");
    return true;
}

bool Cursor::advance(uint32_t target, bool use_skips) {
    if (valid && document >= target)
        return true;
    if (use_skips && term.blocks && seen < term.documents) {
        uint32_t low = 0, high = term.blocks;

        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            require(fseeko(index.file,
                           static_cast<off_t>(index.header.skips + term.skips +
                                              uint64_t(middle) * skip_size),
                           SEEK_SET) == 0,
                    "Cannot seek file");
            uint32_t last = read32(index.file);
            ++counters.skip_reads;
            if (last < target)
                low = middle + 1;
            else
                high = middle;
        }
        if (low == term.blocks) {
            seen = term.documents;
            valid = false;
            return false;
        }
        require(
            fseeko(index.file,
                   static_cast<off_t>(index.header.skips + term.skips + uint64_t(low) * skip_size),
                   SEEK_SET) == 0,
            "Cannot seek file");
        uint32_t last = read32(index.file), prior = read32(index.file);
        uint64_t offset = read64(index.file);
        uint32_t ordinal = read32(index.file), count = read32(index.file);
        ++counters.skip_reads;
        require(last < index.header.documents && ordinal < term.documents && count &&
                    count <= term.documents - ordinal && offset < term.bytes &&
                    ordinal == uint64_t(low) * index.header.stride && (!ordinal || prior < last),
                "Invalid skip entry");
        if (ordinal > seen) {
            next_offset = offset;
            seen = ordinal;
            previous = prior;
            valid = false;
            ++counters.jumps;
        }
    }

    while ((!valid || document < target) && next()) {
    }

    return valid && document >= target;
}

void statistics(const char* path) {
    Index index(path);
    const Header& data = index.header;
    uint64_t postings = 0;

    for (uint64_t i = 0; i < data.terms; ++i)
        postings += index.term(i).documents;
    printf(
        "{\"documents\":%llu,\"terms\":%llu,\"positions\":%llu,\"postings\":%llu,\"index_bytes\":%"
        "llu,\"coordinate_bytes\":%llu,\"skip_bytes\":%llu,\"average_positions_per_term\":%.9f,"
        "\"average_positions_per_posting\":%.9f,\"compressed\":%s,\"skip_stride\":%u}\n",
        (unsigned long long)data.documents, (unsigned long long)data.terms,
        (unsigned long long)data.tokens, (unsigned long long)postings,
        (unsigned long long)data.size, (unsigned long long)(data.skips - data.postings),
        (unsigned long long)(data.offsets - data.skips),
        data.terms ? double(data.tokens) / data.terms : 0,
        postings ? double(data.tokens) / postings : 0, data.flags & 2 ? "true" : "false",
        data.stride);
}

} // namespace ir::pos
