#include "game/app_audiopairs.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "media/smacker.h"

namespace fgp {
namespace {

const auto kButtonPositions = originalArray<SDL_Point, 20>(0x4cb720U);

// The two 14-entry coordinate tables at FGP.exe 0x0049d660/0x0049d6d0.
const auto kLeftPartPositions = originalArray<SDL_Point, 14>(0x0049d660U);
const auto kRightPartPositions = originalArray<SDL_Point, 14>(0x0049d6d0U);

constexpr std::array<std::string_view, 5> kSoundFamilies{
    "sound", "music", "birds", "farm", "horn"};

TexturePtr makeAnimationTexture(SDL_Renderer *renderer,
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

std::string numberedAsset(std::string_view stem, int number) {
    std::ostringstream result;
    result << stem << std::setw(2) << std::setfill('0') << number;
    return result.str();
}

} // namespace

AudiopairsScene::AudiopairsGame AudiopairsScene::loadAudiopairsGame(Package &package) {
    AudiopairsGame game;
    constexpr std::array<std::string_view, 4> colours{
        "green", "yellow", "red", "black"};

    for (std::size_t colour = 0; colour < colours.size(); ++colour) {
        for (std::size_t row = 0; row < 4; ++row) {
            const std::string asset =
                "bitmap\\" + std::string(colours[colour]) +
                std::to_string(row) + ".bmp";
            SurfacePtr surface =
                loadGreenKeyedBmp(package.data(package.find(asset)));
            game.buttonTextures[colour][row] =
                makeTexture(renderer, surface.get());
            SDL_SetTextureBlendMode(game.buttonTextures[colour][row].get(),
                                    SDL_BLENDMODE_BLEND);
            for (std::size_t column = 0; column < 5; ++column) {
                const std::size_t index = row * 5 + column;
                game.buttons[index].bounds = {
                    kButtonPositions[index].x, kButtonPositions[index].y,
                    surface->w, surface->h};
            }
        }
    }
    // Scene initialization calls 0x00428fd0 with state 0 at 0x00428d22.
    // The state switch at 0x0042902c maps 0 to the `green` bitmap.
    for (auto &button : game.buttons) {
        button.state = AudiopairsButtonState::Green;
    }

    auto loadParts = [&](std::string_view prefix,
                         const auto &positions, auto &parts) {
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const std::string asset =
                "bitmap\\" + numberedAsset(prefix, static_cast<int>(index + 1)) +
                ".bmp";
            SurfacePtr surface =
                loadGreenKeyedBmp(package.data(package.find(asset)));
            parts[index].bounds = {positions[index].x, positions[index].y,
                                   surface->w, surface->h};
            parts[index].texture = makeTexture(renderer, surface.get());
            SDL_SetTextureBlendMode(parts[index].texture.get(),
                                    SDL_BLENDMODE_BLEND);
        }
    };
    loadParts("v", kLeftPartPositions, game.leftParts);
    loadParts("h", kRightPartPositions, game.rightParts);
    // 0x00428d2f and 0x00428d3d call 0x00428e70 with count 14 for sides
    // 0 and 1, so both completed mechanisms frame the initial Start screen.
    game.leftPartCount = static_cast<int>(game.leftParts.size());
    game.rightPartCount = static_cast<int>(game.rightParts.size());

    SurfacePtr pig = loadGreenKeyedBmp(
        package.data(package.find("bitmap\\pig.bmp")));
    game.pigWidth = pig->w;
    game.pigHeight = pig->h;
    game.pigTexture = makeTexture(renderer, pig.get());
    SDL_SetTextureBlendMode(game.pigTexture.get(), SDL_BLENDMODE_BLEND);

    constexpr std::array<std::tuple<std::string_view, int, int>, 4>
        animationDefinitions{{
            {"start", 556, 128}, {"lever", 560, 115},
            {"left", 0, 84}, {"money", 234, 130},
        }};
    for (std::size_t index = 0; index < animationDefinitions.size(); ++index) {
        const auto &[name, x, y] = animationDefinitions[index];
        auto &animation = game.animations[index];
        animation.name = name;
        animation.x = x;
        animation.y = y;
        animation.media = decodeActiveSceneClip(std::string(name));
        animation.texture = makeAnimationTexture(renderer, animation.media);
    }
    for (std::size_t index = 0; index < game.partTones.size(); ++index) {
        game.partTones[index] = decodeSmacker(
            activeSceneAnimationData(numberedAsset(
                "part", static_cast<int>(index + 1))),
            false, true);
    }

