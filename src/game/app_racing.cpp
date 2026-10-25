#include "game/app_racing.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
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

constexpr int kWidth = 640;   // 0x00474907: 0x004017f0(0x280, 0x1e0)
constexpr int kHeight = 480;
constexpr float kCarSize = 0.85F;          // 0x0050a970
constexpr float kPi = 3.14159274F;         // 0x0049d620
constexpr float kHalfPi = 1.57079637F;     // 0x0049da38
constexpr double kCarOffset = 1.0 / 12.0;  // 0x004a0108 (double)
constexpr float kShadowOffset = 2.0F;      // 0x0049c788
constexpr float kFixed = 65536.0F;         // 0x0049da4c
constexpr int kAtlasWidth = 0x100;         // 0x00474e4a
constexpr int kTimerTwo = 20000;           // EVENT_AddTimer(now + 20000, 20000, 2)

// HUD record 0 (0x0050ace8), its initialiser's rectangles as left, top,
// right, bottom (+0x2a4..+0x344) and descriptor positions (+0x54/+0x58).
struct Box {
    int left;
    int top;
    int right;
    int bottom;
};
Box kBestLabel() { return OriginalData::instance().read<Box>(0x0050af8cU); }
Box kBestValue() { return OriginalData::instance().read<Box>(0x0050af9cU); }
Box kLapLabel() { return OriginalData::instance().read<Box>(0x0050afacU); }
Box kLapRow() { return OriginalData::instance().read<Box>(0x0050afbcU); } // Rows are 24 pixels apart.
Box kPositionLabel() { return OriginalData::instance().read<Box>(0x0050afccU); }
Box kPositionValue() { return OriginalData::instance().read<Box>(0x0050afdcU); }
Box kDamageLabel() { return OriginalData::instance().read<Box>(0x0050afecU); }
Box kDamageValue() { return OriginalData::instance().read<Box>(0x0050affcU); }
Box kSpeedLabel() { return OriginalData::instance().read<Box>(0x0050b00cU); }
Box kSpeedValue() { return OriginalData::instance().read<Box>(0x0050b01cU); }
Box kMessageLine() { return OriginalData::instance().read<Box>(0x0050b02cU); }
constexpr SDL_Point kPickupIconAt{588, 428};      // +0x58/+0x5c
constexpr SDL_Point kMessageClipAt{2, 362};       // +0x100/+0x104
constexpr SDL_Point kMinimapAt{2, 386};           // +0x1a8/+0x1ac
constexpr SDL_Point kCountAt{262, 182};           // +0x250/+0x254
// 0x004a0048: per detail level, best, lap times, position, damage, speed
// and the minimap.
const auto kHudLevels = originalArray<std::array<bool, 6>, 5>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x004a0048U + static_cast<std::uint32_t>(index * 24);
    std::array<bool, 6> result{};
    for (std::size_t field = 0; field < result.size(); ++field)
        result[field] = source.read<int>(address + static_cast<std::uint32_t>(4 * field)) != 0;
    return result;
});
// 0x004a0100, 0x004a00fc: the minimap's size in the dot formula.
constexpr float kMinimapWidth = 143.0F;
constexpr float kMinimapHeight = 92.0F;

// The display is 16 bits per pixel (0x00403db0 asks DirectDraw for
// 640x480x16); every blit lands in RGB565. Keys compare RGB565 words
// (0x0046f800 compares the atlas's top-left word).
std::uint32_t quantize565(std::uint32_t argb) {
    return (argb & 0x00f8fcf8U) | 0xff000000U;
}

std::uint32_t *pixelRow(SDL_Surface *surface, int y) {
    return reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(surface->pixels) +
                                             static_cast<std::ptrdiff_t>(y) * surface->pitch);
}

SurfacePtr argbCopy(SDL_Surface *surface) {
    SurfacePtr converted(SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) throw std::runtime_error(SDL_GetError());
    for (int y = 0; y < converted->h; ++y) {
        auto *row = pixelRow(converted.get(), y);
        for (int x = 0; x < converted->w; ++x) row[x] = quantize565(row[x]);
    }
    return converted;
}

SurfacePtr blankSurface(int width, int height, std::uint32_t argb) {
    SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                      SDL_PIXELFORMAT_ARGB8888));
    if (!surface) throw std::runtime_error(SDL_GetError());
    for (int y = 0; y < height; ++y) {
        auto *row = pixelRow(surface.get(), y);
        std::fill(row, row + width, argb);
    }
    return surface;
}

// BltFast (0x00404a70 through 0x00401ca0): a clipped copy, keyed against
// the source's colour key when `keyed`.
void bltFast(SDL_Surface *destination, int dx, int dy, SDL_Surface *source,
             SDL_Rect from, bool keyed) {
    if (from.x < 0) { dx -= from.x; from.w += from.x; from.x = 0; }
    if (from.y < 0) { dy -= from.y; from.h += from.y; from.y = 0; }
    from.w = std::min(from.w, source->w - from.x);
    from.h = std::min(from.h, source->h - from.y);
    if (dx < 0) { from.x -= dx; from.w += dx; dx = 0; }
    if (dy < 0) { from.y -= dy; from.h += dy; dy = 0; }
    from.w = std::min(from.w, destination->w - dx);
    from.h = std::min(from.h, destination->h - dy);
    if (from.w <= 0 || from.h <= 0) return;
    const std::uint32_t key = pixelRow(source, 0)[0];
    for (int row = 0; row < from.h; ++row) {
        const auto *in = pixelRow(source, from.y + row) + from.x;
        auto *out = pixelRow(destination, dy + row) + dx;
        for (int column = 0; column < from.w; ++column) {
            if (!keyed || in[column] != key) out[column] = in[column];
        }
    }
}

// __ftol after the +-0.5 the race adds before converting (0x00478680,
// 0x00478090, 0x00470490): round half away from zero.
int roundAway(double value) {
    return static_cast<int>(0.0 < value ? value + 0.5 : value - 0.5);
}

// One vertex of the textured polygon (0x0046f5a0's 0x20-byte records).
struct RasterVertex {
    float x{};
    float y{};
    float ceilY{};
    int iy{};
    float u{};
    float v{};
    int next{-1};
    int prev{-1};
};

// 0x0046fb70: clip the ring against the viewport's four planes
// (0x0050a6c8: x > left, x < right, y > top, y < bottom; a point on a
// plane is outside), interpolating new vertices as 0x0046fd60 does.
std::vector<RasterVertex> clipPolygon(const std::vector<RasterVertex> &input,
                                      const SDL_Rect &view) {
    struct Plane {
        float a;
        float b;
        float c;
    };
    const std::array<Plane, 4> planes{{
        {1.0F, 0.0F, static_cast<float>(view.x)},
        {-1.0F, 0.0F, -static_cast<float>(view.x + view.w)},
        {0.0F, 1.0F, static_cast<float>(view.y)},
        {0.0F, -1.0F, -static_cast<float>(view.y + view.h)},
    }};
    std::vector<RasterVertex> polygon = input;
    for (const auto &plane : planes) {
        if (polygon.empty()) break;
        std::vector<RasterVertex> output;
        const std::size_t count = polygon.size();
        const auto side = [&](const RasterVertex &vertex) {
            return plane.a * vertex.x + plane.b * vertex.y - plane.c;
        };
        const auto between = [](const RasterVertex &from, const RasterVertex &to,
                                float t) {
            RasterVertex made;
            made.x = (to.x - from.x) * t + from.x;
            made.y = (to.y - from.y) * t + from.y;
            made.u = (to.u - from.u) * t + from.u;
            made.v = (to.v - from.v) * t + from.v;
            return made;
        };
        for (std::size_t index = 0; index < count; ++index) {
            const RasterVertex &current = polygon[index];
            const RasterVertex &following = polygon[(index + 1) % count];
            const float here = side(current);
            const float there = side(following);
            const int outside = (here <= 0.0F ? 1 : 0) | (there <= 0.0F ? 2 : 0);
            if (outside == 0) {
                output.push_back(current);
            } else if (outside == 1) {
                output.push_back(between(current, following, -(here / (there - here))));
            } else if (outside == 2) {
                output.push_back(current);
                output.push_back(between(current, following, -(here / (there - here))));
            }
        }
        polygon = std::move(output);
    }
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        polygon[index].next = static_cast<int>((index + 1) % polygon.size());
        polygon[index].prev =
            static_cast<int>((index + polygon.size() - 1) % polygon.size());
    }
    return polygon;
}

// 0x0046f800: an affine, keyed, 16.16 fixed-point span fill of the ring
// from the atlas (256 texels to a row), left edge along prev and right edge
// along next.
void rasterizePolygon(SDL_Surface *destination, std::vector<RasterVertex> &ring,
                      const SDL_Surface *atlas, std::int32_t dudx,
                      std::int32_t dvdx, std::uint32_t key) {
    if (ring.size() < 2) return;
    for (auto &vertex : ring) {
        vertex.x -= 0.5F;
        vertex.y -= 0.5F;
        vertex.ceilY = static_cast<float>(std::ceil(static_cast<double>(vertex.y)));
        vertex.iy = static_cast<int>(vertex.ceilY);
    }
    int top = 0;
    int bottom = 0;
    for (int index = ring[0].next; index != 0; index = ring[static_cast<std::size_t>(index)].next) {
        const auto &vertex = ring[static_cast<std::size_t>(index)];
        if (vertex.y < ring[static_cast<std::size_t>(top)].y) top = index;
        if (vertex.y > ring[static_cast<std::size_t>(bottom)].y) bottom = index;
    }
    const auto at = [&](int index) -> RasterVertex & {
        return ring[static_cast<std::size_t>(index)];
    };
    if (at(top).iy == at(bottom).iy) return;
    int left = top;
    int right = top;
    if (at(at(top).next).iy == at(top).iy) {
        right = at(top).next;
    } else if (at(at(top).prev).iy == at(top).iy) {
        right = top;
        left = at(top).prev;
    }
    std::int32_t x = 0, xStep = 0, u = 0, uStep = 0, v = 0, vStep = 0;
    std::int32_t rightX = 0, rightStep = 0;
    const int lastRow = at(bottom).iy;
    const auto *texels = static_cast<const std::uint32_t *>(atlas->pixels);
    const int texelPitch = atlas->pitch / 4;
    for (int y = at(top).iy; y < lastRow; ++y) {
        if (y < at(left).iy) {
            x += xStep;
            u += uStep;
            v += vStep;
        } else {
            int to = at(left).prev;
            while (at(to).iy == at(left).iy && to != bottom) {
                left = to;
                to = at(to).prev;
            }
            const RasterVertex &from = at(left);
            const RasterVertex &end = at(to);
            const double dy = static_cast<double>(end.y) - from.y;
            if (dy > 0.0) {
                const double prestep = static_cast<double>(from.ceilY) - from.y;
                const double dxdy = (static_cast<double>(end.x) - from.x) / dy;
                xStep = static_cast<std::int32_t>(dxdy * kFixed);
                x = static_cast<std::int32_t>((prestep * dxdy + from.x) * kFixed);
                const double dudy = (static_cast<double>(end.u) - from.u) / dy;
                uStep = static_cast<std::int32_t>(dudy * kFixed);
                u = static_cast<std::int32_t>((prestep * dudy + from.u) * kFixed);
                const double dvdy = (static_cast<double>(end.v) - from.v) / dy;
                vStep = static_cast<std::int32_t>(dvdy * kFixed);
                v = static_cast<std::int32_t>((prestep * dvdy + from.v) * kFixed);
            } else {
                x = static_cast<std::int32_t>(static_cast<double>(from.x) * kFixed);
                u = static_cast<std::int32_t>(static_cast<double>(from.u) * kFixed);
                v = static_cast<std::int32_t>(static_cast<double>(from.v) * kFixed);
            }
            left = to;
        }
        if (y < at(right).iy) {
            rightX += rightStep;
        } else {
            int to = at(right).next;
            while (at(to).iy == at(right).iy && to != bottom) {
                right = to;
                to = at(to).next;
            }
            const RasterVertex &from = at(right);
            const RasterVertex &end = at(to);
            const double dy = static_cast<double>(end.y) - from.y;
            if (dy > 0.0) {
                const double dxdy = (static_cast<double>(end.x) - from.x) / dy;
                rightStep = static_cast<std::int32_t>(dxdy * kFixed);
                rightX = static_cast<std::int32_t>(
                    ((static_cast<double>(from.ceilY) - from.y) * dxdy + from.x) * kFixed);
            } else {
                rightX = static_cast<std::int32_t>(static_cast<double>(from.x) * kFixed);
            }
            right = to;
        }
        const std::uint32_t spanStart = static_cast<std::uint32_t>(x) + 0x10000U;
        const auto first = static_cast<std::int32_t>(spanStart >> 16);
        const auto last = static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(rightX) + 0x10000U) >> 16);
        std::int32_t count = last - first - 1;
        if (count <= 0 || y < 0 || y >= destination->h) continue;
        const auto fraction = static_cast<std::int32_t>(
            (spanStart & 0xffff0000U) - static_cast<std::uint32_t>(x));
        std::uint32_t texelV = static_cast<std::uint32_t>(
            v + static_cast<std::int32_t>((static_cast<std::int64_t>(dvdx) * fraction) >> 16));
        std::uint32_t texelU = static_cast<std::uint32_t>(
            u + static_cast<std::int32_t>((static_cast<std::int64_t>(dudx) * fraction) >> 16));
        auto *row = pixelRow(destination, y);
        for (std::int32_t column = first; count > 0; ++column, --count) {
            const std::uint32_t index = (texelU >> 16) + ((texelV >> 8) & 0xffff00U);
            texelV += static_cast<std::uint32_t>(dvdx);
            texelU += static_cast<std::uint32_t>(dudx);
            const std::uint32_t row256 = index >> 8;
            const std::uint32_t col256 = index & 0xffU;
            if (static_cast<int>(row256) >= atlas->h) continue;
            const std::uint32_t texel =
                texels[static_cast<std::size_t>(row256) * static_cast<std::size_t>(texelPitch) + col256];
            if (texel != key && column >= 0 && column < destination->w) row[column] = texel;
        }
    }
}

