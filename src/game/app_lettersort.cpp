#include "game/app_lettersort.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "media/smacker.h"

namespace fgp {
namespace {

// country.ini order, also used by the executable's random-country index.
constexpr std::array<std::string_view, 12> kCountries{{
    "Norge", "Sverige", "Danmark", "England", "Tyskland", "Frankrike",
    "Spania", "Italia", "Ireland", "USA", "Canada", "Japan",
}};

struct LevelDefinition {
    int completedBags;
    int bagInterval;
    int letterInterval;
    int countries;
};

// FGP.exe 0x004db5a0: cumulative completed sacks, sack interval, letter
// interval, and the number of countries in play.
const auto kLevels = originalArray<LevelDefinition, 10>(0x004db5a0U);

// Every Smacker clip in Data/lettersort.bin. The click, drop, wrong, pickup,
// dropall, stamp and "pusher 1" files are 4x4 audio carriers that FGP.exe
// plays through clone template 0x4dbc28 (SCENE_PlayAnimClone 0x412c20).
enum Clip : std::size_t {
    Start, Bell, Bagout, BagoutGold, ClawGrab, ClawBrown, ClawGold,
    ClawRetry, SolDown, SolUp, SolDrop1, SolDrop2, SolPost1, SolPost2,
    SolJump, SolRocket, Click1, Drop1, Wrong1, Pickup1, Pickup2, Dropall1,
    Dropall2, Stamp1, PusherSound,
};
constexpr std::array<std::string_view, 25> kClipNames{{
    "s1_1_start", "s1_1_bell", "bagout", "bagoutgold", "clawgrab",
    "clawbrown", "clawgold", "clawretry", "soldown", "solopp", "soldrop1",
    "soldrop2", "solpost1", "solpost2", "soljump", "solrocket", "click 1",
    "drop 1", "wrong 1", "pickup 1", "pickup 2", "dropall 1", "dropall 2",
    "stamp 1", "pusher 1",
}};

std::size_t clipIndex(std::string_view name) {
    const auto found = std::find(kClipNames.begin(), kClipNames.end(), name);
    if (found == kClipNames.end()) {
        throw std::runtime_error("Unknown Letter Sort clip: " +
                                 std::string(name));
    }
    return static_cast<std::size_t>(std::distance(kClipNames.begin(), found));
}

// 0x49ea80: loose-letter rectangle (28,311)-(772,573). 0x43ed60 subtracts
// the 161x104 placement extent, so origins are x=28..610 and y=311..468.
constexpr int kTrayLeft = 28;
constexpr int kTrayTop = 311;
constexpr int kTrayRight = 772;
constexpr int kTrayBottom = 573;
constexpr int kPlacementWidth = 161;
constexpr int kPlacementHeight = 104;
// 0x49ea90: rack rectangle, left edge 27. A new sack starts at 27-84
// (0x43f988) and the pusher at 27-248, peaking at 27-50 and retracting to
// 27-164 (0x43f670).
constexpr int kRackLeft = 27;
constexpr int kSackWidth = 84;
constexpr int kSackHeight = 62;
constexpr int kSackY = 126;
constexpr int kPusherY = 115;
constexpr int kPusherWidth = 164;
constexpr int kPusherHeight = 73;
// 0x43f43c: the rightmost sack passing x=614 overflows the rack.
constexpr int kOverflowX = 614;
// 0x43e3a5: releases below y=188 land in the tray, not on a sack.
constexpr int kDropLineY = 188;
constexpr int kLetterWidth = 150;
constexpr int kLetterHeight = 97;
constexpr int kSackCapacity = 4; // 0x43dd00 stores 4 at 0x673388.
constexpr SDL_Rect kStartBounds{276, 232, 260, 88};
constexpr SDL_Rect kScoreBounds{46, 232, 108, 49};
constexpr SDL_Rect kPointsLabelBounds{45, 276, 109, 19};
constexpr SDL_Rect kTitleBounds{154, 3, 487, 43};

TexturePtr makeAnimationTexture(SDL_Renderer *renderer,
                                const DecodedMedia &media) {
    if (media.videoFrames.empty()) {
        return {};
    }
    const auto &frame = media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!texture) {
        throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                 SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

std::string twoDigits(int value) {
    std::ostringstream result;
    if (value < 10) {
        result << '0';
    }
    result << value;
    return result.str();
}

bool contains(const SDL_Rect &bounds, int x, int y) {
    const SDL_Point point{x, y};
    return SDL_PointInRect(&point, &bounds) == SDL_TRUE;
}

// 0x43e5f0: "ltr%s%02d", with "Gold" and variant 1 for a gold letter.
std::string letterAsset(bool gold, int country, int variant) {
    return gold ? std::string("ltrGold01")
                : "ltr" + std::string(kCountries[static_cast<std::size_t>(
                              country)]) +
                      twoDigits(variant + 1);
}

// 0x43e330: "Tiny %s" with the country name or "Gold".
std::string tinyAsset(bool gold, int country) {
    return gold ? std::string("Tiny Gold")
                : "Tiny " + std::string(kCountries[static_cast<std::size_t>(
                                country)]);
}

// 0x43e540: "bag%d%s" with the fill and "gold" or "".
std::string sackAsset(int fill, bool gold) {
    return "bag" + std::to_string(fill) + (gold ? "gold" : "");
}

std::string flagAsset(int country) {
    return "flg" + std::string(kCountries[static_cast<std::size_t>(country)]);
}

} // namespace

LettersortScene::LettersortGame LettersortScene::loadLettersortGame(Package &package) {
    LettersortGame game;
    const auto loadKeyed = [&](const std::string &asset) {
        SurfacePtr surface =
            loadGreenKeyedBmp(package.data(package.find(asset)));
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        return texture;
    };
    const auto loadOpaque = [&](const std::string &asset) {
        SurfacePtr surface = loadBmp(package.data(package.find(asset)));
        return makeTexture(renderer, surface.get());
    };

    for (int gold = 0; gold < 2; ++gold) {
        for (int fill = 0; fill <= kSackCapacity; ++fill) {
            game.bagTextures[static_cast<std::size_t>(gold)]
                            [static_cast<std::size_t>(fill)] =
                loadKeyed("bitmap\\" + sackAsset(fill, gold != 0) + ".bmp");
        }
    }
    for (std::size_t country = 0; country < kCountries.size(); ++country) {
        // These indexed flag bitmaps contain no chroma green. Asking SDL to
        // map green into their palette can select a real colour as the
        // nearest key (red on some flags), so keep every pixel opaque.
        game.flagTextures[country] = loadOpaque(
            "bitmap\\" + flagAsset(static_cast<int>(country)) + ".bmp");
        for (int variant = 0; variant < 12; ++variant) {
            game.letterTextures[country][static_cast<std::size_t>(variant)] =
                loadKeyed("bitmap\\" +
                          letterAsset(false, static_cast<int>(country),
                                      variant) +
                          ".bmp");
        }
        game.tinyTextures[country] = loadKeyed(
            "bitmap\\" + tinyAsset(false, static_cast<int>(country)) +
            ".bmp");
        game.levelTextures[country] = loadKeyed(
            "bitmap\\level" + twoDigits(static_cast<int>(country + 1)) +
            ".bmp");
    }
    game.goldLetterTexture = loadKeyed("bitmap\\ltrGold01.bmp");
    game.tinyGoldTexture = loadKeyed("bitmap\\Tiny Gold.bmp");
    game.borderTexture = loadKeyed("bitmap\\borderleft.bmp");
    game.frameTexture = loadKeyed("bitmap\\s1_1_frame.bmp");
    game.pusherTexture = loadKeyed("bitmap\\pusher.bmp");

    game.media.resize(kClipNames.size());
    for (std::size_t index = 0; index < kClipNames.size(); ++index) {
        auto &clip = game.media[index];
        // The deliveries, cameos and bags play under descriptors the scene
        // renames (0x4db8e0, 0x4db838, 0x4db640); all three set +0x48, so
        // scene_DrawAnim keys them on their top-left pixel. soldrop1 draws
        // white in palette index 0.
        const std::string_view name = kClipNames[index];
        const bool renamedKeyed =
            name.rfind("sol", 0) == 0 || name.rfind("bagout", 0) == 0;
        clip.media = decodeSmacker(
            activeSceneAnimationData(std::string(name)), true, true, true, 0,
            nullptr, SmackerIndexZeroAlpha::Transparent,
            renamedKeyed ? SmackerKeying::TopLeftRgb565
                         : activeSceneClipKeying(name));
        clip.texture = makeAnimationTexture(renderer, clip.media);
    }

    // Scene initializer 0x43dc30. The help/exit controls are shared scene
    // chrome; the minigame adds the parked start clip (hotspot 1, layer
    // -15), the left border, the rail frame and the parked bell (descriptor
    // 0x4db790, default (677,324), layer -45), all at their descriptor
    // positions (x=y=-1 in the call). Windows traces show the same four.
    game.start.media = Start;
    game.start.asset = "s1_1_start";
    game.start.x = kStartBounds.x;
    game.start.y = kStartBounds.y;
    game.start.layer = -15;
    game.start.added = true;
    game.start.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", "s1_1_start"}, {"x", -1}, {"y", -1}, {"layer", -15},
        {"hotspot", 1U}, {"conflict_mask", 0U}});
    referenceTrace.emit("animation_add", {
        {"asset", "borderleft"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"conflict_mask", 0U}});
    referenceTrace.emit("animation_add", {
        {"asset", "s1_1_frame"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"conflict_mask", 0U}});
    game.bell.media = Bell;
    game.bell.asset = "s1_1_bell";
    game.bell.x = 677;
    game.bell.y = 324;
    game.bell.layer = -45;
    game.bell.loop = true;
    game.bell.added = true;
    game.bell.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", "s1_1_bell"}, {"x", -1}, {"y", -1}, {"layer", -45},
        {"conflict_mask", 0U}});
    game.lastTimerSecond = lettersortAppClockMs() / 1000;

    std::cout << "Loaded executable-derived Letter Sort game: 12 countries, "
                 "145 envelopes, 10 sack states, and 10 pacing levels\n";
    return game;
}

