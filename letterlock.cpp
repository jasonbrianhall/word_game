// Letterlock: a word-guessing game in C++ / SDL2 + SDL_ttf, using the embedded DejaVu Sans Mono font.
// Guess the hidden word; each letter comes back green (right spot), yellow (in the word) or gray.
// Six modes: EASY (4 letters, eight guesses), NORMAL (5), HARD (6), EXPERT (7), GENIUS (8) and
// MENSA (9), six guesses each.
// Four-letter words give fewer clues per guess, so EASY gets two extra guesses.
// HINT (button, F3 or ?) suggests ten dictionary words, once per guess: the ones that fit every
// color so far, topped up with looser scrabbler-style matches, shuffled together.
// Guesses are checked against words.h (all 4- to 7-letter words in linux.words, from gen_words.sh);
// answers come from answers.h (the ~2000 most common of each length, from gen_answers.py).
// Build: make   (or: sh gen_words.sh > words.h && g++ -std=c++17 -O2 letterlock.cpp -o letterlock $(sdl2-config --cflags --libs) -lSDL2_ttf)

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
#include <ctime>
#include <fstream>
#include <random>
#include <cstring>
#include <string>
#include <vector>

enum Mark { EMPTY, PENDING, GRAY, YELLOW, GREEN };

// ---------- Modes ----------
// Word lists are packed: `count` words of `len` letters back to back, lowercase, sorted.
struct Mode {
    const char* name;
    int len;
    int rows;                        // guesses allowed
    const char* valid; size_t validCount;
    const char* answers; size_t answerCount;
    const char* statsFile;
};
static const Mode MODES[] = {
    {"EASY",   4, 8, VALID_WORDS_4, VALID_WORDS_4_COUNT, ANSWER_WORDS_4, ANSWER_WORDS_4_COUNT, "stats-easy.txt"},
    {"NORMAL", 5, 6, VALID_WORDS_5, VALID_WORDS_5_COUNT, ANSWER_WORDS_5, ANSWER_WORDS_5_COUNT, "stats.txt"},
    {"HARD",   6, 6, VALID_WORDS_6, VALID_WORDS_6_COUNT, ANSWER_WORDS_6, ANSWER_WORDS_6_COUNT, "stats-hard.txt"},
    {"EXPERT", 7, 6, VALID_WORDS_7, VALID_WORDS_7_COUNT, ANSWER_WORDS_7, ANSWER_WORDS_7_COUNT, "stats-expert.txt"},
    {"GENIUS", 8, 6, VALID_WORDS_8, VALID_WORDS_8_COUNT, ANSWER_WORDS_8, ANSWER_WORDS_8_COUNT, "stats-genius.txt"},
    {"MENSA",  9, 6, VALID_WORDS_9, VALID_WORDS_9_COUNT, ANSWER_WORDS_9, ANSWER_WORDS_9_COUNT, "stats-mensa.txt"},
};
constexpr int NUM_MODES = 6, MAX_LEN = 9, MAX_ROWS = 8;

// Binary search of a packed, sorted word list for `w` (lowercase, len letters).
static bool inList(const char* list, size_t count, int len, const std::string& w) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::memcmp(list + mid * len, w.data(), len);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

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
constexpr int FLIP_MS = 500;                        // tile reveal
// Delay between tiles flipping: quicker for 8 and 9 letters, so a row still reveals in ~2 s.
static int flipStagger(int len) { return len <= 7 ? 300 : 200; }
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
static const float GREEN_NOTES[MAX_LEN]  = {523.25f, 587.33f, 659.25f, 783.99f, 880.00f, 1046.50f, 1174.66f,
                                            1318.51f, 1567.98f};                                  // C5 D5 E5 G5 A5 C6 D6 E6 G6
static const float YELLOW_NOTES[MAX_LEN] = {261.63f, 293.66f, 329.63f, 392.00f, 440.00f, 523.25f, 587.33f,
                                            659.25f, 783.99f};                                    // C4 D4 E4 G4 A4 C5 D5 E5 G5
