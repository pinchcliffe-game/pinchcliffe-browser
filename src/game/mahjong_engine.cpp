#include "game/mahjong_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#include <stdexcept>

namespace fgp::mahjong {
namespace {

// 0x0049f650: the float degree-to-radian constant the rotation builders use.
constexpr float kDegreesToRadians = 0.01745329238474369f;
// 0x0049f65c: one third, as the float the face-depth average multiplies by.
constexpr float kOneThird = 0.3333333432674408f;
constexpr double kFixedOne = 65536.0;              // 0x0049da4c

// One corner of a triangle as the fillers take it: seven floats, 0x1c bytes
// (screen x, y, z, texture u, v and second-texture s, t; all 16.16 scaled
// texture coordinates).
struct RasterVertex {
    float x{};
    float y{};
    float z{};
    float u{};
    float v{};
    float s{};
    float t{};
};

// MSVC's _ftol (0x0048c214) truncates toward zero.
std::int32_t ftol(double value) {
    return static_cast<std::int32_t>(value);
}

// 0x00455fa0: ceiling of a 16.16 value.
std::int32_t ceil16(std::int32_t value) {
    return (value + 0xffff) >> 16;
}

// 0x00455f80: (a * b) >> 16, keeping the low 32 bits.
std::int32_t mul16(std::int32_t a, std::int32_t b) {
    const auto product = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b));
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(product >> 16));
}

// 0x004568d0: (a * b) >> 14, keeping the low 32 bits.
std::int32_t mul14(std::int32_t a, std::int32_t b) {
    const auto product = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b));
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(product >> 14));
}

// 0x00455f50: (a << 16) / b with the divisor raised to at least 0x7fff.
std::int32_t div16(std::int32_t a, std::int32_t b) {
    if (b < 0x7fff) {
        b = 0x7fff;
    }
    return static_cast<std::int32_t>((static_cast<std::int64_t>(a) * 65536) /
                                     static_cast<std::int64_t>(b));
}

// 0x40000000 / d, as the fillers compute the one-row reciprocal.
std::int32_t reciprocal30(std::int32_t d) {
    return static_cast<std::int32_t>(0x40000000LL / static_cast<std::int64_t>(d));
}

// The slope rule every filler uses: a one-row span multiplies by the
// reciprocal (0x004568d0), anything else divides (0x00455f50).
std::int32_t edgeSlope(std::int32_t delta, std::int32_t span, bool oneRow) {
    return oneRow ? mul14(delta, reciprocal30(span)) : div16(delta, span);
}

// The row/attribute state of one triangle corner in 16.16 fixed point.
struct FixedCorner {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t u{};
    std::int32_t v{};
    std::int32_t s{};
    std::int32_t t{};
};

// One edge walker: the current x and attributes, their per-row steps and
// the rows left on this edge.
struct EdgeWalker {
    std::int32_t x{};
    std::int32_t dx{};
    std::int32_t u{};
    std::int32_t du{};
    std::int32_t v{};
    std::int32_t dv{};
    std::int32_t s{};
    std::int32_t ds{};
    std::int32_t t{};
    std::int32_t dt{};
    std::int32_t rows{};
};

// Sorts three corners by y exactly as the fillers do: a strict swap of the
// first two, then of the last two, then of the first two again.
void sortCorners(FixedCorner &a, FixedCorner &b, FixedCorner &c) {
    if (b.y < a.y) {
        std::swap(a, b);
    }
    if (c.y < b.y) {
        std::swap(b, c);
    }
    if (b.y < a.y) {
        std::swap(a, b);
    }
}

// The edge setup shared by the three fillers (0x00456370, 0x004568f0 and
// 0x00456e20). On return `left` walks a->b and `right` a->c, swapped so
// that the left edge has the smaller slope; `b` and `c` are swapped with
// them, so `b` always ends the left edge and `c` the right one. `attributes`
// says whether u/v (and `pair` s/t) are interpolated along the left edge.
void setupEdges(FixedCorner &a, FixedCorner &b, FixedCorner &c,
                EdgeWalker &left, EdgeWalker &right, std::int32_t &totalRows,
                bool attributes, bool pair) {
    const std::int32_t rowsAB = ceil16(b.y) - ceil16(a.y);
    const std::int32_t slopeAB = edgeSlope(b.x - a.x, b.y - a.y, rowsAB == 1);
    const std::int32_t xAB = mul16(slopeAB, ceil16(a.y) * 0x10000 - a.y) + a.x;
    const std::int32_t rowsAC = ceil16(c.y) - ceil16(a.y);
    const std::int32_t slopeAC = edgeSlope(c.x - a.x, c.y - a.y, rowsAC == 1);
    const std::int32_t xAC = mul16(slopeAC, ceil16(a.y) * 0x10000 - a.y) + a.x;
    totalRows = rowsAC;

    left.dx = slopeAB;
    left.x = xAB;
    right.dx = slopeAC;
    right.x = xAC;
    right.rows = rowsAC;
    if (slopeAC < slopeAB) {
        std::swap(b, c);
        left.dx = slopeAC;
        left.x = xAC;
        right.dx = slopeAB;
        right.x = xAB;
        right.rows = rowsAB;
    }

    left.rows = ceil16(b.y) - ceil16(a.y);
    if (attributes) {
        const std::int32_t span = b.y - a.y;
        const bool oneRow = left.rows == 1;
        left.du = edgeSlope(b.u - a.u, span, oneRow);
        left.dv = edgeSlope(b.v - a.v, span, oneRow);
        if (pair) {
            left.ds = edgeSlope(b.s - a.s, span, oneRow);
            left.dt = edgeSlope(b.t - a.t, span, oneRow);
        }
        const std::int32_t prestep = ceil16(a.y) * 0x10000 - a.y;
        left.u = mul16(left.du, prestep) + a.u;
        left.v = mul16(left.dv, prestep) + a.v;
        if (pair) {
            left.s = mul16(left.ds, prestep) + a.s;
            left.t = mul16(left.dt, prestep) + a.t;
        }
    }
}

