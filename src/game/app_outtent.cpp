#include "game/app_outtent.h"

#include <algorithm>
#include <iostream>

namespace fgp {

namespace {

// The bitmap players 0x004260d0 and the AnimStarted/AnimFinished handlers
// add and remove (SCENE_AddAnim 0x00412840 with the descriptor's own x/y):
// descriptor, asset, x, y (descriptor +0x54/+0x58), layer passed to AddAnim.
struct OuttentBitmap {
    std::uint32_t descriptor;
    const char *asset;
    const char *traceName;
    int x;
    int y;
    int layer;
};
constexpr OuttentBitmap kOuttentBitmaps[] = {
    // 0x004261f2: mode 0 only, under everything.
    {0x004bdf50U, "paper", "paper", 506, 369, -20},
    // 0x004262c6: when SS-001a finishes. The bike exists only in SS-001a
    // and SS-001b's own frames until then.
    {0x004bde00U, "bike", "bike", 491, 380, -20},
    // 0x00426262: while SS-D01 or SS-D02 plays. The descriptor has no name;
    // the initializer points its surface (+0x8c, 0x004260e4) at backdrop2,
    // the view through the binoculars.
    {0x004bdd58U, "backdrop2", "", 0, 0, -10},
    // 0x004262ab: when SS-007a finishes, until SS-007b starts (0x00426249).
    {0x004bdea8U, "banana", "banana", 562, 476, 0},
};

} // namespace

void OuttentScene::setOuttentBitmap(std::string_view asset, bool visible) {
    for (const auto &definition : kOuttentBitmaps) {
        if (asset != definition.asset) {
            continue;
        }
        for (auto &bitmap : sceneBitmapLayers) {
            if (bitmap.name != asset || bitmap.visible == visible) {
                continue;
            }
            bitmap.visible = visible;
            if (visible) {
                referenceTrace.emit("animation_add", {
                    {"asset", definition.traceName}, {"x", -1}, {"y", -1},
                    {"layer", definition.layer}, {"hotspot", 0},
                    {"conflict_mask", 0}});
                referenceTrace.emit("animation_start", {
                    {"asset", definition.traceName}, {"x", -1}, {"y", -1},
                    {"layer", definition.layer}, {"loop", false}});
            } else {
                referenceTrace.emit("animation_remove", {
                    {"asset", definition.traceName}, {"reason", "removed"}});
            }
        }
    }
}

void OuttentScene::buildOuttentScene(SceneBuild &build) {
    for (const auto &definition : kOuttentBitmaps) {
        build.addMenuBitmap(*build.nextPackage, definition.asset,
                            definition.x, definition.y);
        build.nextSceneBitmapLayers.back().layer = definition.layer;
        build.nextSceneBitmapLayers.back().visible =
            definition.descriptor == 0x004bdf50U && build.entryMode == 0;
    }
    // S09AN-SS-D01a, the sound the first SS-D01 or SS-D02 of a visit starts
    // (0x0042627e); the initializer clears the count at 0x00426131.
    build.addScenePlayer(0x000be7d8U, false);
    outtentD01aStarted = false;
    double sequenceOffset = 0.0;
    // 0x004261f0: only entry mode 0 runs the opening table 0x004bfb68; any
    // other mode seeds the groups (0x0042621a) with no opening.
    if (build.entryMode == 0) {
        for (const auto &definition :
             executableGameData.animationSequenceAtRva(0x000bfb68U)) {
            sequenceOffset +=
                build.appendSceneDefinition(definition, sequenceOffset);
        }
    }
    build.nextSceneSequenceBlocksInput = build.entryMode == 0;
    std::cout << "Recovered outtent opening from FGP.exe ("
              << build.nextSceneClips.size() << " stages, "
              << sequenceOffset << " seconds)\n";
}

void OuttentScene::handleOuttentAnimationStarted(std::string_view name) {
    // 0x00426230 compares the started clip with SS-D01 (0x004be688) and
    // SS-D02 (0x004be730) and writes 0 to the music factor at 0x0042625d.
    // It also adds the image descriptor 0x004bdd58 at layer -10 (the
    // binocular view) and, the first time per visit ([0x005b87ac]), starts
    // S09AN-SS-D01a.
    if (name == "S09AN-SS-D01" || name == "S09AN-SS-D02") {
        setMusicDuck(0.0F);
        setOuttentBitmap("backdrop2", true);
        if (!outtentD01aStarted) {
            outtentD01aStarted = true;
            playSceneAnimationObject("S09AN-SS-D01a");
        }
    } else if (name == "S09AN-SS-007b") {
        setOuttentBitmap("banana", false);
    }
}

void OuttentScene::handleOuttentAnimationFinished(std::string_view name) {
    // 0x004262da/0x004262e1 route either clip's end to 0x00426360, which
    // restores the music (0x00426369) and removes 0x004bdd58.
    if (name == "S09AN-SS-D01" || name == "S09AN-SS-D02") {
        setMusicDuck(1.0F);
        setOuttentBitmap("backdrop2", false);
    } else if (name == "S09AN-SS-001a") {
        setOuttentBitmap("bike", true);
    } else if (name == "S09AN-SS-001d1") {
        // 0x004262e8: Solan rides off with the bike.
        setOuttentBitmap("paper", false);
        setOuttentBitmap("bike", false);
    } else if (name == "S09AN-SS-007a") {
        setOuttentBitmap("banana", true);
    }
}

} // namespace fgp
