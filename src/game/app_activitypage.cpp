#include "game/app_activitypage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <tuple>
#include <stdexcept>

#include "assets/profile.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {
namespace {

// Rectangles the module passes around, left/top/right/bottom in FGP.exe.
constexpr SDL_Rect kTitleRect{205, 23, 581 - 205, 73 - 23};      // 0x0049f668
constexpr SDL_Rect kPageRect{10, 90, 543 - 10, 585 - 90};        // 0x0049f678
constexpr SDL_Rect kSpinnerRect{606, 392, 639 - 606, 427 - 392}; // 0x0049f688
constexpr SDL_Rect kListRect{566, 138, 768 - 566, 376 - 138};    // 0x0049f698
constexpr SDL_Rect kSwatchRect{566, 397, 594 - 566, 421 - 397};  // 0x0049f6a8

// Control ids (0x0045c8c0) and their hotspots.bmp regions.
constexpr std::uint32_t kHelp = 1;
constexpr std::uint32_t kExit = 2;
constexpr int kTabText = 10;
constexpr int kTabShape = 11;
constexpr int kTabPicture = 12;
constexpr std::uint32_t kScrollId = 0x14;
constexpr std::uint32_t kColour = 0x32;
constexpr std::uint32_t kSpinnerArea = 0x33;
constexpr std::uint32_t kSizeUp = 0x34;
constexpr std::uint32_t kSizeDown = 0x35;
constexpr std::uint32_t kEdit = 0x36;
constexpr std::uint32_t kBold = 0x37;
constexpr std::uint32_t kItalic = 0x38;
constexpr std::uint32_t kUnderline = 0x39;
constexpr std::uint32_t kAdd = 0x3c;
constexpr std::uint32_t kRemove = 0x3d;
constexpr std::uint32_t kClear = 0x3e;
constexpr std::uint32_t kLayout = 0x46;
constexpr std::uint32_t kSave = 0x47;
constexpr std::uint32_t kPrint = 0x48;
constexpr std::uint32_t kCloseHotspot = 100;  // descriptor 0x00502a10
constexpr std::uint32_t kResizeHotspot = 101; // descriptor 0x00502968

// Layers: 0x0045c050 and 0x0045c8c0.
constexpr int kLogoLayer = 0xc9;
constexpr int kTitleLayer = 0xca;
constexpr int kFrameLayer = 200;
constexpr int kPanelLayer = 0xfa;
constexpr int kSwatchLayer = 0xfb;
constexpr int kListLayer = 0xc9;     // 0x00408980(0xc9) in 0x0045c310
constexpr int kPaperLayer = -500;    // 0x0069852c = 0xfffffe0c
constexpr int kSelectionLayer = 0x32;
constexpr int kHandleLayer = 0x33;
constexpr std::uint32_t kListColour = 0x23234b;  // 0x0045c3da, COLORREF
constexpr std::uint32_t kListInitialColour = 0x3e5167;  // 0x004085a0
constexpr std::uint32_t kInitialColour = 0xff0000;      // 0x0045c1e1
constexpr int kInitialSize = 200;                       // 0x0045c1f5
constexpr int kEntryPicture = 0x80;
// 0x0049f6b8: the page's aspect ratio; 0x0049f6bc = 1/490, the library's
// large pictures being 490 pixels square.
constexpr float kPaperAspect = 0.7774616479873657F;
constexpr float kPictureScale = 0.0020408162381500006F;
// Shape polygons 0x005027d0/0x005027f0/0x00502840, point counts 0x005028ac.
constexpr std::uint32_t kShapePoints = 0x00502898;
constexpr std::uint32_t kShapeCounts = 0x005028ac;
constexpr std::uint32_t kShapeNames = 0x005027b8;
// The tooltip table handed to 0x00409b50 (id, language key, -1 ends).
constexpr std::uint32_t kTooltipTable = 0x00502768;
// The shared picture library (0x0041b3f0 family).
constexpr std::uint32_t kLibraryCategories = 0x004adc04;
constexpr std::uint32_t kLibraryPatterns = 0x004adc18;
constexpr std::uint32_t kLibraryBases = 0x004adc2c;
constexpr std::uint32_t kLibraryTotal = 0x004adc40;
constexpr std::uint32_t kAwardInitial = 0x0049ca60;
constexpr std::uint32_t kAwardCounts = 0x0049ca88;
constexpr std::uint32_t kAwardRemap = 0x0049cab0;
constexpr std::uint32_t kAwardNames = 0x004ad89c;

// 0x005028b8 is a static the scene never resets: the layer counter carries
// over from one visit to the next.
int gActivityLayerCounter = -400;

int roundAway(double value) {
    return static_cast<int>(value > 0.0 ? value + 0.5 : value - 0.5);
}

// COLORREF 0x00BBGGRR to 0x00RRGGBB.
std::uint32_t colourRgb(std::uint32_t colour) {
    return ((colour & 0xffU) << 16U) | (colour & 0xff00U) |
           ((colour >> 16U) & 0xffU);
}

std::uint32_t mapRgb(SDL_Surface *surface, std::uint32_t rgb) {
    return SDL_MapRGB(surface->format, (rgb >> 16U) & 0xffU,
                      (rgb >> 8U) & 0xffU, rgb & 0xffU);
}

void putPixel(SDL_Surface *surface, int x, int y, std::uint32_t value) {
    if (x < 0 || y < 0 || x >= surface->w || y >= surface->h) return;
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(surface->pixels) + y * surface->pitch);
    row[x] = value;
}

void drawLine(SDL_Surface *surface, int x0, int y0, int x1, int y1,
              std::uint32_t value) {
    // GDI LineTo excludes the end point; the polygon closes the loop.
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (x0 != x1 || y0 != y1) {
        putPixel(surface, x0, y0, value);
        const int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

// A surface keyed on 0xff00 green, as 0x00407880 fills new descriptors.
SurfacePtr greenSurface(int width, int height) {
    SurfacePtr surface = createSurface(width, height);
    fillSurface(surface.get(), nullptr, 0x00ff00U);
    return surface;
}

SurfacePtr toArgb(SDL_Surface *surface) {
    SurfacePtr converted(
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) throw std::runtime_error(SDL_GetError());
    SDL_SetSurfaceBlendMode(converted.get(), SDL_BLENDMODE_NONE);
    return converted;
}

// StretchBlt with HALFTONE (0x0045bc40): a box-filtered resample.
void stretchInto(SDL_Surface *source, SDL_Surface *target) {
    SurfacePtr from = toArgb(source);
    const int sw = from->w;
    const int sh = from->h;
    const int tw = target->w;
    const int th = target->h;
    for (int y = 0; y < th; ++y) {
        const double y0 = static_cast<double>(y) * sh / th;
        const double y1 = static_cast<double>(y + 1) * sh / th;
        for (int x = 0; x < tw; ++x) {
            const double x0 = static_cast<double>(x) * sw / tw;
            const double x1 = static_cast<double>(x + 1) * sw / tw;
            double r = 0.0, g = 0.0, b = 0.0, total = 0.0;
            for (int sy = static_cast<int>(y0);
                 sy < std::min(sh, static_cast<int>(std::ceil(y1))); ++sy) {
                const double wy = std::min<double>(sy + 1, y1) -
                                  std::max<double>(sy, y0);
                const auto *row = reinterpret_cast<const std::uint32_t *>(
                    static_cast<const std::uint8_t *>(from->pixels) +
                    sy * from->pitch);
                for (int sx = static_cast<int>(x0);
                     sx < std::min(sw, static_cast<int>(std::ceil(x1)));
                     ++sx) {
                    const double wx = std::min<double>(sx + 1, x1) -
                                      std::max<double>(sx, x0);
                    const double weight = wx * wy;
                    const std::uint32_t pixel = row[sx];
                    r += ((pixel >> 16U) & 0xffU) * weight;
                    g += ((pixel >> 8U) & 0xffU) * weight;
                    b += (pixel & 0xffU) * weight;
                    total += weight;
                }
            }
            if (total <= 0.0) continue;
            const auto channel = [&](double value) {
                return static_cast<std::uint32_t>(
                    std::clamp(roundAway(value / total), 0, 255));
            };
            putPixel(target, x, y,
                     0xff000000U | (channel(r) << 16U) | (channel(g) << 8U) |
                         channel(b));
        }
    }
}

std::uint32_t pixelAt(SDL_Surface *surface, int x, int y) {
    const auto *row = reinterpret_cast<const std::uint32_t *>(
        static_cast<const std::uint8_t *>(surface->pixels) + y * surface->pitch);
    return row[x];
}

} // namespace

SurfacePtr ActivitypageScene::activityBitmap(std::string_view name) {
    const std::string asset = "bitmap\\" + std::string(name) + ".bmp";
    for (Package *candidate :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional(asset))
            return toArgb(loadBmp(candidate->data(*entry)).get());
    }
    throw std::runtime_error("Activity bitmap not found: " + asset);
}

SurfacePtr ActivitypageScene::activityCommonBitmap(std::string_view path) {
    const std::string asset(path);
    for (Package *candidate :
         {addonCommon.get(), data2Common.get(), editionCommon.get(), &common}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional(asset))
            return toArgb(loadBmp(candidate->data(*entry)).get());
    }
    return {};
}

