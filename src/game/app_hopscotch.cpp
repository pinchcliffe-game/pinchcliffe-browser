#include "game/app_hopscotch.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

constexpr std::uint32_t kStartHotspot = 10U;
constexpr std::uint32_t kTileHotspotBase = 3000U;
const auto kGridOrigins = originalArray<SDL_Point, 4>(0x49e628U);
const auto kTileCounts = originalArray<int, 4>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e76cU + static_cast<std::uint32_t>(index * 8);
    const int count = source.read<int>(address);
    if (count < 1 || count > 12) throw std::runtime_error("Invalid original Hopscotch tile count");
    return count;
});
const auto kTileOffsets = originalArray<std::array<SDL_Point, 12>, 4>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e768U + static_cast<std::uint32_t>(index * 8);
    std::array<SDL_Point, 12> result{};
    const auto pointer = source.read<std::uint32_t>(address);
    for (int tile = 0; tile < kTileCounts[index]; ++tile)
        result[static_cast<std::size_t>(tile)] = source.read<SDL_Point>(pointer + static_cast<std::uint32_t>(tile * 8));
    return result;
});
constexpr std::array<std::string_view, 8> kDirections{{
    "000", "045", "090", "135", "180", "225", "270", "315",
}};
// FGP.exe 0x0049e5e8 and 0x0049e608, adjusted by the -38 pixel
// centering offset applied at 0x0043c0a0 for the 152-pixel actors.
const auto kLudvigStarts = originalArray<SDL_Point, 4>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e5e8U + static_cast<std::uint32_t>(index * 8);
    const auto point = source.read<SDL_Point>(address);
    return SDL_Point{point.x - 38, point.y - 38};
});
const auto kSolanStarts = originalArray<SDL_Point, 4>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049e608U + static_cast<std::uint32_t>(index * 8);
    const auto point = source.read<SDL_Point>(address);
    return SDL_Point{point.x - 38, point.y - 38};
});

SDL_Rect tileBounds(int board, int tile) {
    const auto origin = kGridOrigins[static_cast<std::size_t>(board)];
    const auto offset = kTileOffsets[static_cast<std::size_t>(board)]
                                    [static_cast<std::size_t>(tile)];
    return {origin.x + offset.x, origin.y + offset.y, 75, 75};
}

TexturePtr makeMediaTexture(SDL_Renderer *renderer,
                            const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
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

int directionFor(SDL_Point from, SDL_Point to) {
    const double radians = std::atan2(
        static_cast<double>(to.y - from.y),
        static_cast<double>(to.x - from.x));
    int direction = static_cast<int>(
        std::lround(radians * 4.0 / 3.14159265358979323846));
    direction = (direction + 10) % 8;
    // Asset 000 faces up and the remaining filenames advance clockwise.
    return direction;
}

SDL_Point tileActorPosition(int board, int tile) {
    const auto bounds = tileBounds(board, tile);
    // 0x0043c0a0 centers the 152x152 actor on the 75x75 tile, rounding
    // the tile's half-size upward: 75 - 152, divided by two toward -inf.
    return {bounds.x - 38, bounds.y - 38};
}

std::string actorAsset(std::string_view family, int direction) {
    return std::string(family) +
        std::string(kDirections[static_cast<std::size_t>(direction)]);
}

double hopscotchCompletionDuration(const DecodedMedia &media) {
    if (media.videoFrames.empty()) return media.videoDuration;
    // The descriptor callback at 0x0043c9b6 is observed on the first native
    // update that reaches the final frame: first-round reference deltas are
    // 750 ms for soljump and 343 ms for rotate. Do not add a pacing delay.
    return media.videoFrames.metadata(media.videoFrames.size() - 1U).time;
}

} // namespace

