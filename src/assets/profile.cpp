#include "assets/profile.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <tuple>

#include "util/binary.h"

namespace fgp {

std::optional<ProfileBirthday> profileBirthday(const fs::path &path) {
    if (!fs::exists(path)) {
        return std::nullopt;
    }
    const auto bytes = readBinaryFile(path);
    // Validate the whole fixed profile structure before trusting metadata.
    profileSceneEntries(bytes);
    const auto month = readLe32(bytes, 0x34);
    const auto day = readLe32(bytes, 0x38);
    if (month >= 12U || day == 0U || day > 31U) {
        throw std::runtime_error("FGP profile has an invalid birthday");
    }
    return ProfileBirthday{static_cast<int>(month), static_cast<int>(day)};
}

std::vector<ProfileSceneEntry>
profileSceneEntries(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kProfileFixedSize ||
        readLe32(bytes, 0x00) != 0x00face01U ||
        readLe32(bytes, 0x04) != 0x00000102U ||
        readLe32(bytes, 0x08) != 0x00000b78U) {
        throw std::runtime_error("Invalid or truncated FGP profile");
    }
    if (readLe32(bytes, 0x78) != 0x00000200U ||
        readLe32(bytes, 0x80) != 0x00000b00U) {
        throw std::runtime_error("FGP profile has no valid scene directory");
    }

    std::vector<ProfileSceneEntry> result;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string>> ranges;
    for (std::size_t index = 0; index < kProfileDirectoryEntries; ++index) {
        const std::size_t slot =
            kProfileDirectoryOffset + index * kProfileDirectoryEntrySize;
        std::size_t nameLength = 0;
        while (nameLength < 32 && bytes[slot + nameLength] != 0) {
            ++nameLength;
        }
        const bool emptyName = nameLength == 0;
        const auto flags = readLe32(bytes, slot + 32);
        const auto blockOffset = readLe32(bytes, slot + 36);
        const auto blockSize = readLe32(bytes, slot + 40);
        if (emptyName) {
            const bool cleanName = std::all_of(
                bytes.begin() + static_cast<std::ptrdiff_t>(slot),
                bytes.begin() + static_cast<std::ptrdiff_t>(slot + 32),
                [](std::uint8_t byte) { return byte == 0; });
            if (!cleanName || flags != 0 || blockOffset != 0 || blockSize != 0) {
                throw std::runtime_error("Malformed empty FGP profile scene slot");
            }
            continue;
        }
        if (nameLength == 32) {
            throw std::runtime_error("Unterminated FGP profile scene name");
        }
        if (blockOffset < kProfileFixedSize ||
            static_cast<std::uint64_t>(blockOffset) + blockSize > bytes.size()) {
            throw std::runtime_error("FGP profile scene block is out of range");
        }
        std::string name(
            reinterpret_cast<const char *>(bytes.data() + slot), nameLength);
        result.push_back({slot, name, flags, blockOffset, blockSize});
        ranges.emplace_back(blockOffset, blockOffset + blockSize, name);
    }

    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (std::get<0>(ranges[index]) < std::get<1>(ranges[index - 1])) {
            throw std::runtime_error("Overlapping FGP profile scene blocks");
        }
    }
    return result;
}

std::optional<std::uint32_t>
profileSceneRecordU32(const fs::path &path,
                      std::string_view sceneName,
                      std::uint32_t recordType,
                      std::uint32_t key,
                      bool requireExactPayload) {
    if (!fs::exists(path)) {
        return std::nullopt;
    }
    const auto bytes = readBinaryFile(path);
    const auto scenes = profileSceneEntries(bytes);
    const auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [&](const ProfileSceneEntry &entry) { return entry.name == sceneName; });
    if (scene == scenes.end()) {
        return std::nullopt;
    }
    std::size_t position = scene->blockOffset;
    const std::size_t end = position + scene->blockSize;
    while (position < end) {
        if (position + 12 > end) {
            throw std::runtime_error("Truncated FGP profile record header");
        }
        const auto type = readLe32(bytes, position);
        const auto recordKey = readLe32(bytes, position + 4);
        const auto payloadSize = readLe32(bytes, position + 8);
        const std::size_t payload = position + 12;
        if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
            throw std::runtime_error("Truncated FGP profile record payload");
        }
        if (type == recordType && recordKey == key) {
            if (payloadSize < 4 ||
                (requireExactPayload && payloadSize != 4)) {
                throw std::runtime_error("FGP profile record is not a u32");
            }
            return readLe32(bytes, payload);
        }
        position = payload + payloadSize;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>>