int LettersortScene::lettersortAppClockMs() const {
    // 0x40c450 is the application millisecond clock. EVENT_Timer and the
    // pusher both read it, so neither is relative to the round or scene.
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - applicationClockStart)
            .count());
}

void LettersortScene::addLettersortClip(LettersortClip &clip,
                                        std::string_view asset, int x, int y,
                                        int layer, int tracedX, int tracedY,
                                        bool loop) {
    if (clip.audio != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.audio);
        clip.audio = LiveAudioMixer::kInvalidEffectHandle;
    }
    clip.added = true;
    clip.playing = false;
    clip.media = clipIndex(asset);
    clip.asset = std::string(asset);
    clip.x = x;
    clip.y = y;
    clip.layer = layer;
    clip.loop = loop;
    clip.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", clip.asset}, {"x", tracedX}, {"y", tracedY},
        {"layer", layer}, {"conflict_mask", 0U}});
}

void LettersortScene::startLettersortClip(LettersortClip &clip) {
    if (!lettersortGame || !clip.added) {
        return;
    }
    auto &media = lettersortGame->media[clip.media].media;
    if (clip.audio != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.audio);
    }
    clip.playing = true;
    clip.started = sceneElapsedNow();
    referenceTrace.emit("animation_start", {
        {"asset", clip.asset}, {"x", clip.x}, {"y", clip.y},
        {"layer", clip.layer}, {"loop", clip.loop}});
    clip.audio = audioMixer.addEffect(media,
                                      audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void LettersortScene::removeLettersortClip(LettersortClip &clip) {
    if (!clip.added) {
        return;
    }
    if (clip.audio != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.audio);
        clip.audio = LiveAudioMixer::kInvalidEffectHandle;
    }
    referenceTrace.emit("animation_remove", {
        {"asset", clip.asset}, {"reason", "removed"}});
    clip.added = false;
    clip.playing = false;
}

void LettersortScene::playLettersortSound(std::string_view name,
                                          int variants) {
    // 0x43e2a0 formats "%s %d" with a random 1..variants suffix and plays it
    // through the clone template at (0,0), layer 0.
    if (!lettersortGame) {
        return;
    }
    const int variant =
        variants > 1
            ? std::uniform_int_distribution<int>(1, variants)(randomEngine)
            : 1;
    LettersortClip clip;
    addLettersortClip(clip, std::string(name) + " " + std::to_string(variant),
                      0, 0, 0, 0, 0);
    startLettersortClip(clip);
    lettersortGame->sounds.push_back(clip);
}

void LettersortScene::removeLettersortLetter(std::size_t slot) {
    // 0x43e110 removes the player and frees the slot.
    auto &game = *lettersortGame;
    auto &letter = game.letters[slot];
    if (!letter.active) {
        return;
    }
    referenceTrace.emit("animation_remove", {
        {"asset", game.draggingLetter == slot
                      ? tinyAsset(letter.gold, letter.country)
                      : letterAsset(letter.gold, letter.country,
                                    letter.variant)},
        {"reason", "removed"}});
    if (game.draggingLetter == slot) {
        game.draggingLetter.reset();
    }
    letter.active = false;
}

void LettersortScene::removeLettersortSack(std::size_t sack) {
    // 0x43e090 removes the sack and then its flag.
    auto &bag = lettersortGame->sacks[sack];
    if (!bag.active) {
        return;
    }
    referenceTrace.emit("animation_remove", {
        {"asset", sackAsset(bag.fill, bag.gold)}, {"reason", "removed"}});
    referenceTrace.emit("animation_remove", {
        {"asset", flagAsset(bag.country)}, {"reason", "removed"}});
    bag.active = false;
}

void LettersortScene::resetLettersortRound() {
    // 0x43e020 clears the seven counters at 0x676c98 (playing, score, level,
    // letters created, sacks created, letters sorted, sacks completed),
    // removes every letter and sack, empties the claw queue and shows the
    // score as "0". The letter layer counter and the claws are left alone.
    auto &game = *lettersortGame;
    game.playing = false;
    game.score = 0;
    game.level = 0;
    game.createdLetters = 0;
    game.createdSacks = 0;
    game.sortedLetters = 0;
    game.completedSacks = 0;
    for (std::size_t slot = 0; slot < game.letters.size(); ++slot) {
        removeLettersortLetter(slot);
    }
    for (std::size_t sack = 0; sack < game.sacks.size(); ++sack) {
        removeLettersortSack(sack);
    }
    game.clawQueue.clear();
    game.scoreVisible = true;
}

