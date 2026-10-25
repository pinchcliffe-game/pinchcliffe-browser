#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/app_core.h"
#include "game/puzzle_gem_renderer.h"

namespace fgp {

// The "puzzle" activity (Solines Smykkeskrin, Soline's jewel box): its state
// and logic. Implemented in app_puzzle.cpp, which transcribes FGP.exe's board
// code 0x00452840-0x00454FB0 and scene callback 0x00454FB0-0x00455CC0. The
// gems themselves are drawn by the executable's small 3D engine
// (0x00457F80-0x0045ABD0); see renderPuzzle.
class PuzzleScene : protected virtual AppCore {
protected:
    static constexpr int kPuzzleColumns = 9;
    static constexpr int kPuzzleRows = 8;
    static constexpr int kPuzzleCells = kPuzzleColumns * kPuzzleRows;

    // One 0x104-byte gem object (0x00453000). Timers are in the 0x112
    // handler's units: milliseconds * 0.1 (0x0049F244).
    struct PuzzleGem {
        int id{};                  // native handle; slots hold it
        int cell{};                // +0x18
        int type{};                // +0xF4: 0-7 colours, 8 joker, 9 gold,
                                   //        10 silver
        bool small{};              // +0x28: a seed that has not grown yet
        float smallTime{};         // +0x2C
        bool growing{};            // +0x30
        float growTime{};          // +0x34
        bool dying{};              // +0x38
        float dieTime{};           // +0x3C
        bool selected{};           // +0x40
        std::array<float, 3> rotation{};  // +0x1C..+0x24, rand() % 360 each
        float jokerTime{};         // +0x100
        // The gem's two 3D meshes: +0x08 the gem, +0x0C its seed form.
        PuzzleMeshInstance body;
        PuzzleMeshInstance seed;
        TexturePtr texture;
        VideoFrame frame;
    };
    // The 0x288-byte board object 0x00453590 allocates ([0x00693AE0]).
    struct PuzzleBoard {
        std::array<int, kPuzzleCells> slots{};  // +0x000, 0 = empty
        std::vector<PuzzleGem> gems;            // list at +0x26C, in order
        int level{1};                           // +0x270
        bool jokerGrouped{};                    // +0x274
        bool goldGrouped{};                     // +0x278
        bool silverGrouped{};                   // +0x27C
        int score{};                            // +0x280
        unsigned spawned{};                     // +0x284
    };
    struct PuzzleClip {
        bool added{};
        bool playing{};
        double startedAt{};
    };
    struct PuzzleCloneSound {
        std::string asset;
        double endsAt{};
    };
    struct PuzzleGame {
        DecodedMedia lid;
        DecodedMedia lid2;
        DecodedMedia bottle;
        DecodedMedia rotate;
        DecodedMedia pickup;
        DecodedMedia drop1;
        DecodedMedia drop2;
        DecodedMedia group;
        PuzzleGemRenderer gemRenderer;
        TexturePtr lidTexture;
        TexturePtr bottleTexture;
        // mask.bmp (200x150) labelled 1..72 by the 0x004552A0 flood fill;
        // 0 is black and -1 a pixel no seed reached.
        std::vector<int> mask;

        std::optional<PuzzleBoard> board;           // 0x00693AE0
        bool busy{};                                // 0x00693A30
        bool playing{};                             // 0x00693A34
        bool gameOver{};                            // 0x00695C10
        float gameOverTime{};                       // 0x00695D00
        int selected{};                             // 0x00695C14 (gem id)
        unsigned specialThreshold{};                // 0x00695CD4
        float levelProgress{};                      // 0x00695C04
        int finalScore{};                           // 0x00694BC0
        // 0x00455620 / 0x00455650 / 0x00455AF0: the rotate sound's fade.
        bool rotateFading{};                        // 0x00694BCC
        bool rotateFadeOut{};                       // 0x00695C00
        float rotateFadeTime{};                     // 0x00695CE0
        bool rotateAdded{};
        bool rotatePlaying{};
        double rotateStartedAt{};
        LiveAudioMixer::EffectHandle rotateEffect{};
        // The two text controls 0x00455090 creates (0x00695C28 score,
        // 0x00693AE8 level) and the strings 0x00455720 prints into them.
        bool textControls{};
        std::string scoreText;
        std::string levelText;
        int bottleFrame{};
        PuzzleClip lidClip;
        PuzzleClip lid2Clip;
        PuzzleClip bottleClip;
        std::vector<PuzzleCloneSound> clones;
        // Path preview 0x00454DA0 draws into the 0x1CC x 0x140 overlay at
        // (170,172), layer 98; cleared on every click.
        std::vector<std::uint16_t> pathOverlay;
        TexturePtr pathTexture;
        VideoFrame pathFrame;
        bool pathDirty{};
        int nextGemId{1};
        double lastUpdate{-1.0};
        bool initialized{};
        bool gameOverMessagePending{};
        bool highScorePending{};
        std::string snapshotPath;
    };

