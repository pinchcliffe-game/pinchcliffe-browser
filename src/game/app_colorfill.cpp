#include "game/app_colorfill.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {
namespace {

// 0x00462fe0 sets the canvas size 0x006a4270 to 0x1ea.
constexpr int kSize = 490;
constexpr std::size_t kPixels = static_cast<std::size_t>(kSize) * kSize;
// Canvas descriptor 0x00505f00: hotspot 1 at +0x50, position (32,93).
constexpr SDL_Point kCanvas{32, 93};
// Colour swatch descriptor 0x00505fa8 at (566,161); color.bmp is 146x28.
constexpr SDL_Rect kSwatch{566, 161, 146, 28};
// Cross descriptor 0x00506050 at (563,310).
constexpr SDL_Point kCrossDefault{563, 310};
// 0x0049fcc8: the palette rectangle scanned by 0x00463500.
constexpr SDL_Rect kPalette{564, 200, 716 - 564, 484 - 200};
// 0x0049fcd8: the brush list rectangle.
constexpr SDL_Rect kBrushList{729, 160, 765 - 729, 484 - 160};
// 0x0049fcb8: the title text box (205,23)-(581,73).
constexpr SDL_Rect kTitle{205, 23, 581 - 205, 73 - 23};
// Scrollbar 0x1e (0x00463825): x 773, track y 191 height 261, 14 wide;
// arrows at y 161 and 467 (0x00463839/0x00463851).
constexpr int kScrollX = 773;
constexpr int kScrollY = 0xbf;
constexpr int kScrollWidth = 0x0e;
constexpr int kScrollHeight = 0x105;
constexpr int kScrollUpY = 0xa1;
constexpr int kScrollDownY = 0x1d3;
// 0x00408c70 numbers scrollbar arrow controls from 2000.
constexpr int kScrollUpId = 2000;
constexpr int kScrollDownId = 2001;
// List highlight 0x00407d90(list, 0x23234b) is a COLORREF.
constexpr SDL_Color kHighlight{0x4b, 0x23, 0x23, 255};

enum Tool { Pen = 1, Bucket = 2, Eraser = 3, Picker = 4, Light = 5 };

// Controls in creation order (0x00463182-0x004632d5). Tools are radio
// buttons of group 1; the bottom four are push buttons with captions.
struct ControlSpec {
    int id;
    int x;
    int y;
    int style;
    const char *normal;
    const char *pressed;
    const char *caption;
};
constexpr std::array<ControlSpec, 8> kControls{{
    {3, 0x233, 0x5b, 2, "pen_off", "pen_on", nullptr},
    {4, 0x260, 0x5b, 2, "buck_off", "buck_on", nullptr},
    {5, 0x28d, 0x5b, 2, "erase_off", "erase_on", nullptr},
    {6, 699, 0x5b, 2, "pick_off", "pick_on", nullptr},
    {7, 0x2e8, 0x5b, 2, "light_off", "light_on", nullptr},
    {0x14, 0x231, 0x223, 1, "canvas_0", "canvas_1", "CANVAS"},
    {0x15, 0x2a5, 0x1fd, 1, "clear_0", "clear_1", "CLEAR"},
    {0x16, 0x2a6, 0x223, 1, "activity_0", "activity_1", "ACTIVITY"},
}};
// Undo is created last, after Activity (0x004632c9-0x004632d5).
constexpr ControlSpec kUndoControl{
    0x17, 0x231, 0x1fd, 1, "undo_0", "undo_1", "UNDO"};

// Tooltip table 0x00506100: registered id -> language key.
constexpr std::array<std::pair<int, const char *>, 10> kTips{{
    {3, "TIP_BRUSH"}, {4, "TIP_PAINT"}, {5, "TIP_ERASER"},
    {6, "TIP_PICKUP"}, {7, "TIP_LIGHT"}, {0x14, "TIP_CANVAS"},
    {0x15, "TIP_CLEAR"}, {0x16, "TIP_ACTIVITY"}, {0x17, "TIP_UNDO"},
    {2, "TIP_COLOR"},
}};

struct DecodedBitmap {
    int width{};
    int height{};
    int bitsPerPixel{};
    std::vector<std::array<std::uint8_t, 3>> palette;
    // Palette indices for 8 bpp, else 3 bytes (R,G,B) per pixel.
    std::vector<std::uint8_t> pixels;
};

// The subset of BMP the colorfill assets use: 8 bpp (plain or RLE8) and
// 24 bpp. Rows come out top first, as 0x00414c40 flips bottom-up files.
std::optional<DecodedBitmap> decodeBitmap(std::span<const std::uint8_t> b) {
    auto u16 = [&](std::size_t o) {
        return static_cast<unsigned>(b[o] | (b[o + 1] << 8));
    };
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8) |
                                          (b[o + 2] << 16) |
                                          (static_cast<unsigned>(b[o + 3]) << 24));
    };
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return std::nullopt;
    const std::uint32_t offset = u32(10);
    const std::uint32_t header = u32(14);
    DecodedBitmap out;
    out.width = static_cast<std::int32_t>(u32(18));
    const auto rawHeight = static_cast<std::int32_t>(u32(22));
    out.height = std::abs(rawHeight);
    out.bitsPerPixel = static_cast<int>(u16(28));
    const std::uint32_t compression = u32(30);
    if (out.width <= 0 || out.height <= 0 || offset > b.size()) {
        return std::nullopt;
    }
    const auto w = static_cast<std::size_t>(out.width);
    const auto h = static_cast<std::size_t>(out.height);
    if (out.bitsPerPixel == 8) {
        std::uint32_t colours = u32(46);
        if (colours == 0) colours = 256;
        for (std::uint32_t i = 0; i < colours; ++i) {
            const std::size_t p = 14 + header + i * 4;
            if (p + 3 > b.size()) break;
            out.palette.push_back({b[p + 2], b[p + 1], b[p]});
        }
        out.pixels.assign(w * h, 0);
        auto put = [&](std::size_t x, std::size_t row, std::uint8_t v) {
            if (x < w && row < h) out.pixels[row * w + x] = v;
        };
        if (compression == 1) {
            std::size_t i = offset, x = 0, row = 0;
            while (i + 1 < b.size()) {
                const std::uint8_t n = b[i], v = b[i + 1];
                i += 2;
                if (n != 0) {
                    for (std::uint8_t k = 0; k < n; ++k) put(x++, row, v);
                } else if (v == 0) {
                    x = 0;
                    ++row;
                } else if (v == 1) {
                    break;
                } else if (v == 2) {
                    if (i + 1 >= b.size()) break;
                    x += b[i];
                    row += b[i + 1];
                    i += 2;
                } else {
                    for (std::uint8_t k = 0; k < v && i + k < b.size(); ++k) {
                        put(x++, row, b[i + k]);
                    }
                    i += v + (v & 1U);
                }
            }
        } else {
            const std::size_t stride = (w + 3) & ~std::size_t{3};
            for (std::size_t row = 0; row < h; ++row) {
                for (std::size_t x = 0; x < w; ++x) {
                    const std::size_t p = offset + row * stride + x;
                    if (p < b.size()) put(x, row, b[p]);
                }
            }
        }
    } else if (out.bitsPerPixel == 24) {
        out.pixels.assign(w * h * 3, 0);
        const std::size_t stride = (w * 3 + 3) & ~std::size_t{3};
        for (std::size_t row = 0; row < h; ++row) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t p = offset + row * stride + x * 3;
                if (p + 2 >= b.size()) continue;
                std::uint8_t *d = &out.pixels[(row * w + x) * 3];
                d[0] = b[p + 2];
                d[1] = b[p + 1];
                d[2] = b[p];
            }
        }
    } else {
        return std::nullopt;
    }
    if (rawHeight > 0) {
        const std::size_t rowBytes = out.pixels.size() / h;
        for (std::size_t row = 0; row < h / 2; ++row) {
            std::swap_ranges(out.pixels.begin() + static_cast<std::ptrdiff_t>(row * rowBytes),
                             out.pixels.begin() + static_cast<std::ptrdiff_t>((row + 1) * rowBytes),
                             out.pixels.begin() + static_cast<std::ptrdiff_t>((h - 1 - row) * rowBytes));
        }
    }
    return out;
}

// UNCERTAIN: how the engine's surface loader 0x00413940 maps 8-bit
// palette entries and 24-bit pixels to the 16-bit display surface. Plain
// truncation to RGB565 is assumed (the reverse 0x00462500 is a plain
// shift); captures confirm or refute it through the colour logpoint.
std::uint16_t pack565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) |
                                      (b >> 3));
}

// 0x00462500: 565 -> 0x00RRGGBB by plain shifts.
std::uint32_t expand565(std::uint16_t p) {
    const std::uint32_t r = (p >> 11) & 0x1fU, g = (p >> 5) & 0x3fU,
                        b = p & 0x1fU;
    return (r << 19) | (g << 10) | (b << 3);
}

// 0x0046bf80: 0x00RRGGBB -> 565 by plain shifts.
std::uint16_t pack888(std::uint32_t v) {
    return static_cast<std::uint16_t>((((v >> 10) & 0x3fU) << 5) |
                                      (((v >> 19) & 0x1fU) << 11) |
                                      ((v >> 3) & 0x1fU));
}

// 0x00462560: scale every channel by factor/256.
std::uint32_t scaleRgb(std::uint32_t v, std::uint32_t factor) {
    factor &= 0xffU;
    const std::uint32_t r = (((v >> 16) & 0xffU) * factor) >> 8;
    const std::uint32_t g = (((v >> 8) & 0xffU) * factor) >> 8;
    const std::uint32_t b = ((v & 0xffU) * factor) >> 8;
    return (r << 16) | (g << 8) | b;
}

// Display colour of a 565 pixel: captures of the original under DxWnd
// show plain shifts (565 white is (248,252,248) on screen).
SDL_Color displayColour(std::uint16_t p) {
    return {static_cast<std::uint8_t>(((p >> 11) & 0x1fU) << 3),
            static_cast<std::uint8_t>(((p >> 5) & 0x3fU) << 2),
            static_cast<std::uint8_t>((p & 0x1fU) << 3), 255};
}

