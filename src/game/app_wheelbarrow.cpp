#include "game/app_wheelbarrow.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "media/smacker.h"

// Wheelbarrow is a port of FGP.exe 0x0044ed50-0x00450310. Every rule below
// names the address it was read from; REVERSE_ENGINEERING.md ("Wheelbarrow")
// has the complete account. The game is driven by original 0xa8-byte
// animation descriptors, so the native port keeps one WheelbarrowAnim per
// descriptor and mirrors each SCENE_AddAnim (0x00412840), SCENE_RemoveAnim
// (0x00412aa0) and SCENE_StopAnim (0x00412a60) call in the reference trace.

namespace fgp {
namespace {

constexpr std::uint32_t kStartHotspot = 301U;
// 0x0044fca1 passes the cart's y as the immediate 0x1de.
constexpr int kCartY = 478;
constexpr float kCartMinimum = 55.0F;   // FGP.exe 0x0049f20c
constexpr float kCartMaximum = 578.0F;  // FGP.exe 0x0049f208
constexpr float kCartSpeed = 300.0F;    // FGP.exe 0x0044f162
constexpr float kCartStart = 324.0F;    // FGP.exe 0x0044f153
constexpr float kDumpMaximum = 60.0F;   // FGP.exe 0x0049e060
constexpr float kAppleGravity = 600.0F; // FGP.exe 0x0049da18
constexpr float kAppleFloor = 620.0F;   // FGP.exe 0x0049f210
constexpr float kTickCap = 0.1F;        // FGP.exe 0x0049c908
constexpr int kCartCapacity = 12;       // 0x0044fefe
constexpr int kLevelApples = 24;        // 0x0044ef8f stores it at 0x0068bae8
constexpr int kLifeScore = 3000;        // 0x0044f8eb

struct RoofStage { int y; int restitution; };
// FGP.exe 0x0049f1d0, consumed by the update at 0x00450033.
const auto kRoofStages = originalArray<RoofStage, 5>(0x0049f1d0U);
// FGP.exe 0x0049f108. The stage selector chooses the bounce multiplier.
const auto kRestitution = originalArray<float, 4>(0x0049f108U);

struct SpawnPoint { int x; int y; int route; };
// FGP.exe 0x0049f158, ten triples selected by 0x00450270.
const auto kSpawnPoints = originalArray<SpawnPoint, 10>(0x0049f158U);

// 0x0044fd20: the cart's catch rectangles in local LTRB, left- and
// right-facing; 0x0044fe00 translates them by the rounded cart position.
constexpr std::array<SDL_Rect, 2> kCatchRects{{
    {39, 42, 94 - 39, 65 - 42}, {57, 42, 112 - 57, 65 - 42},
}};

// The direction and state name tables at 0x004e5f30 and 0x004e5f38.
constexpr std::array<std::string_view, 2> kDirections{{"left", "right"}};
constexpr std::array<std::string_view, 4> kCartStates{{
    "neutral", "cycle", "stop", "dump"}};
constexpr int kCartNeutral = 0;
constexpr int kCartCycle = 1;
constexpr int kCartDump = 3;

// SCENE clones (0x00412c60) take the first free 0xa8-byte slot here.
constexpr std::uint32_t kCloneBase = 0x0054fea8U;
constexpr std::uint32_t kCloneStride = 0xa8U;
constexpr std::uint32_t kAppleBase = 0x0068ca28U;
constexpr std::uint32_t kAppleStride = 0xc4U;

int wheelbarrowFill(int apples) {
    // 0x0044f410: apples * 5 times 0x2aaaaaab (a division by 6), then
    // `sar edx, 1` halves it again, so the stage is apples * 5 / 12 and only
    // a full cart of 12 reaches stage 5.
    return std::clamp(apples * 5 / 12, 0, 5);
}

// The x87 sequence used for every position (0x0044f37f, 0x0044f9d2, ...):
// add or subtract 0.5 by sign, then truncate through _ftol (0x0048c214).
int roundAway(float value) {
    return static_cast<int>(value > 0.0F ? value + 0.5F : value - 0.5F);
}

// The completion boundary originalSmackerCompletionDuration() derives from
// decoded frames, read from the Smacker header for audio-only clips: the
// last frame's start plus two 64 Hz player passes, capped at the length.
double smackerHeaderCompletion(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 20U) {
        return 0.0;
    }
    const auto u32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(bytes[offset]) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    };
    const auto frames = u32(12U);
    const auto rate = static_cast<std::int32_t>(u32(16U));
    const double frameSeconds = rate > 0 ? rate / 1000.0
        : (rate < 0 ? -rate / 100000.0 : 0.1);
    if (frames == 0U) {
        return 0.0;
    }
    return std::min(frames * frameSeconds,
                    (frames - 1U) * frameSeconds + 2.0 / 64.0);
}

std::string descriptorText(std::uint32_t address) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

} // namespace

WheelbarrowScene::WheelbarrowAnim WheelbarrowScene::staticAnim(
    std::uint32_t address, std::string name, bool smacker, bool visible,
    bool loop, int x, int y, std::uint32_t hotspot) {
    WheelbarrowAnim anim;
    anim.address = address;
    anim.name = std::move(name);
    anim.smacker = smacker;
    anim.visible = visible;
    anim.loop = loop;
    anim.defaultX = x;
    anim.defaultY = y;
    anim.hotspot = hotspot;
    return anim;
}

