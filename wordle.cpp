// Wordle in C++ / SDL2 + SDL_ttf, using the embedded DejaVu Sans Mono font.
// Guesses are checked against words.h (all 5-letter words in linux.words, from gen_words.sh);
// answers come from answers.h (the most common of those, from gen_answers.py).
// Build: make   (or: sh gen_words.sh > words.h && g++ -std=c++17 -O2 wordle.cpp -o wordle $(sdl2-config --cflags --libs) -lSDL2_ttf)

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <map>
#include <tuple>
#include "DejaVuMono.h"
#include "words.h"
#include "answers.h"
#include <array>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

enum Mark { EMPTY, PENDING, GRAY, YELLOW, GREEN };

// ---------- Font ----------
static std::vector<unsigned char> decodeBase64(const char* in, size_t len) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(len * 3 / 4);
    unsigned buf = 0; int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        int v = val(in[i]);
        if (v < 0) continue;  // skips '=', whitespace
        buf = (buf << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((unsigned char)((buf >> bits) & 0xFF)); }
    }
    return out;
}

struct TextRenderer {
    std::vector<unsigned char> data;  // must outlive the fonts
    std::map<int, TTF_Font*> fonts;
    std::map<std::tuple<int, std::string, Uint32>, SDL_Texture*> cache;

    bool init() {
        data = decodeBase64(DEJAVU_REGULAR_FONT_B64, DEJAVU_REGULAR_FONT_B64_SIZE);
        return data.size() == DEJAVU_REGULAR_FONT_ORIGINAL_SIZE;
    }
    TTF_Font* font(int pt) {
        TTF_Font*& f = fonts[pt];
        if (!f) f = TTF_OpenFontRW(SDL_RWFromConstMem(data.data(), (int)data.size()), 1, pt);
        return f;
    }
    void shutdown() {
        for (auto& kv : cache) SDL_DestroyTexture(kv.second);
        for (auto& kv : fonts) if (kv.second) TTF_CloseFont(kv.second);
        cache.clear(); fonts.clear();
    }
} g_text;

// Draws text centered at (cx, cy); vertical centering is on the capital letters.
static void drawText(SDL_Renderer* r, const std::string& s, int cx, int cy, int pt, SDL_Color col,
                     float sx = 1.f, float sy = 1.f) {
    if (s.empty()) return;
    TTF_Font* f = g_text.font(pt);
    if (!f) return;
    auto key = std::make_tuple(pt, s, (Uint32)(col.r << 16 | col.g << 8 | col.b));
    SDL_Texture*& tex = g_text.cache[key];
    if (!tex) {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return;
        tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
        if (!tex) return;
    }
    int w, h, minx, maxx, miny, capH;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    TTF_GlyphMetrics(f, 'H', &minx, &maxx, &miny, &capH, nullptr);
    SDL_Rect dst{cx - w / 2, cy - (TTF_FontAscent(f) - capH / 2), w, h};
    if (sx != 1.f || sy != 1.f) {  // scale about (cx, cy)
        dst = {cx + (int)std::lround((dst.x - cx) * sx), cy + (int)std::lround((dst.y - cy) * sy),
               (int)std::lround(w * sx), (int)std::lround(h * sy)};
        if (dst.w <= 0 || dst.h <= 0) return;
    }
    SDL_RenderCopy(r, tex, nullptr, &dst);
}

static void setColor(SDL_Renderer* r, SDL_Color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); }

// ---------- Colors ----------
static const SDL_Color BG{18, 18, 19, 255}, WHITE{255, 255, 255, 255}, BORDER{58, 58, 60, 255},
    BORDER_ACTIVE{86, 87, 88, 255}, C_GRAY{58, 58, 60, 255}, C_YELLOW{181, 159, 59, 255},
    C_GREEN{83, 141, 78, 255}, KEY_DEFAULT{129, 131, 132, 255};

static SDL_Color markColor(Mark m) {
    switch (m) {
        case GREEN: return C_GREEN;
        case YELLOW: return C_YELLOW;
        case GRAY: return C_GRAY;
        default: return KEY_DEFAULT;
    }
}

// ---------- Animation timing (ms) ----------
constexpr float PI = 3.14159265f;
constexpr int FLIP_MS = 500, FLIP_STAGGER = 300;   // tile reveal
constexpr int POP_MS = 100;                         // letter typed
constexpr int SHAKE_MS = 600;                       // invalid guess
constexpr int BOUNCE_MS = 500, BOUNCE_STAGGER = 100; // win celebration

