#pragma once

#include "game/app_core.h"

namespace fgp {

// The "pee" story scene: its state and logic.
// Implemented in app_pee.cpp.
class PeeScene : protected virtual AppCore {
protected:
    struct PeeParticleVariant {
        std::string name;
        DecodedMedia media;
        TexturePtr texture;
    };
    // The ambient morph particle recovered from 0x00426820/0x00426c10. The
    // path table keeps the executable's fixed 50 slots because the update
    // wraps its next-point index with % 50 and never clears stale slots.
    static constexpr std::size_t kParticlePathSlots = 50;
    static constexpr std::size_t kParticleFamilies = 3;
    static constexpr std::size_t kParticleDirections = 8;

    struct PeeParticleField {
        // Indexed as family * kParticleDirections + direction / 45.
        std::vector<PeeParticleVariant> variants;
        std::array<SDL_Point, kParticlePathSlots> path{};
        int pointCount{};
        int family{};
        double progress{};
        double speed{};
        double lastUpdate{};
        bool leftToRight{};
        int direction{-1};
        SDL_Point position{};
        double playStart{};
    };

    void startPeeParticleCycle();
    void updatePeeParticleField();
    int peeParticleDirection(SDL_Point current, SDL_Point next) const;
    void renderPeeParticleField();
    // Scene-specific responses to a click on hotspot `hotspot`. Returns
    // false when the generic hotspot handling should run instead.
    bool handlePeeHotspot(std::uint32_t hotspot);
    // Adds this scene's contents while loadScene builds it.
    void buildPeeScene(
        SceneBuild &build,
        std::optional<PeeParticleField> &nextPeeParticleField);

    std::optional<PeeParticleField> peeParticleField;
    bool peeMagnetActive{false};
};

} // namespace fgp
