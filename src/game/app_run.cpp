#include "game/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "game/menu_names.h"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

int GameApp::run() {
    while (step()) {
    }
    shutdown();
    return 0;
}

// One iteration of the main loop. Desktop builds call it from run(); the
// browser build calls it once per animation frame, because a page cannot
// block in a loop of its own.
bool GameApp::step() {
    if (!running) {
        return false;
    }
    if (developerExitAfter >= 0.0 &&
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - developerRunStart)
                .count() >= developerExitAfter) {
        running = false;
        return false;
    }
    handleEvents();
    renderFrame();
    if (developerCommandIndex < developerCommands.size() &&
        executeDeveloperCommand(developerCommands[developerCommandIndex])) {
        ++developerCommandIndex;
    }
    return running;
}

void GameApp::shutdown() {
    SDL_CloseAudioDevice(audioDevice);
    saveSynonymRanks();
    saveTriviaRanks();
    saveBouquet();
    saveColorfillState();
    colorfillGame.reset();
    sceneClips.clear();
    sceneCharacterGroups.clear();
    sceneAnimationObjects.clear();
    recoveredMinigameRuntime.reset();
    whackamoleGame.reset();
    balloonhuntGame.reset();
    pipelineGame.reset();
    hopscotchGame.reset();
    beemazeGame.reset();
    bugzzzGame.reset();
    wheelbarrowGame.reset();
    goodbyeState.reset();
    outroState.reset();
    buildabikeGame.reset();
    mountainGame.reset();
    hustleGame.reset();
    chessGame.reset();
    minigameBusyCursor = false;
    jigsawGame.reset();
    slidingGame.reset();
    fenceGame.reset();
    resetViewerScenes();
    puzzleGame.reset();
    draughtsGame.reset();
    textinvaderGame.reset();
    synonymGame.reset();
    mathlabGame.reset();
    mainscoreBoard.reset();
    tournamentScreen.reset();
    triviaGame.reset();
    peeParticleField.reset();
    sceneBitmapLayers.clear();
    sceneCornerButtons.clear();
    for (auto &button : menuCornerButtons) {
        button.pressedTexture.reset();
        button.normalTexture.reset();
        button.normalSurface.reset();
    }
    sceneCollectibles.clear();
    sceneIndexTiles.clear();
    sceneIndexTileBounds.clear();
    sceneIndexMedals.clear();
    sceneIndexMedalBounds.clear();
    sceneIndexGoldFrameTexture.reset();
    for (auto &texture : sceneIndexTabTextures) {
        texture.reset();
    }
    sceneTitleTexture.reset();
    sceneHotspotTexture.reset();
    sceneBackdropTexture.reset();
    sceneHotspots.reset();
    sceneBackdrop.reset();
    scenePackage.reset();
    sceneEditionPackage.reset();
    sceneData2Package.reset();
    sceneAddonPackage.reset();
    sceneLanguagePackage.reset();
    for (auto &cursor : cursors) {
        cursor.texture.reset();
    }
    menuEffects.clear();
    menuClips.clear();
    dialogFont.texture.reset();
    dialogFont.surface.reset();
    calendarFont.texture.reset();
    calendarFont.surface.reset();
    menuTitleFont.texture.reset();
    menuTitleFont.surface.reset();
    messageFont.texture.reset();
    messageFont.surface.reset();
    settingsTogglePressedTexture.reset();
    settingsToggleNormalTexture.reset();
    settingsThumbTexture.reset();
    settingsUpPressedTexture.reset();
    settingsUpNormalTexture.reset();
    settingsDownPressedTexture.reset();
    settingsDownNormalTexture.reset();
    settingsButtonPressedTexture.reset();
    settingsButtonNormalTexture.reset();
    settingsBackdropTexture.reset();
    settingsTogglePressed.reset();
    settingsToggleNormal.reset();
    settingsThumb.reset();
    settingsUpPressed.reset();
    settingsUpNormal.reset();
    settingsDownPressed.reset();
    settingsDownNormal.reset();
    settingsButtonPressed.reset();
    settingsButtonNormal.reset();
    settingsBackdrop.reset();
    helpThumbTexture.reset();
    helpDownPressedTexture.reset();
    helpDownNormalTexture.reset();
    helpUpPressedTexture.reset();
    helpUpNormalTexture.reset();
    helpNextPressedTexture.reset();
    helpNextNormalTexture.reset();
    helpPreviousPressedTexture.reset();
    helpPreviousNormalTexture.reset();
    helpButtonPressedTexture.reset();
    helpButtonNormalTexture.reset();
    helpBackdropTexture.reset();
    helpThumb.reset();
    helpDownPressed.reset();
    helpDownNormal.reset();
    helpUpPressed.reset();
    helpUpNormal.reset();
    helpNextPressed.reset();
    helpNextNormal.reset();
    helpPreviousPressed.reset();
    helpPreviousNormal.reset();
    helpButtonPressed.reset();
    helpButtonNormal.reset();
    helpBackdrop.reset();
    messageButtonPressedTexture.reset();
    messageButtonNormalTexture.reset();
    messageBackdropTexture.reset();
    messageButtonPressed.reset();
    messageButtonNormal.reset();
    messageBackdrop.reset();
    awardIconTexture.reset();
    awardMedalTexture.reset();
    awardButtonPressedTexture.reset();
    awardButtonNormalTexture.reset();
    awardBackdropTexture.reset();
    awardButtonPressed.reset();
    awardButtonNormal.reset();
    awardBackdrop.reset();
    highScoreClosePressedTexture.reset();
    for (auto &texture : highScoreSelectedTabTextures) {
        texture.reset();
    }
    highScoreBackdropTexture.reset();
    highScoreClosePressed.reset();
    for (auto &surface : highScoreSelectedTabs) {
        surface.reset();
    }
    highScoreBackdrop.reset();
    for (auto &button : storyNavigationButtons) {
        button.disabledTexture.reset();
        button.pressedTexture.reset();
        button.normalTexture.reset();
        button.normalSurface.reset();
    }
    storyNavigationBackdropTexture.reset();
    storyNavigationBackdrop.reset();
    calendarOkTexture.reset();
    calendarNextTexture.reset();
    calendarPreviousTexture.reset();
    calendarMarkTexture.reset();
    calendarBackdropTexture.reset();
    calendarOk.reset();
    calendarNext.reset();
    calendarPrevious.reset();
    calendarMark.reset();
    calendarHotspots.reset();
    calendarBackdrop.reset();
    nameDialogCancelTexture.reset();
    nameDialogOkTexture.reset();
    nameDialogBackdropTexture.reset();
    nameDialogCancel.reset();
    nameDialogOk.reset();
    nameDialogHotspots.reset();
    nameDialogBackdrop.reset();
    animationTexture.reset();
    modalMeshTexture.reset();
    titleTexture.reset();
    hotspotTexture.reset();
    backdropTexture.reset();
    hotspots.reset();
    title.reset();
    backdrop.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    referenceTrace.finish(0);
}

} // namespace fgp
