#include "tokenizer.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

struct Case {
    const char* name;
    std::string input;
    std::vector<std::string> expected;
};

static const std::vector<Case> cases = {
    {"empty", "", {}},
    {"whitespace", " \t\n\r ", {}},
    {"punctuation", ".,;!?()[]{}<>+-=*/\\#&|", {}},
    {"prose", "Memory allocation uses malloc().", {"memory", "allocation", "uses", "malloc"}},
    {"duplicates", "Linux linux LINUX", {"linux", "linux", "linux"}},
    {"stop_words", "The kernel is in the memory.", {"the", "kernel", "is", "in", "the", "memory"}},
    {"identifier",
     "pthread_create __init__ _Atomic atomic_",
     {"pthread_create", "__init__", "_atomic", "atomic_"}},
    {"underscore_only", "_ __ ___", {}},
    {"camel_case", "MemoryBarrier getPageSize", {"memorybarrier", "getpagesize"}},
    {"namespaces", "std::vector std::vector::iterator", {"std::vector", "std::vector::iterator"}},
    {"incomplete_namespaces", "::malloc std:: :value value:", {"malloc", "std", "value", "value"}},
    {"template", "std::vector<std::pair<int, int>>", {"std::vector", "std::pair", "int", "int"}},
    {"namespace_spaces", "std :: vector", {"std", "vector"}},
    {"cpp_names", "C++ G++ clang++ c++20 C++23", {"c++", "g++", "clang++", "c++20", "c++23"}},
    {"increment", "value++ ++value i+++j", {"value", "value", "i", "j"}},
    {"cpp_suffix", "C++suffix C++::name", {"c", "suffix", "c++", "name"}},
    {"hyphens",
     "memory-mapped lock-free read—write",
     {"memory", "mapped", "lock", "free", "read", "write"}},
    {"versions", "Linux 6.12 LLVM 20.1.0", {"linux", "6", "12", "llvm", "20", "1", "0"}},
    {"numbers", "0xFF 123 -42 3.14 1e10", {"0xff", "123", "42", "3", "14", "1e10"}},
    {"preprocessor",
     "#include <sys/types.h>\n#define PAGE_SIZE 4096",
     {"include", "sys", "types", "h", "define", "page_size", "4096"}},
    {"members", "obj.member ptr->field", {"obj", "member", "ptr", "field"}},
    {"flags", "-O2 -Wall --help", {"o2", "wall", "help"}},
    {"path", "/usr/include/linux/mm.h", {"usr", "include", "linux", "mm", "h"}},
    {"url",
     "https://docs.kernel.org/process/howto.html",
     {"https", "docs", "kernel", "org", "process", "howto", "html"}},
    {"apostrophes", "can't kernel’s", {"can", "t", "kernel", "s"}},
    {"code",
     "void *ptr = malloc(PAGE_SIZE);\n    free(ptr);",
     {"void", "ptr", "malloc", "page_size", "free", "ptr"}},
    {"cyrillic", "ПАМЯТЬ и потоки ЁЖ", {"память", "и", "потоки", "ёж"}},
    {"latin_accents", "CAFÉ Straße", {"café", "straße"}},
    {"greek", "ΔΕΛΤΑ Σ", {"δελτα", "σ"}},
    {"cjk", "内存 管理", {"内存", "管理"}},
    {"unicode_digits", "١٢٣ １２３", {"١٢٣", "１２３"}},
    {"combining_marks", "É é", {"é", "é"}},
    {"leading_combining_mark", "́word", {"word"}},
    {"unicode_whitespace", "first second third", {"first", "second", "third"}},
    {"emoji", "memory🧠safe", {"memory", "safe"}},
    {"no_stemming", "thread threads threading", {"thread", "threads", "threading"}},
    {"supplementary_unicode", "𐐀 İ 𝟙", {"𐐨", "i", "𝟙"}},
};

TEST(Tokenizer, ExamplesAndUnicodeLengths) {
    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        std::vector<std::string> actual;

        tokenizer::tokenize(test.input, [&](const tokenizer::Token& token) {
            actual.push_back(token.text);
            std::size_t length = 0;

            for (unsigned char byte : token.text)
                if ((byte & 0xc0) != 0x80)
                    ++length;

            EXPECT_EQ(length, token.length);
        });

        EXPECT_EQ(actual, test.expected);
    }
}

TEST(Tokenizer, InvalidUtf8) {
    for (const std::string invalid : {
             "\xff",
             "\x80",
             "\xc0\xaf",
             "\xc2",
             "\xe2\x82",
             "\xe2(\xa1",
             "\xe0\x80\xaf",
             "\xed\xa0\x80",
             "\xf0\x80\x80\xaf",
             "\xf4\x90\x80\x80",
         }) {
        EXPECT_THROW(tokenizer::tokenize("valid text " + invalid, [](const auto&) {}),
                     std::runtime_error);
    }
}

TEST(Tokenizer, LongToken) {
    const std::string text(100000, 'a');
    std::size_t emitted = 0;

    tokenizer::tokenize(text, [&](const tokenizer::Token& token) {
        ++emitted;
        EXPECT_EQ(token.text, text);
        EXPECT_EQ(token.length, text.size());
    });

    EXPECT_EQ(emitted, 1);
}
