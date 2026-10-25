#include "game/app.h"

#include <algorithm>
#include <bit>
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
#include "game/menu_names.h"
#include "media/audio.h"
#include "platform/web_audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "platform/remote_data.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

void AppCore::uploadVideoFrame(SDL_Texture *texture,
                               const VideoFrame &frame) {
    if (!texture || frame.bgra.empty()) {
        return;
    }
    const auto found = uploadedVideoFrames.find(texture);
    if (found != uploadedVideoFrames.end() &&
        found->second == std::pair{&frame, frame.revision}) {
        return;
    }
    if (SDL_UpdateTexture(texture, nullptr, frame.bgra.data(), frame.width * 4) !=
        0) {
        throw std::runtime_error(std::string("SDL_UpdateTexture failed: ") +
                                 SDL_GetError());
    }
    uploadedVideoFrames[texture] = {&frame, frame.revision};
}

void AppCore::invalidateUploadedVideoFrames() {
    uploadedVideoFrames.clear();
}

void GameApp::renderFrame() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = dialogClockFrozen()
                               ? helpFrozenElapsed
                               : paused
                                     ? pausedAt
                                     : std::chrono::duration<double>(now - start)
                                           .count();
    // Pending narration timers, the equivalent of the original's 0x111 timer
    // expiry: 0x0040bf50 walks its slots and broadcasts, and the scene's
    // handler starts the clip whose tag came back.
    if (viewMode == ViewMode::Scene && !pendingScene) {
        updatePendingNarrations();
    }
    if (viewMode == ViewMode::Scene && !pendingScene) {
        updateSceneAmbience(elapsed);
    }
    if (viewMode == ViewMode::Scene && !pendingScene &&
        activeScene == "morning") {
        // Ahead of the group update below, so a timer that queues birdleave
        // lands on the clip boundary it waited for.
        updateMorning(elapsed);
    }
    if (viewMode == ViewMode::Scene && !pendingScene) {
        // A one-shot player reports AnimFinished once its last frame has
        // been shown, as the sequence stages do.
        for (auto &object : sceneAnimationObjects) {
            if (!object.playing || object.loop || object.completed ||
                object.media.videoFrames.empty()) {
                continue;
            }
            const double duration =
                originalStoryClipCompletion(object.media);
            if (elapsed - object.startedAt < duration) {
                continue;
            }
            object.completed = true;
            object.playing = false;
            if (object.hideOnComplete) {
                object.visible = false;
            }
            referenceTrace.emit("animation_stop", {{"asset", object.name},
                                                    {"reason", "completed"}});
            const std::string name = object.name;
            handleSceneAnimationFinished(name, object.startedAt + duration);
            if (pendingScene || viewMode != ViewMode::Scene) {
                break;
            }
        }
    }
    if (viewMode == ViewMode::Scene && sceneTimedTransition &&
        elapsed >= sceneTimedTransition->first && !pendingScene) {
        const std::string target = sceneTimedTransition->second;
        sceneTimedTransition.reset();
        if (target == "menu") {
            enterMenu();
        } else {
            enterScene(target, target);
        }
    }
    if (activeAmpLevel > 0) {
        const auto found = std::find_if(
            menuEffects.begin(), menuEffects.end(), [&](const MenuEffect &effect) {
                return effect.ampLevel == activeAmpLevel;
            });
        if (found == menuEffects.end() ||
            elapsed - ampEffectStart >= found->media.videoDuration) {
            activeAmpLevel = -1;
        }
    }
    if (fuseActive) {
        const auto found = std::find_if(
            menuEffects.begin(), menuEffects.end(), [](const MenuEffect &effect) {
                return effect.name == "fuse";
            });
        if (found == menuEffects.end() ||
            elapsed - fuseEffectStart >= found->media.videoDuration) {
            fuseActive = false;
        }
    }
    if (viewMode == ViewMode::Scene && !sceneClips.empty() &&
        !sceneSequenceFinished) {
        const auto startCurrentSceneClip = [&]() {
            if (sceneClipStarted || sceneClipIndex >= sceneClips.size() ||
                elapsed < sceneClipTimelineStart) {
                return;
            }
            const auto &startedClip = sceneClips[sceneClipIndex];
            const std::uint32_t acquiredConflictMask =
                startedClip.conflictMask &
                ~sceneSequenceHeldConflictMask;
            const double releaseAt = sceneCharacterConflictReleaseAt(
                acquiredConflictMask, sceneClipTimelineStart);
            if (releaseAt > elapsed) {
                sceneClipTimelineStart = releaseAt;
                return;
            }
            sceneClipTimelineStart = releaseAt;
            sceneClipStarted = true;
            // 0x0040a660 removes the players of the groups it takes over,
            // the held stage among them, then adds this one.
            releaseSceneHeldStage();
            acquireSceneCharacterConflicts(
                acquiredConflictMask, elapsed);
            sceneSequencePendingConflictMask &= ~acquiredConflictMask;
            sceneSequenceHeldConflictMask = startedClip.conflictMask;
            referenceTrace.emit("animation_start", {{"asset", startedClip.name},
                                                     {"x", startedClip.x},
                                                     {"y", startedClip.y},
                                                     {"layer", startedClip.layer},
                                                     {"loop", startedClip.loopVideo}});
            handleSceneAnimationStarted(startedClip.name, releaseAt);
            if (sceneUsesAudioTimeline && !sceneClipStartOffsets.empty()) {
                if (sceneClipIndex == 0) {
                    sceneAudioTimelineStartedAt = releaseAt;
                }
                const double scheduledAudioAt =
                    sceneAudioTimelineStartedAt +
                    sceneClipStartOffsets[sceneClipIndex] -
                    sceneClipStartOffsets.front();
                referenceTrace.emit(
                    "diagnostic",
                    {{"kind", "scene_audio_alignment"},
                     {"asset", startedClip.name},
                     {"scheduled_audio_ms", static_cast<int>(std::llround(
                          scheduledAudioAt * 1000.0))},
                     {"actual_video_ms", static_cast<int>(std::llround(
                          releaseAt * 1000.0))},
                     {"delta_ms", static_cast<int>(std::llround(
                          (releaseAt - scheduledAudioAt) * 1000.0))},
                     {"resynchronized", sceneClipIndex == 0 ||
                          acquiredConflictMask != 0U}});
            }
            if (sceneClipIndex == 0 && sceneUsesAudioTimeline) {
                audioMixer.setForeground(
                    sceneAudioTimeline,
                    audioVolumes[1] * sceneAudioGains[1],
                    &sceneEffectTimeline,
                    audioVolumes[0] * sceneAudioGains[0]);
                referenceTrace.emit("audio_start", {{"asset", startedClip.name},
                                                     {"role", "speech"}});
            } else if (!sceneUsesAudioTimeline) {
                audioMixer.setForeground(startedClip.media,
                                         clipAudioVolume(startedClip.media));
            }
            if (sceneUsesAudioTimeline) {
                if (sceneClipIndex > 0 && acquiredConflictMask != 0U) {
                    audioMixer.seekForeground(
                        sceneClipStartOffsets[sceneClipIndex] -
                        sceneClipStartOffsets.front());
                } else {
                    audioMixer.clearForegroundHold();
                }
                if (sceneClipIndex + 1 < sceneClips.size()) {
                    const std::uint32_t nextAcquiredConflictMask =
                        sceneClips[sceneClipIndex + 1].conflictMask &
                        ~startedClip.conflictMask;
                    if (nextAcquiredConflictMask != 0U) {
                        audioMixer.holdForegroundAt(
                            sceneClipStartOffsets[sceneClipIndex + 1] -
                            sceneClipStartOffsets.front());
                    }
                }
            }
            SDL_PauseAudioDevice(audioDevice,
                                 musicEnabled && !paused ? 0 : 1);
            const std::string title =
                "FGP Native — " + activeScene + " — " +
                startedClip.name +
                " — native animation + audio · exit control back";
            SDL_SetWindowTitle(window, title.c_str());
        };
        startCurrentSceneClip();
        while (sceneClipStarted && sceneClipIndex + 1 < sceneClips.size() &&
               elapsed - sceneClipTimelineStart >=
                   sceneClips[sceneClipIndex].duration) {
            const double completedAt =
                sceneClipTimelineStart +
                sceneClips[sceneClipIndex].duration;
            referenceTrace.emit("animation_stop", {{"asset", sceneClips[sceneClipIndex].name},
                                                    {"reason", "completed"}});
            handleSceneAnimationFinished(
                sceneClips[sceneClipIndex].name, completedAt);
            const std::size_t finishedIndex = sceneClipIndex;
            ++sceneClipIndex;
            // 0x0040aad7 removes a finished stage only when its conflict
            // mask is 0. One that holds bits stays on its last frame until
            // a group with one of those bits replaces it through 0x0040a660:
            // the next stage once it can start, or at once a group whose
            // bit the next stage does not take (0x0040abc0 resumes it).
            const std::uint32_t finishedMask =
                sceneClips[finishedIndex].conflictMask;
            const std::uint32_t releasedMask =
                finishedMask & ~sceneClips[sceneClipIndex].conflictMask;
            const bool groupResumes = std::any_of(
                sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
                [&](const SceneCharacterGroup &group) {
                    return group.enabled && !group.baseClips.empty() &&
                           (group.conflictMask & releasedMask) != 0U;
                });
            if (finishedMask != 0U) {
                sceneHeldStageIndex = finishedIndex;
                if (groupResumes) {
                    releaseSceneHeldStage();
                }
            }
            sceneSequenceHeldConflictMask &=
                sceneClips[sceneClipIndex].conflictMask;
            sceneSequencePendingConflictMask =
                sceneClips[sceneClipIndex].conflictMask &
                ~sceneSequenceHeldConflictMask;
            sceneClipTimelineStart = completedAt;
            sceneClipStarted = false;
            startCurrentSceneClip();
        }
        if (activeScene == "intro" &&
            sceneClipStarted &&
            sceneClipIndex + 1 == sceneClips.size() &&
            elapsed - sceneClipTimelineStart >=
                sceneClips[sceneClipIndex].duration &&
            !pendingScene) {
            // 0x00416692: the completed S00AN-SS-001c enters the Yard.
            referenceTrace.emit("animation_stop",
                                {{"asset", sceneClips[sceneClipIndex].name},
                                 {"reason", "completed"}});
            enterScene("yard", "Yard");
        }
        if (activeScene == "logo" &&
            sceneClipStarted &&
            sceneClipIndex + 1 == sceneClips.size() &&
            elapsed - sceneClipTimelineStart >=
                sceneClips[sceneClipIndex].duration) {
            referenceTrace.emit(
                "animation_stop",
                {{"asset", sceneClips[sceneClipIndex].name},
                 {"reason", "completed"}});
            sceneSequenceBlocksInput = false;
            sceneSequenceFinished = true;
            sceneClipStarted = false;
            sceneSequenceHeldConflictMask = 0;
            sceneSequencePendingConflictMask = 0;
            enterScene("birthday", "birthday");
        }
        if (activeScene != "intro" && sceneSequenceBlocksInput &&
            sceneClipStarted &&
            sceneClipIndex + 1 == sceneClips.size() &&
            elapsed - sceneClipTimelineStart >=
                sceneClips[sceneClipIndex].duration) {
            referenceTrace.emit("animation_stop", {{"asset", sceneClips[sceneClipIndex].name},
                                                    {"reason", "completed"}});
            // 0x0040aad7-0x0040aaec remove a finished stage only when its
            // conflict mask is 0. One that holds a bit keeps its last frame
            // until a group with that bit takes over, or the scene ends
            // (S07AN-SOL-001 in the morning captures).
            const std::string finishedName = sceneClips[sceneClipIndex].name;
            const bool holdFinalStage =
                sceneClips[sceneClipIndex].conflictMask != 0U;
            if (!holdFinalStage) {
                referenceTrace.emit("animation_remove", {
                    {"asset", finishedName},
                    {"reason", "removed"},
                });
            }
            const double completedAt =
                sceneClipTimelineStart +
                sceneClips[sceneClipIndex].duration;
            handleSceneAnimationFinished(finishedName, completedAt);
            sceneFinalStageHeld = holdFinalStage && !sceneClips.empty() &&
                                  sceneClips.back().name == finishedName;
            sceneSequenceBlocksInput = false;
            sceneSequenceFinished = true;
            sceneClipStarted = false;
            sceneSequenceHeldConflictMask = 0;
            sceneSequencePendingConflictMask = 0;
            const bool deskToPee =
                activeScene == "desk" &&
                sceneClips.back().name == "S02AN-SS-007a";
            const bool peeToDesk =
                activeScene == "pee" &&
                sceneClips.back().name == "S03AN-SS-002";
            if (deskToPee) {
                bool profileSaved = true;
                if (profiles[profileIndex] != "No profile") {
                    const fs::path profilePath =
                        profileDirectory /
                        (profiles[profileIndex] + ".pfl");
                    try {
                        incrementProfileSceneRecordU32(
                            profilePath, "desk", 0x402U, 1U);
                    } catch (const std::exception &error) {
                        profileSaved = false;
                        showMessage(std::string(
                                        "Kunne ikke lagre fremdriften: ") +
                                    error.what());
                    }
                }
                if (profileSaved) {
                    enterScene("pee", "pee");
                }
            } else if (peeToDesk) {
                // 0x00426be2: the pee callback matches this exact stage and
                // calls GotoScene("desk", 2). Desk's mode-2 arm at
                // 0x0042486e seeds groups 1 and 4 and plays only
                // S02AN-SS-007b, instead of replaying the whole opening.
                enterScene("desk", "desk", 2);
            } else {
                // The desk's 0x11f handler at 0x00424d30 compares the finished
                // table against 0x004b2770 (the SS-001a..001e opening) and, on
                // a first visit, arms S02AN-NAR-001 ten seconds later
                // (0x00424d6a: now + 0x2710). Only entry mode 0 runs that
                // table, so this cannot fire on a return from `pee`.
                if (activeScene == "desk" && activeSceneEntryMode == 0 &&
                    activeSceneVisitCount <= 1U) {
                    armSceneNarration("S02AN-NAR-001", 10.0, true);
                }
                if (activeScene == "pee" &&
                    sceneClips.back().name == "S03AN-SS-009") {
                    peeMagnetActive = false;
                    setSceneCharacterGroupEnabled(21U, false);
                }
                seedOpeningCharacterGroups();
                if (activeScene == "morning") {
                    // 0x00425f90: the finished squirrel/SOL-001 table sets
                    // cursor mode 0 again, so the mouse stays swallowed
                    // until the RIDEDOWN question.
                    sceneSequenceBlocksInput = true;
                } else {
                    hoveredHotspot = hotspotAt(mouseX, mouseY);
                    hoveredBounds =
                        currentHotspotBounds(hoveredHotspot);
                    const std::string title =
                        "FGP Native — " + activeScene +
                        " — original sequence complete · exit control back";
                    SDL_SetWindowTitle(window, title.c_str());
                }
            }
        }
    }
    if (viewMode == ViewMode::Scene && audiopairsGame) {
        updateAudiopairs(elapsed);
    }
    if (viewMode == ViewMode::Scene && sockdrawerGame) {
        updateSockdrawer(elapsed);
    }
    if (viewMode == ViewMode::Scene && butterflyGame) {
        updateButterfly(elapsed);
    }
    if (viewMode == ViewMode::Scene && lettersortGame) {
        updateLettersort(elapsed);
    }
    if (viewMode == ViewMode::Scene && mahjongGame) {
        updateMahjong(elapsed);
    }
    if (viewMode == ViewMode::Scene && whackamoleGame) {
        updateWhackamole(elapsed);
    }
    if (viewMode == ViewMode::Scene && balloonhuntGame) {
        updateBalloonhunt(elapsed);
    }
    if (viewMode == ViewMode::Scene && pipelineGame) {
        updatePipeline(elapsed);
    }
    if (viewMode == ViewMode::Scene && hopscotchGame) {
        updateHopscotch(elapsed);
    }
    if (viewMode == ViewMode::Scene && beemazeGame) {
        updateBeemaze(elapsed);
    }
    if (viewMode == ViewMode::Scene && bugzzzGame) {
        updateBugzzz(elapsed);
    }
    if (viewMode == ViewMode::Scene && wheelbarrowGame) {
        updateWheelbarrow(elapsed);
    }
    if (viewMode == ViewMode::Scene && goodbyeState) {
        updateGoodbye(elapsed);
    }
    if (viewMode == ViewMode::Scene && buildacarWorkshop) {
        updateBuildacar(elapsed);
    }
    if (viewMode == ViewMode::Scene && outroState) {
        updateOutro(elapsed);
    }
    if (viewMode == ViewMode::Scene && buildabikeGame) {
        updateBuildabike(elapsed);
    }
    if (viewMode == ViewMode::Scene && mountainGame) {
        updateMountain(elapsed);
    }
    if (viewMode == ViewMode::Scene && hustleGame) {
        updateHustle(elapsed);
    }
    if (viewMode == ViewMode::Scene && chessGame) {
        updateChess(elapsed);
    }
    if (viewMode == ViewMode::Scene && jigsawGame) {
        updateJigsaw(elapsed);
    }
    if (viewMode == ViewMode::Scene && slidingGame) {
        updateSliding(elapsed);
    }
    if (viewMode == ViewMode::Scene && birthdayGame) {
        updateBirthday(elapsed);
    }
    if (viewMode == ViewMode::Scene && bouquetGame) {
        updateBouquet(elapsed);
    }
    if (viewMode == ViewMode::Scene && fenceGame) {
        updateFence(elapsed);
    }
    if (viewMode == ViewMode::Scene) {
        updateViewerScenes(elapsed);
    }
    if (viewMode == ViewMode::Scene && puzzleGame) {
        updatePuzzle(elapsed);
    }
    if (viewMode == ViewMode::Scene && draughtsGame) {
        updateDraughts(elapsed);
    }
    if (viewMode == ViewMode::Scene && activitypageGame) {
        updateActivitypage(elapsed);
    }
    if (viewMode == ViewMode::Scene && racemenuGame) {
        updateRacemenu(elapsed);
    }
    if (viewMode == ViewMode::Scene && racingGame) {
        updateRacing(elapsed);
    }
    if (viewMode == ViewMode::Scene && buttercolAlbum) {
        updateButtercol(elapsed);
    }
    if (viewMode == ViewMode::Scene && creditsGame) {
        updateCredits(elapsed);
    }
    if (viewMode == ViewMode::Scene && textinvaderGame) {
        updateTextinvader(elapsed);
    }
    if (viewMode == ViewMode::Scene && synonymGame) {
        updateSynonym(elapsed);
    }
    if (viewMode == ViewMode::Scene && mathlabGame) {
        updateMathlab(elapsed);
    }
    if (viewMode == ViewMode::Scene && triviaGame) {
        updateTrivia(elapsed);
    }
    if (viewMode == ViewMode::Scene && colorfillGame) {
        updateColorfill(elapsed);
    }

    // FGP waits for idle to finish after a 5-second initial deadline.
    // After a specific animation, it waits for idle to finish after a
    // 30-second deadline. This preserves animation-complete keyframes.
    const double idleDuration = menuClips.empty()
                                    ? 0.0
                                    : menuClips.back().media.videoDuration;
    double characterTime = elapsed;
    bool showSpecific = false;
    if (idleDuration > 0.0 && animation.videoDuration > 0.0) {
        const double firstIdleRun =
            std::ceil(5.0 / idleDuration) * idleDuration;
        if (elapsed >= firstIdleRun) {
            double afterFirstIdle = elapsed - firstIdleRun;
            if (afterFirstIdle < animation.videoDuration) {
                showSpecific = true;
                characterTime = afterFirstIdle;
            } else {
                afterFirstIdle -= animation.videoDuration;
                const double idleRun =
                    std::ceil(30.0 / idleDuration) * idleDuration;
                const double cycleDuration = idleRun + animation.videoDuration;
                const double cycleTime =
                    std::fmod(afterFirstIdle, cycleDuration);
                showSpecific = cycleTime >= idleRun;
                characterTime = showSpecific
                                    ? cycleTime - idleRun
                                    : cycleTime;
            }
        }
    }
    currentFrame = frameIndexAt(animation, characterTime, false);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_Texture *activeBackdropTexture =
        viewMode == ViewMode::Scene ? sceneBackdropTexture.get()
                                    : backdropTexture.get();
    SDL_RenderCopy(renderer, activeBackdropTexture, nullptr, nullptr);

    if (viewMode == ViewMode::Scene && audiopairsGame) {
        renderAudiopairs(elapsed);
    }
    if (viewMode == ViewMode::Scene && sockdrawerGame) {
        renderSockdrawer(elapsed);
    }
    if (viewMode == ViewMode::Scene && butterflyGame) {
        renderButterfly(elapsed);
    }
    if (viewMode == ViewMode::Scene && lettersortGame) {
        renderLettersort(elapsed);
    }
    if (viewMode == ViewMode::Scene && mahjongGame) {
        renderMahjong(elapsed);
    }
    if (viewMode == ViewMode::Scene && whackamoleGame) {
        renderWhackamole(elapsed);
    }
    if (viewMode == ViewMode::Scene && balloonhuntGame) {
        renderBalloonhunt(elapsed);
    }
    if (viewMode == ViewMode::Scene && pipelineGame) {
        renderPipeline(elapsed);
    }
    if (viewMode == ViewMode::Scene && hopscotchGame) {
        renderHopscotch(elapsed);
    }
    if (viewMode == ViewMode::Scene && beemazeGame) {
        renderBeemaze(elapsed);
    }
    if (viewMode == ViewMode::Scene && bugzzzGame) {
        renderBugzzz(elapsed);
    }
    if (viewMode == ViewMode::Scene && wheelbarrowGame) {
        renderWheelbarrow(elapsed);
    }
    if (viewMode == ViewMode::Scene && goodbyeState) {
        renderGoodbye(elapsed);
    }
    if (viewMode == ViewMode::Scene && outroState) {
        renderOutro(elapsed);
    }
    if (viewMode == ViewMode::Scene && buildabikeGame) {
        renderBuildabike(elapsed);
    }
    if (viewMode == ViewMode::Scene && mountainGame) {
        renderMountain(elapsed);
    }
    if (viewMode == ViewMode::Scene && hustleGame) {
        renderHustle(elapsed);
    }
    if (viewMode == ViewMode::Scene && chessGame) {
        renderChess(elapsed);
    }
    if (viewMode == ViewMode::Scene && jigsawGame) {
        renderJigsaw(elapsed);
    }
    if (viewMode == ViewMode::Scene && slidingGame) {
        renderSliding(elapsed);
    }
    if (viewMode == ViewMode::Scene && fenceGame) {
        renderFence(elapsed);
    }
    if (viewMode == ViewMode::Scene) {
        renderViewerScenes(elapsed);
    }
    if (viewMode == ViewMode::Scene && puzzleGame) {
        renderPuzzle(elapsed);
    }
    if (viewMode == ViewMode::Scene && draughtsGame) {
        renderDraughts(elapsed);
    }
    if (viewMode == ViewMode::Scene && activitypageGame) {
        renderActivitypage(elapsed);
    }
    if (viewMode == ViewMode::Scene && racemenuGame) {
        renderRacemenu();
    }
    if (viewMode == ViewMode::Scene && racingGame) {
        renderRacing();
    }
    if (viewMode == ViewMode::Scene && buttercolAlbum) {
        renderButtercol();
    }
    if (viewMode == ViewMode::Scene && creditsGame) {
        renderCredits(elapsed);
    }
    if (viewMode == ViewMode::Scene && textinvaderGame) {
        renderTextinvader(elapsed);
    }
    if (viewMode == ViewMode::Scene && synonymGame) {
        renderSynonym(elapsed);
    }
    if (viewMode == ViewMode::Scene && mathlabGame) {
        renderMathlab(elapsed);
    }
    if (viewMode == ViewMode::Scene && triviaGame) {
        renderTrivia(elapsed);
    }
    if (viewMode == ViewMode::Scene && colorfillGame) {
        renderColorfill(elapsed);
    }
    if (viewMode == ViewMode::Scene && recoveredMinigameRuntime) {
        renderRecoveredMinigame(elapsed);
    }

    if (viewMode == ViewMode::Scene && activeScene == "buildacar" &&
        buildacarWorkshop) {
        renderBuildacarWorkshop(elapsed);
    }

    if (viewMode == ViewMode::Scene && activeScene == "sceneindex") {
        if (sceneIndexTab == 3U && sceneIndexGoldFrameTexture) {
            SDL_Rect frame{42, 116, 712, 386};
            SDL_RenderCopy(renderer, sceneIndexGoldFrameTexture.get(),
                           nullptr, &frame);
        }
        for (std::size_t index = 0;
             index < sceneIndexTiles.size() &&
             index < sceneIndexTileBounds.size();
             ++index) {
            SDL_RenderCopy(renderer, sceneIndexTiles[index].get(), nullptr,
                           &sceneIndexTileBounds[index]);
        }
        for (std::size_t index = 0;
             index < sceneIndexMedals.size() &&
             index < sceneIndexMedalBounds.size();
             ++index) {
            if (sceneIndexMedals[index]) {
                SDL_RenderCopy(renderer, sceneIndexMedals[index].get(),
                               nullptr, &sceneIndexMedalBounds[index]);
            }
        }
        constexpr std::array<SDL_Rect, 4> tabBounds{{
            {45, 510, 169, 50}, {223, 510, 169, 50},
            {402, 510, 169, 50}, {581, 510, 169, 50}}};
        for (std::size_t index = 0; index < tabBounds.size(); ++index) {
            SDL_Texture *texture =
                sceneIndexTabTextures[index == sceneIndexTab ? 1U : 0U]
                    .get();
            if (texture) {
                SDL_RenderCopy(renderer, texture, nullptr,
                               &tabBounds[index]);
            }
        }
    }

    if (townReelShowing() && modalMeshTexture) {
        // Layer 5 mesh under the layer 7 frame (0x00427097).
        SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr, nullptr);
    }
    // Players draw in ascending layer order (comparator 0x0047e220): a
    // bitmap below the collectibles' layer -11 goes first, the rest after
    // them (Outtent's bike and paper at -20, its binocular view at -10).
    const auto drawSceneBitmaps = [&](bool belowCollectibles) {
        if (viewMode != ViewMode::Scene || activeScene == "yard") {
            return;
        }
        for (auto &bitmap : sceneBitmapLayers) {
            if (!bitmap.texture || !bitmap.visible ||
                (bitmap.layer < -11) != belowCollectibles) {
                continue;
            }
            SDL_Rect destination{bitmap.x, bitmap.y,
                                 bitmap.width, bitmap.height};
            SDL_RenderCopy(renderer, bitmap.texture.get(), nullptr,
                           &destination);
        }
    };
    drawSceneBitmaps(true);

    auto drawLocalizedCentered = [&](const BitmapFont &font,
                                     std::string_view section,
                                     std::string_view key,
                                     const SDL_Rect &bounds,
                                     std::string fallback = {}) {
        drawCenteredText(renderer, font,
                         language.get(section, key, std::move(fallback)),
                         bounds);
    };
    if (viewMode == ViewMode::Scene) {
        if (activeScene == "sceneindex") {
            drawLocalizedCentered(
                menuTitleFont, "sceneindex", "SCENENAME",
                SDL_Rect{210, 62, 378, 43}, "Innholdsoversikt");
            constexpr std::array<SDL_Rect, 4> tabBounds{{
                {45, 510, 169, 50}, {223, 510, 169, 50},
                {402, 510, 169, 50}, {581, 510, 169, 50}}};
            constexpr std::array<std::string_view, 4> tabKeys{
                "STORYPAGE", "SUBGAME", "ACTIVITY", "GOLD"};
            constexpr std::array<std::string_view, 4> tabFallbacks{
                "Scener", "Spill", "Aktiviteter", "Gullutgave"};
            for (std::size_t index = 0; index < tabBounds.size(); ++index) {
                drawLocalizedCentered(
                    calendarFont, "sceneindex", tabKeys[index],
                    tabBounds[index], std::string(tabFallbacks[index]));
            }
        } else if ((activeScene == "tournament" ||
                    activeScene == "tourneyscore") && tournamentScreen) {
            renderTournament();
        } else if (activeScene == "mainscore" && mainscoreBoard) {
            renderMainscore();
        }
    }

    if (viewMode == ViewMode::Scene && activeScene == "intro" &&
        sceneTitleTexture && sceneClipIndex < 2) {
        // The executable places the intro title at (105,194), layer 0.
        // SS-001a/b use layer 1 and therefore draw on top of it.
        SDL_Rect destination{105, 194, sceneTitleWidth, sceneTitleHeight};
        SDL_RenderCopy(renderer, sceneTitleTexture.get(), nullptr, &destination);
    }

    auto drawYardBitmapLayers = [&](bool aboveAnimations) {
        if (viewMode != ViewMode::Scene || activeScene != "yard") {
            return;
        }
        const std::uint32_t constructionMode =
            yardConstructionFlags & 0x30U;
        for (auto &bitmap : sceneBitmapLayers) {
            if ((bitmap.layer > 0) != aboveAnimations) {
                continue;
            }
            bool visible = bitmap.name == "bush";
            if (bitmap.name == "button_0") {
                visible = constructionMode == 0 ||
                          constructionMode == 0x10U;
            } else if (bitmap.name == "button_1") {
                visible = constructionMode == 0x20U;
            } else if (bitmap.name == "weight_0") {
                visible = (yardConstructionFlags & 0x02U) == 0;
            } else if (bitmap.name == "weight_1") {
                visible = (yardConstructionFlags & 0x02U) != 0;
            } else if (bitmap.name == "mailbox_1") {
                visible = bitmap.visible;
            }
            if (!visible || !bitmap.texture) {
                continue;
            }
            SDL_Rect destination{bitmap.x, bitmap.y,
                                 bitmap.width, bitmap.height};
            SDL_RenderCopy(renderer, bitmap.texture.get(), nullptr,
                           &destination);
        }
    };

    if (viewMode == ViewMode::Scene) {
        for (auto &collectible : sceneCollectibles) {
            if (!collectible.visible || !collectible.texture ||
                !collectible.surface) {
                continue;
            }
            SDL_Rect destination{collectible.x, collectible.y,
                                 collectible.surface->w,
                                 collectible.surface->h};
            SDL_RenderCopy(renderer, collectible.texture.get(), nullptr,
                           &destination);
        }
        drawSceneBitmaps(false);
        for (auto &object : sceneAnimationObjects) {
            if (!object.visible || !object.texture ||
                object.media.videoFrames.empty()) {
                continue;
            }
            const double localTime = elapsed - object.startedAt;
            if (object.playing && object.hideOnComplete &&
                localTime >= object.media.videoDuration) {
                continue;
            }
            const std::size_t frameIndex =
                object.completed
                    ? object.media.videoFrames.size() - 1U
                : object.playing
                    ? frameIndexAt(object.media, localTime, object.loop)
                    : 0U;
            const auto &frame = object.media.videoFrames[frameIndex];
            uploadVideoFrame(object.texture.get(), frame);
            SDL_Rect destination{object.x, object.y, frame.width,
                                 frame.height};
            SDL_RenderCopy(renderer, object.texture.get(), nullptr,
                           &destination);
        }
    }

    if (viewMode == ViewMode::Scene && birthdayGame) {
        renderBirthday(elapsed);
    }

    // Bouquet's flowers stand above the layer -10 car part.
    if (viewMode == ViewMode::Scene && bouquetGame) {
        renderBouquet(elapsed);
    }

    if (viewMode == ViewMode::Scene) {
        updateSceneCharacterGroups(elapsed);
    }

    drawYardBitmapLayers(false);

    const bool drawHeldStage =
        viewMode == ViewMode::Scene && !sceneSequenceFinished &&
        !sceneClipStarted && sceneHeldStageIndex.has_value() &&
        *sceneHeldStageIndex < sceneClips.size();
    const bool drawSequenceStage =
        viewMode == ViewMode::Scene && !sceneClips.empty() &&
        ((!sceneSequenceFinished && sceneClipStarted) ||
         (sceneSequenceFinished && sceneFinalStageHeld) || drawHeldStage);
    std::string drawnClips;
    if (drawSequenceStage) {
        auto &clip = drawHeldStage ? sceneClips[*sceneHeldStageIndex]
                     : sceneSequenceFinished ? sceneClips.back()
                                             : sceneClips[sceneClipIndex];
        drawnClips += clip.name;
        if (clip.visible && clip.texture &&
            !clip.media.videoFrames.empty()) {
            const double localTime = elapsed - sceneClipTimelineStart;
            const std::size_t frameIndex =
                sceneSequenceFinished || drawHeldStage
                    ? clip.media.videoFrames.size() - 1U
                    : frameIndexAt(clip.media, localTime, clip.loopVideo);
            const auto &frame = clip.media.videoFrames[frameIndex];
            uploadVideoFrame(clip.texture.get(), frame);
            if (clip.media.smackerInterlaced) {
                SDL_Rect area{clip.x, clip.y, frame.width, frame.height * 2};
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(renderer, &area);
                for (int row = 0; row < frame.height; ++row) {
                    const SDL_Rect source{0, row, frame.width, 1};
                    const SDL_Rect line{clip.x, clip.y + row * 2,
                                        frame.width, 1};
                    SDL_RenderCopy(renderer, clip.texture.get(), &source,
                                   &line);
                }
            } else {
                SDL_Rect destination{clip.x, clip.y, frame.width,
                                     frame.height};
                SDL_RenderCopy(renderer, clip.texture.get(), nullptr,
                               &destination);
            }
        }
    }

    if (viewMode == ViewMode::Scene) {
        for (auto &group : sceneCharacterGroups) {
            if (!sceneCharacterVisible(group)) {
                const auto *held = group.heldClip;
                if (held == nullptr || !group.enabled || !held->visible ||
                    !held->texture || held->media.videoFrames.empty()) {
                    continue;
                }
                const auto &frame = held->media.videoFrames[
                    held->media.videoFrames.size() - 1U];
                uploadVideoFrame(held->texture.get(), frame);
                SDL_Rect destination{
                    held->x, held->y, frame.width, frame.height};
                SDL_RenderCopy(renderer, held->texture.get(), nullptr,
                               &destination);
                drawnClips += (drawnClips.empty() ? "" : ",") + held->name;
                continue;
            }
            const auto [clip, frameIndex] =
                sceneCharacterPoseAt(group, elapsed);
            if (clip == nullptr) {
                continue;
            }
            if (!clip->visible || !clip->texture ||
                frameIndex >= clip->media.videoFrames.size()) {
                continue;
            }
            const auto &frame = clip->media.videoFrames[frameIndex];
            uploadVideoFrame(clip->texture.get(), frame);
            SDL_Rect destination{
                clip->x, clip->y, frame.width, frame.height};
            SDL_RenderCopy(renderer, clip->texture.get(), nullptr,
                           &destination);
            drawnClips += (drawnClips.empty() ? "" : ",") + clip->name;
        }
    }
    if (viewMode == ViewMode::Scene && drawnClips != sceneDrawnClips) {
        sceneDrawnClips = drawnClips;
        referenceTrace.emit("diagnostic", {{"kind", "scene_drawn_clips"},
                                           {"clips", drawnClips}});
    }

    drawYardBitmapLayers(true);

    if (viewMode == ViewMode::Scene && outroState) {
        renderOutroCredits(elapsed);
    }

    if (viewMode != ViewMode::Scene) {
        for (auto &clip : menuClips) {
            if (!clip.texture || (clip.idle && showSpecific) ||
                (!clip.idle && clip.media.videoFrames.empty())) {
                continue;
            }
            const double clipTime = clip.idle ? characterTime : elapsed;
            const std::size_t clipFrameIndex =
                frameIndexAt(clip.media, clipTime);
            const auto &clipFrame = clip.media.videoFrames[clipFrameIndex];
            uploadVideoFrame(clip.texture.get(), clipFrame);
            SDL_Rect destination{clip.x, clip.y, clipFrame.width, clipFrame.height};
            SDL_RenderCopy(renderer, clip.texture.get(), nullptr, &destination);
        }
    }

    if (viewMode != ViewMode::Scene && showSpecific && animationTexture &&
        currentFrame < animation.videoFrames.size()) {
        const auto &frame = animation.videoFrames[currentFrame];
        uploadVideoFrame(animationTexture.get(), frame);
        SDL_Rect destination{
            434,
            233,
            animationWidth,
            animationHeight};
        SDL_RenderCopy(renderer, animationTexture.get(), nullptr, &destination);
    }
    if (viewMode == ViewMode::Menu) {
        for (auto &effect : menuEffects) {
            const bool active = effect.ampLevel > 0
                                    ? effect.ampLevel == activeAmpLevel
                                    : effect.name == "fuse" && fuseActive;
            if (!active || !effect.texture || effect.media.videoFrames.empty()) {
                continue;
            }
            const double effectStart =
                effect.ampLevel > 0 ? ampEffectStart : fuseEffectStart;
            const auto frameIndex =
                frameIndexAt(effect.media, elapsed - effectStart, false);
            const auto &frame = effect.media.videoFrames[frameIndex];
            uploadVideoFrame(effect.texture.get(), frame);
            SDL_Rect destination{effect.x, effect.y, frame.width, frame.height};
            SDL_RenderCopy(renderer, effect.texture.get(), nullptr, &destination);
        }
    }
    if (viewMode != ViewMode::Scene) {
        // FGP.exe inserts this bitmap at layer 10; all continuously
        // running menu animations use layer 0.
        SDL_Rect titleDestination{298, 77, title->w, title->h};
        SDL_RenderCopy(renderer, titleTexture.get(), nullptr,
                       &titleDestination);
    }
    if (viewMode == ViewMode::Menu && profiles[profileIndex] != "No profile") {
        // 0x00417700 creates the selected-profile text object at
        // (288,512), size 224x58, with Amerigo BT_18_. 0x00417810 replaces
        // its text whenever the previous/next profile controls are used.
        if (const auto cp1252Name =
                utf8ToCp1252(profiles[profileIndex])) {
            drawCenteredText(renderer, menuTitleFont, *cp1252Name,
                             SDL_Rect{288, 512, 224, 58});
        }
    }
    if (showHotspots) {
        SDL_Texture *activeHotspotTexture =
            viewMode == ViewMode::Scene ? sceneHotspotTexture.get()
                                        : hotspotTexture.get();
        if (activeHotspotTexture) {
            SDL_RenderCopy(renderer, activeHotspotTexture, nullptr, nullptr);
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (showHotspots && hoveredBounds && viewMode != ViewMode::Options) {
        SDL_Rect bounds = *hoveredBounds;
        SDL_SetRenderDrawColor(renderer, 255, 220, 50, 55);
        SDL_RenderFillRect(renderer, &bounds);
        SDL_SetRenderDrawColor(renderer, 255, 235, 90, 230);
        SDL_RenderDrawRect(renderer, &bounds);
    }

    if (viewMode == ViewMode::Menu) {
        for (std::size_t index = 0; index < menuCornerButtons.size(); ++index) {
            auto &button = menuCornerButtons[index];
            SDL_Texture *texture = pressedMenuCornerButton == index
                                       ? button.pressedTexture.get()
                                       : button.normalTexture.get();
            if (texture) {
                SDL_RenderCopy(renderer, texture, nullptr, &button.bounds);
            }
        }
    } else if (viewMode == ViewMode::Scene) {
        for (std::size_t index = 0; index < sceneCornerButtons.size();
             ++index) {
            auto &button = sceneCornerButtons[index];
            SDL_Texture *texture = pressedSceneCornerButton == index
                                       ? button.pressedTexture.get()
                                       : button.normalTexture.get();
            if (texture) {
                SDL_RenderCopy(renderer, texture, nullptr, &button.bounds);
            }
        }
    }

    // The morph particle uses layer 100, above every other scene element but
    // still belongs to the scene beneath a modal popup.
    if (viewMode == ViewMode::Scene && peeParticleField) {
        renderPeeParticleField();
    }

    // The Tournament's list dialog sits above the scene and its corner
    // buttons, like every 0x00407290 dialog (backdrop layer 100, mesh 99).
    if (viewMode == ViewMode::Scene && tournamentScreen) {
        renderTournamentPicker();
    }

    const bool modalMeshVisible =
        viewMode == ViewMode::Options ||
        dialogMode != DialogMode::None || storyNavigationOpen ||
        colorfillChooserOpen();
    // In the race the mesh goes on the 640x480 frame with the dialog, below.
    const bool raceDialog = dialogMode != DialogMode::None &&
                            viewMode == ViewMode::Scene && activeScene == "racing" &&
                            racingGame && racingGame->target;
    if (modalMeshVisible && modalMeshTexture && !raceDialog) {
        SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr, nullptr);
    }
    if (viewMode == ViewMode::Scene && colorfillGame) {
        renderColorfillOverlay();
    }

    if (viewMode == ViewMode::Options) {
        const SDL_Point origin = optionsDialogPosition();
        SDL_Rect panel{origin.x, origin.y,
                       settingsBackdrop->w, settingsBackdrop->h};
        SDL_RenderCopy(renderer, settingsBackdropTexture.get(), nullptr, &panel);

        drawCenteredText(
            renderer, calendarFont,
            language.get("sounddlg", "TITLE", "Innstillinger"),
            SDL_Rect{origin.x + 13, origin.y + 8, 351, 30});

        static constexpr std::array<const char *, 3> labelKeys{
            "EFFECT", "SPEECH", "MUSIC"};
        static constexpr std::array<const char *, 3> labelFallbacks{
            "Effekt", "Tale", "Musikk"};
        static constexpr std::array<int, 3> rowY{53, 91, 130};
        static constexpr std::array<int, 3> labelTop{52, 90, 129};
        static constexpr std::array<OptionsControl, 3> downControls{
            OptionsControl::EffectDown,
            OptionsControl::SpeechDown,
            OptionsControl::MusicDown};
        static constexpr std::array<OptionsControl, 3> upControls{
            OptionsControl::EffectUp,
            OptionsControl::SpeechUp,
            OptionsControl::MusicUp};

        auto drawStateImage = [&](TexturePtr &normalTexture,
                                  TexturePtr &pressedTexture,
                                  SDL_Surface *surface,
                                  int x,
                                  int y,
                                  OptionsControl control) {
            SDL_Rect destination{origin.x + x, origin.y + y,
                                 surface->w, surface->h};
            SDL_RenderCopy(renderer, normalTexture.get(), nullptr,
                           &destination);
            if (optionsPressedControl == control) {
                SDL_RenderCopy(renderer, pressedTexture.get(), nullptr,
                               &destination);
            }
        };

        for (int channel = 0; channel < 3; ++channel) {
            drawCenteredText(
                renderer, dialogFont,
                language.get("sounddlg", labelKeys[channel],
                             labelFallbacks[channel]),
                SDL_Rect{origin.x + 11, origin.y + labelTop[channel],
                         69, channel == 0 ? 23 : 22});
            drawStateImage(settingsDownNormalTexture,
                           settingsDownPressedTexture,
                           settingsDownNormal.get(), 91, rowY[channel],
                           downControls[channel]);
            drawStateImage(settingsUpNormalTexture,
                           settingsUpPressedTexture,
                           settingsUpNormal.get(), 300, rowY[channel],
                           upControls[channel]);

            const int travel = 163 - settingsThumb->w;
            const int thumbX = 123 + static_cast<int>(std::lround(
                                         audioVolumes[channel] * travel));
            SDL_Rect thumb{origin.x + thumbX, origin.y + rowY[channel],
                           settingsThumb->w, settingsThumb->h};
            SDL_RenderCopy(renderer, settingsThumbTexture.get(), nullptr, &thumb);

            const std::string value = std::to_string(static_cast<int>(
                std::lround(audioVolumes[channel] * 100.0F)));
            drawCenteredText(
                renderer, dialogFont, value,
                SDL_Rect{origin.x + 326, origin.y + labelTop[channel],
                         39, channel == 0 ? 23 : 22});
        }

        if (hasSubtitleSettings) {
            drawCenteredText(
                renderer, dialogFont,
                language.get("sounddlg", "SUBTITLE", "Undertekst"),
                SDL_Rect{origin.x + 11, origin.y + 167, 69, 22});
            drawStateImage(settingsDownNormalTexture,
                           settingsDownPressedTexture,
                           settingsDownNormal.get(), 91, 168,
                           OptionsControl::SubtitlePrevious);
            drawStateImage(settingsUpNormalTexture,
                           settingsUpPressedTexture,
                           settingsUpNormal.get(), 300, 168,
                           OptionsControl::SubtitleNext);
            // 0x00422880 shows the entry's text after its ':'.
            const auto countries = optionsCountries();
            std::string countryName;
            if (subtitleLanguage >= 0 &&
                static_cast<std::size_t>(subtitleLanguage) < countries.size()) {
                const std::string &entry =
                    countries[static_cast<std::size_t>(subtitleLanguage)];
                const auto colon = entry.find(':');
                countryName = colon == std::string::npos
                                  ? entry
                                  : entry.substr(colon + 1);
            }
            drawCenteredText(
                renderer, dialogFont, countryName,
                SDL_Rect{origin.x + 123, origin.y + 167, 163, 23});
            drawStateImage(settingsToggleNormalTexture,
                           settingsTogglePressedTexture,
                           settingsToggleNormal.get(), 324, 165,
                           OptionsControl::SubtitleToggle);
            drawCenteredText(
                renderer, dialogFont,
                subtitlesEnabled
                    ? language.get("sounddlg", "ON", "På")
                    : language.get("sounddlg", "OFF", "Av"),
                SDL_Rect{origin.x + 324, origin.y + 165,
                         settingsToggleNormal->w,
                         settingsToggleNormal->h});
        }

        const int buttonY = hasSubtitleSettings ? 205 : 168;
        drawStateImage(settingsButtonNormalTexture,
                       settingsButtonPressedTexture,
                       settingsButtonNormal.get(), 197, buttonY,
                       OptionsControl::Ok);
        drawStateImage(settingsButtonNormalTexture,
                       settingsButtonPressedTexture,
                       settingsButtonNormal.get(), 286, buttonY,
                       OptionsControl::Cancel);
        drawCenteredText(
            renderer, dialogFont,
            language.get("interfaceh", "OK", "OK"),
            SDL_Rect{origin.x + 197, origin.y + buttonY,
                     settingsButtonNormal->w, settingsButtonNormal->h});
        drawCenteredText(
            renderer, dialogFont,
            language.get("interfaceh", "CANCEL", "Avbryt"),
            SDL_Rect{origin.x + 286, origin.y + buttonY,
                     settingsButtonNormal->w, settingsButtonNormal->h});
    }

    if (storyNavigationOpen) {
        SDL_Rect backdropDestination{
            storyNavigationOrigin.x, storyNavigationOrigin.y,
            storyNavigationBackdrop->w, storyNavigationBackdrop->h};
        SDL_RenderCopy(renderer, storyNavigationBackdropTexture.get(), nullptr,
                       &backdropDestination);
        for (const auto &button : storyNavigationButtons) {
            const bool disabled =
                (button.control == StoryNavigationControl::Previous &&
                 storyPreviousScene.empty()) ||
                (button.control == StoryNavigationControl::Next &&
                 storyNextScene.empty());
            SDL_Texture *texture = disabled
                                       ? button.disabledTexture.get()
                                       : pressedStoryNavigationControl ==
                                                 button.control
                                             ? button.pressedTexture.get()
                                             : button.normalTexture.get();
            SDL_Rect destination{
                storyNavigationOrigin.x + button.relativeBounds.x,
                storyNavigationOrigin.y + button.relativeBounds.y,
                button.relativeBounds.w, button.relativeBounds.h};
            SDL_RenderCopy(renderer, texture, nullptr, &destination);
        }
    }

    // The race runs in a 640x480 display mode, and the original centres its
    // boxes on that display (0x00421070 reads the back buffer's size), which
    // DxWnd stretches with the race. So in the race the dialog is drawn on an
    // 800x600 target and its centred 640x480 part is laid over the race's
    // frame before that is stretched again.
    SDL_Texture *dialogPrevious = nullptr;
    if (raceDialog) {
        if (!raceDialogTarget) {
            raceDialogTarget.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                     SDL_TEXTUREACCESS_TARGET, 800, 600));
            SDL_SetTextureBlendMode(raceDialogTarget.get(), SDL_BLENDMODE_BLEND);
        }
        dialogPrevious = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, raceDialogTarget.get());
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
    }
    if (dialogMode == DialogMode::Name) {
        constexpr int dialogX = 249;
        constexpr int dialogY = 247;
        SDL_Rect destination{dialogX, dialogY,
                             nameDialogBackdrop->w, nameDialogBackdrop->h};
        SDL_RenderCopy(renderer, nameDialogBackdropTexture.get(), nullptr,
                       &destination);

        if (dialogPressedControl == DialogControl::NameOk) {
            SDL_Rect button{dialogX + 15, dialogY + 69,
                            nameDialogOk->w, nameDialogOk->h};
            SDL_RenderCopy(renderer, nameDialogOkTexture.get(), nullptr, &button);
        }
        if (dialogPressedControl == DialogControl::NameCancel) {
            SDL_Rect button{dialogX + 204, dialogY + 69,
                            nameDialogCancel->w, nameDialogCancel->h};
            SDL_RenderCopy(renderer, nameDialogCancelTexture.get(), nullptr,
                           &button);
        }

        drawText(renderer, dialogFont,
                 language.get("menu", "ENTERNAME", "Fyll inn navn"),
                 dialogX + 11, dialogY + 6);
        const auto cp1252Name = utf8ToCp1252(pendingProfileName);
        if (cp1252Name) {
            // FGP.exe assigns Amerigo BT 14 to the (11,30,281,33)
            // input object and flag 1 vertically centers the text.
            drawText(renderer, calendarFont, *cp1252Name,
                     dialogX + 11, dialogY + 35);
            if ((SDL_GetTicks() / 500U) % 2U == 0U) {
                const int caretX =
                    dialogX + 11 + textWidth(calendarFont, *cp1252Name);
                drawText(renderer, calendarFont, "|", caretX, dialogY + 35);
            }
        }
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "OK", "OK"),
                         SDL_Rect{dialogX + 15, dialogY + 69, 84, 30});
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "CANCEL", "Avbryt"),
                         SDL_Rect{dialogX + 204, dialogY + 69, 84, 29});
    } else if (dialogMode == DialogMode::Calendar) {
        constexpr int dialogX = 250;
        constexpr int dialogY = 157;
        static constexpr std::array<int, 12> daysInMonth{
            31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        static constexpr std::array<const char *, 12> monthKeys{
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DES"};
        static constexpr std::array<const char *, 12> monthFallbacks{
            "Januar", "Februar", "Mars", "April", "Mai", "Juni",
            "Juli", "August", "September", "Oktober", "November", "Desember"};

        SDL_Rect destination{dialogX, dialogY,
                             calendarBackdrop->w, calendarBackdrop->h};
        SDL_RenderCopy(renderer, calendarBackdropTexture.get(), nullptr,
                       &destination);

        if (dialogPressedControl == DialogControl::CalendarPrevious) {
            SDL_Rect button{dialogX + 11, dialogY + 40,
                            calendarPrevious->w, calendarPrevious->h};
            SDL_RenderCopy(renderer, calendarPreviousTexture.get(), nullptr,
                           &button);
        }
        if (dialogPressedControl == DialogControl::CalendarNext) {
            SDL_Rect button{dialogX + 249, dialogY + 38,
                            calendarNext->w, calendarNext->h};
            SDL_RenderCopy(renderer, calendarNextTexture.get(), nullptr, &button);
        }
        if (dialogPressedControl == DialogControl::CalendarOk) {
            SDL_Rect button{dialogX + 194, dialogY + 239,
                            calendarOk->w, calendarOk->h};
            SDL_RenderCopy(renderer, calendarOkTexture.get(), nullptr, &button);
        }

        drawText(renderer, dialogFont,
                 language.get("calendar", "TITLE", "Fyll inn fødselsdato"),
                 dialogX + 13, dialogY + 12);
        drawCenteredText(
            renderer, calendarFont,
            language.get("calendar", monthKeys[pendingBirthMonth],
                         monthFallbacks[pendingBirthMonth]),
            SDL_Rect{dialogX + 55, dialogY + 44, 190, 27});

        const int selectedIndex = pendingBirthDay - 1;
        SDL_Rect mark{dialogX + 13 + (selectedIndex % 7) * 39,
                      dialogY + 81 + (selectedIndex / 7) * 38,
                      calendarMark->w, calendarMark->h};
        SDL_RenderCopy(renderer, calendarMarkTexture.get(), nullptr, &mark);

        for (int day = 1; day <= daysInMonth[pendingBirthMonth]; ++day) {
            const int index = day - 1;
            const std::string label = day < 10
                                          ? " " + std::to_string(day)
                                          : std::to_string(day);
            // The executable draws "%2d" in Amerigo BT 14 at
            // (8 + 39*column, 4 + 38*row) inside the day-grid object.
            drawText(renderer, calendarFont, label,
                     dialogX + 21 + (index % 7) * 39,
                     dialogY + 85 + (index / 7) * 38);
        }
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "OK", "OK"),
                         SDL_Rect{dialogX + 194, dialogY + 239, 87, 34});
    } else if (dialogMode == DialogMode::Message) {
        constexpr int dialogX = (800 - 380) / 2;
        const auto lines = messageLines();
        const int textHeight = static_cast<int>(lines.size()) * messageFont.height;
        const int scalableHeight = textHeight + 12;
        const int dialogHeight = 121 + scalableHeight;
        // 0x00421070 centres as 300 - height / 2, halving each term, so an
        // odd height sits a pixel lower than (600 - height) / 2.
        const int dialogY = 600 / 2 - dialogHeight / 2;

        // Exact nine-slice bounds recovered from FGP.exe: the source
        // template's scalable rectangle is (16,58)-(364,501), replaced
        // by a 342 x (measured text height + 12) middle.
        static constexpr std::array<int, 4> sourceX{0, 16, 364, 386};
        static constexpr std::array<int, 4> destinationX{0, 16, 358, 380};
        const std::array<int, 4> sourceY{0, 58, 501, 564};
        const std::array<int, 4> destinationY{
            0, 58, 58 + scalableHeight, dialogHeight};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                SDL_Rect source{
                    sourceX[column], sourceY[row],
                    sourceX[column + 1] - sourceX[column],
                    sourceY[row + 1] - sourceY[row]};
                SDL_Rect destination{
                    dialogX + destinationX[column],
                    dialogY + destinationY[row],
                    destinationX[column + 1] - destinationX[column],
                    destinationY[row + 1] - destinationY[row]};
                SDL_RenderCopy(renderer, messageBackdropTexture.get(), &source,
                               &destination);
            }
        }

        // The original text objects: the title uses Amerigo BT_14_ in
        // (16,9,342,31) with alignment 0x101, so it is centered on both
        // axes; the body uses Comic Sans MS_10_ in (22,64,330,textH+12),
        // drawn from its top (0x00402750 at 0x0049cbf0 + 6, 0x0049cbf4 + 6).
        drawCenteredText(renderer, calendarFont, dialogTitle,
                         SDL_Rect{dialogX + 16, dialogY + 9, 342, 31});
        int lineY = dialogY + 64;
        for (const auto &line : lines) {
            drawText(renderer, messageFont, line, dialogX + 22, lineY);
            lineY += messageFont.height;
        }

        const int buttonY = dialogY + 85 + textHeight;
        auto drawMessageButton = [&](int localX,
                                     DialogControl control,
                                     std::string_view label) {
            SDL_Rect button{dialogX + localX, buttonY,
                            messageButtonNormal->w, messageButtonNormal->h};
            SDL_RenderCopy(renderer, messageButtonNormalTexture.get(), nullptr,
                           &button);
            if (dialogPressedControl == control) {
                SDL_RenderCopy(renderer, messageButtonPressedTexture.get(), nullptr,
                               &button);
            }
            drawCenteredText(renderer, dialogFont, label, button);
        };
        if (dialogMessageKind == MessageKind::ExitConfirmation ||
            dialogMessageKind == MessageKind::MorningRideConfirmation ||
            dialogMessageKind ==
                MessageKind::BuildacarDemolishConfirmation ||
            dialogMessageKind == MessageKind::ChessAbortConfirmation ||
            dialogMessageKind == MessageKind::RacingQuitConfirmation ||
            dialogMessageKind == MessageKind::ColorfillConfirmation ||
            dialogMessageKind == MessageKind::DraughtsAbortConfirmation ||
            dialogMessageKind ==
                MessageKind::ActivitypageClearConfirmation ||
            dialogMessageKind ==
                MessageKind::ActivitypagePrintConfirmation ||
            dialogMessageKind == MessageKind::TournamentAbortConfirmation) {
            drawMessageButton(188, DialogControl::MessagePrimary,
                              language.get("interfaceh", "YES", "Ja"));
            drawMessageButton(275, DialogControl::MessageSecondary,
                              language.get("interfaceh", "NO", "Nei"));
        } else {
            drawMessageButton(275, DialogControl::MessagePrimary,
                              language.get("interfaceh", "OK", "OK"));
        }
    } else if (dialogMode == DialogMode::Award) {
        constexpr int dialogX = (800 - 317) / 2;
        constexpr int dialogY = (600 - 345) / 2;
        SDL_Rect backdropDestination{
            dialogX, dialogY, awardBackdrop->w, awardBackdrop->h};
        SDL_RenderCopy(renderer, awardBackdropTexture.get(), nullptr,
                       &backdropDestination);

        drawCenteredText(renderer, calendarFont, awardTitle,
                         SDL_Rect{dialogX + 13, dialogY + 8,
                                  291, 31});
        int descriptionY = dialogY + 55;
        for (const auto &line :
             wrapText(dialogFont, awardDescription, 285)) {
            drawText(renderer, dialogFont, line,
                     dialogX + 16, descriptionY);
            descriptionY += dialogFont.height;
        }

        if (awardIconTexture && awardIconWidth > 0 &&
            awardIconHeight > 0) {
            SDL_Rect iconDestination{dialogX + 10, dialogY + 236,
                                     awardIconWidth, awardIconHeight};
            SDL_RenderCopy(renderer, awardIconTexture.get(), nullptr,
                           &iconDestination);
        }
        if (awardMedalTexture && awardMedalWidth > 0 &&
            awardMedalHeight > 0) {
            SDL_Rect medalDestination{dialogX + 242, dialogY + 236,
                                      awardMedalWidth, awardMedalHeight};
            SDL_RenderCopy(renderer, awardMedalTexture.get(), nullptr,
                           &medalDestination);
        }

        SDL_Rect closeButton{dialogX + 216, dialogY + 297,
                             awardButtonNormal->w,
                             awardButtonNormal->h};
        SDL_RenderCopy(renderer, awardButtonNormalTexture.get(), nullptr,
                       &closeButton);
        if (dialogPressedControl == DialogControl::AwardClose) {
            SDL_RenderCopy(renderer, awardButtonPressedTexture.get(), nullptr,
                           &closeButton);
        }
        drawCenteredText(
            renderer, dialogFont,
            language.get("interfaceh", "CLOSE", "Lukk"), closeButton);
    } else if (dialogMode == DialogMode::HighScore) {
        constexpr int dialogX = (800 - 325) / 2;
        constexpr int dialogY = (600 - 395) / 2;
        SDL_Rect backdropDestination{
            dialogX, dialogY, highScoreBackdrop->w, highScoreBackdrop->h};
        SDL_RenderCopy(renderer, highScoreBackdropTexture.get(), nullptr,
                       &backdropDestination);

        static constexpr std::array<SDL_Rect, 3> tabBounds{{
            {14, 0, 94, 23}, {112, 0, 95, 23}, {211, 0, 93, 23}}};
        const std::size_t selected =
            static_cast<std::size_t>(highScoreTab);
        SDL_Rect selectedDestination{
            dialogX + tabBounds[selected].x,
            dialogY + tabBounds[selected].y,
            tabBounds[selected].w, tabBounds[selected].h};
        SDL_RenderCopy(renderer, highScoreSelectedTabTextures[selected].get(),
                       nullptr, &selectedDestination);

        if (dialogPressedControl == DialogControl::HighScoreClose) {
            SDL_Rect closePressed{dialogX + 221, dialogY + 352,
                                  highScoreClosePressed->w,
                                  highScoreClosePressed->h};
            SDL_RenderCopy(renderer, highScoreClosePressedTexture.get(),
                           nullptr, &closePressed);
        }

        // 0x0041f360 and 0x0041f4a0: a table whose format is a time
        // (flags & 3, 0x0041f36c and 0x0041f5b0) takes the *_TIME tab labels
        // and TIME for its score column.
        const bool timed = (highScoreFlags & 3U) != 0U;
        drawCenteredText(renderer, dialogFont,
                         timed ? language.get("interfaceh", "SCORE_PROFILE_TIME", "Tider")
                               : language.get("interfaceh", "SCORE_PROFILE", "Poeng"),
                         SDL_Rect{dialogX + 14, dialogY, 94, 23});
        drawCenteredText(
            renderer, dialogFont,
            timed ? language.get("interfaceh", "SCORE_GLOBAL_TIME", "Bestetider")
                  : language.get("interfaceh", "SCORE_GLOBAL", "Poengtopp"),
            SDL_Rect{dialogX + 112, dialogY, 95, 23});
        drawCenteredText(
            renderer, dialogFont,
            timed ? language.get("interfaceh", "SCORE_UNION_TIME", "Spillertopp")
                  : language.get("interfaceh", "SCORE_UNION", "Spillertopp"),
            SDL_Rect{dialogX + 211, dialogY, 93, 23});
        drawCenteredText(renderer, dialogFont, highScoreGameName,
                         SDL_Rect{dialogX + 8, dialogY + 29, 302, 18});
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "NAME", "Navn"),
                         SDL_Rect{dialogX + 6, dialogY + 49, 118, 19});
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "DATE", "Dato"),
                         SDL_Rect{dialogX + 126, dialogY + 49, 83, 19});
        drawCenteredText(renderer, dialogFont,
                         timed ? language.get("interfaceh", "TIME", "Tid")
                               : language.get("interfaceh", "POINTS", "Poeng"),
                         SDL_Rect{dialogX + 211, dialogY + 49, 102, 19});

        const auto drawRow = [&](const HighScoreRow &row, int y) {
            // 0x0041ed90: points as %d, times as minutes, seconds and
            // hundredths (flags 0x102).
            const std::string score = formatScore(row.score, highScoreFlags);
            drawText(renderer, dialogFont, row.player, dialogX + 14, y);
            drawCenteredText(renderer, dialogFont, row.date,
                             SDL_Rect{dialogX + 126, y - 2, 83, 21});
            drawText(renderer, dialogFont, score,
                     dialogX + 310 - textWidth(dialogFont, score), y);
        };
        if (highScoreHasCurrentRow) {
            drawRow({highScorePlayer, highScoreDate,
                     static_cast<std::uint32_t>(std::max(0, highScoreValue))},
                    dialogY + 74);
        }

        const std::vector<HighScoreRow> *rankedRows = nullptr;
        if (highScoreTab == HighScoreTab::Profile) {
            rankedRows = &highScoreProfileRows;
        } else if (highScoreTab == HighScoreTab::Global) {
            rankedRows = &highScoreGlobalRows;
        } else {
            rankedRows = &highScoreUnionRows;
        }
        for (std::size_t index = 0; index < rankedRows->size(); ++index) {
            drawRow((*rankedRows)[index],
                    dialogY + 100 + static_cast<int>(index) * 26);
        }
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "CLOSE", "Lukk"),
                         SDL_Rect{dialogX + 221, dialogY + 352, 82, 31});
    } else if (dialogMode == DialogMode::Help) {
        constexpr int dialogX = 158;
        constexpr int dialogY = 93;
        SDL_Rect backdropDestination{
            dialogX, dialogY, helpBackdrop->w, helpBackdrop->h};
        SDL_RenderCopy(renderer, helpBackdropTexture.get(), nullptr,
                       &backdropDestination);

        // HELP_Create at 0x0041e390 uses these executable-owned rectangles:
        // title (15,10)-(463,39), topics (15,56)-(134,348), and body
        // (153,56)-(443,348). Both text panes use Amerigo BT_10_; the title
        // uses Amerigo BT_14_.
        drawCenteredText(renderer, calendarFont, dialogTitle,
                         SDL_Rect{dialogX + 15, dialogY + 10, 448, 29});

        SDL_Rect topicClip{dialogX + 15, dialogY + 56, 119, 292};
        SDL_RenderSetClipRect(renderer, &topicClip);
        const std::size_t visibleTopics = std::max<std::size_t>(
            1U, 292U /
                    static_cast<std::size_t>(std::max(dialogFont.height, 1)));
        for (std::size_t row = 0; row < visibleTopics; ++row) {
            const std::size_t index = helpFirstVisibleTopic + row;
            if (index >= helpPages.size()) {
                break;
            }
            if (index == helpTopicIndex) {
                // The reference game fills the selected topic with the same
                // light, subtly textured material as the title panel. Reuse
                // that packaged artwork instead of approximating its colour.
                SDL_Rect selectionSource{20, 15, 119, dialogFont.height};
                SDL_Rect selectionDestination{
                    dialogX + 15,
                    dialogY + 56 +
                        static_cast<int>(row) * dialogFont.height,
                    119, dialogFont.height};
                SDL_RenderCopy(renderer, helpBackdropTexture.get(),
                               &selectionSource, &selectionDestination);
            }
            drawText(renderer, dialogFont, helpPages[index].title,
                     dialogX + 15,
                     dialogY + 56 +
                         static_cast<int>(row) * dialogFont.height);
        }

        SDL_Rect bodyClip{dialogX + 153, dialogY + 56, 290, 292};
        SDL_RenderSetClipRect(renderer, &bodyClip);
        const auto bodyLines = helpBodyLines();
        int lineY = dialogY + 56 - helpBodyScrollLine * dialogFont.height;
        for (const auto &line : bodyLines) {
            drawText(renderer, dialogFont, line, dialogX + 153, lineY);
            lineY += dialogFont.height;
        }
        SDL_RenderSetClipRect(renderer, nullptr);

        auto drawHelpControl = [&](TexturePtr &normal, TexturePtr &pressed,
                                   DialogControl control, SDL_Rect bounds) {
            SDL_RenderCopy(renderer, normal.get(), nullptr, &bounds);
            if (dialogPressedControl == control) {
                SDL_RenderCopy(renderer, pressed.get(), nullptr, &bounds);
            }
        };
        drawHelpControl(
            helpPreviousNormalTexture, helpPreviousPressedTexture,
            DialogControl::HelpPrevious,
            SDL_Rect{dialogX + 10, dialogY + 364, 31, 30});
        drawHelpControl(
            helpNextNormalTexture, helpNextPressedTexture,
            DialogControl::HelpNext,
            SDL_Rect{dialogX + 109, dialogY + 364, 31, 30});
        drawHelpControl(
            helpUpNormalTexture, helpUpPressedTexture,
            DialogControl::HelpScrollUp,
            SDL_Rect{dialogX + 450, dialogY + 51, 14, 17});
        drawHelpControl(
            helpDownNormalTexture, helpDownPressedTexture,
            DialogControl::HelpScrollDown,
            SDL_Rect{dialogX + 450, dialogY + 338, 14, 17});

        SDL_Rect thumb = helpScrollThumbBounds();
        SDL_RenderCopy(renderer, helpThumbTexture.get(), nullptr, &thumb);

        SDL_Rect okButton{dialogX + 383, dialogY + 365, 83, 27};
        SDL_RenderCopy(renderer, helpButtonNormalTexture.get(), nullptr,
                       &okButton);
        if (dialogPressedControl == DialogControl::HelpOk) {
            SDL_RenderCopy(renderer, helpButtonPressedTexture.get(), nullptr,
                           &okButton);
        }
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "OK", "OK"), okButton);
    }
    // Resolve hover state from one authoritative logical coordinate on
    // every frame. Motion events normally keep this current, but a
    // click can reposition the pointer without a preceding motion
    // event, and the yard's animated masks can change beneath a
    // stationary pointer. Limiting this refresh to the yard left the
    // menu and other scenes with stale cursor classifications.
    if (mouseInsideWindow && !sceneSequenceBlocksInput &&
        dialogMode == DialogMode::None &&
        viewMode != ViewMode::Options) {
        const auto resolvedHotspot = hotspotAt(mouseX, mouseY);
        const bool animatedSceneCharacter =
            viewMode == ViewMode::Scene &&
            !sceneCharacterGroups.empty();
        if (resolvedHotspot != hoveredHotspot ||
            animatedSceneCharacter) {
            if (resolvedHotspot != hoveredHotspot) {
                referenceTrace.emit("hotspot_resolve",
                                    {{"logical_x", mouseX},
                                     {"logical_y", mouseY},
                                     {"hotspot", resolvedHotspot},
                                     {"reason", "hover"}});
            }
            hoveredHotspot = resolvedHotspot;
            hoveredBounds = currentHotspotBounds(resolvedHotspot);
        }
    }
    if (raceDialog) {
        SDL_SetRenderTarget(renderer, racingGame->target.get());
        // The shared modal mesh (0x004a61b0) at its own pixel size.
        const SDL_Rect frame{0, 0, 640, 480};
        if (modalMeshTexture) SDL_RenderCopy(renderer, modalMeshTexture.get(), &frame, nullptr);
        const SDL_Rect centre{80, 60, 640, 480};
        SDL_RenderCopy(renderer, raceDialogTarget.get(), &centre, nullptr);
        SDL_SetRenderTarget(renderer, dialogPrevious);
        SDL_RenderCopy(renderer, racingGame->target.get(), nullptr, nullptr);
    }
    if (mouseInsideWindow) {
        const CursorKind wantedCursor = cursorForContext();
        const auto cursorNow = std::chrono::steady_clock::now();
        if (wantedCursor != activeCursor) {
            activeCursor = wantedCursor;
            cursorAnimationStart = cursorNow;
        }
        auto &cursor = cursors[static_cast<std::size_t>(activeCursor)];
        const bool butterflyNetCursor =
            viewMode == ViewMode::Scene && activeScene == "butterfly" &&
            butterflyGame &&
            butterflyGame->phase == ButterflyPhase::Playing &&
            dialogMode == DialogMode::None;
        const bool whackamoleCursor =
            viewMode == ViewMode::Scene && activeScene == "whackamole" &&
            whackamoleGame &&
            whackamoleGame->phase == WhackamolePhase::Playing &&
            dialogMode == DialogMode::None;
        // The logo's 0x103 handler (0x00416730) calls 0x0040c370 with the
        // empty name at 0x0049c7a0, cursor mode 0: no cursor until the next
        // scene sets one (the 2026-10-18 logo capture shows none).
        // Town's initializer does the same (0x00426fd6) for the whole scene;
        // the 2026-10-19 town-reel-skip capture shows no cursor over reel1.
        // The race hides it too (0x0040c370 with the same name at
        // 0x0047490c) except while a popup is open, which sets "default"
        // (0x0047a8b0) and hides it again on closing (0x0047a940).
        const bool logoCursorHidden =
            viewMode == ViewMode::Scene &&
            (activeScene == "logo" || activeScene == "town" ||
             (activeScene == "racing" && dialogMode == DialogMode::None));
        if (!butterflyNetCursor && !whackamoleCursor && !logoCursorHidden &&
            !colorfillHidesCursor() && cursor.texture &&
            !cursor.media.videoFrames.empty()) {
            const double cursorTime = std::chrono::duration<double>(
                                          cursorNow - cursorAnimationStart)
                                          .count();
            const auto frameIndex =
                frameIndexAt(cursor.media, cursorTime, true);
            const auto &frame = cursor.media.videoFrames[frameIndex];
            uploadVideoFrame(cursor.texture.get(), frame);
            // FGP.exe positions the cursor Smacker player directly at
            // the logical mouse coordinate. The artwork itself places
            // the active tip at its (0,0) origin.
            SDL_Rect destination{
                mouseX, mouseY, frame.width, frame.height};
            SDL_RenderCopy(renderer, cursor.texture.get(), nullptr,
                           &destination);
        }
        if (whackamoleCursor) {
            renderWhackamoleCursor(sceneElapsedNow());
        }
        if (showHotspots) {
            const std::string hoverDiagnostic =
                "hotspot " + std::to_string(hoveredHotspot) + " -> " +
                cursorNames[static_cast<std::size_t>(wantedCursor)] + ".smk";
            SDL_Rect diagnosticBackdrop{
                6, 6,
                textWidth(dialogFont, hoverDiagnostic) + 12,
                dialogFont.height + 8};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
            SDL_RenderFillRect(renderer, &diagnosticBackdrop);
            drawText(renderer, dialogFont, hoverDiagnostic, 12, 10);
        }
    }
    // Browser build: a pending scene's data is fetched in the background
    // first, behind a loading overlay, and the scene loads once it has
    // arrived. Desktop builds always report it ready.
    updateWebControls();
    const double sceneData =
        pendingScene ? sceneDataProgress(pendingScene->first) : 1.0;
    sceneDataLoading = sceneData < 1.0;
    if (sceneDataLoading) {
        drawLoadingOverlay(sceneData);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    if (!developerScreenshotPath.empty()) {
        int width = kLogicalWidth;
        int height = kLogicalHeight;
        SDL_GetRendererOutputSize(renderer, &width, &height);
        SurfacePtr shot(SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        if (shot && SDL_RenderReadPixels(renderer, nullptr,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         shot->pixels, shot->pitch) == 0) {
            SDL_SaveBMP(shot.get(), developerScreenshotPath.c_str());
        }
        developerScreenshotPath.clear();
    }
    SDL_RenderPresent(renderer);
    if (sceneReadyAfterPresent) {
        referenceTrace.emit("scene_ready", {{"scene", *sceneReadyAfterPresent}});
        sceneReadyAfterPresent.reset();
    }
    if (pendingScene && !sceneDataLoading) {
        auto request = std::move(*pendingScene);
        const int entryMode = pendingSceneEntryMode;
        pendingScene.reset();
        pendingSceneEntryMode = 0;
        if (audioDumpActive) {
            referenceTrace.emit("diagnostic", {
                {"kind", "audio_dump_scene_load"}, {"phase", "begin"},
                {"frames", audioMixer.dumpedFrames()}});
        }
        // Loading decodes the scene synchronously; in the browser that
        // starves the main-thread WebAudio feed (platform/web_audio.h).
        suspendWebAudioForBlockingWork();
        loadScene(request.first, request.second, entryMode);
        resumeWebAudioAfterBlockingWork();
        audioMixer.releaseBackgroundHold();
        if (audioDumpActive) {
            referenceTrace.emit("diagnostic", {
                {"kind", "audio_dump_scene_load"}, {"phase", "end"},
                {"frames", audioMixer.dumpedFrames()}});
        }
    }
#ifndef __EMSCRIPTEN__
    // A browser frame returns to the event loop instead; a delay there would
    // only busy-wait.
    SDL_Delay(1);
#endif
}

// Shown while the browser fetches a scene's data. It is built from the
// original's own parts: the modal checker mesh that darkens the scene behind
// dialogs, the animated wait cursor, and the [gamec] LOADING text ("Vent
// litt"), with a progress bar in the menu's brass colours.
void GameApp::drawLoadingOverlay(double progress) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (modalMeshTexture) {
        SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr, nullptr);
    }
    int top = 236;
    auto &wait = cursors[static_cast<std::size_t>(CursorKind::Wait)];
    if (wait.texture && !wait.media.videoFrames.empty()) {
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        const auto &frame = wait.media.videoFrames[frameIndexAt(wait.media, now, true)];
        uploadVideoFrame(wait.texture.get(), frame);
        SDL_Rect destination{400 - frame.width, top, frame.width * 2, frame.height * 2};
        SDL_RenderCopy(renderer, wait.texture.get(), nullptr, &destination);
        top += frame.height * 2 + 12;
    }
    const std::string text = language.get("gamec", "LOADING", "Vent litt");
    drawText(renderer, messageFont, text, 400 - textWidth(messageFont, text) / 2, top);
    top += messageFont.height + 12;

    const SDL_Rect frame{268, top, 264, 12};
    SDL_SetRenderDrawColor(renderer, 30, 20, 10, 230);
    SDL_RenderFillRect(renderer, &frame);
    SDL_SetRenderDrawColor(renderer, 196, 152, 74, 255);
    SDL_RenderDrawRect(renderer, &frame);
    const int filled = static_cast<int>((frame.w - 4) * std::clamp(progress, 0.0, 1.0));
    const SDL_Rect fill{frame.x + 2, frame.y + 2, filled, frame.h - 4};
    SDL_SetRenderDrawColor(renderer, 232, 184, 74, 255);
    SDL_RenderFillRect(renderer, &fill);
}

} // namespace fgp
