#include "game/app_sliding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#include "assets/profile.h"

// Sliding is a port of FGP.exe 0x0046dc50-0x0046ef10. Every rule names the
// address it was read from; REVERSE_ENGINEERING.md ("Sliding puzzle") has
// the complete account.

namespace fgp {
namespace {

// 0x0046dd10: control ids.
constexpr int kHelpControl = 0;
constexpr int kExitControl = 1;
constexpr int kPrintControl = 2;
constexpr int kScoreControl = 3;
constexpr int kStartControl = 5;
// 0x00406690 leaves the panel layer at 1; the clock box uses 1 too.
constexpr int kPanelLayer = 1;
// 0x0046e1f8: tile hotspots are columns * y + x + 7.
constexpr std::uint32_t kTileHotspot = 7U;
// [0x0049ffac]: the shuffle's move count.
constexpr int kShuffleMoves = 0x3e8;
constexpr int kClockPeriod = 500;

int slidingGridOf(int picture) { return slidingGridSize(picture); }

} // namespace

int SlidingScene::slidingClock() const {
    return static_cast<int>(std::floor(
        const_cast<SlidingScene *>(this)->sceneElapsedNow() * 1000.0));
}

SlidingScene::SlidingGame SlidingScene::loadSlidingGame(Package &package) {
    SlidingGame game;
    loadPuzzleStage(game.stage, package);
    // 0x004078c0(0x006c3318, 0x2b0, 0x6a, 0x5c, 0x1d, 1): the clock.
    game.clock.width = 0x5c;
    game.clock.height = 0x1d;
    game.picture490.defaultX = 0x9b;   // 0x0050a448 +0x54/+0x58
    game.picture490.defaultY = 0x37;
    game.preview.defaultX = 0x2b0;     // 0x0050a3a0
    game.preview.defaultY = 0x97;
    sceneCornerButtons.clear();
    return game;
}

void SlidingScene::openSlidingChooser() {
    // 0x0046e880: the whole library; sliding takes no category argument.
    openPuzzleChooser(slidingGame->stage, "sliding", "CHOOSE", 0, kPictureCount,
                      slidingGridOf);
}

void SlidingScene::clearSlidingBoard() {
    // 0x0046e7c0: the large picture and every placed tile.
    auto &game = *slidingGame;
    puzzleRemove(game.stage, game.picture490);
    if (!game.session) return;
    for (int y = 0; y < game.grid; ++y) {
        for (int x = 0; x < game.grid; ++x) {
            auto &tile = game.tiles[static_cast<std::size_t>(y * 5 + x)];
            puzzleRemove(game.stage, tile.descriptor);
            tile.descriptor.ownedTexture.reset();
        }
    }
    game.session = false;
}

void SlidingScene::startSlidingGame(int picture) {
    auto &game = *slidingGame;
    auto &stage = game.stage;
    // 0x0046de60 after the chooser returned a picture.
    clearSlidingBoard();
    // 0x0046eb40: the session record, cleared first (0x0046eb49).
    if (game.fixedSeed) slidingRandom.seed(*game.fixedSeed);
    game.picture = picture;
    game.grid = slidingGridSize(picture);
    game.lockedCount = 0;
    game.locked = {};
    game.record = puzzlePictureRecord("sliding", picture);
    if (game.record == 0U) {
        game.record = 2U;
        setPuzzlePictureRecord("sliding", picture, 2U);
    }
    // 0x0046ebda-0x0046ec50: one locked tile above 3x3 past index 5, a second
    // above 4x4 past index 11, both strictly inside the board.
    if (game.grid > 3 && picture > 5) {
        game.lockedCount = 1;
        game.locked[0].x = slidingRandom.below(game.grid - 2) + 1;
        game.locked[0].y = slidingRandom.below(game.grid - 2) + 1;
    }
    if (game.grid > 4 && picture > 0xb) {
        game.lockedCount = 2;
        game.locked[1].x = slidingRandom.below(game.grid - 2) + 1;
        game.locked[1].y = slidingRandom.below(game.grid - 2) + 1;
    }
    game.session = true;
    if (game.record != 3U) game.record = 2U;
    puzzleSetControlState(stage, kPrintControl, 3);
    puzzleSetControlState(stage, kScoreControl, 0);
    buildSlidingBoard();
    shuffleSliding();
    // 0x0046de95-0x0046decf: the medium preview.
    puzzleRemove(stage, game.preview);
    game.preview.asset =
        "..\\..\\common\\bitmap\\medium\\" + pictureFileName(picture);
    const std::string asset =
        "bitmap\\medium\\" + pictureFileName(picture) + ".bmp";
    for (Package *package : {addonCommon.get(), data2Common.get(),
                             editionCommon.get(), &common}) {
        if (!package) continue;
        const auto *entry = package->findOptional(asset);
        if (!entry) continue;
        SurfacePtr surface = loadBmp(package->data(*entry));
        game.preview.width = surface->w;
        game.preview.height = surface->h;
        game.preview.ownedTexture = makeTexture(renderer, surface.get());
        break;
    }
    puzzleAdd(stage, game.preview, -1, -1, 0);
    game.clockStart = slidingClock();
    game.clockTimerDue = game.clockStart + kClockPeriod;
    game.elapsed = 0;
    slidingTimer(game.clockStart);
}

void SlidingScene::buildSlidingBoard() {
    auto &game = *slidingGame;
    auto &stage = game.stage;
    // 0x0046df60.
    const std::string asset =
        "bitmap\\large\\" + pictureFileName(game.picture) + ".bmp";
    SurfacePtr surface;
    for (Package *package : {addonCommon.get(), data2Common.get(),
                             editionCommon.get(), &common}) {
        if (!package) continue;
        if (const auto *entry = package->findOptional(asset)) {
            surface = loadBmp(package->data(*entry));
            break;
        }
    }
    if (!surface) throw std::runtime_error("Missing sliding picture " + asset);
    game.picture490.asset =
        "..\\..\\common\\bitmap\\large\\" + pictureFileName(game.picture);
    game.picture490.width = surface->w;
    game.picture490.height = surface->h;
    game.picture490.ownedTexture = makeTexture(renderer, surface.get());
    Rgb565Image picture = rgb565FromSurface(surface.get());
    game.tileWidth = picture.width / game.grid;
    game.tileHeight = picture.height / game.grid;
    // 0x0046dfe0-0x0046e0a0: two-pixel grid lines (0x3c) in a 0x80 plane,
    // four blur passes, then the bevel with light direction 4.
    HeightField lines(kHeightFieldSize, 0x80U);
    HeightField scratch(kHeightFieldSize, 0x80U);
    for (int column = 0; column <= game.grid; ++column) {
        for (int row = 0; row < kHeightFieldStride; ++row) {
            const std::size_t index = static_cast<std::size_t>(row) *
                                          kHeightFieldStride +
                                      static_cast<std::size_t>(game.tileWidth * column);
            if (index < lines.size()) lines[index] = 0x3cU;
            if (index + 1 < lines.size()) lines[index + 1] = 0x3cU;
        }
    }
    for (int row = 0; row <= game.grid; ++row) {
        for (int x = 0; x < kHeightFieldStride; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(game.tileHeight * row) * kHeightFieldStride +
                static_cast<std::size_t>(x);
            if (index < lines.size()) lines[index] = 0x3cU;
            if (index + kHeightFieldStride < lines.size()) {
                lines[index + kHeightFieldStride] = 0x3cU;
            }
        }
    }
    blurHeightField(scratch, lines);
    blurHeightField(lines, scratch);
    blurHeightField(scratch, lines);
    blurHeightField(lines, scratch);
    bevelImage(picture, lines, 4);
    // 0x0046e0d9 / 0x0046e100: the board centred on the screen.
    game.originX = (800 - game.grid * game.tileWidth) / 2;
    game.originY = (600 - game.grid * game.tileHeight) / 2;
    game.blankX = 0;
    game.blankY = 0;
    game.cells.fill(nullptr);
    for (int y = 0; y < game.grid; ++y) {
        for (int x = 0; x < game.grid; ++x) {
            auto &tile = game.tiles[static_cast<std::size_t>(y * 5 + x)];
            tile.homeX = tile.x = x;
            tile.homeY = tile.y = y;
            tile.locked =
                (game.lockedCount >= 1 && game.locked[0].x == x &&
                 game.locked[0].y == y) ||
                (game.lockedCount >= 2 && game.locked[1].x == x &&
                 game.locked[1].y == y);
            tile.image.width = game.tileWidth;
            tile.image.height = game.tileHeight;
            tile.image.pixels.resize(
                static_cast<std::size_t>(game.tileWidth) * game.tileHeight);
            for (int ty = 0; ty < game.tileHeight; ++ty) {
                for (int tx = 0; tx < game.tileWidth; ++tx) {
                    tile.image.at(tx, ty) =
                        picture.at(game.tileWidth * x + tx, game.tileHeight * y + ty);
                }
            }
            auto &descriptor = tile.descriptor;
            descriptor.asset.clear();
            descriptor.defaultX = game.tileWidth * x + game.originX;
            descriptor.defaultY = game.tileHeight * y + game.originY;
            descriptor.width = game.tileWidth;
            descriptor.height = game.tileHeight;
            descriptor.keyed = false;
            descriptor.hotspot = static_cast<std::uint32_t>(game.grid * y + x) +
                                 kTileHotspot;
            SurfacePtr tileSurface = surfaceFromRgb565(tile.image, false);
            descriptor.ownedTexture = makeTexture(renderer, tileSurface.get());
            // 0x0046e201: " %2d " in Amerigo BT_10_, flags 0x200.
            char label[16];
            std::snprintf(label, sizeof label, " %2d ", game.grid * y + x + 1);
            descriptor.label = label;
            descriptor.labelFont = &game.stage.amerigo10;
            // 0x0046e226: the home cell (0,0) is the blank and is never placed.
            if (x == 0 && y == 0) continue;
            game.cells[static_cast<std::size_t>(y * 5 + x)] = &tile;
            puzzleAdd(stage, descriptor, -1, -1, 0);
        }
    }
    // 0x0046e283-0x0046e33b: the red frames of the locked tiles.
    for (auto &frame : game.frames) puzzleRemove(stage, frame);
    for (int index = 0; index < game.lockedCount; ++index) {
        auto &frame = game.frames[static_cast<std::size_t>(index)];
        char name[16];
        std::snprintf(name, sizeof name, "fixed-%d", game.grid);
        frame.asset = name;
        SurfacePtr bitmap = loadGreenKeyedBmp(scenePackage->data(
            scenePackage->find(std::string("bitmap\\") + name + ".bmp")));
        frame.width = bitmap->w;
        frame.height = bitmap->h;
        frame.ownedTexture = makeTexture(renderer, bitmap.get());
        SDL_SetTextureBlendMode(frame.ownedTexture.get(), SDL_BLENDMODE_BLEND);
        frame.keyed = true;
        frame.hotspot = 0U;
        frame.defaultX = game.locked[static_cast<std::size_t>(index)].x *
                             game.tileWidth + game.originX;
        frame.defaultY = game.locked[static_cast<std::size_t>(index)].y *
                             game.tileHeight + game.originY;
        puzzleAdd(stage, frame, -1, -1, 1);
    }
    game.busy = true;
}

void SlidingScene::slidingMovePiece(SlidingTile &tile, int x, int y) {
    // 0x0046e5f0: the tile takes the blank's cell, the blank its old one.
    auto &game = *slidingGame;
    game.blankX = tile.x;
    game.blankY = tile.y;
    game.cells[static_cast<std::size_t>(tile.y * 5 + tile.x)] = nullptr;
    tile.x = x;
    tile.y = y;
    game.cells[static_cast<std::size_t>(y * 5 + x)] = &tile;
    tile.descriptor.x = game.tileWidth * x + game.originX;
    tile.descriptor.y = game.tileHeight * y + game.originY;
}

bool SlidingScene::slidingMakeMove(int x, int y) {
    // 0x0046e400: slide the run between the blank and (x, y) one cell
    // towards the blank, unless a locked tile lies on the way.
    auto &game = *slidingGame;
    const auto cell = [&](int cx, int cy) {
        return game.cells[static_cast<std::size_t>(cy * 5 + cx)];
    };
    const auto lockedAt = [&](int cx, int cy) {
        const auto *tile = cell(cx, cy);
        return tile && tile->locked;
    };
    if (x == game.blankX) {
        if (y == game.blankY) return false;   // "invalid move"
        const int step = y > game.blankY ? 1 : -1;
        for (int cy = game.blankY + step;; cy += step) {
            if (lockedAt(game.blankX, cy)) return false;
            if (cy == y) break;
        }
        for (int cy = game.blankY; cy != y; cy += step) {
            slidingMovePiece(*cell(game.blankX, cy + step), game.blankX, cy);
        }
        return true;
    }
    if (y == game.blankY) {
        const int step = x > game.blankX ? 1 : -1;
        for (int cx = game.blankX + step;; cx += step) {
            if (lockedAt(cx, game.blankY)) return false;
            if (cx == x) break;
        }
        for (int cx = game.blankX; cx != x; cx += step) {
            slidingMovePiece(*cell(cx + step, game.blankY), cx, game.blankY);
        }
        return true;
    }
    // 0x0046e5c7: a diagonal target is reported and still returns 1.
    return true;
}

void SlidingScene::shuffleSliding() {
    // 0x0046e350: 1000 random legal row or column slides.
    auto &game = *slidingGame;
    referenceTrace.emit("minigame_state", {
        {"game", "sliding"}, {"state", "board"}, {"picture", game.picture},
        {"grid", game.grid}, {"locked", game.lockedCount}});
    for (int move = 0; move < kShuffleMoves; ++move) {
        int x = game.blankX;
        int y = game.blankY;
        if (!slidingRandom.oneIn(2)) {
            x = (slidingRandom.below(game.grid - 1) + 1 + game.blankX) % game.grid;
        } else {
            y = (slidingRandom.below(game.grid - 1) + 1 + game.blankY) % game.grid;
        }
        slidingMakeMove(x, y);
    }
    game.busy = false;
}

bool SlidingScene::slidingSolved() const {
    // 0x0046ed30.
    const auto &game = *slidingGame;
    if (game.blankX != 0 || game.blankY != 0) return false;
    for (int y = 0; y < game.grid; ++y) {
        for (int x = 0; x < game.grid; ++x) {
            const auto *tile = game.cells[static_cast<std::size_t>(y * 5 + x)];
            if (!tile) {
                if (x != 0 || y != 0) return false;
                continue;
            }
            if (tile->x != tile->homeX || tile->y != tile->homeY) return false;
        }
    }
    return true;
}

void SlidingScene::slidingWin() {
    // 0x0046edb0.
    auto &game = *slidingGame;
    auto &stage = game.stage;
    referenceTrace.emit("minigame_state", {
        {"game", "sliding"}, {"state", "solved"}, {"picture", game.picture}});
    game.busy = true;
    puzzleAdd(stage, game.picture490, -1, -1, 2);
    puzzleAdd(stage, game.tiles[0].descriptor, -1, -1, 0);
    puzzleSetControlState(stage, kPrintControl, 0);
    if (game.record == 2U) {
        // 0x0046edb6: edi starts at 1 for the picture just solved.
        int solved = 1;
        for (int picture = 0; picture < kPictureCount; ++picture) {
            if (puzzlePictureRecord("sliding", picture) == 3U) ++solved;
        }
        if (solved % 5 == 0) {
            const fs::path path = puzzleProfilePath();
            std::error_code error;
            if (!path.empty() && fs::exists(path, error)) {
                // 0x0041c180: one car part under awarddlg:TITLE_JIGSAW.
                const auto part = awardMedalCarPart(path);
                std::string number = std::to_string(part);
                if (number.size() < 2) number.insert(number.begin(), '0');
                showAward("bitmap\\award\\part_" + number + ".bmp",
                          "TITLE_JIGSAW", "DESC_JIGSAW");
            }
        }
        game.record = 3U;
        setPuzzlePictureRecord("sliding", game.picture, 3U);
    }
    // 0x0046ee63-0x0046ee83: 0x0041ee50(picture, elapsed, NULL, 0x102,
    // 0x006c0ff8, 0x006c5428): the picture's time table, record 0x401 keyed
    // by the picture, lowest time first.
    game.lastScoreRow = submitSceneScore(
        static_cast<std::uint32_t>(std::max(0, game.elapsed)),
        static_cast<std::uint32_t>(game.picture), kScoreTimeFlags);
    game.clockTimerDue.reset();
}

void SlidingScene::clickSlidingTile(int index) {
    // 0x0046eca0: the hotspot names the tile by its home cell; the move uses
    // where the tile is now.
    auto &game = *slidingGame;
    if (game.busy || !game.session || index < 0 || index >= game.grid * game.grid) {
        return;
    }
    const int slot = (index / game.grid) * 5 + index % game.grid;
    const auto &tile = game.tiles[static_cast<std::size_t>(slot)];
    if (tile.x == game.blankX) {
        if (tile.y == game.blankY) return;
    } else if (tile.y != game.blankY) {
        return;
    }
    if (slidingMakeMove(tile.x, tile.y)) {
        // 0x0046eea0: "slide 1".."slide 4".
        puzzlePlayClone(game.stage,
                        "slide " + std::to_string(slidingRandom.below(4) + 1));
    }
    if (slidingSolved()) slidingWin();
}

void SlidingScene::slidingTimer(int now) {
    // 0x0046e710: " mm:ss".
    auto &game = *slidingGame;
    const int seconds = (now - game.clockStart) / 1000;
    const int magnitude = std::abs(seconds);
    char text[16];
    std::snprintf(text, sizeof text, "%c%02d:%02d", seconds < 0 ? '-' : ' ',
                  magnitude / 60, magnitude % 60);
    game.clock.text = text;
}

void SlidingScene::slidingControlClicked(int id) {
    // 0x0046ef10 (event 0x116).
    auto &game = *slidingGame;
    referenceTrace.emit("click_dispatch", {{"scene", "sliding"}, {"hotspot", id}});
    switch (id) {
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        returnFromScene();
        break;
    case kPrintControl:
        // UNCERTAIN: as Jigsaw's Print, only the navigation is reproduced.
        if (game.session && game.busy) enterScene("activity", "activity");
        break;
    case kScoreControl:
        // 0x0046ef34-0x0046ef60: 0x0041f640 with the picture's key and
        // flags 0x102, boxing the last row while 0x006c33c0 is set.
        if (game.picture >= 0) {
            showScoreTable(activeScene, "",
                           static_cast<std::uint32_t>(game.picture),
                           kScoreTimeFlags,
                           game.busy ? game.lastScoreRow : std::nullopt);
        }
        break;
    case kStartControl:
        openSlidingChooser();
        break;
    default:
        break;
    }
}

bool SlidingScene::beginSlidingInteraction(int x, int y) {
    if (!slidingGame || activeScene != "sliding") return false;
    auto &game = *slidingGame;
    auto &stage = game.stage;
    if (stage.chooser.open) {
        puzzleChooserPress(stage, x, y);
        return true;
    }
    const std::uint32_t hotspot = puzzleHotspotAt(stage, x, y);
    referenceTrace.emit("click_dispatch", {{"scene", "sliding"}, {"hotspot", hotspot}});
    if (puzzlePressControl(stage, x, y)) return true;
    // 0x0046ec90: event 0x105 with the hotspot minus 7.
    clickSlidingTile(static_cast<int>(hotspot) - static_cast<int>(kTileHotspot));
    return true;
}

void SlidingScene::moveSlidingInteraction(int x, int y) {
    if (!slidingGame) return;
    if (slidingGame->stage.chooser.open) {
        puzzleChooserMove(slidingGame->stage, x, y);
    }
}

void SlidingScene::endSlidingInteraction(int x, int y) {
    if (!slidingGame) return;
    auto &stage = slidingGame->stage;
    if (stage.chooser.open) {
        puzzleChooserRelease(stage, x, y);
        return;
    }
    if (const auto control = puzzleReleaseControl(stage, x, y)) {
        slidingControlClicked(*control);
    }
}

bool SlidingScene::slidingKey(SDL_Keycode key) {
    if (!slidingGame || activeScene != "sliding") return false;
    auto &game = *slidingGame;
    if (game.stage.chooser.open) {
        puzzleChooserKey(game.stage, key);
        return true;
    }
    return false;
}

void SlidingScene::updateSliding(double) {
    if (!slidingGame) return;
    auto &game = *slidingGame;
    auto &stage = game.stage;
    if (!game.readyAnnounced) {
        game.readyAnnounced = true;
        // 0x0046dd10 (event 0x103).
        game.clock.font = &stage.lcd14;
        puzzleAdd(stage, game.clock, 0x2b0, 0x6a, kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kHelpControl, 0, 0,
                         {"help_0", "help_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kExitControl, 0x2d8, 0,
                         {"exit_0", "exit_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kPrintControl, 2, 0x200,
                         {"print_0", "print_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kStartControl, 0xf, 0x66,
                         {"button_0", "button_1", "", ""},
                         language.get("sliding", "START"), &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kScoreControl, 0xf, 0x9b,
                         {"button_0", "button_1", "button_0", ""},
                         language.get("sliding", "HISCORE"), &stage.amerigo10,
                         kPanelLayer);
        puzzleSetControlState(stage, kPrintControl, 3);
        puzzleSetControlState(stage, kScoreControl, 2);
        game.busy = false;
        openSlidingChooser();
        holdPuzzleSceneReady(stage);
    }
    if (stage.chooser.open && stage.chooser.result != 0) {
        const int picture = closePuzzleChooser(stage);
        if (picture >= 0) startSlidingGame(picture);
        releasePuzzleSceneReady(stage);
    }
    const int now = slidingClock();
    tickPuzzleClones(stage, now / 1000.0);
    if (stage.chooser.open) return;
    // 0x0046eed0 (event 0x112) and 0x0046eef0 (event 0x111).
    if (!game.busy) game.elapsed = now - game.clockStart;
    if (game.clockTimerDue && now >= *game.clockTimerDue) {
        const int due = *game.clockTimerDue;
        game.clockTimerDue = due + kClockPeriod;
        if (!game.busy) slidingTimer(due);
    }
}

void SlidingScene::renderSliding(double) {
    if (!slidingGame) return;
    drawPuzzleStage(slidingGame->stage);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
SlidingScene::slidingHotspotAt(int x, int y) const {
    if (!slidingGame || activeScene != "sliding") return std::nullopt;
    const auto hotspot = puzzleHotspotAt(slidingGame->stage, x, y);
    if (hotspot == 0U) return std::nullopt;
    return std::pair{hotspot, SDL_Rect{x, y, 1, 1}};
}

bool SlidingScene::executeSlidingDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "sliding:";
    if (activeScene != "sliding" || !slidingGame) return false;
    auto &game = *slidingGame;
    const auto control = command.substr(prefix.size());
    if (control.rfind("seed:", 0) == 0) {
        game.fixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control.rfind("choose:", 0) == 0) {
        if (!game.stage.chooser.open) return false;
        const int picture = std::stoi(std::string(control.substr(7)));
        const auto &rows = game.stage.chooser.rows;
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
            return row.picture == picture;
        });
        if (found == rows.end()) return false;
        game.stage.chooser.selected = static_cast<int>(found - rows.begin());
        game.stage.chooser.result = 1;
    } else if (control.rfind("tile:", 0) == 0) {
        // Click the tile whose home cell is the given 1-based number.
        clickSlidingTile(std::stoi(std::string(control.substr(5))) - 1);
    } else if (puzzlePointerCommand(control)) {
    } else if (control.rfind("snapshot:", 0) == 0) {
        puzzleSnapshot(std::string(control.substr(9)));
    } else if (control == "status") {
        std::cout << "Sliding status: picture " << game.picture << ", grid "
                  << game.grid << ", blank " << game.blankX << ','
                  << game.blankY << ", locked " << game.lockedCount
                  << ", solved " << (slidingSolved() ? 1 : 0) << '\n';
        for (int y = 0; game.session && y < game.grid; ++y) {
            std::cout << "  ";
            for (int x = 0; x < game.grid; ++x) {
                const auto *tile = game.cells[static_cast<std::size_t>(y * 5 + x)];
                std::cout << (tile ? tile->homeY * game.grid + tile->homeX + 1 : 0)
                          << (tile && tile->locked ? "* " : " ");
            }
            std::cout << '\n';
        }
    } else {
        throw std::runtime_error("Unknown developer Sliding control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
