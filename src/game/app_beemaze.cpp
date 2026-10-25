#include "game/app_beemaze.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

constexpr int kColumns = 22;
constexpr int kRows = 16;
constexpr int kCell = 32;
// Bees, items and the smoke cloud are placed at board pixel + (0x30, 0x4a)
// (0x0043217c-0x00432184, 0x004323eb-0x004323f0, 0x00430e38-0x00430e3b).
constexpr int kBoardX = 48;
constexpr int kBoardY = 74;
// Ludvig's 40x52 sprite descriptor 0x004cf990 sits at board pixel +
// (0x2b, 0x42) (0x00431f53-0x00431f5b, 0x0043107c-0x0043108a).
constexpr int kPlayerOffsetX = 43;
constexpr int kPlayerOffsetY = 66;
constexpr int kPlayerWidth = 40;
constexpr int kPlayerHeight = 52;
constexpr std::uint32_t kStartHotspot = 101U;
// 0x00432a50's timer tags are addresses; these stand in for them.
constexpr int kTagPlaying = 1;      // 0x006584bc
constexpr int kTagSmokeFlash = 2;   // 0x006598bc
constexpr int kTagSmoke = 3;        // descriptor 0x004cfa38
constexpr int kTagMushroom = 4;     // 0x006584b4
constexpr int kTagBeeStun = 10;     // 0x006584c8 + 4 * bee
// Bee movement modes chosen by 0x0042f520.
constexpr int kModeRandom = 0;
constexpr int kModePanic = 1;
constexpr int kModeTrail = 2;
constexpr int kModePissed = 3;
// Directions: 1 right, 2 down, 3 left, 4 up (key handler 0x0042f2c0 maps
// VK_LEFT/UP/RIGHT/DOWN to 3/4/1/2).
constexpr std::array<std::string_view, 4> kSheetNames{{
    "walkcycle", "honeycycle", "smokecycle", "honsmokecycle"}};
constexpr int kSheetWalk = 0;
constexpr int kSheetHoney = 1;
constexpr int kSheetSmoke = 2;
constexpr int kSheetHoneySmoke = 3;

// FGP.exe 0x004cd7e0: five 22x16 DWORD maps. 4 is wall; 1 marks the start
// area, 2 pickup terrain, 3 hive candidates, 5 bee-only passages and 6 open
// edge cells.
const auto kMaps = originalArray<std::array<std::uint32_t, 352>, 5>(0x004cd7e0U);

// 0x004cd7b8: where the arrow and 250_points clips show home, per level.
const auto kHome = originalArray<SDL_Point, 5>(0x4cd7b8U);

// 0x004cf978 lists each level's ambient descriptors (0x004cf360-0x004cf750).
// Mode +0x4c 1 loops from level start (0x00432590); mode 0 clips are rolled
// every five seconds (0x0042f460).
struct AmbientDescriptor {
    std::string_view name;
    int x;
    int y;
    bool looping;
};
const auto kAmbient = originalArray<AmbientDescriptor, 7>([](const OriginalData &source, std::size_t index) {
    constexpr std::array<std::uint32_t, 7> descriptors{
        0x004cf360U, 0x004cf408U, 0x004cf4b0U, 0x004cf558U, 0x004cf600U, 0x004cf6a8U, 0x004cf750U};
    const auto address = descriptors[index];
    return AmbientDescriptor{source.string(address + 0), source.read<int>(address + 0x54), source.read<int>(address + 0x58), source.read<int>(address + 0x4c) != 0};
});
constexpr std::array<std::pair<std::size_t, std::size_t>, 5> kAmbientRange{{
    {0, 2}, {2, 4}, {4, 5}, {5, 6}, {6, 7}}};

int mapValue(int level, int x, int y) {
    return kMaps[static_cast<std::size_t>(level % 5)]
                [static_cast<std::size_t>(y * kColumns + x)];
}

// 0x0042f7a0: cells a bee may enter (everything but walls).
bool beePassable(int level, int x, int y) {
    if (x < 0 || x >= kColumns || y < 0 || y >= kRows) return false;
    const int value = mapValue(level, x, y);
    return value == 1 || value == 2 || value == 3 || value == 5 ||
           value == 6;
}

// 0x00431ad0: cells Ludvig may enter; value 5 is a bee-only passage.
bool playerPassable(int level, int x, int y) {
    if (x < 0 || x > 21 || y < 0 || y > 15) return false;
    const int value = mapValue(level, x, y);
    return value == 1 || value == 2 || value == 3 || value == 6;
}

// 0x004314a0 is called with Ludvig's current cell. It allows the left and
// up wraps from column 0 / row 0 onto value-6 edge cells; its right and down
// tests compare against column 22 and row 16, which no cell has.
bool playerWraps(int level, int x, int y, int direction) {
    if (direction == 2 || direction == 4) {
        if (y == 0) return mapValue(level, x, 0) == 6;
        if (y == 16) return mapValue(level, x, 15) == 6;
        return false;
    }
    if (direction == 1 || direction == 3) {
        if (x == 0) return mapValue(level, 0, y) == 6;
        if (x == 22) return mapValue(level, 21, y) == 6;
    }
    return false;
}

int cellIndex(int x, int y) { return x * kRows + y; }

TexturePtr mediaTexture(SDL_Renderer *renderer, const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame = media.videoFrames.front();
    TexturePtr result(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!result) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(result.get(), SDL_BLENDMODE_BLEND);
    return result;
}

} // namespace

BeemazeScene::BeemazeGame BeemazeScene::loadBeemazeGame(Package &package) {
    BeemazeGame game;
    const auto texture = [&](std::string asset, bool keyed = true) {
        SurfacePtr surface = keyed
            ? loadGreenKeyedBmp(package.data(package.find(asset)))
            : loadBmp(package.data(package.find(asset)));
        TexturePtr result = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(result.get(), SDL_BLENDMODE_BLEND);
        return result;
    };
    // Descriptor 0x004d0560 (+0x48 = 0) shows the level bitmap opaque.
    for (int level = 0; level < 5; ++level) {
        game.levelTextures[static_cast<std::size_t>(level)] = texture(
            "bitmap\\level" + std::to_string(level + 1) + ".bmp", false);
    }
    // Item and life descriptors set transparent flag +0x48
    // (0x004323fe, 0x00432015).
    game.stickTexture = texture("bitmap\\stick.bmp");
    game.nutTexture = texture("bitmap\\nut.bmp");
    game.mushTexture = texture("bitmap\\Mush.bmp");
    game.berriesTexture = texture("bitmap\\berries.bmp");
    game.gunTexture = texture("bitmap\\gun.bmp");
    game.honeyTexture = texture("bitmap\\honey.bmp");
    game.lifeTexture = texture("bitmap\\life.bmp");
    game.startTexture = texture("bitmap\\start_icon.bmp");
    for (std::size_t sheet = 0; sheet < kSheetNames.size(); ++sheet) {
        game.playerSheets[sheet] = texture(
            "bitmap\\" + std::string(kSheetNames[sheet]) + ".bmp");
    }

    const auto placed = [&](std::string name) {
        HopscotchMedia result;
        result.media = decodeActiveSceneClip(std::move(name), true, false);
        result.texture = mediaTexture(renderer, result.media);
        return result;
    };
    game.bees = placed("bees");
    game.beeSleep = placed("bee_sleep");
    game.smoke = placed("smoke");
    game.points250 = placed("250_points");
    game.arrow = placed("arrow");
    for (std::size_t index = 0; index < kAmbient.size(); ++index) {
        game.ambient[index] = placed(std::string(kAmbient[index].name));
    }
    // 0x00431a00 clones "<family>_<n>"; 0x00432880 clones "cry". Video is
    // decoded too: the clone ends when its Smacker player finishes.
    for (std::string_view name : {"berries_1", "nuts_1", "nuts_2",
                                  "mushroom_1", "smoke_1", "honey_1",
                                  "cry"}) {
        game.sounds.emplace(
            std::string(name),
            decodeSmacker(activeSceneAnimationData(std::string(name)), true,
                          true, true));
    }
    game.timers.reserve(16);
    // Scene initializer 0x00432d50 adds start_icon at (-1,-1), layer 150,
    // unless a tournament starts play at once (0x00432e64-0x00432e73).
    std::cout << "Loaded executable-derived Bee Maze: five 22x16 maps, "
                 "pixel movement, recovered bee modes and timers\n";
    return game;
}

