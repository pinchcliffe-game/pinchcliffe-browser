#include "game/app_buttercol.h"

#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "game/awards.h"

namespace fgp {

namespace {

// 0x0043a2a0 gives Help id 2 and Exit id 1; 0x00432d10 dispatches them.
constexpr std::uint32_t kExit = 1;
constexpr std::uint32_t kHelp = 2;
// 0x0043a340: specimen n gets hotspot 0x14 + n and is added at layer -10.
constexpr std::uint32_t kFirstSpecimen = 0x14;
constexpr int kSpecimenLayer = -10;
constexpr std::uint32_t kSpecimenTable = 0x004d8bd0U;
// 0x0043a3b0's info record: article "butterfly" (0x004adc98), award type 7.
constexpr std::string_view kCheat = "LOVELYFLIES"; // 0x004d8c5c
constexpr std::uint32_t kCheatSoundRva = 0x000a6498U; // 0x0040ae50

SurfacePtr toArgb(SDL_Surface *surface) {
    SurfacePtr converted(
        SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ARGB8888, 0));
    if (!converted) {
        throw std::runtime_error(std::string("SDL_ConvertSurfaceFormat failed: ") +
                                 SDL_GetError());
    }
    return converted;
}

} // namespace

SurfacePtr ButtercolScene::buttercolBitmap(std::string_view name) {
    const std::string asset = "bitmap\\" + std::string(name) + ".bmp";
    for (Package *candidate :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (!candidate) continue;
        if (const auto *entry = candidate->findOptional(asset))
            return toArgb(loadBmp(candidate->data(*entry)).get());
    }
    throw std::runtime_error("Butterfly album bitmap not found: " + asset);
}

ButtercolScene::ButtercolAlbum ButtercolScene::loadButtercolAlbum() {
    ButtercolAlbum album;
    album.stage = std::make_unique<ActivityStage>(renderer, referenceTrace);
    album.panel = std::make_unique<ActivityPanel>(
        *album.stage,
        [this](std::string_view name) { return buttercolBitmap(name); });
    for (std::size_t index = 0; index < album.specimens.size(); ++index) {
        const std::uint32_t address =
            executableGameData.u32At(kSpecimenTable + 4U * static_cast<std::uint32_t>(index));
        const auto definition = executableGameData.animationAtRva(address - 0x400000U);
        const int sprite = album.stage->create(
            definition.name, definition.x, definition.y,
            kFirstSpecimen + static_cast<std::uint32_t>(index), definition.keyed);
        album.stage->setSurface(sprite, buttercolBitmap(definition.name));
        album.specimens[index] = sprite;
    }
    // 0x0041b540(7) and 0x0041b5d0: the album shows every species the
    // butterfly award value (or its initial value) includes.
    std::uint32_t value = 0;
    if (const auto path = activeProfilePath()) {
        try {
            value = profileAwards(*path)[kAwardButterflies];
        } catch (const std::exception &error) {
            std::cerr << "Could not read awards from " << *path << ": "
                      << error.what() << '\n';
        }
    }
    int shown = 0;
    for (std::size_t index = 0; index < album.collected.size(); ++index) {
        album.collected[index] = awardItemAvailable(
            executableGameData, kAwardButterflies, value, static_cast<int>(index));
        shown += album.collected[index] ? 1 : 0;
    }
    std::cout << "Loaded executable-derived butterfly album: " << shown
              << " of 15 species\n";
    return album;
}

std::uint32_t ButtercolScene::buttercolHotspotAt(int x, int y) {
    auto &album = *buttercolAlbum;
    if (const auto hit = album.stage->hotspotAt(x, y)) return *hit;
    if (!sceneHotspots) return 0;
    if (x < 0 || y < 0 || x >= sceneHotspots->w || y >= sceneHotspots->h)
        return 0;
    return pixelValue(sceneHotspots.get(), x, y);
}

void ButtercolScene::buttercolHotspotChanged(std::uint32_t hotspot) {
    auto &album = *buttercolAlbum;
    if (hotspot == album.hotspot) return;
    album.panel->setHover(album.hotspot, false);
    album.hotspot = hotspot;
    album.panel->setHover(hotspot, true);
}

// 0x0043a340.
void ButtercolScene::showButtercolSpecimens() {
    auto &album = *buttercolAlbum;
    for (std::size_t index = 0; index < album.specimens.size(); ++index) {
        const int sprite = album.specimens[index];
        album.stage->removeIfActive(sprite);
        if (album.collected[index]) album.stage->add(sprite, -1, -1, kSpecimenLayer);
    }
}

// Scene initialisation 0x0043a2a0.
void ButtercolScene::announceButtercolEntry() {
    auto &album = *buttercolAlbum;
    album.announced = true;
    const std::uint32_t hover = buttercolHotspotAt(mouseX, mouseY);
    album.hotspot = hover;
    using Type = ActivityPanel::Type;
    album.panel->addControl(kHelp, 0, 0, {"help_0", "help_1", "", ""},
                            Type::Push, 0, {}, hover, true);
    album.panel->addControl(kExit, 0x2d8, 0, {"exit_0", "exit_1", "", ""},
                            Type::Push, 0, {}, hover, true);
    showButtercolSpecimens();
}

