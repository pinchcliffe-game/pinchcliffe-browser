#include "game/app_puzzle.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "game/puzzle_data.h"
#include "media/smacker.h"

namespace fgp {
namespace {


constexpr int kMaskWidth = 200;   // 0x00455177: [0x00695C18] = 200
constexpr int kMaskHeight = 150;
// Gem types (+0xF4).
constexpr int kJoker = 8;
constexpr int kGold = 9;
constexpr int kSilver = 10;
// 0x00455720 indexes this table at 0x005020A8 by the current level. Only
// levels 1-7 can be left: index 8 reads the first dword of the "pickup"
// descriptor that follows the table (0x005020C8, bytes "pick"), so the
// level-8 bar never fills and TOPLEVEL is unreachable.
const auto kLevelThresholds = originalArray<int, 9>(0x005020a8U);
// 0x0049F62C: bottle.smk frame scale (101 frames).
constexpr float kBottleFrames = 101.0F;
// 0x00455C30 adds bottle at (0x2B0, 0x5E), layer 100.
constexpr int kBottleX = 0x2B0;
constexpr int kBottleY = 0x5E;
constexpr int kGemLayer = 100;     // 0x004533E5
constexpr int kLidLayer = 120;     // 0x00455260
// 0x004553FE-0x00455426: the closed lid starts on a click inside
// (0x4B, 0x9B)-(0x2D7, 0x226), exclusive.
constexpr SDL_Rect kLidStart{0x4C, 0x9C, 0x2D7 - 0x4C, 0x226 - 0x9C};
// 0x0049F608 / 0x0049F618: score and level text rectangles.
constexpr SDL_Rect kScoreRect{266, 43, 380 - 266, 78 - 43};
constexpr SDL_Rect kLevelRect{418, 43, 533 - 418, 78 - 43};
// 0x00455890: timer units per millisecond (0x0049F244) and the Game Over
// delay (0x0049F640).
constexpr float kUnitsPerMs = 0.1F;
constexpr float kGameOverDelay = 120.0F;
// Gem animation lengths: 0x00453930 (seed pop-in, 0x0049F5F0), 0x004539C0
// (growth, 0x0049CC68), 0x00453A50 / 0x004537B0 (death, [0x00693698] =
// 100 set by 0x00453590).
constexpr float kSmallPop = 75.0F;
constexpr float kGrowLength = 100.0F;
constexpr float kDieLength = 100.0F;
// 0x00455AF0: rotate fades over 100 units, three times faster out.
constexpr float kRotateFade = 100.0F;
// 0x00501FF0: per-type colour (r, g, b) that the 3D materials use.
const auto kGemColours = originalArray<std::array<std::uint8_t, 3>, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x00501ff0U + static_cast<std::uint32_t>(index * 8);
    std::array<std::uint8_t, 3> colour{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto value = source.read<std::uint16_t>(address + static_cast<std::uint32_t>(2*channel));
        if (value > 255) throw std::runtime_error("Invalid original gem colour");
        colour[channel] = static_cast<std::uint8_t>(value);
    }
    return colour;
});

std::uint16_t gemColour(int type) {
    // 0x00453590 packs each 0x00501FF0 row with 0x00455CC0.
    const auto &rgb = kGemColours[static_cast<std::size_t>(type)];
    return static_cast<std::uint16_t>(((rgb[0] & 0xF8) << 8) |
                                      ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
}

PuzzleMeshId gemMesh(int type) {
    // The mesh switch in 0x00453000 / 0x00454390; types 6-8 keep the
    // default 0x004E6FA8.
    switch (type) {
    case 1:
    case 2: return PuzzleMeshId::Square;
    case 3: return PuzzleMeshId::Type3;
    case 4: return PuzzleMeshId::Type4;
    case 5: return PuzzleMeshId::Type5;
    case 9: return PuzzleMeshId::Gold;
    case 10: return PuzzleMeshId::Silver;
    default: return PuzzleMeshId::Round;
    }
}

PuzzleMaterial gemMaterial(int type) {
    // 0x00452F60.
    PuzzleMaterial material;
    material.flags = 0x20;
    material.colour = gemColour(type);
    material.diffuse = 0.5F;
    material.specular = 1.0F;
    material.exponent = 50.0F;
    if (type == 9 || type == 10) {
        material.flags = 4;
        material.secondFlags = 8;
        material.texture = type == 9 ? 0 : 1;
    }
    return material;
}

// 0x00502048: the joker's colour-cycle stops; 0x00502064 (700) is the
// float that happens to follow them.
const auto kJokerStops = originalArray<float, 8>(0x00502048U);

TexturePtr streamingTexture(SDL_Renderer *renderer, const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame = media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!texture) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

bool specialType(int type) {
    // 0x004541D0.
    return type == kJoker || type == kGold || type == kSilver;
}

} // namespace

PuzzleScene::PuzzleGame PuzzleScene::loadPuzzleGame(Package &package) {
    PuzzleGame game;
    const auto clip = [&](std::string name, bool video) {
        return decodeActiveSceneClip(std::move(name), video, true);
    };
    game.lid = clip("lid", true);
    game.lid2 = clip("lid2", true);
    game.bottle = clip("bottle", true);
    // The sound clips are 4x4 Smacker players; their frames time them.
    game.rotate = clip("rotate", true);
    game.pickup = clip("pickup", true);
    game.drop1 = clip("drop1", true);
    game.drop2 = clip("drop2", true);
    game.group = clip("group", true);
    game.gemRenderer.load(
        package.data(package.find("bitmap\\illum.bmp")),
        package.data(package.find("bitmap\\gold.bmp")),
        package.data(package.find("bitmap\\silver.bmp")));
    game.lidTexture = streamingTexture(renderer, game.lid);
    game.bottleTexture = streamingTexture(renderer, game.bottle);

    // 0x00455168 loads mask.bmp; 0x00455192-0x004551E1 flood-fills it from
    // each cell's projected centre with the cell's 1-based id (0x004552A0:
    // every non-zero 4-connected pixel).
    SurfacePtr mask = loadBmp(package.data(package.find("bitmap\\mask.bmp")));
    if (mask->w != kMaskWidth || mask->h != kMaskHeight) {
        throw std::runtime_error("unexpected Puzzle mask.bmp size");
    }
    game.mask.assign(static_cast<std::size_t>(kMaskWidth * kMaskHeight), 0);
    for (int y = 0; y < kMaskHeight; ++y) {
        for (int x = 0; x < kMaskWidth; ++x) {
            Uint8 r = 0;
            Uint8 g = 0;
            Uint8 b = 0;
            SDL_GetRGB(pixelValue(mask.get(), x, y), mask->format, &r, &g, &b);
            game.mask[static_cast<std::size_t>(y * kMaskWidth + x)] =
                (r >> 3) || (g >> 2) || (b >> 3) ? -1 : 0;
        }
    }
    for (int cell = 0; cell < kPuzzleCells; ++cell) {
        const auto seed = puzzleOriginalData().seeds[static_cast<std::size_t>(cell)];
        std::vector<SDL_Point> stack{{seed.x, seed.y}};
        while (!stack.empty()) {
            const SDL_Point point = stack.back();
            stack.pop_back();
            if (point.x < 0 || point.y < 0 || point.x >= kMaskWidth ||
                point.y >= kMaskHeight) {
                continue;
            }
            int &value = game.mask[static_cast<std::size_t>(
                point.y * kMaskWidth + point.x)];
            if (value == 0 || value == cell + 1) continue;
            value = cell + 1;
            stack.push_back({point.x + 1, point.y});
            stack.push_back({point.x - 1, point.y});
            stack.push_back({point.x, point.y + 1});
            stack.push_back({point.x, point.y - 1});
        }
    }
    std::cout << "Loaded executable-derived Puzzle: 9x8 jewel board, "
                 "path moves, rectangle groups, jokers, gold and silver\n";
    return game;
}

