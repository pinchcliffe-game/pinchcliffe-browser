#pragma once

#include <array>
#include <memory>
#include <string>

#include "game/activity_panel.h"
#include "game/app_core.h"

namespace fgp {

// Butterfly's album ("buttercol", Sommerfuglsamlingen): the species the
// profile has caught, each opening its info page. Implemented in
// app_buttercol.cpp, a port of FGP.exe 0x0043a230-0x0043a404.
class ButtercolScene : protected virtual AppCore {
protected:
    struct ButtercolAlbum {
        std::unique_ptr<ActivityStage> stage;
        std::unique_ptr<ActivityPanel> panel;
        // The fifteen specimen descriptors of the table at 0x004d8bd0.
        std::array<int, 15> specimens{};
        // 0x00666c60: which species are shown.
        std::array<bool, 15> collected{};
        // Progress through the LOVELYFLIES key sequence (0x0040ae60).
        std::size_t cheatProgress{};
        std::uint32_t hotspot{};
        bool announced{};
    };

    ButtercolAlbum loadButtercolAlbum();
    bool handleButtercolEvent(const SDL_Event &event);
    void updateButtercol(double elapsed);
    void renderButtercol();
    // Developer control API for "buttercol:..." commands.
    bool executeButtercolDeveloperCommand(std::string_view command);

    std::optional<ButtercolAlbum> buttercolAlbum;

private:
    void announceButtercolEntry();
    SurfacePtr buttercolBitmap(std::string_view name);
    std::uint32_t buttercolHotspotAt(int x, int y);
    void buttercolHotspotChanged(std::uint32_t hotspot);
    void showButtercolSpecimens();
    void openButtercolSpecimen(std::uint32_t hotspot);
    void buttercolControlClicked(std::uint32_t id);
    void buttercolKey(SDL_Keycode key);
};

} // namespace fgp
