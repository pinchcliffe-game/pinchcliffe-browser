#pragma once

#include <string_view>

// Hooks from the game to the browser page (tools/web/shell.html).

namespace fgp {

// A rectangle in the game's 800x600 logical coordinates.
struct WebRect {
    int x{};
    int y{};
    int width{800};
    int height{600};
};

// Tells the page whether the game is waiting for typed text (the new-player
// name dialog, Text Invader), so it can offer a button that opens the phone's
// on-screen keyboard, and where the text field is, so the page can keep it in
// view above that keyboard. Cheap to call every frame; only changes reach the
// page. Does nothing outside the browser build.
void setTextEntryWanted(bool wanted, WebRect field = {});

// Tells the page which on-screen keys the current scene needs on a touch
// screen (src/game/touch_controls.h describes the spec; empty for none).
// Cheap to call every frame; only changes reach the page. Does nothing outside
// the browser build.
void setTouchControls(std::string_view spec);

// True once after the page's Back button was pressed (the page calls the
// exported fgp_back_pressed). Always false outside the browser build.
bool takeWebBackRequest();

} // namespace fgp
