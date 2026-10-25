#include "assets/fact_table.h"

#include <algorithm>
#include <cstdlib>

#include "util/text.h"

namespace fgp {
namespace {

constexpr std::size_t kMaxParagraphs = 64;

// The executable stops collecting paragraphs at the first missing or
// "<EMPTY>" entry rather than reading to the end of the section.
bool isTerminator(const std::string &value) {
    return value.empty() || value == "<EMPTY>";
}

} // namespace

FactTable::FactTable(std::span<const std::uint8_t> bytes) {
    const std::string text(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
    std::size_t position = 0;
    Entry current;
    bool haveSection = false;
    std::vector<std::pair<std::size_t, std::string>> paragraphs;

    const auto flush = [&] {
        if (!haveSection) {
            return;
        }
        std::sort(paragraphs.begin(), paragraphs.end(),
                  [](const auto &left, const auto &right) {
                      return left.first < right.first;
                  });
        for (std::size_t index = 0; index < paragraphs.size(); ++index) {
            if (paragraphs[index].first != index ||
                isTerminator(paragraphs[index].second)) {
                break;
            }
            current.paragraphs.push_back(paragraphs[index].second);
        }
        current.ordinal = entries_.size();
        entries_.push_back(std::move(current));
        current = Entry{};
        paragraphs.clear();
    };

    while (position <= text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::string raw =
            trim(text.substr(position, end == std::string::npos
                                           ? std::string::npos
                                           : end - position));
        if (!raw.empty() && raw.front() == '[' && raw.back() == ']') {
            flush();
            current.title = trim(raw.substr(1, raw.size() - 2));
            haveSection = true;
        } else if (haveSection && !raw.empty() && raw.front() != ';') {
            const std::size_t separator = raw.find('=');
            if (separator != std::string::npos) {
                const std::string key = normalizedPath(trim(raw.substr(0, separator)));
                const std::string value =
                    unescapeIniValue(trim(raw.substr(separator + 1)));
                if (key == "hotspot") {
                    current.hotspot = static_cast<std::uint32_t>(
                        std::strtoul(value.c_str(), nullptr, 10));
                } else if (key.rfind("text", 0) == 0) {
                    const std::string digits = key.substr(4);
                    if (!digits.empty() &&
                        digits.find_first_not_of("0123456789") ==
                            std::string::npos) {
                        const auto index = static_cast<std::size_t>(
                            std::strtoul(digits.c_str(), nullptr, 10));
                        if (index < kMaxParagraphs) {
                            paragraphs.emplace_back(index, value);
                        }
                    }
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    flush();
}

const FactTable::Entry *FactTable::findByHotspot(std::uint32_t hotspot) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [hotspot](const Entry &entry) {
                                        return entry.hotspot == hotspot;
                                    });
    return found == entries_.end() ? nullptr : &*found;
}

std::string FactTable::body(const Entry &entry) {
    // 0x0041dde0 joins the paragraphs with two newlines and trims the
    // trailing separator.
    std::string result;
    for (const auto &paragraph : entry.paragraphs) {
        if (!result.empty()) {
            result += "\n\n";
        }
        result += paragraph;
    }
    return result;
}

std::uint32_t FactTable::completeMask() const {
    if (entries_.empty() || entries_.size() >= 32) {
        return 0;
    }
    return (1U << entries_.size()) - 1U;
}

} // namespace fgp
