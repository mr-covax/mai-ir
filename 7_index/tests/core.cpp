#include "corpus.hpp"
#include "index.hpp"
#include <gtest/gtest.h>

TEST(Index, ExternalSortAndMetadata) {
    TemporaryDirectory temporary;
    auto corpus = temporary.file("corpus");
    auto small = temporary.file("small");
    auto large = temporary.file("large");
    write_corpus(corpus, {"memory allocation connections mutex mutex", "memory allocator connected",
                          "connections", ""});
    ir::build(corpus.c_str(), small.c_str(), true, 128);
    ir::build(corpus.c_str(), large.c_str(), true, 1024 * 1024);

    std::unique_ptr<FILE, int (*)(FILE*)> a_owner(fopen(small.c_str(), "rb"), &fclose);
    FILE* a = a_owner.get();
    ir::require(a != nullptr, "Cannot open file");
    std::unique_ptr<FILE, int (*)(FILE*)> b_owner(fopen(large.c_str(), "rb"), &fclose);
    FILE* b = b_owner.get();
    ir::require(b != nullptr, "Cannot open file");
    ASSERT_EQ(std::filesystem::file_size(small), std::filesystem::file_size(large));

    for (uint64_t i = 0, size = std::filesystem::file_size(small); i < size; ++i)
        ASSERT_EQ(fgetc(a), fgetc(b)) << i;

    ir::Index index(small.c_str());
    EXPECT_EQ(index.header.documents, 4);
    EXPECT_EQ(index.header.tokens, 9);
    ir::Term term{};
    ASSERT_TRUE(index.find("connect", term));
    EXPECT_EQ(term.documents, 3);
    EXPECT_EQ(term.frequency, 3);
    ASSERT_TRUE(index.find("mutex", term));
    EXPECT_EQ(term.documents, 1);
    EXPECT_EQ(term.frequency, 2);
    EXPECT_FALSE(index.find("missing", term));

    char *url = nullptr, *title = nullptr;
    index.document(3, url, title);
    EXPECT_STREQ(url, "https://example.test/3");
    EXPECT_STREQ(title, "Unicode заголовок");
    free(url);
    free(title);

    EXPECT_THROW(ir::build(corpus.c_str(), small.c_str(), false, 128), ir::Error);
}

TEST(Index, RejectsTruncatedAndInvalidText) {
    TemporaryDirectory temporary;
    auto path = temporary.file("text");
    {
        std::unique_ptr<FILE, int (*)(FILE*)> output_owner(fopen(path.c_str(), "wb"), &fclose);
        FILE* output = output_owner.get();
        ir::require(output != nullptr, "Cannot open file");
        const char bytes[] = {'a', 0, 'b'};
        ir::require(fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes), "I/O error");
    }
    {
        std::unique_ptr<FILE, int (*)(FILE*)> input_owner(fopen(path.c_str(), "rb"), &fclose);
        FILE* input = input_owner.get();
        ir::require(input != nullptr, "Cannot open file");
        EXPECT_THROW(ir::read_text(input, 3, 3), ir::Error);
        ir::require(fseeko(input, static_cast<off_t>(4), SEEK_SET) == 0, "Cannot seek file");
        EXPECT_THROW(ir::read_text(input, 1, 3), ir::Error);
    }
    {
        std::unique_ptr<FILE, int (*)(FILE*)> input_owner(fopen(path.c_str(), "rb"), &fclose);
        FILE* input = input_owner.get();
        ir::require(input != nullptr, "Cannot open file");
        ASSERT_EQ(std::filesystem::file_size(path), 3);
        std::filesystem::resize_file(path, 1);
        EXPECT_THROW(ir::read_text(input, 3, 3), ir::Error);
    }
}
