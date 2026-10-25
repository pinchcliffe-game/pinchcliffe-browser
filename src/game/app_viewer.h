#pragma once

// The engine's on-screen controls, as the media viewers (gallery, info,
// movieplayer, anaglyph) create them through 0x00406ed0 and the radio
// wrapper 0x00406fb0. Only the behaviour those four scenes rely on is
// modelled here; every rule cites the FGP.exe address it was read from.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/app_core.h"
#include "render/sdl_util.h"

namespace fgp {

struct ViewerWidget {
    // Widget +0x08: 1 push button (0x00406ed0), 2 radio (0x00406fb0 sets
    // it through 0x00406880). Toggles (type 3) are not used by the viewers.
    enum class Type { Button = 1, Radio = 2 };

    int id{};
    Type type{Type::Button};
    // Widget +0x0c, the radio group 0x00406aa0 stores; 0x00406b00 clears
    // every radio of a group before 0x00406c80 selects one.
    int group{};
    SDL_Point position{};
    // Artwork slots 0..2 ("_0" normal, "_1" pressed, "_2" disabled), filled
    // by 0x004068b0 only for non-empty names.
    std::array<SurfacePtr, 3> art;
    std::array<TexturePtr, 3> textures;
    // Widget +0x270, the frame index 0x00406740 stores.
    int frame{};
    // Localised label (0x00406910 slot 0xb) drawn centred over the artwork.
    std::string label;
    // Tooltip text the scene supplies for event 0x11a (hover).
    std::string tooltip;
};

class ViewerWidgets {
public:
    void clear() { widgets.clear(); }

    // 0x00406ed0(id, x, y, art0, art1, art2, ...). Missing artwork is
    // passed as an empty span.
    ViewerWidget &add(SDL_Renderer *renderer, int id, int x, int y,
                      std::span<const std::uint8_t> normal,
                      std::span<const std::uint8_t> pressed,
                      std::span<const std::uint8_t> disabled = {},
                      ViewerWidget::Type type = ViewerWidget::Type::Button,
                      int group = 0);
    // 0x00406610: destroy the widget with this id; false when absent.
    bool remove(int id);
    ViewerWidget *find(int id);
    const ViewerWidget *find(int id) const;
    // 0x00406720(id, frame).
    void setFrame(int id, int frame);
    // 0x00406700: the frame, or -1 when no widget has this id.
    int frame(int id) const;

    // The hotspot event id at a point: the topmost widget whose current
    // artwork is opaque there, else the static hotspots.bmp value.
    std::uint32_t eventIdAt(int x, int y, std::uint32_t staticHotspot) const;
    // Mouse down 0x105 through 0x00406b50: the first widget whose bounds
    // hold the point and whose id is the event id. A radio notifies on the
    // press (0x00406c80); a button only arms (0x00406c50). Returns the id a
    // 0x116 notification carries, if any.
    std::optional<int> press(int x, int y, std::uint32_t staticHotspot);
    // True while a button is armed (frame 1) by a press.
    bool armed() const;
    // Mouse up 0x106 through 0x00406cf0/0x00406d40: every armed button
    // returns to frame 0; it notifies 0x116 when the pointer is still over
    // it (widget +0x274).
    std::optional<int> release(int x, int y, std::uint32_t staticHotspot);
    std::optional<SDL_Rect> bounds(int id) const;

    // 0x00406840: a button at frame 1 draws the pressed art only while the
    // pointer is inside; every other frame draws its own slot.
    template <typename DrawLabel>
    void render(SDL_Renderer *renderer, int mouseX, int mouseY,
                std::uint32_t hoveredId, DrawLabel &&drawLabel) const {
        for (const auto &widget : widgets) {
            std::size_t slot = static_cast<std::size_t>(widget.frame);
            if (widget.type == ViewerWidget::Type::Button &&
                widget.frame == 1 &&
                (hoveredId != static_cast<std::uint32_t>(widget.id) ||
                 !contains(widget, mouseX, mouseY)))
                slot = 0U;
            if (slot < widget.textures.size() && widget.textures[slot]) {
                const auto &surface = widget.art[slot];
                SDL_Rect destination{widget.position.x, widget.position.y,
                                     surface->w, surface->h};
                SDL_RenderCopy(renderer, widget.textures[slot].get(), nullptr,
                               &destination);
            }
            if (!widget.label.empty())
                if (const auto rect = boundsOf(widget)) drawLabel(widget, *rect);
        }
    }

private:
    static std::optional<SDL_Rect> boundsOf(const ViewerWidget &widget);
    static bool contains(const ViewerWidget &widget, int x, int y);

    std::vector<ViewerWidget> widgets;
};

// Services the four viewer scenes share: their package lookup, the static
// hotspot mask and the trace records the original's x32dbg logpoints on
// SCENE_AddAnim (0x00412840), SCENE_RemoveAnim (0x00412aa0), SCENE_StopAnim
// (0x00412a60) and the animation-finished event (0x0040c8a0) produce.
class ViewerSceneSupport : protected virtual AppCore {
protected:
    // The scene's packages, most specific first, as loadScene layers them.
    std::span<const std::uint8_t> viewerAsset(std::string_view name);
    std::optional<std::span<const std::uint8_t>> viewerAssetOptional(
        std::string_view name);
    std::span<const std::uint8_t> viewerCommonAsset(std::string_view name);
    std::uint32_t viewerStaticHotspotAt(int x, int y) const;
    std::optional<fs::path> viewerProfilePath() const;

    void traceViewerAdd(std::string_view asset, int x, int y, int layer,
                        std::uint32_t hotspot = 0U,
                        std::uint32_t conflictMask = 0U);
    void traceViewerRemove(std::string_view asset);
    void traceViewerStop(std::string_view asset);
    void traceViewerFinished(std::string_view asset);
    void traceViewerControl(std::string_view scene, int id);
};

} // namespace fgp
