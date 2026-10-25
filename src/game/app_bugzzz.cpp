#include "game/app_bugzzz.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <numbers>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

constexpr std::uint32_t kPlayerHotspotBase = 201U;
constexpr std::uint32_t kStartHotspot = 205U;
constexpr double kTau = std::numbers::pi * 2.0;
// 0x00436C70 scales every movement constant by elapsed_ms * 0.001 * 50
// (0x0049D9F0, and 0x0065D7A8 set to 0x32 at 0x004372FA): speeds are in
// pixels per 1/50 s.
constexpr double kNominalFramesPerSecond = 50.0;
// Engine timers registered by the initializer at 0x0043747A-0x00437516:
// the animation/frame counter 0x00659A28 every 0x42 ms, the seconds counter
// 0x00661264 every 0x3E8 ms and the spit gate 0x00661260 every 0x14D ms.
constexpr double kTickSeconds = 0.066;
constexpr double kSpitTickSeconds = 0.333;
// 0x00437BA0 stores now + 0x96 ms before the board is rebuilt.
constexpr double kStartDelaySeconds = 0.150;
// 0x00433650 makes a respawned worm immune for 0x16 ticks (+0xEB0).
constexpr long long kInvulnerableTicks = 22;
// 0x00434870 caps worm speed at 11.0 (0x0049DB3C); 0x004367E0 lays chain
// joints 11 px apart and compares the squared step with 121.0 (0x0049DB5C).
constexpr double kMaximumSpeed = 11.0;
constexpr double kJointSpacing = 11.0;
// 0x004369B0 turns by 0.12 rad (0x0049DB60) per nominal frame while held.
constexpr double kTurnPerFrame = 0.12;
// Collision, eating and spit origins probe 22 px (0x0049DB50) ahead along
// the 32-direction table filled at 0x00437560.
constexpr double kProbeDistance = 22.0;
// 0x004375A2: 30 rows from 524.0 (0x0049DB64), velocity +0.3 per row.
const auto kPanelSlide = originalArray<int, 30>([](const OriginalData &source, std::size_t index) {
    double position = source.read<float>(0x0049db64), velocity = source.read<float>(0x0049c304);
    const double acceleration = source.read<float>(0x0049dad0);
    for (std::size_t frame = 0; frame < index; ++frame) { position += velocity; velocity += acceleration; }
    return static_cast<int>(position);
});

// FGP.exe 0x004d7210, indexed with a 48-byte stride by 0x00433260.
// The first eight DWORDs are concurrent pickup counts for types 1..8,
// followed by the woodpecker clock, ant quota, worm speed and ant speed.
struct RecoveredLevel {
    std::array<int, 8> counts;
    int seconds;
    int antQuota;
    float wormSpeed;
    float antSpeed;
};

const auto kLevels = originalArray<RecoveredLevel, 9>(0x004d7210U);

// Player records 0x004D3888: start x, y and heading per slot.
struct WormStart {
    double x;
    double y;
    double heading;
};
const auto kWormStarts = originalArray<WormStart, 4>([](const OriginalData &source, std::size_t index) {
    const auto row = source.read<std::array<float, 3>>(0x004d3888U + static_cast<std::uint32_t>(12 * index));
    return WormStart{row[0], row[1], row[2]};
});

// Slot names in executable order (descriptor tables at 0x004D38B8 onward).
constexpr std::array<std::string_view, 4> kSlotNames{{
    "green", "blue", "red", "yellow"}};
constexpr std::array<std::string_view, 4> kSlotFiles{{
    "Green", "Blue", "Red", "Yellow"}};
// 0x004D3DF8: the in-game leaf descriptor names keep their odd casing.
constexpr std::array<std::string_view, 4> kLeafNames{{
    "green_Leaf", "blue_leaf", "red_leaf", "yellow_leaf"}};

// Sprite-set records at 0x004D1158 (stride 0x120): frame count, frame size,
// one-shot flag (+0x108), directional flag (+0x10C), rate (+0x110) and the
// direction row shift (+0x114).
struct SpriteRecord {
    std::string_view name;
    int frames;
    int size;
    bool oneShot;
    bool directional;
    double rate;
    int rowShift;
};
SpriteRecord spriteRecord(const OriginalData &source, std::uint32_t address) {
    return {source.string(address + 8), source.read<int>(address), source.read<int>(address + 4),
        source.read<int>(address + 0x108) != 0, source.read<int>(address + 0x10c) != 0,
        source.read<float>(address + 0x110), source.read<int>(address + 0x114)};
}
SpriteRecord kHead() { return spriteRecord(OriginalData::instance(), 0x004d1158U); }
SpriteRecord kEat() { return spriteRecord(OriginalData::instance(), 0x004d1a58U); }
SpriteRecord kDie() { return spriteRecord(OriginalData::instance(), 0x004d1b78U); }
SpriteRecord kExplode() { return spriteRecord(OriginalData::instance(), 0x004d1c98U); }
SpriteRecord kSpit() { return spriteRecord(OriginalData::instance(), 0x004d2478U); }
SpriteRecord kLife() { return spriteRecord(OriginalData::instance(), 0x004d30d8U); }
const auto kBodies = originalArray<SpriteRecord, 4>([](const OriginalData &source, std::size_t index) {
    const std::array<SpriteRecord, 4> records{{
    spriteRecord(source, 0x004d1278U),
    spriteRecord(source, 0x004d3438U),
    spriteRecord(source, 0x004d31f8U),
    spriteRecord(source, 0x004d3318U),
}};
    return records[index];
});
// 0x004338F0: appear / main / stop-or-burst sets per pickup type.
struct PickupRecord {
    SpriteRecord appear;
    SpriteRecord main;
    SpriteRecord stopOrBurst;
};
const auto kPickupRecords = originalArray<PickupRecord, 10>([](const OriginalData &source, std::size_t index) {
    const std::array<PickupRecord, 10> records{{
    {},
    {spriteRecord(source, 0x004d1ed8U), spriteRecord(source, 0x004d2358U),
     spriteRecord(source, 0x004d2118U)},
    {spriteRecord(source, 0x004d1db8U), spriteRecord(source, 0x004d2238U),
     spriteRecord(source, 0x004d1ff8U)},
    {spriteRecord(source, 0x004d1938U), spriteRecord(source, 0x004d1398U),
     spriteRecord(source, 0x004d14b8U)},
    {spriteRecord(source, 0x004d16f8U), spriteRecord(source, 0x004d16f8U),
     spriteRecord(source, 0x004d2fb8U)},
    {spriteRecord(source, 0x004d26b8U), spriteRecord(source, 0x004d26b8U),
     spriteRecord(source, 0x004d2b38U)},
    {spriteRecord(source, 0x004d15d8U), spriteRecord(source, 0x004d15d8U),
     spriteRecord(source, 0x004d2e98U)},
    {spriteRecord(source, 0x004d1818U), spriteRecord(source, 0x004d1818U),
     spriteRecord(source, 0x004d2c58U)},
    {spriteRecord(source, 0x004d2598U), spriteRecord(source, 0x004d2598U),
     spriteRecord(source, 0x004d2d78U)},
    {spriteRecord(source, 0x004d30d8U), spriteRecord(source, 0x004d30d8U),
     spriteRecord(source, 0x004d1c98U)},
}};
    return records[index];
});
const auto kPopups = originalArray<SpriteRecord, 3>([](const OriginalData &source, std::size_t index) {
    const std::array<SpriteRecord, 3> records{{
    spriteRecord(source, 0x004d27d8U),
    spriteRecord(source, 0x004d28f8U),
    spriteRecord(source, 0x004d2a18U),
}};
    return records[index];
});

// 0x004365E0: points per pickup type.
constexpr std::array<int, 10> kPickupPoints{{0, 5, 10, 15, 10, 5, 0, 10, 15, 0}};

TexturePtr cornerKeyedTexture(SDL_Renderer *renderer, Package &package,
                              const std::string &asset) {
    SurfacePtr surface = loadBmp(package.data(package.find(asset)));
    const Uint32 key = pixelValue(surface.get(), 0, 0);
    if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

TexturePtr plainTexture(SDL_Renderer *renderer, Package &package,
                        const std::string &asset) {
    SurfacePtr surface = loadBmp(package.data(package.find(asset)));
    return makeTexture(renderer, surface.get());
}

double normalizedAngle(double angle) {
    angle = std::fmod(angle, kTau);
    return angle < 0.0 ? angle + kTau : angle;
}

// 0x00435A80: the 32-way direction of a heading.
int direction32(double heading) {
    return static_cast<int>(normalizedAngle(heading) / kTau * 32.0) & 31;
}

// 0x00434E20: the sprite row index, a quarter turn ahead of the heading.
int spriteDirection(double heading) {
    return static_cast<int>(
        normalizedAngle(heading + std::numbers::pi / 2.0) / kTau * 32.0) & 31;
}

SDL_FPoint probePoint(double x, double y, double heading) {
    const double angle = direction32(heading) * kTau / 32.0;
    return {static_cast<float>(x + std::cos(angle) * kProbeDistance),
            static_cast<float>(y - std::sin(angle) * kProbeDistance)};
}

bool inBox(SDL_FPoint point, double cx, double cy, int half) {
    // 0x00435560 compares truncated integers inclusively.
    const int px = static_cast<int>(point.x);
    const int py = static_cast<int>(point.y);
    const int x = static_cast<int>(cx);
    const int y = static_cast<int>(cy);
    return px >= x - half && px <= x + half &&
           py >= y - half && py <= y + half;
}

std::string bugzzzJoinPrompt(std::string prompt, int playerNumber) {
    const auto placeholder = prompt.find("%d");
    if (placeholder != std::string::npos) {
        prompt.replace(placeholder, 2, std::to_string(playerNumber));
    }
    return prompt;
}

std::string replaceFirst(std::string text, std::string_view token,
                         std::string_view value) {
    const auto at = text.find(token);
    if (at != std::string::npos) text.replace(at, token.size(), value);
    return text;
}

int panelX(std::size_t slot) { return -100 + 200 * static_cast<int>(slot); }

} // namespace

