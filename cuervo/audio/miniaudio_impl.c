/* The miniaudio implementation (third-party; built by cmake/Dependencies.cmake
   with warnings off). OGG Vorbis comes from stb_vorbis in miniaudio's extras:
   its declarations must be visible before the implementation so miniaudio
   enables its Vorbis decoder, and its body is compiled after. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
