#pragma once

// The 16-bit surface arithmetic the jigsaw and sliding puzzles share. FGP.exe
// draws into RGB565 DirectDraw surfaces (0x00462500 and 0x0046bf80 test the
// green mask for 0x7e0), and both puzzles shade their pictures with the same
// height-field bevel from the jigsaw module (0x0046bd30, 0x0046c0f0).
// Implemented in puzzle_image.cpp.

#include <SDL.h>

#include <cstdint>
#include <vector>

#include "render/sdl_util.h"

namespace fgp {

// FGP's colour key 0xff00 (0x00404720) converted to RGB565.
constexpr std::uint16_t kPuzzleKey565 = 0x07e0U;

struct Rgb565Image {
    int width{};
    int height{};
    std::vector<std::uint16_t> pixels;

    std::uint16_t at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    std::uint16_t &at(int x, int y) {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};

// The two 800x800 byte planes (stride 0x320) the bevel reads.
constexpr int kHeightFieldStride = 800;
constexpr std::size_t kHeightFieldSize = 640000;
using HeightField = std::vector<std::uint8_t>;

// A picture as the original holds it once DirectDraw converted it to the
// 16-bit display format: every channel truncated to 5/6/5 bits.
Rgb565Image rgb565FromSurface(SDL_Surface *surface);
// 0x00462500 / 0x0046bf80: no bit replication on expansion.
std::uint32_t rgb565To888(std::uint16_t pixel);
std::uint16_t rgb888To565(std::uint32_t rgb);
// 0x0046bfe0: darken below 0x80, lighten above it.
std::uint32_t shadeRgb888(std::uint32_t rgb, std::uint8_t value);

// 0x0046c0f0: one pass of the four-neighbour box blur over an 800x800 plane.
void blurHeightField(HeightField &destination, const HeightField &source);
// 0x0046bd30: light the picture from the height field's gradient. Direction
// 0..3 is the jigsaw's per-rotation light, 4 the sliding puzzle's.
void bevelImage(Rgb565Image &image, const HeightField &height, int direction);
// 0x00401f70 -> 0x00402060 transforms 0..3: r quarter turns clockwise.
Rgb565Image rotateImage(const Rgb565Image &image, int quarterTurns);

// An SDL surface for drawing; keyed pixels become transparent.
SurfacePtr surfaceFromRgb565(const Rgb565Image &image, bool keyed);

} // namespace fgp
