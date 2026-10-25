#include "game/app_mainscore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "platform/leaderboard.h"
#include "util/text.h"

namespace fgp {

namespace {

// Controls and objects created at 0x00417180 and 0x00416d50.
constexpr SDL_Rect kListBounds{48, 143, 132 - 48, 518 - 143}; // 0x0049c938
constexpr Uint32 kListFrameColour = 0x808080;                  // 0x0041697a
constexpr std::array<int, 4> kTabX{0xd1, 0x146, 0x1ba, 0x27e};
constexpr int kTabY = 0x62;
constexpr SDL_Point kUpArrow{0x95, 0x88};
constexpr SDL_Point kDownArrow{0x95, 0x1fd};
constexpr SDL_Rect kScrollTrack{0x95, 0xa8, 0xe, 0x146};
// Text objects (left, top, right, bottom) from 0x00416d50.
constexpr SDL_Rect kTitleBounds{0xce, 0x19, 0x24b - 0xce, 0x49 - 0x19};
constexpr SDL_Rect kPlayerBounds{0x29, 0x66, 0xa0 - 0x29, 0x7f - 0x66};
constexpr SDL_Rect kGameNameBounds{0xc9, 0x8a, 0x2ef - 0xc9, 0xa5 - 0x8a};
constexpr SDL_Rect kRowsBounds{0xce, 0xd2, 0x2ec - 0xce, 0x20b - 0xd2};
constexpr std::array<SDL_Rect, 3> kHeaderBounds{{
    {0xd0, 0xad, 0x1a5 - 0xd0, 0xc7 - 0xad},
    {0x1b5, 0xad, 0x227 - 0x1b5, 0xc7 - 0xad},
    {0x237, 0xad, 0x2ea - 0x237, 0xc7 - 0xad}}};
// The logo descriptor at 0x004ab9c0 and carpartsbg at 0x004aba68 carry
// their default positions at +0x54/+0x58.
constexpr SDL_Point kLogo{0xd9, 0x1d};
constexpr SDL_Point kCarPartsBackground{0xc6, 0x88};

// Horizontal placement of 0x004124e0: flags & 0xff00 is 0 left, 0x100
// centre, 0x200 right; every text object here centres vertically (& 0xff
// is 1).
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

} // namespace

std::span<const std::uint8_t>
MainscoreScene::mainscoreAsset(const std::string &asset) {
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
    throw std::runtime_error("Scoreboard asset not found: " + asset);
}

MainscoreScene::MainscoreImage &
MainscoreScene::mainscoreImage(const std::string &name) {
    auto &images = mainscoreBoard->images;
    auto found = images.find(name);
    if (found == images.end()) {
        MainscoreImage image;
        image.surface =
            loadGreenKeyedBmp(mainscoreAsset("bitmap\\" + name + ".bmp"));
        image.texture = makeTexture(renderer, image.surface.get());
        SDL_SetTextureBlendMode(image.texture.get(), SDL_BLENDMODE_BLEND);
        found = images.emplace(name, std::move(image)).first;
    }
    return found->second;
}

void MainscoreScene::drawMainscoreImage(const std::string &name, int x,
                                        int y) {
    const auto &image = mainscoreImage(name);
    const SDL_Rect destination{x, y, image.surface->w, image.surface->h};
    SDL_RenderCopy(renderer, image.texture.get(), nullptr, &destination);
}

void MainscoreScene::loadMainscoreBoard() {
    mainscoreBoard.emplace();
    MainscoreBoard &board = *mainscoreBoard;
    const bool hasProfile =
        profileIndex < profiles.size() && profiles[profileIndex] != "No profile";
    // 0x00411eb0: the active profile's name, or profile:NONAME.
    board.playerName = hasProfile ? profiles[profileIndex]
                                  : language.get("profile", "NONAME", "");
    const fs::path activeProfile =
        hasProfile ? profileDirectory / (profiles[profileIndex] + ".pfl")
                   : fs::path{};
    if (hasProfile) {
        try {
            // 0x0041b540(0) reads award value 0; 0x0041b690 counts the
            // parts below it.
            board.carPartCount = profileCarPartCount(activeProfile);
        } catch (const std::exception &error) {
            std::cerr << "Could not read car parts from " << activeProfile
                      << ": " << error.what() << '\n';
        }
    }

    // 0x00416960 walks subgame.ini's game%d/version%d/name%d keys, up to 20.
    const auto ini = mainscoreAsset("subgame.ini");
    const LanguageTable subgames(ini);
    for (int index = 0; index < 20; ++index) {
        const std::string number = std::to_string(index);
        const std::string scene = subgames.get("subgame", "game" + number);
        if (scene.empty()) {
            break;
        }
        MainscoreGame game;
        game.scene = scene;
        game.version = static_cast<std::uint32_t>(
            std::max(0, std::atoi(subgames.get("subgame", "version" + number,
                                               "0").c_str())));
        // Only games the active profile has a score in are listed
        // (0x004169ee tests the loaded count).
        if (!hasProfile) {
            continue;
        }
        std::optional<ProfileScoreTable> own;
        try {
            own = profileSceneScores(activeProfile, scene, game.version);
        } catch (const std::exception &error) {
            std::cerr << "Could not read " << scene << " scores from "
                      << activeProfile << ": " << error.what() << '\n';
        }
        if (!own || own->entries.empty()) {
            continue;
        }
        game.medal = own->medalLevel;
        for (const auto &entry : own->entries) {
            game.rows[0].push_back(
                {profiles[profileIndex], entry.timestamp, entry.score});
        }
        // name%d is a language key; otherwise the scene's own title key
        // from the scene table (src/generated/game_manifest.json).
        game.title =
            subgameTitle(scene, subgames.get("subgame", "name" + number));

        // 0x0041ebd0 -> 0x00411d40: up to 64 profiles' tables in the profile
        // list's order; 0x0041eaf0 merges them into Global, 0x0041eb60 into
        // Union. The same lists the score dialog shows (score_tables.cpp).
        try {
            const ScoreLists lists =
                loadScoreLists(scoreProfiles(), scene, game.version, 0U);
            for (const auto &row : lists.global.rows) {
                game.rows[1].push_back({row.player, row.timestamp, row.score});
            }
            for (const auto &row : lists.players.rows) {
                game.rows[2].push_back({row.player, row.timestamp, row.score});
            }
        } catch (const std::exception &error) {
            std::cerr << "Could not read " << scene << " score lists: "
                      << error.what() << '\n';
        }
        // Browser build: "every registered player" is everyone on the shared
        // leaderboard. Mountain's two tracks share one scene there, so they
        // keep the local lists.
        if (scene != "mountain") {
            if (const auto shared = leaderboardEntries(scene)) {
                game.rows[1].clear();
                game.rows[2].clear();
                ScoreList global;
                ScoreList best;
                for (const auto &entry : *shared) {
                    std::tm parsed{};
                    std::uint32_t timestamp = 0;
                    if (entry.date.size() >= 10 &&
                        std::sscanf(entry.date.c_str(), "%d-%d-%d",
                                    &parsed.tm_year, &parsed.tm_mon,
                                    &parsed.tm_mday) == 3) {
                        parsed.tm_year -= 1900;
                        parsed.tm_mon -= 1;
                        parsed.tm_hour = 12;
                        timestamp = static_cast<std::uint32_t>(
                            std::mktime(&parsed));
                    }
                    const ScoreRow row{
                        utf8ToCp1252(entry.player).value_or("?"), timestamp,
                        entry.score};
                    insertScore(global, row, 0U);
                    insertPlayerBest(best, row, 0U);
                }
                for (const auto &row : global.rows) {
                    game.rows[1].push_back(
                        {row.player, row.timestamp, row.score});
                }
                for (const auto &row : best.rows) {
                    game.rows[2].push_back(
                        {row.player, row.timestamp, row.score});
                }
            }
        }

        // 0x00416ba0: the list item is the list's width less two by the
        // picture's height plus four, keyed green, with the picture centred
        // two pixels down; 0x00416b40 lays the medal in the top-right corner.
        std::string picture = scene;
        if (game.version > 0) {
            picture += std::to_string(game.version);
        }
        SurfacePtr image =
            loadBmp(mainscoreAsset("bitmap\\" + picture + ".bmp"));
        const int width = kListBounds.w - 2;
        const int height = image->h + 4;
        SurfacePtr item(SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        if (!item) {
            throw std::runtime_error(std::string("SDL_CreateRGBSurface: ") +
                                     SDL_GetError());
        }
        SDL_FillRect(item.get(), nullptr, SDL_MapRGBA(item->format, 0, 0, 0, 0));
        SDL_Rect pictureAt{(width - image->w) / 2, 2, image->w, image->h};
        SDL_BlitSurface(image.get(), nullptr, item.get(), &pictureAt);
        if (game.medal >= 1U && game.medal <= 4U) {
            static constexpr std::array<const char *, 4> medals{
                "bronze", "silver", "gold", "platinum"}; // 0x00416900
            SurfacePtr medal = loadGreenKeyedBmp(mainscoreAsset(
                std::string("bitmap\\") + medals[game.medal - 1U] + ".bmp"));
            SDL_Rect medalAt{width - medal->w, 0, medal->w, medal->h};
            SDL_BlitSurface(medal.get(), nullptr, item.get(), &medalAt);
        }
        game.thumbnail = makeTexture(renderer, item.get());
        SDL_SetTextureBlendMode(game.thumbnail.get(), SDL_BLENDMODE_BLEND);
        game.thumbnailWidth = width;
        game.thumbnailHeight = height;
        board.games.push_back(std::move(game));
    }

    // 0x00408160: the item height is the first item's, and the list shows
    // as many as fit, at most 32.
    if (!board.games.empty()) {
        board.itemHeight = board.games.front().thumbnailHeight;
        board.visibleCount =
            std::min(32, kListBounds.h / std::max(1, board.itemHeight));
        board.selected = 0;
    }

    // 0x00416ca0: part_01..part_62, nine to a row from (0xd7, 0x97).
    const auto commonData = [&](const std::string &asset) {
        for (Package *package :
             {addonCommon.get(), data2Common.get(), editionCommon.get()}) {
            if (package) {
                if (const auto *entry = package->findOptional(asset)) {
                    return package->data(*entry);
                }
            }
        }
        return common.data(common.find(asset));
    };
    for (int part = 0; part < static_cast<int>(board.carPartCount); ++part) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "bitmap\\award\\part_%02d.bmp",
                      part + 1);
        MainscoreImage image;
        image.surface = loadGreenKeyedBmp(commonData(name));
        image.texture = makeTexture(renderer, image.surface.get());
        SDL_SetTextureBlendMode(image.texture.get(), SDL_BLENDMODE_BLEND);
        board.parts.push_back(std::move(image));
    }
    // 0x00409d00 sets tooltips in Small Fonts 7.
    board.tooltipFont = loadBitmapFont(
        renderer, commonData("fonts\\Small Fonts_07_.bmp"));
    for (const char *name : {"tooltip", "tooldot"}) {
        MainscoreImage image;
        image.surface =
            loadGreenKeyedBmp(commonData(std::string("bitmap\\") + name + ".bmp"));
        image.texture = makeTexture(renderer, image.surface.get());
        SDL_SetTextureBlendMode(image.texture.get(), SDL_BLENDMODE_BLEND);
        board.images.emplace(std::string("common:") + name, std::move(image));
    }

}

