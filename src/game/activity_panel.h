#pragma once

// Native ports of the shared FGP.exe UI pieces the activity scenes build on:
// the SCENE descriptor list as those scenes use it, the control panel
// (0x00406540 family), the list control (0x004085a0 family), the scroll
// control (0x004090e0 family) and the tooltip (0x00409b50 family).
//
// Every SCENE_AddAnim (0x00412840) and SCENE_RemoveAnim (0x00412aa0) the
// original makes through these pieces is emitted to the reference trace with
// the same arguments, so captures compare descriptor for descriptor.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/bitmap_font.h"
#include "render/sdl_util.h"

namespace fgp {

class ReferenceTrace;

// One 0xa8-byte SCENE descriptor. Runtime descriptors (0x004078c0) have an
// empty name, which is what the trace shows for them.
struct StageSprite {
    std::string name;
    int defaultX{};
    int defaultY{};
    std::uint32_t hotspot{};
    // +0x48: draw with a source colour key taken from the surface's top-left
    // pixel (scene_DrawAnim 0x00413050).
    bool keyed{};
    SurfacePtr surface;
    TexturePtr texture;
    bool textureDirty{};
    bool active{};
    int x{};
    int y{};
    int layer{};
    std::uint64_t order{};
};

class ActivityStage {
public:
    ActivityStage(SDL_Renderer *renderer, ReferenceTrace &trace);

    int create(std::string name = {}, int defaultX = 0, int defaultY = 0,
               std::uint32_t hotspot = 0, bool keyed = false);
    StageSprite &sprite(int id) { return sprites_.at(static_cast<std::size_t>(id)); }
    const StageSprite &sprite(int id) const {
        return sprites_.at(static_cast<std::size_t>(id));
    }
    void setSurface(int id, SurfacePtr surface);
    // Marks a surface drawn into after setSurface for re-upload.
    void touch(int id);
    // SCENE_AddAnim 0x00412840: (-1,-1) selects the descriptor's default
    // position. The trace records the arguments as passed.
    void add(int id, int x, int y, int layer);
    // SCENE_RemoveAnim 0x00412aa0; callers test active() first, as the
    // original's callers test +0x7c.
    void remove(int id);
    void removeIfActive(int id) {
        if (active(id)) remove(id);
    }
    // 0x00412cc0 (untraced).
    void move(int id, int x, int y);
    // 0x00412cf0: centre the descriptor on (x, y).
    void moveCentre(int id, int x, int y);
    // 0x00412d50 (untraced).
    void setLayer(int id, int layer);
    bool active(int id) const { return sprite(id).active; }
    // 0x00413400: the descriptor's rectangle.
    SDL_Rect rect(int id) const;
    // The descriptor half of 0x004136c0: the topmost active descriptor with
    // a hotspot under the point (rectangle test, or a non-key pixel for keyed
    // descriptors). Returns nullopt when no descriptor claims the point.
    std::optional<std::uint32_t> hotspotAt(int x, int y,
                                           std::uint32_t ignoredHotspot = 0) const;
    void render();

private:
    SDL_Renderer *renderer_;
    ReferenceTrace &trace_;
    std::vector<StageSprite> sprites_;
    std::uint64_t nextOrder_{};
};

// Software drawing helpers on ARGB8888 surfaces.
SurfacePtr createSurface(int width, int height);
void fillSurface(SDL_Surface *surface, const SDL_Rect *rect, std::uint32_t rgb);
// Blits a bitmap-font string (Windows-1252) at (x, y).
void drawFontText(SDL_Surface *target, const BitmapFont &font,
                  std::string_view text, int x, int y);
// The paragraph layout of 0x00402750: 0x00402530 breaks a line once its
// width reaches maximumWidth (or at a newline), back at the last word end,
// and the lines are font.height apart (the font's line spacing +0x1024 is
// never set). Returns each line's start and length; a word wider than the
// width ends the paragraph there, as the original's zero-length break does.
struct ParagraphLine {
    std::size_t start{};
    std::size_t length{};
};
std::vector<ParagraphLine> breakParagraph(const BitmapFont &font,
                                          std::string_view text,
                                          int maximumWidth);
// Draws the paragraph from (x, y) and returns its height (0x00402670).
int drawParagraph(SDL_Surface *target, const BitmapFont &font,
                  std::string_view text, int x, int y, int maximumWidth);
// 0x00420b00: a width x height surface built from source, whose centre
// rectangle (left, top, right, bottom) is tiled over the middle and whose
// edges and corners are tiled around it (0x00420dc0).
SurfacePtr nineSlice(int width, int height, SDL_Surface *source,
                     const SDL_Rect &centre);
// 0x004124e0 with flags 0x101: centre (w, h) in rect, C division.
SDL_Point centreIn(const SDL_Rect &rect, int width, int height);

// The control panel at 0x00533688, one 0x278-byte slot per control.
class ActivityPanel {
public:
    // Push button (0x00406ed0), radio button (0x00406fb0) and toggle
    // (0x00407000): the control type at +8.
    enum class Type { Push = 1, Radio = 2, Toggle = 3 };
    struct Control {
        std::uint32_t id{};
        Type type{Type::Push};
        int group{};
        int x{};
        int y{};
        std::array<std::string, 4> images;
        int state{};
        bool hover{};
        int width{};
        int height{};
        int sprite{-1};
        int captionSprite{-1};
        int layer{};
    };
    using ImageLoader = std::function<SurfacePtr(std::string_view)>;

