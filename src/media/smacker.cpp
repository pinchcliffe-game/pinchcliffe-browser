#include "media/smacker.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fgp {

std::string ffError(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
}

namespace {

struct MemoryInput {
    std::span<const std::uint8_t> bytes;
    std::int64_t position{};
};

int readMemory(void *opaque, std::uint8_t *buffer, int bufferSize) {
    auto &input = *static_cast<MemoryInput *>(opaque);
    const auto remaining = static_cast<std::int64_t>(input.bytes.size()) - input.position;
    if (remaining <= 0) {
        return AVERROR_EOF;
    }
    const auto amount = static_cast<int>(std::min<std::int64_t>(remaining, bufferSize));
    std::memcpy(buffer, input.bytes.data() + input.position, static_cast<std::size_t>(amount));
    input.position += amount;
    return amount;
}

std::int64_t seekMemory(void *opaque, std::int64_t offset, int whence) {
    auto &input = *static_cast<MemoryInput *>(opaque);
    if (whence == AVSEEK_SIZE) {
        return static_cast<std::int64_t>(input.bytes.size());
    }
    const int origin = whence & ~AVSEEK_FORCE;
    std::int64_t target = 0;
    if (origin == SEEK_SET) {
        target = offset;
    } else if (origin == SEEK_CUR) {
        target = input.position + offset;
    } else if (origin == SEEK_END) {
        target = static_cast<std::int64_t>(input.bytes.size()) + offset;
    } else {
        return AVERROR(EINVAL);
    }
    if (target < 0 || target > static_cast<std::int64_t>(input.bytes.size())) {
        return AVERROR(EINVAL);
    }
    input.position = target;
    return target;
}

struct CodecContextDeleter {
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};

struct PacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};

struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

CodecContextPtr openDecoder(const AVStream *stream) {
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        throw std::runtime_error("FFmpeg decoder is unavailable for codec " +
                                 std::to_string(stream->codecpar->codec_id));
    }
    CodecContextPtr context(avcodec_alloc_context3(decoder));
    if (!context) {
        throw std::runtime_error("avcodec_alloc_context3 failed");
    }
    int result = avcodec_parameters_to_context(context.get(), stream->codecpar);
    if (result < 0) {
        throw std::runtime_error("avcodec_parameters_to_context failed: " + ffError(result));
    }
    result = avcodec_open2(context.get(), decoder, nullptr);
    if (result < 0) {
        throw std::runtime_error("avcodec_open2 failed: " + ffError(result));
    }
    return context;
}

// SMACKW32 writes frames into the 16-bit surface as RGB565 (0x00402d60
// selects SMACKBUFFER565 when the display's green mask is 0x7e0), and the
// colour key is compared in that form. FFmpeg expands Smacker's 6-bit palette
// components to 8 bits, so dropping the low bits recovers the 5/6/5 values.
constexpr std::uint16_t rgb565(std::uint8_t red,
                               std::uint8_t green,
                               std::uint8_t blue) {
    return static_cast<std::uint16_t>(((red >> 3U) << 11U) |
                                      ((green >> 2U) << 5U) | (blue >> 3U));
}

struct FrameKeying {
    SmackerKeying keying{SmackerKeying::IndexZeroAndGreen};
    SmackerIndexZeroAlpha indexZeroAlpha{SmackerIndexZeroAlpha::Transparent};
    // Taken from frame 0, which PLAYER_Create (0x00403128) extracts into the
    // surface before scene_DrawAnim reads its key.
    std::optional<std::uint16_t> topLeftRgb565;
};

