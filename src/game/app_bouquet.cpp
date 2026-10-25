#include "game/app_bouquet.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#include "assets/profile.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// 0x005031b0: the 22 flower names, indexed by bed hotspot - 100. The package
// also carries white_honeysuckle, which no table names.
const auto kFlowerNames = originalArray<std::string_view, 22>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x005031b0U + static_cast<std::uint32_t>(index * 4);
    return source.string(source.read<std::uint32_t>(address));
});

// 0x0049f7b0: each flower's offset in the vase, per rotation frame
// (index type * 5 + frame).
const auto kFlowerOffsets = originalArray<std::array<SDL_Point, 5>, 22>(0x0049f7b0U);

// 0x0049f718: each vase's offset from the vase origin at 0x0049f710.
const auto kVaseOffsets = originalArray<SDL_Point, 18>(0x0049f718U);
constexpr SDL_Point kVaseOrigin{499, 360};   // 0x0049f710
constexpr SDL_Point kFlowerOrigin{376, 137}; // 0x0049f7a8

// 0x0049f6c0: the five vase zones, left/top/right/bottom (exclusive), that
// pick a flower's rotation frame while it is held over the vase.
const auto kZones = originalArray<SDL_Rect, 5>(0x0049f6c0U);

constexpr int kVaseLayer = 300;        // 0x0045e44f
constexpr int kSmallLayer = 450;       // 0x0045e70c
constexpr std::uint32_t kVaseHotspot = 2000; // 0x00503450
constexpr std::uint32_t kFlowerHotspotBase = 1000; // 0x0045e3cb

struct ControlSpec {
    std::uint32_t id;
    int x;
    int y;
    std::string_view stem;
};
// 0x0045e2d3-0x0045e365: the controls 0x00406ed0 creates after help (1)
// and exit (2), which the shared corner buttons provide.
constexpr std::array<ControlSpec, 4> kControls{{
    {3, 0, 528, "print"},
    {30, 520, 539, "prev"},
    {31, 640, 539, "next"},
    {32, 747, 520, "clear"},
}};

struct BoatSpec {
    std::string_view name;
    int x;
    int y;
    int layer;
};
// Descriptors 0x00503208 and 0x005032b0.
const auto kBoats = originalArray<BoatSpec, 2>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x00503208U + static_cast<std::uint32_t>(index * 168);
    return BoatSpec{source.string(address), source.read<int>(address + 0x54), source.read<int>(address + 0x58), source.read<int>(address + 0x64)};
});

constexpr std::size_t kSaveSize = 0xf8;

std::uint32_t topLeftKey(SDL_Surface *surface) {
    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
    const std::uint32_t key = pixelValue(surface, 0, 0);
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
    return key;
}

TexturePtr boatTexture(SDL_Renderer *renderer, const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame = media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        frame.width, frame.height));
    if (!texture) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    return texture;
}

void writeU32(std::array<std::uint8_t, kSaveSize> &bytes, std::size_t offset,
              std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

std::int32_t readI32(const std::vector<std::uint8_t> &bytes,
                     std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index])
                 << (index * 8U);
    }
    return static_cast<std::int32_t>(value);
}

} // namespace

