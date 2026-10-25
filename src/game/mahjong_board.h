#pragma once

// The Mahjong board of FGP.exe (0x00450f40-0x00452720): the 144-tile
// layout, the deal, the free/blocked and matching rules, the pair count
// and hint, removal and undo, and the per-frame board render. The board
// record is the 0x4ed8-byte allocation 0x00450f40 makes; tiles are its
// 0x84-byte records. Nothing here depends on SDL.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "game/mahjong_engine.h"

namespace fgp::mahjong {

// The 3D view: a 750x400 RGB565 surface drawn at (25, 100) on layer 100
// (0x00450abf), and the half-resolution id buffer 0x00450df0 reads.
constexpr int kViewX = 25;
constexpr int kViewY = 100;
constexpr int kViewWidth = 750;
constexpr int kViewHeight = 400;
constexpr int kIdWidth = kViewWidth / 2;    // 375 (0x177)
constexpr int kIdHeight = 200;              // 0x00450e12 compares with 200
constexpr std::size_t kIdBufferSize = 150000; // 0x00450696 malloc(150000)
constexpr int kTileCount = 144;             // 0x90

// The Microsoft C runtime rand() the original links (0x0048d323):
// state = state * 0x343fd + 0x269ec3, result (state >> 16) & 0x7fff.
class MsvcRandom {
public:
    explicit MsvcRandom(std::uint32_t seed = 1) : state_(seed) {}
    void seed(std::uint32_t seed) { state_ = seed; }
    int next() {
        state_ = state_ * 0x343fdU + 0x269ec3U;
        return static_cast<int>((state_ >> 16) & 0x7fffU);
    }

private:
    std::uint32_t state_;
};

// 0x004e6b10: three layouts of 0x141 bytes; the New handler always passes
// layout 0 (`push 0` at 0x00450a87). Each layer has eight rows of
// {enabled, first column, count, unused}.
struct LayoutRow {
    std::uint8_t enabled{};
    std::uint8_t firstColumn{};
    std::uint8_t count{};
};
struct Layout {
    int layers{};
    std::array<std::array<LayoutRow, 8>, 10> rows{};
};
const Layout &layout(int index);

// 0x004e6b08: distinct tiles per kind (kinds 0-4 are dealt four times,
// kinds 5 and 6 once), and 0x004e6a60-0x004e6af8: their bitmap names.
constexpr std::array<int, 7> kKindCounts{9, 9, 9, 4, 3, 4, 4};
std::string_view tileTextureName(int kind, int rank);

// 0x00450750: the camera angles for a button-up pointer move to (x, y).
// Pitch follows the cubic 0x00451a40 over (0,50,50,0) above the middle of
// the screen and (0,-50,-50,0) below it; yaw is linear in x.
struct CameraAngles {
    float pitch{};
    float yaw{};
};
CameraAngles pointerCamera(int x, int y);

// One 0x84-byte tile record.
struct Tile {
    int column{};                 // +0x00
    int row{};                    // +0x04
    int layer{};                  // +0x08
    std::array<float, 3> position{}; // +0x0c
    Mesh *mesh{};                 // +0x18
    std::unique_ptr<Material> top;   // +0x1c
    std::unique_ptr<Material> side;  // +0x20
    std::uint8_t kind{};          // +0x24
    std::uint8_t rank{};          // +0x25
    bool alive{};                 // +0x28
    bool selected{};              // +0x2c
    std::array<float, 10> flight{}; // +0x30-+0x58, set by 0x00451730, unused
    std::array<float, 3> spin{};  // +0x60
    float removeTime{};           // +0x6c
    bool removing{};              // +0x70
    bool blinking{};              // +0x74
    bool blinkRestoreSelected{};  // +0x78
    std::int8_t blinkCount{};     // +0x7c
    std::uint8_t blinkPhase{};    // +0x7d
    float blinkTimer{};           // +0x80
};

class Board {
public:
    using Random = std::function<int()>;
    // Returns a fresh copy of a scene bitmap (bitmap\<name>.bmp) as an
    // RGB565 texture; 0x0045abd0 loads one per tile.
    using TextureLoader =
        std::function<std::shared_ptr<Texture>(std::string_view name)>;

