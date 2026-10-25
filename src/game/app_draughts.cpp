#include "game/app_draughts.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

using draughts::Move;

// Controls 0x00464070 creates: ids, positions and pressed art.
constexpr int kControlEasy = 2;
constexpr int kControlMode = 5;
constexpr int kControlHint = 6;
constexpr int kControlReset = 7;
// hotspots.bmp ids: 1 board, 2..4 radios, 5..7 buttons.
constexpr std::uint32_t kBoardHotspot = 1U;
constexpr SDL_Point kModeButton{12, 554};
constexpr SDL_Point kHintButton{699, 497};
constexpr SDL_Point kResetButton{700, 548};
constexpr std::array<SDL_Point, 3> kLevelRadios{{{15, 502}, {48, 502},
                                                 {81, 502}}};
// Text rectangles 0x0049fd18 (title, Amerigo BT_18_), 0x0049fd28 and
// 0x0049fd38 (captured counters, Amerigo BT_14_, flags 0x101).
constexpr SDL_Rect kTitleText{246, 8, 579 - 246, 55 - 8};
constexpr SDL_Rect kWhiteLostText{29, 231, 87 - 29, 273 - 231};
constexpr SDL_Rect kBlackLostText{722, 231, 780 - 722, 273 - 231};
// 0x0049fd10: animation steps per square of a computer move.
constexpr int kStepsPerMove = 50;
// The 0x112 update runs once per idle pump pass, a runtime rate. On the
// reference host the computer's 50-step slide takes 78-94 ms (GetTickCount
// resolution), so about 1.7 ms per update.
constexpr double kTickSeconds = 0.0017;
// 0x004648ae / 0x00466076: a lifted piece is drawn on layer 15.
constexpr int kLiftedLayer = 15;

constexpr std::array<std::string_view, 3> kModeKeys{{"1PLAYER", "2PLAYER",
                                                     "DEMO"}};
constexpr std::array<std::string_view, 3> kLevelKeys{{
    "LETTER_EASY", "LETTER_MEDIUM", "LETTER_HARD"}};

std::string pieceAsset(int index, bool king) {
    if (king) return index < 12 ? "w_king" : "b_king";
    return index < 12 ? "w_pawn" : "b_pawn";
}

} // namespace

DraughtsScene::DraughtsGame DraughtsScene::loadDraughtsGame(Package &package) {
    DraughtsGame game;
    const auto texture = [&](std::string_view name) {
        const std::string asset = "bitmap\\" + std::string(name) + ".bmp";
        SurfacePtr surface = loadGreenKeyedBmp(package.data(package.find(asset)));
        TexturePtr result = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(result.get(), SDL_BLENDMODE_BLEND);
        return result;
    };
    game.whitePawn = texture("w_pawn");
    game.blackPawn = texture("b_pawn");
    game.whiteKing = texture("w_king");
    game.blackKing = texture("b_king");
    game.levelPressed = texture("level_1");
    game.button1Pressed = texture("button1_1");
    game.button2Pressed = texture("button2_1");
    for (std::string_view name : {"click 1", "down 1", "down 2", "down 3",
                                  "fall 1", "fall 2", "fall 3", "fall 4",
                                  "pickup 1", "pickup 2", "wrong 1"}) {
        game.sounds.emplace_back(
            std::string(name),
            decodeSmacker(activeSceneAnimationData(std::string(name)), true,
                          true, true));
    }
    std::cout << "Loaded executable-derived Draughts: 1/2-player and Auto "
                 "modes, three search depths, mandatory captures, "
                 "multi-jumps and crowning\n";
    return game;
}

int DraughtsScene::draughtsMode() const {
    // 0x00465150: 0 one player, 1 two players, 2 computer only.
    const auto &players = draughtsGame->players;
    if (players[0] != players[1]) return 0;
    return players[0] == 0 ? 1 : 2;
}

void DraughtsScene::setDraughtsMode(int mode) {
    // 0x00464270.
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "mode"}, {"mode", mode}});
    auto &players = draughtsGame->players;
    if (mode == 0) {
        players = {1, 0};
    } else if (mode == 1) {
        players = {0, 0};
    } else if (mode == 2) {
        players = {1, 1};
    }
}

