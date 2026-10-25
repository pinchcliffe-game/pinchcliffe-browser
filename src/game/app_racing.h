#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/activity_panel.h"
#include "game/app_core.h"
#include "game/racing_data.h"
#include "game/racing_ai.h"
#include "game/racing_physics.h"

namespace fgp {

// The race ("racing", scene 21): a real-time simulation in a 640x480 frame,
// entered from the racing menu with a parameter block or from the story's
// TV station without one. Implemented in app_racing.cpp from FGP.exe
// 0x0046f0f0-0x0047a980; see "Racing" in REVERSE_ENGINEERING.md.
//
// This first piece (issue 250) loads a race, shows the grid with the HUD
// through the countdown and leaves with Escape. Nothing moves yet.
class RacingScene : protected virtual AppCore {
protected:
    // BITMAP_LoadSprite 0x0046f0f0: the bitmap with its own top-left pixel
    // as the colour key.
    struct RaceSprite {
        SurfacePtr surface;
        int width{};
        int height{};
    };
    // A car (0x29c bytes from RACE_LoadCar 0x00475ce0) and its slot.
    struct RaceCar {
        std::string resource;
        std::string name;      // +0x04: race:DRIVER<id>, or the profile's
        racing::CarSpec spec;
        std::array<RaceSprite, 4> sprites; // damage states, %s0..3.bmp
        RaceSprite shadow;
        // +0x20..+0x34: the sprite's rectangle in the two atlases.
        float halfWidth{};
        float halfHeight{};
        int atlasX{};
        int atlasY{};
        int width{};
        int height{};
        // The dynamic state (racing_physics.h).
        racing::PhysicsCar body;
        // +0x1b4 and +0x1d4: the wheels this frame and last, for the skid
        // marks (0x00476780, 0x004767f0).
        std::array<std::array<float, 2>, 4> wheels{};
        std::array<std::array<float, 2>, 4> wheelsBefore{};
        int position{};        // +0x20c
        int lap{1};            // +0x200
        std::array<std::uint32_t, 8> lapTimes{}; // +0x234
        double lapStartMs{};   // +0x254
        // Pickups (0x00476810, 0x004798f0, 0x00479a80).
        int held{};            // +0x224
        int trick{};           // +0x228, the trick in use
        int trickLeft{};       // +0x22c
        float trickTimer{};    // +0x230
        float smokeTimer{};    // +0x1b0
        double lastCrashMs{};  // +0x214
        std::uint32_t bestLap{racing::kNoTime}; // +0x258
        bool human{};
    };
    // A SCENE descriptor holding a Smacker clip (count, the message clip).
    struct RaceClip {
        int sprite{-1};
        std::string name;
        std::vector<std::uint8_t> bytes;
        DecodedMedia media;
        int shown{-1};
        bool playing{};        // +0x78
        bool loop{};           // +0x4c
        double startedAt{};    // race clock, ms
        double completesAt{};
    };
    // An overlay piece queued to be drawn over the cars (0x00478b70).
    struct OverlayDraw {
        int screenX{};
        int screenY{};
        SDL_Rect source{};
        int overlay{};
        int ontop{};
    };
    // A pickup or a hazard in the object pool (0x24 bytes at 0x006cbea0).
    struct RaceObject {
        float x{};
        float y{};
        bool active{};
        bool pickup{};         // +0xc: 1 a pickup, 0 a hazard left by a trick
        int type{};            // +0x10, the pickup type 1..6
        int particle{};        // +0x14, a hazard's particle type (2 oil, 5 spike)
        int node{-1};          // +0x1c
        int owner{};           // +0x20
    };
    // A particle (0x20 bytes at 0x006c9d58, 128 of them).
    struct RaceParticle {
        int x{};
        int y{};
        bool active{};
        int owner{};
        double startMs{};
        float frame{};
        int type{};            // 0x0050aa30's index
        float speed{};
    };
    struct RacingGame {
        racing::RaceBlock block;
        racing::TrackSpec track;
        SurfacePtr trackSurface;      // track +0x10
        int trackWidth{};
        int trackHeight{};
        std::vector<RaceSprite> overlays;
        std::vector<racing::Segment> collision;
        racing::CollisionWorld world;
        racing::AiPath aiPath;
        // The slot records (0x006cad68) and what the AI reads of each car;
        // aiCars point into cars and aiSlots, which never grow after load.
        std::vector<racing::AiSlot> aiSlots;
        std::vector<racing::AiCar> aiCars;
        // When each car's horn stops sounding (the AI waits on it).
        std::vector<double> hornUntilMs;
        std::vector<double> hornLengthMs;
        // terrain.bmp, a quarter-scale index map (0x0047a0b0).
        std::vector<std::uint8_t> terrain;
        int terrainWidth{};
        int terrainHeight{};
        // Player 1's six keys as the control setup polls them (0x00479c10):
        // accelerate, brake, left, right, use, horn.
        std::array<bool, 6> keys{};
        std::vector<racing::PathNode> path;
        std::vector<racing::SoundEmitter> sounds;
        std::vector<RaceCar> cars;
        SurfacePtr carAtlas;          // 0x006cad58
        SurfacePtr shadowAtlas;       // 0x006cad60
        std::vector<OverlayDraw> overlayDraws;
        int cameraX{};                // track +0x1c
        int cameraY{};                // track +0x20
        // The 640x480 back buffer and its presentation.
        SurfacePtr frame;
        TexturePtr frameTexture;
        TexturePtr target;
        std::unique_ptr<ActivityStage> stage;
        BitmapFont font12;            // arial_12_  (0x006c6448)
        BitmapFont font16;            // arial_16_b (0x006ce908)
        BitmapFont font20;            // arial_20_b (0x006cae40)
        // HUD record 0 (0x0050ace8).
        int pickupIcon{-1};           // +0x04
        // Pickups and tricks (issue 254).
        std::array<RaceSprite, 7> pickupSprites;   // pickup1..6 (0x006c7550)
        std::array<RaceSprite, 6> slotSprites;     // slot0..5 (0x006c748c)
        std::array<RaceSprite, 6> particleSprites; // 0x0050aa30's bitmaps
        std::vector<RaceObject> objects;
        std::array<RaceParticle, 128> particles{};
        int randomPickups{};          // 0x006cf968
        double spawnDueMs{};          // timer 2
        bool spawning{};
        bool hiddenPartTaken{};       // 0x006c7568
        int windX{};                  // 0x006c6440
        int windY{};                  // 0x006c6444
        RaceClip message;             // +0xac
        int minimap{-1};              // +0x154
        RaceClip count;               // +0x1fc
        std::array<int, 4> dots{-1, -1, -1, -1}; // +0x398
        std::string messageLine;      // +0x354
        int messageMs{};              // +0x394
        int blink{};                  // 0x006cf974
        int blinkLast{};              // 0x006cf970
        int state{3};                 // 0x006c7484
        double clockMs{};             // 0x0040c450 as the race sees it
        double lastMs{};              // 0x006cf960
        float dt{1.0F};               // 0x0050a974
        double startMs{};             // 0x006cbe78
        bool ended{};
        bool pendingBuild{};
        std::uint64_t frames{};
        // The scene clock restarts once the scene has loaded; the first
        // frame moves everything taken from it at load onto the new base.
        bool clockStarted{};
        // Timer 1, the lap clock (0x006ce8d0).
        bool lapClock{};
        // The end of a finished race (0x00477390): the track award's box,
        // then the roster (0x00421a70), then the racing menu.
        enum class Ending { None, HiddenPart, Award, Roster } ending{Ending::None};
        BitmapFont rosterTitleFont;   // Amerigo BT_10_b
        BitmapFont rosterNameFont;    // Amerigo BT_10_b
        BitmapFont rosterButtonFont;  // Amerigo BT_10_
        BitmapFont rosterTimeFont;    // LCD_14_
        std::vector<int> rosterSprites;
        SDL_Rect rosterButton{};
        bool rosterButtonDown{};
    };

