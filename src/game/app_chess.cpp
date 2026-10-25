#include "game/app_chess.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "assets/profile.h"
#include "media/smacker.h"
#include "util/binary.h"

namespace fgp {
namespace {

using chess::Move;
using chess::Piece;

// 0x0049fc30-0x0049fc3c: the board's top-left corner and square size, used
// by the pointer-to-square conversion 0x0045f9c0.
constexpr int kBoardLeft = 188;
constexpr int kBoardTop = 137;
constexpr int kSquare = 54;
// 0x0045f6d0 moves a square's corner to the piece bitmap's origin:
// x + (0xc6 - 188), y + (0x68 - 137).
constexpr int kSpriteLeft = 0xc6;
constexpr int kSpriteTop = 0x68;
// 0x0045f8e3 and 0x00461422 lift a picked-up piece to layer 15.
constexpr int kLiftedLayer = 15;
// 0x0049d9f0: the computer's piece glides for 1 / 0.001 ms.
constexpr float kGlideRate = 0.001F;
// Data/chess/hotspots.bmp id 1 is the board.
constexpr std::uint32_t kBoardHotspot = 1U;
// Rectangles at 0x0049fc70 (title), 0x0049fc60 (side 0 captures, right)
// and 0x0049fc50 (side 1 captures, left), left/top/right/bottom.
constexpr SDL_Rect kTitleRect{287, 26, 516 - 287, 85 - 26};
constexpr std::array<SDL_Rect, 2> kCaptureRects{{
    {697, 128, 731 - 697, 463 - 128},
    {65, 128, 99 - 65, 463 - 128},
}};
// 0x0045f3c0's row order: pawn 4, bishop 1, knight 2, rook 3, queen 0, so
// the numbers line up with the queen-to-pawn icons on the backdrop.
constexpr std::array<int, 5> kCaptureRows{4, 1, 2, 3, 0};

// The piece table at 0x00503680 names each descriptor; the queens at
// 0x00504d80 are eight w_queen and then eight b_queen.
std::string spriteName(std::size_t sprite) {
    static constexpr std::array<std::string_view, 6> kNames{
        "pawn", "bishop", "knight", "rook", "queen", "king"};
    static constexpr std::array<int, 8> kBackRow{3, 2, 1, 4, 5, 1, 2, 3};
    if (sprite >= 32U) {
        return sprite < 40U ? "w_queen" : "b_queen";
    }
    const std::size_t group = sprite / 8U;
    const bool pawns = group % 2U == 0U;
    const int type = pawns ? 0 : kBackRow[sprite % 8U];
    return std::string(group < 2U ? "w_" : "b_") +
           std::string(kNames[static_cast<std::size_t>(type)]);
}

// 0x00413071-0x004130a2 keys a transparent descriptor's surface on its own
// top-left pixel.
TexturePtr keyedTexture(SDL_Renderer *renderer,
                        std::span<const std::uint8_t> data, int &width,
                        int &height) {
    SurfacePtr surface = loadBmp(data);
    const Uint32 key = pixelValue(surface.get(), 0, 0);
    if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0)
        throw std::runtime_error(SDL_GetError());
    width = surface->w;
    height = surface->h;
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

// 0x004124e0 with flags 0x101: centre a w x h text surface in a rectangle.
SDL_Point centredText(const SDL_Rect &bounds, int width, int height) {
    return {bounds.x + (bounds.w - width) / 2,
            bounds.y + (bounds.h - height) / 2};
}

// 0x0045f9c0: a pointer coordinate to a square, with C's truncating
// division, so the column left of the board still reads as 0.
int squareOf(int value, int origin) { return (value - origin) / kSquare; }

std::string replaceFormat(std::string format, const std::string &value) {
    const auto at = format.find("%s");
    if (at != std::string::npos) format.replace(at, 2, value);
    return format;
}

} // namespace

ChessScene::ChessGame ChessScene::loadChessGame(Package &package) {
    ChessGame game;
    const auto asset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Chess asset not found: " + std::string(name));
    };
    {
        // 0x0045f190 copies the backdrop's (188,138)-(617,568) into a new
        // 430x431 surface, turns it a quarter clockwise with
        // DRAW_TransformSurface mode 1 (0x00401f70 -> 0x00402060: source
        // (x, y) lands at (width - 1 - y, x)) and copies the first 429x430
        // back. So (X, Y) shows the backdrop's (Y + 50, 756 - X); column 188
        // and row 567 come from the unwritten edge of the new surface, which
        // the original shows black.
        SurfacePtr source(SDL_ConvertSurfaceFormat(
            loadBmp(asset("backdrop.bmp")).get(), SDL_PIXELFORMAT_ARGB8888, 0));
        if (!source) throw std::runtime_error(SDL_GetError());
        constexpr int kLeft = 188, kTop = 138, kWidth = 429, kHeight = 430;
        SurfacePtr turned(SDL_CreateRGBSurfaceWithFormat(
            0, kWidth, kHeight, 32, SDL_PIXELFORMAT_ARGB8888));
        if (!turned) throw std::runtime_error(SDL_GetError());
        const auto *from = static_cast<const std::uint8_t *>(source->pixels);
        auto *to = static_cast<std::uint8_t *>(turned->pixels);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int screenX = kLeft + x;
                const int screenY = kTop + y;
                std::uint32_t pixel = 0xff000000U;
                if (x != 0 && y != kHeight - 1) {
                    std::memcpy(&pixel,
                                from + (756 - screenX) * source->pitch +
                                    (screenY + 50) * 4,
                                4);
                }
                std::memcpy(to + y * turned->pitch + x * 4, &pixel, 4);
            }
        }
        game.board.texture = makeTexture(renderer, turned.get());
        game.board.width = kWidth;
        game.board.height = kHeight;
    }
    for (std::string_view side : {"w_", "b_"}) {
        for (std::string_view type :
             {"pawn", "bishop", "knight", "rook", "queen", "king"}) {
            const std::string name = std::string(side) + std::string(type);
            ChessTexture image;
            image.texture = keyedTexture(
                renderer, asset("bitmap\\" + name + ".bmp"), image.width,
                image.height);
            game.images.emplace(name, std::move(image));
        }
    }
    for (std::string_view name : {"button1_1", "button2_1", "level_1"}) {
        ChessTexture image;
        image.texture = keyedTexture(
            renderer, asset("bitmap\\" + std::string(name) + ".bmp"),
            image.width, image.height);
        game.images.emplace(std::string(name), std::move(image));
    }
    // The sound clones 0x0045f910 names "<family> <rand() % n + 1>".
    const std::array<std::pair<std::string_view, int>, 7> families{{
        {"pickup", 2}, {"down", 3}, {"fall", 4}, {"wrong", 1},
        {"click", 1}, {"wcheck", 3}, {"bcheck", 5}}};
    for (const auto &[family, count] : families) {
        for (int variant = 1; variant <= count; ++variant) {
            const std::string name =
                std::string(family) + " " + std::to_string(variant);
            game.sounds.emplace(
                name, decodeSmacker(activeSceneAnimationData(name), true, true,
                                    true));
        }
    }
    // FGP seeds rand() from time() at startup (0x0040bfc4).
    game.random.state = static_cast<std::uint32_t>(std::time(nullptr));
    game.titleText = language.get("chess", "SCENENAME", "Sjakk");
    std::cout << "Loaded executable-derived Chess: board, shared buttons, "
                 "1/2-player and Auto modes, three search depths, hint, "
                 "check/checkmate/stalemate and the car-part award\n";
    return game;
}