// The left edge ran out of rows: continue along b->c. Nothing changes when
// that edge has no rows either (the caller's x then stays where it was).
void restartLeft(const FixedCorner &b, const FixedCorner &c,
                 EdgeWalker &left, bool attributes, bool pair) {
    left.rows = ceil16(c.y) - ceil16(b.y);
    if (left.rows <= 0) {
        return;
    }
    std::int32_t span = c.y - b.y;
    const bool oneRow = left.rows == 1;
    if (oneRow && span == 0) {
        span = 1;
    }
    left.dx = edgeSlope(c.x - b.x, span, oneRow);
    if (attributes) {
        left.du = edgeSlope(c.u - b.u, span, oneRow);
        left.dv = edgeSlope(c.v - b.v, span, oneRow);
        if (pair) {
            left.ds = edgeSlope(c.s - b.s, span, oneRow);
            left.dt = edgeSlope(c.t - b.t, span, oneRow);
        }
    }
    const std::int32_t prestep = ceil16(b.y) * 0x10000 - b.y;
    left.x = mul16(left.dx, prestep) + b.x;
    if (attributes) {
        left.u = mul16(left.du, prestep) + b.u;
        left.v = mul16(left.dv, prestep) + b.v;
        if (pair) {
            left.s = mul16(left.ds, prestep) + b.s;
            left.t = mul16(left.dt, prestep) + b.t;
        }
    }
}

// The right edge ran out of rows: continue along c->b.
void restartRight(const FixedCorner &b, const FixedCorner &c,
                  EdgeWalker &right) {
    right.rows = ceil16(b.y) - ceil16(c.y);
    if (right.rows <= 0) {
        return;
    }
    std::int32_t span = b.y - c.y;
    const bool oneRow = right.rows == 1;
    if (oneRow && span == 0) {
        span = 1;
    }
    right.dx = edgeSlope(b.x - c.x, span, oneRow);
    right.x = mul16(right.dx, ceil16(c.y) * 0x10000 - c.y) + c.x;
}

// Advances both edges by one row (the tail of every filler's row loop).
void stepEdges(const FixedCorner &b, const FixedCorner &c, EdgeWalker &left,
               EdgeWalker &right, bool attributes, bool pair) {
    if (left.rows < 2) {
        restartLeft(b, c, left, attributes, pair);
    } else {
        left.x += left.dx;
        if (attributes) {
            left.u += left.du;
            left.v += left.dv;
            if (pair) {
                left.s += left.ds;
                left.t += left.dt;
            }
        }
        --left.rows;
    }
    if (right.rows < 2) {
        restartRight(b, c, right);
    } else {
        right.x += right.dx;
        --right.rows;
    }
}

// The initial edge-row checks right after setup: an edge with no rows of
// its own starts on its second half immediately.
void startEdges(const FixedCorner &b, const FixedCorner &c, EdgeWalker &left,
                EdgeWalker &right, bool attributes, bool pair) {
    if (left.rows < 1) {
        restartLeft(b, c, left, attributes, pair);
    }
    if (right.rows < 1) {
        restartRight(b, c, right);
    }
}

FixedCorner fixedCorner(const RasterVertex &vertex, double scale) {
    FixedCorner corner;
    corner.x = ftol(static_cast<double>(vertex.x) * scale * kFixedOne);
    corner.y = ftol(static_cast<double>(vertex.y) * scale * kFixedOne);
    return corner;
}

// 0x00456370: fills the triangle into the half-resolution id buffer.
void fillIds(const std::array<RasterVertex, 3> &vertices, std::uint8_t id,
             std::uint8_t *ids, int halfWidth) {
    FixedCorner a = fixedCorner(vertices[0], 0.5);
    FixedCorner b = fixedCorner(vertices[1], 0.5);
    FixedCorner c = fixedCorner(vertices[2], 0.5);
    sortCorners(a, b, c);
    EdgeWalker left;
    EdgeWalker right;
    std::int32_t rows = 0;
    setupEdges(a, b, c, left, right, rows, false, false);
    startEdges(b, c, left, right, false, false);
    std::uint8_t *row = ids + static_cast<std::ptrdiff_t>(ceil16(a.y)) * halfWidth;
    for (; rows > 0; --rows) {
        const std::int32_t start = ceil16(left.x);
        const std::int32_t end = ceil16(right.x);
        if (start < end) {
            std::memset(row + start, id, static_cast<std::size_t>(end - start));
        }
        stepEdges(b, c, left, right, false, false);
        row += halfWidth;
    }
}

