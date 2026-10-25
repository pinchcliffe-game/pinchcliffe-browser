#pragma once

// Shared leaderboard for the browser build.
//
// When a scored game ends, the browser build posts the score to the page's
// server (tools/web/serve.py, /api/scores), which keeps the best scores per
// game for every device on the network; tools/web/leaderboard.html shows
// them. This is an addition outside the original game: its own Scoreboard
// screen still shows only the active profile's scores. Desktop builds do
// nothing here.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgp {

// `scene` is the scene name (e.g. "whackamole"); `title` and `player` are the
// game's Windows-1252 strings (localized game name, profile name).
void submitLeaderboardScore(std::string_view scene, std::string_view title,
                            std::string_view player, int score);

struct LeaderboardEntry {
    std::string player;  // UTF-8
    std::uint32_t score{};
    std::string date;    // ISO 8601, as the server stores it
};

// The shared leaderboard's best entries for `scene`, best first, from the
// copy the page keeps (refreshed every 30 s and after each submitted score).
// std::nullopt outside the browser build.
std::optional<std::vector<LeaderboardEntry>> leaderboardEntries(std::string_view scene);

} // namespace fgp
