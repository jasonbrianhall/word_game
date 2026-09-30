// The slice of SDL2 and SDL_ttf that letterlock.cpp uses, implemented in software
// on the kernel's back buffer, input queues and sound driver.
//
//  - SDL_Renderer: solid and outlined rects, alpha-blended textures, and
//    SDL_RenderSetLogicalSize (the game's 500x720 window, scaled by a whole
//    number when the screen has room, centered, like SDL's letterboxing).
//  - SDL_ttf: DejaVu Sans Mono pre-rasterized at each size the game asks for
//    (fonts.h, from the same embedded font), so text looks as on the desktop.
//  - Events: key presses from PS/2 or USB keyboards, mouse motion and buttons
//    from PS/2 or USB mice, in game (logical) coordinates.
//  - Audio: the game's float callback is pulled whenever the driver has room.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "platform.hpp"
#include "audio.hpp"
#include "fonts.h"

// ---------------------------------------------------------------- misc
int SDL_Init(Uint32) { return 0; }
int SDL_InitSubSystem(Uint32) { return 0; }
void SDL_Quit(void) {}
const char* SDL_GetError(void) { return "unsupported on bare metal"; }
void SDL_Log(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    printf("%s\n", buf);
}
Uint32 SDL_GetTicks(void) { return platform_ticks(); }
void SDL_Delay(Uint32 ms) {
    Uint32 end = platform_ticks() + ms;
    while ((int32_t)(end - platform_ticks()) > 0) { platform_service(); __asm__ volatile("hlt"); }
}
// Files written under this path live in RAM until reboot (see overrides/stream_shim.h).
char* SDL_GetPrefPath(const char*, const char* app) {
    size_t n = strlen(app);
    char* p = (char*)malloc(n + 3);
    p[0] = '/'; memcpy(p + 1, app, n); p[n + 1] = '/'; p[n + 2] = 0;
    return p;
}
void SDL_free(void* p) { free(p); }
SDL_Keymod SDL_GetModState(void) { return (SDL_Keymod)platform_mods(); }
int SDL_ShowCursor(int) { return 1; }

// ---------------------------------------------------------------- renderer
struct SDL_Window { char title[64]; char first_title[64]; };
struct SDL_Renderer { Uint8 r, g, b, a; SDL_BlendMode blend; };
struct SDL_Texture { int w, h; uint32_t* px; };      // 0xAARRGGBB, straight alpha
struct SDL_RWops { int unused; };

static SDL_Window the_window;
static SDL_Renderer the_renderer;
static float view_scale = 1.f;                       // logical -> screen
static int view_x, view_y;                           // screen offset of logical (0, 0)

SDL_Window* SDL_CreateWindow(const char* title, int, int, int, int, Uint32) {
    strncpy(the_window.title, title, sizeof(the_window.title) - 1);
    strncpy(the_window.first_title, title, sizeof(the_window.first_title) - 1);
    return &the_window;
}
void SDL_SetWindowTitle(SDL_Window* w, const char* title) {
    strncpy(w->title, title, sizeof(w->title) - 1);
    printf("Title: %s\n", title);
}
void SDL_DestroyWindow(SDL_Window*) {}
SDL_Renderer* SDL_CreateRenderer(SDL_Window*, int, Uint32) {
    the_renderer = {0, 0, 0, 255, SDL_BLENDMODE_NONE};
    view_scale = 1.f; view_x = view_y = 0;
    return &the_renderer;
}
void SDL_DestroyRenderer(SDL_Renderer*) {}

static int logical_w, logical_h;
int SDL_GetRendererOutputSize(SDL_Renderer*, int* w, int* h) {
    if (w) *w = logical_w;
    if (h) *h = logical_h;
    return logical_w > 0 ? 0 : -1;
}
int SDL_RenderSetLogicalSize(SDL_Renderer*, int w, int h) {
    logical_w = w; logical_h = h;
    float s = (float)back_w / w < (float)back_h / h ? (float)back_w / w : (float)back_h / h;
    if (s >= 1.f) s = (float)(int)s;                   // whole-number scale keeps text crisp
    view_scale = s;
    view_x = (int)(back_w - w * s) / 2;
    view_y = (int)(back_h - h * s) / 2;
    return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 red, Uint8 g, Uint8 b, Uint8 a) {
    r->r = red; r->g = g; r->b = b; r->a = a;
    return 0;
}
int SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode) { r->blend = mode; return 0; }

static inline int iround(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia) / 255;
    uint32_t b = ((src & 255) * a + (dst & 255) * ia) / 255;
    return r << 16 | g << 8 | b;
}

