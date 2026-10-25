#pragma once

#include "game/app_core.h"

namespace fgp {

// Game Trivia (scene "gametrivia"): its state and logic.
// Implemented in app_trivia.cpp.
class TriviaScene : protected virtual AppCore {
protected:
    enum class TriviaPhase { Attract, Playing, Feedback, GameOver };
    struct TriviaQuestion {
        int type{};
        int difficulty{};
        int scene{};
        std::uint32_t stableId{};
        std::uint32_t lastSeen{};
        std::string payload;
    };
    struct TriviaAnswer {
        int scene{};
        int picture{};
    };
    struct TriviaRound {
        int questions{8};
        std::array<int,3> difficulty{};
        std::array<double,3> seconds{};
        int scenes{3};
        int basePoints{100};
        int secondsPoints{40};
    };
    struct TriviaAnimation {
        std::string name;
        DecodedMedia media;
        TexturePtr texture;
        int x{};
        int y{};
        std::uint32_t conflictMask{};
        bool visible{};
    };
    struct TriviaActiveAnimation {
        std::size_t animation{};
        double startedAt{};
    };
    struct TriviaGame {
        TexturePtr startTexture;
        TexturePtr progressTexture;
        TexturePtr correctTexture;
        TexturePtr timeTexture;
        TexturePtr radioTexture;
        TexturePtr dragTexture;
        TexturePtr questionTexture;
        TexturePtr answerFrameTexture;
        std::array<TexturePtr,6> answerTextures;
        DecodedMedia questionAudio;
        std::vector<TriviaQuestion> questions;
        std::vector<TriviaAnswer> answers;
        std::array<TriviaRound,4> rounds;
        std::array<TriviaAnimation,9> animations;
        std::vector<TriviaActiveAnimation> activeAnimations;
        std::vector<std::size_t> roundQuestions;
        std::vector<int> answerScenes;
        std::vector<int> answerPictures;
        std::array<SDL_Rect,6> answerBounds{};
        TriviaPhase phase{TriviaPhase::Attract};
        // Normal-mode init 0x0047E386 sets selector 0x006DA3AC to 1;
        // active tournament play may replace it through 0x004194A0.
        int difficulty{1};
        int round{};
        int questionNumber{};
        int correctAnswers{};
        int score{};
        int currentScene{};
        double deadline{};
        std::optional<std::size_t> pendingTransitionAnimation;
        double pendingTransitionStartsAt{};
        bool lastCorrect{};
        bool draggingQuestion{};
        std::string questionText;
        std::string phaseMessage;
    };

    TriviaGame loadTriviaGame(Package &package);
    void saveTriviaRanks();
    void startTriviaGame();
    bool prepareTriviaRound();
    void endTriviaGame();
    void returnTriviaToAttract();
    void beginTriviaQuestion();
    void answerTrivia(std::size_t answer);
    void playTriviaAnimation(std::size_t animation, bool emitNamedStart);
    void startTriviaTransition(bool correct);
    bool beginTriviaInteraction(int x,int y);
    bool endTriviaInteraction(int x,int y);
    void updateTrivia(double elapsed);
    void renderTrivia(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        triviaHotspotAt(int x,int y) const;
    // Developer control API for "trivia:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeTriviaDeveloperCommand(std::string_view command);

    std::optional<TriviaGame> triviaGame;
    // 0x006D71A0 is the persistent used-mask passed to the shared no-repeat
    // selector for Game Trivia's four Solan transition clips.
    NoRepeatState triviaTransitionSelector;
};

} // namespace fgp
