#include "game/app_buildabike.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#include "media/smacker.h"

// Build-a-Bike is a port of FGP.exe 0x00437d10-0x0043a210. Every rule below
// names the address it was read from; REVERSE_ENGINEERING.md ("Build-a-Bike")
// has the complete account. The game is driven by original 0xa8-byte
// animation descriptors, so the native port keeps one BuildabikeAnim per
// descriptor and mirrors each SCENE_AddAnim (0x00412840), SCENE_RemoveAnim
// (0x00412aa0) and SCENE_StopAnim (0x00412a60) call in the reference trace.

namespace fgp {
namespace {

// The eleven part families in the order of the name tables at 0x004d7648
// (broken_), 0x004d7674 (fixed_), 0x004d76a0 (line_) and 0x004d76cc (wall_).
const auto kParts = originalArray<std::string_view, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004d7648U + static_cast<std::uint32_t>(index * 4);
    const auto name = source.string(source.read<std::uint32_t>(address));
    if (!name.starts_with("broken_")) throw std::runtime_error("Invalid original bike part name");
    return name.substr(7);
});
// 0x0049dba0: wall descriptor layer per family.
const auto kWallLayers = originalArray<int, 11>(0x49dba0U);
// 0x0049dd80: wall descriptor positions (left/top of the 0x0049dd80 rows).
const auto kWallPositions = originalArray<SDL_Point, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049dd80U + static_cast<std::uint32_t>(index * 16);
    return source.read<SDL_Point>(address);
});
// 0x0049de30: wall count labels, drawn centred (flags 0x101 at 0x00438ea6).
const auto kCountRects = originalArray<SDL_Rect, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049de30U + static_cast<std::uint32_t>(index * 16);
    const auto r = source.read<std::array<int, 4>>(address);
    return SDL_Rect{r[0], r[1], r[2] - r[0], r[3] - r[1]};
});
// 0x0049dee0: the three bike rectangles; parts are placed from their
// top-left corner and the 170-pixel height is the platform travel.
const auto kBikeOrigins = originalArray<SDL_Point, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049dee0U + static_cast<std::uint32_t>(index * 16);
    return source.read<SDL_Point>(address);
});
constexpr int kPlatformTravel = 595 - 425;
// 0x0049df40: platform descriptor positions.
const auto kPlatformPositions = originalArray<SDL_Point, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049df40U + static_cast<std::uint32_t>(index * 16);
    return source.read<SDL_Point>(address);
});
// 0x0049df70: per-platform countdown text controls, right aligned (0x201).
const auto kTimerRects = originalArray<SDL_Rect, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049df70U + static_cast<std::uint32_t>(index * 16);
    const auto r = source.read<std::array<int, 4>>(address);
    return SDL_Rect{r[0], r[1], r[2] - r[0], r[3] - r[1]};
});
// 0x0049dfa0: score counter; 0x0049dfb0: energy progress bar origin.
constexpr SDL_Rect kScoreRect{555, 133, 73, 14};
constexpr SDL_Point kEnergyOrigin{513, 162};
// 0x0049db70/0x0049dbcc: slot bit and layer for the nine bike slots.
const auto kSlotLayers = originalArray<int, 9>(0x49dbccU);
// 0x0049db94: first hotspot of each platform's nine slots.
const auto kPlatformHotspots = originalArray<int, 3>(0x49db94U);
// 0x0049dbf0: conveyor hit margin per family (0x004389e5).
const auto kLineMargins = originalArray<int, 11>(0x49dbf0U);
// 0x0049dc20 and 0x0049dcd0: broken and fixed part offsets per family.
const auto kBrokenOffsets = originalArray<SDL_Point, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049dc20U + static_cast<std::uint32_t>(index * 16);
    return source.read<SDL_Point>(address);
});
const auto kFixedOffsets = originalArray<SDL_Point, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049dcd0U + static_cast<std::uint32_t>(index * 16);
    return source.read<SDL_Point>(address);
});
// 0x0049dfc0 and 0x0049dfe4: points for a repaired slot and the energy
// penalty for a broken or missing slot (0x00438840).
const auto kSlotPoints = originalArray<int, 9>(0x49dfc0U);
const auto kSlotPenalties = originalArray<int, 9>(0x49dfe4U);
// 0x0049e008: bikes completed before a second and third platform opens.
const auto kBikeThresholds = originalArray<int, 3>(0x49e008U);
// 0x0049e014: (bikes, energy) per difficulty. 0x00439623 selects 1 unless a
// tournament participant overrides it, so the normal game is medium.
constexpr int kStartBikes = 5;
constexpr int kStartEnergy = 75;
constexpr float kPlatformSeconds = 2.0F;   // 0x0049e040
constexpr float kConveyorSeconds = 15.0F;  // 0x0049e044
constexpr float kScoreRate = 250.0F;       // 0x0049e048
constexpr float kEnergyRate = 50.0F;       // 0x0049e04c
constexpr int kConveyorLeft = -100;        // 0x0049e050
constexpr int kConveyorY = 60;             // 0x0049e054
constexpr int kConveyorRight = 900;        // 0x0049e058
constexpr std::uint32_t kStartHotspot = 18;
constexpr std::uint32_t kMachineHotspot = 12;
constexpr std::uint32_t kCloneBase = 0x0054fea8U; // 0x00412c60
constexpr std::uint32_t kCloneStride = 0xa8U;

// 0x00438f10: the part family of each slot. Slots 1 and 8 draw rand(2).
int slotFamily(int slot, bool firstVariant) {
    switch (slot) {
    case 0: return 2;
    case 1: return firstVariant ? 10 : 1;
    case 2: return 4;
    case 3: return 5;
    case 4: return 6;
    case 5: return 7;
    case 6: return 8;
    case 7: return 9;
    case 8: return firstVariant ? 3 : 0;
    default: return -1;
    }
}

// 0x00439ec0: the slot a family is fitted to.
int familySlot(int family) {
    static constexpr std::array<int, 11> slots{{8, 1, 0, 8, 2, 3, 4, 5, 6, 7, 1}};
    return family >= 0 && family < 11 ? slots[static_cast<std::size_t>(family)]
                                       : -1;
}

// 0x00438fa0: the family named by any of the four name tables.
int partFamily(std::string_view name) {
    for (std::size_t family = 0; family < kParts.size(); ++family) {
        for (std::string_view prefix : {"broken_", "fixed_", "wall_", "line_"}) {
            if (name.size() == prefix.size() + kParts[family].size() &&
                name.substr(0, prefix.size()) == prefix &&
                name.substr(prefix.size()) == kParts[family]) {
                return static_cast<int>(family);
            }
        }
    }
    return -1;
}

// 0x00438900: strstr(name, "broken_").
bool brokenName(std::string_view name) {
    return name.find("broken_") != std::string_view::npos;
}

std::string partName(std::string_view prefix, int family) {
    return std::string(prefix) +
           std::string(kParts[static_cast<std::size_t>(family)]);
}

// 0x004d75f0/0x004d761c: hit masks exist for the two frames and the rack.
std::string hitMaskName(std::string_view name, int family) {
    if (family != 1 && family != 8 && family != 10) {
        return {};
    }
    return std::string(name) + "_hs";
}

std::string descriptorText(std::uint32_t address) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

