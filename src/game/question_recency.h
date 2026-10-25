#pragma once

// Shared recency timestamp used by the original INI-driven question games.
// Trivia derives the epoch at 0x0047E786; Synonym's equivalent is 0x004827C0.

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <limits>

namespace fgp {

inline std::uint32_t questionMinuteStamp(std::time_t unixSeconds) {
    constexpr std::int64_t kEpochMinutes = 15120000; // 1998-10-01 00:00 UTC
    const auto minutes = static_cast<std::int64_t>(unixSeconds) / 60;
    // Both executable paths subtract unsigned and would wrap before the epoch.
    // Clamping an impossible modern system clock is a deliberate defensive
    // divergence, not arithmetic recovered from FGP.exe.
    if (minutes <= kEpochMinutes) {
        return 0U;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        minutes - kEpochMinutes, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace fgp