void DraughtsScene::setDraughtsLevel(int level) {
    // 0x00464340: search depth 2 * level + 3.
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "level"}, {"level", level}});
    draughtsGame->level = level;
    draughtsGame->depth = level * 2 + 3;
}

void DraughtsScene::addDraughtsPieceArt(int piece, bool king, int x,
                                        int y) {
    // SCENE_AddAnim 0x00412840 on layer 0.
    auto &game = *draughtsGame;
    auto &art = game.art[static_cast<std::size_t>(piece)];
    (king ? art.kingAdded : art.pawnAdded) = true;
    art.layer = 0;
    art.order = game.nextOrder++;
    referenceTrace.emit("animation_add", {
        {"asset", pieceAsset(piece, king)}, {"x", x}, {"y", y}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
}

void DraughtsScene::removeDraughtsPieceArt(int piece, bool king) {
    // SCENE_RemoveAnim 0x00412aa0.
    auto &art = draughtsGame->art[static_cast<std::size_t>(piece)];
    bool &added = king ? art.kingAdded : art.pawnAdded;
    if (!added) return;
    added = false;
    referenceTrace.emit("animation_remove", {
        {"asset", pieceAsset(piece, king)}, {"reason", "removed"}});
}

void DraughtsScene::placeDraughtsPiece(int piece) {
    auto &game = *draughtsGame;
    const auto &record = game.board.pieces[static_cast<std::size_t>(piece)];
    const draughts::Point origin = draughts::squareOrigin(record.col, record.row);
    auto &art = game.art[static_cast<std::size_t>(piece)];
    art.x = origin.x;
    art.y = origin.y;
}

void DraughtsScene::setDraughtsLayer(int piece, int layer) {
    // 0x00412d50 re-sorts the descriptor.
    auto &game = *draughtsGame;
    auto &art = game.art[static_cast<std::size_t>(piece)];
    art.layer = layer;
    art.order = game.nextOrder++;
}

void DraughtsScene::resetDraughts() {
    // 0x00464350, after 0x00464540 has stopped any search.
    auto &game = *draughtsGame;
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "reset"}, {"moves", draughtsMoves}});
    game.board = draughts::initialBoard();
    for (int piece = 0; piece < 24; ++piece) {
        // 0x00464570: remove the pawn if it is shown, then add it at its
        // square.
        removeDraughtsPieceArt(piece, false);
        const auto &record = game.board.pieces[static_cast<std::size_t>(piece)];
        const draughts::Point origin = draughts::squareOrigin(record.col, record.row);
        addDraughtsPieceArt(piece, false, origin.x, origin.y);
        placeDraughtsPiece(piece);
    }
    // 0x00464480: then every king still in the scene goes.
    for (int piece = 0; piece < 24; ++piece) {
        removeDraughtsPieceArt(piece, true);
    }
    game.state = DraughtsState::Waiting;
    game.turn = 1;
    game.plan.clear();
    game.animationStep = 0;
    game.jumpingPiece = -1;
    game.dragPiece = -1;
    draughtsMoves = 0;
}

void DraughtsScene::initializeDraughts() {
    // 0x00464070 after the fonts and controls: mode 0 (0x00464209), the
    // board (0x00464256), radio 3 selected and level 1 (0x00464266).
    draughtsGame->initialized = true;
    setDraughtsMode(0);
    resetDraughts();
    setDraughtsLevel(1);
}

void DraughtsScene::playDraughtsSound(std::string_view name, int variants) {
    // 0x004648f0: "<name> <rand() % n + 1>" through SCENE_PlayAnimClone.
    auto &game = *draughtsGame;
    const int variant =
        game.soundVariant
            ? std::min(*game.soundVariant, variants)
            : std::uniform_int_distribution<int>(1, variants)(randomEngine);
    const std::string asset = std::string(name) + " " + std::to_string(variant);
    const auto sound = std::find_if(
        game.sounds.begin(), game.sounds.end(),
        [&](const auto &entry) { return entry.first == asset; });
    if (sound == game.sounds.end()) return;
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", 0}, {"y", 0}, {"layer", 0}, {"hotspot", 0},
        {"conflict_mask", 0}});
    audioMixer.addEffect(sound->second, audioVolumes[0] * sceneAudioGains[0]);
    game.cloneSounds.push_back(
        {asset, sceneElapsedNow() +
                    originalSmackerCompletionDuration(sound->second)});
}

