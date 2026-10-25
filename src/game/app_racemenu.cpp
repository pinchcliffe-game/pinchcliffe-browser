#include "game/app_racemenu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "game/awards.h"
#include "media/smacker.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

namespace {

// Controls (0x0047b700), sliders (0x0047b440) and hotspots.bmp ovals.
constexpr std::uint32_t kHelp = 1;
constexpr std::uint32_t kExit = 2;
constexpr std::uint32_t kTrackOval = 3;
constexpr std::uint32_t kCar1Oval = 4;
constexpr std::uint32_t kCar2Oval = 5;
constexpr std::uint32_t kModeSlider = 6;
constexpr std::uint32_t kLapsSlider = 7;
constexpr std::uint32_t kDriversSlider = 8;
constexpr std::uint32_t kTrackPrevious = 10;
constexpr std::uint32_t kTrackNext = 11;
constexpr std::uint32_t kPlayer1Controls = 12;
constexpr std::uint32_t kCar1Previous = 13;
constexpr std::uint32_t kCar1Next = 14;
constexpr std::uint32_t kPlayer2Controls = 15;
constexpr std::uint32_t kCar2Previous = 16;
constexpr std::uint32_t kCar2Next = 17;
constexpr std::uint32_t kStart = 18;
constexpr std::uint32_t kTricks = 19;
constexpr std::uint32_t kRaceTimes = 20;
constexpr std::uint32_t kLapTimes = 21;
constexpr std::uint32_t kSound = 22;

// Settings words (0x0047baa0).
constexpr std::size_t kSetTrack = 0;
constexpr std::size_t kSetCar1 = 1;
constexpr std::size_t kSetCar2 = 2;
constexpr std::size_t kSetMode = 3;
constexpr std::size_t kSetLaps = 4;
constexpr std::size_t kSetDrivers = 5;
constexpr std::size_t kSetTrackFrame = 6;
constexpr std::size_t kSetCar1Frame = 7;
constexpr std::size_t kSetCar2Frame = 8;
constexpr std::size_t kSetTricks = 0x3b;
constexpr std::size_t kSettingsBytes = 0xf0;

// Tables in FGP.exe.
constexpr std::uint32_t kLapCounts = 0x004a0128U;   // 3, 5, 8
constexpr std::uint32_t kModes = 0x004a0134U;       // 0, 2, 1
constexpr std::uint32_t kTimeKeyBits = 0x004a0140U; // per lap count
constexpr std::uint32_t kTrackNameRect = 0x004a01d0U;
constexpr std::uint32_t kCar1NameRect = 0x004a01e0U;
constexpr std::uint32_t kCar2NameRect = 0x004a01f0U;
constexpr std::uint32_t kModeSliderRect = 0x004a0290U;
constexpr std::uint32_t kLapsSliderRect = 0x004a02a0U;
constexpr std::uint32_t kDriversSliderRect = 0x004a02b0U;
constexpr std::uint32_t kModePictures = 0x004a02c0U;
constexpr std::uint32_t kLapsPictures = 0x004a02d8U;
constexpr std::uint32_t kDriversPictures = 0x004a02f0U;
// The descriptors' +0x54/+0x58 default positions.
constexpr SDL_Point kTrackClipAt{0x40, 0x43};
constexpr SDL_Point kCar1ClipAt{0x40, 0x16c};
constexpr SDL_Point kCar2ClipAt{0x1d4, 0x16c};
constexpr SDL_Point kPlayer2OffAt{0x1d8, 0x16a};
constexpr SDL_Point kBlinkerAt{0x17d, 0x94};

struct Label {
    std::uint32_t rect;
    const char *key;
};
// 0x0047b530 after the three name objects.
constexpr std::array<Label, 9> kLabels{{
    {0x004a0200U, "TRACK"},    {0x004a0210U, "PLAYER1"},
    {0x004a0220U, "PLAYER2"},  {0x004a0230U, "MODE"},
    {0x004a0240U, "LAPS"},     {0x004a0250U, "DRIVERS"},
    {0x004a0270U, "RACETIME"}, {0x004a0260U, "LAPTIME"},
    {0x004a0280U, "SOUND"},
}};

SurfacePtr toArgb(SDL_Surface *surface) {
    SurfacePtr converted(
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) {
        throw std::runtime_error(std::string("SDL_ConvertSurfaceFormat failed: ") +
                                 SDL_GetError());
    }
    return converted;
}

SurfacePtr greenSurface(int width, int height) {
    SurfacePtr surface = createSurface(width, height);
    fillSurface(surface.get(), nullptr, 0x00ff00U);
    return surface;
}

// sprintf with the one integer the racing menu's names take (ILTEMPO's
// "Il Tempo Gigante V%d").
std::string formatName(const std::string &format, int value) {
    std::string result;
    for (std::size_t index = 0; index < format.size(); ++index) {
        if (format[index] == '%' && index + 1 < format.size()) {
            const char next = format[index + 1];
            if (next == 'd') {
                result += std::to_string(value);
                ++index;
                continue;
            }
            if (next == '%') {
                result.push_back('%');
                ++index;
                continue;
            }
        }
        result.push_back(format[index]);
    }
    return result;
}

} // namespace

