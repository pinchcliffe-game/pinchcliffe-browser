#include "game/app_fence.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "media/smacker.h"
#include "util/text.h"

namespace fgp {
namespace {

// Names the module builds descriptor names from.
// 0x00508b84: duration names, indexed by FenceNote::duration.
constexpr std::array<const char *, 8> kDurationNames{{
    "quaver", "quaver-dot", "crotchet", "crotchet-dot",
    "minim", "minim-dot", "semibreve", "semibreve-dot",
}};
// 0x00509248: the landing clip for each duration.
constexpr std::array<const char *, 8> kLandingNames{{
    "crotchet", "crotchet", "crotchet", "crotchet",
    "minim", "minim", "semibreve", "semibreve",
}};
// 0x00508ba4: the sound suffix of each pitch; 0x00508bec its letter.
constexpr std::array<const char *, 18> kPitchSounds{{
    "A1", "B1", "C1", "D1", "E1", "F1", "G1", "A2", "B2",
    "C2", "D2", "E2", "F2", "G2", "A3", "B3", "C3", "D3",
}};
constexpr std::array<const char *, 18> kPitchLetters{{
    "A", "B", "C", "D", "E", "F", "G", "A", "B",
    "C", "D", "E", "F", "G", "A", "B", "C", "D",
}};
// 0x0049fe98: note lengths in eighths of a beat; 0x0049fea0 (= 8) is
// also the divisor 0x00467fa5 uses.
const auto kDurationEighths = originalArray<int, 8>(0x49fe98U);
constexpr int kEighthsPerBeat = 8;
// 0x00509228-0x00509244: where a bird stands relative to its note, as
// (x, y) pairs for the take-off and landing frames of up and down stems.
constexpr int kTakeOffUpX = 0, kTakeOffUpY = 9;        // 0x00509228
constexpr int kTakeOffDownX = 0, kTakeOffDownY = 3;    // 0x00509230
constexpr int kLandUpX = 9, kLandUpY = -7;             // 0x00509238
constexpr int kLandDownX = -9, kLandDownY = -1;        // 0x00509240
// Slot geometry: 0x0046665d places slot i at x = i * 0x2b + 0x32.
constexpr int kSlotWidth = 0x2b;
constexpr int kSlotLeft = 0x32;
constexpr int kSlots = 16;
constexpr int kMaximumNotes = 0x400;   // 0x00467c08
// Hotspots of the scene's hotspots.bmp and its controls.
constexpr std::uint32_t kPlankFirst = 1U;          // 1-18: pitches 0-17
constexpr std::uint32_t kPlankLast = 18U;
constexpr std::uint32_t kFreeButton = 0x13U;
constexpr std::uint32_t kPlayButton = 0x14U;
constexpr std::uint32_t kRecordButton = 0x15U;
constexpr std::uint32_t kMusicStand = 0x17U;
constexpr std::uint32_t kInstruments = 0x18U;
constexpr std::uint32_t kLettersBoard = 0x19U;
constexpr std::uint32_t kPowerBox = 0x1aU;
constexpr std::uint32_t kBpmUp = 0x1bU;
constexpr std::uint32_t kBpmDown = 0x1cU;
constexpr std::uint32_t kLoadControl = 0x1dU;
constexpr std::uint32_t kSaveControl = 0x1eU;
constexpr std::uint32_t kHelpControl = 0x1fU;
constexpr std::uint32_t kExitControl = 0x20U;
constexpr std::uint32_t kScrollControl = 0x21U;
constexpr std::uint32_t kNoteHotspot = 0x22U;     // 0x22-0x31, per slot
// Panel buttons 0x00466c60-0x00466d48 create, (id, x, y, images,
// caption). Help and Exit are the shell's corner buttons.
struct FencePanelButton {
    std::uint32_t id;
    int x;
    int y;
    const char *image;
    const char *caption;
};
constexpr std::array<FencePanelButton, 5> kPanelButtons{{
    {kBpmUp, 0xf9, 0x222, "bpminc", nullptr},
    {kBpmDown, 0xbf, 0x224, "bpmdec", nullptr},
    {kInstruments, 0x1e3, 0x224, "instruments", nullptr},
    {kLoadControl, 0x229, 0x222, "load", "LOAD"},
    {kSaveControl, 0x229, 0x23d, "save", "SAVE"},
}};
// Text controls: FREE/PLAY/RECORD labels (0x0049fe58-0x0049fe78) and the
// tempo box (0x0049fe88), left/top/right/bottom.
constexpr SDL_Rect kFreeLabel{0x111, 0x223, 0x14d - 0x111, 0x247 - 0x223};
constexpr SDL_Rect kPlayLabel{0x153, 0x223, 0x18f - 0x153, 0x247 - 0x223};
constexpr SDL_Rect kRecordLabel{0x194, 0x223, 0x1d0 - 0x194, 0x247 - 0x223};
constexpr SDL_Rect kBpmText{0xd4, 0x224, 0xf3 - 0xd4, 0x242 - 0x224};
// Scroll control 0x21 (0x00466d6c): the sock on the clothes line.
constexpr SDL_Rect kScrollTrack{0x28, 0xe6, 0x2cb, 0x23};
// Bird paths: 0x0049fed0/0x0049fed4 (arrival spread), 0x0049fed8 and
// 0x0049fedc (departure), and the float constants they combine with.
constexpr int kArrivalSpreadX = 0x190;
constexpr int kArrivalSpreadY = 0x12c;
constexpr int kDepartureSpreadX = 0x339;
constexpr int kDepartureRise = 0xc8;
constexpr float kRandomScale = 3.0518509447574615e-05F;  // 0x0049d610
constexpr float kHalf = 0.5F;                            // 0x0049c300

// The x87 ftol helper 0x0048c214 truncates toward zero.
int ftol(double value) { return static_cast<int>(value); }

TexturePtr keyedTexture(SDL_Renderer *renderer,
                        std::span<const std::uint8_t> data) {
    SurfacePtr surface = loadBmp(data);
    const Uint32 key = pixelValue(surface.get(), 0, 0);
    if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0)
        throw std::runtime_error(SDL_GetError());
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

TexturePtr opaqueTexture(SDL_Renderer *renderer,
                         std::span<const std::uint8_t> data) {
    SurfacePtr surface = loadBmp(data);
    return makeTexture(renderer, surface.get());
}

std::string updown(int pitch) { return pitch > 7 ? "down" : "up"; }

} // namespace

// ---------------------------------------------------------------------
// Loading

FenceScene::FenceGame FenceScene::loadFenceGame(Package &package) {
    FenceGame game;
    const auto asset = [&](std::string_view name)
        -> std::optional<std::span<const std::uint8_t>> {
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        return std::nullopt;
    };
    // 0x00466bd0 loads instrument.ini through 0x00413910's scene path.
    // UNCERTAIN: which package layer the original's file layer resolves
    // first; this takes the topmost (Data3 lists 18 instruments, Data1 10).
    if (const auto ini = asset("instrument.ini")) {
        const LanguageTable table(*ini);
        for (int index = 0; index < 0x18; ++index) {
            const std::string resource = trim(table.get(
                "instrument", "resource" + std::to_string(index)));
            const std::string name = trim(table.get(
                "instrument", "name" + std::to_string(index)));
            if (name.empty()) break;
            game.instruments.emplace_back(resource, name);
        }
    }
    if (const auto birds = asset("sound\\birds.wav"))
        game.birdsSound = decodeSmacker(*birds, false, true, true);
    std::cout << "Loaded executable-derived Fence: " << game.instruments.size()
              << " instruments, stave of 16 bird notes, free/play/record "
                 "modes and tunes from Data/fence/tunes\n";
    return game;
}

const TexturePtr &FenceScene::fenceBitmap(const std::string &name) {
    auto &game = *fenceGame;
    const auto found = game.bitmaps.find(name);
    if (found != game.bitmaps.end()) return found->second;
    TexturePtr texture;
    for (Package *candidate : {
             sceneLanguagePackage.get(), sceneAddonPackage.get(),
             sceneData2Package.get(), sceneEditionPackage.get(),
             scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry =
                candidate->findOptional("bitmap\\" + name + ".bmp")) {
            const auto data = candidate->data(*entry);
            SurfacePtr probe = loadBmp(data);
            const Uint32 corner = pixelValue(probe.get(), 0, 0);
            const SDL_PixelFormat *format = probe->format;
            Uint8 r = 0, g = 0, b = 0;
            SDL_GetRGB(corner, format, &r, &g, &b);
            // Static descriptors carry +0x48, so their top-left pixel is
            // the colour key (scene_DrawAnim 0x00413050). Panel buttons
            // without a chroma corner are drawn opaque by the control code.
            texture = (r == 0 && g == 255 && b == 0) ||
                              name.find('_') == std::string::npos
                          ? keyedTexture(renderer, data)
                          : opaqueTexture(renderer, data);
            break;
        }
    }
    return game.bitmaps.emplace(name, std::move(texture)).first->second;
}

const DecodedMedia &FenceScene::fenceClip(const std::string &name) {
    auto &game = *fenceGame;
    const auto found = game.clips.find(name);
    if (found != game.clips.end()) return found->second;
    // Every clip here is a bird's. Both bird descriptors set +0x48 when they
    // are built (0x00467242/0x0046726c, 0x004687d0/0x004687fd), so
    // scene_DrawAnim keys them on their top-left pixel and the notes' black
    // palette index 0 is drawn.
    DecodedMedia media = decodeSmacker(
        activeSceneAnimationData(name), true, false, true, 0, nullptr,
        SmackerIndexZeroAlpha::Transparent, SmackerKeying::TopLeftRgb565);
    if (!media.videoFrames.empty()) {
        const auto &frame = media.videoFrames.front();
        TexturePtr texture(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            frame.width, frame.height));
        if (texture) SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        game.clipTextures[name] = std::move(texture);
    }
    return game.clips.emplace(name, std::move(media)).first->second;
}

