#pragma once

#include "game/app_core.h"

namespace fgp {

// The "butterfly" minigame: its state and logic.
// Implemented in app_butterfly.cpp.
class ButterflyScene : protected virtual AppCore {
protected:
    enum class ButterflyPhase { Attract, Playing, GameOver };
    struct ButterflySpecies {
        std::string name;
        int rarityClass{};
        int points{};
        int spawnWeight{};
        TexturePtr iconTexture;
        std::array<DecodedMedia, 8> directionalMedia;
        std::array<TexturePtr, 8> directionalTextures;
    };
    struct ButterflyEntity {
        bool pest{};
        int species{};
        double x{};
        double y{};
        double vx{};
        double vy{};
        double nextTurnAt{};
    };
    struct ButterflyOverlay {
        SDL_Rect bounds{};
        TexturePtr texture;
    };
    struct ButterflyAnimation {
        DecodedMedia media;
        TexturePtr texture;
    };
    struct ButterflyGame {
        std::array<TexturePtr, 4> levelTextures;
        std::array<ButterflyOverlay, 8> borders;
        std::array<TexturePtr, 2> albumTextures;
        std::array<ButterflySpecies, 15> species;
        std::array<ButterflySpecies, 2> pests;
        // start, catcher, catch, miss, scream, timer start/move/end.
        std::array<ButterflyAnimation, 8> animations;
        std::vector<ButterflyEntity> entities;
        std::array<int, 15> caught{};
        ButterflyPhase phase{ButterflyPhase::Attract};
        double levelStartedAt{};
        double lastUpdateAt{};
        double nextButterflyAt{};
        double nextPestAt{};
        double catcherSwingStarted{-1.0};
        double attractStartedAt{};
        double gameOverStarted{};
        int score{};
        int finalScore{};
        int levelScore{};
        int level{};
        bool numericHudInitialized{};
    };

    ButterflyGame loadButterflyGame(Package &package);
    void startButterflyGame();
    void spawnButterflyEntity(bool pest, double elapsed);
    void updateButterfly(double elapsed);
    void renderButterfly(double elapsed);
    void catchButterflyAt(int x, int y);
    void advanceButterflyLevel(double elapsed);
    void playButterflyEffect(std::size_t animation);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        butterflyHotspotAt(int x, int y) const;
    // Developer control API for "butterfly:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeButterflyDeveloperCommand(std::string_view command);

    std::optional<ButterflyGame> butterflyGame;
};

} // namespace fgp
