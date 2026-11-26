#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace tokenizer {

struct Token {
    std::string text;
    std::size_t length;
};

void tokenize(std::string_view input, const std::function<void(const Token&)>& emit);

}
