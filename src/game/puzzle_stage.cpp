#include "game/puzzle_stage.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assets/profile.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// listdlg (0x00420750): a 320x483 dialog centred on the 800x600 screen.
constexpr int kChooserWidth = 0x140;
constexpr int kChooserHeight = 0x1e3;
// 0x004b01f8: the list control's rectangle, dialog-relative.
constexpr SDL_Rect kChooserList{0x14, 0x3d, 0x102 - 0x14, 0x19a - 0x3d};
// 0x004b0208: the caption rectangle.
constexpr SDL_Rect kChooserTitle{0x11, 0x0d, 0x11e - 0x11, 0x28 - 0x0d};
// 0x0046b8ed / 0x0046e8ed: each row is a 0xee x 0x28 surface.
constexpr int kChooserRowHeight = 0x28;
// 0x004085a0 (param_1[0x36]): the selection bar's colour, a GDI COLORREF
// (0x00bbggrr) that 0x00404720 converts to the 16-bit display format.
constexpr std::uint32_t kChooserSelection = 0x3e5167U;
// 0x00420550: OK (1000) and Cancel (0x3e9) at y 0x1ab.
constexpr int kChooserOk = 1000;
constexpr int kChooserCancel = 0x3e9;
// 0x004205f0: the scroll bar 0x3ea and its arrows.
constexpr SDL_Rect kChooserScroll{0x115, 0x56, 0x0f, 0x122};
constexpr SDL_Point kChooserUp{0x10f, 0x31};
constexpr SDL_Point kChooserDown{0x110, 0x186};
// The dialog's layers: backdrop 100 (0x00420451), controls and list 0x65,
// rows one above (0x00407c00).
constexpr int kChooserLayer = 0x65;

} // namespace

void PuzzleStageScene::holdPuzzleSceneReady(PuzzleStage &stage) {
    if (sceneReadyAfterPresent) {
        stage.holdingSceneReady = true;
        sceneReadyAfterPresent.reset();
    }
}

void PuzzleStageScene::releasePuzzleSceneReady(PuzzleStage &stage) {
    if (!stage.holdingSceneReady) return;
    stage.holdingSceneReady = false;
    sceneReadyAfterPresent = activeScene;
}

bool PuzzleStageScene::puzzlePointerCommand(std::string_view control) {
    const auto colon = control.find(':');
    if (colon == std::string_view::npos) return false;
    const std::string verb(control.substr(0, colon));
    if (verb != "click" && verb != "down" && verb != "up" && verb != "move" &&
        verb != "rclick") {
        return false;
    }
    const std::string where(control.substr(colon + 1));
    const auto comma = where.find(',');
    if (comma == std::string::npos) return false;
    const int x = std::stoi(where.substr(0, comma));
    const int y = std::stoi(where.substr(comma + 1));
    const auto push = [&](Uint32 type, Uint8 button) {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_MOUSEMOTION) {
            event.motion.x = x;
            event.motion.y = y;
        } else {
            event.button.button = button;
            event.button.x = x;
            event.button.y = y;
        }
        SDL_PushEvent(&event);
    };
    push(SDL_MOUSEMOTION, 0);
    if (verb == "click" || verb == "down") push(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT);
    if (verb == "click" || verb == "up") push(SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT);
    if (verb == "rclick") {
        push(SDL_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT);
        push(SDL_MOUSEBUTTONUP, SDL_BUTTON_RIGHT);
    }
    return true;
}

void PuzzleStageScene::puzzleSnapshot(const std::string &path) {
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(renderer, &width, &height);
    SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
    if (!surface) return;
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             surface->pixels, surface->pitch) == 0) {
        SDL_SaveBMP(surface.get(), path.c_str());
    }
}

fs::path PuzzleStageScene::puzzleProfilePath() const {
    if (profileIndex >= profiles.size() ||
        profiles[profileIndex] == "No profile") {
        return {};
    }
    return profileDirectory / (profiles[profileIndex] + ".pfl");
}

std::uint32_t PuzzleStageScene::puzzlePictureRecord(std::string_view scene,
                                                    int picture) const {
    const fs::path path = puzzleProfilePath();
    std::error_code error;
    if (path.empty() || !fs::exists(path, error)) return 0;
    return profileSceneRecordU32(path, scene, 0x402U,
                                 static_cast<std::uint32_t>(picture))
        .value_or(0U);
}

void PuzzleStageScene::setPuzzlePictureRecord(std::string_view scene,
                                              int picture,
                                              std::uint32_t value) {
    const fs::path path = puzzleProfilePath();
    std::error_code error;
    if (path.empty() || !fs::exists(path, error)) return;
    setProfileSceneRecordU32(path, scene, 0x402U,
                             static_cast<std::uint32_t>(picture), value);
}

