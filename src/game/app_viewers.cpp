#include "game/app_viewers.h"

#include <string>

namespace fgp {

void ViewerScenes::loadViewerScene(const std::string &sceneName) {
    if (sceneName == "gallery") {
        galleryViewer = loadGalleryViewer();
    } else if (sceneName == "info") {
        infoViewer = loadInfoViewer();
    } else if (sceneName == "movieplayer") {
        movieplayerViewer = loadMovieplayerViewer();
    } else if (sceneName == "anaglyph") {
        anaglyphViewer = loadAnaglyphViewer();
    } else {
        return;
    }
    // The viewers create their help and exit controls themselves
    // (0x00406ed0 ids 1/2, or 3/4 in the movie player), so the shared
    // corner buttons would only duplicate them.
    sceneCornerButtons.clear();
    pressedSceneCornerButton.reset();
}

void ViewerScenes::resetViewerScenes() {
    if (infoViewer) leaveInfoScene();
    if (movieplayerViewer) leaveMovieplayerScene();
    if (galleryViewer) leaveGalleryScene();
    if (anaglyphViewer) leaveAnaglyphScene();
    galleryViewer.reset();
    anaglyphViewer.reset();
    infoViewer.reset();
    movieplayerViewer.reset();
}

bool ViewerScenes::viewerSceneActive() const {
    return galleryViewer || infoViewer || movieplayerViewer ||
           anaglyphViewer;
}

void ViewerScenes::updateViewerScenes(double elapsed) {
    updateGallery(elapsed);
    updateInfo();
    updateMovieplayer(elapsed);
    updateAnaglyph(elapsed);
}

void ViewerScenes::renderViewerScenes(double elapsed) {
    renderGallery();
    renderInfo();
    renderMovieplayer(elapsed);
    renderAnaglyph(elapsed);
}

bool ViewerScenes::beginViewerInteraction(int x, int y) {
    return beginGalleryInteraction(x, y) || beginInfoInteraction(x, y) ||
           beginMovieplayerInteraction(x, y) ||
           beginAnaglyphInteraction(x, y);
}

bool ViewerScenes::endViewerInteraction(int x, int y) {
    return endGalleryInteraction(x, y) || endInfoInteraction(x, y) ||
           endMovieplayerInteraction(x, y) ||
           endAnaglyphInteraction(x, y);
}

bool ViewerScenes::handleViewerKey(SDL_Keycode key) {
    if (handleGalleryKey(key) || handleInfoKey(key) ||
        handleAnaglyphKey(key))
        return true;
    if (movieplayerViewer && key == SDLK_ESCAPE) {
        // The movie player leaves Esc to the shared handler 0x0040d300,
        // whose 0x0040cc30 follows the registry parent: tvstation, mode 1.
        if (!navigationHistory.empty()) navigationHistory.pop_back();
        pendingSceneEntryMode = 1;
        pendingScene = std::pair{std::string("tvstation"),
                                 std::string("tvstation")};
        return true;
    }
    return false;
}

bool ViewerScenes::executeViewerDeveloperCommand(std::string_view command) {
    const auto split = command.find(':');
    if (split == std::string_view::npos) return false;
    const auto scene = command.substr(0, split);
    auto action = command.substr(split + 1);
    // "<scene>:at:<seconds>:<action>" waits (by refusing, so the command
    // is retried next frame) until the scene has run that long.
    if (action.rfind("at:", 0) == 0) {
        const auto timeEnd = action.find(':', 3);
        if (timeEnd == std::string_view::npos) return false;
        const double due = std::stod(std::string(action.substr(3, timeEnd - 3)));
        if (activeScene != scene || sceneElapsedNow() < due) return false;
        action = action.substr(timeEnd + 1);
    }
    if (scene == "gallery") return executeGalleryDeveloperCommand(action);
    if (scene == "info") return executeInfoDeveloperCommand(action);
    if (scene == "movieplayer") return executeMovieplayerDeveloperCommand(action);
    if (scene == "anaglyph") return executeAnaglyphDeveloperCommand(action);
    return false;
}

} // namespace fgp