int BeemazeScene::beemazeRandom() {
    // Statically linked MSVC rand() at 0x0048d323.
    if (!beemazeRandomSeeded) {
        beemazeRandomState = static_cast<std::uint32_t>(randomEngine());
        beemazeRandomSeeded = true;
    }
    beemazeRandomState = beemazeRandomState * 0x343fdU + 0x269ec3U;
    return static_cast<int>((beemazeRandomState >> 16) & 0x7fffU);
}

// The application clock 0x0040c450 is timeGetTime-based milliseconds. The
// scene runs one timer service and one 0x112 update per idle pump pass
// (0x0040d1c0), whose rate is a runtime property. Reference captures put
// Bee Maze's movement at one step per 21 ms: bees take 672 ms per cell
// (1 px = trunc(21 * 128 * 0.6) / 1000, 32 steps) and Ludvig 336-343 ms
// (2 px = 21 * 128 / 1000, 16 steps). Native therefore runs a pass every
// 10.5 ms, so the 20 ms gates open on every second pass at 21 ms.
constexpr std::uint64_t kPassNumerator = 21U;   // pass k at k * 21 / 2 ms
constexpr std::uint64_t kPassDenominator = 2U;
int BeemazeScene::beemazeClock() {
    if (beemazeGame && beemazeGame->passTime) return beemazePassClock();
    return 100000 + static_cast<int>(std::floor(sceneElapsedNow() * 1000.0));
}

double BeemazeScene::beemazeNow() {
    if (beemazeGame && beemazeGame->passTime) return *beemazeGame->passTime;
    return sceneElapsedNow();
}

int BeemazeScene::beemazePassClock() const {
    return 100000 + static_cast<int>(
        (beemazeGame->passIndex * kPassNumerator) / kPassDenominator);
}

void BeemazeScene::beemazeTraceState(std::string_view state) {
    const auto &game = *beemazeGame;
    referenceTrace.emit("minigame_state", {
        {"game", "beemaze"}, {"state", state}, {"level", game.level},
        {"lives", game.lives}, {"score", beemazeScore}});
}

void BeemazeScene::beemazeAddClip(BeemazeClip &clip, std::string_view asset,
                                  int x, int y, int layer,
                                  std::uint32_t hotspot) {
    // SCENE_AddAnim 0x00412840.
    clip.added = true;
    clip.x = x;
    clip.y = y;
    clip.layer = layer;
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", 0}});
}

void BeemazeScene::beemazeRemoveClip(BeemazeClip &clip,
                                     std::string_view asset) {
    // SCENE_RemoveAnim 0x00412aa0 logs its own boundary, then stops the
    // descriptor through SCENE_StopAnim 0x00412a60.
    clip.added = false;
    clip.playing = false;
    referenceTrace.emit("animation_remove", {
        {"asset", asset}, {"reason", "removed"}});
    referenceTrace.emit("animation_stop", {
        {"asset", asset}, {"reason", "stopped"}});
}

void BeemazeScene::beemazePlayClip(BeemazeClip &clip) {
    // 0x004129d0 starts a Smacker descriptor's player.
    clip.playing = true;
    clip.startedAt = beemazeNow();
}

int BeemazeScene::beemazeArmTimer(int due, int tag) {
    // EVENT_AddTimer 0x0040c480 takes the first free slot.
    auto &timers = beemazeGame->timers;
    for (std::size_t slot = 0; slot < timers.size(); ++slot) {
        if (!timers[slot].used) {
            timers[slot] = {true, due, tag};
            return static_cast<int>(slot);
        }
    }
    timers.push_back({true, due, tag});
    return static_cast<int>(timers.size() - 1U);
}

void BeemazeScene::beemazeCancelTimer(int &timer) {
    // RemoveTimer 0x0040c500.
    auto &timers = beemazeGame->timers;
    if (timer >= 0 && static_cast<std::size_t>(timer) < timers.size()) {
        timers[static_cast<std::size_t>(timer)].used = false;
    }
    timer = -1;
}

void BeemazeScene::beemazeClearTimers() {
    // 0x0040c510 resets the highest used slot to -1, dropping every timer.
    beemazeGame->timers.clear();
}

void BeemazeScene::beemazeServiceTimers(int clock) {
    // 0x0040bf50 runs before each 0x112 update and delivers each due
    // one-shot timer once, in slot order.
    auto &timers = beemazeGame->timers;
    for (std::size_t slot = 0; slot < timers.size(); ++slot) {
        if (!timers[slot].used || timers[slot].due > clock) continue;
        timers[slot].used = false;
        beemazeTimer(timers[slot].tag);
        if (!beemazeGame) return;
    }
}

void BeemazeScene::startBeemazeGame() {
    // Start hotspot 101 (0x00432c70) runs 0x00432c90, then removes
    // start_icon.
    if (!beemazeGame) return;
    auto &game = *beemazeGame;
    if (!game.startClip.added) return;
    game.gameOver = false;
    game.playing = true;
    game.lives = 3;
    game.level = 0;
    beemazeStartLevel();
    beemazeScore = 0;
    beemazeDrawScore();
    beemazeRemoveClip(game.startClip, "start_icon");
}

void BeemazeScene::beemazeDrawScore() {
    // 0x00431a30 prints "%d" into the top-right field.
    beemazeGame->scoreShown = true;
    beemazeTraceState("score");
}

void BeemazeScene::beemazeBlitPlayer(int column, int row, int sheet) {
    auto &game = *beemazeGame;
    game.sheetColumn = column;
    game.sheetRow = row;
    game.sheet = sheet;
}

void BeemazeScene::beemazeStartLevel() {
    // 0x00431ea0.
    auto &game = *beemazeGame;
    beemazeTraceState("level_start");
    if (beemazeFixedSeed) {
        // Capture fixture, not original behaviour: the reference script
        // reseeds rand() at every Bee Maze level start (see
        // reference/windows/x32dbg/yard_timeline_headless.txt).
        beemazeRandomState = *beemazeFixedSeed;
        beemazeRandomSeeded = true;
    }
    game.playing = false;
    beemazeClearTimers();
    game.mushrooms = 0;
    game.berries = 0;
    game.sticks = 0;
    game.nuts = 0;
    if (game.levelClip.added) beemazeRemoveClip(game.levelClip, game.levelName);
    game.levelName = "level" + std::to_string(game.level % 5 + 1);
    beemazeAddClip(game.levelClip, game.levelName, -1, -1, 0);
    game.walkCounter = 0;
    game.facing = 1;
    beemazePlaceSmokers();
    beemazePlaceItems();
    beemazePlaceBees();
    game.powerups = 0;
    beemazeClearTimers();
    beemazeAddClip(game.playerClip, "", game.playerX + kPlayerOffsetX,
                   game.playerY + kPlayerOffsetY, 200);
    beemazeBlitPlayer(game.walkCounter % 15, game.facing - 1, kSheetWalk);
    for (auto &life : game.lifeClips) {
        if (life.added) beemazeRemoveClip(life, "life");
    }
    for (int life = 0; life < game.lives && life < 3; ++life) {
        beemazeAddClip(game.lifeClips[static_cast<std::size_t>(life)],
                       "life", 78 + life * 36, 27, 150);
    }
    game.levelComplete = false;
    // 0x00432049-0x004320ba: a trail of min(level, 25) cells, all Ludvig's
    // start cell, written from the last slot.
    const int trailLength = std::min(game.level, 25);
    game.trail.assign(static_cast<std::size_t>(std::max(trailLength, 0)),
                      SDL_Point{game.playerX / kCell, game.playerY / kCell});
    game.trailIndex = trailLength - 1;
    for (int bee = 0; bee < game.beeCount; ++bee) {
        game.beeStunned[static_cast<std::size_t>(bee)] = false;
    }
    beemazeArmTimer(beemazeClock() + 1000, kTagPlaying);
    game.resetBeeClock = true;
}

void BeemazeScene::beemazePlaceSmokers() {
    // 0x00432350: min(level / 5 + 2, 7) smokers on random value-2 cells.
    auto &game = *beemazeGame;
    game.smokers = std::min(game.level / 5 + 2, 7);
    for (int smoker = 0; smoker < game.smokers; ++smoker) {
        int x = 0;
        int y = 0;
        do {
            x = beemazeRandom() % kColumns;
            y = beemazeRandom() % kRows;
        } while (mapValue(game.level, x, y) != 2);
        const int index = cellIndex(x, y);
        game.cellNames[static_cast<std::size_t>(index)] = "gun";
        BeemazeClip clip;
        beemazeAddClip(clip, "gun", x * kCell + kBoardX, y * kCell + kBoardY,
                       101);
        game.cellAdded[static_cast<std::size_t>(index)] = true;
    }
}