// 0x0046f5a0: the sprite rectangle (+0x20: half width, half height, atlas
// x, y, width, height) rotated by the heading and scaled by the car size,
// centred on (cx, cy), clipped to the viewport and filled.
void drawRotated(SDL_Surface *destination, const SDL_Surface *atlas, float halfWidth,
                 float halfHeight, int atlasX, int atlasY, int width, int height,
                 float cx, float cy, float heading, float size, const SDL_Rect &view) {
    const std::uint32_t key = static_cast<const std::uint32_t *>(atlas->pixels)[0];
    const double a = static_cast<double>(heading) + kPi;
    const double b = -a;
    const auto ax = static_cast<float>(-(std::cos(b) * halfWidth * size));
    const auto ay = static_cast<float>(-(std::sin(b) * halfWidth * size));
    const double c = -(a + kHalfPi);
    const auto bx = static_cast<float>(-(std::cos(c) * halfHeight * size));
    const auto by = static_cast<float>(-(std::sin(c) * halfHeight * size));
    std::vector<RasterVertex> ring(4);
    const float p = ax + cx;
    const float q = ay + cy;
    ring[0].x = p - bx;
    ring[0].y = q - by;
    ring[0].u = static_cast<float>(atlasX);
    ring[0].v = static_cast<float>(atlasY);
    const float r = cx - ax;
    const float s = cy - ay;
    ring[1].x = r - bx;
    ring[1].y = s - by;
    ring[1].u = static_cast<float>(width) + static_cast<float>(atlasX);
    ring[1].v = static_cast<float>(atlasY);
    ring[2].x = r + bx;
    ring[2].y = s + by;
    ring[2].u = ring[1].u;
    ring[2].v = static_cast<float>(height) + static_cast<float>(atlasY);
    ring[3].x = p + bx;
    ring[3].y = q + by;
    ring[3].u = static_cast<float>(atlasX);
    ring[3].v = ring[2].v;
    // The gradients over v0, v1, v2.
    const double d12y = static_cast<double>(ring[1].y) - ring[2].y;
    const double d02y = static_cast<double>(ring[0].y) - ring[2].y;
    const double determinant = (static_cast<double>(ring[0].x) - ring[2].x) * d12y -
                               (static_cast<double>(ring[1].x) - ring[2].x) * d02y;
    const double scale = 1.0 / determinant * kFixed;
    const auto dudx = static_cast<std::int32_t>(-static_cast<double>(width) * d12y * scale);
    const auto dvdx =
        static_cast<std::int32_t>(-((d12y - d02y) * static_cast<double>(height) * scale));
    std::vector<RasterVertex> clipped = clipPolygon(ring, view);
    if (clipped.empty()) return;
    rasterizePolygon(destination, clipped, atlas, dudx, dvdx, key);
}

// 0x004124e0: place a w x h box in the rectangle by the flags' high byte
// (0 left, 0x100 centre, 0x200 right) and low byte (0 top, 1 centre,
// 2 bottom), with C division.
SDL_Point placeText(const Box &box, int width, int height, unsigned flags) {
    SDL_Point at{box.left, box.top};
    const unsigned horizontal = flags & 0xff00U;
    if (horizontal == 0x100U) at.x = (box.right - box.left - width) / 2 + box.left;
    else if (horizontal == 0x200U) at.x = box.right - width;
    const unsigned vertical = flags & 0xffU;
    if (vertical == 1U) at.y = (box.bottom - box.top - height) / 2 + box.top;
    else if (vertical == 2U) at.y = box.bottom - height;
    return at;
}

// 0x00401e20: formatted text placed in the rectangle.
void drawBoxText(SDL_Surface *target, const BitmapFont &font, const Box &box,
                 unsigned flags, const std::string &text) {
    const SDL_Point at = placeText(box, textWidth(font, text), font.height, flags);
    drawFontText(target, font, text, at.x, at.y);
}

} // namespace

std::optional<std::span<const std::uint8_t>>
RacingScene::findRacingAsset(const std::string &asset) {
    // The same layers as the racing menu: Data2 (two extra tracks) and
    // Data1 (the replaced track.ini files) before Data.
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
    return std::nullopt;
}

std::span<const std::uint8_t> RacingScene::racingAsset(const std::string &asset) {
    if (auto found = findRacingAsset(asset)) return *found;
    throw std::runtime_error("Missing racing asset " + asset);
}

RacingScene::RaceSprite RacingScene::loadRaceSprite(const std::string &asset) {
    RaceSprite sprite;
    SurfacePtr loaded = loadBmp(racingAsset(asset));
    sprite.surface = argbCopy(loaded.get());
    sprite.width = sprite.surface->w;
    sprite.height = sprite.surface->h;
    return sprite;
}

void RacingScene::loadRaceClip(RaceClip &clip, const std::string &name) {
    clip.name = name;
    const auto bytes = racingAsset("animation\\" + name + ".smk");
    clip.bytes.assign(bytes.begin(), bytes.end());
    clip.media = decodeSmackerStreamed(
        std::span<const std::uint8_t>(clip.bytes.data(), clip.bytes.size()),
        "racing " + name, SmackerKeying::TopLeftRgb565, false, true);
    clip.shown = -1;
}