VideoFrame convertVideoFrame(AVFrame *frame,
                             const AVStream *stream,
                             SwsContext *&scaler,
                             double fallbackTime,
                             FrameKeying &keying) {
    VideoFrame output;
    output.width = frame->width;
    output.height = frame->height;
    const auto timestamp = frame->best_effort_timestamp;
    output.time = timestamp != AV_NOPTS_VALUE
                      ? timestamp * av_q2d(stream->time_base)
                      : fallbackTime;
    output.bgra.resize(static_cast<std::size_t>(output.width) *
                       static_cast<std::size_t>(output.height) * 4U);
    std::uint8_t *destination[] = {output.bgra.data(), nullptr, nullptr, nullptr};
    int destinationStride[] = {output.width * 4, 0, 0, 0};
    scaler = sws_getCachedContext(scaler,
                                  frame->width,
                                  frame->height,
                                  static_cast<AVPixelFormat>(frame->format),
                                  frame->width,
                                  frame->height,
                                  AV_PIX_FMT_BGRA,
                                  SWS_POINT,
                                  nullptr,
                                  nullptr,
                                  nullptr);
    if (!scaler) {
        throw std::runtime_error("sws_getCachedContext failed");
    }
    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
              destination, destinationStride);

    if (keying.keying == SmackerKeying::TopLeftRgb565) {
        if (!keying.topLeftRgb565 && !output.bgra.empty()) {
            keying.topLeftRgb565 = rgb565(
                output.bgra[2], output.bgra[1], output.bgra[0]);
        }
        const std::uint16_t key = keying.topLeftRgb565.value_or(0U);
        for (std::size_t pixel = 0; pixel < output.bgra.size(); pixel += 4U) {
            output.bgra[pixel + 3] =
                rgb565(output.bgra[pixel + 2], output.bgra[pixel + 1],
                       output.bgra[pixel]) == key
                    ? 0
                    : 255;
        }
        return output;
    }

    // Clips whose original blit path has not been traced use palette index 0
    // and chroma green as their key. Some full-frame animations use index 0
    // as an ordinary colour instead, so callers select that policy from
    // executable/capture evidence rather than losing real pixels
    // unconditionally.
    const bool paletted = frame->format == AV_PIX_FMT_PAL8 && frame->data[0];
    const bool transparentIndexZero =
        keying.indexZeroAlpha == SmackerIndexZeroAlpha::Transparent;
    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * output.width + x) * 4U;
            const auto blue = output.bgra[pixel];
            const auto green = output.bgra[pixel + 1];
            const auto red = output.bgra[pixel + 2];
            const bool chromaGreen = green > 235 && red < 20 && blue < 20;
            if (paletted) {
                const auto paletteIndex = frame->data[0][
                    static_cast<std::size_t>(y) * frame->linesize[0] + x];
                output.bgra[pixel + 3] =
                    (transparentIndexZero && paletteIndex == 0) || chromaGreen
                        ? 0
                        : 255;
            } else {
                output.bgra[pixel + 3] = chromaGreen ? 0 : 255;
            }
        }
    }
    return output;
}

bool mediaDiagnosticsEnabled() {
    const char *value = std::getenv("FGP_MEDIA_DIAGNOSTICS");
    return value && value[0] != '\0' && std::string_view(value) != "0";
}

std::uint32_t smackerFrameCount(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16U) {
        throw std::runtime_error("Smacker header is too short");
    }
    return static_cast<std::uint32_t>(bytes[12]) |
           (static_cast<std::uint32_t>(bytes[13]) << 8U) |
           (static_cast<std::uint32_t>(bytes[14]) << 16U) |
           (static_cast<std::uint32_t>(bytes[15]) << 24U);
}

class SmackerVideoProvider final : public VideoFrameProvider {
public:
    SmackerVideoProvider(std::span<const std::uint8_t> bytes,
                         std::string name,
                         SmackerKeying keying,
                         std::size_t windowFrames,
                         std::size_t readAheadFrames)
        : bytes_(bytes),
          name_(std::move(name)),
          keying_{keying},
          windowFrames_(std::max<std::size_t>(1U, windowFrames)),
          readAheadFrames_(std::min(readAheadFrames, windowFrames_ - 1U)),
          diagnosticsEnabled_(mediaDiagnosticsEnabled()) {
        frameCount_ = smackerFrameCount(bytes_);
        if (frameCount_ == 0U) {
            throw std::runtime_error("Smacker video has no frames: " + name_);
        }
        try {
            open();
        } catch (...) {
            close();
            throw;
        }
        if (diagnosticsEnabled_) {
            std::cerr << "video_window_open clip=" << std::quoted(name_)
                      << " frames=" << frameCount_
                      << " window=" << windowFrames_
                      << " read_ahead=" << readAheadFrames_ << '\n';
        }
    }

    ~SmackerVideoProvider() override {
        if (diagnosticsEnabled_) {
            std::cerr << "video_window_close clip=" << std::quoted(name_)
                      << " decoded_frames=" << decodedFrames_
                      << " window_refills=" << windowRefills_
                      << " decode_stalls=" << decodeStalls_
                      << " resets=" << decoderResets_
                      << " playhead_skips=" << playheadSkips_ << '\n';
        }
        close();
    }

