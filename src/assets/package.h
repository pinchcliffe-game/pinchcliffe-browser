#pragma once

// Reader for the original DATA packages (Data*/Language*/*.bin).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/binary.h"
#ifdef __EMSCRIPTEN__
#include "platform/remote_data.h"
#endif
#include "util/text.h"

namespace fgp {
namespace fs = std::filesystem;

constexpr std::size_t kPackageHeaderSize = 12;
constexpr std::size_t kEntryHeaderSize = 264;
constexpr std::size_t kEntryNameSize = 72;
constexpr std::size_t kEntryPayloadSizeOffset = 260;

// Development aid for the browser build: when FGP_RECORD_PACKAGE_READS names
// a file, every archive payload read for the first time is appended to it as
// "<package path>\t<offset>\t<size>". tools/web/record_scene_reads.py runs
// each scene this way to learn what to prefetch before loading it.
inline void recordPackageRead(const fs::path &package, std::size_t offset,
                              std::size_t size) {
    static std::FILE *log = [] {
        const char *path = std::getenv("FGP_RECORD_PACKAGE_READS");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    if (log) {
        std::fprintf(log, "%s\t%zu\t%zu\n", package.string().c_str(), offset, size);
        std::fflush(log);
    }
}

class Package {
public:
    struct Entry {
        std::string name;
        // Byte offset of the payload in the archive file; unused for a
        // loose-file package, whose entries read `file` instead.
        std::size_t dataOffset{};
        std::size_t size{};
        fs::path file;
    };

    // Empty package; usable as a class member assigned during loading.
    Package() = default;

    // Accepts either a packed DATA archive or a directory holding the same
    // assets as loose files, so an installation can be used in whichever
    // form is at hand.
    //
    // Only the entry table is read here. Each payload is read from disk the
    // first time data() asks for it and then stays resident for the life of
    // the package, so a span returned by data() remains valid as long as the
    // package does (the streamed Smacker decoder relies on that). Packages
    // are used from the main thread only; data() is not thread-safe.
    explicit Package(const fs::path &path) : path_(path) {
        if (fs::is_directory(path)) {
            loadDirectory();
        } else {
            loadArchive();
        }
    }

    [[nodiscard]] const fs::path &path() const { return path_; }
    [[nodiscard]] const std::vector<Entry> &entries() const { return entries_; }

    [[nodiscard]] std::span<const std::uint8_t> data(const Entry &entry) const {
        const auto index = static_cast<std::size_t>(&entry - entries_.data());
        if (index >= entries_.size() || &entries_[index] != &entry) {
            throw std::logic_error("Entry does not belong to " + path_.string());
        }
        auto &payload = payloads_[index];
        if (!payload) {
            payload = std::make_unique<std::vector<std::uint8_t>>(readPayload(entry));
            residentBytes_ += entry.size;
            if (entry.file.empty()) {
                recordPackageRead(path_, entry.dataOffset, entry.size);
            }
        }
        return {payload->data(), payload->size()};
    }

    // Payload bytes read so far; the whole package is never held at once.
    [[nodiscard]] std::size_t residentBytes() const { return residentBytes_; }

    [[nodiscard]] const Entry &find(std::string_view name) const {
        const Entry *entry = findOptional(name);
        if (!entry) {
            throw std::runtime_error("Asset not found in " + path_.string() + ": " +
                                     std::string(name));
        }
        return *entry;
    }

    [[nodiscard]] const Entry *findOptional(std::string_view name) const {
        const auto wanted = normalizedPath(std::string(name));
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) {
            return normalizedPath(entry.name) == wanted;
        });
        if (found == entries_.end()) {
            return nullptr;
        }
        return &*found;
    }

private:
    void loadArchive() {
#ifdef __EMSCRIPTEN__
        // The browser imports local files; the virtual file system
        // holds only an empty placeholder and the entry index beside it.
        if (isLocalBrowserPackage(path_)) {
            loadBrowserIndex();
            return;
        }
#endif
        std::ifstream input(path_, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("Cannot open package: " + path_.string());
        }
        const auto end = input.tellg();
        if (end < 0) {
            throw std::runtime_error("Cannot determine package size: " + path_.string());
        }
        parse(input, static_cast<std::size_t>(end));
        payloads_.resize(entries_.size());
    }

