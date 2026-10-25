#include "game/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "game/menu_names.h"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {
namespace {

SDL_Point sceneSurfacePoint(SDL_Surface *surface, int x, int y) {
    if (!surface) {
        return {x, y};
    }
    return {
        x * surface->w / kLogicalWidth,
        y * surface->h / kLogicalHeight,
    };
}

SDL_Rect sceneLogicalRect(SDL_Surface *surface, const SDL_Rect &bounds) {
    if (!surface ||
        (surface->w == kLogicalWidth && surface->h == kLogicalHeight)) {
        return bounds;
    }
    const int left = bounds.x * kLogicalWidth / surface->w;
    const int top = bounds.y * kLogicalHeight / surface->h;
    const int right =
        (bounds.x + bounds.w) * kLogicalWidth / surface->w;
    const int bottom =
        (bounds.y + bounds.h) * kLogicalHeight / surface->h;
    return {left, top, std::max(1, right - left),
            std::max(1, bottom - top)};
}

} // namespace

AppCore::CursorKind AppCore::cursorKindForName(std::string_view name) {
    for (std::size_t index = 0; index < cursorNames.size(); ++index) {
        if (name == cursorNames[index]) {
            return static_cast<CursorKind>(index);
        }
    }
    return CursorKind::Default;
}

std::uint32_t GameApp::hotspotAt(int x, int y) {
    if (viewMode == ViewMode::Scene) {
        if (sceneCornerButtonAt(x, y)) {
            return 0U;
        }
        if (const auto balloonhunt = balloonhuntHotspotAt(x, y)) {
            return balloonhunt->first;
        }
        if (const auto pipeline = pipelineHotspotAt(x, y)) {
            return pipeline->first;
        }
        if (const auto hopscotch = hopscotchHotspotAt(x, y)) {
            return hopscotch->first;
        }
        if (const auto beemaze = beemazeHotspotAt(x, y)) {
            return beemaze->first;
        }
        if (const auto bugzzz = bugzzzHotspotAt(x, y)) {
            return bugzzz->first;
        }
        if (const auto wheelbarrow = wheelbarrowHotspotAt(x, y)) {
            return wheelbarrow->first;
        }
        if (const auto buildabike = buildabikeHotspotAt(x, y)) {
            return buildabike->first;
        }
        if (const auto mountain = mountainHotspotAt(x, y)) {
            return mountain->first;
        }
        if (const auto hustle = hustleHotspotAt(x, y)) {
            return hustle->first;
        }
        if (const auto jigsaw = jigsawHotspotAt(x, y)) {
            return jigsaw->first;
        }
        if (const auto sliding = slidingHotspotAt(x, y)) {
            return sliding->first;
        }
        if (const auto bouquet = bouquetHotspotAt(x, y)) {
            return bouquet->first;
        }
        if (const auto fence = fenceHotspotAt(x, y)) {
            return fence->first;
        }
        if (const auto textinvader = textinvaderHotspotAt(x, y)) {
            return textinvader->first;
        }
        if (const auto synonym = synonymHotspotAt(x, y)) {
            return synonym->first;
        }
        if (const auto mathlab = mathlabHotspotAt(x, y)) {
            return mathlab->first;
        }
        if (const auto trivia = triviaHotspotAt(x, y)) {
            return trivia->first;
        }
        if (const auto colorfill = colorfillHotspotAt(x, y)) {
            return colorfill->first;
        }
        if (const auto dynamic = sceneDynamicHotspotAt(x, y)) {
            return dynamic->first;
        }
        if (const auto object = sceneAnimationObjectAt(x, y)) {
            return object->first;
        }
        if (const auto collectible = sceneCollectibleAt(x, y)) {
            return collectible->first;
        }
    }
    SDL_Surface *surface =
        viewMode == ViewMode::Scene ? sceneHotspots.get() : hotspots.get();
    if (viewMode == ViewMode::Scene) {
        const SDL_Point point = sceneSurfacePoint(surface, x, y);
        return pixelValue(surface, point.x, point.y);
    }
    return pixelValue(surface, x, y);
}

