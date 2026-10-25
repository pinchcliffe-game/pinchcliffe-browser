#pragma once

#include "game/app_core.h"

namespace fgp {

// The "house" story scene: its state and logic.
// Implemented in app_house.cpp.
class HouseScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildHouseScene(SceneBuild &build);
};

} // namespace fgp
