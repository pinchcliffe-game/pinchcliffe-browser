#pragma once

#include "game/app_core.h"

namespace fgp {

// The main menu's Scoreboard ("mainscore", Poengtavle): the games the active
// profile has scored in, with their medals, and each game's score tables
// across every profile, plus the car parts the profile has collected.
// Implemented in app_mainscore.cpp, a port of FGP.exe 0x00416840-0x004175e0.
class MainscoreScene : protected virtual AppCore {
protected:
    // One row of the 0x174-byte score list (0x0041ebd0): a 0x24-byte entry
    // of a 24-character name, the Unix timestamp and the score.
    struct MainscoreRow {
        std::string player;
        std::uint32_t timestamp{};
        std::uint32_t score{};
    };
    // One game in the left-hand list, from subgame.ini (0x00416960).
    struct MainscoreGame {
        std::string scene;
        std::uint32_t version{};
        std::string title;
        std::uint32_t medal{};
        TexturePtr thumbnail;
        int thumbnailWidth{};
        int thumbnailHeight{};
        std::array<std::vector<MainscoreRow>, 3> rows; // Profile, Global, Union
    };
    struct MainscoreImage {
        SurfacePtr surface;
        TexturePtr texture;
    };
    struct MainscoreBoard {
        std::vector<MainscoreGame> games;
        int selected{-1};      // 0x0056d650
        int firstVisible{};    // list +0x20
        int visibleCount{};    // list +0x24
        int itemHeight{};      // list +0x2c
        int tab{3};            // 0x005716f8: 3 Profile, 4 Global, 5 Union
        bool carParts{};       // the Bildeler tab (control 6) is showing
        std::uint32_t carPartCount{}; // 0x005716fc
        std::string playerName;
        std::unordered_map<std::string, MainscoreImage> images;
        std::vector<MainscoreImage> parts;
        BitmapFont tooltipFont;
        int pressedArrow{};    // -1 up, 1 down, 0 none
        bool draggingThumb{};
        int hoveredPart{-1};
    };

    void loadMainscoreBoard();
    bool beginMainscoreInteraction(int x, int y);
    void moveMainscorePointer(int x, int y);
    void endMainscoreInteraction();
    bool handleMainscoreKey(SDL_Keycode key);
    void renderMainscore();
    // Developer control API for "mainscore:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeMainscoreDeveloperCommand(std::string_view command);

    std::optional<MainscoreBoard> mainscoreBoard;

private:
    void selectMainscoreGame(int index);
    void scrollMainscoreList(int first);
    int mainscoreScrollRange() const;
    std::span<const std::uint8_t> mainscoreAsset(const std::string &asset);
    MainscoreImage &mainscoreImage(const std::string &name);
    void drawMainscoreImage(const std::string &name, int x, int y);
    void drawMainscoreTooltip(const std::string &text);
};

} // namespace fgp