std::optional<std::size_t> AppCore::sceneCornerButtonAt(int x, int y) {
    if (viewMode != ViewMode::Scene) {
        return std::nullopt;
    }
    const SDL_Point point{x, y};
    for (std::size_t index = sceneCornerButtons.size(); index > 0; --index) {
        if (SDL_PointInRect(&point, &sceneCornerButtons[index - 1].bounds)) {
            return index - 1;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> AppCore::menuCornerButtonAt(int x, int y) {
    if (viewMode != ViewMode::Menu) {
        return std::nullopt;
    }
    const SDL_Point point{x, y};
    for (std::size_t index = menuCornerButtons.size(); index > 0; --index) {
        if (SDL_PointInRect(&point, &menuCornerButtons[index - 1].bounds)) {
            return index - 1;
        }
    }
    return std::nullopt;
}

std::optional<SDL_Rect> GameApp::currentHotspotBounds(std::uint32_t hotspot) {
    if (viewMode == ViewMode::Scene) {
        if (const auto balloonhunt = balloonhuntHotspotAt(mouseX, mouseY);
            balloonhunt && balloonhunt->first == hotspot) {
            return balloonhunt->second;
        }
        if (const auto pipeline = pipelineHotspotAt(mouseX, mouseY);
            pipeline && pipeline->first == hotspot) {
            return pipeline->second;
        }
        if (const auto hopscotch = hopscotchHotspotAt(mouseX, mouseY);
            hopscotch && hopscotch->first == hotspot) {
            return hopscotch->second;
        }
        if (const auto beemaze = beemazeHotspotAt(mouseX, mouseY);
            beemaze && beemaze->first == hotspot) {
            return beemaze->second;
        }
        if (const auto bugzzz = bugzzzHotspotAt(mouseX, mouseY);
            bugzzz && bugzzz->first == hotspot) {
            return bugzzz->second;
        }
        if (const auto wheelbarrow = wheelbarrowHotspotAt(mouseX, mouseY);
            wheelbarrow && wheelbarrow->first == hotspot) {
            return wheelbarrow->second;
        }
        if (const auto buildabike =
                buildabikeHotspotAt(mouseX, mouseY);
            buildabike && buildabike->first == hotspot) {
            return buildabike->second;
        }
        if (const auto mountain = mountainHotspotAt(mouseX, mouseY);
            mountain && mountain->first == hotspot) {
            return mountain->second;
        }
        if (const auto hustle = hustleHotspotAt(mouseX, mouseY);
            hustle && hustle->first == hotspot) {
            return hustle->second;
        }
        if (const auto fence = fenceHotspotAt(mouseX, mouseY);
            fence && fence->first == hotspot) {
            return fence->second;
        }
        if (const auto textinvader =
                textinvaderHotspotAt(mouseX, mouseY);
            textinvader && textinvader->first == hotspot) {
            return textinvader->second;
        }
        if (const auto synonym = synonymHotspotAt(mouseX, mouseY);
            synonym && synonym->first == hotspot) {
            return synonym->second;
        }
        if (const auto mathlab = mathlabHotspotAt(mouseX, mouseY);
            mathlab && mathlab->first == hotspot) {
            return mathlab->second;
        }
        if (const auto trivia = triviaHotspotAt(mouseX, mouseY);
            trivia && trivia->first == hotspot) {
            return trivia->second;
        }
        if (const auto dynamic =
                sceneDynamicHotspotAt(mouseX, mouseY);
            dynamic && dynamic->first == hotspot) {
            return std::optional<SDL_Rect>{dynamic->second};
        }
        if (const auto object =
                sceneAnimationObjectAt(mouseX, mouseY);
            object && object->first == hotspot) {
            return std::optional<SDL_Rect>{object->second};
        }
        if (const auto collectible =
                sceneCollectibleAt(mouseX, mouseY);
            collectible && collectible->first == hotspot) {
            return std::optional<SDL_Rect>{collectible->second};
        }
    }
    SDL_Surface *surface =
        viewMode == ViewMode::Scene ? sceneHotspots.get() : hotspots.get();
    auto bounds = hotspotBounds(surface, hotspot);
    if (bounds && viewMode == ViewMode::Scene) {
        *bounds = sceneLogicalRect(surface, *bounds);
    }
    return bounds;
}

AppCore::CursorKind AppCore::cursorForContext() {
    if (pendingScene) {
        return CursorKind::Wait;
    }
    if (dialogMode != DialogMode::None || storyNavigationOpen ||
        viewMode == ViewMode::Options) {
        return CursorKind::Default;
    }
    if (minigameBusyCursor) {
        return CursorKind::Wait;
    }
    if (sceneSequenceBlocksInput ||
        (viewMode == ViewMode::Scene &&
         sceneCharacterReactionBlocksInput())) {
        // Scripted scene responses block pointer input until their
        // completion event. The original keeps a visible hourglass
        // during that interval instead of making the pointer vanish.
        return CursorKind::Wait;
    }
    if (viewMode == ViewMode::Menu) {
        // The menu scene descriptor has no cursor override table.
        // Its round interface controls therefore retain the hand.
        return CursorKind::Default;
    }

    if (activeScene == "sceneindex") {
        if (sceneIndexTabAt(mouseX, mouseY)) {
            return CursorKind::Default;
        }
        const SDL_Point point{mouseX, mouseY};
        for (std::size_t index = 0;
             index < sceneIndexTileBounds.size() &&
             index < sceneIndexCursorKinds.size();
             ++index) {
            if (SDL_PointInRect(&point, &sceneIndexTileBounds[index]) &&
                index < sceneIndexNames.size() &&
                !sceneIndexNames[index].empty()) {
                return sceneIndexCursorKinds[index];
            }
        }
    }
    return cursorKindForName(
        executableGameData.cursorFor(activeScene, hoveredHotspot));
}
} // namespace fgp
