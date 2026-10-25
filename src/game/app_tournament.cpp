#include "game/app_tournament.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"

namespace fgp {

namespace {

// Setup screen (0x00419870, 0x00419d80; rectangles 0x0049ca00-0x0049ca4f).
constexpr SDL_Rect kPlayerList{0x2c, 0xc8, 0xef - 0x2c, 0x226 - 0xc8};
constexpr SDL_Rect kGameList{0x232, 0xc8, 0x2f4 - 0x232, 0x226 - 0xc8};
constexpr SDL_Rect kSetupTitle{0xca, 0x19, 0x248 - 0xca, 0x47 - 0x19};
constexpr SDL_Rect kPlayersLabel{0x25, 0x6a, 0xf6 - 0x25, 0x8a - 0x6a};
constexpr SDL_Rect kGamesLabel{0x22b, 0x6a, 0x2f9 - 0x22b, 0x8a - 0x6a};
constexpr SDL_Point kSetupLogo{0xe7, 0x1c}; // descriptor 0x004ad100
struct SetupButton {
    int id;
    int x;
    int y;
    const char *image;
    const char *label; // tournament:<label>, or none
};
constexpr std::array<SetupButton, 11> kSetupButtons{{
    {1, 0x100, 199, "standard_1", "ADD"},
    {2, 0x100, 0xfc, "standard_1", "REMOVE"},
    {3, 0x100, 0x131, "standard_1", "CLEAR"},
    {7, 0x98, 0x9e, "sortV_opp_1", nullptr},
    {8, 0xc0, 0x9e, "sortV_ned_1", nullptr},
    {4, 0x1b5, 199, "standard_1", "ADD"},
    {5, 0x1b5, 0xfc, "standard_1", "REMOVE"},
    {6, 0x1b5, 0x131, "standard_1", "CLEAR"},
    {9, 0x22d, 0x9e, "sortV_opp_1", nullptr},
    {10, 0x255, 0x9e, "sortV_ned_1", nullptr},
    {11, 0x155, 0x1b6, "start_1", "START"},
}};
constexpr int kMaxPlayers = 8;  // the slots 0x00583f30-0x00584070
constexpr int kMaxGames = 12;   // 0x0041a246
// 0x004085a0's default selection colour (no 0x00407d90 call here), a
// COLORREF 0x00bbggrr.
constexpr Uint32 kSelection = 0x3e5167;

// The list dialog (0x00420750/0x00420430), centred 320x483; its rows and
// buttons as the Fence capture measured them (app_fence.cpp).
constexpr SDL_Point kPicker{240, 58};
constexpr SDL_Rect kPickerTitle{240 + 0x11, 58 + 0x0d, 0x11e - 0x11, 0x28 - 0x0d};
constexpr SDL_Rect kPickerList{240 + 0x14, 58 + 0x3d, 0x102 - 0x14, 0x19a - 0x3d};
constexpr int kPickerRowHeight = 22;
constexpr SDL_Point kPickerOk{240 + 0x7e, 58 + 0x1ab};
constexpr SDL_Point kPickerCancel{240 + 0xd7, 58 + 0x1ab};
constexpr SDL_Rect kPickerTrack{240 + 0x115, 58 + 0x56, 0x0f, 0x122};
constexpr SDL_Point kPickerUp{240 + 0x10f, 58 + 0x31};
constexpr SDL_Point kPickerDown{240 + 0x110, 58 + 0x186};
constexpr int kPickerOkId = 1000;
constexpr int kPickerCancelId = 0x3e9;

// Results screen (0x0041a8f0, 0x0041ac80, 0x0041b0d0).
constexpr SDL_Rect kResultList{48, 143, 132 - 48, 518 - 143}; // 0x0049ca50
constexpr SDL_Point kResultsLogo{0xd9, 0x1d}; // descriptor 0x004ad3b8
constexpr SDL_Rect kResultsTitle{0xce, 0x19, 0x24b - 0xce, 0x49 - 0x19};
constexpr SDL_Rect kRoundBox{0x29, 0x66, 0xa0 - 0x29, 0x7f - 0x66};
constexpr SDL_Rect kGameHeader{0xc9, 0x8a, 0x2ef - 0xc9, 0xa5 - 0x8a};
constexpr std::array<SDL_Rect, 3> kColumnHeaders{{
    {0xd0, 0xad, 0x1a5 - 0xd0, 0xc7 - 0xad},
    {0x1b5, 0xad, 0x227 - 0x1b5, 0xc7 - 0xad},
    {0x237, 0xad, 0x2ea - 0x237, 0xc7 - 0xad}}};
constexpr std::array<int, 2> kResultTabX{0xd1, 0x146};
constexpr int kResultTabY = 0x62;
constexpr SDL_Point kNextButton{0x283, 0x223};
constexpr SDL_Point kResultUp{0x95, 0x88};
constexpr SDL_Point kResultDown{0x95, 0x1fd};
constexpr SDL_Rect kResultTrack{0x95, 0xa8, 0xe, 0x150};
// 0x0041aaf0: placement points by row.
constexpr std::array<std::uint32_t, 10> kPlacementPoints{10, 7, 5, 4, 3,
                                                         2, 2, 1, 1, 0};

enum class Align { Left, Centre, Right };

void drawAligned(SDL_Renderer *renderer, const BitmapFont &font,
                 std::string_view text, const SDL_Rect &bounds, Align align) {
    const int width = textWidth(font, text);
    int x = bounds.x;
    if (align == Align::Centre) {
        x = bounds.x + (bounds.w - width) / 2;
    } else if (align == Align::Right) {
        x = bounds.x + bounds.w - width;
    }
    drawText(renderer, font, text, x, bounds.y + (bounds.h - font.height) / 2);
}

void fillSelection(SDL_Renderer *renderer, const SDL_Rect &bounds) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, kSelection & 0xff,
                           (kSelection >> 8) & 0xff,
                           (kSelection >> 16) & 0xff, 255);
    SDL_RenderFillRect(renderer, &bounds);
}

