#pragma once

#include <atomic>
#include <map>
#include <memory>

#include "game/app_core.h"
#include "game/chess_engine.h"

namespace fgp {

// The "chess" activity (Sjakk): its descriptors, controls and turn flow.
// Implemented in app_chess.cpp; the scene callback is FGP.exe 0x0045ee90
// and the rules and computer player are in chess_engine.cpp.
class ChessScene : protected virtual AppCore {
protected:
    // One animation descriptor the scene adds and removes: the 32 pieces
    // (0x00503690, stride 0xb8) and the 16 promotion queens (0x00504d80,
    // stride 0xa8).
    struct ChessSprite {
        std::string name;
        int x{};
        int y{};
        int layer{};
        bool active{};
        std::uint64_t order{};
    };
    // A shared push (type 1) or radio (type 2) button, 0x00406540.
    struct ChessButton {
        int id{};
        int group{};
        int type{1};
        int x{};
        int y{};
        std::string pressedImage;   // the state-1 image; none has a state 0
        int state{};                // +0x270
        int width{};                // +0x10, measured once
        int height{};               // +0x14
        std::string label;
        bool labelActive{};
    };
    // A SCENE_PlayAnimClone sound (0x00412c20).
    struct ChessCloneSound {
        std::string asset;
        double endsAt{};
    };
    // The search runs on its own thread, as 0x00460dc0 starts one.
    struct ChessSearch {
        chess::Engine engine;
        bool maximizing{};
        chess::Move result;
        std::atomic<bool> done{};
    };
    struct ChessTexture {
        TexturePtr texture;
        int width{};
        int height{};
    };
    struct ChessGame {
        // 0x0045f190 turns the board part of the backdrop a quarter turn.
        ChessTexture board;
        std::map<std::string, ChessTexture, std::less<>> images;
        std::map<std::string, DecodedMedia, std::less<>> sounds;
        chess::Engine engine;
        chess::Random random;              // the main thread's rand()
        std::array<ChessSprite, 48> sprites;
        std::vector<ChessButton> buttons;
        std::uint64_t nextOrder{};
        bool initialized{};
        int pressedButton{};
        // 0x006a4070/0x006a4074: whether the computer plays each turn.
        std::array<bool, 2> computer{true, false};
        int turn{1};                       // 0x006a4078
        int state{};                       // 0x006a29c8
        int moves{};                       // 0x006a3e60
        std::array<std::array<int, 6>, 2> captured{}; // 0x006a3e30
        std::string titleText;
        // A piece being dragged (0x0040c1f0).
        int dragPiece{-1};
        int dragMouseX{};
        int dragMouseY{};
        int dragHomeX{};
        int dragHomeY{};
        // The computer's move: found (0x006a29b0), then glided (0x006a3e68)
        // for one second from glideStart (0x006a3f10).
        std::shared_ptr<ChessSearch> search;
        chess::Move found;
        chess::Move gliding;
        double glideStart{};
        std::vector<ChessCloneSound> clones;
        // Award after a won game against the computer (0x00460d00).
        bool awardPending{};
        int awardLevel{};
        int awardCount{};
        bool busyCursor{};
    };

    ChessGame loadChessGame(Package &package);
    bool beginChessInteraction(int x, int y);
    void moveChessInteraction(int x, int y);
    void endChessInteraction(int x, int y);
    // Key handler 0x0045fa00; true when it asked the abort question.
    bool handleChessKey(SDL_Keycode key);
    void updateChess(double elapsed);
    void renderChess(double elapsed);
    // The Yes answer to the chess:ABORT question (0x0045fab0).
    void confirmChessReset();
    // Developer control API for "chess:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeChessDeveloperCommand(std::string_view command);

    std::optional<ChessGame> chessGame;

private:
    void initializeChess();
    void addChessSprite(std::size_t sprite, int x, int y, int layer);
    void removeChessSprite(std::size_t sprite);
    void playChessSound(std::string_view family, int variants);
    void finishChessClones(double now);
    ChessButton *chessButton(int id);
    void createChessButton(int id, int group, int type, int x, int y,
                           std::string pressedImage, std::string label);
    SDL_Rect measureChessButton(ChessButton &button);
    void setChessButtonState(ChessButton &button, int state);
    void setChessButtonLabel(ChessButton &button, std::string label);
    void emitChessText(std::string_view text, const BitmapFont &font,
                       const SDL_Rect &bounds, int layer);
    void setChessPlayers(int mode);
    int chessPlayersMode() const;
    void resetChess();
    void chessControl(int id);
    void askChessReset();
    void placeChessPiece(int index);
    SDL_Point chessSquarePosition(int x, int y) const;
    void applyChessMove(const chess::Move &move, bool visual);
    void endChessTurn();
    void startChessSearch();
    void takeChessSearchResult();
    void glideChessMove(double now);
    void finishChessGame(int winnerTurn, bool checkmate);
    void dropChessPiece(std::uint32_t hotspot, int x, int y);
    void snapChessPieceBack(int index);
};

} // namespace fgp