void BeemazeScene::beemazePlaceItems() {
    // 0x00432470 walks columns, then rows.
    auto &game = *beemazeGame;
    int hives = 0;
    for (int x = 0; x < kColumns; ++x) {
        for (int y = 0; y < kRows; ++y) {
            const int value = mapValue(game.level, x, y);
            if (value == 2 &&
                game.cellNames[static_cast<std::size_t>(cellIndex(x, y))] !=
                    "gun") {
                beemazePlaceItem(x, y);
            }
            if (value == 3 && hives < 4) {
                game.hiveCandidates[static_cast<std::size_t>(hives++)] = {x, y};
            }
            if (value == 1) {
                game.playerX = game.targetX = x * kCell;
                game.playerY = game.targetY = y * kCell;
            }
        }
    }
    beemazePlaceHives();
    beemazeStartLoopingAmbient();
}

void BeemazeScene::beemazePlaceItem(int x, int y) {
    // 0x00432600: rand() % 20 == 0 leaves the cell empty; 1 berries, 2 nut,
    // 3 mushroom, anything else a stick.
    auto &game = *beemazeGame;
    const int roll = beemazeRandom() % 20;
    if (roll == 0) return;
    const auto index = static_cast<std::size_t>(cellIndex(x, y));
    std::string_view name = "stick";
    if (roll == 1) {
        name = "berries";
        ++game.berries;
    } else if (roll == 2) {
        name = "nut";
        ++game.nuts;
    } else if (roll == 3) {
        name = "mush";
        ++game.mushrooms;
    } else {
        ++game.sticks;
    }
    game.cellNames[index] = std::string(name);
    BeemazeClip clip;
    beemazeAddClip(clip, name, x * kCell + kBoardX, y * kCell + kBoardY, 101);
    game.cellAdded[index] = true;
}

void BeemazeScene::beemazePlaceHives() {
    // 0x00432730: min(level / 5 + 1, 4) distinct hive candidates.
    auto &game = *beemazeGame;
    game.hivesRemaining = std::min(game.level / 5 + 1, 4);
    for (int hive = 0; hive < game.hivesRemaining; ++hive) {
        SDL_Point cell{};
        do {
            cell = game.hiveCandidates[static_cast<std::size_t>(
                beemazeRandom() % 4)];
        } while (game.cellNames[static_cast<std::size_t>(
                     cellIndex(cell.x, cell.y))] == "honey");
        const auto index = static_cast<std::size_t>(cellIndex(cell.x, cell.y));
        game.cellNames[index] = "honey";
        BeemazeClip clip;
        beemazeAddClip(clip, "honey", cell.x * kCell + kBoardX,
                       cell.y * kCell + kBoardY, 101);
        game.cellAdded[index] = true;
    }
}

void BeemazeScene::beemazeStartLoopingAmbient() {
    // 0x00432590 starts the level's looping ambient clips and schedules the
    // first roll for the others five seconds later.
    auto &game = *beemazeGame;
    const auto [first, last] =
        kAmbientRange[static_cast<std::size_t>(game.level % 5)];
    for (std::size_t index = first; index < last; ++index) {
        if (!kAmbient[index].looping) continue;
        auto &clip = game.ambientClips[index];
        if (!clip.added) {
            beemazeAddClip(clip, kAmbient[index].name, -1, -1, 101);
            clip.x = kAmbient[index].x;
            clip.y = kAmbient[index].y;
        }
        beemazePlayClip(clip);
    }
    game.ambientDue = beemazeClock() + 5000;
}

void BeemazeScene::beemazePlaceBees() {
    // 0x00432100: min(level / 5 + 2, 7) bees on random open cells. The test
    // at 0x004321f0 bounds x by 0x200 before it bounds x + 24 by 0x2c0, so
    // bees only start in columns 0-16; 0x00432310 keeps them at least 160
    // pixels from Ludvig on one axis.
    auto &game = *beemazeGame;
    game.beeCount = std::min(game.level / 5 + 2, 7);
    const auto fits = [&](int x, int y) {
        if (x < 0 || y < 0 || x > 0x200 || y > 0x2c0) return false;
        for (const int dy : {8, 24}) {
            for (const int dx : {8, 24}) {
                const int column = (x + dx) / kCell;
                const int row = (y + dy) / kCell;
                if (column >= kColumns || row >= kRows) return false;
                if (mapValue(game.level, column, row) == 4) return false;
            }
        }
        return x + 24 <= 0x2c0 && y + 24 <= 0x200;
    };
    const auto farFromPlayer = [&](int x, int y) {
        const int dx = x - game.playerX;
        const int dy = y - game.playerY;
        return !(dx > -160 && dx < 160 && dy > -160 && dy < 160);
    };
    for (int bee = 0; bee < game.beeCount; ++bee) {
        int x = 0;
        int y = 0;
        do {
            x = (beemazeRandom() % kColumns) * kCell;
            y = (beemazeRandom() % kRows) * kCell;
        } while (!fits(x, y) || !farFromPlayer(x, y));
        const auto index = static_cast<std::size_t>(bee);
        beemazeAddClip(game.beeClips[index], "bees", x + kBoardX,
                       y + kBoardY, 201);
        beemazePlayClip(game.beeClips[index]);
        game.beeX[index] = x;
        game.beeY[index] = y;
        game.beeCellX[index] = x / kCell;
        game.beeCellY[index] = y / kCell;
    }
}

void BeemazeScene::beemazeRestartLevel() {
    // 0x00431cf0: tear the board down, advance after a completed level and
    // build the next one.
    auto &game = *beemazeGame;
    for (std::size_t index = 0; index < kAmbient.size(); ++index) {
        if (game.ambientClips[index].added) {
            beemazeRemoveClip(game.ambientClips[index], kAmbient[index].name);
        }
    }
    game.playing = false;
    game.heldDirection = 0;
    for (int x = 0; x < kColumns; ++x) {
        for (int y = 0; y < kRows; ++y) {
            const auto index = static_cast<std::size_t>(cellIndex(x, y));
            if (game.cellAdded[index]) {
                BeemazeClip clip;
                beemazeRemoveClip(clip, game.cellNames[index]);
                game.cellAdded[index] = false;
            }
        }
    }
    game.cellNames.fill({});
    if (game.smokeClip.added) beemazeRemoveClip(game.smokeClip, "smoke");
    for (int bee = 0; bee < game.beeCount; ++bee) {
        const auto index = static_cast<std::size_t>(bee);
        if (game.beeClips[index].added) {
            beemazeRemoveClip(game.beeClips[index], "bees");
        }
        if (game.beeSleepClips[index].added) {
            beemazeRemoveClip(game.beeSleepClips[index], "bee_sleep");
        }
    }
    if (game.arrowClip.added) beemazeRemoveClip(game.arrowClip, "arrow");
    if (game.levelComplete) ++game.level;
    beemazeStartLevel();
}

void BeemazeScene::beemazePlayClone(std::string_view base, int variants) {
    // 0x00431a00: "<base>_<rand() % variants + 1>" through
    // SCENE_PlayAnimClone 0x00412c20.
    // 0x0040c530 returns 0 without calling rand() only for a zero bound.
    const int variant = variants == 0 ? 0 : beemazeRandom() % variants;
    beemazeCloneAnimation(std::string(base) + "_" +
                          std::to_string(variant + 1));
}

void BeemazeScene::beemazeCloneAnimation(const std::string &asset) {
    auto &game = *beemazeGame;
    const auto sound = game.sounds.find(asset);
    if (sound == game.sounds.end()) return;
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    audioMixer.addEffect(sound->second, audioVolumes[0] * sceneAudioGains[0]);
    game.cloneSounds.push_back(
        {asset, beemazeNow() +
                    originalSmackerCompletionDuration(sound->second)});
}

