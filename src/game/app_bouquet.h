#pragma once

#include "game/app_core.h"

namespace fgp {

// The "bouquet" activity (Ludvigs blomsterbinderi): flowers dragged from the
// bed into one of 18 vases. Implemented in app_bouquet.cpp; the dispatcher is
// FGP.exe 0x0045e0f0 and the module ends at 0x0045ee8f.
class BouquetScene : protected virtual AppCore {
protected:
    static constexpr std::size_t kBouquetFlowerTypes = 22;
    static constexpr std::size_t kBouquetFrames = 5;
    static constexpr std::size_t kBouquetSlots = 30;
    static constexpr std::size_t kBouquetVases = 18;

    struct BouquetImage {
        SurfacePtr surface;
        TexturePtr texture;
    };
    // One SCENE_AddAnim'd bitmap descriptor.
    struct BouquetSprite {
        bool added{};
        std::string name;
        int x{};
        int y{};
        int layer{};
        std::uint32_t hotspot{};
        std::uint64_t order{};
        const BouquetImage *image{};
    };
    // One 0xc0-byte flower slot of the array at 0x006a0130.
    struct BouquetSlot {
        bool inUse{};
        BouquetSprite sprite;
    };
    // A type-1 control created by 0x00406ed0 (print, prev, next, clear).
    struct BouquetControl {
        std::uint32_t id{};
        std::string stem;
        int x{};
        int y{};
        BouquetImage normal;
        BouquetImage pressed;
    };
    struct BouquetBoat {
        HopscotchMedia clip;
        bool playing{};
        double startedAt{};
    };
    struct BouquetGame {
        std::array<BouquetImage, kBouquetVases> vases;
        std::array<std::array<BouquetImage, kBouquetFrames>,
                   kBouquetFlowerTypes> flowers;
        std::array<BouquetImage, kBouquetFlowerTypes> small;
        std::vector<BouquetControl> controls;
        std::optional<std::size_t> pressedControl;
        // The vase descriptor 0x00503400.
        BouquetSprite vase;
        std::array<BouquetSlot, kBouquetSlots> slots;
        // 0x006a1968: the flower being dragged.
        std::optional<std::size_t> dragged;
        std::array<BouquetBoat, 2> boats;
        double nextBoatAt{};
        std::uint64_t nextOrder{};
        // 0x006a197c (vase) and 0x0069f048 (raise counter).
        int vaseIndex{};
        int raiseCounter{};
        bool entered{};
    };

    void loadBouquetGame(Package &package);
    bool beginBouquetInteraction(int x, int y);
    void moveBouquetInteraction(int x, int y);
    void endBouquetInteraction(int x, int y);
    void updateBouquet(double elapsed);
    void renderBouquet(double elapsed);
    // 0x0045e800 (event 0x104): writes the arrangement to the profile.
    void saveBouquet();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        bouquetHotspotAt(int x, int y) const;
    bool bouquetDragging() const;
    // Developer control API for "bouquet:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBouquetDeveloperCommand(std::string_view command);

    std::optional<BouquetGame> bouquetGame;

private:
    // The slot fields at +0x4..+0x14 and the boat index 0x006a0080 live in
    // FGP.exe's .bss, so they keep their values between visits; only the
    // in-use flag and descriptor are reset on entry (0x0045e3b0).
    struct BouquetSlotMemory {
        int type{};
        int frame{};
        int offsetX{};
        int offsetY{};
        int layer{};
    };
    std::array<BouquetSlotMemory, kBouquetSlots> bouquetSlotMemory{};
    int bouquetBoatIndex{};

    void enterBouquet();
    void traceBouquetControl(const std::string &image, std::uint32_t id,
                             bool add);

    void addBouquetSprite(BouquetSprite &sprite, int x, int y, int layer);
    void removeBouquetSprite(BouquetSprite &sprite);
    void setBouquetVase(int vase);
    SDL_Point bouquetFlowerPosition(std::size_t slot) const;
    int bouquetFlowerLayer(std::size_t slot) const;
    void raiseBouquetFlower(std::size_t slot);
    std::optional<std::size_t> allocateBouquetFlower(int type, int frame,
                                                     int x, int y);
    void setBouquetFlowerFrame(std::size_t slot, int frame, int x, int y);
    void centreBouquetSprite(BouquetSprite &sprite, int x, int y);
    void freeBouquetFlower(std::size_t slot);
    void removeBouquetDuplicates(std::size_t slot);
    void pressBouquet(std::uint32_t hotspot, int x, int y);
    void activateBouquetControl(std::uint32_t id);
    void clearBouquet();
    void restoreBouquet();
    std::vector<std::size_t> sortedBouquetFlowers() const;
    SurfacePtr composeBouquetPicture() const;
    std::uint32_t bouquetSpriteHotspotAt(int x, int y, bool &hit) const;
    std::optional<std::size_t> bouquetControlAt(int x, int y) const;
};

} // namespace fgp