int MainscoreScene::mainscoreScrollRange() const {
    const auto &board = *mainscoreBoard;
    return std::max(0, static_cast<int>(board.games.size()) -
                           board.visibleCount);
}

void MainscoreScene::scrollMainscoreList(int first) {
    // 0x00407af0 clamps the first visible item to the list.
    auto &board = *mainscoreBoard;
    board.firstVisible = std::clamp(first, 0, mainscoreScrollRange());
}

void MainscoreScene::selectMainscoreGame(int index) {
    // 0x00407e60 selects and scrolls the item into view; 0x00416f30 then
    // shows its rows.
    auto &board = *mainscoreBoard;
    if (board.games.empty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(board.games.size()) - 1);
    board.selected = index;
    if (index < board.firstVisible) {
        scrollMainscoreList(index);
    } else if (index >= board.firstVisible + board.visibleCount) {
        scrollMainscoreList(index - board.visibleCount + 1);
    }
}

bool MainscoreScene::beginMainscoreInteraction(int x, int y) {
    auto &board = *mainscoreBoard;
    const SDL_Point point{x, y};
    // Tabs: controls 3-5 pick a list (0x00417170) and take the car parts
    // down (0x00417550); control 6 shows them (0x004174e0).
    for (std::size_t index = 0; index < kTabX.size(); ++index) {
        const auto &tab = mainscoreImage("tab_0");
        const SDL_Rect bounds{kTabX[index], kTabY, tab.surface->w,
                              tab.surface->h};
        if (SDL_PointInRect(&point, &bounds)) {
            if (index == 3U) {
                board.carParts = true;
            } else {
                board.carParts = false;
                board.hoveredPart = -1;
                board.tab = static_cast<int>(index) + 3;
            }
            return true;
        }
    }
    // 0x00417420: a click on the list selects the item under it
    // (0x00407f00).
    if (SDL_PointInRect(&point, &kListBounds) && board.itemHeight > 0) {
        const int index =
            (y - kListBounds.y) / board.itemHeight + board.firstVisible;
        if (index >= 0 && index < static_cast<int>(board.games.size())) {
            selectMainscoreGame(index);
        }
        return true;
    }
    // Scroll control 0x65: the arrows step one item (0x004175e0 takes the
    // new value), the thumb drags.
    const auto &up = mainscoreImage("up_0");
    const SDL_Rect upBounds{kUpArrow.x, kUpArrow.y, up.surface->w,
                            up.surface->h};
    const auto &down = mainscoreImage("down_0");
    const SDL_Rect downBounds{kDownArrow.x, kDownArrow.y, down.surface->w,
                              down.surface->h};
    if (SDL_PointInRect(&point, &upBounds)) {
        board.pressedArrow = -1;
        scrollMainscoreList(board.firstVisible - 1);
        return true;
    }
    if (SDL_PointInRect(&point, &downBounds)) {
        board.pressedArrow = 1;
        scrollMainscoreList(board.firstVisible + 1);
        return true;
    }
    if (SDL_PointInRect(&point, &kScrollTrack)) {
        board.draggingThumb = true;
        moveMainscorePointer(x, y);
        return true;
    }
    return false;
}

