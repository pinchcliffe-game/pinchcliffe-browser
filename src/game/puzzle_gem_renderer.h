#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fgp {

// Puzzle's gems are drawn by a small software 3D engine in FGP.exe
// (0x00457F80-0x0045ABD0). Every gem owns a 64x64 RGB565 surface which the
// engine renders from the board camera looking at the gem's cell; this is a
// transcription of the parts Puzzle uses.

enum class PuzzleMeshId : std::uint8_t {
    Round, Square, Type3, Type4, Type5, Gold, Silver,
};

// A 0x38-byte material (0x0045AE70). `flags` is +0x00; 0x20 lights the
// face colour and adds the sphere-mapped illum.bmp texel (the gems), 2 fills
// flat, 4 with secondFlags 8 averages illum.bmp with gold.bmp/silver.bmp.
struct PuzzleMaterial {
    std::uint32_t flags{0x20};
    std::uint16_t colour{};        // +0x08, RGB565
    float diffuse{};               // +0x0C
    float specular{};              // +0x10
    float exponent{};              // +0x14
    std::uint32_t secondFlags{};   // +0x1C
    int texture{-1};               // +0x34: 0 gold.bmp, 1 silver.bmp
};

// One 0xB8-byte mesh instance (0x00458BF0) as Puzzle animates it.
struct PuzzleMeshInstance {
    PuzzleMeshId mesh{PuzzleMeshId::Round};
    bool visible{};                         // +0xB4
    std::array<float, 3> rotation{};        // +0x0C..+0x14, degrees
    float scale{1.0F};                      // +0x24..+0x2C
    PuzzleMaterial material;
};

class PuzzleGemRenderer {
public:
    static constexpr int kSize = 64;

    // Textures as 24-bit BMP bytes; each must be 64x64.
    void load(std::span<const std::uint8_t> illum,
              std::span<const std::uint8_t> gold,
              std::span<const std::uint8_t> silver);
    [[nodiscard]] bool loaded() const { return !illum_.empty(); }

    // 0x00459AD0 for one gem: the board camera looks at `target` (the cell
    // centre); `out` receives 64x64 RGB565 pixels, 0 where nothing drew.
    void render(const std::array<float, 3> &target,
                std::span<const PuzzleMeshInstance> instances,
                std::array<std::uint16_t, kSize * kSize> &out) const;

private:
    struct Vertex {
        std::array<float, 3> position{};
        std::array<float, 3> normal{};
        std::array<float, 2> uv{};  // 16.16 sphere coordinates (0x00459110)
    };
    struct Face {
        std::array<int, 3> index{};
        std::array<float, 3> normal{};
    };
    struct Mesh {
        std::vector<Vertex> vertices;
        std::vector<Face> faces;
    };

    std::array<Mesh, 7> meshes_;
    std::vector<std::uint16_t> illum_;
    std::array<std::vector<std::uint16_t>, 2> textures_;
};

// 0x00452AB0 and 0x00452880: the jittered 1D value noise the selected gem's
// wobble and the joker's colour cycle read.
float puzzleWobbleNoise(float x, float bias, float jitter, float amount);
float puzzleColourNoise(float x, float bias, float jitter, float amount);

} // namespace fgp
