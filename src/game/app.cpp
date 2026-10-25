#include "game/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "game/menu_names.h"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/console.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

GameApp::GameApp(int argc, char **argv) {
#if defined(FGP_DEVELOPMENT_MODE) && FGP_DEVELOPMENT_MODE
    developmentMode = true;
#endif
    if (const char *value = std::getenv("FGP_DEV_MODE")) {
        const std::string requested = normalizedPath(value);
        developmentMode = requested == "1" || requested == "true" ||
                          requested == "yes" || requested == "on";
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        constexpr std::string_view commandPrefix = "--dev-command=";
        constexpr std::string_view exitPrefix = "--dev-exit-after=";
        if (argument.rfind(commandPrefix, 0) == 0) {
            developerCommands.emplace_back(
                argument.substr(commandPrefix.size()));
        } else if (argument.rfind(exitPrefix, 0) == 0) {
            developerExitAfter = std::stod(
                std::string(argument.substr(exitPrefix.size())));
        } else if (argument == "--dev-console") {
            developerConsoleRequested = true;
        }
    }
    if (const char *value = std::getenv("FGP_DEV_CONSOLE")) {
        const std::string requested = normalizedPath(value);
        developerConsoleRequested = requested == "1" || requested == "true" ||
                                    requested == "yes" || requested == "on";
    }
    if (const char *commands = std::getenv("FGP_DEV_COMMANDS")) {
        std::string pending(commands);
        std::size_t offset = 0;
        while (offset <= pending.size()) {
            const std::size_t separator = pending.find(';', offset);
            const std::string command = pending.substr(
                offset, separator == std::string::npos
                            ? std::string::npos
                            : separator - offset);
            if (!command.empty()) {
                developerCommands.push_back(command);
            }
            if (separator == std::string::npos) {
                break;
            }
            offset = separator + 1;
        }
    }
    if (!developmentMode && !developerCommands.empty()) {
        throw std::runtime_error(
            "Developer commands require FGP development mode");
    }
    if (!developmentMode && developerConsoleRequested) {
        throw std::runtime_error(
            "The developer console requires FGP development mode");
    }
    // Do this before the first diagnostic so nothing is lost to the old
    // stream bindings.
    if (developmentMode && openDeveloperConsole(developerConsoleRequested)) {
        std::cout << "FGP Native developer console\n";
    }

    gameDirectory = findGameDirectory(argc, argv);
    const fs::path menuPath = packagePath(gameDirectory, "Data", "menu");
    const fs::path commonPath = packagePath(gameDirectory, "Data", "common");
    if (!fs::exists(menuPath) || !fs::exists(commonPath)) {
        throw std::runtime_error(
            "Could not find Data/menu.bin and Data/common.bin under: " +
            gameDirectory.string() +
            "\n\nPut the original game's files in a folder called \"game\" "
            "beside this program, or start it with the game's folder as its "
            "argument or in FGP_GAME_DIR.");
    }

    std::cout << "Reading original assets from " << gameDirectory << '\n';
    menu = Package(menuPath);
    common = Package(commonPath);
    executableGameData = ExecutableGameData(gameDirectory / "FGP.exe");
    const fs::path editionCommonPath =
        packagePath(gameDirectory, "Data1", "common");
    if (fs::exists(editionCommonPath)) {
        editionCommon = std::make_unique<Package>(editionCommonPath);
    }
    const fs::path data2CommonPath =
        packagePath(gameDirectory, "Data2", "common");
    if (fs::exists(data2CommonPath)) {
        data2Common = std::make_unique<Package>(data2CommonPath);
    }
    const fs::path addonCommonPath =
        packagePath(gameDirectory, "Data3", "common");
    if (fs::exists(addonCommonPath)) {
        addonCommon = std::make_unique<Package>(addonCommonPath);
    }
    std::unique_ptr<Package> storyIndexConfigurationPackage;
    for (const std::string_view layer : {"Data3", "Data2", "Data1"}) {
        const fs::path storyIndexConfigurationPath =
            packagePath(gameDirectory, layer, "sceneindex");
        if (fs::exists(storyIndexConfigurationPath)) {
            storyIndexConfigurationPackage =
                std::make_unique<Package>(storyIndexConfigurationPath);
            if (const auto *entry = storyIndexConfigurationPackage
                                        ->findOptional("scene.ini")) {
                storyIndexConfiguration = std::make_unique<LanguageTable>(
                    storyIndexConfigurationPackage->data(*entry));
                break;
            }
        }
    }
    std::cout << "Parsed " << menu.entries().size() << " menu assets and "
              << common.entries().size() << " common assets";
    if (editionCommon) {
        std::cout << " plus " << editionCommon->entries().size()
                  << " edition-specific common assets";
    }
    if (data2Common) {
        std::cout << ", " << data2Common->entries().size()
                  << " Data2 common assets";
    }
    if (addonCommon) {
        std::cout << ", " << addonCommon->entries().size()
                  << " Data3 common assets";
    }
    std::cout << '\n';
    std::cout << "Recovered original cursor tables for "
              << executableGameData.sceneCount() << " scenes from FGP.exe\n";

    fs::path languagePath = packagePath(gameDirectory, "Language3", "common");
    if (!fs::exists(languagePath)) {
        languagePath = packagePath(gameDirectory, "Language", "common");
    }
    Package commonLanguage(languagePath);
    language = LanguageTable(
        commonLanguage.data(commonLanguage.find("language.ini")));
    std::cout << "Loaded localized interface strings from " << languagePath << '\n';

    const auto &animationEntry = menu.find("animation\\specific.smk");
    const auto &musicEntry = common.find("music\\theme.smk");
    std::cout << "Decoding " << animationEntry.name << " and " << musicEntry.name
              << " in-process with the linked FFmpeg libraries\n";
    // Every menu clip's descriptor (0x004abe20-0x004ac558) sets +0x48, so
    // scene_DrawAnim keys it on its top-left pixel and draws index 0.
    const auto decodeMenuClip = [&](std::string_view name,
                                    std::span<const std::uint8_t> bytes) {
        return decodeSmacker(bytes, true, false, false, 0, nullptr,
                             SmackerIndexZeroAlpha::Transparent,
                             sceneClipKeying("menu", name));
    };
    animation = decodeMenuClip("specific", menu.data(animationEntry));
    music = decodeSmacker(common.data(musicEntry), false, true);
    std::cout << "Decoded " << animation.videoFrames.size() << " animation frames and "
              << music.audioSamples.size() / 2 << " stereo audio frames\n";

    // These are the five animations explicitly started by the original
    // menu initialization routine, in its insertion order. Birds and the
    // ammeter clips are event-triggered and must not loop continuously.
    const std::array<std::tuple<const char *, int, int, bool>, 5> menuClipDefinitions{{
        {"wind", 304, 105, false},
        {"radar", 508, 177, false},
        {"wheel_1", 407, 325, false},
        {"wheel_2", 571, 263, false},
        {"idle", 443, 233, true},
    }};
    for (const auto &[name, x, y, idle] : menuClipDefinitions) {
        const std::string asset = "animation\\" + std::string(name) + ".smk";
        const auto &entry = menu.find(asset);
        menuClips.push_back(
            {name, x, y, idle, decodeMenuClip(name, menu.data(entry)), {}});
    }
    std::cout << "Decoded " << menuClips.size()
              << " additional menu animation layers\n";

    // FGP.exe starts these in response to pointer/click events. They are
    // deliberately separate from the five continuously running layers.
    const std::array<std::tuple<const char *, int, int, int>, 3>
        menuEffectDefinitions{{
            {"amp_1", 651, 441, 1},
            {"amp_2", 651, 441, 2},
            {"fuse", 675, 83, 0},
        }};
    for (const auto &[name, x, y, ampLevel] : menuEffectDefinitions) {
        const std::string asset = "animation\\" + std::string(name) + ".smk";
        menuEffects.push_back({name,
                               x,
                               y,
                               ampLevel,
                               decodeMenuClip(name, menu.data(menu.find(asset))),
                               {}});
    }

    for (std::size_t index = 0; index < cursors.size(); ++index) {
        const std::string asset =
            "cursors\\" + std::string(cursorNames[index]) + ".smk";
        // The cursor descriptor at 0x005544a8 is keyed (+0x48 = 1 at
        // 0x00412e13) and drawn last by scene_DrawAnim (0x00412f8e), so it
        // takes the top-left colour key like any keyed sprite: palette index
        // 0 is drawn. subgame.smk and character.smk fill their arrow badge
        // with index 0 (white), which the old rule cut out.
        cursors[index] = CursorAnimation{
            cursorNames[index],
            decodeSmacker(common.data(common.find(asset)), true, false, false,
                          0, nullptr, SmackerIndexZeroAlpha::Transparent,
                          SmackerKeying::TopLeftRgb565),
            {}};
    }
    std::cout << "Decoded " << cursors.size()
              << " original animated cursor types\n";

#if defined(__APPLE__)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#elif defined(_WIN32)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    // The window takes the program's own icon, resource 1 of
    // resources/windows/FGPNativePOC.rc.in, instead of the generic one.
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    // A run with no window is a scripted or test run: keep it silent too, so
    // headless runs on a developer's machine never play music nobody can see.
    // An explicit SDL_AUDIODRIVER still wins.
    if (const char *video = SDL_getenv("SDL_VIDEODRIVER");
        video && SDL_strcasecmp(video, "dummy") == 0 &&
        !SDL_getenv("SDL_AUDIODRIVER")) {
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    int windowWidth = 1000;
    int windowHeight = 750;
    if (const char *windowSize = std::getenv("FGP_WINDOW_SIZE")) {
        const std::string requested(windowSize);
        const auto separator = requested.find_first_of("xX");
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "FGP_WINDOW_SIZE must use WIDTHxHEIGHT");
        }
        windowWidth = std::stoi(requested.substr(0, separator));
        windowHeight = std::stoi(requested.substr(separator + 1));
        if (windowWidth <= 0 || windowHeight <= 0) {
            throw std::runtime_error(
                "FGP_WINDOW_SIZE dimensions must be positive");
        }
    }
    window = SDL_CreateWindow(
        "FGP Native POC — H hotspots · Space skip · M music · ↑/↓ volume · Esc quit",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        windowWidth,
        windowHeight,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    // Name entry and the recovered Text Invader both consume SDL_TEXTINPUT;
    // keep composition active for the lifetime of the application.
    SDL_StartTextInput();
    renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") +
                                 SDL_GetError());
    }
    SDL_RenderSetLogicalSize(renderer, kLogicalWidth, kLogicalHeight);

    // The original modal layer darkens the live scene with a one-pixel
    // checker mesh (0x00407160): an 800x600 surface whose pixels with an
    // even x + y are the green key and whose odd ones are COLORREF
    // 0x00402020, RGB (32,32,64). Every modal dialog shares it (0x00538594).
    // Build it once so dialogs can reuse it without per-frame draw calls.
    SurfacePtr modalMeshSurface(SDL_CreateRGBSurfaceWithFormat(
        0, kLogicalWidth, kLogicalHeight, 32, SDL_PIXELFORMAT_ARGB8888));
    if (!modalMeshSurface) {
        throw std::runtime_error(
            std::string("SDL_CreateRGBSurfaceWithFormat failed: ") +
            SDL_GetError());
    }
    if (SDL_LockSurface(modalMeshSurface.get()) != 0) {
        throw std::runtime_error(
            std::string("SDL_LockSurface failed: ") + SDL_GetError());
    }
    const Uint32 meshColour = SDL_MapRGBA(
        modalMeshSurface->format, 32, 32, 64, 255);
    const Uint32 meshClear = SDL_MapRGBA(
        modalMeshSurface->format, 0, 0, 0, 0);
    for (int y = 0; y < kLogicalHeight; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(
            static_cast<Uint8 *>(modalMeshSurface->pixels) +
            static_cast<std::size_t>(y) * modalMeshSurface->pitch);
        for (int x = 0; x < kLogicalWidth; ++x) {
            row[x] = ((x + y) & 1) != 0 ? meshColour : meshClear;
        }
    }
    SDL_UnlockSurface(modalMeshSurface.get());
    modalMeshTexture = makeTexture(renderer, modalMeshSurface.get());
    SDL_SetTextureBlendMode(modalMeshTexture.get(), SDL_BLENDMODE_BLEND);

    SDL_RendererInfo rendererInfo{};
    SDL_GetRendererInfo(renderer, &rendererInfo);
    std::cout << "SDL renderer: " << (rendererInfo.name ? rendererInfo.name : "unknown")
              << '\n';

    backdrop = loadBmp(menu.data(menu.find("backdrop.bmp")));
    hotspots = loadBmp(menu.data(menu.find("hotspots.bmp")));
    title = loadGreenKeyedBmp(menu.data(menu.find("bitmap\\title.bmp")));
    backdropTexture = makeTexture(renderer, backdrop.get());
    hotspotTexture = makeTexture(renderer, hotspots.get());
    titleTexture = makeTexture(renderer, title.get());
    SDL_SetTextureBlendMode(titleTexture.get(), SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(hotspotTexture.get(), SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(hotspotTexture.get(), 115);

    const auto loadMenuCornerButton =
        [&](std::size_t index, SceneCornerButtonKind kind,
            std::string_view stem, int x) {
            SurfacePtr normal = loadGreenKeyedBmp(menu.data(menu.find(
                "bitmap\\" + std::string(stem) + "_0.bmp")));
            SurfacePtr pressed = loadGreenKeyedBmp(menu.data(menu.find(
                "bitmap\\" + std::string(stem) + "_1.bmp")));
            TexturePtr normalTexture = makeTexture(renderer, normal.get());
            TexturePtr pressedTexture = makeTexture(renderer, pressed.get());
            SDL_SetTextureBlendMode(normalTexture.get(), SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(pressedTexture.get(), SDL_BLENDMODE_BLEND);
            menuCornerButtons[index] = {
                kind, SDL_Rect{x, 0, normal->w, normal->h},
                std::move(normal), std::move(normalTexture),
                std::move(pressedTexture)};
        };
    loadMenuCornerButton(0, SceneCornerButtonKind::Help, "help", 0);
    loadMenuCornerButton(1, SceneCornerButtonKind::Exit, "exit",
                         kLogicalWidth - 72);

    Package &settingsAssets = editionCommon ? *editionCommon : common;
    hasSubtitleSettings = editionCommon != nullptr;
    const std::string settingsAssetRoot = hasSubtitleSettings
                                              ? "dialogue\\subtitledlg\\"
                                              : "dialogue\\sounddlg\\";
    auto loadSettingsSurface = [&](std::string_view name) {
        return loadGreenKeyedBmp(settingsAssets.data(
            settingsAssets.find(settingsAssetRoot + std::string(name))));
    };
    settingsBackdrop = loadSettingsSurface("backdrop.bmp");
    settingsButtonNormal = loadSettingsSurface("button_0.bmp");
    settingsButtonPressed = loadSettingsSurface("button_1.bmp");
    settingsDownNormal = loadSettingsSurface("down_0.bmp");
    settingsDownPressed = loadSettingsSurface("down_1.bmp");
    settingsUpNormal = loadSettingsSurface("up_0.bmp");
    settingsUpPressed = loadSettingsSurface("up_1.bmp");
    settingsThumb = loadSettingsSurface("thumb.bmp");
    if (hasSubtitleSettings) {
        settingsToggleNormal = loadSettingsSurface("toggle_0.bmp");
        settingsTogglePressed = loadSettingsSurface("toggle_1.bmp");
    }
    settingsBackdropTexture =
        makeTexture(renderer, settingsBackdrop.get());
    settingsButtonNormalTexture =
        makeTexture(renderer, settingsButtonNormal.get());
    settingsButtonPressedTexture =
        makeTexture(renderer, settingsButtonPressed.get());
    settingsDownNormalTexture =
        makeTexture(renderer, settingsDownNormal.get());
    settingsDownPressedTexture =
        makeTexture(renderer, settingsDownPressed.get());
    settingsUpNormalTexture =
        makeTexture(renderer, settingsUpNormal.get());
    settingsUpPressedTexture =
        makeTexture(renderer, settingsUpPressed.get());
    settingsThumbTexture = makeTexture(renderer, settingsThumb.get());
    if (hasSubtitleSettings) {
        settingsToggleNormalTexture =
            makeTexture(renderer, settingsToggleNormal.get());
        settingsTogglePressedTexture =
            makeTexture(renderer, settingsTogglePressed.get());
    }

    nameDialogBackdrop = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\name\\backdrop.bmp")));
    nameDialogHotspots = loadBmp(
        common.data(common.find("dialogue\\name\\hotspot.bmp")));
    nameDialogOk = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\name\\ok_1.bmp")));
    nameDialogCancel = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\name\\cancel_1.bmp")));
    nameDialogBackdropTexture =
        makeTexture(renderer, nameDialogBackdrop.get());
    nameDialogOkTexture = makeTexture(renderer, nameDialogOk.get());
    nameDialogCancelTexture = makeTexture(renderer, nameDialogCancel.get());

    calendarBackdrop = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\calendar\\backdrop.bmp")));
    calendarHotspots = loadBmp(
        common.data(common.find("dialogue\\calendar\\hotspot.bmp")));
    calendarMark = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\calendar\\mark.bmp")));
    calendarPrevious = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\calendar\\prev_1.bmp")));
    calendarNext = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\calendar\\next_1.bmp")));
    calendarOk = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\calendar\\ok_1.bmp")));
    calendarBackdropTexture = makeTexture(renderer, calendarBackdrop.get());
    calendarMarkTexture = makeTexture(renderer, calendarMark.get());
    calendarPreviousTexture = makeTexture(renderer, calendarPrevious.get());
    calendarNextTexture = makeTexture(renderer, calendarNext.get());
    calendarOkTexture = makeTexture(renderer, calendarOk.get());

    messageBackdrop = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\msgbox\\backdrop.bmp")));
    messageButtonNormal = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\msgbox\\button_0.bmp")));
    messageButtonPressed = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\msgbox\\button_1.bmp")));
    messageBackdropTexture = makeTexture(renderer, messageBackdrop.get());
    messageButtonNormalTexture =
        makeTexture(renderer, messageButtonNormal.get());
    messageButtonPressedTexture =
        makeTexture(renderer, messageButtonPressed.get());

    awardBackdrop = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\awarddlg\\backdrop.bmp")));
    awardButtonNormal = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\awarddlg\\button_0.bmp")));
    awardButtonPressed = loadGreenKeyedBmp(
        common.data(common.find("dialogue\\awarddlg\\button_1.bmp")));
    awardBackdropTexture = makeTexture(renderer, awardBackdrop.get());
    awardButtonNormalTexture =
        makeTexture(renderer, awardButtonNormal.get());
    awardButtonPressedTexture =
        makeTexture(renderer, awardButtonPressed.get());

    const std::string highScoreRoot = "dialogue\\highscore\\";
    highScoreBackdrop = loadGreenKeyedBmp(
        common.data(common.find(highScoreRoot + "backdrop.bmp")));
    highScoreSelectedTabs[0] = loadGreenKeyedBmp(
        common.data(common.find(highScoreRoot + "profile_1.bmp")));
    highScoreSelectedTabs[1] = loadGreenKeyedBmp(
        common.data(common.find(highScoreRoot + "global_1.bmp")));
    highScoreSelectedTabs[2] = loadGreenKeyedBmp(
        common.data(common.find(highScoreRoot + "union_1.bmp")));
    highScoreClosePressed = loadGreenKeyedBmp(
        common.data(common.find(highScoreRoot + "close_1.bmp")));
    highScoreBackdropTexture =
        makeTexture(renderer, highScoreBackdrop.get());
    for (std::size_t index = 0; index < highScoreSelectedTabs.size(); ++index) {
        highScoreSelectedTabTextures[index] =
            makeTexture(renderer, highScoreSelectedTabs[index].get());
    }
    highScoreClosePressedTexture =
        makeTexture(renderer, highScoreClosePressed.get());

    const std::string navigationRoot = "dialogue\\navigate\\";
    storyNavigationBackdrop = loadGreenKeyedBmp(
        common.data(common.find(navigationRoot + "backdrop.bmp")));
    storyNavigationBackdropTexture =
        makeTexture(renderer, storyNavigationBackdrop.get());
    SDL_SetTextureBlendMode(storyNavigationBackdropTexture.get(),
                            SDL_BLENDMODE_BLEND);
    auto loadNavigationButton =
        [&](std::size_t index, StoryNavigationControl control,
            std::string_view stem, SDL_Rect bounds) {
            auto loadState = [&](int state) {
                return loadGreenKeyedBmp(common.data(common.find(
                    navigationRoot + std::string(stem) + "_" +
                    std::to_string(state) + ".bmp")));
            };
            SurfacePtr normal = loadState(0);
            SurfacePtr pressed = loadState(1);
            SurfacePtr disabled = loadState(2);
            TexturePtr normalTexture = makeTexture(renderer, normal.get());
            TexturePtr pressedTexture = makeTexture(renderer, pressed.get());
            TexturePtr disabledTexture = makeTexture(renderer, disabled.get());
            SDL_SetTextureBlendMode(normalTexture.get(), SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(pressedTexture.get(), SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(disabledTexture.get(), SDL_BLENDMODE_BLEND);
            storyNavigationButtons[index] = {
                control, bounds, std::move(normal), std::move(normalTexture),
                std::move(pressedTexture), std::move(disabledTexture)};
        };
    // Coordinates and insertion order are from 0x004215b0. Help and exit
    // intentionally render over the inner edges of the large arrows.
    loadNavigationButton(0, StoryNavigationControl::Next, "next",
                         SDL_Rect{91, 27, 69, 68});
    loadNavigationButton(1, StoryNavigationControl::Previous, "prev",
                         SDL_Rect{4, 25, 71, 70});
    loadNavigationButton(2, StoryNavigationControl::Help, "help",
                         SDL_Rect{59, 5, 47, 45});
    loadNavigationButton(3, StoryNavigationControl::Exit, "exit",
                         SDL_Rect{59, 71, 47, 45});

    auto loadHelpSurface = [&](std::string_view name) {
        return loadGreenKeyedBmp(common.data(common.find(
            "dialogue\\help\\" + std::string(name))));
    };
    helpBackdrop = loadHelpSurface("backdrop.bmp");
    helpButtonNormal = loadHelpSurface("button_0.bmp");
    helpButtonPressed = loadHelpSurface("button_1.bmp");
    helpPreviousNormal = loadHelpSurface("prev_0.bmp");
    helpPreviousPressed = loadHelpSurface("prev_1.bmp");
    helpNextNormal = loadHelpSurface("next_0.bmp");
    helpNextPressed = loadHelpSurface("next_1.bmp");
    helpUpNormal = loadHelpSurface("up_0.bmp");
    helpUpPressed = loadHelpSurface("up_1.bmp");
    helpDownNormal = loadHelpSurface("down_0.bmp");
    helpDownPressed = loadHelpSurface("down_1.bmp");
    helpThumb = loadHelpSurface("thumb.bmp");
    helpBackdropTexture = makeTexture(renderer, helpBackdrop.get());
    helpButtonNormalTexture = makeTexture(renderer, helpButtonNormal.get());
    helpButtonPressedTexture = makeTexture(renderer, helpButtonPressed.get());
    helpPreviousNormalTexture = makeTexture(renderer, helpPreviousNormal.get());
    helpPreviousPressedTexture = makeTexture(renderer, helpPreviousPressed.get());
    helpNextNormalTexture = makeTexture(renderer, helpNextNormal.get());
    helpNextPressedTexture = makeTexture(renderer, helpNextPressed.get());
    helpUpNormalTexture = makeTexture(renderer, helpUpNormal.get());
    helpUpPressedTexture = makeTexture(renderer, helpUpPressed.get());
    helpDownNormalTexture = makeTexture(renderer, helpDownNormal.get());
    helpDownPressedTexture = makeTexture(renderer, helpDownPressed.get());
    helpThumbTexture = makeTexture(renderer, helpThumb.get());

    dialogFont = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Amerigo BT_10_.bmp")));
    calendarFont = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Amerigo BT_14_.bmp")));
    menuTitleFont = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Amerigo BT_18_.bmp")));
    // 0x00420f40 assigns Comic Sans MS_10_ to the message body object at
    // 0x005ac9c0 and Amerigo BT_14_ to the title object at 0x005ad9f8.
    messageFont = loadBitmapFont(
        renderer, common.data(common.find("fonts\\Comic Sans MS_10_.bmp")));

    animationWidth = 0;
    animationHeight = 0;
    if (!animation.videoFrames.empty()) {
        animationWidth = animation.videoFrames.front().width;
        animationHeight = animation.videoFrames.front().height;
        animationTexture.reset(SDL_CreateTexture(renderer,
                                                 SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 animationWidth,
                                                 animationHeight));
        if (!animationTexture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                     SDL_GetError());
        }
        SDL_SetTextureBlendMode(animationTexture.get(), SDL_BLENDMODE_BLEND);
    }
    for (auto &clip : menuClips) {
        if (clip.media.videoFrames.empty()) {
            continue;
        }
        const auto &frame = clip.media.videoFrames.front();
        clip.texture.reset(SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             frame.width,
                                             frame.height));
        if (!clip.texture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                     SDL_GetError());
        }
        SDL_SetTextureBlendMode(clip.texture.get(), SDL_BLENDMODE_BLEND);
    }
    for (auto &effect : menuEffects) {
        if (effect.media.videoFrames.empty()) {
            continue;
        }
        const auto &frame = effect.media.videoFrames.front();
        effect.texture.reset(SDL_CreateTexture(renderer,
                                                SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                frame.width,
                                                frame.height));
        if (!effect.texture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                     SDL_GetError());
        }
        SDL_SetTextureBlendMode(effect.texture.get(), SDL_BLENDMODE_BLEND);
    }
    for (auto &cursor : cursors) {
        if (cursor.media.videoFrames.empty()) {
            continue;
        }
        const auto &frame = cursor.media.videoFrames.front();
        cursor.texture.reset(SDL_CreateTexture(renderer,
                                                SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                frame.width,
                                                frame.height));
        if (!cursor.texture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                     SDL_GetError());
        }
        SDL_SetTextureBlendMode(cursor.texture.get(), SDL_BLENDMODE_BLEND);
    }
    SDL_ShowCursor(SDL_DISABLE);

    SDL_AudioSpec wanted{};
    wanted.freq = music.audioRate;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = static_cast<Uint8>(music.audioChannels);
    wanted.samples = 2048;
    wanted.callback = &LiveAudioMixer::callback;
    wanted.userdata = &audioMixer;
    SDL_AudioSpec obtained{};
    audioDevice =
        SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
    if (audioDevice == 0) {
        throw std::runtime_error(std::string("SDL_OpenAudioDevice failed: ") +
                                 SDL_GetError());
    }
    std::cout << "Native SDL audio: " << obtained.freq << " Hz, "
              << static_cast<int>(obtained.channels) << " channels, buffer "
              << obtained.samples << " samples, driver "
              << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none")
              << '\n';
    audioMixer.attach(audioDevice, obtained.freq, obtained.channels);
    if (const char *dumpPath = std::getenv("FGP_AUDIO_DUMP")) {
        audioDumpActive = audioMixer.startDump(dumpPath);
        std::cout << (audioDumpActive ? "Writing mixer output to "
                                      : "Could not open audio dump ")
                  << dumpPath << '\n';
    }

    // Packages read entries on demand, so this is what startup actually
    // pulled from disk (and what a browser build would have to download
    // before the first scene).
    std::size_t sharedBytesRead = menu.residentBytes() + common.residentBytes();
    for (const auto *package : {editionCommon.get(), data2Common.get(),
                                addonCommon.get()}) {
        if (package) {
            sharedBytesRead += package->residentBytes();
        }
    }
    std::cout << "Read " << sharedBytesRead / 1024 << " KiB of shared package "
              << "data at startup\n";

    // FGP.exe stores the channels as effect, speech, music and initializes
    // all three to 0.75 in its default option table.
    audioVolumes = std::array<float, 3>{0.75F, 0.75F, 0.75F};
    musicEnabled = true;
    // Startup decides whether to play the logo clip or take the explicit
    // development bypass. Keep the mixer silent until that decision.
    SDL_PauseAudioDevice(audioDevice, 1);

    running = true;
    paused = false;
    showHotspots = std::getenv("FGP_SHOW_HOTSPOTS") != nullptr;
    currentFrame = 0;
    pausedAt = 0.0;
    start = std::chrono::steady_clock::now();

    viewMode = ViewMode::Menu;
    dialogMode = DialogMode::None;
    optionsPressedControl = OptionsControl::None;
    optionsDraggingChannel = -1;
    savedAudioVolumes = audioVolumes;
    subtitleLanguage = 2;
    savedSubtitleLanguage = subtitleLanguage;
    subtitlesEnabled = false;
    savedSubtitlesEnabled = subtitlesEnabled;
    activeScene = "menu";
    sceneTitleWidth = 0;
    sceneTitleHeight = 0;
    sceneUsesAudioTimeline = false;
    sceneSequenceBlocksInput = false;
    sceneSequenceFinished = false;
    sceneClipIndex = 0;
    sceneClipTimelineStart = 0.0;
    sceneClipStartOffsets.clear();
    sceneSequenceRequestedAt = 0.0;
    sceneClipStarted = false;
    sceneSequenceHeldConflictMask = 0;
    randomEngine = std::mt19937(std::random_device{}());
    crtRandom.state = static_cast<std::uint32_t>(std::time(nullptr));

    yardHotspot99Variant = 0;
    yardConstructionFlags = 0;
    yardHotspot58BaseVariant = 0;
    yardHotspot58CompleteVariant = 0;

    peeMagnetActive = false;
    activeAmpLevel = -1;
    ampEffectStart = 0.0;
    fuseActive = false;
    fuseEffectStart = 0.0;
    hoveredHotspot = 0;
    mouseX = kLogicalWidth / 2;
    mouseY = kLogicalHeight / 2;
    mouseInsideWindow = true;
    activeCursor = CursorKind::Default;
    cursorAnimationStart = std::chrono::steady_clock::now();
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    pendingBirthMonth = 0;
    pendingBirthDay = 1;
    dialogMessageKind = MessageKind::Notice;
    awardIconWidth = 0;
    awardIconHeight = 0;
    awardMedalWidth = 0;
    awardMedalHeight = 0;

    if (std::optional<fs::path> requested = environmentPath("FGP_PROFILE_DIR")) {
        profileDirectory = *requested;
    } else {
        profileDirectory = chooseProfileDirectory(gameDirectory);
        if (profileDirectory != gameDirectory / "Profiles") {
            std::cout << "The game folder cannot be written to; profiles are in "
                      << profileDirectory << '\n';
        }
    }
    if (fs::exists(profileDirectory)) {
        for (const auto &item : fs::directory_iterator(profileDirectory)) {
            if (item.is_regular_file() && item.path().extension() == ".pfl") {
                profiles.push_back(item.path().stem().string());
            }
        }
        std::sort(profiles.begin(), profiles.end());
    }
    if (profiles.empty()) {
        profiles.push_back("No profile");
    }
    profileIndex = 0;
    // 0x0040e801 -> 0x0040e830 selects a profile at startup through
    // 0x0040d860, which reads its options (nothing plays yet).
    loadProfileOptions(false);
    developerRunStart = std::chrono::steady_clock::now();
    applicationClockStart = developerRunStart;

    const char *startView = std::getenv("FGP_START_VIEW");
    const std::string requestedStartView =
        startView ? normalizedPath(startView) : std::string("logo");
    // Entry mode decides whether a scene plays its opening at all, so it has
    // to be reachable without walking the story to the transition that
    // supplies it. Only `pee` ever hands the desk mode 2, for instance, and
    // only the movie player hands tvstation mode 1.
    int startEntryMode = 0;
    if (const char *mode = std::getenv("FGP_START_ENTRY_MODE")) {
        startEntryMode = std::atoi(mode);
    }
    if (requestedStartView == "menu" || requestedStartView == "options") {
        sceneAudioGains = readSceneAudioGains(menu);
        audioMixer.setBackground(music, audioVolumes[2] * sceneAudioGains[2]);
        SDL_PauseAudioDevice(audioDevice, 0);
        setMenuTitle();
        referenceTrace.emit("scene_change", {{"old_scene", ""},
                                              {"scene", "menu"},
                                              {"entry_mode", 0}});
        sceneReadyAfterPresent = "menu";
        if (requestedStartView == "options") {
            enterOptions();
        }
    } else {
        // Startup scenes precede the menu and therefore have no synthetic
        // menu predecessor in the trace or navigation history.
        activeScene.clear();
        loadScene(requestedStartView, requestedStartView, startEntryMode);
    }
    if (const char *startDialog = std::getenv("FGP_START_DIALOG")) {
        const std::string requested = normalizedPath(startDialog);
        if (requested == "name") {
            openNameDialog();
        } else if (requested == "award-carpart") {
            showCarPartAward(1);
        } else if (requested == "award-anaglyph") {
            showAnaglyphAward();
        }
    }
}

} // namespace fgp
