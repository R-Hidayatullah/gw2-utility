// gw2_audio.hpp -- locate the embedded audio blob inside a GW2 audio packfile.
//
// GW2 stores sound effects / voice as PF packfiles with container fourcc "ASND"
// (chunk ASND -> AsndFileDataV0/V1/V2). The chunk carries a small header (voiceId,
// flags, length, sample info, ...) followed by an `audioData` array_ptr that holds a
// complete, standard audio file -- in practice **MP3** (0xFF frame sync, usually with
// a Xing/Info VBR header), occasionally Ogg Vorbis / WAV. "AMSP" packfiles are sound
// pools/scripts (metadata that references sounds), not embedded audio.
//
// Rather than parse the version-specific AsndFileData struct, we locate the audio by
// its file signature within the packfile payload: the fields before `audioData` are
// small ints/floats, so the first valid MP3 frame (or ID3/OggS/RIFF/fLaC marker) is
// the real audio start, and it runs to the end of the entry (a few bytes of trailing
// padding are ignored by every decoder). This is robust across ASND versions.
#ifndef GW2_AUDIO_HPP
#define GW2_AUDIO_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>

namespace gw2audio {

enum Codec { CODEC_UNKNOWN = 0, CODEC_MP3, CODEC_OGG, CODEC_WAV, CODEC_FLAC };

inline const char* codecName(Codec c) {
    switch (c) {
        case CODEC_MP3:  return "MP3";
        case CODEC_OGG:  return "Ogg Vorbis";
        case CODEC_WAV:  return "WAV";
        case CODEC_FLAC: return "FLAC";
        default:         return "unknown";
    }
}

struct Clip {
    std::string name;            // e.g. "sound" or a voiceId
    Codec       codec = CODEC_UNKNOWN;
    size_t      offset = 0;      // start of the audio bytes within the source
    std::vector<uint8_t> data;   // the audio file bytes (MP3/Ogg/WAV/FLAC)
};

namespace detail {
// A valid MPEG-1/2/2.5 audio frame header (4 bytes) starting at b[i].
inline bool isMp3Frame(const uint8_t* b, size_t n, size_t i) {
    if (i + 4 > n) return false;
    if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) return false; // 11 sync bits
    uint8_t ver = (b[i + 1] >> 3) & 0x3; // 01 = reserved
    uint8_t lay = (b[i + 1] >> 1) & 0x3; // 00 = reserved
    uint8_t br  = (b[i + 2] >> 4) & 0xF; // 0000 free / 1111 bad
    uint8_t sr  = (b[i + 2] >> 2) & 0x3; // 11 = reserved
    return ver != 1 && lay != 0 && br != 0 && br != 0xF && sr != 3;
}
// Byte length of the MPEG audio frame at b[i], or 0 if not a valid frame header.
// Used to walk frame-by-frame and split a bank into individual sounds.
inline size_t mp3FrameLen(const uint8_t* b, size_t n, size_t i) {
    if (!isMp3Frame(b, n, i)) return 0;
    static const int SR1[]  = {44100, 48000, 32000};       // MPEG-1
    static const int SR2[]  = {22050, 24000, 16000};       // MPEG-2
    static const int SR25[] = {11025, 12000,  8000};       // MPEG-2.5
    static const int BR_L1_M1[]  = {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448};
    static const int BR_L2_M1[]  = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384};
    static const int BR_L3_M1[]  = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const int BR_L1_M2[]  = {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256};
    static const int BR_L23_M2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    uint8_t verb = (b[i + 1] >> 3) & 0x3; // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    uint8_t lay  = (b[i + 1] >> 1) & 0x3; // 3=LayerI, 2=LayerII, 1=LayerIII
    uint8_t brix = (b[i + 2] >> 4) & 0xF;
    uint8_t srix = (b[i + 2] >> 2) & 0x3;
    uint8_t pad  = (b[i + 2] >> 1) & 0x1;
    bool m1 = (verb == 3);
    int sr = (verb == 3 ? SR1 : verb == 2 ? SR2 : SR25)[srix];
    int br;
    if (lay == 3)      br = (m1 ? BR_L1_M1 : BR_L1_M2)[brix];
    else if (lay == 2) br = (m1 ? BR_L2_M1 : BR_L23_M2)[brix];
    else               br = (m1 ? BR_L3_M1 : BR_L23_M2)[brix];
    if (br == 0 || sr == 0) return 0;
    br *= 1000;
    if (lay == 3) return (size_t)(12 * br / sr + pad) * 4;           // Layer I
    int coef = (m1 || lay == 2) ? 144 : 72;                          // Layer II/III
    return (size_t)(coef * br / sr + pad);
}
inline bool match(const uint8_t* b, size_t n, size_t i, const char* sig, size_t len) {
    if (i + len > n) return false;
    for (size_t k = 0; k < len; ++k) if (b[i + k] != (uint8_t)sig[k]) return false;
    return true;
}
} // namespace detail

