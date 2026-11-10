// MS-DOS (DJGPP) frontend for Letterlock.
//
// Plays the part of baremetal/kernel.cpp: runs the unchanged ../letterlock.cpp
// (its main() renamed letterlock_main) on top of the bare-metal build's SDL2 /
// SDL_ttf stand-in (../baremetal/sdl.cpp), which draws into a back buffer.
// DOS and DJGPP supply the C library, files (stats in the current directory)
// and memory; this file supplies what platform.hpp and audio.hpp ask for:
//
//   video     VESA VBE 2.0 linear framebuffer (32/24/16/15/8 bpp), a banked
//             VBE 1.2 window without one, or VGA mode 12h (640x480, 16
//             colours) with no VESA BIOS. SET LETTERVID=WxHxBPP picks a mode.
//   keyboard  BIOS (INT 16h)
//   mouse     INT 33h driver (CuteMouse, MOUSE.COM, ...); the cursor is drawn here
//   sound     Sound Blaster (from BLASTER=A220 D1 H5), else the PC speaker:
//             the game's mixed stream is turned back into tones
//   time      IRQ0, sped up to ~1165 Hz (the BIOS still gets its 18.2 Hz)
#include <SDL2/SDL.h>
#include "platform.hpp"
#include "audio.hpp"

#include <crt0.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <dos.h>
#include <bios.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <sys/exceptn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

int letterlock_main(int argc, char** argv);   // ../letterlock.cpp, built with -Dmain=letterlock_main

// Everything the timer interrupt touches must stay in memory.
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

// ---------------------------------------------------------------- timer
// PIT channel 0 at 1193182 / 1024 Hz; every 64th tick goes on to the BIOS
// handler, which keeps the time of day, the floppy motor and INT 1Ch going.
static const uint32_t PIT_HZ = 1193182;
static const uint32_t TICK_DIV = 1024;

static volatile uint32_t ticks;
static _go32_dpmi_seginfo old_irq0, new_irq0;
static struct __attribute__((packed)) { uint32_t off; uint16_t sel; } old_vec;
static bool timer_on;

// PC speaker tone queue, filled by the main loop: PIT divisor (0 = silent)
// and length in PIT counts.
struct Seg { uint16_t div; int32_t counts; };
static const int QN = 1024;
static Seg q[QN];
static volatile int q_head, q_tail;
static volatile int32_t seg_left;          // counts left of the playing segment
static volatile uint16_t spk_div;          // what the speaker is playing

static inline void speaker_set(uint16_t div) {
    if (div == spk_div) return;
    uint16_t was = spk_div;
    spk_div = div;
    if (!div) { outportb(0x61, inportb(0x61) & ~3); return; }
    if (!was) outportb(0x43, 0xB6);
    outportb(0x42, div & 0xFF);
    outportb(0x42, div >> 8);
    if (!was) outportb(0x61, inportb(0x61) | 3);
}

static void timer_isr() {
    ticks++;
    if (q_head != q_tail || seg_left > 0) {
        int32_t left = seg_left - (int32_t)TICK_DIV;
        uint16_t div = spk_div;
        while (left <= 0 && q_head != q_tail) {
            left += q[q_head].counts;
            div = q[q_head].div;
            q_head = (q_head + 1) % QN;
        }
        if (left <= 0) { left = 0; div = 0; }       // ran dry: silence
        seg_left = left;
        speaker_set(div);
    }
    if ((ticks & 63) == 0)
        __asm__ __volatile__("pushfl\n\tlcall *%0" :: "m"(old_vec) : "memory", "cc");
    else
        outportb(0x20, 0x20);
}

static void pit_rate(uint32_t div) {
    outportb(0x43, 0x36);
    outportb(0x40, div & 0xFF);
    outportb(0x40, (div >> 8) & 0xFF);
}

static void timer_init() {
    _go32_dpmi_get_protected_mode_interrupt_vector(8, &old_irq0);
    old_vec.off = old_irq0.pm_offset;
    old_vec.sel = old_irq0.pm_selector;
    new_irq0.pm_offset = (unsigned long)timer_isr;
    new_irq0.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&new_irq0);
    disable();
    _go32_dpmi_set_protected_mode_interrupt_vector(8, &new_irq0);
    pit_rate(TICK_DIV);
    enable();
    timer_on = true;
}

static void timer_shutdown() {
    if (!timer_on) return;
    disable();
    pit_rate(0);                                    // 65536: 18.2 Hz
    _go32_dpmi_set_protected_mode_interrupt_vector(8, &old_irq0);
    enable();
    _go32_dpmi_free_iret_wrapper(&new_irq0);
    spk_div = 1;
    speaker_set(0);
    timer_on = false;
}

uint32_t platform_ticks() { return (uint32_t)((uint64_t)ticks * TICK_DIV * 1000 / PIT_HZ); }

// Give the time slice away (Windows, OS/2, DOSBox); a plain DPMI host just returns.
void platform_idle() { __dpmi_yield(); }