    [[nodiscard]] std::size_t frameCount() const { return frameCount_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] double frameDuration() const { return frameDuration_; }
    [[nodiscard]] double videoDuration() const {
        return static_cast<double>(frameCount_) * frameDuration_;
    }

    void ensureFrame(std::vector<VideoFrame> &frames,
                     std::size_t index) override {
        if (index >= frames.size()) {
            throw std::out_of_range("Smacker frame index outside metadata");
        }
        recordRequest(index);
        if (!frames[index].bgra.empty()) {
            return;
        }
        if (index < nextFrame_) {
            restart(frames);
        }

        const auto started = std::chrono::steady_clock::now();
        const std::size_t requestedEnd = std::min(
            frames.size() - 1U, index + readAheadFrames_);
        decodeThrough(frames, requestedEnd);
        lastDecodeMilliseconds_ = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
        ++windowRefills_;
        constexpr double renderFrameBudgetMilliseconds = 1000.0 / 60.0;
        if (lastDecodeMilliseconds_ > renderFrameBudgetMilliseconds) {
            ++decodeStalls_;
        }
        if (frames[index].bgra.empty()) {
            throw std::runtime_error("Smacker decoder ended before frame " +
                                     std::to_string(index) + " of " + name_);
        }
        if (diagnosticsEnabled_) {
            const auto state = diagnostics(frames);
            std::cerr << "video_window clip=" << std::quoted(name_)
                      << " request=" << index
                      << " resident=" << state.firstResidentFrame << '-'
                      << state.lastResidentFrame
                      << " resident_frames=" << state.residentFrames
                      << " resident_bytes=" << state.residentBytes
                      << " window_refills=" << windowRefills_
                      << " decode_stalls=" << state.decodeStalls
                      << " resets=" << state.decoderResets
                      << " playhead_skips=" << state.playheadSkips
                      << " decode_ms=" << std::fixed << std::setprecision(3)
                      << state.lastDecodeMilliseconds << '\n';
        }
    }

