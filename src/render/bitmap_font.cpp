#include "render/bitmap_font.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace fgp {

BitmapFont loadBitmapFont(SDL_Renderer *renderer,
                          std::span<const std::uint8_t> bytes) {
    BitmapFont font;
    font.surface = loadBmp(bytes);
    font.height = font.surface->h;

    Uint8 separatorRed = 0;
    Uint8 separatorGreen = 0;
    Uint8 separatorBlue = 0;
    SDL_GetRGB(pixelValue(font.surface.get(), 0, 0), font.surface->format,
               &separatorRed, &separatorGreen, &separatorBlue);
    const auto near = [](Uint8 value, Uint8 expected) {
        return std::abs(static_cast<int>(value) -
                        static_cast<int>(expected)) <= 8;
    };

    std::vector<std::pair<int, int>> delimiters;
    bool inDelimiter = false;
    int delimiterStart = 0;
    for (int x = 0; x <= font.surface->w; ++x) {
        bool separator = x < font.surface->w;
        if (separator) {
            for (int y = 0; y < font.surface->h; ++y) {
                Uint8 red = 0;
                Uint8 green = 0;
                Uint8 pixelBlue = 0;
                SDL_GetRGB(pixelValue(font.surface.get(), x, y),
                           font.surface->format, &red, &green, &pixelBlue);
                if (!near(red, separatorRed) ||
                    !near(green, separatorGreen) ||
                    !near(pixelBlue, separatorBlue)) {
                    separator = false;
                    break;
                }
            }
        }
        if (separator && !inDelimiter) {
            delimiterStart = x;
            inDelimiter = true;
        } else if (!separator && inDelimiter) {
            delimiters.emplace_back(delimiterStart, x - 1);
            inDelimiter = false;
        }
    }
    if (delimiters.size() < 257) {
        throw std::runtime_error("Unsupported FGP bitmap-font strip");
    }
    const int backgroundX = std::min(
        font.surface->w - 1, delimiters.front().second + 1);
    Uint8 backgroundRed = 0;
    Uint8 backgroundGreen = 0;
    Uint8 backgroundBlue = 0;
    SDL_GetRGB(pixelValue(font.surface.get(), backgroundX, 0),
               font.surface->format, &backgroundRed, &backgroundGreen,
               &backgroundBlue);
    for (std::size_t code = 0; code < font.glyphs.size(); ++code) {
        const int left = delimiters[code].second + 1;
        const int right = delimiters[code + 1].first;
        font.glyphs[code] = SDL_Rect{left, 0, std::max(0, right - left), font.height};
    }

    // Both the glyph background and the blue separator columns must become
    // fully transparent. A colour key can only cover one of them, and under
    // fractional window scaling the sampler reaches a glyph rectangle's
    // outermost columns, which made the separators bleed in as thin vertical
    // bars between words. Converting once and clearing both colours keeps
    // every glyph edge transparent instead.
    SurfacePtr converted(SDL_ConvertSurfaceFormat(
        font.surface.get(), SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) {
        throw std::runtime_error(
            std::string("SDL_ConvertSurfaceFormat(font) failed: ") +
            SDL_GetError());
    }
    if (SDL_MUSTLOCK(converted.get()) &&
        SDL_LockSurface(converted.get()) != 0) {
        throw std::runtime_error(std::string("SDL_LockSurface(font) failed: ") +
                                 SDL_GetError());
    }
    for (int y = 0; y < converted->h; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(
            static_cast<Uint8 *>(converted->pixels) +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(converted->pitch));
        for (int x = 0; x < converted->w; ++x) {
            Uint8 red = 0;
            Uint8 green = 0;
            Uint8 blue = 0;
            SDL_GetRGB(row[x], converted->format, &red, &green, &blue);
            const bool background = near(red, backgroundRed) &&
                                    near(green, backgroundGreen) &&
                                    near(blue, backgroundBlue);
            const bool separator = near(red, separatorRed) &&
                                   near(green, separatorGreen) &&
                                   near(blue, separatorBlue);
            if (background || separator) {
                row[x] = 0;
            }
        }
    }
    if (SDL_MUSTLOCK(converted.get())) {
        SDL_UnlockSurface(converted.get());
    }
    font.surface = std::move(converted);
    font.texture = makeTexture(renderer, font.surface.get());
    SDL_SetTextureBlendMode(font.texture.get(), SDL_BLENDMODE_BLEND);
    return font;
}

int textWidth(const BitmapFont &font, std::string_view cp1252) {
    int width = 0;
    for (const unsigned char code : cp1252) {
        width += font.glyphs[code].w;
    }
    return width;
}

void drawText(SDL_Renderer *renderer,
              const BitmapFont &font,
              std::string_view cp1252,
              int x,
              int y) {
    for (const unsigned char code : cp1252) {
        const SDL_Rect &source = font.glyphs[code];
        SDL_Rect destination{x, y, source.w, source.h};
        SDL_RenderCopy(renderer, font.texture.get(), &source, &destination);
        x += source.w;
    }
}

void drawCenteredText(SDL_Renderer *renderer,
                      const BitmapFont &font,
                      std::string_view cp1252,
                      const SDL_Rect &bounds) {
    drawText(renderer, font, cp1252,
             bounds.x + (bounds.w - textWidth(font, cp1252)) / 2,
             bounds.y + (bounds.h - font.height) / 2);
}

std::vector<std::string> wrapText(const BitmapFont &font,
                                  std::string_view cp1252,
                                  int maximumWidth) {
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    auto flushWord = [&] {
        if (word.empty()) {
            return;
        }
        const std::string candidate = line.empty() ? word : line + ' ' + word;
        if (!line.empty() && textWidth(font, candidate) > maximumWidth) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char character : cp1252) {
        if (character == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else if (character == ' ') {
            flushWord();
        } else {
            word.push_back(character);
        }
    }
    flushWord();
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace fgp
