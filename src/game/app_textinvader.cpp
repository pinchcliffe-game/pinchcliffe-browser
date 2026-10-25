#include "game/app_textinvader.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "media/smacker.h"
#include "util/text.h"

// Text Invader is a port of FGP.exe 0x0044c640-0x0044d980. Every rule below
// names the address it was read from; REVERSE_ENGINEERING.md ("Text Invader")
// has the complete account. The scene is driven by original 0xa8-byte
// animation descriptors and text controls, so the port keeps one object per
// descriptor and mirrors each SCENE_AddAnim (0x00412840), SCENE_RemoveAnim
// (0x00412aa0) and SCENE_StopAnim (0x00412a60) call in the reference trace,
// plus the `textinvader` minigame_state logpoints that
// reference/windows/x32dbg/yard_timeline_headless.txt sets in the original.

namespace fgp {
namespace {

// Message 0x111 timer ids (0x0044cea0): a slot index spawns that slot, 1000
// plus a slot removes a completed word, -2 ends the orange slow-down.
constexpr int kRemoveTimerBase = 1000;
constexpr int kSlowTimer = -2;
// 0x004de620: base fall speed (pixels per second) per difficulty.
const auto kInitialSpeeds = originalArray<float, 3>(0x004de620U);
// 0x004de62c: fall-speed factor per level step.
const auto kLevelSpeedScales = originalArray<float, 3>(0x004de62cU);
// 0x004de638: orange slow-down length in milliseconds.
const auto kSlowMilliseconds = originalArray<int, 3>(0x004de638U);
// 0x004de644: completed words per bonus word.
const auto kBonusThresholds = originalArray<int, 3>(0x004de644U);
// 0x004de650: removed words per level step.
const auto kLevelThresholds = originalArray<int, 3>(0x004de650U);
// 0x0049f090: score text rectangle, left/top/right/bottom 20,546,105,566,
// drawn centred (flags 0x101 at 0x0044ce77).
constexpr SDL_Rect kScoreRect{20, 546, 85, 20};
// 0x004de898 +0x54/+0x58: julelys2, also the julelys1 progress bar origin
// passed to PROGRESS_Create at 0x0044c843.
constexpr SDL_Point kLightsOrigin{165, 493};
constexpr int kLightsWidth = 350;
constexpr int kLightsHeight = 24;
// 0x004de940/0x004de9e8 +0x58: both skilt descriptors sit at (0,518).
constexpr SDL_Rect kSignBounds{0, 518, 140, 82};
constexpr std::uint32_t kSignHotspot = 4U;
// A word misses once its top passes y=490 (0x0044d572).
constexpr int kMissY = 0x1ea;
// Clone slots for SCENE_PlayAnimClone (0x00412c20) start at 0x0054fea8.
constexpr std::uint32_t kCloneBase = 0x0054fea8U;
constexpr std::uint32_t kCloneStride = 0xa8U;
constexpr std::uint32_t kSlotBase = 0x00682c20U;
constexpr std::uint32_t kSlotStride = 0x258U;
// Font indices in TextinvaderGame::fonts, in 0x0044c740's load order.
constexpr std::size_t kBlueFont = 0;   // 0x006864c8
constexpr std::size_t kGreenFont = 1;  // 0x00688568
constexpr std::size_t kYellowFont = 3; // 0x00687538
constexpr std::size_t kOrangeFont = 4; // 0x00685488
constexpr std::size_t kRedFont = 5;    // 0x00684458

std::string descriptorText(std::uint32_t address) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

// The word list holds only ASCII and the Latin-1 letters æ/ø/å, so the
// cp1252 bytes above 0x9f map one to one onto U+00A0-U+00FF.
std::string latin1ToUtf8(std::string_view text) {
    std::string result;
    for (const unsigned char character : text) {
        if (character < 0x80U) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back(static_cast<char>(0xc0U | (character >> 6)));
            result.push_back(static_cast<char>(0x80U | (character & 0x3fU)));
        }
    }
    return result;
}

// FGP.exe calls setlocale(LC_CTYPE, "Russian_Russia.1251") at startup
// (0x0040e7e0-0x0040e7e7), so the CRT's isalpha in 0x0044cfe0 (ctype mask
// 0x103) and tolower in 0x0048daff use the cp1251 tables: every byte
// 0xc0-0xff is a letter and 0xc0-0xdf lower by 0x20, which for Æ/Ø/Å/Ä/Ö
// gives the same bytes as cp1252. UNCERTAIN: cp1251 also classes some bytes
// between 0x80 and 0xbf (0xa8, 0xb8, ...) as letters; they are not accepted
// here, and no Norwegian layout key produces them as characters.
bool textinvaderLetter(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character >= 0xc0U ||
           character == '-';
}

unsigned char textinvaderLower(unsigned char character) {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 0xc0U && character <= 0xdfU)) {
        return static_cast<unsigned char>(character + 0x20U);
    }
    return character;
}

std::size_t fontForBonus(int bonus) {
    // 0x0044cd6e-0x0044cd95.
    switch (bonus) {
    case 1: return kYellowFont;
    case 2: return kOrangeFont;
    case 3: return kRedFont;
    default: return kBlueFont;
    }
}

TexturePtr bitmapTexture(SDL_Renderer *renderer,
                         std::span<const std::uint8_t> bytes, bool keyed) {
    SurfacePtr surface = loadBmp(bytes);
    if (keyed) {
        const Uint32 key = pixelValue(surface.get(), 0, 0);
        if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0)
            throw std::runtime_error(SDL_GetError());
    }
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

TexturePtr greenKeyedTexture(SDL_Renderer *renderer,
                             std::span<const std::uint8_t> bytes) {
    SurfacePtr surface = loadBmp(bytes);
    const Uint32 key = SDL_MapRGB(surface->format, 0, 255, 0);
    if (SDL_SetColorKey(surface.get(), SDL_TRUE, key) != 0)
        throw std::runtime_error(SDL_GetError());
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

} // namespace