ActivitypageScene::ActivitypageGame ActivitypageScene::loadActivitypageGame() {
    ActivitypageGame game;
    game.stage = std::make_unique<ActivityStage>(renderer, referenceTrace);
    game.panel = std::make_unique<ActivityPanel>(
        *game.stage,
        [this](std::string_view name) { return activityBitmap(name); });
    game.list = std::make_unique<ActivityList>(*game.stage);
    game.scrollbar =
        std::make_unique<ActivityScrollbar>(*game.stage, *game.panel);
    SurfacePtr tooltipFrame = activityCommonBitmap("bitmap\\tooltip.bmp");
    SurfacePtr tooltipDot = activityCommonBitmap("bitmap\\tooldot.bmp");
    game.tooltip = std::make_unique<ActivityTooltip>(
        *game.stage, dialogFont, std::move(tooltipFrame),
        std::move(tooltipDot));
    std::map<std::uint32_t, std::string> tips;
    for (std::uint32_t entry = kTooltipTable;; entry += 8U) {
        const auto id = static_cast<std::int32_t>(executableGameData.u32At(entry));
        if (id < 0) break;
        const std::string key =
            executableGameData.stringAt(executableGameData.u32At(entry + 4U));
        const auto colon = key.find(':');
        tips[static_cast<std::uint32_t>(id)] =
            language.get(key.substr(0, colon), key.substr(colon + 1), "");
    }
    game.tooltip->setTable(std::move(tips));

    // Static descriptors: logo (keyed, +0x48 at 0x00502908) and the frame
    // pieces at 0x00502b60..0x00502e00.
    game.logo = game.stage->create("logo", 206, 29, 0, true);
    game.stage->setSurface(game.logo, activityBitmap("logo"));
    game.title = game.stage->create({}, 0, 0, 0, true);
    const std::array<std::tuple<const char *, int, int>, 5> frame{{
        {"top", 0, 0}, {"left", 0, 90}, {"right", 544, 90},
        {"bottomleft", 0, 554}, {"bottom", 50, 586}}};
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const auto &[name, x, y] = frame[index];
        game.frame[index] = game.stage->create(name, x, y, 0, false);
        game.stage->setSurface(game.frame[index], activityBitmap(name));
    }
    game.paper = game.stage->create({}, 0, 0, 0, true);
    game.swatch = game.stage->create({}, 0, 0, 0, false);
    game.spinnerSprite = game.stage->create({}, 0, 0, 0, true);
    game.selection = game.stage->create({}, 0, 0, 0, true);
    game.closeHandle = game.stage->create("close", 0, 0, kCloseHotspot, false);
    game.stage->setSurface(game.closeHandle, activityBitmap("close"));
    game.resizeHandle =
        game.stage->create("resize", 0, 0, kResizeHotspot, false);
    game.stage->setSurface(game.resizeHandle, activityBitmap("resize"));
    for (auto &object : game.objects) {
        object.sprite = game.stage->create({}, 0, 0, 0, false);
    }

    // Native substitute for 0x0045c4b0 (see REVERSE_ENGINEERING.md): the
    // original lists the host's TrueType fonts (EnumFontFamiliesA); native
    // lists the game's own bitmap fonts, sorted by name as 0x0048d931 does.
    std::vector<std::string> fontNames;
    for (const auto &entry : common.entries()) {
        std::string name = entry.name;
        std::replace(name.begin(), name.end(), '/', '\\');
        constexpr std::string_view prefix = "fonts\\";
        if (name.rfind(prefix, 0) != 0 || name.size() < prefix.size() + 5)
            continue;
        fontNames.push_back(name.substr(prefix.size(),
                                        name.size() - prefix.size() - 4));
    }
    std::sort(fontNames.begin(), fontNames.end());
    for (const auto &name : fontNames) {
        try {
            ActivityFont font;
            font.name = name;
            font.font = loadBitmapFont(
                renderer, common.data(common.find("fonts\\" + name + ".bmp")));
            game.fonts.push_back(std::move(font));
        } catch (const std::exception &) {
            // Strips with fewer than 256 glyphs (Counter) are not text fonts.
        }
    }

    if (pendingActivityEntry) {
        game.entryScene = pendingActivityEntry->scene;
        game.entryMode = pendingActivityEntry->mode;
        game.entryPicture = std::move(pendingActivityEntry->picture);
        pendingActivityEntry.reset();
    }
    std::cout << "Loaded executable-derived Activity page: 32 object slots, "
                 "text/shape/picture tabs, selection frame with close and "
                 "resize handles, spinner, portrait/landscape paper, "
                 "picture library list and control panel traces\n";
    return game;
}

std::uint32_t ActivitypageScene::activityHotspotAt(int x, int y) {
    // 0x004136c0: descriptors with a hotspot first, then hotspots.bmp. A
    // dragged descriptor's own hotspot is ignored.
    auto &game = *activitypageGame;
    std::uint32_t ignored = 0;
    if (game.drag != ActivityDrag::None && game.dragSprite >= 0)
        ignored = game.stage->sprite(game.dragSprite).hotspot;
    if (const auto hit = game.stage->hotspotAt(x, y, ignored)) return *hit;
    if (!sceneHotspots) return 0;
    return pixelValue(sceneHotspots.get(), x, y);
}

void ActivitypageScene::activityHotspotChanged(std::uint32_t hotspot) {
    // 0x0040c160: event 0x114 for the old hotspot, 0x113 for the new one.
    auto &game = *activitypageGame;
    if (hotspot == game.hotspot) return;
    const std::uint32_t previous = game.hotspot;
    game.hotspot = hotspot;
    game.panel->setHover(previous, false);
    game.panel->setHover(hotspot, true);
    game.tooltip->hotspotChanged(hotspot, sceneElapsedNow());
}

// Scene initialisation 0x0045c050.
void ActivitypageScene::announceActivitypageEntry() {
    auto &game = *activitypageGame;
    game.announced = true;
    auto &stage = *game.stage;
    // 0x0045c050 clears every slot and the selection.
    game.selected = -1;
    game.tab = 0;
    stage.add(game.logo, -1, -1, kLogoLayer);
    // 0x0040ce70(0x101): the scene title, Amerigo BT_18_, centred in the
    // rectangle at 0x0049f668 (0x00407a30).
    {
        const std::string title =
            language.get("activity", "SCENENAME", "Aktivitetssenteret");
        const int width = textWidth(menuTitleFont, title);
        const int height = menuTitleFont.height;
        SurfacePtr label = greenSurface(width, height);
        drawFontText(label.get(), menuTitleFont, title, 0, 0);
        stage.setSurface(game.title, std::move(label));
        const SDL_Point at = centreIn(kTitleRect, width, height);
        stage.add(game.title, at.x, at.y, kTitleLayer);
    }
    for (const int piece : game.frame) stage.add(piece, -1, -1, kFrameLayer);

    // 0x0045c8c0: the control panel at layer 250, captions in Amerigo 14.
    auto &panel = *game.panel;
    const std::uint32_t hover = activityHotspotAt(mouseX, mouseY);
    game.hotspot = hover;
    using Type = ActivityPanel::Type;
    panel.setLayer(kPanelLayer);
    panel.setCaptionFont(&calendarFont);
    const auto caption = [&](const char *key) {
        return language.get("activity", key, key);
    };
    panel.addControl(kHelp, 0, 0, {"help_0", "help_1", "", ""}, Type::Push, 0,
                     {}, hover, true);
    panel.addControl(kExit, 0x2d8, 0, {"exit_0", "exit_1", "", ""},
                     Type::Push, 0, {}, hover, true);
    panel.addControl(kTabText, 0x232, 0x57, {"", "text_1", "", ""},
                     Type::Radio, 1, {}, hover);
    panel.addControl(kTabShape, 0x27c, 0x57, {"", "shape_1", "", ""},
                     Type::Radio, 1, {}, hover);
    panel.addControl(kTabPicture, 0x2c8, 0x57, {"", "image_1", "", ""},
                     Type::Radio, 1, {}, hover);
    panel.addControl(kColour, 0x230, 0x185, {"", "color_1", "", ""},
                     Type::Push, 0, {}, hover);
    panel.addControl(kSizeUp, 0x27f, 0x187, {"", "sizeinc_1", "", ""},
                     Type::Push, 0, {}, hover);
    panel.addControl(kSizeDown, 0x27f, 0x19a, {"", "sizedec_1", "", ""},
                     Type::Push, 0, {}, hover);
    panel.addControl(kEdit, 0x29a, 0x185, {"", "edit_1", "", ""}, Type::Push,
                     0, {}, hover);
    panel.addControl(kBold, 0x2ce, 0x189, {"", "bold_1", "", ""},
                     Type::Toggle, 0, {}, hover);
    panel.addControl(kItalic, 0x2e6, 0x189, {"", "italic_1", "", ""},
                     Type::Toggle, 0, {}, hover);
    panel.addControl(kUnderline, 0x2fe, 0x189, {"", "underline_1", "", ""},
                     Type::Toggle, 0, {}, hover);
    panel.addControl(kAdd, 0x25d, 0x1ba, {"", "add_1", "", ""}, Type::Push, 0,
                     caption("ADD"), hover);
    panel.addControl(kRemove, 0x25d, 0x1dc, {"", "remove_1", "", ""},
                     Type::Push, 0, caption("REMOVE"), hover);
    panel.addControl(kClear, 0x25d, 0x1fe, {"", "clear_1", "", ""},
                     Type::Push, 0, caption("CLEAR"), hover);
    panel.addControl(kLayout, 9, 0x231, {"", "layout_1", "", ""},
                     Type::Toggle, 0, {}, hover);
    panel.addControl(kPrint, 0x232, 0x229, {"", "print_1", "", ""},
                     Type::Push, 0, caption("PRINT"), hover);
    panel.addControl(kSave, 0x2a5, 0x229, {"", "save_1", "", ""}, Type::Push,
                     0, caption("SAVE"), hover);
    // 0x0045cbc1-0x0045cc0d: scroll control 0x14 at x 0x307, track y 0xab
    // height 0xab, then its up/down buttons and step 1.
    game.scrollbar->create(kScrollId, SDL_Rect{0x307, 0xab, 0xe, 0xab},
                           activityBitmap("scrolltab"), kScrollId,
                           kPanelLayer, true);
    game.scrollbar->addButton(0x307, 0x8d, "up_0", "up_1", true, hover);
    game.scrollbar->addButton(0x307, 0x165, "down_0", "down_1", false, hover);
    game.scrollbar->setStep(1);

    // 0x0045c1c3-0x0045c1d2: the colour swatch, opaque (0x00407830(1)).
    stage.setSurface(game.swatch, createSurface(kSwatchRect.w, kSwatchRect.h));
    stage.add(game.swatch, kSwatchRect.x, kSwatchRect.y, kSwatchLayer);
    setActivityColour(kInitialColour);
    // 0x0045c250: the spinner, value 200.
    game.spinner = ActivitySpinner{};
    game.spinner.value = kInitialSize;
    stage.setSurface(game.spinnerSprite,
                     greenSurface(kSpinnerRect.w, kSpinnerRect.h));
    stage.add(game.spinnerSprite, kSpinnerRect.x, kSpinnerRect.y, kPanelLayer);
    drawActivitySpinner(game.spinner.value);
    gActivityLayerCounter = std::clamp(gActivityLayerCounter, -10000, 10000);
    game.layerCounter = gActivityLayerCounter;
    setActivityLayout(1);
    panel.setState(kTabPicture, 1);
    setActivityTab(kTabPicture);
    if (game.entryPicture) addActivityPictureObject(kEntryPicture);
}

