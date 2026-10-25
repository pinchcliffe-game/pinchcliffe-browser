#include "game/puzzle_image.h"

#include <cmath>
#include <stdexcept>

namespace fgp {
namespace {

// 0x0049ff58: the light's elevation, stored as a double slightly short of
// pi/3. 0x0049ff50 is 1/255 and 0x0049dac4 is 255 (both float).
constexpr double kLightAngle = 1.0471975511666667;
constexpr float kHeightScale = 1.0F / 255.0F;

} // namespace

std::uint32_t rgb565To888(std::uint16_t pixel) {
    const std::uint32_t red = (pixel >> 11U) & 0x1fU;
    const std::uint32_t green = (pixel >> 5U) & 0x3fU;
    const std::uint32_t blue = pixel & 0x1fU;
    return (red << 19U) | (green << 10U) | (blue << 3U);
}

std::uint16_t rgb888To565(std::uint32_t rgb) {
    return static_cast<std::uint16_t>(((rgb >> 19U) & 0x1fU) << 11U |
                                      ((rgb >> 10U) & 0x3fU) << 5U |
                                      ((rgb >> 3U) & 0x1fU));
}

std::uint32_t shadeRgb888(std::uint32_t rgb, std::uint8_t value) {
    std::uint32_t red = (rgb >> 16U) & 0xffU;
    std::uint32_t green = (rgb >> 8U) & 0xffU;
    std::uint32_t blue = rgb & 0xffU;
    if (value < 0x80U) {
        red = value * red / 0x7fU;
        green = value * green / 0x7fU;
        blue = value * blue / 0x7fU;
    } else if (value > 0x80U) {
        const std::uint32_t lift = static_cast<std::uint8_t>(value + 0x80U);
        red += (0xffU - red) * lift / 0x7fU;
        green += (0xffU - green) * lift / 0x7fU;
        blue += (0xffU - blue) * lift / 0x7fU;
    }
    return (red << 16U) | (green << 8U) | blue;
}

Rgb565Image rgb565FromSurface(SDL_Surface *surface) {
    if (!surface) throw std::runtime_error("Missing puzzle picture");
    Rgb565Image image;
    image.width = surface->w;
    image.height = surface->h;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
    SDL_LockSurface(surface);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto *row = static_cast<const std::uint8_t *>(surface->pixels) +
                              static_cast<std::ptrdiff_t>(y) * surface->pitch;
            const int bytes = surface->format->BytesPerPixel;
            std::uint32_t value = 0;
            const auto *pixel = row + static_cast<std::ptrdiff_t>(x) * bytes;
            for (int byte = 0; byte < bytes; ++byte) {
                value |= static_cast<std::uint32_t>(pixel[byte]) << (8 * byte);
            }
            std::uint8_t red = 0;
            std::uint8_t green = 0;
            std::uint8_t blue = 0;
            SDL_GetRGB(value, surface->format, &red, &green, &blue);
            image.at(x, y) = static_cast<std::uint16_t>(
                ((red >> 3U) << 11U) | ((green >> 2U) << 5U) | (blue >> 3U));
        }
    }
    SDL_UnlockSurface(surface);
    return image;
}

void blurHeightField(HeightField &destination, const HeightField &source) {
    destination.resize(kHeightFieldSize);
    for (int row = 0; row < kHeightFieldStride; ++row) {
        for (int column = 0; column < kHeightFieldStride; ++column) {
            const std::size_t index =
                static_cast<std::size_t>(row) * kHeightFieldStride + column;
            unsigned sum = source[index];
            unsigned count = 1;
            if (row > 0) {
                sum += source[index - kHeightFieldStride];
                ++count;
            }
            if (column > 0) {
                sum += source[index - 1];
                ++count;
            }
            if (row < kHeightFieldStride - 1) {
                sum += source[index + kHeightFieldStride];
                ++count;
            }
            if (column < kHeightFieldStride - 1) {
                sum += source[index + 1];
                ++count;
            }
            destination[index] = static_cast<std::uint8_t>(sum / count);
        }
    }
}

