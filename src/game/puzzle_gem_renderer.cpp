#include "game/puzzle_gem_renderer.h"

#include <SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "game/puzzle_data.h"
#include "render/sdl_util.h"

namespace fgp {
namespace {


constexpr float kPi = 3.1415927410125732F;          // 0x0049D620
constexpr float kTwoPi = 6.2831854820251465F;       // 0x0049DB40
constexpr float kInverseTwoPi = 0.15915493667125702F;  // 0x0049DB44
constexpr float kInversePi = 0.31830987334251404F;  // 0x0049F658
constexpr float kDegrees = 0.01745329238474369F;    // 0x0049F650
constexpr float kFixed = 65536.0F;                  // 0x0049DA4C
constexpr float kThird = 0.3333333432674408F;       // 0x0049F65C
constexpr float kChannelLimit = 200.0F;             // 0x0049EB54
constexpr int kTextureShift = 6;                    // 64-texel textures

std::uint16_t pack565(int r, int g, int b) {
    // 0x00455CC0.
    return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                      ((b & 0xFF) >> 3));
}

std::uint16_t saturatingAdd(std::uint16_t a, std::uint16_t b) {
    // 0x00455CF0: per-channel saturating RGB565 add.
    const int r = std::min(31, (a >> 11) + (b >> 11));
    const int g = std::min(63, ((a >> 5) & 63) + ((b >> 5) & 63));
    const int blue = std::min(31, (a & 31) + (b & 31));
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | blue);
}

std::vector<std::uint16_t> loadTexture(std::span<const std::uint8_t> bytes) {
    // 0x0045ABD0 loads a bitmap into a 16-bit RGB565 texel array.
    SurfacePtr surface = loadBmp(bytes);
    SurfacePtr rgb(SDL_ConvertSurfaceFormat(surface.get(),
                                            SDL_PIXELFORMAT_RGB888, 0));
    if (!rgb || rgb->w != PuzzleGemRenderer::kSize ||
        rgb->h != PuzzleGemRenderer::kSize) {
        throw std::runtime_error("Puzzle gem textures must be 64x64");
    }
    std::vector<std::uint16_t> texels(
        static_cast<std::size_t>(rgb->w * rgb->h));
    for (int y = 0; y < rgb->h; ++y) {
        const auto *row = reinterpret_cast<const std::uint32_t *>(
            static_cast<const std::uint8_t *>(rgb->pixels) + y * rgb->pitch);
        for (int x = 0; x < rgb->w; ++x) {
            const std::uint32_t pixel = row[x];
            texels[static_cast<std::size_t>(y * rgb->w + x)] = pack565(
                static_cast<int>((pixel >> 16) & 0xFF),
                static_cast<int>((pixel >> 8) & 0xFF),
                static_cast<int>(pixel & 0xFF));
        }
    }
    return texels;
}

using Vec3 = std::array<float, 3>;
using Matrix = std::array<float, 16>;

Vec3 cross(const Vec3 &a, const Vec3 &b) {
    // 0x00458990.
    return {b[2] * a[1] - a[2] * b[1], a[2] * b[0] - b[2] * a[0],
            a[0] * b[1] - b[0] * a[1]};
}

Vec3 normalised(const Vec3 &v) {
    // 0x004589F0.
    const float scale =
        1.0F / std::sqrt(v[2] * v[2] + v[1] * v[1] + v[0] * v[0]);
    return {scale * v[0], scale * v[1], scale * v[2]};
}

Matrix identity() {
    Matrix m{};
    m[0] = m[5] = m[10] = m[15] = 1.0F;
    return m;
}

Matrix multiply(const Matrix &a, const Matrix &b) {
    // 0x00458870: row-vector convention, a applies first.
    Matrix c{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            c[static_cast<std::size_t>(i * 4 + j)] =
                a[static_cast<std::size_t>(i * 4)] *
                    b[static_cast<std::size_t>(j)] +
                a[static_cast<std::size_t>(i * 4 + 1)] *
                    b[static_cast<std::size_t>(4 + j)] +
                a[static_cast<std::size_t>(i * 4 + 2)] *
                    b[static_cast<std::size_t>(8 + j)] +
                a[static_cast<std::size_t>(i * 4 + 3)] *
                    b[static_cast<std::size_t>(12 + j)];
        }
    }
    return c;
}