void ActivitypageScene::setActivityTab(int tab) {
    // 0x0045c310.
    auto &game = *activitypageGame;
    if (game.tab == tab) return;
    game.list->clear();
    std::vector<ActivityList::Item> items;
    if (tab == kTabText) {
        // Rows are the font name in its own face, at least 200x20
        // (0x00408470 / 0x004084b0).
        for (std::size_t index = 0; index < game.fonts.size(); ++index) {
            const auto &font = game.fonts[index];
            const int width = std::max(textWidth(font.font, font.name), 200);
            const int height = std::max(font.font.height, 20);
            SurfacePtr row = greenSurface(width, height);
            drawFontText(row.get(), font.font, font.name, 0, 0);
            items.push_back({std::move(row), static_cast<int>(index)});
        }
        game.list->fill(kListRect, std::move(items), kListLayer,
                        kListInitialColour);
        setActivitySpinnerRange(6, 0x2a);
    } else if (tab == kTabShape) {
        // 0x0045c650: 200x34 rows, figure_N icon at (1,1), name at (0x28,8).
        for (int index = 0; index < 5; ++index) {
            SurfacePtr row = greenSurface(200, 0x22);
            SurfacePtr icon = activityBitmap("figure_" + std::to_string(index + 1));
            SDL_Rect at{1, 1, icon->w, icon->h};
            SDL_BlitSurface(icon.get(), nullptr, row.get(), &at);
            const std::string key = executableGameData.stringAt(
                executableGameData.u32At(kShapeNames + 4U * index));
            const auto colon = key.find(':');
            drawFontText(row.get(), dialogFont,
                         language.get(key.substr(0, colon),
                                      key.substr(colon + 1), ""),
                         0x28, 8);
            items.push_back({std::move(row), 0});
        }
        game.list->fill(kListRect, std::move(items), kListLayer,
                        kListInitialColour);
        setActivitySpinnerRange(0x14, 0x168);
    } else if (tab == kTabPicture) {
        for (const int picture : game.pictures) {
            SurfacePtr row = greenSurface(200, 0x22);
            std::string label;
            SurfacePtr icon;
            if (picture == kEntryPicture) {
                icon = activityBitmap("activity_1");
                label = language.get(game.entryScene, "SCENENAME",
                                     game.entryScene);
            } else {
                const auto &library = executableGameData;
                int group = -1;
                for (int candidate = 0; candidate < 5; ++candidate) {
                    if (picture < static_cast<int>(library.u32At(
                                      kLibraryBases + 4U * (candidate + 1))))
                        { group = candidate; break; }
                }
                const int base = static_cast<int>(
                    library.u32At(kLibraryBases + 4U * group));
                char name[64]{};
                std::snprintf(name, sizeof(name),
                              library.stringAt(library.u32At(
                                  kLibraryPatterns + 4U * group)).c_str(),
                              picture - base + 1);
                icon = activityCommonBitmap(std::string("bitmap\\small\\") +
                                            name + ".bmp");
                // 0x0041b460 -> 0x0041b580.
                const int category = static_cast<int>(
                    library.u32At(kLibraryCategories + 4U * group));
                int local = picture - base;
                const int count = static_cast<int>(
                    library.u32At(kAwardCounts + 4U * category));
                std::string key = "awarddlg:NONE";
                if (local >= 0 && local < count) {
                    if (category == 7)
                        local = static_cast<int>(
                            library.u32At(kAwardRemap + 4U * local));
                    key = library.stringAt(library.u32At(
                        library.u32At(kAwardNames + 4U * category) +
                        4U * local));
                }
                const auto colon = key.find(':');
                label = language.get(key.substr(0, colon),
                                     key.substr(colon + 1), "");
            }
            if (icon) {
                SDL_Rect at{1, 1, icon->w, icon->h};
                SDL_BlitSurface(icon.get(), nullptr, row.get(), &at);
            }
            drawFontText(row.get(), dialogFont, label, 0x28, 8);
            items.push_back({std::move(row), picture});
        }
        game.list->fill(kListRect, std::move(items), kListLayer,
                        kListInitialColour);
        setActivitySpinnerRange(0x28, 0xf0);
    }
    game.list->setColour(kListColour);
    game.list->select(0);
    game.scrollbar->setRange(0, game.list->count() - game.list->visibleRows());
    game.scrollbar->setValue(0);
    game.tab = tab;
}

void ActivitypageScene::setActivitySpinnerRange(int minimum, int maximum) {
    // 0x0045c430.
    auto &spinner = activitypageGame->spinner;
    spinner.minimum = minimum;
    spinner.maximum = maximum;
    setActivitySpinner(spinner.value);
}

void ActivitypageScene::setActivitySpinner(int value) {
    // 0x0045c470.
    auto &spinner = activitypageGame->spinner;
    if (value < spinner.minimum) {
        value = spinner.minimum;
    } else if (value > spinner.maximum) {
        value = spinner.maximum;
    }
    if (value == spinner.value) return;
    spinner.value = value;
    drawActivitySpinner(value);
}

void ActivitypageScene::drawActivitySpinner(int value) {
    // 0x0045c2d0: "%d" in Amerigo BT_14_ centred on the spinner (untraced).
    auto &game = *activitypageGame;
    auto &sprite = game.stage->sprite(game.spinnerSprite);
    fillSurface(sprite.surface.get(), nullptr, 0x00ff00U);
    const std::string text = std::to_string(value);
    const SDL_Point at = centreIn(
        SDL_Rect{0, 0, sprite.surface->w, sprite.surface->h},
        textWidth(calendarFont, text), calendarFont.height);
    drawFontText(sprite.surface.get(), calendarFont, text, at.x, at.y);
    game.stage->touch(game.spinnerSprite);
}

void ActivitypageScene::setActivityColour(std::uint32_t colour) {
    // 0x0045c880.
    auto &game = *activitypageGame;
    fillSurface(game.stage->sprite(game.swatch).surface.get(), nullptr,
                colourRgb(colour));
    game.stage->touch(game.swatch);
    game.colour = colour;
}

int ActivitypageScene::activityLayout() const {
    // 0x0045b300: the layout toggle on means landscape.
    return activitypageGame->panel->state(kLayout) == 1 ? 2 : 1;
}

void ActivitypageScene::setActivityLayout(int layout) {
    // 0x0045b320.
    auto &game = *activitypageGame;
    game.stage->removeIfActive(game.paper);
    int width = kPageRect.w;
    int height = kPageRect.h;
    if (layout == 1) {
        width = roundAway(static_cast<float>(height) * kPaperAspect);
    } else if (layout == 2) {
        height = roundAway(static_cast<float>(width) * kPaperAspect);
    }
    SurfacePtr paper = activityBitmap("paper");
    SDL_Point at = centreIn(kPageRect, width, height);
    at.x -= 1;
    at.y -= 1;
    height += 6;
    width += 5;
    // 0x00420b00 with the centre rectangle 0x11,0x10,0x51,0x50.
    game.stage->setSurface(
        game.paper, nineSlice(width, height, paper.get(),
                              SDL_Rect{0x11, 0x10, 0x51 - 0x11, 0x50 - 0x10}));
    game.stage->add(game.paper, at.x, at.y, kPaperLayer);
}