// The "Counter" font is a 59-glyph strip (codes 0-58); the remaining codes
// have no glyph. The original draws its digits with a one-pixel gap, which
// the capture of the timer text "27.76" at x = 177, 189, 203 and 215 shows.
BitmapFont loadCounterFont(SDL_Renderer *renderer,
                           std::span<const std::uint8_t> bytes) {
    BitmapFont font;
    SurfacePtr source = loadBmp(bytes);
    SurfacePtr converted(SDL_ConvertSurfaceFormat(
        source.get(), SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) {
        throw std::runtime_error(SDL_GetError());
    }
    font.height = converted->h;
    const Uint32 separator = pixelValue(converted.get(), 0, 0);
    std::vector<int> separators;
    for (int x = 0; x < converted->w; ++x) {
        bool column = true;
        for (int y = 0; y < converted->h && column; ++y) {
            column = pixelValue(converted.get(), x, y) == separator;
        }
        if (column) {
            separators.push_back(x);
        }
    }
    separators.push_back(converted->w);
    for (std::size_t code = 0; code + 1 < separators.size() &&
                               code < font.glyphs.size(); ++code) {
        const int left = separators[code] + 1;
        font.glyphs[code] = SDL_Rect{left, 0, separators[code + 1] - left,
                                     font.height};
    }
    if (SDL_MUSTLOCK(converted.get())) {
        SDL_LockSurface(converted.get());
    }
    const Uint32 white = SDL_MapRGB(converted->format, 255, 255, 255);
    for (int y = 0; y < converted->h; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(
            static_cast<Uint8 *>(converted->pixels) +
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(converted->pitch));
        for (int x = 0; x < converted->w; ++x) {
            if ((row[x] | 0xff000000U) == (separator | 0xff000000U) ||
                (row[x] | 0xff000000U) == (white | 0xff000000U)) {
                row[x] = 0;
            }
        }
    }
    if (SDL_MUSTLOCK(converted.get())) {
        SDL_UnlockSurface(converted.get());
    }
    font.texture = makeTexture(renderer, converted.get());
    SDL_SetTextureBlendMode(font.texture.get(), SDL_BLENDMODE_BLEND);
    font.surface = std::move(converted);
    return font;
}

int counterTextWidth(const BitmapFont &font, std::string_view text) {
    int width = 0;
    for (const unsigned char code : text) {
        width += font.glyphs[code].w + 1;
    }
    return text.empty() ? 0 : width - 1;
}

void drawCounterText(SDL_Renderer *renderer, const BitmapFont &font,
                     std::string_view text, int x, int y) {
    for (const unsigned char code : text) {
        const SDL_Rect &source = font.glyphs[code];
        if (source.w > 0) {
            SDL_Rect destination{x, y, source.w, source.h};
            SDL_RenderCopy(renderer, font.texture.get(), &source, &destination);
        }
        x += source.w + 1;
    }
}

} // namespace