void LettersortScene::clickLettersortStart() {
    // 0x43e274: hotspot 1 works only while the start clip is not playing.
    auto &game = *lettersortGame;
    if (!game.start.added || game.start.playing) {
        return;
    }
    playLettersortSound("click", 1);
    // 0x43dea0: reset, reload the country names, set the sack layer to -30
    // and start the parked clip. Its add happened at scene entry.
    resetLettersortRound();
    game.sackLayer = -30;
    startLettersortClip(game.start);
    SDL_SetWindowTitle(window,
                       "FGP Native — Brevsortering — level 1 · 0 points");
}

void LettersortScene::beginLettersortLevel(int level) {
    // 0x43ef00: store the level, start a Solan delivery, then 0x43f010
    // replaces the level badge (descriptor 0x4db988, default (626,63),
    // layer -45) and plays "stamp 1".
    auto &game = *lettersortGame;
    game.level = level;
    startLettersortDelivery();
    if (game.levelBadge) {
        referenceTrace.emit("animation_remove", {
            {"asset", game.levelBadgeAsset}, {"reason", "removed"}});
    }
    game.levelBadge = true;
    game.levelBadgeAsset = "level" + twoDigits(level + 1);
    game.levelBadgeSerial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", game.levelBadgeAsset}, {"x", -1}, {"y", -1},
        {"layer", -45}, {"conflict_mask", 0U}});
    playLettersortSound("stamp", 1);
    SDL_SetWindowTitle(
        window, ("FGP Native — Brevsortering — level " +
                 std::to_string(game.level + 1) + " · " +
                 std::to_string(game.score) + " points")
                    .c_str());
}

void LettersortScene::startLettersortDelivery() {
    // 0x43ef20 removes a running cameo, then picks the delivery: solpost1
    // at the first level, otherwise rand()%4 over solpost1 (294,241),
    // soldrop1 (260,225), solopp (294,242) and soldown (294,242). All use
    // layer -44 on descriptor 0x4db8e0, which is re-added even when a
    // delivery is still running.
    auto &game = *lettersortGame;
    if (game.cameo.added) {
        removeLettersortClip(game.cameo);
    }
    int choice = 0;
    if (game.level > 0) {
        choice = std::uniform_int_distribution<int>(0, 3)(randomEngine);
    }
    static constexpr std::array<std::tuple<std::string_view, int, int>, 4>
        kDeliveries{{{"solpost1", 294, 241}, {"soldrop1", 260, 225},
                     {"solopp", 294, 242}, {"soldown", 294, 242}}};
    const auto &[asset, x, y] = kDeliveries[static_cast<std::size_t>(choice)];
    addLettersortClip(game.delivery, asset, x, y, -44, x, y);
    startLettersortClip(game.delivery);
}

void LettersortScene::startLettersortCameo() {
    // 0x43f850: only while no delivery is added; replaces a running cameo
    // with rand()%2 over soljump (292,239) and solrocket (291,241), layer
    // -46 on descriptor 0x4db838.
    auto &game = *lettersortGame;
    if (game.delivery.added) {
        return;
    }
    if (game.cameo.added) {
        removeLettersortClip(game.cameo);
    }
    if (std::uniform_int_distribution<int>(0, 1)(randomEngine) == 0) {
        addLettersortClip(game.cameo, "soljump", 292, 239, -46, 292, 239);
    } else {
        addLettersortClip(game.cameo, "solrocket", 291, 241, -46, 291, 241);
    }
    startLettersortClip(game.cameo);
}

void LettersortScene::finishLettersortDelivery() {
    // 0x43eb30: remove the finished delivery. Unless its name ends in '2',
    // create ten letters and play "dropall 1|2" (0x43eba0); a name ending
    // in '1' then continues as the matching '2' clip, re-added at the
    // descriptor's updated default position (traced as x=y=-1).
    auto &game = *lettersortGame;
    const std::string asset = game.delivery.asset;
    const int x = game.delivery.x;
    const int y = game.delivery.y;
    game.delivery.audio = LiveAudioMixer::kInvalidEffectHandle;
    removeLettersortClip(game.delivery);
    if (asset.back() == '2') {
        return;
    }
    for (int count = 0; count < 10; ++count) {
        createLettersortLetter();
    }
    playLettersortSound("dropall", 2);
    if (asset.back() == '1') {
        std::string followup = asset;
        followup.back() = '2';
        addLettersortClip(game.delivery, followup, x, y, -44, -1, -1);
        startLettersortClip(game.delivery);
    }
}

void LettersortScene::placeLettersortLetter(std::size_t slot) {
    // 0x43e630: the letter takes the next global layer. When the counter
    // reaches 60, every letter moves down 60 layers and the bell is placed
    // one layer below the lowest letter.
    auto &game = *lettersortGame;
    if (lettersortLetterLayer < 0 || lettersortLetterLayer >= 60) {
        int lowest = std::numeric_limits<int>::max();
        for (auto &letter : game.letters) {
            if (!letter.active || &letter == &game.letters[slot]) {
                continue;
            }
            letter.layer -= 60;
            lowest = std::min(lowest, letter.layer);
        }
        // With no other letter 0x43e68c leaves INT_MAX - 1 for the bell.
        game.bell.layer = lowest - 1;
        lettersortLetterLayer -= 60;
    }
    auto &letter = game.letters[slot];
    letter.layer = lettersortLetterLayer++;
    letter.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", letterAsset(letter.gold, letter.country, letter.variant)},
        {"x", letter.x}, {"y", letter.y}, {"layer", letter.layer},
        {"hotspot", static_cast<std::uint32_t>(100U + slot)},
        {"conflict_mask", 0U}});
}

