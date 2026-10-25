#include "game/story_descriptors.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "media/smacker.h"

namespace fgp {
namespace {

constexpr std::uint32_t kImageBase = 0x00400000U;
// The shared modal mesh descriptor (0x00407110).
constexpr std::uint32_t kModalMeshDescriptor = 0x004a61b0U;

// The completion boundary originalSmackerCompletionDuration() derives from
// decoded frames, read from the Smacker header for audio-only clips: the
// last frame's start plus two 64 Hz player passes, capped at the length.
double smackerHeaderCompletion(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 20U) {
        return 0.0;
    }
    const auto u32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(bytes[offset]) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    };
    const auto frames = u32(12U);
    const auto rate = static_cast<std::int32_t>(u32(16U));
    const double frameSeconds = rate > 0 ? rate / 1000.0
        : (rate < 0 ? -rate / 100000.0 : 0.1);
    if (frames == 0U) {
        return 0.0;
    }
    return std::min(frames * frameSeconds,
                    (frames - 1U) * frameSeconds + 2.0 / 64.0);
}

std::string descriptorText(std::uint32_t address) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", address);
    return text;
}

} // namespace

namespace {

template <typename Clip>
void decodeStoryClip(SDL_Renderer *renderer, Clip &clip,
                     std::span<const std::uint8_t> bytes,
                     const std::string &name, bool visible, bool keyed) {
    if (visible) {
        clip.media = decodeSmackerStreamed(
            bytes, name,
            keyed ? SmackerKeying::TopLeftRgb565
                  : SmackerKeying::IndexZeroAndGreen,
            true, true);
        clip.completion = originalSmackerCompletionDuration(clip.media);
        if (!clip.media.videoFrames.empty()) {
            const auto &frame = clip.media.videoFrames.metadata(0U);
            clip.texture.reset(SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height));
            if (!clip.texture) {
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(clip.texture.get(), SDL_BLENDMODE_BLEND);
        }
    } else {
        clip.media = decodeSmacker(bytes, false, true, true);
        clip.completion = smackerHeaderCompletion(bytes);
    }
}

} // namespace

StoryDescriptors::StoryAnim &StoryDescriptors::loadStoryAnim(
    StoryStage &stage, std::uint32_t address, bool speech) {
    const auto definition =
        executableGameData.animationAtRva(address - kImageBase);
    auto anim = std::make_unique<StoryAnim>();
    anim->address = address;
    anim->name = definition.name;
    anim->smacker = definition.smacker;
    anim->visible = definition.visible;
    anim->keyed = definition.keyed;
    anim->loop = definition.loop;
    anim->defaultX = definition.x;
    anim->defaultY = definition.y;
    anim->defaultLayer = definition.layer;
    anim->hotspot = definition.hotspot;
    anim->conflictMask = definition.conflictMask;
    anim->speech = speech;
    if (anim->smacker && !stage.clips.contains(anim->name)) {
        decodeStoryClip(renderer, stage.clips[anim->name],
                        activeSceneAnimationData(anim->name), anim->name,
                        anim->visible, anim->keyed);
    }
    stage.anims.push_back(std::move(anim));
    return *stage.anims.back();
}

StoryDescriptors::StoryAnim &StoryDescriptors::makeStoryAnim(
    StoryStage &stage, std::uint32_t address, std::string name, bool smacker,
    bool visible, bool keyed, bool loop, int x, int y,
    std::uint32_t hotspot) {
    auto anim = std::make_unique<StoryAnim>();
    anim->address = address;
    anim->name = std::move(name);
    anim->smacker = smacker;
    anim->visible = visible;
    anim->keyed = keyed;
    anim->loop = loop;
    anim->defaultX = x;
    anim->defaultY = y;
    anim->hotspot = hotspot;
    stage.anims.push_back(std::move(anim));
    return *stage.anims.back();
}

const StoryDescriptors::StoryImage *StoryDescriptors::storyImage(
    StoryStage &stage, const std::string &name, bool keyed) {
    if (name.empty()) {
        return nullptr;
    }
    if (const auto found = stage.images.find(name);
        found != stage.images.end()) {
        return &found->second;
    }
    const std::string asset = "bitmap\\" + name + ".bmp";
    for (Package *candidate :
         {sceneLanguagePackage.get(), sceneAddonPackage.get(),
          sceneData2Package.get(), sceneEditionPackage.get(),
          scenePackage.get()}) {
        if (!candidate) {
            continue;
        }
        if (const auto *entry = candidate->findOptional(asset)) {
            SurfacePtr surface = keyed
                ? loadGreenKeyedBmp(candidate->data(*entry))
                : loadBmp(candidate->data(*entry));
            StoryImage image;
            image.width = surface->w;
            image.height = surface->h;
            image.texture = makeTexture(renderer, surface.get());
            if (keyed) {
                SDL_SetTextureBlendMode(image.texture.get(),
                                        SDL_BLENDMODE_BLEND);
            }
            return &stage.images.emplace(name, std::move(image)).first->second;
        }
    }
    return nullptr;
}