void RacingScene::showRaceClipFrame(RaceClip &clip) {
    auto &game = *racingGame;
    const auto frames = static_cast<int>(clip.media.videoFrames.size());
    if (frames <= 0) return;
    int frame = 0;
    if (frames > 1) {
        const double frameMs =
            (clip.media.videoFrames.metadata(1).time -
             clip.media.videoFrames.metadata(0).time) * 1000.0;
        if (frameMs > 0.0)
            frame = static_cast<int>(std::floor((game.clockMs - clip.startedAt) / frameMs));
    }
    frame = clip.loop ? ((frame % frames) + frames) % frames : std::clamp(frame, 0, frames - 1);
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

// SCENE_AddAnim (0x00412840) then play (0x004129d0).
void RacingScene::addRaceClip(RaceClip &clip, int layer) {
    auto &game = *racingGame;
    game.stage->sprite(clip.sprite).name = clip.name;
    game.stage->add(clip.sprite, -1, -1, layer);
    clip.playing = true;
    clip.startedAt = game.clockMs;
    clip.completesAt =
        game.clockMs + originalSmackerCompletionDuration(clip.media) * 1000.0;
    clip.shown = -1;
    showRaceClipFrame(clip);
}

// 0x00476550: the message clip (+0xac) shows `name`, unless it is already
// playing it; nothing is shown once a human has finished (state 2).
void RacingScene::setRaceMessageClip(const std::string &name, bool loop) {
    auto &game = *racingGame;
    if (game.state == 2) return;
    if (game.message.name == name && game.message.playing) return;
    if (game.stage->active(game.message.sprite)) game.stage->remove(game.message.sprite);
    game.message.playing = false;
    loadRaceClip(game.message, name);
    game.message.loop = loop;
    addRaceClip(game.message, 3);
}

// 0x00479d70's trigger lines, on the HUD of the car's record: kind 2 starts
// lap{N}_0 looping (N laps left by the path lap +0x204), kind 1 turns it
// into lap{N}_1 and counts the lap, kind 3 takes lap{N}_0 off, kind 4 plays
// ending in the story race.
void RacingScene::crossTrigger(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    const bool record = slot == 0 && game.block.mode != 2;
    const int pathLap = game.aiCars[slot].pathLap;
    switch (car.body.hitKind) {
    case 1: {
        const int left = game.block.laps - pathLap + 1;
        if (record && 0 < left && left < 8 &&
            game.message.name == "lap" + std::to_string(left) + "_0")
            setRaceMessageClip("lap" + std::to_string(left) + "_1");
        countLap(slot);
        break;
    }
    case 2: {
        const int left = game.block.laps - pathLap;
        if (record && 0 < left && left < 8)
            setRaceMessageClip("lap" + std::to_string(left) + "_0", true);
        break;
    }
    case 3: {
        const int left = game.block.laps - pathLap;
        if (record && 0 < left && left < 8 &&
            game.message.name == "lap" + std::to_string(left) + "_0" && game.message.playing) {
            game.stage->remove(game.message.sprite);
            game.message.playing = false;
        }
        break;
    }
    case 4:
        if (game.block.story && record) setRaceMessageClip("ending");
        break;
    default:
        break;
    }
}

// 0x0047a350: a lap counts when the slot's own lap counter is ahead of the
// car's, so backing over the line does not.
void RacingScene::countLap(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    if (!(game.state < 2 && car.lap < game.aiSlots[slot].lapCount)) return;
    const auto time = static_cast<std::uint32_t>(game.clockMs - car.lapStartMs);
    if (0 < car.lap && car.lap <= game.block.laps && car.lap <= 8)
        car.lapTimes[static_cast<std::size_t>(car.lap - 1)] = time;
    if (time < car.bestLap) {
        car.bestLap = time;
        if (!car.body.ai && time < game.block.best) {
            // race:BESTLAP on the message line for 5000 ms.
            game.messageLine = language.get("race", "BESTLAP", "Ny bestetid");
            game.messageMs = 5000;
            game.block.best = car.bestLap;
        }
    }
    ++car.lap;
    car.lapStartMs = game.clockMs;
    if (!car.body.finished && game.block.laps < car.lap) {
        // State 1 when any car finishes; the record's car plays finish,
        // makes it 2 and stops the lap clock.
        game.state = 1;
        car.body.finished = true;
        if (slot == 0 && game.block.mode != 2) {
            setRaceMessageClip("finish");
            game.state = 2;
            game.lapClock = false;
        }
    }
}

double RacingScene::raceClockNow() {
    // 0x0040c450: milliseconds, frozen while a message box or help is open.
    return std::floor(sceneElapsedNow() * 1000.0);
}

bool RacingScene::loadRacingGame() {
    racingGame.reset();
    racingGame.emplace();
    racingGame->pendingBuild = true;
    return true;
}

void RacingScene::buildRace() {
    racingGame.reset();
    RacingGame game;
    // 0x00474870: the entry mode is the block; without one the story block.
    if (raceParameterBlock) {
        game.block = *raceParameterBlock;
    } else {
        game.block = racing::storyRaceBlock();
    }
    auto &block = game.block;
    game.stage = std::make_unique<ActivityStage>(renderer, referenceTrace);

    // race_LoadData 0x004749d0.
    // The best lap (+0xac): the first row of the active profile's own list
    // keyed by +0x180 (0x00411d10 into a 0x174-byte list at 0x00474a1a; the
    // count at +4, row 0's score at +0x28), 0x7fffffff when it is empty.
    block.best = racing::kNoTime;
    if (const auto profile = activeProfilePath()) {
        try {
            const auto table = profileSceneScores(*profile, "racing", block.lapTimeKey);
            if (table && !table->entries.empty()) block.best = table->entries.front().score;
        } catch (const std::exception &error) {
            std::cerr << "Could not read racing scores: " << error.what() << '\n';
        }
    }
    game.font12 = loadBitmapFont(renderer, common.data(common.find("fonts\\arial_12_.bmp")));
    game.font16 = loadBitmapFont(renderer, common.data(common.find("fonts\\arial_16_b.bmp")));
    game.font20 = loadBitmapFont(renderer, common.data(common.find("fonts\\arial_20_b.bmp")));

    // The loading bar (0x00476210): PROGRESS_Create (0x004089a0) at
    // (0x17a - 1, 0x1b0) on layer 0, removed at 100 (0x00408ac0).
    const int progress = game.stage->create("", 0x17a - 1, 0x1b0, 0, false);
    game.stage->setSurface(progress, argbCopy(loadBmp(racingAsset("bitmap\\progress.bmp")).get()));
    game.stage->add(progress, -1, -1, 0);

    // RACE_LoadTrack 0x00475270.
    const std::string trackDir = "tracks\\" + block.track + "\\";
    game.track = racing::parseTrackIni(racingAsset(trackDir + "track.ini"));
    {
        SurfacePtr loaded = loadBmp(racingAsset(trackDir + game.track.track));
        game.trackSurface = argbCopy(loaded.get());
        game.trackWidth = game.trackSurface->w;
        game.trackHeight = game.trackSurface->h;
    }
    game.collision = racing::parseCollision(racingAsset(trackDir + game.track.collision));
    game.path = racing::parsePath(racingAsset(trackDir + game.track.path));
    if (!game.track.sounds.empty())
        game.sounds = racing::parseSounds(racingAsset(trackDir + game.track.sounds));
    for (const auto &overlay : game.track.overlays)
        game.overlays.push_back(loadRaceSprite(trackDir + overlay.source));
    SurfacePtr map = argbCopy(loadBmp(racingAsset(trackDir + game.track.map)).get());
    {
        // 0x004149f0: the 8-bit index bitmap as stored.
        SurfacePtr terrain = loadBmp(racingAsset(trackDir + game.track.terrain));
        if (terrain->format->BytesPerPixel != 1)
            throw std::runtime_error("terrain.bmp is not an 8-bit bitmap");
        game.terrainWidth = terrain->w;
        game.terrainHeight = terrain->h;
        game.terrain.resize(static_cast<std::size_t>(terrain->w * terrain->h));
        SDL_LockSurface(terrain.get());
        for (int row = 0; row < terrain->h; ++row) {
            std::memcpy(game.terrain.data() + static_cast<std::size_t>(row * terrain->w),
                        static_cast<const std::uint8_t *>(terrain->pixels) +
                            static_cast<std::ptrdiff_t>(row) * terrain->pitch,
                        static_cast<std::size_t>(terrain->w));
        }
        SDL_UnlockSurface(terrain.get());
    }

    // The cars (0x00474c8a): slot 0 is human in mode 0, slots 0 and 1 in
    // mode 2, none in mode 1.
    std::uint32_t placedParts = 0;
    if (const auto path = activeProfilePath()) {
        try {
            if (const auto state = profileBuildacarState(*path))
                placedParts = state->placedParts;
        } catch (const std::exception &error) {
            std::cerr << "Could not read the Build-a-Car state: " << error.what() << '\n';
        }
    }
    const int stage = buildacarLevel(executableGameData, placedParts);
    std::vector<racing::CarSpec> specs;
    for (int slot = 0; slot < block.cars_count && slot < 4; ++slot) {
        const bool ai = block.mode == 0 ? slot != 0
                                         : block.mode == 2 ? slot >= 2 : true;
        RaceCar car;
        car.resource = block.cars[static_cast<std::size_t>(slot)];
        const std::string carDir = "cars\\" + car.resource + "\\";
        // 0x00475d5e: "%s/stage%d.ini" with the Build-a-Car level (0x004231e0).
        const std::string ini = racing::usesStageFile(car.resource)
                                    ? carDir + "stage" + std::to_string(stage) + ".ini"
                                    : carDir + "car.ini";
        car.spec = racing::parseCarIni(racingAsset(ini), ai);
        car.human = !ai;
        // 0x00476011: race:DRIVER<id>; in mode 0 slot 0 takes the profile's
        // name (0x00411eb0 at 0x00474d01).
        car.name = language.get("race", "DRIVER" + std::to_string(car.spec.id), "");
        if (block.mode == 0 && slot == 0 && profiles[profileIndex] != "No profile")
            car.name = profiles[profileIndex];
        for (int sprite = 0; sprite < 4; ++sprite) {
            car.sprites[static_cast<std::size_t>(sprite)] =
                loadRaceSprite(carDir + car.resource + std::to_string(sprite) + ".bmp");
        }
        car.shadow = loadRaceSprite(carDir + "shadow.bmp");
        specs.push_back(car.spec);
        game.cars.push_back(std::move(car));
    }

    // 0x00474db2: the two atlases, 256 wide, cars packed left to right.
    int x = 0;
    int y = 0;
    int rowBase = 0;
    int atlasHeight = 0;
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        auto &car = game.cars[slot];
        const auto &first = car.sprites[0];
        car.atlasX = x;
        car.atlasY = y;
        car.halfWidth = static_cast<float>(first.width) * 0.5F;
        car.halfHeight = static_cast<float>(first.height) * 0.5F;
        car.width = first.width;
        car.height = first.height;
        atlasHeight = std::max(atlasHeight, first.height + rowBase);
        if (slot + 1 < game.cars.size()) {
            x += first.width;
            if (game.cars[slot + 1].sprites[0].width + x > 0xff) {
                x = 0;
                rowBase = atlasHeight;
                y = atlasHeight;
            }
        }
    }
    // Both filled with the key colour 0xff00 (0x00404720).
    game.carAtlas = blankSurface(kAtlasWidth, std::max(atlasHeight, 1), 0xff00ff00U);
    game.shadowAtlas = blankSurface(kAtlasWidth, std::max(atlasHeight, 1), 0xff00ff00U);
    for (auto &car : game.cars) {
        const SDL_Rect whole{0, 0, car.width, car.height};
        bltFast(game.carAtlas.get(), car.atlasX, car.atlasY,
                car.sprites[0].surface.get(), whole, false);
        bltFast(game.shadowAtlas.get(), car.atlasX, car.atlasY,
                car.shadow.surface.get(),
                SDL_Rect{0, 0, car.shadow.width, car.shadow.height}, false);
    }

    // HUD record 0 belongs to slot 0 in modes 0 and 1 (0x00474f6c). The
    // minimap (+0x154) takes the track's map and is added at its default
    // place on layer 1; a dot per car follows on layer 2.
    game.minimap = game.stage->create("", kMinimapAt.x, kMinimapAt.y, 0, true);
    game.stage->setSurface(game.minimap, std::move(map));
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        const std::string name = slot == 0 ? "playerdot" : "cardot";
        const int dot = game.stage->create(name, 0, 0, 0, false);
        game.stage->setSurface(dot, argbCopy(loadBmp(racingAsset("bitmap\\" + name + ".bmp")).get()));
        game.dots[slot] = dot;
    }
    game.pickupIcon = game.stage->create("", kPickupIconAt.x, kPickupIconAt.y, 0, true);
    game.message.sprite = game.stage->create("", kMessageClipAt.x, kMessageClipAt.y, 0, false);
    game.count.sprite = game.stage->create("count", kCountAt.x, kCountAt.y, 0, false);

    racingGame = std::move(game);
    auto &loaded = *racingGame;
    const auto &entered = loaded.block;
    if (entered.mode != 2) {
        loaded.stage->add(loaded.minimap, -1, -1, 1);
        for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot)
            loaded.stage->add(loaded.dots[slot], 0, 0, 2);
    }

    loaded.stage->remove(progress);

    // 0x004750f0, 0x00475170, 0x00475220.
    const float scale = racing::loadScale(entered.mode, loaded.track, specs);
    racing::applyLoadScaling(specs, scale, loaded.track.speed);
    for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot) {
        auto &car = loaded.cars[slot];
        car.spec = specs[slot];
        auto &body = car.body;
        body.id = car.spec.id;
        body.ai = car.spec.ai;
        body.accel = car.spec.accel;
        body.topSpeed = car.spec.topSpeed;
        // 0x004730c0: the collision box is the ini size times the car size.
        body.boxWidth = car.spec.width * kCarSize;
        body.boxHeight = car.spec.height * kCarSize;
        body.braking = car.spec.braking;
        body.turning = car.spec.turning;
        body.grip = car.spec.grip;
        body.mass = car.spec.mass;
        body.shield = car.spec.shield;
        body.gears = car.spec.gears;
        body.spriteWidth = car.sprites[0].width;
    }
    // COL_LoadCollisionData's segments, then four per car for the outlines
    // (0x00471660).
    for (const auto &segment : loaded.collision)
        loaded.world.segments.push_back(
            {segment.x1, segment.y1, segment.x2, segment.y2, segment.kind, 0, -1});
    loaded.world.staticCount = static_cast<int>(loaded.world.segments.size());
    loaded.aiPath = racing::makeAiPath(loaded.path);
    loaded.aiSlots.resize(loaded.cars.size());
    loaded.hornUntilMs.assign(loaded.cars.size(), 0.0);
    for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot) {
        racing::AiCar ai;
        ai.body = &loaded.cars[slot].body;
        ai.slot = &loaded.aiSlots[slot];
        ai.spec = loaded.cars[slot].spec;
        loaded.aiCars.push_back(ai);
    }
    // Each car's horn.wav length (data bytes over bytes per second), for how
    // long its horn sounds.
    loaded.hornLengthMs.assign(loaded.cars.size(), 0.0);
    for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot) {
        const auto wav = findRacingAsset("cars\\" + loaded.cars[slot].resource + "\\horn.wav");
        if (!wav || wav->size() < 44) continue;
        std::size_t at = 12;
        std::uint32_t rate = 0;
        while (at + 8 <= wav->size()) {
            const std::string id(reinterpret_cast<const char *>(wav->data() + at), 4);
            const std::uint32_t size = readLe32(*wav, at + 4);
            if (id == "fmt " && at + 16 <= wav->size()) rate = readLe32(*wav, at + 16);
            if (id == "data" && rate != 0) {
                loaded.hornLengthMs[slot] = 1000.0 * size / rate;
                break;
            }
            at += 8 + size + (size & 1U);
        }
    }
    for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot) {
        const auto edges = racing::carOutline(loaded.cars[slot].body,
                                              loaded.cars[slot].body.boxWidth,
                                              loaded.cars[slot].body.boxHeight);
        for (std::size_t index = 0; index < 4; ++index) {
            const auto &from = edges[index];
            const auto &to = edges[(index + 1) & 3U];
            loaded.world.segments.push_back({from[0], from[1], to[0], to[1], -3,
                                             loaded.cars[slot].body.id,
                                             static_cast<int>(slot)});
        }
    }

    loaded.frame = blankSurface(kWidth, kHeight, 0xff000000U);
    loaded.target.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_TARGET, kWidth, kHeight));

    // 0x00474990: the wind, one random direction per entry.
    {
        // The wind: a random angle (rand() times 0x004a00f0), 75 long
        // (0x004a00e8), truncated.
        const double angle = static_cast<double>(crtRandom.next()) * 1.91747207e-4;
        loaded.windX = static_cast<int>(std::cos(angle) * 75.0);
        loaded.windY = static_cast<int>(std::sin(angle) * 75.0);
    }
    for (int type = 1; type <= 6; ++type)
        loaded.pickupSprites[static_cast<std::size_t>(type)] =
            loadRaceSprite("bitmap\\pickup" + std::to_string(type) + ".bmp");
    for (int type = 0; type <= 5; ++type)
        loaded.slotSprites[static_cast<std::size_t>(type)] =
            loadRaceSprite("bitmap\\slot" + std::to_string(type) + ".bmp");
    {
        const std::array<const char *, 6> names{"crashanim", "damageanim", "oilanim",
                                                "smokeanim", "sparkanim", "spikeanim"};
        for (std::size_t type = 0; type < names.size(); ++type)
            loaded.particleSprites[type] =
                loadRaceSprite(std::string("bitmap\\") + names[type] + ".bmp");
    }
    loaded.clockMs = raceClockNow();
    resetRace();
    std::cout << "Loaded executable-derived race: " << entered.track << ", "
              << loaded.cars.size() << " car(s) (";
    for (std::size_t slot = 0; slot < loaded.cars.size(); ++slot)
        std::cout << (slot ? ", " : "") << loaded.cars[slot].resource;
    std::cout << "), " << entered.laps << " laps, mode " << entered.mode
              << ", Build-a-Car stage " << stage << '\n';
}

