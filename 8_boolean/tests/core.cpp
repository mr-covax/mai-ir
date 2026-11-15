#include "corpus.hpp"
#include "search.hpp"
#include <gtest/gtest.h>
#include <vector>

TEST(BooleanSearch, OperatorsAndEmptyDocuments) {
    TemporaryDirectory temporary;
    auto corpus = temporary.file("corpus");
    auto path = temporary.file("index");
    write_corpus(corpus, {"memory allocation connections mutex mutex", "memory allocator connected",
                          "connections", ""});
    ir::build(corpus.c_str(), path.c_str(), false, 1024 * 1024);
    ir::Index index(path.c_str());
    const char* queries[] = {"memory && (allocation || allocator)",
                             "!memory",
                             "connections !mutex",
                             "!!memory",
                             "missing || mutex",
                             "memory || connections",
                             ""};
    const std::vector<uint32_t> expected[] = {{0, 1}, {2, 3}, {2}, {0, 1}, {0}, {0, 1, 2}, {}};

    for (size_t i = 0; i < std::size(queries); ++i) {
        SCOPED_TRACE(queries[i]);
        ir::Query query(queries[i]);
        ir::SearchContext context{index};
        auto result = ir::evaluate(query.root, index.header.documents, ir::lookup, &context);
        std::vector<uint32_t> actual;

        for (size_t j = 0; j < result.size(); ++j)
            actual.push_back(result[j]);

        EXPECT_EQ(actual, expected[i]);
    }
}

TEST(BooleanQuery, PrecedenceAndInvalidSyntax) {
    ir::Query query("alpha || beta && !delta");
    ASSERT_NE(query.root, nullptr);
    EXPECT_EQ(query.root->operation, ir::Operation::disjunction);
    ASSERT_NE(query.root->right, nullptr);
    EXPECT_EQ(query.root->right->operation, ir::Operation::conjunction);
    ASSERT_NE(query.root->right->right, nullptr);
    EXPECT_EQ(query.root->right->right->operation, ir::Operation::negation);

    for (const char* text : {"(", "alpha & beta", "alpha ||"}) {
        SCOPED_TRACE(text);
        EXPECT_THROW(ir::Query invalid(text), ir::Error);
    }
}
