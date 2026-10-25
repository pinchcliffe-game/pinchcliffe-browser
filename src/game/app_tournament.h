#pragma once

#include "game/app_core.h"

namespace fgp {

// The main menu's Tournament: the setup screen ("tournament") and the
// results shown after every round ("tourneyscore"). The running state lives
// in AppCore::TournamentState, because every minigame and the shared
// score path reach it. Implemented in app_tournament.cpp, a port of FGP.exe
// 0x00419410-0x0041b3d0.
class TournamentScene : protected virtual AppCore {
protected:
    struct TournamentImage {
        SurfacePtr surface;
        TexturePtr texture;
    };
    // The shared list dialog 0x00420750 with text rows and Cancel (flags
    // 0x102), as the setup screen opens it for a new player or game.
    struct TournamentPicker {
        bool players{};
        std::string title;
        std::vector<std::string> items;
        int selected{};
        int firstVisible{};
        int pressed{};   // the button held: 1000 OK, 0x3e9 Cancel, 1/2 arrows
    };
    struct TournamentSetup {
        // The games this profile may play (0x00419a20), from subgame.ini.
        std::vector<TournamentGame> offered;
        int playerSelected{-1};
        int gameSelected{-1};
        int pressedControl{};
        std::optional<TournamentPicker> picker;
        std::array<TournamentImage, 3> difficulties; // easy, medium, hard
    };
    struct TournamentResults {
        struct Item {
            TexturePtr thumbnail;
            int width{};
            int height{};
        };
        std::vector<Item> items;
        // Per round: the game's rows with their placement points
        // (0x0041aaf0) and the running totals (0x0041ab90).
        std::vector<std::vector<TournamentRow>> gameRows;
        std::vector<std::vector<TournamentRow>> totalRows;
        int selected{};
        int firstVisible{};
        int visibleCount{};
        int itemHeight{};
        int tab{3};
        bool pressedNext{};
        int pressedArrow{};
        bool draggingThumb{};
    };
    struct TournamentScreen {
        std::unordered_map<std::string, TournamentImage> images;
        std::optional<TournamentSetup> setup;
        std::optional<TournamentResults> results;
    };

    void loadTournamentScreen();
    bool beginTournamentInteraction(int x, int y);
    void moveTournamentPointer(int x, int y);
    void endTournamentInteraction(int x, int y);
    bool handleTournamentKey(SDL_Keycode key);
    void renderTournament();
    // The open list dialog, drawn over the corner buttons.
    void renderTournamentPicker();
    bool executeTournamentDeveloperCommand(std::string_view command);

    std::optional<TournamentScreen> tournamentScreen;

private:
    std::span<const std::uint8_t> tournamentAsset(const std::string &asset);
    TournamentImage &tournamentImage(const std::string &name);
    void drawTournamentImage(const std::string &name, int x, int y);
    void loadTournamentSetup();
    void loadTournamentResults();
    void renderTournamentSetup();
    void renderTournamentResults();
    bool beginTournamentSetupInteraction(int x, int y);
    bool beginTournamentPickerInteraction(int x, int y);
    bool beginTournamentResultsInteraction(int x, int y);
    void tournamentSetupControl(int control);
    void openTournamentPicker(bool players);
    void closeTournamentPicker(bool accept);
    void startTournament();
    void scrollTournamentResults(int first);
    void selectTournamentResult(int index);
    int tournamentResultsScrollRange() const;
};

} // namespace fgp
