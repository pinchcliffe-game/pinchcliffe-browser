#include "game/app_mountain.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// 640x480 logical display (0x004442bf -> 0x004017f0(0x280, 0x1e0)).
constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr double kWindowScale = 800.0 / 640.0;

// g_6782b8 has one writer, 0x00442ea5 (300); every speed is (300/10)*f.
constexpr float kUnit = 300.0F / 10.0F;
// g_678448 = 750 (0x00441031); 0x004456a2 reads it as 750/1000.
constexpr float kGround = 750.0F / 1000.0F;
constexpr float kLevelAngle = 4.712389F;   // 0x0049eb38
constexpr float kPi = 3.14159274F;         // 0x0049d620

// Control ids (0x00448228 switch).
constexpr int kControlEasy = 0x6f;
constexpr int kControlMedium = 0x70;
constexpr int kControlHard = 0x71;
constexpr int kControlStart = 0x72;
constexpr int kControlUp = 0xc9;
constexpr int kControlDown = 0xca;

// Player hitbox templates {x0,y0,x1,y1}: riding 0x004dbe70, ducking
// 0x004dbec0, airborne 0x004dbf10.
const auto kRideHitbox = originalArray<std::array<float, 4>, 5>(0x004dbe70U);
const auto kDuckHitbox = originalArray<std::array<float, 4>, 5>(0x004dbec0U);
const auto kAirHitbox = originalArray<std::array<float, 4>, 5>(0x004dbf10U);
// Moving-object outlines: milk truck 0x004dbf60, walker 0x004dbfc0.
const auto kTruckOutline = originalArray<float, 24>(0x004dbf60U);
const auto kWalkerOutline = originalArray<float, 24>(0x004dbfc0U);

// Pickup type table 0x004dc668 (stride 0x168): name, score +0x110,
// persisted flag +0x118, slot item +0x11c, sound +0x120. Every entry has
// 32 frames of 32x32 cells at one frame per 66 ms tick.
struct PickupType {
    const char *name;
    int score;
    int flag;
    int slot;
    const char *sound;
};
const auto kPickups = originalArray<PickupType, 7>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004dc668U + static_cast<std::uint32_t>(index * 360);
    return PickupType{source.string(address + 8).data(), source.read<int>(address + 0x110),
        source.read<int>(address + 0x118), source.read<int>(address + 0x11c), source.string(address + 0x120).data()};
});
// 0x004dc630: slot icon names.
const auto kSlotIcons = originalArray<const char *, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004dc630U + static_cast<std::uint32_t>(index * 4);
    return source.string(source.read<std::uint32_t>(address)).data();
});
// 0x004dc024: difficulty score multipliers.
const auto kDifficultyScale = originalArray<float, 3>(0x004dc024U);
// Default descriptor positions: solanhead 0x004dc240.., slot icons
// 0x004dc438...
const auto kHeads = originalArray<SDL_Point, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004dc294U + static_cast<std::uint32_t>(index * 168);
    return source.read<SDL_Point>(address);
});
const auto kSlotIconPositions = originalArray<SDL_Point, 3>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004dc48cU + static_cast<std::uint32_t>(index * 168);
    return source.read<SDL_Point>(address);
});

int ftol(double value) {
    // 0x0048c214 truncates toward zero.
    return static_cast<int>(value);
}

std::string lower(std::string text) {
    for (char &character : text) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

// The display runs at 16 bits per pixel; colour keys compare RGB565 words
// (0x004471a3, 0x0046f800).
std::uint16_t rgb565(std::uint32_t argb) {
    const std::uint32_t r = (argb >> 16) & 0xffU;
    const std::uint32_t g = (argb >> 8) & 0xffU;
    const std::uint32_t b = argb & 0xffU;
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) |
                                      (b >> 3));
}

SurfacePtr argbSurface(std::span<const std::uint8_t> data) {
    SurfacePtr loaded = loadBmp(data);
    SurfacePtr converted(SDL_ConvertSurfaceFormat(
        loaded.get(), SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) throw std::runtime_error(SDL_GetError());
    return converted;
}

std::uint32_t argbAt(SDL_Surface *surface, int x, int y) {
    const auto *row = static_cast<const std::uint8_t *>(surface->pixels) +
                      static_cast<std::ptrdiff_t>(y) * surface->pitch;
    return reinterpret_cast<const std::uint32_t *>(row)[x] | 0xff000000U;
}

// A copy of `surface` whose pixels matching `key` (as RGB565) are clear.
TexturePtr keyedTexture(SDL_Renderer *renderer, SDL_Surface *surface,
                        std::uint16_t key) {
    SurfacePtr copy(SDL_ConvertSurfaceFormat(surface,
                                             SDL_PIXELFORMAT_ARGB8888, 0));
    if (!copy) throw std::runtime_error(SDL_GetError());
    for (int y = 0; y < copy->h; ++y) {
        auto *row = reinterpret_cast<std::uint32_t *>(
            static_cast<std::uint8_t *>(copy->pixels) +
            static_cast<std::ptrdiff_t>(y) * copy->pitch);
        for (int x = 0; x < copy->w; ++x) {
            row[x] = rgb565(row[x]) == key ? 0U : (row[x] | 0xff000000U);
        }
    }
    TexturePtr texture = makeTexture(renderer, copy.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

SDL_Rect windowRect(int x, int y, int w, int h) {
    return {static_cast<int>(std::lround(x * kWindowScale)),
            static_cast<int>(std::lround(y * kWindowScale)),
            static_cast<int>(std::lround(w * kWindowScale)),
            static_cast<int>(std::lround(h * kWindowScale))};
}

// 0x004465de: 2 left of the segment, 1 within it, 3 right of it.
int classify(const std::vector<MountainScene::MountainNode> &nodes,
             const MountainScene::MountainEdge &edge, float x) {
    if (x < nodes[static_cast<std::size_t>(edge.from)].x) return 2;
    if (x <= nodes[static_cast<std::size_t>(edge.to)].x) return 1;
    return 3;
}

// 0x00446500.
bool crossPositive(float ax, float ay, float bx, float by) {
    return ax * by - ay * bx > 0.0F;
}

// 0x00443a33.
float segmentY(const std::vector<MountainScene::MountainNode> &nodes,
               const MountainScene::MountainEdge &edge, float x) {
    const auto &a = nodes[static_cast<std::size_t>(edge.from)];
    const auto &b = nodes[static_cast<std::size_t>(edge.to)];
    return (b.y - a.y) * ((x - a.x) / (b.x - a.x)) + a.y;
}

// 0x004467d5.
void rotate(float out[2], const float in[2], float c, float s) {
    out[0] = c * in[0] - s * in[1];
    out[1] = s * in[0] + c * in[1];
}

// 0x0044507e: the segment's truncated first point inside the rectangle.
bool pointInRect(const int rect[4], const float segment[4]) {
    const int x = ftol(segment[0]);
    const int y = ftol(segment[1]);
    return !(x < rect[0] || x > rect[2] || y < rect[1] || y > rect[3]);
}

// 0x00444d33, including the bottom-edge test at 0x00445027 that compares
// the crossing x with the rectangle's top and bottom.
bool segmentHitsRect(const float s[4], const int r[4]) {
    const int x0 = ftol(s[0]);
    const int y0 = ftol(s[1]);
    const int x1 = ftol(s[2]);
    const int y1 = ftol(s[3]);
    if (x0 == x1) {
        if (x0 < r[0] || x0 > r[2]) return false;
        if (y0 < r[1] && y1 < r[1]) return false;
        if (y0 > r[3] && y1 > r[3]) return false;
        return true;
    }
    if (y0 == y1) {
        if (y0 < r[1] || y0 > r[3]) return false;
        if (x0 < r[0] && x1 < r[0]) return false;
        if (x0 > r[2] && x1 > r[2]) return false;
        return true;
    }
    const float dx = s[2] - s[0];
    const float dy = s[3] - s[1];
    const auto yAt = [&](float x) {
        return static_cast<double>(x - s[0]) / dx * dy + s[1];
    };
    const auto xAt = [&](float y) {
        return static_cast<double>(y - s[1]) / dy * dx + s[0];
    };
    if ((x0 < r[0]) != (x1 < r[0])) {
        const int yi = ftol(yAt(static_cast<float>(r[0])));
        if (yi >= r[1] && yi <= r[3]) return true;
    }
    if ((x0 > r[2]) != (x1 > r[2])) {
        const int yi = ftol(yAt(static_cast<float>(r[2])));
        if (yi >= r[1] && yi <= r[3]) return true;
    }
    if ((y0 < r[1]) != (y1 < r[1])) {
        const int xi = ftol(xAt(static_cast<float>(r[1])));
        if (xi >= r[0] && xi <= r[2]) return true;
    }
    if ((y0 > r[3]) != (y1 > r[3])) {
        const int xi = ftol(xAt(static_cast<float>(r[3])));
        if (xi >= r[1] && xi <= r[3]) return true;
    }
    return false;
}

// 0x00446fdc.
bool segmentHasPoint(const float s[4], float x, float y) {
    float n = x - s[0];
    float d = s[2] - s[0];
    if (d == 0.0F) {
        n = y - s[1];
        d = s[3] - s[1];
    }
    const float r = n / d;
    if (std::isnan(r)) return false;
    return !(r > 1.0F) && !(r < 0.0F);
}

// 0x00446e7c: a vertical segment never meets a horizontal one and parallel
// segments never intersect.
bool segmentsIntersect(const float a[4], const float b[4]) {
    const float dxa = a[2] - a[0];
    const float dya = a[3] - a[1];
    const float dxb = b[2] - b[0];
    const float dyb = b[3] - b[1];
    const float ma = dxa == 0.0F ? 0.0F : dya / dxa;
    const float mb = dxb == 0.0F ? 0.0F : dyb / dxb;
    if (ma == mb) return false;
    const float ca = a[1] - ma * a[0];
    const float cb = b[1] - mb * b[0];
    float x;
    float y;
    if (dxa == 0.0F) {
        x = a[0];
        y = mb * x + cb;
    } else if (dxb == 0.0F) {
        x = b[0];
        y = ma * x + ca;
    } else {
        x = (cb - ca) / (ma - mb);
        y = ma * x + ca;
    }
    return segmentHasPoint(a, x, y) && segmentHasPoint(b, x, y);
}

// 0x004483c3.
int wrapIndex(int current, int count, int delta) {
    const int value = current + delta;
    if (count <= 1) return 0;
    if (value < 0) return count - ((-value) % count);
    return value % count;
}

std::int32_t readI32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size())
        throw std::runtime_error("Mountain map truncated");
    std::int32_t value;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

float readF32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at + 4 > bytes.size())
        throw std::runtime_error("Mountain map truncated");
    float value;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// Assets

const std::uint8_t *MountainScene::mountainAssetOptional(
    std::string_view name, std::size_t &size) const {
    for (Package *candidate : {sceneAddonPackage.get(), sceneData2Package.get(),
                               sceneEditionPackage.get(), scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional(name)) {
            const auto data = candidate->data(*entry);
            size = data.size();
            return data.data();
        }
    }
    return nullptr;
}

std::span<const std::uint8_t> MountainScene::mountainAsset(
    std::string_view name) const {
    std::size_t size = 0;
    const std::uint8_t *data = mountainAssetOptional(name, size);
    if (!data)
        throw std::runtime_error("Mountain asset not found: " +
                                 std::string(name));
    return {data, size};
}

int MountainScene::mountainClock() const {
    // 0x0040c450, the application clock in milliseconds.
    return static_cast<int>(
        std::floor(const_cast<MountainScene *>(this)->sceneElapsedNow() *
                   1000.0));
}

int MountainScene::mountainTicks() const {
    // g_678b10: the 66 ms timer (0x00440fe4) counts with the phase of the
    // application clock because 0x0040bec0 discards due periods first.
    return mountainGame->tickOffset + mountainClock() / 66;
}

void MountainScene::parseMountainIni(std::span<const std::uint8_t> bytes) {
    // 0x0040dc10 keeps sections with their file ordinal; the track index
    // g_678b08 selects by that ordinal (0x0040e430).
    auto &game = *mountainGame;
    game.tracks.clear();
    std::string text(bytes.begin(), bytes.end());
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(std::string_view(text).substr(
            start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            game.tracks.push_back({line.substr(1, line.size() - 2), {}});
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || game.tracks.empty()) continue;
        game.tracks.back().second.push_back(
            {lower(trim(std::string_view(line).substr(0, equals))),
             trim(std::string_view(line).substr(equals + 1))});
    }
}

std::string MountainScene::mountainKey(std::string_view key,
                                       std::string_view fallback) const {
    const auto &game = *mountainGame;
    if (game.track < 0 || game.track >= static_cast<int>(game.tracks.size()))
        return std::string(fallback);
    for (const auto &[name, value] :
         game.tracks[static_cast<std::size_t>(game.track)].second) {
        if (name == key) return value;
    }
    return std::string(fallback);
}

void MountainScene::readMountainTrackKeys() {
    // 0x004415b5.
    auto &game = *mountainGame;
    const std::string section =
        game.track >= 0 && game.track < static_cast<int>(game.tracks.size())
            ? game.tracks[static_cast<std::size_t>(game.track)].first
            : std::string{};
    const auto colon = section.find(':');
    game.trackText.text = language.get(
        colon == std::string::npos ? "mountain" : section.substr(0, colon),
        colon == std::string::npos ? section : section.substr(colon + 1),
        section);
    game.mapNames[0] = mountainKey("easy", "dritenkelt2");
    game.mapNames[1] = mountainKey("medium", "middels2");
    game.mapNames[2] = mountainKey("hard", "new_level10");
    game.fffName = mountainKey("fff", "new_level10");
    game.effName = mountainKey("eff", "new_level10");
    game.postWalk = mountainKey("postwalk", "gustav_walk");
    game.postCrash = mountainKey("postcrash", "gustav_fall");
}

int MountainScene::loadMountainClip(const std::string &name) {
    auto &game = *mountainGame;
    MountainClip clip;
    clip.name = name;
    clip.media = decodeSmacker(mountainAsset("animation\\" + name + ".smk"),
                               true, true, true, 0, nullptr,
                               SmackerIndexZeroAlpha::Opaque);
    // 0x0046f800 skips texels equal to the sprite's first pixel (RGB565).
    for (std::size_t index = 0; index < clip.media.videoFrames.size(); ++index) {
        auto &frame = clip.media.videoFrames[index];
        auto *pixels = reinterpret_cast<std::uint32_t *>(frame.bgra.data());
        const std::size_t count = static_cast<std::size_t>(frame.width) *
                                  static_cast<std::size_t>(frame.height);
        if (count == 0) continue;
        const std::uint16_t key = rgb565(pixels[0]);
        for (std::size_t pixel = 0; pixel < count; ++pixel)
            pixels[pixel] = rgb565(pixels[pixel]) == key
                ? 0U : (pixels[pixel] | 0xff000000U);
    }
    if (!clip.media.videoFrames.empty()) {
        const auto &frame = clip.media.videoFrames.metadata(0U);
        clip.texture.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            frame.width, frame.height));
        if (!clip.texture) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(clip.texture.get(), SDL_BLENDMODE_BLEND);
    }
    game.clips.push_back(std::move(clip));
    return static_cast<int>(game.clips.size()) - 1;
}

