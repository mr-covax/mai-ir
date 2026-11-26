#include "corpus.hpp"
#include "index.hpp"

#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

TEST(Codec, BoundariesAndInvalidEncoding) {
    TemporaryDirectory temporary;
    auto path = temporary.file("codec");
    std::vector<uint32_t> values = {0,       1,       127,       128,       16383,     16384,
                                    2097151, 2097152, 268435455, 268435456, UINT32_MAX};
    uint32_t state = 0x92a91873;

    for (unsigned i = 0; i < 1000; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        values.push_back(state);
    }

    for (bool compressed : {false, true}) {
        FILE* file = fopen(path.c_str(), "w+b");
        ir::require(file != nullptr, "Cannot open file");

        for (uint32_t value : values)
            ir::pos::encode(file, value, compressed);

        uint64_t end = uint64_t(ftello(file));
        ir::require(fseeko(file, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");

        for (uint32_t value : values)
            ASSERT_EQ(ir::pos::decode(file, compressed, end), value);

        EXPECT_EQ(uint64_t(ftello(file)), end);
        EXPECT_EQ(fclose(file), 0);
    }

    FILE* file = fopen(path.c_str(), "w+b");
    ir::require(file != nullptr, "Cannot open file");

    unsigned char invalid[] = {128, 0};
    ir::require(fwrite(invalid, 1, sizeof(invalid), file) == sizeof(invalid), "I/O error");
    ir::require(fseeko(file, static_cast<off_t>(0), SEEK_SET) == 0, "Cannot seek file");
    EXPECT_THROW(ir::pos::decode(file, true, sizeof(invalid)), ir::Error);
    EXPECT_EQ(fclose(file), 0);
}

class PositionalIndex : public testing::Test {
protected:
    TemporaryDirectory temporary;
    std::array<std::string, 3> paths;

    void SetUp() override {
        auto corpus = temporary.file("corpus");
        write_corpus(corpus, {"alpha beta beta gamma", "alpha x beta gamma", "beta alpha gamma",
                              "alpha beta gamma", "", "delta gamma", "alpha alpha beta",
                              "common common delta"});

        paths = {temporary.file("raw"), temporary.file("vb"), temporary.file("blocked")};
        ir::pos::build(corpus.c_str(), paths[0].c_str(), 0, false, 1024 * 1024, 0);
        ir::pos::build(corpus.c_str(), paths[1].c_str(), 0, true, 1024 * 1024, 1);
        ir::pos::build(corpus.c_str(), paths[2].c_str(), 0, true, 128, 1);
    }
};

TEST_F(PositionalIndex, BooleanAndOrderedPhrases) {
    const char* queries[] = {"\"alpha beta\"",
                             "\"alpha beta\" / 2",
                             "«alpha alpha»",
                             "!alpha",
                             "(alpha || delta) && !x",
                             "alpha || beta && delta",
                             "alpha && !beta",
                             "alpha && gamma",
                             "\"beta gamma\"",
                             "\"beta alpha\" / 1",
                             "\"unclosed",
                             "\"alpha beta\" / -1"};
    const std::vector<uint32_t> expected[] = {
        {0, 3, 6},       {0, 1, 3, 6}, {6},          {4, 5, 7}, {0, 2, 3, 5, 6, 7},
        {0, 1, 2, 3, 6}, {},           {0, 1, 2, 3}, {0, 1, 3}, {2}};

    for (const auto& path : paths) {
        ir::pos::Index index(path.c_str());

        for (bool skips : {false, true}) {
            ir::pos::Options options;
            options.use_skips = skips;

            for (size_t i = 0; i < std::size(expected); ++i) {
                SCOPED_TRACE(queries[i]);
                ir::Query query(queries[i]);
                ir::pos::Search search(index, options);
                auto result = search.run(query.root);
                std::vector<uint32_t> actual;

                for (size_t j = 0; j < result.size(); ++j)
                    actual.push_back(result[j]);

                EXPECT_EQ(actual, expected[i]);
            }
        }
    }

    for (size_t i = std::size(expected); i < std::size(queries); ++i) {
        SCOPED_TRACE(queries[i]);
        EXPECT_THROW(ir::Query invalid(queries[i]), ir::Error);
    }
}

TEST_F(PositionalIndex, MergeMetadataNormsAndSkips) {
    FILE* a = fopen(paths[1].c_str(), "rb");
    ir::require(a != nullptr, "Cannot open file");

    FILE* b = fopen(paths[2].c_str(), "rb");
    ir::require(b != nullptr, "Cannot open file");
    ASSERT_EQ(std::filesystem::file_size(paths[1]), std::filesystem::file_size(paths[2]));

    for (uint64_t i = 0, size = std::filesystem::file_size(paths[1]); i < size; ++i)
        ASSERT_EQ(fgetc(a), fgetc(b)) << i;
    EXPECT_EQ(fclose(a), 0);
    EXPECT_EQ(fclose(b), 0);

    for (const auto& path : paths) {
        ir::pos::Index index(path.c_str());
        EXPECT_EQ(index.header.documents, 8);
        EXPECT_EQ(index.header.tokens, 22);

        double idf = log(9.0 / 6) + 1;
        double beta = 1 + log(2.0);
        EXPECT_NEAR(index.norms[0], idf * sqrt(2 + beta * beta), 1e-12);
        EXPECT_EQ(index.norms[4], 0);
        EXPECT_EQ(index.lengths[4], 0);

        char* url = nullptr;
        char* title = nullptr;

        index.document(7, url, title);
        EXPECT_STREQ(url, "https://example.test/7");
        EXPECT_STREQ(title, "Unicode заголовок");
        free(url);
        free(title);

        ir::pos::Term term;
        ASSERT_TRUE(index.find("delta", term));
        ir::pos::Counters counts;
        ir::pos::Cursor cursor(index, term, counts);
        ASSERT_TRUE(cursor.advance(7, true));
        EXPECT_EQ(cursor.document, 7);
        EXPECT_FALSE(cursor.advance(8, true));
    }
}

TEST_F(PositionalIndex, FuzzySearchAndTfIdfRanking) {
    for (const auto& path : paths) {
        ir::pos::Index index(path.c_str());
        ir::pos::Options options;
        options.ranked = options.fuzzy = true;

        ir::Array<ir::pos::Hit> hits;
        ir::pos::Counters counts;
        uint64_t comparisons = 0;

        ir::pos::compute(index, "alpha delta", options, hits, counts, comparisons);
        EXPECT_EQ(hits.size, 7);

        ir::Array<ir::pos::Hit> strict;
        ir::pos::compute(index, "alpha && delta", options, strict, counts, comparisons);
        EXPECT_EQ(strict.size, 0);

        for (bool skips : {false, true}) {
            options.use_skips = skips;

            ir::Array<ir::pos::Hit> ranked;
            ir::pos::compute(index, "alpha beta", options, ranked, counts, comparisons);
            ASSERT_EQ(ranked.size, 5);

            const uint32_t order[] = {6, 0, 2, 3, 1};

            for (size_t i = 0; i < ranked.size; ++i)
                EXPECT_EQ(ranked[i].document, order[i]);

            double tf = 1 + log(2.0);
            EXPECT_NEAR(ranked[0].score, (tf + 1) / (sqrt(tf * tf + 1) * sqrt(2.0)), 1e-12);
            EXPECT_DOUBLE_EQ(ranked[2].score, ranked[3].score);
        }
    }
}