void BouquetScene::loadBouquetGame(Package &package) {
    // The sprites point into these arrays, so the game is built in place.
    auto &game = bouquetGame.emplace();
    const auto image = [&](const std::string &asset) {
        BouquetImage loaded;
        // Every Bouquet bitmap has pure green at (0,0), the key SCENE draws
        // a keyed (+0x48) descriptor with.
        loaded.surface = loadGreenKeyedBmp(package.data(package.find(asset)));
        loaded.texture = makeTexture(renderer, loaded.surface.get());
        SDL_SetTextureBlendMode(loaded.texture.get(), SDL_BLENDMODE_BLEND);
        return loaded;
    };
    for (std::size_t vase = 0; vase < kBouquetVases; ++vase) {
        game.vases[vase] = image("bitmap\\vases\\big_" +
                                 std::to_string(vase + 1) + ".bmp");
    }
    // 0x0045e7a0 preloads "flowers\%s_%d" for frames 1..5; the dragged icon
    // "small\%s" is loaded by SCENE_AddAnim on every pick-up.
    for (std::size_t type = 0; type < kBouquetFlowerTypes; ++type) {
        const std::string name(kFlowerNames[type]);
        for (std::size_t frame = 0; frame < kBouquetFrames; ++frame) {
            game.flowers[type][frame] = image(
                "bitmap\\flowers\\" + name + "_" +
                std::to_string(frame + 1) + ".bmp");
        }
        game.small[type] = image("bitmap\\small\\" + name + ".bmp");
    }
    for (const auto &spec : kControls) {
        BouquetControl control;
        control.id = spec.id;
        control.stem = std::string(spec.stem);
        control.x = spec.x;
        control.y = spec.y;
        control.normal =
            image("bitmap\\" + std::string(spec.stem) + "_0.bmp");
        control.pressed =
            image("bitmap\\" + std::string(spec.stem) + "_1.bmp");
        game.controls.push_back(std::move(control));
    }
    for (std::size_t boat = 0; boat < kBoats.size(); ++boat) {
        auto &clip = game.boats[boat].clip;
        clip.media = decodeActiveSceneClip(std::string(kBoats[boat].name));
        clip.texture = boatTexture(renderer, clip.media);
    }
    game.vase.hotspot = kVaseHotspot;
    for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
        // 0x0045e3b0: descriptor zeroed, visible and keyed, hotspot 1000+i.
        game.slots[slot].sprite.hotspot =
            kFlowerHotspotBase + static_cast<std::uint32_t>(slot);
    }
    std::cout << "Loaded executable-derived Bouquet: 22 flowers x 5 frames, "
                 "18 vases, 30 slots, five vase zones\n";
}

void BouquetScene::enterBouquet() {
    // 0x0045e200 (event 0x103), run on the first update so that its adds
    // follow the scene change.
    auto &game = *bouquetGame;
    game.entered = true;
    game.dragged.reset();
    game.raiseCounter = 0;
    setBouquetVase(0);
    restoreBouquet();
    // 0x0045e22f-0x0045e252: the first boat is due 60-180 s after entry
    // (0x0040c550), then every 180 s.
    game.nextBoatAt =
        sceneElapsedNow() +
        std::uniform_int_distribution<int>(60000, 179999)(randomEngine) /
            1000.0;
    // 0x0045e285: the title text control, a zeroed descriptor that
    // 0x004124e0 centres both ways in the rectangle (295,3)-(503,36) at
    // 0x0049fb20 (truncating halves), on layer 0.
    const std::string title =
        language.get("bouquet", "SCENENAME", "Ludvigs blomsterbinderi");
    const int titleWidth = textWidth(calendarFont, title);
    referenceTrace.emit("animation_add", {
        {"asset", ""}, {"x", 295 + (208 - titleWidth) / 2},
        {"y", 3 + (33 - calendarFont.height) / 2}, {"layer", 0},
        {"hotspot", 0}, {"conflict_mask", 0}});
    // 0x0045e28f-0x0045e365: 0x00406ed0 adds each control's _0 image at its
    // own position on layer 1 (0x004067fd), help and exit first.
    for (const auto &[id, stem] :
         {std::pair<std::uint32_t, std::string_view>{1, "help"},
          {2, "exit"}}) {
        traceBouquetControl(std::string(stem) + "_0", id, true);
    }
    for (const auto &control : game.controls) {
        traceBouquetControl(control.stem + "_0", control.id, true);
    }
    // 0x0045e397: the car part at layer -10 while record key 0 is 0.
    const bool carpartVisible = std::any_of(
        sceneCollectibles.begin(), sceneCollectibles.end(),
        [](const SceneCollectible &collectible) {
            return collectible.name == "carpart" && collectible.visible;
        });
    if (carpartVisible) {
        referenceTrace.emit("animation_add", {
            {"asset", "carpart"}, {"x", -1}, {"y", -1}, {"layer", -10},
            {"hotspot", 4}, {"conflict_mask", 0}});
    }
}

