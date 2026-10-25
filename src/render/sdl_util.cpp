#include "render/sdl_util.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace fgp {

SurfacePtr loadBmp(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("BMP asset is too large for SDL");
    }
    SDL_RWops *stream = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (!stream) {
        throw std::runtime_error(std::string("SDL_RWFromConstMem failed: ") + SDL_GetError());
    }
    SurfacePtr surface(SDL_LoadBMP_RW(stream, SDL_TRUE));
    if (!surface) {
        throw std::runtime_error(std::string("SDL_LoadBMP_RW failed: ") + SDL_GetError());
    }
    return surface;
}

SurfacePtr loadGreenKeyedBmp(std::span<const std::uint8_t> bytes) {
    SurfacePtr surface = loadBmp(bytes);
    const Uint32 green = SDL_MapRGB(surface->format, 0, 255, 0);
    if (SDL_SetColorKey(surface.get(), SDL_TRUE, green) != 0) {
        throw std::runtime_error(std::string("SDL_SetColorKey failed: ") + SDL_GetError());
    }
    return surface;
}

TexturePtr makeTexture(SDL_Renderer *renderer, SDL_Surface *surface) {
    TexturePtr texture(SDL_CreateTextureFromSurface(renderer, surface));
    if (!texture) {
        throw std::runtime_error(std::string("SDL_CreateTextureFromSurface failed: ") +
                                 SDL_GetError());
    }
    return texture;
}

std::uint32_t pixelValue(SDL_Surface *surface, int x, int y) {
    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return 0;
    }
    const auto *row = static_cast<const std::uint8_t *>(surface->pixels) + y * surface->pitch;
    const int bytes = surface->format->BytesPerPixel;
    if (bytes == 1) {
        return row[x];
    }
    if (bytes == 2) {
        std::uint16_t value{};
        std::memcpy(&value, row + x * bytes, sizeof(value));
        return value;
    }
    if (bytes == 3) {
        const auto *pixel = row + x * bytes;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        return (static_cast<std::uint32_t>(pixel[0]) << 16U) |
               (static_cast<std::uint32_t>(pixel[1]) << 8U) | pixel[2];
#else
        return pixel[0] | (static_cast<std::uint32_t>(pixel[1]) << 8U) |
               (static_cast<std::uint32_t>(pixel[2]) << 16U);
#endif
    }
    std::uint32_t value{};
    std::memcpy(&value, row + x * bytes, sizeof(value));
    return value;
}

bool opaqueSurfacePixel(SDL_Surface *surface, int x, int y) {
    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return false;
    }
    Uint32 colorKey{};
    return SDL_GetColorKey(surface, &colorKey) != 0 ||
           pixelValue(surface, x, y) != colorKey;
}

std::optional<SDL_Rect> hotspotBounds(SDL_Surface *surface, std::uint32_t id) {
    if (!surface || id == 0) {
        return std::nullopt;
    }
    int minX = surface->w;
    int minY = surface->h;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < surface->h; ++y) {
        for (int x = 0; x < surface->w; ++x) {
            if (pixelValue(surface, x, y) == id) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY) {
        return std::nullopt;
    }
    return SDL_Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

} // namespace fgp
