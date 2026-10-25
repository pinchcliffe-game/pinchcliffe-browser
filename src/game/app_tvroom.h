#pragma once

#include "game/app_core.h"

namespace fgp {

// The "tvroom" story scene: its state and logic.
// Implemented in app_tvroom.cpp.
class TvroomScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildTvroomScene(
        SceneBuild &build,
        std::optional<DecodedMedia> &nextTvroomConcurrentClip);
    // Responds to this scene's animations finishing (its AnimFinished
    // callback); see GameApp::handleSceneAnimationFinished.
    void handleTvroomAnimationFinished(std::string_view name,
                                       double completedAt);
};

} // namespace fgp