void BouquetScene::traceBouquetControl(const std::string &image,
                                       std::uint32_t id, bool add) {
    // A type-1 control swaps its descriptor between <name>_0 and <name>_1:
    // removed, then re-added with x and y -1 on layer 1.
    if (add) {
        referenceTrace.emit("animation_add", {
            {"asset", image}, {"x", -1}, {"y", -1}, {"layer", 1},
            {"hotspot", id}, {"conflict_mask", 0}});
    } else {
        referenceTrace.emit("animation_remove", {
            {"asset", image}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", image}, {"reason", "stopped"}});
    }
}

void BouquetScene::addBouquetSprite(BouquetSprite &sprite, int x, int y,
                                    int layer) {
    if (!bouquetGame) return;
    // SCENE_AddAnim 0x00412840; its logpoint reports the arguments.
    referenceTrace.emit("animation_add", {
        {"asset", sprite.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", sprite.hotspot}, {"conflict_mask", 0}});
    sprite.added = true;
    sprite.x = x;
    sprite.y = y;
    sprite.layer = layer;
    sprite.order = ++bouquetGame->nextOrder;
}

void BouquetScene::removeBouquetSprite(BouquetSprite &sprite) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60.
    referenceTrace.emit("animation_remove", {
        {"asset", sprite.name}, {"reason", "removed"}});
    referenceTrace.emit("animation_stop", {
        {"asset", sprite.name}, {"reason", "stopped"}});
    sprite.added = false;
}

void BouquetScene::setBouquetVase(int vase) {
    // 0x0045e400.
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    bouquetGame->vaseIndex = vase;
    if (game.vase.added) removeBouquetSprite(game.vase);
    game.vase.name = "vases\\big_" + std::to_string(vase + 1);
    game.vase.image = &game.vases[static_cast<std::size_t>(vase)];
    addBouquetSprite(game.vase,
                     kVaseOrigin.x + kVaseOffsets[static_cast<std::size_t>(vase)].x,
                     kVaseOrigin.y + kVaseOffsets[static_cast<std::size_t>(vase)].y,
                     kVaseLayer);
    // 0x0045e47d-0x0045e4b9 move every flower with the vase (0x00412cc0,
    // not traced).
    for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
        if (!game.slots[slot].inUse || bouquetSlotMemory[slot].frame < 0) {
            continue;
        }
        const SDL_Point position = bouquetFlowerPosition(slot);
        game.slots[slot].sprite.x = position.x;
        game.slots[slot].sprite.y = position.y;
    }
}

SDL_Point BouquetScene::bouquetFlowerPosition(std::size_t slot) const {
    // 0x0045e4c0.
    const auto &memory = bouquetSlotMemory[slot];
    const SDL_Point flower =
        kFlowerOffsets[static_cast<std::size_t>(memory.type)]
                      [static_cast<std::size_t>(memory.frame)];
    const SDL_Point vase = kVaseOffsets[static_cast<std::size_t>(bouquetGame->vaseIndex)];
    return {flower.x + vase.x + memory.offsetX + kFlowerOrigin.x,
            flower.y + vase.y + memory.offsetY + kFlowerOrigin.y};
}

int BouquetScene::bouquetFlowerLayer(std::size_t slot) const {
    // 0x0045e650: fern and evergreen (types 9 and 10) in their middle three
    // frames stand in front of the vase; everything else behind it.
    const auto &memory = bouquetSlotMemory[slot];
    const bool front = (memory.type == 9 || memory.type == 10) &&
                       memory.frame >= 1 && memory.frame <= 3;
    return memory.layer + (front ? 400 : 50);
}

void BouquetScene::raiseBouquetFlower(std::size_t slot) {
    // 0x0045e5c0: every raise takes the next order number; past 30 the
    // others are renumbered down by 30. 0x00412d50 (set layer) is not traced.
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    if (bouquetGame->raiseCounter > 30) {
        for (std::size_t other = 0; other < kBouquetSlots; ++other) {
            if (!game.slots[other].inUse || other == slot) continue;
            bouquetSlotMemory[other].layer -= 30;
            game.slots[other].sprite.layer = bouquetFlowerLayer(other);
            game.slots[other].sprite.order = ++game.nextOrder;
        }
        bouquetGame->raiseCounter -= 30;
    }
    bouquetSlotMemory[slot].layer = bouquetGame->raiseCounter++;
    game.slots[slot].sprite.layer = bouquetFlowerLayer(slot);
    game.slots[slot].sprite.order = ++game.nextOrder;
}

std::optional<std::size_t>
BouquetScene::allocateBouquetFlower(int type, int frame, int x, int y) {
    // 0x0045e690: the first free slot (0x0045e6e0), or none.
    if (!bouquetGame) return std::nullopt;
    auto &game = *bouquetGame;
    for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
        if (game.slots[slot].inUse) continue;
        auto &memory = bouquetSlotMemory[slot];
        memory.offsetX = 0;
        memory.offsetY = 0;
        memory.type = type;
        game.slots[slot].inUse = true;
        setBouquetFlowerFrame(slot, frame, x, y);
        return slot;
    }
    return std::nullopt;
}

void BouquetScene::setBouquetFlowerFrame(std::size_t slot, int frame, int x,
                                         int y) {
    // 0x0045e700: a frame >= 0 places the preloaded bitmap at its fixed spot
    // in the vase under the empty descriptor name; frame -1 shows the small
    // bed icon at (x, y) on layer 450.
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    auto &memory = bouquetSlotMemory[slot];
    auto &sprite = game.slots[slot].sprite;
    memory.frame = frame;
    if (sprite.added) removeBouquetSprite(sprite);
    const auto type = static_cast<std::size_t>(memory.type);
    int layer = kSmallLayer;
    if (frame >= 0) {
        const SDL_Point position = bouquetFlowerPosition(slot);
        x = position.x;
        y = position.y;
        layer = bouquetFlowerLayer(slot);
        sprite.name.clear();
        sprite.image = &game.flowers[type][static_cast<std::size_t>(frame)];
    } else {
        sprite.name = "small\\" + std::string(kFlowerNames[type]);
        sprite.image = &game.small[type];
    }
    addBouquetSprite(sprite, x, y, layer);
}

void BouquetScene::centreBouquetSprite(BouquetSprite &sprite, int x, int y) {
    // 0x00412cf0 centres the descriptor on (x, y); not traced.
    if (!sprite.image || !sprite.image->surface) return;
    sprite.x = x - sprite.image->surface->w / 2;
    sprite.y = y - sprite.image->surface->h / 2;
}

void BouquetScene::freeBouquetFlower(std::size_t slot) {
    // 0x0045ea80.
    if (!bouquetGame) return;
    auto &entry = bouquetGame->slots[slot];
    if (entry.sprite.added) removeBouquetSprite(entry.sprite);
    entry.inUse = false;
}

void BouquetScene::removeBouquetDuplicates(std::size_t slot) {
    // 0x0045eab0: a flower dropped where the same flower already stands in
    // the same frame replaces it.
    if (!bouquetGame) return;
    for (std::size_t other = 0; other < kBouquetSlots; ++other) {
        if (other == slot || !bouquetGame->slots[other].inUse) continue;
        if (bouquetSlotMemory[other].type == bouquetSlotMemory[slot].type &&
            bouquetSlotMemory[other].frame == bouquetSlotMemory[slot].frame) {
            freeBouquetFlower(other);
        }
    }
}

std::uint32_t BouquetScene::bouquetSpriteHotspotAt(int x, int y,
                                                   bool &hit) const {
    // 0x004136c0 walks the layer-sorted list from the top; mode 3 (keyed,
    // 0x00413881) hits where the pixel differs from the top-left one.
    hit = false;
    if (!bouquetGame) return 0;
    const auto &game = *bouquetGame;
    std::vector<const BouquetSprite *> sprites;
    if (game.vase.added) sprites.push_back(&game.vase);
    for (const auto &slot : game.slots) {
        if (slot.inUse && slot.sprite.added) sprites.push_back(&slot.sprite);
    }
    std::stable_sort(sprites.begin(), sprites.end(),
                     [](const BouquetSprite *left, const BouquetSprite *right) {
                         return left->layer != right->layer
                                    ? left->layer > right->layer
                                    : left->order > right->order;
                     });
    for (const auto *sprite : sprites) {
        if (!sprite->image || !sprite->image->surface) continue;
        SDL_Surface *surface = sprite->image->surface.get();
        const int localX = x - sprite->x;
        const int localY = y - sprite->y;
        if (localX < 0 || localY < 0 || localX >= surface->w ||
            localY >= surface->h) {
            continue;
        }
        if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
        const bool opaque =
            pixelValue(surface, localX, localY) != topLeftKey(surface);
        if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
        if (opaque) {
            hit = true;
            return sprite->hotspot;
        }
    }
    return 0;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
BouquetScene::bouquetHotspotAt(int x, int y) const {
    bool hit = false;
    const std::uint32_t hotspot = bouquetSpriteHotspotAt(x, y, hit);
    if (!hit) return std::nullopt;
    return std::pair{hotspot, SDL_Rect{x, y, 1, 1}};
}

std::optional<std::size_t> BouquetScene::bouquetControlAt(int x,
                                                          int y) const {
    if (!bouquetGame) return std::nullopt;
    const SDL_Point point{x, y};
    for (std::size_t index = 0; index < bouquetGame->controls.size();
         ++index) {
        const auto &control = bouquetGame->controls[index];
        const SDL_Rect bounds{control.x, control.y,
                              control.normal.surface->w,
                              control.normal.surface->h};
        if (SDL_PointInRect(&point, &bounds)) return index;
    }
    return std::nullopt;
}

bool BouquetScene::bouquetDragging() const {
    return bouquetGame &&
           (bouquetGame->dragged.has_value() ||
            bouquetGame->pressedControl.has_value());
}

bool BouquetScene::beginBouquetInteraction(int x, int y) {
    if (!bouquetGame) return false;
    auto &game = *bouquetGame;
    if (const auto control = bouquetControlAt(x, y)) {
        // Pressing shows the _1 image (0x00406d40 on mouse down).
        game.pressedControl = control;
        const auto &pressed = game.controls[*control];
        traceBouquetControl(pressed.stem + "_0", pressed.id, false);
        traceBouquetControl(pressed.stem + "_1", pressed.id, true);
        return true;
    }
    bool hit = false;
    std::uint32_t hotspot = bouquetSpriteHotspotAt(x, y, hit);
    if (!hit) {
        hotspot = hotspotAt(x, y);
        // The hidden car part (hotspot 4) is the shared collectible.
        if (hotspot == 4U) return false;
    }
    pressBouquet(hotspot, x, y);
    return true;
}

void BouquetScene::pressBouquet(std::uint32_t hotspot, int x, int y) {
    // 0x0045eb00 (event 0x105).
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    if (hotspot >= 100U && hotspot < 122U) {
        // 0x0045ebc0: a bed flower starts as its small icon under the
        // pointer, or the vase is full.
        const auto slot = allocateBouquetFlower(
            static_cast<int>(hotspot - 100U), -1, x, y);
        if (!slot) {
            showMessageWithTitle(
                language.get("interfaceh", "WARNING", "Advarsel"),
                language.get("bouquet", "FLOWERSMAX",
                             "Du har fylt opp vasen."));
            return;
        }
        centreBouquetSprite(game.slots[*slot].sprite, x, y);
        raiseBouquetFlower(*slot);
        game.dragged = *slot;
    } else if (hotspot >= kFlowerHotspotBase &&
               hotspot < kFlowerHotspotBase + kBouquetSlots) {
        // 0x0045eba0: a flower in the vase comes to the front and is held.
        const std::size_t slot = hotspot - kFlowerHotspotBase;
        raiseBouquetFlower(slot);
        game.dragged = slot;
    }
}

namespace {
int bouquetZoneAt(int x, int y) {
    // 0x0045e9e0 with 0x00412310 (right and bottom exclusive).
    for (std::size_t zone = 0; zone < kZones.size(); ++zone) {
        const auto &rect = kZones[zone];
        if (rect.x <= x && x < rect.w && rect.y <= y && y < rect.h) {
            return static_cast<int>(zone);
        }
    }
    return -1;
}
} // namespace

void BouquetScene::moveBouquetInteraction(int x, int y) {
    // 0x0045e960 (event 0x109): over a zone the held flower takes that
    // zone's frame; elsewhere it is the small icon following the pointer.
    if (!bouquetGame || !bouquetGame->dragged) return;
    const std::size_t slot = *bouquetGame->dragged;
    const int zone = bouquetZoneAt(x, y);
    if (bouquetSlotMemory[slot].frame != zone) {
        setBouquetFlowerFrame(slot, zone, x, y);
    }
    if (zone < 0) {
        centreBouquetSprite(bouquetGame->slots[slot].sprite, x, y);
    }
}

void BouquetScene::endBouquetInteraction(int x, int y) {
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    if (game.pressedControl) {
        const std::size_t pressed = *game.pressedControl;
        game.pressedControl.reset();
        // Release restores _0, then the control posts 0x116.
        // UNCERTAIN: whether a release off the control also restores _0
        // without posting 0x116; no capture has released off a control.
        traceBouquetControl(game.controls[pressed].stem + "_1",
                            game.controls[pressed].id, false);
        traceBouquetControl(game.controls[pressed].stem + "_0",
                            game.controls[pressed].id, true);
        if (bouquetControlAt(x, y) == pressed) {
            activateBouquetControl(game.controls[pressed].id);
        }
        return;
    }
    // 0x0045ea20 (event 0x106): dropped over the vase it stays, replacing
    // an identical flower; anywhere else it goes back to the bed.
    if (!game.dragged) return;
    const std::size_t slot = *game.dragged;
    if (bouquetZoneAt(x, y) >= 0) {
        removeBouquetDuplicates(slot);
    } else {
        freeBouquetFlower(slot);
    }
    game.dragged.reset();
}

void BouquetScene::activateBouquetControl(std::uint32_t id) {
    // 0x0045ec10 (event 0x116).
    if (!bouquetGame) return;
    if (id == 30U || id == 31U) {
        // 0x0045ecf0: (vase + step + 18) % 18.
        const int step = id == 30U ? -1 : 1;
        setBouquetVase((bouquetGame->vaseIndex + step + 18) % 18);
    } else if (id == 32U) {
        clearBouquet();
    } else if (id == 3U) {
        // 0x0045ec49: hand the arrangement to the activity centre with
        // return scene "bouquet" and mode 0.
        activityPicture = composeBouquetPicture();
        activityReturnScene = "bouquet";
        enterScene("activity", "activity");
    }
}

void BouquetScene::clearBouquet() {
    // 0x0045ecc0. The raise counter is left as it is.
    if (!bouquetGame) return;
    for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
        if (bouquetGame->slots[slot].inUse) freeBouquetFlower(slot);
    }
}

std::vector<std::size_t> BouquetScene::sortedBouquetFlowers() const {
    // 0x0045e8b0: placed flowers, sorted by layer (0x0045e900).
    std::vector<std::size_t> flowers;
    if (!bouquetGame) return flowers;
    for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
        if (bouquetGame->slots[slot].inUse &&
            bouquetSlotMemory[slot].frame >= 0) {
            flowers.push_back(slot);
        }
    }
    std::stable_sort(flowers.begin(), flowers.end(),
                     [&](std::size_t left, std::size_t right) {
                         return bouquetGame->slots[left].sprite.layer <
                                bouquetGame->slots[right].sprite.layer;
                     });
    return flowers;
}

void BouquetScene::saveBouquet() {
    // 0x0045e830: record 1 is {vase, count, type[30], frame[30]}.
    if (!bouquetGame || profiles[profileIndex] == "No profile") return;
    const fs::path path =
        profileDirectory / (profiles[profileIndex] + ".pfl");
    if (!fs::exists(path)) return;
    std::array<std::uint8_t, kSaveSize> bytes{};
    const auto flowers = sortedBouquetFlowers();
    writeU32(bytes, 0, static_cast<std::uint32_t>(bouquetGame->vaseIndex));
    writeU32(bytes, 4, static_cast<std::uint32_t>(flowers.size()));
    for (std::size_t index = 0; index < flowers.size(); ++index) {
        const auto &memory = bouquetSlotMemory[flowers[index]];
        writeU32(bytes, 8 + index * 4, static_cast<std::uint32_t>(memory.type));
        writeU32(bytes, 0x80 + index * 4,
                 static_cast<std::uint32_t>(memory.frame));
    }
    try {
        setProfileSceneRecordBytes(path, "bouquet", 0x402U, 1U, bytes);
    } catch (const std::exception &error) {
        std::cerr << "Could not save the bouquet: " << error.what() << '\n';
    }
}

void BouquetScene::restoreBouquet() {
    // 0x0045e530: an absent record reads as zeroes (vase 0, no flowers).
    std::vector<std::uint8_t> bytes(kSaveSize, 0);
    if (profiles[profileIndex] != "No profile") {
        const fs::path path =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(path)) {
            if (auto saved = profileSceneRecordBytes(path, "bouquet", 0x402U,
                                                     1U, kSaveSize)) {
                bytes = std::move(*saved);
            }
        }
    }
    const int count = std::clamp(readI32(bytes, 4), 0, 30);
    for (int index = 0; index < count; ++index) {
        const int type = readI32(bytes, 8 + static_cast<std::size_t>(index) * 4);
        const int frame =
            readI32(bytes, 0x80 + static_cast<std::size_t>(index) * 4);
        if (type < 0 || type >= static_cast<int>(kBouquetFlowerTypes) ||
            frame < 0 || frame >= static_cast<int>(kBouquetFrames)) {
            continue;
        }
        if (const auto slot = allocateBouquetFlower(type, frame, 0, 0)) {
            raiseBouquetFlower(*slot);
        }
    }
    const int vase = readI32(bytes, 0);
    setBouquetVase(vase >= 0 && vase < 18 ? vase : 0);
}