void PuzzleStageScene::loadPuzzleStage(PuzzleStage &stage, Package &package) {
    const auto font = [&](const char *name) {
        return loadBitmapFont(
            renderer, common.data(common.find(std::string("fonts\\") + name)));
    };
    stage.amerigo10 = font("Amerigo BT_10_.bmp");
    stage.amerigo14 = font("Amerigo BT_14_.bmp");
    stage.amerigo18 = font("Amerigo BT_18_.bmp");
    stage.lcd14 = font("LCD_14_.bmp");
    // check (0x00509d68) and cross (0x004ddc48) from the scene's bitmaps.
    stage.checkSurface =
        loadGreenKeyedBmp(package.data(package.find("bitmap\\check.bmp")));
    stage.crossSurface =
        loadGreenKeyedBmp(package.data(package.find("bitmap\\cross.bmp")));
    stage.checkTexture = makeTexture(renderer, stage.checkSurface.get());
    stage.crossTexture = makeTexture(renderer, stage.crossSurface.get());
    SDL_SetTextureBlendMode(stage.checkTexture.get(), SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(stage.crossTexture.get(), SDL_BLENDMODE_BLEND);
}

void PuzzleStageScene::puzzleAdd(PuzzleStage &stage,
                                 PuzzleDescriptor &descriptor, int x, int y,
                                 int layer) {
    // 0x00412840 ignores a descriptor that is already active (+0x78).
    if (descriptor.added) return;
    referenceTrace.emit("animation_add", {
        {"asset", descriptor.asset}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", descriptor.hotspot}, {"conflict_mask", 0}});
    if (x == -1 && y == -1) {
        x = descriptor.defaultX;
        y = descriptor.defaultY;
    }
    descriptor.x = x;
    descriptor.y = y;
    descriptor.layer = layer;
    descriptor.added = true;
    descriptor.order = ++stage.nextOrder;
    stage.list.push_back(&descriptor);
}

void PuzzleStageScene::puzzleRemove(PuzzleStage &stage,
                                    PuzzleDescriptor &descriptor) {
    if (!descriptor.added) return;
    // 0x00412aa0 is logged before the 0x00412a60 stop it calls.
    referenceTrace.emit("animation_remove",
                        {{"asset", descriptor.asset}, {"reason", "removed"}});
    referenceTrace.emit("animation_stop",
                        {{"asset", descriptor.asset}, {"reason", "stopped"}});
    descriptor.added = false;
    stage.list.erase(std::remove(stage.list.begin(), stage.list.end(),
                                 &descriptor),
                     stage.list.end());
}

void PuzzleStageScene::puzzleSetLayer(PuzzleStage &stage,
                                      PuzzleDescriptor &descriptor,
                                      int layer) {
    // 0x00412d50 writes +0x74 and re-sorts; it is not traced.
    (void)stage;
    descriptor.layer = layer;
}

std::uint32_t PuzzleStageScene::puzzleHotspotAt(const PuzzleStage &stage,
                                                int x, int y) const {
    // 0x004136c0 walks the sorted list from the top. A keyed descriptor
    // (mode 3) only hits on a non-key pixel, any other on its rectangle.
    std::vector<const PuzzleDescriptor *> sorted(stage.list.begin(),
                                                 stage.list.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PuzzleDescriptor *a, const PuzzleDescriptor *b) {
                         return a->layer != b->layer ? a->layer < b->layer
                                                     : a->order < b->order;
                     });
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        const auto &descriptor = **it;
        if (descriptor.hotspot == 0U) continue;
        const int localX = x - descriptor.x;
        const int localY = y - descriptor.y;
        if (localX < 0 || localY < 0 || localX >= descriptor.width ||
            localY >= descriptor.height) {
            continue;
        }
        if (descriptor.keyed && descriptor.pixels &&
            descriptor.pixels->at(localX, localY) == kPuzzleKey565) {
            continue;
        }
        return descriptor.hotspot;
    }
    return 0U;
}

void PuzzleStageScene::drawPuzzleStage(PuzzleStage &stage) {
    std::vector<PuzzleDescriptor *> sorted(stage.list.begin(),
                                           stage.list.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PuzzleDescriptor *a, const PuzzleDescriptor *b) {
                         return a->layer != b->layer ? a->layer < b->layer
                                                     : a->order < b->order;
                     });
    for (auto *descriptor : sorted) {
        if (descriptor->font) {
            if (!descriptor->text.empty()) {
                const SDL_Rect box{descriptor->x, descriptor->y,
                                   descriptor->width, descriptor->height};
                drawCenteredText(renderer, *descriptor->font, descriptor->text,
                                 box);
            }
            continue;
        }
        SDL_Texture *texture = descriptor->texture
                                   ? descriptor->texture
                                   : descriptor->ownedTexture.get();
        if (!texture) continue;
        const SDL_Rect destination{descriptor->x, descriptor->y,
                                   descriptor->width, descriptor->height};
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
        if (descriptor->labelFont && !descriptor->label.empty()) {
            // 0x004124e0 with flags 0x200: right-aligned, at the top.
            drawText(renderer, *descriptor->labelFont, descriptor->label,
                     descriptor->x + descriptor->width -
                         textWidth(*descriptor->labelFont, descriptor->label),
                     descriptor->y);
        }
    }

}

