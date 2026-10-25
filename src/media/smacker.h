#pragma once

// In-process Smacker decoding through the linked FFmpeg libraries.

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>

#include "media/media.h"

namespace fgp {

std::string ffError(int error);

enum class SmackerIndexZeroAlpha {
    Transparent,
    Opaque,
};

// Which decoded pixels become transparent.
enum class SmackerKeying {
    // Palette index 0 and chroma green. Clips whose original blit path has
    // not been traced keep this native policy.
    IndexZeroAndGreen,
    // The original scene_DrawAnim rule for descriptors with +0x48 set
    // (0x00413071-0x004130a2): a DirectDraw source colour key equal to the
    // RGB565 value of the clip's top-left pixel. Native reads that pixel from
    // frame 0 and keys every pixel whose RGB565 value matches it; palette
    // index 0 is an ordinary colour.
    TopLeftRgb565,
};

DecodedMedia decodeSmacker(std::span<const std::uint8_t> bytes,
                           bool decodeVideo,
                           bool decodeAudio,
                           bool allowMissingAudio = false,
                           int audioStreamOrdinal = 0,
                           DecodedMedia *secondaryAudio = nullptr,
                           SmackerIndexZeroAlpha indexZeroAlpha =
                               SmackerIndexZeroAlpha::Transparent,
                           SmackerKeying keying =
                               SmackerKeying::IndexZeroAndGreen);

// Keep the Smacker video decoder open and retain only a bounded BGRA frame
// window. Audio remains eagerly decoded because the mixer consumes a complete
// PCM timeline. The source bytes must outlive the returned media.
DecodedMedia decodeSmackerStreamed(
    std::span<const std::uint8_t> bytes,
    std::string diagnosticName,
    SmackerKeying keying,
    bool decodeAudio,
    bool allowMissingAudio = false,
    int audioStreamOrdinal = 0,
    DecodedMedia *secondaryAudio = nullptr,
    std::size_t windowFrames = 12,
    std::size_t readAheadFrames = 4);

} // namespace fgp
