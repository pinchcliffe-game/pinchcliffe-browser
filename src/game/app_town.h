#pragma once

#include <string_view>

#include "game/app_core.h"

namespace fgp {

// The "town" story scene: its state and logic.
// Implemented in app_town.cpp.
class TownScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildTownScene(SceneBuild &build);
    // The AnimFinished arm of Town's callback (0x00427050).
    void handleTownAnimationFinished(std::string_view name);
    // True while reel1 plays inside its frame over the modal mesh.
    [[nodiscard]] bool townReelShowing() const;
};

} // namespace fgp
