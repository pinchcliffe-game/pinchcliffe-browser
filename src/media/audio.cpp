#include "media/audio.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fgp {

namespace {

void writeLittleEndian(std::FILE *file, std::uint32_t value, int bytes) {
    for (int index = 0; index < bytes; ++index) {
        std::fputc(static_cast<int>((value >> (8 * index)) & 0xffU), file);
    }
}

void writeWavHeader(std::FILE *file, int rate, int channels,
                    std::uint32_t dataBytes) {
    std::fputs("RIFF", file);
    writeLittleEndian(file, 36U + dataBytes, 4);
    std::fputs("WAVEfmt ", file);
    writeLittleEndian(file, 16U, 4);
    writeLittleEndian(file, 1U, 2);
    writeLittleEndian(file, static_cast<std::uint32_t>(channels), 2);
    writeLittleEndian(file, static_cast<std::uint32_t>(rate), 4);
    writeLittleEndian(file, static_cast<std::uint32_t>(rate * channels * 2), 4);
    writeLittleEndian(file, static_cast<std::uint32_t>(channels * 2), 2);
    writeLittleEndian(file, 16U, 2);
    std::fputs("data", file);
    writeLittleEndian(file, dataBytes, 4);
}

} // namespace

LiveAudioMixer::~LiveAudioMixer() {
    if (dump_ == nullptr) {
        return;
    }
    if (device_ != 0) {
        SDL_PauseAudioDevice(device_, 1);
    }
    lock();
    std::FILE *file = dump_;
    dump_ = nullptr;
    const auto dataBytes =
        static_cast<std::uint32_t>(dumpedSamples_ * sizeof(std::int16_t));
    unlock();
    std::fseek(file, 0, SEEK_SET);
    writeWavHeader(file, rate_, channels_, dataBytes);
    std::fclose(file);
}

bool LiveAudioMixer::startDump(const std::string &path) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    writeWavHeader(file, rate_, channels_, 0U);
    lock();
    dump_ = file;
    dumpedSamples_ = 0U;
    unlock();
    return true;
}

std::uint64_t LiveAudioMixer::dumpedFrames() {
    lock();
    const std::uint64_t frames =
        channels_ > 0 ? dumpedSamples_ / static_cast<std::uint64_t>(channels_)
                      : 0U;
    unlock();
    return frames;
}

void LiveAudioMixer::callback(void *userdata, Uint8 *stream, int length) {
    static_cast<LiveAudioMixer *>(userdata)->render(stream, length);
}

float LiveAudioMixer::smackerDirectSoundGain(float volume) {
    if (!(volume > 0.0F)) {
        return 0.0F;
    }
    const auto smackVolume = static_cast<std::uint32_t>(
        std::min(static_cast<double>(volume) * 32768.0, 32768.0));
    return static_cast<float>((smackVolume * 127U) >> 15U) / 127.0F;
}

void LiveAudioMixer::attach(SDL_AudioDeviceID device, int rate, int channels) {
    device_ = device;
    rate_ = rate;
    channels_ = channels;
}

void LiveAudioMixer::setBackground(const DecodedMedia &media, float volume,
                                   double offsetSeconds) {
    Layer next = makeLayer(media, volume, true, offsetSeconds);
    lock();
    backgroundVolume_ = volume;
    backgroundHeld_ = false;
    background_ = std::move(next);
    background_.volume = smackerDirectSoundGain(volume * musicDuck_);
    unlock();
}

void LiveAudioMixer::setBackgroundVolume(float volume) {
    lock();
    backgroundVolume_ = volume;
    background_.volume = smackerDirectSoundGain(volume * musicDuck_);
    unlock();
}

void LiveAudioMixer::holdBackground() {
    lock();
    backgroundHeld_ = true;
    unlock();
}

void LiveAudioMixer::releaseBackgroundHold() {
    lock();
    backgroundHeld_ = false;
    unlock();
}