// MSVC _ftol (0x0048c214) truncates toward zero.
int ftol(double value) { return static_cast<int>(value); }

// 0x004620c0 with a 565 surface: channel -> 0..255 through the float
// constants 1/31 (0x0049fcf8) and 1/63 (0x0049fcf4) and 255 (0x0049dac4).
// The red product stays on the x87 stack; green and blue are stored as
// floats first. The x87 precision is assumed to be the MSVC default of
// 53 bits, so the intermediate products are doubles.
std::uint32_t normalRgb(std::uint16_t p) {
    const double c31 = static_cast<double>(0.032258064F);
    const double c63 = static_cast<double>(0.015873017F);
    const double red = static_cast<double>((p >> 11) & 0x1fU) * c31;
    const float green = static_cast<float>(
        static_cast<double>((p >> 5) & 0x3fU) * c63);
    const float blue = static_cast<float>(
        static_cast<double>(p & 0x1fU) * c31);
    const int r = ftol(red * 255.0);
    const int g = ftol(static_cast<double>(green) * 255.0);
    const int b = ftol(static_cast<double>(blue) * 255.0);
    return static_cast<std::uint32_t>((r << 16) | (g << 8) | b);
}

struct LightVector {
    int x;
    int y;
    int z;
};

// 0x00461e20: the light direction from a canvas point. Constants: 1/245
// (0x0049fcf0), 0.36 (0x0049fcec), 1.0 (0x0049c758), 127 (0x0049fce8),
// 0.6 (0x0049e070). dy and 1/len are stored as floats (0x00461e6b,
// 0x00461e87); dx and the first 1/len stay on the x87 stack.
LightVector lightVector(int x, int y) {
    const double scale = static_cast<double>(0.0040816324F);
    const double dx = static_cast<double>(x - 245) * scale;
    const double dyFull = static_cast<double>(y - 245) * scale;
    const float dy = static_cast<float>(dyFull);
    const double dy2 = dyFull * static_cast<double>(dy);
    const double dx2 = dx * dx;
    double sum = dx2 + dy2;
    sum = sum + static_cast<double>(0.36F);
    const double root = std::sqrt(sum);
    const double inverse = 1.0 / root;
    const float inverseStored = static_cast<float>(inverse);
    LightVector v{};
    double t = inverse * dx;
    v.x = ftol(t * 127.0);
    t = static_cast<double>(inverseStored) * static_cast<double>(dy);
    v.y = ftol(t * 127.0);
    t = static_cast<double>(inverseStored) * static_cast<double>(0.6F);
    v.z = ftol(t * 127.0);
    return v;
}

std::vector<ColorfillScene::ColorfillIniSection>
parseIniSections(std::span<const std::uint8_t> bytes) {
    std::vector<ColorfillScene::ColorfillIniSection> sections;
    std::string contents(reinterpret_cast<const char *>(bytes.data()),
                         bytes.size());
    std::size_t position = 0;
    while (position < contents.size()) {
        auto end = contents.find_first_of("\r\n", position);
        if (end == std::string::npos) end = contents.size();
        const std::string line = trim(contents.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            sections.push_back({line.substr(1, line.size() - 2), {}});
        } else if (!sections.empty()) {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                sections.back().values.emplace_back(
                    trim(line.substr(0, equals)),
                    trim(line.substr(equals + 1)));
            }
        }
    }
    return sections;
}

std::string iniValue(const ColorfillScene::ColorfillIniSection &section,
                     std::string_view key, std::string fallback) {
    for (const auto &[name, value] : section.values) {
        if (normalizedPath(name) == normalizedPath(std::string(key))) {
            return value;
        }
    }
    return fallback;
}

// 0x0040e3a0 reads an integer key with a default.
int iniInt(const ColorfillScene::ColorfillIniSection &section,
           std::string_view key, int fallback) {
    const std::string value = iniValue(section, key, {});
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (const std::exception &) {
        return fallback;
    }
}

std::size_t at(int x, int y) {
    return static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x);
}

TexturePtr textureFromRgb(SDL_Renderer *renderer, int width, int height,
                          const std::vector<SDL_Color> &pixels) {
    SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
    if (!surface) throw std::runtime_error(SDL_GetError());
    for (int y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<std::uint32_t *>(
            static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < width; ++x) {
            const SDL_Color c = pixels[static_cast<std::size_t>(y) * width + x];
            row[x] = (static_cast<std::uint32_t>(c.a) << 24) |
                     (static_cast<std::uint32_t>(c.r) << 16) |
                     (static_cast<std::uint32_t>(c.g) << 8) | c.b;
        }
    }
    TexturePtr texture = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

} // namespace

std::optional<ColorfillScene::ColorfillIndexed>
ColorfillScene::loadColorfillIndexed(std::string_view name) const {
    // 0x00461fa0 formats "%s/colorfill/bitmap/%s.bmp" and loads it raw
    // with 0x004149f0; a missing file yields a null image.
    if (!scenePackage || name.empty()) return std::nullopt;
    const auto *entry =
        scenePackage->findOptional("bitmap\\" + std::string(name) + ".bmp");
    if (!entry) return std::nullopt;
    const auto decoded = decodeBitmap(scenePackage->data(*entry));
    if (!decoded || decoded->bitsPerPixel != 8) return std::nullopt;
    return ColorfillIndexed{decoded->width, decoded->height, decoded->pixels};
}

std::vector<std::uint16_t>
ColorfillScene::loadColorfillSurface(std::string_view name) const {
    // 0x00413940 loads a bitmap into a display surface (RGB565).
    std::vector<std::uint16_t> out(kPixels, 0xffffU);
    if (!scenePackage) return out;
    const auto *entry =
        scenePackage->findOptional("bitmap\\" + std::string(name) + ".bmp");
    if (!entry) return out;
    const auto decoded = decodeBitmap(scenePackage->data(*entry));
    if (!decoded) return out;
    for (int y = 0; y < std::min(decoded->height, kSize); ++y) {
        for (int x = 0; x < std::min(decoded->width, kSize); ++x) {
            const std::size_t i =
                static_cast<std::size_t>(y) * decoded->width + x;
            if (decoded->bitsPerPixel == 8) {
                const std::uint8_t index = decoded->pixels[i];
                const auto colour = index < decoded->palette.size()
                                        ? decoded->palette[index]
                                        : std::array<std::uint8_t, 3>{};
                out[at(x, y)] = pack565(colour[0], colour[1], colour[2]);
            } else {
                out[at(x, y)] = pack565(decoded->pixels[i * 3],
                                        decoded->pixels[i * 3 + 1],
                                        decoded->pixels[i * 3 + 2]);
            }
        }
    }
    return out;
}

const ColorfillScene::ColorfillIniSection *ColorfillScene::colorfillSection(
    const std::vector<ColorfillIniSection> &sections,
    std::string_view name) const {
    // 0x0040db80 selects a section by name (case-insensitive ini lookup).
    for (const auto &section : sections) {
        if (normalizedPath(section.name) == normalizedPath(std::string(name))) {
            return &section;
        }
    }
    return nullptr;
}

ColorfillScene::ColorfillGame ColorfillScene::loadColorfillGame(
    Package &package) {
    ColorfillGame game;
    if (const auto *entry = package.findOptional("canvas.ini")) {
        game.canvasSections = parseIniSections(package.data(*entry));
    }
    if (const auto *entry = package.findOptional("brush.ini")) {
        game.brushSections = parseIniSections(package.data(*entry));
    }
    game.paint.assign(kPixels, 0xffffU);
    game.undo.assign(kPixels, 0xffffU);
    game.rendered.assign(kPixels, 0xffffU);
    game.outlineFlags.assign(kPixels, 0U);
    game.normals.assign(kPixels * 3, 0);
    game.light.assign(kPixels, 0xffU);
    game.paletteMap.assign(0x10000U, SDL_Point{kCrossDefault.x + 7,
                                               kCrossDefault.y + 7});
    if (const auto *entry = package.findOptional("bitmap\\cross.bmp")) {
        SurfacePtr surface = loadGreenKeyedBmp(package.data(*entry));
        game.crossTexture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(game.crossTexture.get(), SDL_BLENDMODE_BLEND);
    }
    game.tool = colorfillLastTool;
    game.canvasTexture.reset(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSize, kSize));
    return game;
}

