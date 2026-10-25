#pragma once

#include <memory>
#include <string>
#include <vector>

#include "game/activity_panel.h"
#include "game/app_core.h"

namespace fgp {

// The main menu's "credits" scene (Rulletekst): credits.ini laid out as
// centred lines and scrolled with the shared scroll control. Implemented in
// app_credits.cpp; the dispatcher is FGP.exe 0x00415710.
class CreditsScene : protected virtual AppCore {
protected:
    // One textN/fontN/pauseN/imageN group of credits.ini (0x00415ad0).
    struct CreditsEntry {
        std::vector<std::string> lines;
        int font{1};
        int pause{};
        bool image{};
    };
    struct CreditsGame {
        std::unique_ptr<ActivityStage> stage;
        std::unique_ptr<ActivityPanel> panel;
        std::unique_ptr<ActivityScrollbar> scrollbar;
        // 0x0055afa0 (font 0, White Amerigo BT_10_) and 0x0055bfd0 (font 1,
        // White Amerigo BT_14_).
        std::array<BitmapFont, 2> fonts;
        std::vector<CreditsEntry> entries;
        // 0x0055ad4c: each entry's top in the laid-out text.
        std::vector<int> tops;
        // 0x0055af88: the text viewport.
        SDL_Rect viewport{};
        // The laid-out text's height (0x004159c0).
        int height{};
        int text{-1};
        int overlay{-1};
        int smacker{-1};
        bool dragging{};
        bool announced{};
    };

    CreditsGame loadCreditsGame();
    // Every SDL event while the scene runs; true when consumed.
    bool handleCreditsEvent(const SDL_Event &event);
    void updateCredits(double elapsed);
    void renderCredits(double elapsed);
    // Leaving: 0x00415de0 (event 0x104) removes the text, then the scene's
    // remaining descriptors go; resets creditsGame.
    void closeCreditsGame();

    std::optional<CreditsGame> creditsGame;

private:
    SurfacePtr creditsBitmap(std::string_view name);
    // 0x00415e10 (event 0x117): draw from the scroll value.
    void showCreditsAt(int value);
    // 0x00415bb0: draw entries from index, the first at offset.
    void drawCredits(int index, int offset);
    void setCreditsValue(int value);
};

} // namespace fgp
