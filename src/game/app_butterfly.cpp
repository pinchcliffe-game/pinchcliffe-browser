#include "game/app_butterfly.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string_view>

#include "media/smacker.h"

namespace fgp {
namespace {

enum ButterflyAnimationIndex : std::size_t {
    Start,
    Catcher,
    Catch,
    Miss,
    Scream,
    TimerStart,
    TimerMove,
    TimerEnd,
};

struct SpeciesDefinition {
    std::string_view name;
    int rarityClass;
    int points;
    int spawnWeight;
};

// FGP.exe 0x0049e078, 64 bytes per entry.
const auto kSpecies = originalArray<SpeciesDefinition, 15>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e078U + static_cast<std::uint32_t>(index * 64);
    return SpeciesDefinition{source.string(source.read<std::uint32_t>(address + 4)),
        source.read<int>(address), source.read<int>(address + 8), source.read<int>(address + 12)};
});

const auto kPests = originalArray<SpeciesDefinition, 2>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e438U + static_cast<std::uint32_t>(index * 64);
    return SpeciesDefinition{source.string(source.read<std::uint32_t>(address + 4)),
        source.read<int>(address), source.read<int>(address + 8), source.read<int>(address + 12)};
});

struct LevelDefinition {
    double seconds;
    int target;
    double speed;
};

// FGP.exe 0x0049e4b8: milliseconds, target, float multiplier.
const auto kLevels = originalArray<LevelDefinition, 11>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e4b8U + static_cast<std::uint32_t>(index * 12);
    return LevelDefinition{source.read<int>(address) / 1000.0, source.read<int>(address + 4), source.read<float>(address + 8)};
});

constexpr std::array<int, 8> kDirections{{0, 45, 90, 135, 180, 225, 270,
                                          315}};

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

int directionIndex(double vx, double vy) {
    constexpr double kPi = 3.14159265358979323846;
    double degrees = std::atan2(-vy, vx) * 180.0 / kPi;
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return static_cast<int>(std::lround(degrees / 45.0)) % 8;
}

} // namespace

