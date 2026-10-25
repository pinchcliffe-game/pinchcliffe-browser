#include "assets/help_table.h"

#include <algorithm>

#include "util/text.h"

namespace fgp {

std::vector<HelpPage> parseHelpPages(std::span<const std::uint8_t> bytes) {
    std::vector<HelpPage> pages;
    std::string contents(reinterpret_cast<const char *>(bytes.data()),
                         bytes.size());
    std::size_t position = 0;
    while (position <= contents.size()) {
        const auto end = contents.find_first_of("\r\n", position);
        std::string line = trim(contents.substr(
            position,
            end == std::string::npos ? std::string::npos : end - position));
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            pages.push_back({trim(line.substr(1, line.size() - 2)), {}});
        } else if (!line.empty() && line.front() != ';' && !pages.empty()) {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                const std::string key = normalizedPath(
                    trim(line.substr(0, equals)));
                if (key.rfind("text", 0) == 0) {
                    const std::string value =
                        trim(unescapeIniValue(line.substr(equals + 1)));
                    // HELP_BuildPage at 0x0041dfe0 asks for text0, text1,
                    // ... until the INI reader returns its missing-key
                    // sentinel. An explicitly empty value is therefore a
                    // real blank line, not a missing paragraph.
                    pages.back().paragraphs.push_back(value);
                }
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
    std::erase_if(pages, [](const HelpPage &page) {
        return page.paragraphs.empty();
    });
    return pages;
}

} // namespace fgp
