#include "game/app_minigame_recovery.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "media/smacker.h"

namespace fgp {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char byte) {
                       return static_cast<char>(std::tolower(byte));
                   });
    return value;
}

std::string animationStem(const Package::Entry &entry) {
    std::string name = entry.name;
    std::replace(name.begin(), name.end(), '/', '\\');
    name = lower(std::move(name));
    constexpr std::string_view prefix = "animation\\";
    constexpr std::string_view suffix = ".smk";
    if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size() + suffix.size() ||
        name.substr(name.size() - suffix.size()) != suffix) {
        return {};
    }
    return name.substr(prefix.size(),
                       name.size() - prefix.size() - suffix.size());
}

bool isDetailedNativeGame(std::string_view scene) {
    return scene == "lettersort" || scene == "audiopairs" ||
           scene == "sockdrawer" || scene == "butterfly";
}

bool usableDescriptor(
    const ExecutableGameData::AnimationDefinition &definition) {
    return definition.hotspot != 0U && definition.hotspot != 0xffffffffU &&
           definition.name.find_first_not_of(' ') != std::string::npos &&
           definition.x >= -512 && definition.x <= kLogicalWidth &&
           definition.y >= -512 && definition.y <= kLogicalHeight;
}

// scene_DrawAnim keys a descriptor's clip on its top-left pixel when +0x48
// is set (0x00413071).
SmackerKeying descriptorKeying(
    const ExecutableGameData::AnimationDefinition &definition) {
    return definition.keyed ? SmackerKeying::TopLeftRgb565
                            : SmackerKeying::IndexZeroAndGreen;
}

} // namespace

RecoveredMinigameScene::RecoveredMinigameRuntime RecoveredMinigameScene::loadRecoveredMinigameRuntime() {
    RecoveredMinigameRuntime runtime;
    if (!sceneIsMinigameShell || isDetailedNativeGame(activeScene)) {
        return runtime;
    }

    std::unordered_set<std::uint32_t> directAddresses;
    for (const auto &definition :
         executableGameData.directlyReferencedAnimations(activeScene)) {
        directAddresses.insert(definition.address);
    }

    std::unordered_set<std::string> stems;
    for (Package *package : {scenePackage.get(), sceneEditionPackage.get(),
                             sceneData2Package.get(), sceneAddonPackage.get(),
                             sceneLanguagePackage.get()}) {
        if (!package) {
            continue;
        }
        for (const auto &entry : package->entries()) {
            if (auto stem = animationStem(entry); !stem.empty()) {
                stems.insert(std::move(stem));
            }
        }
    }

    std::unordered_set<std::uint32_t> seen;
    for (const auto &stem : stems) {
        for (const auto &definition :
             executableGameData.animationDefinitionsNamed(stem)) {
            if (!usableDescriptor(definition) ||
                !seen.insert(definition.address).second) {
                continue;
            }
            runtime.clips.push_back(
                {definition, directAddresses.contains(definition.address)});
        }
    }
    std::stable_sort(runtime.clips.begin(), runtime.clips.end(),
                     [](const RecoveredMinigameClip &left,
                        const RecoveredMinigameClip &right) {
                         if (left.directlyReferenced !=
                             right.directlyReferenced) {
                             return left.directlyReferenced;
                         }
                         if (left.definition.hotspot !=
                             right.definition.hotspot) {
                             return left.definition.hotspot <
                                    right.definition.hotspot;
                         }
                         return left.definition.address <
                                right.definition.address;
                     });

    // A directly referenced Start player is strong enough to show before the
    // first click. Other animations remain lazy: decoding hundreds of Whack-
    // a-Mole clips at scene entry would waste memory and would incorrectly
    // expose mutually exclusive players simultaneously.
    const auto start = std::find_if(
        runtime.clips.begin(), runtime.clips.end(),
        [](const RecoveredMinigameClip &clip) {
            const std::string name = lower(clip.definition.name);
            return clip.directlyReferenced &&
                   name.find("start") != std::string::npos;
        });
    if (start != runtime.clips.end()) {
        DecodedMedia media = decodeSmacker(
            activeSceneAnimationData(start->definition.name), true, true,
            true, 0, nullptr, SmackerIndexZeroAlpha::Transparent,
            descriptorKeying(start->definition));
        TexturePtr texture;
        if (!media.videoFrames.empty()) {
            const auto &frame = media.videoFrames.front();
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
        runtime.activeClip = RecoveredMinigameActiveClip{
            start->definition, std::move(media), std::move(texture),
            sceneElapsedNow(), false};
    }

    std::cout << "Recovered " << runtime.clips.size()
              << " hotspot animation candidate(s), "
              << directAddresses.size() << " direct executable reference(s) for "
              << activeScene << '\n';
    return runtime;
}

bool RecoveredMinigameScene::playRecoveredMinigameHotspot(std::uint32_t hotspot) {
    if (!recoveredMinigameRuntime || hotspot == 0U) {
        return false;
    }
    auto &runtime = *recoveredMinigameRuntime;
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < runtime.clips.size(); ++index) {
        if (runtime.clips[index].definition.hotspot == hotspot) {
            candidates.push_back(index);
        }
    }
    if (candidates.empty()) {
        return false;
    }

    auto next = std::find_if(
        runtime.nextByHotspot.begin(), runtime.nextByHotspot.end(),
        [hotspot](const auto &item) { return item.first == hotspot; });
    if (next == runtime.nextByHotspot.end()) {
        runtime.nextByHotspot.push_back({hotspot, 0U});
        next = std::prev(runtime.nextByHotspot.end());
    }
    const std::size_t selected = candidates[next->second % candidates.size()];
    ++next->second;
    const auto &definition = runtime.clips[selected].definition;
    DecodedMedia media = decodeSmacker(
        activeSceneAnimationData(definition.name), true, true, true, 0,
        nullptr, SmackerIndexZeroAlpha::Transparent,
        descriptorKeying(definition));
    TexturePtr texture;
    if (!media.videoFrames.empty()) {
        const auto &frame = media.videoFrames.front();
        texture.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            frame.width, frame.height));
        if (!texture) {
            throw std::runtime_error(
                std::string("SDL_CreateTexture failed: ") + SDL_GetError());
        }
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
    }
    runtime.activeClip = RecoveredMinigameActiveClip{
        definition, std::move(media), std::move(texture),
        sceneElapsedNow(), true};
    runtime.lastHotspot = hotspot;
    ++runtime.interactions;
    audioMixer.setEffect(runtime.activeClip->media,
                         audioVolumes[0] * sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    std::cout << "Recovered minigame hotspot " << hotspot << " -> "
              << definition.name << " @ (" << definition.x << ','
              << definition.y << ")\n";
    return true;
}

