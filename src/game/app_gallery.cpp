#include "game/app_gallery.h"
#include "assets/original_data.h"

#include <array>
#include <cstdio>
#include <iostream>
#include <string>

#include "assets/profile.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// 0x0049fee8: the (x, y) each portrait bitmap is added at, indexed by the
// shown character; 0x00469041/0x0046903a read it as [index*8], [index*8+4].
const auto kPortraitPositions = originalArray<SDL_Point, 6>(0x49fee8U);
// 0x00469140 wraps the index modulo 6 (`mov ecx, 6; idiv ecx`).
constexpr int kPortraitCount = 6;
// 0x00469072: EVENT_AddTimer(now + 0x5dc, 0, 1) -- the speech starts 1.5 s
// after its portrait.
constexpr double kSpeechDelay = 1.5;
// The controls 0x00468f20 creates, with their 0x116 ids.
constexpr int kHelpControl = 1;
constexpr int kExitControl = 2;
constexpr int kPreviousControl = 3;
constexpr int kNextControl = 4;

std::string portraitName(int index) {
    char name[16]{};
    std::snprintf(name, sizeof(name), "character_%02d", index + 1);
    return name;
}

} // namespace

// Scene initialisation 0x00468f20.
GalleryScene::GalleryViewer GalleryScene::loadGalleryViewer() {
    GalleryViewer viewer;
    const auto art = [&](std::string_view name) {
        return viewerAsset("bitmap\\" + std::string(name) + ".bmp");
    };
    // 0x00468f44-0x00468fb3: help (0,0), exit (0x2d8,0), previous
    // (0x2a6,0x225) and next (0x2e5,0x225), each with _0/_1 artwork.
    viewer.widgets.add(renderer, kHelpControl, 0, 0, art("help_0"),
                       art("help_1"));
    viewer.widgets.add(renderer, kExitControl, 0x2d8, 0, art("exit_0"),
                       art("exit_1"));
    viewer.widgets.add(renderer, kPreviousControl, 0x2a6, 0x225,
                       art("prev_0"), art("prev_1"));
    viewer.widgets.add(renderer, kNextControl, 0x2e5, 0x225, art("next_0"),
                       art("next_1"));
    for (int index = 0; index < kPortraitCount; ++index) {
        auto &surface = viewer.portraits[static_cast<std::size_t>(index)];
        surface = loadGreenKeyedBmp(art(portraitName(index)));
        auto &texture = viewer.portraitTextures[static_cast<std::size_t>(index)];
        texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    }
    std::cout << "Loaded executable-derived Gallery: six portraits from "
                 "0x0049fee8, speech 1.5 s after each, previous/next wrap "
                 "modulo 6\n";
    return viewer;
}

void GalleryScene::cancelGallerySpeechTimer() {
    // 0x00469090: EVENT_RemoveTimer when 0x006b9674 holds one.
    if (galleryViewer) galleryViewer->speechTimerDue.reset();
}

void GalleryScene::removeGallerySpeech() {
    auto &viewer = *galleryViewer;
    if (!viewer.speechAdded) return;
    viewer.speechAdded = false;
    if (viewer.speechEffect != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(viewer.speechEffect);
        viewer.speechEffect = LiveAudioMixer::kInvalidEffectHandle;
    }
    traceViewerRemove(portraitName(viewer.index));
}

// 0x00468fe0: replace the portrait and re-arm the speech timer.
void GalleryScene::showGalleryPortrait(int index, double now) {
    auto &viewer = *galleryViewer;
    // 0x00468fe0-0x0046901c: remove the bitmap 0x00509718, then the speech
    // 0x005097c0, whichever is added (0x004138f0 reads +0x7c).
    if (viewer.portraitAdded) {
        viewer.portraitAdded = false;
        traceViewerRemove(portraitName(viewer.index));
    }
    removeGallerySpeech();
    cancelGallerySpeechTimer();
    viewer.index = index;
    // 0x00469051: SCENE_AddAnim(bitmap, table x, table y, layer 0).
    const auto position = kPortraitPositions[static_cast<std::size_t>(index)];
    viewer.portraitAdded = true;
    traceViewerAdd(portraitName(index), position.x, position.y, 0);
    viewer.speechTimerDue = now + kSpeechDelay;
}

void GalleryScene::leaveGallery() {
    // 0x004690f0: the tvroom visit record (0x00411370, type 0x400, key 0)
    // decides the way out. A visited TV room returns through the declared
    // parent (0x0046911d, tvroom with mode 1); otherwise Contents.
    std::uint32_t visits = 0;
    if (const auto profile = viewerProfilePath())
        visits = profileSceneRecordU32(*profile, "tvroom", 0x400U, 0U, false)
                     .value_or(0U);
    if (static_cast<std::int32_t>(visits) > 0) {
        returnFromScene();
    } else {
        // 0x00469126-0x0046912d: GAME_GotoSceneByName("sceneindex", 0).
        if (!navigationHistory.empty()) navigationHistory.pop_back();
        pendingSceneEntryMode = 0;
        pendingScene = std::pair{std::string("sceneindex"),
                                 std::string("Contents")};
    }
}

