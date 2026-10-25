#pragma once

#include "game/app_core.h"

namespace fgp {

// The "audiopairs" minigame: its state and logic.
// Implemented in app_audiopairs.cpp.
class AudiopairsScene : protected virtual AppCore {
protected:
    enum class AudiopairsPhase {
        Attract,
        Playing,
        Resolving,
        LevelComplete,
        ClearingLeft,
        GameOver,
    };
    enum class AudiopairsButtonState { Green, Yellow, Red, Black };
    struct AudiopairsButton {
        SDL_Rect bounds{};
        int sound{-1};
        AudiopairsButtonState state{AudiopairsButtonState::Black};
    };
    struct AudiopairsPart {
        SDL_Rect bounds{};
        TexturePtr texture;
    };
    struct AudiopairsAnimation {
        std::string name;
        int x{};
        int y{};
        DecodedMedia media;
        TexturePtr texture;
    };
    struct AudiopairsGame {
        std::array<AudiopairsButton, 20> buttons;
        // [colour][row]. FGP.exe selects the bitmap suffix with index / 5.
        std::array<std::array<TexturePtr, 4>, 4> buttonTextures;
        std::array<AudiopairsPart, 14> leftParts;
        std::array<AudiopairsPart, 14> rightParts;
        TexturePtr pigTexture;
        int pigWidth{};
        int pigHeight{};
        std::array<std::optional<DecodedMedia>, 50> sounds;
        std::array<DecodedMedia, 14> partTones;
        // start, lever, completed-left-machine, coin/pig sequence.
        std::array<AudiopairsAnimation, 4> animations;
        AudiopairsPhase phase{AudiopairsPhase::Attract};
        std::optional<std::size_t> firstSelection;
        std::optional<std::size_t> secondSelection;
        std::optional<std::size_t> activeAnimation;
        double activeAnimationStarted{};
        double resolveAt{};
        double nextPartClearAt{};
        double partClearInterval{};
        bool countersVisible{};
        int attempts{15};
        int points{};
        int finalScore{};
        int level{};
        int leftPartCount{};
        int rightPartCount{};
        int activePairs{};
    };

    AudiopairsGame loadAudiopairsGame(Package &package);
    void startAudiopairsGame();
    void dealAudiopairsBoard();
    bool pressAudiopairsButton(std::size_t index);
    void resolveAudiopairsPair();
    void updateAudiopairs(double elapsed);
    void renderAudiopairs(double elapsed);
    void playAudiopairsAnimation(std::size_t animation);
    DecodedMedia &audiopairsSound(int sound);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        audiopairsHotspotAt(int x, int y) const;
    // Developer control API for "audiopairs:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeAudiopairsDeveloperCommand(std::string_view command);

    std::optional<AudiopairsGame> audiopairsGame;
};

} // namespace fgp
