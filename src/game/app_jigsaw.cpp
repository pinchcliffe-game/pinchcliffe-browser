#include "game/app_jigsaw.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

#include "assets/profile.h"

// Jigsaw is a port of FGP.exe 0x00469960-0x0046d1f0. Every rule names the
// address it was read from; REVERSE_ENGINEERING.md ("Jigsaw") has the
// complete account. Piece descriptors are the original's 0xa8-byte
// descriptors, so each SCENE_AddAnim (0x00412840) and SCENE_RemoveAnim
// (0x00412aa0) the original makes is mirrored in the reference trace.

namespace fgp {
namespace {

// 0x0046b441 / 0x0046b46c and 0x0046b000: the board's top-left corner.
constexpr int kBoardX = 0x99;
constexpr int kBoardY = 0x37;
// 0x0046bb80: control ids.
constexpr int kHelpControl = 1;
constexpr int kExitControl = 2;
constexpr int kStartControl = 3;
constexpr int kPrintControl = 4;
constexpr int kScoreControl = 5;
// 0x0046a98d: piece hotspots are 0x3e8 + index, below 0x7cf.
constexpr std::uint32_t kPieceHotspot = 1000U;
// 0x00469d24: joins need |offset| < 8 on both axes.
constexpr int kSnapTolerance = 8;
// 0x0046bb80 / 0x00406ec0(0xfffffc18): the panel and clock layer.
constexpr int kPanelLayer = -1000;
// 0x0046ae50 / 0x0046b3dc: the clock redraws every 500 ms; the solution
// steps every 100 ms (0x0046afe9).
constexpr int kClockPeriod = 500;
constexpr int kSolvePeriod = 100;
// 0x0046abc0 runs on every idle message (0x112). UNCERTAIN: the original's
// idle rate is a runtime property; Bee Maze's captures measured one pass per
// 21 ms (app_beemaze.cpp), which the slide to the frame uses here.
constexpr int kIdlePass = 21;
// 0x0046abd6: the completed board slides home at most 5 pixels per pass.
constexpr int kSlideStep = 5;

int jigsawGridOf(int picture) { return jigsawGridSize(picture); }

} // namespace

JigsawScene::JigsawPiece &JigsawScene::jigsawPiece(int index) {
    auto &game = *jigsawGame;
    const int rotation = game.rotations[static_cast<std::size_t>(index)];
    return game.sets[static_cast<std::size_t>(rotation)]
                    [static_cast<std::size_t>(index)];
}

int JigsawScene::jigsawClock() const {
    // 0x0040c450: the application clock in milliseconds.
    return static_cast<int>(std::floor(
        const_cast<JigsawScene *>(this)->sceneElapsedNow() * 1000.0));
}

JigsawScene::JigsawGame JigsawScene::loadJigsawGame(Package &package) {
    JigsawGame game;
    loadPuzzleStage(game.stage, package);
    // 0x0046bb80 (event 0x103). The scene argument selects one award
    // category's pictures (0x0046b864); the anaglyph enters with 9.
    if (activeSceneEntryMode != 0) game.category = activeSceneEntryMode;
    // 0x004078c0(0x006bfae0, 0x2b0, 0x6a, 0x5c, 0x1d, -1000): the clock.
    game.clock.width = 0x5c;
    game.clock.height = 0x1d;
    // The descriptors the board and win sequence add at (-1,-1).
    game.picture490.defaultX = kBoardX;   // 0x00509a78 +0x54/+0x58
    game.picture490.defaultY = kBoardY;
    game.preview.defaultX = 0x2b0;        // 0x00509b20
    game.preview.defaultY = 0x97;
    game.frame.asset = "frame";           // 0x005099d0
    game.frame.defaultX = 0x83;
    game.frame.defaultY = 0x22;
    // 0x005099d0 +0x48 is 0: the frame is drawn without a colour key. (Its
    // palette has no pure green, so a green key would hit the nearest entry,
    // the frame's own (156,115,49) bevel row.)
    SurfacePtr frame = loadBmp(package.data(package.find("bitmap\\frame.bmp")));
    game.frame.width = frame->w;
    game.frame.height = frame->h;
    game.frame.ownedTexture = makeTexture(renderer, frame.get());
    // The native corner buttons stand in for nothing here: the panel below
    // owns Help and Exit, as 0x0046bc0e-0x0046bc32 add them.
    sceneCornerButtons.clear();
    return game;
}

void JigsawScene::openJigsawChooser() {
    auto &game = *jigsawGame;
    // 0x0046b800: the whole library, or the entry category's range.
    int first = 0;
    int end = kPictureCount;
    if (game.category >= 0) {
        std::tie(first, end) = pictureCategoryRange(game.category);
    }
    openPuzzleChooser(game.stage, "jigsaw", "CHOOSE", first, end, jigsawGridOf);
}

void JigsawScene::clearJigsawBoard() {
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    // 0x0046b6b0: remove every placed piece and free the board.
    if (game.session) {
        for (int index = 0; index < game.grid * game.grid; ++index) {
            puzzleRemove(stage, jigsawPiece(index).descriptor);
        }
        for (auto &set : game.sets) set.clear();
        game.edges.clear();
        game.rotations.clear();
        game.session = false;
    }
    puzzleRemove(stage, game.picture490);
    puzzleRemove(stage, game.frame);
}

void JigsawScene::startJigsawGame(int picture) {
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    // 0x0046b2d0 after the chooser returned a picture.
    clearJigsawBoard();
    puzzleSetControlState(stage, kPrintControl, 3);
    puzzleSetControlState(stage, kScoreControl, 0);
    // 0x0046baf0: the session record.
    game.picture = picture;
    game.grid = jigsawGridSize(picture);
    game.rotation = game.grid > 3;
    game.record = puzzlePictureRecord("jigsaw", picture);
    if (game.record == 0U) {
        game.record = 2U;
        setPuzzlePictureRecord("jigsaw", picture, 2U);
    }
    game.session = true;
    puzzleSetControlCaption(stage, kStartControl,
                            language.get("jigsaw", "SOLUTION"));
    game.solutionUsed = false;
    game.solved = false;
    if (game.record != 3U) game.record = 2U;
    buildJigsawBoard();
    scatterJigsawPieces();
    // 0x0046b364-0x0046b3b7: the medium preview, re-added per picture.
    puzzleRemove(stage, game.preview);
    game.preview.asset =
        "..\\..\\common\\bitmap\\medium\\" + pictureFileName(picture);
    {
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
    }
    puzzleAdd(stage, game.preview, -1, -1, kPanelLayer);
    // 0x0046b3c1-0x0046b3f3: start the clock and its 500 ms redraw.
    game.clockStopped = false;
    game.clockStart = jigsawClock();
    game.clockTimerDue = game.clockStart + kClockPeriod;
    game.elapsed = 0;
    jigsawTimer(game.clockStart);
}

void JigsawScene::buildJigsawBoard() {
    auto &game = *jigsawGame;
    // 0x0046b4a0. The capture fixture reseeds rand() here.
    if (game.fixedSeed) jigsawRandom.seed(*game.fixedSeed);
    const int pieces = game.grid * game.grid;
    game.edges.assign(static_cast<std::size_t>(pieces), 0U);
    game.rotations.assign(static_cast<std::size_t>(pieces), 0);
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
    if (!surface) throw std::runtime_error("Missing jigsaw picture " + asset);
    game.source = rgb565FromSurface(surface.get());
    game.picture490.asset =
        "..\\..\\common\\bitmap\\large\\" + pictureFileName(game.picture);
    game.picture490.width = surface->w;
    game.picture490.height = surface->h;
    game.picture490.ownedTexture = makeTexture(renderer, surface.get());
    JigsawCut cut = cutJigsaw(game.source, game.grid, jigsawRandom);
    std::cout << "Jigsaw: cut picture " << game.picture << " into "
              << game.grid << 'x' << game.grid << " in " << cut.attempts
              << " attempt(s)\n";
    for (int rotation = 0; rotation < 4; ++rotation) {
        auto &set = game.sets[static_cast<std::size_t>(rotation)];
        set.resize(static_cast<std::size_t>(pieces));
        for (int index = 0; index < pieces; ++index) {
            auto &piece = set[static_cast<std::size_t>(index)];
            piece.image = std::move(cut.rotations[static_cast<std::size_t>(rotation)]
                                                 [static_cast<std::size_t>(index)]);
            auto &descriptor = piece.descriptor;
            descriptor.asset.clear();
            descriptor.defaultX = piece.image.homeX;
            descriptor.defaultY = piece.image.homeY;
            descriptor.width = piece.image.image.width;
            descriptor.height = piece.image.image.height;
            descriptor.keyed = true;          // +0x48
            descriptor.hotspot = kPieceHotspot + static_cast<std::uint32_t>(index);
            descriptor.pixels = &piece.image.image;
            SurfacePtr pieceSurface = surfaceFromRgb565(piece.image.image, true);
            descriptor.ownedTexture = makeTexture(renderer, pieceSurface.get());
            SDL_SetTextureBlendMode(descriptor.ownedTexture.get(),
                                    SDL_BLENDMODE_BLEND);
        }
    }
    // 0x0046b588-0x0046b697: the edge mask starts with the border edges
    // marked (bit 0 top, 1 left, 2 right, 3 bottom), and with rotation on
    // every pass draws each piece a new quarter turn.
    for (int pass = 0; pass < 4; ++pass) {
        for (int index = 0; index < pieces; ++index) {
            if (game.rotation) {
                game.rotations[static_cast<std::size_t>(index)] =
                    jigsawRandom.below(4);
            }
            auto &mask = game.edges[static_cast<std::size_t>(index)];
            if (index < game.grid) mask |= 1U;
            if (index >= (game.grid - 1) * game.grid) mask |= 8U;
            if (index % game.grid == 0) mask |= 2U;
            if (index % game.grid == game.grid - 1) mask |= 4U;
        }
    }
}

void JigsawScene::scatterJigsawPieces() {
    auto &game = *jigsawGame;
    // 0x0046b410: each piece at a random top-left inside the board, at a
    // layer equal to its index.
    const int range = kJigsawPictureSize - kJigsawPictureSize / game.grid;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "board"}, {"picture", game.picture},
        {"grid", game.grid}});
    for (int index = 0; index < game.grid * game.grid; ++index) {
        const int x = jigsawRandom.below(range) + kBoardX;
        const int y = jigsawRandom.below(range) + kBoardY;
        puzzleAdd(game.stage, jigsawPiece(index).descriptor, x, y, index);
    }
}