void BeemazeScene::beemazePickup(int cellX, int cellY) {
    // 0x00431570 runs on every update in which Ludvig is at a cell centre.
    auto &game = *beemazeGame;
    const int value = mapValue(game.level, cellX, cellY);
    const auto index = static_cast<std::size_t>(cellIndex(cellX, cellY));
    if ((value == 2 || value == 3) && game.cellAdded[index]) {
        const std::string name = game.cellNames[index];
        if (name == "stick") {
            --game.sticks;
            beemazeScore += 2;
        }
        if (name == "berries") {
            beemazePlayClone("berries", 1);
            --game.berries;
            beemazeScore += 10;
        }
        if (name == "nut") {
            beemazePlayClone("nuts", 2);
            --game.nuts;
            beemazeScore += 20;
        }
        if (name == "mush") {
            beemazePlayClone("mushroom", 1);
            if (game.powerups & 2U) beemazeCancelTimer(game.mushroomTimer);
            game.powerups |= 2U;
            --game.mushrooms;
            game.mushroomTimer =
                beemazeArmTimer(beemazeClock() + 10000, kTagMushroom);
            beemazeScore += 30;
        }
        if (name == "gun") {
            beemazePlayClone("smoke", 1);
            if (game.powerups & 1U) beemazeCancelTimer(game.smokeTimer);
            if (game.powerups & 8U) {
                beemazeCancelTimer(game.smokeFlashTimer);
                game.powerups &= ~8U;
            }
            game.powerups |= 1U;
            if (!game.smokeClip.added) {
                beemazeAddClip(game.smokeClip, "smoke",
                               game.playerX + kPlayerOffsetX,
                               game.playerY + kPlayerOffsetY, 202);
                beemazePlayClip(game.smokeClip);
            }
            game.smokeTimer = beemazeArmTimer(beemazeClock() + 10000, kTagSmoke);
        }
        if (name == "honey") {
            // 0x004318d3-0x004318da: a second hive is left in place while
            // one is carried, and the rest of the handler is skipped.
            if (game.powerups & 4U) return;
            beemazePlayClone("honey", 1);
            game.powerups |= 4U;
            --game.hivesRemaining;
            const auto home = kHome[static_cast<std::size_t>(game.level % 5)];
            beemazeAddClip(game.arrowClip, "arrow", home.x, home.y, 199);
            beemazePlayClip(game.arrowClip);
        }
        BeemazeClip clip;
        beemazeRemoveClip(clip, name);
        game.cellAdded[index] = false;
    }
    if ((game.powerups & 4U) && value == 1) {
        game.powerups &= 0xfffffffbU;
        beemazeScore += 250;
        if (game.arrowClip.added) beemazeRemoveClip(game.arrowClip, "arrow");
        const auto home = kHome[static_cast<std::size_t>(game.level % 5)];
        beemazeAddClip(game.pointsClip, "250_points", home.x, home.y, 199);
        beemazePlayClip(game.pointsClip);
    }
    if (beemazeDrawnScore != beemazeScore) {
        beemazeDrawScore();
        beemazeDrawnScore = beemazeScore;
    }
}

std::pair<int, int> BeemazeScene::beemazeStepPlayer(int x, int y, int step) {
    // 0x004310e0: move toward the target cell; at its centre, collect and
    // turn to the held direction.
    auto &game = *beemazeGame;
    const int dx = game.targetX - x;
    const int dy = game.targetY - y;
    game.walkCounter += (game.powerups & 2U) ? 2 : 1;
    const int absoluteX = std::abs(dx);
    const int absoluteY = std::abs(dy);
    if ((absoluteX < step) != (absoluteY < step)) {
        if (dx == 0) return {x, dy > 0 ? y + step : y - step};
        if (dy == 0) return {dx > 0 ? x + step : x - step, y};
    } else {
        int remaining = step;
        if (dy == 0) {
            x = game.targetX;
            remaining = step - absoluteX;
        }
        if (dx == 0) {
            y = game.targetY;
            remaining = step - absoluteY;
        }
        const int cellX = game.targetX / kCell;
        const int cellY = game.targetY / kCell;
        beemazePickup(cellX, cellY);
        const auto beginStep = [&](int direction) {
            const int trailLength = std::min(game.level, 25);
            const int stepX = game.targetX / kCell;
            const int stepY = game.targetY / kCell;
            // 0x00431a70 records the cell Ludvig starts moving into.
            referenceTrace.emit("minigame_state", {
                {"game", "beemaze"}, {"state", "step"},
                {"cell_x", stepX}, {"cell_y", stepY},
                {"score", beemazeScore}});
            if (trailLength >= 1 && !game.trail.empty()) {
                game.trail[static_cast<std::size_t>(game.trailIndex)] =
                    {stepX, stepY};
                game.trailIndex = game.trailIndex == trailLength - 1
                                      ? 0 : game.trailIndex + 1;
            }
            game.facing = direction;
        };
        switch (game.heldDirection) {
        case 1:
            if (!playerPassable(game.level, cellX + 1, cellY)) {
                if (!playerWraps(game.level, cellX, cellY, 1)) {
                    game.heldDirection = 0;
                    return {x, y};
                }
                game.targetX -= 704;
                x -= 704;
            }
            game.targetX += kCell;
            x += remaining;
            beginStep(1);
            return {x, y};
        case 2:
            if (!playerPassable(game.level, cellX, cellY + 1)) {
                if (!playerWraps(game.level, cellX, cellY, 2)) {
                    game.heldDirection = 0;
                    return {x, y};
                }
                game.targetY -= 512;
                y -= 512;
            }
            game.targetY += kCell;
            y += remaining;
            beginStep(2);
            return {x, y};
        case 3:
            if (!playerPassable(game.level, cellX - 1, cellY)) {
                if (!playerWraps(game.level, cellX, cellY, 3)) {
                    game.heldDirection = 0;
                    return {x, y};
                }
                game.targetX += 704;
                x += 704;
            }
            game.targetX -= kCell;
            x -= remaining;
            beginStep(3);
            return {x, y};
        case 4:
            if (!playerPassable(game.level, cellX, cellY - 1)) {
                if (!playerWraps(game.level, cellX, cellY, 4)) {
                    game.heldDirection = 0;
                    return {x, y};
                }
                game.targetY += 512;
                y += 512;
            }
            game.targetY -= kCell;
            y -= remaining;
            beginStep(4);
            return {x, y};
        default:
            break;
        }
    }
    // 0x00431454: no move this update.
    game.walkCounter -= (game.powerups & 2U) ? 2 : 1;
    return {x, y};
}

void BeemazeScene::beemazeMovePlayer(int clock) {
    // 0x00430e50: 128 pixels per second (1.5x with the mushroom), applied
    // only after more than 20 ms; a gap over a second restarts the clock.
    auto &game = *beemazeGame;
    if (game.moveClock == 0) {
        game.moveClock = clock;
        game.animationClock = clock;
        game.animationCounter = 0;
        game.lastWalkCounter = 0;
    }
    const int elapsed = clock - game.moveClock;
    if (elapsed > 1000) {
        game.moveClock = 0;
        return;
    }
    if (elapsed > 20) {
        int step = 0;
        if (game.powerups & 2U) {
            step = static_cast<int>(static_cast<double>(elapsed << 7) * 1.5) /
                   1000;
        } else {
            step = (elapsed << 7) / 1000;
        }
        game.moveClock = clock;
        const auto [x, y] = beemazeStepPlayer(game.playerX, game.playerY, step);
        if (!beemazeGame) return;
        game.playerX = x;
        game.playerY = y;
    }
    if (game.lastWalkCounter != game.walkCounter) {
        game.lastWalkCounter = game.walkCounter;
        const int frameGap = (game.powerups & 2U) ? 15 : 25;
        if (clock - game.animationClock > frameGap) {
            ++game.animationCounter;
            game.animationClock = clock;
            const bool smoke = (game.powerups & 1U) != 0U;
            const bool honey = (game.powerups & 4U) != 0U;
            const int sheet = smoke ? (honey ? kSheetHoneySmoke : kSheetSmoke)
                                    : (honey ? kSheetHoney : kSheetWalk);
            beemazeBlitPlayer(
                static_cast<int>(static_cast<unsigned>(game.animationCounter) %
                                 15U),
                game.facing - 1, sheet);
        }
    }
    game.playerClip.x = game.playerX + kPlayerOffsetX;
    game.playerClip.y = game.playerY + kPlayerOffsetY;
}

void BeemazeScene::beemazeFollowSmoke() {
    // 0x00430d90 keeps the smoke cloud one cell ahead of Ludvig.
    auto &game = *beemazeGame;
    if (!(game.powerups & 1U)) return;
    switch (game.facing) {
    case 1: game.smokeX = game.playerX + kCell; game.smokeY = game.playerY; break;
    case 2: game.smokeX = game.playerX; game.smokeY = game.playerY + kCell; break;
    case 3: game.smokeX = game.playerX - kCell; game.smokeY = game.playerY; break;
    case 4: game.smokeX = game.playerX; game.smokeY = game.playerY - kCell; break;
    default: break;
    }
    if (game.smokeClip.playing) {
        game.smokeClip.x = game.smokeX + kBoardX;
        game.smokeClip.y = game.smokeY + kBoardY;
    }
}