HopscotchScene::HopscotchGame HopscotchScene::loadHopscotchGame(Package &package) {
    HopscotchGame game;
    // FGP.exe 0x0049e788: 16 records of six DWORDs.
    for (std::size_t index = 0; index < game.levels.size(); ++index) {
        const auto row = OriginalData::instance().read<std::array<int, 6>>(
            0x0049e788U + static_cast<std::uint32_t>(index * 24));
        game.levels[index] = {row[0], row[1], row[2], row[3], row[4], row[5] != 0};
    }

    const auto texture = [&](const std::string &asset) {
        SurfacePtr surface = loadGreenKeyedBmp(
            package.data(package.find(asset)));
        TexturePtr result = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(result.get(), SDL_BLENDMODE_BLEND);
        return result;
    };
    for (int board = 0; board < 4; ++board) {
        game.gridTextures[static_cast<std::size_t>(board)] =
            texture("bitmap\\grid" + std::to_string(board + 1) + ".bmp");
    }
    for (int kind = 0; kind < 2; ++kind) {
        for (int tile = 0; tile < 12; ++tile) {
            std::ostringstream asset;
            asset << "bitmap\\" << (kind == 0 ? "number" : "symbol")
                  << std::setw(2) << std::setfill('0') << tile + 1 << ".bmp";
            game.tileTextures[static_cast<std::size_t>(kind)]
                             [static_cast<std::size_t>(tile)] =
                texture(asset.str());
        }
    }
    game.startTextures[0] = texture("bitmap\\start_1.bmp");
    game.startTextures[1] = texture("bitmap\\start_2.bmp");

    const auto loadMedia = [&](std::string name, bool video, bool audio) {
        return decodeActiveSceneClip(std::move(name), video, audio);
    };
    for (int board = 0; board < 4; ++board) {
        const std::string prefix = board == 0 ? "" :
            std::string(1, static_cast<char>('A' + board));
        for (int tile = 0; tile < 12; ++tile) {
            game.tones[static_cast<std::size_t>(board)]
                      [static_cast<std::size_t>(tile)] =
                loadMedia(prefix + std::to_string(tile + 1), false, true);
        }
    }
    const auto loadPlaced = [&](std::string name) {
        HopscotchMedia placed;
        placed.media = loadMedia(std::move(name), true, false);
        placed.texture = makeMediaTexture(renderer, placed.media);
        return placed;
    };
    for (int direction = 0; direction < 8; ++direction) {
        const auto suffix = std::string(kDirections[direction]);
        game.solanJumps[static_cast<std::size_t>(direction)] =
            loadPlaced("soljump" + suffix);
        game.ludvigJumps[static_cast<std::size_t>(direction)] =
            loadPlaced("ludjump" + suffix);
        game.ludvigFalls[static_cast<std::size_t>(direction)] =
            loadPlaced("ludfall" + suffix);
    }
    // Board zero's reset descriptors select these exact clips. The other
    // board-specific reset orientations remain UNCERTAIN.
    game.solanStart = loadPlaced("solstart180");
    game.ludvigStart = loadPlaced("ludstart000");
    game.timer = loadPlaced("timer");
    game.rotate = loadPlaced("rotate");
    game.bird = loadPlaced("bird");
    game.wrongSound = loadMedia("wrong", false, true);

    std::cout << "Loaded executable-derived Hopscotch: four exact boards, "
                 "36 tile coordinates, 16 level records, 48 tones, and "
                 "directional Solan/Ludvig animation families\n";
    return game;
}

void HopscotchScene::endHopscotchGame(double elapsed) {
    // 0x0043cd90: interfaceh:GAMEOVER over hopscotch:GAMEOVER (0x0043cdb8,
    // the modal 0x00421310), then 0x0043bde0 holds the timer's last frame,
    // 0x0043c4f0(0) returns the button to Start and 0x0043bee0 resets both
    // actors; only then does 0x0043cdec pass the score [0x0066ea38] and the
    // table 0x0049e918 to the score wrapper (GameApp::closeDialog).
    auto &game = *hopscotchGame;
    game.phase = HopscotchPhase::GameOver;
    game.phaseStarted = elapsed;
    game.phaseMessage.clear();
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
        language.get("hopscotch", "GAMEOVER", "Tiden er ute!"),
        MessageKind::HopscotchGameOver);
}

