#pragma once

// Read-only access to the scene graph recovered from the original FGP.exe:
// scene registry, cursor tables, and animation descriptors at fixed RVAs of
// the update-1.2 build (see REVERSE_ENGINEERING.md, Provenance).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/binary.h"
#include "assets/original_data.h"

namespace fgp {
namespace fs = std::filesystem;

class ExecutableGameData {
public:
    struct AnimationDefinition {
        std::uint32_t address{};
        std::string name;
        int x{};
        int y{};
        int layer{};
        std::uint32_t hotspot{};
        std::uint32_t conflictMask{};
        bool visible{};
        // Descriptor +0x48: scene_DrawAnim blits with a source colour key.
        bool keyed{};
        // Descriptor +0x40: a Smacker player rather than a bitmap
        // (read at 0x004128c8), and +0x4c: the player loops.
        bool smacker{};
        bool loop{};
    };

    struct BuildacarRectangle {
        int left{};
        int top{};
        int right{};
        int bottom{};
    };

    struct BuildacarPartDefinition {
        std::uint32_t number{};
        BuildacarRectangle menuBounds;
        BuildacarRectangle targetBounds;
        std::int32_t dependency{-1};
        std::int32_t variantGroup{-1};
    };

    // One 16-byte row of the placed-part milestones at 0x004b0d80
    // (0x004244b0 scans them up to 0x004b0f80).
    struct BuildacarMilestone {
        std::uint32_t part{};
        std::uint32_t kind{};
        std::uint32_t first{};
        std::uint32_t second{};
    };

    struct BuildacarStageDefinition {
        std::uint32_t firstPart{};
        std::vector<BuildacarPartDefinition> parts;
    };

    struct WhackamoleLevelDefinition {
        std::uint32_t birds{};
        std::uint32_t squirrels{};
        std::uint32_t maximumLoops{};
        std::uint32_t difficulty{};
        std::uint32_t durationMilliseconds{};
    };

    // Empty instance; usable as a class member assigned during loading.
    ExecutableGameData() = default;