int PuzzleScene::puzzleRandom() {
    // Statically linked MSVC rand() at 0x0048D323.
    if (!puzzleRandomSeeded) {
        puzzleRandomState = static_cast<std::uint32_t>(randomEngine());
        puzzleRandomSeeded = true;
    }
    puzzleRandomState = puzzleRandomState * 0x343FDU + 0x269EC3U;
    return static_cast<int>((puzzleRandomState >> 16) & 0x7FFFU);
}

int PuzzleScene::puzzleRandomBelow(int bound) {
    // 0x0040C530 returns 0 without calling rand() for a zero bound.
    return bound == 0 ? 0 : puzzleRandom() % bound;
}

double PuzzleScene::puzzleNow() { return sceneElapsedNow(); }

void PuzzleScene::puzzleCloneSound(std::string_view asset) {
    // SCENE_PlayAnimClone 0x00412C20: a clone of the descriptor is added
    // with its packaged position (0,0), layer 0, and released when its
    // Smacker player finishes.
    auto &game = *puzzleGame;
    const DecodedMedia &media = asset == "pickup" ? game.pickup
                              : asset == "drop1"  ? game.drop1
                              : asset == "drop2"  ? game.drop2
                                                  : game.group;
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    audioMixer.addEffect(media, audioVolumes[0] * sceneAudioGains[0]);
    game.clones.push_back(
        {std::string(asset),
         puzzleNow() + originalSmackerCompletionDuration(media)});
}

void PuzzleScene::puzzleStartRotate() {
    // 0x00455620: restart the fade-in and play the looping rotate sound
    // through 0x00412B90, which adds the descriptor the first time only.
    auto &game = *puzzleGame;
    game.rotateFadeTime = 0.0F;
    game.rotateFadeOut = false;
    game.rotateFading = true;
    if (!game.rotateAdded) {
        game.rotateAdded = true;
        referenceTrace.emit("animation_add", {
            {"asset", "rotate"}, {"x", 0}, {"y", 0}, {"layer", 0},
            {"hotspot", 0}, {"conflict_mask", 0}});
    }
    audioMixer.removeEffect(game.rotateEffect);
    game.rotateEffect = audioMixer.addEffect(game.rotate, 0.0F, true);
    game.rotatePlaying = true;
    game.rotateStartedAt = puzzleNow();
}

void PuzzleScene::puzzleFadeRotate() {
    // 0x00455650.
    auto &game = *puzzleGame;
    game.rotateFadeTime = 0.0F;
    game.rotateFadeOut = true;
    game.rotateFading = true;
}

void PuzzleScene::puzzleStopRotate() {
    // SCENE_StopAnim 0x00412A60 on the rotate descriptor.
    auto &game = *puzzleGame;
    audioMixer.removeEffect(game.rotateEffect);
    game.rotateEffect = LiveAudioMixer::kInvalidEffectHandle;
    game.rotatePlaying = false;
    referenceTrace.emit("animation_stop", {
        {"asset", "rotate"}, {"reason", "stopped"}});
}

void PuzzleScene::puzzleInitialize() {
    // Scene initializer 0x00455090 (also rerun by 0x00455BC0 after lid2).
    auto &game = *puzzleGame;
    game.board.reset();
    game.selected = 0;
    game.busy = false;
    game.playing = false;
    game.gameOver = false;
    game.gameOverTime = 0.0F;
    game.levelProgress = 0.0F;
    game.finalScore = 0;
    game.rotateFading = false;
    game.textControls = true;
    game.scoreText.clear();
    game.levelText.clear();
    puzzleClearPath();
    game.lidClip = {};
    game.lidClip.added = true;
    referenceTrace.emit("animation_add", {
        {"asset", "lid"}, {"x", 0}, {"y", 0}, {"layer", kLidLayer},
        {"hotspot", 0}, {"conflict_mask", 0}});
}

PuzzleScene::PuzzleGem *PuzzleScene::puzzleGem(int id) {
    if (id == 0 || !puzzleGame || !puzzleGame->board) return nullptr;
    auto &gems = puzzleGame->board->gems;
    const auto found = std::find_if(gems.begin(), gems.end(),
        [id](const PuzzleGem &gem) { return gem.id == id; });
    return found == gems.end() ? nullptr : &*found;
}

PuzzleScene::PuzzleGem *PuzzleScene::puzzleCellGem(int cell) {
    return puzzleGem(
        puzzleGame->board->slots[static_cast<std::size_t>(cell)]);
}

std::optional<int> PuzzleScene::puzzleCellAt(int x, int y) const {
    // 0x00454D60 bounds, then the labelled mask pixel.
    if ((x >> 2) < 0 || (y >> 2) < 0 || (x >> 2) > kMaskWidth ||
        (y >> 2) >= 0x97) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>((x >> 2) + (y >> 2) * 200);
    if (index >= puzzleGame->mask.size()) return 0;
    return puzzleGame->mask[index];
}

void PuzzleScene::puzzleSpawnGem(int cell, int type, bool small) {
    // 0x00453000: allocate, place (0x004532B0 adds the 64x64 player at the
    // projected cell), roll three rotations, append to the gem list.
    auto &game = *puzzleGame;
    auto &board = *game.board;
    referenceTrace.emit("minigame_state", {
        {"game", "puzzle"}, {"state", "spawn"}, {"cell", cell},
        {"gem", type}, {"small", small ? 1 : 0}});
    PuzzleGem gem;
    gem.id = game.nextGemId++;
    gem.cell = cell;
    gem.type = type;
    const auto anchor = puzzleOriginalData().anchors[static_cast<std::size_t>(cell)];
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", anchor.x}, {"y", anchor.y},
        {"layer", kGemLayer}, {"hotspot", 0}, {"conflict_mask", 0}});
    board.slots[static_cast<std::size_t>(cell)] = gem.id;
    for (float &angle : gem.rotation) {
        angle = static_cast<float>(puzzleRandomBelow(360));
    }
    gem.small = small;
    // 0x00453000: the gem mesh at 0.8, the seed mesh at 0.4 with the colour
    // halved (& 0x7BEF), full diffuse and no highlight.
    gem.body.mesh = gemMesh(type);
    gem.body.scale = 0.8F;
    gem.body.material = gemMaterial(type);
    gem.seed.mesh = gem.body.mesh;
    gem.seed.scale = 0.4F;
    gem.seed.material.flags = 0x20;
    gem.seed.material.colour =
        static_cast<std::uint16_t>((gemColour(type) >> 1) & 0x7BEF);
    gem.seed.material.diffuse = 1.0F;
    if (small) {
        gem.body.material.flags = 2;
        gem.seed.visible = true;
    } else {
        gem.body.visible = true;
    }
    board.gems.push_back(std::move(gem));
    ++board.spawned;
}

int PuzzleScene::puzzleFreeCell() {
    // 0x00453D00: up to 0x1440 random probes for an empty slot.
    const auto &slots = puzzleGame->board->slots;
    for (int attempt = 0; attempt < 0x1440; ++attempt) {
        const int cell = puzzleRandomBelow(kPuzzleCells);
        if (slots[static_cast<std::size_t>(cell)] == 0) return cell;
    }
    return -1;
}

