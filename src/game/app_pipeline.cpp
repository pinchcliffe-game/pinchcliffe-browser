#include "game/app_pipeline.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "media/smacker.h"

namespace fgp {
namespace {

// Board geometry: 0x0049eb80 (x), 0x0049eb84/88 (cell size) and the y
// origin written to 0x0067ee80 at 0x00448683.
constexpr int kGridX = 70;
constexpr int kGridY = 106;
constexpr int kCellSize = 56;
constexpr int kColumns = 10;
constexpr int kRows = 8;

// Hotspot ids: hotspots.bmp gives the board (1) and the seven stock rows
// (2-8); the controls created at 0x00448557-0x004485c3 carry 12-15.
constexpr std::uint32_t kGridHotspot = 1U;
constexpr std::uint32_t kStartControl = 14U;
constexpr std::uint32_t kTurboControl = 15U;
constexpr SDL_Rect kStartButton{138, 29, 102, 39};
constexpr SDL_Rect kStartText{146, 34, 57, 22};
constexpr SDL_Rect kTurboButton{463, 29, 96, 39};
constexpr SDL_Rect kTurboText{494, 34, 57, 22};
constexpr SDL_Rect kTurboMeter{453, 35, 7, 22};
// hotspots.bmp bounds; every id fills its rectangle completely.
constexpr SDL_Rect kGridHotspotBounds{69, 105, 561, 450};
constexpr std::array<SDL_Rect, 7> kStockHotspots{{
    {702, 157, 83, 59}, {702, 227, 83, 53}, {702, 288, 83, 53},
    {702, 348, 83, 53}, {702, 410, 83, 52}, {702, 470, 83, 53},
    {702, 530, 83, 51},
}};
// Text objects: stock counts 0x0049ef20 (757, 158 + 62 * row, 26x56),
// timer 0x0049ef00 and score 0x0049ef10 (left, top, right, bottom).
constexpr SDL_Rect kTimerText{314, 28, 68, 25};
constexpr SDL_Rect kScoreText{695, 91, 92, 28};

// The five 80-byte terrain masks at 0x0049ece8.
const auto kTerrainMasks = originalArray<std::array<std::uint8_t, 80>, 5>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049ece8U + static_cast<std::uint32_t>(index * 80);
    return source.read<std::array<std::uint8_t, 80>>(address);
});

// The 32-byte pipe records at 0x0049eba8: name, connection count, pipe
// width, oil capacity, north/east/south/west openings.
struct PipePiece {
    std::string_view name;
    int connections;
    int width;
    int capacity;
    std::array<int, 4> open;
};
const auto kPieces = originalArray<PipePiece, 10>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049eba8U + static_cast<std::uint32_t>(index * 32);
    return PipePiece{source.string(source.read<std::uint32_t>(address)),
        source.read<int>(address + 4), source.read<int>(address + 8), source.read<int>(address + 12),
        source.read<std::array<int, 4>>(address + 16)};
});
constexpr int kWell = 5;
constexpr int kRefinery = 6;
constexpr int kValve = 7;
// Stock rows, top to bottom: 0x0049eb8c.
const auto kStockTypes = originalArray<int, 7>(0x49eb8cU);
// Direction steps north, east, south, west: 0x0049eb60.
const auto kSteps = originalArray<std::array<int, 2>, 4>(0x49eb60U);
// Endpoint animations by level % 5: 0x004dd5c8.
const auto kWellNames = originalArray<std::string_view, 5>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004dd5c8U + static_cast<std::uint32_t>(index * 4);
    return source.string(source.read<std::uint32_t>(address));
});
// Score: base times 0x0049ee8c and multipliers 0x0049ee78.
const auto kBaseTimes = originalArray<int, 5>(0x49ee8cU);
const auto kMultipliers = originalArray<float, 5>(0x49ee78U);
// Score-table medal thresholds passed at 0x0044a7f8: 0x0049eea0.
constexpr int kRandomBuildAttempts = 10; // 0x0049eec8
constexpr int kKeepPathBelowLevel = 2;   // 0x0049eecc
constexpr int kCursorBelowLevel = 4;     // 0x0049eed0
constexpr float kMaximumTurbo = 10.0F;   // 0x0049eeb8

int opposite(int direction) { return (direction + 2) & 3; }

int stockSlot(int type) {
    // 0x00449c50.
    for (std::size_t slot = 0; slot < kStockTypes.size(); ++slot) {
        if (kStockTypes[slot] == type) return static_cast<int>(slot);
    }
    return -1;
}

bool opensTowards(int type, int rotation, int direction) {
    // 0x0044ac20.
    return kPieces[static_cast<std::size_t>(type)]
               .open[static_cast<std::size_t>((direction - rotation + 4) & 3)] != 0;
}

bool clampCell(int &column, int &row) {
    // 0x004497a0 clamps in place and reports whether it had to.
    bool inside = true;
    if (column < 0) { column = 0; inside = false; }
    if (column > kColumns - 1) { column = kColumns - 1; inside = false; }
    if (row < 0) { row = 0; inside = false; }
    if (row > kRows - 1) { row = kRows - 1; inside = false; }
    return inside;
}

void pointerCell(int &x, int &y) {
    // 0x0044a000: a negative quotient becomes -1, others truncate.
    const float column = static_cast<float>(x - kGridX) / kCellSize;
    const float row = static_cast<float>(y - kGridY) / kCellSize;
    x = column < 0.0F ? -1 : static_cast<int>(column);
    y = row < 0.0F ? -1 : static_cast<int>(row);
}

std::string cellName(int type, int rotation) {
    return std::string(kPieces[static_cast<std::size_t>(type)].name) + "-" +
           std::to_string(rotation);
}

void makeGreenTransparent(DecodedMedia &media) {
    // The endpoint Smackers predate palette-index alpha and keep a literal
    // bright-green matte. Keep the conversion local because green is valid
    // artwork in many other FGP animations.
    for (auto &frame : media.videoFrames) {
        for (std::size_t pixel = 0; pixel + 3U < frame.bgra.size();
             pixel += 4U) {
            const Uint8 blue = frame.bgra[pixel];
            const Uint8 green = frame.bgra[pixel + 1U];
            const Uint8 red = frame.bgra[pixel + 2U];
            if (green > 190U && red < 90U && blue < 90U) {
                frame.bgra[pixel + 3U] = 0U;
            }
        }
    }
}

} // namespace

