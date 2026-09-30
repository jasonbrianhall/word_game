// Minimal SDL2 stand-in for the bare-metal build of letterlock.cpp.
//
// Declares only what the game uses: a software SDL_Renderer (rects, textures,
// alpha blending, logical size), keyboard and mouse events, ticks, the
// preference path, and a float audio device. sdl.cpp implements it on top of
// the kernel's framebuffer, input queues and HDA/AC97 driver.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

// ---------------------------------------------------------------- init / misc
#define SDL_INIT_TIMER  0x0001u
#define SDL_INIT_AUDIO  0x0010u
#define SDL_INIT_VIDEO  0x0020u
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);
const char* SDL_GetError(void);
void SDL_Log(const char* fmt, ...);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
char* SDL_GetPrefPath(const char* org, const char* app);
void SDL_free(void* p);
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
static inline SDL_bool SDL_SetHint(const char*, const char*) { return SDL_TRUE; }

// ---------------------------------------------------------------- geometry
typedef struct SDL_Point { int x, y; } SDL_Point;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
static inline SDL_bool SDL_PointInRect(const SDL_Point* p, const SDL_Rect* r) {
    return (p->x >= r->x && p->x < r->x + r->w && p->y >= r->y && p->y < r->y + r->h) ? SDL_TRUE : SDL_FALSE;
}

// ---------------------------------------------------------------- keyboard
typedef int32_t SDL_Keycode;
enum {
    SDLK_UNKNOWN = 0,
    SDLK_BACKSPACE = 8, SDLK_TAB = 9, SDLK_RETURN = 13, SDLK_ESCAPE = 27,
    SDLK_SPACE = ' ', SDLK_MINUS = '-', SDLK_SLASH = '/',
    SDLK_0 = '0', SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7, SDLK_8, SDLK_9,
    SDLK_LEFTBRACKET = '[', SDLK_RIGHTBRACKET = ']',
    SDLK_a = 'a', SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, SDLK_g, SDLK_h, SDLK_i, SDLK_j,
    SDLK_k, SDLK_l, SDLK_m, SDLK_n, SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t,
    SDLK_u, SDLK_v, SDLK_w, SDLK_x, SDLK_y, SDLK_z,
};
#define SDLK_SCANCODE_MASK (1 << 30)
enum {
    SDLK_F1 = 58 | SDLK_SCANCODE_MASK, SDLK_F2, SDLK_F3, SDLK_F4, SDLK_F5, SDLK_F6,
    SDLK_F7, SDLK_F8, SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12,
    SDLK_RIGHT = 79 | SDLK_SCANCODE_MASK, SDLK_LEFT, SDLK_DOWN, SDLK_UP,
    SDLK_KP_ENTER = 88 | SDLK_SCANCODE_MASK,
};
typedef enum {
    KMOD_NONE = 0,
    KMOD_LSHIFT = 0x0001, KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL = 0x0040, KMOD_RCTRL = 0x0080,
    KMOD_LALT = 0x0100, KMOD_RALT = 0x0200,
    KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
    KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
    KMOD_ALT = KMOD_LALT | KMOD_RALT,
} SDL_Keymod;
SDL_Keymod SDL_GetModState(void);

// ---------------------------------------------------------------- events
enum {
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300, SDL_KEYUP,
    SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP,
};
#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3
#define SDL_PRESSED  1
#define SDL_RELEASED 0
typedef struct SDL_Keysym { int scancode; SDL_Keycode sym; Uint16 mod; Uint32 unused; } SDL_Keysym;
typedef struct SDL_KeyboardEvent { Uint32 type, timestamp, windowID; Uint8 state, repeat, pad2, pad3; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type, timestamp, windowID, which; Uint8 button, state, clicks, pad1; Sint32 x, y; } SDL_MouseButtonEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type, timestamp, windowID, which, state; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
enum { SDL_WINDOWEVENT_SIZE_CHANGED = 6 };
typedef struct SDL_WindowEvent { Uint32 type, timestamp, windowID; Uint8 event, pad1, pad2, pad3; Sint32 data1, data2; } SDL_WindowEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseButtonEvent button;
    SDL_MouseMotionEvent motion;
    Uint8 padding[56];
} SDL_Event;
int SDL_PollEvent(SDL_Event* e);
int SDL_WaitEventTimeout(SDL_Event* e, int timeout_ms);
int SDL_ShowCursor(int toggle);
#define SDL_ENABLE  1
#define SDL_DISABLE 0
#define SDL_QUERY  -1

