#pragma once

// The small software 3D renderer that FGP.exe's Mahjong board is drawn with
// (FGP.exe 0x00455cc0-0x0045af40). It is a painter's-algorithm renderer: it
// transforms box meshes, sorts every front face by its mean depth with the
// quicksort at 0x0045a0c0, and fills affine, 16.16 fixed-point triangles into
// an RGB565 frame. It also fills a half-resolution byte buffer with each
// face's object id, which is how Mahjong hit-tests the pointer.
//
// The structures keep the original's field order; the comments give the
// original byte offsets. Nothing here depends on SDL.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fgp::mahjong {

// 0x00455cc0: pack 8-bit channels into RGB565 (r & 0xf8, g & 0xfc, b >> 3).
std::uint16_t rgb565(int red, int green, int blue);
// 0x00455cf0: per-channel saturating add of two RGB565 colours.
std::uint16_t saturatingAdd565(std::uint16_t first, std::uint16_t second);

// 0x0045abd0's 12-byte texture: log2 of the width (+0), width (+2),
// height (+4) and RGB565 pixels (+8).
struct Texture {
    int log2Size{};
    int width{};
    int height{};
    std::vector<std::uint16_t> pixels;
};
// Builds the texture record from RGB565 pixels, as 0x0045abd0 does after
// the bitmap has been loaded into a 16-bit surface.
std::shared_ptr<Texture> makeTexture(int width, int height,
                                     std::vector<std::uint16_t> pixels);

// 0x0045ae70 allocates a 0x38-byte material of two 0x1c-byte layers. Layer
// flags (+0, +0x1c): 1 wireframe, 2 flat colour, 4 texture by vertex uv,
// 8 environment map by vertex normal, 0x10/0x20 shaded variants, 0x40 no
// backface cull.
struct MaterialLayer {
    std::uint32_t flags{};               // +0x00
    std::uint16_t color{};               // +0x08
    float light0{};                      // +0x0c
    float light1{};                      // +0x10
    std::shared_ptr<Texture> texture;    // +0x18
};
struct Material {
    std::array<MaterialLayer, 2> layers;
};

using Matrix = std::array<float, 16>;

// 0x44-byte vertex.
struct Vertex {
    std::array<float, 3> position{};     // +0x00
    std::array<float, 3> view{};         // +0x0c
    std::array<float, 3> screen{};       // +0x18
    std::array<float, 3> normal{};       // +0x24
    std::array<float, 3> viewNormal{};   // +0x30
    float u{};                           // +0x3c
    float v{};                           // +0x40
};

// 0x40-byte face.
struct Face {
    std::array<int, 3> vertices{};       // +0x00 pointers, +0x0c indices
    bool enabled{};                      // +0x14
    float depth{};                       // +0x18
    std::int16_t sortIndex{};            // +0x1c
    std::array<float, 3> normal{};       // +0x20
    std::array<float, 3> viewNormal{};   // +0x2c
    Material *material{};                // +0x38
    std::uint8_t id{};                   // +0x3c
};

// 0xb8-byte mesh made by 0x00459230: always a box of eight vertices and
// twelve faces.
struct Mesh {
    std::array<Vertex, 8> vertices;      // +0x00
    std::array<Face, 12> faces;          // +0x04
    std::array<float, 3> rotation{};     // +0x0c degrees about x, y, z
    std::array<float, 3> position{};     // +0x18
    std::array<float, 3> scale{};        // +0x24
    Matrix world{};                      // +0x30
    Matrix rotationMatrix{};             // +0x70
    Material *top{};                     // +0xb0
    bool visible{};                      // +0xb4
};

// 0x00458840 and the three rotation builders (0x004588d0, 0x00458910,
// 0x00458940), which only overwrite the rotation's four cells.
Matrix identityMatrix();
void rotateX(Matrix &matrix, float degrees);
void rotateY(Matrix &matrix, float degrees);
void rotateZ(Matrix &matrix, float degrees);
// 0x00458870: row-vector product a * b.
Matrix multiply(const Matrix &a, const Matrix &b);
// 0x004596b0: point times matrix, with translation.
std::array<float, 3> transformPoint(const std::array<float, 3> &point,
                                    const Matrix &matrix);
// 0x00459710: world = scale * (Rz * (Rx * Ry)), translation = position.
void updateMeshMatrix(Mesh &mesh);

// The renderer record of 0x0045a950 (0x84 bytes).
class Renderer {
public:
    // 0x0045a950(width, height, fov degrees, mesh capacity, face capacity).
    Renderer(int width, int height, float fovDegrees);

    // 0x00459230: a box of half extents (x, y, z). Faces 2 and 3 (the -z
    // quad) take `top`, the rest `side`; every face carries `id`.
    Mesh &addBox(float halfX, float halfY, float halfZ, Material *top,
                 Material *side, std::uint8_t id);
    // 0x00459a60: eye and target; builds the view matrix with 0x00459880.
    void setCamera(const std::array<float, 3> &eye,
                   const std::array<float, 3> &target);
    // 0x00459ad0: draws every visible mesh into `pixels` (RGB565, `pitch`
    // pixels per row) and, when `ids` is set, the half-resolution id buffer
    // ((width / 2) bytes per row). Neither buffer is cleared first. A null
    // `pixels` fills only the ids.
    void render(std::uint16_t *pixels, int pitch, std::uint8_t *ids);

    // A world point through the current view and 0x00459fb0's projection.
    std::array<float, 2> project(const std::array<float, 3> &point) const;

    int width() const { return width_; }
    int height() const { return height_; }
    const Matrix &viewMatrix() const { return view_; }

private:
    void drawFace(const Face &face, const Mesh &mesh);
    void drawLayer(const Face &face, const Mesh &mesh, int layer);

    int width_{};
    int height_{};
    float focalX_{};                     // +0x30
    float focalY_{};                     // +0x34
    std::array<float, 3> eye_{};         // +0x18
    std::array<float, 3> target_{};      // +0x24
    Matrix view_{};                      // +0x38
    std::vector<std::unique_ptr<Mesh>> meshes_;  // +0x08, count +0x0c
    std::vector<Face *> faces_;          // +0x00, count +0x04
    std::uint16_t *target_pixels_{};
    int pitch_{};
    std::uint8_t *ids_{};
};

} // namespace fgp::mahjong