// 0x004762b0.
void RacingScene::resetRace() {
    auto &game = *racingGame;
    game.state = 3;
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        auto &car = game.cars[slot];
        car.body.x = static_cast<float>(game.track.startX[slot]);
        car.body.y = static_cast<float>(game.track.startY[slot]);
        car.position = static_cast<int>(slot);
        car.body.heading = game.track.startAngle;
        // 0x00476620.
        car.lapTimes.fill(0U);
        car.lap = 1;
        car.bestLap = racing::kNoTime;
        racing::resetPhysicsCar(car.body);
        car.body.terrain = racing::TerrainEntry{};
        // 0x00476760: no skid mark joins the old wheels to the new.
        car.wheels = racing::carOutline(car.body, car.body.boxWidth - 10.0F,
                                        car.body.boxHeight - 20.0F);
        car.wheelsBefore = car.wheels;
        // The slot record: node 0 as the target, its position as the
        // follower's lap position, the view timer 2.5, the rest zero.
        auto &record = game.aiSlots[slot];
        record = racing::AiSlot{};
        record.target = 0;
        record.lapPosition = game.aiPath.nodes.empty() ? 0 : game.aiPath.nodes[0].position;
        record.lapCount = 1;
        record.revTarget = 0.0F;
        auto &ai = game.aiCars[slot];
        ai.pathLap = 1;
        ai.nodePosition = 0;
        ai.reverseTimer = 0.0F;
        ai.progress = 0.0F;
        ai.raceTime = 0;
    }
    // 0x004769d0, 0x00476920: no objects, no particles; 0x00476490: the
    // track's hidden car part while its record (0x402, keyed by the track id)
    // is 0, on the first node whose +0x14 is 666 (0x44268000). It neither
    // marks its node nor counts toward the four.
    game.objects.clear();
    for (auto &particle : game.particles) particle.active = false;
    game.randomPickups = 0;
    game.hiddenPartTaken = false;
    game.spawning = false;
    for (auto &node : game.aiPath.nodes) node.pickup = false;
    {
        std::uint32_t taken = 0;
        if (const auto path = activeProfilePath()) {
            try {
                taken = profileSceneRecordU32(*path, "racing", 0x402U,
                                              static_cast<std::uint32_t>(game.track.id))
                            .value_or(0U);
            } catch (const std::exception &) {
                taken = 0;
            }
        }
        if (taken == 0) {
            for (const auto &node : game.aiPath.nodes) {
                if (node.target == 666.0F) {
                    placePickup(node.index, 6);
                    break;
                }
            }
        }
    }
    // 0x00476620 -> 0x00476810(car, 0): nothing held, the empty slot shown.
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        auto &car = game.cars[slot];
        car.trick = 0;
        car.trickLeft = 0;
        car.trickTimer = 0.0F;
        car.smokeTimer = 0.0F;
        holdPickup(slot, 0);
    }
    // HUD record 0: the countdown clip on layer 3, then the start message.
    if (!game.cars.empty() && game.block.mode != 2) {
        if (game.count.media.videoFrames.empty()) loadRaceClip(game.count, "count");
        addRaceClip(game.count, 3);
        setRaceMessageClip("start");
    }
    game.dt = 1.0F;
    game.lastMs = game.clockMs;
}

void RacingScene::queueOverlaysOver(SDL_Rect rect) {
    // 0x00478980, given the car's bounding box as left, top, right, bottom.
    auto &game = *racingGame;
    const int left = rect.x - 7 <= 0 ? 0 : rect.x - 7;
    const int top = rect.y - 7 <= 0 ? 0 : rect.y - 7;
    const int right = std::min(rect.w + 7, game.trackWidth - 1);
    const int bottom = std::min(rect.h + 7, game.trackHeight - 1);
    if (left >= right || top >= bottom) return;
    for (std::size_t index = 0; index < game.track.overlays.size(); ++index) {
        const auto &spec = game.track.overlays[index];
        auto &sprite = game.overlays[index];
        const int ox2 = spec.x + sprite.width;
        const int oy2 = spec.y + sprite.height;
        if (!(spec.x < right && spec.y < bottom && left < ox2 && top < oy2)) continue;
        const int ix = std::max(spec.x, left);
        const int iy = std::max(spec.y, top);
        const int iw = std::min(ox2, right) - ix;
        const int ih = std::min(oy2, bottom) - iy;
        const SDL_Rect source{ix - spec.x, iy - spec.y, iw, ih};
        // The overlay is painted into the track surface itself (0x00478a95)
        // and queued to be drawn again over the cars.
        bltFast(game.trackSurface.get(), ix, iy, sprite.surface.get(), source, true);
        OverlayDraw draw;
        draw.screenX = ix - game.cameraX;
        draw.screenY = iy - game.cameraY;
        draw.source = source;
        draw.overlay = static_cast<int>(index);
        draw.ontop = spec.ontop;
        game.overlayDraws.push_back(draw);
    }
}

// 0x004784f0.
void RacingScene::drawRaceCar(RaceCar &car) {
    auto &game = *racingGame;
    const auto &body = car.body;
    const float screenX = (body.x - static_cast<float>(game.cameraX)) + 0.0F;
    const float screenY = (body.y - static_cast<float>(game.cameraY)) + 0.0F;
    const auto &sprite = car.sprites[static_cast<std::size_t>(body.sprite)];
    const auto px = static_cast<float>(
        std::sin(static_cast<double>(body.heading)) * sprite.width * kCarOffset + screenX);
    const auto py = static_cast<float>(
        std::cos(static_cast<double>(body.heading)) * sprite.height * kCarOffset + screenY);
    const SDL_Rect view{0, 0, kWidth, kHeight};
    drawRotated(game.frame.get(), game.shadowAtlas.get(), car.halfWidth, car.halfHeight,
                car.atlasX, car.atlasY, car.width, car.height, px + kShadowOffset,
                py + kShadowOffset, body.heading, kCarSize, view);
    drawRotated(game.frame.get(), game.carAtlas.get(), car.halfWidth, car.halfHeight,
                car.atlasX, car.atlasY, car.width, car.height, px, py, body.heading,
                kCarSize, view);
    // 0x00470490: the bounding box of the outline (0x00470340), the
    // collision box (+0x118/+0x11c = width, height times the car size)
    // shifted by a twelfth of the sprite width and turned by the heading.
    const auto corners = racing::carOutline(body, body.boxWidth, body.boxHeight);
    float minX = corners[0][0], minY = corners[0][1], maxX = corners[0][0], maxY = corners[0][1];
    for (const auto &corner : corners) {
        minX = std::min(minX, corner[0]);
        minY = std::min(minY, corner[1]);
        maxX = std::max(maxX, corner[0]);
        maxY = std::max(maxY, corner[1]);
    }
    queueOverlaysOver(SDL_Rect{roundAway(minX), roundAway(minY), roundAway(maxX),
                               roundAway(maxY)});
}

// 0x00477ba0 for one viewport (0x00478190).
void RacingScene::drawRaceView() {
    auto &game = *racingGame;
    game.overlayDraws.clear();
    if (game.cars.empty()) return;
    // 0x00478680: centre player 1's car, rounded, clamped to the track.
    const auto &focus = game.cars[0];
    int cameraX = roundAway(static_cast<double>(focus.body.x - static_cast<float>(kWidth / 2)));
    int cameraY = roundAway(static_cast<double>(focus.body.y - static_cast<float>(kHeight / 2)));
    cameraX = cameraX < 0 ? 0 : std::min(cameraX, game.trackWidth - kWidth);
    cameraY = cameraY < 0 ? 0 : std::min(cameraY, game.trackHeight - kHeight);
    game.cameraX = cameraX;
    game.cameraY = cameraY;
    // 0x004782f0: the track, opaque.
    bltFast(game.frame.get(), 0, 0, game.trackSurface.get(),
            SDL_Rect{cameraX, cameraY, kWidth, kHeight}, false);
    // 0x004783c0 and 0x004781e0(0): the pickups and hazards, then the
    // particles under the cars.
    drawObjects();
    drawParticles(0);
    // 0x00478380: every car, its shadow first.
    for (auto &car : game.cars) drawRaceCar(car);
    // 0x00478330(0), 0x004781e0(1), 0x00478330(1).
    const auto overlays = [&](int layer) {
        for (const auto &draw : game.overlayDraws) {
            if (draw.ontop != layer) continue;
            bltFast(game.frame.get(), draw.screenX, draw.screenY,
                    game.overlays[static_cast<std::size_t>(draw.overlay)].surface.get(),
                    draw.source, true);
        }
    };
    overlays(0);
    drawParticles(1);
    overlays(1);
}

// 0x00478090.
void RacingScene::moveRaceDots() {
    auto &game = *racingGame;
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        const int dot = game.dots[slot];
        if (dot < 0) continue;
        const auto &car = game.cars[slot];
        const int x = roundAway(static_cast<double>(car.body.x * kMinimapWidth /
                                                    static_cast<float>(game.trackWidth))) +
                      kMinimapAt.x;
        const int y = roundAway(static_cast<double>(car.body.y * kMinimapHeight /
                                                    static_cast<float>(game.trackHeight))) +
                      kMinimapAt.y;
        game.stage->moveCentre(dot, x, y);
    }
}

// 0x00477c40 for HUD record 0.
void RacingScene::drawRaceHud() {
    auto &game = *racingGame;
    if (game.cars.empty()) return;
    const auto &flags = kHudLevels[static_cast<std::size_t>(std::clamp(racingHudLevel, 0, 4))];
    const auto &car = game.cars[0];
    SDL_Surface *frame = game.frame.get();
    if (flags[0]) {
        drawBoxText(frame, game.font12, kBestLabel(), 1, language.get("race", "BEST", "Bestetid"));
        drawBoxText(frame, game.font16, kBestValue(), 0x201,
                    game.block.best < racing::kNoTime ? racing::formatRaceTime(game.block.best)
                                                      : std::string("-"));
    }
    if (flags[1]) {
        drawBoxText(frame, game.font12, kLapLabel(), 1,
                    language.get("race", "LAPTIME", "Rundetider"));
        for (int lap = 0; lap < car.lap && lap < game.block.laps && lap < 8; ++lap) {
            const Box row{kLapRow().left, kLapRow().top + lap * 0x18, kLapRow().right,
                          kLapRow().bottom + lap * 0x18};
            drawBoxText(frame, game.font16, row, 1, std::to_string(lap + 1) + ".");
            drawBoxText(frame, game.font16, row, 0x201,
                        racing::formatRaceTime(car.lapTimes[static_cast<std::size_t>(lap)]));
        }
    }
    if (flags[2]) {
        drawBoxText(frame, game.font12, kPositionLabel(), 1,
                    language.get("race", "POSITION", "Plassering"));
        drawBoxText(frame, game.font20, kPositionValue(), 0x201,
                    std::to_string(car.position + 1) + " / " +
                        std::to_string(game.block.cars_count));
    }
    if (flags[3]) {
        drawBoxText(frame, game.font12, kDamageLabel(), 1, language.get("race", "DAMAGE", "Skade"));
        drawBoxText(frame, game.font16, kDamageValue(), 0x201,
                    std::to_string(static_cast<int>(car.body.damage * 100.0F)) + "%");
    }
    if (flags[4]) {
        // The smaller of revs / gears * top speed / 2 and |forward speed| / 2.
        const int geared = static_cast<int>(car.body.revs / static_cast<float>(car.body.gears) *
                                            (car.body.effectiveTop * 0.5F));
        const int moving = static_cast<int>(std::fabs(car.body.forward)) / 2;
        drawBoxText(frame, game.font12, kSpeedLabel(), 1,
                    language.get("race", "SPEED", "Hastighet"));
        drawBoxText(frame, game.font16, kSpeedValue(), 0x201,
                    std::to_string(std::min(geared, moving)) + " km/t");
    }
    if (flags[5]) moveRaceDots();
    // The message line (+0x354, +0x394 ms), blinking 500 ms on and off.
    const int now = static_cast<int>(game.clockMs);
    game.blink = (game.blinkLast - now + game.blink) % 1000;
    const int spent = std::min(now - game.blinkLast, game.messageMs);
    game.messageMs -= spent;
    if (game.blink <= -0x1f5) {
        // UNCERTAIN: race:WRONGWAY needs car +0x220, which nothing in the
        // racing code writes; it is not shown.
        if (game.messageMs != 0 && !game.messageLine.empty())
            drawBoxText(frame, game.font16, kMessageLine(), 0x101, game.messageLine);
    }
    game.blinkLast = now;
}