void HopscotchScene::returnHopscotchToAttract() {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    game.phase = HopscotchPhase::Attract;
    game.characterTile = -1;
}

void HopscotchScene::startHopscotchGame() {
    if (!hopscotchGame) return;
    hopscotchGame->level = 0;
    hopscotchGame->score = 0;
    beginHopscotchLevel(false);
}

void HopscotchScene::beginHopscotchLevel(bool retainScore) {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    if (!retainScore) game.score = 0;
    game.round = 0;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    game.masterSequence.resize(static_cast<std::size_t>(
        level.startingLength + level.rounds));
    const int tileCount = std::min(
        level.availableTiles,
        kTileCounts[static_cast<std::size_t>(level.board)]);
    std::vector<int> candidates(static_cast<std::size_t>(tileCount));
    std::iota(candidates.begin(), candidates.end(), 0);
    std::shuffle(candidates.begin(), candidates.end(), randomEngine);
    for (std::size_t index = 0; index < game.masterSequence.size(); ++index) {
        if (index != 0U && index % candidates.size() == 0U) {
            std::shuffle(candidates.begin(), candidates.end(), randomEngine);
        }
        game.masterSequence[index] = candidates[index % candidates.size()];
    }
    beginHopscotchRound();
}

void HopscotchScene::beginHopscotchRound() {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    game.playerPosition = 0;
    game.shownPosition = -1;
    demonstrateHopscotch(false);
}

void HopscotchScene::demonstrateHopscotch(bool hint) {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    game.phase = HopscotchPhase::Demonstrating;
    game.phaseStarted = sceneElapsedNow();
    game.shownPosition = -1;
    game.characterTile = -1;
    game.characterIsSolan = true;
    game.characterFalling = false;
    game.demonstrationStartsTurn = !hint;
    if (!hint) game.deadline = 0.0;
    startHopscotchDemonstrationJump(game.phaseStarted);
}

void HopscotchScene::startHopscotchDemonstrationJump(double elapsed) {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    const int next = game.shownPosition + 1;
    const int length = level.startingLength + game.round;
    if (next >= length) return;

    const int tile = game.masterSequence[static_cast<std::size_t>(next)];
    const SDL_Point from = next == 0
        ? kSolanStarts[static_cast<std::size_t>(level.board)]
        : tileActorPosition(
              level.board,
              game.masterSequence[static_cast<std::size_t>(next - 1)]);
    const SDL_Point to = tileActorPosition(level.board, tile);
    game.characterDirection = directionFor(from, to);
    game.characterTile = tile;
    game.characterX = from.x;
    game.characterY = from.y;
    game.characterTargetX = to.x;
    game.characterTargetY = to.y;
    game.characterStarted = elapsed;
    game.demonstrationGapStage = 0;
    game.shownPosition = next;
    const auto asset = actorAsset("soljump", game.characterDirection);
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", from.x}, {"y", from.y}, {"layer", 5}});
    referenceTrace.emit("animation_start", {
        {"asset", asset}, {"x", from.x}, {"y", from.y}});
    audioMixer.setEffect(
        game.tones[static_cast<std::size_t>(level.board)]
                  [static_cast<std::size_t>(tile)],
        audioVolumes[0] * sceneAudioGains[0]);
}