// 0x0041e9c0: before the first row the new score beats, ten at most.
template <typename Row>
void insertByScore(std::vector<Row> &rows, Row row) {
    const auto position = std::find_if(
        rows.begin(), rows.end(),
        [&](const Row &existing) { return row.score > existing.score; });
    rows.insert(position, std::move(row));
    if (rows.size() > 10U) {
        rows.resize(10U);
    }
}

std::string formatOne(std::string pattern, std::string_view value) {
    const auto at = pattern.find("%s");
    if (at != std::string::npos) {
        pattern.replace(at, 2, value);
    }
    return pattern;
}

} // namespace

// ---------------------------------------------------------------------
// Running the tournament (AppCore)

std::string AppCore::subgameTitle(const std::string &scene,
                                  const std::string &nameKey) const {
    std::string section = scene;
    std::string key = "SCENENAME";
    if (!nameKey.empty() && nameKey.find(':') != std::string::npos) {
        section = nameKey.substr(0, nameKey.find(':'));
        key = nameKey.substr(nameKey.find(':') + 1);
    } else if (scene == "bugzzz") {
        key = "TITLE";
    } else if (scene == "whackamole") {
        key = "GAMENAME";
    }
    return language.get(section, key, scene);
}

void AppCore::showTournamentPlayerReady() {
    // 0x00419660: "Er %s klar?" in a box titled tournament:SCENENAME.
    if (!tournament.active || tournament.players.empty()) {
        return;
    }
    const auto &player =
        tournament.players[static_cast<std::size_t>(tournament.player)];
    showMessageWithTitle(
        language.get("tournament", "SCENENAME", "Turnering"),
        formatOne(language.get("tournament", "PLAYERREADY", "Er %s klar?"),
                  player.name),
        MessageKind::TournamentPlayerReady);
}

void AppCore::recordTournamentScore(int score) {
    // 0x004196c0: the player's score goes into the game's list; 0x00419740
    // then hands over to the next player in the same game or, after the
    // last, to the results.
    auto &state = tournament;
    if (state.scores.size() < state.games.size()) {
        state.scores.resize(state.games.size());
    }
    insertByScore(state.scores[static_cast<std::size_t>(state.game)],
                  TournamentRow{
                      state.players[static_cast<std::size_t>(state.player)]
                          .name.substr(0, 24),
                      0U, static_cast<std::uint32_t>(std::max(0, score))});
    state.player = (state.player + 1) % static_cast<int>(state.players.size());
    if (state.player > 0) {
        const auto &game = state.games[static_cast<std::size_t>(state.game)];
        enterScene(game.scene, game.scene, static_cast<int>(game.version));
    } else {
        enterScene("tourneyscore", "tourneyscore");
    }
}

bool AppCore::askTournamentAbort() {
    // 0x0040cd90: leaving a minigame during a tournament asks
    // tournament:ABORT (0x00419410) instead of leaving; the results screen's
    // exit control asks the same.
    if (!tournament.active) {
        return false;
    }
    const bool inRound = std::any_of(
        tournament.games.begin(), tournament.games.end(),
        [this](const TournamentGame &game) { return game.scene == activeScene; });
    if (!inRound && activeScene != "tourneyscore") {
        return false;
    }
    showMessageWithTitle(
        language.get("interfaceh", "WARNING", "Advarsel"),
        language.get("tournament", "ABORT", ""),
        MessageKind::TournamentAbortConfirmation);
    return true;
}

void AppCore::abortTournament() {
    // 0x00419450.
    tournament.active = false;
    enterScene("tournament", "tournament");
}

void AppCore::advanceTournamentGame() {
    // 0x00419790: the next game, or after the last the setup screen.
    if (!tournament.active || tournament.games.empty()) {
        return;
    }
    tournament.game =
        (tournament.game + 1) % static_cast<int>(tournament.games.size());
    if (tournament.game < 1) {
        abortTournament();
        return;
    }
    const auto &game =
        tournament.games[static_cast<std::size_t>(tournament.game)];
    enterScene(game.scene, game.scene, static_cast<int>(game.version));
}

// ---------------------------------------------------------------------
// Assets

std::span<const std::uint8_t>
TournamentScene::tournamentAsset(const std::string &asset) {
    for (Package *package :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (package) {
            if (const auto *entry = package->findOptional(asset)) {
                return package->data(*entry);
            }
        }
    }
    throw std::runtime_error("Tournament asset not found: " + asset);
}

TournamentScene::TournamentImage &
TournamentScene::tournamentImage(const std::string &name) {
    auto &images = tournamentScreen->images;
    auto found = images.find(name);
    if (found == images.end()) {
        TournamentImage image;
        if (name.rfind("common:", 0) == 0) {
            image.surface = loadGreenKeyedBmp(
                common.data(common.find(name.substr(7))));
        } else {
            image.surface = loadGreenKeyedBmp(
                tournamentAsset("bitmap\\" + name + ".bmp"));
        }
        image.texture = makeTexture(renderer, image.surface.get());
        SDL_SetTextureBlendMode(image.texture.get(), SDL_BLENDMODE_BLEND);
        found = images.emplace(name, std::move(image)).first;
    }
    return found->second;
}

void TournamentScene::drawTournamentImage(const std::string &name, int x,
                                          int y) {
    const auto &image = tournamentImage(name);
    const SDL_Rect destination{x, y, image.surface->w, image.surface->h};
    SDL_RenderCopy(renderer, image.texture.get(), nullptr, &destination);
}