void MainscoreScene::moveMainscorePointer(int x, int y) {
    auto &board = *mainscoreBoard;
    if (board.draggingThumb) {
        // 0x00408eb0 inverted: the value whose thumb centre is nearest y.
        const int range = mainscoreScrollRange();
        if (range > 0) {
            const int offset = std::clamp(y - kScrollTrack.y, 0, kScrollTrack.h);
            scrollMainscoreList((offset * range + kScrollTrack.h / 2) /
                                kScrollTrack.h);
        }
        return;
    }
    // 0x00417450: hotspots 500-561 are the parts; hovering one shows its
    // award name (0x0041b580) as a tooltip.
    board.hoveredPart = -1;
    if (!board.carParts) {
        return;
    }
    for (int part = static_cast<int>(board.parts.size()) - 1; part >= 0;
         --part) {
        const SDL_Surface *surface = board.parts[part].surface.get();
        const int left = 0xd7 + (part % 9) * 0x3b;
        const int top = 0x97 + (part / 9) * 0x35;
        if (x < left || y < top || x >= left + surface->w ||
            y >= top + surface->h) {
            continue;
        }
        Uint32 key = 0;
        const bool keyed =
            SDL_GetColorKey(const_cast<SDL_Surface *>(surface), &key) == 0;
        if (keyed &&
            pixelValue(const_cast<SDL_Surface *>(surface), x - left, y - top) ==
                key) {
            continue;
        }
        board.hoveredPart = part;
        return;
    }
}

