#include "index.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using namespace ir;
using namespace ir::pos;

namespace {
uint32_t integer(const unsigned char*& next, const unsigned char* end) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        require(next < end, "Truncated posting");
        const unsigned byte = *next++;
        require(shift < 28 || byte <= 15, "Invalid posting");
        value |= (byte & 127) << shift;
        if (!(byte & 128))
            return value;
    }
    throw Error{"Invalid posting"};
}

void copy_range(FILE* output, const unsigned char* from, uint64_t size) {
    while (size) {
        const auto count = size_t(std::min<uint64_t>(size, 4 * 1024 * 1024));
        require(fwrite(from, 1, count, output) == count, "Write error");
        from += count;
        size -= count;
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "Usage: change_stride INPUT OUTPUT STRIDE");
        Index input(argv[1]);
        auto header = input.header;
        require((header.flags & 2) && !header.stride && header.skips == header.offsets,
                "Expected a VB index without skips");
        char* end = nullptr;
        errno = 0;
        const auto value = strtoul(argv[3], &end, 10);
        require(argv[3][0] >= '0' && argv[3][0] <= '9' && end && !*end && !errno &&
                    value > 0 && value <= UINT32_MAX,
                "Stride must be an integer in 1..UINT32_MAX");
        const auto stride = uint32_t(value);

        void* mapping = mmap(nullptr, header.size, PROT_READ, MAP_PRIVATE, fileno(input.file), 0);
        require(mapping != MAP_FAILED, "Cannot map VB index");
        const auto* data = static_cast<const unsigned char*>(mapping);
        const std::string target = std::string(argv[2]) + ".partial";
        std::unique_ptr<FILE, decltype(&fclose)> output(fopen(target.c_str(), "wbx"), &fclose);
        require(output != nullptr, "Cannot create index");
        const std::string scratch = std::string(argv[2]) + ".skips";
        std::unique_ptr<FILE, decltype(&fclose)> skips(fopen(scratch.c_str(), "w+bx"), &fclose);
        require(skips != nullptr, "Cannot create skip section");
        require(unlink(scratch.c_str()) == 0, "Cannot unlink scratch section");
        require(fseeko(output.get(), header.dictionary, SEEK_SET) == 0, "Seek error");
        uint64_t skip_bytes = 0;
        for (uint64_t id = 0; id < header.terms; ++id) {
            Term term = input.term(id);
            term.skips = skip_bytes;
            term.blocks = (term.documents - 1) / stride + 1;
            write_term(output.get(), term);
            const auto* start = data + header.postings + term.postings;
            const auto* next = start;
            const auto* end = start + term.bytes;
            uint32_t doc = 0;
            uint32_t ordinal = 0;
            while (ordinal < term.documents) {
                const uint32_t prior = doc;
                const uint64_t offset = next - start;
                const uint32_t count = std::min(stride, term.documents - ordinal);
                for (uint32_t i = 0; i < count; ++i) {
                    doc += integer(next, end);
                    const uint32_t frequency = integer(next, end);
                    for (uint32_t j = 0; j < frequency; ++j)
                        integer(next, end);
                }
                write32(skips.get(), doc);
                write32(skips.get(), prior);
                write64(skips.get(), offset);
                write32(skips.get(), ordinal);
                write32(skips.get(), count);
                ordinal += count;
                skip_bytes += skip_size;
            }
            require(next == end, "Unexpected posting length");
        }
        copy_range(output.get(), data + header.strings, header.skips - header.strings);
        append(output.get(), skips.get());
        copy_range(output.get(), data + header.offsets, header.size - header.offsets);
        header.stride = stride;
        header.offsets += skip_bytes;
        header.metadata += skip_bytes;
        header.lengths += skip_bytes;
        header.norms += skip_bytes;
        header.size += skip_bytes;
        write_header(output.get(), header);
        require(fflush(output.get()) == 0, "Write error");
        require(munmap(mapping, input.header.size) == 0, "Cannot unmap index");
        require(link(target.c_str(), argv[2]) == 0, "Cannot publish index");
        require(unlink(target.c_str()) == 0, "Cannot unlink temporary index");
    } catch (const Error& e) {
        fprintf(stderr, "%s\n", e.message);
        return 1;
    }
}