    [[nodiscard]] VideoWindowDiagnostics diagnostics(
        const std::vector<VideoFrame> &frames) const override {
        VideoWindowDiagnostics state;
        state.name = name_;
        state.capacityFrames = windowFrames_;
        state.readAheadFrames = readAheadFrames_;
        state.firstResidentFrame = frames.size();
        for (std::size_t index = 0; index < frames.size(); ++index) {
            if (frames[index].bgra.empty()) {
                continue;
            }
            state.firstResidentFrame =
                std::min(state.firstResidentFrame, index);
            state.lastResidentFrame = index;
            ++state.residentFrames;
            state.residentBytes += frames[index].bgra.size();
        }
        if (state.residentFrames == 0U) {
            state.firstResidentFrame = 0U;
            state.lastResidentFrame = 0U;
        }
        state.decodedFrames = decodedFrames_;
        state.decodeStalls = decodeStalls_;
        state.decoderResets = decoderResets_;
        state.playheadSkips = playheadSkips_;
        state.lastDecodeMilliseconds = lastDecodeMilliseconds_;
        return state;
    }

private:
    void open() {
        memory_ = {bytes_, 0};
        constexpr int ioBufferSize = 32 * 1024;
        auto *ioBuffer = static_cast<std::uint8_t *>(av_malloc(ioBufferSize));
        if (!ioBuffer) {
            throw std::runtime_error("av_malloc failed for streaming input");
        }
        io_ = avio_alloc_context(ioBuffer, ioBufferSize, 0, &memory_,
                                 readMemory, nullptr, seekMemory);
        if (!io_) {
            av_free(ioBuffer);
            throw std::runtime_error("avio_alloc_context failed for streaming input");
        }
        format_ = avformat_alloc_context();
        if (!format_) {
            close();
            throw std::runtime_error("avformat_alloc_context failed");
        }
        format_->pb = io_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
        int result = avformat_open_input(&format_, nullptr, nullptr, nullptr);
        if (result < 0) {
            close();
            throw std::runtime_error("avformat_open_input failed: " + ffError(result));
        }
        result = avformat_find_stream_info(format_, nullptr);
        if (result < 0) {
            close();
            throw std::runtime_error("avformat_find_stream_info failed: " +
                                     ffError(result));
        }
        videoIndex_ =
            av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0) {
            close();
            throw std::runtime_error("Smacker asset has no video stream");
        }
        AVStream *stream = format_->streams[videoIndex_];
        videoContext_ = openDecoder(stream);
        width_ = videoContext_->width;
        height_ = videoContext_->height;
        const AVRational rate = av_guess_frame_rate(format_, stream, nullptr);
        frameDuration_ = rate.num > 0 && rate.den > 0
                             ? av_q2d(av_inv_q(rate))
                             : 1.0 / 15.0;
        packet_.reset(av_packet_alloc());
        frame_.reset(av_frame_alloc());
        if (!packet_ || !frame_) {
            close();
            throw std::runtime_error("FFmpeg packet/frame allocation failed");
        }
        nextFrame_ = 0U;
        eof_ = false;
    }

    void close() {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
        frame_.reset();
        packet_.reset();
        videoContext_.reset();
        if (format_) {
            avformat_close_input(&format_);
        }
        if (io_) {
            avio_context_free(&io_);
        }
        videoIndex_ = -1;
    }

    void restart(std::vector<VideoFrame> &frames) {
        for (auto &frame : frames) {
            frame.bgra.resize(0U);
        }
        close();
        open();
        ++decoderResets_;
    }

    void recordRequest(std::size_t index) {
        if (lastRequestedFrame_ != std::numeric_limits<std::size_t>::max() &&
            index > lastRequestedFrame_ + 1U) {
            playheadSkips_ += index - lastRequestedFrame_ - 1U;
        }
        lastRequestedFrame_ = index;
    }

    void storeDecodedFrame(std::vector<VideoFrame> &frames) {
        if (nextFrame_ >= frames.size()) {
            av_frame_unref(frame_.get());
            return;
        }
        VideoFrame output = convertVideoFrame(
            frame_.get(), format_->streams[videoIndex_], scaler_,
            static_cast<double>(nextFrame_) * frameDuration_, keying_);
        output.revision = ++frameRevision_;
        frames[nextFrame_] = std::move(output);
        ++nextFrame_;
        ++decodedFrames_;
        if (nextFrame_ > windowFrames_) {
            frames[nextFrame_ - windowFrames_ - 1U].bgra.resize(0U);
        }
        av_frame_unref(frame_.get());
    }

    void drainFrames(std::vector<VideoFrame> &frames) {
        while (true) {
            const int result =
                avcodec_receive_frame(videoContext_.get(), frame_.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            if (result < 0) {
                throw std::runtime_error("Video decode failed: " + ffError(result));
            }
            storeDecodedFrame(frames);
        }
    }

    void decodeThrough(std::vector<VideoFrame> &frames,
                       std::size_t requestedEnd) {
        while (nextFrame_ <= requestedEnd && !eof_) {
            int result = av_read_frame(format_, packet_.get());
            if (result == AVERROR_EOF) {
                eof_ = true;
                result = avcodec_send_packet(videoContext_.get(), nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    throw std::runtime_error("Video flush failed: " +
                                             ffError(result));
                }
                drainFrames(frames);
                break;
            }
            if (result < 0) {
                throw std::runtime_error("av_read_frame failed: " + ffError(result));
            }
            if (packet_->stream_index == videoIndex_) {
                result = avcodec_send_packet(videoContext_.get(), packet_.get());
                if (result < 0) {
                    av_packet_unref(packet_.get());
                    throw std::runtime_error(
                        "avcodec_send_packet(video) failed: " + ffError(result));
                }
                drainFrames(frames);
            }
            av_packet_unref(packet_.get());
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::string name_;
    FrameKeying keying_;
    std::size_t windowFrames_{};
    std::size_t readAheadFrames_{};
    bool diagnosticsEnabled_{};
    MemoryInput memory_{};
    AVIOContext *io_{};
    AVFormatContext *format_{};
    CodecContextPtr videoContext_;
    PacketPtr packet_;
    FramePtr frame_;
    SwsContext *scaler_{};
    int videoIndex_{-1};
    int width_{};
    int height_{};
    std::size_t frameCount_{};
    std::size_t nextFrame_{};
    bool eof_{};
    double frameDuration_{1.0 / 15.0};
    std::size_t decodedFrames_{};
    std::size_t windowRefills_{};
    std::size_t decodeStalls_{};
    std::size_t decoderResets_{};
    std::size_t playheadSkips_{};
    std::size_t lastRequestedFrame_{std::numeric_limits<std::size_t>::max()};
    std::uint64_t frameRevision_{};
    double lastDecodeMilliseconds_{};
};

} // namespace

// FFmpeg's Smacker demuxer adds one audio stream per present track, in
// track order, so the n-th audio stream is the n-th track flagged present.
static int smackerTrackForAudioOrdinal(std::span<const std::uint8_t> bytes,
                                       int ordinal) {
    if (bytes.size() < 0x48U + 7U * 4U) {
        return -1;
    }
    int seen = 0;
    for (std::size_t track = 0; track < 7U; ++track) {
        const std::size_t offset = 0x48U + track * 4U;
        if ((bytes[offset + 3U] & 0x40U) == 0U) {
            continue;
        }
        if (seen++ == ordinal) {
            return static_cast<int>(track);
        }
    }
    return -1;
}

// The header's seven audio-rate dwords at +0x48; bit 30 marks a track whose
// data is present.
static bool smackerHeaderHasAudio(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 0x48U + 7U * 4U) {
        return false;
    }
    for (std::size_t track = 0; track < 7U; ++track) {
        const std::size_t offset = 0x48U + track * 4U;
        const std::uint32_t flags =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
        if ((flags & 0x40000000U) != 0U) {
            return true;
        }
    }
    return false;
}

