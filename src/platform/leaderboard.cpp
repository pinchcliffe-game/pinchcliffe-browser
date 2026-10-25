#include "platform/leaderboard.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>

#include "util/text.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Fire and forget: a lost score must never disturb the game.
// clang-format off
EM_JS(void, fgp_post_score, (const char *scene, const char *title,
                             const char *player, int score), {
    try {
        fetch('api/scores', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            keepalive: true,
            body: JSON.stringify({
                scene: UTF8ToString(scene), title: UTF8ToString(title),
                player: UTF8ToString(player), score: score,
            }),
        }).then(() => { if (Module.fgpRefreshLeaderboard) Module.fgpRefreshLeaderboard(); })
          .catch(() => {});
    } catch (error) {}
});
// One line per entry: player, score and date separated by tabs.
EM_JS(char *, fgp_leaderboard_rows, (const char *scene), {
    const text = Module.fgpLeaderboardRows ? Module.fgpLeaderboardRows(UTF8ToString(scene)) : null;
    return text === null ? 0 : stringToNewUTF8(text);
});
// clang-format on
#endif

namespace fgp {

void submitLeaderboardScore(std::string_view scene, std::string_view title,
                            std::string_view player, int score) {
#ifdef __EMSCRIPTEN__
    const std::string sceneText(scene);
    const std::string titleText = cp1252ToUtf8(title);
    const std::string playerText = cp1252ToUtf8(player);
    fgp_post_score(sceneText.c_str(), titleText.c_str(), playerText.c_str(), score);
#else
    (void)scene;
    (void)title;
    (void)player;
    (void)score;
#endif
}

std::optional<std::vector<LeaderboardEntry>> leaderboardEntries(std::string_view scene) {
#ifdef __EMSCRIPTEN__
    const std::string sceneText(scene);
    char *raw = fgp_leaderboard_rows(sceneText.c_str());
    std::vector<LeaderboardEntry> entries;
    if (raw == nullptr) {
        return entries;
    }
    const std::string text(raw);
    std::free(raw);
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second == std::string::npos) {
            continue;
        }
        try {
            entries.push_back({line.substr(0, first),
                               static_cast<std::uint32_t>(
                                   std::stoul(line.substr(first + 1, second - first - 1))),
                               line.substr(second + 1)});
        } catch (const std::exception &) {
        }
    }
    return entries;
#else
    (void)scene;
    return std::nullopt;
#endif
}

} // namespace fgp
