#pragma once

#include "game/app_core.h"
#include "game/puzzle_stage.h"

namespace fgp {

// The "colorfill" activity (Reodors tegnebord): a 490x490 painting canvas
// with pen, paint bucket, eraser, colour picker and light-source tools,
// a brush column, a colour palette, single-level undo, Clear, a picture
// chooser and the hand-off to the Activity page. Implemented in
// app_colorfill.cpp; the dispatcher is FGP.exe 0x00461600.
class ColorfillScene : protected virtual PuzzleStageScene {
public:
    // An 8-bit image as the raw loader 0x004149f0 returns it: palette
    // indices, top row first ({width, height, bytes per pixel, pixels}).
    struct ColorfillIndexed {
        int width{};
        int height{};
        std::vector<std::uint8_t> pixels;
    };
    // One ini section with its keys in file order (0x0040e430 enumerates
    // sections by index, 0x0040e340/0x0040e3a0 read keys).
    struct ColorfillIniSection {
        std::string name;
        std::vector<std::pair<std::string, std::string>> values;
    };

protected:
    // A control of the activity panel (0x00406ed0): style 1 is a push
    // button that reports 0x116 on release, style 2 a radio button of a
    // group that reports 0x116 on press (0x00406b50/0x00406cf0).
    struct ColorfillControl {
        int id{};
        int x{};
        int y{};
        int style{1};
        int group{};
        int state{};
        bool hover{};
        std::array<std::string, 2> images;
        std::array<TexturePtr, 2> textures;
        std::array<SDL_Point, 2> sizes{};
        std::string caption;
        std::string shownImage;
    };
    struct ColorfillGame {
        std::vector<ColorfillIniSection> canvasSections;
        std::vector<ColorfillIniSection> brushSections;
        // The picture: paint surface 0x006a79ec (RGB565), its undo copy
        // 0x006a79e4, the composited canvas surface (descriptor 0x00505f00
        // +0x8c), outline flags 0x006a79f0, mask and outline images
        // 0x006a79f4/0x006a79f8, normals 0x006a79e8 and light 0x006a79fc.
        std::vector<std::uint16_t> paint;
        std::vector<std::uint16_t> undo;
        std::vector<std::uint16_t> rendered;
        std::vector<std::uint8_t> outlineFlags;
        std::optional<ColorfillIndexed> mask;
        std::optional<ColorfillIndexed> outline;
        std::vector<std::int8_t> normals;
        std::vector<std::uint8_t> light;
        std::string picture;
        int lightX{};
        int lightY{};
        // Brush 0x006a7a04 and its cursor outline 0x006a7a08.
        std::string brush;
        std::optional<ColorfillIndexed> brushImage;
        std::optional<ColorfillIndexed> brushOutline;
        std::optional<ColorfillIndexed> pickerCursor;
        std::optional<ColorfillIndexed> sunCursor;
        std::optional<ColorfillIndexed> bucketCursor;
        // Tool 0x005060fc (1 pen, 2 bucket, 3 eraser, 4 picker, 5 light)
        // and colour 0x005060f8.
        int tool{1};
        std::uint16_t colour{0xffffU};
        int lastX{};
        int lastY{};
        // Runtime position of the cross descriptor 0x00506050.
        SDL_Point cross{563, 310};
        // 0x00463500: palette colour -> backdrop position of that colour.
        std::vector<SDL_Point> paletteMap;
        std::vector<ColorfillControl> controls;
        std::optional<std::size_t> pressedControl;
        // Brush list 0x006a53b0 and its scrollbar 0x1e.
        std::vector<TexturePtr> brushItems;
        int brushSelected{};
        int brushTop{};
        int brushVisible{9};
        int scrollValue{};
        int scrollMaximum{};
        bool scrollDragging{};
        TexturePtr scrollTab;
        SDL_Point scrollTabSize{};
        TexturePtr crossTexture;
        TexturePtr canvasTexture;
        bool canvasDirty{true};
        // Last drawn XOR overlays (0x004639d0), so the canvas texture is only
        // refreshed when they move.
        std::optional<SDL_Point> drawnCursor;
        int drawnCursorTool{};
        std::string drawnCursorBrush;
        std::optional<SDL_Point> drawnSun;
        bool leftHeld{};
        // The hotspot under the pointer as of the last pointer move (the
        // engine's 0x0054bf60); a modal dialog clears it.
        std::uint32_t hoverHotspot{};
        // Tooltip timer 0x00409b50: 1500 ms on the same registered id.
        int tipCandidate{-1};
        double tipCandidateSince{};
        int tipShown{-1};
        // The tip bubble 0x00409d00 builds: tooltip.bmp stretched around
        // the text, tooldot.bmp at the pointer, Small Fonts_07_ text.
        SurfacePtr tipFrame;
        SurfacePtr tipDot;
        BitmapFont tipFont;
        TexturePtr tipTexture;
        SDL_Rect tipBounds{};
        std::string tipText;
        // The picture chooser: the shared listdlg 0x00420750 (puzzle_stage),
        // used only for the dialog's descriptors.
        PuzzleStage chooserStage;
        std::optional<int> pendingConfirmation;
    };