void TournamentScene::loadTournamentScreen() {
    tournamentScreen.emplace();
    if (activeScene == "tournament") {
        loadTournamentSetup();
    } else {
        loadTournamentResults();
    }
}

// ---------------------------------------------------------------------
// Setup screen

void TournamentScene::loadTournamentSetup() {
    auto &setup = tournamentScreen->setup.emplace();
    const bool hasProfile =
        profileIndex < profiles.size() && profiles[profileIndex] != "No profile";
    // 0x00419c1a: lists built under another profile are dropped.
    const std::string profile =
        hasProfile ? profiles[profileIndex]
                   : language.get("profile", "NONAME", "");
    if (profile != tournament.profile) {
        tournament = TournamentState{};
        tournament.profile = profile;
    }
    tournament.active = false;

    // 0x00419a20: subgame.ini's games, offered when the profile has visited
    // the scene (record 0x400) or state%d says so.
    const LanguageTable subgames(tournamentAsset("subgame.ini"));
    const fs::path profilePath =
        hasProfile ? profileDirectory / (profiles[profileIndex] + ".pfl")
                   : fs::path{};
    for (int index = 0; index < 20; ++index) {
        const std::string number = std::to_string(index);
        const std::string scene = subgames.get("subgame", "game" + number);
        if (scene.empty()) {
            break;
        }
        std::uint32_t visits = 0;
        if (hasProfile) {
            try {
                visits = profileSceneRecordU32(profilePath, scene, 0x400U, 0U,
                                               false)
                             .value_or(0U);
            } catch (const std::exception &) {
            }
        }
        const int state = std::atoi(
            subgames.get("subgame", "state" + number, std::to_string(visits))
                .c_str());
        if (state <= 0) {
            continue;
        }
        TournamentGame game;
        game.scene = scene;
        game.version = static_cast<std::uint32_t>(std::max(
            0, std::atoi(subgames.get("subgame", "version" + number, "0")
                             .c_str())));
        game.title = subgameTitle(scene, subgames.get("subgame", "name" + number));
        setup.offered.push_back(std::move(game));
    }

    // 0x00419870: easy, medium and hard, each labelled in Small Fonts 7.
    const BitmapFont smallFont = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Small Fonts_07_.bmp")));
    static constexpr std::array<std::pair<const char *, const char *>, 3>
        difficulties{{{"easy", "EASY"}, {"medium", "MEDIUM"}, {"hard", "HARD"}}};
    for (std::size_t index = 0; index < difficulties.size(); ++index) {
        SurfacePtr base = loadBmp(tournamentAsset(
            std::string("bitmap\\") + difficulties[index].first + ".bmp"));
        TexturePtr target(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, base->w,
                                            base->h));
        TexturePtr baseTexture = makeTexture(renderer, base.get());
        SDL_Texture *previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, target.get());
        SDL_RenderCopy(renderer, baseTexture.get(), nullptr, nullptr);
        drawAligned(renderer, smallFont,
                    language.get("tournament", difficulties[index].second, ""),
                    SDL_Rect{0, 0, base->w, base->h}, Align::Centre);
        SDL_SetRenderTarget(renderer, previous);
        setup.difficulties[index].surface = std::move(base);
        setup.difficulties[index].texture = std::move(target);
    }
    setup.playerSelected = tournament.players.empty() ? -1 : 0;
    setup.gameSelected = tournament.games.empty() ? -1 : 0;
}

void TournamentScene::renderTournamentSetup() {
    auto &setup = *tournamentScreen->setup;
    drawTournamentImage("logo", kSetupLogo.x, kSetupLogo.y);
    drawAligned(renderer, menuTitleFont,
                language.get("tournament", "SCENENAME", "Turnering"),
                kSetupTitle, Align::Centre);
    drawAligned(renderer, calendarFont,
                language.get("tournament", "PLAYERS", "Spillere"),
                kPlayersLabel, Align::Centre);
    drawAligned(renderer, calendarFont,
                language.get("tournament", "GAME", "Spill"), kGamesLabel,
                Align::Centre);
    // The buttons' resting look is the backdrop's; only a held button shows
    // its image (0x00406ed0 with no normal image).
    for (const auto &button : kSetupButtons) {
        const auto &image = tournamentImage(button.image);
        const SDL_Rect bounds{button.x, button.y, image.surface->w,
                              image.surface->h};
        if (setup.pressedControl == button.id && !setup.picker) {
            drawTournamentImage(button.image, button.x, button.y);
        }
        if (button.label != nullptr) {
            drawAligned(renderer, dialogFont,
                        language.get("tournament", button.label, ""), bounds,
                        Align::Centre);
        }
    }

    // The players: name left, difficulty right, three pixels in
    // (0x00419ce0); rows the font's height plus two.
    const int playerRow = calendarFont.height + 2;
    for (std::size_t index = 0; index < tournament.players.size(); ++index) {
        const int y = kPlayerList.y + static_cast<int>(index) * playerRow;
        if (y + playerRow > kPlayerList.y + kPlayerList.h) {
            break;
        }
        if (static_cast<int>(index) == setup.playerSelected) {
            fillSelection(renderer, SDL_Rect{kPlayerList.x, y, kPlayerList.w,
                                             playerRow});
        }
        const auto &player = tournament.players[index];
        drawText(renderer, calendarFont, player.name, kPlayerList.x, y);
        const auto &badge =
            setup.difficulties[static_cast<std::size_t>(player.difficulty)];
        const SDL_Rect at{kPlayerList.x + kPlayerList.w - badge.surface->w - 3,
                          y + (playerRow - badge.surface->h) / 2,
                          badge.surface->w, badge.surface->h};
        SDL_RenderCopy(renderer, badge.texture.get(), nullptr, &at);
    }
    // The games: rows the text's height (0x004087f0).
    const int gameRow = calendarFont.height;
    for (std::size_t index = 0; index < tournament.games.size(); ++index) {
        const int y = kGameList.y + static_cast<int>(index) * gameRow;
        if (static_cast<int>(index) == setup.gameSelected) {
            fillSelection(renderer,
                          SDL_Rect{kGameList.x, y, kGameList.w, gameRow});
        }
        drawText(renderer, calendarFont, tournament.games[index].title,
                 kGameList.x, y);
    }
}