    explicit ExecutableGameData(const fs::path &path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("Cannot open executable: " + path.string());
        }
        const auto end = input.tellg();
        if (end < 0) {
            throw std::runtime_error("Cannot determine executable size");
        }
        bytes_.resize(static_cast<std::size_t>(end));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(bytes_.data()),
                        static_cast<std::streamsize>(bytes_.size()))) {
            throw std::runtime_error("Cannot read executable: " + path.string());
        }
        parsePe();
        parseCursorMaps();
        parseAnimationDefinitions();
        parseSceneAnimationReferences();
        OriginalData::instance().install(bytes_);
    }

    // The type-3 (scored) scenes in registry order, as 0x0040d9b0 walks
    // them (`cmp dword ptr [edx], 3` at 0x0040d9d2).
    [[nodiscard]] const std::vector<std::string> &scoredScenes() const {
        return scoredScenes_;
    }

    // Bit 2 of a scene record's +0x28 flags: GAME_EnterScene (0x0040ccd0)
    // tests it on the scene being entered (0x0040cce5) and, when set, draws
    // the Loading box 0x0040cb20, which first pauses the music (0x0040cb46
    // calls 0x0040b6f0).
    [[nodiscard]] bool sceneShowsLoadingBox(std::string_view scene) const {
        return std::find(loadingBoxScenes_.begin(), loadingBoxScenes_.end(),
                         scene) != loadingBoxScenes_.end();
    }

    [[nodiscard]] std::string_view cursorFor(std::string_view scene,
                                              std::uint32_t hotspot) const {
        const auto sceneFound = maps_.find(std::string(scene));
        if (sceneFound == maps_.end()) {
            return "default";
        }
        const auto cursorFound = sceneFound->second.find(hotspot);
        return cursorFound == sceneFound->second.end()
                   ? std::string_view("default")
                   : std::string_view(cursorFound->second);
    }

    [[nodiscard]] std::size_t sceneCount() const { return maps_.size(); }

    [[nodiscard]] const std::vector<AnimationDefinition> &
    animationDefinitionsNamed(std::string_view name) const {
        std::string key(name);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        const auto found = animationsByName_.find(key);
        return found == animationsByName_.end() ? emptyAnimations_
                                                : found->second;
    }

    [[nodiscard]] const std::vector<AnimationDefinition> &
    directlyReferencedAnimations(std::string_view scene) const {
        const auto found = directAnimationsByScene_.find(std::string(scene));
        return found == directAnimationsByScene_.end() ? emptyAnimations_
                                                       : found->second;
    }

    // Raw reads for tables a scene module recovers itself (the activity
    // page's picture library and tooltip tables).
    [[nodiscard]] std::uint32_t u32At(std::uint32_t address) const {
        return va32(address);
    }
    [[nodiscard]] std::string stringAt(std::uint32_t address) const {
        return vaString(address);
    }

    [[nodiscard]] std::array<std::vector<AnimationDefinition>, 3>
    yardOpeningSequences() const {
        // fcn.00428350 chooses one of these three null-terminated animation
        // chains through the game's no-repeat random-bag helper.
        return {
            animationSequence(imageBase_ + 0x000cb050U),
            animationSequence(imageBase_ + 0x000cb064U),
            animationSequence(imageBase_ + 0x000cb074U),
        };
    }

    [[nodiscard]] AnimationDefinition yardOpeningNarration() const {
        // The yard schedules this descriptor two seconds after entry.
        return animationDefinition(imageBase_ + 0x000c8ae8U);
    }

    [[nodiscard]] AnimationDefinition animationAtRva(std::uint32_t rva) const {
        return animationDefinition(imageBase_ + rva);
    }

    [[nodiscard]] std::uint32_t
    animationHotspotAtRva(std::uint32_t rva) const {
        return va32(imageBase_ + rva + 0x50U);
    }

    [[nodiscard]] std::vector<AnimationDefinition>
    animationSequenceAtRva(std::uint32_t rva) const {
        return animationSequence(imageBase_ + rva);
    }

    [[nodiscard]] std::vector<BuildacarMilestone> buildacarMilestones() const {
        std::vector<BuildacarMilestone> result;
        for (std::uint32_t entry = imageBase_ + 0x000b0d80U;
             entry < imageBase_ + 0x000b0f80U; entry += 16U) {
            result.push_back({va32(entry), va32(entry + 4U),
                              va32(entry + 8U), va32(entry + 12U)});
        }
        return result;
    }

    [[nodiscard]] BuildacarStageDefinition
    buildacarStage(std::size_t stage) const {
        // fcn.00423500 indexes these five tables when changing workshop
        // stage. 0x004238d0 consumes the menu rectangles, 0x004239f0 the
        // destination rectangles, and 0x00424210 the dependency table.
        if (stage >= 5U) {
            throw std::runtime_error("Invalid Build-a-Car stage");
        }
        const std::uint32_t count =
            va32(imageBase_ + 0x0009cc70U + static_cast<std::uint32_t>(stage * 4U));
        const std::uint32_t firstPart =
            va32(imageBase_ + 0x0009cc84U + static_cast<std::uint32_t>(stage * 4U));
        if (count == 0U || count > 32U || firstPart == 0U ||
            firstPart + count - 1U > 62U) {
            throw std::runtime_error("Invalid Build-a-Car part table");
        }
        const auto tablePointer = [&](std::uint32_t rva) {
            return va32(imageBase_ + rva +
                        static_cast<std::uint32_t>(stage * 4U));
        };
        const std::uint32_t menuTable = tablePointer(0x000b0d30U);
        const std::uint32_t targetTable = tablePointer(0x000b0d44U);
        const std::uint32_t dependencyTable = tablePointer(0x000b0d58U);
        const std::uint32_t variantTable = tablePointer(0x000b0d6cU);

        BuildacarStageDefinition result;
        result.firstPart = firstPart;
        result.parts.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto rectangle = [&](std::uint32_t table) {
                const std::uint32_t entry = table + index * 16U;
                return BuildacarRectangle{
                    vaI32(entry), vaI32(entry + 4U),
                    vaI32(entry + 8U), vaI32(entry + 12U)};
            };
            result.parts.push_back({
                firstPart + index,
                rectangle(menuTable),
                rectangle(targetTable),
                vaI32(dependencyTable + index * 4U),
                vaI32(variantTable + index * 4U),
            });
        }
        return result;
    }

    [[nodiscard]] std::array<WhackamoleLevelDefinition, 18>
    whackamoleLevels() const {
        // fcn.0044df80 selects one 20-byte record by (level - 1) % 18.
        // 0x0044e740 consumes bird/squirrel counts; 0x0044e830 and
        // 0x0044ea30 consume the loop bound; +0x10 is the level clock.
        std::array<WhackamoleLevelDefinition, 18> result{};
        const std::uint32_t table = imageBase_ + 0x000e59f8U;
        for (std::size_t index = 0; index < result.size(); ++index) {
            const std::uint32_t entry =
                table + static_cast<std::uint32_t>(index * 20U);
            result[index] = {va32(entry), va32(entry + 4U),
                             va32(entry + 8U), va32(entry + 12U),
                             va32(entry + 16U)};
        }
        return result;
    }