static const float WIN_NOTES[MAX_LEN]    = {523.25f, 659.25f, 783.99f, 1046.50f, 1318.51f, 1567.98f, 2093.00f,
                                            2637.02f, 3135.96f};                                  // C5 E5 G5 C6 E6 G6 C7 E7 G7
static const float LOSE_NOTES[3]   = {392.00f, 311.13f, 261.63f};                     // G4 Eb4 C4

// ---------- Statistics (saved in the per-user app data folder, one file per mode) ----------
static std::string prefDir() {  // e.g. ~/.local/share/letterlock/ on Linux
    static std::string dir;
    if (dir.empty()) if (char* p = SDL_GetPrefPath("letterlock", "letterlock")) { dir = p; SDL_free(p); }
    return dir;
}

struct Stats {
    int played = 0, wins = 0, curStreak = 0, maxStreak = 0;
    int rows = 6;       // guesses allowed in this mode
    int dist[MAX_ROWS] = {};  // wins by guess number (1..rows)
    int lastGuess = 0;  // guess number of the most recent win, 0 if it was a loss (highlighted bar)
    // The same for games where a hint was used (a subset of the totals above).
    int hintPlayed = 0, hintWins = 0, hintDist[MAX_ROWS] = {};
    bool lastHinted = false;  // the most recent game used a hint
    std::string dir, file;

    void load(const char* fileName, int guesses) {
        dir = prefDir();
        file = fileName;
        rows = guesses;
        std::ifstream f(dir + file);
        std::string key;
        while (f >> key) {
            if (key == "played") f >> played;
            else if (key == "wins") f >> wins;
            else if (key == "current_streak") f >> curStreak;
            else if (key == "max_streak") f >> maxStreak;
            else if (key == "distribution") for (int i = 0; i < rows; ++i) f >> dist[i];
            else if (key == "hint_played") f >> hintPlayed;
            else if (key == "hint_wins") f >> hintWins;
            else if (key == "hint_distribution") for (int i = 0; i < rows; ++i) f >> hintDist[i];
            else f.ignore(1 << 20, '\n');
        }
    }
    void save() const {
        std::ofstream f(dir + file);
        f << "played " << played << "\nwins " << wins << "\ncurrent_streak " << curStreak
          << "\nmax_streak " << maxStreak << "\ndistribution";
        for (int i = 0; i < rows; ++i) f << ' ' << dist[i];
        f << "\nhint_played " << hintPlayed << "\nhint_wins " << hintWins << "\nhint_distribution";
        for (int i = 0; i < rows; ++i) f << ' ' << hintDist[i];
        f << '\n';
    }
    // guessNum = 1..rows for a win, 0 for a loss; hints = hints used in that game.
    // Also appends the game to history.csv.
    void record(int guessNum, const std::string& answer, int hints) {
        ++played;
        lastGuess = guessNum;
        lastHinted = hints > 0;
        if (lastHinted) ++hintPlayed;
        if (guessNum) {
            ++wins; ++dist[guessNum - 1];
            if (lastHinted) { ++hintWins; ++hintDist[guessNum - 1]; }
            maxStreak = std::max(maxStreak, ++curStreak);
        } else {
            curStreak = 0;
        }
        save();

        std::string path = dir + "history.csv";
        bool fresh = !std::ifstream(path).good();
        std::ofstream h(path, std::ios::app);
        if (fresh) h << "date,answer,result,hints\n";   // older files keep their 3-column header
        char date[32];
        std::time_t now = std::time(nullptr);
        std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", std::localtime(&now));
        h << date << ',' << answer << ',' << (guessNum ? std::to_string(guessNum) : std::string("X")) + "/" + std::to_string(rows)
          << ',' << hints << '\n';
    }
} g_stats[NUM_MODES];

// The last mode played, so the game reopens in it.
static int loadMode() {
    std::ifstream f(prefDir() + "mode.txt");
    int m = 1;
    if (f >> m && m >= 0 && m < NUM_MODES) return m;
    return 1;
}
static void saveMode(int m) { std::ofstream f(prefDir() + "mode.txt"); f << m << '\n'; }

