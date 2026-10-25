#pragma once

#include "game/app_core.h"

namespace fgp {

// The "buildabike" minigame: its state and logic.
// Implemented in app_buildabike.cpp, a port of FGP.exe 0x00437d10-0x0043a210.
class BuildabikeScene : protected virtual AppCore {
protected:
    // One original 0xa8-byte animation descriptor (or a text/progress
    // control's descriptor). Every SCENE_AddAnim/RemoveAnim/StopAnim call
    // the original makes on it is mirrored in the reference trace.
    enum class BuildabikeAnimKind { Bitmap, Smacker, Text, Progress };
    struct BuildabikeAnim {
        std::uint32_t address{};
        BuildabikeAnimKind kind{BuildabikeAnimKind::Bitmap};
        std::string name;
        int defaultX{};
        int defaultY{};
        int defaultLayer{};
        std::uint32_t hotspot{};
        bool visible{true};     // descriptor +0x44
        bool transparent{true}; // descriptor +0x48
        bool loop{};            // descriptor +0x4c
        bool clone{};
        bool active{};          // +0x7c
        bool playing{};         // +0x78
        int x{};
        int y{};
        int layer{};
        std::uint64_t order{};
        int hitMode{};          // +0x9c
        int hitValue{};         // +0xa0
        std::string hitMask;    // +0xa4, an hs bitmap name
        double playStarted{};
        double frozenAt{};
        std::size_t audioLoops{};
        std::uint64_t audioHandle{};
        // Text and progress controls.
        SDL_Rect rect{};
        std::string text;
        int textFlags{};
        float progress{};
    };
    struct BuildabikeImage {
        SurfacePtr surface;
        TexturePtr texture;
        int width{};
        int height{};
        Uint32 key{};
    };
    struct BuildabikeClip {
        DecodedMedia media;
        TexturePtr texture;
        double completion{};
    };
    struct BuildabikePlatform {
        int state{4};        // +0xa8: 0 lowering, 1 rising, 3 down, 4 up
        float position{};    // +0xac: 0 raised, 1 lowered
        float timer{};       // +0xb0
        bool hasBike{};      // +0xb4
        bool pendingBike{};  // +0xb8
        int serial{};        // +0xbc
        std::uint16_t repairMask{}; // +0xc0
    };
    struct BuildabikeQueueEntry {
        std::string name;
        float timer{};
        bool active{};
    };
    struct BuildabikeLine {
        float progress{};
        int counter{};
    };
    struct BuildabikeGame {
        std::unordered_map<std::string, BuildabikeImage> images;
        std::unordered_map<std::string, BuildabikeClip> clips;
        BitmapFont counterFont;
        BuildabikeAnim points;
        std::array<BuildabikeAnim, 3> timerTexts;
        BuildabikeAnim energy;
        BuildabikeAnim scoreText;
        std::array<BuildabikeAnim, 11> countTexts;
        std::array<BuildabikeAnim, 3> platformAnims;
        BuildabikeAnim cover;
        BuildabikeAnim lcogwheel;
        BuildabikeAnim rcogwheel;
        BuildabikeAnim wheel;
        BuildabikeAnim lever;
        BuildabikeAnim string;
        BuildabikeAnim machine;
        BuildabikeAnim drag;
        BuildabikeAnim start;
        std::array<BuildabikeAnim, 11> walls;
        std::array<BuildabikeAnim, 27> slots;
        std::array<BuildabikeAnim, 11> lines;
        std::vector<BuildabikeAnim> clones;
        std::array<BuildabikePlatform, 3> platforms;
        std::array<BuildabikeQueueEntry, 22> queue;
        std::array<BuildabikeLine, 11> conveyor;
        std::array<int, 11> wallCounts{};
        int queueHead{};
        int dragSource{};
        int dragTag{};
        int dragOffsetX{};
        int dragOffsetY{};
        bool dragging{};
        bool initialized{};
        bool gameOverModal{};
        bool playing{};
        int bikes{};
        int score{};
        int health{};
        int maximumHealth{};
        int recycleSeconds{25};
        float shownScore{};
        float shownHealth{};
        double startedAt{};
        double lastUpdate{};
        std::uint64_t nextOrder{};
        std::optional<int> pinnedRandom;
        std::optional<double> developerWaitUntil;
    };

