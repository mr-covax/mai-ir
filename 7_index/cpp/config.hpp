#pragma once

#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ir {

class IndexConfig {
    std::map<std::string, std::string, std::less<>> values;

    static std::string_view trim(std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r");
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    }

public:
    IndexConfig() {
        std::ifstream input("config");
        if (!input)
            throw std::runtime_error("Cannot open ./config");

        std::string line;
        size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            auto text = trim(line);
            if (text.empty() || text.front() == '#')
                continue;
            const auto split = text.find('=');
            const auto location = "config:" + std::to_string(number) + ": ";
            if (split == std::string_view::npos || text.find('\0') != std::string_view::npos)
                throw std::runtime_error(location + "Expected key = value");
            const auto key = trim(text.substr(0, split));
            auto value = trim(text.substr(split + 1));
            if (key.empty())
                throw std::runtime_error(location + "Missing key");
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value.remove_prefix(1);
                value.remove_suffix(1);
            }
            if (!values.emplace(key, value).second)
                throw std::runtime_error(location + "Duplicate setting: " + std::string(key));
        }
        if (!input.eof() || input.bad())
            throw std::runtime_error("Cannot read ./config");
    }

    const std::string& get(std::string_view key, bool allow_empty = false) const {
        const auto found = values.find(key);
        if (found == values.end() || (!allow_empty && found->second.empty()))
            throw std::runtime_error("config: Missing value for " + std::string(key));
        return found->second;
    }

    uint64_t number(std::string_view key, uint64_t minimum, uint64_t maximum) const {
        const auto& text = get(key);
        uint64_t value = 0;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum)
            throw std::runtime_error("config: Invalid number for " + std::string(key));
        return value;
    }
};

}