bool PuzzleScene::puzzleSpawnBatch(bool small, bool special) {
    // 0x00453D30.
    auto &board = *puzzleGame->board;
    if (!special) {
        if (board.level == 0) return true;
        for (int type = 0; type < board.level; ++type) {
            const int cell = puzzleFreeCell();
            if (cell < 0) return false;
            puzzleSpawnGem(cell, type, small);
        }
        return true;
    }
    const int cell = puzzleFreeCell();
    if (cell < 0) return false;
    const int roll = puzzleRandomBelow(3);
    puzzleSpawnGem(cell, roll == 0 ? kJoker : roll == 1 ? kGold : kSilver,
                   small);
    return true;
}

bool PuzzleScene::puzzleSpecialDue() {
    // 0x00454C30.
    auto &game = *puzzleGame;
    if (game.board->spawned < game.specialThreshold) return false;
    game.board->spawned = 0;
    game.specialThreshold = static_cast<unsigned>(35 + puzzleRandomBelow(15));
    return true;
}

void PuzzleScene::puzzleSpawnRound(bool small) {
    // 0x00454CD0: 4/3/2 batches at levels 1/2/3, one batch from level 4.
    const int level = puzzleGame->board->level;
    const int batches = level == 1 ? 4 : level == 2 ? 3 : level == 3 ? 2 : 1;
    for (int batch = 0; batch < batches; ++batch) {
        puzzleSpawnBatch(small, false);
    }
    if (puzzleSpecialDue()) puzzleSpawnBatch(small, true);
}

int PuzzleScene::puzzleCellType(int column, int row) const {
    // 0x004541A0: a gem's type, small or not; -1 for an empty slot.
    const auto &board = *puzzleGame->board;
    const int id = board.slots[static_cast<std::size_t>(column + row * 9)];
    if (id == 0) return -1;
    for (const auto &gem : board.gems) {
        if (gem.id == id) return gem.type;
    }
    return -1;
}

bool PuzzleScene::puzzleUniform(int column, int row, int width,
                                int height) const {
    // 0x004540A0. Jokers, gold and silver match anything. The reference
    // colour is the first other cell; the check before each cell means an
    // empty slot only fails the rectangle if a cell follows it.
    std::vector<int> cells;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cells.push_back(puzzleCellType(column + x, row + y));
        }
    }
    int reference = -2;
    for (const int type : cells) {
        if (reference == -1) return false;
        if (reference == -2) {
            if (!specialType(type)) reference = type;
        } else if (!specialType(type) && type != reference) {
            return false;
        }
    }
    return true;
}

bool PuzzleScene::puzzleExtendGroup(std::array<int, 4> &rect, int grow,
                                    int growDown) const {
    // 0x00454200: grow the rectangle right or down while it stays uniform
    // and keep whichever recursion covers more cells.
    rect[2] += grow;
    rect[3] += growDown;
    const int area = rect[2] * rect[3];
    std::array<int, 4> best = rect;
    if (rect[0] + rect[2] > 9) return false;
    if (rect[1] + rect[3] >= 9) return false;
    if (!puzzleUniform(rect[0], rect[1], rect[2], rect[3])) return false;
    int bestArea = area;
    std::array<int, 4> wider = rect;
    if (puzzleExtendGroup(wider, 1, 0) && wider[2] * wider[3] > area) {
        best = wider;
        bestArea = wider[2] * wider[3];
    }
    std::array<int, 4> taller = rect;
    if (!puzzleExtendGroup(taller, 0, 1) ||
        taller[2] * taller[3] <= bestArea) {
        taller = best;
    }
    rect = taller;
    return true;
}

void PuzzleScene::puzzleDeselect(PuzzleGem &gem) {
    // 0x00453E70.
    gem.body.rotation = {};
    gem.body.scale = 0.8F;
    gem.selected = false;
}

void PuzzleScene::puzzleKillGem(PuzzleGem &gem) {
    // 0x00453540.
    gem.dying = true;
    gem.dieTime = 0.0F;
}

int PuzzleScene::puzzleRemoveGroups() {
    // 0x00453F70: scan every 2x2 corner, grow it, and clear the gems in it.
    auto &board = *puzzleGame->board;
    int removed = 0;
    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 8; ++column) {
            if (!puzzleUniform(column, row, 2, 2)) continue;
            std::array<int, 4> rect{column, row, 2, 2};
            puzzleExtendGroup(rect, 0, 0);
            for (int y = 0; y < rect[3]; ++y) {
                for (int x = 0; x < rect[2]; ++x) {
                    const auto slot = static_cast<std::size_t>(
                        rect[0] + x + (rect[1] + y) * 9);
                    PuzzleGem *gem = puzzleGem(board.slots[slot]);
                    if (gem == nullptr) continue;
                    if (gem->type == kJoker) board.jokerGrouped = true;
                    if (gem->type == kGold) board.goldGrouped = true;
                    if (gem->type == kSilver) board.silverGrouped = true;
                    puzzleKillGem(*gem);
                    ++removed;
                    board.slots[slot] = 0;
                }
            }
        }
    }
    return removed;
}

void PuzzleScene::puzzleRecolour(int type) {
    // 0x00454390 (gold): every full, living gem takes one colour and
    // replays its growth animation.
    for (auto &gem : puzzleGame->board->gems) {
        if (gem.small || gem.dying) continue;
        gem.body = {};
        gem.body.mesh = gemMesh(type);
        gem.body.scale = 0.8F;
        gem.body.material = gemMaterial(type);
        gem.body.visible = true;
        gem.seed.visible = false;
        gem.small = false;
        gem.growing = true;
        gem.growTime = 0.0F;
        gem.type = type;
    }
}

void PuzzleScene::puzzleSilverBlast() {
    // 0x00454580 (silver): each full, living gem dies when its roll
    // matches one shared roll.
    auto &board = *puzzleGame->board;
    const int target = puzzleRandomBelow(3);
    for (auto &gem : board.gems) {
        if (gem.small || gem.dying) continue;
        if (puzzleRandomBelow(3) != target) continue;
        puzzleKillGem(gem);
        board.slots[static_cast<std::size_t>(gem.cell)] = 0;
    }
}

void PuzzleScene::puzzleMatureSmall() {
    // 0x004545F0: every seed becomes a full gem.
    for (auto &gem : puzzleGame->board->gems) {
        if (!gem.small) continue;
        gem.small = false;
        gem.growing = true;
        gem.growTime = 0.0F;
        gem.body.visible = true;
        gem.seed.visible = false;
    }
}

void PuzzleScene::puzzleRemoveGem(int id) {
    // 0x00453480: unlink and SCENE_RemoveAnim the gem's player.
    auto &gems = puzzleGame->board->gems;
    const auto found = std::find_if(gems.begin(), gems.end(),
        [id](const PuzzleGem &gem) { return gem.id == id; });
    if (found == gems.end()) return;
    gems.erase(found);
    referenceTrace.emit("animation_remove", {
        {"asset", ""}, {"reason", "removed"}});
}

int PuzzleScene::puzzleMoveGem(PuzzleGem &gem, int targetId, bool overSmall) {
    // 0x00453EC0.
    auto &board = *puzzleGame->board;
    const int target = targetId - 1;
    referenceTrace.emit("minigame_state", {
        {"game", "puzzle"}, {"state", "move"}, {"cell", gem.cell},
        {"target", target}, {"small", overSmall ? 1 : 0}});
    const int moving = gem.id;
    if (overSmall) {
        puzzleRemoveGem(board.slots[static_cast<std::size_t>(target)]);
        board.slots[static_cast<std::size_t>(target)] = 0;
    }
    PuzzleGem *placed = puzzleGem(moving);
    board.slots[static_cast<std::size_t>(placed->cell)] = 0;
    placed->cell = target;
    board.slots[static_cast<std::size_t>(target)] = moving;
    board.jokerGrouped = false;
    board.goldGrouped = false;
    board.silverGrouped = false;
    const int removed = puzzleRemoveGroups();
    if (removed != 0) {
        if (board.goldGrouped) puzzleRecolour(puzzleRandomBelow(board.level));
        if (board.silverGrouped) puzzleSilverBlast();
    }
    return removed;
}