SDL_Rect ActivitypageScene::activityPageRect() const {
    // 0x0045b2c0: the paper descriptor without its border.
    const auto &game = *activitypageGame;
    SDL_Rect rect = game.stage->rect(game.paper);
    rect.x += 1;
    rect.y += 1;
    rect.w -= 5;
    rect.h -= 6;
    return rect;
}

int ActivitypageScene::freeActivitySlot() const {
    // 0x0045b5f0.
    const auto &objects = activitypageGame->objects;
    for (std::size_t slot = 0; slot < objects.size(); ++slot) {
        if (!objects[slot].used) return static_cast<int>(slot);
    }
    return -1;
}

void ActivitypageScene::raiseActivityObject(int slot) {
    // 0x0045b570: a 32-value window from -400; when it runs out every object
    // drops 32 layers and the counter restarts.
    auto &game = *activitypageGame;
    if (gActivityLayerCounter < -400 || gActivityLayerCounter > -0x171) {
        for (auto &object : game.objects) {
            if (!object.used) continue;
            object.layer -= 0x20;
            game.stage->setLayer(object.sprite, object.layer);
        }
        gActivityLayerCounter = -400;
    }
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    object.layer = gActivityLayerCounter++;
    game.layerCounter = gActivityLayerCounter;
    game.stage->setLayer(object.sprite, object.layer);
}

unsigned ActivitypageScene::activityStyle() const {
    // 0x0045b770.
    const auto &panel = *activitypageGame->panel;
    return (panel.state(kBold) == 1 ? 1U : 0U) |
           (panel.state(kItalic) == 1 ? 2U : 0U) |
           (panel.state(kUnderline) == 1 ? 4U : 0U);
}

void ActivitypageScene::addActivityObject() {
    // 0x0045b470.
    auto &game = *activitypageGame;
    bool added = true;
    if (game.tab == kTabText) {
        added = addActivityText();
    } else if (game.tab == kTabShape) {
        added = addActivityShape();
    } else if (game.tab == kTabPicture) {
        added = addActivityPicture();
    }
    if (!added) {
        showMessage(language.get("activity", "OBJECTSMAX",
                                 "Du har fylt opp arket."));
    }
}

bool ActivitypageScene::addActivityText() {
    // 0x0045b4d0.
    auto &game = *activitypageGame;
    const int slot = freeActivitySlot();
    if (slot < 0) return false;
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    const int sprite = object.sprite;
    object = ActivityObject{};
    object.sprite = sprite;
    object.colour = game.colour;
    object.size = game.spinner.value;
    object.style = activityStyle();
    const int selected = game.list->selected();
    if (selected >= 0) {
        object.font = game.list->parameter(selected);
        openActivityText(ActivityTextPurpose::NewText, slot);
    }
    return true;
}

bool ActivitypageScene::addActivityShape() {
    // 0x0045b7b0.
    auto &game = *activitypageGame;
    const int slot = freeActivitySlot();
    if (slot < 0) return false;
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    const int sprite = object.sprite;
    object = ActivityObject{};
    object.sprite = sprite;
    const int selected = game.list->selected();
    if (selected < 0) return true;
    object.shape = selected;
    object.colour = game.colour;
    object.width = game.spinner.value;
    object.height = game.spinner.value;
    object.layer = 0;
    renderActivityShape(object);
    object.used = true;
    object.type = kTabShape;
    raiseActivityObject(slot);
    return true;
}

bool ActivitypageScene::addActivityPicture() {
    // 0x0045b960.
    auto &game = *activitypageGame;
    const int selected = game.list->selected();
    if (selected < 0) return true;
    return addActivityPictureObject(game.list->parameter(selected));
}

bool ActivitypageScene::addActivityPictureObject(int picture) {
    // 0x0045b9a0.
    auto &game = *activitypageGame;
    const int slot = freeActivitySlot();
    if (slot < 0) return false;
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    const int sprite = object.sprite;
    object = ActivityObject{};
    object.sprite = sprite;
    object.picture = picture;
    const float scale =
        static_cast<float>(game.spinner.value) * kPictureScale;
    object.scaleX = scale;
    object.scaleY = scale;
    object.layer = 0;
    loadActivityPicture(object);
    object.used = true;
    object.type = kTabPicture;
    raiseActivityObject(slot);
    return true;
}

int ActivitypageScene::renderActivityText(ActivityObject &object) {
    // 0x0045b660. Native substitute for the GDI text: the chosen bitmap font
    // scaled to the object's size, recoloured; bold doubles the strokes one
    // pixel right and underline draws the bottom row. UNCERTAIN: italic.
    if (object.text.empty()) return 2;
    auto &game = *activitypageGame;
    SDL_Rect rect = kPageRect;
    if (game.stage->active(object.sprite)) {
        rect = game.stage->rect(object.sprite);
        game.stage->remove(object.sprite);
    }
    if (game.fonts.empty()) return 1;
    const auto &font =
        game.fonts[static_cast<std::size_t>(std::clamp<int>(
            object.font, 0, static_cast<int>(game.fonts.size()) - 1))].font;
    const int naturalW = std::max(1, textWidth(font, object.text));
    const int naturalH = std::max(1, font.height);
    SurfacePtr natural = createSurface(naturalW + 1, naturalH);
    SDL_FillRect(natural.get(), nullptr, 0);
    drawFontText(natural.get(), font, object.text, 0, 0);
    if (object.style & 1U) drawFontText(natural.get(), font, object.text, 1, 0);
    const double scale = static_cast<double>(std::max(1, object.size)) / naturalH;
    const int width = std::max(1, roundAway((naturalW + 1) * scale));
    const int height = std::max(1, object.size);
    SurfacePtr scaled = createSurface(width, height);
    SDL_FillRect(scaled.get(), nullptr, 0);
    SDL_BlitScaled(natural.get(), nullptr, scaled.get(), nullptr);
    SurfacePtr surface = greenSurface(width, height);
    const std::uint32_t ink = mapRgb(surface.get(), colourRgb(object.colour));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((pixelAt(scaled.get(), x, y) >> 24U) >= 0x80U)
                putPixel(surface.get(), x, y, ink);
        }
    }
    if (object.style & 4U) {
        for (int x = 0; x < width; ++x) putPixel(surface.get(), x, height - 1, ink);
    }
    auto &sprite = game.stage->sprite(object.sprite);
    sprite.keyed = true;
    game.stage->setSurface(object.sprite, std::move(surface));
    const SDL_Point at = centreIn(rect, width, height);
    game.stage->add(object.sprite, at.x, at.y, object.layer);
    return 1;
}

void ActivitypageScene::renderActivityShape(ActivityObject &object) {
    // 0x0045b840 + 0x0045bcf0.
    auto &game = *activitypageGame;
    SDL_Rect rect = kPageRect;
    if (game.stage->active(object.sprite)) {
        rect = game.stage->rect(object.sprite);
        game.stage->remove(object.sprite);
    }
    const int width = std::max(1, object.width);
    const int height = std::max(1, object.height);
    const SDL_Point at = centreIn(rect, width, height);
    SurfacePtr surface = greenSurface(width, height);
    const std::uint32_t fill = mapRgb(surface.get(), colourRgb(object.colour));
    const std::uint32_t pen = mapRgb(surface.get(), 0);
    if (object.shape == 0) {
        // Rectangle(0, 0, w, h): pen on the outer pixels.
        SDL_Rect inner{0, 0, width, height};
        SDL_FillRect(surface.get(), &inner, pen);
        inner = {1, 1, width - 2, height - 2};
        if (inner.w > 0 && inner.h > 0) SDL_FillRect(surface.get(), &inner, fill);
    } else if (object.shape == 1) {
        // Ellipse(0, 0, w, h).
        const double cx = (width - 1) / 2.0;
        const double cy = (height - 1) / 2.0;
        const double rx = std::max(0.5, width / 2.0);
        const double ry = std::max(0.5, height / 2.0);
        const auto inside = [&](int x, int y) {
            const double dx = (x - cx) / rx;
            const double dy = (y - cy) / ry;
            return dx * dx + dy * dy <= 1.0;
        };
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (!inside(x, y)) continue;
                const bool edge = !inside(x - 1, y) || !inside(x + 1, y) ||
                                  !inside(x, y - 1) || !inside(x, y + 1);
                putPixel(surface.get(), x, y, edge ? pen : fill);
            }
        }
    } else {
        // Polygon with the points of 0x005028a0[shape - 2], scaled and
        // rounded away from zero (0x0045bdd4-0x0045be23).
        const auto &data = executableGameData;
        const std::uint32_t table = data.u32At(kShapePoints + 4U * object.shape);
        const int count = static_cast<int>(
            data.u32At(kShapeCounts + 4U * (object.shape - 2)));
        std::vector<SDL_Point> points;
        for (int index = 0; index < count; ++index) {
            const std::uint32_t xBits = data.u32At(table + 8U * index);
            const std::uint32_t yBits = data.u32At(table + 8U * index + 4U);
            float fx = 0.0F;
            float fy = 0.0F;
            std::memcpy(&fx, &xBits, 4);
            std::memcpy(&fy, &yBits, 4);
            points.push_back({roundAway(fx * width), roundAway(fy * height)});
        }
        // Even-odd scanline fill at pixel centres, then the outline.
        for (int y = 0; y < height; ++y) {
            const double sampleY = y + 0.5;
            std::vector<double> crossings;
            for (std::size_t index = 0; index < points.size(); ++index) {
                const SDL_Point a = points[index];
                const SDL_Point b = points[(index + 1) % points.size()];
                if ((a.y <= sampleY && b.y > sampleY) ||
                    (b.y <= sampleY && a.y > sampleY)) {
                    crossings.push_back(a.x + (sampleY - a.y) *
                                                  (b.x - a.x) /
                                                  static_cast<double>(b.y - a.y));
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t index = 0; index + 1 < crossings.size(); index += 2) {
                for (int x = static_cast<int>(std::ceil(crossings[index] - 0.5));
                     x < static_cast<int>(std::ceil(crossings[index + 1] - 0.5));
                     ++x) {
                    putPixel(surface.get(), x, y, fill);
                }
            }
        }
        for (std::size_t index = 0; index < points.size(); ++index) {
            const SDL_Point a = points[index];
            const SDL_Point b = points[(index + 1) % points.size()];
            drawLine(surface.get(), a.x, a.y, b.x, b.y, pen);
        }
    }
    auto &sprite = game.stage->sprite(object.sprite);
    // 0x00407830(*shape == 0): only the rectangle is opaque.
    sprite.keyed = object.shape != 0;
    game.stage->setSurface(object.sprite, std::move(surface));
    game.stage->add(object.sprite, at.x, at.y, object.layer);
}