private:
    struct Section {
        std::uint32_t rva{};
        std::uint32_t virtualSize{};
        std::uint32_t rawSize{};
        std::uint32_t rawOffset{};
    };

    [[nodiscard]] std::uint16_t raw16(std::size_t offset) const {
        if (offset + 2 > bytes_.size()) {
            throw std::runtime_error("Truncated PE data");
        }
        return static_cast<std::uint16_t>(bytes_[offset]) |
               (static_cast<std::uint16_t>(bytes_[offset + 1]) << 8U);
    }

    [[nodiscard]] std::uint32_t raw32(std::size_t offset) const {
        if (offset + 4 > bytes_.size()) {
            throw std::runtime_error("Truncated PE data");
        }
        return static_cast<std::uint32_t>(bytes_[offset]) |
               (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(bytes_[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(bytes_[offset + 3]) << 24U);
    }

    [[nodiscard]] std::size_t vaOffset(std::uint32_t address,
                                       std::size_t length = 1) const {
        if (address < imageBase_) {
            throw std::runtime_error("PE address is below image base");
        }
        const std::uint32_t rva = address - imageBase_;
        for (const auto &section : sections_) {
            const std::uint32_t extent =
                std::max(section.virtualSize, section.rawSize);
            if (rva < section.rva || rva - section.rva >= extent) {
                continue;
            }
            const std::size_t delta = rva - section.rva;
            if (delta + length > section.rawSize) {
                throw std::runtime_error("PE address has no file-backed data");
            }
            return static_cast<std::size_t>(section.rawOffset) + delta;
        }
        throw std::runtime_error("PE address is outside known sections");
    }

    [[nodiscard]] std::uint32_t va32(std::uint32_t address) const {
        return raw32(vaOffset(address, 4));
    }

    [[nodiscard]] std::int32_t vaI32(std::uint32_t address) const {
        return static_cast<std::int32_t>(va32(address));
    }

    [[nodiscard]] std::string vaString(std::uint32_t address) const {
        const std::size_t offset = vaOffset(address);
        std::size_t end = offset;
        while (end < bytes_.size() && bytes_[end] != 0 && end - offset < 256) {
            ++end;
        }
        if (end == bytes_.size() || end - offset == 256) {
            throw std::runtime_error("Unterminated PE string");
        }
        return std::string(reinterpret_cast<const char *>(bytes_.data() + offset),
                           end - offset);
    }

    void parsePe() {
        if (bytes_.size() < 0x40 || raw16(0) != 0x5a4d) {
            throw std::runtime_error("FGP.exe is not a DOS/PE executable");
        }
        const std::size_t pe = raw32(0x3c);
        if (raw32(pe) != 0x00004550) {
            throw std::runtime_error("FGP.exe has no PE signature");
        }
        const std::uint16_t sectionCount = raw16(pe + 6);
        const std::uint16_t optionalSize = raw16(pe + 20);
        const std::size_t optional = pe + 24;
        if (raw16(optional) != 0x10b) {
            throw std::runtime_error("FGP.exe is not a 32-bit PE image");
        }
        imageBase_ = raw32(optional + 28);
        const std::size_t sectionHeaders = optional + optionalSize;
        sections_.reserve(sectionCount);
        for (std::uint16_t index = 0; index < sectionCount; ++index) {
            const std::size_t header = sectionHeaders + index * 40U;
            sections_.push_back({raw32(header + 12), raw32(header + 8),
                                 raw32(header + 16), raw32(header + 20)});
        }
    }

    void parseCursorMaps() {
        // Scene registry recovered from GAME_FindScene (0x0040cc70).
        const std::uint32_t table = imageBase_ + 0x000a7f08U;
        const std::uint32_t countAddress = imageBase_ + 0x000a7ff0U;
        const std::uint32_t count = va32(countAddress);
        if (count == 0 || count > 256) {
            throw std::runtime_error("Unexpected FGP scene registry size");
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uint32_t descriptor = va32(table + index * 4U);
            const std::string scene = vaString(va32(descriptor + 4));
            const std::uint32_t sceneType = va32(descriptor);
            const bool primaryScene = sceneType != 0;
            if (sceneType == 3U) {
                scoredScenes_.push_back(scene);
            }
            if ((va32(descriptor + 0x28U) & 2U) != 0U) {
                loadingBoxScenes_.push_back(scene);
            }
            if ((sceneType == 3U || sceneType == 4U) &&
                va32(descriptor + 0x24U) != 0U) {
                sceneCallbacks_.push_back(
                    {scene, va32(descriptor + 0x24U)});
            }
            const std::uint32_t cursorMap =
                va32(descriptor + (primaryScene ? 0x10U : 0x08U));
            if (cursorMap == 0) {
                continue;
            }
            auto &destination = maps_[scene];
            for (std::uint32_t cursorIndex = 0; cursorIndex < 256; ++cursorIndex) {
                const std::uint32_t entry = cursorMap + cursorIndex * 8U;
                const auto hotspot = static_cast<std::int32_t>(va32(entry));
                const std::uint32_t name = va32(entry + 4);
                if (hotspot < 0 || name == 0) {
                    break;
                }
                destination[static_cast<std::uint32_t>(hotspot)] = vaString(name);
            }
        }
    }

    [[nodiscard]] AnimationDefinition
    animationDefinition(std::uint32_t address) const {
        return {
            address,
            vaString(address),
            vaI32(address + 0x54U),
            vaI32(address + 0x58U),
            vaI32(address + 0x64U),
            va32(address + 0x50U),
            va32(address + 0x60U),
            va32(address + 0x44U) != 0,
            va32(address + 0x48U) != 0,
            va32(address + 0x40U) != 0,
            va32(address + 0x4cU) != 0,
        };
    }

    void parseAnimationDefinitions() {
        constexpr std::size_t descriptorSize = 0xa8U;
        for (const auto &section : sections_) {
            for (std::uint32_t relative = 0;
                 relative + descriptorSize <= section.rawSize;
                 relative += 4U) {
                const std::size_t offset = section.rawOffset + relative;
                std::size_t nameLength = 0;
                while (nameLength < 0x40U &&
                       bytes_[offset + nameLength] != 0U) {
                    const auto value = bytes_[offset + nameLength];
                    if (value < 0x20U || value > 0x7eU) {
                        nameLength = 0;
                        break;
                    }
                    ++nameLength;
                }
                if (nameLength == 0U || nameLength >= 0x40U ||
                    std::any_of(bytes_.begin() + offset + nameLength,
                                bytes_.begin() + offset + 0x40U,
                                [](std::uint8_t value) { return value != 0U; })) {
                    continue;
                }
                const std::array flags{
                    raw32(offset + 0x40U), raw32(offset + 0x44U),
                    raw32(offset + 0x48U), raw32(offset + 0x4cU)};
                if (std::any_of(flags.begin(), flags.end(),
                                [](std::uint32_t value) { return value > 1U; }) ||
                    (flags[0] == 0U && flags[1] == 0U && flags[2] == 0U)) {
                    continue;
                }
                const auto x = static_cast<std::int32_t>(raw32(offset + 0x54U));
                const auto y = static_cast<std::int32_t>(raw32(offset + 0x58U));
                const auto layer =
                    static_cast<std::int32_t>(raw32(offset + 0x64U));
                if (x < -4096 || x > 4096 || y < -4096 || y > 4096 ||
                    layer < -1024 || layer > 1024) {
                    continue;
                }
                std::string name(
                    reinterpret_cast<const char *>(bytes_.data() + offset),
                    nameLength);
                std::string key = name;
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char value) {
                                   return static_cast<char>(std::tolower(value));
                               });
                const std::uint32_t address =
                    imageBase_ + section.rva + relative;
                animationsByName_[key].push_back({
                    address, name, x, y, layer, raw32(offset + 0x50U),
                    raw32(offset + 0x60U), flags[1] != 0U, flags[2] != 0U});
                animationsByAddress_[address] =
                    animationsByName_[key].back();
            }
        }
    }

    void parseSceneAnimationReferences() {
        std::sort(sceneCallbacks_.begin(), sceneCallbacks_.end(),
                  [](const auto &left, const auto &right) {
                      return left.second < right.second;
                  });
        for (std::size_t index = 0; index < sceneCallbacks_.size(); ++index) {
            const auto &[scene, begin] = sceneCallbacks_[index];
            std::uint32_t end = 0U;
            if (index + 1U < sceneCallbacks_.size()) {
                end = sceneCallbacks_[index + 1U].second;
            } else {
                for (const auto &section : sections_) {
                    const std::uint32_t sectionBegin =
                        imageBase_ + section.rva;
                    const std::uint32_t sectionEnd =
                        sectionBegin + section.rawSize;
                    if (begin >= sectionBegin && begin < sectionEnd) {
                        end = sectionEnd;
                        break;
                    }
                }
            }
            if (end <= begin) {
                continue;
            }
            const std::size_t offset = vaOffset(begin, end - begin);
            auto &result = directAnimationsByScene_[scene];
            for (std::size_t position = offset;
                 position + 4U <= offset + (end - begin); ++position) {
                const auto found =
                    animationsByAddress_.find(raw32(position));
                if (found == animationsByAddress_.end() ||
                    std::any_of(result.begin(), result.end(),
                                [&](const AnimationDefinition &item) {
                                    return item.address == found->first;
                                })) {
                    continue;
                }
                result.push_back(found->second);
            }
        }
    }

    [[nodiscard]] std::vector<AnimationDefinition>
    animationSequence(std::uint32_t table) const {
        std::vector<AnimationDefinition> result;
        for (std::uint32_t index = 0; index < 256; ++index) {
            const std::uint32_t descriptor = va32(table + index * 4U);
            if (descriptor == 0) {
                break;
            }
            result.push_back(animationDefinition(descriptor));
        }
        if (result.empty()) {
            throw std::runtime_error("Empty executable animation sequence");
        }
        return result;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint32_t imageBase_{};
    std::vector<Section> sections_;
    std::unordered_map<std::string,
                       std::unordered_map<std::uint32_t, std::string>> maps_;
    std::unordered_map<std::string, std::vector<AnimationDefinition>>
        animationsByName_;
    std::unordered_map<std::uint32_t, AnimationDefinition>
        animationsByAddress_;
    std::vector<std::pair<std::string, std::uint32_t>> sceneCallbacks_;
    std::vector<std::string> scoredScenes_;
    std::vector<std::string> loadingBoxScenes_;
    std::unordered_map<std::string, std::vector<AnimationDefinition>>
        directAnimationsByScene_;
    std::vector<AnimationDefinition> emptyAnimations_;
};

} // namespace fgp
