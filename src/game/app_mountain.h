#pragma once

#include "game/app_core.h"

namespace fgp {

// The "mountain" minigame: its state and logic.
// Implemented in app_mountain.cpp, a port of FGP.exe 0x0043fd90-0x00448410.
// The original runs at 640x480 (0x004442bf); every coordinate below is in
// that space and the frame is scaled to the 800x600 window when drawn.
class MountainScene : protected virtual AppCore {
protected:
    // A Smacker player (0x00403960 PLAYER_Create): the decoded clip, its
    // keyed upload texture and the play state the original keeps at +0xc
    // (playing) and in Smack->FrameNum.
    struct MountainClip {
        std::string name;
        DecodedMedia media;
        TexturePtr texture;
        bool playing{};
        int frame{};
        double frameStartedAt{};
    };
    // A sound loaded through SOUND_Load (0x004139c0).
    struct MountainSound {
        std::string name;
        DecodedMedia media;
        std::uint64_t handle{};
        double startedAt{};
        bool looping{};
        bool playing{};
    };
    // One 32x32 tile cut from the tile set by 0x0044356b, with the 16-bit
    // colour key mask used by the collision stamp 0x004471a3.
    struct MountainTile {
        std::array<bool, 32 * 32> opaque{};
        bool any{};
    };
public:
    // Track graph records (.fff), public for the file-local geometry helpers.
    struct MountainNode {
        float x{};
        float y{};
        int type{};
        std::vector<int> links;
        std::array<int, 2> out{{-1, -1}};
        int outCount{};
        std::array<int, 2> in{{-1, -1}};
        int inCount{};
    };
    struct MountainEdge {
        int from{};
        int to{};
        float dx{};
        float dy{};
        float nx{};
        float ny{};
        float angle{};
    };

protected:
    struct MountainItem {
        int x{};
        int y{};
        int kind{};
        int left{};
        int top{};
        int right{};
        int bottom{};
        int startTick{};
        bool active{true};
    };
    // A moving object (0xbc bytes, built by 0x00442de3): the milk truck
    // (node types 4->5) or the walking postman (6->7).
    struct MountainActor {
        float x{};
        float y{};
        float speed{};
        float baseSpeed{};
        bool upright{};
        int walkClip{-1};
        int fallClip{-1};
        bool hit{};
        float angle{};
        int spawnNode{};
        int triggerNode{};
        bool active{};
        int edge{-1};
        std::array<std::array<float, 4>, 6> hitbox{};
        const float *outline{};
        int sound{-1};
        float spriteHalfW{};
        float spriteHalfH{};
        float spriteW{};
        float spriteH{};
    };
    // Obstacle tile record m+0x88 (stride 0x14), refilled by the world draw.
    struct MountainSolid {
        int left{};
        int top{};
        int right{};
        int bottom{};
        int tile{-1};
    };
    struct MountainMap {
        int widthTiles{};
        int heightTiles{};
        int widthPx{};
        int heightPx{};
        int startX{10};
        int finishX{100};
        std::array<std::vector<std::uint16_t>, 4> layers;
        std::vector<MountainTile> tiles;
        SurfacePtr tileSurface;
        TexturePtr tileTexture;
        std::uint32_t tileKey{};
        int tileColumns{8};
        int camX{320};
        int camY{240};
        std::vector<MountainNode> nodes;
        std::vector<MountainEdge> edges;
        std::vector<MountainItem> items;
        std::vector<MountainActor> actors;
        std::vector<int> trucks;
        std::vector<int> walkers;
        int truckChoice{};
        int walkerChoice{};
        int startEdge{0};
        std::vector<MountainSolid> solids;
    };
    struct MountainPlayer {
        float x{};
        float y{};
        float vx{};
        float vy{};
        float gravityResidual{};   // +0x18
        float launchVy{};          // +0x1c
        float flightT0{};          // +0x20
        float mass{50.0F};         // +0x24
        bool airborne{};           // +0x28
        bool falling{};            // +0x2c
        bool ducking{};            // +0x30
        bool duckClip{};           // +0x34
        bool crashed{};            // +0x38
        bool crashClipStarted{};   // +0x3c
        int crashType{};           // +0x40
        int edge{-1};              // +0x58
        int checkpoint{-1};        // +0x5c
        int upTime{-1000};         // +0x6c
        bool started{};            // +0x70
        bool finished{};           // +0x74
        int startedAt{};           // +0x78
        int finishedAt{};          // +0x7c
        int score{};               // +0x80
        int lives{3};              // +0x84
        std::array<int, 2> collected{};          // +0x98
        std::array<int, 3> slots{{-1, -1, -1}};  // +0xa0
        float angle{};             // +0xdc
        float jumpAngle{};         // +0xe0
        std::array<std::array<float, 4>, 5> hitbox{};  // +0x18c
        bool jumpPressed{};        // +0x1ec
        // Clip queue +0x104/+0x184/+0x188.
        std::array<std::pair<int, bool>, 16> queue{};
        int queueHead{-1};
        int queueTail{};
        float spriteHalfW{};
        float spriteHalfH{};
        float spriteW{};
        float spriteH{};
    };
    // A control created through 0x00406ed0/0x00406fb0; traced as an anim
    // named after its current image (0x00406760).
    struct MountainControl {
        int id{};
        int x{};
        int y{};
        int layer{};
        std::array<std::string, 2> images;
        int frame{};
        bool radio{};
        bool active{};
        std::string traced;
    };
    // A text control (0x004078c0): an unnamed 0xa8-byte descriptor.
    struct MountainText {
        SDL_Rect rect{};
        int layer{};
        std::string text;
        int flags{};
        bool active{};
    };
    enum class MountainClipSlot {
        Cycle, InAir, Jump, Land, Duck, Crash2, Crash1, Count
    };