PuzzleStageScene::PuzzleControl &PuzzleStageScene::puzzleAddControl(
    PuzzleStage &stage, Package &package, int id, int x, int y,
    std::array<std::string, 4> names, const std::string &caption,
    const BitmapFont *font, int layer) {
    auto control = std::make_unique<PuzzleControl>();
    control->id = id;
    control->x = x;
    control->y = y;
    control->names = std::move(names);
    control->layer = layer;
    control->captionFont = font;
    for (std::size_t state = 0; state < 4; ++state) {
        const auto &name = control->names[state];
        if (name.empty()) continue;
        std::string asset = name;
        std::replace(asset.begin(), asset.end(), '/', '\\');
        const Package *source = &package;
        const Package::Entry *entry = nullptr;
        if (asset.rfind("..\\..\\common\\", 0) == 0) {
            asset.erase(0, 13);
            source = &common;
        } else {
            asset = "bitmap\\" + asset;
        }
        entry = source->findOptional(asset + ".bmp");
        if (!entry) continue;
        SurfacePtr surface = loadGreenKeyedBmp(source->data(*entry));
        control->widths[state] = surface->w;
        control->heights[state] = surface->h;
        control->textures[state] = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(control->textures[state].get(),
                                SDL_BLENDMODE_BLEND);
    }
    control->bitmap.hotspot = static_cast<std::uint32_t>(id);
    control->bitmap.defaultX = x;
    control->bitmap.defaultY = y;
    control->caption.font = font;
    control->caption.text = caption;
    auto &added = *control;
    stage.controls.push_back(std::move(control));
    refreshControl(stage, added);
    placeCaption(stage, added);
    return added;
}

void PuzzleStageScene::refreshControl(PuzzleStage &stage,
                                      PuzzleControl &control) {
    // 0x00406760: swap the descriptor's bitmap for the state's, re-adding it
    // only when the name changes.
    const auto state = static_cast<std::size_t>(std::clamp(control.state, 0, 3));
    const std::string &name = control.names[state];
    if (control.bitmap.added) {
        if (control.bitmap.asset == name) return;
        puzzleRemove(stage, control.bitmap);
    }
    if (name.empty()) return;
    control.bitmap.asset = name;
    control.bitmap.texture = control.textures[state].get();
    control.bitmap.width = control.widths[state];
    control.bitmap.height = control.heights[state];
    puzzleAdd(stage, control.bitmap, -1, -1, control.layer);
}

void PuzzleStageScene::placeCaption(PuzzleStage &stage,
                                    PuzzleControl &control) {
    // 0x00406950: a text box over the control's rectangle, centred
    // (flags 0x101), one layer above it.
    if (control.caption.added) puzzleRemove(stage, control.caption);
    if (control.caption.text.empty() || !control.captionFont) return;
    int width = control.widths[0];
    int height = control.heights[0];
    control.caption.width = width;
    control.caption.height = height;
    const int textWidthPixels =
        textWidth(*control.captionFont, control.caption.text);
    const int textHeight = control.captionFont->height;
    // 0x00407a30 sizes the box to the text and centres it in the rectangle.
    const int x = control.x + (width - textWidthPixels) / 2;
    const int y = control.y + (height - textHeight) / 2;
    control.caption.width = textWidthPixels;
    control.caption.height = textHeight;
    control.caption.asset.clear();
    puzzleAdd(stage, control.caption, x, y, control.layer + 1);
}

void PuzzleStageScene::puzzleSetControlState(PuzzleStage &stage, int id,
                                             int state) {
    for (auto &control : stage.controls) {
        if (control->id != id) continue;
        if (control->state == state) return;
        control->state = state;
        refreshControl(stage, *control);
        return;
    }
}

