#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/app_core.h"

namespace fgp {

// Original 0xa8-byte animation descriptors for the story scenes whose
// handlers drive them directly (Goodbye, Outro) rather than through the
// character-group scheduler. Each SCENE_AddAnim (0x00412840),
// SCENE_RemoveAnim (0x00412aa0), SCENE_StopAnim (0x00412a60) and
// AnimationFinished (0x0040c8a0) the original makes is mirrored in the
// reference trace, in the same order, as the Wheelbarrow port does.
// Implemented in story_descriptors.cpp.
class StoryDescriptors : protected virtual AppCore {
protected:
    struct StoryAnim {
        std::uint32_t address{};
        std::string name;
        bool smacker{};     // +0x40
        bool visible{true}; // +0x44
        bool keyed{};       // +0x48
        bool loop{};        // +0x4c
        int defaultX{};     // +0x54
        int defaultY{};     // +0x58
        int defaultLayer{}; // +0x64
        std::uint32_t hotspot{};      // +0x50
        std::uint32_t conflictMask{}; // +0x60
        // Mixed at the speech volume rather than the sound volume.
        bool speech{};
        // A clone made by 0x00412c20/0x00412c60: removed when it finishes.
        bool clone{};
        // A text control (0x00407930): `text` drawn in `font`, placed in
        // `textRect` by 0x004124e0 with `textFlags`.
        const BitmapFont *font{};
        std::string text;
        SDL_Rect textRect{};
        int textFlags{};
        bool active{};  // +0x7c, read by 0x004138f0
        bool playing{}; // +0x78, read by 0x00413900
        int x{};
        int y{};
        int layer{};
        std::uint64_t order{};
        double playStarted{};
        double frozenAt{};
        std::uint64_t audioHandle{LiveAudioMixer::kInvalidEffectHandle};
    };
    struct StoryClip {
        DecodedMedia media;
        TexturePtr texture;
        double completion{};
    };
    struct StoryImage {
        TexturePtr texture;
        int width{};
        int height{};
    };
    struct StoryStage {
        std::vector<std::unique_ptr<StoryAnim>> anims;
        std::unordered_map<std::string, StoryClip> clips;
        std::unordered_map<std::string, StoryImage> images;
        std::uint64_t nextOrder{};
    };

    // Reads the descriptor at `address` and decodes its clip (video only
    // when it is visible). The returned reference stays valid while the
    // stage lives.
    StoryAnim &loadStoryAnim(StoryStage &stage, std::uint32_t address,
                             bool speech = false);
    // A descriptor the scene fills in itself (BSS arrays, renamed
    // descriptors); Smacker clips are decoded when first played.
    StoryAnim &makeStoryAnim(StoryStage &stage, std::uint32_t address,
                             std::string name, bool smacker, bool visible,
                             bool keyed, bool loop, int x, int y,
                             std::uint32_t hotspot);
    // The scene's bitmap\<name>.bmp, loaded once.
    const StoryImage *storyImage(StoryStage &stage, const std::string &name,
                                 bool keyed);
    // 0x00412c20: plays an invisible, non-looping Smacker clone of `name`
    // in the first free slot from 0x0054fea8; it is removed when done.
    void playStoryClone(StoryStage &stage, const std::string &name,
                        double now);
    // SCENE_AddAnim 0x00412840; -1 selects the descriptor's own position.
    void addStoryAnim(StoryStage &stage, StoryAnim &anim, int x, int y,
                      int layer);
    void stopStoryAnim(StoryAnim &anim, double now);
    void removeStoryAnim(StoryAnim &anim, double now);
    // SCENE_PlayAnim 0x004129d0.
    void playStoryAnim(StoryStage &stage, StoryAnim &anim, double now);
    // 0x00412b90: add an inactive descriptor at its own position and
    // layer, then play it.
    void addAndPlayStoryAnim(StoryStage &stage, StoryAnim &anim, double now);
    // The descriptor update loop 0x00413200: a finished non-looping player
    // is stopped and reported, then `finished` runs the scene's 0x110
    // handler for it.
    void updateStoryDescriptors(StoryStage &stage, double now,
                                const std::function<void(StoryAnim &)> &finished);
    void renderStoryDescriptors(StoryStage &stage, double now);
};

} // namespace fgp
