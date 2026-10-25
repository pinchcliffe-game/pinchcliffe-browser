#pragma once

#include "game/app_anaglyph.h"
#include "game/app_gallery.h"
#include "game/app_info.h"
#include "game/app_movieplayer.h"

namespace fgp {

// The media viewers (gallery, info, movieplayer, anaglyph) behind one set of hooks,
// so GameApp's frame, input and scene loading dispatch to them in one
// place. Implemented in app_viewers.cpp.
class ViewerScenes : protected GalleryScene,
                     protected InfoScene,
                     protected MovieplayerScene,
                     protected AnaglyphScene {
protected:
    // Builds the viewer for a scene loadScene has just activated.
    void loadViewerScene(const std::string &sceneName);
    // Event 0x104 for the viewer being left, then drops its state.
    void resetViewerScenes();
    void updateViewerScenes(double elapsed);
    void renderViewerScenes(double elapsed);
    bool beginViewerInteraction(int x, int y);
    bool endViewerInteraction(int x, int y);
    bool handleViewerKey(SDL_Keycode key);
    bool viewerSceneActive() const;
    // "gallery:...", "info:...", "movieplayer:..." and "anaglyph:..."
    // commands.
    bool executeViewerDeveloperCommand(std::string_view command);
};

} // namespace fgp