profileSceneRecordBytes(const fs::path &path,
                        std::string_view sceneName,
                        std::uint32_t recordType,
                        std::uint32_t key,
                        std::optional<std::size_t> expectedSize) {
    if (!fs::exists(path)) {
        return std::nullopt;
    }
    const auto bytes = readBinaryFile(path);
    const auto scenes = profileSceneEntries(bytes);
    const auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [&](const ProfileSceneEntry &entry) { return entry.name == sceneName; });
    if (scene == scenes.end()) {
        return std::nullopt;
    }
    std::size_t position = scene->blockOffset;
    const std::size_t end = position + scene->blockSize;
    while (position < end) {
        if (position + 12 > end) {
            throw std::runtime_error("Truncated FGP profile record header");
        }
        const auto type = readLe32(bytes, position);
        const auto recordKey = readLe32(bytes, position + 4);
        const auto payloadSize = readLe32(bytes, position + 8);
        const std::size_t payload = position + 12;
        if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
            throw std::runtime_error("Truncated FGP profile record payload");
        }
        if (type == recordType && recordKey == key) {
            if (expectedSize && payloadSize != *expectedSize) {
                return std::nullopt;
            }
            return std::vector<std::uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(payload),
                bytes.begin() + static_cast<std::ptrdiff_t>(payload + payloadSize));
        }
        position = payload + payloadSize;
    }
    return std::nullopt;
}

std::uint32_t upsertProfileSceneRecordU32(
    std::vector<std::uint8_t> &bytes,
    std::string_view sceneName,
    std::uint32_t recordType,
    std::uint32_t key,
    std::uint32_t requestedValue,
    ProfileRecordUpdate update) {
    if (sceneName.empty() || sceneName.size() >= 32) {
        throw std::runtime_error("Invalid FGP profile scene name");
    }
    auto scenes = profileSceneEntries(bytes);
    auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [&](const ProfileSceneEntry &entry) { return entry.name == sceneName; });

    std::uint32_t nextValue = requestedValue;
    bool foundRecord = false;
    if (scene != scenes.end()) {
        std::size_t position = scene->blockOffset;
        const std::size_t end = position + scene->blockSize;
        while (position < end) {
            if (position + 12 > end) {
                throw std::runtime_error("Truncated FGP profile record header");
            }
            const auto type = readLe32(bytes, position);
            const auto recordKey = readLe32(bytes, position + 4);
            const auto payloadSize = readLe32(bytes, position + 8);
            const std::size_t payload = position + 12;
            if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
                throw std::runtime_error("Truncated FGP profile record payload");
            }
            if (type == recordType && recordKey == key) {
                if (payloadSize != 4) {
                    throw std::runtime_error("FGP profile record is not a u32");
                }
                if (update == ProfileRecordUpdate::BitwiseOr) {
                    nextValue |= readLe32(bytes, payload);
                }
                writeLe32(bytes, payload, nextValue);
                foundRecord = true;
                break;
            }
            position = payload + payloadSize;
        }

        if (!foundRecord) {
            std::vector<std::uint8_t> record(16, 0);
            writeLe32(record, 0, recordType);
            writeLe32(record, 4, key);
            writeLe32(record, 8, 4);
            writeLe32(record, 12, nextValue);
            const std::size_t insertion =
                static_cast<std::size_t>(scene->blockOffset) + scene->blockSize;
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion),
                         record.begin(), record.end());
            writeLe32(bytes, scene->slotOffset + 40, scene->blockSize + 16U);
            for (const auto &other : scenes) {
                if (other.slotOffset != scene->slotOffset &&
                    other.blockOffset >= insertion) {
                    writeLe32(bytes, other.slotOffset + 36,
                              other.blockOffset + 16U);
                }
            }
        }
    } else {
        std::optional<std::size_t> emptySlot;
        for (std::size_t index = 0; index < kProfileDirectoryEntries; ++index) {
            const std::size_t slot =
                kProfileDirectoryOffset + index * kProfileDirectoryEntrySize;
            if (bytes[slot] == 0) {
                emptySlot = slot;
                break;
            }
        }
        if (!emptySlot) {
            throw std::runtime_error("FGP profile scene directory is full");
        }
        const std::uint32_t blockOffset =
            static_cast<std::uint32_t>(bytes.size());
        std::copy(sceneName.begin(), sceneName.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(*emptySlot));
        writeLe32(bytes, *emptySlot + 32, 0);
        writeLe32(bytes, *emptySlot + 36, blockOffset);
        writeLe32(bytes, *emptySlot + 40, 16);
        const std::size_t oldSize = bytes.size();
        bytes.resize(oldSize + 16, 0);
        writeLe32(bytes, oldSize, recordType);
        writeLe32(bytes, oldSize + 4, key);
        writeLe32(bytes, oldSize + 8, 4);
        writeLe32(bytes, oldSize + 12, nextValue);
    }
    return nextValue;
}

