#include "game/mahjong_board.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>
#include <stdexcept>
#include <string>

namespace fgp::mahjong {
namespace {

constexpr float kRemoveSpeed = 0.02f;        // 0x0069281c = 0x3ca3d70a
constexpr int kRemoveDuration = 50;          // 0x00692820 = 0x32
constexpr float kBlinkInterval = 25.0f;      // 0x0049f260
constexpr float kBlinkStart = 50.0f;         // 0x42480000 at 0x004521aa
constexpr float kFlyInSpeed = 0.005f;        // 0x0049f264
constexpr float kFlyInDistance = 9625.0f;    // 0x0049f268
constexpr float kFlyInStart = 10000.0f;      // 0x0049c8f8
constexpr float kViewDistance = -375.0f;     // 0xc3bb8000 at 0x00451b61
constexpr float kTargetX = 10.0f;            // 0x0049c784 / 0x41200000

// 0x004e6b10, layout 0 (the only one the New handler builds): a 144-tile
// turtle. Layers 5, 6 and 7 hold the three end tiles, which 0x00451260
// lays at depth 0 half a row down; layer 4 is the cap.
const auto kLayouts = originalArray<Layout, 1>([](const OriginalData &source, std::size_t) {
    Layout result;
    result.layers = source.read<std::uint8_t>(0x004e6b10);
    if (result.layers < 1 || result.layers > 10) throw std::runtime_error("Invalid original Mahjong layer count");
    for (int layer = 0; layer < result.layers; ++layer)
        for (std::size_t row = 0; row < 8; ++row)
            result.rows[static_cast<std::size_t>(layer)][row] = source.read<LayoutRow>(
                0x004e6b11U + static_cast<std::uint32_t>(layer * 32 + row * 4));
    return result;
});

// 0x004e6a60 (A), 0x004e6a84 (B), 0x004e6aa8 (C), 0x004e6acc (E),
// 0x004e6adc (D), 0x004e6ae8 (seasons) and 0x004e6af8 (flowers).
constexpr std::array<std::array<std::string_view, 9>, 7> kTextureNames{{
    {"A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9"},
    {"B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9"},
    {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9"},
    {"E1", "E2", "E3", "E4"},
    {"D1", "D2", "D3"},
    {"SUM", "AUT", "WIN", "SPR"},
    {"MUM", "PLUM", "BAM", "ORC"},
}};

// One layout slot of 0x00451260's 144-entry table at 0x00692830: position,
// then column, row and layer.
struct Slot {
    std::array<float, 3> position{};
    int column{};
    int row{};
    int layer{};
};

std::vector<Slot> layoutSlots(const Layout &table) {
    std::vector<Slot> slots;
    for (int layer = 0; layer < table.layers; ++layer) {
        for (int row = 0; row < 8; ++row) {
            const LayoutRow &entry = table.rows[static_cast<std::size_t>(layer)]
                                               [static_cast<std::size_t>(row)];
            if (entry.enabled == 0) {
                continue;
            }
            const float y = (static_cast<float>(row) - 4.0f) * 32.0f;
            const float z = static_cast<float>(-layer) * 32.0f;
            for (int index = 0; index < entry.count; ++index) {
                const int column = entry.firstColumn + index;
                Slot slot;
                slot.position = {(static_cast<float>(column) - 8.0f) * 32.0f, y, z};
                slot.column = column;
                slot.row = row;
                slot.layer = layer;
                if (layer == 4) {
                    slot.position[0] += 16.0f;
                    slot.position[1] += 16.0f;
                    slot.position[2] -= 8.0f;
                } else if (layer > 4) {
                    slot.position[2] = 0.0f;
                    slot.position[1] += 16.0f;
                }
                slots.push_back(slot);
            }
        }
    }
    return slots;
}

// 0x00450e80: smoothstep of t between a and b.
float smoothStep(float a, float b, float t) {
    if (t < a) {
        return 0.0f;
    }
    if (b <= t) {
        return 1.0f;
    }
    const double f = (static_cast<double>(t) - a) / (static_cast<double>(b) - a);
    return static_cast<float>((3.0 - (f + f)) * f * f);
}

} // namespace

CameraAngles pointerCamera(int x, int y) {
    const float rawPitch = static_cast<float>(
        (static_cast<double>(-y) * 40.5 * 0.0033333334140479565 + 40.5) *
        0.02500000037252903);
    const float yaw = static_cast<float>(
        static_cast<double>(x) * 20.25 * 0.0024999999441206455 - 20.25);
    // 0x00451a40: de Casteljau over (0, h, h, 0).
    const auto bezier = [](float t, float h) {
        const double a = static_cast<double>(h) * t;
        const float b = static_cast<float>((static_cast<double>(h) - h) * t + h);
        const double ab = (static_cast<double>(b) - a) * t + a;
        const float c = static_cast<float>((0.0 - static_cast<double>(h)) * t + h);
        const double bc = (static_cast<double>(c) - b) * t + b;
        return static_cast<float>((bc - ab) * t + ab);
    };
    float pitch = rawPitch;
    if (pitch < -1.0f) {
        pitch = bezier(0.0f, -50.0f);
    } else if (pitch > 1.0f) {
        pitch = bezier(1.0f, 50.0f);
    } else if (pitch > 0.0f) {
        pitch = bezier(pitch, 50.0f);
    } else {
        pitch = bezier(static_cast<float>(static_cast<double>(pitch) + 1.0),
                       -50.0f);
    }
    return {pitch, yaw};
}

const Layout &layout(int index) {
    if (index != 0) {
        throw std::invalid_argument("Only Mahjong layout 0 is dealt");
    }
    return kLayouts[0];
}

std::string_view tileTextureName(int kind, int rank) {
    return kTextureNames.at(static_cast<std::size_t>(kind))
        .at(static_cast<std::size_t>(rank));
}

Board::Board(std::vector<std::uint16_t> background, int layoutIndex,
             Random random, const TextureLoader &loadTexture)
    : background_(std::move(background)) {
    if (background_.size() !=
        static_cast<std::size_t>(kViewWidth) * kViewHeight) {
        throw std::invalid_argument("Mahjong background must be 750x400");
    }
    // 0x00450ffe: Renderer(750, 400, 100 degrees, 200 meshes, 5000 faces).
    renderer_ = std::make_unique<Renderer>(kViewWidth, kViewHeight, 100.0f);
    const std::vector<Slot> slots = layoutSlots(layout(layoutIndex));
    if (slots.size() != static_cast<std::size_t>(kTileCount)) {
        throw std::runtime_error("Mahjong layout does not hold 144 tiles");
    }
    // 0x004513b0: shuffle the slot order with rand() % 144 swaps.
    std::array<std::uint8_t, kTileCount> order{};
    for (int index = 0; index < kTileCount; ++index) {
        order[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(index);
    }
    for (int index = 0; index < kTileCount; ++index) {
        const int other = random() % kTileCount;
        std::swap(order[static_cast<std::size_t>(other)],
                  order[static_cast<std::size_t>(index)]);
    }

    // 0x00451400: one tile.
    const auto createTile = [&](int kind, int rank, int slotIndex) {
        const int index = created_++;
        Tile &tile = tiles_[static_cast<std::size_t>(index)];
        tile = Tile{};
        tile.alive = true;
        // 0x00452420: texture by uv, plus the "illum" environment map.
        tile.top = std::make_unique<Material>();
        tile.top->layers[0].flags = 4;
        tile.top->layers[0].texture = loadTexture(tileTextureName(kind, rank));
        tile.top->layers[1].flags = 8;
        tile.top->layers[1].texture = loadTexture("illum");
        // 0x00451523-0x0045156e: flat sides of a random grey; the first
        // rand() result is the blue argument, the last the red one.
        tile.side = std::make_unique<Material>();
        tile.side->layers[0].flags = 2;
        const int blue = random() % 0x30 + 0x68;
        const int green = random() % 0x30 + 0x68;
        const int red = random() % 0x30 + 0x68;
        tile.side->layers[0].color = rgb565(red, green, blue);
        // 0x004518f0: place it on its shuffled slot.
        const Slot &slot = slots[order[static_cast<std::size_t>(slotIndex)]];
        tile.column = slot.column;
        tile.row = slot.row;
        tile.layer = slot.layer;
        tile.position = {slot.position[0] + 16.0f, slot.position[1] + 16.0f,
                         slot.position[2] + 16.0f};
        tile.mesh = &renderer_->addBox(16.0f, 16.0f, 16.0f, tile.top.get(),
                                       tile.side.get(),
                                       static_cast<std::uint8_t>(created_));
        tile.mesh->position = tile.position;
        updateMeshMatrix(*tile.mesh);
        // 0x00451584-0x004516bf: lower layers get darker faces. Layers
        // above 4 (the three end tiles) count as 100, like layer 0.
        const int shade = tile.layer > 4 ? 100 : (5 - tile.layer) * 20;
        const float factor = static_cast<float>(
            (1.0 - static_cast<double>(shade) * 0.009999999776482582) +
            0.6000000238418579);
        for (auto &pixel : tile.top->layers[0].texture->pixels) {
            float red8 = static_cast<float>(static_cast<double>(
                static_cast<std::int16_t>((pixel >> 8) & 0xf8)) * factor);
            float green8 = static_cast<float>(static_cast<double>(
                static_cast<std::int16_t>((pixel >> 3) & 0xfc)) * factor);
            double blue8 = static_cast<double>((pixel & 0x1f) << 3) * factor;
            if (red8 > 255.0f) {
                red8 = 255.0f;
            }
            if (green8 > 255.0f) {
                green8 = 255.0f;
            }
            if (blue8 > 255.0) {
                blue8 = 255.0;
            }
            pixel = rgb565(static_cast<int>(red8), static_cast<int>(green8),
                           static_cast<int>(blue8));
        }
        tile.kind = static_cast<std::uint8_t>(kind);
        tile.rank = static_cast<std::uint8_t>(rank);
        ++tilesLeft_;
        // 0x00451730: a fly-away path (+0x30-+0x58) nothing reads, then the
        // spin the removal uses. Every rand() call is kept for the deal.
        for (int index2 = 0; index2 < 4; ++index2) {
            tile.flight[static_cast<std::size_t>(index2)] =
                static_cast<float>(random() % 0x300 + 0x80);
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            tile.spin[axis] = static_cast<float>(random() % 360) - 180.0f;
        }
    };

    // 0x004510d7, 0x00451115, 0x00451149: kinds 0-4 four times, then 5
    // and 6 once.
    int slot = 0;
    for (int copy = 0; copy < 4; ++copy) {
        for (int kind = 0; kind < 5; ++kind) {
            for (int rank = 0; rank < kKindCounts[static_cast<std::size_t>(kind)]; ++rank) {
                createTile(kind, rank, slot++);
            }
        }
    }
    for (int kind = 5; kind < 7; ++kind) {
        for (int rank = 0; rank < kKindCounts[static_cast<std::size_t>(kind)]; ++rank) {
            createTile(kind, rank, slot++);
        }
    }

    // 0x00451197-0x004511fe: remember the cap (layer 4) and the two side
    // end tiles with the two tiles each one blocks, then file the end
    // tiles (and layer 7) under layer 0.
    for (int index = 0; index < kTileCount; ++index) {
        Tile &tile = tiles_[static_cast<std::size_t>(index)];
        if (tile.layer == 4) {
            topTile_ = index;
        }
        if (tile.layer == 5) {
            leftEnd_ = index;
            leftEndNeighbours_ = {find(tile.column + 1, tile.row, 0),
                                  find(tile.column + 1, tile.row + 1, 0)};
            tile.layer = 0;
        }
        if (tile.layer == 6) {
            rightEnd_ = index;
            rightEndNeighbours_ = {find(tile.column - 1, tile.row, 0),
                                   find(tile.column - 1, tile.row + 1, 0)};
            tile.layer = 0;
        }
        if (tile.layer == 7) {
            tile.layer = 0;
        }
    }
    undo_.clear();
    updateFaces();
    flyIn_ = 0.0f;
}

std::optional<int> Board::find(int column, int row, int layer) const {
    for (int index = 0; index < created_; ++index) {
        const Tile &tile = tiles_[static_cast<std::size_t>(index)];
        if (tile.column == column && tile.row == row && tile.layer == layer) {
            return index;
        }
    }
    return std::nullopt;
}

bool Board::occupied(int column, int row, int layer) const {
    for (int index = 0; index < created_; ++index) {
        const Tile &tile = tiles_[static_cast<std::size_t>(index)];
        if (tile.column == column && tile.row == row && tile.layer == layer &&
            tile.alive) {
            return true;
        }
    }
    return false;
}

bool Board::blocked(int index) const {
    const Tile &tile = tiles_[static_cast<std::size_t>(index)];
    if (tile.layer == 3 && topTile_ &&
        tiles_[static_cast<std::size_t>(*topTile_)].alive) {
        return true;
    }
    if (leftEnd_ && (leftEndNeighbours_[0] == index ||
                     leftEndNeighbours_[1] == index) &&
        tiles_[static_cast<std::size_t>(*leftEnd_)].alive) {
        return true;
    }
    if (rightEnd_ && (rightEndNeighbours_[0] == index ||
                      rightEndNeighbours_[1] == index) &&
        tiles_[static_cast<std::size_t>(*rightEnd_)].alive) {
        return true;
    }
    const bool left = occupied(tile.column - 1, tile.row, tile.layer);
    const bool right = occupied(tile.column + 1, tile.row, tile.layer);
    const bool above = occupied(tile.column, tile.row, tile.layer + 1);
    return (left && right) || above;
}

bool Board::matches(const Tile &first, const Tile &second) {
    if (&first == &second) {
        return false;
    }
    if (first.kind == 5 || first.kind == 6) {
        return first.kind == second.kind;
    }
    if (first.kind <= 4) {
        return first.kind == second.kind && first.rank == second.rank;
    }
    return first.kind == second.kind;
}

int Board::availablePairs() const {
    std::int8_t pairs = 0;
    for (int first = 0; first < created_ - 1; ++first) {
        const Tile &a = tiles_[static_cast<std::size_t>(first)];
        if (!a.alive || blocked(first)) {
            continue;
        }
        for (int second = first + 1; second < created_; ++second) {
            const Tile &b = tiles_[static_cast<std::size_t>(second)];
            if (b.alive && !blocked(second) && matches(a, b)) {
                pairs = static_cast<std::int8_t>(pairs + 1);
            }
        }
    }
    return pairs;
}

void Board::hint() {
    for (int first = 0; first < created_ - 1; ++first) {
        Tile &a = tiles_[static_cast<std::size_t>(first)];
        if (!a.alive || blocked(first)) {
            continue;
        }
        for (int second = first + 1; second < created_; ++second) {
            Tile &b = tiles_[static_cast<std::size_t>(second)];
            if (b.alive && !blocked(second) && matches(a, b)) {
                // 0x004521a2-0x004521d0: blink both, out of phase.
                a.blinkRestoreSelected = a.selected;
                a.blinking = true;
                a.blinkCount = 0;
                a.blinkPhase = 0;
                a.blinkTimer = kBlinkStart;
                b.blinking = true;
                b.blinkRestoreSelected = b.selected;
                b.blinkCount = 0;
                b.blinkPhase = 1;
                b.blinkTimer = kBlinkStart;
                return;
            }
        }
    }
}

bool Board::cleared() const {
    return std::none_of(tiles_.begin(), tiles_.end(),
                        [](const Tile &tile) { return tile.alive; });
}

void Board::select(int index) {
    Tile &tile = tiles_[static_cast<std::size_t>(index)];
    if (tile.alive) {
        tile.selected = true;
        tile.top->layers[1].flags = 0;
    }
}

void Board::deselect(int index) {
    Tile &tile = tiles_[static_cast<std::size_t>(index)];
    if (tile.alive) {
        tile.selected = false;
        tile.top->layers[1].flags = 8;
    }
}

void Board::enableAllFaces(int index) {
    Tile &tile = tiles_[static_cast<std::size_t>(index)];
    for (const std::size_t face : {0, 1, 4, 5, 6, 7, 8, 9, 10, 11}) {
        tile.mesh->faces[face].enabled = true;
    }
}

void Board::removePair(int first, int second) {
    undo_.emplace_back(first, second);
    for (const int index : {first, second}) {
        Tile &tile = tiles_[static_cast<std::size_t>(index)];
        tile.alive = false;
        tile.removeTime = 0.0f;
        tile.removing = true;
        enableAllFaces(index);
    }
    updateFaces();
    tilesLeft_ -= 2;
}

void Board::undo() {
    if (undo_.empty()) {
        return;
    }
    const auto [first, second] = undo_.back();
    undo_.pop_back();
    for (const int index : {first, second}) {
        Tile &tile = tiles_[static_cast<std::size_t>(index)];
        tile.alive = true;
        tile.selected = false;
        tile.mesh->visible = true;
        tile.top->layers[1].flags = 8;
        tile.mesh->position = tile.position;
        tile.mesh->scale = {1.0f, 1.0f, 1.0f};
        tile.mesh->rotation = {0.0f, 0.0f, 90.0f};
        updateMeshMatrix(*tile.mesh);
    }
}

void Board::updateFaces() {
    for (int index = 0; index < created_; ++index) {
        Tile &tile = tiles_[static_cast<std::size_t>(index)];
        if (!tile.alive) {
            continue;
        }
        if (tile.layer == 3 ||
            !occupied(tile.column, tile.row, tile.layer + 1)) {
            tile.top->layers[0].flags = 4;
            tile.top->layers[1].flags = 8;
        } else {
            tile.top->layers[0].flags = 0;
            tile.top->layers[1].flags = 0;
        }
        auto &faces = tile.mesh->faces;
        const bool left = occupied(tile.column - 1, tile.row, tile.layer);
        faces[0].enabled = faces[1].enabled = !left;
        const bool right = occupied(tile.column + 1, tile.row, tile.layer);
        faces[4].enabled = faces[5].enabled = !right;
        faces[6].enabled = faces[7].enabled = false;
        const bool up = occupied(tile.column, tile.row - 1, tile.layer);
        faces[8].enabled = faces[9].enabled = !up;
        const bool down = occupied(tile.column, tile.row + 1, tile.layer);
        faces[10].enabled = faces[11].enabled = !down;
    }
}

std::optional<int> Board::pick(int screenX, int screenY,
                               std::uint8_t *ids) const {
    const int x = (screenX - kViewX) >> 1;
    const int y = (screenY - kViewY) >> 1;
    if (!(0 < x && 0 < y && x < kIdWidth && y < kIdHeight)) {
        return std::nullopt;
    }
    const std::uint8_t id = ids[x + y * kIdWidth];
    std::memset(ids, 0, kIdBufferSize);
    if (id == 0) {
        return std::nullopt;
    }
    const int index = id - 1;
    if (blocked(index)) {
        return std::nullopt;
    }
    return index;
}

void Board::placeCamera(float pitch, float yaw) {
    // 0x00451aa0: the camera orbits (0, 0, d) by pitch about x, then yaw
    // about y, and looks at (10, 0, 0) from 10 units to the right.
    Matrix pitchMatrix = identityMatrix();
    Matrix yawMatrix = identityMatrix();
    rotateX(pitchMatrix, pitch);
    rotateY(yawMatrix, yaw);
    const Matrix orbit = multiply(pitchMatrix, yawMatrix);
    float distance = kViewDistance;
    if (flyIn_ < 1.0f) {
        distance = static_cast<float>(
            static_cast<double>(smoothStep(0.0f, 1.0f, flyIn_)) * kFlyInDistance -
            kFlyInStart);
    }
    const std::array<float, 3> eye =
        transformPoint({0.0f, 0.0f, distance}, orbit);
    renderer_->setCamera({eye[0] + kTargetX, eye[1], eye[2]},
                         {kTargetX, 0.0f, 0.0f});
}

std::optional<std::pair<int, int>> Board::clickPoint(int index) {
    const Tile &tile = tiles_[static_cast<std::size_t>(index)];
    if (!tile.alive || blocked(index)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> ids(kIdBufferSize, 0);
    constexpr std::array<std::array<float, 2>, 9> kOffsets{{
        {0, 0}, {-6, 0}, {6, 0}, {0, -6}, {0, 6},
        {-9, -9}, {9, -9}, {-9, 9}, {9, 9},
    }};
    for (const auto &offset : kOffsets) {
        // The top quad is the tile's -z face.
        const std::array<float, 3> target{tile.position[0] + offset[0],
                                          tile.position[1] + offset[1],
                                          tile.position[2] - 16.0f};
        // The pitch changes steeply with y near the middle of the screen,
        // so instead of iterating y, scan it: for each row, settle x (yaw
        // changes slowly) and keep the row whose own camera projects the
        // target closest to it.
        int x = kViewX + kViewWidth / 2;
        int y = -1;
        double bestError = 1.5;
        for (int row = kViewY + 2; row < kViewY + kViewHeight - 2; ++row) {
            int column = kViewX + kViewWidth / 2;
            std::array<float, 2> screen{};
            for (int iteration = 0; iteration < 4; ++iteration) {
                const CameraAngles angles = pointerCamera(column, row);
                placeCamera(angles.pitch, angles.yaw);
                screen = renderer_->project(target);
                column = kViewX + static_cast<int>(std::lround(screen[0]));
            }
            const double error = std::abs(static_cast<double>(screen[1]) +
                                          kViewY - row);
            if (error < bestError) {
                bestError = error;
                x = column;
                y = row;
            }
        }
        if (y < 0) {
            continue;
        }
        const CameraAngles angles = pointerCamera(x, y);
        placeCamera(angles.pitch, angles.yaw);
        std::fill(ids.begin(), ids.end(), std::uint8_t{0});
        renderer_->render(nullptr, kViewWidth, ids.data());
        const auto idAt = [&](int px, int py) -> int {
            const int ix = (px - kViewX) >> 1;
            const int iy = (py - kViewY) >> 1;
            if (!(0 < ix && 0 < iy && ix < kIdWidth && iy < kIdHeight)) {
                return 0;
            }
            return ids[static_cast<std::size_t>(ix + iy * kIdWidth)];
        };
        bool solid = true;
        for (const auto &[dx, dy] : std::array<std::pair<int, int>, 5>{{
                 {0, 0}, {-4, 0}, {4, 0}, {0, -4}, {0, 4}}}) {
            if (idAt(x + dx, y + dy) != index + 1) {
                solid = false;
            }
        }
        if (solid) {
            return std::pair{x, y};
        }
    }
    return std::nullopt;
}

void Board::render(float dt, float pitch, float yaw, std::uint16_t *frame,
                   std::uint8_t *ids) {
    placeCamera(pitch, yaw);
    if (flyIn_ < 1.0f) {
        flyIn_ = static_cast<float>(static_cast<double>(dt) * kFlyInSpeed + flyIn_);
    }
    std::memcpy(frame, background_.data(),
                background_.size() * sizeof(std::uint16_t));
    renderer_->render(frame, kViewWidth, ids);

    for (Tile &tile : tiles_) {
        if (tile.removing) {
            const float progress =
                smoothStep(0.0f, 1.0f, kRemoveSpeed * tile.removeTime);
            const float scale = static_cast<float>(progress * -1.0 + 1.0);
            tile.mesh->scale = {scale, scale, scale};
            const float spin = smoothStep(0.0f, 1.0f, kRemoveSpeed * tile.removeTime);
            tile.mesh->rotation = {
                static_cast<float>(static_cast<double>(tile.spin[0]) * spin),
                static_cast<float>(static_cast<double>(tile.spin[1]) * spin),
                static_cast<float>(static_cast<double>(tile.spin[2]) * spin)};
            updateMeshMatrix(*tile.mesh);
            tile.removeTime = dt + tile.removeTime;
            if (static_cast<float>(kRemoveDuration) < tile.removeTime) {
                tile.removing = false;
                tile.mesh->visible = false;
            }
        }
        if (tile.blinking) {
            tile.blinkTimer = dt + tile.blinkTimer;
            if (kBlinkInterval < tile.blinkTimer) {
                tile.blinkTimer = 0.0f;
                ++tile.blinkCount;
                tile.top->layers[1].flags = tile.blinkPhase == 0 ? 8U : 0U;
                tile.blinkPhase ^= 1U;
                if (tile.blinkCount > 7) {
                    tile.blinking = false;
                    tile.top->layers[1].flags =
                        tile.blinkRestoreSelected ? 0U : 8U;
                }
            }
        }
    }
}

} // namespace fgp::mahjong
