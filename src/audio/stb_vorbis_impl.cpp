// The one translation unit that compiles stb_vorbis (public domain Ogg Vorbis decoder).

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4456 4457 4701 4703)
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
