#include "game/picture_library.h"

#include <cstdio>
#include <system_error>

#include "util/binary.h"

namespace fgp {

AwardInventory profileAwardInventory(const fs::path &profilePath) {
    AwardInventory inventory{};
    std::error_code error;
    if (profilePath.empty() || !fs::exists(profilePath, error)) {
        return inventory;
    }
    const auto bytes = readBinaryFile(profilePath);
    // 0x00411b70 reads record type 0x301, key 0, 32 bytes; the profile
    // writer keeps that record at 0x4c (see awardCarPart in profile.cpp).
    if (bytes.size() < 0x78U || readLe32(bytes, 0x4c) != 0x301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        return inventory;
    }
    for (std::size_t index = 0; index < inventory.size(); ++index) {
        inventory[index] = readLe32(bytes, 0x58 + index * 4U);
    }
    return inventory;
}

int pictureGroupOf(int index) {
    // 0x0041b446-0x0041b45b: index < base[i + 1] for the five groups.
    if (index < 0) return -1;
    for (std::size_t group = 0; group < kPictureGroups.size(); ++group) {
        const auto &entry = kPictureGroups[group];
        if (index < entry.first + entry.count) return static_cast<int>(group);
    }
    return -1;
}

std::string pictureFileName(int index) {
    const int group = pictureGroupOf(index);
    if (group < 0) return {};
    const auto &entry = kPictureGroups[static_cast<std::size_t>(group)];
    char number[8];
    std::snprintf(number, sizeof number, "%02d", index - entry.first + 1);
    return std::string(entry.filePrefix) + number;
}

std::string pictureTitleKey(int index) {
    const int group = pictureGroupOf(index);
    if (group < 0) return "NONE";
    const auto &entry = kPictureGroups[static_cast<std::size_t>(group)];
    return std::string(entry.titlePrefix) +
           std::to_string(index - entry.first + 1);
}

bool pictureAvailable(int index, const AwardInventory &inventory) {
    const int group = pictureGroupOf(index);
    if (group < 0) return false;
    const auto &entry = kPictureGroups[static_cast<std::size_t>(group)];
    // 0x0041b40f: category 9 (the 3D images) is always available.
    if (entry.category == 9) return true;
    // 0x0041b5fb/0x0041b660: a bit of the starting mask or of the owned
    // mask for the category.
    const int item = index - entry.first;
    const std::uint32_t owned =
        inventory[static_cast<std::size_t>(entry.category)];
    return (((entry.initialMask >> item) & 1U) != 0U) ||
           (((owned >> item) & 1U) != 0U);
}

std::pair<int, int> pictureCategoryRange(int category) {
    for (const auto &entry : kPictureGroups) {
        if (entry.category == category) {
            return {entry.first, entry.first + entry.count};
        }
    }
    return {0, 0};
}

} // namespace fgp