void PuzzleStageScene::puzzleSetControlCaption(PuzzleStage &stage, int id,
                                               const std::string &caption) {
    for (auto &control : stage.controls) {
        if (control->id != id) continue;
        control->caption.text = caption;
        placeCaption(stage, *control);
        return;
    }
}

void PuzzleStageScene::measureControls(PuzzleStage &stage) {
    // 0x00406b50 asks every control for its rectangle (0x004069b0). One whose
    // bitmap is not shown and whose size is not cached yet is measured by
    // adding and removing its first named bitmap (0x00406a50).
    for (auto &control : stage.controls) {
        if (control->bitmap.added) {
            control->sizeKnown = true;
            continue;
        }
        if (control->sizeKnown) continue;
        const auto named = std::find_if(control->names.begin(), control->names.end(),
                                        [](const std::string &name) { return !name.empty(); });
        if (named == control->names.end()) continue;
        const auto state = static_cast<std::size_t>(named - control->names.begin());
        control->bitmap.asset = *named;
        control->bitmap.texture = control->textures[state].get();
        control->bitmap.width = control->widths[state];
        control->bitmap.height = control->heights[state];
        puzzleAdd(stage, control->bitmap, -1, -1, control->layer);
        puzzleRemove(stage, control->bitmap);
        control->sizeKnown = true;
    }
}

bool PuzzleStageScene::puzzlePressControl(PuzzleStage &stage, int x, int y) {
    measureControls(stage);
    const SDL_Point point{x, y};
    for (auto &control : stage.controls) {
        const SDL_Rect bounds{control->x, control->y, control->widths[0],
                              control->heights[0]};
        if (!SDL_PointInRect(&point, &bounds)) continue;
        // 0x00406c50: only a control in state 0 can be pressed.
        if (control->state != 0) return false;
        control->state = 1;
        refreshControl(stage, *control);
        stage.pressedControl = control->id;
        return true;
    }
    return false;
}

std::optional<int> PuzzleStageScene::puzzleReleaseControl(PuzzleStage &stage,
                                                         int x, int y) {
    const int pressed = stage.pressedControl;
    stage.pressedControl = -1;
    if (pressed < 0) return std::nullopt;
    const SDL_Point point{x, y};
    for (auto &control : stage.controls) {
        if (control->id != pressed) continue;
        // 0x00406d40: back to state 0; the click (0x116) needs the pointer
        // still over the control (+0x274).
        if (control->state != 1) return std::nullopt;
        control->state = 0;
        refreshControl(stage, *control);
        const SDL_Rect bounds{control->x, control->y, control->widths[0],
                              control->heights[0]};
        if (!SDL_PointInRect(&point, &bounds)) return std::nullopt;
        return pressed;
    }
    return std::nullopt;
}