// ================================================================ sound
// audio.hpp: the SDL stand-in pulls the game's mono stream at 48 kHz and
// hands it to audio_write() as fast as audio_frames_wanted() allows.
enum DosAudio { DA_NONE, DA_SB, DA_SPEAKER };
static DosAudio adrv = DA_NONE;
static const char* aname = "none";

const char* audio_name() { return aname; }

// ---------------------------------------------------------------- Sound Blaster
// SB16: 16-bit signed mono at 44.1 kHz on the 16-bit DMA channel; SB 2.0 /
// Pro: 8-bit unsigned mono at 22 kHz. Both from an auto-initialising ring in
// conventional memory, positions read from the DMA controller (no IRQ).
static uint16_t sb_base = 0x220;
static int sb_dma = 1, sb_hdma = 5;
static bool sb16;
static uint32_t sb_rate;
#define SB_RING 16384                               // frames
static int sb_seg_sel = -1;                         // the DOS block (selector)
static uint32_t sb_phys;                            // 32 KB aligned ring, < 1 MB
static uint32_t sb_write, sb_ahead;

static const uint8_t kDmaAddr[8]  = {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
static const uint8_t kDmaCount[8] = {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};
static const uint8_t kDmaPage[8]  = {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};

static bool sb_wait_write() { for (int i = 0; i < 20000; i++) if (!(inportb(sb_base + 0xC) & 0x80)) return true; return false; }
static bool sb_wait_read()  { for (int i = 0; i < 20000; i++) if (inportb(sb_base + 0xE) & 0x80) return true; return false; }
static void sb_out(uint8_t v) { if (sb_wait_write()) outportb(sb_base + 0xC, v); }
static int sb_in() { return sb_wait_read() ? inportb(sb_base + 0xA) : -1; }
static void sb_mixer(uint8_t reg, uint8_t v) { outportb(sb_base + 4, reg); outportb(sb_base + 5, v); }

static bool sb_reset() {
    outportb(sb_base + 0x6, 1);
    for (int i = 0; i < 10; i++) inportb(0x80);       // >3 us
    outportb(sb_base + 0x6, 0);
    for (int i = 0; i < 20000; i++)
        if ((inportb(sb_base + 0xE) & 0x80) && inportb(sb_base + 0xA) == 0xAA) return true;
    return false;
}

static int sb_channel() { return sb16 ? sb_hdma : sb_dma; }

static void dma_start(int ch, uint32_t phys, uint32_t units) {
    bool hi = ch >= 4;
    uint16_t mask = hi ? 0xD4 : 0x0A, mode = hi ? 0xD6 : 0x0B, ff = hi ? 0xD8 : 0x0C;
    uint32_t addr = hi ? (phys >> 1) & 0xFFFF : phys & 0xFFFF;
    uint8_t page = hi ? (uint8_t)((phys >> 16) & 0xFE) : (uint8_t)(phys >> 16);
    outportb(mask, 0x04 | (ch & 3));
    outportb(ff, 0);
    outportb(mode, 0x58 | (ch & 3));                  // single, auto-init, read (memory -> card)
    outportb(kDmaAddr[ch], addr & 0xFF);
    outportb(kDmaAddr[ch], (addr >> 8) & 0xFF);
    outportb(kDmaPage[ch], page);
    outportb(kDmaCount[ch], (units - 1) & 0xFF);
    outportb(kDmaCount[ch], ((units - 1) >> 8) & 0xFF);
    outportb(mask, ch & 3);
}

static uint32_t sb_play_pos() {
    int ch = sb_channel();
    uint16_t ff = ch >= 4 ? 0xD8 : 0x0C;
    inportb(sb_base + (sb16 ? 0xF : 0xE));             // acknowledge the DSP's block interrupt
    uint32_t c1, c2;
    do {
        outportb(ff, 0);
        c1 = inportb(kDmaCount[ch]); c1 |= inportb(kDmaCount[ch]) << 8;
        outportb(ff, 0);
        c2 = inportb(kDmaCount[ch]); c2 |= inportb(kDmaCount[ch]) << 8;
    } while (c1 - c2 > 4 && c2 - c1 > 4);
    uint32_t left = (c2 + 1) & 0xFFFF;
    if (left > SB_RING) left = SB_RING;
    return (SB_RING - left) % SB_RING;
}

// BLASTER=A220 I5 D1 H5 T6
static bool sb_init() {
    const char* b = getenv("BLASTER");
    if (!b) return false;
    for (const char* p = b; *p; p++) {
        char c = (char)toupper((unsigned char)*p);
        if (p != b && p[-1] != ' ') continue;
        if (c == 'A') sb_base = (uint16_t)strtoul(p + 1, nullptr, 16);
        else if (c == 'D') sb_dma = atoi(p + 1) & 3;
        else if (c == 'H') sb_hdma = atoi(p + 1);
    }
    if (sb_hdma < 5 || sb_hdma > 7) sb_hdma = -1;
    if (!sb_reset()) return false;
    sb_out(0xE1);
    int major = sb_in(), minor = sb_in();
    if (major < 2) return false;                        // SB 1.x: no auto-init DMA
    sb16 = major >= 4 && sb_hdma >= 0;

    // 64 KB of conventional memory holds a 32 KB block on a 32 KB boundary,
    // which never crosses a DMA page.
    int sel;
    int seg = __dpmi_allocate_dos_memory(65536 / 16, &sel);
    if (seg < 0) return false;
    sb_seg_sel = sel;
    sb_phys = ((uint32_t)seg * 16 + 0x7FFF) & ~0x7FFFu;
    uint8_t zero[1024];
    memset(zero, sb16 ? 0 : 0x80, sizeof zero);
    for (uint32_t o = 0; o < 32768; o += sizeof zero) dosmemput(zero, sizeof zero, sb_phys + o);

    if (sb16) {
        sb_mixer(0x30, 0xF8); sb_mixer(0x31, 0xF8);
        sb_mixer(0x32, 0xF8); sb_mixer(0x33, 0xF8);
        dma_start(sb_hdma, sb_phys, SB_RING);            // words
        sb_rate = 44100;
        sb_out(0xD1);
        sb_out(0x41); sb_out(sb_rate >> 8); sb_out(sb_rate & 0xFF);
        sb_out(0xB6); sb_out(0x10);                      // 16-bit auto-init output, mono signed
        sb_out((SB_RING - 1) & 0xFF); sb_out(((SB_RING - 1) >> 8) & 0xFF);
        aname = "Sound Blaster 16";
    } else {
        if (major >= 3) { sb_mixer(0x0E, 0x00); sb_mixer(0x04, 0xFF); sb_mixer(0x22, 0xFF); }
        dma_start(sb_dma, sb_phys, SB_RING);             // bytes
        int tc = 211;
        sb_rate = 1000000 / (256 - tc);                  // 22222 Hz
        sb_out(0xD1);
        sb_out(0x40); sb_out((uint8_t)tc);
        sb_out(0x48); sb_out((SB_RING - 1) & 0xFF); sb_out(((SB_RING - 1) >> 8) & 0xFF);
        sb_out(0x1C);
        aname = "Sound Blaster";
    }
    sb_ahead = sb_rate * 50 / 1000;                       // 50 ms queued
    sb_write = (sb_play_pos() + sb_ahead) % SB_RING;
    return true;
}

static void sb_shutdown() {
    if (adrv != DA_SB) return;
    sb_out(sb16 ? 0xD5 : 0xD0);                         // pause DMA
    sb_out(sb16 ? 0xD9 : 0xDA);                         // leave auto-init
    sb_reset();
    sb_out(0xD3);
    int ch = sb_channel();
    outportb(ch >= 4 ? 0xD4 : 0x0A, 0x04 | (ch & 3));   // mask the channel
    if (sb_seg_sel >= 0) __dpmi_free_dos_memory(sb_seg_sel);
    sb_seg_sel = -1;
    adrv = DA_NONE;
}

// 48 kHz in, sb_rate out (linear interpolation for 44.1 kHz, a box filter for 22 kHz).
static uint32_t sb_phase;
static int32_t sb_prev, sb_acc, sb_accn;
static uint8_t sb_out_buf[2048];
static int sb_out_n;                                     // frames in sb_out_buf

static void sb_flush() {
    int unit = sb16 ? 2 : 1;
    int done = 0;
    while (done < sb_out_n) {
        int k = sb_out_n - done;
        if (k > (int)(SB_RING - sb_write)) k = SB_RING - sb_write;
        dosmemput(sb_out_buf + done * unit, k * unit, sb_phys + sb_write * unit);
        sb_write = (sb_write + k) % SB_RING;
        done += k;
    }
    sb_out_n = 0;
}
static inline void sb_put(int32_t v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    if (sb16) ((int16_t*)sb_out_buf)[sb_out_n++] = (int16_t)v;
    else sb_out_buf[sb_out_n++] = (uint8_t)((v >> 8) + 128);
    if (sb_out_n == 1024) sb_flush();
}
static void sb_submit(const int16_t* s, int n) {
    for (int i = 0; i < n; i++) {
        int32_t x = s[i];
        sb_phase += sb_rate;
        if (sb16) {
            if (sb_phase >= 48000) {
                sb_phase -= 48000;
                int32_t f = (int32_t)(((uint64_t)sb_phase << 16) / sb_rate);
                sb_put(x - (int32_t)(((int64_t)(x - sb_prev) * f) >> 16));
            }
            sb_prev = x;
        } else {
            sb_acc += x; sb_accn++;
            if (sb_phase >= 48000) {
                sb_phase -= 48000;
                sb_put(sb_acc / sb_accn);
                sb_acc = 0; sb_accn = 0;
            }
        }
    }
    sb_flush();
}

// ---------------------------------------------------------------- PC speaker
// The game's notes are mixed into one stream before we see it, so the
// speaker gets its pitch back by timing the rising zero crossings, and plays
// it in 10 ms steps while the stream is loud enough to be heard.
static const int WIN = 480;                              // 10 ms at 48 kHz
static uint64_t sample_no, last_cross;
static float period;                                     // samples, smoothed
static bool armed;
static int win_n, win_peak;
static uint64_t pushed_samples, pushed_counts;

static int64_t queued_counts() {
    disable();
    int64_t n = seg_left;
    for (int i = q_head; i != q_tail; i = (i + 1) % QN) n += q[i].counts;
    enable();
    return n;
}

static void q_push(uint16_t div, uint32_t samples) {
    pushed_samples += samples;
    uint64_t c = pushed_samples * PIT_HZ / 48000;
    int32_t counts = (int32_t)(c - pushed_counts);
    pushed_counts = c;
    int next = (q_tail + 1) % QN;
    if (next == q_head) return;                          // full: drop
    q[q_tail].div = div;
    q[q_tail].counts = counts;
    __asm__ __volatile__("" ::: "memory");
    q_tail = next;
}

static void speaker_submit(const int16_t* s, int n) {
    for (int i = 0; i < n; i++) {
        int x = s[i], ax = x < 0 ? -x : x;
        if (ax > win_peak) win_peak = ax;
        int hyst = win_peak / 4 > 64 ? win_peak / 4 : 64;
        if (x < -hyst) armed = true;
        else if (armed && x > hyst) {                    // a rising crossing
            armed = false;
            uint64_t p = sample_no - last_cross;
            last_cross = sample_no;
            if (p >= 12 && p <= 800) period = period > 0.f && (p > period * 0.8f && p < period * 1.25f)
                                                ? period * 0.6f + (float)p * 0.4f : (float)p;
        }
        sample_no++;
        if (++win_n == WIN) {
            uint16_t div = 0;
            if (win_peak > 700 && period > 0.f) {
                uint32_t d = (uint32_t)((float)PIT_HZ * period / 48000.f + 0.5f);
                div = d > 65535 ? 0 : (uint16_t)d;
            }
            if (win_peak <= 700) period = 0.f;
            q_push(div, WIN);
            win_n = 0;
            win_peak = 0;
        }
    }
}

// ---------------------------------------------------------------- audio.hpp
uint32_t audio_frames_wanted() {
    if (adrv == DA_SB) {
        uint32_t play = sb_play_pos();
        uint32_t ahead = (sb_write + SB_RING - play) % SB_RING;
        if (ahead > SB_RING / 2) { sb_write = play; ahead = 0; }
        return ahead < sb_ahead ? (sb_ahead - ahead) * 48000u / sb_rate : 0;
    }
    if (adrv == DA_SPEAKER) {
        static const int64_t target = PIT_HZ / 16;       // ~60 ms
        int64_t have = queued_counts();
        if (have >= target) return 0;
        return (uint32_t)((target - have) * 48000 / PIT_HZ) + 1;
    }
    return 0;
}
void audio_write(const int16_t* samples, int n) {
    if (adrv == DA_SB) sb_submit(samples, n);
    else if (adrv == DA_SPEAKER) speaker_submit(samples, n);
}
uint32_t audio_play_pos() { return adrv == DA_SB ? sb_play_pos() : 0; }

// ================================================================ video
enum VMode { V_NONE, V_LFB, V_BANKED, V_VGA12 };
static VMode vmode = V_NONE;
static uint32_t fb_w, fb_h, fb_pitch;
static int fb_bytes = 4;
static bool fb_indexed;
static uint8_t r_pos = 16, r_len = 8, g_pos = 8, g_len = 8, b_pos = 0, b_len = 8;
static int lfb_sel = -1;
static __dpmi_meminfo lfb_map;
static uint32_t win_seg = 0xA000, win_gran = 65536, win_size = 65536;
static int cur_bank = -1;

uint32_t* back;
uint32_t back_w, back_h;

// 8-bit modes: the first 192 colours get exact palette entries, the rest
// map onto a 4x4x4 cube in 192..255.
static uint32_t pal_key[1024];
static uint8_t pal_val[1024];
static int pal_n, pal_keys;
static void dac_set(int i, uint32_t c) {
    outportb(0x3C8, (uint8_t)i);
    outportb(0x3C9, (c >> 18) & 63); outportb(0x3C9, (c >> 10) & 63); outportb(0x3C9, (c >> 2) & 63);
}
static uint8_t pal_index(uint32_t c) {
    uint32_t h = (c * 2654435761u) >> 22;
    for (;; h = (h + 1) & 1023) {
        if (pal_key[h] == c) return pal_val[h];
        if (pal_key[h] == 0xFFFFFFFF) break;
    }
    uint8_t idx;
    if (pal_n < 192) { idx = (uint8_t)pal_n++; dac_set(idx, c); }
    else idx = (uint8_t)(192 + ((c >> 22) & 3) * 16 + ((c >> 14) & 3) * 4 + ((c >> 6) & 3));
    if (pal_keys < 900) { pal_key[h] = c; pal_val[h] = idx; pal_keys++; }
    return idx;
}
static void pal_reset() {
    for (auto& k : pal_key) k = 0xFFFFFFFF;
    pal_n = pal_keys = 0;
    for (int i = 0; i < 64; i++)
        dac_set(192 + i, (uint32_t)((i >> 4) * 85) << 16 | (uint32_t)(((i >> 2) & 3) * 85) << 8 | (uint32_t)((i & 3) * 85));
    pal_index(0x000000);
}

static const uint32_t ega16[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };
static uint8_t nearest16(uint32_t c) {
    static uint32_t ck[512]; static uint8_t cv[512]; static bool init;
    if (!init) { for (auto& k : ck) k = 0xFFFFFFFF; init = true; }
    uint32_t h = (c * 2654435761u) >> 23;
    if (ck[h] == c) return cv[h];
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, best = 0;
    long bd = 1L << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)((ega16[i] >> 16) & 255), dg = g - (int)((ega16[i] >> 8) & 255), db = b - (int)(ega16[i] & 255);
        long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    ck[h] = c; cv[h] = (uint8_t)best;
    return (uint8_t)best;
}