void ColorfillScene::restoreColorfillState() {
    // Called from updateColorfill on the first frame, once colorfillGame is
    // in place: the rest of 0x00462fe0 from the canvas descriptors on.
    auto &game = *colorfillGame;
    // 0x00463007-0x0046303f: canvas, color and cross at layer 10.
    referenceTrace.emit("animation_add", {{"asset", "canvas"}, {"x", -1},
                                          {"y", -1}, {"layer", 10},
                                          {"hotspot", 1}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {{"asset", "color"}, {"x", -1},
                                          {"y", -1}, {"layer", 10},
                                          {"hotspot", 0}, {"conflict_mask", 0}});
    referenceTrace.emit("animation_add", {{"asset", "cross"}, {"x", -1},
                                          {"y", -1}, {"layer", 10},
                                          {"hotspot", 0}, {"conflict_mask", 0}});
    game.colour = 0xffffU;
    if (const auto *entry = common.findOptional("bitmap\\tooltip.bmp")) {
        SurfacePtr loaded = loadGreenKeyedBmp(common.data(*entry));
        game.tipFrame.reset(SDL_ConvertSurfaceFormat(loaded.get(),
                                                     SDL_PIXELFORMAT_ARGB8888, 0));
    }
    if (const auto *entry = common.findOptional("bitmap\\tooldot.bmp")) {
        SurfacePtr dot = loadBmp(common.data(*entry));
        SDL_SetColorKey(dot.get(), SDL_TRUE, pixelValue(dot.get(), 0, 0));
        game.tipDot = std::move(dot);
    }
    if (const auto *entry = common.findOptional("fonts\\Small Fonts_07_.bmp")) {
        game.tipFont = loadBitmapFont(renderer, common.data(*entry));
    }
    game.pickerCursor = loadColorfillIndexed("picker");
    game.sunCursor = loadColorfillIndexed("sun");
    game.bucketCursor = loadColorfillIndexed("bucket");

    // 0x00463380: the saved picture, if the profile has one.
    bool restored = false;
    if (!profiles.empty() && profiles[profileIndex] != "No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        try {
            const auto header = profileSceneRecordBytes(
                profilePath, "colorfill", 0x402U, 0xdeadU, 0x218U);
            if (header) {
                const auto readString = [&](std::size_t offset) {
                    const char *text =
                        reinterpret_cast<const char *>(header->data() + offset);
                    return std::string(text, strnlen(text, 256));
                };
                const int lightX = static_cast<std::int32_t>(readLe32(*header, 0));
                const int lightY = static_cast<std::int32_t>(readLe32(*header, 4));
                const int crossX = static_cast<std::int32_t>(readLe32(*header, 8));
                const int crossY = static_cast<std::int32_t>(readLe32(*header, 12));
                const std::uint32_t packed = readLe32(*header, 0x210);
                if (loadColorfillPicture(readString(16), false)) {
                    loadColorfillBrush(readString(16 + 256));
                    pickColorfillPalette(crossX, crossY);
                    const auto data = profileSceneRecordBytes(
                        profilePath, "colorfill", 0x402U, 0xbabeU, packed);
                    // 0x004638c0: control byte, high bit = run of the next
                    // byte, else that many literal bytes.
                    std::vector<std::uint8_t> planes(kPixels * 3, 0U);
                    if (data) {
                        std::size_t in = 0, out = 0;
                        while (in < data->size()) {
                            const std::uint8_t control = (*data)[in++];
                            const std::size_t count = control & 0x7fU;
                            if (control & 0x80U) {
                                const std::uint8_t value =
                                    in < data->size() ? (*data)[in] : 0U;
                                ++in;
                                for (std::size_t k = 0; k < count && out < planes.size(); ++k)
                                    planes[out++] = value;
                            } else {
                                for (std::size_t k = 0; k < count && in < data->size(); ++k) {
                                    if (out < planes.size()) planes[out] = (*data)[in];
                                    ++out;
                                    ++in;
                                }
                            }
                        }
                    }
                    // 0x0046340e-0x004634b5: blue, green, red planes.
                    for (std::size_t i = 0; i < kPixels; ++i) {
                        const std::uint32_t rgb =
                            (static_cast<std::uint32_t>(planes[i + kPixels * 2]) << 16) |
                            (static_cast<std::uint32_t>(planes[i + kPixels]) << 8) |
                            planes[i];
                        game.paint[i] = pack888(rgb);
                    }
                    relightColorfill(lightX, lightY);
                    renderColorfillCanvas(std::nullopt);
                    restored = true;
                }
            }
        } catch (const std::exception &error) {
            std::cerr << "Could not restore Color Fill: " << error.what() << '\n';
        }
    }
    if (!restored) {
        // 0x0046327a-0x00463290.
        loadColorfillPicture("colorfill:I1", true);
        loadColorfillBrush("square4");
    }

    // Tool buttons, group 1 (0x00406fb0), then 0x00406ac0(1,0) and
    // 0x00406720(3,1) select the pen. Push buttons follow (0x00406ed0).
    auto addControl = [&](const ControlSpec &spec, int group) {
        ColorfillControl control;
        control.id = spec.id;
        control.x = spec.x;
        control.y = spec.y;
        control.style = spec.style;
        control.group = group;
        control.images = {spec.normal, spec.pressed};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto *entry = scenePackage->findOptional(
                "bitmap\\" + control.images[i] + ".bmp");
            if (!entry) continue;
            SurfacePtr surface = loadGreenKeyedBmp(scenePackage->data(*entry));
            control.sizes[i] = {surface->w, surface->h};
            control.textures[i] = makeTexture(renderer, surface.get());
            SDL_SetTextureBlendMode(control.textures[i].get(), SDL_BLENDMODE_BLEND);
        }
        if (spec.caption) {
            control.caption = language.get("colorfill", spec.caption, {});
        }
        control.shownImage = control.images[0];
        game.controls.push_back(std::move(control));
        referenceTrace.emit("animation_add",
                            {{"asset", game.controls.back().images[0]},
                             {"x", -1}, {"y", -1}, {"layer", 1},
                             {"hotspot", spec.id}, {"conflict_mask", 0}});
    };
    for (std::size_t i = 0; i < 5; ++i) addControl(kControls[i], 1);
    for (auto &control : game.controls) {
        if (control.id == 3) setColorfillControlState(control, 1);
    }
    for (std::size_t i = 5; i < kControls.size(); ++i) addControl(kControls[i], 0);
    addControl(kUndoControl, 0);
    game.tool = Pen;

    // 0x00463670: the brush column.
    const auto preview = [&](const ColorfillIniSection &section) {
        std::vector<SDL_Color> pixels(36 * 36, SDL_Color{0, 0, 0, 0});
        const std::string name = iniValue(section, "preview", "blank");
        if (const auto *entry =
                scenePackage->findOptional("bitmap\\" + name + ".bmp")) {
            if (const auto decoded = decodeBitmap(scenePackage->data(*entry))) {
                // 0x0046378d-0x004637ac: keyed by the preview's own (0,0)
                // pixel and drawn at (2,2) on a green-keyed 36x36 surface.
                auto colourAt = [&](int x, int y) {
                    const std::size_t i =
                        static_cast<std::size_t>(y) * decoded->width + x;
                    if (decoded->bitsPerPixel == 8) {
                        const auto c = decoded->pixels[i] < decoded->palette.size()
                                           ? decoded->palette[decoded->pixels[i]]
                                           : std::array<std::uint8_t, 3>{};
                        return pack565(c[0], c[1], c[2]);
                    }
                    return pack565(decoded->pixels[i * 3], decoded->pixels[i * 3 + 1],
                                   decoded->pixels[i * 3 + 2]);
                };
                const std::uint16_t key = colourAt(0, 0);
                for (int y = 0; y < decoded->height && y + 2 < 36; ++y) {
                    for (int x = 0; x < decoded->width && x + 2 < 36; ++x) {
                        const std::uint16_t c = colourAt(x, y);
                        if (c == key || c == 0x07e0U) continue;
                        pixels[static_cast<std::size_t>(y + 2) * 36 + x + 2] =
                            displayColour(c);
                    }
                }
            }
        }
        return textureFromRgb(renderer, 36, 36, pixels);
    };
    int selected = 0;
    for (std::size_t i = 0; i < game.brushSections.size(); ++i) {
        if (game.brushSections[i].name == game.brush) {
            selected = static_cast<int>(i);
        }
        game.brushItems.push_back(preview(game.brushSections[i]));
    }
    game.brushVisible = std::min(32, kBrushList.h / 36);
    game.scrollMaximum = std::max(
        0, static_cast<int>(game.brushItems.size()) - game.brushVisible);
    if (const auto *entry = scenePackage->findOptional("bitmap\\scrolltab.bmp")) {
        SurfacePtr surface = loadGreenKeyedBmp(scenePackage->data(*entry));
        game.scrollTabSize = {surface->w, surface->h};
        game.scrollTab = makeTexture(renderer, surface.get());
    }
    for (const auto &[id, y, normal, pressed] :
         std::array<std::tuple<int, int, const char *, const char *>, 2>{{
             {kScrollUpId, kScrollUpY, "up_0", "up_1"},
             {kScrollDownId, kScrollDownY, "down_0", "down_1"}}}) {
        addControl(ControlSpec{id, kScrollX, y, 1, normal, pressed, nullptr}, 0);
    }
    game.scrollValue = 0;
    setColorfillScroll(selected);
    // 0x00407e60 selects without reporting the brush again.
    game.brushSelected = selected;
    if (selected < game.brushTop ||
        selected >= game.brushTop + game.brushVisible) {
        game.brushTop = std::clamp(
            selected > game.brushTop ? selected + 1 - game.brushVisible : selected,
            0, game.scrollMaximum);
    }

    // 0x00463500: palette colour positions from the hotspot-2 pixels.
    if (sceneBackdrop && sceneHotspots) {
        for (int y = kPalette.y; y < kPalette.y + kPalette.h; ++y) {
            for (int x = kPalette.x; x < kPalette.x + kPalette.w; ++x) {
                if (pixelValue(sceneHotspots.get(), x, y) != 2U) continue;
                Uint8 r = 0, g = 0, b = 0;
                SDL_GetRGB(pixelValue(sceneBackdrop.get(), x, y),
                           sceneBackdrop->format, &r, &g, &b);
                game.paletteMap[pack565(r, g, b)] = {x, y};
            }
        }
    }
    // 0x00463301: the undo copy starts as the current picture.
    game.undo = game.paint;
}