bool JigsawScene::jigsawComplete() const {
    // 0x00469bb0: every piece upright and joined on all four sides.
    const auto &game = *jigsawGame;
    if (!game.session) return true;
    for (std::size_t index = 0; index < game.edges.size(); ++index) {
        if (game.rotations[index] != 0 || game.edges[index] != 0xfU) return false;
    }
    return true;
}

void JigsawScene::jigsawPlayDrop() {
    // 0x00469b60("drop", 2): "drop1" or "drop2".
    const int variant = jigsawRandom.below(2) + 1;
    puzzlePlayClone(jigsawGame->stage, "drop" + std::to_string(variant));
}

void JigsawScene::jigsawSetGroupLayer(int piece, int layer,
                                      std::vector<char> &visited) {
    // 0x0046a810: set the layer of a piece and every piece joined to it.
    auto &game = *jigsawGame;
    const int grid = game.grid;
    puzzleSetLayer(game.stage, jigsawPiece(piece).descriptor, layer);
    visited[static_cast<std::size_t>(piece)] = 1;
    const auto mask = game.edges[static_cast<std::size_t>(piece)];
    const int up = piece - grid;
    // 0x0046a8b3: `0 < up`, so the up link never reaches piece 0.
    if (up > 0 && (mask & 1U) && !visited[static_cast<std::size_t>(up)]) {
        jigsawSetGroupLayer(up, layer, visited);
    }
    if (piece % grid != 0 && (mask & 2U) &&
        !visited[static_cast<std::size_t>(piece - 1)]) {
        jigsawSetGroupLayer(piece - 1, layer, visited);
    }
    if ((piece + 1) % grid != 0 && (mask & 4U) &&
        !visited[static_cast<std::size_t>(piece + 1)]) {
        jigsawSetGroupLayer(piece + 1, layer, visited);
    }
    const int down = piece + grid;
    if (down < grid * grid && (mask & 8U) &&
        !visited[static_cast<std::size_t>(down)]) {
        jigsawSetGroupLayer(down, layer, visited);
    }
}

