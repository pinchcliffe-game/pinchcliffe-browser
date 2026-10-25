#pragma once

#include "game/app_core.h"

namespace fgp {

// The "synonym" minigame: its state and logic.
// Implemented in app_synonym.cpp.
class SynonymScene : protected virtual AppCore {
protected:
    enum class SynonymPhase {
        Attract,
        LaunchBall1,
        LaunchBall2,
        Playing,
        TransitionBlank,
        TransitionChain,
        TimeoutClockring,
        GameOver,
    };
    struct SynonymClip {
        std::string name;
        DecodedMedia media;
        TexturePtr texture;
        int x{};
        int y{};
    };
    struct SynonymTransitionBall {
        unsigned char letter{};
        double startX{};
        double startY{};
        bool removed{};
    };
    struct SynonymEntry {
        std::string word;
        std::vector<std::string> synonyms;
        int difficulty{};
        std::uint32_t stableId{};
        std::uint32_t lastSeen{};
    };
    struct SynonymLevel {
        std::array<int,3> minimumLength{};
        std::array<int,3> maximumLength{};
        int required{10};
        double passDelay{10.0};
        int hints{1};
        double totalSeconds{260.0};
        int basePoints{20};
        int secondsPoints{3};
    };
    struct SynonymGame {
        BitmapFont promptFont;
        TexturePtr startTexture;
        TexturePtr hintTexture;
        std::array<TexturePtr,3> hintButtonTextures;
        TexturePtr passTexture;
        std::array<TexturePtr,3> passButtonTextures;
        std::array<TexturePtr,2> shuffleTextures;
        TexturePtr overlayLeftTexture;
        TexturePtr overlayRightTexture;
        TexturePtr overlayFunnelTexture;
        std::array<TexturePtr,29> largeLetters;
        std::array<TexturePtr,29> smallLetters;
        SynonymClip ballStart1;
        SynonymClip ballStart2;
        SynonymClip time;
        SynonymClip clock;
        SynonymClip correct;
        SynonymClip blank1;
        SynonymClip chain;
        SynonymClip clockring;
        std::array<DecodedMedia,2> ballFallSounds;
        DecodedMedia startBallSound;
        DecodedMedia clockTickSound;
        DecodedMedia correctSound;
        DecodedMedia wrongSound;
        DecodedMedia clickSound;
        DecodedMedia noHintSound;
        std::vector<SynonymEntry> entries;
        std::vector<std::size_t> questionPool;
        std::array<SynonymLevel,12> levels;
        SynonymPhase phase{SynonymPhase::Attract};
        std::string prompt;
        std::string answer;
        std::vector<unsigned char> shuffledLetters;
        std::vector<unsigned char> placedLetters;
        std::vector<SynonymTransitionBall> transitionBalls;
        std::optional<std::size_t> draggingLetter;
        int dragX{};
        int dragY{};
        int startingLevel{};
        int level{};
        std::size_t currentQuestion{};
        int levelCorrect{};
        int score{};
        int hintsUsed{};
        int totalHintsUsed{};
        int roundPoints{};
        std::size_t launchedLetters{};
        std::size_t clockFrame{};
        double ballStart1Started{-1.0};
        double ballStart2Started{-1.0};
        double launchStarted{-1.0};
        double blankStarted{-1.0};
        double chainStarted{-1.0};
        double chainAudioNext{-1.0};
        double clockringStarted{-1.0};
        bool showAttractResult{};
        double startBallSoundStarted{-1.0};
        double clockTickStarted{-1.0};
        double levelDeadline{};
        double levelStarted{};
        double wordStarted{};
        std::string phaseMessage;
    };

    SynonymGame loadSynonymGame(Package &package);
    void saveSynonymRanks();
    void startSynonymGame();
    bool prepareSynonymLevel();
    void beginSynonymRound();
    bool beginSynonymInteraction(int x, int y);
    void moveSynonymInteraction(int x, int y);
    void endSynonymInteraction(int x, int y);
    void completeSynonymRound();
    void beginSynonymTransition(bool pass);
    void advanceSynonymAfterTransition();
    void skipSynonymPresentation();
    void updateSynonym(double elapsed);
    void renderSynonym(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        synonymHotspotAt(int x, int y) const;
    // Developer control API for "synonym:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeSynonymDeveloperCommand(std::string_view command);

    std::optional<SynonymGame> synonymGame;
};

} // namespace fgp