bool ColorfillScene::loadColorfillPicture(std::string_view name,
                                          bool resetLight) {
    auto &game = *colorfillGame;
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "picture"},
                         {"name", std::string(name)},
                         {"reset", resetLight ? 1 : 0}});
    // 0x00461b50.
    const auto *section = colorfillSection(game.canvasSections, name);
    if (!section) return false;
    game.picture = std::string(name);
    // 0x00461b9f-0x00461bcb: the outline becomes the paint surface;
    // 0x00462000 keeps pure-black pixels as outline, the rest white.
    game.paint = loadColorfillSurface(iniValue(*section, "outline", "blank"));
    game.outline = loadColorfillIndexed(iniValue(*section, "outline", "blank"));
    for (std::size_t i = 0; i < kPixels; ++i) {
        game.outlineFlags[i] = game.paint[i] == 0U ? 1U : 0U;
        if (!game.outlineFlags[i]) game.paint[i] = 0xffffU;
    }
    game.mask = loadColorfillIndexed(iniValue(*section, "mask", "blank"));
    // 0x00461cc0: normals, channel - 127 clamped to -127..127. The normal
    // map is loaded with 0x00405d00(1), which makes 0x00405fa0 select
    // SetStretchBltMode(HALFTONE) before StretchBlt-ing the 24-bit bitmap
    // onto the 16-bit surface. UNCERTAIN: that GDI halftone dither is not
    // part of FGP.exe; native truncates to 565 instead. Against a capture of
    // picture I2 at the default light, 31% of the lit pixels then differ by
    // one shading step; the mask, outline, fill region and every pixel
    // outside the mask match exactly. A learned 4x4 ordered threshold
    // explains 90% of the original's 5-bit values, truncation 65%.
    const auto normals =
        loadColorfillSurface(iniValue(*section, "normals", "blank"));
    for (std::size_t i = 0; i < kPixels; ++i) {
        const std::uint32_t rgb = normalRgb(normals[i]);
        game.normals[i * 3] = static_cast<std::int8_t>(
            std::clamp(static_cast<int>((rgb >> 16) & 0xffU) - 0x7f, -0x7f, 0x7f));
        game.normals[i * 3 + 1] = static_cast<std::int8_t>(
            std::clamp(static_cast<int>((rgb >> 8) & 0xffU) - 0x7f, -0x7f, 0x7f));
        game.normals[i * 3 + 2] = static_cast<std::int8_t>(
            std::clamp(static_cast<int>(rgb & 0xffU) - 0x7f, -0x7f, 0x7f));
    }
    game.undo = game.paint;
    if (resetLight) {
        relightColorfill(iniInt(*section, "light_x", kSize / 2),
                         iniInt(*section, "light_y", kSize / 2));
        renderColorfillCanvas(std::nullopt);
    }
    return true;
}

void ColorfillScene::loadColorfillBrush(std::string_view name) {
    auto &game = *colorfillGame;
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "brush"},
                         {"name", std::string(name)}});
    // 0x00462cd0.
    const auto *section = colorfillSection(game.brushSections, name);
    if (!section) return;
    game.brush = std::string(name);
    game.brushImage = loadColorfillIndexed(iniValue(*section, "brush", "blank"));
    game.brushOutline =
        loadColorfillIndexed(iniValue(*section, "outline", "blank"));
}

void ColorfillScene::relightColorfill(int x, int y) {
    auto &game = *colorfillGame;
    referenceTrace.emit("minigame_state", {{"game", "colorfill"},
                                           {"state", "light"},
                                           {"x", x}, {"y", y}});
    game.lightX = x;
    game.lightY = y;
    const LightVector l = lightVector(x, y);
    for (std::size_t i = 0; i < kPixels; ++i) {
        const bool inside =
            game.mask && i < game.mask->pixels.size() && game.mask->pixels[i] == 0U;
        if (!inside) {
            game.light[i] = 0xffU;
            continue;
        }
        const int value = ((game.normals[i * 3 + 2] * l.z >> 7) +
                           (game.normals[i * 3 + 1] * l.y >> 7) +
                           (game.normals[i * 3] * l.x >> 7)) * 2;
        game.light[i] = static_cast<std::uint8_t>(std::clamp(value, 0, 0xff));
    }
}

void ColorfillScene::renderColorfillCanvas(std::optional<SDL_Rect> area) {
    // 0x004622d0 with an inclusive-exclusive {left,top,right,bottom} rect.
    auto &game = *colorfillGame;
    SDL_Rect r = area.value_or(SDL_Rect{0, 0, kSize, kSize});
    const int left = std::max(r.x, 0);
    const int top = std::max(r.y, 0);
    const int right = std::min(r.w, kSize);
    const int bottom = std::min(r.h, kSize);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const std::size_t i = at(x, y);
            const bool inside = game.mask && game.mask->pixels[i] == 0U;
            if (!inside) {
                game.rendered[i] = game.paint[i];
                continue;
            }
            const std::uint32_t outline =
                game.outline && i < game.outline->pixels.size()
                    ? game.outline->pixels[i]
                    : 0xffU;
            const std::uint32_t factor = (outline * game.light[i]) >> 8;
            game.rendered[i] = pack888(scaleRgb(expand565(game.paint[i]), factor));
        }
    }
    game.canvasDirty = true;
}

void ColorfillScene::stampColorfillBrush(int x, int y, std::uint16_t colour,
                                         SDL_Rect &changed) {
    // 0x004628b0. The loops bound rows by the width and columns by the
    // height, as the original does; every brush is square.
    auto &game = *colorfillGame;
    if (!game.brushImage) return;
    const auto &brush = *game.brushImage;
    const int left = x + (1 - brush.width / 2);
    const int top = y + (1 - brush.height / 2);
    for (int row = 0; row < brush.width; ++row) {
        const int py = top + row;
        if (py >= kSize) break;
        if (py < 0) continue;
        for (int column = 0; column < brush.height; ++column) {
            const int px = left + column;
            if (px >= kSize) break;
            if (px < 0) continue;
            if (game.outlineFlags[at(px, py)] == 0U &&
                brush.pixels[static_cast<std::size_t>(brush.width) * row + column] == 0U) {
                game.paint[at(px, py)] = colour;
            }
        }
    }
    // 0x004629da-0x00462a03: 0x006a4260 = {left, top, left+w, top+h}.
    changed = {left, top, left + brush.width, top + brush.height};
}

bool ColorfillScene::fillColorfill(int x, int y, std::uint16_t colour,
                                   SDL_Rect &changed) {
    // 0x00462a10: four-neighbour flood through pixels of exactly the clicked
    // colour that are not outline, breadth first with two queues
    // (0x006a4234/0x006a4248); 0x00462bf0 tests and paints one pixel.
    auto &game = *colorfillGame;
    int minX = kSize + 1, minY = kSize + 1, maxX = -1, maxY = -1;
    if (game.outlineFlags[at(x, y)] != 0U) return false;
    const std::uint16_t target = game.paint[at(x, y)];
    if (target == colour) return false;
    auto visit = [&](std::vector<SDL_Point> &queue, int px, int py) {
        if (px >= kSize || px < 0 || py >= kSize || py < 0) return;
        const std::size_t i = at(px, py);
        if (game.paint[i] != target || game.outlineFlags[i] != 0U) return;
        minX = std::min(minX, px);
        maxX = std::max(maxX, px);
        minY = std::min(minY, py);
        maxY = std::max(maxY, py);
        game.paint[i] = colour;
        queue.push_back({px, py});
    };
    std::vector<SDL_Point> current, next;
    visit(current, x, y);
    if (current.empty()) return false;
    while (!current.empty()) {
        next.clear();
        for (const SDL_Point p : current) {
            visit(next, p.x - 1, p.y);
            visit(next, p.x + 1, p.y);
            visit(next, p.x, p.y - 1);
            visit(next, p.x, p.y + 1);
        }
        std::swap(current, next);
    }
    // 0x00462bbd-0x00462bd2: right and bottom become exclusive.
    changed = {minX, minY, maxX + 1, maxY + 1};
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "fill"},
                         {"left", minX}, {"top", minY},
                         {"right", maxX + 1}, {"bottom", maxY + 1},
                         {"colour", static_cast<int>(game.colour)}});
    return true;
}

void ColorfillScene::showColorfillColour() {
    // 0x00462840: the swatch is filled with the colour.
    auto &game = *colorfillGame;
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "colour"},
                         {"colour", static_cast<int>(game.colour)},
                         {"x", game.cross.x}, {"y", game.cross.y},
                         {"tool", game.tool}});
}

void ColorfillScene::pickColorfillPalette(int x, int y) {
    // 0x00462870: the backdrop pixel under the pointer; the cross is
    // centred on it (16x16, offset -7).
    auto &game = *colorfillGame;
    if (sceneBackdrop && x >= 0 && y >= 0 && x < sceneBackdrop->w &&
        y < sceneBackdrop->h) {
        Uint8 r = 0, g = 0, b = 0;
        SDL_GetRGB(pixelValue(sceneBackdrop.get(), x, y), sceneBackdrop->format,
                   &r, &g, &b);
        game.colour = pack565(r, g, b);
    } else {
        game.colour = 0U;
    }
    game.cross = {x - 7, y - 7};
    showColorfillColour();
}

void ColorfillScene::colorfillCanvasDown(int x, int y) {
    // 0x004625d0, hotspot 1, coordinates already canvas-relative.
    auto &game = *colorfillGame;
    SDL_Rect changed{};
    switch (game.tool) {
    case Pen:
    case Eraser:
        game.undo = game.paint;
        game.lastX = x;
        game.lastY = y;
        stampColorfillBrush(x, y, game.tool == Pen ? game.colour : 0xffffU,
                            changed);
        renderColorfillCanvas(changed);
        break;
    case Bucket:
        game.undo = game.paint;
        if (fillColorfill(x, y, game.colour, changed)) {
            renderColorfillCanvas(changed);
        }
        break;
    case Picker:
        // 0x004627a1: the colour under the pointer on the paint surface;
        // the cross moves to where that colour sits on the palette.
        game.colour = game.paint[at(x, y)];
        game.cross = {game.paletteMap[game.colour].x - 7,
                      game.paletteMap[game.colour].y - 7};
        showColorfillColour();
        break;
    case Light:
        relightColorfill(x, y);
        renderColorfillCanvas(std::nullopt);
        break;
    default:
        break;
    }
}