SurfacePtr BouquetScene::composeBouquetPicture() const {
    // 0x0045ed10: the union of the vase and flower rectangles on white;
    // flowers in layer order, then the vase over them.
    if (!bouquetGame || !bouquetGame->vase.image) return {};
    const auto &game = *bouquetGame;
    const auto flowers = sortedBouquetFlowers();
    const auto rectOf = [](const BouquetSprite &sprite) {
        return SDL_Rect{sprite.x, sprite.y, sprite.image->surface->w,
                        sprite.image->surface->h};
    };
    SDL_Rect bounds = rectOf(game.vase);
    for (const auto slot : flowers) {
        const SDL_Rect rect = rectOf(game.slots[slot].sprite);
        SDL_UnionRect(&bounds, &rect, &bounds);
    }
    SurfacePtr picture(SDL_CreateRGBSurfaceWithFormat(
        0, bounds.w, bounds.h, 32, SDL_PIXELFORMAT_ARGB8888));
    if (!picture) return {};
    SDL_FillRect(picture.get(), nullptr,
                 SDL_MapRGB(picture->format, 255, 255, 255));
    const auto blit = [&](const BouquetSprite &sprite) {
        SDL_Rect destination{sprite.x - bounds.x, sprite.y - bounds.y, 0, 0};
        SDL_BlitSurface(sprite.image->surface.get(), nullptr, picture.get(),
                        &destination);
    };
    for (const auto slot : flowers) blit(game.slots[slot].sprite);
    blit(game.vase);
    return picture;
}