int MountainScene::loadMountainSound(const std::string &name) {
    auto &game = *mountainGame;
    MountainSound sound;
    sound.name = name;
    std::size_t size = 0;
    if (const auto *data =
            mountainAssetOptional("sound\\" + name + ".wav", size)) {
        sound.media = decodeSmacker({data, size}, false, true, true);
    }
    game.sounds.push_back(std::move(sound));
    return static_cast<int>(game.sounds.size()) - 1;
}

bool MountainScene::mountainSoundPlaying(int index) {
    if (index < 0 || !mountainGame) return false;
    auto &sound = mountainGame->sounds[static_cast<std::size_t>(index)];
    if (!sound.playing) return false;
    if (sound.looping) return true;
    const double duration = sound.media.audioSamples.empty()
        ? 0.0
        : static_cast<double>(sound.media.audioSamples.size()) /
              std::max(1, sound.media.audioChannels) /
              std::max(1, sound.media.audioRate);
    if (sceneElapsedNow() - sound.startedAt >= duration) sound.playing = false;
    return sound.playing;
}

void MountainScene::playMountainSound(int index, bool loop) {
    // 0x00413a40 SOUND_Play; loop = DSBPLAY_LOOPING.
    if (index < 0 || !mountainGame) return;
    auto &sound = mountainGame->sounds[static_cast<std::size_t>(index)];
    if (sound.handle != LiveAudioMixer::kInvalidEffectHandle)
        audioMixer.removeEffect(sound.handle);
    sound.handle = LiveAudioMixer::kInvalidEffectHandle;
    if (!sound.media.audioSamples.empty())
        sound.handle = audioMixer.addEffect(
            sound.media, audioVolumes[0] * sceneAudioGains[0]);
    sound.startedAt = sceneElapsedNow();
    sound.looping = loop;
    sound.playing = true;
}

void MountainScene::stopMountainSound(int index) {
    if (index < 0 || !mountainGame) return;
    auto &sound = mountainGame->sounds[static_cast<std::size_t>(index)];
    if (sound.handle != LiveAudioMixer::kInvalidEffectHandle)
        audioMixer.removeEffect(sound.handle);
    sound.handle = LiveAudioMixer::kInvalidEffectHandle;
    sound.playing = false;
    sound.looping = false;
}

void MountainScene::playMountainSoundPooled(int index) {
    // 0x00444bfb: a busy sound is duplicated into one of 20 pool buffers.
    if (index < 0 || !mountainGame) return;
    auto &sound = mountainGame->sounds[static_cast<std::size_t>(index)];
    if (!mountainSoundPlaying(index)) {
        playMountainSound(index, false);
        return;
    }
    if (!sound.media.audioSamples.empty())
        audioMixer.addEffect(sound.media,
                             audioVolumes[0] * sceneAudioGains[0]);
}

void MountainScene::startMountainClip(int index) {
    // 0x00403a20: rewind to frame 0 and play (sound on).
    if (index < 0) return;
    auto &clip = mountainGame->clips[static_cast<std::size_t>(index)];
    clip.playing = true;
    clip.frame = 0;
    clip.frameStartedAt = sceneElapsedNow();
    if (!clip.media.audioSamples.empty())
        audioMixer.addEffect(clip.media, audioVolumes[0] * sceneAudioGains[0]);
}

void MountainScene::stopMountainClip(int index) {
    // 0x00403590.
    if (index < 0) return;
    mountainGame->clips[static_cast<std::size_t>(index)].playing = false;
}

void MountainScene::advanceMountainClip(int index) {
    // 0x00403bc0: SmackWait releases the next frame on the clip's own
    // schedule from SmackGoto(0); at most one frame per update.
    if (index < 0) return;
    auto &clip = mountainGame->clips[static_cast<std::size_t>(index)];
    if (!clip.playing || clip.media.videoFrames.empty()) return;
    const int last = static_cast<int>(clip.media.videoFrames.size()) - 1;
    if (clip.frame >= last) return;
    const double due = clip.media.videoFrames
                           .metadata(static_cast<std::size_t>(clip.frame + 1))
                           .time -
                       clip.media.videoFrames.metadata(0U).time;
    if (sceneElapsedNow() - clip.frameStartedAt >= due) ++clip.frame;
}

// ---------------------------------------------------------------------------
// Map loading (0x00441f58, 0x004419d0, 0x0044356b, 0x00442de3)

void MountainScene::loadMountainMap() {
    auto &game = *mountainGame;
    MountainMap map;
    const std::string mapName =
        game.mapNames[static_cast<std::size_t>(game.difficulty)];
    const auto lData = mountainAsset("maps\\" + mapName + ".l");
    // 0x00441e4f RLE: one continuous PackBits-style stream after the
    // discarded lead dword.
    std::size_t at = 4;
    int run = 0;
    std::uint8_t control = 0;
    std::uint8_t value = 0;
    const auto next = [&]() -> std::uint8_t {
        if (run == 0) {
            if (at >= lData.size())
                throw std::runtime_error("Mountain map stream ended");
            control = lData[at++];
            if (control & 0x80U) value = lData[at++];
            run = control & 0x7f;
        }
        if (!(control & 0x80U)) value = lData[at++];
        --run;
        return value;
    };
    std::array<std::uint8_t, 0xdc> header{};
    for (auto &byte : header) byte = next();
    std::int32_t layers;
    std::memcpy(&layers, header.data() + 0x08, 4);
    std::memcpy(&map.widthTiles, header.data() + 0x0c, 4);
    std::memcpy(&map.heightTiles, header.data() + 0x10, 4);
    map.widthPx = map.widthTiles << 5;
    map.heightPx = map.heightTiles << 5;
    const std::size_t cells = static_cast<std::size_t>(map.widthTiles) *
                              static_cast<std::size_t>(map.heightTiles);
    for (int layer = 0; layer < 4; ++layer) {
        auto &grid = map.layers[static_cast<std::size_t>(layer)];
        grid.assign(cells, 0xffffU);
        if (layers <= layer) continue;
        // Column-major (0x0044208e).
        for (int x = 0; x < map.widthTiles; ++x) {
            for (int y = 0; y < map.heightTiles; ++y) {
                const std::uint8_t low = next();
                const std::uint8_t high = next();
                grid[static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(map.widthTiles) +
                     static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>(low | (high << 8));
            }
        }
    }
    // 0x004422ce: the tile set is header+0x98, cut at the first '.'.
    std::string tileSet(reinterpret_cast<const char *>(header.data() + 0x99),
                        32);
    tileSet = tileSet.substr(0, tileSet.find('.'));
    map.tileSurface = argbSurface(mountainAsset("bitmap\\" + tileSet + ".bmp"));
    // 0x004435e3: the colour key is the tile set's bottom-right pixel.
    const std::uint16_t key = rgb565(argbAt(
        map.tileSurface.get(), map.tileSurface->w - 1,
        map.tileSurface->h - 1));
    map.tileKey = key;
    map.tileColumns = map.tileSurface->w / 32;
    const int rows = map.tileSurface->h / 32;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < map.tileColumns; ++column) {
            MountainTile tile;
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 32; ++x) {
                    const bool opaque =
                        rgb565(argbAt(map.tileSurface.get(), column * 32 + x,
                                      row * 32 + y)) != key;
                    tile.opaque[static_cast<std::size_t>(y * 32 + x)] = opaque;
                    tile.any = tile.any || opaque;
                }
            }
            map.tiles.push_back(tile);
        }
    }
    map.tileTexture = keyedTexture(renderer, map.tileSurface.get(), key);

    // .fff track graph ("FUNKY FILE FORMAT", 0x004424d3).
    const auto fff = mountainAsset("maps\\" + game.fffName + ".fff");
    if (fff.size() < 0x16 ||
        std::memcmp(fff.data(), "FUNKY FILE FORMAT", 17) != 0)
        throw std::runtime_error("Mountain .fff magic mismatch");
    std::size_t offset = 0x12;
    const int nodeCount = readI32(fff, offset);
    offset += 4;
    map.nodes.resize(static_cast<std::size_t>(nodeCount));
    for (auto &node : map.nodes) {
        node.x = readF32(fff, offset);
        node.y = readF32(fff, offset + 4);
        node.type = readI32(fff, offset + 8);
        const int links = readI32(fff, offset + 12);
        offset += 16;
        for (int link = 0; link < links; ++link) {
            node.links.push_back(readI32(fff, offset));
            offset += 4;
        }
    }
    for (std::size_t index = 0; index < map.nodes.size(); ++index) {
        for (const int link : map.nodes[index].links) {
            MountainEdge edge;
            edge.from = static_cast<int>(index);
            edge.to = link;
            map.edges.push_back(edge);
        }
    }
    for (std::size_t node = 0; node < map.nodes.size(); ++node) {
        for (std::size_t edge = 0; edge < map.edges.size(); ++edge) {
            if (map.edges[edge].from == static_cast<int>(node) &&
                map.nodes[node].outCount < 2)
                map.nodes[node].out[static_cast<std::size_t>(
                    map.nodes[node].outCount++)] = static_cast<int>(edge);
        }
        for (std::size_t edge = 0; edge < map.edges.size(); ++edge) {
            if (map.edges[edge].to == static_cast<int>(node) &&
                map.nodes[node].inCount < 2)
                map.nodes[node].in[static_cast<std::size_t>(
                    map.nodes[node].inCount++)] = static_cast<int>(edge);
        }
    }
    // 0x00442c1f; 0x0048f430 is atan2(dx, dy).
    for (auto &edge : map.edges) {
        const auto &a = map.nodes[static_cast<std::size_t>(edge.from)];
        const auto &b = map.nodes[static_cast<std::size_t>(edge.to)];
        edge.dx = b.x - a.x;
        edge.dy = b.y - a.y;
        const float length = static_cast<float>(
            std::sqrt(static_cast<double>(edge.dx * edge.dx + edge.dy * edge.dy)));
        edge.dx /= length;
        edge.dy /= length;
        edge.nx = edge.dy;
        edge.ny = -edge.dx;
        edge.angle = static_cast<float>(std::atan2(
                         static_cast<double>(edge.dx),
                         static_cast<double>(edge.dy))) +
                     kPi;
    }

    // .eff pickups ("EVEN FUNKIER FILEFORMAT", 0x0044299d).
    std::size_t effSize = 0;
    if (const auto *effData = mountainAssetOptional(
            "maps\\" + game.effName + ".eff", effSize)) {
        const std::span<const std::uint8_t> eff{effData, effSize};
        if (eff.size() < 0x1c ||
            std::memcmp(eff.data(), "EVEN FUNKIER FILEFORMAT", 23) != 0)
            throw std::runtime_error("Mountain .eff magic mismatch");
        const int itemCount = readI32(eff, 0x18);
        for (int index = 0; index < itemCount; ++index) {
            const std::size_t record = 0x1c + static_cast<std::size_t>(index) * 12;
            MountainItem item;
            item.x = readI32(eff, record);
            item.y = readI32(eff, record + 4);
            item.kind = readI32(eff, record + 8);
            const int half = 32 / 2;
            item.left = item.x - half;
            item.right = item.x + half;
            item.top = item.y - half;
            item.bottom = item.y + half;
            item.startTick = index;   // item+0x20 is the item's own index
            map.items.push_back(item);
        }
    }

    // 0x00442de3: one actor per trigger node (4 = truck, 6 = walker),
    // spawned at the next end node (5 / 7).
    int last = -1;
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        const int type = map.nodes[i].type;
        if (type != 4 && type != 6) continue;
        const int want = type == 4 ? 5 : 7;
        for (std::size_t j = i + 1; j < map.nodes.size(); ++j) {
            if (map.nodes[j].type != want || last == static_cast<int>(j))
                continue;
            MountainActor actor;
            actor.triggerNode = static_cast<int>(i);
            actor.spawnNode = static_cast<int>(j);
            actor.edge = map.nodes[j].in[0];
            actor.angle = actor.edge >= 0
                ? map.edges[static_cast<std::size_t>(actor.edge)].angle
                : kLevelAngle;
            actor.x = map.nodes[j].x;
            actor.y = map.nodes[j].y;
            if (want == 5) {
                actor.sound = loadMountainSound("melkebil");
                actor.baseSpeed = kUnit * 10.0F;
                actor.walkClip = loadMountainClip("melkebil");
                actor.outline = kTruckOutline.data();
                map.trucks.push_back(static_cast<int>(map.actors.size()));
            } else {
                actor.baseSpeed = kUnit * 2.2F;
                actor.walkClip = loadMountainClip(game.postWalk);
                std::uniform_int_distribution<int> variant(0, 1);
                actor.fallClip = loadMountainClip(
                    game.postCrash + std::to_string(variant(randomEngine) + 1));
                actor.upright = true;
                actor.outline = kWalkerOutline.data();
                map.walkers.push_back(static_cast<int>(map.actors.size()));
            }
            actor.speed = actor.baseSpeed;
            const auto &first =
                game.clips[static_cast<std::size_t>(actor.walkClip)];
            const int w = first.media.videoFrames.empty()
                ? 256 : first.media.videoFrames.metadata(0U).width;
            const int h = first.media.videoFrames.empty()
                ? 336 : first.media.videoFrames.metadata(0U).height;
            actor.spriteW = static_cast<float>(w);
            actor.spriteH = static_cast<float>(h) - 3.0F;
            actor.spriteHalfW = static_cast<float>(w) / 2.0F;
            actor.spriteHalfH = static_cast<float>(h - 3) / 2.0F;
            last = static_cast<int>(j);
            map.actors.push_back(actor);
            break;
        }
    }

    // 0x00442ce7: last start (2) and finish (3) nodes win.
    int startX = -1;
    int finishX = -1;
    int startEdge = 0;
    for (const auto &node : map.nodes) {
        if (node.type == 2) {
            startX = ftol(node.x);
            startEdge = node.out[0];
        }
        if (node.type == 3) finishX = ftol(node.x);
    }
    if (startX == -1 || finishX == -1) {
        std::cout << "mountain_ValidateMap: unable to validate map. using "
                     "defaults\n";
        startX = 10;
        finishX = 100;
        startEdge = 0;
    }
    map.startX = startX;
    map.finishX = finishX;
    map.startEdge = startEdge;
    game.map = std::move(map);
    game.loadedDifficulty = game.difficulty;
}