TextinvaderScene::TextinvaderGame
TextinvaderScene::loadTextinvaderGame(Package &package) {
    TextinvaderGame game;
    const auto asset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Text Invader asset not found: " +
                                 std::string(name));
    };
    // `fjell.bmp` is a full-screen chroma-key overlay: its literal 0,255,0
    // sky is transparent, and at layer 6 it hides words behind the mountain.
    game.mountainTexture = greenKeyedTexture(renderer, asset("bitmap\\fjell.bmp"));
    game.lightsOffTexture =
        bitmapTexture(renderer, asset("bitmap\\julelys2.bmp"), false);
    game.lightsOnTexture =
        bitmapTexture(renderer, asset("bitmap\\julelys1.bmp"), false);
    game.signTexture = bitmapTexture(renderer, asset("bitmap\\skilt.bmp"), true);

    if (!data2Common) {
        throw std::runtime_error("Text Invader needs the Data2 common fonts");
    }
    // 0x0044c740 loads six fonts; the third, Comic Sans MS_10_, is the shared
    // messageFont and is used only by the score text.
    constexpr std::array<std::pair<std::size_t, std::string_view>, 5> fontAssets{{
        {kBlueFont, "fonts\\Comic Sans MS_16_lightish_blue.bmp"},
        {kGreenFont, "fonts\\Comic Sans MS_16_lightish_green.bmp"},
        {kYellowFont, "fonts\\Comic Sans MS_16_yellow.bmp"},
        {kOrangeFont, "fonts\\comic sans ms_16_orange.bmp"},
        {kRedFont, "fonts\\comic sans ms_16_red.bmp"},
    }};
    for (const auto &[index, name] : fontAssets) {
        game.fonts[index] =
            loadBitmapFont(renderer, data2Common->data(data2Common->find(name)));
    }

    for (const char *name : {"skilt", "li", "start", "right", "wrong", "drop"}) {
        auto &clip = game.clips[name];
        clip.media = decodeActiveSceneClip(name);
        clip.completion = originalSmackerCompletionDuration(clip.media);
        if (!clip.media.videoFrames.empty()) {
            const auto &frame = clip.media.videoFrames.front();
            clip.texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                frame.width, frame.height));
            if (!clip.texture) throw std::runtime_error(SDL_GetError());
            SDL_SetTextureBlendMode(clip.texture.get(), SDL_BLENDMODE_BLEND);
        }
    }

    // The 0xa8-byte descriptors from 0x004de6a0: +0x40 Smacker, +0x50
    // hotspot, +0x54/+0x58 default position.
    game.mountain = {"fjell", 0x004de6a0U, false, true, false, 0, 0, 0U};
    game.lightsOff = {"julelys2", 0x004de898U, false, true, false,
                      kLightsOrigin.x, kLightsOrigin.y, 0U};
    game.startSign = {"skilt", 0x004de940U, true, true, false,
                      kSignBounds.x, kSignBounds.y, kSignHotspot};
    game.scoreSign = {"skilt", 0x004de9e8U, false, true, false,
                      kSignBounds.x, kSignBounds.y, kSignHotspot};
    game.scoreText.address = 0x006843a8U;
    game.scoreText.layer = 8;
    for (std::size_t index = 0; index < game.slots.size(); ++index) {
        auto &slot = game.slots[index];
        const std::uint32_t base =
            kSlotBase + static_cast<std::uint32_t>(index) * kSlotStride;
        slot.word.address = base + 0x34U;
        slot.word.layer = 3;
        slot.typed.address = base + 0xdcU;
        slot.typed.layer = 4;
        slot.typed.font = kGreenFont;
        slot.lightning = {"li", base + 0x184U, true, true, false, 0, 0, 0U};
    }

    // 0x0044d7e0 indexes `wordlist.txt` by length 1..31 in file order. The
    // first pass cuts each line at "\r\n" (0x0044d877), the second at '\r'
    // (0x0044d90c); words are used as stored, without case folding.
    const auto listData = asset("wordlist\\wordlist.txt");
    std::string list(reinterpret_cast<const char *>(listData.data()),
                     listData.size());
    std::istringstream lines(list);
    std::string word;
    while (std::getline(lines, word)) {
        if (const auto cr = word.find('\r'); cr != std::string::npos)
            word.erase(cr);
        if (word.empty() || word.size() >= game.wordsByLength.size()) continue;
        game.wordsByLength[word.size()].push_back(std::move(word));
    }
    game.randomState = static_cast<std::uint32_t>(randomEngine());
    std::cout << "Loaded executable-derived Text Invader: " << listData.size()
              << " word-list bytes, descriptors and word slots from "
                 "0x0044c640-0x0044d980\n";
    return game;
}

int TextinvaderScene::textinvaderNow() {
    // 0x0040c450: the game clock in milliseconds.
    return static_cast<int>(std::floor(sceneElapsedNow() * 1000.0));
}

int TextinvaderScene::textinvaderRandom(int bound) {
    // 0x0040c530 returns 0 for bound 0 without calling rand(); otherwise
    // rand() % bound with the MSVC generator (0x0048d323).
    if (bound == 0) return 0;
    auto &game = *textinvaderGame;
    game.randomState = game.randomState * 0x343fdU + 0x269ec3U;
    return static_cast<int>((game.randomState >> 16) & 0x7fffU) % bound;
}

void TextinvaderScene::traceTextinvaderHud() {
    const auto &game = *textinvaderGame;
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "hud"}, {"score", game.score},
        {"energy", game.energy}, {"completed", game.completedWords},
        {"removed", game.removedWords}, {"level", game.levelStep},
        {"max_words", game.maximumWords},
        {"max_length", game.maximumLength}});
}

