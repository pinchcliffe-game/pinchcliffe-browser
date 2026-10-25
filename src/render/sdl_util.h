#pragma once

// SDL surface/texture helpers shared by the views: BMP loading with the
// legacy green color key, pixel probing, and hotspot-mask bounds.

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace fgp {

struct SurfaceDeleter {
    void operator()(SDL_Surface *surface) const { SDL_FreeSurface(surface); }
};

struct TextureDeleter {
    void operator()(SDL_Texture *texture) const { SDL_DestroyTexture(texture); }
};

using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

SurfacePtr loadBmp(std::span<const std::uint8_t> bytes);
SurfacePtr loadGreenKeyedBmp(std::span<const std::uint8_t> bytes);
TexturePtr makeTexture(SDL_Renderer *renderer, SDL_Surface *surface);

std::uint32_t pixelValue(SDL_Surface *surface, int x, int y);
bool opaqueSurfacePixel(SDL_Surface *surface, int x, int y);
std::optional<SDL_Rect> hotspotBounds(SDL_Surface *surface, std::uint32_t id);

} // namespace fgp
