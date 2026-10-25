#include "game/app_mathlab.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#include "media/smacker.h"

// Math Lab is a port of FGP.exe 0x0047fe50-0x00482230. Every rule below names
// the address it was read from; REVERSE_ENGINEERING.md ("Math Lab") has the
// complete account. The game is driven by original 0xa8-byte animation
// descriptors, text controls (0x00407930), progress bars (0x004089a0) and the
// shared push/radio buttons (0x00406540), so the native port keeps one
// MathlabAnim per descriptor and mirrors each SCENE_AddAnim (0x00412840),
// SCENE_RemoveAnim (0x00412aa0) and SCENE_StopAnim (0x00412a60) call in the
// reference trace.

namespace fgp {
namespace {

// 0x005120b0: 23 rows of nine dwords per level: five tank bases, the random
// range added to each base (0x004806ab), the ball range (0x004802d0), the
// goal multiplier (0x004806fa) and the seconds added per level (0x0048065d).
struct MathlabLevel {
    std::array<int, 5> bases;
    int spread;
    int balls;
    int multiplier;
    int seconds;
};
const auto kLevels = originalArray<MathlabLevel, 23>(0x005120b0U);
// 0x0048001e-0x00480032: the first deadline by difficulty.
constexpr std::array<int, 3> kStartMilliseconds{{100000, 60000, 30000}};
// 0x00513630: the tap's frame over each tank; 0x00513638 is its rest frame.
const auto kTapFrames = originalArray<int, 5>(0x00513630U);
constexpr int kTapRestFrame = 11;
// 0x004a03e8: extra points per tank by pours (capped at 3, 0x0048206f).
const auto kPourFactors = originalArray<float, 4>(0x004a03e8U);
// 0x0051279c: the exact-tank bonus factor by the number of exact tanks.
const auto kExactFactors = originalArray<float, 6>(0x0051279cU);
constexpr float kFanVolume = 0.4F;      // 0x0049f5e8
constexpr float kEndballVolume = 0.65F; // 0x0049d9c4
constexpr int kRackFront = 0x24;        // 0x00481aaf
constexpr int kRackDrop = 0x32;         // 0x00481acb, 0x00481bce
constexpr int kRollMilliseconds = 0x42; // 0x00481b75: one frame per 66 ms
constexpr int kTapMilliseconds = 50;    // 0x00481d77: 0x51eb851f >> 4
constexpr int kBlinkMilliseconds = 300; // 0x00481ef4: 0x1b4e81b5 >> 5
constexpr std::uint32_t kCloneBase = 0x0054fea8U; // 0x00412c60
constexpr std::uint32_t kCloneStride = 0xa8U;

// Fonts loaded at 0x004809d7-0x00480a1d, in the order the code uses them.
enum MathlabFont { kRed14, kRed16, kBlack16, kYellow14, kGreen14 };
constexpr std::array<const char *, 5> kFontNames{{
    "Amerigo BT_14_red", "Amerigo BT_16_red", "Amerigo BT_16_black",
    "Amerigo BT_14_yellow", "Amerigo BT_14_green"}};

// Text rectangles (left, top, right, bottom) at 0x004a0408-0x004a04d8.
constexpr SDL_Rect rectFromEdges(int left, int top, int right, int bottom) {
    return SDL_Rect{left, top, right - left, bottom - top};
}
constexpr SDL_Rect kScoreRect = rectFromEdges(698, 212, 765, 229);
constexpr SDL_Rect kSpareRect = rectFromEdges(699, 207, 768, 229);
constexpr SDL_Rect kLevelRect = rectFromEdges(723, 189, 746, 204);
constexpr std::array<SDL_Rect, 5> kValueRects{{
    rectFromEdges(160, 448, 208, 466), rectFromEdges(240, 481, 289, 504),
    rectFromEdges(328, 491, 381, 513), rectFromEdges(460, 486, 511, 506),
    rectFromEdges(545, 438, 590, 458)}};
constexpr std::array<SDL_Rect, 5> kCapacityRects{{
    rectFromEdges(161, 479, 208, 497), rectFromEdges(239, 517, 290, 539),
    rectFromEdges(328, 526, 381, 547), rectFromEdges(458, 516, 510, 537),
    rectFromEdges(543, 469, 588, 488)}};
constexpr SDL_Rect kClockRect = rectFromEdges(318, 33, 407, 65);
// 0x004a04e8: the six operand digit columns (two per chamber), rows
// 0xd7-0xe8 at 0x00480c89; 0x004a0500: the four result digit columns, rows
// 0x141-0x152 at 0x00480cde.
const auto kOperandDigitX = originalArray<int, 6>(0x004a04e8U);
const auto kResultDigitX = originalArray<int, 4>(0x004a0500U);
// 0x00480a65-0x00480b15: PROGRESS_Create(x, y, 15, bitmap, 0, 0x10).
struct MathlabBar {
    const char *image;
    int x;
    int y;
};
constexpr MathlabBar kLevelBar{"tank_fluidindicator", 0x2a1, 0x6c};
constexpr std::array<MathlabBar, 5> kTankBars{{
    {"fluid_0", 0xd9, 0x1c0}, {"fluid_1", 0x12d, 0x1db},
    {"fluid_2", 0x18a, 0x1e6}, {"fluid_3", 0x1b8, 0x1e1},
    {"fluid_4", 0x211, 0x1b4}}};

// The C runtime qsort FGP.exe links at 0x0048d931: partitions above eight
// elements around the middle element (swapped to the front), selection-sorts
// smaller ones from the back (0x0048da85). It is not stable, so equal keys
// keep the order this algorithm leaves them in.
template <typename T, typename Compare>
void originalQsort(std::vector<T> &items, Compare compare) {
    if (items.size() < 2) {
        return;
    }
    std::array<std::ptrdiff_t, 30> lowStack{};
    std::array<std::ptrdiff_t, 30> highStack{};
    int stack = 0;
    std::ptrdiff_t lo = 0;
    std::ptrdiff_t hi = static_cast<std::ptrdiff_t>(items.size()) - 1;
    const auto at = [&](std::ptrdiff_t index) -> T & {
        return items[static_cast<std::size_t>(index)];
    };
    for (;;) {
        const std::ptrdiff_t size = hi - lo + 1;
        if (size <= 8) {
            for (std::ptrdiff_t top = hi; top > lo; --top) {
                std::ptrdiff_t max = lo;
                for (std::ptrdiff_t p = lo + 1; p <= top; ++p) {
                    if (compare(at(p), at(max)) > 0) {
                        max = p;
                    }
                }
                std::swap(at(max), at(top));
            }
        } else {
            std::swap(at(lo + size / 2), at(lo));
            std::ptrdiff_t loGuy = lo;
            std::ptrdiff_t hiGuy = hi + 1;
            for (;;) {
                do {
                    ++loGuy;
                } while (loGuy <= hi && compare(at(loGuy), at(lo)) <= 0);
                do {
                    --hiGuy;
                } while (hiGuy > lo && compare(at(hiGuy), at(lo)) >= 0);
                if (hiGuy < loGuy) {
                    break;
                }
                std::swap(at(loGuy), at(hiGuy));
            }
            std::swap(at(lo), at(hiGuy));
            if (hiGuy - 1 - lo >= hi - loGuy) {
                if (lo + 1 < hiGuy) {
                    lowStack[static_cast<std::size_t>(stack)] = lo;
                    highStack[static_cast<std::size_t>(stack)] = hiGuy - 1;
                    ++stack;
                }
                if (loGuy < hi) {
                    lo = loGuy;
                    continue;
                }
            } else {
                if (loGuy < hi) {
                    lowStack[static_cast<std::size_t>(stack)] = loGuy;
                    highStack[static_cast<std::size_t>(stack)] = hi;
                    ++stack;
                }
                if (lo + 1 < hiGuy) {
                    hi = hiGuy - 1;
                    continue;
                }
            }
        }
        if (--stack < 0) {
            return;
        }
        lo = lowStack[static_cast<std::size_t>(stack)];
        hi = highStack[static_cast<std::size_t>(stack)];
    }
}

std::string descriptorText(std::uint32_t address) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

std::string twoDigits(const char *format, int value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

} // namespace

MathlabScene::MathlabGame MathlabScene::loadMathlabGame(Package &package) {
    MathlabGame game;
    game.package = &package;
    const auto sceneAsset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {sceneLanguagePackage.get(),
                                   sceneAddonPackage.get(),
                                   sceneData2Package.get(),
                                   sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name)) {
                return candidate->data(*entry);
            }
        }
        throw std::runtime_error("Math Lab asset not found: " +
                                 std::string(name));
    };
    const auto commonAsset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {addonCommon.get(), data2Common.get(),
                                   editionCommon.get(), &common}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name)) {
                return candidate->data(*entry);
            }
        }
        throw std::runtime_error("Common asset not found: " +
                                 std::string(name));
    };
    const auto image = [&](const std::string &name) {
        auto &result = game.images[name];
        result.surface = loadBmp(sceneAsset("bitmap\\" + name + ".bmp"));
        result.width = result.surface->w;
        result.height = result.surface->h;
        result.key = SDL_MapRGB(result.surface->format, 0, 255, 0);
        // Math Lab's keyed bitmaps use pure green, the colour the progress
        // surfaces are cleared to (0x00408b48).
        SurfacePtr keyed = loadBmp(sceneAsset("bitmap\\" + name + ".bmp"));
        SDL_SetColorKey(keyed.get(), SDL_TRUE,
                        SDL_MapRGB(keyed->format, 0, 255, 0));
        result.texture = makeTexture(renderer, keyed.get());
        SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_BLEND);
    };
    for (const char *name :
         {"start", "labels", "disable2", "ok_button", "plus_1", "minus_1",
          "multiply_1", "plus_2", "minus_2", "multiply_2",
          "tank_fluidindicator", "fluid_0", "fluid_1", "fluid_2", "fluid_3",
          "fluid_4"}) {
        image(name);
    }
    for (int value = 0; value <= 20; ++value) {
        image(value == 0 ? "ball_0" : twoDigits("ball_%02d", value));
    }
    if (const auto *entry = package.findOptional("hotspots.bmp")) {
        game.images["hotspots"].surface = loadBmp(package.data(*entry));
    }
    // 0x00401000 loads each font from the layered common package.
    for (std::size_t index = 0; index < kFontNames.size(); ++index) {
        game.fonts[index] = loadBitmapFont(
            renderer, commonAsset(std::string("fonts\\") + kFontNames[index] +
                                  ".bmp"));
    }
    for (const char *name :
         {"fan", "new fan long1", "fan fade LONG", "new_pump", "ballopener",
          "ballcloser", "lightbulb", "dumpball", "flush", "overflow", "balls",
          "startballs", "endballs", "newball", "balls1", "endball1", "tap",
          "water1", "water2", "water3", "water4", "water5"}) {
        loadMathlabClip(game.clips, name);
    }

    // Static descriptors, with the fields `fgp_inspect.py animations` reads.
    const auto bitmapAnim = [](std::uint32_t address, std::string name, int x,
                               int y, std::uint32_t hotspot) {
        MathlabAnim anim;
        anim.address = address;
        anim.name = std::move(name);
        anim.defaultX = x;
        anim.defaultY = y;
        anim.hotspot = hotspot;
        return anim;
    };
    const auto smackerAnim = [](std::uint32_t address, std::string name, int x,
                                int y, bool visible, bool loop) {
        MathlabAnim anim;
        anim.address = address;
        anim.kind = MathlabAnimKind::Smacker;
        anim.name = std::move(name);
        anim.defaultX = x;
        anim.defaultY = y;
        anim.visible = visible;
        anim.loop = loop;
        return anim;
    };
    game.helpButton = bitmapAnim(0x005336a4U, "help_0", 0, 0, 1);
    game.helpButton.shellDrawn = true;
    game.exitButton = bitmapAnim(0x0053391cU, "exit_0", 0x2d8, 0, 2);
    game.exitButton.shellDrawn = true;
    game.ballDrag = bitmapAnim(0x00512868U, "ball_0", 0, 0, 0);
    game.labels = bitmapAnim(0x00512910U, "labels", 161, 445, 0);
    game.disable2 = bitmapAnim(0x005129b8U, "disable2", 389, 170, 0);
    game.start = bitmapAnim(0x00512a60U, "start", 307, 37, 3);
    game.fan = smackerAnim(0x00512b08U, "fan", 60, 355, true, true);
    game.fanLong = smackerAnim(0x00512bb0U, "new fan long1", 0, 0, false, true);
    game.fanFade = smackerAnim(0x00512c58U, "fan fade LONG", 0, 0, false, false);
    game.pump = smackerAnim(0x00512d00U, "new_pump", 714, 461, true, false);
    game.opener = smackerAnim(0x00512da8U, "ballopener", 182, 24, true, false);
    game.closer = smackerAnim(0x00512e50U, "ballcloser", 182, 24, true, false);
    game.lightbulb = smackerAnim(0x00512fa0U, "lightbulb", 532, 3, true, false);
    game.dumpball = smackerAnim(0x00513048U, "dumpball", 150, 126, true, false);
    game.flush = smackerAnim(0x005130f0U, "flush", 390, 517, true, false);
    game.overflow = smackerAnim(0x00513198U, "overflow", 185, 520, true, false);
    game.balls = smackerAnim(0x00513240U, "balls", 0, 128, true, false);
    game.startballs =
        smackerAnim(0x005132e8U, "startballs", 0, 127, true, false);
    game.endballs = smackerAnim(0x00513390U, "endballs", 0, 127, true, false);
    game.newball = smackerAnim(0x00513438U, "newball", 42, 87, true, false);
    game.ballsSound = smackerAnim(0x005134e0U, "balls1", 0, 0, false, true);
    game.endballSound =
        smackerAnim(0x00513588U, "endball1", 0, 0, false, false);
    game.tap = smackerAnim(0x00513648U, "tap", 188, 345, true, false);
    // 0x005136f0 onwards: tank 0 (hotspot 20, leftmost) fills through water5.
    const auto waterPositions = originalArray<SDL_Point, 5>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x00513744U + static_cast<std::uint32_t>(index * 168);
    return source.read<SDL_Point>(address);
});
    for (std::size_t tank = 0; tank < 5; ++tank) {
        game.water[tank] = smackerAnim(
            0x005136f0U + static_cast<std::uint32_t>(tank) * 0xa8U,
            "water" + std::to_string(5 - tank), waterPositions[tank].x,
            waterPositions[tank].y, true, false);
    }
    // 0x00480d70: thirty ball descriptors from the template at 0x00512ef8
    // (a visible keyed Smacker at (166,55)) with hotspots 200-229.
    for (std::size_t index = 0; index < game.ballAnims.size(); ++index) {
        auto &ball = game.ballAnims[index];
        ball = smackerAnim(
            0x006dbb28U + static_cast<std::uint32_t>(index) * 0xa8U, "", 166,
            55, true, false);
        ball.hotspot = 200U + static_cast<std::uint32_t>(index);
    }
    // Text controls 0x006e3d70, 0x006e1ce0, 0x006dba78 and 0x006e3148, the
    // tank labels from 0x006e3930/0x006e31f0, the operand digits from
    // 0x006e3540 and the result digits from 0x006e1d90.
    game.scoreText.address = 0x006e3d70U;
    game.levelText.address = 0x006e1ce0U;
    game.spareText.address = 0x006dba78U;
    game.clockText.address = 0x006e3148U;
    for (std::size_t tank = 0; tank < 5; ++tank) {
        game.capacityTexts[tank].address =
            0x006e3930U + static_cast<std::uint32_t>(tank) * 0xa8U;
        game.valueTexts[tank].address =
            0x006e31f0U + static_cast<std::uint32_t>(tank) * 0xa8U;
    }
    for (std::size_t digit = 0; digit < game.digitTexts.size(); ++digit) {
        game.digitTexts[digit].address =
            digit < 6 ? 0x006e3540U + static_cast<std::uint32_t>(digit) * 0xa8U
                      : 0x006e1d90U +
                            static_cast<std::uint32_t>(digit - 6) * 0xa8U;
    }
    for (std::size_t counter = 0; counter < 3; ++counter) {
        game.operandCounters[counter].digits = 2;
        game.operandCounters[counter].firstText =
            static_cast<int>(counter) * 2;
    }
    game.buttons.reserve(8);
    game.resultCounter.digits = 4;
    game.resultCounter.firstText = 6;
    // The C runtime seeds rand() from time(); native seeds its LCG from the
    // shared engine until a capture fixes it (mathlab:seed:N).
    game.randomState = static_cast<std::uint32_t>(
        std::uniform_int_distribution<std::uint32_t>()(randomEngine));
    std::cout << "Loaded executable-derived Math Lab: ball rack, tap, five "
                 "tanks and the level table from 0x0047fe50-0x00482230\n";
    return game;
}