// 0x004770c0.
void RacingScene::endRace() {
    auto &game = *racingGame;
    if (game.ended) return;
    game.ended = true;
    // 0x004770c0: a collected hidden car part is awarded first
    // (0x0041c150: awarddlg TITLE_HIDDEN and DESC_HIDDEN, the next part).
    if (game.hiddenPartTaken) {
        game.hiddenPartTaken = false;
        if (const auto path = activeProfilePath()) {
            try {
                auto awards = profileAwards(*path);
                const std::uint32_t next = std::min<std::uint32_t>(awards[kAwardCarParts] + 1U, 62U);
                awards[kAwardCarParts] = next;
                setProfileAwards(*path, awards);
                showCarPartAward(next);
                game.ending = RacingGame::Ending::HiddenPart;
                return;
            } catch (const std::exception &error) {
                std::cerr << "Could not award the hidden car part: " << error.what() << '\n';
            }
        }
    }
    // Then the positions are sorted once more and a race that was not quit
    // records its result (0x00477390).
    sortRacePositions();
    if (game.state != 5) {
        beginRaceResults();
        return;
    }
    finishRaceResults();
}

// 0x00477390.
void RacingScene::beginRaceResults() {
    auto &game = *racingGame;
    // Unfinished cars get their time projected from their progress, or
    // 0x7fffffff without any.
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        auto &ai = game.aiCars[slot];
        if (game.cars[slot].body.finished) continue;
        if (ai.progress <= 0.0F) {
            ai.raceTime = racing::kNoTime;
        } else {
            ai.raceTime = static_cast<std::uint32_t>(
                1.0F / ai.progress * static_cast<double>(ai.raceTime));
        }
    }
    // The track award: a four-car race won by the human (slot 0 in mode 0,
    // either player in mode 2), for a track that states one not yet held.
    bool won = false;
    if (game.block.cars_count == 4) {
        if (game.block.mode == 0) won = game.cars[0].position == 0;
        else if (game.block.mode == 2)
            won = game.cars[0].position == 0 || game.cars[1].position == 0;
    }
    if (won && game.track.award >= 0) {
        if (const auto path = activeProfilePath()) {
            try {
                auto awards = profileAwards(*path);
                if (!awardItemAvailable(executableGameData, kAwardTracks, awards[kAwardTracks],
                                        game.track.award)) {
                    awards[kAwardTracks] = awardItemGrant(executableGameData, kAwardTracks,
                                                          awards[kAwardTracks], game.track.award);
                    setProfileAwards(*path, awards);
                    // 0x0041bb20(3, 0, 0, award, 0x10): TITLE_TRACK, DESC_TRACK
                    // and the track icon (0x004ad880).
                    // UNCERTAIN: the original centres the box on the race's
                    // 640x480 display; native draws it at 800x600 size.
                    showAward("bitmap\\award\\track.bmp", "TITLE_TRACK", "DESC_TRACK");
                    game.ending = RacingGame::Ending::Award;
                    return;
                }
            } catch (const std::exception &error) {
                std::cerr << "Could not grant the track award: " << error.what() << '\n';
            }
        }
    }
    openRaceRoster();
}

int RacingScene::raceStageText(const std::string &name, const SDL_Rect &rect,
                               const BitmapFont &font, const std::string &text, int layer) {
    // 0x00407930 then 0x004079c0 with 0x101: a keyed object, the text
    // centred in it.
    auto &game = *racingGame;
    const int sprite = game.stage->create(name, rect.x, rect.y, 0, true);
    SurfacePtr surface = blankSurface(rect.w, rect.h, 0xff00ff00U);
    const SDL_Point at = centreIn(SDL_Rect{0, 0, rect.w, rect.h}, textWidth(font, text), font.height);
    drawFontText(surface.get(), font, text, at.x, at.y);
    game.stage->setSurface(sprite, std::move(surface));
    // 0x00407929 adds the text object at its rectangle's corner.
    game.stage->add(sprite, rect.x, rect.y, layer);
    return sprite;
}

// 0x00421a70: the roster at (204, 38) on layer 500, a cell per finishing
// place (0x004b0590) with the place's base, the car's picture car_%02d by its
// id, the place number, the name and the time.
void RacingScene::openRaceRoster() {
    auto &game = *racingGame;
    game.ending = RacingGame::Ending::Roster;
    constexpr SDL_Point origin{0xcc, 0x26};
    const auto commonSprite = [&](const std::string &asset) {
        return argbCopy(loadBmp(common.data(common.find(asset))).get());
    };
    game.rosterTitleFont = loadBitmapFont(renderer, common.data(common.find("fonts\\Amerigo BT_10_b.bmp")));
    game.rosterNameFont = loadBitmapFont(renderer, common.data(common.find("fonts\\Amerigo BT_10_b.bmp")));
    game.rosterButtonFont = loadBitmapFont(renderer, common.data(common.find("fonts\\Amerigo BT_10_.bmp")));
    game.rosterTimeFont = loadBitmapFont(renderer, common.data(common.find("fonts\\LCD_14_.bmp")));
    // The backdrop is added at its position (0x00407302); the cells' pieces
    // keep the position they were created with (0x00421b62-0x00421b83).
    const auto add = [&](const std::string &name, const std::string &asset, int x, int y,
                         int layer, bool placed = false, bool keyed = true) {
        const int sprite = game.stage->create(name, x, y, 0, keyed);
        game.stage->setSurface(sprite, commonSprite(asset));
        game.stage->add(sprite, placed ? x : -1, placed ? y : -1, layer);
        game.rosterSprites.push_back(sprite);
    };
    add("../../common/dialogue/roster/backdrop", "dialogue\\roster\\backdrop.bmp", origin.x,
        origin.y, 500, true);
    // 0x00421c40: the title (0x004b0580) and the Ok button (0x004b05b0).
    game.rosterSprites.push_back(raceStageText(
        "", SDL_Rect{origin.x + 8, origin.y + 9, 219 - 8, 25 - 9}, game.rosterTitleFont,
        language.get("roster", "TITLE", ""), 1000));
    game.rosterButton = SDL_Rect{origin.x + 0x4b, origin.y + 0x16f, 83, 27};
    {
        const int sprite = game.stage->create("../../common/dialogue/roster/button_0",
                                              game.rosterButton.x, game.rosterButton.y, 0, true);
        SurfacePtr button = commonSprite("dialogue\\roster\\button_0.bmp");
        // The label is the literal "Ok" at 0x004b07c0.
        const std::string label = "Ok";
        const SDL_Point at = centreIn(SDL_Rect{0, 0, button->w, button->h},
                                      textWidth(game.rosterButtonFont, label),
                                      game.rosterButtonFont.height);
        drawFontText(button.get(), game.rosterButtonFont, label, at.x, at.y);
        game.stage->setSurface(sprite, std::move(button));
        game.stage->add(sprite, -1, -1, 1000);
        game.rosterSprites.push_back(sprite);
    }
    const auto cells = originalArray<SDL_Point, 4>(0x4b0590U);
    for (int place = 0; place < 4; ++place) {
        const RaceCar *car = nullptr;
        std::size_t slot = 0;
        for (std::size_t index = 0; index < game.cars.size(); ++index) {
            if (game.cars[index].position == place) {
                car = &game.cars[index];
                slot = index;
            }
        }
        if (!car) continue;
        const int x = origin.x + cells[static_cast<std::size_t>(place)].x;
        const int y = origin.y + cells[static_cast<std::size_t>(place)].y;
        char carAsset[64];
        std::snprintf(carAsset, sizeof carAsset, "car_%02d", car->spec.id);
        add("../../common/dialogue/roster/base", "dialogue\\roster\\base.bmp", x, y, 1000);
        // The car picture and the place number keep their black backgrounds
        // in the original's roster (run 20261020-130220, checkpoint roster),
        // so they are not keyed by their top-left pixel.
        add(std::string("../../common/dialogue/roster/") + carAsset,
            std::string("dialogue\\roster\\") + carAsset + ".bmp", x + 11, y + 27, 1001,
            false, false);
        add("../../common/dialogue/roster/pos" + std::to_string(place + 1),
            "dialogue\\roster\\pos" + std::to_string(place + 1) + ".bmp", x + 30, y + 90,
            1001, false, false);
        const std::uint32_t time = game.aiCars[slot].raceTime;
        const std::string shown = 1.0F <= car->body.damage || 0x7ffffffeU < time
                                      ? std::string("-")
                                      : racing::formatRaceTime(time);
        game.rosterSprites.push_back(raceStageText("", SDL_Rect{x + 4, y + 131, 89, 22},
                                                   game.rosterTimeFont, shown, 1001));
        game.rosterSprites.push_back(raceStageText("", SDL_Rect{x + 4, y + 5, 89, 16},
                                                   game.rosterNameFont, car->name, 1001));
    }
}

void RacingScene::closeRaceRoster() {
    auto &game = *racingGame;
    for (const int sprite : game.rosterSprites) game.stage->removeIfActive(sprite);
    game.rosterSprites.clear();
    game.ending = RacingGame::Ending::None;
    finishRaceResults();
}

void RacingScene::finishRaceResults() {
    auto &game = *racingGame;
    // 0x0047751e-0x004775ba, after the roster: a race that was not quit, run
    // from the menu (block not the story's 0x0050aaf0) in mode 0, whose
    // slot-0 car set a lap (+0x258 below 0x7fffffff), submits its race time
    // (+0x25c) under +0x17c and its best lap under +0x180, both with no
    // medal table and flags 0x102 (0x0041ee50 at 0x00477580, 0x004775b5).
    // The rows land at 0x006d05d4 and 0x006d05f8 for the racing menu.
    if (game.state != 5 && !game.block.story && game.block.mode == 0 && !game.cars.empty() &&
        game.cars[0].bestLap < racing::kNoTime) {
        raceScoreRows.recorded = true;
        raceScoreRows.raceTime = submitSceneScore(game.aiCars[0].raceTime,
                                                  game.block.raceTimeKey, kScoreTimeFlags);
        raceScoreRows.lapTime =
            submitSceneScore(game.cars[0].bestLap, game.block.lapTimeKey, kScoreTimeFlags);
    }
    if (game.block.story) {
        // UNCERTAIN: the story race's exits (goodbye, race:TRYAGAIN) are
        // issue 255.
        enterScene("menu", "menu");
        return;
    }
    enterScene("racemenu", "racemenu");
}

void RacingScene::finishCrashedRace() {
    if (racingGame) endRace();
}

void RacingScene::confirmRacingQuit() {
    if (!racingGame) return;
    racingGame->state = 5;
    endRace();
}

bool RacingScene::handleRacingEvent(const SDL_Event &event) {
    if (!racingGame || activeScene != "racing") return false;
    if (racingGame->pendingBuild) return event.type != SDL_QUIT;
    auto &game = *racingGame;
    if (game.ending == RacingGame::Ending::Roster) {
        // 0x00422060: Enter or Escape closes the roster; so does its button
        // (0x116 -> 0x00422050). The mouse arrives in 800x600 coordinates
        // over the stretched 640x480 frame.
        if (event.type == SDL_KEYDOWN && !event.key.repeat &&
            (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER ||
             event.key.keysym.sym == SDLK_ESCAPE)) {
            closeRaceRoster();
            return true;
        }
        if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
            const SDL_Point point{event.button.x * kWidth / 800, event.button.y * kHeight / 600};
            const bool over = SDL_PointInRect(&point, &game.rosterButton);
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                game.rosterButtonDown = over;
            } else {
                const bool clicked = game.rosterButtonDown && over;
                game.rosterButtonDown = false;
                if (clicked) closeRaceRoster();
            }
        }
        return true;
    }
    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        // Player 1's default control setup (0x004a0150): the arrows, Ctrl
        // and Shift, which the frame polls (0x0040e630).
        const bool down = event.type == SDL_KEYDOWN;
        switch (event.key.keysym.sym) {
        case SDLK_UP: game.keys[0] = down; break;
        case SDLK_DOWN: game.keys[1] = down; break;
        case SDLK_LEFT: game.keys[2] = down; break;
        case SDLK_RIGHT: game.keys[3] = down; break;
        case SDLK_LCTRL: case SDLK_RCTRL: game.keys[4] = down; break;
        case SDLK_LSHIFT: case SDLK_RSHIFT: game.keys[5] = down; break;
        default: break;
        }
    }
    if (event.type == SDL_KEYDOWN) {
        const SDL_Keycode key = event.key.keysym.sym;
        if (key == SDLK_ESCAPE) {
            if (event.key.repeat) return true;
            // 0x0047a7f0: race:CONFIRM titled interfaceh:WARNING unless every
            // car is AI.
            if (game.block.mode != 1) {
                helpFrozenElapsed = animationTimeNow();
                showMessageWithTitle(
                    language.get("interfaceh", "WARNING", "Advarsel"),
                    language.get("race", "CONFIRM",
                                 "Er du helt sikker på at du vil avslutte dette billøpet?"),
                    MessageKind::RacingQuitConfirmation);
            } else {
                game.state = 5;
                endRace();
            }
            return true;
        }
        if (key == SDLK_TAB) return true;
        if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
            // At most once per 50 ms (0x006cf97c).
            static double nextAllowed = 0.0;
            if (game.clockMs >= nextAllowed) {
                nextAllowed = game.clockMs + 0x32;
                if (key == SDLK_PAGEUP && racingHudLevel > 0) --racingHudLevel;
                else if (key == SDLK_PAGEDOWN && racingHudLevel < 4) ++racingHudLevel;
            }
            return true;
        }
        return true;
    }
    if (event.type == SDL_KEYUP) return true;
    // The mouse does nothing in a race.
    if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
        event.type == SDL_MOUSEMOTION) {
        return true;
    }
    return false;
}