    // Scene load: the race is built on its first frame (buildRace), after
    // the scene change is announced, as the original's 0x103 handler runs
    // after GAME_GotoScene's switch.
    bool loadRacingGame();
    bool handleRacingEvent(const SDL_Event &event);
    void updateRacing(double elapsed);
    void renderRacing();
    // The racing menu's Start (0x0047bfa0) leaves the block here.
    void confirmRacingQuit();
    // After race:CRASHED closes.
    void finishCrashedRace();
    // Developer control API for "racing:..." commands.
    bool executeRacingDeveloperCommand(std::string_view command);

    std::optional<RacingGame> racingGame;
    // 0x006cf964: the HUD detail level, never initialised, so it starts at
    // 0 and lasts the session.
    int racingHudLevel{};

private:
    void buildRace();
    std::span<const std::uint8_t> racingAsset(const std::string &asset);
    std::optional<std::span<const std::uint8_t>> findRacingAsset(
        const std::string &asset);
    RaceSprite loadRaceSprite(const std::string &asset);
    void loadRaceClip(RaceClip &clip, const std::string &name);
    void addRaceClip(RaceClip &clip, int layer);
    void showRaceClipFrame(RaceClip &clip);
    void setRaceMessageClip(const std::string &name, bool loop = false);
    void crossTrigger(std::size_t slot);
    void countLap(std::size_t slot);
    void resetRace();
    void drawRaceView();
    void drawRaceCar(RaceCar &car);
    void queueOverlaysOver(SDL_Rect rect);
    void drawRaceHud();
    void moveRaceDots();
    void endRace();
    void beginRaceResults();
    void openRaceRoster();
    void closeRaceRoster();
    void finishRaceResults();
    int raceStageText(const std::string &name, const SDL_Rect &rect, const BitmapFont &font,
                      const std::string &text, int layer);
    void updateRaceCar(std::size_t slot);
    void sampleTerrain(RaceCar &car);
    void drawSkidMarks(RaceCar &car);
    void refreshCarSprite(RaceCar &car);
    racing::AiWorld raceAiWorld();
    void sortRacePositions();
    void holdPickup(std::size_t slot, int type);
    void usePickup(std::size_t slot);
    void runTrick(std::size_t slot);
    void collectPickups(std::size_t slot);
    void spawnPickup(int type);
    void placePickup(int node, int type);
    void spawnParticle(int x, int y, int type, int owner);
    void updateParticles();
    void drawParticles(int layer);
    void drawObjects();
    double raceClockNow();
};

} // namespace fgp
