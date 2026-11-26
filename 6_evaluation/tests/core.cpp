#include "metrics.hpp"

#include <cmath>
#include <cstdlib>
#include <gtest/gtest.h>
#include <unistd.h>

TEST(Metrics, HandCalculatedScores) {
    unsigned grades[] = {3, 0, 2}, ideal[] = {3, 2};
    auto score = ir::metrics(grades, 3, ideal, 2, 3);

    EXPECT_NEAR(score.precision, 2.0 / 3, 1e-12);
    EXPECT_NEAR(score.dcg, 8.5, 1e-12);
    EXPECT_NEAR(score.ndcg, 8.5 / (7 + 3 / log2(3.0)), 1e-12);
    EXPECT_NEAR(score.err, 0.875 + 0.125 * 0.375 / 3, 1e-12);
}

TEST(Metrics, EmptyAndShortResults) {
    unsigned grades[] = {3}, ideal[] = {3, 2};

    EXPECT_EQ(ir::metrics(nullptr, 0, nullptr, 0, 5).ndcg, 0);
    EXPECT_NEAR(ir::metrics(grades, 1, ideal, 2, 5).precision, 0.2, 1e-12);
}

TEST(Metrics, RejectsNulInInput) {
    char path[] = "/tmp/ir-metrics-XXXXXX";
    int file = mkstemp(path);
    ASSERT_GE(file, 0);

    const char text[] = "q1\ttest\0suffix\n";
    EXPECT_EQ(write(file, text, sizeof(text) - 1), ssize_t(sizeof(text) - 1));
    close(file);

    EXPECT_THROW(ir::evaluate(path, "", ""), ir::Error);
    unlink(path);
}
