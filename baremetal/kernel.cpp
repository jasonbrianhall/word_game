// Bare-metal kernel for Wordle.
//
// Boots via Multiboot (GRUB or QEMU -kernel) or the UEFI loader, sets up the
// framebuffer, memory, timer, PS/2 keyboard and mouse, USB keyboards and mice,
// and sound, then runs the unchanged wordle.cpp main() on top of the SDL
// stand-in in sdl.cpp.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hw.hpp"
#include "platform.hpp"
#include "audio.hpp"
#include "pci.hpp"
#include "usb.hpp"

int main(int argc, char** argv);   // wordle.cpp

// ---------------------------------------------------------------- serial
#define COM1 0x3F8
extern "C" void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (uint8_t)c);
}
extern "C" void serial_puts(const char* s) { while (*s) serial_putc(*s++); }
static void serial_init() {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
}

// ---------------------------------------------------------------- multiboot
struct __attribute__((packed)) MultibootInfo {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr, drives_length, drives_addr;
    uint32_t config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t fb_bpp, fb_type;
};
struct __attribute__((packed)) MultibootMmap { uint32_t size; uint64_t addr, len; uint32_t type; };
extern "C" uint32_t mb_magic, mb_info;
extern "C" uint64_t phys_limit;
uint64_t phys_limit = 0x100000000ull;
extern "C" char __kernel_start[], __kernel_end[];
extern "C" void heap_add(void* p, size_t n);

// ---------------------------------------------------------------- memory
// Give the heap every usable RAM region the boot loader reports, minus the
// kernel image and anything below 1 MiB.
static void heap_init(const MultibootInfo* mbi) {
    const uint64_t k0 = (uintptr_t)__kernel_start & ~0xFFFull;
    const uint64_t k1 = ((uintptr_t)__kernel_end + 0xFFF) & ~0xFFFull;
    uint64_t total = 0;
    auto add = [&](uint64_t a, uint64_t e) {
        if (e > phys_limit) e = phys_limit;
        if (a < 0x100000) a = 0x100000;
        if (a < k1 && e > k0) {
            if (a < k0) { heap_add((void*)(uintptr_t)a, (size_t)(k0 - a)); total += k0 - a; }
            a = k1;
        }
        if (e > a) { heap_add((void*)(uintptr_t)a, (size_t)(e - a)); total += e - a; }
    };
    if (mbi->flags & (1 << 6)) {
        static MultibootMmap map[128];               // copy first: the heap may reuse it
        uintptr_t p = mbi->mmap_addr, end = p + mbi->mmap_length;
        int n = 0;
        while (p < end && n < 128) {
            const MultibootMmap* m = (const MultibootMmap*)p;
            map[n++] = *m;
            p += m->size + 4;
        }
        for (int i = 0; i < n; i++)
            if (map[i].type == 1) add(map[i].addr, map[i].addr + map[i].len);
    } else if (mbi->flags & 1) {
        add(0x100000, 0x100000 + (uint64_t)mbi->mem_upper * 1024);
    }
    printf("Heap: %lu MB of RAM\n", (unsigned long)(total >> 20));
}

// ---------------------------------------------------------------- video
// 1024x768 is the mode every VM's video BIOS offers, so it looks the same
// everywhere; the game's 500x720 window sits centered in it.
#ifndef SCREEN_W
#define SCREEN_W 1024u
#define SCREEN_H 768u
#endif
static volatile uint32_t* fb;
static uint32_t fb_w, fb_h, fb_pitch;   // pitch in pixels
uint32_t* back;
uint32_t back_w, back_h;

static bool bga_init(uint32_t w, uint32_t h) {
    uint32_t base = 0;
    PciDevice vga;
    if (pci_find_id(0x1234, 0x1111, &vga))                  // QEMU/Bochs std VGA
        base = pci_read(vga, 0x10) & 0xFFFFFFF0;
    outw(0x1CE, 0); if (!base || inw(0x1CF) < 0xB0C0) return false;
    auto w16 = [](uint16_t i, uint16_t v) { outw(0x1CE, i); outw(0x1CF, v); };
    w16(4, 0); w16(1, w); w16(2, h); w16(3, 32); w16(4, 0x41);
    fb = (volatile uint32_t*)(uintptr_t)base;
    fb_w = w; fb_h = h; fb_pitch = w;
    return true;
}