// Fill screen pixels [x0,x1) x [y0,y1).
static void fill_px(int x0, int y0, int x1, int y1, uint32_t rgb, uint32_t a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)back_w) x1 = back_w;
    if (y1 > (int)back_h) y1 = back_h;
    if (a == 0) return;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = back + (size_t)y * back_w;
        if (a >= 255) for (int x = x0; x < x1; x++) row[x] = rgb;
        else for (int x = x0; x < x1; x++) row[x] = blend(row[x], rgb, a);
    }
}

// A logical rect in screen pixels (edges rounded, so neighbours tile exactly).
// SDL_RenderSetViewport: drawing is offset by the viewport's corner and
// clipped to it (all in logical coordinates, as in SDL).
static SDL_Rect viewport;
static bool has_viewport;
static int clip_x0, clip_y0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;   // screen pixels

// A logical rect in screen pixels (edges rounded, so neighbours tile exactly).
static void to_screen(const SDL_Rect& r, int& x0, int& y0, int& x1, int& y1) {
    int ox = has_viewport ? viewport.x : 0, oy = has_viewport ? viewport.y : 0;
    x0 = view_x + iround((r.x + ox) * view_scale);
    y0 = view_y + iround((r.y + oy) * view_scale);
    x1 = view_x + iround((r.x + ox + r.w) * view_scale);
    y1 = view_y + iround((r.y + oy + r.h) * view_scale);
}
static void clip(int& x0, int& y0, int& x1, int& y1) {
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
}
int SDL_RenderSetViewport(SDL_Renderer*, const SDL_Rect* rect) {
    has_viewport = rect != nullptr;
    if (!rect) { clip_x0 = clip_y0 = 0; clip_x1 = clip_y1 = 1 << 30; return 0; }
    viewport = *rect;
    has_viewport = false;                              // the clip rect itself isn't offset
    to_screen(*rect, clip_x0, clip_y0, clip_x1, clip_y1);
    has_viewport = true;
    return 0;
}

static void fill_logical(SDL_Renderer* r, const SDL_Rect& rc) {
    int x0, y0, x1, y1;
    to_screen(rc, x0, y0, x1, y1);
    clip(x0, y0, x1, y1);
    uint32_t a = r->blend == SDL_BLENDMODE_BLEND ? r->a : 255;
    fill_px(x0, y0, x1, y1, (uint32_t)r->r << 16 | r->g << 8 | r->b, a);
}

int SDL_RenderClear(SDL_Renderer* r) {             // the whole screen, like SDL's letterbox
    fill_px(0, 0, back_w, back_h, (uint32_t)r->r << 16 | r->g << 8 | r->b, 255);
    return 0;
}
int SDL_RenderFillRect(SDL_Renderer* r, const SDL_Rect* rect) {
    if (rect) fill_logical(r, *rect);
    else {
        int x0 = 0, y0 = 0, x1 = back_w, y1 = back_h;
        clip(x0, y0, x1, y1);
        fill_px(x0, y0, x1, y1, (uint32_t)r->r << 16 | r->g << 8 | r->b, r->blend ? r->a : 255);
    }
    return 0;
}
int SDL_RenderDrawRect(SDL_Renderer* r, const SDL_Rect* rc) {  // 1 logical pixel wide
    if (!rc || rc->w <= 0 || rc->h <= 0) return 0;
    fill_logical(r, {rc->x, rc->y, rc->w, 1});
    fill_logical(r, {rc->x, rc->y + rc->h - 1, rc->w, 1});
    fill_logical(r, {rc->x, rc->y, 1, rc->h});
    fill_logical(r, {rc->x + rc->w - 1, rc->y, 1, rc->h});
    return 0;
}

SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer*, SDL_Surface* s) {
    if (!s) return nullptr;
    SDL_Texture* t = (SDL_Texture*)malloc(sizeof(SDL_Texture));
    t->w = s->w; t->h = s->h;
    t->px = (uint32_t*)malloc((size_t)s->w * s->h * 4);
    memcpy(t->px, s->pixels, (size_t)s->w * s->h * 4);
    return t;
}
int SDL_QueryTexture(SDL_Texture* t, Uint32* format, int* access, int* w, int* h) {
    if (format) *format = 0;
    if (access) *access = 0;
    if (w) *w = t->w;
    if (h) *h = t->h;
    return 0;
}
void SDL_DestroyTexture(SDL_Texture* t) { if (t) { free(t->px); free(t); } }
void SDL_FreeSurface(SDL_Surface* s) { if (s) { free(s->pixels); free(s); } }
SDL_RWops* SDL_RWFromConstMem(const void*, int) { static SDL_RWops rw; return &rw; }

