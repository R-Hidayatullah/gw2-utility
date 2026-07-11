// audio_player.cpp -- MP3 decode (dr_mp3) + waveOut playback. This is the single
// translation unit that instantiates dr_mp3.
#include "audio_player.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Ogg Vorbis decoder (some GW2 sounds are Vorbis, e.g. AMSP music banks). stb_vorbis
// is compiled as a separate C unit (src/stb_vorbis_impl.c); declare its entry point.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

#include "gw2_audio.hpp" // codec detection (shared with the parser)

#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <vector>

namespace gw2snd {
namespace {

HWAVEOUT           g_hwo = nullptr;
WAVEHDR            g_hdr{};
std::vector<int16_t> g_pcm;      // decoded PCM, kept alive during playback
std::atomic<bool>  g_done{true}; // set by the waveOut callback at WOM_DONE

// waveOut calls this on its own thread; keep it to a flag set (no waveOut* calls here).
void CALLBACK wave_cb(HWAVEOUT, UINT msg, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (msg == WOM_DONE) g_done.store(true);
}

void close_device() {
    if (!g_hwo) return;
    waveOutReset(g_hwo); // stop + return buffers
    if (g_hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
    waveOutClose(g_hwo);
    g_hwo = nullptr;
    g_hdr = WAVEHDR{};
}

// Decode an MP3 buffer fully to interleaved s16 PCM. Returns false on failure.
bool decode_mp3(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                unsigned& channels, unsigned& sampleRate, double& seconds) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, data, size, nullptr)) return false;
    channels = mp3.channels;
    sampleRate = mp3.sampleRate;
    drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
    if (channels == 0 || sampleRate == 0 || total == 0) { drmp3_uninit(&mp3); return false; }
    pcm.assign((size_t)total * channels, 0);
    drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, total, pcm.data());
    pcm.resize((size_t)got * channels);
    seconds = (double)got / sampleRate;
    drmp3_uninit(&mp3);
    return got > 0;
}

// Decode an Ogg Vorbis buffer fully to interleaved s16 PCM (stb_vorbis).
bool decode_ogg(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                unsigned& channels, unsigned& sampleRate, double& seconds) {
    int ch = 0, sr = 0; short* out = nullptr;
    int frames = stb_vorbis_decode_memory(data, (int)size, &ch, &sr, &out);
    if (frames <= 0 || !out || ch <= 0 || sr <= 0) { if (out) free(out); return false; }
    channels = (unsigned)ch; sampleRate = (unsigned)sr;
    pcm.assign(out, out + (size_t)frames * ch);
    free(out);
    seconds = (double)frames / sr;
    return true;
}

// Decode any supported clip (MP3 or Ogg) to interleaved s16 PCM.
bool decode_clip(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                 unsigned& channels, unsigned& sampleRate, double& seconds) {
    auto clips = gw2audio::extract(data, size);
    if (clips.empty()) return false;
    const auto& c = clips.front();
    if (c.codec == gw2audio::CODEC_MP3) return decode_mp3(c.data.data(), c.data.size(), pcm, channels, sampleRate, seconds);
    if (c.codec == gw2audio::CODEC_OGG) return decode_ogg(c.data.data(), c.data.size(), pcm, channels, sampleRate, seconds);
    return false;
}

} // namespace

ClipInfo probe(const uint8_t* data, size_t size) {
    ClipInfo info;
    if (!data || size == 0) return info;
    auto clips = gw2audio::extract(data, size);
    if (!clips.empty()) info.codec = gw2audio::codecName(clips.front().codec);
    std::vector<int16_t> pcm;
    unsigned ch = 0, sr = 0; double secs = 0;
    if (decode_clip(data, size, pcm, ch, sr, secs)) {
        info.ok = true; info.channels = ch; info.sampleRate = sr; info.seconds = secs;
    }
    return info;
}

bool play(const uint8_t* data, size_t size) {
    stop();
    if (!data || size == 0) return false;

    unsigned channels = 0, sampleRate = 0; double seconds = 0;
    if (!decode_clip(data, size, g_pcm, channels, sampleRate, seconds)) return false;

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = sampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, (DWORD_PTR)wave_cb, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        g_hwo = nullptr;
        return false;
    }
    g_hdr = WAVEHDR{};
    g_hdr.lpData = (LPSTR)g_pcm.data();
    g_hdr.dwBufferLength = (DWORD)(g_pcm.size() * sizeof(int16_t));
    if (waveOutPrepareHeader(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR) { close_device(); return false; }
    g_done.store(false);
    if (waveOutWrite(g_hwo, &g_hdr, sizeof(g_hdr)) != MMSYSERR_NOERROR) { g_done.store(true); close_device(); return false; }
    return true;
}

void stop() {
    close_device();
    g_done.store(true);
}

bool is_playing() {
    return g_hwo != nullptr && !g_done.load();
}

void shutdown() { stop(); }

} // namespace gw2snd