void writeProfileBytesAtomically(const fs::path &path,
                                 std::span<const std::uint8_t> bytes) {
    fs::path temporary = path;
    temporary += ".native-update";
    // Only this function writes the temporary file, and it removes it on
    // every error it sees, so one that exists was left by a process that
    // ended mid-save. Refusing to save over it made the profile unsaveable
    // for good, so it is replaced.
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output.write(reinterpret_cast<const char *>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()))) {
                throw std::runtime_error("Could not write updated FGP profile");
            }
            output.flush();
            if (!output) {
                throw std::runtime_error("Could not finish updated FGP profile");
            }
        }
        std::error_code permissionError;
        fs::permissions(temporary, fs::status(path).permissions(), permissionError);
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::optional<BuildacarProfileState>
profileBuildacarState(const fs::path &path) {
    if (!fs::exists(path)) {
        return std::nullopt;
    }
    const auto bytes = readBinaryFile(path);
    const auto scenes = profileSceneEntries(bytes);
    const auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [](const ProfileSceneEntry &entry) { return entry.name == "buildacar"; });
    if (scene == scenes.end()) {
        return std::nullopt;
    }
    std::size_t position = scene->blockOffset;
    const std::size_t end = position + scene->blockSize;
    while (position < end) {
        if (position + 12U > end) {
            throw std::runtime_error("Truncated FGP profile record header");
        }
        const auto type = readLe32(bytes, position);
        const auto key = readLe32(bytes, position + 4U);
        const auto payloadSize = readLe32(bytes, position + 8U);
        const std::size_t payload = position + 12U;
        if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
            throw std::runtime_error("Truncated FGP profile record payload");
        }
        if (type == 0x402U && key == 0U) {
            if (payloadSize != 20U) {
                throw std::runtime_error(
                    "FGP Build-a-Car profile record is not 20 bytes");
            }
            return BuildacarProfileState{
                readLe32(bytes, payload),
                readLe32(bytes, payload + 4U),
                readLe32(bytes, payload + 8U),
                readLe32(bytes, payload + 12U),
                readLe32(bytes, payload + 16U),
            };
        }
        position = payload + payloadSize;
    }
    return std::nullopt;
}

void setProfileBuildacarState(const fs::path &path,
                              const BuildacarProfileState &state) {
    auto bytes = readBinaryFile(path);
    auto scenes = profileSceneEntries(bytes);
    auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [](const ProfileSceneEntry &entry) { return entry.name == "buildacar"; });

    const auto writePayload = [&](std::size_t payload) {
        writeLe32(bytes, payload, state.collectedParts);
        writeLe32(bytes, payload + 4U, state.placedParts);
        writeLe32(bytes, payload + 8U, state.currentStage);
        writeLe32(bytes, payload + 12U, state.completedStages);
        writeLe32(bytes, payload + 16U, state.stageMask);
    };

    if (scene != scenes.end()) {
        std::size_t position = scene->blockOffset;
        const std::size_t end = position + scene->blockSize;
        while (position < end) {
            if (position + 12U > end) {
                throw std::runtime_error("Truncated FGP profile record header");
            }
            const auto type = readLe32(bytes, position);
            const auto key = readLe32(bytes, position + 4U);
            const auto payloadSize = readLe32(bytes, position + 8U);
            const std::size_t payload = position + 12U;
            if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
                throw std::runtime_error("Truncated FGP profile record payload");
            }
            if (type == 0x402U && key == 0U) {
                if (payloadSize != 20U) {
                    throw std::runtime_error(
                        "FGP Build-a-Car profile record is not 20 bytes");
                }
                writePayload(payload);
                writeProfileBytesAtomically(path, bytes);
                return;
            }
            position = payload + payloadSize;
        }

        std::vector<std::uint8_t> record(32U, 0U);
        writeLe32(record, 0U, 0x402U);
        writeLe32(record, 4U, 0U);
        writeLe32(record, 8U, 20U);
        writeLe32(record, 12U, state.collectedParts);
        writeLe32(record, 16U, state.placedParts);
        writeLe32(record, 20U, state.currentStage);
        writeLe32(record, 24U, state.completedStages);
        writeLe32(record, 28U, state.stageMask);
        const std::size_t insertion =
            static_cast<std::size_t>(scene->blockOffset) + scene->blockSize;
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion),
                     record.begin(), record.end());
        writeLe32(bytes, scene->slotOffset + 40U, scene->blockSize + 32U);
        for (const auto &other : scenes) {
            if (other.slotOffset != scene->slotOffset &&
                other.blockOffset >= insertion) {
                writeLe32(bytes, other.slotOffset + 36U,
                          other.blockOffset + 32U);
            }
        }
    } else {
        std::optional<std::size_t> emptySlot;
        for (std::size_t index = 0; index < kProfileDirectoryEntries; ++index) {
            const std::size_t slot =
                kProfileDirectoryOffset + index * kProfileDirectoryEntrySize;
            if (bytes[slot] == 0U) {
                emptySlot = slot;
                break;
            }
        }
        if (!emptySlot) {
            throw std::runtime_error("FGP profile scene directory is full");
        }
        constexpr std::string_view sceneName = "buildacar";
        const std::uint32_t blockOffset =
            static_cast<std::uint32_t>(bytes.size());
        std::copy(sceneName.begin(), sceneName.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(*emptySlot));
        writeLe32(bytes, *emptySlot + 32U, 0U);
        writeLe32(bytes, *emptySlot + 36U, blockOffset);
        writeLe32(bytes, *emptySlot + 40U, 32U);
        const std::size_t oldSize = bytes.size();
        bytes.resize(oldSize + 32U, 0U);
        writeLe32(bytes, oldSize, 0x402U);
        writeLe32(bytes, oldSize + 4U, 0U);
        writeLe32(bytes, oldSize + 8U, 20U);
        writePayload(oldSize + 12U);
    }
    writeProfileBytesAtomically(path, bytes);
}