void StoryDescriptors::playStoryClone(StoryStage &stage,
                                      const std::string &name, double now) {
    if (!stage.clips.contains(name)) {
        decodeStoryClip(renderer, stage.clips[name],
                        activeSceneAnimationData(name), name, false, false);
    }
    // 0x00412c60 takes the first 0xa8-byte slot from 0x0054fea8 that no
    // clone in the scene holds.
    std::uint32_t address = 0x0054fea8U;
    while (std::any_of(stage.anims.begin(), stage.anims.end(),
                       [&](const std::unique_ptr<StoryAnim> &anim) {
                           return anim->clone && anim->active &&
                                  anim->address == address;
                       })) {
        address += 0xa8U;
    }
    StoryAnim &clone =
        makeStoryAnim(stage, address, name, true, false, false, false, 0, 0, 0U);
    clone.clone = true;
    addAndPlayStoryAnim(stage, clone, now);
}

void StoryDescriptors::addStoryAnim(StoryStage &stage, StoryAnim &anim,
                                    int x, int y, int layer) {
    anim.active = true;
    anim.playing = false;
    anim.frozenAt = 0.0;
    anim.x = x == -1 ? anim.defaultX : x;
    anim.y = y == -1 ? anim.defaultY : y;
    anim.layer = layer;
    anim.order = ++stage.nextOrder;
    referenceTrace.emit("animation_add", {
        {"asset", anim.name}, {"x", x}, {"y", y}, {"layer", layer},
        {"hotspot", anim.hotspot}, {"conflict_mask", anim.conflictMask},
        {"descriptor", descriptorText(anim.address)}});
}

void StoryDescriptors::stopStoryAnim(StoryAnim &anim, double now) {
    // SCENE_StopAnim 0x00412a60 is logged on entry for every descriptor.
    referenceTrace.emit("animation_stop", {
        {"asset", anim.name}, {"reason", "stopped"},
        {"descriptor", descriptorText(anim.address)}});
    if (anim.smacker && anim.playing) {
        anim.frozenAt = now - anim.playStarted;
        anim.playing = false;
    }
    if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
        audioMixer.removeEffect(anim.audioHandle);
        anim.audioHandle = LiveAudioMixer::kInvalidEffectHandle;
    }
}

void StoryDescriptors::removeStoryAnim(StoryAnim &anim, double now) {
    // SCENE_RemoveAnim 0x00412aa0 stops the descriptor through 0x00412a60,
    // so its logpoint precedes the nested StopAnim record.
    referenceTrace.emit("animation_remove", {
        {"asset", anim.name}, {"reason", "removed"},
        {"descriptor", descriptorText(anim.address)}});
    stopStoryAnim(anim, now);
    anim.active = false;
    anim.playing = false;
}