PipelineScene::PipelineGame PipelineScene::loadPipelineGame(Package &package) {
    PipelineGame game;
    const auto texture = [&](std::string_view asset) {
        SurfacePtr surface = loadGreenKeyedBmp(package.data(package.find(asset)));
        TexturePtr result = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(result.get(), SDL_BLENDMODE_BLEND);
        return result;
    };
    for (int level = 0; level < 5; ++level) {
        game.levelTextures[static_cast<std::size_t>(level)] = texture(
            "bitmap\\level0" + std::to_string(level) + ".bmp");
    }
    // 0x004487b0 loads every family except none and point for the oil
    // overlay and keys each with the colour at (28,28) of straight-0;
    // native keeps all ten for drawing the cell descriptors. A transparent
    // descriptor's surface is keyed on its top-left pixel
    // (0x00413071-0x004130a2): green for pipes, black for none-*.
    for (std::size_t type = 0; type < kPieces.size(); ++type) {
        for (int rotation = 0; rotation < 4; ++rotation) {
            const std::string asset = "bitmap\\" +
                std::string(kPieces[type].name) + "-" +
                std::to_string(rotation) + ".bmp";
            SurfacePtr loaded = loadBmp(package.data(package.find(asset)));
            SurfacePtr argb(SDL_ConvertSurfaceFormat(
                loaded.get(), SDL_PIXELFORMAT_ARGB8888, 0));
            if (!argb) {
                throw std::runtime_error(std::string("SDL_ConvertSurfaceFormat failed: ") +
                                         SDL_GetError());
            }
            SDL_LockSurface(argb.get());
            const Uint32 key =
                *static_cast<const Uint32 *>(argb->pixels) & 0x00ffffffU;
            for (int y = 0; y < argb->h; ++y) {
                auto *row = reinterpret_cast<Uint32 *>(
                    static_cast<Uint8 *>(argb->pixels) + y * argb->pitch);
                for (int x = 0; x < argb->w; ++x) {
                    if ((row[x] & 0x00ffffffU) == key) row[x] = 0;
                }
            }
            SDL_UnlockSurface(argb.get());
            auto &image = game.pieces[type][static_cast<std::size_t>(rotation)];
            image.texture = makeTexture(renderer, argb.get());
            SDL_SetTextureBlendMode(image.texture.get(), SDL_BLENDMODE_BLEND);
            image.surface = std::move(argb);
        }
    }
    game.interiorColour = pixelValue(game.pieces[1][0].surface.get(), 28, 28) |
                          0xff000000U;
    game.startButtonTextures[0] = texture("bitmap\\start_0.bmp");
    game.startButtonTextures[1] = texture("bitmap\\start_1.bmp");
    game.turboButtonTextures[0] = texture("bitmap\\turbo_0.bmp");
    game.turboButtonTextures[1] = texture("bitmap\\turbo_1.bmp");
    game.turboMeterTexture = texture("bitmap\\turbo.bmp");

    for (const char *name : {"pump", "well", "platform", "lamp", "cursor"}) {
        DecodedMedia media =
            decodeSmacker(activeSceneAnimationData(name), true, false, true);
        makeGreenTransparent(media);
        if (!media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.front();
            TexturePtr frameTexture(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            SDL_SetTextureBlendMode(frameTexture.get(), SDL_BLENDMODE_BLEND);
            game.mediaTextures[name] = std::move(frameTexture);
        }
        game.media[name] = std::move(media);
    }
    for (const char *name : {"building-1", "rotating-1", "rotating-2",
                             "rotating-3", "running-1", "running-2",
                             "running-3", "running-4"}) {
        // The 4x4 video track carries the clip's length.
        game.media[name] =
            decodeSmacker(activeSceneAnimationData(name), true, true, true);
    }
    std::cout << "Loaded FGP.exe Pipeline port: 10x8 board, seeded route "
                 "builder, stock share-out, oil flow and turbo\n";
    return game;
}

void PipelineScene::announcePipelineEntry() {
    // Initializer 0x00448500: Help/Exit and the Start/Turbo controls on
    // layer 7, then the level00 backdrop (text objects carry no name).
    pipelineTraceAdd("help_0", -1, -1, 7, 12);
    pipelineTraceAdd("exit_0", -1, -1, 7, 13);
    pipelineTraceAdd("start_0", -1, -1, 7, static_cast<int>(kStartControl));
    pipelineTraceAdd("turbo_0", -1, -1, 7, static_cast<int>(kTurboControl));
    auto &game = *pipelineGame;
    game.levelName = "level00";
    pipelineTraceAdd(game.levelName, -1, -1, 0);
    game.levelAdded = true;
}

int PipelineScene::pipelineClock() {
    // 0x0040c450 reads the game clock in milliseconds.
    return static_cast<int>(std::llround(sceneElapsedNow() * 1000.0));
}

int PipelineScene::pipelineRandom(int bound) {
    // 0x0040c530: the C runtime rand() modulo the bound; rand() is called
    // even for a bound of 1.
    auto &game = *pipelineGame;
    if (bound == 0) return 0;
    if (!game.randomSeeded) {
        game.randomState = static_cast<std::uint32_t>(randomEngine());
        game.randomSeeded = true;
    }
    game.randomState = game.randomState * 0x343fdU + 0x269ec3U;
    return static_cast<int>((game.randomState >> 16) & 0x7fffU) % bound;
}

PipelineScene::PipelineCell &PipelineScene::pipelineCell(int column, int row) {
    return pipelineGame->cells[static_cast<std::size_t>(column + kColumns * row)];
}

void PipelineScene::pipelineTraceAdd(std::string_view asset, int x, int y,
                                     int layer, int hotspot) {
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", 0}});
}

void PipelineScene::pipelineTraceRemove(std::string_view asset) {
    // SCENE_RemoveAnim 0x00412aa0 logs before its nested StopAnim.
    referenceTrace.emit("animation_remove", {
        {"asset", asset}, {"reason", "removed"}});
    pipelineTraceStop(asset);
}

void PipelineScene::pipelineTraceStop(std::string_view asset) {
    referenceTrace.emit("animation_stop", {
        {"asset", asset}, {"reason", "stopped"}});
}

void PipelineScene::pipelineAddClip(PipelineClip &clip, int x, int y,
                                    int layer) {
    clip.added = true;
    pipelineTraceAdd(clip.name, x, y, layer);
}

void PipelineScene::pipelinePlayClip(PipelineClip &clip) {
    // SCENE_PlayAnim 0x004129d0.
    clip.playing = true;
    clip.started = sceneElapsedNow();
    const auto found = pipelineGame->media.find(clip.name);
    if (found != pipelineGame->media.end() &&
        !found->second.audioSamples.empty()) {
        if (clip.audio != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(clip.audio);
        }
        clip.audio = audioMixer.addEffect(found->second,
                                          audioVolumes[0] * sceneAudioGains[0]);
    }
}

void PipelineScene::pipelineStopClip(PipelineClip &clip) {
    // SCENE_StopAnim 0x00412a60.
    pipelineTraceStop(clip.name);
    clip.playing = false;
    if (clip.audio != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.audio);
        clip.audio = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void PipelineScene::pipelineRemoveClip(PipelineClip &clip) {
    referenceTrace.emit("animation_remove", {
        {"asset", clip.name}, {"reason", "removed"}});
    pipelineStopClip(clip);
    clip.added = false;
}

void PipelineScene::pipelinePlaySound(std::string_view family, int variants) {
    // 0x00449ec0 names "<family>-<rand(n)+1>" in the one-shot template
    // 0x004dd9d0 and plays a clone of it (0x00412c20); the clone is added
    // at the template's (0,0) on layer 0 and removed when it finishes.
    const int variant = pipelineRandom(variants) + 1;
    PipelineClip clone;
    clone.name = std::string(family) + "-" + std::to_string(variant);
    pipelineAddClip(clone, 0, 0, 0);
    pipelinePlayClip(clone);
    pipelineGame->clones.push_back(std::move(clone));
}

void PipelineScene::pipelineStartGame() {
    // 0x00448880, from Start control 14 while inactive.
    auto &game = *pipelineGame;
    game.level = 0;
    game.score = 0;
    pipelineSetupLevel();
    game.active = true;
    game.buildStart = pipelineClock();
    game.held = -1;
    game.heldRotation = 0;
}

void PipelineScene::pipelineSetupLevel() {
    // 0x004488d0.
    auto &game = *pipelineGame;
    pipelineLayTerrain();
    pipelineRandomBuild();
    pipelineComplicate();
    pipelineShareInventory();
    pipelineRedrawBoard();
    for (int slot = 0; slot < 7; ++slot) pipelineSetInventoryText(slot);
    pipelineSetTurbo(0.0F);
    game.reachedEnd = false;
}

void PipelineScene::pipelineLayTerrain() {
    // 0x00448b40.
    auto &game = *pipelineGame;
    const int terrain = game.level % 5;
    if (game.levelAdded) pipelineTraceRemove(game.levelName);
    char name[16]{};
    std::snprintf(name, sizeof(name), "level%02d", terrain);
    game.levelName = name;
    pipelineTraceAdd(game.levelName, -1, -1, 0);
    game.levelAdded = true;
    game.flowRate = static_cast<int>(
        (3.0 - 8.0 / static_cast<double>(game.level + 4)) * 10000.0);
    game.buildDuration = static_cast<int>(
        10.0 / static_cast<double>(game.level + 10) * 20000.0);
    game.maximumRoute = 20;
    // 0x00448bef-0x00448c01 keeps 10 + 2 * level only when it exceeds the
    // maximum; otherwise the minimum becomes the maximum itself.
    game.minimumRoute = 10 + game.level * 2;
    if (game.minimumRoute <= game.maximumRoute) {
        game.minimumRoute = game.maximumRoute;
    }
    const auto mask = kTerrainMasks[static_cast<std::size_t>(terrain)];
    for (std::size_t index = 0; index < game.cells.size(); ++index) {
        auto &cell = game.cells[index];
        cell.terrain = mask[index];
        cell.type = 0;
        cell.rotation = 0;
        cell.oil = 0;
        cell.arms = {};
        pipelineRedrawCell(cell);
    }
}

void PipelineScene::pipelineRandomBuild() {
    // 0x00448c70. Capture fixture, not original behaviour: the reference
    // x32dbg adapter reseeds rand() to the fixed seed on entry here.
    auto &game = *pipelineGame;
    if (game.fixedSeed) {
        game.randomState = *game.fixedSeed;
        game.randomSeeded = true;
    }
    for (int attempt = 0; attempt < kRandomBuildAttempts; ++attempt) {
        if (!pipelineBuildRoute(true)) continue;
        const int length = pipelineExtractRoute();
        if (length >= game.minimumRoute && length <= game.maximumRoute) {
            return;
        }
    }
    // "Random build failed, using sized pipe."
    pipelineBuildRoute(false);
    pipelineExtractRoute();
}

int PipelineScene::pipelineRandomDirection(int column, int row) {
    // 0x004492f0: from a random direction, the first open neighbour.
    const int first = pipelineRandom(4);
    int direction = first;
    do {
        const int nextColumn = column + kSteps[static_cast<std::size_t>(direction)][0];
        const int nextRow = row + kSteps[static_cast<std::size_t>(direction)][1];
        if (nextColumn >= 0 && nextRow >= 0 && nextColumn < kColumns &&
            nextRow < kRows && pipelineCell(nextColumn, nextRow).terrain == 0) {
            return direction;
        }
        direction = (direction + 1) & 3;
    } while (direction != first);
    return -1;
}

bool PipelineScene::pipelineBuildRoute(bool randomEnds) {
    // 0x00448ce0: best-first search over the board.
    auto &game = *pipelineGame;
    game.nodes.fill(PipelineNode{});
    do {
        game.startColumn = pipelineRandom(10);
        game.startRow = pipelineRandom(8);
    } while (pipelineCell(game.startColumn, game.startRow).terrain != 0);
    if (randomEnds) {
        for (;;) {
            game.endColumn = pipelineRandom(10);
            game.endRow = pipelineRandom(8);
            if (game.endColumn == game.startColumn &&
                game.endRow == game.startRow) continue;
            if (pipelineCell(game.endColumn, game.endRow).terrain != 0) continue;
            break;
        }
        game.startDirection = pipelineRandomDirection(game.startColumn, game.startRow);
        game.endDirection = pipelineRandomDirection(game.endColumn, game.endRow);
        if (game.startDirection < 0 || game.endDirection < 0) return false;
    } else {
        game.startDirection = -1;
        game.endDirection = -1;
    }
    const auto distanceToEnd = [&](int column, int row) {
        return std::abs(game.endColumn - column) + std::abs(game.endRow - row);
    };
    const auto distanceFromStart = [&](int column, int row) {
        return std::abs(row - game.startRow) + std::abs(column - game.startColumn);
    };
    auto &start = game.nodes[static_cast<std::size_t>(
        game.startColumn + kColumns * game.startRow)];
    start.g = 0;
    start.h = randomEnds ? distanceToEnd(game.startColumn, game.startRow)
                         : distanceFromStart(game.startColumn, game.startRow);
    start.open = 1;
    const auto anyOpen = [&] {
        return std::any_of(game.nodes.begin(), game.nodes.end(),
                           [](const PipelineNode &node) { return node.open == 1; });
    };
    while (anyOpen()) {
        // 0x00448fc0: the first open node with the lowest f, row-major.
        int best = 0x7fffffff;
        int column = 0;
        int row = 0;
        for (int index = 0; index < 80; ++index) {
            const auto &node = game.nodes[static_cast<std::size_t>(index)];
            if (node.open == 1 && node.f < best) {
                best = node.f;
                column = index % kColumns;
                row = index / kColumns;
            }
        }
        if (randomEnds) {
            if (column == game.endColumn && row == game.endRow) return true;
        } else {
            // "Trying end-point as (%d, %d)."
            game.endColumn = column;
            game.endRow = row;
            if (std::abs(game.endRow - game.startRow) +
                    std::abs(game.endColumn - game.startColumn) >=
                game.minimumRoute) {
                return true;
            }
        }
        pipelineExpandNode(column, row, randomEnds);
        game.nodes[static_cast<std::size_t>(column + kColumns * row)].open = 2;
    }
    // "Build failed."
    return !randomEnds;
}

void PipelineScene::pipelineExpandNode(int column, int row, bool randomEnds) {
    // 0x00449040.
    auto &game = *pipelineGame;
    const int current = column + kColumns * row;
    for (int direction = 0; direction < 4; ++direction) {
        const int dx = kSteps[static_cast<std::size_t>(direction)][0];
        const int dy = kSteps[static_cast<std::size_t>(direction)][1];
        if (game.startDirection > -1 && column == game.startColumn &&
            row == game.startRow && direction != game.startDirection) {
            continue;
        }
        const int nextColumn = column + dx;
        const int nextRow = row + dy;
        if (nextColumn < 0 || nextRow < 0 || nextColumn >= kColumns ||
            nextRow >= kRows) {
            continue;
        }
        // 0x004490d5-0x004490e3 compares the end column with the next
        // cell but the end row with the current one, so only horizontal
        // entries into the end are forced to face its opening.
        if (game.endDirection > -1 && nextColumn == game.endColumn &&
            row == game.endRow && direction != opposite(game.endDirection)) {
            continue;
        }
        // Bridges (terrain 1) are crossed straight: the cell behind a
        // bridge and the one after it must be open land. No terrain-1
        // cell lies on the board's edge, so these stay on the board.
        if (game.cells[static_cast<std::size_t>(current)].terrain == 1 &&
            game.cells[static_cast<std::size_t>(current - dx - kColumns * dy)]
                    .terrain != 0) {
            continue;
        }
        const int next = nextColumn + kColumns * nextRow;
        if (game.cells[static_cast<std::size_t>(next)].terrain == 1 &&
            game.cells[static_cast<std::size_t>(next + dx + kColumns * dy)]
                    .terrain != 0) {
            continue;
        }
        const int h = randomEnds
            ? std::abs(game.endColumn - nextColumn) + std::abs(game.endRow - nextRow)
            : std::abs(nextRow - game.startRow) + std::abs(nextColumn - game.startColumn);
        if (game.cells[static_cast<std::size_t>(next)].terrain == 2) continue;
        auto &node = game.nodes[static_cast<std::size_t>(next)];
        const int g = game.nodes[static_cast<std::size_t>(current)].g;
        if (node.open != 0 && node.f <= g + h) continue;
        node.open = 1;
        node.h = h;
        node.g = g + 1;
        node.f = node.g + h;
        node.parentColumn = column;
        node.parentRow = row;
    }
}

void PipelineScene::pipelinePlaceRoutePiece(PipelineCell &cell, int nextColumn,
                                            int nextRow, int column, int row,
                                            int previousColumn,
                                            int previousRow) {
    // 0x00449c70 with its 14-way table at 0x00449d48.
    const auto side = [&](int otherColumn, int otherRow) {
        if (otherRow < row) return 0;
        if (otherColumn > column) return 1;
        return otherRow > row ? 2 : 3;
    };
    static constexpr std::array<std::array<int, 2>, 14> kTable{{
        {2, 3}, {1, 0}, {2, 2}, {2, 3}, {-1, 0}, {2, 0}, {1, 1},
        {1, 0}, {2, 0}, {-1, 0}, {2, 1}, {2, 2}, {1, 1}, {2, 1},
    }};
    const int index = side(nextColumn, nextRow) + side(previousColumn, previousRow) * 4 - 1;
    if (index < 0 || index > 13 || kTable[static_cast<std::size_t>(index)][0] < 0) {
        return;
    }
    cell.type = kTable[static_cast<std::size_t>(index)][0];
    cell.rotation = kTable[static_cast<std::size_t>(index)][1];
}

int PipelineScene::pipelineExtractRoute() {
    // 0x00449360: lay the found route back from the end; returns its length.
    auto &game = *pipelineGame;
    const auto node = [&](int column, int row) -> PipelineNode & {
        return game.nodes[static_cast<std::size_t>(column + kColumns * row)];
    };
    const auto rotationTowards = [](int column, int row, int otherColumn,
                                    int otherRow) {
        if (row > otherRow) return 0;
        if (column < otherColumn) return 1;
        return row < otherRow ? 2 : 3;
    };
    const int parentColumn = node(game.endColumn, game.endRow).parentColumn;
    const int parentRow = node(game.endColumn, game.endRow).parentRow;
    int previousColumn = node(parentColumn, parentRow).parentColumn;
    int previousRow = node(parentColumn, parentRow).parentRow;
    // Rotations survive: only types and the path flags are cleared.
    for (auto &cell : game.cells) {
        cell.type = 0;
        cell.path = false;
    }
    auto &end = pipelineCell(game.endColumn, game.endRow);
    end.type = kRefinery;
    end.rotation = rotationTowards(game.endColumn, game.endRow, parentColumn,
                                   parentRow);
    pipelinePlaceRoutePiece(pipelineCell(parentColumn, parentRow),
                            game.endColumn, game.endRow, parentColumn,
                            parentRow, previousColumn, previousRow);
    int length = 3;
    int nextColumn = parentColumn;
    int nextRow = parentRow;
    int column = previousColumn;
    int row = previousRow;
    while (column >= 0 && row >= 0) {
        const int beforeColumn = node(column, row).parentColumn;
        const int beforeRow = node(column, row).parentRow;
        // The piece whose predecessor is the start (parent -1) is flagged.
        // When the walk reaches the start itself, 0x004494a5 reads the
        // parent of node (-1,-1) outside the node array; that only ever
        // flags the start cell, which no later pass reads, so native skips
        // that read.
        if (beforeColumn >= 0 && beforeRow >= 0 &&
            node(beforeColumn, beforeRow).parentColumn < 0) {
            pipelineCell(column, row).path = true;
        }
        if (beforeColumn <= -1 || beforeRow <= -1) break;
        pipelinePlaceRoutePiece(pipelineCell(column, row), nextColumn, nextRow,
                                column, row, beforeColumn, beforeRow);
        ++length;
        nextColumn = column;
        nextRow = row;
        column = beforeColumn;
        row = beforeRow;
    }
    auto &start = pipelineCell(column, row);
    start.type = kWell;
    start.rotation = rotationTowards(column, row, nextColumn, nextRow);
    return length;
}

bool PipelineScene::pipelineCellFree(int column, int row) {
    // 0x004497f0.
    if (column < 0 || column >= kColumns || row < 0 || row >= kRows) return false;
    const auto &cell = pipelineCell(column, row);
    return cell.type == 0 && cell.terrain != 2;
}

bool PipelineScene::pipelineBendToCross(int column, int row) {
    // 0x00449650: a bend becomes a cross with valves on both free sides.
    auto &cell = pipelineCell(column, row);
    const int first = cell.rotation;
    const int second = (cell.rotation + 3) & 3;
    int firstColumn = column + kSteps[static_cast<std::size_t>(first)][0];
    int firstRow = row + kSteps[static_cast<std::size_t>(first)][1];
    int secondColumn = column + kSteps[static_cast<std::size_t>(second)][0];
    int secondRow = row + kSteps[static_cast<std::size_t>(second)][1];
    if (!clampCell(firstColumn, firstRow)) return false;
    if (!clampCell(secondColumn, secondRow)) return false;
    if (!pipelineCellFree(firstColumn, firstRow)) return false;
    if (!pipelineCellFree(secondColumn, secondRow)) return false;
    cell.type = 4;
    auto &firstValve = pipelineCell(firstColumn, firstRow);
    firstValve.type = kValve;
    firstValve.rotation = opposite(first);
    auto &secondValve = pipelineCell(secondColumn, secondRow);
    secondValve.type = kValve;
    secondValve.rotation = opposite(second);
    return true;
}

bool PipelineScene::pipelineBendToTee(int column, int row) {
    // 0x00449830: a bend becomes a tee with a valve on one free side.
    // The loop counts its pass in ebp but picks the side from edi, the
    // random draw itself (0x0044986a), so both passes try the same side;
    // the new rotation then uses ebp.
    auto &cell = pipelineCell(column, row);
    const int first = pipelineRandom(2);
    int side = first;
    do {
        int direction = cell.rotation;
        if (first == 0) direction = (direction + 3) & 3;
        int nextColumn = column + kSteps[static_cast<std::size_t>(direction)][0];
        int nextRow = row + kSteps[static_cast<std::size_t>(direction)][1];
        if (clampCell(nextColumn, nextRow) &&
            pipelineCellFree(nextColumn, nextRow)) {
            auto &valve = pipelineCell(nextColumn, nextRow);
            valve.type = kValve;
            valve.rotation = opposite(direction);
            cell.type = 3;
            cell.rotation = (cell.rotation + (side == 0 ? 1 : 0) + 1) & 3;
            return true;
        }
        side = (side + 1) & 1;
    } while (side != first);
    return false;
}

bool PipelineScene::pipelineStraightToTee(int column, int row) {
    // 0x00449950: a straight gains a valved branch on one or both sides.
    auto &cell = pipelineCell(column, row);
    const std::array<int, 2> directions{(cell.rotation + 1) & 3,
                                        (cell.rotation + 3) & 3};
    std::array<int, 2> columns{};
    std::array<int, 2> rows{};
    for (std::size_t side = 0; side < 2; ++side) {
        columns[side] = column + kSteps[static_cast<std::size_t>(directions[side])][0];
        rows[side] = row + kSteps[static_cast<std::size_t>(directions[side])][1];
    }
    const int first = pipelineRandom(2);
    int side = first;
    int added = 0;
    do {
        const auto s = static_cast<std::size_t>(side);
        if (clampCell(columns[s], rows[s]) &&
            pipelineCellFree(columns[s], rows[s])) {
            cell.type = 3;
            cell.rotation = directions[s];
            auto &valve = pipelineCell(columns[s], rows[s]);
            valve.type = kValve;
            valve.rotation = opposite(directions[s]);
            ++added;
        }
        side = (side + 1) & 1;
    } while (side != first);
    if (added == 2) cell.type = 4;
    return added != 0;
}

void PipelineScene::pipelineComplicate() {
    // 0x00449590: 2^level branch complications along the route.
    auto &game = *pipelineGame;
    int remaining = static_cast<int>(std::pow(2.0, game.level));
    for (int row = 0; row < kRows && remaining != 0; ++row) {
        for (int column = 0; column < kColumns && remaining != 0; ++column) {
            const auto &cell = pipelineCell(column, row);
            if (cell.path && game.level < kKeepPathBelowLevel) continue;
            if (cell.type == 1) {
                if (pipelineRandom(5) == 0 && pipelineStraightToTee(column, row)) {
                    --remaining;
                }
            } else if (cell.type == 2) {
                if (pipelineBendToCross(column, row) ||
                    pipelineBendToTee(column, row)) {
                    --remaining;
                }
            }
        }
    }
}

void PipelineScene::pipelineShareInventory() {
    // 0x00449a90: lift the route into the stock rows.
    auto &game = *pipelineGame;
    const int straightSlot = stockSlot(1);
    game.inventory.fill(0);
    game.pieceCount = 0;
    int bridges = 0;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            auto &cell = pipelineCell(column, row);
            game.solution[static_cast<std::size_t>(column + kColumns * row)] =
                {cell.type, cell.rotation};
            const int slot = stockSlot(cell.type);
            if (slot < 0) continue;
            ++game.pieceCount;
            if (cell.path) {
                if (game.level < kCursorBelowLevel) {
                    const int x = kGridX + column * kCellSize;
                    const int y = kGridY + row * kCellSize;
                    if (!game.cursor.added) pipelineAddClip(game.cursor, x, y, 5);
                    game.cursorX = x;
                    game.cursorY = y;
                    pipelinePlayClip(game.cursor);
                }
                if (game.level < kKeepPathBelowLevel) continue;
            }
            const int count = pipelineRandom(game.level + 1) != 0 ? 1 : 2;
            cell.type = 0;
            game.inventory[static_cast<std::size_t>(slot)] += count;
            if (cell.terrain == 1) ++bridges;
        }
    }
    while (game.inventory[static_cast<std::size_t>(straightSlot)] > bridges) {
        if (pipelineRandom(2) == 0) break;
        const int type = pipelineRandom(2) != 0 ? 8 : 9;
        --game.inventory[static_cast<std::size_t>(straightSlot)];
        ++game.inventory[static_cast<std::size_t>(stockSlot(type))];
    }
}

