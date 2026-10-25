#include "game/app_whackamole.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

constexpr std::size_t kInvalidClip =
    std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t kWhackamoleFeedbackClone = 0x0054FEA8U;

TexturePtr makeWhackTexture(SDL_Renderer *renderer,
                            const DecodedMedia &media) {
    if (media.videoFrames.empty()) {
        return {};
    }
    const auto &frame = media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!texture) {
        throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                 SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

bool surfaceAlphaContains(SDL_Surface *surface, const SDL_Rect &bounds,
                          int x, int y) {
    if (!surface || x < bounds.x || y < bounds.y ||
        x >= bounds.x + bounds.w || y >= bounds.y + bounds.h) {
        return false;
    }
    const int localX = x - bounds.x;
    const int localY = y - bounds.y;
    const auto *row = static_cast<const std::uint8_t *>(surface->pixels) +
                      localY * surface->pitch;
    std::uint32_t pixel = 0U;
    std::memcpy(&pixel, row + localX * surface->format->BytesPerPixel,
                surface->format->BytesPerPixel);
    std::uint8_t red = 0U;
    std::uint8_t green = 0U;
    std::uint8_t blue = 0U;
    std::uint8_t alpha = 0U;
    SDL_GetRGBA(pixel, surface->format, &red, &green, &blue, &alpha);
    return alpha != 0U;
}

bool frameAlphaContains(const VideoFrame &frame, int localX, int localY) {
    if (localX < 0 || localY < 0 || localX >= frame.width ||
        localY >= frame.height) {
        return false;
    }
    const std::size_t alpha =
        (static_cast<std::size_t>(localY) * frame.width + localX) * 4U + 3U;
    return alpha < frame.bgra.size() && frame.bgra[alpha] != 0U;
}

void makeBlackMatteTransparent(DecodedMedia &media) {
    // cursor.smk predates the green-keyed scene players and stores its matte
    // as palette black.  Keep this conversion local: several target clips use
    // legitimate black pixels in their artwork.
    for (auto &frame : media.videoFrames) {
        for (std::size_t pixel = 0; pixel + 3U < frame.bgra.size();
             pixel += 4U) {
            if (frame.bgra[pixel] == 0U && frame.bgra[pixel + 1U] == 0U &&
                frame.bgra[pixel + 2U] == 0U) {
                frame.bgra[pixel + 3U] = 0U;
            }
        }
    }
}

std::string_view whackamoleTargetName(bool squirrel) {
    return squirrel ? std::string_view{"squirrel"} :
                      std::string_view{"bird"};
}

void traceWhackamoleClipStart(
    ReferenceTrace &trace,
    const ExecutableGameData::AnimationDefinition &definition,
    std::size_t holeIndex, std::string_view target, std::string_view phase,
    int loopsRemaining, bool reverse) {
    // The original dynamic calls at 0x0044E87E, 0x0044E546,
    // 0x0044E59B, 0x0044E6B3 and 0x0044E6F3 pass (-1, -1, 10) to
    // SCENE_AddAnim. The descriptor supplies the actual render coordinates.
    trace.emit("animation_add", {
        {"asset", definition.name}, {"x", -1}, {"y", -1}, {"layer", 10},
        {"hotspot", definition.hotspot},
        {"conflict_mask", definition.conflictMask},
        {"descriptor", definition.address}});
    trace.emit("animation_start", {
        {"asset", definition.name}, {"x", -1}, {"y", -1}, {"layer", 10},
        {"hotspot", definition.hotspot}, {"loop", false},
        {"descriptor", definition.address}});
    trace.emit("diagnostic", {
        {"kind", "whackamole_target_phase"},
        {"target_kind", target}, {"phase", phase},
        {"hotspot", static_cast<int>(holeIndex + 1U)},
        {"asset", definition.name}, {"descriptor", definition.address},
        {"loops_remaining", loopsRemaining}, {"reverse", reverse}});
}

void traceWhackamoleClipStop(
    ReferenceTrace &trace,
    const ExecutableGameData::AnimationDefinition &definition,
    std::string_view reason) {
    const auto emitStop = [&] {
        trace.emit("animation_stop", {
            {"asset", definition.name}, {"reason", reason},
            {"descriptor", definition.address}});
    };
    const auto emitRemove = [&] {
        trace.emit("animation_remove", {
            {"asset", definition.name}, {"reason", "removed"},
            {"descriptor", definition.address}});
    };
    if (reason == "stopped") {
        // Click removal reaches 0x00412AA0 first; its RemoveAnim logpoint
        // precedes the nested StopAnim boundary in the original trace.
        emitRemove();
        emitStop();
    } else {
        emitStop();
        emitRemove();
    }
}

void traceWhackamoleChirpStart(
    ReferenceTrace &trace,
    const ExecutableGameData::AnimationDefinition &definition,
    std::size_t holeIndex) {
    // Bird waits call SCENE_PlayAnim at 0x0044E565/0x0044E6D2. Its
    // 0x00412B90 helper adds the invisible .s descriptor through
    // SCENE_AddAnim at 0x00412BB2 only when it is not already active.
    trace.emit("animation_add", {
        {"asset", definition.name}, {"x", definition.x},
        {"y", definition.y}, {"layer", definition.layer},
        {"hotspot", definition.hotspot},
        {"conflict_mask", definition.conflictMask},
        {"descriptor", definition.address}});
    trace.emit("diagnostic", {
        {"kind", "whackamole_chirp"}, {"phase", "start"},
        {"hotspot", static_cast<int>(holeIndex + 1U)},
        {"asset", definition.name}, {"descriptor", definition.address}});
}

void traceWhackamoleChirpStop(
    ReferenceTrace &trace,
    const ExecutableGameData::AnimationDefinition &definition,
    std::size_t holeIndex) {
    // Both the natural retreat path (0x0044E5A6-0x0044E5BE) and the
    // bird-hit paths (0x0044DB62-0x0044DB76 and
    // 0x0044DC0F-0x0044DC23) test the descriptor's active handle and
    // remove it through 0x00412AA0.
    trace.emit("animation_remove", {
        {"asset", definition.name}, {"reason", "removed"},
        {"descriptor", definition.address}});
    trace.emit("animation_stop", {
        {"asset", definition.name}, {"reason", "stopped"},
        {"descriptor", definition.address}});
    trace.emit("diagnostic", {
        {"kind", "whackamole_chirp"}, {"phase", "stop"},
        {"hotspot", static_cast<int>(holeIndex + 1U)},
        {"asset", definition.name}, {"descriptor", definition.address}});
}

void traceWhackamoleWrongFeedbackStart(ReferenceTrace &trace) {
    // The squirrel-hit branch at 0x0044DAFF passes "false" to 0x0044DCB0.
    // That wrapper reaches SCENE_PlayAnimClone at 0x00412C20; the allocator at
    // 0x00412C60 returns 0x0054FEA8 for the first free 0xA8-byte clone slot.
    trace.emit("animation_add", {
        {"asset", "false"}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0},
        {"descriptor", kWhackamoleFeedbackClone}});
    trace.emit("animation_start", {
        {"asset", "false"}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"loop", false},
        {"descriptor", kWhackamoleFeedbackClone}});
    trace.emit("diagnostic", {
        {"kind", "whackamole_feedback"}, {"phase", "start"},
        {"asset", "false"}, {"descriptor", kWhackamoleFeedbackClone}});
}