void JigsawScene::jigsawRaise(int piece) {
    // 0x0046aa10: above the highest piece; past 100 every layer is
    // renormalised first.
    auto &game = *jigsawGame;
    const int pieces = game.grid * game.grid;
    int highest = std::numeric_limits<int>::min();
    for (int index = 0; index < pieces; ++index) {
        highest = std::max(highest, jigsawPiece(index).descriptor.layer);
    }
    std::vector<char> visited(36, 0);
    if (highest > 100) {
        for (int index = 0; index < pieces; ++index) {
            auto &descriptor = jigsawPiece(index).descriptor;
            puzzleSetLayer(game.stage, descriptor, descriptor.layer - highest);
        }
        jigsawSetGroupLayer(piece, 1, visited);
        return;
    }
    jigsawSetGroupLayer(piece, highest + 1, visited);
}

void JigsawScene::jigsawLower(int piece) {
    // 0x0046a750: below the lowest piece; past -100 renormalised first.
    auto &game = *jigsawGame;
    const int pieces = game.grid * game.grid;
    int lowest = std::numeric_limits<int>::max();
    for (int index = 0; index < pieces; ++index) {
        lowest = std::min(lowest, jigsawPiece(index).descriptor.layer);
    }
    std::vector<char> visited(36, 0);
    if (lowest < -100) {
        for (int index = 0; index < pieces; ++index) {
            auto &descriptor = jigsawPiece(index).descriptor;
            puzzleSetLayer(game.stage, descriptor, descriptor.layer - lowest);
        }
        jigsawSetGroupLayer(piece, -1, visited);
        return;
    }
    jigsawSetGroupLayer(piece, lowest - 1, visited);
}

void JigsawScene::pressJigsawPiece(int piece, int x, int y) {
    // 0x0046a9a0.
    auto &game = *jigsawGame;
    game.dragged = piece;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "pickup"}, {"piece", piece}});
    jigsawRaise(piece);
    const auto &descriptor = jigsawPiece(piece).descriptor;
    game.dragOffsetX = descriptor.x - x;
    game.dragOffsetY = descriptor.y - y;
}

void JigsawScene::jigsawAlignGroup(int piece, std::vector<char> &visited,
                                   int &depth) {
    // 0x0046a140: place a joined piece relative to the dragged one and give
    // it the dragged piece's layer.
    auto &game = *jigsawGame;
    const int grid = game.grid;
    const auto &anchor = jigsawPiece(game.dragged).descriptor;
    auto &descriptor = jigsawPiece(piece).descriptor;
    descriptor.x = descriptor.defaultX - anchor.defaultX + anchor.x;
    descriptor.y = descriptor.defaultY - anchor.defaultY + anchor.y;
    puzzleSetLayer(game.stage, descriptor, anchor.layer);
    visited[static_cast<std::size_t>(piece)] = 1;
    ++depth;
    const auto mask = game.edges[static_cast<std::size_t>(piece)];
    const int up = piece - grid;
    if (up >= 0 && !visited[static_cast<std::size_t>(up)] && (mask & 1U)) {
        jigsawAlignGroup(up, visited, depth);
    }
    const int left = piece - 1;
    if (left % grid != grid - 1 && left >= 0 &&
        !visited[static_cast<std::size_t>(left)] && (mask & 2U)) {
        jigsawAlignGroup(left, visited, depth);
    }
    const int right = piece + 1;
    if (right % grid != 0 && right < grid * grid &&
        !visited[static_cast<std::size_t>(right)] && (mask & 4U)) {
        jigsawAlignGroup(right, visited, depth);
    }
    const int down = piece + grid;
    if (down < grid * grid && !visited[static_cast<std::size_t>(down)] &&
        (mask & 8U)) {
        jigsawAlignGroup(down, visited, depth);
    }
    --depth;
}

