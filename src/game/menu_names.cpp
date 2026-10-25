#include "game/menu_names.h"

namespace fgp {

std::string menuHotspotName(std::uint32_t id) {
    switch (id) {
    case 1: return "Help";
    case 2: return "Quit";
    case 3: return "Settings";
    case 4: return "Scoreboard";
    case 5: return "Start";
    case 6: return "Contents";
    case 7: return "Tournament";
    case 8: return "Racing";
    case 9: return "Add profile";
    case 10: return "Remove profile";
    case 11: return "Previous profile";
    case 12: return "Next profile";
    case 13: return "Credits";
    case 14: return "Hidden car hotspot";
    default: return {};
    }
}

} // namespace fgp
