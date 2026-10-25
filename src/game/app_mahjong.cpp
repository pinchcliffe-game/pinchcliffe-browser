#include "game/app_mahjong.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "media/smacker.h"

namespace fgp {
namespace {

namespace mj = fgp::mahjong;

// 0x00450870's control ids, as 0x00450400 registers them.
constexpr int kControlExit = 1;
constexpr int kControlHelp = 2;
constexpr int kControlHint = 3;
constexpr int kControlUndo = 4;
constexpr int kControlNew = 5;
constexpr int kControlHiscore = 6;

// 0x0049f218 and 0x0049f228: the title/pairs and clock rectangles
// (left, top, right, bottom), both drawn centred (flags 0x101).
constexpr SDL_Rect kTitleRect{297, 19, 503 - 297, 36 - 19};
constexpr SDL_Rect kClockRect{357, 46, 445 - 357, 62 - 46};

// 0x0049e060: the Game Over/Solved message follows 60 ticks (600 ms).
constexpr float kDialogDelay = 60.0f;
// 0x0049f244: milliseconds to the board's 10 ms ticks.
constexpr float kTicksPerMillisecond = 0.1f;

constexpr std::array<std::string_view, 6> kSounds{{
    "noselect1", "noselect2", "select1", "select2", "win", "loose",
}};

// 24-bit BMP pixels to the 16-bit surface the original loads them into.
// UNCERTAIN: GDI's 24-to-16-bit blit (0x00405fa0) is taken to truncate each
// channel, which is what 0x00455cc0 does for every colour it builds.
std::vector<std::uint16_t> surfaceTo565(SDL_Surface *surface, SDL_Rect area) {
    SurfacePtr converted(
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGB888, 0));
    if (!converted) {
        throw std::runtime_error(std::string("SDL_ConvertSurfaceFormat: ") +
                                 SDL_GetError());
    }
    std::vector<std::uint16_t> pixels(
        static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h));
    SDL_LockSurface(converted.get());
    for (int y = 0; y < area.h; ++y) {
        const auto *row = reinterpret_cast<const std::uint32_t *>(
            static_cast<const std::uint8_t *>(converted->pixels) +
            static_cast<std::ptrdiff_t>(y + area.y) * converted->pitch);
        for (int x = 0; x < area.w; ++x) {
            const std::uint32_t pixel = row[x + area.x];
            pixels[static_cast<std::size_t>(y * area.w + x)] = mj::rgb565(
                static_cast<int>((pixel >> 16) & 0xffU),
                static_cast<int>((pixel >> 8) & 0xffU),
                static_cast<int>(pixel & 0xffU));
        }
    }
    SDL_UnlockSurface(converted.get());
    return pixels;
}