void ColorfillScene::colorfillCanvasDrag(int hotspot, int x, int y) {
    // 0x00462dc0 (event 0x109, left button held).
    auto &game = *colorfillGame;
    x -= kCanvas.x;
    y -= kCanvas.y;
    if (hotspot != 1) {
        game.lastX = x;
        game.lastY = y;
        return;
    }
    if (game.tool != Pen && game.tool != Eraser) return;
    SDL_Rect total{kSize + 1, kSize + 1, -1, -1};
    auto unite = [&](const SDL_Rect &r) {
        total.x = std::min(total.x, r.x);
        total.y = std::min(total.y, r.y);
        total.w = std::max(total.w, r.w);
        total.h = std::max(total.h, r.h);
    };
    const std::uint16_t colour = game.tool == Pen ? game.colour : 0xffffU;
    const int dx = x - game.lastX;
    const int dy = y - game.lastY;
    const int distance =
        ftol(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
    int step = (game.brushImage ? game.brushImage->width : 0) / 8;
    if (step < 1) step = 1;
    SDL_Rect changed{};
    if (distance > step) {
        const int count = distance / step;
        const double total_ = static_cast<double>(static_cast<float>(count));
        for (int i = 0; i < count; ++i) {
            const double t = static_cast<double>(i) / total_;
            double py = static_cast<double>(dy) * t;
            py = py + static_cast<double>(game.lastY);
            double px = static_cast<double>(dx) * t;
            px = px + static_cast<double>(game.lastX);
            stampColorfillBrush(ftol(px), ftol(py), colour, changed);
            unite(changed);
        }
    }
    stampColorfillBrush(x, y, colour, changed);
    unite(changed);
    renderColorfillCanvas(total);
    game.lastX = x;
    game.lastY = y;
}

void ColorfillScene::setColorfillControlState(ColorfillControl &control,
                                              int state) {
    // 0x00406740 -> 0x00406760: a state change swaps the descriptor image,
    // removing the old one and adding the new one (0x00406810 picks it).
    control.state = state;
    const int slot = control.style == 1 ? (state == 1 && control.hover ? 1 : 0)
                                        : state;
    const std::string &image = control.images[static_cast<std::size_t>(slot)];
    if (control.shownImage == image) return;
    referenceTrace.emit("animation_remove",
                        {{"asset", control.shownImage}, {"reason", "removed"}});
    control.shownImage = image;
    referenceTrace.emit("animation_add",
                        {{"asset", image}, {"x", -1}, {"y", -1}, {"layer", 1},
                         {"hotspot", control.id}, {"conflict_mask", 0}});
}

void ColorfillScene::setColorfillScroll(int value) {
    // 0x00408eb0 -> 0x00408f30: the value is clamped, the tab placed, and
    // 0x117 reported when the value changed; 0x00461720 then scrolls the
    // list with 0x00407af0.
    auto &game = *colorfillGame;
    value = std::clamp(value, 0, game.scrollMaximum);
    if (value == game.scrollValue) return;
    game.scrollValue = value;
    game.brushTop = std::clamp(value, 0, game.scrollMaximum);
}

void ColorfillScene::selectColorfillBrush(int index) {
    // 0x00407e60 then 0x00462ca0 -> 0x00462cd0.
    auto &game = *colorfillGame;
    const int count = static_cast<int>(game.brushSections.size());
    if (count == 0) return;
    index = std::clamp(index, 0, count - 1);
    game.brushSelected = index;
    if (index < game.brushTop || index >= game.brushTop + game.brushVisible) {
        game.brushTop = std::clamp(
            index > game.brushTop ? index + 1 - game.brushVisible : index, 0,
            game.scrollMaximum);
    }
    loadColorfillBrush(game.brushSections[static_cast<std::size_t>(index)].name);
}

void ColorfillScene::colorfillControl(int id) {
    // 0x00461740.
    auto &game = *colorfillGame;
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "control"},
                         {"control", id}, {"tool", game.tool}});
    if (id >= 3 && id <= 7) {
        game.tool = id - 2;
    } else if (id == 0x14) {
        openColorfillChooser();
    } else if (id == 0x15) {
        game.hoverHotspot = 0U;
        game.pendingConfirmation = 0x15;
        showMessageWithTitle(language.get("colorfill", "CLEAR_TITLE", {}),
                             language.get("colorfill", "CLEAR_TEXT", {}),
                             MessageKind::ColorfillConfirmation);
    } else if (id == 0x16) {
        game.hoverHotspot = 0U;
        game.pendingConfirmation = 0x16;
        showMessageWithTitle(language.get("colorfill", "ACTIVITY_TITLE", {}),
                             language.get("colorfill", "ACTIVITY_TEXT", {}),
                             MessageKind::ColorfillConfirmation);
    } else if (id == 0x17) {
        // 0x00461940.
        referenceTrace.emit("minigame_state",
                            {{"game", "colorfill"}, {"state", "undo"}});
        game.paint = game.undo;
        renderColorfillCanvas(std::nullopt);
    } else if (id == kScrollUpId) {
        setColorfillScroll(game.scrollValue - 1);
    } else if (id == kScrollDownId) {
        setColorfillScroll(game.scrollValue + 1);
    }
}

void ColorfillScene::colorfillCornerButton(bool help, bool released, int x,
                                           int y) {
    if (activeScene != "colorfill" || !colorfillGame) return;
    auto &game = *colorfillGame;
    const int id = help ? 0x62 : 99;
    if (!released) {
        referenceTrace.emit("minigame_state",
                            {{"game", "colorfill"}, {"state", "mouse_down"},
                             {"hotspot", id}, {"x", x}, {"y", y},
                             {"tool", game.tool}});
        return;
    }
    // 0x00461760 (help, 0x0041e580) and 0x0046175b (exit, 0x0040cc30).
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "control"},
                         {"control", id}, {"tool", game.tool}});
    game.hoverHotspot = 0U;
}

void ColorfillScene::colorfillConfirmed(bool yes) {
    if (!colorfillGame) return;
    auto &game = *colorfillGame;
    const auto pending = game.pendingConfirmation;
    game.pendingConfirmation.reset();
    if (!yes || !pending) return;
    if (*pending == 0x15) {
        // 0x004617f8-0x00461840: undo copy, then 0x004618b0 keeps only the
        // outline (black) and whitens the rest.
        game.undo = game.paint;
        referenceTrace.emit("minigame_state",
                            {{"game", "colorfill"}, {"state", "clear"}});
        for (std::size_t i = 0; i < kPixels; ++i) {
            game.paint[i] = game.outlineFlags[i] ? 0U : 0xffffU;
        }
        renderColorfillCanvas(std::nullopt);
    } else if (*pending == 0x16) {
        // 0x00462220: a 490x490 copy of the lit canvas goes to the
        // Activity page in the 0x006a4250 parameter block {"colorfill", 0,
        // surface}.
        SurfacePtr picture(SDL_CreateRGBSurfaceWithFormat(
            0, kSize, kSize, 32, SDL_PIXELFORMAT_ARGB8888));
        if (picture) {
            for (int y = 0; y < kSize; ++y) {
                auto *row = reinterpret_cast<std::uint32_t *>(
                    static_cast<std::uint8_t *>(picture->pixels) + y * picture->pitch);
                for (int x = 0; x < kSize; ++x) {
                    const SDL_Color c = displayColour(game.rendered[at(x, y)]);
                    row[x] = 0xff000000U | (static_cast<std::uint32_t>(c.r) << 16) |
                             (static_cast<std::uint32_t>(c.g) << 8) | c.b;
                }
            }
        }
        pendingActivityEntry = ActivityEntryParameter{"colorfill", 0, std::move(picture)};
        enterScene("activity", "activity");
    }
}

void ColorfillScene::openColorfillChooser() {
    // 0x00461970 builds one 238x68 row per canvas.ini section: a green-keyed
    // surface (0x00461a0c-0x00461a27), the 64x64 preview copied to (2,2)
    // (0x00461a62) and the localized section name in (72,2)-(238,66) with
    // Amerigo BT_14_ and flags 1 (0x00461a91), then hands them to the shared
    // list dialog 0x00420750 titled colorfill:CANVAS_DIALOG, with Cancel
    // (flags 2). The row id is the canvas.ini index (0x00461b14).
    auto &game = *colorfillGame;
    auto &stage = game.chooserStage;
    if (!stage.amerigo14.texture) {
        const auto font = [&](const char *name) {
            return loadBitmapFont(
                renderer, common.data(common.find(std::string("fonts\\") + name)));
        };
        stage.amerigo10 = font("Amerigo BT_10_.bmp");
        stage.amerigo14 = font("Amerigo BT_14_.bmp");
    }
    std::vector<PuzzleChooserRow> rows;
    SDL_Texture *previousTarget = SDL_GetRenderTarget(renderer);
    for (std::size_t index = 0; index < game.canvasSections.size(); ++index) {
        const auto &section = game.canvasSections[index];
        std::vector<SDL_Color> pixels(238 * 68, SDL_Color{0, 0, 0, 0});
        const std::string name = iniValue(section, "preview", "blank");
        if (const auto *entry =
                scenePackage->findOptional("bitmap\\" + name + ".bmp")) {
            if (const auto decoded = decodeBitmap(scenePackage->data(*entry))) {
                for (int y = 0; y < decoded->height && y + 2 < 68; ++y) {
                    for (int x = 0; x < decoded->width && x + 2 < 238; ++x) {
                        const std::size_t i =
                            static_cast<std::size_t>(y) * decoded->width + x;
                        std::uint16_t colour = 0;
                        if (decoded->bitsPerPixel == 8) {
                            const auto rgb = decoded->pixels[i] < decoded->palette.size()
                                                 ? decoded->palette[decoded->pixels[i]]
                                                 : std::array<std::uint8_t, 3>{};
                            colour = pack565(rgb[0], rgb[1], rgb[2]);
                        } else {
                            colour = pack565(decoded->pixels[i * 3],
                                             decoded->pixels[i * 3 + 1],
                                             decoded->pixels[i * 3 + 2]);
                        }
                        pixels[static_cast<std::size_t>(y + 2) * 238 + x + 2] =
                            displayColour(colour);
                    }
                }
            }
        }
        TexturePtr preview = textureFromRgb(renderer, 238, 68, pixels);
        TexturePtr row(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_TARGET, 238, 68));
        SDL_SetTextureBlendMode(row.get(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(renderer, row.get());
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, preview.get(), nullptr, nullptr);
        // Flags 1: left-aligned, centred vertically in (72,2)-(238,66).
        const std::string label = language.get(
            "colorfill", section.name.substr(std::string("colorfill:").size()),
            {});
        drawText(renderer, stage.amerigo14, label, 72,
                 2 + (64 - stage.amerigo14.height) / 2);
        rows.push_back({static_cast<int>(index), std::move(row)});
    }
    SDL_SetRenderTarget(renderer, previousTarget);
    openPuzzleListDialog(stage, language.get("colorfill", "CANVAS_DIALOG", {}),
                         std::move(rows), true);
    game.hoverHotspot = 0U;
}