// ---------- Game logic ----------
// The colors `guess` earns against `answer` (both uppercase, len letters).
static std::array<Mark, MAX_LEN> score(const std::string& guess, const std::string& answer, int len) {
    std::array<Mark, MAX_LEN> res; res.fill(GRAY);
    int counts[26] = {};
    for (int i = 0; i < len; ++i) {
        if (guess[i] == answer[i]) res[i] = GREEN;
        else counts[answer[i] - 'A']++;
    }
    for (int i = 0; i < len; ++i)
        if (res[i] != GREEN && counts[guess[i] - 'A'] > 0) { res[i] = YELLOW; counts[guess[i] - 'A']--; }
    return res;
}

struct Game {
    std::vector<std::string> answers[NUM_MODES];
    int mode = 1;                    // index into MODES
    int len = 5;                     // letters per word in this mode
    int rows = 6;                    // guesses allowed in this mode

    std::string answer, cur;
    std::array<std::string, MAX_ROWS> guesses;
    std::array<std::array<Mark, MAX_LEN>, MAX_ROWS> marks{};
    std::array<Mark, 26> keys{};
    int row = 0;
    bool over = false, won = false;
    bool showStats = false;
    std::string msg;
    Uint32 msgUntil = 0;

    // Animation state
    int revealRow = -1;              // row currently flipping, -1 = none
    Uint32 revealStart = 0, popStart = 0, shakeStart = 0, bounceStart = 0;
    int popCol = -1;

    // Hints: up to HINT_MAX dictionary words consistent with every guess so far, one request per guess.
    static constexpr int HINT_MAX = 10;
    std::vector<std::string> hint;   // shown on the left, sorted
    int hintCount = 0;               // how many words suggested
    int hintRow = -1;                // row the current hint was asked on; -1 = none
    int hintsUsed = 0;               // hints asked for this game (recorded in the statistics)
    std::array<Mark, 26> pendingKeys{};  // keyboard colors applied after the flip finishes

