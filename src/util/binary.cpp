#include "util/binary.h"

#include <fstream>

namespace fgp {

std::vector<std::uint8_t> readBinaryFile(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine file size: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot read file: " + path.string());
    }
    return bytes;
}

} // namespace fgp