// Returns true while an animation started at `start` (0 = never) is running; t = elapsed ms.
static bool running(Uint32 start, Uint32 dur, Uint32 now, Uint32& t) {
    if (!start) return false;
    t = now - start;
    return t < dur;
}

// ---------- Sound (synthesized at runtime; no audio files) ----------
enum Tone { BELL, SOFT, TICK, BUZZ };
struct Voice { Tone tone; float freq, amp; int delay, pos, len; };

struct Audio {
    SDL_AudioDeviceID dev = 0;
    int rate = 44100;
    std::vector<Voice> voices;  // touched by the audio thread; lock before changing
    bool muted = false;

    bool init() {
        SDL_AudioSpec want{}, have{};
        want.freq = 44100;
        want.format = AUDIO_F32SYS;
        want.channels = 1;
        want.samples = 512;
        want.callback = [](void* u, Uint8* buf, int bytes) {
            static_cast<Audio*>(u)->mix(reinterpret_cast<float*>(buf), bytes / (int)sizeof(float));
        };
        want.userdata = this;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts to the device format
        if (!dev) return false;
        rate = have.freq;
        SDL_PauseAudioDevice(dev, 0);
        return true;
    }
    void shutdown() { if (dev) SDL_CloseAudioDevice(dev); dev = 0; }

    // Schedules a note `delayMs` from now, so sounds line up exactly with the animations.
    void play(Tone t, float freq, float amp, int delayMs, int lenMs) {
        if (!dev || muted) return;
        SDL_LockAudioDevice(dev);
        voices.push_back({t, freq, amp, delayMs * rate / 1000, 0, lenMs * rate / 1000});
        SDL_UnlockAudioDevice(dev);
    }
    void stopAll() {
        if (!dev) return;
        SDL_LockAudioDevice(dev);
        voices.clear();
        SDL_UnlockAudioDevice(dev);
    }

    static float sample(const Voice& v, float t) {
        float attack = std::min(1.f, t / 0.004f);
        float w = 2.f * PI * v.freq * t;
        switch (v.tone) {
            case BELL:  // bright chime: decaying partials, slightly inharmonic 3rd for shimmer
                return v.amp * attack * (std::sin(w) * std::exp(-t / 0.5f) +
                                         0.35f * std::sin(2.f * w) * std::exp(-t / 0.22f) +
                                         0.12f * std::sin(3.01f * w) * std::exp(-t / 0.1f));
            case SOFT:  // mellow, rounder tone
                return v.amp * attack * (std::sin(w) + 0.15f * std::sin(2.f * w)) * std::exp(-t / 0.22f);
            case TICK: {  // short wooden tap with a quick downward pitch bend
                float phase = 2.f * PI * v.freq * (t + 0.02f * (1.f - std::exp(-t / 0.01f)));
                return v.amp * attack * std::sin(phase) * std::exp(-t / 0.035f);
            }
            case BUZZ:  // low, soft "nope"
                return v.amp * attack * (std::sin(w) + std::sin(3.f * w) / 3.f + std::sin(5.f * w) / 5.f) *
                       std::exp(-t / 0.15f);
        }
        return 0.f;
    }

    void mix(float* out, int n) {
        int fade = rate / 100;  // 10 ms release so notes never click off
        for (int i = 0; i < n; ++i) {
            float acc = 0.f;
            for (Voice& v : voices) {
                if (v.delay > 0) { --v.delay; continue; }
                if (v.pos >= v.len) continue;
                float rel = std::min(1.f, (v.len - v.pos) / (float)fade);
                acc += sample(v, v.pos / (float)rate) * rel;
                ++v.pos;
            }
            out[i] = std::tanh(acc) * 0.6f;  // soft-limit so chords never clip
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(),
                                    [](const Voice& v) { return v.delay <= 0 && v.pos >= v.len; }),
                     voices.end());
    }
} g_audio;

// Notes (Hz). Green climbs a C-major pentatonic across the row; yellow uses the octave below.
static const float GREEN_NOTES[5]  = {523.25f, 587.33f, 659.25f, 783.99f, 880.00f};   // C5 D5 E5 G5 A5
static const float YELLOW_NOTES[5] = {261.63f, 293.66f, 329.63f, 392.00f, 440.00f};   // C4 D4 E4 G4 A4
static const float WIN_NOTES[5]    = {523.25f, 659.25f, 783.99f, 1046.50f, 1318.51f}; // C5 E5 G5 C6 E6
static const float LOSE_NOTES[3]   = {392.00f, 311.13f, 261.63f};                     // G4 Eb4 C4

