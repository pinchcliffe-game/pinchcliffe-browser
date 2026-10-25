#pragma once

// Reader/writer for the original .pfl player profiles. Writes are atomic
// (temporary file + rename) and never delete existing profiles.

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

constexpr std::size_t kProfileFixedSize = 0x0b84;
constexpr std::size_t kProfileDirectoryOffset = 0x84;
constexpr std::size_t kProfileDirectoryEntries = 64;
constexpr std::size_t kProfileDirectoryEntrySize = 44;

struct ProfileSceneEntry {
    std::size_t slotOffset{};
    std::string name;
    std::uint32_t flags{};
    std::uint32_t blockOffset{};
    std::uint32_t blockSize{};
};

enum class ProfileRecordUpdate { Replace, BitwiseOr };

// Exact 20-byte type-0x402/key-0 payload used by the original Build-a-Car
// scene (fcn.00423350). The collected count is refreshed from the profile's
// award section on entry; the remaining fields preserve workshop progress.
struct BuildacarProfileState {
    std::uint32_t collectedParts{};
    std::uint32_t placedParts{};
    std::uint32_t currentStage{};
    std::uint32_t completedStages{};
    std::uint32_t stageMask{};
};

struct ProfileBirthday {
    int zeroBasedMonth{};
    int day{};
};

struct ProfileScoreEntry {
    std::uint32_t timestamp{};
    std::uint32_t score{};
};

struct ProfileScoreTable {
    std::uint32_t medalLevel{};
    std::vector<ProfileScoreEntry> entries;
};

// The original profile header stores the selected birthday at 0x34/0x38.
// Returns nullopt for a missing file and rejects malformed profile containers.
std::optional<ProfileBirthday> profileBirthday(const fs::path &path);

std::vector<ProfileSceneEntry>
profileSceneEntries(std::span<const std::uint8_t> bytes);

std::optional<std::uint32_t>
profileSceneRecordU32(const fs::path &path,
                      std::string_view sceneName,
                      std::uint32_t recordType,
                      std::uint32_t key,
                      bool requireExactPayload = true);

// Read an arbitrary scene-record payload. When expectedSize is supplied, a
// record with any other payload size is treated as absent, matching the
// original exact-length read contract used by Game Trivia's rank array.
std::optional<std::vector<std::uint8_t>>
profileSceneRecordBytes(const fs::path &path,
                        std::string_view sceneName,
                        std::uint32_t recordType,
                        std::uint32_t key,
                        std::optional<std::size_t> expectedSize = std::nullopt);

std::uint32_t upsertProfileSceneRecordU32(std::vector<std::uint8_t> &bytes,
                                          std::string_view sceneName,
                                          std::uint32_t recordType,
                                          std::uint32_t key,
                                          std::uint32_t requestedValue,
                                          ProfileRecordUpdate update);

void writeProfileBytesAtomically(const fs::path &path,
                                 std::span<const std::uint8_t> bytes);

std::uint32_t awardCarPart(const fs::path &path, std::string_view sceneName);
// Medal awards advance the shared sequential car-part counter but, unlike
// hidden scene collectibles, do not add a type-0x402 marker to the minigame.
std::uint32_t awardMedalCarPart(const fs::path &path);
// The same for `count` parts (0x0041bb20's fourth argument: a medal award
// grants one per level gained). Returns the first part granted, or 0 when
// all 62 are already owned. UNCERTAIN: past 62 the original falls back to
// award category 6 (0x0041b9d0), which is not ported.
std::uint32_t awardMedalCarParts(const fs::path &path, std::uint32_t count);
void awardAnaglyphScene(const fs::path &path, std::uint32_t awardIndex);

std::uint32_t incrementProfileSceneRecordU32(const fs::path &path,
                                             std::string_view sceneName,
                                             std::uint32_t recordType,
                                             std::uint32_t key);

// Record type 0x400/key 0 is the engine-owned scene visit record. Its
// payload is two u32 values: the visit count and the Unix timestamp of the
// most recent entry. Returns the new visit count.
std::uint32_t recordProfileSceneVisit(const fs::path &path,
                                      std::string_view sceneName,
                                      std::uint32_t unixTimestamp);