bool DraughtsScene::playDraughtsMove(const Move &move) {
    // 0x00464d50 with the record flag set.
    auto &game = *draughtsGame;
    const auto &piece = game.board.pieces[static_cast<std::size_t>(move.piece)];
    const int capturedIndex =
        move.captured >= 0 ? move.captured : -1;
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "move"}, {"piece", move.piece},
        {"kind", piece.type}, {"side", piece.side}, {"from_col", piece.col},
        {"from_row", piece.row}, {"to_col", move.col}, {"to_row", move.row},
        {"captured", capturedIndex}, {"moves", draughtsMoves}});
    draughts::recordHistory(draughtsHistory, game.board, move);
    if (move.captured >= 0) {
        // 0x00464f20: the captured piece's current descriptor goes.
        const auto &captured =
            game.board.pieces[static_cast<std::size_t>(move.captured)];
        removeDraughtsPieceArt(move.captured, captured.type == 1);
    }
    const bool wasMan = piece.type == 0;
    const bool notCrowned = draughts::applyMove(game.board, move);
    if (move.captured >= 0) {
        playDraughtsSound("fall", 4);
    } else {
        playDraughtsSound("down", 3);
    }
    if (wasMan && !notCrowned) {
        // 0x00464e53: swap the pawn for this piece's king descriptor, added
        // at (-1,-1) and then moved to the square.
        removeDraughtsPieceArt(move.piece, false);
        addDraughtsPieceArt(move.piece, true, -1, -1);
    }
    placeDraughtsPiece(move.piece);
    return notCrowned;
}

void DraughtsScene::endDraughtsTurn() {
    // 0x00464cc0.
    auto &game = *draughtsGame;
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "turn_end"}, {"turn", game.turn},
        {"moves", draughtsMoves}});
    ++draughtsMoves;
    game.jumpingPiece = -1;
    game.animationStep = 0;
    game.plan.clear();
    game.turn = (game.turn + 1) % 2;
    const int side = game.turn;
    if (draughts::movablePieces(game.board, side) == 0) {
        // 0x00465010 names draughts:WHITE for side 0 and BLACK otherwise.
        // Side 0 is the dark art, so the name is the colour on screen of the
        // side that can still move: the winner.
        const int pieces = draughts::alivePieces(game.board, side);
        referenceTrace.emit("minigame_state", {
            {"game", "draughts"}, {"state", "game_over"}, {"side", side},
            {"pieces", pieces}});
        game.waitCursor = false;
        game.gameOverSide = side;
        game.gameOverPending = true;
        char message[512]{};
        const std::string format =
            language.get("draughts", "ENDMESSAGE", "%s spiller vant.");
        const std::string colour = language.get(
            "draughts", side == 0 ? "WHITE" : "BLACK",
            side == 0 ? "Hvit" : "Svart");
        std::snprintf(message, sizeof(message), format.c_str(), colour.c_str());
        showMessageWithTitle(
            language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
            message, MessageKind::DraughtsGameOver);
        return;
    }
    if (game.players[static_cast<std::size_t>(side)] == 0) {
        game.state = DraughtsState::Waiting;
    } else {
        startDraughtsThinking();
    }
}

void DraughtsScene::finishDraughtsGameOver() {
    // 0x004650a2-0x004650ab after the modal returns: the award check
    // 0x004650c0, then state 5.
    if (!draughtsGame) return;
    auto &game = *draughtsGame;
    game.gameOverPending = false;
    // UNCERTAIN: 0x004650c0 grants a difficulty award through 0x0041c0b0
    // when the computer (side 0) loses a one-player game at a level above
    // the stored record type 0x402 key 0. That path is not ported.
    game.state = DraughtsState::GameOver;
}

