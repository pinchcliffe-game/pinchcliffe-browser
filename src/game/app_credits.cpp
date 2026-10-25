#include "game/app_credits.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "assets/language_table.h"

namespace fgp {
namespace {

// 0x0049c928: the text rectangle (350,5)-(775,479).
constexpr SDL_Rect kTextRect{350, 5, 425, 474};
// 0x0049c918: the scroll track (781,107)-(793,563), and the arrows
// 0x004091f0 places at (780,77) and (780,577).
constexpr SDL_Rect kTrack{781, 107, 12, 456};
constexpr SDL_Point kUpArrow{780, 77};
constexpr SDL_Point kDownArrow{780, 577};
constexpr std::uint32_t kScrollId = 1;
// 0x00406ec0(2): the control layer; the thumb goes one below.
constexpr int kControlLayer = 2;
constexpr int kThumbLayer = 1;
// 0x00415e1c-0x00415e1f: one scroll step is 24 pixels of text.
constexpr int kScrollStep = 24;
// 0x00415eb8: Page Up and Page Down move 18 steps.
constexpr int kPageSteps = 18;
// Descriptor 0x004a99c0's default position, where 0x00415d57 parks the
// logo when the last drawn entry has no image.
constexpr SDL_Point kSmackerParked{0, 3000};

constexpr std::string_view kMissing{"\x01"};

SurfacePtr toArgb(SDL_Surface *surface) {
    SurfacePtr converted(
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) throw std::runtime_error(SDL_GetError());
    SDL_SetSurfaceBlendMode(converted.get(), SDL_BLENDMODE_NONE);
    return converted;
}

} // namespace

SurfacePtr CreditsScene::creditsBitmap(std::string_view name) {
    const std::string asset = "bitmap\\" + std::string(name) + ".bmp";
    for (Package *candidate :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional(asset))
            return toArgb(loadBmp(candidate->data(*entry)).get());
    }
    throw std::runtime_error("Credits bitmap not found: " + asset);
}