void BouquetScene::updateBouquet(double elapsed) {
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    if (!game.entered) enterBouquet();
    // 0x0045ee50 (timer): sailboat and raceboat take turns; the boat index
    // at 0x006a0080 survives leaving the scene.
    if (elapsed >= game.nextBoatAt) {
        game.nextBoatAt += 180.0;
        auto &boat = game.boats[static_cast<std::size_t>(bouquetBoatIndex)];
        const auto &spec = kBoats[static_cast<std::size_t>(bouquetBoatIndex)];
        if (!boat.playing) {
            referenceTrace.emit("animation_add", {
                {"asset", spec.name}, {"x", spec.x}, {"y", spec.y},
                {"layer", spec.layer}, {"hotspot", 0},
                {"conflict_mask", 0}});
            boat.playing = true;
            boat.startedAt = elapsed;
            if (!boat.clip.media.audioSamples.empty()) {
                audioMixer.addEffect(boat.clip.media,
                                     audioVolumes[0] * sceneAudioGains[0]);
                SDL_PauseAudioDevice(audioDevice,
                                     musicEnabled && !paused ? 0 : 1);
            }
        }
        bouquetBoatIndex = (bouquetBoatIndex + 1) % 2;
    }
    // 0x0045ee30 (event 0x110): a finished boat is removed.
    for (std::size_t index = 0; index < game.boats.size(); ++index) {
        auto &boat = game.boats[index];
        if (boat.playing &&
            elapsed - boat.startedAt >=
                originalSmackerCompletionDuration(boat.clip.media)) {
            boat.playing = false;
            referenceTrace.emit("animation_stop", {
                {"asset", kBoats[index].name}, {"reason", "finished"}});
            referenceTrace.emit("animation_remove", {
                {"asset", kBoats[index].name}, {"reason", "removed"}});
        }
    }
}

