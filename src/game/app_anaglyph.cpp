#include "game/app_anaglyph.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "assets/profile.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// One static 0xa8-byte descriptor: name, default x/y (+0x54/+0x58), layer
// (+0x64), Smacker (+0x40), video (+0x44), keyed (+0x48), loop (+0x4c).
struct AnimSpec {
    std::string_view name;
    int x;
    int y;
    int layer;
    bool smacker;
    bool video;
    bool keyed;
    bool loop;
};

// The master list 0x00510a40 (every descriptor 0x0047d7d0 stops and
// 0x0047cf10 removes), then the music list 0x00510b6c, then help_2 and
// exit_2 (0x0050d1b0, 0x0050d258).
const auto kAnims = originalArray<AnimSpec, 87>([](const OriginalData &source, std::size_t index) {
    constexpr std::array<std::uint32_t, 87> descriptors{
        0x0050d470U, 0x0050d518U, 0x0050d5c0U, 0x0050d668U, 0x0050d710U, 0x0050d7b8U, 0x0050d860U, 0x0050d908U, 0x0050d9b0U, 0x0050da58U, 0x0050db00U, 0x0050dba8U, 0x0050dc50U, 0x0050dcf8U, 0x0050dda0U, 0x0050de48U, 0x0050df98U, 0x0050e040U, 0x0050e0e8U, 0x0050e190U, 0x0050e238U, 0x0050e2e0U, 0x0050e388U, 0x0050e430U, 0x0050e4d8U, 0x0050e580U, 0x0050e628U, 0x0050e6d0U, 0x0050e778U, 0x0050e820U, 0x0050e8c8U, 0x0050e970U, 0x0050ea18U, 0x0050eb68U, 0x0050ec10U, 0x0050ecb8U, 0x0050ed60U, 0x0050ee08U, 0x0050ef58U, 0x0050f000U, 0x0050f0a8U, 0x0050f150U, 0x0050f1f8U, 0x0050f2a0U, 0x0050f348U, 0x0050f3f0U, 0x0050f498U, 0x0050f540U, 0x0050f5e8U, 0x0050f738U, 0x0050f7e0U, 0x0050f888U, 0x0050f930U, 0x0050f9d8U, 0x0050fa80U, 0x0050fbd0U, 0x0050fc78U, 0x0050fd20U, 0x0050fdc8U, 0x0050ff18U, 0x0050ffc0U, 0x00510068U, 0x00510110U, 0x005101b8U, 0x00510260U, 0x00510308U, 0x00510458U, 0x00510500U, 0x00510650U, 0x005106f8U, 0x005107a0U, 0x00510848U, 0x005108f0U, 0x00510998U, 0x0050def0U, 0x0050eac0U, 0x0050eeb0U, 0x0050f690U, 0x0050fb28U, 0x0050fe70U, 0x005103b0U, 0x005105a8U, 0x0050d1b0U, 0x0050d258U, 0x00000000U, 0x00000000U, 0x00000000U};
    const auto address = descriptors[index];
    if (!address) return AnimSpec{};
    return AnimSpec{source.string(address + 0), source.read<int>(address + 0x54), source.read<int>(address + 0x58), source.read<int>(address + 0x64), source.read<int>(address + 0x40) != 0, source.read<int>(address + 0x44) != 0, source.read<int>(address + 0x48) != 0, source.read<int>(address + 0x4c) != 0};
});
constexpr std::size_t kMasterCount = 74;
constexpr std::size_t kMusicFirst = 74;
constexpr std::size_t kMusicCount = 8;
constexpr std::size_t kHelpFrame = 82;
constexpr std::size_t kExitFrame = 83;

// Named indices into kAnims.
constexpr std::size_t kIdle = 0;
constexpr std::size_t kStartCenter = 3;
constexpr std::size_t kHit = 4;
constexpr std::size_t kNotHit = 13;
constexpr std::size_t kPickUp = 14;
constexpr std::size_t kRubberball = 15;
constexpr std::size_t kPingpong = 16;
constexpr std::size_t kPingpongBitmap = 23;
constexpr std::size_t kCars = 29;
constexpr std::size_t kCarsBitmap = 32;
constexpr std::size_t kGalleryBitmap = 33;
constexpr std::size_t kGalleryVoice = 38;
constexpr std::size_t kYardShock = 43;
constexpr std::size_t kYardSolan = 44;
constexpr std::size_t kYardDrown = 45;
constexpr std::size_t kYardPump = 46;
constexpr std::size_t kYardWindmill = 47;
constexpr std::size_t kYardBitmap = 48;
constexpr std::size_t kYardBirds = 49;
constexpr std::size_t kPeeLudvig = 50;
constexpr std::size_t kPeeSolan1 = 52;
constexpr std::size_t kPeeSolan2 = 53;
constexpr std::size_t kPeeBitmap = 54;
constexpr std::size_t kPeeBirds = 55;
constexpr std::size_t kHouseClip = 56;
constexpr std::size_t kHouseWindmill = 57;
constexpr std::size_t kHouseBitmap = 58;
constexpr std::size_t kTvroomBoxing = 59;
constexpr std::size_t kTvroomExercise = 60;
constexpr std::size_t kTvroomLaugh = 61;
constexpr std::size_t kTvroomTwist = 62;
constexpr std::size_t kTvroomLudvig = 63;
constexpr std::size_t kTvroomCandles = 64;
constexpr std::size_t kTvroomBitmap = 65;
constexpr std::size_t kOuttentClip = 66;
constexpr std::size_t kOuttentBitmap = 67;
constexpr std::size_t kIntentClip = 68;
constexpr std::size_t kIntentBitmap = 69;
constexpr std::size_t kOutroClip = 70;
constexpr std::size_t kOutroShot = 71;
constexpr std::size_t kOutroWindmill = 72;
constexpr std::size_t kOutroBitmap = 73;
constexpr std::size_t kRubberballMusic = 74;
constexpr std::size_t kCarsMusic = 75;
constexpr std::size_t kGalleryMusic = 76;
constexpr std::size_t kYardMusic = 77;
constexpr std::size_t kPeeMusic = 78;
constexpr std::size_t kHouseMusic = 79;
constexpr std::size_t kTvroomMusic = 80;
constexpr std::size_t kOuttentMusic = 81;

// Chapter sequences 0x00510bc8 (yard), 0x00510bd8 (pee), 0x00510bec
// (tvroom) and 0x00510c00 (cars); 0x00510b90 the idles and 0x00510ba0
// the nine hits.
constexpr std::array<std::size_t, 3> kYardSequence{{kYardShock, kYardSolan, kYardDrown}};
constexpr std::array<std::size_t, 4> kPeeSequence{{kPeeLudvig, 51, kPeeSolan1, kPeeSolan2}};
constexpr std::array<std::size_t, 4> kTvroomSequence{{
    kTvroomLaugh, kTvroomTwist, kTvroomBoxing, kTvroomExercise}};
constexpr std::array<std::size_t, 3> kCarsSequence{{kCars, kCars + 1, kCars + 2}};
constexpr std::array<std::size_t, 3> kIdles{{kIdle, kIdle + 1, kIdle + 2}};
constexpr std::array<std::size_t, 9> kHits{{
    kHit, kHit + 1, kHit + 2, kHit + 3, kHit + 4, kHit + 5, kHit + 6,
    kHit + 7, kHit + 8}};