bool HopscotchScene::pressHopscotchAt(int, int, std::uint32_t hotspot) {
    if (!hopscotchGame) return false;
    auto &game = *hopscotchGame;
    if (hotspot == kStartHotspot) {
        if (game.phase == HopscotchPhase::Attract ||
            game.phase == HopscotchPhase::GameOver) {
            startHopscotchGame();
        } else if (game.phase == HopscotchPhase::PlayerTurn) {
            demonstrateHopscotch(true);
        }
        return true;
    }
    if (hotspot < kTileHotspotBase ||
        game.phase != HopscotchPhase::PlayerTurn) return false;
    const int tile = static_cast<int>(hotspot - kTileHotspotBase);
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    const int length = level.startingLength + game.round;
    if (tile < 0 || tile >= std::min(
            level.availableTiles,
            kTileCounts[static_cast<std::size_t>(level.board)])) return false;

    const SDL_Point from = game.playerPosition == 0
        ? kLudvigStarts[static_cast<std::size_t>(level.board)]
        : tileActorPosition(
              level.board,
              game.masterSequence[static_cast<std::size_t>(
                  game.playerPosition - 1)]);
    const auto to = tileActorPosition(level.board, tile);
    game.characterDirection = directionFor(from, to);
    game.characterTile = tile;
    game.characterX = from.x;
    game.characterY = from.y;
    game.characterTargetX = to.x;
    game.characterTargetY = to.y;
    game.characterStarted = sceneElapsedNow();
    game.characterIsSolan = false;
    audioMixer.setEffect(
        game.tones[static_cast<std::size_t>(level.board)]
                  [static_cast<std::size_t>(tile)],
        audioVolumes[0] * sceneAudioGains[0]);

    if (tile != game.masterSequence[static_cast<std::size_t>(
                    game.playerPosition)]) {
        game.playerPosition = 0;
        game.characterFalling = true;
        audioMixer.setEffect(game.wrongSound,
                             audioVolumes[0] * sceneAudioGains[0]);
        return true;
    }
    game.characterFalling = false;
    ++game.playerPosition;
    game.score += 5; // 0x0043ca4e
    if (game.playerPosition < length) return true;

    const int timeBonus = static_cast<int>(
        std::max(0.0, game.deadline - sceneElapsedNow())) * 5;
    game.score += 100 + timeBonus; // 0x0043ca28 + 0x0043cb90
    game.phase = HopscotchPhase::RoundComplete;
    game.phaseStarted = sceneElapsedNow();
    game.phaseMessage = language.get("hopscotch", "HINT", "Bra hoppet!");
    return true;
}