// ---------- Game logic ----------
struct Game {
    std::vector<std::string> answers;
    std::unordered_set<std::string> valid;

    std::string answer, cur;
    std::array<std::string, 6> guesses;
    std::array<std::array<Mark, 5>, 6> marks{};
    std::array<Mark, 26> keys{};
    int row = 0;
    bool over = false, won = false;
    std::string msg;
    Uint32 msgUntil = 0;

    // Animation state
    int revealRow = -1;              // row currently flipping, -1 = none
    Uint32 revealStart = 0, popStart = 0, shakeStart = 0, bounceStart = 0;
    int popCol = -1;
    std::array<Mark, 26> pendingKeys{};  // keyboard colors applied after the flip finishes

    Uint32 revealEnd() const { return revealStart + 4 * FLIP_STAGGER + FLIP_MS; }
    void update(Uint32 now) {
        if (revealRow >= 0 && now >= revealEnd()) {
            revealRow = -1;
            keys = pendingKeys;
            if (won) bounceStart = now;
        }
    }
    bool animating(Uint32 now) const {
        Uint32 t;
        return revealRow >= 0 || running(popStart, POP_MS, now, t) || running(shakeStart, SHAKE_MS, now, t) ||
               running(bounceStart, 4 * BOUNCE_STAGGER + BOUNCE_MS, now, t);
    }
    std::mt19937 rng{std::random_device{}()};

    void loadWords() {
        for (size_t i = 0; i < VALID_WORDS_COUNT; ++i) {
            std::string w = VALID_WORDS[i];
            for (char& c : w) c = (char)std::toupper((unsigned char)c);
            valid.insert(w);
        }
        // Answers: the most common words, and only ones the dictionary accepts.
        for (size_t i = 0; i < ANSWER_WORDS_COUNT; ++i) {
            std::string w = ANSWER_WORDS[i];
            for (char& c : w) c = (char)std::toupper((unsigned char)c);
            if (valid.count(w)) answers.push_back(w);
        }
        if (answers.empty()) answers.assign(valid.begin(), valid.end());
    }

    void reset() {
        g_audio.stopAll();
        answer = answers[std::uniform_int_distribution<size_t>(0, answers.size() - 1)(rng)];
        for (auto& g : guesses) g.clear();
        for (auto& m : marks) m.fill(EMPTY);
        keys.fill(EMPTY);
        cur.clear();
        row = 0; over = won = false; msgUntil = 0;
        revealRow = -1; revealStart = popStart = shakeStart = bounceStart = 0; popCol = -1;
    }

    void flash(const std::string& m) {
        msg = m;
        msgUntil = SDL_GetTicks() + 1500;
        shakeStart = SDL_GetTicks();
        g_audio.play(BUZZ, 98.f, 0.18f, 0, 220);
    }

    void submit() {
        std::array<Mark, 5> res; res.fill(GRAY);
        int counts[26] = {};
        for (int i = 0; i < 5; ++i) {
            if (cur[i] == answer[i]) res[i] = GREEN;
            else counts[answer[i] - 'A']++;
        }
        for (int i = 0; i < 5; ++i)
            if (res[i] != GREEN && counts[cur[i] - 'A'] > 0) { res[i] = YELLOW; counts[cur[i] - 'A']--; }

        pendingKeys = keys;
        for (int i = 0; i < 5; ++i) {
            Mark& k = pendingKeys[cur[i] - 'A'];
            if (res[i] > k) k = res[i];  // GREEN > YELLOW > GRAY
        }
        // Sounds, timed to each tile's flip midpoint (when its color appears).
        for (int i = 0; i < 5; ++i) {
            int at = i * FLIP_STAGGER + FLIP_MS / 2;
            if (res[i] == GREEN)       g_audio.play(BELL, GREEN_NOTES[i], 0.32f, at, 1200);
            else if (res[i] == YELLOW) g_audio.play(SOFT, YELLOW_NOTES[i], 0.30f, at, 700);
            else                       g_audio.play(TICK, 150.f, 0.30f, at, 150);
        }
        int end = 4 * FLIP_STAGGER + FLIP_MS;
        if (cur == answer) {  // rising arpeggio in step with the tile bounce, then a sustained chord
            for (int i = 0; i < 5; ++i) g_audio.play(BELL, WIN_NOTES[i], 0.26f, end + i * BOUNCE_STAGGER, 1000);
            for (float f : {523.25f, 659.25f, 783.99f, 1046.50f})
                g_audio.play(BELL, f, 0.16f, end + 5 * BOUNCE_STAGGER + 100, 2200);
        } else if (row == 5) {  // last guess missed: gentle descending minor phrase
            for (int i = 0; i < 3; ++i) g_audio.play(SOFT, LOSE_NOTES[i], 0.28f, end + 200 + i * 260, 900);
        }

        guesses[row] = cur;
        marks[row] = res;
        revealRow = row;
        revealStart = SDL_GetTicks();
        ++row;
        if (cur == answer) { over = won = true; }
        else if (row == 6) { over = true; }
        cur.clear();
    }