static bool video_init(const MultibootInfo* mbi) {
    if ((mbi->flags & (1 << 12)) && mbi->fb_type == 1 && mbi->fb_bpp == 32 && mbi->fb_addr < phys_limit) {
        fb = (volatile uint32_t*)(uintptr_t)mbi->fb_addr;
        fb_w = mbi->fb_width; fb_h = mbi->fb_height; fb_pitch = mbi->fb_pitch / 4;
        printf("Using bootloader framebuffer %ux%u\n", fb_w, fb_h);
        return true;
    }
    if (bga_init(SCREEN_W, SCREEN_H)) { printf("Using Bochs/QEMU VBE %ux%u\n", SCREEN_W, SCREEN_H); return true; }
    return false;
}

// ---------------------------------------------------------------- mouse cursor
// A plain arrow, drawn on the framebuffer after each frame is copied, so the
// game's own back buffer never contains it. Hidden until the mouse moves.
static const char* const CURSOR[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ",
    "      X..X  ", "       XX   ",
};
static int mouse_x, mouse_y;           // screen pixels
static bool cursor_on;

static void draw_cursor() {
    if (!cursor_on) return;
    for (int r = 0; r < 20; r++)
        for (int c = 0; CURSOR[r][c]; c++) {
            int x = mouse_x + c, y = mouse_y + r;
            if (CURSOR[r][c] == ' ' || x >= (int)fb_w || y >= (int)fb_h) continue;
            fb[y * fb_pitch + x] = CURSOR[r][c] == 'X' ? 0x000000 : 0xFFFFFF;
        }
}

void platform_present() {
    for (uint32_t y = 0; y < back_h; y++)
        memcpy((void*)&fb[y * fb_pitch], &back[y * back_w], back_w * 4);
    draw_cursor();
}

// ---------------------------------------------------------------- interrupts
struct __attribute__((packed)) IdtEntry {
    uint16_t off_lo, sel; uint8_t ist, type; uint16_t off_mid; uint32_t off_hi, zero;
};
static IdtEntry idt[256];
extern "C" void isr_timer(), isr_keyboard(), isr_mouse(), isr_spurious(), isr_fault();

static void set_gate(int n, void (*h)()) {
    uintptr_t a = (uintptr_t)h;
    idt[n] = { (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
}

extern volatile uint32_t ticks;
extern volatile uint8_t kbd_buf[256];
extern volatile uint8_t kbd_head, kbd_tail;
extern volatile uint8_t mouse_buf[256];
extern volatile uint8_t mouse_head, mouse_tail;

uint32_t platform_ticks() { return ticks; }   // milliseconds (the PIT runs at 1 kHz)

// ---- PS/2 mouse on the i8042's second port
static bool i8042_wait_write() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true; return false; }
static bool i8042_wait_read()  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true; return false; }
static bool mouse_cmd(uint8_t b) {
    i8042_wait_write(); outb(0x64, 0xD4);
    i8042_wait_write(); outb(0x60, b);
    for (int tries = 0; tries < 4; tries++) {          // skip stray bytes until the ACK
        if (!i8042_wait_read()) return false;
        if (inb(0x60) == 0xFA) return true;
    }
    return false;
}
static bool ps2_mouse_init() {
    i8042_wait_write(); outb(0x64, 0xA8);              // enable the aux port
    i8042_wait_write(); outb(0x64, 0x20);              // read controller config
    if (!i8042_wait_read()) return false;
    uint8_t cfg = inb(0x60);
    cfg = (cfg | 0x02) & ~0x20;                        // aux interrupt on, aux clock on
    i8042_wait_write(); outb(0x64, 0x60);
    i8042_wait_write(); outb(0x60, cfg);
    if (!mouse_cmd(0xF6) || !mouse_cmd(0xF4)) {        // defaults, then start streaming
        printf("PS/2 mouse: none\n");
        return false;
    }
    printf("PS/2 mouse: ready\n");
    return true;
}