std::uint32_t awardCarPart(const fs::path &path, std::string_view sceneName) {
    auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }

    // Award type 0 is the sequential car-part collection. fcn.0041b740
    // reads the eight u32 values in section 0x301; case 0 of 0x0041b6e0
    // advances the first value and returns the previous zero-based part
    // index for bitmap/award/part_%02d.bmp.
    const std::uint32_t current = readLe32(bytes, 0x58);
    const std::uint32_t next = std::min<std::uint32_t>(current + 1U, 62U);
    writeLe32(bytes, 0x58, next);
    upsertProfileSceneRecordU32(bytes, sceneName, 0x402U, 0U, 1U,
                                ProfileRecordUpdate::Replace);
    writeProfileBytesAtomically(path, bytes);
    return next;
}

std::uint32_t awardMedalCarPart(const fs::path &path) {
    auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }

    // The original bronze capture advances the shared award-section counter
    // at profile offset 0x58 from 0 to 1, but leaves Whack-a-Mole without the
    // type-0x402 marker used by a hidden scene collectible.
    const std::uint32_t current = readLe32(bytes, 0x58);
    const std::uint32_t next = std::min<std::uint32_t>(current + 1U, 62U);
    writeLe32(bytes, 0x58, next);
    writeProfileBytesAtomically(path, bytes);
    return next;
}

std::uint32_t awardMedalCarParts(const fs::path &path, std::uint32_t count) {
    auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }
    const std::uint32_t current = readLe32(bytes, 0x58);
    if (count == 0U || current >= 62U) {
        return 0U;
    }
    const std::uint32_t next =
        std::min<std::uint32_t>(current + std::min(count, 62U), 62U);
    writeLe32(bytes, 0x58, next);
    writeProfileBytesAtomically(path, bytes);
    return current + 1U;
}

void awardAnaglyphScene(const fs::path &path, std::uint32_t awardIndex) {
    if (awardIndex >= 32U) {
        throw std::runtime_error("Invalid FGP anaglyph award index");
    }
    auto bytes = readBinaryFile(path);
    upsertProfileSceneRecordU32(bytes, "anaglyph", 0x402U, 100U,
                                1U << awardIndex,
                                ProfileRecordUpdate::BitwiseOr);
    writeProfileBytesAtomically(path, bytes);
}

std::uint32_t incrementProfileSceneRecordU32(const fs::path &path,
                                             std::string_view sceneName,
                                             std::uint32_t recordType,
                                             std::uint32_t key) {
    const std::uint32_t current =
        profileSceneRecordU32(path, sceneName, recordType, key).value_or(0U);
    if (current == std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("FGP profile scene counter overflow");
    }
    auto bytes = readBinaryFile(path);
    const std::uint32_t next = current + 1U;
    upsertProfileSceneRecordU32(bytes, sceneName, recordType, key, next,
                                ProfileRecordUpdate::Replace);
    writeProfileBytesAtomically(path, bytes);
    return next;
}