// 0x004568f0: flat colour fill.
void fillFlat(const std::array<RasterVertex, 3> &vertices, std::uint16_t color,
              std::uint16_t *pixels, int pitch) {
    FixedCorner a = fixedCorner(vertices[0], 1.0);
    FixedCorner b = fixedCorner(vertices[1], 1.0);
    FixedCorner c = fixedCorner(vertices[2], 1.0);
    sortCorners(a, b, c);
    EdgeWalker left;
    EdgeWalker right;
    std::int32_t rows = 0;
    setupEdges(a, b, c, left, right, rows, false, false);
    startEdges(b, c, left, right, false, false);
    std::uint16_t *row =
        pixels + static_cast<std::ptrdiff_t>(ceil16(a.y)) * pitch;
    for (; rows > 0; --rows) {
        const std::int32_t start = ceil16(left.x);
        const std::int32_t end = ceil16(right.x);
        for (std::int32_t x = start; x < end; ++x) {
            row[x] = color;
        }
        stepEdges(b, c, left, right, false, false);
        row += pitch;
    }
}

// 0x00456e20(vertices, texture, second texture, frame, halve, colour): the
// affine texture filler. With a second texture each pixel is the average of
// both texels ((a >> 1) & 0x7bef plus (b >> 1) & 0x7bef); `halve` halves a
// single texel; a colour other than 0xffff is added with saturation.
void fillTextured(const std::array<RasterVertex, 3> &vertices,
                  const Texture &texture, const Texture *second,
                  std::uint16_t *pixels, int pitch, bool halve, int color) {
    const std::int32_t x0 = ftol(static_cast<double>(vertices[0].x) * kFixedOne);
    const std::int32_t y0 = ftol(static_cast<double>(vertices[0].y) * kFixedOne);
    const std::int32_t x1 = ftol(static_cast<double>(vertices[1].x) * kFixedOne);
    const std::int32_t y1 = ftol(static_cast<double>(vertices[1].y) * kFixedOne);
    const std::int32_t x2 = ftol(static_cast<double>(vertices[2].x) * kFixedOne);
    const std::int32_t y2 = ftol(static_cast<double>(vertices[2].y) * kFixedOne);
    // 0x00456eb3-0x00456f03: the determinant in float, with the y deltas
    // rounded to float first.
    constexpr double kInverseFixed = 1.0 / 65536.0;  // qword 0x0049f648
    const float dy12 = static_cast<float>(static_cast<double>(y1 - y2) * kInverseFixed);
    const float dy02 = static_cast<float>(static_cast<double>(y0 - y2) * kInverseFixed);
    const double determinant =
        static_cast<double>(x0 - x2) * kInverseFixed * static_cast<double>(dy12) -
        static_cast<double>(x1 - x2) * kInverseFixed * static_cast<double>(dy02);
    if (determinant == 0.0) {
        return;
    }
    const double inverse = 1.0 / determinant;
    const std::uint32_t mask = (1U << texture.log2Size) - 1U;
    const int shift = texture.log2Size;

    FixedCorner a{x0, y0, ftol(vertices[0].u), ftol(vertices[0].v), 0, 0};
    FixedCorner b{x1, y1, ftol(vertices[1].u), ftol(vertices[1].v), 0, 0};
    FixedCorner c{x2, y2, ftol(vertices[2].u), ftol(vertices[2].v), 0, 0};
    const bool pair = second != nullptr;
    if (pair) {
        a.s = ftol(vertices[0].s);
        a.t = ftol(vertices[0].t);
        b.s = ftol(vertices[1].s);
        b.t = ftol(vertices[1].t);
        c.s = ftol(vertices[2].s);
        c.t = ftol(vertices[2].t);
    }
    const auto gradient = [&](std::int32_t q0, std::int32_t q1,
                              std::int32_t q2) {
        return ftol((static_cast<double>(q0 - q2) * static_cast<double>(dy12) -
                     static_cast<double>(q1 - q2) * static_cast<double>(dy02)) *
                    inverse);
    };
    const std::int32_t dudx = gradient(a.u, b.u, c.u);
    const std::int32_t dvdx = gradient(a.v, b.v, c.v);
    std::int32_t dsdx = 0;
    std::int32_t dtdx = 0;
    if (pair) {
        dsdx = gradient(a.s, b.s, c.s);
        dtdx = gradient(a.t, b.t, c.t);
    }

    sortCorners(a, b, c);
    EdgeWalker left;
    EdgeWalker right;
    std::int32_t rows = 0;
    setupEdges(a, b, c, left, right, rows, true, pair);
    startEdges(b, c, left, right, true, pair);

    const std::uint16_t *texels = texture.pixels.data();
    const std::uint16_t *texels2 = pair ? second->pixels.data() : nullptr;
    const auto texel = [&](const std::uint16_t *source, std::int32_t u,
                           std::int32_t v) {
        const std::uint32_t index =
            ((static_cast<std::uint32_t>(v >> 16) & mask) << shift) +
            (static_cast<std::uint32_t>(u >> 16) & mask);
        return source[index];
    };
    std::uint16_t *row =
        pixels + static_cast<std::ptrdiff_t>(ceil16(a.y)) * pitch;
    for (; rows > 0; --rows) {
        const std::int32_t start = ceil16(left.x);
        const std::int32_t end = ceil16(right.x);
        const std::int32_t prestep = start * 0x10000 - left.x;
        std::int32_t u = mul16(dudx, prestep) + left.u;
        std::int32_t v = mul16(dvdx, prestep) + left.v;
        std::int32_t s = 0;
        std::int32_t t = 0;
        if (pair && !halve) {
            s = mul16(dsdx, prestep) + left.s;
            t = mul16(dtdx, prestep) + left.t;
        }
        for (std::int32_t x = start; x < end; ++x) {
            std::uint16_t value = texel(texels, u, v);
            if (halve) {
                value = static_cast<std::uint16_t>((value >> 1) & 0x7bef);
            } else if (pair) {
                const std::uint16_t other = texel(texels2, s, t);
                value = static_cast<std::uint16_t>(
                    ((other >> 1) & 0x7bef) + ((value >> 1) & 0x7bef));
                s += dsdx;
                t += dtdx;
            } else if (color != 0xffff) {
                value = saturatingAdd565(value, static_cast<std::uint16_t>(color));
            }
            row[x] = value;
            u += dudx;
            v += dvdx;
        }
        stepEdges(b, c, left, right, true, pair);
        row += pitch;
    }
}