void TournamentScene::renderTournamentPicker() {
    if (!tournamentScreen || !tournamentScreen->setup ||
        !tournamentScreen->setup->picker) {
        return;
    }
    auto &picker = *tournamentScreen->setup->picker;
    // 0x00407333: the modal mesh under the dialog.
    if (modalMeshTexture) {
        SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr, nullptr);
    }
    drawTournamentImage("common:dialogue\\listdlg\\backdrop.bmp", kPicker.x,
                        kPicker.y);
    drawAligned(renderer, calendarFont, picker.title, kPickerTitle,
                Align::Centre);
    const int visible = kPickerList.h / kPickerRowHeight;
    for (int row = 0; row < visible; ++row) {
        const int index = picker.firstVisible + row;
        if (index >= static_cast<int>(picker.items.size())) {
            break;
        }
        const int y = kPickerList.y + row * kPickerRowHeight;
        if (index == picker.selected) {
            fillSelection(renderer, SDL_Rect{kPickerList.x, y, kPickerList.w,
                                             kPickerRowHeight});
        }
        drawText(renderer, calendarFont,
                 picker.items[static_cast<std::size_t>(index)], kPickerList.x,
                 y);
    }
    const auto drawButton = [&](SDL_Point at, int id, const char *key,
                                const char *fallback) {
        const std::string image = picker.pressed == id
                                      ? "common:dialogue\\listdlg\\button_1.bmp"
                                      : "common:dialogue\\listdlg\\button_0.bmp";
        drawTournamentImage(image, at.x, at.y);
        const auto &button = tournamentImage(image);
        drawAligned(renderer, dialogFont,
                    language.get("interfaceh", key, fallback),
                    SDL_Rect{at.x, at.y, button.surface->w, button.surface->h},
                    Align::Centre);
    };
    drawButton(kPickerOk, kPickerOkId, "OK", "OK");
    drawButton(kPickerCancel, kPickerCancelId, "CANCEL", "Avbryt");
    drawTournamentImage(picker.pressed == 1 ? "common:dialogue\\listdlg\\up_1.bmp"
                                            : "common:dialogue\\listdlg\\up_0.bmp",
                        kPickerUp.x, kPickerUp.y);
    drawTournamentImage(picker.pressed == 2
                            ? "common:dialogue\\listdlg\\down_1.bmp"
                            : "common:dialogue\\listdlg\\down_0.bmp",
                        kPickerDown.x, kPickerDown.y);
    const int range =
        std::max(0, static_cast<int>(picker.items.size()) - visible);
    const auto &thumb = tournamentImage("common:dialogue\\listdlg\\thumb.bmp");
    const int centreY =
        kPickerTrack.y +
        (range > 0 ? kPickerTrack.h * picker.firstVisible / range : 0);
    drawTournamentImage("common:dialogue\\listdlg\\thumb.bmp",
                        kPickerTrack.x + kPickerTrack.w / 2 -
                            thumb.surface->w / 2,
                        centreY - thumb.surface->h / 2);
}

void TournamentScene::openTournamentPicker(bool players) {
    auto &setup = *tournamentScreen->setup;
    const auto warn = [&](const char *key) {
        showMessageWithTitle(language.get("interfaceh", "WARNING", "Advarsel"),
                             language.get("tournament", key, ""),
                             MessageKind::Notice);
    };
    TournamentPicker picker;
    picker.players = players;
    if (players) {
        // 0x0041a3f0 finds a free slot; 0x0041a410 the profiles not listed.
        if (static_cast<int>(tournament.players.size()) >= kMaxPlayers) {
            warn("MAXPLAYER");
            return;
        }
        for (const auto &name : profiles) {
            if (name == "No profile") {
                continue;
            }
            const bool listed = std::any_of(
                tournament.players.begin(), tournament.players.end(),
                [&](const TournamentPlayer &player) {
                    return player.name == name.substr(0, 24);
                });
            if (!listed) {
                picker.items.push_back(name);
            }
        }
        if (picker.items.empty()) {
            warn("MAXPROFILE");
            return;
        }
        picker.title = language.get("tournament", "NEWPLAYER", "Ny spiller");
    } else {
        if (setup.offered.empty()) {
            warn("NOSUBGAME");
            return;
        }
        if (static_cast<int>(tournament.games.size()) >= kMaxGames) {
            warn("MAXSUBGAME");
            return;
        }
        for (const auto &game : setup.offered) {
            picker.items.push_back(game.title);
        }
        picker.title = language.get("tournament", "NEWGAME", "Nytt spill");
    }
    setup.picker = std::move(picker);
}

void TournamentScene::closeTournamentPicker(bool accept) {
    auto &setup = *tournamentScreen->setup;
    auto picker = std::move(*setup.picker);
    setup.picker.reset();
    if (!accept || picker.selected < 0 ||
        picker.selected >= static_cast<int>(picker.items.size())) {
        return;
    }
    if (picker.players) {
        // 0x0041a2fa: a new player is medium.
        tournament.players.push_back(
            {picker.items[static_cast<std::size_t>(picker.selected)].substr(0, 24),
             1});
        setup.playerSelected = static_cast<int>(tournament.players.size()) - 1;
    } else {
        tournament.games.push_back(
            setup.offered[static_cast<std::size_t>(picker.selected)]);
        setup.gameSelected = static_cast<int>(tournament.games.size()) - 1;
    }
}