MathlabScene::MathlabClip *MathlabScene::mathlabClip(const std::string &name) {
    return mathlabGame ? loadMathlabClip(mathlabGame->clips, name) : nullptr;
}

MathlabScene::MathlabClip *MathlabScene::loadMathlabClip(
    std::unordered_map<std::string, MathlabClip> &clips,
    const std::string &name) {
    if (name.empty()) {
        return nullptr;
    }
    const auto found = clips.find(name);
    if (found != clips.end()) {
        return &found->second;
    }
    // Ball clips are decoded on first use: twenty 51-frame 324x84 strips.
    MathlabClip clip;
    try {
        // The ball descriptors are copies of the keyed template 0x00512ef8
        // (0x00480d70), renamed "ball%02d"; scene_DrawAnim keys them on
        // their top-left pixel.
        const bool ball = name.rfind("ball", 0) == 0 && name.size() == 6U;
        clip.media = decodeSmacker(
            activeSceneAnimationData(name), true, true, true, 0, nullptr,
            SmackerIndexZeroAlpha::Transparent,
            ball ? SmackerKeying::TopLeftRgb565
                 : activeSceneClipKeying(name));
    } catch (const std::exception &error) {
        std::cerr << "Math Lab clip " << name << ": " << error.what() << '\n';
        return nullptr;
    }
    clip.completion = originalSmackerCompletionDuration(clip.media);
    if (!clip.media.audioSamples.empty()) {
        // Paired captures: a clip with a sound track finishes a 64 Hz pass
        // or two after a silent clip of the same length started in the same
        // tick (newball1 after newball in 29 of 29 pairs, "ball in fluid"
        // after water2 in 4 of 4). UNCERTAIN: the cause is inside
        // SMACKW32.DLL's SmackWait (import 0x0049c2e4, called at 0x00403688),
        // presumably pacing such files by their audio; one extra pass keeps
        // the observed order.
        clip.completion += 1.0 / 64.0;
    }
    if (!clip.media.videoFrames.empty()) {
        const auto &frame = clip.media.videoFrames.metadata(0);
        clip.texture.reset(SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             frame.width, frame.height));
        if (!clip.texture) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(clip.texture.get(), SDL_BLENDMODE_BLEND);
    }
    return &clips.emplace(name, std::move(clip)).first->second;
}

int MathlabScene::mathlabNow() {
    // 0x0040c450: the application clock in milliseconds.
    return static_cast<int>(std::floor(sceneElapsedNow() * 1000.0));
}

int MathlabScene::mathlabRandom(int bound) {
    // 0x0040c530: rand() % bound, with the MSVC rand() at 0x0048d323.
    auto &game = *mathlabGame;
    if (bound == 0) {
        return 0;
    }
    game.randomState = game.randomState * 0x343fdU + 0x269ec3U;
    const int value = static_cast<int>((game.randomState >> 16U) & 0x7fffU);
    return value % bound;
}

std::vector<MathlabScene::MathlabAnim *> MathlabScene::mathlabAnims() {
    if (!mathlabGame) {
        return {};
    }
    return mathlabGame->activeList;
}

std::vector<const MathlabScene::MathlabAnim *>
MathlabScene::mathlabAnims() const {
    auto anims = const_cast<MathlabScene *>(this)->mathlabAnims();
    return {anims.begin(), anims.end()};
}