void PipelineScene::pipelineRedrawCell(PipelineCell &cell) {
    // 0x00448940: replace the cell descriptor, and re-add the endpoint
    // animation over the well (layer 4, -5,-15) or refinery (-9,-5).
    auto &game = *pipelineGame;
    const int index = static_cast<int>(&cell - game.cells.data());
    const int x = kGridX + (index % kColumns) * kCellSize;
    const int y = kGridY + (index / kColumns) * kCellSize;
    if (cell.added) pipelineTraceRemove(cell.name);
    cell.name = cellName(cell.type, cell.rotation);
    pipelineTraceAdd(cell.name, -1, -1, 3);
    cell.added = true;
    cell.oilDirty = true;
    if (cell.type == kWell) {
        if (game.well.added) pipelineRemoveClip(game.well);
        game.well.name = std::string(kWellNames[static_cast<std::size_t>(game.level % 5)]);
        pipelineAddClip(game.well, x - 5, y - 15, 4);
        game.wellX = x - 5;
        game.wellY = y - 15;
        pipelinePlayClip(game.well);
    }
    if (cell.type == kRefinery) {
        if (game.lamp.added) pipelineRemoveClip(game.lamp);
        pipelineAddClip(game.lamp, x - 9, y - 5, 4);
        game.lampX = x - 9;
        game.lampY = y - 5;
        pipelinePlayClip(game.lamp);
    }
}