// 0x0047a0b0: the terrain under the car, a quarter-scale index map; an
// entry with repair above zero repairs and plays pitstop.
void RacingScene::sampleTerrain(RaceCar &car) {
    auto &game = *racingGame;
    const int columns = game.trackWidth / 4;
    const int rows = game.trackHeight / 4;
    int x = roundAway(static_cast<double>(car.body.x * 0.25F));
    int y = roundAway(static_cast<double>(car.body.y * 0.25F));
    x = x < 0 ? 0 : std::min(x, columns);
    y = y < 0 ? 0 : std::min(y, rows);
    // UNCERTAIN: the clamp lets the index reach the map's width and height,
    // one past its last texel, as the original's (0x0047a13d-0x0047a160)
    // does; native reads 0 there instead of the next row.
    std::uint8_t index = 0;
    const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(columns) +
                        static_cast<std::size_t>(x);
    if (offset < game.terrain.size()) index = game.terrain[offset];
    if (index > 0xf) index = 0;
    car.body.terrain = game.track.terrains[index];
    const bool hasRecord = &car == &game.cars[0] && game.block.mode != 2;
    if (car.body.terrain.repair != 0.0F) {
        // 0x0047a1ad: pitstop loops while the car is on the repair terrain.
        if (hasRecord) setRaceMessageClip("pitstop", true);
        racing::applyDamage(car.body, -(game.dt * car.body.terrain.repair * 0.1F));
        return;
    }
    if (hasRecord && game.message.name == "pitstop" && game.message.playing) {
        game.stage->remove(game.message.sprite);
        game.message.playing = false;
    }
}

// 0x00478bf0's sprite refresh: the damage sprite into the car atlas.
void RacingScene::refreshCarSprite(RaceCar &car) {
    if (!car.body.spriteChanged) return;
    auto &game = *racingGame;
    const auto &sprite = car.sprites[static_cast<std::size_t>(car.body.sprite)];
    bltFast(game.carAtlas.get(), car.atlasX, car.atlasY, sprite.surface.get(),
            SDL_Rect{0, 0, sprite.width, sprite.height}, false);
    car.body.spriteChanged = false;
}

// 0x00478f70: while slipping, a 2-pixel line from each wheel's last place
// to its new one, darkened into the track (0x0046f410, 0x0046f530).
void RacingScene::drawSkidMarks(RaceCar &car) {
    auto &game = *racingGame;
    const auto wheels = racing::carOutline(car.body, car.body.boxWidth - 10.0F,
                                           car.body.boxHeight - 20.0F);
    if (!car.body.slip) {
        car.wheels = wheels;
        car.wheelsBefore = wheels;
        return;
    }
    car.wheelsBefore = car.wheels;
    car.wheels = wheels;
    SDL_Surface *track = game.trackSurface.get();
    const auto darken = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= track->w || y >= track->h) return;
        std::uint32_t &pixel = pixelRow(track, y)[x];
        const auto p = static_cast<std::uint32_t>(((pixel >> 8) & 0xf800U) |
                                                  ((pixel >> 5) & 0x07e0U) |
                                                  ((pixel >> 3) & 0x001fU));
        constexpr std::uint32_t mask = 0x7bef;
        const std::uint32_t q = ((mask >> 1) & (p >> 2) & mask) + ((p >> 1) & mask);
        pixel = 0xff000000U | ((q & 0xf800U) << 8) | ((q & 0x07e0U) << 5) | ((q & 0x001fU) << 3);
    };
    for (std::size_t wheel = 0; wheel < 4; ++wheel) {
        const int x1 = static_cast<int>(car.wheelsBefore[wheel][0]);
        const int y1 = static_cast<int>(car.wheelsBefore[wheel][1]);
        const int x2 = static_cast<int>(car.wheels[wheel][0]);
        const int y2 = static_cast<int>(car.wheels[wheel][1]);
        if (x1 < 0 || y1 < 0 || x1 >= game.trackWidth || y1 >= game.trackHeight || x2 < 0 ||
            y2 < 0 || x2 >= game.trackWidth || y2 >= game.trackHeight)
            continue;
        // 0x0046f410 with width 2 (0x006cf95c): Bresenham, each step a
        // two-pixel run across the line starting one pixel before it.
        int dx = x2 - x1;
        int dy = y2 - y1;
        const int stepX = dx < 0 ? -1 : 1;
        const int stepY = dy < 0 ? -1 : 1;
        dx = std::abs(dx);
        dy = std::abs(dy);
        int x = x1;
        int y = y1;
        int error = 0;
        if (dy < dx) {
            for (int count = dx; count > 0; --count) {
                darken(x, y - 1);
                darken(x, y);
                error += dy;
                if (dy < error) {
                    error -= dx;
                    y += stepY;
                }
                x += stepX;
            }
        } else {
            for (int count = dy; count > 0; --count) {
                darken(x - 1, y);
                darken(x, y);
                error += dx;
                if (0 < error) {
                    error -= dy;
                    x += stepX;
                }
                y += stepY;
            }
        }
    }
}

// 0x00478bf0 for one slot.
void RacingScene::updateRaceCar(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    auto &body = car.body;
    // An AI car uses what it holds at once: turbo always, anything with
    // other cars about (0x00478c28-0x00478c40).
    if (body.ai && (car.held == 4 || 1 < game.block.cars_count)) usePickup(slot);
    // 0x00479d70: the free step, then the terrain under the new place.
    const float forwardBefore = body.forward;
    if (racing::freeStep(game.world, body, game.dt)) {
        sampleTerrain(car);
        if (body.trigger) {
            crossTrigger(slot);
        } else if (body.hit && 1.0F < forwardBefore &&
                   0x19 < game.clockMs - car.lastCrashMs) {
            // A crash (at 0.9 of the top speed or more, 0x0049db18) or a
            // spark at the hit point. UNCERTAIN: the crash and contact sounds
            // are issue 257.
            const int kind = body.effectiveTop * 0.9F <= forwardBefore ? 0 : 4;
            spawnParticle(static_cast<int>(body.hitX), static_cast<int>(body.hitY), kind, body.id);
            car.lastCrashMs = game.clockMs;
        }
    }
    refreshCarSprite(car);
    if (game.state == 3) {
        // The countdown: accelerate only revs the engine (0x00479080 with
        // one gear and no speed clamp).
        if (!body.ai) {
            racing::updateRevs(body, game.keys[0], 1, false, 0.0F, game.dt);
        } else {
            // Toward the slot's target (+0x2c, 0 at reset); on reaching it
            // exactly a new one is drawn.
            auto &record = game.aiSlots[slot];
            racing::updateRevs(body, body.revs < record.revTarget, 1, false, 0.0F, game.dt);
            if (body.revs == record.revTarget) {
                record.revTarget = 0.5F < record.revTarget
                                       ? static_cast<float>(crtRandom.below(0x19)) * 0.01F
                                       : static_cast<float>(crtRandom.below(0x19)) * 0.01F + 0.75F;
            }
        }
        return;
    }
    // Damage smoke above 0.25: a damageanim near the car each (1 - damage)
    // * 0.1 s (0x00478d39-0x00478de4).
    if (0.25F < body.damage) {
        car.smokeTimer -= game.dt;
        if (car.smokeTimer < 0.0F) {
            const int w = static_cast<int>(body.boxWidth);
            const int dx = crtRandom.below(2 * w) - w;
            const int x = dx / 2 + static_cast<int>(body.x);
            const int dy = crtRandom.below(2 * w) - w;
            const int y = dy / 2 + static_cast<int>(body.y);
            spawnParticle(x, y, 1, body.id);
            car.smokeTimer = (1.0F - body.damage) * 0.1F;
        }
    }
    if (game.state < 3) {
        // 0x004721f0: the path follower for every car, the AI for AI cars.
        auto world = raceAiWorld();
        racing::driveAi(world, static_cast<int>(slot), game.dt, game.state);
    }
    if (game.state < 3 && !body.ai) {
        // 0x00479c10: steer from Left and Right (Right last, so it wins),
        // then the pedals; both pedals coast.
        if (game.keys[2]) body.steer = 0;
        if (game.keys[3]) body.steer = 1;
        if (!game.keys[2] && !game.keys[3]) body.steer = 2;
        // Ctrl fires what the car holds (0x00479c10 -> 0x004798f0).
        if (game.keys[4]) usePickup(slot);
        const bool up = game.keys[0];
        const bool down = game.keys[1];
        if (up && !down) racing::throttle(body, game.dt, game.state);
        else if (down && !up) racing::brake(body, game.dt);
        else racing::coast(body, game.dt);
    }
    // UNCERTAIN: the slot's view rectangle (0x00478e70) only matters in
    // the split screen, issue 256.
    racing::steerCar(game.world, body, racing::steerDirection(body.steer), game.dt);
    racing::writeOutline(game.world, body, static_cast<int>(slot));
    racing::updateForwardSpeed(body, game.dt);
    collectPickups(slot);
    runTrick(slot);
    drawSkidMarks(car);
}

// 0x004776e0, the frame.
void RacingScene::updateRacing(double) {
    if (!racingGame || activeScene != "racing") return;
    if (racingGame->pendingBuild) buildRace();
    auto &game = *racingGame;
    if (game.ending == RacingGame::Ending::Award && dialogMode == DialogMode::None)
        openRaceRoster();
    if (game.ending == RacingGame::Ending::HiddenPart && dialogMode == DialogMode::None) {
        game.ending = RacingGame::Ending::None;
        sortRacePositions();
        if (game.state != 5) beginRaceResults();
        else finishRaceResults();
        return;
    }
    if (game.ended) return;
    // 0x0047a8b0: a popup sets the pause flag and the frame does nothing.
    if (dialogMode != DialogMode::None) return;
    const double now = raceClockNow();
    if (!game.clockStarted) {
        game.clockStarted = true;
        const double shift = now - game.clockMs;
        for (RaceClip *clip : {&game.count, &game.message}) {
            clip->startedAt += shift;
            clip->completesAt += shift;
        }
        game.lastMs += shift;
        game.blinkLast = static_cast<int>(now);
    }
    game.clockMs = now;
    ++game.frames;
    // 0x0047a490.
    float dt = static_cast<float>((game.clockMs - game.lastMs) * 0.001);
    if (dt < 0.0F) dt = 0.0F;
    if (dt > 0.1F) dt = 0.1F;
    game.dt = dt;
    game.lastMs = game.clockMs;

    // Timer 1 (0x004775d0, armed at GO every 10 ms): the service runs once
    // per pass with the pass's clock and, before state 2, writes the current
    // lap time of each HUD record's unfinished car (0x004775ec-0x00477647
    // walk the records at 0x0050ace8): slot 0's, the demo's AI car included.
    // UNCERTAIN: which records the split screen (mode 2, issue 256) gives
    // which slots; native takes its two human cars.
    if (game.lapClock && game.state < 2) {
        for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
            auto &car = game.cars[slot];
            const bool recorded = slot == 0 || (game.block.mode == 2 && slot == 1);
            if (!recorded || car.body.finished) continue;
            if (0 < car.lap && car.lap <= game.block.laps && car.lap <= 8)
                car.lapTimes[static_cast<std::size_t>(car.lap - 1)] =
                    static_cast<std::uint32_t>(game.clockMs - car.lapStartMs);
        }
    }

    // Timer 2 (0x004775d0, id 2): a random pickup of type rand(5) + 1.
    if (game.spawning && game.spawnDueMs <= game.clockMs) {
        game.spawnDueMs += kTimerTwo;
        spawnPickup(crtRandom.below(5) + 1);
    }

    // Steps 3-5: the outline cursor, every car's pre-pass, every slot.
    game.world.cursor = 0;
    for (auto &car : game.cars) {
        racing::sweepCar(game.world, car.body, dt, [&](int other, float x, float y) {
            if (other >= 0 && other < static_cast<int>(game.cars.size()))
                racing::carContact(car.body, game.cars[static_cast<std::size_t>(other)].body, x, y);
        });
    }
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot) {
        // 0x00478bf0's race time (+0x25c) while the race runs.
        if (game.state < 2 && !game.cars[slot].body.finished)
            game.aiCars[slot].raceTime = static_cast<std::uint32_t>(game.clockMs - game.startMs);
        updateRaceCar(slot);
    }
    // Step 6: progress, the order and the catch-up.
    if (game.state < 2) sortRacePositions();
    // Step 7.
    for (auto &car : game.cars) racing::updateEffective(car.body, dt);

    // The clips: a finished one stops (+0x78) and 0x110 removes it
    // (0x004770a0 -> 0x00476600).
    for (RaceClip *clip : {&game.count, &game.message}) {
        if (!clip->playing) continue;
        if (!clip->loop && game.clockMs >= clip->completesAt) {
            clip->playing = false;
            if (game.stage->active(clip->sprite)) game.stage->remove(clip->sprite);
            // 0x004770a0: with a human finished, a clip ending ends the race.
            if (game.state == 2) {
                endRace();
                return;
            }
        } else {
            showRaceClipFrame(*clip);
        }
    }

    // Step 8: the draw, after the particles move (0x00478840).
    updateParticles();
    drawRaceView();
    drawRaceHud();

    // Step 9: the countdown ends when count stops. Revs become wheel spin,
    // and any car's spin plays spinup on player 1's HUD (0x00478740).
    if (game.state == 3 && !game.count.playing) {
        // 0x0047795d: the state becomes 0 before the timers and the spin
        // loop, so GO is traced before spinup is added.
        game.state = 0;
        referenceTrace.emit("minigame_state", {{"game", "racing"}, {"state", "go"}});
        game.startMs = game.clockMs;
        game.lapClock = true;
        // Timer 2 with dirty tricks on (0x0047798e).
        game.spawning = game.block.tricks != 0U;
        game.spawnDueMs = game.clockMs + kTimerTwo;
        for (auto &car : game.cars) {
            car.lapStartMs = game.clockMs;
            racing::updateSpin(car.body, true, dt);
            if (game.block.mode != 2 && car.body.spin != 0.0F) setRaceMessageClip("spinup");
        }
        std::cout << "Race started at " << game.clockMs << " ms\n";
    }
    // Step 10: a human car wrecked ends the race.
    for (const auto &car : game.cars) {
        if (!car.body.ai && 1.0F <= car.body.damage) game.state = 4;
    }
    if (game.state == 4 && dialogMode == DialogMode::None) {
        // race:CRASHED titled race:TITLE, then the race ends (0x004770c0).
        helpFrozenElapsed = animationTimeNow();
        showMessageWithTitle(language.get("race", "TITLE", "Flåklypa Grand Prix"),
                             language.get("race", "CRASHED", ""),
                             MessageKind::RacingCrashed);
    }
}

