#pragma once

#include "game/jigsaw_cut.h"
#include "game/puzzle_stage.h"

namespace fgp {

// The "jigsaw" activity (Puslespill): its state and logic. Implemented in
// app_jigsaw.cpp, a port of FGP.exe 0x00469960-0x0046d1f0; the scene
// callback is 0x00469960. REVERSE_ENGINEERING.md ("Jigsaw") has the account.
class JigsawScene : protected virtual PuzzleStageScene {
protected:
    struct JigsawPiece {
        JigsawPieceImage image;
        PuzzleDescriptor descriptor;
    };
    struct JigsawGame {
        PuzzleStage stage;
        // Session record 0x006bea50 ([0x006bea44] points at it while a
        // board exists).
        bool session{};
        int picture{-1};               // +0x00
        int grid{};                    // +0x3c, +0x40
        bool rotation{};               // +0x38
        std::uint32_t record{};        // +0x50, profile record 0x402
        // 0x006bfb9c: the category argument of the scene entry, or -1.
        int category{-1};
        // 0x006bfad8: four piece sets, one per quarter turn.
        std::array<std::vector<JigsawPiece>, 4> sets;
        std::vector<std::uint32_t> edges;   // 0x006bea40
        std::vector<int> rotations;         // 0x006bea48
        int dragged{-1};                    // 0x00509c70
        int dragOffsetX{};                  // 0x006bc9b0
        int dragOffsetY{};                  // 0x006bc9ac
        bool joined{};                      // 0x006bfbb0
        bool solutionUsed{};                // 0x006bfba8
        bool solved{};                      // 0x006bfbac
        // 0x006bd9e8: the row 0x0041ee50 returned for the last solve.
        std::optional<ScoreRow> lastScoreRow;
        bool clockStopped{true};            // 0x006bfb88
        bool sliding{};                     // 0x006bfba4
        int solveIndex{-1};                 // 0x00509c80
        std::optional<int> solveTimerDue;
        int clockStart{};                   // 0x006bc9a8
        int elapsed{};                      // 0x006bc910
        std::optional<int> clockTimerDue;   // 0x006bfb98, every 500 ms
        int lastPass{};
        PuzzleDescriptor clock;             // 0x006bfae0
        PuzzleDescriptor picture490;        // 0x00509a78
        PuzzleDescriptor preview;           // 0x00509b20
        PuzzleDescriptor frame;             // 0x005099d0
        TexturePtr frameTexture;
        Rgb565Image source;
        // Developer seed: replaces the time-seeded rand() state before the
        // board build, as the capture fixture's 0x0048d32b breakpoint does.
        std::optional<std::uint32_t> fixedSeed;
        bool readyAnnounced{};
        // A left press reached this scene; its release is event 0x106.
        bool leftPressed{};
    };

    JigsawGame loadJigsawGame(Package &package);
    bool beginJigsawInteraction(int x, int y);
    bool beginJigsawRightInteraction(int x, int y);
    void moveJigsawInteraction(int x, int y);
    void endJigsawInteraction(int x, int y);
    bool jigsawKey(SDL_Keycode key);
    void updateJigsaw(double elapsed);
    void renderJigsaw(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        jigsawHotspotAt(int x, int y) const;
    bool executeJigsawDeveloperCommand(std::string_view command);

    std::optional<JigsawGame> jigsawGame;
    MsvcRandom jigsawRandom;

private:
    JigsawPiece &jigsawPiece(int index);
    int jigsawClock() const;
    void openJigsawChooser();
    void startJigsawGame(int picture);
    void clearJigsawBoard();
    void buildJigsawBoard();
    void scatterJigsawPieces();
    void jigsawControlClicked(int id);
    void jigsawExit();
    void pressJigsawPiece(int piece, int x, int y);
    void dropJigsawPiece();
    void jigsawJoin(int piece, std::vector<char> &visited, int &depth);
    void jigsawAlignGroup(int piece, std::vector<char> &visited, int &depth);
    void jigsawSetGroupLayer(int piece, int layer, std::vector<char> &visited);
    void jigsawRaise(int piece);
    void jigsawLower(int piece);
    void jigsawRotate(int direction, int x, int y);
    void jigsawRotateGroup(int rotation, int piece, std::vector<char> &visited);
    bool jigsawComplete() const;
    void jigsawWin();
    void jigsawSolveStep();
    void jigsawFinishSlide();
    void jigsawTimer(int now);
    void jigsawIdle(int now);
    void jigsawPlayDrop();
    std::optional<SDL_Point> jigsawPickPoint(int piece);
};

} // namespace fgp
