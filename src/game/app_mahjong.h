#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/app_core.h"
#include "game/mahjong_board.h"

namespace fgp {

// The "mahjong" activity (Mah Jongg): its scene state and event handling.
// Implemented in app_mahjong.cpp; the board and its software 3D renderer
// are mahjong_board.h and mahjong_engine.h. The scene callback is FGP.exe
// 0x00450310: init 0x00450400, exit 0x004506d0, mouse down 0x00450730,
// pointer move 0x00450750, tick 0x00450b00 and controls 0x00450870.
class MahjongScene : protected virtual AppCore {
protected:
    // One 0x00406ed0 type-1 button: id, top-left, normal and pressed art,
    // and its label (drawn in Amerigo BT_10_c).
    struct MahjongButton {
        int id{};
        int x{};
        int y{};
        int width{};
        int height{};
        TexturePtr normal;
        TexturePtr pressed;
        std::string normalName;
        std::string pressedName;
        std::string label;
        // The label is its own text control, sized to the text and centred
        // on the button.
        int labelX{};
        int labelY{};
    };
    struct MahjongSound {
        std::string name;
        DecodedMedia media;
    };
    // A playing clone of a sound descriptor (0x00412c20); it is removed
    // when its clip ends.
    struct MahjongClone {
        std::string name;
        double endsAt{};
    };
    struct MahjongGame {
        std::vector<MahjongButton> buttons;
        std::optional<std::size_t> pressedButton;
        BitmapFont titleFont;      // Amerigo BT_10_b (0x006915a0)
        BitmapFont clockFont;      // LCD_14_WHITE (0x00690568)
        BitmapFont buttonFont;     // Amerigo BT_10_c (0x0068f530)
        std::vector<MahjongSound> sounds;
        std::vector<MahjongClone> clones;
        // Decoded RGB565 copies of the tile bitmaps; each tile gets its own
        // copy because 0x00451400 shades it in place.
        std::unordered_map<std::string, std::vector<std::uint16_t>> bitmaps;
        std::vector<std::uint16_t> background;   // backdrop (25,100,750,400)
        // 0x006925e0: the title/pairs text and 0x00692688: the clock.
        std::string titleText;
        std::string clockText;
        bool entryAnnounced{};
        bool noselectAdded{};                    // noselect1 is still added
        std::optional<mahjong::Board> board;     // 0x0069159c
        bool boardShown{};                       // 0x0068f52c
        bool gameOver{};                         // 0x006925d8
        bool solved{};                           // 0x0068f504
        // 0x0068f508: the row 0x0041ee50 returned for the solve.
        std::optional<ScoreRow> lastScoreRow;
        bool dialogPending{};                    // 0x00691598
        float dialogDelay{};                     // 0x0068f500
        std::optional<int> selected;             // 0x0068f450
        // The stopwatch at 0x006927e8 (0x0047c5a0 resets, 0x0047c610 stops).
        double clockStartedAt{};
        double clockLast{};
        double clockDelta{};
        double clockFrozenAt{};
        bool clockStopped{};
        bool clockValid{};
        std::vector<std::uint16_t> frame;
        TexturePtr frameTexture;
    };

    MahjongGame loadMahjongGame(Package &package);
    // Mouse down 0x105 (0x00450730 records a click for the next tick) and
    // the type-1 button press (0x00406c50). Always consumes the event.
    bool beginMahjongInteraction(int x, int y);
    // 0x108 (0x00450750) moves the camera; held moves (0x109) do nothing.
    void moveMahjongPointer(int x, int y, bool buttonHeld);
    // The type-1 button release (0x00406d40) posts control event 0x116.
    void endMahjongInteraction(int x, int y);
    // The 0x112 tick, 0x00450b00.
    void updateMahjong(double elapsed);
    void renderMahjong(double elapsed);
    // Developer control API for "mahjong:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeMahjongDeveloperCommand(std::string_view command);

    std::optional<MahjongGame> mahjongGame;

private:
    void announceMahjongEntry();
    // 0x00406740: a button's frame change removes one art and adds the other.
    void swapMahjongButtonArt(const MahjongButton &button, bool pressed);
    // 0x00450870: the control handler for ids 1-6.
    void mahjongControl(int id);
    // 0x00450a50: New.
    void dealMahjong();
    // 0x00450950: refresh the pairs text, or end the game.
    void updateMahjongPairs();
    void playMahjongSound(std::string_view name, bool clone);
    std::shared_ptr<mahjong::Texture> mahjongTexture(std::string_view name);
    double mahjongClockMs() const;

    // Globals the original never resets on entry: the camera angles
    // (0x0068f44c pitch, 0x0068f454 yaw), the pending click (0x0068f448
    // at 0x0068f428/0x0068f42c) and the C runtime rand() state.
    float mahjongPitch{};
    float mahjongYaw{};
    bool mahjongClickPending{};
    int mahjongClickX{};
    int mahjongClickY{};
    mahjong::MsvcRandom mahjongRandom{1};
    bool mahjongRandomSeeded{};
    std::optional<std::uint32_t> mahjongPinnedSeed;
    // 0x00692730: the id buffer, allocated and zeroed once per scene entry.
    std::vector<std::uint8_t> mahjongIds;
};

} // namespace fgp