WheelbarrowScene::WheelbarrowGame WheelbarrowScene::loadWheelbarrowGame(
    Package &package) {
    WheelbarrowGame game;
    const auto image = [&](const std::string &name) {
        SurfacePtr surface = loadBmp(package.data(
            package.find("bitmap\\" + name + ".bmp")));
        const Uint32 key = pixelValue(surface.get(), 0, 0);
        if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0) {
            throw std::runtime_error(SDL_GetError());
        }
        auto &result = game.images[name];
        result.width = surface->w;
        result.height = surface->h;
        result.texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_BLEND);
    };
    const auto clip = [&](const std::string &name, bool video = true) {
        auto &result = game.clips[name];
        const auto bytes = activeSceneAnimationData(name);
        result.media = decodeSmacker(bytes, video, true, true, 0, nullptr,
                                     SmackerIndexZeroAlpha::Transparent,
                                     activeSceneClipKeying(name));
        result.completion = video
            ? originalSmackerCompletionDuration(result.media)
            : smackerHeaderCompletion(bytes);
        if (video && !result.media.videoFrames.empty()) {
            const auto &frame = result.media.videoFrames.front();
            result.texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!result.texture) {
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_BLEND);
        }
    };
    image("start");
    image("apple");
    image("pie");
    for (int fill = 0; fill < 4; ++fill) {
        image("basket" + std::to_string(fill));
    }
    for (int fill = 0; fill < 6; ++fill) {
        for (std::string_view direction : kDirections) {
            image(std::string(direction) + "_neutral" + std::to_string(fill));
            clip(std::string(direction) + "_cycle" + std::to_string(fill));
        }
        clip("left_dump" + std::to_string(fill));
    }
    // The package has right_dump0..3 only; 0x0044f6b0 dumps facing left.
    for (int fill = 0; fill < 4; ++fill) {
        clip("right_dump" + std::to_string(fill));
    }
    for (const char *name : {"apple_0", "apple_1", "rotten_0", "rotten_1",
                             "rotten_2", "pickup", "pie", "putdown"}) {
        clip(name);
    }
    // The invisible descriptors carry only sound: the bird ambience and
    // the 4x4 feedback clips (rotten1's video does not decode).
    clip("birds", false);
    // 0x0044f700 names its sound clones "<family><1..n>".
    for (const auto &[family, variants] :
         std::array<std::pair<std::string_view, int>, 8>{
             std::pair{"roof", 4}, std::pair{"rotten", 3},
             std::pair{"cart", 3}, std::pair{"ground", 2},
             std::pair{"splash", 4}, std::pair{"lifedown", 1},
             std::pair{"lifeup", 1}, std::pair{"empty", 1}}) {
        for (int variant = 1; variant <= variants; ++variant) {
            clip(std::string(family) + std::to_string(variant), false);
        }
    }

    // Static descriptors, with the fields `fgp_inspect.py animations` reads.
    game.start = staticAnim(0x004e6338U, "start", false, true, false, 353,
                            267, 3);
    game.birds = staticAnim(0x004e6290U, "birds", true, false, true, 0, 0, 0);
    game.basket = staticAnim(0x004e61e8U, "basket0", false, true, false, 54,
                             535, 0);
    game.pickup = staticAnim(0x004e5ff0U, "pickup", true, true, false, 0, 478,
                             0);
    game.pie = staticAnim(0x004e6140U, "pie", true, true, false, 312, 403, 0);
    game.putdown = staticAnim(0x004e6098U, "putdown", true, true, false, 0,
                              475, 0);
    game.cart = staticAnim(0x0068baf0U, "left_neutral0", false, true, false,
                           0, 0, 0);
    // 0x0044f040: twelve cart-count apples from (208,3), 30 pixels apart.
    for (std::size_t index = 0; index < game.counters.size(); ++index) {
        game.counters[index] = staticAnim(
            0x0068bbb0U + static_cast<std::uint32_t>(index) * 0xa8U, "apple",
            false, true, false, 208 + static_cast<int>(index) * 30, 3, 0);
    }
    // 0x0044f0b0: ten level pies from (732,546), stacked 10 pixels upward.
    for (std::size_t index = 0; index < game.pies.size(); ++index) {
        game.pies[index] = staticAnim(
            0x0068c398U + static_cast<std::uint32_t>(index) * 0xa8U, "pie",
            false, true, false, 732, 546 - static_cast<int>(index) * 10, 0);
    }
    for (std::size_t index = 0; index < game.apples.size(); ++index) {
        game.apples[index].anim.address =
            kAppleBase + static_cast<std::uint32_t>(index) * kAppleStride +
            0x1cU;
    }
    std::cout << "Loaded executable-derived Wheelbarrow: ten roof spawns, "
                 "55..578 cart bounds, 12-apple cart, 24 delivered apples "
                 "per level, from 0x0044ed50-0x00450310\n";
    return game;
}