void MountainScene::reloadMountainTiles() {
    // 0x004419d0: same track, other difficulty: only the tile layers.
    auto &game = *mountainGame;
    if (game.loadedDifficulty == game.difficulty || !game.map) return;
    game.loadedDifficulty = game.difficulty;
    auto &map = *game.map;
    const auto lData = mountainAsset(
        "maps\\" + game.mapNames[static_cast<std::size_t>(game.difficulty)] +
        ".l");
    std::size_t at = 4;
    int run = 0;
    std::uint8_t control = 0;
    std::uint8_t value = 0;
    const auto next = [&]() -> std::uint8_t {
        if (run == 0) {
            control = lData[at++];
            if (control & 0x80U) value = lData[at++];
            run = control & 0x7f;
        }
        if (!(control & 0x80U)) value = lData[at++];
        --run;
        return value;
    };
    std::array<std::uint8_t, 0xdc> header{};
    for (auto &byte : header) byte = next();
    std::int32_t layers;
    std::memcpy(&layers, header.data() + 0x08, 4);
    for (int layer = 0; layer < 4; ++layer) {
        auto &grid = map.layers[static_cast<std::size_t>(layer)];
        for (int x = 0; x < map.widthTiles; ++x) {
            for (int y = 0; y < map.heightTiles; ++y) {
                const std::size_t cell =
                    static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(map.widthTiles) +
                    static_cast<std::size_t>(x);
                if (layers > layer) {
                    const std::uint8_t low = next();
                    const std::uint8_t high = next();
                    grid[cell] = static_cast<std::uint16_t>(low | (high << 8));
                } else {
                    grid[cell] = 0xffffU;
                }
            }
        }
    }
}

void MountainScene::loadMountainPlayer() {
    // 0x00443a6e.
    auto &game = *mountainGame;
    MountainPlayer player;
    const auto slot = [&](MountainClipSlot which) -> int & {
        return game.playerClips[static_cast<std::size_t>(which)];
    };
    slot(MountainClipSlot::Cycle) = loadMountainClip(mountainKey("cycle", "cycle"));
    slot(MountainClipSlot::Jump) = loadMountainClip(mountainKey("jump", "jumpup"));
    slot(MountainClipSlot::InAir) =
        loadMountainClip(mountainKey("inair", "inair_cycle"));
    slot(MountainClipSlot::Land) = loadMountainClip(mountainKey("land", "jumpdown"));
    slot(MountainClipSlot::Crash2) =
        loadMountainClip(mountainKey("crash2", "crashforward"));
    slot(MountainClipSlot::Crash1) =
        loadMountainClip(mountainKey("crash1", "crashbackward"));
    slot(MountainClipSlot::Duck) = loadMountainClip(mountainKey("duck", "downcycle"));
    const auto &cycle =
        game.clips[static_cast<std::size_t>(slot(MountainClipSlot::Cycle))];
    const int w = cycle.media.videoFrames.empty()
        ? 256 : cycle.media.videoFrames.metadata(0U).width;
    const int h = cycle.media.videoFrames.empty()
        ? 336 : cycle.media.videoFrames.metadata(0U).height;
    player.spriteW = static_cast<float>(w);
    player.spriteH = static_cast<float>(h) - 3.0F;
    player.spriteHalfW = static_cast<float>(w) / 2.0F;
    player.spriteHalfH = static_cast<float>(h - 3) / 2.0F;
    game.soundRoll = loadMountainSound("bloop5");
    game.soundJump = loadMountainSound(mountainKey("jumpsound", "jump1"));
    game.soundDuringJump = loadMountainSound("during jump");
    game.soundGround = game.soundMode == 1
        ? loadMountainSound(mountainKey("groundsound", "bloop5")) : -1;
    game.player = player;
}

// ---------------------------------------------------------------------------
// Scene anims, controls and text (traced like SCENE_AddAnim/RemoveAnim)

