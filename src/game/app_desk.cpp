#include "game/app_desk.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "assets/profile.h"

namespace fgp {

bool DeskScene::handleDeskHotspot(std::uint32_t hotspot) {
    bool handled = true;
    switch (hotspot) {
    case 51:
        enterScene("sockdrawer", "sockdrawer");
        break;
    case 53:
        enterScene("bouquet", "bouquet");
        break;
    case 54:
        enterScene("colorfill", "colorfill");
        break;
    case 55: {
        const fs::path profilePath =
            profileDirectory /
            (profiles[profileIndex] + ".pfl");
        try {
            const auto partNumber =
                awardCarPart(profilePath, "desk");
            hideSceneCollectible("carpart");
            showCarPartAward(partNumber);
        } catch (const std::exception &error) {
            showMessage(std::string(
                            "Kunne ikke lagre premien: ") +
                        error.what());
        }
        break;
    }
    case 101: {
        static constexpr std::array<std::uint32_t, 3>
            tables{0x000b2f68U, 0x000b2f98U,
                   0x000b2f80U};
        const auto variant = nextNoRepeatOption(
            deskHotspot101Selector, 3);
        startSceneSequence(
            executableGameData.animationSequenceAtRva(
                tables[variant]),
            "hotspot 101 response");
        break;
    }
    case 102: {
        static constexpr std::array<std::uint32_t, 3>
            tables{0x000b3640U, 0x000b3654U,
                   0x000b3668U};
        const auto variant = nextNoRepeatOption(
            deskHotspot102Selector, 3);
        startSceneSequence(
            executableGameData.animationSequenceAtRva(
                tables[variant]),
            "hotspot 102 response");
        break;
    }
    case 103:
        startSceneAnimation(0x000b4110U,
                            "hotspot 103 response");
        break;
    case 104:
        startSceneAnimation(0x000b41b8U,
                            "hotspot 104 response");
        break;
    case 106:
        startSceneAnimation(0x000b3a70U,
                            "hotspot 106 response");
        break;
    case 108:
        startSceneSequence(
            executableGameData.animationSequenceAtRva(
                0x000b4100U),
            "hotspot 108 response");
        break;
    case 110: {
        static constexpr std::array<std::uint32_t, 3>
            descriptors{0x000b3d10U, 0x000b3db8U,
                        0x000b3e60U};
        const auto variant = nextNoRepeatOption(
            deskHotspot110Selector, 3);
        startSceneAnimation(descriptors[variant],
                            "hotspot 110 response");
        break;
    }
    case 113:
        startSceneAnimation(0x000b3b18U,
                            "hotspot 113 response");
        break;
    case 115:
        startSceneAnimation(0x000b3bc0U,
                            "hotspot 115 response");
        break;
    case 116:
        startSceneAnimation(0x000b4260U,
                            "hotspot 116 response");
        break;
    case 117:
        startSceneAnimation(0x000b3c68U,
                            "hotspot 117 response");
        break;
    case 118:
        startSceneAnimation(0x000b3878U,
                            "hotspot 118 arc");
        break;
    case 119:
        startSceneAnimation(0x000b3920U,
                            "hotspot 119 chime");
        break;
    case 120:
        startSceneAnimation(0x000b2fb0U,
                            "hotspot 120 story transition");
        break;
    case 121:
        startSceneAnimation(0x000b39c8U,
                            "hotspot 121 bear");
        break;
    default:
        handled = false;
        break;
    }
    return handled;
}

void DeskScene::buildDeskScene(SceneBuild &build) {
    // fcn.00424750 registers three persistent random-animation
    // groups before starting the normal-entry table at 0x004b2770.
    // fcn.0040a2b0 supplies the dynamic hotspot IDs 1/2/3 and
    // conflict masks 1/2/4 respectively.
    build.addSceneCharacterGroup(1U, 0x01U, 0x000b46f8U);
    build.addSceneCharacterGroup(2U, 0x02U, 0x000b5328U);
    build.addSceneCharacterGroup(3U, 0x04U, 0x000b5b50U);

    const bool hasProfile =
        profiles[profileIndex] != "No profile" &&
        fs::exists(profileDirectory /
                   (profiles[profileIndex] + ".pfl"));
    bool carPartVisible = false;
    std::uint32_t deskProgress = 0;
    if (hasProfile) {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        carPartVisible =
            profileSceneRecordU32(profilePath, "desk", 0x402U, 0U)
                .value_or(0U) == 0U;
        deskProgress =
            profileSceneRecordU32(profilePath, "desk", 0x402U, 1U)
                .value_or(0U);
    }
    // Descriptor 0x004b3680: hotspot 55, (524,179), layer -10.
    build.addSceneCollectible(*build.nextPackage, "carpart", 524, 179, 55U, 2,
                              carPartVisible);

    // fcn.00424750's entry-mode switch. Mode 0 starts the full opening
    // table; mode 1 (0x0042488d) seeds every group with no cutscene;
    // mode 2 (0x0042486e) seeds groups 1 and 4 and plays only
    // S02AN-SS-007b, which is how a return from `pee` re-enters.
    std::size_t deskStageCount = 0;
    double sequenceOffset = 0.0;
    if (activeSceneEntryMode == 2) {
        sequenceOffset += build.appendSceneDefinition(
            executableGameData.animationAtRva(0x000b3058U),
            sequenceOffset);
        deskStageCount = 1;
    } else if (activeSceneEntryMode == 0) {
        const auto deskOpening =
            executableGameData.animationSequenceAtRva(0x000b2770U);
        for (const auto &definition : deskOpening) {
            sequenceOffset +=
                build.appendSceneDefinition(definition, sequenceOffset);
        }
        deskStageCount = deskOpening.size();
    }

    // Desk narration is NOT scheduled here.
    //
    // An earlier reading had NAR-001 starting one second after entry and
    // NAR-002 chained to its completion, which is why both narrations used
    // to play back to back the moment the scene opened. Neither is what the
    // original does, and the one-second figure came from the analogue clock
    // (0x004248a8 arms a repeating 1000 ms timer whose handler 0x004248c0
    // moves the hands).
    //
    //   NAR-001  armed when the opening sequence finishes -- event 0x11f
    //            carrying table 0x004b2770, compared at 0x00424d37 -- and
    //            only when the profile's visit count is <= 1 (gate
    //            0x00424d65). It then waits ten seconds (0x00424d6a arms
    //            now + 0x2710). Entry modes 1 and 2 never produce 0x11f for
    //            that table, so it cannot play on those entries at all.
    //   NAR-002  started only by clicking the navigate overlay's next
    //            arrow: event 0x11e with button id 0x7d2, gated on the
    //            persisted key-1 counter (0x00424dcb). That handler
    //            returns 1, so the click does not navigate.
    //
    // Both are armed from their triggers instead; see the 0x11f hook in
    // app_render.cpp and activateStoryNavigation.

    build.nextSceneUsesAudioTimeline = true;
    build.nextSceneSequenceBlocksInput = deskStageCount != 0;
    std::cout << "Recovered desk entry mode " << activeSceneEntryMode
              << " from FGP.exe (" << deskStageCount << " stages, "
              << sequenceOffset << " seconds)\n";
}

} // namespace fgp