void traceWhackamoleWrongFeedbackStop(ReferenceTrace &trace) {
    trace.emit("animation_stop", {
        {"asset", "false"}, {"reason", "stopped"},
        {"descriptor", kWhackamoleFeedbackClone}});
    trace.emit("animation_remove", {
        {"asset", "false"}, {"reason", "removed"},
        {"descriptor", kWhackamoleFeedbackClone}});
    trace.emit("diagnostic", {
        {"kind", "whackamole_feedback"}, {"phase", "stop"},
        {"asset", "false"}, {"descriptor", kWhackamoleFeedbackClone}});
}

} // namespace

WhackamoleScene::WhackamoleGame WhackamoleScene::loadWhackamoleGame(Package &package) {
    WhackamoleGame game;
    game.levels = executableGameData.whackamoleLevels();
    game.clips.reserve(156);

    game.startSurface = loadGreenKeyedBmp(
        package.data(package.find("bitmap\\startbuttn.bmp")));
    game.startBounds.w = game.startSurface->w;
    game.startBounds.h = game.startSurface->h;
    game.startTexture = makeTexture(renderer, game.startSurface.get());
    SDL_SetTextureBlendMode(game.startTexture.get(), SDL_BLENDMODE_BLEND);

    const auto loadAudio = [&](std::string_view name) {
        return decodeSmacker(activeSceneAnimationData(std::string(name)),
                             false, true, true);
    };
    game.correctSound = loadAudio("true");
    game.wrongSound = decodeActiveSceneClip("false");
    game.wrongFeedbackTexture = makeWhackTexture(renderer, game.wrongSound);
    game.thankYouSounds[0] = loadAudio("thank you on bird language1");
    game.thankYouSounds[1] = loadAudio("thank you on bird language2");
    game.cursorMedia = decodeSmacker(activeSceneAnimationData("cursor"),
                                     true, false);
    makeBlackMatteTransparent(game.cursorMedia);
    game.cursorTexture = makeWhackTexture(renderer, game.cursorMedia);

    const auto addClip = [&](std::string name, std::uint32_t hotspot) {
        const auto &definitions =
            executableGameData.animationDefinitionsNamed(name);
        const auto definition = std::find_if(
            definitions.begin(), definitions.end(),
            [hotspot](const auto &candidate) {
                return candidate.hotspot == hotspot;
            });
        if (definition == definitions.end()) {
            throw std::runtime_error(
                "Missing executable Whack-a-Mole descriptor: " + name);
        }
        DecodedMedia media = decodeActiveSceneClip(name);
        game.clips.push_back(
            {*definition, std::move(media), {}});
        game.clips.back().texture =
            makeWhackTexture(renderer, game.clips.back().media);
        return game.clips.size() - 1U;
    };

    for (std::size_t index = 0; index < game.holes.size(); ++index) {
        auto &hole = game.holes[index];
        hole.birdForward.fill(kInvalidClip);
        hole.birdReverse.fill(kInvalidClip);
        hole.squirrel.fill(kInvalidClip);
        const bool house = index < 7U;
        const int number = house ? static_cast<int>(index + 1U)
                                 : static_cast<int>(index - 6U);
        const std::string prefix =
            std::string("bird ") + (house ? "house " : "nest ") +
            std::to_string(number);
        const std::string squirrelPrefix =
            std::string("squirrel ") + (house ? "house " : "nest ") +
            std::to_string(number);
        const auto packaged = [&](std::string stem) {
            if (package.findOptional("animation\\" + stem + ".smk")) {
                return stem;
            }
            if (package.findOptional("animation\\" + stem + ".lst.smk")) {
                return stem + ".lst";
            }
            throw std::runtime_error(
                "Missing Whack-a-Mole animation family: " + stem);
        };
        constexpr std::array<std::string_view, 5> forwardSuffixes{
            "a", "b1", "b2", "b3", "c"};
        constexpr std::array<std::string_view, 5> reverseSuffixes{
            "ar", "b1r", "b2r", "b3r", "cr"};
        for (std::size_t phase = 0; phase < forwardSuffixes.size(); ++phase) {
            hole.birdForward[phase] = addClip(
                packaged(prefix + std::string(forwardSuffixes[phase])),
                static_cast<std::uint32_t>(index + 1U));
            hole.birdReverse[phase] = addClip(
                packaged(prefix + std::string(reverseSuffixes[phase])),
                static_cast<std::uint32_t>(index + 1U));
        }
        constexpr std::array<std::string_view, 3> squirrelSuffixes{
            "a", "b1", "c"};
        for (std::size_t phase = 0; phase < squirrelSuffixes.size(); ++phase) {
            hole.squirrel[phase] = addClip(
                packaged(squirrelPrefix +
                         std::string(squirrelSuffixes[phase])),
                static_cast<std::uint32_t>(index + 1U));
        }
        const std::string chirpName = packaged(prefix + "b1.s");
        const auto &chirpDefinitions =
            executableGameData.animationDefinitionsNamed(chirpName);
        if (chirpDefinitions.size() != 1U) {
            throw std::runtime_error(
                "Missing unique executable Whack-a-Mole chirp descriptor: " +
                chirpName);
        }
        hole.chirpDefinition = chirpDefinitions.front();
        hole.chirp = loadAudio(chirpName);
    }

    std::cout << "Loaded executable-derived Whack-a-Mole: 18 level records, "
              << game.clips.size()
              << " placed target clips, 12 dynamic holes, and original cursor\n";
    return game;
}