Matrix lookAt(const Vec3 &eye, const Vec3 &target) {
    // 0x00459880.
    const Vec3 forward = normalised(
        {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]});
    Vec3 up{target[0] - eye[0], target[1] + 1.0F - eye[1],
            target[2] - eye[2]};
    if (forward[0] == 0.0F && forward[2] == 0.0F) {
        up = {-forward[1] - eye[0], 0.0F - eye[1], 0.0F - eye[2]};
    }
    const float along =
        up[0] * forward[0] + up[1] * forward[1] + up[2] * forward[2];
    up = normalised({up[0] - along * forward[0], up[1] - along * forward[1],
                     up[2] - along * forward[2]});
    const Vec3 right = normalised(cross(up, forward));
    Matrix m{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto a = static_cast<std::size_t>(axis);
        m[a * 4] = right[a];
        m[a * 4 + 1] = up[a];
        m[a * 4 + 2] = forward[a];
    }
    m[12] = -(right[0] * eye[0]) - right[1] * eye[1] - right[2] * eye[2];
    m[13] = -(up[0] * eye[0]) - up[1] * eye[1] - up[2] * eye[2];
    m[14] = -(forward[0] * eye[0]) - forward[1] * eye[1] -
            forward[2] * eye[2];
    return m;
}

float reciprocal(float z) {
    // 0x00459FB0: a bit-trick reciprocal estimate and one Newton step.
    const float estimate = std::bit_cast<float>(
        0x7F000000U - std::bit_cast<std::uint32_t>(z));
    return (2.0F - z * estimate) * estimate;
}

std::uint32_t hashNoise(std::uint32_t n) {
    // 0x00452840 (the integer part; callers scale by 2^-31).
    n ^= n << 13;
    return ((n * n * 0x3D73U + 0xC0AE5U) * n + 0xD208DD0DU) & 0x7FFFFFFFU;
}

float noiseValue(std::uint32_t n) {
    return static_cast<float>(hashNoise(n)) * 4.656612873077393e-10F;
}

template <bool Smooth>
float jitteredNoise(float x, float bias, float jitter, float amount) {
    // 0x00452880 / 0x00452AB0.
    const int base = static_cast<int>(x >= 0.0F ? x : x - 1.0F);
    std::uint32_t low = static_cast<std::uint32_t>(base);
    std::uint32_t high = low + 1U;
    float t = x - static_cast<float>(base);
    if (jitter != 0.0F) {
        float start = (noiseValue(low * 17U) - 0.5F) * jitter;
        float end = (noiseValue(high * 17U) - 0.5F) * jitter + 1.0F;
        if (start <= t) {
            if (end < t) {
                t -= 1.0F;
                low = high;
                high = static_cast<std::uint32_t>(base) + 2U;
                start = end - 1.0F;
                end = (noiseValue(high * 17U) - 0.5F) * jitter + 1.0F;
            }
        } else {
            t += 1.0F;
            end = start + 1.0F;
            start = (noiseValue((static_cast<std::uint32_t>(base) - 1U) *
                                17U) - 0.5F) * jitter;
            low = static_cast<std::uint32_t>(base) - 1U;
            high = static_cast<std::uint32_t>(base);
        }
        t = (t - start) / (end - start);
    }
    if ((low & 1U) != 0U) {
        t = 1.0F - t;
        ++low;
        --high;
    }
    if (bias >= 0.0F) {
        if (bias > 0.0F) t = t / ((1.0F - bias) + t * bias);
    } else {
        t = ((bias + 1.0F) * t) / (t * bias + 1.0F);
    }
    if constexpr (Smooth) t = ((3.0F - t) - t) * t * t;
    if (amount == 0.0F) return t;
    const float from = noiseValue(low * 0x61U) * amount * 0.5F;
    return ((1.0F - from) - noiseValue(high * 0x61U) * amount * 0.5F) * t +
           from;
}

// One projected corner as 0x00456E20 receives it: screen position, then up
// to two texture coordinate pairs in 16.16 texels.
struct Corner {
    float x{};
    float y{};
    std::array<float, 2> first{};
    std::array<float, 2> second{};
};

enum class SpanMode { Copy, Add, Average };