void DraughtsScene::startDraughtsThinking() {
    // 0x00465180: wait cursor, then the search thread. Native searches at
    // once; the result is consumed by the next 0x112 update, as the
    // thread's state 3 is.
    auto &game = *draughtsGame;
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "think"}, {"turn", game.turn},
        {"depth", game.depth}});
    game.state = DraughtsState::Thinking;
    game.waitCursor = true;
    game.plan = draughts::computeMoves(game.board, draughtsHistory, game.turn,
                                       game.depth);
    game.state = DraughtsState::MoveReady;
}

void DraughtsScene::tickDraughts() {
    // 0x00466060.
    auto &game = *draughtsGame;
    if (game.state == DraughtsState::MoveReady) {
        if (game.plan.empty()) {
            game.state = DraughtsState::Waiting;
            return;
        }
        setDraughtsLayer(game.plan.front().piece, kLiftedLayer);
        playDraughtsSound("pickup", 2);
        game.animationStep = 0;
        game.state = DraughtsState::Animating;
        return;
    }
    if (game.state != DraughtsState::Animating) return;
    // 0x00466170.
    const int index = game.animationStep / kStepsPerMove;
    const int step = game.animationStep % kStepsPerMove;
    const int count = static_cast<int>(game.plan.size());
    const auto finish = [&](int piece) {
        game.waitCursor = false;
        setDraughtsLayer(
            piece, game.board.pieces[static_cast<std::size_t>(piece)].row);
        endDraughtsTurn();
    };
    if (index >= count) {
        finish(game.plan.back().piece);
        return;
    }
    const Move move = game.plan[static_cast<std::size_t>(index)];
    const auto &piece = game.board.pieces[static_cast<std::size_t>(move.piece)];
    const draughts::Point origin = draughts::squareOrigin(piece.col, piece.row);
    auto &art = game.art[static_cast<std::size_t>(move.piece)];
    art.x = origin.x + (move.col - piece.col) * step;
    art.y = origin.y + (move.row - piece.row) * step;
    if (step == kStepsPerMove - 1 && !playDraughtsMove(move)) {
        finish(move.piece);
        return;
    }
    ++game.animationStep;
}