void ActivitypageScene::loadActivityPicture(ActivityObject &object) {
    // 0x0045ba10: library pictures from common/bitmap/large, the entry
    // picture copied.
    auto &game = *activitypageGame;
    object.bitmap.reset();
    if (object.picture < kEntryPicture) {
        const auto &library = executableGameData;
        int group = 0;
        for (int candidate = 0; candidate < 5; ++candidate) {
            if (object.picture <
                static_cast<int>(library.u32At(kLibraryBases + 4U * (candidate + 1)))) {
                group = candidate;
                break;
            }
        }
        const int base =
            static_cast<int>(library.u32At(kLibraryBases + 4U * group));
        char name[64]{};
        std::snprintf(name, sizeof(name),
                      library.stringAt(library.u32At(kLibraryPatterns + 4U * group))
                          .c_str(),
                      object.picture - base + 1);
        object.bitmap =
            activityCommonBitmap(std::string("bitmap\\large\\") + name + ".bmp");
    } else if (game.entryPicture) {
        object.bitmap = toArgb(game.entryPicture.get());
    }
    if (!object.bitmap) object.bitmap = createSurface(1, 1);
    renderActivityPicture(object);
}

void ActivitypageScene::renderActivityPicture(ActivityObject &object) {
    // 0x0045bae0 + 0x0045bc40.
    auto &game = *activitypageGame;
    SDL_Rect rect = kPageRect;
    if (game.stage->active(object.sprite)) {
        rect = game.stage->rect(object.sprite);
        game.stage->remove(object.sprite);
    }
    const int width = std::max(1, roundAway(static_cast<float>(object.bitmap->w) *
                                            static_cast<float>(object.scaleX)));
    const int height = std::max(1, roundAway(static_cast<float>(object.bitmap->h) *
                                             static_cast<float>(object.scaleY)));
    const SDL_Point at = centreIn(rect, width, height);
    SurfacePtr surface = createSurface(width, height);
    stretchInto(object.bitmap.get(), surface.get());
    auto &sprite = game.stage->sprite(object.sprite);
    sprite.keyed = false;
    game.stage->setSurface(object.sprite, std::move(surface));
    game.stage->add(object.sprite, at.x, at.y, object.layer);
}

void ActivitypageScene::removeActivityObject(int slot) {
    // 0x0045cd60.
    auto &game = *activitypageGame;
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    if (game.stage->active(object.sprite) && object.type > 9) {
        if (object.type == kTabPicture) object.bitmap.reset();
        if (object.type <= kTabPicture) game.stage->remove(object.sprite);
    }
    if (game.selected == slot) deselectActivityObject();
    object.used = false;
}

void ActivitypageScene::removeSelectedActivityObject() {
    // 0x0045cfa0.
    auto &game = *activitypageGame;
    if (game.selected >= 0) removeActivityObject(game.selected);
}

void ActivitypageScene::clearActivityObjects() {
    // 0x0045cd30.
    auto &game = *activitypageGame;
    for (std::size_t slot = 0; slot < game.objects.size(); ++slot) {
        if (game.objects[slot].used) removeActivityObject(static_cast<int>(slot));
    }
}

void ActivitypageScene::deselectActivityObject() {
    // 0x0045cde0.
    auto &game = *activitypageGame;
    game.stage->removeIfActive(game.selection);
    game.stage->removeIfActive(game.closeHandle);
    game.stage->removeIfActive(game.resizeHandle);
    game.selected = -1;
}

void ActivitypageScene::refreshActivitySelection() {
    // 0x0045d080: a black ring 2 pixels outside the object, the close
    // handle on its top-left and (not for text) the resize handle inside its
    // bottom-right corner.
    auto &game = *activitypageGame;
    if (game.selected < 0) return;
    auto &object = game.objects[static_cast<std::size_t>(game.selected)];
    game.stage->removeIfActive(game.selection);
    SDL_Rect rect = game.stage->rect(object.sprite);
    rect = {rect.x - 3, rect.y - 3, rect.w + 6, rect.h + 6};
    SurfacePtr frame = greenSurface(rect.w, rect.h);
    game.stage->setSurface(game.selection, std::move(frame));
    game.stage->add(game.selection, rect.x, rect.y, kSelectionLayer);
    if (!game.stage->active(game.closeHandle)) {
        game.stage->add(game.closeHandle, rect.x, rect.y, kHandleLayer);
    } else {
        game.stage->move(game.closeHandle, rect.x, rect.y);
    }
    if (object.type != kTabText) {
        const int x = rect.x + rect.w - 0x10;
        const int y = rect.y + rect.h - 0x10;
        if (!game.stage->active(game.resizeHandle)) {
            game.stage->add(game.resizeHandle, x, y, kHandleLayer);
        } else {
            game.stage->move(game.resizeHandle, x, y);
        }
    }
    auto *surface = game.stage->sprite(game.selection).surface.get();
    SDL_Rect ring{1, 1, rect.w - 2, rect.h - 2};
    fillSurface(surface, &ring, 0);
    ring = {2, 2, rect.w - 4, rect.h - 4};
    fillSurface(surface, &ring, 0x00ff00U);
    game.stage->touch(game.selection);
}

void ActivitypageScene::followActivitySelection() {
    // 0x0045dea0 during a drag (untraced moves).
    auto &game = *activitypageGame;
    if (game.selected < 0) return;
    const auto &object = game.objects[static_cast<std::size_t>(game.selected)];
    SDL_Rect rect = game.stage->rect(object.sprite);
    rect = {rect.x - 3, rect.y - 3, rect.w + 6, rect.h + 6};
    if (game.stage->active(game.selection))
        game.stage->move(game.selection, rect.x, rect.y);
    if (game.stage->active(game.closeHandle))
        game.stage->move(game.closeHandle, rect.x, rect.y);
    if (game.stage->active(game.resizeHandle))
        game.stage->move(game.resizeHandle, rect.x + rect.w - 0x10,
                         rect.y + rect.h - 0x10);
}

void ActivitypageScene::syncActivitySpinner(int slot) {
    // 0x0045dc40.
    auto &game = *activitypageGame;
    if (slot < 0) return;
    const auto &object = game.objects[static_cast<std::size_t>(slot)];
    if (object.type == kTabText) {
        setActivitySpinner(object.size);
    } else if (object.type == kTabShape) {
        setActivitySpinner(object.width);
    } else if (object.type == kTabPicture) {
        setActivitySpinner(game.stage->rect(object.sprite).w);
    }
}

void ActivitypageScene::selectActivityObject(int slot) {
    // 0x0045dac0.
    auto &game = *activitypageGame;
    deselectActivityObject();
    game.selected = slot;
    raiseActivityObject(slot);
    refreshActivitySelection();
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    game.panel->setGroupState(1, 0);
    game.panel->setState(static_cast<std::uint32_t>(object.type), 1);
    setActivityTab(object.type);
    syncActivitySpinner(slot);
    if (object.type == kTabText) {
        setActivityColour(object.colour);
        setActivitySpinner(object.size);
        // 0x0045dc00.
        game.panel->setState(kBold, (object.style & 1U) ? 1 : 0);
        game.panel->setState(kItalic, (object.style & 2U) ? 1 : 0);
        game.panel->setState(kUnderline, (object.style & 4U) ? 1 : 0);
        game.list->select(object.font);
    } else if (object.type == kTabShape) {
        setActivityColour(object.colour);
        setActivitySpinner(object.width);
        game.list->select(object.shape);
    } else if (object.type == kTabPicture) {
        game.list->select(game.list->indexOf(object.picture));
    }
    if (game.scrollbar->setValue(game.list->top()))
        game.list->scrollTo(game.scrollbar->value());
}

