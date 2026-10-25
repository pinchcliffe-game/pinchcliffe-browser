#include "game/app_sockdrawer.h"
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

enum SockAnimation : std::size_t {
    Start,
    Points,
    DrawerOpen,
    DrawerClose,
    AllOpen,
    AllClose,
    OneOpen,
    OneClose,
    TimerRotate,
    TimerMiddleStart,
    TimerMiddleRunning,
    TimerMiddleEnd,
};

struct SockLevel {
    int activeSocks;
    int sockVariety;
    double seconds;
};

// FGP.exe 0x0049ef48: active drawers, available designs, milliseconds.
// Level five contains an obvious extra zero (950000); 95 seconds preserves
// the progression between the neighboring 75- and 110-second levels.
const auto kLevels = originalArray<SockLevel, 15>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049ef48U + static_cast<std::uint32_t>(index * 12);
    // Preserve the existing level-five timer correction.
    const double divisor = index == 4 ? 10000.0 : 1000.0;
    return SockLevel{source.read<int>(address), source.read<int>(address + 4), source.read<int>(address + 8) / divisor};
});

// The similarity order at 0x0049effc. Later levels add the more easily
// confused sock designs from the tail of this table.
const auto kSockOrder = originalArray<int, 24>(0x0049effcU);

TexturePtr makeSockAnimationTexture(SDL_Renderer *renderer,
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

std::string numbered(std::string_view stem, int number) {
    std::ostringstream result;
    result << stem << std::setw(2) << std::setfill('0') << number;
    return result.str();
}

} // namespace

SockdrawerScene::SockdrawerGame SockdrawerScene::loadSockdrawerGame(Package &package) {
    SockdrawerGame game;

    for (std::size_t index = 0; index < game.drawers.size(); ++index) {
        const int column = static_cast<int>(index % 5U);
        const int row = static_cast<int>(index / 5U);
        const int x = 189 + column * 114;
        const int y = 118 + row * 114;
        game.drawers[index].bounds = {x, y, 108, 108};
        game.drawers[index].sockBounds = {x + 15, y + 13, 85, 88};
    }

    const auto loadKeyedTexture = [&](std::string_view asset) {
        SurfacePtr surface =
            loadGreenKeyedBmp(package.data(package.find(asset)));
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        return texture;
    };
    for (std::size_t index = 0; index < game.sockTextures.size(); ++index) {
        game.sockTextures[index] = loadKeyedTexture(
            "bitmap\\" + numbered("Sock", static_cast<int>(index + 1U)) +
            ".bmp");
    }
    for (std::size_t index = 0; index < game.timerTopTextures.size(); ++index) {
        game.timerTopTextures[index] = loadKeyedTexture(
            "bitmap\\" + numbered("timertop", static_cast<int>(index)) +
            ".bmp");
        game.timerBottomTextures[index] = loadKeyedTexture(
            "bitmap\\" + numbered("timerbottom",
                                    static_cast<int>(index + 2U)) +
            ".bmp");
    }

    constexpr std::array<std::string_view, 12> animationNames{{
        "start", "points", "drawer1open", "drawer1close",
        "allopen", "allclose", "oneopen", "oneclose",
        "timerrotate", "timermiddlestart", "timermiddlerunning",
        "timermiddleend",
    }};
    for (std::size_t index = 0; index < animationNames.size(); ++index) {
        auto &animation = game.animations[index];
        // drawer1open/close are complete 108x108 drawer images. Their gold
        // handles use palette index zero as visible colour; treating that
        // index as alpha reveals red/navy pixels from the backdrop below.
        const auto indexZeroAlpha =
            index == DrawerOpen || index == DrawerClose
                ? SmackerIndexZeroAlpha::Opaque
                : SmackerIndexZeroAlpha::Transparent;
        animation.media = decodeSmacker(
            activeSceneAnimationData(std::string(animationNames[index])),
            true, true, true, 0, nullptr, indexZeroAlpha);
        animation.texture =
            makeSockAnimationTexture(renderer, animation.media);
    }

    std::cout << "Loaded executable-derived Sock Drawer: 20 drawers, 24 "
                 "designs, 15 levels, and 63 hourglass frames\n";
    return game;
}

