#include "game/app.h"

#include <algorithm>
#include <cctype>
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
#include <utility>

#include "assets/profile.h"
#include "game/menu_names.h"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

void AppCore::setMenuTitle(std::string_view message) {
    std::string title = "FGP Native — Main menu — Profile: " + profiles[profileIndex];
    if (!message.empty()) {
        title += " — ";
        title += message;
    }
    title += " — H hotspots · Esc quit";
    SDL_SetWindowTitle(window, title.c_str());
}

void AppCore::resetAnimationClock() {
    start = std::chrono::steady_clock::now();
    pausedAt = 0.0;
    currentFrame = 0;
    paused = false;
}

void GameApp::enterMenu(bool restartMusic) {
    const std::string previousScene = activeScene;
    saveSynonymRanks();
    saveTriviaRanks();
    saveRacemenu();
    racemenuGame.reset();
    buttercolAlbum.reset();
    invalidateUploadedVideoFrames();
    // Returning to the menu leaves the scene, so its narration ends with it.
    stopSceneNarration();
    stopSceneAmbience();
    const double preservedMusicPosition =
        restartMusic ? 0.0 : audioMixer.backgroundSeconds();
    viewMode = ViewMode::Menu;
    activeScene = "menu";
    sceneHotspotTexture.reset();
    sceneTitleTexture.reset();
    sceneTitleWidth = 0;
    sceneTitleHeight = 0;
    sceneBackdropTexture.reset();
    sceneIndexTiles.clear();
    sceneIndexTileBounds.clear();
    for (auto &texture : sceneIndexTabTextures) {
        texture.reset();
    }
    sceneIndexGoldFrameTexture.reset();
    sceneClips.clear();
    sceneHeldStageIndex.reset();
    sceneCharacterGroups.clear();
    sceneAnimationObjects.clear();
    recoveredMinigameRuntime.reset();
    whackamoleGame.reset();
    balloonhuntGame.reset();
    pipelineGame.reset();
    hopscotchGame.reset();
    beemazeGame.reset();
    bugzzzGame.reset();
    wheelbarrowGame.reset();
    buildabikeGame.reset();
    mountainGame.reset();
    hustleGame.reset();
    jigsawGame.reset();
    slidingGame.reset();
    draughtsGame.reset();
    resetViewerScenes();
    textinvaderGame.reset();
    synonymGame.reset();
    mathlabGame.reset();
    mainscoreBoard.reset();
    tournamentScreen.reset();
    triviaGame.reset();
    peeParticleField.reset();
    sceneBitmapLayers.clear();
    sceneCornerButtons.clear();
    pressedSceneCornerButton.reset();
    pressedMenuCornerButton.reset();
    closeStoryNavigation();
    storyNavigationAvailable = false;
    storyPreviousScene.clear();
    storyNextScene.clear();
    activeSceneEntryMode = 0;
    pendingSceneEntryMode = 0;
    navigationHistory.clear();
    sceneCollectibles.clear();
    sceneAudioTimeline = {};
    sceneEffectTimeline = {};
    sceneBackgroundMusic = {};
    // The menu is an ordinary scene to 0x0040ccd0, so its own sound.ini
    // (music 0.15) scales the theme like any scene's.
    sceneAudioGains = readSceneAudioGains(menu);
    sceneBackgroundMusicAsset.clear();
    sceneUsesAudioTimeline = false;
    sceneSequenceBlocksInput = false;
    sceneSequenceFinished = false;
    sceneClipIndex = 0;
    sceneClipTimelineStart = 0.0;
    sceneClipStartOffsets.clear();
    sceneSequenceRequestedAt = 0.0;
    sceneClipStarted = false;
    sceneSequenceHeldConflictMask = 0;
    sceneTimedTransition.reset();
    sceneIndexNames.clear();
    sceneIndexUnlocked.clear();
    sceneIndexCursorKinds.clear();
    sceneHotspots.reset();
    sceneBackdrop.reset();
    scenePackage.reset();
    sceneEditionPackage.reset();
    sceneData2Package.reset();
    sceneAddonPackage.reset();
    sceneLanguagePackage.reset();
    sceneIsMinigameShell = false;
    hoveredHotspot = pixelValue(hotspots.get(), mouseX, mouseY);
    hoveredBounds = hotspotBounds(hotspots.get(), hoveredHotspot);
    activeAmpLevel = -1;
    fuseActive = false;
    resetAnimationClock();
    audioMixer.setBackground(music, audioVolumes[2] * sceneAudioGains[2],
                             preservedMusicPosition);
    audioMixer.clearForeground();
    SDL_PauseAudioDevice(audioDevice, musicEnabled ? 0 : 1);
    setMenuTitle();
    if (previousScene != "menu") {
        if (!std::exchange(menuSceneChangeTraced, false)) {
            referenceTrace.emit("scene_change",
                                {{"old_scene", previousScene},
                                 {"scene", "menu"},
                                 {"entry_mode", 0}});
        }
        sceneReadyAfterPresent = "menu";
    }
    leaveSceneMusicDuck(previousScene);
    if (creditsGame) closeCreditsGame();
#ifdef __EMSCRIPTEN__
    // Browser build only: scores go to a shared leaderboard under the profile
    // name, and a phone visitor would otherwise never open the new-player
    // dialog, so it opens once when the menu first appears without a profile.
    if (!askedForWebProfile && dialogMode == DialogMode::None &&
        profiles.size() == 1 && profiles.front() == "No profile") {
        askedForWebProfile = true;
        openNameDialog();
    }
#endif
}