    struct MountainGame {
        // Scene-level state (init 0x00440d6e).
        bool initialized{};
        bool attract{true};          // g_4dc020
        bool running{};              // g_678b18
        int difficulty{1};           // g_678afc
        int track{};                 // g_678b08
        int loadedTrack{-1};         // g_678b04
        int loadedDifficulty{-1};    // g_678b00
        bool takeOff{};              // g_678a3c
        int collisionStamp{};        // g_67833c
        std::vector<std::uint32_t> stamp;   // g_678b0c, 640x480
        std::array<float, 300> flight{};    // g_67844c
        int frameMs{};               // g_676d68
        int previousFrameMs{};       // g_676d6c
        float dt{};                  // g_6782b0
        double tickBase{};
        int tickOffset{};
        bool subgameInProgress{};
        bool gameOverModal{};
        // maps.ini sections in file order, and the current track's keys.
        std::vector<std::pair<std::string,
                              std::vector<std::pair<std::string, std::string>>>>
            tracks;
        std::array<std::string, 3> mapNames;
        std::string fffName;
        std::string effName;
        std::string postWalk;
        std::string postCrash;
        int soundMode{};
        std::string interfaceName{"interface"};
        // Media.
        std::optional<MountainMap> map;
        std::optional<MountainPlayer> player;
        std::vector<MountainClip> clips;
        std::array<int, static_cast<std::size_t>(MountainClipSlot::Count)>
            playerClips{};
        std::vector<MountainSound> sounds;
        int soundRoll{-1};
        int soundDuringJump{-1};
        int soundJump{-1};
        int soundGround{-1};
        int soundFanfare{-1};
        std::array<int, 7> pickupSounds{};
        std::array<SurfacePtr, 7> pickupSurfaces;
        std::array<TexturePtr, 7> pickupTextures;
        std::array<TexturePtr, 3> slotIconTextures;
        SurfacePtr interfaceSurface;
        TexturePtr interfaceTexture;
        TexturePtr startInterfaceTexture;
        TexturePtr progressTexture;
        TexturePtr headTexture;
        std::unordered_map<std::string, TexturePtr> controlTextures;
        TexturePtr frameTarget;
        // Scene anims and controls.
        std::vector<MountainControl> controls;
        int pressedControl{};
        bool interfaceAdded{};
        bool startInterfaceAdded{};
        std::array<bool, 3> headsAdded{};
        std::array<bool, 3> slotIconsAdded{};
        std::array<std::string, 3> slotIconNames;
        MountainText timeText;
        MountainText scoreText;
        bool progressAdded{};
        float progress{};
        MountainText easyText;
        MountainText mediumText;
        MountainText hardText;
        MountainText startText;
        MountainText trackText;
        // Developer pins.
        std::optional<int> pinnedTruck;
        std::optional<int> pinnedWalker;
        std::optional<double> developerWaitUntil;
        std::string developerSnapshot;
        std::array<double, 5> developerKeysUntil{};  // L, R, U, D, Space
    };