racing::AiWorld RacingScene::raceAiWorld() {
    auto &game = *racingGame;
    racing::AiWorld world;
    world.path = &game.aiPath;
    world.collision = &game.world;
    world.cars = &game.aiCars;
    world.laps = game.block.laps;
    world.mode = game.block.mode;
    world.cameraX = game.cameraX;
    world.cameraY = game.cameraY;
    world.random = &crtRandom;
    // UNCERTAIN: race sound is issue 257; the horn is tracked only for how
    // long it sounds, which the AI's horn timer reads (0x00414240).
    world.sound = [&game](int car, int channel) {
        const auto slot = static_cast<std::size_t>(car);
        if (channel == 8) game.hornUntilMs[slot] = game.clockMs + game.hornLengthMs[slot];
        else if (channel == -8) game.hornUntilMs[slot] = 0.0;
    };
    world.hornPlaying = [&game](int car) {
        return game.clockMs < game.hornUntilMs[static_cast<std::size_t>(car)];
    };
    return world;
}

// Frame step 6: every car's progress (0x00477ae0), the order (0x004771a0
// over 0x006cf940), the catch-up factor 1 + min((leader - this) * 2.5, 0.5)
// and the position index.
void RacingScene::sortRacePositions() {
    auto &game = *racingGame;
    auto world = raceAiWorld();
    for (std::size_t slot = 0; slot < game.cars.size(); ++slot)
        game.aiCars[slot].progress = racing::raceProgress(world, static_cast<int>(slot));
    std::vector<std::size_t> order(game.cars.size());
    for (std::size_t slot = 0; slot < order.size(); ++slot) order[slot] = slot;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return racing::comparePositions(game.aiCars[a], game.aiCars[b]) < 0;
    });
    if (order.empty()) return;
    const float leader = game.aiCars[order[0]].progress;
    for (std::size_t place = 0; place < order.size(); ++place) {
        auto &car = game.cars[order[place]];
        float gain = (leader - game.aiCars[order[place]].progress) * racing::kCatchUpGain;
        if (0.5F <= gain) gain = 0.5F;
        car.body.catchUp = gain + 1.0F;
        car.position = static_cast<int>(place);
    }
}

void RacingScene::renderRacing() {
    if (!racingGame || activeScene != "racing" || racingGame->pendingBuild) return;
    auto &game = *racingGame;
    if (!game.target) return;
    if (!game.frameTexture) {
        game.frameTexture.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                  SDL_TEXTUREACCESS_STREAMING, kWidth,
                                                  kHeight));
    }
    // The back buffer changes every frame; it is copied in, not memoized
    // like the Smacker frames.
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(game.frameTexture.get(), nullptr, &pixels, &pitch) == 0) {
        for (int row = 0; row < kHeight; ++row) {
            std::memcpy(static_cast<std::uint8_t *>(pixels) + static_cast<std::ptrdiff_t>(row) * pitch,
                        pixelRow(game.frame.get(), row), static_cast<std::size_t>(kWidth) * 4U);
        }
        SDL_UnlockTexture(game.frameTexture.get());
    }
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, game.target.get());
    SDL_RenderCopy(renderer, game.frameTexture.get(), nullptr, nullptr);
    game.stage->render();
    SDL_SetRenderTarget(renderer, previous);
    // The capture host's DxWnd stretches the 640x480 mode over the 800x600
    // window, as the paired Mountain captures show.
    SDL_RenderCopy(renderer, game.target.get(), nullptr, nullptr);
}

bool RacingScene::executeRacingDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "racing:";
    if (command.rfind(prefix, 0) != 0) return false;
    const std::string_view rest = command.substr(prefix.size());
    if (rest == "status") {
        if (!racingGame) {
            std::cout << "racing: no race\n";
            return true;
        }
        const auto &game = *racingGame;
        std::cout << "racing: frames " << game.frames << " clock " << game.clockMs
                  << " state " << game.state << " track " << game.block.track
                  << " mode " << game.block.mode << " laps " << game.block.laps
                  << " camera " << game.cameraX << "," << game.cameraY;
        for (const auto &car : game.cars)
            std::cout << " [" << car.resource << " " << car.body.x << "," << car.body.y
                      << " h " << car.body.heading << " v " << car.body.forward << " d "
                      << car.body.damage << "]";
        int objects = 0, particles = 0;
        for (const auto &object : game.objects) objects += object.active ? 1 : 0;
        for (const auto &particle : game.particles) particles += particle.active ? 1 : 0;
        std::cout << " objects " << objects << " particles " << particles << " held";
        for (const auto &car : game.cars) std::cout << ' ' << car.held << '/' << car.trick;
        for (const auto &object : game.objects) {
            if (object.active && object.pickup && object.type == 6)
                std::cout << " hidden " << object.x << ',' << object.y;
        }
        std::cout << '\n';
        return true;
    }
    if (rest.rfind("keys=", 0) == 0 && racingGame) {
        // racing:keys=up,left holds those keys until the next keys=.
        racingGame->keys.fill(false);
        const std::string list(rest.substr(5));
        const std::array<const char *, 6> names{"up", "down", "left", "right", "ctrl", "shift"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (list.find(names[index]) != std::string::npos) racingGame->keys[index] = true;
        }
        return true;
    }
    if (rest == "await-go") {
        // Holds the command queue until GO (state 0).
        return racingGame && !racingGame->pendingBuild && racingGame->state == 0;
    }
    if (rest == "await-roster") {
        // Holds the command queue until the results' roster is open.
        return racingGame && racingGame->ending == RacingGame::Ending::Roster;
    }
    if (rest == "autopilot" && racingGame && !racingGame->cars.empty()) {
        // Developer only, not in the original: the AI drives slot 0's car
        // (its body counts as AI from here on), so a scripted run can finish
        // a race.
        // A human car's ini has no [bias] section read, so the AI's
        // defaults stand in (0x00475ce0's AI-only reads).
        racingGame->cars[0].body.ai = true;
        const auto defaults = racing::parseCarIni({}, true);
        auto &spec = racingGame->aiCars[0].spec;
        spec.lane = defaults.lane;
        spec.pickup = defaults.pickup;
        spec.car = defaults.car;
        spec.wall = defaults.wall;
        spec.trick = defaults.trick;
        spec.trackWeight = defaults.trackWeight;
        spec.horn = defaults.horn;
        return true;
    }
    if (rest.rfind("teleport=", 0) == 0 && racingGame && !racingGame->cars.empty()) {
        // Developer only, not in the original: racing:teleport=X,Y puts slot
        // 0's car there (to test the hidden part).
        const std::string value(rest.substr(9));
        const auto comma = value.find(',');
        if (comma == std::string::npos) return false;
        racingGame->cars[0].body.x = std::stof(value.substr(0, comma));
        racingGame->cars[0].body.y = std::stof(value.substr(comma + 1));
        return true;
    }
    if (rest.rfind("hud=", 0) == 0) {
        racingHudLevel = std::clamp(std::stoi(std::string(rest.substr(4))), 0, 4);
        return true;
    }
    throw std::runtime_error("Unknown developer racing command: " + std::string(command));
}

} // namespace fgp

// ---------------------------------------------------------------------------
// Pickups, dirty tricks and particles (issue 254).

namespace fgp {

namespace {

// 0x0050aa30: the particle table, 0x20 bytes an entry: the bitmap, its frame
// count, the draw layer, how many a trick drops and how far apart, and the
// speed's random spread.
struct ParticleKind {
    const char *name;
    int frames;
    int layer;
    int drops;
    float interval;
    float spread;
};
const auto kParticles = originalArray<ParticleKind, 6>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0050aa30U + static_cast<std::uint32_t>(index * 32);
    return ParticleKind{source.string(source.read<std::uint32_t>(address)).data(),
        source.read<int>(address + 8), source.read<int>(address + 12), source.read<int>(address + 16),
        source.read<float>(address + 20), source.read<float>(address + 28)};
});

// 0x00479a50: the particle a trick drops.
int trickParticle(int trick) {
    switch (trick) {
    case 1: return 2;
    case 2: return 5;
    case 3: return 3;
    default: return -1;
    }
}

} // namespace

// 0x00476810: the car holds `type` (0 none); with dirty tricks on, its HUD
// shows slot<type> in a 50x50 keyed surface.
void RacingScene::holdPickup(std::size_t slot, int type) {
    auto &game = *racingGame;
    game.cars[slot].held = type;
    if (game.block.tricks == 0U || slot != 0 || game.block.mode == 2) return;
    SurfacePtr icon = blankSurface(0x32, 0x32, 0xff00ff00U);
    const auto &sprite = game.slotSprites[static_cast<std::size_t>(std::clamp(type, 0, 5))];
    bltFast(icon.get(), 0, 0, sprite.surface.get(), SDL_Rect{0, 0, sprite.width, sprite.height},
            true);
    game.stage->setSurface(game.pickupIcon, std::move(icon));
    if (!game.stage->active(game.pickupIcon)) game.stage->add(game.pickupIcon, -1, -1, 1);
}

// 0x004798f0.
void RacingScene::usePickup(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    if (!(game.state < 2) || car.held == 0 || car.trick != 0) return;
    const bool record = slot == 0 && game.block.mode != 2;
    switch (car.held) {
    case 1:
    case 2:
    case 3: {
        const auto &kind = kParticles[static_cast<std::size_t>(trickParticle(car.held))];
        car.trickLeft = kind.drops;
        car.trickTimer = kind.interval;
        if (record) setRaceMessageClip("dirtytrick");
        break;
    }
    case 4:
        // Turbo: 1.2 (0x006ce8d4's 1200 times 0.001) for 8 s.
        car.body.boosting = true;
        car.trickLeft = 1;
        car.trickTimer = 8.0F;
        car.body.boost = 1200.0F * 0.001F;
        if (record) setRaceMessageClip(crtRandom.below(2) == 0 ? "turbo2" : "turbo1");
        break;
    case 5:
        if (car.body.damage == 0.0F) return;
        racing::applyDamage(car.body, -0.1F);
        break;
    default:
        break;
    }
    car.trick = car.held;
    holdPickup(slot, 0);
}