namespace {

bool sameCodeIgnoringCase(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

// 0x00422100: an entry's code is the text before its ':' (none without one).
std::string countryCode(const std::vector<std::string> &countries, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= countries.size()) {
        return {};
    }
    const std::string &entry = countries[static_cast<std::size_t>(index)];
    const auto colon = entry.find(':');
    return colon == std::string::npos ? std::string{} : entry.substr(0, colon);
}

} // namespace

std::vector<std::string> AppCore::optionsCountries() const {
    // A NUL-separated list ending at an empty entry (0x00422840 counts it,
    // 0x00422140 walks it); language.ini writes the NULs as \0.
    const std::string list = language.get("sounddlg", "COUNTRIES");
    std::vector<std::string> countries;
    std::size_t position = 0;
    while (position < list.size() && list[position] != '\0') {
        const auto end = list.find('\0', position);
        countries.push_back(list.substr(position, end - position));
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (countries.empty()) {
        // Native fallback for a language table without the list; every
        // edition here carries this one.
        countries = {"DK:Dansk", "UK:Engelsk", "NO:Norsk", "SE:Svensk"};
    }
    return countries;
}

int AppCore::optionsCountryIndex(std::string_view code, int fallback) const {
    // 0x004227b0 compares only up to a ';' and returns the fallback for an
    // empty code.
    code = code.substr(0, code.find(';'));
    if (code.empty()) {
        return fallback;
    }
    const auto countries = optionsCountries();
    for (std::size_t index = 0; index < countries.size(); ++index) {
        // _strnicmp(entry, code, strlen(code)) (0x0048d830 at 0x004227fd).
        if (countries[index].size() >= code.size() &&
            sameCodeIgnoringCase(
                std::string_view(countries[index]).substr(0, code.size()),
                code)) {
            return static_cast<int>(index);
        }
    }
    return fallback;
}

float AppCore::optionsSliderValue(int value) {
    // fild, fmul dword [0x0049c90c] (0.01f), fstp dword: the product of an
    // integer up to 100 and the float is exact in a double, so this is the
    // same single rounding.
    return static_cast<float>(static_cast<double>(value) *
                              static_cast<double>(0.01F));
}

void AppCore::loadProfileOptions(bool applyToMusic) {
    try {
        profileOptionValues = profileOptions(activeProfilePath());
    } catch (const std::exception &error) {
        // A profile 0x00411ef0 cannot load leaves none loaded, so the
        // defaults apply.
        std::cout << "Could not read the profile's options: " << error.what()
                  << '\n';
        profileOptionValues = ProfileOptions{};
    }
    // 0x0040b4e0, 0x0040b4ee, 0x0040b4fc: each slider through 0x0040b410,
    // which applies it at once (0x0040b430).
    audioVolumes = profileOptionValues.volumes;
    if (applyToMusic) {
        audioMixer.setBackgroundVolume(audioVolumes[2] * sceneAudioGains[2]);
    }
}

void AppCore::enterOptions() {
    // 0x00418140 selects the profile again first (0x0041815b), so the
    // dialog starts from what the profile holds. It hands 0x00422a00 the
    // subtitle flag and "<subtitle code>[;<second code>]"; 0x00422650 turns
    // the codes into list indices, defaulting to sounddlg:COUNTRYDEF.
    loadProfileOptions();
    const int fallback =
        optionsCountryIndex(language.get("sounddlg", "COUNTRYDEF"), 0);
    subtitleLanguage =
        optionsCountryIndex(profileOptionValues.subtitleLanguage, fallback);
    subtitlesEnabled = profileOptionValues.subtitlesEnabled != 0U;
    savedAudioVolumes = audioVolumes;
    savedSubtitleLanguage = subtitleLanguage;
    savedSubtitlesEnabled = subtitlesEnabled;
    optionsPressedControl = OptionsControl::None;
    optionsDraggingChannel = -1;
    viewMode = ViewMode::Options;
    activeScene = "options";
    hoveredHotspot = 0;
    hoveredBounds.reset();
    SDL_SetWindowTitle(window, "FGP Native — Innstillinger");
}

void AppCore::leaveOptions(bool accept) {
    // 0x00422a00 removes the preview players when the dialog closes.
    stopOptionsPreview(0);
    stopOptionsPreview(1);
    if (!accept) {
        audioVolumes = savedAudioVolumes;
        subtitleLanguage = savedSubtitleLanguage;
        subtitlesEnabled = savedSubtitlesEnabled;
    } else {
        saveProfileOptions();
    }
    optionsPressedControl = OptionsControl::None;
    optionsDraggingChannel = -1;
    enterMenu(false);
}

void AppCore::saveProfileOptions() {
    // OK: 0x00422080 returns the flag and "<code>;<code>" for the two
    // indices; 0x00418140 splits it at ';', stores the flag (0x004095c0)
    // and the subtitle code (0x00409560), and saves (0x004182aa ->
    // 0x0040b5a0, the sliders as they stand). Cancel saves nothing.
    const auto countries = optionsCountries();
    const int fallback =
        optionsCountryIndex(language.get("sounddlg", "COUNTRYDEF"), 0);
    const int secondIndex =
        optionsCountryIndex(profileOptionValues.secondLanguage, fallback);
    profileOptionValues.volumes = audioVolumes;
    profileOptionValues.subtitlesEnabled = subtitlesEnabled ? 1U : 0U;
    profileOptionValues.subtitleLanguage =
        countryCode(countries, subtitleLanguage).substr(0, 7);
    // 0x004845c0 compares the returned second code with the held one ("" as
    // "no"); only a different one is stored (0x00484580) and saved again
    // (0x004182ca), which leaves the same bytes as saving it once.
    const std::string second = countryCode(countries, secondIndex);
    const std::string held = profileOptionValues.secondLanguage.empty()
                                 ? std::string("no")
                                 : profileOptionValues.secondLanguage;
    if (!sameCodeIgnoringCase(held, second)) {
        profileOptionValues.secondLanguage = normalizedSecondLanguage(second);
    }
    // 0x004116c0 writes nothing when no profile is loaded.
    const auto path = activeProfilePath();
    if (!path) {
        return;
    }
    try {
        setProfileOptions(*path, profileOptionValues);
    } catch (const std::exception &error) {
        std::cout << "Could not save the options to the profile: "
                  << error.what() << '\n';
    }
}

double AppCore::animationTimeNow() {
    if (dialogClockFrozen()) {
        return helpFrozenElapsed;
    }
    if (paused) {
        return pausedAt;
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          start)
        .count();
}

void AppCore::startAmpEffect(int level) {
    // The original only promotes the current response: hover requests
    // level 1, while a mouse-down requests level 2 plus the fuse LEDs.
    if (level <= activeAmpLevel) {
        return;
    }
    const double eventTime = animationTimeNow();
    activeAmpLevel = level;
    ampEffectStart = eventTime;
    if (level == 2) {
        fuseActive = true;
        fuseEffectStart = eventTime;
        referenceTrace.emit("animation_start", {{"asset", "fuse"},
                                                 {"loop", false}});
    }
    referenceTrace.emit("animation_start", {{"asset", "amp"},
                                             {"level", level},
                                             {"loop", false}});
}

SDL_Point AppCore::optionsDialogPosition() {
    const int layoutHeight = hasSubtitleSettings ? 289 : 215;
    return SDL_Point{(kLogicalWidth - settingsBackdrop->w) / 2,
                     (kLogicalHeight - layoutHeight) / 2};
}

AppCore::OptionsControl AppCore::optionsControlAt(int x, int y) {
    const SDL_Point origin = optionsDialogPosition();
    const int localX = x - origin.x;
    const int localY = y - origin.y;
    static constexpr std::array<int, 3> rowY{53, 91, 130};
    static constexpr std::array<OptionsControl, 3> downControls{
        OptionsControl::EffectDown,
        OptionsControl::SpeechDown,
        OptionsControl::MusicDown};
    static constexpr std::array<OptionsControl, 3> trackControls{
        OptionsControl::EffectTrack,
        OptionsControl::SpeechTrack,
        OptionsControl::MusicTrack};
    static constexpr std::array<OptionsControl, 3> upControls{
        OptionsControl::EffectUp,
        OptionsControl::SpeechUp,
        OptionsControl::MusicUp};
    for (int channel = 0; channel < 3; ++channel) {
        if (localX >= 91 && localX < 91 + settingsDownNormal->w &&
            localY >= rowY[channel] &&
            localY < rowY[channel] + settingsDownNormal->h) {
            return downControls[channel];
        }
        if (localX >= 123 && localX < 286 &&
            localY >= rowY[channel] && localY < rowY[channel] + 21) {
            return trackControls[channel];
        }
        if (localX >= 300 && localX < 300 + settingsUpNormal->w &&
            localY >= rowY[channel] &&
            localY < rowY[channel] + settingsUpNormal->h) {
            return upControls[channel];
        }
    }
    if (hasSubtitleSettings) {
        if (localX >= 91 && localX < 91 + settingsDownNormal->w &&
            localY >= 168 && localY < 168 + settingsDownNormal->h) {
            return OptionsControl::SubtitlePrevious;
        }
        if (localX >= 300 && localX < 300 + settingsUpNormal->w &&
            localY >= 168 && localY < 168 + settingsUpNormal->h) {
            return OptionsControl::SubtitleNext;
        }
        if (localX >= 324 && localX < 324 + settingsToggleNormal->w &&
            localY >= 165 && localY < 165 + settingsToggleNormal->h) {
            return OptionsControl::SubtitleToggle;
        }
    }
    const int buttonY = hasSubtitleSettings ? 205 : 168;
    if (localX >= 197 && localX < 197 + settingsButtonNormal->w &&
        localY >= buttonY && localY < buttonY + settingsButtonNormal->h) {
        return OptionsControl::Ok;
    }
    if (localX >= 286 && localX < 286 + settingsButtonNormal->w &&
        localY >= buttonY && localY < buttonY + settingsButtonNormal->h) {
        return OptionsControl::Cancel;
    }
    return OptionsControl::None;
}

int AppCore::optionsChannelForControl(OptionsControl control) {
    switch (control) {
    case OptionsControl::EffectDown:
    case OptionsControl::EffectTrack:
    case OptionsControl::EffectUp: return 0;
    case OptionsControl::SpeechDown:
    case OptionsControl::SpeechTrack:
    case OptionsControl::SpeechUp: return 1;
    case OptionsControl::MusicDown:
    case OptionsControl::MusicTrack:
    case OptionsControl::MusicUp: return 2;
    default: return -1;
    }
}

bool AppCore::setOptionsSliderFromMouse(int channel, int x) {
    const SDL_Point origin = optionsDialogPosition();
    const int travel = 163 - settingsThumb->w;
    const float position = std::clamp(
        (x - origin.x - 123 - settingsThumb->w / 2) /
            static_cast<float>(std::max(1, travel)),
        0.0F, 1.0F);
    // The slider control holds an integer 0-100 (0x00408f30 rounds the
    // thumb position to it) and the dialog scales it by 0.01 (0x004230ae).
    const float next =
        optionsSliderValue(static_cast<int>(std::lround(position * 100.0F)));
    if (next == audioVolumes[channel]) {
        return false;
    }
    setOptionsVolume(channel, next);
    return true;
}

void AppCore::setOptionsVolume(int channel, float volume) {
    // 0x0040b410 stores the value and 0x0040b430 applies it at once to the
    // running music player or to every playing clip's track, which
    // includes the dialog's own preview sample.
    audioVolumes[channel] = volume;
    const float effective = audioVolumes[channel] * sceneAudioGains[channel];
    if (channel == 2) {
        audioMixer.setBackgroundVolume(effective);
        SDL_PauseAudioDevice(audioDevice,
                             musicEnabled && !paused ? 0 : 1);
    } else {
        audioMixer.setEffectVolume(optionsPreviewHandles[channel], effective);
    }
}

void AppCore::previewOptionsChannel(int channel) {
    // The dialog callback 0x00422b80 plays a sample for the effect and
    // speech sliders on every value change and every drag movement, but
    // only when that sample is not already playing (0x00413900), so a
    // drag hears it from the start and then again each time it finishes.
    // The music slider has no sample: the running music is the preview.
    if (channel < 0 || channel > 1) {
        return;
    }
    if (audioMixer.effectPlaying(optionsPreviewHandles[channel])) {
        return;
    }
    if (!optionsPreviewLoaded[channel]) {
        optionsPreviewLoaded[channel] = true;
        const char *asset =
            channel == 0 ? "sound\\effect.smk" : "sound\\speech.smk";
        try {
            // effect.smk carries audio track 0 and speech.smk track 1, so
            // the players' effect and speech volumes apply (0x00403068,
            // 0x00403070).
            optionsPreviewMedia[channel] = decodeSmacker(
                common.data(common.find(asset)), false, true, true);
        } catch (const std::exception &error) {
            std::cerr << "Could not decode " << asset << ": "
                      << error.what() << '\n';
        }
    }
    if (optionsPreviewMedia[channel].audioSamples.empty()) {
        return;
    }
    optionsPreviewHandles[channel] = audioMixer.addEffect(
        optionsPreviewMedia[channel],
        audioVolumes[channel] * sceneAudioGains[channel]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void AppCore::stopOptionsPreview(int channel) {
    if (channel < 0 || channel > 1) {
        return;
    }
    audioMixer.removeEffect(optionsPreviewHandles[channel]);
    optionsPreviewHandles[channel] = LiveAudioMixer::kInvalidEffectHandle;
}

float AppCore::clipAudioVolume(const DecodedMedia &media) const {
    const std::size_t channel = media.smackerAudioTrack == 0 ? 0U : 1U;
    return audioVolumes[channel] * sceneAudioGains[channel];
}

std::array<float, 3> AppCore::readSceneAudioGains(const Package &package) {
    std::array<float, 3> gains{1.0F, 1.0F, 1.0F};
    const auto *soundEntry = package.findOptional("sound.ini");
    if (!soundEntry) {
        return gains;
    }
    const LanguageTable soundSettings(package.data(*soundEntry));
    const std::array<std::string_view, 3> keys{"sound", "speech", "music"};
    for (std::size_t channel = 0; channel < keys.size(); ++channel) {
        try {
            gains[channel] = std::clamp(
                std::stof(soundSettings.get("Volume", keys[channel], "1.0")),
                0.0F, 1.0F);
        } catch (const std::exception &) {
            gains[channel] = 1.0F;
        }
    }
    return gains;
}
} // namespace fgp
