#include "game/activity_panel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "util/reference_trace.h"

namespace fgp {
namespace {

// The x87 idiom FGP.exe uses before __ftol (0x0048c214): add or subtract 0.5
// by sign, then truncate.
int roundAway(double value) {
    return static_cast<int>(value > 0.0 ? value + 0.5 : value - 0.5);
}

} // namespace

SurfacePtr createSurface(int width, int height) {
    SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
        0, std::max(1, width), std::max(1, height), 32,
        SDL_PIXELFORMAT_ARGB8888));
    if (!surface) {
        throw std::runtime_error(std::string("SDL_CreateRGBSurface failed: ") +
                                 SDL_GetError());
    }
    SDL_SetSurfaceBlendMode(surface.get(), SDL_BLENDMODE_NONE);
    return surface;
}

void fillSurface(SDL_Surface *surface, const SDL_Rect *rect,
                 std::uint32_t rgb) {
    SDL_FillRect(surface, rect,
                 SDL_MapRGB(surface->format, (rgb >> 16U) & 0xffU,
                            (rgb >> 8U) & 0xffU, rgb & 0xffU));
}

void drawFontText(SDL_Surface *target, const BitmapFont &font,
                  std::string_view text, int x, int y) {
    if (!font.surface) return;
    SDL_SetSurfaceBlendMode(font.surface.get(), SDL_BLENDMODE_BLEND);
    for (const char character : text) {
        const auto code = static_cast<unsigned char>(character);
        SDL_Rect source = font.glyphs[code];
        SDL_Rect destination{x, y, source.w, source.h};
        SDL_BlitSurface(font.surface.get(), &source, target, &destination);
        x += source.w;
    }
}

namespace {
// 0x00402640.
bool layoutSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r';
}
} // namespace

std::vector<ParagraphLine> breakParagraph(const BitmapFont &font,
                                          std::string_view text,
                                          int maximumWidth) {
    std::vector<ParagraphLine> lines;
    const auto at = [&](std::size_t index) {
        return index < text.size() ? text[index] : '\0';
    };
    std::size_t start = 0;
    while (true) {
        // 0x00402530 from start.
        if (at(start) == '\0') break;
        bool inWord = !layoutSpace(at(start));
        std::size_t wordEnd = 0;
        std::size_t index = 0;
        while (true) {
            const char character = at(start + index);
            if (inWord) {
                if (layoutSpace(character)) {
                    inWord = false;
                    wordEnd = index;
                }
            } else if (!layoutSpace(character)) {
                inWord = true;
            }
            const int width =
                textWidth(font, text.substr(start, index + 1));
            if (width >= maximumWidth || character == '\n') break;
            ++index;
            if (at(start + index) == '\0') break;
        }
        if (at(start + index) != '\0') index = wordEnd;
        const std::size_t consumed =
            layoutSpace(at(start + index)) ? index + 1 : index;
        if (consumed == 0) break;
        lines.push_back({start, index});
        start += consumed;
    }
    return lines;
}

int drawParagraph(SDL_Surface *target, const BitmapFont &font,
                  std::string_view text, int x, int y, int maximumWidth) {
    const auto lines = breakParagraph(font, text, maximumWidth);
    for (std::size_t line = 0; line < lines.size(); ++line) {
        drawFontText(target, font,
                     text.substr(lines[line].start, lines[line].length), x,
                     y + static_cast<int>(line) * font.height);
    }
    return static_cast<int>(lines.size()) * font.height;
}