// 0x004589f0.
void normalize(std::array<float, 3> &vector) {
    const double length = std::sqrt(
        static_cast<double>(vector[2]) * vector[2] +
        static_cast<double>(vector[1]) * vector[1] +
        static_cast<double>(vector[0]) * vector[0]);
    const float scale = static_cast<float>(1.0 / length);
    vector[0] = scale * vector[0];
    vector[1] = scale * vector[1];
    vector[2] = scale * vector[2];
}

// 0x00458970.
double dot(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    return static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] +
           static_cast<double>(a[2]) * b[2];
}

// 0x00458990: a x b.
std::array<float, 3> cross(const std::array<float, 3> &a,
                           const std::array<float, 3> &b) {
    return {
        static_cast<float>(static_cast<double>(b[2]) * a[1] -
                           static_cast<double>(a[2]) * b[1]),
        static_cast<float>(static_cast<double>(a[2]) * b[0] -
                           static_cast<double>(b[2]) * a[0]),
        static_cast<float>(static_cast<double>(a[0]) * b[1] -
                           static_cast<double>(b[0]) * a[1]),
    };
}

// 0x00459880: the view matrix for an eye looking at a target.
Matrix lookAt(const std::array<float, 3> &eye,
              const std::array<float, 3> &target) {
    std::array<float, 3> forward{target[0] - eye[0], target[1] - eye[1],
                                 target[2] - eye[2]};
    normalize(forward);
    std::array<float, 3> up{};
    if (forward[0] == 0.0f && forward[2] == 0.0f) {
        up = {-forward[1], 0.0f, 0.0f};
    } else {
        up = {target[0], target[1] + 1.0f, target[2]};
    }
    up = {up[0] - eye[0], up[1] - eye[1], up[2] - eye[2]};
    const double along = dot(up, forward);
    up[0] = static_cast<float>(up[0] - along * forward[0]);
    up[1] = static_cast<float>(up[1] - along * forward[1]);
    up[2] = static_cast<float>(up[2] - along * forward[2]);
    normalize(up);
    std::array<float, 3> right = cross(up, forward);
    normalize(right);
    Matrix matrix{};
    matrix[0] = right[0];
    matrix[4] = right[1];
    matrix[8] = right[2];
    matrix[1] = up[0];
    matrix[5] = up[1];
    matrix[9] = up[2];
    matrix[2] = forward[0];
    matrix[6] = forward[1];
    matrix[10] = forward[2];
    matrix[13] = static_cast<float>(-(static_cast<double>(up[0]) * eye[0]) -
                                    static_cast<double>(up[1]) * eye[1] -
                                    static_cast<double>(up[2]) * eye[2]);
    matrix[12] = static_cast<float>(-(static_cast<double>(right[0]) * eye[0]) -
                                    static_cast<double>(right[1]) * eye[1] -
                                    static_cast<double>(right[2]) * eye[2]);
    matrix[14] = static_cast<float>(-(static_cast<double>(forward[0]) * eye[0]) -
                                    static_cast<double>(forward[1]) * eye[1] -
                                    static_cast<double>(forward[2]) * eye[2]);
    return matrix;
}

// 0x00459ea0: a vertex into view space, its normal by the rotation only.
void transformVertex(Vertex &vertex, const Matrix &world, const Matrix &rotation) {
    const double x = vertex.position[0];
    const double y = vertex.position[1];
    const double z = vertex.position[2];
    vertex.view[0] = static_cast<float>(x * world[0] + world[4] * y + world[8] * z + world[12]);
    vertex.view[1] = static_cast<float>(world[5] * y + world[9] * z + world[1] * x + world[13]);
    vertex.view[2] = static_cast<float>(world[6] * y + world[10] * z + world[2] * x + world[14]);
    const double nx = vertex.normal[0];
    const double ny = vertex.normal[1];
    const double nz = vertex.normal[2];
    vertex.viewNormal[0] = static_cast<float>(nx * rotation[0] + rotation[4] * ny + rotation[8] * nz);
    vertex.viewNormal[1] = static_cast<float>(rotation[1] * nx + rotation[5] * ny + rotation[9] * nz);
    vertex.viewNormal[2] = static_cast<float>(rotation[2] * nx + rotation[6] * ny + rotation[10] * nz);
}