void TextinvaderScene::addTextinvaderAnim(TextinvaderAnim &anim, int x, int y,
                                          int layer) {
    auto &game = *textinvaderGame;
    anim.active = true;
    anim.playing = false;
    anim.frozenAt = 0.0;
    anim.x = x == -1 ? anim.defaultX : x;
    anim.y = y == -1 ? anim.defaultY : y;
    anim.layer = layer;
    anim.order = ++game.nextOrder;
    referenceTrace.emit("animation_add", {
        {"asset", anim.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", anim.hotspot}, {"conflict_mask", 0},
        {"descriptor", descriptorText(anim.address)}});
}

void TextinvaderScene::stopTextinvaderAnim(TextinvaderAnim &anim) {
    // SCENE_StopAnim 0x00412a60 is logged on entry for every descriptor.
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "stopped"},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.smacker && anim.playing) {
        anim.frozenAt = sceneElapsedNow() - anim.playStarted;
        anim.playing = false;
    }
    if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void TextinvaderScene::removeTextinvaderAnim(TextinvaderAnim &anim) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60,
    // so its logpoint precedes the nested StopAnim record.
    referenceTrace.emit("animation_remove", {
        {"asset", anim.name}, {"reason", "removed"},
        {"descriptor", descriptorText(anim.address)}});
    stopTextinvaderAnim(anim);
    anim.active = false;
    anim.playing = false;
}

void TextinvaderScene::playTextinvaderAnim(TextinvaderAnim &anim) {
    // SCENE_PlayAnim 0x004129d0 starts a Smacker descriptor's player.
    auto &game = *textinvaderGame;
    const auto found = game.clips.find(anim.name);
    if (!anim.smacker || found == game.clips.end()) return;
    anim.playStarted = sceneElapsedNow() - anim.frozenAt;
    anim.playing = true;
    if (!found->second.media.audioSamples.empty()) {
        if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle)
            audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = audioMixer.addEffect(
            found->second.media, audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void TextinvaderScene::playTextinvaderSound(std::string_view name) {
    // 0x0044d1e0 copies the name into the invisible template 0x004de748 and
    // plays a clone of it (0x00412c20). Clones take the first free 0xa8-byte
    // slot from 0x0054fea8, are added at (0,0) layer 0 and are removed when
    // they finish (0x00413259).
    auto &game = *textinvaderGame;
    std::uint32_t address = kCloneBase;
    while (std::any_of(game.clones.begin(), game.clones.end(),
                       [&](const TextinvaderAnim &clone) {
                           return clone.active && clone.address == address;
                       })) {
        address += kCloneStride;
    }
    TextinvaderAnim clone{std::string(name), address, true, false, true, 0, 0, 0U};
    game.clones.push_back(std::move(clone));
    addTextinvaderAnim(game.clones.back(), 0, 0, 0);
    playTextinvaderAnim(game.clones.back());
}

void TextinvaderScene::finishTextinvaderClip(TextinvaderAnim &anim) {
    // 0x00413245-0x00413269: a finished player is stopped; a clone is then
    // removed, any other descriptor reports AnimationFinished, which reaches
    // the scene as message 0x110.
    stopTextinvaderAnim(anim);
    if (anim.clone) {
        removeTextinvaderAnim(anim);
        return;
    }
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "finished"},
        {"descriptor", descriptorText(anim.address)}});
    // 0x0044c920 removes whatever finished; the Start clip starts the game.
    removeTextinvaderAnim(anim);
    auto &game = *textinvaderGame;
    if (&anim == &game.startSign) {
        game.starting = false;
        startTextinvaderGame();
    }
}

void TextinvaderScene::createTextinvaderText(TextinvaderText &control, int x,
                                             int y) {
    // 0x00407930 adds the control's descriptor at the rectangle's corner.
    control.active = true;
    control.x = x;
    control.y = y;
    control.text.clear();
    control.order = ++textinvaderGame->nextOrder;
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", x}, {"y", y}, {"layer", control.layer},
        {"hotspot", 0}, {"conflict_mask", 0},
        {"descriptor", descriptorText(control.address)}});
}

void TextinvaderScene::destroyTextinvaderText(TextinvaderText &control) {
    // 0x00407840 removes the control's descriptor.
    if (!control.active) return;
    control.active = false;
    control.text.clear();
    referenceTrace.emit("animation_remove", {
        {"asset", ""}, {"reason", "removed"},
        {"descriptor", descriptorText(control.address)}});
    referenceTrace.emit("animation_stop", {
        {"asset", ""}, {"reason", "stopped"},
        {"descriptor", descriptorText(control.address)}});
}

void TextinvaderScene::addTextinvaderTimer(int due, int id) {
    // EVENT_AddTimer 0x0040c480(due, period 0, id). The service pass at
    // 0x0040bf50 walks the slots in order, posts message 0x111 for each one
    // whose due time has come, and frees a period-0 timer.
    textinvaderGame->timers.push_back({due, id});
}

void TextinvaderScene::initializeTextinvaderScene() {
    // Scene initializer 0x0044c740, in its call order. It runs on the first
    // update so the trace follows scene_change.
    auto &game = *textinvaderGame;
    game.initialized = true;
    // 0x0044c7b0: the score text control at 0x0049f090, layer 8.
    createTextinvaderText(game.scoreText, kScoreRect.x, kScoreRect.y);
    addTextinvaderAnim(game.mountain, -1, -1, 6);
    addTextinvaderAnim(game.lightsOff, -1, -1, 7);
    // 0x0044c843: PROGRESS_Create(165, 493, layer 100, "julelys1"). Its first
    // PROGRESS_Set adds the empty bar's own unnamed descriptor (0x00408c4b).
    game.lights = 0.0F;
    game.lightsOrder = ++game.nextOrder;
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", -1}, {"y", -1}, {"layer", 100},
        {"hotspot", 0}, {"conflict_mask", 0}});
    game.playing = false;
    game.starting = false;
    game.spawnSpread = 6000; // 0x0044c877
    showTextinvaderStartSign();
}