int ActivitypageScene::activityObjectAt(int x, int y) const {
    // 0x0045d1f0: the topmost slot by layer; ties keep the lower slot.
    const auto &game = *activitypageGame;
    int found = -1;
    int best = std::numeric_limits<int>::min();
    const SDL_Point point{x, y};
    for (std::size_t slot = 0; slot < game.objects.size(); ++slot) {
        const auto &object = game.objects[slot];
        if (!object.used || object.layer <= best) continue;
        if (!game.stage->active(object.sprite)) continue;
        const SDL_Rect rect = game.stage->rect(object.sprite);
        if (!SDL_PointInRect(&point, &rect)) continue;
        best = object.layer;
        found = static_cast<int>(slot);
    }
    return found;
}

void ActivitypageScene::applyActivitySize(int width, int height) {
    // 0x0045d5e0.
    auto &game = *activitypageGame;
    if (game.selected < 0) return;
    auto &object = game.objects[static_cast<std::size_t>(game.selected)];
    if (object.type == kTabText) {
        object.size = height;
        renderActivityText(object);
    } else if (object.type == kTabShape) {
        object.width = width;
        object.height = height;
        renderActivityShape(object);
    } else if (object.type == kTabPicture) {
        object.scaleX = static_cast<float>(width) /
                        static_cast<float>(object.bitmap->w);
        object.scaleY = static_cast<float>(height) /
                        static_cast<float>(object.bitmap->h);
        renderActivityPicture(object);
    }
    refreshActivitySelection();
}

void ActivitypageScene::applyActivityColour(std::uint32_t colour) {
    // 0x0045d580.
    auto &game = *activitypageGame;
    setActivityColour(colour);
    if (game.selected < 0) return;
    auto &object = game.objects[static_cast<std::size_t>(game.selected)];
    if (object.type == kTabText) {
        object.colour = colour;
        renderActivityText(object);
    } else if (object.type == kTabShape) {
        object.colour = colour;
        renderActivityShape(object);
    }
}

void ActivitypageScene::activityListPicked(int index) {
    // 0x0045cfd0: a row picked while an object of this tab is selected.
    auto &game = *activitypageGame;
    if (game.selected < 0) return;
    auto &object = game.objects[static_cast<std::size_t>(game.selected)];
    if (object.type != game.tab) return;
    if (object.type == kTabText) {
        object.font = game.list->parameter(index);
        renderActivityText(object);
    } else if (object.type == kTabShape) {
        object.shape = index;
        renderActivityShape(object);
    } else if (object.type == kTabPicture) {
        object.picture = game.list->parameter(index);
        loadActivityPicture(object);
    }
    refreshActivitySelection();
}

void ActivitypageScene::activityControlClicked(std::uint32_t id) {
    // Event 0x116: the scene (0x0045d290) and the scroll control (0x00409450)
    // both see it.
    auto &game = *activitypageGame;
    if (game.scrollbar->clickButton(id)) game.list->scrollTo(game.scrollbar->value());
    switch (id) {
    case kHelp:
        showSceneHelp();
        return;
    case kExit:
        if (!game.entryScene.empty()) {
            enterScene(game.entryScene, game.entryScene, game.entryMode);
        } else {
            returnFromScene();
        }
        return;
    case kTabText:
    case kTabShape:
    case kTabPicture:
        deselectActivityObject();
        setActivityTab(static_cast<int>(id));
        return;
    case kColour:
        // UNCERTAIN: the colour dialog 0x0041cc10 (common/dialogue/colordlg)
        // is not ported; "activity:colour:RRGGBB" applies a colour the way
        // its OK path does (0x0045d580).
        std::cout << "Activity page: colour dialog not ported\n";
        return;
    case kSizeUp:
    case kSizeDown:
        setActivitySpinner(game.spinner.value + (id == kSizeUp ? 5 : -5));
        applyActivitySize(game.spinner.value, game.spinner.value);
        return;
    case kEdit:
        if (game.selected >= 0 &&
            game.objects[static_cast<std::size_t>(game.selected)].type == kTabText)
            openActivityText(ActivityTextPurpose::EditText, game.selected);
        return;
    case kBold:
    case kItalic:
    case kUnderline:
        if (game.selected >= 0 &&
            game.objects[static_cast<std::size_t>(game.selected)].type == kTabText) {
            auto &object = game.objects[static_cast<std::size_t>(game.selected)];
            object.style = activityStyle();
            renderActivityText(object);
            refreshActivitySelection();
        }
        return;
    case kAdd:
        addActivityObject();
        return;
    case kRemove:
        removeSelectedActivityObject();
        return;
    case kClear:
        showMessage(language.get("activity", "DOCLEAR",
                                 "Er du sikker på at du ønsker å slette denne siden?"),
                    MessageKind::ActivitypageClearConfirmation);
        return;
    case kLayout:
        setActivityLayout(activityLayout());
        return;
    case kSave:
        openActivityText(ActivityTextPurpose::Save, -1);
        return;
    case kPrint:
        showMessage(language.get("activity", "DOPRINT",
                                 "Vil du skrive ut denne siden?"),
                    MessageKind::ActivitypagePrintConfirmation);
        return;
    default:
        return;
    }
}

void ActivitypageScene::confirmActivitypageClear() {
    if (activitypageGame) clearActivityObjects();
}

void ActivitypageScene::confirmActivitypagePrint() {
    // 0x0045d4f1-0x0045d50b: the objects sorted by layer (0x0045d6b0) go to
    // 0x004104f0, which opens PageSetupDlgA, sets the orientation from the
    // layout and draws the page through 0x0045d750 into the printer DC.
    // UNCERTAIN/not ported: native has no printing path.
    std::cout << "Activity page: printing is not available in the native "
                 "build\n";
}

SurfacePtr ActivitypageScene::renderActivityPage() const {
    // 0x0045d9c0: the page, white, with every object in layer order at its
    // offset from the page (0x0045d6b0 sort, 0x0045d870 per object).
    const auto &game = *activitypageGame;
    const SDL_Rect page = activityPageRect();
    SurfacePtr result = createSurface(page.w, page.h);
    fillSurface(result.get(), nullptr, 0xffffffU);
    std::vector<const ActivityObject *> order;
    for (const auto &object : game.objects) {
        if (object.used && game.stage->active(object.sprite))
            order.push_back(&object);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const ActivityObject *left, const ActivityObject *right) {
                         return left->layer < right->layer;
                     });
    for (const auto *object : order) {
        const auto &sprite = game.stage->sprite(object->sprite);
        SDL_Surface *surface = sprite.surface.get();
        if (sprite.keyed) {
            SDL_SetColorKey(surface, SDL_TRUE, pixelValue(surface, 0, 0));
        } else {
            SDL_SetColorKey(surface, SDL_FALSE, 0);
        }
        SDL_Rect at{sprite.x - page.x, sprite.y - page.y, surface->w, surface->h};
        SDL_BlitSurface(surface, nullptr, result.get(), &at);
    }
    return result;
}

void ActivitypageScene::openActivityText(ActivityTextPurpose purpose, int slot) {
    // 0x0041ffb0 at (0xf5, 0xf3): EDITTEXT with 64 characters (0x0045b610) or
    // SAVEPATH with 24 (0x0045d474).
    auto &game = *activitypageGame;
    game.textPurpose = purpose;
    game.textObject = slot;
    if (purpose == ActivityTextPurpose::Save) {
        game.textTitle = language.get("activity", "SAVEPATH", "Lagre som");
        game.textValue.clear();
        game.textLimit = 0x18;
    } else {
        game.textTitle = language.get("activity", "EDITTEXT", "Rediger tekst");
        game.textValue = game.objects[static_cast<std::size_t>(slot)].text;
        game.textLimit = 0x40;
    }
    SDL_StartTextInput();
}

void ActivitypageScene::closeActivityText(bool accept) {
    auto &game = *activitypageGame;
    const auto purpose = game.textPurpose;
    const int slot = game.textObject;
    game.textPurpose = ActivityTextPurpose::None;
    SDL_StopTextInput();
    if (purpose == ActivityTextPurpose::Save) {
        if (!accept || game.textValue.empty()) return;
        // 0x0045d494: "%s\%s.jpg" in <install>\Layouts (0x0054de40, built at
        // 0x00410165), written as a quality-100 JPEG (0x00414dc0).
        // UNCERTAIN/native substitute: native writes the same page as BMP,
        // next to the profiles directory, because it carries no JPEG encoder.
        const fs::path directory = profileDirectory.parent_path() / "Layouts";
        std::error_code error;
        fs::create_directories(directory, error);
        const fs::path path = directory / (game.textValue + ".bmp");
        SurfacePtr page = renderActivityPage();
        if (SDL_SaveBMP(page.get(), path.string().c_str()) != 0) {
            std::cerr << "Activity page: could not save " << path << ": "
                      << SDL_GetError() << '\n';
        } else {
            std::cout << "Activity page saved to " << path << '\n';
        }
        return;
    }
    auto &object = game.objects[static_cast<std::size_t>(slot)];
    if (accept) {
        // 0x0041ffb0 copies the text only on OK (strncpy, 0x00420017).
        object.text = game.textValue;
        const int result = renderActivityText(object);
        if (purpose == ActivityTextPurpose::NewText && result == 1) {
            object.used = true;
            object.type = kTabText;
            raiseActivityObject(slot);
        }
    }
    if (purpose == ActivityTextPurpose::EditText) refreshActivitySelection();
}