void JigsawScene::jigsawJoin(int piece, std::vector<char> &visited,
                             int &depth) {
    // 0x00469c00: join every piece of the group to any unjoined neighbour of
    // the same rotation lying within 8 pixels of its place.
    auto &game = *jigsawGame;
    const int grid = game.grid;
    const int pieces = grid * grid;
    if (visited[static_cast<std::size_t>(piece)]) return;
    visited[static_cast<std::size_t>(piece)] = 1;
    ++depth;
    const int rotation = game.rotations[static_cast<std::size_t>(piece)];
    auto &self = jigsawPiece(piece).descriptor;
    // 0x00469c5c: the displacement is read once, before any snap.
    const int offsetX = self.x - self.defaultX;
    const int offsetY = self.y - self.defaultY;
    const auto tryJoin = [&](int neighbour, std::uint32_t bit,
                             std::uint32_t opposite) {
        auto &mask = game.edges[static_cast<std::size_t>(piece)];
        if (mask & bit) {
            jigsawJoin(neighbour, visited, depth);
            return;
        }
        if (game.rotations[static_cast<std::size_t>(neighbour)] != rotation) return;
        const auto &other = jigsawPiece(neighbour).descriptor;
        const int dx = (other.defaultX - other.x) + offsetX;
        const int dy = (other.defaultY - other.y) + offsetY;
        if (std::abs(dx) >= kSnapTolerance || std::abs(dy) >= kSnapTolerance) return;
        mask |= bit;
        game.edges[static_cast<std::size_t>(neighbour)] |= opposite;
        self.x = other.x - other.defaultX + self.defaultX;
        self.y = other.y - other.defaultY + self.defaultY;
        game.joined = true;
    };
    const int up = piece - grid;
    if (up < 0) {
        game.edges[static_cast<std::size_t>(piece)] |= 1U;
    } else {
        tryJoin(up, 1U, 8U);
    }
    const int left = piece - 1;
    if (left % grid == grid - 1 || left < 0) {
        game.edges[static_cast<std::size_t>(piece)] |= 2U;
    } else {
        tryJoin(left, 2U, 4U);
    }
    const int right = piece + 1;
    if (right % grid == 0 || right >= pieces) {
        game.edges[static_cast<std::size_t>(piece)] |= 4U;
    } else {
        tryJoin(right, 4U, 2U);
    }
    // 0x00469e9a: down is piece + rows (the board is square).
    const int down = piece + grid;
    if (down < pieces) {
        tryJoin(down, 8U, 1U);
    } else {
        game.edges[static_cast<std::size_t>(piece)] |= 8U;
    }
    --depth;
}

void JigsawScene::dropJigsawPiece() {
    // 0x0046ab30 (event 0x106).
    auto &game = *jigsawGame;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "drop"}, {"piece", game.dragged}});
    if (game.dragged == -1) return;
    game.joined = false;
    std::vector<char> visited(static_cast<std::size_t>(game.grid * game.grid), 0);
    int depth = -1;
    jigsawJoin(game.dragged, visited, depth);
    // 0x0046a0f3: the outermost call realigns the dragged group.
    if (!game.solutionUsed) {
        std::vector<char> aligned(visited.size(), 0);
        int alignDepth = -1;
        jigsawAlignGroup(game.dragged, aligned, alignDepth);
    }
    if (game.joined) jigsawPlayDrop();
    game.dragged = -1;
    if (jigsawComplete()) jigsawWin();
}

void JigsawScene::jigsawWin() {
    // 0x00469b90: stop the clock and start the slide home.
    auto &game = *jigsawGame;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "complete"}, {"picture", game.picture}});
    game.clockStopped = true;
    game.sliding = true;
    game.clockTimerDue.reset();
}

