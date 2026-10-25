#include "game/awards.h"

namespace fgp {

namespace {

constexpr std::uint32_t kAwardInitial = 0x0049ca60U;
constexpr std::uint32_t kAwardCounts = 0x0049ca88U;
constexpr std::uint32_t kButterflyOrder = 0x0049cab0U;
constexpr std::uint32_t kStageSizes = 0x0049cc70U;
constexpr std::uint32_t kAwardNames = 0x004ad89cU;

bool countedType(int type) { return type == 0 || type == 6; }

} // namespace

int awardItemCount(const ExecutableGameData &exe, int type) {
    if (type < 0 || type > 7) return 0;
    return static_cast<int>(exe.u32At(kAwardCounts + 4U * type));
}

bool awardItemAvailable(const ExecutableGameData &exe, int type,
                        std::uint32_t value, int item) {
    if (type < 0 || type > 7) return false;
    const auto initial =
        static_cast<std::int32_t>(exe.u32At(kAwardInitial + 4U * type));
    if (countedType(type)) {
        return item < initial || item < static_cast<std::int32_t>(value);
    }
    // 0x0041b660: 1 << item (the shift count masked to 5 bits).
    const std::uint32_t bit = 1U << (static_cast<unsigned>(item) & 31U);
    return (static_cast<std::uint32_t>(initial) & bit) != 0U ||
           (value & bit) != 0U;
}

std::uint32_t awardItemGrant(const ExecutableGameData &exe, int type,
                             std::uint32_t value, int item) {
    if (item >= awardItemCount(exe, type)) return value;
    if (countedType(type)) {
        return static_cast<std::int32_t>(value) <= item
                   ? static_cast<std::uint32_t>(item + 1)
                   : value;
    }
    return value | (1U << (static_cast<unsigned>(item) & 31U));
}

int butterflyAwardOrder(const ExecutableGameData &exe, int item) {
    if (item < 0 || item >= 15) return -1;
    return static_cast<int>(exe.u32At(kButterflyOrder + 4U * item));
}

std::string awardItemNameKey(const ExecutableGameData &exe, int type,
                             int item) {
    if (type >= 0 && type <= 7 && item >= 0 &&
        item < awardItemCount(exe, type)) {
        if (type == kAwardButterflies) item = butterflyAwardOrder(exe, item);
        const std::uint32_t table = exe.u32At(kAwardNames + 4U * type);
        if (table != 0U && item >= 0) {
            return exe.stringAt(exe.u32At(table + 4U * item));
        }
    }
    return "awarddlg:NONE";
}

int buildacarLevel(const ExecutableGameData &exe, std::uint32_t placedParts) {
    auto remaining = static_cast<std::int32_t>(placedParts);
    for (int level = 0; level < 5; ++level) {
        remaining -= static_cast<std::int32_t>(exe.u32At(kStageSizes + 4U * level));
        if (remaining < 0) return level;
    }
    return 5;
}

} // namespace fgp