void WheelbarrowScene::initializeWheelbarrowScene() {
    // Scene initializer 0x0044ee50, in its call order. It runs on the first
    // update so the trace follows scene_change, as GAME_EnterScene's
    // logpoint precedes the original initializer. The help/exit buttons and
    // the SCORE/LIFE text controls are unnamed descriptors.
    auto &game = *wheelbarrowGame;
    game.initialized = true;
    // 0x0044eeb0 and 0x0044eed2: the shared Help and Exit buttons.
    referenceTrace.emit("animation_add", {
        {"asset", "help_0"}, {"x", -1}, {"y", -1}, {"layer", 100},
        {"hotspot", 1}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {
        {"asset", "exit_0"}, {"x", -1}, {"y", -1}, {"layer", 100},
        {"hotspot", 2}, {"conflict_mask", 0}});
    // 0x0044efd0: `start` with its START text control.
    addWheelbarrowAnim(game.start, -1, -1, 100);
    // 0x0044ef82: the bird ambience runs for the whole scene.
    addAndPlayWheelbarrowAnim(game.birds);
}

std::vector<WheelbarrowScene::WheelbarrowAnim *>
WheelbarrowScene::wheelbarrowAnims() {
    std::vector<WheelbarrowAnim *> anims;
    if (!wheelbarrowGame) {
        return anims;
    }
    auto &game = *wheelbarrowGame;
    for (WheelbarrowAnim *anim :
         {&game.start, &game.birds, &game.basket, &game.pickup, &game.pie,
          &game.putdown, &game.cart}) {
        anims.push_back(anim);
    }
    for (auto &anim : game.counters) anims.push_back(&anim);
    for (auto &anim : game.pies) anims.push_back(&anim);
    for (auto &apple : game.apples) anims.push_back(&apple.anim);
    for (auto &anim : game.clones) anims.push_back(&anim);
    std::erase_if(anims, [](const WheelbarrowAnim *anim) {
        return !anim->active;
    });
    std::stable_sort(anims.begin(), anims.end(),
                     [](const WheelbarrowAnim *left,
                        const WheelbarrowAnim *right) {
                         return left->layer != right->layer
                                    ? left->layer < right->layer
                                    : left->order < right->order;
                     });
    return anims;
}

int WheelbarrowScene::wheelbarrowRandom(int bound, std::optional<int> pinned) {
    // 0x0040c530: rand() % bound. Reference captures pin each call site.
    if (bound <= 0) {
        return 0;
    }
    if (pinned) {
        return std::clamp(*pinned, 0, bound - 1);
    }
    return std::uniform_int_distribution<int>(0, bound - 1)(randomEngine);
}

void WheelbarrowScene::addWheelbarrowAnim(WheelbarrowAnim &anim, int x, int y,
                                          int layer) {
    // SCENE_AddAnim 0x00412840; -1 selects the descriptor's own position.
    auto &game = *wheelbarrowGame;
    anim.active = true;
    anim.playing = false;
    anim.frozenAt = 0.0;
    anim.x = x == -1 ? anim.defaultX : x;
    anim.y = y == -1 ? anim.defaultY : y;
    anim.layer = layer;
    anim.order = ++game.nextOrder;
    referenceTrace.emit("animation_add", {
        {"asset", anim.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", anim.hotspot}, {"conflict_mask", 0},
        {"descriptor", descriptorText(anim.address)}});
}

void WheelbarrowScene::stopWheelbarrowAnim(WheelbarrowAnim &anim) {
    // SCENE_StopAnim 0x00412a60 is logged on entry for every descriptor.
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "stopped"},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.smacker && anim.playing) {
        anim.frozenAt = wheelbarrowClock() - anim.playStarted;
        anim.playing = false;
    }
    if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void WheelbarrowScene::removeWheelbarrowAnim(WheelbarrowAnim &anim) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60,
    // so its logpoint precedes the nested StopAnim record.
    referenceTrace.emit("animation_remove", {
        {"asset", anim.name}, {"reason", "removed"},
        {"descriptor", descriptorText(anim.address)}});
    stopWheelbarrowAnim(anim);
    anim.active = false;
    anim.playing = false;
}

void WheelbarrowScene::playWheelbarrowAnim(WheelbarrowAnim &anim) {
    // SCENE_PlayAnim 0x004129d0 starts a Smacker descriptor's player.
    if (!anim.smacker || !wheelbarrowGame) {
        return;
    }
    const auto found = wheelbarrowGame->clips.find(anim.name);
    if (found == wheelbarrowGame->clips.end()) {
        return;
    }
    const bool finished =
        !anim.loop && anim.frozenAt >= found->second.completion;
    anim.playStarted = wheelbarrowClock() - (finished ? 0.0 : anim.frozenAt);
    anim.playing = true;
    if (!found->second.media.audioSamples.empty()) {
        if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(anim.audioHandle);
        }
        anim.audioHandle = audioMixer.addEffect(
            found->second.media, audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void WheelbarrowScene::addAndPlayWheelbarrowAnim(WheelbarrowAnim &anim) {
    // 0x00412b90 adds an inactive descriptor at its own position and layer.
    if (!anim.active) {
        addWheelbarrowAnim(anim, anim.defaultX, anim.defaultY,
                           anim.defaultLayer);
    }
    playWheelbarrowAnim(anim);
}

void WheelbarrowScene::finishWheelbarrowClip(WheelbarrowAnim &anim) {
    // 0x00413245-0x00413269: a finished non-looping player is stopped; a
    // clone is then removed, any other descriptor reports AnimationFinished.
    anim.frozenAt = wheelbarrowClock() - anim.playStarted;
    stopWheelbarrowAnim(anim);
    if (anim.clone) {
        removeWheelbarrowAnim(anim);
        return;
    }
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "finished"},
        {"descriptor", descriptorText(anim.address)}});
    finishWheelbarrowAnim(anim);
}

void WheelbarrowScene::playWheelbarrowSound(std::string_view family,
                                            int variants) {
    // 0x0044f700 names the invisible template 0x004e5f48 "<family><n>",
    // n = rand(variants) + 1, and plays a clone of it (0x00412c20).
    auto &game = *wheelbarrowGame;
    const int variant =
        wheelbarrowRandom(variants, game.pinnedVariant) + 1;
    std::uint32_t address = kCloneBase;
    while (std::any_of(game.clones.begin(), game.clones.end(),
                       [&](const WheelbarrowAnim &clone) {
                           return clone.active && clone.address == address;
                       })) {
        address += kCloneStride;
    }
    WheelbarrowAnim clone = staticAnim(
        address, std::string(family) + std::to_string(variant), true, false,
        false, 0, 0, 0);
    clone.clone = true;
    game.clones.push_back(std::move(clone));
    addAndPlayWheelbarrowAnim(game.clones.back());
}

void WheelbarrowScene::setWheelbarrowCartState(int state) {
    // 0x0044f300: the cart descriptor is renamed "<dir>_<state><fill>" and
    // re-added at the rounded cart position; Smacker states play at once.
    auto &game = *wheelbarrowGame;
    const int fill = wheelbarrowFill(game.cartApples);
    if (game.cart.active) {
        removeWheelbarrowAnim(game.cart);
    }
    game.cart.name = std::string(kDirections[static_cast<std::size_t>(
                         game.direction)]) + "_" +
                     std::string(kCartStates[static_cast<std::size_t>(state)]) +
                     std::to_string(fill);
    game.cart.smacker = state != kCartNeutral;
    game.cart.loop = state == kCartCycle;
    addWheelbarrowAnim(game.cart, roundAway(game.cartX), kCartY, 100);
    if (game.cart.smacker) {
        playWheelbarrowAnim(game.cart);
    }
    game.cartState = state;
}