void PuzzleStageScene::openPuzzleChooser(PuzzleStage &stage,
                                         std::string_view scene,
                                         std::string_view captionKey,
                                         int firstPicture, int endPicture,
                                         int (*gridSize)(int)) {
    std::vector<PuzzleChooserRow> rows;
    const AwardInventory inventory = profileAwardInventory(puzzleProfilePath());
    // 0x0046b8a0 / 0x0046e8e0: one 0xee x 0x28 row per available picture.
    for (int picture = firstPicture; picture < endPicture; ++picture) {
        if (!pictureAvailable(picture, inventory)) continue;
        PuzzleChooserRow row;
        row.picture = picture;
        TexturePtr target(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET, 0xee,
                                            kChooserRowHeight));
        SDL_SetTextureBlendMode(target.get(), SDL_BLENDMODE_BLEND);
        SDL_Texture *previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, target.get());
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        // The small thumbnail at (2,5).
        const std::string small =
            "bitmap\\small\\" + pictureFileName(picture) + ".bmp";
        for (Package *package : {addonCommon.get(), data2Common.get(),
                                 editionCommon.get(), &common}) {
            if (!package) continue;
            const auto *entry = package->findOptional(small);
            if (!entry) continue;
            SurfacePtr thumb = loadBmp(package->data(*entry));
            TexturePtr texture = makeTexture(renderer, thumb.get());
            const SDL_Rect destination{2, 5, thumb->w, thumb->h};
            SDL_RenderCopy(renderer, texture.get(), nullptr, &destination);
            break;
        }
        // 0x0046b918: cross for a seen picture (2), check for a solved one (3).
        const std::uint32_t record = puzzlePictureRecord(scene, picture);
        SDL_Texture *mark = record == 2U   ? stage.crossTexture.get()
                            : record == 3U ? stage.checkTexture.get()
                                           : nullptr;
        if (mark) {
            int w = 0, h = 0;
            SDL_QueryTexture(mark, nullptr, nullptr, &w, &h);
            const SDL_Rect destination{0xa0, 6, w, h};
            SDL_RenderCopy(renderer, mark, nullptr, &destination);
        }
        drawText(renderer, stage.amerigo10,
                 language.get("awarddlg", pictureTitleKey(picture),
                              language.get("awarddlg", "NONE")),
                 0x28, 6);
        // 0x0046b97c / 0x0046e9d2: 0x0041ebd0(scene, picture, &list, NULL,
        // NULL, 0x102) reads only the active profile's time table; with rows,
        // the first (best) time in whole seconds (`idiv` by 1000), else 0.
        int seconds = 0;
        const fs::path profile = puzzleProfilePath();
        std::error_code missing;
        if (!profile.empty() && fs::exists(profile, missing)) {
            try {
                const auto table = profileSceneScores(
                    profile, scene, static_cast<std::uint32_t>(picture));
                if (table && !table->entries.empty()) {
                    seconds = static_cast<std::int32_t>(
                                  table->entries.front().score) / 1000;
                }
            } catch (const std::exception &) {
            }
        }
        char time[16];
        std::snprintf(time, sizeof time, "%02d:%02d", seconds / 60, seconds % 60);
        drawText(renderer, stage.amerigo10, time, 0x28, 0x16);
        char grid[16];
        std::snprintf(grid, sizeof grid, "%dx%d", gridSize(picture),
                      gridSize(picture));
        drawText(renderer, stage.amerigo18, grid, 0xbe, 6);
        SDL_SetRenderTarget(renderer, previous);
        row.texture = std::move(target);
        rows.push_back(std::move(row));
    }
    openPuzzleListDialog(stage,
                         language.get(std::string(scene), std::string(captionKey)),
                         std::move(rows), true);
}

