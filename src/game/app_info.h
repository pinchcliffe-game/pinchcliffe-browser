#pragma once

#include "game/app_viewer.h"

namespace fgp {

// The "info" activity (Infosenter): a shared article viewer the album and
// the racing menu open with a parameter record. Implemented in
// app_info.cpp; the scene callback is FGP.exe 0x004691f0.
class InfoScene : protected virtual ViewerSceneSupport {
protected:
    struct InfoLine {
        std::string text;
        int font{};
    };
    struct InfoViewer {
        ViewerWidgets widgets;
        std::vector<std::string> sections;
        LanguageTable table;
        bool entered{};
        // 0x006b967c: the shown page.
        int page{};
        bool pictureAdded{};
        std::string pictureAsset;
        SurfacePtr picture;
        TexturePtr pictureTexture;
        SDL_Point picturePosition{};
        std::vector<InfoLine> lines;
    };

    // What a caller does before GAME_GotoSceneByName("info", &record).
    void requestInfo(InfoRequest request);
    InfoViewer loadInfoViewer();
    void updateInfo();
    void renderInfo();
    bool beginInfoInteraction(int x, int y);
    bool endInfoInteraction(int x, int y);
    bool handleInfoKey(SDL_Keycode key);
    // Event 0x104 (0x00469760): the page goes back into the record.
    void leaveInfoScene();
    bool executeInfoDeveloperCommand(std::string_view command);

    std::optional<InfoViewer> infoViewer;

private:
    void showInfoPage(int page);
    int nextInfoPage(int step);
    void infoControl(int id);
    void returnFromInfo();
};

} // namespace fgp