void JigsawScene::jigsawRotateGroup(int rotation, int piece,
                                    std::vector<char> &visited) {
    // 0x0046a530: give every joined piece the new rotation, placed relative
    // to the dragged piece.
    auto &game = *jigsawGame;
    const int grid = game.grid;
    if (visited[static_cast<std::size_t>(piece)]) return;
    visited[static_cast<std::size_t>(piece)] = 1;
    const auto &anchorBefore = jigsawPiece(game.dragged).descriptor;
    const int anchorX = anchorBefore.x;
    const int anchorY = anchorBefore.y;
    auto &old = jigsawPiece(piece).descriptor;
    const int layer = old.layer;
    puzzleRemove(game.stage, old);
    game.rotations[static_cast<std::size_t>(piece)] = rotation;
    const auto &anchor = jigsawPiece(game.dragged).descriptor;
    auto &fresh = jigsawPiece(piece).descriptor;
    puzzleAdd(game.stage, fresh, fresh.defaultX - anchor.defaultX + anchorX,
              fresh.defaultY - anchor.defaultY + anchorY, layer);
    const auto mask = game.edges[static_cast<std::size_t>(piece)];
    const int up = piece - grid;
    if (up >= 0 && !visited[static_cast<std::size_t>(up)] && (mask & 1U)) {
        jigsawRotateGroup(rotation, up, visited);
    }
    const int left = piece - 1;
    if (left % grid != grid - 1 && left >= 0 &&
        !visited[static_cast<std::size_t>(left)] && (mask & 2U)) {
        jigsawRotateGroup(rotation, left, visited);
    }
    const int right = piece + 1;
    // 0x0046a6c2: no upper bound test; (n*n) % n == 0 keeps it in range.
    if (right % grid != 0 && !visited[static_cast<std::size_t>(right)] &&
        (mask & 4U)) {
        jigsawRotateGroup(rotation, right, visited);
    }
    const int down = piece + grid;
    if (down < grid * grid && !visited[static_cast<std::size_t>(down)] &&
        (mask & 8U)) {
        jigsawRotateGroup(rotation, down, visited);
    }
}

void JigsawScene::jigsawRotate(int direction, int x, int y) {
    // 0x0046a3b0: 0x6f turns clockwise, 0x70 counter-clockwise; the dragged
    // piece is re-centred on the pointer.
    auto &game = *jigsawGame;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "rotate"}, {"piece", game.dragged},
        {"direction", direction}});
    if (game.dragged == -1) return;
    if (x == -1 || y == -1) {
        x = mouseX;
        y = mouseY;
    }
    auto &old = jigsawPiece(game.dragged).descriptor;
    const int layer = old.layer;
    puzzleRemove(game.stage, old);
    int &rotation = game.rotations[static_cast<std::size_t>(game.dragged)];
    if (direction == 0x6f) {
        rotation = rotation == 3 ? 0 : rotation + 1;
    } else if (direction == 0x70) {
        rotation = rotation == 0 ? 3 : rotation - 1;
    }
    auto &fresh = jigsawPiece(game.dragged).descriptor;
    puzzleAdd(game.stage, fresh, x - fresh.width / 2, y - fresh.height / 2,
              layer);
    game.dragOffsetX = -(fresh.width / 2);
    game.dragOffsetY = -(fresh.height / 2);
    std::vector<char> visited(static_cast<std::size_t>(game.grid * game.grid), 0);
    jigsawRotateGroup(rotation, game.dragged, visited);
}

void JigsawScene::jigsawSolveStep() {
    // 0x0046aee0: the Solution button places one piece per 100 ms.
    auto &game = *jigsawGame;
    referenceTrace.emit("minigame_state", {
        {"game", "jigsaw"}, {"state", "solve_step"}, {"piece", game.solveIndex}});
    if (game.solveIndex == -1) {
        game.solutionUsed = true;
        game.solveIndex = 0;
    }
    const int index = game.solveIndex;
    // 0x0046b000: upright, then home.
    if (game.rotations[static_cast<std::size_t>(index)] != 0) {
        auto &old = jigsawPiece(index).descriptor;
        const int layer = old.layer;
        const int x = old.x;
        const int y = old.y;
        puzzleRemove(game.stage, old);
        game.rotations[static_cast<std::size_t>(index)] = 0;
        puzzleAdd(game.stage, jigsawPiece(index).descriptor, x, y, layer);
    }
    auto &descriptor = jigsawPiece(index).descriptor;
    descriptor.x = descriptor.defaultX + kBoardX;
    descriptor.y = descriptor.defaultY + kBoardY;
    game.joined = false;
    std::vector<char> visited(static_cast<std::size_t>(game.grid * game.grid), 0);
    int depth = -1;
    jigsawJoin(index, visited, depth);
    if (game.joined) jigsawPlayDrop();
    const auto &placed = jigsawPiece(index).descriptor;
    if (placed.y == placed.defaultY + kBoardY &&
        placed.x == placed.defaultX + kBoardX) {
        ++game.solveIndex;
    }
    if (game.solveIndex == game.grid * game.grid) {
        game.solveIndex = -1;
        game.clockStopped = true;
        game.solved = true;
        puzzleSetControlCaption(game.stage, kStartControl,
                                language.get("jigsaw", "START"));
        return;
    }
    game.solveTimerDue = jigsawClock() + kSolvePeriod;
}

void JigsawScene::jigsawTimer(int now) {
    // 0x0046ae50: " mm:ss" (a leading '-' for a negative time).
    auto &game = *jigsawGame;
    const int milliseconds = now - game.clockStart;
    const int seconds = milliseconds / 1000;
    const int magnitude = std::abs(seconds);
    char text[16];
    std::snprintf(text, sizeof text, "%c%02d:%02d", seconds < 0 ? '-' : ' ',
                  magnitude / 60, magnitude % 60);
    game.clock.text = text;
}