// Nearest-neighbour stretch with alpha blending.
int SDL_RenderCopy(SDL_Renderer*, SDL_Texture* t, const SDL_Rect* src, const SDL_Rect* dst) {
    if (!t) return -1;
    SDL_Rect s = src ? *src : SDL_Rect{0, 0, t->w, t->h};
    SDL_Rect d = dst ? *dst : SDL_Rect{0, 0, (int)((back_w - 2 * view_x) / view_scale), (int)((back_h - 2 * view_y) / view_scale)};
    if (s.w <= 0 || s.h <= 0 || d.w <= 0 || d.h <= 0) return 0;
    int x0, y0, x1, y1;
    to_screen(d, x0, y0, x1, y1);
    int dw = x1 - x0, dh = y1 - y0;
    if (dw <= 0 || dh <= 0) return 0;
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
    clip(cx0, cy0, cx1, cy1);
    for (int y = cy0 > 0 ? cy0 : 0; y < cy1 && y < (int)back_h; y++) {
        int ty = s.y + (int)((int64_t)(y - y0) * s.h / dh);
        const uint32_t* srow = t->px + (size_t)ty * t->w;
        uint32_t* row = back + (size_t)y * back_w;
        for (int x = cx0 > 0 ? cx0 : 0; x < cx1 && x < (int)back_w; x++) {
            uint32_t p = srow[s.x + (int)((int64_t)(x - x0) * s.w / dw)];
            uint32_t a = p >> 24;
            if (a == 255) row[x] = p & 0xFFFFFF;
            else if (a) row[x] = blend(row[x], p, a);
        }
    }
    return 0;
}

static void draw_text_px(const char* s, int x, int y, int pt, uint32_t rgb);

void SDL_RenderPresent(SDL_Renderer*) {
    // The game only signals "muted" through the window title; show a changed
    // title in the top-left corner of the screen.
    if (strcmp(the_window.title, the_window.first_title) != 0)
        draw_text_px(the_window.title, 8, 6, 15, 0x818384);
    platform_present();
    platform_service();
}

// ---------------------------------------------------------------- SDL_ttf
struct TTF_Font { const BakedFont* f; };

int TTF_Init(void) { return 0; }
void TTF_Quit(void) {}
const char* TTF_GetError(void) { return "unsupported on bare metal"; }

static const BakedFont* baked_for(int pt) {
    const BakedFont* best = &baked_fonts[0];
    for (const BakedFont& f : baked_fonts)
        if (abs(f.pt - pt) < abs(best->pt - pt)) best = &f;
    return best;
}
TTF_Font* TTF_OpenFontRW(SDL_RWops*, int, int ptsize) {
    TTF_Font* f = (TTF_Font*)malloc(sizeof(TTF_Font));
    f->f = baked_for(ptsize);
    return f;
}
void TTF_CloseFont(TTF_Font* f) { free(f); }
int TTF_FontAscent(const TTF_Font* f) { return f->f->ascent; }
int TTF_FontHeight(const TTF_Font* f) { return f->f->height; }
int TTF_GlyphMetrics(TTF_Font* f, Uint16, int* minx, int* maxx, int* miny, int* maxy, int* advance) {
    if (minx) *minx = 0;
    if (maxx) *maxx = f->f->adv;
    if (miny) *miny = 0;
    if (maxy) *maxy = f->f->cap_h;
    if (advance) *advance = f->f->adv;
    return 0;
}
// Coverage cell for a character; characters that weren't baked draw as '?'.
static inline const uint8_t* glyph(const BakedFont* f, unsigned char c) {
    int idx = (c >= 32 && c <= 126) ? f->index[c - 32] : -1;
    if (idx < 0) idx = f->index['?' - 32];
    return f->cov + (size_t)idx * f->height * f->adv;
}
static inline uint32_t alpha(uint8_t cov) { return cov * 17u; }   // 0-15 -> 0-255
SDL_Surface* TTF_RenderUTF8_Blended(TTF_Font* font, const char* text, SDL_Color fg) {
    const BakedFont* f = font->f;
    int n = (int)strlen(text);
    if (n == 0) return nullptr;
    SDL_Surface* s = (SDL_Surface*)calloc(1, sizeof(SDL_Surface));
    s->w = n * f->adv; s->h = f->height; s->pitch = s->w * 4;
    uint32_t* px = (uint32_t*)malloc((size_t)s->w * s->h * 4);
    s->pixels = px;
    uint32_t rgb = (uint32_t)fg.r << 16 | fg.g << 8 | fg.b;
    for (int i = 0; i < n; i++) {
        const uint8_t* g = glyph(f, (unsigned char)text[i]);
        for (int y = 0; y < f->height; y++)
            for (int x = 0; x < f->adv; x++)
                px[(size_t)y * s->w + i * f->adv + x] = alpha(g[y * f->adv + x]) << 24 | rgb;
    }
    return s;
}

