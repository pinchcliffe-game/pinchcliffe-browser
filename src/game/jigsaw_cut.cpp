#include "game/jigsaw_cut.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fgp {
namespace {

constexpr int kStride = kHeightFieldStride;
constexpr int kSize = kJigsawPictureSize;
// 0x0049db4c: the guide point 30 pixels before each edge's start.
constexpr float kGuideOffset = 30.0F;
// 0x0049ff60: the curve parameter step, 1/399, and 400 samples per segment.
constexpr float kCurveStep = 1.0F / 399.0F;
constexpr int kCurveSamples = 400;
// 0x0049da3c: the Hermite tangent scale.
constexpr float kTension = 0.7F;
// 0x43f50000: 490.0f, the far border.
constexpr float kFarBorder = 490.0F;

// One grid point of 0x0046c3d0: two nine-point edge curves, 0x90 bytes.
// `down` (+0x00..+0x44) runs from the point down the cell's left side,
// `across` (+0x48..+0x8c) from the point across the cell's top.
struct EdgePoint {
    std::array<float, 18> down{};
    std::array<float, 18> across{};
};

// Integer divisions of the picture side by multiples of the grid, each a
// separate 64-bit division in the original (`0x1ea / (longlong)(n * k)`).
int part(int grid, int multiple) { return kSize / (grid * multiple); }

// 0x0046cc50: the vertical edge. `sign` is +1 for direction 0, -1 for 1.
void randomiseDown(EdgePoint &point, int grid, int sign, MsvcRandom &random) {
    auto &f = point.down;
    const float x0 = f[2];
    const float y0 = f[3];
    const int u7 = part(grid, 7);
    const int u8 = part(grid, 8);
    f[5] = static_cast<float>(random.next() % u7) +
           static_cast<float>(u7 * 2) + y0;
    f[4] = static_cast<float>((random.next() % u8) * sign) + x0;
    f[7] = static_cast<float>(random.next() % u8) +
           static_cast<float>(part(grid, 4)) + y0;
    f[6] = x0 - static_cast<float>(part(grid, 5) * sign);
    f[9] = static_cast<float>(part(grid, 2)) + y0;
    f[8] = (x0 - static_cast<float>(part(grid, 3) * sign)) +
           static_cast<float>((random.next() % part(grid, 12)) * sign);
    f[11] = (static_cast<float>(part(grid, 4) * 3) + y0) -
            static_cast<float>(random.next() % u8);
    f[10] = x0 - static_cast<float>(part(grid, 5) * sign);
    f[13] = static_cast<float>(random.next() % u7) +
            static_cast<float>(u7 * 5) + y0;
    f[12] = static_cast<float>((random.next() % u8) * sign) + x0;
    f[14] = x0;
    f[15] = static_cast<float>(part(grid, 1)) + y0;
    f[16] = x0;
    f[17] = static_cast<float>(part(grid, 1) * 2) + y0;
}

// 0x0046c9a0: the horizontal edge, the same shape with the axes swapped
// except that its sixth point subtracts the random term (0x0046cb7a).
void randomiseAcross(EdgePoint &point, int grid, int sign,
                     MsvcRandom &random) {
    auto &f = point.across;
    const float x0 = f[2];
    const float y0 = f[3];
    const int u7 = part(grid, 7);
    const int u8 = part(grid, 8);
    f[4] = static_cast<float>(random.next() % u7) +
           static_cast<float>(u7 * 2) + x0;
    f[5] = static_cast<float>((random.next() % u8) * sign) + y0;
    f[6] = static_cast<float>(random.next() % u8) +
           static_cast<float>(part(grid, 4)) + x0;
    f[7] = y0 - static_cast<float>(part(grid, 5) * sign);
    f[8] = static_cast<float>(part(grid, 2)) + x0;
    f[9] = (y0 - static_cast<float>(part(grid, 3) * sign)) +
           static_cast<float>((random.next() % part(grid, 12)) * sign);
    f[10] = (static_cast<float>(part(grid, 4) * 3) + x0) -
            static_cast<float>(random.next() % u8);
    f[11] = y0 - static_cast<float>(part(grid, 5) * sign);
    f[12] = (static_cast<float>(part(grid, 7) * 5) + x0) -
            static_cast<float>(random.next() % u7);
    f[13] = static_cast<float>((random.next() % u8) * sign) + y0;
    f[14] = static_cast<float>(part(grid, 1)) + x0;
    f[15] = y0;
    f[16] = static_cast<float>(part(grid, 1) * 2) + x0;
    f[17] = y0;
}

// 0x0046c820 with two dimensions: Hermite from p1 to p2, tangents
// 0.7 * (p1 - p0) and 0.7 * (p3 - p2). The basis weights and the result are
// stored as floats.
std::array<float, 2> hermite(const float *p0, const float *p1,
                             const float *p2, const float *p3, float t) {
    const double t2 = static_cast<double>(t) * t;
    const double t3 = t2 * t;
    const auto h00 = static_cast<float>((t3 + t3 - t2 * 3.0) + 1.0);
    const auto h01 = static_cast<float>(t2 * 3.0 - (t3 + t3));
    const auto h10 = static_cast<float>((t3 - (t2 + t2)) + t);
    const auto h11 = static_cast<float>(t3 - t2);
    std::array<float, 2> out{};
    for (int d = 0; d < 2; ++d) {
        const auto m1 = static_cast<float>(
            (static_cast<double>(p1[d]) - p0[d]) * kTension);
        const auto m2 = static_cast<float>(
            (static_cast<double>(p3[d]) - p2[d]) * kTension);
        out[static_cast<std::size_t>(d)] = static_cast<float>(
            ((static_cast<double>(h10) * m1 + static_cast<double>(h11) * m2) +
             static_cast<double>(h00) * p1[d]) +
            static_cast<double>(h01) * p2[d]);
    }
    return out;
}

// 0x0046c66c-0x0046c6a1: truncate (MSVC _ftol) and clamp to the plane.
int plot(float value) {
    return std::clamp(static_cast<int>(value), 0, kStride - 1);
}

// 0x0046ced0 -> 0x0046cf40: flood fill the zero pixels connected to a cell
// centre with the cell's label, inside the 490x490 picture (0x0046d060).
void fillCell(std::vector<std::uint8_t> &labels, int x, int y,
              std::uint8_t value) {
    const auto open = [&](int px, int py) {
        return px >= 0 && px < kSize && py >= 0 && py < kSize &&
               labels[static_cast<std::size_t>(py) * kStride + px] == 0U;
    };
    if (!open(x, y)) return;
    std::vector<std::pair<int, int>> frontier{{x, y}};
    labels[static_cast<std::size_t>(y) * kStride + x] = value;
    while (!frontier.empty()) {
        const auto [px, py] = frontier.back();
        frontier.pop_back();
        for (const auto &[nx, ny] : {std::pair{px - 1, py}, std::pair{px + 1, py},
                                     std::pair{px, py - 1}, std::pair{px, py + 1}}) {
            if (!open(nx, ny)) continue;
            labels[static_cast<std::size_t>(ny) * kStride + nx] = value;
            frontier.emplace_back(nx, ny);
        }
    }
}

// 0x0046d0c0. UNCERTAIN: for x == 0 on row 0 and for rows 0 and 1 the
// original reads bytes before the plane's heap block; they are taken as
// non-zero here, as allocator headers are.
bool acceptLabels(std::vector<std::uint8_t> &labels) {
    const auto at = [&](long index) -> int {
        if (index < 0) return 1;
        return labels[static_cast<std::size_t>(index)];
    };
    for (long y = 0; y < kSize; ++y) {
        const long row = y * kStride;
        for (long x = 0; x < kSize; ++x) {
            if (labels[static_cast<std::size_t>(row + x)] != 0U) continue;
            const int right = at(row + x + 1);
            // 0x0046d0f9: the fourth neighbour is two rows up.
            if (right == 0 || at(row + x - 1) == 0 ||
                at(row + kStride + x) == 0 || at(row - 2 * kStride + x) == 0) {
                return false;
            }
            labels[static_cast<std::size_t>(row + x)] =
                static_cast<std::uint8_t>(right);
        }
    }
    return true;
}

} // namespace

