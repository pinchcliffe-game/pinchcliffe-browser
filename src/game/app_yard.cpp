#include "game/app_yard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "assets/profile.h"

namespace fgp {

bool YardScene::handleYardHotspot(std::uint32_t hotspot) {
    bool handled = true;
    switch (hotspot) {
    case 1:
        enterScene("lettersort", "lettersort");
        break;
    case 2:
        enterScene("bugzzz", "bugzzz");
        break;
    case 3:
        enterScene("jigsaw", "jigsaw");
        break;
    // 0x00428775 / 0x00428767 hand the sound descriptors to 0x0040a770,
    // which runs each as a one-stage sequence (the yard-055 capture: added
    // at (0,0), AnimFinished about a second later).
    case 55:
        startSceneAnimation(0x000c94c0U, "electric effect");
        break;
    case 56:
        startSceneAnimation(0x000c9418U, "wind effect");
        break;
    case 57:
        startSceneSequence(
            executableGameData.animationSequenceAtRva(
                0x000cb0b0U),
            "hotspot 57 sequence");
        break;
    case 58: {
        const std::uint32_t constructionMode =
            yardConstructionFlags & 0x30U;
        if ((yardConstructionFlags & 0x04U) != 0) {
            const std::uint32_t table =
                yardHotspot58CompleteVariant == 0
                    ? 0x000cb188U
                    : 0x000cb198U;
            yardHotspot58CompleteVariant =
                (yardHotspot58CompleteVariant + 1) % 2;
            startSceneSequence(
                executableGameData.animationSequenceAtRva(
                    table),
                "hotspot 58 completed-pump response");
        } else if (constructionMode == 0x30U &&
                   (yardConstructionFlags & 0x02U) != 0) {
            startSceneAnimation(
                0x000c7fc0U,
                "hotspot 58 construction response");
        } else if (yardHotspot58BaseVariant == 0) {
            startSceneSequence(
                executableGameData.animationSequenceAtRva(
                    0x000cb0dcU),
                "hotspot 58 base response");
            yardHotspot58BaseVariant = 1;
        } else {
            startSceneAnimation(
                0x000c72a0U,
                "hotspot 58 alternate response");
            yardHotspot58BaseVariant = 0;
        }
        break;
    }
    case 59:
        if ((yardConstructionFlags & 0x30U) == 0x30U) {
            if ((yardConstructionFlags & 0x02U) != 0) {
                startSceneAnimation(
                    0x000c8a40U,
                    "hotspot 59 water pump");
                yardConstructionFlags |= 0x04U;
            } else {
                startSceneSequence(
                    executableGameData.animationSequenceAtRva(
                        0x000cb124U),
                    "hotspot 59 pump preparation");
                yardConstructionFlags |= 0x02U;
            }
        }
        break;
    case 60: {
        const std::uint32_t constructionMode =
            yardConstructionFlags & 0x30U;
        if (constructionMode == 0) {
            startSceneSequence(
                executableGameData.animationSequenceAtRva(
                    0x000cb0f0U),
                "hotspot 60 construction stage one");
            yardConstructionFlags =
                (yardConstructionFlags & ~0x30U) | 0x20U;
        } else if (constructionMode == 0x20U) {
            startSceneSequence(
                executableGameData.animationSequenceAtRva(
                    0x000cb13cU),
                "hotspot 60 construction stage two");
            yardConstructionFlags =
                (yardConstructionFlags & ~0x30U) | 0x30U;
        }
        break;
    }
    case 99: {
        const std::uint32_t table =
            yardHotspot99Variant == 0 ? 0x000cb154U
                                      : 0x000cb168U;
        yardHotspot99Variant =
            (yardHotspot99Variant + 1) % 2;
        startSceneSequence(
            executableGameData.animationSequenceAtRva(table),
            "hotspot 99 alternating sequence");
        break;
    }
    case 112:
        startSceneAnimation(0x000c8998U,
                           "hotspot 112 animation");
        break;
    case 113: {
        const std::uint32_t constructionMode =
            yardConstructionFlags & 0x30U;
        if (constructionMode == 0x20U ||
            constructionMode == 0x30U) {
            startSceneSequence(
                executableGameData.animationSequenceAtRva(
                    0x000cb114U),
                "hotspot 113 construction sequence");
            yardConstructionFlags |= 0x02U;
        }
        break;
    }
    case 254: {
        const fs::path profilePath =
            profileDirectory /
            (profiles[profileIndex] + ".pfl");
        try {
            awardAnaglyphScene(profilePath, 1U);
            hideSceneCollectible("3dglasses");
            showAnaglyphAward();
        } catch (const std::exception &error) {
            showMessage(std::string(
                            "Kunne ikke lagre premien: ") +
                        error.what());
        }
        break;
    }
    case 255: {
        const fs::path profilePath =
            profileDirectory /
            (profiles[profileIndex] + ".pfl");
        try {
            const auto partNumber =
                awardCarPart(profilePath, "yard");
            hideSceneCollectible("carpart");
            showCarPartAward(partNumber);
        } catch (const std::exception &error) {
            showMessage(std::string(
                            "Kunne ikke lagre premien: ") +
                        error.what());
        }
        break;
    }
    default:
        handled = false;
        break;
    }
    return handled;
}

void YardScene::buildYardScene(SceneBuild &build) {
    // fcn.00428350 clears the scene-local construction word on
    // every normal yard entry. The two alternating response
    // counters are separate globals and intentionally persist.
    yardConstructionFlags = 0;
    const std::array<std::tuple<const char *, int, int, int>, 6>
        bitmapDefinitions{{
            {"button_0", 20, 315, -10},
            {"button_1", 25, 326, -10},
            {"weight_0", 6, 236, -5},
            {"weight_1", 6, 236, -5},
            {"bush", 515, 484, 15},
            // Added by S01AN-SS-009b2's completion (0x004284f5) and removed
            // by S01AN-SS-009f's (0x00428522) or Space (0x004286b0).
            {"mailbox_1", 420, 176, -10},
        }};
    for (const auto &[name, x, y, layer] : bitmapDefinitions) {
        const std::string asset =
            "bitmap\\" + std::string(name) + ".bmp";
        SurfacePtr surface = loadGreenKeyedBmp(
            build.nextPackage->data(build.nextPackage->find(asset)));
        build.nextSceneBitmapLayers.push_back(
            {name, x, y, layer, surface->w, surface->h,
             makeTexture(renderer, surface.get())});
        SDL_SetTextureBlendMode(
            build.nextSceneBitmapLayers.back().texture.get(),
            SDL_BLENDMODE_BLEND);
        build.nextSceneBitmapLayers.back().visible =
            std::string_view(name) != "mailbox_1";
    }

    // fcn.00428080 conditionally creates these two layer -10
    // bitmap players after reading the active profile. Their
    // descriptors supply both the exact coordinates and dynamic
    // hotspot IDs; the 3D glasses live in the Data3 add-on layer.
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
            profileSceneRecordU32(profilePath, "yard", 0x402U, 0U)
                .value_or(0U) == 0U;
        glassesVisible =
            (profileSceneRecordU32(profilePath, "anaglyph", 0x402U,
                                   100U)
                 .value_or(0U) &
             (1U << 1U)) == 0U;
    }
    build.addSceneCollectible(*build.nextPackage, "carpart", 766, 398, 255U, 1,
                              carPartVisible);
    if (build.nextAddonPackage) {
        build.addSceneCollectible(*build.nextAddonPackage, "3dglasses", 754,
                                  323, 254U, 1, glassesVisible);
    }
    // fcn.00428350 selects through the shared no-repeat helper at
    // 0x0040a150, whose first cycle is IN TABLE ORDER: the first yard
    // visit of a session always opens with chain 0's intro variant.
    const std::size_t sequenceIndex =
        nextNoRepeatOption(yardOpeningSelector, 3);
    const auto yardSequences = executableGameData.yardOpeningSequences();
    // 0x00428420: when S01AN-SS-001a4 or 001b2 completes, the yard
    // callback replaces the running sequence with the shared story tail
    // at 0x004cb088 (NAR-002a .. LUD-002). Every intro chain therefore
    // converges into the same story; this is also what bounds the
    // second chain, whose raw table has no terminator of its own.
    std::vector<ExecutableGameData::AnimationDefinition> yardSequence;
    for (const auto &definition : yardSequences[sequenceIndex]) {
        yardSequence.push_back(definition);
        if (definition.name == "S01AN-SS-001a4" ||
            definition.name == "S01AN-SS-001b2") {
            break;
        }
    }
    for (const auto &definition :
         executableGameData.animationSequenceAtRva(0x000cb088U)) {
        yardSequence.push_back(definition);
    }

