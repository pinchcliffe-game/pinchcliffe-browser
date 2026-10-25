#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/app_core.h"
#include "game/draughts_rules.h"

namespace fgp {

// The "draughts" activity: board, controls, drag-and-drop, the computer's
// turn and its animation. The dispatcher is FGP.exe 0x00463f60.
class DraughtsScene : protected virtual AppCore {
protected:
    // 0x006a7d18.
    enum class DraughtsState {
        Waiting = 0, Thinking = 2, MoveReady = 3, Animating = 4, GameOver = 5,
    };
    // Which of the piece's two descriptors (its pawn at 0x00506470 + 0xb8 * i,
    // its king at 0x005075a0 + 0xa8 * i) is in the scene, and where.
    struct DraughtsPieceArt {
        bool pawnAdded{};
        bool kingAdded{};
        int x{};
        int y{};
        int layer{};
        std::uint64_t order{};
    };
    struct DraughtsCloneSound {
        std::string asset;
        double endsAt{};
    };
    struct DraughtsGame {
        TexturePtr whitePawn;
        TexturePtr blackPawn;
        TexturePtr whiteKing;
        TexturePtr blackKing;
        TexturePtr levelPressed;
        TexturePtr button1Pressed;
        TexturePtr button2Pressed;
        std::vector<std::pair<std::string, DecodedMedia>> sounds;
        std::vector<DraughtsCloneSound> cloneSounds;

        draughts::Board board;
        std::array<DraughtsPieceArt, 24> art{};
        std::uint64_t nextOrder{};
        DraughtsState state{DraughtsState::Waiting};
        int turn{1};
        // 0x006aa374/0x006aa378: 0 human, 1 computer, per side.
        std::array<int, 2> players{1, 0};
        int level{1};
        int depth{5};
        int jumpingPiece{-1};
        std::vector<draughts::Move> plan;
        int animationStep{};
        // The wait cursor 0x0040c370("wait") shows while the computer
        // thinks and moves; the cursor module's handler 0x0040f150 then
        // swallows every mouse message (0x0040f210).
        bool waitCursor{};
        // Mouse-down accepted a piece; the engine drag 0x0040c1f0 follows
        // the pointer until release posts 0x10b.
        int dragPiece{-1};
        SDL_Point dragPointer{};
        SDL_Point dragOrigin{};
        // The pressed control (ids 2..7) while the button is held.
        int pressedControl{};
        bool abortPending{};
        bool gameOverPending{};
        int gameOverSide{};
        double nextTick{};
        std::optional<int> soundVariant;
        bool initialized{};
        bool clonesHeld{};
    };

    DraughtsGame loadDraughtsGame(Package &package);
    bool beginDraughtsInteraction(int x, int y);
    void moveDraughtsInteraction(int x, int y);
    void endDraughtsInteraction(int x, int y);
    bool handleDraughtsKey(SDL_Keycode key);
    void updateDraughts(double elapsed);
    void renderDraughts(double elapsed);
    // Message box continuations (modal 0x00421310 returns).
    void finishDraughtsAbort(bool accepted);
    void finishDraughtsGameOver();
    // Developer control API for "draughts:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeDraughtsDeveloperCommand(std::string_view command);

    std::optional<DraughtsGame> draughtsGame;
    // Globals the original never reinitialises on scene entry: the history
    // ring 0x006a7a18 and the move counter 0x006a9190 (reported before
    // 0x00464350 zeroes it).
    draughts::History draughtsHistory;
    int draughtsMoves{};

private:
    void initializeDraughts();
    void setDraughtsMode(int mode);
    void setDraughtsLevel(int level);
    void resetDraughts();
    void addDraughtsPieceArt(int piece, bool king, int x, int y);
    void removeDraughtsPieceArt(int piece, bool king);
    void placeDraughtsPiece(int piece);
    void setDraughtsLayer(int piece, int layer);
    void playDraughtsSound(std::string_view name, int variants);
    bool playDraughtsMove(const draughts::Move &move);
    void endDraughtsTurn();
    void startDraughtsThinking();
    void tickDraughts();
    void controlDraughts(int id);
    void requestDraughtsReset();
    int draughtsMode() const;
    std::uint32_t draughtsHotspotAt(int x, int y) const;
};

} // namespace fgp