bool drawJigsawLabels(std::vector<std::uint8_t> &labels,
                      std::vector<std::uint8_t> &height, int grid,
                      MsvcRandom &random) {
    labels.assign(kHeightFieldSize, 0U);
    height.assign(kHeightFieldSize, 0x80U);
    // 0x0046c22d -> 0x0046c3dc: srand(rand()).
    random.seed(static_cast<std::uint32_t>(random.next()));
    const int points = grid + 4;
    std::vector<std::vector<EdgePoint>> grid2(
        static_cast<std::size_t>(points),
        std::vector<EdgePoint>(static_cast<std::size_t>(points)));
    const auto unit = static_cast<float>(kSize / grid);
    for (int a = 0; a < points; ++a) {
        for (int b = 0; b < points; ++b) {
            auto &point = grid2[static_cast<std::size_t>(a)]
                               [static_cast<std::size_t>(b)];
            const float x = unit * static_cast<float>(b);
            const float y = unit * static_cast<float>(a);
            point.across[2] = x;
            point.across[3] = y;
            point.down[2] = x;
            point.down[3] = y;
            point.down[0] = x;
            point.down[1] = y - kGuideOffset;
            point.across[0] = x - kGuideOffset;
            point.across[1] = y;
            // 0x0046c940: rand() % 2 picks each edge's tab direction.
            const int downSign = random.next() % 2 == 0 ? 1 : -1;
            randomiseDown(point, grid, downSign, random);
            const int acrossSign = random.next() % 2 == 0 ? 1 : -1;
            randomiseAcross(point, grid, acrossSign, random);
            // 0x0046c52d-0x0046c59d: the four borders are straight.
            if (a == 0) {
                for (int k = 0; k < 9; ++k) point.across[2 * k + 1] = 0.0F;
            }
            if (b == 0) {
                for (int k = 0; k < 9; ++k) point.down[2 * k] = 0.0F;
            }
            if (a == grid) {
                for (int k = 0; k < 9; ++k) point.across[2 * k + 1] = kFarBorder;
            }
            if (b == grid) {
                for (int k = 0; k < 9; ++k) point.down[2 * k] = kFarBorder;
            }
        }
    }
    // 0x0046c5cb-0x0046c7dc: draw every edge with its cell's label; the last
    // column and row reuse the previous cell's label.
    std::uint8_t label = 100;
    for (int a = 0; a <= grid; ++a) {
        for (int b = 0; b <= grid; ++b) {
            const auto &point = grid2[static_cast<std::size_t>(a)]
                                     [static_cast<std::size_t>(b)];
            for (int segment = 0; segment < 6; ++segment) {
                for (int sample = 0; sample < kCurveSamples; ++sample) {
                    const auto t = static_cast<float>(
                        static_cast<double>(sample) * kCurveStep);
                    const float *across = point.across.data() + segment * 2;
                    const auto h = hermite(across, across + 2, across + 4,
                                           across + 6, t);
                    const int hx = plot(h[0]);
                    const int hy = plot(h[1]);
                    const std::size_t hi =
                        static_cast<std::size_t>(hy) * kStride + hx;
                    labels[hi] = label;
                    height[hi] = 0U;
                    if (hi + 1 < height.size()) height[hi + 1] = 0U;
                    const float *down = point.down.data() + segment * 2;
                    const auto v = hermite(down, down + 2, down + 4, down + 6, t);
                    const int vx = plot(v[0]);
                    const int vy = plot(v[1]);
                    const std::size_t vi =
                        static_cast<std::size_t>(vy) * kStride + vx;
                    labels[vi] = label;
                    height[vi] = 0U;
                    // 0x0046c76a writes the pixel below; past the last row it
                    // lands outside the plane, which is dropped here.
                    if (vi + kStride < height.size()) height[vi + kStride] = 0U;
                }
            }
            if (b != grid - 1) ++label;
            if (a == grid - 1 && b == grid) label = static_cast<std::uint8_t>(label - grid);
        }
    }
    // 0x0046ced0: one seed per cell centre, labels 100 + row * grid + column.
    const int half = kSize / (grid * 2);
    const int step = kSize / grid;
    std::uint8_t cell = 100;
    for (int row = 0; row < grid; ++row) {
        for (int column = 0; column < grid; ++column) {
            fillCell(labels, step * column + half, step * row + half, cell++);
        }
    }
    return acceptLabels(labels);
}

