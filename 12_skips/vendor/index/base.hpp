#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>

namespace ir {

struct Error {
    const char* message;
};

void require(bool condition, const char* message);

template <class T> struct Array {
    T* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;

    Array() = default;
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    ~Array() {
        free(data);
    }

    void push(T value) {
        if (size == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            require(next > capacity && next <= SIZE_MAX / sizeof(T), "Array too large");
            void* memory = realloc(data, next * sizeof(T));
            require(memory != nullptr, "Out of memory");
            data = static_cast<T*>(memory);
            capacity = next;
        }
        data[size++] = value;
    }

    T& operator[](size_t index) {
        return data[index];
    }

    const T& operator[](size_t index) const {
        return data[index];
    }
};

template <class T, class Less> void sort(T* data, size_t size, Less less) {
    auto sift = [&](size_t root, size_t end) {
        while (root < end / 2) {
            size_t child = root * 2 + 1;
            if (child + 1 < end && less(data[child], data[child + 1]))
                ++child;
            if (!less(data[root], data[child]))
                break;
            T value = data[root];
            data[root] = data[child];
            data[child] = value;
            root = child;
        }
    };
    for (size_t i = size / 2; i > 0; --i)
        sift(i - 1, size);
    for (size_t end = size; end > 1;) {
        --end;
        T value = data[0];
        data[0] = data[end];
        data[end] = value;
        sift(0, end);
    }
}

uint32_t read32(FILE* file);
uint64_t read64(FILE* file);
void write32(FILE* file, uint32_t value);
void write64(FILE* file, uint64_t value);
char* read_text(FILE* file, uint64_t length, uint64_t end);
void tokenize(const char* text, size_t length, void (*emit)(const char*, size_t, void*),
              void* context);
void stem(char* word);

}