void LettersortScene::createLettersortLetter() {
    auto &game = *lettersortGame;
    // 0x43ebc0 asks 0x43ed30 for one of the 30 slots and simply returns when
    // none is free. A full tray is not a loss; only the rack overflow is.
    const auto free = std::find_if(game.letters.begin(), game.letters.end(),
                                   [](const LettersortLetter &letter) {
                                       return !letter.active;
                                   });
    if (free == game.letters.end()) {
        return;
    }
    const std::size_t slot =
        static_cast<std::size_t>(std::distance(game.letters.begin(), free));

    // 0x43ed60 positions first: up to 100 random origins in x=28..610,
    // y=311..468 whose 161x104 extent misses the 32x32 square around the
    // pointer (0x43ee70). The last candidate is kept when all overlap.
    const int pointerLeft = mouseX - 16;
    const int pointerTop = mouseY - 16;
    std::uniform_int_distribution<int> randomX(
        kTrayLeft, kTrayRight - kPlacementWidth - 1);
    std::uniform_int_distribution<int> randomY(
        kTrayTop, kTrayBottom - kPlacementHeight - 1);
    int chosenX = kTrayLeft;
    int chosenY = kTrayTop;
    for (int attempt = 0; attempt < 100; ++attempt) {
        chosenX = randomX(randomEngine);
        chosenY = randomY(randomEngine);
        // 0x43ee30 treats touching edges as overlapping.
        const bool separate =
            chosenY + kPlacementHeight < pointerTop ||
            chosenX + kPlacementWidth < pointerLeft ||
            pointerTop + 32 < chosenY || pointerLeft + 32 < chosenX;
        if (separate) {
            break;
        }
    }

    // 0x43eed0: letter 100 is gold, followed by every 120th letter.
    const bool gold = game.createdLetters >= 100 &&
                      (game.createdLetters - 100) % 120 == 0;
    // 0x43ec40/0x43ec70: a random country unlocked for the level with fewer
    // than eleven loose letters, 100 attempts, then the first such country
    // (0x43ecb0). Three countries times eleven exceeds the 30 slots, so a
    // country is always found.
    const int countries =
        kLevels[static_cast<std::size_t>(game.level)].countries;
    std::array<int, 12> looseCounts{};
    for (const auto &existing : game.letters) {
        if (existing.active) {
            ++looseCounts[static_cast<std::size_t>(existing.country)];
        }
    }
    int country = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int candidate = std::uniform_int_distribution<int>(
            0, countries - 1)(randomEngine);
        if (looseCounts[static_cast<std::size_t>(candidate)] < 11) {
            country = candidate;
            break;
        }
    }
    if (country < 0) {
        for (int candidate = 0; candidate < countries; ++candidate) {
            if (looseCounts[static_cast<std::size_t>(candidate)] < 11) {
                country = candidate;
                break;
            }
        }
    }
    if (country < 0) {
        return;
    }
    auto &letter = *free;
    letter = {};
    letter.active = true;
    letter.x = chosenX;
    letter.y = chosenY;
    letter.gold = gold;
    letter.country = country;
    letter.variant = std::uniform_int_distribution<int>(0, 11)(randomEngine);
    placeLettersortLetter(slot);
    ++game.createdLetters;
}

std::vector<std::size_t> LettersortScene::sortedLettersortSacks() const {
    // 0x43e800 lists active sack slots; 0x43ea30 sorts them by x.
    std::vector<std::size_t> order;
    for (std::size_t sack = 0; sack < lettersortGame->sacks.size(); ++sack) {
        if (lettersortGame->sacks[sack].active) {
            order.push_back(sack);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t left, std::size_t right) {
                         return lettersortGame->sacks[left].x <
                                lettersortGame->sacks[right].x;
                     });
    return order;
}

void LettersortScene::createLettersortSack() {
    auto &game = *lettersortGame;
    const auto free = std::find_if(game.sacks.begin(), game.sacks.end(),
                                   [](const LettersortSack &sack) {
                                       return !sack.active;
                                   });
    if (free == game.sacks.end()) {
        return;
    }
    const int countries =
        kLevels[static_cast<std::size_t>(game.level)].countries;
    std::array<int, 12> looseCounts{};
    int looseCount = 0;
    for (const auto &letter : game.letters) {
        if (letter.active) {
            ++looseCount;
            ++looseCounts[static_cast<std::size_t>(letter.country)];
        }
    }
    std::array<int, 12> sackCounts{};
    for (const auto &sack : game.sacks) {
        if (sack.active) {
            ++sackCounts[static_cast<std::size_t>(sack.country)];
        }
    }
    // 0x43fa40: below 22 loose letters at most four sacks per country
    // (0x43ec70 with limit 4); otherwise a country with at least three loose
    // letters (0x43fab0). Both retry 100 times and then take the first valid
    // country.
    const auto eligible = [&](int country) {
        return looseCount < 22
                   ? sackCounts[static_cast<std::size_t>(country)] < 4
                   : looseCounts[static_cast<std::size_t>(country)] > 2;
    };
    int country = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int candidate = std::uniform_int_distribution<int>(
            0, countries - 1)(randomEngine);
        if (eligible(candidate)) {
            country = candidate;
            break;
        }
    }
    if (country < 0) {
        for (int candidate = 0; candidate < countries; ++candidate) {
            if (eligible(candidate)) {
                country = candidate;
                break;
            }
        }
    }
    // The -1 fallback (0x43faf0) is unreachable: below 22 letters some
    // country holds fewer than four of the ten sacks, and from 22 letters
    // on one of at most twelve countries holds three letters.
    if (country < 0) {
        country = 0;
    }
    auto &sack = *free;
    sack = {};
    sack.active = true;
    sack.x = kRackLeft - kSackWidth;
    sack.y = kSackY;
    sack.country = country;
    sack.fill = 0;
    // 0x43fba0: the sack created after every 20 sacks is gold.
    sack.gold = game.createdSacks > 0 && game.createdSacks % 20 == 0;
    sack.serial = lettersortNextSerial++;
    // 0x43f9ef/0x43fa01: the sack at the sack layer and its flag at
    // (x, y+47), layer -15. Gold sacks get a flag too.
    referenceTrace.emit("animation_add", {
        {"asset", sackAsset(0, sack.gold)}, {"x", sack.x}, {"y", sack.y},
        {"layer", game.sackLayer}, {"conflict_mask", 0U}});
    referenceTrace.emit("animation_add", {
        {"asset", flagAsset(country)}, {"x", sack.x}, {"y", sack.y + 47},
        {"layer", -15}, {"conflict_mask", 0U}});
    ++game.createdSacks;
}

void LettersortScene::startLettersortPusher() {
    // 0x43fbc0 (re)starts the pusher at x=-221 with phase marker 1000, plays
    // "pusher 1" and adds the bitmap at layer -45.
    auto &game = *lettersortGame;
    game.pusherActive = true;
    game.pusherPhase = 1000;
    game.pusherX = kRackLeft - 248;
    game.pusherStartedMs = lettersortAppClockMs();
    playLettersortSound("pusher", 1);
    game.pusherSerial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", "pusher"}, {"x", game.pusherX}, {"y", kPusherY},
        {"layer", -45}, {"conflict_mask", 0U}});
}

void LettersortScene::updateLettersortPusher() {
    // Per-frame handler 0x43f3c0 -> 0x43f670.
    auto &game = *lettersortGame;
    if (!game.pusherActive) {
        return;
    }
    const int from = kRackLeft - 248;
    const int peak = kRackLeft - 50;
    const int back = kRackLeft - 164;
    float t = static_cast<float>(lettersortAppClockMs() -
                                 game.pusherStartedMs) *
              0.001F;
    // The first frame at or after one second is clamped to exactly 1.0, so
    // the stroke always reaches its peak.
    if (game.pusherPhase >= 0 && t >= 1.0F) {
        t = 1.0F;
        game.pusherPhase = -1000;
    }
    int x = 0;
    if (t <= 1.0F) {
        x = from + static_cast<int>(static_cast<float>(peak - from) * t +
                                    0.5F);
    } else if (t <= 2.0F) {
        x = peak + static_cast<int>(static_cast<float>(back - peak) *
                                        (t - 1.0F) +
                                    0.5F);
    } else {
        // 0x43f650: the stroke is over; remove the bitmap.
        game.pusherActive = false;
        referenceTrace.emit("animation_remove", {
            {"asset", "pusher"}, {"reason", "removed"}});
        return;
    }
    game.pusherX = x;

    // 0x43f4f0: walk the sacks left to right. Each one overlapped by the
    // previous right edge (the pusher's first) moves to that edge; the walk
    // stops at the first gap, so sacks never close up on their own.
    int previousRight = x + kPusherWidth;
    for (const std::size_t index : sortedLettersortSacks()) {
        auto &sack = game.sacks[index];
        if (previousRight <= sack.x) {
            break;
        }
        sack.x = previousRight;
        previousRight = sack.x + kSackWidth;
    }

    // 0x43f400/0x43f460: the rightmost sack beyond x=614 drops out of the
    // rack as bagout/bagoutgold (descriptor 0x4db640, default (614,112)) one
    // layer below the sacks, and 0x43f4e0 ends play. With sacks 84 apart
    // from x=141 this is the push that brings in the seventh sack.
    const auto order = sortedLettersortSacks();
    if (!order.empty() && game.sacks[order.back()].x > kOverflowX) {
        const std::size_t index = order.back();
        const bool gold = game.sacks[index].gold;
        removeLettersortSack(index);
        addLettersortClip(game.bagout, gold ? "bagoutgold" : "bagout",
                          kOverflowX, 112, game.sackLayer - 1, -1, -1);
        startLettersortClip(game.bagout);
        game.playing = false;
        SDL_SetWindowTitle(window, "FGP Native — Brevsortering — game over");
    }
}

