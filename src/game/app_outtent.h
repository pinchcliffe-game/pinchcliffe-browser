#pragma once

#include "game/app_core.h"

namespace fgp {

// The "outtent" story scene: its state and logic.
// Implemented in app_outtent.cpp.
class OuttentScene : protected virtual AppCore {
protected:
    // Add this scene's contents while loadScene builds it.
    void buildOuttentScene(SceneBuild &build);
    // AnimStarted (0x00426230) and AnimFinished (0x004262a0): the music
    // is silenced while S09AN-SS-D01 or S09AN-SS-D02 plays.
    void handleOuttentAnimationStarted(std::string_view name);
    void handleOuttentAnimationFinished(std::string_view name);
    // Show or remove one of the scene's bitmap players (paper, bike,
    // backdrop2, banana), tracing the add or remove. Space removes them
    // in skipSceneForeground.
    void setOuttentBitmap(std::string_view asset, bool visible);

    bool outtentD01aStarted{false};
};

} // namespace fgp