bool PuzzleScene::puzzleNoMovableGem() const {
    // 0x00454770 walks ids 1..71 through 0x00453E20, so cell 71 is never
    // examined. A joker is movable; gold and silver are not.
    const auto &board = *puzzleGame->board;
    for (int id = 1; id < kPuzzleCells; ++id) {
        const int slot = board.slots[static_cast<std::size_t>(id - 1)];
        if (slot == 0) continue;
        for (const auto &gem : board.gems) {
            if (gem.id != slot) continue;
            if (!gem.small && !gem.dying && gem.type != kGold &&
                gem.type != kSilver) {
                return false;
            }
        }
    }
    return true;
}

bool PuzzleScene::puzzleSlotOpen(int index) const {
    // 0x00454720. Index 72 reads the first board coordinate pair at +0x120,
    // which is (0,0), so the slot past the last cell is always open.
    if (index < 0 || index > kPuzzleCells) return false;
    if (index == kPuzzleCells) return true;
    const auto &board = *puzzleGame->board;
    const int slot = board.slots[static_cast<std::size_t>(index)];
    if (slot == 0) return true;
    for (const auto &gem : board.gems) {
        if (gem.id == slot) return gem.small || gem.dying;
    }
    return true;
}

bool PuzzleScene::puzzleNearlyFull() const {
    // 0x00454640 counts slots that are empty or hold a full gem with an open
    // neighbour, and reports fewer than five. 0x00454680 tests the cells
    // +-1 and +-72 (not +-9), so rows wrap and only cell 0 sees "below".
    const auto &board = *puzzleGame->board;
    int free = 0;
    for (int cell = 0; cell < kPuzzleCells; ++cell) {
        const int slot = board.slots[static_cast<std::size_t>(cell)];
        if (slot == 0) {
            ++free;
            continue;
        }
        const PuzzleGem *gem = nullptr;
        for (const auto &candidate : board.gems) {
            if (candidate.id == slot) gem = &candidate;
        }
        if (gem == nullptr || gem->small || gem->dying) continue;
        if (puzzleSlotOpen(gem->cell - 72) || puzzleSlotOpen(gem->cell - 1) ||
            puzzleSlotOpen(gem->cell + 1) || puzzleSlotOpen(gem->cell + 72)) {
            ++free;
        }
    }
    return free < 5;
}

void PuzzleScene::puzzleGameOver() {
    // 0x00454BC0.
    auto &game = *puzzleGame;
    auto &board = *game.board;
    referenceTrace.emit("minigame_state", {
        {"game", "puzzle"}, {"state", "game_over"}, {"score", board.score},
        {"level", board.level}});
    if (game.rotatePlaying) puzzleStopRotate();
    game.finalScore = board.score * 7;
    for (auto &gem : board.gems) {
        if (!gem.dying) puzzleKillGem(gem);
    }
    game.playing = false;
    game.gameOver = true;
    game.gameOverTime = 0.0F;
}

void PuzzleScene::puzzleAfterMove() {
    // 0x00454C70.
    auto &game = *puzzleGame;
    if (game.gameOver) return;
    if (puzzleNoMovableGem()) {
        puzzleSpawnRound(false);
        return;
    }
    if (!puzzleNearlyFull()) return;
    // 0x00407840 releases both text controls before 0x00454BC0.
    game.textControls = false;
    puzzleGameOver();
}

std::optional<std::vector<int>> PuzzleScene::puzzleFindPath(int from,
                                                            int to) const {
    // 0x00454880 on the 9x8 grid 0x00454820 allocates: a breadth-first
    // flood whose neighbour order is left, right, up (row - 1), down; the
    // target must itself be passable (0x00454A90: empty, a seed, or dying).
    const auto &board = *puzzleGame->board;
    const auto passable = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= kPuzzleColumns || y >= kPuzzleRows) {
            return false;
        }
        const int slot = board.slots[static_cast<std::size_t>(x + y * 9)];
        if (slot == 0) return true;
        for (const auto &gem : board.gems) {
            if (gem.id == slot) return gem.small || gem.dying;
        }
        return true;
    };
    const int fromX = from % 9;
    const int fromY = from / 9;
    const int toX = to % 9;
    const int toY = to / 9;
    if (!passable(toX, toY)) return std::nullopt;
    if (fromX == toX && fromY == toY) return std::vector<int>{from};
    std::array<int, kPuzzleCells> parent{};
    parent.fill(-1);
    parent[static_cast<std::size_t>(from)] = -2;
    std::vector<int> frontier{from};
    while (!frontier.empty()) {
        std::vector<int> next;
        for (const int node : frontier) {
            const int x = node % 9;
            const int y = node / 9;
            const std::array<std::pair<int, int>, 4> steps{{
                {x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}};
            for (const auto &[nx, ny] : steps) {
                if (nx == toX && ny == toY) {
                    std::vector<int> path{to, node};
                    for (int at = node; at != from;) {
                        at = parent[static_cast<std::size_t>(at)];
                        path.push_back(at);
                    }
                    return path;
                }
                if (!passable(nx, ny)) continue;
                const auto index = static_cast<std::size_t>(nx + ny * 9);
                if (parent[index] != -1) continue;
                parent[index] = node;
                next.push_back(nx + ny * 9);
            }
        }
        frontier = std::move(next);
    }
    return std::nullopt;
}

bool PuzzleScene::puzzleTryMove(int targetId, bool overSmall) {
    // 0x00455670.
    auto &game = *puzzleGame;
    PuzzleGem *gem = puzzleGem(game.selected);
    if (gem == nullptr || !puzzleFindPath(gem->cell, targetId - 1)) {
        return false;
    }
    const int removed = puzzleMoveGem(*gem, targetId, overSmall);
    if (PuzzleGem *moved = puzzleGem(game.selected)) puzzleDeselect(*moved);
    game.selected = 0;
    if (removed == 0) {
        puzzleMatureSmall();
        puzzleSpawnRound(true);
        puzzleAfterMove();
        return true;
    }
    puzzleUpdateScore(removed);
    puzzleCloneSound("group");
    puzzleAfterMove();
    return true;
}

void PuzzleScene::puzzleUpdateScore(int removed) {
    // 0x00455720.
    auto &game = *puzzleGame;
    auto &board = *game.board;
    referenceTrace.emit("minigame_state", {
        {"game", "puzzle"}, {"state", "score"}, {"count", removed},
        {"score", board.score}, {"level", board.level}});
    board.score += removed;
    game.levelProgress += static_cast<float>(removed);
    bool top = false;
    if (static_cast<float>(kLevelThresholds[static_cast<std::size_t>(
            board.level)]) < game.levelProgress) {
        top = board.level > 7;
        if (!top) ++board.level;
        game.levelProgress = 0.0F;
    }
    game.scoreText = language.get("puzzle", "SCORE", "Poeng") + " " +
                     std::to_string(board.score * 7);
    game.bottleFrame = static_cast<int>(
        game.levelProgress /
        static_cast<float>(kLevelThresholds[static_cast<std::size_t>(
            board.level)]) *
        kBottleFrames);
    game.levelText = top ? language.get("puzzle", "TOPLEVEL", "Siste niv\xe5!")
                         : language.get("puzzle", "LEVEL", "Niv\xe5") + " " +
                               std::to_string(board.level);
}