void DraughtsScene::updateDraughts(double elapsed) {
    (void)elapsed;
    auto &game = *draughtsGame;
    if (!game.initialized) initializeDraughts();
    const double now = sceneElapsedNow();
    if (game.nextTick == 0.0) game.nextTick = now;
    // The modal message box blocks the original's loop, ticks included.
    while (dialogMode == DialogMode::None && game.nextTick <= now) {
        game.nextTick += kTickSeconds;
        tickDraughts();
        if (!draughtsGame) return;
    }
    if (dialogMode != DialogMode::None) {
        // The modal boxes (0x00421310) freeze the descriptor updates too:
        // clones that end meanwhile are retired in the first pass after the
        // box closes, newest first (the Game Over capture removes fall,
        // pickup, down from clone slots 2, 1, 0).
        game.nextTick = now;
        game.clonesHeld = true;
        return;
    }
    const auto retire = [&](const DraughtsCloneSound &sound) {
        referenceTrace.emit("animation_stop", {
            {"asset", sound.asset}, {"reason", "stopped"}});
        referenceTrace.emit("animation_remove", {
            {"asset", sound.asset}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", sound.asset}, {"reason", "stopped"}});
    };
    if (game.clonesHeld) {
        game.clonesHeld = false;
        for (auto sound = game.cloneSounds.rbegin();
             sound != game.cloneSounds.rend(); ++sound) {
            if (now >= sound->endsAt) retire(*sound);
        }
        std::erase_if(game.cloneSounds, [now](const DraughtsCloneSound &sound) {
            return now >= sound.endsAt;
        });
        return;
    }
    // Otherwise each clone ends in its own update pass, in time order.
    for (;;) {
        const auto first = std::min_element(
            game.cloneSounds.begin(), game.cloneSounds.end(),
            [](const DraughtsCloneSound &left, const DraughtsCloneSound &right) {
                return left.endsAt < right.endsAt;
            });
        if (first == game.cloneSounds.end() || now < first->endsAt) break;
        retire(*first);
        game.cloneSounds.erase(first);
    }
}

void DraughtsScene::controlDraughts(int id) {
    // Control event 0x116, 0x00465fb0.
    auto &game = *draughtsGame;
    if (id >= kControlEasy && id < kControlEasy + 3) {
        playDraughtsSound("click", 1);
        setDraughtsLevel(id - kControlEasy);
    } else if (id == kControlMode) {
        if (draughtsMoves == 0) setDraughtsMode((draughtsMode() + 1) % 3);
    } else if (id == kControlHint) {
        if (game.state == DraughtsState::Waiting) startDraughtsThinking();
    } else if (id == kControlReset) {
        requestDraughtsReset();
    }
}

void DraughtsScene::requestDraughtsReset() {
    // 0x00464a80: confirm unless the game is over.
    auto &game = *draughtsGame;
    if (game.state == DraughtsState::GameOver) {
        resetDraughts();
        return;
    }
    game.abortPending = true;
    showMessageWithTitle(
        language.get("interfaceh", "WARNING", "Advarsel"),
        language.get("draughts", "ABORT",
                     "Er du sikker du vil avslutte pågående spill?"),
        MessageKind::DraughtsAbortConfirmation);
}

void DraughtsScene::finishDraughtsAbort(bool accepted) {
    // 0x00464ac2: "no" restores the saved cursor; "yes" resets with the
    // normal cursor 0x0040ef90(4) chose before the box.
    if (!draughtsGame) return;
    draughtsGame->abortPending = false;
    if (accepted) {
        draughtsGame->waitCursor = false;
        resetDraughts();
    }
}

std::uint32_t DraughtsScene::draughtsHotspotAt(int x, int y) const {
    // The 0x105 and 0x10b events carry the hotspots.bmp id under the
    // pointer: 1 the board, 2..4 the radios, 5..7 the buttons.
    if (!sceneHotspots || x < 0 || y < 0 || x >= sceneHotspots->w ||
        y >= sceneHotspots->h) {
        return 0U;
    }
    return pixelValue(sceneHotspots.get(), x, y);
}

bool DraughtsScene::beginDraughtsInteraction(int x, int y) {
    auto &game = *draughtsGame;
    const std::uint32_t hotspot = draughtsHotspotAt(x, y);
    if (hotspot >= 2U && hotspot <= 7U) {
        // 0x00463fb4: once a move is made, the radios are swallowed.
        if (hotspot <= 4U && draughtsMoves != 0) return true;
        game.pressedControl = static_cast<int>(hotspot);
        if (hotspot <= 4U) {
            // A radio reports on the press (widget type 2, 0x00406c80)
            // when it is not already selected.
            if (game.level != static_cast<int>(hotspot) - kControlEasy) {
                controlDraughts(static_cast<int>(hotspot));
            }
            game.pressedControl = 0;
        }
        return true;
    }
    if (hotspot != kBoardHotspot) return false;
    // mouse_down 0x00464790.
    if (game.state != DraughtsState::Waiting) return true;  // not your turn
    const draughts::Point square = draughts::squareAt(x, y);
    if (square.x < 0 || square.x > 7 || square.y < 0 || square.y > 7) {
        return true;                                       // outside board
    }
    const int piece = draughts::pieceAt(game.board, square.x, square.y);
    if (piece < 0) return true;                            // no piece
    if (game.board.pieces[static_cast<std::size_t>(piece)].side != game.turn) {
        return true;                                       // opponent's piece
    }
    if (game.jumpingPiece >= 0 && piece != game.jumpingPiece) {
        return true;                                       // wrong piece
    }
    referenceTrace.emit("minigame_state", {
        {"game", "draughts"}, {"state", "pickup"}, {"piece", piece},
        {"turn", game.turn}});
    playDraughtsSound("pickup", 2);
    setDraughtsLayer(piece, kLiftedLayer);
    game.dragPiece = piece;
    game.dragPointer = {x, y};
    const auto &art = game.art[static_cast<std::size_t>(piece)];
    game.dragOrigin = {art.x, art.y};
    return true;
}

void DraughtsScene::moveDraughtsInteraction(int x, int y) {
    // 0x0040c300: the dragged descriptor follows the pointer's motion.
    auto &game = *draughtsGame;
    if (game.dragPiece < 0) return;
    auto &art = game.art[static_cast<std::size_t>(game.dragPiece)];
    art.x = game.dragOrigin.x + x - game.dragPointer.x;
    art.y = game.dragOrigin.y + y - game.dragPointer.y;
}

void DraughtsScene::endDraughtsInteraction(int x, int y) {
    auto &game = *draughtsGame;
    if (game.pressedControl != 0) {
        // Button release 0x00406d40 posts 0x116 while still inside.
        const int id = game.pressedControl;
        game.pressedControl = 0;
        const std::uint32_t hotspot = draughtsHotspotAt(x, y);
        if (static_cast<int>(hotspot) == id) controlDraughts(id);
        return;
    }
    if (game.dragPiece < 0) return;
    moveDraughtsInteraction(x, y);
    const int piece = game.dragPiece;
    game.dragPiece = -1;
    const auto restore = [&] {
        // 0x0040c240 moves the descriptor back; 0x00465f70 sets its layer
        // to its row.
        auto &art = game.art[static_cast<std::size_t>(piece)];
        art.x = game.dragOrigin.x;
        art.y = game.dragOrigin.y;
        setDraughtsLayer(
            piece, game.board.pieces[static_cast<std::size_t>(piece)].row);
    };
    // drag_end 0x00464ae0: the hotspot under the pointer must be the board.
    const std::uint32_t hotspot = draughtsHotspotAt(x, y);
    if (hotspot != kBoardHotspot) {
        restore();
        return;
    }
    const draughts::Point square = draughts::squareAt(x, y);
    const auto &record = game.board.pieces[static_cast<std::size_t>(piece)];
    if (square.x == record.col && square.y == record.row) {
        restore();
        return;
    }
    if (square.x < 0 || square.x > 7 || square.y < 0 || square.y > 7) {
        restore();
        return;
    }
    int captured = -1;
    if (!draughts::validateMove(game.board, piece, square.x, square.y,
                                &captured)) {
        referenceTrace.emit("minigame_state", {
            {"game", "draughts"}, {"state", "invalid"}, {"piece", piece}});
        restore();
        playDraughtsSound("wrong", 1);
        return;
    }
    const bool notCrowned =
        playDraughtsMove({piece, square.x, square.y, captured, 0.0F});
    setDraughtsLayer(piece,
                     game.board.pieces[static_cast<std::size_t>(piece)].row);
    game.jumpingPiece = piece;
    if (captured >= 0 && draughts::pieceCanMove(game.board, piece, true) &&
        notCrowned) {
        return;  // the same piece must jump again
    }
    endDraughtsTurn();
}

bool DraughtsScene::handleDraughtsKey(SDL_Keycode key) {
    // 0x004649d0: Pause, Escape, P and F5 are left alone; any other key
    // while the wait cursor is up (0x00464a60) asks to abort the game.
    if (key == SDLK_PAUSE || key == SDLK_ESCAPE || key == SDLK_p ||
        key == SDLK_F5) {
        return false;
    }
    auto &game = *draughtsGame;
    if (!game.waitCursor) return false;
    requestDraughtsReset();
    return true;
}

void DraughtsScene::renderDraughts(double elapsed) {
    (void)elapsed;
    auto &game = *draughtsGame;
    drawCenteredText(renderer, menuTitleFont,
                     language.get("draughts", "SCENENAME", "Damm"), kTitleText);
    drawCenteredText(
        renderer, calendarFont,
        std::to_string(12 - draughts::alivePieces(game.board, 0)),
        kWhiteLostText);
    drawCenteredText(
        renderer, calendarFont,
        std::to_string(12 - draughts::alivePieces(game.board, 1)),
        kBlackLostText);
    const auto drawTexture = [&](SDL_Texture *texture, SDL_Point at) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
        const SDL_Rect destination{at.x, at.y, width, height};
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
        return destination;
    };
    // Control captions use the font 0x004640b6 hands to 0x004068a0, Amerigo
    // BT_14_. Radios show their _1 art while selected, with the level letter.
    for (int level = 0; level < 3; ++level) {
        const SDL_Point at = kLevelRadios[static_cast<std::size_t>(level)];
        SDL_Rect bounds{at.x, at.y, 29, 33};
        if (game.level == level) {
            bounds = drawTexture(game.levelPressed.get(), at);
        }
        drawCenteredText(
            renderer, calendarFont,
            language.get("draughts", kLevelKeys[static_cast<std::size_t>(level)],
                         ""),
            bounds);
    }
    const auto button = [&](int id, SDL_Texture *pressed, SDL_Point at,
                            std::string text) {
        int width = 0;
        int height = 0;
        SDL_QueryTexture(pressed, nullptr, nullptr, &width, &height);
        const SDL_Rect bounds{at.x, at.y, width, height};
        if (game.pressedControl == id) drawTexture(pressed, at);
        drawCenteredText(renderer, calendarFont, text, bounds);
    };
    button(kControlMode, game.button1Pressed.get(), kModeButton,
           language.get("draughts",
                        kModeKeys[static_cast<std::size_t>(draughtsMode())],
                        ""));
    button(kControlHint, game.button2Pressed.get(), kHintButton,
           language.get("draughts", draughtsMode() == 2 ? "START" : "HINT",
                        ""));
    button(kControlReset, game.button2Pressed.get(), kResetButton,
           language.get("draughts", "RESET", "Omstart"));

    std::array<int, 24> order{};
    for (int index = 0; index < 24; ++index) order[static_cast<std::size_t>(index)] = index;
    std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
        const auto &a = game.art[static_cast<std::size_t>(left)];
        const auto &b = game.art[static_cast<std::size_t>(right)];
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.order < b.order;
    });
    for (const int index : order) {
        const auto &art = game.art[static_cast<std::size_t>(index)];
        if (art.pawnAdded) {
            drawTexture((index < 12 ? game.whitePawn : game.blackPawn).get(),
                        {art.x, art.y});
        }
        if (art.kingAdded) {
            drawTexture((index < 12 ? game.whiteKing : game.blackKing).get(),
                        {art.x, art.y});
        }
    }
}