void PuzzleStageScene::openPuzzleListDialog(PuzzleStage &stage,
                                            const std::string &title,
                                            std::vector<PuzzleChooserRow> rows,
                                            bool withCancel) {
    auto &chooser = stage.chooser;
    chooser = PuzzleChooser{};
    chooser.open = true;
    chooser.left = (800 - kChooserWidth) / 2;
    chooser.top = (600 - kChooserHeight) / 2;
    chooser.rows = std::move(rows);
    // 0x00408160: rows are as tall as the first row's surface, and as many
    // whole rows as fit are visible, at most 32.
    chooser.rowWidth = 0xee;
    chooser.rowHeight = kChooserRowHeight;
    if (!chooser.rows.empty() && chooser.rows.front().texture) {
        SDL_QueryTexture(chooser.rows.front().texture.get(), nullptr, nullptr,
                         &chooser.rowWidth, &chooser.rowHeight);
    }
    chooser.visibleRows =
        std::min(kChooserList.h / std::max(chooser.rowHeight, 1), 32);
    const auto load = [&](const char *name) {
        return loadGreenKeyedBmp(common.data(
            common.find(std::string("dialogue\\listdlg\\") + name + ".bmp")));
    };
    SurfacePtr backdrop = load("backdrop");
    chooser.backdrop.asset = "../../common/dialogue/listdlg/backdrop";
    chooser.backdrop.width = backdrop->w;
    chooser.backdrop.height = backdrop->h;
    chooser.backdrop.ownedTexture = makeTexture(renderer, backdrop.get());
    SDL_SetTextureBlendMode(chooser.backdrop.ownedTexture.get(),
                            SDL_BLENDMODE_BLEND);
    puzzleAdd(stage, chooser.backdrop, chooser.left, chooser.top, 100);
    // 0x00407333 -> 0x00407110: the modal mesh one layer below the dialog,
    // clipped to its rectangle (0x00413630), so it never shows.
    puzzleAdd(stage, chooser.mesh, -1, -1, 99);
    // 0x00407650 -> 0x00407a30: the localised caption in Amerigo BT_14_, a
    // text box the size of the text centred in the caption rectangle.
    chooser.title.font = &stage.amerigo14;
    chooser.title.text = title;
    chooser.title.width = textWidth(stage.amerigo14, chooser.title.text);
    chooser.title.height = stage.amerigo14.height;
    puzzleAdd(stage, chooser.title,
              chooser.left + kChooserTitle.x +
                  (kChooserTitle.w - chooser.title.width) / 2,
              chooser.top + kChooserTitle.y +
                  (kChooserTitle.h - chooser.title.height) / 2,
              kChooserLayer);
    // 0x00420550: Cancel when the flags allow it (2), then OK.
    const auto addButton = [&](int id, int x, const std::string &caption) {
        auto &control = puzzleAddControl(
            stage, *scenePackage, id, chooser.left + x, chooser.top + 0x1ab,
            {"../../common/dialogue/listdlg/button_0",
             "../../common/dialogue/listdlg/button_1", "", ""},
            caption, &stage.amerigo10, kChooserLayer);
        (void)control;
    };
    // 0x00420550: with Cancel (flags & 2) OK moves left to 0x7e, else 0xd7.
    if (withCancel) {
        addButton(kChooserCancel, 0xd7,
                  language.get("interfaceh", "CANCEL", "Avbryt"));
    }
    addButton(kChooserOk, withCancel ? 0x7e : 0xd7,
              language.get("interfaceh", "OK", "OK"));
    // 0x00420690 -> 0x004085a0: the rows and the selection bar.
    SurfacePtr bar(SDL_CreateRGBSurfaceWithFormat(0, chooser.rowWidth,
                                                  chooser.rowHeight, 32,
                                                  SDL_PIXELFORMAT_RGBA32));
    const std::uint32_t rgb = rgb565To888(rgb888To565(
        ((kChooserSelection & 0xffU) << 16U) | (kChooserSelection & 0xff00U) |
        ((kChooserSelection >> 16U) & 0xffU)));
    SDL_FillRect(bar.get(), nullptr,
                 SDL_MapRGBA(bar->format, (rgb >> 16U) & 0xffU,
                             (rgb >> 8U) & 0xffU, rgb & 0xffU, 0xffU));
    chooser.selection.ownedTexture = makeTexture(renderer, bar.get());
    chooser.selection.width = chooser.rowWidth;
    chooser.selection.height = chooser.rowHeight;
    for (auto &slot : chooser.rowSlots) {
        slot.width = chooser.rowWidth;
        slot.height = chooser.rowHeight;
    }
    scrollChooser(stage, 0);
    // 0x004205f0: the scroll bar 0x3ea's thumb, then its two arrows.
    SurfacePtr thumb = load("thumb");
    chooser.thumb.asset = "../../common/dialogue/listdlg/thumb";
    chooser.thumb.hotspot = 0x3ea;
    chooser.thumb.width = thumb->w;
    chooser.thumb.height = thumb->h;
    chooser.thumb.ownedTexture = makeTexture(renderer, thumb.get());
    SDL_SetTextureBlendMode(chooser.thumb.ownedTexture.get(),
                            SDL_BLENDMODE_BLEND);
    puzzleAdd(stage, chooser.thumb, -1, -1, kChooserLayer);
    scrollChooser(stage, 0);
    chooser.upId = stage.nextScrollControl++;
    chooser.downId = stage.nextScrollControl++;
    puzzleAddControl(stage, *scenePackage, chooser.upId,
                     chooser.left + kChooserUp.x, chooser.top + kChooserUp.y,
                     {"../../common/dialogue/listdlg/up_0",
                      "../../common/dialogue/listdlg/up_1", "", ""},
                     "", nullptr, kChooserLayer);
    puzzleAddControl(stage, *scenePackage, chooser.downId,
                     chooser.left + kChooserDown.x, chooser.top + kChooserDown.y,
                     {"../../common/dialogue/listdlg/down_0",
                      "../../common/dialogue/listdlg/down_1", "", ""},
                     "", nullptr, kChooserLayer);
}

void PuzzleStageScene::scrollChooser(PuzzleStage &stage, int firstVisible) {
    auto &chooser = stage.chooser;
    const int rows = static_cast<int>(chooser.rows.size());
    // 0x00407af0: clamp so the last page is full; redraw only on a change,
    // except for the first draw (0x00407b40 from 0x004085a0).
    const int maximum = std::max(0, rows - chooser.visibleRows);
    const int clamped = std::clamp(firstVisible, 0, maximum);
    const bool firstDraw = !chooser.rowSlots[0].added && rows > 0;
    if (clamped != chooser.firstVisible || firstDraw) {
        chooser.firstVisible = clamped;
        // 0x00407c00: every visible row descriptor is re-added.
        for (int row = 0; row < chooser.visibleRows; ++row) {
            const int index = chooser.firstVisible + row;
            if (index >= rows) break;
            auto &slot = chooser.rowSlots[static_cast<std::size_t>(row)];
            puzzleRemove(stage, slot);
            slot.texture = chooser.rows[static_cast<std::size_t>(index)].texture.get();
            puzzleAdd(stage, slot, chooser.left + kChooserList.x,
                      chooser.top + kChooserList.y + row * chooser.rowHeight,
                      kChooserLayer + 1);
        }
        placeChooserSelection(stage);
    }
    // 0x00408eb0 / 0x00408f30: the thumb centre tracks the value over the
    // track's height.
    if (chooser.thumb.added) {
        const int position = maximum > 0
            ? kChooserScroll.h * chooser.firstVisible / maximum + kChooserScroll.y
            : kChooserScroll.y;
        chooser.thumb.x = chooser.left + kChooserScroll.x + kChooserScroll.w / 2 -
                          chooser.thumb.width / 2;
        chooser.thumb.y = chooser.top + position - chooser.thumb.height / 2;
    }
}