    ColorfillGame loadColorfillGame(Package &package);
    bool beginColorfillInteraction(int x, int y);
    void moveColorfillPointer(int x, int y, bool leftHeld);
    void endColorfillInteraction(int x, int y);
    bool colorfillKey(SDL_Keycode key);
    void updateColorfill(double elapsed);
    void renderColorfill(double elapsed);
    // The list dialog and tooltip, drawn after the modal mesh.
    void renderColorfillOverlay();
    bool colorfillChooserOpen() const;
    // 0x00463cb0: the picture survives the scene in profile records
    // 0x402/0xdead and 0x402/0xbabe.
    void saveColorfillState();
    // Scene teardown: the engine removes every descriptor of the scene.
    void endColorfillScene();
    // 0x004625b0: the cursor is hidden while the pointer is over the canvas.
    bool colorfillHidesCursor() const;
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        colorfillHotspotAt(int x, int y) const;
    // The shared Help (0x62) and Exit (99) corner controls: the scene's
    // mouse-down sees their id on press and 0x00461740 their release.
    void colorfillCornerButton(bool help, bool released, int x, int y);
    // Answers of the shared yes/no box 0x00421310 for Clear and Activity.
    void colorfillConfirmed(bool yes);
    bool executeColorfillDeveloperCommand(std::string_view command);

    std::optional<ColorfillGame> colorfillGame;
    // 0x005060fc is a static initialised to 1 that only the initializer's
    // end resets; until then a new visit still sees the last visit's tool.
    int colorfillLastTool{1};

private:
    std::optional<ColorfillIndexed> loadColorfillIndexed(
        std::string_view name) const;
    std::vector<std::uint16_t> loadColorfillSurface(std::string_view name) const;
    const ColorfillIniSection *colorfillSection(
        const std::vector<ColorfillIniSection> &sections,
        std::string_view name) const;
    bool loadColorfillPicture(std::string_view section, bool resetLight);
    void loadColorfillBrush(std::string_view section);
    void relightColorfill(int x, int y);
    void renderColorfillCanvas(std::optional<SDL_Rect> area);
    void stampColorfillBrush(int x, int y, std::uint16_t colour,
                             SDL_Rect &changed);
    bool fillColorfill(int x, int y, std::uint16_t colour, SDL_Rect &changed);
    void pickColorfillPalette(int x, int y);
    void showColorfillColour();
    void colorfillCanvasDown(int x, int y);
    void colorfillCanvasDrag(int hotspot, int x, int y);
    void colorfillControl(int id);
    void setColorfillControlState(ColorfillControl &control, int state);
    void selectColorfillBrush(int index);
    void setColorfillScroll(int value);
    void openColorfillChooser();
    void showColorfillTip(int id);
    void restoreColorfillState();
    std::uint32_t colorfillHotspotId(int x, int y) const;
};

} // namespace fgp