void BouquetScene::renderBouquet(double elapsed) {
    if (!bouquetGame) return;
    auto &game = *bouquetGame;
    // Layer -10: the boats.
    for (std::size_t index = 0; index < game.boats.size(); ++index) {
        auto &boat = game.boats[index];
        if (!boat.playing || !boat.clip.texture) continue;
        const std::size_t frameIndex =
            frameIndexAt(boat.clip.media, elapsed - boat.startedAt, false);
        const auto &frame = boat.clip.media.videoFrames[frameIndex];
        uploadVideoFrame(boat.clip.texture.get(), frame);
        SDL_Rect destination{kBoats[index].x, kBoats[index].y, frame.width,
                             frame.height};
        SDL_RenderCopy(renderer, boat.clip.texture.get(), nullptr,
                       &destination);
    }
    // Layer 0: the localized title (0x00407a30, rect 0x0049fb20, Amerigo
    // BT_14_) and the controls.
    drawCenteredText(renderer, calendarFont,
                     language.get("bouquet", "SCENENAME",
                                  "Ludvigs blomsterbinderi"),
                     SDL_Rect{295, 3, 208, 33});
    for (std::size_t index = 0; index < game.controls.size(); ++index) {
        const auto &control = game.controls[index];
        const auto &image = game.pressedControl == index ? control.pressed
                                                          : control.normal;
        SDL_Rect destination{control.x, control.y, image.surface->w,
                             image.surface->h};
        SDL_RenderCopy(renderer, image.texture.get(), nullptr, &destination);
    }
    // The vase and the flowers in layer order.
    std::vector<const BouquetSprite *> sprites;
    if (game.vase.added) sprites.push_back(&game.vase);
    for (const auto &slot : game.slots) {
        if (slot.inUse && slot.sprite.added) sprites.push_back(&slot.sprite);
    }
    std::stable_sort(sprites.begin(), sprites.end(),
                     [](const BouquetSprite *left, const BouquetSprite *right) {
                         return left->layer != right->layer
                                    ? left->layer < right->layer
                                    : left->order < right->order;
                     });
    for (const auto *sprite : sprites) {
        if (!sprite->image || !sprite->image->texture) continue;
        SDL_Rect destination{sprite->x, sprite->y, sprite->image->surface->w,
                             sprite->image->surface->h};
        SDL_RenderCopy(renderer, sprite->image->texture.get(), nullptr,
                       &destination);
    }
}

