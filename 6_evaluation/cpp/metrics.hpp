#pragma once
#include <cstddef>

namespace ir {

struct Error {
    const char* message;
};

inline void require(bool condition, const char* message) {
    if (!condition)
        throw Error{message};
}

void evaluate(const char* queries, const char* judgments, const char* runs);

struct Scores {
    double precision, dcg, ndcg, err;
};

Scores metrics(const unsigned* grades, size_t count, const unsigned* ideal, size_t ideal_count,
               size_t cutoff);

} // namespace ir