void MainscoreScene::endMainscoreInteraction() {
    auto &board = *mainscoreBoard;
    board.pressedArrow = 0;
    board.draggingThumb = false;
}

bool MainscoreScene::handleMainscoreKey(SDL_Keycode key) {
    // 0x004175a0 passes the key to the list (0x00407f60): Page Up/Down move
    // a page, Home/End to the ends, the arrows one item.
    auto &board = *mainscoreBoard;
    if (board.games.empty()) {
        return false;
    }
    int index = board.selected;
    switch (key) {
    case SDLK_PAGEUP: index -= board.visibleCount; break;
    case SDLK_PAGEDOWN: index += board.visibleCount; break;
    case SDLK_HOME: index = 0; break;
    case SDLK_END: index = static_cast<int>(board.games.size()) - 1; break;
    case SDLK_UP: index -= 1; break;
    case SDLK_DOWN: index += 1; break;
    default: return false;
    }
    selectMainscoreGame(index);
    return true;
}

void MainscoreScene::drawMainscoreTooltip(const std::string &text) {
    // 0x00409c00/0x00409d00: the text in Small Fonts 7 on tooltip.bmp
    // stretched as a nine-slice around the inner rectangle (6,11)-(38,15)
    // (0x0049c790, 0x00420b00), twelve wider and eighteen taller than the
    // text. It sits under the cursor, above it when it would leave the
    // screen, and tooldot.bmp marks the pointer's tip inside it
    // (0x00409dd0).
    // UNCERTAIN: 0x00420dc0, which draws each slice, is read as a stretch
    // rather than a tile.
    auto &board = *mainscoreBoard;
    const BitmapFont &font = board.tooltipFont;
    const int width = textWidth(font, text) + 12;
    const int height = font.height + 18;
    int cursorWidth = 32;
    int cursorHeight = 32;
    const auto &cursor = cursors[static_cast<std::size_t>(CursorKind::Default)];
    if (!cursor.media.videoFrames.empty()) {
        cursorWidth = cursor.media.videoFrames.front().width;
        cursorHeight = cursor.media.videoFrames.front().height;
    }
    int left = mouseX;
    int top = mouseY + cursorHeight;
    if (left + width > kLogicalWidth) {
        left = kLogicalWidth - width;
    }
    if (top + height > kLogicalHeight) {
        top = mouseY - height;
    }
    (void)cursorWidth;
    const auto &box = board.images.at("common:tooltip");
    const int sourceWidth = box.surface->w;
    const int sourceHeight = box.surface->h;
    constexpr int innerLeft = 6, innerTop = 11, innerRight = 38, innerBottom = 15;
    const std::array<int, 4> sourceX{0, innerLeft, innerRight, sourceWidth};
    const std::array<int, 4> sourceY{0, innerTop, innerBottom, sourceHeight};
    const std::array<int, 4> targetX{0, innerLeft,
                                     width - (sourceWidth - innerRight), width};
    const std::array<int, 4> targetY{0, innerTop,
                                     height - (sourceHeight - innerBottom),
                                     height};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const SDL_Rect source{sourceX[column], sourceY[row],
                                  sourceX[column + 1] - sourceX[column],
                                  sourceY[row + 1] - sourceY[row]};
            const SDL_Rect target{left + targetX[column], top + targetY[row],
                                  targetX[column + 1] - targetX[column],
                                  targetY[row + 1] - targetY[row]};
            SDL_RenderCopy(renderer, box.texture.get(), &source, &target);
        }
    }
    drawAligned(renderer, font, text, SDL_Rect{left, top, width, height},
                Align::Centre);
    const auto &dot = board.images.at("common:tooldot");
    const int dotX = std::clamp(mouseX, left, left + width - dot.surface->w);
    const int dotY = std::clamp(mouseY, top, top + height - dot.surface->h);
    const SDL_Rect dotAt{dotX, dotY, dot.surface->w, dot.surface->h};
    SDL_RenderCopy(renderer, dot.texture.get(), nullptr, &dotAt);
}

