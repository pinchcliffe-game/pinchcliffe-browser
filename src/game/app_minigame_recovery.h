#pragma once

#include "game/app_core.h"

namespace fgp {

// Shared runtime for minigames that only have recovered clip tables.
// Implemented in app_minigame_recovery.cpp.
class RecoveredMinigameScene : protected virtual AppCore {
protected:
    struct RecoveredMinigameClip {
        ExecutableGameData::AnimationDefinition definition;
        bool directlyReferenced{};
    };
    struct RecoveredMinigameActiveClip {
        ExecutableGameData::AnimationDefinition definition;
        DecodedMedia media;
        TexturePtr texture;
        double startedAt{};
        bool playing{};
    };
    struct RecoveredMinigameRuntime {
        std::vector<RecoveredMinigameClip> clips;
        std::vector<std::pair<std::uint32_t, std::size_t>> nextByHotspot;
        std::optional<RecoveredMinigameActiveClip> activeClip;
        std::uint32_t lastHotspot{};
        std::size_t interactions{};
    };

    RecoveredMinigameRuntime loadRecoveredMinigameRuntime();
    bool playRecoveredMinigameHotspot(std::uint32_t hotspot);
    void renderRecoveredMinigame(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        recoveredMinigameHotspotAt(int x, int y) const;
    // Developer control API for "recovered:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeRecoveredMinigameDeveloperCommand(std::string_view command);

    std::optional<RecoveredMinigameRuntime> recoveredMinigameRuntime;
};

} // namespace fgp