void PuzzleStageScene::placeChooserSelection(PuzzleStage &stage) {
    // 0x00407b60: move the bar to the selected row, add it when it was not
    // shown, remove it when the row is scrolled away (0x00407be0).
    auto &chooser = stage.chooser;
    if (chooser.rows.empty()) return;
    const int row = chooser.selected - chooser.firstVisible;
    if (row < 0 || row >= chooser.visibleRows) {
        puzzleRemove(stage, chooser.selection);
        return;
    }
    const int x = chooser.left + kChooserList.x;
    const int y = chooser.top + kChooserList.y + row * chooser.rowHeight;
    if (chooser.selection.added) {
        chooser.selection.x = x;
        chooser.selection.y = y;
        return;
    }
    puzzleAdd(stage, chooser.selection, x, y, kChooserLayer);
}

bool PuzzleStageScene::puzzleChooserPress(PuzzleStage &stage, int x, int y) {
    auto &chooser = stage.chooser;
    if (!chooser.open) return false;
    // 0x004209f0: controls, the scroll bar, then the list.
    if (puzzlePressControl(stage, x, y)) return true;
    const SDL_Point point{x, y};
    const SDL_Rect track{chooser.left + kChooserScroll.x - chooser.thumb.width / 2,
                         chooser.top + kChooserScroll.y,
                         kChooserScroll.w + chooser.thumb.width,
                         kChooserScroll.h};
    const SDL_Rect thumb{chooser.thumb.x, chooser.thumb.y, chooser.thumb.width,
                         chooser.thumb.height};
    if (SDL_PointInRect(&point, &track) || SDL_PointInRect(&point, &thumb)) {
        chooser.draggingThumb = true;
        puzzleChooserMove(stage, x, y);
        return true;
    }
    const SDL_Rect list{chooser.left + kChooserList.x,
                        chooser.top + kChooserList.y, kChooserList.w,
                        kChooserList.h};
    if (SDL_PointInRect(&point, &list)) {
        // 0x00407f00: row = (y - top) / row height + first visible.
        const int row = (y - list.y) / chooser.rowHeight + chooser.firstVisible;
        // 0x00407ec0 -> 0x00407e60 for a new row.
        if (row >= 0 && row < static_cast<int>(chooser.rows.size()) &&
            row != chooser.selected) {
            chooser.selected = row;
            placeChooserSelection(stage);
        }
    }
    return true;
}

void PuzzleStageScene::puzzleChooserMove(PuzzleStage &stage, int, int y) {
    auto &chooser = stage.chooser;
    if (!chooser.open || !chooser.draggingThumb) return;
    const int maximum =
        std::max(0, static_cast<int>(chooser.rows.size()) - chooser.visibleRows);
    const int top = chooser.top + kChooserScroll.y;
    const int position = std::clamp(y, top, top + kChooserScroll.h);
    if (maximum == 0) return;
    scrollChooser(stage, (position - top) * maximum / kChooserScroll.h);
}

void PuzzleStageScene::puzzleChooserRelease(PuzzleStage &stage, int x, int y) {
    auto &chooser = stage.chooser;
    if (!chooser.open) return;
    chooser.draggingThumb = false;
    const auto released = puzzleReleaseControl(stage, x, y);
    if (!released) return;
    // 0x00420920: OK and Cancel end the modal loop; the arrows step the
    // scroll value by one (0x00409450).
    if (*released == kChooserOk) {
        chooser.result = 1;
    } else if (*released == kChooserCancel) {
        chooser.result = 2;
    } else if (*released == chooser.upId) {
        scrollChooser(stage, chooser.firstVisible - 1);
    } else if (*released == chooser.downId) {
        scrollChooser(stage, chooser.firstVisible + 1);
    }
}