BugzzzScene::BugzzzGame BugzzzScene::loadBugzzzGame(Package &package) {
    BugzzzGame game;
    for (std::size_t index = 0; index < kLevels.size(); ++index) {
        game.levels[index].counts = kLevels[index].counts;
        game.levels[index].seconds = kLevels[index].seconds;
        game.levels[index].antQuota = kLevels[index].antQuota;
        game.levels[index].wormSpeed = kLevels[index].wormSpeed;
        game.levels[index].antSpeed = kLevels[index].antSpeed;
    }
    game.middleTextures[0] = plainTexture(renderer, package,
                                           "bitmap\\Middle01.bmp");
    game.middleTextures[1] = plainTexture(renderer, package,
                                           "bitmap\\Middle02.bmp");
    game.bottomTexture = plainTexture(renderer, package, "bitmap\\Bottom.bmp");
    game.difficultyBackdropTexture = plainTexture(
        renderer, package, "bitmap\\Dif_backdrop.bmp");
    game.helpLeafTexture = cornerKeyedTexture(
        renderer, package, "bitmap\\Help.bmp");
    game.exitLeafTexture = cornerKeyedTexture(
        renderer, package, "bitmap\\Exit.bmp");
    game.woodBorderTexture = cornerKeyedTexture(
        renderer, package, "bitmap\\Wood boarder.bmp");
    // The Bugzzz initializer loads this exact common font at FGP.exe
    // 0x0043704c. It is materially smaller and darker than the normal
    // dialog body font, and 0x00436080 draws every HUD counter with it.
    game.hudFont = loadBitmapFont(
        renderer,
        common.data(common.find("fonts\\Comic Sans MS_08_VERY_DARK.bmp")));
    // 0x00433598: level 0 collides against `Tree mask`, later boards
    // against `Tree mask02`.
    game.treeMasks[0] = loadBmp(
        package.data(package.find("bitmap\\Tree mask.bmp")));
    game.treeMasks[1] = loadBmp(
        package.data(package.find("bitmap\\Tree mask02.bmp")));
    game.hotspotMask = loadBmp(package.data(package.find("hotspots.bmp")));

    for (std::size_t slot = 0; slot < kSlotFiles.size(); ++slot) {
        const std::string prefix(kSlotFiles[slot]);
        game.playerOnTextures[slot] = cornerKeyedTexture(
            renderer, package, "bitmap\\" + prefix + "_on.bmp");
        game.playerOffTextures[slot] = cornerKeyedTexture(
            renderer, package, "bitmap\\" + prefix + "_off.bmp");
        game.playerInfoTextures[slot] = cornerKeyedTexture(
            renderer, package, "bitmap\\" + prefix + "_info.bmp");
        game.playerTextTextures[slot] = cornerKeyedTexture(
            renderer, package, "bitmap\\" + prefix + "_text.bmp");
        game.playerLeafTextures[slot] = cornerKeyedTexture(
            renderer, package, "bitmap\\" + prefix + "_leaf.bmp");
    }
    game.startTextures[0] = cornerKeyedTexture(
        renderer, package, "bitmap\\Start_off.bmp");
    game.startTextures[1] = cornerKeyedTexture(
        renderer, package, "bitmap\\Start_on.bmp");

    const auto sprite = [&](const SpriteRecord &record) {
        BugzzzSpriteSet set;
        set.texture = cornerKeyedTexture(
            renderer, package, "bitmap\\" + std::string(record.name) + ".bmp");
        set.frames = record.frames;
        set.size = record.size;
        set.oneShot = record.oneShot;
        set.directional = record.directional;
        set.rate = record.rate;
        set.rowShift = record.rowShift;
        return set;
    };
    for (std::size_t slot = 0; slot < kBodies.size(); ++slot) {
        game.bodySets[slot] = sprite(kBodies[slot]);
    }
    game.headSet = sprite(kHead());
    game.eatSet = sprite(kEat());
    game.dieSet = sprite(kDie());
    game.explodeSet = sprite(kExplode());
    game.spitSet = sprite(kSpit());
    game.lifeSet = sprite(kLife());
    for (std::size_t type = 1; type < kPickupRecords.size(); ++type) {
        game.appearSets[type] = sprite(kPickupRecords[type].appear);
        game.mainSets[type] = sprite(kPickupRecords[type].main);
        if (type <= 3U) {
            game.stopSets[type] = sprite(kPickupRecords[type].stopOrBurst);
        } else {
            game.burstSets[type] = sprite(kPickupRecords[type].stopOrBurst);
        }
    }
    for (std::size_t index = 0; index < kPopups.size(); ++index) {
        game.popupSets[index] = sprite(kPopups[index]);
    }
    // Descriptor branch01..branch30 (0x004D4D10, stride 0xA8) name
    // bitmaps branch01..branch30; branch00 is never shown.
    for (int frame = 0; frame <= 30; ++frame) {
        const std::string number = frame < 10 ? "0" + std::to_string(frame)
                                             : std::to_string(frame);
        game.branchTextures[static_cast<std::size_t>(frame)] = plainTexture(
            renderer, package, "bitmap\\branch" + number + ".bmp");
    }
    const auto smacker = [&](std::string name) {
        HopscotchMedia placed;
        placed.media = decodeActiveSceneClip(std::move(name));
        if (!placed.media.videoFrames.empty()) {
            const auto &frame = placed.media.videoFrames.front();
            placed.texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!placed.texture) throw std::runtime_error(SDL_GetError());
            SDL_SetTextureBlendMode(placed.texture.get(), SDL_BLENDMODE_BLEND);
        }
        return placed;
    };
    game.birdIn = smacker("bird_in");
    game.birdPeck = smacker("bird_hakke");
    game.birdFall = smacker("branch_fall_from_top");
    game.birdTimeout = smacker("hakkned");
    // 0x004372FA-0x00437313: only the green slot is selected on entry and
    // its panel starts as green_info; the others show their join prompt.
    game.worms[0].selected = true;
    for (std::size_t slot = 0; slot < game.panels.size(); ++slot) {
        game.panels[slot].current =
            std::string(kSlotNames[slot]) + (slot == 0 ? "_info" : "_text");
    }
    // EVENT_AddTimer (0x0040C480) timers keep their phase on the
    // application clock (0x0040C450), so the tick and seconds counters
    // turn over on whole multiples of their periods of app time.
    game.clockOrigin = 0.0;
    std::cout << "Loaded executable-derived Bugzzz: nine exact level rows, "
                 "four-player controls, tree collision masks, 32-direction "
                 "worms, pickups, growth, spit and woodpecker clock\n";
    buildBugzzzLobby(game, true);
    return game;
}

int BugzzzScene::bugzzzSecond(double now) const {
    if (!bugzzzGame) return 0;
    return static_cast<int>(std::floor(now - bugzzzGame->clockOrigin));
}

long long BugzzzScene::bugzzzTick(double now) const {
    if (!bugzzzGame) return 0;
    return static_cast<long long>(
        std::floor((now - bugzzzGame->clockOrigin) / kTickSeconds));
}