void WheelbarrowScene::setWheelbarrowCounters(int count) {
    // 0x0044f440 shows the first `count` cart-count apples at layer 100.
    auto &game = *wheelbarrowGame;
    for (int index = 0; index < static_cast<int>(game.counters.size());
         ++index) {
        auto &anim = game.counters[static_cast<std::size_t>(index)];
        if (!anim.active && index < count) {
            addWheelbarrowAnim(anim, -1, -1, 100);
        } else if (anim.active && count <= index) {
            removeWheelbarrowAnim(anim);
        }
    }
}

void WheelbarrowScene::setWheelbarrowBasket(int delivered) {
    // 0x0044f280 always removes the basket; a non-negative count re-adds
    // basket<delivered * 4 / 24> at layer 50.
    auto &game = *wheelbarrowGame;
    if (game.basket.active) {
        removeWheelbarrowAnim(game.basket);
    }
    if (delivered < 0) {
        return;
    }
    const int index = std::clamp(delivered * 4 / kLevelApples, 0, 3);
    game.basket.name = "basket" + std::to_string(index);
    addWheelbarrowAnim(game.basket, -1, -1, 50);
}

void WheelbarrowScene::setWheelbarrowSpawnTimer(int period) {
    // 0x0044f550: cancel the spawn EVENT; a non-negative period re-arms it
    // with a first deadline 2000 ms from now (0x0044f57c).
    auto &game = *wheelbarrowGame;
    game.spawnTimer = period >= 0;
    if (game.spawnTimer) {
        game.nextSpawn = wheelbarrowClock() + 2.0;
        game.spawnPeriod = period / 1000.0;
    }
}

void WheelbarrowScene::setWheelbarrowLevel(int level) {
    // 0x0044f520: show one pie per completed level (0x0044f590, layer
    // index-100) and re-arm the spawn clock at 3750 - 300*level ms.
    auto &game = *wheelbarrowGame;
    for (int index = 0; index < static_cast<int>(game.pies.size()); ++index) {
        auto &anim = game.pies[static_cast<std::size_t>(index)];
        if (!anim.active && index < level) {
            addWheelbarrowAnim(anim, -1, -1, index - 100);
        } else if (anim.active && level <= index) {
            removeWheelbarrowAnim(anim);
        }
    }
    setWheelbarrowSpawnTimer(3750 - level * 300);
}

void WheelbarrowScene::addWheelbarrowScore(int points) {
    // 0x0044f8d0: one life for crossing a 3000-point boundary.
    auto &game = *wheelbarrowGame;
    const int previous = game.score;
    game.score += points;
    game.scoreText = std::to_string(game.score);
    if (previous / kLifeScore < game.score / kLifeScore) {
        ++game.lives;
        playWheelbarrowSound("lifeup", 1);
        game.livesText = std::to_string(game.lives);
    }
}

bool WheelbarrowScene::loseWheelbarrowLife() {
    // 0x004500d0: returns whether lives remain.
    auto &game = *wheelbarrowGame;
    --game.lives;
    playWheelbarrowSound("lifedown", 1);
    game.livesText = std::to_string(game.lives);
    referenceTrace.emit("diagnostic", {
        {"kind", "wheelbarrow_miss"}, {"lives", game.lives},
        {"score", game.score}});
    return game.lives > 0;
}

void WheelbarrowScene::removeWheelbarrowApple(WheelbarrowApple &apple) {
    // 0x0044f250.
    if (apple.anim.active) {
        removeWheelbarrowAnim(apple.anim);
    }
    apple.active = false;
}

void WheelbarrowScene::setWheelbarrowAppleMode(WheelbarrowApple &apple,
                                               int mode) {
    // 0x0044f980: re-add the object's descriptor as apple_<mode> or
    // rotten_<mode> at layer slot + 10 * stage (0x0044fac0). The falling
    // clip (mode 1) loops and is not started here; 0x00450078 starts it at
    // the first roof bounce.
    auto &game = *wheelbarrowGame;
    if (apple.anim.active) {
        removeWheelbarrowAnim(apple.anim);
    }
    apple.anim.loop = mode == 1;
    apple.anim.smacker = true;
    apple.anim.name = std::string(apple.type == 0 ? "apple_" : "rotten_") +
                      std::to_string(mode);
    const auto slot = static_cast<int>(&apple - game.apples.data());
    addWheelbarrowAnim(apple.anim, roundAway(apple.x), roundAway(apple.y),
                       slot + apple.stage * 10);
    if (mode != 1) {
        playWheelbarrowAnim(apple.anim);
    }
    apple.mode = mode;
}

void WheelbarrowScene::spawnWheelbarrowApple() {
    // 0x00450270, the spawn EVENT's handler: the first free of ten slots,
    // rand(10) for the roof triple, rand(100) < 10 for a rotten apple.
    auto &game = *wheelbarrowGame;
    const auto free = std::find_if(game.apples.begin(), game.apples.end(),
                                   [](const WheelbarrowApple &apple) {
                                       return !apple.active;
                                   });
    if (free == game.apples.end()) {
        return;
    }
    auto &apple = *free;
    const std::uint32_t address = apple.anim.address;
    apple = WheelbarrowApple{};
    apple.anim.address = address;
    apple.active = true;
    const auto point = kSpawnPoints[static_cast<std::size_t>(
        wheelbarrowRandom(10, game.pinnedSpawn))];
    apple.x = static_cast<float>(point.x);
    apple.y = static_cast<float>(point.y);
    apple.stage = point.route;
    ++game.chanceDraws;
    int chance = wheelbarrowRandom(100, game.pinnedChance);
    if (game.pinnedRottenDraw == game.chanceDraws) {
        chance = 0;
    }
    apple.type = chance < 10 ? 1 : 0;
    setWheelbarrowAppleMode(apple, 0);
}