static void interrupts_init() {
    for (int i = 0; i < 32; i++) set_gate(i, isr_fault);
    for (int i = 32; i < 256; i++) set_gate(i, isr_spurious);
    set_gate(32, isr_timer);
    set_gate(33, isr_keyboard);
    set_gate(44, isr_mouse);
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } idtr = { sizeof(idt) - 1, (uintptr_t)idt };
    __asm__ volatile("lidt %0" ::"m"(idtr));

    // Remap the PICs to vectors 32..47; unmask timer, keyboard, cascade and mouse.
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 32);   outb(0xA1, 40);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xF8); outb(0xA1, 0xEF);

    uint16_t div = 1193182 / 1000;                     // 1 kHz: SDL_GetTicks() is in ms
    outb(0x43, 0x36); outb(0x40, div & 0xFF); outb(0x40, div >> 8);

    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    __asm__ volatile("sti");
}

void platform_reboot() {
    printf("Rebooting\n");
    __asm__ volatile("cli");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);                                  // i8042 pulse reset line
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);              // PCI reset control
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(none));     // triple fault
    for (;;) __asm__ volatile("hlt");
}

// ---------------------------------------------------------------- keyboard
static uint32_t mods;
uint32_t platform_mods() { return mods; }

// Set-1 scancode -> SDL keycode (ASCII for printable keys, like SDL).
static int32_t keycode(bool ext, uint8_t code) {
    if (ext) {
        switch (code) {
        case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;  case 0x4D: return KEY_RIGHT;
        case 0x1C: return KEY_KP_ENTER;
        }
        return 0;
    }
    static const char row1[] = "1234567890-=";          // 0x02..0x0D
    static const char row2[] = "qwertyuiop[]";          // 0x10..0x1B
    static const char row3[] = "asdfghjkl;'`";          // 0x1E..0x29
    static const char row4[] = "\\zxcvbnm,./";          // 0x2B..0x35
    if (code >= 0x02 && code <= 0x0D) return row1[code - 0x02];
    if (code >= 0x10 && code <= 0x1B) return row2[code - 0x10];
    if (code >= 0x1E && code <= 0x29) return row3[code - 0x1E];
    if (code >= 0x2B && code <= 0x35) return row4[code - 0x2B];
    switch (code) {
    case 0x01: return 27;  case 0x0E: return 8;  case 0x0F: return 9;
    case 0x1C: return 13;  case 0x39: return ' ';
    case 0x57: return KEY_F1 + 10;  case 0x58: return KEY_F1 + 11;   // F11, F12
    }
    if (code >= 0x3B && code <= 0x44) return KEY_F1 + (code - 0x3B);
    return 0;
}

// Queue a scancode from a source other than the PS/2 interrupt (USB).
void kbd_push(uint8_t b) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

static void poll_keyboard() {
    static bool ext;
    while (kbd_tail != kbd_head) {
        uint8_t b = kbd_buf[kbd_tail++];
        if (b == 0xE0) { ext = true; continue; }
        if (b == 0xE1) { ext = false; continue; }
        bool down = !(b & 0x80), e = ext;
        uint8_t code = b & 0x7F;
        ext = false;
        uint32_t bit = 0;
        switch (code) {
        case 0x1D: bit = e ? MOD_RCTRL : MOD_LCTRL; break;
        case 0x38: bit = e ? MOD_RALT : MOD_LALT; break;
        case 0x2A: if (!e) bit = MOD_LSHIFT; break;
        case 0x36: if (!e) bit = MOD_RSHIFT; break;
        }
        if (bit) { if (down) mods |= bit; else mods &= ~bit; continue; }
        if (e && (code == 0x2A || code == 0x36)) continue;   // fake shifts around E0 keys
        if (int32_t k = keycode(e, code)) sdl_key_event(k, down);
    }
}

// ---------------------------------------------------------------- mouse
static int buttons;