void StoryDescriptors::playStoryAnim(StoryStage &stage, StoryAnim &anim,
                                     double now) {
    // 0x004129d0 starts the player (+0x84/+0x88) and sets +0x78.
    if (!anim.smacker) {
        return;
    }
    if (!stage.clips.contains(anim.name)) {
        decodeStoryClip(renderer, stage.clips[anim.name],
                        activeSceneAnimationData(anim.name), anim.name,
                        anim.visible, anim.keyed);
    }
    const auto found = stage.clips.find(anim.name);
    const bool finished =
        !anim.loop && anim.frozenAt >= found->second.completion;
    anim.playStarted = now - (finished ? 0.0 : anim.frozenAt);
    anim.playing = true;
    if (!found->second.media.audioSamples.empty()) {
        if (anim.audioHandle != LiveAudioMixer::kInvalidEffectHandle) {
            audioMixer.removeEffect(anim.audioHandle);
        }
        const std::size_t bus = anim.speech ? 1U : 0U;
        anim.audioHandle = audioMixer.addEffect(
            found->second.media, audioVolumes[bus] * sceneAudioGains[bus],
            anim.loop);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void StoryDescriptors::addAndPlayStoryAnim(StoryStage &stage,
                                           StoryAnim &anim, double now) {
    if (!anim.active) {
        addStoryAnim(stage, anim, anim.defaultX, anim.defaultY,
                     anim.defaultLayer);
    }
    playStoryAnim(stage, anim, now);
}

void StoryDescriptors::updateStoryDescriptors(
    StoryStage &stage, double now,
    const std::function<void(StoryAnim &)> &finished) {
    std::vector<StoryAnim *> done;
    for (const auto &anim : stage.anims) {
        if (!anim->active || !anim->smacker || !anim->playing ||
            anim->loop) {
            continue;
        }
        const auto found = stage.clips.find(anim->name);
        if (found != stage.clips.end() &&
            now - anim->playStarted >= found->second.completion) {
            done.push_back(anim.get());
        }
    }
    std::stable_sort(done.begin(), done.end(),
                     [](const StoryAnim *left, const StoryAnim *right) {
                         return left->order < right->order;
                     });
    for (StoryAnim *anim : done) {
        if (!anim->active || !anim->playing) {
            continue;
        }
        // 0x00413245-0x00413269: the finished player is stopped, then the
        // descriptor reports AnimationFinished and posts event 0x110.
        anim->frozenAt = now - anim->playStarted;
        stopStoryAnim(*anim, now);
        if (anim->clone) {
            removeStoryAnim(*anim, now);
            continue;
        }
        referenceTrace.emit("animation_stop", {
            {"asset", anim->name}, {"reason", "finished"},
            {"descriptor", descriptorText(anim->address)}});
        finished(*anim);
    }
    std::erase_if(stage.anims, [](const std::unique_ptr<StoryAnim> &anim) {
        return anim->clone && !anim->active;
    });
}

void StoryDescriptors::renderStoryDescriptors(StoryStage &stage, double now) {
    std::vector<StoryAnim *> drawn;
    for (const auto &anim : stage.anims) {
        if (anim->active && anim->visible) {
            drawn.push_back(anim.get());
        }
    }
    // The comparator at 0x0047e220 draws in ascending +0x74 layer order.
    std::stable_sort(drawn.begin(), drawn.end(),
                     [](const StoryAnim *left, const StoryAnim *right) {
                         return left->layer != right->layer
                                    ? left->layer < right->layer
                                    : left->order < right->order;
                     });
    for (StoryAnim *anim : drawn) {
        if (anim->address == kModalMeshDescriptor) {
            // 0x00407110 adds 0x004a61b0 with the surface 0x00407160
            // builds: the checker mesh every modal dialog shares.
            if (modalMeshTexture) {
                SDL_RenderCopy(renderer, modalMeshTexture.get(), nullptr,
                               nullptr);
            }
            continue;
        }
        if (anim->font) {
            // 0x004124e0: flags & 0xff00 picks left, centre (0x100) or
            // right (0x200); flags & 0xff picks top, middle (1) or bottom
            // (2). Halves truncate.
            const int width = textWidth(*anim->font, anim->text);
            const int height = anim->font->height;
            const SDL_Rect &rect = anim->textRect;
            int x = rect.x;
            if ((anim->textFlags & 0xff00) == 0x100) {
                x = rect.x + (rect.w - width) / 2;
            } else if ((anim->textFlags & 0xff00) == 0x200) {
                x = rect.x + rect.w - width;
            }
            int y = rect.y;
            if ((anim->textFlags & 0xff) == 1) {
                y = rect.y + (rect.h - height) / 2;
            } else if ((anim->textFlags & 0xff) == 2) {
                y = rect.y + rect.h - height;
            }
            drawText(renderer, *anim->font, anim->text, x, y);
            continue;
        }
        if (!anim->smacker) {
            if (const StoryImage *image =
                    storyImage(stage, anim->name, anim->keyed)) {
                const SDL_Rect target{anim->x, anim->y, image->width,
                                      image->height};
                SDL_RenderCopy(renderer, image->texture.get(), nullptr,
                               &target);
            }
            continue;
        }
        const auto found = stage.clips.find(anim->name);
        if (found == stage.clips.end() || !found->second.texture ||
            found->second.media.videoFrames.empty()) {
            continue;
        }
        const double time =
            anim->playing ? now - anim->playStarted : anim->frozenAt;
        const auto index =
            frameIndexAt(found->second.media, time, anim->loop);
        const auto &frame = found->second.media.videoFrames[index];
        uploadVideoFrame(found->second.texture.get(), frame);
        const SDL_Rect target{anim->x, anim->y, frame.width, frame.height};
        SDL_RenderCopy(renderer, found->second.texture.get(), nullptr,
                       &target);
    }
}

} // namespace fgp
