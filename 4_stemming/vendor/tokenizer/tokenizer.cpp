#include "tokenizer.hpp"
#include "unicode_data.hpp"

#include <algorithm>
#include <stdexcept>

namespace tokenizer {
namespace {

std::u32string decode(std::string_view input) {
    std::u32string result;

    for (std::size_t position = 0; position < input.size();) {
        const auto start = position;
        const auto first = static_cast<unsigned char>(input[position++]);
        char32_t value = first;
        unsigned count = 0;
        char32_t minimum = 0;

        if (first < 0x80) {
            count = 0;
        } else if (first >= 0xC2 && first <= 0xDF) {
            value = first & 0x1F;
            count = 1;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            value = first & 0x0F;
            count = 2;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            value = first & 0x07;
            count = 3;
            minimum = 0x10000;
        } else {
            throw std::runtime_error("Invalid UTF-8 at byte " + std::to_string(start));
        }

        if (input.size() - position < count) {
            throw std::runtime_error("Truncated UTF-8 at byte " + std::to_string(start));
        }

        for (unsigned index = 0; index < count; ++index) {
            const auto next = static_cast<unsigned char>(input[position++]);

            if ((next & 0xC0) != 0x80) {
                throw std::runtime_error("Invalid UTF-8 continuation at byte " +
                                         std::to_string(position - 1));
            }

            value = (value << 6) | (next & 0x3F);
        }

        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            throw std::runtime_error("Invalid UTF-8 code point at byte " + std::to_string(start));
        }

        result.push_back(value);
    }

    return result;
}

unsigned kind(char32_t value) {
    const auto found =
        std::lower_bound(unicode::ranges.begin(), unicode::ranges.end(), value,
                         [](const auto& range, char32_t point) { return range.last < point; });

    return found != unicode::ranges.end() && found->first <= value ? found->kind : 0;
}

char32_t lowercase(char32_t value) {
    const auto found =
        std::lower_bound(unicode::lowercase.begin(), unicode::lowercase.end(), value,
                         [](const auto& mapping, char32_t point) { return mapping.from < point; });

    return found != unicode::lowercase.end() && found->from == value ? found->to : value;
}

std::string encode(std::u32string_view input) {
    std::string result;

    for (auto value : input) {
        value = lowercase(value);

        if (value < 0x80) {
            result.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (value >> 6)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (value >> 12)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (value >> 18)));
            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
    }

    return result;
}

struct Segment {
    std::size_t end;
    bool valid;
};

Segment scan_segment(std::u32string_view text, std::size_t start) {
    auto end = start;
    bool valid = false;

    while (end < text.size()) {
        const auto category = kind(text[end]);

        if (category != 1 && text[end] != U'_' && !(category == 2 && end > start)) {
            break;
        }

        valid = valid || category == 1;
        ++end;
    }

    return {end, valid};
}

std::size_t cpp_end(std::u32string_view text, std::size_t start, std::size_t end) {
    const auto name = encode(text.substr(start, end - start));

    if ((name != "c" && name != "g" && name != "clang") || text.substr(end, 2) != U"++") {
        return end;
    }

    auto position = end + 2;

    while (position < text.size() && text[position] >= U'0' && text[position] <= U'9') {
        ++position;
    }

    if (position < text.size() && (kind(text[position]) != 0 || text[position] == U'_')) {
        return end;
    }

    return position;
}

}

void tokenize(std::string_view input, const std::function<void(const Token&)>& emit) {
    const auto text = decode(input);
    const std::u32string_view view = text;
    std::size_t position = 0;

    while (position < text.size()) {
        const auto start = position;
        const auto segment = scan_segment(view, start);

        if (!segment.valid) {
            position = std::max(start + 1, segment.end);
            continue;
        }

        auto end = segment.end;
        const auto special_end = cpp_end(view, start, end);

        if (special_end != end) {
            end = special_end;
        } else {
            while (view.substr(end, 2) == U"::") {
                const auto next = scan_segment(view, end + 2);

                if (!next.valid) {
                    break;
                }

                end = next.end;
            }
        }

        emit(Token{encode(view.substr(start, end - start)), end - start});
        position = end;
    }
}

}
