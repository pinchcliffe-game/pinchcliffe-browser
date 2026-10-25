#include "assets/score_tables.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <stdexcept>

#include "assets/profile.h"
#include "generated/medal_tables.hpp"
#include "util/binary.h"

namespace fgp {
namespace {

constexpr std::uint32_t kScoreRecordType = 0x401U;
constexpr std::size_t kScoreRecordSize = 0x58U;
// 0x00411d40 is called with room for 0x40 profiles (0x0041ec06).
constexpr std::size_t kMaximumScoreProfiles = 64U;

ScoreRow trimmedRow(ScoreRow row) {
    if (row.player.size() > kScoreNameLength) {
        row.player.resize(kScoreNameLength);
    }
    return row;
}

// _stricmp in the C locale (0x0049a070): ASCII letters fold to lower case.
bool lessIgnoringAsciiCase(const std::string &left, const std::string &right) {
    const auto fold = [](unsigned char c) {
        return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
    };
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [&](char a, char b) {
            return fold(static_cast<unsigned char>(a)) <
                   fold(static_cast<unsigned char>(b));
        });
}

// 0x004115c0 -> 0x00411620: a stored record as a list, rows named `player`.
std::optional<ScoreList> readScoreList(const fs::path &profile,
                                       std::string_view scene,
                                       std::uint32_t key,
                                       const std::string &player) {
    const auto table = profileSceneScores(profile, scene, key);
    if (!table) {
        return std::nullopt;
    }
    ScoreList list;
    list.medalLevel = table->medalLevel;
    for (const auto &entry : table->entries) {
        list.rows.push_back(trimmedRow({player, entry.timestamp, entry.score}));
    }
    return list;
}

} // namespace

int compareScores(std::uint32_t a, std::uint32_t b, std::uint32_t flags) {
    if ((flags & 0xff00U) == kScoreLowFirst) {
        std::swap(a, b);  // 0x0041e94a: 0x0041e8c0 swaps the two arguments
    }
    // `sub eax, ecx` at 0x0041e95a: the wrapped 32-bit difference, signed.
    return static_cast<std::int32_t>(a - b);
}

int insertScore(ScoreList &list, const ScoreRow &row, std::uint32_t flags) {
    const ScoreRow inserted = trimmedRow(row);
    for (std::size_t index = 0; index < list.rows.size(); ++index) {
        if (compareScores(inserted.score, list.rows[index].score, flags) > 0) {
            // 0x0041ea1e: a full list drops its last row.
            if (list.rows.size() >= kScoreListCapacity) {
                list.rows.resize(kScoreListCapacity - 1U);
            }
            list.rows.insert(list.rows.begin() +
                                 static_cast<std::ptrdiff_t>(index),
                             inserted);
            return static_cast<int>(index);
        }
    }
    if (list.rows.size() >= kScoreListCapacity) {
        return -1;  // 0x0041e9fb
    }
    list.rows.push_back(inserted);
    return static_cast<int>(list.rows.size() - 1U);
}

int insertPlayerBest(ScoreList &list, const ScoreRow &row,
                     std::uint32_t flags) {
    const ScoreRow inserted = trimmedRow(row);
    const auto same = std::find_if(
        list.rows.begin(), list.rows.end(),
        [&](const ScoreRow &existing) {
            return existing.player == inserted.player;
        });
    if (same != list.rows.end()) {
        if (compareScores(inserted.score, same->score, flags) <= 0) {
            return -1;  // 0x0041ea87
        }
        list.rows.erase(same);  // 0x0041ea9c
    }
    return insertScore(list, inserted, flags);
}

std::uint32_t
medalLevelForScore(std::uint32_t score,
                   const std::array<std::uint32_t, 4> *thresholds,
                   std::uint32_t flags) {
    if (thresholds == nullptr) {
        return 0U;
    }
    // 0x0041eff4-0x0041f013: from platinum down, the first tier reached.
    for (int tier = 3; tier >= 0; --tier) {
        if (compareScores(score, (*thresholds)[static_cast<std::size_t>(tier)],
                          flags) >= 0) {
            return static_cast<std::uint32_t>(tier + 1);
        }
    }
    return 0U;
}