std::uint32_t recordProfileSceneVisit(const fs::path &path,
                                      std::string_view sceneName,
                                      std::uint32_t unixTimestamp) {
    if (sceneName.empty() || sceneName.size() >= 32) {
        throw std::runtime_error("Invalid FGP profile scene name");
    }

    auto bytes = readBinaryFile(path);
    auto scenes = profileSceneEntries(bytes);
    auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [&](const ProfileSceneEntry &entry) { return entry.name == sceneName; });

    std::uint32_t nextCount = 1U;
    if (scene != scenes.end()) {
        std::size_t position = scene->blockOffset;
        const std::size_t end = position + scene->blockSize;
        while (position < end) {
            if (position + 12 > end) {
                throw std::runtime_error("Truncated FGP profile record header");
            }
            const auto type = readLe32(bytes, position);
            const auto key = readLe32(bytes, position + 4);
            const auto payloadSize = readLe32(bytes, position + 8);
            const std::size_t payload = position + 12;
            if (static_cast<std::uint64_t>(payload) + payloadSize > end) {
                throw std::runtime_error("Truncated FGP profile record payload");
            }
            if (type == 0x400U && key == 0U) {
                if (payloadSize != 8U) {
                    throw std::runtime_error(
                        "FGP profile scene visit record is not 8 bytes");
                }
                const std::uint32_t current = readLe32(bytes, payload);
                if (current == std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("FGP profile scene counter overflow");
                }
                nextCount = current + 1U;
                writeLe32(bytes, payload, nextCount);
                writeLe32(bytes, payload + 4, unixTimestamp);
                writeProfileBytesAtomically(path, bytes);
                return nextCount;
            }
            position = payload + payloadSize;
        }

        std::vector<std::uint8_t> record(20, 0);
        writeLe32(record, 0, 0x400U);
        writeLe32(record, 4, 0U);
        writeLe32(record, 8, 8U);
        writeLe32(record, 12, nextCount);
        writeLe32(record, 16, unixTimestamp);
        const std::size_t insertion =
            static_cast<std::size_t>(scene->blockOffset) + scene->blockSize;
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion),
                     record.begin(), record.end());
        writeLe32(bytes, scene->slotOffset + 40, scene->blockSize + 20U);
        for (const auto &other : scenes) {
            if (other.slotOffset != scene->slotOffset &&
                other.blockOffset >= insertion) {
                writeLe32(bytes, other.slotOffset + 36,
                          other.blockOffset + 20U);
            }
        }
    } else {
        std::optional<std::size_t> emptySlot;
        for (std::size_t index = 0; index < kProfileDirectoryEntries; ++index) {
            const std::size_t slot =
                kProfileDirectoryOffset + index * kProfileDirectoryEntrySize;
            if (bytes[slot] == 0) {
                emptySlot = slot;
                break;
            }
        }
        if (!emptySlot) {
            throw std::runtime_error("FGP profile scene directory is full");
        }
        const std::uint32_t blockOffset =
            static_cast<std::uint32_t>(bytes.size());
        std::copy(sceneName.begin(), sceneName.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(*emptySlot));
        writeLe32(bytes, *emptySlot + 32, 0U);
        writeLe32(bytes, *emptySlot + 36, blockOffset);
        writeLe32(bytes, *emptySlot + 40, 20U);
        const std::size_t oldSize = bytes.size();
        bytes.resize(oldSize + 20U, 0);
        writeLe32(bytes, oldSize, 0x400U);
        writeLe32(bytes, oldSize + 4, 0U);
        writeLe32(bytes, oldSize + 8, 8U);
        writeLe32(bytes, oldSize + 12, nextCount);
        writeLe32(bytes, oldSize + 16, unixTimestamp);
    }

    writeProfileBytesAtomically(path, bytes);
    return nextCount;
}

void setProfileSceneRecordU32(const fs::path &path,
                              std::string_view sceneName,
                              std::uint32_t recordType,
                              std::uint32_t key,
                              std::uint32_t value) {
    auto bytes = readBinaryFile(path);
    upsertProfileSceneRecordU32(bytes, sceneName, recordType, key, value,
                                ProfileRecordUpdate::Replace);
    writeProfileBytesAtomically(path, bytes);
}