void HopscotchScene::updateHopscotch(double elapsed) {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    if (game.phase == HopscotchPhase::Demonstrating) {
        const int length = level.startingLength + game.round;
        auto &jump = game.solanJumps[
            static_cast<std::size_t>(game.characterDirection)];
        const auto asset = actorAsset("soljump", game.characterDirection);
        if (game.demonstrationGapStage != 0) {
            if (elapsed < game.nextDemonstrationStep) return;
            if (game.demonstrationGapStage == 1) {
                referenceTrace.emit("animation_add", {
                    {"asset", asset}, {"x", game.characterTargetX},
                    {"y", game.characterTargetY}, {"layer", 5}});
                game.characterX = game.characterTargetX;
                game.characterY = game.characterTargetY;
                game.demonstrationGapStage = 2;
                game.nextDemonstrationStep = elapsed + 1.0 / 64.0;
                return;
            }
            referenceTrace.emit("animation_remove", {
                {"asset", asset}, {"reason", "removed"}});
            referenceTrace.emit("animation_stop", {
                {"asset", asset}, {"reason", "stopped"}});
            game.demonstrationGapStage = 0;
            if (game.shownPosition + 1 < length) {
                startHopscotchDemonstrationJump(elapsed);
                return;
            }

            game.characterTile = -1;
            const auto solanHome =
                kSolanStarts[static_cast<std::size_t>(level.board)];
            referenceTrace.emit("animation_add", {
                {"asset", "solstart180"},
                {"x", solanHome.x}, {"y", solanHome.y}, {"layer", 5}});
            if (!game.demonstrationStartsTurn) {
                game.phase = HopscotchPhase::PlayerTurn;
                game.phaseStarted = elapsed;
                return;
            }
            game.phase = HopscotchPhase::TurnTransition;
            game.phaseStarted = elapsed;
            game.rotateStarted = false;
            referenceTrace.emit("animation_remove", {
                {"asset", "timer"}, {"reason", "removed"}});
            referenceTrace.emit("animation_stop", {
                {"asset", "timer"}, {"reason", "stopped"}});
            return;
        }
        if (elapsed - game.characterStarted <
            hopscotchCompletionDuration(jump.media)) {
            return;
        }

        referenceTrace.emit("animation_stop", {
            {"asset", asset}, {"reason", "finished"}});
        referenceTrace.emit("animation_remove", {
            {"asset", asset}, {"reason", "removed"}});
        game.demonstrationGapStage = 1;
        game.nextDemonstrationStep = elapsed + 1.0 / 64.0;
        return;
    }
    if (game.phase == HopscotchPhase::TurnTransition) {
        if (!game.rotateStarted) {
            if (elapsed - game.phaseStarted < 1.0 / 64.0) return;
            game.rotateStarted = true;
            game.phaseStarted = elapsed;
            referenceTrace.emit("animation_add", {
                {"asset", "rotate"}, {"x", 717}, {"y", 495},
                {"layer", 0}});
            referenceTrace.emit("animation_start", {
                {"asset", "rotate"}, {"x", 717}, {"y", 495}});
            return;
        }
        if (elapsed - game.phaseStarted <
            hopscotchCompletionDuration(game.rotate.media)) return;
        referenceTrace.emit("animation_stop", {
            {"asset", "rotate"}, {"reason", "finished"}});
        referenceTrace.emit("animation_remove", {
            {"asset", "rotate"}, {"reason", "removed"}});
        game.phase = HopscotchPhase::TimerTransition;
        game.phaseStarted = elapsed;
        return;
    }
    if (game.phase == HopscotchPhase::TimerTransition) {
        if (elapsed - game.phaseStarted < 1.0 / 64.0) return;
        referenceTrace.emit("animation_add", {
            {"asset", "timer"}, {"x", -1}, {"y", -1}, {"layer", -10}});
        game.phase = HopscotchPhase::PlayerTurn;
        game.phaseStarted = elapsed;
        game.deadline = elapsed + level.milliseconds / 1000.0;
        return;
    }
    if (game.phase == HopscotchPhase::PlayerTurn &&
        elapsed >= game.deadline) {
        // 0x0043cc33 -> 0x0043cd90 once the time runs out.
        endHopscotchGame(elapsed);
        return;
    }
    if (game.phase == HopscotchPhase::RoundComplete &&
        elapsed - game.phaseStarted >= 1.2) {
        ++game.round;
        if (game.round < level.rounds) {
            beginHopscotchRound();
        } else if (game.level + 1 < static_cast<int>(game.levels.size())) {
            ++game.level;
            beginHopscotchLevel(true);
        } else {
            // UNCERTAIN: what the original does after the sixteenth level
            // record is not read; native ends the game as on a timeout.
            endHopscotchGame(elapsed);
        }
    }
}