// 0x00479a80: a trick drops its particles behind the car until done.
void RacingScene::runTrick(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    if (car.trick == 0) return;
    car.trickTimer -= game.dt;
    if (car.trickTimer < 0.0F) {
        const int particle = trickParticle(car.trick);
        --car.trickLeft;
        if (particle >= 0) {
            const auto &sprite = car.sprites[static_cast<std::size_t>(car.body.sprite)];
            const double back = static_cast<double>(sprite.height) * 0.5;
            const double h = car.body.heading;
            const int x = roundAway(car.body.x - std::sin(h) * back);
            const int y = roundAway(car.body.y - std::cos(h) * back);
            car.trickTimer = kParticles[static_cast<std::size_t>(particle)].interval;
            if (car.trick == 3) {
                const int w = static_cast<int>(car.body.boxWidth);
                const int dx = crtRandom.below(2 * w) - w;
                const int dy = crtRandom.below(2 * w) - w;
                spawnParticle(dx / 2 + x, dy / 2 + y, 3, car.body.id);
            } else {
                spawnParticle(x, y, car.trick == 1 ? 2 : 5, car.body.id);
            }
        }
    }
    if (car.trickLeft < 1) {
        if (car.trick == 4) {
            car.body.boosting = false;
            car.body.boost = 1.0F;
        }
        car.trick = 0;
    }
}

// 0x00477650: a random pickup on a free path node, at most four at once.
void RacingScene::spawnPickup(int type) {
    auto &game = *racingGame;
    if (game.randomPickups >= 4 || game.aiPath.nodes.empty()) return;
    const int count = static_cast<int>(game.aiPath.nodes.size());
    const int start = crtRandom.below(count);
    int index = (start + 1) % count;
    if (index == start) return;
    while (game.aiPath.nodes[static_cast<std::size_t>(index)].pickup) {
        index = (index + 1) % count;
        if (index == start) return;
    }
    placePickup(index, type);
    game.aiPath.nodes[static_cast<std::size_t>(index)].pickup = true;
    ++game.randomPickups;
}

// 0x00476a80 then 0x004769f0: a pickup object centred on the node.
void RacingScene::placePickup(int node, int type) {
    auto &game = *racingGame;
    if (game.objects.size() >= 300) return;
    RaceObject object;
    const auto &at = game.aiPath.nodes[static_cast<std::size_t>(node)];
    const auto &sprite = game.pickupSprites[static_cast<std::size_t>(type)];
    object.node = node;
    object.type = type;
    object.pickup = true;
    object.active = true;
    object.x = static_cast<float>(at.x - sprite.width / 2);
    object.y = static_cast<float>(at.y - sprite.height / 2);
    game.objects.push_back(object);
}

// 0x00479560: a particle centred on (x, y), replacing the oldest when all
// 128 are in use; its speed is 1 +- spread / 2 from rand().
void RacingScene::spawnParticle(int x, int y, int type, int owner) {
    auto &game = *racingGame;
    std::size_t slot = 0;
    for (; slot < game.particles.size(); ++slot)
        if (!game.particles[slot].active) break;
    if (slot == game.particles.size()) {
        slot = 0;
        for (std::size_t index = 0; index < game.particles.size(); ++index)
            if (game.particles[index].startMs < game.particles[slot].startMs) slot = index;
    }
    const auto &kind = kParticles[static_cast<std::size_t>(type)];
    const auto &sprite = game.particleSprites[static_cast<std::size_t>(type)];
    auto &particle = game.particles[slot];
    particle.x = x - (sprite.width / kind.frames) / 2;
    particle.y = y - sprite.height / 2;
    particle.active = true;
    particle.type = type;
    particle.startMs = game.clockMs;
    particle.frame = 0.0F;
    particle.owner = owner;
    particle.speed = (static_cast<float>(crtRandom.next()) * (1.0F / 32768.0F) - 0.5F) *
                         kind.spread +
                     1.0F;
}

// 0x00478840: frames advance at 10 a second times the speed; damage smoke
// and trick smoke drift with the wind; finished oil and spikes stay as
// hazards (0x00478bb0).
void RacingScene::updateParticles() {
    auto &game = *racingGame;
    for (auto &particle : game.particles) {
        if (!particle.active) continue;
        particle.frame = particle.speed * 10.0F * game.dt + particle.frame;
        const auto &kind = kParticles[static_cast<std::size_t>(particle.type)];
        if (!(particle.frame < static_cast<float>(kind.frames))) {
            if (particle.type == 2 || particle.type == 5) {
                RaceObject hazard;
                hazard.x = static_cast<float>(particle.x);
                hazard.y = static_cast<float>(particle.y);
                hazard.active = true;
                hazard.pickup = false;
                hazard.particle = particle.type;
                hazard.owner = particle.owner;
                if (game.objects.size() < 300) game.objects.push_back(hazard);
            }
            particle.active = false;
        } else if (particle.type == 1 || particle.type == 3) {
            particle.x += static_cast<int>(static_cast<float>(game.windX) * game.dt);
            particle.y += static_cast<int>(static_cast<float>(game.windY) * game.dt);
        }
    }
}

// 0x004781e0: the particles of one layer, their current frame.
void RacingScene::drawParticles(int layer) {
    auto &game = *racingGame;
    for (const auto &particle : game.particles) {
        if (!particle.active) continue;
        const auto &kind = kParticles[static_cast<std::size_t>(particle.type)];
        if (kind.layer != layer) continue;
        const auto &sprite = game.particleSprites[static_cast<std::size_t>(particle.type)];
        const int width = sprite.width / kind.frames;
        const int frame = static_cast<int>(particle.frame);
        bltFast(game.frame.get(), particle.x - game.cameraX, particle.y - game.cameraY,
                sprite.surface.get(), SDL_Rect{width * frame, 0, width, sprite.height}, true);
        queueOverlaysOver(SDL_Rect{particle.x, particle.y, particle.x + width,
                                   particle.y + sprite.height});
    }
}

// 0x004783c0: pickups by their sprite, hazards by their particle's last
// frame.
void RacingScene::drawObjects() {
    auto &game = *racingGame;
    for (const auto &object : game.objects) {
        if (!object.active) continue;
        const int x = roundAway(object.x);
        const int y = roundAway(object.y);
        SDL_Rect area{};
        if (object.pickup) {
            const auto &sprite = game.pickupSprites[static_cast<std::size_t>(object.type)];
            bltFast(game.frame.get(), x - game.cameraX, y - game.cameraY, sprite.surface.get(),
                    SDL_Rect{0, 0, sprite.width, sprite.height}, true);
            area = SDL_Rect{x, y, x + sprite.width, y + sprite.height};
        } else {
            const auto &kind = kParticles[static_cast<std::size_t>(object.particle)];
            const auto &sprite = game.particleSprites[static_cast<std::size_t>(object.particle)];
            const int width = sprite.width / kind.frames;
            bltFast(game.frame.get(), x - game.cameraX, y - game.cameraY, sprite.surface.get(),
                    SDL_Rect{sprite.width - width, 0, width, sprite.height}, true);
            area = SDL_Rect{x, y, x + width, y + sprite.height};
        }
        queueOverlaysOver(area);
    }
}

// 0x00479650: an object whose rectangle meets the car's bounding box and
// crosses one of its outline edges. Oil spins a car at speed 100 or more
// (0x00479770); a spike slows it to 0.2 and damages it 0.075 and goes; a
// pickup is held, the hidden part (type 6) taken by a human only.
void RacingScene::collectPickups(std::size_t slot) {
    auto &game = *racingGame;
    auto &car = game.cars[slot];
    auto &body = car.body;
    const auto corners = racing::carOutline(body, body.boxWidth, body.boxHeight);
    float minX = corners[0][0], minY = corners[0][1], maxX = minX, maxY = minY;
    for (const auto &corner : corners) {
        minX = std::min(minX, corner[0]);
        minY = std::min(minY, corner[1]);
        maxX = std::max(maxX, corner[0]);
        maxY = std::max(maxY, corner[1]);
    }
    const int left = roundAway(minX), top = roundAway(minY);
    const int right = roundAway(maxX), bottom = roundAway(maxY);
    for (auto &object : game.objects) {
        if (!object.active) continue;
        int width = 0, height = 0;
        if (object.pickup) {
            width = game.pickupSprites[static_cast<std::size_t>(object.type)].width;
            height = game.pickupSprites[static_cast<std::size_t>(object.type)].height;
        } else {
            const auto &sprite = game.particleSprites[static_cast<std::size_t>(object.particle)];
            width = sprite.width / kParticles[static_cast<std::size_t>(object.particle)].frames;
            height = sprite.height;
        }
        const int ox = static_cast<int>(object.x), oy = static_cast<int>(object.y);
        const int ox2 = ox + width, oy2 = oy + height;
        // 0x00470780: inclusive overlap of the two boxes.
        if (!(left <= ox2 && top <= oy2 && ox <= right && oy <= bottom)) continue;
        // 0x00470bb0 through 0x00470a80: an outline edge crosses a side.
        bool crosses = false;
        const std::array<racing::WorldSegment, 4> sides{{
            {float(ox), float(oy), float(ox2), float(oy)},
            {float(ox2), float(oy), float(ox2), float(oy2)},
            {float(ox2), float(oy2), float(ox), float(oy2)},
            {float(ox), float(oy2), float(ox), float(oy)},
        }};
        for (std::size_t index = 0; index < 4 && !crosses; ++index) {
            const auto &from = corners[index];
            const auto &to = corners[(index + 1) & 3U];
            for (const auto &side : sides) {
                // A segment-against-segment test in slope form, as 0x00470880.
                const float aSpan = to[0] - from[0];
                const float bSpan = side.x2 - side.x1;
                const float aSlope = aSpan == 0.0F ? 0.0F : (to[1] - from[1]) / aSpan;
                const float bSlope = bSpan == 0.0F ? 0.0F : (side.y2 - side.y1) / bSpan;
                if (aSlope == bSlope) continue;
                const float aIntercept = from[1] - aSlope * from[0];
                const float bIntercept = side.y1 - bSlope * side.x1;
                float px = 0.0F, py = 0.0F;
                if (aSpan == 0.0F) {
                    px = from[0];
                    py = px * bSlope + bIntercept;
                } else {
                    px = bSpan == 0.0F ? side.x1 : (bIntercept - aIntercept) / (aSlope - bSlope);
                    py = px * aSlope + aIntercept;
                }
                const auto within = [&](float x1, float y1, float x2, float y2) {
                    float along = px - x1, span = x2 - x1;
                    if (span == 0.0F) {
                        along = py - y1;
                        span = y2 - y1;
                    }
                    const float t = along / span;
                    return t <= 1.0F && 0.0F <= t;
                };
                if (within(from[0], from[1], to[0], to[1]) &&
                    within(side.x1, side.y1, side.x2, side.y2)) {
                    crosses = true;
                    break;
                }
            }
        }
        if (!crosses) continue;
        if (!object.pickup) {
            if (object.particle == 2) {
                if (100.0F <= body.forward) {
                    if (std::fabs(body.pushX) < std::fabs(body.vx)) body.pushX += body.vx;
                    if (std::fabs(body.pushY) < std::fabs(body.vy)) body.pushY += body.vy;
                    if (std::fabs(body.angular) < std::fabs(body.forward * 0.025F)) {
                        const float sign = crtRandom.below(2) != 0 ? -1.0F : 1.0F;
                        body.angular = sign * body.forward * 0.025F;
                    }
                    body.vx = 0.0F;
                    body.vy = 0.0F;
                }
            } else if (object.particle == 5) {
                body.force = 0.0F;
                body.vx *= 0.2F;
                body.vy *= 0.2F;
                racing::applyDamage(body, 0.075F);
                object.active = false;
            }
            continue;
        }
        // UNCERTAIN: the pickup sound (channel 2) is issue 257.
        const auto remove = [&]() {
            object.active = false;
            if (object.node >= 0)
                game.aiPath.nodes[static_cast<std::size_t>(object.node)].pickup = false;
            --game.randomPickups;
        };
        if (object.type == 6) {
            if (body.ai) return;
            // 0x00479740: the track's record (0x402, keyed by its id) and the
            // flag the race end awards from.
            if (const auto path = activeProfilePath()) {
                try {
                    setProfileSceneRecordU32(*path, "racing", 0x402U,
                                             static_cast<std::uint32_t>(game.track.id), 1U);
                } catch (const std::exception &error) {
                    std::cerr << "Could not store the hidden car part: " << error.what() << '\n';
                }
            }
            game.hiddenPartTaken = true;
            remove();
        } else {
            holdPickup(slot, object.type);
            remove();
        }
    }
}

} // namespace fgp