void ActivitypageScene::renderActivityTextDialog() {
    auto &game = *activitypageGame;
    constexpr int dialogX = 0xf5;
    constexpr int dialogY = 0xf3;
    if (nameDialogBackdropTexture && nameDialogBackdrop) {
        SDL_Rect destination{dialogX, dialogY, nameDialogBackdrop->w,
                             nameDialogBackdrop->h};
        SDL_RenderCopy(renderer, nameDialogBackdropTexture.get(), nullptr,
                       &destination);
    }
    // 0x00420040: the title in (11,5,0x119,0x12), the text in
    // (11,0x1e,0x119,0x21), OK/Cancel at (15,69)/(204,69).
    drawText(renderer, dialogFont, game.textTitle, dialogX + 11, dialogY + 5);
    drawText(renderer, calendarFont, game.textValue, dialogX + 11,
             dialogY + 0x1e + (0x21 - calendarFont.height) / 2);
    if ((SDL_GetTicks() / 500U) % 2U == 0U) {
        drawText(renderer, calendarFont, "|",
                 dialogX + 11 + textWidth(calendarFont, game.textValue),
                 dialogY + 0x1e + (0x21 - calendarFont.height) / 2);
    }
    drawCenteredText(renderer, dialogFont, language.get("interfaceh", "OK", "OK"),
                     SDL_Rect{dialogX + 15, dialogY + 69, 84, 30});
    drawCenteredText(renderer, dialogFont,
                     language.get("interfaceh", "CANCEL", "Avbryt"),
                     SDL_Rect{dialogX + 204, dialogY + 69, 84, 29});
}

void ActivitypageScene::activityMouseDown(int x, int y, bool doubleClick) {
    auto &game = *activitypageGame;
    game.leftDown = true;
    // 0x0040c590: the hotspot is resolved (0x113/0x114) before 0x105.
    const std::uint32_t hotspot = activityHotspotAt(x, y);
    activityHotspotChanged(hotspot);
    game.tooltip->hide();
    // Scene handler 0x0045ce50.
    if (hotspot == game.scrollbar->thumbHotspot() &&
        game.stage->active(game.scrollbar->thumbSprite())) {
        // 0x004093f0: dragging the thumb (tag = the scroll id).
        game.drag = ActivityDrag::Scroll;
        game.dragSprite = game.scrollbar->thumbSprite();
    } else {
        bool dragging = false;
        if (game.scrollbar->pressTrack(x, y, dragging))
            game.list->scrollTo(game.scrollbar->value());
        if (dragging) {
            game.drag = ActivityDrag::Scroll;
            game.dragSprite = game.scrollbar->thumbSprite();
        }
    }
    game.dragLastX = x;
    game.dragLastY = y;
    const int row = game.list->hit(x, y);
    if (row >= 0) {
        activityListPicked(row);
    } else {
        // 0x0045cf60: a press inside the spinner starts a value drag.
        const SDL_Point point{x, y};
        if (SDL_PointInRect(&point, &kSpinnerRect)) {
            game.spinner.dragging = true;
            game.spinner.startX = x;
            game.spinner.startY = y;
            game.spinner.delta = 0;
        }
        switch (hotspot) {
        case kScrollId:
        case kColour:
        case kSpinnerArea:
        case kSizeUp:
        case kSizeDown:
        case kEdit:
        case kBold:
        case kItalic:
        case kUnderline:
        case kRemove:
            break;
        case kCloseHotspot:
            removeSelectedActivityObject();
            break;
        case kResizeHotspot:
            game.drag = ActivityDrag::Resize;
            game.dragSprite = game.resizeHandle;
            break;
        default: {
            deselectActivityObject();
            const int slot = activityObjectAt(x, y);
            if (slot >= 0) {
                game.drag = ActivityDrag::Object;
                game.dragObject = slot;
                game.dragSprite = game.objects[static_cast<std::size_t>(slot)].sprite;
            }
            break;
        }
        }
    }
    // Panel handler 0x00406b50.
    if (const auto clicked = game.panel->press(hotspot, x, y))
        activityControlClicked(*clicked);
    // 0x0040c600: a double click adds event 0x11d (0x0045da90).
    if (doubleClick) {
        const int slot = activityObjectAt(x, y);
        if (slot >= 0) selectActivityObject(slot);
    }
}

void ActivitypageScene::activityMouseUp(int x, int y) {
    auto &game = *activitypageGame;
    game.leftDown = false;
    activityHotspotChanged(activityHotspotAt(x, y));
    // Event 0x106: the panel (0x00406cf0) and the spinner (0x0045dd10).
    if (const auto clicked = game.panel->release())
        activityControlClicked(*clicked);
    if (game.spinner.dragging) {
        // 0x0045dd40.
        game.spinner.dragging = false;
        game.spinner.value += game.spinner.delta;
        applyActivitySize(game.spinner.value, game.spinner.value);
    }
    if (game.drag == ActivityDrag::None) return;
    const auto drag = game.drag;
    const int object = game.dragObject;
    game.drag = ActivityDrag::None;
    game.dragObject = -1;
    game.dragSprite = -1;
    // Event 0x10b (0x0040c260).
    if (drag == ActivityDrag::Scroll) {
        // 0x004092b0: snap the thumb to its value.
        game.scrollbar->setValue(game.scrollbar->value());
    } else if (drag == ActivityDrag::Object) {
        // 0x0045dfa0: an object dropped clear of the page is removed.
        const SDL_Rect rect =
            game.stage->rect(game.objects[static_cast<std::size_t>(object)].sprite);
        const SDL_Rect page = kPageRect;
        const bool overlaps = page.x < rect.x + rect.w && page.y < rect.y + rect.h &&
                              rect.x < page.x + page.w && rect.y < page.y + page.h;
        if (!overlaps) removeActivityObject(object);
    } else if (drag == ActivityDrag::Resize && game.selected >= 0) {
        const int slot = game.selected;
        const int sprite = game.objects[static_cast<std::size_t>(slot)].sprite;
        SDL_Rect rect = game.stage->rect(sprite);
        const int width = std::clamp(x - rect.x, game.spinner.minimum,
                                     game.spinner.maximum);
        const int height = std::clamp(y - rect.y, game.spinner.minimum,
                                      game.spinner.maximum);
        const SDL_Point centred = centreIn(rect, width, height);
        game.stage->move(sprite, rect.x * 2 - centred.x, rect.y * 2 - centred.y);
        applyActivitySize(width, height);
        syncActivitySpinner(slot);
    }
}

void ActivitypageScene::activityMouseMove(int x, int y) {
    auto &game = *activitypageGame;
    activityHotspotChanged(activityHotspotAt(x, y));
    if (game.leftDown && game.spinner.dragging) {
        // Event 0x109 -> 0x0045dd90.
        auto &spinner = game.spinner;
        int value = spinner.value +
                    spinner.step * ((spinner.startY - y) / spinner.divisor);
        if (value < spinner.minimum) {
            value = spinner.minimum;
        } else if (value > spinner.maximum) {
            value = spinner.maximum;
        }
        if (value - spinner.value != spinner.delta) {
            spinner.delta = value - spinner.value;
            drawActivitySpinner(value);
        }
    }
    if (game.drag == ActivityDrag::None) return;
    // 0x0040c300 moves the dragged descriptor by the pointer delta, then
    // event 0x10a reaches the handlers.
    const SDL_Rect rect = game.stage->rect(game.dragSprite);
    game.stage->move(game.dragSprite, rect.x + (x - game.dragLastX),
                     rect.y + (y - game.dragLastY));
    game.dragLastX = x;
    game.dragLastY = y;
    if (game.drag == ActivityDrag::Scroll) {
        // 0x00409270.
        if (game.scrollbar->dragTo(y)) game.list->scrollTo(game.scrollbar->value());
    } else if (game.drag == ActivityDrag::Object) {
        if (game.dragObject == game.selected) followActivitySelection();
    } else if (game.drag == ActivityDrag::Resize && game.selected >= 0) {
        // 0x0045de00: the handle centre stays within the size range.
        const SDL_Rect object = game.stage->rect(
            game.objects[static_cast<std::size_t>(game.selected)].sprite);
        const int dx = std::clamp(x - object.x, game.spinner.minimum,
                                  game.spinner.maximum);
        const int dy = std::clamp(y - object.y, game.spinner.minimum,
                                  game.spinner.maximum);
        game.stage->moveCentre(game.resizeHandle, object.x + dx, object.y + dy);
    }
}