int BeemazeScene::beemazePanicDirection(int bee) const {
    // 0x0042fab0: flee from Ludvig, preferring the axis he is nearer on.
    const auto &game = *beemazeGame;
    const auto index = static_cast<std::size_t>(bee);
    const int dx = game.playerX - game.beeX[index];
    const int dy = game.playerY - game.beeY[index];
    const int cx = game.beeCellX[index];
    const int cy = game.beeCellY[index];
    const auto open = [&](int x, int y) {
        return beePassable(game.level, x, y);
    };
    const auto belowLeft = [&]() {
        if (std::abs(dx) < std::abs(dy)) {
            if (open(cx + 1, cy)) return 1;
            if (open(cx, cy - 1)) return 4;
            return 0;
        }
        if (open(cx, cy - 1)) return 4;
        if (open(cx + 1, cy)) return 1;
        return 0;
    };
    if (dx < 0) {
        if (dy >= 0) return belowLeft();
        if (dx > dy) {
            if (open(cx + 1, cy)) return 1;
            if (open(cx, cy + 1)) return 2;
        } else {
            if (open(cx, cy + 1)) return 2;
            if (open(cx + 1, cy)) return 1;
        }
        return 0;
    }
    if (dy < 0) {
        if (std::abs(dx) > std::abs(dy)) {
            if (open(cx - 1, cy)) return 3;
            if (open(cx, cy + 1)) return 2;
        } else {
            if (open(cx, cy + 1)) return 2;
            if (open(cx - 1, cy)) return 3;
        }
        return 0;
    }
    if (dx < dy) {
        if (open(cx - 1, cy)) return 3;
        if (open(cx, cy - 1)) return 4;
        return 0;
    }
    if (open(cx, cy - 1)) return 4;
    if (open(cx - 1, cy)) return 3;
    return 0;
}

bool BeemazeScene::beemazeBeeOnTrail(int cellX, int cellY) const {
    // 0x0042f820 scans `level` entries. UNCERTAIN: past level 25 that reads
    // beyond the 25-entry array; native stops at the array's end.
    const auto &game = *beemazeGame;
    const auto count = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(game.level, 0)), game.trail.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (game.trail[index].x == cellX && game.trail[index].y == cellY) {
            return true;
        }
    }
    return false;
}

int BeemazeScene::beemazeTrailDirection(int cellX, int cellY) const {
    // 0x00430030: find the bee's cell in Ludvig's trail and head for the
    // following entry.
    const auto &game = *beemazeGame;
    const int length = std::min(game.level, 25);
    const int size = static_cast<int>(game.trail.size());
    const auto matches = [&](int index) {
        // UNCERTAIN: the second scan starts at index `length`, one past the
        // allocation; native treats that slot as never matching.
        return index >= 0 && index < size &&
               game.trail[static_cast<std::size_t>(index)].x == cellX &&
               game.trail[static_cast<std::size_t>(index)].y == cellY;
    };
    int found = -1;
    for (int index = game.trailIndex - 1; index > -1; --index) {
        if (matches(index)) {
            found = index;
            break;
        }
    }
    if (found == -1) {
        if (length < game.trailIndex) return 0;
        for (int index = length; index >= game.trailIndex; --index) {
            if (matches(index)) {
                found = index;
                break;
            }
        }
        if (found == -1) return 0;
    }
    const int wrap = game.level < 25 ? game.level - 1 : 24;
    const int next = found == wrap ? 0 : found + 1;
    if (next < 0 || next >= size) return 0;
    const SDL_Point target = game.trail[static_cast<std::size_t>(next)];
    if (target.x > cellX) return 1;
    if (target.x < cellX) return 3;
    if (target.y > cellY) return 2;
    if (target.y < cellY) return 4;
    return 0;
}

int BeemazeScene::beemazePissedDirection(int bee) const {
    // 0x00430370: breadth-first distances from Ludvig's cell; the first
    // reached neighbour that is the bee's cell gives the step toward him.
    const auto &game = *beemazeGame;
    const auto index = static_cast<std::size_t>(bee);
    const int beeX = game.beeCellX[index];
    const int beeY = game.beeCellY[index];
    std::array<int, kColumns * kRows> distance;
    distance.fill(-1);
    bool first = true;
    for (int level = 1; level <= kColumns * kRows + 1; ++level) {
        int x = 0;
        int y = 0;
        bool startAtPlayer = first;
        for (x = 0; x < kColumns; ++x) {
            for (y = 0; y < kRows; ++y) {
                if (startAtPlayer) {
                    startAtPlayer = false;
                    first = false;
                    x = game.playerX / kCell;
                    y = game.playerY / kCell;
                    distance[static_cast<std::size_t>(cellIndex(x, y))] =
                        level - 1;
                }
                if (distance[static_cast<std::size_t>(cellIndex(x, y))] !=
                    level - 1) {
                    continue;
                }
                const auto visit = [&](int nx, int ny, int answer) {
                    if (!beePassable(game.level, nx, ny)) return 0;
                    if (nx == beeX && ny == beeY) return answer;
                    auto &slot =
                        distance[static_cast<std::size_t>(cellIndex(nx, ny))];
                    if (slot == -1) slot = level;
                    return 0;
                };
                if (const int answer = visit(x + 1, y, 3)) return answer;
                if (const int answer = visit(x, y + 1, 4)) return answer;
                if (const int answer = visit(x - 1, y, 1)) return answer;
                if (const int answer = visit(x, y - 1, 2)) return answer;
            }
        }
    }
    // The original loops forever when no path exists; native gives up.
    return 0;
}

int BeemazeScene::beemazeRandomDirection(int bee) {
    // 0x00430800 dispatches on the current direction to 0x00430850 (down or
    // none), 0x00430950 (right), 0x00430b60 (left) or 0x00430a50 (up). Each
    // tests three exits, draws rand() % 10 and picks from its jump table.
    const auto &game = *beemazeGame;
    const auto index = static_cast<std::size_t>(bee);
    const int cx = game.beeCellX[index];
    const int cy = game.beeCellY[index];
    const auto open = [&](int x, int y) {
        return beePassable(game.level, x, y);
    };
    // Mirrors the BeemazeBeeTurn logpoint at 0x00430800's entry.
    referenceTrace.emit("minigame_state", {
        {"game", "beemaze"}, {"state", "bee_turn"}, {"bee", bee},
        {"cell_x", cx}, {"cell_y", cy}});
    const int direction = beemazeBeeDirections[index];
    unsigned mask = 0;
    if (direction == 1) {
        if (open(cx + 1, cy)) mask |= 1U;
        if (open(cx, cy + 1)) mask |= 2U;
        if (open(cx, cy - 1)) mask |= 4U;
    } else if (direction == 3) {
        if (open(cx - 1, cy)) mask |= 1U;
        if (open(cx, cy - 1)) mask |= 2U;
        if (open(cx, cy + 1)) mask |= 4U;
    } else if (direction == 4) {
        if (open(cx, cy - 1)) mask |= 1U;
        if (open(cx + 1, cy)) mask |= 2U;
        if (open(cx - 1, cy)) mask |= 4U;
    } else if (direction == 0 || direction == 2) {
        if (open(cx, cy + 1)) mask |= 1U;
        if (open(cx - 1, cy)) mask |= 2U;
        if (open(cx + 1, cy)) mask |= 4U;
    } else {
        return 0;
    }
    const int roll = beemazeRandom() % 10;
    if (direction == 1) {
        // Table 0x00430a24. Mask 4 (only up open) reuses the reverse entry
        // 0x004309be and turns the bee back left.
        switch (mask) {
        case 0: return 3;
        case 1: return 1;
        case 2: return 2;
        case 3: return roll == 1 ? 2 : 1;
        case 4: return 3;
        case 5: return roll == 1 ? 4 : 1;
        case 6: return roll >= 5 ? 4 : 2;
        default: return roll == 1 ? 2 : roll == 2 ? 4 : 1;
        }
    }
    if (direction == 3) {
        // Table 0x00430c44.
        switch (mask) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 4;
        case 3: return roll == 1 ? 4 : 3;
        case 4: return 2;
        case 5: return roll == 1 ? 2 : 3;
        case 6: return roll >= 5 ? 2 : 4;
        default: return roll == 1 ? 4 : roll == 2 ? 2 : 3;
        }
    }
    if (direction == 4) {
        // Table 0x00430b34.
        switch (mask) {
        case 0: return 2;
        case 1: return 4;
        case 2: return 1;
        case 3: return roll == 1 ? 1 : 4;
        case 4: return 3;
        case 5: return roll == 1 ? 3 : 4;
        case 6: return roll >= 5 ? 3 : 1;
        default: return roll == 1 ? 1 : roll == 2 ? 3 : 4;
        }
    }
    // Table 0x0043092c.
    switch (mask) {
    case 0: return 4;
    case 1: return 2;
    case 2: return 3;
    case 3: return roll == 1 ? 3 : 2;
    case 4: return 1;
    case 5: return roll == 1 ? 1 : 2;
    case 6: return roll >= 5 ? 1 : 3;
    default: return roll == 1 ? 3 : roll == 2 ? 1 : 2;
    }
}