void LettersortScene::setLettersortSackFill(std::size_t sack, int delta) {
    // 0x43e4d0 clamps the fill to 0..4 and swaps the bitmap by removing and
    // re-adding the sack at its position and the sack layer.
    auto &game = *lettersortGame;
    auto &bag = game.sacks[sack];
    const int fill = std::clamp(bag.fill + delta, 0, kSackCapacity);
    if (fill == bag.fill) {
        return;
    }
    referenceTrace.emit("animation_remove", {
        {"asset", sackAsset(bag.fill, bag.gold)}, {"reason", "removed"}});
    bag.fill = fill;
    bag.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", sackAsset(bag.fill, bag.gold)}, {"x", bag.x},
        {"y", bag.y}, {"layer", game.sackLayer}, {"conflict_mask", 0U}});
}

void LettersortScene::queueLettersortClaw(std::size_t sack) {
    // 0x43e8e0: queue a full sack unless a claw already holds it
    // (0x43e9b0) or it is already queued (0x43e930).
    auto &game = *lettersortGame;
    const bool clawed = std::any_of(
        game.claws.begin(), game.claws.end(),
        [&](const LettersortClaw &claw) {
            return claw.active && claw.sack == static_cast<int>(sack);
        });
    const bool queued =
        std::find(game.clawQueue.begin(), game.clawQueue.end(),
                  static_cast<int>(sack)) != game.clawQueue.end();
    if (!clawed && !queued) {
        game.clawQueue.push_back(static_cast<int>(sack));
    }
}

void LettersortScene::startLettersortClaw(std::size_t sack) {
    // 0x43fc30: a free one of nine claw slots grabs at (sack.x, 48), layer
    // -10 (0x43f2f0). With no free slot the popped sack is dropped.
    auto &game = *lettersortGame;
    const auto free = std::find_if(game.claws.begin(), game.claws.end(),
                                   [](const LettersortClaw &claw) {
                                       return !claw.active;
                                   });
    if (free == game.claws.end()) {
        return;
    }
    auto &claw = *free;
    claw.active = true;
    claw.state = 0;
    claw.sack = static_cast<int>(sack);
    claw.gold = game.sacks[sack].gold;
    claw.x = game.sacks[sack].x;
    claw.y = 48;
    addLettersortClip(claw.clip, "clawgrab", claw.x, claw.y, -10, claw.x,
                      claw.y);
    startLettersortClip(claw.clip);
}

void LettersortScene::finishLettersortClaw(std::size_t index) {
    // 0x43f0a0, by claw state.
    auto &game = *lettersortGame;
    auto &claw = game.claws[index];
    claw.clip.audio = LiveAudioMixer::kInvalidEffectHandle;
    const auto sackValid = [&] {
        return claw.sack >= 0 && claw.sack < 10 &&
               game.sacks[static_cast<std::size_t>(claw.sack)].active;
    };
    if (claw.state == 0) {
        // Grab finished: the sack goes only if it is still under the claw
        // (0x43f330); +100 at 0x43f1cc. Otherwise the claw retries.
        removeLettersortClip(claw.clip);
        if (sackValid() &&
            claw.x == game.sacks[static_cast<std::size_t>(claw.sack)].x) {
            claw.state = 2;
            removeLettersortSack(static_cast<std::size_t>(claw.sack));
            game.score += 100;
        } else {
            claw.state = 1;
        }
        addLettersortClip(claw.clip,
                          claw.state == 1
                              ? "clawretry"
                              : (claw.gold ? "clawgold" : "clawbrown"),
                          claw.x, claw.y, -10, claw.x, claw.y);
        startLettersortClip(claw.clip);
    } else if (claw.state == 1) {
        // Retry finished: grab again at the sack's current x if play goes on
        // and the sack still exists; otherwise release the claw (0x43f370).
        removeLettersortClip(claw.clip);
        if (game.playing && sackValid()) {
            claw.state = 0;
            claw.x = game.sacks[static_cast<std::size_t>(claw.sack)].x;
            claw.y = 48;
            addLettersortClip(claw.clip, "clawgrab", claw.x, claw.y, -10,
                              claw.x, claw.y);
            startLettersortClip(claw.clip);
        } else {
            claw.active = false;
        }
    } else {
        // Carry finished (0x43f0dd): release, count the completed sack and
        // advance the level once its cumulative target is reached (0x43f2a0).
        removeLettersortClip(claw.clip);
        claw.active = false;
        ++game.completedSacks;
        if (game.playing && game.level < 9 &&
            kLevels[static_cast<std::size_t>(game.level)].completedBags <=
                game.completedSacks) {
            beginLettersortLevel(game.level + 1);
        }
    }
    SDL_SetWindowTitle(
        window, ("FGP Native — Brevsortering — level " +
                 std::to_string(game.level + 1) + " · " +
                 std::to_string(game.score) + " points")
                    .c_str());
}

void LettersortScene::lettersortTimer(int second) {
    // 0x43f750, once per whole second of the application clock.
    auto &game = *lettersortGame;
    if (game.bell.playing) {
        // 0x43fce0: the bell rings until its countdown passes zero, then it
        // is stopped and the titled GAMEOVER message opens; the score table
        // follows it (0x41f9a0 with callback 0x43fd50).
        if (--game.bellCountdown < 0) {
            game.bell.playing = false;
            if (game.bell.audio != LiveAudioMixer::kInvalidEffectHandle) {
                audioMixer.removeEffect(game.bell.audio);
                game.bell.audio = LiveAudioMixer::kInvalidEffectHandle;
            }
            referenceTrace.emit("animation_stop", {
                {"asset", "s1_1_bell"}, {"reason", "stopped"}});
            showMessageWithTitle(
                language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
                language.get("lettersort", "GAMEOVER", "Spillet er slutt"),
                MessageKind::LettersortGameOver);
            return;
        }
    }
    if (!game.playing) {
        return;
    }
    const auto &level = kLevels[static_cast<std::size_t>(game.level)];
    // 0x43f79e..0x43f802: 120-second cameo, sack interval, letter interval.
    if (second % 120 == 0 && !game.delivery.added) {
        startLettersortCameo();
    }
    if (second % level.bagInterval == 0) {
        createLettersortSack();
        startLettersortPusher();
    }
    if (second % level.letterInterval == 0 && !game.delivery.playing) {
        createLettersortLetter();
    }
    // 0x43f807: pop one queued sack per second and send a claw for it.
    if (!game.clawQueue.empty()) {
        const int sack = game.clawQueue.front();
        game.clawQueue.erase(game.clawQueue.begin());
        if (sack >= 0 && sack < 10 &&
            game.sacks[static_cast<std::size_t>(sack)].active) {
            startLettersortClaw(static_cast<std::size_t>(sack));
        }
    }
}