void LiveAudioMixer::setMusicDuck(float duck) {
    lock();
    musicDuck_ = duck;
    background_.volume = smackerDirectSoundGain(backgroundVolume_ * duck);
    unlock();
}

void LiveAudioMixer::setForeground(const DecodedMedia &speech,
                                   float speechVolume,
                                   const DecodedMedia *effects,
                                   float effectsVolume) {
    Layer nextSpeech = makeLayer(speech, speechVolume, false, 0.0);
    Layer nextEffects = effects
                            ? makeLayer(*effects, effectsVolume, false, 0.0)
                            : Layer{};
    lock();
    speech_ = std::move(nextSpeech);
    effects_ = std::move(nextEffects);
    addedEffects_.clear();
    foregroundPosition_ = 0U;
    foregroundHoldPosition_.reset();
    unlock();
}

void LiveAudioMixer::setEffect(const DecodedMedia &effects,
                               float effectsVolume) {
    lock();
    effects_ = makeLayer(effects, effectsVolume, false, 0.0);
    unlock();
}

LiveAudioMixer::EffectHandle LiveAudioMixer::addEffect(
    const DecodedMedia &effects, float effectsVolume, bool loop) {
    Layer next = makeLayer(effects, effectsVolume, loop, 0.0);
    lock();
    const EffectHandle handle = nextEffectHandle_++;
    addedEffects_.push_back(AddedEffect{handle, std::move(next)});
    unlock();
    return handle;
}

void LiveAudioMixer::setEffectVolume(EffectHandle handle,
                                     float effectsVolume) {
    lock();
    for (auto &effect : addedEffects_) {
        if (effect.handle == handle) {
            effect.layer.volume = smackerDirectSoundGain(effectsVolume);
        }
    }
    unlock();
}

void LiveAudioMixer::removeEffect(EffectHandle handle) {
    if (handle == kInvalidEffectHandle) {
        return;
    }
    lock();
    addedEffects_.erase(std::remove_if(
        addedEffects_.begin(), addedEffects_.end(),
        [handle](const AddedEffect &effect) {
            return effect.handle == handle;
        }), addedEffects_.end());
    unlock();
}

bool LiveAudioMixer::effectPlaying(EffectHandle handle) {
    if (handle == kInvalidEffectHandle) {
        return false;
    }
    lock();
    const bool playing = std::any_of(
        addedEffects_.begin(), addedEffects_.end(),
        [handle](const AddedEffect &effect) {
            return effect.handle == handle &&
                   (effect.layer.loop ||
                    effect.layer.position < effect.layer.samples.size());
        });
    unlock();
    return playing;
}

void LiveAudioMixer::seekForeground(double offsetSeconds) {
    const auto offsetSamples = static_cast<std::size_t>(std::llround(
        std::max(0.0, offsetSeconds) * rate_ * channels_));
    lock();
    speech_.position = std::min(offsetSamples, speech_.samples.size());
    effects_.position = std::min(offsetSamples, effects_.samples.size());
    foregroundPosition_ = offsetSamples;
    foregroundHoldPosition_.reset();
    unlock();
}

void LiveAudioMixer::holdForegroundAt(double offsetSeconds) {
    const auto offsetSamples = static_cast<std::size_t>(std::llround(
        std::max(0.0, offsetSeconds) * rate_ * channels_));
    lock();
    foregroundHoldPosition_ = offsetSamples;
    unlock();
}

void LiveAudioMixer::clearForegroundHold() {
    lock();
    foregroundHoldPosition_.reset();
    unlock();
}

void LiveAudioMixer::clearForeground() {
    lock();
    speech_ = {};
    effects_ = {};
    addedEffects_.clear();
    foregroundPosition_ = 0U;
    foregroundHoldPosition_.reset();
    unlock();
}