namespace {

void traceAdd(ReferenceTrace &trace, std::string_view asset, int layer,
              int x = -1, int y = -1, int hotspot = 0) {
    trace.emit("animation_add", {
        {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", 0}});
}

void traceRemove(ReferenceTrace &trace, std::string_view asset) {
    // 0x00412AA0's RemoveAnim logpoint precedes the nested StopAnim.
    trace.emit("animation_remove", {{"asset", asset}, {"reason", "removed"}});
    trace.emit("animation_stop", {{"asset", asset}, {"reason", "stopped"}});
}

} // namespace

void BugzzzScene::buildBugzzzLobby(BugzzzGame &game, bool initial) {
    // 0x00434060(1): player buttons at layer 10, the panel updater, the
    // Start text, difficulty backing (9), Start control (10), background
    // (8), corner leaves and controls, wood border (11).
    game.phase = BugzzzPhase::Lobby;
    game.pressedStart = false;
    game.level = 0;
    game.pickups.clear();
    game.spits.clear();
    game.popups.clear();
    for (auto &worm : game.worms) worm.playing = false;
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        traceAdd(referenceTrace,
                 std::string(kSlotNames[slot]) +
                     (game.worms[slot].selected ? "_on" : "_off"), 10);
    }
    if (!initial) {
        for (std::size_t slot = 0; slot < game.panels.size(); ++slot) {
            game.panels[slot].current =
                std::string(kSlotNames[slot]) +
                (game.worms[slot].selected ? "_info" : "_text");
            game.panels[slot].pending.clear();
            game.panels[slot].state = 1;
        }
    }
    for (std::size_t slot = 0; slot < game.panels.size(); ++slot) {
        traceAdd(referenceTrace, game.panels[slot].current,
                 20 - static_cast<int>(slot));
    }
    traceAdd(referenceTrace, "dif_backdrop", 9);
    traceAdd(referenceTrace, "start_off", 10);
    traceAdd(referenceTrace, "bottom", 8);
    traceAdd(referenceTrace, "Exit", 90);
    traceAdd(referenceTrace, "Help", 90);
    traceAdd(referenceTrace, "help_0", 93, -1, -1, 31);
    traceAdd(referenceTrace, "exit_0", 93, -1, -1, 32);
    traceAdd(referenceTrace, "wood boarder", 11);
}

void BugzzzScene::setBugzzzPanel(std::size_t slot, std::string descriptor) {
    if (!bugzzzGame || slot >= bugzzzGame->panels.size()) return;
    // 0x00433BD0 advances the slot state, queues the new descriptor and
    // timestamps the slide with the application clock.
    auto &panel = bugzzzGame->panels[slot];
    ++panel.state;
    panel.pending = std::move(descriptor);
    panel.changedAt = sceneElapsedNow();
}

void BugzzzScene::updateBugzzzPanels(double now) {
    if (!bugzzzGame) return;
    // 0x00433C20 walks slots from layer 20 (green) to 17 (yellow).
    for (std::size_t slot = 0; slot < bugzzzGame->panels.size(); ++slot) {
        auto &panel = bugzzzGame->panels[slot];
        const int steps = static_cast<int>((now - panel.changedAt) * 60.0);
        if (panel.state == 0) {
            if (29 - steps < 0) panel.state = 1;
        } else if (panel.state >= 2 && steps >= 30) {
            traceRemove(referenceTrace, panel.current);
            panel.state = 0;
            panel.current = panel.pending;
            panel.pending.clear();
            panel.changedAt = now;
            traceAdd(referenceTrace, panel.current,
                     20 - static_cast<int>(slot));
        }
    }
}

void BugzzzScene::toggleBugzzzPlayer(std::size_t slot) {
    if (!bugzzzGame || slot >= bugzzzGame->worms.size()) return;
    auto &game = *bugzzzGame;
    auto &worm = game.worms[slot];
    const std::string name(kSlotNames[slot]);
    if (game.phase == BugzzzPhase::Lobby) {
        // 0x00437A3C: only a resting panel accepts a toggle.
        if (game.panels[slot].state != 1 || game.pressedStart) return;
        std::array<bool, 4> before{};
        for (std::size_t other = 0; other < before.size(); ++other) {
            before[other] = game.worms[other].selected;
        }
        worm.selected = !worm.selected;
        setBugzzzPanel(slot, name + (worm.selected ? "_info" : "_text"));
        // 0x00434060(0) removes every on/off button, then re-adds one per
        // slot in slot order.
        for (std::size_t other = 0; other < kSlotNames.size(); ++other) {
            traceRemove(referenceTrace, std::string(kSlotNames[other]) +
                                            (before[other] ? "_on" : "_off"));
        }
        for (std::size_t other = 0; other < kSlotNames.size(); ++other) {
            traceAdd(referenceTrace,
                     std::string(kSlotNames[other]) +
                         (game.worms[other].selected ? "_on" : "_off"),
                     10);
        }
        return;
    }
    if (game.phase != BugzzzPhase::Playing) return;
    // 0x00437AA4: during play the first press readies an idle slot and the
    // second press drops the worm into the running board (0x00437B50).
    if (worm.playing || game.panels[slot].state != 1) return;
    if (!worm.selected) {
        worm.selected = true;
        setBugzzzPanel(slot, name + "_info");
    } else {
        worm.playing = true;
        worm.lives = 3;
        worm.score = 0;
        worm.antsEaten = 0;
        worm.frags = 0;
        resetBugzzzWorm(slot);
        setBugzzzPanel(slot, std::string(kLeafNames[slot]));
    }
}

bool BugzzzScene::handleBugzzzKey(SDL_Keycode key) {
    if (!bugzzzGame) return false;
    auto &game = *bugzzzGame;
    // 0x00437C10: keys 1..4 act like clicking the slot's hotspot.
    if (key >= SDLK_1 && key <= SDLK_4) {
        toggleBugzzzPlayer(static_cast<std::size_t>(key - SDLK_1));
        return true;
    }
    if (game.phase != BugzzzPhase::Playing) return false;
    // Steering and spitting read the held key state every frame
    // (0x004369B0 through 0x0040CA60); swallow the presses here.
    switch (key) {
    case SDLK_LEFT: case SDLK_RIGHT: case SDLK_RETURN: case SDLK_KP_ENTER:
    case SDLK_a: case SDLK_s: case SDLK_LCTRL: case SDLK_RCTRL:
    case SDLK_j: case SDLK_k: case SDLK_SPACE:
    case SDLK_KP_MULTIPLY: case SDLK_KP_MINUS: case SDLK_KP_DIVIDE:
        return true;
    default:
        return false;
    }
}

bool BugzzzScene::pressBugzzzHotspot(std::uint32_t hotspot) {
    if (!bugzzzGame) return false;
    if (hotspot >= kPlayerHotspotBase && hotspot < kPlayerHotspotBase + 4U) {
        toggleBugzzzPlayer(static_cast<std::size_t>(
            hotspot - kPlayerHotspotBase));
        return true;
    }
    if (hotspot == kStartHotspot) {
        pressBugzzzStart();
        return true;
    }
    return false;
}

void BugzzzScene::pressBugzzzStart() {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    if (game.phase != BugzzzPhase::Lobby || game.pressedStart) return;
    // 0x00437BA0 requires at least one selected slot (0x004338C0), swaps
    // start_off for start_on at layer 9 and starts 150 ms later.
    if (std::none_of(game.worms.begin(), game.worms.end(),
                     [](const auto &worm) { return worm.selected; })) {
        return;
    }
    traceRemove(referenceTrace, "start_off");
    traceAdd(referenceTrace, "start_on", 9);
    game.pressedStart = true;
    game.phase = BugzzzPhase::Starting;
    game.startAt = sceneElapsedNow() + kStartDelaySeconds;
}

void BugzzzScene::startBugzzzGame() {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    if (std::none_of(game.worms.begin(), game.worms.end(),
                     [](const auto &worm) { return worm.selected; })) {
        game.worms[0].selected = true;
    }
    // 0x004342C0 clears the lobby and rebuilds the board on the background
    // that 0x00434010 selects for level 0: `bottom`.
    if (game.phase == BugzzzPhase::Lobby ||
        game.phase == BugzzzPhase::Starting) {
        traceRemove(referenceTrace, "bottom");
        traceRemove(referenceTrace, game.pressedStart ? "start_on"
                                                      : "start_off");
        traceRemove(referenceTrace, "dif_backdrop");
        for (std::size_t slot = 0; slot < kSlotNames.size(); ++slot) {
            traceRemove(referenceTrace,
                        std::string(kSlotNames[slot]) +
                            (game.worms[slot].selected ? "_on" : "_off"));
        }
        traceRemove(referenceTrace, "wood boarder");
        for (std::size_t slot = kSlotNames.size(); slot-- > 0;) {
            traceRemove(referenceTrace, game.panels[slot].current);
        }
        traceRemove(referenceTrace, "Help");
        traceRemove(referenceTrace, "Exit");
        traceRemove(referenceTrace, "exit_0");
        traceRemove(referenceTrace, "help_0");
    }
    game.level = 0;
    traceAdd(referenceTrace, "bottom", 8);
    traceAdd(referenceTrace, "Exit", 90);
    traceAdd(referenceTrace, "Help", 90);
    traceAdd(referenceTrace, "help_0", 93, -1, -1, 31);
    traceAdd(referenceTrace, "exit_0", 93, -1, -1, 32);
    traceAdd(referenceTrace, "wood boarder", 11);
    for (std::size_t slot = 0; slot < game.panels.size(); ++slot) {
        auto &panel = game.panels[slot];
        panel.state = 1;
        panel.pending.clear();
        panel.current = std::string(kSlotNames[slot]) +
                        (game.worms[slot].selected ? "_info" : "_text");
        traceAdd(referenceTrace, panel.current, 20 - static_cast<int>(slot));
    }
    // 0x004337F2: every selected slot starts playing with its lives
    // (0x00433890: three, one in tournament mode) and its panel slides to
    // the in-game leaf.
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        worm.playing = worm.selected;
        worm.score = 0;
        worm.frags = 0;
        if (worm.playing) {
            worm.lives = 3;
            setBugzzzPanel(slot, std::string(kLeafNames[slot]));
        } else {
            worm.lives = 0;
        }
    }
    game.bonusThreshold = 300;
    game.gameOverShown = false;
    game.pressedStart = false;
    setupBugzzzLevel();
}

void BugzzzScene::resetBugzzzWorm(std::size_t slot) {
    if (!bugzzzGame || slot >= bugzzzGame->worms.size()) return;
    auto &game = *bugzzzGame;
    auto &worm = game.worms[slot];
    const double now = sceneElapsedNow();
    // 0x00433650: start record, 100 joints laid back 11 px apart, seven
    // drawn nodes, level speed and a 22-tick spawn immunity.
    const auto &start = kWormStarts[slot];
    worm.x = start.x;
    worm.y = start.y;
    worm.heading = start.heading;
    worm.anchorX = worm.x;
    worm.anchorY = worm.y;
    worm.joints.clear();
    worm.jointHeadings.clear();
    for (int joint = 0; joint < 100; ++joint) {
        worm.joints.push_back({
            static_cast<float>(worm.x - std::cos(worm.heading) * joint *
                                            kJointSpacing),
            static_cast<float>(worm.y + std::sin(worm.heading) * joint *
                                            kJointSpacing)});
        worm.jointHeadings.push_back(worm.heading);
    }
    worm.nodes = 7;
    worm.eating = false;
    worm.dead = false;
    worm.poisoned = false;
    worm.speedEffect = false;
    worm.spits = 0;
    worm.lastSpitTick = -1;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    worm.speed = std::min(level.wormSpeed * 2.0, kMaximumSpeed);
    worm.invulnerable = true;
    worm.invulnerableUntilTick = bugzzzTick(now) + kInvulnerableTicks;
}

void BugzzzScene::setupBugzzzLevel() {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    const double now = sceneElapsedNow();
    // 0x00433260: re-initialize every playing worm, clear pickups, spits
    // and popups, then queue the level's pickups.
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        if (!worm.playing) continue;
        worm.antsEaten = 0;
        resetBugzzzWorm(slot);
    }
    game.pickups.clear();
    game.spits.clear();
    game.popups.clear();
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    const int second = bugzzzSecond(now);
    const auto random = [&](int range) {
        if (range <= 0) return 0;
        return std::uniform_int_distribution<int>(0, range - 1)(randomEngine);
    };
    const auto queue = [&](BugzzzPickupKind kind, int delay) {
        BugzzzPickup pickup;
        pickup.kind = kind;
        pickup.state = BugzzzPickupState::Waiting;
        pickup.spawnSecond = second + delay;
        game.pickups.push_back(pickup);
    };
    for (std::size_t type = 0; type < level.counts.size(); ++type) {
        for (int count = 0; count < level.counts[type]; ++count) {
            // Ants (types 1..3) wait rand(5) s; mushrooms wait
            // rand(deadline - 10) s (0x004332C1-0x00433473).
            const int delay = type < 3 ? random(5) : random(level.seconds - 10);
            queue(static_cast<BugzzzPickupKind>(type + 1), delay);
        }
    }
    // Remove whatever woodpecker clip or branch is still present, then
    // start bird_in at layer 10 (0x00433497-0x004335C5).
    if (!game.birdClip.empty()) {
        traceRemove(referenceTrace, game.birdClip);
    }
    if (game.branchIndex >= 0) {
        traceRemove(referenceTrace,
                    "branch" + std::string(game.branchIndex + 1 < 10 ? "0" : "") +
                        std::to_string(game.branchIndex + 1));
    }
    game.branchIndex = -1;
    game.birdClip = "bird_in";
    game.birdStarted = now;
    traceAdd(referenceTrace, "bird_in", 10);
    // 0x00433240: the deadline is the level's seconds past the current
    // seconds counter.
    game.deadlineSecond = level.seconds + second;
    game.phase = BugzzzPhase::Playing;
    game.lastUpdate = now;
}