DecodedMedia decodeSmacker(std::span<const std::uint8_t> bytes,
                           bool decodeVideo,
                           bool decodeAudio,
                           bool allowMissingAudio,
                           int audioStreamOrdinal,
                           DecodedMedia *secondaryAudio,
                           SmackerIndexZeroAlpha indexZeroAlpha,
                           SmackerKeying frameKeying) {
    MemoryInput memory{bytes, 0};
    constexpr int ioBufferSize = 32 * 1024;
    auto *ioBuffer = static_cast<std::uint8_t *>(av_malloc(ioBufferSize));
    if (!ioBuffer) {
        throw std::runtime_error("av_malloc failed for input buffer");
    }
    AVIOContext *io = avio_alloc_context(ioBuffer, ioBufferSize, 0, &memory,
                                         readMemory, nullptr, seekMemory);
    if (!io) {
        av_free(ioBuffer);
        throw std::runtime_error("avio_alloc_context failed");
    }

    AVFormatContext *format = avformat_alloc_context();
    if (!format) {
        avio_context_free(&io);
        throw std::runtime_error("avformat_alloc_context failed");
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
    if (result < 0) {
        avformat_free_context(format);
        avio_context_free(&io);
        throw std::runtime_error("avformat_open_input failed: " + ffError(result));
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        avformat_close_input(&format);
        avio_context_free(&io);
        throw std::runtime_error("avformat_find_stream_info failed: " + ffError(result));
    }

    int videoIndex = decodeVideo
                         ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)
                         : -1;
    int audioIndex = -1;
    int secondaryAudioIndex = -1;
    if (decodeAudio) {
        int ordinal = 0;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            if (format->streams[index]->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO) {
                continue;
            }
            if (ordinal == audioStreamOrdinal) {
                audioIndex = static_cast<int>(index);
            }
            if (secondaryAudio && ordinal == 1) {
                secondaryAudioIndex = static_cast<int>(index);
            }
            ++ordinal;
        }
    }
    if (decodeVideo && videoIndex < 0) {
        avformat_close_input(&format);
        avio_context_free(&io);
        throw std::runtime_error("Smacker asset has no video stream");
    }
    if (decodeAudio && audioIndex < 0 && !allowMissingAudio) {
        avformat_close_input(&format);
        avio_context_free(&io);
        throw std::runtime_error("Smacker asset has no audio stream");
    }

    CodecContextPtr videoContext;
    CodecContextPtr audioContext;
    CodecContextPtr secondaryAudioContext;
    if (videoIndex >= 0) {
        videoContext = openDecoder(format->streams[videoIndex]);
    }
    if (audioIndex >= 0) {
        audioContext = openDecoder(format->streams[audioIndex]);
    }
    if (secondaryAudioIndex >= 0 && secondaryAudioIndex != audioIndex) {
        secondaryAudioContext =
            openDecoder(format->streams[secondaryAudioIndex]);
    }

    SwsContext *scaler = nullptr;
    SwrContext *resampler = nullptr;
    SwrContext *secondaryResampler = nullptr;
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    DecodedMedia media;

    const auto prepareResampler = [&](SwrContext **target,
                                      const CodecContextPtr &context,
                                      const DecodedMedia &output) {
        result = swr_alloc_set_opts2(target,
                                     &outputLayout,
                                     AV_SAMPLE_FMT_S16,
                                     output.audioRate,
                                     &context->ch_layout,
                                     context->sample_fmt,
                                     context->sample_rate,
                                     0,
                                     nullptr);
        if (result < 0 || !*target) {
            avformat_close_input(&format);
            avio_context_free(&io);
            throw std::runtime_error("swr_alloc_set_opts2 failed: " + ffError(result));
        }
        // The original hands a mono track to DirectSound as a mono buffer
        // at centre pan (player pan 0.5, 0x0040305f), which plays it at full
        // level on both speakers. swresample's default mono-to-stereo
        // matrix would put it 3 dB lower, so copy it unchanged instead.
        if (context->ch_layout.nb_channels == 1) {
            const std::array<double, 2> monoToStereo{1.0, 1.0};
            result = swr_set_matrix(*target, monoToStereo.data(), 1);
            if (result < 0) {
                swr_free(target);
                avformat_close_input(&format);
                avio_context_free(&io);
                throw std::runtime_error("swr_set_matrix failed: " +
                                         ffError(result));
            }
        }
        result = swr_init(*target);
        if (result < 0) {
            swr_free(target);
            avformat_close_input(&format);
            avio_context_free(&io);
            throw std::runtime_error("swr_init failed: " + ffError(result));
        }
    };
    if (audioContext) {
        prepareResampler(&resampler, audioContext, media);
    }
    if (secondaryAudioContext) {
        prepareResampler(&secondaryResampler, secondaryAudioContext,
                         *secondaryAudio);
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        swr_free(&resampler);
        swr_free(&secondaryResampler);
        avformat_close_input(&format);
        avio_context_free(&io);
        throw std::runtime_error("FFmpeg packet/frame allocation failed");
    }

    FrameKeying keying{frameKeying, indexZeroAlpha, {}};
    std::size_t fallbackVideoFrame = 0;
    double fallbackFrameDuration = 1.0 / 15.0;
    if (videoIndex >= 0) {
        const AVRational rate = av_guess_frame_rate(format, format->streams[videoIndex], nullptr);
        if (rate.num > 0 && rate.den > 0) {
            fallbackFrameDuration = av_q2d(av_inv_q(rate));
        }
    }

    auto drainVideo = [&] {
        while (true) {
            const int receive = avcodec_receive_frame(videoContext.get(), frame.get());
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
                break;
            }
            if (receive < 0) {
                throw std::runtime_error("Video decode failed: " + ffError(receive));
            }

            VideoFrame output = convertVideoFrame(
                frame.get(), format->streams[videoIndex], scaler,
                static_cast<double>(fallbackVideoFrame) * fallbackFrameDuration,
                keying);
            ++fallbackVideoFrame;
            media.videoFrames.push_back(std::move(output));
            av_frame_unref(frame.get());
        }
    };

    auto drainAudio = [&](const CodecContextPtr &context,
                          SwrContext *streamResampler,
                          DecodedMedia &output) {
        while (true) {
            const int receive = avcodec_receive_frame(context.get(), frame.get());
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
                break;
            }
            if (receive < 0) {
                throw std::runtime_error("Audio decode failed: " + ffError(receive));
            }

            const auto delayed =
                swr_get_delay(streamResampler, context->sample_rate);
            const int capacity = static_cast<int>(av_rescale_rnd(
                delayed + frame->nb_samples,
                output.audioRate,
                context->sample_rate,
                AV_ROUND_UP));
            const std::size_t oldSize = output.audioSamples.size();
            output.audioSamples.resize(oldSize +
                                       static_cast<std::size_t>(capacity) *
                                           static_cast<std::size_t>(output.audioChannels));
            std::uint8_t *destination[] = {
                reinterpret_cast<std::uint8_t *>(output.audioSamples.data() + oldSize),
                nullptr};
            const int converted = swr_convert(
                streamResampler,
                destination,
                capacity,
                const_cast<const std::uint8_t **>(frame->extended_data),
                frame->nb_samples);
            if (converted < 0) {
                throw std::runtime_error("swr_convert failed: " + ffError(converted));
            }
            output.audioSamples.resize(oldSize +
                                       static_cast<std::size_t>(converted) *
                                           static_cast<std::size_t>(output.audioChannels));
            av_frame_unref(frame.get());
        }
    };

    try {
        while ((result = av_read_frame(format, packet.get())) >= 0) {
            if (packet->stream_index == videoIndex) {
                result = avcodec_send_packet(videoContext.get(), packet.get());
                if (result < 0) {
                    throw std::runtime_error("avcodec_send_packet(video) failed: " +
                                             ffError(result));
                }
                drainVideo();
            } else if (packet->stream_index == audioIndex) {
                result = avcodec_send_packet(audioContext.get(), packet.get());
                if (result < 0) {
                    throw std::runtime_error("avcodec_send_packet(audio) failed: " +
                                             ffError(result));
                }
                drainAudio(audioContext, resampler, media);
            } else if (packet->stream_index == secondaryAudioIndex) {
                result = avcodec_send_packet(secondaryAudioContext.get(),
                                             packet.get());
                if (result < 0) {
                    throw std::runtime_error(
                        "avcodec_send_packet(secondary audio) failed: " +
                        ffError(result));
                }
                drainAudio(secondaryAudioContext, secondaryResampler,
                           *secondaryAudio);
            }
            av_packet_unref(packet.get());
        }
        if (result != AVERROR_EOF) {
            throw std::runtime_error("av_read_frame failed: " + ffError(result));
        }
        if (videoContext) {
            avcodec_send_packet(videoContext.get(), nullptr);
            drainVideo();
        }
        if (audioContext) {
            avcodec_send_packet(audioContext.get(), nullptr);
            drainAudio(audioContext, resampler, media);
        }
        if (secondaryAudioContext) {
            avcodec_send_packet(secondaryAudioContext.get(), nullptr);
            drainAudio(secondaryAudioContext, secondaryResampler,
                       *secondaryAudio);
        }
    } catch (...) {
        sws_freeContext(scaler);
        swr_free(&resampler);
        swr_free(&secondaryResampler);
        avformat_close_input(&format);
        avio_context_free(&io);
        throw;
    }

    if (!media.videoFrames.empty()) {
        media.videoDuration = media.videoFrames.back().time + fallbackFrameDuration;
    }

    sws_freeContext(scaler);
    swr_free(&resampler);
    swr_free(&secondaryResampler);
    avformat_close_input(&format);
    avio_context_free(&io);
    if (audioIndex >= 0) {
        media.smackerAudioTrack =
            smackerTrackForAudioOrdinal(bytes, audioStreamOrdinal);
    }
    if (secondaryAudio && secondaryAudioIndex >= 0) {
        secondaryAudio->smackerAudioTrack =
            smackerTrackForAudioOrdinal(bytes, 1);
    }
    media.smackerAudio = smackerHeaderHasAudio(bytes);
    media.smackerInterlaced = bytes.size() >= 0x18U && (bytes[0x14] & 0x02U) != 0U;
    return media;
}