    Uint32 revealEnd() const { return revealStart + (len - 1) * flipStagger(len) + FLIP_MS; }
    Stats& stats() { return g_stats[mode]; }
    const Stats& stats() const { return g_stats[mode]; }
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
               running(bounceStart, (len - 1) * BOUNCE_STAGGER + BOUNCE_MS, now, t);
    }
    std::mt19937 rng{std::random_device{}()};

    // Answers: the most common words of each length, and only ones the dictionary accepts.
    void loadWords() {
        for (int m = 0; m < NUM_MODES; ++m) {
            const Mode& md = MODES[m];
            for (size_t i = 0; i < md.answerCount; ++i) {
                std::string w(md.answers + i * md.len, md.len);
                if (!inList(md.valid, md.validCount, md.len, w)) continue;
                for (char& c : w) c = (char)std::toupper((unsigned char)c);
                answers[m].push_back(w);
            }
            if (answers[m].empty())
                for (size_t i = 0; i < md.validCount; ++i) {
                    std::string w(md.valid + i * md.len, md.len);
                    for (char& c : w) c = (char)std::toupper((unsigned char)c);
                    answers[m].push_back(w);
                }
        }
    }
    bool isValid(const std::string& w) const {
        std::string lower = w;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        return inList(MODES[mode].valid, MODES[mode].validCount, len, lower);
    }

    // Switch modes; an unfinished game is dropped without counting in the statistics.
    void setMode(int m) {
        if (m < 0 || m >= NUM_MODES || revealRow >= 0) return;
        if (m != mode) saveMode(m);
        mode = m;
        len = MODES[m].len;
        rows = MODES[m].rows;
        reset();
    }

    void reset() {
        g_audio.stopAll();
        const std::vector<std::string>& list = answers[mode];
        answer = list[std::uniform_int_distribution<size_t>(0, list.size() - 1)(rng)];
        for (auto& g : guesses) g.clear();
        for (auto& m : marks) m.fill(EMPTY);
        keys.fill(EMPTY);
        cur.clear();
        row = 0; over = won = showStats = false; msgUntil = 0;
        hint.clear(); hintCount = 0; hintRow = -1; hintsUsed = 0;
        revealRow = -1; revealStart = popStart = shakeStart = bounceStart = 0; popCol = -1;
    }

    bool canHint() const { return !over && revealRow < 0 && hintRow != row; }

    // Fuzzy hints over the whole dictionary (every word of this length in
    // linux.words). First the words that fit every color shown: were each the
    // answer, every earlier guess would have scored exactly as it did. If fewer
    // than HINT_MAX fit, the rest are filled with scrabbler-style matches: green
    // letters in place, no gray letters, each letter used once (plus once per
    // green spot), yellows ignored. Both kinds are sorted together, so the
    // list doesn't say which words really fit.
    void askHint() {
        if (!canHint()) return;
        const Mode& md = MODES[mode];

        // What the colors say, scrabbler-style.
        char green[MAX_LEN] = {};
        int greens[26] = {};
        bool seen[26] = {}, gray[26] = {};
        for (int r = 0; r < row; ++r)
            for (int i = 0; i < len; ++i) {
                int c = guesses[r][i] - 'A';
                if (marks[r][i] == GREEN) { if (!green[i]) ++greens[c]; green[i] = guesses[r][i]; }
                if (marks[r][i] == GRAY) gray[c] = true; else seen[c] = true;
            }
        auto loose = [&](const std::string& w) {
            int count[26] = {};
            for (int i = 0; i < len; ++i) {
                int c = w[i] - 'A';
                if (green[i] && w[i] != green[i]) return false;
                if (gray[c] && !seen[c]) return false;       // known absent
                if (++count[c] > 1 + greens[c]) return false;
            }
            return true;
        };

        std::vector<std::string> exact, fuzzy;
        std::string w(len, ' ');
        for (size_t k = 0; k < md.validCount; ++k) {
            for (int i = 0; i < len; ++i) w[i] = (char)std::toupper((unsigned char)md.valid[k * len + i]);
            bool ok = true;
            for (int r = 0; r < row && ok; ++r) {
                std::array<Mark, MAX_LEN> sc = score(guesses[r], w, len);
                for (int i = 0; i < len; ++i) if (sc[i] != marks[r][i]) { ok = false; break; }
            }
            if (ok) exact.push_back(w);
            else if (loose(w)) fuzzy.push_back(w);
        }
        std::shuffle(exact.begin(), exact.end(), rng);
        std::shuffle(fuzzy.begin(), fuzzy.end(), rng);
        hint.assign(exact.begin(), exact.begin() + std::min<size_t>(exact.size(), HINT_MAX));
        for (size_t i = 0; hint.size() < (size_t)HINT_MAX && i < fuzzy.size(); ++i) hint.push_back(fuzzy[i]);
        std::sort(hint.begin(), hint.end());
        hintCount = (int)hint.size();
        hintRow = row;
        ++hintsUsed;
        g_audio.play(SOFT, 659.25f, 0.18f, 0, 250);
    }

    void flash(const std::string& m) {
        msg = m;
        msgUntil = SDL_GetTicks() + 1500;
        shakeStart = SDL_GetTicks();
        g_audio.play(BUZZ, 98.f, 0.18f, 0, 220);
    }

    void submit() {
        std::array<Mark, MAX_LEN> res = score(cur, answer, len);

        pendingKeys = keys;
        for (int i = 0; i < len; ++i) {
            Mark& k = pendingKeys[cur[i] - 'A'];
            if (res[i] > k) k = res[i];  // GREEN > YELLOW > GRAY
        }
        // Sounds, timed to each tile's flip midpoint (when its color appears).
        for (int i = 0; i < len; ++i) {
            int at = i * flipStagger(len) + FLIP_MS / 2;
            if (res[i] == GREEN)       g_audio.play(BELL, GREEN_NOTES[i], 0.32f, at, 1200);
            else if (res[i] == YELLOW) g_audio.play(SOFT, YELLOW_NOTES[i], 0.30f, at, 700);
            else                       g_audio.play(TICK, 150.f, 0.30f, at, 150);
        }
        int end = (len - 1) * flipStagger(len) + FLIP_MS;
        if (cur == answer) {  // rising arpeggio in step with the tile bounce, then a sustained chord
            for (int i = 0; i < len; ++i) g_audio.play(BELL, WIN_NOTES[i], 0.26f, end + i * BOUNCE_STAGGER, 1000);
            for (float f : {523.25f, 659.25f, 783.99f, 1046.50f})
                g_audio.play(BELL, f, 0.16f, end + len * BOUNCE_STAGGER + 100, 2200);
        } else if (row == rows - 1) {  // last guess missed: gentle descending minor phrase
            for (int i = 0; i < 3; ++i) g_audio.play(SOFT, LOSE_NOTES[i], 0.28f, end + 200 + i * 260, 900);
        }

        guesses[row] = cur;
        marks[row] = res;
        revealRow = row;
        revealStart = SDL_GetTicks();
        ++row;
        if (cur == answer) { over = won = true; stats().record(row, answer, hintsUsed); }
        else if (row == rows) { over = true; stats().record(0, answer, hintsUsed); }
        cur.clear();
    }

    void input(char c) {
        if (revealRow >= 0) return;  // ignore input while tiles are flipping
        if (showStats && !over) { if (c == '\n') showStats = false; return; }  // opened mid-game with F2
        if (over) {  // Enter #1: show statistics; Enter #2: new game
            if (c == '\n') { if (showStats) reset(); else showStats = true; }
            return;
        }
        if (c >= 'A' && c <= 'Z') {
            if ((int)cur.size() < len) { cur += c; popCol = (int)cur.size() - 1; popStart = SDL_GetTicks(); }
        }
        else if (c == '\b') { if (!cur.empty()) cur.pop_back(); }
        else if (c == '\n') {
            if ((int)cur.size() < len) flash("NOT ENOUGH LETTERS");
            else if (!isValid(cur)) flash("NOT IN WORD LIST");
            else submit();
        }
    }
};