void ChessScene::addChessSprite(std::size_t sprite, int x, int y, int layer) {
    auto &game = *chessGame;
    ChessSprite &placed = game.sprites[sprite];
    placed.name = spriteName(sprite);
    referenceTrace.emit("animation_add", {
        {"asset", placed.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", 0}, {"conflict_mask", 0}});
    placed.x = x;
    placed.y = y;
    placed.layer = layer;
    placed.active = true;
    placed.order = game.nextOrder++;
}

void ChessScene::removeChessSprite(std::size_t sprite) {
    auto &game = *chessGame;
    ChessSprite &placed = game.sprites[sprite];
    if (!placed.active) return;
    referenceTrace.emit("animation_remove", {
        {"asset", placed.name}, {"reason", "removed"}});
    placed.active = false;
}

void ChessScene::playChessSound(std::string_view family, int variants) {
    // 0x0045f910/0x0045f930 through SCENE_PlayAnimClone 0x00412c20.
    auto &game = *chessGame;
    const int pinned = game.engine.pinnedRandom;
    const int variant = pinned >= 0 ? pinned : game.random.below(variants);
    const std::string name =
        std::string(family) + " " + std::to_string(variant + 1);
    const auto sound = game.sounds.find(name);
    if (sound == game.sounds.end()) return;
    referenceTrace.emit("animation_add", {
        {"asset", name}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    audioMixer.addEffect(sound->second, audioVolumes[0] * sceneAudioGains[0]);
    game.clones.push_back(
        {name, sceneElapsedNow() +
                   originalSmackerCompletionDuration(sound->second)});
}

void ChessScene::finishChessClones(double now) {
    auto &game = *chessGame;
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
}

// Buttons ------------------------------------------------------------------

ChessScene::ChessButton *ChessScene::chessButton(int id) {
    // 0x004065e0.
    for (auto &button : chessGame->buttons) {
        if (button.id == id) return &button;
    }
    return nullptr;
}

void ChessScene::createChessButton(int id, int group, int type, int x, int y,
                                   std::string pressedImage,
                                   std::string label) {
    // 0x00406ed0 (push) and 0x00406fb0 (radio: group, then type 2). None of
    // the chess buttons has a state-0 image, so creation adds nothing until
    // a label is set.
    ChessButton button;
    button.id = id;
    button.group = group;
    button.type = type;
    button.x = x;
    button.y = y;
    button.pressedImage = std::move(pressedImage);
    chessGame->buttons.push_back(std::move(button));
    if (!label.empty()) {
        setChessButtonLabel(chessGame->buttons.back(), std::move(label));
    }
}

SDL_Rect ChessScene::measureChessButton(ChessButton &button) {
    // 0x004069b0: the first named image is added and removed once to read
    // its size, which +0x10/+0x14 then keep.
    if (button.width == 0 && button.height == 0) {
        const auto &image = chessGame->images.at(button.pressedImage);
        if (button.state == 0) {
            referenceTrace.emit("animation_add", {
                {"asset", button.pressedImage}, {"x", -1}, {"y", -1},
                {"layer", 1}, {"hotspot", button.id}, {"conflict_mask", 0}});
            referenceTrace.emit("animation_remove", {
                {"asset", button.pressedImage}, {"reason", "removed"}});
        }
        button.width = image.width;
        button.height = image.height;
    }
    return {button.x, button.y, button.width, button.height};
}

void ChessScene::setChessButtonState(ChessButton &button, int state) {
    // 0x00406740 refreshes (0x00406760) only on a change: the old state's
    // image goes and the new one's comes, at layer 1 with the id as hotspot.
    if (button.state == state) return;
    if (button.state == 1) {
        referenceTrace.emit("animation_remove", {
            {"asset", button.pressedImage}, {"reason", "removed"}});
    }
    button.state = state;
    if (state == 1) {
        referenceTrace.emit("animation_add", {
            {"asset", button.pressedImage}, {"x", -1}, {"y", -1},
            {"layer", 1}, {"hotspot", button.id}, {"conflict_mask", 0}});
    }
}

void ChessScene::emitChessText(std::string_view text, const BitmapFont &font,
                               const SDL_Rect &bounds, int layer) {
    // 0x00407a30: a text descriptor sized to its text, placed by
    // 0x004124e0 and added unnamed.
    const SDL_Point at = centredText(bounds, textWidth(font, text), font.height);
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", at.x}, {"y", at.y}, {"layer", layer},
        {"hotspot", 0}, {"conflict_mask", 0}});
}

void ChessScene::setChessButtonLabel(ChessButton &button, std::string label) {
    // 0x00406950: replace the label descriptor (+0xc4) with one centred in
    // the button's rectangle at the button's layer + 1, in the button font
    // (Amerigo BT 14, 0x004068a0).
    if (button.labelActive) {
        referenceTrace.emit("animation_remove", {
            {"asset", ""}, {"reason", "removed"}});
        button.labelActive = false;
    }
    button.label = std::move(label);
    if (button.label.empty()) return;
    const SDL_Rect rect = measureChessButton(button);
    emitChessText(button.label, calendarFont, rect, 2);
    button.labelActive = true;
}

int ChessScene::chessPlayersMode() const {
    // 0x00460d90: 0 one player, 1 two players, 2 Auto.
    const auto &game = *chessGame;
    if (game.computer[0] != game.computer[1]) return 0;
    return game.computer[0] ? 2 : 1;
}

void ChessScene::setChessPlayers(int mode) {
    // 0x0045f250: in one-player mode the computer plays turn 0 (side 0).
    auto &game = *chessGame;
    game.computer[0] = mode != 1;
    game.computer[1] = mode == 2;
    static constexpr std::array<std::string_view, 3> kModeKeys{
        "1PLAYER", "2PLAYER", "DEMO"};
    static constexpr std::array<std::string_view, 3> kModeFallbacks{
        "1 spiller", "2 spiller", "Auto"};
    const auto index = static_cast<std::size_t>(std::clamp(mode, 0, 2));
    if (ChessButton *button = chessButton(5)) {
        setChessButtonLabel(*button, language.get("chess", kModeKeys[index],
                                                  std::string(kModeFallbacks[index])));
    }
    if (ChessButton *button = chessButton(6)) {
        setChessButtonLabel(*button,
                            mode == 2 ? language.get("chess", "START", "Start")
                                      : language.get("chess", "HINT", "Hint"));
    }
}

// Scene ----------------------------------------------------------------------

void ChessScene::initializeChess() {
    // Scene initializer 0x0045efa0, in its call order. It runs on the first
    // update so the trace follows scene_change, as GAME_EnterScene's
    // logpoint precedes the original initializer.
    auto &game = *chessGame;
    game.initialized = true;
    // 0x0045effb/0x0045f020: Help (11) and Exit (10), drawn by the shell.
    referenceTrace.emit("animation_add", {
        {"asset", "help_0"}, {"x", -1}, {"y", -1}, {"layer", 1},
        {"hotspot", 11}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {
        {"asset", "exit_0"}, {"x", -1}, {"y", -1}, {"layer", 1},
        {"hotspot", 10}, {"conflict_mask", 0}});
    createChessButton(5, 0, 1, 0xc, 0x22a, "button1_1", "");
    createChessButton(6, 0, 1, 699, 0x1f1, "button2_1", "");
    createChessButton(7, 0, 1, 700, 0x224, "button2_1",
                      language.get("chess", "RESET", "Omstart"));
    createChessButton(2, 10, 2, 0xf, 0x1f6, "level_1",
                      language.get("chess", "LETTER_EASY", "L"));
    createChessButton(3, 10, 2, 0x30, 0x1f6, "level_1",
                      language.get("chess", "LETTER_MEDIUM", "M"));
    createChessButton(4, 10, 2, 0x51, 0x1f6, "level_1",
                      language.get("chess", "LETTER_HARD", "V"));
    setChessPlayers(0);
    // 0x0045f134-0x0045f172: the title in Amerigo BT 18, then the two
    // capture panels, each added at layer 1.
    emitChessText(game.titleText, menuTitleFont, kTitleRect, 1);
    for (const SDL_Rect &rect : kCaptureRects) {
        referenceTrace.emit("animation_add", {
            {"asset", ""}, {"x", rect.x}, {"y", rect.y}, {"layer", 1},
            {"hotspot", 0}, {"conflict_mask", 0}});
    }
    resetChess();
    // 0x0045f17c-0x0045f187: Medium is selected, depth 3.
    if (ChessButton *button = chessButton(3)) setChessButtonState(*button, 1);
    game.engine.depth = 3;
}

SDL_Point ChessScene::chessSquarePosition(int x, int y) const {
    // 0x0045f6d0.
    return {kSquare * x + kSpriteLeft, kSquare * y + kSpriteTop};
}

void ChessScene::placeChessPiece(int index) {
    // 0x0045f5c0: the descriptor is removed if present, then added on its
    // square at the layer of its row.
    auto &game = *chessGame;
    const auto sprite = static_cast<std::size_t>(index);
    removeChessSprite(sprite);
    const Piece &piece = game.engine.position.pieces[sprite];
    const SDL_Point at = chessSquarePosition(piece.x, piece.y);
    addChessSprite(sprite, at.x, at.y, piece.y);
}

void ChessScene::resetChess() {
    // 0x0045f330.
    auto &game = *chessGame;
    referenceTrace.emit("minigame_state", {
        {"game", "chess"}, {"state", "reset"}, {"moves", game.moves}});
    game.search.reset();                 // 0x0045f590 ends the search thread
    game.engine.reset();
    for (int index = 0; index < 32; ++index) placeChessPiece(index);
    // 0x0045f4d0: promotion queens go.
    for (std::size_t sprite = 32; sprite < game.sprites.size(); ++sprite) {
        removeChessSprite(sprite);
    }
    game.captured = {};                  // 0x0045f3a0
    game.turn = 1;
    game.state = 0;
    game.moves = 0;
    game.dragPiece = -1;
    game.found = Move{};
    game.gliding = Move{};
    game.busyCursor = false;
    minigameBusyCursor = false;
}

void ChessScene::confirmChessReset() {
    if (chessGame) resetChess();
}

void ChessScene::askChessReset() {
    // 0x0045fab0: a game still in progress asks first, with the normal
    // cursor (0x0040ef90(4)) so the question can be answered.
    if (chessGame->state != 5) {
        showMessageWithTitle(
            language.get("interfaceh", "WARNING", "Advarsel"),
            language.get("chess", "ABORT",
                         "Er du sikker du vil avslutte pågående spill?"),
            MessageKind::ChessAbortConfirmation);
    } else {
        resetChess();
    }
}

bool ChessScene::handleChessKey(SDL_Keycode key) {
    // 0x0045fa00: Pause, Escape, P and F5 (VK 0x13, 0x1b, 0x50, 0x74) go on
    // to the shell; any other key while the computer has the wait cursor
    // (0x0040f100 == 1) asks the abort question, the only way to stop it
    // because the cursor handler drops mouse events meanwhile.
    if (!chessGame || !chessGame->initialized || !chessGame->busyCursor)
        return false;
    if (key == SDLK_PAUSE || key == SDLK_ESCAPE || key == SDLK_p ||
        key == SDLK_F5)
        return false;
    askChessReset();
    return true;
}

void ChessScene::chessControl(int id) {
    // Control event 0x116, 0x00461350.
    auto &game = *chessGame;
    switch (id) {
    case 2:
    case 3:
    case 4:
        playChessSound("click", 1);
        game.engine.depth = id;          // 0x0045f320: level + 2
        break;
    case 5:
        if (game.moves == 0) setChessPlayers((chessPlayersMode() + 1) % 3);
        break;
    case 6:
        if (game.state == 0) startChessSearch();
        break;
    case 7:
        askChessReset();
        break;
    default:
        break;
    }
}

// Moves -----------------------------------------------------------------------

void ChessScene::applyChessMove(const Move &move, bool visual) {
    // 0x004601a0 (visual), with 0x004603e0, 0x004603a0, 0x00460230 and
    // 0x00460260.
    auto &game = *chessGame;
    auto &pieces = game.engine.position.pieces;
    const int index = move.piece - 1;
    const Piece before = pieces[static_cast<std::size_t>(index)];
    referenceTrace.emit("minigame_state", {
        {"game", "chess"}, {"state", "move"}, {"piece", index},
        {"from_x", before.x}, {"from_y", before.y}, {"cell_x", move.x},
        {"cell_y", move.y}, {"turn", game.turn}, {"moves", game.moves}});
    if (visual) game.engine.record(move);
    if (move.captured != 0) {
        const Piece &taken = pieces[static_cast<std::size_t>(move.captured - 1)];
        if (visual) {
            removeChessSprite(static_cast<std::size_t>(taken.descriptor));
            playChessSound("fall", 4);
            ++game.captured[static_cast<std::size_t>(taken.side)]
                           [static_cast<std::size_t>(taken.type)];
        }
    } else if (visual) {
        playChessSound("down", 3);
    }
    const bool promotes = before.type == chess::Pawn &&
                          chess::rankOf(before.side, move.y) == 7;
    if (promotes && visual) {
        // 0x004602e6-0x00460300: the pawn's descriptor goes and the queen
        // descriptor for its slot (index - side * 8) is added unplaced.
        removeChessSprite(static_cast<std::size_t>(before.descriptor));
        const int queen = 32 + index - before.side * 8;
        pieces[static_cast<std::size_t>(index)].descriptor = queen;
        addChessSprite(static_cast<std::size_t>(queen), -1, -1, 0);
    }
    game.engine.apply(move);
    if (visual) {
        const Piece &moved = pieces[static_cast<std::size_t>(index)];
        ChessSprite &sprite =
            game.sprites[static_cast<std::size_t>(moved.descriptor)];
        const SDL_Point at = chessSquarePosition(move.x, move.y);
        sprite.x = at.x;
        sprite.y = at.y;
        sprite.layer = move.y;
    }
}

void ChessScene::endChessTurn() {
    // 0x0045fd00 with 0x0045fda0.
    auto &game = *chessGame;
    const int previous = game.turn;
    ++game.moves;
    game.turn ^= 1;
    const int king = chess::kKingIndex[static_cast<std::size_t>(game.turn)];
    const bool check = game.engine.isAttacked(king);
    Move any;
    game.engine.search(1, game.turn == 0, game.random, &any);
    if (any.piece == 0) {
        finishChessGame(previous, check);
        return;
    }
    if (check) {
        if (previous == 0) playChessSound("wcheck", 3);
        else playChessSound("bcheck", 5);
    }
    if (!game.computer[static_cast<std::size_t>(game.turn)]) {
        game.state = 0;
    } else {
        startChessSearch();
    }
}

void ChessScene::startChessSearch() {
    // 0x00460dc0: state 2, the wait cursor, and a new thread (0x00460e00)
    // that searches the side to move. A new thread starts the C runtime's
    // per-thread rand() at 1 (srand 0x0048d316 writes the per-thread block).
    auto &game = *chessGame;
    referenceTrace.emit("minigame_state", {
        {"game", "chess"}, {"state", "think"}, {"turn", game.turn},
        {"level", game.engine.depth}, {"moves", game.moves}});
    game.state = 2;
    game.busyCursor = true;
    minigameBusyCursor = true;
    auto search = std::make_shared<ChessSearch>();
    search->engine = game.engine;
    search->maximizing = game.turn == 0;
    game.search = search;
    std::thread([search] {
        chess::Random random;
        Move best;
        search->engine.search(search->engine.depth, search->maximizing, random,
                              &best);
        search->result = best;
        search->done.store(true);
    }).detach();
}

void ChessScene::takeChessSearchResult() {
    // 0x00461400 state 3, 0x00461450: take the found move (searching one ply
    // on this thread if none was found), lift the piece and start the glide.
    auto &game = *chessGame;
    game.found = game.search->result;
    game.search.reset();
    if (game.found.piece == 0) {
        game.engine.search(1, game.turn == 0, game.random, &game.found);
    }
    game.gliding = game.found;
    const Piece &piece =
        game.engine.position.pieces[static_cast<std::size_t>(game.gliding.piece - 1)];
    game.sprites[static_cast<std::size_t>(piece.descriptor)].layer = kLiftedLayer;
    playChessSound("pickup", 2);
    game.glideStart = sceneElapsedNow();
    game.state = 4;
}

void ChessScene::glideChessMove(double now) {
    // 0x00461510: from the piece's square towards the target, for one second
    // of the millisecond clock 0x0040c450, truncated by _ftol 0x0048c214.
    auto &game = *chessGame;
    const Move move = game.gliding;
    const Piece &piece =
        game.engine.position.pieces[static_cast<std::size_t>(move.piece - 1)];
    const SDL_Point from = chessSquarePosition(piece.x, piece.y);
    const SDL_Point to = chessSquarePosition(move.x, move.y);
    const int milliseconds =
        static_cast<int>((now - game.glideStart) * 1000.0);
    const float progress = static_cast<float>(milliseconds) * kGlideRate;
    ChessSprite &sprite = game.sprites[static_cast<std::size_t>(piece.descriptor)];
    sprite.x = from.x + static_cast<int>(static_cast<double>(to.x - from.x) *
                                         static_cast<double>(progress));
    sprite.y = from.y + static_cast<int>(static_cast<double>(to.y - from.y) *
                                         static_cast<double>(progress));
    if (progress >= 1.0F) {
        applyChessMove(move, true);
        game.busyCursor = false;
        minigameBusyCursor = false;
        endChessTurn();
    }
}

void ChessScene::finishChessGame(int winnerTurn, bool checkmate) {
    // 0x00460c40: the side that just moved wins; stalemate names it too.
    auto &game = *chessGame;
    referenceTrace.emit("minigame_state", {
        {"game", "chess"}, {"state", "game_over"}, {"turn", winnerTurn},
        {"score", checkmate ? 1 : 0}, {"moves", game.moves}});
    const std::string side =
        winnerTurn == 0 ? language.get("chess", "BLACK", "Svart")
                        : language.get("chess", "WHITE", "Hvit");
    const std::string text = replaceFormat(
        checkmate ? language.get("chess", "CHECKMATE", "Sjakk matt: %s spiller vant.")
                  : language.get("chess", "STALEMATE", "Patt: %s spiller vant."),
        side);
    game.busyCursor = false;
    minigameBusyCursor = false;
    showMessageWithTitle(language.get("interfaceh", "GAMEOVER", "Spillet er over"),
                         text, MessageKind::Notice);
    // 0x00460d00: a one-player checkmate by the human raises the profile's
    // record (type 0x402, key 0) to level + 1, awarding the difference.
    if (checkmate && chessPlayersMode() == 0 &&
        !game.computer[static_cast<std::size_t>(winnerTurn)] &&
        !profiles.empty()) {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        try {
            const std::uint32_t stored =
                profileSceneRecordU32(profilePath, "chess", 0x402U, 0U)
                    .value_or(0U);
            const int target = game.engine.depth - 1;
            if (static_cast<int>(stored) < target) {
                game.awardPending = true;
                game.awardLevel = game.engine.depth - 2;
                game.awardCount = target - static_cast<int>(stored);
                setProfileSceneRecordU32(profilePath, "chess", 0x402U, 0U,
                                         static_cast<std::uint32_t>(target));
            }
        } catch (const std::exception &error) {
            std::cout << "Could not record the chess level: " << error.what()
                      << '\n';
        }
    }
    game.state = 5;
}

void ChessScene::snapChessPieceBack(int index) {
    // 0x0040c240 returns the descriptor; 0x00465f70 restores its layer.
    auto &game = *chessGame;
    const Piece &piece = game.engine.position.pieces[static_cast<std::size_t>(index)];
    ChessSprite &sprite = game.sprites[static_cast<std::size_t>(piece.descriptor)];
    sprite.x = game.dragHomeX;
    sprite.y = game.dragHomeY;
    sprite.layer = piece.y;
}

void ChessScene::dropChessPiece(std::uint32_t hotspot, int x, int y) {
    // Drag end 0x0045fb10.
    auto &game = *chessGame;
    const int index = game.dragPiece;
    game.dragPiece = -1;
    const Piece &piece = game.engine.position.pieces[static_cast<std::size_t>(index)];
    if (hotspot != kBoardHotspot) {
        snapChessPieceBack(index);
        return;
    }
    const int squareX = squareOf(x, kBoardLeft);
    const int squareY = squareOf(y, kBoardTop);
    if (squareX == piece.x && squareY == piece.y) {
        snapChessPieceBack(index);
        return;
    }
    if (squareX < 0 || squareX > 7 || squareY < 0 || squareY > 7) {
        snapChessPieceBack(index);
        return;
    }
    const int target = game.engine.indexAt(squareX, squareY);
    if (!game.engine.isLegalHumanMove(index, squareX, squareY, target)) {
        snapChessPieceBack(index);
        playChessSound("wrong", 1);
        return;
    }
    applyChessMove(Move{index + 1, squareX, squareY, target}, true);
    endChessTurn();
}

// Input -----------------------------------------------------------------------

bool ChessScene::beginChessInteraction(int x, int y) {
    if (!chessGame || !chessGame->initialized) return false;
    auto &game = *chessGame;
    // The cursor module's handler 0x0040f150 runs first (priority 100) and
    // keeps mouse events 0x105-0x109 while the cursor mode 0x0054c248 is 0
    // or 1 (0x0040f210), so nothing is pressed while the computer moves.
    if (game.busyCursor) return true;
    const std::uint32_t hotspot = hotspotAt(x, y);
    // Mouse-down 0x0045f7c0, then 0x0045f7e0 on the board.
    if (hotspot == kBoardHotspot && game.state == 0) {
        const int squareX = squareOf(x, kBoardLeft);
        const int squareY = squareOf(y, kBoardTop);
        const int index = squareX >= 0 && squareX <= 7 && squareY >= 0 &&
                                  squareY <= 7
                              ? game.engine.indexAt(squareX, squareY) - 1
                              : -1;
        if (index >= 0 &&
            game.engine.position.pieces[static_cast<std::size_t>(index)].side ==
                game.turn) {
            playChessSound("pickup", 2);
            const Piece &piece =
                game.engine.position.pieces[static_cast<std::size_t>(index)];
            ChessSprite &sprite =
                game.sprites[static_cast<std::size_t>(piece.descriptor)];
            sprite.layer = kLiftedLayer;
            game.dragPiece = index;
            game.dragMouseX = x;
            game.dragMouseY = y;
            game.dragHomeX = sprite.x;
            game.dragHomeY = sprite.y;
        }
    }
    // 0x0045ef02: once a move is made the scene keeps the level buttons'
    // presses (returning 1 stops the dispatch at 0x0040ece8).
    if (game.moves != 0 && hotspot >= 2U && hotspot <= 4U) return true;
    // Shared button handler 0x00406b50 (priority -10, after the scene).
    for (auto &button : game.buttons) {
        const SDL_Rect rect = measureChessButton(button);
        const SDL_Point point{x, y};
        if (!SDL_PointInRect(&point, &rect) ||
            static_cast<std::uint32_t>(button.id) != hotspot) {
            continue;
        }
        if (button.type == 1) {
            // 0x00406c50: pressed until the release posts 0x116.
            if (button.state != 0) continue;
            setChessButtonState(button, 1);
            game.pressedButton = button.id;
            return true;
        }
        if (button.type == 2) {
            // 0x00406c80: clear the group, select, post 0x116 at once.
            if (button.state != 0) continue;
            for (auto &other : game.buttons) {
                if (other.group == button.group) setChessButtonState(other, 0);
            }
            setChessButtonState(button, 1);
            chessControl(button.id);
            return true;
        }
    }
    return true;
}

void ChessScene::moveChessInteraction(int x, int y) {
    // 0x0040c300: the dragged descriptor follows the pointer's movement.
    if (!chessGame || chessGame->dragPiece < 0) return;
    auto &game = *chessGame;
    const Piece &piece =
        game.engine.position.pieces[static_cast<std::size_t>(game.dragPiece)];
    ChessSprite &sprite = game.sprites[static_cast<std::size_t>(piece.descriptor)];
    sprite.x += x - game.dragMouseX;
    sprite.y += y - game.dragMouseY;
    game.dragMouseX = x;
    game.dragMouseY = y;
}

void ChessScene::endChessInteraction(int x, int y) {
    if (!chessGame) return;
    auto &game = *chessGame;
    if (game.busyCursor) return;           // 0x0040f150, as above
    if (game.pressedButton != 0) {
        // 0x00406cf0/0x00406d40: the pressed push button resets and posts
        // 0x116 wherever the pointer is released.
        const int id = game.pressedButton;
        game.pressedButton = 0;
        if (ChessButton *button = chessButton(id)) {
            setChessButtonState(*button, 0);
            chessControl(id);
        }
    }
    if (game.dragPiece >= 0) {
        moveChessInteraction(x, y);
        dropChessPiece(hotspotAt(x, y), x, y);
    }
}

// Update and render -----------------------------------------------------------

void ChessScene::updateChess(double) {
    auto &game = *chessGame;
    if (!game.initialized) {
        initializeChess();
        return;
    }
    // A modal dialog holds the original's main loop.
    if (dialogMode != DialogMode::None) return;
    if (game.awardPending) {
        // 0x00460d00 runs as soon as the Game Over box returns, before any
        // further tick, so the award opens before anything else moves.
        // 0x0041c0b0 -> 0x0041bb20 -> 0x0041b900: 0x0041b830 grants the
        // next awardCount car parts not yet owned (category 0, 62 parts),
        // and 0x0041bce0 shows one part_%02d icon for each.
        // UNCERTAIN: where 0x0041bce0 places a second and third icon; native
        // shows the first part. With all 62 owned nothing is granted and
        // 0x0041b9d0 tries the category-6 table at 0x004ad8c8 instead; its
        // first index has no entry, so the original shows no dialog (as the
        // capture with a complete car showed), but later indexes can grant
        // other awards that native does not port.
        game.awardPending = false;
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        try {
            const auto bytes = readBinaryFile(profilePath);
            const std::uint32_t owned =
                bytes.size() >= 0x5cU ? readLe32(bytes, 0x58) : 62U;
            std::uint32_t first = 0;
            for (int count = 0; count < game.awardCount && owned + count < 62U;
                 ++count) {
                const std::uint32_t part = awardMedalCarPart(profilePath);
                if (count == 0) first = part;
            }
            if (first != 0) {
                static constexpr std::array<std::string_view, 3> kTitles{
                    "TITLE_EASY", "TITLE_MEDIUM", "TITLE_HARD"};
                static constexpr std::array<std::string_view, 3> kDescriptions{
                    "DESC_EASY", "DESC_MEDIUM", "DESC_HARD"};
                const auto level =
                    static_cast<std::size_t>(std::clamp(game.awardLevel, 0, 2));
                std::string number = std::to_string(first);
                if (number.size() < 2) number.insert(number.begin(), '0');
                showAward("bitmap\\award\\part_" + number + ".bmp",
                          kTitles[level], kDescriptions[level]);
            }
        } catch (const std::exception &error) {
            std::cout << "Could not award the chess car part: " << error.what()
                      << '\n';
        }
        return;
    }
    const double now = sceneElapsedNow();
    // Tick 0x112, 0x00461400.
    if (game.state == 3) {
        takeChessSearchResult();
    } else if (game.state == 4) {
        glideChessMove(now);
    }
    // The search thread sets state 3 (0x00460e62) when it finishes.
    if (chessGame && game.state == 2 && game.search && game.search->done.load()) {
        game.state = 3;
    }
    if (!chessGame) return;
    finishChessClones(now);
}

void ChessScene::renderChess(double) {
    auto &game = *chessGame;
    {
        SDL_Rect board{188, 138, game.board.width, game.board.height};
        SDL_RenderCopy(renderer, game.board.texture.get(), nullptr, &board);
    }
    if (!game.initialized) return;
    const auto draw = [&](std::string_view name, int x, int y) {
        const auto image = game.images.find(name);
        if (image == game.images.end()) return;
        SDL_Rect destination{x, y, image->second.width, image->second.height};
        SDL_RenderCopy(renderer, image->second.texture.get(), nullptr,
                       &destination);
    };
    // Title and capture counts (layer 1), drawn with 0x00401e20's "%d".
    const auto text = [&](const BitmapFont &font, std::string_view value,
                          const SDL_Rect &bounds) {
        const SDL_Point at = centredText(bounds, textWidth(font, value),
                                         font.height);
        drawText(renderer, font, value, at.x, at.y);
    };
    text(menuTitleFont, game.titleText, kTitleRect);
    for (std::size_t side = 0; side < 2; ++side) {
        const SDL_Rect &panel = kCaptureRects[side];
        const int row = panel.h / 5;
        for (std::size_t type = 0; type < 5; ++type) {
            const SDL_Rect cell{panel.x, panel.y + kCaptureRows[type] * row,
                                panel.w, row};
            text(calendarFont, std::to_string(game.captured[side][type]), cell);
        }
    }
    // Buttons: the state image at layer 1, the label at layer 2.
    for (const auto &button : game.buttons) {
        if (button.state == 1) draw(button.pressedImage, button.x, button.y);
    }
    for (const auto &button : game.buttons) {
        if (!button.labelActive) continue;
        text(calendarFont, button.label,
             SDL_Rect{button.x, button.y, button.width, button.height});
    }
    // Pieces by layer: rows from the top, a lifted piece above them all.
    std::vector<const ChessSprite *> order;
    for (const auto &sprite : game.sprites) {
        if (sprite.active) order.push_back(&sprite);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const ChessSprite *left, const ChessSprite *right) {
                         if (left->layer != right->layer)
                             return left->layer < right->layer;
                         return left->order < right->order;
                     });
    for (const ChessSprite *sprite : order) {
        if (sprite->x == -1 && sprite->y == -1) continue;
        draw(sprite->name, sprite->x, sprite->y);
    }
}

// Developer commands ------------------------------------------------------------

bool ChessScene::executeChessDeveloperCommand(std::string_view command) {
    if (!chessGame) return false;
    auto &game = *chessGame;
    const auto value = [&](std::string_view prefix) -> std::optional<long> {
        if (command.rfind(prefix, 0) != 0) return std::nullopt;
        return std::stol(std::string(command.substr(prefix.size())));
    };
    if (const auto seed = value("chess:seed:")) {
        // Mirrors the capture fixture that seeds the main thread's rand()
        // at the first call after the initializer 0x0045efa0.
        game.random.state = static_cast<std::uint32_t>(*seed);
        return true;
    }
    if (const auto pinned = value("chess:random:")) {
        game.engine.pinnedRandom = static_cast<int>(*pinned);
        return true;
    }
    if (const auto depth = value("chess:level:")) {
        game.engine.depth = static_cast<int>(*depth) + 2;
        return true;
    }
    if (command.rfind("chess:move:", 0) == 0) {
        // chess:move:fx,fy,tx,ty drags a piece from square to square.
        int fx = 0, fy = 0, tx = 0, ty = 0;
        if (std::sscanf(std::string(command.substr(11)).c_str(), "%d,%d,%d,%d",
                        &fx, &fy, &tx, &ty) != 4)
            return false;
        const int px = kBoardLeft + kSquare * fx + kSquare / 2;
        const int py = kBoardTop + kSquare * fy + kSquare / 2;
        beginChessInteraction(px, py);
        if (game.dragPiece < 0) return false;
        endChessInteraction(kBoardLeft + kSquare * tx + kSquare / 2,
                            kBoardTop + kSquare * ty + kSquare / 2);
        return true;
    }
    if (command.rfind("chess:control:", 0) == 0) {
        chessControl(std::stoi(std::string(command.substr(14))));
        return true;
    }
    if (command == "chess:state") {
        std::cout << "Chess: turn " << game.turn << " state " << game.state
                  << " moves " << game.moves << " depth " << game.engine.depth
                  << '\n';
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const Piece *piece = game.engine.pieceAt(x, y);
                static constexpr std::string_view kLetters = "pbnrqk";
                char letter = '.';
                if (piece != nullptr) {
                    letter = kLetters[static_cast<std::size_t>(piece->type)];
                    if (piece->side == 1) letter = static_cast<char>(letter - 32);
                }
                std::cout << letter;
            }
            std::cout << '\n';
        }
        return true;
    }
    return false;
}

} // namespace fgp
