#pragma once

#include "game/app_core.h"

namespace fgp {

// The "whackamole" minigame: its state and logic.
// Implemented in app_whackamole.cpp.
class WhackamoleScene : protected virtual AppCore {
protected:
    enum class WhackamolePhase {
        Attract,
        Playing,
        LevelComplete,
        GameOver,
    };
    enum class WhackamoleTargetKind { None, Bird, Squirrel };
    enum class WhackamoleClipPhase { Appear, Beg, Retreat };
    struct WhackamoleClip {
        ExecutableGameData::AnimationDefinition definition;
        DecodedMedia media;
        TexturePtr texture;
    };
    struct WhackamoleHole {
        std::array<std::size_t, 5> birdForward{};
        std::array<std::size_t, 5> birdReverse{};
        std::array<std::size_t, 3> squirrel{};
        ExecutableGameData::AnimationDefinition chirpDefinition;
        DecodedMedia chirp;
        std::optional<std::size_t> activeClip;
        WhackamoleTargetKind kind{WhackamoleTargetKind::None};
        WhackamoleClipPhase clipPhase{WhackamoleClipPhase::Appear};
        int loopsRemaining{};
        bool reverse{};
        bool accepted{};
        bool chirpActive{};
        LiveAudioMixer::EffectHandle chirpHandle{
            LiveAudioMixer::kInvalidEffectHandle};
        double clipStarted{};
    };
    struct WhackamoleGame {
        std::array<ExecutableGameData::WhackamoleLevelDefinition, 18> levels;
        std::vector<WhackamoleClip> clips;
        std::array<WhackamoleHole, 12> holes;
        SurfacePtr startSurface;
        TexturePtr startTexture;
        SDL_Rect startBounds{203, 238, 428, 136};
        DecodedMedia correctSound;
        DecodedMedia wrongSound;
        TexturePtr wrongFeedbackTexture;
        std::array<DecodedMedia, 2> thankYouSounds;
        DecodedMedia cursorMedia;
        TexturePtr cursorTexture;
        WhackamolePhase phase{WhackamolePhase::Attract};
        int level{};
        int score{};
        int hits{};
        int opportunities{};
        int birdsSpawned{};
        int squirrelsSpawned{};
        double levelStarted{};
        double levelDuration{};
        double birdInterval{};
        double squirrelInterval{};
        double nextBirdAt{};
        double nextSquirrelAt{};
        double phaseStarted{};
        double wrongFeedbackStarted{};
        bool wrongFeedbackActive{};
    };

    WhackamoleGame loadWhackamoleGame(Package &package);
    void startWhackamoleGame();
    void beginWhackamoleLevel();
    bool spawnWhackamoleTarget(bool squirrel, double elapsed);
    bool pressWhackamoleHole(std::size_t index);
    void advanceWhackamoleHole(std::size_t index, double elapsed);
    void updateWhackamole(double elapsed);
    void renderWhackamole(double elapsed);
    void renderWhackamoleCursor(double elapsed);
    int whackamolePercent() const;
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        whackamoleHotspotAt(int x, int y) const;
    // Developer control API for "whackamole:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeWhackamoleDeveloperCommand(std::string_view command);

    std::optional<WhackamoleGame> whackamoleGame;
};

} // namespace fgp