void bevelImage(Rgb565Image &image, const HeightField &height, int direction) {
    // 0x0046bd47-0x0046bdee: the light vector per direction. `alongY`
    // multiplies the vertical gradient, `alongX` the horizontal one.
    const float lightZ = static_cast<float>(std::cos(kLightAngle));
    const float sine = static_cast<float>(std::sin(kLightAngle));
    float alongY = 0.0F;
    float alongX = 0.0F;
    switch (direction) {
    case 0: alongX = sine; break;
    case 1: alongY = -sine; break;
    case 2: alongX = -sine; break;
    case 3: alongY = sine; break;
    case 4: alongY = sine; alongX = sine; break;
    default: break;
    }
    for (int y = 0; y < image.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * kHeightFieldStride;
        for (int x = 0; x < image.width; ++x) {
            const int here = height[row + x];
            // 0x0046be44-0x0046be74: three times the forward differences,
            // scaled by 1/255.
            const double dx = static_cast<double>((height[row + x + 1] - here) * 3) *
                              kHeightScale;
            const double dy = static_cast<double>(
                                  (height[row + x + kHeightFieldStride] - here) * 3) *
                              kHeightScale;
            double light = std::sqrt(1.0 - (dx * dx + dy * dy)) * lightZ +
                           dy * alongY + dx * alongX;
            // 0x0046bea0-0x0046bec6: an unordered (NaN) or negative value
            // takes 0, anything above 1 takes 1.
            if (!(light >= 0.0)) {
                light = 0.0;
            } else if (light > 1.0) {
                light = 1.0;
            }
            const auto value = static_cast<std::uint8_t>(
                static_cast<int>(light * 255.0));
            auto &pixel = image.at(x, y);
            pixel = rgb888To565(shadeRgb888(rgb565To888(pixel), value));
        }
    }
}

Rgb565Image rotateImage(const Rgb565Image &image, int quarterTurns) {
    Rgb565Image rotated;
    const bool swap = (quarterTurns & 1) != 0;
    rotated.width = swap ? image.height : image.width;
    rotated.height = swap ? image.width : image.height;
    rotated.pixels.resize(image.pixels.size());
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint16_t value = image.at(x, y);
            switch (quarterTurns & 3) {
            case 0: rotated.at(x, y) = value; break;
            case 1: rotated.at(rotated.width - 1 - y, x) = value; break;
            case 2:
                rotated.at(rotated.width - 1 - x, rotated.height - 1 - y) = value;
                break;
            case 3: rotated.at(y, rotated.height - 1 - x) = value; break;
            }
        }
    }
    return rotated;
}

SurfacePtr surfaceFromRgb565(const Rgb565Image &image, bool keyed) {
    SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
        0, image.width, image.height, 32, SDL_PIXELFORMAT_RGBA32));
    if (!surface) throw std::runtime_error(SDL_GetError());
    SDL_LockSurface(surface.get());
    for (int y = 0; y < image.height; ++y) {
        auto *row = reinterpret_cast<std::uint8_t *>(surface->pixels) +
                    static_cast<std::ptrdiff_t>(y) * surface->pitch;
        for (int x = 0; x < image.width; ++x) {
            const std::uint16_t value = image.at(x, y);
            const std::uint32_t rgb = rgb565To888(value);
            auto *pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
            pixel[0] = static_cast<std::uint8_t>(rgb >> 16U);
            pixel[1] = static_cast<std::uint8_t>(rgb >> 8U);
            pixel[2] = static_cast<std::uint8_t>(rgb);
            pixel[3] = keyed && value == kPuzzleKey565 ? 0U : 0xffU;
        }
    }
    SDL_UnlockSurface(surface.get());
    return surface;
}

} // namespace fgp
