#include "game/app_viewer.h"

#include <algorithm>
#include <stdexcept>

namespace fgp {

ViewerWidget &ViewerWidgets::add(SDL_Renderer *renderer, int id, int x, int y,
                                 std::span<const std::uint8_t> normal,
                                 std::span<const std::uint8_t> pressed,
                                 std::span<const std::uint8_t> disabled,
                                 ViewerWidget::Type type, int group) {
    remove(id);
    ViewerWidget widget;
    widget.id = id;
    widget.type = type;
    widget.group = group;
    widget.position = {x, y};
    const std::array<std::span<const std::uint8_t>, 3> sources{
        normal, pressed, disabled};
    for (std::size_t slot = 0; slot < sources.size(); ++slot) {
        if (sources[slot].empty()) continue;
        widget.art[slot] = loadGreenKeyedBmp(sources[slot]);
        widget.textures[slot] = makeTexture(renderer, widget.art[slot].get());
        SDL_SetTextureBlendMode(widget.textures[slot].get(),
                                SDL_BLENDMODE_BLEND);
    }
    widgets.push_back(std::move(widget));
    return widgets.back();
}

bool ViewerWidgets::remove(int id) {
    const auto found = std::find_if(
        widgets.begin(), widgets.end(),
        [id](const ViewerWidget &widget) { return widget.id == id; });
    if (found == widgets.end()) return false;
    widgets.erase(found);
    return true;
}

ViewerWidget *ViewerWidgets::find(int id) {
    for (auto &widget : widgets)
        if (widget.id == id) return &widget;
    return nullptr;
}

const ViewerWidget *ViewerWidgets::find(int id) const {
    for (const auto &widget : widgets)
        if (widget.id == id) return &widget;
    return nullptr;
}

void ViewerWidgets::setFrame(int id, int frame) {
    if (auto *widget = find(id)) widget->frame = frame;
}

int ViewerWidgets::frame(int id) const {
    const auto *widget = find(id);
    return widget ? widget->frame : -1;
}

std::optional<SDL_Rect> ViewerWidgets::boundsOf(const ViewerWidget &widget) {
    // 0x004069b0 measures the widget by its artwork; every slot of one
    // control has the same size in the viewers' packages.
    for (const auto &surface : widget.art)
        if (surface)
            return SDL_Rect{widget.position.x, widget.position.y, surface->w,
                            surface->h};
    return std::nullopt;
}

std::optional<SDL_Rect> ViewerWidgets::bounds(int id) const {
    const auto *widget = find(id);
    return widget ? boundsOf(*widget) : std::nullopt;
}

bool ViewerWidgets::contains(const ViewerWidget &widget, int x, int y) {
    const auto rect = boundsOf(widget);
    const SDL_Point point{x, y};
    return rect && SDL_PointInRect(&point, &*rect);
}

std::uint32_t ViewerWidgets::eventIdAt(int x, int y,
                                       std::uint32_t staticHotspot) const {
    for (auto widget = widgets.rbegin(); widget != widgets.rend(); ++widget) {
        const std::size_t slot =
            widget->frame == 2 ? 2U : widget->frame == 1 ? 1U : 0U;
        const auto &surface = widget->art[slot] ? widget->art[slot]
                                                : widget->art[0];
        if (!surface || !contains(*widget, x, y)) continue;
        if (opaqueSurfacePixel(surface.get(), x - widget->position.x,
                               y - widget->position.y))
            return static_cast<std::uint32_t>(widget->id);
    }
    return staticHotspot;
}

std::optional<int> ViewerWidgets::press(int x, int y,
                                        std::uint32_t staticHotspot) {
    const std::uint32_t eventId = eventIdAt(x, y, staticHotspot);
    for (auto &widget : widgets) {
        if (static_cast<std::uint32_t>(widget.id) != eventId ||
            !contains(widget, x, y))
            continue;
        if (widget.type == ViewerWidget::Type::Radio) {
            // 0x00406c80: only an unselected radio reacts; it clears its
            // group (0x00406b00), selects itself and notifies at once.
            if (widget.frame != 0) continue;
            for (auto &other : widgets)
                if (other.type == ViewerWidget::Type::Radio &&
                    other.group == widget.group)
                    other.frame = 0;
            widget.frame = 1;
            return widget.id;
        }
        // 0x00406c50: a button at frame 0 arms (frame 1); anything else is
        // left alone and the search continues.
        if (widget.frame != 0) continue;
        widget.frame = 1;
        return std::nullopt;
    }
    return std::nullopt;
}

bool ViewerWidgets::armed() const {
    return std::any_of(widgets.begin(), widgets.end(),
                       [](const ViewerWidget &widget) {
                           return widget.type == ViewerWidget::Type::Button &&
                                  widget.frame == 1;
                       });
}

std::optional<int> ViewerWidgets::release(int x, int y,
                                          std::uint32_t staticHotspot) {
    const std::uint32_t eventId = eventIdAt(x, y, staticHotspot);
    std::optional<int> notified;
    for (auto &widget : widgets) {
        if (widget.type != ViewerWidget::Type::Button || widget.frame != 1)
            continue;
        widget.frame = 0;
        if (!notified && static_cast<std::uint32_t>(widget.id) == eventId &&
            contains(widget, x, y))
            notified = widget.id;
    }
    return notified;
}

std::optional<std::span<const std::uint8_t>>
ViewerSceneSupport::viewerAssetOptional(std::string_view name) {
    const std::string asset(name);
    for (Package *package :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (!package) continue;
        if (const auto *entry = package->findOptional(asset))
            return package->data(*entry);
    }
    return std::nullopt;
}

std::span<const std::uint8_t> ViewerSceneSupport::viewerAsset(
    std::string_view name) {
    if (const auto data = viewerAssetOptional(name)) return *data;
    throw std::runtime_error("Viewer asset not found: " + std::string(name));
}

std::span<const std::uint8_t> ViewerSceneSupport::viewerCommonAsset(
    std::string_view name) {
    const std::string asset(name);
    for (Package *package :
         {addonCommon.get(), data2Common.get(), editionCommon.get()}) {
        if (!package) continue;
        if (const auto *entry = package->findOptional(asset))
            return package->data(*entry);
    }
    return common.data(common.find(asset));
}

std::uint32_t ViewerSceneSupport::viewerStaticHotspotAt(int x, int y) const {
    SDL_Surface *surface = sceneHotspots.get();
    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h)
        return 0U;
    return pixelValue(surface, x, y);
}

