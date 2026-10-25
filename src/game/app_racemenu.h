#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/activity_panel.h"
#include "game/app_core.h"

namespace fgp {

// The main menu's racing menu ("racemenu", Bilspillmeny): the track and the
// two players' cars, mode, laps, drivers and dirty tricks, the best-time
// tables, the race sound settings, and the info pages for a track or car.
// The race itself ("racing") is separate. Implemented in app_racemenu.cpp, a
// port of FGP.exe 0x0047a980-0x0047c4e0.
class RacemenuScene : protected virtual AppCore {
protected:
    // A track or car descriptor playing its Smacker turntable
    // (0x0050c9c0, 0x0050ca68, 0x0050cb10) or the blinker (0x0050cc60).
    struct RacemenuClip {
        int sprite{-1};
        std::vector<std::uint8_t> bytes;
        DecodedMedia media;
        std::string resource;
        int frames{};
        // The frame on screen, and FrameNum (+0x374), one past it.
        int shown{-1};
        int frameNumber{};
        bool playing{};
        double playStartedAt{};
        int playStartFrame{};
        double frameSeconds{1.0 / 15.0};
    };
    struct RacemenuGame {
        std::unique_ptr<ActivityStage> stage;
        std::unique_ptr<ActivityPanel> panel;
        std::map<std::uint32_t, std::unique_ptr<ActivityScrollbar>> sliders;
        std::unique_ptr<ActivityTooltip> tooltip;
        BitmapFont font;      // Arial Black 08 (0x006d1668)
        std::unique_ptr<LanguageTable> ini; // race.ini (0x006cf980)
        // The playable resources (0x0047b1d0): tracks 0x006cf984, cars
        // 0x006cfdc4, as race.ini resource numbers.
        std::vector<int> tracks;
        std::vector<int> cars;
        int level{};          // 0x006d03b8, from Build-a-Car
        int track{};          // 0x006d0054
        int car1{};           // 0x006d0468
        int car2{};           // 0x006d05c0
        RacemenuClip trackClip;
        RacemenuClip car1Clip;
        RacemenuClip car2Clip;
        RacemenuClip blinker;
        int player2Off{-1};   // 0x0050cbb8
        // The value pictures 0x006d01a8 (mode), 0x006cfbc0 (laps) and
        // 0x006cfa70 (drivers), named "%s%d".
        std::map<std::uint32_t, int> values;
        int trackName{-1};    // 0x006d0518
        int car1Name{-1};     // 0x006d0470
        int car2Name{-1};     // 0x006cfb18
        // The profile's 0xf0-byte settings (record 0x402 key 0); words
        // 9..0x3a are the two players' control setups, kept as read.
        std::array<std::uint32_t, 60> settings{};
        std::uint32_t hotspot{};
        std::uint32_t draggingSlider{};
        int dragLastX{};
        int dragLastY{};
        bool announced{};
    };

    RacemenuGame loadRacemenuGame();
    bool handleRacemenuEvent(const SDL_Event &event);
    void updateRacemenu(double elapsed);
    void renderRacemenu();
    // Event 0x104 (0x0047ba50): the settings go back to the profile.
    void saveRacemenu();
    // Main menu entry 8 (0x004180a0): racing needs eight placed car parts.
    void enterRacemenuFromMenu();
    // Developer control API for "racemenu:..." commands.
    bool executeRacemenuDeveloperCommand(std::string_view command);

    std::optional<RacemenuGame> racemenuGame;

private:
    void announceRacemenuEntry();
    std::span<const std::uint8_t> racemenuAsset(const std::string &asset);
    SurfacePtr racemenuBitmap(std::string_view name);
    std::uint32_t racemenuHotspotAt(int x, int y);
    void racemenuHotspotChanged(std::uint32_t hotspot);
    void racemenuMouseDown(int x, int y);
    void racemenuMouseUp(int x, int y);
    void racemenuMouseMove(int x, int y);
    void racemenuControlClicked(std::uint32_t id);
    void racemenuSliderChanged(std::uint32_t id, int value);
    std::vector<int> racemenuList(const std::string &section, int defaultState,
                                  int maximum, std::uint32_t trackAwards,
                                  std::uint32_t carAwards);
    void grantRacemenuAwards();
    void showRacemenuResource(RacemenuClip &clip, int resource,
                              const std::string &section, int nameSprite,
                              int format);
    void setRacemenuText(int sprite, const std::string &text);
    void setRacemenuPlayer2(bool enabled);
    void setRacemenuValuePicture(std::uint32_t slider, int value);
    void loadRacemenuClip(RacemenuClip &clip, const std::string &resource);
    void gotoRacemenuFrame(RacemenuClip &clip, int frame);
    void showRacemenuFrame(RacemenuClip &clip, int frame);
    void playRacemenuClip(RacemenuClip &clip);
    void stopRacemenuClip(RacemenuClip &clip);
    void tickRacemenuClip(RacemenuClip &clip, double now);
    std::array<std::uint32_t, 60> collectRacemenuSettings();
    void openRacemenuInfo(std::uint32_t oval);
    void startRace();
    void showRacemenuBestTimes(std::uint32_t id);
    int racemenuCarFormat() const;
};

} // namespace fgp