// The scene records at 0x0050d3b0: id, title key, the anaglyph award bit
// that opens it (-1: always open) and its list items.
struct SceneSpec {
    int id;
    std::string_view title;
    int awardBit;
    std::array<std::string_view, 7> items;
};
constexpr std::array<SceneSpec, 12> kScenes{{
    {0x0f, "SCENE_PINGPONG", -1, {"PINGPONG0", "PINGPONG1", "PINGPONG2",
                                  "PINGPONG3", "PINGPONG4", "PINGPONG5",
                                  "PINGPONG6"}},
    {0x10, "SCENE_RUBBERBALL", -1, {"RUBBERBALL0", "RUBBERBALL1"}},
    {0x12, "SCENE_GALLERY", -1, {"GALLERY0", "GALLERY1", "GALLERY2",
                                 "GALLERY3", "GALLERY4"}},
    {0x11, "SCENE_CARS", -1, {"CARS0", "CARS1", "CARS2"}},
    {0x13, "SCENE_3DJIGSAW", -1, {}},
    {0x01, "SCENE_YARD", 1, {"PLAYALL", "YARD1", "YARD2", "YARD3"}},
    {0x03, "SCENE_PEE", 3, {"PLAYALL", "PEE1", "PEE2", "PEE3"}},
    {0x04, "SCENE_HOUSE", 4, {"SCENE_HOUSE"}},
    {0x05, "SCENE_TVROOM", 5, {"PLAYALL", "TVROOM1", "TVROOM2", "TVROOM3"}},
    {0x08, "SCENE_OUTTENT", 8, {"SCENE_OUTTENT"}},
    {0x09, "SCENE_INTENT", 9, {"SCENE_INTENT"}},
    {0x0e, "SCENE_OUTRO", 14, {"SCENE_OUTRO"}},
}};
constexpr int kRubberballScene = 0x10;

// Controls 0x0047c740 creates, with their 0x116 ids.
constexpr int kHelpControl = 1;
constexpr int kExitControl = 2;
constexpr int kPlayControl = 3;
constexpr int kLoadControl = 4;
constexpr int kStopControl = 5;
constexpr int kPrintControl = 7;
constexpr int kFasterControl = 8;
constexpr int kSlowerControl = 9;
// Hotspot id 3 (the PLAY button) is the Rubberball swing (0x0047d9e0).
constexpr std::uint32_t kSwingHotspot = 3U;

// 0x004a0320 (label) and 0x004a0330 (value) text boxes, 0x004a0340 the list.
constexpr SDL_Rect kLabelBox{717, 566, 788 - 717, 583 - 566};
constexpr SDL_Rect kValueBox{721, 515, 773 - 721, 550 - 515};
constexpr SDL_Rect kListBounds{12, 512, 215 - 12, 587 - 512};
// The list dialog 0x00420430 opens at (0xf0, 10); 0x004b01f8 is its list
// (20,61)-(258,410) and 0x004b0208 its title, both relative.
constexpr SDL_Point kPickerOrigin{0xf0, 10};
constexpr SDL_Rect kPickerList{20, 61, 258 - 20, 410 - 61};
constexpr SDL_Rect kPickerTitle{17, 13, 286 - 17, 40 - 13};
constexpr int kPickerOk = 1000;
constexpr int kPickerCancel = 1001;
// 0x0047d886-0x0047d88e: each picker row is a 238x23 text image.
constexpr int kPickerRowHeight = 0x17;
// Highlight colours are COLORREFs (0x00bbggrr): 0x23234b, which 0x0047c965
// passes to 0x00407d90 for the item list, and the list control's default
// 0x3e5167 (0x004085ef) the picker keeps. The captures show dark red and tan.
constexpr SDL_Color kHighlight{0x4b, 0x23, 0x23, 0xff};
constexpr SDL_Color kPickerHighlight{0x67, 0x51, 0x3e, 0xff};
// Scroll parts: the list's hup (0xdd,0x1fe) and hdown (0xdd,0x239); the
// picker's up (0x10f,0x31) and down (0x110,0x186) relative to the dialog.
// The thumb sits below the up button and travels to 0x21c (list); in the
// picker it is centred in the 15-pixel track at x 0x115.
constexpr SDL_Point kListUp{0xdd, 0x1fe};
constexpr SDL_Point kListDown{0xdd, 0x239};
constexpr int kListThumbLowest = 0x21c;

std::string_view animName(std::size_t index) { return kAnims[index].name; }

} // namespace

bool AnaglyphScene::anaglyphSceneOpen(int scene) {
    // 0x0041b8c0: bit `awardBit` of the anaglyph record (0x402, key 100).
    const int bit = kScenes[static_cast<std::size_t>(scene)].awardBit;
    if (bit < 0) return true;
    std::uint32_t mask = 0U;
    if (const auto profile = viewerProfilePath())
        mask = profileSceneRecordU32(*profile, "anaglyph", 0x402U, 100U)
                   .value_or(0U);
    return (mask & (1U << static_cast<unsigned>(bit))) != 0U;
}

void AnaglyphScene::ensureAnaglyphAnim(std::size_t index) {
    auto &anim = anaglyphViewer->anims[index];
    if (anim.loaded) return;
    anim.loaded = true;
    const auto &spec = kAnims[index];
    try {
        if (spec.smacker) {
            const auto data = activeSceneAnimationData(std::string(spec.name));
            anim.media = spec.video
                ? decodeSmackerStreamed(
                      data, std::string(spec.name),
                      // 0x00413071: +0x48 selects the top-left colour key.
                      spec.keyed ? SmackerKeying::TopLeftRgb565
                                 : SmackerKeying::IndexZeroAndGreen,
                      true, true)
                : decodeSmacker(data, true, true, true);
            if (spec.video && !anim.media.videoFrames.empty()) {
                const auto &frame = anim.media.videoFrames.metadata(0);
                anim.texture.reset(SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
                if (anim.texture)
                    SDL_SetTextureBlendMode(anim.texture.get(),
                                            SDL_BLENDMODE_BLEND);
            }
        } else {
            const auto data =
                viewerAsset("bitmap\\" + std::string(spec.name) + ".bmp");
            anim.bitmap = spec.keyed ? loadGreenKeyedBmp(data) : loadBmp(data);
            anim.texture = makeTexture(renderer, anim.bitmap.get());
            SDL_SetTextureBlendMode(anim.texture.get(), SDL_BLENDMODE_BLEND);
        }
    } catch (const std::exception &error) {
        std::cerr << "Anaglyph asset " << spec.name << ": " << error.what()
                  << '\n';
    }
}

// SCENE_AddAnim 0x00412840: -1,-1 selects the default position.
void AnaglyphScene::addAnaglyphAnim(std::size_t index, int x, int y,
                                    int layer) {
    auto &viewer = *anaglyphViewer;
    auto &anim = viewer.anims[index];
    if (anim.playing) return;  // 0x00412849: only when +0x78 is clear
    ensureAnaglyphAnim(index);
    const auto &spec = kAnims[index];
    traceViewerAdd(spec.name, x, y, layer);
    anim.added = true;
    anim.playing = false;
    anim.heldTime = 0.0;
    anim.x = x == -1 && y == -1 ? spec.x : x;
    anim.y = x == -1 && y == -1 ? spec.y : y;
    anim.layer = layer;
    anim.addOrder = viewer.nextAddOrder++;
    // 0x0047cf20 imposes the viewer's rate on every Smacker player
    // (0x00402f40), so a clip runs at frameRate rather than its own.
    anim.rate = 1.0;
    if (spec.smacker && !anim.media.videoFrames.empty() &&
        anim.media.videoDuration > 0.0 && viewer.frameRate > 0)
        anim.rate = viewer.frameRate * anim.media.videoDuration /
                    static_cast<double>(anim.media.videoFrames.size());
}

void AnaglyphScene::removeAnaglyphAnim(std::size_t index) {
    auto &anim = anaglyphViewer->anims[index];
    if (!anim.added) return;
    traceViewerRemove(animName(index));
    anim.added = false;
    anim.playing = false;
    if (anim.sound != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.sound);
        anim.sound = LiveAudioMixer::kInvalidEffectHandle;
    }
}

