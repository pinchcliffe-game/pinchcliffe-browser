#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "game/story_descriptors.h"

namespace fgp {

// The "goodbye" story scene: its state and logic.
// Implemented in app_goodbye.cpp, a port of FGP.exe 0x00415f50-0x004162a1.
class GoodbyeScene : protected virtual StoryDescriptors {
protected:
    // One EVENT_AddTimer slot (0x0040c480): due time, period (0 is one
    // shot) and the id the 0x111 handler receives.
    struct GoodbyeTimer {
        double due{};
        double period{};
        int id{};
    };
    struct GoodbyeState {
        StoryStage stage;
        std::array<StoryAnim *, 7> fireworks{}; // 0x004aa790
        std::array<StoryAnim *, 9> stars{};     // 0x004aa2d0
        StoryAnim *narration1{};                // 0x004a9b98
        StoryAnim *narration2{};                // 0x004a9c40
        std::vector<GoodbyeTimer> timers;
        bool initialized{};
        int fireworksShown{}; // 0x0055d000
        // Navigation-overlay time, which holds the scene clock.
        double suspendedAt{-1.0};
        double suspended{};
        // Developer pin for rand() % 7 in 0x004161d0.
        std::optional<int> pinnedRandom;
    };

    // Replaces the generic story build: Goodbye's handler drives its own
    // descriptors and timers.
    void loadGoodbyeScene();
    void updateGoodbye(double elapsed);
    void renderGoodbye(double elapsed);
    // 0x00416260 (event 0x10c). Returns true when the scene consumed the
    // key. Clicks (0x00416080: hotspot 1 enters Text Invader) take the
    // generated kSceneNavigations path.
    bool handleGoodbyeKey(SDL_Keycode key);
    bool executeGoodbyeDeveloperCommand(std::string_view command);

    std::optional<GoodbyeState> goodbyeState;
    // goodbye:random=N, kept across scenes so a capture can pin the draw
    // before Goodbye's entry makes its first one.
    std::optional<int> goodbyePinnedRandom;
    // goodbye:random=cycle: draw N (from 1, over the process) returns
    // (N - 1) % bound, as the capture adapter's breakpoint counter does.
    bool goodbyeCycleRandom{};
    int goodbyeDraws{};

private:
    double goodbyeClock(double elapsed);
    void initializeGoodbye(double now);
    int goodbyeRandom(int bound);
    int pickGoodbyeAnim(const StoryAnim *const *table, int count);
    void showGoodbyeFirework(double now);
    void showGoodbyeStar(double now);
    void finishGoodbyeAnim(StoryAnim &anim, double now);
};

} // namespace fgp