double LiveAudioMixer::backgroundSeconds() {
    lock();
    const double seconds = rate_ > 0 && channels_ > 0
                               ? background_.position /
                                     static_cast<double>(rate_ * channels_)
                               : 0.0;
    unlock();
    return seconds;
}

LiveAudioMixer::Layer LiveAudioMixer::makeLayer(const DecodedMedia &media,
                                                float volume, bool loop,
                                                double offsetSeconds) const {
    if (!media.audioSamples.empty() &&
        (media.audioRate != rate_ || media.audioChannels != channels_)) {
        throw std::runtime_error("Cannot mix audio with a different format");
    }
    Layer layer{media.audioSamples, 0U, smackerDirectSoundGain(volume),
                loop};
    if (!layer.samples.empty()) {
        const auto offsetFrames = static_cast<std::size_t>(std::llround(
            std::max(0.0, offsetSeconds) * rate_));
        layer.position =
            (offsetFrames * static_cast<std::size_t>(channels_)) %
            layer.samples.size();
    }
    return layer;
}

double LiveAudioMixer::nextSample(Layer &layer) {
    if (layer.samples.empty()) {
        return 0.0;
    }
    if (layer.position >= layer.samples.size()) {
        if (!layer.loop) {
            return 0.0;
        }
        layer.position %= layer.samples.size();
    }
    return layer.samples[layer.position++] * layer.volume;
}

void LiveAudioMixer::setNarration(const DecodedMedia &narration,
                                  float volume) {
    lock();
    narration_ = makeLayer(narration, volume, false, 0.0);
    unlock();
}

void LiveAudioMixer::clearNarration() {
    lock();
    narration_ = Layer{};
    unlock();
}

void LiveAudioMixer::setAmbience(const DecodedMedia &ambience,
                                 float volume) {
    Layer next = makeLayer(ambience, volume, false, 0.0);
    lock();
    ambience_ = std::move(next);
    unlock();
}

void LiveAudioMixer::clearAmbience() {
    lock();
    ambience_ = Layer{};
    unlock();
}

void LiveAudioMixer::render(Uint8 *stream, int length) {
    auto *output = reinterpret_cast<std::int16_t *>(stream);
    const std::size_t sampleCount =
        static_cast<std::size_t>(length) / sizeof(std::int16_t);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        double foreground = 0.0;
        if (!foregroundHoldPosition_ ||
            foregroundPosition_ < *foregroundHoldPosition_) {
            foreground = nextSample(speech_) + nextSample(effects_);
            ++foregroundPosition_;
        }
        for (auto &effect : addedEffects_) {
            foreground += nextSample(effect.layer);
        }
        const double background =
            backgroundHeld_ ? 0.0 : nextSample(background_);
        const long mixed = static_cast<long>(std::lround(
            background + foreground + nextSample(narration_) +
            nextSample(ambience_)));
        output[index] = static_cast<std::int16_t>(std::clamp(
            mixed,
            static_cast<long>(std::numeric_limits<std::int16_t>::min()),
            static_cast<long>(std::numeric_limits<std::int16_t>::max())));
    }
    if (dump_ != nullptr) {
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const auto value = static_cast<std::uint16_t>(output[index]);
            std::fputc(value & 0xffU, dump_);
            std::fputc(value >> 8U, dump_);
        }
        dumpedSamples_ += sampleCount;
    }
    addedEffects_.erase(std::remove_if(
        addedEffects_.begin(), addedEffects_.end(),
        [](const AddedEffect &effect) {
            return effect.layer.samples.empty() ||
                   (!effect.layer.loop &&
                    effect.layer.position >= effect.layer.samples.size());
        }), addedEffects_.end());
}

void LiveAudioMixer::lock() {
    if (device_ != 0) {
        SDL_LockAudioDevice(device_);
    }
}

void LiveAudioMixer::unlock() {
    if (device_ != 0) {
        SDL_UnlockAudioDevice(device_);
    }
}

} // namespace fgp