BuildabikeScene::BuildabikeGame BuildabikeScene::loadBuildabikeGame(
    Package &package) {
    BuildabikeGame game;
    const auto image = [&](const std::string &name) {
        auto &result = game.images[name];
        result.surface = loadBmp(package.data(
            package.find("bitmap\\" + name + ".bmp")));
        result.width = result.surface->w;
        result.height = result.surface->h;
        result.key = pixelValue(result.surface.get(), 0, 0);
        // Every keyed Build-a-Bike bitmap uses pure green; energy.bmp has
        // none and PROGRESS_Set (0x00408b48) clears its surface to 0x00ff00.
        SurfacePtr keyed = loadBmp(package.data(
            package.find("bitmap\\" + name + ".bmp")));
        SDL_SetColorKey(keyed.get(), SDL_TRUE,
                        SDL_MapRGB(keyed->format, 0, 255, 0));
        result.texture = makeTexture(renderer, keyed.get());
        SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_BLEND);
    };
    for (std::size_t family = 0; family < kParts.size(); ++family) {
        for (std::string_view prefix : {"wall_", "broken_", "fixed_"}) {
            image(partName(prefix, static_cast<int>(family)));
        }
    }
    for (const char *name :
         {"broken_blueframe_hs", "broken_rack_hs", "broken_redframe_hs",
          "fixed_blueframe_hs", "fixed_rack_hs", "fixed_redframe_hs",
          "platform_left", "platform_middle", "platform_right",
          "platform_cover", "start", "points", "energy"}) {
        image(name);
    }
    if (const auto *entry = package.findOptional("hotspots.bmp")) {
        game.images["hotspots"].surface = loadBmp(package.data(*entry));
    }
    const auto clip = [&](const std::string &name) {
        auto &result = game.clips[name];
        result.media = decodeActiveSceneClip(name);
        result.completion = originalSmackerCompletionDuration(result.media);
        if (!result.media.videoFrames.empty()) {
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
    for (std::size_t family = 0; family < kParts.size(); ++family) {
        clip(partName("line_", static_cast<int>(family)));
    }
    for (const char *name : {"lcogwheel", "rcogwheel", "wheel", "lever",
                             "string", "machine", "click", "drop1", "drop2",
                             "up", "down", "correct", "wrong"}) {
        clip(name);
    }
    game.counterFont = loadCounterFont(
        renderer, common.data(common.find("fonts\\Counter.bmp")));

    // Static descriptors, with the fields `fgp_inspect.py animations` reads.
    const auto bitmapAnim = [](std::uint32_t address, std::string name,
                               int x, int y, std::uint32_t hotspot) {
        BuildabikeAnim anim;
        anim.address = address;
        anim.name = std::move(name);
        anim.defaultX = x;
        anim.defaultY = y;
        anim.hotspot = hotspot;
        return anim;
    };
    const auto smackerAnim = [](std::uint32_t address, std::string name,
                                int x, int y, std::uint32_t hotspot,
                                bool visible, bool loop) {
        BuildabikeAnim anim;
        anim.address = address;
        anim.kind = BuildabikeAnimKind::Smacker;
        anim.name = std::move(name);
        anim.defaultX = x;
        anim.defaultY = y;
        anim.hotspot = hotspot;
        anim.visible = visible;
        anim.transparent = visible;
        anim.loop = loop;
        return anim;
    };
    game.points = bitmapAnim(0x004d7cf0U, "points", 511, 128, 0);
    game.cover = bitmapAnim(0x004d7c48U, "platform_cover", 0, 552, 0);
    game.start = bitmapAnim(0x004d7ba0U, "start", 259, 159, kStartHotspot);
    game.drag = bitmapAnim(0x004d7af8U, "<unknown>", 0, 0, 0);
    game.lcogwheel = smackerAnim(0x004d7708U, "lcogwheel", 0, 18, 0, true, true);
    game.rcogwheel =
        smackerAnim(0x004d77b0U, "rcogwheel", 684, 10, 0, true, true);
    game.wheel = smackerAnim(0x004d7858U, "wheel", 718, 335, 0, true, true);
    game.lever = smackerAnim(0x004d7900U, "lever", 604, 315, kMachineHotspot,
                             true, false);
    game.string = smackerAnim(0x004d79a8U, "string", 0, 0, 0, false, true);
    game.machine = smackerAnim(0x004d7a50U, "machine", 0, 0, 0, false, true);
    // 0x004392f0, 0x00439320 and 0x00439360: text and progress controls.
    for (std::size_t index = 0; index < 3; ++index) {
        auto &text = game.timerTexts[index];
        text.kind = BuildabikeAnimKind::Text;
        text.address = 0x00664350U + static_cast<std::uint32_t>(index) * 0xa8U;
        text.rect = kTimerRects[index];
        text.defaultLayer = 0x13;
        text.textFlags = 0x201;
        auto &platform = game.platformAnims[index];
        platform = bitmapAnim(
            0x006631d0U + static_cast<std::uint32_t>(index) * 0xc4U,
            index == 0 ? "platform_left"
                       : index == 1 ? "platform_middle" : "platform_right",
            kPlatformPositions[index].x, kPlatformPositions[index].y, 0);
    }
    game.energy.kind = BuildabikeAnimKind::Progress;
    game.energy.name = "";
    game.energy.address = 0;
    game.energy.defaultX = kEnergyOrigin.x - 1; // PROGRESS_Create 0x00408a1e
    game.energy.defaultY = kEnergyOrigin.y;
    game.scoreText.kind = BuildabikeAnimKind::Text;
    game.scoreText.address = 0x00665b68U;
    game.scoreText.rect = kScoreRect;
    game.scoreText.defaultLayer = 5;
    game.scoreText.textFlags = 0x201;
    for (std::size_t family = 0; family < 11; ++family) {
        auto &text = game.countTexts[family];
        text.kind = BuildabikeAnimKind::Text;
        text.address = 0x00664558U + static_cast<std::uint32_t>(family) * 0xa8U;
        text.rect = kCountRects[family];
        text.defaultLayer = 0x12;
        text.textFlags = 0x101;
        // 0x004393a0: wall descriptors carry hotspot family+1.
        game.walls[family] = bitmapAnim(
            0x00665430U + static_cast<std::uint32_t>(family) * 0xa8U,
            partName("wall_", static_cast<int>(family)),
            kWallPositions[family].x, kWallPositions[family].y,
            static_cast<std::uint32_t>(family + 1));
        game.walls[family].defaultLayer = kWallLayers[family];
        // 0x004395b0: conveyor slots are looping Smacker descriptors with
        // hotspot 0x30+i and layer 6+i.
        auto &line = game.lines[family];
        line = smackerAnim(
            0x00664c98U + static_cast<std::uint32_t>(family) * 0xb0U,
            "<unknown>", 0, 0, 0x30U + static_cast<std::uint32_t>(family),
            true, true);
        line.defaultLayer = 6 + static_cast<int>(family);
    }
    // 0x00439500: slot descriptors, hotspot base+slot and layer per slot.
    for (std::size_t platform = 0; platform < 3; ++platform) {
        for (std::size_t slot = 0; slot < 9; ++slot) {
            auto &anim = game.slots[platform * 9 + slot];
            anim = bitmapAnim(
                0x00661fe8U +
                    static_cast<std::uint32_t>(platform * 9 + slot) * 0xa8U,
                "<unknown>", kBikeOrigins[platform].x,
                kBikeOrigins[platform].y,
                static_cast<std::uint32_t>(kPlatformHotspots[platform]) +
                    static_cast<std::uint32_t>(slot));
            anim.defaultLayer = kSlotLayers[slot];
        }
    }
    std::cout << "Loaded executable-derived Build-a-Bike: platform, conveyor, "
                 "recycler and wall-store descriptors from 0x00437d10-"
                 "0x0043a210\n";
    return game;
}

void BuildabikeScene::initializeBuildabikeScene() {
    // Scene initializer 0x004391a0, in its call order. It runs on the first
    // update so the trace follows scene_change, as GAME_EnterScene's
    // logpoint precedes the original initializer.
    auto &game = *buildabikeGame;
    game.initialized = true;
    addBuildabikeAnim(game.points, -1, -1, 5);
    for (auto &text : game.timerTexts) {
        createBuildabikeTextControl(text);
    }
    addBuildabikeAnim(game.energy, -1, -1, 5);
    createBuildabikeTextControl(game.scoreText);
    for (auto &text : game.countTexts) {
        createBuildabikeTextControl(text);
    }
    // 0x00439389 calls 0x004392f0 a second time, replacing the timers.
    for (auto &text : game.timerTexts) {
        createBuildabikeTextControl(text);
    }
    for (auto &platform : game.platformAnims) {
        addBuildabikeAnim(platform, -1, -1, 0);
    }
    addBuildabikeAnim(game.cover, -1, -1, 0x11);
    addBuildabikeAnim(game.lcogwheel, -1, -1, 0);
    addBuildabikeAnim(game.rcogwheel, -1, -1, 0);
    addBuildabikeAnim(game.wheel, -1, -1, 0);
    addBuildabikeAnim(game.lever, -1, -1, 0);
    addBuildabikeAnim(game.start, -1, -1, 0x16);
}

std::vector<BuildabikeScene::BuildabikeAnim *>
BuildabikeScene::buildabikeAnims() {
    std::vector<BuildabikeAnim *> anims;
    if (!buildabikeGame) {
        return anims;
    }
    auto &game = *buildabikeGame;
    for (BuildabikeAnim *anim :
         {&game.points, &game.energy, &game.scoreText, &game.cover,
          &game.lcogwheel, &game.rcogwheel, &game.wheel, &game.lever,
          &game.string, &game.machine, &game.drag, &game.start}) {
        anims.push_back(anim);
    }
    for (auto &anim : game.timerTexts) anims.push_back(&anim);
    for (auto &anim : game.countTexts) anims.push_back(&anim);
    for (auto &anim : game.platformAnims) anims.push_back(&anim);
    for (auto &anim : game.walls) anims.push_back(&anim);
    for (auto &anim : game.slots) anims.push_back(&anim);
    for (auto &anim : game.lines) anims.push_back(&anim);
    for (auto &anim : game.clones) anims.push_back(&anim);
    std::erase_if(anims, [](const BuildabikeAnim *anim) {
        return !anim->active;
    });
    std::stable_sort(anims.begin(), anims.end(),
                     [](const BuildabikeAnim *left, const BuildabikeAnim *right) {
                         return left->layer != right->layer
                                    ? left->layer < right->layer
                                    : left->order < right->order;
                     });
    return anims;
}

std::vector<const BuildabikeScene::BuildabikeAnim *>
BuildabikeScene::buildabikeAnims() const {
    auto anims = const_cast<BuildabikeScene *>(this)->buildabikeAnims();
    return {anims.begin(), anims.end()};
}

int BuildabikeScene::buildabikeRandom(int bound) {
    // 0x00437d10: rand() % bound. Reference captures pin it to a constant.
    if (buildabikeGame && buildabikeGame->pinnedRandom) {
        return *buildabikeGame->pinnedRandom % bound;
    }
    return std::uniform_int_distribution<int>(0, bound - 1)(randomEngine);
}

void BuildabikeScene::addBuildabikeAnim(BuildabikeAnim &anim, int x, int y,
                                        int layer) {
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    anim.active = true;
    anim.playing = false;
    anim.x = x == -1 ? anim.defaultX : x;
    anim.y = y == -1 ? anim.defaultY : y;
    anim.layer = layer;
    anim.order = ++game.nextOrder;
    anim.frozenAt = 0.0;
    // 0x00412962-0x00412984: a visible descriptor with a hotspot is hit
    // tested by rectangle (mode 2) or, when keyed, by opaque pixel (mode 3).
    const bool surface = anim.kind == BuildabikeAnimKind::Bitmap ||
                         (anim.kind == BuildabikeAnimKind::Smacker &&
                          anim.visible);
    setBuildabikeHit(anim, surface && anim.hotspot != 0
                               ? (anim.transparent ? 3 : 2) : 0, 0);
    referenceTrace.emit("animation_add", {
        {"asset", anim.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", anim.hotspot}, {"conflict_mask", 0},
        {"descriptor", descriptorText(anim.address)}});
}

void BuildabikeScene::stopBuildabikeAnim(BuildabikeAnim &anim) {
    // SCENE_StopAnim 0x00412a60 is logged on entry for every descriptor.
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "stopped"},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.kind == BuildabikeAnimKind::Smacker && anim.playing) {
        anim.frozenAt = sceneElapsedNow() - anim.playStarted;
        anim.playing = false;
    }
    if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void BuildabikeScene::removeBuildabikeAnim(BuildabikeAnim &anim) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60,
    // so its logpoint precedes the nested StopAnim record.
    referenceTrace.emit("animation_remove", {
        {"asset", anim.name}, {"reason", "removed"},
        {"descriptor", descriptorText(anim.address)}});
    stopBuildabikeAnim(anim);
    anim.active = false;
    anim.playing = false;
}

