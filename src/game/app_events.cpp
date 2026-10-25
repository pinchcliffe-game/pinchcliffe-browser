#include "game/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "platform/web_ui.h"
#include "game/menu_names.h"
#include "generated/scene_escape.hpp"
#include "generated/scene_logic.hpp"
#include "generated/story_clicks.hpp"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

void GameApp::handleEvents() {
    if (takeWebBackRequest()) {
        performWebBack();
    }
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_MOUSEMOTION) {
            mouseX = event.motion.x;
            mouseY = event.motion.y;
        } else if (event.type == SDL_MOUSEBUTTONDOWN ||
                   event.type == SDL_MOUSEBUTTONUP) {
            mouseX = event.button.x;
            mouseY = event.button.y;
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_ENTER) {
                mouseInsideWindow = true;
            } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                mouseInsideWindow = false;
            }
        }
        if (event.type == SDL_MOUSEBUTTONDOWN) {
            const char *action = event.button.button == SDL_BUTTON_LEFT
                                     ? "left_click"
                                     : event.button.button == SDL_BUTTON_RIGHT
                                           ? "right_click"
                                           : "mouse_button";
            referenceTrace.emit("input", {{"action", action},
                                           {"logical_x", event.button.x},
                                           {"logical_y", event.button.y},
                                           {"button", static_cast<int>(event.button.button)}});
        }
        if (sceneDataLoading &&
            (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
             event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
             event.type == SDL_TEXTINPUT || event.type == SDL_FINGERDOWN ||
             event.type == SDL_FINGERUP)) {
            continue;
        }
        if (event.type == SDL_QUIT) {
            if (dialogMode == DialogMode::None) {
                showExitConfirmation();
            }
            continue;
        } else if (dialogMode != DialogMode::None) {
            if (event.type == SDL_MOUSEMOTION &&
                dialogMode == DialogMode::Help && helpScrollDragging) {
                constexpr int trackTop = 93 + 82;
                constexpr int trackHeight = 242;
                const int thumbHeight = helpThumb ? helpThumb->h : 29;
                const int travel = std::max(0, trackHeight - thumbHeight);
                const int thumbTop = std::clamp(
                    event.motion.y - helpScrollDragOffsetY,
                    trackTop, trackTop + travel);
                const int maximum = helpMaximumBodyScroll();
                helpBodyScrollLine = travel > 0
                                         ? static_cast<int>(std::lround(
                                               maximum *
                                               (thumbTop - trackTop) /
                                               static_cast<double>(travel)))
                                         : 0;
            } else if (event.type == SDL_MOUSEWHEEL &&
                       dialogMode == DialogMode::Help) {
                scrollHelpBody(-event.wheel.y * 3);
            } else if (event.type == SDL_TEXTINPUT && dialogMode == DialogMode::Name) {
                const std::string candidate =
                    pendingProfileName + std::string(event.text.text);
                const auto cp1252 = utf8ToCp1252(candidate);
                bool valid = cp1252 && cp1252->size() <= 15;
                if (valid) {
                    static constexpr std::string_view forbidden = "<>:\"/\\|?*";
                    for (const unsigned char character : *cp1252) {
                        if (character < 0x20 ||
                            forbidden.find(static_cast<char>(character)) !=
                                std::string_view::npos) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (valid) {
                    pendingProfileName = candidate;
                }
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (dialogMode == DialogMode::Name) {
                    if (key == SDLK_ESCAPE) {
                        closeDialog();
                    } else if (key == SDLK_BACKSPACE) {
                        removeLastUtf8Character(pendingProfileName);
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        acceptNameDialog();
                    }
                } else if (dialogMode == DialogMode::Calendar) {
                    static constexpr std::array<int, 12> daysInMonth{
                        31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                    if (key == SDLK_ESCAPE) {
                        closeDialog();
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        acceptCalendarDialog();
                    } else if (key == SDLK_LEFT) {
                        pendingBirthMonth = (pendingBirthMonth + 11) % 12;
                        pendingBirthDay = std::min(
                            pendingBirthDay, daysInMonth[pendingBirthMonth]);
                    } else if (key == SDLK_RIGHT) {
                        pendingBirthMonth = (pendingBirthMonth + 1) % 12;
                        pendingBirthDay = std::min(
                            pendingBirthDay, daysInMonth[pendingBirthMonth]);
                    } else if (key == SDLK_UP) {
                        pendingBirthDay = std::max(1, pendingBirthDay - 7);
                    } else if (key == SDLK_DOWN) {
                        pendingBirthDay = std::min(
                            daysInMonth[pendingBirthMonth], pendingBirthDay + 7);
                    }
                } else if (dialogMode == DialogMode::Message) {
                    if (key == SDLK_ESCAPE) {
                        closeDialog();
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        if (dialogMessageKind == MessageKind::ExitConfirmation) {
                            running = false;
                        } else if (dialogMessageKind ==
                                   MessageKind::MorningRideConfirmation) {
                            closeDialog();
                            enterScene("mountain", "mountain");
                        } else if (dialogMessageKind ==
                                   MessageKind::BuildacarDemolishConfirmation) {
                            closeDialog();
                            demolishBuildacar();
                        } else if (dialogMessageKind ==
                                   MessageKind::ChessAbortConfirmation) {
                            closeDialog();
                            confirmChessReset();
                        } else if (dialogMessageKind ==
                                   MessageKind::RacingQuitConfirmation) {
                            closeDialog();
                            confirmRacingQuit();
                        } else if (dialogMessageKind ==
                                   MessageKind::DraughtsAbortConfirmation) {
                            finishDraughtsAbort(true);
                            closeDialog();
                        } else if (dialogMessageKind ==
                                   MessageKind::ActivitypageClearConfirmation) {
                            closeDialog();
                            confirmActivitypageClear();
                        } else if (dialogMessageKind ==
                                   MessageKind::ActivitypagePrintConfirmation) {
                            closeDialog();
                            confirmActivitypagePrint();
                        } else if (dialogMessageKind ==
                                   MessageKind::TournamentAbortConfirmation) {
                            closeDialog();
                            abortTournament();
                        } else if (dialogMessageKind ==
                                   MessageKind::ColorfillConfirmation) {
                            closeDialog();
                            colorfillConfirmed(true);
                        } else {
                            closeDialog();
                        }
                    }
                } else if (dialogMode == DialogMode::Help) {
                    if (key == SDLK_ESCAPE || key == SDLK_RETURN ||
                        key == SDLK_KP_ENTER) {
                        closeDialog();
                    } else if (key == SDLK_UP || key == SDLK_LEFT) {
                        if (helpTopicIndex > 0) {
                            selectHelpTopic(helpTopicIndex - 1U);
                        }
                    } else if (key == SDLK_DOWN || key == SDLK_RIGHT) {
                        if (helpTopicIndex + 1U < helpPages.size()) {
                            selectHelpTopic(helpTopicIndex + 1U);
                        }
                    } else if (key == SDLK_PAGEUP) {
                        scrollHelpBody(-std::max(
                            1, 292 / std::max(dialogFont.height, 1)));
                    } else if (key == SDLK_PAGEDOWN) {
                        scrollHelpBody(std::max(
                            1, 292 / std::max(dialogFont.height, 1)));
                    } else if (key == SDLK_HOME && !helpPages.empty()) {
                        selectHelpTopic(0);
                    } else if (key == SDLK_END && !helpPages.empty()) {
                        selectHelpTopic(helpPages.size() - 1U);
                    }
                } else if (dialogMode == DialogMode::HighScore) {
                    if (key == SDLK_ESCAPE || key == SDLK_RETURN ||
                        key == SDLK_KP_ENTER) {
                        closeDialog();
                    }
                } else if (dialogMode == DialogMode::Award &&
                           (key == SDLK_ESCAPE || key == SDLK_RETURN ||
                            key == SDLK_KP_ENTER)) {
                    closeDialog();
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                dialogPressedControl =
                    dialogControlAt(dialogPointX(event.button.x), dialogPointY(event.button.y));
                dialogPressedDay =
                    dialogPressedControl == DialogControl::CalendarDay
                        ? calendarDayAt(event.button.x, event.button.y)
                        : 0;
                if (dialogPressedControl == DialogControl::HelpTopic) {
                    if (const auto topic =
                            helpTopicAt(event.button.x, event.button.y)) {
                        selectHelpTopic(*topic);
                    }
                } else if (dialogPressedControl ==
                           DialogControl::HelpScrollTrack) {
                    const SDL_Rect thumb = helpScrollThumbBounds();
                    const SDL_Point pointer{
                        event.button.x, event.button.y};
                    if (SDL_PointInRect(&pointer, &thumb)) {
                        helpScrollDragging = true;
                        helpScrollDragOffsetY = event.button.y - thumb.y;
                    } else {
                        const int page = std::max(
                            1, 292 / std::max(dialogFont.height, 1));
                        scrollHelpBody(event.button.y < thumb.y ? -page : page);
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONUP &&
                       event.button.button == SDL_BUTTON_LEFT) {
                const DialogControl releasedControl =
                    dialogControlAt(dialogPointX(event.button.x), dialogPointY(event.button.y));
                const DialogControl pressedControl = dialogPressedControl;
                const int pressedDay = dialogPressedDay;
                dialogPressedControl = DialogControl::None;
                dialogPressedDay = 0;
                helpScrollDragging = false;
                if (releasedControl != pressedControl ||
                    pressedControl == DialogControl::None) {
                    continue;
                }
                if (pressedControl == DialogControl::NameOk) {
                    acceptNameDialog();
                } else if (pressedControl == DialogControl::NameCancel) {
                    closeDialog();
                } else if (pressedControl == DialogControl::CalendarPrevious ||
                           pressedControl == DialogControl::CalendarNext) {
                    static constexpr std::array<int, 12> daysInMonth{
                        31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                    if (pressedControl == DialogControl::CalendarPrevious) {
                        pendingBirthMonth = (pendingBirthMonth + 11) % 12;
                    } else {
                        pendingBirthMonth = (pendingBirthMonth + 1) % 12;
                    }
                    pendingBirthDay = std::min(
                        pendingBirthDay, daysInMonth[pendingBirthMonth]);
                } else if (pressedControl == DialogControl::CalendarOk) {
                    acceptCalendarDialog();
                } else if (pressedControl == DialogControl::CalendarDay &&
                           calendarDayAt(event.button.x, event.button.y) ==
                               pressedDay) {
                    pendingBirthDay = pressedDay;
                } else if (pressedControl == DialogControl::MessagePrimary) {
                    if (dialogMessageKind == MessageKind::ExitConfirmation) {
                        running = false;
                    } else if (dialogMessageKind ==
                               MessageKind::MorningRideConfirmation) {
                        closeDialog();
                        enterScene("mountain", "mountain");
                    } else if (dialogMessageKind ==
                               MessageKind::BuildacarDemolishConfirmation) {
                        closeDialog();
                        demolishBuildacar();
                    } else if (dialogMessageKind ==
                               MessageKind::ChessAbortConfirmation) {
                        closeDialog();
                        confirmChessReset();
                    } else if (dialogMessageKind ==
                               MessageKind::RacingQuitConfirmation) {
                        closeDialog();
                        confirmRacingQuit();
                    } else if (dialogMessageKind ==
                               MessageKind::DraughtsAbortConfirmation) {
                        finishDraughtsAbort(true);
                        closeDialog();
                    } else if (dialogMessageKind ==
                               MessageKind::ActivitypageClearConfirmation) {
                        closeDialog();
                        confirmActivitypageClear();
                    } else if (dialogMessageKind ==
                               MessageKind::ActivitypagePrintConfirmation) {
                        closeDialog();
                        confirmActivitypagePrint();
                    } else if (dialogMessageKind ==
                               MessageKind::TournamentAbortConfirmation) {
                        closeDialog();
                        abortTournament();
                    } else if (dialogMessageKind ==
                               MessageKind::ColorfillConfirmation) {
                        closeDialog();
                        colorfillConfirmed(true);
                    } else {
                        closeDialog();
                    }
                } else if (pressedControl == DialogControl::MessageSecondary) {
                    const bool continueStory =
                        dialogMessageKind ==
                        MessageKind::MorningRideConfirmation;
                    closeDialog();
                    if (continueStory) {
                        enterScene("town", "town", 0);
                    }
                } else if (pressedControl == DialogControl::AwardClose) {
                    closeDialog();
                } else if (pressedControl ==
                           DialogControl::HighScoreProfile) {
                    highScoreTab = HighScoreTab::Profile;
                } else if (pressedControl ==
                           DialogControl::HighScoreGlobal) {
                    highScoreTab = HighScoreTab::Global;
                    fillSharedGlobalRows();  // newer scores may have arrived
                } else if (pressedControl ==
                           DialogControl::HighScoreUnion) {
                    highScoreTab = HighScoreTab::Union;
                } else if (pressedControl == DialogControl::HighScoreClose) {
                    closeDialog();
                } else if (pressedControl == DialogControl::HelpOk) {
                    closeDialog();
                } else if (pressedControl == DialogControl::HelpPrevious) {
                    if (helpTopicIndex > 0) {
                        selectHelpTopic(helpTopicIndex - 1U);
                    }
                } else if (pressedControl == DialogControl::HelpNext) {
                    if (helpTopicIndex + 1U < helpPages.size()) {
                        selectHelpTopic(helpTopicIndex + 1U);
                    }
                } else if (pressedControl == DialogControl::HelpScrollUp) {
                    scrollHelpBody(-1);
                } else if (pressedControl == DialogControl::HelpScrollDown) {
                    scrollHelpBody(1);
                }
            }
            continue;
        } else if (viewMode == ViewMode::Scene && activitypageGame &&
                   handleActivitypageEvent(event)) {
            continue;
        } else if (viewMode == ViewMode::Scene && racemenuGame &&
                   handleRacemenuEvent(event)) {
            continue;
        } else if (viewMode == ViewMode::Scene && racingGame &&
                   handleRacingEvent(event)) {
            continue;
        } else if (viewMode == ViewMode::Scene && buttercolAlbum &&
                   handleButtercolEvent(event)) {
            continue;
        } else if (viewMode == ViewMode::Scene && creditsGame &&
                   handleCreditsEvent(event)) {
            continue;
        } else if (storyNavigationOpen) {
            if ((event.type == SDL_KEYDOWN && !event.key.repeat &&
                 event.key.keysym.sym == SDLK_ESCAPE) ||
                (event.type == SDL_MOUSEBUTTONDOWN &&
                 event.button.button == SDL_BUTTON_RIGHT)) {
                closeStoryNavigation();
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                const auto control = storyNavigationControlAt(
                    event.button.x, event.button.y);
                const bool disabled =
                    (control == StoryNavigationControl::Previous &&
                     storyPreviousScene.empty()) ||
                    (control == StoryNavigationControl::Next &&
                     storyNextScene.empty());
                pressedStoryNavigationControl =
                    disabled ? StoryNavigationControl::None : control;
            } else if (event.type == SDL_MOUSEBUTTONUP &&
                       event.button.button == SDL_BUTTON_LEFT) {
                const auto released = storyNavigationControlAt(
                    event.button.x, event.button.y);
                const auto pressed = pressedStoryNavigationControl;
                pressedStoryNavigationControl =
                    StoryNavigationControl::None;
                if (pressed != StoryNavigationControl::None &&
                    pressed == released) {
                    activateStoryNavigation(pressed);
                }
            }
            continue;
        } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                   event.button.button == SDL_BUTTON_RIGHT &&
                   viewMode == ViewMode::Scene && activeScene == "fence" &&
                   fenceGame &&
                   beginFenceInteraction(event.button.x, event.button.y,
                                         true)) {
            continue;
        } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                   event.button.button == SDL_BUTTON_RIGHT &&
                   viewMode == ViewMode::Scene && jigsawGame &&
                   beginJigsawRightInteraction(event.button.x,
                                               event.button.y)) {
            continue;
        } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                   event.button.button == SDL_BUTTON_RIGHT &&
                   viewMode == ViewMode::Scene && activeScene == "pipeline" &&
                   pipelineGame &&
                   rotatePipelineAt(event.button.x, event.button.y)) {
            continue;
        } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                   event.button.button == SDL_BUTTON_RIGHT &&
                   viewMode == ViewMode::Scene &&
                   !sceneSequenceBlocksInput) {
            if (openStoryNavigation(event.button.x, event.button.y)) {
                continue;
            }
        } else if (event.type == SDL_TEXTINPUT &&
                   viewMode == ViewMode::Scene && fenceGame &&
                   typeFenceText(event.text.text)) {
            continue;
        } else if (event.type == SDL_TEXTINPUT &&
                   viewMode == ViewMode::Scene && textinvaderGame &&
                   typeTextinvader(event.text.text)) {
            continue;
        } else if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
                   viewMode == ViewMode::Scene && beemazeGame &&
                   handleBeemazeKey(event.key.keysym.sym,
                                    event.type == SDL_KEYDOWN)) {
            // Bee Maze reads every WM_KEYDOWN, auto-repeat included, and the
            // matching WM_KEYUP (0x0040ea0b, 0x0040e9fc).
            continue;
        } else if (event.type == SDL_KEYDOWN && viewMode == ViewMode::Scene &&
                   mainscoreBoard &&
                   handleMainscoreKey(event.key.keysym.sym)) {
            continue;
        } else if (event.type == SDL_KEYDOWN && viewMode == ViewMode::Scene &&
                   tournamentScreen && dialogMode == DialogMode::None &&
                   handleTournamentKey(event.key.keysym.sym)) {
            continue;
        } else if (event.type == SDL_KEYUP && viewMode == ViewMode::Scene &&
                   wheelbarrowGame &&
                   handleWheelbarrowKey(event.key.keysym.sym, false)) {
            continue;
        } else if (event.type == SDL_KEYUP && viewMode == ViewMode::Scene &&
                   mountainGame &&
                   handleMountainKey(event.key.keysym.sym, false)) {
            continue;
        } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
            // Both story scenes cancel pending narration from their key-press
            // handlers as well as their mouse-down handlers (yard 0x00428630,
            // desk 0x00424ca0). This is not redundant with the click path: a
            // capture against the original shows keyboard events reach the
            // scene during an opening while pointer hotspot dispatch is still
            // gated, so the keyboard is the only way a player can cancel a
            // narration armed while the opening is still running.
            if (viewMode == ViewMode::Scene) {
                cancelPendingNarrations();
            }
            if (viewMode == ViewMode::Scene &&
                handleViewerKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && balloonhuntGame &&
                handleBalloonhuntKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && jigsawGame &&
                jigsawKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && slidingGame &&
                slidingKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && bugzzzGame &&
                handleBugzzzKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && wheelbarrowGame &&
                handleWheelbarrowKey(event.key.keysym.sym, true)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && goodbyeState &&
                handleGoodbyeKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && buildacarWorkshop &&
                handleBuildacarKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && outroState &&
                handleOutroKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && mountainGame &&
                handleMountainKey(event.key.keysym.sym, true)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && chessGame &&
                handleChessKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && fenceGame &&
                handleFenceKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene &&
                textinvaderClaimsKey(event.key.keysym.sym)) {
                // Letters reach Text Invader as SDL_TEXTINPUT; the native
                // shortcuts below (h, m, q, r) must not see them.
                continue;
            }
            if (viewMode == ViewMode::Scene && draughtsGame &&
                handleDraughtsKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && colorfillGame &&
                colorfillKey(event.key.keysym.sym)) {
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "birthday" &&
                birthdayGame && handleBirthdayKey(event.key.keysym.sym)) {
                continue;
            }
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                if (viewMode == ViewMode::Scene && activeScene == "logo" &&
                    sceneSequenceBlocksInput) {
                    // The original treats Escape during credits as immediate
                    // completion, including its birthday callback, rather
                    // than as ordinary scene-back navigation.
                    enterScene("birthday", "birthday");
                } else if (viewMode == ViewMode::Scene &&
                           activeScene == "birthday" && birthdayGame) {
                    // Leaving runs the scene's 0x104 handler, which may open
                    // awards. UNCERTAIN: the scene's own key handler
                    // (0x00418980) reads only Space; Escape is taken to reach
                    // the same parent jump.
                    leaveBirthday();
                } else if (viewMode == ViewMode::Menu) {
                    showExitConfirmation();
                } else if (viewMode == ViewMode::Options) {
                    leaveOptions(false);
                } else if (viewMode == ViewMode::Scene) {
                    handleSceneEscape();
                }
                break;
            case SDLK_q:
                showExitConfirmation();
                break;
            case SDLK_TAB:
                if (viewMode == ViewMode::Menu) {
                    enterScene("sceneindex", "Contents");
                }
                break;
            case SDLK_h:
                showHotspots = !showHotspots;
                break;
            case SDLK_SPACE:
                if (viewMode == ViewMode::Scene) {
                    skipSceneForeground();
                }
                break;
            case SDLK_m:
                musicEnabled = !musicEnabled;
                SDL_PauseAudioDevice(audioDevice,
                                     musicEnabled && !paused
                                         ? 0
                                         : 1);
                break;
            case SDLK_RIGHT:
            case SDLK_UP:
                audioVolumes[2] = std::min(1.0F, audioVolumes[2] + 0.1F);
                if (viewMode != ViewMode::Scene) {
                    audioMixer.setBackgroundVolume(audioVolumes[2] *
                                                   sceneAudioGains[2]);
                }
                if (musicEnabled && !paused && viewMode != ViewMode::Scene) {
                    SDL_PauseAudioDevice(audioDevice, 0);
                }
                break;
            case SDLK_LEFT:
            case SDLK_DOWN:
                audioVolumes[2] = std::max(0.0F, audioVolumes[2] - 0.1F);
                if (viewMode != ViewMode::Scene) {
                    audioMixer.setBackgroundVolume(audioVolumes[2] *
                                                   sceneAudioGains[2]);
                }
                if (musicEnabled && !paused && viewMode != ViewMode::Scene) {
                    SDL_PauseAudioDevice(audioDevice, 0);
                }
                break;
            case SDLK_r:
                start = std::chrono::steady_clock::now();
                pausedAt = 0.0;
                currentFrame = 0;
                if (viewMode == ViewMode::Scene && !sceneClips.empty()) {
                    sceneClipIndex = 0;
                    sceneSequenceRequestedAt = 0.0;
                    sceneClipTimelineStart = sceneClipStartOffsets.empty()
                                                 ? 0.0
                                                 : sceneClipStartOffsets.front();
                    sceneClipStarted = sceneClipTimelineStart <= 0.0;
                    sceneSequenceHeldConflictMask = sceneClipStarted
                                                            ? sceneClips.front().conflictMask
                                                            : 0U;
                    sceneSequencePendingConflictMask = 0;
                    sceneSequenceBlocksInput = activeScene == "yard";
                    sceneSequenceFinished = false;
                    if (sceneUsesAudioTimeline && sceneClipStarted) {
                        audioMixer.setForeground(
                            sceneAudioTimeline,
                            audioVolumes[1] * sceneAudioGains[1],
                            &sceneEffectTimeline,
                            audioVolumes[0] * sceneAudioGains[0]);
                    } else if (sceneClipStarted) {
                        audioMixer.setForeground(
                            sceneClips.front().media,
                            clipAudioVolume(sceneClips.front().media));
                    }
                } else if (viewMode != ViewMode::Scene) {
                    audioMixer.setBackground(
                        music, audioVolumes[2] * sceneAudioGains[2]);
                }
                if (musicEnabled && !paused &&
                    (viewMode != ViewMode::Scene || !sceneClips.empty())) {
                    SDL_PauseAudioDevice(audioDevice, 0);
                }
                break;
            case SDLK_F1:
                if (viewMode == ViewMode::Menu) {
                    setMenuTitle("F1 Help: hover over an icon to identify it");
                }
                break;
            default:
                break;
            }
        } else if (event.type == SDL_MOUSEMOTION) {
            // SDL_RenderSetLogicalSize filters mouse events into the
            // renderer's 800x600 coordinates, including HiDPI and its
            // letterboxed viewport. Applying either transform again
            // makes hit testing drift as the window is resized.
            const int x = event.motion.x;
            const int y = event.motion.y;
            if (sceneSequenceBlocksInput) {
                continue;
            }

            if (viewMode == ViewMode::Options &&
                optionsDraggingChannel >= 0) {
                // Every movement during a slider drag posts 0x10a
                // (0x0040c758), which the dialog turns into a preview
                // request (0x004230d0) before moving the thumb.
                previewOptionsChannel(optionsDraggingChannel);
                setOptionsSliderFromMouse(optionsDraggingChannel, x);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "pipeline" &&
                pipelineGame) {
                movePipelinePointer(x, y);
            }
            if (viewMode == ViewMode::Scene && activeScene == "lettersort" &&
                lettersortGame && lettersortGame->draggingLetter) {
                moveLettersortLetter(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "buildabike" &&
                buildabikeGame && buildabikeGame->dragging) {
                moveBuildabikeInteraction(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "hustle" &&
                hustleGame && hustleGame->betDragging) {
                moveHustleBet(y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "chess" &&
                chessGame && chessGame->dragPiece >= 0) {
                moveChessInteraction(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "jigsaw" &&
                jigsawGame) {
                moveJigsawInteraction(x, y);
            }
            if (viewMode == ViewMode::Scene && activeScene == "sliding" &&
                slidingGame) {
                moveSlidingInteraction(x, y);
            }
            if (viewMode == ViewMode::Scene && activeScene == "bouquet" &&
                bouquetGame && bouquetDragging()) {
                moveBouquetInteraction(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "fence" &&
                fenceGame &&
                (fenceGame->dragging || fenceGame->scrollDragging)) {
                moveFenceInteraction(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "puzzle" &&
                puzzleGame) {
                movePuzzlePointer(x, y);
            }
            if (viewMode == ViewMode::Scene && activeScene == "draughts" &&
                draughtsGame && draughtsGame->dragPiece >= 0) {
                moveDraughtsInteraction(x, y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "synonym" &&
                synonymGame && synonymGame->draggingLetter) {
                moveSynonymInteraction(x,y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "mathlab" &&
                mathlabDragging()) {
                moveMathlabInteraction(x,y);
                continue;
            }
            if (viewMode == ViewMode::Scene && activeScene == "mahjong" &&
                mahjongGame) {
                moveMahjongPointer(
                    x, y, (event.motion.state & SDL_BUTTON_LMASK) != 0);
            }
            if (viewMode == ViewMode::Scene && activeScene == "mainscore" &&
                mainscoreBoard) {
                moveMainscorePointer(x, y);
            }
            if (viewMode == ViewMode::Scene && tournamentScreen) {
                moveTournamentPointer(x, y);
            }
            if (viewMode == ViewMode::Scene && activeScene == "colorfill" &&
                colorfillGame) {
                // WM_MOUSEMOVE 0x0040eb0e: 0x109 with the left button held,
                // else 0x108.
                moveColorfillPointer(
                    x, y, (event.motion.state & SDL_BUTTON_LMASK) != 0U);
            }
            const auto hotspot = viewMode == ViewMode::Options
                                     ? 0U
                                     : hotspotAt(x, y);
            if (hotspot != hoveredHotspot) {
                referenceTrace.emit("hotspot_resolve", {{"logical_x", x},
                                                         {"logical_y", y},
                                                         {"hotspot", hotspot},
                                                         {"reason", "hover"}});
                hoveredHotspot = hotspot;
                hoveredBounds = currentHotspotBounds(hotspot);
                if (viewMode == ViewMode::Menu) {
                    if (hotspot >= 1 && hotspot <= 13) {
                        startAmpEffect(1);
                    }
                    setMenuTitle(menuHotspotName(hotspot));
                } else if (viewMode == ViewMode::Scene && hotspot != 0) {
                    const std::string title =
                        "FGP Native — " + activeScene + " — hotspot " +
                        std::to_string(hotspot) + " · exit control back";
                    SDL_SetWindowTitle(window, title.c_str());
                }
            }
        } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                   event.button.button == SDL_BUTTON_LEFT) {
            const int x = event.button.x;
            const int y = event.button.y;

            if (viewMode == ViewMode::Menu) {
                // The Windows handler raises the ammeter and starts the
                // top-right fuse strip on every left-button press.
                startAmpEffect(2);
                if (const auto button = menuCornerButtonAt(x, y)) {
                    pressedMenuCornerButton = *button;
                    continue;
                }
                pressedMenuCornerButton.reset();
            }

            if (viewMode == ViewMode::Options) {
                optionsPressedControl = optionsControlAt(x, y);
                const int channel =
                    optionsChannelForControl(optionsPressedControl);
                if (optionsPressedControl == OptionsControl::EffectTrack ||
                    optionsPressedControl == OptionsControl::SpeechTrack ||
                    optionsPressedControl == OptionsControl::MusicTrack) {
                    // A press on the track jumps the thumb there and starts
                    // a drag (0x004092d0); the jump posts the value change
                    // 0x117 only when the value moved (0x0040906b).
                    optionsDraggingChannel = channel;
                    if (setOptionsSliderFromMouse(channel, x)) {
                        previewOptionsChannel(channel);
                    }
                }
                continue;
            }

            // A running sequence swallows the click; it does not skip. Starting
            // one sets cursor mode 0 (0x0040a74d via 0x0040c370), and while the
            // mode at 0x0054c248 is 0 or 1 the cursor module's gate 0x0040f210
            // consumes mouse messages 0x105-0x109 before the scene sees them.
            // The intro-navigation capture shows it: a left click during
            // S00AN-SS-001a changes nothing and the opening plays on.
            if (sceneSequenceBlocksInput ||
                (viewMode == ViewMode::Scene &&
                 sceneCharacterReactionBlocksInput())) {
                // A swallowed developer click must not leak its hotspot
                // into the next real one.
                developerHotspotOverride.reset();
                continue;
            }

            if (viewMode == ViewMode::Scene && activeScene == "draughts" &&
                draughtsGame && draughtsGame->waitCursor) {
                continue;
            }
            if (viewMode == ViewMode::Scene) {
                if (beginViewerInteraction(x, y)) {
                    continue;
                }
                // Mahjong's 0x105 handler sees every press, Help and Exit
                // included; it records the click for its next tick.
                if (activeScene == "mahjong" && mahjongGame &&
                    beginMahjongInteraction(x, y) &&
                    !sceneCornerButtonAt(x, y)) {
                    continue;
                }
                if (const auto button = sceneCornerButtonAt(x, y);
                    button && fenceControlsEnabled()) {
                    pressedSceneCornerButton = *button;
                    if (colorfillGame) {
                        colorfillCornerButton(
                            sceneCornerButtons[*button].kind ==
                                SceneCornerButtonKind::Help,
                            false, x, y);
                    }
                    continue;
                }
                pressedSceneCornerButton.reset();
                if (activeScene == "buildacar" &&
                    beginBuildacarInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "lettersort" && lettersortGame &&
                    beginLettersortInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "buildabike" && buildabikeGame &&
                    beginBuildabikeInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "hustle" && hustleGame &&
                    beginHustleInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "chess" && chessGame &&
                    beginChessInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "jigsaw" && jigsawGame &&
                    beginJigsawInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "sliding" && slidingGame &&
                    beginSlidingInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "fence" && fenceGame &&
                    beginFenceInteraction(x, y, false)) {
                    continue;
                }
                if (activeScene == "puzzle" && puzzleGame &&
                    beginPuzzleInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "pipeline" && pipelineGame &&
                    beginPipelineInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "bouquet" && bouquetGame &&
                    beginBouquetInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "draughts" && draughtsGame &&
                    beginDraughtsInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "mountain" && mountainGame &&
                    beginMountainInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "synonym" && synonymGame &&
                    beginSynonymInteraction(x,y)) {
                    continue;
                }
                if (activeScene == "mathlab" && mathlabGame &&
                    beginMathlabInteraction(x,y)) {
                    continue;
                }
                if (activeScene == "mainscore" && mainscoreBoard &&
                    beginMainscoreInteraction(x, y)) {
                    continue;
                }
                if ((activeScene == "tournament" ||
                     activeScene == "tourneyscore") &&
                    tournamentScreen && beginTournamentInteraction(x, y)) {
                    continue;
                }
                if (activeScene == "gametrivia" && triviaGame &&
                    beginTriviaInteraction(x,y)) {
                    continue;
                }
                if (activeScene == "colorfill" && colorfillGame &&
                    beginColorfillInteraction(x, y)) {
                    continue;
                }
            }

            // 0x004283e0 (yard) and 0x00424c60 (desk) run at the very top of
            // their mouse-down handlers: any click drops the scene's pending
            // narration, so a narration the player has already acted past is
            // never heard.
            cancelPendingNarrations();
            const auto hotspot = developerHotspotOverride
                                     ? *developerHotspotOverride
                                     : hotspotAt(x, y);
            developerHotspotOverride.reset();
            std::cout << "Click at " << x << ',' << y << " -> hotspot value "
                      << hotspot << '\n';
            referenceTrace.emit("hotspot_resolve", {{"logical_x", x},
                                                     {"logical_y", y},
                                                     {"hotspot", hotspot},
                                                     {"reason", "click"}});
            referenceTrace.emit("click_dispatch", {{"scene", activeScene},
                                                     {"hotspot", hotspot}});
            if (viewMode == ViewMode::Scene) {
                // Intent's two desk hotspots start multi-clip foreground
                // sequences rather than the single animation descriptors
                // represented by the generated fallback below. The original
                // mouse dispatcher calls these exact tables at 0x00425A13
                // (hotspot 7) and 0x00425A21 (hotspot 8).
                if (activeScene == "intent" && hotspot == 7U) {
                    startSceneSequence(
                        executableGameData.animationSequenceAtRva(0x000bd358U),
                        "hotspot 7 intent sequence");
                    continue;
                }
                if (activeScene == "intent" && hotspot == 8U) {
                    startSceneSequence(
                        executableGameData.animationSequenceAtRva(0x000bb700U),
                        "hotspot 8 intent sequence");
                    continue;
                }
                if (activeScene == "audiopairs" && audiopairsGame) {
                    if (hotspot == 3U) {
                        startAudiopairsGame();
                    } else if (hotspot >= 10U && hotspot < 30U) {
                        pressAudiopairsButton(
                            static_cast<std::size_t>(hotspot - 10U));
                    }
                    continue;
                }
                if (activeScene == "sockdrawer" && sockdrawerGame) {
                    if (hotspot == 3U) {
                        startSockdrawerGame();
                    } else if (hotspot >= 101U && hotspot <= 120U) {
                        pressSockdrawerDrawer(static_cast<std::size_t>(
                            hotspot - 101U));
                    }
                    continue;
                }
                if (activeScene == "butterfly" && butterflyGame) {
                    if (hotspot == 1U) {
                        startButterflyGame();
                    } else if (hotspot == 4U) {
                        enterScene("buttercol", "buttercol");
                    } else if (butterflyGame->phase ==
                               ButterflyPhase::Playing) {
                        catchButterflyAt(x, y);
                    }
                    continue;
                }
                if (activeScene == "whackamole" && whackamoleGame) {
                    if (hotspot == 34U) {
                        startWhackamoleGame();
                    } else if (hotspot >= 1U && hotspot <= 12U) {
                        pressWhackamoleHole(
                            static_cast<std::size_t>(hotspot - 1U));
                    }
                    continue;
                }
                if (activeScene == "balloonhunt" && balloonhuntGame) {
                    pressBalloonhuntHotspot(hotspot);
                    continue;
                }
                if (activeScene == "hopscotch" && hopscotchGame) {
                    pressHopscotchAt(x, y, hotspot);
                    continue;
                }
                if (activeScene == "beemaze" && beemazeGame) {
                    if (hotspot == 101U) startBeemazeGame();
                    continue;
                }
                if (activeScene == "bugzzz" && bugzzzGame) {
                    pressBugzzzHotspot(hotspot);
                    continue;
                }
                if (activeScene == "wheelbarrow" && wheelbarrowGame) {
                    if (hotspot == 301U) startWheelbarrowGame();
                    continue;
                }
                if (activeScene == "mountain" && mountainGame) {
                    continue;
                }
                if (activeScene == "textinvader" && textinvaderGame) {
                    pressTextinvaderHotspot(hotspot);
                    continue;
                }
                if (recoveredMinigameRuntime &&
                    playRecoveredMinigameHotspot(hotspot)) {
                    continue;
                }
                const auto collectible = std::find_if(
                    sceneCollectibles.begin(), sceneCollectibles.end(),
                    [&](const SceneCollectible &candidate) {
                        return candidate.visible &&
                               candidate.hotspot == hotspot;
                    });
                if (collectible != sceneCollectibles.end()) {
                    const fs::path profilePath =
                        profileDirectory /
                        (profiles[profileIndex] + ".pfl");
                    try {
                        if (collectible->name == "carpart") {
                            const auto partNumber =
                                awardCarPart(profilePath, activeScene);
                            collectible->visible = false;
                            showCarPartAward(partNumber);
                        } else if (collectible->name == "3dglasses" &&
                                   collectible->storyIndex >= 0) {
                            awardAnaglyphScene(
                                profilePath,
                                static_cast<std::uint32_t>(
                                    collectible->storyIndex));
                            collectible->visible = false;
                            showAnaglyphAward();
                        }
                    } catch (const std::exception &error) {
                        showMessage(
                            std::string("Kunne ikke lagre premien: ") +
                            error.what());
                    }
                    continue;
                }
                if (activeScene == "yard") {
                    if (handleYardHotspot(hotspot)) {
                        continue;
                    }
                } else if (activeScene == "desk") {
                    if (handleDeskHotspot(hotspot)) {
                        continue;
                    }
                } else if (activeScene == "pee") {
                    if (handlePeeHotspot(hotspot)) {
                        continue;
                    }
                }
                // The response each scene callback starts for this hotspot
                // (tools/fgp_click_scan.py). A random one picks through the
                // shared no-repeat selector 0x0040a150, whose state, like
                // the original's globals, lasts for the process.
                const auto response = std::find_if(
                    fgp_generated::kStoryClickResponses.begin(),
                    fgp_generated::kStoryClickResponses.end(),
                    [&](const auto &candidate) {
                        return candidate.scene == activeScene &&
                               candidate.hotspot == hotspot;
                    });
                if (response != fgp_generated::kStoryClickResponses.end()) {
                    std::size_t variant = 0;
                    if (response->pickCount > 1U) {
                        auto &bag = generatedClickBags[
                            activeScene + ":" + std::to_string(hotspot)];
                        variant = nextNoRepeatOption(bag.selector,
                                                     response->pickCount);
                    }
                    const auto &pick = response->picks[variant];
                    const std::string label =
                        "generated hotspot " + std::to_string(hotspot) +
                        " response";
                    if (pick.table) {
                        startSceneSequence(
                            executableGameData.animationSequenceAtRva(
                                pick.rva),
                            label);
                    } else {
                        startSceneAnimation(pick.rva, label);
                    }
                    continue;
                }
                if (activeScene == "sceneindex") {
                    if (const auto tab = sceneIndexTabAt(x, y)) {
                        selectSceneIndexTab(*tab);
                        continue;
                    }
                    const SDL_Point point{x, y};
                    for (std::size_t index = 0;
                         index < sceneIndexTileBounds.size() &&
                         index < sceneIndexNames.size();
                         ++index) {
                        if (!SDL_PointInRect(
                                &point, &sceneIndexTileBounds[index])) {
                            continue;
                        }
                        if (sceneIndexNames[index].empty()) {
                            break;
                        }
                        if (sceneIndexUnlocked[index]) {
                            const std::string selected = sceneIndexNames[index];
                            enterScene(selected, selected);
                        } else {
                            const std::string title =
                                "FGP Native — " + sceneIndexNames[index] +
                                " is locked in profile " +
                                profiles[profileIndex] + " · exit control back";
                            SDL_SetWindowTitle(window, title.c_str());
                        }
                        break;
                    }
                    continue;
                }
                // The group scheduler's mouse handler 0x0040a980 sits at
                // priority -10 too and returns 0, so a press on a
                // character the scene left alone plays its reaction.
                if (requestSceneCharacterReaction(hotspot)) {
                    continue;
                }
                // The shared fact handler (0x0041dc70) is registered at
                // dispatch priority -10, so it only sees a hotspot the
                // scene's own dispatcher left unhandled. That is why the
                // scene dispatchers carry no cases for the fact ranges.
                if (showFactPage(hotspot)) {
                    continue;
                }
                // Navigation recovered from the original's own dispatchers.
                // This sits after the hand-written scene handlers and the
                // shared fact handler, matching the original's priority
                // ordering: a scene's dispatcher gets first refusal, and only
                // what it leaves unhandled reaches the shared layers.
                if (navigateGeneratedHotspot(hotspot)) {
                    continue;
                }
                const std::string title =
                    "FGP Native — " + activeScene + " — hotspot " +
                    std::to_string(hotspot) +
                    " detected; scene logic is the next layer · exit control back";
                SDL_SetWindowTitle(window, title.c_str());
                continue;
            }

            switch (hotspot) {
            case 1:
                showMenuHelp();
                break;
            case 2:
                showExitConfirmation();
                break;
            case 3:
                enterOptions();
                break;
            // Entries 4-8 and 13 select the profile before they leave
            // (0x00418056, 0x00418086, 0x004180d3 call 0x0040d860), which
            // reads its options again.
            case 4:
                loadProfileOptions();
                enterScene("mainscore", "Scoreboard");
                break;
            case 5:
                loadProfileOptions();
                enterScene("intro", "Opening scene");
                break;
            case 6:
                loadProfileOptions();
                enterScene("sceneindex", "Contents");
                break;
            case 7:
                loadProfileOptions();
                enterScene("tournament", "Tournament");
                break;
            case 8:
                loadProfileOptions();
                enterRacemenuFromMenu();
                break;
            case 9:
                openNameDialog();
                break;
            case 10:
                setMenuTitle("Profile deletion is disabled in this proof of concept");
                break;
            case 11:
                profileIndex = (profileIndex + profiles.size() - 1) % profiles.size();
                setMenuTitle("Previous profile");
                break;
            case 12:
                profileIndex = (profileIndex + 1) % profiles.size();
                setMenuTitle("Next profile");
                break;
            case 13:
                loadProfileOptions();
                enterScene("credits", "Credits");
                break;
            case 14:
                setMenuTitle("Hidden car hotspot discovered");
                break;
            default:
                break;
            }
        } else if (event.type == SDL_MOUSEBUTTONUP &&
                   event.button.button == SDL_BUTTON_LEFT &&
                   viewMode == ViewMode::Menu) {
            const auto released =
                menuCornerButtonAt(event.button.x, event.button.y);
            const auto pressed = pressedMenuCornerButton;
            pressedMenuCornerButton.reset();
            if (!pressed || released != pressed ||
                *pressed >= menuCornerButtons.size()) {
                continue;
            }
            const auto kind = menuCornerButtons[*pressed].kind;
            if (kind == SceneCornerButtonKind::Help) {
                showMenuHelp();
            } else {
                showExitConfirmation();
            }
        } else if (event.type == SDL_MOUSEBUTTONUP &&
                   event.button.button == SDL_BUTTON_LEFT &&
                   viewMode == ViewMode::Scene) {
            if (activeScene == "gametrivia" && triviaGame &&
                triviaGame->draggingQuestion) {
                endTriviaInteraction(event.button.x,event.button.y);
                continue;
            }
            if (activeScene == "colorfill" && colorfillGame) {
                endColorfillInteraction(event.button.x, event.button.y);
            }
            if (activeScene == "lettersort" && lettersortGame &&
                lettersortGame->draggingLetter) {
                endLettersortInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "buildabike" && buildabikeGame &&
                buildabikeGame->dragging) {
                endBuildabikeInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "hustle" && hustleGame &&
                (hustleGame->betDragging || hustleGame->betPressed)) {
                endHustleInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "chess" && chessGame &&
                (chessGame->dragPiece >= 0 || chessGame->pressedButton != 0)) {
                endChessInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "jigsaw" && jigsawGame) {
                endJigsawInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "sliding" && slidingGame) {
                endSlidingInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "bouquet" && bouquetGame &&
                bouquetDragging()) {
                endBouquetInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "fence" && fenceGame &&
                (fenceGame->dragging || fenceGame->scrollDragging ||
                 fenceGame->pressedControl != 0U)) {
                endFenceInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "pipeline" && pipelineGame &&
                pipelineGame->pressedControl != 0U) {
                endPipelineInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "draughts" && draughtsGame &&
                (draughtsGame->dragPiece >= 0 ||
                 draughtsGame->pressedControl != 0)) {
                endDraughtsInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "mountain" && mountainPressing()) {
                endMountainInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "synonym" && synonymGame &&
                synonymGame->draggingLetter) {
                endSynonymInteraction(event.button.x,event.button.y);
                continue;
            }
            if (activeScene == "mathlab" && mathlabGame &&
                (mathlabGame->dragging || mathlabGame->pressedButton != 0)) {
                endMathlabInteraction(event.button.x,event.button.y);
                continue;
            }
            if (activeScene == "mahjong" && mahjongGame &&
                mahjongGame->pressedButton) {
                endMahjongInteraction(event.button.x, event.button.y);
                continue;
            }
            if (activeScene == "mainscore" && mainscoreBoard &&
                (mainscoreBoard->draggingThumb ||
                 mainscoreBoard->pressedArrow != 0)) {
                endMainscoreInteraction();
                continue;
            }
            if (viewMode == ViewMode::Scene && tournamentScreen &&
                dialogMode == DialogMode::None) {
                endTournamentInteraction(event.button.x, event.button.y);
            }
            if (activeScene == "buildacar" && buildacarInteractionActive()) {
                endBuildacarInteraction(event.button.x, event.button.y);
                continue;
            }
            if (viewMode == ViewMode::Scene &&
                endViewerInteraction(event.button.x, event.button.y)) {
                continue;
            }
            const auto released =
                sceneCornerButtonAt(event.button.x, event.button.y);
            const auto pressed = pressedSceneCornerButton;
            pressedSceneCornerButton.reset();
            if (!pressed || released != pressed ||
                *pressed >= sceneCornerButtons.size()) {
                continue;
            }
            const auto kind = sceneCornerButtons[*pressed].kind;
            if (colorfillGame) {
                colorfillCornerButton(kind == SceneCornerButtonKind::Help,
                                      true, event.button.x, event.button.y);
            }
            if (activeScene == "mahjong") {
                // 0x00450870 receives Help (2) and Exit (1) as controls.
                referenceTrace.emit("click_dispatch", {
                    {"scene", "mahjong"},
                    {"hotspot", kind == SceneCornerButtonKind::Help ? 2 : 1}});
            }
            if (kind == SceneCornerButtonKind::Help) {
                showSceneHelp();
            } else {
                returnFromScene();
            }
        } else if (event.type == SDL_MOUSEBUTTONUP &&
                   event.button.button == SDL_BUTTON_LEFT &&
                   viewMode == ViewMode::Options) {
            const OptionsControl releasedControl =
                optionsControlAt(event.button.x, event.button.y);
            const OptionsControl pressedControl = optionsPressedControl;
            optionsPressedControl = OptionsControl::None;
            // Dropping a slider drag posts 0x10b, which stops that slider's
            // preview sample at once (0x00423130 -> 0x00422b80 event 1).
            stopOptionsPreview(optionsDraggingChannel);
            optionsDraggingChannel = -1;
            if (releasedControl != pressedControl ||
                pressedControl == OptionsControl::None) {
                continue;
            }

            const int channel = optionsChannelForControl(pressedControl);
            const bool decrease =
                pressedControl == OptionsControl::EffectDown ||
                pressedControl == OptionsControl::SpeechDown ||
                pressedControl == OptionsControl::MusicDown;
            const bool increase =
                pressedControl == OptionsControl::EffectUp ||
                pressedControl == OptionsControl::SpeechUp ||
                pressedControl == OptionsControl::MusicUp;
            if (decrease || increase) {
                // The arrows step the integer slider value by 10
                // (0x00409450 -> 0x00408eb0); a change posts 0x117, which
                // also starts the preview sample if it is not playing.
                const int current = static_cast<int>(
                    std::lround(audioVolumes[channel] * 100.0F));
                const int next =
                    std::clamp(current + (increase ? 10 : -10), 0, 100);
                if (next != current) {
                    setOptionsVolume(channel, optionsSliderValue(next));
                    previewOptionsChannel(channel);
                }
            } else if (pressedControl == OptionsControl::SubtitlePrevious) {
                subtitleLanguage = (subtitleLanguage + 3) % 4;
            } else if (pressedControl == OptionsControl::SubtitleNext) {
                subtitleLanguage = (subtitleLanguage + 1) % 4;
            } else if (pressedControl == OptionsControl::SubtitleToggle) {
                subtitlesEnabled = !subtitlesEnabled;
            } else if (pressedControl == OptionsControl::Ok) {
                leaveOptions(true);
            } else if (pressedControl == OptionsControl::Cancel) {
                leaveOptions(false);
            }
        }
    }
}

} // namespace fgp

namespace fgp {

void GameApp::handleSceneEscape() {
    // The window procedure posts every WM_KEYDOWN as event 0x10c with the
    // virtual key (0x0040e9e6 -> 0x0040c7d0), and the dispatcher 0x0040ecb0
    // offers it to the registered handlers, highest priority first, until one
    // returns 1 (0x0040ece8). The scene callback (priority 0, 0x0040bff4)
    // comes first; the last is the shared 0x0040d2c0 (priority -100), which
    // sends VK_ESCAPE (0x1b) through 0x0040d300's table to 0x0040cc30, the
    // registry parent. tools/fgp_escape_scan.py reads what every callback
    // returns for Escape into src/generated/scene_escape.hpp. Scenes with a
    // native handler of their own (Balloon Hunt, the Activity page's text
    // box, Jigsaw, the list dialogs, the Tournament's picker) have already
    // taken the key before this point.
    if (activeScene == "town") {
        // 0x00427010: Escape, like Space, leaves at once for outtent, or
        // buildacar on the mode-2 route, and is consumed (0x00426f7a
        // returns 1 for it); skipSceneForeground has the destinations.
        skipSceneForeground();
        return;
    }
    if (activeScene == "gallery") {
        // 0x00469170 -> 0x004690f0: back to the parent once the TV room has
        // been visited, otherwise to the Contents.
        bool tvroomVisited = false;
        if (profileIndex < profiles.size() &&
            profiles[profileIndex] != "No profile") {
            try {
                tvroomVisited =
                    profileSceneRecordU32(
                        profileDirectory / (profiles[profileIndex] + ".pfl"),
                        "tvroom", 0x400U, 0U, false)
                        .value_or(0U) > 0U;
            } catch (const std::exception &) {
            }
        }
        if (tvroomVisited) {
            returnFromScene();
        } else {
            enterScene("sceneindex", "Contents");
        }
        return;
    }
    if (activeScene == "info") {
        // 0x00469940: back to the scene that opened the page.
        returnFromScene();
        return;
    }
    if (activeScene == "racing") {
        // The race keeps Escape and handles it itself (app_racing.cpp,
        // 0x0047a7f0).
        return;
    }
    const auto row = std::find_if(
        fgp_generated::kSceneEscape.begin(), fgp_generated::kSceneEscape.end(),
        [this](const auto &entry) { return entry.scene == activeScene; });
    if (row == fgp_generated::kSceneEscape.end()) {
        return;
    }
    switch (row->escape) {
    case fgp_generated::SceneEscape::Passes:
        // The callback returns 0, so 0x0040d2c0 sends the scene to its
        // registry parent: the menu from a story scene, the story scene (in
        // mode 1) from a minigame.
        leaveToRegistryParent();
        break;
    case fgp_generated::SceneEscape::Depends:
        if (activeScene == "activity" || activeScene == "textinvader") {
            // activity: 0x0045bf54 keeps Escape only while a scene that
            // handed over a picture is stored ([0x0069dff8]); that case is
            // handled by the page itself. textinvader: 0x0044cfe0 keeps
            // letters and '-' only (isalpha, 0x0044d008, or 0x2d), and 0x1b
            // is a control character, so 0x0044c69a's return 1 is not taken.
            leaveToRegistryParent();
        }
        break;
    case fgp_generated::SceneEscape::Keeps:
        // Handled above or by the scene's own key handler.
        break;
    }
}

void GameApp::performWebBack() {
    const bool pickerOpen = tournamentScreen && tournamentScreen->setup &&
                            tournamentScreen->setup->picker;
    if (viewMode == ViewMode::Scene && dialogMode == DialogMode::None &&
        !storyNavigationOpen && !pickerOpen) {
        // What a tap on the exit control does: during an opening any tap
        // skips it first; otherwise the corner button's release.
        if (sceneSequenceBlocksInput) {
            skipSceneForeground();
        } else {
            returnFromScene();
        }
        return;
    }
    // Elsewhere the button is Escape: a dialog's Cancel, the quit question
    // on the menu, Options' Cancel, skipping the credits.
    for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
        SDL_Event key{};
        key.type = type;
        key.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        key.key.keysym.sym = SDLK_ESCAPE;
        key.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        SDL_PushEvent(&key);
    }
}

} // namespace fgp