void PipelineScene::pipelineRedrawBoard() {
    // 0x00448910.
    for (auto &cell : pipelineGame->cells) pipelineRedrawCell(cell);
}

void PipelineScene::pipelineSetInventoryText(int slot) {
    // 0x00448a80.
    auto &game = *pipelineGame;
    game.inventoryText[static_cast<std::size_t>(slot)] =
        std::to_string(game.inventory[static_cast<std::size_t>(slot)]);
}

void PipelineScene::pipelineSetTurbo(float value) {
    // 0x00448ad0 clamps to 0..10; the progress object shows value / 10.
    pipelineGame->turbo = std::clamp(value, 0.0F, kMaximumTurbo);
}

void PipelineScene::pipelineRefreshHeld() {
    // 0x00449f30: re-add the carried piece on layer 11 where its
    // descriptor last stood (0x004ddb8c/90 are its runtime position,
    // descriptor +0x6c/+0x70), snapped onto the cell that corner lies over.
    auto &game = *pipelineGame;
    if (game.heldAdded) {
        pipelineTraceRemove(game.heldName);
        game.heldAdded = false;
    }
    if (game.held < 0) return;
    game.heldName = cellName(kStockTypes[static_cast<std::size_t>(game.held)],
                             game.heldRotation);
    pipelineTraceAdd(game.heldName, game.heldX, game.heldY, 11);
    game.heldAdded = true;
    int column = game.heldX;
    int row = game.heldY;
    pointerCell(column, row);
    if (clampCell(column, row)) {
        game.heldX = kGridX + column * kCellSize;
        game.heldY = kGridY + row * kCellSize;
    }
}

