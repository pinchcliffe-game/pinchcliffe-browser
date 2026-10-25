#include "game/app_hustle.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

// Every clip Hustle plays, with the default coordinates its static
// descriptor holds at +0x54/+0x58 (FGP.exe 0x004d9e20-0x004daf60).
struct HustleClipSpec {
    std::string_view name;
    int x;
    int y;
};
constexpr std::size_t kClipStart = 0U;
constexpr std::size_t kClipHand = 1U;
constexpr std::size_t kClipClockLoop = 2U;
constexpr std::size_t kClipClick = 3U;       // click1..click4
constexpr std::size_t kClipIntro = 7U;
constexpr std::size_t kClipIntro2a = 8U;
constexpr std::size_t kClipIntro2b = 9U;
constexpr std::size_t kClipIntro3 = 10U;
constexpr std::size_t kClipMove = 11U;       // 0x004da750, 11 families
constexpr std::size_t kClipWin = 22U;        // 0x004db008
constexpr std::size_t kClipLose = 25U;       // 0x004db014
constexpr std::size_t kClipReveal = 28U;     // 0x004db020
const auto kClips = originalArray<HustleClipSpec, 31>([](const OriginalData &source, std::size_t index) {
    constexpr std::array<std::uint32_t, 31> descriptors{
        0x004d9ec8U, 0x004d9e20U, 0x004d9f70U, 0x004d3758U, 0x004d3778U, 0x004d3798U, 0x004d37b8U, 0x004da780U, 0x004da828U, 0x004da8d0U, 0x004da978U, 0x004da018U, 0x004da0c0U, 0x004da168U, 0x004da210U, 0x004da2b8U, 0x004da360U, 0x004da408U, 0x004da4b0U, 0x004da558U, 0x004da600U, 0x004da6a8U, 0x004daa20U, 0x004daac8U, 0x004dab70U, 0x004dac18U, 0x004dacc0U, 0x004dad68U, 0x004dae10U, 0x004daeb8U, 0x004daf60U};
    const auto address = descriptors[index];
    return HustleClipSpec{source.string(address + 0), source.read<int>(address + 0x54), source.read<int>(address + 0x58)};
});

// The cups bitmap descriptor 0x004d9d78 sits at (286,334); 0x0043cfdd adds
// it at layer 10 with default coordinates and 0x0043d006 registers
// cupshs.bmp as its mode-4 hotspot mask (0x00413670 stores it at +0xa4).
// cups.bmp itself is entirely chroma green, so it draws nothing: the cups
// on screen belong to the monkey's clips.
constexpr SDL_Point kCupsPosition{286, 334};
// hotspots.bmp ids 3-7: the three cups, start_0 and the bet control.
constexpr std::uint32_t kCupHotspotBase = 3U;
constexpr std::uint32_t kStartHotspot = 6U;
constexpr std::uint32_t kBetHotspot = 7U;
constexpr std::uint32_t kScrollHotspot = 21U;
// The bet control 0x0043d1db creates: id 7, only a hover image (bet_1).
constexpr SDL_Point kBetPosition{693, 404};
// The banana bar 0x0043d0c7 creates at (12,244), filled from the bottom.
constexpr SDL_Point kBarPosition{12, 244};
// Text controls from the rectangle rows 0x0049e930 (stake) and 0x0049e940
// (bananas), left/top/right/bottom, drawn centered (flags 0x101).
constexpr SDL_Rect kStakeText{15, 529, 89 - 15, 546 - 529};
constexpr SDL_Rect kBananaText{15, 559, 89 - 15, 576 - 559};
// Scroll control 0x15 (0x0043d0ed): x 75, width 15, track bottom 0x209.
constexpr int kScrollX = 75;
constexpr int kScrollWidth = 15;
constexpr int kScrollBottom = 0x209;
constexpr int kScrollFullHeight = 278;   // 0x0049ea74
constexpr double kPositionScale = 0.011111111380159855;  // 0x0049ea70, 1/90

