#include "game/app_goodbye.h"
#include "assets/original_data.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// Goodbye is a port of the scene handler at FGP.exe 0x00415f50, which
// switches on event - 0x103 through the byte table at 0x0041600c and the
// jump table at 0x00415ff0: 0x103 enters (0x00416020), 0x105 clicks
// (0x00416080), 0x107 opens the navigation popup (0x004160a0), 0x10c is a
// key (0x00416260), 0x110 an animation finishing (0x004160c0) and 0x111 a
// timer (0x00416140). REVERSE_ENGINEERING.md ("Goodbye") has the account.

namespace fgp {
namespace {

// 0x004aa790: firework1..7. 0x004aa2d0: star2, star3, star4, star6..star11.
const auto kFireworks = originalArray<std::uint32_t, 7>(0x4aa790U);
const auto kStars = originalArray<std::uint32_t, 9>(0x4aa2d0U);
constexpr std::uint32_t kNarration1 = 0x004a9b98U;
constexpr std::uint32_t kNarration2 = 0x004a9c40U;

constexpr int kFireworkTimer = 0;
constexpr int kStarTimer = 1;
constexpr int kOutroTimer = 2;

} // namespace

void GoodbyeScene::loadGoodbyeScene() {
    GoodbyeState state;
    for (std::size_t index = 0; index < kFireworks.size(); ++index) {
        state.fireworks[index] = &loadStoryAnim(state.stage, kFireworks[index]);
    }
    for (std::size_t index = 0; index < kStars.size(); ++index) {
        state.stars[index] = &loadStoryAnim(state.stage, kStars[index]);
    }
    state.narration1 = &loadStoryAnim(state.stage, kNarration1, true);
    state.narration2 = &loadStoryAnim(state.stage, kNarration2, true);
    state.pinnedRandom = goodbyePinnedRandom;
    goodbyeState = std::move(state);
    std::cout << "Loaded executable-derived Goodbye: seven fireworks every "
                 "8 s from +3 s, nine stars every 2 s, NAR-001 at the second "
                 "firework, from 0x00415f50-0x004162a1\n";
}

double GoodbyeScene::goodbyeClock(double elapsed) {
    // The navigation overlay holds the scene's timers; Desk's capture shows
    // a held timer keeping its remaining time (see the narration notes).
    // UNCERTAIN: whether the original also holds running Smacker players
    // under the overlay. Native holds the whole Goodbye clock.
    auto &state = *goodbyeState;
    if (storyNavigationOpen) {
        if (state.suspendedAt < 0.0) {
            state.suspendedAt = elapsed;
        }
        return state.suspendedAt - state.suspended;
    }
    if (state.suspendedAt >= 0.0) {
        state.suspended += elapsed - state.suspendedAt;
        state.suspendedAt = -1.0;
    }
    return elapsed - state.suspended;
}

void GoodbyeScene::initializeGoodbye(double now) {
    // 0x00416020. Its argument is the event's +0x18 word; zero resets the
    // firework count at 0x0055d000. UNCERTAIN: which entries pass a
    // non-zero word. Every native entry is a fresh one and resets it.
    // 0x0041603e plays track14 (the generic scene music path); 0x00416032
    // and 0x00416037 call the shared 0x004094d0 and 0x0041dbf0.
    auto &state = *goodbyeState;
    state.initialized = true;
    state.fireworksShown = 0;
    // 0x0041604b-0x00416062: AddTimer(clock + 3000, period 8000, id 0).
    state.timers.push_back({now + 3.0, 8.0, kFireworkTimer});
    // 0x00416065-0x00416072: AddTimer(clock, period 2000, id 1).
    state.timers.push_back({now, 2.0, kStarTimer});
}

int GoodbyeScene::goodbyeRandom(int bound) {
    // 0x0040c530: rand() % bound. Reference captures pin the call site.
    if (goodbyeCycleRandom) {
        return goodbyeDraws++ % bound;
    }
    if (goodbyeState->pinnedRandom) {
        return std::clamp(*goodbyeState->pinnedRandom, 0, bound - 1);
    }
    return std::uniform_int_distribution<int>(0, bound - 1)(randomEngine);
}

int GoodbyeScene::pickGoodbyeAnim(const StoryAnim *const *table, int count) {
    // 0x004161d0 draws rand() % 7 -- seven even for the nine-star table --
    // up to 100 times for a descriptor that is not playing (+0x78,
    // 0x00413900); failing that, 0x00415680 takes the first idle one of
    // all `count`.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int index = goodbyeRandom(7);
        if (!table[index]->playing) {
            return index;
        }
    }
    for (int index = 0; index < count; ++index) {
        if (!table[index]->playing) {
            return index;
        }
    }
    return -1;
}

