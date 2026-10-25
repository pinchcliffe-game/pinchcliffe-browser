#include "game/app_info.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <string>

#include "game/activity_panel.h"
#include "util/binary.h"

namespace fgp {
namespace {

// The controls 0x00469290 creates, with their 0x116 ids.
constexpr int kHelpControl = 1;
constexpr int kExitControl = 2;
constexpr int kPreviousControl = 3;
constexpr int kNextControl = 4;
// 0x0049ff28: the picture area (left, top, right, bottom); 0x004693f0 centres
// the photograph in it with flags 0x101 (0x004124e0).
constexpr SDL_Rect kPictureArea{18, 85, 512 - 18, 579 - 85};
// 0x0049ff38: the text surface 0x00407930 creates.
constexpr SDL_Rect kTextArea{548, 85, 778 - 548, 494 - 85};
// 0x0049ff18: the title box 0x00407a30 centres the scene name in.
constexpr SDL_Rect kTitleArea{205, 12, 589 - 205, 60 - 12};
// 0x0049cab0: category 7 (butterflies) maps the album page to the
// butterfly.ini section through 0x0041b3d0.
const auto kButterflySections = originalArray<int, 15>(0x49cab0U);
// Award categories (profile award section, 0x00411b70): 0x0049ca60 holds
// each one's always-open default, 0x0049ca88 its entry count and 0x004ad89c
// its awarddlg name keys.
const auto kAwardDefaults = originalArray<std::uint32_t, 8>(0x49ca60U);
const auto kAwardCounts = originalArray<int, 8>(0x49ca88U);
constexpr std::array<std::string_view, 8> kAwardNames{{
    "PART", "FILM", "CAR", "TRACK", "IMAGE", "CHARACTER", "", "BUTTERFLY",
}};

// 0x0041b3d0.
int butterflySection(int page) {
    if (page < 0 || page >= static_cast<int>(kButterflySections.size()))
        return -1;
    return kButterflySections[static_cast<std::size_t>(page)];
}

// The award section's eight dwords at profile offset 0x58 (type 0x301).
std::array<std::uint32_t, 8> profileAwards(const std::optional<fs::path> &path) {
    std::array<std::uint32_t, 8> awards{};
    if (!path) return awards;
    const auto bytes = readBinaryFile(*path);
    if (bytes.size() < 0x78U || readLe32(bytes, 0x4cU) != 0x301U ||
        readLe32(bytes, 0x54U) != 0x20U)
        return awards;
    for (std::size_t index = 0; index < awards.size(); ++index)
        awards[index] = readLe32(bytes, 0x58U + 4U * index);
    return awards;
}

// 0x0041b5d0: is entry `page` of `category` open with profile value
// `owned`? Counting categories (0, 6) compare, the others test a bit
// (0x0041b660).
bool awardOpen(int category, std::uint32_t owned, int page) {
    const auto defaults = kAwardDefaults[static_cast<std::size_t>(category)];
    switch (category) {
    case 0:
    case 6:
        return page < static_cast<int>(defaults) ||
               page < static_cast<int>(owned);
    case 1: case 2: case 3: case 4: case 5: case 7: {
        const auto bit = 1U << (static_cast<unsigned>(page) & 31U);
        return (defaults & bit) != 0U || (owned & bit) != 0U;
    }
    default:
        return false;
    }
}

// 0x00469910: wrap (current + step) into 0..count-1. Like the original,
// a step of exactly -count from page 0 yields count (0x0046992d).
int wrapPage(int current, int count, int step) {
    const int target = step + current;
    if (count < 2) return 0;
    if (target < 0) return count - (-target % count);
    return target % count;
}

std::vector<std::string> iniSections(std::span<const std::uint8_t> bytes) {
    std::vector<std::string> sections;
    std::string contents(reinterpret_cast<const char *>(bytes.data()),
                         bytes.size());
    std::size_t position = 0;
    while (position < contents.size()) {
        auto end = contents.find_first_of("\r\n", position);
        if (end == std::string::npos) end = contents.size();
        std::string line = trim(contents.substr(position, end - position));
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
            sections.push_back(line.substr(1, line.size() - 2));
        position = end + 1;
    }
    return sections;
}

} // namespace

void InfoScene::requestInfo(InfoRequest request) {
    infoRequest = std::move(request);
    enterScene("info", "info");
}

// Scene initialisation 0x00469290.
InfoScene::InfoViewer InfoScene::loadInfoViewer() {
    InfoViewer viewer;
    const auto art = [&](std::string_view name) {
        return viewerAsset("bitmap\\" + std::string(name) + ".bmp");
    };
    // 0x004692b5-0x00469335: help (0,0) and exit (0x2d8,0) with _0/_1
    // artwork; previous (0x234,0x201) and next (0x2a5,0x201) only have a
    // pressed image, so the static hotspots.bmp ids 3 and 4 catch them.
    viewer.widgets.add(renderer, kHelpControl, 0, 0, art("help_0"),
                       art("help_1"));
    viewer.widgets.add(renderer, kExitControl, 0x2d8, 0, art("exit_0"),
                       art("exit_1"));
    viewer.widgets.add(renderer, kPreviousControl, 0x234, 0x201, {},
                       art("prev_1"));
    viewer.widgets.add(renderer, kNextControl, 0x2a5, 0x201, {},
                       art("next_1"));
    if (infoRequest) {
        // 0x004693a7-0x004693c3: "%s.ini" from record +4, opened through
        // the scene's packages. UNCERTAIN: the layer order. Data2 is read
        // first so the edition's eight-track track.ini (Data2, Language2)
        // wins over the base game's six (Data, Language), which is what
        // lets the racing menu's info step to tracks 6 and 7; the base
        // Language package never opens Language2 for this scene.
        const std::string article = infoRequest->article + ".ini";
        std::optional<std::span<const std::uint8_t>> bytes;
        if (sceneData2Package) {
            if (const auto *entry = sceneData2Package->findOptional(article))
                bytes = sceneData2Package->data(*entry);
        }
        if (!bytes) bytes = viewerAssetOptional(article);
        if (bytes) {
            viewer.sections = iniSections(*bytes);
            viewer.table = LanguageTable(*bytes);
        }
    }
    std::cout << "Loaded executable-derived Info: article "
              << (infoRequest ? infoRequest->article : std::string("(none)"))
              << ", " << viewer.sections.size()
              << " pages, award-filtered paging\n";
    return viewer;
}

// 0x004693f0: show one page.
void InfoScene::showInfoPage(int page) {
    if (!infoViewer || !infoRequest || infoViewer->sections.empty()) return;
    auto &viewer = *infoViewer;
    const auto &request = *infoRequest;
    viewer.page = page;
    // 0x0046942b-0x0046943a: remove the previous photograph (0x005098b8).
    if (viewer.pictureAdded) {
        viewer.pictureAdded = false;
        traceViewerRemove(viewer.pictureAsset);
    }
    // 0x00469449-0x00469461: "..\..\common\bitmap\large\%s_%02d" with the
    // page plus one, added at (-1,-1) on layer 0, then centred in
    // 0x0049ff28 (0x004124e0 flags 0x101) and moved there (0x00412cc0).
    char name[96]{};
    std::snprintf(name, sizeof(name), "..\\..\\common\\bitmap\\large\\%s_%02d",
                  request.article.c_str(), page + 1);
    viewer.pictureAsset = name;
    char commonName[64]{};
    std::snprintf(commonName, sizeof(commonName), "bitmap\\large\\%s_%02d.bmp",
                  request.article.c_str(), page + 1);
    viewer.picture.reset();
    viewer.pictureTexture.reset();
    try {
        viewer.picture = loadBmp(viewerCommonAsset(commonName));
        viewer.pictureTexture = makeTexture(renderer, viewer.picture.get());
        viewer.picturePosition = {
            (kPictureArea.w - viewer.picture->w) / 2 + kPictureArea.x,
            (kPictureArea.h - viewer.picture->h) / 2 + kPictureArea.y};
    } catch (const std::exception &error) {
        std::cerr << "Info picture " << commonName << ": " << error.what()
                  << '\n';
    }
    viewer.pictureAdded = true;
    traceViewerAdd(viewer.pictureAsset, -1, -1, 0);

    // 0x004694b4: category 7 maps the page to its section (0x0041b3d0).
    int section = page;
    if (request.awardCategory == 7) section = butterflySection(page);
    viewer.lines.clear();
    if (section < 0 || section >= static_cast<int>(viewer.sections.size()))
        return;
    const std::string &sectionName =
        viewer.sections[static_cast<std::size_t>(section)];
    // 0x004694e6 onwards: text0, text1, ... until a key is missing (the
    // default "<EMPTY>" at 0x004afa24); "<NAME>" is replaced by the
    // award name 0x0041b580 returns; font%d (default 0, clamped 0..2)
    // picks Amerigo 10, 14 or 18.
    for (int index = 0;; ++index) {
        std::string text = viewer.table.get(
            sectionName, "text" + std::to_string(index), "<EMPTY>");
        if (text == "<EMPTY>") break;
        if (text == "<NAME>") {
            const int category = request.awardCategory;
            int entry = section;
            std::string key = "NONE";
            if (category >= 0 && category < 8 && entry >= 0 &&
                entry < kAwardCounts[static_cast<std::size_t>(category)]) {
                if (category == 7) entry = butterflySection(entry);
                key = std::string(kAwardNames[static_cast<std::size_t>(category)]) +
                      "_" + std::to_string(entry + 1);
            }
            text = language.get("awarddlg", key, key);
        }
        int font = 0;
        try {
            font = std::stoi(viewer.table.get(
                sectionName, "font" + std::to_string(index), "0"));
        } catch (const std::exception &) {
            font = 0;
        }
        viewer.lines.push_back({std::move(text), std::clamp(font, 0, 2)});
    }
}

// 0x00469860: the next open page in a direction, or -1.
int InfoScene::nextInfoPage(int step) {
    const auto &viewer = *infoViewer;
    const int count = static_cast<int>(viewer.sections.size());
    if (count == 0) return -1;
    const int category = infoRequest->awardCategory;
    if (category < 0) return wrapPage(viewer.page, count, step);
    if (category >= 8) return -1;
    const auto owned = profileAwards(viewerProfilePath())
        [static_cast<std::size_t>(category)];
    int offset = step;
    for (int tried = 1; tried <= count; ++tried, offset += step) {
        const int candidate = wrapPage(viewer.page, count, offset);
        if (awardOpen(category, owned, candidate)) return candidate;
    }
    return -1;
}

void InfoScene::returnFromInfo() {
    // 0x004697e6-0x004697f3 (and 0x00469947 for Esc): back to the record's
    // scene with its mode.
    if (!infoRequest) {
        returnFromScene();
        return;
    }
    if (!navigationHistory.empty()) navigationHistory.pop_back();
    pendingSceneEntryMode = infoRequest->returnMode;
    pendingScene = std::pair{infoRequest->returnScene, infoRequest->returnScene};
}

// Control event 0x116, 0x004697d0.
void InfoScene::infoControl(int id) {
    traceViewerControl("info", id);
    switch (id) {
    case kHelpControl:
        showSceneHelp();
        break;
    case kExitControl:
        returnFromInfo();
        break;
    case kPreviousControl:
    case kNextControl: {
        // 0x00469820.
        if (!infoRequest || !infoViewer || infoViewer->sections.empty()) break;
        const int page = nextInfoPage(id == kNextControl ? 1 : -1);
        if (page >= 0) showInfoPage(page);
        break;
    }
    default:
        break;
    }
}

void InfoScene::updateInfo() {
    if (!infoViewer || infoViewer->entered) return;
    infoViewer->entered = true;
    // 0x004693d6: the record's page (+0x14) once the article is open.
    if (infoRequest && !infoViewer->sections.empty())
        showInfoPage(infoRequest->page);
}

void InfoScene::leaveInfoScene() {
    if (infoViewer && infoRequest) infoRequest->page = infoViewer->page;
}

void InfoScene::renderInfo() {
    if (!infoViewer) return;
    auto &viewer = *infoViewer;
    drawCenteredText(renderer, menuTitleFont,
                     language.get("info", "SCENENAME", "Infosenter"),
                     kTitleArea);
    if (viewer.pictureAdded && viewer.pictureTexture) {
        SDL_Rect destination{viewer.picturePosition.x, viewer.picturePosition.y,
                             viewer.picture->w, viewer.picture->h};
        SDL_RenderCopy(renderer, viewer.pictureTexture.get(), nullptr,
                       &destination);
    }
    // 0x0046967b draws each line with the paragraph layout 0x00402750 on
    // the text surface; it advances by the larger of its font height and
    // its wrapped height (0x00402670).
    const std::array<const BitmapFont *, 3> fonts{
        &dialogFont, &calendarFont, &menuTitleFont};
    int top = kTextArea.y;
    for (const auto &line : viewer.lines) {
        const BitmapFont &font = *fonts[static_cast<std::size_t>(line.font)];
        const auto wrapped = breakParagraph(font, line.text, kTextArea.w);
        int y = top;
        for (const auto &part : wrapped) {
            if (y + font.height > kTextArea.y + kTextArea.h) break;
            drawText(renderer, font,
                     line.text.substr(part.start, part.length), kTextArea.x,
                     y);
            y += font.height;
        }
        top += std::max(font.height,
                        static_cast<int>(wrapped.size()) * font.height);
    }
    viewer.widgets.render(
        renderer, mouseX, mouseY,
        viewer.widgets.eventIdAt(mouseX, mouseY,
                                 viewerStaticHotspotAt(mouseX, mouseY)),
        [](const ViewerWidget &, const SDL_Rect &) {});
}

bool InfoScene::beginInfoInteraction(int x, int y) {
    if (!infoViewer) return false;
    if (const auto id =
            infoViewer->widgets.press(x, y, viewerStaticHotspotAt(x, y)))
        infoControl(*id);
    return true;
}

bool InfoScene::endInfoInteraction(int x, int y) {
    if (!infoViewer) return false;
    if (const auto id =
            infoViewer->widgets.release(x, y, viewerStaticHotspotAt(x, y)))
        infoControl(*id);
    return true;
}

// Key event 0x10c, 0x00469940: Esc returns through the record and the
// callback reports it handled (0x0046925b).
bool InfoScene::handleInfoKey(SDL_Keycode key) {
    if (!infoViewer) return false;
    if (key == SDLK_ESCAPE) {
        returnFromInfo();
        return true;
    }
    return false;
}

bool InfoScene::executeInfoDeveloperCommand(std::string_view command) {
    // Refused (and so retried next frame) until the first page is shown.
    if (!infoViewer || !infoViewer->entered) return false;
    if (command == "next") {
        infoControl(kNextControl);
    } else if (command == "prev") {
        infoControl(kPreviousControl);
    } else {
        return false;
    }
    return true;
}

} // namespace fgp