// ---------- Layout ----------
constexpr int WIN_W = 500, WIN_H = 720;           // the game area
constexpr int PANEL_W = 180;                       // hint panel to its left
constexpr int WINDOW_W = PANEL_W + WIN_W;
static const SDL_Rect GAME_VIEW{PANEL_W, 0, WIN_W, WIN_H}, PANEL_VIEW{0, 0, PANEL_W, WIN_H};
static const SDL_Rect HINT_BUTTON{20, 84, PANEL_W - 40, 40};   // in window coordinates
constexpr int GAP = 6, GRID_Y = 84, GRID_BOTTOM = 466;   // the board sits above the messages
// Tile size: 58 px when it fits, smaller when EASY's eight rows must fit the
// height or GENIUS/MENSA's eight or nine letters must fit the 500 px width.
static int tileSize(int rows, int len) {
    int t = (GRID_BOTTOM - GRID_Y - (rows - 1) * GAP) / rows;
    int tw = (WIN_W - 20 - (len - 1) * GAP) / len;
    if (tw < t) t = tw;
    return t < 58 ? t : 58;
}
static int gridX(int len, int tile) { return (WIN_W - (len * tile + (len - 1) * GAP)) / 2; }

// Mode tabs under the title.
constexpr int TAB_GAP = 6, TAB_W = (WIN_W - 20 - (NUM_MODES - 1) * TAB_GAP) / NUM_MODES, TAB_H = 26, TAB_Y = 50;
static SDL_Rect tabRect(int m) {
    int x0 = (WIN_W - (NUM_MODES * TAB_W + (NUM_MODES - 1) * TAB_GAP)) / 2;
    return {x0 + m * (TAB_W + TAB_GAP), TAB_Y, TAB_W, TAB_H};
}
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

