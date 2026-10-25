#pragma once

#include "game/app_core.h"

namespace fgp {

// The "lettersort" minigame: its state and logic.
// Implemented in app_lettersort.cpp.
//
// The model follows FGP.exe's own records: 30 letter slots of 0xc0 bytes at
// 0x6745b0, 10 sack slots of 0x168 bytes at 0x672578, 9 claw slots of 0xc0
// bytes at 0x671eb0, the claw queue at 0x67338c, and the seven counters
// cleared together at 0x676c98 (playing, score, level, letters created,
// sacks created, letters sorted, sacks completed).
class LettersortScene : protected virtual AppCore {
protected:
    // One playing (or parked) player of a Smacker clip.
    struct LettersortClip {
        bool added{};
        bool playing{};
        std::size_t media{};
        std::string asset;
        int x{};
        int y{};
        int layer{};
        bool loop{};
        double started{};
        std::uint64_t serial{};
        LiveAudioMixer::EffectHandle audio{
            LiveAudioMixer::kInvalidEffectHandle};
    };
    struct LettersortMedia {
        DecodedMedia media;
        TexturePtr texture;
    };
    struct LettersortSack {
        bool active{};
        bool gold{};
        int country{};
        int fill{};
        int x{};
        int y{};
        std::uint64_t serial{};
    };
    struct LettersortLetter {
        bool active{};
        bool gold{};
        int country{};
        int variant{};
        int x{};
        int y{};
        int layer{};
        std::uint64_t serial{};
    };
    struct LettersortClaw {
        bool active{};
        // 0 grab, 1 retry, 2 carry (claw slot +0x10).
        int state{};
        int sack{};
        bool gold{};
        int x{};
        int y{};
        LettersortClip clip;
    };
    struct LettersortGame {
        std::array<std::array<TexturePtr, 5>, 2> bagTextures;
        std::array<TexturePtr, 12> flagTextures;
        std::array<std::array<TexturePtr, 12>, 12> letterTextures;
        std::array<TexturePtr, 12> tinyTextures;
        TexturePtr goldLetterTexture;
        TexturePtr tinyGoldTexture;
        std::array<TexturePtr, 12> levelTextures;
        TexturePtr borderTexture;
        TexturePtr frameTexture;
        TexturePtr pusherTexture;
        std::vector<LettersortMedia> media;

        std::array<LettersortLetter, 30> letters;
        std::array<LettersortSack, 10> sacks;
        std::array<LettersortClaw, 9> claws;
        std::vector<int> clawQueue;
        std::vector<LettersortClip> sounds;
        LettersortClip start;
        LettersortClip bell;
        LettersortClip delivery;
        LettersortClip cameo;
        LettersortClip bagout;

        bool pusherActive{};
        int pusherX{};
        int pusherStartedMs{};
        int pusherPhase{};
        std::uint64_t pusherSerial{};
        bool levelBadge{};
        std::string levelBadgeAsset;
        std::uint64_t levelBadgeSerial{};

        bool playing{};
        bool scoreVisible{};
        int score{};
        int level{};
        int createdLetters{};
        int createdSacks{};
        int sortedLetters{};
        int completedSacks{};
        int sackLayer{-30};
        int bellCountdown{};
        int lastTimerSecond{-1};
        bool gameOverShown{};

        // The letter being dragged; it is drawn as its "Tiny" bitmap.
        std::optional<std::size_t> draggingLetter;
        int dragX{};
        int dragY{};
    };

    LettersortGame loadLettersortGame(Package &package);
    bool beginLettersortInteraction(int x, int y);
    void moveLettersortLetter(int x, int y);
    void endLettersortInteraction(int x, int y);
    void updateLettersort(double elapsed);
    void renderLettersort(double elapsed);
    // FGP.exe's score-table callback 0x43fd50.
    void returnToLettersortAttract();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        lettersortHotspotAt(int x, int y) const;
    // Developer control API for "lettersort:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeLettersortDeveloperCommand(std::string_view command);

    std::optional<LettersortGame> lettersortGame;

private:
    int lettersortAppClockMs() const;
    void addLettersortClip(LettersortClip &clip, std::string_view asset,
                           int x, int y, int layer, int tracedX, int tracedY,
                           bool loop = false);
    void startLettersortClip(LettersortClip &clip);
    void removeLettersortClip(LettersortClip &clip);
    void playLettersortSound(std::string_view name, int variants);
    void resetLettersortRound();
    void clickLettersortStart();
    void beginLettersortLevel(int level);
    void startLettersortDelivery();
    void startLettersortCameo();
    void finishLettersortDelivery();
    void placeLettersortLetter(std::size_t slot);
    void createLettersortLetter();
    void removeLettersortLetter(std::size_t slot);
    void createLettersortSack();
    void setLettersortSackFill(std::size_t sack, int delta);
    void removeLettersortSack(std::size_t sack);
    void queueLettersortClaw(std::size_t sack);
    void startLettersortClaw(std::size_t sack);
    void finishLettersortClaw(std::size_t claw);
    void startLettersortPusher();
    void updateLettersortPusher();
    void lettersortTimer(int second);
    std::vector<std::size_t> sortedLettersortSacks() const;

    // [0x676d60]: the next letter layer. FGP.exe never resets it, so it
    // carries across rounds and scene visits.
    int lettersortLetterLayer{0};
    // Draw order among equal layers: later additions are drawn on top.
    std::uint64_t lettersortNextSerial{1};
};

} // namespace fgp