    MountainGame loadMountainGame(Package &package);
    bool handleMountainKey(SDL_Keycode key, bool pressed);
    bool beginMountainInteraction(int x, int y);
    bool endMountainInteraction(int x, int y);
    bool mountainPressing() const {
        return mountainGame && mountainGame->pressedControl != 0;
    }
    void updateMountain(double elapsed);
    void renderMountain(double elapsed);
    // The score table returned (0x00447e43 is blocking in the original);
    // the frame handler then rebuilds the attract screen (0x004447fb).
    void finishMountainGameOver();
    // Tournament start (0x00440d6e -> 0x0044108e): the round's track and
    // the player's difficulty, one life, no attract panel.
    void startMountainGame();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        mountainHotspotAt(int x, int y) const;
    // Developer control API for "mountain-game:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeMountainDeveloperCommand(std::string_view command);

    std::optional<MountainGame> mountainGame;

private:
    std::span<const std::uint8_t> mountainAsset(std::string_view name) const;
    const std::uint8_t *mountainAssetOptional(std::string_view name,
                                              std::size_t &size) const;
    int mountainClock() const;
    int mountainTicks() const;
    void parseMountainIni(std::span<const std::uint8_t> bytes);
    std::string mountainKey(std::string_view key,
                            std::string_view fallback) const;
    void readMountainTrackKeys();
    int loadMountainClip(const std::string &name);
    int loadMountainSound(const std::string &name);
    void playMountainSound(int sound, bool loop);
    void stopMountainSound(int sound);
    bool mountainSoundPlaying(int sound);
    void playMountainSoundPooled(int sound);
    void startMountainClip(int clip);
    void stopMountainClip(int clip);
    void advanceMountainClip(int clip);
    void loadMountainMap();
    void reloadMountainTiles();
    void loadMountainPlayer();
    void traceMountainAdd(std::string_view asset, int x, int y, int layer);
    void traceMountainRemove(std::string_view asset);
    void traceMountainState(std::string_view state,
                            std::string_view firstName = {},
                            int firstValue = 0,
                            std::string_view secondName = {},
                            int secondValue = 0);
    void createMountainControl(int id, int x, int y, int layer,
                               std::string off, std::string on, bool radio);
    void setMountainControlFrame(MountainControl &control, int frame);
    void destroyMountainControl(int id);
    void traceMountainPickup(int kind, int item, int score);
    MountainControl *findMountainControl(int id);
    void createMountainText(MountainText &text, int x, int y, int w, int h,
                            int layer);
    void destroyMountainText(MountainText &text);
    void initializeMountainScene();
    void enterMountainAttract();
    void startMountainRun();
    void beginMountainLife();
    void updateMountainHeads();
    void placeMountainPlayer();
    void resetMountainActors();
    void mountainClipQueueReset();
    bool mountainClipQueueEmpty() const;
    void mountainClipPlayNow(int clip, bool loop);
    void mountainClipEnqueue(int clip, bool loop);
    void advanceMountainPlayerClip();
    void mountainFrame();
    float mountainPlayerSpeed() const;
    void mountainSetSpeed(float target);
    void mountainSpeedCruise();
    void mountainSpeedLeft();
    void mountainSpeedRight();
    void mountainSpeedClamp();
    void mountainDuckStart();
    void mountainGravityResidual();
    void mountainCameraFollow();
    void mountainJumpStart();
    void mountainBuildFlight();
    float mountainFlightY(float t) const;
    bool mountainUpdatePlayer();
    void mountainUpdateActors();
    void mountainBuildHitbox();
    void mountainBuildActorHitbox(MountainActor &actor);
    int mountainCollide();
    void mountainStampTile(const MountainSolid &solid);
    bool mountainLineHitsStamp(int x0, int y0, int x1, int y1) const;
    bool mountainLoseLife();
    void mountainGameOver();
    void mountainPickups();
    void mountainAddScore(int points);
    void mountainSlotAdd(int item);
    void mountainSlotsCheck();
    bool mountainFinishCheck();
    void mountainUpdateHud();
    float mountainElapsedSeconds() const;
    void mountainEndOfGame();
    void collectMountainSolids();
    void drawMountainSprite(MountainClip &clip, float cx, float cy,
                            float angle, float scale, float halfW,
                            float halfH, float width, float height);
    void drawMountainText(const MountainText &text);
};

} // namespace fgp