void LettersortScene::updateLettersort(double elapsed) {
    // The GAMEOVER message and the score table are modal in FGP.exe.
    if (!lettersortGame || dialogMode != DialogMode::None) {
        return;
    }
    auto &game = *lettersortGame;
    updateLettersortPusher();

    const auto finished = [&](const LettersortClip &clip) {
        return clip.added && clip.playing && !clip.loop &&
               elapsed - clip.started >=
                   game.media[clip.media].media.videoDuration;
    };
    const auto emitFinished = [&](const LettersortClip &clip) {
        referenceTrace.emit("animation_stop", {
            {"asset", clip.asset}, {"reason", "completed"}});
    };

    // Clone sounds remove themselves when they end.
    for (auto it = game.sounds.begin(); it != game.sounds.end();) {
        if (finished(*it)) {
            emitFinished(*it);
            it->audio = LiveAudioMixer::kInvalidEffectHandle;
            removeLettersortClip(*it);
            it = game.sounds.erase(it);
        } else {
            ++it;
        }
    }
    if (game.bell.playing &&
        elapsed - game.bell.started >= game.media[Bell].media.videoDuration) {
        // The bell descriptor's mode 1 loops the ring until 0x43fce0 stops it.
        startLettersortClip(game.bell);
    }
    // Animation-finished dispatch 0x43ea80.
    if (finished(game.start)) {
        emitFinished(game.start);
        game.start.audio = LiveAudioMixer::kInvalidEffectHandle;
        removeLettersortClip(game.start);
        // 0x43eac1. UNCERTAIN: tournament play (0x419490/0x4194a0) starts
        // at level 3*n; native Letter Sort is not a tournament subgame yet.
        game.playing = true;
        beginLettersortLevel(0);
    }
    if (finished(game.delivery)) {
        emitFinished(game.delivery);
        finishLettersortDelivery();
    }
    if (finished(game.cameo)) {
        emitFinished(game.cameo);
        game.cameo.audio = LiveAudioMixer::kInvalidEffectHandle;
        removeLettersortClip(game.cameo);
    }
    if (finished(game.bagout)) {
        // 0x43eb03 then 0x43f3a0: ring the parked bell for a two-tick count.
        emitFinished(game.bagout);
        game.bagout.audio = LiveAudioMixer::kInvalidEffectHandle;
        removeLettersortClip(game.bagout);
        game.bellCountdown = 2;
        startLettersortClip(game.bell);
    }
    for (std::size_t index = 0; index < game.claws.size(); ++index) {
        if (game.claws[index].active && finished(game.claws[index].clip)) {
            emitFinished(game.claws[index].clip);
            finishLettersortClaw(index);
        }
    }

    // A second that elapses unseen is dropped, as in the original. Its timer
    // is EVENT_AddTimer(0, 250, 0) at 0x0043dc7c; the service routine catches
    // a late timer up one pass at a time (0x0040bf9c), but every catch-up call
    // carries the pass's clock, not the due time (0x0043dbd9 passes
    // [message+4]). 0x0043f750 divides that by 1000 and returns at 0x0043f76f
    // when it equals the last second it saw (0x00672570), so the catch-up calls
    // after a stall all name the current second and the skipped ones are never
    // evaluated. Comparing against the last second once per frame is that.
    const int second = lettersortAppClockMs() / 1000;
    if (second != game.lastTimerSecond) {
        game.lastTimerSecond = second;
        lettersortTimer(second);
    }
}

void LettersortScene::returnToLettersortAttract() {
    // Score-table callback 0x43fd50: reset the round, remove the level badge
    // and add the start clip again at layer -15.
    if (!lettersortGame) {
        return;
    }
    auto &game = *lettersortGame;
    resetLettersortRound();
    if (game.levelBadge) {
        referenceTrace.emit("animation_remove", {
            {"asset", game.levelBadgeAsset}, {"reason", "removed"}});
        game.levelBadge = false;
    }
    game.start.media = Start;
    game.start.asset = "s1_1_start";
    game.start.x = kStartBounds.x;
    game.start.y = kStartBounds.y;
    game.start.layer = -15;
    game.start.added = true;
    game.start.playing = false;
    game.start.serial = lettersortNextSerial++;
    referenceTrace.emit("animation_add", {
        {"asset", "s1_1_start"}, {"x", -1}, {"y", -1}, {"layer", -15},
        {"hotspot", 1U}, {"conflict_mask", 0U}});
}

bool LettersortScene::beginLettersortInteraction(int x, int y) {
    if (!lettersortGame || activeScene != "lettersort") {
        return false;
    }
    auto &game = *lettersortGame;
    const auto hotspot = lettersortHotspotAt(x, y);
    if (!hotspot) {
        return false;
    }
    // Mouse-down handler 0x43e230.
    if (hotspot->first == 1U) {
        clickLettersortStart();
        return true;
    }
    if (!game.playing || hotspot->first < 100U || hotspot->first >= 130U) {
        return false;
    }
    const std::size_t slot = hotspot->first - 100U;
    playLettersortSound("pickup", 2);
    // 0x43e2d0: remove the letter, rename its descriptor "Tiny <country>"
    // and add it at the pointer on layer 61 before the engine drag starts.
    auto &letter = game.letters[slot];
    referenceTrace.emit("animation_remove", {
        {"asset", letterAsset(letter.gold, letter.country, letter.variant)},
        {"reason", "removed"}});
    referenceTrace.emit("animation_add", {
        {"asset", tinyAsset(letter.gold, letter.country)}, {"x", x},
        {"y", y}, {"layer", 61},
        {"hotspot", static_cast<std::uint32_t>(100U + slot)},
        {"conflict_mask", 0U}});
    game.draggingLetter = slot;
    game.dragX = x;
    game.dragY = y;
    return true;
}

void LettersortScene::moveLettersortLetter(int x, int y) {
    if (!lettersortGame || !lettersortGame->draggingLetter) {
        return;
    }
    // UNCERTAIN: the engine drag at 0x40c1f0/0x40c300 was not traced; the
    // Tiny bitmap stays centred on the pointer, as 0x412cf0 centres it when
    // the drag starts.
    lettersortGame->dragX = x;
    lettersortGame->dragY = y;
}