static uint32_t native(uint32_t c) {
    c &= 0xFFFFFF;
    if (fb_indexed) return pal_index(c);
    uint32_t r = c >> 16, g = (c >> 8) & 255, b = c & 255;
    return (r >> (8 - r_len)) << r_pos | (g >> (8 - g_len)) << g_pos | (b >> (8 - b_len)) << b_pos;
}

static void set_bank(int bank) {
    if (bank == cur_bank) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F05; r.x.bx = 0; r.x.dx = (unsigned short)bank;
    __dpmi_int(0x10, &r);
    cur_bank = bank;
}

static void fb_write(uint32_t off, const void* src, uint32_t n) {
    if (vmode == V_LFB) { movedata(_my_ds(), (unsigned)src, lfb_sel, off, n); return; }
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        int bank = (int)(off / win_gran);
        uint32_t in = off - (uint32_t)bank * win_gran;
        uint32_t room = win_size - in;
        uint32_t k = n < room ? n : room;
        set_bank(bank);
        dosmemput(s, k, win_seg * 16 + in);
        off += k; s += k; n -= k;
    }
}

static void vga12_row(uint32_t y, const uint32_t* src) {
    uint8_t planes[4][80];
    memset(planes, 0, sizeof planes);
    uint32_t last = 0xFFFFFFFF; uint8_t v = 0;
    for (uint32_t x = 0; x < 640; x++) {
        uint32_t p = src[x] & 0xFFFFFF;
        if (p != last) { last = p; v = nearest16(p); }
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        for (int pl = 0; pl < 4; pl++) if (v & (1 << pl)) planes[pl][x >> 3] |= bit;
    }
    for (int pl = 0; pl < 4; pl++) {
        outportw(0x3C4, (uint16_t)(((1 << pl) << 8) | 0x02));
        dosmemput(planes[pl], 80, 0xA0000 + y * 80);
    }
    outportw(0x3C4, 0x0F02);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool vbe_mode_info(int mode, uint8_t mi[256]) {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F01; r.x.cx = (unsigned short)mode;
    r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, 256, mi);
    return true;
}

