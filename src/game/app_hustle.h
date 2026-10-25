#pragma once

#include "game/app_core.h"

namespace fgp {

// The "hustle" minigame (Emanuels utfordring): its state and logic.
// Implemented in app_hustle.cpp; the dispatcher is FGP.exe 0x0043ce20.
class HustleScene : protected virtual AppCore {
protected:
    // Mirrors the state word at 0x00670e7c.
    enum class HustlePhase {
        Attract = 0, Betting = 1, Shuffling = 2, Choosing = 3,
    };
    // One clip of the external (category-5) character sequence that
    // 0x0040a6f0 installs for the monkey's group.
    struct HustleSequenceClip {
        std::size_t clip{};
        int fps{};
        double startedAt{};
    };
    struct HustleGame {
        TexturePtr cupsTexture;
        SurfacePtr cupsHotspotMask;
        TexturePtr barTexture;
        TexturePtr scrollTexture;
        TexturePtr betTexture;
        // Every Smacker clip Hustle plays, indexed by kClip* in the .cpp.
        std::vector<HopscotchMedia> clips;
        HustlePhase phase{HustlePhase::Attract};
        bool entryAnnounced{};
        double nextTimerTick{};
        // start_0: added at entry and Game Over, started by hotspot 6.
        bool startAdded{};
        bool startPlaying{};
        double startStartedAt{};
        bool betControl{};
        // Clock (0x0043d5e0): hand_0 frame and the click it plays per step.
        bool clockRunning{};
        double clockStartedAt{};
        bool handAdded{};
        int handFrame{};
        bool clockLoopActive{};
        double clockLoopNextAt{};
        LiveAudioMixer::EffectHandle clockLoopEffect{};
        std::optional<std::size_t> clickClip;
        double clickStartedAt{};
        // Bankroll, stake slider and round state.
        int bananas{};
        int stake{};
        int stakeShown{};
        bool stakeShownValid{};
        int difficulty{1};
        int rounds{};
        int level{};
        std::vector<int> moves;
        std::optional<int> pinnedMoveFamily;
        int scrollRange{90};
        int scrollValue{90};
        int scrollTop{243};
        int scrollHeight{278};
        double barFill{};
        int frameRateOverride{-1};
        // The pending or playing external sequence.
        std::vector<std::size_t> sequence;
        std::size_t sequenceIndex{};
        bool sequenceRequested{};
        bool sequencePlaying{};
        double sequenceStartsAt{};
        HustleSequenceClip sequenceClip;
        bool betDragging{};
        // Control 7 was pressed; its click event 0x116 fires on release.
        bool betPressed{};
        int finalBananas{};
    };

    HustleGame loadHustleGame(Package &package);
    bool beginHustleInteraction(int x, int y);
    void moveHustleBet(int y);
    void endHustleInteraction(int x, int y);
    void updateHustle(double elapsed);
    void renderHustle(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        hustleHotspotAt(int x, int y) const;
    // Developer control API for "hustle:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeHustleDeveloperCommand(std::string_view command);

    std::optional<HustleGame> hustleGame;

private:
    void announceHustleEntry();
    void addHustleClip(std::size_t clip, int x, int y, int layer,
                       std::uint32_t hotspot, std::uint32_t conflictMask);
    void removeHustleClip(std::size_t clip);
    void finishHustleClip(std::size_t clip);
    void startHustleStartButton();
    void setupHustleGame(double now);
    void setHustleBananas(int bananas);
    void rebuildHustleScroll(int maximumPosition);
    void setHustleScrollValue(int value);
    void placeHustleBet(double now);
    void chooseHustleCup(int cup, double now);
    void requestHustleSequence(std::vector<std::size_t> clips, double now);
    void startHustleSequenceClip(double startedAt);
    void hustleAnimationFinished(std::size_t clip, double now);
    bool updateHustleClock(double now);
    void hustleTimerTick(double now);
    void endHustleGame(int bananas);
    int hustleBallCup() const;
};

} // namespace fgp