    void input(char c) {
        if (revealRow >= 0) return;  // ignore input while tiles are flipping
        if (over) { if (c == '\n') reset(); return; }
        if (c >= 'A' && c <= 'Z') {
            if (cur.size() < 5) { cur += c; popCol = (int)cur.size() - 1; popStart = SDL_GetTicks(); }
        }
        else if (c == '\b') { if (!cur.empty()) cur.pop_back(); }
        else if (c == '\n') {
            if (cur.size() < 5) flash("NOT ENOUGH LETTERS");
            else if (!valid.count(cur)) flash("NOT IN WORD LIST");
            else submit();
        }
    }
};

// ---------- Layout ----------
constexpr int WIN_W = 500, WIN_H = 720;
constexpr int TILE = 62, GAP = 6;
constexpr int GRID_X = (WIN_W - (5 * TILE + 4 * GAP)) / 2, GRID_Y = 60;
constexpr int KEY_W = 40, KEY_H = 52, KEY_GAP = 6, WIDE_W = 62, KB_Y = 530;

struct Key { SDL_Rect r; std::string label; char c; };

static std::vector<Key> buildKeyboard() {
    std::vector<Key> keys;
    const char* rows[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int ri = 0; ri < 3; ++ri) {
        std::string s = rows[ri];
        int n = (int)s.size();
        int width = n * KEY_W + (n - 1) * KEY_GAP;
        if (ri == 2) width += 2 * (WIDE_W + KEY_GAP);
        int x = (WIN_W - width) / 2, y = KB_Y + ri * (KEY_H + KEY_GAP);
        if (ri == 2) { keys.push_back({{x, y, WIDE_W, KEY_H}, "ENTER", '\n'}); x += WIDE_W + KEY_GAP; }
        for (char ch : s) { keys.push_back({{x, y, KEY_W, KEY_H}, std::string(1, ch), ch}); x += KEY_W + KEY_GAP; }
        if (ri == 2) keys.push_back({{x, y, WIDE_W, KEY_H}, "DEL", '\b'});
    }
    return keys;
}

static void drawOutline(SDL_Renderer* r, SDL_Rect rc, SDL_Color c) {
    setColor(r, c);
    SDL_RenderDrawRect(r, &rc);
    SDL_Rect in{rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2};
    SDL_RenderDrawRect(r, &in);
}