void PuzzleScene::puzzleStartBoard() {
    // 0x00455C30, after 0x00455BC0 removed the opened lid.
    auto &game = *puzzleGame;
    referenceTrace.emit("minigame_state", {
        {"game", "puzzle"}, {"state", "board_start"}});
    if (game.board) puzzleDestroyBoard();
    game.bottleClip.added = true;
    referenceTrace.emit("animation_add", {
        {"asset", "bottle"}, {"x", kBottleX}, {"y", kBottleY},
        {"layer", kGemLayer}, {"hotspot", 0}, {"conflict_mask", 0}});
    game.board.emplace();
    game.levelProgress = 0.0F;
    puzzleUpdateScore(0);
    if (puzzleFixedSeed) {
        // The capture fixture's reseed lands on the next rand() call.
        puzzleRandomState = *puzzleFixedSeed;
        puzzleRandomSeeded = true;
    }
    game.specialThreshold = static_cast<unsigned>(35 + puzzleRandomBelow(15));
    game.levelProgress = 0.0F;
    game.finalScore = 0;
    puzzleSpawnRound(false);
    puzzleSpawnRound(false);
    game.selected = 0;
    game.lastUpdate = puzzleNow();
}

void PuzzleScene::puzzleDestroyBoard() {
    // 0x00453770.
    auto &game = *puzzleGame;
    while (!game.board->gems.empty()) {
        puzzleRemoveGem(game.board->gems.front().id);
    }
    game.board.reset();
    game.selected = 0;
}

bool PuzzleScene::beginPuzzleInteraction(int x, int y) {
    // Mouse-down handler 0x004553E0 (event 0x105).
    if (!puzzleGame || puzzleGame->busy) return false;
    auto &game = *puzzleGame;
    if (!game.playing) {
        const SDL_Point point{x, y};
        if (!game.lidClip.added || game.lidClip.playing ||
            !SDL_PointInRect(&point, &kLidStart)) {
            return false;
        }
        // 0x0045542C plays the lid; 0x00455BC0 builds the board when it
        // finishes.
        game.lidClip.playing = true;
        game.lidClip.startedAt = puzzleNow();
        audioMixer.addEffect(game.lid, audioVolumes[0] * sceneAudioGains[0]);
        referenceTrace.emit("animation_start", {
            {"asset", "lid"}, {"x", 0}, {"y", 0}, {"layer", kLidLayer},
            {"loop", false}});
        game.busy = true;
        return true;
    }
    if (!game.board) return false;
    const auto cell = puzzleCellAt(x, y);
    if (!cell) return false;
    const int id = *cell;
    puzzleClearPath();
    PuzzleGem *selected = puzzleGem(game.selected);
    const bool outside = id < 1 || id > kPuzzleCells;
    if (outside) {
        if (selected == nullptr) return true;
        puzzleDeselect(*selected);
        puzzleCloneSound("drop1");
        game.selected = 0;
        puzzleFadeRotate();
        return true;
    }
    PuzzleGem *target = puzzleCellGem(id - 1);
    if (target == nullptr || !target->small) {
        if (selected == nullptr) {
            if (target == nullptr) return true;
            game.selected = target->id;
            puzzleCloneSound("pickup");
            target->selected = true;  // 0x00453E50
            puzzleStartRotate();
            return true;
        }
    } else if (selected == nullptr) {
        return true;
    }
    if (target != nullptr && target->id == selected->id) {
        puzzleCloneSound("drop1");
        puzzleDeselect(*selected);
        game.selected = 0;
        puzzleFadeRotate();
        return true;
    }
    if (selected->type == kGold || selected->type == kSilver) {
        puzzleDeselect(*selected);
        puzzleCloneSound("drop1");
        puzzleFadeRotate();
        game.selected = 0;
        return true;
    }
    if (target == nullptr || target->small) {
        puzzleCloneSound("drop2");
        puzzleTryMove(id, target != nullptr);
        puzzleFadeRotate();
    }
    return true;
}

void PuzzleScene::movePuzzlePointer(int x, int y) {
    // Mouse-move handler 0x00455360 (event 0x108): with a gem picked up,
    // redraw the path preview when a path exists. A failed search leaves
    // the previous preview in place.
    if (!puzzleGame || !puzzleGame->board) return;
    auto &game = *puzzleGame;
    if (game.busy || !game.playing) return;
    PuzzleGem *selected = puzzleGem(game.selected);
    if (selected == nullptr) return;
    const auto id = puzzleCellAt(x, y);
    if (!id) return;
    const int target = *id - 1;
    if (target < 0 || target >= kPuzzleCells) return;
    if (auto path = puzzleFindPath(selected->cell, target)) {
        puzzleDrawPath(*path, selected->type);
    }
}

// The path preview overlay 0x00455090 creates: 0x1CC x 0x140 at (170,172),
// layer 98, keyed on black.
constexpr int kOverlayWidth = 0x1CC;
constexpr int kOverlayHeight = 0x140;
constexpr SDL_Point kOverlayPosition{0xAA, 0xAC};

void PuzzleScene::puzzleClearPath() {
    auto &game = *puzzleGame;
    game.pathOverlay.assign(
        static_cast<std::size_t>(kOverlayWidth * kOverlayHeight), 0);
    game.pathDirty = true;
}