// ---------------------------------------------------------------------
// Descriptor bookkeeping mirrored into the reference trace.

void FenceScene::addFenceSprite(FenceSprite &sprite, const std::string &name,
                                int x, int y, int layer,
                                std::uint32_t hotspot) {
    sprite.name = name;
    sprite.added = true;
    sprite.x = x;
    sprite.y = y;
    sprite.layer = layer;
    referenceTrace.emit("animation_start", {
        {"asset", name}, {"x", x}, {"y", y}, {"layer", layer},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", 0}});
}

// SCENE_AddAnim(-1, -1) takes the descriptor's default position; the
// trace logs the -1 arguments.
void FenceScene::addFenceSpriteAtDefault(FenceSprite &sprite,
                                         const std::string &name, int x,
                                         int y, int layer) {
    addFenceSprite(sprite, name, -1, -1, layer);
    sprite.x = x;
    sprite.y = y;
}

void FenceScene::removeFenceSprite(FenceSprite &sprite) {
    if (!sprite.added) return;
    sprite.added = false;
    referenceTrace.emit("animation_remove", {
        {"asset", sprite.name}, {"reason", "removed"}});
}

// SCENE_PlayAnim 0x00412b90 for a bird: add at the descriptor's default
// position, which the bird descriptors never set (+0x54/+0x58 and the
// layer at +0x64 stay 0), then start the clip when +0x40 is set.
void FenceScene::addFenceBird(FenceBird &bird, int x, int y, int layer,
                              double now) {
    bird.added = true;
    bird.layer = layer;
    bird.playing = false;
    referenceTrace.emit("animation_start", {
        {"asset", bird.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"loop", bird.loop}});
    referenceTrace.emit("animation_add", {
        {"asset", bird.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", 0}, {"conflict_mask", 0}});
    bird.x = x;
    bird.y = y;
    if (bird.animated) {
        fenceClip(bird.name);
        bird.playing = true;
        bird.startedAt = now;
    }
}

void FenceScene::removeFenceBird(FenceBird &bird) {
    if (!bird.added) return;
    bird.added = false;
    bird.playing = false;
    referenceTrace.emit("animation_remove", {
        {"asset", bird.name}, {"reason", "removed"}});
}

// MSVC rand() (0x0048d323) and the bounded helper 0x0040c530.
int FenceScene::fenceRandom() {
    auto &game = *fenceGame;
    if (!game.randomSeeded) return static_cast<int>(randomEngine() & 0x7fffU);
    game.randomState = game.randomState * 0x343fdU + 0x269ec3U;
    return static_cast<int>((game.randomState >> 16) & 0x7fffU);
}

int FenceScene::fenceRandomBelow(int bound) {
    return bound != 0 ? fenceRandom() % bound : 0;
}

// 0x00466810/0x00466840: the wires sag as a parabola; a pitch climbs
// 8.5 pixels.
int FenceScene::fenceStaveY(int x, int pitch) const {
    const double along = (static_cast<double>(x) - 395.0) / 367.0;
    const double sag = along * along * -6.0 + 182.0;
    return ftol(sag - static_cast<double>(pitch) * 8.5 - 42.0);
}

// 0x004664b0: the sock's range is (count - 16) / 16 + 1 pages and its value
// the page; 0x00408eb0 puts the thumb at x + width * value / range.
int FenceScene::fenceScrollRange() const {
    return std::max(0, (static_cast<int>(fenceTune.notes.size()) - 16) / 16 + 1);
}

SDL_Rect FenceScene::fenceSockBounds() const {
    const int range = fenceScrollRange();
    const int value = std::clamp(fenceGame->page / 16, 0, range);
    const int position = range > 0
        ? kScrollTrack.x + kScrollTrack.w * value / range : kScrollTrack.x;
    // sock.bmp is 13x37, centred on the line by 0x00412cf0.
    return SDL_Rect{position - 13 / 2,
                    kScrollTrack.y + kScrollTrack.h / 2 - 37 / 2, 13, 37};
}

// Event 0x10a on the scroll control (0x00409270 -> 0x00408f30): the thumb
// follows the pointer along the track and a changed value posts 0x117,
// which 0x00466b70 turns into the page.
void FenceScene::dragFenceSock(int x) {
    auto &game = *fenceGame;
    const int range = fenceScrollRange();
    const int position = std::clamp(x, kScrollTrack.x,
                                    kScrollTrack.x + kScrollTrack.w);
    const double exact = static_cast<double>(position - kScrollTrack.x) *
                         range / kScrollTrack.w;
    const int value = static_cast<int>(exact > 0.0 ? exact + 0.5 : exact - 0.5);
    if (value == game.page / 16) return;
    game.page = value * 16;
    layoutFenceNotes();
    placeFenceCursor();
}

// ---------------------------------------------------------------------
// Scene initialisation 0x00466b90

void FenceScene::enterFence() {
    auto &game = *fenceGame;
    game.entered = true;
    const double now = sceneElapsedNow();
    game.lastUpdate = now;
    game.cursor = 0;
    game.page = 0;
    game.letters = false;
    game.dragSlot = -1;
    // 0x00467400 names the ledger descriptors "line" and gives each note
    // slot hotspot 0x22 + slot.
    for (auto &slot : game.ledgers)
        for (auto &line : slot) line.name = "line";
    selectFenceInstrument(0);
    game.departureSpread = 10;
    enterFenceFree();
    const auto traceAdd = [this](std::string_view asset, int x, int y,
                                 int layer, int hotspot) {
        referenceTrace.emit("animation_start", {
            {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
            {"loop", false}});
        referenceTrace.emit("animation_add", {
            {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
            {"hotspot", hotspot}, {"conflict_mask", 0}});
    };
    // Text controls 0x00407a30/0x00407930 add a nameless descriptor at the
    // text's position: 0x004124e0 centres the measured text in the rect.
    const auto traceText = [&](std::string_view text, const SDL_Rect &rect,
                               int layer) {
        traceAdd("", rect.x + (rect.w - textWidth(dialogFont, text)) / 2,
                 rect.y + (rect.h - dialogFont.height) / 2, layer, 0);
    };
    // 0x00466c60-0x00466d48: the panel buttons, each added through 0x004067f8
    // at its default position on layer 0 with its id as hotspot; the Load
    // and Save captions follow their buttons on layer 1.
    traceAdd("help_0", -1, -1, 0, static_cast<int>(kHelpControl));
    traceAdd("exit_0", -1, -1, 0, static_cast<int>(kExitControl));
    for (const auto &button : kPanelButtons) {
        traceAdd(std::string(button.image) + "_0", -1, -1, 0,
                 static_cast<int>(button.id));
        if (button.caption) {
            int w = 0, h = 0;
            if (const auto &texture =
                    fenceBitmap(std::string(button.image) + "_0"))
                SDL_QueryTexture(texture.get(), nullptr, nullptr, &w, &h);
            traceText(language.get("fence", button.caption),
                      SDL_Rect{button.x, button.y, w, h}, 1);
        }
    }
    // 0x00466d6c: scroll control 0x21 adds its "sock" thumb (0x00408f30).
    traceAdd("sock", -1, -1, 0, static_cast<int>(kScrollControl));
    // 0x00466d9d-0x00466e06: the FREE/PLAY/RECORD labels on layer 3 and
    // the tempo box on layer 2.
    traceText(language.get("fence", "FREE", "Fri"), kFreeLabel, 3);
    traceText(language.get("fence", "PLAY", "Spill"), kPlayLabel, 3);
    traceText(language.get("fence", "RECORD", "Komp."), kRecordLabel, 3);
    traceAdd("", kBpmText.x, kBpmText.y, 2, 0);
    loadFenceTune(std::nullopt, now);
    // 0x00466e1d-0x00466e39: the car part while profile key 0 is unset.
    const bool carpart = std::any_of(
        sceneCollectibles.begin(), sceneCollectibles.end(),
        [](const SceneCollectible &collectible) {
            return collectible.name == "carpart" && collectible.visible;
        });
    if (carpart) traceAdd("carpart", -1, -1, -10, -1);
}

// 0x00466e50: pick the instrument and load its 18 note sounds.
void FenceScene::selectFenceInstrument(int index) {
    auto &game = *fenceGame;
    game.instrument = index;
    std::string resource = "piano";
    if (index >= 0 && index < static_cast<int>(game.instruments.size()) &&
        !game.instruments[static_cast<std::size_t>(index)].first.empty())
        resource = game.instruments[static_cast<std::size_t>(index)].first;
    for (std::size_t pitch = 0; pitch < game.sounds.size(); ++pitch) {
        game.sounds[pitch] = {};
        const std::string name =
            "sound\\" + resource + "-" + kPitchSounds[pitch] + ".wav";
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(),
                 scenePackage.get()}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name)) {
                game.sounds[pitch] =
                    decodeSmacker(candidate->data(*entry), false, true, true);
                break;
            }
        }
    }
}

// 0x004673b0: clamp to 60-240 and print it in the tempo box.
void FenceScene::setFenceBpm(int bpm) {
    if (bpm >= 0xf0) bpm = 0xf0;
    else if (bpm < 0x3d) bpm = 0x3c;
    fenceTune.bpm = bpm;
}

// 0x004674c0: the lit mode button, one descriptor renamed per mode.
void FenceScene::setFenceModeButton(int control) {
    auto &game = *fenceGame;
    removeFenceSprite(game.modeButton);
    if (control == static_cast<int>(kFreeButton))
        addFenceSprite(game.modeButton, "free_1", 0x112, 0x224, 2);
    else if (control == static_cast<int>(kPlayButton))
        addFenceSprite(game.modeButton, "play_1", 0x153, 0x225, 2);
    else if (control == static_cast<int>(kRecordButton))
        addFenceSprite(game.modeButton, "edit_1", 0x192, 0x223, 2);
}

// 0x00467480
void FenceScene::enterFenceFree() {
    auto &game = *fenceGame;
    game.mode = FenceMode::Free;
    setFenceModeButton(static_cast<int>(kFreeButton));
    resetFencePage();
}

// 0x004674a0
void FenceScene::resetFencePage() {
    auto &game = *fenceGame;
    game.page = 0;
    game.cursor = 0;
    layoutFenceNotes();
    placeFenceCursor();
}

// 0x00467f00
void FenceScene::enterFencePlay(double now) {
    auto &game = *fenceGame;
    if (game.mode == FenceMode::Play) {
        if (game.previousMode == FenceMode::Free) {
            enterFenceFree();
            return;
        }
        if (game.previousMode == FenceMode::Record) {
            enterFenceRecord();
            return;
        }
    }
    game.mode = FenceMode::Play;
    setFenceModeButton(static_cast<int>(kPlayButton));
    if (!game.player.added)
        addFenceSpriteAtDefault(game.player, "player", 47, 484, 2);
    resetFencePage();
    game.cursor = 0;
    playNextFenceNote(now);
}

// 0x00468020
void FenceScene::enterFenceRecord() {
    auto &game = *fenceGame;
    game.mode = FenceMode::Record;
    setFenceModeButton(static_cast<int>(kRecordButton));
    if (!game.mike.added) addFenceSpriteAtDefault(game.mike, "mike", 4, 441, 2);
    scrollFenceToCursor();
    layoutFenceNotes();
    placeFenceCursor();
}

// 0x004675b0, 0x004675d0 and 0x00467620, chosen by the current state.
void FenceScene::leaveFenceMode() {
    auto &game = *fenceGame;
    if (game.mode == FenceMode::Free) {
        game.previousMode = FenceMode::Free;
        setFenceModeButton(-1);
    } else if (game.mode == FenceMode::Play) {
        stopFencePlay();
    } else if (game.mode == FenceMode::Record) {
        game.previousMode = FenceMode::Record;
        setFenceModeButton(-1);
        removeFenceSprite(game.mike);
    }
}

// 0x004675d0
void FenceScene::stopFencePlay() {
    auto &game = *fenceGame;
    setFenceModeButton(-1);
    removeFenceSprite(game.player);
    game.playTimerAt.reset();
}

// ---------------------------------------------------------------------
// The stave

// 0x004665a0: rebuild the 16 visible note slots.
void FenceScene::layoutFenceNotes() {
    auto &game = *fenceGame;
    for (int slot = 0; slot < kSlots; ++slot) {
        for (auto &line : game.ledgers[static_cast<std::size_t>(slot)])
            removeFenceSprite(line);
        removeFenceSprite(game.letterLabels[static_cast<std::size_t>(slot)]);
        removeFenceSprite(game.notes[static_cast<std::size_t>(slot)]);
    }
    const int count = static_cast<int>(fenceTune.notes.size());
    const int end = std::min(game.page + kSlots, count);
    for (int index = game.page; index < end; ++index) {
        const int slot = index - game.page;
        const auto &note = fenceTune.notes[static_cast<std::size_t>(index)];
        const int x = slot * kSlotWidth + kSlotLeft;
        auto &sprite = game.notes[static_cast<std::size_t>(slot)];
        addFenceSprite(sprite,
                       std::string(kDurationNames[static_cast<std::size_t>(
                           note.duration & 7)]) + "-" + updown(note.pitch),
                       x, fenceStaveY(x, note.pitch), 4,
                       kNoteHotspot + static_cast<std::uint32_t>(slot));
        auto &ledgers = game.ledgers[static_cast<std::size_t>(slot)];
        if (game.letters && note.pitch >= 0 && note.pitch < 18)
            addFenceSprite(game.letterLabels[static_cast<std::size_t>(slot)],
                           kPitchLetters[static_cast<std::size_t>(note.pitch)],
                           x, fenceStaveY(x, note.pitch), 3);
        if (note.pitch > 13) {
            addFenceSprite(ledgers[0], "line", x, fenceStaveY(x, 14), 2);
            if (note.pitch > 15)
                addFenceSprite(ledgers[1], "line", x, fenceStaveY(x, 16), 2);
        }
        if (note.pitch < 3) {
            addFenceSprite(ledgers[0], "line", x, fenceStaveY(x, 2), 2);
            if (note.pitch < 1)
                addFenceSprite(ledgers[1], "line", x, fenceStaveY(x, 0), 2);
        }
    }
}

// 0x004664f0: the yellow ring on the note at the cursor.
void FenceScene::placeFenceCursor() {
    auto &game = *fenceGame;
    if (game.mode == FenceMode::Free || game.mode == FenceMode::Record) {
        const int count = static_cast<int>(fenceTune.notes.size());
        const int end = std::min(game.page + kSlots, count);
        if (game.page <= game.cursor && game.cursor < end) {
            const auto &note =
                game.notes[static_cast<std::size_t>(game.cursor - game.page)];
            if (!game.cursorSprite.added) {
                addFenceSprite(game.cursorSprite, "cursor", note.x, note.y, 1);
            } else {
                game.cursorSprite.x = note.x;
                game.cursorSprite.y = note.y;
            }
            return;
        }
    }
    removeFenceSprite(game.cursorSprite);
}

// 0x00466460: move the page so the cursor is on it.
void FenceScene::scrollFenceToCursor() {
    auto &game = *fenceGame;
    if (game.cursor < game.page)
        game.page -= ((game.page - game.cursor + 15) >> 4) * 16;
    while (game.page + 16 <= game.cursor) game.page += 16;
    if (game.page < 1) game.page = 0;
}

// 0x00467cb0: step the cursor, turning the page after the last slot.
bool FenceScene::advanceFenceCursor() {
    auto &game = *fenceGame;
    ++game.cursor;
    const int count = static_cast<int>(fenceTune.notes.size());
    if (count <= game.cursor) {
        removeFenceSprite(game.marker);
        return false;
    }
    if (game.page + 16 <= game.cursor) {
        game.page += 16;
        if (count <= game.page) {
            game.page = 0;
            game.cursor = 0;
        }
        if (game.marker.added) {
            if (game.cursor - game.page == 0) {
                removeFenceSprite(game.marker);
            } else {
                // 0x00467d58 moves it with 0x00412cc0 (not traced).
                const int x = (game.cursor - game.page) * kSlotWidth + 7;
                game.marker.x = x;
                game.marker.y = fenceStaveY(
                    x, fenceTune.notes[static_cast<std::size_t>(
                           game.cursor - 1)].pitch);
            }
        }
        layoutFenceNotes();
        placeFenceCursor();
        return game.page != 0;
    }
    placeFenceCursor();
    return true;
}

// 0x00467dc0: sound the plank and mark the note being played.
void FenceScene::playFenceNote(int pitch, double now) {
    auto &game = *fenceGame;
    if (pitch >= 0 && pitch < 18 &&
        !game.sounds[static_cast<std::size_t>(pitch)].audioSamples.empty()) {
        audioMixer.addEffect(game.sounds[static_cast<std::size_t>(pitch)],
                             audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
    removeFenceSprite(game.marker);
    const int slot = game.page < game.cursor ? game.cursor - game.page : 0;
    const int x = slot * kSlotWidth + kSlotLeft;
    addFenceSprite(game.marker, "marker-" + updown(pitch), x,
                   fenceStaveY(x, pitch), 5);
    // EVENT_AddTimer(now + 250, 0, 1); 0x00468380 removes the marker.
    game.markerTimerAt = now + 0.250;
}

// 0x00467bc0: a plank (or number key) for the current state.
void FenceScene::pressFencePlank(int pitch, double now) {
    auto &game = *fenceGame;
    if (game.mode == FenceMode::Free) {
        playFenceNote(pitch, now);
        const int count = static_cast<int>(fenceTune.notes.size());
        if (game.cursor < count &&
            pitch == fenceTune.notes[static_cast<std::size_t>(game.cursor)].pitch)
            advanceFenceCursor();
    } else if (game.mode == FenceMode::Play) {
        playFenceNote(pitch, now);
    } else if (game.mode == FenceMode::Record &&
               static_cast<int>(fenceTune.notes.size()) < kMaximumNotes) {
        const int count = static_cast<int>(fenceTune.notes.size());
        game.cursor = count < 1 ? 0 : game.cursor + 1;
        scrollFenceToCursor();
        fenceTune.notes.insert(fenceTune.notes.begin() + game.cursor,
                               FenceNote{pitch, 2});
        playFenceNote(pitch, now);
        layoutFenceNotes();
        placeFenceCursor();
    }
}

// 0x00467f70: play the note at the cursor and time the next one.
bool FenceScene::playNextFenceNote(double now) {
    auto &game = *fenceGame;
    // With an empty tune the original reads the stale first entry.
    const FenceNote note =
        game.cursor < static_cast<int>(fenceTune.notes.size())
            ? fenceTune.notes[static_cast<std::size_t>(game.cursor)]
            : FenceNote{};
    playFenceNote(note.pitch, now);
    const int delayMs =
        kDurationEighths[static_cast<std::size_t>(note.duration & 7)] * 60000 /
        kEighthsPerBeat / fenceTune.bpm;
    if (advanceFenceCursor()) {
        game.playTimerAt = now + delayMs / 1000.0;
        return true;
    }
    if (game.mode == FenceMode::Play) {
        stopFencePlay();
        if (game.previousMode == FenceMode::Free) enterFenceFree();
        else enterFenceRecord();
    }
    return false;
}

// 0x00468070: left press on a note in record mode starts dragging it.
void FenceScene::beginFenceNoteDrag(int slot) {
    auto &game = *fenceGame;
    game.dragSlot = slot;
    game.dragging = true;
}

// Event 0x10a, 0x00466870: the dragged note follows the pointer onto the
// nearest wire position; below the lowest it shows the trash sign.
void FenceScene::dragFenceNote(int x, int y) {
    auto &game = *fenceGame;
    if (game.dragSlot < 0) return;
    const int index = game.page + game.dragSlot;
    if (index >= static_cast<int>(fenceTune.notes.size())) return;
    auto &note = fenceTune.notes[static_cast<std::size_t>(index)];
    auto &sprite = game.notes[static_cast<std::size_t>(game.dragSlot)];
    const int slotX = game.dragSlot * kSlotWidth + kSlotLeft;
    // The descriptor is 84 pixels high (+0x94); the pointer is its centre.
    const int top = y - 84 / 2;
    int pitch = 0;
    while (pitch < 0x12 && fenceStaveY(slotX, pitch) >= top) ++pitch;
    if (pitch > 0x10) pitch = 0x11;
    note.pitch = pitch;
    removeFenceSprite(sprite);
    addFenceSprite(sprite,
                   std::string(kDurationNames[static_cast<std::size_t>(
                       note.duration & 7)]) + "-" + updown(pitch),
                   slotX, fenceStaveY(slotX, pitch), 2,
                   kNoteHotspot + static_cast<std::uint32_t>(game.dragSlot));
    if (fenceStaveY(slotX, -1) < top) {
        if (!game.trash.added) addFenceSprite(game.trash, "trash", x, y, 5);
        // 0x00412cf0 centres both on the pointer (not traced).
        sprite.x = x - 22;
        sprite.y = y - 42;
        game.trash.x = x - 22;
        game.trash.y = y - 42;
        game.trashShown = true;
        return;
    }
    game.trashShown = false;
    auto &ledgers = game.ledgers[static_cast<std::size_t>(game.dragSlot)];
    for (auto &line : ledgers) removeFenceSprite(line);
    removeFenceSprite(game.letterLabels[static_cast<std::size_t>(game.dragSlot)]);
    removeFenceSprite(game.trash);
    if (game.letters)
        addFenceSprite(game.letterLabels[static_cast<std::size_t>(game.dragSlot)],
                       kPitchLetters[static_cast<std::size_t>(pitch)], slotX,
                       fenceStaveY(slotX, pitch), 3);
    if (pitch > 13) {
        addFenceSprite(ledgers[0], "line", slotX, fenceStaveY(slotX, 14), 2);
        if (pitch > 15)
            addFenceSprite(ledgers[1], "line", slotX, fenceStaveY(slotX, 16), 2);
    }
    if (pitch < 3) {
        addFenceSprite(ledgers[0], "line", slotX, fenceStaveY(slotX, 2), 2);
        if (pitch < 1)
            addFenceSprite(ledgers[1], "line", slotX, fenceStaveY(slotX, 0), 2);
    }
}

// Event 0x10b, 0x004663c0: drop; over the trash the note is deleted.
void FenceScene::dropFenceNote() {
    auto &game = *fenceGame;
    if (game.trash.added) {
        const int index = game.dragSlot + game.page;
        if (index >= 0 && index < static_cast<int>(fenceTune.notes.size()))
            fenceTune.notes.erase(fenceTune.notes.begin() + index);
        const int count = static_cast<int>(fenceTune.notes.size());
        if (game.cursor < 1) game.cursor = 0;
        else if (count <= game.cursor) game.cursor = count - 1;
        scrollFenceToCursor();
        removeFenceSprite(game.trash);
    }
    game.dragSlot = -1;
    game.dragging = false;
    game.trashShown = false;
    layoutFenceNotes();
    placeFenceCursor();
}

// Event 0x107, 0x004680a0: right-click a note in record mode to cycle its
// value; with Shift held it toggles the dot.
void FenceScene::cycleFenceNoteValue(int slot, bool dotted) {
    auto &game = *fenceGame;
    game.cursor = game.page + slot;
    if (game.cursor >= static_cast<int>(fenceTune.notes.size())) return;
    auto &duration =
        fenceTune.notes[static_cast<std::size_t>(game.cursor)].duration;
    if (dotted) duration += (duration % 2 != 0) ? -1 : 1;
    else duration = (duration + 2) % 8;
    layoutFenceNotes();
    placeFenceCursor();
}

// Hotspot 0x19 (the blackboard): show the letter under each note.
void FenceScene::toggleFenceLetters() {
    auto &game = *fenceGame;
    game.letters = !game.letters;
    if (game.letters)
        addFenceSpriteAtDefault(game.lettersBoard, "letters", 0, 450, 2);
    else removeFenceSprite(game.lettersBoard);
    layoutFenceNotes();
}

// ---------------------------------------------------------------------
// Tunes

// 0x004672e0: 0xff, a 1024-byte name, tempo, count, count * {pitch,
// duration} as 32-bit integers.
bool FenceScene::readFenceTune(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    if (file.get() != 0xff) return false;
    std::string name(0x400, '\0');
    if (!file.read(name.data(), 0x400)) return false;
    const int bpm = file.get();
    if (bpm == EOF) return false;
    const int count = file.get();
    if (count == EOF) return false;
    fenceTune.notes.clear();
    fenceTune.bpm = 0x78;
    std::vector<FenceNote> notes(static_cast<std::size_t>(count));
    for (auto &note : notes) {
        std::array<unsigned char, 8> raw{};
        if (!file.read(reinterpret_cast<char *>(raw.data()), 8)) return false;
        const auto word = [&](std::size_t offset) {
            return static_cast<int>(
                static_cast<std::uint32_t>(raw[offset]) |
                (static_cast<std::uint32_t>(raw[offset + 1]) << 8) |
                (static_cast<std::uint32_t>(raw[offset + 2]) << 16) |
                (static_cast<std::uint32_t>(raw[offset + 3]) << 24));
        };
        note = {word(0), word(4)};
    }
    fenceTune.notes = std::move(notes);
    fenceTune.name = name.substr(0, name.find('\0'));
    setFenceBpm(bpm);
    return true;
}

// 0x00468260, after 0x00468130 picked the first free tunes/tune%d.tun.
void FenceScene::saveFenceTune(const std::string &name) {
    const fs::path directory = gameDirectory / "Data" / "fence" / "tunes";
    std::error_code error;
    fs::create_directories(directory, error);
    fs::path path;
    for (int index = 0;; ++index) {
        path = directory / ("tune" + std::to_string(index) + ".tun");
        if (!fs::exists(path)) break;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    file.put(static_cast<char>(0xff));
    std::string buffer = name.substr(0, 0x20);
    buffer.resize(0x400, '\0');
    file.write(buffer.data(), 0x400);
    file.put(static_cast<char>(fenceTune.bpm & 0xff));
    file.put(static_cast<char>(fenceTune.notes.size() & 0xff));
    for (const auto &note : fenceTune.notes) {
        for (const int value : {note.pitch, note.duration}) {
            const auto word = static_cast<std::uint32_t>(value);
            for (int shift = 0; shift < 32; shift += 8)
                file.put(static_cast<char>((word >> shift) & 0xff));
        }
    }
    const std::string saved = "tunes/" + path.filename().string();
    std::string message = language.get("fence", "SAVED",
                                       "Sangen er lagret som '%s'.");
    if (const auto at = message.find("%s"); at != std::string::npos)
        message.replace(at, 2, saved);
    showMessageWithTitle(language.get("fence", "SAVETITLE", "Lagre Sang"),
                         message);
}

std::vector<fs::path> fenceTuneFiles(const fs::path &gameDirectory);

// 0x00466ee0: birds carry the old tune away, the new one is read, and
// 0x00468520 later brings it in. index empty = the default empty tune.
void FenceScene::loadFenceTune(std::optional<int> index, double now) {
    auto &game = *fenceGame;
    leaveFenceMode();
    startFenceDeparture(now);
    if (!index) {
        fenceTune.notes.clear();
        setFenceBpm(0x78);
        fenceTune.name = language.get("fence", "DEFTUNE", "Min melodi");
    } else {
        const auto files = fenceTuneFiles(gameDirectory);
        if (*index < 0 || *index >= static_cast<int>(files.size()) ||
            !readFenceTune(files[static_cast<std::size_t>(*index)])) {
            std::string message = language.get(
                "fence", "IOERR", "Feil ved lesing av fil '%s'.");
            showMessageWithTitle(language.get("fence", "IOERR_TITLE", "Fil feil"),
                                 message);
            return;
        }
    }
    game.cursor = 0;
    game.page = 0;
    for (int slot = 0; slot < kSlots; ++slot) {
        for (auto &line : game.ledgers[static_cast<std::size_t>(slot)])
            removeFenceSprite(line);
        removeFenceSprite(game.letterLabels[static_cast<std::size_t>(slot)]);
        removeFenceSprite(game.notes[static_cast<std::size_t>(slot)]);
    }
    removeFenceSprite(game.marker);
    removeFenceSprite(game.cursorSprite);
}

// ---------------------------------------------------------------------
// Birds

void FenceScene::playFenceBirdsSound() {
    auto &game = *fenceGame;
    if (game.birdsEffect != LiveAudioMixer::kInvalidEffectHandle) return;
    if (game.birdsSound.audioSamples.empty()) return;
    game.birdsEffect = audioMixer.addEffect(
        game.birdsSound, audioVolumes[0] * sceneAudioGains[0]);
    game.birdsSoundEndsAt =
        sceneElapsedNow() +
        static_cast<double>(game.birdsSound.audioSamples.size()) /
            std::max(1, game.birdsSound.audioRate *
                            game.birdsSound.audioChannels);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

void FenceScene::stopFenceBirdsSound() {
    auto &game = *fenceGame;
    if (game.birdsEffect == LiveAudioMixer::kInvalidEffectHandle) return;
    audioMixer.removeEffect(game.birdsEffect);
    game.birdsEffect = LiveAudioMixer::kInvalidEffectHandle;
}

// 0x00467060: every note on the stave takes off.
void FenceScene::startFenceDeparture(double now) {
    auto &game = *fenceGame;
    if (game.randomSeeded) game.randomState = game.randomSeed;
    game.birds.clear();
    for (int slot = 0; slot < kSlots; ++slot) {
        const auto &sprite = game.notes[static_cast<std::size_t>(slot)];
        if (!sprite.added) continue;
        const int index = game.page + slot;
        const FenceNote note =
            index < static_cast<int>(fenceTune.notes.size())
                ? fenceTune.notes[static_cast<std::size_t>(index)]
                : FenceNote{};
        spawnFenceDepartureBird(sprite.x, sprite.y, note, now);
    }
    // 0x0040c370("wait") and the birds loop 0x00413a40(birds, 1).
    playFenceBirdsSound();
    game.mode = FenceMode::BirdsLeaving;
}

// 0x004670e0
void FenceScene::spawnFenceDepartureBird(int x, int y, const FenceNote &note,
                                         double now) {
    auto &game = *fenceGame;
    FenceBird bird;
    const bool up = note.pitch < 8;
    bird.count = fenceRandomBelow(10) + 10;
    bird.path.assign(static_cast<std::size_t>(bird.count) + 1, {});
    const int spread = up ? kDepartureSpreadX : -kDepartureSpreadX;
    for (int point = 0; point <= bird.count; ++point) {
        const float s = static_cast<float>(
            static_cast<double>(point) / static_cast<double>(bird.count - 1));
        const int dx = ftol(std::pow(static_cast<double>(s), 2.0) * spread);
        bird.path[static_cast<std::size_t>(point)].x = x - dx;
        const int random = fenceRandom();
        const double wobble =
            static_cast<double>(kHalf - static_cast<float>(random) * kRandomScale) *
            game.departureSpread;
        const int dy = ftol(wobble + static_cast<double>(kDepartureRise) * s);
        bird.path[static_cast<std::size_t>(point)].y = y - dy;
    }
    bird.path[0] = {x, y};
    bird.pitch = note.pitch;
    bird.duration = note.duration;
    bird.t = 0.0F;
    bird.speed = (kHalf - static_cast<float>(fenceRandom()) * kRandomScale) *
                     0.2F + 0.4F;
    bird.delay = (kHalf - static_cast<float>(fenceRandom()) * kRandomScale) *
                     3.0F + 1.5F;
    bird.state = 5;
    bird.animated = false;
    bird.loop = true;
    bird.name = std::string(kDurationNames[static_cast<std::size_t>(
                    note.duration & 7)]) + "-" + updown(note.pitch);
    const int layer = static_cast<int>(game.birds.size()) + 6;
    game.birds.push_back(std::move(bird));
    addFenceBird(game.birds.back(), x, y, layer, now);
}

// 0x00468520: birds for the first 16 notes of the new tune fly in.
void FenceScene::startFenceArrival(double now) {
    auto &game = *fenceGame;
    if (game.randomSeeded) game.randomState = game.randomSeed;
    game.birds.clear();
    const int count =
        std::min(kSlots, static_cast<int>(fenceTune.notes.size()));
    for (int index = 0; index < count; ++index) {
        const int x = kSlotLeft + index * kSlotWidth;
        const auto &note = fenceTune.notes[static_cast<std::size_t>(index)];
        spawnFenceArrivalBird(x, fenceStaveY(x, note.pitch), note, now);
    }
    playFenceBirdsSound();
    game.mode = FenceMode::BirdsArriving;
}

// 0x004685a0
void FenceScene::spawnFenceArrivalBird(int x, int y, const FenceNote &note,
                                       double now) {
    auto &game = *fenceGame;
    FenceBird bird;
    const int spreadX =
        ftol(static_cast<double>(kHalf - static_cast<float>(fenceRandom()) *
                                             kRandomScale) *
             (kArrivalSpreadX / 2)) + kArrivalSpreadX;
    const int spreadY =
        ftol(static_cast<double>(kHalf - static_cast<float>(fenceRandom()) *
                                             kRandomScale) *
             (kArrivalSpreadY / 2)) + kArrivalSpreadY;
    const bool up = note.pitch < 8;
    const int targetX = x + (up ? kTakeOffUpX + kLandUpX
                                : kTakeOffDownX + kLandDownX);
    const int targetY = y + (up ? kTakeOffUpY + kLandUpY
                                : kTakeOffDownY + kLandDownY);
    bird.count = fenceRandomBelow(10) + 10;
    bird.path.assign(static_cast<std::size_t>(bird.count) + 1, {});
    const int signedX = up ? -spreadX : spreadX;
    for (int point = 0; point < bird.count; ++point) {
        const float s = static_cast<float>(
            1.0 - static_cast<double>(point) /
                      static_cast<double>(bird.count - 1));
        bird.path[static_cast<std::size_t>(point)].x =
            targetX - ftol(static_cast<double>(signedX) * s);
        bird.path[static_cast<std::size_t>(point)].y =
            targetY - ftol(std::pow(static_cast<double>(s), 2.0) *
                           static_cast<double>(spreadY));
    }
    bird.path[static_cast<std::size_t>(bird.count)] = {targetX, targetY};
    bird.path[static_cast<std::size_t>(bird.count) - 1] = {targetX, targetY};
    bird.pitch = note.pitch;
    bird.duration = note.duration;
    bird.t = 0.0F;
    bird.speed = (kHalf - static_cast<float>(fenceRandom()) * kRandomScale) *
                     0.2F + 0.4F;
    bird.delay = (kHalf - static_cast<float>(fenceRandom()) * kRandomScale) *
                     4.0F + 2.0F;
    bird.state = 0;
    bird.animated = true;
    bird.loop = true;
    bird.name = up ? "fly-up" : "fly-down";
    const int layer = static_cast<int>(game.birds.size()) + 6;
    const SDL_Point start = bird.path[0];
    game.birds.push_back(std::move(bird));
    addFenceBird(game.birds.back(), start.x, start.y, layer, now);
}

// 0x00468870: one bird for one update. Returns false once it has gone.
bool FenceScene::stepFenceBird(FenceBird &bird, float seconds, double now) {
    const std::size_t last = static_cast<std::size_t>(bird.count) - 1U;
    const int toTarget = std::abs(bird.x - bird.path[last].x);
    const int fromStart = std::abs(bird.path[0].x - bird.x);
    if (!bird.added) return false;
    const bool up = bird.pitch < 8;
    const auto advance = [&] {
        if (bird.t < 1.0F) bird.t = seconds * bird.speed + bird.t;
    };
    const auto replay = [&](std::string name, bool animated, bool loop) {
        removeFenceBird(bird);
        bird.name = std::move(name);
        bird.animated = animated;
        bird.loop = loop;
        addFenceBird(bird, 0, 0, 0, now);
    };
    switch (bird.state) {
    case 0:
        bird.delay -= seconds;
        if (bird.delay <= 0.0F) bird.state = 1;
        break;
    case 1:
        advance();
        if (toTarget < 150) {
            replay(up ? "land-init-up" : "land-init-down", true, false);
            bird.state = 2;
        }
        break;
    case 2:
        advance();
        if (toTarget == 0) {
            bird.path[last].x -= up ? kTakeOffUpX : kTakeOffDownX;
            bird.path[last + 1].x = bird.path[last].x;
            bird.path[last].y -= up ? kTakeOffUpY : kTakeOffDownY;
            bird.path[last + 1].y = bird.path[last].y;
            replay(up ? "land-up" : "land-down", true, false);
            bird.state = 3;
        }
        break;
    case 3:
        if (toTarget == 0 && !bird.playing) {
            bird.path[last].x -= up ? kLandUpX : kLandDownX;
            bird.path[last + 1].x = bird.path[last].x;
            bird.path[last].y -= up ? kLandUpY : kLandDownY;
            bird.path[last + 1].y = bird.path[last].y;
            replay(std::string(kLandingNames[static_cast<std::size_t>(
                       bird.duration & 7)]) + "-" + updown(bird.pitch),
                   true, false);
            bird.state = 4;
        }
        break;
    case 4:
        return true;
    case 5:
        bird.delay -= seconds;
        if (bird.delay <= 0.0F) {
            bird.path[0].x -= up ? kTakeOffUpX + kLandUpX
                                 : kTakeOffDownX + kLandDownX;
            bird.path[0].y -= up ? kTakeOffUpY + kLandUpY
                                 : kTakeOffDownY + kLandDownY;
            replay(up ? "land-init-up" : "land-init-down", true, true);
            bird.state = 6;
        }
        break;
    case 6:
        advance();
        if (fromStart > 100) {
            replay(up ? "fly-up" : "fly-down", true, true);
            bird.state = 7;
        }
        break;
    case 7:
        advance();
        if (bird.t >= 1.0F) {
            removeFenceBird(bird);
            return false;
        }
        break;
    default:
        break;
    }
    // Interpolate along the polyline: segment ftol((count-1) * t), the
    // fraction fmod of the same float by 1.0 (0x004688e9-0x00468960).
    const float along = static_cast<float>(
        static_cast<double>(bird.count - 1) * static_cast<double>(bird.t));
    const int segment = ftol(static_cast<double>(bird.count - 1) *
                             static_cast<double>(bird.t));
    const double fraction = std::fmod(static_cast<double>(along), 1.0);
    const std::size_t i = static_cast<std::size_t>(
        std::clamp(segment, 0, bird.count - 1));
    bird.x = bird.path[i].x +
             ftol(static_cast<double>(bird.path[i + 1].x - bird.path[i].x) *
                  fraction);
    bird.y = bird.path[i].y +
             ftol(static_cast<double>(bird.path[i + 1].y - bird.path[i].y) *
                  fraction);
    return true;
}

// 0x00468420
void FenceScene::updateFenceBirds(float seconds, double now) {
    auto &game = *fenceGame;
    int active = 0;
    for (auto &bird : game.birds) {
        const int state = bird.state;
        const bool always = state == 2 || state == 3 || state == 5;
        if (always || bird.playing) {
            if (stepFenceBird(bird, seconds, now)) ++active;
        }
    }
    if (active != 0) return;
    if (game.mode == FenceMode::BirdsArriving) {
        for (auto &bird : game.birds) removeFenceBird(bird);
        game.birds.clear();
        stopFenceBirdsSound();
        enterFenceFree();
    } else if (game.mode == FenceMode::BirdsLeaving) {
        startFenceArrival(now);
    }
}

// ---------------------------------------------------------------------
// The list chooser

namespace {
// listdlg: centred 320x483 (0x00407070 with 0x140, 0x1e3), rows 22 pixels
// from (260,119) (capture fence-explore), OK at dialog + (0xd7, 0x1ab)
// when there is no Cancel (0x00420550).
constexpr SDL_Point kListOrigin{240, 58};
constexpr SDL_Rect kListTitle{256, 70, 288, 30};
constexpr int kListRowX = 260;
constexpr int kListRowY = 119;
constexpr int kListRowHeight = 22;
constexpr int kListRowWidth = 238;
constexpr int kListRows = 16;
constexpr SDL_Point kListOk{240 + 0xd7, 58 + 0x1ab};
} // namespace

std::vector<fs::path> fenceTuneFiles(const fs::path &gameDirectory) {
    std::vector<fs::path> files;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(
             gameDirectory / "Data" / "fence" / "tunes", error)) {
        if (entry.path().extension() == ".tun" &&
            entry.path().filename().string().front() != '.')
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// 0x00467a20: every tunes/*.tun with the 0xff mark, by its stored name.
void FenceScene::openFenceTuneList() {
    auto &game = *fenceGame;
    FenceList list;
    list.tunes = true;
    list.title = language.get("fence", "TUNES", "Velg sang");
    for (const auto &path : fenceTuneFiles(gameDirectory)) {
        std::ifstream file(path, std::ios::binary);
        std::string name(0x400, '\0');
        if (file.get() == 0xff) file.read(name.data(), 0x400);
        list.items.push_back(name.substr(0, name.find('\0')));
        list.files.push_back(path);
    }
    if (list.items.empty()) {
        showMessageWithTitle(
            language.get("fence", "NOTUNES_TITLE", "Ingen sanger"),
            language.get("fence", "NOTUNES",
                         "Det er dessverre ingen sanger tilgjengelig."));
        return;
    }
    game.list = std::move(list);
}

// 0x00467990: the instrument ini's name%d keys, localised.
void FenceScene::openFenceInstrumentList() {
    auto &game = *fenceGame;
    FenceList list;
    list.title = language.get("fence", "INSTRUMENTS", "Velg instrument");
    for (const auto &[resource, key] : game.instruments) {
        const auto colon = key.find(':');
        list.items.push_back(
            colon == std::string::npos
                ? key
                : language.get(key.substr(0, colon), key.substr(colon + 1), key));
    }
    game.list = std::move(list);
}

// 0x00420960: OK (Enter, or the OK button) returns the selection, Cancel
// (Escape) -1.
void FenceScene::closeFenceList(bool accept) {
    auto &game = *fenceGame;
    if (!game.list) return;
    FenceList list = std::move(*game.list);
    game.list.reset();
    if (!accept || list.selection < 0 ||
        list.selection >= static_cast<int>(list.items.size()))
        return;
    if (list.tunes) {
        loadFenceTune(list.selection, sceneElapsedNow());
    } else {
        selectFenceInstrument(list.selection);
    }
}

// Typed text for the save prompt (event 0x10e in 0x004201d0).
bool FenceScene::typeFenceText(const char *utf8) {
    if (!fenceGame || !fenceGame->saveName) return false;
    auto &name = *fenceGame->saveName;
    const auto cp1252 = utf8ToCp1252(utf8);
    if (cp1252) {
        name += *cp1252;
        if (name.size() > 0x20) name.resize(0x20);
    }
    return true;
}

bool FenceScene::pressFenceList(int x, int y) {
    auto &game = *fenceGame;
    auto &list = *game.list;
    const SDL_Point point{x, y};
    int w = 83, h = 27;
    if (game.listButton)
        SDL_QueryTexture(game.listButton.get(), nullptr, nullptr, &w, &h);
    const SDL_Rect ok{kListOk.x, kListOk.y, w, h};
    if (SDL_PointInRect(&point, &ok)) {
        closeFenceList(true);
        return true;
    }
    for (int row = 0; row < std::min(kListRows,
                                     static_cast<int>(list.items.size()));
         ++row) {
        const SDL_Rect bounds{kListRowX - 2, kListRowY + row * kListRowHeight,
                              kListRowWidth, kListRowHeight};
        if (SDL_PointInRect(&point, &bounds)) list.selection = row;
    }
    return true;
}

void FenceScene::renderFenceList() {
    auto &game = *fenceGame;
    const auto &list = *game.list;
    if (!game.listBackdrop) {
        if (const auto *entry =
                common.findOptional("dialogue\\listdlg\\backdrop.bmp"))
            game.listBackdrop = keyedTexture(renderer, common.data(*entry));
        if (const auto *entry =
                common.findOptional("dialogue\\listdlg\\button_0.bmp"))
            game.listButton = opaqueTexture(renderer, common.data(*entry));
    }
    if (game.listBackdrop) {
        SDL_Rect destination{kListOrigin.x, kListOrigin.y, 0, 0};
        SDL_QueryTexture(game.listBackdrop.get(), nullptr, nullptr,
                         &destination.w, &destination.h);
        SDL_RenderCopy(renderer, game.listBackdrop.get(), nullptr, &destination);
    }
    drawCenteredText(renderer, calendarFont, list.title, kListTitle);
    for (int row = 0; row < std::min(kListRows,
                                     static_cast<int>(list.items.size()));
         ++row) {
        const int y = kListRowY + row * kListRowHeight;
        if (row == list.selection) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 120, 100, 70, 200);
            const SDL_Rect bar{kListRowX - 2, y, kListRowWidth, kListRowHeight};
            SDL_RenderFillRect(renderer, &bar);
        }
        drawText(renderer, calendarFont,
                 list.items[static_cast<std::size_t>(row)], kListRowX, y);
    }
    if (game.listButton) {
        SDL_Rect destination{kListOk.x, kListOk.y, 0, 0};
        SDL_QueryTexture(game.listButton.get(), nullptr, nullptr,
                         &destination.w, &destination.h);
        SDL_RenderCopy(renderer, game.listButton.get(), nullptr, &destination);
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "OK", "OK"), destination);
    }
}

// ---------------------------------------------------------------------
// Input

// Event 0x105, 0x00467790.
void FenceScene::dispatchFenceHotspot(std::uint32_t hotspot, double now) {
    auto &game = *fenceGame;
    if (game.mode == FenceMode::BirdsArriving ||
        game.mode == FenceMode::BirdsLeaving)
        return;
    const bool editable =
        game.mode == FenceMode::Free || game.mode == FenceMode::Record;
    if (hotspot >= kPlankFirst && hotspot <= kPlankLast) {
        pressFencePlank(static_cast<int>(hotspot - kPlankFirst), now);
    } else if (hotspot == kFreeButton || hotspot == kPlayButton ||
               hotspot == kRecordButton) {
        leaveFenceMode();
        if (hotspot == kFreeButton) enterFenceFree();
        else if (hotspot == kPlayButton) enterFencePlay(now);
        else enterFenceRecord();
    } else if (hotspot == kMusicStand && editable) {
        dispatchFenceControl(kLoadControl, now);
    } else if (hotspot == kInstruments && editable) {
        openFenceInstrumentList();
    } else if (hotspot == kLettersBoard && editable) {
        toggleFenceLetters();
    } else if (hotspot == kPowerBox && editable) {
        loadFenceTune(std::nullopt, now);
    } else if (hotspot == kBpmUp) {
        setFenceBpm(fenceTune.bpm + 10);
    } else if (hotspot == kBpmDown) {
        setFenceBpm(fenceTune.bpm - 10);
    } else if (hotspot >= kNoteHotspot && hotspot < kNoteHotspot + 16U &&
               game.mode == FenceMode::Record) {
        const int slot = static_cast<int>(hotspot - kNoteHotspot);
        beginFenceNoteDrag(slot);
        game.cursor = game.page + slot;
        placeFenceCursor();
    }
}

// Event 0x116, 0x00468130: panel controls in the free and record states.
void FenceScene::dispatchFenceControl(std::uint32_t control, double) {
    auto &game = *fenceGame;
    if (game.mode != FenceMode::Free && game.mode != FenceMode::Record) return;
    if (control == kLoadControl) {
        openFenceTuneList();
    } else if (control == kSaveControl) {
        // 0x00468130: 0x0041ffb0 at (0xf9,0xf7) asks for at most 0x20
        // characters under fence:SAVETITLE, starting from the tune's name.
        game.saveName = fenceTune.name.substr(0, 0x20);
        SDL_StartTextInput();
    } else if (control == kHelpControl) {
        showSceneHelp();
    } else if (control == kExitControl) {
        returnFromScene();
    }
}

bool FenceScene::fenceControlsEnabled() const {
    return !fenceGame || fenceGame->mode == FenceMode::Free ||
           fenceGame->mode == FenceMode::Record;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
FenceScene::fenceHotspotAt(int x, int y) const {
    if (!fenceGame || activeScene != "fence") return std::nullopt;
    const auto &game = *fenceGame;
    const SDL_Point point{x, y};
    // Panel buttons are controls tested before the scene.
    for (const auto &button : kPanelButtons) {
        int w = 0, h = 0;
        const auto found = game.bitmaps.find(std::string(button.image) + "_0");
        if (found != game.bitmaps.end() && found->second)
            SDL_QueryTexture(found->second.get(), nullptr, nullptr, &w, &h);
        const SDL_Rect bounds{button.x, button.y, w, h};
        if (SDL_PointInRect(&point, &bounds)) return std::pair{button.id, bounds};
    }
    // The sock thumb carries the scroll control's id as its hotspot.
    const SDL_Rect sock = fenceSockBounds();
    if (SDL_PointInRect(&point, &sock)) return std::pair{kScrollControl, sock};
    // 0x004136c0 tests descriptors with hotspots from the top: the notes
    // (mode 2, their bounding box).
    for (int slot = kSlots - 1; slot >= 0; --slot) {
        const auto &note = game.notes[static_cast<std::size_t>(slot)];
        if (!note.added) continue;
        const SDL_Rect bounds{note.x, note.y, 44, 84};
        if (SDL_PointInRect(&point, &bounds))
            return std::pair{kNoteHotspot + static_cast<std::uint32_t>(slot),
                             bounds};
    }
    if (sceneHotspots && x >= 0 && y >= 0 && x < sceneHotspots->w &&
        y < sceneHotspots->h) {
        const auto value = pixelValue(sceneHotspots.get(), x, y);
        if ((value >= kPlankFirst && value <= kPlankLast) ||
            (value >= kFreeButton && value <= kPowerBox && value != 0x16U))
            return std::pair{static_cast<std::uint32_t>(value),
                             SDL_Rect{x, y, 1, 1}};
    }
    return std::nullopt;
}

bool FenceScene::beginFenceInteraction(int x, int y, bool rightButton) {
    if (!fenceGame || activeScene != "fence") return false;
    auto &game = *fenceGame;
    const double now = sceneElapsedNow();
    if (game.list) return rightButton || pressFenceList(x, y);
    if (game.saveName) {
        // OK (0xc9) and Cancel (0xca) at dialog + (0xf,0x45)/(0xcc,0x45).
        const SDL_Point point{x, y};
        const SDL_Rect ok{0xf9 + 0xf, 0xf7 + 0x45, 83, 27};
        const SDL_Rect cancel{0xf9 + 0xcc, 0xf7 + 0x45, 83, 27};
        if (!rightButton && SDL_PointInRect(&point, &ok)) {
            const std::string name = *game.saveName;
            game.saveName.reset();
            SDL_StopTextInput();
            if (!name.empty()) saveFenceTune(name);
        } else if (!rightButton && SDL_PointInRect(&point, &cancel)) {
            game.saveName.reset();
            SDL_StopTextInput();
        }
        return true;
    }
    // During the bird phases 0x00467060 set the wait cursor, and the
    // shell delivers no mouse events to the scene (capture fence-explore:
    // no 0x105 for clicks then).
    if (game.mode == FenceMode::BirdsArriving ||
        game.mode == FenceMode::BirdsLeaving)
        return true;
    const auto hit = fenceHotspotAt(x, y);
    // The car part (hotspot -1) goes through the shell's collectible path.
    // UNCERTAIN: whether 0x105 reaches 0x00467790 for a click on no
    // hotspot at all; none is dispatched here.
    if (!hit) return !sceneCollectibleAt(x, y).has_value();
    const auto hotspot = hit->first;
    if (rightButton) {
        // Event 0x107 reaches 0x004680a0 with the hotspot under the
        // pointer; only record-mode notes react.
        if (hotspot >= kNoteHotspot && hotspot < kNoteHotspot + 16U &&
            game.mode == FenceMode::Record) {
            const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            cycleFenceNoteValue(static_cast<int>(hotspot - kNoteHotspot), shift);
        }
        return true;
    }
    referenceTrace.emit("click_dispatch", {
        {"scene", "fence"}, {"hotspot", hotspot}});
    for (const auto &button : kPanelButtons) {
        if (button.id == hotspot) game.pressedControl = hotspot;
    }
    if (hotspot == kScrollControl) {
        game.scrollDragging = true;
        return true;
    }
    dispatchFenceHotspot(hotspot, now);
    return true;
}

void FenceScene::moveFenceInteraction(int x, int y) {
    if (!fenceGame) return;
    if (fenceGame->dragging) dragFenceNote(x, y);
    if (fenceGame->scrollDragging) dragFenceSock(x);
}

void FenceScene::endFenceInteraction(int x, int y) {
    if (!fenceGame) return;
    auto &game = *fenceGame;
    game.scrollDragging = false;
    if (game.dragging) dropFenceNote();
    if (game.pressedControl != 0U) {
        const auto control = game.pressedControl;
        game.pressedControl = 0U;
        const auto hit = fenceHotspotAt(x, y);
        if (hit && hit->first == control) {
            referenceTrace.emit("click_dispatch", {
                {"scene", "fence"}, {"hotspot", control}});
            dispatchFenceControl(control, sceneElapsedNow());
        }
    }
}

// Event 0x10c, 0x00468330: number keys 1-9 play pitches; Left holds the
// low register (0-8), Right the high one (9-17), neither 2-10.
bool FenceScene::handleFenceKey(SDL_Keycode key) {
    if (!fenceGame || activeScene != "fence") return false;
    auto &game = *fenceGame;
    if (game.saveName) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            const std::string name = *game.saveName;
            game.saveName.reset();
            SDL_StopTextInput();
            // 0x00468156: an empty name saves nothing.
            if (!name.empty()) saveFenceTune(name);
        } else if (key == SDLK_ESCAPE) {
            game.saveName.reset();
            SDL_StopTextInput();
        } else if (key == SDLK_BACKSPACE && !game.saveName->empty()) {
            game.saveName->pop_back();
        }
        return true;
    }
    if (game.list) {
        // 0x00420a30: Enter is OK, Escape Cancel, the rest move the list.
        auto &list = *game.list;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) closeFenceList(true);
        else if (key == SDLK_ESCAPE) closeFenceList(false);
        else if (key == SDLK_UP) list.selection = std::max(0, list.selection - 1);
        else if (key == SDLK_DOWN)
            list.selection = std::min(static_cast<int>(list.items.size()) - 1,
                                      list.selection + 1);
        return true;
    }
    if (game.mode == FenceMode::BirdsArriving ||
        game.mode == FenceMode::BirdsLeaving)
        return false;
    if (key < SDLK_1 || key > SDLK_9) return false;
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    int base = 2;
    if (keys[SDL_SCANCODE_LEFT]) base = 0;
    else if (keys[SDL_SCANCODE_RIGHT]) base = 9;
    pressFencePlank(base + static_cast<int>(key - SDLK_1), sceneElapsedNow());
    return true;
}

// ---------------------------------------------------------------------
// Update and render

void FenceScene::updateFence(double elapsed) {
    if (!fenceGame) return;
    auto &game = *fenceGame;
    if (!game.entered) enterFence();
    // The list's nested message loop (0x0040d1c0) holds the scene.
    if (game.list || game.saveName) {
        game.lastUpdate = elapsed;
        return;
    }
    // Timer event 0x111, 0x00468380: tag 1 is the marker, tag 0 the next
    // note. The original's modal dialogs hold timers.
    if (dialogMode == DialogMode::None) {
        if (game.markerTimerAt && elapsed >= *game.markerTimerAt) {
            game.markerTimerAt.reset();
            removeFenceSprite(game.marker);
        }
        if (game.playTimerAt && elapsed >= *game.playTimerAt) {
            const double due = *game.playTimerAt;
            game.playTimerAt.reset();
            playNextFenceNote(due);
        }
    }
    // Update event 0x112, 0x004683c0.
    const double seconds =
        std::floor((elapsed - game.lastUpdate) * 1000.0) * 0.001;
    if (game.mode == FenceMode::BirdsArriving ||
        game.mode == FenceMode::BirdsLeaving) {
        updateFenceBirds(static_cast<float>(seconds), elapsed);
    }
    game.lastUpdate += std::floor((elapsed - game.lastUpdate) * 1000.0) * 0.001;
    // Non-looping clips end: the scene's AnimationFinished, which the bird
    // step sees on the next update (capture fence-load-tune: each landing
    // clip's finish precedes the bird's next add by one 16 ms pass).
    for (auto &bird : game.birds) {
        if (!bird.added || !bird.playing || bird.loop) continue;
        const auto &media = fenceClip(bird.name);
        // The bird clips report AnimationFinished at their last frame's
        // timestamp, a pass earlier than originalSmackerCompletionDuration
        // (capture fence-load-tune: land-up 265-282 ms and crotchet-up
        // 203-219 ms after the add, at 15 fps with 5 and 4 frames).
        // UNCERTAIN: why these clips finish a pass sooner than scene clips.
        const double finishes =
            media.videoFrames.empty()
                ? media.videoDuration
                : media.videoFrames.metadata(media.videoFrames.size() - 1U).time;
        if (elapsed - bird.startedAt >= finishes) {
            bird.playing = false;
            referenceTrace.emit("animation_stop", {
                {"asset", bird.name}, {"reason", "completed"}});
        }
    }
    if (game.birdsEffect != LiveAudioMixer::kInvalidEffectHandle &&
        elapsed >= game.birdsSoundEndsAt) {
        game.birdsEffect = LiveAudioMixer::kInvalidEffectHandle;
        playFenceBirdsSound();
    }
}

void FenceScene::renderFence(double elapsed) {
    if (!fenceGame) return;
    auto &game = *fenceGame;
    // Draw by layer, as SCENE keeps its list sorted (0x004129b0).
    struct Item {
        int layer;
        int order;
        std::function<void()> draw;
    };
    std::vector<Item> items;
    int order = 0;
    const auto addSprite = [&](const FenceSprite &sprite) {
        if (!sprite.added) return;
        items.push_back({sprite.layer, order++, [this, &sprite] {
            const auto &texture = fenceBitmap(sprite.name);
            if (!texture) return;
            SDL_Rect destination{sprite.x, sprite.y, 0, 0};
            SDL_QueryTexture(texture.get(), nullptr, nullptr, &destination.w,
                             &destination.h);
            SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
        }});
    };
    addSprite(game.modeButton);
    addSprite(game.lettersBoard);
    addSprite(game.mike);
    addSprite(game.player);
    for (int slot = 0; slot < kSlots; ++slot) {
        addSprite(game.notes[static_cast<std::size_t>(slot)]);
        addSprite(game.letterLabels[static_cast<std::size_t>(slot)]);
        for (const auto &line : game.ledgers[static_cast<std::size_t>(slot)])
            addSprite(line);
    }
    addSprite(game.cursorSprite);
    addSprite(game.marker);
    addSprite(game.trash);
    for (const auto &bird : game.birds) {
        if (!bird.added) continue;
        items.push_back({bird.layer, order++, [this, &bird, elapsed] {
            if (!bird.animated) {
                const auto &texture = fenceBitmap(bird.name);
                if (!texture) return;
                SDL_Rect destination{bird.x, bird.y, 0, 0};
                SDL_QueryTexture(texture.get(), nullptr, nullptr,
                                 &destination.w, &destination.h);
                SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
                return;
            }
            const auto &media = fenceClip(bird.name);
            const auto texture = fenceGame->clipTextures.find(bird.name);
            if (media.videoFrames.empty() ||
                texture == fenceGame->clipTextures.end() || !texture->second)
                return;
            const double time = bird.playing ? elapsed - bird.startedAt
                                             : media.videoDuration;
            const auto &frame = media.videoFrames[std::min(
                frameIndexAt(media, time, bird.loop),
                media.videoFrames.size() - 1U)];
            uploadVideoFrame(texture->second.get(), frame);
            SDL_Rect destination{bird.x, bird.y, frame.width, frame.height};
            SDL_RenderCopy(renderer, texture->second.get(), nullptr,
                           &destination);
        }});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item &a, const Item &b) {
                         return a.layer < b.layer;
                     });
    for (const auto &item : items) item.draw();

    // Panel buttons (0x00406ed0): the _0 image, _1 while pressed, with the
    // caption in Amerigo BT_10_. UNCERTAIN: the shared control drawing is
    // approximated here.
    for (const auto &button : kPanelButtons) {
        const bool pressed = game.pressedControl == button.id;
        const auto &texture = fenceBitmap(std::string(button.image) +
                                          (pressed ? "_1" : "_0"));
        if (!texture) continue;
        SDL_Rect destination{button.x, button.y, 0, 0};
        SDL_QueryTexture(texture.get(), nullptr, nullptr, &destination.w,
                         &destination.h);
        SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
        if (button.caption)
            drawCenteredText(renderer, dialogFont,
                             language.get("fence", button.caption), destination);
    }
    drawCenteredText(renderer, dialogFont, language.get("fence", "FREE", "Fri"),
                     kFreeLabel);
    drawCenteredText(renderer, dialogFont,
                     language.get("fence", "PLAY", "Spill"), kPlayLabel);
    drawCenteredText(renderer, dialogFont,
                     language.get("fence", "RECORD", "Komp."), kRecordLabel);
    drawCenteredText(renderer, dialogFont, std::to_string(fenceTune.bpm),
                     kBpmText);

    if (const auto &sock = fenceBitmap("sock")) {
        const SDL_Rect destination = fenceSockBounds();
        SDL_RenderCopy(renderer, sock.get(), nullptr, &destination);
    }
    if (game.list) renderFenceList();
    if (game.saveName) {
        if (!game.nameBackdrop) {
            if (const auto *entry =
                    common.findOptional("dialogue\\name\\backdrop.bmp"))
                game.nameBackdrop = opaqueTexture(renderer, common.data(*entry));
        }
        SDL_Rect origin{0xf9, 0xf7, 309, 113};
        if (game.nameBackdrop)
            SDL_RenderCopy(renderer, game.nameBackdrop.get(), nullptr, &origin);
        // 0x00420040: title (0xb,5) 0x119 x 0x12, text (0xb,0x1e).
        drawCenteredText(renderer, dialogFont,
                         language.get("fence", "SAVETITLE", "Lagre Sang"),
                         SDL_Rect{0xf9 + 0xb, 0xf7 + 5, 0x119, 0x12});
        drawText(renderer, calendarFont, *game.saveName + "_", 0xf9 + 0xb + 4,
                 0xf7 + 0x1e + 6);
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "OK", "OK"),
                         SDL_Rect{0xf9 + 0xf, 0xf7 + 0x45, 83, 27});
        drawCenteredText(renderer, dialogFont,
                         language.get("interfaceh", "CANCEL", "Avbryt"),
                         SDL_Rect{0xf9 + 0xcc, 0xf7 + 0x45, 83, 27});
    }
}