// Text straight onto the screen (kernel messages, the muted title).
static void draw_text_px(const char* s, int x, int y, int pt, uint32_t rgb) {
    const BakedFont* f = baked_for(pt);
    for (; *s; s++, x += f->adv) {
        const uint8_t* g = glyph(f, (unsigned char)*s);
        for (int gy = 0; gy < f->height; gy++)
            for (int gx = 0; gx < f->adv; gx++)
                if (uint32_t c = alpha(g[gy * f->adv + gx])) fill_px(x + gx, y + gy, x + gx + 1, y + gy + 1, rgb, c);
    }
}
void sdl_draw_label(const char* s, int cx, int cy, uint32_t rgb) {
    const BakedFont* f = baked_for(20);
    draw_text_px(s, cx - (int)strlen(s) * f->adv / 2, cy - f->height / 2, 20, rgb);
}

// ---------------------------------------------------------------- events
static SDL_Event queue[128];
static unsigned q_head, q_tail;

static void push(const SDL_Event& e) {
    if (q_head - q_tail >= 128) return;              // full: drop
    queue[q_head++ % 128] = e;
}
static bool pop(SDL_Event* e) {
    if (q_tail == q_head) return false;
    SDL_Event ev = queue[q_tail++ % 128];
    if (e) *e = ev;
    else q_tail--;                                   // SDL_PollEvent(NULL) only peeks
    return true;
}

void sdl_key_event(int32_t sym, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = platform_ticks();
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = sym;
    e.key.keysym.mod = (Uint16)platform_mods();
    push(e);
}
static int logical_x(int x) { return (int)((x - view_x) / view_scale); }
static int logical_y(int y) { return (int)((y - view_y) / view_scale); }
void sdl_mouse_motion(int x, int y, int dx, int dy) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.motion.type = SDL_MOUSEMOTION;
    e.motion.timestamp = platform_ticks();
    e.motion.x = logical_x(x); e.motion.y = logical_y(y);
    e.motion.xrel = dx; e.motion.yrel = dy;
    push(e);
}
void sdl_mouse_button(int x, int y, int button, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.timestamp = platform_ticks();
    e.button.button = (Uint8)button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = logical_x(x); e.button.y = logical_y(y);
    push(e);
}

int SDL_PollEvent(SDL_Event* e) {
    platform_service();
    return pop(e) ? 1 : 0;
}
int SDL_WaitEventTimeout(SDL_Event* e, int timeout_ms) {
    Uint32 end = platform_ticks() + (Uint32)timeout_ms;
    for (;;) {
        platform_service();
        if (pop(e)) return 1;
        if (timeout_ms >= 0 && (int32_t)(end - platform_ticks()) <= 0) return 0;
        __asm__ volatile("hlt");
    }
}
int32_t sdl_wait_key() {
    SDL_Event e;
    for (;;)
        if (SDL_WaitEventTimeout(&e, -1) && e.type == SDL_KEYDOWN) return e.key.keysym.sym;
}

// ---------------------------------------------------------------- audio
// The game asks for 44.1 kHz float mono; like SDL, we report what we really
// run (48 kHz, float) and the game synthesizes at that rate.
static SDL_AudioCallback aud_cb;
static void* aud_user;
static SDL_AudioFormat aud_fmt;
static bool aud_open, aud_running;

SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* want, SDL_AudioSpec* got, int) {
    if (!want || !want->callback || audio_name()[0] == 'n') return 0;   // no sound card
    aud_cb = want->callback;
    aud_user = want->userdata;
    aud_fmt = want->format == AUDIO_F32SYS ? AUDIO_F32SYS : AUDIO_S16SYS;
    if (got) { *got = *want; got->freq = audio_rate(); got->format = aud_fmt; got->channels = 1; }
    aud_open = true;
    return 1;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on) { aud_running = aud_open && !pause_on; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID) { aud_open = aud_running = false; }

void sdl_pump_audio() {
    static bool busy;                                  // no re-entry from inside the callback
    if (busy) return;
    busy = true;
    uint32_t want = audio_frames_wanted();
    while (want > 0) {
        int n = want > 256 ? 256 : (int)want;
        static float f[256];
        static int16_t out[256];
        if (aud_running && aud_fmt == AUDIO_F32SYS) {
            aud_cb(aud_user, (Uint8*)f, n * (int)sizeof(float));
            for (int i = 0; i < n; i++) {
                float v = f[i] * 32767.f;
                out[i] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v;
            }
        } else if (aud_running) {
            aud_cb(aud_user, (Uint8*)out, n * 2);
        } else {
            memset(out, 0, n * 2);                     // keep the ring quiet
        }
        audio_write(out, n);
        want -= n;
    }
    busy = false;
}