std::optional<fs::path> ViewerSceneSupport::viewerProfilePath() const {
    if (profiles.empty() || profiles[profileIndex] == "No profile")
        return std::nullopt;
    fs::path path = profileDirectory / (profiles[profileIndex] + ".pfl");
    if (!fs::exists(path)) return std::nullopt;
    return path;
}

void ViewerSceneSupport::traceViewerAdd(std::string_view asset, int x, int y,
                                        int layer, std::uint32_t hotspot,
                                        std::uint32_t conflictMask) {
    // The comparator's playback projection reads the original's
    // SCENE_AddAnim as a start, and native starts as the same.
    referenceTrace.emit("animation_start", {
        {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"loop", false}});
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", hotspot}, {"conflict_mask", conflictMask}});
}

void ViewerSceneSupport::traceViewerRemove(std::string_view asset) {
    referenceTrace.emit("animation_remove",
                        {{"asset", asset}, {"reason", "removed"}});
}

void ViewerSceneSupport::traceViewerStop(std::string_view asset) {
    // Tagged like the original's logpoint so the comparator applies the same
    // rule to both: a stop later followed by a removal does not count.
    referenceTrace.emit("animation_stop",
                        {{"asset", asset}, {"reason", "stopped"},
                         {"raw_event", "SCENE_StopAnim"}});
}

void ViewerSceneSupport::traceViewerFinished(std::string_view asset) {
    referenceTrace.emit("animation_stop",
                        {{"asset", asset}, {"reason", "completed"}});
}

void ViewerSceneSupport::traceViewerControl(std::string_view scene, int id) {
    referenceTrace.emit("click_dispatch",
                        {{"scene", scene}, {"hotspot", id}});
}

} // namespace fgp