void TextinvaderScene::showTextinvaderStartSign() {
    // 0x0044c8b0: remove either skilt, add the Smacker sign at layer 7 and
    // empty the score text.
    auto &game = *textinvaderGame;
    if (game.startSign.active) removeTextinvaderAnim(game.startSign);
    if (game.scoreSign.active) removeTextinvaderAnim(game.scoreSign);
    addTextinvaderAnim(game.startSign, -1, -1, 7);
    game.scoreText.text.clear();
}

void TextinvaderScene::pressTextinvaderHotspot(std::uint32_t hotspot) {
    // 0x0044d720: only the idle sign starts the clip and the `start` sound.
    if (!textinvaderGame) return;
    auto &game = *textinvaderGame;
    if (game.playing || game.starting || hotspot != kSignHotspot) return;
    game.startPressedAt = textinvaderNow();
    game.starting = true;
    playTextinvaderAnim(game.startSign);
    playTextinvaderSound("start");
}

void TextinvaderScene::startTextinvaderGame() {
    // 0x0044c950. Normal play always uses difficulty 0: 0x0044c952 asks
    // 0x00419490 whether a tournament runs and only then reads its level.
    auto &game = *textinvaderGame;
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "start"},
        {"difficulty", game.difficulty}});
    if (game.fixedSeed) {
        // Capture fixture: the x32dbg script reseeds rand() at this entry.
        game.randomState = *game.fixedSeed;
    }
    game.nextBonus = textinvaderRandom(3);
    game.selected = -1;
    game.fallSpeed = kInitialSpeeds[static_cast<std::size_t>(game.difficulty)];
    game.completedWords = 0;
    game.lastBonusQuotient = 0;
    game.speedFactor = 1.0F;
    game.levelStep = 0;
    game.removedWords = 0;
    game.score = 0;
    game.energy = 100;
    game.maximumWords = 1;
    game.maximumLength = 1;
    // 0x0044cdf0: swap the Start clip for the bitmap score sign.
    if (game.startSign.active) removeTextinvaderAnim(game.startSign);
    if (game.scoreSign.active) removeTextinvaderAnim(game.scoreSign);
    addTextinvaderAnim(game.scoreSign, -1, -1, 7);
    game.playing = true;
    resetTextinvaderTyping();
    for (auto &slot : game.slots) slot.active = false;
    for (int slot = 0; slot < game.maximumWords; ++slot) {
        spawnTextinvaderWord(slot);
    }
    updateTextinvaderHud();
    game.lastTick = textinvaderNow();
}

void TextinvaderScene::spawnTextinvaderWord(int index) {
    // 0x0044ca50.
    auto &game = *textinvaderGame;
    auto &slot = game.slots[static_cast<std::size_t>(index)];
    if (slot.active || !game.playing) return;
    slot.completed = false;
    const int quotient = game.completedWords /
        kBonusThresholds[static_cast<std::size_t>(game.difficulty)];
    if (quotient > game.lastBonusQuotient) {
        // 0x0044caaf-0x0044cade: the next colour is one of the other two.
        slot.bonus = game.nextBonus + 1;
        const int step = textinvaderRandom(2);
        game.lastBonusQuotient = quotient;
        game.nextBonus = (step + game.nextBonus + 1) % 3;
    } else {
        slot.bonus = 0;
    }
    // 0x0044caf2-0x0044cb54: draw words until no other live slot shares the
    // initial letter. 0x0044d770 draws a length until its bucket is not
    // empty, then an entry of that length.
    while (true) {
        int length = 0;
        int count = 0;
        do {
            length = textinvaderRandom(game.maximumLength) + 1;
            count = static_cast<int>(
                game.wordsByLength[static_cast<std::size_t>(length)].size());
        } while (count == 0);
        slot.text = game.wordsByLength[static_cast<std::size_t>(length)]
                                      [static_cast<std::size_t>(
                                          textinvaderRandom(count))];
        bool duplicate = false;
        for (int other = 0; other < game.maximumWords; ++other) {
            const auto &active = game.slots[static_cast<std::size_t>(other)];
            if (other != index && active.active &&
                active.text.front() == slot.text.front()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) break;
    }
    // 0x0044cb73: the extent always comes from the blue font; 0x00401200
    // sums glyph widths (these fonts have no extra spacing).
    slot.width = textWidth(game.fonts[kBlueFont], slot.text);
    slot.height = game.fonts[kBlueFont].height;
    // 0x0044cb9d-0x0044cc6d: up to 101 positions in x 80..719-width,
    // y 0..149, until the rectangle misses every other live word. The overlap
    // test 0x00412380 is strict on all four edges.
    for (int attempt = 0;; ++attempt) {
        slot.x = textinvaderRandom(640 - slot.width) + 80;
        slot.y = textinvaderRandom(150);
        slot.rect = {slot.x, slot.y, slot.x + slot.width, slot.y + slot.height};
        bool free = true;
        for (int other = 0; other < game.maximumWords; ++other) {
            const auto &active = game.slots[static_cast<std::size_t>(other)];
            if (!active.active || other == index) continue;
            const auto &box = active.rect;
            if (box.right > slot.rect.left && box.bottom > slot.rect.top &&
                slot.rect.right > box.left && slot.rect.bottom > box.top) {
                free = false;
            }
        }
        if (free || attempt >= 100) break;
    }
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "spawn"}, {"slot", index},
        {"x", slot.x}, {"y", slot.y}, {"bonus", slot.bonus},
        // The trace writer escapes cp1252 strings itself.
        {"word", slot.text}});
    createTextinvaderText(slot.word, slot.x, slot.y);
    createTextinvaderText(slot.typed, slot.x, slot.y);
    slot.fallY = static_cast<float>(slot.y);
    slot.active = true;
    slot.spawnSerial = ++game.nextSpawnSerial;
    slot.developerSkip = false;
    // 0x0044ccb6-0x0044cce2: the lightning is centred on the word.
    slot.offsetX = slot.width / 2 - 64;
    slot.offsetY = slot.height / 2 - 64;
    addTextinvaderAnim(slot.lightning, slot.x + slot.offsetX,
                       slot.y + slot.offsetY, 5);
    playTextinvaderAnim(slot.lightning);
    showTextinvaderWord(index);
}