    BuildabikeGame loadBuildabikeGame(Package &package);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        buildabikeHotspotAt(int x, int y) const;
    bool beginBuildabikeInteraction(int x, int y);
    void moveBuildabikeInteraction(int x, int y);
    void endBuildabikeInteraction(int x, int y);
    void updateBuildabike(double elapsed);
    void renderBuildabike(double elapsed);
    // The game-over modal returned (0x00438d4e); run the score handoff.
    void finishBuildabikeGameOver();
    bool buildabikeDragging() const {
        return buildabikeGame && buildabikeGame->dragging;
    }
    // Developer control API for "buildabike:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBuildabikeDeveloperCommand(std::string_view command);

    std::optional<BuildabikeGame> buildabikeGame;

    // Reached by GameApp::startTournamentRound as well.
    void startBuildabikeGame();
private:
    void initializeBuildabikeScene();
    int buildabikeRandom(int bound);
    void addBuildabikeAnim(BuildabikeAnim &anim, int x, int y, int layer);
    void removeBuildabikeAnim(BuildabikeAnim &anim);
    void stopBuildabikeAnim(BuildabikeAnim &anim);
    void playBuildabikeAnim(BuildabikeAnim &anim);
    void addAndPlayBuildabikeAnim(BuildabikeAnim &anim);
    void moveBuildabikeAnim(BuildabikeAnim &anim, int x, int y);
    void setBuildabikeHit(BuildabikeAnim &anim, int mode, int value,
                          std::string mask = {});
    void createBuildabikeTextControl(BuildabikeAnim &anim);
    void playBuildabikeSound(std::string_view name);
    void finishBuildabikeClip(BuildabikeAnim &anim);
    bool buildabikeAnimHit(const BuildabikeAnim &anim, int x, int y,
                           std::uint32_t &hotspot) const;
    std::uint32_t resolveBuildabikeHotspot(int x, int y,
                                           std::uint32_t exclude) const;
    void setBuildabikePlatformPosition(int platform, float position);
    void setBuildabikePlatformState(int platform, int state);
    bool scoreBuildabikeBike(int platform, bool apply);
    void removeBuildabikeParts(int platform);
    void createBuildabikePart(int platform, int slot, std::string name);
    void newBuildabikeBike(int platform);
    float buildabikeTimerSeconds() const;
    int buildabikeBikeLimit() const;
    void setBuildabikeWallCount(int kind, int count);
    void updateBuildabikePlatform(int platform, float dt);
    void scheduleBuildabikeBikes(float dt);
    void updateBuildabikeTimers();
    void updateBuildabikeConveyor(float dt);
    void updateBuildabikeMachine(float dt);
    void updateBuildabikeScore(float dt);
    void updateBuildabikeEnergy(float dt);
    bool enqueueBuildabikePart(const std::string &name);
    std::optional<std::string> takeBuildabikeRecycledPart();
    void buildabikeGameOver();
    void pressBuildabike(std::uint32_t hotspot, int x, int y);
    void beginBuildabikeDrag(std::string name, int source, int tag, int x,
                             int y);
    bool dropBuildabikeOnBike(std::uint32_t hotspot);
    bool dropBuildabikeInMachine();
    bool dropBuildabikeOnWall(std::uint32_t hotspot);
    void returnBuildabikeDrag();
    std::vector<BuildabikeAnim *> buildabikeAnims();
    std::vector<const BuildabikeAnim *> buildabikeAnims() const;
};

} // namespace fgp