static bool vesa_init() {
    uint8_t info[512];
    memset(info, 0, sizeof info);
    memcpy(info, "VBE2", 4);
    dosmemput(info, sizeof info, __tb);
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F00; r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, sizeof info, info);
    if (memcmp(info, "VESA", 4) != 0) return false;
    int ver = rd16(info + 4);
    uint32_t lp = rd32(info + 14);
    uint32_t list_lin = ((lp >> 16) << 4) + (lp & 0xFFFF);
    uint16_t modes[256];
    int nm = 0;
    for (; nm < 255; nm++) {
        uint16_t m = _farpeekw(_dos_ds, list_lin + nm * 2);
        if (m == 0xFFFF) break;
        modes[nm] = m;
    }

    // LETTERVID=WxHxBPP, otherwise 1024x768: the game's 680x720 window at
    // full size (smaller screens shrink it). Anti-aliased text wants real
    // colour, so 8 bits a pixel comes last.
    int want_w = 1024, want_h = 768, want_bpp = 0;
    if (const char* e = getenv("LETTERVID")) sscanf(e, "%dx%dx%d", &want_w, &want_h, &want_bpp);
    static const int pref[] = {32, 16, 15, 24, 8};
    int best = -1, best_score = -1;
    uint8_t mi[256];
    for (int i = 0; i < nm; i++) {
        if (!vbe_mode_info(modes[i], mi)) continue;
        uint16_t attr = rd16(mi);
        if ((attr & 0x19) != 0x19) continue;            // supported, colour, graphics
        int w = rd16(mi + 18), h = rd16(mi + 20), bpp = mi[25], model = mi[27];
        if (w < 640 || h < 480) continue;
        if (bpp == 8 ? model != 4 : model != 6) continue;
        bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
        if (!lfb && (mi[2] & 5) != 5) continue;
        int score = 0;
        if (w == want_w && h == want_h) score += 1000;
        else score += 500 - abs(w - want_w) / 8 - abs(h - want_h) / 8 + (h >= 720 ? 50 : 0);
        if (want_bpp) { if (bpp == want_bpp) score += 100; }
        else for (int k = 0; k < 5; k++) if (pref[k] == bpp) score += 100 - k * 10;
        if (lfb) score += 5;
        if (score > best_score) { best_score = score; best = modes[i]; }
    }
    if (best < 0 || !vbe_mode_info(best, mi)) return false;

    uint16_t attr = rd16(mi);
    fb_w = rd16(mi + 18); fb_h = rd16(mi + 20);
    int bpp = mi[25];
    fb_bytes = (bpp + 7) / 8;
    fb_indexed = bpp == 8;
    fb_pitch = rd16(mi + 16);
    if (!fb_indexed) {
        r_len = mi[31]; r_pos = mi[32]; g_len = mi[33]; g_pos = mi[34]; b_len = mi[35]; b_pos = mi[36];
        if (!r_len || !g_len || !b_len) {
            if (bpp == 15) { r_pos = 10; r_len = 5; g_pos = 5; g_len = 5; b_pos = 0; b_len = 5; }
            else if (bpp == 16) { r_pos = 11; r_len = 5; g_pos = 5; g_len = 6; b_pos = 0; b_len = 5; }
            else { r_pos = 16; r_len = 8; g_pos = 8; g_len = 8; b_pos = 0; b_len = 8; }
        }
    }
    bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
    if (lfb) {
        if (ver >= 0x300 && rd16(mi + 50)) fb_pitch = rd16(mi + 50);
        lfb_map.address = rd32(mi + 40);
        lfb_map.size = (fb_pitch * fb_h + 0xFFF) & ~0xFFFu;
        if (__dpmi_physical_address_mapping(&lfb_map) != 0) lfb = false;
        else {
            lfb_sel = __dpmi_allocate_ldt_descriptors(1);
            if (lfb_sel < 0) lfb = false;
            else {
                __dpmi_set_segment_base_address(lfb_sel, lfb_map.address);
                __dpmi_set_segment_limit(lfb_sel, (lfb_map.size - 1) | 0xFFF);
            }
        }
    }
    if (!lfb) {
        if ((mi[2] & 5) != 5) return false;
        win_gran = (uint32_t)rd16(mi + 4) * 1024;
        win_size = (uint32_t)rd16(mi + 6) * 1024;
        win_seg = rd16(mi + 8);
        if (!win_gran || !win_size || !win_seg) return false;
    }
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F02; r.x.bx = (unsigned short)(best | (lfb ? 0x4000 : 0));
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    vmode = lfb ? V_LFB : V_BANKED;
    cur_bank = -1;
    return true;
}