void MountainScene::traceMountainAdd(std::string_view asset, int x, int y,
                                     int layer) {
    referenceTrace.emit("animation_add", {
        {"asset", std::string(asset)}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", 0}, {"conflict_mask", 0}});
}

void MountainScene::traceMountainRemove(std::string_view asset) {
    // SCENE_RemoveAnim 0x00412aa0 logs before its nested SCENE_StopAnim.
    referenceTrace.emit("animation_remove", {
        {"asset", std::string(asset)}, {"reason", "removed"}});
    referenceTrace.emit("animation_stop", {
        {"asset", std::string(asset)}, {"reason", "stopped"}});
}

void MountainScene::traceMountainState(std::string_view state,
                                       std::string_view firstName,
                                       int firstValue,
                                       std::string_view secondName,
                                       int secondValue) {
    const std::string name(state);
    if (firstName.empty()) {
        referenceTrace.emit("minigame_state",
                            {{"game", "mountain"}, {"state", name}});
    } else if (secondName.empty()) {
        referenceTrace.emit("minigame_state",
                            {{"game", "mountain"}, {"state", name},
                             {firstName, firstValue}});
    } else {
        referenceTrace.emit("minigame_state",
                            {{"game", "mountain"}, {"state", name},
                             {firstName, firstValue},
                             {secondName, secondValue}});
    }
}

void MountainScene::traceMountainPickup(int kind, int item, int score) {
    referenceTrace.emit("minigame_state",
                        {{"game", "mountain"}, {"state", "pickup"},
                         {"kind", kind}, {"item", item}, {"score", score}});
}

MountainScene::MountainControl *MountainScene::findMountainControl(int id) {
    for (auto &control : mountainGame->controls)
        if (control.id == id && control.active) return &control;
    return nullptr;
}

void MountainScene::createMountainControl(int id, int x, int y, int layer,
                                          std::string off, std::string on,
                                          bool radio) {
    // 0x00406ed0 returns at once if the id exists.
    if (findMountainControl(id)) return;
    MountainControl control;
    control.id = id;
    control.x = x;
    control.y = y;
    control.layer = layer;
    control.images = {std::move(off), std::move(on)};
    control.radio = radio;
    control.active = true;
    control.traced = control.images[0];
    traceMountainAdd(control.traced, -1, -1, layer);
    mountainGame->controls.push_back(std::move(control));
}

void MountainScene::setMountainControlFrame(MountainControl &control,
                                            int frame) {
    // 0x00406740 -> 0x00406760: a new image removes and re-adds the anim.
    if (control.frame == frame) return;
    control.frame = frame;
    const std::string &image =
        control.images[static_cast<std::size_t>(std::clamp(frame, 0, 1))];
    if (image != control.traced) {
        traceMountainRemove(control.traced);
        control.traced = image;
        traceMountainAdd(control.traced, -1, -1, control.layer);
    }
}

void MountainScene::destroyMountainControl(int id) {
    // 0x00406610.
    if (auto *control = findMountainControl(id)) {
        traceMountainRemove(control->traced);
        control->active = false;
    }
}

void MountainScene::createMountainText(MountainText &text, int x, int y,
                                       int w, int h, int layer) {
    // 0x004078c0 destroys the old control first, then adds the new one.
    destroyMountainText(text);
    text.rect = {x, y, w, h};
    text.layer = layer;
    text.active = true;
    text.text.clear();
    traceMountainAdd("", x, y, layer);
}

void MountainScene::destroyMountainText(MountainText &text) {
    // 0x00407840.
    if (text.active) traceMountainRemove("");
    text.active = false;
}

// ---------------------------------------------------------------------------
// Scene lifecycle

MountainScene::MountainGame MountainScene::loadMountainGame(Package &) {
    mountainGame.emplace();
    auto &game = *mountainGame;
    parseMountainIni(mountainAsset("maps.ini"));
    const auto load = [&](std::string_view name, bool keyed) {
        SurfacePtr surface = keyed ? loadGreenKeyedBmp(mountainAsset(name))
                                   : loadBmp(mountainAsset(name));
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        return texture;
    };
    game.startInterfaceTexture = load("bitmap\\start interface.bmp", true);
    game.progressTexture = load("bitmap\\progress.bmp", false);
    game.headTexture = load("bitmap\\solanhead.bmp", true);
    for (std::size_t index = 0; index < kSlotIcons.size(); ++index) {
        game.slotIconTextures[index] = load(
            std::string("bitmap\\") + kSlotIcons[index] + ".bmp", true);
    }
    for (const char *name :
         {"strbtnoff", "strbtnon", "levelcheck", "levelnonchk", "up_0", "up_1",
          "down_0", "down_1"}) {
        game.controlTextures[name] =
            load(std::string("bitmap\\") + name + ".bmp", true);
    }
    // 0x00440f38: pickup strips keyed by their top-left pixel.
    for (std::size_t index = 0; index < kPickups.size(); ++index) {
        game.pickupSurfaces[index] = argbSurface(mountainAsset(
            std::string("bitmap\\") + kPickups[index].name + ".bmp"));
        game.pickupTextures[index] = keyedTexture(
            renderer, game.pickupSurfaces[index].get(),
            rgb565(argbAt(game.pickupSurfaces[index].get(), 0, 0)));
    }
    game.frameTarget.reset(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kWidth,
        kHeight));
    if (game.frameTarget)
        SDL_SetTextureBlendMode(game.frameTarget.get(), SDL_BLENDMODE_BLEND);
    game.stamp.assign(static_cast<std::size_t>(kWidth * kHeight), 0U);
    std::cout << "Loaded executable-derived Mountain: " << game.tracks.size()
              << " maps.ini track(s), .l/.fff/.eff map loader, segment-graph "
                 "physics, pixel-stamp obstacle collision, milk truck and "
                 "postman actors, pickups, slot bonus and three lives\n";
    MountainGame result = std::move(game);
    mountainGame.reset();
    return result;
}

void MountainScene::initializeMountainScene() {
    // 0x00440d6e.
    auto &game = *mountainGame;
    game.difficulty = 1;
    game.track = 0;
    game.loadedTrack = -1;
    game.loadedDifficulty = -1;
    traceMountainAdd("help_0", -1, -1, 1);
    traceMountainAdd("exit_0", -1, -1, 1);
    game.soundFanfare = loadMountainSound("fanfare");
    game.interfaceName = "interface";
    game.interfaceSurface.reset();
    game.interfaceTexture.reset();
    {
        SurfacePtr surface = loadBmp(mountainAsset("bitmap\\interface.bmp"));
        game.interfaceTexture = makeTexture(renderer, surface.get());
    }
    game.interfaceAdded = true;
    traceMountainAdd("interface", -1, -1, 0);
    createMountainText(game.timeText, 465, 25, 65, 13, 1);
    createMountainText(game.scoreText, 465, 50, 65, 13, 1);
    // PROGRESS_Create 0x004089a0 -> PROGRESS_Set adds it at (-1,-1).
    game.progress = 0.0F;
    game.progressAdded = true;
    traceMountainAdd("", -1, -1, 1);
    for (std::size_t index = 0; index < kPickups.size(); ++index)
        game.pickupSounds[index] = loadMountainSound(kPickups[index].sound);
    game.running = false;
    game.frameMs = mountainClock();
    game.previousFrameMs = game.frameMs;
    game.tickOffset = 0;
    game.attract = true;
    // 0x0044103b: a tournament round starts play from init instead.
    if (!tournament.active) enterMountainAttract();
    else readMountainTrackKeys();
}

void MountainScene::startMountainGame() {
    if (!mountainGame) return;
    auto &game = *mountainGame;
    if (!game.initialized) {
        game.initialized = true;
        initializeMountainScene();
    }
    if (tournament.active && tournament.game >= 0 &&
        tournament.game < static_cast<int>(tournament.games.size())) {
        // 0x004410b4-0x0044113e: the round's record +0x24 picks the track.
        // UNCERTAIN: the original clamps with min(count, v), which lets
        // v == count select no section; native keeps the last section.
        const int count = static_cast<int>(game.tracks.size());
        int track = static_cast<int>(
            tournament.games[static_cast<std::size_t>(tournament.game)].version);
        track = std::clamp(track, 0, std::max(0, count - 1));
        game.track = track;
        game.loadedTrack = -1;
        readMountainTrackKeys();
    }
    startMountainRun();
}

void MountainScene::enterMountainAttract() {
    // 0x00444431.
    auto &game = *mountainGame;
    game.attract = true;
    game.startInterfaceAdded = true;
    traceMountainAdd("Start interface", -1, -1, 1);
    createMountainControl(kControlStart, 284, 258, 2, "strbtnoff", "strbtnon",
                          false);
    createMountainControl(kControlEasy, 228, 222, 2, "levelnonchk",
                          "levelcheck", true);
    createMountainControl(kControlMedium, 305, 222, 2, "levelnonchk",
                          "levelcheck", true);
    createMountainControl(kControlHard, 378, 222, 2, "levelnonchk",
                          "levelcheck", true);
    for (auto &control : game.controls)
        if (control.active && control.radio)
            setMountainControlFrame(control, 0);
    createMountainControl(kControlDown, 210, 176, 2, "down_0", "down_1", false);
    createMountainControl(kControlUp, 405, 176, 2, "up_0", "up_1", false);
    if (auto *control = findMountainControl(kControlEasy + game.difficulty))
        setMountainControlFrame(*control, 1);
    createMountainText(game.easyText, 207, 205, 67, 15, 3);
    createMountainText(game.mediumText, 284, 205, 67, 15, 3);
    createMountainText(game.hardText, 358, 205, 67, 15, 3);
    createMountainText(game.startText, 291, 263, 51, 18, 3);
    createMountainText(game.trackText, 228, 173, 176, 20, 3);
    game.easyText.text = language.get("mountain", "EASY", "Lett");
    game.mediumText.text = language.get("mountain", "MEDIUM", "Middels");
    game.hardText.text = language.get("mountain", "HARD", "Vanskelig");
    game.startText.text = language.get("mountain", "START", "Start");
    readMountainTrackKeys();
}

void MountainScene::startMountainRun() {
    // 0x0044108e.
    auto &game = *mountainGame;
    game.subgameInProgress = true;
    if (game.loadedTrack != game.track) {
        game.map.reset();
        game.player.reset();
        game.clips.clear();
        game.sounds.erase(game.sounds.begin() +
                              static_cast<std::ptrdiff_t>(
                                  std::min<std::size_t>(game.sounds.size(),
                                                        8U)),
                          game.sounds.end());
        game.soundMode = std::atoi(mountainKey("soundmode", "0").c_str());
        loadMountainPlayer();
        loadMountainMap();
        game.loadedTrack = game.track;
        // 0x00441270-0x004412a7: the HUD descriptor takes the ini name.
        traceMountainRemove("interface");
        game.interfaceName = mountainKey("interface", "interface");
        SurfacePtr surface = loadBmp(
            mountainAsset("bitmap\\" + game.interfaceName + ".bmp"));
        game.interfaceTexture = makeTexture(renderer, surface.get());
        traceMountainAdd("interface", -1, -1, 0);
    } else {
        reloadMountainTiles();
    }
    auto &map = *game.map;
    auto &player = *game.player;
    player.checkpoint = map.startEdge;
    player.started = false;
    player.lives = tournament.active ? 1 : 3;   // 0x004412d8
    player.score = 0;
    for (int index = 0; index < 3; ++index) {
        player.slots[static_cast<std::size_t>(index)] = -1;
        if (game.slotIconsAdded[static_cast<std::size_t>(index)]) {
            traceMountainRemove(game.slotIconNames[static_cast<std::size_t>(index)]);
            game.slotIconsAdded[static_cast<std::size_t>(index)] = false;
        }
    }
    player.collected = {0, 0};
    // 0x00441411: profile record 0x402, key 0, eight bytes.
    if (profileIndex < profiles.size() && profiles[profileIndex] != "No profile") {
        const fs::path path = profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(path)) {
            if (const auto bytes =
                    profileSceneRecordBytes(path, "mountain", 0x402U, 0U);
                bytes && bytes->size() >= 8) {
                std::memcpy(player.collected.data(), bytes->data(), 8);
            }
        }
    }
    for (auto &item : map.items) {
        const int flag = item.kind >= 0 && item.kind < 7
            ? kPickups[static_cast<std::size_t>(item.kind)].flag : -1;
        item.active = !(flag >= 0 &&
                        player.collected[static_cast<std::size_t>(flag)] != 0);
    }
    const auto roll = [&](int count) {
        if (count == 0) return 0;
        std::uniform_int_distribution<int> distribution(0, 0x7fff);
        return distribution(randomEngine) % count;
    };
    map.truckChoice = game.pinnedTruck.value_or(
        roll(static_cast<int>(map.trucks.size())));
    map.walkerChoice = game.pinnedWalker.value_or(
        roll(static_cast<int>(map.walkers.size())));
    game.attract = false;
    traceMountainState("run_start", "level", game.difficulty, "lives", player.lives);
    destroyMountainControl(kControlStart);
    destroyMountainControl(kControlEasy);
    destroyMountainControl(kControlMedium);
    destroyMountainControl(kControlHard);
    destroyMountainControl(kControlDown);
    destroyMountainControl(kControlUp);
    destroyMountainText(game.trackText);
    destroyMountainText(game.easyText);
    destroyMountainText(game.mediumText);
    destroyMountainText(game.hardText);
    destroyMountainText(game.startText);
    if (game.startInterfaceAdded) {
        traceMountainRemove("Start interface");
        game.startInterfaceAdded = false;
    }
    beginMountainLife();
}

void MountainScene::beginMountainLife() {
    // 0x00441747.
    auto &game = *mountainGame;
    resetMountainActors();
    placeMountainPlayer();
    playMountainSound(game.soundRoll, true);
    if (game.soundGround >= 0) playMountainSound(game.soundGround, true);
    updateMountainHeads();
    // 0x004417a2 resets the 66 ms counter to the pickup count.
    game.tickOffset = static_cast<int>(game.map->items.size()) -
                      mountainClock() / 66;
    game.previousFrameMs = mountainClock();
    game.running = true;
    game.takeOff = false;
}

void MountainScene::updateMountainHeads() {
    // 0x0044181d.
    auto &game = *mountainGame;
    for (std::size_t index = 0; index < 3; ++index) {
        if (game.player->lives > static_cast<int>(index)) {
            if (!game.headsAdded[index]) {
                game.headsAdded[index] = true;
                traceMountainAdd("solanhead", -1, -1, 1);
            }
        } else if (game.headsAdded[index]) {
            game.headsAdded[index] = false;
            traceMountainRemove("solanhead");
        }
    }
}

void MountainScene::resetMountainActors() {
    // 0x004418c5.
    auto &game = *mountainGame;
    auto &map = *game.map;
    for (auto &actor : map.actors) {
        const auto &spawn = map.nodes[static_cast<std::size_t>(actor.spawnNode)];
        actor.edge = spawn.in[0];
        actor.angle = actor.edge >= 0
            ? map.edges[static_cast<std::size_t>(actor.edge)].angle
            : actor.angle;
        actor.x = spawn.x;
        actor.y = spawn.y;
        actor.speed = actor.baseSpeed;
        actor.active = false;
        actor.hit = false;
        if (actor.sound >= 0) stopMountainSound(actor.sound);
        stopMountainClip(actor.walkClip);
        stopMountainClip(actor.fallClip);
    }
}

void MountainScene::placeMountainPlayer() {
    // 0x004436f3.
    auto &game = *mountainGame;
    auto &map = *game.map;
    auto &player = *game.player;
    const auto &edge = map.edges[static_cast<std::size_t>(player.checkpoint)];
    player.x = map.nodes[static_cast<std::size_t>(edge.from)].x + 1.0F;
    player.y = segmentY(map.nodes, edge, player.x) + 1.0F;
    player.airborne = player.falling = player.ducking = false;
    player.duckClip = player.crashed = player.crashClipStarted = false;
    player.upTime = -1000;
    player.angle = edge.angle;
    player.edge = player.checkpoint;
    player.finished = false;
    player.vx = kUnit * 14.0F * edge.dx;
    player.vy = kUnit * 14.0F * edge.dy;
    player.gravityResidual = 0.0F;
    player.jumpPressed = false;
    mountainClipQueueReset();
    mountainClipPlayNow(
        game.playerClips[static_cast<std::size_t>(MountainClipSlot::Cycle)],
        true);
}

void MountainScene::mountainClipQueueReset() {
    // 0x0044386f.
    auto &player = *mountainGame->player;
    player.queueTail = 0;
    player.queueHead = -1;
}

bool MountainScene::mountainClipQueueEmpty() const {
    // 0x00440958.
    const auto &player = *mountainGame->player;
    return player.queueHead < player.queueTail;
}

void MountainScene::mountainClipEnqueue(int clip, bool loop) {
    // 0x0044398c writes before testing for a full queue.
    auto &player = *mountainGame->player;
    ++player.queueHead;
    player.queue[static_cast<std::size_t>(player.queueHead % 16)] = {clip, loop};
}

void MountainScene::mountainClipPlayNow(int clip, bool loop) {
    // 0x00443893: stop the playing clip, then play `clip` alone.
    auto &player = *mountainGame->player;
    if (!mountainClipQueueEmpty()) {
        auto &current =
            player.queue[static_cast<std::size_t>(player.queueTail % 16)];
        current.second = false;
        stopMountainClip(current.first);
    }
    mountainClipQueueReset();
    mountainClipEnqueue(clip, loop);
    startMountainClip(clip);
}