    // 0x00450f40: builds layout `layoutIndex` over `background` (the
    // 750x400 RGB565 copy of the backdrop under the view).
    Board(std::vector<std::uint16_t> background, int layoutIndex,
          Random random, const TextureLoader &loadTexture);

    const std::array<Tile, kTileCount> &tiles() const { return tiles_; }
    Tile &tile(int index) { return tiles_[static_cast<std::size_t>(index)]; }
    const Tile &tile(int index) const {
        return tiles_[static_cast<std::size_t>(index)];
    }

    // 0x00451fa0: true when the tile may not be taken.
    bool blocked(int index) const;
    // 0x00451f30: kinds 0-4 match on kind and rank, 5 and 6 on kind.
    static bool matches(const Tile &first, const Tile &second);
    // 0x004520d0(board, 0): the number of free matching pairs, as the
    // signed byte the original returns.
    int availablePairs() const;
    // 0x004520d0(board, 1): starts the blink on the first free pair.
    void hint();
    // 0x004526f0: every tile removed.
    bool cleared() const;
    // 0x00451ed0 / 0x00451f00.
    void select(int index);
    void deselect(int index);
    // 0x004521f0: removes a matched pair and records it for undo.
    void removePair(int first, int second);
    // 0x00452300: restores the last removed pair.
    void undo();
    // 0x00450df0: the free tile under view-relative (x, y) in `ids`, or
    // none. It clears the whole id buffer when the point is inside it.
    std::optional<int> pick(int screenX, int screenY, std::uint8_t *ids) const;
    // 0x00451aa0: draws the board into `frame` (750x400 RGB565, pitch 750)
    // and `ids`, then advances the removal and hint animations by `dt`
    // ticks of 10 ms. Pitch and yaw are the pointer-driven camera angles.
    void render(float dt, float pitch, float yaw, std::uint16_t *frame,
                std::uint8_t *ids);

    // Not in the original: a screen point whose click, with the camera the
    // pointer move to that point sets, picks tile `index` (checked against
    // a fresh id render). Used to script captures.
    std::optional<std::pair<int, int>> clickPoint(int index);

    // +0x4ec9: decremented by removals, never restored by undo.
    int tilesLeft() const { return tilesLeft_; }
    // 0x00692824: the deal's fly-in progress, 0..1 over two seconds.
    float flyIn() const { return flyIn_; }
    std::size_t undoDepth() const { return undo_.size(); }

private:
    // 0x004519e0 / 0x00452070.
    std::optional<int> find(int column, int row, int layer) const;
    bool occupied(int column, int row, int layer) const;
    // 0x00452550: shows or hides the top and the four side quads of every
    // live tile from its neighbours.
    void updateFaces();
    // The camera part of 0x00451aa0 without advancing the fly-in.
    void placeCamera(float pitch, float yaw);
    // 0x00452270: enables every side quad of a removed tile.
    void enableAllFaces(int index);

    std::array<Tile, kTileCount> tiles_;
    int created_{};                              // +0x4a40
    std::vector<std::pair<int, int>> undo_;      // +0x4a44, count +0x4c84
    int tilesLeft_{};                            // +0x4ec9
    std::vector<std::uint16_t> background_;      // +0x4ed0
    std::unique_ptr<Renderer> renderer_;         // +0x4ed4
    float flyIn_{};                              // 0x00692824
    std::optional<int> topTile_;                 // 0x00693694 (layer 4)
    std::optional<int> leftEnd_;                 // 0x00692818 (layer 5)
    std::optional<int> rightEnd_;                // 0x00692828 (layer 6)
    std::array<std::optional<int>, 2> leftEndNeighbours_; // 0x006935b0/b4
    std::array<std::optional<int>, 2> rightEndNeighbours_; // 0x006935fc/00693600
};

} // namespace fgp::mahjong