void PuzzleScene::puzzleDrawPath(const std::vector<int> &path, int type) {
    // 0x00454DA0 with drawing on: one key per path cell, from the target
    // back to the gem, at its projected point less (168, 180) (0x0049F628,
    // 0x0049F248), ten time units apart (0x0049C784); 0x00458630 then
    // samples the Kochanek-Bartels spline (0x00458030, tension, continuity
    // and bias all 0) ten times per key and joins the samples with
    // 0x00455FB0's anti-aliased lines in the gem's colour, added to
    // (0x8B, 0x2F, 0x3F).
    puzzleClearPath();
    auto &game = *puzzleGame;
    struct Key {
        float time;
        std::array<float, 2> point;
        std::array<float, 2> in{};
        std::array<float, 2> out{};
    };
    std::vector<Key> keys;
    for (std::size_t index = 0; index < path.size(); ++index) {
        const auto &point = puzzleOriginalData().path[
            static_cast<std::size_t>(path[index])];
        keys.push_back({static_cast<float>(index) * 10.0F,
                        {point[0] - 168.0F, point[1] - 180.0F}});
    }
    if (keys.size() < 2) return;
    const std::size_t last = keys.size() - 1;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (keys.size() < 3) {
            // 0x004585B0 / 0x004585F0.
            keys[0].out[axis] = keys[1].point[axis] - keys[0].point[axis];
            keys[1].in[axis] = keys[1].point[axis] - keys[0].point[axis];
            continue;
        }
        for (std::size_t index = 1; index < last; ++index) {
            // 0x004582B0.
            const Key &before = keys[index - 1];
            const Key &after = keys[index + 1];
            Key &key = keys[index];
            const float span = after.time - before.time;
            const float into = (key.time - before.time) / span;
            const float outof = (after.time - key.time) / span;
            const float sum = (key.point[axis] - before.point[axis]) +
                              (after.point[axis] - key.point[axis]);
            key.in[axis] = sum * into;
            key.out[axis] = sum * outof;
        }
        // 0x00458430 and 0x004584F0: the end tangents.
        const auto end = [&](const Key &a, const Key &b, const Key &c,
                             float near, float far) {
            const float weight = (0.25F - near / (far + far)) + 0.5F;
            return weight * (c.point[axis] - a.point[axis]) +
                   ((b.point[axis] - a.point[axis]) -
                    (c.point[axis] - a.point[axis]) * 0.5F) * 1.5F;
        };
        keys[0].out[axis] = end(keys[0], keys[1], keys[2],
                                keys[1].time - keys[0].time,
                                keys[2].time - keys[0].time);
        const Key &a = keys[last - 2];
        const Key &b = keys[last - 1];
        const Key &c = keys[last];
        const float weight =
            (0.25F - (c.time - b.time) / ((c.time - a.time) * 2.0F)) + 0.5F;
        keys[last].in[axis] =
            weight * (c.point[axis] - a.point[axis]) +
            ((c.point[axis] - b.point[axis]) -
             (c.point[axis] - a.point[axis]) * 0.5F) * 1.5F;
    }
    const auto sample = [&](float time) {
        if (time < keys[0].time) return keys[0].point;
        if (keys[last].time < time) return keys[last].point;
        std::size_t segment = 0;
        while (segment < last && keys[segment + 1].time < time) ++segment;
        if (segment >= last) segment = last - 1;
        const Key &from = keys[segment];
        const Key &to = keys[segment + 1];
        const float t = (time - from.time) / (to.time - from.time);
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h1 = (t3 + t3) - t2 * 3.0F + 1.0F;
        const float h2 = t2 * 3.0F - (t3 + t3);
        const float h3 = (t3 - (t2 + t2)) + t;
        const float h4 = t3 - t2;
        return std::array<float, 2>{
            h1 * from.point[0] + h3 * from.out[0] + h2 * to.point[0] +
                h4 * to.in[0],
            h1 * from.point[1] + h3 * from.out[1] + h2 * to.point[1] +
                h4 * to.in[1]};
    };
    const auto &rgb = kGemColours[static_cast<std::size_t>(
        std::clamp(type, 0, 10))];
    constexpr std::uint16_t base = ((0x8B & 0xF8) << 8) |
                                   ((0x2F & 0xFC) << 3) | (0x3F >> 3);
    const auto plot = [&](int x, int y, int intensity) {
        if (x < 0 || y < 0 || x >= kOverlayWidth || y >= kOverlayHeight) {
            return;
        }
        const int r = intensity * rgb[0] >> 8;
        const int g = intensity * rgb[1] >> 8;
        const int b = intensity * rgb[2] >> 8;
        const auto colour = static_cast<std::uint16_t>(
            ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
        const int red = std::min(31, (colour >> 11) + (base >> 11));
        const int green =
            std::min(63, ((colour >> 5) & 63) + ((base >> 5) & 63));
        const int blue = std::min(31, (colour & 31) + (base & 31));
        game.pathOverlay[static_cast<std::size_t>(y * kOverlayWidth + x)] =
            static_cast<std::uint16_t>((red << 11) | (green << 5) | blue);
    };
    const auto line = [&](std::array<float, 2> from,
                          std::array<float, 2> to) {
        // 0x00455FB0: step the major axis, split the minor fraction
        // between the two pixels it straddles (each at most half bright).
        const auto fixed = [](float value) {
            return static_cast<int>(value * 65536.0F);
        };
        int x0 = fixed(from[0]);
        int y0 = fixed(from[1]);
        int x1 = fixed(to[0]);
        int y1 = fixed(to[1]);
        const bool xMajor = std::abs(y1 - y0) <= std::abs(x1 - x0);
        int major0 = xMajor ? x0 : y0;
        int major1 = xMajor ? x1 : y1;
        int minor0 = xMajor ? y0 : x0;
        int minor1 = xMajor ? y1 : x1;
        if (major1 < major0) {
            std::swap(major0, major1);
            std::swap(minor0, minor1);
        }
        if (major1 - major0 == 0) return;
        const auto ceilFixed = [](int value) { return (value + 0xFFFF) >> 16; };
        const std::int64_t slope =
            (static_cast<std::int64_t>(minor1 - minor0) << 16) /
            std::max(major1 - major0, 0x7FFF);
        const int first = ceilFixed(major0);
        std::int64_t minor =
            minor0 + ((slope * (static_cast<std::int64_t>(first) * 65536 -
                                major0)) >> 16);
        const int steps = ceilFixed(major1) - first;
        for (int step = 0; step < steps; ++step) {
            const int pixel = ceilFixed(static_cast<int>(minor));
            const int fraction = static_cast<int>(minor >> 8) & 0xFF;
            const int near = fraction >> 1;
            const int far = ((256 - fraction) & 0xFF) >> 1;
            const int at = first + step;
            if (xMajor) {
                plot(at, pixel, near);
                plot(at, pixel - 1, far);
            } else {
                plot(pixel, at, near);
                plot(pixel - 1, at, far);
            }
            minor += slope;
        }
    };
    const int samples = static_cast<int>(keys.size()) * 10;
    const float step = keys[last].time / static_cast<float>(samples);
    float time = 0.0F;
    std::array<float, 2> previous = sample(keys[0].time);
    for (int index = 0; index < samples; ++index) {
        time += step;
        const auto current = sample(time);
        line(previous, current);
        previous = current;
    }
}

void PuzzleScene::puzzleUpload(SDL_Texture *texture, VideoFrame &frame,
                               const std::uint32_t *pixels, int width,
                               int height) {
    const auto bytes = static_cast<std::size_t>(width * height * 4);
    if (frame.bgra.size() != bytes) {
        frame.bgra.resize(bytes);
        frame.width = width;
        frame.height = height;
        frame.revision = ++puzzleFrameRevision;
        std::memcpy(frame.bgra.data(), pixels, bytes);
    } else if (std::memcmp(frame.bgra.data(), pixels, bytes) != 0) {
        frame.revision = ++puzzleFrameRevision;
        std::memcpy(frame.bgra.data(), pixels, bytes);
    }
    uploadVideoFrame(texture, frame);
}

void PuzzleScene::puzzleAnimate(float step) {
    // 0x004537B0 per gem: seed pop-in, growth, death (removed once its
    // timer passes 100), and the joker's colour cycle (visual only).
    auto &gems = puzzleGame->board->gems;
    std::vector<int> finished;
    for (auto &gem : gems) {
        if (gem.small) {
            // 0x00453930.
            if (gem.smallTime < kSmallPop) {
                const float t = gem.smallTime * (1.0F / kSmallPop);
                gem.seed.scale = t * 0.4F;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    gem.body.rotation[axis] =
                        -gem.rotation[axis] * t + gem.rotation[axis];
                }
            }
            gem.smallTime += step;
        }
        if (gem.growing) {
            // 0x004539C0.
            if (gem.growTime < kGrowLength) {
                const float t = gem.growTime * 0.01F;
                gem.body.material.diffuse = t * 0.5F;
                gem.body.material.specular = t;
                gem.body.rotation = {};
                gem.body.scale = 0.8F;
            }
            gem.body.material.flags = 0x20;
            gem.growTime += step;
        }
        if (gem.dying) {
            // 0x00453A50.
            if (gem.dieTime < kDieLength) {
                const float t = gem.dieTime / kDieLength;
                gem.body.material.diffuse = t * -0.5F + 0.5F;
                gem.body.material.specular = t * -1.0F + 1.0F;
                gem.body.scale = t * -0.8F + 0.8F;
                gem.seed.scale = t * -0.4F + 0.4F;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    gem.body.rotation[axis] = gem.rotation[axis] * t;
                    gem.seed.rotation[axis] = gem.rotation[axis] * t;
                }
            }
            gem.dieTime += step;
        }
        if (gem.type == kJoker) {
            // 0x00453B70: blend between neighbouring colours along the
            // stops at 0x00502048.
            const float value =
                puzzleColourNoise(gem.jokerTime * 0.005F, 0.0F, 0.7F, 0.7F) *
                    319.9F + 320.0F;
            gem.jokerTime += step;
            std::size_t stop = 6;
            while (stop > 0 && value < kJokerStops[stop]) --stop;
            if (value >= kJokerStops[stop]) {
                const float t = (value - kJokerStops[stop]) /
                                (kJokerStops[stop + 1] - kJokerStops[stop]);
                const auto &from = kGemColours[stop];
                const auto &to = kGemColours[stop + 1];
                const auto mix = [&](std::size_t channel) {
                    return static_cast<int>(
                        (static_cast<float>(to[channel]) -
                         static_cast<float>(from[channel])) * t +
                        static_cast<float>(from[channel]));
                };
                const int r = mix(0) & 0xFF;
                const int g = mix(1) & 0xFF;
                const int b = mix(2) & 0xFF;
                const auto colour = static_cast<std::uint16_t>(
                    ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                gem.body.material.colour = colour;
                gem.seed.material.colour = colour;
            }
        }
        if (gem.dying && kDieLength < gem.dieTime) finished.push_back(gem.id);
    }
    for (const int id : finished) puzzleRemoveGem(id);
}