void PipelineScene::pipelineReturnHeld() {
    // 0x00449ef0.
    auto &game = *pipelineGame;
    if (game.held < 0) return;
    ++game.inventory[static_cast<std::size_t>(game.held)];
    pipelineSetInventoryText(game.held);
    game.held = -1;
    pipelineRefreshHeld();
}

void PipelineScene::pipelineTakeFromInventory(int slot, int x, int y) {
    // 0x0044a090.
    auto &game = *pipelineGame;
    if (game.inventory[static_cast<std::size_t>(slot)] <= 0) return;
    --game.inventory[static_cast<std::size_t>(slot)];
    pipelineSetInventoryText(slot);
    game.heldX = x - kCellSize / 2;
    game.heldY = y - kCellSize / 2;
    game.held = slot;
    game.heldRotation = 0;
    pipelineRefreshHeld();
}

bool PipelineScene::pipelinePlaceHeld(int x, int y) {
    // 0x0044a100.
    auto &game = *pipelineGame;
    if (game.held < 0) return false;
    int column = x;
    int row = y;
    pointerCell(column, row);
    if (!clampCell(column, row) || !pipelineCellFree(column, row)) return false;
    auto &cell = pipelineCell(column, row);
    const int type = kStockTypes[static_cast<std::size_t>(game.held)];
    // Bridges only take straights, vents and valves.
    if (cell.terrain == 1 && type != 1 && type != 9 && type != kValve) {
        return false;
    }
    if (game.cursor.added) pipelineRemoveClip(game.cursor);
    cell.type = type;
    cell.rotation = game.heldRotation;
    cell.oil = 0;
    cell.arms = {};
    pipelineRedrawCell(cell);
    game.held = -1;
    pipelineRefreshHeld();
    return true;
}

bool PipelineScene::pipelinePickUp(int x, int y) {
    // 0x0044a200: lift a dry piece; it does not go back into the stock.
    auto &game = *pipelineGame;
    int column = x;
    int row = y;
    pointerCell(column, row);
    if (!clampCell(column, row)) return false;
    auto &cell = pipelineCell(column, row);
    if (cell.type == 0 || cell.type == kWell || cell.type == kRefinery ||
        cell.oil > 0) {
        return false;
    }
    game.held = stockSlot(cell.type);
    game.heldRotation = cell.rotation;
    game.heldX = kGridX + column * kCellSize;
    game.heldY = kGridY + row * kCellSize;
    pipelineRefreshHeld();
    cell.type = 0;
    pipelineRedrawCell(cell);
    return true;
}

bool PipelineScene::pipelineRotatePlaced(int x, int y) {
    // 0x0044a3f0.
    int column = x;
    int row = y;
    pointerCell(column, row);
    if (!clampCell(column, row)) return false;
    auto &cell = pipelineCell(column, row);
    if (cell.type == 0 || cell.type == kWell || cell.type == kRefinery ||
        cell.oil > 0) {
        return false;
    }
    cell.rotation = (cell.rotation + 1) & 3;
    pipelineRedrawCell(cell);
    return true;
}

void PipelineScene::pipelineHotspotPress(std::uint32_t hotspot, int x, int y) {
    // Event 0x105 (every left press) reaches 0x00449e20 with the hotspot
    // under the pointer, 0 for none; it acts only while a round runs.
    auto &game = *pipelineGame;
    if (!game.active || hotspot > 8U) return;
    if (hotspot == 0U) {
        pipelineReturnHeld();
    } else if (hotspot == kGridHotspot) {
        const bool changed = game.held >= 0 ? pipelinePlaceHeld(x, y)
                                            : pipelinePickUp(x, y);
        if (changed) pipelinePlaySound("building", 1);
    } else {
        pipelineReturnHeld();
        pipelineTakeFromInventory(static_cast<int>(hotspot) - 2, x, y);
    }
}

void PipelineScene::pipelineControl(std::uint32_t control) {
    // Control event 0x116: 0x0044b580.
    auto &game = *pipelineGame;
    if (control == kStartControl) {
        if (!game.active) pipelineStartGame();
    } else if (control == kTurboControl && game.active) {
        // 0x0044b5b4-0x0044b5d6 ends a running build at once.
        const int now = pipelineClock();
        if (now < game.buildStart + game.buildDuration) {
            game.buildDuration = pipelineClock() - game.buildStart;
        }
        if (game.turbo < kMaximumTurbo) game.turbo += 1.0F;
        pipelineSetTurbo(game.turbo);
    }
}

bool PipelineScene::beginPipelineInteraction(int x, int y) {
    if (!pipelineGame || activeScene != "pipeline") return false;
    auto &game = *pipelineGame;
    const auto hit = pipelineHotspotAt(x, y);
    const std::uint32_t hotspot = hit ? hit->first : 0U;
    referenceTrace.emit("click_dispatch", {
        {"scene", "pipeline"}, {"hotspot", hotspot}});
    if (hotspot == kStartControl || hotspot == kTurboControl) {
        // The control shows its pressed bitmap until the button is released.
        const std::string_view base = hotspot == kStartControl ? "start" : "turbo";
        pipelineTraceRemove(std::string(base) + "_0");
        pipelineTraceAdd(std::string(base) + "_1", -1, -1, 7,
                         static_cast<int>(hotspot));
        game.pressedControl = hotspot;
    }
    pipelineHotspotPress(hotspot, x, y);
    return true;
}

void PipelineScene::endPipelineInteraction(int x, int y) {
    if (!pipelineGame) return;
    auto &game = *pipelineGame;
    const std::uint32_t control = game.pressedControl;
    if (control == 0U) return;
    game.pressedControl = 0U;
    const std::string_view base = control == kStartControl ? "start" : "turbo";
    pipelineTraceRemove(std::string(base) + "_1");
    pipelineTraceAdd(std::string(base) + "_0", -1, -1, 7,
                     static_cast<int>(control));
    const auto hit = pipelineHotspotAt(x, y);
    if (hit && hit->first == control) pipelineControl(control);
}

bool PipelineScene::rotatePipelineAt(int x, int y) {
    // Event 0x107 (right press): 0x0044a370.
    if (!pipelineGame || activeScene != "pipeline") return false;
    auto &game = *pipelineGame;
    if (!game.active) return false;
    if (game.held >= 0) {
        // 0x0044a3c0.
        game.heldRotation = (game.heldRotation + 1) & 3;
        pipelineRefreshHeld();
        pipelinePlaySound("rotating", 3);
    } else if (pipelineRotatePlaced(x, y)) {
        pipelinePlaySound("rotating", 3);
    }
    return true;
}

void PipelineScene::movePipelinePointer(int x, int y) {
    // Event 0x108 while carrying: 0x0044a2f0 snaps onto the cell under the
    // pointer or centres the piece on it (0x00412cf0).
    if (!pipelineGame || !pipelineGame->active || pipelineGame->held < 0) return;
    auto &game = *pipelineGame;
    int column = x;
    int row = y;
    pointerCell(column, row);
    if (clampCell(column, row)) {
        game.heldX = kGridX + column * kCellSize;
        game.heldY = kGridY + row * kCellSize;
    } else {
        game.heldX = x - kCellSize / 2;
        game.heldY = y - kCellSize / 2;
    }
}