void BugzzzScene::beginBugzzzLevel(bool retainScore) {
    if (!bugzzzGame) return;
    if (!retainScore) {
        for (auto &worm : bugzzzGame->worms) worm.score = 0;
    }
    setupBugzzzLevel();
}

bool BugzzzScene::bugzzzMaskBlocked(int x, int y) const {
    if (!bugzzzGame) return true;
    // 0x004355A0: outside the top, left or bottom edge, or any mask byte
    // different from the free byte at (200,0), blocks.
    if (x < 0 || y < 0 || y >= 600) return true;
    SDL_Surface *mask =
        bugzzzGame->treeMasks[bugzzzGame->level == 0 ? 0 : 1].get();
    return pixelValue(mask, x, y) != pixelValue(mask, 200, 0);
}

bool BugzzzScene::bugzzzPlayableAt(double x, double y, int radius) const {
    constexpr std::array<SDL_Point, 9> samples{{
        {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1}}};
    for (const auto sample : samples) {
        if (bugzzzMaskBlocked(static_cast<int>(x) + sample.x * radius,
                              static_cast<int>(y) + sample.y * radius)) {
            return false;
        }
    }
    return true;
}

void BugzzzScene::turnBugzzzWorm(std::size_t index, int direction) {
    if (!bugzzzGame || index >= bugzzzGame->worms.size()) return;
    auto &worm = bugzzzGame->worms[index];
    if (!worm.playing || worm.dead) return;
    // One nominal frame of a held key (0x004369B0).
    worm.heading -= direction * kTurnPerFrame;
}

void BugzzzScene::spitBugzzzWorm(std::size_t index) {
    if (!bugzzzGame || index >= bugzzzGame->worms.size()) return;
    auto &game = *bugzzzGame;
    auto &worm = game.worms[index];
    if (!worm.playing || worm.dead || worm.spits <= 0) return;
    // 0x00436A20: one spit per 333 ms gate tick, launched from the probe
    // point at the worm's speed plus 3.0 (0x0049DA20).
    const auto gate = static_cast<long long>(
        std::floor((sceneElapsedNow() - game.clockOrigin) / kSpitTickSeconds));
    if (gate == worm.lastSpitTick) return;
    worm.lastSpitTick = gate;
    --worm.spits;
    const double angle = direction32(worm.heading) * kTau / 32.0;
    const SDL_FPoint origin = probePoint(worm.x, worm.y, worm.heading);
    game.spits.push_back({index, origin.x, origin.y,
                          std::cos(angle) * (worm.speed + 3.0),
                          -std::sin(angle) * (worm.speed + 3.0)});
}

void BugzzzScene::loseBugzzzLife(std::size_t index, std::string_view reason) {
    if (!bugzzzGame || index >= bugzzzGame->worms.size()) return;
    auto &game = *bugzzzGame;
    auto &worm = game.worms[index];
    // 0x00435EE0 ignores an immune, dead or out-of-lives worm.
    if (!worm.playing || worm.dead || worm.invulnerable || worm.lives <= 0) {
        return;
    }
    --worm.lives;
    worm.dead = true;
    worm.deathTick = bugzzzTick(sceneElapsedNow());
    std::cout << "Bugzzz player " << index + 1 << " lost a life (" << reason
              << "), " << worm.lives << " remaining\n";
}

void BugzzzScene::finishBugzzzGame() {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    if (game.gameOverShown) return;
    game.gameOverShown = true;
    game.phase = BugzzzPhase::Finished;
    const auto playing = static_cast<int>(std::count_if(
        game.worms.begin(), game.worms.end(),
        [](const BugzzzWorm &worm) { return worm.playing; }));
    // 0x00434530: one player reaches the shared scored-minigame wrapper
    // (0x0041F9A0) with the 500/1000/1500/2000 table at 0x004D7200; more
    // players get the bugzzz:RANKTITLE ranking message instead.
    if (playing <= 1) {
        const auto winner = std::find_if(
            game.worms.begin(), game.worms.end(),
            [](const BugzzzWorm &worm) { return worm.playing; });
        showHighScore(winner == game.worms.end() ? 0 : winner->score,
                      "HISCORE");
    } else {
        std::array<std::size_t, 4> order{{0U, 1U, 2U, 3U}};
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t left, std::size_t right) {
                             return game.worms[left].score >
                                    game.worms[right].score;
                         });
        std::string ranking;
        int rank = 1;
        for (const std::size_t slot : order) {
            if (!game.worms[slot].playing) continue;
            std::string row = language.get(
                "bugzzz", "RANKFORMAT", "%d. %s: %d");
            row = replaceFirst(row, "%d", std::to_string(rank++));
            row = replaceFirst(row, "%s",
                               language.get("bugzzz",
                                            "PLAYER" + std::to_string(slot + 1),
                                            std::string(kSlotNames[slot])));
            row = replaceFirst(row, "%d",
                               std::to_string(game.worms[slot].score));
            ranking += row + "\n";
        }
        showMessageWithTitle(language.get("bugzzz", "RANKTITLE", "Resultat"),
                             ranking, MessageKind::Notice);
    }
    game.pendingLobby = true;
}