bool BouquetScene::executeBouquetDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "bouquet:";
    if (activeScene != "bouquet" || !bouquetGame) return false;
    const std::string control(command.substr(prefix.size()));
    int values[5]{};
    if (control.rfind("drag=", 0) == 0) {
        // drag=SX,SY,X,Y[,MS]: the held moves reference_runner.py sends, one
        // per 25 ms of the gesture.
        values[4] = 600;
        const int read = std::sscanf(control.c_str() + 5, "%d,%d,%d,%d,%d",
                                     &values[0], &values[1], &values[2],
                                     &values[3], &values[4]);
        if (read < 4) throw std::runtime_error("bouquet:drag=SX,SY,X,Y[,MS]");
        beginBouquetInteraction(values[0], values[1]);
        const int steps = std::max(2, values[4] / 25);
        for (int step = 1; step <= steps; ++step) {
            moveBouquetInteraction(
                values[0] + (values[2] - values[0]) * step / steps,
                values[1] + (values[3] - values[1]) * step / steps);
        }
        endBouquetInteraction(values[2], values[3]);
    } else if (control.rfind("click=", 0) == 0) {
        if (std::sscanf(control.c_str() + 6, "%d,%d", &values[0],
                        &values[1]) != 2) {
            throw std::runtime_error("bouquet:click=X,Y");
        }
        if (beginBouquetInteraction(values[0], values[1])) {
            endBouquetInteraction(values[0], values[1]);
        }
    } else if (control.rfind("control=", 0) == 0) {
        activateBouquetControl(
            static_cast<std::uint32_t>(std::stoul(control.substr(8))));
    } else if (control == "boat") {
        bouquetGame->nextBoatAt = sceneElapsedNow();
    } else if (control == "status") {
        std::cout << "Bouquet status: vase " << bouquetGame->vaseIndex + 1
                  << ", raise counter " << bouquetGame->raiseCounter;
        for (std::size_t slot = 0; slot < kBouquetSlots; ++slot) {
            if (!bouquetGame->slots[slot].inUse) continue;
            const auto &memory = bouquetSlotMemory[slot];
            std::cout << "; slot " << slot << ' '
                      << kFlowerNames[static_cast<std::size_t>(memory.type)]
                      << " frame " << memory.frame << " layer "
                      << bouquetGame->slots[slot].sprite.layer;
        }
        std::cout << '\n';
    } else {
        throw std::runtime_error("Unknown developer Bouquet control: " +
                                 control);
    }
    return true;
}

} // namespace fgp