void MountainScene::advanceMountainPlayerClip() {
    // 0x00440801.
    auto &player = *mountainGame->player;
    if (mountainClipQueueEmpty()) return;
    auto &entry = player.queue[static_cast<std::size_t>(player.queueTail % 16)];
    advanceMountainClip(entry.first);
    if (entry.second) return;
    auto &clip = mountainGame->clips[static_cast<std::size_t>(entry.first)];
    if (clip.frame == static_cast<int>(clip.media.videoFrames.size()) - 1) {
        stopMountainClip(entry.first);
        ++player.queueTail;
        if (!mountainClipQueueEmpty()) {
            startMountainClip(
                player.queue[static_cast<std::size_t>(player.queueTail % 16)]
                    .first);
        }
    }
}

// ---------------------------------------------------------------------------
// Input

bool MountainScene::handleMountainKey(SDL_Keycode key, bool pressed) {
    if (!mountainGame) return false;
    auto &game = *mountainGame;
    const bool steering = key == SDLK_LEFT || key == SDLK_RIGHT ||
                          key == SDLK_UP || key == SDLK_DOWN ||
                          key == SDLK_SPACE;
    if (!steering) return false;
    // 0x004481d0: WM_KEYDOWN Up stamps the time.
    if (pressed && key == SDLK_UP && game.player)
        game.player->upTime = mountainClock();
    return !game.attract;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
MountainScene::mountainHotspotAt(int x, int y) const {
    if (!mountainGame || activeScene != "mountain") return std::nullopt;
    const SDL_Point point{static_cast<int>(x / kWindowScale),
                          static_cast<int>(y / kWindowScale)};
    for (const auto &control : mountainGame->controls) {
        if (!control.active) continue;
        const auto found = mountainGame->controlTextures.find(control.traced);
        int w = 0;
        int h = 0;
        if (found != mountainGame->controlTextures.end())
            SDL_QueryTexture(found->second.get(), nullptr, nullptr, &w, &h);
        const SDL_Rect bounds{control.x, control.y, w, h};
        if (SDL_PointInRect(&point, &bounds))
            return std::pair{static_cast<std::uint32_t>(control.id),
                             windowRect(control.x, control.y, w, h)};
    }
    return std::nullopt;
}

bool MountainScene::beginMountainInteraction(int x, int y) {
    if (!mountainGame) return false;
    const auto hit = mountainHotspotAt(x, y);
    if (!hit) return false;
    auto &game = *mountainGame;
    game.pressedControl = static_cast<int>(hit->first);
    if (auto *control = findMountainControl(game.pressedControl);
        control && !control->radio)
        setMountainControlFrame(*control, 1);
    return true;
}

bool MountainScene::endMountainInteraction(int x, int y) {
    if (!mountainGame || mountainGame->pressedControl == 0) return false;
    auto &game = *mountainGame;
    const int id = game.pressedControl;
    game.pressedControl = 0;
    auto *control = findMountainControl(id);
    if (!control) return true;
    if (!control->radio) setMountainControlFrame(*control, 0);
    const auto hit = mountainHotspotAt(x, y);
    if (!hit || static_cast<int>(hit->first) != id) return true;
    if (control->radio) {
        // UNCERTAIN: the shared control code's radio ordering; the others
        // in group 1 are cleared before the clicked one is checked.
        for (auto &other : game.controls)
            if (other.active && other.radio && other.id != id)
                setMountainControlFrame(other, 0);
        setMountainControlFrame(*control, 1);
    }
    // 0x00448228.
    switch (id) {
    case kControlEasy: game.difficulty = 0; break;
    case kControlMedium: game.difficulty = 1; break;
    case kControlHard: game.difficulty = 2; break;
    case kControlStart: startMountainRun(); break;
    case kControlUp:
        game.track = wrapIndex(game.track, static_cast<int>(game.tracks.size()), 1);
        readMountainTrackKeys();
        break;
    case kControlDown:
        game.track = wrapIndex(game.track, static_cast<int>(game.tracks.size()), -1);
        readMountainTrackKeys();
        break;
    default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame handler 0x004446b2

void MountainScene::updateMountain(double) {
    if (!mountainGame || activeScene != "mountain") return;
    if (!mountainGame->initialized) {
        // Message 0x103 reaches 0x00440d6e after the scene switch.
        mountainGame->initialized = true;
        initializeMountainScene();
        return;
    }
    if (mountainGame->gameOverModal || dialogMode != DialogMode::None) return;
    mountainFrame();
}

void MountainScene::mountainFrame() {
    auto &game = *mountainGame;
    if (game.attract) return;
    if (!game.running) {
        startMountainRun();
        return;
    }
    auto &player = *game.player;
    game.frameMs = mountainClock();
    game.dt = static_cast<float>(game.frameMs - game.previousFrameMs) * 0.001F;
    game.previousFrameMs = game.frameMs;
    // 0x0040ca60 polls GetAsyncKeyState for each virtual key.
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    const double now = sceneElapsedNow();
    const auto held = [&](SDL_Scancode code) {
        const std::size_t forced = code == SDL_SCANCODE_LEFT ? 0
                                   : code == SDL_SCANCODE_RIGHT ? 1
                                   : code == SDL_SCANCODE_UP ? 2
                                   : code == SDL_SCANCODE_DOWN ? 3 : 4;
        return (keys && keys[code]) || now < game.developerKeysUntil[forced];
    };
    if (held(SDL_SCANCODE_UP)) player.upTime = game.frameMs;
    if (held(SDL_SCANCODE_RIGHT)) mountainSpeedRight();
    else if (held(SDL_SCANCODE_LEFT)) mountainSpeedLeft();
    else mountainSpeedCruise();
    if (held(SDL_SCANCODE_DOWN)) {
        mountainDuckStart();
    } else {
        if (player.duckClip && !player.crashed) {
            mountainClipPlayNow(
                game.playerClips[static_cast<std::size_t>(MountainClipSlot::Cycle)],
                true);
            player.duckClip = false;
        }
        player.ducking = false;
    }
    mountainGravityResidual();
    mountainSpeedClamp();
    if (!mountainUpdatePlayer()) {
        // 0x004447fb: attract once the (blocking) score table returns.
        return;
    }
    mountainCameraFollow();
    if (held(SDL_SCANCODE_SPACE)) mountainJumpStart();
    mountainPickups();
    // 0x004400e8's state effects: camera clamp, obstacle list, rider clip.
    collectMountainSolids();
    advanceMountainPlayerClip();
    mountainUpdateHud();
    if (mountainFinishCheck()) {
        game.subgameInProgress = false;
        game.running = false;
        mountainEndOfGame();
        return;
    }
}

float MountainScene::mountainPlayerSpeed() const {
    // 0x00446536.
    const auto &player = *mountainGame->player;
    const float square = player.vx * player.vx + player.vy * player.vy;
    return square < 1e-6F ? 0.0F
                          : static_cast<float>(std::sqrt(static_cast<double>(square)));
}

void MountainScene::mountainSetSpeed(float target) {
    // 0x00447b33.
    auto &player = *mountainGame->player;
    const float square = player.vx * player.vx + player.vy * player.vy;
    if (square < 1e-6F) return;
    const float magnitude =
        static_cast<float>(std::sqrt(static_cast<double>(square)));
    const float factor = target / magnitude;
    player.vx = factor * player.vx;
    player.vy = factor * player.vy;
}

void MountainScene::mountainSpeedCruise() {
    // 0x00447aa0.
    if (mountainGame->player->airborne) return;
    const float speed = mountainPlayerSpeed();
    const float cruise = kUnit * 14.0F;
    if (cruise > speed || cruise < speed) mountainSetSpeed(cruise);
}

void MountainScene::mountainSpeedLeft() {
    // 0x00447bdd.
    auto &player = *mountainGame->player;
    if (player.ducking || player.airborne) {
        mountainSpeedCruise();
        return;
    }
    const float speed = mountainPlayerSpeed();
    if (kUnit * 7.0F < speed) {
        const float value = speed - kUnit * 0.5F;
        mountainSetSpeed(kUnit * 7.0F < value ? value : kUnit * 7.0F);
    }
}

void MountainScene::mountainSpeedRight() {
    // 0x00447c97.
    auto &player = *mountainGame->player;
    if (player.ducking || player.airborne) {
        mountainSpeedCruise();
        return;
    }
    const float speed = mountainPlayerSpeed();
    if (kUnit * 21.0F > speed) {
        const float value = speed + kUnit * 0.5F;
        mountainSetSpeed(kUnit * 21.0F > value ? value : kUnit * 21.0F);
    }
}

void MountainScene::mountainSpeedClamp() {
    // 0x00447d51.
    if (mountainGame->player->airborne) return;
    const float speed = mountainPlayerSpeed();
    if (kUnit * 7.0F > speed) mountainSetSpeed(kUnit * 7.0F);
    else if (kUnit * 21.0F < speed) mountainSetSpeed(kUnit * 21.0F);
}

void MountainScene::mountainDuckStart() {
    // 0x00447de4.
    auto &game = *mountainGame;
    auto &player = *game.player;
    if (player.ducking || player.airborne || player.crashed) return;
    player.ducking = true;
    player.duckClip = true;
    mountainClipPlayNow(
        game.playerClips[static_cast<std::size_t>(MountainClipSlot::Duck)], true);
    mountainSpeedCruise();
}

void MountainScene::mountainGravityResidual() {
    // 0x00445440 (its result +0x18 has no reader in the physics update).
    auto &game = *mountainGame;
    auto &player = *game.player;
    if (player.edge < 0 || player.airborne) {
        player.gravityResidual = kUnit * 29.0F;
        return;
    }
    const auto &edge = game.map->edges[static_cast<std::size_t>(player.edge)];
    const float gy = kUnit * 29.0F * player.mass;
    const float dot = -gy * edge.ny;
    float py = dot * edge.ny;
    float px = dot * edge.nx;
    if (px < 0.0F) px = 0.0F;
    player.gravityResidual = (gy + py) / player.mass;
}

void MountainScene::mountainCameraFollow() {
    // 0x00445355.
    auto &game = *mountainGame;
    auto &map = *game.map;
    const auto &player = *game.player;
    if (static_cast<float>(map.camX) - player.x > 20.0F)
        map.camX = ftol(player.x + 20.0F);
    else if (static_cast<float>(map.camX) - player.x < -20.0F)
        map.camX = ftol(player.x) - 20;
    if (static_cast<float>(map.camY) - player.y > 50.0F)
        map.camY = ftol(player.y + 50.0F);
    else if (static_cast<float>(map.camY) - player.y < -50.0F)
        map.camY = ftol(player.y) - 50;
}

void MountainScene::mountainJumpStart() {
    // 0x0044554e.
    auto &game = *mountainGame;
    auto &player = *game.player;
    if (player.airborne || player.ducking || player.edge < 0 || player.crashed)
        return;
    player.gravityResidual = 0.0F;
    player.falling = false;
    player.ducking = false;
    player.duckClip = false;
    player.jumpAngle =
        game.map->edges[static_cast<std::size_t>(player.edge)].angle + 0.2F;
    player.jumpPressed = true;
    mountainBuildFlight();
    playMountainSound(game.soundJump, false);
}

void MountainScene::mountainBuildFlight() {
    // 0x004455ff: a 300-entry height table in 0.05 s steps.
    auto &game = *mountainGame;
    auto &player = *game.player;
    const float dv = kUnit * 29.0F / 20.0F;
    const float step = 0.05F;
    player.airborne = true;
    if (player.falling) return;
    player.launchVy = player.vy;
    player.flightT0 = static_cast<float>(mountainClock()) / 1000.0F;
    float velocity = player.jumpPressed ? -(kUnit * 12.5F) : player.vy;
    float y = player.y - kGround * 2.0F;
    for (auto &entry : game.flight) {
        entry = y;
        y = velocity * step + y;
        velocity = velocity + dv;
    }
}

float MountainScene::mountainFlightY(float t) const {
    // 0x004463a6.
    const auto &flight = mountainGame->flight;
    const float f = t * 20.0F;
    int index = ftol(f);
    if (index < 0) index = 0;
    if (index >= 0x12b) index = 0x12a;
    const float fraction = f - static_cast<float>(index);
    return (flight[static_cast<std::size_t>(index + 1)] -
            flight[static_cast<std::size_t>(index)]) *
               fraction +
           flight[static_cast<std::size_t>(index)];
}

bool MountainScene::mountainUpdatePlayer() {
    // 0x004456ff. Returns false once the game is over.
    auto &game = *mountainGame;
    auto &map = *game.map;
    auto &player = *game.player;
    const auto &edges = map.edges;
    const auto &nodes = map.nodes;
    player.x += game.dt * player.vx;
    if (player.airborne)
        player.y = mountainFlightY(static_cast<float>(mountainClock()) * 0.001F -
                                   player.flightT0);
    else
        player.y += game.dt * player.vy;

    // Follow the segment graph (0x00445780-0x004459b4). The guard stops a
    // zero-length self-loop (winter.fff node 333) from spinning forever.
    for (int guard = 0; player.edge >= 0 && guard < 4096; ++guard) {
        const int old = player.edge;
        const auto &oldEdge = edges[static_cast<std::size_t>(old)];
        const int side = classify(nodes, oldEdge, player.x);
        if (side == 1) break;
        if (side == 2) {
            player.edge = nodes[static_cast<std::size_t>(oldEdge.from)].in[0];
            if (player.edge >= 0) {
                const auto &edge = edges[static_cast<std::size_t>(player.edge)];
                if (!crossPositive(edge.dx, edge.dy, oldEdge.dx, oldEdge.dy) &&
                    !player.airborne) {
                    const float speed = mountainPlayerSpeed();
                    player.vx = speed * edge.dx;
                    player.vy = speed * edge.dy;
                }
            }
            continue;
        }
        const auto &end = nodes[static_cast<std::size_t>(oldEdge.to)];
        if (!end.links.empty()) {
            if (mountainClock() - player.upTime < 1000 && end.links.size() > 1)
                player.edge = end.out[1];
            else
                player.edge = end.out[0];
            if (player.edge >= 0) {
                const auto &edge = edges[static_cast<std::size_t>(player.edge)];
                if (crossPositive(edge.dx, edge.dy, oldEdge.dx, oldEdge.dy) &&
                    !player.airborne) {
                    player.vx = (player.vx * edge.dx + player.vy * edge.dy) *
                                edge.dx;
                    // 0x0044597d uses the new vx.
                    player.vy = (player.vx * edge.dx + player.vy * edge.dy) *
                                edge.dy;
                }
            }
        } else {
            player.edge = -1;
        }
    }

    mountainUpdateActors();

    if (player.edge >= 0 && player.edge != player.checkpoint &&
        nodes[static_cast<std::size_t>(
                  edges[static_cast<std::size_t>(player.edge)].from)]
                .type == 1)
        player.checkpoint = player.edge;

    const auto clip = [&](MountainClipSlot slot) {
        return game.playerClips[static_cast<std::size_t>(slot)];
    };
    if (player.edge >= 0) {
        const auto &edge = edges[static_cast<std::size_t>(player.edge)];
        const float ground = segmentY(nodes, edge, player.x);
        if (ground - player.y >= kGround) {
            mountainBuildFlight();
            if (!player.falling) {
                player.falling = true;
                player.ducking = false;
            }
        } else {
            if (player.airborne && !player.crashed && game.takeOff) {
                mountainClipPlayNow(clip(MountainClipSlot::Land), false);
                mountainClipEnqueue(clip(MountainClipSlot::Cycle), true);
                game.takeOff = false;
            }
            if (player.falling) {
                // 0x00446590 keeps vx.
                const float speed = mountainPlayerSpeed();
                player.vy = speed * edge.dy;
                stopMountainSound(game.soundJump);
            }
            player.airborne = false;
            player.falling = false;
            if (player.jumpPressed && !player.crashed)
                stopMountainSound(game.soundJump);
            if (game.soundGround >= 0 && !player.crashed &&
                !mountainSoundPlaying(game.soundGround))
                playMountainSound(game.soundGround, true);
            player.jumpPressed = false;
        }
        if (ground - player.y < 10.0F) {
            if (mountainSoundPlaying(game.soundDuringJump)) {
                stopMountainSound(game.soundDuringJump);
                playMountainSound(game.soundRoll, true);
            }
        } else {
            if (mountainSoundPlaying(game.soundRoll)) {
                stopMountainSound(game.soundRoll);
                playMountainSound(game.soundDuringJump, true);
            }
            if (!game.takeOff && !player.crashed) {
                mountainClipPlayNow(clip(MountainClipSlot::Jump), false);
                mountainClipEnqueue(clip(MountainClipSlot::InAir), true);
                game.takeOff = true;
            }
            if (game.soundGround >= 0 && mountainSoundPlaying(game.soundGround))
                stopMountainSound(game.soundGround);
        }
        if (player.y - kGround > ground) {
            player.y = ground;
            player.airborne = false;
        }
        const float step = kUnit * 0.1F * game.dt;
        if (!player.airborne) {
            if (player.angle < edge.angle)
                player.angle = std::min(player.angle + step, edge.angle);
            if (player.angle > edge.angle)
                player.angle = std::max(player.angle - step, edge.angle);
        } else if (player.jumpPressed && player.angle < kLevelAngle) {
            player.angle = std::min(player.angle + step, kLevelAngle);
        }
    }
    // The rolling sound's pitch (0x004141c0) and the truck's distance fade
    // are not modelled: the native mixer has neither pitch nor live volume.

    mountainBuildHitbox();
    const int hit = mountainCollide();
    if (hit != 0) {
        player.crashed = true;
        player.crashClipStarted = false;
        player.crashType = hit;
        traceMountainState("crash", "kind", hit);
        stopMountainSound(game.soundRoll);
        stopMountainSound(game.soundDuringJump);
        stopMountainSound(game.soundJump);
        stopMountainSound(game.soundGround);
    }
    if (player.crashed) {
        if (!player.airborne) {
            player.vx = 0.0F;
            player.vy = 0.0F;
        }
        if (!player.crashClipStarted) {
            if (player.crashType == 1) {
                mountainClipPlayNow(clip(MountainClipSlot::Crash2), false);
            } else if (player.crashType == 2) {
                mountainClipPlayNow(clip(MountainClipSlot::Crash1), false);
                player.vx = 0.0F;
                player.vy = 0.0F;
            }
            player.crashClipStarted = true;
            return true;
        }
        const auto atEnd = [&](int index) {
            const auto &crash = game.clips[static_cast<std::size_t>(index)];
            return crash.frame >=
                   static_cast<int>(crash.media.videoFrames.size()) - 1;
        };
        if (atEnd(clip(MountainClipSlot::Crash2)) ||
            atEnd(clip(MountainClipSlot::Crash1))) {
            game.clips[static_cast<std::size_t>(clip(MountainClipSlot::Crash2))]
                .frame = 0;
            game.clips[static_cast<std::size_t>(clip(MountainClipSlot::Crash1))]
                .frame = 0;
            if (mountainLoseLife()) {
                traceMountainState("respawn", "lives", player.lives);
                beginMountainLife();
            } else {
                traceMountainState("game_over", "score", player.score);
                mountainGameOver();
                return false;
            }
        }
    }
    return true;
}

void MountainScene::mountainUpdateActors() {
    // 0x0044733f.
    auto &game = *mountainGame;
    auto &map = *game.map;
    const auto &player = *game.player;
    for (std::size_t index = 0; index < map.actors.size(); ++index) {
        auto &actor = map.actors[index];
        if (actor.active) {
            if (actor.edge >= 0 &&
                static_cast<float>(map.camX - 0x280) <
                    map.nodes[static_cast<std::size_t>(
                                  map.edges[static_cast<std::size_t>(actor.edge)].to)]
                        .x) {
                const auto &edge = map.edges[static_cast<std::size_t>(actor.edge)];
                actor.x = actor.x - game.dt * edge.dx * actor.speed;
                actor.y = actor.y - game.dt * edge.dy * actor.speed;
                const float step = kUnit * 0.1F * game.dt;
                if (actor.angle < edge.angle)
                    actor.angle = std::min(actor.angle + step, edge.angle);
                if (actor.angle > edge.angle)
                    actor.angle = std::max(actor.angle - step, edge.angle);
                switch (classify(map.nodes, edge, actor.x)) {
                case 1: break;
                case 2:
                    actor.edge = map.nodes[static_cast<std::size_t>(edge.from)].in[0];
                    break;
                default: actor.edge = -1; break;
                }
                if (actor.hit && actor.fallClip >= 0 &&
                    game.clips[static_cast<std::size_t>(actor.walkClip)].playing) {
                    stopMountainClip(actor.walkClip);
                    startMountainClip(actor.fallClip);
                }
                advanceMountainClip(actor.hit && actor.fallClip >= 0
                                        ? actor.fallClip : actor.walkClip);
            } else {
                actor.active = false;
                if (actor.sound >= 0) stopMountainSound(actor.sound);
            }
        } else if (player.edge >= 0 &&
                   map.edges[static_cast<std::size_t>(player.edge)].from ==
                       actor.triggerNode) {
            const int chosenTruck = map.trucks.empty() ||
                    map.truckChoice >= static_cast<int>(map.trucks.size())
                ? -1 : map.trucks[static_cast<std::size_t>(map.truckChoice)];
            const int chosenWalker = map.walkers.empty() ||
                    map.walkerChoice >= static_cast<int>(map.walkers.size())
                ? -1 : map.walkers[static_cast<std::size_t>(map.walkerChoice)];
            if (static_cast<int>(index) == chosenTruck ||
                static_cast<int>(index) == chosenWalker) {
                startMountainClip(actor.walkClip);
                actor.active = true;
                if (actor.sound >= 0 && !mountainSoundPlaying(actor.sound))
                    playMountainSound(actor.sound, true);
            }
        }
    }
}

void MountainScene::mountainBuildHitbox() {
    // 0x00446619.
    auto &player = *mountainGame->player;
    const float c = static_cast<float>(
        std::cos(-static_cast<double>(player.angle) - 1.57));
    const float s = static_cast<float>(
        std::sin(-static_cast<double>(player.angle) - 1.57));
    const auto &shape = player.ducking ? kDuckHitbox
                        : player.airborne ? kAirHitbox
                                          : kRideHitbox;
    for (std::size_t index = 0; index < 5; ++index) {
        auto &segment = player.hitbox[index];
        rotate(&segment[0], &shape[index][0], c, s);
        rotate(&segment[2], &shape[index][2], c, s);
        segment[0] += player.x;
        segment[1] += player.y;
        segment[2] += player.x;
        segment[3] += player.y;
    }
}

void MountainScene::mountainBuildActorHitbox(MountainActor &actor) {
    // 0x00446c83.
    const float c = static_cast<float>(
        std::cos(-static_cast<double>(actor.angle) - 1.57));
    const float s = static_cast<float>(
        std::sin(-static_cast<double>(actor.angle) - 1.57));
    for (std::size_t index = 0; index < 6; ++index) {
        const float *shape = actor.outline + index * 4;
        auto &segment = actor.hitbox[index];
        if (actor.upright) {
            segment = {actor.x + shape[0], actor.y + shape[1],
                       actor.x + shape[2], actor.y + shape[3]};
        } else {
            rotate(&segment[0], &shape[0], c, s);
            rotate(&segment[2], &shape[2], c, s);
            segment[0] += actor.x;
            segment[1] += actor.y;
            segment[2] += actor.x;
            segment[3] += actor.y;
        }
    }
}

int MountainScene::mountainCollide() {
    // 0x0044680b.
    auto &game = *mountainGame;
    auto &map = *game.map;
    auto &player = *game.player;
    if (player.crashed) return 0;
    ++game.collisionStamp;
    bool anyTile = false;
    for (const auto &solid : map.solids) {
        const int rect[4]{solid.left, solid.top, solid.right, solid.bottom};
        bool touch = false;
        for (const auto &segment : player.hitbox)
            if (pointInRect(rect, segment.data())) touch = true;
        for (const auto &segment : player.hitbox) {
            if (segmentHitsRect(segment.data(), rect)) {
                touch = true;
                break;
            }
        }
        if (touch) {
            mountainStampTile(solid);
            anyTile = true;
        }
    }
    if (anyTile) {
        const int ox = map.camX - 0xa0;
        const int oy = map.camY - 0xf0;
        for (int i = 0; i < 5; ++i) {
            const auto &s = player.hitbox[static_cast<std::size_t>(i)];
            if (mountainLineHitsStamp(ftol(s[0]) - ox, ftol(s[1]) - oy,
                                      ftol(s[2]) - ox, ftol(s[3]) - oy))
                return (i > 0 && i < 3) ? 2 : 1;
        }
        for (int j = 0; j < 5; ++j) {
            for (int i = 0; i < 5; ++i) {
                if (i == j) continue;
                const auto &a = player.hitbox[static_cast<std::size_t>(i)];
                const auto &b = player.hitbox[static_cast<std::size_t>(j)];
                if (mountainLineHitsStamp(ftol(a[0]) - ox, ftol(a[1]) - oy,
                                          ftol(b[0]) - ox, ftol(b[1]) - oy))
                    return (i > 0 && i < 3) ? 2 : 1;
            }
        }
    }
    for (auto &actor : map.actors) {
        if (!actor.active) continue;
        mountainBuildActorHitbox(actor);
        for (const auto &segment : player.hitbox) {
            for (const auto &edge : actor.hitbox) {
                if (segmentsIntersect(edge.data(), segment.data())) {
                    if (actor.fallClip >= 0) actor.speed = 0.0F;
                    actor.hit = true;
                    return 1;
                }
            }
        }
    }
    return 0;
}

void MountainScene::mountainStampTile(const MountainSolid &solid) {
    // 0x004471a3, including its x clamp to 479 and the unshifted source
    // pixel for tiles clipped at the top or left.
    auto &game = *mountainGame;
    const auto &map = *game.map;
    if (solid.tile < 0) return;
    const auto &tile = map.tiles[static_cast<std::size_t>(solid.tile)];
    const auto clamp0 = [](int value, int high) {
        return value < 0 ? 0 : value > high ? high : value;
    };
    const int cameraLeft = map.camX - 160;
    const int cameraTop = map.camY - 0xf0;
    const int sy = solid.top - cameraTop;
    const int y0 = clamp0(sy, 0x1df);
    const int y1 = clamp0(sy + 0x20, 0x1df);
    if (y0 >= y1) return;
    const int sx = solid.left - cameraLeft;
    const int x0 = clamp0(sx, 0x1df);
    const int x1 = clamp0(sx + 0x20, 0x27f);
    if (x0 >= x1) return;
    for (int y = y0; y < y1; ++y) {
        const int row = y - y0;
        for (int x = x0; x < x1; ++x) {
            const int column = x - x0;
            const bool opaque = row < 32 && column < 32
                ? tile.opaque[static_cast<std::size_t>(row * 32 + column)]
                : false;
            if (opaque)
                game.stamp[static_cast<std::size_t>(y * kWidth + x)] =
                    static_cast<std::uint32_t>(game.collisionStamp);
        }
    }
}

bool MountainScene::mountainLineHitsStamp(int x0, int y0, int x1,
                                          int y1) const {
    // 0x00447056: n pixels from the start, bounds 639 x 479 exclusive.
    const auto &game = *mountainGame;
    int adx;
    int ady;
    int sx;
    int sy;
    if (x0 < x1) { adx = x1 - x0; sx = 1; } else { adx = x0 - x1; sx = -1; }
    if (y0 < y1) { ady = y1 - y0; sy = 1; } else { ady = y0 - y1; sy = -1; }
    const int n = std::max(adx, ady);
    if (n == 0) return false;
    int ey = n >> 1;
    int ex = ey;
    for (int i = 0; i < n; ++i) {
        if (x0 >= 0 && x0 < 0x27f && y0 >= 0 && y0 < 0x1df &&
            game.stamp[static_cast<std::size_t>(y0 * kWidth + x0)] ==
                static_cast<std::uint32_t>(game.collisionStamp))
            return true;
        ex += adx;
        if (ex >= n) { ex -= n; x0 += sx; }
        ey += ady;
        if (ey >= n) { ey -= n; y0 += sy; }
    }
    return false;
}

bool MountainScene::mountainLoseLife() {
    // 0x00446434.
    auto &game = *mountainGame;
    stopMountainSound(game.soundRoll);
    stopMountainSound(game.soundDuringJump);
    stopMountainSound(game.soundGround);
    --game.player->lives;
    traceMountainState("life_lost", "lives", game.player->lives);
    updateMountainHeads();
    return game.player->lives > 0;
}

void MountainScene::mountainGameOver() {
    // 0x00446414.
    auto &game = *mountainGame;
    game.subgameInProgress = false;
    game.running = false;
    mountainEndOfGame();
}

void MountainScene::mountainEndOfGame() {
    // 0x00447e43: persist the film/dings flags, then the score table.
    auto &game = *mountainGame;
    auto &player = *game.player;
    if (profileIndex < profiles.size() && profiles[profileIndex] != "No profile") {
        const fs::path path = profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(path)) {
            try {
                std::vector<std::uint8_t> bytes(8);
                std::memcpy(bytes.data(), player.collected.data(), 8);
                setProfileSceneRecordBytes(path, "mountain", 0x402U, 0U, bytes);
            } catch (const std::exception &error) {
                std::cerr << "Could not record Mountain collectibles: "
                          << error.what() << '\n';
            }
        }
    }
    // UNCERTAIN: the award popup 0x0041bb20 for newly collected film or
    // dings is not reproduced.
    const std::string section =
        game.track >= 0 && game.track < static_cast<int>(game.tracks.size())
            ? game.tracks[static_cast<std::size_t>(game.track)].first : "";
    const auto colon = section.find(':');
    const std::string titleKey =
        colon == std::string::npos ? "SCENENAME" : section.substr(colon + 1);
    const auto listKey = static_cast<std::uint32_t>(std::max(0, game.track));
    if (tournament.active) {
        // 0x0041f8c0's tournament branch: no popup, so no 0x11b freeze.
        showHighScore(player.score, titleKey, listKey);
        return;
    }
    // 0x0043ff03: the popup's 0x11b freezes the view into a 640x480 text
    // control at (0,0) layer 1 while a run is in progress.
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", 0}, {"y", 0}, {"layer", 1}, {"hotspot", 0},
        {"conflict_mask", 0}});
    game.gameOverModal = true;
    showHighScore(player.score, titleKey, listKey);
}