void TournamentScene::startTournament() {
    // 0x0041a520.
    const auto warn = [&](const char *key) {
        showMessageWithTitle(language.get("interfaceh", "WARNING", "Advarsel"),
                             language.get("tournament", key, ""),
                             MessageKind::Notice);
    };
    if (tournament.players.empty()) {
        warn("NOPLAYER");
        return;
    }
    if (tournament.games.empty()) {
        warn("NOGAME");
        return;
    }
    tournament.scores.assign(tournament.games.size(), {});
    tournament.player = 0;
    tournament.game = 0;
    tournament.active = true;
    const auto &game = tournament.games.front();
    enterScene(game.scene, game.scene, static_cast<int>(game.version));
}

void TournamentScene::tournamentSetupControl(int control) {
    // 0x0041a080.
    auto &setup = *tournamentScreen->setup;
    const auto move = [](auto &list, int &selected, int step) {
        // 0x00408330: the chosen item swaps one place.
        if (selected < 0 || selected >= static_cast<int>(list.size())) {
            return;
        }
        const int target = std::clamp(selected + step, 0,
                                      static_cast<int>(list.size()) - 1);
        if (target != selected) {
            std::swap(list[static_cast<std::size_t>(selected)],
                      list[static_cast<std::size_t>(target)]);
            selected = target;
        }
    };
    const auto remove = [](auto &list, int &selected) {
        // 0x004081e0: the chosen item goes, the choice stays in range.
        if (selected < 0 || selected >= static_cast<int>(list.size())) {
            return;
        }
        list.erase(list.begin() + selected);
        if (selected >= static_cast<int>(list.size())) {
            selected = static_cast<int>(list.size()) - 1;
        }
    };
    switch (control) {
    case 1: openTournamentPicker(true); break;
    case 2: remove(tournament.players, setup.playerSelected); break;
    case 3:
        tournament.players.clear();
        setup.playerSelected = -1;
        break;
    case 4: openTournamentPicker(false); break;
    case 5: remove(tournament.games, setup.gameSelected); break;
    case 6:
        tournament.games.clear();
        setup.gameSelected = -1;
        break;
    case 7: move(tournament.players, setup.playerSelected, -1); break;
    case 8: move(tournament.players, setup.playerSelected, 1); break;
    case 9: move(tournament.games, setup.gameSelected, -1); break;
    case 10: move(tournament.games, setup.gameSelected, 1); break;
    case 11: startTournament(); break;
    default: break;
    }
}

bool TournamentScene::beginTournamentPickerInteraction(int x, int y) {
    auto &picker = *tournamentScreen->setup->picker;
    const SDL_Point point{x, y};
    const auto hit = [&](SDL_Point at, const std::string &image) {
        const auto &bitmap = tournamentImage(image);
        const SDL_Rect bounds{at.x, at.y, bitmap.surface->w, bitmap.surface->h};
        return SDL_PointInRect(&point, &bounds) == SDL_TRUE;
    };
    if (hit(kPickerOk, "common:dialogue\\listdlg\\button_0.bmp")) {
        picker.pressed = kPickerOkId;
    } else if (hit(kPickerCancel, "common:dialogue\\listdlg\\button_0.bmp")) {
        picker.pressed = kPickerCancelId;
    } else if (hit(kPickerUp, "common:dialogue\\listdlg\\up_0.bmp")) {
        picker.pressed = 1;
        picker.firstVisible = std::max(0, picker.firstVisible - 1);
    } else if (hit(kPickerDown, "common:dialogue\\listdlg\\down_0.bmp")) {
        picker.pressed = 2;
        const int visible = kPickerList.h / kPickerRowHeight;
        picker.firstVisible = std::min(
            std::max(0, static_cast<int>(picker.items.size()) - visible),
            picker.firstVisible + 1);
    } else if (SDL_PointInRect(&point, &kPickerList)) {
        const int index =
            picker.firstVisible + (y - kPickerList.y) / kPickerRowHeight;
        if (index < static_cast<int>(picker.items.size())) {
            picker.selected = index;
        }
    }
    // The dialog is modal: every click is its own.
    return true;
}

