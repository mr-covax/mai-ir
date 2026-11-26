#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace ir {

class IndexConfig {
    std::map<std::string, std::string> values;

    static std::string trim(const std::string& text) {
        size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return "";
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

            size_t split = text.find('=');
            std::string location = "config:" + std::to_string(number) + ": ";

            if (split == std::string::npos || text.find('\0') != std::string::npos)
                throw std::runtime_error(location + "Expected key = value");

            std::string key = trim(text.substr(0, split));
            std::string value = trim(text.substr(split + 1));

            if (key.empty())
                throw std::runtime_error(location + "Missing key");

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }

            if (!values.emplace(key, value).second)
                throw std::runtime_error(location + "Duplicate setting: " + std::string(key));
        }

        if (!input.eof() || input.bad())
            throw std::runtime_error("Cannot read ./config");
    }

    const std::string& get(const std::string& key, bool allow_empty = false) const {
        auto found = values.find(key);
        if (found == values.end() || (!allow_empty && found->second.empty()))
            throw std::runtime_error("config: Missing value for " + key);
        return found->second;
    }

    uint64_t number(const std::string& key, uint64_t minimum, uint64_t maximum) const {
        const auto& text = get(key);
        uint64_t value = 0;
        for (char symbol : text) {
            if (symbol < '0' || symbol > '9')
                throw std::runtime_error("config: Invalid number for " + key);

            uint64_t digit = uint64_t(symbol - '0');
            if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
                throw std::runtime_error("config: Invalid number for " + key);

            value = value * 10 + digit;
        }

        if (value < minimum || value > maximum)
            throw std::runtime_error("config: Invalid number for " + key);
        return value;
    }
};

} // namespace ir
