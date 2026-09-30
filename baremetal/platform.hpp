#pragma once
// Glue between the kernel (kernel.cpp: hardware) and the SDL stand-in
// (sdl.cpp: what the game sees).
#include <stdint.h>

// Key and modifier values, identical to SDL's.
enum : int32_t {
    KEY_F1 = 58 | (1 << 30),
    KEY_RIGHT = 79 | (1 << 30), KEY_LEFT, KEY_DOWN, KEY_UP,
    KEY_KP_ENTER = 88 | (1 << 30),
};
enum : uint32_t {
    MOD_LSHIFT = 0x0001, MOD_RSHIFT = 0x0002, MOD_LCTRL = 0x0040, MOD_RCTRL = 0x0080,
    MOD_LALT = 0x0100, MOD_RALT = 0x0200,
};

// ---- kernel.cpp
extern uint32_t* back;                 // back buffer, 0x00RRGGBB, back_w x back_h
extern uint32_t back_w, back_h;
uint32_t platform_ticks();             // milliseconds since boot
uint32_t platform_mods();              // SDL_Keymod bits
void platform_service();               // poll input, USB and sound
void platform_present();               // back buffer (+ mouse cursor) to the screen
void platform_reboot();

// ---- sdl.cpp
void sdl_key_event(int32_t sym, bool down);
void sdl_mouse_motion(int x, int y, int dx, int dy);        // screen pixels
void sdl_mouse_button(int x, int y, int button, bool down);  // SDL button numbers
void sdl_pump_audio();
int32_t sdl_wait_key();                                      // blocks; next key press
void sdl_draw_label(const char* s, int cx, int cy, uint32_t rgb);   // centered, 20 pt