void setProfileSceneRecordBytes(const fs::path &path,
                                std::string_view sceneName,
                                std::uint32_t recordType,
                                std::uint32_t key,
                                std::span<const std::uint8_t> payloadBytes) {
    if (sceneName.empty() || sceneName.size() >= 32) {
        throw std::runtime_error("Invalid FGP profile scene name");
    }
    if (payloadBytes.size() > std::numeric_limits<std::uint32_t>::max() - 12U) {
        throw std::runtime_error("FGP profile record payload is too large");
    }

    auto bytes = readBinaryFile(path);
    const auto scenes = profileSceneEntries(bytes);
    const auto scene = std::find_if(
        scenes.begin(), scenes.end(),
        [&](const ProfileSceneEntry &entry) { return entry.name == sceneName; });
    const auto makeRecord = [&] {
        std::vector<std::uint8_t> record(12U + payloadBytes.size(), 0U);
        writeLe32(record, 0U, recordType);
        writeLe32(record, 4U, key);
        writeLe32(record, 8U, static_cast<std::uint32_t>(payloadBytes.size()));
        std::copy(payloadBytes.begin(), payloadBytes.end(), record.begin() + 12);
        return record;
    };
    const auto shiftFollowingScenes = [&](std::size_t threshold,
                                          std::int64_t delta,
                                          std::size_t ownerSlot) {
        for (const auto &other : scenes) {
            if (other.slotOffset == ownerSlot || other.blockOffset < threshold) {
                continue;
            }
            const auto shifted = static_cast<std::int64_t>(other.blockOffset) + delta;
            if (shifted < 0 ||
                shifted > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("FGP profile scene offset overflow");
            }
            writeLe32(bytes, other.slotOffset + 36U,
                      static_cast<std::uint32_t>(shifted));
        }
    };

    if (scene != scenes.end()) {
        std::size_t position = scene->blockOffset;
        const std::size_t end = position + scene->blockSize;
        while (position < end) {
            if (position + 12U > end) {
                throw std::runtime_error("Truncated FGP profile record header");
            }
            const auto type = readLe32(bytes, position);
            const auto recordKey = readLe32(bytes, position + 4U);
            const auto payloadSize = readLe32(bytes, position + 8U);
            const std::size_t recordEnd = position + 12U + payloadSize;
            if (recordEnd > end) {
                throw std::runtime_error("Truncated FGP profile record payload");
            }
            if (type == recordType && recordKey == key) {
                auto record = makeRecord();
                const auto oldSize = recordEnd - position;
                const auto delta = static_cast<std::int64_t>(record.size()) -
                                   static_cast<std::int64_t>(oldSize);
                const auto newBlockSize =
                    static_cast<std::int64_t>(scene->blockSize) + delta;
                if (newBlockSize < 0 ||
                    newBlockSize > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("FGP profile scene block overflow");
                }
                bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                            bytes.begin() + static_cast<std::ptrdiff_t>(recordEnd));
                bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                             record.begin(), record.end());
                writeLe32(bytes, scene->slotOffset + 40U,
                          static_cast<std::uint32_t>(newBlockSize));
                shiftFollowingScenes(recordEnd, delta, scene->slotOffset);
                writeProfileBytesAtomically(path, bytes);
                return;
            }
            position = recordEnd;
        }

        auto record = makeRecord();
        if (record.size() >
            std::numeric_limits<std::uint32_t>::max() - scene->blockSize) {
            throw std::runtime_error("FGP profile scene block overflow");
        }
        // The original record manager links a newly created game-owned record
        // ahead of the engine-owned 0x400 visit record. The Whack-a-Mole
        // before/after capture demonstrates 0x401 then 0x400 after modal close.
        const std::size_t insertion = scene->blockOffset;
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insertion),
                     record.begin(), record.end());
        writeLe32(bytes, scene->slotOffset + 40U,
                  scene->blockSize + static_cast<std::uint32_t>(record.size()));
        shiftFollowingScenes(insertion, static_cast<std::int64_t>(record.size()),
                             scene->slotOffset);
        writeProfileBytesAtomically(path, bytes);
        return;
    }

    std::optional<std::size_t> emptySlot;
    for (std::size_t index = 0; index < kProfileDirectoryEntries; ++index) {
        const std::size_t slot =
            kProfileDirectoryOffset + index * kProfileDirectoryEntrySize;
        if (bytes[slot] == 0U) {
            emptySlot = slot;
            break;
        }
    }
    if (!emptySlot) {
        throw std::runtime_error("FGP profile scene directory is full");
    }
    auto record = makeRecord();
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("FGP profile scene offset overflow");
    }
    const auto blockOffset = static_cast<std::uint32_t>(bytes.size());
    std::copy(sceneName.begin(), sceneName.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(*emptySlot));
    writeLe32(bytes, *emptySlot + 32U, 0U);
    writeLe32(bytes, *emptySlot + 36U, blockOffset);
    writeLe32(bytes, *emptySlot + 40U,
              static_cast<std::uint32_t>(record.size()));
    bytes.insert(bytes.end(), record.begin(), record.end());
    writeProfileBytesAtomically(path, bytes);
}

std::optional<ProfileScoreTable>
profileSceneScores(const fs::path &path, std::string_view sceneName) {
    return profileSceneScores(path, sceneName, 0U);
}

std::uint32_t profileCarPartCount(const fs::path &path) {
    const auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }
    return std::min<std::uint32_t>(readLe32(bytes, 0x58), 62U);
}

