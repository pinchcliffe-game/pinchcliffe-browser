#include "game/app_movieplayer.h"
#include "assets/original_data.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "assets/profile.h"
#include "media/smacker.h"
#include "util/binary.h"

namespace fgp {
namespace {

// The controls 0x0046d560 creates, with their 0x116 ids.
constexpr int kStopControl = 1;
constexpr int kPlayControl = 2;
constexpr int kHelpControl = 3;
constexpr int kExitControl = 4;
constexpr int kFilmTab = 6;
constexpr int kSoundTab = 7;
// Reel radios use the award index plus 100 (0x0046d7a6).
constexpr int kReelControlBase = 100;
// 0x0049ff64 (film) and 0x0049ff88 (sound): the award indices of the nine
// reels on each tab. Category 1 (FILM) holds 18 entries (0x0049ca8c).
const auto kFilmAwards = originalArray<int, 9>(0x49ff64U);
const auto kSoundAwards = originalArray<int, 9>(0x49ff88U);
constexpr int kFilmAwardCount = 18;
// Category 1's always-open entries (0x0049ca64).
constexpr std::uint32_t kFilmAwardDefaults = 1U;
// Static descriptors (+0x54/+0x58): film1 0x00509e50, film2 0x00509ef8,
// needle1..3 0x00509fa0.., and the reel 0x0050a1a8.
constexpr SDL_Point kFilm1Position{538, 56};
constexpr SDL_Point kFilm2Position{0, 119};
const auto kNeedlePositions = originalArray<SDL_Point, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x00509ff4U + static_cast<std::uint32_t>(index * 168);
    return source.read<SDL_Point>(address);
});
constexpr SDL_Point kReelPosition{74, 130};
// 0x0046d6b8-0x0046d6c4: EVENT_AddTimer(now + 10000, 20000, 0).
constexpr double kNeedleFirst = 10.0;
constexpr double kNeedlePeriod = 20.0;

// 0x0046d950: which tab an award belongs to, and its position there.
std::optional<std::pair<int, int>> reelOf(int award) {
    for (int index = 0; index < 9; ++index) {
        if (kFilmAwards[static_cast<std::size_t>(index)] == award)
            return std::pair{kFilmTab, index};
    }
    for (int index = 0; index < 9; ++index) {
        if (kSoundAwards[static_cast<std::size_t>(index)] == award)
            return std::pair{kSoundTab, index};
    }
    return std::nullopt;
}

TexturePtr streamingTexture(SDL_Renderer *renderer, const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame = media.videoFrames.metadata(0);
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (texture) SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

} // namespace

std::uint32_t MovieplayerScene::movieplayerFilmAwards() {
    // 0x0041b540(1): the second dword of the profile award section.
    const auto profile = viewerProfilePath();
    if (!profile) return 0U;
    const auto bytes = readBinaryFile(*profile);
    if (bytes.size() < 0x60U || readLe32(bytes, 0x4cU) != 0x301U ||
        readLe32(bytes, 0x54U) != 0x20U)
        return 0U;
    return readLe32(bytes, 0x5cU);
}

void MovieplayerScene::loadMovieplayerClip(MovieplayerClip &clip,
                                           std::string name,
                                           SDL_Point position, bool keyed) {
    clip.name = std::move(name);
    clip.position = position;
    const auto data = activeSceneAnimationData(clip.name);
    clip.interlaced = data.size() >= 24 && (data[20] & 0x02U) != 0U;
    // scene_DrawAnim keys a descriptor with +0x48 set on its top-left pixel
    // (0x00413071): film1, film2 and the needles; the reel 0x0050a1a8 has
    // +0x48 clear and keeps the native default.
    clip.media = decodeSmackerStreamed(
        data, clip.name,
        keyed ? SmackerKeying::TopLeftRgb565
              : SmackerKeying::IndexZeroAndGreen,
        true, true);
    clip.texture = streamingTexture(renderer, clip.media);
    clip.duration = originalSmackerCompletionDuration(clip.media);
    clip.added = false;
    clip.playing = false;
    clip.heldFrame = 0U;
}

// SCENE_AddAnim 0x00412840. (-1,-1) selects the descriptor's position;
// SCENE_PlayAnim (0x00412b90) passes that position explicitly.
void MovieplayerScene::addMovieplayerClip(MovieplayerClip &clip, int x,
                                          int y) {
    if (clip.added) return;
    clip.added = true;
    clip.playing = false;
    clip.heldFrame = 0U;
    traceViewerAdd(clip.name, x, y, 0);
}