// FGP.exe 0x0049e984 and 0x0049e9b8, indexed by level.
const auto kShuffleCounts = originalArray<int, 13>(0x0049e984U);
const auto kMoveFamilyCounts = originalArray<int, 13>(0x0049e9b8U);
// 0x0049e950: the SmackFrameRate value 0x0043d268 sets per level.
const auto kShuffleFrameRates = originalArray<int, 13>(0x0049e950U);
// 0x0049e9ec: ternary state transition selected as
// table[currentCup + moveFamily * 3].
const auto kCupTransitions = originalArray<int, 33>(0x0049e9ecU);
// 0x0043d3c0 builds {1,10,100,1000,10000,100000}; 0x0043d320 builds the
// same table without its first entry.
constexpr std::array<int, 6> kStakeScales{{1,10,100,1000,10000,100000}};
// 0x0043d5ec-0x0043d625: the clock runs (now - start) * 3.3333333e-6, so
// one revolution of hand_0 takes 300 seconds.
constexpr double kClockScale = 3.3333333249174757e-06;
// 0x0043cf5d-0x0043cf6a: EVENT_AddTimer(now, 1000, 0).
constexpr double kTimerPeriod = 1.0;
// 0x0043d22e: a round leaves the game once fewer than 10 bananas remain.
constexpr int kMinimumBananas = 10;

// 0x0043d3c0: slider position (0..90) to stake.
int hustleStake(int position) {
    position = std::clamp(position, 0, 90);
    return kStakeScales[static_cast<std::size_t>(position / 18)] *
           (position % 18 + 2) * 5;
}

// 0x0043d320: bananas to the largest slider position (0..100).
int hustleMaximumPosition(int bananas) {
    if (bananas <= 10) return 0;
    int index = 4;
    while (index > 0 &&
           kStakeScales[static_cast<std::size_t>(index) + 1U] > bananas)
        --index;
    const int scale = kStakeScales[static_cast<std::size_t>(index) + 1U];
    const int position = (bananas - scale) / (scale * 5 / 10) + index * 18;
    return std::clamp(position, 0, 100);
}

// The x87 rounding idiom 0x0043d4d8-0x0043d508 uses: add or subtract 0.5
// by sign, then truncate (0x0048c214).
int roundAway(double value) {
    return static_cast<int>(value > 0.0 ? value + 0.5 : value - 0.5);
}

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

TexturePtr mediaTexture(SDL_Renderer *renderer, const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame = media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!texture) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

// Completion of a clip opened while SmackFrameRate overrides the rate.
// UNCERTAIN: the original trace measured anim_a and anim_d at exactly
// (frames - 1) / fps; anim_c, anim_e and anim_f finished up to 110 ms
// earlier, which this does not model.
double hustleClipDuration(const DecodedMedia &media, int fps) {
    if (fps <= 0 || media.videoFrames.empty())
        return originalSmackerCompletionDuration(media);
    return static_cast<double>(media.videoFrames.size() - 1U) / fps;
}

std::size_t hustleFrameIndex(const DecodedMedia &media, double time,
                             int fps) {
    if (media.videoFrames.empty()) return 0U;
    if (fps <= 0) return frameIndexAt(media, time, false);
    const auto frame = static_cast<std::size_t>(
        std::max(0.0, std::floor(time * fps)));
    return std::min(frame, media.videoFrames.size() - 1U);
}

} // namespace

HustleScene::HustleGame HustleScene::loadHustleGame(Package &package) {
    HustleGame game;
    const auto asset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {
                 sceneLanguagePackage.get(), sceneAddonPackage.get(),
                 sceneData2Package.get(), sceneEditionPackage.get(), &package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Hustle asset not found: " +
                                 std::string(name));
    };
    game.cupsTexture = keyedTexture(renderer, asset("bitmap\\cups.bmp"));
    game.cupsHotspotMask = loadBmp(asset("bitmap\\cupshs.bmp"));
    game.barTexture = keyedTexture(renderer, asset("bitmap\\bar.bmp"));
    game.scrollTexture = keyedTexture(renderer, asset("bitmap\\scroll.bmp"));
    game.betTexture = keyedTexture(renderer, asset("bitmap\\bet_1.bmp"));
    game.clips.reserve(kClips.size());
    for (const auto &spec : kClips) {
        HopscotchMedia placed;
        // Palette index 0 is a drawn colour in these clips (the pale cup
        // highlights of intro/win2, the jacket of lose1/intro3, black in
        // anim_b); only chroma green is transparent. The original win
        // checkpoint shows those pixels solid.
        placed.media = decodeSmacker(
            activeSceneAnimationData(std::string(spec.name)), true, true, true,
            0, nullptr, SmackerIndexZeroAlpha::Opaque);
        placed.texture = mediaTexture(renderer, placed.media);
        game.clips.push_back(std::move(placed));
    }
    std::cout << "Loaded executable-derived Hustle: start_0 attract, cups "
                 "bitmap with cupshs hotspot mask, slider stakes, "
                 "slider-chosen level, per-level shuffle frame rate, "
                 "300-second hand_0 clock and Game Over below 10 bananas\n";
    return game;
}

