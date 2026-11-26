#pragma once
#include "base.hpp"

namespace ir {

constexpr size_t max_query_bytes = 4096;

struct List {
    Array<uint32_t> values;
    List() = default;
    List(const List&) = delete;

    List(List&& other) noexcept {
        values.data = other.values.data;
        values.size = other.values.size;
        values.capacity = other.values.capacity;
        other.values.data = nullptr;
        other.values.size = other.values.capacity = 0;
    }

    List& operator=(List&& other) noexcept {
        if (this != &other) {
            free(values.data);
            values.data = other.values.data;
            values.size = other.values.size;
            values.capacity = other.values.capacity;
            other.values.data = nullptr;
            other.values.size = other.values.capacity = 0;
        }
        return *this;
    }

    size_t size() const {
        return values.size;
    }

    uint32_t operator[](size_t i) const {
        return values[i];
    }

    void push(uint32_t id) {
        values.push(id);
    }
};

enum class Operation { term, phrase, conjunction, disjunction, negation };

struct Node {
    Operation operation;
    char* text = nullptr;
    uint32_t distance = UINT32_MAX;
    Node* left = nullptr;
    Node* right = nullptr;

    explicit Node(Operation op) : operation(op) {}

    ~Node() {
        free(text);
        delete left;
        delete right;
    }
};

struct Query {
    const char* input;
    size_t position = 0;
    unsigned depth = 0;
    bool implicit_or;
    Node* root = nullptr;
    explicit Query(const char* text, bool fuzzy = false);

    ~Query() {
        delete root;
    }

    void spaces();
    bool take(const char* text);
    bool starts_operand();
    Node* disjunction();
    Node* conjunction();
    Node* unary();
};

List intersection(const List& a, const List& b, size_t skip = 0, uint64_t* comparisons = nullptr);
List unite(const List& a, const List& b);
List complement(const List& a, uint64_t documents);
using Lookup = List (*)(const Node&, void*);
List evaluate(const Node* node, uint64_t documents, Lookup lookup, void* context, size_t skip = 0,
              uint64_t* comparisons = nullptr);

}
