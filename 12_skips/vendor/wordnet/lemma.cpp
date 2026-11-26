#include "lemma.hpp"
#include "base.hpp"

namespace ir {

extern const unsigned char wordnet_data[];
extern const size_t wordnet_size;

namespace {

uint32_t integer(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct Dictionary {
    const unsigned char* base = wordnet_data;
    const unsigned char* entries;
    const unsigned char* exceptions;
    const unsigned char* strings;
    uint32_t count, irregular, bytes, maximum, identifier;

    Dictionary() {
        require(wordnet_size >= 32 && !memcmp(base, "WNLEM001", 8),
                "Invalid embedded WordNet dictionary");
        count = integer(base + 8);
        irregular = integer(base + 12);
        bytes = integer(base + 16);
        maximum = integer(base + 20);
        identifier = integer(base + 24);
        entries = base + 32;
        exceptions = entries + uint64_t(count) * 16;
        strings = exceptions + uint64_t(irregular) * 20;
        require(strings <= (wordnet_data + wordnet_size) &&
                    uint64_t((wordnet_data + wordnet_size) - strings) == bytes && maximum < 256,
                "Invalid WordNet sections");
    }

    int compare(const char* word, size_t length, const unsigned char* entry) const {
        uint32_t offset = integer(entry), size = integer(entry + 4);
        require(offset <= bytes && size <= bytes - offset, "Invalid dictionary string");
        size_t common = length < size ? length : size;
        int order = memcmp(word, strings + offset, common);
        return order ? order : length < size ? -1 : length > size;
    }

    uint32_t flags(const char* word, size_t length) const {
        size_t low = 0, high = count;

        while (low < high) {
            size_t middle = low + (high - low) / 2;
            const unsigned char* entry = entries + middle * 16;
            int order = compare(word, length, entry);
            if (!order)
                return integer(entry + 8);
            if (order < 0)
                high = middle;
            else
                low = middle + 1;
        }

        return 0;
    }

    bool exception(char* word, size_t capacity, uint32_t part) const {
        size_t length = strlen(word), low = 0, high = irregular;

        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (compare(word, length, exceptions + middle * 20) > 0)
                low = middle + 1;
            else
                high = middle;
        }

        for (size_t i = low; i < irregular; ++i) {
            const unsigned char* entry = exceptions + i * 20;
            if (compare(word, length, entry))
                break;
            if (!(integer(entry + 16) & part))
                continue;
            uint32_t offset = integer(entry + 8), count = integer(entry + 12);
            require(offset <= bytes && count <= bytes - offset && count < capacity,
                    "Lemma buffer too small");
            memcpy(word, strings + offset, count);
            word[count] = 0;
            return true;
        }

        return false;
    }
};

const Dictionary& dictionary() {
    static Dictionary data;
    return data;
}

bool vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

bool replace(char* word, size_t capacity, const char* ending, const char* suffix, uint32_t part) {
    size_t length = strlen(word), removed = strlen(ending), added = strlen(suffix);
    if (length <= removed || strcmp(word + length - removed, ending))
        return false;
    size_t size = length - removed + added;
    if (size >= 256 || size >= capacity)
        return false;
    char candidate[256];
    memcpy(candidate, word, length - removed);
    memcpy(candidate + length - removed, suffix, added);
    candidate[size] = 0;
    if (!(dictionary().flags(candidate, size) & part))
        return false;
    memcpy(word, candidate, size + 1);
    return true;
}

bool verb_suffix(char* word, size_t capacity, const char* ending) {
    size_t length = strlen(word), removed = strlen(ending);
    if (length <= removed || strcmp(word + length - removed, ending))
        return false;
    size_t prefix = length - removed;
    bool silent_e = prefix >= 3 && !vowel(word[prefix - 3]) && vowel(word[prefix - 2]) &&
                    !vowel(word[prefix - 1]) && !strchr("wxy", word[prefix - 1]);
    if (silent_e && replace(word, capacity, ending, "e", 2))
        return true;
    if (replace(word, capacity, ending, "", 2) || replace(word, capacity, ending, "e", 2))
        return true;
    if (prefix >= 2 && word[prefix - 1] == word[prefix - 2] && !vowel(word[prefix - 1])) {
        char candidate[256];
        if (prefix >= sizeof(candidate))
            return false;
        memcpy(candidate, word, prefix - 1);
        candidate[prefix - 1] = 0;
        if (dictionary().flags(candidate, prefix - 1) & 2) {
            memcpy(word, candidate, prefix);
            return true;
        }
    }

    return false;
}

}

uint32_t lemma_dictionary_id() {
    return dictionary().identifier ^ 0x4c454d31u;
}

void lemmatize(char* word, size_t capacity) {
    size_t length = strlen(word);
    if (length < 2 || length > dictionary().maximum + 5)
        return;

    for (size_t i = 0; i < length; ++i)
        if (word[i] < 'a' || word[i] > 'z')
            return;
    if (dictionary().exception(word, capacity, 2) || dictionary().exception(word, capacity, 1))
        return;
    uint32_t flags = dictionary().flags(word, length);
    if (flags & 3)
        return;
    if (replace(word, capacity, "ies", "y", 1) || replace(word, capacity, "s", "", 1) ||
        replace(word, capacity, "es", "", 1))
        return;
    if (verb_suffix(word, capacity, "ing") || verb_suffix(word, capacity, "ed"))
        return;
    if (replace(word, capacity, "ies", "y", 2) || replace(word, capacity, "s", "", 2) ||
        replace(word, capacity, "es", "", 2))
        return;
    if (flags)
        return;
    if (dictionary().exception(word, capacity, 4) || dictionary().exception(word, capacity, 8))
        return;
    if (replace(word, capacity, "iest", "y", 4) || replace(word, capacity, "ier", "y", 4))
        return;
    if (replace(word, capacity, "est", "", 4) || replace(word, capacity, "er", "", 4))
        return;
    if (replace(word, capacity, "est", "e", 4) || replace(word, capacity, "er", "e", 4))
        return;
}
}