void TextinvaderScene::showTextinvaderWord(int index) {
    // 0x0044cd30: a live, uncompleted word shows its whole text in its bonus
    // colour and no typed prefix.
    auto &game = *textinvaderGame;
    auto &slot = game.slots[static_cast<std::size_t>(index)];
    if (!slot.active || slot.completed) return;
    slot.word.text = slot.text;
    slot.word.font = fontForBonus(slot.bonus);
    slot.typed.text.clear();
}

void TextinvaderScene::resetTextinvaderTyping() {
    // 0x0044cdb0.
    auto &game = *textinvaderGame;
    for (int index = 0; index < game.maximumWords; ++index) {
        showTextinvaderWord(index);
    }
    game.typed.clear();
    game.selected = -1;
}

void TextinvaderScene::updateTextinvaderHud() {
    // 0x0044ce40: clamp the energy at 0, show it on julelys1 and the score.
    auto &game = *textinvaderGame;
    traceTextinvaderHud();
    if (game.energy < 0) game.energy = 0;
    game.lights = std::clamp(static_cast<float>(game.energy) * 0.01F, 0.0F, 1.0F);
    game.scoreText.text = std::to_string(game.score);
}

void TextinvaderScene::removeTextinvaderWord(int index) {
    // 0x0044cf00.
    auto &game = *textinvaderGame;
    auto &slot = game.slots[static_cast<std::size_t>(index)];
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "remove"}, {"slot", index},
        {"active", slot.active ? 1 : 0}});
    if (!slot.active) return;
    ++game.removedWords;
    slot.active = false;
    destroyTextinvaderText(slot.word);
    destroyTextinvaderText(slot.typed);
    if (slot.lightning.active) removeTextinvaderAnim(slot.lightning);
}

void TextinvaderScene::textinvaderTimer(int id) {
    // 0x0044cea0.
    auto &game = *textinvaderGame;
    if (id == kSlowTimer) {
        game.speedFactor = 1.0F;
    } else if (id >= kRemoveTimerBase) {
        const int index = id - kRemoveTimerBase;
        removeTextinvaderWord(index);
        addTextinvaderTimer(textinvaderNow() + textinvaderRandom(500), index);
    } else {
        spawnTextinvaderWord(id);
    }
}

void TextinvaderScene::applyTextinvaderBonus(int bonus) {
    // 0x0044d220.
    auto &game = *textinvaderGame;
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "bonus"}, {"bonus", bonus}});
    if (bonus == 1) {
        // 0x0044d240, yellow: clear every live word. 0x0044d259-0x0044d268
        // add three times slot field +0x0c, which nothing ever writes (its
        // only reference is this read), so the clear scores nothing. Every
        // slot then respawns within 0..5999 ms (0x006843a0).
        for (int index = 0; index < game.maximumWords; ++index) {
            auto &slot = game.slots[static_cast<std::size_t>(index)];
            if (!slot.active) continue;
            game.score += slot.yellowWeight * 3;
            removeTextinvaderWord(index);
        }
        for (int index = 0; index < game.maximumWords; ++index) {
            addTextinvaderTimer(
                textinvaderNow() + textinvaderRandom(game.spawnSpread), index);
        }
    } else if (bonus == 2) {
        // 0x0044d2c0, orange: half speed until timer -2.
        game.speedFactor = 0.5F;
        addTextinvaderTimer(
            textinvaderNow() +
                kSlowMilliseconds[static_cast<std::size_t>(game.difficulty)],
            kSlowTimer);
    } else if (bonus == 3) {
        // 0x0044d2f0, red: ten energy, capped at 100.
        game.energy = std::min(100, game.energy + 10);
        updateTextinvaderHud();
    }
}

void TextinvaderScene::tickTextinvader() {
    // Message 0x112, 0x0044d4f0.
    auto &game = *textinvaderGame;
    const int now = textinvaderNow();
    const double dt =
        static_cast<double>(now - game.lastTick) * 0.0010000000474974513;
    game.lastTick = now;
    if (!game.playing) {
        if (game.starting) {
            // 0x0044d64e-0x0044d673: the lights fill while the sign plays.
            game.lights = std::clamp(
                static_cast<float>(now - game.startPressedAt) *
                    0.0033333334140479565F,
                0.0F, 1.0F);
        }
        return;
    }
    for (int index = 0; index < game.maximumWords; ++index) {
        auto &slot = game.slots[static_cast<std::size_t>(index)];
        if (!slot.active) continue;
        const double fall = static_cast<double>(game.speedFactor) *
                            static_cast<double>(game.fallSpeed) * dt;
        const double y = static_cast<double>(slot.fallY) + fall;
        slot.fallY = static_cast<float>(y);
        slot.y = static_cast<int>(y);
        slot.rect.top = slot.y;
        slot.rect.bottom = slot.height + slot.y;
        if (slot.y > kMissY && !slot.completed) {
            referenceTrace.emit("minigame_state", {
                {"game", "textinvader"}, {"state", "miss"}, {"slot", index}});
            game.energy -= 5;
            playTextinvaderSound("drop");
            removeTextinvaderWord(index);
            addTextinvaderTimer(now + textinvaderRandom(500), index);
            if (game.selected == index) resetTextinvaderTyping();
            updateTextinvaderHud();
            checkTextinvaderGameOver();
            continue;
        }
        // 0x0044d5e1-0x0044d61f: move both text controls and the lightning.
        slot.word.x = slot.typed.x = slot.x;
        slot.word.y = slot.typed.y = slot.y;
        slot.lightning.x = slot.x + slot.offsetX;
        slot.lightning.y = slot.y + slot.offsetY;
    }
    advanceTextinvaderLevel();
}