std::pair<int, int> BeemazeScene::beemazeStepBee(int bee, int step, int mode) {
    // Shared shape of 0x00430550 (random), 0x0042f860 (panic), 0x0042fd80
    // (trail) and 0x00430120 (pissed): continue toward the target cell, and
    // at its centre choose the next cell.
    auto &game = *beemazeGame;
    const auto index = static_cast<std::size_t>(bee);
    int &cx = game.beeCellX[index];
    int &cy = game.beeCellY[index];
    const int bx = game.beeX[index];
    const int by = game.beeY[index];
    const int dx = std::abs(bx - cx * kCell);
    const int dy = std::abs(by - cy * kCell);
    step = std::min(step, std::max(dx, dy));   // 0x0042fd60
    if ((dy < step) != (dx < step)) {
        switch (beemazeBeeDirections[index]) {
        case 1: return {bx + step, by};
        case 2: return {bx, by + step};
        case 3: return {bx - step, by};
        case 4: return {bx, by - step};
        default: return {bx, by};
        }
    }
    int x = cx * kCell;
    int y = cy * kCell;
    if (dx != 0) {
        step -= dx;
    } else if (dy != 0) {
        step -= dy;
    }
    int direction = 0;
    switch (mode) {
    case kModePissed: direction = beemazePissedDirection(bee); break;
    case kModeTrail: direction = beemazeTrailDirection(cx, cy); break;
    case kModePanic: direction = beemazePanicDirection(bee); break;
    default: direction = beemazeRandomDirection(bee); break;
    }
    // Trail moves only take an open cell (0x0042feb9-0x0042fecd); the others
    // move regardless and log "bee_nextcell IS ILLEGAL".
    const bool checkOpen = mode == kModeTrail;
    switch (direction) {
    case 1:
        if (!checkOpen || beePassable(game.level, cx + 1, cy)) ++cx;
        x += step;
        break;
    case 2:
        if (!checkOpen || beePassable(game.level, cx, cy + 1)) ++cy;
        y += step;
        break;
    case 3:
        if (!checkOpen || beePassable(game.level, cx - 1, cy)) --cx;
        x -= step;
        break;
    case 4:
        if (!checkOpen || beePassable(game.level, cx, cy - 1)) --cy;
        y -= step;
        break;
    default:
        return {x, y};
    }
    beemazeBeeDirections[index] = direction;
    return {x, y};
}

void BeemazeScene::beemazeMoveBees(int clock) {
    // 0x0042f520: bees move (0.6 + 0.1 * (level / 5)) * 128 pixels per
    // second, 0.9 instead of 0.6 while Ludvig carries a hive.
    auto &game = *beemazeGame;
    if (game.resetBeeClock) {
        game.beeClock = 0;
        game.resetBeeClock = false;
    }
    if (game.beeClock == 0) game.beeClock = clock;
    const int elapsed = clock - game.beeClock;
    if (elapsed < 20) return;
    // x87 with the MSVC runtime's 53-bit precision control: each operation
    // rounds to double, so separate statements keep the same rounding.
    const double levelFactor = static_cast<double>(game.level / 5) * 0.1;
    const double factor =
        levelFactor + ((game.powerups & 4U) ? 0.9 : 0.6);
    const double scaled = static_cast<double>(elapsed << 7) * factor;
    const int step = static_cast<int>(scaled) / 1000;
    game.beeClock = clock;
    if (step == 0 || game.beeCount <= 0) return;
    for (int bee = 0; bee < game.beeCount; ++bee) {
        const auto index = static_cast<std::size_t>(bee);
        if (!game.beeClips[index].playing) continue;
        const int cellX = game.beeX[index] / kCell;
        const int cellY = game.beeY[index] / kCell;
        int mode = kModeRandom;
        if (game.powerups & 4U) {
            mode = kModePissed;
        } else if (beemazeBeeOnTrail(cellX, cellY)) {
            mode = kModeTrail;
        } else if (game.powerups & 1U) {
            mode = kModePanic;
        }
        const auto [x, y] = beemazeStepBee(bee, step, mode);
        game.beeX[index] = x;
        game.beeY[index] = y;
        game.beeClips[index].x = x + kBoardX;
        game.beeClips[index].y = y + kBoardY;
    }
}

void BeemazeScene::beemazeSmokeBees(int clock) {
    // 0x00430c70: a bee within 16 pixels of the smoke cloud sleeps for ten
    // seconds.
    auto &game = *beemazeGame;
    if (!(game.powerups & 1U)) return;
    for (int bee = 0; bee < game.beeCount; ++bee) {
        const auto index = static_cast<std::size_t>(bee);
        if (std::abs(game.smokeX - game.beeX[index]) >= 16 ||
            std::abs(game.smokeY - game.beeY[index]) >= 16) {
            continue;
        }
        if (game.beeStunned[index]) {
            beemazeCancelTimer(game.beeStunTimer[index]);
        }
        game.beeStunTimer[index] =
            beemazeArmTimer(clock + 10000, kTagBeeStun + bee);
        game.beeStunned[index] = true;
        if (game.beeClips[index].playing) {
            beemazeRemoveClip(game.beeClips[index], "bees");
        }
        if (!game.beeSleepClips[index].playing) {
            beemazeAddClip(game.beeSleepClips[index], "bee_sleep",
                           game.beeX[index] + kBoardX,
                           game.beeY[index] + kBoardY, 202);
            beemazePlayClip(game.beeSleepClips[index]);
        }
    }
}

bool BeemazeScene::beemazeBeeCaughtPlayer() const {
    // 0x004329e0.
    const auto &game = *beemazeGame;
    for (int bee = 0; bee < game.beeCount; ++bee) {
        const auto index = static_cast<std::size_t>(bee);
        if (!game.beeClips[index].playing) continue;
        if (std::abs(game.playerX - game.beeX[index]) < 16 &&
            std::abs(game.playerY - game.beeY[index]) < 16) {
            return true;
        }
    }
    return false;
}

void BeemazeScene::beemazeLoseLife() {
    // 0x00432880.
    auto &game = *beemazeGame;
    referenceTrace.emit("minigame_state", {
        {"game", "beemaze"}, {"state", "life_lost"}, {"level", game.level},
        {"lives", game.lives}, {"score", beemazeScore}});
    game.playing = false;
    beemazeCloneAnimation("cry");
    --game.lives;
    if (game.lives >= 0 && game.lives < 3) {
        auto &life = game.lifeClips[static_cast<std::size_t>(game.lives)];
        beemazeRemoveClip(life, "life");
    }
    if (game.lives > 0) {
        beemazeClearTimers();
        beemazeRestartLevel();
        return;
    }
    // 0x004328e0: Game Over modal, then the board is rebuilt and cleared
    // before the shared score table (continueBeemazeAfterGameOverMessage).
    beemazeTraceState("game_over");
    game.gameOverMessagePending = true;
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
        language.get("beemaze", "GAMEOVER", "Spillet er slutt!"),
        MessageKind::BeemazeGameOver);
}

void BeemazeScene::continueBeemazeAfterGameOverMessage() {
    // 0x00432913-0x004329b2 after modal wrapper 0x00421310 returns.
    if (!beemazeGame || !beemazeGame->gameOverMessagePending) return;
    auto &game = *beemazeGame;
    game.gameOverMessagePending = false;
    beemazeRestartLevel();
    beemazeClearTimers();
    game.playing = false;
    game.gameOver = true;
    for (int x = 0; x < kColumns; ++x) {
        for (int y = 0; y < kRows; ++y) {
            const auto index = static_cast<std::size_t>(cellIndex(x, y));
            if (game.cellAdded[index]) {
                BeemazeClip clip;
                beemazeRemoveClip(clip, game.cellNames[index]);
                game.cellAdded[index] = false;
            }
        }
    }
    for (int bee = 0; bee < game.beeCount; ++bee) {
        auto &clip = game.beeClips[static_cast<std::size_t>(bee)];
        if (clip.added) beemazeRemoveClip(clip, "bees");
    }
    // Shared score wrapper 0x0041f9a0 with thresholds 0x0049db00.
    showHighScore(beemazeScore);
}