std::optional<std::uint32_t> parseUnsigned(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

MahjongScene::MahjongGame MahjongScene::loadMahjongGame(Package &package) {
    MahjongGame game;
    const auto asset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Mahjong asset not found: " +
                                 std::string(name));
    };
    const auto commonAsset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {addonCommon.get(), data2Common.get(),
                                   editionCommon.get(), &common}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Common asset not found: " +
                                 std::string(name));
    };
    // 0x00450403-0x0045042b: the three fonts.
    game.titleFont = loadBitmapFont(renderer,
                                    commonAsset("fonts\\Amerigo BT_10_b.bmp"));
    game.clockFont = loadBitmapFont(renderer,
                                    commonAsset("fonts\\LCD_14_WHITE.bmp"));
    game.buttonFont = loadBitmapFont(renderer,
                                     commonAsset("fonts\\Amerigo BT_10_c.bmp"));

    // 0x004504b2-0x0045056e: Undo, Hint, New and the high-score button,
    // all at y 541 with localized labels. Hint shows undo_1 when pressed;
    // the high-score button reuses New's art.
    struct ButtonSpec {
        int id;
        int x;
        std::string_view normal;
        std::string_view pressed;
        std::string_view label;
    };
    constexpr std::array<ButtonSpec, 4> kButtons{{
        {kControlUndo, 176, "undo_0", "undo_1", "UNDO"},
        {kControlHint, 293, "hint_0", "undo_1", "HINT"},
        {kControlNew, 409, "new_0", "new_1", "NEW"},
        {kControlHiscore, 525, "new_0", "new_1", "HISCORE"},
    }};
    for (const auto &spec : kButtons) {
        MahjongButton button;
        button.id = spec.id;
        button.x = spec.x;
        button.y = 541;
        SurfacePtr normal = loadGreenKeyedBmp(
            asset("bitmap\\" + std::string(spec.normal) + ".bmp"));
        SurfacePtr pressed = loadGreenKeyedBmp(
            asset("bitmap\\" + std::string(spec.pressed) + ".bmp"));
        button.width = normal->w;
        button.height = normal->h;
        button.normal = makeTexture(renderer, normal.get());
        button.pressed = makeTexture(renderer, pressed.get());
        SDL_SetTextureBlendMode(button.normal.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(button.pressed.get(), SDL_BLENDMODE_BLEND);
        button.normalName = std::string(spec.normal);
        button.pressedName = std::string(spec.pressed);
        button.label = language.get("mahjong", std::string(spec.label),
                                    std::string(spec.label));
        game.buttons.push_back(std::move(button));
    }

    for (auto &button : game.buttons) {
        // The capture shows each label control centred on its button with
        // integer halves: Angre at (213,557) on (176,541), Hint at
        // (336,557), Nytt spill at (440,557), Poengtabell at (548,557).
        button.labelX =
            button.x + (button.width - textWidth(game.buttonFont, button.label)) / 2;
        button.labelY = button.y + (button.height - game.buttonFont.height) / 2;
    }
    for (const auto name : kSounds) {
        MahjongSound sound;
        sound.name = std::string(name);
        // The 4x4 frames are decoded too: they time the clone's removal.
        sound.media = decodeSmacker(activeSceneAnimationData(sound.name),
                                    true, true, true);
        game.sounds.push_back(std::move(sound));
    }

    // 0x0045068c: the backdrop's (25,100,750,400) rectangle under the view.
    if (!sceneBackdrop) {
        throw std::runtime_error("Mahjong needs its backdrop");
    }
    game.background = surfaceTo565(
        sceneBackdrop.get(),
        SDL_Rect{mj::kViewX, mj::kViewY, mj::kViewWidth, mj::kViewHeight});
    game.frame.assign(static_cast<std::size_t>(mj::kViewWidth) *
                          mj::kViewHeight, 0);
    game.frameTexture.reset(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        mj::kViewWidth, mj::kViewHeight));
    if (!game.frameTexture) {
        throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                 SDL_GetError());
    }

    // The tile bitmaps and the environment map, decoded once.
    const auto loadBitmap = [&](std::string_view name) {
        SurfacePtr surface =
            loadBmp(asset("bitmap\\" + std::string(name) + ".bmp"));
        game.bitmaps[std::string(name)] =
            surfaceTo565(surface.get(), SDL_Rect{0, 0, surface->w, surface->h});
    };
    for (int kind = 0; kind < 7; ++kind) {
        for (int rank = 0; rank < mj::kKindCounts[static_cast<std::size_t>(kind)];
             ++rank) {
            loadBitmap(mj::tileTextureName(kind, rank));
        }
    }
    loadBitmap("illum");

    // 0x00450596-0x004505ab: the title shows the scene's localized name
    // until a deal.
    game.titleText = language.get("mahjong", "SCENENAME", "Mah Jongg");
    // 0x00450696: the id buffer, zeroed.
    mahjongIds.assign(mj::kIdBufferSize, 0);
    // 0x00450448-0x00450454: no board, nothing selected, not solved.
    std::cout << "Loaded executable-derived Mahjong: 144-tile turtle with "
                 "the original software 3D board, pointer-tilted camera, "
                 "free-tile and pair rules, Hint, Undo and New\n";
    return game;
}

std::shared_ptr<mahjong::Texture>
MahjongScene::mahjongTexture(std::string_view name) {
    const auto found = mahjongGame->bitmaps.find(std::string(name));
    if (found == mahjongGame->bitmaps.end()) {
        throw std::runtime_error("Mahjong bitmap not loaded: " +
                                 std::string(name));
    }
    // Every bitmap the board uses is 64x64.
    return mj::makeTexture(64, 64, found->second);
}