void TextinvaderScene::advanceTextinvaderLevel() {
    // 0x0044d680: one level step whenever removed / threshold grows; an even
    // quotient lengthens the words, an odd one adds a slot.
    auto &game = *textinvaderGame;
    const int quotient = game.removedWords /
        kLevelThresholds[static_cast<std::size_t>(game.difficulty)];
    if (quotient <= game.levelStep) return;
    if (quotient % 2 == 0) {
        // UNCERTAIN: the original raises the cap to 32 (0x0044d6b5), past the
        // last length bucket, 31; that needs 62 level steps and is not
        // reproduced.
        if (game.maximumLength < 31) ++game.maximumLength;
    } else if (game.maximumWords < 10) {
        const int index = game.maximumWords++;
        addTextinvaderTimer(textinvaderNow() + textinvaderRandom(500), index);
    }
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "level"}, {"level", quotient},
        {"max_words", game.maximumWords}, {"max_length", game.maximumLength}});
    game.levelStep = quotient;
    game.fallSpeed *= kLevelSpeedScales[static_cast<std::size_t>(game.difficulty)];
}

void TextinvaderScene::checkTextinvaderGameOver() {
    // 0x0044d450.
    if (textinvaderGame->energy <= 0) textinvaderGameOver();
}

void TextinvaderScene::textinvaderGameOver() {
    // 0x0044d460, up to the blocking GAMEOVER modal at 0x0044d4a4.
    auto &game = *textinvaderGame;
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "game_over"},
        {"score", game.score}, {"energy", game.energy}});
    for (int index = 0; index < static_cast<int>(game.slots.size()); ++index) {
        removeTextinvaderWord(index);
    }
    game.playing = false;
    game.gameOverModal = true;
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt!"),
        language.get("textinvader", "GAMEOVER", "Spillet er over."),
        MessageKind::TextinvaderGameOver);
}

void TextinvaderScene::finishTextinvaderGameOver() {
    // 0x0044d4c0-0x0044d4ca, after the shared score wrapper 0x0041f9a0
    // returns: outside a tournament the Start sign comes back.
    if (!textinvaderGame) return;
    textinvaderGame->gameOverModal = false;
    showTextinvaderStartSign();
}

int TextinvaderScene::judgeTextinvaderTyping() const {
    // 0x0044d320: 1 wrong (longer than the target or not its prefix),
    // 2 the whole word, 0 a correct prefix.
    const auto &game = *textinvaderGame;
    if (game.typed.size() > game.target.size()) return 1;
    if (game.typed == game.target) return 2;
    return game.target.compare(0, game.typed.size(), game.typed) == 0 ? 0 : 1;
}

void TextinvaderScene::selectTextinvaderTarget() {
    // 0x0044d3b0: the first live, uncompleted word with the typed initial.
    auto &game = *textinvaderGame;
    game.target.clear();
    for (int index = 0; index < game.maximumWords; ++index) {
        const auto &slot = game.slots[static_cast<std::size_t>(index)];
        if (slot.active && !slot.completed && slot.text.front() == game.typed.front()) {
            game.selected = index;
            game.target = slot.text;
            return;
        }
    }
}

void TextinvaderScene::keyTextinvader(unsigned char character) {
    // Message 0x10e (EVENT_EnterChar, WM_CHAR), 0x0044d030.
    auto &game = *textinvaderGame;
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "key"},
        {"char", static_cast<int>(character)}});
    if (!game.playing || !textinvaderLetter(character)) return;
    game.typed.push_back(static_cast<char>(textinvaderLower(character)));
    if (game.typed.size() == 1) selectTextinvaderTarget();
    const int result = judgeTextinvaderTyping();
    referenceTrace.emit("minigame_state", {
        {"game", "textinvader"}, {"state", "judge"}, {"result", result},
        {"typed", static_cast<int>(game.typed.size())},
        {"selected", game.selected}});
    if (result == 0) {
        // 0x0044d18f-0x0044d1c0: three points per correct key, and the typed
        // prefix drawn over the word in green.
        game.score += 3;
        game.slots[static_cast<std::size_t>(game.selected)].typed.text = game.typed;
    } else if (result == 1) {
        // 0x0044d151-0x0044d16b.
        game.energy -= 3;
        playTextinvaderSound("wrong");
        resetTextinvaderTyping();
    } else {
        // 0x0044d09c-0x0044d145: the completed word scores four points per
        // letter (lea edx,[ecx+eax*4] at 0x0044d0b9) and goes 500 ms later.
        auto &slot = game.slots[static_cast<std::size_t>(game.selected)];
        ++game.completedWords;
        game.score += static_cast<int>(game.typed.size()) * 4;
        playTextinvaderSound("right");
        slot.typed.text = game.typed;
        slot.completed = true;
        addTextinvaderTimer(textinvaderNow() + 500,
                            kRemoveTimerBase + game.selected);
        applyTextinvaderBonus(slot.bonus);
        resetTextinvaderTyping();
    }
    updateTextinvaderHud();
    checkTextinvaderGameOver();
}

bool TextinvaderScene::textinvaderClaimsKey(SDL_Keycode key) const {
    // Message 0x10c returns 1 when 0x0044cfe0 accepts the virtual key: CRT
    // isalpha under cp1251 (letters A-Z, and the OEM keys whose codes
    // 0xc0-0xdf are cp1251 letters, such as Æ/Ø/Å) or 0x2d. The event stack
    // walk 0x0040ecb0 stops at the first handler returning 1, so no global
    // key handling sees those keys while the scene runs.
    if (!textinvaderGame || activeScene != "textinvader") return false;
    if (key >= SDLK_a && key <= SDLK_z) return true;
    // UNCERTAIN: VK_OEM_MINUS is 0xbd, a cp1251 letter (S-cyrillic); the
    // minus key is claimed as that, and Æ/Ø/Å as their Latin-1 keycodes.
    return key == SDLK_MINUS || (key >= 0xc0 && key <= 0xff);
}

