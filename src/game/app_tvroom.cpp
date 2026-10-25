#include "game/app_tvroom.h"

#include <iostream>
#include <optional>

#include "media/smacker.h"

namespace fgp {

void TvroomScene::buildTvroomScene(
    SceneBuild &build,
    std::optional<DecodedMedia> &nextTvroomConcurrentClip) {
    // Normal TV Room entry activates all three persistent characters.
    // The callback uses several initializer functions, so the conservative
    // generated-group pass intentionally omits the scene. Dynamic traces
    // from FGP.exe confirm the effective base/category-2 tables and the
    // 51/52/53 hotspot bindings below. These groups are also the conflict
    // owners that delay shared responses until every current idle clip
    // reaches its completion boundary.
    build.addSceneCharacterGroup(52U, 0x02U, 0x000c4818U,
                                 0x000c3ea0U, 0x000c34a8U);
    build.addSceneCharacterGroup(51U, 0x01U, 0x000c484cU,
                                 0x000c3ec4U, 0U);
    build.addSceneCharacterGroup(53U, 0x04U, 0x000c4834U,
                                 0x000c3eb4U, 0x000c34a0U);
    build.addSceneCharacterStoredTables(0x04U, 0x000c4840U,
                                        0x000c3ebcU, 0U);
    build.addSceneCharacterStoredTables(0x02U, 0x000c4828U, 0U, 0U);
    build.addSceneCharacterStoredTables(0x01U, 0x000c485cU,
                                        0x000c3ee0U, 0U);

    if (activeSceneEntryMode == 0) {
        double sequenceOffset = 0.0;
        for (const auto &definition :
             executableGameData.animationSequenceAtRva(0x000c2f14U)) {
            sequenceOffset +=
                build.appendSceneDefinition(definition, sequenceOffset);
        }
        nextTvroomConcurrentClip = decodeSmacker(
            build.sceneAnimationData("S05AN-SS-003c"), true, true);
        build.nextSceneSequenceBlocksInput = true;
        std::cout << "Recovered TV room mode-0 opening from FGP.exe ("
                  << build.nextSceneClips.size() << " stages, "
                  << sequenceOffset << " seconds)\n";
    }
}

void TvroomScene::handleTvroomAnimationFinished(std::string_view name,
                                                double completedAt) {
    if (name == "S05AN-SS-002a") {
        replaceSceneCharacterGroupTables(
            0x04U, 0x000c4840U, 0x000c3ebcU, 0U, completedAt);
        replaceSceneCharacterGroupTables(
            0x02U, 0x000c4828U, 0U, 0U, completedAt);
        replaceSceneCharacterGroupTables(
            0x01U, 0x000c485cU, 0x000c3ee0U, 0U, completedAt);
        if (tvroomConcurrentClip.has_value()) {
            const double completionDuration =
                tvroomConcurrentClip->videoDuration;
            startSceneNarration("S05AN-SS-003c", false,
                                std::move(tvroomConcurrentClip),
                                completionDuration);
            // This is an invisible Smacker stage, not speech. Its
            // AnimFinished callback is keyed to video completion.
            tvroomConcurrentFollowupPending = true;
            // Its AnimStarted event reaches 0x004276c0, which ducks the
            // music to 0.25 while it plays (0x004276d3).
            setMusicDuck(0.25F);
        }
    } else if (name == "S05AN-SS-004a1") {
        replaceSceneCharacterGroupTables(
            0x04U, 0x000c4834U, 0x000c3eb4U,
            0x000c34a0U, completedAt);
        replaceSceneCharacterGroupTables(
            0x02U, 0x000c4818U, 0x000c3ea0U,
            0x000c34a8U, completedAt);
        replaceSceneCharacterGroupTables(
            0x01U, 0x000c484cU, 0x000c3ec4U, 0U, completedAt);
    }
}

} // namespace fgp