std::string formatScore(std::uint32_t value, std::uint32_t flags) {
    char text[32]{};
    const auto signedValue = static_cast<std::int32_t>(value);
    switch (flags & 0xffU) {
    case kScoreFormatInteger:
        std::snprintf(text, sizeof(text), "%d", signedValue);
        break;
    case kScoreFormatClock: {
        // 0x0041ecb0 with hours, minutes and seconds.
        const std::int32_t seconds = signedValue / 1000;
        const std::int32_t minutes = seconds / 60;
        std::snprintf(text, sizeof(text), "%02d:%02d:%02d", minutes / 60,
                      minutes % 60, seconds % 60);
        break;
    }
    case kScoreFormatStopwatch: {
        // 0x0041ecb0 without hours: minutes are not reduced.
        const std::int32_t seconds = signedValue / 1000;
        std::snprintf(text, sizeof(text), "%02d:%02d.%02d", seconds / 60,
                      seconds % 60, (signedValue % 1000) / 10);
        break;
    }
    default:
        break;  // 0x0041eda9: any other format leaves the buffer as it was
    }
    return text;
}

std::string formatScoreDate(std::uint32_t timestamp) {
    const std::time_t value = static_cast<std::time_t>(timestamp);
    const std::tm *local = std::localtime(&value);
    if (local == nullptr) {
        return {};
    }
    char date[16]{};
    std::snprintf(date, sizeof(date), "%02d.%02d.%02d", local->tm_mday,
                  local->tm_mon + 1, (local->tm_year + 1900) % 100);
    return date;
}

std::optional<SceneMedalTable> sceneMedalTable(std::string_view scene) {
    for (const auto &site : generated::kMedalTableSites) {
        if (site.scene == scene && site.tableAddress != 0U) {
            return SceneMedalTable{OriginalData::instance().read<std::array<std::uint32_t, 4>>(site.tableAddress), site.awardsCarParts};
        }
    }
    return std::nullopt;
}

std::string profileDisplayName(const fs::path &profile) {
    try {
        const auto bytes = readBinaryFile(profile);
        profileSceneEntries(bytes);
        // Section 0x100 (0x0c) holds the name from 0x18; 0x00410e10 copies
        // that section into the list record whose name 0x00411620 uses.
        std::string name;
        for (std::size_t offset = 0x18U;
             offset < 0x18U + kScoreNameLength && bytes[offset] != 0U;
             ++offset) {
            name.push_back(static_cast<char>(bytes[offset]));
        }
        if (!name.empty()) {
            return name;
        }
    } catch (const std::exception &) {
    }
    std::string stem = profile.stem().string();
    if (stem.size() > kScoreNameLength) {
        stem.resize(kScoreNameLength);
    }
    return stem;
}

ScoreLists loadScoreLists(const ScoreProfiles &profiles,
                          std::string_view scene, std::uint32_t key,
                          std::uint32_t flags) {
    ScoreLists lists;
    std::vector<std::string> names = profiles.names;
    std::stable_sort(names.begin(), names.end(), lessIgnoringAsciiCase);
    if (names.size() > kMaximumScoreProfiles) {
        names.resize(kMaximumScoreProfiles);
    }
    // 0x00411d40 -> 0x0041eaf0 / 0x0041eb60: each profile's rows in turn,
    // a profile's remaining rows skipped once one does not fit.
    for (const auto &name : names) {
        const fs::path path = profiles.directory / (name + ".pfl");
        std::optional<ScoreList> table;
        try {
            table = readScoreList(path, scene, key, profileDisplayName(path));
        } catch (const std::exception &) {
            continue;  // 0x004113a0 found no exact 0x58-byte record
        }
        if (!table) {
            continue;
        }
        for (const auto &row : table->rows) {
            if (insertScore(lists.global, row, flags) < 0) {
                break;
            }
        }
        for (const auto &row : table->rows) {
            if (insertPlayerBest(lists.players, row, flags) < 0) {
                break;
            }
        }
    }
    // 0x0041eac0: the active profile's own record.
    if (profiles.current) {
        const fs::path path =
            profiles.directory / (*profiles.current + ".pfl");
        try {
            if (auto own = readScoreList(path, scene, key,
                                         profileDisplayName(path))) {
                lists.profile = std::move(*own);
            }
        } catch (const std::exception &) {
        }
    }
    lists.global.medalLevel = 0U;
    lists.players.medalLevel = 0U;
    return lists;
}