void WheelbarrowScene::startWheelbarrowGame(int level) {
    // 0x0044f110.
    if (!wheelbarrowGame) {
        return;
    }
    auto &game = *wheelbarrowGame;
    if (!game.initialized) {
        initializeWheelbarrowScene();
    }
    if (game.start.active) {
        removeWheelbarrowAnim(game.start);
    }
    for (auto &apple : game.apples) {
        if (apple.active) {
            removeWheelbarrowApple(apple);
        }
    }
    if (game.cart.active) {
        removeWheelbarrowAnim(game.cart);
    }
    game.active = false;
    game.cartX = kCartStart;
    game.score = 0;
    game.speed = kCartSpeed;
    game.level = 0;
    game.delivered = 0;
    game.lives = 0;
    game.direction = 0;
    game.cartApples = 0;
    game.cartState = kCartNeutral;
    setWheelbarrowCartState(kCartNeutral);
    setWheelbarrowCounters(game.cartApples);
    game.active = true;
    game.level = level;
    game.lives = 3;
    game.scoreText = std::to_string(game.score);
    game.livesText = std::to_string(game.lives);
    setWheelbarrowLevel(game.level);
    setWheelbarrowBasket(game.delivered);
    game.lastTick = std::floor(wheelbarrowClock() * 1000.0);
}

bool WheelbarrowScene::handleWheelbarrowKey(SDL_Keycode key, bool pressed) {
    if (!wheelbarrowGame) {
        return false;
    }
    auto &game = *wheelbarrowGame;
    if (key == SDLK_LEFT) game.leftHeld = pressed;
    if (key == SDLK_RIGHT) game.rightHeld = pressed;
    if (key != SDLK_LEFT && key != SDLK_RIGHT && key != SDLK_UP) {
        return false;
    }
    // 0x0044f640 (key down) and 0x0044f730 (key up) ignore input while the
    // game is inactive or the cart is dumping.
    if (!game.active || game.gameOverModal || game.cartState == kCartDump) {
        return true;
    }
    if (!pressed) {
        const int released = key == SDLK_LEFT ? 0 : key == SDLK_RIGHT ? 1 : -1;
        if (released == game.direction) {
            setWheelbarrowCartState(kCartNeutral);
        }
        return true;
    }
    if (key == SDLK_UP) {
        // 0x0044f6b0: dump at the basket, facing left, with apples aboard.
        if (!(kDumpMaximum < game.cartX) && game.direction == 0 &&
            game.cartApples > 0 && game.basket.active) {
            playWheelbarrowSound("empty", 1);
            setWheelbarrowCartState(kCartDump);
        }
        return true;
    }
    const int direction = key == SDLK_LEFT ? 0 : 1;
    if (game.cartState == kCartCycle && game.direction == direction) {
        return true;
    }
    game.direction = direction;
    setWheelbarrowCartState(kCartCycle);
    return true;
}

void WheelbarrowScene::resumeWheelbarrowCart() {
    // 0x0044f870: after a dump, follow whichever arrow key is held.
    auto &game = *wheelbarrowGame;
    if (game.rightHeld) {
        game.direction = 1;
        setWheelbarrowCartState(kCartCycle);
    } else if (game.leftHeld) {
        game.direction = 0;
        setWheelbarrowCartState(kCartCycle);
    } else {
        setWheelbarrowCartState(kCartNeutral);
    }
}

void WheelbarrowScene::completeWheelbarrowDump() {
    // 0x0044faf0 reads the cart count when the dump clip finishes, so an
    // apple caught (or a rotten splash) during the clip changes the
    // delivery. Game over clears the active flag first.
    auto &game = *wheelbarrowGame;
    if (!game.active) {
        return;
    }
    game.delivered += game.cartApples;
    addWheelbarrowScore(game.cartApples * 20);
    game.cartApples = 0;
    setWheelbarrowCounters(0);
    referenceTrace.emit("diagnostic", {
        {"kind", "wheelbarrow_dump_complete"}, {"score", game.score},
        {"delivered", game.delivered}});
    if (game.delivered < kLevelApples) {
        setWheelbarrowBasket(game.delivered);
        return;
    }
    // 0x0044fb45: stop spawning, lift the basket and start the pie.
    setWheelbarrowSpawnTimer(-1);
    game.delivered = 0;
    setWheelbarrowBasket(-1);
    addAndPlayWheelbarrowAnim(game.pickup);
}

void WheelbarrowScene::finishWheelbarrowAnim(WheelbarrowAnim &anim) {
    // The animation-finished dispatcher 0x0044f780.
    auto &game = *wheelbarrowGame;
    for (auto &apple : game.apples) {
        if (!apple.active || &apple.anim != &anim) {
            continue;
        }
        // 0x0044f950: ripening ends in the falling clip; a splash removes
        // the object.
        if (apple.mode == 0) {
            setWheelbarrowAppleMode(apple, 1);
        } else if (apple.mode == 2) {
            removeWheelbarrowApple(apple);
        }
        return;
    }
    if (&anim == &game.cart) {
        if (game.cartState == kCartDump) {
            completeWheelbarrowDump();
        }
        if (game.cartState >= 2) {
            resumeWheelbarrowCart();
        }
    } else if (&anim == &game.pickup) {
        removeWheelbarrowAnim(game.pickup);
        addAndPlayWheelbarrowAnim(game.pie);
    } else if (&anim == &game.pie) {
        removeWheelbarrowAnim(game.pie);
        if (game.active) {
            ++game.level;
            setWheelbarrowLevel(game.level);
            addWheelbarrowScore(500);
            addAndPlayWheelbarrowAnim(game.putdown);
            referenceTrace.emit("diagnostic", {
                {"kind", "wheelbarrow_level_complete"},
                {"level", game.level + 1}, {"score", game.score}});
        }
    } else if (&anim == &game.putdown) {
        removeWheelbarrowAnim(game.putdown);
        setWheelbarrowBasket(0);
    }
}

