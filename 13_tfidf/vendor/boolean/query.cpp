#include "query.hpp"

namespace ir {
namespace {

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Node* combine(Operation op, Node* left, Node* right) {
    Node* result = new Node(op);
    result->left = left;
    result->right = right;
    return result;
}

}

Query::Query(const char* text, bool fuzzy) : input(text), implicit_or(fuzzy) {
    require(strlen(text) <= max_query_bytes, "Query exceeds 4096 bytes");
    spaces();
    if (!input[position])
        return;
    Node* parsed = nullptr;

    try {
        parsed = disjunction();
        spaces();
        require(!input[position], "Unexpected query token");
        root = parsed;
    } catch (...) {
        delete parsed;
        throw;
    }
}

void Query::spaces() {
    while (space(input[position]))
        ++position;
}

bool Query::take(const char* text) {
    spaces();
    size_t length = strlen(text);
    if (strncmp(input + position, text, length))
        return false;
    position += length;
    return true;
}

bool Query::starts_operand() {
    spaces();
    char c = input[position];
    return c && c != ')' && c != '&' && c != '|';
}

Node* Query::disjunction() {
    Node* left = conjunction();

    try {
        while (take("||") || (implicit_or && starts_operand())) {
            Node* right = conjunction();
            left = combine(Operation::disjunction, left, right);
        }

        return left;
    } catch (...) {
        delete left;
        throw;
    }
}

Node* Query::conjunction() {
    Node* left = unary();

    try {
        while (take("&&") || (!implicit_or && starts_operand())) {
            Node* right = unary();
            left = combine(Operation::conjunction, left, right);
        }

        return left;
    } catch (...) {
        delete left;
        throw;
    }
}

Node* Query::unary() {
    require(++depth <= 128, "Query nesting exceeds 128");
    Node* result = nullptr;

    try {
        if (take("!")) {
            result = new Node(Operation::negation);
            result->left = unary();
        } else if (take("(")) {
            result = disjunction();
            require(take(")"), "Missing closing parenthesis");
        } else {
            spaces();
            bool ascii = take("\"");
            bool quote = ascii || take("«");
            size_t start = position;
            if (quote) {
                const char* ending = ascii ? "\"" : "»";

                while (input[position] && strncmp(input + position, ending, strlen(ending)))
                    ++position;
                require(input[position], "Unclosed phrase");
                result = new Node(Operation::phrase);
                result->text = strndup(input + start, position - start);
                require(result->text != nullptr, "Out of memory");
                position += strlen(ending);
                if (take("/")) {
                    spaces();
                    require(input[position] >= '0' && input[position] <= '9',
                            "Expected phrase distance");
                    uint64_t value = 0;

                    while (input[position] >= '0' && input[position] <= '9') {
                        value = value * 10 + unsigned(input[position++] - '0');
                        require(value <= UINT32_MAX, "Phrase distance too large");
                    }
                    result->distance = uint32_t(value);
                }

                require(*result->text, "Empty phrase");
            } else {
                while (input[position] && !space(input[position]) &&
                       !strchr("()!&|\"", input[position]) && strncmp(input + position, "«", 2) &&
                       strncmp(input + position, "»", 2))
                    ++position;
                require(position > start, "Expected operand");
                result = new Node(Operation::term);
                result->text = strndup(input + start, position - start);
                require(result->text != nullptr, "Out of memory");
            }
        }
        --depth;
        return result;
    } catch (...) {
        --depth;
        delete result;
        throw;
    }
}

List intersection(const List& a, const List& b, size_t skip, uint64_t* comparisons) {
    List result;
    size_t i = 0, j = 0;

    while (i < a.size() && j < b.size()) {
        if (comparisons)
            ++*comparisons;
        if (a[i] == b[j]) {
            result.push(a[i]);
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            if (skip > 1)
                while (skip < a.size() - i && a[i + skip] < b[j]) {
                    i += skip;
                    if (comparisons)
                        ++*comparisons;
                }
            ++i;
        } else {
            if (skip > 1)
                while (skip < b.size() - j && b[j + skip] < a[i]) {
                    j += skip;
                    if (comparisons)
                        ++*comparisons;
                }
            ++j;
        }
    }

    return result;
}

List unite(const List& a, const List& b) {
    List result;
    size_t i = 0, j = 0;

    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i] < b[j]))
            result.push(a[i++]);
        else if (i == a.size() || b[j] < a[i])
            result.push(b[j++]);
        else {
            result.push(a[i]);
            ++i;
            ++j;
        }
    }

    return result;
}

List complement(const List& a, uint64_t documents) {
    List result;
    size_t cursor = 0;

    for (uint64_t id = 0; id < documents; ++id) {
        if (cursor < a.size() && a[cursor] == id)
            ++cursor;
        else
            result.push(uint32_t(id));
    }

    return result;
}

List evaluate(const Node* node, uint64_t documents, Lookup lookup, void* context, size_t skip,
              uint64_t* comparisons) {
    if (!node)
        return List{};
    if (node->operation == Operation::term || node->operation == Operation::phrase)
        return lookup(*node, context);
    List a = evaluate(node->left, documents, lookup, context, skip, comparisons);
    if (node->operation == Operation::negation)
        return complement(a, documents);
    List b = evaluate(node->right, documents, lookup, context, skip, comparisons);
    return node->operation == Operation::conjunction ? intersection(a, b, skip, comparisons)
                                                     : unite(a, b);
}

}