double AnaglyphScene::anaglyphMediaTime(const AnaglyphAnim &anim,
                                        double now) const {
    if (!anim.playing) return anim.heldTime;
    return (now - anim.startedAt) * anim.rate;
}

double AnaglyphScene::anaglyphCompletion(const AnaglyphAnim &anim) const {
    return originalSmackerCompletionDuration(anim.media);
}

// 0x004129d0: SmackGoto(0), then run.
void AnaglyphScene::startAnaglyphAnim(std::size_t index, double now) {
    auto &anim = anaglyphViewer->anims[index];
    if (!kAnims[index].smacker || !anim.added) return;
    anim.heldTime = 0.0;
    resumeAnaglyphAnim(index, now);
    // 0x0040c860 posts "started" (0x10f); 0x0047da50 starts the yard pump
    // with Solan's swing.
    auto &viewer = *anaglyphViewer;
    if (index == kYardSolan && viewer.scene >= 0 &&
        kScenes[static_cast<std::size_t>(viewer.scene)].id == 0x01)
        playAnaglyphAnim(kYardPump, now);
}

// 0x00412a20: run on from the held frame.
void AnaglyphScene::resumeAnaglyphAnim(std::size_t index, double now) {
    auto &anim = anaglyphViewer->anims[index];
    if (!kAnims[index].smacker || !anim.added) return;
    anim.playing = true;
    anim.startedAt = now - anim.heldTime / std::max(anim.rate, 1e-6);
    if (anim.sound != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.sound);
        anim.sound = LiveAudioMixer::kInvalidEffectHandle;
    }
    anim.nextLoopAt = now + anim.media.videoDuration / std::max(anim.rate, 1e-6);
    if (!anim.media.audioSamples.empty() && anim.heldTime <= 0.0) {
        const bool music = index >= kMusicFirst && index < kMusicFirst + kMusicCount;
        anim.sound = audioMixer.addEffect(
            anim.media, music ? audioVolumes[2] * sceneAudioGains[2]
                              : audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

// SCENE_StopAnim 0x00412a60: hold the current frame.
void AnaglyphScene::stopAnaglyphAnim(std::size_t index, double now) {
    auto &anim = anaglyphViewer->anims[index];
    traceViewerStop(animName(index));
    if (!kAnims[index].smacker) return;
    if (anim.playing) {
        double time = anaglyphMediaTime(anim, now);
        if (kAnims[index].loop && anim.media.videoDuration > 0.0)
            time = std::fmod(time, anim.media.videoDuration);
        anim.heldTime = time;
    }
    anim.playing = false;
    if (anim.sound != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.sound);
        anim.sound = LiveAudioMixer::kInvalidEffectHandle;
    }
}

// SCENE_PlayAnim 0x00412b90: add at the descriptor's own position and
// layer (0x00412ba7 pushes +0x54, +0x58 and +0x64), then start.
void AnaglyphScene::playAnaglyphAnim(std::size_t index, double now) {
    if (!anaglyphViewer->anims[index].added)
        addAnaglyphAnim(index, kAnims[index].x, kAnims[index].y,
                        kAnims[index].layer);
    startAnaglyphAnim(index, now);
}

// 0x0047ce40: remove every added descriptor of a list.
void AnaglyphScene::removeAnaglyphList(std::span<const std::size_t> list) {
    for (const auto index : list) removeAnaglyphAnim(index);
}

void AnaglyphScene::refreshAnaglyphValue() {
    auto &viewer = *anaglyphViewer;
    char text[32]{};
    if (viewer.scene >= 0 &&
        kScenes[static_cast<std::size_t>(viewer.scene)].id == kRubberballScene) {
        // 0x0047ce80: "%d" hits.
        std::snprintf(text, sizeof(text), "%d", viewer.hits);
    } else {
        // 0x0047cec0: "%3.0f %%" of frameRate * 0.0666667 * 100.
        std::snprintf(text, sizeof(text), "%3.0f %%",
                      viewer.frameRate * 0.06666667 * 100.0);
    }
    viewer.valueText = text;
}

// 0x0047cf20: clamp to 15..30 and rebuild every added clip at the new rate.
void AnaglyphScene::setAnaglyphFrameRate(int frameRate, double now) {
    auto &viewer = *anaglyphViewer;
    frameRate = std::clamp(frameRate, 15, 30);
    if (frameRate == viewer.frameRate) return;
    viewer.frameRate = frameRate;
    refreshAnaglyphValue();
    for (std::size_t index = 0; index < kMasterCount; ++index) {
        auto &anim = viewer.anims[index];
        if (!anim.added || !kAnims[index].smacker) continue;
        const bool wasPlaying = anim.playing;
        double time = anaglyphMediaTime(anim, now);
        if (kAnims[index].loop && anim.media.videoDuration > 0.0)
            time = std::fmod(time, anim.media.videoDuration);
        const int x = anim.x;
        const int y = anim.y;
        const int layer = anim.layer;
        const auto sound = anim.sound;
        anim.sound = LiveAudioMixer::kInvalidEffectHandle;
        removeAnaglyphAnim(index);
        addAnaglyphAnim(index, x, y, layer);
        anim.heldTime = time;
        anim.sound = sound;
        if (wasPlaying) {
            // 0x00412a20 resumes on the frame 0x00403770 restored.
            anim.playing = true;
            anim.startedAt = now - time / std::max(anim.rate, 1e-6);
        }
    }
}

// 0x0047d7d0 (through 0x0047d7c0, which Rubberball skips): stop every
// added descriptor of the master list.
void AnaglyphScene::stopAnaglyphScene(double now) {
    auto &viewer = *anaglyphViewer;
    if (viewer.scene >= 0 &&
        kScenes[static_cast<std::size_t>(viewer.scene)].id == kRubberballScene)
        return;
    for (std::size_t index = 0; index < kMasterCount; ++index)
        if (viewer.anims[index].added) stopAnaglyphAnim(index, now);
}

// 0x0047e000: run every added, stopped descriptor -- resumed while the
// chapter is still under way, restarted once it has ended.
void AnaglyphScene::playAnaglyphScene(double now) {
    auto &viewer = *anaglyphViewer;
    for (std::size_t index = 0; index < kMasterCount; ++index) {
        auto &anim = viewer.anims[index];
        if (!anim.added || anim.playing) continue;
        if (viewer.chapterEnded) startAnaglyphAnim(index, now);
        else resumeAnaglyphAnim(index, now);
    }
    viewer.chapterEnded = false;
}

// 0x0047e060: after "play all" ended, put the first clip back first.
void AnaglyphScene::playAnaglyphSequence(std::span<const std::size_t> list,
                                         double now) {
    auto &viewer = *anaglyphViewer;
    if (viewer.chapterEnded && viewer.item == 0) {
        removeAnaglyphList(list);
        addAnaglyphAnim(list[0], -1, -1, kAnims[list[0]].layer);
    }
    playAnaglyphScene(now);
}

// 0x0047d090: select a list item (chapter, or Rubberball's mode).
void AnaglyphScene::selectAnaglyphItem(int item, double now) {
    auto &viewer = *anaglyphViewer;
    viewer.item = item;
    viewer.chapterEnded = false;
    stopAnaglyphScene(now);
    const int id = kScenes[static_cast<std::size_t>(viewer.scene)].id;
    const auto addDefault = [&](std::size_t index) {
        addAnaglyphAnim(index, -1, -1, kAnims[index].layer);
    };
    switch (id) {
    case 0x01:  // 0x0047d180
        removeAnaglyphList(kYardSequence);
        if (item == 0 || item == 1) addDefault(kYardShock);
        else if (item == 2) {
            addDefault(kYardPump);
            addDefault(kYardSolan);
        } else if (item == 3) addDefault(kYardDrown);
        break;
    case 0x03:  // 0x0047d220
        removeAnaglyphList(kPeeSequence);
        if (item == 0 || item == 1) addDefault(kPeeLudvig);
        else if (item == 2) addDefault(kPeeSolan1);
        else if (item == 3) addDefault(kPeeSolan2);
        break;
    case 0x04:  // 0x0047d2a0
        if (item == 0) addAnaglyphAnim(kHouseClip, -1, -1, 0x19);
        break;
    case 0x05:  // 0x0047d2c0
        removeAnaglyphList(kTvroomSequence);
        if (item >= 0 && item <= 4)
            addDefault(kTvroomSequence[static_cast<std::size_t>(
                std::max(0, item - 1))]);
        break;
    case 0x08:  // 0x0047d370
        if (item == 0) addAnaglyphAnim(kOuttentClip, -1, -1, 0x19);
        break;
    case 0x09:  // 0x0047d390
        if (item == 0) addAnaglyphAnim(kIntentClip, -1, -1, 0x19);
        break;
    case 0x0e:  // 0x0047d3b0
        if (item == 0) {
            addAnaglyphAnim(kOutroClip, -1, -1, 0x19);
            addAnaglyphAnim(kOutroShot, -1, -1, 0x19);
        }
        break;
    case 0x0f: {  // 0x0047d450: pingpong clip at 0x19, its still at -10
        for (std::size_t index = 0; index < kMasterCount; ++index)
            removeAnaglyphAnim(index);
        if (item >= 0 && item <= 6) {
            addAnaglyphAnim(kPingpong + static_cast<std::size_t>(item), -1,
                            -1, 0x19);
            if (item > 0)
                addAnaglyphAnim(kPingpongBitmap +
                                    static_cast<std::size_t>(item - 1),
                                -1, -1, -10);
        }
        break;
    }
    case 0x10:  // 0x0047d580
        for (std::size_t index = kIdle; index <= kPickUp; ++index)
            removeAnaglyphAnim(index);
        if (item == 0 || item == 1)
            playAnaglyphAnim(rubberballNext(0, std::nullopt), now);
        break;
    case 0x11:  // 0x0047d3e0
        removeAnaglyphList(kCarsSequence);
        if (item >= 0 && item <= 2)
            addDefault(kCarsSequence[static_cast<std::size_t>(item)]);
        break;
    case 0x12:  // 0x0047d6d0: the portrait at 0x19, its voice at -10
        for (std::size_t index = 0; index < kMasterCount; ++index)
            removeAnaglyphAnim(index);
        if (item >= 0 && item <= 4) {
            addAnaglyphAnim(kGalleryBitmap + static_cast<std::size_t>(item),
                            -1, -1, 0x19);
            addAnaglyphAnim(kGalleryVoice + static_cast<std::size_t>(item),
                            -1, -1, -10);
        }
        break;
    default:
        break;
    }
}

// 0x0047c9e0: load one scene record.
void AnaglyphScene::loadAnaglyphScene(int scene, double now) {
    auto &viewer = *anaglyphViewer;
    viewer.scene = scene;
    const auto &spec = kScenes[static_cast<std::size_t>(scene)];
    // 0x0047ca00-0x0047ca3c: the list, filled with the localised items.
    viewer.list.items.clear();
    viewer.list.selected = 0;
    viewer.list.top = 0;
    for (const auto key : spec.items) {
        if (key.empty()) break;
        viewer.list.items.push_back(
            language.get("anaglyph", std::string(key), std::string(key)));
    }
    // 0x0047cf10 and 0x0047ce40(music): everything goes.
    for (std::size_t index = 0; index < kMasterCount; ++index)
        removeAnaglyphAnim(index);
    for (std::size_t index = kMusicFirst; index < kMusicFirst + kMusicCount;
         ++index)
        removeAnaglyphAnim(index);
    viewer.labelText = language.get("anaglyph", "FRAMERATE", "FART");
    setAnaglyphFrameRate(15, now);
    refreshAnaglyphValue();
    // 0x0047ca9a-0x0047cab9: STOP's label, and +/- back to frame 0.
    if (auto *stop = viewer.widgets.find(kStopControl))
        stop->label = language.get("anaglyph", "STOP", "STOPP");
    viewer.widgets.setFrame(kFasterControl, 0);
    viewer.widgets.setFrame(kSlowerControl, 0);
    std::optional<std::size_t> music;
    switch (spec.id) {
    case 0x01:
        addAnaglyphAnim(kYardBitmap, -1, -1, -10);
        addAnaglyphAnim(kYardWindmill, -1, -1, -9);
        addAnaglyphAnim(kYardBirds, -1, -1, -10);
        addAnaglyphAnim(kYardMusic, -1, -1, -10);
        music = kYardMusic;
        break;
    case 0x03:
        addAnaglyphAnim(kPeeBitmap, -1, -1, -10);
        addAnaglyphAnim(kPeeBirds, -1, -1, -10);
        addAnaglyphAnim(kPeeMusic, -1, -1, -10);
        music = kPeeMusic;
        break;
    case 0x04:
        addAnaglyphAnim(kHouseBitmap, -1, -1, -10);
        addAnaglyphAnim(kHouseWindmill, -1, -1, -9);
        addAnaglyphAnim(kHouseMusic, -1, -1, -10);
        music = kHouseMusic;
        break;
    case 0x05:
        addAnaglyphAnim(kTvroomLudvig, -1, -1, -9);
        addAnaglyphAnim(kTvroomCandles, -1, -1, -9);
        addAnaglyphAnim(kTvroomBitmap, -1, -1, -10);
        addAnaglyphAnim(kTvroomMusic, -1, -1, -10);
        music = kTvroomMusic;
        break;
    case 0x08:
        addAnaglyphAnim(kOuttentBitmap, -1, -1, -10);
        addAnaglyphAnim(kOuttentMusic, -1, -1, -10);
        music = kOuttentMusic;
        break;
    case 0x09:
        addAnaglyphAnim(kIntentBitmap, -1, -1, -10);
        break;
    case 0x0e:
        addAnaglyphAnim(kOutroBitmap, -1, -1, -10);
        addAnaglyphAnim(kOutroWindmill, -1, -1, -9);
        break;
    case 0x10:
        // 0x0047cb9f-0x0047cbe0: HIT and the hit count replace the rate,
        // STOP becomes HISCORE and +/- are disabled (frame 2).
        viewer.labelText = language.get("anaglyph", "HIT", "TREFF");
        viewer.hits = 0;
        refreshAnaglyphValue();
        if (auto *stop = viewer.widgets.find(kStopControl))
            stop->label = language.get("anaglyph", "HISCORE", "Poengtabell");
        viewer.widgets.setFrame(kFasterControl, 2);
        viewer.widgets.setFrame(kSlowerControl, 2);
        addAnaglyphAnim(kRubberball, -1, -1, -10);
        addAnaglyphAnim(kRubberballMusic, -1, -1, -10);
        music = kRubberballMusic;
        break;
    case 0x11:
        addAnaglyphAnim(kCarsBitmap, -1, -1, -10);
        addAnaglyphAnim(kCarsMusic, -1, -1, -10);
        music = kCarsMusic;
        break;
    case 0x12:
        addAnaglyphAnim(kGalleryMusic, -1, -1, -10);
        music = kGalleryMusic;
        break;
    default:
        break;
    }
    if (music) playAnaglyphAnim(*music, now);
    // 0x0047cd8b-0x0047cd9c: the first item, then its chapter.
    viewer.list.selected = 0;
    selectAnaglyphItem(0, now);
}

std::size_t AnaglyphScene::rubberballNext(int state,
                                          std::optional<int> direction) {
    auto &viewer = *anaglyphViewer;
    // 0x0047d5b0.
    switch (state) {
    case 0:
        viewer.startClicks = 0;
        return kIdles[nextNoRepeatOption(viewer.idleBag, kIdles.size())];
    case 1:
        return kStartCenter;
    case 2: {
        std::size_t hit;
        if (direction) {
            hit = static_cast<std::size_t>(*direction * 3 + particleRandom(3));
        } else {
            hit = nextNoRepeatOption(viewer.hitBag, kHits.size());
        }
        viewer.startClicks = 0;
        return kHits[std::min<std::size_t>(hit, kHits.size() - 1U)];
    }
    case 3:
        return kNotHit;
    default:
        return kPickUp;
    }
}

// 0x0047db40: Rubberball's state machine runs on each finished clip.
void AnaglyphScene::rubberballFinished(std::size_t index, double now) {
    auto &viewer = *anaglyphViewer;
    int state;
    std::optional<int> direction;
    if (index >= kIdle && index < kIdle + 3) state = 0;
    else if (index >= kHit && index < kHit + 9) {
        state = 2;
        direction = static_cast<int>((index - kHit) / 3U);
    } else if (index == kStartCenter) {
        state = 1;
        direction = 0;
    } else if (index == kNotHit) state = 3;
    else if (index == kPickUp) state = 4;
    else return;
    int next = state;
    std::optional<int> nextDirection;
    switch (state) {
    case 0:
        // 0x0047dc20: a click on START during the idle starts a rally;
        // the watched one lasts rand(15..50) hits (0x0040c550).
        if (viewer.startClicks > 0) {
            next = 1;
            viewer.watchedTarget = 15 + particleRandom(50 - 15);
            viewer.hits = 0;
            refreshAnaglyphValue();
        }
        break;
    case 1:
        next = 2;
        nextDirection = direction;
        break;
    case 2: {
        // 0x0047dc30.
        bool kept;
        if (viewer.item == 0) {
            kept = viewer.hits < viewer.watchedTarget;
        } else {
            int window = (100 - viewer.hits) * 20;
            if (window < 400) window = (0xa0 - viewer.hits) * 5;
            if (window < 200) window = 200;
            kept = viewer.startClicks > 0 &&
                   (now - viewer.firstClickAt) * 1000.0 <= window;
        }
        if (kept) {
            ++viewer.hits;
            refreshAnaglyphValue();
        } else {
            next = 3;
        }
        break;
    }
    case 3:
        next = 4;
        // 0x0047dca0: a game played by the user enters the score table,
        // 0x0041ee50(0, hits, NULL, 0, 0x006d4888, 0x006d26b0): anaglyph's
        // record 0x401 key 0, points, no medal and no dialog.
        if (viewer.item == 1) {
            viewer.lastScoreRow = submitSceneScore(
                static_cast<std::uint32_t>(std::max(0, viewer.hits)), 0U, 0U);
            ++viewer.gamesPlayed;
        }
        break;
    default:
        next = 0;
        break;
    }
    removeAnaglyphAnim(index);
    playAnaglyphAnim(rubberballNext(next, nextDirection), now);
}

void AnaglyphScene::anaglyphSequenceFinished(std::span<const std::size_t> list,
                                             std::size_t index, double now) {
    auto &viewer = *anaglyphViewer;
    const auto found = std::find(list.begin(), list.end(), index);
    if (found == list.end()) return;
    // 0x0047dde0 ("play all"): the next clip of the chain, else the end;
    // 0x0047de40 (one chapter): the end.
    if (viewer.item == 0) {
        const auto next = std::next(found);
        if (next != list.end()) {
            removeAnaglyphAnim(index);
            playAnaglyphAnim(*next, now);
            return;
        }
    }
    stopAnaglyphScene(now);
    viewer.chapterEnded = true;
}

// Animation-finished event 0x110, 0x0047da70.
void AnaglyphScene::anaglyphFinished(std::size_t index, double now) {
    auto &viewer = *anaglyphViewer;
    auto &anim = viewer.anims[index];
    anim.playing = false;
    anim.heldTime = anim.media.videoDuration;
    anim.sound = LiveAudioMixer::kInvalidEffectHandle;
    traceViewerFinished(animName(index));
    const int id = kScenes[static_cast<std::size_t>(viewer.scene)].id;
    switch (id) {
    case 0x01:
        if (index == kYardPump) removeAnaglyphAnim(kYardPump);
        anaglyphSequenceFinished(kYardSequence, index, now);
        break;
    case 0x03:
        anaglyphSequenceFinished(kPeeSequence, index, now);
        break;
    case 0x05:
        anaglyphSequenceFinished(kTvroomSequence, index, now);
        break;
    case 0x0e:
        if (index == kOutroShot) break;
        stopAnaglyphScene(now);
        viewer.chapterEnded = true;
        break;
    case 0x04: case 0x08: case 0x09: case 0x0f: case 0x11: case 0x12:
        stopAnaglyphScene(now);
        viewer.chapterEnded = true;
        break;
    case 0x10:
        rubberballFinished(index, now);
        break;
    default:
        break;
    }
}

void AnaglyphScene::openAnaglyphPicker() {
    auto &viewer = *anaglyphViewer;
    AnaglyphPicker picker;
    picker.title = language.get("anaglyph", "LOADTITLE", "Åpne 3D-aktivitet");
    picker.list.bounds = {kPickerOrigin.x + kPickerList.x,
                          kPickerOrigin.y + kPickerList.y, kPickerList.w,
                          kPickerList.h};
    picker.list.rowHeight = kPickerRowHeight;
    picker.list.visibleRows = std::min(32, kPickerList.h / kPickerRowHeight);
    for (std::size_t scene = 0; scene < kScenes.size(); ++scene) {
        picker.list.items.push_back(language.get(
            "anaglyph", std::string(kScenes[scene].title),
            std::string(kScenes[scene].title)));
        // 0x0047d8b0-0x0047d8c4: closed scenes use the grey font.
        picker.list.grey.push_back(!anaglyphSceneOpen(static_cast<int>(scene)));
    }
    const auto dialogAsset = [&](std::string_view name) {
        return viewerCommonAsset("dialogue\\listdlg\\" + std::string(name) +
                                 ".bmp");
    };
    const std::array<std::string_view, 3> scrollParts{{"up_0", "down_0", "thumb"}};
    for (std::size_t part = 0; part < scrollParts.size(); ++part) {
        picker.scroll[part] = loadGreenKeyedBmp(dialogAsset(scrollParts[part]));
        picker.scrollTextures[part] =
            makeTexture(renderer, picker.scroll[part].get());
        SDL_SetTextureBlendMode(picker.scrollTextures[part].get(),
                                SDL_BLENDMODE_BLEND);
    }
    picker.backdrop = loadGreenKeyedBmp(dialogAsset("backdrop"));
    picker.backdropTexture = makeTexture(renderer, picker.backdrop.get());
    SDL_SetTextureBlendMode(picker.backdropTexture.get(), SDL_BLENDMODE_BLEND);
    // 0x00420550: Cancel (0x3e9) at 0xd7 and OK (0x3e8) at 0x7e when the
    // dialog has a Cancel, both at y 0x1ab.
    auto &cancel = picker.widgets.add(
        renderer, kPickerCancel, kPickerOrigin.x + 0xd7, kPickerOrigin.y + 0x1ab,
        dialogAsset("button_0"), dialogAsset("button_1"));
    cancel.label = language.get("interfaceh", "CANCEL", "Avbryt");
    auto &ok = picker.widgets.add(
        renderer, kPickerOk, kPickerOrigin.x + 0x7e, kPickerOrigin.y + 0x1ab,
        dialogAsset("button_0"), dialogAsset("button_1"));
    ok.label = language.get("interfaceh", "OK", "OK");
    // 0x00407290: the backdrop is a descriptor added at the dialog's
    // origin on layer 100.
    traceViewerAdd("../../common/dialogue/listdlg/backdrop", kPickerOrigin.x,
                   kPickerOrigin.y, 100);
    viewer.picker = std::move(picker);
}

// 0x0047d800, after 0x00420430 returns.
void AnaglyphScene::closeAnaglyphPicker(int result, double now) {
    auto &viewer = *anaglyphViewer;
    viewer.picker.reset();
    traceViewerRemove("../../common/dialogue/listdlg/backdrop");
    if (viewer.sceneReadyPending) {
        viewer.sceneReadyPending = false;
        sceneReadyAfterPresent = activeScene;
    }
    if (result < 0) return;
    const auto &spec = kScenes[static_cast<std::size_t>(result)];
    if (!anaglyphSceneOpen(result)) {
        showMessageWithTitle(
            language.get("anaglyph", "NOTAVAILABLE", "Ikke tilgjengelig"),
            language.get("anaglyph", "NOTAVAILABLEDESC", ""));
        return;
    }
    if (spec.id == 0x13) {
        // 0x0047d849: the 3D jigsaw is the jigsaw scene with mode 9.
        if (!navigationHistory.empty()) navigationHistory.pop_back();
        pendingSceneEntryMode = 9;
        pendingScene = std::pair{std::string("jigsaw"), std::string("jigsaw")};
        return;
    }
    loadAnaglyphScene(result, now);
}

// Control event 0x116, 0x0047de90.
void AnaglyphScene::anaglyphControl(int id, double now) {
    traceViewerControl("anaglyph", id);
    auto &viewer = *anaglyphViewer;
    const int scene = kScenes[static_cast<std::size_t>(viewer.scene)].id;
    switch (id) {
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        // 0x0040cc30: the registry parent, sceneindex with mode 0.
        leaveToRegistryParent();
        break;
    case kPlayControl:
        if (scene == 0x01) playAnaglyphSequence(kYardSequence, now);
        else if (scene == 0x03) playAnaglyphSequence(kPeeSequence, now);
        else if (scene == 0x05) playAnaglyphSequence(kTvroomSequence, now);
        else if (scene != kRubberballScene) playAnaglyphScene(now);
        break;
    case kLoadControl:
        openAnaglyphPicker();
        break;
    case kStopControl:
        stopAnaglyphScene(now);
        if (scene == kRubberballScene) {
            // 0x0047df45-0x0047df78: the Rubberball score table through
            // 0x0041f640, boxing the last game's row once one was played.
            showScoreTable(activeScene,
                           language.get("anaglyph", "SCENE_RUBBERBALL", ""),
                           0U, 0U,
                           viewer.gamesPlayed > 0 ? viewer.lastScoreRow
                                                  : std::nullopt);
        }
        break;
    case kPrintControl: {
        // 0x0047e0a0: confirm, then hand the composed picture to the
        // activity page. UNCERTAIN: the native activity page does not take
        // the picture yet, so only the confirmation is shown.
        showMessageWithTitle(
            language.get("anaglyph", "ACTIVITY_TITLE", "Aktivitetssenter"),
            language.get("anaglyph", "ACTIVITY_TEXT", ""));
        break;
    }
    case kFasterControl:
        if (scene != kRubberballScene)
            setAnaglyphFrameRate(viewer.frameRate + 1, now);
        break;
    case kSlowerControl:
        if (scene != kRubberballScene)
            setAnaglyphFrameRate(viewer.frameRate - 1, now);
        break;
    default:
        break;
    }
}

// Scene initialisation 0x0047c710 / 0x0047c740.
AnaglyphScene::AnaglyphViewer AnaglyphScene::loadAnaglyphViewer() {
    AnaglyphViewer viewer;
    viewer.anims.resize(kAnims.size());
    const auto art = [&](std::string_view name) {
        return viewerAsset("bitmap\\" + std::string(name) + ".bmp");
    };
    viewer.widgets.add(renderer, kHelpControl, 0, 0, art("help_0"),
                       art("help_1"));
    viewer.widgets.add(renderer, kExitControl, 0x2d8, 0, art("exit_0"),
                       art("exit_1"));
    auto &load = viewer.widgets.add(renderer, kLoadControl, 0xf2, 0x1ff,
                                    art("buttonlong_0"), art("buttonlong_1"));
    load.label = language.get("anaglyph", "LOAD", "ÅPNE 3D-AKTIVITET");
    auto &play = viewer.widgets.add(renderer, kPlayControl, 0xf4, 0x229,
                                    art("button_0"), art("button_1"));
    play.label = language.get("anaglyph", "PLAY", "START");
    auto &stop = viewer.widgets.add(renderer, kStopControl, 0x19b, 0x229,
                                    art("button_0"), art("button_1"));
    stop.label = language.get("anaglyph", "STOP", "STOPP");
    auto &print = viewer.widgets.add(renderer, kPrintControl, 0x242, 0x229,
                                     art("button_0"), art("button_1"));
    print.label = language.get("anaglyph", "PRINT", "SKRIV UT");
    viewer.widgets.add(renderer, kFasterControl, 0x305, 0x203, {},
                       art("sizeinc_1"), art("sizeinc_2"));
    viewer.widgets.add(renderer, kSlowerControl, 0x305, 0x216, {},
                       art("sizedec_1"), art("sizedec_2"));
    const std::array<std::string_view, 3> scrollParts{{"hup_0", "hdown_0",
                                                       "scrolltab"}};
    for (std::size_t part = 0; part < scrollParts.size(); ++part) {
        viewer.scroll[part] = loadGreenKeyedBmp(art(scrollParts[part]));
        viewer.scrollTextures[part] =
            makeTexture(renderer, viewer.scroll[part].get());
        SDL_SetTextureBlendMode(viewer.scrollTextures[part].get(),
                                SDL_BLENDMODE_BLEND);
    }
    viewer.pickerFont = loadBitmapFont(
        renderer, viewerCommonAsset("fonts\\Amerigo BT_12_.bmp"));
    viewer.pickerGreyFont = loadBitmapFont(
        renderer, viewerCommonAsset("fonts\\Amerigo BT_12_grey.bmp"));
    viewer.list.bounds = kListBounds;
    viewer.list.rowHeight = std::max(1, dialogFont.height);
    viewer.list.visibleRows =
        std::clamp(kListBounds.h / viewer.list.rowHeight, 1, 32);
    std::cout << "Loaded executable-derived 3D viewer: 12 scene records from "
                 "0x0050d3b0, chapter lists, frame rate 15..30, Rubberball\n";
    return viewer;
}

void AnaglyphScene::leaveAnaglyphScene() {
    if (!anaglyphViewer) return;
    auto &viewer = *anaglyphViewer;
    for (auto &anim : viewer.anims) {
        if (anim.sound != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(anim.sound);
            anim.sound = LiveAudioMixer::kInvalidEffectHandle;
        }
    }
    // 0x0047d950: the loaded record's index goes to key 0 (type 0x402).
    if (viewer.scene >= 0)
        if (const auto profile = viewerProfilePath())
            setProfileSceneRecordU32(*profile, "anaglyph", 0x402U, 0U,
                                     static_cast<std::uint32_t>(viewer.scene));
}

void AnaglyphScene::updateAnaglyph(double elapsed) {
    if (!anaglyphViewer) return;
    auto &viewer = *anaglyphViewer;
    if (!viewer.entered) {
        viewer.entered = true;
        // The picker runs its modal loop inside the initialiser
        // (0x0047c736 -> 0x0047d800 -> 0x00420430), so the scene change
        // only returns -- and the scene is ready -- once it closes.
        viewer.sceneReadyPending = sceneReadyAfterPresent.has_value();
        sceneReadyAfterPresent.reset();
        // 0x0047c8b8-0x0047c8d8: the frames above help and exit.
        addAnaglyphAnim(kHelpFrame, -1, -1, 0x1d);
        addAnaglyphAnim(kExitFrame, -1, -1, 0x1d);
        // 0x0047c71e: the first record, then 0x0047d800 opens the picker.
        loadAnaglyphScene(0, elapsed);
        openAnaglyphPicker();
    }
    if (viewer.picker) return;  // the modal loop holds the scene's events
    for (std::size_t index = 0; index < viewer.anims.size(); ++index) {
        auto &anim = viewer.anims[index];
        if (!anim.playing) continue;
        const double rate = std::max(anim.rate, 1e-6);
        if (kAnims[index].loop) {
            if (anim.media.videoDuration > 0.0 && elapsed >= anim.nextLoopAt) {
                anim.nextLoopAt += anim.media.videoDuration / rate;
                if (!anim.media.audioSamples.empty()) {
                    if (anim.sound != LiveAudioMixer::kInvalidEffectHandle)
                        audioMixer.removeEffect(anim.sound);
                    const bool music = index >= kMusicFirst &&
                                       index < kMusicFirst + kMusicCount;
                    anim.sound = audioMixer.addEffect(
                        anim.media, music ? audioVolumes[2] * sceneAudioGains[2]
                                          : audioVolumes[0] * sceneAudioGains[0]);
                }
            }
            continue;
        }
        const double finishAt = anim.startedAt + anaglyphCompletion(anim) / rate;
        if (elapsed >= finishAt) anaglyphFinished(index, finishAt);
    }
}

void AnaglyphScene::renderAnaglyph(double elapsed) {
    if (!anaglyphViewer) return;
    auto &viewer = *anaglyphViewer;
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < viewer.anims.size(); ++index)
        if (viewer.anims[index].added && viewer.anims[index].texture)
            order.push_back(index);
    // 0x0047e220 sorts by runtime layer.
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        const auto &left = viewer.anims[a];
        const auto &right = viewer.anims[b];
        return left.layer != right.layer ? left.layer < right.layer
                                         : left.addOrder < right.addOrder;
    });
    for (const auto index : order) {
        auto &anim = viewer.anims[index];
        if (anim.bitmap) {
            SDL_Rect destination{anim.x, anim.y, anim.bitmap->w, anim.bitmap->h};
            SDL_RenderCopy(renderer, anim.texture.get(), nullptr, &destination);
            continue;
        }
        if (anim.media.videoFrames.empty()) continue;
        double time = anaglyphMediaTime(anim, elapsed);
        const std::size_t frameIndex =
            frameIndexAt(anim.media, time, kAnims[index].loop);
        const auto &frame = anim.media.videoFrames[frameIndex];
        uploadVideoFrame(anim.texture.get(), frame);
        SDL_Rect destination{anim.x, anim.y, frame.width, frame.height};
        SDL_RenderCopy(renderer, anim.texture.get(), nullptr, &destination);
    }
    const auto drawList = [&](const AnaglyphList &list, const BitmapFont &font,
                              const BitmapFont *greyFont, SDL_Color highlight) {
        const int last = std::min<int>(static_cast<int>(list.items.size()),
                                       list.top + list.visibleRows);
        for (int row = list.top; row < last; ++row) {
            const int y = list.bounds.y + (row - list.top) * list.rowHeight;
            if (row == list.selected) {
                SDL_SetRenderDrawColor(renderer, highlight.r, highlight.g,
                                       highlight.b, highlight.a);
                SDL_Rect bar{list.bounds.x, y, list.bounds.w, list.rowHeight};
                SDL_RenderFillRect(renderer, &bar);
            }
            const bool grey = static_cast<std::size_t>(row) < list.grey.size() &&
                              list.grey[static_cast<std::size_t>(row)];
            drawText(renderer, grey && greyFont ? *greyFont : font,
                     list.items[static_cast<std::size_t>(row)], list.bounds.x, y);
        }
    };
    drawList(viewer.list, dialogFont, nullptr, kHighlight);
    const auto drawPart = [&](const SurfacePtr &surface,
                              const TexturePtr &texture, int x, int y) {
        if (!surface || !texture) return;
        SDL_Rect destination{x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
    };
    drawPart(viewer.scroll[0], viewer.scrollTextures[0], kListUp.x, kListUp.y);
    drawPart(viewer.scroll[1], viewer.scrollTextures[1], kListDown.x,
             kListDown.y);
    if (viewer.scroll[0]) {
        const int highest = kListUp.y + viewer.scroll[0]->h;
        const int range = std::max(
            0, static_cast<int>(viewer.list.items.size()) -
                   viewer.list.visibleRows);
        const int thumbY = range > 0
            ? highest + (kListThumbLowest - highest) * viewer.list.top / range
            : highest;
        drawPart(viewer.scroll[2], viewer.scrollTextures[2], kListUp.x,
                 thumbY);
    }
    drawCenteredText(renderer, dialogFont, viewer.labelText, kLabelBox);
    drawCenteredText(renderer, dialogFont, viewer.valueText, kValueBox);
    const auto hovered = viewer.widgets.eventIdAt(
        mouseX, mouseY, viewerStaticHotspotAt(mouseX, mouseY));
    viewer.widgets.render(renderer, mouseX, mouseY, hovered,
                          [&](const ViewerWidget &widget, const SDL_Rect &rect) {
                              drawCenteredText(renderer, dialogFont,
                                               widget.label, rect);
                          });
    if (viewer.picker) {
        auto &picker = *viewer.picker;
        // The dialog greys the scene beneath it like every modal.
        if (modalMeshTexture)
            SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr, nullptr);
        SDL_Rect destination{kPickerOrigin.x, kPickerOrigin.y,
                             picker.backdrop->w, picker.backdrop->h};
        SDL_RenderCopy(renderer, picker.backdropTexture.get(), nullptr,
                       &destination);
        drawCenteredText(renderer, calendarFont, picker.title,
                         SDL_Rect{kPickerOrigin.x + kPickerTitle.x,
                                  kPickerOrigin.y + kPickerTitle.y,
                                  kPickerTitle.w, kPickerTitle.h});
        drawList(picker.list, viewer.pickerFont, &viewer.pickerGreyFont,
                 kPickerHighlight);
        const int upX = kPickerOrigin.x + 0x10f;
        const int upY = kPickerOrigin.y + 0x31;
        drawPart(picker.scroll[0], picker.scrollTextures[0], upX, upY);
        drawPart(picker.scroll[1], picker.scrollTextures[1],
                 kPickerOrigin.x + 0x110, kPickerOrigin.y + 0x186);
        if (picker.scroll[0] && picker.scroll[2])
            drawPart(picker.scroll[2], picker.scrollTextures[2],
                     kPickerOrigin.x + 0x115 + (0xf - picker.scroll[2]->w) / 2,
                     upY + picker.scroll[0]->h);
        picker.widgets.render(
            renderer, mouseX, mouseY,
            picker.widgets.eventIdAt(mouseX, mouseY, 0U),
            [&](const ViewerWidget &widget, const SDL_Rect &rect) {
                drawCenteredText(renderer, dialogFont, widget.label, rect);
            });
    }
}

