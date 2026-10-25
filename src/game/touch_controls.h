#pragma once

#include <array>
#include <string_view>

namespace fgp {

// On-screen keys the browser page (tools/web/shell.html) shows on touch
// screens, for the scenes that cannot be played with taps, drags and long
// presses alone. GameApp::updateWebControls picks the row for the current
// scene and state.
//
// A spec is a list of items separated by spaces. "L=<code>", "R=<code>",
// "U=<code>" and "D=<code>" put a key on the left, right, up or down arm of
// the d-pad; any other item is an action button "<code>[:<label>]", with '_'
// for a space in the label. Codes are DOM KeyboardEvent.code values; the page
// holds each key down for as long as the finger stays on it.
//
// tools/fgp_input_inventory.py reads this table and checks it against the
// keys each scene's source handles; docs/WEB_INPUT.md is generated from both.
struct TouchLayout {
    std::string_view scene;
    std::string_view state;
    std::string_view spec;
};

inline constexpr std::array<TouchLayout, 20> kTouchLayouts{{
    // Steering polls the four arrows and Space every frame.
    {"mountain", "", "U=ArrowUp D=ArrowDown L=ArrowLeft R=ArrowRight Space:Hopp"},
    // Each worm steers and spits from its own held keys (0x00659D10..):
    // the controls follow the first worm that is playing.
    {"bugzzz", "worm1", "L=ArrowLeft R=ArrowRight Enter:Spytt"},
    {"bugzzz", "worm2", "L=KeyA R=KeyS ControlLeft:Spytt"},
    {"bugzzz", "worm3", "L=KeyJ R=KeyK Space:Spytt"},
    {"bugzzz", "worm4", "L=NumpadMultiply R=NumpadSubtract NumpadDivide:Spytt"},
    // The mode menu reads the arrows and Enter; flying polls the arrows,
    // A/Z for speed and Page Up/Down for the radar.
    {"balloonhunt", "menu", "U=ArrowUp D=ArrowDown L=ArrowLeft R=ArrowRight Enter:Start"},
    {"balloonhunt", "playing",
     "U=ArrowUp D=ArrowDown L=ArrowLeft R=ArrowRight KeyA:Fortere KeyZ:Saktere "
     "PageUp:Radar_+ PageDown:Radar_-"},
    {"balloonhunt", "over", "Enter:OK"},
    // Left and right drive while held; Up tips the cart (0x0044f640).
    {"wheelbarrow", "", "U=ArrowUp L=ArrowLeft R=ArrowRight"},
    {"beemaze", "", "U=ArrowUp D=ArrowDown L=ArrowLeft R=ArrowRight"},
    // Player 1's control template (0x004a0150): the arrows, Ctrl to use a
    // pickup and Shift for the horn.
    {"racing", "", "U=ArrowUp D=ArrowDown L=ArrowLeft R=ArrowRight ControlLeft:Bruk ShiftLeft:Tut"},
    // The arrows turn the piece being dragged (0x0046ab80).
    {"jigsaw", "rotation", "ArrowLeft:\xe2\x9f\xb2 ArrowRight:\xe2\x9f\xb3"},
    // Scenes whose own key handler acts on Space.
    {"intro", "", "Space:Hopp_over"},
    {"morning", "", "Space:Mellomrom"},
    {"town", "", "Space:Hopp_over"},
    {"birthday", "", "Space:Hopp_over"},
    {"goodbye", "", "Space:Hopp_over"},
    {"outro", "", "Space:Hopp_over"},
    {"buildacar", "", "Space:Hopp_over"},
    {"gallery", "", "Space:Stille"},
}};

// Any other scene while its opening or narration plays: Space skips it.
inline constexpr std::string_view kTouchStorySkip = "Space:Hopp_over";

inline std::string_view touchLayout(std::string_view scene,
                                    std::string_view state = {}) {
    for (const auto &layout : kTouchLayouts) {
        if (layout.scene == scene && layout.state == state) {
            return layout.spec;
        }
    }
    return {};
}

} // namespace fgp