// 0x00459f50: a face normal by the rotation.
void rotateFaceNormal(Face &face, const Matrix &rotation) {
    const double x = face.normal[0];
    const double y = face.normal[1];
    const double z = face.normal[2];
    face.viewNormal[0] = static_cast<float>(x * rotation[0] + rotation[8] * z + rotation[4] * y);
    face.viewNormal[1] = static_cast<float>(rotation[9] * z + rotation[1] * x + rotation[5] * y);
    face.viewNormal[2] = static_cast<float>(rotation[10] * z + rotation[2] * x + rotation[6] * y);
}

// 0x0045a040: inside [margin, size - margin] on both axes.
bool onScreen(const std::array<float, 3> &screen, int width, int height) {
    return !(screen[0] < 0.0f || static_cast<float>(width) < screen[0] ||
             screen[1] < 0.0f || static_cast<float>(height) < screen[1]);
}

// 0x0045a0c0: quicksort of the face list by truncated depth. It swaps the
// depth values and the 0x1c indices between the faces rather than the
// pointers, and the depth moved into the upper slot is the truncated one.
void sortFaces(std::vector<Face *> &list, int low, int high) {
    for (;;) {
        const std::int32_t pivot = ftol(list[(low + high) >> 1]->depth);
        int i = low;
        int j = high;
        while (i <= j) {
            while (ftol(list[i]->depth) < pivot) {
                ++i;
            }
            while (pivot < ftol(list[j]->depth)) {
                --j;
            }
            if (i > j) {
                break;
            }
            const std::int32_t truncated = ftol(list[i]->depth);
            list[i]->depth = list[j]->depth;
            list[j]->depth = static_cast<float>(truncated);
            std::swap(list[i]->sortIndex, list[j]->sortIndex);
            ++i;
            --j;
        }
        if (low < j) {
            sortFaces(list, low, j);
        }
        if (i >= high) {
            return;
        }
        low = i;
    }
}

} // namespace

std::uint16_t rgb565(int red, int green, int blue) {
    const unsigned r = static_cast<unsigned>(red) & 0xf8U;
    const unsigned g = static_cast<unsigned>(green) & 0xfcU;
    const unsigned b = (static_cast<unsigned>(blue) & 0xffU) >> 3;
    return static_cast<std::uint16_t>(((r << 5) + g) * 8 + b);
}

std::uint16_t saturatingAdd565(std::uint16_t first, std::uint16_t second) {
    const auto spread = [](std::uint32_t value) {
        return ((((value & 0xf800U) << 3) | (value & 0x7e0U)) << 2 |
                (value & 0x1fU)) << 3 | 0x20003U;
    };
    std::uint32_t sum = spread(first) + spread(second);
    sum = sum | (0x1010100U - ((sum & 0x1010100U) >> 7));
    return static_cast<std::uint16_t>(
        (((((sum >> 3) & 0x1f0000U) | (sum & 0xfc00U)) >> 2) | (sum & 0xf8U)) >> 3);
}

std::shared_ptr<Texture> makeTexture(int width, int height,
                                     std::vector<std::uint16_t> pixels) {
    auto texture = std::make_shared<Texture>();
    texture->width = width;
    texture->height = height;
    // 0x0045abd0's switch on the width: 2 -> 1 ... 256 -> 8.
    for (int log2 = 1; log2 <= 8; ++log2) {
        if ((1 << log2) == width) {
            texture->log2Size = log2;
        }
    }
    texture->pixels = std::move(pixels);
    return texture;
}

Matrix identityMatrix() {
    Matrix matrix{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    return matrix;
}

void rotateX(Matrix &matrix, float degrees) {
    const double angle = static_cast<double>(degrees) * kDegreesToRadians;
    const float s = static_cast<float>(std::sin(angle));
    const float c = static_cast<float>(std::cos(angle));
    matrix[9] = s;
    matrix[5] = c;
    matrix[6] = -s;
    matrix[10] = c;
}

void rotateY(Matrix &matrix, float degrees) {
    const double angle = static_cast<double>(degrees) * kDegreesToRadians;
    const float s = static_cast<float>(std::sin(angle));
    const float c = static_cast<float>(std::cos(angle));
    matrix[0] = c;
    matrix[8] = -s;
    matrix[10] = c;
    matrix[2] = s;
}

void rotateZ(Matrix &matrix, float degrees) {
    const double angle = static_cast<double>(degrees) * kDegreesToRadians;
    const float s = static_cast<float>(std::sin(angle));
    const float c = static_cast<float>(std::cos(angle));
    matrix[4] = s;
    matrix[0] = c;
    matrix[1] = -s;
    matrix[5] = c;
}

Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const float *ar = &a[static_cast<std::size_t>(row * 4)];
            out[static_cast<std::size_t>(row * 4 + column)] = static_cast<float>(
                static_cast<double>(b[static_cast<std::size_t>(8 + column)]) * ar[2] +
                static_cast<double>(ar[3]) * b[static_cast<std::size_t>(12 + column)] +
                static_cast<double>(ar[0]) * b[static_cast<std::size_t>(column)] +
                static_cast<double>(ar[1]) * b[static_cast<std::size_t>(4 + column)]);
        }
    }
    return out;
}