void MahjongScene::announceMahjongEntry() {
    auto &game = *mahjongGame;
    game.entryAnnounced = true;
    // 0x00450469-0x0045056e: each 0x00406ed0 widget adds its art at layer 1
    // under its id, and a labelled one then adds its label control at
    // layer 2. Help (2) and Exit (1) come first.
    referenceTrace.emit("animation_add", {
        {"asset", "help_0"}, {"x", -1}, {"y", -1}, {"layer", 1},
        {"hotspot", kControlHelp}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {
        {"asset", "exit_0"}, {"x", -1}, {"y", -1}, {"layer", 1},
        {"hotspot", kControlExit}, {"conflict_mask", 0}});
    for (const auto &button : game.buttons) {
        referenceTrace.emit("animation_add", {
            {"asset", button.normalName}, {"x", -1}, {"y", -1}, {"layer", 1},
            {"hotspot", button.id}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_add", {
            {"asset", ""}, {"x", button.labelX}, {"y", button.labelY},
            {"layer", 2}, {"hotspot", 0}, {"conflict_mask", 0}});
    }
    // 0x00450573-0x0045058e: the two text controls are added at layer 0.
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", kTitleRect.x}, {"y", kTitleRect.y},
        {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", kClockRect.x}, {"y", kClockRect.y},
        {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});
}

double MahjongScene::mahjongClockMs() const {
    const auto &game = *mahjongGame;
    if (!game.clockValid) {
        return 0.0;
    }
    if (game.clockStopped) {
        return game.clockFrozenAt;
    }
    return std::floor((const_cast<MahjongScene *>(this)->sceneElapsedNow() -
                       game.clockStartedAt) * 1000.0);
}

void MahjongScene::playMahjongSound(std::string_view name, bool clone) {
    auto &game = *mahjongGame;
    const auto found = std::find_if(
        game.sounds.begin(), game.sounds.end(),
        [&](const MahjongSound &sound) { return sound.name == name; });
    if (found == game.sounds.end()) {
        return;
    }
    // 0x00412c20 clones the descriptor and 0x00412b90 adds the clone at
    // the descriptor's (0,0), layer 0, before playing it. noselect1 is
    // played through 0x00412b90 directly, which adds the static descriptor
    // only when 0x004138f0 finds it not yet added; nothing removes it.
    if (clone || !game.noselectAdded) {
        referenceTrace.emit("animation_add", {
            {"asset", found->name}, {"x", 0}, {"y", 0}, {"layer", 0},
            {"hotspot", 0}, {"conflict_mask", 0}});
    }
    if (!clone) {
        game.noselectAdded = true;
    } else {
        game.clones.push_back(
            {found->name,
             sceneElapsedNow() + originalSmackerCompletionDuration(found->media)});
    }
    audioMixer.addEffect(found->media, audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void MahjongScene::updateMahjongPairs() {
    // 0x00450950.
    auto &game = *mahjongGame;
    const int pairs = static_cast<std::int8_t>(game.board->availablePairs());
    referenceTrace.emit("minigame_state", {
        {"game", "mahjong"}, {"state", "pairs"},
        {"pairs", static_cast<int>(static_cast<std::uint8_t>(pairs))},
        {"tiles", game.board->tilesLeft() & 0xff}});
    if (pairs == 0) {
        game.titleText = language.get("mahjong", "NOMORETILES",
                                      "Ingen mulige trekk");
        const bool cleared = game.board->cleared();
        playMahjongSound(cleared ? "win" : "loose", true);
        game.solved = cleared;
        game.gameOver = true;
        game.dialogPending = true;
        game.dialogDelay = 0.0f;
        // 0x0047c610 stops the stopwatch.
        game.clockFrozenAt = mahjongClockMs();
        game.clockStopped = true;
        return;
    }
    game.titleText = std::to_string(pairs) + " " +
                     language.get("mahjong", "PAIRS", "tilgjengelige par");
}

void MahjongScene::dealMahjong() {
    // 0x00450a50.
    auto &game = *mahjongGame;
    referenceTrace.emit("minigame_state", {{"game", "mahjong"}, {"state", "new"}});
    // 0x0047c5a0 restarts the stopwatch.
    const double now = sceneElapsedNow();
    game.clockStartedAt = now;
    game.clockLast = 0.0;
    game.clockDelta = 0.0;
    game.clockStopped = false;
    game.clockValid = true;
    if (!mahjongRandomSeeded) {
        // The original seeds rand() from time() once at start-up (0x0040bfc4).
        mahjongRandom.seed(static_cast<std::uint32_t>(std::time(nullptr)));
        mahjongRandomSeeded = true;
    }
    if (mahjongPinnedSeed) {
        mahjongRandom.seed(*mahjongPinnedSeed);
    }
    // 0x00450a78: one discarded rand() % 3 before the board is built.
    (void)(mahjongRandom.next() % 3);
    game.board.reset();
    game.board.emplace(
        game.background, 0, [this]() { return mahjongRandom.next(); },
        [this](std::string_view name) { return mahjongTexture(name); });
    // 0x00450a96-0x00450abf: the view surface is (re)added at (25,100),
    // layer 100.
    if (game.boardShown) {
        referenceTrace.emit("animation_remove", {{"asset", ""}, {"reason", "removed"}});
    }
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", mj::kViewX}, {"y", mj::kViewY}, {"layer", 100},
        {"hotspot", 0}, {"conflict_mask", 0}});
    game.selected.reset();
    mahjongClickPending = false;
    game.boardShown = true;
    game.gameOver = false;
    updateMahjongPairs();
}

void MahjongScene::mahjongControl(int id) {
    // 0x00450870.
    auto &game = *mahjongGame;
    referenceTrace.emit("click_dispatch", {{"scene", "mahjong"}, {"hotspot", id}});
    switch (id) {
    case kControlExit:
        returnFromScene();
        return;
    case kControlHelp:
        showSceneHelp();
        return;
    case kControlHint:
        if (game.boardShown && !game.gameOver) {
            game.board->hint();
            playMahjongSound("noselect1", false);
            updateMahjongPairs();
        }
        return;
    case kControlUndo:
        if (game.boardShown && !game.gameOver) {
            game.board->undo();
            updateMahjongPairs();
        }
        return;
    case kControlNew:
        dealMahjong();
        return;
    case kControlHiscore:
        // 0x00450903-0x00450928: 0x0041f640(NULL, 0, row, ranks, 0x101):
        // the same table, ranked lowest first as submitted but shown as
        // hours, minutes and seconds; the row is boxed once solved.
        showScoreTable(activeScene, "", 0U, kScoreLowFirst | kScoreFormatClock,
                       game.solved ? game.lastScoreRow : std::nullopt);
        return;
    default:
        return;
    }
}

void MahjongScene::swapMahjongButtonArt(const MahjongButton &button,
                                       bool pressed) {
    referenceTrace.emit("animation_remove", {
        {"asset", pressed ? button.normalName : button.pressedName},
        {"reason", "removed"}});
    referenceTrace.emit("animation_add", {
        {"asset", pressed ? button.pressedName : button.normalName},
        {"x", -1}, {"y", -1}, {"layer", 1}, {"hotspot", button.id},
        {"conflict_mask", 0}});
}

bool MahjongScene::beginMahjongInteraction(int x, int y) {
    if (!mahjongGame || activeScene != "mahjong") {
        return false;
    }
    auto &game = *mahjongGame;
    // 0x00406b50/0x00406c50: a type-1 button under the press shows its
    // pressed art.
    game.pressedButton.reset();
    for (std::size_t index = 0; index < game.buttons.size(); ++index) {
        const auto &button = game.buttons[index];
        const SDL_Rect bounds{button.x, button.y, button.width, button.height};
        const SDL_Point point{x, y};
        if (SDL_PointInRect(&point, &bounds)) {
            game.pressedButton = index;
        }
    }
    if (game.pressedButton) {
        // 0x00406c50 -> 0x00406740: frame 1 swaps in the pressed art.
        swapMahjongButtonArt(game.buttons[*game.pressedButton], true);
    }
    // 0x00450730: every mouse down in the scene, buttons included, records
    // a click for the next tick.
    mahjongClickPending = true;
    mahjongClickX = x;
    mahjongClickY = y;
    return true;
}

void MahjongScene::moveMahjongPointer(int x, int y, bool buttonHeld) {
    if (!mahjongGame || buttonHeld) {
        return;
    }
    // 0x00450750.
    const mj::CameraAngles angles = mj::pointerCamera(x, y);
    mahjongPitch = angles.pitch;
    mahjongYaw = angles.yaw;
}

void MahjongScene::endMahjongInteraction(int x, int y) {
    if (!mahjongGame) {
        return;
    }
    auto &game = *mahjongGame;
    const auto pressed = game.pressedButton;
    game.pressedButton.reset();
    if (!pressed) {
        return;
    }
    // 0x00406d40 -> 0x00406740: frame 0 restores the normal art first.
    swapMahjongButtonArt(game.buttons[*pressed], false);
    // 0x00406d40: the release posts 0x116 only while the pointer is still
    // over the button.
    const auto &button = game.buttons[*pressed];
    const SDL_Rect bounds{button.x, button.y, button.width, button.height};
    const SDL_Point point{x, y};
    if (SDL_PointInRect(&point, &bounds)) {
        mahjongControl(button.id);
    }
}

void MahjongScene::updateMahjong(double elapsed) {
    (void)elapsed;
    if (!mahjongGame || activeScene != "mahjong") {
        return;
    }
    auto &game = *mahjongGame;
    if (!game.entryAnnounced) {
        announceMahjongEntry();
    }
    // The original's modal message box and Help hold the 0x112 tick.
    if (dialogMode != DialogMode::None) {
        return;
    }
    // 0x00450b08-0x00450b3e: the stopwatch's delta in 10 ms ticks. A
    // stopped stopwatch keeps returning its last delta (0x0047c5e0).
    double deltaMs = 0.0;
    if (game.clockValid) {
        if (!game.clockStopped) {
            const double now = mahjongClockMs();
            game.clockDelta = now - game.clockLast;
            game.clockLast = now;
        }
        deltaMs = game.clockDelta;
    }
    const float dt = static_cast<float>(deltaMs * kTicksPerMillisecond);
    // A finished clone is stopped and removed (SCENE_RemoveAnim 0x00412aa0).
    const double now = sceneElapsedNow();
    for (auto clone = game.clones.begin(); clone != game.clones.end();) {
        if (now >= clone->endsAt) {
            referenceTrace.emit("animation_remove", {
                {"asset", clone->name}, {"reason", "removed"}});
            clone = game.clones.erase(clone);
        } else {
            ++clone;
        }
    }
    if (!game.boardShown) {
        return;
    }
    // 0x00450b4a-0x00450ba8: the clock text, %02d:%02d:%02d from the
    // stopwatch (0x0041ecb0 splits hours, minutes and seconds).
    const int totalMs = static_cast<int>(mahjongClockMs());
    const int seconds = totalMs / 1000;
    char clock[32]{};
    std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", seconds / 3600,
                  (seconds / 60) % 60, seconds % 60);
    game.clockText = clock;
    // 0x00450bd1: draw the board.
    game.board->render(dt, mahjongPitch, mahjongYaw, game.frame.data(),
                       mahjongIds.data());
    // The board is redrawn every tick, so the frame is copied straight
    // into the streaming texture (it is not a Smacker frame to memoize).
    void *texels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(game.frameTexture.get(), nullptr, &texels, &pitch) == 0) {
        // The original's 16-bit screen reaches the display with zero low
        // bits (a 565 blue of 9 shows as 72, the capture's (32,36,72) at
        // (40,110)), not with the high bits replicated.
        for (int row = 0; row < mj::kViewHeight; ++row) {
            auto *out = reinterpret_cast<std::uint32_t *>(
                static_cast<std::uint8_t *>(texels) +
                static_cast<std::ptrdiff_t>(row) * pitch);
            const std::uint16_t *in =
                game.frame.data() + static_cast<std::ptrdiff_t>(row) * mj::kViewWidth;
            for (int column = 0; column < mj::kViewWidth; ++column) {
                const std::uint32_t pixel = in[column];
                out[column] = 0xff000000U | ((pixel & 0xf800U) << 8) |
                              ((pixel & 0x07e0U) << 5) | ((pixel & 0x001fU) << 3);
            }
        }
        SDL_UnlockTexture(game.frameTexture.get());
    }
    // 0x00450be4-0x00450c88: the Game Over/Solved message 600 ms later.
    if (game.dialogPending) {
        game.dialogDelay += dt;
        if (kDialogDelay < game.dialogDelay) {
            game.dialogPending = false;
            const std::string title = game.solved
                ? language.get("mahjong", "SOLVED", "Gratulerer!")
                : language.get("mahjong", "GAMEOVER", "Spillet er over");
            const std::string text = game.solved
                ? language.get("mahjong", "SOLVED_TEXT", "")
                : language.get("mahjong", "GAMEOVER_TEXT", "");
            showMessageWithTitle(title, text, MessageKind::Notice);
            // 0x00450c62-0x00450c83: a solved board then submits the
            // stopwatch (0x0047c5c0) as 0x0041ee50(0, time, NULL, 0x102,
            // 0x0068f508, 0x0068f430). UNCERTAIN: the original's message box
            // is modal, so it submits after the box closes; native submits
            // as it opens.
            if (game.solved) {
                game.lastScoreRow = submitSceneScore(
                    static_cast<std::uint32_t>(std::max(0.0, mahjongClockMs())),
                    0U, kScoreTimeFlags);
            }
        }
    }
    // 0x00450c8b-0x00450de5: a pending click, resolved against the id
    // buffer this tick's render has just written.
    if (!mahjongClickPending || game.gameOver) {
        return;
    }
    mahjongClickPending = false;
    const auto picked =
        game.board->pick(mahjongClickX, mahjongClickY, mahjongIds.data());
    if (!picked) {
        referenceTrace.emit("minigame_state", {
            {"game", "mahjong"}, {"state", "miss"}});
        if (game.selected) {
            game.board->deselect(*game.selected);
            game.selected.reset();
            playMahjongSound("noselect2", true);
        }
        return;
    }
    const auto &tile = game.board->tile(*picked);
    referenceTrace.emit("minigame_state", {
        {"game", "mahjong"}, {"state", "pick"},
        {"cell_x", tile.column}, {"cell_y", tile.row}, {"layer", tile.layer},
        {"kind", tile.kind}, {"rank", tile.rank}});
    if (game.selected) {
        if (!game.board->blocked(*picked) &&
            !game.board->blocked(*game.selected) &&
            mj::Board::matches(tile, game.board->tile(*game.selected))) {
            game.board->removePair(*picked, *game.selected);
            playMahjongSound("select1", true);
            updateMahjongPairs();
            game.selected.reset();
            return;
        }
        game.board->deselect(*game.selected);
        game.selected = *picked;
        game.board->select(*picked);
        playMahjongSound("select2", true);
        return;
    }
    playMahjongSound("select2", true);
    game.selected = *picked;
    game.board->select(*picked);
}