bool DraughtsScene::executeDraughtsDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "draughts:";
    if (activeScene != "draughts" || !draughtsGame) return false;
    auto &game = *draughtsGame;
    const auto control = command.substr(prefix.size());
    if (control.rfind("sound-variant:", 0) == 0) {
        // Mirrors the capture fixture at 0x004648fa.
        const int variant = std::stoi(std::string(control.substr(14)));
        if (variant < 1) return false;
        game.soundVariant = variant;
    } else if (control.rfind("control:", 0) == 0) {
        const int id = std::stoi(std::string(control.substr(8)));
        if (id < kControlEasy || id > kControlReset) return false;
        controlDraughts(id);
    } else if (control.rfind("move:", 0) == 0) {
        // move:<from col>,<from row>,<to col>,<to row> as a drag.
        int values[4]{};
        std::size_t start = 5;
        for (int &value : values) {
            const auto end = control.find(',', start);
            value = std::stoi(std::string(control.substr(start, end - start)));
            start = end == std::string_view::npos ? end : end + 1;
        }
        // Waits (returns false) until that side may pick the piece up.
        if (!game.initialized || game.state != DraughtsState::Waiting ||
            dialogMode != DialogMode::None) {
            return false;
        }
        const draughts::Point from = draughts::squareOrigin(values[0], values[1]);
        const draughts::Point to = draughts::squareOrigin(values[2], values[3]);
        beginDraughtsInteraction(from.x + 22, from.y + 24);
        if (game.dragPiece < 0) return false;
        endDraughtsInteraction(to.x + 22, to.y + 24);
    } else if (control == "status") {
        std::cout << "Draughts status: state " << static_cast<int>(game.state)
                  << ", turn " << game.turn << ", moves " << draughtsMoves
                  << ", depth " << game.depth << ", mode " << draughtsMode()
                  << ", white " << draughts::alivePieces(game.board, 0)
                  << ", black " << draughts::alivePieces(game.board, 1)
                  << '\n';
    } else {
        return false;
    }
    std::cout << "Developer control API: " << command << '\n';
    return true;
}

} // namespace fgp
