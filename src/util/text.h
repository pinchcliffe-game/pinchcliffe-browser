#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fgp {

// Path normalization used for package-entry lookups.
std::string normalizedPath(std::string value);

std::string trim(std::string value);
std::string unescapeIniValue(std::string value);

// Conversions between UTF-8 input and the game's Windows-1252 data.
std::optional<std::string> utf8ToCp1252(std::string_view utf8);
std::string cp1252ToUtf8(std::string_view cp1252);
void removeLastUtf8Character(std::string &value);

} // namespace fgp