void MahjongScene::renderMahjong(double elapsed) {
    (void)elapsed;
    if (!mahjongGame || activeScene != "mahjong") {
        return;
    }
    auto &game = *mahjongGame;
    if (game.boardShown) {
        const SDL_Rect view{mj::kViewX, mj::kViewY, mj::kViewWidth,
                            mj::kViewHeight};
        SDL_RenderCopy(renderer, game.frameTexture.get(), nullptr, &view);
    }
    drawCenteredText(renderer, game.titleFont, game.titleText, kTitleRect);
    if (game.boardShown) {
        drawCenteredText(renderer, game.clockFont, game.clockText, kClockRect);
    }
    for (std::size_t index = 0; index < game.buttons.size(); ++index) {
        const auto &button = game.buttons[index];
        const SDL_Rect bounds{button.x, button.y, button.width, button.height};
        // 0x00406840: the pressed art shows only while the pointer is on
        // the pressed button.
        const SDL_Point point{mouseX, mouseY};
        const bool pressed = game.pressedButton == index &&
                             SDL_PointInRect(&point, &bounds);
        SDL_RenderCopy(renderer,
                       pressed ? button.pressed.get() : button.normal.get(),
                       nullptr, &bounds);
        drawText(renderer, game.buttonFont, button.label, button.labelX,
                 button.labelY);
    }
}