// 0x00420b00, tiling each piece with 0x00420dc0.
SurfacePtr nineSlice(int width, int height, SDL_Surface *source,
                     const SDL_Rect &centre) {
    SurfacePtr result = createSurface(width, height);
    const int sourceW = source->w;
    const int sourceH = source->h;
    const int right = width - (sourceW - (centre.x + centre.w));
    const int bottom = height - (sourceH - (centre.y + centre.h));
    const std::array<int, 4> sourceX{0, centre.x, centre.x + centre.w, sourceW};
    const std::array<int, 4> sourceY{0, centre.y, centre.y + centre.h, sourceH};
    const std::array<int, 4> targetX{0, centre.x, right, width};
    const std::array<int, 4> targetY{0, centre.y, bottom, height};
    SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const int tileW = sourceX[column + 1] - sourceX[column];
            const int tileH = sourceY[row + 1] - sourceY[row];
            if (tileW <= 0 || tileH <= 0) continue;
            for (int y = targetY[row]; y < targetY[row + 1]; y += tileH) {
                for (int x = targetX[column]; x < targetX[column + 1];
                     x += tileW) {
                    SDL_Rect from{sourceX[column], sourceY[row],
                                  std::min(tileW, targetX[column + 1] - x),
                                  std::min(tileH, targetY[row + 1] - y)};
                    SDL_Rect to{x, y, from.w, from.h};
                    SDL_BlitSurface(source, &from, result.get(), &to);
                }
            }
        }
    }
    return result;
}

SDL_Point centreIn(const SDL_Rect &rect, int width, int height) {
    return {(rect.w - width) / 2 + rect.x, (rect.h - height) / 2 + rect.y};
}

ActivityStage::ActivityStage(SDL_Renderer *renderer, ReferenceTrace &trace)
    : renderer_(renderer), trace_(trace) {}

int ActivityStage::create(std::string name, int defaultX, int defaultY,
                          std::uint32_t hotspot, bool keyed) {
    StageSprite sprite;
    sprite.name = std::move(name);
    sprite.defaultX = defaultX;
    sprite.defaultY = defaultY;
    sprite.hotspot = hotspot;
    sprite.keyed = keyed;
    sprites_.push_back(std::move(sprite));
    return static_cast<int>(sprites_.size() - 1U);
}

void ActivityStage::setSurface(int id, SurfacePtr surface) {
    auto &target = sprite(id);
    target.surface = std::move(surface);
    target.textureDirty = true;
}

void ActivityStage::touch(int id) { sprite(id).textureDirty = true; }

