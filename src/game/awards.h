#pragma once

// The profile's award section (record 0x301: eight u32 values) as the
// original's award helpers at 0x0041b3d0-0x0041b740 read it. The tables come
// from FGP.exe itself.

#include <cstdint>
#include <string>

#include "assets/executable_data.h"

namespace fgp {

// Award types, the index into the eight values.
enum AwardType : int {
    kAwardCarParts = 0,
    kAwardFilms = 1,
    kAwardCars = 2,
    kAwardTracks = 3,
    kAwardImages = 4,
    kAwardCharacters = 5,
    kAwardButterflies = 7,
};

// 0x0049ca88: how many items a type has.
int awardItemCount(const ExecutableGameData &exe, int type);
// 0x0041b5d0: whether item is available, given the profile's value. Types 0
// and 6 count items; the others are bit sets that also include the type's
// initial value (0x0049ca60).
bool awardItemAvailable(const ExecutableGameData &exe, int type,
                        std::uint32_t value, int item);
// 0x0041b6e0: the value with item granted.
std::uint32_t awardItemGrant(const ExecutableGameData &exe, int type,
                             std::uint32_t value, int item);
// 0x0041b3d0: a butterfly's index in the album order (0x0049cab0), or -1.
int butterflyAwardOrder(const ExecutableGameData &exe, int item);
// 0x0041b580: the item's language key (awarddlg:...), or awarddlg:NONE.
std::string awardItemNameKey(const ExecutableGameData &exe, int type,
                             int item);
// 0x004231e0: the Build-a-Car level (0..5) for the placed-part count of the
// buildacar record, from the stage sizes at 0x0049cc70.
int buildacarLevel(const ExecutableGameData &exe, std::uint32_t placedParts);

} // namespace fgp
