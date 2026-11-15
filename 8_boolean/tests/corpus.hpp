#pragma once

#include "base.hpp"
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <unistd.h>

struct TemporaryDirectory {
    std::string path;

    TemporaryDirectory() {
        char pattern[] = "/tmp/ir-test-XXXXXX";
        if (!mkdtemp(pattern))
            throw std::runtime_error("Cannot create temporary directory");
        path = pattern;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::string file(const char* name) const {
        return path + "/" + name;
    }
};

inline void write_corpus(const std::string& path, std::initializer_list<const char*> texts) {
    std::unique_ptr<FILE, int (*)(FILE*)> file_owner(fopen(path.c_str(), "wb"), &fclose);
    FILE* file = file_owner.get();
    ir::require(file != nullptr, "Cannot open file");
    ir::require(fwrite("IRDOC001", 1, 8, file) == 8, "I/O error");
    unsigned id = 0;

    for (const char* text : texts) {
        std::string url = "https://example.test/" + std::to_string(id++);
        const char* title = "Unicode заголовок";
        ir::write32(file, url.size());
        ir::write32(file, strlen(title));
        ir::write64(file, strlen(text));
        ir::require(fwrite(url.data(), 1, url.size(), file) == url.size(), "I/O error");
        ir::require(fwrite(title, 1, strlen(title), file) == strlen(title), "I/O error");
        ir::require(fwrite(text, 1, strlen(text), file) == strlen(text), "I/O error");
    }
}