void ActivityStage::add(int id, int x, int y, int layer) {
    auto &target = sprite(id);
    trace_.emit("animation_add", {
        {"asset", target.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", target.hotspot}, {"conflict_mask", 0}});
    if (x == -1 && y == -1) {
        x = target.defaultX;
        y = target.defaultY;
    }
    target.x = x;
    target.y = y;
    target.layer = layer;
    target.active = true;
    target.order = nextOrder_++;
    target.textureDirty = true;
}

void ActivityStage::remove(int id) {
    auto &target = sprite(id);
    // 0x00412aa0 logs RemoveAnim before its nested StopAnim (0x00412a60).
    trace_.emit("animation_remove",
                {{"asset", target.name}, {"reason", "removed"}});
    trace_.emit("animation_stop",
                {{"asset", target.name}, {"reason", "stopped"}});
    target.active = false;
}

void ActivityStage::move(int id, int x, int y) {
    auto &target = sprite(id);
    target.x = x;
    target.y = y;
}

void ActivityStage::moveCentre(int id, int x, int y) {
    const SDL_Rect bounds = rect(id);
    move(id, x - bounds.w / 2, y - bounds.h / 2);
}

void ActivityStage::setLayer(int id, int layer) {
    auto &target = sprite(id);
    target.layer = layer;
    target.order = nextOrder_++;
}

SDL_Rect ActivityStage::rect(int id) const {
    const auto &target = sprite(id);
    if (!target.surface) return SDL_Rect{target.x, target.y, 0, 0};
    return SDL_Rect{target.x, target.y, target.surface->w, target.surface->h};
}

std::optional<std::uint32_t>
ActivityStage::hotspotAt(int x, int y, std::uint32_t ignoredHotspot) const {
    std::vector<const StageSprite *> order;
    for (const auto &candidate : sprites_) {
        if (candidate.active && candidate.surface && candidate.hotspot != 0U)
            order.push_back(&candidate);
    }
    std::sort(order.begin(), order.end(),
              [](const StageSprite *left, const StageSprite *right) {
                  return left->layer != right->layer
                             ? left->layer > right->layer
                             : left->order > right->order;
              });
    const SDL_Point point{x, y};
    for (const auto *candidate : order) {
        const SDL_Rect bounds{candidate->x, candidate->y,
                              candidate->surface->w, candidate->surface->h};
        if (!SDL_PointInRect(&point, &bounds)) continue;
        // 0x00413750 mode 3: keyed descriptors only claim non-key pixels.
        if (candidate->keyed &&
            pixelValue(candidate->surface.get(), x - bounds.x, y - bounds.y) ==
                pixelValue(candidate->surface.get(), 0, 0))
            continue;
        if (candidate->hotspot == ignoredHotspot) continue;
        return candidate->hotspot;
    }
    return std::nullopt;
}

void ActivityStage::render() {
    std::vector<StageSprite *> order;
    for (auto &candidate : sprites_) {
        if (candidate.active && candidate.surface) order.push_back(&candidate);
    }
    std::sort(order.begin(), order.end(),
              [](const StageSprite *left, const StageSprite *right) {
                  return left->layer != right->layer
                             ? left->layer < right->layer
                             : left->order < right->order;
              });
    for (auto *candidate : order) {
        if (candidate->textureDirty || !candidate->texture) {
            SDL_Surface *surface = candidate->surface.get();
            if (candidate->keyed) {
                SDL_SetColorKey(surface, SDL_TRUE, pixelValue(surface, 0, 0));
            } else {
                SDL_SetColorKey(surface, SDL_FALSE, 0);
            }
            candidate->texture = makeTexture(renderer_, surface);
            SDL_SetTextureBlendMode(candidate->texture.get(),
                                    SDL_BLENDMODE_BLEND);
            candidate->textureDirty = false;
        }
        const SDL_Rect destination{candidate->x, candidate->y,
                                   candidate->surface->w,
                                   candidate->surface->h};
        SDL_RenderCopy(renderer_, candidate->texture.get(), nullptr,
                       &destination);
    }
}

ActivityPanel::ActivityPanel(ActivityStage &stage, ImageLoader loader)
    : stage_(stage), loader_(std::move(loader)) {}

ActivityPanel::Control *ActivityPanel::find(std::uint32_t id) {
    for (auto &control : controls_) {
        if (control.id == id) return &control;
    }
    return nullptr;
}

const ActivityPanel::Control *ActivityPanel::find(std::uint32_t id) const {
    for (const auto &control : controls_) {
        if (control.id == id) return &control;
    }
    return nullptr;
}

void ActivityPanel::addControl(std::uint32_t id, int x, int y,
                               std::array<std::string, 4> images, Type type,
                               int group, std::string caption,
                               std::uint32_t hoverId, bool keyed) {
    if (find(id)) return;
    Control control;
    control.id = id;
    control.type = type;
    control.group = group;
    control.x = x;
    control.y = y;
    control.images = std::move(images);
    control.layer = layer_;
    // The control's descriptor sits at +0x1c; its hotspot is the id.
    control.sprite = stage_.create({}, x, y, id, keyed);
    controls_.push_back(std::move(control));
    Control &added = controls_.back();
    // 0x004068b0 redraws when it sets the image of the current state (0).
    redraw(added);
    if (!caption.empty() && captionFont_) {
        // 0x00406950: measure the control, then a centred label one layer
        // above it (0x00407a30 with flags 0x101).
        const SDL_Rect bounds = measure(added);
        const int width = textWidth(*captionFont_, caption);
        const int height = captionFont_->height;
        SurfacePtr label = createSurface(width, height);
        fillSurface(label.get(), nullptr, 0x00ff00U);
        drawFontText(label.get(), *captionFont_, caption, 0, 0);
        added.captionSprite = stage_.create({}, 0, 0, 0, true);
        stage_.setSurface(added.captionSprite, std::move(label));
        const SDL_Point at = centreIn(bounds, width, height);
        stage_.add(added.captionSprite, at.x, at.y, added.layer + 1);
    }
    // 0x00406ed0 ends with 0x00406dc0(hotspot under the cursor, 1).
    if (hoverId == id) {
        added.hover = true;
        redraw(added);
    }
}

void ActivityPanel::redraw(Control &control) {
    // 0x00406760 with the image choice of 0x00406810/0x00406840.
    std::string_view name = control.images[static_cast<std::size_t>(
        std::clamp(control.state, 0, 3))];
    if (control.type == Type::Push && control.state == 1) {
        name = control.hover ? control.images[1] : control.images[0];
    }
    auto &sprite = stage_.sprite(control.sprite);
    if (sprite.active) {
        if (!name.empty() && sprite.name == name) return;
        stage_.remove(control.sprite);
    }
    if (name.empty()) return;
    sprite.name = std::string(name);
    stage_.setSurface(control.sprite, loader_(name));
    stage_.add(control.sprite, -1, -1, control.layer);
}

SDL_Rect ActivityPanel::measure(Control &control) {
    // 0x004069b0: the size is cached after the first measurement. An
    // invisible control is measured by adding and removing its first image.
    if (control.width == 0 && control.height == 0) {
        auto &sprite = stage_.sprite(control.sprite);
        if (sprite.active) {
            const SDL_Rect bounds = stage_.rect(control.sprite);
            control.width = bounds.w;
            control.height = bounds.h;
            return bounds;
        }
        const auto first = std::find_if(
            control.images.begin(), control.images.end(),
            [](const std::string &name) { return !name.empty(); });
        if (first == control.images.end()) return SDL_Rect{0, 0, 0, 0};
        sprite.name = *first;
        stage_.setSurface(control.sprite, loader_(*first));
        stage_.add(control.sprite, -1, -1, control.layer);
        const SDL_Rect bounds = stage_.rect(control.sprite);
        stage_.remove(control.sprite);
        control.width = bounds.w;
        control.height = bounds.h;
        return bounds;
    }
    return SDL_Rect{control.x, control.y, control.width, control.height};
}

void ActivityPanel::setControlState(Control &control, int state) {
    // 0x00406740.
    if (control.state == state) return;
    control.state = state;
    redraw(control);
}

void ActivityPanel::setState(std::uint32_t id, int state) {
    if (auto *control = find(id)) setControlState(*control, state);
}

int ActivityPanel::state(std::uint32_t id) const {
    const auto *control = find(id);
    return control ? control->state : -1;
}

void ActivityPanel::setGroupState(int group, int state) {
    for (auto &control : controls_) {
        if (control.group == group) setControlState(control, state);
    }
}

void ActivityPanel::setHover(std::uint32_t id, bool hover) {
    for (auto &control : controls_) {
        if (control.id == id) {
            control.hover = hover;
            redraw(control);
        }
    }
}

std::optional<std::uint32_t> ActivityPanel::press(std::uint32_t hotspot,
                                                  int x, int y) {
    const SDL_Point point{x, y};
    for (auto &control : controls_) {
        const SDL_Rect bounds = measure(control);
        if (!SDL_PointInRect(&point, &bounds) || control.id != hotspot)
            continue;
        if (control.type == Type::Push) {
            // 0x00406c50.
            if (control.state != 0) continue;
            setControlState(control, 1);
            return std::nullopt;
        }
        if (control.type == Type::Radio) {
            // 0x00406c80: the group (except disabled controls, state 2)
            // drops to 0 before this control turns on (0x00406b00).
            if (control.state != 0) continue;
            for (auto &member : controls_) {
                if (member.group == control.group && member.state != 2)
                    setControlState(member, 0);
            }
            setControlState(control, 1);
            return control.id;
        }
        // 0x00406be0.
        if (control.state < 0 || control.state > 1) continue;
        setControlState(control, control.state == 0 ? 1 : 0);
        return control.id;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ActivityPanel::release() {
    for (auto &control : controls_) {
        if (control.type != Type::Push || control.state != 1) continue;
        // 0x00406d40.
        setControlState(control, 0);
        if (control.hover) return control.id;
    }
    return std::nullopt;
}

ActivityList::ActivityList(ActivityStage &stage) : stage_(stage) {
    rowSprites_.reserve(32);
    for (int row = 0; row < 32; ++row) {
        // 0x004086d0: row descriptors are keyed and carry no hotspot.
        rowSprites_.push_back(stage_.create({}, 0, 0, 0, true));
    }
    highlight_ = stage_.create({}, 0, 0, 0, false);
}

void ActivityList::fill(SDL_Rect rect, std::vector<Item> items, int layer,
                        std::uint32_t colour) {
    clear();
    rect_ = rect;
    layer_ = layer;
    colour_ = colour;
    items_ = std::move(items);
    top_ = 0;
    selected_ = 0;
    filled_ = true;
    if (items_.empty()) return;
    // 0x00408160: the first item's height is the row pitch; at most 32 rows.
    rowHeight_ = std::max(1, items_.front().surface->h);
    visible_ = std::min(rect_.h / rowHeight_, 32);
    rebuildHighlight();
    showRows(top_, visible_);
    placeHighlight();
}

void ActivityList::clear() {
    if (!filled_) return;
    // 0x004083c0 -> 0x00408450: rows, then the highlight (0x00407be0);
    // 0x00407e20 removes the highlight again if it is still shown.
    hideRows(top_, visible_);
    stage_.removeIfActive(highlight_);
    items_.clear();
    top_ = 0;
    selected_ = 0;
    visible_ = 0;
    filled_ = false;
}

void ActivityList::rebuildHighlight() {
    // 0x00407db0.
    stage_.removeIfActive(highlight_);
    if (items_.empty()) return;
    SurfacePtr surface = createSurface(rect_.w, rowHeight_);
    // COLORREF 0x00BBGGRR.
    const std::uint32_t rgb = ((colour_ & 0xffU) << 16U) | (colour_ & 0xff00U) |
                              ((colour_ >> 16U) & 0xffU);
    fillSurface(surface.get(), nullptr, rgb);
    stage_.setSurface(highlight_, std::move(surface));
}

void ActivityList::setColour(std::uint32_t colour) {
    colour_ = colour;
    rebuildHighlight();
}

void ActivityList::showRows(int first, int count) {
    // 0x00407c00: rows of [first, first+count) that are in view.
    const int from = std::max(first, top_);
    const int to = std::min(first + count, top_ + visible_);
    for (int index = from; index < to && index < this->count(); ++index) {
        const int row = index - top_;
        const int sprite = rowSprites_[static_cast<std::size_t>(row)];
        stage_.removeIfActive(sprite);
        auto &target = stage_.sprite(sprite);
        // The row shares the item's surface.
        SDL_Surface *source = items_[static_cast<std::size_t>(index)].surface.get();
        SurfacePtr copy(SDL_DuplicateSurface(source));
        stage_.setSurface(sprite, std::move(copy));
        target.keyed = true;
        stage_.add(sprite, rect_.x, rect_.y + row * rowHeight_, layer_ + 1);
    }
}

void ActivityList::hideRows(int first, int count) {
    // 0x004082a0.
    const int from = std::max(first, top_);
    const int to = std::min(first + count, top_ + visible_);
    for (int index = from; index < to; ++index) {
        stage_.removeIfActive(rowSprites_[static_cast<std::size_t>(index - top_)]);
    }
}

void ActivityList::placeHighlight() {
    // 0x00407b60.
    if (count() > 0 && selected_ >= top_ && selected_ < top_ + visible_) {
        const int y = (selected_ - top_) * rowHeight_ + rect_.y;
        if (stage_.active(highlight_)) {
            stage_.move(highlight_, rect_.x, y);
        } else {
            stage_.add(highlight_, rect_.x, y, layer_);
        }
        return;
    }
    stage_.removeIfActive(highlight_);
}

void ActivityList::select(int index) {
    // 0x00407e60.
    if (index < 0) {
        index = 0;
    } else if (index > count() - 1) {
        index = count() - 1;
    }
    selected_ = index;
    if (top_ <= index) {
        if (index < visible_ + top_) {
            placeHighlight();
            return;
        }
        if (top_ < index) index += 1 - visible_;
    }
    scrollTo(index);
}

void ActivityList::scrollTo(int top) {
    // 0x00407af0.
    if (count() <= 0) return;
    top = std::clamp(top, 0, count() - 1);
    const int overflow = count() - visible_ - top;
    if (overflow < 0) top += overflow;
    if (top < 1) top = 0;
    if (top == top_) return;
    // 0x00407b40 re-adds every row in view, then the highlight.
    top_ = top;
    showRows(top_, visible_);
    placeHighlight();
}

int ActivityList::hit(int x, int y) {
    // 0x00407ec0 -> 0x00407f00.
    const SDL_Point point{x, y};
    if (!SDL_PointInRect(&point, &rect_) || count() <= 0) return -1;
    const int index = (y - rect_.y) / rowHeight_ + top_;
    if (index < 0 || index >= count()) return -1;
    if (selected_ != index) select(index);
    return index;
}

void ActivityList::moveBy(int rows) {
    // 0x00407ab0.
    if (count() <= 0) return;
    const int index = std::clamp(selected_ + rows, 0, count() - 1);
    if (index != selected_) select(index);
}

int ActivityList::key(SDL_Keycode key) {
    // 0x00407f60.
    if (count() <= 0) return -1;
    switch (key) {
    case SDLK_PAGEUP: scrollTo(top_ - visible_); return selected_;
    case SDLK_PAGEDOWN: scrollTo(top_ + visible_); return selected_;
    case SDLK_END: select(count() - 1); return selected_;
    case SDLK_HOME: select(0); return selected_;
    case SDLK_UP: moveBy(-1); return selected_;
    case SDLK_DOWN: moveBy(1); return selected_;
    default: return -1;
    }
}

int ActivityList::selected() const {
    return selected_ >= 0 && selected_ < count() ? selected_ : -1;
}

int ActivityList::parameter(int index) const {
    if (index < 0 || index >= count()) return 0;
    return items_[static_cast<std::size_t>(index)].parameter;
}

int ActivityList::indexOf(int parameter) const {
    for (int index = 0; index < count(); ++index) {
        if (items_[static_cast<std::size_t>(index)].parameter == parameter)
            return index;
    }
    return -1;
}

ActivityScrollbar::ActivityScrollbar(ActivityStage &stage,
                                     ActivityPanel &panel)
    : stage_(stage), panel_(panel) {}

void ActivityScrollbar::create(std::uint32_t id, SDL_Rect track,
                               SurfacePtr thumb, std::uint32_t thumbHotspot,
                               int layer, bool vertical,
                               std::string thumbName) {
    // 0x004090e0.
    id_ = id;
    track_ = track;
    layer_ = layer;
    vertical_ = vertical;
    thumbHotspot_ = thumbHotspot;
    // The thumb descriptor at +0x38 is not keyed (flags & 1 at +0x80).
    thumb_ = stage_.create(std::move(thumbName), 0, 0, thumbHotspot, false);
    stage_.setSurface(thumb_, std::move(thumb));
    setRange(0, 0);
    setValue(0);
}

void ActivityScrollbar::addButton(int x, int y, std::string normal,
                                  std::string pressed, bool up,
                                  std::uint32_t hoverId) {
    // 0x004091f0: button ids count up from 2000 (0x004a6354).
    static_assert(sizeof(std::uint32_t) == 4);
    std::uint32_t id = 2000U;
    for (const auto &control : panel_.controls()) {
        if (control.id >= 2000U) id = std::max(id, control.id + 1U);
    }
    panel_.addControl(id, x, y, {std::move(normal), std::move(pressed), {}, {}},
                      ActivityPanel::Type::Push, 0, {}, hoverId);
    (up ? upId_ : downId_) = id;
}

void ActivityScrollbar::setRange(int minimum, int maximum) {
    minimum_ = minimum;
    maximum_ = std::max(minimum, maximum);
    range_ = maximum_ - minimum_;
}

bool ActivityScrollbar::place(int position) {
    // 0x00408f30.
    if (!stage_.active(thumb_)) stage_.add(thumb_, -1, -1, layer_);
    const int previous = value_;
    position = std::max(position, 0);
    int centreX = 0;
    int centreY = 0;
    double value = 0.0;
    if (!vertical_) {
        position = std::clamp(position, track_.x, track_.x + track_.w);
        centreX = position;
        centreY = track_.h / 2 + track_.y;
        value = track_.w > 0 ? static_cast<double>((position - track_.x) * range_) /
                                   track_.w
                             : 0.0;
    } else {
        position = std::clamp(position, track_.y, track_.y + track_.h);
        centreX = track_.w / 2 + track_.x;
        centreY = position;
        value = track_.h > 0 ? static_cast<double>((position - track_.y) * range_) /
                                   track_.h
                             : 0.0;
    }
    value_ = roundAway(value);
    stage_.moveCentre(thumb_, centreX, centreY);
    return value_ != previous;
}

bool ActivityScrollbar::setValue(int value) {
    // 0x00408eb0.
    value = std::clamp(value, minimum_, maximum_);
    if (range_ <= 0) return place(0);
    if (!vertical_) return place(track_.w * value / range_ + track_.x);
    return place(track_.h * value / range_ + track_.y);
}

bool ActivityScrollbar::dragTo(int position) { return place(position); }

bool ActivityScrollbar::pressTrack(int x, int y, bool &dragging) {
    // 0x004092d0: the track widened by the thumb, or the thumb itself.
    dragging = false;
    const SDL_Rect thumbRect = stage_.rect(thumb_);
    const int thumbWidth = thumbRect.w;
    const SDL_Rect track{track_.x - thumbWidth / 2, track_.y,
                         track_.w + thumbWidth, track_.h};
    const SDL_Point point{x, y};
    if (!SDL_PointInRect(&point, &track) &&
        !(stage_.active(thumb_) && SDL_PointInRect(&point, &thumbRect)))
        return false;
    dragging = true;
    return place(vertical_ ? y : x);
}

bool ActivityScrollbar::clickButton(std::uint32_t id) {
    if (id == upId_) return setValue(value_ - step_);
    if (id == downId_) return setValue(value_ + step_);
    return false;
}

ActivityTooltip::ActivityTooltip(ActivityStage &stage, const BitmapFont &font,
                                 SurfacePtr frame, SurfacePtr dot)
    : stage_(stage), font_(font), frame_(std::move(frame)),
      dot_(std::move(dot)) {
    // 0x004a63a0 is a keyed static descriptor with no name.
    sprite_ = stage_.create({}, 0, 0, 0, true);
}

void ActivityTooltip::hide() {
    // 0x00409bc0.
    stage_.removeIfActive(sprite_);
}

void ActivityTooltip::hotspotChanged(std::uint32_t hotspot, double now) {
    // 0x00409e90.
    hide();
    shown_ = pending_;
    pending_ = hotspot;
    armedAt_ = now;
}

void ActivityTooltip::tick(double now, const SDL_Rect &cursor) {
    // 0x00409ed0: after 1500 ms (0x0053dacc) event 0x11a shows the text.
    if (pending_ == shown_ || now < armedAt_ + 1.5) return;
    shown_ = pending_;
    const auto text = table_.find(static_cast<std::uint32_t>(pending_));
    if (text == table_.end() || text->second.empty() || !frame_) return;
    // 0x00409d00: text size + (12, 18), nine-sliced from tooltip.bmp with
    // the centre rectangle at 0x0049c790, text centred.
    const int textW = textWidth(font_, text->second);
    const int textH = font_.height;
    const int width = textW + 12;
    const int height = textH + 18;
    SurfacePtr box = nineSlice(width, height, frame_.get(),
                               SDL_Rect{6, 11, 38 - 6, 15 - 11});
    const SDL_Point at = centreIn(SDL_Rect{0, 0, width, height}, textW, textH);
    drawFontText(box.get(), font_, text->second, at.x, at.y);
    // 0x00409c00: below the cursor sprite, kept on the 800x600 screen.
    int x = cursor.x;
    int y = cursor.y + cursor.h;
    if (800 < x + width) x = 800 - width;
    if (600 < y + height) y = cursor.y - height;
    stage_.setSurface(sprite_, std::move(box));
    stage_.add(sprite_, x, y, 1500);
}

} // namespace fgp
