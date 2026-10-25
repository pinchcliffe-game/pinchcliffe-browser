#pragma once

// The picture library that `jigsaw`, `sliding` and `activity` draw from:
// five parallel tables in FGP.exe's .data plus the profile-global award
// inventory. Implemented in picture_library.cpp; tools/fgp_picture_library_scan.py
// recovers the same tables from the executable and the tests pin them.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace fgp {
namespace fs = std::filesystem;

struct PictureGroup {
    // 0x004adc18: the sprintf pattern's prefix ("image_%02d" -> "image_").
    std::string_view filePrefix;
    // 0x004ad89c: the award-name key prefix in [awarddlg] ("IMAGE_").
    std::string_view titlePrefix;
    // 0x004adc04: the award category that owns the group.
    int category{};
    // 0x004adc2c: the group's first library index.
    int first{};
    int count{};
    // 0x0049ca60: the bitmask of items available before any award.
    std::uint32_t initialMask{};
};

constexpr std::array<PictureGroup, 5> kPictureGroups{{
    {"image_", "IMAGE_", 4, 0, 31, 0xffU},
    {"car_", "CAR_", 2, 31, 10, 0x1U},
    {"track_", "TRACK_", 3, 41, 8, 0x1U},
    {"butterfly_", "BUTTERFLY_", 7, 49, 15, 0x0U},
    {"3dimage_", "3DIMAGE_", 9, 64, 15, 0x0U},
}};
// [0x004adc40], returned by 0x0041b4e0.
constexpr int kPictureCount = 79;

// The profile-global award inventory: record type 0x301, key 0, eight u32
// values indexed by award category, at profile offset 0x58 (0x00411b70).
using AwardInventory = std::array<std::uint32_t, 8>;
AwardInventory profileAwardInventory(const fs::path &profilePath);

// 0x0041b440: the group ordinal of a library index, or -1.
int pictureGroupOf(int index);
// 0x0041b4a0: "image_01" etc.; the file numbering is one-based (0x0041b4ca).
std::string pictureFileName(int index);
// 0x0041b460 -> 0x0041b580: the [awarddlg] key of the localised title.
std::string pictureTitleKey(int index);
// 0x0041b3f0: whether the player may choose the picture.
bool pictureAvailable(int index, const AwardInventory &inventory);
// 0x0041b4f0: the [first, end) library range of one award category, used
// when a scene is entered with a category argument (anaglyph passes 9).
std::pair<int, int> pictureCategoryRange(int category);

// 0x0046bad0 and 0x0046eb20: the board is square and its size is a pure
// function of the library index.
constexpr int jigsawGridSize(int index) { return index % 4 + 3; }
constexpr int slidingGridSize(int index) { return index % 3 + 3; }

// The statically linked MSVC rand() at 0x0048d323 (per-thread state at
// _getptd()+0x14, seeded by srand 0x0048d316) and FGP's two wrappers.
struct MsvcRandom {
    std::uint32_t state{1};
    void seed(std::uint32_t value) { state = value; }
    int next() {
        state = state * 0x343fdU + 0x269ec3U;
        return static_cast<int>((state >> 16) & 0x7fffU);
    }
    // 0x0040c530: rand() % n, and 0 for n == 0.
    int below(int n) { return n != 0 ? next() % n : 0; }
    // 0x0040c570: 1 iff rand() % n == 0.
    bool oneIn(int n) { return below(n) == 0; }
};

} // namespace fgp