std::array<float, 2> Renderer::project(const std::array<float, 3> &point) const {
    const std::array<float, 3> view = transformPoint(point, view_);
    const float z = view[2];
    std::int32_t bits = 0;
    std::memcpy(&bits, &z, sizeof bits);
    const std::int32_t estimateBits = 0x7f000000 - bits;
    float estimate = 0.0f;
    std::memcpy(&estimate, &estimateBits, sizeof estimate);
    const double reciprocal = (2.0 - static_cast<double>(z) * estimate) * estimate;
    return {static_cast<float>(static_cast<double>(focalX_) * view[0] * reciprocal +
                               static_cast<double>(width_) * 0.5),
            static_cast<float>(static_cast<double>(focalY_) * view[1] * reciprocal +
                               static_cast<double>(height_) * 0.5)};
}

std::array<float, 3> transformPoint(const std::array<float, 3> &point,
                                    const Matrix &m) {
    const double x = point[0];
    const double y = point[1];
    const double z = point[2];
    return {
        static_cast<float>(x * m[0] + m[4] * y + m[8] * z + m[12]),
        static_cast<float>(m[5] * y + m[9] * z + m[1] * x + m[13]),
        static_cast<float>(m[6] * y + m[10] * z + m[2] * x + m[14]),
    };
}

void updateMeshMatrix(Mesh &mesh) {
    Matrix rotationX = identityMatrix();
    Matrix rotationY = identityMatrix();
    Matrix rotationZ = identityMatrix();
    Matrix scale = identityMatrix();
    rotateX(rotationX, mesh.rotation[0]);
    rotateY(rotationY, mesh.rotation[1]);
    rotateZ(rotationZ, mesh.rotation[2]);
    scale[0] = mesh.scale[0];
    scale[5] = mesh.scale[1];
    scale[10] = mesh.scale[2];
    const Matrix xy = multiply(rotationX, rotationY);
    mesh.rotationMatrix = multiply(rotationZ, xy);
    mesh.world = multiply(scale, mesh.rotationMatrix);
    mesh.world[12] = mesh.position[0];
    mesh.world[13] = mesh.position[1];
    mesh.world[14] = mesh.position[2];
}

Renderer::Renderer(int width, int height, float fovDegrees)
    : width_(width), height_(height) {
    // 0x0045a950: fx = (w * 0.5) / (1 / tan(fov * 0.5)), and fy the same
    // after dividing the half height by the aspect h / w.
    const double tangent = std::tan(static_cast<double>(fovDegrees) * 0.5 *
                                    static_cast<double>(kDegreesToRadians));
    const double cotangent = 1.0 / tangent;
    focalX_ = static_cast<float>((static_cast<double>(width) * 0.5) / cotangent);
    focalY_ = static_cast<float>(((static_cast<double>(height) * 0.5) /
                                  (static_cast<double>(height) /
                                   static_cast<double>(width))) /
                                 cotangent);
}