std::uint32_t ColorfillScene::colorfillHotspotId(int x, int y) const {
    // 0x004136c0 for this scene: the canvas descriptor (rectangle mode,
    // hotspot 1) above the scene's hotspots.bmp (palette = 2).
    const SDL_Point point{x, y};
    const SDL_Rect canvas{kCanvas.x, kCanvas.y, kSize, kSize};
    if (SDL_PointInRect(&point, &canvas)) return 1U;
    if (sceneHotspots && x >= 0 && y >= 0 && x < sceneHotspots->w &&
        y < sceneHotspots->h) {
        return pixelValue(sceneHotspots.get(), x, y);
    }
    return 0U;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
ColorfillScene::colorfillHotspotAt(int x, int y) const {
    if (activeScene != "colorfill" || !colorfillGame) return std::nullopt;
    const SDL_Point point{x, y};
    for (const auto &control : colorfillGame->controls) {
        const auto &size = control.sizes[0];
        const SDL_Rect bounds{control.x, control.y, size.x, size.y};
        if (SDL_PointInRect(&point, &bounds)) {
            return std::pair{static_cast<std::uint32_t>(control.id), bounds};
        }
    }
    const SDL_Rect canvas{kCanvas.x, kCanvas.y, kSize, kSize};
    if (SDL_PointInRect(&point, &canvas)) return std::pair{1U, canvas};
    return std::nullopt;
}

bool ColorfillScene::colorfillHidesCursor() const {
    // 0x004625b0: cursor 3 (none) over the canvas, else the hotspot cursor.
    return activeScene == "colorfill" && colorfillGame &&
           dialogMode == DialogMode::None &&
           !colorfillGame->chooserStage.chooser.open &&
           colorfillGame->hoverHotspot == 1U;
}

bool ColorfillScene::beginColorfillInteraction(int x, int y) {
    if (!colorfillGame) return false;
    auto &game = *colorfillGame;
    if (game.chooserStage.chooser.open) {
        puzzleChooserPress(game.chooserStage, x, y);
        return true;
    }
    game.leftHeld = true;
    // 0x00409bc0 removes a shown tip; it reappears only for another id.
    if (game.tipShown >= 0) {
        game.tipShown = -2;
        game.tipTexture.reset();
    }
    const SDL_Point point{x, y};
    // The scene's 0x004625d0 runs first, with the hotspot under the
    // pointer: a control's id, the canvas (1) or the palette (2).
    std::uint32_t hotspot = colorfillHotspotId(x, y);
    if (const auto control = colorfillHotspotAt(x, y);
        control && control->first != 1U) {
        hotspot = control->first;
    }
    referenceTrace.emit("minigame_state",
                        {{"game", "colorfill"}, {"state", "mouse_down"},
                         {"hotspot", static_cast<int>(hotspot)},
                         {"x", x}, {"y", y}, {"tool", game.tool}});
    // Panel controls (0x00406b50): radio buttons report on press.
    for (std::size_t i = 0; i < game.controls.size(); ++i) {
        auto &control = game.controls[i];
        const SDL_Rect bounds{control.x, control.y, control.sizes[0].x,
                              control.sizes[0].y};
        if (!SDL_PointInRect(&point, &bounds)) continue;
        control.hover = true;
        if (control.style == 2) {
            if (control.state != 0) return true;
            for (auto &other : game.controls) {
                if (other.style == 2 && other.group == control.group &&
                    &other != &control) {
                    setColorfillControlState(other, 0);
                }
            }
            setColorfillControlState(control, 1);
            colorfillControl(control.id);
        } else if (control.state == 0) {
            setColorfillControlState(control, 1);
            game.pressedControl = i;
        }
        return true;
    }
    // 0x004093f0 -> 0x004092d0: the track widened by the tab's width
    // (x - w/2 .. x + width + w/2), or the tab itself.
    const SDL_Rect track{kScrollX - game.scrollTabSize.x / 2, kScrollY,
                         kScrollWidth + game.scrollTabSize.x, kScrollHeight};
    const int tabY = game.scrollMaximum > 0
                         ? kScrollY + kScrollHeight * game.scrollValue / game.scrollMaximum
                         : kScrollY;
    const SDL_Rect tab{kScrollX + kScrollWidth / 2 - game.scrollTabSize.x / 2,
                       tabY - game.scrollTabSize.y / 2, game.scrollTabSize.x,
                       game.scrollTabSize.y};
    if (SDL_PointInRect(&point, &track) || SDL_PointInRect(&point, &tab)) {
        game.scrollDragging = true;
        const int clamped = std::clamp(y, kScrollY, kScrollY + kScrollHeight);
        const double value = static_cast<double>((clamped - kScrollY) *
                                                 game.scrollMaximum) /
                             static_cast<double>(kScrollHeight);
        setColorfillScroll(ftol(value < 0.0 ? value - 0.5 : value + 0.5));
    }
    // 0x00407ec0: a brush row.
    if (SDL_PointInRect(&point, &kBrushList)) {
        const int index = (y - kBrushList.y) / 36 + game.brushTop;
        if (index >= 0 && index < static_cast<int>(game.brushSections.size())) {
            selectColorfillBrush(index);
        }
    }
    if (hotspot == 1U) {
        colorfillCanvasDown(x - kCanvas.x, y - kCanvas.y);
    } else if (hotspot == 2U) {
        pickColorfillPalette(x, y);
    }
    return true;
}

void ColorfillScene::moveColorfillPointer(int x, int y, bool leftHeld) {
    if (!colorfillGame) return;
    auto &game = *colorfillGame;
    if (game.chooserStage.chooser.open) {
        puzzleChooserMove(game.chooserStage, x, y);
        return;
    }
    game.hoverHotspot = colorfillHotspotId(x, y);
    const SDL_Point point{x, y};
    for (auto &control : game.controls) {
        const SDL_Rect bounds{control.x, control.y, control.sizes[0].x,
                              control.sizes[0].y};
        const bool hover = SDL_PointInRect(&point, &bounds);
        if (hover != control.hover) {
            control.hover = hover;
            if (control.style == 1) setColorfillControlState(control, control.state);
        }
    }
    if (!leftHeld) return;
    if (game.scrollDragging) {
        // 0x00409270: the tab follows the pointer.
        const int clamped = std::clamp(y, kScrollY, kScrollY + kScrollHeight);
        const double value = static_cast<double>((clamped - kScrollY) *
                                                 game.scrollMaximum) /
                             static_cast<double>(kScrollHeight);
        setColorfillScroll(ftol(value < 0.0 ? value - 0.5 : value + 0.5));
        return;
    }
    colorfillCanvasDrag(static_cast<int>(colorfillHotspotId(x, y)), x, y);
}

void ColorfillScene::endColorfillInteraction(int x, int y) {
    if (!colorfillGame) return;
    auto &game = *colorfillGame;
    if (game.chooserStage.chooser.open) {
        puzzleChooserRelease(game.chooserStage, x, y);
        return;
    }
    game.leftHeld = false;
    game.scrollDragging = false;
    const auto pressed = game.pressedControl;
    game.pressedControl.reset();
    if (!pressed || *pressed >= game.controls.size()) return;
    auto &control = game.controls[*pressed];
    const SDL_Point point{x, y};
    const SDL_Rect bounds{control.x, control.y, control.sizes[0].x,
                          control.sizes[0].y};
    control.hover = SDL_PointInRect(&point, &bounds);
    // 0x00406d40: state back to 0; report only when still over it.
    if (control.state != 1) return;
    setColorfillControlState(control, 0);
    if (control.hover) colorfillControl(control.id);
}

bool ColorfillScene::colorfillKey(SDL_Keycode key) {
    // 0x00463990 -> 0x00407f60: page, end, home and arrow keys move the
    // brush list; the tab follows the top row.
    if (!colorfillGame) return false;
    auto &game = *colorfillGame;
    if (game.chooserStage.chooser.open) {
        puzzleChooserKey(game.chooserStage, key);
        return true;
    }
    const int count = static_cast<int>(game.brushSections.size());
    if (count <= 0) return false;
    int selected = game.brushSelected;
    switch (key) {
    case SDLK_PAGEUP:
        game.brushTop = std::clamp(game.brushTop - game.brushVisible, 0, game.scrollMaximum);
        break;
    case SDLK_PAGEDOWN:
        game.brushTop = std::clamp(game.brushTop + game.brushVisible, 0, game.scrollMaximum);
        break;
    case SDLK_END:
        selected = count - 1;
        break;
    case SDLK_HOME:
        selected = 0;
        break;
    case SDLK_UP:
        selected = std::max(0, selected - 1);
        break;
    case SDLK_DOWN:
        selected = std::min(count - 1, selected + 1);
        break;
    default:
        return false;
    }
    game.brushSelected = selected;
    if (selected < game.brushTop || selected >= game.brushTop + game.brushVisible) {
        game.brushTop = std::clamp(
            selected > game.brushTop ? selected + 1 - game.brushVisible : selected,
            0, game.scrollMaximum);
    }
    game.scrollValue = game.brushTop;
    loadColorfillBrush(game.brushSections[static_cast<std::size_t>(selected)].name);
    return true;
}

void ColorfillScene::updateColorfill(double elapsed) {
    if (!colorfillGame) return;
    auto &game = *colorfillGame;
    if (game.controls.empty()) {
        restoreColorfillState();
    }
    // 0x00420430 returns once OK or Cancel closed the dialog; OK with a
    // row loads that picture with the ini light (0x00461b2a-0x00461b3a).
    if (game.chooserStage.chooser.open && game.chooserStage.chooser.result != 0) {
        const int index = closePuzzleChooser(game.chooserStage);
        if (index >= 0 && index < static_cast<int>(game.canvasSections.size())) {
            loadColorfillPicture(
                game.canvasSections[static_cast<std::size_t>(index)].name, true);
        }
    }
    // 0x00409ed0: a tip appears after 1500 ms over the same registered id.
    int candidate = -1;
    const std::uint32_t id = colorfillHotspotId(mouseX, mouseY);
    if (const auto control = colorfillHotspotAt(mouseX, mouseY)) {
        candidate = static_cast<int>(control->first);
    } else if (id == 2U) {
        candidate = 2;
    }
    if (candidate != game.tipCandidate) {
        game.tipCandidate = candidate;
        game.tipCandidateSince = elapsed;
        game.tipShown = -1;
        game.tipTexture.reset();
    } else if (candidate >= 0 && game.tipShown == -1 &&
               elapsed - game.tipCandidateSince >= 1.5) {
        showColorfillTip(candidate);
    }
}

void ColorfillScene::showColorfillTip(int id) {
    // 0x00409fc0 -> 0x00409c00.
    auto &game = *colorfillGame;
    const char *key = nullptr;
    for (const auto &[tipId, tipKey] : kTips) {
        if (tipId == id) key = tipKey;
    }
    game.tipShown = id;
    game.tipText = key ? language.get("colorfill", key, {}) : std::string();
    if (game.tipText.empty() || !game.tipFrame || !game.tipFont.texture) {
        game.tipTexture.reset();
        return;
    }
    // 0x00409d5a-0x00409d72: text size + (12, 18), nine-sliced from
    // tooltip.bmp with the inner rectangle 0x0049c790 = (6,11)-(38,15).
    const int width = textWidth(game.tipFont, game.tipText) + 12;
    const int height = game.tipFont.height + 18;
    SurfacePtr bubble(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                     SDL_PIXELFORMAT_ARGB8888));
    if (!bubble) return;
    SDL_FillRect(bubble.get(), nullptr, 0U);
    SDL_Surface *frame = game.tipFrame.get();
    const int xs[4] = {0, 6, 38, frame->w};
    const int ys[4] = {0, 11, 15, frame->h};
    const int xd[4] = {0, 6, width - (frame->w - 38), width};
    const int yd[4] = {0, 11, height - (frame->h - 15), height};
    SDL_SetSurfaceBlendMode(frame, SDL_BLENDMODE_NONE);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            SDL_Rect source{xs[column], ys[row], xs[column + 1] - xs[column],
                            ys[row + 1] - ys[row]};
            SDL_Rect target{xd[column], yd[row], xd[column + 1] - xd[column],
                            yd[row + 1] - yd[row]};
            if (source.w <= 0 || source.h <= 0 || target.w <= 0 || target.h <= 0)
                continue;
            SDL_BlitScaled(frame, &source, bubble.get(), &target);
        }
    }
    // 0x00409c00: below the cursor, or above it when that leaves the
    // screen; pushed left to fit.
    const auto &cursor = cursors[static_cast<std::size_t>(activeCursor)];
    const int cursorHeight = cursor.media.videoFrames.empty()
                                 ? 32
                                 : cursor.media.videoFrames.front().height;
    int x = mouseX;
    int y = mouseY + cursorHeight;
    if (kLogicalWidth < x + width) x = kLogicalWidth - width;
    if (kLogicalHeight < y + height) y = mouseY - height;
    // 0x00409dd0: the dot at the pointer, clamped into the bubble.
    if (game.tipDot) {
        int dx = mouseX, dy = mouseY;
        if (dx >= x) dx = std::min(dx, x + width - game.tipDot->w);
        else dx = x;
        if (dy >= y) dy = std::min(dy, y + height - game.tipDot->h);
        else dy = y;
        SDL_Rect target{dx - x, dy - y, game.tipDot->w, game.tipDot->h};
        SDL_BlitSurface(game.tipDot.get(), nullptr, bubble.get(), &target);
    }
    game.tipBounds = {x, y, width, height};
    game.tipTexture = makeTexture(renderer, bubble.get());
    SDL_SetTextureBlendMode(game.tipTexture.get(), SDL_BLENDMODE_BLEND);
}