bool AnaglyphScene::beginAnaglyphInteraction(int x, int y) {
    if (!anaglyphViewer) return false;
    auto &viewer = *anaglyphViewer;
    const double now = sceneElapsedNow();
    if (viewer.picker) {
        auto &picker = *viewer.picker;
        // 0x004209f0: buttons, then the list row under the pointer.
        picker.widgets.press(x, y, 0U);
        const SDL_Point point{x, y};
        if (SDL_PointInRect(&point, &picker.list.bounds)) {
            const int row = (y - picker.list.bounds.y) / picker.list.rowHeight +
                            picker.list.top;
            if (row >= 0 && row < static_cast<int>(picker.list.items.size()))
                picker.list.selected = row;
        }
        return true;
    }
    const std::uint32_t staticHotspot = viewerStaticHotspotAt(x, y);
    const std::uint32_t eventId = viewer.widgets.eventIdAt(x, y, staticHotspot);
    // Hotspot event 0x105 (0x0047d9e0): id 3 counts a Rubberball swing;
    // any other id -- 0 included, since the list has no hotspot of its own
    // in hotspots.bmp -- tries the list.
    traceViewerControl("anaglyph", static_cast<int>(eventId));
    if (eventId == kSwingHotspot) {
        if (viewer.startClicks == 0) viewer.firstClickAt = now;
        ++viewer.startClicks;
    } else {
        const SDL_Point point{x, y};
        if (SDL_PointInRect(&point, &viewer.list.bounds)) {
            const int row = (y - viewer.list.bounds.y) / viewer.list.rowHeight +
                            viewer.list.top;
            if (row >= 0 && row < static_cast<int>(viewer.list.items.size())) {
                viewer.list.selected = row;
                if (row != viewer.item) selectAnaglyphItem(row, now);
            }
        }
    }
    if (const auto id = viewer.widgets.press(x, y, staticHotspot))
        anaglyphControl(*id, now);
    return true;
}