void LettersortScene::endLettersortInteraction(int x, int y) {
    if (!lettersortGame || !lettersortGame->draggingLetter) {
        return;
    }
    auto &game = *lettersortGame;
    const std::size_t slot = *game.draggingLetter;
    auto &letter = game.letters[slot];
    const auto returnHome = [&] {
        // 0x43e750 re-adds the letter at its home on the top layer.
        referenceTrace.emit("animation_remove", {
            {"asset", tinyAsset(letter.gold, letter.country)},
            {"reason", "removed"}});
        game.draggingLetter.reset();
        placeLettersortLetter(slot);
    };

    // Drop handler 0x43e390.
    if (y > kDropLineY) {
        // 0x43e570: a release in the tray makes (x-80, y-104/2) the new
        // home, clamped into the placement area with -2..2 jitter
        // (0x43e6d0).
        playLettersortSound("drop", 1);
        int homeX = x - 80;
        int homeY = y - kPlacementHeight / 2;
        const int jitterX =
            std::uniform_int_distribution<int>(0, 4)(randomEngine) - 2;
        const int jitterY =
            std::uniform_int_distribution<int>(0, 4)(randomEngine) - 2;
        if (homeX < kTrayLeft) {
            homeX = kTrayLeft + jitterX;
        } else if (homeX > kTrayRight - kPlacementWidth) {
            homeX = kTrayRight - kPlacementWidth + jitterX;
        }
        if (homeY < kTrayTop) {
            homeY = kTrayTop + jitterY;
        } else if (homeY > kTrayBottom - kPlacementHeight) {
            homeY = kTrayBottom - kPlacementHeight + jitterY;
        }
        letter.x = homeX;
        letter.y = homeY;
        returnHome();
        return;
    }
    // 0x43e790: the first sack slot with left < x < left+84; no y test.
    std::optional<std::size_t> target;
    for (std::size_t sack = 0; sack < game.sacks.size(); ++sack) {
        const auto &bag = game.sacks[sack];
        if (bag.active && bag.x < x && x < bag.x + kSackWidth) {
            target = sack;
            break;
        }
    }
    if (!game.playing || !target ||
        game.sacks[*target].fill >= kSackCapacity) {
        playLettersortSound("drop", 1);
        returnHome();
        return;
    }
    auto &bag = game.sacks[*target];
    // 0x43e880 accepts an exact country match or a gold letter.
    if (!letter.gold && letter.country != bag.country) {
        playLettersortSound("wrong", 1);
        returnHome();
        return;
    }
    playLettersortSound("drop", 1);
    ++game.sortedLetters;
    const bool gold = letter.gold;
    removeLettersortLetter(slot);
    game.score += 10;
    // 0x43e45d: a gold letter fills the sack at once. No replacement letter
    // is created; the tray refills only on the timer.
    setLettersortSackFill(*target, gold ? kSackCapacity : 1);
    if (bag.fill >= kSackCapacity) {
        queueLettersortClaw(*target);
        if (bag.gold) {
            // 0x43e9f0: a full gold sack queues every sack, left to right.
            for (const std::size_t sack : sortedLettersortSacks()) {
                queueLettersortClaw(sack);
            }
        }
    }
    SDL_SetWindowTitle(
        window, ("FGP Native — Brevsortering — level " +
                 std::to_string(game.level + 1) + " · " +
                 std::to_string(game.score) + " points")
                    .c_str());
}

void LettersortScene::renderLettersort(double elapsed) {
    if (!lettersortGame) {
        return;
    }
    auto &game = *lettersortGame;
    // FGP.exe draws players in layer order; among equal layers the later
    // addition is drawn on top.
    struct Drawable {
        int layer;
        std::uint64_t serial;
        std::function<void()> draw;
    };
    std::vector<Drawable> drawables;
    const auto drawClip = [&](const LettersortClip &clip) {
        auto &animation = game.media[clip.media];
        // The 4x4 clone sounds are never visible (template 0x4dbc28).
        if (!animation.texture || animation.media.videoFrames.empty() ||
            animation.media.videoFrames.front().width <= 4) {
            return;
        }
        const double localTime =
            clip.playing ? std::max(0.0, elapsed - clip.started) : 0.0;
        const std::size_t frameIndex =
            frameIndexAt(animation.media, localTime, clip.loop);
        const auto &videoFrame = animation.media.videoFrames[frameIndex];
        uploadVideoFrame(animation.texture.get(), videoFrame);
        SDL_Rect destination{clip.x, clip.y, videoFrame.width,
                             videoFrame.height};
        SDL_RenderCopy(renderer, animation.texture.get(), nullptr,
                       &destination);
    };
    const auto addClip = [&](const LettersortClip &clip) {
        if (clip.added) {
            drawables.push_back(
                {clip.layer, clip.serial, [&drawClip, &clip] { drawClip(clip); }});
        }
    };
    const auto copy = [this](SDL_Texture *texture, SDL_Rect destination) {
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
    };

    drawables.push_back({0, 0, [&] {
        copy(game.borderTexture.get(), SDL_Rect{0, 99, 24, 108});
    }});
    drawables.push_back({0, 0, [&] {
        copy(game.frameTexture.get(), SDL_Rect{116, 48, 564, 17});
    }});
    addClip(game.start);
    addClip(game.bell);
    addClip(game.delivery);
    addClip(game.cameo);
    addClip(game.bagout);
    for (const auto &claw : game.claws) {
        if (claw.active) {
            addClip(claw.clip);
        }
    }
    if (game.pusherActive) {
        drawables.push_back({-45, game.pusherSerial, [&] {
            copy(game.pusherTexture.get(),
                 SDL_Rect{game.pusherX, kPusherY, kPusherWidth,
                          kPusherHeight});
        }});
    }
    for (const auto &sack : game.sacks) {
        if (!sack.active) {
            continue;
        }
        drawables.push_back({game.sackLayer, sack.serial, [&game, &copy, &sack] {
            copy(game.bagTextures[sack.gold ? 1U : 0U]
                                 [static_cast<std::size_t>(sack.fill)]
                                     .get(),
                 SDL_Rect{sack.x, sack.y, kSackWidth, kSackHeight});
        }});
        // 0x43f5f0 places the flag at (sack.x, sack.y + 47).
        drawables.push_back({-15, sack.serial, [&game, &copy, &sack] {
            copy(game.flagTextures[static_cast<std::size_t>(sack.country)]
                     .get(),
                 SDL_Rect{sack.x, sack.y + 47, 22, 15});
        }});
    }
    if (game.levelBadge) {
        drawables.push_back({-45, game.levelBadgeSerial, [&] {
            copy(game.levelTextures[static_cast<std::size_t>(game.level)]
                     .get(),
                 SDL_Rect{626, 63, 123, 118});
        }});
    }
    for (std::size_t slot = 0; slot < game.letters.size(); ++slot) {
        const auto &letter = game.letters[slot];
        if (!letter.active) {
            continue;
        }
        if (game.draggingLetter == slot) {
            drawables.push_back({61, lettersortNextSerial, [&game, &copy, &letter] {
                SDL_Texture *texture =
                    letter.gold
                        ? game.tinyGoldTexture.get()
                        : game.tinyTextures[static_cast<std::size_t>(
                                                letter.country)]
                              .get();
                int width = 0;
                int height = 0;
                SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
                copy(texture, SDL_Rect{game.dragX - width / 2,
                                       game.dragY - height / 2, width,
                                       height});
            }});
            continue;
        }
        drawables.push_back({letter.layer, letter.serial, [&game, &copy, &letter] {
            SDL_Texture *texture =
                letter.gold
                    ? game.goldLetterTexture.get()
                    : game.letterTextures[static_cast<std::size_t>(
                                              letter.country)]
                                         [static_cast<std::size_t>(
                                              letter.variant)]
                          .get();
            copy(texture, SDL_Rect{letter.x, letter.y, kLetterWidth,
                                   kLetterHeight});
        }});
    }
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable &left, const Drawable &right) {
                         return left.layer != right.layer
                                    ? left.layer < right.layer
                                    : left.serial < right.serial;
                     });
    for (const auto &drawable : drawables) {
        drawable.draw();
    }

    // The HUD text objects at 0x49eaa0 (score), 0x49eab0 (POINTS) and
    // 0x49eac0 (LONGNAME). The score stays blank until the first reset at
    // 0x43e020 writes "0".
    drawCenteredText(renderer, menuTitleFont,
                     language.get("lettersort", "LONGNAME",
                                  "Reodors dampdrevne postsorteringsmaskin"),
                     kTitleBounds);
    if (game.scoreVisible) {
        drawCenteredText(renderer, menuTitleFont, std::to_string(game.score),
                         kScoreBounds);
    }
    drawCenteredText(renderer, dialogFont,
                     language.get("lettersort", "POINTS", "Poeng"),
                     kPointsLabelBounds);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