void BugzzzScene::updateBugzzz(double elapsed) {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    updateBugzzzPanels(elapsed);
    // FGP.exe's message boxes are modal; native dialogs are asynchronous,
    // so the board waits while one is open and resumes on dismissal.
    if (dialogMode != DialogMode::None) {
        game.lastUpdate = elapsed;
        return;
    }
    if (game.pendingLobby) {
        game.pendingLobby = false;
        buildBugzzzLobby(game, false);
        return;
    }
    if (game.pendingTimeoutRestart) {
        // 0x00437CEE-0x00437CF8: after TIMEOUT closes, restart the board.
        game.pendingTimeoutRestart = false;
        setupBugzzzLevel();
        return;
    }
    if (game.pendingLevelScroll) {
        // 0x00436EA0 adds the next background above the screen and starts
        // the 600 px climb.
        game.pendingLevelScroll = false;
        traceAdd(referenceTrace, (game.level & 1) == 0 ? "middle01" : "middle02",
                 8, -1, -600);
        game.scrollStarted = elapsed;
        game.phase = BugzzzPhase::Scrolling;
        return;
    }
    if (game.phase == BugzzzPhase::Starting) {
        if (elapsed >= game.startAt) startBugzzzGame();
        return;
    }
    if (game.phase == BugzzzPhase::Scrolling) {
        // 0x00433E40: 600 px over 4000 ms, then 0x00433220 advances (capped
        // at the ninth row) and sets the board up again.
        if ((elapsed - game.scrollStarted) * 150.0 >= 600.0) {
            game.level = std::min(game.level + 1, 8);
            setupBugzzzLevel();
        }
        return;
    }
    if (game.phase == BugzzzPhase::Timeout) {
        const auto &clip = game.birdClip == "hakkned" ? game.birdTimeout
                                                      : game.birdFall;
        if (elapsed - game.birdStarted >= clip.media.videoDuration) {
            // 0x00437C80: the finished clip opens bugzzz:TIMEOUT.
            traceRemove(referenceTrace, game.birdClip);
            game.birdClip.clear();
            showMessageWithTitle(
                language.get("bugzzz", "TIMEOUT", "Tiden er ute"),
                language.get("bugzzz", "TIMEOUTMSG",
                             "Hakkespetten hakket gjennom grenen."),
                MessageKind::Notice);
            game.pendingTimeoutRestart = true;
        }
        return;
    }
    if (game.phase != BugzzzPhase::Playing) return;

    const double dt = std::clamp(elapsed - game.lastUpdate, 0.0, 0.25);
    game.lastUpdate = elapsed;
    const double scale = dt * kNominalFramesPerSecond;
    const int second = bugzzzSecond(elapsed);
    const long long tick = bugzzzTick(elapsed);
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];

    // bird_in hands over to the looping bird_hakke (0x00437C80).
    if (game.birdClip == "bird_in" &&
        elapsed - game.birdStarted >= game.birdIn.media.videoDuration) {
        traceRemove(referenceTrace, "bird_in");
        game.birdClip = "bird_hakke";
        game.birdStarted = elapsed;
        traceAdd(referenceTrace, "bird_hakke", 10);
    }

    // 0x00435F50: the seconds clock reaching the deadline kills every
    // playing worm and swaps the woodpecker for its timeout clip.
    const int remaining = game.deadlineSecond - second;
    if (remaining <= 0) {
        for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
            auto &worm = game.worms[slot];
            if (worm.playing && worm.lives > 0 && !worm.dead) {
                loseBugzzzLife(slot, "woodpecker timeout");
            }
        }
        if (game.birdClip == "bird_hakke" || game.birdClip == "bird_in") {
            traceRemove(referenceTrace, game.birdClip);
        }
        if (game.branchIndex >= 0) {
            traceRemove(referenceTrace,
                        "branch" +
                            std::string(game.branchIndex + 1 < 10 ? "0" : "") +
                            std::to_string(game.branchIndex + 1));
            game.branchIndex = -1;
        }
        game.birdClip = game.level == 0 ? "hakkned" : "branch_fall_from_top";
        game.birdStarted = elapsed;
        traceAdd(referenceTrace, game.birdClip, 10);
        game.phase = BugzzzPhase::Timeout;
        return;
    }

    // 0x00436D30: dying worms finish their 21-tick die strip and respawn.
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        if (!worm.playing || !worm.dead) continue;
        if (tick - worm.deathTick >= game.dieSet.frames) {
            worm.dead = false;
            if (worm.lives > 0) resetBugzzzWorm(slot);
        }
    }
    // 0x00436FA0: the level ends when the playing worms' ants together
    // reach the quota; 0x00436DB0 announces it before the climb.
    int eaten = 0;
    for (const auto &worm : game.worms) {
        if (worm.playing) eaten += worm.antsEaten;
    }
    if (eaten >= level.antQuota) {
        std::size_t best = 0;
        for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
            if (game.worms[slot].playing &&
                (!game.worms[best].playing ||
                 game.worms[slot].score > game.worms[best].score)) {
                best = slot;
            }
        }
        const auto playing = std::count_if(
            game.worms.begin(), game.worms.end(),
            [](const BugzzzWorm &worm) { return worm.playing; });
        std::string title = replaceFirst(
            language.get("bugzzz", "TITLEMSG", "Brett %d"), "%d",
            std::to_string(game.level + 1));
        std::string body = playing > 1
            ? replaceFirst(language.get("bugzzz", "MPLAYERMSG", "%s vant!"),
                           "%s",
                           language.get("bugzzz",
                                        "PLAYER" + std::to_string(best + 1),
                                        std::string(kSlotNames[best])))
            : language.get("bugzzz", "SPLAYERMSG", "Bra!");
        showMessageWithTitle(title, body, MessageKind::Notice);
        game.pendingLevelScroll = true;
        return;
    }
    // 0x00436C20: no playing worm with lives or a running die strip ends
    // the game (0x00434530).
    if (std::none_of(game.worms.begin(), game.worms.end(),
                     [](const BugzzzWorm &worm) {
                         return worm.playing && (worm.lives > 0 || worm.dead);
                     })) {
        finishBugzzzGame();
        return;
    }

    // 0x004352C0: branch index = trunc((1 - remaining / deadline) * 30.0).
    const int branch = static_cast<int>(
        (1.0 - static_cast<double>(remaining) / level.seconds) * 30.0);
    if (game.branchIndex == -1) {
        game.branchIndex = branch;
        traceAdd(referenceTrace, "branch01", 99);
    } else if (branch != game.branchIndex) {
        const auto name = [](int index) {
            return "branch" + std::string(index + 1 < 10 ? "0" : "") +
                   std::to_string(index + 1);
        };
        traceRemove(referenceTrace, name(game.branchIndex));
        game.branchIndex = branch;
        traceAdd(referenceTrace, name(branch), 10);
    }

    // 0x00435370: each 300 points scored queues a bonus life (type 9).
    for (const auto &worm : game.worms) {
        if (!worm.playing || worm.score < game.bonusThreshold) continue;
        BugzzzPickup bonus;
        bonus.kind = BugzzzPickupKind::Life;
        bonus.spawnSecond = second + 5;
        game.pickups.push_back(bonus);
        game.bonusThreshold += 300;
    }
    // 0x004353C0: spawn immunity ends on its tick.
    for (auto &worm : game.worms) {
        if (worm.invulnerable && worm.invulnerableUntilTick <= tick) {
            worm.invulnerable = false;
        }
    }

    // 0x00436710: steer and spit from the held keys, expire speed
    // mushrooms after ten seconds (0x00436BE0), then move.
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    struct Controls {
        SDL_Scancode left;
        SDL_Scancode right;
        SDL_Scancode spit;
        SDL_Scancode spitAlt;
    };
    // Key codes stored at 0x00659D10, 0x0065ABC4, 0x0065BA78 and 0x0065C92C.
    constexpr std::array<Controls, 4> controls{{
        {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN,
         SDL_SCANCODE_KP_ENTER},
        {SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_LCTRL,
         SDL_SCANCODE_RCTRL},
        {SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_SPACE,
         SDL_SCANCODE_SPACE},
        {SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_MINUS,
         SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_DIVIDE},
    }};
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        if (!worm.playing || worm.lives <= 0 || worm.dead) continue;
        if (keys[controls[slot].left]) worm.heading += kTurnPerFrame * scale;
        if (keys[controls[slot].right]) worm.heading -= kTurnPerFrame * scale;
        if (keys[controls[slot].spit] || keys[controls[slot].spitAlt]) {
            spitBugzzzWorm(slot);
        }
        if (worm.speedEffect && second - worm.speedEffectSecond >= 10) {
            worm.speed = std::min(level.wormSpeed * 2.0, kMaximumSpeed);
            worm.speedEffect = false;
        }
        // 0x004367E0 moves the head, then advances the chain by one
        // 11 px joint whenever the head is 11 px from the last joint.
        worm.x += std::cos(worm.heading) * worm.speed * scale;
        worm.y -= std::sin(worm.heading) * worm.speed * scale;
        const double dx = worm.x - worm.anchorX;
        const double dy = worm.y - worm.anchorY;
        const double squared = dx * dx + dy * dy;
        if (squared >= kJointSpacing * kJointSpacing) {
            const double step = kJointSpacing / std::sqrt(squared);
            worm.anchorX += dx * step;
            worm.anchorY += dy * step;
            worm.joints.insert(worm.joints.begin(),
                               {static_cast<float>(worm.anchorX),
                                static_cast<float>(worm.anchorY)});
            worm.jointHeadings.insert(worm.jointHeadings.begin(),
                                      worm.heading);
            worm.joints.pop_back();
            worm.jointHeadings.pop_back();
        }
    }

    // Pickups: activation on their second, appear strips, ant walking.
    for (auto &pickup : game.pickups) {
        const auto type = static_cast<std::size_t>(pickup.kind);
        if (pickup.state == BugzzzPickupState::Waiting) {
            if (second < pickup.spawnSecond) continue;
            // 0x00433AB4: x in [30,450), y in [100,450), avoiding other
            // live pickups.
            std::uniform_int_distribution<int> xs(30, 449);
            std::uniform_int_distribution<int> ys(100, 449);
            for (int attempt = 0; attempt < 2; ++attempt) {
                pickup.x = xs(randomEngine);
                pickup.y = ys(randomEngine);
                const bool overlaps = std::any_of(
                    game.pickups.begin(), game.pickups.end(),
                    [&](const BugzzzPickup &other) {
                        return &other != &pickup &&
                               other.state != BugzzzPickupState::Waiting &&
                               other.state != BugzzzPickupState::Gone &&
                               std::abs(other.x - pickup.x) < 32 &&
                               std::abs(other.y - pickup.y) < 32;
                    });
                if (!overlaps) break;
            }
            pickup.heading = std::uniform_real_distribution<double>(
                0.0, kTau)(randomEngine);
            // 0x00434A90: yellow ants walk at the level's ant speed, red
            // ants at 1.5x and brown ants at 2x, doubled per nominal frame.
            const double antSpeed = level.antSpeed;
            pickup.speed = pickup.kind == BugzzzPickupKind::YellowAnt
                ? antSpeed
                : pickup.kind == BugzzzPickupKind::RedAnt ? antSpeed * 1.5
                                                          : antSpeed * 2.0;
            pickup.state = pickup.kind == BugzzzPickupKind::Life
                ? BugzzzPickupState::Static
                : BugzzzPickupState::Appearing;
            pickup.stateTick = tick;
            continue;
        }
        if (pickup.state == BugzzzPickupState::Appearing) {
            const auto &set = game.appearSets[type];
            if (tick - pickup.stateTick >= set.frames) {
                pickup.state = type <= 3U ? BugzzzPickupState::Walking
                                          : BugzzzPickupState::Static;
                pickup.stateTick = tick;
            }
            continue;
        }
        if (pickup.state == BugzzzPickupState::Exploding) {
            const auto &set = game.burstSets[type];
            if ((tick - pickup.stateTick) * set.rate >= set.frames) {
                pickup.state = BugzzzPickupState::Gone;
            }
            continue;
        }
        if (pickup.state != BugzzzPickupState::Walking) continue;
        // UNCERTAIN: 0x00434AF0's steering (bounds re-aim, worm avoidance,
        // turn timers) is condensed to its bounds and mask reactions here.
        const double nx = pickup.x + std::cos(pickup.heading) * pickup.speed * scale;
        const double ny = pickup.y - std::sin(pickup.heading) * pickup.speed * scale;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const auto aim = [&](double low, double high) {
            pickup.heading = low + (high - low) * unit(randomEngine);
        };
        if (nx <= 30.0) aim(-1.4960, 1.4960);
        else if (nx >= 600.0) aim(1.6534, 4.7951);
        else if (ny <= 30.0) aim(3.1916, 6.2332);
        else if (ny >= 522.0) aim(0.05, 3.0916);
        else if (bugzzzMaskBlocked(static_cast<int>(nx), static_cast<int>(ny))) {
            pickup.heading += std::numbers::pi / 2.0 +
                              0.1 + 2.566 * unit(randomEngine);
        } else {
            pickup.x = nx;
            pickup.y = ny;
        }
    }
    std::erase_if(game.pickups, [](const BugzzzPickup &pickup) {
        return pickup.state == BugzzzPickupState::Gone;
    });

    // Spits fly until they leave the screen or hit something.
    for (auto &spit : game.spits) {
        spit.x += spit.dx * scale;
        spit.y += spit.dy * scale;
    }

    // 0x004362D0: eating. The probe point must land in the pickup's box:
    // size/3 around ants and mushrooms, size/5 around the red mushroom.
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        if (!worm.playing || worm.lives <= 0 || worm.dead) continue;
        const SDL_FPoint probe = probePoint(worm.x, worm.y, worm.heading);
        const std::size_t count = game.pickups.size();
        for (std::size_t index = 0; index < count; ++index) {
            auto &pickup = game.pickups[index];
            if (pickup.state == BugzzzPickupState::Waiting ||
                pickup.state == BugzzzPickupState::Appearing ||
                pickup.state == BugzzzPickupState::Exploding ||
                pickup.state == BugzzzPickupState::Gone) {
                continue;
            }
            const auto type = static_cast<std::size_t>(pickup.kind);
            const int size = type <= 3U ? 32 : 64;
            const int half = pickup.kind == BugzzzPickupKind::RedMushroom
                ? size / 5 : size / 3;
            if (!inBox(probe, pickup.x, pickup.y, half)) continue;
            const int points = kPickupPoints[type];
            worm.score += points;
            if (points > 0) {
                game.popups.push_back({points, pickup.x, pickup.y, tick});
            }
            if (type <= 3U) {
                BugzzzPickup next;
                next.kind = pickup.kind;
                next.spawnSecond = second +
                    std::uniform_int_distribution<int>(0, 4)(randomEngine);
                game.pickups.push_back(next);
            }
            auto &eatenPickup = game.pickups[index];
            switch (eatenPickup.kind) {
            case BugzzzPickupKind::YellowAnt:
            case BugzzzPickupKind::RedAnt:
            case BugzzzPickupKind::BrownAnt:
                ++worm.antsEaten;
                eatenPickup.state = BugzzzPickupState::Gone;
                break;
            case BugzzzPickupKind::WhiteMushroom:
                worm.speed = std::min(level.wormSpeed * 2.0 + 2.0,
                                      kMaximumSpeed);
                worm.speedEffect = true;
                worm.speedEffectSecond = second;
                break;
            case BugzzzPickupKind::BlackMushroom:
                worm.speed = std::min(level.wormSpeed * 2.0 - 1.0,
                                      kMaximumSpeed);
                worm.speedEffect = true;
                worm.speedEffectSecond = second;
                break;
            case BugzzzPickupKind::RedMushroom:
                worm.poisoned = true;
                break;
            case BugzzzPickupKind::BlueMushroom:
                worm.spits += 5;
                break;
            case BugzzzPickupKind::GreenMushroom:
                for (auto &ant : game.pickups) {
                    if (static_cast<int>(ant.kind) <= 3 &&
                        ant.state == BugzzzPickupState::Walking) {
                        ant.state = BugzzzPickupState::Stopped;
                    }
                }
                break;
            case BugzzzPickupKind::Life:
                ++worm.lives;
                break;
            }
            if (eatenPickup.state != BugzzzPickupState::Gone) {
                eatenPickup.state = BugzzzPickupState::Exploding;
                eatenPickup.stateTick = tick;
            }
            if (!worm.eating) {
                worm.eating = true;
                worm.eatTick = tick;
            }
            if (worm.nodes < 97) ++worm.nodes;
        }
    }
    std::erase_if(game.pickups, [](const BugzzzPickup &pickup) {
        return pickup.state == BugzzzPickupState::Gone;
    });

    // Spit hits: ants stop, mushrooms burst, worms die (0x00435BD0).
    for (auto &spit : game.spits) {
        const SDL_FPoint point{static_cast<float>(spit.x),
                               static_cast<float>(spit.y)};
        bool hit = false;
        for (auto &pickup : game.pickups) {
            if (pickup.state != BugzzzPickupState::Walking &&
                pickup.state != BugzzzPickupState::Static &&
                pickup.state != BugzzzPickupState::Stopped) {
                continue;
            }
            const int half = static_cast<int>(pickup.kind) <= 3 ? 8 : 16;
            if (!inBox(point, pickup.x, pickup.y, half)) continue;
            if (static_cast<int>(pickup.kind) <= 3) {
                pickup.state = BugzzzPickupState::Stopped;
            } else {
                pickup.state = BugzzzPickupState::Exploding;
                pickup.stateTick = tick;
            }
            hit = true;
            break;
        }
        for (std::size_t slot = 0; !hit && slot < game.worms.size(); ++slot) {
            auto &target = game.worms[slot];
            if (slot == spit.owner || !target.playing || target.dead ||
                target.lives <= 0) {
                continue;
            }
            if (inBox(point, target.x, target.y, 16)) {
                loseBugzzzLife(slot, "spit");
                ++game.worms[spit.owner].frags;
                hit = true;
            }
        }
        if (hit) spit.x = -1000.0;
    }
    std::erase_if(game.spits, [](const BugzzzSpit &spit) {
        return spit.x < 0.0 || spit.x >= 800.0 || spit.y < 0.0 ||
               spit.y >= 600.0;
    });

    // 0x00435690 / 0x00435850: poison, the tree mask at the probe point,
    // other heads (+/-16), other bodies and the worm's own nodes past the
    // sixth (+/-8).
    for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
        auto &worm = game.worms[slot];
        if (!worm.playing || worm.lives <= 0 || worm.dead) continue;
        const SDL_FPoint probe = probePoint(worm.x, worm.y, worm.heading);
        std::string_view reason;
        if (worm.poisoned) {
            reason = "poison mushroom";
        } else if (bugzzzMaskBlocked(static_cast<int>(probe.x),
                                     static_cast<int>(probe.y))) {
            reason = "tree edge";
        }
        const auto bodyNode = [](const BugzzzWorm &owner, int node) {
            const double dx = owner.x - owner.anchorX;
            const double dy = owner.y - owner.anchorY;
            const double fraction =
                std::sqrt(dx * dx + dy * dy) / kJointSpacing;
            const auto &near = owner.joints[static_cast<std::size_t>(node - 1)];
            const auto &far = owner.joints[static_cast<std::size_t>(node)];
            return SDL_FPoint{
                static_cast<float>(far.x + (near.x - far.x) * fraction),
                static_cast<float>(far.y + (near.y - far.y) * fraction)};
        };
        for (int node = 7; reason.empty() && node < worm.nodes; ++node) {
            const SDL_FPoint at = bodyNode(worm, node);
            if (inBox(probe, at.x, at.y, 8)) reason = "own body";
        }
        for (std::size_t other = 0; reason.empty() && other < game.worms.size();
             ++other) {
            const auto &rival = game.worms[other];
            if (other == slot || !rival.playing || rival.lives <= 0) continue;
            if (inBox(probe, rival.x, rival.y, 16)) reason = "other worm";
            for (int node = 1; reason.empty() && node < rival.nodes; ++node) {
                const SDL_FPoint at = bodyNode(rival, node);
                if (inBox(probe, at.x, at.y, 8)) reason = "other worm";
            }
        }
        if (!reason.empty()) loseBugzzzLife(slot, reason);
        if (worm.eating && tick - worm.eatTick >= game.eatSet.frames) {
            worm.eating = false;
        }
    }
    std::erase_if(game.popups, [&](const BugzzzPopup &popup) {
        return tick - popup.startTick >= 16;
    });
}