// Find where the audio file starts inside `b` (searching from `from`). Returns the
// codec + offset, or CODEC_UNKNOWN if nothing recognizable is found.
inline Codec findAudioStart(const uint8_t* b, size_t n, size_t from, size_t& outOff) {
    using namespace detail;
    for (size_t i = from; i + 4 <= n; ++i) {
        if (match(b, n, i, "OggS", 4)) { outOff = i; return CODEC_OGG; }
        if (match(b, n, i, "ID3",  3)) { outOff = i; return CODEC_MP3; }
        if (match(b, n, i, "fLaC", 4)) { outOff = i; return CODEC_FLAC; }
        if (match(b, n, i, "RIFF", 4) && match(b, n, i + 8, "WAVE", 4)) { outOff = i; return CODEC_WAV; }
        if (isMp3Frame(b, n, i)) { outOff = i; return CODEC_MP3; }
    }
    outOff = n;
    return CODEC_UNKNOWN;
}

// True if the bytes look like a GW2 audio packfile (or a raw audio file).
inline bool isAudio(const uint8_t* b, size_t n) {
    if (n >= 12 && b[0] == 'P' && b[1] == 'F') {
        const char* c = (const char*)b + 8;
        if (!std::memcmp(c, "ASND", 4) || !std::memcmp(c, "AMSP", 4) ||
            !std::memcmp(c, "ABNK", 4))
            return true;
    }
    // Raw audio entry (rare): starts directly with a known signature.
    size_t off = 0;
    return findAudioStart(b, n, 0, off) != CODEC_UNKNOWN && off < 64;
}

// Extract the embedded audio clip(s) from a decompressed entry. An ASND / raw asnd
// yields a single clip; an ABNK bank yields one clip per embedded sound (found by
// walking MP3 frames -- each sound is a contiguous run of frames, separated by a
// short per-sound header). Non-MP3 (Ogg/WAV/FLAC) is returned as a single blob.
// Returns an empty list if no audio is present (e.g. an AMSP metadata pool).
inline std::vector<Clip> extract(const uint8_t* b, size_t n) {
    using namespace detail;
    std::vector<Clip> clips;
    if (!b || n < 4) return clips;

    bool pf = (n >= 12 && b[0] == 'P' && b[1] == 'F');
    size_t searchFrom = pf ? 12 : 0; // past the PF header, if any

    // Non-MP3 containers (rare in GW2): a single blob to EOF.
    {
        size_t off = 0;
        Codec c = CODEC_UNKNOWN;
        for (size_t i = searchFrom; i + 4 <= n; ++i) {
            if (match(b, n, i, "OggS", 4)) { c = CODEC_OGG; off = i; break; }
            if (match(b, n, i, "fLaC", 4)) { c = CODEC_FLAC; off = i; break; }
            if (match(b, n, i, "RIFF", 4) && match(b, n, i + 8, "WAVE", 4)) { c = CODEC_WAV; off = i; break; }
            if (isMp3Frame(b, n, i) || match(b, n, i, "ID3", 3)) break; // MP3 -> handled below
        }
        if (c != CODEC_UNKNOWN) {
            Clip clip; clip.name = "sound"; clip.codec = c; clip.offset = off;
            clip.data.assign(b + off, b + n);
            clips.push_back(std::move(clip));
            return clips;
        }
    }

    // MP3: walk frames. Each maximal run of >= kMinFrames contiguous frames is one
    // sound (a bank concatenates several, each preceded by a small header/gap).
    const int kMinFrames = 3;
    size_t i = searchFrom;
    while (i + 4 <= n) {
        size_t fl = mp3FrameLen(b, n, i);
        if (fl == 0) { ++i; continue; }
        size_t start = i;
        int frames = 0;
        while (i + 4 <= n) {
            size_t l = mp3FrameLen(b, n, i);
            if (l == 0) break;
            i += l; ++frames;
        }
        if (frames >= kMinFrames) {
            Clip clip;
            clip.name = "sound " + std::to_string(clips.size() + 1);
            clip.codec = CODEC_MP3;
            clip.offset = start;
            clip.data.assign(b + start, b + (i > n ? n : i));
            clips.push_back(std::move(clip));
        }
    }
    return clips;
}

} // namespace gw2audio
#endif // GW2_AUDIO_HPP