void PuzzleScene::puzzleWobble(PuzzleGem &gem) {
    // 0x00455890 on the picked-up gem: pulse its size and sway it, from
    // the millisecond clock.
    const float clock = static_cast<float>(
        static_cast<std::uint32_t>(sceneElapsedNow() * 1000.0));
    gem.body.scale =
        puzzleWobbleNoise(clock * 0.0013333333F, 0.0F, 0.0F, 0.0F) * 0.25F +
        0.75F;
    gem.body.rotation[0] =
        puzzleWobbleNoise(clock * 0.001F, 0.0F, 0.75F, 0.75F) * 35.0F;
    gem.body.rotation[2] =
        puzzleWobbleNoise(clock * -0.001F, 0.0F, 0.75F, 0.75F) * 35.0F;
    gem.body.rotation[1] +=
        puzzleWobbleNoise(clock * 0.00125F, 0.0F, 0.75F, 0.75F);
}

void PuzzleScene::puzzleFinishClips(double now) {
    auto &game = *puzzleGame;
    for (auto clone = game.clones.begin(); clone != game.clones.end();) {
        if (now < clone->endsAt) {
            ++clone;
            continue;
        }
        referenceTrace.emit("animation_stop", {
            {"asset", clone->asset}, {"reason", "stopped"}});
        referenceTrace.emit("animation_remove", {
            {"asset", clone->asset}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", clone->asset}, {"reason", "stopped"}});
        clone = game.clones.erase(clone);
    }
    // Event 0x110 handler 0x00455BC0.
    if (game.lidClip.playing &&
        now - game.lidClip.startedAt >=
            originalSmackerCompletionDuration(game.lid)) {
        game.lidClip = {};
        referenceTrace.emit("animation_stop", {
            {"asset", "lid"}, {"reason", "finished"}});
        referenceTrace.emit("animation_remove", {
            {"asset", "lid"}, {"reason", "removed"}});
        puzzleStartBoard();
        game.playing = true;
        game.busy = false;
    }
    if (game.lid2Clip.playing &&
        now - game.lid2Clip.startedAt >=
            originalSmackerCompletionDuration(game.lid2)) {
        game.lid2Clip = {};
        if (game.board) puzzleDestroyBoard();
        referenceTrace.emit("animation_stop", {
            {"asset", "lid2"}, {"reason", "finished"}});
        referenceTrace.emit("animation_remove", {
            {"asset", "lid2"}, {"reason", "removed"}});
        puzzleInitialize();
    }
}

void PuzzleScene::continuePuzzleAfterGameOverMessage() {
    // 0x00455917-0x00455920: the score table follows the modal.
    if (!puzzleGame || !puzzleGame->gameOverMessagePending) return;
    puzzleGame->gameOverMessagePending = false;
    puzzleGame->highScorePending = true;
    showHighScore(puzzleGame->finalScore);
}

void PuzzleScene::continuePuzzleAfterHighScore() {
    // 0x00455925: lid2 closes the box; 0x00455BC0 resets the scene when it
    // finishes.
    if (!puzzleGame || !puzzleGame->highScorePending) return;
    auto &game = *puzzleGame;
    game.highScorePending = false;
    game.lid2Clip.added = true;
    game.lid2Clip.playing = true;
    game.lid2Clip.startedAt = puzzleNow();
    referenceTrace.emit("animation_add", {
        {"asset", "lid2"}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    audioMixer.addEffect(game.lid2, audioVolumes[0] * sceneAudioGains[0]);
}

void PuzzleScene::updatePuzzle(double elapsed) {
    if (!puzzleGame) return;
    auto &game = *puzzleGame;
    if (!game.initialized) {
        game.initialized = true;
        puzzleInitialize();
    }
    if (game.board) {
        // 0x00455890: one pass per frame, timed by the millisecond clock.
        const int nowMs = static_cast<int>(std::floor(elapsed * 1000.0));
        const int lastMs = static_cast<int>(
            std::floor(std::max(0.0, game.lastUpdate) * 1000.0));
        game.lastUpdate = elapsed;
        const float step = static_cast<float>(std::max(0, nowMs - lastMs)) *
                           kUnitsPerMs;
        if (game.gameOver && !game.busy) {
            game.gameOverTime += step;
            if (kGameOverDelay < game.gameOverTime &&
                dialogMode == DialogMode::None) {
                game.busy = true;
                game.gameOverMessagePending = true;
                showMessageWithTitle(
                    language.get("puzzle", "GAMEOVER", "Spillet er over"),
                    language.get("puzzle", "LOOSE",
                                 "Det er dessverre ingen flere mulige "
                                 "trekk."),
                    MessageKind::PuzzleGameOver);
            }
        }
        if (game.board) {
            if (PuzzleGem *gem = puzzleGem(game.selected)) puzzleWobble(*gem);
            puzzleAnimate(step);
        }
        // 0x00455AF0.
        if (game.rotateFading && game.rotateFadeTime <= kRotateFade) {
            float level = game.rotateFadeTime * 0.01F;
            if (game.rotateFadeOut) level = 1.0F - level;
            audioMixer.setEffectVolume(
                game.rotateEffect,
                audioVolumes[0] * sceneAudioGains[0] * level);
            game.rotateFadeTime += game.rotateFadeOut ? step * 3.0F : step;
            if (kRotateFade <= game.rotateFadeTime && game.rotateFadeOut) {
                game.rotateFading = false;
                puzzleStopRotate();
            }
        }
    }
    puzzleFinishClips(elapsed);
}

void PuzzleScene::renderPuzzle(double elapsed) {
    if (!puzzleGame) return;
    auto &game = *puzzleGame;
    // Layer 98: the path preview overlay.
    if (game.board && !game.pathOverlay.empty()) {
        if (!game.pathTexture) {
            game.pathTexture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, kOverlayWidth, kOverlayHeight));
            if (game.pathTexture) {
                SDL_SetTextureBlendMode(game.pathTexture.get(),
                                        SDL_BLENDMODE_BLEND);
            }
            game.pathDirty = true;
        }
        if (game.pathTexture && game.pathDirty) {
            std::vector<std::uint32_t> argb(game.pathOverlay.size());
            for (std::size_t index = 0; index < argb.size(); ++index) {
                const std::uint16_t pixel = game.pathOverlay[index];
                if (pixel == 0) continue;
                const std::uint32_t r = (pixel >> 11) & 31U;
                const std::uint32_t g = (pixel >> 5) & 63U;
                const std::uint32_t b = pixel & 31U;
                argb[index] = 0xFF000000U | (((r << 3) | (r >> 2)) << 16) |
                              (((g << 2) | (g >> 4)) << 8) |
                              ((b << 3) | (b >> 2));
            }
            puzzleUpload(game.pathTexture.get(), game.pathFrame, argb.data(),
                         kOverlayWidth, kOverlayHeight);
            game.pathDirty = false;
        }
        if (game.pathTexture) {
            const SDL_Rect destination{kOverlayPosition.x, kOverlayPosition.y,
                                       kOverlayWidth, kOverlayHeight};
            SDL_RenderCopy(renderer, game.pathTexture.get(), nullptr,
                           &destination);
        }
    }
    const auto drawFrame = [&](const DecodedMedia &media, SDL_Texture *texture,
                               std::size_t frameIndex, int x, int y) {
        if (texture == nullptr || media.videoFrames.empty()) return;
        frameIndex = std::min(frameIndex, media.videoFrames.size() - 1U);
        const auto &frame = media.videoFrames[frameIndex];
        uploadVideoFrame(texture, frame);
        const SDL_Rect destination{x, y, frame.width, frame.height};
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
    };
    // Layer 100: the bottle (added first), then every gem's 64x64 surface,
    // keyed on black, in list order.
    if (game.bottleClip.added) {
        drawFrame(game.bottle, game.bottleTexture.get(),
                  static_cast<std::size_t>(std::max(0, game.bottleFrame)),
                  kBottleX, kBottleY);
    }
    if (game.board) {
        constexpr int size = PuzzleGemRenderer::kSize;
        std::array<std::uint16_t, size * size> pixels{};
        std::array<std::uint32_t, size * size> argb{};
        for (auto &gem : game.board->gems) {
            const std::array<PuzzleMeshInstance, 2> meshes{gem.body, gem.seed};
            game.gemRenderer.render(
                puzzleOriginalData().centres[static_cast<std::size_t>(
                    gem.cell)],
                meshes, pixels);
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                const std::uint16_t pixel = pixels[index];
                if (pixel == 0) {
                    argb[index] = 0;
                    continue;
                }
                const std::uint32_t r = (pixel >> 11) & 31U;
                const std::uint32_t g = (pixel >> 5) & 63U;
                const std::uint32_t b = pixel & 31U;
                argb[index] = 0xFF000000U | (((r << 3) | (r >> 2)) << 16) |
                              (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
            if (!gem.texture) {
                gem.texture.reset(SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, size, size));
                if (!gem.texture) continue;
                SDL_SetTextureBlendMode(gem.texture.get(),
                                        SDL_BLENDMODE_BLEND);
            }
            puzzleUpload(gem.texture.get(), gem.frame, argb.data(), size,
                         size);
            const auto anchor =
                puzzleOriginalData().anchors[static_cast<std::size_t>(gem.cell)];
            const SDL_Rect destination{anchor.x, anchor.y, size, size};
            SDL_RenderCopy(renderer, gem.texture.get(), nullptr, &destination);
        }
    }
    // The score and level text controls.
    if (game.textControls) {
        if (!game.scoreText.empty()) {
            drawCenteredText(renderer, dialogFont, game.scoreText, kScoreRect);
        }
        if (!game.levelText.empty()) {
            drawCenteredText(renderer, dialogFont, game.levelText, kLevelRect);
        }
    }
    // Layer 120: the lid, held on its first frame until Start.
    if (game.lidClip.added) {
        const std::size_t frame = game.lidClip.playing
            ? frameIndexAt(game.lid, elapsed - game.lidClip.startedAt, false)
            : 0U;
        drawFrame(game.lid, game.lidTexture.get(), frame, 0, 0);
    }
    if (game.lid2Clip.added) {
        drawFrame(game.lid2, game.lidTexture.get(),
                  frameIndexAt(game.lid2, elapsed - game.lid2Clip.startedAt,
                               false),
                  0, 0);
    }
    if (!game.snapshotPath.empty()) {
        // Developer aid: the frame as drawn so far, without the shared
        // corner controls and dialogs drawn after the scene.
        int width = 800;
        int height = 600;
        SDL_GetRendererOutputSize(renderer, &width, &height);
        SDL_RenderSetLogicalSize(renderer, 0, 0);
        SurfacePtr shot(SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        if (shot && SDL_RenderReadPixels(renderer, nullptr,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         shot->pixels, shot->pitch) == 0) {
            SDL_SaveBMP(shot.get(), game.snapshotPath.c_str());
        }
        SDL_RenderSetLogicalSize(renderer, 800, 600);
        game.snapshotPath.clear();
    }
}

bool PuzzleScene::executePuzzleDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "puzzle:";
    if (activeScene != "puzzle" || !puzzleGame) return false;
    const std::string_view control = command.substr(prefix.size());
    auto &game = *puzzleGame;
    if (control.rfind("seed:", 0) == 0) {
        // Matches the capture fixture in the x32dbg script.
        puzzleFixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control.rfind("after:", 0) == 0) {
        // Holds the developer command queue until this scene time (seconds).
        return sceneElapsedNow() >= std::stod(std::string(control.substr(6)));
    } else if (control == "start") {
        if (!game.initialized) {
            game.initialized = true;
            puzzleInitialize();
        }
        beginPuzzleInteraction(400, 350);
    } else if (control.rfind("cell:", 0) == 0) {
        // Click the centre of a board cell (column + 9 * row).
        const int cell = std::stoi(std::string(control.substr(5)));
        if (cell < 0 || cell >= kPuzzleCells) return false;
        const auto seed = puzzleOriginalData().seeds[static_cast<std::size_t>(cell)];
        beginPuzzleInteraction(seed.x * 4 + 2, seed.y * 4 + 2);
    } else if (control.rfind("hover:", 0) == 0) {
        // Move the pointer over the centre of a board cell.
        const int cell = std::stoi(std::string(control.substr(6)));
        if (cell < 0 || cell >= kPuzzleCells) return false;
        const auto seed = puzzleOriginalData().seeds[static_cast<std::size_t>(cell)];
        movePuzzlePointer(seed.x * 4 + 2, seed.y * 4 + 2);
    } else if (control.rfind("snapshot:", 0) == 0) {
        game.snapshotPath = std::string(control.substr(9));
    } else if (control == "status") {
        std::cout << "Puzzle status: playing " << game.playing << ", busy "
                  << game.busy << ", game over " << game.gameOver;
        if (game.board) {
            std::cout << ", level " << game.board->level << ", score "
                      << game.board->score * 7 << ", gems "
                      << game.board->gems.size() << '\n';
            for (int row = kPuzzleRows - 1; row >= 0; --row) {
                std::cout << "  ";
                for (int column = 0; column < kPuzzleColumns; ++column) {
                    const PuzzleGem *gem = puzzleCellGem(column + row * 9);
                    if (gem == nullptr) {
                        std::cout << " .";
                    } else {
                        const char symbol = gem->type < 8
                            ? static_cast<char>('0' + gem->type)
                            : "JGS"[gem->type - 8];
                        std::cout << ' '
                                  << (gem->small
                                          ? static_cast<char>(symbol + 0x20 *
                                                (symbol >= 'A'))
                                          : symbol)
                                  << (gem->small ? '\'' : ' ');
                    }
                }
                std::cout << '\n';
            }
        } else {
            std::cout << '\n';
        }
    } else {
        throw std::runtime_error("Unknown developer Puzzle control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