void HopscotchScene::renderHopscotch(double elapsed) {
    if (!hopscotchGame) return;
    auto &game = *hopscotchGame;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    const int board = level.board;
    const auto origin = kGridOrigins[static_cast<std::size_t>(board)];
    SDL_Rect grid{origin.x, origin.y, 0, 0};
    SDL_QueryTexture(game.gridTextures[static_cast<std::size_t>(board)].get(),
                     nullptr, nullptr, &grid.w, &grid.h);
    SDL_RenderCopy(renderer,
        game.gridTextures[static_cast<std::size_t>(board)].get(), nullptr, &grid);
    for (int tile = 0; tile < kTileCounts[static_cast<std::size_t>(board)];
         ++tile) {
        const SDL_Rect destination = tileBounds(board, tile);
        SDL_RenderCopy(renderer,
            game.tileTextures[level.symbols ? 1U : 0U]
                             [static_cast<std::size_t>(tile)].get(),
            nullptr, &destination);
    }

    SDL_Rect startButton{13, 497, 78, 78};
    SDL_RenderCopy(renderer, game.startTextures[game.pressedStart ? 1U : 0U].get(),
                   nullptr, &startButton);
    const bool showStart = game.phase == HopscotchPhase::Attract ||
                           game.phase == HopscotchPhase::GameOver;
    drawCenteredText(
        renderer, calendarFont,
        language.get("hopscotch", showStart ? "START" : "HINT",
                     showStart ? "Start" : "Hint"),
        SDL_Rect{23, 520, 57, 25});
    // The Amerigo BT_10_ object created at 0x0043bcb3 is passed to the title
    // object at 0x0043bce1. Its rectangle comes from 0x0049e908, and
    // 0x0043bcdb obtains the active scene descriptor's localized title
    // through 0x0040ce70. The descriptor at 0x004d9c60 names
    // hopscotch:SCENENAME, not the runtime score.
    drawCenteredText(renderer, dialogFont,
                     language.get("hopscotch", "SCENENAME",
                                  "Solan og Ludvig i Paradis"),
                     SDL_Rect{315, 2, 191, 31});

    const auto drawPlaced = [&](HopscotchMedia &placed, int x, int y,
                                double time, bool loop) {
        if (!placed.texture || placed.media.videoFrames.empty()) return;
        const auto frameIndex = frameIndexAt(placed.media, time, loop);
        const auto &frame = placed.media.videoFrames[frameIndex];
        uploadVideoFrame(placed.texture.get(), frame);
        SDL_Rect destination{x, y, frame.width, frame.height};
        SDL_RenderCopy(renderer, placed.texture.get(), nullptr, &destination);
    };
    if (game.phase == HopscotchPhase::Attract ||
        game.phase == HopscotchPhase::Demonstrating) {
        // 0x0043be04 reads the Smacker handle, 0x0043be0d obtains its frame
        // count, and 0x0043be12-0x0043be21 seeks to Frames - 1 before
        // stopping the attract-state timer.
        const double stoppedTimerTime = game.timer.media.videoFrames.empty()
                                            ? 0.0
                                            : game.timer.media.videoFrames.back().time;
        drawPlaced(game.timer, 731, 514, stoppedTimerTime, false);
    } else if (game.phase == HopscotchPhase::PlayerTurn) {
        const double total = level.milliseconds / 1000.0;
        const double remaining = std::max(0.0, game.deadline - elapsed);
        const double timerTime = (1.0 - remaining / total) *
                                 game.timer.media.videoDuration;
        drawPlaced(game.timer, 731, 514, timerTime, false);
    } else if (game.phase == HopscotchPhase::TurnTransition &&
               game.rotateStarted) {
        drawPlaced(game.rotate, 717, 495, elapsed - game.phaseStarted, false);
    }
    if (game.phase == HopscotchPhase::RoundComplete) {
        drawPlaced(game.bird, 533, 80, elapsed - game.phaseStarted, false);
    }
    const bool solanAtStart = game.characterTile < 0 ||
                              !game.characterIsSolan;
    if (solanAtStart) {
        const auto position = kSolanStarts[static_cast<std::size_t>(board)];
        drawPlaced(game.solanStart, position.x, position.y, 0.0, false);
    }
    if (game.characterTile >= 0) {
        auto &placed = game.characterIsSolan
            ? game.solanJumps[static_cast<std::size_t>(game.characterDirection)]
            : game.characterFalling
                ? game.ludvigFalls[static_cast<std::size_t>(game.characterDirection)]
                : game.ludvigJumps[static_cast<std::size_t>(game.characterDirection)];
        const double animationTime = elapsed - game.characterStarted;
        const double duration = std::max(
            hopscotchCompletionDuration(placed.media), 0.001);
        const double progress = std::clamp(animationTime / duration, 0.0, 1.0);
        const int actorX = static_cast<int>(std::lround(
            game.characterX + (game.characterTargetX - game.characterX) *
                                  progress));
        const int actorY = static_cast<int>(std::lround(
            game.characterY + (game.characterTargetY - game.characterY) *
                                  progress));
        drawPlaced(placed, actorX, actorY, animationTime, false);
    }
    const auto ludvig = kLudvigStarts[static_cast<std::size_t>(board)];
    if (game.characterTile < 0 || game.characterIsSolan) {
        drawPlaced(game.ludvigStart, ludvig.x, ludvig.y, 0.0, false);
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
HopscotchScene::hopscotchHotspotAt(int x, int y) const {
    if (!hopscotchGame || activeScene != "hopscotch") return std::nullopt;
    const SDL_Point point{x, y};
    const SDL_Rect startButton{13, 497, 78, 78};
    if (SDL_PointInRect(&point, &startButton)) {
        return std::pair{kStartHotspot, startButton};
    }
    const auto &game = *hopscotchGame;
    const auto &level = game.levels[static_cast<std::size_t>(game.level)];
    const int count = std::min(
        level.availableTiles,
        kTileCounts[static_cast<std::size_t>(level.board)]);
    for (int tile = 0; tile < count; ++tile) {
        const SDL_Rect bounds = tileBounds(level.board, tile);
        if (SDL_PointInRect(&point, &bounds)) {
            return std::pair{kTileHotspotBase +
                                 static_cast<std::uint32_t>(tile), bounds};
        }
    }
    return std::nullopt;
}

bool HopscotchScene::executeHopscotchDeveloperCommand(std::string_view command) {
    constexpr std::string_view hopscotchControlPrefix = "hopscotch:";
    if (activeScene != "hopscotch" || !hopscotchGame) {
        return false;
    }
    const std::string_view control =
        command.substr(hopscotchControlPrefix.size());
    if (control == "start") {
        startHopscotchGame();
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 1 || level > 16) return false;
        hopscotchGame->level = level - 1;
        beginHopscotchLevel(false);
    } else if (control == "ready") {
        hopscotchGame->phase = HopscotchPhase::PlayerTurn;
        hopscotchGame->playerPosition = 0;
        hopscotchGame->deadline = sceneElapsedNow() +
            hopscotchGame->levels[static_cast<std::size_t>(
                hopscotchGame->level)].milliseconds / 1000.0;
    } else if (control == "solve") {
        if (hopscotchGame->phase == HopscotchPhase::Attract) {
            startHopscotchGame();
        }
        hopscotchGame->phase = HopscotchPhase::PlayerTurn;
        hopscotchGame->playerPosition = 0;
        hopscotchGame->deadline = sceneElapsedNow() + 10.0;
        const auto level = hopscotchGame->levels[
            static_cast<std::size_t>(hopscotchGame->level)];
        const int length = level.startingLength + hopscotchGame->round;
        for (int index = 0; index < length; ++index) {
            pressHopscotchAt(0, 0, 3000U +
                static_cast<std::uint32_t>(hopscotchGame->masterSequence[
                    static_cast<std::size_t>(index)]));
        }
    } else if (control == "wrong") {
        hopscotchGame->phase = HopscotchPhase::PlayerTurn;
        hopscotchGame->playerPosition = 0;
        hopscotchGame->deadline = sceneElapsedNow() + 10.0;
        const int expected = hopscotchGame->masterSequence.front();
        const auto level = hopscotchGame->levels[
            static_cast<std::size_t>(hopscotchGame->level)];
        static constexpr std::array<int, 4> tileCounts{6, 9, 9, 12};
        const int count = std::min(level.availableTiles,
            tileCounts[static_cast<std::size_t>(level.board)]);
        pressHopscotchAt(0, 0, 3000U +
            static_cast<std::uint32_t>((expected + 1) % count));
    } else if (control == "hint") {
        demonstrateHopscotch(true);
    } else if (control == "timeout") {
        hopscotchGame->phase = HopscotchPhase::PlayerTurn;
        hopscotchGame->deadline = sceneElapsedNow() - 1.0;
    } else if (control == "status") {
        std::cout << "Hopscotch status: level "
                  << hopscotchGame->level + 1 << ", round "
                  << hopscotchGame->round + 1 << ", score "
                  << hopscotchGame->score << ", sequence "
                  << hopscotchGame->masterSequence.size()
                  << ", position " << hopscotchGame->playerPosition
                  << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Hopscotch control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