std::array<std::uint32_t, 8> profileAwards(const fs::path &path) {
    const auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }
    std::array<std::uint32_t, 8> awards{};
    for (std::size_t index = 0; index < awards.size(); ++index) {
        awards[index] = readLe32(bytes, 0x58U + 4U * index);
    }
    return awards;
}

void setProfileAwards(const fs::path &path,
                      const std::array<std::uint32_t, 8> &awards) {
    auto bytes = readBinaryFile(path);
    profileSceneEntries(bytes);
    if (readLe32(bytes, 0x4c) != 0x00000301U ||
        readLe32(bytes, 0x54) != 0x20U) {
        throw std::runtime_error("FGP profile has no valid award section");
    }
    for (std::size_t index = 0; index < awards.size(); ++index) {
        writeLe32(bytes, 0x58U + 4U * index, awards[index]);
    }
    writeProfileBytesAtomically(path, bytes);
}

std::optional<ProfileScoreTable>
profileSceneScores(const fs::path &path, std::string_view sceneName,
                   std::uint32_t key) {
    const auto payload =
        profileSceneRecordBytes(path, sceneName, 0x401U, key, 0x58U);
    if (!payload) {
        return std::nullopt;
    }

    ProfileScoreTable table;
    table.medalLevel = readLe32(*payload, 0U);
    if (table.medalLevel > 4U) {
        throw std::runtime_error("Invalid FGP profile medal level");
    }
    const auto count = readLe32(*payload, 4U);
    if (count > 10U) {
        throw std::runtime_error("Invalid FGP profile score count");
    }
    table.entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t offset = 8U + static_cast<std::size_t>(index) * 8U;
        table.entries.push_back(
            {readLe32(*payload, offset), readLe32(*payload, offset + 4U)});
    }
    return table;
}

void insertProfileScore(ProfileScoreTable &table,
                        std::uint32_t timestamp,
                        std::uint32_t score) {
    const auto insertion = std::find_if(
        table.entries.begin(), table.entries.end(),
        [score](const ProfileScoreEntry &entry) { return score > entry.score; });
    table.entries.insert(insertion, {timestamp, score});
    if (table.entries.size() > 10U) {
        table.entries.resize(10U);
    }
}

void recordProfileSceneScore(const fs::path &path,
                             std::string_view sceneName,
                             std::uint32_t timestamp,
                             std::uint32_t score,
                             std::uint32_t medalLevel,
                             std::uint32_t key) {
    if (medalLevel > 4U) {
        throw std::runtime_error("Invalid FGP profile medal level");
    }
    ProfileScoreTable table =
        profileSceneScores(path, sceneName, key).value_or(ProfileScoreTable{});
    insertProfileScore(table, timestamp, score);
    table.medalLevel = std::max(table.medalLevel, medalLevel);

    std::vector<std::uint8_t> payload(0x58U, 0U);
    writeLe32(payload, 0U, table.medalLevel);
    writeLe32(payload, 4U,
              static_cast<std::uint32_t>(table.entries.size()));
    for (std::size_t index = 0; index < table.entries.size(); ++index) {
        const std::size_t offset = 8U + index * 8U;
        writeLe32(payload, offset, table.entries[index].timestamp);
        writeLe32(payload, offset + 4U, table.entries[index].score);
    }
    setProfileSceneRecordBytes(path, sceneName, 0x401U, key, payload);
}

namespace {

// A char[8] field as a C string, cut where the original's copies cut it:
// strncpy(dst, src, 8) followed by dst[7] = 0 (0x00409560, 0x00484580).
std::string optionsCode(std::span<const std::uint8_t> field) {
    std::string code;
    for (std::size_t index = 0; index < 7U && index < field.size(); ++index) {
        if (field[index] == 0U) {
            break;
        }
        code.push_back(static_cast<char>(field[index]));
    }
    return code;
}

bool equalsIgnoringAsciiCase(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                      [](char a, char b) {
                          const auto lower = [](char c) {
                              return c >= 'A' && c <= 'Z'
                                         ? static_cast<char>(c + ('a' - 'A'))
                                         : c;
                          };
                          return lower(a) == lower(b);
                      });
}

} // namespace

std::string normalizedSecondLanguage(std::string_view code) {
    // 0x00484580: _stricmp("no", code) == 0 empties the buffer; anything
    // else is copied with strncpy(..., 8) and byte 7 cleared.
    if (equalsIgnoringAsciiCase("no", code)) {
        return {};
    }
    return std::string(code.substr(0, 7));
}

