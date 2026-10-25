#include "game/app_outro.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <stdexcept>

#include "util/text.h"

// Outro is a port of the scene handler at FGP.exe 0x004182f0, which
// switches on event - 0x103 through the byte table at 0x004183b8 and the
// jump table at 0x00418398: 0x103 enters (0x004183d0), 0x105 clicks
// (0x00418940, the 3D glasses), 0x107 opens the navigation popup
// (0x00418960), 0x10c is a key (0x00418980), 0x110 an animation finishing
// (0x004184a0), 0x111 a timer (0x004184e0) and 0x112 the per-pass tick
// (0x00418860). REVERSE_ENGINEERING.md ("Outro") has the account.

namespace fgp {
namespace {

// 0x004ace18: firework3, firework4, firework9, firework10, firework11.
const auto kFireworks = originalArray<std::uint32_t, 5>(0x4ace18U);

constexpr int kFireworkTimer = 0;
constexpr int kCreditTimer = 1;

// 0x0049c9b8: the credit lines' rectangle (70,-128)-(800,600). 0x00418716
// places each line with flags 2: rect.left, and rect.bottom less the text
// height (0x004124e0).
constexpr int kLineLeft = 70;
constexpr int kLineTop = -128;
constexpr int kLineBottom = 600;
constexpr int kLineLayer = 15;    // 0x00418715
constexpr double kLineGap = 0.75; // 0x004186b5 and 0x00418646, 750 ms
constexpr int kMaximumLine = 63;  // 0x00418682

int ftolRound(double value) {
    // 0x004188cb-0x004188e8: add or subtract 0.5 by sign, then _ftol.
    return static_cast<int>(value > 0.0 ? value + 0.5 : value - 0.5);
}

int iniNumber(const std::string *value, int fallback) {
    // 0x0040e3a0: sscanf "%d" over the string, else the default.
    int result = fallback;
    if (value) {
        std::sscanf(value->c_str(), "%d", &result);
    }
    return result;
}

} // namespace

void OutroScene::buildOutroScene(SceneBuild &build) {
    build.appendSceneDefinition(executableGameData.animationAtRva(0x000ac978U),
                                0.0);
    // As Intent's 0x0040a770 opening: native completes and removes the
    // sequence's last clip only on this path. UNCERTAIN: whether the
    // original gates pointer input while S14AN-SS-001 plays.
    build.nextSceneSequenceBlocksInput = true;
}

void OutroScene::loadOutroScene() {
    OutroState state;
    for (std::size_t index = 0; index < kFireworks.size(); ++index) {
        state.fireworks[index] = &loadStoryAnim(state.stage, kFireworks[index]);
    }
    // 0x004183d0-0x004183db: the scene's credits.ini (resolved through
    // 0x00413910, so the language layer wins).
    std::span<const std::uint8_t> bytes;
    for (Package *candidate :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (candidate) {
            if (const auto *entry = candidate->findOptional("credits.ini")) {
                bytes = candidate->data(*entry);
                break;
            }
        }
    }
    const std::string contents(reinterpret_cast<const char *>(bytes.data()),
                               bytes.size());
    bool inCredits = false;
    std::size_t position = 0;
    while (position < contents.size()) {
        const auto end = contents.find_first_of("\r\n", position);
        const std::string line = trim(contents.substr(
            position,
            end == std::string::npos ? std::string::npos : end - position));
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            inCredits = normalizedPath(trim(line.substr(1, line.size() - 2))) ==
                        "credits";
        } else if (inCredits && !line.empty() && line.front() != ';') {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                state.credits.emplace_back(
                    normalizedPath(trim(line.substr(0, equals))),
                    unescapeIniValue(line.substr(equals + 1)));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    // 0x004183e0-0x00418408: "White Amerigo BT_10_" and "_14_".
    state.fonts[0] = loadBitmapFont(
        renderer, common.data(common.find("fonts\\white amerigo bt_10_.bmp")));
    state.fonts[1] = loadBitmapFont(
        renderer, common.data(common.find("fonts\\white amerigo bt_14_.bmp")));
    state.pinnedRandom = outroPinnedRandom;
    outroState = std::move(state);
    std::cout << "Loaded executable-derived Outro: five fireworks every 6 s "
                 "from +3 s, credits from +62 s ("
              << outroState->credits.size()
              << " credits.ini values), from 0x004182f0-0x00418990\n";
}

const std::string *OutroScene::outroCredit(std::string_view key) const {
    for (const auto &[name, value] : outroState->credits) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

double OutroScene::outroClock(double elapsed) {
    // As in Goodbye, the navigation overlay holds the scene clock.
    auto &state = *outroState;
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

void OutroScene::initializeOutro(double now) {
    // 0x004183d0. The character group, S14AN-SS-001 (0x0040a770), track15
    // and the 3D glasses (0x00418475-0x0041848d) are the generic path.
    auto &state = *outroState;
    state.initialized = true;
    state.nextEntry = 0; // 0x004183ef
    // 0x00418410-0x00418422: AddTimer(clock + 3000, period 6000, id 0).
    state.timers.push_back({now + 3.0, 6.0, kFireworkTimer});
    // 0x0041842a-0x00418439: AddTimer(clock + 62000, one shot, id 1).
    state.timers.push_back({now + 62.0, 0.0, kCreditTimer});
    // 0x00418475-0x0041848d: the glasses are added unless story award 14
    // is held (0x0041b8c0). The generic collectible draws them.
    for (const auto &collectible : sceneCollectibles) {
        if (collectible.name == "3dglasses" && collectible.visible) {
            referenceTrace.emit("animation_add", {
                {"asset", "3dglasses"}, {"x", -1}, {"y", -1}, {"layer", 0},
                {"hotspot", collectible.hotspot}, {"conflict_mask", 0}});
        }
    }
}

void OutroScene::showOutroFirework(double now) {
    // 0x004187e0: 0x00418830 draws rand() % 5 up to 100 times for a
    // firework that is not playing (+0x78); failing that 0x00418800 takes
    // the first idle one. 0x00412b90 adds it at its own position and plays.
    auto &state = *outroState;
    StoryAnim *chosen = nullptr;
    for (int attempt = 0; attempt < 100 && !chosen; ++attempt) {
        const int index = outroCycleRandom ? outroDraws++ % 5
            : state.pinnedRandom
            ? std::clamp(*state.pinnedRandom, 0, 4)
            : std::uniform_int_distribution<int>(0, 4)(randomEngine);
        if (!state.fireworks[static_cast<std::size_t>(index)]->playing) {
            chosen = state.fireworks[static_cast<std::size_t>(index)];
        }
    }
    for (StoryAnim *firework : state.fireworks) {
        if (!chosen && !firework->playing) {
            chosen = firework;
        }
    }
    if (chosen) {
        addAndPlayStoryAnim(state.stage, *chosen, now);
    }
}

double OutroScene::addOutroLines(const std::string &text, int font,
                                 double now) {
    // 0x00418630: one text descriptor per '\n'-separated line (at most 63
    // characters), each starting 750 ms after the last; an empty text
    // takes 750 ms and shows nothing.
    auto &state = *outroState;
    if (text.empty()) {
        return kLineGap;
    }
    int count = 0;
    std::size_t position = 0;
    double start = now;
    while (true) {
        const auto newline = text.find('\n', position);
        std::string line = text.substr(
            position, newline == std::string::npos ? std::string::npos
                                                   : newline - position);
        if (line.size() > static_cast<std::size_t>(kMaximumLine)) {
            line.resize(static_cast<std::size_t>(kMaximumLine));
        }
        // 0x004186f0: the first slot not in the scene (0x00418740).
        const auto slot = std::find_if(
            state.lines.begin(), state.lines.end(),
            [](const OutroLine &candidate) { return !candidate.active; });
        if (slot != state.lines.end()) {
            // 0x00407a30 -> 0x004078c0: an unnamed descriptor added at the
            // placed position on layer 15; its x becomes the default x.
            const auto &bitmapFont = state.fonts[static_cast<std::size_t>(font)];
            slot->active = true;
            slot->text = std::move(line);
            slot->font = font;
            slot->x = kLineLeft;
            slot->y = kLineBottom - bitmapFont.height;
            slot->start = start;
            referenceTrace.emit("animation_add", {
                {"asset", ""}, {"x", slot->x}, {"y", slot->y},
                {"layer", kLineLayer}, {"hotspot", 0}, {"conflict_mask", 0}});
        }
        ++count;
        start += kLineGap;
        if (newline == std::string::npos) {
            break;
        }
        position = newline + 1;
    }
    return count * kLineGap;
}

void OutroScene::showOutroCredit(double now) {
    // 0x004184f9: credit entry N reads textN, fontN (default 1, clamped to
    // 0..1), imageN and pauseN, then re-arms itself pauseN * 750 ms after
    // its last line. A missing textN ends the roll.
    auto &state = *outroState;
    const int entry = state.nextEntry++;
    const std::string suffix = std::to_string(entry);
    const std::string *text = outroCredit("text" + suffix);
    if (!text) {
        return;
    }
    const int font = std::clamp(iniNumber(outroCredit("font" + suffix), 1),
                                0, 1);
    if (outroCredit("image" + suffix)) {
        // 0x00418770: if the outro's profile word (0x00411e70, key 0, four
        // bytes) is still 0, store 1 and call the award wrapper 0x0041bb20
        // with no medal and outro:AWARD_TITLE / outro:AWARD_DESC.
        // UNCERTAIN: what 0x0041bb20 grants with medal 0 (0x0041b900);
        // native does not show this award yet.
        std::cout << "Outro credit " << entry
                  << " carries an award the native port does not grant yet\n";
    }
    const int pause = iniNumber(outroCredit("pause" + suffix), 0);
    const double lines = addOutroLines(*text, font, now);
    state.timers.push_back({now + pause * kLineGap + lines, 0.0,
                            kCreditTimer});
}

void OutroScene::scrollOutroLines(double now) {
    // 0x00418870, every pass: a line rises 0.04 * 728 pixels per second
    // from y = 600 (float constants 0x0049c9cc, 0x0049c9c8; 728 is
    // 600 - -128 from 0x0049c9c4/0x0049c9bc) and is removed (0x00407840)
    // once above -128; otherwise 0x00412cc0 moves it.
    auto &state = *outroState;
    const double nowMs = std::floor(now * 1000.0);
    for (auto &line : state.lines) {
        if (!line.active) {
            continue;
        }
        const double elapsedMs = nowMs - std::floor(line.start * 1000.0);
        const float seconds =
            static_cast<float>(elapsedMs) * 0.001F; // fstp to a float
        const double offset = static_cast<double>(seconds) *
                              static_cast<double>(0.04F) * 728.0;
        const int y = kLineBottom - ftolRound(offset);
        if (y < kLineTop) {
            line.active = false;
            referenceTrace.emit("animation_remove", {
                {"asset", ""}, {"reason", "removed"}});
            referenceTrace.emit("animation_stop", {
                {"asset", ""}, {"reason", "stopped"}});
        } else {
            line.y = y;
        }
    }
}

void OutroScene::updateOutro(double elapsed) {
    if (!outroState || pendingScene) {
        return;
    }
    auto &state = *outroState;
    const double now = outroClock(elapsed);
    if (!state.initialized) {
        initializeOutro(now);
    }
    if (storyNavigationOpen) {
        return;
    }
    // 0x004184a0: a finished outro firework is removed.
    updateStoryDescriptors(state.stage, now, [&](StoryAnim &anim) {
        removeStoryAnim(anim, now);
    });
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
        if (id == kFireworkTimer) {
            showOutroFirework(now);
        } else if (id == kCreditTimer) {
            showOutroCredit(now);
        }
    }
    scrollOutroLines(now);
}

void OutroScene::renderOutro(double elapsed) {
    if (!outroState) {
        return;
    }
    renderStoryDescriptors(outroState->stage, outroClock(elapsed));
}

void OutroScene::renderOutroCredits(double /*elapsed*/) {
    if (!outroState) {
        return;
    }
    auto &state = *outroState;
    for (const auto &line : state.lines) {
        if (line.active && line.y < kLineBottom) {
            drawText(renderer, state.fonts[static_cast<std::size_t>(line.font)],
                     line.text, line.x, line.y);
        }
    }
}

bool OutroScene::handleOutroKey(SDL_Keycode key) {
    // 0x00418980: Space tail-jumps to 0x0040cc30, which enters the parent
    // named in the scene's record (0x004ace38: "menu").
    if (!outroState || key != SDLK_SPACE) {
        return false;
    }
    enterMenu();
    return true;
}

bool OutroScene::executeOutroDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "outro:";
    const auto control = command.substr(prefix.size());
    if (control == "random=cycle") {
        outroCycleRandom = true;
        return true;
    }
    if (control.rfind("random=", 0) == 0) {
        // Pin rand() % 5 in 0x00418830, as the capture adapter does at
        // 0x0041883b. Accepted in any scene and kept for later entries.
        outroPinnedRandom = std::stoi(std::string(control.substr(7)));
        if (outroState) {
            outroState->pinnedRandom = outroPinnedRandom;
        }
        return true;
    }
    if (activeScene != "outro" || !outroState) {
        return false;
    }
    if (control.rfind("wait=", 0) == 0) {
        // Hold the developer command queue until SECONDS of Outro time.
        if (outroClock(sceneElapsedNow()) <
            std::stod(std::string(control.substr(5)))) {
            return false;
        }
    } else {
        throw std::runtime_error("Unknown developer Outro control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
