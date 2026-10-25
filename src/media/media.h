#pragma once

// Decoded frames and interleaved 16-bit stereo audio shared by the players.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fgp {

// std::vector::resize value-initializes every byte before FFmpeg immediately
// overwrites the complete BGRA destination. For long Smacker sequences that
// unnecessary clearing dominated Debug scene-load profiles. This move-only
// buffer preserves the small vector-like API used by hit testing/rendering,
// but allocates uninitialized storage for the decoder to fill exactly once.
class PixelBuffer {
public:
    PixelBuffer() = default;
    PixelBuffer(PixelBuffer &&) noexcept = default;
    PixelBuffer &operator=(PixelBuffer &&) noexcept = default;
    PixelBuffer(const PixelBuffer &) = delete;
    PixelBuffer &operator=(const PixelBuffer &) = delete;

    void resize(std::size_t size) {
        bytes_.reset(size == 0 ? nullptr : new std::uint8_t[size]);
        size_ = size;
    }
    [[nodiscard]] std::uint8_t *data() { return bytes_.get(); }
    [[nodiscard]] const std::uint8_t *data() const { return bytes_.get(); }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    std::uint8_t &operator[](std::size_t index) { return bytes_[index]; }
    const std::uint8_t &operator[](std::size_t index) const {
        return bytes_[index];
    }

private:
    std::unique_ptr<std::uint8_t[]> bytes_;
    std::size_t size_{};
};

struct VideoFrame {
    double time{};
    int width{};
    int height{};
    std::uint64_t revision{};
    PixelBuffer bgra;
};

struct VideoWindowDiagnostics {
    std::string name;
    std::size_t capacityFrames{};
    std::size_t readAheadFrames{};
    std::size_t firstResidentFrame{};
    std::size_t lastResidentFrame{};
    std::size_t residentFrames{};
    std::size_t residentBytes{};
    std::size_t decodedFrames{};
    std::size_t decodeStalls{};
    std::size_t decoderResets{};
    std::size_t playheadSkips{};
    double lastDecodeMilliseconds{};
};

// A provider owns the live decoder behind a bounded frame collection. The
// public collection deliberately keeps the vector-like surface used by the
// game while allowing operator[] to materialise just the requested window.
class VideoFrameProvider {
public:
    virtual ~VideoFrameProvider() = default;
    virtual void ensureFrame(std::vector<VideoFrame> &frames,
                             std::size_t index) = 0;
    [[nodiscard]] virtual VideoWindowDiagnostics diagnostics(
        const std::vector<VideoFrame> &frames) const = 0;
};

class VideoFrames {
public:
    using iterator = std::vector<VideoFrame>::iterator;
    using const_iterator = std::vector<VideoFrame>::const_iterator;

    VideoFrames() = default;
    VideoFrames(VideoFrames &&) noexcept = default;
    VideoFrames &operator=(VideoFrames &&) noexcept = default;
    VideoFrames(const VideoFrames &) = delete;
    VideoFrames &operator=(const VideoFrames &) = delete;

    [[nodiscard]] bool empty() const { return frames_.empty(); }
    [[nodiscard]] std::size_t size() const { return frames_.size(); }
    void resize(std::size_t size) { frames_.resize(size); }
    void push_back(VideoFrame frame) { frames_.push_back(std::move(frame)); }

    VideoFrame &operator[](std::size_t index) {
        if (provider_) {
            provider_->ensureFrame(frames_, index);
        }
        return frames_[index];
    }
    const VideoFrame &operator[](std::size_t index) const {
        return const_cast<VideoFrames *>(this)->operator[](index);
    }
    VideoFrame &front() { return operator[](0); }
    const VideoFrame &front() const { return operator[](0); }
    VideoFrame &back() { return operator[](frames_.size() - 1U); }
    const VideoFrame &back() const { return operator[](frames_.size() - 1U); }
    [[nodiscard]] const VideoFrame &metadata(std::size_t index) const {
        return frames_[index];
    }

    // Iteration is used for timestamp searches and eager post-processing. It
    // does not materialise a streamed frame; pixel consumers use operator[].
    iterator begin() { return frames_.begin(); }
    iterator end() { return frames_.end(); }
    const_iterator begin() const { return frames_.begin(); }
    const_iterator end() const { return frames_.end(); }

    void setProvider(std::unique_ptr<VideoFrameProvider> provider) {
        provider_ = std::move(provider);
    }
    [[nodiscard]] bool isStreamed() const { return provider_ != nullptr; }
    [[nodiscard]] std::optional<VideoWindowDiagnostics> diagnostics() const {
        if (!provider_) {
            return std::nullopt;
        }
        return provider_->diagnostics(frames_);
    }

private:
    std::vector<VideoFrame> frames_;
    std::unique_ptr<VideoFrameProvider> provider_;
};

struct DecodedMedia {
    VideoFrames videoFrames;
    std::vector<std::int16_t> audioSamples;
    int audioRate{48000};
    int audioChannels{2};
    double videoDuration{};
    // The Smacker header marks an audio track present (+0x48 rate flags).
    bool smackerAudio{};
    // The Smacker track (0-6) the decoded audio came from, or -1. The
    // original's players give track 0 the effect volume and track 1 the
    // speech volume (PLAYER_Create, 0x00403068 and 0x00403070).
    int smackerAudioTrack{-1};
    // Smacker header flag 2 (+0x14): Y-interlaced. Each decoded line is
    // shown on every other row of a frame twice as tall; the rows between
    // stay black (Town's reel1 in the town-reel-skip capture).
    bool smackerInterlaced{};
};

// The original non-looping Smacker player marks its backing player done when
// the last frame is decoded, then the scene observes that flag on the next
// update. This is earlier than FFmpeg's end-of-final-frame duration.
double originalSmackerCompletionDuration(const DecodedMedia &media);

// When the original reports a story-scene clip finished, measured from its
// first frame. See "Story clip timing" in REVERSE_ENGINEERING.md.
double originalStoryClipCompletion(const DecodedMedia &media);

void mixAudioIntoAt(DecodedMedia &destination,
                    DecodedMedia source,
                    double offsetSeconds);

inline void mixAudioInto(DecodedMedia &destination, DecodedMedia source) {
    mixAudioIntoAt(destination, std::move(source), 0.0);
}

std::size_t frameIndexAt(const DecodedMedia &media, double time,
                         bool loop = true);

} // namespace fgp