    game.activeAnimation = 0U;
    std::cout << "Loaded executable-derived Audiopairs board: 20 buttons, "
                 "28 mechanism parts, five sound families\n";
    return game;
}

DecodedMedia &AudiopairsScene::audiopairsSound(int sound) {
    if (!audiopairsGame || sound < 0 || sound >= 10) {
        throw std::runtime_error("Invalid Audiopairs sound index");
    }
    auto &game = *audiopairsGame;
    const int family = game.level % static_cast<int>(kSoundFamilies.size());
    auto &cached = game.sounds[family * 10 + sound];
    if (!cached) {
        cached = decodeSmacker(
            activeSceneAnimationData(numberedAsset(kSoundFamilies[family],
                                                    sound + 1)),
            false, true);
    }
    return *cached;
}

void AudiopairsScene::dealAudiopairsBoard() {
    if (!audiopairsGame) {
        return;
    }
    auto &game = *audiopairsGame;
    game.activePairs = std::min(10, 3 + game.level);
    game.firstSelection.reset();
    game.secondSelection.reset();

    for (auto &button : game.buttons) {
        button.sound = -1;
        button.state = AudiopairsButtonState::Black;
    }
    std::array<int, 10> soundOrder{};
    std::iota(soundOrder.begin(), soundOrder.end(), 0);
    std::shuffle(soundOrder.begin(), soundOrder.end(), randomEngine);
    std::array<std::size_t, 20> buttonOrder{};
    std::iota(buttonOrder.begin(), buttonOrder.end(), 0U);
    std::shuffle(buttonOrder.begin(), buttonOrder.end(), randomEngine);
    for (int pair = 0; pair < game.activePairs; ++pair) {
        for (int copy = 0; copy < 2; ++copy) {
            auto &button = game.buttons[buttonOrder[pair * 2 + copy]];
            button.sound = soundOrder[pair];
            button.state = AudiopairsButtonState::Green;
        }
    }
    // The original package keeps each level's ten short clips ready. Decode
    // the current family during the deal so the first button does not hitch.
    for (int sound = 0; sound < 10; ++sound) {
        (void)audiopairsSound(sound);
    }
    game.phase = AudiopairsPhase::Playing;
}

void AudiopairsScene::startAudiopairsGame() {
    if (!audiopairsGame) {
        return;
    }
    auto &game = *audiopairsGame;
    game.attempts = 15;
    game.points = 0;
    game.finalScore = 0;
    game.level = 0;
    game.leftPartCount = 0;
    game.rightPartCount = 0;
    game.countersVisible = true;
    game.activeAnimation.reset();
    audioMixer.setEffect(DecodedMedia{}, 0.0F);
    dealAudiopairsBoard();
    SDL_SetWindowTitle(window,
                       "FGP Native — Lydpar — 3 pairs · 15 attempts");
}

bool AudiopairsScene::pressAudiopairsButton(std::size_t index) {
    if (!audiopairsGame || index >= audiopairsGame->buttons.size()) {
        return false;
    }
    auto &game = *audiopairsGame;
    auto &button = game.buttons[index];
    if (game.phase != AudiopairsPhase::Playing ||
        button.state != AudiopairsButtonState::Green) {
        return false;
    }

    button.state = AudiopairsButtonState::Yellow;
    if (!game.firstSelection) {
        game.firstSelection = index;
    } else {
        game.secondSelection = index;
        game.phase = AudiopairsPhase::Resolving;
    }
    DecodedMedia &sound = audiopairsSound(button.sound);
    audioMixer.setEffect(sound,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    if (game.secondSelection) {
        const double duration =
            sound.audioRate > 0 && sound.audioChannels > 0
                ? sound.audioSamples.size() /
                      static_cast<double>(sound.audioRate *
                                          sound.audioChannels)
                : 0.0;
        game.resolveAt = sceneElapsedNow() + std::max(0.65, duration + 0.1);
    }
    return true;
}

void AudiopairsScene::playAudiopairsAnimation(std::size_t animation) {
    if (!audiopairsGame || animation >= audiopairsGame->animations.size()) {
        return;
    }
    auto &game = *audiopairsGame;
    game.activeAnimation = animation;
    game.activeAnimationStarted = sceneElapsedNow();
    audioMixer.setEffect(game.animations[animation].media,
                         audioVolumes[0] * sceneAudioGains[0]);
}

void AudiopairsScene::resolveAudiopairsPair() {
    if (!audiopairsGame || !audiopairsGame->firstSelection ||
        !audiopairsGame->secondSelection) {
        return;
    }
    auto &game = *audiopairsGame;
    auto &first = game.buttons[*game.firstSelection];
    auto &second = game.buttons[*game.secondSelection];
    const bool match = first.sound == second.sound;
    if (match) {
        first.state = AudiopairsButtonState::Red;
        second.state = AudiopairsButtonState::Red;
        game.points += 5;
        ++game.leftPartCount;

        if (game.leftPartCount >= 14) {
            ++game.level;
            game.points += game.attempts * 25;
            game.firstSelection.reset();
            game.secondSelection.reset();
            game.phase = AudiopairsPhase::LevelComplete;
            // 0x00429751 starts `left`; its completion callback starts
            // `money`, which awards the right-side part and five attempts.
            playAudiopairsAnimation(2U);
            return;
        }
    } else {
        first.state = AudiopairsButtonState::Green;
        second.state = AudiopairsButtonState::Green;
        --game.attempts;
    }
    game.firstSelection.reset();
    game.secondSelection.reset();

    if (game.attempts <= 0) {
        game.finalScore = game.points;
        game.points = 0;
        game.phase = AudiopairsPhase::GameOver;
        game.activeAnimation.reset();
        showMessageWithTitle(
            language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
            language.get("audiopairs", "ENDMESSAGE",
                         "Beklager, ingen flere forsøk igjen."),
            MessageKind::AudiopairsGameOver);
        return;
    }

    const bool boardComplete = std::none_of(
        game.buttons.begin(), game.buttons.end(),
        [](const AudiopairsButton &button) {
            return button.state == AudiopairsButtonState::Green ||
                   button.state == AudiopairsButtonState::Yellow;
        });
    if (boardComplete) {
        game.attempts += 3;
        dealAudiopairsBoard();
        // 0x00429450 deals the next board and only then plays `lever`.
        // Individual matches merely reveal mechanism parts.
        playAudiopairsAnimation(1U);
    } else {
        game.phase = AudiopairsPhase::Playing;
    }
    SDL_SetWindowTitle(
        window,
        ("FGP Native — Lydpar — level " + std::to_string(game.level + 1) +
         " · " + std::to_string(game.activePairs) + " pairs · " +
         std::to_string(game.attempts) + " attempts · " +
         std::to_string(game.points) + " points")
            .c_str());
}

void AudiopairsScene::updateAudiopairs(double elapsed) {
    if (!audiopairsGame) {
        return;
    }
    auto &game = *audiopairsGame;
    if (game.phase == AudiopairsPhase::Resolving && elapsed >= game.resolveAt) {
        resolveAudiopairsPair();
    }
    if (game.activeAnimation && *game.activeAnimation != 0U) {
        const std::size_t completed = *game.activeAnimation;
        const auto &animation = game.animations[completed];
        if (elapsed - game.activeAnimationStarted >=
            animation.media.videoDuration) {
            if (game.phase == AudiopairsPhase::LevelComplete &&
                completed == 2U) {
                playAudiopairsAnimation(3U);
            } else if (game.phase == AudiopairsPhase::LevelComplete &&
                       completed == 3U) {
                game.activeAnimation.reset();
                game.attempts += 5;
                ++game.rightPartCount;
                if (game.rightPartCount >= 14) {
                    game.rightPartCount = 0;
                    game.points += 1000;
                }
                game.phase = AudiopairsPhase::ClearingLeft;
                game.partClearInterval =
                    std::uniform_real_distribution<double>(0.15, 0.30)(
                        randomEngine);
                game.nextPartClearAt = elapsed + game.partClearInterval;
            } else {
                game.activeAnimation.reset();
            }
        }
    }
    if (game.phase == AudiopairsPhase::ClearingLeft &&
        elapsed >= game.nextPartClearAt) {
        if (game.leftPartCount > 0) {
            // 0x004297a0/0x004297c0 remove the last registered part and
            // start part14.smk, part13.smk ... part01.smk in that order.
            const std::size_t part =
                static_cast<std::size_t>(game.leftPartCount - 1);
            --game.leftPartCount;
            audioMixer.setEffect(game.partTones[part],
                                 audioVolumes[0] * sceneAudioGains[0]);
            game.nextPartClearAt += game.partClearInterval;
        } else {
            // Completing the machine abandons any remaining old-board
            // pairs. The next deal therefore reflects the new level
            // immediately instead of waiting for that board to finish.
            dealAudiopairsBoard();
            playAudiopairsAnimation(1U);
            SDL_SetWindowTitle(
                window,
                ("FGP Native — Lydpar — level " +
                 std::to_string(game.level + 1) + " · " +
                 std::to_string(game.activePairs) + " pairs · " +
                 std::to_string(game.attempts) + " attempts · " +
                 std::to_string(game.points) + " points")
                    .c_str());
        }
    }
}

void AudiopairsScene::renderAudiopairs(double elapsed) {
    if (!audiopairsGame) {
        return;
    }
    auto &game = *audiopairsGame;
    for (int index = 0; index < game.leftPartCount; ++index) {
        SDL_RenderCopy(renderer, game.leftParts[index].texture.get(), nullptr,
                       &game.leftParts[index].bounds);
    }
    for (int index = 0; index < game.rightPartCount; ++index) {
        SDL_RenderCopy(renderer, game.rightParts[index].texture.get(), nullptr,
                       &game.rightParts[index].bounds);
    }
    SDL_Rect pigBounds{373, 519, game.pigWidth, game.pigHeight};
    SDL_RenderCopy(renderer, game.pigTexture.get(), nullptr, &pigBounds);

    for (std::size_t index = 0; index < game.buttons.size(); ++index) {
        const auto &button = game.buttons[index];
        const auto colour = static_cast<std::size_t>(button.state);
        const std::size_t row = index / 5U;
        SDL_RenderCopy(renderer, game.buttonTextures[colour][row].get(),
                       nullptr, &button.bounds);
    }

    auto drawAnimation = [&](std::size_t index, double localTime) {
        auto &animation = game.animations[index];
        if (animation.texture && !animation.media.videoFrames.empty()) {
            const std::size_t frameIndex =
                frameIndexAt(animation.media, localTime, false);
            const auto &frame = animation.media.videoFrames[frameIndex];
            uploadVideoFrame(animation.texture.get(), frame);
            SDL_Rect destination{animation.x, animation.y, frame.width,
                                 frame.height};
            SDL_RenderCopy(renderer, animation.texture.get(), nullptr,
                           &destination);
        }
    };

    // FGP.exe creates the lever player during scene initialization, so its
    // first/resting frame remains visible even when it is not playing.
    const bool leverPlaying =
        game.activeAnimation && *game.activeAnimation == 1U;
    drawAnimation(1U, leverPlaying
                           ? elapsed - game.activeAnimationStarted
                           : 0.0);

    if (game.phase == AudiopairsPhase::Attract) {
        drawAnimation(0U, elapsed - game.activeAnimationStarted);
    } else if (game.activeAnimation && *game.activeAnimation >= 2U) {
        drawAnimation(*game.activeAnimation,
                      elapsed - game.activeAnimationStarted);
    }

    drawCenteredText(renderer, menuTitleFont,
                     language.get("audiopairs", "SCENENAME", "Lydpar"),
                     SDL_Rect{243, 7, 320, 54});
    drawCenteredText(renderer, dialogFont,
                     language.get("audiopairs", "LIFE", "Forsøk"),
                     SDL_Rect{300, 124, 70, 15});
    drawCenteredText(renderer, dialogFont,
                     language.get("audiopairs", "POINTS", "Poeng"),
                     SDL_Rect{433, 124, 69, 15});
    // The initial controls remain blank until the Start handler calls the
    // numeric setters at 0x004290c6. Keep them visible thereafter: the
    // verified post-score attract state intentionally displays 0 / 0.
    if (game.countersVisible) {
        drawCenteredText(renderer, menuTitleFont, std::to_string(game.attempts),
                         SDL_Rect{300, 134, 69, 29});
        drawCenteredText(renderer, menuTitleFont, std::to_string(game.points),
                         SDL_Rect{433, 134, 69, 29});
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
AudiopairsScene::audiopairsHotspotAt(int x, int y) const {
    if (!audiopairsGame || activeScene != "audiopairs") {
        return std::nullopt;
    }
    const auto &game = *audiopairsGame;
    const SDL_Point point{x, y};
    if (game.phase == AudiopairsPhase::Attract ||
        game.phase == AudiopairsPhase::GameOver) {
        const auto &start = game.animations[0];
        if (!start.media.videoFrames.empty()) {
            const auto &frame = start.media.videoFrames.front();
            const SDL_Rect bounds{start.x, start.y, frame.width, frame.height};
            if (SDL_PointInRect(&point, &bounds)) {
                return std::pair{3U, bounds};
            }
        }
        return std::nullopt;
    }
    if (game.phase != AudiopairsPhase::Playing) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < game.buttons.size(); ++index) {
        const auto &button = game.buttons[index];
        if (button.state == AudiopairsButtonState::Green &&
            SDL_PointInRect(&point, &button.bounds)) {
            return std::pair{static_cast<std::uint32_t>(10U + index),
                             button.bounds};
        }
    }
    return std::nullopt;
}

bool AudiopairsScene::executeAudiopairsDeveloperCommand(std::string_view command) {
    constexpr std::string_view audiopairsControlPrefix = "audiopairs:";
    if (activeScene != "audiopairs" || !audiopairsGame) {
        return false;
    }
    const std::string_view control =
        command.substr(audiopairsControlPrefix.size());
    if (control == "start") {
        startAudiopairsGame();
    } else if (control == "fixture-deal") {
        if (audiopairsGame->phase != AudiopairsPhase::Playing) {
            return false;
        }
        // Match the x32dbg adapter's bounded-random return fixture:
        // the original deal registers indices 0-5 as three pairs.
        for (auto &button : audiopairsGame->buttons) {
            button.sound = -1;
            button.state = AudiopairsButtonState::Black;
        }
        for (std::size_t index = 0; index < 6; ++index) {
            auto &button = audiopairsGame->buttons[index];
            button.sound = static_cast<int>(index / 2);
            button.state = AudiopairsButtonState::Green;
        }
        audiopairsGame->activePairs = 3;
        audiopairsGame->firstSelection.reset();
        audiopairsGame->secondSelection.reset();
    } else if (control.rfind("set-score=", 0) == 0) {
        // Retry until Start has initialized the run; it resets score.
        if (audiopairsGame->phase != AudiopairsPhase::Playing) {
            return false;
        }
        const auto parsed = std::stoul(std::string(control.substr(
            std::string_view("set-score=").size())));
        if (parsed > static_cast<unsigned long>(
                         std::numeric_limits<int>::max())) {
            throw std::runtime_error(
                "Developer Audiopairs score is out of range");
        }
        audiopairsGame->points = static_cast<int>(parsed);
    } else if (control == "last-attempt") {
        if (audiopairsGame->phase != AudiopairsPhase::Playing) {
            return false;
        }
        audiopairsGame->attempts = 1;
    } else if (control.rfind("press:", 0) == 0) {
        const auto index = static_cast<std::size_t>(std::stoul(
            std::string(control.substr(6))));
        if (!pressAudiopairsButton(index)) {
            return false;
        }
    } else if (control == "match" || control == "mismatch" ||
               control == "complete-level") {
        if (audiopairsGame->phase == AudiopairsPhase::Attract ||
            audiopairsGame->phase == AudiopairsPhase::GameOver) {
            startAudiopairsGame();
        }
        if (audiopairsGame->phase != AudiopairsPhase::Playing) {
            return false;
        }
        if (control == "complete-level") {
            audiopairsGame->leftPartCount = 13;
        }
        std::optional<std::pair<std::size_t, std::size_t>> pair;
        for (std::size_t first = 0;
             first < audiopairsGame->buttons.size() && !pair; ++first) {
            const auto &left = audiopairsGame->buttons[first];
            if (left.state != AudiopairsButtonState::Green) {
                continue;
            }
            for (std::size_t second = first + 1;
                 second < audiopairsGame->buttons.size(); ++second) {
                const auto &right = audiopairsGame->buttons[second];
                if (right.state != AudiopairsButtonState::Green) {
                    continue;
                }
                const bool same = left.sound == right.sound;
                if (same == (control != "mismatch")) {
                    pair = std::pair{first, second};
                    break;
                }
            }
        }
        if (!pair || !pressAudiopairsButton(pair->first) ||
            !pressAudiopairsButton(pair->second)) {
            return false;
        }
        resolveAudiopairsPair();
    } else if (control == "status") {
        std::cout << "Audiopairs status: level "
                  << audiopairsGame->level + 1 << ", "
                  << audiopairsGame->activePairs << " active pairs, "
                  << audiopairsGame->attempts << " attempts, "
                  << audiopairsGame->points << " points, parts "
                  << audiopairsGame->leftPartCount << "/14 + "
                  << audiopairsGame->rightPartCount << "/14\n";
    } else {
        throw std::runtime_error(
            "Unknown developer Audiopairs control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
