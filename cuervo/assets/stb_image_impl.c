// The one translation unit that compiles stb_image (DESIGN.md 4.19 "11.2a").
// PNG and JPEG only: QOI has its own small decoder (image.cpp), and every other
// stb format is compiled out to keep the untrusted-input surface small.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