void BeemazeScene::restoreBeemazeStartAfterHighScore() {
    // Callback 0x004329c0 adds start_icon at layer 150.
    if (!beemazeGame || beemazeGame->startClip.added) return;
    beemazeAddClip(beemazeGame->startClip, "start_icon", -1, -1, 150,
                   kStartHotspot);
}

void BeemazeScene::beemazeTimer(int tag) {
    // Timer event 0x111 handler 0x00432a50.
    auto &game = *beemazeGame;
    if (tag == kTagPlaying) {
        game.playing = true;
        beemazeTraceState("playing");
        return;
    }
    if (!game.playing) return;
    const int clock = beemazePassClock();
    if (tag == kTagSmokeFlash) {
        ++game.smokeFlashCount;
        if (game.smokeClip.added) {
            beemazeRemoveClip(game.smokeClip, "smoke");
        } else {
            // 0x00432ab4 re-adds the cloud at its raw board coordinates.
            beemazeAddClip(game.smokeClip, "smoke", game.smokeX, game.smokeY,
                           202);
            beemazePlayClip(game.smokeClip);
        }
        if (game.smokeFlashCount > 6) {
            if (game.smokeClip.added) beemazeRemoveClip(game.smokeClip, "smoke");
            game.smokeFlashCount = 0;
            game.powerups &= 0xfffffff6U;
            return;
        }
        game.smokeFlashTimer = beemazeArmTimer(clock + 300, kTagSmokeFlash);
        return;
    }
    if (tag == kTagSmoke) {
        if (game.smokeClip.added) beemazeRemoveClip(game.smokeClip, "smoke");
        game.smokeFlashTimer = beemazeArmTimer(clock + 300, kTagSmokeFlash);
        game.powerups |= 8U;
        game.smokeFlashCount = 0;
        return;
    }
    if (tag == kTagMushroom) {
        game.powerups &= 0xfffffffdU;
        return;
    }
    for (int bee = 0; bee < game.beeCount; ++bee) {
        if (tag != kTagBeeStun + bee) continue;
        const auto index = static_cast<std::size_t>(bee);
        if (game.beeSleepClips[index].playing) {
            beemazeRemoveClip(game.beeSleepClips[index], "bee_sleep");
        }
        beemazeAddClip(game.beeClips[index], "bees",
                       game.beeX[index] + kBoardX, game.beeY[index] + kBoardY,
                       202);
        beemazePlayClip(game.beeClips[index]);
        game.beeStunned[index] = false;
    }
}

void BeemazeScene::beemazeAmbientTick(int clock) {
    // 0x0042f460: every five seconds, unless a rolled clip still plays, each
    // rolled clip in turn gets a one-in-three chance to start.
    auto &game = *beemazeGame;
    if (game.ambientDue > clock) return;
    game.ambientDue = clock + 5000;
    const auto [first, last] =
        kAmbientRange[static_cast<std::size_t>(game.level % 5)];
    for (std::size_t index = first; index < last; ++index) {
        if (!kAmbient[index].looping && game.ambientClips[index].playing) {
            return;
        }
    }
    for (std::size_t index = first; index < last; ++index) {
        if (kAmbient[index].looping) continue;
        if (beemazeRandom() % 3 != 0) continue;
        auto &clip = game.ambientClips[index];
        if (!clip.added) {
            beemazeAddClip(clip, kAmbient[index].name, -1, -1, 101);
            clip.x = kAmbient[index].x;
            clip.y = kAmbient[index].y;
        }
        beemazePlayClip(clip);
        return;
    }
}

void BeemazeScene::beemazeTick(int clock) {
    // Update event 0x112 handler 0x0042f3c0.
    auto &game = *beemazeGame;
    if (!game.playing) return;
    beemazeAmbientTick(clock);
    if (game.sticks == 0 && game.berries == 0 && game.mushrooms == 0 &&
        game.nuts == 0) {
        game.levelComplete = true;
        referenceTrace.emit("minigame_state", {
            {"game", "beemaze"}, {"state", "level_complete"},
            {"level", game.level}, {"score", beemazeScore}});
        beemazeRestartLevel();
    }
    if (!game.playing) return;
    beemazeMovePlayer(clock);
    beemazeFollowSmoke();
    beemazeMoveBees(clock);
    if (game.powerups & 1U) {
        beemazeSmokeBees(clock);
        return;
    }
    if (beemazeBeeCaughtPlayer()) beemazeLoseLife();
}

