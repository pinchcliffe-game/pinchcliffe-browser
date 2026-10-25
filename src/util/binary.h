#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace fgp {
namespace fs = std::filesystem;

inline std::uint32_t readLe32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("Unexpected end of package");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

inline void writeLe32(std::vector<std::uint8_t> &bytes,
               std::size_t offset,
               std::uint32_t value) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("Profile serialization overflow");
    }
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::vector<std::uint8_t> readBinaryFile(const fs::path &path);

} // namespace fgp