CreditsScene::CreditsGame CreditsScene::loadCreditsGame() {
    CreditsGame game;
    game.stage = std::make_unique<ActivityStage>(renderer, referenceTrace);
    game.panel = std::make_unique<ActivityPanel>(
        *game.stage, [this](std::string_view name) { return creditsBitmap(name); });
    game.scrollbar =
        std::make_unique<ActivityScrollbar>(*game.stage, *game.panel);

    // 0x004157cf-0x004157e8: White Amerigo BT_10_ and _14_ from the layered
    // common package.
    const auto font = [this](std::string_view name) {
        const std::string asset = "fonts\\" + std::string(name) + ".bmp";
        for (Package *candidate : {addonCommon.get(), data2Common.get(),
                                   editionCommon.get(), &common}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(asset))
                return loadBitmapFont(renderer, candidate->data(*entry));
        }
        throw std::runtime_error("Credits font not found: " + asset);
    };
    game.fonts[0] = font("White Amerigo BT_10_");
    game.fonts[1] = font("White Amerigo BT_14_");

    // 0x004157ed: credits.ini through the scene's package layers. The
    // edition's own copy (Data3/Language3, with "Dag Gundersen" and
    // "Kunnskapsforlaget" in text37) wins over the base Language layer: the
    // 2026-10-18 capture's last step ends 3188 pixels of text, not 3144.
    std::span<const std::uint8_t> ini;
    for (Package *candidate :
         {sceneAddonPackage.get(), sceneData2Package.get(),
          sceneEditionPackage.get(), sceneLanguagePackage.get(),
          scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional("credits.ini")) {
            ini = candidate->data(*entry);
            break;
        }
    }
    const LanguageTable table(ini);

    // 0x004159c0: entries run while textN exists (an empty text still takes
    // a line). Each records its top, every line adds its font's height and
    // the widest line sets the width, then pauseN adds that many font-0
    // line heights (0x00415b70 multiplies by 0x0055bfbc).
    int width = 0;
    int height = 0;
    for (int index = 0;; ++index) {
        const std::string number = std::to_string(index);
        std::string text = table.get("credits", "text" + number,
                                     std::string(kMissing));
        if (text == kMissing) break;
        CreditsEntry entry;
        // 0x00415b35: fontN defaults to 1 and is clamped to 0..1.
        try {
            entry.font = std::clamp(
                std::stoi(table.get("credits", "font" + number, "1")), 0, 1);
        } catch (const std::exception &) {
            entry.font = 0;
        }
        try {
            entry.pause = std::stoi(table.get("credits", "pause" + number, "0"));
        } catch (const std::exception &) {
            entry.pause = 0;
        }
        entry.image = table.get("credits", "image" + number,
                                std::string(kMissing)) != kMissing;
        std::size_t start = 0;
        while (true) {
            const auto end = text.find('\n', start);
            entry.lines.push_back(text.substr(
                start, end == std::string::npos ? std::string::npos
                                                : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        game.tops.push_back(height);
        const auto &lineFont = game.fonts[static_cast<std::size_t>(entry.font)];
        for (const auto &line : entry.lines) {
            width = std::max(width, textWidth(lineFont, line));
            height += lineFont.height;
        }
        height += entry.pause * game.fonts[0].height;
        game.entries.push_back(std::move(entry));
    }

    // 0x00415813-0x0041586c: the viewport is the text's width centred
    // horizontally in the rectangle (0x004124e0 flag 0x100), at its top and
    // full height.
    game.viewport = SDL_Rect{kTextRect.x + (kTextRect.w - width) / 2,
                             kTextRect.y, width, kTextRect.h};
    // 0x00415871: the text descriptor 0x0055aee0, layer 0.
    game.text = game.stage->create({}, game.viewport.x, game.viewport.y);
    game.stage->setSurface(game.text,
                           createSurface(game.viewport.w, game.viewport.h));
    game.overlay = game.stage->create("overlay", 0, 0, 0, true);
    game.stage->setSurface(game.overlay, creditsBitmap("overlay"));
    game.smacker = game.stage->create("smacker", kSmackerParked.x,
                                      kSmackerParked.y, 0, true);
    game.stage->setSurface(game.smacker, creditsBitmap("smacker"));

    game.height = height;
    std::cout << "Recovered credits: " << game.entries.size() << " entries, "
              << width << "x" << height << " text\n";
    return game;
}

void CreditsScene::drawCredits(int index, int offset) {
    auto &game = *creditsGame;
    SDL_Surface *surface = game.stage->sprite(game.text).surface.get();
    // 0x00407880 clears the text descriptor.
    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    int y = offset;
    for (int entry = index; entry < static_cast<int>(game.entries.size());
         ++entry) {
        const auto &credits = game.entries[static_cast<std::size_t>(entry)];
        const auto &font = game.fonts[static_cast<std::size_t>(credits.font)];
        for (const auto &line : credits.lines) {
            // 0x00415c67: each line centred in (0, y, width, lineHeight).
            const SDL_Point at = centreIn(
                SDL_Rect{0, y, game.viewport.w, font.height},
                textWidth(font, line), font.height);
            drawFontText(surface, font, line, at.x, at.y);
            y += font.height;
        }
        // 0x00415d20: an entry with imageN centres the logo horizontally in
        // the viewport at the text's y (not offset by the viewport's top,
        // as the capture's (532,332) at the last step shows); any other
        // entry parks it, so only the last drawn entry decides.
        if (credits.image) {
            const SDL_Rect logo = game.stage->rect(game.smacker);
            game.stage->move(game.smacker,
                             game.viewport.x + (game.viewport.w - logo.w) / 2,
                             y);
        } else {
            game.stage->move(game.smacker, kSmackerParked.x,
                             kSmackerParked.y);
        }
        y += credits.pause * game.fonts[0].height;
        // 0x00415ce2: stop once the text reaches the viewport's bottom.
        if (game.viewport.h <= y) break;
    }
    game.stage->touch(game.text);
}

void CreditsScene::showCreditsAt(int value) {
    auto &game = *creditsGame;
    // 0x00415e40: the last entry whose top is at or above the position.
    const int position = value * kScrollStep;
    int index = 0;
    while (index < static_cast<int>(game.tops.size()) &&
           position >= game.tops[static_cast<std::size_t>(index)]) {
        ++index;
    }
    index = std::clamp(index - 1, 0,
                       std::max(0, static_cast<int>(game.tops.size()) - 1));
    drawCredits(index, game.tops.empty()
                           ? 0
                           : game.tops[static_cast<std::size_t>(index)] -
                                 position);
}

void CreditsScene::setCreditsValue(int value) {
    // 0x00408eb0 posts event 0x117 when the value changes.
    if (creditsGame->scrollbar->setValue(value))
        showCreditsAt(creditsGame->scrollbar->value());
}

void CreditsScene::updateCredits(double) {
    auto &game = *creditsGame;
    if (game.announced) return;
    // 0x004157c0 (event 0x103), run on the first update so that its adds
    // follow the scene change: the text, help and exit (the shared corner
    // buttons), overlay at layer 1, the parked logo, then the scroll control
    // 0x004090e0(1, track, "thumb", flag 0x10: vertical, unkeyed), whose
    // thumb is added at layer 1, and its arrows (flags 0x10000 up, 0x1000
    // down), ids from 2000 (0x00408c70 resets the counter).
    game.announced = true;
    game.stage->add(game.text, game.viewport.x, game.viewport.y, 0);
    game.stage->add(game.overlay, -1, -1, 1);
    game.stage->add(game.smacker, -1, -1, 0);
    game.panel->setLayer(kControlLayer);
    game.scrollbar->create(kScrollId, kTrack, creditsBitmap("thumb"),
                           kScrollId, kThumbLayer, true, "thumb");
    game.scrollbar->addButton(kUpArrow.x, kUpArrow.y, "up_0", "up_1", true, 0);
    game.scrollbar->addButton(kDownArrow.x, kDownArrow.y, "down_0", "down_1",
                              false, 0);
    game.scrollbar->setStep(1);
    // 0x0041596b-0x004159a1: the range is the overflow in 24-pixel steps,
    // C division, plus one; then the first entry at the top.
    game.scrollbar->setValue(0);
    game.scrollbar->setRange(0, (game.height - kTextRect.h) / kScrollStep + 1);
    drawCredits(0, 0);
}

void CreditsScene::closeCreditsGame() {
    auto &game = *creditsGame;
    if (game.announced) {
        // The capture's order after EnterScene menu: the text (0x00415deb),
        // then the scroll control's thumb and its arrows down then up, help
        // and exit (the shared corner buttons, traced elsewhere), smacker
        // and overlay.
        auto &stage = *game.stage;
        stage.removeIfActive(game.text);
        stage.removeIfActive(game.scrollbar->thumbSprite());
        for (const std::uint32_t id : {2001U, 2000U}) {
            for (const auto &control : game.panel->controls()) {
                if (control.id == id && control.sprite >= 0)
                    stage.removeIfActive(control.sprite);
            }
        }
        stage.removeIfActive(game.smacker);
        stage.removeIfActive(game.overlay);
    }
    creditsGame.reset();
}

void CreditsScene::renderCredits(double) {
    if (!creditsGame || !creditsGame->announced) return;
    creditsGame->stage->render();
}

bool CreditsScene::handleCreditsEvent(const SDL_Event &event) {
    if (!creditsGame || activeScene != "credits" || !creditsGame->announced)
        return false;
    auto &game = *creditsGame;
    switch (event.type) {
    case SDL_MOUSEMOTION: {
        const int x = event.motion.x;
        const int y = event.motion.y;
        const auto hotspot = game.stage->hotspotAt(x, y).value_or(0U);
        for (const auto &control : game.panel->controls())
            game.panel->setHover(control.id, control.id == hotspot);
        if (game.dragging) {
            if (game.scrollbar->dragTo(y))
                showCreditsAt(game.scrollbar->value());
            return true;
        }
        return false;
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        const int x = event.button.x;
        const int y = event.button.y;
        const auto hotspot = game.stage->hotspotAt(x, y).value_or(0U);
        if (hotspot >= 2000U) {
            game.panel->press(hotspot, x, y);
            return true;
        }
        bool dragging = false;
        const bool changed = game.scrollbar->pressTrack(x, y, dragging);
        if (!dragging) return false;
        game.dragging = true;
        if (changed) showCreditsAt(game.scrollbar->value());
        return true;
    }
    case SDL_MOUSEBUTTONUP: {
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        if (game.dragging) {
            game.dragging = false;
            return true;
        }
        if (const auto id = game.panel->release()) {
            // 0x00409450 (event 0x116): the arrows step the value.
            if (game.scrollbar->clickButton(*id))
                showCreditsAt(game.scrollbar->value());
            return true;
        }
        return false;
    }
    case SDL_KEYDOWN: {
        // 0x00415ea0 (event 0x10c).
        const int value = game.scrollbar->value();
        switch (event.key.keysym.sym) {
        case SDLK_SPACE:
            // 0x00415f1e: Space jumps to the parent scene (0x0040cc30).
            returnFromScene();
            return true;
        case SDLK_PAGEUP:
            setCreditsValue(value - kPageSteps);
            return true;
        case SDLK_PAGEDOWN:
            setCreditsValue(value + kPageSteps);
            return true;
        case SDLK_END:
            // 0x00408e90: the range's maximum.
            setCreditsValue(std::numeric_limits<int>::max());
            return true;
        case SDLK_HOME:
            setCreditsValue(0);
            return true;
        case SDLK_UP:
            setCreditsValue(value - 1);
            return true;
        case SDLK_DOWN:
            setCreditsValue(value + 1);
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

} // namespace fgp