void MainscoreScene::renderMainscore() {
    auto &board = *mainscoreBoard;
    drawMainscoreImage("logo", kLogo.x, kLogo.y);
    drawAligned(renderer, menuTitleFont,
                language.get("mainscore", "SCENENAME", "Poengtavle"),
                kTitleBounds, Align::Centre);
    drawAligned(renderer, dialogFont, board.playerName, kPlayerBounds,
                Align::Centre);

    // Tab controls 3-6: radio buttons in group 1, the chosen one in tab_1.
    static constexpr std::array<std::pair<const char *, const char *>, 4>
        tabLabels{{{"interfaceh", "SCORE_PROFILE"},
                   {"interfaceh", "SCORE_GLOBAL"},
                   {"interfaceh", "SCORE_UNION"},
                   {"mainscore", "CARPARTS"}}};
    const int chosenTab = board.carParts ? 6 : board.tab;
    for (std::size_t index = 0; index < kTabX.size(); ++index) {
        const bool chosen = static_cast<int>(index) + 3 == chosenTab;
        const std::string image = chosen ? "tab_1" : "tab_0";
        drawMainscoreImage(image, kTabX[index], kTabY);
        const auto &tab = mainscoreImage(image);
        drawAligned(renderer, dialogFont,
                    language.get(tabLabels[index].first,
                                 tabLabels[index].second, ""),
                    SDL_Rect{kTabX[index], kTabY, tab.surface->w,
                             tab.surface->h},
                    Align::Centre);
    }

    static constexpr std::array<const char *, 3> headers{"NAME", "DATE",
                                                         "POINTS"};
    for (std::size_t index = 0; index < headers.size(); ++index) {
        drawAligned(renderer, dialogFont,
                    language.get("interfaceh", headers[index], ""),
                    kHeaderBounds[index], Align::Centre);
    }

    // The game list: the chosen item stands on a frame-coloured block the
    // list's width (0x00407db0), the items over it (0x00407c00).
    if (board.selected >= board.firstVisible &&
        board.selected < board.firstVisible + board.visibleCount) {
        const SDL_Rect frame{
            kListBounds.x,
            kListBounds.y + (board.selected - board.firstVisible) *
                                board.itemHeight,
            kListBounds.w, board.itemHeight};
        SDL_SetRenderDrawColor(renderer, (kListFrameColour >> 16) & 0xff,
                               (kListFrameColour >> 8) & 0xff,
                               kListFrameColour & 0xff, 255);
        SDL_RenderFillRect(renderer, &frame);
    }
    for (int row = 0; row < board.visibleCount; ++row) {
        const int index = board.firstVisible + row;
        if (index >= static_cast<int>(board.games.size())) {
            break;
        }
        const auto &game = board.games[index];
        const SDL_Rect destination{kListBounds.x,
                                   kListBounds.y + row * board.itemHeight,
                                   game.thumbnailWidth, game.thumbnailHeight};
        SDL_RenderCopy(renderer, game.thumbnail.get(), nullptr, &destination);
    }
    drawMainscoreImage(board.pressedArrow < 0 ? "up_1" : "up_0", kUpArrow.x,
                       kUpArrow.y);
    drawMainscoreImage(board.pressedArrow > 0 ? "down_1" : "down_0",
                       kDownArrow.x, kDownArrow.y);
    {
        // 0x00408eb0/0x00408f30: the thumb is centred on the track's
        // position for the first visible item.
        const int range = mainscoreScrollRange();
        const int centreY =
            kScrollTrack.y +
            (range > 0 ? kScrollTrack.h * board.firstVisible / range : 0);
        const auto &thumb = mainscoreImage("scrolltab");
        drawMainscoreImage("scrolltab",
                           kScrollTrack.x + kScrollTrack.w / 2 -
                               thumb.surface->w / 2,
                           centreY - thumb.surface->h / 2);
    }

    // 0x00416f40/0x00416ff0: the chosen game's name and the chosen list,
    // rows 30 apart from five below the rows' top; name left at +13 in
    // 190, date centred at +232 in 113, score right in +373..+530.
    if (board.selected >= 0 &&
        board.selected < static_cast<int>(board.games.size())) {
        const auto &game = board.games[board.selected];
        drawAligned(renderer, calendarFont, game.title, kGameNameBounds,
                    Align::Centre);
        const auto &rows = game.rows[static_cast<std::size_t>(board.tab - 3)];
        int y = kRowsBounds.y + 5;
        for (const auto &row : rows) {
            drawAligned(renderer, calendarFont, row.player,
                        SDL_Rect{0xdb, y, 0xbe, 0x1e}, Align::Left);
            drawAligned(renderer, calendarFont, formatScoreDate(row.timestamp),
                        SDL_Rect{0x1b6, y, 0x71, 0x1e}, Align::Centre);
            drawAligned(renderer, calendarFont, std::to_string(row.score),
                        SDL_Rect{0x243, y, 0x9d, 0x1e}, Align::Right);
            y += 0x1e;
        }
    }

    if (board.carParts) {
        drawMainscoreImage("carpartsbg", kCarPartsBackground.x,
                           kCarPartsBackground.y);
        for (std::size_t part = 0; part < board.parts.size(); ++part) {
            const auto &image = board.parts[part];
            const SDL_Rect destination{
                0xd7 + static_cast<int>(part % 9) * 0x3b,
                0x97 + static_cast<int>(part / 9) * 0x35, image.surface->w,
                image.surface->h};
            SDL_RenderCopy(renderer, image.texture.get(), nullptr,
                           &destination);
        }
        if (board.hoveredPart >= 0) {
            drawMainscoreTooltip(language.get(
                "awarddlg", "PART_" + std::to_string(board.hoveredPart + 1),
                ""));
        }
    }
}

