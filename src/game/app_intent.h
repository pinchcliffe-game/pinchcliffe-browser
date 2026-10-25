#pragma once

#include "game/app_core.h"

namespace fgp {

// The "intent" story scene: its state and logic.
// Implemented in app_intent.cpp.
class IntentScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildIntentScene(SceneBuild &build);
    // Responds to this scene's animations finishing (its AnimFinished
    // callback); see GameApp::handleSceneAnimationFinished.
    void handleIntentAnimationFinished(std::string_view name,
                                       double completedAt);
    // Responds to this scene's clips starting (AnimStarted, 0x10f).
    void handleIntentAnimationStarted(std::string_view name,
                                      double startedAt);
};

} // namespace fgp