static void drawStats(SDL_Renderer* r, const Game& g) {
    const Stats& st = g.stats();
    // Dim everything behind the panel
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_Rect all{0, 0, WIN_W, WIN_H};
    SDL_RenderFillRect(r, &all);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    SDL_Rect panel{30, 64, WIN_W - 60, 560};
    setColor(r, {30, 30, 32, 255});
    SDL_RenderFillRect(r, &panel);
    drawOutline(r, panel, BORDER);

    int cx = WIN_W / 2, y = panel.y + 36;
    if (g.over) {
        std::string res = g.won ? "SOLVED IN " + std::to_string(g.row) + (g.row == 1 ? " GUESS" : " GUESSES")
                                : "THE WORD WAS " + g.answer;
        if (g.won && g.hintsUsed)
            res += " WITH " + std::to_string(g.hintsUsed) + (g.hintsUsed == 1 ? " HINT" : " HINTS");
        drawText(r, res, cx, y, 16, !g.won ? WHITE : g.hintsUsed ? C_YELLOW : C_GREEN);
        y += 36;
    }
    drawText(r, std::string(MODES[g.mode].name) + " STATISTICS", cx, y, 20, WHITE);
    y += 50;

    // Four headline numbers
    int winPct = st.played ? (int)std::lround(100.0 * st.wins / st.played) : 0;
    const int vals[4] = {st.played, winPct, st.curStreak, st.maxStreak};
    const char* top[4] = {"PLAYED", "WIN %", "CURRENT", "MAX"};
    const char* bot[4] = {"", "", "STREAK", "STREAK"};
    int colW = (panel.w - 40) / 4;
    for (int i = 0; i < 4; ++i) {
        int x = panel.x + 20 + colW * i + colW / 2;
        drawText(r, std::to_string(vals[i]), x, y, 30, WHITE);
        drawText(r, top[i], x, y + 34, 11, WHITE);
        if (*bot[i]) drawText(r, bot[i], x, y + 50, 11, WHITE);
    }
    y += 84;

    // Games won without and with hints (hinted games show in yellow below).
    int plain = st.played - st.hintPlayed, plainWins = st.wins - st.hintWins;
    drawText(r, "NO HINTS: " + std::to_string(plainWins) + " OF " + std::to_string(plain) + " WON",
             panel.x + panel.w / 4, y, 13, WHITE);
    drawText(r, "HINTS: " + std::to_string(st.hintWins) + " OF " + std::to_string(st.hintPlayed) + " WON",
             panel.x + panel.w * 3 / 4, y, 13, C_YELLOW);
    y += 40;

    drawText(r, "GUESS DISTRIBUTION", cx, y, 15, WHITE);
    y += 30;
    int maxD = 1;
    for (int d : st.dist) maxD = std::max(maxD, d);
    int barX = panel.x + 60, barMaxW = panel.w - 100, barH = st.rows > 6 ? 20 : 26;
    for (int i = 0; i < st.rows; ++i) {
        drawText(r, std::to_string(i + 1), panel.x + 40, y + barH / 2, 15, WHITE);
        int w = std::max(30, barMaxW * st.dist[i] / maxD);
        int hw = st.dist[i] ? w * st.hintDist[i] / st.dist[i] : 0;   // hinted wins, drawn at the end
        SDL_Rect bar{barX, y, w - hw, barH}, hbar{barX + w - hw, y, hw, barH};
        bool latest = g.over && st.lastGuess == i + 1;
        setColor(r, latest && !st.lastHinted ? C_GREEN : C_GRAY);
        SDL_RenderFillRect(r, &bar);
        setColor(r, latest && st.lastHinted ? C_YELLOW : SDL_Color{120, 106, 40, 255});
        SDL_RenderFillRect(r, &hbar);
        drawText(r, std::to_string(st.dist[i]), barX + w - 16, y + barH / 2, 14, WHITE);
        y += barH + (st.rows > 6 ? 6 : 8);
    }

    drawText(r, g.over ? "PRESS ENTER TO PLAY AGAIN" : "PRESS ENTER TO CONTINUE",
             cx, panel.y + panel.h - 26, 15, KEY_DEFAULT);
}