void rasterise(std::array<Corner, 3> corners,
               std::span<const std::uint16_t> first,
               std::span<const std::uint16_t> second, SpanMode mode,
               std::uint16_t colour,
               std::array<std::uint16_t, PuzzleGemRenderer::kSize *
                                             PuzzleGemRenderer::kSize> &out) {
    // 0x00456E20: an affine, top-left-filled span rasteriser over 64-texel
    // wrapping textures. Rows and columns start at ceil() of the edge.
    const auto &a = corners[0];
    const auto &b = corners[1];
    const auto &c = corners[2];
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0.0F) return;
    const auto gradient = [&](float fa, float fb, float fc) {
        const float dx = ((fb - fa) * (c.y - a.y) - (fc - fa) * (b.y - a.y)) /
                         area;
        const float dy = ((fc - fa) * (b.x - a.x) - (fb - fa) * (c.x - a.x)) /
                         area;
        return std::pair{dx, dy};
    };
    const auto [du1x, du1y] = gradient(a.first[0], b.first[0], c.first[0]);
    const auto [dv1x, dv1y] = gradient(a.first[1], b.first[1], c.first[1]);
    const auto [du2x, du2y] = gradient(a.second[0], b.second[0], c.second[0]);
    const auto [dv2x, dv2y] = gradient(a.second[1], b.second[1], c.second[1]);
    std::sort(corners.begin(), corners.end(),
              [](const Corner &l, const Corner &r) { return l.y < r.y; });
    const Corner &top = corners[0];
    const Corner &middle = corners[1];
    const Corner &bottom = corners[2];
    const auto edgeX = [](const Corner &from, const Corner &to, float y) {
        if (to.y == from.y) return from.x;
        return from.x + (to.x - from.x) * (y - from.y) / (to.y - from.y);
    };
    constexpr int mask = (1 << kTextureShift) - 1;
    const auto sample = [&](std::span<const std::uint16_t> texture, float u,
                            float v) {
        const int column = (static_cast<int>(std::floor(u / kFixed))) & mask;
        const int row = (static_cast<int>(std::floor(v / kFixed))) & mask;
        return texture[static_cast<std::size_t>((row << kTextureShift) +
                                                column)];
    };
    const int firstRow = static_cast<int>(std::ceil(top.y));
    const int lastRow = static_cast<int>(std::ceil(bottom.y));
    for (int y = std::max(0, firstRow);
         y < std::min(lastRow, PuzzleGemRenderer::kSize); ++y) {
        const float row = static_cast<float>(y);
        const float longX = edgeX(top, bottom, row);
        const float shortX = row < middle.y ? edgeX(top, middle, row)
                                            : edgeX(middle, bottom, row);
        const float left = std::min(longX, shortX);
        const float right = std::max(longX, shortX);
        const int start = std::max(0, static_cast<int>(std::ceil(left)));
        const int end = std::min(PuzzleGemRenderer::kSize,
                                 static_cast<int>(std::ceil(right)));
        for (int x = start; x < end; ++x) {
            const float dx = static_cast<float>(x) - a.x;
            const float dy = row - a.y;
            const float u1 = a.first[0] + dx * du1x + dy * du1y;
            const float v1 = a.first[1] + dx * dv1x + dy * dv1y;
            const std::uint16_t texel = sample(first, u1, v1);
            std::uint16_t pixel = texel;
            if (mode == SpanMode::Add) {
                pixel = saturatingAdd(texel, colour);
            } else if (mode == SpanMode::Average) {
                const float u2 = a.second[0] + dx * du2x + dy * du2y;
                const float v2 = a.second[1] + dx * dv2x + dy * dv2y;
                pixel = static_cast<std::uint16_t>(
                    ((sample(second, u2, v2) >> 1) & 0x7BEF) +
                    ((texel >> 1) & 0x7BEF));
            }
            out[static_cast<std::size_t>(y * PuzzleGemRenderer::kSize + x)] =
                pixel;
        }
    }
}