    PuzzleGame loadPuzzleGame(Package &package);
    bool beginPuzzleInteraction(int x, int y);
    void movePuzzlePointer(int x, int y);
    void updatePuzzle(double elapsed);
    void renderPuzzle(double elapsed);
    // Called by GameApp when the Game Over message or the score table
    // closes.
    void continuePuzzleAfterGameOverMessage();
    void continuePuzzleAfterHighScore();
    // Developer control API for "puzzle:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executePuzzleDeveloperCommand(std::string_view command);

    std::optional<PuzzleGame> puzzleGame;

private:
    int puzzleRandom();
    int puzzleRandomBelow(int bound);
    double puzzleNow();
    void puzzleInitialize();
    void puzzleCloneSound(std::string_view asset);
    void puzzleStartRotate();
    void puzzleFadeRotate();
    void puzzleStopRotate();
    void puzzleStartBoard();
    void puzzleUpdateScore(int removed);
    PuzzleGem *puzzleGem(int id);
    PuzzleGem *puzzleCellGem(int cell);
    std::optional<int> puzzleCellAt(int x, int y) const;
    void puzzleSpawnGem(int cell, int type, bool small);
    int puzzleFreeCell();
    bool puzzleSpawnBatch(bool small, bool special);
    void puzzleSpawnRound(bool small);
    bool puzzleSpecialDue();
    int puzzleCellType(int column, int row) const;
    bool puzzleUniform(int column, int row, int width, int height) const;
    bool puzzleExtendGroup(std::array<int, 4> &rect, int grow, int growDown)
        const;
    int puzzleRemoveGroups();
    int puzzleMoveGem(PuzzleGem &gem, int targetId, bool overSmall);
    void puzzleRecolour(int type);
    void puzzleSilverBlast();
    void puzzleMatureSmall();
    bool puzzleNoMovableGem() const;
    bool puzzleNearlyFull() const;
    bool puzzleSlotOpen(int index) const;
    void puzzleAfterMove();
    void puzzleGameOver();
    std::optional<std::vector<int>> puzzleFindPath(int from, int to) const;
    bool puzzleTryMove(int targetId, bool overSmall);
    void puzzleKillGem(PuzzleGem &gem);
    void puzzleDeselect(PuzzleGem &gem);
    void puzzleRemoveGem(int id);
    void puzzleDestroyBoard();
    void puzzleAnimate(float step);
    // Copies ARGB pixels into `frame` and uploads them through the shared
    // memoizing uploadVideoFrame when they changed.
    void puzzleUpload(SDL_Texture *texture, VideoFrame &frame,
                      const std::uint32_t *pixels, int width, int height);
    void puzzleClearPath();
    void puzzleDrawPath(const std::vector<int> &path, int type);
    void puzzleWobble(PuzzleGem &gem);
    void puzzleFinishClips(double now);

    // The C runtime rand() state is process-wide; the capture fixture
    // reseeds it when 0x00455C30 builds a board.
    std::uint32_t puzzleRandomState{};
    bool puzzleRandomSeeded{};
    std::optional<std::uint32_t> puzzleFixedSeed;
    std::uint64_t puzzleFrameRevision{};
};

} // namespace fgp