ProfileOptions profileOptions(const std::optional<fs::path> &path) {
    // 0x0040b490 fills both buffers with the defaults before each read and
    // applies them whether or not the read succeeds; 0x00411680 reads
    // nothing when no profile is loaded ([0x0054f6f4] == 0).
    ProfileOptions options;
    if (!path) {
        return options;
    }
    if (const auto volumes = profileSceneRecordBytes(
            *path, kProfileOptionsScene, kProfileOptionsRecordType, 0U, 12U)) {
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const std::uint32_t bits = readLe32(*volumes, channel * 4U);
            std::memcpy(&options.volumes[channel], &bits, sizeof(bits));
        }
    }
    if (const auto subtitles = profileSceneRecordBytes(
            *path, kProfileOptionsScene, kProfileOptionsRecordType, 1U, 20U)) {
        const std::span<const std::uint8_t> payload(*subtitles);
        options.subtitlesEnabled = readLe32(payload, 0U);
        options.subtitleLanguage = optionsCode(payload.subspan(4U, 8U));
        // 0x00484580 compares the field with _stricmp before copying, so an
        // unterminated field is read to its eighth byte here.
        std::string second;
        for (std::size_t index = 12U; index < 20U && payload[index] != 0U;
             ++index) {
            second.push_back(static_cast<char>(payload[index]));
        }
        options.secondLanguage = normalizedSecondLanguage(second);
    }
    return options;
}

void setProfileOptions(const fs::path &path, const ProfileOptions &options) {
    // 0x0040b5a0 builds both payloads in zeroed stack buffers and strcpy's
    // the two codes into them, so every byte after a terminator is zero.
    std::vector<std::uint8_t> volumes(12U, 0U);
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &options.volumes[channel], sizeof(bits));
        writeLe32(volumes, channel * 4U, bits);
    }
    std::vector<std::uint8_t> subtitles(20U, 0U);
    writeLe32(subtitles, 0U, options.subtitlesEnabled);
    const auto copyCode = [&](std::string_view code, std::size_t offset) {
        const std::size_t length = std::min<std::size_t>(code.size(), 7U);
        for (std::size_t index = 0; index < length && code[index] != '\0';
             ++index) {
            subtitles[offset + index] = static_cast<std::uint8_t>(code[index]);
        }
    };
    copyCode(options.subtitleLanguage, 4U);
    copyCode(options.secondLanguage, 12U);
    setProfileSceneRecordBytes(path, kProfileOptionsScene,
                               kProfileOptionsRecordType, 0U, volumes);
    setProfileSceneRecordBytes(path, kProfileOptionsScene,
                               kProfileOptionsRecordType, 1U, subtitles);
}

void writeFreshProfile(const fs::path &profileDirectory,
                       const std::string &utf8Name,
                       std::string_view cp1252Name,
                       int zeroBasedMonth,
                       int day) {
    if (cp1252Name.empty() || cp1252Name.size() > 15) {
        throw std::runtime_error("Invalid profile name length");
    }
    fs::create_directories(profileDirectory);
    const fs::path destination = profileDirectory / (utf8Name + ".pfl");
    if (fs::exists(destination)) {
        throw std::runtime_error("Profile already exists");
    }

    // This is the exact empty structure emitted by FGP.exe at 0x00411060:
    // a 0x102 container holding profile metadata and three empty sections.
    constexpr std::size_t totalSize = 12 + 0xb78;
    std::vector<std::uint8_t> bytes(totalSize, 0);
    writeLe32(bytes, 0x00, 0x00face01);
    writeLe32(bytes, 0x04, 0x00000102);
    writeLe32(bytes, 0x08, 0x00000b78);

    writeLe32(bytes, 0x0c, 0x00000100);
    writeLe32(bytes, 0x10, 0);
    writeLe32(bytes, 0x14, 0x24);
    std::copy(cp1252Name.begin(), cp1252Name.end(), bytes.begin() + 0x18);
    writeLe32(bytes, 0x34, static_cast<std::uint32_t>(zeroBasedMonth));
    writeLe32(bytes, 0x38, static_cast<std::uint32_t>(day));

    writeLe32(bytes, 0x3c, 0x00000300);
    writeLe32(bytes, 0x40, 0);
    writeLe32(bytes, 0x44, 4);
    writeLe32(bytes, 0x48, static_cast<std::uint32_t>(std::time(nullptr)));

    writeLe32(bytes, 0x4c, 0x00000301);
    writeLe32(bytes, 0x50, 0);
    writeLe32(bytes, 0x54, 0x20);

    writeLe32(bytes, 0x78, 0x00000200);
    writeLe32(bytes, 0x7c, 0);
    writeLe32(bytes, 0x80, 0x0b00);

    fs::path temporary = destination;
    temporary += ".native-new";
    if (fs::exists(temporary)) {
        throw std::runtime_error("Temporary profile path already exists");
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()))) {
            throw std::runtime_error("Could not write new profile");
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("Could not finish writing new profile");
        }
    }
    try {
        fs::rename(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace fgp