// 0x004129d0: SmackGoto(0), then run (0x004034a0 -> 0x00403330).
void MovieplayerScene::startMovieplayerClip(MovieplayerClip &clip, double now) {
    if (!clip.added) return;
    if (clip.sound != LiveAudioMixer::kInvalidEffectHandle)
        audioMixer.removeEffect(clip.sound);
    clip.playing = true;
    clip.startedAt = now;
    clip.nextLoopAt = now + clip.media.videoDuration;
    clip.heldFrame = 0U;
    if (!clip.media.audioSamples.empty()) {
        clip.sound = audioMixer.addEffect(
            clip.media, audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

// SCENE_StopAnim 0x00412a60: pause on the current frame.
void MovieplayerScene::stopMovieplayerClip(MovieplayerClip &clip, double now) {
    if (!clip.playing) return;
    const double time = now - clip.startedAt;
    clip.heldFrame = clip.media.videoFrames.empty()
        ? 0U
        : frameIndexAt(clip.media,
                       clip.loop && clip.media.videoDuration > 0.0
                           ? std::fmod(time, clip.media.videoDuration)
                           : time,
                       false);
    clip.playing = false;
    if (clip.sound != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.sound);
        clip.sound = LiveAudioMixer::kInvalidEffectHandle;
    }
    traceViewerStop(clip.name);
}

// SCENE_RemoveAnim 0x00412aa0.
void MovieplayerScene::removeMovieplayerClip(MovieplayerClip &clip) {
    if (!clip.added) return;
    clip.added = false;
    clip.playing = false;
    if (clip.sound != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(clip.sound);
        clip.sound = LiveAudioMixer::kInvalidEffectHandle;
    }
    traceViewerRemove(clip.name);
}

// SCENE_PlayAnim 0x00412b90: add when absent, then start.
void MovieplayerScene::playMovieplayerAnimation(MovieplayerClip &clip,
                                                double now) {
    addMovieplayerClip(clip, clip.position.x, clip.position.y);
    startMovieplayerClip(clip, now);
}

// 0x0046d890: stop the reel and the running film strip.
void MovieplayerScene::stopMovieplayerReel(double now) {
    auto &viewer = *movieplayerViewer;
    if (viewer.film2.playing) removeMovieplayerClip(viewer.film2);
    if (viewer.reel.added && viewer.reel.playing)
        stopMovieplayerClip(viewer.reel, now);
}

// 0x0046d850: the reel-change strip, a stop, and the reel taken down.
void MovieplayerScene::prepareMovieplayerReel(double now) {
    auto &viewer = *movieplayerViewer;
    if (!viewer.film1.playing) playMovieplayerAnimation(viewer.film1, now);
    stopMovieplayerReel(now);
    removeMovieplayerClip(viewer.reel);
}

// 0x0046d8e0: load one reel onto the screen, stopped on its first frame.
void MovieplayerScene::selectMovieplayerReel(int award, double now) {
    auto &viewer = *movieplayerViewer;
    prepareMovieplayerReel(now);
    const auto reel = reelOf(award);
    if (!reel) return;
    const auto [tab, index] = *reel;
    if (tab == kFilmTab) viewer.filmIndex = index;
    else viewer.soundIndex = index;
    char name[16]{};
    std::snprintf(name, sizeof(name), tab == kFilmTab ? "reel%d" : "sound%d",
                  index);
    loadMovieplayerClip(viewer.reel, name, kReelPosition, false);
    // 0x0046d913: descriptor +0x4c = (tab != film), so sound reels loop.
    viewer.reel.loop = tab != kFilmTab;
    // 0x0046d93f: SCENE_AddAnim(reel, -1, -1, 0).
    addMovieplayerClip(viewer.reel, -1, -1);
}

// 0x0046db20: restart the loaded reel and run the film strip.
void MovieplayerScene::playMovieplayerReel(double now) {
    auto &viewer = *movieplayerViewer;
    if (!viewer.reel.added) return;
    if (viewer.reel.playing) stopMovieplayerClip(viewer.reel, now);
    startMovieplayerClip(viewer.reel, now);
    playMovieplayerAnimation(viewer.film2, now);
}

// 0x0046d700: rebuild one tab's reels and load its selected reel.
void MovieplayerScene::selectMovieplayerTab(int tab, double now) {
    auto &viewer = *movieplayerViewer;
    viewer.tab = tab;
    for (int award = 0; award < kFilmAwardCount; ++award)
        viewer.widgets.remove(kReelControlBase + award);
    const std::uint32_t owned = movieplayerFilmAwards();
    const auto &awards = tab == kSoundTab ? kSoundAwards : kFilmAwards;
    const int selected =
        awards[static_cast<std::size_t>(tab == kSoundTab ? viewer.soundIndex
                                                         : viewer.filmIndex)];
    for (int index = 0; index < 9; ++index) {
        const int award = awards[static_cast<std::size_t>(index)];
        // 0x0041b5d0(1, owned, award): a bit of the default or the profile.
        const std::uint32_t bit = 1U << award;
        if ((kFilmAwardDefaults & bit) == 0U && (owned & bit) == 0U) continue;
        // 0x0046d74b: one of three reel pictures, rand() % 3 (0x0040c530).
        const int picture = particleRandom(3);
        char normal[24]{};
        char pressed[24]{};
        std::snprintf(normal, sizeof(normal), "bitmap\\reel%d_0.bmp", picture);
        std::snprintf(pressed, sizeof(pressed), "bitmap\\reel%d_1.bmp",
                      picture);
        // 0x0046d7a6: radio (0x00406fb0) in group 0 at
        // (0x208 + 0x44*(i%3), 0x91 + 0x45*(i/3)).
        auto &widget = viewer.widgets.add(
            renderer, kReelControlBase + award, 0x208 + (index % 3) * 0x44,
            0x91 + (index / 3) * 0x45, viewerAsset(normal),
            viewerAsset(pressed), {}, ViewerWidget::Type::Radio, 0);
        char key[24]{};
        std::snprintf(key, sizeof(key), "FILM_%d", award + 1);
        widget.tooltip = language.get("awarddlg", key, key);
    }
    prepareMovieplayerReel(now);
    // 0x0046d7dd-0x0046d7f0: select the remembered reel and load it if it
    // exists on this tab.
    viewer.widgets.setFrame(kReelControlBase + selected, 1);
    if (viewer.widgets.frame(kReelControlBase + selected) >= 0)
        selectMovieplayerReel(selected, now);
}

// Scene initialisation 0x0046d560.
MovieplayerScene::MovieplayerViewer MovieplayerScene::loadMovieplayerViewer() {
    MovieplayerViewer viewer;
    const auto art = [&](std::string_view name) {
        return viewerAsset("bitmap\\" + std::string(name) + ".bmp");
    };
    viewer.widgets.add(renderer, kHelpControl, 0, 0, art("help_0"),
                       art("help_1"));
    viewer.widgets.add(renderer, kExitControl, 0x2d8, 0, art("exit_0"),
                       art("exit_1"));
    viewer.widgets.add(renderer, kPlayControl, 0x42, 0x18a, {}, art("play_1"));
    viewer.widgets.add(renderer, kStopControl, 0x9a, 0x18a, {}, art("stop_1"));
    // 0x0046d5f7-0x0046d62e: the two tabs are radios of group 1 labelled
    // movieplayer:FILMCLIP and movieplayer:SOUNDCLIP.
    auto &film = viewer.widgets.add(renderer, kFilmTab, 0x216, 0x196, {},
                                    art("button1_1"), {},
                                    ViewerWidget::Type::Radio, 1);
    film.label = language.get("movieplayer", "FILMCLIP", "Filmklipp");
    auto &sound = viewer.widgets.add(renderer, kSoundTab, 0x273, 0x196, {},
                                     art("button2_1"), {},
                                     ViewerWidget::Type::Radio, 1);
    sound.label = language.get("movieplayer", "SOUNDCLIP", "Lydklipp");
    viewer.widgets.setFrame(kFilmTab, 1);
    // 0x0046d650-0x0046d668: the selections saved by the last visit.
    if (const auto profile = viewerProfilePath()) {
        const std::uint32_t saved =
            profileSceneRecordU32(*profile, "movieplayer", 0x402U, 1U)
                .value_or(0U);
        viewer.filmIndex = static_cast<int>(saved & 0xffffU);
        viewer.soundIndex = static_cast<int>(saved >> 16U);
    }
    // 0x0046d673-0x0046d6a5: the hidden car part (descriptor 0x00509da8)
    // while the scene record's key 0 is still zero. The shared collectible
    // code draws it and awards it on hotspot 5 (0x0046da60).
    std::uint32_t carPartTaken = 1U;
    if (const auto profile = viewerProfilePath())
        carPartTaken =
            profileSceneRecordU32(*profile, "movieplayer", 0x402U, 0U)
                .value_or(0U);
    viewer.carPartShown = carPartTaken == 0U;
    std::cout << "Loaded executable-derived Movie Player: film/sound reels "
                 "from award category 1, film1/film2 projector strips, "
                 "needle timer every 20 s\n";
    return viewer;
}

void MovieplayerScene::leaveMovieplayerScene() {
    if (!movieplayerViewer) return;
    auto &viewer = *movieplayerViewer;
    for (auto *clip : {&viewer.film1, &viewer.film2, &viewer.needles[0],
                       &viewer.needles[1], &viewer.needles[2], &viewer.reel}) {
        if (clip->sound != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(clip->sound);
            clip->sound = LiveAudioMixer::kInvalidEffectHandle;
        }
    }
    if (const auto profile = viewerProfilePath()) {
        const std::uint32_t saved =
            (static_cast<std::uint32_t>(viewer.soundIndex) << 16U) |
            (static_cast<std::uint32_t>(viewer.filmIndex) & 0xffffU);
        setProfileSceneRecordU32(*profile, "movieplayer", 0x402U, 1U, saved);
    }
}

// Animation-finished event 0x110, 0x0046db90.
void MovieplayerScene::movieplayerFinished(MovieplayerClip &clip, double now) {
    (void)now;
    auto &viewer = *movieplayerViewer;
    clip.playing = false;
    clip.heldFrame = clip.media.videoFrames.empty()
        ? 0U : clip.media.videoFrames.size() - 1U;
    clip.sound = LiveAudioMixer::kInvalidEffectHandle;
    traceViewerFinished(clip.name);
    if (&clip == &viewer.reel) {
        if (viewer.film2.playing) removeMovieplayerClip(viewer.film2);
    } else if (&clip == &viewer.film1) {
        removeMovieplayerClip(viewer.film1);
    } else {
        for (auto &needle : viewer.needles)
            if (&clip == &needle) removeMovieplayerClip(needle);
    }
}

// Control event 0x116, 0x0046da90.
void MovieplayerScene::movieplayerControl(int id, double now) {
    traceViewerControl("movieplayer", id);
    auto &viewer = *movieplayerViewer;
    switch (id) {
    case kStopControl:
        stopMovieplayerReel(now);
        break;
    case kPlayControl:
        playMovieplayerReel(now);
        break;
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        // 0x0046dad1: GAME_GotoSceneByName("tvstation", 1).
        if (!navigationHistory.empty()) navigationHistory.pop_back();
        pendingSceneEntryMode = 1;
        pendingScene = std::pair{std::string("tvstation"),
                                 std::string("tvstation")};
        break;
    case kFilmTab:
    case kSoundTab:
        selectMovieplayerTab(id, now);
        break;
    default:
        if (id >= kReelControlBase && id < kReelControlBase + kFilmAwardCount)
            selectMovieplayerReel(id - kReelControlBase, now);
        break;
    }
    (void)viewer;
}

void MovieplayerScene::updateMovieplayer(double elapsed) {
    if (!movieplayerViewer) return;
    auto &viewer = *movieplayerViewer;
    if (!viewer.entered) {
        viewer.entered = true;
        if (viewer.carPartShown) traceViewerAdd("carpart", -1, -1, -10, 5U);
        loadMovieplayerClip(viewer.film1, "film1", kFilm1Position, true);
        loadMovieplayerClip(viewer.film2, "film2", kFilm2Position, true);
        viewer.film2.loop = true;  // descriptor 0x00509ef8 +0x4c = 1
        for (std::size_t index = 0; index < viewer.needles.size(); ++index)
            loadMovieplayerClip(viewer.needles[index],
                                "needle" + std::to_string(index + 1),
                                kNeedlePositions[index], true);
        selectMovieplayerTab(kFilmTab, elapsed);
        viewer.nextNeedleAt = elapsed + kNeedleFirst;
    }
    // Timer event 0x111 (0x0046db70): a random needle, rand() % 3. Once per
    // pass, advanced by one period (0x0040bf9c), so not `while`.
    if (elapsed >= viewer.nextNeedleAt) {
        const double due = viewer.nextNeedleAt;
        viewer.nextNeedleAt += kNeedlePeriod;
        auto &needle =
            viewer.needles[static_cast<std::size_t>(particleRandom(3))];
        playMovieplayerAnimation(needle, due);
    }
    std::array<MovieplayerClip *, 6> clips{
        &viewer.film1, &viewer.film2, &viewer.needles[0], &viewer.needles[1],
        &viewer.needles[2], &viewer.reel};
    for (auto *clip : clips) {
        if (clip->playing && clip->loop && clip->media.videoDuration > 0.0 &&
            elapsed >= clip->nextLoopAt) {
            // A looping Smacker player restarts its sound with its frames.
            clip->nextLoopAt += clip->media.videoDuration;
            if (!clip->media.audioSamples.empty()) {
                if (clip->sound != LiveAudioMixer::kInvalidEffectHandle)
                    audioMixer.removeEffect(clip->sound);
                clip->sound = audioMixer.addEffect(
                    clip->media, audioVolumes[0] * sceneAudioGains[0]);
            }
        }
        if (!clip->playing || clip->loop) continue;
        if (elapsed >= clip->startedAt + clip->duration)
            movieplayerFinished(*clip, clip->startedAt + clip->duration);
    }
}

void MovieplayerScene::renderMovieplayer(double elapsed) {
    if (!movieplayerViewer) return;
    auto &viewer = *movieplayerViewer;
    const auto draw = [&](MovieplayerClip &clip) {
        if (!clip.added || !clip.texture || clip.media.videoFrames.empty())
            return;
        std::size_t index = clip.heldFrame;
        if (clip.playing) {
            double time = elapsed - clip.startedAt;
            if (clip.loop && clip.media.videoDuration > 0.0)
                time = std::fmod(time, clip.media.videoDuration);
            index = frameIndexAt(clip.media, time, false);
        }
        index = std::min(index, clip.media.videoFrames.size() - 1U);
        const auto &frame = clip.media.videoFrames[index];
        uploadVideoFrame(clip.texture.get(), frame);
        if (clip.interlaced) {
            SDL_Rect area{clip.position.x, clip.position.y, frame.width,
                          frame.height * 2};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &area);
            for (int row = 0; row < frame.height; ++row) {
                SDL_Rect source{0, row, frame.width, 1};
                SDL_Rect line{clip.position.x, clip.position.y + row * 2,
                              frame.width, 1};
                SDL_RenderCopy(renderer, clip.texture.get(), &source, &line);
            }
            return;
        }
        SDL_Rect destination{clip.position.x, clip.position.y, frame.width,
                             frame.height};
        SDL_RenderCopy(renderer, clip.texture.get(), nullptr, &destination);
    };
    draw(viewer.reel);
    draw(viewer.film2);
    draw(viewer.film1);
    for (auto &needle : viewer.needles) draw(needle);
    viewer.widgets.render(
        renderer, mouseX, mouseY,
        viewer.widgets.eventIdAt(mouseX, mouseY,
                                 viewerStaticHotspotAt(mouseX, mouseY)),
        [&](const ViewerWidget &widget, const SDL_Rect &rect) {
            drawCenteredText(renderer, dialogFont, widget.label, rect);
        });
}

bool MovieplayerScene::beginMovieplayerInteraction(int x, int y) {
    if (!movieplayerViewer) return false;
    const std::uint32_t staticHotspot = viewerStaticHotspotAt(x, y);
    // Every press with a hotspot id reaches the hotspot handler 0x0046da60
    // (event 0x105) before the control layer sees it.
    if (const auto eventId =
            movieplayerViewer->widgets.eventIdAt(x, y, staticHotspot))
        traceViewerControl("movieplayer", static_cast<int>(eventId));
    if (const auto id = movieplayerViewer->widgets.press(
            x, y, staticHotspot)) {
        movieplayerControl(*id, sceneElapsedNow());
        return true;
    }
    // Hotspot events (0x105, 0x0046da60) fall through to the scene's own
    // collectibles: the car part is hotspot 5.
    return movieplayerViewer->widgets.armed();
}

bool MovieplayerScene::endMovieplayerInteraction(int x, int y) {
    if (!movieplayerViewer) return false;
    if (const auto id = movieplayerViewer->widgets.release(
            x, y, viewerStaticHotspotAt(x, y))) {
        movieplayerControl(*id, sceneElapsedNow());
        return true;
    }
    return false;
}

bool MovieplayerScene::executeMovieplayerDeveloperCommand(
    std::string_view command) {
    if (!movieplayerViewer) return false;
    const double now = sceneElapsedNow();
    if (command == "play") {
        movieplayerControl(kPlayControl, now);
    } else if (command == "stop") {
        movieplayerControl(kStopControl, now);
    } else if (command == "film") {
        movieplayerControl(kFilmTab, now);
    } else if (command == "sound") {
        movieplayerControl(kSoundTab, now);
    } else {
        return false;
    }
    return true;
}

} // namespace fgp
