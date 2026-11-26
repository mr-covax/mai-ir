#include "stem.hpp"

#include <cstddef>
#include <cstring>

namespace ir {
namespace {

bool consonant(const char* word, size_t i) {
    size_t ys = 0;

    while (i > 0 && word[i] == 'y') {
        ++ys;
        --i;
    }

    char c = word[i];
    bool value = c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u';
    return ys % 2 ? !value : value;
}

unsigned measure(const char* word, size_t length) {
    unsigned result = 0;
    bool vowel = false;

    for (size_t i = 0; i < length; ++i) {
        char letter = word[i];
        bool c = letter == 'y' ? (i == 0 || vowel)
                               : letter != 'a' && letter != 'e' && letter != 'i' && letter != 'o' &&
                                     letter != 'u';
        if (vowel && c)
            ++result;
        vowel = !c;
    }

    return result;
}

bool has_vowel(const char* word, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (word[i] == 'a' || word[i] == 'e' || word[i] == 'i' || word[i] == 'o' ||
            word[i] == 'u' || (i && word[i] == 'y'))
            return true;
    }

    return false;
}

bool cvc(const char* word, size_t length) {
    if (length < 3)
        return false;
    char last = word[length - 1];
    return consonant(word, length - 1) && !consonant(word, length - 2) &&
           consonant(word, length - 3) && last != 'w' && last != 'x' && last != 'y';
}

bool ends(const char* word, size_t length, const char* suffix) {
    size_t count = strlen(suffix);
    return length >= count && memcmp(word + length - count, suffix, count) == 0;
}

struct Rule {
    const char* suffix;
    const char* replacement;
};

void replace(char* word, size_t& length, const char* suffix, const char* replacement) {
    length -= strlen(suffix);
    strcpy(word + length, replacement);
    length += strlen(replacement);
}

void rules(char* word, size_t& length, const Rule* table, size_t count, unsigned minimum) {
    for (size_t i = 0; i < count; ++i) {
        if (!ends(word, length, table[i].suffix))
            continue;
        if (measure(word, length - strlen(table[i].suffix)) >= minimum)
            replace(word, length, table[i].suffix, table[i].replacement);
        return;
    }
}

}

void stem(char* word) {
    size_t length = strlen(word);
    if (length < 3)
        return;

    for (size_t i = 0; i < length; ++i)
        if (word[i] < 'a' || word[i] > 'z')
            return;

    if (ends(word, length, "sses"))
        replace(word, length, "sses", "ss");
    else if (ends(word, length, "ies"))
        replace(word, length, "ies", "i");
    else if (!ends(word, length, "ss") && ends(word, length, "s"))
        word[--length] = 0;

    if (ends(word, length, "eed")) {
        if (measure(word, length - 3))
            word[--length] = 0;
    } else {
        size_t removed = ends(word, length, "ed") ? 2 : ends(word, length, "ing") ? 3 : 0;
        if (removed && has_vowel(word, length - removed)) {
            length -= removed;
            word[length] = 0;
            if (ends(word, length, "at") || ends(word, length, "bl") || ends(word, length, "iz")) {
                word[length++] = 'e';
                word[length] = 0;
            } else if (length >= 2 && word[length - 1] == word[length - 2] &&
                       consonant(word, length - 1)) {
                char last = word[length - 1];
                if (last != 'l' && last != 's' && last != 'z')
                    word[--length] = 0;
            } else if (measure(word, length) == 1 && cvc(word, length)) {
                word[length++] = 'e';
                word[length] = 0;
            }
        }
    }

    if (word[length - 1] == 'y' && has_vowel(word, length - 1))
        word[length - 1] = 'i';
    const Rule second[] = {
        {"ational", "ate"}, {"tional", "tion"}, {"enci", "ence"},   {"anci", "ance"},
        {"izer", "ize"},    {"abli", "able"},   {"alli", "al"},     {"entli", "ent"},
        {"eli", "e"},       {"ousli", "ous"},   {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"},    {"alism", "al"},    {"iveness", "ive"}, {"fulness", "ful"},
        {"ousness", "ous"}, {"aliti", "al"},    {"iviti", "ive"},   {"biliti", "ble"}};
    rules(word, length, second, sizeof(second) / sizeof(*second), 1);
    const Rule third[] = {{"icate", "ic"}, {"ative", ""}, {"alize", "al"}, {"iciti", "ic"},
                          {"ical", "ic"},  {"ful", ""},   {"ness", ""}};
    rules(word, length, third, sizeof(third) / sizeof(*third), 1);
    const char* fourth[] = {"al",  "ance",  "ence", "er",  "ic",  "able", "ible",
                            "ant", "ement", "ment", "ent", "ion", "ou",   "ism",
                            "ate", "iti",   "ous",  "ive", "ize"};

    for (auto suffix : fourth) {
        if (!ends(word, length, suffix))
            continue;
        size_t prefix = length - strlen(suffix);
        if (strcmp(suffix, "ion") == 0 &&
            (!prefix || (word[prefix - 1] != 's' && word[prefix - 1] != 't')))
            break;
        if (measure(word, prefix) > 1)
            word[length = prefix] = 0;
        break;
    }
    if (ends(word, length, "e")) {
        unsigned m = measure(word, length - 1);
        if (m > 1 || (m == 1 && !cvc(word, length - 1)))
            word[--length] = 0;
    }
    if (ends(word, length, "ll") && measure(word, length) > 1)
        word[--length] = 0;
}

}
