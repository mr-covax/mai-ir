#pragma once

#include "metrics.hpp"

#include <cstdint>
#include <cstdlib>

namespace ir {

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

} // namespace ir