bool WheelbarrowScene::moveWheelbarrowApple(WheelbarrowApple &apple,
                                            float dt) {
    // 0x0044ff80. Returns false only when a missed good apple took the
    // last life.
    auto &game = *wheelbarrowGame;
    const float y = apple.vy * dt + apple.y;
    apple.y = y;
    apple.vy = kAppleGravity * dt + apple.vy;
    if (kAppleFloor < y) {
        removeWheelbarrowApple(apple);
        return true;
    }
    apple.anim.x = roundAway(apple.x);
    apple.anim.y = roundAway(y);
    if (apple.stage >= static_cast<int>(kRoofStages.size())) {
        return true;
    }
    const auto stage = kRoofStages[static_cast<std::size_t>(apple.stage)];
    if (static_cast<float>(stage.y) > y) {
        return true;
    }
    // 0x00450110: roof or rotten thuds on the roofs, ground or splash on
    // the final stage; selector 2 is silent.
    if (stage.restitution <= 1) {
        apple.type == 0 ? playWheelbarrowSound("roof", 4)
                        : playWheelbarrowSound("rotten", 3);
    } else if (stage.restitution == 3) {
        apple.type == 0 ? playWheelbarrowSound("ground", 2)
                        : playWheelbarrowSound("splash", 4);
    }
    apple.vy = -(kRestitution[static_cast<std::size_t>(stage.restitution)] *
                 apple.vy);
    ++apple.stage;
    const auto slot = static_cast<int>(&apple - game.apples.data());
    apple.anim.layer = slot + apple.stage * 10; // 0x00412d50
    if (!apple.anim.playing) {
        playWheelbarrowAnim(apple.anim);
    }
    if (stage.restitution != 3) {
        return true;
    }
    if (apple.type == 1) {
        setWheelbarrowAppleMode(apple, 2);
        return true;
    }
    return loseWheelbarrowLife();
}

bool WheelbarrowScene::updateWheelbarrowApple(WheelbarrowApple &apple,
                                              const SDL_Rect &cart,
                                              float dt) {
    // 0x0044fe50: physics runs only in mode 1; the swept rectangle from
    // before and after the move is tested against the cart only while the
    // apple is on its last stage before the ground (stage 4).
    auto &game = *wheelbarrowGame;
    if (apple.mode != 1) {
        return true;
    }
    const auto clip = game.clips.find(apple.anim.name);
    const int width = clip == game.clips.end() ||
                              clip->second.media.videoFrames.empty()
        ? 0 : clip->second.media.videoFrames.front().width;
    const int height = clip == game.clips.end() ||
                               clip->second.media.videoFrames.empty()
        ? 0 : clip->second.media.videoFrames.front().height;
    const SDL_Rect before{apple.anim.x, apple.anim.y, width, height};
    if (!moveWheelbarrowApple(apple, dt)) {
        return false;
    }
    if (apple.stage != 4 || !apple.active) {
        return true;
    }
    const SDL_Rect after{apple.anim.x, apple.anim.y, width, height};
    SDL_Rect swept{};
    SDL_UnionRect(&before, &after, &swept);
    if (!SDL_HasIntersection(&swept, &cart)) {
        return true;
    }
    const int fill = wheelbarrowFill(game.cartApples);
    if (apple.type == 0) {
        if (game.cartApples >= kCartCapacity) {
            return true;
        }
        playWheelbarrowSound("cart", 3);
        ++game.cartApples;
        // 0x0044ff13: (level * 0.2 + 1) * 10 points.
        addWheelbarrowScore(static_cast<int>(
            (static_cast<float>(game.level) * 0.2F + 1.0F) * 10.0F));
        referenceTrace.emit("diagnostic", {
            {"kind", "wheelbarrow_catch"}, {"score", game.score},
            {"cart_apples", game.cartApples}});
    } else {
        playWheelbarrowSound("splash", 4);
        game.cartApples = 0;
        referenceTrace.emit("diagnostic", {
            {"kind", "wheelbarrow_rotten_catch"}, {"score", game.score}});
    }
    setWheelbarrowCounters(game.cartApples);
    removeWheelbarrowApple(apple);
    if (wheelbarrowFill(game.cartApples) != fill) {
        setWheelbarrowCartState(game.cartState);
    }
    return true;
}

void WheelbarrowScene::tickWheelbarrow(double now) {
    // 0x0044fb80: one update at millisecond `now`. 0x0040c450 is a
    // millisecond clock; the delta is scaled by 0.001 (0x0049d9f0) and
    // capped at 0.1 s (0x0049c908).
    auto &game = *wheelbarrowGame;
    float dt = static_cast<float>(now - game.lastTick) * 0.001F;
    game.lastTick = now;
    if (!(dt < kTickCap)) {
        dt = kTickCap;
    }
    // 0x0044fbe0: only the cycling cart moves.
    if (game.cartState == kCartCycle) {
        const float step = game.speed * dt;
        game.cartX = game.direction == 0 ? game.cartX - step
                                         : game.cartX + step;
        if (game.cartX < kCartMinimum) {
            game.cartX = kCartMinimum;
        } else if (kCartMaximum < game.cartX) {
            game.cartX = kCartMaximum;
        }
        game.cart.x = roundAway(game.cartX);
    }
    // 0x0044fcc0.
    SDL_Rect cart = kCatchRects[static_cast<std::size_t>(game.direction)];
    cart.x += roundAway(game.cartX);
    cart.y += kCartY;
    for (auto &apple : game.apples) {
        if (apple.active && !updateWheelbarrowApple(apple, cart, dt)) {
            wheelbarrowGameOver();
            return;
        }
    }
}