std::span<const std::uint8_t>
RacemenuScene::racemenuAsset(const std::string &asset) {
    // UNCERTAIN: the original's layer order; Data2 (the edition's race.ini
    // with eight tracks) is searched before Data1.
    for (Package *package :
         {sceneAddonPackage.get(), sceneData2Package.get(),
          sceneLanguagePackage.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (package) {
            if (const auto *entry = package->findOptional(asset)) {
                return package->data(*entry);
            }
        }
    }
    throw std::runtime_error("Racing menu asset not found: " + asset);
}

SurfacePtr RacemenuScene::racemenuBitmap(std::string_view name) {
    return toArgb(
        loadBmp(racemenuAsset("bitmap\\" + std::string(name) + ".bmp")).get());
}

RacemenuScene::RacemenuGame RacemenuScene::loadRacemenuGame() {
    RacemenuGame game;
    game.stage = std::make_unique<ActivityStage>(renderer, referenceTrace);
    game.panel = std::make_unique<ActivityPanel>(
        *game.stage,
        [this](std::string_view name) { return racemenuBitmap(name); });
    // 0x00409d00 draws tips in Small Fonts_07_.
    static BitmapFont tipFont;
    if (!tipFont.surface) {
        tipFont = loadBitmapFont(
            renderer, common.data(common.find("fonts\\Small Fonts_07_.bmp")));
    }
    SurfacePtr tipFrame;
    SurfacePtr tipDot;
    for (Package *candidate :
         {addonCommon.get(), data2Common.get(), editionCommon.get(), &common}) {
        if (!candidate) continue;
        if (!tipFrame) {
            if (const auto *entry = candidate->findOptional("bitmap\\tooltip.bmp"))
                tipFrame = toArgb(loadBmp(candidate->data(*entry)).get());
        }
        if (!tipDot) {
            if (const auto *entry = candidate->findOptional("bitmap\\tooldot.bmp"))
                tipDot = toArgb(loadBmp(candidate->data(*entry)).get());
        }
    }
    game.tooltip = std::make_unique<ActivityTooltip>(
        *game.stage, tipFont, std::move(tipFrame), std::move(tipDot));
    game.font = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Arial Black 08.bmp")));
    game.ini = std::make_unique<LanguageTable>(racemenuAsset("race.ini"));

    // Descriptors (+0x48 keyed): the three turntables (hotspots 3-5 at
    // +0x50), player2off and the blinker.
    game.trackClip.sprite =
        game.stage->create({}, kTrackClipAt.x, kTrackClipAt.y, kTrackOval, true);
    game.car1Clip.sprite =
        game.stage->create({}, kCar1ClipAt.x, kCar1ClipAt.y, kCar1Oval, true);
    game.car2Clip.sprite =
        game.stage->create({}, kCar2ClipAt.x, kCar2ClipAt.y, kCar2Oval, true);
    game.player2Off = game.stage->create("player2off", kPlayer2OffAt.x,
                                         kPlayer2OffAt.y, 0, true);
    game.stage->setSurface(game.player2Off, racemenuBitmap("player2off"));
    game.blinker.sprite =
        game.stage->create("blinker", kBlinkerAt.x, kBlinkerAt.y, 0, true);
    for (const std::uint32_t slider : {kModeSlider, kLapsSlider, kDriversSlider})
        game.values[slider] = game.stage->create({}, 0, 0, 0, false);
    game.trackName = game.stage->create({}, 0, 0, 0, true);
    game.car1Name = game.stage->create({}, 0, 0, 0, true);
    game.car2Name = game.stage->create({}, 0, 0, 0, true);
    std::cout << "Loaded executable-derived racing menu: race.ini, turntables, "
                 "mode/laps/drivers sliders, dirty tricks and best-time "
                 "controls\n";
    return game;
}

std::uint32_t RacemenuScene::racemenuHotspotAt(int x, int y) {
    auto &game = *racemenuGame;
    std::uint32_t ignored = 0;
    if (game.draggingSlider != 0U)
        ignored = game.sliders.at(game.draggingSlider)->thumbHotspot();
    if (const auto hit = game.stage->hotspotAt(x, y, ignored)) return *hit;
    if (!sceneHotspots) return 0;
    if (x < 0 || y < 0 || x >= sceneHotspots->w || y >= sceneHotspots->h)
        return 0;
    return pixelValue(sceneHotspots.get(), x, y);
}

void RacemenuScene::racemenuHotspotChanged(std::uint32_t hotspot) {
    // 0x0040c160: 0x114 for the old hotspot, then 0x113 for the new one.
    auto &game = *racemenuGame;
    if (hotspot == game.hotspot) return;
    const std::uint32_t previous = game.hotspot;
    game.hotspot = hotspot;
    game.panel->setHover(previous, false);
    // 0x0047bc80: leaving an oval stops its turntable.
    if (previous == kTrackOval) stopRacemenuClip(game.trackClip);
    if (previous == kCar1Oval) stopRacemenuClip(game.car1Clip);
    if (previous == kCar2Oval) stopRacemenuClip(game.car2Clip);
    game.panel->setHover(hotspot, true);
    // 0x0047bc00 -> 0x0047bc40: entering one plays it from FrameNum.
    if (hotspot == kTrackOval) playRacemenuClip(game.trackClip);
    if (hotspot == kCar1Oval) playRacemenuClip(game.car1Clip);
    if (hotspot == kCar2Oval) playRacemenuClip(game.car2Clip);
    game.tooltip->hotspotChanged(hotspot, sceneElapsedNow());
}

void RacemenuScene::loadRacemenuClip(RacemenuClip &clip,
                                     const std::string &resource) {
    if (clip.resource == resource && !clip.media.videoFrames.empty()) return;
    clip.resource = resource;
    const auto bytes = racemenuAsset("animation\\" + resource + ".smk");
    clip.bytes.assign(bytes.begin(), bytes.end());
    clip.media = decodeSmackerStreamed(
        std::span<const std::uint8_t>(clip.bytes.data(), clip.bytes.size()),
        "racemenu " + resource, SmackerKeying::TopLeftRgb565, false, true);
    clip.frames = static_cast<int>(clip.media.videoFrames.size());
    if (clip.frames > 1) {
        const double duration = clip.media.videoFrames.metadata(1).time -
                                clip.media.videoFrames.metadata(0).time;
        if (duration > 0.0) clip.frameSeconds = duration;
    }
    clip.shown = -1;
}

// SmackGoto (0x00403770) and the frame extraction 0x00403160, which shows
// the frame and then calls SmackNextFrame. The captures show the frame number
// SmackGoto takes is one past the frame it shows: 0x0047ac10's default of 1
// shows the first frame, and FrameNum (+0x374, 0x00403c60) read back and
// passed to SmackGoto again shows the same frame. UNCERTAIN: SmackGoto(0),
// which a new settings record passes, is taken to show the first frame too.
void RacemenuScene::gotoRacemenuFrame(RacemenuClip &clip, int frame) {
    showRacemenuFrame(clip, std::max(frame - 1, 0));
}

void RacemenuScene::showRacemenuFrame(RacemenuClip &clip, int frame) {
    auto &game = *racemenuGame;
    if (clip.frames <= 0) return;
    frame = ((frame % clip.frames) + clip.frames) % clip.frames;
    clip.frameNumber = frame + 1;
    if (frame == clip.shown) return;
    clip.shown = frame;
    const VideoFrame &video = clip.media.videoFrames[static_cast<std::size_t>(frame)];
    SurfacePtr surface = createSurface(video.width, video.height);
    for (int row = 0; row < video.height; ++row) {
        std::memcpy(static_cast<std::uint8_t *>(surface->pixels) +
                        static_cast<std::size_t>(row) * surface->pitch,
                    video.bgra.data() + static_cast<std::size_t>(row) * video.width * 4U,
                    static_cast<std::size_t>(video.width) * 4U);
    }
    game.stage->setSurface(clip.sprite, std::move(surface));
}

void RacemenuScene::playRacemenuClip(RacemenuClip &clip) {
    // 0x0047bc40: FrameNum, play (0x004129d0), SmackGoto(FrameNum).
    if (clip.frames <= 0 || !racemenuGame->stage->active(clip.sprite)) return;
    const int frame = clip.frameNumber;
    clip.playing = true;
    gotoRacemenuFrame(clip, frame);
    clip.playStartFrame = clip.shown;
    clip.playStartedAt = sceneElapsedNow();
}

void RacemenuScene::stopRacemenuClip(RacemenuClip &clip) {
    // 0x00412a60.
    clip.playing = false;
}

void RacemenuScene::tickRacemenuClip(RacemenuClip &clip, double now) {
    if (!clip.playing || clip.frames <= 0) return;
    const int advanced = static_cast<int>(
        std::floor((now - clip.playStartedAt) / clip.frameSeconds));
    showRacemenuFrame(clip, clip.playStartFrame + advanced);
}

void RacemenuScene::setRacemenuText(int sprite, const std::string &text) {
    // 0x004079c0 -> 0x00407970: clear to the key colour, then the text
    // centred (0x101) in the object.
    auto &game = *racemenuGame;
    const SDL_Rect rect = game.stage->rect(sprite);
    SurfacePtr surface = greenSurface(rect.w, rect.h);
    if (!text.empty()) {
        const SDL_Point at = centreIn(SDL_Rect{0, 0, rect.w, rect.h},
                                      textWidth(game.font, text), game.font.height);
        drawFontText(surface.get(), game.font, text, at.x, at.y);
    }
    game.stage->setSurface(sprite, std::move(surface));
}

int RacemenuScene::racemenuCarFormat() const {
    // 0x0047ac00: Il Tempo's V number.
    return racemenuGame->level * 2 + 2;
}

// 0x0047ac10.
void RacemenuScene::showRacemenuResource(RacemenuClip &clip, int resource,
                                         const std::string &section,
                                         int nameSprite, int format) {
    auto &game = *racemenuGame;
    const std::string number = std::to_string(resource);
    if (clip.sprite >= 0) {
        int frame = 1;
        bool playing = false;
        if (game.stage->active(clip.sprite)) {
            frame = clip.frameNumber;
            playing = clip.playing;
            game.stage->remove(clip.sprite);
            clip.playing = false;
        }
        const std::string name =
            trim(game.ini->get(section, "resource" + number));
        if (!name.empty()) {
            loadRacemenuClip(clip, name);
            game.stage->sprite(clip.sprite).name = name;
            game.stage->add(clip.sprite, -1, -1, 0);
            gotoRacemenuFrame(clip, frame);
            if (playing) {
                clip.playing = true;
                clip.playStartFrame = clip.shown;
                clip.playStartedAt = sceneElapsedNow();
            }
        }
    }
    if (nameSprite >= 0) {
        const std::string key = trim(game.ini->get(section, "name" + number));
        if (!key.empty()) {
            const auto colon = key.find(':');
            const std::string text =
                colon == std::string::npos
                    ? key
                    : language.get(key.substr(0, colon), key.substr(colon + 1), key);
            setRacemenuText(nameSprite, formatName(text, format));
        }
    }
}

// 0x0047b1d0: the resources of a race.ini section the profile may use.
std::vector<int> RacemenuScene::racemenuList(const std::string &section,
                                             int defaultState, int maximum,
                                             std::uint32_t trackAwards,
                                             std::uint32_t carAwards) {
    auto &game = *racemenuGame;
    std::vector<int> list;
    for (int resource = 0; resource < maximum; ++resource) {
        const std::string number = std::to_string(resource);
        if (trim(game.ini->get(section, "resource" + number)).empty()) break;
        const int state = std::atoi(
            game.ini->get(section, "state" + number, std::to_string(defaultState))
                .c_str());
        const int level =
            std::atoi(game.ini->get(section, "level" + number, "0").c_str());
        bool available = false;
        switch (state) {
        case 0:
            available = true;
            break;
        case 1:
            available = awardItemAvailable(executableGameData, kAwardTracks,
                                           trackAwards, level);
            break;
        case 2:
            available = awardItemAvailable(executableGameData, kAwardCars,
                                           carAwards, level);
            break;
        case 3:
            available = level <= game.level;
            break;
        default:
            break;
        }
        if (available) list.push_back(resource);
    }
    return list;
}

// 0x0047b330: every listed car and track is written into the award values,
// and the profile saved when that changed anything.
void RacemenuScene::grantRacemenuAwards() {
    auto &game = *racemenuGame;
    const auto path = activeProfilePath();
    if (!path) return;
    try {
        auto awards = profileAwards(*path);
        std::uint32_t cars = awards[kAwardCars];
        for (const int car : game.cars) {
            if (!awardItemAvailable(executableGameData, kAwardCars, cars, car))
                cars = awardItemGrant(executableGameData, kAwardCars, cars, car);
        }
        std::uint32_t tracks = awards[kAwardTracks];
        for (const int track : game.tracks) {
            if (!awardItemAvailable(executableGameData, kAwardTracks, tracks, track))
                tracks = awardItemGrant(executableGameData, kAwardTracks, tracks, track);
        }
        if (awards[kAwardCars] != cars || awards[kAwardTracks] != tracks) {
            awards[kAwardCars] = cars;
            awards[kAwardTracks] = tracks;
            setProfileAwards(*path, awards);
        }
    } catch (const std::exception &error) {
        std::cerr << "Could not update racing awards: " << error.what() << '\n';
    }
}

// 0x0047ab50.
void RacemenuScene::setRacemenuPlayer2(bool enabled) {
    auto &game = *racemenuGame;
    const int state = enabled ? 0 : 2;
    game.panel->setState(kCar2Previous, state);
    game.panel->setState(kCar2Next, state);
    game.panel->setState(kPlayer2Controls, state);
    if (enabled) {
        game.stage->removeIfActive(game.player2Off);
        // The call passes no descriptor, only the name object.
        const int resource =
            game.cars.empty() ? 0 : game.cars[static_cast<std::size_t>(game.car2)];
        RacemenuClip none;
        showRacemenuResource(none, resource, "cars", game.car2Name,
                             racemenuCarFormat());
        return;
    }
    if (!game.stage->active(game.player2Off))
        game.stage->add(game.player2Off, -1, -1, 1);
    setRacemenuText(game.car2Name, {});
}

// 0x0047aa50 / 0x0047aae0: the value picture "%s%d" at its table position.
void RacemenuScene::setRacemenuValuePicture(std::uint32_t slider, int value) {
    auto &game = *racemenuGame;
    std::uint32_t table = 0;
    const char *prefix = "";
    if (slider == kModeSlider) {
        table = kModePictures;
        prefix = "mode";
    } else if (slider == kLapsSlider) {
        table = kLapsPictures;
        prefix = "laps";
    } else if (slider == kDriversSlider) {
        table = kDriversPictures;
        prefix = "drivers";
    } else {
        return;
    }
    const int sprite = game.values.at(slider);
    game.stage->removeIfActive(sprite);
    const std::string name = std::string(prefix) + std::to_string(value + 1);
    game.stage->sprite(sprite).name = name;
    game.stage->setSurface(sprite, racemenuBitmap(name));
    const auto x = static_cast<std::int32_t>(
        executableGameData.u32At(table + 8U * static_cast<std::uint32_t>(value)));
    const auto y = static_cast<std::int32_t>(
        executableGameData.u32At(table + 4U + 8U * static_cast<std::uint32_t>(value)));
    game.stage->add(sprite, x, y, 0);
}

void RacemenuScene::racemenuSliderChanged(std::uint32_t id, int value) {
    setRacemenuValuePicture(id, value);
    if (id == kModeSlider) {
        // 0x0047ab30: modes 1 and 2 have a second player.
        const int mode = static_cast<std::int32_t>(
            executableGameData.u32At(kModes + 4U * static_cast<std::uint32_t>(value)));
        setRacemenuPlayer2(mode > 0 && mode < 3);
    }
}

// Scene initialisation 0x0047ae80.
void RacemenuScene::announceRacemenuEntry() {
    auto &game = *racemenuGame;
    game.announced = true;
    auto &stage = *game.stage;
    auto &panel = *game.panel;
    const auto rectAt = [this](std::uint32_t address) {
        const auto value = [&](std::uint32_t offset) {
            return static_cast<std::int32_t>(executableGameData.u32At(address + offset));
        };
        return SDL_Rect{value(0), value(4), value(8) - value(0), value(12) - value(4)};
    };
    // UNCERTAIN: 0x0040b1f0("theme", 0) starts the theme music; native keeps
    // the scene shell's music.

    // 0x0047b530: the name objects (0x00407930, layer 0), then the labels
    // (0x00407a30), each centred (0x101) in its rectangle.
    const std::array<std::pair<int, std::uint32_t>, 3> names{{
        {game.trackName, kTrackNameRect},
        {game.car1Name, kCar1NameRect},
        {game.car2Name, kCar2NameRect}}};
    for (const auto &[sprite, address] : names) {
        const SDL_Rect rect = rectAt(address);
        stage.setSurface(sprite, greenSurface(rect.w, rect.h));
        stage.add(sprite, rect.x, rect.y, 0);
    }
    for (const auto &label : kLabels) {
        const std::string text = language.get("racemenu", label.key, label.key);
        const int width = textWidth(game.font, text);
        const int height = game.font.height;
        SurfacePtr surface = greenSurface(width, height);
        drawFontText(surface.get(), game.font, text, 0, 0);
        const int sprite = stage.create({}, 0, 0, 0, true);
        stage.setSurface(sprite, std::move(surface));
        const SDL_Point at = centreIn(rectAt(label.rect), width, height);
        stage.add(sprite, at.x, at.y, 0);
    }

    // 0x0047b700: the controls at layer 0. Their ninth argument, a tip key,
    // goes to control +0x26c (0x00406940), which nothing reads; with the
    // tooltip armed without a table (0x00409b50(0)) the captures show none.
    const std::uint32_t hover = racemenuHotspotAt(mouseX, mouseY);
    game.hotspot = hover;
    using Type = ActivityPanel::Type;
    const auto control = [&](std::uint32_t id, int x, int y, const char *normal,
                             const char *pressed, const char *tip, bool keyed,
                             Type type = Type::Push) {
        (void)tip;
        panel.addControl(id, x, y, {normal, pressed, "", ""}, type, 0, {}, hover,
                         keyed);
    };
    panel.setLayer(0);
    control(kHelp, 0, 0, "help_0", "help_1", nullptr, true);
    control(kExit, 0x2d8, 0, "exit_0", "exit_1", nullptr, true);
    control(kStart, 0x165, 0x121, "start_0", "start_1", "TIP_START", false);
    control(kLapTimes, 0x1b7, 0x1c, "button_0", "button_1", "TIP_LAPTIME", false);
    control(kRaceTimes, 0x1b7, 0x3d, "button_0", "button_1", "TIP_RACETIME", false);
    control(kSound, 0x1b7, 0x5d, "button_0", "button_1", "TIP_SOUND", false);
    control(kTricks, 0x163, 0x1f2, "tricks_0", "tricks_1", "TIP_TRICKS", false,
            Type::Toggle);
    control(kTrackNext, 0x117, 0xfb, "next_0", "next_1", "TRACK_NEXT", true);
    control(kCar1Next, 0x117, 0x222, "next_0", "next_1", "CAR_NEXT", true);
    control(kCar2Next, 0x2af, 0x222, "next_0", "next_1", "CAR_NEXT", true);
    control(kTrackPrevious, 0x51, 0xfb, "prev_0", "prev_1", "TRACK_PREV", true);
    control(kCar1Previous, 0x51, 0x222, "prev_0", "prev_1", "CAR_PREV", true);
    control(kCar2Previous, 0x1e8, 0x221, "prev_0", "prev_1", "CAR_PREV", true);
    control(kPlayer1Controls, 0x1e, 0x200, "ply1_0", "ply1_1", "CAR_INFO", false);
    control(kPlayer2Controls, 0x2da, 0x1ff, "ply2_0", "ply2_1", "CAR_INFO", false);

    // 0x0047b440: laps, drivers, then mode; thumbs at layer 0 (0x004094c0).
    const std::array<std::tuple<std::uint32_t, std::uint32_t, int>, 3> sliders{{
        {kLapsSlider, kLapsSliderRect, 2},
        {kDriversSlider, kDriversSliderRect, 3},
        {kModeSlider, kModeSliderRect, 2}}};
    for (const auto &[id, address, maximum] : sliders) {
        auto slider = std::make_unique<ActivityScrollbar>(stage, panel);
        slider->create(id, rectAt(address), racemenuBitmap("slider_button"), id,
                       0, false, "slider_button");
        slider->setRange(0, maximum);
        game.sliders[id] = std::move(slider);
    }

    // 0x0047b180: the lists, with the awards they imply.
    std::uint32_t trackAwards = 0;
    std::uint32_t carAwards = 0;
    std::uint32_t placedParts = 0;
    if (const auto path = activeProfilePath()) {
        try {
            const auto awards = profileAwards(*path);
            trackAwards = awards[kAwardTracks];
            carAwards = awards[kAwardCars];
            if (const auto state = profileBuildacarState(*path))
                placedParts = state->placedParts;
        } catch (const std::exception &error) {
            std::cerr << "Could not read the racing profile: " << error.what() << '\n';
        }
    }
    game.level = buildacarLevel(executableGameData, placedParts);
    game.cars = racemenuList("cars", 2, 0x18, trackAwards, carAwards);
    game.tracks = racemenuList("tracks", 1, 0x10, trackAwards, carAwards);
    grantRacemenuAwards();

    // 0x0047b0a0 defaults (drivers 3, tricks on), then the profile's record.
    game.settings.fill(0);
    game.settings[kSetDrivers] = 3;
    game.settings[kSetTricks] = 1;
    if (const auto path = activeProfilePath()) {
        try {
            if (const auto stored = profileSceneRecordBytes(
                    *path, "racemenu", 0x402U, 0U, kSettingsBytes)) {
                for (std::size_t word = 0; word < game.settings.size(); ++word)
                    game.settings[word] = readLe32(*stored, word * 4U);
            }
        } catch (const std::exception &error) {
            std::cerr << "Could not read the racing settings: " << error.what() << '\n';
        }
    }

    // 0x0047af20.
    const auto clampIndex = [](std::uint32_t stored, std::size_t count) {
        const auto value = static_cast<std::int32_t>(stored);
        if (value < 0) return 0;
        return std::min(value, static_cast<int>(count) - 1);
    };
    game.track = clampIndex(game.settings[kSetTrack], game.tracks.size());
    game.car1 = clampIndex(game.settings[kSetCar1], game.cars.size());
    game.car2 = clampIndex(game.settings[kSetCar2], game.cars.size());
    game.track = std::max(game.track, 0);
    game.car1 = std::max(game.car1, 0);
    game.car2 = std::max(game.car2, 0);
    const auto trackResource = [&]() {
        return game.tracks.empty() ? 0 : game.tracks[static_cast<std::size_t>(game.track)];
    };
    const auto carResource = [&](int index) {
        return game.cars.empty() ? 0 : game.cars[static_cast<std::size_t>(index)];
    };
    showRacemenuResource(game.trackClip, trackResource(), "tracks", game.trackName, 0);
    showRacemenuResource(game.car1Clip, carResource(game.car1), "cars",
                         game.car1Name, racemenuCarFormat());
    showRacemenuResource(game.car2Clip, carResource(game.car2), "cars",
                         game.car2Name, racemenuCarFormat());
    gotoRacemenuFrame(game.trackClip, static_cast<int>(game.settings[kSetTrackFrame]));
    gotoRacemenuFrame(game.car1Clip, static_cast<int>(game.settings[kSetCar1Frame]));
    gotoRacemenuFrame(game.car2Clip, static_cast<int>(game.settings[kSetCar2Frame]));
    // 0x0047b039-0x0047b055: each slider set (0x00408eb0) that moves its
    // value posts 0x117 at once (0x00408f30, 0x0040906b-0x00409085), so a
    // stored mode or driver count shows its picture before the direct calls
    // below (0x0047b05a-0x0047b076) show all three again.
    for (const auto &[id, word] :
         std::array<std::pair<std::uint32_t, std::size_t>, 3>{{
             {kModeSlider, kSetMode}, {kLapsSlider, kSetLaps},
             {kDriversSlider, kSetDrivers}}}) {
        if (game.sliders.at(id)->setValue(static_cast<std::int32_t>(game.settings[word])))
            racemenuSliderChanged(id, game.sliders.at(id)->value());
    }
    for (const auto &[id, word] :
         std::array<std::pair<std::uint32_t, std::size_t>, 3>{{
             {kModeSlider, kSetMode}, {kLapsSlider, kSetLaps},
             {kDriversSlider, kSetDrivers}}}) {
        racemenuSliderChanged(id, game.sliders.at(id)->value());
    }
    panel.setState(kTricks, game.settings[kSetTricks] != 0U ? 1 : 0);

    // 0x0047b960: back from an info page (entry modes 3-5), select what it
    // showed last.
    if (infoRequest && infoRequest->returnScene == "racemenu" &&
        activeSceneEntryMode >= 3 && activeSceneEntryMode <= 5) {
        const int resource = infoRequest->page;
        if (activeSceneEntryMode == 3) {
            showRacemenuResource(game.trackClip, resource, "tracks", game.trackName, 0);
            const auto found = std::find(game.tracks.begin(), game.tracks.end(), resource);
            if (found != game.tracks.end())
                game.track = static_cast<int>(found - game.tracks.begin());
        } else {
            const bool second = activeSceneEntryMode == 5;
            showRacemenuResource(second ? game.car2Clip : game.car1Clip, resource,
                                 "cars", second ? game.car2Name : game.car1Name,
                                 racemenuCarFormat());
            const auto found = std::find(game.cars.begin(), game.cars.end(), resource);
            if (found != game.cars.end())
                (second ? game.car2 : game.car1) =
                    static_cast<int>(found - game.cars.begin());
        }
    }

    // The blinker at layer -1, playing.
    loadRacemenuClip(game.blinker, "blinker");
    showRacemenuFrame(game.blinker, 0);
    stage.add(game.blinker.sprite, -1, -1, -1);
    game.blinker.playing = true;
    game.blinker.playStartFrame = 0;
    game.blinker.playStartedAt = sceneElapsedNow();
}

// 0x0047baa0.
std::array<std::uint32_t, 60> RacemenuScene::collectRacemenuSettings() {
    auto &game = *racemenuGame;
    auto settings = game.settings;
    settings[kSetTrackFrame] = static_cast<std::uint32_t>(game.trackClip.frameNumber);
    settings[kSetCar1Frame] = static_cast<std::uint32_t>(game.car1Clip.frameNumber);
    settings[kSetCar2Frame] = static_cast<std::uint32_t>(game.car2Clip.frameNumber);
    settings[kSetTrack] = static_cast<std::uint32_t>(game.track);
    settings[kSetCar1] = static_cast<std::uint32_t>(game.car1);
    settings[kSetCar2] = static_cast<std::uint32_t>(game.car2);
    settings[kSetMode] = static_cast<std::uint32_t>(game.sliders.at(kModeSlider)->value());
    settings[kSetLaps] = static_cast<std::uint32_t>(game.sliders.at(kLapsSlider)->value());
    settings[kSetDrivers] =
        static_cast<std::uint32_t>(game.sliders.at(kDriversSlider)->value());
    settings[kSetTricks] = game.panel->state(kTricks) == 1 ? 1U : 0U;
    return settings;
}

void RacemenuScene::saveRacemenu() {
    if (!racemenuGame || !racemenuGame->announced) return;
    const auto settings = collectRacemenuSettings();
    racemenuGame->settings = settings;
    const auto path = activeProfilePath();
    if (!path) return;
    std::vector<std::uint8_t> payload(kSettingsBytes);
    for (std::size_t word = 0; word < settings.size(); ++word)
        writeLe32(payload, word * 4U, settings[word]);
    try {
        // UNCERTAIN: words 9..0x3a hold the control setups, whose defaults
        // (0x0047a980) include runtime pointers; native keeps what it read
        // and writes zeros for a profile without the record.
        setProfileSceneRecordBytes(*path, "racemenu", 0x402U, 0U, payload);
    } catch (const std::exception &error) {
        std::cerr << "Could not save the racing settings: " << error.what() << '\n';
    }
}

// 0x0047bcf0: the info record for the track (3) or a player's car (4, 5).
void RacemenuScene::openRacemenuInfo(std::uint32_t oval) {
    auto &game = *racemenuGame;
    const auto settings = collectRacemenuSettings();
    InfoRequest record;
    record.returnScene = "racemenu";
    record.returnMode = static_cast<int>(oval);
    if (oval == kTrackOval) {
        record.article = "track";
        record.awardCategory = kAwardTracks;
        record.page = game.tracks.empty()
                           ? 0
                           : game.tracks[static_cast<std::size_t>(settings[kSetTrack])];
    } else {
        record.article = "car";
        record.awardCategory = kAwardCars;
        const auto car = settings[oval == kCar1Oval ? kSetCar1 : kSetCar2];
        record.page = game.cars.empty() ? 0 : game.cars[static_cast<std::size_t>(car)];
    }
    infoRequest = record;
    saveRacemenu();
    enterScene("info", "info");
}

// 0x0047bfa0 builds the race's parameter block at 0x006cfed0 (0x0047c000)
// and enters racing with it.
void RacemenuScene::startRace() {
    auto &game = *racemenuGame;
    const auto settings = collectRacemenuSettings();
    const auto listed = [](const std::vector<int> &list, std::uint32_t index) {
        return list.empty() ? 0 : list[std::min<std::size_t>(index, list.size() - 1U)];
    };
    const int track = listed(game.tracks, settings[kSetTrack]);
    racing::RaceBlock block;
    // 0x0047c100 and 0x0047c0e0: the two score keys.
    block.raceTimeKey =
        executableGameData.u32At(kTimeKeyBits + 4U * settings[kSetLaps]) |
        static_cast<std::uint32_t>(track);
    block.lapTimeKey = static_cast<std::uint32_t>(track) | 0x80000000U;
    block.cars_count = static_cast<int>(settings[kSetDrivers]) + 1;
    block.laps = static_cast<std::int32_t>(
        executableGameData.u32At(kLapCounts + 4U * settings[kSetLaps]));
    block.mode = static_cast<std::int32_t>(
        executableGameData.u32At(kModes + 4U * settings[kSetMode]));
    // 0x0047c063: two cars at least in mode 2.
    if (block.mode == 2 && block.cars_count < 2) block.cars_count = 2;
    // "resource%d" in race.ini's [tracks] (0x0047c09a) and [cars]
    // (0x0047c260).
    block.track = trim(game.ini->get("tracks", "resource" + std::to_string(track), "unknown"));
    racing::drawRaceCars(
        block, listed(game.cars, settings[kSetCar1]), listed(game.cars, settings[kSetCar2]),
        static_cast<int>(settings[kSetCar1]), static_cast<int>(settings[kSetCar2]),
        [&](int resource) {
            return trim(game.ini->get("cars", "resource" + std::to_string(resource),
                                      "unknown"));
        },
        crtRandom);
    block.tricks = settings[kSetTricks];
    raceParameterBlock = block;
    raceScoreRows.recorded = false;
    raceScoreRows.track = track;
    raceScoreRows.laps = block.laps;
    saveRacemenu();
    enterScene("racing", "racing");
}

// 0x0047c2a0: the best race times (per track and lap count) or lap times
// (per track) in the shared score table 0x0041f030.
void RacemenuScene::showRacemenuBestTimes(std::uint32_t id) {
    auto &game = *racemenuGame;
    const auto settings = collectRacemenuSettings();
    const int track =
        game.tracks.empty() ? 0 : game.tracks[static_cast<std::size_t>(settings[kSetTrack])];
    const std::string key =
        trim(game.ini->get("tracks", "name" + std::to_string(track), "unknown"));
    const auto colon = key.find(':');
    const std::string trackName =
        colon == std::string::npos
            ? key
            : language.get(key.substr(0, colon), key.substr(colon + 1), key);
    std::string title;
    std::uint32_t tableKey = 0;
    std::optional<ScoreRow> current;
    const bool sameTrack = raceScoreRows.recorded && raceScoreRows.track == track;
    if (id == kRaceTimes) {
        const int laps = static_cast<std::int32_t>(
            executableGameData.u32At(kLapCounts + 4U * settings[kSetLaps]));
        // 0x0047c39b-0x0047c3d3: the last race's row, for this track and
        // lap count.
        if (sameTrack && raceScoreRows.laps == laps) current = raceScoreRows.raceTime;
        title = language.get("racemenu", "HISCORE_RACETIME", "%s");
        const auto s = title.find("%s");
        if (s != std::string::npos) title.replace(s, 2, trackName);
        title = formatName(title, laps);
        // 0x0047c100.
        tableKey = executableGameData.u32At(kTimeKeyBits + 4U * settings[kSetLaps]) |
                   static_cast<std::uint32_t>(track);
    } else {
        title = language.get("racemenu", "HISCORE_LAPTIME", "%s");
        const auto s = title.find("%s");
        if (s != std::string::npos) title.replace(s, 2, trackName);
        // 0x0047c0e0.
        tableKey = static_cast<std::uint32_t>(track) | 0x80000000U;
        // 0x0047c32f-0x0047c35c: the last race's best lap, for this track.
        if (sameTrack) current = raceScoreRows.lapTime;
    }
    // 0x0047c3d8-0x0047c3ea: 0x0041f030("racing", title, key, row, ranks,
    // 0x102).
    std::cout << "Racing menu best times: " << title << " (racing key 0x"
              << std::hex << tableKey << std::dec << ")\n";
    showScoreTable("racing", title, tableKey, kScoreTimeFlags, current);
}

// 0x0047bdb0.
void RacemenuScene::racemenuControlClicked(std::uint32_t id) {
    auto &game = *racemenuGame;
    const auto step = [&](int &selection, const std::vector<int> &list, int direction) {
        // 0x00469910.
        const int count = static_cast<int>(list.size());
        int value = selection + direction;
        if (count < 2) value = 0;
        else if (value < 0) value = count - (-value) % count;
        else value %= count;
        selection = value;
    };
    switch (id) {
    case kHelp:
        showSceneHelp();
        return;
    case kExit:
        // 0x0040cc30: the registry parent of racemenu is menu.
        saveRacemenu();
        enterMenu();
        return;
    case kTrackPrevious:
    case kTrackNext:
        step(game.track, game.tracks, id == kTrackNext ? 1 : -1);
        showRacemenuResource(game.trackClip,
                             game.tracks.empty() ? 0 : game.tracks[static_cast<std::size_t>(game.track)],
                             "tracks", game.trackName, 0);
        return;
    case kCar1Previous:
    case kCar1Next:
        step(game.car1, game.cars, id == kCar1Next ? 1 : -1);
        showRacemenuResource(game.car1Clip,
                             game.cars.empty() ? 0 : game.cars[static_cast<std::size_t>(game.car1)],
                             "cars", game.car1Name, racemenuCarFormat());
        return;
    case kCar2Previous:
    case kCar2Next:
        step(game.car2, game.cars, id == kCar2Next ? 1 : -1);
        showRacemenuResource(game.car2Clip,
                             game.cars.empty() ? 0 : game.cars[static_cast<std::size_t>(game.car2)],
                             "cars", game.car2Name, racemenuCarFormat());
        return;
    case kPlayer1Controls:
    case kPlayer2Controls:
        // UNCERTAIN: 0x0041d140, the control-setup dialog, is not ported.
        std::cout << "Racing menu: control setup for player "
                  << (id == kPlayer1Controls ? 1 : 2) << " (0x0041d140)\n";
        return;
    case kStart:
        startRace();
        return;
    case kRaceTimes:
    case kLapTimes:
        showRacemenuBestTimes(id);
        return;
    case kSound:
        // UNCERTAIN: 0x00422180, the race volume dialog, is not ported.
        std::cout << "Racing menu: race sound settings (0x00422180)\n";
        return;
    default:
        return;
    }
}

void RacemenuScene::racemenuMouseDown(int x, int y) {
    auto &game = *racemenuGame;
    const std::uint32_t hotspot = racemenuHotspotAt(x, y);
    racemenuHotspotChanged(hotspot);
    game.tooltip->hide();
    // 0x0047bcc0: an oval opens its info page; otherwise the slider with
    // the hotspot's id takes the press (0x004092d0).
    if (hotspot >= kTrackOval && hotspot <= kCar2Oval) {
        openRacemenuInfo(hotspot);
        return;
    }
    const auto slider = game.sliders.find(hotspot);
    if (slider != game.sliders.end()) {
        bool dragging = false;
        const int before = slider->second->value();
        slider->second->pressTrack(x, y, dragging);
        if (dragging) {
            game.draggingSlider = hotspot;
            game.dragLastX = x;
            game.dragLastY = y;
        }
        if (slider->second->value() != before)
            racemenuSliderChanged(hotspot, slider->second->value());
    }
    if (const auto clicked = game.panel->press(hotspot, x, y))
        racemenuControlClicked(*clicked);
}

void RacemenuScene::racemenuMouseUp(int x, int y) {
    auto &game = *racemenuGame;
    racemenuHotspotChanged(racemenuHotspotAt(x, y));
    if (game.draggingSlider != 0U) {
        // 0x004092b0: snap the thumb to its value.
        auto &slider = *game.sliders.at(game.draggingSlider);
        game.draggingSlider = 0;
        slider.setValue(slider.value());
    }
    if (const auto clicked = game.panel->release()) racemenuControlClicked(*clicked);
}

void RacemenuScene::racemenuMouseMove(int x, int y) {
    auto &game = *racemenuGame;
    racemenuHotspotChanged(racemenuHotspotAt(x, y));
    if (game.draggingSlider == 0U) return;
    // 0x0040c300 moves the thumb by the pointer delta; 0x00409270 then
    // places it at the pointer's x.
    auto &slider = *game.sliders.at(game.draggingSlider);
    const SDL_Rect rect = game.stage->rect(slider.thumbSprite());
    game.stage->move(slider.thumbSprite(), rect.x + (x - game.dragLastX),
                     rect.y + (y - game.dragLastY));
    game.dragLastX = x;
    game.dragLastY = y;
    const int before = slider.value();
    slider.dragTo(x);
    if (slider.value() != before)
        racemenuSliderChanged(game.draggingSlider, slider.value());
}

void RacemenuScene::enterRacemenuFromMenu() {
    // 0x004180a0: the buildacar record's placed-part count must reach 8.
    std::uint32_t placedParts = 0;
    if (const auto path = activeProfilePath()) {
        try {
            if (const auto state = profileBuildacarState(*path))
                placedParts = state->placedParts;
        } catch (const std::exception &error) {
            std::cerr << "Could not read Build-a-Car progress: " << error.what() << '\n';
        }
    }
    if (placedParts < 8U) {
        showMessageWithTitle(language.get("menu", "RACE_TITLE", "Bilspillet"),
                             language.get("menu", "RACE_LOCKED", ""));
        return;
    }
    enterScene("racemenu", "Racing menu");
}

bool RacemenuScene::handleRacemenuEvent(const SDL_Event &event) {
    if (!racemenuGame || activeScene != "racemenu" || !racemenuGame->announced)
        return false;
    switch (event.type) {
    case SDL_MOUSEMOTION:
        racemenuMouseMove(event.motion.x, event.motion.y);
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        racemenuMouseDown(event.button.x, event.button.y);
        return true;
    case SDL_MOUSEBUTTONUP:
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        racemenuMouseUp(event.button.x, event.button.y);
        return true;
    default:
        // Keys go to the engine: racemenu's callback has no 0x10c case.
        return false;
    }
}

void RacemenuScene::updateRacemenu(double) {
    if (!racemenuGame || activeScene != "racemenu") return;
    auto &game = *racemenuGame;
    if (!game.announced) announceRacemenuEntry();
    const double now = sceneElapsedNow();
    tickRacemenuClip(game.trackClip, now);
    tickRacemenuClip(game.car1Clip, now);
    tickRacemenuClip(game.car2Clip, now);
    tickRacemenuClip(game.blinker, now);
    game.tooltip->tick(now, SDL_Rect{mouseX, mouseY, 32, 32});
}

void RacemenuScene::renderRacemenu() {
    if (!racemenuGame || activeScene != "racemenu" || !racemenuGame->announced)
        return;
    racemenuGame->stage->render();
}

bool RacemenuScene::executeRacemenuDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "racemenu:";
    if (command.substr(prefix.size()).rfind("seed:", 0) == 0) {
        // Capture fixture: the C runtime rand() state, as the x32dbg script
        // reseeds it when the opponent draw 0x0047c120 starts.
        crtRandom.state = static_cast<std::uint32_t>(
            std::stoul(std::string(command.substr(prefix.size() + 5))));
        return true;
    }
    if (command.substr(prefix.size()) == "enter") {
        // The main menu's racing button (entry 8).
        if (viewMode != ViewMode::Menu || dialogMode != DialogMode::None)
            return false;
        enterRacemenuFromMenu();
        return true;
    }
    if (activeScene != "racemenu" || !racemenuGame || !racemenuGame->announced)
        return false;
    auto &game = *racemenuGame;
    const auto control = command.substr(prefix.size());
    const auto number = [&](std::size_t offset) {
        return std::stoi(std::string(control.substr(offset)));
    };
    if (control.rfind("click:", 0) == 0) {
        const auto id = static_cast<std::uint32_t>(number(6));
        if (id == kTricks) {
            game.panel->setState(kTricks, game.panel->state(kTricks) == 1 ? 0 : 1);
        }
        racemenuControlClicked(id);
    } else if (control.rfind("slider:", 0) == 0) {
        // slider:ID=VALUE
        const auto equals = control.find('=');
        if (equals == std::string_view::npos) return false;
        const auto id = static_cast<std::uint32_t>(
            std::stoi(std::string(control.substr(7, equals - 7))));
        const auto found = game.sliders.find(id);
        if (found == game.sliders.end()) return false;
        if (found->second->setValue(number(equals + 1)))
            racemenuSliderChanged(id, found->second->value());
    } else if (control.rfind("info:", 0) == 0) {
        const auto oval = static_cast<std::uint32_t>(number(5));
        if (oval < kTrackOval || oval > kCar2Oval) return false;
        openRacemenuInfo(oval);
    } else if (control == "status") {
        std::cout << "Racing menu status: level " << game.level << ", tracks";
        for (const int track : game.tracks) std::cout << ' ' << track;
        std::cout << " (selected " << game.track << "), cars";
        for (const int car : game.cars) std::cout << ' ' << car;
        std::cout << " (selected " << game.car1 << '/' << game.car2 << "), mode "
                  << game.sliders.at(kModeSlider)->value() << ", laps "
                  << game.sliders.at(kLapsSlider)->value() << ", drivers "
                  << game.sliders.at(kDriversSlider)->value() << ", tricks "
                  << game.panel->state(kTricks) << ", frames "
                  << game.trackClip.shown << '/' << game.car1Clip.shown << '/'
                  << game.car2Clip.shown << '\n';
    } else {
        throw std::runtime_error("Unknown developer racing menu control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
