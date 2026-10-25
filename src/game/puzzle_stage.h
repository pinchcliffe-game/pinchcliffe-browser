#pragma once

// Engine pieces the jigsaw and sliding puzzles share, ported as one mixin:
//
// * a scene descriptor list (SCENE_AddAnim 0x00412840, SCENE_RemoveAnim
//   0x00412aa0, the layer sort 0x004129b0 and the top-down hit test
//   0x004136c0) for descriptors the puzzles own;
// * the activity control panel (0x00406ed0 add, 0x00406720 state,
//   0x00406910 caption, press 0x00406c50 / release 0x00406d40);
// * the `listdlg` picture chooser (0x00420750 -> 0x00420430), filled the way
//   0x0046b800 and 0x0046e880 fill it;
// * SCENE_PlayAnimClone (0x00412c20) sound clones.
//
// Implemented in puzzle_stage.cpp.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/app_core.h"
#include "game/picture_library.h"
#include "game/puzzle_image.h"

namespace fgp {

class PuzzleStageScene : protected virtual AppCore {
protected:
    // One 0xa8-byte descriptor. `defaultX/Y` are +0x54/+0x58 (used for an
    // add at (-1,-1)), `x/y` +0x6c/+0x70, `layer` +0x74, `hotspot` +0x50,
    // `keyed` +0x48. Owned by the scene; the stage only lists it.
    struct PuzzleDescriptor {
        std::string asset;
        SDL_Texture *texture{};
        TexturePtr ownedTexture;
        // Keyed hit testing (0x004136c0 mode 3) reads the 16-bit pixels.
        const Rgb565Image *pixels{};
        int width{};
        int height{};
        int defaultX{};
        int defaultY{};
        int x{};
        int y{};
        int layer{};
        std::uint32_t hotspot{};
        bool keyed{};
        bool added{};
        std::uint64_t order{};
        // A text box (0x004078c0): drawn instead of a texture.
        std::string text;
        const BitmapFont *font{};
        // Text drawn into the bitmap's top-right corner (flags 0x200).
        std::string label;
        const BitmapFont *labelFont{};
    };
    struct PuzzleControl {
        int id{};
        int x{};
        int y{};
        // +0x16c + state * 0x40: the bitmap per state, empty for none.
        std::array<std::string, 4> names;
        std::array<TexturePtr, 4> textures;
        std::array<int, 4> widths{};
        std::array<int, 4> heights{};
        int state{};
        bool hover{};
        // +0x10/+0x14: the measured size 0x004069b0 caches.
        bool sizeKnown{};
        int layer{};
        PuzzleDescriptor bitmap;
        PuzzleDescriptor caption;
        const BitmapFont *captionFont{};
    };
    // One list row: a caller-built surface and the id OK returns (the
    // library index for the picture choosers).
    struct PuzzleChooserRow {
        int picture{};
        TexturePtr texture;
    };
    // listdlg: 0x00420430 with list control 0x65, scroll bar 0x3ea and the
    // OK (1000) / Cancel (0x3e9) controls.
    struct PuzzleChooser {
        bool open{};
        int left{};
        int top{};
        std::vector<PuzzleChooserRow> rows;
        int selected{};
        int firstVisible{};
        int visibleRows{};
        int rowWidth{0xee};
        int rowHeight{0x28};
        PuzzleDescriptor backdrop;
        // 0x00407110: the modal mesh, clipped to the dialog, at layer 99.
        PuzzleDescriptor mesh;
        PuzzleDescriptor title;
        PuzzleDescriptor thumb;
        // 0x00407c00: one descriptor per visible row (list +0xdc), and the
        // selection bar (list +0x30) one layer below them.
        std::array<PuzzleDescriptor, 32> rowSlots;
        PuzzleDescriptor selection;
        std::vector<std::unique_ptr<PuzzleControl>> controls;
        int pressedControl{-1};
        bool draggingThumb{};
        int upId{};
        int downId{};
        // 1 = OK, 2 = Cancel (0x00420960).
        int result{};
    };
    struct PuzzleCloneSound {
        std::string asset;
        double endsAt{};
        LiveAudioMixer::EffectHandle handle{};
    };
    struct PuzzleStage {
        std::vector<PuzzleDescriptor *> list;
        std::uint64_t nextOrder{};
        std::vector<std::unique_ptr<PuzzleControl>> controls;
        PuzzleChooser chooser;
        std::vector<PuzzleCloneSound> clones;
        std::map<std::string, DecodedMedia, std::less<>> sounds;
        int pressedControl{-1};
        // The original's chooser is modal inside event 0x103, so the scene is
        // only ready once it closes (SceneReady follows the board's adds).
        bool holdingSceneReady{};
        // DAT_004a6354: each scroll bar's arrows take the next two control
        // ids (0x004091f0). Only 0x00408c70 resets it to 2000, from other
        // scenes' initialisers; captures entered from Contents show 2000 and
        // 2001 for the first chooser and 2002/2003 for the next.
        int nextScrollControl{2000};
        BitmapFont amerigo10;
        BitmapFont amerigo14;
        BitmapFont amerigo18;
        BitmapFont lcd14;
        TexturePtr checkTexture;
        TexturePtr crossTexture;
        SurfacePtr checkSurface;
        SurfacePtr crossSurface;
    };