LettersortScene::lettersortHotspotAt(int x, int y) const {
    if (!lettersortGame || activeScene != "lettersort") {
        return std::nullopt;
    }
    const auto &game = *lettersortGame;
    if (game.draggingLetter) {
        return std::nullopt;
    }
    // The engine hit test returns the topmost hotspot under the pointer: the
    // highest letter layer, later additions first among equal layers.
    // UNCERTAIN: whether 0x4136c0 tests the keyed bitmap or its rectangle;
    // the letter rectangle is used.
    std::optional<std::size_t> best;
    for (std::size_t slot = 0; slot < game.letters.size(); ++slot) {
        const auto &letter = game.letters[slot];
        if (!letter.active ||
            !contains(SDL_Rect{letter.x, letter.y, kLetterWidth,
                               kLetterHeight},
                      x, y)) {
            continue;
        }
        if (!best || letter.layer > game.letters[*best].layer ||
            (letter.layer == game.letters[*best].layer &&
             letter.serial > game.letters[*best].serial)) {
            best = slot;
        }
    }
    if (best) {
        const auto &letter = game.letters[*best];
        return std::pair{static_cast<std::uint32_t>(100 + *best),
                         SDL_Rect{letter.x, letter.y, kLetterWidth,
                                  kLetterHeight}};
    }
    if (game.start.added && contains(kStartBounds, x, y)) {
        return std::pair{1U, kStartBounds};
    }
    return std::nullopt;
}

bool LettersortScene::executeLettersortDeveloperCommand(std::string_view command) {
    constexpr std::string_view lettersortControlPrefix = "lettersort:";
    if (activeScene != "lettersort" || !lettersortGame) {
        return false;
    }
    auto &game = *lettersortGame;
    const std::string_view control =
        command.substr(lettersortControlPrefix.size());
    // Native-only fixtures for scripted checks. They install state directly
    // instead of waiting for the application-clock timers.
    const auto ensurePlaying = [&] {
        if (!game.playing) {
            if (game.start.added) {
                removeLettersortClip(game.start);
            }
            resetLettersortRound();
            game.sackLayer = -30;
            game.playing = true;
            beginLettersortLevel(0);
        }
    };
    const auto ensureSack = [&]() -> std::size_t {
        for (std::size_t sack = 0; sack < game.sacks.size(); ++sack) {
            if (game.sacks[sack].active && !game.sacks[sack].gold &&
                game.sacks[sack].fill < kSackCapacity) {
                return sack;
            }
        }
        createLettersortSack();
        for (std::size_t sack = 0; sack < game.sacks.size(); ++sack) {
            auto &bag = game.sacks[sack];
            if (bag.active && bag.x == kRackLeft - kSackWidth) {
                // The first rail slot, where a finished push leaves it.
                bag.x = kRackLeft - 50 + kPusherWidth;
                return sack;
            }
        }
        throw std::runtime_error("Letter Sort fixture has no free sack");
    };
    if (control == "start") {
        clickLettersortStart();
    } else if (control == "match" || control == "complete-bag") {
        ensurePlaying();
        const std::size_t sack = ensureSack();
        const int repeats = control == "complete-bag"
                                ? kSackCapacity - game.sacks[sack].fill
                                : 1;
        for (int count = 0; count < repeats; ++count) {
            auto letter = std::find_if(
                game.letters.begin(), game.letters.end(),
                [&](const LettersortLetter &candidate) {
                    return candidate.active && !candidate.gold &&
                           candidate.country == game.sacks[sack].country;
                });
            if (letter == game.letters.end()) {
                letter = std::find_if(game.letters.begin(),
                                      game.letters.end(),
                                      [](const LettersortLetter &candidate) {
                                          return !candidate.active;
                                      });
                if (letter == game.letters.end()) {
                    return false;
                }
                *letter = {};
                letter->active = true;
                letter->country = game.sacks[sack].country;
                letter->x = kTrayLeft;
                letter->y = kTrayTop;
                placeLettersortLetter(static_cast<std::size_t>(
                    std::distance(game.letters.begin(), letter)));
            }
            game.draggingLetter = static_cast<std::size_t>(
                std::distance(game.letters.begin(), letter));
            endLettersortInteraction(game.sacks[sack].x + kSackWidth / 2,
                                     game.sacks[sack].y + kSackHeight / 2);
        }
    } else if (control == "overflow") {
        // Six sacks on the rail, then the push that brings in a seventh.
        ensurePlaying();
        for (std::size_t index = 0; index < 6; ++index) {
            auto &sack = game.sacks[index];
            if (!sack.active) {
                sack = {};
                sack.active = true;
                sack.country = static_cast<int>(index % 3);
                sack.y = kSackY;
                sack.serial = lettersortNextSerial++;
            }
            sack.x = kRackLeft - 50 + kPusherWidth +
                     static_cast<int>(index) * kSackWidth;
        }
        createLettersortSack();
        startLettersortPusher();
    } else if (control == "status") {
        const auto sacks = std::count_if(
            game.sacks.begin(), game.sacks.end(),
            [](const LettersortSack &sack) { return sack.active; });
        const auto letters = std::count_if(
            game.letters.begin(), game.letters.end(),
            [](const LettersortLetter &letter) { return letter.active; });
        const auto claws = std::count_if(
            game.claws.begin(), game.claws.end(),
            [](const LettersortClaw &claw) { return claw.active; });
        std::cout << "Letter Sort status: level " << game.level + 1 << ", "
                  << game.score << " points, " << sacks << " sacks, "
                  << letters << " loose letters, " << claws << " claws, "
                  << game.clawQueue.size() << " queued, "
                  << game.completedSacks << " completed sacks, "
                  << (game.playing ? "playing" : "not playing") << "\n";
    } else {
        throw std::runtime_error(
            "Unknown developer Letter Sort control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