bool AnaglyphScene::endAnaglyphInteraction(int x, int y) {
    if (!anaglyphViewer) return false;
    auto &viewer = *anaglyphViewer;
    const double now = sceneElapsedNow();
    if (viewer.picker) {
        // 0x00420920: OK returns the selection, Cancel -1.
        if (const auto id = viewer.picker->widgets.release(x, y, 0U)) {
            const int result =
                *id == kPickerOk ? viewer.picker->list.selected : -1;
            closeAnaglyphPicker(result, now);
        }
        return true;
    }
    if (const auto id =
            viewer.widgets.release(x, y, viewerStaticHotspotAt(x, y)))
        anaglyphControl(*id, now);
    return true;
}

bool AnaglyphScene::handleAnaglyphKey(SDL_Keycode key) {
    if (!anaglyphViewer) return false;
    auto &viewer = *anaglyphViewer;
    const double now = sceneElapsedNow();
    if (viewer.picker) {
        // 0x00420a30: Enter is OK, Esc Cancel, the rest moves the list.
        auto &list = viewer.picker->list;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            closeAnaglyphPicker(list.selected, now);
        } else if (key == SDLK_ESCAPE) {
            closeAnaglyphPicker(-1, now);
        } else if (key == SDLK_UP) {
            list.selected = std::max(0, list.selected - 1);
        } else if (key == SDLK_DOWN) {
            list.selected = std::min(static_cast<int>(list.items.size()) - 1,
                                     list.selected + 1);
        } else if (key == SDLK_HOME) {
            list.selected = 0;
        } else if (key == SDLK_END) {
            list.selected = static_cast<int>(list.items.size()) - 1;
        }
        return true;
    }
    if (key == SDLK_ESCAPE) {
        // No key handler of its own: the shared 0x0040d300 sends Esc to
        // 0x0040cc30, the registry parent.
        leaveToRegistryParent();
        return true;
    }
    return false;
}

