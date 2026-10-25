#include "game/app_intent.h"

#include <array>
#include <cstdint>
#include <utility>

namespace fgp {

void IntentScene::buildIntentScene(SceneBuild &build) {
    // Like TV Room, normal Inside Tent entry composes several initializer
    // functions and is intentionally absent from the conservative
    // generated group table. Original traces consistently activate these
    // three base/category-2 pairs in Sli/Bent/EmL order.
    build.addSceneCharacterGroup(51U, 0x00000400U, 0x000bd364U,
                                 0x000bd3acU, 0U);
    build.addSceneCharacterGroup(50U, 0x00000100U, 0x000bd37cU,
                                 0x000bd388U, 0x000bd3c0U);
    build.addSceneCharacterGroup(52U, 0x00000200U, 0x000bd370U,
                                 0x000bd398U, 0x000bd3d4U);

    build.addSceneCharacterStoredTables(0x00000400U, 0x000bd3e0U, 0U, 0U);
    build.addSceneCharacterStoredTables(0x00000200U, 0x000bd3f8U, 0U, 0U);
    build.addSceneCharacterStoredTables(0x00000100U, 0x000bd410U, 0U, 0U);
    build.addSceneCharacterStoredTables(0x00000200U, 0x000bd440U, 0U, 0U);
    build.addSceneCharacterStoredTables(0x00000100U, 0x000bd428U, 0U, 0U);

    for (const auto [bit, table] :
         std::array<std::pair<std::uint32_t, std::uint32_t>, 9>{{
             {0x00000400U, 0x000bd3e8U},
             {0x00000200U, 0x000bd3f0U},
             {0x00000100U, 0x000bd408U},
             {0x00000200U, 0x000bd438U},
             {0x00000100U, 0x000bd420U},
             {0x00000200U, 0x000bd448U},
             {0x00000100U, 0x000bd430U},
             {0x00000200U, 0x000bd400U},
             {0x00000100U, 0x000bd418U},
         }}) {
        build.addSceneCharacterCategory4Table(bit, table);
    }

    if (activeSceneEntryMode == 0) {
        build.appendSceneDefinition(
            executableGameData.animationAtRva(0x000bb070U), 0.0);
        build.nextSceneSequenceBlocksInput = true;
    }
}

void IntentScene::handleIntentAnimationFinished(std::string_view name,
                                                double completedAt) {
    if (name == "S10AN-SLI-T01") {
        replaceSceneCharacterGroupTables(
            0x00000400U, 0x000bd3e0U, 0U, 0U, completedAt);
        replaceSceneCharacterGroupTables(
            0x00000200U, 0x000bd3f8U, 0U, 0U, completedAt);
        startSceneCharacterCategory4(
            0x00000200U, 0x000bd3f0U, completedAt);
        replaceSceneCharacterGroupTables(
            0x00000100U, 0x000bd410U, 0U, 0U, completedAt);
        startSceneCharacterCategory4(
            0x00000100U, 0x000bd408U, completedAt);
    } else if (name == "S10AN-SS-006a") {
        // 0x00425bd8 ducks the music to 0.25 (0x3e800000). It then starts
        // S10AN-SS-006b (0x00425be2), which native does not play yet.
        setMusicDuck(0.25F);
        replaceSceneCharacterGroupTables(
            0x00000200U, 0x000bd440U, 0U, 0U, completedAt);
        startSceneCharacterCategory4(
            0x00000200U, 0x000bd438U, completedAt);
        replaceSceneCharacterGroupTables(
            0x00000100U, 0x000bd428U, 0U, 0U, completedAt);
        startSceneCharacterCategory4(
            0x00000100U, 0x000bd420U, completedAt);
    } else if (name == "S10AN-SS-006c") {
        // 0x00425c41 restores the music before removing SS-006b.
        setMusicDuck(1.0F);
        replaceSceneCharacterGroupTables(
            0x00000200U, 0x000bd370U, 0x000bd398U,
            0x000bd3d4U, completedAt);
        startSceneCharacterCategory4(
            0x00000200U, 0x000bd448U, completedAt);
        replaceSceneCharacterGroupTables(
            0x00000100U, 0x000bd37cU, 0x000bd388U,
            0x000bd3c0U, completedAt);
        startSceneCharacterCategory4(
            0x00000100U, 0x000bd430U, completedAt);
    }
}

void IntentScene::handleIntentAnimationStarted(std::string_view name,
                                               double startedAt) {
    // The AnimStarted handler 0x00425b20 (event 0x10f, dispatched from
    // 0x004256f7) acts only on S10AN-SS-002b: it ducks the music to 0.25
    // (0x00425b33) and arms timer 0 at the clock plus 0xA7F8 ms
    // (0x00425b3b-0x00425b52), whose expiry 0x004258c0 restores it.
    if (name != "S10AN-SS-002b") {
        return;
    }
    setMusicDuck(0.25F);
    intentGroupRestoreAt = startedAt + 43.0;
    referenceTrace.emit("diagnostic", {
        {"kind", "intent_group_restore_timer"},
        {"deadline_ms", static_cast<int>(
            *intentGroupRestoreAt * 1000.0)},
    });
}

} // namespace fgp