void BuildabikeScene::playBuildabikeAnim(BuildabikeAnim &anim) {
    // SCENE_PlayAnim 0x004129d0 starts a Smacker descriptor's player.
    if (anim.kind != BuildabikeAnimKind::Smacker || !buildabikeGame) {
        return;
    }
    const auto found = buildabikeGame->clips.find(anim.name);
    if (found == buildabikeGame->clips.end()) {
        return;
    }
    const double now = sceneElapsedNow();
    const bool finished = !anim.loop && anim.frozenAt >= found->second.completion;
    anim.playStarted = now - (finished ? 0.0 : anim.frozenAt);
    anim.playing = true;
    anim.audioLoops = 0;
    if (!found->second.media.audioSamples.empty()) {
        if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(anim.audioHandle);
        }
        anim.audioHandle = audioMixer.addEffect(
            found->second.media, audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void BuildabikeScene::addAndPlayBuildabikeAnim(BuildabikeAnim &anim) {
    // 0x00412b90 adds an inactive descriptor at its own position and layer.
    if (!anim.active) {
        addBuildabikeAnim(anim, anim.defaultX, anim.defaultY, anim.defaultLayer);
    }
    playBuildabikeAnim(anim);
}

void BuildabikeScene::moveBuildabikeAnim(BuildabikeAnim &anim, int x, int y) {
    // 0x00412cc0 only changes the runtime position.
    anim.x = x;
    anim.y = y;
}

void BuildabikeScene::setBuildabikeHit(BuildabikeAnim &anim, int mode,
                                       int value, std::string mask) {
    // 0x00413670: +0x9c mode, +0xa0 margin or hotspot base, +0xa4 mask.
    anim.hitMode = mode;
    anim.hitValue = value;
    anim.hitMask = std::move(mask);
}

void BuildabikeScene::createBuildabikeTextControl(BuildabikeAnim &anim) {
    // 0x00407930 replaces an existing control's descriptor before adding it
    // at the rectangle's corner.
    if (anim.active) {
        removeBuildabikeAnim(anim);
    }
    anim.text.clear();
    addBuildabikeAnim(anim, anim.rect.x, anim.rect.y, anim.defaultLayer);
}

void BuildabikeScene::playBuildabikeSound(std::string_view name) {
    // 0x00438250 builds an invisible one-shot Smacker descriptor and plays a
    // clone of it (0x00412c20); clones take the first free 0xa8-byte slot
    // from 0x0054fea8 and are removed when they finish (0x00413259).
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    std::uint32_t address = kCloneBase;
    while (std::any_of(game.clones.begin(), game.clones.end(),
                       [&](const BuildabikeAnim &clone) {
                           return clone.address == address;
                       })) {
        address += kCloneStride;
    }
    BuildabikeAnim clone;
    clone.kind = BuildabikeAnimKind::Smacker;
    clone.address = address;
    clone.name = std::string(name);
    clone.visible = false;
    clone.transparent = false;
    clone.clone = true;
    game.clones.push_back(std::move(clone));
    addAndPlayBuildabikeAnim(game.clones.back());
}

void BuildabikeScene::finishBuildabikeClip(BuildabikeAnim &anim) {
    // 0x00413245-0x00413269: a finished non-looping player is stopped; a
    // clone is then removed, any other descriptor reports AnimationFinished.
    anim.frozenAt = sceneElapsedNow() - anim.playStarted;
    stopBuildabikeAnim(anim);
    if (anim.clone) {
        removeBuildabikeAnim(anim);
    } else {
        referenceTrace.emit("animation_stop", {
            {"asset", anim.name}, {"reason", "finished"},
            {"descriptor", descriptorText(anim.address)}});
    }
}

bool BuildabikeScene::buildabikeAnimHit(const BuildabikeAnim &anim, int x,
                                        int y, std::uint32_t &hotspot) const {
    // 0x00413750, by hit mode.
    if (anim.hitMode == 0 || !buildabikeGame) {
        return false;
    }
    const auto &game = *buildabikeGame;
    int width = 0;
    int height = 0;
    const BuildabikeImage *image = nullptr;
    if (anim.kind == BuildabikeAnimKind::Bitmap) {
        const auto found = game.images.find(anim.name);
        if (found == game.images.end()) {
            return false;
        }
        image = &found->second;
        width = image->width;
        height = image->height;
    } else if (anim.kind == BuildabikeAnimKind::Smacker) {
        const auto found = game.clips.find(anim.name);
        if (found == game.clips.end() ||
            found->second.media.videoFrames.empty()) {
            return false;
        }
        width = found->second.media.videoFrames.front().width;
        height = found->second.media.videoFrames.front().height;
    }
    const int localX = x - anim.x;
    const int localY = y - anim.y;
    if (anim.hitMode == 2) {
        const int margin = anim.hitValue;
        if (localX < -margin || localY < -margin || localX >= width + margin ||
            localY >= height + margin) {
            return false;
        }
        hotspot = anim.hotspot;
        return true;
    }
    if (localX < 0 || localY < 0 || localX >= width || localY >= height) {
        return false;
    }
    if (anim.hitMode == 3) {
        if (image != nullptr &&
            pixelValue(image->surface.get(), localX, localY) == image->key) {
            return false;
        }
        hotspot = anim.hotspot;
        return true;
    }
    if (anim.hitMode == 4) {
        const auto mask = game.images.find(anim.hitMask);
        if (mask == game.images.end() || localX >= mask->second.width ||
            localY >= mask->second.height) {
            return false;
        }
        const auto value = pixelValue(mask->second.surface.get(), localX, localY);
        if (value == 0) {
            return false;
        }
        hotspot = value + static_cast<std::uint32_t>(anim.hitValue);
        return true;
    }
    return false;
}

std::uint32_t BuildabikeScene::resolveBuildabikeHotspot(
    int x, int y, std::uint32_t exclude) const {
    // 0x004136c0: the topmost hit-testable descriptor wins; a hit on a
    // hotspot-0 descriptor blocks, an excluded hotspot is skipped, and
    // hotspots.bmp is the fallback.
    if (!buildabikeGame) {
        return 0;
    }
    const auto anims = buildabikeAnims();
    for (auto it = anims.rbegin(); it != anims.rend(); ++it) {
        std::uint32_t hotspot = 0;
        if (!buildabikeAnimHit(**it, x, y, hotspot)) {
            continue;
        }
        if (hotspot == 0) {
            return 0;
        }
        if (hotspot != exclude) {
            return hotspot;
        }
    }
    const auto mask = buildabikeGame->images.find("hotspots");
    if (mask == buildabikeGame->images.end() || !mask->second.surface ||
        x < 0 || y < 0 || x >= mask->second.surface->w ||
        y >= mask->second.surface->h) {
        return 0;
    }
    return pixelValue(mask->second.surface.get(), x, y);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
BuildabikeScene::buildabikeHotspotAt(int x, int y) const {
    if (!buildabikeGame || activeScene != "buildabike") {
        return std::nullopt;
    }
    const std::uint32_t hotspot = resolveBuildabikeHotspot(x, y, 0);
    if (hotspot == 0) {
        return std::nullopt;
    }
    return std::pair{hotspot, SDL_Rect{x, y, 1, 1}};
}

void BuildabikeScene::startBuildabikeGame() {
    // 0x00439620.
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    const double now = sceneElapsedNow();
    game.playing = true;
    game.bikes = kStartBikes;
    game.startedAt = now;
    game.lastUpdate = now;
    game.score = 0;
    updateBuildabikeScore(0.0F);
    game.maximumHealth = kStartEnergy;
    game.health = kStartEnergy;
    // 0x004396d0 redraws the bar from the shown value with zero energy.
    {
        const int health = game.health;
        game.health = 0;
        updateBuildabikeEnergy(0.0F);
        game.health = health;
    }
    // 0x00439730: every platform raised, empty, at state 4.
    for (int platform = 0; platform < 3; ++platform) {
        auto &state = game.platforms[static_cast<std::size_t>(platform)];
        state.hasBike = false;
        state.pendingBike = false;
        state.state = 4;
        state.serial = 0;
        setBuildabikePlatformPosition(platform, 0.0F);
    }
    // 0x00439710: three of every part on the wall.
    for (int family = 0; family < 11; ++family) {
        setBuildabikeWallCount(family, 3);
    }
    // 0x00439770: clear the 27 slot descriptors.
    for (auto &slot : game.slots) {
        if (slot.active) {
            removeBuildabikeAnim(slot);
        }
        slot.name = "<unknown>";
    }
    // 0x004397d0: conveyor slots spaced 1/11 apart.
    for (std::size_t index = 0; index < game.lines.size(); ++index) {
        if (game.lines[index].active) {
            removeBuildabikeAnim(game.lines[index]);
        }
        game.lines[index].name = "<unknown>";
        game.conveyor[index].progress =
            static_cast<float>(index) * (1.0F / 11.0F);
        game.conveyor[index].counter = 0;
    }
    // 0x00439850: empty recycling queue.
    for (auto &entry : game.queue) {
        entry = {};
    }
    game.queueHead = 0;
}

void BuildabikeScene::setBuildabikePlatformPosition(int platform,
                                                    float position) {
    // 0x00438170 moves the platform and 0x004387d0 its nine slots.
    auto &game = *buildabikeGame;
    auto &state = game.platforms[static_cast<std::size_t>(platform)];
    state.position = position;
    auto &anim = game.platformAnims[static_cast<std::size_t>(platform)];
    const int offset = static_cast<int>(
        static_cast<float>(kPlatformTravel) * position);
    moveBuildabikeAnim(anim, kPlatformPositions[static_cast<std::size_t>(platform)].x,
                       kPlatformPositions[static_cast<std::size_t>(platform)].y +
                           offset);
    for (int slot = 0; slot < 9; ++slot) {
        auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
        if (part.active) {
            moveBuildabikeAnim(part, part.x,
                               part.defaultY + anim.y - anim.defaultY);
        }
    }
}

void BuildabikeScene::setBuildabikePlatformState(int platform, int state) {
    // 0x004381e0: lowering plays "down" and judges a bike in place; rising
    // plays "up".
    auto &game = *buildabikeGame;
    auto &current = game.platforms[static_cast<std::size_t>(platform)];
    current.state = state;
    if (state == 0) {
        playBuildabikeSound("down");
        if (current.hasBike) {
            playBuildabikeSound(scoreBuildabikeBike(platform, false) ? "correct"
                                                                     : "wrong");
        }
    } else if (state == 1) {
        playBuildabikeSound("up");
    }
}

bool BuildabikeScene::scoreBuildabikeBike(int platform, bool apply) {
    // 0x00438840: a fitted part in a slot that started broken earns points;
    // a broken or missing part costs energy. True when nothing is wrong.
    auto &game = *buildabikeGame;
    const auto &state = game.platforms[static_cast<std::size_t>(platform)];
    int points = 0;
    int penalty = 0;
    for (int slot = 0; slot < 9; ++slot) {
        const auto &part =
            game.slots[static_cast<std::size_t>(platform * 9 + slot)];
        if (brokenName(part.name) || !part.active) {
            penalty += kSlotPenalties[static_cast<std::size_t>(slot)];
        } else if (state.repairMask & (1U << slot)) {
            points += kSlotPoints[static_cast<std::size_t>(slot)];
        }
    }
    if (apply) {
        game.score += points;
        game.health -= penalty;
    }
    return penalty == 0;
}

void BuildabikeScene::removeBuildabikeParts(int platform) {
    // 0x00438640.
    auto &game = *buildabikeGame;
    for (int slot = 0; slot < 9; ++slot) {
        auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
        if (part.active) {
            removeBuildabikeAnim(part);
        }
    }
}

void BuildabikeScene::createBuildabikePart(int platform, int slot,
                                           std::string name) {
    // 0x00438680.
    auto &game = *buildabikeGame;
    const int family = partFamily(name);
    auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
    if (part.active) {
        removeBuildabikeAnim(part);
    }
    part.name = std::move(name);
    if (family < 0) {
        return;
    }
    const bool broken = brokenName(part.name);
    const SDL_Point offset = broken
        ? kBrokenOffsets[static_cast<std::size_t>(family)]
        : kFixedOffsets[static_cast<std::size_t>(family)];
    part.defaultX = kBikeOrigins[static_cast<std::size_t>(platform)].x + offset.x;
    part.defaultY = kBikeOrigins[static_cast<std::size_t>(platform)].y + offset.y;
    addBuildabikeAnim(part, -1, -1, kSlotLayers[static_cast<std::size_t>(slot)]);
    const std::string mask = hitMaskName(part.name, family);
    if (!mask.empty()) {
        // Mask value 1 plus base + slot - 1 is the slot's own hotspot.
        setBuildabikeHit(part, 4,
                         kPlatformHotspots[static_cast<std::size_t>(platform)] +
                             slot - 1,
                         mask);
    } else {
        setBuildabikeHit(part, 2, 0);
    }
}

void BuildabikeScene::newBuildabikeBike(int platform) {
    // 0x00438510: 1-4 broken parts from a random slot onwards.
    auto &game = *buildabikeGame;
    auto &state = game.platforms[static_cast<std::size_t>(platform)];
    int broken = game.bikes / 3 - buildabikeRandom(3) + 1;
    broken = std::clamp(broken, 1, 4);
    const int first = buildabikeRandom(9);
    int slot = first;
    do {
        int family = slotFamily(slot, false);
        if (slot == 1 || slot == 8) {
            family = slotFamily(slot, buildabikeRandom(2) == 0);
        }
        const int remaining = broken--;
        createBuildabikePart(platform, slot,
                             partName(remaining > 0 ? "broken_" : "fixed_",
                                      family));
        if (broken >= 0) {
            state.repairMask |= static_cast<std::uint16_t>(1U << slot);
        } else {
            state.repairMask &= static_cast<std::uint16_t>(~(1U << slot));
        }
        slot = (slot + 1) % 9;
    } while (slot != first);
}

int BuildabikeScene::buildabikeBikeLimit() const {
    // 0x004384f0 and 0x00438049: one, two or three concurrent bikes.
    const int bikes = buildabikeGame ? buildabikeGame->bikes : 0;
    return static_cast<int>(std::count_if(
        kBikeThresholds.begin(), kBikeThresholds.end(),
        [&](int threshold) { return threshold <= bikes; }));
}

float BuildabikeScene::buildabikeTimerSeconds() const {
    // 0x00438480: 42 * (4 / (2n + 10) + 0.6), plus 0.4^k shares for each
    // further concurrent bike.
    const int bikes = buildabikeGame ? buildabikeGame->bikes : 0;
    float term = (4.0F / static_cast<float>(bikes * 2 + 10) + 0.6F) * 42.0F;
    float total = term;
    const int limit = buildabikeBikeLimit();
    for (int index = 1; index < limit; ++index) {
        term = static_cast<float>(static_cast<double>(term) * 0.4);
        total += term;
    }
    return total;
}

void BuildabikeScene::setBuildabikeWallCount(int family, int count) {
    // 0x00438e50.
    auto &game = *buildabikeGame;
    count = std::clamp(count, 0, 9);
    game.wallCounts[static_cast<std::size_t>(family)] = count;
    game.countTexts[static_cast<std::size_t>(family)].text = std::to_string(count);
    auto &wall = game.walls[static_cast<std::size_t>(family)];
    if (count > 0) {
        if (!wall.active) {
            addBuildabikeAnim(wall, -1, -1, wall.defaultLayer);
            setBuildabikeHit(wall, 2, 0);
        }
    } else if (wall.active) {
        removeBuildabikeAnim(wall);
    }
}

void BuildabikeScene::updateBuildabikePlatform(int platform, float dt) {
    // 0x004382d0.
    auto &game = *buildabikeGame;
    auto &state = game.platforms[static_cast<std::size_t>(platform)];
    if (state.state == 4) {
        state.timer -= dt;
        if (state.timer > 0.0F) {
            return;
        }
        setBuildabikePlatformPosition(platform, -state.timer);
        setBuildabikePlatformState(platform, 0);
        state.timer = 0.0F;
    } else if (state.state == 1) {
        state.position -= dt / kPlatformSeconds;
        if (state.position <= 0.0F) {
            const float timer = buildabikeTimerSeconds();
            state.timer = timer + kPlatformSeconds * state.position;
            state.state = 4;
            state.position = 0.0F;
        }
        setBuildabikePlatformPosition(platform, state.position);
    } else if (state.state == 3) {
        setBuildabikePlatformState(platform, 1);
        setBuildabikePlatformPosition(platform, 1.0F);
    } else if (state.state == 0) {
        state.position += dt / kPlatformSeconds;
        if (state.position >= 1.0F) {
            if (state.hasBike) {
                scoreBuildabikeBike(platform, true);
                removeBuildabikeParts(platform);
                if (game.health <= 0) {
                    buildabikeGameOver();
                    return;
                }
                state.hasBike = false;
                ++game.bikes;
            } else if (state.pendingBike) {
                newBuildabikeBike(platform);
                state.hasBike = true;
                state.pendingBike = false;
            }
            state.state = 3;
        }
        setBuildabikePlatformPosition(platform, state.position);
    }
}

void BuildabikeScene::scheduleBuildabikeBikes(float dt) {
    // 0x00438030: open platforms until the concurrent-bike limit is met.
    auto &game = *buildabikeGame;
    const int limit = buildabikeBikeLimit();
    int busy = static_cast<int>(std::count_if(
        game.platforms.begin(), game.platforms.end(),
        [](const BuildabikePlatform &state) {
            return state.hasBike || state.pendingBike;
        }));
    if (limit > busy) {
        const int first = buildabikeRandom(3);
        int platform = first;
        do {
            auto &state = game.platforms[static_cast<std::size_t>(platform)];
            if (!state.hasBike && !state.pendingBike) {
                if (state.state == 3) {
                    state.hasBike = true;
                    ++state.serial;
                    newBuildabikeBike(platform);
                    setBuildabikePlatformPosition(platform, 1.0F);
                } else if (state.state == 4) {
                    setBuildabikePlatformState(platform, 0);
                    state.pendingBike = true;
                    ++state.serial;
                }
                ++busy;
            }
            platform = (platform + 1) % 3;
        } while (platform != first && busy < limit);
    }
    for (int platform = 0; platform < 3 && game.playing; ++platform) {
        const auto &state = game.platforms[static_cast<std::size_t>(platform)];
        if (state.hasBike || state.state != 4) {
            updateBuildabikePlatform(platform, dt);
        }
    }
}

void BuildabikeScene::updateBuildabikeTimers() {
    // 0x00437da0 and 0x00437df0: "%.2f" below a minute, else "%02d %02d".
    auto &game = *buildabikeGame;
    for (std::size_t platform = 0; platform < 3; ++platform) {
        const auto &state = game.platforms[platform];
        auto &text = game.timerTexts[platform];
        if (!state.hasBike || state.state != 4) {
            text.text.clear();
            continue;
        }
        char buffer[32]{};
        if (state.timer < 60.0F) {
            std::snprintf(buffer, sizeof(buffer), "%.2f",
                          static_cast<double>(state.timer));
        } else {
            const int seconds = static_cast<int>(state.timer);
            std::snprintf(buffer, sizeof(buffer), "%02d %02d", seconds / 60,
                          seconds % 60);
        }
        text.text = buffer;
    }
}

std::optional<std::string> BuildabikeScene::takeBuildabikeRecycledPart() {
    // 0x00438bf0: the newest finished queue entry behind the head.
    auto &game = *buildabikeGame;
    int index = game.queueHead;
    do {
        index = index - 1 < 0 ? 21 : index - 1;
        auto &entry = game.queue[static_cast<std::size_t>(index)];
        if (entry.active && entry.timer <= 0.0F) {
            entry.active = false;
            return entry.name;
        }
    } while (index != game.queueHead);
    return std::nullopt;
}

void BuildabikeScene::updateBuildabikeConveyor(float dt) {
    // 0x00438920: each slot crosses from x=-100 to x=900 in 15 seconds and
    // picks up a recycled part when it wraps.
    auto &game = *buildabikeGame;
    bool moving = false;
    for (std::size_t index = 0; index < game.lines.size(); ++index) {
        auto &slot = game.conveyor[index];
        auto &line = game.lines[index];
        slot.progress += dt / kConveyorSeconds;
        if (slot.progress > 1.0F) {
            const auto part = takeBuildabikeRecycledPart();
            if (line.active) {
                removeBuildabikeAnim(line);
            }
            slot.progress = std::fmod(slot.progress, 1.0F);
            ++slot.counter;
            if (part) {
                const int family = partFamily(*part);
                line.name = partName("line_", family);
                addBuildabikeAnim(line, -1, -1, line.defaultLayer);
                setBuildabikeHit(line, 2,
                                 kLineMargins[static_cast<std::size_t>(family)]);
                playBuildabikeAnim(line);
            }
        }
        if (line.active) {
            moveBuildabikeAnim(
                line,
                static_cast<int>(static_cast<float>(kConveyorRight -
                                                    kConveyorLeft) *
                                 slot.progress) + kConveyorLeft,
                kConveyorY);
            moving = true;
        }
    }
    const bool draggingFromConveyor =
        game.drag.active && game.dragSource > 0x30 && game.dragSource < 0x3b;
    if (moving || draggingFromConveyor) {
        if (!game.lcogwheel.playing) playBuildabikeAnim(game.lcogwheel);
        if (!game.rcogwheel.playing) playBuildabikeAnim(game.rcogwheel);
        if (!game.string.playing) addAndPlayBuildabikeAnim(game.string);
    } else {
        if (game.lcogwheel.playing) stopBuildabikeAnim(game.lcogwheel);
        if (game.rcogwheel.playing) stopBuildabikeAnim(game.rcogwheel);
        if (game.string.playing) removeBuildabikeAnim(game.string);
    }
}

void BuildabikeScene::updateBuildabikeMachine(float dt) {
    // 0x00438b40: the recycler runs while any queued part is unfinished.
    auto &game = *buildabikeGame;
    bool running = false;
    for (auto &entry : game.queue) {
        if (entry.active && entry.timer > 0.0F) {
            entry.timer -= dt;
            running = true;
        }
    }
    if (running) {
        if (!game.wheel.playing) playBuildabikeAnim(game.wheel);
        if (!game.machine.playing) addAndPlayBuildabikeAnim(game.machine);
    } else {
        if (game.wheel.playing) stopBuildabikeAnim(game.wheel);
        if (game.machine.playing) removeBuildabikeAnim(game.machine);
    }
}

void BuildabikeScene::updateBuildabikeScore(float dt) {
    // 0x00437e90: the counter climbs 250 points per second, "%03d %03d".
    auto &game = *buildabikeGame;
    const float score = static_cast<float>(game.score);
    if (score < game.shownScore) {
        game.shownScore = 0.0F;
    }
    if (game.shownScore == score) {
        return;
    }
    if (game.shownScore < score) {
        game.shownScore += kScoreRate * dt;
    }
    if (game.shownScore > score) {
        game.shownScore = score;
    }
    const int shown = static_cast<int>(game.shownScore);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%03d %03d", shown / 1000,
                  shown % 1000);
    game.scoreText.text = buffer;
}

void BuildabikeScene::updateBuildabikeEnergy(float dt) {
    // 0x00437f60: the bar follows energy at 50 points per second and shows
    // shown/maximum of energy.bmp (PROGRESS_Set 0x00408b00).
    auto &game = *buildabikeGame;
    const float health = static_cast<float>(game.health);
    if (game.shownHealth == health) {
        return;
    }
    const float step = kEnergyRate * dt;
    if (health > game.shownHealth) {
        game.shownHealth = std::min(health, game.shownHealth + step);
    } else {
        game.shownHealth = std::max(health, game.shownHealth - step);
    }
    game.energy.progress = game.shownHealth > 0.0F && game.maximumHealth > 0
        ? game.shownHealth / static_cast<float>(game.maximumHealth)
        : 0.0F;
}

bool BuildabikeScene::enqueueBuildabikePart(const std::string &name) {
    // 0x00439f90: 22 recycling places, 25 seconds each (0x00439278).
    auto &game = *buildabikeGame;
    int index = game.queueHead;
    while (game.queue[static_cast<std::size_t>(index)].active) {
        index = (index + 1) % 22;
        if (index == game.queueHead) {
            return false;
        }
    }
    auto &entry = game.queue[static_cast<std::size_t>(index)];
    entry.name = partName("wall_", partFamily(name));
    entry.active = true;
    entry.timer = static_cast<float>(game.recycleSeconds);
    game.queueHead = (game.queueHead + 1) % 22;
    return true;
}

void BuildabikeScene::buildabikeGameOver() {
    // 0x00438c50, up to the blocking GAMEOVER modal at 0x00438d4e.
    auto &game = *buildabikeGame;
    const int seconds = static_cast<int>(sceneElapsedNow() - game.startedAt);
    game.playing = false;
    updateBuildabikeScore(static_cast<float>(game.score) / kScoreRate);
    updateBuildabikeEnergy(static_cast<float>(100 - game.health) / kEnergyRate);
    stopBuildabikeAnim(game.lcogwheel);
    stopBuildabikeAnim(game.rcogwheel);
    stopBuildabikeAnim(game.lever);
    stopBuildabikeAnim(game.wheel);
    if (game.drag.active) {
        removeBuildabikeAnim(game.drag);
    }
    game.dragging = false;
    game.gameOverModal = true;
    std::string message = language.get("buildabike", "ENDMESSAGE", "");
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer), message.c_str(), seconds / 60,
                  seconds % 60, game.score);
    showMessageWithTitle(language.get("interfaceh", "GAMEOVER",
                                      "Spillet er slutt!"),
                         buffer, MessageKind::BuildabikeGameOver);
}