void JigsawScene::jigsawFinishSlide() {
    // 0x0046ac56-0x0046ade2: frame and finished picture, the solved flag,
    // the car-part award every sixth solved picture, and the score.
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    for (int index = 0; index < game.grid * game.grid; ++index) {
        puzzleSetLayer(stage, jigsawPiece(index).descriptor, 0);
    }
    puzzleAdd(stage, game.frame, -1, -1, 1);
    puzzleAdd(stage, game.picture490, -1, -1, 2);
    game.sliding = false;
    if (!game.solutionUsed) {
        if (game.record == 2U) {
            // 0x0046acf9: ebx starts at 1 for the picture just solved.
            int solved = 1;
            for (int picture = 0; picture < kPictureCount; ++picture) {
                if (puzzlePictureRecord("jigsaw", picture) == 3U) ++solved;
            }
            if (solved % 6 == 0) {
                // 0x0041c180: one car part, "awarddlg:TITLE_JIGSAW".
                const fs::path path = puzzleProfilePath();
                std::error_code error;
                if (!path.empty() && fs::exists(path, error)) {
                    const auto part = awardMedalCarPart(path);
                    std::string number = std::to_string(part);
                    if (number.size() < 2) number.insert(number.begin(), '0');
                    showAward("bitmap\\award\\part_" + number + ".bmp",
                              "TITLE_JIGSAW", "DESC_JIGSAW");
                }
            }
            game.record = 3U;
            setPuzzlePictureRecord("jigsaw", game.picture, 3U);
        }
        // 0x0046ad62-0x0046ad81: 0x0041ee50(picture, elapsed, NULL, 0x102,
        // 0x006bd9e8, 0x006bfb8c): the picture's time table, record 0x401
        // keyed by the picture, lowest time first.
        game.lastScoreRow = submitSceneScore(
            static_cast<std::uint32_t>(std::max(0, game.elapsed)),
            static_cast<std::uint32_t>(game.picture), kScoreTimeFlags);
    }
    puzzleSetControlState(stage, kPrintControl, 0);
    puzzleSetControlCaption(stage, kStartControl, language.get("jigsaw", "START"));
}

void JigsawScene::jigsawIdle(int now) {
    // 0x0046abc0 (event 0x112).
    auto &game = *jigsawGame;
    if (!game.clockStopped) game.elapsed = now - game.clockStart;
    if (!game.sliding || !game.session) return;
    const auto &first = jigsawPiece(0).descriptor;
    const auto step = [](int offset) {
        if (offset < 1) return std::min(-offset, kSlideStep);
        return -std::min(offset, kSlideStep);
    };
    const int stepX = step(first.x - kBoardX);
    const int stepY = step(first.y - kBoardY);
    if (stepX == 0 && stepY == 0) {
        jigsawFinishSlide();
        return;
    }
    for (int index = 0; index < game.grid * game.grid; ++index) {
        auto &descriptor = jigsawPiece(index).descriptor;
        descriptor.x += stepX;
        descriptor.y += stepY;
    }
}

void JigsawScene::jigsawExit() {
    // 0x0046b0c0: back to the anaglyph when entered from it (category 9),
    // otherwise a tail jump (0x0046b0c9) to the registry parent, 0x0040cc30.
    if (jigsawGame && jigsawGame->category == 9) {
        enterScene("anaglyph", "anaglyph");
    } else {
        leaveToRegistryParent();
    }
}

void JigsawScene::jigsawControlClicked(int id) {
    // 0x0046b0e0 (event 0x116).
    auto &game = *jigsawGame;
    referenceTrace.emit("click_dispatch", {{"scene", "jigsaw"}, {"hotspot", id}});
    switch (id) {
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        jigsawExit();
        break;
    case kStartControl:
        if (game.sliding) break;
        if (!jigsawComplete() && !game.solved) {
            jigsawSolveStep();
        } else {
            openJigsawChooser();
        }
        break;
    case kPrintControl:
        // 0x0046b130: the finished picture goes to the Activity Centre.
        // UNCERTAIN: the scene's entry arguments (the printed surface) have
        // no native counterpart yet; only the navigation is reproduced.
        if (game.session && jigsawComplete()) enterScene("activity", "activity");
        break;
    case kScoreControl:
        // 0x0046b13b-0x0046b17d: 0x0041f640 with the picture's key and
        // flags 0x102, boxing the last row while the board is complete and
        // 0x006bfbac is clear.
        if (game.picture >= 0) {
            const bool boxed = jigsawComplete() && !game.solved;
            showScoreTable(activeScene, "",
                           static_cast<std::uint32_t>(game.picture),
                           kScoreTimeFlags,
                           boxed ? game.lastScoreRow : std::nullopt);
        }
        break;
    default:
        break;
    }
}

bool JigsawScene::beginJigsawInteraction(int x, int y) {
    if (!jigsawGame || activeScene != "jigsaw") return false;
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    game.leftPressed = true;
    if (stage.chooser.open) {
        puzzleChooserPress(stage, x, y);
        return true;
    }
    const std::uint32_t hotspot = puzzleHotspotAt(stage, x, y);
    // Event 0x105 reaches 0x0046a960 with the hotspot under the pointer;
    // the panel presses its control first (0x00406b50).
    referenceTrace.emit("click_dispatch", {{"scene", "jigsaw"}, {"hotspot", hotspot}});
    if (puzzlePressControl(stage, x, y)) return true;
    // 0x0046a960: no drag during the solution or once complete.
    if (!game.solutionUsed && !jigsawComplete() && hotspot >= kPieceHotspot &&
        hotspot < 1999U) {
        pressJigsawPiece(static_cast<int>(hotspot - kPieceHotspot), x, y);
    }
    return true;
}