void MathlabScene::addMathlabAnim(MathlabAnim &anim, int x, int y, int layer) {
    // SCENE_AddAnim 0x00412840. Its logpoint records the stack arguments, so
    // the trace keeps -1 for "the descriptor's own position".
    auto &game = *mathlabGame;
    referenceTrace.emit("animation_add", {
        {"asset", anim.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", anim.hotspot}, {"conflict_mask", 0},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.playing) {
        return; // 0x00412848: a playing descriptor is not added again.
    }
    anim.active = true;
    anim.x = x == -1 && y == -1 ? anim.defaultX : x;
    anim.y = x == -1 && y == -1 ? anim.defaultY : y;
    anim.layer = layer;
    anim.frozenAt = 0.0;
    // 0x00412991-0x0041299e: append, then sort by layer (0x0047e220).
    if (std::find(game.activeList.begin(), game.activeList.end(), &anim) ==
        game.activeList.end()) {
        game.activeList.push_back(&anim);
    }
    originalQsort(game.activeList,
                  [](const MathlabAnim *left, const MathlabAnim *right) {
                      return left->layer - right->layer;
                  });
    anim.frame = -1;
    // 0x00412958-0x00412984: a descriptor with a surface and a hotspot is hit
    // tested by rectangle (2) or, when keyed, by opaque pixel (3).
    const bool surface = anim.kind == MathlabAnimKind::Bitmap ||
                         anim.kind == MathlabAnimKind::Text ||
                         anim.kind == MathlabAnimKind::Progress ||
                         (anim.kind == MathlabAnimKind::Smacker && anim.visible);
    anim.hitMode = surface && anim.hotspot != 0 ? 3 : 0;
}

void MathlabScene::stopMathlabAnim(MathlabAnim &anim) {
    // SCENE_StopAnim 0x00412a60 is logged on entry for every descriptor.
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "stopped"},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.kind == MathlabAnimKind::Smacker && anim.playing) {
        anim.frozenAt = sceneElapsedNow() - anim.playStarted;
        anim.playing = false;
    }
    if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void MathlabScene::removeMathlabAnim(MathlabAnim &anim) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60.
    referenceTrace.emit("animation_remove", {
        {"asset", anim.name}, {"reason", "removed"},
        {"descriptor", descriptorText(anim.address)}});
    stopMathlabAnim(anim);
    anim.active = false;
    anim.playing = false;
    // 0x00412b67-0x00412b83: the last entry fills the gap, then the list is
    // sorted again.
    auto &list = mathlabGame->activeList;
    const auto found = std::find(list.rbegin(), list.rend(), &anim);
    if (found == list.rend()) {
        return;
    }
    *found = list.back();
    list.pop_back();
    originalQsort(list, [](const MathlabAnim *left, const MathlabAnim *right) {
        return left->layer - right->layer;
    });
}

void MathlabScene::playMathlabAnim(MathlabAnim &anim) {
    // SCENE_PlayAnim 0x004129d0 starts a Smacker descriptor's player.
    if (anim.kind != MathlabAnimKind::Smacker) {
        return;
    }
    MathlabClip *clip = mathlabClip(anim.name);
    if (clip == nullptr) {
        return;
    }
    const double now = sceneElapsedNow();
    const bool finished = !anim.loop && anim.frozenAt >= clip->completion;
    anim.playStarted = now - (finished ? 0.0 : anim.frozenAt);
    anim.playing = true;
    anim.frame = -1;
    anim.audioLoops = 0;
    if (!clip->media.audioSamples.empty()) {
        if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(anim.audioHandle);
        }
        anim.audioHandle = audioMixer.addEffect(
            clip->media, audioVolumes[0] * sceneAudioGains[0] * anim.volume);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void MathlabScene::addAndPlayMathlabAnim(MathlabAnim &anim) {
    // 0x00412b90 adds an inactive descriptor at its own position and layer.
    if (!anim.active) {
        addMathlabAnim(anim, anim.defaultX, anim.defaultY, anim.defaultLayer);
    }
    playMathlabAnim(anim);
}

void MathlabScene::setMathlabFrame(MathlabAnim &anim, int frame) {
    // 0x00403770 calls SmackGoto through the import at 0x0049c2ec, then the
    // redraw at 0x00413370 shows it. SmackGoto counts frames from 1: the
    // capture shows the tap at its rest frame 11 as the upright spout
    // (decoded frame 10), and the rail balls at frame n where decoded frame
    // n - 1 puts them, 6 px right of frame n.
    anim.frame = std::max(frame - 1, 0);
}

void MathlabScene::playMathlabSound(std::string_view name) {
    // 0x004804c0 copies the name into the invisible one-shot template at
    // 0x005127b8 and plays a clone of it (0x00412c20); clones take the first
    // free 0xa8-byte slot from 0x0054fea8 and are removed when they finish.
    auto &game = *mathlabGame;
    std::uint32_t address = kCloneBase;
    while (std::any_of(game.clones.begin(), game.clones.end(),
                       [&](const MathlabAnim &clone) {
                           return clone.active && clone.address == address;
                       })) {
        address += kCloneStride;
    }
    MathlabAnim clone;
    clone.kind = MathlabAnimKind::Smacker;
    clone.address = address;
    clone.name = std::string(name);
    clone.visible = false;
    clone.clone = true;
    // A finished clone's slot is reused in place: the active list holds
    // pointers into this deque, so nothing is ever erased from it.
    const auto slot = std::find_if(
        game.clones.begin(), game.clones.end(),
        [](const MathlabAnim &candidate) { return !candidate.active; });
    MathlabAnim *target = nullptr;
    if (slot != game.clones.end()) {
        *slot = std::move(clone);
        target = &*slot;
    } else {
        game.clones.push_back(std::move(clone));
        target = &game.clones.back();
    }
    addAndPlayMathlabAnim(*target);
}

void MathlabScene::finishMathlabClip(MathlabAnim &anim) {
    // 0x00413245-0x00413269: a finished non-looping player is stopped; a
    // clone is then removed, any other descriptor reports AnimationFinished
    // to the scene (event 0x110, 0x00481070).
    MathlabClip *clip = mathlabClip(anim.name);
    anim.frozenAt = clip ? clip->completion
                         : sceneElapsedNow() - anim.playStarted;
    stopMathlabAnim(anim);
    if (anim.clone) {
        removeMathlabAnim(anim);
        return;
    }
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "finished"},
        {"descriptor", descriptorText(anim.address)}});
    finishMathlabAnim(anim);
}

void MathlabScene::createMathlabText(MathlabAnim &anim, SDL_Rect rect) {
    // 0x00407930/0x004078c0: a text control replaces any earlier one and is
    // added at its rectangle's corner on layer 15.
    if (anim.active) {
        removeMathlabAnim(anim);
    }
    anim.kind = MathlabAnimKind::Text;
    anim.name.clear();
    anim.rect = rect;
    anim.text.clear();
    anim.defaultX = rect.x;
    anim.defaultY = rect.y;
    addMathlabAnim(anim, rect.x, rect.y, 15);
}

void MathlabScene::setMathlabText(MathlabAnim &anim, int font,
                                  std::string text, int flags) {
    // 0x004079c0 clears the control's surface and draws the text aligned by
    // the flags (0x004124e0).
    anim.font = font;
    anim.text = std::move(text);
    anim.textFlags = flags;
}

void MathlabScene::setMathlabNumber(MathlabAnim &anim, int font, int value) {
    // 0x004807d0: "%d", right aligned and vertically centred (0x201).
    setMathlabText(anim, font, std::to_string(value), 0x201);
}

void MathlabScene::setMathlabProgress(MathlabAnim &anim, float value) {
    // PROGRESS_Set 0x00408b00 clamps to 0..1 and adds an inactive bar.
    anim.progress = std::clamp(value, 0.0F, 1.0F);
    if (!anim.active) {
        addMathlabAnim(anim, -1, -1, anim.defaultLayer);
    }
}

bool MathlabScene::mathlabAnimHit(const MathlabAnim &anim, int x,
                                  int y) const {
    // 0x00413750, hit mode 3: an opaque pixel of the current image.
    if (anim.hitMode == 0 || !mathlabGame) {
        return false;
    }
    const auto &game = *mathlabGame;
    const int localX = x - anim.x;
    const int localY = y - anim.y;
    if (anim.kind == MathlabAnimKind::Bitmap) {
        const auto found = game.images.find(anim.name);
        if (found == game.images.end() || localX < 0 || localY < 0 ||
            localX >= found->second.width || localY >= found->second.height) {
            return false;
        }
        return pixelValue(found->second.surface.get(), localX, localY) !=
               found->second.key;
    }
    if (anim.kind == MathlabAnimKind::Smacker) {
        const auto found = game.clips.find(anim.name);
        if (found == game.clips.end() ||
            found->second.media.videoFrames.empty()) {
            return false;
        }
        const auto &media = found->second.media;
        std::size_t index = 0;
        if (anim.frame >= 0) {
            index = std::min<std::size_t>(static_cast<std::size_t>(anim.frame),
                                          media.videoFrames.size() - 1U);
        } else {
            const double time = anim.playing
                ? const_cast<MathlabScene *>(this)->sceneElapsedNow() -
                      anim.playStarted
                : anim.frozenAt;
            index = frameIndexAt(media, time, anim.loop);
        }
        const auto &frame = media.videoFrames[index];
        if (localX < 0 || localY < 0 || localX >= frame.width ||
            localY >= frame.height) {
            return false;
        }
        const std::size_t pixel =
            (static_cast<std::size_t>(localY) *
                 static_cast<std::size_t>(frame.width) +
             static_cast<std::size_t>(localX)) * 4U;
        return frame.bgra[pixel + 3U] != 0;
    }
    return false;
}