void fillFlat(const std::array<Corner, 3> &corners, std::uint16_t colour,
              std::array<std::uint16_t, PuzzleGemRenderer::kSize *
                                            PuzzleGemRenderer::kSize> &out) {
    // 0x004568F0: the same edges filled with one colour.
    std::array<Corner, 3> sorted = corners;
    std::sort(sorted.begin(), sorted.end(),
              [](const Corner &l, const Corner &r) { return l.y < r.y; });
    const auto edgeX = [](const Corner &from, const Corner &to, float y) {
        if (to.y == from.y) return from.x;
        return from.x + (to.x - from.x) * (y - from.y) / (to.y - from.y);
    };
    const int firstRow = static_cast<int>(std::ceil(sorted[0].y));
    const int lastRow = static_cast<int>(std::ceil(sorted[2].y));
    for (int y = std::max(0, firstRow);
         y < std::min(lastRow, PuzzleGemRenderer::kSize); ++y) {
        const float row = static_cast<float>(y);
        const float longX = edgeX(sorted[0], sorted[2], row);
        const float shortX = row < sorted[1].y
                                 ? edgeX(sorted[0], sorted[1], row)
                                 : edgeX(sorted[1], sorted[2], row);
        const int start = std::max(
            0, static_cast<int>(std::ceil(std::min(longX, shortX))));
        const int end = std::min(PuzzleGemRenderer::kSize,
            static_cast<int>(std::ceil(std::max(longX, shortX))));
        for (int x = start; x < end; ++x) {
            out[static_cast<std::size_t>(y * PuzzleGemRenderer::kSize + x)] =
                colour;
        }
    }
}

} // namespace

float puzzleWobbleNoise(float x, float bias, float jitter, float amount) {
    return jitteredNoise<true>(x, bias, jitter, amount);
}

float puzzleColourNoise(float x, float bias, float jitter, float amount) {
    return jitteredNoise<false>(x, bias, jitter, amount);
}

void PuzzleGemRenderer::load(std::span<const std::uint8_t> illum,
                             std::span<const std::uint8_t> gold,
                             std::span<const std::uint8_t> silver) {
    illum_ = loadTexture(illum);
    textures_[0] = loadTexture(gold);
    textures_[1] = loadTexture(silver);
    // 0x00458BF0 with 0x00458DE0 and 0x00458FD0 for every gem mesh. Each
    // material references illum.bmp at +0x18, so 0x00459110 always assigns
    // sphere coordinates (on the swapped, not yet centred, position).
    for (std::size_t id = 0; id < meshes_.size(); ++id) {
        Mesh &mesh = meshes_[id];
        mesh.vertices.clear();
        mesh.faces.clear();
        for (const auto &source : puzzleOriginalData().vertices[id]) {
            Vertex vertex;
            const float x = static_cast<float>(source.x);
            const float y = static_cast<float>(source.y);
            const float z = static_cast<float>(source.z);
            vertex.position = {x, -z, y};  // 0x0045AF20
            const auto &p = vertex.position;
            const float inverse =
                1.0F / std::sqrt(p[2] * p[2] + p[1] * p[1] + p[0] * p[0]);
            const float nx = inverse * p[0];
            const float ny = inverse * p[1];
            const float nz = inverse * p[2];
            const float latitude = std::asin(ny) * kInversePi + 0.5F;
            float longitude = 0.0F;
            if (std::sqrt(nz * nz + nx * nx) != 0.0F) {
                if (nz == 0.0F) {
                    longitude = nx > 0.0F ? 0.0F : kPi;
                } else {
                    longitude = std::acos(nx / std::sqrt(nz * nz + nx * nx));
                    if (nz < 0.0F) longitude = kTwoPi - longitude;
                }
                longitude *= kInverseTwoPi;
            }
            vertex.uv = {longitude * static_cast<float>(kSize) * kFixed,
                         latitude * static_cast<float>(kSize) * kFixed};
            mesh.vertices.push_back(vertex);
        }
        for (const auto &source : puzzleOriginalData().faces[id]) {
            mesh.faces.push_back({{source.a, source.b, source.c}, {}});
        }
        // 0x00458DE0: face normals and the normalised sum per vertex.
        for (auto &face : mesh.faces) {
            const auto &p0 = mesh.vertices[static_cast<std::size_t>(
                face.index[0])].position;
            const auto &p1 = mesh.vertices[static_cast<std::size_t>(
                face.index[1])].position;
            const auto &p2 = mesh.vertices[static_cast<std::size_t>(
                face.index[2])].position;
            face.normal = normalised(cross(
                {p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2]},
                {p0[0] - p2[0], p0[1] - p2[1], p0[2] - p2[2]}));
            for (const int corner : face.index) {
                auto &normal =
                    mesh.vertices[static_cast<std::size_t>(corner)].normal;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    normal[axis] += face.normal[axis];
                }
            }
        }
        for (auto &vertex : mesh.vertices) {
            vertex.normal = normalised(vertex.normal);
        }
        // 0x00458FD0: centre the bounding box on the origin.
        Vec3 low{1e30F, 1e30F, 1e30F};
        Vec3 high{-1e30F, -1e30F, -1e30F};
        for (const auto &vertex : mesh.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], vertex.position[axis]);
                high[axis] = std::max(high[axis], vertex.position[axis]);
            }
        }
        for (auto &vertex : mesh.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                vertex.position[axis] -=
                    high[axis] - (high[axis] - low[axis]) * 0.5F;
            }
        }
    }
}

