#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "media/media.h"

namespace fgp {

// Callback-driven three-bus mixer. Background music loops independently while
// speech and the primary effect can be replaced without resetting the music
// playhead; independent effect one-shots may overlap on the effects bus.
class LiveAudioMixer {
public:
    using EffectHandle = std::uint64_t;
    static constexpr EffectHandle kInvalidEffectHandle = 0;

    LiveAudioMixer() = default;
    LiveAudioMixer(const LiveAudioMixer &) = delete;
    LiveAudioMixer &operator=(const LiveAudioMixer &) = delete;
    ~LiveAudioMixer();

    static void callback(void *userdata, Uint8 *stream, int length);

    // The amplitude the original's Smacker DirectSound driver plays for a
    // volume factor. FGP.exe passes trunc(volume * 32768) to SmackVolumePan
    // (0x00403416-0x0040342b). SMACKW32.DLL's DirectSound driver (its own
    // image: VA 0x100098c0) clamps that to 0x8000, takes the index
    // (value * 127) >> 15 and sets the buffer to that entry of its centibel
    // table (VA 0x1001f8f8), which is 2000 * log10(index / 127). The audible
    // gain is therefore index / 127.
    // Every layer applies its volume through this curve.
    [[nodiscard]] static float smackerDirectSoundGain(float volume);

    void attach(SDL_AudioDeviceID device, int rate, int channels);
    void setBackground(const DecodedMedia &media, float volume,
                       double offsetSeconds = 0.0);
    void setBackgroundVolume(float volume);
    // The third factor of the music channel's volume: FGP.exe's set 2
    // (0x004a68dc, written through 0x0040b410), which story scene callbacks
    // use to duck the music under certain clips. The music player plays
    // the product of the slider, the scene's sound.ini and this factor
    // (0x0040b3b0), so a change applies to the running music at once and
    // to any music started later. It defaults to 1.0.
    void setMusicDuck(float duck);
    // Pauses the music where it is (the Loading box's 0x0040b6f0) until the
    // next setBackground or releaseBackgroundHold.
    void holdBackground();
    void releaseBackgroundHold();
    void setForeground(const DecodedMedia &speech, float speechVolume,
                       const DecodedMedia *effects = nullptr,
                       float effectsVolume = 0.0F);
    void setEffect(const DecodedMedia &effects, float effectsVolume);
    // Starts an independent one-shot on the effects bus without replacing
    // one-shots which are already playing.
    // A looping one-shot repeats until removed.
    EffectHandle addEffect(const DecodedMedia &effects, float effectsVolume,
                           bool loop = false);
    void setEffectVolume(EffectHandle handle, float effectsVolume);
    void removeEffect(EffectHandle handle);
    // Whether a one-shot started by addEffect is still sounding.
    [[nodiscard]] bool effectPlaying(EffectHandle handle);
    // Narration plays on its own layer because the original's
    // 0x00412b90 creates an independent player for every clip it starts:
    // a narration descriptor's conflict mask is 0, so it never contends
    // with the running sequence and is mixed alongside it. Putting it on
    // the speech layer would cut whatever stage audio is playing, which
    // is what forced narration to be pre-mixed onto a fixed timeline.
    void setNarration(const DecodedMedia &narration, float volume);
    void clearNarration();
    // The story scenes' ambience player (0x0040b0f0) runs beside the music
    // and is not cleared when a response sequence replaces the foreground.
    void setAmbience(const DecodedMedia &ambience, float volume);
    void clearAmbience();
    void seekForeground(double offsetSeconds);
    void holdForegroundAt(double offsetSeconds);
    void clearForegroundHold();
    void clearForeground();
    [[nodiscard]] double backgroundSeconds();
    // Development aid: writes everything the mixer renders from now on to a
    // 16-bit PCM WAV file, finished when the mixer is destroyed. A silent
    // headless run (SDL's dummy audio driver still pulls the callback in
    // real time) can then show what a listener would have heard.
    bool startDump(const std::string &path);
    // Frames rendered since startDump, to place trace events in the dump.
    [[nodiscard]] std::uint64_t dumpedFrames();

private:
    struct Layer {
        std::vector<std::int16_t> samples;
        std::size_t position{};
        float volume{};
        bool loop{};
    };
    struct AddedEffect {
        EffectHandle handle{kInvalidEffectHandle};
        Layer layer;
    };

    [[nodiscard]] Layer makeLayer(const DecodedMedia &media, float volume,
                                  bool loop, double offsetSeconds) const;
    static double nextSample(Layer &layer);
    void render(Uint8 *stream, int length);
    void lock();
    void unlock();

    SDL_AudioDeviceID device_{};
    int rate_{};
    int channels_{};
    Layer background_;
    // The volume the background was last given, before the duck.
    float backgroundVolume_{};
    float musicDuck_{1.0F};
    Layer speech_;
    Layer effects_;
    std::vector<AddedEffect> addedEffects_;
    EffectHandle nextEffectHandle_{1};
    Layer narration_;
    Layer ambience_;
    bool backgroundHeld_{};
    std::FILE *dump_{};
    std::uint64_t dumpedSamples_{};
    std::size_t foregroundPosition_{};
    std::optional<std::size_t> foregroundHoldPosition_;
};

} // namespace fgp
