#pragma once

#include <array>
#include <vector>

namespace fgp {
struct CellPoint { int x, y; };
struct PuzzleMeshVertex { double x, y, z; };
struct PuzzleMeshFace { int a, b, c; };
struct PuzzleOriginalData {
    std::array<float, 3> eye;
    std::array<std::array<float, 3>, 72> centres;
    std::array<CellPoint, 72> anchors, seeds;
    std::array<std::array<float, 2>, 72> path;
    std::array<std::vector<PuzzleMeshVertex>, 7> vertices;
    std::array<std::vector<PuzzleMeshFace>, 7> faces;
};

// Decodes and projects the player's executable data after it is selected.
// No original vertices, faces, colours or board coordinates are compiled in.
const PuzzleOriginalData &puzzleOriginalData();
} // namespace fgp