void ActivitypageScene::activityRightDown(int x, int y) {
    // Event 0x107: tooltip hide (0x00409f20) and 0x0045da90.
    auto &game = *activitypageGame;
    activityHotspotChanged(activityHotspotAt(x, y));
    game.tooltip->hide();
    const int slot = activityObjectAt(x, y);
    if (slot >= 0) selectActivityObject(slot);
}

void ActivitypageScene::activityKey(SDL_Keycode key) {
    // Event 0x10c -> 0x0045df60.
    auto &game = *activitypageGame;
    if (game.list->key(key) >= 0) {
        if (game.scrollbar->setValue(game.list->top()))
            game.list->scrollTo(game.scrollbar->value());
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        addActivityObject();
    } else if (key == SDLK_DELETE) {
        removeSelectedActivityObject();
    }
}

bool ActivitypageScene::handleActivitypageEvent(const SDL_Event &event) {
    if (!activitypageGame || activeScene != "activity") return false;
    auto &game = *activitypageGame;
    if (!game.announced) return false;
    if (game.textPurpose != ActivityTextPurpose::None) {
        if (event.type == SDL_TEXTINPUT) {
            if (const auto text = utf8ToCp1252(event.text.text)) {
                for (const char character : *text) {
                    if (game.textValue.size() >= game.textLimit) break;
                    if (game.textPurpose == ActivityTextPurpose::Save &&
                        std::string_view("\\/:*?\"<>|").find(character) !=
                            std::string_view::npos)
                        continue;
                    game.textValue.push_back(character);
                }
            }
        } else if (event.type == SDL_KEYDOWN) {
            const auto key = event.key.keysym.sym;
            if (key == SDLK_BACKSPACE && !game.textValue.empty()) {
                game.textValue.pop_back();
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                closeActivityText(true);
            } else if (key == SDLK_ESCAPE) {
                closeActivityText(false);
            }
        } else if (event.type == SDL_MOUSEBUTTONUP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            const int localX = event.button.x - 0xf5;
            const int localY = event.button.y - 0xf3;
            if (localY >= 69 && localY < 99 && localX >= 15 && localX < 99) {
                closeActivityText(true);
            } else if (localY >= 69 && localY < 98 && localX >= 204 &&
                       localX < 288) {
                closeActivityText(false);
            }
        }
        return event.type != SDL_QUIT;
    }
    switch (event.type) {
    case SDL_MOUSEMOTION:
        activityMouseMove(event.motion.x, event.motion.y);
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            activityMouseDown(event.button.x, event.button.y,
                              event.button.clicks >= 2);
            return true;
        }
        if (event.button.button == SDL_BUTTON_RIGHT) {
            activityRightDown(event.button.x, event.button.y);
            return true;
        }
        return false;
    case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            activityMouseUp(event.button.x, event.button.y);
            return true;
        }
        return false;
    case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_ESCAPE) {
            // 0x0045bf54: Escape returns to the scene that handed over a
            // picture; otherwise the engine's own Escape handling applies.
            if (game.entryScene.empty()) return false;
            enterScene(game.entryScene, game.entryScene, game.entryMode);
            return true;
        }
        activityKey(event.key.keysym.sym);
        return false;
    default:
        return false;
    }
}

void ActivitypageScene::updateActivitypage(double) {
    if (!activitypageGame || activeScene != "activity") return;
    auto &game = *activitypageGame;
    if (!game.announced) {
        // 0x0045c6f0: the picture list (entry picture first, then every
        // available library picture, 0x0041b3f0).
        game.pictures.clear();
        if (game.entryPicture) game.pictures.push_back(kEntryPicture);
        std::array<std::uint32_t, 8> awards{};
        if (!profiles.empty() && profileIndex < profiles.size() &&
            profiles[profileIndex] != "No profile") {
            const fs::path path =
                profileDirectory / (profiles[profileIndex] + ".pfl");
            std::error_code error;
            if (fs::exists(path, error)) {
                const auto bytes = readBinaryFile(path);
                // 0x00411b70: section 0x301 at 0x4c, eight u32 values.
                if (bytes.size() >= 0x78U && readLe32(bytes, 0x4c) == 0x301U &&
                    readLe32(bytes, 0x54) == 0x20U) {
                    for (std::size_t index = 0; index < awards.size(); ++index)
                        awards[index] = readLe32(bytes, 0x58U + 4U * index);
                }
            }
        }
        const auto &library = executableGameData;
        const int total = static_cast<int>(library.u32At(kLibraryTotal));
        for (int index = 0; index < total; ++index) {
            int group = -1;
            for (int candidate = 0; candidate < 5; ++candidate) {
                if (index < static_cast<int>(
                                library.u32At(kLibraryBases + 4U * (candidate + 1)))) {
                    group = candidate;
                    break;
                }
            }
            if (group < 0) continue;
            const int category = static_cast<int>(
                library.u32At(kLibraryCategories + 4U * group));
            const int local = index - static_cast<int>(
                library.u32At(kLibraryBases + 4U * group));
            bool available = false;
            if (category == 9) {
                available = true;
            } else if (category >= 0 && category < 8) {
                // 0x0041b5d0.
                const std::uint32_t initial =
                    library.u32At(kAwardInitial + 4U * category);
                const std::uint32_t value = awards[static_cast<std::size_t>(category)];
                if (category == 0 || category == 6) {
                    available = local < static_cast<int>(initial) ||
                                local < static_cast<int>(value);
                } else {
                    const std::uint32_t bit = 1U << (local & 31);
                    available = (initial & bit) != 0U || (value & bit) != 0U;
                }
            }
            if (available) game.pictures.push_back(index);
        }
        announceActivitypageEntry();
    }
    // Event 0x112 for the tooltip; the cursor sprite is 32 pixels tall at
    // the pointer (0x004135e0). UNCERTAIN: the cursor size is the activity
    // cursor's, read from the capture rather than from its frames.
    game.tooltip->tick(sceneElapsedNow(), SDL_Rect{mouseX, mouseY, 32, 32});
}

void ActivitypageScene::renderActivitypage(double) {
    if (!activitypageGame || activeScene != "activity") return;
    auto &game = *activitypageGame;
    if (!game.announced) return;
    game.stage->render();
    if (game.textPurpose != ActivityTextPurpose::None) renderActivityTextDialog();
}

bool ActivitypageScene::executeActivitypageDeveloperCommand(
    std::string_view command) {
    constexpr std::string_view prefix = "activity:";
    if (activeScene != "activity" || !activitypageGame ||
        !activitypageGame->announced)
        return false;
    auto &game = *activitypageGame;
    const auto control = command.substr(prefix.size());
    const auto number = [&](std::size_t offset) {
        return std::stoi(std::string(control.substr(offset)));
    };
    if (control.rfind("click:", 0) == 0) {
        const auto id = static_cast<std::uint32_t>(number(6));
        // A toggle flips before its click, as 0x00406be0 does on the press.
        for (const auto &candidate : game.panel->controls()) {
            if (candidate.id == id &&
                candidate.type == ActivityPanel::Type::Toggle)
                game.panel->setState(id, candidate.state == 0 ? 1 : 0);
        }
        activityControlClicked(id);
    } else if (control.rfind("tab:", 0) == 0) {
        const int tab = number(4);
        if (tab < kTabText || tab > kTabPicture) return false;
        game.panel->setGroupState(1, 0);
        game.panel->setState(static_cast<std::uint32_t>(tab), 1);
        activityControlClicked(static_cast<std::uint32_t>(tab));
    } else if (control.rfind("list:", 0) == 0) {
        const int index = number(5);
        if (index < 0 || index >= game.list->count()) return false;
        game.list->select(index);
        activityListPicked(index);
    } else if (control.rfind("select:", 0) == 0) {
        const int slot = number(7);
        if (slot < 0 || slot >= 32 ||
            !game.objects[static_cast<std::size_t>(slot)].used)
            return false;
        selectActivityObject(slot);
    } else if (control.rfind("colour:", 0) == 0) {
        applyActivityColour(colourRgb(static_cast<std::uint32_t>(
            std::stoul(std::string(control.substr(7)), nullptr, 16))));
    } else if (control.rfind("text:", 0) == 0) {
        if (game.textPurpose == ActivityTextPurpose::None) return false;
        game.textValue = std::string(control.substr(5));
        closeActivityText(true);
    } else if (control == "status") {
        int used = 0;
        for (const auto &object : game.objects) used += object.used ? 1 : 0;
        std::cout << "Activity status: tab " << game.tab << ", objects "
                  << used << ", selected " << game.selected << ", spinner "
                  << game.spinner.value << ", layout " << activityLayout()
                  << ", list " << game.list->selected() << "/"
                  << game.list->count() << " top " << game.list->top()
                  << ", pictures " << game.pictures.size() << ", fonts "
                  << game.fonts.size() << ", layer counter "
                  << gActivityLayerCounter << '\n';
        for (std::size_t slot = 0; slot < game.objects.size(); ++slot) {
            const auto &object = game.objects[slot];
            if (!object.used) continue;
            const SDL_Rect rect = game.stage->rect(object.sprite);
            std::cout << "  slot " << slot << " type " << object.type
                      << " layer " << object.layer << " rect " << rect.x
                      << "," << rect.y << " " << rect.w << "x" << rect.h
                      << '\n';
        }
    } else {
        throw std::runtime_error("Unknown developer Activity control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
