#pragma once

#include "game/app_core.h"

namespace fgp {

// The "desk" story scene: its state and logic.
// Implemented in app_desk.cpp.
class DeskScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildDeskScene(SceneBuild &build);

    // Scene-specific responses to a click on hotspot `hotspot`. Returns
    // false when the generic hotspot handling should run instead.
    bool handleDeskHotspot(std::uint32_t hotspot);

    NoRepeatState deskHotspot101Selector;
    NoRepeatState deskHotspot102Selector;
    NoRepeatState deskHotspot110Selector;
};

} // namespace fgp
