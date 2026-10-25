#include "game/app.h"

#include "game/touch_controls.h"
#include "platform/web_ui.h"

namespace fgp {

// Browser build: tells the page what the current screen needs from a phone,
// the keyboard for typing and the on-screen keys for the scenes that read
// keys (src/game/touch_controls.h). Both calls do nothing natively.
void GameApp::updateWebControls() {
    const bool inScene = viewMode == ViewMode::Scene;
    // The dialogs that take text, where the page keeps them above the phone's
    // keyboard: the name dialog (app_render.cpp), Fence's save dialog and the
    // Activity page's text dialog, which share its backdrop.
    const int dialogWidth = nameDialogBackdrop ? nameDialogBackdrop->w : 309;
    const int dialogHeight = nameDialogBackdrop ? nameDialogBackdrop->h : 113;
    if (dialogMode == DialogMode::Name) {
        setTextEntryWanted(true, {249, 247, dialogWidth, dialogHeight});
    } else if (dialogMode == DialogMode::None && inScene && fenceGame &&
               fenceGame->saveName) {
        setTextEntryWanted(true, {0xf9, 0xf7, dialogWidth, dialogHeight});
    } else if (dialogMode == DialogMode::None && inScene && activitypageGame &&
               activitypageGame->textPurpose != ActivityTextPurpose::None) {
        setTextEntryWanted(true, {0xf5, 0xf3, dialogWidth, dialogHeight});
    } else if (dialogMode == DialogMode::None && inScene &&
               activeScene == "textinvader") {
        // The whole screen: the words fall from the top.
        setTextEntryWanted(true);
    } else {
        setTextEntryWanted(false);
    }

    std::string_view controls;
    if (inScene && dialogMode == DialogMode::None && !storyNavigationOpen &&
        !pendingScene && !sceneDataLoading) {
        if (bugzzzGame) {
            std::size_t worm = 0;
            while (worm < bugzzzGame->worms.size() &&
                   !bugzzzGame->worms[worm].playing) {
                ++worm;
            }
            if (worm == bugzzzGame->worms.size()) worm = 0;
            static constexpr std::array<std::string_view, 4> states{
                "worm1", "worm2", "worm3", "worm4"};
            controls = touchLayout("bugzzz", states[worm]);
        } else if (balloonhuntGame) {
            controls = touchLayout(
                "balloonhunt",
                balloonhuntGame->phase == BalloonhuntPhase::Menu      ? "menu"
                : balloonhuntGame->phase == BalloonhuntPhase::Playing ? "playing"
                                                                       : "over");
        } else if (jigsawGame) {
            if (jigsawGame->session && jigsawGame->rotation) {
                controls = touchLayout("jigsaw", "rotation");
            }
        } else {
            controls = touchLayout(activeScene);
        }
        if (controls.empty() && !sceneClips.empty() && !sceneSequenceFinished &&
            activeScene != "logo") {
            controls = kTouchStorySkip;
        }
    }
    setTouchControls(controls);
}

} // namespace fgp
