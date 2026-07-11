/* stb_vorbis Ogg Vorbis decoder, compiled as C (it does not build cleanly as C++).
   Used by audio_player.cpp for GW2 sounds stored as Ogg Vorbis. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
