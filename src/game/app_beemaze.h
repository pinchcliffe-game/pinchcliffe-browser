#pragma once

#include <map>

#include "game/app_core.h"

namespace fgp {

// The "beemaze" minigame (Ludvigs Labyrint): its state and logic.
// Implemented in app_beemaze.cpp, which transcribes FGP.exe's module
// 0x0042f1b0-0x00432fe0. Positions are the original's pixel coordinates
// inside the 704x512 board; the board itself is drawn from (48,74).
class BeemazeScene : protected virtual AppCore {
protected:
    // One SCENE_AddAnim descriptor. `added` mirrors descriptor +0x7c (read by
    // 0x004138f0), `playing` mirrors +0x78 (read by 0x00413900).
    struct BeemazeClip {
        bool added{};
        bool playing{};
        int x{};
        int y{};
        int layer{};
        double startedAt{};
    };
    struct BeemazeTimer {
        bool used{};
        int due{};
        int tag{};
    };
    // A SCENE_PlayAnimClone sound (0x00412c20): added, then torn down when
    // its Smacker player finishes.
    struct BeemazeCloneSound {
        std::string asset;
        double endsAt{};
    };
    struct BeemazeGame {
        std::array<TexturePtr, 5> levelTextures;
        TexturePtr stickTexture;
        TexturePtr nutTexture;
        TexturePtr mushTexture;
        TexturePtr berriesTexture;
        TexturePtr gunTexture;
        TexturePtr honeyTexture;
        TexturePtr lifeTexture;
        TexturePtr startTexture;
        // 0x00432d5f-0x00432db3: walkcycle, honeycycle, smokecycle and
        // honsmokecycle, 15 columns by four 40x52 directions each.
        std::array<TexturePtr, 4> playerSheets;
        HopscotchMedia bees;
        HopscotchMedia beeSleep;
        HopscotchMedia smoke;
        HopscotchMedia points250;
        HopscotchMedia arrow;
        std::array<HopscotchMedia, 7> ambient;
        std::map<std::string, DecodedMedia, std::less<>> sounds;

        bool playing{};               // 0x006584bc
        bool gameOver{};              // 0x006584f0
        bool levelComplete{};         // 0x00648d64
        bool startIconAdded{};
        bool scoreShown{};
        int level{};                  // 0x006598a0
        int lives{};                  // 0x00659668
        unsigned powerups{};          // 0x00658504: 1 smoke, 2 mushroom,
                                      // 4 honey, 8 smoke flash
        int sticks{};                 // 0x006584f4
        int berries{};                // 0x00658550
        int nuts{};                   // 0x00658578
        int mushrooms{};              // 0x006584b4
        int hivesRemaining{};         // 0x006584e4
        int smokers{};                // 0x00648d74

        int playerX{};                // 0x006584f8
        int playerY{};                // 0x006584fc
        int targetX{};                // 0x00658580
        int targetY{};                // 0x00658584
        int heldDirection{};          // 0x00658588
        int facing{1};                // 0x00658500
        int walkCounter{};            // 0x00648c98
        int moveClock{};              // 0x006598ac
        int animationClock{};         // 0x006598b4
        int animationCounter{};       // 0x006598b8
        int lastWalkCounter{};        // 0x006598b0
        int sheet{};
        int sheetColumn{};
        int sheetRow{};

        int beeCount{};               // 0x006598a4
        std::array<int, 7> beeX{};    // 0x00658510
        std::array<int, 7> beeY{};
        std::array<int, 7> beeCellX{};  // 0x00648c60
        std::array<int, 7> beeCellY{};
        std::array<bool, 7> beeStunned{};  // 0x006584c8
        std::array<int, 7> beeStunTimer{}; // 0x00648c9c
        int beeClock{};               // 0x006598a8
        bool resetBeeClock{};         // 0x006584b8

        // 22x16 grid of item descriptors at 0x00648d80, indexed x * 16 + y.
        std::array<std::string, 22 * 16> cellNames;
        std::array<bool, 22 * 16> cellAdded{};
        std::array<SDL_Point, 4> hiveCandidates{};  // 0x00659878
        std::vector<SDL_Point> trail;               // 0x006584b0
        int trailIndex{};                           // 0x00648d60