static void vga12_init() {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0012;
    __dpmi_int(0x10, &r);
    fb_w = 640; fb_h = 480; fb_pitch = 80; fb_bytes = 1; fb_indexed = false;
    vmode = V_VGA12;
}

static void text_mode() {
    if (vmode == V_NONE) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
    vmode = V_NONE;
}

// ---------------------------------------------------------------- presenting
// Rows go to the screen only when they changed (per-row checksum). The mouse
// cursor is laid over the rows on their way out, never into the back buffer.
static const char* const CURSOR[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ",
    "      X..X  ", "       XX   ",
};
static const int CURSOR_H = 20, CURSOR_W = 12;
static bool have_mouse, cursor_on;
static int mouse_x, mouse_y;                        // screen pixels
static int shown_x = -1, shown_y = -1;              // where the cursor is drawn now
static uint32_t* row_sum;
static uint32_t* line32;                            // a row with the cursor laid over it
static uint8_t* line;                               // that row in the screen's format

static void write_row(uint32_t y) {
    const uint32_t* src = &back[y * back_w];
    if (cursor_on && (int)y >= mouse_y && (int)y < mouse_y + CURSOR_H) {
        memcpy(line32, src, back_w * 4);
        const char* c = CURSOR[y - mouse_y];
        for (int x = 0; x < CURSOR_W && mouse_x + x < (int)back_w; x++)
            if (c[x] == 'X') line32[mouse_x + x] = 0x000000;
            else if (c[x] == '.') line32[mouse_x + x] = 0xFFFFFF;
        src = line32;
    }
    if (vmode == V_VGA12) { vga12_row(y, src); return; }
    if (fb_bytes == 4 && !fb_indexed && r_pos == 16 && g_pos == 8 && b_pos == 0) {
        fb_write(y * fb_pitch, src, back_w * 4);
        return;
    }
    uint8_t* d = line;
    uint32_t last = 0xFFFFFFFF, v = 0;
    for (uint32_t x = 0; x < back_w; x++) {
        uint32_t p = src[x];
        if (p != last) { last = p; v = native(p); }
        switch (fb_bytes) {
        case 1: *d++ = (uint8_t)v; break;
        case 2: *(uint16_t*)d = (uint16_t)v; d += 2; break;
        case 3: d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d += 3; break;
        default: *(uint32_t*)d = v; d += 4; break;
        }
    }
    fb_write(y * fb_pitch, line, back_w * fb_bytes);
}

