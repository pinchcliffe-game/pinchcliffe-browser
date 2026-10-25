#pragma once

#include "game/puzzle_stage.h"

namespace fgp {

// The "sliding" activity (Skyvepuslespill): its state and logic. Implemented
// in app_sliding.cpp, a port of FGP.exe 0x0046dc50-0x0046ef10; the scene
// callback is 0x0046dc50. REVERSE_ENGINEERING.md ("Sliding puzzle") has the
// account.
class SlidingScene : protected virtual PuzzleStageScene {
protected:
    // One 0xbc-byte cell of the fixed 5x5 array at 0x006c20b8.
    struct SlidingTile {
        int homeX{};          // +0x00
        int homeY{};          // +0x04
        int x{};              // +0x08
        int y{};              // +0x0c
        bool locked{};        // +0x10
        PuzzleDescriptor descriptor;  // +0x14
        Rgb565Image image;
    };
    struct SlidingGame {
        PuzzleStage stage;
        // Session record 0x006c2058 ([0x006c2050] points at it).
        bool session{};
        int picture{-1};               // +0x00
        int grid{};                    // +0x3c, +0x40
        int lockedCount{};             // +0x44
        std::array<SDL_Point, 2> locked{};  // +0x48..+0x54
        std::uint32_t record{};        // +0x5c, profile record 0x402
        std::array<SlidingTile, 25> tiles;
        // 0x006c0e1c: the tile at each current cell (x + y * 5), or none.
        std::array<SlidingTile *, 25> cells{};
        int blankX{};                  // 0x006c0e88
        int blankY{};                  // 0x006c0e8c
        int originX{};                 // 0x006c0e80
        int originY{};                 // 0x006c0e84
        int tileWidth{};               // 0x006c0e98
        int tileHeight{};              // 0x006c0e9c
        bool busy{};                   // 0x006c33c0
        // 0x006c0ff8: the row 0x0041ee50 returned for the last solve.
        std::optional<ScoreRow> lastScoreRow;
        int clockStart{};              // 0x006c0ea0
        int elapsed{};                 // 0x006c0e18
        std::optional<int> clockTimerDue;  // 0x006c33c4, every 500 ms
        std::array<PuzzleDescriptor, 2> frames;  // 0x006c0ea8, 0x006c0f50
        std::array<TexturePtr, 2> frameTextures;
        std::array<Rgb565Image, 2> frameImages;
        PuzzleDescriptor clock;        // 0x006c3318
        PuzzleDescriptor picture490;   // 0x0050a448
        PuzzleDescriptor preview;      // 0x0050a3a0
        std::optional<std::uint32_t> fixedSeed;
        bool readyAnnounced{};
    };

    SlidingGame loadSlidingGame(Package &package);
    bool beginSlidingInteraction(int x, int y);
    void moveSlidingInteraction(int x, int y);
    void endSlidingInteraction(int x, int y);
    bool slidingKey(SDL_Keycode key);
    void updateSliding(double elapsed);
    void renderSliding(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        slidingHotspotAt(int x, int y) const;
    bool executeSlidingDeveloperCommand(std::string_view command);

    std::optional<SlidingGame> slidingGame;
    MsvcRandom slidingRandom;

private:
    int slidingClock() const;
    void openSlidingChooser();
    void startSlidingGame(int picture);
    void clearSlidingBoard();
    void buildSlidingBoard();
    void shuffleSliding();
    bool slidingMakeMove(int x, int y);
    void slidingMovePiece(SlidingTile &tile, int x, int y);
    void clickSlidingTile(int index);
    bool slidingSolved() const;
    void slidingWin();
    void slidingTimer(int now);
    void slidingControlClicked(int id);
};

} // namespace fgp