    // A package extracted to loose files: every regular file below the
    // directory becomes an entry named by its relative path, which
    // normalizedPath() maps onto the same key the packed names use.
    void loadDirectory() {
        std::vector<fs::path> files;
        for (const auto &item : fs::recursive_directory_iterator(path_)) {
            if (!item.is_regular_file()) {
                continue;
            }
            const fs::path relative = item.path().lexically_relative(path_);
            // Extraction parks colliding names here; they are not assets.
            if (std::find(relative.begin(), relative.end(), "__duplicates__") !=
                relative.end()) {
                continue;
            }
            files.push_back(item.path());
        }
        // Keep a deterministic order so first-match lookups are stable.
        std::sort(files.begin(), files.end(),
                  [this](const fs::path &left, const fs::path &right) {
                      return normalizedPath(
                                 left.lexically_relative(path_).generic_string()) <
                             normalizedPath(
                                 right.lexically_relative(path_).generic_string());
                  });

        entries_.reserve(files.size());
        for (const fs::path &file : files) {
            std::error_code error;
            const auto size = fs::file_size(file, error);
            if (error) {
                throw std::runtime_error("Cannot determine loose asset size: " +
                                         file.string());
            }
            entries_.push_back({file.lexically_relative(path_).generic_string(),
                                0, static_cast<std::size_t>(size), file});
        }
        payloads_.resize(entries_.size());
    }

#ifdef __EMSCRIPTEN__
    // `<package>.idx`, written locally by tools/web/local-files-worker.js: the magic
    // "FGPIDX1\n", a little-endian entry count, then per entry a 16-bit name
    // length, the name, and 32-bit payload offset and size.
    void loadBrowserIndex() {
        localBrowser_ = true;
        const fs::path indexPath(path_.string() + ".idx");
        std::ifstream input(indexPath, std::ios::binary | std::ios::ate);
        const auto end = input.tellg();
        if (!input || end < 12) {
            throw std::runtime_error("Cannot read package index: " + indexPath.string());
        }
        std::vector<std::uint8_t> index(static_cast<std::size_t>(end));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(index.data()),
                   static_cast<std::streamsize>(index.size()));
        if (std::memcmp(index.data(), "FGPIDX1\n", 8) != 0) {
            throw std::runtime_error("Not a package index: " + indexPath.string());
        }
        const std::span<const std::uint8_t> bytes(index);
        const auto count = readLe32(bytes, 8);
        std::size_t position = 12;
        entries_.reserve(count);
        for (std::uint32_t item = 0; item < count; ++item) {
            if (position + 2 > bytes.size()) {
                throw std::runtime_error("Truncated package index: " + indexPath.string());
            }
            const std::size_t nameLength = bytes[position] | (bytes[position + 1] << 8);
            position += 2;
            if (position + nameLength + 8 > bytes.size()) {
                throw std::runtime_error("Truncated package index: " + indexPath.string());
            }
            std::string name(reinterpret_cast<const char *>(bytes.data() + position),
                             nameLength);
            position += nameLength;
            const auto offset = readLe32(bytes, position);
            const auto size = readLe32(bytes, position + 4);
            position += 8;
            entries_.push_back({std::move(name), offset, size, {}});
        }
        payloads_.resize(entries_.size());
    }
#endif

    [[nodiscard]] std::vector<std::uint8_t> readPayload(const Entry &entry) const {
#ifdef __EMSCRIPTEN__
        if (localBrowser_) {
            std::vector<std::uint8_t> bytes(entry.size);
            if (entry.size != 0) {
                readLocalBrowserRange(path_, entry.dataOffset, entry.size, bytes.data());
            }
            return bytes;
        }
#endif
        const fs::path &source = entry.file.empty() ? path_ : entry.file;
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot open package data: " + source.string());
        }
        std::vector<std::uint8_t> bytes(entry.size);
        input.seekg(static_cast<std::streamoff>(entry.file.empty() ? entry.dataOffset : 0));
        if (entry.size != 0 &&
            !input.read(reinterpret_cast<char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()))) {
            throw std::runtime_error("Cannot read " + entry.name + " from " +
                                     source.string());
        }
        return bytes;
    }

    // Walks the archive's entry headers. Each payload directly follows its
    // 264-byte header, so the table is read with one small read and one seek
    // per entry; payload bytes are not touched.
    void parse(std::ifstream &input, std::size_t fileSize) {
        std::array<std::uint8_t, kPackageHeaderSize> header{};
        input.seekg(0);
        if (fileSize < kPackageHeaderSize ||
            !input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
            std::memcmp(header.data(), "DATA", 4) != 0) {
            throw std::runtime_error("Not an FGP DATA package: " + path_.string());
        }

        const auto version = readLe32(header, 4);
        const auto count = readLe32(header, 8);
        if (version != 0x100) {
            throw std::runtime_error("Unsupported FGP package version: " +
                                     std::to_string(version));
        }

        std::array<std::uint8_t, kEntryHeaderSize> entryHeader{};
        std::size_t position = kPackageHeaderSize;
        entries_.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            input.seekg(static_cast<std::streamoff>(position));
            if (position + kEntryHeaderSize > fileSize ||
                !input.read(reinterpret_cast<char *>(entryHeader.data()),
                            entryHeader.size())) {
                throw std::runtime_error("Truncated entry header in " + path_.string());
            }

            const auto *nameStart = reinterpret_cast<const char *>(entryHeader.data());
            std::size_t nameLength = 0;
            while (nameLength < kEntryNameSize && nameStart[nameLength] != '\0') {
                ++nameLength;
            }
            const auto payloadSize = static_cast<std::size_t>(
                readLe32(entryHeader, kEntryPayloadSizeOffset));
            const auto payloadOffset = position + kEntryHeaderSize;
            if (payloadSize > fileSize - payloadOffset) {
                throw std::runtime_error("Truncated payload in " + path_.string());
            }

            entries_.push_back({std::string(nameStart, nameLength), payloadOffset,
                                payloadSize, {}});
            position = payloadOffset + payloadSize;
        }

        if (position != fileSize) {
            throw std::runtime_error("Unexpected trailing data in " + path_.string());
        }
    }

    fs::path path_;
    std::vector<Entry> entries_;
    // One lazily filled buffer per entry. Separate allocations keep earlier
    // spans valid when later entries are loaded.
    mutable std::vector<std::unique_ptr<std::vector<std::uint8_t>>> payloads_;
    mutable std::size_t residentBytes_{};
    bool localBrowser_{false};
};

// Resolves one package inside an installation, preferring the packed archive
// and falling back to a directory of loose files. Returns the archive path
// when neither exists, so callers can keep testing it with fs::exists.
inline fs::path packagePath(const fs::path &gameDirectory,
                            std::string_view layer,
                            std::string_view package) {
    const fs::path layerDirectory = gameDirectory / layer;
    const fs::path archive = layerDirectory / (std::string(package) + ".bin");
    if (fs::is_regular_file(archive)) {
        return archive;
    }
    const fs::path directory = layerDirectory / package;
    if (fs::is_directory(directory)) {
        return directory;
    }
    return archive;
}

} // namespace fgp