void PipelineScene::pipelineUpdateTimer(int now) {
    // 0x0044a530: build time counts up to zero as "-mm:ss", flow after it.
    auto &game = *pipelineGame;
    const float seconds = static_cast<float>(now - game.buildDuration -
                                             game.buildStart) * 0.001F;
    const int whole = static_cast<int>(seconds > 0.0F ? seconds + 0.5F
                                                      : seconds - 0.5F);
    const int magnitude = std::abs(whole);
    char text[16]{};
    std::snprintf(text, sizeof(text), whole < 0 ? "-%02d:%02d" : "%02d:%02d",
                  magnitude / 60, magnitude % 60);
    game.timerText = text;
}

void PipelineScene::pipelineFillArms(int fromDirection, PipelineCell &cell,
                                     int amount) {
    // 0x0044b3f0: how far the oil has reached along each opening.
    const auto &piece = kPieces[static_cast<std::size_t>(cell.type)];
    const int perArm = piece.capacity / piece.connections;
    const int entry = opposite(fromDirection);
    cell.oil += amount;
    if (cell.oil >= piece.capacity) {
        for (auto &arm : cell.arms) {
            arm[0] = arm[0] != 0 ? perArm : 0;
            arm[1] = arm[1] != 0 ? perArm : 0;
        }
        return;
    }
    auto &inlet = cell.arms[static_cast<std::size_t>(entry)];
    inlet[0] += amount;
    if (inlet[0] + inlet[1] <= perArm) return;
    int excess = inlet[0] + inlet[1] - perArm;
    inlet[0] = perArm;
    inlet[1] = 0;
    if (piece.connections <= 1) return;
    std::array<int, 4> open{};
    int count = 0;
    for (int direction = 0; direction < 4; ++direction) {
        if (direction != entry &&
            opensTowards(cell.type, cell.rotation, direction)) {
            open[static_cast<std::size_t>(direction)] = 1;
            ++count;
        }
    }
    while (count > 0 && excess > 0) {
        // 0x0044b2d0 splits evenly, the remainder going to the last arm.
        std::array<int, 4> share{};
        std::vector<int> slots;
        for (int direction = 0; direction < 4; ++direction) {
            if (open[static_cast<std::size_t>(direction)]) slots.push_back(direction);
        }
        if (slots.size() == 1) {
            share[static_cast<std::size_t>(slots[0])] = excess;
        } else if (slots.size() == 2) {
            share[static_cast<std::size_t>(slots[0])] = excess / 2;
            share[static_cast<std::size_t>(slots[1])] = excess - excess / 2;
        } else {
            share[static_cast<std::size_t>(slots[0])] = excess / 3;
            share[static_cast<std::size_t>(slots[1])] = excess / 3;
            share[static_cast<std::size_t>(slots[2])] = excess - (2 * excess) / 3;
        }
        excess = 0;
        for (int direction = 0; direction < 4; ++direction) {
            const auto d = static_cast<std::size_t>(direction);
            if (!open[d]) continue;
            auto &arm = cell.arms[d];
            arm[1] += share[d];
            const int sum = arm[0] + arm[1];
            if (sum > perArm) {
                excess += sum - perArm;
                arm[0] = perArm;
                arm[1] = 0;
                open[d] = 0;
                --count;
            }
        }
    }
}

int PipelineScene::pipelineFlow(int fromDirection, int column, int row,
                                int amount, const PipelineVisit *visited) {
    // 0x0044a9a0: push oil into a cell; -1 on a leak, else what it could
    // not take.
    auto &game = *pipelineGame;
    auto &cell = pipelineCell(column, row);
    const auto &piece = kPieces[static_cast<std::size_t>(cell.type)];
    const bool full = cell.oil >= piece.capacity;
    if (cell.type == kRefinery) game.reachedEnd = true;
    for (const PipelineVisit *visit = visited; visit != nullptr; visit = visit->next) {
        if (visit->cell == &cell) return amount;
    }
    pipelineFillArms(fromDirection, cell, amount);
    int overflow = cell.oil - piece.capacity;
    if (overflow <= 0) {
        if (!full) cell.oilDirty = true;
        return 0;
    }
    cell.oil -= overflow;
    const PipelineVisit here{&cell, visited};
    std::array<int, 4> slots{};
    int count = 0;
    for (int direction = 0; direction < 4; ++direction) {
        const auto d = static_cast<std::size_t>(direction);
        const int nextColumn = column + kSteps[d][0];
        const int nextRow = row + kSteps[d][1];
        if (!opensTowards(cell.type, cell.rotation, direction)) continue;
        if (nextColumn < 0 || nextColumn >= kColumns || nextRow < 0 ||
            nextRow >= kRows) {
            slots[d] = 1;
            ++count;
            continue;
        }
        const auto &next = pipelineCell(nextColumn, nextRow);
        if (visited != nullptr && visited->cell == &next) continue;
        slots[d] = opensTowards(next.type, next.rotation, opposite(direction)) ? 2 : 1;
        ++count;
    }
    while (count > 0 && overflow > 0) {
        std::array<int, 4> results{};
        std::vector<std::size_t> active;
        for (std::size_t d = 0; d < 4; ++d) {
            if (slots[d] != 0) active.push_back(d);
        }
        if (active.size() == 1) {
            results[active[0]] = overflow;
        } else if (active.size() == 2) {
            results[active[0]] = overflow / 2;
            results[active[1]] = overflow - overflow / 2;
        } else if (active.size() == 3) {
            results[active[0]] = overflow / 3;
            results[active[1]] = overflow / 3;
            results[active[2]] = overflow - (2 * overflow) / 3;
        }
        for (int direction = 0; direction < 4; ++direction) {
            const auto d = static_cast<std::size_t>(direction);
            if (slots[d] == 0) continue;
            if (slots[d] == 1) return -1;
            const int taken = pipelineFlow(direction, column + kSteps[d][0],
                                           row + kSteps[d][1], results[d], &here);
            results[d] = taken;
            if (taken < 0) return -1;
            if (taken > 0) {
                slots[d] = 0;
                --count;
            }
        }
        overflow = results[0] + results[1] + results[2] + results[3];
    }
    if (!full) cell.oilDirty = true;
    return overflow;
}

int PipelineScene::pipelineFlowStep(float seconds) {
    // 0x0044a930: start the looping pump sound once, then push
    // (turbo + 1) * rate * seconds units from the well.
    auto &game = *pipelineGame;
    if (!game.running.playing) {
        game.running.name = "running-" + std::to_string(pipelineRandom(4) + 1);
        // 0x00412b90 adds the descriptor only when it is not in the scene.
        if (!game.running.added) pipelineAddClip(game.running, 0, 0, 0);
        pipelinePlayClip(game.running);
    }
    const int amount = static_cast<int>(
        (static_cast<double>(game.turbo) + 1.0) * game.flowRate * seconds);
    return pipelineFlow(0, game.startColumn, game.startRow, amount, nullptr);
}

void PipelineScene::pipelineTick(int now) {
    // Event 0x112: 0x0044a480.
    auto &game = *pipelineGame;
    if (!game.active) return;
    if (game.lastTick == 0) game.lastTick = game.buildStart;
    pipelineUpdateTimer(now);
    if (now > game.buildStart + game.buildDuration) {
        const float seconds = static_cast<float>(now - game.lastTick) * 0.001F;
        const int result = pipelineFlowStep(seconds);
        if (result == -1) {
            pipelineGameOver();
        } else if (result != 0) {
            if (game.reachedEnd) {
                pipelineLevelComplete();
                game.lastTick = now;
                return;
            }
            pipelineGameOver();
        }
    }
    game.lastTick = now;
}

void PipelineScene::pipelineLevelComplete() {
    // 0x0044a5f0, up to the blocking LEVELCOMPLETE modal at 0x0044a6d4.
    auto &game = *pipelineGame;
    const int terrain = game.level % 5;
    const int elapsed = pipelineClock() - game.buildDuration - game.buildStart;
    const int speed = static_cast<int>(
        static_cast<double>(kBaseTimes[static_cast<std::size_t>(terrain)]) /
        elapsed * (game.level * 100));
    game.score += static_cast<int>(
        static_cast<double>(speed + game.pieceCount * 25) *
        kMultipliers[static_cast<std::size_t>(terrain)]);
    game.active = false;
    game.scoreText = std::to_string(game.score);
    pipelineReturnHeld();
    if (game.running.playing) pipelineStopClip(game.running);
    showMessageWithTitle(
        language.get("pipeline", "LEVELCOMPLETE", "Gratulerer!"),
        language.get("pipeline", "LEVELMESSAGE", "Rørledningen er ferdig!"),
        MessageKind::PipelineLevelComplete);
}