bool JigsawScene::beginJigsawRightInteraction(int x, int y) {
    if (!jigsawGame || activeScene != "jigsaw") return false;
    auto &game = *jigsawGame;
    if (game.stage.chooser.open) return true;
    // 0x0046a350 (event 0x107).
    if (game.dragged == -1) {
        const std::uint32_t hotspot = puzzleHotspotAt(game.stage, x, y);
        if (hotspot >= kPieceHotspot && !jigsawComplete()) {
            jigsawLower(static_cast<int>(hotspot - kPieceHotspot));
        }
    } else if (game.rotation) {
        jigsawRotate(0x6f, x, y);
    }
    return true;
}

void JigsawScene::moveJigsawInteraction(int x, int y) {
    if (!jigsawGame) return;
    auto &game = *jigsawGame;
    if (game.stage.chooser.open) {
        puzzleChooserMove(game.stage, x, y);
        return;
    }
    // 0x0046aad0 (event 0x109).
    if (game.dragged == -1) return;
    auto &descriptor = jigsawPiece(game.dragged).descriptor;
    descriptor.x = game.dragOffsetX + x;
    descriptor.y = game.dragOffsetY + y;
    std::vector<char> visited(static_cast<std::size_t>(game.grid * game.grid), 0);
    int depth = -1;
    jigsawAlignGroup(game.dragged, visited, depth);
}

void JigsawScene::endJigsawInteraction(int x, int y) {
    if (!jigsawGame) return;
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    // The native scene starts on the press that entered it (Contents enters
    // on the original's 0x116, after that release), so a release whose press
    // belongs to another scene is not this scene's 0x106.
    if (!game.leftPressed) return;
    game.leftPressed = false;
    if (stage.chooser.open) {
        puzzleChooserRelease(stage, x, y);
        return;
    }
    // Event 0x106 always reaches 0x0046ab30; a control's click (0x116) is
    // posted by the panel and arrives after it.
    dropJigsawPiece();
    const auto control = puzzleReleaseControl(stage, x, y);
    if (control) jigsawControlClicked(*control);
}

bool JigsawScene::jigsawKey(SDL_Keycode key) {
    if (!jigsawGame || activeScene != "jigsaw") return false;
    auto &game = *jigsawGame;
    if (game.stage.chooser.open) {
        puzzleChooserKey(game.stage, key);
        return true;
    }
    // 0x0046ab80: the arrow keys turn the dragged piece.
    if (game.session && game.rotation) {
        if (key == SDLK_LEFT) {
            jigsawRotate(0x70, -1, -1);
            return true;
        }
        if (key == SDLK_RIGHT) {
            jigsawRotate(0x6f, -1, -1);
            return true;
        }
    }
    if (key == SDLK_ESCAPE) {
        // 0x00469a6d: Escape (event 0x10c) goes straight to 0x0046b0c0.
        jigsawExit();
        return true;
    }
    return false;
}

