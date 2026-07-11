// audio_player.h -- decode a GW2 audio clip (MP3 via dr_mp3) and play it through
// the Windows waveOut API. Single-clip: starting a new clip stops the current one.
#ifndef GW2_AUDIO_PLAYER_H
#define GW2_AUDIO_PLAYER_H

#include <cstdint>
#include <cstddef>

namespace gw2snd {

struct ClipInfo {
    bool          ok = false;
    unsigned      channels = 0;
    unsigned      sampleRate = 0;
    double        seconds = 0.0;
    const char*   codec = "unknown";
};

// Decode just enough to report channels / sample rate / duration (no playback).
ClipInfo probe(const uint8_t* data, size_t size);

// Decode `data` (an MP3/… audio file) and start playing it. Stops any current
// playback first. Returns false if the audio could not be decoded.
bool play(const uint8_t* data, size_t size);

// Stop playback and release the audio device.
void stop();

// True while a clip is actively playing (auto-clears when it reaches the end).
bool is_playing();

// Release the device on shutdown.
void shutdown();

} // namespace gw2snd

#endif // GW2_AUDIO_PLAYER_H