bool TextinvaderScene::typeTextinvader(std::string_view utf8) {
    if (!textinvaderGame || activeScene != "textinvader" ||
        textinvaderGame->gameOverModal) {
        return false;
    }
    const auto input = utf8ToCp1252(utf8);
    if (!input) return true;
    for (const unsigned char character : *input) keyTextinvader(character);
    return true;
}

void TextinvaderScene::updateTextinvader(double elapsed) {
    if (!textinvaderGame) return;
    auto &game = *textinvaderGame;
    if (!game.initialized) initializeTextinvaderScene();
    // Every dialog close stops SDL text input; the scene reads WM_CHAR.
    if (dialogMode == DialogMode::None && !SDL_IsTextInputActive()) {
        SDL_StartTextInput();
    }
    // The GAMEOVER modal (0x0044d4a4) and the score wrapper (0x0044d4b8)
    // block the original's main loop: no timers, ticks or clip completions
    // until they return.
    if (game.gameOverModal) return;
    // 0x0040bf50: post 0x111 for every due timer, in slot order.
    const int now = textinvaderNow();
    for (std::size_t index = 0; index < game.timers.size();) {
        if (game.timers[index].due <= now) {
            const int id = game.timers[index].id;
            game.timers.erase(game.timers.begin() +
                              static_cast<std::ptrdiff_t>(index));
            textinvaderTimer(id);
            if (game.gameOverModal) return;
        } else {
            ++index;
        }
    }
    tickTextinvader();
    if (game.gameOverModal) return;
    // The descriptor update 0x00413200 finishes one-shot players.
    std::vector<TextinvaderAnim *> players{&game.startSign};
    for (auto &slot : game.slots) players.push_back(&slot.lightning);
    for (auto &clone : game.clones) players.push_back(&clone);
    for (TextinvaderAnim *anim : players) {
        if (!anim->active || !anim->playing) continue;
        const auto found = game.clips.find(anim->name);
        if (found == game.clips.end()) continue;
        if (elapsed - anim->playStarted >= found->second.completion) {
            finishTextinvaderClip(*anim);
            if (game.gameOverModal) return;
        }
    }
    std::erase_if(game.clones,
                  [](const TextinvaderAnim &clone) { return !clone.active; });
}

void TextinvaderScene::renderTextinvader(double elapsed) {
    if (!textinvaderGame) return;
    auto &game = *textinvaderGame;
    struct Item {
        int layer;
        std::uint64_t order;
        std::function<void()> draw;
    };
    std::vector<Item> items;
    const auto drawClip = [&](const TextinvaderAnim &anim) {
        const auto found = game.clips.find(anim.name);
        if (found == game.clips.end() || !found->second.texture ||
            found->second.media.videoFrames.empty()) {
            return;
        }
        const double time = anim.playing ? elapsed - anim.playStarted
                                         : anim.frozenAt;
        const auto frameIndex = frameIndexAt(found->second.media, time, false);
        const auto &frame = found->second.media.videoFrames[frameIndex];
        uploadVideoFrame(found->second.texture.get(), frame);
        const SDL_Rect target{anim.x, anim.y, frame.width, frame.height};
        SDL_RenderCopy(renderer, found->second.texture.get(), nullptr, &target);
    };
    const auto addText = [&](const TextinvaderText &control) {
        if (!control.active || control.text.empty()) return;
        items.push_back({control.layer, control.order, [&game, &control, this] {
            drawText(renderer, game.fonts[control.font], control.text,
                     control.x, control.y);
        }});
    };
    for (const auto &slot : game.slots) {
        addText(slot.word);
        addText(slot.typed);
        if (slot.lightning.active) {
            items.push_back({slot.lightning.layer, slot.lightning.order,
                             [&, anim = &slot.lightning] { drawClip(*anim); }});
        }
    }
    if (game.mountain.active) {
        items.push_back({game.mountain.layer, game.mountain.order, [&] {
            SDL_RenderCopy(renderer, game.mountainTexture.get(), nullptr, nullptr);
        }});
    }
    if (game.lightsOff.active) {
        items.push_back({game.lightsOff.layer, game.lightsOff.order, [&] {
            const SDL_Rect target{kLightsOrigin.x, kLightsOrigin.y, kLightsWidth,
                                  kLightsHeight};
            SDL_RenderCopy(renderer, game.lightsOffTexture.get(), nullptr, &target);
        }});
    }
    if (game.startSign.active) {
        items.push_back({game.startSign.layer, game.startSign.order,
                         [&] { drawClip(game.startSign); }});
    }
    if (game.scoreSign.active) {
        items.push_back({game.scoreSign.layer, game.scoreSign.order, [&] {
            SDL_RenderCopy(renderer, game.signTexture.get(), nullptr, &kSignBounds);
        }});
    }
    if (game.scoreText.active && !game.scoreText.text.empty()) {
        items.push_back({game.scoreText.layer, game.scoreText.order, [&] {
            drawCenteredText(renderer, messageFont, game.scoreText.text, kScoreRect);
        }});
    }
    if (game.initialized) {
        // PROGRESS_Set 0x00408b00 copies round(350 x value) columns of
        // julelys1 from the left (0x00408bdb-0x00408c00).
        items.push_back({100, game.lightsOrder, [&] {
            const int width = static_cast<int>(
                std::floor(static_cast<float>(kLightsWidth) * game.lights + 0.5F));
            if (width <= 0) return;
            const SDL_Rect source{0, 0, width, kLightsHeight};
            const SDL_Rect target{kLightsOrigin.x, kLightsOrigin.y, width,
                                  kLightsHeight};
            SDL_RenderCopy(renderer, game.lightsOnTexture.get(), &source, &target);
        }});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        return a.layer != b.layer ? a.layer < b.layer : a.order < b.order;
    });
    for (const auto &item : items) item.draw();
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
TextinvaderScene::textinvaderHotspotAt(int x, int y) const {
    if (!textinvaderGame || activeScene != "textinvader") return std::nullopt;
    const auto &game = *textinvaderGame;
    const SDL_Point point{x, y};
    if ((game.startSign.active || game.scoreSign.active) &&
        SDL_PointInRect(&point, &kSignBounds)) {
        return std::pair{kSignHotspot, kSignBounds};
    }
    return std::nullopt;
}