Mesh &Renderer::addBox(float halfX, float halfY, float halfZ, Material *top,
                       Material *side, std::uint8_t id) {
    auto mesh = std::make_unique<Mesh>();
    // 0x0045929e-0x0045938b: the eight corners.
    const float a = halfX;
    const float b = halfY;
    const float c = halfZ;
    const std::array<std::array<float, 3>, 8> corners{{
        {-a, -b, -c}, {-a, -b, c}, {a, -b, c}, {a, -b, -c},
        {-a, b, -c}, {-a, b, c}, {a, b, c}, {a, b, -c},
    }};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        mesh->vertices[index].position = corners[index];
    }
    // The six quads, each split into faces (p0, p1, p2) and (p0, p2, p3).
    constexpr std::array<std::array<int, 4>, 6> quads{{
        {1, 0, 3, 2}, {0, 4, 7, 3}, {4, 5, 6, 7},
        {2, 6, 5, 1}, {3, 7, 6, 2}, {1, 5, 4, 0},
    }};
    const bool textured = top != nullptr && top->layers[0].texture != nullptr;
    // The texture width in 16.16 (texture width times 65536) and one 16.16
    // unit (65536.0 = 0x47800000) for the first face's third corner. The
    // corners are shared, so later quads overwrite earlier coordinates.
    const float span = textured
        ? static_cast<float>(top->layers[0].texture->width) * 65536.0f : 0.0f;
    for (std::size_t quad = 0; quad < quads.size(); ++quad) {
        const auto &q = quads[quad];
        Face &first = mesh->faces[quad * 2];
        first.vertices = {q[0], q[1], q[2]};
        first.enabled = true;
        if (textured) {
            mesh->vertices[static_cast<std::size_t>(q[0])].u = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[0])].v = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[1])].u = span;
            mesh->vertices[static_cast<std::size_t>(q[1])].v = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[2])].u = span;
            mesh->vertices[static_cast<std::size_t>(q[2])].v = 65536.0f;
        }
        Face &second = mesh->faces[quad * 2 + 1];
        second.vertices = {q[0], q[2], q[3]};
        second.enabled = true;
        if (textured) {
            mesh->vertices[static_cast<std::size_t>(q[0])].u = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[0])].v = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[2])].u = span;
            mesh->vertices[static_cast<std::size_t>(q[2])].v = span;
            mesh->vertices[static_cast<std::size_t>(q[3])].u = 0.0f;
            mesh->vertices[static_cast<std::size_t>(q[3])].v = span;
        }
    }
    for (std::size_t index = 0; index < mesh->faces.size(); ++index) {
        Face &face = mesh->faces[index];
        face.material = (index == 2 || index == 3) ? top : side;
        face.id = id;
    }
    mesh->top = top;
    mesh->visible = true;
    // 0x00458de0: face normals (p0 - p1) x (p0 - p2), summed into the
    // corners and normalized.
    for (auto &vertex : mesh->vertices) {
        vertex.normal = {0.0f, 0.0f, 0.0f};
    }
    for (auto &face : mesh->faces) {
        const auto &p0 = mesh->vertices[static_cast<std::size_t>(face.vertices[0])].position;
        const auto &p1 = mesh->vertices[static_cast<std::size_t>(face.vertices[1])].position;
        const auto &p2 = mesh->vertices[static_cast<std::size_t>(face.vertices[2])].position;
        const std::array<float, 3> e1{p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2]};
        const std::array<float, 3> e2{p0[0] - p2[0], p0[1] - p2[1], p0[2] - p2[2]};
        face.normal = cross(e1, e2);
        normalize(face.normal);
        for (int corner = 0; corner < 3; ++corner) {
            auto &normal = mesh->vertices[static_cast<std::size_t>(face.vertices[static_cast<std::size_t>(corner)])].normal;
            normal[0] = face.normal[0] + normal[0];
            normal[1] = face.normal[1] + normal[1];
            normal[2] = face.normal[2] + normal[2];
        }
    }
    for (auto &vertex : mesh->vertices) {
        normalize(vertex.normal);
    }
    mesh->scale = {1.0f, 1.0f, 1.0f};
    mesh->rotation = {0.0f, 0.0f, 90.0f};
    updateMeshMatrix(*mesh);
    meshes_.push_back(std::move(mesh));
    return *meshes_.back();
}

void Renderer::setCamera(const std::array<float, 3> &eye,
                         const std::array<float, 3> &target) {
    eye_ = eye;
    target_ = target;
    view_ = lookAt(eye, target);
}

void Renderer::render(std::uint16_t *pixels, int pitch, std::uint8_t *ids) {
    target_pixels_ = pixels;
    pitch_ = pitch;
    ids_ = ids;
    faces_.clear();
    std::vector<const Mesh *> owners;
    for (const auto &meshPointer : meshes_) {
        Mesh &mesh = *meshPointer;
        if (!mesh.visible) {
            continue;
        }
        const Matrix world = multiply(mesh.world, view_);
        const Matrix rotation = multiply(mesh.rotationMatrix, view_);
        for (auto &vertex : mesh.vertices) {
            transformVertex(vertex, world, rotation);
            // 0x00459fb0: the perspective divide uses the bit-trick
            // reciprocal 0x7f000000 - bits(z) and one Newton step.
            const float z = vertex.view[2];
            std::int32_t bits = 0;
            std::memcpy(&bits, &z, sizeof bits);
            const std::int32_t estimateBits = 0x7f000000 - bits;
            float estimate = 0.0f;
            std::memcpy(&estimate, &estimateBits, sizeof estimate);
            const double reciprocal =
                (2.0 - static_cast<double>(z) * estimate) * estimate;
            vertex.screen[0] = static_cast<float>(
                static_cast<double>(focalX_) * vertex.view[0] * reciprocal +
                static_cast<double>(width_) * 0.5);
            vertex.screen[1] = static_cast<float>(
                static_cast<double>(focalY_) * vertex.view[1] * reciprocal +
                static_cast<double>(height_) * 0.5);
            vertex.screen[2] = z;
        }
        for (auto &face : mesh.faces) {
            if (!face.enabled) {
                continue;
            }
            rotateFaceNormal(face, rotation);
            const double sum =
                static_cast<double>(mesh.vertices[static_cast<std::size_t>(face.vertices[2])].screen[2]) +
                mesh.vertices[static_cast<std::size_t>(face.vertices[1])].screen[2] +
                mesh.vertices[static_cast<std::size_t>(face.vertices[0])].screen[2];
            face.depth = static_cast<float>(sum * kOneThird);
            face.sortIndex = static_cast<std::int16_t>(faces_.size());
            faces_.push_back(&face);
            owners.push_back(&mesh);
        }
    }
    if (!faces_.empty()) {
        sortFaces(faces_, 0, static_cast<int>(faces_.size()) - 1);
    }
    for (int index = static_cast<int>(faces_.size()) - 1; index >= 0; --index) {
        const std::size_t chosen = static_cast<std::size_t>(
            faces_[static_cast<std::size_t>(index)]->sortIndex);
        const Face &face = *faces_[chosen];
        const Mesh &mesh = *owners[chosen];
        if (onScreen(mesh.vertices[static_cast<std::size_t>(face.vertices[0])].screen, width_, height_) &&
            onScreen(mesh.vertices[static_cast<std::size_t>(face.vertices[1])].screen, width_, height_) &&
            onScreen(mesh.vertices[static_cast<std::size_t>(face.vertices[2])].screen, width_, height_)) {
            drawFace(face, mesh);
        }
    }
    // Mahjong registers no flares (renderer +0x10/+0x14), so the sprite
    // pass at the end of 0x00459ad0 never draws anything here.
}

