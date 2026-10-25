#include "game/app_intro.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "assets/package.h"
#include "media/media.h"
#include "media/smacker.h"

namespace fgp {

void IntroScene::buildIntroScene(SceneBuild &build) {
    SurfacePtr introTitle = loadGreenKeyedBmp(
        build.nextPackage->data(build.nextPackage->find("bitmap\\title.bmp")));
    build.nextSceneTitleWidth = introTitle->w;
    build.nextSceneTitleHeight = introTitle->h;
    build.nextSceneTitleTexture = makeTexture(renderer, introTitle.get());
    SDL_SetTextureBlendMode(build.nextSceneTitleTexture.get(),
                            SDL_BLENDMODE_BLEND);

    // Narration and music are not part of the stages. The completion
    // callback 0x00416640 starts them as their own players: NAR-001 when
    // SS-001a ends (0x00416659), and when SS-001b ends it removes the title,
    // starts track1 (0x0040b1f0) and NAR-002 (0x0041666e-0x00416689). They
    // are decoded here so starting them costs nothing at the boundary.
    fs::path narrationPath =
        packagePath(gameDirectory, "Language1", "intro");
    if (!fs::exists(narrationPath)) {
        narrationPath = packagePath(gameDirectory, "Language", "intro");
    }
    const Package narrationPackage(narrationPath);
    for (auto [name, media] :
         std::array<std::pair<const char *, DecodedMedia *>, 2>{{
             {"S00AN-NAR-001", &introNarration001},
             {"S00AN-NAR-002", &introNarration002},
         }}) {
        *media = decodeSmacker(
            narrationPackage.data(narrationPackage.find(
                "animation\\" + std::string(name) + ".smk")),
            false, true);
    }
    introTrack1 = decodeSmacker(common.data(common.find("music\\track1.smk")),
                                false, true);

    struct IntroDefinition {
        const char *video;
        std::uint32_t descriptorRva;
        bool embeddedAudio;
        int x;
        int y;
        std::uint32_t conflictMask;
    };
    // SS-001a and 001b carry conflict bit 1 (the SCENE_AddAnim records in
    // the intro-opening capture), so Reodor's idle group below waits until
    // 001b ends; 001c carries none. The descriptors are the entries of the
    // sequence table 0x004ab858.
    const std::array<IntroDefinition, 3> introDefinitions{{
        {"S00AN-SS-001a", 0x000ab318U, false, 217, 164, 0x1U},
        {"S00AN-SS-001b", 0x000ab3c0U, true, 174, 92, 0x1U},
        {"S00AN-SS-001c", 0x000ab468U, false, 86, 183, 0U},
    }};

    for (const auto &definition : introDefinitions) {
        const std::string videoAsset =
            "animation\\" + std::string(definition.video) + ".smk";
        // All three descriptors set +0x48, so scene_DrawAnim keys them on
        // their top-left pixel (0x00413071). Palette index 0 is a drawn
        // colour in all three clips: Reodor's skin highlight in 001a and 001b
        // ((251,211,134) in 001b), which the old index-0 key cut out of his
        // face.
        const bool keyed =
            executableGameData.animationAtRva(definition.descriptorRva).keyed;
        DecodedMedia media = decodeSmackerStreamed(
            build.nextPackage->data(build.nextPackage->find(videoAsset)),
            definition.video,
            keyed ? SmackerKeying::TopLeftRgb565
                  : SmackerKeying::IndexZeroAndGreen,
            definition.embeddedAudio);

        TexturePtr texture;
        if (!media.videoFrames.empty()) {
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
        // The original sequence advances on Smacker video-complete
        // events. Audio tracks overlap but do not stretch a stage.
        const double duration = originalStoryClipCompletion(media);
        std::cout << "Decoded intro clip " << definition.video << " at ("
                  << definition.x << ',' << definition.y << ") for "
                  << duration << " seconds\n";
        build.nextSceneClips.push_back({definition.video,
                                   definition.x,
                                   definition.y,
                                   false,
                                   std::move(media),
                                   duration,
                                   std::move(texture),
                                   true,
                                   0,
                                   definition.conflictMask});
    }

    // Reodor's idles are a character group, not a fixed alternation: the
    // initializer binds group bit 1 with no dynamic hotspot (0x004165e8),
    // installs base table 0x004ab868 (RAN-B01) as category 1 and
    // 0x004ab870 (RAN-001..004) as category 2 (0x004165f6, 0x00416604), and
    // never seeds it. The shared scheduler starts B01 at the boundary that
    // releases bit 1 -- the end of SS-001b, 32 ms before the capture's first
    // RAN-B01 -- repeats it, and plays the next special once the 15-25 s
    // deadline (0x0040ac60) has passed, as in the Yard.
    build.addSceneCharacterGroup(0U, 0x1U, 0x000ab868U, 0x000ab870U);
    // The initializer starts its three stages as one sequence (0x00416609
    // passes table 0x004ab858 to 0x0040a6f0), which hides the cursor for the
    // whole opening; only the key handler's Space reaches the scene.
    build.nextSceneSequenceBlocksInput = true;
}

void IntroScene::handleIntroAnimationFinished(std::string_view name) {
    if (name == "S00AN-SS-001a") {
        startSceneNarration("S00AN-NAR-001", false,
                            std::move(introNarration001));
    } else if (name == "S00AN-SS-001b") {
        // The title player is removed here; renderFrame stops drawing it
        // once the sequence reaches 001c.
        sceneBackgroundMusic = std::move(introTrack1);
        sceneBackgroundMusicAsset = "track1";
        audioMixer.setBackground(sceneBackgroundMusic,
                                 audioVolumes[2] * sceneAudioGains[2]);
        referenceTrace.emit("audio_start", {{"asset", "track1"},
                                             {"role", "music"}});
        startSceneNarration("S00AN-NAR-002", false,
                            std::move(introNarration002));
    }
}

} // namespace fgp
