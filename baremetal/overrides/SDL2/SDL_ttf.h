// Minimal SDL_ttf stand-in: DejaVu Sans Mono pre-rasterized at the sizes the
// game uses (fonts.h). Implemented in sdl.cpp.
#pragma once
#include "SDL.h"

typedef struct TTF_Font TTF_Font;
int TTF_Init(void);
void TTF_Quit(void);
const char* TTF_GetError(void);
TTF_Font* TTF_OpenFontRW(SDL_RWops* src, int freesrc, int ptsize);
void TTF_CloseFont(TTF_Font* font);
int TTF_FontAscent(const TTF_Font* font);
int TTF_FontHeight(const TTF_Font* font);
int TTF_GlyphMetrics(TTF_Font* font, Uint16 ch, int* minx, int* maxx, int* miny, int* maxy, int* advance);
SDL_Surface* TTF_RenderUTF8_Blended(TTF_Font* font, const char* text, SDL_Color fg);
