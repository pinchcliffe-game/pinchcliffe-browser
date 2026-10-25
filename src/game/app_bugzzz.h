#pragma once

#include "game/app_core.h"

namespace fgp {

// The "bugzzz" minigame: its state and logic.
// Implemented in app_bugzzz.cpp. Player slots follow FGP.exe's own order
// (green, blue, red, yellow), which is also the order of the lobby panels,
// the hotspot mask and the per-player records at 0x00659CD0.
class BugzzzScene : protected virtual AppCore {
protected:
    // Lobby: the pre-Start composition. Starting: the 150 ms between the
    // Start press and the board rebuild (0x00437BA0). Playing: the per-frame
    // dispatcher body at 0x004331DA. Timeout: the woodpecker has finished the
    // branch (0x00435F50 sets 0x00661FC8). Scrolling: the climb to the next
    // board (0x00436EA0 / 0x00433E40). Finished: game over, waiting for the
    // score dialog to close.
    enum class BugzzzPhase { Lobby, Starting, Playing, Timeout, Scrolling,
                             Finished };
    // The executable's pickup types 1..9 (0x004338F0 switch at 0x00433B88).
    enum class BugzzzPickupKind : std::uint8_t {
        YellowAnt = 1,
        RedAnt = 2,
        BrownAnt = 3,
        WhiteMushroom = 4,
        BlackMushroom = 5,
        RedMushroom = 6,
        BlueMushroom = 7,
        GreenMushroom = 8,
        Life = 9,
    };
    enum class BugzzzPickupState : std::uint8_t {
        Waiting, Appearing, Walking, Stopped, Static, Exploding, Gone,
    };
    // One 0x120-byte sprite-set record from 0x004D1158.
    struct BugzzzSpriteSet {
        TexturePtr texture;
        int frames{};
        int size{};
        bool oneShot{};
        bool directional{};
        double rate{1.0};
        int rowShift{};
    };
    struct BugzzzLevel {
        std::array<int, 8> counts{};
        int seconds{};
        int antQuota{};
        float wormSpeed{};
        float antSpeed{};
    };
    struct BugzzzPickup {
        BugzzzPickupKind kind{BugzzzPickupKind::YellowAnt};
        BugzzzPickupState state{BugzzzPickupState::Waiting};
        double x{};
        double y{};
        double heading{};
        double speed{};
        int spawnSecond{};
        long long stateTick{};
        bool exploded{};
    };
    struct BugzzzPopup {
        int points{};
        double x{};
        double y{};
        long long startTick{};
    };
    struct BugzzzSpit {
        std::size_t owner{};
        double x{};
        double y{};
        double dx{};
        double dy{};
    };
    struct BugzzzWorm {
        bool selected{};
        bool playing{};
        int lives{};
        int score{};
        int antsEaten{};
        int spits{};
        int frags{};
        double x{};
        double y{};
        double heading{};
        double speed{};
        double anchorX{};
        double anchorY{};
        // Chain joints, newest first; joints[0] is the head's last 11 px
        // step point (worm +0x24/+0x28 and the +0x1C node list).
        std::vector<SDL_FPoint> joints;
        std::vector<double> jointHeadings;
        int nodes{7};
        bool dead{};
        long long deathTick{};
        bool eating{};
        long long eatTick{};
        bool invulnerable{};
        long long invulnerableUntilTick{};
        bool poisoned{};
        bool speedEffect{};
        int speedEffectSecond{};
        long long lastSpitTick{-1};
    };
    // Lobby/HUD panel slot state machine at 0x00661210 (0x00433C20).
    struct BugzzzPanel {
        int state{1};
        std::string current;
        std::string pending;
        double changedAt{};
    };
    struct BugzzzGame {
        std::array<BugzzzLevel, 9> levels;
        std::array<TexturePtr, 2> middleTextures;
        TexturePtr bottomTexture;
        TexturePtr difficultyBackdropTexture;
        TexturePtr helpLeafTexture;
        TexturePtr exitLeafTexture;
        std::array<SurfacePtr, 2> treeMasks;
        TexturePtr woodBorderTexture;
        std::array<TexturePtr, 4> playerOnTextures;
        std::array<TexturePtr, 4> playerOffTextures;
        std::array<TexturePtr, 4> playerInfoTextures;
        std::array<TexturePtr, 4> playerTextTextures;
        std::array<TexturePtr, 4> playerLeafTextures;
        std::array<TexturePtr, 2> startTextures;
        std::array<TexturePtr, 31> branchTextures;
        std::array<BugzzzSpriteSet, 4> bodySets;
        BugzzzSpriteSet headSet;
        BugzzzSpriteSet eatSet;
        BugzzzSpriteSet dieSet;
        BugzzzSpriteSet explodeSet;
        BugzzzSpriteSet spitSet;
        BugzzzSpriteSet lifeSet;
        // Indexed by pickup type 1..9 (index 0 unused).
        std::array<BugzzzSpriteSet, 10> appearSets;
        std::array<BugzzzSpriteSet, 10> mainSets;
        std::array<BugzzzSpriteSet, 10> stopSets;
        std::array<BugzzzSpriteSet, 10> burstSets;
        std::array<BugzzzSpriteSet, 3> popupSets;
        HopscotchMedia birdIn;
        HopscotchMedia birdPeck;
        HopscotchMedia birdFall;
        HopscotchMedia birdTimeout;
        BitmapFont hudFont;
        SurfacePtr hotspotMask;
        BugzzzPhase phase{BugzzzPhase::Lobby};
        std::array<BugzzzWorm, 4> worms;
        std::array<BugzzzPanel, 4> panels;
        std::vector<BugzzzPickup> pickups;
        std::vector<BugzzzPopup> popups;
        std::vector<BugzzzSpit> spits;
        int level{};
        double clockOrigin{};
        double startAt{};
        double lastUpdate{};
        double scrollStarted{};
        double birdStarted{};
        int deadlineSecond{};
        int branchIndex{-1};
        int bonusThreshold{300};
        std::string birdClip;
        bool pressedStart{};
        bool gameOverShown{};
        bool pendingTimeoutRestart{};
        bool pendingLevelScroll{};
        bool pendingLobby{};
    };

    BugzzzGame loadBugzzzGame(Package &package);
    void buildBugzzzLobby(BugzzzGame &game, bool initial);
    void pressBugzzzStart();
    void startBugzzzGame();
    void setupBugzzzLevel();
    void resetBugzzzWorm(std::size_t worm);
    void beginBugzzzLevel(bool retainScore = true);
    bool handleBugzzzKey(SDL_Keycode key);
    bool pressBugzzzHotspot(std::uint32_t hotspot);
    void toggleBugzzzPlayer(std::size_t slot);
    void setBugzzzPanel(std::size_t slot, std::string descriptor);
    void updateBugzzzPanels(double now);
    void turnBugzzzWorm(std::size_t worm, int direction);
    void spitBugzzzWorm(std::size_t worm);
    void updateBugzzz(double elapsed);
    void renderBugzzz(double elapsed);
    bool bugzzzPlayableAt(double x, double y, int radius = 0) const;
    bool bugzzzMaskBlocked(int x, int y) const;
    void loseBugzzzLife(std::size_t worm, std::string_view reason);
    void finishBugzzzGame();
    int bugzzzSecond(double now) const;
    long long bugzzzTick(double now) const;
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        bugzzzHotspotAt(int x, int y) const;
    // Developer control API for "bugzzz:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBugzzzDeveloperCommand(std::string_view command);

    std::optional<BugzzzGame> bugzzzGame;
};

} // namespace fgp
