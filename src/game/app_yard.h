#pragma once

#include "game/app_core.h"

namespace fgp {

// The "yard" story scene: its state and logic.
// Implemented in app_yard.cpp.
class YardScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildYardScene(SceneBuild &build);
    // Responds to this scene's animations finishing (its AnimFinished
    // callback); see GameApp::handleSceneAnimationFinished.
    void handleYardAnimationFinished(std::string_view name,
                                     double completedAt);

    // Scene-specific responses to a click on hotspot `hotspot`. Returns
    // false when the generic hotspot handling should run instead.
    bool handleYardHotspot(std::uint32_t hotspot);

    NoRepeatState yardOpeningSelector;
    int yardHotspot99Variant{0};
    std::uint32_t yardConstructionFlags{0};
    int yardHotspot58BaseVariant{0};
    int yardHotspot58CompleteVariant{0};
    // Narration a scene wants armed on entry, held until the animation clock
    // has been reset so the delay is measured from the same zero the original's
    // scene clock 0x0040c450 uses.
    std::string pendingYardOpeningNarration;
};

} // namespace fgp