static void rewrite_rows(int y0) {                   // the CURSOR_H rows from y0
    for (int y = y0; y < y0 + CURSOR_H && y < (int)back_h; y++)
        if (y >= 0) write_row(y);
}

// Move the cursor on screen to where the mouse is.
static void update_cursor() {
    if (!cursor_on || (mouse_x == shown_x && mouse_y == shown_y)) return;
    int old_y = shown_y;
    shown_x = mouse_x; shown_y = mouse_y;
    if (old_y >= 0) rewrite_rows(old_y);
    rewrite_rows(mouse_y);
}

static inline uint32_t row_checksum(const uint32_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void platform_present() {
    if (!row_sum) {
        row_sum = (uint32_t*)malloc(back_h * sizeof(uint32_t));
        for (uint32_t y = 0; y < back_h; y++) row_sum[y] = ~row_checksum(&back[y * back_w], back_w);
    }
    for (uint32_t y = 0; y < back_h; y++) {
        if ((y & 63) == 63) sdl_pump_audio();
        uint32_t s = row_checksum(&back[y * back_w], back_w);
        if (s == row_sum[y]) continue;
        row_sum[y] = s;
        write_row(y);
    }
    update_cursor();
}

// ---------------------------------------------------------------- keyboard
static uint32_t mods;
uint32_t platform_mods() { return mods; }

static void read_mods() {
    int f = bioskey(0x12);                          // extended shift state
    mods = 0;
    if (f & 1) mods |= MOD_RSHIFT;
    if (f & 2) mods |= MOD_LSHIFT;
    if (f & 0x100) mods |= MOD_LCTRL;
    if (f & 0x200) mods |= MOD_LALT;
    if (f & 0x400) mods |= MOD_RCTRL;
    if (f & 0x800) mods |= MOD_RALT;
    if ((f & 4) && !(f & 0x500)) mods |= MOD_LCTRL;  // no extended bits (old BIOS)
    if ((f & 8) && !(f & 0xA00)) mods |= MOD_LALT;
}

// INT 16h AH=10h: scan code (high byte) and ASCII (low). The scan codes are
// set 1's, so letters keep their key with Shift, Ctrl or Alt held.
static int32_t keycode(int k) {
    uint8_t scan = (uint8_t)(k >> 8), ascii = (uint8_t)k;
    if (scan == 0xE0) return ascii == 13 ? KEY_KP_ENTER : ascii;   // keypad Enter, keypad /
    switch (scan) {
    case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;  case 0x4D: return KEY_RIGHT;
    case 0x85: return KEY_F1 + 10;  case 0x86: return KEY_F1 + 11;   // F11, F12
    case 0x01: return 27;  case 0x0E: return 8;  case 0x0F: return 9;
    case 0x1C: return 13;  case 0x39: return ' ';
    case 0x35: return '/';
    }
    if (scan >= 0x3B && scan <= 0x44) return KEY_F1 + (scan - 0x3B);
    static const char row1[] = "1234567890-=";          // 0x02..0x0D
    static const char row2[] = "qwertyuiop[]";          // 0x10..0x1B
    static const char row3[] = "asdfghjkl;'`";          // 0x1E..0x29
    static const char row4[] = "\\zxcvbnm,./";          // 0x2B..0x35
    if (scan >= 0x02 && scan <= 0x0D) return row1[scan - 0x02];
    if (scan >= 0x10 && scan <= 0x1B) return row2[scan - 0x10];
    if (scan >= 0x1E && scan <= 0x29) return row3[scan - 0x1E];
    if (scan >= 0x2B && scan <= 0x35) return row4[scan - 0x2B];
    if (ascii >= 'A' && ascii <= 'Z') return ascii - 'A' + 'a';   // Alt+numpad and the like
    return ascii >= 32 && ascii < 127 ? ascii : 0;
}

static void poll_keyboard() {
    // Look at the BIOS buffer's head and tail before calling INT 16h.
    while (_farpeekw(_dos_ds, 0x41A) != _farpeekw(_dos_ds, 0x41C) && bioskey(0x11)) {
        int k = bioskey(0x10);
        read_mods();
        if (int32_t sym = keycode(k)) { sdl_key_event(sym, true); sdl_key_event(sym, false); }
    }
}

// ---------------------------------------------------------------- mouse
static int buttons;

static bool mouse_init() {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0000;
    __dpmi_int(0x33, &r);
    if (r.x.ax != 0xFFFF) return false;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0007; r.x.cx = 0; r.x.dx = (unsigned short)(back_w - 1);
    __dpmi_int(0x33, &r);
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0008; r.x.cx = 0; r.x.dx = (unsigned short)(back_h - 1);
    __dpmi_int(0x33, &r);
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0004; r.x.cx = (unsigned short)(back_w / 2); r.x.dx = (unsigned short)(back_h / 2);
    __dpmi_int(0x33, &r);
    mouse_x = back_w / 2; mouse_y = back_h / 2;
    return true;
}

static void poll_mouse() {
    if (!have_mouse) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x33, &r);
    int x = (int16_t)r.x.cx, y = (int16_t)r.x.dx, b = r.x.bx & 7;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)back_w) x = back_w - 1;
    if (y >= (int)back_h) y = back_h - 1;
    if (x != mouse_x || y != mouse_y) {
        int dx = x - mouse_x, dy = y - mouse_y;
        mouse_x = x; mouse_y = y;
        cursor_on = true;
        sdl_mouse_motion(x, y, dx, dy);
    }
    for (int i = 0; i < 3; i++) {
        int bit = 1 << i;
        if ((b ^ buttons) & bit) {
            cursor_on = true;
            static const int sdl_button[3] = {1, 3, 2};   // driver: left, right, middle
            sdl_mouse_button(mouse_x, mouse_y, sdl_button[i], (b & bit) != 0);
        }
    }
    buttons = b;
}