void BugzzzScene::renderBugzzz(double elapsed) {
    if (!bugzzzGame) return;
    auto &game = *bugzzzGame;
    const long long tick = bugzzzTick(elapsed);
    const bool board = game.phase != BugzzzPhase::Lobby &&
                       game.phase != BugzzzPhase::Starting;
    const bool scrolling = game.phase == BugzzzPhase::Scrolling;
    const int scroll = scrolling
        ? std::min(600, static_cast<int>((elapsed - game.scrollStarted) * 150.0))
        : 0;
    // FGP.exe 0x00434010 selects `bottom` for level 0 and middle01/middle02
    // alternately for later levels (also the pre-Start lobby, level 0).
    const auto background = [&](int level) -> SDL_Texture * {
        if (level == 0) return game.bottomTexture.get();
        return game.middleTextures[static_cast<std::size_t>((level - 1) & 1)]
            .get();
    };
    SDL_Rect screen{0, scroll, 800, 600};
    SDL_RenderCopy(renderer, background(game.level), nullptr, &screen);
    if (scrolling) {
        SDL_Rect next{0, scroll - 600, 800, 600};
        SDL_RenderCopy(renderer, background(std::min(game.level + 1, 8)),
                       nullptr, &next);
    }

    const auto drawMedia = [&](HopscotchMedia &placed, int x, int y,
                               double time, bool loop) {
        if (!placed.texture || placed.media.videoFrames.empty()) return;
        const auto frameIndex = frameIndexAt(placed.media, time, loop);
        const auto &frame = placed.media.videoFrames[frameIndex];
        uploadVideoFrame(placed.texture.get(), frame);
        SDL_Rect destination{x, y, frame.width, frame.height};
        SDL_RenderCopy(renderer, placed.texture.get(), nullptr, &destination);
    };
    const auto drawSprite = [&](const BugzzzSpriteSet &set, double x, double y,
                                long long startTick, int direction) {
        // 0x004351D0: frame = (tick - start) * rate, clamped for one-shot
        // strips; directional strips choose the row (direction >> shift).
        if (!set.texture) return;
        long long frame = static_cast<long long>(
            std::max<long long>(0, tick - startTick) * set.rate);
        frame = set.oneShot ? std::min<long long>(frame, set.frames - 1)
                            : frame % set.frames;
        const int row = set.directional ? (direction & 31) >> set.rowShift : 0;
        SDL_Rect source{static_cast<int>(frame) * set.size, row * set.size,
                        set.size, set.size};
        SDL_Rect destination{static_cast<int>(x) - set.size / 2,
                             static_cast<int>(y) - set.size / 2, set.size,
                             set.size};
        SDL_RenderCopy(renderer, set.texture.get(), &source, &destination);
    };
    const auto drawCornerLeaves = [&] {
        // The Bugzzz corner initializer adds the keyed Help and Exit leaf
        // descriptors at layer 90 before installing the two controls
        // (FGP.exe 0x00434240-0x004342B0). Their descriptor coordinates are
        // (0,-1) at 0x004D4338 and (703,0) at 0x004D4290 respectively.
        SDL_Rect help{0, -1, 90, 89};
        SDL_Rect exit{703, 0, 97, 114};
        SDL_RenderCopy(renderer, game.helpLeafTexture.get(), nullptr, &help);
        SDL_RenderCopy(renderer, game.exitLeafTexture.get(), nullptr, &exit);
    };
    const auto panelY = [&](std::size_t slot) {
        // 0x00433C20: slide out along the table, slide in reversed.
        const auto &panel = game.panels[slot];
        const int steps = static_cast<int>((elapsed - panel.changedAt) * 60.0);
        if (panel.state == 0) {
            return kPanelSlide[static_cast<std::size_t>(
                std::clamp(29 - steps, 0, 29))];
        }
        if (panel.state >= 2) {
            return kPanelSlide[static_cast<std::size_t>(
                std::clamp(steps, 0, 29))];
        }
        return 524;
    };
    const auto panelTexture = [&](std::size_t slot) -> SDL_Texture * {
        const std::string &name = game.panels[slot].current;
        if (name.ends_with("_info")) return game.playerInfoTextures[slot].get();
        if (name.ends_with("eaf")) return game.playerLeafTextures[slot].get();
        return game.playerTextTextures[slot].get();
    };
    const auto drawPanels = [&] {
        SDL_Rect wood{0, 524, 800, 76};
        SDL_RenderCopy(renderer, game.woodBorderTexture.get(), nullptr, &wood);
        // Layers fall from green (20) to yellow (17), so paint right to
        // left; the layer-31 text objects follow once every leaf is down.
        for (std::size_t slot = game.panels.size(); slot-- > 0;) {
            SDL_Rect panel{panelX(slot), panelY(slot), 315, 76};
            SDL_RenderCopy(renderer, panelTexture(slot), nullptr, &panel);
        }
        for (std::size_t slot = 0; slot < game.panels.size(); ++slot) {
            const std::string &name = game.panels[slot].current;
            const int x = panelX(slot);
            const int y = panelY(slot);
            if (name.ends_with("_info")) {
                // The ready-message descriptors begin at x=40/240/440/640
                // and y=579 (for example 0x004d6f30 in the first slot).
                drawText(renderer, game.hudFont,
                         language.get("bugzzz", "READYMSG2",
                                      "Gj\xf8r deg klar!"),
                         x + 140, y + 55);
            } else if (name.ends_with("_text")) {
                // 0x00436FE0 sets the join prompt through 0x00407A10, which
                // is 0x004079C0 with flags 0: left/top of the rectangle at
                // (125,42) inside the panel (0x004D6C40).
                drawText(renderer, game.hudFont,
                         bugzzzJoinPrompt(language.get(
                             "bugzzz", "JOINMSG1",
                             "Trykk '%d' for \xe5 bli med!"),
                             static_cast<int>(slot + 1)),
                         x + 125, y + 42);
            } else {
                // 0x00436080 writes four right-aligned (flag 0x200)
                // counters into the rectangles at 0x004D60C0 (score),
                // 0x004D63A0 (spits), 0x004D6680 (lives) and 0x004D6960
                // (ants still needed).
                const auto &worm = game.worms[slot];
                const auto &level =
                    game.levels[static_cast<std::size_t>(game.level)];
                const auto right = [&](int value, int dx, int dy, int width) {
                    const std::string text = std::to_string(value);
                    drawText(renderer, game.hudFont, text,
                             x + dx + width - textWidth(game.hudFont, text),
                             y + dy);
                };
                right(worm.score, 200, 53, 74);
                right(worm.spits, 155, 23, 16);
                right(worm.lives, 220, 23, 32);
                right(level.antQuota - worm.antsEaten, 131, 51, 25);
            }
        }
    };

    if (!board) {
        SDL_Rect selector{203, 243, 348, 125};
        // Initialization layers the wooden difficulty backing at 9, the
        // player and Start controls at 10, and localized Start text at 11
        // (FGP.exe 0x00434109-0x00434199).
        SDL_RenderCopy(renderer, game.difficultyBackdropTexture.get(),
                       nullptr, &selector);
        SDL_RenderCopy(renderer,
            game.startTextures[static_cast<std::size_t>(game.pressedStart)].get(),
            nullptr, &selector);
        for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
            SDL_RenderCopy(renderer,
                (game.worms[slot].selected ? game.playerOnTextures[slot]
                                           : game.playerOffTextures[slot]).get(),
                nullptr, &selector);
        }
        // 0x00434109-0x0043414D: Amerigo BT_18_ (0x0065FA10), centered
        // (flag 0x101) in (265,295)-(493,338) at layer 11.
        drawCenteredText(renderer, menuTitleFont,
                         language.get("bugzzz", "START", "Start"),
                         SDL_Rect{265, 295, 228, 43});
        drawPanels();
        drawCornerLeaves();
        return;
    }

    if (game.branchIndex >= 0) {
        SDL_Rect branchDestination{698, 242 + scroll, 25, 22};
        SDL_RenderCopy(renderer,
            game.branchTextures[static_cast<std::size_t>(
                std::clamp(game.branchIndex + 1, 1, 30))].get(),
            nullptr, &branchDestination);
    }
    // Descriptor origins: bird_in (632,177) at 0x004D0EB8, bird_hakke
    // (637,198) at 0x004D1008, hakkned (636,178) at 0x004D0F60 and
    // branch_fall_from_top (630,180) at 0x004D10B0.
    const double clipTime = elapsed - game.birdStarted;
    if (game.birdClip == "bird_in") {
        drawMedia(game.birdIn, 632, 177 + scroll, clipTime, false);
    } else if (game.birdClip == "bird_hakke") {
        drawMedia(game.birdPeck, 637, 198 + scroll, clipTime, true);
    } else if (game.birdClip == "hakkned") {
        drawMedia(game.birdTimeout, 636, 178, clipTime, false);
    } else if (game.birdClip == "branch_fall_from_top") {
        drawMedia(game.birdFall, 630, 180, clipTime, false);
    }

    if (!scrolling) {
        for (const auto &pickup : game.pickups) {
            const auto type = static_cast<std::size_t>(pickup.kind);
            const int direction = spriteDirection(pickup.heading);
            switch (pickup.state) {
            case BugzzzPickupState::Appearing:
                drawSprite(game.appearSets[type], pickup.x, pickup.y,
                           pickup.stateTick, direction);
                break;
            case BugzzzPickupState::Walking:
                drawSprite(game.mainSets[type], pickup.x, pickup.y,
                           pickup.stateTick, direction);
                break;
            case BugzzzPickupState::Stopped:
                drawSprite(game.stopSets[type], pickup.x, pickup.y,
                           pickup.stateTick, direction);
                break;
            case BugzzzPickupState::Static:
                // One-shot mushroom strips hold their last frame once
                // they have appeared; the life pickup loops.
                drawSprite(game.mainSets[type], pickup.x, pickup.y,
                           pickup.stateTick - game.mainSets[type].frames,
                           direction);
                break;
            case BugzzzPickupState::Exploding:
                drawSprite(game.burstSets[type], pickup.x, pickup.y,
                           pickup.stateTick, direction);
                break;
            default:
                break;
            }
        }
        for (const auto &popup : game.popups) {
            const std::size_t index = popup.points == 5 ? 0U
                                      : popup.points == 10 ? 1U : 2U;
            drawSprite(game.popupSets[index], popup.x, popup.y,
                       popup.startTick, 0);
        }
        for (const auto &spit : game.spits) {
            drawSprite(game.spitSet, spit.x, spit.y, tick, 0);
        }
        for (std::size_t slot = 0; slot < game.worms.size(); ++slot) {
            const auto &worm = game.worms[slot];
            if (!worm.playing || (worm.lives <= 0 && !worm.dead)) continue;
            // 0x004356D1: a respawn-immune worm blinks on odd ticks.
            if (worm.invulnerable && (tick & 1) != 0) continue;
            const double dx = worm.x - worm.anchorX;
            const double dy = worm.y - worm.anchorY;
            const double fraction =
                std::sqrt(dx * dx + dy * dy) / kJointSpacing;
            const long long start = worm.dead ? worm.deathTick : 0;
            for (int node = worm.nodes - 1; node >= 1; --node) {
                const auto &near =
                    worm.joints[static_cast<std::size_t>(node - 1)];
                const auto &far = worm.joints[static_cast<std::size_t>(node)];
                const double angle =
                    worm.jointHeadings[static_cast<std::size_t>(node)] +
                    (worm.jointHeadings[static_cast<std::size_t>(node - 1)] -
                     worm.jointHeadings[static_cast<std::size_t>(node)]) *
                        fraction;
                drawSprite(worm.dead ? game.explodeSet : game.bodySets[slot],
                           far.x + (near.x - far.x) * fraction,
                           far.y + (near.y - far.y) * fraction, start,
                           spriteDirection(angle));
            }
            const auto &head = worm.dead ? game.dieSet
                               : worm.eating ? game.eatSet
                                             : game.headSet;
            drawSprite(head, worm.x, worm.y,
                       worm.dead ? worm.deathTick
                                 : worm.eating ? worm.eatTick : 0,
                       spriteDirection(worm.heading));
        }
    }

    // 0x00436080: level number and m:ss clock, centered (flag 0x101) in the
    // 80x20 rectangles at (725,230) and (725,310) created at 0x004371EE.
    const int remaining = std::max(0, game.deadlineSecond - bugzzzSecond(elapsed));
    char clock[16]{};
    std::snprintf(clock, sizeof(clock), "%02d:%02d", remaining / 60,
                  remaining % 60);
    drawCenteredText(renderer, dialogFont, std::to_string(game.level + 1),
                     SDL_Rect{725, 230 + scroll, 80, 20});
    drawCenteredText(renderer, dialogFont,
                     scrolling ? std::string{} : std::string(clock),
                     SDL_Rect{725, 310 + scroll, 80, 20});
    drawPanels();
    drawCornerLeaves();
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
BugzzzScene::bugzzzHotspotAt(int x, int y) const {
    if (!bugzzzGame || activeScene != "bugzzz" || !bugzzzGame->hotspotMask) {
        return std::nullopt;
    }
    // The package's own hotspots.bmp: 1..4 are the player slots (their
    // lobby button and bottom panel), 5 is Start.
    const auto id = pixelValue(bugzzzGame->hotspotMask.get(), x, y);
    if (id < 1U || id > 5U) return std::nullopt;
    if (id == 5U && bugzzzGame->phase != BugzzzPhase::Lobby) {
        return std::nullopt;
    }
    constexpr std::array<SDL_Rect, 5> bounds{{
        {0, 257, 292, 343}, {200, 257, 200, 343}, {381, 257, 219, 343},
        {464, 257, 336, 343}, {265, 295, 228, 45}}};
    return std::pair{id == 5U ? kStartHotspot
                              : kPlayerHotspotBase + (id - 1U),
                     bounds[id - 1U]};
}