    double sequenceOffset = 0.0;
    for (auto definition : yardSequence) {
        // Group setup mutates this shared descriptor before the opening
        // reaches it. The on-disk PE value is zero, but the live original
        // descriptor carries Ludvig's dynamic hotspot and conflict bit.
        if (definition.name == "S01AN-LUD-002") {
            definition.hotspot = 252U;
            definition.conflictMask = 0x02U;
        }
        // S01AN-NAR-002a and 002d stay here: they are genuine stages of the
        // shared story tail 0x004cb088, entries 0 and 7, started by the
        // sequence engine itself. The narration that HANGS OFF stage
        // completions -- 002b, 002c and 003 -- is triggered in
        // app_render.cpp's stage-advance loop instead, because the original
        // starts those from its AnimFinished handler 0x00428420, and a
        // pre-mixed clip can be neither cancelled nor traced.
        sequenceOffset +=
            build.appendSceneDefinition(definition, sequenceOffset);
    }

    // The yard registers two persistent random-animation groups
    // before starting its opening sequence. fcn.0040a2b0 binds
    // group 1 to dynamic hotspot 251 and group 2 to 252;
    // fcn.0040a470 installs the null-terminated base-animation
    // tables at 0x004cb1a8 and 0x004cb1f0. Every descriptor in a
    // group receives the hotspot ID and its group conflict bit at
    // runtime. Decode those exact base lists so the characters
    // remain present after the opening and can supply their
    // original per-frame hit masks.
    // 0x00428080 binds hotspot 251 (push 0xfb at 0x004280a1) to the
    // table pushed at 0x004280ca and hotspot 252 to the one at
    // 0x004280f5, so 251 uses 0x004cb1f0 and 252 uses 0x004cb1a8.
    // Pairing them the other way round suppressed the wrong character
    // during opening stages that claim conflict bit 0x01.
    build.addSceneCharacterGroup(251U, 0x01U, 0x000cb1f0U,
                                 0x000cb204U);
    build.addSceneCharacterGroup(252U, 0x02U, 0x000cb1a8U,
                                 0x000cb1b8U);
    // Entry path 0 in 0x00428080 binds the group slots but does not seed
    // them explicitly: only entry path 1 calls 0x0040a880 immediately.
    // Binding still participates in conflict scheduling. Once an opening
    // boundary releases a group's bit, the generic scheduler starts its
    // base clip; kSceneEntrySeeding records only which groups were already
    // activated before such a boundary.