bool AnaglyphScene::executeAnaglyphDeveloperCommand(std::string_view command) {
    if (!anaglyphViewer) return false;
    auto &viewer = *anaglyphViewer;
    const double now = sceneElapsedNow();
    if (command == "cancel" && viewer.picker) {
        closeAnaglyphPicker(-1, now);
    } else if (command.rfind("load:", 0) == 0) {
        const int scene = std::stoi(std::string(command.substr(5)));
        if (scene < 0 || scene >= static_cast<int>(kScenes.size())) return false;
        if (!viewer.picker) openAnaglyphPicker();
        viewer.picker->list.selected = scene;
        closeAnaglyphPicker(scene, now);
    } else if (command.rfind("item:", 0) == 0) {
        if (viewer.picker) return false;
        const int item = std::stoi(std::string(command.substr(5)));
        if (item < 0 || item >= static_cast<int>(viewer.list.items.size()))
            return false;
        viewer.list.selected = item;
        if (item != viewer.item) selectAnaglyphItem(item, now);
    } else if (command == "play" && !viewer.picker) {
        anaglyphControl(kPlayControl, now);
    } else if (command == "stop" && !viewer.picker) {
        anaglyphControl(kStopControl, now);
    } else if (command == "faster" && !viewer.picker) {
        anaglyphControl(kFasterControl, now);
    } else if (command == "slower" && !viewer.picker) {
        anaglyphControl(kSlowerControl, now);
    } else {
        return false;
    }
    return true;
}

} // namespace fgp
