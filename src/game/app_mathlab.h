#pragma once

#include <deque>

#include "game/app_core.h"

namespace fgp {

// The "mathlab" minigame: its state and logic.
// Implemented in app_mathlab.cpp, a port of FGP.exe 0x0047fe50-0x00482230.
class MathlabScene : protected virtual AppCore {
protected:
    // One original 0xa8-byte animation descriptor, or a text or progress
    // control's descriptor. Every SCENE_AddAnim/RemoveAnim/StopAnim call the
    // original makes on it is mirrored in the reference trace.
    enum class MathlabAnimKind { Bitmap, Smacker, Text, Progress };
    struct MathlabAnim {
        std::uint32_t address{};
        MathlabAnimKind kind{MathlabAnimKind::Bitmap};
        std::string name;
        int defaultX{};
        int defaultY{};
        int defaultLayer{};
        std::uint32_t hotspot{};
        bool visible{true}; // descriptor +0x44: a Smacker draws its video
        bool loop{};        // descriptor +0x4c
        bool clone{};
        bool active{};      // +0x7c
        bool playing{};     // +0x78
        int x{};
        int y{};
        int layer{};
        bool shellDrawn{};  // Help and Exit: listed, drawn by the shell
        int hitMode{};      // +0x9c
        double playStarted{};
        double frozenAt{};
        int frame{-1};      // a frame set by SmackGoto (0x00403770)
        float volume{1.0F}; // 0x00403620
        std::size_t audioLoops{};
        std::uint64_t audioHandle{};
        // Text controls (0x00407930) and progress bars (0x004089a0).
        SDL_Rect rect{};
        std::string text;
        int textFlags{};
        int font{};
        std::string image;
        float progress{};
    };
    struct MathlabImage {
        SurfacePtr surface;
        TexturePtr texture;
        int width{};
        int height{};
        Uint32 key{};
    };
    struct MathlabClip {
        DecodedMedia media;
        TexturePtr texture;
        double completion{};
    };
    // A shared push or radio button (0x00406540, 0x278 bytes from
    // 0x00533688), whose +0x1c descriptor shows the image of its state.
    struct MathlabButton {
        int id{};
        int group{};       // +0x0c
        int type{1};       // +0x08: 1 push, 2 radio
        int state{};       // +0x270
        bool enabled{};    // +0x274
        int width{};       // +0x10, measured once
        int height{};      // +0x14
        std::array<std::string, 4> images; // +0x16c
        MathlabAnim anim;
    };
    // One rolling number ball (0x24-byte record at 0x006e3e20).
    struct MathlabBall {
        int value{};       // +0x00
        bool active{};     // +0x04
        int position{};    // +0x08, the frame of its ball%02d clip
        int limit{};       // +0x0c
        int time{};        // +0x14
        int descriptor{-1};// +0x18, index into the 30 ball descriptors
        int released{};    // +0x1c
        int sound{};       // +0x20
    };
    // A rolling digit counter (0x1c bytes, 0x00480500/0x00480df0).
    struct MathlabCounter {
        bool animating{};
        int digits{};
        int firstText{};   // index into MathlabGame::digitTexts
        int shown{};
        int from{};
        int target{};
        int started{};
    };
    struct MathlabPour {
        int tank{};
        int value{};
    };
    struct MathlabGame {
        std::unordered_map<std::string, MathlabImage> images;
        std::unordered_map<std::string, MathlabClip> clips;
        std::array<BitmapFont, 5> fonts;
        Package *package{};
        // Static descriptors.
        MathlabAnim start, labels, disable2, ballDrag;
        MathlabAnim fan, fanLong, fanFade, pump, opener, closer, lightbulb,
            dumpball, flush, overflow, balls, startballs, endballs, newball,
            ballsSound, endballSound, tap;
        std::array<MathlabAnim, 5> water;
        std::array<MathlabAnim, 30> ballAnims;
        std::array<bool, 30> ballAnimUsed{};
        // A deque keeps clone references stable while a finishing clip
        // starts another sound mid-update; finished slots are reused.
        std::deque<MathlabAnim> clones;
        // Text controls and progress bars.
        MathlabAnim scoreText, levelText, spareText, clockText;
        std::array<MathlabAnim, 5> capacityTexts;
        std::array<MathlabAnim, 5> valueTexts;
        std::array<MathlabAnim, 10> digitTexts;
        MathlabAnim levelBar;
        std::array<MathlabAnim, 5> tankBars;
        std::vector<MathlabButton> buttons;
        int pressedButton{};
        // Game state, by original global.
        int phase{};                  // 0x006e1d88
        int score{};                  // 0x006e2030
        int shownScore{};             // 0x006dba70
        int levelIndex{};             // 0x006e3d68
        int levelProgress{};          // 0x006dba74
        int levelGoal{1};             // 0x006e3140
        int deadline{};               // 0x006e3538
        int clockBase{};              // 0x006dba50
        int penalty{};                // 0x006dba68
        bool thirdDisabled{true};     // 0x00512860
        bool openerMode{true};        // 0x005120a8
        int releaseSerial{1};         // 0x006dba6c
        int pendingBalls{};           // 0x006dba4c
        bool flushPending{};          // 0x006e3e18
        int tapState{};               // 0x006ddbf8
        int tapFrame{};               // 0x006e3c80
        int tapFrom{};                // 0x006e3c7c
        int tapStarted{};             // 0x006e3d64
        int bonus{};                  // 0x006e3cd8
        std::array<int, 3> operands{};       // 0x006e1cc0
        std::array<int, 2> operators{};      // 0x006e3cdc
        std::array<int, 5> capacities{};     // 0x006e312c
        std::array<int, 5> tanks{};          // 0x006e20b0
        std::array<int, 5> pours{};          // 0x006dba38
        std::array<int, 5> blinkValue{};     // 0x006e3ce4
        std::array<int, 5> blinkStarted{};   // 0x006e3cf8
        std::array<MathlabCounter, 3> operandCounters; // 0x006e3d10
        MathlabCounter resultCounter;                  // 0x006e3110
        std::array<MathlabBall, 7> rack;
        std::vector<MathlabPour> queue;      // 0x006e3c88
        // Drag (0x0040c1f0).
        bool dragging{};
        int dragValue{};
        int dragOffsetX{};
        int dragOffsetY{};
        bool initialized{};
        bool gameOverModal{};
        std::uint32_t randomState{};
        std::optional<std::uint32_t> fixedSeed;
        std::optional<int> developerWaitUntil;
        // SCENE's active list (0x00554958, count 0x0054fe90): appended by
        // SCENE_AddAnim, compacted by SCENE_RemoveAnim and re-sorted by layer
        // with the C runtime qsort after each change (0x004129b0). Its order
        // is the draw, hit-test and update order.
        std::vector<MathlabAnim *> activeList;
        MathlabAnim helpButton;
        MathlabAnim exitButton;
    };

