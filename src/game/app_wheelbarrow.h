#pragma once

#include "game/app_core.h"

namespace fgp {

// The "wheelbarrow" minigame: its state and logic.
// Implemented in app_wheelbarrow.cpp, a port of FGP.exe 0x0044ed50-0x00450310.
class WheelbarrowScene : protected virtual AppCore {
protected:
    // One original 0xa8-byte animation descriptor. Every SCENE_AddAnim,
    // SCENE_RemoveAnim and SCENE_StopAnim call the original makes on it is
    // mirrored in the reference trace.
    struct WheelbarrowAnim {
        std::uint32_t address{};
        std::string name;
        bool smacker{};   // descriptor +0x40
        bool visible{true}; // +0x44
        bool loop{};      // +0x4c
        bool clone{};
        int defaultX{};   // +0x54
        int defaultY{};   // +0x58
        int defaultLayer{}; // +0x64
        std::uint32_t hotspot{}; // +0x50
        bool active{};
        bool playing{};
        int x{};
        int y{};
        int layer{};
        std::uint64_t order{};
        double playStarted{};
        double frozenAt{};
        std::uint64_t audioHandle{LiveAudioMixer::kInvalidEffectHandle};
    };
    // One of the ten falling-object records at 0x0068ca28 (stride 0xc4).
    struct WheelbarrowApple {
        bool active{};    // +0x00
        int type{};       // +0x04: 0 good, 1 rotten
        float x{};        // +0x08
        float y{};        // +0x0c
        int stage{};      // +0x10: next roof stage
        float vy{};       // +0x14
        int mode{};       // +0x18: 0 ripening, 1 falling, 2 splashed
        WheelbarrowAnim anim; // +0x1c
    };
    struct WheelbarrowClip {
        DecodedMedia media;
        TexturePtr texture;
        double completion{};
    };
    struct WheelbarrowImage {
        TexturePtr texture;
        int width{};
        int height{};
    };
    struct WheelbarrowGame {
        std::unordered_map<std::string, WheelbarrowImage> images;
        std::unordered_map<std::string, WheelbarrowClip> clips;
        WheelbarrowAnim start;       // 0x004e6338
        WheelbarrowAnim birds;       // 0x004e6290
        WheelbarrowAnim basket;      // 0x004e61e8
        WheelbarrowAnim pickup;      // 0x004e5ff0
        WheelbarrowAnim pie;         // 0x004e6140
        WheelbarrowAnim putdown;     // 0x004e6098
        WheelbarrowAnim cart;        // 0x0068baf0
        std::array<WheelbarrowAnim, 12> counters; // 0x0068bbb0
        std::array<WheelbarrowAnim, 10> pies;     // 0x0068c398
        std::array<WheelbarrowApple, 10> apples;  // 0x0068ca28
        std::vector<WheelbarrowAnim> clones;      // 0x0054fea8
        bool initialized{};
        bool active{};        // 0x0068ba28
        int score{};          // 0x0068ba2c
        int level{};          // 0x0068ba30
        int delivered{};      // 0x0068ba34
        int lives{};          // 0x0068ba38
        float cartX{324.0F};  // 0x0068bb98
        int direction{};      // 0x0068bb9c: 0 left, 1 right
        float speed{300.0F};  // 0x0068bba0
        int cartApples{};     // 0x0068bba4
        int cartState{};      // 0x0068bba8: neutral, cycle, stop, dump
        double lastTick{};    // 0x0068c390, in milliseconds
        // The repeating spawn EVENT at 0x0068c394.
        bool spawnTimer{};
        double nextSpawn{};
        double spawnPeriod{};
        bool leftHeld{};
        bool rightHeld{};
        bool gameOverModal{};
        // The pass time while updateWheelbarrow replays milliseconds.
        std::optional<double> simulatedClock;
        std::string scoreText;
        std::string livesText;
        std::uint64_t nextOrder{};
        // Developer pins for the three rand(n) draws (spawn, rotten chance,
        // feedback variant) and the 1-based chance draw forced rotten.
        std::optional<int> pinnedSpawn;
        std::optional<int> pinnedChance;
        std::optional<int> pinnedVariant;
        int pinnedRottenDraw{};
        int chanceDraws{};
    };

    WheelbarrowGame loadWheelbarrowGame(Package &package);
    void startWheelbarrowGame(int level = 0);
    bool handleWheelbarrowKey(SDL_Keycode key, bool pressed = true);
    void updateWheelbarrow(double elapsed);
    void renderWheelbarrow(double elapsed);
    // The GAMEOVER message and score table returned (0x00450242).
    void finishWheelbarrowGameOver();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        wheelbarrowHotspotAt(int x, int y) const;
    // Developer control API for "wheelbarrow:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeWheelbarrowDeveloperCommand(std::string_view command);

    std::optional<WheelbarrowGame> wheelbarrowGame;

private:
    std::optional<double> wheelbarrowWaitUntil;
    static WheelbarrowAnim staticAnim(std::uint32_t address, std::string name,
                                      bool smacker, bool visible, bool loop,
                                      int x, int y, std::uint32_t hotspot);
    void initializeWheelbarrowScene();
    int wheelbarrowRandom(int bound, std::optional<int> pinned);
    void addWheelbarrowAnim(WheelbarrowAnim &anim, int x, int y, int layer);
    void stopWheelbarrowAnim(WheelbarrowAnim &anim);
    void removeWheelbarrowAnim(WheelbarrowAnim &anim);
    void playWheelbarrowAnim(WheelbarrowAnim &anim);
    void addAndPlayWheelbarrowAnim(WheelbarrowAnim &anim);
    void finishWheelbarrowClip(WheelbarrowAnim &anim);
    void playWheelbarrowSound(std::string_view family, int variants);
    void setWheelbarrowCartState(int state);
    void setWheelbarrowCounters(int count);
    void setWheelbarrowBasket(int delivered);
    void setWheelbarrowLevel(int level);
    void setWheelbarrowSpawnTimer(int period);
    void addWheelbarrowScore(int points);
    bool loseWheelbarrowLife();
    void removeWheelbarrowApple(WheelbarrowApple &apple);
    void setWheelbarrowAppleMode(WheelbarrowApple &apple, int mode);
    void spawnWheelbarrowApple();
    void resumeWheelbarrowCart();
    void completeWheelbarrowDump();
    void finishWheelbarrowAnim(WheelbarrowAnim &anim);
    bool updateWheelbarrowApple(WheelbarrowApple &apple, const SDL_Rect &cart,
                                float dt);
    bool moveWheelbarrowApple(WheelbarrowApple &apple, float dt);
    void tickWheelbarrow(double nowMilliseconds);
    double wheelbarrowClock();
    void updateWheelbarrowDescriptors(double now);
    void wheelbarrowGameOver();
    std::vector<WheelbarrowAnim *> wheelbarrowAnims();
};

} // namespace fgp
