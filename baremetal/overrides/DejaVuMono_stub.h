// Force-included ahead of letterlock.cpp (-include) so its own DejaVuMono.h, the
// 450 KB base64 TTF that only real SDL_ttf needs, is skipped via its include
// guard. The bare-metal SDL_ttf draws from fonts.h instead, which is baked
// from that same font by tools/gen_fonts.py. Keeping it out lets the kernel
// fit on a 1.44 MB floppy.
#pragma once
#define DEJAVU_REGULAR_H
#include <stddef.h>
static const char DEJAVU_REGULAR_FONT_B64[] = "AAAA";      // decodes to 3 bytes
static const size_t DEJAVU_REGULAR_FONT_B64_SIZE = 4;
static const unsigned int DEJAVU_REGULAR_FONT_ORIGINAL_SIZE = 3;