std::uint32_t MathlabScene::resolveMathlabHotspot(int x, int y) const {
    // 0x004136c0: the topmost hit-testable descriptor wins, then
    // hotspots.bmp.
    if (!mathlabGame) {
        return 0;
    }
    const auto anims = mathlabAnims();
    for (auto it = anims.rbegin(); it != anims.rend(); ++it) {
        if (mathlabAnimHit(**it, x, y)) {
            return (*it)->hotspot;
        }
    }
    const auto mask = mathlabGame->images.find("hotspots");
    if (mask == mathlabGame->images.end() || !mask->second.surface || x < 0 ||
        y < 0 || x >= mask->second.surface->w ||
        y >= mask->second.surface->h) {
        return 0;
    }
    return pixelValue(mask->second.surface.get(), x, y);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
MathlabScene::mathlabHotspotAt(int x, int y) const {
    if (!mathlabGame || activeScene != "mathlab") {
        return std::nullopt;
    }
    const std::uint32_t hotspot = resolveMathlabHotspot(x, y);
    if (hotspot == 0) {
        return std::nullopt;
    }
    return std::pair{hotspot, SDL_Rect{x, y, 1, 1}};
}

// Buttons ------------------------------------------------------------------

MathlabScene::MathlabButton *MathlabScene::mathlabButton(int id) {
    // 0x004065e0.
    for (auto &button : mathlabGame->buttons) {
        if (button.id == id) {
            return &button;
        }
    }
    return nullptr;
}

void MathlabScene::createMathlabButton(int id, int group, int type, int x,
                                       int y, std::string pressed) {
    // 0x00406ed0 (push) and 0x00406fb0 (radio, type 2 and group): the button
    // takes the first free 0x278-byte slot from 0x00533688; its descriptor
    // (+0x1c) carries the id as hotspot and layer 1 (0x0040669f). Every Math
    // Lab button has no state-0 image, so the creation refresh adds nothing.
    auto &game = *mathlabGame;
    std::size_t slot = 0;
    while (slot < game.buttons.size() && game.buttons[slot].id != 0) {
        ++slot;
    }
    if (slot == game.buttons.size()) {
        game.buttons.emplace_back();
    }
    // Slots 0 and 1 are the shared Help and Exit buttons (0x00480b4b,
    // 0x00480b6d), which the native shell draws.
    MathlabButton &button = game.buttons[slot];
    button = MathlabButton{};
    button.id = id;
    button.group = group;
    button.type = type;
    button.enabled = true;
    button.images[1] = std::move(pressed);
    button.anim.address =
        0x00533688U + static_cast<std::uint32_t>(slot + 2U) * 0x278U + 0x1cU;
    button.anim.hotspot = static_cast<std::uint32_t>(id);
    button.anim.defaultX = x;
    button.anim.defaultY = y;
    button.anim.defaultLayer = 1;
}

void MathlabScene::destroyMathlabButton(int id) {
    // 0x00406610/0x00406640.
    MathlabButton *button = mathlabButton(id);
    if (button == nullptr) {
        return;
    }
    if (button->anim.active) {
        removeMathlabAnim(button->anim);
    }
    button->id = 0;
}

void MathlabScene::refreshMathlabButton(MathlabButton &button) {
    // 0x00406760: replace the descriptor's image with the state's.
    const std::string &name =
        button.images[static_cast<std::size_t>(std::clamp(button.state, 0, 3))];
    if (button.anim.active) {
        if (!name.empty() && button.anim.name == name) {
            return;
        }
        removeMathlabAnim(button.anim);
    }
    if (!name.empty()) {
        button.anim.name = name;
        addMathlabAnim(button.anim, -1, -1, button.anim.defaultLayer);
    }
}

void MathlabScene::setMathlabButtonState(MathlabButton &button, int state) {
    // 0x00406740 refreshes only on a change.
    if (button.state != state) {
        button.state = state;
        refreshMathlabButton(button);
    }
}

void MathlabScene::setMathlabGroupState(int group, int state) {
    // 0x00406ac0.
    for (auto &button : mathlabGame->buttons) {
        if (button.id != 0 && button.group == group) {
            setMathlabButtonState(button, state);
        }
    }
}

std::optional<SDL_Rect> MathlabScene::measureMathlabButton(
    MathlabButton &button) {
    // 0x004069b0: an inactive descriptor is added and removed again to read
    // the size of its first named image (0x00406a09-0x00406a6c).
    const auto &game = *mathlabGame;
    if (button.width != 0) {
        // 0x004069c8: the size measured once is kept at +0x10/+0x14.
        return SDL_Rect{button.anim.defaultX, button.anim.defaultY,
                        button.width, button.height};
    }
    std::string name;
    if (button.anim.active) {
        name = button.anim.name;
    } else {
        for (const auto &image : button.images) {
            if (!image.empty()) {
                name = image;
                break;
            }
        }
        if (name.empty()) {
            return std::nullopt;
        }
        button.anim.name = name;
        addMathlabAnim(button.anim, -1, -1, button.anim.defaultLayer);
        removeMathlabAnim(button.anim);
    }
    const auto found = game.images.find(name);
    if (found == game.images.end()) {
        return std::nullopt;
    }
    button.width = found->second.width;
    button.height = found->second.height;
    return SDL_Rect{button.anim.defaultX, button.anim.defaultY,
                    found->second.width, found->second.height};
}

void MathlabScene::pressMathlabButtons(std::uint32_t hotspot, int x, int y) {
    // Mouse-down handler 0x00406b50, which runs after the scene's own: the
    // first button whose rectangle holds the pointer and whose id is the
    // resolved hotspot takes the press.
    auto &game = *mathlabGame;
    for (std::size_t slot = 0; slot < game.buttons.size(); ++slot) {
        MathlabButton &button = game.buttons[slot];
        if (button.id == 0) {
            continue;
        }
        const auto rect = measureMathlabButton(button);
        const SDL_Point point{x, y};
        if (!rect || !SDL_PointInRect(&point, &*rect) ||
            static_cast<std::uint32_t>(button.id) != hotspot) {
            continue;
        }
        if (button.type == 1) {
            // 0x00406c50: a push button shows its pressed image until the
            // release (0x00406d40) posts 0x116.
            if (button.state != 0) {
                continue;
            }
            setMathlabButtonState(button, 1);
            game.pressedButton = button.id;
            return;
        }
        if (button.type == 2) {
            // 0x00406c80: a radio button clears its group and posts 0x116.
            if (button.state != 0) {
                continue;
            }
            setMathlabGroupState(button.group, 0);
            setMathlabButtonState(button, 1);
            controlMathlab(button.id);
            return;
        }
    }
}

void MathlabScene::controlMathlab(int id) {
    // Control event 0x116, 0x00481350. Help (1) and Exit (2) belong to the
    // native shell's corner controls.
    auto &game = *mathlabGame;
    if (game.phase != 1) {
        return;
    }
    switch (id) {
    case 100: game.operators[0] = 0; break;
    case 101: game.operators[0] = 1; break;
    case 102: game.operators[0] = 2; break;
    case 103: game.operators[1] = 0; break;
    case 104: game.operators[1] = 1; break;
    case 105: game.operators[1] = 2; break;
    case 140: flushMathlab(); break;
    default: break;
    }
    checkMathlabLevel();
    if (id >= 100 && id < 106) {
        refreshMathlabResult();
        playMathlabSound("operator_click");
    }
}

// Scene ----------------------------------------------------------------------

void MathlabScene::initializeMathlabScene() {
    // Scene initializer 0x0047ff70 and setup 0x004809d0, in their call order.
    // It runs on the first update so the trace follows scene_change, as
    // GAME_EnterScene's logpoint precedes the original initializer.
    auto &game = *mathlabGame;
    game.initialized = true;
    game.phase = 0;
    game.score = 0;
    // 0x00480a65-0x00480b1d: the level bar and the five tank bars.
    const auto bar = [&](MathlabAnim &anim, const MathlabBar &source) {
        anim.kind = MathlabAnimKind::Progress;
        anim.name.clear();
        anim.image = source.image;
        anim.defaultX = source.x - 1; // 0x00408a1e
        anim.defaultY = source.y;
        anim.defaultLayer = 15;
        setMathlabProgress(anim, 0.0F);
    };
    bar(game.levelBar, kLevelBar);
    for (std::size_t tank = 0; tank < 5; ++tank) {
        bar(game.tankBars[tank], kTankBars[tank]);
    }
    // 0x00480b31-0x00480b6d: Help and Exit (drawn by the native shell).
    // They stay in the active list, so they take part in its ordering.
    for (MathlabAnim *button : {&game.helpButton, &game.exitButton}) {
        addMathlabAnim(*button, -1, -1, 1);
    }
    addMathlabAnim(game.start, -1, -1, 1);
    addMathlabAnim(game.labels, -1, -1, 1);
    addMathlabAnim(game.opener, -1, -1, 1);
    addMathlabAnim(game.tap, -1, -1, 1);
    // 0x00480ee0: the tap rests over the middle tank.
    game.tapFrame = kTapRestFrame;
    setMathlabFrame(game.tap, game.tapFrame);
    game.queue.clear();
    game.tapState = 0;
    addMathlabAnim(game.fan, -1, -1, 1);
    createMathlabText(game.scoreText, kScoreRect);
    createMathlabText(game.levelText, kLevelRect);
    createMathlabText(game.spareText, kSpareRect);
    createMathlabText(game.clockText, kClockRect);
    for (std::size_t tank = 0; tank < 5; ++tank) {
        createMathlabText(game.capacityTexts[tank], kCapacityRects[tank]);
        createMathlabText(game.valueTexts[tank], kValueRects[tank]);
    }
    // 0x00480c5f-0x00480cd2: two digits per operand chamber; 0x00480dc0
    // starts each counter at zero.
    for (std::size_t counter = 0; counter < 3; ++counter) {
        for (std::size_t digit = 0; digit < 2; ++digit) {
            const int x = kOperandDigitX[counter * 2 + digit];
            createMathlabText(game.digitTexts[counter * 2 + digit],
                              rectFromEdges(x, 0xd7, x + 0xe, 0xe8));
        }
        startMathlabCounter(game.operandCounters[counter], 0);
        drawMathlabCounter(game.operandCounters[counter]);
    }
    for (std::size_t digit = 0; digit < 4; ++digit) {
        const int x = kResultDigitX[digit];
        createMathlabText(game.digitTexts[6 + digit],
                          rectFromEdges(x, 0x141, x + 0xe, 0x152));
    }
    startMathlabCounter(game.resultCounter, 0);
    drawMathlabCounter(game.resultCounter);
    // 0x00480f20 clears the tank labels; 0x00480940 resets the operands.
    for (std::size_t tank = 0; tank < 5; ++tank) {
        setMathlabText(game.capacityTexts[tank], kYellow14, "", 0);
        setMathlabText(game.valueTexts[tank], kYellow14, "", 0);
    }
    resetMathlabOperands();
    game.releaseSerial = 1; // 0x0047ff99
}

void MathlabScene::startMathlabCounter(MathlabCounter &counter, int target) {
    // 0x00480500.
    counter.from = counter.shown;
    counter.target = target;
    counter.animating = true;
    counter.started = mathlabNow();
}

void MathlabScene::drawMathlabCounter(MathlabCounter &counter) {
    // 0x00480df0: the shown value moves one unit per 4 ms towards the target
    // and is drawn digit by digit, black or (negative) red, with a red minus
    // over the first digit.
    auto &game = *mathlabGame;
    if (!counter.animating) {
        return;
    }
    const int steps = (mathlabNow() - counter.started) / 4;
    if (counter.target > counter.from) {
        counter.shown = counter.from + steps;
        if (counter.shown > counter.target) {
            counter.shown = counter.target;
            counter.animating = false;
        }
    } else {
        counter.shown = counter.from - steps;
        if (counter.shown < counter.target) {
            counter.shown = counter.target;
            counter.animating = false;
        }
    }
    int value = std::abs(counter.shown);
    const int font = counter.shown < 0 ? kRed16 : kBlack16;
    for (int digit = counter.digits - 1; digit >= 0; --digit) {
        setMathlabNumber(
            game.digitTexts[static_cast<std::size_t>(counter.firstText + digit)],
            font, value % 10);
        value /= 10;
    }
    if (counter.shown < 0) {
        setMathlabText(
            game.digitTexts[static_cast<std::size_t>(counter.firstText)],
            kRed16, "-", 0x201);
    }
}

int MathlabScene::mathlabResult() const {
    // 0x00481790: left to right; with two chambers the third operand is
    // zero and its operator stays "+".
    const auto &game = *mathlabGame;
    int result = game.operands[0];
    for (std::size_t index = 0; index < 2; ++index) {
        const int value = game.operands[index + 1];
        switch (game.operators[index]) {
        case 0: result += value; break;
        case 1: result -= value; break;
        case 2: result *= value; break;
        default: break;
        }
    }
    return result;
}

void MathlabScene::refreshMathlabResult() {
    // 0x004817d0.
    startMathlabCounter(mathlabGame->resultCounter, mathlabResult());
}

void MathlabScene::resetMathlabOperands() {
    // 0x00480940.
    auto &game = *mathlabGame;
    game.operators = {0, 0};
    for (std::size_t index = 0; index < 3; ++index) {
        game.operands[index] = 0;
        startMathlabCounter(game.operandCounters[index], 0);
    }
    setMathlabGroupState(1, 0);
    if (MathlabButton *button = mathlabButton(100)) {
        setMathlabButtonState(*button, 1);
    }
    setMathlabGroupState(2, 0);
    if (MathlabButton *button = mathlabButton(103)) {
        setMathlabButtonState(*button, 1);
    }
    refreshMathlabResult();
    if (game.phase == 1) {
        playMathlabSound("click2");
    }
}

void MathlabScene::showMathlabTank(int tank) {
    // 0x00480850: fill bar, MAX label and the tank's value: yellow below
    // the maximum, green at it, red above.
    auto &game = *mathlabGame;
    const auto index = static_cast<std::size_t>(tank);
    const int value = game.tanks[index];
    const int capacity = game.capacities[index];
    setMathlabProgress(game.tankBars[index],
                       static_cast<float>(value) / static_cast<float>(capacity));
    setMathlabNumber(game.capacityTexts[index], kRed14, capacity);
    setMathlabNumber(game.valueTexts[index],
                     value < capacity ? kYellow14
                                      : value == capacity ? kGreen14 : kRed16,
                     value);
}

void MathlabScene::clearMathlabTank(int tank) {
    // 0x00480810.
    auto &game = *mathlabGame;
    const auto index = static_cast<std::size_t>(tank);
    setMathlabProgress(game.tankBars[index], 0.0F);
    game.tanks[index] = 0;
    game.pours[index] = 0;
    showMathlabTank(tank);
}

void MathlabScene::showMathlabScore() {
    // 0x00480900: the score and the level bar.
    auto &game = *mathlabGame;
    game.shownScore = game.score;
    setMathlabNumber(game.scoreText, kRed14, game.score);
    setMathlabProgress(game.levelBar,
                       static_cast<float>(game.levelProgress) /
                           static_cast<float>(game.levelGoal));
}

void MathlabScene::penalizeMathlab(int seconds) {
    // 0x004818c0: seconds off the clock (negative adds time). Once the
    // deadline falls behind the elapsed time since the first deadline the
    // game ends (state 2) and the balls are released.
    auto &game = *mathlabGame;
    const int now = mathlabNow();
    if (game.phase == 2) {
        return;
    }
    game.penalty = std::max(0, game.penalty + seconds);
    game.deadline -= seconds * 1000;
    if (game.deadline < now - game.clockBase) {
        game.phase = 2;
        releaseMathlabBalls();
    }
}

void MathlabScene::startMathlabGame(int difficulty) {
    // 0x0047ffd0.
    auto &game = *mathlabGame;
    game.penalty = 0;
    game.score = 0;
    game.shownScore = 0;
    game.levelIndex = difficulty;
    const int now = mathlabNow();
    game.clockBase = now;
    game.thirdDisabled = true;
    int deadline = now;
    if (difficulty >= 0 && difficulty < 3) {
        deadline += kStartMilliseconds[static_cast<std::size_t>(difficulty)];
        game.clockBase = deadline;
    }
    game.deadline = deadline;
    enterMathlabLevel(true);
    if (!game.disable2.active) {
        addMathlabAnim(game.disable2, -1, -1, 1);
    }
    showMathlabScore();
    startMathlabCounter(game.resultCounter, 0);
    resetMathlabOperands();
    resetMathlabOperands();
    game.phase = 1;
    // 0x004800a8-0x0048013c, then 0x00480144-0x00480151.
    createMathlabButton(140, 0, 1, 0x27f, 0x1fb, "ok_button");
    createMathlabButton(100, 1, 2, 0x124, 0xbf, "plus_1");
    createMathlabButton(101, 1, 2, 0x124, 0xd7, "minus_1");
    createMathlabButton(102, 1, 2, 0x124, 0xee, "multiply_1");
    setMathlabGroupState(1, 0);
    if (MathlabButton *button = mathlabButton(100)) {
        setMathlabButtonState(*button, 1);
    }
    for (auto &ball : game.rack) {
        ball.active = false;
    }
    for (int count = 0; count < 7; ++count) {
        spawnMathlabBall();
    }
}

void MathlabScene::enterMathlabLevel(bool first) {
    // 0x00480530.
    auto &game = *mathlabGame;
    if (!first) {
        game.levelIndex = std::min(game.levelIndex + 1, 0x16);
        releaseMathlabBalls();
        if (!game.lightbulb.active) {
            addAndPlayMathlabAnim(game.lightbulb);
            playMathlabSound("light_bulb");
        }
    } else {
        game.openerMode = false;
    }
    if (game.levelIndex >= 2 && game.thirdDisabled) {
        // 0x004805a6: the third chamber and its operators open.
        createMathlabButton(103, 2, 2, 0x1a1, 0xc0, "plus_2");
        createMathlabButton(104, 2, 2, 0x1a0, 0xd7, "minus_2");
        createMathlabButton(105, 2, 2, 0x19f, 0xee, "multiply_2");
        setMathlabGroupState(2, 0);
        if (MathlabButton *button = mathlabButton(103)) {
            setMathlabButtonState(*button, 1);
        }
        playMathlabSound("3 shows up");
        if (game.disable2.active) {
            removeMathlabAnim(game.disable2);
        }
        game.thirdDisabled = false;
    }
    const MathlabLevel &level =
        kLevels[static_cast<std::size_t>(game.levelIndex)];
    game.pendingBalls = 0;
    game.flushPending = false;
    game.deadline += level.seconds * 1000;
    setMathlabProgress(game.levelBar, 0.0F);
    setMathlabNumber(game.levelText, kRed14, game.levelIndex + 1);
    // 0x0048069c: a capture fixture reseeds rand() here (mathlab:seed:N).
    if (game.fixedSeed) {
        game.randomState = *game.fixedSeed;
    }
    int goal = 0;
    for (std::size_t tank = 0; tank < 5; ++tank) {
        game.capacities[tank] = mathlabRandom(level.spread) + level.bases[tank];
        clearMathlabTank(static_cast<int>(tank));
        goal += level.bases[tank] * level.multiplier;
    }
    game.levelProgress = 0;
    game.levelGoal = goal;
}

void MathlabScene::releaseMathlabBalls() {
    // 0x00480730: open the drum gate; every ball already on the rail rolls
    // out, tagged with this release.
    auto &game = *mathlabGame;
    if (game.openerMode) {
        return;
    }
    game.openerMode = true;
    if (!game.opener.active) {
        addMathlabAnim(game.opener, -1, -1, 1);
    }
    playMathlabAnim(game.opener);
    playMathlabSound("openerp");
    for (auto &ball : game.rack) {
        if (ball.active && ball.position >= 0) {
            ball.released = game.releaseSerial;
        }
    }
    ++game.releaseSerial;
}

int MathlabScene::allocateMathlabBallAnim() {
    // 0x00480390: the first free of thirty ball descriptors.
    auto &game = *mathlabGame;
    for (std::size_t index = 0; index < game.ballAnimUsed.size(); ++index) {
        if (!game.ballAnimUsed[index]) {
            game.ballAnimUsed[index] = true;
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool MathlabScene::mathlabValueCrowded(int value) const {
    // 0x004803d0: at most three balls of one value, and from ranges above
    // ten at most three balls of ten or more before another above ten.
    const auto &game = *mathlabGame;
    const MathlabLevel &level =
        kLevels[static_cast<std::size_t>(game.levelIndex)];
    if (level.balls > 10) {
        const auto large = std::count_if(
            game.rack.begin(), game.rack.end(), [](const MathlabBall &ball) {
                return ball.active && ball.value >= 10;
            });
        if (large > 3 && value > 10) {
            return true;
        }
    }
    const auto same = std::count_if(
        game.rack.begin(), game.rack.end(), [value](const MathlabBall &ball) {
            return ball.active && ball.value == value;
        });
    return same > 2;
}

void MathlabScene::sortMathlabRack() {
    // 0x00480440: qsort with 0x00480460, active balls first, furthest along
    // first.
    std::vector<MathlabBall> rack(mathlabGame->rack.begin(),
                                  mathlabGame->rack.end());
    originalQsort(rack, [](const MathlabBall &left, const MathlabBall &right) {
        if (!left.active && !right.active) return 0;
        if (!left.active) return 1;
        if (!right.active) return -1;
        if (left.position < right.position) return 1;
        return left.position > right.position ? -1 : 0;
    });
    std::copy(rack.begin(), rack.end(), mathlabGame->rack.begin());
}

bool MathlabScene::spawnMathlabBall() {
    // 0x00480180: one new ball for a free rack slot, with the drum's
    // start/roll/new-ball clips.
    auto &game = *mathlabGame;
    const auto freeSlot = std::find_if(
        game.rack.begin(), game.rack.end(),
        [](const MathlabBall &ball) { return !ball.active; });
    if (freeSlot == game.rack.end()) {
        return false;
    }
    const bool endActive = game.endballs.active;
    const bool startPlaying = game.startballs.playing;
    const bool ballsActive = game.balls.active;
    const auto newBall = [&] {
        if (game.newball.active) {
            ++game.pendingBalls;
        } else {
            addAndPlayMathlabAnim(game.newball);
            playMathlabSound("newball1");
        }
    };
    if (startPlaying || endActive) {
        if (ballsActive) {
            newBall();
        } else {
            ++game.pendingBalls;
        }
    } else if (ballsActive) {
        newBall();
    } else {
        addAndPlayMathlabAnim(game.startballs);
        playMathlabAnim(game.fan);
        addAndPlayMathlabAnim(game.fanFade);
        game.fanFade.volume = kFanVolume;
        if (game.fanFade.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(game.fanFade.audioHandle);
            game.fanFade.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
            playMathlabAnim(game.fanFade);
        }
        addAndPlayMathlabAnim(game.ballsSound);
        ++game.pendingBalls;
    }
    int position = endActive ? -42 : startPlaying ? -36 : -30;
    int limit = kRackFront;
    for (const auto &ball : game.rack) {
        if (ball.active) {
            limit = std::min(limit, ball.position - 5);
            position = std::min(position, ball.position - 5);
        }
    }
    const MathlabLevel &level =
        kLevels[static_cast<std::size_t>(game.levelIndex)];
    int value = 0;
    do {
        value = mathlabRandom(level.balls) + 1;
    } while (mathlabValueCrowded(value));
    MathlabBall &ball = *std::find_if(
        game.rack.begin(), game.rack.end(),
        [](const MathlabBall &candidate) { return !candidate.active; });
    ball.value = value;
    ball.time = mathlabNow();
    ball.active = true;
    ball.position = position;
    ball.limit = limit;
    ball.descriptor = allocateMathlabBallAnim();
    if (ball.descriptor >= 0) {
        MathlabAnim &anim =
            game.ballAnims[static_cast<std::size_t>(ball.descriptor)];
        anim.name = twoDigits("ball%02d", value);
        mathlabClip(anim.name);
        addMathlabAnim(anim, -1, -1, -1);
        if (limit >= 0) {
            setMathlabFrame(anim, 0);
        }
    }
    ball.released = 0;
    ball.sound = 0;
    sortMathlabRack();
    return true;
}

void MathlabScene::removeMathlabBall(std::uint32_t hotspot) {
    // 0x004816a0/0x00481700.
    auto &game = *mathlabGame;
    for (auto &ball : game.rack) {
        if (!ball.active || ball.descriptor < 0) {
            continue;
        }
        MathlabAnim &anim =
            game.ballAnims[static_cast<std::size_t>(ball.descriptor)];
        if (anim.hotspot != hotspot) {
            continue;
        }
        removeMathlabAnim(anim);
        game.ballAnimUsed[static_cast<std::size_t>(ball.descriptor)] = false;
        ball.active = false;
        sortMathlabRack();
        return;
    }
}

void MathlabScene::checkMathlabLevel() {
    // 0x00481450.
    auto &game = *mathlabGame;
    if (game.levelProgress >= game.levelGoal) {
        enterMathlabLevel(false);
    }
}

void MathlabScene::flushMathlab() {
    // 0x00481470: the red TØM button pumps the tanks out once something is
    // in them; the scoring follows in the tick (0x00481a5d).
    auto &game = *mathlabGame;
    if (game.flushPending ||
        std::all_of(game.tanks.begin(), game.tanks.end(),
                    [](int value) { return value == 0; })) {
        return;
    }
    game.flushPending = true;
    playMathlabSound("Flush in2P");
    if (!game.pump.playing) {
        addAndPlayMathlabAnim(game.pump);
    }
    if (!game.flush.active) {
        addAndPlayMathlabAnim(game.flush);
    }
}

void MathlabScene::scoreMathlabTank(int tank, bool single) {
    // 0x00481ff0: a tank within its maximum banks its value plus a factor
    // by pours; an exact tank adds ten seconds and its maximum to the bonus.
    auto &game = *mathlabGame;
    const auto index = static_cast<std::size_t>(tank);
    if (single) {
        game.bonus = 0;
    }
    const int value = game.tanks[index];
    if (value > 0 && value <= game.capacities[index]) {
        if (value == game.capacities[index]) {
            penalizeMathlab(-10);
            game.bonus += game.capacities[index];
        }
        penalizeMathlab(0);
        game.score += value;
        game.levelProgress += value;
        const int pours = std::min(game.pours[index], 3);
        game.score += static_cast<int>(
            static_cast<float>(value) *
            kPourFactors[static_cast<std::size_t>(pours)]);
    }
    if (single) {
        if (game.bonus != 0) {
            game.score += game.bonus;
        }
        showMathlabScore();
    }
    clearMathlabTank(tank);
}

void MathlabScene::scoreMathlabTanks() {
    // 0x00481f70: then the exact-tank bonus times 0x0051279c[exact].
    auto &game = *mathlabGame;
    game.bonus = 0;
    int exact = 0;
    for (std::size_t tank = 0; tank < 5; ++tank) {
        exact += game.tanks[tank] == game.capacities[tank] ? 1 : 0;
    }
    for (int tank = 0; tank < 5; ++tank) {
        scoreMathlabTank(tank, false);
    }
    if (game.bonus > 0) {
        game.bonus = static_cast<int>(
            static_cast<float>(game.bonus) *
            kExactFactors[static_cast<std::size_t>(exact)]);
        game.score += game.bonus;
    }
    showMathlabScore();
}

void MathlabScene::updateMathlabTap() {
    // 0x00481d10: the tap swings to the first queued tank at one frame per
    // 50 ms, then pours; an overfilled tank washes all five out.
    auto &game = *mathlabGame;
    if (game.queue.empty() || game.tapState == 2) {
        return;
    }
    const int now = mathlabNow();
    if (game.tapState == 0) {
        playMathlabSound("pipe squeek1");
        game.tapFrom = game.tapFrame;
        game.tapStarted = now;
        game.tapState = 1;
    }
    const MathlabPour pour = game.queue.front();
    const auto index = static_cast<std::size_t>(pour.tank);
    const int target = kTapFrames[index];
    const int steps = (now - game.tapStarted) / kTapMilliseconds;
    int frame = 0;
    bool arrived = false;
    if (target < game.tapFrom) {
        frame = game.tapFrom - steps;
        arrived = frame <= target;
    } else {
        frame = game.tapFrom + steps;
        arrived = frame >= target;
    }
    if (arrived) {
        game.tanks[index] += pour.value;
        ++game.pours[index];
        if (game.tanks[index] > game.capacities[index]) {
            game.blinkValue[index] = game.tanks[index];
            game.blinkStarted[index] = now;
            penalizeMathlab(10);
            for (int tank = 0; tank < 5; ++tank) {
                clearMathlabTank(tank);
            }
            playMathlabSound("flush out1P");
            if (!game.overflow.active) {
                addAndPlayMathlabAnim(game.overflow);
            }
        } else {
            showMathlabTank(pour.tank);
        }
        addAndPlayMathlabAnim(game.water[index]);
        playMathlabSound("ball in fluid");
        game.queue.erase(game.queue.begin());
        game.tapState = 2;
        frame = target;
    }
    game.tapFrame = frame;
    setMathlabFrame(game.tap, frame);
}

void MathlabScene::updateMathlabRack() {
    // 0x00481aa0: balls roll one frame per 66 ms up to their limit; the
    // front ball stops at 36, each next five behind. Released balls roll to
    // 50 and drop out; then the gate closes and a new set rolls in.
    auto &game = *mathlabGame;
    if (!game.rack[0].active) {
        return;
    }
    game.rack[0].limit = std::max(game.rack[0].limit, kRackFront);
    if (game.openerMode) {
        game.rack[0].limit = kRackDrop;
    }
    for (std::size_t index = 1; index < game.rack.size(); ++index) {
        MathlabBall &ball = game.rack[index];
        if (!ball.active) {
            continue;
        }
        ball.limit = std::max(ball.limit, game.rack[index - 1].limit - 5);
        if (ball.released == 0 && ball.limit > kRackFront) {
            ball.limit = kRackFront;
        }
    }
    const int now = mathlabNow();
    for (std::size_t index = 0; index < game.rack.size(); ++index) {
        MathlabBall &ball = game.rack[index];
        if (ball.descriptor >= 0 &&
            game.ballAnims[static_cast<std::size_t>(ball.descriptor)].active) {
            if (ball.position > 5 && ball.sound == 0) {
                // 0x00481ca0: rand(30) picks ball1, ball2 or ball3.
                const int pick = mathlabRandom(30);
                playMathlabSound(pick < 10 ? "ball1"
                                           : pick < 20 ? "ball2" : "ball3");
                ++ball.sound;
            } else if (ball.position > kRackFront && ball.sound == 1) {
                // 0x00481ce0.
                playMathlabSound(mathlabRandom(20) < 10 ? "balls out new1"
                                                        : "balls out new2");
                ++ball.sound;
            }
        }
        if (!ball.active || (game.openerMode && ball.released == 0)) {
            continue;
        }
        int elapsed = now - ball.time;
        int position = ball.position;
        if (elapsed > kRollMilliseconds) {
            const int steps = (elapsed - 1) / kRollMilliseconds;
            position += steps;
            elapsed -= steps * kRollMilliseconds;
        }
        if (position == ball.position) {
            continue;
        }
        position = std::min(position, ball.limit);
        ball.position = position;
        ball.time = now - elapsed;
        if (position < 0 || ball.descriptor < 0) {
            continue;
        }
        MathlabAnim &anim =
            game.ballAnims[static_cast<std::size_t>(ball.descriptor)];
        setMathlabFrame(anim, position);
        if (position == kRackDrop) {
            removeMathlabBall(anim.hotspot);
        }
    }
    if (!game.openerMode) {
        return;
    }
    if (std::any_of(game.rack.begin(), game.rack.end(),
                    [](const MathlabBall &ball) {
                        return ball.active && ball.released != 0;
                    })) {
        return;
    }
    for (auto &ball : game.rack) {
        if (ball.active && ball.released == 0) {
            ball.time = now;
        }
    }
    removeMathlabAnim(game.opener);
    addAndPlayMathlabAnim(game.closer);
    if (game.phase != 2) {
        playMathlabSound("closerP");
    }
    game.openerMode = false;
    if (game.phase != 2) {
        for (int count = 0; count < 7; ++count) {
            spawnMathlabBall();
        }
    }
}

void MathlabScene::updateMathlabBlink() {
    // 0x00481ed0: an overfilled tank's value blinks red ten times, 300 ms
    // per phase, before the tank shows again.
    auto &game = *mathlabGame;
    const int now = mathlabNow();
    for (std::size_t tank = 0; tank < 5; ++tank) {
        if (game.blinkValue[tank] == 0) {
            continue;
        }
        const int period = (now - game.blinkStarted[tank]) / kBlinkMilliseconds;
        if (period < 10) {
            if ((period & 1) == 1) {
                setMathlabNumber(game.valueTexts[tank], kRed14,
                                 game.blinkValue[tank]);
            } else {
                setMathlabText(game.valueTexts[tank], kRed14, "", 0);
            }
        } else {
            game.blinkValue[tank] = 0;
            showMathlabTank(static_cast<int>(tank));
        }
    }
}

void MathlabScene::mathlabTick() {
    // Update event 0x112, 0x00481930.
    auto &game = *mathlabGame;
    for (std::size_t counter = 0; counter < 3; ++counter) {
        if (game.thirdDisabled && counter == 2) {
            // 0x00481961: the closed third chamber shows no digits.
            for (int digit = 0; digit < 2; ++digit) {
                setMathlabText(game.digitTexts[static_cast<std::size_t>(
                                   game.operandCounters[2].firstText + digit)],
                               kBlack16, "", 0);
            }
            break;
        }
        drawMathlabCounter(game.operandCounters[counter]);
    }
    drawMathlabCounter(game.resultCounter);
    if (game.phase == 0) {
        return;
    }
    if (game.phase != 2) {
        spawnMathlabBall();
    }
    updateMathlabTap();
    updateMathlabRack();
    updateMathlabBlink();
    const int remaining = game.deadline - mathlabNow();
    {
        // 0x0041ecb0 splits the milliseconds; the clock reads mm:ss:cc.
        const int shown = std::max(remaining, 0);
        char text[32]{};
        std::snprintf(text, sizeof(text), "%02d:%02d:%02d", shown / 60000,
                      shown / 1000 % 60, shown % 1000 / 10);
        setMathlabText(game.clockText, kRed14, text, 0x101);
    }
    if (remaining < 0) {
        penalizeMathlab(20);
    }
    if (game.tapState == 0 && game.flushPending && game.queue.empty()) {
        scoreMathlabTanks();
        checkMathlabLevel();
        game.flushPending = false;
    }
    if (game.phase == 2 && game.tapState == 0 && game.queue.empty() &&
        !game.openerMode && !game.gameOverModal) {
        mathlabGameOver();
    }
}

void MathlabScene::mathlabGameOver() {
    // 0x004820d0, up to the blocking message box (0x00482131) and the score
    // wrapper (0x00482143).
    auto &game = *mathlabGame;
    for (int id : {100, 101, 102}) {
        destroyMathlabButton(id);
    }
    if (!game.thirdDisabled) {
        for (int id : {103, 104, 105}) {
            destroyMathlabButton(id);
        }
    }
    game.gameOverModal = true;
    // UNCERTAIN: the body is the "whackamole" section's GAMEOVER string,
    // pushed at 0x00482115; the title is interfaceh:GAMEOVER.
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt!"),
        language.get("whackamole", "GAMEOVER", ""),
        MessageKind::MathlabGameOver);
}

void MathlabScene::finishMathlabGameOver() {
    // 0x0048215a-0x00482212, after the score wrapper returns.
    if (!mathlabGame) {
        return;
    }
    auto &game = *mathlabGame;
    game.gameOverModal = false;
    for (std::size_t tank = 0; tank < 5; ++tank) {
        setMathlabText(game.capacityTexts[tank], kYellow14, "", 0);
        setMathlabText(game.valueTexts[tank], kYellow14, "", 0);
    }
    for (MathlabAnim *text : {&game.scoreText, &game.levelText,
                              &game.spareText, &game.clockText}) {
        setMathlabText(*text, kRed14, "", 0);
    }
    resetMathlabOperands();
    for (auto &bar : game.tankBars) {
        setMathlabProgress(bar, 0.0F);
    }
    setMathlabProgress(game.levelBar, 0.0F);
    destroyMathlabButton(140);
    addMathlabAnim(game.start, -1, -1, 1);
    game.phase = 0;
    game.tapFrame = kTapRestFrame;
    setMathlabFrame(game.tap, game.tapFrame);
    game.queue.clear();
    game.tapState = 0;
}

void MathlabScene::finishMathlabAnim(MathlabAnim &anim) {
    // Animation-finished event 0x110, 0x00481070.
    auto &game = *mathlabGame;
    if (&anim == &game.closer) {
        removeMathlabAnim(game.closer);
        if (game.opener.active) {
            removeMathlabAnim(game.opener);
        }
        addMathlabAnim(game.opener, -1, -1, 1);
    } else if (&anim == &game.overflow || &anim == &game.flush ||
               &anim == &game.pump || &anim == &game.dumpball ||
               &anim == &game.lightbulb) {
        removeMathlabAnim(anim);
    } else if (&anim >= &game.water[0] && &anim <= &game.water[4]) {
        removeMathlabAnim(anim);
        game.tapState = 0;
    } else if (&anim == &game.startballs) {
        removeMathlabAnim(anim);
        addAndPlayMathlabAnim(game.balls);
        if (game.pendingBalls != 0 && !game.newball.active) {
            addAndPlayMathlabAnim(game.newball);
            playMathlabSound("newball1");
            --game.pendingBalls;
        }
    } else if (&anim == &game.balls) {
        removeMathlabAnim(game.balls);
        if (game.ballsSound.active) {
            removeMathlabAnim(game.ballsSound);
        }
        if (game.pendingBalls == 0) {
            addAndPlayMathlabAnim(game.endballs);
            addAndPlayMathlabAnim(game.endballSound);
            game.endballSound.volume = kEndballVolume;
            if (game.endballSound.audioHandle !=
                LiveAudioMixer::kInvalidEffectHandle) {
                audioMixer.removeEffect(game.endballSound.audioHandle);
                game.endballSound.audioHandle =
                    LiveAudioMixer::kInvalidEffectHandle;
                playMathlabAnim(game.endballSound);
            }
        } else {
            addAndPlayMathlabAnim(game.balls);
            addAndPlayMathlabAnim(game.ballsSound);
        }
    } else if (&anim == &game.endballs) {
        removeMathlabAnim(anim);
        if (game.pendingBalls != 0) {
            addAndPlayMathlabAnim(game.startballs);
        } else if (game.fan.active) {
            stopMathlabAnim(game.fan);
            if (game.fanLong.active) {
                removeMathlabAnim(game.fanLong);
            }
            if (game.fanFade.active) {
                removeMathlabAnim(game.fanFade);
            }
        }
    } else if (&anim == &game.newball) {
        removeMathlabAnim(anim);
        if (game.pendingBalls != 0) {
            addAndPlayMathlabAnim(game.newball);
            playMathlabSound("newball1");
            --game.pendingBalls;
        }
    } else if (&anim == &game.fanFade) {
        removeMathlabAnim(anim);
        game.fanLong.volume = kFanVolume;
        addAndPlayMathlabAnim(game.fanLong);
    } else if (&anim == &game.endballSound) {
        removeMathlabAnim(anim);
    }
}

void MathlabScene::dropMathlabBall(std::uint32_t hotspot) {
    // Drop event 0x10b, 0x004817f0: a ball let go over chamber 30-32 fills
    // it; anywhere else it is dumped, two seconds off the clock.
    auto &game = *mathlabGame;
    if (game.ballDrag.active) {
        removeMathlabAnim(game.ballDrag);
    }
    if (game.phase != 1) {
        return;
    }
    const int id = static_cast<int>(hotspot);
    if (id >= 30 && id < 33) {
        const int slot = id - 30;
        if (game.thirdDisabled && slot >= 2) {
            return;
        }
        playMathlabSound("click2");
        game.operands[static_cast<std::size_t>(slot)] = game.dragValue;
        startMathlabCounter(game.operandCounters[static_cast<std::size_t>(slot)],
                            game.dragValue);
        refreshMathlabResult();
        return;
    }
    penalizeMathlab(2);
    if (!game.dumpball.active) {
        addAndPlayMathlabAnim(game.dumpball);
    }
}

bool MathlabScene::beginMathlabInteraction(int x, int y) {
    // Mouse-down 0x004814f0, then the button handler 0x00406b50.
    if (!mathlabGame || activeScene != "mathlab" || !mathlabGame->initialized) {
        return false;
    }
    auto &game = *mathlabGame;
    if (game.gameOverModal) {
        return true;
    }
    const std::uint32_t hotspot = resolveMathlabHotspot(x, y);
    const int id = static_cast<int>(hotspot);
    if (game.phase == 0) {
        if (id == 3) {
            removeMathlabAnim(game.start);
            startMathlabGame(0);
        }
    } else if (game.phase != 2) {
        if (id >= 200 && id < 230) {
            auto ball = std::find_if(
                game.rack.begin(), game.rack.end(),
                [&](const MathlabBall &candidate) {
                    return candidate.active && candidate.descriptor >= 0 &&
                           game.ballAnims[static_cast<std::size_t>(
                                              candidate.descriptor)]
                                   .hotspot == hotspot;
                });
            if (ball != game.rack.end() && ball->position <= kRackFront) {
                // 0x0040c1f0 registers ball_0 as the movable descriptor;
                // it is added centred on the pointer at layer 0x70.
                game.dragValue = ball->value;
                game.dragging = true;
                if (!game.ballDrag.active) {
                    game.ballDrag.name = twoDigits("ball_%02d", ball->value);
                    addMathlabAnim(game.ballDrag, x - 20, y - 20, 0x70);
                }
                game.dragOffsetX = game.ballDrag.x - x;
                game.dragOffsetY = game.ballDrag.y - y;
                removeMathlabBall(hotspot);
                spawnMathlabBall();
            }
        } else if (id >= 20 && id < 25) {
            // 0x00481607: a result above zero from at least two filled
            // chambers is queued for the tank; anything else is refused.
            const int result = mathlabResult();
            const auto filled = std::count_if(
                game.operands.begin(), game.operands.end(),
                [](int value) { return value != 0; });
            if (result > 0 && filled >= 2) {
                game.queue.push_back({id - 20, result});
                resetMathlabOperands();
            } else {
                playMathlabSound("negative");
            }
        }
        // UNCERTAIN: 0x004815bb-0x004815fa drag a descriptor from
        // 0x00512404 for hotspots 50-54, but neither hotspots.bmp nor any
        // Math Lab descriptor carries those ids, so the branch is unreachable.
    }
    pressMathlabButtons(hotspot, x, y);
    return true;
}

void MathlabScene::moveMathlabInteraction(int x, int y) {
    // 0x0040c300 moves the dragged descriptor with the pointer.
    if (!mathlabGame || !mathlabGame->dragging) {
        return;
    }
    auto &game = *mathlabGame;
    game.ballDrag.x = x + game.dragOffsetX;
    game.ballDrag.y = y + game.dragOffsetY;
}

void MathlabScene::endMathlabInteraction(int x, int y) {
    if (!mathlabGame) {
        return;
    }
    auto &game = *mathlabGame;
    // 0x00406cf0/0x00406d40: a pressed push button is released and posts
    // control event 0x116.
    if (game.pressedButton != 0) {
        const int id = game.pressedButton;
        game.pressedButton = 0;
        if (MathlabButton *button = mathlabButton(id);
            button != nullptr && button->state == 1) {
            setMathlabButtonState(*button, 0);
            if (button->enabled) {
                controlMathlab(id);
            }
        }
    }
    // 0x0040c260 resolves the hotspot under the pointer and posts 0x10b.
    if (game.dragging) {
        game.dragging = false;
        moveMathlabInteraction(x, y);
        dropMathlabBall(resolveMathlabHotspot(x, y));
    }
}

void MathlabScene::updateMathlab(double elapsed) {
    if (!mathlabGame) {
        return;
    }
    auto &game = *mathlabGame;
    if (!game.initialized) {
        initializeMathlabScene();
    }
    // The tick (0x112) runs before the descriptor update loop 0x00413200,
    // which finishes one-shot players. The modal dialogs block the
    // original's main loop.
    if (game.gameOverModal) {
        return;
    }
    mathlabTick();
    // 0x00413200 walks the live list by index: a clip removed on finishing
    // moves the last entry into its place, which this pass then skips.
    for (std::size_t index = 0; index < game.activeList.size(); ++index) {
        MathlabAnim *anim = game.activeList[index];
        if (anim->kind != MathlabAnimKind::Smacker || !anim->playing) {
            continue;
        }
        MathlabClip *clip = mathlabClip(anim->name);
        if (clip == nullptr) {
            continue;
        }
        const double played = elapsed - anim->playStarted;
        if (!anim->loop && played >= clip->completion) {
            finishMathlabClip(*anim);
        } else if (anim->loop && clip->media.videoDuration > 0.0 &&
                   !clip->media.audioSamples.empty()) {
            const auto loops =
                static_cast<std::size_t>(played / clip->media.videoDuration);
            if (loops > anim->audioLoops) {
                anim->audioLoops = loops;
                if (anim->audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
                    audioMixer.removeEffect(anim->audioHandle);
                }
                anim->audioHandle = audioMixer.addEffect(
                    clip->media,
                    audioVolumes[0] * sceneAudioGains[0] * anim->volume);
            }
        }
    }
}

void MathlabScene::renderMathlab(double elapsed) {
    if (!mathlabGame) {
        return;
    }
    auto &game = *mathlabGame;
    for (MathlabAnim *anim : mathlabAnims()) {
        if (anim->shellDrawn) {
            continue;
        }
        switch (anim->kind) {
        case MathlabAnimKind::Bitmap: {
            const auto found = game.images.find(anim->name);
            if (found == game.images.end()) {
                break;
            }
            const SDL_Rect target{anim->x, anim->y, found->second.width,
                                  found->second.height};
            SDL_RenderCopy(renderer, found->second.texture.get(), nullptr,
                           &target);
            break;
        }
        case MathlabAnimKind::Smacker: {
            if (!anim->visible) {
                break;
            }
            MathlabClip *clip = mathlabClip(anim->name);
            if (clip == nullptr || !clip->texture ||
                clip->media.videoFrames.empty()) {
                break;
            }
            std::size_t index = 0;
            if (anim->frame >= 0 && !anim->playing) {
                index = std::min<std::size_t>(
                    static_cast<std::size_t>(anim->frame),
                    clip->media.videoFrames.size() - 1U);
            } else {
                const double time = anim->playing
                                        ? elapsed - anim->playStarted
                                        : anim->frozenAt;
                index = frameIndexAt(clip->media, time, anim->loop);
            }
            const auto &frame = clip->media.videoFrames[index];
            uploadVideoFrame(clip->texture.get(), frame);
            const SDL_Rect target{anim->x, anim->y, frame.width, frame.height};
            SDL_RenderCopy(renderer, clip->texture.get(), nullptr, &target);
            break;
        }
        case MathlabAnimKind::Text: {
            if (anim->text.empty()) {
                break;
            }
            // 0x004124e0: high byte 0 left, 0x100 centre, 0x200 right; low
            // byte 0 top, 1 centre, 2 bottom.
            const BitmapFont &font =
                game.fonts[static_cast<std::size_t>(anim->font)];
            const int width = textWidth(font, anim->text);
            const int horizontal = anim->textFlags & 0xff00;
            const int vertical = anim->textFlags & 0xff;
            const int x = horizontal == 0x200
                              ? anim->rect.x + anim->rect.w - width
                          : horizontal == 0x100
                              ? anim->rect.x + (anim->rect.w - width) / 2
                              : anim->rect.x;
            const int y = vertical == 2
                              ? anim->rect.y + anim->rect.h - font.height
                          : vertical == 1
                              ? anim->rect.y + (anim->rect.h - font.height) / 2
                              : anim->rect.y;
            SDL_Rect clip = anim->rect;
            SDL_RenderSetClipRect(renderer, &clip);
            drawText(renderer, font, anim->text, x, y);
            SDL_RenderSetClipRect(renderer, nullptr);
            break;
        }
        case MathlabAnimKind::Progress: {
            const auto found = game.images.find(anim->image);
            if (found == game.images.end()) {
                break;
            }
            // 0x00408bd1-0x00408c2c (flag 0x10): round(height x value) rows
            // copied from the bitmap's bottom.
            const float filled =
                static_cast<float>(found->second.height) * anim->progress;
            const int rows = static_cast<int>(filled > 0.0F ? filled + 0.5F
                                                            : filled - 0.5F);
            if (rows <= 0) {
                break;
            }
            const SDL_Rect source{0, found->second.height - rows,
                                  found->second.width, rows};
            const SDL_Rect target{anim->x + 1,
                                  anim->y + found->second.height - rows,
                                  found->second.width, rows};
            SDL_RenderCopy(renderer, found->second.texture.get(), &source,
                           &target);
            break;
        }
        }
    }
}

bool MathlabScene::executeMathlabDeveloperCommand(std::string_view command) {
    constexpr std::string_view mathlabControlPrefix = "mathlab:";
    if (activeScene != "mathlab" || !mathlabGame) {
        return false;
    }
    auto &game = *mathlabGame;
    if (!game.initialized) {
        return false;
    }
    const auto control = command.substr(mathlabControlPrefix.size());
    if (control == "start") {
        if (game.phase != 0) {
            return true;
        }
        removeMathlabAnim(game.start);
        startMathlabGame(0);
    } else if (control.rfind("seed:", 0) == 0) {
        // Reference captures reseed rand() where the original adapter does,
        // at the tank-capacity draw 0x0048069c.
        game.fixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control == "wait-rack") {
        // Holds the command queue until seven balls rest on the rail.
        return game.phase == 1 &&
               std::all_of(game.rack.begin(), game.rack.end(),
                           [](const MathlabBall &ball) {
                               return ball.active && ball.position == ball.limit;
                           });
    } else if (control.rfind("drag=", 0) == 0) {
        // drag=SX,SY,X,Y presses at (SX,SY) and releases at (X,Y).
        int points[4]{};
        if (std::sscanf(std::string(control.substr(5)).c_str(), "%d,%d,%d,%d",
                        &points[0], &points[1], &points[2], &points[3]) != 4) {
            throw std::runtime_error("mathlab:drag=SX,SY,X,Y");
        }
        beginMathlabInteraction(points[0], points[1]);
        moveMathlabInteraction(points[2], points[3]);
        endMathlabInteraction(points[2], points[3]);
    } else if (control.rfind("click=", 0) == 0) {
        int points[2]{};
        if (std::sscanf(std::string(control.substr(6)).c_str(), "%d,%d",
                        &points[0], &points[1]) != 2) {
            throw std::runtime_error("mathlab:click=X,Y");
        }
        beginMathlabInteraction(points[0], points[1]);
        endMathlabInteraction(points[0], points[1]);
    } else if (control.rfind("wait=", 0) == 0) {
        const int now = mathlabNow();
        if (!game.developerWaitUntil) {
            game.developerWaitUntil =
                now + std::stoi(std::string(control.substr(5)));
        }
        if (now < *game.developerWaitUntil) {
            return false;
        }
        game.developerWaitUntil.reset();
    } else if (control == "timeout") {
        if (game.phase == 1) {
            game.deadline = mathlabNow();
        }
    } else if (control == "status") {
        std::cout << "Math Lab status: phase " << game.phase << ", level "
                  << game.levelIndex + 1 << ", score " << game.score
                  << ", progress " << game.levelProgress << '/'
                  << game.levelGoal << ", result " << mathlabResult()
                  << ", tanks";
        for (std::size_t tank = 0; tank < 5; ++tank) {
            std::cout << ' ' << game.tanks[tank] << '/'
                      << game.capacities[tank];
        }
        std::cout << ", rack";
        for (const auto &ball : game.rack) {
            if (ball.active) {
                std::cout << ' ' << ball.value << '@' << ball.position << '/'
                          << ball.limit;
            }
        }
        std::cout << '\n';
    } else {
        throw std::runtime_error("Unknown developer Math Lab control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