int WhackamoleScene::whackamolePercent() const {
    if (!whackamoleGame || whackamoleGame->opportunities <= 0) {
        return 0;
    }
    // 0x0044DF10 computes this as an x87 float, but each integer consumer
    // calls 0x0048C214. That helper selects truncate-toward-zero before fistp.
    // Hits and opportunities are non-negative, so integer division is exact.
    return whackamoleGame->hits * 100 /
           whackamoleGame->opportunities;
}

void WhackamoleScene::startWhackamoleGame() {
    if (!whackamoleGame) {
        return;
    }
    auto &game = *whackamoleGame;
    game.level = 0;
    game.score = 0;
    beginWhackamoleLevel();
}

void WhackamoleScene::beginWhackamoleLevel() {
    if (!whackamoleGame) {
        return;
    }
    auto &game = *whackamoleGame;
    ++game.level;
    const auto &definition = game.levels[
        static_cast<std::size_t>((game.level - 1) % game.levels.size())];
    std::uint32_t duration = definition.durationMilliseconds;
    if (game.level > static_cast<int>(game.levels.size())) {
        duration /= 2U;
        duration -= duration % 5000U;
    }
    game.phase = WhackamolePhase::Playing;
    game.levelStarted = sceneElapsedNow();
    game.levelDuration = duration / 1000.0;
    game.birdInterval =
        game.levelDuration / static_cast<double>(definition.birds + 1U);
    game.squirrelInterval =
        game.levelDuration / static_cast<double>(definition.squirrels + 1U);
    game.hits = 0;
    game.opportunities = 0;
    game.birdsSpawned = 0;
    game.squirrelsSpawned = 0;
    game.wrongFeedbackActive = false;
    for (auto &hole : game.holes) {
        audioMixer.removeEffect(hole.chirpHandle);
        hole.activeClip.reset();
        hole.kind = WhackamoleTargetKind::None;
        hole.accepted = false;
        hole.chirpActive = false;
        hole.chirpHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
    std::uniform_real_distribution<double> birdDelay(0.0,
                                                      game.birdInterval);
    std::uniform_real_distribution<double> squirrelDelay(
        0.0, game.squirrelInterval);
    game.nextBirdAt = game.levelStarted + birdDelay(randomEngine);
    game.nextSquirrelAt = game.levelStarted + squirrelDelay(randomEngine);
    SDL_SetWindowTitle(
        window,
        ("FGP Native — Fuglemating — level " +
         std::to_string(game.level)).c_str());
}

bool WhackamoleScene::spawnWhackamoleTarget(bool squirrel, double elapsed) {
    if (!whackamoleGame) {
        return false;
    }
    auto &game = *whackamoleGame;
    std::optional<std::size_t> selected;
    std::uniform_int_distribution<std::size_t> holeChoice(
        0U, game.holes.size() - 1U);
    for (int attempt = 0; attempt < 10; ++attempt) {
        const std::size_t candidate = holeChoice(randomEngine);
        if (!game.holes[candidate].activeClip) {
            selected = candidate;
            break;
        }
    }
    if (!selected) {
        return false;
    }
    auto &hole = game.holes[*selected];
    const auto &level = game.levels[
        static_cast<std::size_t>((game.level - 1) % game.levels.size())];
    std::uniform_int_distribution<int> loops(
        1, std::max(1, static_cast<int>(level.maximumLoops)));
    hole.loopsRemaining = loops(randomEngine);
    hole.kind = squirrel ? WhackamoleTargetKind::Squirrel
                         : WhackamoleTargetKind::Bird;
    hole.clipPhase = WhackamoleClipPhase::Appear;
    hole.reverse = !squirrel &&
                   std::uniform_int_distribution<int>(0, 999)(randomEngine) >=
                       500;
    hole.accepted = false;
    hole.clipStarted = elapsed;
    hole.activeClip = squirrel
        ? hole.squirrel[0]
        : (hole.reverse ? hole.birdReverse[0] : hole.birdForward[0]);
    traceWhackamoleClipStart(
        referenceTrace, game.clips[*hole.activeClip].definition, *selected,
        whackamoleTargetName(squirrel), "appear", hole.loopsRemaining,
        hole.reverse);
    if (!squirrel) {
        ++game.opportunities;
    }
    return true;
}

void WhackamoleScene::advanceWhackamoleHole(std::size_t index, double elapsed) {
    if (!whackamoleGame || index >= whackamoleGame->holes.size()) {
        return;
    }
    auto &game = *whackamoleGame;
    auto &hole = game.holes[index];
    if (!hole.activeClip) {
        return;
    }
    traceWhackamoleClipStop(
        referenceTrace, game.clips[*hole.activeClip].definition, "completed");
    if (hole.clipPhase == WhackamoleClipPhase::Retreat) {
        hole.activeClip.reset();
        hole.kind = WhackamoleTargetKind::None;
        return;
    }
    bool startChirp = false;
    bool stopChirp = false;
    if (hole.loopsRemaining > 0) {
        --hole.loopsRemaining;
        hole.clipPhase = WhackamoleClipPhase::Beg;
        if (hole.kind == WhackamoleTargetKind::Squirrel) {
            hole.activeClip = hole.squirrel[1];
            audioMixer.setEffect(game.clips[*hole.activeClip].media,
                                 audioVolumes[0] * sceneAudioGains[0]);
        } else {
            const int variation =
                std::uniform_int_distribution<int>(1, 3)(randomEngine);
            hole.activeClip = hole.reverse
                ? hole.birdReverse[static_cast<std::size_t>(variation)]
                : hole.birdForward[static_cast<std::size_t>(variation)];
            startChirp = !hole.chirpActive;
            if (startChirp) {
                // Each original b1.s descriptor owns an independent player.
                // Start it once when the hole enters its wait phase; repeated
                // b2/b3 animation loops do not restart the audio descriptor.
                hole.chirpHandle = audioMixer.addEffect(
                    hole.chirp, audioVolumes[0] * sceneAudioGains[0]);
            }
        }
    } else {
        hole.clipPhase = WhackamoleClipPhase::Retreat;
        hole.activeClip = hole.kind == WhackamoleTargetKind::Squirrel
            ? hole.squirrel[2]
            : (hole.reverse ? hole.birdReverse[4] : hole.birdForward[4]);
        stopChirp = hole.chirpActive;
    }
    hole.clipStarted = elapsed;
    traceWhackamoleClipStart(
        referenceTrace, game.clips[*hole.activeClip].definition, index,
        whackamoleTargetName(hole.kind == WhackamoleTargetKind::Squirrel),
        hole.clipPhase == WhackamoleClipPhase::Beg ? "wait" : "retreat",
        hole.loopsRemaining, hole.reverse);
    if (startChirp) {
        traceWhackamoleChirpStart(
            referenceTrace, hole.chirpDefinition, index);
        hole.chirpActive = true;
    } else if (stopChirp) {
        // Natural retreat adds its c clip before removing b1.s.
        traceWhackamoleChirpStop(
            referenceTrace, hole.chirpDefinition, index);
        audioMixer.removeEffect(hole.chirpHandle);
        hole.chirpHandle = LiveAudioMixer::kInvalidEffectHandle;
        hole.chirpActive = false;
    }
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

bool WhackamoleScene::pressWhackamoleHole(std::size_t index) {
    if (!whackamoleGame || index >= whackamoleGame->holes.size() ||
        whackamoleGame->phase != WhackamolePhase::Playing) {
        return false;
    }
    auto &game = *whackamoleGame;
    auto &hole = game.holes[index];
    if (!hole.activeClip || hole.accepted) {
        return false;
    }
    traceWhackamoleClipStop(
        referenceTrace, game.clips[*hole.activeClip].definition, "stopped");
    if (hole.kind == WhackamoleTargetKind::Bird && hole.chirpActive) {
        // The hit handler removes b1.s before adding the c clip.
        traceWhackamoleChirpStop(
            referenceTrace, hole.chirpDefinition, index);
        audioMixer.removeEffect(hole.chirpHandle);
        hole.chirpHandle = LiveAudioMixer::kInvalidEffectHandle;
        hole.chirpActive = false;
    }
    hole.accepted = true;
    hole.clipPhase = WhackamoleClipPhase::Retreat;
    hole.clipStarted = sceneElapsedNow();
    const bool squirrelHit =
        hole.kind == WhackamoleTargetKind::Squirrel;
    if (squirrelHit) {
        hole.activeClip = hole.squirrel[2];
        ++game.opportunities;
    } else {
        hole.activeClip = hole.reverse ? hole.birdReverse[4]
                                       : hole.birdForward[4];
        ++game.hits;
        game.score += 13;
    }
    traceWhackamoleClipStart(
        referenceTrace, game.clips[*hole.activeClip].definition, index,
        whackamoleTargetName(squirrelHit),
        "retreat", hole.loopsRemaining, hole.reverse);
    if (squirrelHit) {
        // The original starts the c retreat at 0x0044DAE5-0x0044DAF7,
        // then dispatches the cloned "false" feedback at 0x0044DAFF.
        // The clone player created through 0x00412C20 has conflict mask 0.
        // A captured squirrel click left an unrelated bird b1.s handle alive
        // while "false" played on a second handle, so this must not replace
        // the currently active chirp.
        audioMixer.addEffect(game.wrongSound,
                             audioVolumes[0] * sceneAudioGains[0]);
        game.wrongFeedbackStarted = sceneElapsedNow();
        game.wrongFeedbackActive = true;
        traceWhackamoleWrongFeedbackStart(referenceTrace);
    } else {
        // 0x0044DB9E selects language1 for forward birds; the reversed
        // branch at 0x0044DC4B selects language2, after starting c. A captured
        // hit played this feedback on a second handle while another hole's
        // b1.s chirp remained open, so it is an additive one-shot too.
        audioMixer.addEffect(game.thankYouSounds[hole.reverse ? 1U : 0U],
                             audioVolumes[0] * sceneAudioGains[0]);
    }
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    return true;
}

void WhackamoleScene::updateWhackamole(double elapsed) {
    if (!whackamoleGame) {
        return;
    }
    auto &game = *whackamoleGame;
    if (game.wrongFeedbackActive &&
        elapsed - game.wrongFeedbackStarted >= game.wrongSound.videoDuration) {
        traceWhackamoleWrongFeedbackStop(referenceTrace);
        game.wrongFeedbackActive = false;
    }
    if (game.phase != WhackamolePhase::Playing) {
        return;
    }
    if (elapsed >= game.levelStarted + game.levelDuration) {
        for (std::size_t index = 0; index < game.holes.size(); ++index) {
            auto &hole = game.holes[index];
            if (hole.activeClip) {
                traceWhackamoleClipStop(
                    referenceTrace, game.clips[*hole.activeClip].definition,
                    "stopped");
            }
            if (hole.chirpActive) {
                traceWhackamoleChirpStop(
                    referenceTrace, hole.chirpDefinition, index);
                audioMixer.removeEffect(hole.chirpHandle);
                hole.chirpHandle = LiveAudioMixer::kInvalidEffectHandle;
                hole.chirpActive = false;
            }
            hole.activeClip.reset();
            hole.kind = WhackamoleTargetKind::None;
        }
        const int percent = whackamolePercent();
        game.phaseStarted = elapsed;
        if (percent >= 70) {
            game.score += (percent - 70) * 29;
            game.phase = WhackamolePhase::LevelComplete;
            // 0x0044EB7D-0x0044EBA0 supplies WELLDONE as the title and
            // NEXTLEVEL as the body. 0x0044EBD5 starts the next level only
            // after the blocking modal has returned.
            showMessageWithTitle(
                language.get("whackamole", "WELLDONE", "Gratulerer!"),
                language.get("whackamole", "NEXTLEVEL",
                             "Klar for neste niv\xe5?"),
                MessageKind::WhackamoleNextLevel);
        } else {
            game.phase = WhackamolePhase::GameOver;
            // 0x0044EBF4-0x0044EC12 shows the titled GAMEOVER modal. Once
            // it returns, 0x0044EC17-0x0044EC2B opens the shared score table;
            // only after that returns does 0x0044EC37 add startbuttn.
            showMessageWithTitle(
                language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
                language.get("whackamole", "GAMEOVER", "Spillet er over."),
                MessageKind::WhackamoleGameOver);
        }
        return;
    }

    for (std::size_t index = 0; index < game.holes.size(); ++index) {
        auto &hole = game.holes[index];
        if (!hole.activeClip) {
            continue;
        }
        const auto &clip = game.clips[*hole.activeClip];
        if (elapsed - hole.clipStarted >= clip.media.videoDuration) {
            advanceWhackamoleHole(index, elapsed);
        }
    }

    const auto &level = game.levels[
        static_cast<std::size_t>((game.level - 1) % game.levels.size())];
    if (game.birdsSpawned < static_cast<int>(level.birds) &&
        elapsed >= game.nextBirdAt &&
        spawnWhackamoleTarget(false, elapsed)) {
        ++game.birdsSpawned;
        std::uniform_real_distribution<double> jitter(0.0,
                                                       game.birdInterval);
        game.nextBirdAt = game.levelStarted +
                          game.birdInterval * game.birdsSpawned +
                          jitter(randomEngine);
    }
    if (game.squirrelsSpawned < static_cast<int>(level.squirrels) &&
        elapsed >= game.nextSquirrelAt &&
        spawnWhackamoleTarget(true, elapsed)) {
        ++game.squirrelsSpawned;
        std::uniform_real_distribution<double> jitter(
            0.0, game.squirrelInterval);
        game.nextSquirrelAt = game.levelStarted +
                              game.squirrelInterval * game.squirrelsSpawned +
                              jitter(randomEngine);
    }
}

void WhackamoleScene::renderWhackamole(double elapsed) {
    if (!whackamoleGame) {
        return;
    }
    auto &game = *whackamoleGame;
    for (auto &hole : game.holes) {
        if (!hole.activeClip) {
            continue;
        }
        auto &clip = game.clips[*hole.activeClip];
        if (!clip.texture || clip.media.videoFrames.empty()) {
            continue;
        }
        const auto frameIndex = frameIndexAt(
            clip.media, elapsed - hole.clipStarted, false);
        const auto &frame = clip.media.videoFrames[frameIndex];
        uploadVideoFrame(clip.texture.get(), frame);
        const SDL_Rect destination{clip.definition.x, clip.definition.y,
                                   frame.width, frame.height};
        SDL_RenderCopy(renderer, clip.texture.get(), nullptr, &destination);
    }

    if (game.wrongFeedbackActive && game.wrongFeedbackTexture &&
        !game.wrongSound.videoFrames.empty()) {
        const auto frameIndex = frameIndexAt(
            game.wrongSound, elapsed - game.wrongFeedbackStarted, false);
        const auto &frame = game.wrongSound.videoFrames[frameIndex];
        uploadVideoFrame(game.wrongFeedbackTexture.get(), frame);
        const SDL_Rect destination{0, 0, frame.width, frame.height};
        SDL_RenderCopy(renderer, game.wrongFeedbackTexture.get(), nullptr,
                       &destination);
    }

    if (game.phase == WhackamolePhase::Attract) {
        SDL_RenderCopy(renderer, game.startTexture.get(), nullptr,
                       &game.startBounds);
        return;
    }

    const int percent = whackamolePercent();
    const double remaining = game.phase == WhackamolePhase::Playing
        ? std::max(0.0, game.levelDuration - (elapsed - game.levelStarted))
        : 0.0;
    const int seconds = static_cast<int>(std::ceil(remaining));
    const auto labelled = [&](std::string_view key, std::string_view fallback,
                              int value) {
        return language.get("whackamole", key, std::string(fallback)) + ": " +
               std::to_string(value);
    };
    const auto drawHudText = [&](std::string_view text,
                                 const SDL_Rect &bounds) {
        drawText(renderer, calendarFont, text, bounds.x,
                 bounds.y + (bounds.h - calendarFont.height) / 2);
    };
    drawHudText(labelled("POINTS", "Poeng", game.score),
                SDL_Rect{618, 175, 135, 29});
    drawHudText(labelled("PERCENT", "Prosent", percent),
                SDL_Rect{619, 224, 135, 24});
    drawHudText(labelled("LEVEL", "Niv\xe5", std::max(1, game.level)),
                SDL_Rect{622, 271, 92, 17});
    const std::string time =
        language.get("whackamole", "TIME", "Tid") + ": " +
        (seconds < 600 ? "0" : "") + std::to_string(seconds / 60) + ":" +
        (seconds % 60 < 10 ? "0" : "") +
        std::to_string(seconds % 60);
    drawHudText(time, SDL_Rect{620, 297, 97, 24});

    if (game.phase == WhackamolePhase::Attract ||
        (game.phase == WhackamolePhase::GameOver &&
         dialogMode == DialogMode::None)) {
        SDL_RenderCopy(renderer, game.startTexture.get(), nullptr,
                       &game.startBounds);
    }
}

void WhackamoleScene::renderWhackamoleCursor(double elapsed) {
    if (!whackamoleGame ||
        whackamoleGame->phase != WhackamolePhase::Playing ||
        !whackamoleGame->cursorTexture ||
        whackamoleGame->cursorMedia.videoFrames.empty()) {
        return;
    }
    auto &game = *whackamoleGame;
    const auto frameIndex = frameIndexAt(game.cursorMedia, elapsed, true);
    const auto &frame = game.cursorMedia.videoFrames[frameIndex];
    uploadVideoFrame(game.cursorTexture.get(), frame);
    const SDL_Rect destination{mouseX - 16, mouseY - 25,
                               frame.width, frame.height};
    SDL_RenderCopy(renderer, game.cursorTexture.get(), nullptr, &destination);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
WhackamoleScene::whackamoleHotspotAt(int x, int y) const {
    if (!whackamoleGame || activeScene != "whackamole") {
        return std::nullopt;
    }
    const auto &game = *whackamoleGame;
    if (game.phase != WhackamolePhase::Playing) {
        if (surfaceAlphaContains(game.startSurface.get(), game.startBounds,
                                 x, y)) {
            return std::pair{34U, game.startBounds};
        }
        return std::nullopt;
    }
    const double elapsed =
        const_cast<WhackamoleScene *>(this)->sceneElapsedNow();
    for (std::size_t reverse = game.holes.size(); reverse > 0; --reverse) {
        const auto &hole = game.holes[reverse - 1U];
        if (!hole.activeClip) {
            continue;
        }
        const auto &clip = game.clips[*hole.activeClip];
        if (clip.media.videoFrames.empty()) {
            continue;
        }
        const auto frameIndex = frameIndexAt(
            clip.media, elapsed - hole.clipStarted, false);
        const auto &frame = clip.media.videoFrames[frameIndex];
        if (frameAlphaContains(frame, x - clip.definition.x,
                               y - clip.definition.y)) {
            return std::pair{
                static_cast<std::uint32_t>(reverse),
                SDL_Rect{clip.definition.x, clip.definition.y,
                         frame.width, frame.height}};
        }
    }
    return std::nullopt;
}

bool WhackamoleScene::executeWhackamoleDeveloperCommand(std::string_view command) {
    constexpr std::string_view whackamoleControlPrefix = "whackamole:";
    if (activeScene != "whackamole" || !whackamoleGame) {
        return false;
    }
    const std::string_view control =
        command.substr(whackamoleControlPrefix.size());
    const double now = sceneElapsedNow();
    if (control == "start") {
        startWhackamoleGame();
    } else if (control == "hit" || control == "squirrel" ||
               control == "miss") {
        if (whackamoleGame->phase != WhackamolePhase::Playing) {
            startWhackamoleGame();
        }
        const bool squirrel = control == "squirrel";
        if (!spawnWhackamoleTarget(squirrel, now)) {
            return false;
        }
        const auto found = std::find_if(
            whackamoleGame->holes.begin(),
            whackamoleGame->holes.end(),
            [squirrel](const WhackamoleHole &hole) {
                return hole.activeClip &&
                       hole.kind ==
                           (squirrel
                                ? WhackamoleTargetKind::Squirrel
                                : WhackamoleTargetKind::Bird);
            });
        if (found == whackamoleGame->holes.end()) {
            return false;
        }
        const auto index = static_cast<std::size_t>(
            std::distance(whackamoleGame->holes.begin(), found));
        if (control == "miss") {
            auto &hole = whackamoleGame->holes[index];
            hole.activeClip.reset();
            hole.kind = WhackamoleTargetKind::None;
        } else {
            pressWhackamoleHole(index);
        }
    } else if (control == "await-idle") {
        // Holds the command queue until the running game has ended, so a
        // later set-score reaches the next game, not this one.
        return whackamoleGame->phase != WhackamolePhase::Playing;
    } else if (control.rfind("set-score=", 0) == 0) {
        // Keep retrying the runner command until Start has initialized
        // the round; startWhackamoleGame() resets score to zero.
        if (whackamoleGame->phase != WhackamolePhase::Playing) {
            return false;
        }
        const auto parsed = std::stoul(
            std::string(control.substr(std::string_view("set-score=").size())));
        if (parsed > static_cast<unsigned long>(
                         std::numeric_limits<int>::max())) {
            throw std::runtime_error(
                "Developer Whack-a-Mole score is out of range");
        }
        whackamoleGame->score = static_cast<int>(parsed);
    } else if (control == "complete-level") {
        if (whackamoleGame->phase != WhackamolePhase::Playing) {
            startWhackamoleGame();
        }
        whackamoleGame->opportunities =
            std::max(1, whackamoleGame->opportunities);
        whackamoleGame->hits = whackamoleGame->opportunities;
        whackamoleGame->levelStarted =
            now - whackamoleGame->levelDuration;
    } else if (control == "timeout") {
        if (whackamoleGame->phase != WhackamolePhase::Playing) {
            startWhackamoleGame();
        }
        whackamoleGame->hits = 0;
        whackamoleGame->opportunities =
            std::max(1, whackamoleGame->opportunities);
        whackamoleGame->levelStarted =
            now - whackamoleGame->levelDuration;
    } else if (control == "status") {
        std::cout << "Whack-a-Mole status: level "
                  << whackamoleGame->level << ", "
                  << whackamoleGame->score << " points, "
                  << whackamolePercent() << "%, "
                  << whackamoleGame->hits << '/'
                  << whackamoleGame->opportunities
                  << " correct, birds "
                  << whackamoleGame->birdsSpawned << ", squirrels "
                  << whackamoleGame->squirrelsSpawned << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Whack-a-Mole control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