void PuzzleStageScene::puzzleChooserKey(PuzzleStage &stage, SDL_Keycode key) {
    auto &chooser = stage.chooser;
    if (!chooser.open) return;
    const int rows = static_cast<int>(chooser.rows.size());
    // 0x00420a30 and 0x00407f60.
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        chooser.result = 1;
        return;
    }
    if (key == SDLK_ESCAPE) {
        chooser.result = 2;
        return;
    }
    if (rows <= 0) return;
    int selected = chooser.selected;
    switch (key) {
    case SDLK_UP: selected -= 1; break;
    case SDLK_DOWN: selected += 1; break;
    case SDLK_HOME: selected = 0; break;
    case SDLK_END: selected = rows - 1; break;
    case SDLK_PAGEUP:
        scrollChooser(stage, chooser.firstVisible - chooser.visibleRows);
        return;
    case SDLK_PAGEDOWN:
        scrollChooser(stage, chooser.firstVisible + chooser.visibleRows);
        return;
    default: return;
    }
    // 0x00407e60: keep the selection on screen.
    selected = std::clamp(selected, 0, rows - 1);
    chooser.selected = selected;
    if (selected < chooser.firstVisible) {
        scrollChooser(stage, selected);
    } else if (selected >= chooser.firstVisible + chooser.visibleRows) {
        scrollChooser(stage, selected + 1 - chooser.visibleRows);
    } else {
        placeChooserSelection(stage);
    }
}

int PuzzleStageScene::closePuzzleChooser(PuzzleStage &stage) {
    auto &chooser = stage.chooser;
    if (!chooser.open) return -1;
    const int result = chooser.result;
    const int selected = chooser.selected;
    // 0x00420430's tail: the dialog's controls in creation order (OK and its
    // caption, Cancel and its caption), its caption text, the scroll bar
    // (0x00408700), the backdrop, then the list's descriptors.
    const auto removeControl = [&](int id) {
        for (auto it = stage.controls.begin(); it != stage.controls.end(); ++it) {
            if ((*it)->id != id) continue;
            puzzleRemove(stage, (*it)->bitmap);
            puzzleRemove(stage, (*it)->caption);
            stage.controls.erase(it);
            return;
        }
    };
    removeControl(kChooserOk);
    removeControl(kChooserCancel);
    puzzleRemove(stage, chooser.title);
    puzzleRemove(stage, chooser.thumb);
    removeControl(chooser.downId);
    removeControl(chooser.upId);
    puzzleRemove(stage, chooser.backdrop);
    puzzleRemove(stage, chooser.mesh);
    puzzleRemove(stage, chooser.selection);
    for (auto &slot : chooser.rowSlots) puzzleRemove(stage, slot);
    std::vector<PuzzleChooserRow> rows = std::move(chooser.rows);
    chooser = PuzzleChooser{};
    // 0x00420430: OK with a valid selection returns its row's picture.
    if (result == 1 && selected >= 0 &&
        selected < static_cast<int>(rows.size())) {
        return rows[static_cast<std::size_t>(selected)].picture;
    }
    return -1;
}

void PuzzleStageScene::puzzlePlayClone(PuzzleStage &stage,
                                       const std::string &asset) {
    auto sound = stage.sounds.find(asset);
    if (sound == stage.sounds.end()) {
        try {
            sound = stage.sounds
                        .emplace(asset, decodeSmacker(activeSceneAnimationData(asset),
                                                      true, true, true))
                        .first;
        } catch (const std::exception &error) {
            std::cerr << "Puzzle sound " << asset << ": " << error.what() << '\n';
            return;
        }
    }
    // 0x00412c20: the clone is added at its descriptor's (0,0), layer 0.
    referenceTrace.emit("animation_add", {
        {"asset", asset}, {"x", 0}, {"y", 0}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    PuzzleCloneSound clone;
    clone.asset = asset;
    clone.handle = audioMixer.addEffect(
        sound->second, audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    clone.endsAt = sceneElapsedNow() +
                   originalSmackerCompletionDuration(sound->second);
    stage.clones.push_back(std::move(clone));
}

void PuzzleStageScene::tickPuzzleClones(PuzzleStage &stage, double now) {
    for (auto clone = stage.clones.begin(); clone != stage.clones.end();) {
        if (now < clone->endsAt) {
            ++clone;
            continue;
        }
        referenceTrace.emit("animation_stop", {
            {"asset", clone->asset}, {"reason", "stopped"}});
        referenceTrace.emit("animation_remove", {
            {"asset", clone->asset}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", clone->asset}, {"reason", "stopped"}});
        clone = stage.clones.erase(clone);
    }
}

} // namespace fgp