        std::string levelName;                      // descriptor 0x004d0560
        BeemazeClip levelClip;
        BeemazeClip playerClip;                     // descriptor 0x004cf990
        BeemazeClip smokeClip;
        BeemazeClip arrowClip;
        BeemazeClip pointsClip;
        BeemazeClip startClip;
        std::array<BeemazeClip, 3> lifeClips;
        std::array<BeemazeClip, 7> beeClips;
        std::array<BeemazeClip, 7> beeSleepClips;
        std::array<BeemazeClip, 7> ambientClips;
        int ambientDue{};                           // 0x00658558
        int smokeX{};                               // 0x00658560
        int smokeY{};                               // 0x00658564
        int smokeFlashCount{};                      // 0x006598bc
        int smokeTimer{-1};                         // 0x00648d70
        int smokeFlashTimer{-1};                    // 0x006584c4
        int mushroomTimer{-1};                      // 0x00658554
        std::vector<BeemazeTimer> timers;
        std::vector<BeemazeCloneSound> cloneSounds;
        std::uint64_t passIndex{};
        // Set while an update pass runs: handlers then read the pass clock.
        std::optional<double> passTime;
        bool initialized{};
        bool gameOverMessagePending{};
    };

    BeemazeGame loadBeemazeGame(Package &package);
    void startBeemazeGame();
    bool handleBeemazeKey(SDL_Keycode key, bool down);
    void updateBeemaze(double elapsed);
    void renderBeemaze(double elapsed);
    // Called by GameApp when the Game Over message and the score table close.
    void continueBeemazeAfterGameOverMessage();
    void restoreBeemazeStartAfterHighScore();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        beemazeHotspotAt(int x, int y) const;
    // Developer control API for "beemaze:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBeemazeDeveloperCommand(std::string_view command);

    std::optional<BeemazeGame> beemazeGame;

private:
    int beemazeRandom();
    int beemazeClock();
    double beemazeNow();
    int beemazePassClock() const;
    void beemazeRunPass(double passTime);
    void beemazeServiceTimers(int clock);
    int beemazeArmTimer(int due, int tag);
    void beemazeCancelTimer(int &timer);
    void beemazeClearTimers();
    void beemazeTimer(int tag);
    void beemazeTick(int clock);
    void beemazeAmbientTick(int clock);
    void beemazeStartLevel();
    void beemazeRestartLevel();
    void beemazePlaceSmokers();
    void beemazePlaceItems();
    void beemazePlaceItem(int x, int y);
    void beemazePlaceHives();
    void beemazeStartLoopingAmbient();
    void beemazePlaceBees();
    void beemazePickup(int cellX, int cellY);
    void beemazePlayClone(std::string_view base, int variants);
    void beemazeCloneAnimation(const std::string &asset);
    void beemazeMovePlayer(int clock);
    std::pair<int, int> beemazeStepPlayer(int x, int y, int step);
    void beemazeFollowSmoke();
    void beemazeMoveBees(int clock);
    std::pair<int, int> beemazeStepBee(int bee, int step, int mode);
    int beemazePanicDirection(int bee) const;
    int beemazeTrailDirection(int cellX, int cellY) const;
    int beemazePissedDirection(int bee) const;
    int beemazeRandomDirection(int bee);
    bool beemazeBeeOnTrail(int cellX, int cellY) const;
    void beemazeSmokeBees(int clock);
    bool beemazeBeeCaughtPlayer() const;
    void beemazeLoseLife();
    void beemazeFinishClips(double now);
    void beemazeDrawScore();
    void beemazeBlitPlayer(int column, int row, int sheet);
    void beemazeAddClip(BeemazeClip &clip, std::string_view asset, int x,
                        int y, int layer, std::uint32_t hotspot = 0U);
    void beemazeRemoveClip(BeemazeClip &clip, std::string_view asset);
    void beemazePlayClip(BeemazeClip &clip);
    void beemazeTraceState(std::string_view state);

    // Process-lifetime .data/.bss state that the scene initializer at
    // 0x00432d50 does not clear: the C runtime rand() state, the bee
    // direction table at 0x004cd780 (initialised to 2), the score at
    // 0x006584c0 and the last drawn score at 0x006584e8.
    std::uint32_t beemazeRandomState{};
    bool beemazeRandomSeeded{};
    std::optional<std::uint32_t> beemazeFixedSeed;
    std::array<int, 7> beemazeBeeDirections{2, 2, 2, 2, 2, 2, 2};
    int beemazeScore{};
    int beemazeDrawnScore{};
};

} // namespace fgp