// The hint panel: a HINT button, then the words it found.
static void drawHintPanel(SDL_Renderer* r, const Game& g) {
    SDL_RenderSetViewport(r, &PANEL_VIEW);
    bool can = g.canHint();
    if (can) { setColor(r, KEY_DEFAULT); SDL_RenderFillRect(r, &HINT_BUTTON); }
    else drawOutline(r, HINT_BUTTON, BORDER);
    drawText(r, "HINT", HINT_BUTTON.x + HINT_BUTTON.w / 2, HINT_BUTTON.y + HINT_BUTTON.h / 2, 18,
             can ? WHITE : C_GRAY);
    int cx = PANEL_W / 2, y = HINT_BUTTON.y + HINT_BUTTON.h + 22;
    drawText(r, g.over ? "" : can ? "F3 OR ?" : "ONE PER GUESS", cx, y, 11, KEY_DEFAULT);
    y += 30;
    if (g.hintRow >= 0 && !g.over) {
        std::string head = g.hintCount == 0 ? "NO IDEAS" : "MAYBE...";
        drawText(r, head, cx, y, 14, WHITE);
        y += 32;
        for (const std::string& w : g.hint) { drawText(r, w, cx, y, 20, WHITE); y += 30; }
    }
    // Divider between the panel and the game.
    SDL_Rect line{PANEL_W - 1, 70, 1, WIN_H - 140};
    setColor(r, BORDER);
    SDL_RenderFillRect(r, &line);
}