std::array<std::uint8_t, 0x58> encodeScoreRecord(const ScoreList &list) {
    std::vector<std::uint8_t> payload(kScoreRecordSize, 0U);
    const std::size_t count = std::min(list.rows.size(), kScoreListCapacity);
    writeLe32(payload, 0U, list.medalLevel);
    writeLe32(payload, 4U, static_cast<std::uint32_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        writeLe32(payload, 8U + index * 8U, list.rows[index].timestamp);
        writeLe32(payload, 12U + index * 8U, list.rows[index].score);
    }
    std::array<std::uint8_t, 0x58> result{};
    std::copy(payload.begin(), payload.end(), result.begin());
    return result;
}

void writeProfileScoreList(const fs::path &profile, std::string_view scene,
                           std::uint32_t key, const ScoreList &list) {
    if (list.medalLevel > 4U) {
        throw std::runtime_error("Invalid FGP profile medal level");
    }
    const auto payload = encodeScoreRecord(list);
    setProfileSceneRecordBytes(profile, scene, kScoreRecordType, key, payload);
}

std::uint32_t lowestMedalLevel(const fs::path &profile,
                               std::span<const std::string> scoredScenes) {
    std::uint32_t lowest = 0x7fffffffU;
    for (const auto &scene : scoredScenes) {
        std::uint32_t level = 0U;  // a missing record reads as a zeroed list
        try {
            if (const auto table = profileSceneScores(profile, scene, 0U)) {
                level = table->medalLevel;
            }
        } catch (const std::exception &) {
        }
        lowest = std::min(lowest, level);  // `cmp esi, eax; jl` at 0x0040d9fc
    }
    return lowest;
}

ScoreSubmission submitScore(const ScoreProfiles &profiles,
                            std::string_view scene, std::uint32_t key,
                            std::uint32_t score,
                            const std::array<std::uint32_t, 4> *thresholds,
                            std::uint32_t flags, bool awardsCarParts,
                            std::uint32_t timestamp,
                            std::span<const std::string> scoredScenes) {
    ScoreSubmission result;
    const std::optional<fs::path> active =
        profiles.current
            ? std::optional<fs::path>(profiles.directory /
                                      (*profiles.current + ".pfl"))
            : std::nullopt;
    const std::uint32_t lowestBefore =
        active ? lowestMedalLevel(*active, scoredScenes) : 0x7fffffffU;

    // 0x0041e8e0: the active profile's name, time(NULL) and the score.
    result.row = trimmedRow(
        {active ? profileDisplayName(*active) : std::string{}, timestamp,
         score});
    result.lists = loadScoreLists(profiles, scene, key, flags);
    // 0x0041ef80: Profile and Global by 0x0041e9c0, Union by 0x0041ea50.
    if (active) {
        result.lists.profile.inserted =
            insertScore(result.lists.profile, result.row, flags);
    }
    result.lists.global.inserted =
        insertScore(result.lists.global, result.row, flags);
    result.lists.players.inserted =
        insertPlayerBest(result.lists.players, result.row, flags);

    // 0x0041eef8-0x0041ef34: grade, and store a better medal.
    result.previousMedal = result.lists.profile.medalLevel;
    const std::uint32_t earned = medalLevelForScore(score, thresholds, flags);
    result.medal = result.previousMedal;
    if (active && static_cast<std::int32_t>(result.previousMedal) <
                      static_cast<std::int32_t>(earned)) {
        result.medal = earned;
        result.medalCarParts = awardsCarParts ? earned - result.previousMedal
                                              : 0U;
        result.lists.profile.medalLevel = earned;
    }

    // 0x0041ef3a-0x0041ef52: the record is written only when the row
    // entered the active profile's list.
    if (active && result.lists.profile.inserted >= 0) {
        writeProfileScoreList(*active, scene, key, result.lists.profile);
        result.written = true;
    }

    // 0x0041ef55-0x0041ef6c: 0x0040d9b0 again, after the write.
    if (active && !scoredScenes.empty()) {
        const std::uint32_t lowestAfter =
            lowestMedalLevel(*active, scoredScenes);
        const auto gained = static_cast<std::int32_t>(lowestAfter - lowestBefore);
        if (gained > 0) {
            result.bonusCarParts = static_cast<std::uint32_t>(gained);
        }
    }
    return result;
}

} // namespace fgp
