#pragma once

#include <string_view>

#include "game/app_core.h"

namespace fgp {

// The "intro" story scene: its state and logic.
// Implemented in app_intro.cpp.
class IntroScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildIntroScene(SceneBuild &build);
    // The AnimFinished arm of Intro's callback (0x00416640) for 001a/001b.
    void handleIntroAnimationFinished(std::string_view name);

    DecodedMedia introNarration001;
    DecodedMedia introNarration002;
    DecodedMedia introTrack1;
};

} // namespace fgp
