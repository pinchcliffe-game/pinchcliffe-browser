#pragma once

// Parser for the packaged Windows-1252 language.ini key/value tables.

#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "util/text.h"

namespace fgp {

class LanguageTable {
public:
    // Empty table; usable as a class member assigned during loading.
    LanguageTable() = default;

    explicit LanguageTable(std::span<const std::uint8_t> bytes) {
        std::string section;
        std::string contents(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        std::size_t position = 0;
        while (position <= contents.size()) {
            const auto end = contents.find_first_of("\r\n", position);
            std::string line = trim(contents.substr(
                position, end == std::string::npos ? std::string::npos : end - position));
            if (!line.empty() && line.front() == '[' && line.back() == ']') {
                section = normalizedPath(line.substr(1, line.size() - 2));
            } else if (!line.empty() && line.front() != ';') {
                const auto equals = line.find('=');
                if (equals != std::string::npos) {
                    const std::string key = normalizedPath(trim(line.substr(0, equals)));
                    values_[section + ':' + key] =
                        unescapeIniValue(line.substr(equals + 1));
                }
            }
            if (end == std::string::npos) {
                break;
            }
            position = end + 1;
            if (position < contents.size() && contents[end] == '\r' &&
                contents[position] == '\n') {
                ++position;
            }
        }
    }

    [[nodiscard]] std::string get(std::string_view section,
                                  std::string_view key,
                                  std::string fallback = {}) const {
        const std::string lookup = normalizedPath(std::string(section)) + ':' +
                                   normalizedPath(std::string(key));
        const auto found = values_.find(lookup);
        return found == values_.end() ? std::move(fallback) : found->second;
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace fgp
