#pragma once

// Browser packages backed by user-selected local files. The import worker
// writes entry indexes and empty placeholders into the virtual filesystem;
// payloads are read on demand from local Blob slices, never over HTTP.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fgp {

bool isLocalBrowserPackage(const std::filesystem::path &archive);
void readLocalBrowserRange(const std::filesystem::path &archive, std::size_t offset,
                           std::size_t size, std::uint8_t *destination);

// Retained for the scene-loading UI. Local imports need no network prefetch.
double sceneDataProgress(const std::string &scene);

} // namespace fgp