bool TournamentScene::beginTournamentSetupInteraction(int x, int y) {
    auto &setup = *tournamentScreen->setup;
    if (setup.picker) {
        return beginTournamentPickerInteraction(x, y);
    }
    const SDL_Point point{x, y};
    for (const auto &button : kSetupButtons) {
        const auto &image = tournamentImage(button.image);
        const SDL_Rect bounds{button.x, button.y, image.surface->w,
                              image.surface->h};
        if (SDL_PointInRect(&point, &bounds)) {
            setup.pressedControl = button.id;
            return true;
        }
    }
    // 0x0041a5d0: a click picks a list item; a click on the player already
    // chosen steps its difficulty.
    if (SDL_PointInRect(&point, &kPlayerList)) {
        const int index = (y - kPlayerList.y) / (calendarFont.height + 2);
        if (index < static_cast<int>(tournament.players.size())) {
            if (index == setup.playerSelected) {
                auto &player =
                    tournament.players[static_cast<std::size_t>(index)];
                player.difficulty = (player.difficulty + 1) % 3;
            }
            setup.playerSelected = index;
        }
        return true;
    }
    if (SDL_PointInRect(&point, &kGameList)) {
        const int index = (y - kGameList.y) / calendarFont.height;
        if (index < static_cast<int>(tournament.games.size())) {
            setup.gameSelected = index;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Results screen

void TournamentScene::loadTournamentResults() {
    auto &results = tournamentScreen->results.emplace();
    const int rounds = tournament.active ? tournament.game + 1 : 0;
    // 0x0041a970: the list item, as the Scoreboard's without a medal.
    for (int round = 0; round < rounds; ++round) {
        const auto &game = tournament.games[static_cast<std::size_t>(round)];
        std::string picture = game.scene;
        if (game.version > 0) {
            picture += std::to_string(game.version);
        }
        SurfacePtr image =
            loadBmp(tournamentAsset("bitmap\\" + picture + ".bmp"));
        const int width = kResultList.w - 2;
        const int height = image->h + 4;
        SurfacePtr item(SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        SDL_FillRect(item.get(), nullptr, SDL_MapRGBA(item->format, 0, 0, 0, 0));
        SDL_Rect at{(width - image->w) / 2, 2, image->w, image->h};
        SDL_BlitSurface(image.get(), nullptr, item.get(), &at);
        TexturePtr texture = makeTexture(renderer, item.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        results.items.push_back({std::move(texture), width, height});
    }
    // 0x0041aaf0: placement points; a row tied with the one above takes its
    // points.
    for (int round = 0; round < rounds; ++round) {
        auto rows = tournament.scores[static_cast<std::size_t>(round)];
        for (std::size_t row = 0; row < rows.size(); ++row) {
            rows[row].points =
                row > 0 && rows[row].score == rows[row - 1].score
                    ? rows[row - 1].points
                    : kPlacementPoints[std::min<std::size_t>(row, 9U)];
        }
        results.gameRows.push_back(std::move(rows));
    }
    // 0x0041ab90: per round, each player's points in it (+0x1c) and the sum
    // so far (+0x20), ranked by the sum.
    for (int round = 0; round < rounds; ++round) {
        std::vector<TournamentRow> totals;
        for (const auto &player : tournament.players) {
            const std::string name = player.name.substr(0, 24);
            std::uint32_t sum = 0;
            std::uint32_t points = 0;
            for (int earlier = 0; earlier <= round; ++earlier) {
                points = 0;
                for (const auto &row :
                     results.gameRows[static_cast<std::size_t>(earlier)]) {
                    if (row.player == name) {
                        points += row.points;
                    }
                }
                sum += points;
            }
            insertByScore(totals, TournamentRow{name, points, sum});
        }
        results.totalRows.push_back(std::move(totals));
    }
    if (!results.items.empty()) {
        results.itemHeight = results.items.front().height;
        results.visibleCount =
            std::min(32, kResultList.h / std::max(1, results.itemHeight));
        selectTournamentResult(tournament.game);
    }
    // 0x0041a700: after the last game the winners are named at once.
    if (tournament.active &&
        tournament.game == static_cast<int>(tournament.games.size()) - 1 &&
        !results.totalRows.empty()) {
        const auto &final = results.totalRows.back();
        std::string names;
        for (std::size_t index = 0; index < final.size(); ++index) {
            if (final[index].score != final.front().score) {
                break;
            }
            std::size_t tied = 0;
            while (tied < final.size() &&
                   final[tied].score == final.front().score) {
                ++tied;
            }
            if (index > 0) {
                names += index + 1 < tied
                             ? language.get("tourneyscore", "COMMA", ", ")
                             : language.get("tourneyscore", "AND", " og ");
            }
            names += final[index].player;
        }
        showMessageWithTitle(
            language.get("tourneyscore", "WINNERTITLE", "Vinner"),
            formatOne(language.get("tourneyscore", "WINNER", "%s"), names),
            MessageKind::Notice);
    }
}

int TournamentScene::tournamentResultsScrollRange() const {
    const auto &results = *tournamentScreen->results;
    return std::max(0, static_cast<int>(results.items.size()) -
                           results.visibleCount);
}

void TournamentScene::scrollTournamentResults(int first) {
    auto &results = *tournamentScreen->results;
    results.firstVisible = std::clamp(first, 0, tournamentResultsScrollRange());
}

void TournamentScene::selectTournamentResult(int index) {
    auto &results = *tournamentScreen->results;
    if (results.items.empty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(results.items.size()) - 1);
    results.selected = index;
    if (index < results.firstVisible) {
        scrollTournamentResults(index);
    } else if (index >= results.firstVisible + results.visibleCount) {
        scrollTournamentResults(index - results.visibleCount + 1);
    }
}

void TournamentScene::renderTournamentResults() {
    auto &results = *tournamentScreen->results;
    drawTournamentImage("logo", kResultsLogo.x, kResultsLogo.y);
    drawAligned(renderer, menuTitleFont,
                language.get("tourneyscore", "SCENENAME", "Resultatliste"),
                kResultsTitle, Align::Centre);
    // 0x0041ac80: "%d / %d", the round and the number of games.
    drawAligned(renderer, dialogFont,
                std::to_string(tournament.game + 1) + " / " +
                    std::to_string(tournament.games.size()),
                kRoundBox, Align::Centre);
    static constexpr std::array<const char *, 2> tabKeys{"GAME", "TOTAL"};
    for (std::size_t index = 0; index < kResultTabX.size(); ++index) {
        const bool chosen = static_cast<int>(index) + 3 == results.tab;
        const std::string image = chosen ? "tab_1" : "tab_0";
        drawTournamentImage(image, kResultTabX[index], kResultTabY);
        const auto &tab = tournamentImage(image);
        drawAligned(renderer, dialogFont,
                    language.get("tourneyscore", tabKeys[index], ""),
                    SDL_Rect{kResultTabX[index], kResultTabY, tab.surface->w,
                             tab.surface->h},
                    Align::Centre);
    }
    drawAligned(renderer, dialogFont, language.get("interfaceh", "NAME", ""),
                kColumnHeaders[0], Align::Centre);
    drawAligned(renderer, dialogFont,
                language.get("tourneyscore", "AWARD", "Poengplass"),
                kColumnHeaders[1], Align::Centre);
    drawAligned(renderer, dialogFont, language.get("interfaceh", "POINTS", ""),
                kColumnHeaders[2], Align::Centre);

    if (results.selected >= results.firstVisible &&
        results.selected < results.firstVisible + results.visibleCount) {
        fillSelection(renderer,
                      SDL_Rect{kResultList.x,
                               kResultList.y +
                                   (results.selected - results.firstVisible) *
                                       results.itemHeight,
                               kResultList.w, results.itemHeight});
    }
    for (int row = 0; row < results.visibleCount; ++row) {
        const int index = results.firstVisible + row;
        if (index >= static_cast<int>(results.items.size())) {
            break;
        }
        const auto &item = results.items[static_cast<std::size_t>(index)];
        const SDL_Rect at{kResultList.x, kResultList.y + row * results.itemHeight,
                          item.width, item.height};
        SDL_RenderCopy(renderer, item.thumbnail.get(), nullptr, &at);
    }
    drawTournamentImage(results.pressedArrow < 0 ? "up_1" : "up_0",
                        kResultUp.x, kResultUp.y);
    drawTournamentImage(results.pressedArrow > 0 ? "down_1" : "down_0",
                        kResultDown.x, kResultDown.y);
    {
        const int range = tournamentResultsScrollRange();
        const int centreY =
            kResultTrack.y +
            (range > 0 ? kResultTrack.h * results.firstVisible / range : 0);
        const auto &thumb = tournamentImage("scrolltab");
        drawTournamentImage("scrolltab",
                            kResultTrack.x + kResultTrack.w / 2 -
                                thumb.surface->w / 2,
                            centreY - thumb.surface->h / 2);
    }
    const bool last =
        tournament.game == static_cast<int>(tournament.games.size()) - 1;
    drawTournamentImage(results.pressedNext ? "button_1" : "button_0",
                        kNextButton.x, kNextButton.y);
    {
        const auto &button = tournamentImage("button_0");
        drawAligned(renderer, dialogFont,
                    language.get("tourneyscore", last ? "ENDGAME" : "NEXTGAME",
                                 ""),
                    SDL_Rect{kNextButton.x, kNextButton.y, button.surface->w,
                             button.surface->h},
                    Align::Centre);
    }

    // 0x0041ae90: "%s (%d)" over the chosen round's rows (0x0041afc0).
    if (results.selected >= 0 &&
        results.selected < static_cast<int>(results.gameRows.size())) {
        const auto &game =
            tournament.games[static_cast<std::size_t>(results.selected)];
        drawAligned(renderer, calendarFont,
                    game.title + " (" + std::to_string(results.selected + 1) + ")",
                    kGameHeader, Align::Centre);
        const auto &rows =
            results.tab == 3
                ? results.gameRows[static_cast<std::size_t>(results.selected)]
                : results.totalRows[static_cast<std::size_t>(results.selected)];
        int y = 0xd2;
        for (const auto &row : rows) {
            drawAligned(renderer, calendarFont, row.player,
                        SDL_Rect{0xdc, y, 0xcb, 0x19}, Align::Left);
            drawAligned(renderer, calendarFont, std::to_string(row.points),
                        SDL_Rect{0x1c0, y, 0x69, 0x19}, Align::Centre);
            drawAligned(renderer, calendarFont, std::to_string(row.score),
                        SDL_Rect{0x240, y, 0xab, 0x19}, Align::Right);
            y += 0x19;
        }
    }
}

bool TournamentScene::beginTournamentResultsInteraction(int x, int y) {
    auto &results = *tournamentScreen->results;
    const SDL_Point point{x, y};
    for (std::size_t index = 0; index < kResultTabX.size(); ++index) {
        const auto &tab = tournamentImage("tab_0");
        const SDL_Rect bounds{kResultTabX[index], kResultTabY, tab.surface->w,
                              tab.surface->h};
        if (SDL_PointInRect(&point, &bounds)) {
            results.tab = static_cast<int>(index) + 3; // 0x0041b0c0
            return true;
        }
    }
    {
        const auto &button = tournamentImage("button_0");
        const SDL_Rect bounds{kNextButton.x, kNextButton.y, button.surface->w,
                              button.surface->h};
        if (SDL_PointInRect(&point, &bounds)) {
            results.pressedNext = true;
            return true;
        }
    }
    if (SDL_PointInRect(&point, &kResultList) && results.itemHeight > 0) {
        const int index =
            (y - kResultList.y) / results.itemHeight + results.firstVisible;
        if (index < static_cast<int>(results.items.size())) {
            selectTournamentResult(index); // 0x0041b320
        }
        return true;
    }
    const auto &up = tournamentImage("up_0");
    const SDL_Rect upBounds{kResultUp.x, kResultUp.y, up.surface->w,
                            up.surface->h};
    const auto &down = tournamentImage("down_0");
    const SDL_Rect downBounds{kResultDown.x, kResultDown.y, down.surface->w,
                              down.surface->h};
    if (SDL_PointInRect(&point, &upBounds)) {
        results.pressedArrow = -1;
        scrollTournamentResults(results.firstVisible - 1);
        return true;
    }
    if (SDL_PointInRect(&point, &downBounds)) {
        results.pressedArrow = 1;
        scrollTournamentResults(results.firstVisible + 1);
        return true;
    }
    if (SDL_PointInRect(&point, &kResultTrack)) {
        results.draggingThumb = true;
        moveTournamentPointer(x, y);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Dispatch

bool TournamentScene::beginTournamentInteraction(int x, int y) {
    if (!tournamentScreen) {
        return false;
    }
    if (tournamentScreen->setup) {
        return beginTournamentSetupInteraction(x, y);
    }
    if (tournamentScreen->results) {
        return beginTournamentResultsInteraction(x, y);
    }
    return false;
}

void TournamentScene::moveTournamentPointer(int, int y) {
    if (!tournamentScreen || !tournamentScreen->results ||
        !tournamentScreen->results->draggingThumb) {
        return;
    }
    const int range = tournamentResultsScrollRange();
    if (range > 0) {
        const int offset = std::clamp(y - kResultTrack.y, 0, kResultTrack.h);
        scrollTournamentResults((offset * range + kResultTrack.h / 2) /
                                kResultTrack.h);
    }
}

void TournamentScene::endTournamentInteraction(int x, int y) {
    if (!tournamentScreen) {
        return;
    }
    const SDL_Point point{x, y};
    if (tournamentScreen->setup) {
        auto &setup = *tournamentScreen->setup;
        if (setup.picker) {
            const int pressed = setup.picker->pressed;
            setup.picker->pressed = 0;
            const auto inside = [&](SDL_Point at) {
                const auto &image =
                    tournamentImage("common:dialogue\\listdlg\\button_0.bmp");
                const SDL_Rect bounds{at.x, at.y, image.surface->w,
                                      image.surface->h};
                return SDL_PointInRect(&point, &bounds) == SDL_TRUE;
            };
            if (pressed == kPickerOkId && inside(kPickerOk)) {
                closeTournamentPicker(true);
            } else if (pressed == kPickerCancelId && inside(kPickerCancel)) {
                closeTournamentPicker(false);
            }
            return;
        }
        const int pressed = setup.pressedControl;
        setup.pressedControl = 0;
        for (const auto &button : kSetupButtons) {
            if (button.id != pressed) {
                continue;
            }
            const auto &image = tournamentImage(button.image);
            const SDL_Rect bounds{button.x, button.y, image.surface->w,
                                  image.surface->h};
            if (SDL_PointInRect(&point, &bounds)) {
                tournamentSetupControl(pressed);
            }
        }
        return;
    }
    if (tournamentScreen->results) {
        auto &results = *tournamentScreen->results;
        const bool next = results.pressedNext;
        results.pressedNext = false;
        results.pressedArrow = 0;
        results.draggingThumb = false;
        const auto &button = tournamentImage("button_0");
        const SDL_Rect bounds{kNextButton.x, kNextButton.y, button.surface->w,
                              button.surface->h};
        if (next && SDL_PointInRect(&point, &bounds)) {
            advanceTournamentGame(); // control 0x29a, 0x0041b37f
        }
    }
}

bool TournamentScene::handleTournamentKey(SDL_Keycode key) {
    if (!tournamentScreen) {
        return false;
    }
    if (tournamentScreen->setup && tournamentScreen->setup->picker) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            closeTournamentPicker(true);
            return true;
        }
        if (key == SDLK_ESCAPE) {
            closeTournamentPicker(false);
            return true;
        }
        return true;
    }
    if (tournamentScreen->results) {
        // 0x0041b390: the list's keys (0x00407f60).
        auto &results = *tournamentScreen->results;
        int index = results.selected;
        switch (key) {
        case SDLK_PAGEUP: index -= results.visibleCount; break;
        case SDLK_PAGEDOWN: index += results.visibleCount; break;
        case SDLK_HOME: index = 0; break;
        case SDLK_END: index = static_cast<int>(results.items.size()) - 1; break;
        case SDLK_UP: index -= 1; break;
        case SDLK_DOWN: index += 1; break;
        default: return false;
        }
        selectTournamentResult(index);
        return true;
    }
    return false;
}

void TournamentScene::renderTournament() {
    if (!tournamentScreen) {
        return;
    }
    if (tournamentScreen->setup) {
        renderTournamentSetup();
    } else if (tournamentScreen->results) {
        renderTournamentResults();
    }
}

bool TournamentScene::executeTournamentDeveloperCommand(
    std::string_view command) {
    // tournament:control=N presses a setup control, tournament:pick=N picks
    // a row of the open list and confirms it, tournament:tab=N and
    // tournament:next drive the results screen, tournament:score=N ends the
    // current round with that score.
    constexpr std::string_view controlPrefix = "tournament:control=";
    constexpr std::string_view pickPrefix = "tournament:pick=";
    constexpr std::string_view tabPrefix = "tournament:tab=";
    constexpr std::string_view scorePrefix = "tournament:score=";
    if (command.rfind(scorePrefix, 0) == 0) {
        if (!tournament.active) {
            return false;
        }
        recordTournamentScore(
            std::stoi(std::string(command.substr(scorePrefix.size()))));
        return true;
    }
    if (!tournamentScreen) {
        return false;
    }
    if (command.rfind(controlPrefix, 0) == 0 && tournamentScreen->setup) {
        tournamentSetupControl(
            std::stoi(std::string(command.substr(controlPrefix.size()))));
        return true;
    }
    if (command.rfind(pickPrefix, 0) == 0 && tournamentScreen->setup &&
        tournamentScreen->setup->picker) {
        tournamentScreen->setup->picker->selected =
            std::stoi(std::string(command.substr(pickPrefix.size())));
        closeTournamentPicker(true);
        return true;
    }
    if (command.rfind(tabPrefix, 0) == 0 && tournamentScreen->results) {
        tournamentScreen->results->tab =
            std::stoi(std::string(command.substr(tabPrefix.size())));
        return true;
    }
    if (command == "tournament:next" && tournamentScreen->results) {
        advanceTournamentGame();
        return true;
    }
    return false;
}

} // namespace fgp