// Relative motion and button state from either mouse driver.
void mouse_push(int dx, int dy, int b) {
    if (dx || dy) {
        mouse_x += dx; mouse_y += dy;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int)fb_w) mouse_x = fb_w - 1;
        if (mouse_y >= (int)fb_h) mouse_y = fb_h - 1;
        cursor_on = true;
        sdl_mouse_motion(mouse_x, mouse_y, dx, dy);
    }
    for (int i = 0; i < 3; i++) {
        int bit = 1 << i;
        if ((b ^ buttons) & bit) {
            cursor_on = true;
            // PS/2 and USB order: left, right, middle; SDL: 1 left, 2 middle, 3 right.
            static const int sdl_button[3] = {1, 3, 2};
            sdl_mouse_button(mouse_x, mouse_y, sdl_button[i], (b & bit) != 0);
        }
    }
    buttons = b;
}

static void poll_ps2_mouse() {
    static uint8_t pkt[3];
    static int n;
    while (mouse_tail != mouse_head) {
        uint8_t b = mouse_buf[mouse_tail++];
        if (n == 0 && !(b & 0x08)) continue;           // resync: byte 0 always has bit 3 set
        pkt[n++] = b;
        if (n < 3) continue;
        n = 0;
        if (pkt[0] & 0xC0) continue;                   // overflow: drop the packet
        int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
        int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
        mouse_push(dx, -dy, pkt[0] & 7);               // PS/2 y grows upward
    }
}

// Called from every SDL wait, poll and present: input, USB and sound.
void platform_service() {
    usb_poll();
    poll_keyboard();
    poll_ps2_mouse();
    sdl_pump_audio();
}

// ---------------------------------------------------------------- main
extern "C" void (*__init_array_start[])(), (*__init_array_end[])();

static void text_line(int y, const char* s, uint32_t color) {
    sdl_draw_label(s, (int)back_w / 2, y, color);
}

extern "C" void kmain() {
    serial_init();
    printf("\nWordle - bare metal\n");
    for (auto f = __init_array_start; f != __init_array_end; f++) (*f)();

    const MultibootInfo* mbi = (const MultibootInfo*)(uintptr_t)mb_info;
    if (mb_magic != 0x2BADB002) { printf("Not booted by a Multiboot loader\n"); return; }
    static MultibootInfo info;
    info = *mbi;
    static char cmdbuf[512];
    const char* cmdline = nullptr;
    if (info.flags & (1 << 2)) {
        strncpy(cmdbuf, (const char*)(uintptr_t)info.cmdline, sizeof(cmdbuf) - 1);
        cmdline = cmdbuf;
    }
    heap_init(&info);
    if (!video_init(&info)) { printf("No usable 32-bit framebuffer found\n"); return; }

    back_w = fb_w; back_h = fb_h;
    back = (uint32_t*)malloc((size_t)back_w * back_h * 4);
    if (!back) { printf("No memory for the back buffer\n"); return; }
    memset(back, 0, (size_t)back_w * back_h * 4);
    mouse_x = fb_w / 2; mouse_y = fb_h / 2;

    audio_init(cmdline);
    usb_init(cmdline);
    ps2_mouse_init();
    interrupts_init();

    for (;;) {
        printf("Running wordle.cpp\n");
        main(0, nullptr);
        // The game quit (Esc). Offer to start again rather than rebooting
        // on a stray key press.
        printf("Game exited\n");
        for (uint32_t i = 0; i < back_w * back_h; i++) back[i] = 0x121213;
        text_line((int)back_h / 2 - 20, "WORDLE HAS QUIT", 0xFFFFFF);
        text_line((int)back_h / 2 + 16, "ENTER: PLAY AGAIN    ESC: REBOOT", 0x818384);
        platform_present();
        for (;;) {
            int32_t k = sdl_wait_key();
            if (k == 13 || k == KEY_KP_ENTER) break;
            if (k == 27) platform_reboot();
        }
    }
}

extern "C" void fault_handler() {
    printf("CPU exception - halted\n");
}
