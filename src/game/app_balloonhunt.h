#pragma once

#include "game/app_core.h"

namespace fgp {

// The "balloonhunt" minigame: its state and logic.
// Implemented in app_balloonhunt.cpp.
class BalloonhuntScene : protected virtual AppCore {
protected:
    enum class BalloonhuntMode { Competition, Practice, Challenge };
    enum class BalloonhuntPhase {
        Menu,
        Playing,
        TrackComplete,
        Complete,
        GameOver,
    };
    struct BalloonhuntBalloon {
        std::string name;
        double x{};
        double altitude{};
        double z{};
        int number{};
        bool collected{};
    };
    struct BalloonhuntTrack {
        int number{};
        double size{};
        double seconds{};
        double startX{};
        double startAltitude{};
        double startZ{};
        double startHeading{};
        std::vector<BalloonhuntBalloon> balloons;
    };
    struct BalloonhuntScenery {
        std::size_t texture{};
        double x{};
        double altitude{};
        double z{};
    };
    struct BalloonhuntGame {
        std::array<BalloonhuntTrack, 20> tracks;
        std::vector<BalloonhuntScenery> scenery;
        std::array<TexturePtr, 24> balloonTextures;
        std::array<TexturePtr, 24> radarBalloonTextures;
        std::array<TexturePtr, 9> sceneryTextures;
        std::array<TexturePtr, 49> gyroTextures;
        SurfacePtr startSurface;
        SurfacePtr trackSelectorSurface;
        TexturePtr startTexture;
        TexturePtr trackSelectorTexture;
        TexturePtr startButtonTexture;
        TexturePtr startButtonPressedTexture;
        TexturePtr interfaceTopTexture;
        TexturePtr interfaceBottomTexture;
        TexturePtr mapTexture;
        TexturePtr skyTexture;
        TexturePtr compassTexture;
        TexturePtr autopilotTexture;
        DecodedMedia solanMedia;
        TexturePtr solanTexture;
        DecodedMedia windSound;
        DecodedMedia hitSound;
        DecodedMedia wrongSound;
        DecodedMedia winSound;
        DecodedMedia gameOverSound;
        BalloonhuntMode selectedMode{BalloonhuntMode::Competition};
        BalloonhuntMode activeMode{BalloonhuntMode::Competition};
        BalloonhuntPhase phase{BalloonhuntPhase::Menu};
        int selectedTrack{1};
        int activeTrack{1};
        int currentBalloon{};
        int score{};
        double x{};
        double altitude{90.0};
        double z{};
        double heading{};
        double speed{300.0};
        double verticalSpeed{};
        double fuel{100.0};
        double radarZoom{100.0};
        double trackStarted{};
        double lastUpdate{};
        double lastHit{};
        double phaseStarted{};
        double collisionCooldownUntil{};
        bool autopilot{};
        int practiceRespawns{};
        std::string phaseMessage;
    };

    BalloonhuntGame loadBalloonhuntGame(Package &package);
    void startBalloonhuntGame(BalloonhuntMode mode);
    void startBalloonhuntTrack(int track, bool retainScore = true);
    void returnToBalloonhuntMenu();
    bool handleBalloonhuntKey(SDL_Keycode key);
    bool pressBalloonhuntHotspot(std::uint32_t hotspot);
    void hitBalloonhuntBalloon(
        bool correct,
        std::optional<std::size_t> collidedBalloon = std::nullopt);
    void updateBalloonhunt(double elapsed);
    void renderBalloonhunt(double elapsed);
    void renderBalloonhuntTerrain();
    void renderBalloonhuntInterface(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        balloonhuntHotspotAt(int x, int y) const;
    // Developer control API for "balloonhunt:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBalloonhuntDeveloperCommand(std::string_view command);

    std::optional<BalloonhuntGame> balloonhuntGame;
};

} // namespace fgp