DecodedMedia decodeSmackerStreamed(
    std::span<const std::uint8_t> bytes,
    std::string diagnosticName,
    SmackerKeying keying,
    bool decodeAudio,
    bool allowMissingAudio,
    int audioStreamOrdinal,
    DecodedMedia *secondaryAudio,
    std::size_t windowFrames,
    std::size_t readAheadFrames) {
    DecodedMedia media;
    if (decodeAudio) {
        media = decodeSmacker(bytes, false, true, allowMissingAudio,
                              audioStreamOrdinal, secondaryAudio);
    }

    auto provider = std::make_unique<SmackerVideoProvider>(
        bytes, std::move(diagnosticName), keying, windowFrames,
        readAheadFrames);
    media.videoDuration = provider->videoDuration();
    media.videoFrames.resize(provider->frameCount());
    std::size_t index = 0U;
    for (auto &frame : media.videoFrames) {
        frame.time = static_cast<double>(index) * provider->frameDuration();
        frame.width = provider->width();
        frame.height = provider->height();
        ++index;
    }
    media.videoFrames.setProvider(std::move(provider));
    media.smackerAudio = smackerHeaderHasAudio(bytes);
    media.smackerInterlaced = bytes.size() >= 0x18U && (bytes[0x14] & 0x02U) != 0U;
    return media;
}

} // namespace fgp