void HustleScene::addHustleClip(std::size_t clip, int x, int y, int layer,
                                std::uint32_t hotspot,
                                std::uint32_t conflictMask) {
    const auto name = kClips[clip].name;
    referenceTrace.emit("animation_start", {
        {"asset", name}, {"x", x}, {"y", y}, {"layer", layer},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", conflictMask}});
}

void HustleScene::removeHustleClip(std::size_t clip) {
    referenceTrace.emit("animation_remove", {
        {"asset", kClips[clip].name}, {"reason", "removed"}});
}

void HustleScene::finishHustleClip(std::size_t clip) {
    referenceTrace.emit("animation_stop", {
        {"asset", kClips[clip].name}, {"reason", "completed"}});
    removeHustleClip(clip);
}

// Scene initialisation 0x0043cf20, from the scroll control onwards.
void HustleScene::announceHustleEntry() {
    auto &game = *hustleGame;
    game.entryAnnounced = true;
    game.nextTimerTick = sceneElapsedNow() + kTimerPeriod;
    // 0x0043d0ed creates scroll control 0x15; 0x0043d0f5-0x0043d104 give it
    // the range 0..90 at value 90, which puts the arrow at the bottom.
    game.scrollRange = 90;
    game.scrollValue = 90;
    game.scrollTop = 243;
    game.scrollHeight = kScrollFullHeight;
    game.barFill = 0.0;
    referenceTrace.emit("animation_start", {
        {"asset", "scroll"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", "scroll"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"hotspot", kScrollHotspot}, {"conflict_mask", 0}});
    game.startAdded = true;
    addHustleClip(kClipStart, -1, -1, 0, 0U, 0U);
    referenceTrace.emit("animation_start", {
        {"asset", "cups"}, {"x", -1}, {"y", -1}, {"layer", 10},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", "cups"}, {"x", -1}, {"y", -1}, {"layer", 10},
        {"hotspot", 0}, {"conflict_mask", 0}});
}

// Hotspot 6 while the state is 0: 0x0043d89c starts start_0.
void HustleScene::startHustleStartButton() {
    auto &game = *hustleGame;
    if (game.phase != HustlePhase::Attract || !game.startAdded ||
        game.startPlaying)
        return;
    game.startPlaying = true;
    game.startStartedAt = sceneElapsedNow();
}

// 0x0043d560, reached when start_0 finishes.
void HustleScene::setupHustleGame(double now) {
    auto &game = *hustleGame;
    game.clockStartedAt = now;
    game.clockRunning = true;
    game.phase = HustlePhase::Betting;
    // 0x0043d57e stores 1; outside tournament play 0x00419490 leaves it.
    game.difficulty = 1;
    game.rounds = 0;
    setHustleBananas(100);
    // 0x0043d5b1-0x0043d5b5 moves the arrow to the top: the whole bankroll.
    setHustleScrollValue(0);
    updateHustleClock(now);
    referenceTrace.emit("animation_start", {
        {"asset", "clockloop"}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"loop", true}});
    referenceTrace.emit("animation_add", {
        {"asset", "clockloop"}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    game.clockLoopActive = true;
    game.clockLoopNextAt = now;
}

// 0x0043d280: store and print the bankroll, then resize bar and slider.
void HustleScene::setHustleBananas(int bananas) {
    auto &game = *hustleGame;
    game.bananas = bananas;
    const int maximum = hustleMaximumPosition(bananas);
    game.barFill = std::clamp(maximum * kPositionScale, 0.0, 1.0);
    rebuildHustleScroll(maximum);
    game.stakeShown = bananas > 0
        ? hustleStake(game.scrollRange - game.scrollValue) : 0;
    game.stakeShownValid = true;
}

// 0x0043d440: recreate the scroll control for a new maximum position and
// keep the arrow at the same fraction of its track.
void HustleScene::rebuildHustleScroll(int maximumPosition) {
    auto &game = *hustleGame;
    const double fraction = game.scrollRange > 0
        ? static_cast<double>(game.scrollValue) / game.scrollRange : 0.0;
    const int clamped = std::clamp(maximumPosition, 0, 90);
    const int height = static_cast<int>(
        clamped * kPositionScale * kScrollFullHeight);
    referenceTrace.emit("animation_remove", {
        {"asset", "scroll"}, {"reason", "removed"}});
    referenceTrace.emit("animation_start", {
        {"asset", "scroll"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", "scroll"}, {"x", -1}, {"y", -1}, {"layer", 0},
        {"hotspot", kScrollHotspot}, {"conflict_mask", 0}});
    game.scrollTop = kScrollBottom - height;
    game.scrollHeight = height;
    game.scrollRange = std::max(0, clamped);
    game.scrollValue = 0;
    setHustleScrollValue(roundAway(fraction * clamped));
}

// 0x00408eb0 and 0x00408f30: place the arrow for a value, then read the
// value back from the arrow's pixel position.
void HustleScene::setHustleScrollValue(int value) {
    auto &game = *hustleGame;
    value = std::clamp(value, 0, game.scrollRange);
    int position = game.scrollTop;
    if (game.scrollRange > 0)
        position = game.scrollTop +
                   game.scrollHeight * value / game.scrollRange;
    const int previous = game.scrollValue;
    game.scrollValue = game.scrollHeight > 0
        ? roundAway(static_cast<double>(position - game.scrollTop) *
                    game.scrollRange / game.scrollHeight)
        : 0;
    // A changed value posts event 0x117; 0x0043d850 reprints the stake
    // only while betting.
    if (game.scrollValue != previous && game.phase == HustlePhase::Betting)
        game.stakeShown = hustleStake(game.scrollRange - game.scrollValue);
}

// Control 7 while betting: 0x0043d9c0, 0x0043da30 and 0x0043da80.
void HustleScene::placeHustleBet(double now) {
    auto &game = *hustleGame;
    const int position = game.scrollRange - game.scrollValue;
    game.level = std::clamp(game.difficulty + position / 9, 0, 11);
    game.stake = hustleStake(position);
    ++game.rounds;
    const auto level = static_cast<std::size_t>(game.level);
    const int moves = kShuffleCounts[level];
    std::uniform_int_distribution<int> family(0, kMoveFamilyCounts[level] - 1);
    game.moves.clear();
    for (int move = 0; move < moves; ++move)
        game.moves.push_back(game.pinnedMoveFamily
                                 ? *game.pinnedMoveFamily
                                 : family(randomEngine));
    std::vector<std::size_t> sequence;
    if (game.rounds <= 2) {
        sequence.push_back(kClipIntro);
    } else {
        sequence.push_back(kClipIntro2a);
        if (game.rounds <= 6) sequence.push_back(kClipIntro2b);
    }
    for (const int move : game.moves)
        sequence.push_back(kClipMove + static_cast<std::size_t>(move));
    sequence.push_back(kClipIntro3);
    requestHustleSequence(std::move(sequence), now);
    game.phase = HustlePhase::Shuffling;
}

// 0x0043d920: walk the recorded moves through the transition table.
int HustleScene::hustleBallCup() const {
    int cup = 1;
    for (const int move : hustleGame->moves)
        cup = kCupTransitions[static_cast<std::size_t>(cup + move * 3)];
    return cup;
}

// Hotspots 3-5 while choosing: 0x0043d8c0.
void HustleScene::chooseHustleCup(int cup, double now) {
    const int ball = hustleBallCup();
    if (cup == ball) {
        requestHustleSequence({kClipWin + static_cast<std::size_t>(ball)}, now);
    } else {
        requestHustleSequence({kClipLose + static_cast<std::size_t>(cup),
                               kClipReveal + static_cast<std::size_t>(ball)},
                              now);
    }
}

// 0x0040a6f0 installs the table for the monkey's character group (conflict
// bit 1). 0x0040a610 refuses to add it while the group's clip is running,
// so the first clip starts at that clip's end.
// UNCERTAIN: a second request replaces the remaining entries here; the
// original's behaviour for a click during a running result is unverified.
void HustleScene::requestHustleSequence(std::vector<std::size_t> clips,
                                        double now) {
    auto &game = *hustleGame;
    game.sequence = std::move(clips);
    game.sequenceIndex = 0U;
    if (game.sequencePlaying) {
        game.sequenceRequested = true;
        return;
    }
    game.sequenceRequested = true;
    game.sequenceStartsAt = sceneCharacterConflictReleaseAt(1U, now);
    sceneSequencePendingConflictMask |= 1U;
}

void HustleScene::startHustleSequenceClip(double startedAt) {
    auto &game = *hustleGame;
    const auto clip = game.sequence[game.sequenceIndex];
    game.sequenceClip = {clip, game.frameRateOverride, startedAt};
    game.sequencePlaying = true;
    addHustleClip(clip, kClips[clip].x, kClips[clip].y, 0, 0U, 1U);
    audioMixer.addEffect(game.clips[clip].media,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
}

// The scene's animation-finished callback 0x0043d160.
void HustleScene::hustleAnimationFinished(std::size_t clip, double now) {
    auto &game = *hustleGame;
    if (clip == kClipIntro || clip == kClipIntro2b ||
        (clip == kClipIntro2a && game.rounds > 6)) {
        game.frameRateOverride =
            kShuffleFrameRates[static_cast<std::size_t>(game.level)];
        return;
    }
    if (clip == kClipIntro3) {
        game.frameRateOverride = -1;
        game.phase = HustlePhase::Choosing;
        return;
    }
    if (clip == kClipStart) {
        game.startAdded = false;
        game.startPlaying = false;
        game.betControl = true;
        setupHustleGame(now);
        return;
    }
    const bool won = clip >= kClipWin && clip < kClipWin + 3U;
    const bool lost = clip >= kClipLose && clip < kClipLose + 3U;
    if (!won && !lost) return;
    const int bananas = game.bananas + (won ? game.stake : -game.stake);
    setHustleBananas(bananas);
    game.phase = HustlePhase::Betting;
    if (bananas < kMinimumBananas || !game.clockRunning)
        endHustleGame(bananas);
}

// 0x0043d5e0: step hand_0 and click once per step. Returns false once the
// clock has passed a full revolution on a step.
bool HustleScene::updateHustleClock(double now) {
    auto &game = *hustleGame;
    if (!game.handAdded) {
        game.handAdded = true;
        game.handFrame = 0;
        addHustleClip(kClipHand, -1, -1, -1, 0U, 0U);
    }
    const double elapsedMs =
        std::floor((now - game.clockStartedAt) * 1000.0);
    // 0x0043d62b stores the product as a float before using it.
    const double turn = static_cast<float>(elapsedMs * kClockScale);
    const int frames = static_cast<int>(
        game.clips[kClipHand].media.videoFrames.size());
    const int frame = roundAway(frames * turn) + 1;
    if (frame == game.handFrame) return true;
    game.handFrame = frame;
    std::uniform_int_distribution<int> click(0, 3);
    const auto clip = kClipClick + static_cast<std::size_t>(click(randomEngine));
    if (game.clickClip) removeHustleClip(*game.clickClip);
    game.clickClip = clip;
    game.clickStartedAt = now;
    addHustleClip(clip, 0, 0, 0, 0U, 0U);
    audioMixer.addEffect(game.clips[clip].media,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    return turn < 1.0;
}

// Timer event 0x111: 0x0043d950.
void HustleScene::hustleTimerTick(double now) {
    auto &game = *hustleGame;
    if (!game.clockRunning) return;
    game.clockRunning = updateHustleClock(now);
    if (!game.clockRunning && game.phase == HustlePhase::Betting)
        endHustleGame(game.bananas);
}

// 0x0043d770.
void HustleScene::endHustleGame(int bananas) {
    auto &game = *hustleGame;
    game.phase = HustlePhase::Attract;
    // 0x0043d77a: 0x0040a880 seeds the monkey's group again, which drops
    // the rest of its external sequence. Capture hustle-lose shows em_bo4
    // added instead of the noits reveal that would follow lose1.
    game.sequence.clear();
    game.sequenceRequested = false;
    sceneSequencePendingConflictMask &= ~1U;
    sceneSequenceHeldConflictMask &= ~1U;
    if (!game.startAdded) {
        game.startAdded = true;
        game.startPlaying = false;
        addHustleClip(kClipStart, -1, -1, 0, 0U, 0U);
    }
    if (game.handAdded) {
        game.handAdded = false;
        removeHustleClip(kClipHand);
    }
    if (game.clockLoopActive) {
        game.clockLoopActive = false;
        audioMixer.removeEffect(game.clockLoopEffect);
        game.clockLoopEffect = LiveAudioMixer::kInvalidEffectHandle;
        removeHustleClip(kClipClockLoop);
    }
    game.betControl = false;
    game.betPressed = false;
    game.frameRateOverride = -1;
    game.finalBananas = bananas;
    // 0x0043d7f8-0x0043d80b picks NOMONEY unless more than 10 bananas
    // remain; 0x0043d829 shows it under interfaceh:GAMEOVER, then
    // 0x0043d838 passes the bananas and medal table 0x004d9d68 to the
    // shared score wrapper 0x0041f9a0.
    const bool noMoney = bananas <= kMinimumBananas;
    showMessageWithTitle(
        language.get("interfaceh", "GAMEOVER", "Spillet er slutt"),
        noMoney ? language.get("hustle", "NOMONEY",
                               "Beklager, du har ikke nok bananer \xe5 vedde med.")
                : language.get("hustle", "TIMEOUT", "Beklager, tiden er ute."),
        MessageKind::HustleGameOver);
}

bool HustleScene::beginHustleInteraction(int x, int y) {
    if (!hustleGame || activeScene != "hustle") return false;
    auto &game = *hustleGame;
    const auto hit = hustleHotspotAt(x, y);
    if (!hit) return false;
    const auto hotspot = hit->first;
    const double now = sceneElapsedNow();
    referenceTrace.emit("click_dispatch", {
        {"scene", "hustle"}, {"hotspot", hotspot}});
    if (hotspot == kScrollHotspot) {
        game.betDragging = true;
        return true;
    }
    // Hotspot event 0x105 reaches 0x0043d880 on the press, also for the
    // bet control's hotspot 7; that id does nothing there.
    if (hotspot == kBetHotspot) {
        game.betPressed = game.betControl;
        return true;
    }
    if (hotspot == kStartHotspot) {
        startHustleStartButton();
    } else if (hotspot >= kCupHotspotBase && hotspot < kCupHotspotBase + 3U &&
               game.phase == HustlePhase::Choosing) {
        chooseHustleCup(static_cast<int>(hotspot - kCupHotspotBase), now);
    }
    return true;
}

// 0x00408f30 for a dragged arrow: the pointer's y, clamped to the track.
void HustleScene::moveHustleBet(int y) {
    if (!hustleGame) return;
    auto &game = *hustleGame;
    const int position = std::clamp(
        y, game.scrollTop, game.scrollTop + game.scrollHeight);
    const int previous = game.scrollValue;
    game.scrollValue = game.scrollHeight > 0
        ? roundAway(static_cast<double>(position - game.scrollTop) *
                    game.scrollRange / game.scrollHeight)
        : 0;
    if (game.scrollValue != previous && game.phase == HustlePhase::Betting)
        game.stakeShown = hustleStake(game.scrollRange - game.scrollValue);
}

void HustleScene::endHustleInteraction(int x, int y) {
    if (!hustleGame) return;
    auto &game = *hustleGame;
    game.betDragging = false;
    if (!game.betPressed) return;
    game.betPressed = false;
    const auto hit = hustleHotspotAt(x, y);
    if (!hit || hit->first != kBetHotspot || !game.betControl) return;
    // Control click event 0x116 on release: 0x0043d980 bets in state 1.
    referenceTrace.emit("click_dispatch", {
        {"scene", "hustle"}, {"hotspot", kBetHotspot}});
    if (game.phase == HustlePhase::Betting) placeHustleBet(sceneElapsedNow());
}

void HustleScene::updateHustle(double elapsed) {
    if (!hustleGame) return;
    auto &game = *hustleGame;
    if (!game.entryAnnounced) announceHustleEntry();
    // One tick per pass, advanced by one period (0x0040bf9c): a stall is
    // made up over the following frames, not in one.
    if (elapsed >= game.nextTimerTick) {
        const double tick = game.nextTimerTick;
        game.nextTimerTick += kTimerPeriod;
        // The original's modal dialogs hold the scene's timer events: the
        // hustle-lose capture shows no clock step while Game Over is open.
        if (dialogMode == DialogMode::None) hustleTimerTick(tick);
    }
    if (game.startPlaying &&
        elapsed - game.startStartedAt >= originalSmackerCompletionDuration(
            game.clips[kClipStart].media)) {
        finishHustleClip(kClipStart);
        hustleAnimationFinished(kClipStart,
                                game.startStartedAt +
                                    originalSmackerCompletionDuration(
                                        game.clips[kClipStart].media));
    }
    if (game.clickClip &&
        elapsed - game.clickStartedAt >= originalSmackerCompletionDuration(
            game.clips[*game.clickClip].media)) {
        removeHustleClip(*game.clickClip);
        game.clickClip.reset();
    }
    if (game.clockLoopActive && elapsed >= game.clockLoopNextAt) {
        const auto &loop = game.clips[kClipClockLoop].media;
        game.clockLoopEffect = audioMixer.addEffect(
            loop, audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
        game.clockLoopNextAt += std::max(0.1, loop.videoDuration);
    }
    if (game.sequenceRequested && !game.sequencePlaying &&
        elapsed >= game.sequenceStartsAt) {
        game.sequenceRequested = false;
        acquireSceneCharacterConflicts(1U, elapsed);
        sceneSequencePendingConflictMask &= ~1U;
        sceneSequenceHeldConflictMask |= 1U;
        startHustleSequenceClip(game.sequenceStartsAt);
    }
    while (game.sequencePlaying) {
        const auto &active = game.sequenceClip;
        const double finishedAt = active.startedAt + hustleClipDuration(
            game.clips[active.clip].media, active.fps);
        if (elapsed < finishedAt) break;
        game.sequencePlaying = false;
        finishHustleClip(active.clip);
        const auto finished = active.clip;
        hustleAnimationFinished(finished, finishedAt);
        if (game.sequenceRequested) {
            // A replacement request arrived while this clip played.
            game.sequenceRequested = false;
            startHustleSequenceClip(finishedAt);
            continue;
        }
        if (game.sequence.empty()) break;
        if (++game.sequenceIndex < game.sequence.size()) {
            startHustleSequenceClip(finishedAt);
        } else {
            game.sequence.clear();
            sceneSequenceHeldConflictMask &= ~1U;
        }
    }
}

void HustleScene::renderHustle(double elapsed) {
    if (!hustleGame) return;
    auto &game = *hustleGame;
    const auto drawClip = [&](std::size_t clip, int x, int y,
                              std::size_t frameIndex) {
        auto &placed = game.clips[clip];
        if (!placed.texture || placed.media.videoFrames.empty()) return;
        const auto &frame = placed.media.videoFrames[std::min(
            frameIndex, placed.media.videoFrames.size() - 1U)];
        uploadVideoFrame(placed.texture.get(), frame);
        SDL_Rect destination{x, y, frame.width, frame.height};
        SDL_RenderCopy(renderer, placed.texture.get(), nullptr, &destination);
    };
    // Layer -1: the clock hand over the backdrop's watch face.
    if (game.handAdded)
        drawClip(kClipHand, kClips[kClipHand].x, kClips[kClipHand].y,
                 static_cast<std::size_t>(std::max(0, game.handFrame - 1)));
    // Layer 0: the shuffle/result sequence, start_0 and the scroll arrow.
    if (game.sequencePlaying) {
        const auto &active = game.sequenceClip;
        drawClip(active.clip, kClips[active.clip].x, kClips[active.clip].y,
                 hustleFrameIndex(game.clips[active.clip].media,
                                  elapsed - active.startedAt, active.fps));
    }
    if (game.startAdded) {
        const std::size_t frame = game.startPlaying
            ? hustleFrameIndex(game.clips[kClipStart].media,
                               elapsed - game.startStartedAt, -1)
            : 0U;
        drawClip(kClipStart, kClips[kClipStart].x, kClips[kClipStart].y,
                 frame);
    }
    int barWidth = 0;
    int barHeight = 0;
    SDL_QueryTexture(game.barTexture.get(), nullptr, nullptr, &barWidth,
                     &barHeight);
    // 0x00408bd1-0x00408c2c copies the bottom fill * height rows.
    double fillRows = barHeight * game.barFill;
    const int filled = std::clamp(roundAway(fillRows), 0, barHeight);
    if (filled > 0) {
        SDL_Rect source{0, barHeight - filled, barWidth, filled};
        SDL_Rect destination{kBarPosition.x,
                             kBarPosition.y + barHeight - filled, barWidth,
                             filled};
        SDL_RenderCopy(renderer, game.barTexture.get(), &source, &destination);
    }
    int scrollWidth = 0;
    int scrollHeight = 0;
    SDL_QueryTexture(game.scrollTexture.get(), nullptr, nullptr, &scrollWidth,
                     &scrollHeight);
    const int arrowY = game.scrollRange > 0
        ? game.scrollTop + game.scrollHeight * game.scrollValue /
                               game.scrollRange
        : game.scrollTop;
    // 0x00412cf0 centres the arrow on (x + width / 2, position).
    SDL_Rect arrow{kScrollX + kScrollWidth / 2 - scrollWidth / 2,
                   arrowY - scrollHeight / 2, scrollWidth, scrollHeight};
    SDL_RenderCopy(renderer, game.scrollTexture.get(), nullptr, &arrow);
    if (game.betControl && hotspotAt(mouseX, mouseY) == kBetHotspot) {
        SDL_Rect bet{kBetPosition.x, kBetPosition.y, 108, 116};
        SDL_QueryTexture(game.betTexture.get(), nullptr, nullptr, &bet.w,
                         &bet.h);
        SDL_RenderCopy(renderer, game.betTexture.get(), nullptr, &bet);
    }
    // Layer 10: the cups bitmap.
    SDL_Rect cups{kCupsPosition.x, kCupsPosition.y, 0, 0};
    SDL_QueryTexture(game.cupsTexture.get(), nullptr, nullptr, &cups.w,
                     &cups.h);
    SDL_RenderCopy(renderer, game.cupsTexture.get(), nullptr, &cups);
    if (game.stakeShownValid) {
        drawCenteredText(renderer, dialogFont, std::to_string(game.stakeShown),
                         kStakeText);
        drawCenteredText(renderer, dialogFont, std::to_string(game.bananas),
                         kBananaText);
    }
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
HustleScene::hustleHotspotAt(int x, int y) const {
    if (!hustleGame || activeScene != "hustle") return std::nullopt;
    const auto &game = *hustleGame;
    const SDL_Point point{x, y};
    // 0x004136c0 tests active descriptors from the top before the scene's
    // hotspot map: the scroll arrow (hotspot 21), then the cups mask.
    const int arrowY = game.scrollRange > 0
        ? game.scrollTop + game.scrollHeight * game.scrollValue /
                               game.scrollRange
        : game.scrollTop;
    const SDL_Rect arrow{kScrollX + kScrollWidth / 2 - 18, arrowY - 15, 37, 31};
    if (SDL_PointInRect(&point, &arrow))
        return std::pair{kScrollHotspot, arrow};
    if (game.cupsHotspotMask) {
        SDL_Surface *mask = game.cupsHotspotMask.get();
        const SDL_Rect cups{kCupsPosition.x, kCupsPosition.y, mask->w, mask->h};
        if (SDL_PointInRect(&point, &cups)) {
            const auto value = pixelValue(mask, x - cups.x, y - cups.y);
            if (value >= kCupHotspotBase && value < kCupHotspotBase + 3U)
                return std::pair{static_cast<std::uint32_t>(value), cups};
        }
    }
    if (sceneHotspots) {
        const auto value = pixelValue(sceneHotspots.get(), x, y);
        if (value >= kCupHotspotBase && value <= kBetHotspot)
            return std::pair{static_cast<std::uint32_t>(value),
                             SDL_Rect{x, y, 1, 1}};
    }
    return std::nullopt;
}

bool HustleScene::executeHustleDeveloperCommand(std::string_view command) {
    constexpr std::string_view hustleControlPrefix = "hustle:";
    if (activeScene != "hustle" || !hustleGame) return false;
    auto &game = *hustleGame;
    const auto control = command.substr(hustleControlPrefix.size());
    const double now = sceneElapsedNow();
    if (control == "start") {
        if (game.phase != HustlePhase::Attract) return false;
        startHustleStartButton();
    } else if (control.rfind("stake-position:", 0) == 0) {
        if (game.phase != HustlePhase::Betting) return false;
        const int position = std::stoi(std::string(control.substr(15)));
        if (position < 0 || position > game.scrollRange) return false;
        setHustleScrollValue(game.scrollRange - position);
    } else if (control == "bet") {
        if (game.phase != HustlePhase::Betting || !game.betControl)
            return false;
        placeHustleBet(now);
    } else if (control.rfind("choose:", 0) == 0) {
        const int cup = std::stoi(std::string(control.substr(7))) - 1;
        if (cup < 0 || cup >= 3 || game.phase != HustlePhase::Choosing)
            return false;
        chooseHustleCup(cup, now);
    } else if (control == "win" || control == "lose") {
        if (game.phase != HustlePhase::Choosing) return false;
        const int ball = hustleBallCup();
        chooseHustleCup(control == "win" ? ball : (ball + 1) % 3, now);
    } else if (control.rfind("move-family:", 0) == 0) {
        // Mirrors the reference adapter's breakpoint at 0x0043da5d, which
        // replaces each rand() result of the move loop.
        const int family = std::stoi(std::string(control.substr(12)));
        if (family < 0 || family > 10) return false;
        game.pinnedMoveFamily = family;
    } else if (control == "timeout") {
        if (!game.clockRunning) return false;
        game.clockStartedAt = now - 310.0;
    } else if (control == "status") {
        std::cout << "Hustle status: state "
                  << static_cast<int>(game.phase) << ", level "
                  << game.level << ", round " << game.rounds << ", bananas "
                  << game.bananas << ", stake " << game.stakeShown
                  << ", ball cup " << hustleBallCup() + 1 << ", moves "
                  << game.moves.size() << ", hand frame " << game.handFrame
                  << '\n';
    } else {
        throw std::runtime_error(
            "Unknown developer Hustle control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