void BeemazeScene::beemazeFinishClips(double now) {
    // The descriptor update after the player's done flag (see
    // originalSmackerCompletionDuration) stops non-looping clips and reports
    // AnimationFinished; event 0x110 handler 0x00432d30 then removes
    // 250_points. Clones are stopped and released without a scene event.
    auto &game = *beemazeGame;
    for (std::size_t index = 0; index < kAmbient.size(); ++index) {
        auto &clip = game.ambientClips[index];
        if (kAmbient[index].looping || !clip.playing) continue;
        if (now - clip.startedAt <
            originalSmackerCompletionDuration(game.ambient[index].media)) {
            continue;
        }
        clip.playing = false;
        referenceTrace.emit("animation_stop", {
            {"asset", kAmbient[index].name}, {"reason", "stopped"}});
        referenceTrace.emit("animation_stop", {
            {"asset", kAmbient[index].name}, {"reason", "finished"}});
    }
    if (game.pointsClip.playing &&
        now - game.pointsClip.startedAt >=
            originalSmackerCompletionDuration(game.points250.media)) {
        game.pointsClip.playing = false;
        referenceTrace.emit("animation_stop", {
            {"asset", "250_points"}, {"reason", "stopped"}});
        referenceTrace.emit("animation_stop", {
            {"asset", "250_points"}, {"reason", "finished"}});
        beemazeRemoveClip(game.pointsClip, "250_points");
    }
    for (auto sound = game.cloneSounds.begin();
         sound != game.cloneSounds.end();) {
        if (now < sound->endsAt) {
            ++sound;
            continue;
        }
        referenceTrace.emit("animation_stop", {
            {"asset", sound->asset}, {"reason", "stopped"}});
        referenceTrace.emit("animation_remove", {
            {"asset", sound->asset}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", sound->asset}, {"reason", "stopped"}});
        sound = game.cloneSounds.erase(sound);
    }
}

void BeemazeScene::beemazeRunPass(double passTime) {
    beemazeGame->passTime = passTime;
    const int clock = beemazePassClock();
    beemazeServiceTimers(clock);
    if (beemazeGame) beemazeTick(clock);
    if (beemazeGame) beemazeFinishClips(passTime);
    if (beemazeGame) beemazeGame->passTime.reset();
}

void BeemazeScene::updateBeemaze(double elapsed) {
    if (!beemazeGame) return;
    auto &game = *beemazeGame;
    if (!game.initialized) {
        // Scene initializer 0x00432d50 adds start_icon at layer 150; native
        // emits it on the first update so it follows the scene_change record.
        game.initialized = true;
        game.passIndex = static_cast<std::uint64_t>(std::ceil(
            elapsed * 1000.0 * kPassDenominator / kPassNumerator));
        if (!game.startClip.added) {
            beemazeAddClip(game.startClip, "start_icon", -1, -1, 150,
                           kStartHotspot);
        }
    }
    const auto passTime = [](std::uint64_t pass) {
        return static_cast<double>(pass * kPassNumerator) /
               (1000.0 * kPassDenominator);
    };
    int passes = 0;
    while (passTime(game.passIndex) <= elapsed) {
        if (++passes > 24) {
            // A stalled frame: the original runs one pass per pump
            // iteration, so it does not catch up either.
            game.passIndex = static_cast<std::uint64_t>(std::ceil(
                elapsed * 1000.0 * kPassDenominator / kPassNumerator));
            break;
        }
        beemazeRunPass(passTime(game.passIndex));
        if (!beemazeGame) return;
        ++game.passIndex;
    }
}

bool BeemazeScene::handleBeemazeKey(SDL_Keycode key, bool down) {
    // Key events 0x10c/0x10d: 0x0042f2c0 sets the held direction while
    // playing (every WM_KEYDOWN, auto-repeat included, reaches it through
    // 0x0040ea0b); 0x0042f340 clears it on the matching key-up.
    if (!beemazeGame) return false;
    auto &game = *beemazeGame;
    int direction = 0;
    switch (key) {
    case SDLK_LEFT: direction = 3; break;
    case SDLK_UP: direction = 4; break;
    case SDLK_RIGHT: direction = 1; break;
    case SDLK_DOWN: direction = 2; break;
    default: return false;
    }
    if (down) {
        if (game.playing) game.heldDirection = direction;
    } else if (game.heldDirection == direction) {
        game.heldDirection = 0;
    }
    return true;
}

void BeemazeScene::renderBeemaze(double elapsed) {
    if (!beemazeGame) return;
    auto &game = *beemazeGame;
    const auto drawMedia = [&](HopscotchMedia &placed, const BeemazeClip &clip,
                               bool loop) {
        if (!clip.added || !placed.texture || placed.media.videoFrames.empty()) {
            return;
        }
        const double time = clip.playing
            ? std::max(0.0, elapsed - clip.startedAt)
            : (loop ? 0.0 : placed.media.videoDuration);
        const auto frameIndex = frameIndexAt(placed.media, time, loop);
        const auto &frame = placed.media.videoFrames[frameIndex];
        uploadVideoFrame(placed.texture.get(), frame);
        SDL_Rect destination{clip.x, clip.y, frame.width, frame.height};
        SDL_RenderCopy(renderer, placed.texture.get(), nullptr, &destination);
    };

    // Layer 0: descriptor 0x004d0560 at (47,73).
    if (game.levelClip.added) {
        SDL_Rect board{47, 73, 703, 511};
        SDL_RenderCopy(renderer,
            game.levelTextures[static_cast<std::size_t>(game.level % 5)].get(),
            nullptr, &board);
    }
    // Layer 101: items, then ambient clips.
    for (int x = 0; x < kColumns; ++x) {
        for (int y = 0; y < kRows; ++y) {
            const auto index = static_cast<std::size_t>(cellIndex(x, y));
            if (!game.cellAdded[index]) continue;
            const std::string &name = game.cellNames[index];
            SDL_Texture *texture =
                name == "stick" ? game.stickTexture.get() :
                name == "nut" ? game.nutTexture.get() :
                name == "mush" ? game.mushTexture.get() :
                name == "berries" ? game.berriesTexture.get() :
                name == "gun" ? game.gunTexture.get() :
                name == "honey" ? game.honeyTexture.get() : nullptr;
            if (!texture) continue;
            SDL_Rect destination{x * kCell + kBoardX, y * kCell + kBoardY,
                                 kCell, kCell};
            SDL_RenderCopy(renderer, texture, nullptr, &destination);
        }
    }
    for (std::size_t index = 0; index < kAmbient.size(); ++index) {
        drawMedia(game.ambient[index], game.ambientClips[index],
                  kAmbient[index].looping);
    }
    // Layer 150: lives and start_icon.
    for (const auto &life : game.lifeClips) {
        if (!life.added) continue;
        int width = 0;
        int height = 0;
        SDL_QueryTexture(game.lifeTexture.get(), nullptr, nullptr, &width,
                         &height);
        SDL_Rect destination{life.x, life.y, width, height};
        SDL_RenderCopy(renderer, game.lifeTexture.get(), nullptr, &destination);
    }
    if (game.startClip.added) {
        SDL_Rect start{225, 172, 350, 256};
        SDL_RenderCopy(renderer, game.startTexture.get(), nullptr, &start);
    }
    // Layer 151: 0x00432dc5-0x00432de9 centres the localized title with
    // Amerigo BT_14_ in (300,22)-(501,54); 0x00431a30 centres the score with
    // Amerigo BT_10_ in (625,32)-(716,55).
    drawCenteredText(renderer, calendarFont,
                     language.get("beemaze", "SCENENAME", "Ludvigs Labyrint"),
                     SDL_Rect{300, 22, 201, 32});
    if (game.scoreShown) {
        drawCenteredText(renderer, dialogFont, std::to_string(beemazeScore),
                         SDL_Rect{625, 32, 91, 23});
    }
    // Layer 199: arrow and 250_points at home.
    drawMedia(game.arrow, game.arrowClip, true);
    drawMedia(game.points250, game.pointsClip, false);
    // Layer 200: Ludvig, clipped to the board by 0x00431b40.
    if (game.playerClip.added) {
        const SDL_Rect clip{48, 74, 752 - 48, 586 - 74};
        SDL_RenderSetClipRect(renderer, &clip);
        SDL_Rect source{game.sheetColumn * kPlayerWidth,
                        game.sheetRow * kPlayerHeight, kPlayerWidth,
                        kPlayerHeight};
        SDL_Rect destination{game.playerClip.x, game.playerClip.y,
                             kPlayerWidth, kPlayerHeight};
        SDL_RenderCopy(renderer,
                       game.playerSheets[static_cast<std::size_t>(game.sheet)]
                           .get(),
                       &source, &destination);
        SDL_RenderSetClipRect(renderer, nullptr);
    }
    // Layers 201-202: bees, sleeping bees and the smoke cloud.
    for (int bee = 0; bee < game.beeCount; ++bee) {
        const auto index = static_cast<std::size_t>(bee);
        if (game.beeClips[index].added) {
            BeemazeClip clip = game.beeClips[index];
            clip.x = game.beeX[index] + kBoardX;
            clip.y = game.beeY[index] + kBoardY;
            drawMedia(game.bees, clip, true);
        }
        drawMedia(game.beeSleep, game.beeSleepClips[index], true);
    }
    drawMedia(game.smoke, game.smokeClip, true);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
BeemazeScene::beemazeHotspotAt(int x, int y) const {
    if (!beemazeGame || activeScene != "beemaze") return std::nullopt;
    const SDL_Rect start{225, 172, 350, 256};
    const SDL_Point point{x, y};
    if (beemazeGame->startClip.added && SDL_PointInRect(&point, &start)) {
        return std::pair{kStartHotspot, start};
    }
    return std::nullopt;
}

bool BeemazeScene::executeBeemazeDeveloperCommand(std::string_view command) {
    constexpr std::string_view beemazeControlPrefix = "beemaze:";
    if (activeScene != "beemaze" || !beemazeGame) return false;
    const std::string_view control =
        command.substr(beemazeControlPrefix.size());
    auto &game = *beemazeGame;
    if (control == "start") {
        game.initialized = true;
        if (!game.startClip.added) {
            beemazeAddClip(game.startClip, "start_icon", -1, -1, 150,
                           kStartHotspot);
        }
        startBeemazeGame();
    } else if (control.rfind("after:", 0) == 0) {
        // Holds the developer command queue until this scene time (seconds).
        return sceneElapsedNow() >= std::stod(std::string(control.substr(6)));
    } else if (control.rfind("seed:", 0) == 0) {
        // Matches the capture fixture in the x32dbg script.
        beemazeFixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 1) return false;
        if (game.startClip.added) startBeemazeGame();
        game.level = level - 1;
        beemazeRestartLevel();
    } else if (control == "up" || control == "left" || control == "down" ||
               control == "right") {
        const SDL_Keycode key = control == "up" ? SDLK_UP :
                                control == "left" ? SDLK_LEFT :
                                control == "down" ? SDLK_DOWN : SDLK_RIGHT;
        handleBeemazeKey(key, true);
    } else if (control == "release") {
        game.heldDirection = 0;
    } else if (control == "smoke" || control == "honey") {
        // Pick up the item as if Ludvig had reached it.
        const auto index = static_cast<std::size_t>(
            cellIndex(game.targetX / kCell, game.targetY / kCell));
        const int value =
            mapValue(game.level, game.targetX / kCell, game.targetY / kCell);
        if (value != 2 && value != 3) return false;
        game.cellNames[index] = control == "smoke" ? "gun" : "honey";
        game.cellAdded[index] = true;
        beemazePickup(game.targetX / kCell, game.targetY / kCell);
    } else if (control == "hit") {
        if (game.beeCount <= 0) return false;
        game.beeX[0] = game.playerX;
        game.beeY[0] = game.playerY;
    } else if (control == "finish") {
        game.sticks = 0;
        game.berries = 0;
        game.nuts = 0;
        game.mushrooms = 0;
    } else if (control == "status") {
        const auto active = std::count(game.cellAdded.begin(),
                                       game.cellAdded.end(), true);
        std::cout << "Bee Maze status: level " << game.level + 1
                  << ", score " << beemazeScore << ", lives " << game.lives
                  << ", playing " << game.playing << ", player "
                  << game.playerX << ',' << game.playerY << ", required "
                  << game.sticks + game.berries + game.nuts + game.mushrooms
                  << ", active items " << active << ", bees "
                  << game.beeCount << ", powerups " << game.powerups << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Bee Maze control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
