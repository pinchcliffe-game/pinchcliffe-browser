#include "game/puzzle_data.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace fgp {
namespace {
using Vec = std::array<double, 3>;
double f32(double value) { return static_cast<float>(value); }
Vec swap(Vec v) { return {v[0], -v[2], v[1]}; }

PuzzleOriginalData load(const OriginalData &source) {
    const auto number = [&](std::uint32_t address) { return static_cast<double>(source.read<float>(address)); };
    const double one = number(0x0049c758);
    const auto normalise = [&](Vec v) {
        const double scale = one / std::sqrt((v[0]*v[0] + v[1]*v[1]) + v[2]*v[2]);
        return Vec{f32(scale*v[0]), f32(scale*v[1]), f32(scale*v[2])};
    };
    std::array<Vec, 72> raw{}, cells{};
    double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin;
    double ymin = xmin, ymax = xmax;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis)
            raw[i][axis] = number(0x0049f26cU + static_cast<std::uint32_t>(12*i + 4*axis));
        xmin = std::min(xmin, raw[i][0]); xmax = std::max(xmax, raw[i][0]);
        ymin = std::min(ymin, raw[i][1]); ymax = std::max(ymax, raw[i][1]);
    }
    if (!(xmax > xmin && ymax > ymin)) throw std::runtime_error("Invalid original Puzzle board");
    std::array<bool, 72> assigned{};
    for (const auto &cell : raw) {
        const double row = std::trunc((cell[1]-ymin)/(ymax-ymin)*number(0x0049f5d0));
        const double column = std::trunc((cell[0]-xmin)/(xmax-xmin)*number(0x0049f5cc));
        const double index = column - 9*row;
        if (!(index >= 0 && index < 72) || assigned[static_cast<std::size_t>(index)])
            throw std::runtime_error("Original Puzzle cells are not a complete board");
        assigned[static_cast<std::size_t>(index)] = true;
        cells[static_cast<std::size_t>(index)] = swap(cell);
    }
    // Immediate float operands in the camera initializer, not copied values.
    const Vec eye = swap({number(0x00452cec), number(0x00452cf4), number(0x00452cff)});
    const Vec target = swap({number(0x00452d0a), number(0x00452d12), number(0x00452d34)});
    const Vec forward = normalise({f32(target[0]-eye[0]), f32(target[1]-eye[1]), f32(target[2]-eye[2])});
    Vec up{f32(target[0]-eye[0]), f32(f32(target[1]+one)-eye[1]), f32(target[2]-eye[2])};
    const double dot = (up[2]*forward[2] + up[1]*forward[1]) + up[0]*forward[0];
    up = normalise({f32(up[0]-dot*forward[0]), f32(up[1]-dot*forward[1]), f32(up[2]-dot*forward[2])});
    const Vec right = normalise({f32(forward[2]*up[1]-up[2]*forward[1]),
                                f32(up[2]*forward[0]-forward[2]*up[0]),
                                f32(up[0]*forward[1]-forward[0]*up[1])});
    const auto offset = [&](Vec axis) { return f32((-(axis[0]*eye[0])-axis[1]*eye[1])-axis[2]*eye[2]); };
    const double m12 = offset(right), m13 = offset(up), m14 = offset(forward);
    const double focal = number(0x0049f25c)/(one/std::tan(source.read<double>(0x0049f5e0)));
    const auto project = [&](Vec v, double lift) {
        v[2] = f32(v[2]+number(0x0049f5dc));
        if (lift) v[1] = f32(v[1]+lift);
        const double vx = f32(((right[2]*v[2]+right[1]*v[1])+v[0]*right[0])+m12);
        const double vy = f32(((up[0]*v[0]+up[2]*v[2])+up[1]*v[1])+m13);
        const double vz = f32(((forward[0]*v[0]+forward[2]*v[2])+forward[1]*v[1])+m14);
        return std::array<float, 2>{static_cast<float>(focal*vx/vz+number(0x0049f25c)),
                                    static_cast<float>(focal*vy/vz+number(0x0049d9b8))};
    };
    PuzzleOriginalData result;
    for (std::size_t axis = 0; axis < 3; ++axis) result.eye[axis] = static_cast<float>(eye[axis]);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) result.centres[i][axis] = static_cast<float>(cells[i][axis]);
        const auto point = project(cells[i], 0);
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || std::abs(point[0]) > 100000 || std::abs(point[1]) > 100000)
            throw std::runtime_error("Invalid original Puzzle projection");
        const int x = static_cast<int>(point[0]), y = static_cast<int>(point[1]);
        result.anchors[i] = {x-32, y-32}; result.seeds[i] = {x >> 2, y >> 2};
        result.path[i] = project(cells[i], number(0x0049d9fc));
    }
    // Record locations selected by the mesh switch at 0x00453000/0x00454390.
    constexpr std::array<std::uint32_t, 7> addresses{0x004e6fa8, 0x004ecfb8, 0x004effc0, 0x004f2fc8, 0x004f5fd0, 0x004fbfe0, 0x004fefe8};
    for (std::size_t mesh = 0; mesh < addresses.size(); ++mesh) {
        const auto address = addresses[mesh];
        const int vertices = source.read<std::int16_t>(address), faces = source.read<std::int16_t>(address+2);
        if (vertices < 1 || vertices > 256 || faces < 1 || faces > 256)
            throw std::runtime_error("Invalid original Puzzle mesh counts");
        for (int i = 0; i < vertices; ++i) {
            const auto vertex = source.read<PuzzleMeshVertex>(address+8+24*static_cast<std::uint32_t>(i));
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) throw std::runtime_error("Invalid original mesh vertex");
            result.vertices[mesh].push_back(vertex);
        }
        for (int i = 0; i < faces; ++i) {
            const auto face = source.read<std::array<double,3>>(address+0x1808+24*static_cast<std::uint32_t>(i));
            for (double value : face) if (!(value >= 0 && value < vertices) || std::trunc(value) != value)
                throw std::runtime_error("Invalid original mesh face");
            result.faces[mesh].push_back({static_cast<int>(face[0]),static_cast<int>(face[1]),static_cast<int>(face[2])});
        }
    }
    return result;
}
} // namespace

const PuzzleOriginalData &puzzleOriginalData() {
    static std::optional<PuzzleOriginalData> data;
    static std::uint64_t generation{};
    const auto &source = OriginalData::instance();
    if (!source.generation()) throw std::runtime_error("Select original game files before loading Puzzle");
    if (!data || generation != source.generation()) {
        data = load(source); generation = source.generation();
    }
    return *data;
}
} // namespace fgp
