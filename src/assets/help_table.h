#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fgp {

struct HelpPage {
    std::string title;
    std::vector<std::string> paragraphs;
};

std::vector<HelpPage> parseHelpPages(std::span<const std::uint8_t> bytes);

} // namespace fgp
