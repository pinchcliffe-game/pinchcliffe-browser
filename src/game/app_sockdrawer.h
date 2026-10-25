#pragma once

#include "game/app_core.h"

namespace fgp {

// The "sockdrawer" minigame: its state and logic.
// Implemented in app_sockdrawer.cpp.
class SockdrawerScene : protected virtual AppCore {
protected:
    enum class SockdrawerPhase {
        Attract,
        ClosingBoard,
        OpeningBoard,
        TurningTimer,
        Playing,
        Resolving,
        ClosingMismatch,
        GameOver,
    };
    enum class SockdrawerDrawerState {
        Closed,
        Opening,
        Open,
        Closing,
        Matched,
    };
    struct SockdrawerDrawer {
        SDL_Rect bounds{};
        SDL_Rect sockBounds{};
        int sock{-1};
        bool active{};
        SockdrawerDrawerState state{SockdrawerDrawerState::Closed};
        double transitionStarted{};
    };
    struct SockdrawerAnimation {
        DecodedMedia media;
        TexturePtr texture;
    };
    struct SockdrawerGame {
        std::array<SockdrawerDrawer, 20> drawers;
        std::array<TexturePtr, 24> sockTextures;
        std::array<TexturePtr, 31> timerTopTextures;
        std::array<TexturePtr, 31> timerBottomTextures;
        // start, points, drawer open/close, all open/close, one open/close,
        // timer rotate/start/running/end.
        std::array<SockdrawerAnimation, 12> animations;
        SockdrawerPhase phase{SockdrawerPhase::Attract};
        std::optional<std::size_t> firstSelection;
        std::optional<std::size_t> secondSelection;
        double phaseStartedAt{};
        double resolveAt{};
        double levelStartedAt{};
        double timeLimit{20.0};
        double pointsAnimationStarted{};
        bool pointsAnimationPlaying{};
        int points{};
        int finalScore{};
        int level{};
        int activeSocks{};
        bool gameOverDialogShown{};
    };

    SockdrawerGame loadSockdrawerGame(Package &package);
    void startSockdrawerGame();
    void returnToSockdrawerAttract();
    void dealSockdrawerBoard(double elapsed);
    bool pressSockdrawerDrawer(std::size_t index);
    void resolveSockdrawerPair();
    void updateSockdrawer(double elapsed);
    void renderSockdrawer(double elapsed);
    void playSockdrawerEffect(std::size_t animation);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        sockdrawerHotspotAt(int x, int y) const;
    // Developer control API for "sockdrawer:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeSockdrawerDeveloperCommand(std::string_view command);

    std::optional<SockdrawerGame> sockdrawerGame;
};

} // namespace fgp
