#include "error.hpp"
#include "tokenizer.hpp"
#include <exception>

namespace ir {

void tokenize(const char* text, size_t length, void (*emit)(const char*, size_t, void*),
              void* context) {
    try {
        tokenizer::tokenize(std::string_view(text, length), [&](const tokenizer::Token& token) {
            emit(token.text.data(), token.text.size(), context);
        });
    } catch (const std::exception&) {
        throw Error{"Tokenization failed: invalid UTF-8 or allocation error"};
    }
}

}