bool MainscoreScene::executeMainscoreDeveloperCommand(
    std::string_view command) {
    if (!mainscoreBoard) {
        return false;
    }
    constexpr std::string_view gamePrefix = "mainscore:game=";
    constexpr std::string_view tabPrefix = "mainscore:tab=";
    constexpr std::string_view hoverPrefix = "mainscore:hover=";
    if (command.rfind(hoverPrefix, 0) == 0) {
        const std::string point(command.substr(hoverPrefix.size()));
        const auto comma = point.find(',');
        if (comma == std::string::npos) {
            return false;
        }
        mouseX = std::stoi(point.substr(0, comma));
        mouseY = std::stoi(point.substr(comma + 1));
        moveMainscorePointer(mouseX, mouseY);
        return true;
    }
    if (command.rfind(gamePrefix, 0) == 0) {
        selectMainscoreGame(std::stoi(std::string(command.substr(gamePrefix.size()))));
        return true;
    }
    if (command.rfind(tabPrefix, 0) == 0) {
        const int control = std::stoi(std::string(command.substr(tabPrefix.size())));
        if (control < 3 || control > 6) {
            return false;
        }
        mainscoreBoard->carParts = control == 6;
        if (control != 6) {
            mainscoreBoard->tab = control;
        }
        return true;
    }
    return false;
}

} // namespace fgp