    // The module's timer 0 starts NAR-001 exactly 2000 ms after entry,
    // independently of the chosen visual chain: 0x0042835a arms it with
    // `now + 0x7d0` through 0x004283c0(tag 0), and 0x00428614 starts it
    // with 0x00412b90, so it overlays the opening rather than replacing it.
    // Armed rather than pre-mixed so that 0x004283e0 -- the cancel every
    // click and key press runs -- has something to cancel.
    const auto narrationDefinition =
        executableGameData.yardOpeningNarration();
    pendingYardOpeningNarration = narrationDefinition.name;

    build.nextSceneUsesAudioTimeline = true;
    build.nextSceneSequenceBlocksInput = true;
    std::cout << "Recovered yard opening chain " << sequenceIndex
              << " from FGP.exe (" << yardSequence.size()
              << " stages, " << sequenceOffset << " seconds, visit "
              << build.sceneVisitCount
              << (build.sceneVisitCount <= 1U ? ", NAR-003 eligible" : "")
              << ")\n";
}

void YardScene::handleYardAnimationFinished(std::string_view name,
                                            double completedAt) {
    if (name == "S01AN-NAR-002a") {
        startSceneNarration("S01AN-NAR-002b", false);
    } else if (name == "S01AN-SS-008c") {
        startSceneNarration("S01AN-NAR-002c", false);
    } else if (name == "S01AN-NAR-002d" &&
               activeSceneVisitCount <= 1U) {
        armSceneNarration("S01AN-NAR-003", 8.0, true, completedAt);
    } else if (name == "S01AN-SS-009b2") {
        setYardMailboxVisible(true);
    } else if (name == "S01AN-SS-009f") {
        setYardMailboxVisible(false);
    }
}

void AppCore::setYardMailboxVisible(bool visible) {
    for (auto &bitmap : sceneBitmapLayers) {
        if (bitmap.name != "mailbox_1" || bitmap.visible == visible) {
            continue;
        }
        bitmap.visible = visible;
        if (visible) {
            referenceTrace.emit("animation_add", {
                {"asset", "mailbox_1"}, {"x", -1}, {"y", -1},
                {"layer", -10}, {"hotspot", 0}, {"conflict_mask", 0}});
            referenceTrace.emit("animation_start", {
                {"asset", "mailbox_1"}, {"x", -1}, {"y", -1},
                {"layer", -10}, {"loop", false}});
        } else {
            referenceTrace.emit("animation_remove", {
                {"asset", "mailbox_1"}, {"reason", "removed"}});
        }
    }
}

} // namespace fgp