void WheelbarrowScene::wheelbarrowGameOver() {
    // 0x00450180, up to the blocking GAMEOVER message at 0x00450217.
    auto &game = *wheelbarrowGame;
    game.active = false;
    setWheelbarrowSpawnTimer(-1);
    setWheelbarrowCartState(kCartNeutral);
    for (WheelbarrowAnim *anim : {&game.pie, &game.pickup, &game.putdown}) {
        if (anim->active) {
            removeWheelbarrowAnim(*anim);
        }
    }
    referenceTrace.emit("diagnostic", {
        {"kind", "wheelbarrow_game_over"}, {"score", game.score}});
    game.gameOverModal = true;
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt!"),
        language.get("wheelbarrow", "GAMEOVER", "Spillet er slutt!"),
        MessageKind::WheelbarrowGameOver);
}

void WheelbarrowScene::finishWheelbarrowGameOver() {
    // 0x00450242-0x00450250, after the score wrapper returns: restore
    // `start` and clear the falling apples.
    if (!wheelbarrowGame) {
        return;
    }
    auto &game = *wheelbarrowGame;
    game.gameOverModal = false;
    if (!game.start.active) {
        addWheelbarrowAnim(game.start, -1, -1, 100);
    }
    for (auto &apple : game.apples) {
        if (apple.active) {
            removeWheelbarrowApple(apple);
        }
    }
}

double WheelbarrowScene::wheelbarrowClock() {
    // Inside the millisecond loop the game runs at the simulated pass time,
    // otherwise at scene time.
    return wheelbarrowGame && wheelbarrowGame->simulatedClock
        ? *wheelbarrowGame->simulatedClock : sceneElapsedNow();
}

void WheelbarrowScene::updateWheelbarrowDescriptors(double now) {
    // The descriptor update loop 0x00413200 finishes one-shot players.
    auto &game = *wheelbarrowGame;
    std::vector<WheelbarrowAnim *> finished;
    for (WheelbarrowAnim *anim : wheelbarrowAnims()) {
        if (!anim->smacker || !anim->playing) {
            continue;
        }
        const auto found = game.clips.find(anim->name);
        if (found == game.clips.end()) {
            continue;
        }
        const double played = now - anim->playStarted;
        if (!anim->loop && played >= found->second.completion) {
            finished.push_back(anim);
        } else if (anim->loop && found->second.media.videoDuration > 0.0 &&
                   !found->second.media.audioSamples.empty() &&
                   played >= found->second.media.videoDuration) {
            // Restart a looping clip's sound with each pass.
            anim->playStarted += found->second.media.videoDuration *
                std::floor(played / found->second.media.videoDuration);
            if (anim->audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
                audioMixer.removeEffect(anim->audioHandle);
            }
            anim->audioHandle = audioMixer.addEffect(
                found->second.media, audioVolumes[0] * sceneAudioGains[0]);
        }
    }
    for (WheelbarrowAnim *anim : finished) {
        if (game.gameOverModal) {
            break;
        }
        if (anim->active && anim->playing) {
            finishWheelbarrowClip(*anim);
        }
    }
    std::erase_if(game.clones, [](const WheelbarrowAnim &clone) {
        return !clone.active;
    });
}

void WheelbarrowScene::updateWheelbarrow(double elapsed) {
    if (!wheelbarrowGame) {
        return;
    }
    auto &game = *wheelbarrowGame;
    if (!game.initialized) {
        initializeWheelbarrowScene();
    }
    // The GAMEOVER message (0x00450217) and the score wrapper (0x0045023a)
    // block the original's main loop: nothing moves or finishes meanwhile.
    if (game.gameOverModal) {
        return;
    }
    // The original's main loop passes about once per millisecond in the
    // reference captures. Each pass posts the 0x112 tick, services the spawn
    // EVENT and runs the descriptor update. The tick integrates the apples
    // with explicit Euler steps, so the pace shows in the physics: an apple
    // from roof spawn 0 bounces 593, 1359 and 1968 ms after apple_1 appears
    // and lands at 3156 ms, which is this integrator at 1 ms steps (16 ms
    // steps give 617, 1400, 2033 and 3250 ms). Native renders far less
    // often, so it replays the elapsed milliseconds one pass at a time; a
    // stall longer than the tick cap is one capped tick, as in the original.
    const double now = std::floor(elapsed * 1000.0);
    if (!game.active) {
        updateWheelbarrowDescriptors(elapsed);
        return;
    }
    if (now - game.lastTick > kTickCap * 1000.0) {
        game.simulatedClock = now / 1000.0;
        tickWheelbarrow(now);
    }
    while (game.lastTick < now && !game.gameOverModal) {
        const double pass = game.lastTick + 1.0;
        game.simulatedClock = pass / 1000.0;
        // The spawn EVENT keeps its phase and fires once per service pass.
        if (game.active && game.spawnTimer &&
            *game.simulatedClock >= game.nextSpawn) {
            game.nextSpawn += game.spawnPeriod;
            spawnWheelbarrowApple();
        }
        if (game.active) {
            tickWheelbarrow(pass);
        } else {
            game.lastTick = pass;
        }
        if (!game.gameOverModal) {
            updateWheelbarrowDescriptors(*game.simulatedClock);
        }
    }
    game.simulatedClock.reset();
}