static void render(SDL_Renderer* r, const Game& g, const std::vector<Key>& kb) {
    setColor(r, BG);
    SDL_RenderClear(r);
    drawText(r, "WORDLE", WIN_W / 2, 30, 34, WHITE);

    // Board
    Uint32 now = SDL_GetTicks(), t;
    for (int row = 0; row < 6; ++row) {
        bool typingRow = row == g.row && !g.over;
        std::string s = row < g.row ? g.guesses[row] : (typingRow ? g.cur : "");

        int shakeX = 0;  // horizontal wobble on invalid guess
        if (typingRow && running(g.shakeStart, SHAKE_MS, now, t))
            shakeX = (int)(std::sin(t * 0.07f) * 9.f * (1.f - t / (float)SHAKE_MS));

        for (int col = 0; col < 5; ++col) {
            int cx = GRID_X + col * (TILE + GAP) + TILE / 2 + shakeX;
            int cy = GRID_Y + row * (TILE + GAP) + TILE / 2;
            Mark m = row < g.row ? g.marks[row][col] : (col < (int)s.size() ? PENDING : EMPTY);
            float sx = 1.f, sy = 1.f;

            if (row == g.revealRow) {  // flip: squash to 0 height, swap color at midpoint, expand
                int ft = (int)(now - g.revealStart) - col * FLIP_STAGGER;
                if (ft < FLIP_MS / 2) m = PENDING;
                if (ft > 0 && ft < FLIP_MS) sy = std::fabs(std::cos(ft / (float)FLIP_MS * PI));
            }
            if (typingRow && col == g.popCol && running(g.popStart, POP_MS, now, t))
                sx = sy = 1.f + 0.12f * std::sin(t / (float)POP_MS * PI);
            if (g.won && row == g.row - 1 && g.revealRow < 0 && g.bounceStart) {
                int bt = (int)(now - g.bounceStart) - col * BOUNCE_STAGGER;
                if (bt > 0 && bt < BOUNCE_MS) cy -= (int)(std::sin(bt / (float)BOUNCE_MS * PI) * 24.f);
            }

            int w = (int)std::lround(TILE * sx), h = (int)std::lround(TILE * sy);
            if (h < 1) continue;
            SDL_Rect rc{cx - w / 2, cy - h / 2, w, h};
            if (m == EMPTY || m == PENDING) drawOutline(r, rc, m == PENDING ? BORDER_ACTIVE : BORDER);
            else { setColor(r, markColor(m)); SDL_RenderFillRect(r, &rc); }
            if (col < (int)s.size())
                drawText(r, std::string(1, s[col]), cx, cy, 36, WHITE, sx, sy);
        }
    }

    // Messages
    if (g.over && g.revealRow < 0) {
        static const char* praise[6] = {"GENIUS", "MAGNIFICENT", "IMPRESSIVE", "SPLENDID", "GREAT", "PHEW"};
        drawText(r, g.won ? praise[g.row - 1] : "ANSWER " + g.answer, WIN_W / 2, 480, 18, WHITE);
        drawText(r, "PRESS ENTER TO PLAY AGAIN", WIN_W / 2, 506, 15, KEY_DEFAULT);
    } else if (SDL_GetTicks() < g.msgUntil) {
        drawText(r, g.msg, WIN_W / 2, 490, 18, WHITE);
    }

    // Keyboard
    for (const Key& k : kb) {
        Mark m = (k.c >= 'A' && k.c <= 'Z') ? g.keys[k.c - 'A'] : EMPTY;
        setColor(r, markColor(m));
        SDL_RenderFillRect(r, &k.r);
        drawText(r, k.label, k.r.x + k.r.w / 2, k.r.y + k.r.h / 2, k.label.size() > 1 ? 13 : 20, WHITE);
    }
    SDL_RenderPresent(r);
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { SDL_Log("SDL_Init failed: %s", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { SDL_Log("TTF_Init failed: %s", TTF_GetError()); return 1; }
    if (!g_text.init()) { SDL_Log("Failed to decode embedded font"); return 1; }
    SDL_Window* win = SDL_CreateWindow("Wordle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);  // fallback
    if (!win || !ren) { SDL_Log("Window/renderer failed: %s", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || !g_audio.init())
        SDL_Log("No audio available, continuing without sound: %s", SDL_GetError());

    Game game;
    game.loadWords();
    game.reset();
    auto kb = buildKeyboard();

    bool quit = false;
    auto handle = [&](const SDL_Event& e) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_F1 || (k == SDLK_m && (e.key.keysym.mod & KMOD_CTRL))) {
                    g_audio.muted = !g_audio.muted;
                    if (g_audio.muted) g_audio.stopAll();
                    SDL_SetWindowTitle(win, g_audio.muted ? "Wordle (muted)" : "Wordle");
                }
                else if (k >= SDLK_a && k <= SDLK_z) game.input((char)('A' + (k - SDLK_a)));
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) game.input('\n');
                else if (k == SDLK_BACKSPACE) game.input('\b');
                else if (k == SDLK_ESCAPE) quit = true;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                SDL_Point p{e.button.x, e.button.y};
                for (const Key& k : kb)
                    if (SDL_PointInRect(&p, &k.r)) { game.input(k.c); break; }
            }
    };

    while (!quit) {
        Uint32 frameStart = SDL_GetTicks();
        bool anim = game.animating(frameStart);
        SDL_Event e;
        // Idle: sleep until input (or 50 ms, for message timeouts). Animating: run at ~60 fps.
        if (!anim && SDL_WaitEventTimeout(&e, 50)) handle(e);
        while (SDL_PollEvent(&e)) handle(e);
        game.update(SDL_GetTicks());
        render(ren, game, kb);
        if (anim) {
            Uint32 el = SDL_GetTicks() - frameStart;
            if (el < 16) SDL_Delay(16 - el);
        }
    }

    g_audio.shutdown();
    g_text.shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