JigsawCut cutJigsaw(const Rgb565Image &picture, int grid, MsvcRandom &random) {
    if (picture.width != kSize || picture.height != kSize) {
        throw std::runtime_error("Jigsaw pictures are 490x490");
    }
    JigsawCut cut;
    cut.grid = grid;
    std::vector<std::uint8_t> labels;
    HeightField height;
    do {
        ++cut.attempts;
    } while (!drawJigsawLabels(labels, height, grid, random));
    // 0x0046c276-0x0046c2a4: eight blur passes, ending in the 0x006bfbc8 plane.
    HeightField scratch(kHeightFieldSize);
    for (int pass = 0; pass < 4; ++pass) {
        blurHeightField(scratch, height);
        blurHeightField(height, scratch);
    }
    // 0x0046d140: the label plane as a 490x490 16-bit surface.
    Rgb565Image labelImage;
    labelImage.width = kSize;
    labelImage.height = kSize;
    labelImage.pixels.resize(static_cast<std::size_t>(kSize) * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            labelImage.at(x, y) = labels[static_cast<std::size_t>(y) * kStride + x];
        }
    }
    const int pieces = grid * grid;
    for (int rotation = 0; rotation < 4; ++rotation) {
        // 0x0046c2c3-0x0046c349: shade a copy for this rotation's light,
        // then rotate both the labels and the shaded picture.
        Rgb565Image shaded = picture;
        bevelImage(shaded, height, rotation);
        const Rgb565Image rotatedLabels = rotateImage(labelImage, rotation);
        const Rgb565Image rotatedPicture = rotateImage(shaded, rotation);
        auto &set = cut.rotations[static_cast<std::size_t>(rotation)];
        set.resize(static_cast<std::size_t>(pieces));
        // 0x0046d1f0: each label's bounding box becomes one keyed piece.
        for (int piece = 0; piece < pieces; ++piece) {
            const std::uint16_t value = static_cast<std::uint16_t>(100 + piece);
            int minX = -1, minY = -1, maxX = -1, maxY = -1;
            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    if (rotatedLabels.at(x, y) != value) continue;
                    if (minY < 0) {
                        minX = x;
                        minY = y;
                        maxX = x + 1;
                        maxY = y + 1;
                    } else if (x < minX) {
                        minX = x;
                    }
                    maxX = std::max(maxX, x + 1);
                    maxY = std::max(maxY, y + 1);
                }
            }
            auto &out = set[static_cast<std::size_t>(piece)];
            if (minY < 0) {
                // A label with no pixels cannot pass 0x0046d0c0; keep a
                // one-pixel transparent piece rather than an empty surface.
                minX = minY = 0;
                maxX = maxY = 1;
            }
            out.homeX = minX;
            out.homeY = minY;
            out.image.width = maxX - minX;
            out.image.height = maxY - minY;
            out.image.pixels.assign(
                static_cast<std::size_t>(out.image.width) * out.image.height,
                kPuzzleKey565);
            for (int y = 0; y < out.image.height; ++y) {
                for (int x = 0; x < out.image.width; ++x) {
                    const int sx = minX + x;
                    const int sy = minY + y;
                    if (sx < kSize && sy < kSize &&
                        rotatedLabels.at(sx, sy) == value) {
                        out.image.at(x, y) = rotatedPicture.at(sx, sy);
                    }
                }
            }
            // 0x0046d3f3: the first pixel always carries the key.
            out.image.pixels.front() = kPuzzleKey565;
        }
    }
    return cut;
}

} // namespace fgp