    MathlabGame loadMathlabGame(Package &package);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        mathlabHotspotAt(int x, int y) const;
    bool beginMathlabInteraction(int x, int y);
    void moveMathlabInteraction(int x, int y);
    void endMathlabInteraction(int x, int y);
    bool mathlabDragging() const {
        return mathlabGame && mathlabGame->dragging;
    }
    void updateMathlab(double elapsed);
    void renderMathlab(double elapsed);
    // The score table closed: 0x0048214b onwards.
    void finishMathlabGameOver();
    // Developer control API for "mathlab:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeMathlabDeveloperCommand(std::string_view command);

    std::optional<MathlabGame> mathlabGame;

    // Reached by GameApp::startTournamentRound as well.
    void startMathlabGame(int difficulty);
private:
    int mathlabNow();
    int mathlabRandom(int bound);
    void initializeMathlabScene();
    std::vector<MathlabAnim *> mathlabAnims();
    std::vector<const MathlabAnim *> mathlabAnims() const;
    MathlabClip *mathlabClip(const std::string &name);
    MathlabClip *loadMathlabClip(
        std::unordered_map<std::string, MathlabClip> &clips,
        const std::string &name);
    void addMathlabAnim(MathlabAnim &anim, int x, int y, int layer);
    void removeMathlabAnim(MathlabAnim &anim);
    void stopMathlabAnim(MathlabAnim &anim);
    void playMathlabAnim(MathlabAnim &anim);
    void addAndPlayMathlabAnim(MathlabAnim &anim);
    void setMathlabFrame(MathlabAnim &anim, int frame);
    void playMathlabSound(std::string_view name);
    void finishMathlabClip(MathlabAnim &anim);
    void createMathlabText(MathlabAnim &anim, SDL_Rect rect);
    void setMathlabText(MathlabAnim &anim, int font, std::string text,
                        int flags);
    void setMathlabNumber(MathlabAnim &anim, int font, int value);
    void setMathlabProgress(MathlabAnim &anim, float value);
    bool mathlabAnimHit(const MathlabAnim &anim, int x, int y) const;
    std::uint32_t resolveMathlabHotspot(int x, int y) const;
    // Buttons (0x00406540-0x00406fb0).
    MathlabButton *mathlabButton(int id);
    void createMathlabButton(int id, int group, int type, int x, int y,
                             std::string pressed);
    void destroyMathlabButton(int id);
    void setMathlabButtonState(MathlabButton &button, int state);
    void setMathlabGroupState(int group, int state);
    void refreshMathlabButton(MathlabButton &button);
    std::optional<SDL_Rect> measureMathlabButton(MathlabButton &button);
    void pressMathlabButtons(std::uint32_t hotspot, int x, int y);
    void controlMathlab(int id);
    // Game (0x0047fe50-0x00482230).
    void enterMathlabLevel(bool first);
    void releaseMathlabBalls();
    int allocateMathlabBallAnim();
    bool mathlabValueCrowded(int value) const;
    void sortMathlabRack();
    bool spawnMathlabBall();
    void removeMathlabBall(std::uint32_t hotspot);
    void startMathlabCounter(MathlabCounter &counter, int target);
    void drawMathlabCounter(MathlabCounter &counter);
    int mathlabResult() const;
    void refreshMathlabResult();
    void resetMathlabOperands();
    void showMathlabTank(int tank);
    void clearMathlabTank(int tank);
    void showMathlabScore();
    void penalizeMathlab(int seconds);
    void checkMathlabLevel();
    void flushMathlab();
    void scoreMathlabTank(int tank, bool single);
    void scoreMathlabTanks();
    void updateMathlabTap();
    void updateMathlabRack();
    void updateMathlabBlink();
    void mathlabTick();
    void mathlabGameOver();
    void finishMathlabAnim(MathlabAnim &anim);
    void dropMathlabBall(std::uint32_t hotspot);
};

} // namespace fgp