bool FenceScene::executeFenceDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "fence:";
    if (!fenceGame || activeScene != "fence") return false;
    auto &game = *fenceGame;
    const auto control = command.substr(prefix.size());
    const double now = sceneElapsedNow();
    if (control.rfind("seed:", 0) == 0) {
        // Mirrors the capture adapter, which pins rand()'s holdrand.
        // The adapter re-arms it at 0x00467060 and 0x00468520, so every
        // departure and arrival draws from the same seed.
        game.randomSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
        game.randomSeeded = true;
    } else if (control.rfind("load:", 0) == 0) {
        if (game.mode != FenceMode::Free && game.mode != FenceMode::Record)
            return false;
        loadFenceTune(std::stoi(std::string(control.substr(5))), now);
    } else if (control.rfind("instrument:", 0) == 0) {
        selectFenceInstrument(std::stoi(std::string(control.substr(11))));
    } else if (control.rfind("save:", 0) == 0) {
        saveFenceTune(std::string(control.substr(5)));
    } else if (control == "wait-idle") {
        // Rejected (so retried next frame) until no birds and no playback.
        return (game.mode == FenceMode::Free ||
                game.mode == FenceMode::Record) && game.entered;
    } else if (control.rfind("wait-ms:", 0) == 0) {
        if (!game.waitUntil)
            game.waitUntil =
                now + std::stod(std::string(control.substr(8))) / 1000.0;
        if (now < *game.waitUntil) return false;
        game.waitUntil.reset();
    } else if (control.rfind("hotspot:", 0) == 0) {
        if (game.mode == FenceMode::BirdsArriving ||
            game.mode == FenceMode::BirdsLeaving)
            return false;
        const auto hotspot = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(8))));
        referenceTrace.emit("click_dispatch", {
            {"scene", "fence"}, {"hotspot", hotspot}});
        dispatchFenceHotspot(hotspot, now);
    } else if (control.rfind("key:", 0) == 0) {
        pressFencePlank(std::stoi(std::string(control.substr(4))), now);
    } else if (control == "status") {
        std::cout << "Fence status: state " << static_cast<int>(game.mode)
                  << ", notes " << fenceTune.notes.size() << ", cursor "
                  << game.cursor << ", page " << game.page << ", bpm "
                  << fenceTune.bpm << ", birds " << game.birds.size()
                  << ", instrument " << game.instrument << ", tune '"
                  << fenceTune.name << "'\n";
    } else {
        throw std::runtime_error("Unknown developer Fence control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