void SockdrawerScene::playSockdrawerEffect(std::size_t animation) {
    if (!sockdrawerGame || animation >= sockdrawerGame->animations.size()) {
        return;
    }
    audioMixer.setEffect(sockdrawerGame->animations[animation].media,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void SockdrawerScene::startSockdrawerGame() {
    if (!sockdrawerGame) {
        return;
    }
    auto &game = *sockdrawerGame;
    game.points = 0;
    game.finalScore = 0;
    game.level = 0;
    game.activeSocks = 0;
    game.firstSelection.reset();
    game.secondSelection.reset();
    game.pointsAnimationPlaying = false;
    game.gameOverDialogShown = false;
    game.phase = SockdrawerPhase::ClosingBoard;
    game.phaseStartedAt = sceneElapsedNow();
    for (auto &drawer : game.drawers) {
        drawer.active = false;
        drawer.sock = -1;
        drawer.state = SockdrawerDrawerState::Closing;
        drawer.transitionStarted = game.phaseStartedAt;
    }
    playSockdrawerEffect(AllClose);
    SDL_SetWindowTitle(window, "FGP Native — Sokkeskapet — level 1");
}

void SockdrawerScene::returnToSockdrawerAttract() {
    if (!sockdrawerGame) {
        return;
    }
    auto &game = *sockdrawerGame;
    const double now = sceneElapsedNow();
    for (auto &drawer : game.drawers) {
        if (drawer.state != SockdrawerDrawerState::Closed) {
            drawer.state = SockdrawerDrawerState::Closing;
            drawer.transitionStarted = now;
        }
    }
    game.firstSelection.reset();
    game.secondSelection.reset();
    game.phase = SockdrawerPhase::Attract;
    game.phaseStartedAt = now;
    game.finalScore = 0;
    game.gameOverDialogShown = false;
    playSockdrawerEffect(AllClose);
    referenceTrace.emit("animation_add", {
        {"asset", "start"}, {"x", -1}, {"y", -1},
        {"layer", -10}, {"hotspot", 3}, {"conflict_mask", 0}});
}

void SockdrawerScene::dealSockdrawerBoard(double elapsed) {
    if (!sockdrawerGame) {
        return;
    }
    auto &game = *sockdrawerGame;
    const auto &level = kLevels[static_cast<std::size_t>(game.level)];
    game.activeSocks = level.activeSocks;
    game.timeLimit = level.seconds;
    game.firstSelection.reset();
    game.secondSelection.reset();

    std::array<std::size_t, 20> positions{};
    std::iota(positions.begin(), positions.end(), 0U);
    std::shuffle(positions.begin(), positions.end(), randomEngine);
    std::vector<int> designs(kSockOrder.begin(),
                             kSockOrder.begin() + level.sockVariety);
    std::shuffle(designs.begin(), designs.end(), randomEngine);

    for (auto &drawer : game.drawers) {
        drawer.active = false;
        drawer.sock = -1;
        drawer.state = SockdrawerDrawerState::Opening;
        drawer.transitionStarted = elapsed;
    }
    for (int pair = 0; pair < level.activeSocks / 2; ++pair) {
        for (int copy = 0; copy < 2; ++copy) {
            auto &drawer = game.drawers[positions[static_cast<std::size_t>(
                pair * 2 + copy)]];
            drawer.active = true;
            drawer.sock = designs[static_cast<std::size_t>(pair)];
            drawer.state = SockdrawerDrawerState::Closed;
        }
    }

    game.phase = SockdrawerPhase::OpeningBoard;
    game.phaseStartedAt = elapsed;
    playSockdrawerEffect(AllOpen);
}

bool SockdrawerScene::pressSockdrawerDrawer(std::size_t index) {
    if (!sockdrawerGame || index >= sockdrawerGame->drawers.size()) {
        return false;
    }
    auto &game = *sockdrawerGame;
    auto &drawer = game.drawers[index];
    if (game.phase != SockdrawerPhase::Playing || !drawer.active ||
        drawer.state != SockdrawerDrawerState::Closed) {
        return false;
    }

    const double now = sceneElapsedNow();
    drawer.state = SockdrawerDrawerState::Opening;
    drawer.transitionStarted = now;
    playSockdrawerEffect(OneOpen);
    if (!game.firstSelection) {
        game.firstSelection = index;
    } else {
        game.secondSelection = index;
        game.phase = SockdrawerPhase::Resolving;
        game.resolveAt =
            now + game.animations[DrawerOpen].media.videoDuration + 0.4;
    }
    return true;
}

void SockdrawerScene::resolveSockdrawerPair() {
    if (!sockdrawerGame || !sockdrawerGame->firstSelection ||
        !sockdrawerGame->secondSelection) {
        return;
    }
    auto &game = *sockdrawerGame;
    auto &first = game.drawers[*game.firstSelection];
    auto &second = game.drawers[*game.secondSelection];
    const double now = sceneElapsedNow();

    if (first.sock != second.sock) {
        first.state = SockdrawerDrawerState::Closing;
        second.state = SockdrawerDrawerState::Closing;
        first.transitionStarted = now;
        second.transitionStarted = now;
        game.phase = SockdrawerPhase::ClosingMismatch;
        game.phaseStartedAt = now;
        playSockdrawerEffect(OneClose);
        return;
    }

    first.state = SockdrawerDrawerState::Matched;
    second.state = SockdrawerDrawerState::Matched;
    game.firstSelection.reset();
    game.secondSelection.reset();
    game.points += 25;
    game.pointsAnimationPlaying = true;
    game.pointsAnimationStarted = now;
    playSockdrawerEffect(Points);

    const bool complete = std::none_of(
        game.drawers.begin(), game.drawers.end(),
        [](const SockdrawerDrawer &drawer) {
            return drawer.active &&
                   drawer.state != SockdrawerDrawerState::Matched;
        });
    if (!complete) {
        game.phase = SockdrawerPhase::Playing;
        return;
    }

    const double remaining = std::max(
        0.0, game.timeLimit - (now - game.levelStartedAt));
    game.points += static_cast<int>(std::lround(
        game.level * 200.0 * remaining / game.timeLimit));
    game.level = std::min(game.level + 1,
                          static_cast<int>(kLevels.size() - 1U));
    game.phase = SockdrawerPhase::ClosingBoard;
    game.phaseStartedAt = now;
    for (auto &drawer : game.drawers) {
        drawer.state = SockdrawerDrawerState::Closing;
        drawer.transitionStarted = now;
    }
    playSockdrawerEffect(AllClose);
    SDL_SetWindowTitle(
        window,
        ("FGP Native — Sokkeskapet — level " +
         std::to_string(game.level + 1) + " · " +
         std::to_string(game.points) + " points")
            .c_str());
}

void SockdrawerScene::updateSockdrawer(double elapsed) {
    if (!sockdrawerGame) {
        return;
    }
    auto &game = *sockdrawerGame;
    const double openDuration =
        game.animations[DrawerOpen].media.videoDuration;
    const double closeDuration =
        game.animations[DrawerClose].media.videoDuration;

    for (auto &drawer : game.drawers) {
        if (drawer.state == SockdrawerDrawerState::Opening &&
            elapsed - drawer.transitionStarted >= openDuration) {
            drawer.state = SockdrawerDrawerState::Open;
        } else if (drawer.state == SockdrawerDrawerState::Closing &&
                   elapsed - drawer.transitionStarted >= closeDuration) {
            drawer.state = SockdrawerDrawerState::Closed;
        }
    }

    if ((game.phase == SockdrawerPhase::Playing ||
         game.phase == SockdrawerPhase::Resolving ||
         game.phase == SockdrawerPhase::ClosingMismatch) &&
        elapsed - game.levelStartedAt >= game.timeLimit) {
        game.finalScore = game.points;
        game.phase = SockdrawerPhase::GameOver;
        game.phaseStartedAt = elapsed;
        game.gameOverDialogShown = false;
        game.firstSelection.reset();
        game.secondSelection.reset();
        playSockdrawerEffect(TimerMiddleEnd);
        return;
    }

    switch (game.phase) {
    case SockdrawerPhase::ClosingBoard:
        if (elapsed - game.phaseStartedAt >=
            game.animations[AllClose].media.videoDuration) {
            dealSockdrawerBoard(elapsed);
        }
        break;
    case SockdrawerPhase::OpeningBoard:
        if (elapsed - game.phaseStartedAt >= openDuration) {
            for (auto &drawer : game.drawers) {
                if (drawer.state == SockdrawerDrawerState::Opening) {
                    drawer.state = SockdrawerDrawerState::Open;
                }
            }
            game.phase = SockdrawerPhase::TurningTimer;
            game.phaseStartedAt = elapsed;
            playSockdrawerEffect(TimerRotate);
        }
        break;
    case SockdrawerPhase::TurningTimer:
        if (elapsed - game.phaseStartedAt >=
            game.animations[TimerRotate].media.videoDuration) {
            game.phase = SockdrawerPhase::Playing;
            game.phaseStartedAt = elapsed;
            game.levelStartedAt = elapsed;
            playSockdrawerEffect(TimerMiddleStart);
        }
        break;
    case SockdrawerPhase::Resolving:
        if (elapsed >= game.resolveAt) {
            resolveSockdrawerPair();
        }
        break;
    case SockdrawerPhase::ClosingMismatch:
        if (elapsed - game.phaseStartedAt >= closeDuration) {
            game.firstSelection.reset();
            game.secondSelection.reset();
            game.phase = SockdrawerPhase::Playing;
        }
        break;
    case SockdrawerPhase::GameOver:
        // FGP.exe starts `timermiddleend` at 0x0044C5A3-0x0044C5B8.
        // Its completion reaches 0x0044C1CC-0x0044C1DD and only then calls
        // the terminal modal/score routine at 0x0044C380. Keep that visible
        // ordering instead of opening the modal at the instant time expires.
        if (!game.gameOverDialogShown &&
            elapsed - game.phaseStartedAt >=
                game.animations[TimerMiddleEnd].media.videoDuration) {
            game.gameOverDialogShown = true;
            showMessageWithTitle(
                language.get("interfaceh", "GAMEOVER",
                             "Spillet er slutt"),
                language.get("sockdrawer", "ENDMESSAGE",
                             "Oops! Der gikk tiden ut."),
                MessageKind::SockdrawerGameOver);
        }
        break;
    case SockdrawerPhase::Attract:
    case SockdrawerPhase::Playing:
        break;
    }

    if (game.pointsAnimationPlaying &&
        elapsed - game.pointsAnimationStarted >=
            game.animations[Points].media.videoDuration) {
        game.pointsAnimationPlaying = false;
    }
}

void SockdrawerScene::renderSockdrawer(double elapsed) {
    if (!sockdrawerGame) {
        return;
    }
    auto &game = *sockdrawerGame;

    const auto drawAnimation = [&](std::size_t animationIndex,
                                   double localTime, bool loop,
                                   const SDL_Rect &destination) {
        auto &animation = game.animations[animationIndex];
        if (!animation.texture || animation.media.videoFrames.empty()) {
            return;
        }
        const std::size_t index =
            frameIndexAt(animation.media, std::max(0.0, localTime), loop);
        const auto &frame = animation.media.videoFrames[index];
        uploadVideoFrame(animation.texture.get(), frame);
        SDL_RenderCopy(renderer, animation.texture.get(), nullptr,
                       &destination);
    };
    const auto lastFrameTime = [&](std::size_t animationIndex) {
        const auto &media = game.animations[animationIndex].media;
        return std::max(0.0, media.videoDuration - 0.0001);
    };

    // 0x0044b73a loads Amerigo BT_18_, then 0x0044b74c-0x0044b768
    // creates the active-scene title in the rectangle at 0x0049f060.
    // Keep it visible under modal overlays as in the original capture.
    drawCenteredText(renderer, menuTitleFont,
                     language.get("sockdrawer", "SCENENAME",
                                  "Sokkeskapet"),
                     SDL_Rect{335, 44, 266, 42});

    for (const auto &drawer : game.drawers) {
        if (drawer.active && drawer.sock >= 0 &&
            drawer.state != SockdrawerDrawerState::Matched) {
            SDL_RenderCopy(
                renderer,
                game.sockTextures[static_cast<std::size_t>(drawer.sock)].get(),
                nullptr, &drawer.sockBounds);
        }
    }
    for (const auto &drawer : game.drawers) {
        switch (drawer.state) {
        case SockdrawerDrawerState::Closed:
            // Init at 0x0044b86d sets the drawer state to one. The selector
            // at 0x0044b887 therefore passes zero to 0x0044b950, which names
            // the descriptor drawer%dopen. Before it starts, frame zero is
            // the original closed-drawer image.
            drawAnimation(DrawerOpen, 0.0, false, drawer.bounds);
            break;
        case SockdrawerDrawerState::Opening:
            drawAnimation(DrawerOpen, elapsed - drawer.transitionStarted,
                          false, drawer.bounds);
            break;
        case SockdrawerDrawerState::Open:
        case SockdrawerDrawerState::Matched:
            drawAnimation(DrawerOpen, lastFrameTime(DrawerOpen), false,
                          drawer.bounds);
            break;
        case SockdrawerDrawerState::Closing:
            drawAnimation(DrawerClose, elapsed - drawer.transitionStarted,
                          false, drawer.bounds);
            break;
        }
    }

    if (game.phase == SockdrawerPhase::Attract) {
        drawAnimation(Start, 0.0, false, SDL_Rect{33, 172, 108, 120});
        // 0x0044b9da passes index 30 to 0x0044ba10, which adds two and names
        // timerbottom32; 0x0044b9ec adds it at layer -20 during scene init.
        SDL_Rect bottom{46, 473, 84, 40};
        SDL_RenderCopy(renderer, game.timerBottomTextures.back().get(),
                       nullptr, &bottom);
    } else {
        const double pointsTime = game.pointsAnimationPlaying
                                      ? elapsed - game.pointsAnimationStarted
                                      : lastFrameTime(Points);
        drawAnimation(Points, pointsTime, false,
                      SDL_Rect{33, 172, 108, 120});
        drawCenteredText(renderer, menuTitleFont, std::to_string(game.points),
                         SDL_Rect{49, 184, 76, 29});
    }

    if (game.phase == SockdrawerPhase::TurningTimer) {
        drawAnimation(TimerRotate, elapsed - game.phaseStartedAt, false,
                      SDL_Rect{0, 335, 180, 200});
    } else if (game.phase != SockdrawerPhase::Attract) {
        const double used = std::clamp(
            elapsed - game.levelStartedAt, 0.0, game.timeLimit);
        const std::size_t timerFrame = static_cast<std::size_t>(std::clamp(
            static_cast<int>(std::floor(used * 30.0 / game.timeLimit)),
            0, 30));
        SDL_Rect top{45, 359, 84, 60};
        SDL_Rect bottom{46, 473, 84, 40};
        SDL_RenderCopy(renderer, game.timerTopTextures[timerFrame].get(),
                       nullptr, &top);
        SDL_RenderCopy(renderer, game.timerBottomTextures[timerFrame].get(),
                       nullptr, &bottom);

        if (game.phase == SockdrawerPhase::GameOver) {
            drawAnimation(TimerMiddleEnd, elapsed - game.phaseStartedAt,
                          false, SDL_Rect{72, 404, 32, 80});
        } else if (game.phase == SockdrawerPhase::Playing ||
                   game.phase == SockdrawerPhase::Resolving ||
                   game.phase == SockdrawerPhase::ClosingMismatch) {
            const double levelTime = elapsed - game.levelStartedAt;
            const double startDuration =
                game.animations[TimerMiddleStart].media.videoDuration;
            if (levelTime < startDuration) {
                drawAnimation(TimerMiddleStart, levelTime, false,
                              SDL_Rect{72, 410, 32, 96});
            } else {
                drawAnimation(TimerMiddleRunning,
                              levelTime - startDuration, true,
                              SDL_Rect{78, 412, 20, 92});
            }
        }
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
SockdrawerScene::sockdrawerHotspotAt(int x, int y) const {
    if (!sockdrawerGame || activeScene != "sockdrawer") {
        return std::nullopt;
    }
    const auto &game = *sockdrawerGame;
    const SDL_Point point{x, y};
    if (game.phase == SockdrawerPhase::Attract) {
        const SDL_Rect startBounds{33, 172, 108, 120};
        if (SDL_PointInRect(&point, &startBounds)) {
            return std::pair{3U, startBounds};
        }
        return std::nullopt;
    }
    if (game.phase != SockdrawerPhase::Playing) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < game.drawers.size(); ++index) {
        const auto &drawer = game.drawers[index];
        const SDL_Rect hotspot{drawer.bounds.x + 3, drawer.bounds.y + 3,
                               102, 103};
        if (drawer.active &&
            drawer.state == SockdrawerDrawerState::Closed &&
            SDL_PointInRect(&point, &hotspot)) {
            return std::pair{static_cast<std::uint32_t>(101U + index),
                             hotspot};
        }
    }
    return std::nullopt;
}

bool SockdrawerScene::executeSockdrawerDeveloperCommand(std::string_view command) {
    constexpr std::string_view sockdrawerControlPrefix = "sockdrawer:";
    if (activeScene != "sockdrawer" || !sockdrawerGame) {
        return false;
    }
    const std::string_view control =
        command.substr(sockdrawerControlPrefix.size());
    if (control == "start") {
        startSockdrawerGame();
    } else if (control == "fixture-deal") {
        // Deterministic capture fixture: the original x32dbg adapter pins
        // the two real selector returns so drawers 0-3 contain designs
        // 0,0,1,1. Production dealing remains random.
        const double now = sceneElapsedNow();
        startSockdrawerGame();
        dealSockdrawerBoard(now);
        for (std::size_t index = 0;
             index < sockdrawerGame->drawers.size(); ++index) {
            auto &drawer = sockdrawerGame->drawers[index];
            drawer.active = index < 4U;
            drawer.sock = index < 4U
                              ? static_cast<int>(index / 2U)
                              : -1;
            // The original `allopen` setup leaves unused drawers open
            // (red interior visible) and closes only the dealt drawers.
            drawer.state = index < 4U
                               ? SockdrawerDrawerState::Closed
                               : SockdrawerDrawerState::Open;
            drawer.transitionStarted = now;
        }
        sockdrawerGame->activeSocks = 4;
        sockdrawerGame->phase = SockdrawerPhase::Playing;
        sockdrawerGame->phaseStartedAt = now;
        sockdrawerGame->levelStartedAt = now;
    } else if (control.rfind("set-score=", 0) == 0) {
        sockdrawerGame->points = std::max(
            0, std::stoi(std::string(control.substr(10))));
    } else if (control.rfind("set-final-score=", 0) == 0) {
        // Capture-only boundary equivalent to the original adapter's
        // write at 0x0044C3B7: affect the shared score handoff without
        // changing the live points display behind the terminal dialogs.
        sockdrawerGame->finalScore = std::max(
            0, std::stoi(std::string(control.substr(16))));
    } else if (control.rfind("open:", 0) == 0) {
        const auto index = static_cast<std::size_t>(std::stoul(
            std::string(control.substr(5))));
        if (!pressSockdrawerDrawer(index)) {
            return false;
        }
    } else if (control == "match" || control == "mismatch" ||
               control == "complete-level") {
        if (sockdrawerGame->phase == SockdrawerPhase::Attract ||
            sockdrawerGame->phase == SockdrawerPhase::GameOver) {
            startSockdrawerGame();
            const double now = sceneElapsedNow();
            dealSockdrawerBoard(now);
            for (auto &drawer : sockdrawerGame->drawers) {
                if (drawer.state == SockdrawerDrawerState::Opening) {
                    drawer.state = SockdrawerDrawerState::Open;
                }
            }
            sockdrawerGame->phase = SockdrawerPhase::Playing;
            sockdrawerGame->phaseStartedAt = now;
            sockdrawerGame->levelStartedAt = now;
        }
        if (sockdrawerGame->phase != SockdrawerPhase::Playing) {
            return false;
        }
        std::optional<std::pair<std::size_t, std::size_t>> pair;
        for (std::size_t first = 0;
             first < sockdrawerGame->drawers.size() && !pair; ++first) {
            const auto &left = sockdrawerGame->drawers[first];
            if (!left.active ||
                left.state != SockdrawerDrawerState::Closed) {
                continue;
            }
            for (std::size_t second = first + 1;
                 second < sockdrawerGame->drawers.size(); ++second) {
                const auto &right = sockdrawerGame->drawers[second];
                if (!right.active ||
                    right.state != SockdrawerDrawerState::Closed) {
                    continue;
                }
                const bool same = left.sock == right.sock;
                if (same == (control != "mismatch")) {
                    pair = std::pair{first, second};
                    break;
                }
            }
        }
        if (!pair) {
            return false;
        }
        if (control == "complete-level") {
            for (std::size_t index = 0;
                 index < sockdrawerGame->drawers.size(); ++index) {
                auto &drawer = sockdrawerGame->drawers[index];
                if (drawer.active && index != pair->first &&
                    index != pair->second) {
                    drawer.state = SockdrawerDrawerState::Matched;
                }
            }
        }
        if (!pressSockdrawerDrawer(pair->first) ||
            !pressSockdrawerDrawer(pair->second)) {
            return false;
        }
        resolveSockdrawerPair();
    } else if (control == "timeout") {
        if (sockdrawerGame->phase != SockdrawerPhase::Playing) {
            return false;
        }
        const double now = sceneElapsedNow();
        sockdrawerGame->levelStartedAt =
            now - sockdrawerGame->timeLimit;
        updateSockdrawer(now);
    } else if (control == "status") {
        const auto remaining = std::count_if(
            sockdrawerGame->drawers.begin(),
            sockdrawerGame->drawers.end(),
            [](const SockdrawerDrawer &drawer) {
                return drawer.active &&
                       drawer.state != SockdrawerDrawerState::Matched;
            });
        std::cout << "Sockdrawer status: level "
                  << sockdrawerGame->level + 1 << ", "
                  << remaining << '/' << sockdrawerGame->activeSocks
                  << " socks remaining, " << sockdrawerGame->points
                  << " points\n";
    } else {
        throw std::runtime_error(
            "Unknown developer Sockdrawer control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