// Control event 0x116, 0x004690b0.
void GalleryScene::galleryControl(int id) {
    traceViewerControl("gallery", id);
    auto &viewer = *galleryViewer;
    const double now = sceneElapsedNow();
    switch (id) {
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        leaveGallery();
        break;
    case kPreviousControl:
    case kNextControl: {
        // 0x00469140: (index + step) wrapped into 0..5.
        int next = viewer.index + (id == kNextControl ? 1 : -1);
        if (next < 0) next += kPortraitCount;
        showGalleryPortrait(next % kPortraitCount, now);
        break;
    }
    default:
        break;
    }
}

void GalleryScene::updateGallery(double elapsed) {
    if (!galleryViewer) return;
    auto &viewer = *galleryViewer;
    if (!viewer.entered) {
        viewer.entered = true;
        // 0x00468fc7: the first portrait. 0x00468fd3 keeps the shared
        // "theme" music (0x0040b1f0 restarts nothing already playing).
        showGalleryPortrait(0, elapsed);
    }
    // Timer event 0x111 with tag 1 (0x004691b0): SCENE_PlayAnim on the
    // speech descriptor, which 0x00412b90 adds at its default (0,0), layer 0.
    if (viewer.speechTimerDue && elapsed >= *viewer.speechTimerDue) {
        const double due = *viewer.speechTimerDue;
        viewer.speechTimerDue.reset();
        const std::string name = portraitName(viewer.index);
        // The 4x4 video track carries the clip's length: the player's
        // completion (0x0040c8a0) follows its last frame.
        viewer.speech = decodeSmacker(activeSceneAnimationData(name), true,
                                      true, true);
        viewer.speechDuration = originalSmackerCompletionDuration(viewer.speech);
        viewer.speechStartedAt = due;
        viewer.speechAdded = true;
        traceViewerAdd(name, 0, 0, 0);
        viewer.speechEffect = audioMixer.addEffect(
            viewer.speech, audioVolumes[1] * sceneAudioGains[1]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
    // Animation-finished event 0x110 (0x004691d0) removes the speech.
    if (viewer.speechAdded &&
        elapsed >= viewer.speechStartedAt + viewer.speechDuration) {
        traceViewerFinished(portraitName(viewer.index));
        viewer.speechEffect = LiveAudioMixer::kInvalidEffectHandle;
        removeGallerySpeech();
    }
}

void GalleryScene::renderGallery() {
    if (!galleryViewer) return;
    auto &viewer = *galleryViewer;
    if (viewer.portraitAdded) {
        const auto index = static_cast<std::size_t>(viewer.index);
        const auto &surface = viewer.portraits[index];
        SDL_Rect destination{kPortraitPositions[index].x,
                             kPortraitPositions[index].y, surface->w,
                             surface->h};
        SDL_RenderCopy(renderer, viewer.portraitTextures[index].get(), nullptr,
                       &destination);
    }
    viewer.widgets.render(
        renderer, mouseX, mouseY,
        viewer.widgets.eventIdAt(mouseX, mouseY,
                                 viewerStaticHotspotAt(mouseX, mouseY)),
        [](const ViewerWidget &, const SDL_Rect &) {});
}

bool GalleryScene::beginGalleryInteraction(int x, int y) {
    if (!galleryViewer) return false;
    if (const auto id = galleryViewer->widgets.press(
            x, y, viewerStaticHotspotAt(x, y)))
        galleryControl(*id);
    return true;
}

bool GalleryScene::endGalleryInteraction(int x, int y) {
    if (!galleryViewer) return false;
    if (const auto id = galleryViewer->widgets.release(
            x, y, viewerStaticHotspotAt(x, y)))
        galleryControl(*id);
    return true;
}

// Key event 0x10c, 0x00469170.
bool GalleryScene::handleGalleryKey(SDL_Keycode key) {
    if (!galleryViewer) return false;
    if (key == SDLK_ESCAPE) {
        leaveGallery();
        return true;
    }
    if (key == SDLK_SPACE) {
        // 0x0046917e-0x00469199: cancel the timer and remove the speech.
        cancelGallerySpeechTimer();
        removeGallerySpeech();
        return true;
    }
    return false;
}

void GalleryScene::leaveGalleryScene() {
    if (!galleryViewer) return;
    auto &viewer = *galleryViewer;
    if (viewer.speechEffect != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(viewer.speechEffect);
        viewer.speechEffect = LiveAudioMixer::kInvalidEffectHandle;
    }
}

bool GalleryScene::executeGalleryDeveloperCommand(std::string_view command) {
    if (!galleryViewer) return false;
    if (command == "next") {
        galleryControl(kNextControl);
    } else if (command == "prev") {
        galleryControl(kPreviousControl);
    } else if (command == "silence") {
        handleGalleryKey(SDLK_SPACE);
    } else {
        return false;
    }
    return true;
}

} // namespace fgp
