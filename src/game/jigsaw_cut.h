#pragma once

// The jigsaw piece generator, FGP.exe 0x0046c1b0: random interlocking
// Hermite-curve edges drawn into a label plane, a flood fill per cell, a
// validity test with retry, a blurred height field for the bevel, and four
// pre-rotated piece sets. Pure logic, implemented in jigsaw_cut.cpp.

#include <array>
#include <cstdint>
#include <vector>

#include "game/picture_library.h"
#include "game/puzzle_image.h"

namespace fgp {

struct JigsawPieceImage {
    Rgb565Image image;
    // Descriptor +0x54/+0x58: the bounding box origin inside the rotated
    // 490x490 picture, i.e. the piece's home position for that rotation.
    int homeX{};
    int homeY{};
};

struct JigsawCut {
    int grid{};
    // Every attempt drew new edges; the first attempt's draw is attempt 1.
    int attempts{};
    // [rotation][piece], piece = row * grid + column (0x0046b5d4).
    std::array<std::vector<JigsawPieceImage>, 4> rotations;
};

// The picture side the generator assumes (0x1ea, pushed at 0x0046c23e).
constexpr int kJigsawPictureSize = 490;

// Runs 0x0046c1b0 on a 490x490 picture with `random` holding the C runtime's
// per-thread rand() state. Every rand() call the original makes, including
// the srand(rand()) at 0x0046c22d/0x0046c3dc, advances `random` the same way.
JigsawCut cutJigsaw(const Rgb565Image &picture, int grid, MsvcRandom &random);

// The label plane alone, for tests: 0x0046c3d0 + 0x0046ced0 + 0x0046d0c0.
// Returns false when 0x0046d0c0 would reject the cut.
bool drawJigsawLabels(std::vector<std::uint8_t> &labels,
                      std::vector<std::uint8_t> &height, int grid,
                      MsvcRandom &random);

} // namespace fgp