// 0x0043a3b0, on event 0x105.
void ButtercolScene::openButtercolSpecimen(std::uint32_t hotspot) {
    if (hotspot < kFirstSpecimen || hotspot >= kFirstSpecimen + 15U) return;
    infoRequest = InfoRequest{"butterfly", "buttercol", 0, kAwardButterflies,
                              static_cast<int>(hotspot - kFirstSpecimen)};
    enterScene("info", "info");
}

// 0x00432d10, on event 0x116.
void ButtercolScene::buttercolControlClicked(std::uint32_t id) {
    if (id == kExit) {
        // 0x0040cc30: the registry parent (0x004d8c10 +8) is butterfly, a
        // type 3 scene, so the entry mode is 0.
        if (!navigationHistory.empty()) navigationHistory.pop_back();
        enterScene("butterfly", "butterfly", 0);
    } else if (id == kHelp) {
        showSceneHelp();
    }
}

// 0x0040ae60 on every key (0x0040d30f): LOVELYFLIES, registered at
// 0x0043a33b, plays the cheat sound and runs 0x0043a390, which shows every
// species for this visit without writing the profile.
void ButtercolScene::buttercolKey(SDL_Keycode key) {
    auto &album = *buttercolAlbum;
    int code = static_cast<int>(key);
    if (code >= 'a' && code <= 'z') code -= 'a' - 'A';
    if (code == kCheat[album.cheatProgress]) {
        ++album.cheatProgress;
    } else {
        album.cheatProgress = 0;
    }
    if (album.cheatProgress == kCheat.size()) {
        album.cheatProgress = 0;
        playSceneSound(kCheatSoundRva, "cheat");
        album.collected.fill(true);
        showButtercolSpecimens();
    }
}

bool ButtercolScene::handleButtercolEvent(const SDL_Event &event) {
    if (!buttercolAlbum || activeScene != "buttercol" ||
        !buttercolAlbum->announced)
        return false;
    auto &album = *buttercolAlbum;
    switch (event.type) {
    case SDL_MOUSEMOTION:
        buttercolHotspotChanged(buttercolHotspotAt(event.motion.x, event.motion.y));
        return true;
    case SDL_MOUSEBUTTONDOWN: {
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        const std::uint32_t hotspot =
            buttercolHotspotAt(event.button.x, event.button.y);
        buttercolHotspotChanged(hotspot);
        if (const auto clicked =
                album.panel->press(hotspot, event.button.x, event.button.y))
            buttercolControlClicked(*clicked);
        openButtercolSpecimen(hotspot);
        return true;
    }
    case SDL_MOUSEBUTTONUP:
        if (event.button.button != SDL_BUTTON_LEFT) return false;
        buttercolHotspotChanged(buttercolHotspotAt(event.button.x, event.button.y));
        if (const auto clicked = album.panel->release())
            buttercolControlClicked(*clicked);
        return true;
    case SDL_KEYDOWN:
        if (!event.key.repeat) buttercolKey(event.key.keysym.sym);
        // Escape and the shared keys go on to the engine.
        return false;
    default:
        return false;
    }
}

void ButtercolScene::updateButtercol(double) {
    if (!buttercolAlbum || activeScene != "buttercol") return;
    if (!buttercolAlbum->announced) announceButtercolEntry();
}

void ButtercolScene::renderButtercol() {
    if (!buttercolAlbum || activeScene != "buttercol" ||
        !buttercolAlbum->announced)
        return;
    buttercolAlbum->stage->render();
}

bool ButtercolScene::executeButtercolDeveloperCommand(std::string_view command) {
    constexpr std::string_view prefix = "buttercol:";
    if (activeScene != "buttercol" || !buttercolAlbum ||
        !buttercolAlbum->announced)
        return false;
    auto &album = *buttercolAlbum;
    const auto control = command.substr(prefix.size());
    if (control.rfind("open:", 0) == 0) {
        const int index = std::stoi(std::string(control.substr(5)));
        if (index < 0 || index >= 15 ||
            !album.collected[static_cast<std::size_t>(index)])
            return false;
        openButtercolSpecimen(kFirstSpecimen + static_cast<std::uint32_t>(index));
    } else if (control == "cheat") {
        for (const char character : kCheat) buttercolKey(character);
    } else if (control == "exit") {
        buttercolControlClicked(kExit);
    } else if (control == "status") {
        std::cout << "Butterfly album status:";
        for (std::size_t index = 0; index < album.collected.size(); ++index)
            std::cout << ' ' << (album.collected[index] ? '1' : '0');
        std::cout << '\n';
    } else {
        throw std::runtime_error("Unknown developer butterfly album control: " +
                                 std::string(control));
    }
    return true;
}

} // namespace fgp
