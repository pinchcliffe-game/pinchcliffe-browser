#include "game/app_tvstation.h"

namespace fgp {

void TvstationScene::buildTvstationScene(SceneBuild &build) {
    build.appendSceneDefinition(
        executableGameData.animationAtRva(0x000c49c8U), 0.0);
    double narrationOffset = 0.0;
    narrationOffset += build.appendSceneNarration(
        "S12AN-NAR-001", narrationOffset);
    if (build.sceneVisitCount <= 1U) {
        build.appendSceneNarration("S12AN-NAR-002", narrationOffset);
    }
    build.nextSceneSequenceBlocksInput = true;
}

} // namespace fgp
