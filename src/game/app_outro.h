#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/story_descriptors.h"

namespace fgp {

// The "outro" story scene's own handler: fireworks and the credits roll.
// Implemented in app_outro.cpp, a port of FGP.exe 0x004182f0-0x00418990.
// The character group (S14AN-SS-002), the 3D-glasses collectible and the
// music stay on the generic story-scene path.
class OutroScene : protected virtual StoryDescriptors {
protected:
    struct OutroTimer {
        double due{};
        double period{};
        int id{};
    };
    // One of the 48 credit-line text descriptors at 0x005783e0 (stride
    // 0xa8) and its start time in 0x0057831c.
    struct OutroLine {
        bool active{};
        std::string text;
        int font{};
        int x{};
        int y{};
        double start{};
    };
    struct OutroState {
        StoryStage stage;
        std::array<StoryAnim *, 5> fireworks{}; // 0x004ace18
        std::vector<OutroTimer> timers;
        std::array<OutroLine, 48> lines{};
        // credits.ini, section [credits].
        std::vector<std::pair<std::string, std::string>> credits;
        int nextEntry{}; // 0x00578318
        std::array<BitmapFont, 2> fonts; // 0x0057a360, 0x0057b390
        bool initialized{};
        double suspendedAt{-1.0};
        double suspended{};
        std::optional<int> pinnedRandom;
    };

    // 0x00418464: S14AN-SS-001 starts through 0x0040a770, as a sequence
    // that holds conflict bit 1 until the group's S14AN-SS-002 takes over.
    void buildOutroScene(SceneBuild &build);
    void loadOutroScene();
    void updateOutro(double elapsed);
    // Fireworks draw on layer 0 and the credit lines on layer 15, so the
    // lines go above the character group's layer-10 clips.
    void renderOutro(double elapsed);
    void renderOutroCredits(double elapsed);
    bool handleOutroKey(SDL_Keycode key);
    bool executeOutroDeveloperCommand(std::string_view command);

    std::optional<OutroState> outroState;
    // outro:random=N, kept across scenes like goodbye:random=N.
    std::optional<int> outroPinnedRandom;
    // outro:random=cycle, like goodbye:random=cycle.
    bool outroCycleRandom{};
    int outroDraws{};

private:
    double outroClock(double elapsed);
    void initializeOutro(double now);
    const std::string *outroCredit(std::string_view key) const;
    void showOutroFirework(double now);
    void showOutroCredit(double now);
    double addOutroLines(const std::string &text, int font, double now);
    void scrollOutroLines(double now);
};

} // namespace fgp
