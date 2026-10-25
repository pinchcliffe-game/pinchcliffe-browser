#pragma once

// Reader for a scene's packaged `fact.ini`, the data behind the original
// Factopedia pages (`FACT_Init` at 0x0041dbf0).
//
// The schema is order-significant: the section name is the localized page
// title, `hotspot=` binds the page to a scene hotspot, and `text0..textN`
// are its paragraphs. The executable scans sections in declaration order
// (0x0041dd40) and uses that ordinal as the bit index of the per-scene
// viewed mask, so sections must not be reordered or deduplicated.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fgp {

class FactTable {
public:
    struct Entry {
        std::string title;
        std::uint32_t hotspot{};
        std::vector<std::string> paragraphs;
        std::size_t ordinal{};
    };

    FactTable() = default;
    explicit FactTable(std::span<const std::uint8_t> bytes);

    [[nodiscard]] const std::vector<Entry> &entries() const { return entries_; }
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    // First entry bound to this hotspot, in declaration order.
    [[nodiscard]] const Entry *findByHotspot(std::uint32_t hotspot) const;

    // The paragraphs joined the way 0x0041dde0 builds the page body.
    [[nodiscard]] static std::string body(const Entry &entry);

    // Mask with one bit per declared section, for comparing against the
    // profile's viewed-fact record.
    [[nodiscard]] std::uint32_t completeMask() const;

private:
    std::vector<Entry> entries_;
};

} // namespace fgp
