#pragma once

// The original's shared score tables: the per-profile type-0x401 records, the
// three lists the score dialog and the Scoreboard show, medal grading and the
// score formats. One implementation for every caller, as in FGP.exe, where the
// scored minigames (0x0041f9a0, 0x0041f9d0), Mountain (0x0041f8c0), the puzzle
// activities and the race (0x0041ee50) and the viewers (0x0041f030, 0x0041f640)
// all reach the same routines at 0x0041e8e0-0x0041f030.
//
// No SDL here, so the race (issue 253) and tests can use it directly.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgp {
namespace fs = std::filesystem;

// The flags argument of 0x0041ee50 (its fourth) and 0x0041f030 (its sixth).
// 0x0041e930 reverses the comparison when (flags & 0xff00) == 0x100, and
// 0x0041ed90 picks the format from flags & 0xff.
constexpr std::uint32_t kScoreLowFirst = 0x100U;
constexpr std::uint32_t kScoreFormatInteger = 0U;    // "%d"
constexpr std::uint32_t kScoreFormatClock = 1U;      // "%02d:%02d:%02d", h:m:s
constexpr std::uint32_t kScoreFormatStopwatch = 2U;  // "%02d:%02d.%02d", m:s.cs
// Times in milliseconds, lowest first, shown as minutes, seconds and
// hundredths: the race (0x00477580, 0x004775b5), Jigsaw, Sliding and Mahjong.
constexpr std::uint32_t kScoreTimeFlags = kScoreLowFirst | kScoreFormatStopwatch;

// A list holds ten rows (0x0041e9c0 caps it); a row is 0x24 bytes, its name
// 24 characters (strncpy 0x18 at 0x00411620).
constexpr std::size_t kScoreListCapacity = 10U;
constexpr std::size_t kScoreNameLength = 24U;

struct ScoreRow {
    std::string player;  // CP1252, as the profile stores it
    std::uint32_t timestamp{};
    std::uint32_t score{};
};

// The 0x174-byte list object: the medal level (the record's first dword),
// the rows, and at +0x170 where the last insertion landed (-1 for none).
struct ScoreList {
    std::uint32_t medalLevel{};
    std::vector<ScoreRow> rows;
    int inserted{-1};
};

// The three lists of 0x0041ebd0, in the dialog's tab order (0x0041f760:
// controls 0xc9-0xcb show 0x005a4718, 0x005a5c18 and 0x005a5d90).
struct ScoreLists {
    ScoreList profile;  // the active profile's own record
    ScoreList global;   // Poengtopp: every profile's rows
    ScoreList players;  // Spillertopp: every profile's best row
};

// 0x0041e930: a - b as a signed 32-bit difference, or b - a for low-first.
int compareScores(std::uint32_t a, std::uint32_t b, std::uint32_t flags);

// 0x0041e9c0: insert before the first row the new one beats; equal rows keep
// their order. Returns the index, or -1 when a full list keeps it out.
int insertScore(ScoreList &list, const ScoreRow &row, std::uint32_t flags);

// 0x0041ea50: one row per player (0x0041e960 compares names exactly); a
// player already listed is replaced only by a better row.
int insertPlayerBest(ScoreList &list, const ScoreRow &row, std::uint32_t flags);

// 0x0041efe0: 0 to 4, the highest threshold the score reaches; 0 without a
// table.
std::uint32_t
medalLevelForScore(std::uint32_t score,
                   const std::array<std::uint32_t, 4> *thresholds,
                   std::uint32_t flags);

// 0x0041ed90 and 0x0041ed50 (the date, "%02d.%02d.%02d" in local time).
std::string formatScore(std::uint32_t value, std::uint32_t flags);
std::string formatScoreDate(std::uint32_t timestamp);

// The bronze-to-platinum thresholds a scene pushes to the score wrapper, from
// src/generated/medal_tables.hpp (the first of its sites that pushes a table).
struct SceneMedalTable {
    std::array<std::uint32_t, 4> thresholds{};
    // False for the scenes that score through 0x0041f9d0, which sets
    // 0x005a6fe0 so that 0x0041ef22 awards the medal with no car part.
    bool awardsCarParts{true};
};
std::optional<SceneMedalTable> sceneMedalTable(std::string_view scene);

// The profile list 0x00411d40 reads: up to 64 profiles from the directory,
// sorted with _stricmp (0x00410d20 -> qsort with 0x0049a070).
struct ScoreProfiles {
    fs::path directory;
    std::vector<std::string> names;      // .pfl stems
    std::optional<std::string> current;  // the active profile's stem
};

// The name a profile's rows carry: the header name (section 0x100 at 0x18),
// not the file name, cut to 24 characters. Falls back to the stem.
std::string profileDisplayName(const fs::path &profile);

// 0x0041ebd0: the active profile's record and the merged Global and Union
// lists for one scene record key.
ScoreLists loadScoreLists(const ScoreProfiles &profiles,
                          std::string_view scene, std::uint32_t key,
                          std::uint32_t flags);

// 0x00411cc0 -> 0x00411c70: the exact 0x58-byte type-0x401 payload.
std::array<std::uint8_t, 0x58> encodeScoreRecord(const ScoreList &list);
void writeProfileScoreList(const fs::path &profile, std::string_view scene,
                           std::uint32_t key, const ScoreList &list);

// 0x0040d9b0: the lowest key-0 medal level over the scored (type-3) scenes,
// in the scene registry's order; 0x7fffffff with none.
std::uint32_t lowestMedalLevel(const fs::path &profile,
                               std::span<const std::string> scoredScenes);

struct ScoreSubmission {
    ScoreRow row;  // 0x0041e8e0: the submitted row
    ScoreLists lists;
    std::uint32_t previousMedal{};
    std::uint32_t medal{};
    // Car parts the medal award grants: the levels gained, or 0 through
    // 0x0041f9d0 (0x0041ef18-0x0041ef22).
    std::uint32_t medalCarParts{};
    // Car parts for raising the lowest medal over every scored scene
    // (0x0041ef55-0x0041ef6c), a second award with no medal.
    std::uint32_t bonusCarParts{};
    // Whether the record was written: only when the row entered the active
    // profile's list (0x0041ef3a-0x0041ef52).
    bool written{};
};

// 0x0041ee50 -> 0x0041eed0: build the lists, insert the row, grade it and
// write the active profile's record. The caller shows the awards (0x0041bb20)
// and grants their car parts; with no active profile nothing is written.
ScoreSubmission submitScore(const ScoreProfiles &profiles,
                            std::string_view scene, std::uint32_t key,
                            std::uint32_t score,
                            const std::array<std::uint32_t, 4> *thresholds,
                            std::uint32_t flags, bool awardsCarParts,
                            std::uint32_t timestamp,
                            std::span<const std::string> scoredScenes = {});

} // namespace fgp