bool TextinvaderScene::executeTextinvaderDeveloperCommand(std::string_view command) {
    constexpr std::string_view textinvaderControlPrefix = "textinvader:";
    if (activeScene != "textinvader" || !textinvaderGame) return false;
    auto &game = *textinvaderGame;
    if (!game.initialized) initializeTextinvaderScene();
    const auto control = command.substr(textinvaderControlPrefix.size());
    const auto ensurePlaying = [&] {
        if (!game.playing && !game.gameOverModal) startTextinvaderGame();
    };
    if (control == "start") {
        // As if the Start clip had just finished (0x0044c920).
        if (!game.playing) {
            if (game.startSign.active) removeTextinvaderAnim(game.startSign);
            game.starting = false;
            startTextinvaderGame();
        }
    } else if (control.rfind("seed:", 0) == 0) {
        // Capture fixture: reseed rand() at every Start, as the x32dbg script
        // does at 0x0044c950 (seed 48869 = 0xBEE5).
        game.fixedSeed = static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(5))));
    } else if (control == "difficulty:easy" || control == "difficulty:medium" ||
               control == "difficulty:hard") {
        // Only a tournament sets this in the original (0x004194a0).
        game.difficulty = control == "difficulty:easy" ? 0 :
                          control == "difficulty:medium" ? 1 : 2;
    } else if (control.rfind("type:", 0) == 0) {
        ensurePlaying();
        typeTextinvader(control.substr(5));
    } else if (control == "type-word" || control == "skip-word") {
        // Wait for a live, uncompleted word, then type all of the oldest one,
        // the order the capture scenarios type spawned words in; skip-word
        // leaves that word falling instead.
        if (!game.playing) return false;
        TextinvaderSlot *found = nullptr;
        for (int index = 0; index < game.maximumWords; ++index) {
            auto &slot = game.slots[static_cast<std::size_t>(index)];
            if (slot.active && !slot.completed && !slot.developerSkip &&
                (!found || slot.spawnSerial < found->spawnSerial)) {
                found = &slot;
            }
        }
        if (!found) return false;
        if (control == "skip-word") {
            found->developerSkip = true;
            return true;
        }
        for (const unsigned char character : std::string(found->text)) {
            keyTextinvader(character);
        }
    } else if (control.rfind("wait=", 0) == 0) {
        // Hold the command queue for this many milliseconds of scene time.
        if (!game.developerWaitUntil) {
            game.developerWaitUntil = textinvaderNow() +
                std::stoi(std::string(control.substr(5)));
        }
        if (textinvaderNow() < *game.developerWaitUntil) return false;
        game.developerWaitUntil.reset();
    } else if (control == "wrong") {
        ensurePlaying();
        typeTextinvader("-");
    } else if (control.rfind("spawn:", 0) == 0) {
        // Replace slot 0's word, keeping its position.
        ensurePlaying();
        const auto word = utf8ToCp1252(control.substr(6));
        if (!word || word->empty() || word->size() >= game.wordsByLength.size())
            return false;
        auto &slot = game.slots[0];
        if (!slot.active) return false;
        slot.text = *word;
        slot.width = textWidth(game.fonts[kBlueFont], slot.text);
        slot.rect.right = slot.rect.left + slot.width;
        showTextinvaderWord(0);
    } else if (control.rfind("bonus:", 0) == 0) {
        ensurePlaying();
        const auto name = control.substr(6);
        const int bonus = name == "yellow" ? 1 : name == "orange" ? 2 :
                          name == "red" ? 3 : 0;
        if (bonus == 0 || !game.slots[0].active) return false;
        game.slots[0].bonus = bonus;
        showTextinvaderWord(0);
    } else if (control == "miss") {
        // Drop slot 0 below the miss line; the next tick misses it.
        ensurePlaying();
        if (!game.slots[0].active) return false;
        game.slots[0].fallY = static_cast<float>(kMissY + 1);
    } else if (control.rfind("level:", 0) == 0) {
        const int level = std::stoi(std::string(control.substr(6)));
        if (level < 0 || level > 30) return false;
        ensurePlaying();
        game.removedWords = level *
            kLevelThresholds[static_cast<std::size_t>(game.difficulty)];
    } else if (control == "drain") {
        ensurePlaying();
        game.energy = 0;
        updateTextinvaderHud();
        checkTextinvaderGameOver();
    } else if (control == "status") {
        std::cout << "Text Invader status: difficulty " << game.difficulty + 1
                  << ", playing " << game.playing << ", score " << game.score
                  << ", energy " << game.energy << ", completed "
                  << game.completedWords << ", removed " << game.removedWords
                  << ", level " << game.levelStep << ", max length "
                  << game.maximumLength << ", words " << game.maximumWords
                  << ", typed \"" << latin1ToUtf8(game.typed) << "\"\n";
        for (int index = 0; index < game.maximumWords; ++index) {
            const auto &slot = game.slots[static_cast<std::size_t>(index)];
            if (!slot.active) continue;
            std::cout << "  slot " << index << ": \"" << latin1ToUtf8(slot.text)
                      << "\" at (" << slot.x << ',' << slot.y << ") bonus "
                      << slot.bonus << (slot.completed ? " completed" : "")
                      << '\n';
        }
    } else {
        throw std::runtime_error("Unknown developer Text Invader control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
