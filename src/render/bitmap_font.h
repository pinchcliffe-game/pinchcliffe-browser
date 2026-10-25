#pragma once

// The original packaged bitmap fonts (blue-delimited glyph strips) and the
// text layout helpers built on them. All text is Windows-1252.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/sdl_util.h"

namespace fgp {

struct BitmapFont {
    SurfacePtr surface;
    TexturePtr texture;
    std::array<SDL_Rect, 256> glyphs{};
    int height{};
};

BitmapFont loadBitmapFont(SDL_Renderer *renderer,
                          std::span<const std::uint8_t> bytes);
int textWidth(const BitmapFont &font, std::string_view cp1252);
void drawText(SDL_Renderer *renderer, const BitmapFont &font,
              std::string_view cp1252, int x, int y);
void drawCenteredText(SDL_Renderer *renderer, const BitmapFont &font,
                      std::string_view cp1252, const SDL_Rect &bounds);
std::vector<std::string> wrapText(const BitmapFont &font,
                                  std::string_view cp1252, int maximumWidth);

} // namespace fgp
