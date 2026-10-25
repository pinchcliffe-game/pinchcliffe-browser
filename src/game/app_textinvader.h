#pragma once

#include "game/app_core.h"

namespace fgp {

// The "textinvader" minigame: a port of FGP.exe 0x0044c640-0x0044d980.
// Implemented in app_textinvader.cpp.
class TextinvaderScene : protected virtual AppCore {
protected:
    // One original 0xa8-byte animation descriptor.
    struct TextinvaderAnim {
        std::string name;
        std::uint32_t address{};
        bool smacker{};
        bool visible{true};
        bool clone{};
        int defaultX{-1};
        int defaultY{-1};
        std::uint32_t hotspot{};
        bool active{};
        bool playing{};
        int x{};
        int y{};
        int layer{};
        std::uint64_t order{};
        double playStarted{};
        double frozenAt{};
        LiveAudioMixer::EffectHandle audioHandle{
            LiveAudioMixer::kInvalidEffectHandle};
    };
    struct TextinvaderClip {
        DecodedMedia media;
        TexturePtr texture;
        double completion{};
    };
    // A text control (0x00407930): a rectangle, a layer and one string.
    struct TextinvaderText {
        std::uint32_t address{};
        int x{};
        int y{};
        int layer{};
        bool active{};
        std::size_t font{};
        std::string text;
        std::uint64_t order{};
    };
    // The original RECT: left, top, right, bottom.
    struct TextinvaderBox {
        int left{};
        int top{};
        int right{};
        int bottom{};
    };
    // One 0x258-byte word slot at 0x00682c20.
    struct TextinvaderSlot {
        bool active{};             // +0x000
        bool completed{};          // +0x004
        int bonus{};               // +0x008: 0 none, 1 yellow, 2 orange, 3 red
        int yellowWeight{};        // +0x00c: read by 0x0044d259, never written
        std::string text;          // +0x010
        TextinvaderText word;      // +0x034, layer 3
        TextinvaderText typed;     // +0x0dc, layer 4
        TextinvaderAnim lightning; // +0x184, copy of `li` 0x004de7f0
        int x{};                   // +0x22c
        int y{};                   // +0x230
        int offsetX{};             // +0x234
        int offsetY{};             // +0x238
        int width{};               // +0x23c
        int height{};              // +0x240
        float fallY{};             // +0x244
        TextinvaderBox rect;       // +0x248
        std::uint64_t spawnSerial{}; // native only: spawn order
        bool developerSkip{};        // native only: left for type-word
    };
    struct TextinvaderTimer {
        int due{};
        int id{};
    };
    struct TextinvaderGame {
        // 0x0044c740's fonts: blue, green, (small: messageFont), yellow,
        // orange, red.
        std::array<BitmapFont, 6> fonts;
        std::unordered_map<std::string, TextinvaderClip> clips;
        TexturePtr mountainTexture;
        TexturePtr lightsOffTexture;
        TexturePtr lightsOnTexture;
        TexturePtr signTexture;
        TextinvaderAnim mountain;  // fjell 0x004de6a0
        TextinvaderAnim lightsOff; // julelys2 0x004de898
        TextinvaderAnim startSign; // skilt 0x004de940 (Smacker)
        TextinvaderAnim scoreSign; // skilt 0x004de9e8 (bitmap)
        std::vector<TextinvaderAnim> clones;
        TextinvaderText scoreText; // 0x006843a8
        std::uint64_t lightsOrder{};
        std::array<std::vector<std::string>, 32> wordsByLength;
        std::array<TextinvaderSlot, 10> slots;
        std::vector<TextinvaderTimer> timers;
        std::string typed;          // 0x00682bf0
        std::string target;         // 0x00687510
        int selected{-1};           // 0x00682be8
        bool playing{};             // 0x00684394
        bool starting{};            // 0x006864b8
        int startPressedAt{};       // 0x00684450
        int difficulty{};           // 0x00687500
        int energy{100};            // 0x006874fc
        int score{};                // 0x00687504
        int completedWords{};       // 0x00682c18
        int removedWords{};         // 0x00682c1c
        int levelStep{};            // 0x006864c0
        int lastBonusQuotient{};    // 0x00684398
        int nextBonus{};            // 0x0068750c
        int maximumWords{1};        // 0x00687534
        int maximumLength{1};       // 0x00684454
        int spawnSpread{6000};      // 0x006843a0
        float fallSpeed{8.0F};      // 0x00682bec
        float speedFactor{1.0F};    // 0x0068439c
        float lights{};             // julelys1 progress value, 0..1
        int lastTick{};             // 0x00682c14
        bool initialized{};
        bool gameOverModal{};
        std::uint64_t nextOrder{};
        std::uint64_t nextSpawnSerial{};
        std::uint32_t randomState{};
        std::optional<std::uint32_t> fixedSeed;
        std::optional<int> developerWaitUntil;
    };

    TextinvaderGame loadTextinvaderGame(Package &package);
    bool typeTextinvader(std::string_view utf8);
    bool textinvaderClaimsKey(SDL_Keycode key) const;
    void pressTextinvaderHotspot(std::uint32_t hotspot);
    void updateTextinvader(double elapsed);
    void renderTextinvader(double elapsed);
    void finishTextinvaderGameOver();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        textinvaderHotspotAt(int x, int y) const;
    // Developer control API for "textinvader:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeTextinvaderDeveloperCommand(std::string_view command);

    std::optional<TextinvaderGame> textinvaderGame;

    // Reached by GameApp::startTournamentRound as well.
    void startTextinvaderGame();
private:
    int textinvaderNow();
    int textinvaderRandom(int bound);
    void traceTextinvaderHud();
    void addTextinvaderAnim(TextinvaderAnim &anim, int x, int y, int layer);
    void removeTextinvaderAnim(TextinvaderAnim &anim);
    void stopTextinvaderAnim(TextinvaderAnim &anim);
    void playTextinvaderAnim(TextinvaderAnim &anim);
    void playTextinvaderSound(std::string_view name);
    void finishTextinvaderClip(TextinvaderAnim &anim);
    void createTextinvaderText(TextinvaderText &control, int x, int y);
    void destroyTextinvaderText(TextinvaderText &control);
    void addTextinvaderTimer(int due, int id);
    void initializeTextinvaderScene();
    void showTextinvaderStartSign();
    void spawnTextinvaderWord(int slot);
    void showTextinvaderWord(int slot);
    void resetTextinvaderTyping();
    void updateTextinvaderHud();
    void removeTextinvaderWord(int slot);
    void textinvaderTimer(int id);
    void applyTextinvaderBonus(int bonus);
    void tickTextinvader();
    void advanceTextinvaderLevel();
    void checkTextinvaderGameOver();
    void textinvaderGameOver();
    int judgeTextinvaderTyping() const;
    void selectTextinvaderTarget();
    void keyTextinvader(unsigned char character);
};

} // namespace fgp