void PipelineScene::continuePipelineAfterLevelMessage() {
    // 0x0044a6d9-0x0044a6f3, after the modal returns.
    if (!pipelineGame) return;
    auto &game = *pipelineGame;
    ++game.level;
    pipelineSetupLevel();
    game.active = true;
    game.buildStart = pipelineClock();
}

void PipelineScene::pipelineGameOver() {
    // 0x0044a740, up to the blocking GAMEOVER modal at 0x0044a7ec.
    auto &game = *pipelineGame;
    game.active = false;
    for (PipelineClip *clip : {&game.well, &game.lamp, &game.cursor, &game.running}) {
        if (clip->playing) pipelineStopClip(*clip);
    }
    pipelineReturnHeld();
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
        language.get("pipeline", "ENDMESSAGE", "Oops! Du tapte."),
        MessageKind::PipelineGameOver);
}

void PipelineScene::finishPipelineGameOver() {
    // 0x0044a808-0x0044a91a, once the score wrapper 0x0041f9a0 returns.
    if (!pipelineGame) return;
    auto &game = *pipelineGame;
    for (auto &cell : game.cells) {
        if (cell.added) {
            pipelineTraceRemove(cell.name);
            cell.added = false;
        }
    }
    if (game.levelAdded) {
        pipelineTraceRemove(game.levelName);
        game.levelAdded = false;
    }
    for (PipelineClip *clip : {&game.well, &game.lamp, &game.cursor}) {
        if (clip->added) pipelineRemoveClip(*clip);
    }
    for (auto &text : game.inventoryText) text.clear();
    game.scoreText.clear();
    game.timerText.clear();
    pipelineSetTurbo(0.0F);
    game.levelName = "level00";
    pipelineTraceAdd(game.levelName, -1, -1, 0);
    game.levelAdded = true;
}

void PipelineScene::updatePipeline(double elapsed) {
    if (!pipelineGame) return;
    auto &game = *pipelineGame;
    // FGP.exe's modals block the main loop: no ticks and no player
    // updates until they return.
    if (!game.entryAnnounced) {
        game.entryAnnounced = true;
        announcePipelineEntry();
    }
    if (dialogMode != DialogMode::None) return;
    pipelineTick(static_cast<int>(std::llround(elapsed * 1000.0)));
    // The descriptor update loop 0x00413200 finishes one-shot clones and
    // removes them; the pump sound loops.
    for (auto clone = game.clones.begin(); clone != game.clones.end();) {
        const auto found = game.media.find(clone->name);
        const double length = found == game.media.end()
            ? 0.0 : originalSmackerCompletionDuration(found->second);
        if (elapsed - clone->started >= length) {
            pipelineStopClip(*clone);
            pipelineRemoveClip(*clone);
            clone = game.clones.erase(clone);
        } else {
            ++clone;
        }
    }
    if (game.running.playing) {
        const auto found = game.media.find(game.running.name);
        if (found != game.media.end() && found->second.videoDuration > 0.0 &&
            elapsed - game.running.started >= found->second.videoDuration) {
            game.running.started += found->second.videoDuration;
            if (game.running.audio != LiveAudioMixer::kInvalidEffectHandle) {
                audioMixer.removeEffect(game.running.audio);
            }
            game.running.audio = audioMixer.addEffect(
                found->second, audioVolumes[0] * sceneAudioGains[0]);
        }
    }
}

void PipelineScene::pipelineComposeOil(PipelineCell &cell) {
    // 0x0044ac50 fills each arm's oil in colour 0 on the cell surface and
    // blits the pipe over it keyed on the interior colour, so oil shows
    // only inside the pipe. Native draws the straight fills from the
    // a/b arm amounts; the quarter-circle polygons a bend uses below a
    // third of an arm (0x0044ad13-0x0044ae29) are approximated by angle.
    auto &game = *pipelineGame;
    cell.oilDirty = false;
    if (cell.oil <= 0 || cell.type == 0) {
        cell.oilTexture.reset();
        return;
    }
    const auto &piece = kPieces[static_cast<std::size_t>(cell.type)];
    SDL_Surface *source = game.pieces[static_cast<std::size_t>(cell.type)]
                                     [static_cast<std::size_t>(cell.rotation)]
                                         .surface.get();
    if (!cell.oilTexture) {
        cell.oilTexture.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            kCellSize, kCellSize));
        SDL_SetTextureBlendMode(cell.oilTexture.get(), SDL_BLENDMODE_BLEND);
    }
    const float perArm = static_cast<float>(piece.capacity) /
                         static_cast<float>(piece.connections);
    const int halfWidth = piece.width / 2;
    std::array<std::uint8_t, kCellSize * kCellSize> oil{};
    const auto fill = [&](int x1, int y1, int x2, int y2) {
        for (int y = std::max(0, std::min(y1, y2)); y < std::min(kCellSize, std::max(y1, y2)); ++y) {
            for (int x = std::max(0, std::min(x1, x2)); x < std::min(kCellSize, std::max(x1, x2)); ++x) {
                oil[static_cast<std::size_t>(y * kCellSize + x)] = 1;
            }
        }
    };
    for (int direction = 0; direction < 4; ++direction) {
        const auto d = static_cast<std::size_t>(direction);
        const float inflow = 1.0F - static_cast<float>(cell.arms[d][0]) / perArm;
        const float outflow = static_cast<float>(cell.arms[d][1]) / perArm;
        const int dx = kSteps[d][0];
        const int dy = kSteps[d][1];
        const int half = kCellSize / 2;
        if (cell.type == 2) {
            if (!opensTowards(cell.type, cell.rotation, direction)) continue;
            // The bend's centre of curvature is the corner between its two
            // openings; this arm covers the half of the quarter circle on
            // its side, inflow from the edge, outflow from the middle.
            const int other = opensTowards(cell.type, cell.rotation, (direction + 1) & 3)
                ? (direction + 1) & 3 : (direction + 3) & 3;
            const int cornerX = half + (kSteps[d][0] + kSteps[static_cast<std::size_t>(other)][0]) * half;
            const int cornerY = half + (kSteps[d][1] + kSteps[static_cast<std::size_t>(other)][1]) * half;
            const float inFraction = 1.0F - inflow;
            for (int y = 0; y < kCellSize; ++y) {
                for (int x = 0; x < kCellSize; ++x) {
                    const float px = static_cast<float>(x) + 0.5F - static_cast<float>(cornerX);
                    const float py = static_cast<float>(y) + 0.5F - static_cast<float>(cornerY);
                    // Angle from this arm's edge (0) to the other's (1).
                    const float alongArm = std::abs(dx != 0 ? px : py);
                    const float alongOther = std::abs(dx != 0 ? py : px);
                    const float t = std::atan2(alongArm, alongOther) / 1.5707964F;
                    const float fromEdge = 1.0F - t;
                    if ((inFraction > 0.0F && fromEdge <= 0.5F * inFraction) ||
                        (outflow > 0.0F && fromEdge >= 0.5F * (1.0F - outflow) &&
                         fromEdge <= 0.5F)) {
                        oil[static_cast<std::size_t>(y * kCellSize + x)] = 1;
                    }
                }
            }
            continue;
        }
        if (inflow < 1.0F) {
            const int xa = static_cast<int>(static_cast<float>(dx * half) * inflow) -
                           dy * halfWidth + half;
            const int xb = dy * halfWidth + dx * half + half;
            const int ya = static_cast<int>(static_cast<float>(dy * half) * inflow) -
                           dx * halfWidth + half;
            const int yb = dx * halfWidth + dy * half + half;
            fill(xa, ya, xb, yb);
        }
        if (outflow > 0.0F) {
            const int xa = static_cast<int>(static_cast<float>(dx * half) * outflow) +
                           dy * halfWidth + half;
            const int xb = half - dy * halfWidth;
            const int ya = static_cast<int>(static_cast<float>(dy * half) * outflow) +
                           dx * halfWidth + half;
            const int yb = half - dx * halfWidth;
            fill(xa, ya, xb, yb);
        }
    }
    std::array<Uint32, kCellSize * kCellSize> pixels{};
    SDL_LockSurface(source);
    for (int y = 0; y < kCellSize && y < source->h; ++y) {
        const auto *row = reinterpret_cast<const Uint32 *>(
            static_cast<const Uint8 *>(source->pixels) + y * source->pitch);
        for (int x = 0; x < kCellSize && x < source->w; ++x) {
            Uint32 value = row[x];
            if (oil[static_cast<std::size_t>(y * kCellSize + x)] &&
                (value | 0xff000000U) == game.interiorColour) {
                value = 0xff000000U;
            }
            pixels[static_cast<std::size_t>(y * kCellSize + x)] = value;
        }
    }
    SDL_UnlockSurface(source);
    void *target = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(cell.oilTexture.get(), nullptr, &target, &pitch) == 0) {
        for (int y = 0; y < kCellSize; ++y) {
            std::memcpy(static_cast<Uint8 *>(target) + y * pitch,
                        pixels.data() + y * kCellSize,
                        kCellSize * sizeof(Uint32));
        }
        SDL_UnlockTexture(cell.oilTexture.get());
    }
}