void RecoveredMinigameScene::renderRecoveredMinigame(double elapsed) {
    if (!recoveredMinigameRuntime ||
        !recoveredMinigameRuntime->activeClip) {
        return;
    }
    auto &clip = *recoveredMinigameRuntime->activeClip;
    if (!clip.texture || clip.media.videoFrames.empty()) {
        return;
    }
    const double localTime = elapsed - clip.startedAt;
    if (clip.playing && clip.media.videoDuration > 0.0 &&
        localTime >= clip.media.videoDuration) {
        return;
    }
    const std::size_t frameIndex = clip.playing
        ? frameIndexAt(clip.media, localTime, false)
        : 0U;
    const auto &frame = clip.media.videoFrames[frameIndex];
    uploadVideoFrame(clip.texture.get(), frame);
    const SDL_Rect destination{clip.definition.x, clip.definition.y,
                               frame.width, frame.height};
    SDL_RenderCopy(renderer, clip.texture.get(), nullptr, &destination);
}

std::optional<std::pair<std::uint32_t, SDL_Rect>>
RecoveredMinigameScene::recoveredMinigameHotspotAt(int x, int y) const {
    if (!recoveredMinigameRuntime ||
        !recoveredMinigameRuntime->activeClip) {
        return std::nullopt;
    }
    const auto &clip = *recoveredMinigameRuntime->activeClip;
    if (clip.definition.hotspot == 0U || clip.media.videoFrames.empty()) {
        return std::nullopt;
    }
    const double localTime = const_cast<RecoveredMinigameScene *>(this)->sceneElapsedNow() -
                             clip.startedAt;
    if (clip.playing && clip.media.videoDuration > 0.0 &&
        localTime >= clip.media.videoDuration) {
        return std::nullopt;
    }
    const std::size_t frameIndex = clip.playing
        ? frameIndexAt(clip.media, localTime, false)
        : 0U;
    const auto &frame = clip.media.videoFrames[frameIndex];
    const int localX = x - clip.definition.x;
    const int localY = y - clip.definition.y;
    if (localX < 0 || localY < 0 || localX >= frame.width ||
        localY >= frame.height) {
        return std::nullopt;
    }
    const std::size_t alpha =
        (static_cast<std::size_t>(localY) * frame.width + localX) * 4U + 3U;
    if (alpha >= frame.bgra.size() || frame.bgra[alpha] == 0U) {
        return std::nullopt;
    }
    return std::pair{clip.definition.hotspot,
                     SDL_Rect{clip.definition.x, clip.definition.y,
                              frame.width, frame.height}};
}

bool RecoveredMinigameScene::executeRecoveredMinigameDeveloperCommand(std::string_view command) {
    constexpr std::string_view recoveredControlPrefix = "recovered:";
    if (!recoveredMinigameRuntime) {
        return false;
    }
    const std::string_view control =
        command.substr(recoveredControlPrefix.size());
    constexpr std::string_view playPrefix = "play:";
    if (control.rfind(playPrefix, 0) == 0) {
        const auto hotspot = static_cast<std::uint32_t>(std::stoul(
            std::string(control.substr(playPrefix.size()))));
        if (!playRecoveredMinigameHotspot(hotspot)) {
            throw std::runtime_error(
                "No recovered animation candidate for hotspot " +
                std::to_string(hotspot));
        }
    } else if (control == "status") {
        std::cout << "Recovered " << activeScene << " status: "
                  << recoveredMinigameRuntime->clips.size()
                  << " candidate players, "
                  << recoveredMinigameRuntime->interactions
                  << " interactions, last hotspot "
                  << recoveredMinigameRuntime->lastHotspot << '\n';
    } else {
        throw std::runtime_error(
            "Unknown recovered-minigame control: " +
            std::string(control));
    }
    return true;
}

} // namespace fgp