void BuildabikeScene::finishBuildabikeGameOver() {
    // 0x00438d6a-0x00438e3a, after the shared score wrapper returns.
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    game.gameOverModal = false;
    for (auto &line : game.lines) {
        if (line.active) {
            removeBuildabikeAnim(line);
        }
    }
    for (auto &entry : game.queue) {
        entry.active = false;
    }
    for (int family = 0; family < 11; ++family) {
        setBuildabikeWallCount(family, 0);
        game.countTexts[static_cast<std::size_t>(family)].text.clear();
    }
    for (int platform = 0; platform < 3; ++platform) {
        removeBuildabikeParts(platform);
        setBuildabikePlatformPosition(platform, 0.0F);
        game.timerTexts[static_cast<std::size_t>(platform)].text.clear();
    }
    game.score = 0;
    game.scoreText.text.clear();
    addBuildabikeAnim(game.start, -1, -1, 0x16);
}

void BuildabikeScene::beginBuildabikeDrag(std::string name, int source,
                                          int tag, int x, int y) {
    // The pick-up handlers copy a name into the drag descriptor at
    // 0x004d7af8, add it at the pointer on layer 21, centre it (0x00412cf0)
    // and register it as movable (0x0040c1f0) with a tag.
    auto &game = *buildabikeGame;
    game.dragSource = source;
    game.drag.name = std::move(name);
    addBuildabikeAnim(game.drag, x, y, 0x15);
    const auto found = game.images.find(game.drag.name);
    const int width = found == game.images.end() ? 0 : found->second.width;
    const int height = found == game.images.end() ? 0 : found->second.height;
    moveBuildabikeAnim(game.drag, x - width / 2, y - height / 2);
    game.dragOffsetX = game.drag.x - x;
    game.dragOffsetY = game.drag.y - y;
    game.dragTag = tag;
    game.dragging = true;
}