void ColorfillScene::renderColorfill(double elapsed) {
    (void)elapsed;
    if (!colorfillGame || !colorfillGame->canvasTexture) return;
    auto &game = *colorfillGame;

    // 0x004639d0: the tool cursor and, for the light tool, the sun are
    // inverted into the frame inside the canvas rectangle.
    // Modal dialogs run their own loop, so 0x119 does not reach the scene
    // and neither overlay is drawn while one is open (seen in captures).
    const bool modal =
        dialogMode != DialogMode::None || game.chooserStage.chooser.open;
    std::optional<SDL_Point> cursor;
    if (!modal && game.hoverHotspot == 1U) {
        cursor = SDL_Point{mouseX, mouseY};
    }
    std::optional<SDL_Point> sun;
    if (!modal && game.tool == Light &&
        (mouseX != game.lightX + kCanvas.x || mouseY != game.lightY + kCanvas.y)) {
        sun = SDL_Point{game.lightX + kCanvas.x, game.lightY + kCanvas.y};
    }
    const auto sameAs = [](const std::optional<SDL_Point> &a,
                           const std::optional<SDL_Point> &b) {
        return a.has_value() == b.has_value() &&
               (!a || (a->x == b->x && a->y == b->y));
    };
    if (!sameAs(cursor, game.drawnCursor) || !sameAs(sun, game.drawnSun) ||
        game.drawnCursorTool != game.tool || game.drawnCursorBrush != game.brush) {
        game.canvasDirty = true;
    }
    if (game.canvasDirty) {
        std::vector<std::uint16_t> frame = game.rendered;
        // 0x00463a60: inverts the pixels under the image's zero pixels,
        // centred like a brush stamp, clipped to the canvas.
        auto invert = [&](const std::optional<ColorfillIndexed> &image,
                          SDL_Point at_) {
            if (!image) return;
            const int left = at_.x + (1 - image->width / 2) - kCanvas.x;
            const int top = at_.y + (1 - image->height / 2) - kCanvas.y;
            for (int row = 0; row < image->width; ++row) {
                const int py = top + row;
                if (py >= kSize) break;
                if (py < 0) continue;
                for (int column = 0; column < image->height; ++column) {
                    const int px = left + column;
                    if (px >= kSize) break;
                    if (px < 0) continue;
                    if (image->pixels[static_cast<std::size_t>(image->width) * row + column] == 0U) {
                        frame[at(px, py)] = static_cast<std::uint16_t>(~frame[at(px, py)]);
                    }
                }
            }
        };
        if (sun) invert(game.sunCursor, *sun);
        if (cursor) {
            switch (game.tool) {
            case Bucket: invert(game.bucketCursor, *cursor); break;
            case Picker: invert(game.pickerCursor, *cursor); break;
            case Light: invert(game.sunCursor, *cursor); break;
            default: invert(game.brushOutline, *cursor); break;
            }
        }
        void *pixels = nullptr;
        int pitch = 0;
        if (SDL_LockTexture(game.canvasTexture.get(), nullptr, &pixels, &pitch) == 0) {
            for (int y = 0; y < kSize; ++y) {
                auto *row = reinterpret_cast<std::uint32_t *>(
                    static_cast<std::uint8_t *>(pixels) + y * pitch);
                for (int x = 0; x < kSize; ++x) {
                    const SDL_Color c = displayColour(frame[at(x, y)]);
                    row[x] = 0xff000000U | (static_cast<std::uint32_t>(c.r) << 16) |
                             (static_cast<std::uint32_t>(c.g) << 8) | c.b;
                }
            }
            SDL_UnlockTexture(game.canvasTexture.get());
        }
        game.canvasDirty = false;
        game.drawnCursor = cursor;
        game.drawnSun = sun;
        game.drawnCursorTool = game.tool;
        game.drawnCursorBrush = game.brush;
    }
    const SDL_Rect canvas{kCanvas.x, kCanvas.y, kSize, kSize};
    SDL_RenderCopy(renderer, game.canvasTexture.get(), nullptr, &canvas);

    const SDL_Color swatch = displayColour(game.colour);
    SDL_SetRenderDrawColor(renderer, swatch.r, swatch.g, swatch.b, 255);
    SDL_RenderFillRect(renderer, &kSwatch);
    if (game.crossTexture) {
        const SDL_Rect cross{game.cross.x, game.cross.y, 16, 16};
        SDL_RenderCopy(renderer, game.crossTexture.get(), nullptr, &cross);
    }

    // Title text box 0x006a4088: the localized scene name, Amerigo BT_18_,
    // centred (0x101).
    drawCenteredText(renderer, menuTitleFont,
                     language.get("colorfill", "SCENENAME", {}), kTitle);

    for (const auto &control : game.controls) {
        const int slot = control.style == 1
                             ? (control.state == 1 && control.hover ? 1 : 0)
                             : control.state;
        const auto &texture = control.textures[static_cast<std::size_t>(slot)];
        if (!texture) continue;
        const SDL_Point size = control.sizes[static_cast<std::size_t>(slot)];
        const SDL_Rect bounds{control.x, control.y, size.x, size.y};
        SDL_RenderCopy(renderer, texture.get(), nullptr, &bounds);
        if (!control.caption.empty()) {
            // Captions use Amerigo BT_14_ (0x004068a0(0x006a4380)).
            drawCenteredText(renderer, calendarFont, control.caption, bounds);
        }
    }

    // Brush list: highlight behind the selected row, then the rows.
    if (game.brushSelected >= game.brushTop &&
        game.brushSelected < game.brushTop + game.brushVisible) {
        const SDL_Rect highlight{kBrushList.x,
                                 kBrushList.y + (game.brushSelected - game.brushTop) * 36,
                                 kBrushList.w, 36};
        SDL_SetRenderDrawColor(renderer, kHighlight.r, kHighlight.g, kHighlight.b, 255);
        SDL_RenderFillRect(renderer, &highlight);
    }
    for (int row = 0; row < game.brushVisible; ++row) {
        const int index = game.brushTop + row;
        if (index >= static_cast<int>(game.brushItems.size())) break;
        const SDL_Rect bounds{kBrushList.x, kBrushList.y + row * 36, 36, 36};
        SDL_RenderCopy(renderer, game.brushItems[static_cast<std::size_t>(index)].get(),
                       nullptr, &bounds);
    }
    if (game.scrollTab && game.scrollMaximum > 0) {
        // 0x00408f30 centres the tab on (x + w/2, y + h*value/range).
        const int ty = kScrollY + kScrollHeight * game.scrollValue / game.scrollMaximum;
        const SDL_Rect tab{kScrollX + kScrollWidth / 2 - game.scrollTabSize.x / 2,
                           ty - game.scrollTabSize.y / 2, game.scrollTabSize.x,
                           game.scrollTabSize.y};
        SDL_RenderCopy(renderer, game.scrollTab.get(), nullptr, &tab);
    }

}

