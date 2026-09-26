// Wordle in C++ / SDL2 + SDL_ttf, using the embedded DejaVu Sans Mono font.
// Guesses are checked against words.h, generated from linux.words by gen_words.sh.
// Build: make   (or: sh gen_words.sh > words.h && g++ -std=c++17 -O2 wordle.cpp -o wordle $(sdl2-config --cflags --libs) -lSDL2_ttf)

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <map>
#include <tuple>
#include "DejaVuMono.h"
#include "words.h"
#include <array>
#include <cctype>
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
static void drawText(SDL_Renderer* r, const std::string& s, int cx, int cy, int pt, SDL_Color col) {
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

// ---------- Built-in answer list ----------
static const char* DEFAULT_WORDS[] = {
    "ABOUT","ABOVE","ACTOR","ADMIT","ADOPT","AFTER","AGAIN","AGENT","AGREE","ALARM",
    "ALBUM","ALERT","ALIVE","ALLOW","ALONE","ANGER","ANGLE","ANGRY","APPLE","APPLY",
    "ARENA","ARGUE","ARISE","ASIDE","AWARD","AWARE","BAKER","BASIC","BEACH","BEGIN",
    "BEING","BELOW","BENCH","BIRTH","BLACK","BLAME","BLANK","BLIND","BLOCK","BLOOD",
    "BOARD","BRAIN","BRAND","BREAD","BREAK","BRICK","BRIEF","BRING","BROAD","BROWN",
    "BUILD","CABIN","CANDY","CARRY","CATCH","CAUSE","CHAIN","CHAIR","CHARM","CHART",
    "CHASE","CHEAP","CHECK","CHEST","CHIEF","CHILD","CLAIM","CLASS","CLEAN","CLEAR",
    "CLIMB","CLOCK","CLOSE","CLOUD","COAST","COUNT","COURT","COVER","CRAFT","CRANE",
    "CRASH","CREAM","CRIME","CROWD","CURVE","CYCLE","DAILY","DANCE","DEALT","DELAY",
    "DEPTH","DOUBT","DRAFT","DRAMA","DREAM","DRESS","DRINK","DRIVE","EARLY","EARTH",
    "EIGHT","EMPTY","ENEMY","ENJOY","ENTER","EQUAL","ERROR","EVENT","EXACT","EXIST",
    "EXTRA","FAITH","FALSE","FAULT","FEAST","FIELD","FIGHT","FINAL","FLAME","FLASH",
    "FLOOR","FOCUS","FORCE","FRAME","FRESH","FRONT","FRUIT","FUNNY","GHOST","GIANT",
    "GLASS","GLOVE","GRACE","GRADE","GRAIN","GRAND","GRANT","GRAPE","GRASS","GREAT",
    "GREEN","GROUP","GUARD","GUESS","GUEST","GUIDE","HAPPY","HEART","HEAVY","HONEY",
    "HORSE","HOTEL","HOUSE","HUMAN","HUMOR","IDEAL","IMAGE","INDEX","INNER","ISSUE",
    "JUDGE","JUICE","KNIFE","LARGE","LASER","LAUGH","LAYER","LEARN","LEMON","LEVEL",
    "LIGHT","LIMIT","LUCKY","LUNCH","MAGIC","MAJOR","MARCH","MATCH","MAYBE","METAL",
    "MIGHT","MINOR","MODEL","MONEY","MONTH","MOUNT","MOUSE","MOUTH","MUSIC","NERVE",
    "NEVER","NIGHT","NOISE","NORTH","NOVEL","NURSE","OCEAN","OFFER","OFTEN","ORDER",
    "OTHER","OWNER","PAINT","PANEL","PAPER","PARTY","PEACE","PHONE","PIANO","PIECE",
    "PILOT","PITCH","PLACE","PLAIN","PLANE","PLANT","PLATE","POINT","POUND","POWER",
    "PRESS","PRICE","PRIDE","PRIME","PRIZE","PROOF","PROUD","QUEEN","QUICK","QUIET",
    "RADIO","RAISE","RANGE","RAPID","REACH","READY","RIVER","ROBOT","ROUND","ROYAL",
    "SCALE","SCENE","SCORE","SENSE","SHAPE","SHARE","SHARP","SHEEP","SHELF","SHIFT",
    "SHINE","SHIRT","SHOCK","SHORT","SIGHT","SKILL","SLEEP","SMALL","SMART","SMILE",
    "SMOKE","SOLID","SOUND","SOUTH","SPACE","SPARE","SPEAK","SPEED","SPEND","SPICE",
    "SPORT","STAFF","STAGE","STAND","START","STEAM","STEEL","STICK","STONE","STORM",
    "STORY","SUGAR","SWEET","TABLE","TASTE","TEACH","THEME","THICK","THING","THINK",
    "THREE","TIGER","TIRED","TITLE","TOAST","TODAY","TOOTH","TOTAL","TOUCH","TOWER",
    "TRACK","TRADE","TRAIN","TREAT","TREND","TRIAL","TRUCK","TRUST","TRUTH","UNCLE",
    "UNDER","UNION","UNTIL","UPPER","URBAN","USUAL","VALUE","VIDEO","VISIT","VOICE",
    "WASTE","WATCH","WATER","WHEEL","WHITE","WHOLE","WOMAN","WORLD","WORRY","WORTH",
    "WOUND","WRITE","WRONG","YOUNG","YOUTH","ZEBRA",
};

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
    std::mt19937 rng{std::random_device{}()};

    void loadWords() {
        for (size_t i = 0; i < VALID_WORDS_COUNT; ++i) {
            std::string w = VALID_WORDS[i];
            for (char& c : w) c = (char)std::toupper((unsigned char)c);
            valid.insert(w);
        }
        // Answers: common words, but only ones the dictionary accepts.
        for (const char* w : DEFAULT_WORDS)
            if (valid.count(w)) answers.emplace_back(w);
        if (answers.empty()) answers.assign(valid.begin(), valid.end());
    }

    void reset() {
        answer = answers[std::uniform_int_distribution<size_t>(0, answers.size() - 1)(rng)];
        for (auto& g : guesses) g.clear();
        for (auto& m : marks) m.fill(EMPTY);
        keys.fill(EMPTY);
        cur.clear();
        row = 0; over = won = false; msgUntil = 0;
    }

    void flash(const std::string& m) { msg = m; msgUntil = SDL_GetTicks() + 1500; }

    void submit() {
        std::array<Mark, 5> res; res.fill(GRAY);
        int counts[26] = {};
        for (int i = 0; i < 5; ++i) {
            if (cur[i] == answer[i]) res[i] = GREEN;
            else counts[answer[i] - 'A']++;
        }
        for (int i = 0; i < 5; ++i)
            if (res[i] != GREEN && counts[cur[i] - 'A'] > 0) { res[i] = YELLOW; counts[cur[i] - 'A']--; }

        for (int i = 0; i < 5; ++i) {
            Mark& k = keys[cur[i] - 'A'];
            if (res[i] > k) k = res[i];  // GREEN > YELLOW > GRAY
        }
        guesses[row] = cur;
        marks[row] = res;
        ++row;
        if (cur == answer) { over = won = true; }
        else if (row == 6) { over = true; }
        cur.clear();
    }

    void input(char c) {
        if (over) { if (c == '\n') reset(); return; }
        if (c >= 'A' && c <= 'Z') { if (cur.size() < 5) cur += c; }
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
    for (int row = 0; row < 6; ++row) {
        std::string s = row < g.row ? g.guesses[row] : (row == g.row && !g.over ? g.cur : "");
        for (int col = 0; col < 5; ++col) {
            SDL_Rect rc{GRID_X + col * (TILE + GAP), GRID_Y + row * (TILE + GAP), TILE, TILE};
            Mark m = row < g.row ? g.marks[row][col] : (col < (int)s.size() ? PENDING : EMPTY);
            if (m == EMPTY || m == PENDING) drawOutline(r, rc, m == PENDING ? BORDER_ACTIVE : BORDER);
            else { setColor(r, markColor(m)); SDL_RenderFillRect(r, &rc); }
            if (col < (int)s.size())
                drawText(r, std::string(1, s[col]), rc.x + TILE / 2, rc.y + TILE / 2, 36, WHITE);
        }
    }

    // Messages
    if (g.over) {
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

    Game game;
    game.loadWords();
    game.reset();
    auto kb = buildKeyboard();

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 50)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k >= SDLK_a && k <= SDLK_z) game.input((char)('A' + (k - SDLK_a)));
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) game.input('\n');
                else if (k == SDLK_BACKSPACE) game.input('\b');
                else if (k == SDLK_ESCAPE) running = false;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                SDL_Point p{e.button.x, e.button.y};
                for (const Key& k : kb)
                    if (SDL_PointInRect(&p, &k.r)) { game.input(k.c); break; }
            }
            if (!running) break;
        }
        render(ren, game, kb);
    }

    g_text.shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
