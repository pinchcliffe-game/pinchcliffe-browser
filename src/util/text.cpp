#include "util/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <utility>

namespace fgp {

std::string normalizedPath(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string unescapeIniValue(std::string value) {
    value = trim(std::move(value));
    // The original's ini table (0x0040e11e, 0x0040e12f) drops a leading
    // quote and, independently, a trailing one: language.ini's
    // buildacar:STAGE is 'Nivå"' and shows as "Nivå".
    if (!value.empty() && value.front() == '"') value.erase(0, 1);
    if (!value.empty() && value.back() == '"') value.pop_back();
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            // 0x0040e160 also turns \0 into a NUL (sounddlg:COUNTRIES is a
            // NUL-separated list) and \t into a tab.
            if (next == 'n') {
                result.push_back('\n');
            } else if (next == '0') {
                result.push_back('\0');
            } else if (next == 't') {
                result.push_back('\t');
            } else {
                result.push_back(next);
            }
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string cp1252ToUtf8(std::string_view cp1252) {
    // 0x80-0x9f are the Windows-1252 additions; the rest of the high half is
    // Latin-1. Bytes Windows-1252 leaves undefined become U+FFFD.
    static constexpr std::array<std::uint32_t, 32> high{{
        0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
        0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178,
    }};
    std::string result;
    for (const char character : cp1252) {
        const auto byte = static_cast<unsigned char>(character);
        const std::uint32_t codepoint =
            byte >= 0x80 && byte <= 0x9f ? high[byte - 0x80] : byte;
        if (codepoint < 0x80) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    return result;
}

std::optional<std::string> utf8ToCp1252(std::string_view utf8) {
    std::string result;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto first = static_cast<unsigned char>(utf8[i]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xe0U) == 0xc0U && i + 1 < utf8.size()) {
            codepoint = ((first & 0x1fU) << 6U) |
                        (static_cast<unsigned char>(utf8[i + 1]) & 0x3fU);
            length = 2;
        } else if ((first & 0xf0U) == 0xe0U && i + 2 < utf8.size()) {
            codepoint = ((first & 0x0fU) << 12U) |
                        ((static_cast<unsigned char>(utf8[i + 1]) & 0x3fU) << 6U) |
                        (static_cast<unsigned char>(utf8[i + 2]) & 0x3fU);
            length = 3;
        } else {
            return std::nullopt;
        }

        if (codepoint <= 0x7f || (codepoint >= 0xa0 && codepoint <= 0xff)) {
            result.push_back(static_cast<char>(codepoint));
        } else {
            static constexpr std::array<std::pair<std::uint32_t, unsigned char>, 27>
                extras{{
                    {0x20ac, 0x80}, {0x201a, 0x82}, {0x0192, 0x83}, {0x201e, 0x84},
                    {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87}, {0x02c6, 0x88},
                    {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c},
                    {0x017d, 0x8e}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201c, 0x93},
                    {0x201d, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97},
                    {0x02dc, 0x98}, {0x2122, 0x99}, {0x0161, 0x9a}, {0x203a, 0x9b},
                    {0x0153, 0x9c}, {0x017e, 0x9e}, {0x0178, 0x9f},
                }};
            const auto found = std::find_if(extras.begin(), extras.end(),
                                            [&](const auto &entry) {
                                                return entry.first == codepoint;
                                            });
            if (found == extras.end()) {
                return std::nullopt;
            }
            result.push_back(static_cast<char>(found->second));
        }
        i += length;
    }
    return result;
}

void removeLastUtf8Character(std::string &value) {
    if (value.empty()) {
        return;
    }
    std::size_t position = value.size() - 1;
    while (position > 0 &&
           (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U) {
        --position;
    }
    value.erase(position);
}

} // namespace fgp