ButterflyScene::ButterflyGame ButterflyScene::loadButterflyGame(Package &package) {
    ButterflyGame game;

    const auto loadKeyedTexture = [&](std::string_view asset,
                                      int *width = nullptr,
                                      int *height = nullptr) {
        SurfacePtr surface =
            loadGreenKeyedBmp(package.data(package.find(asset)));
        if (width) {
            *width = surface->w;
        }
        if (height) {
            *height = surface->h;
        }
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        return texture;
    };

    for (std::size_t index = 0; index < game.levelTextures.size(); ++index) {
        game.levelTextures[index] = loadKeyedTexture(
            "bitmap\\level_" + std::to_string(index) + ".bmp");
    }

    constexpr std::array<std::string_view, 8> borderNames{{
        "borderleft", "borderright", "bordertop", "borderbottom",
        "bordertopleft", "bordertopright", "borderbottomleft",
        "borderbottomright",
    }};
    constexpr std::array<SDL_Point, 8> borderPositions{{
        {-8, 104}, {703, 110}, {37, 56}, {43, 533},
        {-8, 56}, {690, 56}, {-8, 516}, {684, 510},
    }};
    for (std::size_t index = 0; index < game.borders.size(); ++index) {
        int width = 0;
        int height = 0;
        game.borders[index].texture = loadKeyedTexture(
            "bitmap\\" + std::string(borderNames[index]) + ".bmp",
            &width, &height);
        game.borders[index].bounds = {borderPositions[index].x,
                                      borderPositions[index].y,
                                      width, height};
    }
    for (std::size_t index = 0; index < game.albumTextures.size(); ++index) {
        game.albumTextures[index] = loadKeyedTexture(
            "bitmap\\album_" + std::to_string(index) + ".bmp");
    }

    const auto loadSpecies = [&](ButterflySpecies &species,
                                 const SpeciesDefinition &definition,
                                 bool cardinalOnly) {
        species.name = definition.name;
        species.rarityClass = definition.rarityClass;
        species.points = definition.points;
        species.spawnWeight = definition.spawnWeight;
        if (!cardinalOnly) {
            species.iconTexture = loadKeyedTexture(
                "bitmap\\" + species.name + ".bmp");
        }
        for (std::size_t direction = 0; direction < kDirections.size();
             ++direction) {
            if (cardinalOnly && (kDirections[direction] % 90) != 0) {
                continue;
            }
            auto &media = species.directionalMedia[direction];
            media = decodeActiveSceneClip(
                species.name + std::to_string(kDirections[direction]));
            species.directionalTextures[direction] =
                makeAnimationTexture(renderer, media);
        }
    };
    for (std::size_t index = 0; index < game.species.size(); ++index) {
        loadSpecies(game.species[index], kSpecies[index], false);
    }
    for (std::size_t index = 0; index < game.pests.size(); ++index) {
        loadSpecies(game.pests[index], kPests[index], true);
    }

    constexpr std::array<std::string_view, 8> animationNames{{
        "startanim", "catcher", "catch", "miss", "scream",
        "timerstart", "timermove", "timerend",
    }};
    for (std::size_t index = 0; index < game.animations.size(); ++index) {
        game.animations[index].media =
            decodeActiveSceneClip(std::string(animationNames[index]));
        game.animations[index].texture = makeAnimationTexture(
            renderer, game.animations[index].media);
    }

    // FGP.exe 0x0043A70A bytes
    // 6a 00 6a ff 6a ff 68 70 8c 4d 00 e8 26 81 fd ff:
    // add timerstart at (-1,-1), layer 0 during scene initialization.
    referenceTrace.emit("animation_add", {
        {"asset", "timerstart"}, {"x", -1}, {"y", -1},
        {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});

    std::cout << "Loaded executable-derived Butterfly game: 15 species, 2 "
                 "pests, 11 levels, 4 environments, and 128 flight clips\n";
    return game;
}

void ButterflyScene::playButterflyEffect(std::size_t animation) {
    if (!butterflyGame || animation >= butterflyGame->animations.size()) {
        return;
    }
    audioMixer.setEffect(butterflyGame->animations[animation].media,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void ButterflyScene::startButterflyGame() {
    if (!butterflyGame) {
        return;
    }
    auto &game = *butterflyGame;
    const double now = sceneElapsedNow();
    referenceTrace.emit("animation_remove", {
        {"asset", "startanim"}, {"reason", "removed"}});
    referenceTrace.emit("animation_stop", {
        {"asset", "startanim"}, {"reason", "stopped"}});
    game.phase = ButterflyPhase::Playing;
    game.entities.clear();
    game.caught.fill(0);
    game.score = 0;
    game.finalScore = 0;
    game.levelScore = 0;
    game.level = 0;
    game.numericHudInitialized = true;
    game.levelStartedAt = now;
    game.lastUpdateAt = now;
    game.nextButterflyAt = now;
    game.nextPestAt = now + 1.0;
    game.catcherSwingStarted = -1.0;
    for (int index = 0; index < 4; ++index) {
        spawnButterflyEntity(false, now + index * 0.08);
    }
    spawnButterflyEntity(true, now);
    game.nextButterflyAt = now + 0.7;
    game.nextPestAt = now + 1.5;
    playButterflyEffect(TimerStart);
    referenceTrace.emit("animation_start", {
        {"asset", "timerstart"}, {"x", -1}, {"y", -1},
        {"layer", 0}, {"loop", false}});
    SDL_SetWindowTitle(window,
                       "FGP Native — Sommerfugler — level 1");
}

void ButterflyScene::spawnButterflyEntity(bool pest, double elapsed) {
    if (!butterflyGame) {
        return;
    }
    auto &game = *butterflyGame;
    const auto activeCount = std::count_if(
        game.entities.begin(), game.entities.end(),
        [pest](const ButterflyEntity &entity) { return entity.pest == pest; });
    if (activeCount >= 30) {
        return;
    }

    int totalWeight = 0;
    if (pest) {
        for (const auto &species : game.pests) {
            totalWeight += species.spawnWeight;
        }
    } else {
        for (const auto &species : game.species) {
            totalWeight += species.spawnWeight;
        }
    }
    std::uniform_int_distribution<int> weight(1, totalWeight);
    int selectedWeight = weight(randomEngine);
    int selected = 0;
    const auto select = [&](const auto &species) {
        for (std::size_t index = 0; index < species.size(); ++index) {
            selectedWeight -= species[index].spawnWeight;
            if (selectedWeight <= 0) {
                selected = static_cast<int>(index);
                break;
            }
        }
    };
    if (pest) {
        select(game.pests);
    } else {
        select(game.species);
    }

    std::uniform_int_distribution<int> side(0, 1);
    std::uniform_real_distribution<double> y(94.0, 505.0);
    std::uniform_real_distribution<double> vertical(-22.0, 22.0);
    std::uniform_real_distribution<double> speedVariation(0.85, 1.18);
    const bool fromLeft = side(randomEngine) == 0;
    const double multiplier =
        kLevels[static_cast<std::size_t>(game.level)].speed;
    const double speed = (pest ? 72.0 : 58.0) * multiplier *
                         speedVariation(randomEngine);
    game.entities.push_back(ButterflyEntity{
        pest, selected, fromLeft ? -8.0 : 702.0, y(randomEngine),
        fromLeft ? speed : -speed, vertical(randomEngine) * multiplier,
        elapsed + 0.5,
    });
}

void ButterflyScene::advanceButterflyLevel(double elapsed) {
    if (!butterflyGame) {
        return;
    }
    auto &game = *butterflyGame;
    const auto &previous = kLevels[static_cast<std::size_t>(game.level)];
    const double remaining = std::max(
        0.0, previous.seconds - (elapsed - game.levelStartedAt));
    game.score += static_cast<int>(std::lround(
        remaining * static_cast<double>(game.level + 5) / 20.0));
    if (game.level + 1 >= static_cast<int>(kLevels.size())) {
        game.finalScore = game.score;
        game.score = 0;
        game.levelScore = 0;
        game.level = 0;
        game.entities.clear();
        game.caught.fill(0);
        game.phase = ButterflyPhase::GameOver;
        game.gameOverStarted = elapsed;
        playButterflyEffect(TimerEnd);
        referenceTrace.emit("animation_add", {
            {"asset", "timerend"}, {"x", -1}, {"y", -1},
            {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_start", {
            {"asset", "timerend"}, {"x", -1}, {"y", -1},
            {"layer", 0}, {"loop", false}});
        SDL_SetWindowTitle(window,
                           "FGP Native — Sommerfugler — all levels complete");
        return;
    }
    ++game.level;
    game.levelScore = 0;
    game.levelStartedAt = elapsed;
    game.nextButterflyAt = elapsed + 0.2;
    game.nextPestAt = elapsed + 0.8;
    playButterflyEffect(TimerStart);
    SDL_SetWindowTitle(
        window,
        ("FGP Native — Sommerfugler — level " +
         std::to_string(game.level + 1))
            .c_str());
}

void ButterflyScene::updateButterfly(double elapsed) {
    if (!butterflyGame) {
        return;
    }
    auto &game = *butterflyGame;
    if (game.phase == ButterflyPhase::GameOver) {
        if (dialogMode == DialogMode::None &&
            elapsed - game.gameOverStarted >=
                game.animations[TimerEnd].media.videoDuration) {
            // The original callback at 0x0043b990 waits for timerend, then
            // enters 0x0043ba40. That routine opens this titled modal at
            // 0x0043ba5a-0x0043ba78 before passing score 0x00667e2c to the
            // shared wrapper at 0x0043bad1-0x0043bae3.
            showMessageWithTitle(
                language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
                language.get("butterfly", "ENDMESSAGE",
                             "Au! Der gikk tiden ut, men prøv igjen."),
                MessageKind::ButterflyGameOver);
        }
        return;
    }
    if (game.phase != ButterflyPhase::Playing) {
        return;
    }
    const auto &level = kLevels[static_cast<std::size_t>(game.level)];
    if (elapsed - game.levelStartedAt >= level.seconds) {
        game.finalScore = game.score;
        game.score = 0;
        game.levelScore = 0;
        game.level = 0;
        game.entities.clear();
        game.caught.fill(0);
        game.phase = ButterflyPhase::GameOver;
        game.gameOverStarted = elapsed;
        game.catcherSwingStarted = -1.0;
        playButterflyEffect(TimerEnd);
        referenceTrace.emit("animation_add", {
            {"asset", "timerend"}, {"x", -1}, {"y", -1},
            {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_start", {
            {"asset", "timerend"}, {"x", -1}, {"y", -1},
            {"layer", 0}, {"loop", false}});
        SDL_SetWindowTitle(window,
                           "FGP Native — Sommerfugler — time is up");
        return;
    }

    const double delta = std::clamp(elapsed - game.lastUpdateAt, 0.0, 0.05);
    game.lastUpdateAt = elapsed;
    std::uniform_real_distribution<double> turn(-24.0, 24.0);
    for (auto &entity : game.entities) {
        if (elapsed >= entity.nextTurnAt) {
            entity.vy = std::clamp(entity.vy + turn(randomEngine),
                                   -48.0, 48.0);
            entity.nextTurnAt = elapsed + 0.4;
        }
        if (entity.y < 92.0 && entity.vy < 0.0) {
            entity.vy = -entity.vy;
        } else if (entity.y > 510.0 && entity.vy > 0.0) {
            entity.vy = -entity.vy;
        }
        entity.x += entity.vx * delta;
        entity.y += entity.vy * delta;
    }
    std::erase_if(game.entities, [](const ButterflyEntity &entity) {
        return entity.x < -48.0 || entity.x > 750.0 ||
               entity.y < 55.0 || entity.y > 545.0;
    });

    if (elapsed >= game.nextButterflyAt) {
        spawnButterflyEntity(false, elapsed);
        const double interval = std::max(0.30, 0.78 - game.level * 0.035);
        game.nextButterflyAt = elapsed + interval;
    }
    if (elapsed >= game.nextPestAt) {
        spawnButterflyEntity(true, elapsed);
        const double interval = std::max(0.65, 1.55 - game.level * 0.06);
        game.nextPestAt = elapsed + interval;
    }
    if (game.catcherSwingStarted >= 0.0 &&
        elapsed - game.catcherSwingStarted >=
            game.animations[Catcher].media.videoDuration) {
        game.catcherSwingStarted = -1.0;
    }
}

void ButterflyScene::catchButterflyAt(int x, int y) {
    if (!butterflyGame || butterflyGame->phase != ButterflyPhase::Playing) {
        return;
    }
    auto &game = *butterflyGame;
    const double now = sceneElapsedNow();
    game.catcherSwingStarted = now;

    const auto distanceSquared = [x, y](const ButterflyEntity &entity) {
        const double dx = entity.x + 16.0 - x;
        const double dy = entity.y + 16.0 - y;
        return dx * dx + dy * dy;
    };
    const auto nearest = [&](bool pest, double radius) {
        auto result = game.entities.end();
        double best = radius * radius;
        for (auto item = game.entities.begin(); item != game.entities.end();
             ++item) {
            if (item->pest != pest) {
                continue;
            }
            const double distance = distanceSquared(*item);
            if (distance <= best) {
                best = distance;
                result = item;
            }
        }
        return result;
    };

    // The executable deliberately checks stinging insects first.
    if (auto caught = nearest(true, 35.0); caught != game.entities.end()) {
        const int penalty = game.pests[static_cast<std::size_t>(
            caught->species)].points;
        game.score += penalty;
        game.levelScore += penalty;
        game.entities.erase(caught);
        playButterflyEffect(Scream);
        return;
    }
    if (auto caught = nearest(false, 35.0); caught != game.entities.end()) {
        const int species = caught->species;
        const int points =
            game.species[static_cast<std::size_t>(species)].points;
        game.score += points;
        game.levelScore += points;
        ++game.caught[static_cast<std::size_t>(species)];
        game.entities.erase(caught);
        playButterflyEffect(Catch);
        if (game.levelScore >=
            kLevels[static_cast<std::size_t>(game.level)].target) {
            advanceButterflyLevel(now);
        }
        return;
    }

    // Near misses make butterflies scatter, matching the 150-pixel evasion
    // radius recovered from the click handler.
    for (auto &entity : game.entities) {
        if (entity.pest || distanceSquared(entity) > 150.0 * 150.0) {
            continue;
        }
        const double dx = entity.x + 16.0 - x;
        const double dy = entity.y + 16.0 - y;
        const double length = std::max(1.0, std::hypot(dx, dy));
        entity.vx += dx / length * 35.0;
        entity.vy += dy / length * 35.0;
    }
    playButterflyEffect(Miss);
}

void ButterflyScene::renderButterfly(double elapsed) {
    if (!butterflyGame) {
        return;
    }
    auto &game = *butterflyGame;
    if (game.phase != ButterflyPhase::Attract) {
        const std::size_t background =
            static_cast<std::size_t>(game.level) % game.levelTextures.size();
        SDL_Rect field{20, 82, 689, 455};
        SDL_RenderCopy(renderer, game.levelTextures[background].get(), nullptr,
                       &field);
    }

    const auto drawMedia = [&](ButterflyAnimation &animation,
                               double localTime, bool loop,
                               const SDL_Rect &destination) {
        if (!animation.texture || animation.media.videoFrames.empty()) {
            return;
        }
        const std::size_t frameIndex = frameIndexAt(
            animation.media, std::max(0.0, localTime), loop);
        const auto &frame = animation.media.videoFrames[frameIndex];
        uploadVideoFrame(animation.texture.get(), frame);
        SDL_RenderCopy(renderer, animation.texture.get(), nullptr,
                       &destination);
    };

    std::array<bool, 15> activeSpecies{};
    for (const auto &entity : game.entities) {
        auto &species = entity.pest
                            ? game.pests[static_cast<std::size_t>(entity.species)]
                            : game.species[static_cast<std::size_t>(entity.species)];
        int direction = directionIndex(entity.vx, entity.vy);
        if (entity.pest && (direction % 2) != 0) {
            direction = ((direction + 1) / 2 * 2) % 8;
        }
        auto &media = species.directionalMedia[static_cast<std::size_t>(direction)];
        auto &texture = species.directionalTextures[static_cast<std::size_t>(direction)];
        if (!media.videoFrames.empty() && texture) {
            const auto frameIndex = frameIndexAt(media, elapsed, true);
            const auto &frame = media.videoFrames[frameIndex];
            uploadVideoFrame(texture.get(), frame);
            SDL_Rect destination{static_cast<int>(std::lround(entity.x)),
                                 static_cast<int>(std::lround(entity.y)),
                                 frame.width, frame.height};
            SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
        }
        if (!entity.pest) {
            activeSpecies[static_cast<std::size_t>(entity.species)] = true;
        }
    }

    for (const auto &border : game.borders) {
        SDL_RenderCopy(renderer, border.texture.get(), nullptr,
                       &border.bounds);
    }

    int commonX = 77;
    int rareX = 581;
    for (std::size_t index = 0; index < game.species.size(); ++index) {
        if (!activeSpecies[index]) {
            continue;
        }
        auto &species = game.species[index];
        if (species.rarityClass == 2) {
            SDL_Rect icon{rareX, 6, 61, 56};
            SDL_RenderCopy(renderer, species.iconTexture.get(), nullptr,
                           &icon);
            rareX += 61;
        } else {
            SDL_Rect icon{commonX, 6, 52, 56};
            SDL_RenderCopy(renderer, species.iconTexture.get(), nullptr,
                           &icon);
            commonX += 52;
        }
    }

    SDL_Rect album{24, 541, 134, 59};
    SDL_RenderCopy(renderer, game.albumTextures[0].get(), nullptr, &album);

    drawCenteredText(renderer, dialogFont,
                     language.get("butterfly", "COMMON", "Vanlig"),
                     SDL_Rect{190, 63, 267, 16});
    drawCenteredText(renderer, dialogFont,
                     language.get("butterfly", "RARE", "Sjelden"),
                     SDL_Rect{598, 63, 103, 16});
    const auto &level = kLevels[static_cast<std::size_t>(game.level)];
    // Scene init creates these three text objects without contents at
    // 0x0043a5be-0x0043a5ec. Level initializer 0x0043b220 assigns score,
    // remaining target, and level together at 0x0043b288-0x0043b302.
    if (game.numericHudInitialized) {
        drawCenteredText(renderer, menuTitleFont, std::to_string(game.score),
                         SDL_Rect{268, 554, 178, 30});
        drawCenteredText(
            renderer, menuTitleFont,
            std::to_string(std::max(0, level.target - game.levelScore)),
            SDL_Rect{553, 545, 60, 32});
        drawCenteredText(renderer, menuTitleFont,
                         std::to_string(game.level + 1),
                         SDL_Rect{616, 545, 50, 32});
    }
    drawCenteredText(renderer, dialogFont,
                     language.get("butterfly", "NEXTLEVEL", "Neste niv\xe5"),
                     SDL_Rect{553, 577, 60, 17});
    drawCenteredText(renderer, dialogFont,
                     language.get("butterfly", "LEVEL", "Niv\xe5"),
                     SDL_Rect{616, 577, 50, 17});

    if (game.phase == ButterflyPhase::Attract) {
        drawMedia(game.animations[Start], elapsed - game.attractStartedAt, true,
                  SDL_Rect{211, 161, 300, 192});
        return;
    }

    if (game.phase == ButterflyPhase::GameOver) {
        drawMedia(game.animations[TimerEnd], elapsed - game.gameOverStarted,
                  false, SDL_Rect{728, 103, 52, 124});
    } else {
        const double used = std::clamp(elapsed - game.levelStartedAt,
                                       0.0, level.seconds);
        const double startDuration = game.animations[TimerStart].media.videoDuration;
        if (used < startDuration) {
            drawMedia(game.animations[TimerStart], used, false,
                      SDL_Rect{728, 425, 52, 124});
        } else {
            const double progress = used / level.seconds;
            const int timerY = static_cast<int>(std::lround(425.0 - 322.0 * progress));
            drawMedia(game.animations[TimerMove], used - startDuration, true,
                      SDL_Rect{728, timerY, 52, 124});
        }

        const bool swinging = game.catcherSwingStarted >= 0.0;
        drawMedia(game.animations[Catcher],
                  swinging ? elapsed - game.catcherSwingStarted : 0.0,
                  false, SDL_Rect{mouseX - 35, mouseY - 35, 124, 124});
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
ButterflyScene::butterflyHotspotAt(int x, int y) const {
    if (!butterflyGame || activeScene != "butterfly") {
        return std::nullopt;
    }
    const SDL_Point point{x, y};
    const SDL_Rect album{24, 541, 134, 59};
    if (SDL_PointInRect(&point, &album)) {
        return std::pair{4U, album};
    }
    if (butterflyGame->phase == ButterflyPhase::Attract) {
        const SDL_Rect start{211, 161, 300, 192};
        if (SDL_PointInRect(&point, &start)) {
            return std::pair{1U, start};
        }
    }
    return std::nullopt;
}

bool ButterflyScene::executeButterflyDeveloperCommand(std::string_view command) {
    constexpr std::string_view butterflyControlPrefix = "butterfly:";
    if (activeScene != "butterfly" || !butterflyGame) {
        return false;
    }
    const std::string_view control =
        command.substr(butterflyControlPrefix.size());
    if (control == "start") {
        startButterflyGame();
    } else if (control == "catch" || control == "pest" ||
               control == "miss" || control == "complete-level") {
        if (butterflyGame->phase != ButterflyPhase::Playing) {
            startButterflyGame();
        }
        const double now = sceneElapsedNow();
        if (control == "complete-level") {
            static constexpr std::array<int, 11> targets{
                250, 500, 750, 1000, 1500, 2000,
                2500, 3250, 4000, 5000, 7500};
            butterflyGame->levelScore =
                targets[static_cast<std::size_t>(butterflyGame->level)] -
                butterflyGame->species[0].points;
        }
        if (control == "miss") {
            catchButterflyAt(400, 300);
        } else {
            const bool wantPest = control == "pest";
            auto found = std::find_if(
                butterflyGame->entities.begin(),
                butterflyGame->entities.end(),
                [wantPest](const ButterflyEntity &entity) {
                    return entity.pest == wantPest;
                });
            if (found == butterflyGame->entities.end()) {
                spawnButterflyEntity(wantPest, now);
                found = std::prev(butterflyGame->entities.end());
            }
            catchButterflyAt(
                static_cast<int>(std::lround(found->x + 16.0)),
                static_cast<int>(std::lround(found->y + 16.0)));
        }
    } else if (control == "timeout") {
        if (butterflyGame->phase != ButterflyPhase::Playing) {
            return false;
        }
        const double now = sceneElapsedNow();
        butterflyGame->levelStartedAt = now - 1000000.0;
        updateButterfly(now);
    } else if (control.rfind("set-score=", 0) == 0) {
        // Retry until Start has initialized the run; it resets score.
        if (butterflyGame->phase != ButterflyPhase::Playing) {
            return false;
        }
        const auto parsed = std::stoul(std::string(control.substr(
            std::string_view("set-score=").size())));
        if (parsed > static_cast<unsigned long>(
                         std::numeric_limits<int>::max())) {
            throw std::runtime_error(
                "Developer Butterfly score is out of range");
        }
        butterflyGame->score = static_cast<int>(parsed);
    } else if (control == "status") {
        std::cout << "Butterfly status: level "
                  << butterflyGame->level + 1 << ", "
                  << butterflyGame->score << " points, "
                  << butterflyGame->entities.size()
                  << " flying entities, "
                  << std::accumulate(butterflyGame->caught.begin(),
                                     butterflyGame->caught.end(), 0)
                  << " butterflies caught\n";
    } else {
        throw std::runtime_error(
            "Unknown developer Butterfly control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