void BuildabikeScene::pressBuildabike(std::uint32_t hotspot, int x, int y) {
    // 0x004399a0.
    auto &game = *buildabikeGame;
    if (game.drag.active) {
        removeBuildabikeAnim(game.drag);
        game.dragging = false;
    }
    const int id = static_cast<int>(hotspot);
    if (id >= 0x13 && id < 0x2e) {
        // 0x00439a40: borrow or remove a part from a bike at the top.
        const int platform = (id - 0x13) / 9;
        const int slot = (id - 0x13) % 9;
        auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
        const auto &state = game.platforms[static_cast<std::size_t>(platform)];
        if (part.active && state.hasBike && state.state == 4) {
            removeBuildabikeAnim(part);
            beginBuildabikeDrag(part.name, id, state.serial, x, y);
        }
    } else if (id >= 0x30 && id < 0x3b) {
        // 0x00439b20: take a repaired part off the conveyor.
        const auto index = static_cast<std::size_t>(id - 0x30);
        auto &line = game.lines[index];
        const int family = partFamily(line.name);
        if (line.active) {
            removeBuildabikeAnim(line);
            beginBuildabikeDrag(partName("wall_", family), id,
                                game.conveyor[index].counter, x, y);
        }
    } else if (id >= 1 && id <= 11) {
        // 0x00439bc0: take a part from the wall.
        const int family = id - 1;
        const int count = game.wallCounts[static_cast<std::size_t>(family)];
        if (count != 0) {
            setBuildabikeWallCount(family, count - 1);
            beginBuildabikeDrag(partName("wall_", family), id, 0, x, y);
        }
    } else {
        return;
    }
    playBuildabikeSound("click");
}