bool MahjongScene::executeMahjongDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "mahjong:";
    if (command.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string_view action = command.substr(prefix.size());
    // mahjong:seed:<n> pins the rand() state every New starts from, as the
    // capture fixture's 0x0048d32b breakpoint does in the original.
    constexpr std::string_view seedPrefix = "seed:";
    if (action.rfind(seedPrefix, 0) == 0) {
        const auto seed = parseUnsigned(action.substr(seedPrefix.size()));
        if (!seed) {
            return false;
        }
        mahjongPinnedSeed = *seed;
        return true;
    }
    if (!mahjongGame || activeScene != "mahjong") {
        return false;
    }
    if (action == "new") {
        mahjongControl(kControlNew);
        return true;
    }
    if (action == "hint") {
        mahjongControl(kControlHint);
        return true;
    }
    if (action == "undo") {
        mahjongControl(kControlUndo);
        return true;
    }
    // mahjong:wait-board holds the command queue until a dealt board has
    // finished its two-second fly-in.
    if (action == "wait-board") {
        return mahjongGame->board && mahjongGame->board->flyIn() >= 1.0f;
    }
    // mahjong:wait-idle holds the queue while a removal or hint blink is
    // still animating.
    if (action == "wait-idle") {
        if (!mahjongGame->board) {
            return true;
        }
        const auto &tiles = mahjongGame->board->tiles();
        return std::none_of(tiles.begin(), tiles.end(), [](const auto &tile) {
            return tile.removing || tile.blinking;
        });
    }
    // mahjong:pointer:<x>:<y> delivers a button-up pointer move (0x108).
    constexpr std::string_view pointerPrefix = "pointer:";
    if (action.rfind(pointerPrefix, 0) == 0) {
        const std::string_view rest = action.substr(pointerPrefix.size());
        const auto colon = rest.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const auto x = parseUnsigned(rest.substr(0, colon));
        const auto y = parseUnsigned(rest.substr(colon + 1));
        if (!x || !y) {
            return false;
        }
        moveMahjongPointer(static_cast<int>(*x), static_cast<int>(*y), false);
        return true;
    }
    // mahjong:click:<x>:<y> is a pointer move to (x, y), a press and a
    // release there, as the capture runner delivers a left click.
    constexpr std::string_view clickPrefix = "click:";
    if (action.rfind(clickPrefix, 0) == 0) {
        const std::string_view rest = action.substr(clickPrefix.size());
        const auto colon = rest.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const auto x = parseUnsigned(rest.substr(0, colon));
        const auto y = parseUnsigned(rest.substr(colon + 1));
        if (!x || !y) {
            return false;
        }
        const int px = static_cast<int>(*x);
        const int py = static_cast<int>(*y);
        moveMahjongPointer(px, py, false);
        mouseX = px;
        mouseY = py;
        beginMahjongInteraction(px, py);
        endMahjongInteraction(px, py);
        return true;
    }
    // mahjong:plan prints every free tile with a click point for it, and
    // the free matching pairs, for scripting captures.
    if (action == "plan") {
        if (!mahjongGame->board) {
            return false;
        }
        auto &board = *mahjongGame->board;
        std::vector<int> free;
        for (int index = 0; index < mj::kTileCount; ++index) {
            const auto &tile = board.tile(index);
            if (!tile.alive || board.blocked(index)) {
                continue;
            }
            free.push_back(index);
            const auto point = board.clickPoint(index);
            std::cout << "mahjong plan: tile " << index << " "
                      << mj::tileTextureName(tile.kind, tile.rank) << " col "
                      << tile.column << " row " << tile.row << " layer "
                      << tile.layer << " click "
                      << (point ? std::to_string(point->first) + "," +
                                      std::to_string(point->second)
                                : std::string("none"))
                      << '\n';
        }
        for (std::size_t a = 0; a < free.size(); ++a) {
            for (std::size_t b = a + 1; b < free.size(); ++b) {
                if (mj::Board::matches(board.tile(free[a]), board.tile(free[b]))) {
                    std::cout << "mahjong plan: pair " << free[a] << " "
                              << free[b] << '\n';
                }
            }
        }
        return true;
    }
    // mahjong:dump-frame:<path> writes the 750x400 view as a binary PPM.
    constexpr std::string_view dumpPrefix = "dump-frame:";
    if (action.rfind(dumpPrefix, 0) == 0) {
        const std::string path(action.substr(dumpPrefix.size()));
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            return false;
        }
        std::fprintf(file, "P6\n%d %d\n255\n", mj::kViewWidth, mj::kViewHeight);
        for (const std::uint16_t pixel : mahjongGame->frame) {
            const unsigned char rgb[3]{
                static_cast<unsigned char>(((pixel >> 11) & 0x1f) << 3),
                static_cast<unsigned char>(((pixel >> 5) & 0x3f) << 2),
                static_cast<unsigned char>((pixel & 0x1f) << 3)};
            std::fwrite(rgb, 1, sizeof(rgb), file);
        }
        std::fclose(file);
        return true;
    }
    return false;
}

} // namespace fgp