void PuzzleGemRenderer::render(
    const std::array<float, 3> &target,
    std::span<const PuzzleMeshInstance> instances,
    std::array<std::uint16_t, kSize * kSize> &out) const {
    out.fill(0);
    if (!loaded()) return;
    // 0x0045A950 / 0x0045AA50: a 64x64 view with the board's 800x600 focal
    // length (half-angle 69 degrees) and its eye, aimed at the cell.
    const float focal = 400.0F * std::tan(138.0F * 0.5F * kDegrees);
    const Matrix view = lookAt(puzzleOriginalData().eye, target);
    struct Projected {
        float x{};
        float y{};
        std::array<float, 3> normal{};
        std::array<float, 2> uv{};
    };
    struct DrawFace {
        const PuzzleMeshInstance *instance{};
        std::array<Projected, 3> corners;
        float faceNormalZ{};
        float depth{};
    };
    std::vector<DrawFace> faces;
    for (const auto &instance : instances) {
        if (!instance.visible) continue;
        const Mesh &mesh = meshes_[static_cast<std::size_t>(instance.mesh)];
        // 0x00459710: scale, then Rz . (Rx . Ry), then translation.
        const auto rotation = [&](int axis) {
            const float angle =
                instance.rotation[static_cast<std::size_t>(axis)] * kDegrees;
            const float s = std::sin(angle);
            const float co = std::cos(angle);
            Matrix m = identity();
            if (axis == 0) {
                m[5] = co; m[6] = -s; m[9] = s; m[10] = co;
            } else if (axis == 1) {
                m[0] = co; m[8] = -s; m[10] = co; m[2] = s;
            } else {
                m[4] = s; m[0] = co; m[1] = -s; m[5] = co;
            }
            return m;
        };
        const Matrix turn =
            multiply(rotation(2), multiply(rotation(0), rotation(1)));
        Matrix scale = identity();
        scale[0] = scale[5] = scale[10] = instance.scale;
        Matrix world = multiply(scale, turn);
        world[12] = target[0];
        world[13] = target[1];
        world[14] = target[2];
        const Matrix full = multiply(world, view);
        const Matrix normals = multiply(turn, view);
        std::vector<Projected> projected;
        std::vector<float> depths;
        projected.reserve(mesh.vertices.size());
        for (const auto &vertex : mesh.vertices) {
            // 0x00459EA0 and 0x00459FB0.
            const auto &p = vertex.position;
            const float vx = p[0] * full[0] + full[4] * p[1] +
                             full[8] * p[2] + full[12];
            const float vy = full[5] * p[1] + full[9] * p[2] +
                             full[1] * p[0] + full[13];
            const float vz = full[6] * p[1] + full[10] * p[2] +
                             full[2] * p[0] + full[14];
            const auto &n = vertex.normal;
            const float inverse = reciprocal(vz);
            Projected corner;
            corner.x = static_cast<float>(kSize) * 0.5F + focal * vx * inverse;
            corner.y = static_cast<float>(kSize) * 0.5F + focal * vy * inverse;
            corner.normal = {
                n[0] * normals[0] + normals[4] * n[1] + normals[8] * n[2],
                normals[1] * n[0] + normals[5] * n[1] + normals[9] * n[2],
                normals[2] * n[0] + normals[6] * n[1] + normals[10] * n[2]};
            corner.uv = vertex.uv;
            projected.push_back(corner);
            depths.push_back(vz);
        }
        for (const auto &face : mesh.faces) {
            // 0x00459F50 and the depth average at 0x00459BC0.
            DrawFace draw;
            draw.instance = &instance;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                draw.corners[corner] = projected[static_cast<std::size_t>(
                    face.index[corner])];
            }
            const auto &n = face.normal;
            draw.faceNormalZ =
                normals[10] * n[2] + normals[2] * n[0] + normals[6] * n[1];
            draw.depth = (depths[static_cast<std::size_t>(face.index[2])] +
                          depths[static_cast<std::size_t>(face.index[1])] +
                          depths[static_cast<std::size_t>(face.index[0])]) *
                         kThird;
            faces.push_back(draw);
        }
    }
    // 0x0045A0C0 sorts by truncated depth; faces draw far to near.
    std::stable_sort(faces.begin(), faces.end(),
                     [](const DrawFace &l, const DrawFace &r) {
                         return static_cast<int>(l.depth) >
                                static_cast<int>(r.depth);
                     });
    const auto inside = [](const Projected &p) {
        // 0x0045A040 with no margin.
        return !(p.x < 0.0F) && !(static_cast<float>(kSize) < p.x) &&
               !(p.y < 0.0F) && !(static_cast<float>(kSize) < p.y);
    };
    for (const auto &face : faces) {
        const auto &c = face.corners;
        if (!inside(c[0]) || !inside(c[1]) || !inside(c[2])) continue;
        const PuzzleMaterial &material = face.instance->material;
        // 0x0045A3D0 back-face test unless flag 0x40.
        if ((material.flags & 0x40U) == 0U &&
            !(0.0F < (c[2].x - c[0].x) * (c[1].y - c[0].y) -
                         (c[1].x - c[0].x) * (c[2].y - c[0].y))) {
            continue;
        }
        const float halfSize = static_cast<float>(kSize >> 1);
        const auto environment = [&](const Projected &p) {
            return std::array<float, 2>{
                (p.normal[0] + 1.0F) * halfSize * kFixed,
                (p.normal[1] + 1.0F) * halfSize * kFixed};
        };
        std::array<Corner, 3> corners;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            corners[corner].x = c[corner].x;
            corners[corner].y = c[corner].y;
        }
        if ((material.flags & 4U) != 0U && (material.secondFlags & 8U) != 0U &&
            material.texture >= 0) {
            // 0x0045A1D0: illum.bmp on sphere coordinates averaged with
            // gold/silver on environment coordinates.
            for (std::size_t corner = 0; corner < 3; ++corner) {
                corners[corner].first = c[corner].uv;
                corners[corner].second = environment(c[corner]);
            }
            rasterise(corners, illum_,
                      textures_[static_cast<std::size_t>(material.texture)],
                      SpanMode::Average, 0, out);
            continue;
        }
        // 0x0045A410, pass 0 (pass 1 has no flags for Puzzle's gems).
        const std::uint32_t flags = material.flags;
        if ((flags & 1U) != 0U) continue;  // wireframe, unused here
        if ((flags & 0x32U) == 0U) {
            if ((flags & 4U) != 0U) {
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    corners[corner].first = c[corner].uv;
                }
            } else if ((flags & 8U) != 0U) {
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    corners[corner].first = environment(c[corner]);
                }
            } else {
                continue;
            }
            rasterise(corners, illum_, {}, SpanMode::Copy, 0, out);
            continue;
        }
        std::uint16_t colour = material.colour;
        if (material.diffuse > 0.0F || material.specular > 0.0F) {
            const float light = face.faceNormalZ * -0.5F + 0.5F;
            float r = static_cast<float>((material.colour >> 8) & 0xF8);
            float g = static_cast<float>((material.colour >> 3) & 0xFC);
            float b = static_cast<float>((material.colour & 0x1F) << 3);
            if (material.diffuse > 0.0F) {
                const float scale = light * material.diffuse;
                r *= scale;
                g *= scale;
                b *= scale;
            }
            if (material.specular > 0.0F) {
                const float shine = std::pow(light, material.exponent) *
                                    material.specular * 255.0F;
                r += shine;
                g += shine;
                b += shine;
            }
            const auto clamp = [](float value) {
                return static_cast<int>(
                    value <= 0.0F ? 0.0F
                                  : value >= kChannelLimit ? kChannelLimit
                                                           : value);
            };
            colour = pack565(clamp(r), clamp(g), clamp(b));
        }
        if ((flags & 2U) != 0U) {
            fillFlat(corners, colour, out);
            continue;
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            corners[corner].first = (flags & 0x10U) == 0U
                                        ? environment(c[corner])
                                        : c[corner].uv;
        }
        rasterise(corners, illum_, {}, SpanMode::Add, colour, out);
    }
}

} // namespace fgp
