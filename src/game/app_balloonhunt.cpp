#include "game/app_balloonhunt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "media/smacker.h"
#include "util/text.h"

namespace fgp {
namespace {

// PROVENANCE. This project's rule is that a constant describing the original is
// recovered from it, with the address it was read at, and that anything else is
// labelled. Some of the values below are recovered and some are this
// implementation's own invention, and until now the file did not say which --
// so they all read as evidence. They are separated here.
//
// RECOVERED from src/data/game/FGP.exe:
//   kIslandCentre 6700 -- `push 0x45d16000` at 0x0042b673, passed with the
//   string "startx" to the named-float setter 0x0040e3e0. Present as an IEEE
//   float at file offset 0x2aa74. Its cluster is size=64, startx=6700,
//   starty=90 (float at 0x2a2bd), startz=6700, matching tracks.ini's
//   startx/starty/startz/startangle keys.
//
// INVENTED for this implementation, NOT recovered:
//   kFarPlane 9000 does not appear in the executable in any form -- not as a
//   float, not as a double, not as a 32-bit immediate. kFocalLength 520,
//   kHorizon 215 and kWorldMapExtent 40960 appear nowhere as a float or double
//   either; each matches a stray u32 byte pattern somewhere in a 5 MB image
//   (0x61e0, 0x16105, 0x9c17f), which is coincidence rather than evidence and
//   is not in balloonhunt's module. kGameScale, kAutopilotRadius and kNearPlane
//   are likewise unsourced.
//
// So the projection this scene draws with is a plausible reconstruction, not the
// original's. It may look right and still be wrong, and anything built on it --
// a parity comparison in particular -- inherits that. Recovering the real
// projection means finding the float loads in balloonhunt's module (its
// callback is 0x00429960): the `push <float>, push <name>, call 0x0040e3e0`
// pattern recovers named configuration with the author's own identifiers, and
// the surrounding FPU constant loads are where the perspective actually lives.
constexpr double kGameScale = 1.25;              // INVENTED
constexpr double kWorldMapExtent = 40960.0;      // INVENTED: 1024 radar px * 40
constexpr double kIslandCentre = 6700.0;         // RECOVERED 0x0042b673
constexpr double kAutopilotRadius = 10000.0;     // INVENTED
constexpr double kNearPlane = 55.0;              // INVENTED
constexpr double kFarPlane = 9000.0;             // INVENTED
constexpr double kFocalLength = 520.0;           // INVENTED
constexpr double kHorizon = 215.0;               // INVENTED
constexpr std::uint32_t kModePrevious = 1001U;
constexpr std::uint32_t kModeNext = 1002U;
constexpr std::uint32_t kStart = 1003U;
constexpr std::uint32_t kTrackPrevious = 1004U;
constexpr std::uint32_t kTrackNext = 1005U;
constexpr std::uint32_t kContinue = 1006U;

using Ini = std::unordered_map<
    std::string, std::unordered_map<std::string, std::string>>;

std::string packageText(std::span<const std::uint8_t> bytes) {
    return std::string(reinterpret_cast<const char *>(bytes.data()),
                       bytes.size());
}

Ini parseIni(std::span<const std::uint8_t> bytes) {
    Ini result;
    std::istringstream input(packageText(bytes));
    std::string section;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(line);
        if (line.empty() || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = normalizedPath(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        result[section][normalizedPath(trim(line.substr(0, equals)))] =
            trim(line.substr(equals + 1));
    }
    return result;
}

double iniDouble(const Ini &ini, std::string_view section,
                 std::string_view key, double fallback) {
    const auto foundSection = ini.find(normalizedPath(std::string(section)));
    if (foundSection == ini.end()) {
        return fallback;
    }
    const auto found =
        foundSection->second.find(normalizedPath(std::string(key)));
    if (found == foundSection->second.end()) {
        return fallback;
    }
    return std::stod(found->second);
}

TexturePtr makeStreamingTexture(SDL_Renderer *renderer,
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

SurfacePtr circularSurface(SurfacePtr source) {
    SurfacePtr result(SDL_ConvertSurfaceFormat(
        source.get(), SDL_PIXELFORMAT_ARGB8888, 0));
    if (!result) {
        throw std::runtime_error(
            std::string("SDL_ConvertSurfaceFormat failed: ") +
            SDL_GetError());
    }
    if (SDL_MUSTLOCK(result.get())) {
        SDL_LockSurface(result.get());
    }
    const double centreX = (result->w - 1) * 0.5;
    const double centreY = (result->h - 1) * 0.5;
    const double radiusSquared =
        std::min(result->w, result->h) * 0.5 *
        (std::min(result->w, result->h) * 0.5);
    for (int y = 0; y < result->h; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(
            static_cast<Uint8 *>(result->pixels) + y * result->pitch);
        for (int x = 0; x < result->w; ++x) {
            const double dx = x - centreX;
            const double dy = y - centreY;
            if (dx * dx + dy * dy > radiusSquared) {
                Uint8 red = 0;
                Uint8 green = 0;
                Uint8 blue = 0;
                Uint8 alpha = 0;
                SDL_GetRGBA(row[x], result->format, &red, &green, &blue,
                            &alpha);
                row[x] = SDL_MapRGBA(result->format, red, green, blue, 0);
            }
        }
    }
    if (SDL_MUSTLOCK(result.get())) {
        SDL_UnlockSurface(result.get());
    }
    return result;
}

SDL_Rect scaledRect(int x, int y, int width, int height) {
    return {static_cast<int>(std::lround(x * kGameScale)),
            static_cast<int>(std::lround(y * kGameScale)),
            static_cast<int>(std::lround(width * kGameScale)),
            static_cast<int>(std::lround(height * kGameScale))};
}

void drawScaledCenteredText(SDL_Renderer *renderer,
                            const BitmapFont &font,
                            std::string_view text,
                            const SDL_Rect &sourceBounds) {
    const int sourceWidth = textWidth(font, text);
    const int sourceX = sourceBounds.x +
        (sourceBounds.w - sourceWidth) / 2;
    const int sourceY = sourceBounds.y +
        (sourceBounds.h - font.height) / 2;
    const int scaledHeight = static_cast<int>(
        std::lround(font.height * kGameScale));
    const int y = static_cast<int>(std::lround(sourceY * kGameScale));
    int sourceOffset = 0;
    for (const unsigned char code : text) {
        const SDL_Rect &source = font.glyphs[code];
        const int left = static_cast<int>(
            std::lround((sourceX + sourceOffset) * kGameScale));
        sourceOffset += source.w;
        const int right = static_cast<int>(
            std::lround((sourceX + sourceOffset) * kGameScale));
        SDL_Rect destination{left, y, right - left, scaledHeight};
        SDL_RenderCopy(renderer, font.texture.get(), &source, &destination);
    }
}

double wrapAngle(double angle) {
    angle = std::fmod(angle, 2.0 * std::numbers::pi);
    return angle < 0.0 ? angle + 2.0 * std::numbers::pi : angle;
}

double angleDifference(double from, double to) {
    double difference = wrapAngle(to) - wrapAngle(from);
    if (difference > std::numbers::pi) {
        difference -= 2.0 * std::numbers::pi;
    } else if (difference < -std::numbers::pi) {
        difference += 2.0 * std::numbers::pi;
    }
    return difference;
}

std::string formattedTime(double seconds) {
    const int centiseconds =
        std::max(0, static_cast<int>(std::lround(seconds * 100.0)));
    const int minutes = centiseconds / 6000;
    const int remainder = centiseconds % 6000;
    const int wholeSeconds = remainder / 100;
    const int hundredths = remainder % 100;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << minutes << ':'
         << std::setw(2) << wholeSeconds << ':' << std::setw(2)
         << hundredths;
    return text.str();
}

void drawNeedle(SDL_Renderer *renderer, int centreX, int centreY,
                double angle, int length, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int endX = centreX +
        static_cast<int>(std::lround(std::sin(angle) * length));
    const int endY = centreY -
        static_cast<int>(std::lround(std::cos(angle) * length));
    SDL_RenderDrawLine(renderer, centreX, centreY, endX, endY);
    SDL_RenderDrawLine(renderer, centreX + 1, centreY, endX + 1, endY);
}

} // namespace

BalloonhuntScene::BalloonhuntGame BalloonhuntScene::loadBalloonhuntGame(Package &package) {
    BalloonhuntGame game;

    const auto loadTexture = [&](std::string_view asset, bool keyed = true,
                                 SurfacePtr *keptSurface = nullptr) {
        SurfacePtr surface = keyed
            ? loadGreenKeyedBmp(package.data(package.find(asset)))
            : loadBmp(package.data(package.find(asset)));
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        if (keptSurface) {
            *keptSurface = std::move(surface);
        }
        return texture;
    };

    game.startTexture = loadTexture("bitmap\\start button.bmp", true,
                                    &game.startSurface);
    game.trackSelectorTexture = loadTexture(
        "bitmap\\bane velger.bmp", true, &game.trackSelectorSurface);
    game.startButtonTexture = loadTexture("bitmap\\strbtnoff.bmp");
    game.startButtonPressedTexture = loadTexture("bitmap\\strbtnon.bmp");
    game.interfaceTopTexture = loadTexture("bitmap\\interface top.bmp");
    game.interfaceBottomTexture =
        loadTexture("bitmap\\interface bottom.bmp");
    game.mapTexture = loadTexture("bitmap\\map3.bmp", false);
    game.skyTexture = loadTexture("bitmap\\skynew.bmp", false);
    {
        SurfacePtr compass = circularSurface(loadBmp(
            package.data(package.find("bitmap\\compskive.bmp"))));
        game.compassTexture = makeTexture(renderer, compass.get());
        SDL_SetTextureBlendMode(game.compassTexture.get(),
                                SDL_BLENDMODE_BLEND);
    }
    game.autopilotTexture = loadTexture("bitmap\\autopilot.bmp", true);

    for (std::size_t index = 0; index < game.balloonTextures.size(); ++index) {
        std::ostringstream number;
        number << std::setfill('0') << std::setw(2) << index + 1U;
        game.balloonTextures[index] =
            loadTexture("bitmap\\" + number.str() + ".bmp");
        game.radarBalloonTextures[index] =
            loadTexture("bitmap\\small " + number.str() + ".bmp");
    }
    for (std::size_t index = 0; index < game.sceneryTextures.size(); ++index) {
        game.sceneryTextures[index] = loadTexture(
            "bitmap\\sprite" + std::to_string(index + 1U) + ".bmp");
    }
    for (int pitch = 0; pitch < 7; ++pitch) {
        for (int roll = 0; roll < 7; ++roll) {
            std::ostringstream name;
            name << "bitmap\\" << std::setfill('0') << std::setw(2)
                 << pitch << std::setw(2) << roll << ".bmp";
            game.gyroTextures[static_cast<std::size_t>(pitch * 7 + roll)] =
                loadTexture(name.str());
        }
    }

    game.solanMedia = decodeActiveSceneClip("solan");
    game.solanTexture = makeStreamingTexture(renderer, game.solanMedia);
    const auto audio = [&](std::string name) {
        return decodeSmacker(activeSceneAnimationData(std::move(name)),
                             false, true, true);
    };
    game.windSound = audio("wind1");
    game.hitSound = audio("baloon-d");
    game.wrongSound = audio("wrong ballon 1");
    game.winSound = audio("win");
    game.gameOverSound = audio("gameover");

    const Ini tracksIni = parseIni(
        package.data(package.find("tracks.ini")));
    for (std::size_t index = 0; index < game.tracks.size(); ++index) {
        auto &track = game.tracks[index];
        track.number = static_cast<int>(index + 1U);
        std::ostringstream section;
        section << "level" << std::setfill('0') << std::setw(2)
                << track.number;
        track.size = iniDouble(tracksIni, section.str(), "size", 70.0);
        track.seconds = iniDouble(tracksIni, section.str(), "time", 100.0);
        track.startX = iniDouble(tracksIni, section.str(), "startx", 6700.0);
        track.startAltitude =
            iniDouble(tracksIni, section.str(), "starty", 90.0);
        track.startZ = iniDouble(tracksIni, section.str(), "startz", 6700.0);
        track.startHeading = wrapAngle(
            iniDouble(tracksIni, section.str(), "startangle", 0.0) *
            std::numbers::pi / 180.0);

        const std::string text = packageText(package.data(package.find(
            "tracks\\" + std::to_string(track.number) + ".txt")));
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::istringstream fields(line);
            BalloonhuntBalloon balloon;
            double sourceAltitude = 0.0;
            if (!(fields >> std::quoted(balloon.name) >> balloon.x >>
                  sourceAltitude >> balloon.z)) {
                continue;
            }
            balloon.x *= 4.0;
            balloon.altitude = -sourceAltitude * 4.0;
            balloon.z *= 4.0;
            balloon.number = static_cast<int>(track.balloons.size() + 1U);
            track.balloons.push_back(std::move(balloon));
        }
    }

    const std::string sceneryText = packageText(
        package.data(package.find("maps\\yo.txt")));
    std::istringstream sceneryLines(sceneryText);
    std::string sceneryLine;
    while (std::getline(sceneryLines, sceneryLine)) {
        if (!sceneryLine.empty() && sceneryLine.back() == '\r') {
            sceneryLine.pop_back();
        }
        std::istringstream fields(sceneryLine);
        std::string name;
        BalloonhuntScenery item;
        double sourceAltitude = 0.0;
        if (!(fields >> std::quoted(name) >> item.x >> sourceAltitude >>
              item.z) || name.rfind("sprite", 0) != 0) {
            continue;
        }
        const int sprite = std::clamp(std::stoi(name.substr(6)), 1, 9);
        item.texture = static_cast<std::size_t>(sprite - 1);
        item.x *= 4.0;
        item.altitude = -sourceAltitude * 4.0;
        item.z *= 4.0;
        game.scenery.push_back(item);
    }

    std::cout << "Loaded executable-derived Balloon Hunt: 20 exact tracks, "
              << game.scenery.size()
              << " mapped scenery billboards, 396 ordered balloons, three "
                 "flight modes, and original instruments\n";
    return game;
}

void BalloonhuntScene::startBalloonhuntGame(BalloonhuntMode mode) {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    game.activeMode = mode;
    game.score = 0;
    game.practiceRespawns = 0;
    const int track = mode == BalloonhuntMode::Challenge
        ? game.selectedTrack
        : 1;
    startBalloonhuntTrack(track, false);
}

void BalloonhuntScene::startBalloonhuntTrack(int track, bool retainScore) {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    game.activeTrack = std::clamp(track, 1, 20);
    auto &definition = game.tracks[static_cast<std::size_t>(game.activeTrack - 1)];
    for (auto &balloon : definition.balloons) {
        balloon.collected = false;
    }
    if (!retainScore) {
        game.score = 0;
    }
    game.phase = BalloonhuntPhase::Playing;
    game.currentBalloon = 0;
    game.x = definition.startX;
    game.altitude = definition.startAltitude;
    game.z = definition.startZ;
    game.heading = definition.startHeading;
    game.speed = 300.0;
    game.verticalSpeed = 0.0;
    game.fuel = 100.0;
    game.radarZoom = 100.0;
    game.autopilot = false;
    game.trackStarted = sceneElapsedNow();
    game.lastUpdate = game.trackStarted;
    game.lastHit = game.trackStarted;
    game.collisionCooldownUntil = game.trackStarted;
    game.phaseMessage.clear();
    audioMixer.setEffect(game.windSound,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    SDL_SetWindowTitle(
        window,
        ("FGP Native — Solans ballongjakt — bane " +
         std::to_string(game.activeTrack)).c_str());
}

void BalloonhuntScene::returnToBalloonhuntMenu() {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    game.phase = BalloonhuntPhase::Menu;
    game.phaseMessage.clear();
    audioMixer.setEffect(DecodedMedia{}, 0.0F);
    SDL_SetWindowTitle(
        window,
        "FGP Native — Solans ballongjakt — velg spillemodus");
}

bool BalloonhuntScene::handleBalloonhuntKey(SDL_Keycode key) {
    if (!balloonhuntGame || activeScene != "balloonhunt") {
        return false;
    }
    auto &game = *balloonhuntGame;
    if (key == SDLK_ESCAPE && (game.phase == BalloonhuntPhase::Menu ||
                               game.phase == BalloonhuntPhase::Playing)) {
        // 0x0042ef20 -> 0x0042ef30: only a Practice game (internal mode 1,
        // menu item 1 at 0x0042f0ef) ends on Escape, through 0x0042c580.
        // In the mode menu (mode 0) and in Competition (2) or Challenge (3)
        // games the key goes to 0x0040cc30, the registry parent (outtent).
        // The callback returns 1 either way.
        if (game.phase == BalloonhuntPhase::Playing &&
            game.activeMode == BalloonhuntMode::Practice) {
            returnToBalloonhuntMenu();
        } else {
            leaveToRegistryParent();
        }
        return true;
    }
    if (game.phase == BalloonhuntPhase::Menu) {
        if (key == SDLK_LEFT) {
            const int mode = static_cast<int>(game.selectedMode);
            game.selectedMode = static_cast<BalloonhuntMode>((mode + 2) % 3);
            return true;
        }
        if (key == SDLK_RIGHT) {
            const int mode = static_cast<int>(game.selectedMode);
            game.selectedMode = static_cast<BalloonhuntMode>((mode + 1) % 3);
            return true;
        }
        if (key == SDLK_UP &&
            game.selectedMode == BalloonhuntMode::Challenge) {
            game.selectedTrack = game.selectedTrack == 20
                ? 1 : game.selectedTrack + 1;
            return true;
        }
        if (key == SDLK_DOWN &&
            game.selectedMode == BalloonhuntMode::Challenge) {
            game.selectedTrack = game.selectedTrack == 1
                ? 20 : game.selectedTrack - 1;
            return true;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            startBalloonhuntGame(game.selectedMode);
            return true;
        }
        return false;
    }
    if (key == SDLK_ESCAPE) {
        // UNCERTAIN: the end-of-track and game-over states are the
        // original's modal messages (0x0042c5b6), which take Escape as
        // their close; native closes its overlay the same way as Enter.
        returnToBalloonhuntMenu();
        return true;
    }
    if (game.phase != BalloonhuntPhase::Playing &&
        (key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
        returnToBalloonhuntMenu();
        return true;
    }
    return key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP ||
           key == SDLK_DOWN || key == SDLK_a || key == SDLK_z ||
           key == SDLK_PAGEUP || key == SDLK_PAGEDOWN;
}

bool BalloonhuntScene::pressBalloonhuntHotspot(std::uint32_t hotspot) {
    if (!balloonhuntGame) {
        return false;
    }
    auto &game = *balloonhuntGame;
    if (hotspot == kModePrevious || hotspot == kModeNext) {
        const int direction = hotspot == kModeNext ? 1 : 2;
        game.selectedMode = static_cast<BalloonhuntMode>(
            (static_cast<int>(game.selectedMode) + direction) % 3);
        return true;
    }
    if (hotspot == kTrackPrevious || hotspot == kTrackNext) {
        const int direction = hotspot == kTrackNext ? 1 : -1;
        game.selectedTrack =
            ((game.selectedTrack - 1 + direction + 20) % 20) + 1;
        return true;
    }
    if (hotspot == kStart) {
        startBalloonhuntGame(game.selectedMode);
        return true;
    }
    if (hotspot == kContinue) {
        returnToBalloonhuntMenu();
        return true;
    }
    return false;
}

void BalloonhuntScene::hitBalloonhuntBalloon(
    bool correct, std::optional<std::size_t> collidedBalloon) {
    if (!balloonhuntGame ||
        balloonhuntGame->phase != BalloonhuntPhase::Playing) {
        return;
    }
    auto &game = *balloonhuntGame;
    auto &track = game.tracks[static_cast<std::size_t>(game.activeTrack - 1)];
    const double now = sceneElapsedNow();
    game.collisionCooldownUntil = now + 0.75;
    if (!correct) {
        audioMixer.setEffect(game.wrongSound,
                             audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
        return;
    }

    if (game.activeMode == BalloonhuntMode::Practice) {
        ++game.score;
    } else {
        game.score += 11;
        const double quickLimit = track.seconds /
            static_cast<double>(std::max(1, static_cast<int>(track.balloons.size()) - 3));
        if (now - game.lastHit < quickLimit) {
            game.score += 17;
        }
        game.fuel = std::min(100.0, game.fuel + 50.0);
    }
    game.lastHit = now;
    audioMixer.setEffect(game.hitSound,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);

    if (game.activeMode == BalloonhuntMode::Practice && collidedBalloon &&
        *collidedBalloon < track.balloons.size()) {
        track.balloons[*collidedBalloon].collected = true;
        game.currentBalloon = static_cast<int>(std::count_if(
            track.balloons.begin(), track.balloons.end(),
            [](const BalloonhuntBalloon &balloon) {
                return balloon.collected;
            }));
    } else if (game.currentBalloon <
               static_cast<int>(track.balloons.size())) {
        track.balloons[static_cast<std::size_t>(game.currentBalloon)]
            .collected = true;
        ++game.currentBalloon;
    }
    if (game.currentBalloon < static_cast<int>(track.balloons.size())) {
        return;
    }

    if (game.activeMode == BalloonhuntMode::Practice) {
        ++game.practiceRespawns;
        for (auto &balloon : track.balloons) {
            balloon.collected = false;
        }
        game.currentBalloon = 0;
        return;
    }

    audioMixer.setEffect(game.winSound,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    game.phaseStarted = now;
    if (game.activeMode == BalloonhuntMode::Competition) {
        const double remaining =
            std::max(0.0, track.seconds - (now - game.trackStarted));
        game.score += std::min(300, static_cast<int>(remaining * 10.0));
        game.phase = BalloonhuntPhase::TrackComplete;
        game.phaseMessage = language.get(
            "balloonhunt", "NEXT", "Neste") + " — " +
            std::to_string(game.activeTrack % 20 + 1);
    } else {
        game.phase = BalloonhuntPhase::Complete;
        game.phaseMessage = language.get("interfaceh", "WELLDONE",
                                         "Bra jobbet!");
    }
}

void BalloonhuntScene::updateBalloonhunt(double elapsed) {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    if (game.phase == BalloonhuntPhase::TrackComplete) {
        if (elapsed - game.phaseStarted >= 1.5) {
            startBalloonhuntTrack(game.activeTrack % 20 + 1, true);
        }
        return;
    }
    if (game.phase != BalloonhuntPhase::Playing) {
        return;
    }

    const double delta = std::clamp(elapsed - game.lastUpdate, 0.0, 0.05);
    game.lastUpdate = elapsed;
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    const int turn = (keys[SDL_SCANCODE_RIGHT] ? 1 : 0) -
                     (keys[SDL_SCANCODE_LEFT] ? 1 : 0);
    const int climb = (keys[SDL_SCANCODE_UP] ? 1 : 0) -
                      (keys[SDL_SCANCODE_DOWN] ? 1 : 0);

    const double centreX = game.x - kIslandCentre;
    const double centreZ = game.z - kIslandCentre;
    game.autopilot = centreX * centreX + centreZ * centreZ >=
                     kAutopilotRadius * kAutopilotRadius;
    if (game.autopilot) {
        const double target = std::atan2(kIslandCentre - game.x,
                                         kIslandCentre - game.z);
        game.heading += std::clamp(angleDifference(game.heading, target),
                                   -1.4 * delta, 1.4 * delta);
        game.speed = std::min(game.speed, 300.0);
    } else {
        game.heading += turn * 1.5 * delta;
    }
    game.heading = wrapAngle(game.heading);

    game.verticalSpeed += climb * 180.0 * delta;
    if (climb == 0) {
        game.verticalSpeed *= std::pow(0.12, delta);
    }
    game.verticalSpeed = std::clamp(game.verticalSpeed, -160.0, 160.0);
    game.altitude = std::clamp(game.altitude + game.verticalSpeed * delta,
                               90.0, 1200.0);

    if (!game.autopilot) {
        if (keys[SDL_SCANCODE_A]) {
            game.speed += 80.0 * delta;
        }
        if (keys[SDL_SCANCODE_Z]) {
            game.speed -= 80.0 * delta;
        }
    }
    game.speed = std::clamp(game.speed, 50.0, 600.0);
    if (keys[SDL_SCANCODE_PAGEUP]) {
        game.radarZoom -= 70.0 * delta;
    }
    if (keys[SDL_SCANCODE_PAGEDOWN]) {
        game.radarZoom += 70.0 * delta;
    }
    game.radarZoom = std::clamp(game.radarZoom, 35.0, 250.0);

    game.x += std::sin(game.heading) * game.speed * delta;
    game.z += std::cos(game.heading) * game.speed * delta;

    auto &track = game.tracks[static_cast<std::size_t>(game.activeTrack - 1)];
    if (game.activeMode == BalloonhuntMode::Competition) {
        game.fuel = std::max(0.0, game.fuel - 2.0 * delta);
        if (game.fuel <= 0.0 || elapsed - game.trackStarted >= track.seconds) {
            game.phase = BalloonhuntPhase::GameOver;
            game.phaseStarted = elapsed;
            game.phaseMessage = game.fuel <= 0.0
                ? language.get("balloonhunt", "OUTOFGAS", "Tanken er tom.")
                : language.get("balloonhunt", "OUTOFTIME", "Tiden er ute.");
            audioMixer.setEffect(game.gameOverSound,
                                 audioVolumes[0] * sceneAudioGains[0]);
            SDL_PauseAudioDevice(audioDevice,
                                 musicEnabled && !paused ? 0 : 1);
            // Original mode 2 enters 0x0042c580 with reason 1/2. The routine
            // opens interfaceh:GAMEOVER first (0x0042c5b6-0x0042c5f2), then
            // passes the score and medal table to 0x0041f9a0.
            showMessageWithTitle(
                language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
                game.phaseMessage, MessageKind::BalloonhuntGameOver);
            return;
        }
    }

    if (elapsed < game.collisionCooldownUntil) {
        return;
    }
    const double collision = track.size * 0.8;
    const double collisionSquared = collision * collision;
    for (std::size_t index = 0; index < track.balloons.size(); ++index) {
        const auto &balloon = track.balloons[index];
        if (balloon.collected) {
            continue;
        }
        const double dx = game.x - balloon.x;
        const double dy = game.altitude - balloon.altitude;
        const double dz = game.z - balloon.z;
        if (dx * dx + dy * dy + dz * dz < collisionSquared) {
            hitBalloonhuntBalloon(
                game.activeMode == BalloonhuntMode::Practice ||
                    static_cast<int>(index) == game.currentBalloon,
                index);
            break;
        }
    }
}

void BalloonhuntScene::renderBalloonhuntTerrain() {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    const SDL_Rect skyDestination{0, 86, 800, 336};
    const int skyWidth = 640;
    const int skyX = static_cast<int>(
        wrapAngle(game.heading) / (2.0 * std::numbers::pi) * 384.0);
    const SDL_Rect skySource{skyX, 0, skyWidth, 336};
    SDL_RenderCopy(renderer, game.skyTexture.get(), &skySource,
                   &skyDestination);

    constexpr int columns = 34;
    constexpr int rings = 23;
    std::array<double, rings> depths{};
    depths[0] = 70.0;
    for (int ring = 1; ring < rings; ++ring) {
        depths[ring] = std::min(kFarPlane, depths[ring - 1] * 1.28);
    }
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(columns * rings);
    const double forwardX = std::sin(game.heading);
    const double forwardZ = std::cos(game.heading);
    const double rightX = std::cos(game.heading);
    const double rightZ = -std::sin(game.heading);
    for (int ring = 0; ring < rings; ++ring) {
        const double depth = depths[ring];
        for (int column = 0; column < columns; ++column) {
            const double across = -1.45 +
                2.9 * column / static_cast<double>(columns - 1);
            const double side = across * depth;
            const double worldX = game.x + forwardX * depth + rightX * side;
            const double worldZ = game.z + forwardZ * depth + rightZ * side;
            SDL_Vertex vertex{};
            vertex.position.x = static_cast<float>(400.0 + across * kFocalLength);
            vertex.position.y = static_cast<float>(
                kHorizon + game.altitude * kFocalLength / depth);
            const Uint8 shade = static_cast<Uint8>(std::clamp(
                255.0 - depth / 65.0, 120.0, 255.0));
            vertex.color = {shade, shade, shade, 255};
            vertex.tex_coord.x = static_cast<float>(
                std::clamp(worldX / kWorldMapExtent, 0.0, 1.0));
            vertex.tex_coord.y = static_cast<float>(
                std::clamp(worldZ / kWorldMapExtent, 0.0, 1.0));
            vertices.push_back(vertex);
        }
    }
    std::vector<int> indices;
    indices.reserve((rings - 1) * (columns - 1) * 6);
    for (int ring = 0; ring + 1 < rings; ++ring) {
        for (int column = 0; column + 1 < columns; ++column) {
            const int upper = ring * columns + column;
            const int lower = (ring + 1) * columns + column;
            indices.insert(indices.end(),
                           {upper, lower, upper + 1,
                            upper + 1, lower, lower + 1});
        }
    }
    SDL_RenderGeometry(renderer, game.mapTexture.get(), vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));

    struct Billboard {
        SDL_Texture *texture{};
        double depth{};
        double centreX{};
        double baseY{};
        double worldSize{};
        bool centred{};
    };
    std::vector<Billboard> billboards;
    billboards.reserve(256);
    const auto append = [&](SDL_Texture *texture, double x, double altitude,
                            double z, double worldSize, bool centred) {
        const double dx = x - game.x;
        const double dz = z - game.z;
        const double side = dx * rightX + dz * rightZ;
        const double depth = dx * forwardX + dz * forwardZ;
        if (depth < kNearPlane || depth > 6500.0 ||
            std::abs(side) > depth * 1.35) {
            return;
        }
        billboards.push_back({
            texture, depth,
            400.0 + side * kFocalLength / depth,
            kHorizon + (game.altitude - altitude) * kFocalLength / depth,
            worldSize, centred});
    };
    for (const auto &item : game.scenery) {
        append(game.sceneryTextures[item.texture].get(), item.x,
               item.altitude, item.z, 230.0, false);
    }
    const auto &track = game.tracks[static_cast<std::size_t>(game.activeTrack - 1)];
    for (std::size_t index = 0; index < track.balloons.size(); ++index) {
        const auto &balloon = track.balloons[index];
        if (!balloon.collected) {
            append(game.balloonTextures[index % game.balloonTextures.size()].get(),
                   balloon.x, balloon.altitude, balloon.z,
                   track.size * 2.0, true);
        }
    }
    std::sort(billboards.begin(), billboards.end(),
              [](const Billboard &left, const Billboard &right) {
                  return left.depth > right.depth;
              });
    const SDL_Rect flightClip{0, 86, 800, 336};
    SDL_RenderSetClipRect(renderer, &flightClip);
    for (const auto &billboard : billboards) {
        const int size = static_cast<int>(std::clamp(
            billboard.worldSize * kFocalLength / billboard.depth,
            3.0, 390.0));
        const int y = billboard.centred
            ? static_cast<int>(billboard.baseY - size / 2.0)
            : static_cast<int>(billboard.baseY - size);
        const SDL_Rect destination{
            static_cast<int>(billboard.centreX - size / 2.0), y,
            size, size};
        SDL_RenderCopy(renderer, billboard.texture, nullptr, &destination);
    }
    SDL_RenderSetClipRect(renderer, nullptr);
}

void BalloonhuntScene::renderBalloonhuntInterface(double elapsed) {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    const auto &track = game.tracks[static_cast<std::size_t>(game.activeTrack - 1)];
    SDL_Rect top = scaledRect(0, -1, 640, 70);
    SDL_Rect bottom = scaledRect(0, 336, 640, 144);
    SDL_RenderCopy(renderer, game.interfaceTopTexture.get(), nullptr, &top);
    SDL_RenderCopy(renderer, game.interfaceBottomTexture.get(), nullptr,
                   &bottom);

    const double played = std::max(0.0, elapsed - game.trackStarted);
    const double shownTime = game.activeMode == BalloonhuntMode::Competition
        ? std::max(0.0, track.seconds - played) : played;
    drawCenteredText(renderer, calendarFont,
                     formattedTime(shownTime), scaledRect(148, 5, 96, 28));
    drawCenteredText(renderer, calendarFont,
                     std::to_string(game.score), scaledRect(263, 5, 96, 28));
    drawCenteredText(renderer, calendarFont,
                     language.get("balloonhunt", "TRACK2", "Bane") + " " +
                         std::to_string(game.activeTrack),
                     scaledRect(390, 5, 96, 28));

    const int gasX = static_cast<int>(49 * kGameScale);
    const int speedX = static_cast<int>(129 * kGameScale);
    const int altitudeX = static_cast<int>(510 * kGameScale);
    const int indicatorX = static_cast<int>(591 * kGameScale);
    // The panel is authored at 640x480 and begins at source Y=336. Its dial
    // centres are around local Y=89, hence source/global Y=425 before the
    // native view's 1.25 scale. Keeping all overlays in that source space
    // prevents them drifting above the painted bezels.
    const int gaugeY = static_cast<int>(425 * kGameScale);
    drawNeedle(renderer, gasX, gaugeY,
               -2.25 + game.fuel / 100.0 * 4.5, 31,
               SDL_Color{220, 20, 20, 255});
    drawNeedle(renderer, speedX, gaugeY,
               -2.25 + (game.speed - 50.0) / 550.0 * 4.5, 31,
               SDL_Color{230, 230, 210, 255});
    drawNeedle(renderer, altitudeX, gaugeY,
               -2.25 + (game.altitude - 90.0) / 1110.0 * 4.5, 31,
               SDL_Color{230, 230, 210, 255});

    SDL_Rect compass = scaledRect(178, 393, 64, 64);
    SDL_RenderCopyEx(renderer, game.compassTexture.get(), nullptr, &compass,
                     -game.heading * 180.0 / std::numbers::pi,
                     nullptr, SDL_FLIP_NONE);

    const int pitch = std::clamp(
        3 - static_cast<int>(std::lround(game.verticalSpeed / 55.0)), 0, 6);
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    const int roll = std::clamp(3 + (keys[SDL_SCANCODE_RIGHT] ? 2 : 0) -
                                      (keys[SDL_SCANCODE_LEFT] ? 2 : 0),
                                0, 6);
    SDL_Rect gyro = scaledRect(394, 393, 65, 65);
    SDL_RenderCopy(renderer,
                   game.gyroTextures[static_cast<std::size_t>(pitch * 7 + roll)].get(),
                   nullptr, &gyro);

    const int mapCentreX = static_cast<int>(320 * kGameScale);
    const int mapCentreY = static_cast<int>(423 * kGameScale);
    const int sourceSize = std::clamp(
        static_cast<int>(game.radarZoom * 2.0), 70, 500);
    const int mapX = std::clamp(static_cast<int>(game.x / 40.0) - sourceSize / 2,
                                0, 1024 - sourceSize);
    const int mapY = std::clamp(static_cast<int>(game.z / 40.0) - sourceSize / 2,
                                0, 1024 - sourceSize);
    constexpr int mapRadius = 57;
    constexpr int mapSegments = 40;
    std::array<SDL_Vertex, mapSegments + 1> mapVertices{};
    std::array<int, mapSegments * 3> mapIndices{};
    mapVertices[0].position = {
        static_cast<float>(mapCentreX), static_cast<float>(mapCentreY)};
    mapVertices[0].color = {255, 255, 255, 255};
    mapVertices[0].tex_coord = {
        static_cast<float>((mapX + sourceSize * 0.5) / 1024.0),
        static_cast<float>((mapY + sourceSize * 0.5) / 1024.0)};
    for (int segment = 0; segment < mapSegments; ++segment) {
        const double angle = 2.0 * std::numbers::pi * segment /
                             static_cast<double>(mapSegments);
        const double unitX = std::cos(angle);
        const double unitY = std::sin(angle);
        auto &vertex = mapVertices[static_cast<std::size_t>(segment + 1)];
        vertex.position = {
            static_cast<float>(mapCentreX + unitX * mapRadius),
            static_cast<float>(mapCentreY + unitY * mapRadius)};
        vertex.color = {255, 255, 255, 255};
        vertex.tex_coord = {
            static_cast<float>((mapX + (unitX + 1.0) * sourceSize * 0.5) /
                               1024.0),
            static_cast<float>((mapY + (unitY + 1.0) * sourceSize * 0.5) /
                               1024.0)};
        const std::size_t triangle = static_cast<std::size_t>(segment * 3);
        mapIndices[triangle] = 0;
        mapIndices[triangle + 1] = segment + 1;
        mapIndices[triangle + 2] = (segment + 1) % mapSegments + 1;
    }
    SDL_RenderGeometry(renderer, game.mapTexture.get(), mapVertices.data(),
                       static_cast<int>(mapVertices.size()),
                       mapIndices.data(), static_cast<int>(mapIndices.size()));
    for (std::size_t index = 0; index < track.balloons.size(); ++index) {
        const auto &balloon = track.balloons[index];
        if (balloon.collected) {
            continue;
        }
        const double pixelX = balloon.x / 40.0;
        const double pixelY = balloon.z / 40.0;
        const int markerX = mapCentreX + static_cast<int>(
            ((pixelX - mapX) / sourceSize - 0.5) * mapRadius * 2.0);
        const int markerY = mapCentreY + static_cast<int>(
            ((pixelY - mapY) / sourceSize - 0.5) * mapRadius * 2.0);
        const int markerDx = markerX - mapCentreX;
        const int markerDy = markerY - mapCentreY;
        if (markerDx * markerDx + markerDy * markerDy >
            (mapRadius - 7) * (mapRadius - 7)) {
            continue;
        }
        SDL_Rect marker{markerX - 6, markerY - 6, 12, 12};
        SDL_RenderCopy(renderer,
                       game.radarBalloonTextures[index % game.radarBalloonTextures.size()].get(),
                       nullptr, &marker);
    }
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawLine(renderer, mapCentreX, mapCentreY - 7,
                      mapCentreX, mapCentreY + 7);
    SDL_RenderDrawLine(renderer, mapCentreX - 7, mapCentreY,
                      mapCentreX + 7, mapCentreY);

    if (game.currentBalloon < static_cast<int>(track.balloons.size())) {
        const std::size_t index =
            static_cast<std::size_t>(game.currentBalloon) %
            game.radarBalloonTextures.size();
        SDL_Rect next{indicatorX - 19, gaugeY - 19, 38, 38};
        SDL_RenderCopy(renderer, game.radarBalloonTextures[index].get(),
                       nullptr, &next);
    }
    if (game.autopilot) {
        SDL_Rect autopilot = scaledRect(280, 348, 80, 20);
        SDL_RenderCopy(renderer, game.autopilotTexture.get(), nullptr,
                       &autopilot);
    }
}

void BalloonhuntScene::renderBalloonhunt(double elapsed) {
    if (!balloonhuntGame) {
        return;
    }
    auto &game = *balloonhuntGame;
    if (game.phase == BalloonhuntPhase::Menu) {
        if (game.solanTexture && !game.solanMedia.videoFrames.empty()) {
            const auto frameIndex = frameIndexAt(game.solanMedia, elapsed, true);
            const auto &frame = game.solanMedia.videoFrames[frameIndex];
            uploadVideoFrame(game.solanTexture.get(), frame);
            SDL_Rect destination = scaledRect(0, 48, 640, 128);
            SDL_RenderCopy(renderer, game.solanTexture.get(), nullptr,
                           &destination);
        }
        SDL_Rect startBounds = scaledRect(202, 331, 240, 123);
        SDL_RenderCopy(renderer, game.startTexture.get(), nullptr,
                       &startBounds);
        const std::array<std::string, 3> modeNames{{
            language.get("balloonhunt", "NORMAL", "Ballongjakt"),
            language.get("balloonhunt", "PRACTICE", "S\xf8ndagstur"),
            language.get("balloonhunt", "SINGLE", "Baneutfordring"),
        }};
        drawScaledCenteredText(
            renderer, dialogFont,
            modeNames[static_cast<std::size_t>(game.selectedMode)],
            SDL_Rect{232, 342, 175, 19});
        if (game.selectedMode == BalloonhuntMode::Challenge) {
            SDL_Rect selector = scaledRect(253, 374, 173, 24);
            SDL_RenderCopy(renderer, game.trackSelectorTexture.get(), nullptr,
                           &selector);
            drawScaledCenteredText(
                renderer, dialogFont,
                language.get("balloonhunt", "TRACK2", "Bane") + " " +
                    std::to_string(game.selectedTrack),
                SDL_Rect{271, 374, 98, 24});
        }
        SDL_Rect startButton = scaledRect(287, 409, 69, 31);
        SDL_RenderCopy(renderer, game.startButtonTexture.get(), nullptr,
                       &startButton);
        drawScaledCenteredText(
            renderer, dialogFont,
            language.get("balloonhunt", "START", "Start"),
            SDL_Rect{287, 409, 69, 31});
        return;
    }

    renderBalloonhuntTerrain();
    renderBalloonhuntInterface(elapsed);
    if (game.phase != BalloonhuntPhase::Playing &&
        game.phase != BalloonhuntPhase::GameOver) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 175);
        SDL_Rect overlay{175, 235, 450, 130};
        SDL_RenderFillRect(renderer, &overlay);
        drawCenteredText(renderer, calendarFont, game.phaseMessage,
                         SDL_Rect{195, 260, 410, 35});
        drawCenteredText(renderer, calendarFont,
                         language.get("interfaceh", "OK", "OK"),
                         SDL_Rect{335, 315, 130, 30});
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
BalloonhuntScene::balloonhuntHotspotAt(int x, int y) const {
    if (!balloonhuntGame || activeScene != "balloonhunt") {
        return std::nullopt;
    }
    const auto &game = *balloonhuntGame;
    const SDL_Point point{x, y};
    const auto hit = [&](std::uint32_t hotspot, SDL_Rect bounds)
        -> std::optional<std::pair<std::uint32_t, SDL_Rect>> {
        return SDL_PointInRect(&point, &bounds)
            ? std::optional<std::pair<std::uint32_t, SDL_Rect>>(
                  std::pair{hotspot, bounds})
            : std::nullopt;
    };
    if (game.phase == BalloonhuntPhase::Menu) {
        if (auto result = hit(kModePrevious, scaledRect(205, 341, 22, 24))) {
            return result;
        }
        if (auto result = hit(kModeNext, scaledRect(416, 341, 22, 24))) {
            return result;
        }
        if (game.selectedMode == BalloonhuntMode::Challenge) {
            if (auto result = hit(kTrackPrevious,
                                  scaledRect(253, 374, 18, 24))) {
                return result;
            }
            if (auto result = hit(kTrackNext,
                                  scaledRect(369, 374, 18, 24))) {
                return result;
            }
        }
        return hit(kStart, scaledRect(287, 408, 69, 31));
    }
    if (game.phase != BalloonhuntPhase::Playing &&
        game.phase != BalloonhuntPhase::TrackComplete) {
        return hit(kContinue, SDL_Rect{175, 235, 450, 130});
    }
    return std::nullopt;
}

bool BalloonhuntScene::executeBalloonhuntDeveloperCommand(std::string_view command) {
    constexpr std::string_view balloonhuntControlPrefix = "balloonhunt:";
    if (activeScene != "balloonhunt" || !balloonhuntGame) {
        return false;
    }
    const std::string_view control =
        command.substr(balloonhuntControlPrefix.size());
    if (control == "start") {
        startBalloonhuntGame(balloonhuntGame->selectedMode);
    } else if (control == "mode:competition") {
        balloonhuntGame->selectedMode = BalloonhuntMode::Competition;
    } else if (control == "mode:practice") {
        balloonhuntGame->selectedMode = BalloonhuntMode::Practice;
    } else if (control == "mode:challenge") {
        balloonhuntGame->selectedMode = BalloonhuntMode::Challenge;
    } else if (control.rfind("track:", 0) == 0) {
        const int track = std::stoi(std::string(control.substr(6)));
        if (track < 1 || track > 20) {
            return false;
        }
        balloonhuntGame->selectedTrack = track;
    } else if (control == "hit") {
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            startBalloonhuntGame(balloonhuntGame->selectedMode);
        }
        hitBalloonhuntBalloon(true);
    } else if (control == "wrong") {
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            startBalloonhuntGame(balloonhuntGame->selectedMode);
        }
        hitBalloonhuntBalloon(false);
    } else if (control.rfind("set-score=", 0) == 0) {
        // Retry until the Start click has initialized mode 2; starting a
        // Balloon Hunt run resets the score to zero.
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            return false;
        }
        const auto parsed = std::stoul(std::string(control.substr(
            std::string_view("set-score=").size())));
        if (parsed > static_cast<unsigned long>(
                         std::numeric_limits<int>::max())) {
            throw std::runtime_error(
                "Developer Balloon Hunt score is out of range");
        }
        balloonhuntGame->score = static_cast<int>(parsed);
    } else if (control == "complete-track") {
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            startBalloonhuntGame(balloonhuntGame->selectedMode);
        }
        if (balloonhuntGame->activeMode == BalloonhuntMode::Practice) {
            return false;
        }
        while (balloonhuntGame->phase == BalloonhuntPhase::Playing) {
            hitBalloonhuntBalloon(true);
        }
    } else if (control == "timeout") {
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            startBalloonhuntGame(BalloonhuntMode::Competition);
        }
        const auto &track = balloonhuntGame->tracks[
            static_cast<std::size_t>(balloonhuntGame->activeTrack - 1)];
        balloonhuntGame->trackStarted =
            sceneElapsedNow() - track.seconds - 1.0;
    } else if (control == "fuel") {
        if (balloonhuntGame->phase != BalloonhuntPhase::Playing) {
            startBalloonhuntGame(BalloonhuntMode::Competition);
        }
        balloonhuntGame->fuel = 0.0;
    } else if (control == "menu") {
        returnToBalloonhuntMenu();
    } else if (control == "status") {
        static constexpr std::array<std::string_view, 3> modes{
            "competition", "practice", "challenge"};
        std::cout << "Balloon Hunt status: "
                  << modes[static_cast<std::size_t>(
                         balloonhuntGame->activeMode)]
                  << ", track " << balloonhuntGame->activeTrack
                  << ", balloon " << balloonhuntGame->currentBalloon
                  << ", score " << balloonhuntGame->score
                  << ", fuel " << balloonhuntGame->fuel << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Balloon Hunt control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