    ActivityPanel(ActivityStage &stage, ImageLoader loader);

    // 0x00406ec0 / 0x004068a0: layer and caption font for new controls.
    void setLayer(int layer) { layer_ = layer; }
    void setCaptionFont(const BitmapFont *font) { captionFont_ = font; }
    // 0x00406ed0 and its radio/toggle wrappers. hoverId is the hotspot under
    // the cursor at creation (0x0040ca40 + 0x00406dc0); keyed is the flag
    // argument, stored as the descriptor's +0x48 (0x00406540).
    void addControl(std::uint32_t id, int x, int y,
                    std::array<std::string, 4> images, Type type = Type::Push,
                    int group = 0, std::string caption = {},
                    std::uint32_t hoverId = 0, bool keyed = false);
    // 0x00406720.
    void setState(std::uint32_t id, int state);
    // 0x00406700: -1 for a missing control.
    int state(std::uint32_t id) const;
    // 0x00406ac0: every control of the group.
    void setGroupState(int group, int state);
    // 0x00406dc0 for event 0x113 (true) / 0x114 (false).
    void setHover(std::uint32_t id, bool hover);
    // 0x00406b50 on event 0x105. Radio and toggle controls click on the
    // press (event 0x116); the id is returned.
    std::optional<std::uint32_t> press(std::uint32_t hotspot, int x, int y);
    // 0x00406cf0 on event 0x106: a pressed push button clicks if hovered.
    std::optional<std::uint32_t> release();
    const std::vector<Control> &controls() const { return controls_; }

private:
    Control *find(std::uint32_t id);
    const Control *find(std::uint32_t id) const;
    void setControlState(Control &control, int state);
    void redraw(Control &control);
    SDL_Rect measure(Control &control);

    ActivityStage &stage_;
    ImageLoader loader_;
    std::vector<Control> controls_;
    int layer_{};
    const BitmapFont *captionFont_{};
};

// The list control (0x004085a0). Items are surfaces with a parameter.
class ActivityList {
public:
    struct Item {
        SurfacePtr surface;
        int parameter{};
    };
    explicit ActivityList(ActivityStage &stage);