void PipelineScene::renderPipeline(double elapsed) {
    if (!pipelineGame) return;
    auto &game = *pipelineGame;
    const auto mediaFrame = [&](const PipelineClip &clip, std::string_view media,
                                int x, int y) {
        if (!clip.added) return;
        const auto found = game.media.find(std::string(media));
        const auto texture = game.mediaTextures.find(std::string(media));
        if (found == game.media.end() || texture == game.mediaTextures.end() ||
            found->second.videoFrames.empty()) {
            return;
        }
        const double time = clip.playing ? elapsed - clip.started : 0.0;
        const auto &frame = found->second.videoFrames[
            frameIndexAt(found->second, std::max(0.0, time), true)];
        uploadVideoFrame(texture->second.get(), frame);
        const SDL_Rect destination{x, y, frame.width, frame.height};
        SDL_RenderCopy(renderer, texture->second.get(), nullptr, &destination);
    };

    // Layer 0: the level overlay.
    if (game.levelAdded) {
        const int terrain = game.levelName.size() == 7
            ? std::clamp(game.levelName[6] - '0', 0, 4) : 0;
        SDL_RenderCopy(renderer,
            game.levelTextures[static_cast<std::size_t>(terrain)].get(),
            nullptr, nullptr);
    }
    // Layers 1-2: the turbo progress fill and the text objects.
    const int turboHeight = static_cast<int>(std::lround(
        static_cast<double>(kTurboMeter.h) * game.turbo / kMaximumTurbo));
    if (turboHeight > 0) {
        SDL_Rect source{0, kTurboMeter.h - turboHeight, kTurboMeter.w, turboHeight};
        SDL_Rect destination{kTurboMeter.x,
                             kTurboMeter.y + kTurboMeter.h - turboHeight,
                             kTurboMeter.w, turboHeight};
        SDL_RenderCopy(renderer, game.turboMeterTexture.get(), &source,
                       &destination);
    }
    for (std::size_t slot = 0; slot < game.inventoryText.size(); ++slot) {
        drawCenteredText(renderer, calendarFont, game.inventoryText[slot],
            SDL_Rect{757, 158 + static_cast<int>(slot) * 62, 26, 56});
    }
    drawCenteredText(renderer, calendarFont, game.timerText, kTimerText);
    drawCenteredText(renderer, calendarFont, game.scoreText, kScoreText);
    // Layer 3: the cells.
    for (int index = 0; index < 80; ++index) {
        auto &cell = game.cells[static_cast<std::size_t>(index)];
        if (!cell.added) continue;
        if (cell.oilDirty) pipelineComposeOil(cell);
        const SDL_Rect destination{kGridX + (index % kColumns) * kCellSize,
                                   kGridY + (index / kColumns) * kCellSize,
                                   kCellSize, kCellSize};
        SDL_Texture *texture = cell.oilTexture
            ? cell.oilTexture.get()
            : game.pieces[static_cast<std::size_t>(cell.type)]
                         [static_cast<std::size_t>(cell.rotation)].texture.get();
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
    }
    // Layer 4: well and refinery; layer 5: the hint cursor.
    mediaFrame(game.well, game.well.name, game.wellX, game.wellY);
    mediaFrame(game.lamp, "lamp", game.lampX, game.lampY);
    mediaFrame(game.cursor, "cursor", game.cursorX, game.cursorY);
    // Layer 7: the controls, pressed while held down; layer 8: labels.
    SDL_RenderCopy(renderer,
        game.startButtonTextures[game.pressedControl == kStartControl ? 1U : 0U].get(),
        nullptr, &kStartButton);
    SDL_RenderCopy(renderer,
        game.turboButtonTextures[game.pressedControl == kTurboControl ? 1U : 0U].get(),
        nullptr, &kTurboButton);
    drawCenteredText(renderer, calendarFont,
                     language.get("pipeline", "START", "Start"), kStartText);
    drawCenteredText(renderer, calendarFont,
                     language.get("pipeline", "TURBO", "Turbo"), kTurboText);
    // Layer 11: the carried piece.
    if (game.heldAdded && game.held >= 0) {
        const SDL_Rect held{game.heldX, game.heldY, kCellSize, kCellSize};
        SDL_RenderCopy(renderer,
            game.pieces[static_cast<std::size_t>(
                kStockTypes[static_cast<std::size_t>(game.held)])]
                       [static_cast<std::size_t>(game.heldRotation)].texture.get(),
            nullptr, &held);
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
PipelineScene::pipelineHotspotAt(int x, int y) const {
    if (!pipelineGame || activeScene != "pipeline") return std::nullopt;
    const SDL_Point point{x, y};
    if (SDL_PointInRect(&point, &kStartButton)) {
        return std::pair{kStartControl, kStartButton};
    }
    if (SDL_PointInRect(&point, &kTurboButton)) {
        return std::pair{kTurboControl, kTurboButton};
    }
    for (std::size_t slot = 0; slot < kStockHotspots.size(); ++slot) {
        if (SDL_PointInRect(&point, &kStockHotspots[slot])) {
            return std::pair{static_cast<std::uint32_t>(slot + 2),
                             kStockHotspots[slot]};
        }
    }
    if (SDL_PointInRect(&point, &kGridHotspotBounds)) {
        return std::pair{kGridHotspot, kGridHotspotBounds};
    }
    return std::nullopt;
}

bool PipelineScene::executePipelineDeveloperCommand(std::string_view command) {
    constexpr std::string_view pipelineControlPrefix = "pipeline:";
    if (activeScene != "pipeline" || !pipelineGame) {
        return false;
    }
    auto &game = *pipelineGame;
    const std::string_view control =
        command.substr(pipelineControlPrefix.size());
    if (control == "start") {
        pipelineControl(kStartControl);
    } else if (control.rfind("after:", 0) == 0) {
        // Holds the developer command queue until this scene time (seconds).
        return sceneElapsedNow() >= std::stod(std::string(control.substr(6)));
    } else if (control.rfind("click:", 0) == 0 ||
               control.rfind("right:", 0) == 0) {
        // Scripted pointer presses through the same handlers as SDL input.
        const std::string point(control.substr(6));
        const std::size_t comma = point.find(',');
        if (comma == std::string::npos) return false;
        const int x = std::stoi(point.substr(0, comma));
        const int y = std::stoi(point.substr(comma + 1));
        mouseX = x;
        mouseY = y;
        movePipelinePointer(x, y);
        if (control[0] == 'r') {
            rotatePipelineAt(x, y);
        } else {
            beginPipelineInteraction(x, y);
            endPipelineInteraction(x, y);
        }
    } else if (control.rfind("seed:", 0) == 0) {
        // Matches the capture fixture in the Pipeline x32dbg adapter.
        game.fixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 1) return false;
        if (!game.active) pipelineStartGame();
        game.level = level - 1;
        pipelineSetupLevel();
        game.buildStart = pipelineClock();
    } else if (control == "solve") {
        // Lay the generated route, including its valved branches.
        if (!game.active) pipelineStartGame();
        for (std::size_t index = 0; index < game.cells.size(); ++index) {
            auto &cell = game.cells[index];
            const auto [type, rotation] = game.solution[index];
            if (cell.type != 0 || stockSlot(type) < 0) continue;
            cell.type = type;
            cell.rotation = rotation;
            pipelineRedrawCell(cell);
        }
    } else if (control == "turbo") {
        pipelineControl(kTurboControl);
    } else if (control == "timeout") {
        if (!game.active) pipelineStartGame();
        game.buildDuration = pipelineClock() - game.buildStart;
    } else if (control == "finish") {
        if (!game.active || pipelineClock() <= game.buildStart + game.buildDuration) {
            return false;
        }
        game.turbo = kMaximumTurbo;
    } else if (control == "status") {
        std::cout << "Pipeline status: level " << game.level + 1 << ", score "
                  << game.score << ", start " << game.startColumn << ','
                  << game.startRow << ", end " << game.endColumn << ','
                  << game.endRow << ", stock";
        for (const int count : game.inventory) std::cout << ' ' << count;
        std::cout << ", held " << game.held << ", timer " << game.timerText
                  << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Pipeline control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
