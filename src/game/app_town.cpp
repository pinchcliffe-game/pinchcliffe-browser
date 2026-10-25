#include "game/app_town.h"

#include <iostream>
#include <string>

namespace fgp {

// Town's callback 0x00426f30 dispatches four events: BeginScene to
// 0x00426fc0, scene exit to 0x00407210 (which removes the modal mesh), the
// key handler 0x00427010 and AnimFinished 0x00427050. The entry mode is kept
// at 0x005b8a0c. Intent's Next enters with mode 2; Morning's "No" and the
// Mountain return use mode 0.
void TownScene::buildTownScene(SceneBuild &build) {
    double sequenceOffset = 0.0;
    if (build.entryMode == 2) {
        // 0x00426ffa starts ben. When it finishes, 0x00427072-0x0042709b
        // removes it, adds the frame bitmap at layer 7, starts reel1, stops
        // the music (0x0040b2f0) and adds the modal mesh at layer 5
        // (0x00407110). The frame is only drawn from then on
        // (townReelShowing).
        build.addMenuBitmap(*build.nextPackage, "frame", 206, 143);
        sequenceOffset += build.appendSceneDefinition(
            executableGameData.animationAtRva(0x000c1478U),
            sequenceOffset);
        sequenceOffset += build.appendSceneDefinition(
            executableGameData.animationAtRva(0x000c15c8U),
            sequenceOffset);
    } else {
        // 0x00426fec starts bike for every other mode.
        sequenceOffset += build.appendSceneDefinition(
            executableGameData.animationAtRva(0x000c1328U),
            sequenceOffset);
    }
    // The initializer hides the cursor (0x00426fd6 passes "" to 0x0040c370),
    // so the mouse is swallowed for the whole scene.
    build.nextSceneSequenceBlocksInput = true;
    std::cout << "Recovered town route mode " << build.entryMode << " ("
              << build.nextSceneClips.size() << " stage(s), "
              << sequenceOffset << " seconds)\n";
}

void TownScene::handleTownAnimationFinished(std::string_view name) {
    if (name == "ben") {
        referenceTrace.emit("animation_add", {
            {"asset", "frame"}, {"x", -1}, {"y", -1}, {"layer", 7},
            {"hotspot", 0}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_start", {
            {"asset", "frame"}, {"x", -1}, {"y", -1}, {"layer", 7},
            {"loop", false}});
        audioMixer.setBackground(DecodedMedia{}, 0.0F);
        if (!sceneBackgroundMusicAsset.empty()) {
            referenceTrace.emit("audio_stop", {
                {"asset", sceneBackgroundMusicAsset}, {"role", "music"},
                {"reason", "stopped"}});
        }
    } else if (name == "reel1") {
        // 0x004270ad
        enterScene("buildacar", "buildacar", 0);
    } else if (name == "bike") {
        // 0x0042705d
        enterScene("outtent", "outtent", 0);
    }
}

bool TownScene::townReelShowing() const {
    return viewMode == ViewMode::Scene && activeScene == "town" &&
           !sceneSequenceFinished && sceneClipIndex < sceneClips.size() &&
           sceneClips[sceneClipIndex].name == "reel1";
}

} // namespace fgp
