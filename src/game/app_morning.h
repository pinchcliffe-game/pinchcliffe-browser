#pragma once

#include <optional>
#include <string_view>

#include "game/app_core.h"

namespace fgp {

// The "morning" story scene: its state and logic.
// Implemented in app_morning.cpp.
class MorningScene : protected virtual AppCore {
protected:
    // Adds this scene's contents while loadScene builds it.
    void buildMorningScene(SceneBuild &build);
    // The AnimFinished arm of Morning's callback (0x00425ec0).
    void handleMorningAnimationFinished(std::string_view name,
                                        double completedAt);
    // Morning's two timers (0x00425fc0) and the bird group's queued
    // birdleave, run once per frame ahead of the group scheduler.
    void updateMorning(double now);

    // Timer 0, squirrel's end + 6 s: request birdleave.
    std::optional<double> morningBirdLeaveAt;
    // The clip boundary the birdleave request waits for.
    std::optional<double> morningBirdLeaveBoundary;
    // Timer 1, squirrel's end + 23.2 s: start S07AN-LUD-001.
    std::optional<double> morningLudvigAt;
};

} // namespace fgp