// Called from every SDL wait, poll and present: input and sound.
void platform_service() {
    poll_keyboard();
    poll_mouse();
    update_cursor();
    sdl_pump_audio();
}

void platform_reboot() { exit(0); }                  // DOS: Esc just goes back to the prompt

// ---------------------------------------------------------------- start / exit
static void restore_all() {
    sb_shutdown();
    timer_shutdown();
    text_mode();
    if (lfb_sel >= 0) { __dpmi_free_ldt_descriptor(lfb_sel); lfb_sel = -1; }
    if (lfb_map.size) { __dpmi_free_physical_address_mapping(&lfb_map); lfb_map.size = 0; }
    freopen("CON", "w", stdout);
}
static void on_fault(int sig) {
    restore_all();
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char** argv) {
    bool no_sound = false, speaker = false, vga = false;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (*a == '/' || *a == '-') a++;
        if (!strcasecmp(a, "nosound") || !strcasecmp(a, "q")) no_sound = true;
        else if (!strcasecmp(a, "speaker")) speaker = true;
        else if (!strcasecmp(a, "vga")) vga = true;
        else {
            printf("LETTERLK [/NOSOUND] [/SPEAKER] [/VGA]\n"
                   "  /NOSOUND  no sound\n"
                   "  /SPEAKER  PC speaker even with a Sound Blaster (BLASTER=...)\n"
                   "  /VGA      16-colour VGA, skip VESA\n"
                   "  SET LETTERVID=800x600x16 picks a VESA mode (default 1024x768)\n\n"
                   "In the game: type a word, Enter guesses, F1 mutes, F2 statistics,\n"
                   "F3 or ? a hint, 4-9 switch word length, Esc quits.\n");
            return 0;
        }
    }
    if (const char* e = getenv("LETTERVID")) if (!strcasecmp(e, "vga")) vga = true;

    __djgpp_set_ctrl_c(0);
    _go32_want_ctrl_break(1);

    if (!no_sound) {
        if (!speaker && sb_init()) adrv = DA_SB;
        else { adrv = DA_SPEAKER; aname = "PC speaker"; }
    }

    if (vga || !vesa_init()) vga12_init();
    if (fb_indexed) pal_reset();
    atexit(restore_all);
    signal(SIGSEGV, on_fault); signal(SIGFPE, on_fault); signal(SIGILL, on_fault);
    signal(SIGABRT, on_fault);
    freopen("NUL", "w", stdout);                    // the game's log lines would scribble on the screen

    back_w = fb_w; back_h = fb_h;
    back = (uint32_t*)malloc((size_t)back_w * back_h * 4);
    line32 = (uint32_t*)malloc((size_t)back_w * 4);
    line = (uint8_t*)malloc((size_t)back_w * 4);
    if (!back || !line32 || !line) { restore_all(); printf("Not enough memory for the screen.\n"); return 1; }
    memset(back, 0, (size_t)back_w * back_h * 4);
    have_mouse = mouse_init();

    timer_init();
    char name[] = "letterlk";
    char* args[] = {name, nullptr};
    int rc = letterlock_main(1, args);
    restore_all();
    if (rc) printf("Letterlock stopped with an error (%d).\n", rc);
    else printf("Thanks for playing Letterlock!\n");
    return rc;
}
