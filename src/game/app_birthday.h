#pragma once

#include <array>
#include <string>
#include <vector>

#include "game/app_core.h"

namespace fgp {

// The "birthday" scene when a profile's birthday is today: the greeting,
// track14 and timed fireworks and stars over the night backdrop. Implemented
// in app_birthday.cpp; the dispatcher is FGP.exe 0x00414fe0. Without a match
// the scene's 0x103 handler dispatches the menu at once (app_scene.cpp).
class BirthdayScene : protected virtual AppCore {
protected:
    struct BirthdayClip {
        std::string name;
        int x{};
        int y{};
        int layer{};
        bool keyed{};
        bool active{};
        double startedAt{};
        HopscotchMedia clip;
    };
    struct BirthdayGame {
        // 0x00555150: the profiles 0x00414f20 matched, in scan order.
        std::vector<std::string> names;
        // Descriptor 0x0055ac80 (layer 50).
        TexturePtr text;
        SDL_Rect textRect{};
        // 0x004a97f0 (fireworks, layer = index) and 0x004a9330 (stars,
        // layer = index - 100).
        std::array<BirthdayClip, 7> fireworks;
        std::array<BirthdayClip, 9> stars;
        double fireworkDue{};
        double starDue{};
        bool entered{};
        // Set once 0x004154a0 (event 0x104) has run; the menu follows the
        // last award it opens.
        bool leaving{};
        std::vector<std::string> pendingAwards;
    };

    void loadBirthdayGame(std::vector<std::string> names);
    void updateBirthday(double elapsed);
    void renderBirthday(double elapsed);
    // 0x00418980 (event 0x10c): Space leaves for the parent scene.
    bool handleBirthdayKey(SDL_Keycode key);
    // 0x004154a0 (event 0x104), then the menu once its awards are closed.
    void leaveBirthday();
    // An award opened by leaveBirthday was closed: open the next or go on
    // to the menu.
    bool continueBirthdayLeave();

    std::optional<BirthdayGame> birthdayGame;

private:
    void startBirthdayClip(BirthdayClip &clip, double elapsed);
};

} // namespace fgp