void Renderer::drawFace(const Face &face, const Mesh &mesh) {
    // 0x0045a1d0.
    std::array<RasterVertex, 3> corners{};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto &screen =
            mesh.vertices[static_cast<std::size_t>(face.vertices[index])].screen;
        corners[index].x = screen[0];
        corners[index].y = screen[1];
        corners[index].z = screen[2];
    }
    const Material &material = *face.material;
    if ((material.layers[0].flags & 0x40U) == 0) {
        // 0x0045a3d0: draw only clockwise-on-screen faces.
        const double area =
            (static_cast<double>(corners[2].x) - corners[0].x) *
                (static_cast<double>(corners[1].y) - corners[0].y) -
            (static_cast<double>(corners[1].x) - corners[0].x) *
                (static_cast<double>(corners[2].y) - corners[0].y);
        if (!(0.0 < area)) {
            return;
        }
    }
    if (ids_ != nullptr) {
        fillIds(corners, face.id, ids_, width_ >> 1);
    }
    if (target_pixels_ == nullptr) {
        return;  // An id-only pass (used to plan clicks), not the original's.
    }
    if ((material.layers[0].flags & 4U) != 0 &&
        (material.layers[1].flags & 8U) != 0) {
        const Texture &texture = *material.layers[0].texture;
        const Texture &environment = *material.layers[1].texture;
        if (texture.width != environment.width) {
            throw std::runtime_error(
                "TEXTURE & ENVMAP NEEDS TO HAVE THE SAME SIZE");
        }
        const double halfWidth = static_cast<float>(texture.width >> 1);
        const double halfHeight = static_cast<float>(texture.height >> 1);
        for (std::size_t index = 0; index < 3; ++index) {
            const Vertex &vertex =
                mesh.vertices[static_cast<std::size_t>(face.vertices[index])];
            corners[index].u = vertex.u;
            corners[index].v = vertex.v;
            corners[index].s = static_cast<float>(
                (static_cast<double>(vertex.viewNormal[0]) + 1.0) * halfWidth * kFixedOne);
            corners[index].t = static_cast<float>(
                (static_cast<double>(vertex.viewNormal[1]) + 1.0) * halfHeight * kFixedOne);
        }
        fillTextured(corners, texture, &environment, target_pixels_, pitch_,
                     false, 0xffff);
        return;
    }
    drawLayer(face, mesh, 0);
    drawLayer(face, mesh, 1);
}

void Renderer::drawLayer(const Face &face, const Mesh &mesh, int layerIndex) {
    // 0x0045a410.
    const MaterialLayer &layer =
        face.material->layers[static_cast<std::size_t>(layerIndex)];
    std::array<RasterVertex, 3> corners{};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto &screen =
            mesh.vertices[static_cast<std::size_t>(face.vertices[index])].screen;
        corners[index].x = screen[0];
        corners[index].y = screen[1];
        corners[index].z = screen[2];
    }
    const auto environmentCoordinates = [&](const Texture &texture) {
        const double halfWidth = static_cast<float>(texture.width >> 1);
        const double halfHeight = static_cast<float>(texture.height >> 1);
        for (std::size_t index = 0; index < 3; ++index) {
            const Vertex &vertex =
                mesh.vertices[static_cast<std::size_t>(face.vertices[index])];
            corners[index].u = static_cast<float>(
                (static_cast<double>(vertex.viewNormal[0]) + 1.0) * halfWidth * kFixedOne);
            corners[index].v = static_cast<float>(
                (static_cast<double>(vertex.viewNormal[1]) + 1.0) * halfHeight * kFixedOne);
        }
    };
    const auto vertexCoordinates = [&]() {
        for (std::size_t index = 0; index < 3; ++index) {
            const Vertex &vertex =
                mesh.vertices[static_cast<std::size_t>(face.vertices[index])];
            corners[index].u = vertex.u;
            corners[index].v = vertex.v;
        }
    };
    if ((layer.flags & 1U) != 0) {
        // Wireframe (0x00455db0); no Mahjong material sets it.
        return;
    }
    int color = 0xffff;
    if ((layer.flags & 0x32U) == 0) {
        if ((layer.flags & 4U) != 0) {
            vertexCoordinates();
        } else if ((layer.flags & 8U) != 0) {
            environmentCoordinates(*layer.texture);
        } else {
            return;
        }
    } else {
        // UNCERTAIN: the lit colour path (+0x0c or +0x10 above zero) is
        // not reached by Mahjong's materials, whose light terms are zero.
        color = layer.color;
        if ((layer.flags & 2U) != 0) {
            fillFlat(corners, layer.color, target_pixels_, pitch_);
            return;
        }
        if ((layer.flags & 0x10U) == 0) {
            environmentCoordinates(*layer.texture);
        } else {
            vertexCoordinates();
        }
    }
    fillTextured(corners, *layer.texture, nullptr, target_pixels_, pitch_,
                 false, color);
}

} // namespace fgp::mahjong