void MountainScene::finishMountainGameOver() {
    // 0x00440053 removes the frozen view; the frame handler then rebuilds
    // the attract screen (0x004447fb -> 0x00444431) unless a finish moved
    // the scene on to town (0x00444877).
    if (!mountainGame || !mountainGame->gameOverModal) return;
    auto &game = *mountainGame;
    game.gameOverModal = false;
    traceMountainRemove("");
    if (game.player && game.player->finished) {
        enterScene("town", "town");
        return;
    }
    enterMountainAttract();
}

void MountainScene::mountainPickups() {
    // 0x004448f1.
    auto &game = *mountainGame;
    auto &map = *game.map;
    auto &player = *game.player;
    mountainSlotsCheck();
    for (std::size_t index = 0; index < map.items.size(); ++index) {
        auto &item = map.items[index];
        if (!item.active) continue;
        const int rect[4]{item.left, item.top, item.right, item.bottom};
        bool hit = false;
        for (const auto &segment : player.hitbox) {
            if (pointInRect(rect, segment.data()) ||
                segmentHitsRect(segment.data(), rect)) {
                hit = true;
                break;
            }
        }
        if (!hit) continue;
        item.active = false;
        if (item.kind < 0 || item.kind >= 7) continue;
        const auto &type = kPickups[static_cast<std::size_t>(item.kind)];
        mountainAddScore(type.score);
        // Logged where 0x00444a55's call returns (0x00444a5a).
        traceMountainPickup(item.kind, static_cast<int>(index), player.score);
        if (type.flag != -1)
            player.collected[static_cast<std::size_t>(type.flag)] = 1;
        if (type.slot != -1) mountainSlotAdd(type.slot);
        playMountainSoundPooled(game.pickupSounds[static_cast<std::size_t>(item.kind)]);
    }
}