    // 0x004085a0 (+0x00408160, 0x00407db0, 0x00407b40).
    void fill(SDL_Rect rect, std::vector<Item> items, int layer,
              std::uint32_t colour);
    // 0x00408700.
    void clear();
    // 0x00407d90.
    void setColour(std::uint32_t colour);
    // 0x00407e60.
    void select(int index);
    // 0x00407af0.
    void scrollTo(int top);
    // 0x00407ec0: selects and returns the item under the point, or -1.
    int hit(int x, int y);
    // 0x00407f60 for the list keys; returns the new selection or -1.
    int key(SDL_Keycode key);
    // 0x00408850.
    int selected() const;
    // 0x004088c0.
    int parameter(int index) const;
    int count() const { return static_cast<int>(items_.size()); }
    int top() const { return top_; }
    int visibleRows() const { return visible_; }
    bool filled() const { return filled_; }
    // Finds the index holding the parameter (0x0045dcc0).
    int indexOf(int parameter) const;

private:
    void rebuildHighlight();
    void showRows(int first, int count);
    void hideRows(int first, int count);
    void placeHighlight();
    void moveBy(int rows);

    ActivityStage &stage_;
    SDL_Rect rect_{};
    std::vector<Item> items_;
    std::vector<int> rowSprites_;
    int highlight_{-1};
    int layer_{};
    std::uint32_t colour_{};
    int rowHeight_{};
    int visible_{};
    int top_{};
    int selected_{};
    bool filled_{};
};

// The scroll control (0x004090e0) with its up/down push buttons (0x004091f0).
class ActivityScrollbar {
public:
    ActivityScrollbar(ActivityStage &stage, ActivityPanel &panel);

    // thumbName is the descriptor name 0x004090e0 copies to +0x38.
    void create(std::uint32_t id, SDL_Rect track, SurfacePtr thumb,
                std::uint32_t thumbHotspot, int layer, bool vertical,
                std::string thumbName = "scrolltab");
    void addButton(int x, int y, std::string normal, std::string pressed,
                   bool up, std::uint32_t hoverId);
    void setStep(int step) { step_ = step; }
    // 0x00408e20.
    void setRange(int minimum, int maximum);
    // 0x00408eb0; returns true when the value changed (event 0x117).
    bool setValue(int value);
    // 0x00408f30 for a pointer coordinate; true when the value changed.
    bool dragTo(int position);
    // 0x004092d0 on event 0x105: starts a drag when the track or thumb is
    // hit; returns true when the value changed.
    bool pressTrack(int x, int y, bool &dragging);
    // 0x00409450 on event 0x116; true when the value changed.
    bool clickButton(std::uint32_t id);
    int value() const { return value_; }
    std::uint32_t id() const { return id_; }
    std::uint32_t thumbHotspot() const { return thumbHotspot_; }
    int thumbSprite() const { return thumb_; }
    bool vertical() const { return vertical_; }

private:
    bool place(int position);

    ActivityStage &stage_;
    ActivityPanel &panel_;
    std::uint32_t id_{};
    SDL_Rect track_{};
    int thumb_{-1};
    std::uint32_t thumbHotspot_{};
    int layer_{};
    bool vertical_{true};
    int minimum_{};
    int maximum_{};
    int range_{};
    int value_{};
    int step_{1};
    std::uint32_t upId_{};
    std::uint32_t downId_{};
};

// The tooltip (0x00409b50): text appears 1500 ms after the hotspot under the
// cursor changes (events 0x113, 0x112, 0x11a) and a mouse press hides it.
class ActivityTooltip {
public:
    ActivityTooltip(ActivityStage &stage, const BitmapFont &font,
                    SurfacePtr frame, SurfacePtr dot);
    void setTable(std::map<std::uint32_t, std::string> table) {
        table_ = std::move(table);
    }
    void hotspotChanged(std::uint32_t hotspot, double now);
    // cursor: the cursor sprite's rectangle (0x004135e0).
    void tick(double now, const SDL_Rect &cursor);
    void hide();

private:
    ActivityStage &stage_;
    const BitmapFont &font_;
    SurfacePtr frame_;
    SurfacePtr dot_;
    int sprite_{-1};
    std::map<std::uint32_t, std::string> table_;
    std::int64_t pending_{-1};
    std::int64_t shown_{0};
    double armedAt_{};
};

} // namespace fgp
