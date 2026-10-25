#pragma once

#include "game/app_core.h"

namespace fgp {

// The "hopscotch" minigame: its state and logic.
// Implemented in app_hopscotch.cpp.
class HopscotchScene : protected virtual AppCore {
protected:
    enum class HopscotchPhase {
        Attract,
        Demonstrating,
        TurnTransition,
        TimerTransition,
        PlayerTurn,
        RoundComplete,
        GameOver,
    };
    struct HopscotchLevel {
        int startingLength{};
        int rounds{};
        int board{};
        int availableTiles{};
        int milliseconds{};
        bool symbols{};
    };
    struct HopscotchGame {
        std::array<HopscotchLevel, 16> levels;
        std::array<TexturePtr, 4> gridTextures;
        std::array<std::array<TexturePtr, 12>, 2> tileTextures;
        std::array<TexturePtr, 2> startTextures;
        std::array<std::array<DecodedMedia, 12>, 4> tones;
        std::array<HopscotchMedia, 8> solanJumps;
        std::array<HopscotchMedia, 8> ludvigJumps;
        std::array<HopscotchMedia, 8> ludvigFalls;
        HopscotchMedia solanStart;
        HopscotchMedia ludvigStart;
        HopscotchMedia timer;
        HopscotchMedia rotate;
        HopscotchMedia bird;
        DecodedMedia wrongSound;
        HopscotchPhase phase{HopscotchPhase::Attract};
        int level{};
        int round{};
        int score{};
        int playerPosition{};
        int shownPosition{-1};
        int characterTile{-1};
        int characterDirection{2};
        int characterX{};
        int characterY{};
        int characterTargetX{};
        int characterTargetY{};
        std::vector<int> masterSequence;
        double phaseStarted{};
        double deadline{};
        double characterStarted{};
        double nextDemonstrationStep{};
        int demonstrationGapStage{};
        bool characterIsSolan{};
        bool characterFalling{};
        bool demonstrationStartsTurn{};
        bool rotateStarted{};
        bool pressedStart{};
        std::string phaseMessage;
    };

    HopscotchGame loadHopscotchGame(Package &package);
    void endHopscotchGame(double elapsed);
    void returnHopscotchToAttract();
    void startHopscotchGame();
    void beginHopscotchLevel(bool retainScore = true);
    void beginHopscotchRound();
    void demonstrateHopscotch(bool hint = false);
    void startHopscotchDemonstrationJump(double elapsed);
    bool pressHopscotchAt(int x, int y, std::uint32_t hotspot);
    void updateHopscotch(double elapsed);
    void renderHopscotch(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        hopscotchHotspotAt(int x, int y) const;
    // Developer control API for "hopscotch:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeHopscotchDeveloperCommand(std::string_view command);

    std::optional<HopscotchGame> hopscotchGame;
};

} // namespace fgp