void MountainScene::mountainAddScore(int points) {
    // 0x00444bc9.
    auto &game = *mountainGame;
    auto &player = *game.player;
    player.score += ftol(static_cast<float>(points) *
                         kDifficultyScale[static_cast<std::size_t>(game.difficulty)]);
}

void MountainScene::mountainSlotAdd(int item) {
    // 0x004450e3.
    auto &game = *mountainGame;
    auto &player = *game.player;
    for (std::size_t index = 0; index < 3; ++index) {
        if (player.slots[index] >= 0) continue;
        player.slots[index] = item;
        if (game.slotIconsAdded[index]) traceMountainRemove(game.slotIconNames[index]);
        game.slotIconNames[index] = kSlotIcons[static_cast<std::size_t>(item)];
        game.slotIconsAdded[index] = true;
        traceMountainAdd(game.slotIconNames[index], -1, -1, 1);
        return;
    }
}

void MountainScene::mountainSlotsCheck() {
    // 0x004451b7.
    auto &game = *mountainGame;
    auto &player = *game.player;
    for (const int slot : player.slots)
        if (slot < 0) return;
    playMountainSoundPooled(game.soundFanfare);
    const int a = player.slots[0];
    const int b = player.slots[1];
    const int c = player.slots[2];
    if (a == b && a == c) {
        for (const auto &type : kPickups) {
            if (type.slot == a) {
                mountainAddScore(type.score * 10);
                break;
            }
        }
    } else if (a != b && b != c && a != c) {
        mountainAddScore(1500);
    } else {
        mountainAddScore(500);
    }
    for (std::size_t index = 0; index < 3; ++index) {
        player.slots[index] = -1;
        if (game.slotIconsAdded[index]) {
            traceMountainRemove(game.slotIconNames[index]);
            game.slotIconsAdded[index] = false;
        }
    }
}

bool MountainScene::mountainFinishCheck() {
    // 0x00447f43.
    auto &game = *mountainGame;
    auto &player = *game.player;
    const auto &map = *game.map;
    if (!player.started && static_cast<float>(map.startX) <= player.x) {
        player.started = true;
        player.startedAt = mountainClock();
        player.finished = false;
    } else if (!player.finished && static_cast<float>(map.finishX) <= player.x) {
        player.finishedAt = mountainClock();
        player.finished = true;
        traceMountainState("finish", "score", player.score);
    }
    return player.finished;
}

float MountainScene::mountainElapsedSeconds() const {
    // 0x0044814f / 0x00448197.
    const auto &player = *mountainGame->player;
    if (player.finished)
        return static_cast<float>(player.finishedAt - player.startedAt) / 1000.0F;
    if (player.started)
        return static_cast<float>(mountainClock() - player.startedAt) / 1000.0F;
    return 0.0F;
}

void MountainScene::mountainUpdateHud() {
    // 0x00447fda.
    auto &game = *mountainGame;
    const double elapsed = mountainElapsedSeconds();
    const int centiseconds = ftol(std::fmod(elapsed, 1.0) * 100.0);
    const int seconds = ftol(mountainElapsedSeconds());
    char time[32];
    std::snprintf(time, sizeof(time), "%d:%02d", seconds, centiseconds);
    game.timeText.text = time;
    game.timeText.flags = 0x201;
    game.scoreText.text = std::to_string(game.player->score);
    game.scoreText.flags = 0x201;
    // 0x004480a7.
    const auto &map = *game.map;
    float fraction = 0.0F;
    if (game.running) {
        fraction = (game.player->x - static_cast<float>(map.startX)) /
                   static_cast<float>(map.finishX - map.startX);
        fraction = std::clamp(fraction, 0.0F, 1.0F);
    }
    game.progress = fraction;
}