bool BugzzzScene::executeBugzzzDeveloperCommand(std::string_view command) {
    constexpr std::string_view bugzzzControlPrefix = "bugzzz:";
    if (activeScene != "bugzzz" || !bugzzzGame) return false;
    auto &game = *bugzzzGame;
    const std::string_view control =
        command.substr(bugzzzControlPrefix.size());
    const auto playing = [&] {
        if (game.phase != BugzzzPhase::Playing) startBugzzzGame();
    };
    if (control == "start") {
        if (game.phase == BugzzzPhase::Lobby) {
            pressBugzzzStart();
            if (game.phase == BugzzzPhase::Lobby) startBugzzzGame();
        }
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 1 || level > 9) return false;
        playing();
        game.level = level - 1;
        setupBugzzzLevel();
    } else if (control.rfind("player:", 0) == 0) {
        const int player = std::stoi(std::string(control.substr(7)));
        if (player < 1 || player > 4) return false;
        if (!game.worms[static_cast<std::size_t>(player - 1)].selected) {
            toggleBugzzzPlayer(static_cast<std::size_t>(player - 1));
        }
    } else if (control == "left") {
        turnBugzzzWorm(0, -1);
    } else if (control == "right") {
        turnBugzzzWorm(0, 1);
    } else if (control == "spit") {
        game.worms[0].spits = std::max(1, game.worms[0].spits);
        game.worms[0].lastSpitTick = -1;
        spitBugzzzWorm(0);
    } else if (control == "eat-ant") {
        playing();
        auto &worm = game.worms[0];
        auto found = std::find_if(
            game.pickups.begin(), game.pickups.end(),
            [](const BugzzzPickup &pickup) {
                return static_cast<int>(pickup.kind) <= 3;
            });
        if (found == game.pickups.end()) return false;
        const SDL_FPoint probe = probePoint(worm.x, worm.y, worm.heading);
        found->x = probe.x;
        found->y = probe.y;
        found->state = BugzzzPickupState::Walking;
        found->speed = 0.0;
        found->stateTick = bugzzzTick(sceneElapsedNow());
    } else if (control == "poison") {
        playing();
        game.worms[0].invulnerable = false;
        loseBugzzzLife(0, "developer poison");
    } else if (control == "timeout") {
        playing();
        game.deadlineSecond = bugzzzSecond(sceneElapsedNow());
    } else if (control == "finish") {
        playing();
        const int quota = game.levels[
            static_cast<std::size_t>(game.level)].antQuota;
        for (auto &worm : game.worms) {
            if (worm.playing) {
                worm.antsEaten = quota;
                break;
            }
        }
    } else if (control == "dismiss") {
        // Developer-only: acknowledge an open Bugzzz notice (TIMEOUT,
        // TITLEMSG or the ranking) so scripted runs can continue.
        if (dialogMode != DialogMode::Message) return false;
        dialogMode = DialogMode::None;
    } else if (control == "status") {
        const auto activePlayers = std::count_if(
            game.worms.begin(), game.worms.end(),
            [](const BugzzzWorm &worm) { return worm.playing; });
        const auto activePickups = std::count_if(
            game.pickups.begin(), game.pickups.end(),
            [](const BugzzzPickup &pickup) {
                return pickup.state != BugzzzPickupState::Waiting;
            });
        const auto &worm = game.worms[0];
        std::cout << "Bugzzz status: level " << game.level + 1
                  << ", players " << activePlayers << ", score "
                  << worm.score << ", lives " << worm.lives << ", ants "
                  << worm.antsEaten << '/'
                  << game.levels[static_cast<std::size_t>(game.level)]
                         .antQuota
                  << ", nodes " << worm.nodes << ", spits " << worm.spits
                  << ", pickups " << activePickups << ", head "
                  << static_cast<int>(worm.x) << ','
                  << static_cast<int>(worm.y) << ", clock "
                  << game.deadlineSecond - bugzzzSecond(sceneElapsedNow())
                  << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Bugzzz control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