    void loadPuzzleStage(PuzzleStage &stage, Package &package);
    void puzzleAdd(PuzzleStage &stage, PuzzleDescriptor &descriptor, int x,
                   int y, int layer);
    void puzzleRemove(PuzzleStage &stage, PuzzleDescriptor &descriptor);
    void puzzleSetLayer(PuzzleStage &stage, PuzzleDescriptor &descriptor,
                        int layer);
    std::uint32_t puzzleHotspotAt(const PuzzleStage &stage, int x, int y) const;
    void drawPuzzleStage(PuzzleStage &stage);

    // 0x00406ed0 with the caption font the scene selected (0x004068a0) and
    // the panel layer (0x00406ec0).
    PuzzleControl &puzzleAddControl(PuzzleStage &stage, Package &package,
                                    int id, int x, int y,
                                    std::array<std::string, 4> names,
                                    const std::string &caption,
                                    const BitmapFont *font, int layer);
    void puzzleSetControlState(PuzzleStage &stage, int id, int state);
    void puzzleSetControlCaption(PuzzleStage &stage, int id,
                                 const std::string &caption);
    // Mouse plumbing for the panel: a press over a state-0 control presses
    // it; the release reports its id (event 0x116) while still over it.
    bool puzzlePressControl(PuzzleStage &stage, int x, int y);
    std::optional<int> puzzleReleaseControl(PuzzleStage &stage, int x, int y);

    // The chooser rows, as 0x0046b800 / 0x0046e880 build them.
    void openPuzzleChooser(PuzzleStage &stage, std::string_view scene,
                           std::string_view captionKey, int firstPicture,
                           int endPicture, int (*gridSize)(int));
    // The generic listdlg (0x00420750): caller-built rows, an optional Cancel
    // (flags & 2); closePuzzleChooser returns the chosen row's id or -1.
    void openPuzzleListDialog(PuzzleStage &stage, const std::string &title,
                              std::vector<PuzzleChooserRow> rows,
                              bool withCancel);
    bool puzzleChooserPress(PuzzleStage &stage, int x, int y);
    void puzzleChooserMove(PuzzleStage &stage, int x, int y);
    void puzzleChooserRelease(PuzzleStage &stage, int x, int y);
    void puzzleChooserKey(PuzzleStage &stage, SDL_Keycode key);
    // Holds the pending scene_ready until the entry chooser closes.
    void holdPuzzleSceneReady(PuzzleStage &stage);
    void releasePuzzleSceneReady(PuzzleStage &stage);
    // Closes the chooser and returns the chosen library index, or -1.
    int closePuzzleChooser(PuzzleStage &stage);

    void puzzlePlayClone(PuzzleStage &stage, const std::string &asset);
    void tickPuzzleClones(PuzzleStage &stage, double now);

    // Developer aid: queue pointer input ("click:x,y", "down:x,y", "up:x,y",
    // "move:x,y", "rclick:x,y") as SDL events. Returns false if unparsed.
    bool puzzlePointerCommand(std::string_view control);
    // Developer aid: write the last presented frame to a BMP file.
    void puzzleSnapshot(const std::string &path);

    fs::path puzzleProfilePath() const;
    std::uint32_t puzzlePictureRecord(std::string_view scene, int picture) const;
    void setPuzzlePictureRecord(std::string_view scene, int picture,
                                std::uint32_t value);

private:
    void refreshControl(PuzzleStage &stage, PuzzleControl &control);
    void placeCaption(PuzzleStage &stage, PuzzleControl &control);
    void scrollChooser(PuzzleStage &stage, int firstVisible);
    void placeChooserSelection(PuzzleStage &stage);
    void measureControls(PuzzleStage &stage);
};

} // namespace fgp