void JigsawScene::updateJigsaw(double) {
    if (!jigsawGame) return;
    auto &game = *jigsawGame;
    auto &stage = game.stage;
    if (!game.readyAnnounced) {
        game.readyAnnounced = true;
        // 0x0046bb80: clock, panel and the chooser, all from event 0x103.
        // (Font pointers are taken here, once the game sits in its optional.)
        game.clock.font = &stage.lcd14;
        puzzleAdd(stage, game.clock, 0x2b0, 0x6a, kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kHelpControl, 0, 0,
                         {"help_0", "help_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kExitControl, 0x2d8, 0,
                         {"exit_0", "exit_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kPrintControl, 2, 0x210,
                         {"print_0", "print_1", "", ""}, "", &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kStartControl, 0xf, 0x66,
                         {"solution_0", "solution_1", "", ""},
                         language.get("jigsaw", "START"), &stage.amerigo10,
                         kPanelLayer);
        puzzleAddControl(stage, *scenePackage, kScoreControl, 0xf, 0x9b,
                         {"solution_0", "solution_1", "solution_0", ""},
                         language.get("jigsaw", "HISCORE"), &stage.amerigo10,
                         kPanelLayer);
        puzzleSetControlState(stage, kPrintControl, 3);
        puzzleSetControlState(stage, kScoreControl, 2);
        openJigsawChooser();
        holdPuzzleSceneReady(stage);
        game.lastPass = jigsawClock();
    }
    if (stage.chooser.open && stage.chooser.result != 0) {
        const int picture = closePuzzleChooser(stage);
        if (picture >= 0) startJigsawGame(picture);
        releasePuzzleSceneReady(stage);
    }
    const int now = jigsawClock();
    tickPuzzleClones(stage, now / 1000.0);
    if (stage.chooser.open) {
        game.lastPass = now;
        return;
    }
    // Event 0x111: timers fire only while the clock runs (0x0046ae20).
    if (game.clockTimerDue && now >= *game.clockTimerDue) {
        const int due = *game.clockTimerDue;
        game.clockTimerDue = due + kClockPeriod;
        if (!game.clockStopped) jigsawTimer(due);
    }
    if (game.solveTimerDue && now >= *game.solveTimerDue) {
        const int due = *game.solveTimerDue;
        game.solveTimerDue.reset();
        if (!game.clockStopped) {
            jigsawSolveStep();
            jigsawTimer(due);
        }
    }
    while (now - game.lastPass >= kIdlePass) {
        game.lastPass += kIdlePass;
        jigsawIdle(game.lastPass);
    }
}

void JigsawScene::renderJigsaw(double) {
    if (!jigsawGame) return;
    drawPuzzleStage(jigsawGame->stage);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
JigsawScene::jigsawHotspotAt(int x, int y) const {
    if (!jigsawGame || activeScene != "jigsaw") return std::nullopt;
    const auto hotspot = puzzleHotspotAt(jigsawGame->stage, x, y);
    if (hotspot == 0U) return std::nullopt;
    return std::pair{hotspot, SDL_Rect{x, y, 1, 1}};
}

std::optional<SDL_Point> JigsawScene::jigsawPickPoint(int piece) {
    // Scenario authoring aid: a point where the piece is the topmost hit,
    // two pixels clear of any edge, searched outwards from its centre.
    auto &game = *jigsawGame;
    const auto &descriptor = jigsawPiece(piece).descriptor;
    const auto hotspot = kPieceHotspot + static_cast<std::uint32_t>(piece);
    const int cx = descriptor.x + descriptor.width / 2;
    const int cy = descriptor.y + descriptor.height / 2;
    for (int radius = 0; radius < 300; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int x = cx + dx;
                const int y = cy + dy;
                bool solid = true;
                for (int my = -2; my <= 2 && solid; ++my) {
                    for (int mx = -2; mx <= 2 && solid; ++mx) {
                        solid = puzzleHotspotAt(game.stage, x + mx, y + my) == hotspot;
                    }
                }
                if (solid) return SDL_Point{x, y};
            }
        }
    }
    return std::nullopt;
}

bool JigsawScene::executeJigsawDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "jigsaw:";
    if (activeScene != "jigsaw" || !jigsawGame) return false;
    auto &game = *jigsawGame;
    const auto control = command.substr(prefix.size());
    if (control.rfind("seed:", 0) == 0) {
        // Mirrors the capture fixture's reseed at the board build.
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
    } else if (control.rfind("autoplace", 0) == 0) {
        // Scenario authoring aid: drag pieces home through the real press,
        // move and release handlers, printing each drag. "autoplace" places
        // every piece, "autoplace:N" only piece N.
        if (!game.session) return false;
        std::vector<int> order;
        if (control.size() > 10) {
            order.push_back(std::stoi(std::string(control.substr(10))));
        } else {
            for (int index = 0; index < game.grid * game.grid; ++index) {
                order.push_back(index);
            }
        }
        for (const int piece : order) {
            const auto &descriptor = jigsawPiece(piece).descriptor;
            const auto hotspot = kPieceHotspot + static_cast<std::uint32_t>(piece);
            const auto pick = jigsawPickPoint(piece);
            if (!pick) {
                std::cout << "Jigsaw autoplace: piece " << piece << " has no pick point\n";
                return true;
            }
            const int tx = pick->x + (descriptor.defaultX + kBoardX - descriptor.x);
            const int ty = pick->y + (descriptor.defaultY + kBoardY - descriptor.y);
            std::cout << "Jigsaw autoplace: piece " << piece << " drag "
                      << pick->x << ',' << pick->y << " -> " << tx << ',' << ty
                      << '\n';
            referenceTrace.emit("click_dispatch", {{"scene", "jigsaw"}, {"hotspot", hotspot}});
            pressJigsawPiece(piece, pick->x, pick->y);
            moveJigsawInteraction(tx, ty);
            dropJigsawPiece();
        }
    } else if (control.rfind("pick:", 0) == 0) {
        const int piece = std::stoi(std::string(control.substr(5)));
        if (!game.session || piece < 0 || piece >= game.grid * game.grid) return false;
        if (const auto pick = jigsawPickPoint(piece)) {
            std::cout << "Jigsaw pick: piece " << piece << " at " << pick->x
                      << ',' << pick->y << '\n';
        }
    } else if (control == "solve") {
        jigsawControlClicked(kStartControl);
    } else if (puzzlePointerCommand(control)) {
    } else if (control.rfind("snapshot:", 0) == 0) {
        puzzleSnapshot(std::string(control.substr(9)));
    } else if (control == "status") {
        std::cout << "Jigsaw status: picture " << game.picture << ", grid "
                  << game.grid << ", dragged " << game.dragged << ", complete "
                  << (jigsawComplete() ? 1 : 0) << ", solve index "
                  << game.solveIndex << '\n';
        for (int index = 0; game.session && index < game.grid * game.grid;
             ++index) {
            const auto &descriptor = jigsawPiece(index).descriptor;
            std::cout << "  piece " << index << " rotation "
                      << game.rotations[static_cast<std::size_t>(index)]
                      << " at " << descriptor.x << ',' << descriptor.y
                      << " home " << descriptor.defaultX << ','
                      << descriptor.defaultY << " size " << descriptor.width
                      << 'x' << descriptor.height << " layer "
                      << descriptor.layer << " edges "
                      << game.edges[static_cast<std::size_t>(index)] << '\n';
        }
    } else {
        throw std::runtime_error("Unknown developer Jigsaw control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