void setProfileSceneRecordU32(const fs::path &path,
                              std::string_view sceneName,
                              std::uint32_t recordType,
                              std::uint32_t key,
                              std::uint32_t value);

// Replace or create an arbitrary scene-record payload, adjusting the owning
// scene block and every following block offset when its size changes.
void setProfileSceneRecordBytes(const fs::path &path,
                                std::string_view sceneName,
                                std::uint32_t recordType,
                                std::uint32_t key,
                                std::span<const std::uint8_t> payload);

// Type 0x401/key 0 is the original per-scene score table. Its exact 0x58-byte
// payload is medal-level/count followed by ten fixed timestamp/score pairs.
std::optional<ProfileScoreTable>
profileSceneScores(const fs::path &path, std::string_view sceneName);
// The same table under another key: the Scoreboard reads Mountain's two
// tracks as keys 0 and 1 (subgame.ini version9/version10, 0x00416960).
std::optional<ProfileScoreTable>
profileSceneScores(const fs::path &path, std::string_view sceneName,
                   std::uint32_t key);

// The sequential car-part counter: the first of the eight u32 values in the
// award section 0x301 (profile offset 0x58), capped at the 62 parts.
std::uint32_t profileCarPartCount(const fs::path &path);

// The eight u32 award values of section 0x301 (profile offset 0x58), as
// 0x00411b70 reads them and 0x00411c40 writes them back. Value 0 is the
// car-part counter; the others are bit sets (0x0041b6e0).
std::array<std::uint32_t, 8> profileAwards(const fs::path &path);
void setProfileAwards(const fs::path &path,
                      const std::array<std::uint32_t, 8> &awards);

void insertProfileScore(ProfileScoreTable &table,
                        std::uint32_t timestamp,
                        std::uint32_t score);

// `key` is the score wrapper's list key (0x0041f8c0's second argument):
// 0 for every game except Mountain, which passes its track index.
void recordProfileSceneScore(const fs::path &path,
                             std::string_view sceneName,
                             std::uint32_t timestamp,
                             std::uint32_t score,
                             std::uint32_t medalLevel,
                             std::uint32_t key = 0U);

std::optional<BuildacarProfileState>
profileBuildacarState(const fs::path &path);

void setProfileBuildacarState(const fs::path &path,
                              const BuildacarProfileState &state);

// The Settings dialog's choices, kept in the profile's `options` scene block
// (name pointer 0x004a68b8 -> "options" 0x004a68e0) as two type-0x402
// records. 0x0040b490 reads them when a profile is selected and 0x0040b5a0
// writes them when Settings is closed with OK.
//
// Key 0, exactly 12 bytes: the effect, speech and music sliders as IEEE
// floats (default 0.75 each).
// Key 1, exactly 20 bytes: u32 subtitles on (0x0053dac4), char[8] subtitle
// language (0x0053a1b8), char[8] a second language code (0x006ea158) that
// the dialog carries through unchanged; both default to "no".
//
// The strings hold what the original keeps in memory, so they round-trip
// exactly: at most seven characters, and a second code equal to "no" in any
// case is held (and saved) as "" (0x00484580).
struct ProfileOptions {
    std::array<float, 3> volumes{0.75F, 0.75F, 0.75F};
    std::uint32_t subtitlesEnabled{};
    std::string subtitleLanguage{"no"};
    std::string secondLanguage{};
};

inline constexpr std::string_view kProfileOptionsScene = "options";
inline constexpr std::uint32_t kProfileOptionsRecordType = 0x402U;

// A record that is missing or has another size leaves that record's defaults
// in place (0x004113f1 rejects a size mismatch), as does a missing profile.
ProfileOptions profileOptions(const std::optional<fs::path> &path);
// Writes key 0, then key 1 (0x0040b5f2, 0x0040b623).
void setProfileOptions(const fs::path &path, const ProfileOptions &options);
// 0x00484580: the second code as the original holds it.
std::string normalizedSecondLanguage(std::string_view code);

void writeFreshProfile(const fs::path &profileDirectory,
                       const std::string &utf8Name,
                       std::string_view cp1252Name,
                       int zeroBasedMonth,
                       int day);

} // namespace fgp
