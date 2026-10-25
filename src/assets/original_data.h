#pragma once

// File-backed tables from the player's executable. There is no embedded
// fallback. The source is installed after PE validation during game startup.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/binary.h"

namespace fgp {

class OriginalData {
public:
    static OriginalData &instance() {
        static OriginalData source;
        return source;
    }

    void install(std::span<const std::uint8_t> bytes) {
        if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z')
            throw std::runtime_error("Original tables require a PE executable");
        const auto pe = readLe32(bytes, 60);
        if (pe > bytes.size() || bytes.size() - pe < 24 || readLe32(bytes, pe) != 0x4550)
            throw std::runtime_error("Invalid original PE header");
        const auto word = [&](std::size_t at) {
            if (at > bytes.size() || bytes.size() - at < 2) throw std::runtime_error("Truncated PE header");
            return static_cast<std::uint16_t>(bytes[at] | bytes[at + 1] << 8U);
        };
        const auto count = word(pe + 6), optional = word(pe + 20);
        if (optional < 32 || word(pe + 24) != 0x10b || !count || count > 96)
            throw std::runtime_error("Unsupported original PE layout");
        const auto base = readLe32(bytes, pe + 52);
        const std::size_t headers = static_cast<std::size_t>(pe) + 24 + optional;
        if (headers > bytes.size() || count > (bytes.size() - headers) / 40)
            throw std::runtime_error("Truncated PE sections");
        std::vector<Section> sections;
        for (std::size_t i = 0; i < count; ++i) {
            const auto h = headers + i * 40;
            const auto rva = readLe32(bytes, h + 12), size = readLe32(bytes, h + 16), offset = readLe32(bytes, h + 20);
            if (offset > bytes.size() || size > bytes.size() - offset || rva > UINT32_MAX - base)
                throw std::runtime_error("Original PE section outside file");
            sections.push_back({base + rva, size, offset});
        }
        bytes_.assign(bytes.begin(), bytes.end());
        sections_ = std::move(sections);
        ++generation_;
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes(std::uint32_t address, std::size_t length) const {
        if (!generation_) throw std::runtime_error("Select original game files before reading game tables");
        for (const auto &section : sections_) {
            if (address < section.address) continue;
            const std::size_t delta = address - section.address;
            if (delta <= section.size && length <= section.size - delta)
                return std::span(bytes_).subspan(section.offset + delta, length);
        }
        throw std::runtime_error("Original game table outside file-backed PE sections");
    }

    template<class T> [[nodiscard]] T read(std::uint32_t address) const {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        static_assert(std::endian::native == std::endian::little,
                      "Object table reads need a little-endian target");
        T result;
        const auto source = bytes(address, sizeof(T));
        std::memcpy(&result, source.data(), sizeof(T));
        return result;
    }

    [[nodiscard]] std::string_view string(std::uint32_t address, std::size_t limit = 256) const {
        for (std::size_t length = 0; length < limit; ++length) {
            if (length > UINT32_MAX - address) break;
            if (!read<std::uint8_t>(address + static_cast<std::uint32_t>(length))) {
                const auto text = bytes(address, length + 1);
                return {reinterpret_cast<const char *>(text.data()), length};
            }
        }
        throw std::runtime_error("Unterminated original table string");
    }

    [[nodiscard]] std::uint64_t generation() const { return generation_; }

private:
    struct Section { std::uint32_t address, size, offset; };
    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
    std::uint64_t generation_{};
};

// Deferred decoding keeps original values out of static initialization and
// rebuilds caches if a different game-data source is installed by a test.
template<class T, std::size_t N, class Decode> class OriginalArray {
public:
    constexpr explicit OriginalArray(Decode decode) : decode_(decode) {}
    [[nodiscard]] const std::array<T, N> &get() const {
        const auto &source = OriginalData::instance();
        if (!source.generation()) throw std::runtime_error("Original game data is not loaded");
        if (!values_ || generation_ != source.generation()) {
            std::array<T, N> values;
            for (std::size_t i = 0; i < N; ++i) values[i] = decode_(source, i);
            values_ = std::move(values);
            generation_ = source.generation();
        }
        return *values_;
    }
    [[nodiscard]] const T &operator[](std::size_t index) const { return get().at(index); }
    [[nodiscard]] const T &at(std::size_t index) const { return get().at(index); }
    [[nodiscard]] const T &front() const { return get().front(); }
    [[nodiscard]] const T &back() const { return get().back(); }
    [[nodiscard]] const T *data() const { return get().data(); }
    [[nodiscard]] auto begin() const { return get().begin(); }
    [[nodiscard]] auto end() const { return get().end(); }
    [[nodiscard]] constexpr std::size_t size() const { return N; }
    [[nodiscard]] constexpr bool empty() const { return N == 0; }
    operator const std::array<T, N> &() const { return get(); }
private:
    Decode decode_;
    mutable std::optional<std::array<T, N>> values_;
    mutable std::uint64_t generation_{};
};

template<class T, std::size_t N, class Decode>
constexpr auto originalArray(Decode decode) { return OriginalArray<T, N, Decode>(decode); }

template<class T, std::size_t N>
constexpr auto originalArray(std::uint32_t address, std::size_t stride = sizeof(T)) {
    return originalArray<T, N>([address, stride](const OriginalData &source, std::size_t index) {
        if (stride && index > (UINT32_MAX - address) / stride) throw std::runtime_error("Original table address overflow");
        return source.read<T>(address + static_cast<std::uint32_t>(index * stride));
    });
}

} // namespace fgp