// ---------------------------------------------------------------- video
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_RWops SDL_RWops;
typedef struct SDL_Surface {
    Uint32 flags;
    void* format;
    int w, h, pitch;
    void* pixels;                    // 0xAARRGGBB, straight alpha
} SDL_Surface;
typedef enum { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND = 1 } SDL_BlendMode;
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOW_SHOWN            0x00000004u
#define SDL_WINDOW_RESIZABLE        0x00000020u
#define SDL_WINDOW_ALLOW_HIGHDPI    0x00002000u
#define SDL_RENDERER_SOFTWARE       0x00000001u
#define SDL_RENDERER_ACCELERATED    0x00000002u
#define SDL_RENDERER_PRESENTVSYNC   0x00000004u

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void SDL_SetWindowTitle(SDL_Window* w, const char* title);
void SDL_DestroyWindow(SDL_Window* w);
// The screen is the window here: always "fullscreen", never resized.
static inline void SDL_SetWindowMinimumSize(SDL_Window*, int, int) {}
static inline Uint32 SDL_GetWindowFlags(SDL_Window*) { return SDL_WINDOW_FULLSCREEN_DESKTOP; }
static inline int SDL_SetWindowFullscreen(SDL_Window*, Uint32) { return 0; }
SDL_Renderer* SDL_CreateRenderer(SDL_Window* w, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer* r);
int SDL_RenderSetLogicalSize(SDL_Renderer* r, int w, int h);
// Reports the logical size, so the game keeps text at the sizes baked into fonts.h.
int SDL_GetRendererOutputSize(SDL_Renderer* r, int* w, int* h);
int SDL_RenderSetViewport(SDL_Renderer* r, const SDL_Rect* rect);   // NULL: whole target
int SDL_GetRendererOutputSize(SDL_Renderer* r, int* w, int* h);
// No letterbox offset here (sdl.cpp centers the whole view itself).
static inline void SDL_RenderGetViewport(SDL_Renderer* r, SDL_Rect* rect) {
    int w = 0, h = 0; SDL_GetRendererOutputSize(r, &w, &h); *rect = SDL_Rect{0, 0, w, h};
}
int SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 red, Uint8 g, Uint8 b, Uint8 a);
int SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode);
int SDL_RenderClear(SDL_Renderer* r);
int SDL_RenderFillRect(SDL_Renderer* r, const SDL_Rect* rect);
int SDL_RenderDrawRect(SDL_Renderer* r, const SDL_Rect* rect);
int SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* src, const SDL_Rect* dst);
void SDL_RenderPresent(SDL_Renderer* r);
SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* r, SDL_Surface* s);
int SDL_QueryTexture(SDL_Texture* t, Uint32* format, int* access, int* w, int* h);
void SDL_DestroyTexture(SDL_Texture* t);
void SDL_FreeSurface(SDL_Surface* s);
SDL_RWops* SDL_RWFromConstMem(const void* mem, int size);

// ---------------------------------------------------------------- audio
typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16SYS AUDIO_S16LSB
#define AUDIO_F32LSB 0x8120
#define AUDIO_F32SYS AUDIO_F32LSB
typedef void (*SDL_AudioCallback)(void* userdata, Uint8* stream, int len);
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void* userdata;
} SDL_AudioSpec;
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                      SDL_AudioSpec* obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
// The "audio thread" is the main loop, so there's nothing to lock against.
static inline void SDL_LockAudioDevice(SDL_AudioDeviceID) {}
static inline void SDL_UnlockAudioDevice(SDL_AudioDeviceID) {}