void MountainScene::collectMountainSolids() {
    // 0x004400e8 (state part): camera clamp 0x00440ae7 and the obstacle
    // list from layer +0x24 for the next frame's collision.
    auto &game = *mountainGame;
    auto &map = *game.map;
    if (map.camX < 0x140) map.camX = 0x140;
    else if (map.camX >= map.widthPx - 0x140) map.camX = map.widthPx - 0x140;
    if (map.camY < 0xf0) map.camY = 0xf0;
    else if (map.camY >= map.heightPx - 0xf0) map.camY = map.heightPx - 0xf0;
    const int left = map.camX - 0xa0;
    const int top = map.camY - 0xf0;
    const int tx = left / 32;
    const int ty = top / 32;
    map.solids.clear();
    const auto &solidLayer = map.layers[1];
    for (int row = 0; row <= 15; ++row) {
        for (int column = 0; column <= 20; ++column) {
            const int cx = tx + column;
            const int cy = ty + row;
            if (cx < 0 || cy < 0 || cx >= map.widthTiles || cy >= map.heightTiles)
                continue;
            const std::uint16_t value =
                solidLayer[static_cast<std::size_t>(cy) *
                               static_cast<std::size_t>(map.widthTiles) +
                           static_cast<std::size_t>(cx)];
            const auto signedValue = static_cast<std::int16_t>(value);
            if (signedValue < 0 ||
                signedValue >= static_cast<int>(map.tiles.size()))
                continue;
            MountainSolid solid;
            solid.left = cx * 32;
            solid.top = cy * 32;
            solid.right = solid.left + 32;
            solid.bottom = solid.top + 32;
            solid.tile = signedValue;
            map.solids.push_back(solid);
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing

void MountainScene::drawMountainSprite(MountainClip &clip, float cx, float cy,
                                       float angle, float scale, float halfW,
                                       float halfH, float width, float height) {
    // 0x0046fdc0: texture x runs along (cos t, sin t), t = -(angle + pi),
    // from -0.3*halfW to +1.7*halfW; texture y along (-sin t, cos t) from
    // -0.3*halfH to +1.7*halfH. The key is the frame's first pixel.
    if (!clip.texture || clip.media.videoFrames.empty()) return;
    const int index = std::clamp(clip.frame, 0,
        static_cast<int>(clip.media.videoFrames.size()) - 1);
    uploadVideoFrame(clip.texture.get(),
                     clip.media.videoFrames[static_cast<std::size_t>(index)]);
    const float t = -(angle + kPi);
    const SDL_Rect source{0, 0, static_cast<int>(width), static_cast<int>(height)};
    const SDL_FRect destination{cx - 0.3F * halfW * scale,
                                cy - 0.3F * halfH * scale,
                                2.0F * halfW * scale, 2.0F * halfH * scale};
    const SDL_FPoint center{0.3F * halfW * scale, 0.3F * halfH * scale};
    SDL_RenderCopyExF(renderer, clip.texture.get(), &source, &destination,
                      static_cast<double>(t) * 180.0 / M_PI, &center,
                      SDL_FLIP_NONE);
}

void MountainScene::drawMountainText(const MountainText &text) {
    if (!text.active || text.text.empty()) return;
    const int width = textWidth(dialogFont, text.text);
    const int y = text.rect.y + (text.rect.h - dialogFont.height) / 2;
    int x = text.rect.x + (text.rect.w - width) / 2;
    if (text.flags & 0x200) x = text.rect.x + text.rect.w - width;
    drawText(renderer, dialogFont, text.text, x, y);
}

void MountainScene::renderMountain(double) {
    if (!mountainGame || activeScene != "mountain") return;
    auto &game = *mountainGame;
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    if (game.frameTarget) {
        SDL_SetRenderTarget(renderer, game.frameTarget.get());
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
    }
    const bool playing = !game.attract && game.map && game.player;
    if (playing) {
        auto &map = *game.map;
        auto &player = *game.player;
        // 0x004400e8: layers A (+0x20, opaque), B (+0x2c), C (+0x24), the
        // rider, actors, pickups, then layer D (+0x28), clipped to y >= 80.
        const SDL_Rect clip{0, 80, kWidth, kHeight - 80};
        SDL_RenderSetClipRect(renderer, &clip);
        const int left = map.camX - 0xa0;
        const int top = map.camY - 0xf0;
        const int tx = left / 32;
        const int ty = top / 32;
        const int fx = left % 32;
        const int fy = top % 32;
        const auto drawLayer = [&](std::size_t layer, bool keyed) {
            SDL_SetTextureBlendMode(map.tileTexture.get(),
                                    keyed ? SDL_BLENDMODE_BLEND
                                          : SDL_BLENDMODE_NONE);
            for (int row = 0; row <= 15; ++row) {
                for (int column = 0; column <= 20; ++column) {
                    const int cx = tx + column;
                    const int cy = ty + row;
                    if (cx < 0 || cy < 0 || cx >= map.widthTiles ||
                        cy >= map.heightTiles)
                        continue;
                    const auto value = static_cast<std::int16_t>(
                        map.layers[layer][static_cast<std::size_t>(cy) *
                                              static_cast<std::size_t>(map.widthTiles) +
                                          static_cast<std::size_t>(cx)]);
                    if (value < 0 || value >= static_cast<int>(map.tiles.size()))
                        continue;
                    const SDL_Rect source{(value % map.tileColumns) * 32,
                                          (value / map.tileColumns) * 32, 32, 32};
                    const SDL_Rect destination{column * 32 - fx, row * 32 - fy,
                                               32, 32};
                    SDL_RenderCopy(renderer, map.tileTexture.get(), &source,
                                   &destination);
                }
            }
            SDL_SetTextureBlendMode(map.tileTexture.get(), SDL_BLENDMODE_BLEND);
        };
        drawLayer(0, false);
        drawLayer(3, true);
        drawLayer(1, true);
        SDL_RenderSetClipRect(renderer, nullptr);
        if (!mountainClipQueueEmpty()) {
            const int clipIndex =
                player.queue[static_cast<std::size_t>(player.queueTail % 16)].first;
            drawMountainSprite(game.clips[static_cast<std::size_t>(clipIndex)],
                               player.x - static_cast<float>(left),
                               player.y - static_cast<float>(top), player.angle,
                               0.6F, player.spriteHalfW, player.spriteHalfH,
                               player.spriteW, player.spriteH);
        }
        // 0x00440b66.
        for (auto &actor : map.actors) {
            if (!actor.active) continue;
            const int clipIndex =
                actor.hit && actor.fallClip >= 0 ? actor.fallClip : actor.walkClip;
            auto &actorClip = game.clips[static_cast<std::size_t>(clipIndex)];
            if (!actor.upright)
                drawMountainSprite(actorClip, actor.x - static_cast<float>(left),
                                   actor.y - static_cast<float>(top), actor.angle,
                                   0.6F, actor.spriteHalfW, actor.spriteHalfH,
                                   actor.spriteW, actor.spriteH);
            else
                drawMountainSprite(actorClip, actor.x - static_cast<float>(left),
                                   actor.y - static_cast<float>(top) + 60.0F,
                                   -1.5707964F, 0.75F, actor.spriteHalfW,
                                   actor.spriteHalfH, actor.spriteW,
                                   actor.spriteH);
        }
        SDL_RenderSetClipRect(renderer, &clip);
        // 0x00440713: pickups, one frame per 66 ms tick.
        const int ticks = mountainTicks();
        for (const auto &item : map.items) {
            if (!item.active || item.kind < 0 || item.kind >= 7) continue;
            int frame = ticks - item.startTick;
            if (frame < 0) frame = 0;
            frame %= 32;
            const SDL_Rect source{frame * 32, 0, 32, 32};
            const SDL_Rect destination{item.x - left - 16, item.y - top - 16, 32, 32};
            SDL_RenderCopy(renderer,
                           game.pickupTextures[static_cast<std::size_t>(item.kind)].get(),
                           &source, &destination);
        }
        drawLayer(2, true);
        SDL_RenderSetClipRect(renderer, nullptr);
    }
    // Scene anims by layer: interface (0); HUD text, progress, heads and
    // slot icons (1); controls (2); attract text (3).
    if (game.interfaceAdded && game.interfaceTexture) {
        const SDL_Rect bounds{0, 0, kWidth, 80};
        SDL_RenderCopy(renderer, game.interfaceTexture.get(), nullptr, &bounds);
    }
    if (game.startInterfaceAdded) {
        const SDL_Rect bounds{200, 162, 232, 133};
        SDL_RenderCopy(renderer, game.startInterfaceTexture.get(), nullptr, &bounds);
    }
    drawMountainText(game.timeText);
    drawMountainText(game.scoreText);
    if (game.progressAdded) {
        const int columns = static_cast<int>(std::lround(640.0F * game.progress));
        if (columns > 0) {
            const SDL_Rect source{0, 0, columns, 3};
            const SDL_Rect destination{0, 74, columns, 3};
            SDL_RenderCopy(renderer, game.progressTexture.get(), &source, &destination);
        }
    }
    for (std::size_t index = 0; index < 3; ++index) {
        if (game.headsAdded[index]) {
            const SDL_Rect bounds{kHeads[index].x, kHeads[index].y, 52, 40};
            SDL_RenderCopy(renderer, game.headTexture.get(), nullptr, &bounds);
        }
        if (game.slotIconsAdded[index]) {
            std::size_t icon = 0;
            for (std::size_t name = 0; name < kSlotIcons.size(); ++name)
                if (game.slotIconNames[index] == kSlotIcons[name]) icon = name;
            const SDL_Rect bounds{kSlotIconPositions[index].x,
                                  kSlotIconPositions[index].y, 32, 32};
            SDL_RenderCopy(renderer, game.slotIconTextures[icon].get(), nullptr,
                           &bounds);
        }
    }
    for (const auto &control : game.controls) {
        if (!control.active) continue;
        const auto found = game.controlTextures.find(control.traced);
        if (found == game.controlTextures.end()) continue;
        int w = 0;
        int h = 0;
        SDL_QueryTexture(found->second.get(), nullptr, nullptr, &w, &h);
        const SDL_Rect bounds{control.x, control.y, w, h};
        SDL_RenderCopy(renderer, found->second.get(), nullptr, &bounds);
    }
    for (const MountainText *text : {&game.easyText, &game.mediumText,
                                     &game.hardText, &game.startText,
                                     &game.trackText}) {
        drawMountainText(*text);
    }
    if (game.frameTarget && !game.developerSnapshot.empty()) {
        SurfacePtr shot(SDL_CreateRGBSurfaceWithFormat(
            0, kWidth, kHeight, 32, SDL_PIXELFORMAT_ARGB8888));
        if (shot && SDL_RenderReadPixels(renderer, nullptr,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         shot->pixels, shot->pitch) == 0)
            SDL_SaveBMP(shot.get(), game.developerSnapshot.c_str());
        game.developerSnapshot.clear();
    }
    if (game.frameTarget) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_RenderCopy(renderer, game.frameTarget.get(), nullptr, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Developer commands

bool MountainScene::executeMountainDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "mountain-game:";
    if (activeScene != "mountain" || !mountainGame) return false;
    auto &game = *mountainGame;
    // Wait for message 0x103 (the deferred init) before acting.
    if (!game.initialized) return false;
    const auto control = command.substr(prefix.size());
    const auto click = [&](int id) {
        if (const auto *found = findMountainControl(id)) {
            game.pressedControl = id;
            if (auto *pressed = findMountainControl(id); pressed && !pressed->radio)
                setMountainControlFrame(*pressed, 1);
            const int x = static_cast<int>(std::lround((found->x + 2) * kWindowScale));
            const int y = static_cast<int>(std::lround((found->y + 2) * kWindowScale));
            endMountainInteraction(x, y);
        }
    };
    if (control == "start") {
        click(kControlStart);
    } else if (control == "difficulty:easy") {
        click(kControlEasy);
    } else if (control == "difficulty:medium") {
        click(kControlMedium);
    } else if (control == "difficulty:hard") {
        click(kControlHard);
    } else if (control == "track:next") {
        click(kControlUp);
    } else if (control == "track:previous") {
        click(kControlDown);
    } else if (control.rfind("wait=", 0) == 0) {
        if (!game.developerWaitUntil)
            game.developerWaitUntil =
                sceneElapsedNow() + std::stod(std::string(control.substr(5)));
        if (sceneElapsedNow() < *game.developerWaitUntil) return false;
        game.developerWaitUntil.reset();
    } else if (control.rfind("snapshot=", 0) == 0) {
        game.developerSnapshot = std::string(control.substr(9));
    } else if (control.rfind("key=", 0) == 0) {
        // Holds a steering key for the given milliseconds (local checks).
        const auto values = std::string(control.substr(4));
        const auto comma = values.find(',');
        const std::string name = values.substr(0, comma);
        const double seconds = comma == std::string::npos
            ? 0.1 : std::stod(values.substr(comma + 1)) / 1000.0;
        const std::size_t slot = name == "LEFT" ? 0 : name == "RIGHT" ? 1
                                 : name == "UP" ? 2 : name == "DOWN" ? 3 : 4;
        game.developerKeysUntil[slot] = sceneElapsedNow() + seconds;
        if (slot == 2 && game.player) game.player->upTime = mountainClock();
    } else if (control.rfind("actors=", 0) == 0) {
        // Pins 0x004414cb/0x004414e6's rand() picks (truck, walker).
        const auto values = std::string(control.substr(7));
        const auto comma = values.find(',');
        game.pinnedTruck = std::stoi(values.substr(0, comma));
        game.pinnedWalker = comma == std::string::npos
            ? 0 : std::stoi(values.substr(comma + 1));
    } else if (control == "crash") {
        if (game.player && game.running && !game.player->crashed) {
            game.player->crashed = true;
            game.player->crashClipStarted = false;
            game.player->crashType = 1;
            traceMountainState("crash", "kind", 1);
        }
    } else if (control == "status") {
        std::cout << "Mountain status: track " << game.track << ", difficulty "
                  << game.difficulty + 1 << ", attract " << game.attract
                  << ", running " << game.running;
        if (game.player && game.map) {
            std::cout << ", score " << game.player->score << ", lives "
                      << game.player->lives << ", x " << game.player->x
                      << ", y " << game.player->y << ", edge "
                      << game.player->edge << ", checkpoint "
                      << game.player->checkpoint << ", crashed "
                      << game.player->crashed << ", airborne "
                      << game.player->airborne << ", items "
                      << game.map->items.size() << ", nodes "
                      << game.map->nodes.size() << ", actors "
                      << game.map->actors.size();
        }
        std::cout << '\n';
    } else {
        throw std::runtime_error("Unknown developer Mountain control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