bool ColorfillScene::colorfillChooserOpen() const {
    return activeScene == "colorfill" && colorfillGame &&
           colorfillGame->chooserStage.chooser.open;
}

void ColorfillScene::renderColorfillOverlay() {
    // Drawn after the modal mesh: the list dialog (layers 99-102) and the
    // tooltip (layer 1500, 0x00409c00) sit above every scene layer.
    if (!colorfillGame) return;
    auto &game = *colorfillGame;
    if (game.chooserStage.chooser.open) {
        drawPuzzleStage(game.chooserStage);
        return;
    }
    if (game.tipShown < 0 || !game.tipTexture) return;
    SDL_RenderCopy(renderer, game.tipTexture.get(), nullptr, &game.tipBounds);
    drawCenteredText(renderer, game.tipFont, game.tipText, game.tipBounds);
}

void ColorfillScene::endColorfillScene() {
    // Leaving the scene removes all of its descriptors. Order from a capture
    // of the original (2026-10-18): scrollbar tab and arrows (down, up), the
    // tools, the four buttons with their captions, Help and Exit, the brush
    // rows, then canvas, cross, the title box and color.
    if (!colorfillGame || colorfillGame->controls.empty()) return;
    auto &game = *colorfillGame;
    auto remove = [&](const std::string &asset) {
        referenceTrace.emit("animation_remove",
                            {{"asset", asset}, {"reason", "removed"}});
    };
    for (const int id : {kScrollDownId, kScrollUpId, 3, 4, 5, 6, 7, 0x14, 0x15,
                         0x16, 0x17}) {
        for (const auto &control : game.controls) {
            if (control.id == id) remove(control.shownImage);
        }
    }
    remove("canvas");
    remove("cross");
    remove("color");
}

void ColorfillScene::saveColorfillState() {
    // 0x00463cb0, from the scene's exit 0x00463c10.
    if (colorfillGame) colorfillLastTool = colorfillGame->tool;
    if (!colorfillGame || colorfillGame->controls.empty() || profiles.empty() ||
        profiles[profileIndex] == "No profile") {
        return;
    }
    const auto &game = *colorfillGame;
    const fs::path profilePath = profileDirectory / (profiles[profileIndex] + ".pfl");
    if (!fs::exists(profilePath)) return;
    // Blue, green and red planes of the paint surface (0x00462500 bytes).
    std::vector<std::uint8_t> planes(kPixels * 3, 0U);
    for (std::size_t i = 0; i < kPixels; ++i) {
        const std::uint32_t rgb = expand565(game.paint[i]);
        planes[i] = static_cast<std::uint8_t>(rgb & 0xffU);
        planes[i + kPixels] = static_cast<std::uint8_t>((rgb >> 8) & 0xffU);
        planes[i + kPixels * 2] = static_cast<std::uint8_t>((rgb >> 16) & 0xffU);
    }
    // 0x00463e90: runs of up to 127 equal bytes become (0x80|n, byte);
    // otherwise literal runs of up to 127 bytes that stop before a pair.
    std::vector<std::uint8_t> packed;
    const int size = static_cast<int>(planes.size());
    int in = 0;
    while (in < size) {
        if (in == size - 1) {
            packed.push_back(1U);
            packed.push_back(planes[static_cast<std::size_t>(in)]);
            ++in;
            continue;
        }
        int count = 0;
        const std::uint8_t first = planes[static_cast<std::size_t>(in)];
        if (first == planes[static_cast<std::size_t>(in) + 1]) {
            do {
                ++count;
                if (count > 0x7e || count + in >= size) break;
            } while (planes[static_cast<std::size_t>(in + count)] == first);
            packed.push_back(static_cast<std::uint8_t>(count | 0x80));
            packed.push_back(first);
        } else {
            std::uint8_t previous = 0;
            do {
                previous = planes[static_cast<std::size_t>(in + count)];
                ++count;
                if (count > 0x7e || count + in >= size) break;
            } while (planes[static_cast<std::size_t>(in + count)] != previous);
            packed.push_back(static_cast<std::uint8_t>(count));
            packed.insert(packed.end(), planes.begin() + in,
                          planes.begin() + in + count);
        }
        in += count;
    }
    std::vector<std::uint8_t> header(0x218U, 0U);
    writeLe32(header, 0, static_cast<std::uint32_t>(game.lightX));
    writeLe32(header, 4, static_cast<std::uint32_t>(game.lightY));
    writeLe32(header, 8, static_cast<std::uint32_t>(game.cross.x + 7));
    writeLe32(header, 12, static_cast<std::uint32_t>(game.cross.y + 7));
    std::memcpy(header.data() + 16, game.picture.data(),
                std::min<std::size_t>(game.picture.size(), 255));
    std::memcpy(header.data() + 16 + 256, game.brush.data(),
                std::min<std::size_t>(game.brush.size(), 255));
    writeLe32(header, 0x210, static_cast<std::uint32_t>(packed.size()));
    try {
        setProfileSceneRecordBytes(profilePath, "colorfill", 0x402U, 0xdeadU, header);
        setProfileSceneRecordBytes(profilePath, "colorfill", 0x402U, 0xbabeU, packed);
    } catch (const std::exception &error) {
        std::cerr << "Could not save Color Fill: " << error.what() << '\n';
    }
}

bool ColorfillScene::executeColorfillDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "colorfill:";
    if (activeScene != "colorfill" || !colorfillGame ||
        colorfillGame->controls.empty()) {
        return false;
    }
    auto &game = *colorfillGame;
    const std::string control(command.substr(prefix.size()));
    auto numbers = [&](std::string_view text) {
        std::vector<int> values;
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(',', start);
            values.push_back(std::stoi(std::string(
                text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                 : end - start))));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return values;
    };
    if (control.rfind("click:", 0) == 0) {
        const auto v = numbers(std::string_view(control).substr(6));
        if (v.size() != 2) return false;
        mouseX = v[0];
        mouseY = v[1];
        moveColorfillPointer(v[0], v[1], false);
        beginColorfillInteraction(v[0], v[1]);
        endColorfillInteraction(v[0], v[1]);
    } else if (control.rfind("drag:", 0) == 0) {
        // drag:x0,y0,x1,y1[,steps]
        const auto v = numbers(std::string_view(control).substr(5));
        if (v.size() < 4) return false;
        const int steps = v.size() > 4 ? std::max(1, v[4]) : 10;
        mouseX = v[0];
        mouseY = v[1];
        beginColorfillInteraction(v[0], v[1]);
        for (int i = 1; i <= steps; ++i) {
            const int x = v[0] + (v[2] - v[0]) * i / steps;
            const int y = v[1] + (v[3] - v[1]) * i / steps;
            mouseX = x;
            mouseY = y;
            moveColorfillPointer(x, y, true);
        }
        endColorfillInteraction(v[2], v[3]);
    } else if (control.rfind("move:", 0) == 0) {
        const auto v = numbers(std::string_view(control).substr(5));
        if (v.size() != 2) return false;
        mouseX = v[0];
        mouseY = v[1];
        moveColorfillPointer(v[0], v[1], false);
    } else if (control.rfind("picture:", 0) == 0) {
        // Stand-in for the chooser: picture:<index into canvas.ini>.
        const int index = std::stoi(control.substr(8));
        if (index < 0 || index >= static_cast<int>(game.canvasSections.size()))
            return false;
        openColorfillChooser();
        game.chooserStage.chooser.selected = index;
        game.chooserStage.chooser.result = 1;
    } else if (control == "confirm" || control == "decline") {
        if (dialogMode != DialogMode::Message) return false;
        dialogMode = DialogMode::None;
        colorfillConfirmed(control == "confirm");
    } else if (control.rfind("key:", 0) == 0) {
        colorfillKey(static_cast<SDL_Keycode>(std::stoi(control.substr(4))));
    } else if (control == "open-chooser") {
        colorfillControl(0x14);
    } else if (control.rfind("chooser-click:", 0) == 0) {
        const auto v = numbers(std::string_view(control).substr(14));
        if (v.size() != 2 || !game.chooserStage.chooser.open) return false;
        puzzleChooserPress(game.chooserStage, v[0], v[1]);
        puzzleChooserRelease(game.chooserStage, v[0], v[1]);
    } else if (control.rfind("screenshot:", 0) == 0) {
        // screenshot:<path> writes the last presented frame at the
        // renderer's output size.
        int width = 0, height = 0;
        SDL_GetRendererOutputSize(renderer, &width, &height);
        SDL_Rect viewport{};
        SDL_RenderGetViewport(renderer, &viewport);
        SDL_RenderSetViewport(renderer, nullptr);
        SDL_RenderSetLogicalSize(renderer, 0, 0);
        SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        SDL_RenderSetLogicalSize(renderer, kLogicalWidth, kLogicalHeight);
        if (!surface ||
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                 surface->pixels, surface->pitch) != 0) {
            return false;
        }
        SDL_SaveBMP(surface.get(), control.substr(11).c_str());
    } else if (control.rfind("dump:", 0) == 0) {
        // dump:<path> writes the composited canvas as a 24-bit BMP.
        SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
            0, kSize, kSize, 24, SDL_PIXELFORMAT_RGB24));
        if (!surface) return false;
        for (int y = 0; y < kSize; ++y) {
            auto *row = static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch;
            for (int x = 0; x < kSize; ++x) {
                const SDL_Color c = displayColour(game.rendered[at(x, y)]);
                row[x * 3] = c.r;
                row[x * 3 + 1] = c.g;
                row[x * 3 + 2] = c.b;
            }
        }
        SDL_SaveBMP(surface.get(), control.substr(5).c_str());
    } else {
        return false;
    }
    return true;
}

} // namespace fgp
