#pragma once

#include "game/app_viewer.h"

namespace fgp {

// The "gallery" activity (Skuespillere): six character portraits, each
// followed by a spoken introduction. Implemented in app_gallery.cpp; the
// scene callback is FGP.exe 0x00468e70.
class GalleryScene : protected virtual ViewerSceneSupport {
protected:
    struct GalleryViewer {
        ViewerWidgets widgets;
        std::array<SurfacePtr, 6> portraits;
        std::array<TexturePtr, 6> portraitTextures;
        // 0x006b9670: the shown portrait, 0..5.
        int index{};
        bool entered{};
        bool portraitAdded{};
        // 0x006b9674: the pending speech timer (tag 1), as its due time.
        std::optional<double> speechTimerDue;
        // The speech descriptor 0x005097c0 while it is added.
        bool speechAdded{};
        double speechStartedAt{};
        double speechDuration{};
        DecodedMedia speech;
        LiveAudioMixer::EffectHandle speechEffect{
            LiveAudioMixer::kInvalidEffectHandle};
    };

    GalleryViewer loadGalleryViewer();
    void updateGallery(double elapsed);
    void renderGallery();
    bool beginGalleryInteraction(int x, int y);
    bool endGalleryInteraction(int x, int y);
    bool handleGalleryKey(SDL_Keycode key);
    // Developer control API for "gallery:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeGalleryDeveloperCommand(std::string_view command);
    // Leaving the scene silences a speech still playing.
    void leaveGalleryScene();

    std::optional<GalleryViewer> galleryViewer;

private:
    void showGalleryPortrait(int index, double now);
    void cancelGallerySpeechTimer();
    void removeGallerySpeech();
    void galleryControl(int id);
    void leaveGallery();
};

} // namespace fgp