static void render(SDL_Renderer* r, const Game& g, const std::vector<Key>& kb) {
    SDL_RenderSetViewport(r, nullptr);
    setColor(r, BG);
    SDL_RenderClear(r);
    drawHintPanel(r, g);
    SDL_RenderSetViewport(r, &GAME_VIEW);                  // the game draws in its own 500 px area
    drawText(r, "LETTERLOCK", WIN_W / 2, 24, 34, WHITE);
    for (int m = 0; m < NUM_MODES; ++m) {
        SDL_Rect tab = tabRect(m);
        if (m == g.mode) { setColor(r, C_GREEN); SDL_RenderFillRect(r, &tab); }
        else drawOutline(r, tab, BORDER);
        drawText(r, MODES[m].name, tab.x + tab.w / 2, tab.y + tab.h / 2, 13, m == g.mode ? WHITE : KEY_DEFAULT);
    }

    // Board
    Uint32 now = SDL_GetTicks(), t;
    const int TILE = tileSize(g.rows, g.len), letterPt = TILE >= 58 ? 36 : 30;
    for (int row = 0; row < g.rows; ++row) {
        bool typingRow = row == g.row && !g.over;
        std::string s = row < g.row ? g.guesses[row] : (typingRow ? g.cur : "");

        int shakeX = 0;  // horizontal wobble on invalid guess
        if (typingRow && running(g.shakeStart, SHAKE_MS, now, t))
            shakeX = (int)(std::sin(t * 0.07f) * 9.f * (1.f - t / (float)SHAKE_MS));

        for (int col = 0; col < g.len; ++col) {
            int cx = gridX(g.len, TILE) + col * (TILE + GAP) + TILE / 2 + shakeX;
            int cy = GRID_Y + row * (TILE + GAP) + TILE / 2;
            Mark m = row < g.row ? g.marks[row][col] : (col < (int)s.size() ? PENDING : EMPTY);
            float sx = 1.f, sy = 1.f;

            if (row == g.revealRow) {  // flip: squash to 0 height, swap color at midpoint, expand
                int ft = (int)(now - g.revealStart) - col * flipStagger(g.len);
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
                drawText(r, std::string(1, s[col]), cx, cy, letterPt, WHITE, sx, sy);
        }
    }

    // Messages
    if (g.over && g.revealRow < 0 && !g.showStats) {
        static const char* praise6[6] = {"GENIUS", "MAGNIFICENT", "IMPRESSIVE", "SPLENDID", "GREAT", "PHEW"};
        static const char* praise8[8] = {"GENIUS", "MAGNIFICENT", "IMPRESSIVE", "SPLENDID", "GREAT", "NICE",
                                         "CLOSE ONE", "PHEW"};
        drawText(r, g.won ? (g.rows > 6 ? praise8 : praise6)[g.row - 1] : "ANSWER " + g.answer, WIN_W / 2, 480, 18, WHITE);
        drawText(r, "PRESS ENTER FOR STATISTICS", WIN_W / 2, 506, 15, KEY_DEFAULT);
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
    if (g.showStats) {
        drawStats(r, g);
        SDL_RenderSetViewport(r, &PANEL_VIEW);             // dim the hint panel too
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
        SDL_Rect all{0, 0, PANEL_W, WIN_H};
        SDL_RenderFillRect(r, &all);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }
    SDL_RenderPresent(r);
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { SDL_Log("SDL_Init failed: %s", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { SDL_Log("TTF_Init failed: %s", TTF_GetError()); return 1; }
    if (!g_text.init()) { SDL_Log("Failed to decode embedded font"); return 1; }
    SDL_Window* win = SDL_CreateWindow("Letterlock", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       WINDOW_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);  // fallback
    if (!win || !ren) { SDL_Log("Window/renderer failed: %s", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, WINDOW_W, WIN_H);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || !g_audio.init())
        SDL_Log("No audio available, continuing without sound: %s", SDL_GetError());

    for (int m = 0; m < NUM_MODES; ++m) g_stats[m].load(MODES[m].statsFile, MODES[m].rows);
    Game game;
    game.loadWords();
    game.setMode(loadMode());
    auto kb = buildKeyboard();

    bool quit = false;
    auto handle = [&](const SDL_Event& e) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_F1 || (k == SDLK_m && (e.key.keysym.mod & KMOD_CTRL))) {
                    g_audio.muted = !g_audio.muted;
                    if (g_audio.muted) g_audio.stopAll();
                    SDL_SetWindowTitle(win, g_audio.muted ? "Letterlock (muted)" : "Letterlock");
                }
                else if (k >= SDLK_a && k <= SDLK_z) game.input((char)('A' + (k - SDLK_a)));
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) game.input('\n');
                else if (k == SDLK_BACKSPACE) game.input('\b');
                else if (k >= SDLK_4 && k <= SDLK_9) game.setMode((int)(k - SDLK_4));  // 4-9 letters
                else if (k == SDLK_F3 || k == SDLK_SLASH) { if (!game.showStats) game.askHint(); }   // F3 or ?
                else if (k == SDLK_F2) { if (game.revealRow < 0) game.showStats = !game.showStats || game.over; }
                else if (k == SDLK_ESCAPE) { if (game.showStats && !game.over) game.showStats = false; else quit = true; }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                SDL_Point w{e.button.x, e.button.y};             // window coordinates
                if (SDL_PointInRect(&w, &HINT_BUTTON)) { if (!game.showStats) game.askHint(); return; }
                SDL_Point p{w.x - GAME_VIEW.x, w.y - GAME_VIEW.y};  // game-area coordinates
                for (int m = 0; m < NUM_MODES; ++m) {
                    SDL_Rect tab = tabRect(m);
                    if (SDL_PointInRect(&p, &tab)) game.setMode(m);
                }
                for (const Key& k : kb)
                    if (SDL_PointInRect(&p, &k.r)) { game.input(k.c); break; }
            }
    };

    // Redraw only when something can have changed: an event other than plain
    // mouse motion (key, click, window exposed...), a running animation, one
    // more frame after it ends, or the flash message timing out.
    bool dirty = true, wasAnim = false, msgShown = false;
    while (!quit) {
        Uint32 frameStart = SDL_GetTicks();
        bool anim = game.animating(frameStart);
        SDL_Event e;
        // Idle: sleep until input (or 50 ms, for message timeouts). Animating: run at ~60 fps.
        if (!anim && SDL_WaitEventTimeout(&e, 50)) { handle(e); dirty |= e.type != SDL_MOUSEMOTION; }
        while (SDL_PollEvent(&e)) { handle(e); dirty |= e.type != SDL_MOUSEMOTION; }
        game.update(SDL_GetTicks());
        bool msgNow = SDL_GetTicks() < game.msgUntil;
        if (dirty || anim || wasAnim || msgNow != msgShown) {
            render(ren, game, kb);
            dirty = false;
        }
        wasAnim = anim;
        msgShown = msgNow;
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
