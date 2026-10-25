#include "game/app_pee.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "assets/profile.h"
#include "media/media.h"
#include "media/smacker.h"

namespace fgp {

// 0x00426820: choose a fresh path, family and speed.
void PeeScene::startPeeParticleCycle() {
    if (!peeParticleField) {
        return;
    }
    auto &field = *peeParticleField;
    field.leftToRight = particleRandom(2) != 0;
    field.pointCount = 25 + particleRandom(25);
    field.path[0] = {field.leftToRight ? -100 : 900, particleRandom(300)};

    float phase = 0.0F;
    for (int index = 1; index < field.pointCount; ++index) {
        const int along = (1000 * index) / (field.pointCount - 1);
        field.path[static_cast<std::size_t>(index)] = {
            field.leftToRight ? along : 900 - along,
            150 - static_cast<int>(std::sin(phase) * -100.0)};
        phase += static_cast<float>(particleRandomValue()) * 1.52590e-5F;
    }

    field.progress = 0.0;
    field.family = particleRandom(static_cast<int>(kParticleFamilies));
    field.speed =
        (0.5 - particleRandomValue() * 3.05180e-5) * 0.05 + 0.075;
    field.direction = -1;

    static constexpr std::array<const char *, kParticleFamilies> names{
        "largeblue", "smallwhite", "bluemorph"};
    std::cout << "Morph particle cycle: " << names[static_cast<std::size_t>(
                                                 field.family)]
              << ", " << field.pointCount << " points, "
              << (field.leftToRight ? "left to right" : "right to left")
              << ", " << 1.0 / field.speed << " s\n";
}

// 0x00426e50: movement direction of the current segment, snapped to 45
// degrees. See pee-particles-spec.md for the unresolved atan2 operand order.
int PeeScene::peeParticleDirection(SDL_Point current, SDL_Point next) const {
    const double dx = next.x - current.x;
    const double dy = next.y - current.y;
    const int degrees =
        static_cast<int>(std::atan2(dy, dx) * 57.29577951308232);
    int value = dx < 0.0 ? degrees + 180 : (360 - degrees) % 360;
    value = (450 - value) % 360;
    if (value < 0) {
        value += 360;
    }
    return 45 * (((value + 22) % 360) / 45);
}

// 0x00426c10: advance the cycle and place the current directional variant.
void PeeScene::updatePeeParticleField() {
    if (!peeParticleField || peeParticleField->pointCount < 2) {
        return;
    }
    auto &field = *peeParticleField;

    const double now = sceneElapsedNow();
    const double deltaMs = (now - field.lastUpdate) * 1000.0;
    if (deltaMs >= 1000.0) {
        // The original treats a long gap as a stall and only re-bases its
        // timestamp, so no motion is accumulated for that frame.
        field.lastUpdate = now;
        return;
    }
    if (field.progress < 1.0) {
        field.progress += deltaMs * field.speed * 0.001;
    }
    if (field.progress > 1.0) {
        startPeeParticleCycle();
    }

    const double position =
        (field.pointCount - 1) * std::min(field.progress, 1.0);
    const auto index = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(index);
    const SDL_Point current = field.path[index];
    const SDL_Point next = field.path[(index + 1) % kParticlePathSlots];

    const int direction = peeParticleDirection(current, next);
    if (direction != field.direction) {
        field.direction = direction;
        field.playStart = now;
    }

    const double eased = (1.0 - std::cos(fraction * 3.14159274)) * 0.5;
    field.position = {
        current.x + static_cast<int>((next.x - current.x) * fraction),
        static_cast<int>((1.0 - eased) * current.y + eased * next.y)};
    field.lastUpdate = now;
}

bool PeeScene::handlePeeHotspot(std::uint32_t hotspot) {
    // The click handler 0x00426970 indexes byte table 0x00426b08 with
    // hotspot - 1, then block table 0x00426ad0. Every case but the magnet is
    // stateless and comes from kStoryClickResponses (1, 18, 19, 23, 27, 30),
    // the generated navigations (25 wheelbarrow, 26 butterfly, 29 fence, 39
    // desk with mode 1) or the shared collectible path (31, 32).
    if (hotspot != 21U) {
        return false;
    }
    // 0x004269e5: create/reveal or hide the magnet group.
    if (!peeMagnetActive) {
        peeMagnetActive = true;
        setSceneCharacterGroupEnabled(21U, true);
        startSceneAnimation(0x000c0ba0U, "hotspot 21 reveal");
    } else {
        startSceneAnimation(0x000c0c48U, "hotspot 21 hide");
    }
    return true;
}

// Draws the morph particle after advancing it (0x00426c10).
void PeeScene::renderPeeParticleField() {
    updatePeeParticleField();
    auto &field = *peeParticleField;
    const std::size_t variantIndex =
        static_cast<std::size_t>(field.family) * kParticleDirections +
        static_cast<std::size_t>(std::max(field.direction, 0)) / 45U;
    if (field.direction >= 0 && variantIndex < field.variants.size()) {
        auto &variant = field.variants[variantIndex];
        if (variant.texture && !variant.media.videoFrames.empty()) {
            const std::size_t frameIndex = frameIndexAt(
                variant.media, sceneElapsedNow() - field.playStart, true);
            const auto &frame = variant.media.videoFrames[frameIndex];
            uploadVideoFrame(variant.texture.get(), frame);
            SDL_Rect destination{field.position.x, field.position.y,
                                 frame.width, frame.height};
            SDL_RenderCopy(renderer, variant.texture.get(), nullptr,
                           &destination);
        }
    }
}

void PeeScene::buildPeeScene(
    SceneBuild &build,
    std::optional<PeeParticleField> &nextPeeParticleField) {
    // fcn.00426730 configures Ludvig as group 2/hotspot 2 and
    // installs its category-1 base table at 0x004c11a8.
    build.addSceneCharacterGroup(2U, 0x02U, 0x000c11a8U);

    // Hotspot 21 creates this group lazily on its first click.
    // Decode it at load time but keep it disabled until that exact
    // state transition occurs.
    build.addSceneCharacterGroup(21U, 0x10U, 0x000c11e0U);
    build.nextSceneCharacterGroups.back().enabled = false;
    peeMagnetActive = false;

    // The executable creates the door player stopped at frame 0
    // on layer -10. Hotspot 26 starts it; its last frame is fully
    // transparent, so the player disappears on completion.
    build.addSceneAnimationObject(0x000c0e40U, -10, true);

    const bool hasProfile =
        profiles[profileIndex] != "No profile" &&
        fs::exists(profileDirectory /
                   (profiles[profileIndex] + ".pfl"));
    bool carPartVisible = false;
    bool glassesVisible = false;
    if (hasProfile) {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        carPartVisible =
            profileSceneRecordU32(profilePath, "pee", 0x402U, 0U)
                .value_or(0U) == 0U;
        glassesVisible =
            (profileSceneRecordU32(profilePath, "anaglyph", 0x402U,
                                   100U)
                 .value_or(0U) &
             (1U << 3U)) == 0U;
    }
    build.addSceneCollectible(*build.nextPackage, "carpart", 48, 401, 31U, 3,
                              carPartVisible);
    if (build.nextAddonPackage) {
        build.addSceneCollectible(*build.nextAddonPackage, "3dglasses", 723,
                                  289, 32U, 3, glassesVisible);
    }

    const auto peeOpening =
        executableGameData.animationSequenceAtRva(0x000c1188U);
    double sequenceOffset = 0.0;
    for (const auto &definition : peeOpening) {
        sequenceOffset +=
            build.appendSceneDefinition(definition, sequenceOffset);
    }

    // SS-001C completion starts NAR-001; NAR-001 completion then
    // starts NAR-002. Both are independent of the visual stages.
    double narrationOffset = sequenceOffset;
    for (const char *name : {"S03AN-NAR-001", "S03AN-NAR-002"}) {
        narrationOffset += build.appendSceneNarration(name, narrationOffset);
    }

    // The ambient morph particle. 0x00426730 starts its first cycle on
    // every normal entry; the 24 packaged variants are the three
    // families crossed with the eight 45-degree directions.
    static constexpr std::array<const char *, kParticleFamilies>
        particleFamilies{"largeblue", "smallwhite", "bluemorph"};
    static constexpr std::array<int, kParticleDirections>
        particleDirections{0, 45, 90, 135, 180, 225, 270, 315};
    PeeParticleField field;
    field.variants.reserve(kParticleFamilies * kParticleDirections);
    for (const char *familyName : particleFamilies) {
        for (const int direction : particleDirections) {
            const std::string asset =
                "animation\\" + std::string(familyName) +
                std::to_string(direction) + ".smk";
            PeeParticleVariant variant;
            variant.name = asset;
            variant.media = decodeSmackerStreamed(
                build.nextPackage->data(build.nextPackage->find(asset)),
                asset, SmackerKeying::IndexZeroAndGreen, false, true);
            if (!variant.media.videoFrames.empty()) {
                const auto &frame = variant.media.videoFrames.metadata(0U);
                variant.texture.reset(SDL_CreateTexture(
                    renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, frame.width,
                    frame.height));
                if (!variant.texture) {
                    throw std::runtime_error(
                        std::string("SDL_CreateTexture failed: ") +
                        SDL_GetError());
                }
                SDL_SetTextureBlendMode(variant.texture.get(),
                                        SDL_BLENDMODE_BLEND);
            }
            field.variants.push_back(std::move(variant));
        }
    }
    nextPeeParticleField = std::move(field);

    build.nextSceneUsesAudioTimeline = true;
    build.nextSceneSequenceBlocksInput = true;
    std::cout << "Recovered pee opening from FGP.exe ("
              << peeOpening.size() << " stages, " << sequenceOffset
              << " seconds) and "
              << nextPeeParticleField->variants.size()
              << " morph-particle variants\n";
}

} // namespace fgp