void WheelbarrowScene::renderWheelbarrow(double elapsed) {
    if (!wheelbarrowGame) {
        return;
    }
    auto &game = *wheelbarrowGame;
    for (WheelbarrowAnim *anim : wheelbarrowAnims()) {
        if (!anim->visible) {
            continue;
        }
        if (!anim->smacker) {
            const auto found = game.images.find(anim->name);
            if (found == game.images.end()) {
                continue;
            }
            const SDL_Rect target{anim->x, anim->y, found->second.width,
                                  found->second.height};
            SDL_RenderCopy(renderer, found->second.texture.get(), nullptr,
                           &target);
            continue;
        }
        const auto found = game.clips.find(anim->name);
        if (found == game.clips.end() || !found->second.texture ||
            found->second.media.videoFrames.empty()) {
            continue;
        }
        const double time = anim->playing ? elapsed - anim->playStarted
                                          : anim->frozenAt;
        const auto index = frameIndexAt(found->second.media, time, anim->loop);
        const auto &frame = found->second.media.videoFrames[index];
        uploadVideoFrame(found->second.texture.get(), frame);
        const SDL_Rect target{anim->x, anim->y, frame.width, frame.height};
        SDL_RenderCopy(renderer, found->second.texture.get(), nullptr,
                       &target);
    }
    // Text controls (layer 200 and the START label at 101): 0x0044eeda-
    // 0x0044ef58 rectangles from 0x0049f118, START at 0x0044eff4.
    drawCenteredText(renderer, calendarFont,
                     language.get("wheelbarrow", "SCORE", "Poeng"),
                     SDL_Rect{6,85,79,25});
    drawCenteredText(renderer, calendarFont,
                     language.get("wheelbarrow", "LIFE", "Forsøk"),
                     SDL_Rect{715,85,80,25});
    drawCenteredText(renderer, calendarFont, game.scoreText,
                     SDL_Rect{6,110,79,25});
    drawCenteredText(renderer, calendarFont, game.livesText,
                     SDL_Rect{715,110,80,25});
    if (game.start.active) {
        drawCenteredText(renderer, calendarFont,
                         language.get("wheelbarrow", "START", "Start"),
                         SDL_Rect{356,296,89,33});
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
WheelbarrowScene::wheelbarrowHotspotAt(int x, int y) const {
    // `start` carries hotspot 3 (0x004e6388); 0x0044f630 starts on it.
    if (!wheelbarrowGame || activeScene != "wheelbarrow" ||
        !wheelbarrowGame->start.active || wheelbarrowGame->gameOverModal) {
        return std::nullopt;
    }
    const auto &start = wheelbarrowGame->start;
    const auto found = wheelbarrowGame->images.find("start");
    const SDL_Rect bounds{start.x, start.y,
                          found == wheelbarrowGame->images.end()
                              ? 0 : found->second.width,
                          found == wheelbarrowGame->images.end()
                              ? 0 : found->second.height};
    const SDL_Point point{x, y};
    if (SDL_PointInRect(&point, &bounds)) {
        return std::pair{kStartHotspot, bounds};
    }
    return std::nullopt;
}

bool WheelbarrowScene::executeWheelbarrowDeveloperCommand(
    std::string_view command) {
    constexpr std::string_view wheelbarrowControlPrefix = "wheelbarrow:";
    if (activeScene != "wheelbarrow" || !wheelbarrowGame) {
        return false;
    }
    auto &game = *wheelbarrowGame;
    const auto control = command.substr(wheelbarrowControlPrefix.size());
    if (control == "start") {
        startWheelbarrowGame();
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 1) return false;
        startWheelbarrowGame(level - 1);
    } else if (control == "left" || control == "right" || control == "up") {
        handleWheelbarrowKey(control == "left" ? SDLK_LEFT
                             : control == "right" ? SDLK_RIGHT : SDLK_UP);
    } else if (control == "stop") {
        handleWheelbarrowKey(game.direction == 0 ? SDLK_LEFT : SDLK_RIGHT,
                             false);
    } else if (control.rfind("spawn:", 0) == 0) {
        // Spawn now from roof triple N (0x00450270 with rand(10) = N).
        const int spawn = std::stoi(std::string(control.substr(6)));
        if (spawn < 0 || spawn > 9) return false;
        if (!game.active) startWheelbarrowGame();
        const auto pinned = game.pinnedSpawn;
        game.pinnedSpawn = spawn;
        spawnWheelbarrowApple();
        game.pinnedSpawn = pinned;
    } else if (control.rfind("random=", 0) == 0) {
        // random=SPAWN,CHANCE,VARIANT[,ROTTEN_DRAW] pins the rand(n) results
        // the original returns at 0x0045029f, 0x004502cd and 0x0044f70a; the
        // ROTTEN_DRAW-th chance draw (1-based) returns 0 instead.
        std::vector<int> values;
        std::stringstream fields{std::string(control.substr(7))};
        for (std::string field; std::getline(fields, field, ',');) {
            values.push_back(std::stoi(field));
        }
        if (values.size() < 3 || values.size() > 4) return false;
        game.pinnedSpawn = values[0];
        game.pinnedChance = values[1];
        game.pinnedVariant = values[2];
        game.pinnedRottenDraw = values.size() == 4 ? values[3] : 0;
    } else if (control.rfind("wait=", 0) == 0) {
        // Hold the developer command queue for SECONDS of scene time.
        const double now = sceneElapsedNow();
        if (!wheelbarrowWaitUntil) {
            wheelbarrowWaitUntil = now + std::stod(std::string(control.substr(5)));
        }
        if (now < *wheelbarrowWaitUntil) {
            return false;
        }
        wheelbarrowWaitUntil.reset();
    } else if (control == "dump") {
        game.cartX = kCartMinimum;
        game.direction = 0;
        game.cartApples = std::max(1, game.cartApples);
        handleWheelbarrowKey(SDLK_UP);
    } else if (control == "prepare-completion" || control == "finish") {
        if (!game.active) startWheelbarrowGame();
        game.delivered = kLevelApples - 1;
        game.cartApples = 1;
        game.cartX = kCartMinimum;
        game.direction = 0;
        setWheelbarrowCartState(kCartNeutral);
        if (control == "finish") handleWheelbarrowKey(SDLK_UP);
    } else if (control == "status") {
        std::cout << "Wheelbarrow status: level " << game.level + 1
                  << ", score " << game.score << ", lives " << game.lives
                  << ", cart " << game.cartApples << "/" << kCartCapacity
                  << ", delivered " << game.delivered << "/" << kLevelApples
                  << ", x " << game.cartX << ", state " << game.cartState
                  << ", apples "
                  << std::count_if(game.apples.begin(), game.apples.end(),
                                   [](const WheelbarrowApple &apple) {
                                       return apple.active;
                                   })
                  << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Wheelbarrow control: " + std::string(control));
    }
    return true;
}

} // namespace fgp
