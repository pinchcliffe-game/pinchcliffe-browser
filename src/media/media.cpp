#include "media/media.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fgp {

double originalSmackerCompletionDuration(const DecodedMedia &media) {
    if (media.videoFrames.empty()) {
        return media.videoDuration;
    }

    // FGP's player update at 0x004031F2-0x00403218 sets its done flag when
    // FrameNum reaches Frames-1 and skips SmackNextFrame. The descriptor loop
    // at 0x00413200 observes the flag on the following pass. Original traces
    // sample those two passes at 15-16 ms each, so the observable completion
    // boundary is the last frame timestamp plus two 64 Hz updates, capped at
    // FFmpeg's end-of-final-frame duration.
    constexpr double originalUpdatePassSeconds = 1.0 / 64.0;
    return std::min(
        media.videoDuration,
        media.videoFrames.metadata(media.videoFrames.size() - 1U).time +
            2.0 * originalUpdatePassSeconds);
}

double originalStoryClipCompletion(const DecodedMedia &media) {
    if (media.videoFrames.empty()) {
        return media.videoDuration;
    }
    const std::size_t frames = media.videoFrames.size();
    const double lastFrameAt = media.videoFrames.metadata(frames - 1U).time;
    // Measured on the reference host (story-scenes captures, 2026-10-18;
    // "Story clip completion, measured" in REVERSE_ENGINEERING.md). From a
    // clip's SCENE_AddAnim to its AnimationFinished: a clip with an audio
    // track keeps its nominal frame clock and finishes about 52 ms after its
    // last frame is due (16 clips); a silent clip loses about 1.10 ms on
    // every frame interval and finishes about 7 ms after its last frame
    // (Intro SS-001a, SS-001c, RAN-B01 and Morning's birdbreath). The next
    // clip of the same sequence or character group is added about 12 ms
    // after that (11 hand-overs). The span returned here includes the
    // hand-over, so a clip's successor starts on time and its own stop is
    // reported about 12 ms late. UNCERTAIN: why silent clips drift.
    constexpr double kHandover = 0.012;
    if (media.smackerAudio) {
        return lastFrameAt + 0.052 + kHandover;
    }
    return lastFrameAt + static_cast<double>(frames - 1U) * 0.00110 + 0.007 +
           kHandover;
}

void mixAudioIntoAt(DecodedMedia &destination,
                    DecodedMedia source,
                    double offsetSeconds) {
    if (source.audioSamples.empty()) {
        return;
    }
    if (destination.audioSamples.empty()) {
        destination.audioRate = source.audioRate;
        destination.audioChannels = source.audioChannels;
    }
    if (destination.audioRate != source.audioRate ||
        destination.audioChannels != source.audioChannels) {
        throw std::runtime_error("Cannot mix decoded audio with different formats");
    }
    const auto offsetFrames = static_cast<std::size_t>(std::llround(
        std::max(0.0, offsetSeconds) * destination.audioRate));
    const std::size_t offsetSamples =
        offsetFrames * static_cast<std::size_t>(destination.audioChannels);
    destination.audioSamples.resize(
        std::max(destination.audioSamples.size(),
                 offsetSamples + source.audioSamples.size()),
        0);
    for (std::size_t index = 0; index < source.audioSamples.size(); ++index) {
        const std::size_t destinationIndex = offsetSamples + index;
        const int mixed = static_cast<int>(destination.audioSamples[destinationIndex]) +
                          static_cast<int>(source.audioSamples[index]);
        destination.audioSamples[destinationIndex] = static_cast<std::int16_t>(
            std::clamp(mixed,
                       static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                       static_cast<int>(std::numeric_limits<std::int16_t>::max())));
    }
}

std::size_t frameIndexAt(const DecodedMedia &media, double time,
                         bool loop) {
    if (media.videoFrames.empty()) {
        return 0;
    }
    if (media.videoDuration > 0.0) {
        if (loop) {
            time = std::fmod(std::max(0.0, time), media.videoDuration);
        } else {
            time = std::clamp(time, 0.0,
                              std::nextafter(media.videoDuration, 0.0));
        }
    }
    const auto next = std::upper_bound(
        media.videoFrames.begin(), media.videoFrames.end(), time,
        [](double value, const VideoFrame &frame) { return value < frame.time; });
    if (next == media.videoFrames.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(media.videoFrames.begin(), next - 1));
}

} // namespace fgp
