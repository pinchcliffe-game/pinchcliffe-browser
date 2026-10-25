#pragma once

#include "game/app_core.h"

namespace fgp {

// The "tvstation" story scene: its state and logic.
// Implemented in app_tvstation.cpp.
class TvstationScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildTvstationScene(SceneBuild &build);
};

} // namespace fgp