void GoodbyeScene::showGoodbyeFirework(double now) {
    // 0x00416170: add the firework at its own position on layer = index,
    // play it, and start NAR-001 with the second one.
    auto &state = *goodbyeState;
    const int index = pickGoodbyeAnim(state.fireworks.data(),
                                      static_cast<int>(state.fireworks.size()));
    if (index < 0) {
        return;
    }
    StoryAnim &firework = *state.fireworks[static_cast<std::size_t>(index)];
    addStoryAnim(state.stage, firework, -1, -1, index);
    playStoryAnim(state.stage, firework, now);
    if (++state.fireworksShown == 2) {
        addAndPlayStoryAnim(state.stage, *state.narration1, now);
    }
}

void GoodbyeScene::showGoodbyeStar(double now) {
    // 0x00416210: the same pick over the stars, on layer index - 100.
    auto &state = *goodbyeState;
    const int index = pickGoodbyeAnim(state.stars.data(),
                                      static_cast<int>(state.stars.size()));
    if (index < 0) {
        return;
    }
    StoryAnim &star = *state.stars[static_cast<std::size_t>(index)];
    addStoryAnim(state.stage, star, -1, -1, index - 100);
    playStoryAnim(state.stage, star, now);
}

void GoodbyeScene::finishGoodbyeAnim(StoryAnim &anim, double now) {
    // 0x004160c0: NAR-001 continues with NAR-002 only while the scene's
    // visit count (record 0x400, read through 0x00411370) is at most 1;
    // NAR-002 arms the one-shot Outro timer two seconds out. Every
    // finished descriptor is then removed (0x00416129).
    auto &state = *goodbyeState;
    if (&anim == state.narration1 && activeSceneVisitCount <= 1U) {
        addAndPlayStoryAnim(state.stage, *state.narration2, now);
    }
    if (&anim == state.narration2) {
        state.timers.push_back({now + 2.0, 0.0, kOutroTimer});
    }
    removeStoryAnim(anim, now);
}

void GoodbyeScene::updateGoodbye(double elapsed) {
    if (!goodbyeState || pendingScene) {
        return;
    }
    auto &state = *goodbyeState;
    const double now = goodbyeClock(elapsed);
    if (!state.initialized) {
        initializeGoodbye(now);
    }
    if (storyNavigationOpen) {
        return;
    }
    updateStoryDescriptors(state.stage, now, [&](StoryAnim &anim) {
        finishGoodbyeAnim(anim, now);
    });
    // The timer service at 0x0040bf60 walks the slots in order, posts 0x111
    // for each due one, re-arms it by its period or frees a one-shot.
    std::vector<int> fired;
    for (auto timer = state.timers.begin(); timer != state.timers.end();) {
        if (timer->due <= now) {
            fired.push_back(timer->id);
            if (timer->period > 0.0) {
                timer->due += timer->period;
                ++timer;
            } else {
                timer = state.timers.erase(timer);
            }
        } else {
            ++timer;
        }
    }
    for (const int id : fired) {
        // 0x00416140.
        if (id == kFireworkTimer) {
            showGoodbyeFirework(now);
        } else if (id == kStarTimer) {
            showGoodbyeStar(now);
        } else if (id == kOutroTimer) {
            // 0x0041614f: GAME_EnterScene("outro", 0).
            enterScene("outro", "outro");
            return;
        }
    }
}

void GoodbyeScene::renderGoodbye(double elapsed) {
    if (!goodbyeState) {
        return;
    }
    renderStoryDescriptors(goodbyeState->stage, goodbyeClock(elapsed));
}

bool GoodbyeScene::handleGoodbyeKey(SDL_Keycode key) {
    // 0x00416260: Space finishes whichever narration is in the scene
    // (+0x7c, 0x004138f0) through the 0x110 handler, NAR-001 first, so a
    // first-visit Space skips both and arms the Outro timer.
    if (!goodbyeState || key != SDLK_SPACE) {
        return false;
    }
    auto &state = *goodbyeState;
    const double now = goodbyeClock(sceneElapsedNow());
    if (state.narration1->active) {
        finishGoodbyeAnim(*state.narration1, now);
    }
    if (state.narration2->active) {
        finishGoodbyeAnim(*state.narration2, now);
    }
    return true;
}

bool GoodbyeScene::executeGoodbyeDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "goodbye:";
    const auto control = command.substr(prefix.size());
    if (control == "random=cycle") {
        goodbyeCycleRandom = true;
    } else if (control.rfind("random=", 0) == 0) {
        // Pin rand() % 7 in 0x004161d0, as the capture adapter does at
        // 0x004161e0. Accepted in any scene and kept for later entries.
        goodbyePinnedRandom = std::stoi(std::string(control.substr(7)));
        if (goodbyeState) {
            goodbyeState->pinnedRandom = goodbyePinnedRandom;
        }
    } else {
        throw std::runtime_error("Unknown developer Goodbye control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