bool BuildabikeScene::dropBuildabikeOnBike(std::uint32_t hotspot) {
    // 0x00439d70: fit a part to the empty slot of its family.
    auto &game = *buildabikeGame;
    const int family = partFamily(game.drag.name);
    const int slot = familySlot(family);
    const int id = static_cast<int>(hotspot);
    const int platform = id >= 13 && id <= 15 ? id - 13 : (id - 0x13) / 9;
    auto &state = game.platforms[static_cast<std::size_t>(platform)];
    if (!state.hasBike || state.state != 4 || slot < 0) {
        return false;
    }
    auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
    if (part.active) {
        return false;
    }
    if (game.drag.active) {
        removeBuildabikeAnim(game.drag);
    }
    createBuildabikePart(
        platform, slot,
        partName(brokenName(game.drag.name) ? "broken_" : "fixed_", family));
    for (int index = 0; index < 9; ++index) {
        if (brokenName(game.slots[static_cast<std::size_t>(platform * 9 + index)]
                           .name)) {
            return true;
        }
    }
    // A bike without broken parts leaves early with five points per second
    // still on its timer (0x00439e77-0x00439e99).
    game.score += static_cast<int>(state.timer * 5.0F);
    setBuildabikePlatformState(platform, 0);
    return true;
}

bool BuildabikeScene::dropBuildabikeInMachine() {
    // 0x00439f40: any part can be recycled; the lever plays.
    auto &game = *buildabikeGame;
    if (!enqueueBuildabikePart(game.drag.name)) {
        return false;
    }
    if (game.drag.active) {
        removeBuildabikeAnim(game.drag);
    }
    playBuildabikeAnim(game.lever);
    return true;
}

bool BuildabikeScene::dropBuildabikeOnWall(std::uint32_t hotspot) {
    // 0x0043a050: only an unbroken part of the wall slot's own family.
    auto &game = *buildabikeGame;
    const int family = static_cast<int>(hotspot) - 1;
    if (brokenName(game.drag.name) || partFamily(game.drag.name) != family) {
        return false;
    }
    if (game.drag.active) {
        removeBuildabikeAnim(game.drag);
    }
    setBuildabikeWallCount(family,
                           game.wallCounts[static_cast<std::size_t>(family)] + 1);
    return true;
}

void BuildabikeScene::returnBuildabikeDrag() {
    // 0x0043a0c0, 0x0043a170 and 0x0043a1d0 return a refused part to its
    // source when that source still matches the drag tag.
    auto &game = *buildabikeGame;
    const int source = game.dragSource;
    if (!game.drag.active) {
        return;
    }
    if (source >= 0x13 && source < 0x2e) {
        const int platform = (source - 0x13) / 9;
        const int slot = (source - 0x13) % 9;
        auto &part = game.slots[static_cast<std::size_t>(platform * 9 + slot)];
        if (part.active) {
            return;
        }
        removeBuildabikeAnim(game.drag);
        const auto &state = game.platforms[static_cast<std::size_t>(platform)];
        if (state.hasBike && state.state == 4 && state.serial == game.dragTag) {
            createBuildabikePart(platform, slot, part.name);
        }
    } else if (source >= 0x30 && source < 0x3b) {
        const auto index = static_cast<std::size_t>(source - 0x30);
        removeBuildabikeAnim(game.drag);
        auto &line = game.lines[index];
        if (game.conveyor[index].counter == game.dragTag) {
            addAndPlayBuildabikeAnim(line);
            setBuildabikeHit(line, 2, 0);
        }
    } else if (source >= 1 && source <= 11) {
        removeBuildabikeAnim(game.drag);
        setBuildabikeWallCount(source - 1,
                               game.wallCounts[static_cast<std::size_t>(source - 1)] +
                                   1);
    }
}

bool BuildabikeScene::beginBuildabikeInteraction(int x, int y) {
    // Mouse-down 0x00439950: Start while idle, the pick-up handlers while
    // playing.
    if (!buildabikeGame) {
        return false;
    }
    auto &game = *buildabikeGame;
    const std::uint32_t hotspot = resolveBuildabikeHotspot(x, y, 0);
    if (game.playing) {
        pressBuildabike(hotspot, x, y);
        return true;
    }
    if (hotspot != kStartHotspot) {
        return false;
    }
    if (game.start.active) {
        removeBuildabikeAnim(game.start);
    }
    startBuildabikeGame();
    return true;
}

void BuildabikeScene::moveBuildabikeInteraction(int x, int y) {
    if (!buildabikeGame || !buildabikeGame->dragging) {
        return;
    }
    auto &game = *buildabikeGame;
    moveBuildabikeAnim(game.drag, x + game.dragOffsetX, y + game.dragOffsetY);
}

