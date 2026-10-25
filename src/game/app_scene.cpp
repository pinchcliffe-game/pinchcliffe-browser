#include "game/app.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "assets/help_table.h"
#include "game/menu_names.h"
#include "game/minigame_catalog.h"
#include "generated/scene_escape.hpp"
#include "generated/scene_navigations.hpp"
#include "generated/scene_logic.hpp"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

namespace {

// scene_DrawAnim keys a descriptor only when its +0x48 flag is set. Unkeyed
// descriptors keep the native policy: the original blits them without a key,
// but that has not been checked against a capture.
SmackerKeying sceneKeying(
    const ExecutableGameData::AnimationDefinition &definition) {
    return definition.keyed ? SmackerKeying::TopLeftRgb565
                            : SmackerKeying::IndexZeroAndGreen;
}

// Which character groups a scene starts for a given entry mode.
//
// Every story scene's entry initializer ends in the same three-part shape:
// read the entry mode out of the EVENT_BeginScene payload, then take one arm
// per mode. The seed-everything helper 0x0040a880 is the common case, and a
// scene that wants fewer groups during its opening instead issues one
// 0x0040a790(groupBit, kind) call per group it does want. Binding still leaves
// every group active in the shared scheduler: an unseeded group can observe a
// held conflict and begin at the boundary that releases it. So only the arms
// that deviate from "seed everything" need recording here; any scene or mode
// missing from this table initially activates every bound group, which is
// exactly what 0x0040a880 does.
//
// `seededBits` is a mask of group conflict bits, matching the bit each group
// is bound with, because 0x0040a790 takes the group's bit value rather than
// its index.
struct SceneEntrySeeding {
    std::string_view scene;
    int entryMode;
    std::uint32_t seededBits;
};

constexpr std::array<SceneEntrySeeding, 11> kSceneEntrySeeding{{
    // 0x00428169 dispatches the yard. Mode 0 (0x00428186) calls only the
    // chain selector 0x00428350 -- no 0x0040a790 at all -- so neither group is
    // initially activated. They remain bound, though, and the shared scheduler
    // starts each at the first boundary that releases its conflict bit. Mode 1
    // (0x00428175) seeds everything immediately.
    {"yard", 0, 0x00000000U},
    // Mode 2 and above fall through to 0x0042818b, which neither seeds nor
    // opens: the caller is expected to have left the groups running.
    {"yard", 2, 0x00000000U},
    // 0x00424842 dispatches the desk. Mode 0 falls through to 0x00424858,
    // which pushes group bit 4 (with kind 1 shared at 0x00424854) before
    // starting the full opening table 0x004b2770 -- so exactly one of desk's
    // three groups is present while the opening plays.
    {"desk", 0, 0x00000004U},
    // Mode 2 (0x0042486e) is the return from `pee`: it starts group bits 1
    // and 4, deliberately leaving bit 2 unseeded, then plays only the single
    // clip S02AN-SS-007b via 0x0040a770.
    {"desk", 2, 0x00000001U | 0x00000004U},
    // 0x00425896 dispatches intent as a two-way test. Mode 0 starts group
    // bit 0x400 (0x004258a0) and plays the single clip S10AN-SS-001; any
    // other mode jumps to 0x004258b9 and seeds everything.
    {"intent", 0, 0x00000400U},
    // 0x004261e8 dispatches outtent the same way: mode 0 creates the `paper`
    // collectible, starts group bit 0x10 (0x00426204) and runs the opening
    // table 0x004bfb68, leaving its other group (bit 1) unseeded.
    {"outtent", 0, 0x00000010U},
    // Mode 0 of these initializers starts the opening and seeds nothing; any
    // other mode calls 0x0040a880. Garage 0x00424f34 (S06AN-SS-001 through
    // 0x0040a770, else 0x00424f45), house 0x00425343 (else 0x0042537c),
    // tvroom 0x00427344 (table through 0x0040a6f0, else 0x00427355),
    // tvstation 0x00427b82 (else 0x00427b9d) and pee 0x00426805 (else
    // 0x00426816). Their groups wait for the opening's release or for Space,
    // which seeds them on the first base entry (run
    // 20261020-175032-1dce8348-4d8a: garage's groups start SOL-B01, ROD-B01
    // and LUD-B01 at Space).
    {"garage", 0, 0x00000000U},
    {"house", 0, 0x00000000U},
    {"tvroom", 0, 0x00000000U},
    {"tvstation", 0, 0x00000000U},
    {"pee", 0, 0x00000000U},
}};

const SceneEntrySeeding *sceneEntrySeeding(std::string_view scene,
                                          int entryMode) {
    for (const auto &row : kSceneEntrySeeding) {
        if (row.scene == scene && row.entryMode == entryMode) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

void GameApp::loadScene(const std::string &sceneName, std::string_view label,
                        int entryMode) {
    const auto loadStarted = std::chrono::steady_clock::now();
    const std::string previousScene = activeScene;
    invalidateUploadedVideoFrames();
    // Leaving a scene ends its narration, playing or merely armed. EndScene
    // (event 0x104, 0x0040c930) clears every timer slot through 0x0040c520,
    // and the clip belongs to the scene's own packages. Without this a
    // narration started in the desk keeps playing over house or the menu.
    stopSceneNarration();
    stopSceneAmbience();
    // GAME_EnterScene (0x0040ccd0) leaves the old scene before it loads the
    // new one: 0x0040cd50 calls 0x00412eb0, which removes every animation
    // (0x00412aa0 on each), so every clip's sound stops here. Before that,
    // a scene whose record has the Loading box flag pauses the music
    // (0x0040cceb -> 0x0040cb20 -> 0x0040b6f0); otherwise it plays on
    // through the load. Without this the old scene's effects, looping ones
    // included, kept playing until the new scene's audio replaced them.
    if (executableGameData.sceneShowsLoadingBox(sceneName)) {
        audioMixer.holdBackground();
    }
    audioMixer.clearForeground();
    activeSceneEntryMode = entryMode;
    int primaryDataLayer = -1;
    fs::path primaryDataPath;
    for (int layer = 0; layer <= 3; ++layer) {
        const std::string layerName =
            layer == 0 ? "Data" : "Data" + std::to_string(layer);
        const fs::path candidate =
            packagePath(gameDirectory, layerName, sceneName);
        if (fs::exists(candidate)) {
            primaryDataLayer = layer;
            primaryDataPath = candidate;
            break;
        }
    }
    if (primaryDataLayer < 0) {
        throw std::runtime_error(
            "Could not find a data package for scene: " + sceneName);
    }
    auto nextPackage = std::make_unique<Package>(primaryDataPath);
    std::unique_ptr<Package> nextEditionPackage;
    const fs::path editionScenePath =
        packagePath(gameDirectory, "Data1", sceneName);
    if (primaryDataLayer != 1 && fs::exists(editionScenePath)) {
        nextEditionPackage = std::make_unique<Package>(editionScenePath);
    }
    std::unique_ptr<Package> nextData2Package;
    const fs::path data2ScenePath =
        packagePath(gameDirectory, "Data2", sceneName);
    if (primaryDataLayer != 2 && fs::exists(data2ScenePath)) {
        nextData2Package = std::make_unique<Package>(data2ScenePath);
    }
    std::unique_ptr<Package> nextLanguagePackage;
    fs::path languageScenePath;
    if (primaryDataLayer >= 2) {
        languageScenePath = packagePath(
            gameDirectory, "Language" + std::to_string(primaryDataLayer),
            sceneName);
    } else {
        languageScenePath =
            packagePath(gameDirectory, "Language1", sceneName);
    }
    if (languageScenePath.empty() || !fs::exists(languageScenePath)) {
        languageScenePath = packagePath(gameDirectory, "Language", sceneName);
    }
    if (fs::exists(languageScenePath)) {
        nextLanguagePackage = std::make_unique<Package>(languageScenePath);
    }
    std::unique_ptr<Package> nextAddonPackage;
    const fs::path addonScenePath =
        packagePath(gameDirectory, "Data3", sceneName);
    if (primaryDataLayer != 3 && fs::exists(addonScenePath)) {
        nextAddonPackage = std::make_unique<Package>(addonScenePath);
    }
    Package *nextVisualPackage = nextPackage.get();
    if (nextEditionPackage &&
        nextEditionPackage->findOptional("backdrop.bmp")) {
        nextVisualPackage = nextEditionPackage.get();
    }
    if (nextData2Package &&
        nextData2Package->findOptional("backdrop.bmp")) {
        nextVisualPackage = nextData2Package.get();
    }
    if (nextAddonPackage && nextAddonPackage->findOptional("backdrop.bmp")) {
        nextVisualPackage = nextAddonPackage.get();
    }
    const auto &nextBackdropEntry = nextVisualPackage->find("backdrop.bmp");
    SurfacePtr nextBackdrop =
        loadBmp(nextVisualPackage->data(nextBackdropEntry));
    TexturePtr nextBackdropTexture = makeTexture(renderer, nextBackdrop.get());
    std::vector<SceneClip> nextSceneClips;
    std::vector<SceneCharacterGroup> nextSceneCharacterGroups;
    std::vector<SceneAnimationObject> nextSceneAnimationObjects;
    std::optional<PeeParticleField> nextPeeParticleField;
    std::vector<SceneBitmapLayer> nextSceneBitmapLayers;
    std::vector<SceneCornerButton> nextSceneCornerButtons;
    std::vector<SceneCollectible> nextSceneCollectibles;
    std::optional<BuildacarWorkshop> nextBuildacarWorkshop;
    DecodedMedia nextSceneAudioTimeline;
    DecodedMedia nextSceneEffectTimeline;
    DecodedMedia nextSceneBackgroundMusic;
    std::optional<DecodedMedia> nextTvroomConcurrentClip;
    std::array<float, 3> nextSceneAudioGains{1.0F, 1.0F, 1.0F};
    std::string nextSceneBackgroundMusicAsset;
    bool nextSceneUsesAudioTimeline = false;
    bool nextSceneSequenceBlocksInput = false;
    std::optional<std::pair<double, std::string>> nextSceneTimedTransition;

    const auto commonAssetData = [&](const std::string &asset)
        -> std::span<const std::uint8_t> {
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

    auto sceneAnimationData = [&](std::string descriptorName)
        -> std::span<const std::uint8_t> {
        const auto separator = descriptorName.find_last_of("\\/");
        if (separator != std::string::npos) {
            descriptorName.erase(0, separator + 1);
        }
        const std::string asset =
            "animation\\" + descriptorName + ".smk";
        const bool commonAsset =
            descriptorName.rfind("blank", 0) == 0;
        if (commonAsset) {
            return commonAssetData(asset);
        }
        if (nextLanguagePackage) {
            if (const auto *entry = nextLanguagePackage->findOptional(asset)) {
                return nextLanguagePackage->data(*entry);
            }
        }
        if (nextAddonPackage) {
            if (const auto *entry = nextAddonPackage->findOptional(asset)) {
                return nextAddonPackage->data(*entry);
            }
        }
        if (nextData2Package) {
            if (const auto *entry = nextData2Package->findOptional(asset)) {
                return nextData2Package->data(*entry);
            }
        }
        if (nextEditionPackage) {
            if (const auto *entry = nextEditionPackage->findOptional(asset)) {
                return nextEditionPackage->data(*entry);
            }
        }
        if (const auto *entry = nextPackage->findOptional(asset)) {
            return nextPackage->data(*entry);
        }
        throw std::runtime_error("Scene animation asset not found: " + asset);
    };

    // 0x00414f20: every profile whose birthday is today, in list order.
    const auto birthdayProfilesToday = [&]() {
        std::vector<std::string> matched;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        local = *std::localtime(&now);
#endif
        const int year = local.tm_year + 1900;
        const bool leap =
            (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        for (const auto &profile : profiles) {
            if (profile == "No profile") {
                continue;
            }
            const fs::path path = profileDirectory / (profile + ".pfl");
            try {
                const auto birthday = profileBirthday(path);
                if (birthday && birthday->zeroBasedMonth == local.tm_mon &&
                    (birthday->day == local.tm_mday ||
                     (!leap && birthday->zeroBasedMonth == 1 &&
                      birthday->day == 29 && local.tm_mday == 28))) {
                    matched.push_back(profile);
                }
            } catch (const std::exception &error) {
                std::cerr << "Could not read birthday from " << path
                          << ": " << error.what() << '\n';
            }
        }
        return matched;
    };
    std::vector<std::string> birthdayNames;
    if (sceneName == "birthday") birthdayNames = birthdayProfilesToday();
    const bool birthdayMatch = !birthdayNames.empty();

    nextSceneAudioGains = readSceneAudioGains(*nextPackage);

    // Scene music routes are generated from the original executable. Intro
    // remains event-driven because track1 begins part-way through its opening.
    if (sceneName != "intro" &&
        (sceneName != "birthday" || birthdayMatch)) {
        for (const auto &definition : fgp_generated::kSceneMusic) {
            if (definition.scene != sceneName) {
                continue;
            }
            nextSceneBackgroundMusicAsset = definition.asset;
            const std::string asset =
                "music\\" + nextSceneBackgroundMusicAsset + ".smk";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData(asset), false, true);
            break;
        }
        if (sceneName == "tvstation" && editionCommon) {
            nextSceneBackgroundMusicAsset = "ambience5";
            nextSceneBackgroundMusic = decodeSmacker(
                editionCommon->data(
                    editionCommon->find("music\\ambience5.smk")),
                false, true);
        }
        if (sceneName == "sockdrawer") {
            // 0x0044b80d routes this game to the shared subgame3 track.
            nextSceneBackgroundMusicAsset = "subgame3";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\subgame3.smk"), false, true);
        }
        if (sceneName == "bouquet") {
            // 0x0045e3a6 starts the shared activity2 track.
            nextSceneBackgroundMusicAsset = "activity2";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\activity2.smk"), false, true);
        }
        if (sceneName == "jigsaw" || sceneName == "sliding") {
            // 0x0046bce0 / 0x0046de3f: both puzzles start activity9.
            nextSceneBackgroundMusicAsset = "activity9";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\activity9.smk"), false, true);
        }
        if (sceneName == "activity") {
            // 0x0045c23b starts subgame21 (0x0040b1f0).
            nextSceneBackgroundMusicAsset = "subgame21";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\subgame21.smk"), false, true);
        }
        if (sceneName == "town") {
            // 0x00426fc7 starts subgame10 for every entry mode.
            nextSceneBackgroundMusicAsset = "subgame10";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\subgame10.smk"), false, true);
        }
        if (sceneName == "butterfly") {
            // 0x0043a737 routes this game to the shared subgame5 track.
            nextSceneBackgroundMusicAsset = "subgame5";
            nextSceneBackgroundMusic = decodeSmacker(
                commonAssetData("music\\subgame5.smk"), false, true);
        }
    }

    auto appendSceneNarration = [&](std::string_view name,
                                    double offsetSeconds) {
        DecodedMedia narration = decodeSmacker(
            sceneAnimationData(std::string(name)), false, true);
        const double duration =
            narration.audioRate > 0 && narration.audioChannels > 0
                ? narration.audioSamples.size() /
                      static_cast<double>(narration.audioRate *
                                          narration.audioChannels)
                : 0.0;
        mixAudioIntoAt(nextSceneAudioTimeline, std::move(narration),
                       offsetSeconds);
        nextSceneUsesAudioTimeline = true;
        return duration;
    };

    auto appendSceneDefinition =
        [&](const ExecutableGameData::AnimationDefinition &definition,
            double offsetSeconds) {
            const auto assetData = sceneAnimationData(definition.name);
            DecodedMedia secondaryAudio;
            DecodedMedia media = decodeSmackerStreamed(
                assetData, definition.name, sceneKeying(definition),
                true, true, 0, &secondaryAudio);
            const double duration = originalStoryClipCompletion(media);

            DecodedMedia primaryAudio;
            primaryAudio.audioRate = media.audioRate;
            primaryAudio.audioChannels = media.audioChannels;
            primaryAudio.audioSamples = std::move(media.audioSamples);
            if (secondaryAudio.audioSamples.empty()) {
                // A lone track 0 is effects audio; the player gives it
                // the effect volume (0x00403068).
                mixAudioIntoAt(media.smackerAudioTrack == 0
                                   ? nextSceneEffectTimeline
                                   : nextSceneAudioTimeline,
                               std::move(primaryAudio), offsetSeconds);
            } else {
                mixAudioIntoAt(nextSceneEffectTimeline,
                               std::move(primaryAudio), offsetSeconds);
                mixAudioIntoAt(nextSceneAudioTimeline,
                               std::move(secondaryAudio), offsetSeconds);
            }

            TexturePtr texture;
            if (definition.visible && !media.videoFrames.empty()) {
                const auto &frame = media.videoFrames.metadata(0U);
                texture.reset(SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
                if (!texture) {
                    throw std::runtime_error(
                        std::string("SDL_CreateTexture failed: ") +
                        SDL_GetError());
                }
                SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
            }
            nextSceneClips.push_back({definition.name,
                                      definition.x,
                                      definition.y,
                                      false,
                                      std::move(media),
                                      duration,
                                      std::move(texture),
                                      definition.visible,
                                      definition.layer,
                                      definition.conflictMask});
            nextSceneUsesAudioTimeline = true;
            return duration;
        };

    if (sceneName == "logo") {
        // 0x00416730 plays descriptor 0x004ab8d8 through 0x00412b90; its
        // +0x48 is set, so index 0 (199,199,199) is drawn.
        DecodedMedia media = decodeSmackerStreamed(
            sceneAnimationData("credits"), "credits",
            sceneKeying(executableGameData.animationAtRva(0x000ab8d8U)),
            true);
        const double duration = media.videoDuration;
        TexturePtr texture;
        if (!media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.metadata(0U);
            texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!texture) {
                throw std::runtime_error(
                    std::string("SDL_CreateTexture failed: ") +
                    SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        }
        nextSceneClips.push_back({"credits", 104, 55, false,
                                  std::move(media), duration,
                                  std::move(texture), true, 0, 0U});
        nextSceneSequenceBlocksInput = true;
        std::cout << "Recovered pre-menu credits sequence\n";
    } else if (sceneName == "birthday" && !birthdayMatch) {
        // FGP.exe always enters the birthday callback after credits. With no
        // matching profile it immediately dispatches the ordinary menu.
        nextSceneTimedTransition =
            std::pair{0.0, std::string("menu")};
    }

    const auto decodeNextSceneCharacterTable =
        [&](std::uint32_t tableRva, std::uint32_t conflictMask) {
            std::vector<SceneClip> clips;
            if (tableRva == 0U) {
                return clips;
            }
            for (const auto &definition :
                 executableGameData.animationSequenceAtRva(tableRva)) {
                DecodedMedia media = decodeSmackerStreamed(
                    sceneAnimationData(definition.name), definition.name,
                    sceneKeying(definition), false);
                TexturePtr texture;
                if (!media.videoFrames.empty()) {
                    const auto &frame = media.videoFrames.metadata(0U);
                    texture.reset(SDL_CreateTexture(
                        renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, frame.width,
                        frame.height));
                    if (!texture) {
                        throw std::runtime_error(
                            std::string("SDL_CreateTexture failed: ") +
                            SDL_GetError());
                    }
                    SDL_SetTextureBlendMode(texture.get(),
                                            SDL_BLENDMODE_BLEND);
                }
                const double duration =
                    originalStoryClipCompletion(media);
                clips.push_back({definition.name,
                                 definition.x,
                                 definition.y,
                                 false,
                                 std::move(media),
                                 duration,
                                 std::move(texture),
                                 definition.visible,
                                 definition.layer,
                                 conflictMask});
            }
            return clips;
        };
    auto addSceneCharacterGroup =
        [&](std::uint32_t hotspot, std::uint32_t conflictMask,
            std::uint32_t tableRva, std::uint32_t category2TableRva = 0U,
            std::uint32_t category3TableRva = 0U) {
        // 0x0040a470 installs the specials (category 2) and the click
        // reaction (category 3) with the base table. A scene that names only
        // the base table gets the other two from the generated registration
        // of that same base table.
        for (const auto &definition : fgp_generated::kSceneGroups) {
            if (definition.scene == sceneName &&
                definition.conflictMask == conflictMask &&
                definition.baseTableRva == tableRva) {
                if (category2TableRva == 0U) {
                    category2TableRva = definition.category2TableRva;
                }
                if (category3TableRva == 0U) {
                    category3TableRva = definition.category3TableRva;
                }
                break;
            }
        }
        SceneCharacterGroup group;
        group.hotspot = hotspot;
        group.conflictMask = conflictMask;
        group.baseTableRva = tableRva;
        group.category2TableRva = category2TableRva;
        group.category3TableRva = category3TableRva;
        group.baseClips =
            decodeNextSceneCharacterTable(tableRva, conflictMask);
        group.category2Clips =
            decodeNextSceneCharacterTable(category2TableRva, conflictMask);
        group.category3Clips =
            decodeNextSceneCharacterTable(category3TableRva, conflictMask);
        group.storedTables.push_back(
            {tableRva, category2TableRva, category3TableRva});
        nextSceneCharacterGroups.push_back(std::move(group));
    };
    const auto addSceneCharacterStoredTables =
        [&](std::uint32_t conflictMask, std::uint32_t tableRva,
            std::uint32_t category2TableRva,
            std::uint32_t category3TableRva) {
        const auto found = std::find_if(
            nextSceneCharacterGroups.begin(), nextSceneCharacterGroups.end(),
            [&](const SceneCharacterGroup &group) {
                return group.conflictMask == conflictMask;
            });
        if (found == nextSceneCharacterGroups.end()) {
            throw std::runtime_error(
                "Stored character tables have no bound group");
        }
        // Alternate sets are address-only until their recovered event fires.
        // Full-frame BGRA predecode here retained hundreds of megabytes for
        // clips that were not live.
        found->storedTables.push_back(
            {tableRva, category2TableRva, category3TableRva});
    };
    const auto addSceneCharacterCategory4Table =
        [&](std::uint32_t conflictMask, std::uint32_t tableRva) {
        const auto found = std::find_if(
            nextSceneCharacterGroups.begin(), nextSceneCharacterGroups.end(),
            [&](const SceneCharacterGroup &group) {
                return group.conflictMask == conflictMask;
            });
        if (found == nextSceneCharacterGroups.end()) {
            throw std::runtime_error(
                "Category-4 table has no bound character group");
        }
        // All currently recovered category-4 tables are single-entry
        // one-shots. Decode the selected one only when its event fires.
        found->category4Tables.push_back({tableRva, {}});
    };

    auto addSceneCollectible = [&](Package &package,
                                   const char *name,
                                   int x,
                                   int y,
                                   std::uint32_t hotspot,
                                   std::int32_t storyIndex,
                                   bool visible) {
        const std::string asset =
            "bitmap\\" + std::string(name) + ".bmp";
        SurfacePtr surface = loadGreenKeyedBmp(
            package.data(package.find(asset)));
        TexturePtr texture = makeTexture(renderer, surface.get());
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        nextSceneCollectibles.push_back(
            {name, x, y, hotspot, storyIndex, visible,
             std::move(surface), std::move(texture)});
    };

    auto addSceneAnimationObject =
        [&](std::uint32_t descriptorRva,
            int layer,
            bool hideOnComplete) {
            const auto definition =
                executableGameData.animationAtRva(descriptorRva);
            DecodedMedia media = decodeSmackerStreamed(
                sceneAnimationData(definition.name), definition.name,
                sceneKeying(definition), false);
            TexturePtr texture;
            if (!media.videoFrames.empty()) {
                const auto &frame = media.videoFrames.metadata(0U);
                texture.reset(SDL_CreateTexture(
                    renderer,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING,
                    frame.width,
                    frame.height));
                if (!texture) {
                    throw std::runtime_error(
                        std::string("SDL_CreateTexture failed: ") +
                        SDL_GetError());
                }
                SDL_SetTextureBlendMode(texture.get(),
                                        SDL_BLENDMODE_BLEND);
            }
            // +0x50 is the descriptor's dynamic hotspot field.
            const std::uint32_t hotspot =
                executableGameData.animationHotspotAtRva(descriptorRva);
            nextSceneAnimationObjects.push_back({
                definition.name,
                definition.x,
                definition.y,
                layer,
                hotspot,
                true,
                false,
                hideOnComplete,
                0.0,
                std::move(media),
                std::move(texture)});
        };

    auto addScenePlayer = [&](std::uint32_t descriptorRva, bool loop) {
        const auto definition =
            executableGameData.animationAtRva(descriptorRva);
        DecodedMedia audio;
        DecodedMedia media = decodeSmackerStreamed(
            sceneAnimationData(definition.name), definition.name,
            sceneKeying(definition), true, true);
        audio.audioRate = media.audioRate;
        audio.audioChannels = media.audioChannels;
        audio.audioSamples = std::move(media.audioSamples);
        TexturePtr texture;
        if (definition.visible && !media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.metadata(0U);
            texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!texture) {
                throw std::runtime_error(
                    std::string("SDL_CreateTexture failed: ") +
                    SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        }
        SceneAnimationObject object{
            definition.name,
            definition.x,
            definition.y,
            definition.layer,
            executableGameData.animationHotspotAtRva(descriptorRva),
            loop,
            loop,
            false,
            0.0,
            std::move(media),
            std::move(texture)};
        object.loop = loop;
        object.audio = std::move(audio);
        nextSceneAnimationObjects.push_back(std::move(object));
    };

    SurfacePtr nextHotspots;
    TexturePtr nextHotspotTexture;
    TexturePtr nextSceneTitleTexture;
    int nextSceneTitleWidth = 0;
    int nextSceneTitleHeight = 0;
    // Hotspot masks are independently overlaid by the data layers. Some
    // editions (Inside Tent is the concrete case) ship only a replacement
    // hotspots.bmp in Data2 without a replacement backdrop. Selecting the
    // mask from the visual/backdrop package therefore resolves obsolete base
    // IDs. Search the overlay layers from highest to lowest independently.
    const Package *nextHotspotPackage = nullptr;
    const Package::Entry *nextHotspotEntry = nullptr;
    for (const Package *candidate :
         {nextAddonPackage.get(), nextData2Package.get(),
          nextEditionPackage.get(), nextPackage.get()}) {
        if (!candidate) {
            continue;
        }
        if (const auto *entry = candidate->findOptional("hotspots.bmp")) {
            nextHotspotPackage = candidate;
            nextHotspotEntry = entry;
            break;
        }
    }
    if (nextHotspotEntry) {
        nextHotspots = loadBmp(nextHotspotPackage->data(*nextHotspotEntry));
        nextHotspotTexture = makeTexture(renderer, nextHotspots.get());
        SDL_SetTextureBlendMode(nextHotspotTexture.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(nextHotspotTexture.get(), 115);
    }

    auto addMenuBitmap = [&](Package &package, std::string_view name,
                             int x, int y) {
        const std::string asset =
            "bitmap\\" + std::string(name) + ".bmp";
        SurfacePtr surface = loadGreenKeyedBmp(
            package.data(package.find(asset)));
        nextSceneBitmapLayers.push_back(
            {std::string(name), x, y, 0, surface->w, surface->h,
             makeTexture(renderer, surface.get())});
        SDL_SetTextureBlendMode(
            nextSceneBitmapLayers.back().texture.get(), SDL_BLENDMODE_BLEND);
    };

    auto addCornerButton = [&](SceneCornerButtonKind kind,
                               std::string_view stem) {
        // Balloon Hunt's initializer at 0x0042ba40 names its smaller
        // "help/exit out/in" pair explicitly (0x0042ba54-0x0042ba78).
        // The package also contains unrelated 72x72 numeric pairs, so the
        // usual numeric-name lookup would select the wrong controls.
        const bool balloonhuntCorner = sceneName == "balloonhunt";
        std::string normalAsset =
            "bitmap\\" + std::string(stem) +
            (balloonhuntCorner ? " out.bmp" : "_0.bmp");
        std::string pressedAsset =
            "bitmap\\" + std::string(stem) +
            (balloonhuntCorner ? " in.bmp" : "_1.bmp");
        const Package::Entry *normalEntry =
            nextPackage->findOptional(normalAsset);
        const Package::Entry *pressedEntry =
            nextPackage->findOptional(pressedAsset);
        // Whack-a-Mole packages the same normal/pressed controls under the
        // older "out"/"in" filenames instead of the shared numeric names.
        if ((!normalEntry || !pressedEntry) && sceneName == "whackamole") {
            normalAsset = "bitmap\\" + std::string(stem) + " out.bmp";
            pressedAsset = "bitmap\\" + std::string(stem) + " in.bmp";
            normalEntry = nextPackage->findOptional(normalAsset);
            pressedEntry = nextPackage->findOptional(pressedAsset);
        }
        if (!normalEntry || !pressedEntry) {
            return;
        }
        SurfacePtr normal = loadGreenKeyedBmp(
            nextPackage->data(*normalEntry));
        SurfacePtr pressed = loadGreenKeyedBmp(
            nextPackage->data(*pressedEntry));
        const double scaleX =
            kLogicalWidth / static_cast<double>(nextBackdrop->w);
        const double scaleY =
            kLogicalHeight / static_cast<double>(nextBackdrop->h);
        const int width = static_cast<int>(std::lround(normal->w * scaleX));
        const int height = static_cast<int>(std::lround(normal->h * scaleY));
        int x = kind == SceneCornerButtonKind::Help
                    ? 0
                    : kLogicalWidth - width;
        int y = 0;
        // Bugzzz creates its controls with exact arguments (x,y)=(2,1) and
        // (726,2) at 0x00434271-0x004342B0. Those offsets align the 72x72
        // button images over the separate leaf decorations.
        if (sceneName == "bugzzz") {
            x = kind == SceneCornerButtonKind::Help ? 2 : 726;
            y = kind == SceneCornerButtonKind::Help ? 1 : 2;
        }
        // Mountain creates help 0x20 at (12,7) and exit 0x21 at (577,9) in
        // its 640x480 display (0x00440e55, 0x00440e7a).
        if (sceneName == "mountain") {
            x = static_cast<int>(std::lround(
                (kind == SceneCornerButtonKind::Help ? 12 : 577) * scaleX));
            y = static_cast<int>(std::lround(
                (kind == SceneCornerButtonKind::Help ? 7 : 9) * scaleY));
        }
        TexturePtr normalTexture = makeTexture(renderer, normal.get());
        TexturePtr pressedTexture = makeTexture(renderer, pressed.get());
        SDL_SetTextureBlendMode(normalTexture.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(pressedTexture.get(), SDL_BLENDMODE_BLEND);
        nextSceneCornerButtons.push_back(
            {kind, SDL_Rect{x, y, width, height}, std::move(normal),
             std::move(normalTexture), std::move(pressedTexture)});
    };
    // The activity page creates Help and Exit as its own panel controls
    // (0x0045c8fd, 0x0045c91f); app_activitypage.cpp draws and traces them.
    // So do the racing menu (0x0047b700), the butterfly album (0x0043a2a0)
    // and the info viewer (0x00469290), in app_racemenu.cpp, app_buttercol.cpp
    // and app_info.cpp.
    // The race has neither (0x00474870).
    if (sceneName != "activity" && sceneName != "racemenu" &&
        sceneName != "buttercol" && sceneName != "info" &&
        sceneName != "racing") {
        addCornerButton(SceneCornerButtonKind::Help, "help");
        addCornerButton(SceneCornerButtonKind::Exit, "exit");
    }

    // FACT_Init (0x0041dbf0) opens the scene's fact.ini during a normal
    // entry and reads the already-viewed mask from the active profile. The
    // localized text lives in the language package, which the merged
    // installation folds into the scene package.
    FactTable nextSceneFacts;
    std::uint32_t nextSceneFactsViewed = 0;
    for (Package *package :
         {nextLanguagePackage.get(), nextEditionPackage.get(), nextPackage.get()}) {
        if (!package) {
            continue;
        }
        if (const auto *entry = package->findOptional("fact.ini")) {
            nextSceneFacts = FactTable(package->data(*entry));
            break;
        }
    }
    if (!nextSceneFacts.empty() && profiles[profileIndex] != "No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        nextSceneFactsViewed =
            profileSceneRecordU32(profilePath, sceneName, 0x402U, 100U)
                .value_or(0U);
    }

    // GAME_GotoScene updates the engine-owned 0x400 visit record before any
    // scene code runs (0x0040cd1f calls 0x00412030), so scene construction
    // can gate first-visit-only narration on the fresh count. Four scenes do
    // exactly that: desk 0x00424d65, tvstation 0x00427d78, yard 0x004284c1,
    // intro 0x004160f7.
    std::uint32_t sceneVisitCount = 0;
    if (profiles[profileIndex] != "No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(profilePath)) {
            const std::time_t now = std::time(nullptr);
            const auto timestamp = static_cast<std::uint32_t>(
                std::clamp<std::uint64_t>(
                    now < 0 ? 0U : static_cast<std::uint64_t>(now),
                    0U, std::numeric_limits<std::uint32_t>::max()));
            // A profile that cannot be saved (a read-only folder, a full
            // disk) must not stop the game on every scene entry; the scene
            // then runs as it does with no profile, visit count 0.
            try {
                sceneVisitCount =
                    recordProfileSceneVisit(profilePath, sceneName, timestamp);
                std::cout << "Recorded profile visit " << sceneVisitCount
                          << " for " << sceneName << '\n';
            } catch (const std::exception &error) {
                std::cerr << "Could not record the profile visit: "
                          << error.what() << '\n';
            }
        }
    }
    activeSceneVisitCount = sceneVisitCount;

    // Story scenes construct their contents in their own files through
    // this view of the state above; see AppCore::SceneBuild.
    SceneBuild build{
        .sceneName = sceneName,
        .entryMode = entryMode,
        .sceneVisitCount = sceneVisitCount,
        .nextPackage = nextPackage,
        .nextEditionPackage = nextEditionPackage,
        .nextData2Package = nextData2Package,
        .nextLanguagePackage = nextLanguagePackage,
        .nextAddonPackage = nextAddonPackage,
        .nextSceneClips = nextSceneClips,
        .nextSceneCharacterGroups = nextSceneCharacterGroups,
        .nextSceneBitmapLayers = nextSceneBitmapLayers,
        .nextSceneCollectibles = nextSceneCollectibles,
        .nextSceneUsesAudioTimeline = nextSceneUsesAudioTimeline,
        .nextSceneSequenceBlocksInput = nextSceneSequenceBlocksInput,
        .nextSceneTimedTransition = nextSceneTimedTransition,
        .nextSceneTitleTexture = nextSceneTitleTexture,
        .nextSceneTitleWidth = nextSceneTitleWidth,
        .nextSceneTitleHeight = nextSceneTitleHeight,
        .sceneAnimationData = sceneAnimationData,
        .appendSceneNarration = appendSceneNarration,
        .appendSceneDefinition = appendSceneDefinition,
        .addSceneCharacterGroupWithTables = addSceneCharacterGroup,
        .addSceneCharacterStoredTables = addSceneCharacterStoredTables,
        .addSceneCharacterCategory4Table = addSceneCharacterCategory4Table,
        .addSceneCollectible = addSceneCollectible,
        .addSceneAnimationObject = addSceneAnimationObject,
        .addScenePlayer = addScenePlayer,
        .addMenuBitmap = addMenuBitmap,
    };

    if (sceneName == "intro") {
        buildIntroScene(build);
    }

    if (sceneName == "yard") {
        buildYardScene(build);
    }

    if (sceneName == "desk") {
        buildDeskScene(build);
    }

    if (sceneName == "pee") {
        buildPeeScene(build, nextPeeParticleField);
    }

    if (sceneName == "house") {
        buildHouseScene(build);
    } else if (sceneName == "morning") {
        buildMorningScene(build);
    } else if (sceneName == "outtent") {
        buildOuttentScene(build);
    } else if (sceneName == "buildacar") {
        buildBuildacarScene(build, nextBuildacarWorkshop);
    } else if (sceneName == "tvstation") {
        buildTvstationScene(build);
    } else if (sceneName == "town") {
        buildTownScene(build);
    } else if (sceneName == "outro") {
        buildOutroScene(build);
    }

    if (sceneName == "tvroom") {
        buildTvroomScene(build, nextTvroomConcurrentClip);
    }

    if (sceneName == "intent") {
        buildIntentScene(build);
    }

    const bool hasProfile =
        profiles[profileIndex] != "No profile" &&
        fs::exists(profileDirectory / (profiles[profileIndex] + ".pfl"));
    const fs::path activeProfilePath =
        hasProfile
            ? profileDirectory / (profiles[profileIndex] + ".pfl")
            : fs::path{};
    std::size_t generatedCollectibleCount = 0;
    for (const auto &definition : fgp_generated::kSceneCollectibles) {
        if (definition.scene != sceneName) {
            continue;
        }
        const bool alreadyLoaded = std::any_of(
            nextSceneCollectibles.begin(), nextSceneCollectibles.end(),
            [&](const SceneCollectible &candidate) {
                return candidate.name == definition.name &&
                       candidate.hotspot == definition.dynamicHotspot;
            });
        if (alreadyLoaded) {
            continue;
        }
        bool visible = false;
        if (hasProfile && definition.name == "carpart") {
            visible = profileSceneRecordU32(
                          activeProfilePath, sceneName, 0x402U, 0U)
                          .value_or(0U) == 0U;
        } else if (hasProfile && definition.name == "3dglasses" &&
                   definition.storyIndex >= 0) {
            const auto mask = 1U << static_cast<std::uint32_t>(
                                      definition.storyIndex);
            visible = (profileSceneRecordU32(
                           activeProfilePath, "anaglyph", 0x402U, 100U)
                           .value_or(0U) &
                       mask) == 0U;
        }
        Package *package = nextPackage.get();
        if (definition.name == "3dglasses") {
            if (!nextAddonPackage) {
                continue;
            }
            package = nextAddonPackage.get();
        }
        addSceneCollectible(*package, definition.name.data(), definition.x(),
                            definition.y(), definition.dynamicHotspot,
                            definition.storyIndex, visible);
        ++generatedCollectibleCount;
    }
    if (generatedCollectibleCount != 0) {
        std::cout << "Loaded " << generatedCollectibleCount
                  << " generated collectible(s) for " << sceneName << '\n';
    }

    if (nextSceneCharacterGroups.empty()) {
        std::size_t generatedGroupCount = 0;
        for (const auto &definition : fgp_generated::kSceneGroups) {
            if (definition.scene != sceneName) {
                continue;
            }
            addSceneCharacterGroup(definition.dynamicHotspot,
                                   definition.conflictMask,
                                   definition.baseTableRva,
                                   definition.category2TableRva,
                                   definition.category3TableRva);
            ++generatedGroupCount;
        }
        if (generatedGroupCount != 0) {
            std::cout << "Loaded " << generatedGroupCount
                      << " generated persistent group(s) for "
                      << sceneName << '\n';
        }
    }

    // Outtent's second group starts on its table's first entry, EMN-B00
    // (0x00426204 seeds it through 0x0040a7c0). The EMN-B04 the earlier
    // captures saw first follows Space, which reseeds the group onto the
    // next entry (run 20261020-175032-1dce8348-4d8a: B00 at entry, B04 at
    // Space); native used to start on B04 instead of reseeding.

    // Recovered entry-mode seeding. Binding at 0x0040a2b0 and initial
    // activation at 0x0040a790/0x0040a880 are distinct states. An unseeded
    // group remains enabled so it can observe a held conflict and activate at
    // the release boundary; representing it as disabled loses that boundary
    // and made Yard's LUD-002 start before LUD-B02 finished.
    const auto *seeding = sceneEntrySeeding(sceneName,
                                           activeSceneEntryMode);
    for (auto &group : nextSceneCharacterGroups) {
        // A group the scene binds only later stays unbound: Pee's magnet
        // (bit 0x10, hotspot 21) is bound by its click case (0x004269e5,
        // the 0x0040a2b0 call scene_logic lists under 0x00426970), so it
        // must not play from entry.
        if (sceneName == "pee" && group.hotspot == 21U) {
            group.enabled = false;
            group.baseClipActivated = false;
            continue;
        }
        group.enabled = true;
        group.baseClipActivated =
            seeding == nullptr ||
            (group.conflictMask & seeding->seededBits) != 0U;
    }
    if (seeding != nullptr) {
        std::size_t seededCount = 0;
        for (const auto &group : nextSceneCharacterGroups) {
            if (group.baseClipActivated) {
                ++seededCount;
            }
        }
        std::cout << "Entry mode " << activeSceneEntryMode << " seeds "
                  << seededCount << " of " << nextSceneCharacterGroups.size()
                  << " group(s) for " << sceneName << " (bits 0x" << std::hex
                  << seeding->seededBits << std::dec << ")\n";
    }

    scenePackage = std::move(nextPackage);
    sceneEditionPackage = std::move(nextEditionPackage);
    sceneData2Package = std::move(nextData2Package);
    sceneAddonPackage = std::move(nextAddonPackage);
    sceneLanguagePackage = std::move(nextLanguagePackage);
    sceneIsMinigameShell = minigameDefinition(sceneName) != nullptr;
    sceneBackdrop = std::move(nextBackdrop);
    sceneBackdropTexture = std::move(nextBackdropTexture);
    sceneHotspots = std::move(nextHotspots);
    sceneHotspotTexture = std::move(nextHotspotTexture);
    sceneTitleTexture = std::move(nextSceneTitleTexture);
    sceneTitleWidth = nextSceneTitleWidth;
    sceneTitleHeight = nextSceneTitleHeight;
    viewMode = ViewMode::Scene;
    activeScene = sceneName;
    sceneIndexTiles.clear();
    sceneIndexTileBounds.clear();
    sceneIndexMedals.clear();
    sceneIndexMedalBounds.clear();
    for (auto &texture : sceneIndexTabTextures) {
        texture.reset();
    }
    sceneIndexGoldFrameTexture.reset();
    sceneClips = std::move(nextSceneClips);
    sceneCharacterGroups = std::move(nextSceneCharacterGroups);
    tvroomConcurrentClip = std::move(nextTvroomConcurrentClip);
    tvroomConcurrentFollowupPending = false;
    intentGroupRestoreAt.reset();
    sceneAnimationObjects = std::move(nextSceneAnimationObjects);
    peeParticleField = std::move(nextPeeParticleField);
    sceneBitmapLayers = std::move(nextSceneBitmapLayers);
    sceneCornerButtons = std::move(nextSceneCornerButtons);
    pressedSceneCornerButton.reset();
    sceneCollectibles = std::move(nextSceneCollectibles);
    buildacarWorkshop = std::move(nextBuildacarWorkshop);
    audiopairsGame.reset();
    sockdrawerGame.reset();
    butterflyGame.reset();
    lettersortGame.reset();
    mahjongGame.reset();
    whackamoleGame.reset();
    balloonhuntGame.reset();
    pipelineGame.reset();
    hopscotchGame.reset();
    beemazeGame.reset();
    bugzzzGame.reset();
    wheelbarrowGame.reset();
    goodbyeState.reset();
    outroState.reset();
    buildabikeGame.reset();
    mountainGame.reset();
    hustleGame.reset();
    chessGame.reset();
    minigameBusyCursor = false;
    jigsawGame.reset();
    slidingGame.reset();
    saveBouquet();
    bouquetGame.reset();
    fenceGame.reset();
    resetViewerScenes();
    puzzleGame.reset();
    draughtsGame.reset();
    textinvaderGame.reset();
    saveSynonymRanks();
    synonymGame.reset();
    mathlabGame.reset();
    mainscoreBoard.reset();
    tournamentScreen.reset();
    saveTriviaRanks();
    triviaGame.reset();
    saveColorfillState();
    endColorfillScene();
    colorfillGame.reset();
    recoveredMinigameRuntime.reset();
    activitypageGame.reset();
    saveRacemenu();
    racemenuGame.reset();
    racingGame.reset();
    buttercolAlbum.reset();
    birthdayGame.reset();
    creditsGame.reset();
    if (sceneName == "birthday" && birthdayMatch) {
        loadBirthdayGame(std::move(birthdayNames));
    } else if (sceneName == "credits") {
        creditsGame = loadCreditsGame();
    } else if (sceneName == "audiopairs") {
        audiopairsGame = loadAudiopairsGame(*scenePackage);
    } else if (sceneName == "sockdrawer") {
        sockdrawerGame = loadSockdrawerGame(*scenePackage);
    } else if (sceneName == "butterfly") {
        butterflyGame = loadButterflyGame(*scenePackage);
    } else if (sceneName == "lettersort") {
        lettersortGame = loadLettersortGame(*scenePackage);
    } else if (sceneName == "mahjong") {
        mahjongGame = loadMahjongGame(*scenePackage);
    } else if (sceneName == "whackamole") {
        whackamoleGame = loadWhackamoleGame(*scenePackage);
    } else if (sceneName == "balloonhunt") {
        balloonhuntGame = loadBalloonhuntGame(*scenePackage);
    } else if (sceneName == "pipeline") {
        pipelineGame = loadPipelineGame(*scenePackage);
    } else if (sceneName == "hopscotch") {
        hopscotchGame = loadHopscotchGame(*scenePackage);
    } else if (sceneName == "beemaze") {
        beemazeGame = loadBeemazeGame(*scenePackage);
    } else if (sceneName == "bugzzz") {
        bugzzzGame = loadBugzzzGame(*scenePackage);
    } else if (sceneName == "wheelbarrow") {
        wheelbarrowGame = loadWheelbarrowGame(*scenePackage);
    } else if (sceneName == "buildacar") {
        buildacarWorkshop = loadBuildacarWorkshop(*scenePackage);
    } else if (sceneName == "goodbye") {
        loadGoodbyeScene();
    } else if (sceneName == "outro") {
        loadOutroScene();
    } else if (sceneName == "buildabike") {
        buildabikeGame = loadBuildabikeGame(*scenePackage);
    } else if (sceneName == "mountain") {
        mountainGame = loadMountainGame(*scenePackage);
    } else if (sceneName == "hustle") {
        hustleGame = loadHustleGame(*scenePackage);
    } else if (sceneName == "chess") {
        chessGame = loadChessGame(*scenePackage);
    } else if (sceneName == "jigsaw") {
        jigsawGame = loadJigsawGame(*scenePackage);
    } else if (sceneName == "sliding") {
        slidingGame = loadSlidingGame(*scenePackage);
    } else if (sceneName == "bouquet") {
        loadBouquetGame(*scenePackage);
    } else if (sceneName == "fence") {
        fenceGame = loadFenceGame(*scenePackage);
    } else if (sceneName == "puzzle") {
        puzzleGame = loadPuzzleGame(*scenePackage);
    } else if (sceneName == "draughts") {
        draughtsGame = loadDraughtsGame(*scenePackage);
    } else if (sceneName == "textinvader") {
        textinvaderGame = loadTextinvaderGame(*scenePackage);
    } else if (sceneName == "synonym") {
        synonymGame = loadSynonymGame(*scenePackage);
    } else if (sceneName == "mathlab") {
        mathlabGame = loadMathlabGame(*scenePackage);
    } else if (sceneName == "gametrivia") {
        triviaGame = loadTriviaGame(*scenePackage);
    } else if (sceneName == "mainscore") {
        loadMainscoreBoard();
    } else if (sceneName == "colorfill") {
        colorfillGame = loadColorfillGame(*scenePackage);
    } else if (sceneName == "activity") {
        activitypageGame = loadActivitypageGame();
    } else if (sceneName == "racemenu") {
        racemenuGame = loadRacemenuGame();
    } else if (sceneName == "racing") {
        loadRacingGame();
    } else if (sceneName == "buttercol") {
        buttercolAlbum = loadButtercolAlbum();
    } else if (sceneName == "tournament" || sceneName == "tourneyscore") {
        loadTournamentScreen();
    }
    // 0x00419660: every round opens with PLAYERREADY; closing it starts
    // play (startTournamentRound).
    if (tournament.active && !tournament.games.empty() &&
        sceneName ==
            tournament.games[static_cast<std::size_t>(tournament.game)].scene) {
        showTournamentPlayerReady();
    }
    loadViewerScene(sceneName);
    if (sceneIsMinigameShell && !viewerSceneActive() && !audiopairsGame && !sockdrawerGame &&
        !butterflyGame && !lettersortGame && !whackamoleGame &&
        !balloonhuntGame && !pipelineGame && !hopscotchGame && !beemazeGame &&
        !bugzzzGame && !wheelbarrowGame && !buildabikeGame && !mountainGame &&
        !hustleGame && !jigsawGame && !slidingGame && !bouquetGame && !fenceGame && !puzzleGame &&
        !textinvaderGame && !synonymGame &&
        !mathlabGame &&
        !triviaGame && !mahjongGame && !draughtsGame && !chessGame &&
        !activitypageGame && !colorfillGame) {
        recoveredMinigameRuntime = loadRecoveredMinigameRuntime();
    }
    sceneFacts = std::move(nextSceneFacts);
    sceneFactsViewed = nextSceneFactsViewed;
    sceneAudioTimeline = std::move(nextSceneAudioTimeline);
    sceneEffectTimeline = std::move(nextSceneEffectTimeline);
    sceneBackgroundMusic = std::move(nextSceneBackgroundMusic);
    sceneAudioGains = nextSceneAudioGains;
    sceneBackgroundMusicAsset =
        std::move(nextSceneBackgroundMusicAsset);
    sceneUsesAudioTimeline = nextSceneUsesAudioTimeline;
    sceneSequenceBlocksInput = nextSceneSequenceBlocksInput;
    sceneSequenceFinished = false;
    sceneFinalStageHeld = false;
    sceneHeldStageIndex.reset();
    sceneClipIndex = 0;
    sceneClipTimelineStart = 0.0;
    sceneClipStartOffsets =
        scheduleSceneClipStarts(sceneClips, 0.0, 0U);
    sceneSequenceRequestedAt = 0.0;
    sceneClipStarted = !sceneClips.empty();
    sceneSequenceHeldConflictMask = sceneClips.empty()
                                            ? 0U
                                            : sceneClips.front().conflictMask;
    sceneSequencePendingConflictMask = 0;
    sceneTimedTransition = std::move(nextSceneTimedTransition);
    sceneIndexNames.clear();
    sceneIndexUnlocked.clear();
    sceneIndexCursorKinds.clear();
    if (sceneName == "sceneindex") {
        if (sceneAddonPackage) {
            for (std::size_t index = 0; index < 2; ++index) {
                const std::string asset =
                    "bitmap\\button_" + std::to_string(index) + ".bmp";
                if (const auto *entry =
                        sceneAddonPackage->findOptional(asset)) {
                    SurfacePtr tab = loadBmp(sceneAddonPackage->data(*entry));
                    sceneIndexTabTextures[index] =
                        makeTexture(renderer, tab.get());
                }
            }
        }
        selectSceneIndexTab(0);
    }
    configureStoryNavigation(sceneName);
    hoveredHotspot = hotspotAt(mouseX, mouseY);
    hoveredBounds = currentHotspotBounds(hoveredHotspot);
    resetAnimationClock();
    if (!pendingYardOpeningNarration.empty()) {
        // Armed after the clock reset: armSceneNarration measures from
        // animationTimeNow(), which is only zero for this scene from here on.
        armSceneNarration(pendingYardOpeningNarration, 2.0, false);
        pendingYardOpeningNarration.clear();
    }
    if (peeParticleField) {
        // 0x00426730 starts the first cycle as part of a normal entry.
        peeParticleField->lastUpdate = 0.0;
        startPeeParticleCycle();
    }
    audioMixer.setBackground(sceneBackgroundMusic,
                             audioVolumes[2] * sceneAudioGains[2]);
    if (sceneUsesAudioTimeline) {
        audioMixer.setForeground(
            sceneAudioTimeline,
            audioVolumes[1] * sceneAudioGains[1],
            &sceneEffectTimeline,
            audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice,
                             musicEnabled && !paused ? 0 : 1);
    } else if (!sceneClips.empty()) {
        audioMixer.setForeground(sceneClips.front().media,
                                 clipAudioVolume(sceneClips.front().media));
        SDL_PauseAudioDevice(audioDevice,
                             musicEnabled && !paused ? 0 : 1);
    } else {
        audioMixer.clearForeground();
        SDL_PauseAudioDevice(audioDevice,
                             musicEnabled && !paused ? 0 : 1);
    }
    if (audiopairsGame) {
        audioMixer.setEffect(audiopairsGame->animations[0].media,
                             audioVolumes[1] * sceneAudioGains[1]);
    }
    if (butterflyGame) {
        audioMixer.setEffect(butterflyGame->animations[0].media,
                             audioVolumes[0] * sceneAudioGains[0]);
    }
    const bool detailedMinigame = audiopairsGame || sockdrawerGame ||
                                  butterflyGame || lettersortGame ||
                                  whackamoleGame || balloonhuntGame ||
                                  pipelineGame || hopscotchGame || beemazeGame ||
                                  bugzzzGame || wheelbarrowGame ||
                                  buildabikeGame || mountainGame || hustleGame ||
                                  jigsawGame || slidingGame ||
                                  fenceGame ||
                                  textinvaderGame || puzzleGame ||
                                  mahjongGame || draughtsGame;
    const bool detailedMinigameWithEducational =
        detailedMinigame || synonymGame || mathlabGame || triviaGame ||
        chessGame || colorfillGame;
    const std::string title =
        "FGP Native — " + std::string(label) +
        (detailedMinigameWithEducational
             ? " — native gameplay pass · Help and exit available"
             : recoveredMinigameRuntime
                   ? " — recovered interaction pass · Help and exit available"
                   : sceneClips.empty()
                   ? " — original package loaded · exit control back"
                   : " — native animation + audio · exit control back");
    SDL_SetWindowTitle(window, title.c_str());
    referenceTrace.emit("scene_change", {{"old_scene", previousScene},
                                          {"scene", sceneName},
                                          {"entry_mode", entryMode},
                                          {"package_layer", primaryDataLayer}});
    leaveSceneMusicDuck(previousScene);
    sceneReadyAfterPresent = sceneName;
    for (const auto &clip : sceneClips) {
        referenceTrace.emit("animation_add", {{"asset", clip.name},
                                               {"x", clip.x},
                                               {"y", clip.y},
                                               {"layer", clip.layer},
                                               {"conflict_mask", clip.conflictMask}});
    }

    for (const auto &object : sceneAnimationObjects) {
        referenceTrace.emit("animation_add", {{"asset", object.name},
                                               {"x", object.x},
                                               {"y", object.y},
                                               {"layer", object.layer},
                                               {"hotspot", object.hotspot}});
    }
    if (sceneName == "morning") {
        // 0x00425e6f seeds the bird group before 0x00425e79 starts the
        // squirrel sequence, so its first clip is the scene's first add.
        updateSceneCharacterGroups(0.0);
    }
    if (sceneName == "intro" && sceneTitleTexture) {
        // 0x004165d5 adds the title bitmap at its descriptor position before
        // the sequence starts.
        referenceTrace.emit("animation_start", {{"asset", "title"},
                                                 {"x", -1}, {"y", -1},
                                                 {"layer", 0},
                                                 {"loop", false}});
    }
    if (!sceneClips.empty()) {
        const auto &clip = sceneClips.front();
        referenceTrace.emit("animation_start", {{"asset", clip.name},
                                                 {"x", clip.x},
                                                 {"y", clip.y},
                                                 {"layer", clip.layer},
                                                 {"loop", clip.loopVideo}});
        handleSceneAnimationStarted(clip.name, 0.0);
    }
    for (auto &object : sceneAnimationObjects) {
        if (!object.loop || !object.playing) {
            continue;
        }
        referenceTrace.emit("animation_start", {{"asset", object.name},
                                                 {"x", object.x},
                                                 {"y", object.y},
                                                 {"layer", object.layer},
                                                 {"loop", true}});
        // After the scene's setForeground, which drops added effects.
        if (!object.audio.audioSamples.empty()) {
            object.audioEffect = audioMixer.addEffect(
                object.audio, audioVolumes[0] * sceneAudioGains[0], true);
        }
    }
    if (!sceneBackgroundMusicAsset.empty()) {
        referenceTrace.emit("audio_start", {{"asset", sceneBackgroundMusicAsset},
                                             {"role", "music"}});
    }
    if (sceneName == "morning") {
        // 0x00425ead, the initializer's last call.
        startSceneNarration("S07AN-NAR-001", false);
    }
    // The initializers that call 0x0040b0f0: intro 0x004165c5, yard
    // 0x0042808d, house 0x0042524e, morning 0x00425ea8, outtent 0x004260f1,
    // hopscotch 0x0043bc95.
    if (sceneName == "intro" || sceneName == "yard" || sceneName == "house" ||
        sceneName == "morning" || sceneName == "outtent" ||
        sceneName == "hopscotch") {
        startSceneAmbience(0.0);
    }
    std::cout << "Loaded native "
              << (detailedMinigameWithEducational
                      ? "playable minigame "
                      : recoveredMinigameRuntime
                            ? "recovered minigame pass "
                            : "scene ")
              << primaryDataPath.parent_path().filename().string() << '/'
              << sceneName << ".bin (" << scenePackage->entries().size()
              << " assets) in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - loadStarted)
                     .count()
              << " ms\n";
}

std::optional<std::size_t> AppCore::sceneIndexTabAt(int x, int y) const {
    if (viewMode != ViewMode::Scene || activeScene != "sceneindex") {
        return std::nullopt;
    }
    constexpr std::array<SDL_Rect, 4> bounds{{
        {45, 510, 169, 50}, {223, 510, 169, 50},
        {402, 510, 169, 50}, {581, 510, 169, 50}}};
    const SDL_Point point{x, y};
    for (std::size_t index = 0; index < bounds.size(); ++index) {
        if (SDL_PointInRect(&point, &bounds[index])) {
            return index;
        }
    }
    return std::nullopt;
}

void AppCore::selectSceneIndexTab(std::size_t tabIndex) {
    if (viewMode != ViewMode::Scene || activeScene != "sceneindex" ||
        tabIndex >= 4U || !storyIndexConfiguration) {
        return;
    }

    static constexpr std::array<std::string_view, 4> tabNames{
        "storypage", "subgame", "activity", "gold"};
    sceneIndexTab = tabIndex;
    sceneIndexTiles.clear();
    sceneIndexTileBounds.clear();
    sceneIndexMedals.clear();
    sceneIndexMedalBounds.clear();
    sceneIndexNames.clear();
    sceneIndexUnlocked.clear();
    sceneIndexCursorKinds.clear();
    sceneIndexGoldFrameTexture.reset();

    const std::string scopedSection =
        "sceneindex:" + std::string(tabNames[tabIndex]);
    const std::string section =
        storyIndexConfiguration->get(scopedSection, "name0").empty()
            ? std::string(tabNames[tabIndex])
            : scopedSection;
    const std::size_t slotCount = tabIndex == 3U ? 4U : 15U;
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex] + ".pfl");
    const bool hasProfile = profiles[profileIndex] != "No profile" &&
                            fs::exists(profilePath);

    const auto configuredInteger = [&](std::string_view key,
                                       int fallback) {
        try {
            return std::stoi(storyIndexConfiguration->get(
                section, key, std::to_string(fallback)));
        } catch (const std::exception &) {
            return fallback;
        }
    };
    const auto findTileAsset = [&](const std::string &asset)
        -> std::pair<Package *, const Package::Entry *> {
        for (Package *package :
             {sceneAddonPackage.get(), sceneData2Package.get(),
              sceneEditionPackage.get(), scenePackage.get()}) {
            if (package) {
                if (const auto *entry = package->findOptional(asset)) {
                    return {package, entry};
                }
            }
        }
        return {nullptr, nullptr};
    };

    if (tabIndex == 3U && sceneAddonPackage) {
        if (const auto *entry =
                sceneAddonPackage->findOptional("bitmap\\frame_4.bmp")) {
            SurfacePtr frame = loadBmp(sceneAddonPackage->data(*entry));
            sceneIndexGoldFrameTexture = makeTexture(renderer, frame.get());
        }
    }

    for (std::size_t index = 0; index < slotCount; ++index) {
        const std::string suffix = std::to_string(index);
        const std::string name = storyIndexConfiguration->get(
            section, "name" + suffix);
        const int configuredCursor = configuredInteger("cursor" + suffix, -1);
        switch (configuredCursor) {
        case 0: sceneIndexCursorKinds.push_back(CursorKind::StoryPage); break;
        case 1: sceneIndexCursorKinds.push_back(CursorKind::Subgame); break;
        case 2: sceneIndexCursorKinds.push_back(CursorKind::Activity); break;
        case 3: sceneIndexCursorKinds.push_back(CursorKind::ActionGame); break;
        default: sceneIndexCursorKinds.push_back(CursorKind::Default); break;
        }

        const bool configuredOpen =
            configuredInteger("state" + suffix, 0) != 0;
        const bool visited =
            hasProfile && !name.empty() &&
            profileSceneRecordU32(profilePath, name, 0x400U, 0U, false)
                    .value_or(0U) > 0U;
#if defined(FGP_OPEN_ALL_GAMES) && FGP_OPEN_ALL_GAMES
        // Tabs 1 and 2 are the subgames and activities; the story pages
        // (tab 0) still open as the player reaches them.
        const bool openedByBuild = tabIndex == 1U || tabIndex == 2U;
#else
        const bool openedByBuild = false;
#endif
        const bool unlocked = !name.empty() &&
            (developmentMode || openedByBuild || configuredOpen || visited ||
             (tabIndex == 0U && index == 0U));
        sceneIndexNames.push_back(name);
        sceneIndexUnlocked.push_back(unlocked);

        std::string asset = "bitmap\\empty.bmp";
        if (!name.empty()) {
            asset = "bitmap\\" + name +
                    (unlocked ? "_0.bmp" : "_2.bmp");
        }
        auto [tilePackage, tileEntry] = findTileAsset(asset);
        if (!tileEntry) {
            std::tie(tilePackage, tileEntry) =
                findTileAsset("bitmap\\empty.bmp");
        }
        if (!tilePackage || !tileEntry) {
            throw std::runtime_error(
                "Contents tile asset not found: " + asset);
        }
        SurfacePtr tile = loadBmp(tilePackage->data(*tileEntry));
        sceneIndexTiles.push_back(makeTexture(renderer, tile.get()));
        if (tabIndex == 3U) {
            sceneIndexTileBounds.push_back(SDL_Rect{
                configuredInteger("imagex" + suffix, 0),
                configuredInteger("imagey" + suffix, 0),
                tile->w, tile->h});
        } else {
            sceneIndexTileBounds.push_back(SDL_Rect{
                51 + static_cast<int>(index % 5U) * 142,
                126 + static_cast<int>(index / 5U) * 119,
                129, 106});
        }

        TexturePtr medalTexture;
        SDL_Rect medalBounds = sceneIndexTileBounds.back();
        if (hasProfile && unlocked && !name.empty() &&
            configuredInteger("ismedals", 0) != 0) {
            try {
                const auto scores = profileSceneScores(profilePath, name);
                const std::uint32_t medalLevel =
                    scores ? scores->medalLevel : 0U;
                if (medalLevel > 0U && medalLevel <= 4U) {
                    static constexpr std::array<std::string_view, 5>
                        medalNames{"none", "bronze", "silver", "gold",
                                   "platinum"};
                    const std::string medalName =
                        storyIndexConfiguration->get(
                            section, "medal" + std::to_string(medalLevel),
                            std::string(medalNames[medalLevel]));
                    auto [medalPackage, medalEntry] =
                        findTileAsset("bitmap\\" + medalName + ".bmp");
                    if (medalPackage && medalEntry) {
                        SurfacePtr medal = loadGreenKeyedBmp(
                            medalPackage->data(*medalEntry));
                        medalBounds.w = medal->w;
                        medalBounds.h = medal->h;
                        medalTexture = makeTexture(renderer, medal.get());
                        SDL_SetTextureBlendMode(
                            medalTexture.get(), SDL_BLENDMODE_BLEND);
                        referenceTrace.emit("animation_add", {
                            {"asset", medalName},
                            {"x", medalBounds.x}, {"y", medalBounds.y},
                            {"layer", 1}, {"hotspot", 0},
                            {"conflict_mask", 0}});
                    }
                }
            } catch (const std::exception &error) {
                std::cerr << "Could not read Contents medal for " << name
                          << ": " << error.what() << '\n';
            }
        }
        sceneIndexMedals.push_back(std::move(medalTexture));
        sceneIndexMedalBounds.push_back(medalBounds);
    }

    hoveredHotspot = hotspotAt(mouseX, mouseY);
    hoveredBounds = currentHotspotBounds(hoveredHotspot);
    const std::string title =
        "FGP Native — Contents — " + std::string(tabNames[tabIndex]) +
        " · exit control back";
    SDL_SetWindowTitle(window, title.c_str());
}

void AppCore::enterScene(const std::string &sceneName, std::string_view label,
                         int entryMode) {
    // Loading and decoding a package is synchronous. Deferring it until
    // immediately after this frame lets the original wait.smk cursor be
    // presented before the main thread begins that work.
    if (!pendingScene) {
        navigationHistory.push_back(
            {viewMode, activeScene,
             viewMode == ViewMode::Menu ? "Main menu" : activeScene,
             activeSceneEntryMode});
    }
    pendingSceneEntryMode = entryMode;
    pendingScene = std::pair{sceneName, std::string(label)};
}

void AppCore::returnFromScene() {
    pressedSceneCornerButton.reset();
    if (askTournamentAbort()) {
        return;
    }
    if (activeScene == "bouquet" || activeScene == "colorfill") {
        // Bouquet's exit control tail-jumps to 0x0040cc30 (0x0045ec2e), a
        // jump the call-only scan behind kSceneDeclaredParentReturns misses:
        // the registry parent is desk, and a story parent gets entry mode 1.
        // Color Fill's exit control 99 does the same at 0x0046175b; its
        // registry record 0x00506158 also names desk, and a capture of the
        // original on 2026-10-18 shows exactly that scene change.
        if (!navigationHistory.empty()) {
            navigationHistory.pop_back();
        }
        pendingSceneEntryMode = 1;
        pendingScene = std::pair{std::string("desk"), std::string("desk")};
        return;
    }
    const auto declaredParent = std::find_if(
        fgp_generated::kSceneDeclaredParentReturns.begin(),
        fgp_generated::kSceneDeclaredParentReturns.end(),
        [this](const auto &row) { return row.scene == activeScene; });
    if (declaredParent != fgp_generated::kSceneDeclaredParentReturns.end()) {
        // Discard the browser-style predecessor that enterScene recorded. The
        // original's 0x0040CC30 helper ignores where the scene was entered
        // from and reads its parent from the active scene record instead.
        if (!navigationHistory.empty()) {
            navigationHistory.pop_back();
        }
        if (declaredParent->parent == "menu") {
            enterMenu();
        } else {
            pendingSceneEntryMode = declaredParent->entryMode;
            pendingScene = std::pair{
                std::string(declaredParent->parent),
                std::string(declaredParent->parent)};
        }
        return;
    }
    if (navigationHistory.empty()) {
        // `menu` is also the original registry's only root (record +8 is
        // null), so the legacy empty-history fallback agrees with that root
        // even for scenes whose exit mechanism is still unproven.
        enterMenu();
        return;
    }
    NavigationTarget target = std::move(navigationHistory.back());
    navigationHistory.pop_back();
    if (target.mode == ViewMode::Menu || target.scene == "menu") {
        enterMenu();
    } else {
        pendingSceneEntryMode = target.entryMode;
        pendingScene =
            std::pair{std::move(target.scene), std::move(target.label)};
    }
}

void AppCore::leaveToRegistryParent() {
    // 0x0040cc30 reads the active record's parent name (+8, 0x0040cc3c) and
    // enters it through GAME_GotoSceneByName 0x0040cd70, with entry mode 1
    // when the parent is a story scene (0x0040cc53). 0x0040cd70 first asks
    // 0x0040cd90, which during a tournament round asks to abort instead.
    // The parents come from the registry via src/generated/scene_escape.hpp.
    pressedSceneCornerButton.reset();
    if (askTournamentAbort()) {
        return;
    }
    const auto row = std::find_if(
        fgp_generated::kSceneEscape.begin(), fgp_generated::kSceneEscape.end(),
        [this](const auto &entry) { return entry.scene == activeScene; });
    if (row == fgp_generated::kSceneEscape.end() || row->parent.empty()) {
        return;
    }
    // As in returnFromScene: the original ignores where the scene was
    // entered from, so the browser-style predecessor is dropped.
    if (!navigationHistory.empty()) {
        navigationHistory.pop_back();
    }
    if (row->parent == "menu") {
        enterMenu();
        return;
    }
    pendingSceneEntryMode = row->parentEntryMode;
    pendingScene =
        std::pair{std::string(row->parent), std::string(row->parent)};
}

void GameApp::configureStoryNavigation(const std::string &sceneName) {
    closeStoryNavigation();
    storyPreviousScene.clear();
    storyNextScene.clear();
    storyPreviousMode = 0;
    storyNextMode = 0;

    if (sceneName == "intro") {
        storyNextScene = "yard";
    } else if (sceneName == "yard") {
        storyNextScene = "desk";
    } else if (sceneName == "desk") {
        storyPreviousScene = "yard";
        storyNextScene = "house";
    } else if (sceneName == "pee") {
        storyPreviousScene = "desk";
        storyNextScene = "house";
    } else if (sceneName == "house") {
        storyPreviousScene = "desk";
        storyNextScene = "tvroom";
    } else if (sceneName == "tvroom") {
        storyPreviousScene = "house";
        storyNextScene = "garage";
    } else if (sceneName == "garage") {
        storyPreviousScene = "tvroom";
        storyNextScene = "morning";
    } else if (sceneName == "outtent") {
        storyPreviousScene = "garage";
        storyNextScene = "intent";
    } else if (sceneName == "intent") {
        storyPreviousScene = "outtent";
        storyNextScene = "town";
        storyNextMode = 2;
    } else if (sceneName == "buildacar") {
        storyPreviousScene = "intent";
        if (developmentMode ||
            (buildacarWorkshop && buildacarWorkshop->placedParts >= 13U)) {
            storyNextScene = "tvstation";
        }
    } else if (sceneName == "tvstation") {
        storyPreviousScene = "buildacar";
        storyNextScene = "racing";
    } else if (sceneName == "goodbye") {
        storyPreviousScene = "tvstation";
        storyNextScene = "outro";
    } else if (sceneName == "outro") {
        storyPreviousScene = "goodbye";
    }
    // Birthday's right click (event 0x107, 0x00415029) opens the popup
    // through 0x004155f0 with no destinations, so only Help and Exit work.
    storyNavigationAvailable = sceneName == "birthday" ||
                               !storyPreviousScene.empty() ||
                               !storyNextScene.empty();
}

bool AppCore::openStoryNavigation(int x, int y) {
    if (viewMode != ViewMode::Scene || !storyNavigationAvailable) {
        return false;
    }
    storyNavigationOrigin.x = std::clamp(
        x - 82, 0, kLogicalWidth - storyNavigationBackdrop->w);
    storyNavigationOrigin.y = std::clamp(
        y - 62, 0, kLogicalHeight - storyNavigationBackdrop->h);
    storyNavigationOpen = true;
    pressedStoryNavigationControl = StoryNavigationControl::None;
    return true;
}

void AppCore::closeStoryNavigation() {
    storyNavigationOpen = false;
    pressedStoryNavigationControl = StoryNavigationControl::None;
}

AppCore::StoryNavigationControl
AppCore::storyNavigationControlAt(int x, int y) const {
    if (!storyNavigationOpen) {
        return StoryNavigationControl::None;
    }
    const SDL_Point point{x - storyNavigationOrigin.x,
                          y - storyNavigationOrigin.y};
    for (auto button = storyNavigationButtons.rbegin();
         button != storyNavigationButtons.rend(); ++button) {
        if (SDL_PointInRect(&point, &button->relativeBounds)) {
            return button->control;
        }
    }
    return StoryNavigationControl::None;
}

bool AppCore::activateStoryNavigation(StoryNavigationControl control) {
    std::string target;
    int mode = 0;
    if (control == StoryNavigationControl::Previous) {
        target = storyPreviousScene;
        mode = storyPreviousMode;
    } else if (control == StoryNavigationControl::Next) {
        // 0x00424dc0 is the desk's handler for interface hotspot 0x11e. When
        // the button is 0x7d2 and the persisted key-1 counter is still 0, it
        // closes the overlay (0x00421810), plays S02AN-NAR-002, bumps the
        // counter, and returns 1 -- so the click is consumed and the player
        // stays in the scene. The narration is Reodor explaining where to go
        // next, which is why it costs a click rather than accompanying one.
        //
        // Note the same counter is bumped at 0x00424cfa when S02AN-SS-007a
        // finishes and the game leaves for `pee`, so a player who visits the
        // toilet before ever pressing next never hears this. That is the
        // original's behaviour, not an accident to correct.
        if (activeScene == "desk" && deskNarrationPending()) {
            closeStoryNavigation();
            startSceneNarration("S02AN-NAR-002", true);
            consumeDeskNarrationCredit();
            return true;
        }
        target = storyNextScene;
        mode = storyNextMode;
    } else if (control == StoryNavigationControl::Help) {
        closeStoryNavigation();
        showSceneHelp();
        return true;
    } else if (control == StoryNavigationControl::Exit) {
        // The round arrow is control 0x7d4 (0x00421692, images exit_0-2).
        // The popup's handler 0x00421730 closes it (0x004217a0) and its
        // switch at 0x00421748 sends 0x7d4 to 0x00421781, a tail jump into
        // 0x0040cc30: leave for the active scene's registry parent, not for
        // where the scene was entered from. Every scene that builds this
        // popup (the 0x00421560 call sites) is registered with parent `menu`
        // (record +8), a front-end scene, so entry mode 0 and no prompt;
        // 0x0040cd70 asks only to abort a tournament round first.
        // tests/test_story_navigation_exit.py pins all of that.
        closeStoryNavigation();
        leaveToRegistryParent();
        return true;
    } else {
        return false;
    }
    if (target.empty()) {
        return false;
    }
    closeStoryNavigation();
    enterScene(target, target, mode);
    return true;
}

void AppCore::showSceneHelp() {
    const Package *helpPackage = nullptr;
    const Package::Entry *helpEntry = nullptr;
    if (sceneLanguagePackage) {
        helpEntry = sceneLanguagePackage->findOptional("help.ini");
        if (helpEntry) {
            helpPackage = sceneLanguagePackage.get();
        }
    }
    if (!helpEntry && scenePackage) {
        helpEntry = scenePackage->findOptional("help.ini");
        if (helpEntry) {
            helpPackage = scenePackage.get();
        }
    }
    if (!helpPackage || !helpEntry) {
        showMessage(language.get(
            "interfaceh", "NOHELP", "Hjelp er ikke tilgjengelig."));
        return;
    }
    auto pages = parseHelpPages(helpPackage->data(*helpEntry));
    if (pages.empty()) {
        showMessage(language.get(
            "interfaceh", "NOHELP", "Hjelp er ikke tilgjengelig."));
        return;
    }
    openHelpPages(std::move(pages));
}

void AppCore::showMenuHelp() {
    fs::path languageMenuPath =
        packagePath(gameDirectory, "Language1", "menu");
    if (!fs::exists(languageMenuPath)) {
        languageMenuPath = packagePath(gameDirectory, "Language", "menu");
    }
    if (!fs::exists(languageMenuPath)) {
        showMessage(language.get(
            "interfaceh", "NOHELP", "Hjelp er ikke tilgjengelig."));
        return;
    }
    const Package helpPackage(languageMenuPath);
    const auto *helpEntry = helpPackage.findOptional("help.ini");
    if (!helpEntry) {
        showMessage(language.get(
            "interfaceh", "NOHELP", "Hjelp er ikke tilgjengelig."));
        return;
    }
    auto pages = parseHelpPages(helpPackage.data(*helpEntry));
    if (pages.empty()) {
        showMessage(language.get(
            "interfaceh", "NOHELP", "Hjelp er ikke tilgjengelig."));
        return;
    }
    openHelpPages(std::move(pages));
}

SmackerKeying AppCore::sceneClipKeying(std::string_view scene,
                                       std::string_view name) const {
    const auto stemOf = [](std::string_view path) {
        const auto slash = path.find_last_of("\\/");
        if (slash != std::string_view::npos) {
            path.remove_prefix(slash + 1);
        }
        return path;
    };
    const std::string_view wanted = stemOf(name);
    const auto sameName = [&](const std::string &candidate) {
        const std::string_view stem = stemOf(candidate);
        return std::equal(stem.begin(), stem.end(), wanted.begin(),
                          wanted.end(),
                          [](char left, char right) {
                              return std::tolower(static_cast<unsigned char>(
                                         left)) ==
                                     std::tolower(static_cast<unsigned char>(
                                         right));
                          });
    };
    for (const auto &definition :
         executableGameData.directlyReferencedAnimations(scene)) {
        if (sameName(definition.name)) {
            return sceneKeying(definition);
        }
    }
    bool keyed = false;
    bool unkeyed = false;
    for (const auto &definition :
         executableGameData.animationDefinitionsNamed(name)) {
        if (definition.visible) {
            (definition.keyed ? keyed : unkeyed) = true;
        }
    }
    return keyed && !unkeyed ? SmackerKeying::TopLeftRgb565
                             : SmackerKeying::IndexZeroAndGreen;
}

DecodedMedia AppCore::decodeActiveSceneClip(std::string name,
                                            bool decodeVideo,
                                            bool decodeAudio) {
    const SmackerKeying keying = activeSceneClipKeying(name);
    return decodeSmacker(activeSceneAnimationData(std::move(name)),
                         decodeVideo, decodeAudio, true, 0, nullptr,
                         SmackerIndexZeroAlpha::Transparent, keying);
}

std::span<const std::uint8_t> AppCore::activeSceneAnimationData(std::string descriptorName) {
    const auto separator = descriptorName.find_last_of("\\/");
    if (separator != std::string::npos) {
        descriptorName.erase(0, separator + 1);
    }
    const std::string asset = "animation\\" + descriptorName + ".smk";
    if (descriptorName.rfind("blank", 0) == 0) {
        for (Package *package :
             {addonCommon.get(), data2Common.get(), editionCommon.get()}) {
            if (package) {
                if (const auto *entry = package->findOptional(asset)) {
                    return package->data(*entry);
                }
            }
        }
        return common.data(common.find(asset));
    }
    if (sceneLanguagePackage) {
        if (const auto *entry = sceneLanguagePackage->findOptional(asset)) {
            return sceneLanguagePackage->data(*entry);
        }
    }
    if (sceneAddonPackage) {
        if (const auto *entry = sceneAddonPackage->findOptional(asset)) {
            return sceneAddonPackage->data(*entry);
        }
    }
    if (sceneData2Package) {
        if (const auto *entry = sceneData2Package->findOptional(asset)) {
            return sceneData2Package->data(*entry);
        }
    }
    if (sceneEditionPackage) {
        if (const auto *entry = sceneEditionPackage->findOptional(asset)) {
            return sceneEditionPackage->data(*entry);
        }
    }
    if (scenePackage) {
        if (const auto *entry = scenePackage->findOptional(asset)) {
            return scenePackage->data(*entry);
        }
    }
    throw std::runtime_error("Scene animation asset not found: " + asset);
}

void AppCore::startSceneSequence(const std::vector<ExecutableGameData::AnimationDefinition> &definitions, std::string_view label) {
    releaseSceneFinalStage();
    const double requestedAt = sceneElapsedNow();
    const std::uint32_t previouslyHeldConflictMask =
        sceneSequenceHeldConflictMask;
    std::vector<SceneClip> nextClips;
    std::vector<DecodedMedia> primaryAudioTracks;
    std::vector<DecodedMedia> secondaryAudioTracks;
    DecodedMedia nextSpeechTimeline;
    DecodedMedia nextEffectTimeline;
    for (const auto &definition : definitions) {
        const auto assetData = activeSceneAnimationData(definition.name);
        DecodedMedia secondaryAudio;
        DecodedMedia media = decodeSmackerStreamed(
            assetData, definition.name, sceneKeying(definition),
            true, true, 0, &secondaryAudio);
        TexturePtr texture;
        if (definition.visible && !media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.metadata(0U);
            texture.reset(SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            frame.width,
                                            frame.height));
            if (!texture) {
                throw std::runtime_error(
                    std::string("SDL_CreateTexture failed: ") +
                    SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        }
        DecodedMedia primaryAudio;
        primaryAudio.audioRate = media.audioRate;
        primaryAudio.audioChannels = media.audioChannels;
        primaryAudio.audioSamples = std::move(media.audioSamples);
        primaryAudio.smackerAudioTrack = media.smackerAudioTrack;
        primaryAudioTracks.push_back(std::move(primaryAudio));
        secondaryAudioTracks.push_back(std::move(secondaryAudio));
        double duration = originalStoryClipCompletion(media);
        if (activeScene == "intent" && definition.name == "smoke") {
            // FGP's Smacker player holds this 201-frame effect for 13.59-
            // 13.67 s in two clean captures, while the encoded/FFmpeg
            // timeline ends at 13.399 s. Preserve the observed legacy
            // completion boundary so the scene does not accept the next
            // interaction about a quarter-second early. (The general story
            // clip model gives 13.57 s; this measured value stays.)
            duration = media.videoDuration + 0.240;
        }
        nextClips.push_back({definition.name,
                             definition.x,
                             definition.y,
                             false,
                             std::move(media),
                             duration,
                             std::move(texture),
                             definition.visible,
                             definition.layer,
                             definition.conflictMask});
    }
    if (nextClips.empty()) {
        return;
    }
    std::vector<double> nextStartOffsets = scheduleSceneClipStarts(
        nextClips, requestedAt, sceneSequenceHeldConflictMask);
    const double firstStartOffset = nextStartOffsets.front();
    for (std::size_t index = 0; index < nextClips.size(); ++index) {
        const double audioOffset =
            nextStartOffsets[index] - firstStartOffset;
        if (secondaryAudioTracks[index].audioSamples.empty()) {
            const bool effectsTrack =
                primaryAudioTracks[index].smackerAudioTrack == 0;
            mixAudioIntoAt(effectsTrack ? nextEffectTimeline
                                        : nextSpeechTimeline,
                           std::move(primaryAudioTracks[index]),
                           audioOffset);
        } else {
            mixAudioIntoAt(nextEffectTimeline,
                           std::move(primaryAudioTracks[index]),
                           audioOffset);
            mixAudioIntoAt(nextSpeechTimeline,
                           std::move(secondaryAudioTracks[index]),
                           audioOffset);
        }
    }
    releaseSceneHeldStage();
    sceneClips = std::move(nextClips);
    sceneClipStartOffsets = std::move(nextStartOffsets);
    sceneAudioTimeline = std::move(nextSpeechTimeline);
    sceneEffectTimeline = std::move(nextEffectTimeline);
    sceneUsesAudioTimeline = true;
    sceneSequenceBlocksInput = true;
    sceneSequenceFinished = false;
    sceneClipIndex = 0;
    sceneSequenceRequestedAt = requestedAt;
    sceneAudioTimelineStartedAt = 0.0;
    sceneClipTimelineStart = requestedAt + sceneClipStartOffsets.front();
    sceneClipStarted = false;
    sceneSequenceHeldConflictMask =
        previouslyHeldConflictMask & sceneClips.front().conflictMask;
    sceneSequencePendingConflictMask =
        sceneClips.front().conflictMask & ~sceneSequenceHeldConflictMask;
    referenceTrace.emit("diagnostic", {
        {"kind", "scene_pending_conflict"},
        {"clip_count", static_cast<std::uint64_t>(sceneClips.size())},
        {"conflict_mask", sceneSequencePendingConflictMask},
        {"first_start_offset_ms", static_cast<int>(std::llround(
            sceneClipStartOffsets.front() * 1000.0))},
    });
    const std::string title =
        "FGP Native — " + activeScene + " — " +
        std::string(label) + " — queued " +
        sceneClips.front().name;
    SDL_SetWindowTitle(window, title.c_str());
    for (const auto &clip : sceneClips) {
        referenceTrace.emit("animation_add", {{"asset", clip.name},
                                               {"x", clip.x},
                                               {"y", clip.y},
                                               {"layer", clip.layer},
                                               {"conflict_mask", clip.conflictMask}});
    }
}

void GameApp::handleSceneAnimationFinished(std::string_view name,
                                           double completedAt) {
    if (activeScene == "yard") {
        handleYardAnimationFinished(name, completedAt);
        return;
    }

    if (activeScene == "tvroom") {
        handleTvroomAnimationFinished(name, completedAt);
        return;
    }

    if (activeScene == "town") {
        handleTownAnimationFinished(name);
        return;
    }

    if (activeScene == "intro") {
        handleIntroAnimationFinished(name);
        return;
    }

    if (activeScene == "morning") {
        handleMorningAnimationFinished(name, completedAt);
        return;
    }

    if (activeScene == "outtent") {
        handleOuttentAnimationFinished(name);
        return;
    }

    if (activeScene != "intent") {
        return;
    }
    handleIntentAnimationFinished(name, completedAt);
}

void GameApp::handleSceneAnimationStarted(std::string_view name,
                                          double startedAt) {
    if (activeScene == "intent") {
        handleIntentAnimationStarted(name, startedAt);
    } else if (activeScene == "outtent") {
        handleOuttentAnimationStarted(name);
    }
}

// The yard's opening leaves its character groups unseeded (entry path 0 of
// 0x00428080); both natural completion and a skip must seed them, exactly
// as the original does when the opening ends.
void AppCore::seedOpeningCharacterGroups() {
    if (activeScene != "yard") {
        return;
    }
    // Space seeds both groups through 0x0040a880 (the yard-skip capture:
    // CharacterGroupActivate with deadline 0, then SOL-001 and LUD-001 right
    // after the first base clips). After a natural opening they are already
    // running and this changes nothing.
    setSceneCharacterGroupEnabled(251U, true, true);
    setSceneCharacterGroupEnabled(252U, true, true);
}

void AppCore::skipSceneForeground() {
    if (viewMode != ViewMode::Scene) {
        return;
    }

    // The logo is not skippable in the original. Its callback 0x004166e0
    // dispatches only three events -- 0x103 BeginScene to 0x00416730, 0x10c
    // KeyPress to 0x00416706, and 0x110 AnimFinished to 0x00416750 -- and the
    // key handler compares the key with 9 (VK_TAB) at 0x00416706, consumes it
    // and returns, with no VK_SPACE case and no transition anywhere in it. The
    // logo advances only when its animation finishes.
    //
    // Skipping it here therefore does two wrong things at once: it performs a
    // skip the original does not offer, and it clears sceneClips so
    // app_render.cpp's logo completion -- which needs sceneClipStarted, an
    // index one short of the size, and sceneClips[sceneClipIndex].duration --
    // can never fire again, leaving the logo on screen forever. Verified: with
    // a skip the run loads Data/logo.bin and nothing more; without one it goes
    // on to birthday.
    if (activeScene == "logo") {
        return;
    }

    // Intro does not use the ordinary scene-media unload path. Its key handler
    // at 0x004166c0 compares the key with VK_SPACE (0x20) and calls
    // GAME_GotoSceneByName("yard", 0) at 0x004166ce. The natural completion
    // callback independently takes S00AN-SS-001c to Yard at 0x004166a0.
    // Clearing sceneClips here used to make renderFrame's natural-completion
    // condition permanently false, leaving a skipped Intro stuck forever.
    if (activeScene == "intro") {
        enterScene("yard", "Yard");
        return;
    }

    // Morning's key handler 0x00425fb0 answers Space with the RIDEDOWN
    // question at once (0x00425fb7 jumps to 0x00425f40); nothing is skipped.
    if (activeScene == "morning") {
        if (dialogMode == DialogMode::None) {
            showMorningRideConfirmation();
        }
        return;
    }

    // Town's key handler 0x00427010 takes Space, like Escape, straight to the
    // mode's destination: Build-a-Car for mode 2 (0x00427038), otherwise
    // Outtent (0x0042702a).
    if (activeScene == "town") {
        if (activeSceneEntryMode == 2) {
            enterScene("buildacar", "buildacar", 0);
        } else {
            enterScene("outtent", "outtent", 0);
        }
        return;
    }

    // A skip discards only transient scene media. Background music lives on
    // the mixer's independent background channel, and persistent character
    // groups resume as soon as the foreground sequence is marked complete —
    // including the yard's, which stay unseeded until the opening is over.
    // Removal only: the original deletes the players (Yard 0x00428640, the
    // yard-skip capture) and reports no AnimFinished for them.
    releaseSceneHeldStage();
    for (std::size_t index = sceneClipIndex; index < sceneClips.size(); ++index) {
        referenceTrace.emit("animation_remove", {
            {"asset", sceneClips[index].name}, {"reason", "removed"}});
    }
    // Space does not merely end the sequence: the scenes' key handlers unload
    // the narration assets on VK_SPACE (yard 0x00428640-0x0042869a, desk
    // 0x00424cac-0x00424ce5, and the same pattern in pee 0x00426b50 and tvroom
    // 0x00427f60). So a skip silences narration rather than triggering the
    // sequence-completion narration, and this deliberately does not run the
    // 0x11f arming path in renderFrame.
    // Routed through the traced teardown rather than clearing the fields here:
    // an untraced unload leaves the comparator unable to see that the skip
    // silenced narration at all, which is exactly the path this change makes
    // possible. The reason distinguishes it from leaving the scene.
    stopSceneNarration("skipped");
    releaseSceneFinalStage();
    // The three key handlers restore the music on every Space, whether or
    // not anything was ducked: Intent 0x00425cde (which also removes its
    // timer 0 at 0x00425d40), the TV room 0x0042794c and Outtent 0x00426574.
    if (activeScene == "intent" || activeScene == "tvroom" ||
        activeScene == "outtent") {
        setMusicDuck(1.0F);
    }
    if (activeScene == "intent") {
        intentGroupRestoreAt.reset();
    }
    sceneClips.clear();
    sceneClipStartOffsets.clear();
    sceneClipIndex = 0;
    sceneSequenceBlocksInput = false;
    sceneSequenceFinished = true;
    sceneClipStarted = false;
    sceneSequenceHeldConflictMask = 0;
    sceneSequencePendingConflictMask = 0;
    sceneTimedTransition.reset();
    audioMixer.clearForeground();
    seedOpeningCharacterGroups();
    if (activeScene == "yard") {
        setYardMailboxVisible(false);
    } else if (activeScene == "outtent") {
        // 0x00426560: VK_SPACE removes the binocular view, the bike, the
        // banana and the paper, whichever are in the scene.
        for (auto &bitmap : sceneBitmapLayers) {
            if (!bitmap.visible) {
                continue;
            }
            bitmap.visible = false;
            referenceTrace.emit("animation_remove", {
                {"asset", bitmap.name == "backdrop2" ? std::string()
                                                     : bitmap.name},
                {"reason", "removed"}});
        }
        // ...and S09AN-SS-D01a while it plays (0x004265f1).
        for (auto &object : sceneAnimationObjects) {
            if (object.name != "S09AN-SS-D01a" || !object.playing) {
                continue;
            }
            object.playing = false;
            object.visible = false;
            if (object.audioEffect != 0U) {
                audioMixer.removeEffect(object.audioEffect);
                object.audioEffect = 0U;
            }
            referenceTrace.emit("animation_remove", {
                {"asset", object.name}, {"reason", "removed"}});
        }
    }

    // Space can arrive immediately after scene_ready, before the first
    // blocked character-group update has observed the opening clip. The
    // original releases those persistent groups when it discards the
    // foreground sequence, so make that release explicit instead of relying
    // on one render-loop turn having happened first.
    const double releasedAt = sceneElapsedNow();
    if (activeScene == "tvroom") {
        // VK_SPACE restores the three baseline tables at
        // 0x00427962/0x00427978/0x0042798B, even if the alternate set was
        // installed when SS002a completed.
        replaceSceneCharacterGroupTables(
            0x00000004U, 0x000c4834U, 0x000c3eb4U, 0x000c34a0U,
            releasedAt);
        replaceSceneCharacterGroupTables(
            0x00000002U, 0x000c4818U, 0x000c3ea0U, 0x000c34a8U,
            releasedAt);
        replaceSceneCharacterGroupTables(
            0x00000001U, 0x000c484cU, 0x000c3ec4U, 0U, releasedAt);
    } else if (activeScene == "intent") {
        // Intent's key handler has the same baseline reset pattern at
        // 0x00425CF4/0x00425D0D/0x00425D26.
        replaceSceneCharacterGroupTables(
            0x00000400U, 0x000bd364U, 0x000bd3acU, 0U, releasedAt);
        replaceSceneCharacterGroupTables(
            0x00000100U, 0x000bd37cU, 0x000bd388U, 0x000bd3c0U,
            releasedAt);
        replaceSceneCharacterGroupTables(
            0x00000200U, 0x000bd370U, 0x000bd398U, 0x000bd3d4U,
            releasedAt);
    }
    reseedSceneCharacterGroups(releasedAt);
    updateSceneCharacterGroups(releasedAt);

    hoveredHotspot = hotspotAt(mouseX, mouseY);
    hoveredBounds = currentHotspotBounds(hoveredHotspot);
    const std::string title =
        "FGP Native — " + activeScene +
        " — foreground skipped · exit control back";
    SDL_SetWindowTitle(window, title.c_str());
}

void AppCore::playSceneSound(std::uint32_t descriptorRva, std::string_view label) {
    const auto definition = executableGameData.animationAtRva(descriptorRva);
    DecodedMedia media = decodeSmacker(
        activeSceneAnimationData(definition.name), false, true);
    DecodedMedia silence;
    audioMixer.setForeground(silence, 0.0F, &media, clipAudioVolume(media));
    SDL_PauseAudioDevice(audioDevice, musicEnabled ? 0 : 1);
    const std::string title =
        "FGP Native — " + activeScene + " — " + std::string(label) + " (" +
        definition.name + ") · exit control back";
    SDL_SetWindowTitle(window, title.c_str());
    referenceTrace.emit("audio_start", {{"asset", definition.name},
                                         {"role", "effect"}});
}

void AppCore::playStandaloneNarration(std::string_view asset,
                                      std::string_view label) {
    DecodedMedia narration = decodeSmacker(
        activeSceneAnimationData(std::string(asset)), false, true);
    sceneAudioTimeline = std::move(narration);
    sceneEffectTimeline = {};
    sceneUsesAudioTimeline = true;
    audioMixer.setForeground(
        sceneAudioTimeline,
        audioVolumes[1] * sceneAudioGains[1],
        &sceneEffectTimeline,
        audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    const std::string title =
        "FGP Native — " + activeScene + " — " + std::string(label) +
        " — " + std::string(asset);
    SDL_SetWindowTitle(window, title.c_str());
}

bool GameApp::executeDeveloperCommand(std::string_view command) {
    if (!developmentMode || pendingScene) {
        return false;
    }
    // sleep:MS holds the command queue for MS milliseconds, so a scripted
    // run can reach a point inside a timed sequence (a screenshot of Town's
    // reel, say) without input.
    if (command.rfind("sleep:", 0) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!developerSleepStartedAt) {
            developerSleepStartedAt = now;
        }
        const auto wanted = std::chrono::milliseconds(
            std::stoll(std::string(command.substr(6))));
        if (now - *developerSleepStartedAt < wanted) {
            return false;
        }
        developerSleepStartedAt.reset();
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    constexpr std::string_view helpPrefix = "help:";
    if (command.rfind(helpPrefix, 0) == 0) {
        const std::string_view control = command.substr(helpPrefix.size());
        if (control == "open") {
            if (dialogMode != DialogMode::None) {
                return false;
            }
            if (viewMode == ViewMode::Menu) {
                showMenuHelp();
            } else if (viewMode == ViewMode::Scene) {
                showSceneHelp();
            } else {
                return false;
            }
        } else {
            if (dialogMode != DialogMode::Help || helpPages.empty()) {
                return false;
            }
            if (control == "previous") {
                if (helpTopicIndex > 0) {
                    selectHelpTopic(helpTopicIndex - 1U);
                }
            } else if (control == "next") {
                if (helpTopicIndex + 1U < helpPages.size()) {
                    selectHelpTopic(helpTopicIndex + 1U);
                }
            } else if (control == "scroll-up") {
                scrollHelpBody(-1);
            } else if (control == "scroll-down") {
                scrollHelpBody(1);
            } else if (control == "page-up") {
                scrollHelpBody(-std::max(
                    1, 292 / std::max(dialogFont.height, 1)));
            } else if (control == "page-down") {
                scrollHelpBody(std::max(
                    1, 292 / std::max(dialogFont.height, 1)));
            } else if (control.rfind("topic:", 0) == 0) {
                const auto index = static_cast<std::size_t>(std::stoul(
                    std::string(control.substr(6))));
                if (index >= helpPages.size()) {
                    return false;
                }
                selectHelpTopic(index);
            } else if (control == "close") {
                closeDialog();
            } else {
                throw std::runtime_error(
                    "Unknown developer help control: " +
                    std::string(control));
            }
        }
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    constexpr std::string_view minigamePrefix = "minigame:";
    constexpr std::string_view scenePrefix = "scene:";
    if (command.rfind(scenePrefix, 0) == 0) {
        std::string requested(command.substr(scenePrefix.size()));
        if (requested.empty()) {
            throw std::runtime_error("Developer scene name cannot be empty");
        }
        if (requested == "tourneyscore-fixture") {
            // The inventory capture's round: the active profile alone in a
            // Wheelbarrow tournament, scoring 0.
            const std::string player =
                profileIndex < profiles.size() &&
                        profiles[profileIndex] != "No profile"
                    ? profiles[profileIndex]
                    : std::string("Test");
            tournament = TournamentState{};
            tournament.profile = player;
            tournament.players = {{player, 1}};
            tournament.games = {{"wheelbarrow", 0U,
                                 subgameTitle("wheelbarrow", "")}};
            tournament.scores = {{{player, 0U, 0U}}};
            tournament.active = true;
            requested = "tourneyscore";
        }
        enterScene(requested, requested);
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    if (command.rfind(minigamePrefix, 0) == 0) {
        const std::string_view requested =
            command.substr(minigamePrefix.size());
        const auto *definition = minigameDefinition(requested);
        if (!definition) {
            throw std::runtime_error(
                "Unknown minigame shell: " + std::string(requested));
        }
        enterScene(std::string(definition->scene), definition->scene);
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    // The main menu's racing button and screenshots work from the menu (and
    // Settings) too.
    if (command == "racemenu:enter") {
        if (!executeRacemenuDeveloperCommand(command)) {
            return false;
        }
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    if (command.rfind("screenshot:", 0) == 0 &&
        (viewMode == ViewMode::Menu || viewMode == ViewMode::Options)) {
        developerScreenshotPath = std::string(command.substr(11));
        std::cout << "Developer control API: " << command << '\n';
        return true;
    }
    // Raw input works in every view, so the menu and Settings can be
    // scripted too; everything else needs a scene.
    const bool rawInput = command.rfind("click:", 0) == 0 ||
                          command.rfind("drag:", 0) == 0 ||
                          command.rfind("key:", 0) == 0;
    if (viewMode != ViewMode::Scene && !rawInput) {
        return false;
    }
    constexpr std::string_view hotspotPrefix = "hotspot:";
    constexpr std::string_view narrationPrefix = "narration:";
    // arm-narration:NAME:DELAY schedules a clip the way a scene's timer does,
    // so the scheduler -- cancellation, and the overlay pausing the countdown
    // -- can be exercised without waiting out a 78-second opening.
    constexpr std::string_view armNarrationPrefix = "arm-narration:";
    constexpr std::string_view buildacarPrefix = "buildacar-stage:";
    constexpr std::string_view buildacarControlPrefix = "buildacar:";
    constexpr std::string_view cornerPrefix = "corner:";
    constexpr std::string_view navigatePrefix = "navigate:";
    constexpr std::string_view sceneIndexTabPrefix = "sceneindex-tab:";
    constexpr std::string_view audiopairsControlPrefix = "audiopairs:";
    constexpr std::string_view sockdrawerControlPrefix = "sockdrawer:";
    constexpr std::string_view butterflyControlPrefix = "butterfly:";
    constexpr std::string_view lettersortControlPrefix = "lettersort:";
    constexpr std::string_view mahjongControlPrefix = "mahjong:";
    constexpr std::string_view whackamoleControlPrefix = "whackamole:";
    constexpr std::string_view balloonhuntControlPrefix = "balloonhunt:";
    constexpr std::string_view pipelineControlPrefix = "pipeline:";
    constexpr std::string_view hopscotchControlPrefix = "hopscotch:";
    constexpr std::string_view beemazeControlPrefix = "beemaze:";
    constexpr std::string_view bugzzzControlPrefix = "bugzzz:";
    constexpr std::string_view wheelbarrowControlPrefix = "wheelbarrow:";
    constexpr std::string_view goodbyeControlPrefix = "goodbye:";
    constexpr std::string_view outroControlPrefix = "outro:";
    constexpr std::string_view buildabikeControlPrefix = "buildabike:";
    constexpr std::string_view mountainControlPrefix = "mountain-game:";
    constexpr std::string_view hustleControlPrefix = "hustle:";
    constexpr std::string_view chessControlPrefix = "chess:";
    constexpr std::string_view jigsawControlPrefix = "jigsaw:";
    constexpr std::string_view slidingControlPrefix = "sliding:";
    constexpr std::string_view bouquetControlPrefix = "bouquet:";
    constexpr std::string_view fenceControlPrefix = "fence:";
    constexpr std::string_view puzzleControlPrefix = "puzzle:";
    constexpr std::string_view textinvaderControlPrefix = "textinvader:";
    constexpr std::string_view synonymControlPrefix = "synonym:";
    constexpr std::string_view mathlabControlPrefix = "mathlab:";
    constexpr std::string_view triviaControlPrefix = "trivia:";
    constexpr std::string_view recoveredControlPrefix = "recovered:";
    constexpr std::string_view colorfillControlPrefix = "colorfill:";
    constexpr std::string_view activitypageControlPrefix = "activity:";
    if (command.rfind("gallery:", 0) == 0 || command.rfind("info:", 0) == 0 ||
        command.rfind("movieplayer:", 0) == 0 ||
        command.rfind("anaglyph:", 0) == 0) {
        if (!executeViewerDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(colorfillControlPrefix, 0) == 0) {
        if (!executeColorfillDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(activitypageControlPrefix, 0) == 0) {
        if (!executeActivitypageDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(whackamoleControlPrefix, 0) == 0) {
        if (!executeWhackamoleDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(balloonhuntControlPrefix, 0) == 0) {
        if (!executeBalloonhuntDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(wheelbarrowControlPrefix, 0) == 0) {
        if (!executeWheelbarrowDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(goodbyeControlPrefix, 0) == 0) {
        if (!executeGoodbyeDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(outroControlPrefix, 0) == 0) {
        if (!executeOutroDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(hustleControlPrefix, 0) == 0) {
        if (!executeHustleDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(chessControlPrefix, 0) == 0) {
        if (!executeChessDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(jigsawControlPrefix, 0) == 0) {
        if (!executeJigsawDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(slidingControlPrefix, 0) == 0) {
        if (!executeSlidingDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(bouquetControlPrefix, 0) == 0) {
        if (!executeBouquetDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(fenceControlPrefix, 0) == 0) {
        if (!executeFenceDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(puzzleControlPrefix, 0) == 0) {
        if (!executePuzzleDeveloperCommand(command)) {
            return false;
        }
    } else if (command == "key:escape") {
        // A real Escape key press, through the ordinary event path.
        for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
            SDL_Event key{};
            key.type = type;
            key.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            key.key.keysym.sym = SDLK_ESCAPE;
            key.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
            SDL_PushEvent(&key);
        }
    } else if (command == "back") {
        performWebBack(); // the browser build's Back button
    } else if (command.rfind("screenshot:", 0) == 0) {
        developerScreenshotPath = std::string(command.substr(11));
    } else if (command == "character-points" ||
               command == "character-points:first") {
        // For capture scenarios: the logical point each character group
        // covers in the most frames of its base clips (or, with :first, of
        // its first base clip), never covered by another group's base, special or reaction clips.
        // Overlapping characters stack by the engine's active list
        // (0x00554958, appended on each add and qsorted by layer), which
        // changes as clips are re-added, so an overlap is not a stable target.
        const bool firstOnly = command == "character-points:first";
        const auto opaquePoints = [](const SceneClip &clip,
                                     std::size_t index, auto &&visit) {
            const auto &frame = clip.media.videoFrames[index];
            for (int y = 0; y < frame.height; y += 2) {
                for (int x = 0; x < frame.width; x += 2) {
                    const std::size_t alpha =
                        (static_cast<std::size_t>(y) * frame.width + x) *
                            4U + 3U;
                    if (alpha < frame.bgra.size() && frame.bgra[alpha] != 0) {
                        visit(std::pair{clip.x + x, clip.y + y});
                    }
                }
            }
        };
        for (const auto &group : sceneCharacterGroups) {
            std::set<std::pair<int, int>> others;
            for (const auto &other : sceneCharacterGroups) {
                if (&other == &group) {
                    continue;
                }
                for (const auto *clips :
                     {&other.baseClips, &other.category2Clips,
                      &other.category3Clips}) {
                    for (const auto &clip : *clips) {
                        for (std::size_t index = 0;
                             index < clip.media.videoFrames.size();
                             ++index) {
                            opaquePoints(clip, index, [&](auto point) {
                                others.insert(point);
                            });
                        }
                    }
                }
            }
            std::map<std::pair<int, int>, std::size_t> covered;
            std::size_t frames = 0;
            for (const auto &clip : group.baseClips) {
                if (firstOnly && &clip != &group.baseClips.front()) {
                    break;
                }
                for (std::size_t index = 0;
                     index < clip.media.videoFrames.size(); ++index) {
                    ++frames;
                    opaquePoints(clip, index, [&](auto point) {
                        if (!others.contains(point)) {
                            ++covered[point];
                        }
                    });
                }
            }
            std::pair<int, int> best{};
            std::size_t bestCount = 0;
            for (const auto &[point, count] : covered) {
                if (count > bestCount) {
                    best = point;
                    bestCount = count;
                }
            }
            std::cout << "Character point " << activeScene << ' '
                      << group.hotspot << ' '
                      << (group.baseClips.empty()
                              ? std::string()
                              : group.baseClips.front().name + ' ')
                      << best.first << ','
                      << best.second << " in " << bestCount << '/' << frames
                      << " frames\n";
        }
    } else if (command.rfind("wait-until:", 0) == 0) {
        // Holds the command queue until SECONDS after launch.
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - developerRunStart)
                   .count() >= std::stod(std::string(command.substr(11)));
    } else if (command.rfind("click:", 0) == 0) {
        // Queues a left click at logical X,Y (window coordinates when the
        // window is the logical 800x600): motion, press, release.
        const std::string at(command.substr(6));
        const auto comma = at.find(',');
        if (comma == std::string::npos) return false;
        const int x = std::stoi(at.substr(0, comma));
        const int y = std::stoi(at.substr(comma + 1));
        SDL_Event motion{};
        motion.type = SDL_MOUSEMOTION;
        motion.motion.windowID = SDL_GetWindowID(window);
        motion.motion.x = x;
        motion.motion.y = y;
        SDL_PushEvent(&motion);
        for (const Uint32 type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
            SDL_Event event{};
            event.type = type;
            event.button.windowID = SDL_GetWindowID(window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.state =
                type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.button.clicks = 1;
            event.button.x = x;
            event.button.y = y;
            SDL_PushEvent(&event);
        }
    } else if (command.rfind("drag:", 0) == 0) {
        // Queues a left-button drag from logical X1,Y1 to X2,Y2: press,
        // eight motion steps with the button held, release.
        std::array<int, 4> at{};
        std::size_t start = 5;
        for (std::size_t index = 0; index < at.size(); ++index) {
            const std::size_t end = command.find(',', start);
            if ((end == std::string_view::npos) != (index == at.size() - 1)) {
                return false;
            }
            at[index] = std::stoi(std::string(command.substr(
                start, end == std::string_view::npos ? end : end - start)));
            start = end + 1;
        }
        const auto push = [&](Uint32 type, int x, int y) {
            SDL_Event event{};
            event.type = type;
            if (type == SDL_MOUSEMOTION) {
                event.motion.windowID = SDL_GetWindowID(window);
                event.motion.state = SDL_BUTTON_LMASK;
                event.motion.x = x;
                event.motion.y = y;
            } else {
                event.button.windowID = SDL_GetWindowID(window);
                event.button.button = SDL_BUTTON_LEFT;
                event.button.state =
                    type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                event.button.clicks = 1;
                event.button.x = x;
                event.button.y = y;
            }
            SDL_PushEvent(&event);
        };
        push(SDL_MOUSEMOTION, at[0], at[1]);
        push(SDL_MOUSEBUTTONDOWN, at[0], at[1]);
        constexpr int kSteps = 8;
        for (int step = 1; step <= kSteps; ++step) {
            push(SDL_MOUSEMOTION, at[0] + (at[2] - at[0]) * step / kSteps,
                 at[1] + (at[3] - at[1]) * step / kSteps);
        }
        push(SDL_MOUSEBUTTONUP, at[2], at[3]);
    } else if (command.rfind("key:", 0) == 0) {
        // Queues a key press as if typed: key:space, key:escape, key:down...
        const SDL_Keycode key =
            SDL_GetKeyFromName(std::string(command.substr(4)).c_str());
        if (key == SDLK_UNKNOWN) return false;
        for (const Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
            SDL_Event event{};
            event.type = type;
            event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.key.keysym.sym = key;
            event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
            event.key.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&event);
        }
    } else if (command.rfind("tournament:", 0) == 0) {
        if (!executeTournamentDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind("mainscore:", 0) == 0) {
        if (!executeMainscoreDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind("racing:", 0) == 0) {
        if (!executeRacingDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind("racemenu:", 0) == 0) {
        if (!executeRacemenuDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind("buttercol:", 0) == 0) {
        if (!executeButtercolDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind("draughts:", 0) == 0) {
        if (!executeDraughtsDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(mathlabControlPrefix, 0) == 0) {
        if (!executeMathlabDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(triviaControlPrefix, 0) == 0) {
        if (!executeTriviaDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(synonymControlPrefix, 0) == 0) {
        if (!executeSynonymDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(textinvaderControlPrefix, 0) == 0) {
        if (!executeTextinvaderDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(mountainControlPrefix, 0) == 0) {
        if (!executeMountainDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(buildabikeControlPrefix, 0) == 0) {
        if (!executeBuildabikeDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(bugzzzControlPrefix, 0) == 0) {
        if (!executeBugzzzDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(beemazeControlPrefix, 0) == 0) {
        if (!executeBeemazeDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(hopscotchControlPrefix, 0) == 0) {
        if (!executeHopscotchDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(pipelineControlPrefix, 0) == 0) {
        if (!executePipelineDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(recoveredControlPrefix, 0) == 0) {
        if (!executeRecoveredMinigameDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(hotspotPrefix, 0) == 0) {
        if (sceneSequenceBlocksInput) {
            return false;
        }
        developerHotspotOverride = static_cast<std::uint32_t>(
            std::stoul(std::string(command.substr(hotspotPrefix.size()))));
        SDL_Event click{};
        click.type = SDL_MOUSEBUTTONDOWN;
        click.button.button = SDL_BUTTON_LEFT;
        click.button.x = kLogicalWidth / 2;
        click.button.y = kLogicalHeight / 2;
        if (SDL_PushEvent(&click) < 0) {
            throw std::runtime_error(
                std::string("SDL_PushEvent failed: ") + SDL_GetError());
        }
    } else if (command.rfind(armNarrationPrefix, 0) == 0) {
        const std::string request(command.substr(armNarrationPrefix.size()));
        const std::size_t separator = request.find_last_of(':');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "arm-narration expects NAME:DELAY_SECONDS");
        }
        armSceneNarration(request.substr(0, separator),
                          std::stod(request.substr(separator + 1)), false);
    } else if (command.rfind(narrationPrefix, 0) == 0) {
        playStandaloneNarration(command.substr(narrationPrefix.size()),
                                "developer narration control");
    } else if (command.rfind(sceneIndexTabPrefix, 0) == 0) {
        if (activeScene != "sceneindex") {
            return false;
        }
        const std::string_view requested =
            command.substr(sceneIndexTabPrefix.size());
        static constexpr std::array<std::string_view, 4> names{
            "storypage", "subgame", "activity", "gold"};
        const auto found = std::find(names.begin(), names.end(), requested);
        if (found == names.end()) {
            throw std::runtime_error(
                "Unknown Contents tab: " + std::string(requested));
        }
        selectSceneIndexTab(static_cast<std::size_t>(
            std::distance(names.begin(), found)));
    } else if (command.rfind(audiopairsControlPrefix, 0) == 0) {
        if (!executeAudiopairsDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(sockdrawerControlPrefix, 0) == 0) {
        if (!executeSockdrawerDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(butterflyControlPrefix, 0) == 0) {
        if (!executeButterflyDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(lettersortControlPrefix, 0) == 0) {
        if (!executeLettersortDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(mahjongControlPrefix, 0) == 0) {
        if (!executeMahjongDeveloperCommand(command)) {
            return false;
        }
    } else if (command.rfind(buildacarPrefix, 0) == 0) {
        const auto stage = static_cast<std::uint32_t>(std::stoul(
            std::string(command.substr(buildacarPrefix.size()))));
        if (!playBuildacarStageNarration(stage)) {
            return false;
        }
    } else if (command.rfind(buildacarControlPrefix, 0) == 0) {
        if (!executeBuildacarDeveloperCommand(command)) {
            return false;
        }
    } else if (command == "skip") {
        // The same path as the Space key, so scripted runs can exercise
        // what a skipped opening leaves behind.
        skipSceneForeground();
    } else if (command.rfind(cornerPrefix, 0) == 0) {
        const std::string_view control = command.substr(cornerPrefix.size());
        if (control == "help") {
            showSceneHelp();
        } else if (control == "exit") {
            returnFromScene();
        } else {
            throw std::runtime_error(
                "Unknown developer corner control: " +
                std::string(control));
        }
    } else if (command.rfind(navigatePrefix, 0) == 0) {
        const std::string_view control = command.substr(navigatePrefix.size());
        if (control == "open") {
            if (!openStoryNavigation(kLogicalWidth / 2,
                                     kLogicalHeight / 2)) {
                return false;
            }
        } else {
            StoryNavigationControl requested = StoryNavigationControl::None;
            if (control == "previous") {
                requested = StoryNavigationControl::Previous;
            } else if (control == "next") {
                requested = StoryNavigationControl::Next;
            } else if (control == "help") {
                requested = StoryNavigationControl::Help;
            } else if (control == "exit") {
                requested = StoryNavigationControl::Exit;
            } else {
                throw std::runtime_error(
                    "Unknown developer navigation control: " +
                    std::string(control));
            }
            if (!activateStoryNavigation(requested)) {
                return false;
            }
        }
    } else if (command == "morning:ride") {
        if (activeScene != "morning") {
            return false;
        }
        sceneSequenceBlocksInput = false;
        sceneSequenceFinished = true;
        sceneClipStarted = false;
        sceneSequenceHeldConflictMask = 0;
        sceneSequencePendingConflictMask = 0;
        audioMixer.clearForeground();
        showMorningRideConfirmation();
    } else if (command == "morning:continue" ||
               command == "morning:mountain") {
        if (activeScene != "morning" ||
            dialogMode != DialogMode::Message ||
            dialogMessageKind != MessageKind::MorningRideConfirmation) {
            return false;
        }
        closeDialog();
        if (command == "morning:continue") {
            enterScene("town", "town", 0);
        } else {
            enterScene("mountain", "mountain");
        }
    } else {
        throw std::runtime_error(
            "Unknown developer command: " + std::string(command));
    }
    std::cout << "Developer control API: " << command << '\n';
    return true;
}

void AppCore::startSceneAnimation(std::uint32_t descriptorRva, std::string_view label) {
    startSceneSequence(
        std::vector<ExecutableGameData::AnimationDefinition>{
            executableGameData.animationAtRva(descriptorRva)},
        label);
}

void AppCore::hideSceneCollectible(std::string_view name) {
    const auto found = std::find_if(
        sceneCollectibles.begin(), sceneCollectibles.end(),
        [&](const SceneCollectible &collectible) {
            return collectible.name == name;
        });
    if (found != sceneCollectibles.end()) {
        found->visible = false;
    }
}

bool AppCore::playSceneAnimationObject(std::string_view name) {
    const auto found = std::find_if(
        sceneAnimationObjects.begin(), sceneAnimationObjects.end(),
        [&](const SceneAnimationObject &object) {
            return object.name == name;
        });
    if (found == sceneAnimationObjects.end()) {
        return false;
    }
    found->visible = true;
    found->playing = true;
    found->completed = false;
    found->startedAt = animationTimeNow();
    if (!found->audio.audioSamples.empty()) {
        found->audioEffect = audioMixer.addEffect(
            found->audio, audioVolumes[0] * sceneAudioGains[0], found->loop);
    }
    referenceTrace.emit("animation_start", {{"asset", found->name},
                                             {"x", found->x},
                                             {"y", found->y},
                                             {"layer", found->layer},
                                             {"loop", false}});
    return true;
}

void AppCore::setSceneCharacterGroupEnabled(std::uint32_t hotspot, bool enabled,
                                            bool seededDeadlineExpired) {
    const auto found = std::find_if(
        sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
        [&](const SceneCharacterGroup &group) {
            return group.hotspot == hotspot;
        });
    if (found != sceneCharacterGroups.end()) {
        if (!enabled || !found->enabled || !found->baseClipActivated) {
            clearSceneCharacterHold(*found);
            found->category3Pending = false;
            found->category3Playing = false;
        }
        if (enabled && (!found->enabled || !found->baseClipActivated)) {
            found->baseClipIndex = 0;
            found->category2ClipIndex = 0;
            clearSceneCharacterCategory4(*found);
            found->baseClipStartedAt = sceneElapsedNow();
            found->category2DeadlineAt = 0.0;
            found->baseClipActivated = true;
            found->baseClipPlaying = false;
            found->category2Playing = false;
            found->category4Playing = false;
            // 0x0040a8d0 seeds a group without touching its special
            // deadline, which binding (0x0040a2b0) left at 0: the first
            // base clip is then followed by a special at once.
            found->category2DeadlineArmed =
                seededDeadlineExpired && !found->category2Clips.empty();
            found->conflictSeen = false;
            found->conflictBlocked = false;
        } else if (!enabled) {
            found->baseClipActivated = false;
            found->baseClipPlaying = false;
            found->category2Playing = false;
            clearSceneCharacterCategory4(*found);
            found->category2DeadlineArmed = false;
            found->conflictSeen = false;
            found->conflictBlocked = false;
        }
        found->enabled = enabled;
    }
}

std::vector<AppCore::SceneClip> AppCore::decodeSceneCharacterTable(
    std::uint32_t tableRva, std::uint32_t conflictMask) {
    std::vector<SceneClip> clips;
    if (tableRva == 0U) {
        return clips;
    }
    for (const auto &definition :
         executableGameData.animationSequenceAtRva(tableRva)) {
        DecodedMedia media = decodeSmackerStreamed(
            activeSceneAnimationData(definition.name), definition.name,
            sceneKeying(definition), false);
        TexturePtr texture;
        if (!media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.metadata(0U);
            texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!texture) {
                throw std::runtime_error(
                    std::string("SDL_CreateTexture failed: ") +
                    SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        }
        const double duration = originalStoryClipCompletion(media);
        clips.push_back({definition.name,
                         definition.x,
                         definition.y,
                         false,
                         std::move(media),
                         duration,
                         std::move(texture),
                         definition.visible,
                         definition.layer,
                         conflictMask});
    }
    return clips;
}

void AppCore::clearSceneCharacterCategory4(SceneCharacterGroup &group) {
    if (group.category4TableIndex.has_value() &&
        *group.category4TableIndex < group.category4Tables.size()) {
        std::vector<SceneClip>().swap(
            group.category4Tables[*group.category4TableIndex].clips);
    }
    group.category4Playing = false;
    group.category4ClipIndex = 0;
    group.category4TableIndex.reset();
}

void AppCore::clearSceneCharacterHold(SceneCharacterGroup &group) {
    group.heldClip = nullptr;
    std::vector<SceneClip>().swap(group.heldCategory4Clips);
}

bool AppCore::requestSceneCharacterReaction(std::uint32_t hotspot) {
    // The group scheduler's own mouse handler 0x0040a980, registered at
    // priority -10 by 0x0040a910 from each scene initializer, so it sees a
    // press only when the scene callback did not take it. 0x0040a9a0 ->
    // 0x0040aa10 finds the group bound to the hotspot (+0x5c), and
    // 0x0040aa50 marks it pending category 3 when its next category is the
    // base one (+0x4c == 1) and its category-3 table is not empty (+0x30),
    // then sets the wait cursor (0x0040aa79).
    if (hotspot == 0U) {
        return false;
    }
    for (auto &group : sceneCharacterGroups) {
        if (group.hotspot != hotspot || !group.enabled) {
            continue;
        }
        if (group.category3Clips.empty() || !group.baseClipActivated ||
            group.category3Pending ||
            ((sceneSequencePendingConflictMask |
              sceneSequenceHeldConflictMask) & group.conflictMask) != 0U) {
            return false;
        }
        group.category3Pending = true;
        referenceTrace.emit("diagnostic", {
            {"kind", "character_category3_request"},
            {"hotspot", hotspot},
            {"asset", group.category3Clips[group.category3ClipIndex].name}});
        return true;
    }
    return false;
}

void AppCore::reseedSceneCharacterGroups(double timelineTime) {
    // Every story callback returns 0 for VK_SPACE, so the key reaches the
    // scheduler's handler: 0x0040a9cc -> 0x0040acc0 removes the foreground
    // stages (0x0040ace0) and seeds every bound group (0x0040a880 ->
    // 0x0040a8d0). Seeding sets the next category to base (+0x4c = 1,
    // dropping a pending reaction), stops the current clip (0x00412a60) and
    // starts the base table's current entry at once (0x0040a7c0), whose index
    // already moved past the stopped clip when that clip was installed. It
    // leaves the special deadline alone: still 0, expired, if nothing armed
    // it this visit. Read for these scenes' callbacks with the key fixed at
    // 0x20 (the Escape scan's interpreter); the minigames with groups are
    // not covered by that reading.
    static constexpr std::array<std::string_view, 9> kSpacePassesToGroups{
        "yard", "desk", "pee", "house", "tvroom",
        "garage", "outtent", "intent", "tvstation"};
    if (std::find(kSpacePassesToGroups.begin(), kSpacePassesToGroups.end(),
                  activeScene) == kSpacePassesToGroups.end()) {
        for (auto &group : sceneCharacterGroups) {
            group.conflictSeen = group.enabled && !group.baseClips.empty();
            group.conflictBlocked = group.conflictSeen;
        }
        return;
    }
    for (auto &group : sceneCharacterGroups) {
        if (!group.enabled || group.baseClips.empty()) {
            continue;
        }
        clearSceneCharacterHold(group);
        if (group.baseClipActivated && group.baseClipPlaying) {
            if (const auto *clip = activeSceneCharacterClip(group)) {
                referenceTrace.emit("animation_stop", {
                    {"asset", clip->name}, {"reason", "stopped"}});
            }
            if (group.category4Playing) {
                clearSceneCharacterCategory4(group);
            } else if (group.category3Playing) {
                group.category3ClipIndex = (group.category3ClipIndex + 1U) %
                                           group.category3Clips.size();
            } else if (group.category2Playing) {
                group.category2ClipIndex = (group.category2ClipIndex + 1U) %
                                           group.category2Clips.size();
            } else {
                group.baseClipIndex =
                    (group.baseClipIndex + 1U) % group.baseClips.size();
            }
        }
        group.category2Playing = false;
        group.category3Playing = false;
        group.category3Pending = false;
        clearSceneCharacterCategory4(group);
        group.baseClipActivated = true;
        group.conflictSeen = false;
        group.conflictBlocked = false;
        if (!group.category2DeadlineEverArmed &&
            !group.category2Clips.empty()) {
            group.category2DeadlineArmed = true;
            group.category2DeadlineAt = 0.0;
        }
        group.baseClipPlaying = true;
        group.baseClipStartedAt = timelineTime;
        if (const auto *clip = activeSceneCharacterClip(group)) {
            referenceTrace.emit("animation_start", {
                {"asset", clip->name}, {"x", clip->x}, {"y", clip->y},
                {"layer", clip->layer}, {"loop", false}});
        }
    }
}

bool AppCore::sceneCharacterReactionBlocksInput() const {
    // Cursor mode 1 ("wait", 0x0040aa79) from the press, mode 0 while the
    // reaction plays (0x0040a83b), and the normal cursor again when it
    // finishes (0x0040ac12 passes NULL to 0x0040c370). The cursor gate
    // 0x0040f210 swallows clicks in modes 0 and 1.
    return std::any_of(
        sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
        [](const SceneCharacterGroup &group) {
            return group.enabled &&
                   (group.category3Pending || group.category3Playing);
        });
}

const AppCore::SceneClip *AppCore::activeSceneCharacterClip(
    const SceneCharacterGroup &group) const {
    if (group.category4Playing) {
        if (!group.category4TableIndex.has_value() ||
            *group.category4TableIndex >= group.category4Tables.size()) {
            return nullptr;
        }
        const auto &clips =
            group.category4Tables[*group.category4TableIndex].clips;
        return group.category4ClipIndex < clips.size()
                   ? &clips[group.category4ClipIndex]
                   : nullptr;
    }
    if (group.category3Playing) {
        return group.category3ClipIndex < group.category3Clips.size()
                   ? &group.category3Clips[group.category3ClipIndex]
                   : nullptr;
    }
    if (group.category2Playing) {
        return group.category2ClipIndex < group.category2Clips.size()
                   ? &group.category2Clips[group.category2ClipIndex]
                   : nullptr;
    }
    return group.baseClipIndex < group.baseClips.size()
               ? &group.baseClips[group.baseClipIndex]
               : nullptr;
}

void AppCore::replaceSceneCharacterGroupTables(
    std::uint32_t conflictMask, std::uint32_t baseTableRva,
    std::uint32_t category2TableRva, std::uint32_t category3TableRva,
    double timelineTime) {
    const auto found = std::find_if(
        sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
        [&](const SceneCharacterGroup &group) {
            return group.conflictMask == conflictMask;
        });
    if (found == sceneCharacterGroups.end()) {
        throw std::runtime_error("Character-table replacement has no group");
    }

    const bool alreadyCurrent =
        found->baseTableRva == baseTableRva &&
        found->category2TableRva == category2TableRva &&
        found->category3TableRva == category3TableRva;
    if (!alreadyCurrent) {
        const auto stored = std::find_if(
            found->storedTables.begin(), found->storedTables.end(),
            [&](const SceneCharacterGroup::StoredTables &tables) {
                return tables.baseTableRva == baseTableRva &&
                       tables.category2TableRva == category2TableRva &&
                       tables.category3TableRva == category3TableRva;
            });
        if (stored == found->storedTables.end()) {
            throw std::runtime_error("Character table set is not registered");
        }
        clearSceneCharacterHold(*found);
        // Only one table set per group is live. Release the outgoing decoded
        // frames before materializing the incoming set; its RVAs remain in the
        // registry and can be decoded again if a later restore selects it.
        std::vector<SceneClip>().swap(found->baseClips);
        std::vector<SceneClip>().swap(found->category2Clips);
        std::vector<SceneClip>().swap(found->category3Clips);
        found->baseTableRva = baseTableRva;
        found->category2TableRva = category2TableRva;
        found->category3TableRva = category3TableRva;
        found->baseClips =
            decodeSceneCharacterTable(baseTableRva, conflictMask);
        found->category2Clips =
            decodeSceneCharacterTable(category2TableRva, conflictMask);
        found->category3Clips =
            decodeSceneCharacterTable(category3TableRva, conflictMask);
    }
    found->category3ClipIndex = 0;
    found->category3Pending = false;
    found->category3Playing = false;

    // 0x0040A420 resets each installed table's index/state words even when a
    // caller reinstalls the same addresses.
    found->baseClipIndex = 0;
    found->category2ClipIndex = 0;
    found->baseClipStartedAt = timelineTime;
    found->baseClipPlaying = false;
    found->category2Playing = false;
    clearSceneCharacterCategory4(*found);
    found->category2DeadlineArmed = false;
    referenceTrace.emit("diagnostic", {
        {"kind", "CharacterGroupTables"},
        {"group_bit", conflictMask},
        {"base_table", baseTableRva},
        {"category2_table", category2TableRva},
        {"category3_table", category3TableRva},
    });
}

void AppCore::startSceneCharacterCategory4(std::uint32_t conflictMask,
                                            std::uint32_t tableRva,
                                            double timelineTime) {
    const auto found = std::find_if(
        sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
        [&](const SceneCharacterGroup &group) {
            return group.conflictMask == conflictMask;
        });
    if (found == sceneCharacterGroups.end()) {
        throw std::runtime_error("Category-4 trigger has no character group");
    }
    auto table = std::find_if(
        found->category4Tables.begin(), found->category4Tables.end(),
        [&](const SceneCharacterGroup::Category4Table &candidate) {
            return candidate.tableRva == tableRva;
        });
    if (table == found->category4Tables.end()) {
        throw std::runtime_error("Category-4 table is not registered");
    }
    if (const auto *playing = activeSceneCharacterClip(*found);
        found->baseClipPlaying && playing != nullptr) {
        referenceTrace.emit("animation_stop", {
            {"asset", playing->name}, {"reason", "replaced"}});
    }
    clearSceneCharacterCategory4(*found);
    table->clips = decodeSceneCharacterTable(tableRva, conflictMask);
    // The executable-derived table test covers every registered category-4
    // table. They are all one-entry one-shots; fail loudly if that changes.
    if (table->clips.size() != 1U) {
        throw std::runtime_error(
            "Category-4 character table must contain exactly one clip");
    }
    found->category4TableIndex = static_cast<std::size_t>(
        std::distance(found->category4Tables.begin(), table));
    found->category4ClipIndex = 0;
    found->category2Playing = false;
    found->category4Playing = true;
    found->baseClipPlaying = false;
    found->baseClipActivated = true;
    found->baseClipStartedAt = timelineTime;
    updateSceneCharacterGroups(timelineTime);
}

double AppCore::sceneElapsedNow() {
    if (dialogClockFrozen()) {
        return helpFrozenElapsed;
    }
    if (paused) {
        return pausedAt;
    }
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - start)
        .count();
}

std::vector<double> AppCore::scheduleSceneClipStarts(
    const std::vector<SceneClip> &clips, double requestedAt,
    std::uint32_t initialConflictMask) const {
    // A direct one-clip response starts at the latest currently playing
    // conflicting character boundary. Do not project the following base
    // clips: the original holds each group once its current clip completes
    // while it waits for the other acquired bits.
    if (clips.size() == 1U) {
        double startAt = requestedAt;
        const std::uint32_t acquiredConflictMask =
            clips.front().conflictMask & ~initialConflictMask;
        for (const auto &group : sceneCharacterGroups) {
            if (!group.enabled || !group.baseClipPlaying ||
                group.baseClips.empty() ||
                (group.conflictMask & acquiredConflictMask) == 0U) {
                continue;
            }
            const auto *active = activeSceneCharacterClip(group);
            if (active != nullptr) {
                startAt = std::max(
                    startAt, group.baseClipStartedAt + active->duration);
            }
        }
        return {startAt - requestedAt};
    }

    struct ProjectedGroup {
        const SceneCharacterGroup *definition{};
        std::size_t baseClipIndex{};
        std::size_t category2ClipIndex{};
        std::size_t category4ClipIndex{};
        std::optional<std::size_t> category4TableIndex;
        double clipStartedAt{};
        double category2DeadlineAt{};
        bool activated{};
        bool playing{};
        bool category2Playing{};
        bool category4Playing{};
        bool category2DeadlineArmed{};
        std::size_t category3ClipIndex{};
        bool category3Pending{};
        bool category3Playing{};
    };

    std::vector<ProjectedGroup> groups;
    groups.reserve(sceneCharacterGroups.size());
    for (const auto &group : sceneCharacterGroups) {
        groups.push_back({&group, group.baseClipIndex,
                          group.category2ClipIndex,
                          group.category4ClipIndex,
                          group.category4TableIndex,
                          group.baseClipStartedAt,
                          group.category2DeadlineAt,
                          group.baseClipActivated,
                          group.baseClipPlaying,
                          group.category2Playing,
                          group.category4Playing,
                          group.category2DeadlineArmed,
                          group.category3ClipIndex,
                          group.category3Pending,
                          group.category3Playing});
    }

    const auto advanceTo = [](ProjectedGroup &group, double timelineTime,
                              std::uint32_t blockedMask) {
        if (!group.activated || !group.playing ||
            !group.definition->enabled ||
            group.definition->baseClips.empty() ||
            (group.definition->conflictMask & blockedMask) != 0U) {
            return;
        }
        const auto activeDuration = [&]() {
            if (group.category4Playing &&
                group.category4TableIndex.has_value()) {
                return group.definition
                    ->category4Tables[*group.category4TableIndex]
                    .clips[group.category4ClipIndex]
                    .duration;
            }
            if (group.category3Playing) {
                return group.definition
                    ->category3Clips[group.category3ClipIndex]
                    .duration;
            }
            return group.category2Playing
                       ? group.definition
                             ->category2Clips[group.category2ClipIndex]
                             .duration
                       : group.definition
                             ->baseClips[group.baseClipIndex]
                             .duration;
        };
        while (timelineTime - group.clipStartedAt >= activeDuration()) {
            group.clipStartedAt += activeDuration();
            if (group.category4Playing) {
                group.category4Playing = false;
                group.category4ClipIndex = 0;
                group.category4TableIndex.reset();
            } else if (group.category3Playing) {
                group.category3Playing = false;
                group.category3ClipIndex =
                    (group.category3ClipIndex + 1U) %
                    group.definition->category3Clips.size();
                group.category2DeadlineArmed = false;
            } else if (group.category2Playing) {
                group.category2Playing = false;
                group.category2ClipIndex =
                    (group.category2ClipIndex + 1U) %
                    group.definition->category2Clips.size();
                // A special completion rearms with a fresh random delay at
                // runtime. One such interval cannot expire within the tested
                // response, so leave it unarmed in this future projection.
                group.category2DeadlineArmed = false;
            } else {
                group.baseClipIndex =
                    (group.baseClipIndex + 1U) %
                    group.definition->baseClips.size();
                if (!group.category3Pending &&
                    group.category2DeadlineArmed &&
                    !group.definition->category2Clips.empty() &&
                    group.clipStartedAt >= group.category2DeadlineAt) {
                    group.category2Playing = true;
                    group.category2DeadlineArmed = false;
                }
            }
            if (group.category3Pending) {
                group.category3Pending = false;
                group.category2Playing = false;
                group.category3Playing = true;
            }
        }
    };

    std::vector<double> starts;
    starts.reserve(clips.size());
    double scheduledAt = requestedAt;
    std::uint32_t heldMask = initialConflictMask;
    for (const auto &clip : clips) {
        for (auto &group : groups) {
            advanceTo(group, scheduledAt, heldMask);
        }

        const std::uint32_t releasedMask = heldMask & ~clip.conflictMask;
        for (auto &group : groups) {
            if (!group.definition->enabled ||
                group.definition->baseClips.empty() ||
                (group.definition->conflictMask & releasedMask) == 0U) {
                continue;
            }
            group.activated = true;
            if (!group.playing) {
                group.playing = true;
                group.category2Playing = false;
                group.clipStartedAt = scheduledAt;
            }
        }

        const std::uint32_t acquiredMask =
            clip.conflictMask & ~heldMask;
        double releaseAt = scheduledAt;
        for (const auto &group : groups) {
            if (!group.activated || !group.playing ||
                !group.definition->enabled ||
                group.definition->baseClips.empty() ||
                (group.definition->conflictMask & acquiredMask) == 0U) {
                continue;
            }
            releaseAt = std::max(
                releaseAt,
                group.clipStartedAt + [&]() {
                    if (group.category4Playing &&
                        group.category4TableIndex.has_value()) {
                        return group.definition
                            ->category4Tables[*group.category4TableIndex]
                            .clips[group.category4ClipIndex]
                            .duration;
                    }
                    if (group.category3Playing) {
                        return group.definition
                            ->category3Clips[group.category3ClipIndex]
                            .duration;
                    }
                    return group.category2Playing
                               ? group.definition
                                     ->category2Clips[
                                         group.category2ClipIndex]
                                     .duration
                               : group.definition
                                     ->baseClips[group.baseClipIndex]
                                     .duration;
                }());
        }

        const std::uint32_t retainedMask =
            heldMask & clip.conflictMask;
        for (auto &group : groups) {
            advanceTo(group, releaseAt, retainedMask);
        }
        scheduledAt = releaseAt;
        for (auto &group : groups) {
            if ((group.definition->conflictMask & acquiredMask) == 0U) {
                continue;
            }
            group.playing = false;
            group.category2Playing = false;
            group.category3Playing = false;
            group.category3Pending = false;
            group.category4Playing = false;
            group.category4TableIndex.reset();
        }

        starts.push_back(scheduledAt - requestedAt);
        heldMask = clip.conflictMask;
        scheduledAt += clip.duration;
        for (auto &group : groups) {
            advanceTo(group, scheduledAt, heldMask);
        }
    }
    return starts;
}

void AppCore::armSceneCharacterDeadline(SceneCharacterGroup &group,
                                        double timelineTime) {
    if (group.category2Clips.empty()) {
        group.category2DeadlineArmed = false;
        return;
    }
    std::uniform_int_distribution<int> delayMilliseconds(15000, 24999);
    group.category2DeadlineAt =
        timelineTime + delayMilliseconds(randomEngine) / 1000.0;
    group.category2DeadlineArmed = true;
    group.category2DeadlineEverArmed = true;
    referenceTrace.emit("diagnostic", {
        {"kind", "character_category_deadline"},
        {"hotspot", group.hotspot},
        {"deadline_ms", static_cast<int>(
             group.category2DeadlineAt * 1000.0)}});
}

void AppCore::acquireSceneCharacterConflicts(
    std::uint32_t conflictMask, double timelineTime) {
    for (auto &group : sceneCharacterGroups) {
        if ((group.conflictMask & conflictMask) != 0U) {
            // 0x0040a660 removes the group's current player, finished or
            // not, before it adds the stage.
            clearSceneCharacterHold(group);
        }
        if (!group.enabled || !group.baseClipPlaying ||
            (group.conflictMask & conflictMask) == 0U) {
            continue;
        }
        const auto *clip = activeSceneCharacterClip(group);
        if (clip == nullptr) {
            continue;
        }
        referenceTrace.emit("animation_stop", {
            {"asset", clip->name}, {"reason", "conflict"}});
        if (timelineTime - group.baseClipStartedAt >= clip->duration) {
            if (group.category4Playing) {
                clearSceneCharacterCategory4(group);
            } else if (group.category3Playing) {
                group.category3ClipIndex =
                    (group.category3ClipIndex + 1U) %
                    group.category3Clips.size();
                armSceneCharacterDeadline(
                    group, group.baseClipStartedAt + clip->duration);
            } else if (group.category2Playing) {
                group.category2ClipIndex =
                    (group.category2ClipIndex + 1U) %
                    group.category2Clips.size();
                armSceneCharacterDeadline(
                    group, group.baseClipStartedAt + clip->duration);
            } else {
                group.baseClipIndex =
                    (group.baseClipIndex + 1U) % group.baseClips.size();
            }
        }
        group.baseClipPlaying = false;
        group.category2Playing = false;
        group.category3Playing = false;
        group.category3Pending = false;
        clearSceneCharacterCategory4(group);
        group.conflictSeen = true;
        group.conflictBlocked = true;
    }
}

double AppCore::sceneCharacterConflictReleaseAt(
    std::uint32_t conflictMask, double timelineTime) const {
    double releaseAt = timelineTime;
    for (const auto &group : sceneCharacterGroups) {
        if (!group.enabled || !group.baseClipPlaying ||
            (group.conflictMask & conflictMask) == 0U) {
            continue;
        }
        const auto *clip = activeSceneCharacterClip(group);
        if (clip != nullptr) {
            releaseAt = std::max(
                releaseAt, group.baseClipStartedAt + clip->duration);
        }
    }
    return releaseAt;
}

void AppCore::releaseSceneFinalStage() {
    if (!sceneFinalStageHeld) {
        return;
    }
    sceneFinalStageHeld = false;
    if (!sceneClips.empty()) {
        referenceTrace.emit("animation_remove", {
            {"asset", sceneClips.back().name}, {"reason", "removed"}});
    }
}

void AppCore::releaseSceneHeldStage() {
    if (!sceneHeldStageIndex) {
        return;
    }
    if (*sceneHeldStageIndex < sceneClips.size()) {
        referenceTrace.emit("animation_remove", {
            {"asset", sceneClips[*sceneHeldStageIndex].name},
            {"reason", "removed"}});
    }
    sceneHeldStageIndex.reset();
}

void AppCore::updateSceneCharacterGroups(double timelineTime) {
    if (sceneFinalStageHeld && !sceneClips.empty()) {
        const std::uint32_t heldMask = sceneClips.back().conflictMask;
        for (const auto &group : sceneCharacterGroups) {
            if (group.enabled && !group.baseClips.empty() &&
                (group.conflictMask & heldMask) != 0U) {
                // The group's next clip replaces the frozen stage.
                releaseSceneFinalStage();
                break;
            }
        }
    }
    for (auto &group : sceneCharacterGroups) {
        if (!group.enabled || group.baseClips.empty()) {
            group.baseClipPlaying = false;
            clearSceneCharacterHold(group);
            continue;
        }
        const bool blocked =
            (group.conflictMask & sceneSequenceHeldConflictMask) != 0U;
        if (blocked) {
            // The stage that holds this bit has replaced the group's player.
            clearSceneCharacterHold(group);
            group.conflictSeen = true;
            group.conflictBlocked = true;
            if (group.baseClipPlaying) {
                const auto *clip = activeSceneCharacterClip(group);
                if (clip != nullptr &&
                    timelineTime - group.baseClipStartedAt >= clip->duration) {
                    referenceTrace.emit("animation_stop", {
                        {"asset", clip->name}, {"reason", "conflict"}});
                    if (group.category4Playing) {
                        clearSceneCharacterCategory4(group);
                    } else if (group.category3Playing) {
                        group.category3ClipIndex =
                            (group.category3ClipIndex + 1U) %
                            group.category3Clips.size();
                        armSceneCharacterDeadline(
                            group, group.baseClipStartedAt + clip->duration);
                    } else if (group.category2Playing) {
                        group.category2Playing = false;
                        group.category2ClipIndex =
                            (group.category2ClipIndex + 1U) %
                            group.category2Clips.size();
                        armSceneCharacterDeadline(
                            group, group.baseClipStartedAt + clip->duration);
                    } else {
                        group.baseClipIndex =
                            (group.baseClipIndex + 1U) %
                            group.baseClips.size();
                    }
                }
                group.baseClipPlaying = false;
                group.category2Playing = false;
                group.category3Playing = false;
                group.category3Pending = false;
                clearSceneCharacterCategory4(group);
            }
            continue;
        }
        if (group.conflictBlocked) {
            armSceneCharacterDeadline(group, timelineTime);
            group.conflictBlocked = false;
        }
        if (!group.baseClipActivated) {
            if (!group.conflictSeen) {
                continue;
            }
            group.baseClipActivated = true;
        }
        if (!group.baseClipPlaying &&
            (group.conflictMask &
             sceneSequencePendingConflictMask) != 0U) {
            continue;
        }
        if (!group.baseClipPlaying) {
            clearSceneCharacterHold(group);
            group.baseClipPlaying = true;
            group.category2Playing = false;
            group.baseClipStartedAt = timelineTime;
            if (const auto *clip = activeSceneCharacterClip(group)) {
                referenceTrace.emit("animation_start", {
                    {"asset", clip->name}, {"x", clip->x}, {"y", clip->y},
                    {"layer", clip->layer}, {"loop", false}});
            }
        }
        const auto activeClip = [&]() -> const SceneClip * {
            return activeSceneCharacterClip(group);
        };
        while (activeClip() != nullptr &&
               timelineTime - group.baseClipStartedAt >=
                   activeClip()->duration) {
            const auto *finished = activeClip();
            referenceTrace.emit("animation_stop", {
                {"asset", finished->name}, {"reason", "completed"}});
            group.baseClipStartedAt += finished->duration;
            // Swapping keeps `finished` valid when it is a category-4 clip.
            std::vector<SceneClip> finishedCategory4;
            if (group.category4Playing) {
                if (group.category4TableIndex.has_value() &&
                    *group.category4TableIndex <
                        group.category4Tables.size()) {
                    finishedCategory4.swap(
                        group.category4Tables[*group.category4TableIndex]
                            .clips);
                }
                clearSceneCharacterCategory4(group);
            } else if (group.category3Playing) {
                // 0x0040abf0 on a finished category-3 clip: 0x0040ac12
                // restores the cursor and rearms the special deadline.
                group.category3Playing = false;
                group.category3ClipIndex =
                    (group.category3ClipIndex + 1U) %
                    group.category3Clips.size();
                armSceneCharacterDeadline(group,
                                          group.baseClipStartedAt);
            } else if (group.category2Playing) {
                group.category2Playing = false;
                group.category2ClipIndex =
                    (group.category2ClipIndex + 1U) %
                    group.category2Clips.size();
                armSceneCharacterDeadline(group,
                                          group.baseClipStartedAt);
            } else {
                group.baseClipIndex =
                    (group.baseClipIndex + 1U) % group.baseClips.size();
                // 0x0040ac25 looks at the special deadline only when the
                // next category is the base one, so a pending reaction
                // wins over an expired deadline, which stays armed.
                if (!group.category3Pending &&
                    group.category2DeadlineArmed &&
                    !group.category2Clips.empty() &&
                    group.baseClipStartedAt >=
                        group.category2DeadlineAt) {
                    group.category2Playing = true;
                    group.category2DeadlineArmed = false;
                }
            }
            const bool stagePending =
                (group.conflictMask &
                 sceneSequencePendingConflictMask) != 0U;
            if (group.category3Pending && !stagePending) {
                // 0x0040a7c0 with category 3 plays the table's current
                // entry; 0x0040a580 advances the index afterwards.
                group.category3Pending = false;
                group.category2Playing = false;
                group.category3Playing = true;
            }
            if (stagePending) {
                // A stage request overwrites the pending category
                // (0x0040a5d0 writes kind 5 to +0x4c).
                group.category3Pending = false;
                referenceTrace.emit("diagnostic", {
                    {"kind", "character_conflict_hold"},
                    {"hotspot", group.hotspot},
                    {"conflict_mask", group.conflictMask},
                });
                group.baseClipPlaying = false;
                group.category2Playing = false;
                clearSceneCharacterCategory4(group);
                group.conflictSeen = true;
                group.conflictBlocked = true;
                group.heldCategory4Clips = std::move(finishedCategory4);
                group.heldClip = finished;
                group.category3Playing = false;
                break;
            }
            referenceTrace.emit("animation_start", {
                {"asset", activeClip()->name},
                {"x", activeClip()->x}, {"y", activeClip()->y},
                {"layer", activeClip()->layer},
                {"loop", false}});
        }
    }
}

std::pair<const AppCore::SceneClip *, std::size_t>
AppCore::sceneCharacterPoseAt(const SceneCharacterGroup &group,
                              double timelineTime) {
    if (!group.baseClipPlaying || group.baseClips.empty()) {
        return {nullptr, 0U};
    }
    const auto *clip = activeSceneCharacterClip(group);
    if (clip == nullptr) {
        return {nullptr, 0U};
    }
    const double localTime =
        std::max(0.0, timelineTime - group.baseClipStartedAt);
    return {clip,
            frameIndexAt(clip->media, localTime, false)};
}

bool AppCore::sceneCharacterVisible(const SceneCharacterGroup &group) {
    if (viewMode != ViewMode::Scene || !group.enabled ||
        !group.baseClipPlaying ||
        group.baseClips.empty()) {
        return false;
    }
    if (!sceneSequenceFinished) {
        return (sceneSequenceHeldConflictMask & group.conflictMask) == 0;
    }
    return true;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>> GameApp::sceneDynamicHotspotAt(int x, int y) {
    if (const auto lettersort = lettersortHotspotAt(x, y)) {
        return lettersort;
    }
    if (const auto audiopairs = audiopairsHotspotAt(x, y)) {
        return audiopairs;
    }
    if (const auto sockdrawer = sockdrawerHotspotAt(x, y)) {
        return sockdrawer;
    }
    if (const auto butterfly = butterflyHotspotAt(x, y)) {
        return butterfly;
    }
    if (const auto whackamole = whackamoleHotspotAt(x, y)) {
        return whackamole;
    }
    if (const auto recovered = recoveredMinigameHotspotAt(x, y)) {
        return recovered;
    }
    const double timelineTime = sceneElapsedNow();
    // Later-registered players win if two opaque animation pixels
    // overlap, matching the scene player's topmost-first hit test.
    for (auto group = sceneCharacterGroups.rbegin();
         group != sceneCharacterGroups.rend(); ++group) {
        if (!sceneCharacterVisible(*group)) {
            continue;
        }
        const auto [clip, frameIndex] =
            sceneCharacterPoseAt(*group, timelineTime);
        if (clip == nullptr) {
            continue;
        }
        if (!clip->visible || frameIndex >= clip->media.videoFrames.size()) {
            continue;
        }
        const auto &frame = clip->media.videoFrames[frameIndex];
        const int localX = x - clip->x;
        const int localY = y - clip->y;
        if (localX < 0 || localY < 0 || localX >= frame.width ||
            localY >= frame.height) {
            continue;
        }
        const std::size_t alpha =
            (static_cast<std::size_t>(localY) * frame.width + localX) *
                4U +
            3U;
        if (alpha < frame.bgra.size() && frame.bgra[alpha] != 0) {
            return std::pair{
                group->hotspot,
                SDL_Rect{clip->x, clip->y, frame.width, frame.height}};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>> AppCore::sceneAnimationObjectAt(int x, int y) {
    if (viewMode != ViewMode::Scene) {
        return std::nullopt;
    }
    const double timelineTime = sceneElapsedNow();
    for (auto object = sceneAnimationObjects.rbegin();
         object != sceneAnimationObjects.rend(); ++object) {
        if (!object->visible || object->media.videoFrames.empty()) {
            continue;
        }
        const double localTime = timelineTime - object->startedAt;
        if (object->playing && object->hideOnComplete &&
            localTime >= object->media.videoDuration) {
            continue;
        }
        const std::size_t frameIndex = object->playing
                                           ? frameIndexAt(
                                                 object->media,
                                                 localTime,
                                                 false)
                                           : 0U;
        if (frameIndex >= object->media.videoFrames.size()) {
            continue;
        }
        const auto &frame = object->media.videoFrames[frameIndex];
        const int localX = x - object->x;
        const int localY = y - object->y;
        if (localX < 0 || localY < 0 || localX >= frame.width ||
            localY >= frame.height) {
            continue;
        }
        const std::size_t alpha =
            (static_cast<std::size_t>(localY) * frame.width + localX) *
                4U +
            3U;
        if (alpha < frame.bgra.size() && frame.bgra[alpha] != 0) {
            return std::pair{
                object->hotspot,
                SDL_Rect{object->x, object->y, frame.width,
                         frame.height}};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, SDL_Rect>> AppCore::sceneCollectibleAt(int x, int y) {
    if (viewMode != ViewMode::Scene) {
        return std::nullopt;
    }
    for (auto collectible = sceneCollectibles.rbegin();
         collectible != sceneCollectibles.rend(); ++collectible) {
        if (!collectible->visible || !collectible->surface) {
            continue;
        }
        const int localX = x - collectible->x;
        const int localY = y - collectible->y;
        if (opaqueSurfacePixel(collectible->surface.get(), localX,
                               localY)) {
            return std::pair{
                collectible->hotspot,
                SDL_Rect{collectible->x, collectible->y,
                         collectible->surface->w,
                         collectible->surface->h}};
        }
    }
    return std::nullopt;
}

// The executable's rand() has RAND_MAX 32767; the recovered formulas scale
// raw rand() values, so the same range is reproduced here.
int AppCore::particleRandomValue() {
    return std::uniform_int_distribution<int>(0, 32767)(randomEngine);
}

int AppCore::particleRandom(int bound) {
    return bound == 0 ? 0 : particleRandomValue() % bound;
}

// Narration scheduling.
//
// The original does not lay narration onto a timeline. A narration clip is an
// ordinary 0xa8-byte descriptor with `smacker=1`, `visible=0` and a conflict
// mask of 0, and it reaches the player one of two ways:
//
//   0x00412b90(descriptor)  plays it immediately. It checks the descriptor's
//                           conflict mask at +0x60 against the active group
//                           slots, and because narration masks are 0 it never
//                           contends -- so the clip is heard concurrently with
//                           whatever sequence is running.
//   0x0040a770(descriptor)  writes the descriptor into the one-element table at
//                           0x0053dadc and starts it as a sequence, which
//                           REPLACES the running one.
//
// Scenes arm a clip with a one-shot timer (0x0040c480 with interval 0) and
// cancel every pending one on the first click or key press. Pre-mixing cannot
// express either behaviour, which is why narration used to play in a fixed
// procession on entry.
void AppCore::armSceneNarration(std::string_view name, double delaySeconds,
                                bool replacesSequenceInOriginal,
                                std::optional<double> armedAt) {
    const double timerBase = armedAt.value_or(animationTimeNow());
    DecodedMedia clip;
    try {
        // The original timer dispatches a descriptor whose package data is
        // already available. Native decode time is an implementation detail;
        // paying it after the deadline delayed NAR-003 by about 52 ms.
        clip = decodeSmacker(
            activeSceneAnimationData(std::string(name)), false, true);
    } catch (const std::exception &error) {
        std::cout << "Could not arm narration " << name << ": "
                  << error.what() << '\n';
        return;
    }
    pendingNarrations.push_back(
        PendingNarration{std::string(name), timerBase + delaySeconds,
                         std::move(clip),
                         replacesSequenceInOriginal});
    std::cout << "Armed narration " << name << " in " << delaySeconds << "s\n";
    // `diagnostic` rather than a new event name: the shared schema in
    // reference/trace.schema.json enumerates valid names, and an unknown one
    // aborts a capture rather than being ignored. Arming is native-side
    // bookkeeping with no counterpart in the original's trace, so it does not
    // deserve a schema change.
    referenceTrace.emit("diagnostic",
                        {{"kind", "narration_arm"},
                         {"asset", std::string(name)},
                         {"delay_seconds", delaySeconds},
                         {"replaces_sequence_in_original",
                          replacesSequenceInOriginal}});
}

void AppCore::startSceneNarration(std::string_view name,
                                  bool replacesSequenceInOriginal,
                                  std::optional<DecodedMedia> predecoded,
                                  std::optional<double> durationOverride) {
    if (predecoded.has_value()) {
        sceneNarrationClip = std::move(*predecoded);
    } else {
        try {
            // activeSceneAnimationData resolves against the packages of the
            // scene that is already running, which is what a mid-scene trigger
            // needs; loadScene's own lambda only sees packages being loaded.
            sceneNarrationClip = decodeSmacker(
                activeSceneAnimationData(std::string(name)), false, true);
        } catch (const std::exception &error) {
            std::cout << "Could not start narration " << name << ": "
                      << error.what() << '\n';
            return;
        }
    }
    sceneNarrationName = std::string(name);
    sceneNarrationStartedAt = animationTimeNow();
    sceneNarrationDuration = durationOverride.value_or(
        sceneNarrationClip.audioRate > 0 && sceneNarrationClip.audioChannels > 0
            ? static_cast<double>(sceneNarrationClip.audioSamples.size()) /
                  static_cast<double>(sceneNarrationClip.audioRate *
                                      sceneNarrationClip.audioChannels)
            : 0.0);
    // Its own mixer layer, so a concurrent narration does not cut the stage
    // audio the way replacing the speech bus would.
    audioMixer.setNarration(sceneNarrationClip,
                            clipAudioVolume(sceneNarrationClip));
    // The original starts some of these with 0x0040a770, which replaces the
    // running sequence. We always mix on the independent narration layer
    // instead, so the visual sequence continues underneath. That is a known
    // divergence rather than a faithful reproduction, and the log says which
    // primitive the original used rather than implying we used it.
    std::cout << "Narration " << name << " started on the narration layer"
              << (replacesSequenceInOriginal
                      ? " (original uses 0x0040a770, which would replace the"
                        " running sequence)"
                      : " (original uses 0x00412b90, concurrent)")
              << '\n';
    // The batch comparator aligns on these, so narration has to report the
    // same lifecycle a visual clip does or it is invisible to A/B runs.
    referenceTrace.emit("animation_add", {{"asset", sceneNarrationName},
                                         {"kind", "narration"}});
    // A narration descriptor sits at (0,0) on layer 0, which is what the
    // original's SCENE_AddAnim record reports for it.
    referenceTrace.emit("animation_start",
                        {{"asset", sceneNarrationName},
                         {"kind", "narration"},
                         {"x", 0}, {"y", 0}, {"layer", 0},
                         {"duration_seconds", sceneNarrationDuration},
                         {"loop", false}});
}

void AppCore::updatePendingNarrations() {
    const double now = animationTimeNow();
    // Report completion, so a native run emits the same stop/remove pair the
    // visual sequence does and the comparator can align on it.
    if (!sceneNarrationName.empty() && sceneNarrationDuration > 0.0 &&
        now >= sceneNarrationStartedAt + sceneNarrationDuration) {
        const std::string completedName = sceneNarrationName;
        referenceTrace.emit("animation_stop", {{"asset", sceneNarrationName},
                                              {"kind", "narration"},
                                              {"reason", "completed"}});
        if (activeScene == "tvroom" &&
            completedName == "S05AN-SS-003c" &&
            tvroomConcurrentFollowupPending) {
            // 0x004277a2: the music comes back up before 0x004277a8
            // removes the clip and the follow-up table starts.
            setMusicDuck(1.0F);
        }
        referenceTrace.emit("animation_remove", {{"asset", sceneNarrationName},
                                                {"kind", "narration"},
                                                {"reason", "completed"}});
        std::cout << "Narration " << sceneNarrationName << " finished\n";
        sceneNarrationName.clear();
        sceneNarrationDuration = 0.0;
        if (activeScene == "tvroom" &&
            completedName == "S05AN-SS-003c" &&
            tvroomConcurrentFollowupPending) {
            tvroomConcurrentFollowupPending = false;
            startSceneSequence(
                executableGameData.animationSequenceAtRva(0x000c2f2cU),
                "TV room SS-003c follow-up");
        }
    }
    if (activeScene == "intent" && intentGroupRestoreAt.has_value() &&
        now >= *intentGroupRestoreAt) {
        const double restoredAt = *intentGroupRestoreAt;
        intentGroupRestoreAt.reset();
        // 0x004258c9: timer 0 restores the music before the tables.
        setMusicDuck(1.0F);
        replaceSceneCharacterGroupTables(
            0x00000400U, 0x000bd364U, 0x000bd3acU, 0U, restoredAt);
        startSceneCharacterCategory4(
            0x00000400U, 0x000bd3e8U, restoredAt);
        replaceSceneCharacterGroupTables(
            0x00000200U, 0x000bd370U, 0x000bd398U,
            0x000bd3d4U, restoredAt);
        startSceneCharacterCategory4(
            0x00000200U, 0x000bd400U, restoredAt);
        replaceSceneCharacterGroupTables(
            0x00000100U, 0x000bd37cU, 0x000bd388U,
            0x000bd3c0U, restoredAt);
        startSceneCharacterCategory4(
            0x00000100U, 0x000bd418U, restoredAt);
    }
    if (pendingNarrations.empty()) {
        return;
    }
    // The navigate overlay suspends timer dispatch. Measured against the
    // original: a right-click after the desk's NAR-001 timer is armed does not
    // reach the cancel path at 0x00424c72, yet holding the overlay open for
    // twelve seconds still prevents NAR-001 from starting. So the overlay stops
    // the scene's timers from being serviced rather than dropping them.
    //
    // Settled by capture rather than assumption: the countdown PAUSES and its
    // remaining time is preserved. In the original the overlay opened 115 ms
    // after the desk's timer was armed, stayed open for 12.1 s with no cancel
    // and no narration, and NAR-001 then started 9.91 s after it closed --
    // which is the 10 s delay minus the 115 ms already elapsed. An
    // overdue-fires-immediately implementation would have started it at once
    // on close, so this is a real distinction and not bookkeeping.
    if (storyNavigationOpen) {
        if (narrationSuspendedAt == 0.0) {
            narrationSuspendedAt = now;
        }
        return;
    }
    if (narrationSuspendedAt != 0.0) {
        const double suspended = now - narrationSuspendedAt;
        for (auto &entry : pendingNarrations) {
            entry.dueSeconds += suspended;
        }
        std::cout << "Resumed " << pendingNarrations.size()
                  << " pending narration(s) after " << suspended
                  << "s of navigation overlay\n";
        narrationSuspendedAt = 0.0;
    }
    for (auto entry = pendingNarrations.begin();
         entry != pendingNarrations.end();) {
        if (entry->dueSeconds <= now) {
            PendingNarration due = std::move(*entry);
            entry = pendingNarrations.erase(entry);
            startSceneNarration(due.name, due.replacesSequenceInOriginal,
                                std::move(due.clip));
        } else {
            ++entry;
        }
    }
}

void AppCore::stopSceneNarration(std::string_view reason) {
    cancelPendingNarrations();
    // Reset the suspension before the early return, not after it. Leaving a
    // scene while the navigate overlay is open and a narration is pending takes
    // exactly that path: the pending vector is cleared here, so there is no
    // resume update to consume the timestamp, and a stale one would then shift
    // the NEXT scene's narration by however long ago that was -- pushing it far
    // enough out that it never fires.
    narrationSuspendedAt = 0.0;
    if (sceneNarrationName.empty()) {
        return;
    }
    std::cout << "Stopped narration " << sceneNarrationName << '\n';
    // A removed narration player reports no AnimFinished in the original
    // (Yard's Space removes S01AN-NAR-001 in the yard-skip capture), so the
    // trace records the removal alone.
    referenceTrace.emit("animation_remove", {{"asset", sceneNarrationName},
                                            {"kind", "narration"},
                                            {"reason", std::string(reason)}});
    audioMixer.clearNarration();
    sceneNarrationDuration = 0.0;
    sceneNarrationClip = DecodedMedia{};
    sceneNarrationName.clear();
}

bool AppCore::deskNarrationPending() {
    if (profiles[profileIndex] == "No profile") {
        // Without a profile there is nothing to persist against, so the
        // narration would repeat on every click. Treat it as already heard.
        return false;
    }
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex] + ".pfl");
    if (!fs::exists(profilePath)) {
        return false;
    }
    return profileSceneRecordU32(profilePath, "desk", 0x402U, 1U)
               .value_or(0U) == 0U;
}

void AppCore::consumeDeskNarrationCredit() {
    if (profiles[profileIndex] == "No profile") {
        return;
    }
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex] + ".pfl");
    try {
        incrementProfileSceneRecordU32(profilePath, "desk", 0x402U, 1U);
    } catch (const std::exception &error) {
        std::cout << "Could not persist the desk narration credit: "
                  << error.what() << '\n';
    }
}

void AppCore::cancelPendingNarrations() {
    if (pendingNarrations.empty()) {
        return;
    }
    // 0x004283e0 and 0x00424c60 drop the scene's pending narration timer as
    // soon as the player touches anything, so a narration the player has
    // already acted past is never heard.
    std::cout << "Cancelled " << pendingNarrations.size()
              << " pending narration(s) on player input\n";
    for (const auto &entry : pendingNarrations) {
        referenceTrace.emit("diagnostic", {{"kind", "narration_cancel"},
                                          {"asset", entry.name},
                                          {"reason", "player_input"}});
    }
    pendingNarrations.clear();
}

} // namespace fgp

namespace fgp {

// Each minigame's init ends, in a tournament, by starting play at once at
// the player's difficulty d (0 easy, 1 medium, 2 hard). The original calls
// and their difficulty use are listed per game; where the native port has
// no matching setting yet the game starts at its normal level.
void GameApp::startTournamentRound(int difficulty) {
    const auto start = [this](std::string_view command) {
        executeDeveloperCommand(command);
    };
    if (activeScene == "audiopairs" && audiopairsGame) {
        // 0x004290a0; level += 2d (0x004290ea) not applied yet.
        startAudiopairsGame();
    } else if (activeScene == "balloonhunt" && balloonhuntGame) {
        // 0x0042ad60(2, d): Competition from track d (not applied yet).
        startBalloonhuntGame(BalloonhuntMode::Competition);
    } else if (activeScene == "beemaze" && beemazeGame) {
        // 0x00432c90; one life and level 5d (0x00433890-region) not applied.
        startBeemazeGame();
    } else if (activeScene == "bugzzz" && bugzzzGame) {
        // 0x00433790; level += 2d (0x004337bd) not applied yet.
        startBugzzzGame();
    } else if (activeScene == "buildabike" && buildabikeGame) {
        // 0x00439620; row d of 0x0049e014 not applied yet.
        startBuildabikeGame();
    } else if (activeScene == "butterfly" && butterflyGame) {
        // 0x0043a820; level 3d (0x0043ba15) not applied yet.
        startButterflyGame();
    } else if (activeScene == "hopscotch" && hopscotchGame) {
        // 0x0043c310 -> 0x0043c3c0(3d) not applied yet.
        startHopscotchGame();
    } else if (activeScene == "hustle" && hustleGame) {
        // 0x00412b90(start_0); base level d (0x0043d598) not applied yet.
        start("hustle:start");
    } else if (activeScene == "lettersort" && lettersortGame) {
        // 0x0043dea0; level 3d at the clip's end (0x0043eadb) not applied.
        start("lettersort:start");
    } else if (activeScene == "mountain" && mountainGame) {
        // 0x0044108e with difficulty 0x00678afc = d.
        mountainGame->difficulty = difficulty;
        startMountainGame();
    } else if (activeScene == "pipeline" && pipelineGame) {
        pipelineStartGame(); // 0x00448880, no difficulty
    } else if (activeScene == "sockdrawer" && sockdrawerGame) {
        // 0x0044ba30; level 5d (0x0044b836) not applied yet.
        startSockdrawerGame();
    } else if (activeScene == "textinvader" && textinvaderGame) {
        // 0x0044c950 sets 0x00687500 = d.
        textinvaderGame->difficulty = difficulty;
        startTextinvaderGame();
    } else if (activeScene == "whackamole" && whackamoleGame) {
        startWhackamoleGame(); // 0x0044da80(0x22), no difficulty
    } else if (activeScene == "wheelbarrow" && wheelbarrowGame) {
        startWheelbarrowGame(difficulty * 3); // 0x0044f110(3d)
    } else if (activeScene == "gametrivia" && triviaGame) {
        triviaGame->difficulty = difficulty; // 0x0047e399 -> 0x006da3ac
        startTriviaGame();
    } else if (activeScene == "mathlab" && mathlabGame) {
        startMathlabGame(difficulty); // 0x0047ffd0(d)
    } else if (activeScene == "synonym" && synonymGame) {
        synonymGame->startingLevel = difficulty * 2; // 0x00482400(2d)
        startSynonymGame();
    }
}

void AppCore::startSceneAmbience(double now) {
    if (!sceneAmbienceDecoded) {
        for (std::size_t index = 0; index < sceneAmbienceTracks.size();
             ++index) {
            const std::string asset =
                "music\\ambience" + std::to_string(index + 1U) + ".smk";
            sceneAmbienceTracks[index] =
                decodeSmacker(common.data(common.find(asset)), true, true);
        }
        sceneAmbienceDecoded = true;
    }
    std::uniform_int_distribution<std::size_t> pick(
        0U, sceneAmbienceTracks.size() - 1U);
    const std::size_t index = pick(randomEngine);
    sceneAmbienceIndex = index;
    sceneAmbienceEndsAt =
        now + originalStoryClipCompletion(sceneAmbienceTracks[index]);
    // 0x0040b11f starts it with 0x00412b90, an ordinary clip player
    // (0x00403270 -> 0x00402f80), so its track-0 audio takes the effect
    // volume although the assets live under music\.
    audioMixer.setAmbience(sceneAmbienceTracks[index],
                           clipAudioVolume(sceneAmbienceTracks[index]));
    referenceTrace.emit("animation_start", {
        {"asset", "../../common/music/ambience" + std::to_string(index + 1U)},
        {"x", 0}, {"y", 0}, {"layer", 0}, {"loop", false}});
}

void AppCore::updateSceneAmbience(double now) {
    if (!sceneAmbienceIndex || now < sceneAmbienceEndsAt) {
        return;
    }
    const double endedAt = sceneAmbienceEndsAt;
    referenceTrace.emit("animation_stop", {
        {"asset", "../../common/music/ambience" +
                      std::to_string(*sceneAmbienceIndex + 1U)},
        {"reason", "completed"}});
    startSceneAmbience(endedAt);
}

void AppCore::stopSceneAmbience() {
    if (!sceneAmbienceIndex) {
        return;
    }
    // 0x0040b1e0 removes the player when the scene is left.
    referenceTrace.emit("animation_remove", {
        {"asset", "../../common/music/ambience" +
                      std::to_string(*sceneAmbienceIndex + 1U)},
        {"reason", "removed"}});
    sceneAmbienceIndex.reset();
    audioMixer.clearAmbience();
}

void AppCore::leaveSceneMusicDuck(std::string_view leftScene) {
    // Of the scenes that duck the music, only the TV room restores it when
    // it is left: its EndScene handler 0x00427870 (event 0x104) writes 1.0
    // at 0x00427879. Intent and Outtent have no 0x104 handler, and leaving
    // a scene clears its timers (0x0040c930), so music ducked there stays
    // ducked in the next scene. EndScene runs inside 0x0040ccd0, after
    // GAME_EnterScene was called, so the trace reports it after the
    // scene_change (the TV room capture of 2026-10-20).
    if (leftScene == "tvroom") {
        setMusicDuck(1.0F);
    }
}

void AppCore::setMusicDuck(float duck) {
    // 0x0040b410(2, 2, duck): set 2, channel 2 (music). The value is a
    // step, applied by one SmackVolumePan on the music player (0x0040b44a
    // -> 0x00403620); nothing ramps it.
    audioMixer.setMusicDuck(duck);
    referenceTrace.emit("audio_volume", {
        {"set", 2}, {"channel", 2}, {"volume", static_cast<double>(duck)}});
}

} // namespace fgp
