#pragma once

#include <array>
#include <string_view>

namespace fgp {

enum class MinigameKind { Game = 3, Activity = 4 };

struct MinigameDefinition {
    std::string_view scene;
    MinigameKind kind;
};

// FGP.exe's scene registry types 3 (games) and 4 (activities). Keeping this
// catalog independent of individual game rules gives every package a native
// shell now and a stable place for later clean-room implementations.
inline constexpr std::array<MinigameDefinition, 32> kMinigames{{
    {"lettersort", MinigameKind::Game},
    {"bugzzz", MinigameKind::Game},
    {"sockdrawer", MinigameKind::Game},
    {"wheelbarrow", MinigameKind::Game},
    {"audiopairs", MinigameKind::Game},
    {"hopscotch", MinigameKind::Game},
    {"beemaze", MinigameKind::Game},
    {"buildabike", MinigameKind::Game},
    {"mountain", MinigameKind::Game},
    {"pipeline", MinigameKind::Game},
    {"butterfly", MinigameKind::Game},
    {"hustle", MinigameKind::Game},
    {"textinvader", MinigameKind::Game},
    {"balloonhunt", MinigameKind::Game},
    {"whackamole", MinigameKind::Game},
    {"gallery", MinigameKind::Activity},
    {"bouquet", MinigameKind::Activity},
    {"fence", MinigameKind::Activity},
    {"jigsaw", MinigameKind::Activity},
    {"draughts", MinigameKind::Activity},
    {"sliding", MinigameKind::Activity},
    {"chess", MinigameKind::Activity},
    {"movieplayer", MinigameKind::Activity},
    {"info", MinigameKind::Activity},
    {"activity", MinigameKind::Activity},
    {"colorfill", MinigameKind::Activity},
    {"mahjong", MinigameKind::Activity},
    {"puzzle", MinigameKind::Activity},
    {"anaglyph", MinigameKind::Activity},
    {"gametrivia", MinigameKind::Game},
    {"mathlab", MinigameKind::Game},
    {"synonym", MinigameKind::Game},
}};

inline const MinigameDefinition *minigameDefinition(std::string_view scene) {
    for (const auto &definition : kMinigames) {
        if (definition.scene == scene) {
            return &definition;
        }
    }
    return nullptr;
}

} // namespace fgp