void BuildabikeScene::endBuildabikeInteraction(int x, int y) {
    // 0x0040c260 resolves the hotspot under the pointer and posts 0x10b;
    // 0x00439c70 dispatches it.
    if (!buildabikeGame || !buildabikeGame->dragging) {
        return;
    }
    auto &game = *buildabikeGame;
    game.dragging = false;
    if (!game.playing || !game.drag.active) {
        return;
    }
    moveBuildabikeAnim(game.drag, x + game.dragOffsetX, y + game.dragOffsetY);
    const std::uint32_t hotspot = resolveBuildabikeHotspot(x, y, 0);
    const int id = static_cast<int>(hotspot);
    bool accepted = false;
    if ((id >= 13 && id <= 15) || (id >= 0x13 && id < 0x2e)) {
        accepted = dropBuildabikeOnBike(hotspot);
    } else if (hotspot == kMachineHotspot) {
        accepted = dropBuildabikeInMachine();
    } else if (id >= 1 && id <= 11) {
        accepted = dropBuildabikeOnWall(hotspot);
    }
    if (accepted) {
        playBuildabikeSound("drop1");
        return;
    }
    returnBuildabikeDrag();
    playBuildabikeSound("drop2");
}

void BuildabikeScene::updateBuildabike(double elapsed) {
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    if (!game.initialized) {
        initializeBuildabikeScene();
    }
    // 0x00437d20: one tick of the running game. It runs before the
    // descriptor update in a frame: at 40984 ms of the first capture the
    // tick's "down" and "up" precede a finished "down" clone's stop.
    if (game.playing) {
        const float dt = static_cast<float>(elapsed - game.lastUpdate);
        game.lastUpdate = elapsed;
        scheduleBuildabikeBikes(dt);
        if (game.playing) {
            updateBuildabikeTimers();
            updateBuildabikeConveyor(dt);
            updateBuildabikeMachine(dt);
            updateBuildabikeScore(dt);
            updateBuildabikeEnergy(dt);
        }
    }
    // The descriptor update loop 0x00413200 finishes one-shot players. The
    // GAMEOVER modal (0x00438d4e) and the score wrapper (0x00438d62) block
    // the original's main loop, so a clone that was still playing finishes
    // only after `start` is back: the capture's "down" clone stops 16 ms
    // after that add instead of 30 ms after game over.
    if (game.gameOverModal) {
        return;
    }
    for (BuildabikeAnim *anim : buildabikeAnims()) {
        if (anim->kind != BuildabikeAnimKind::Smacker || !anim->playing) {
            continue;
        }
        const auto found = game.clips.find(anim->name);
        if (found == game.clips.end()) {
            continue;
        }
        const double played = elapsed - anim->playStarted;
        if (!anim->loop && played >= found->second.completion) {
            finishBuildabikeClip(*anim);
        } else if (anim->loop && found->second.media.videoDuration > 0.0 &&
                   !found->second.media.audioSamples.empty()) {
            const auto loops = static_cast<std::size_t>(
                played / found->second.media.videoDuration);
            if (loops > anim->audioLoops) {
                anim->audioLoops = loops;
                if (anim->audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
                    audioMixer.removeEffect(anim->audioHandle);
                }
                anim->audioHandle = audioMixer.addEffect(
                    found->second.media, audioVolumes[0] * sceneAudioGains[0]);
            }
        }
    }
    std::erase_if(game.clones, [](const BuildabikeAnim &clone) {
        return !clone.active;
    });
}

void BuildabikeScene::renderBuildabike(double elapsed) {
    if (!buildabikeGame) {
        return;
    }
    auto &game = *buildabikeGame;
    for (BuildabikeAnim *anim : buildabikeAnims()) {
        if (!anim->visible) {
            continue;
        }
        switch (anim->kind) {
        case BuildabikeAnimKind::Bitmap: {
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
        case BuildabikeAnimKind::Smacker: {
            const auto found = game.clips.find(anim->name);
            if (found == game.clips.end() || !found->second.texture ||
                found->second.media.videoFrames.empty()) {
                break;
            }
            const double time = anim->playing ? elapsed - anim->playStarted
                                              : anim->frozenAt;
            const auto index = frameIndexAt(found->second.media, time,
                                            anim->loop);
            const auto &frame = found->second.media.videoFrames[index];
            uploadVideoFrame(found->second.texture.get(), frame);
            const SDL_Rect target{anim->x, anim->y, frame.width, frame.height};
            SDL_RenderCopy(renderer, found->second.texture.get(), nullptr,
                           &target);
            break;
        }
        case BuildabikeAnimKind::Text: {
            if (anim->text.empty()) {
                break;
            }
            const int width = counterTextWidth(game.counterFont, anim->text);
            const int align = anim->textFlags & 0xff00;
            const int x = align == 0x200
                ? anim->rect.x + anim->rect.w - width
                : align == 0x100 ? anim->rect.x + (anim->rect.w - width) / 2
                                 : anim->rect.x;
            const int y = anim->rect.y +
                          (anim->rect.h - game.counterFont.height) / 2;
            drawCounterText(renderer, game.counterFont, anim->text, x, y);
            break;
        }
        case BuildabikeAnimKind::Progress: {
            const auto found = game.images.find("energy");
            if (found == game.images.end()) {
                break;
            }
            // 0x00408b72-0x00408bcf: width * value rounded half away from 0,
            // copied from the bitmap's left edge into the control.
            const float filled =
                static_cast<float>(found->second.width) * anim->progress;
            const int width = static_cast<int>(filled > 0.0F ? filled + 0.5F
                                                             : filled - 0.5F);
            if (width <= 0) {
                break;
            }
            const SDL_Rect source{0, 0, width, found->second.height};
            const SDL_Rect target{anim->x + 1, anim->y, width,
                                  found->second.height};
            SDL_RenderCopy(renderer, found->second.texture.get(), &source,
                           &target);
            break;
        }
        }
    }
}

bool BuildabikeScene::executeBuildabikeDeveloperCommand(
    std::string_view command) {
    constexpr std::string_view buildabikeControlPrefix = "buildabike:";
    if (activeScene != "buildabike" || !buildabikeGame) {
        return false;
    }
    auto &game = *buildabikeGame;
    const auto control = command.substr(buildabikeControlPrefix.size());
    if (control == "start") {
        if (game.start.active) {
            removeBuildabikeAnim(game.start);
        }
        startBuildabikeGame();
    } else if (control.rfind("random=", 0) == 0) {
        // Reference captures pin 0x00437d10's result, as the original adapter
        // does with a breakpoint on its return.
        game.pinnedRandom = std::stoi(std::string(control.substr(7)));
    } else if (control.rfind("bikes=", 0) == 0) {
        game.bikes = std::stoi(std::string(control.substr(6)));
    } else if (control.rfind("energy=", 0) == 0) {
        game.health = std::stoi(std::string(control.substr(7)));
    } else if (control.rfind("drag=", 0) == 0) {
        // drag=SX,SY,X,Y presses at (SX,SY) and releases at (X,Y), the
        // gesture reference scenarios send with drag_pointer.
        int points[4]{};
        if (std::sscanf(std::string(control.substr(5)).c_str(), "%d,%d,%d,%d",
                        &points[0], &points[1], &points[2], &points[3]) != 4) {
            throw std::runtime_error("buildabike:drag=SX,SY,X,Y");
        }
        beginBuildabikeInteraction(points[0], points[1]);
        moveBuildabikeInteraction(points[2], points[3]);
        endBuildabikeInteraction(points[2], points[3]);
    } else if (control == "wait-bike") {
        // Holds the command queue until a bike is up and counting down.
        return std::any_of(game.platforms.begin(), game.platforms.end(),
                           [](const BuildabikePlatform &platform) {
                               return platform.hasBike && platform.state == 4;
                           });
    } else if (control.rfind("wait=", 0) == 0) {
        const double now = sceneElapsedNow();
        if (!game.developerWaitUntil) {
            game.developerWaitUntil =
                now + std::stod(std::string(control.substr(5)));
        }
        if (now < *game.developerWaitUntil) {
            return false;
        }
        game.developerWaitUntil.reset();
    } else if (control == "timeout") {
        for (auto &platform : game.platforms) {
            if (platform.hasBike && platform.state == 4) {
                platform.timer = 0.0F;
            }
        }
    } else if (control == "status") {
        std::cout << "Build-a-Bike status: playing " << game.playing
                  << ", bikes " << game.bikes << ", score " << game.score
                  << ", energy " << game.health << '/' << game.maximumHealth;
        for (std::size_t platform = 0; platform < 3; ++platform) {
            const auto &state = game.platforms[platform];
            std::cout << ", platform " << platform << " state " << state.state
                      << " bike " << state.hasBike << " timer " << state.timer;
        }
        std::cout << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Build-a-Bike control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
