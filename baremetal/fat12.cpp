// FAT12 filesystem driver. See fat12.hpp.
//
// Safety on power loss or a pulled disk: file data always goes to newly
// allocated clusters first, then the FAT, then the directory entry, and only
// then are the old clusters freed. An interrupted write leaves either the old
// file or the new one (at worst plus some lost clusters that fsck reclaims),
// never a cross-linked or half-written file.
#include "fat12.hpp"
#include <string.h>
#include <time.h>
#include <vector>

namespace {

constexpr uint32_t SEC = 512;
constexpr uint32_t NONE = 0xFFFFFFFFu;
enum : uint8_t { A_RO = 0x01, A_HID = 0x02, A_SYS = 0x04, A_VOL = 0x08, A_DIR = 0x10, A_ARC = 0x20, A_LFN = 0x0F };
enum : uint8_t { NT_LOWER_BASE = 0x08, NT_LOWER_EXT = 0x10 };

struct __attribute__((packed)) Dirent {
    uint8_t name[11];
    uint8_t attr, ntres, ctime_tenth;
    uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus;
    uint32_t size;
};
struct __attribute__((packed)) LfnEnt {
    uint8_t ord;
    uint16_t n1[5];
    uint8_t attr, type, sum;
    uint16_t n2[6];
    uint16_t clus;
    uint16_t n3[2];
};
static_assert(sizeof(Dirent) == 32 && sizeof(LfnEnt) == 32, "directory entries are 32 bytes");

// ---------------------------------------------------------------- volume state
struct Volume {
    FatDisk disk{};
    bool mounted = false;
    uint32_t spc = 0, csize = 0;           // sectors / bytes per cluster
    uint32_t fat_lba = 0, fat_secs = 0, nfats = 0;
    uint32_t root_lba = 0, root_secs = 0;
    uint32_t data_lba = 0, nclus = 0;      // valid clusters are 2 .. nclus+1
    std::vector<uint8_t> fat;              // first FAT copy, in memory
    std::vector<uint8_t> fat_dirty;        // per FAT sector
    uint32_t hint = 2;                     // where to start looking for free clusters
} v;

// One-sector write-back cache for directory sectors.
uint8_t cache[SEC];
uint32_t cache_lba = NONE;
bool cache_dirty = false;

bool cache_flush() {
    if (!cache_dirty) return true;
    if (!v.disk.write(cache_lba, 1, cache)) return false;
    cache_dirty = false;
    return true;
}
uint8_t* sec_get(uint32_t lba) {
    if (lba == cache_lba) return cache;
    if (!cache_flush()) return nullptr;
    cache_lba = NONE;
    if (!v.disk.read(lba, 1, cache)) return nullptr;
    cache_lba = lba;
    return cache;
}
// Bulk transfers bypass the cache; keep it coherent.
bool raw_write(uint32_t lba, uint32_t n, const void* p) {
    if (cache_lba != NONE && cache_lba >= lba && cache_lba < lba + n) {
        if (cache_dirty && !cache_flush()) return false;
        cache_lba = NONE;
    }
    return v.disk.write(lba, n, p);
}
bool raw_read(uint32_t lba, uint32_t n, void* p) {
    if (cache_dirty && cache_lba >= lba && cache_lba < lba + n && !cache_flush()) return false;
    return v.disk.read(lba, n, p);
}
void idle() { if (v.disk.idle) v.disk.idle(); }

// ---------------------------------------------------------------- FAT
bool valid(uint32_t c) { return c >= 2 && c <= v.nclus + 1; }
bool is_eoc(uint32_t x) { return x >= 0xFF8; }
uint32_t clus_lba(uint32_t c) { return v.data_lba + (c - 2) * v.spc; }

uint32_t fat_get(uint32_t c) {
    uint32_t o = c + c / 2;
    uint32_t x = v.fat[o] | (uint32_t)v.fat[o + 1] << 8;
    return (c & 1) ? x >> 4 : x & 0xFFF;
}
void fat_set(uint32_t c, uint32_t x) {
    uint32_t o = c + c / 2;
    if (c & 1) {
        v.fat[o] = (uint8_t)((v.fat[o] & 0x0F) | (x << 4));
        v.fat[o + 1] = (uint8_t)(x >> 4);
    } else {
        v.fat[o] = (uint8_t)x;
        v.fat[o + 1] = (uint8_t)((v.fat[o + 1] & 0xF0) | ((x >> 8) & 0x0F));
    }
    v.fat_dirty[o / SEC] = v.fat_dirty[(o + 1) / SEC] = 1;
}
// Write the changed FAT sectors to every copy.
bool fat_flush() {
    for (uint32_t s = 0; s < v.fat_secs;) {
        if (!v.fat_dirty[s]) { s++; continue; }
        uint32_t e = s;
        while (e < v.fat_secs && v.fat_dirty[e]) e++;
        for (uint32_t k = 0; k < v.nfats; k++)
            if (!raw_write(v.fat_lba + k * v.fat_secs + s, e - s, &v.fat[s * SEC])) return false;
        for (uint32_t i = s; i < e; i++) v.fat_dirty[i] = 0;
        s = e;
    }
    return true;
}
void free_chain(uint32_t c) {
    for (uint32_t n = 0; valid(c) && n <= v.nclus; n++) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        if (c < v.hint) v.hint = c;
        c = next;
    }
}
bool chain(uint32_t c, std::vector<uint32_t>& out) {
    out.clear();
    while (valid(c)) {
        if (out.size() > v.nclus) return false;          // loop in the FAT
        out.push_back(c);
        c = fat_get(c);
    }
    return c == 0 ? out.empty() : is_eoc(c);
}
// Allocate n linked clusters (in memory only). Returns the first, or 0.
uint32_t alloc_chain(uint32_t n, std::vector<uint32_t>& got) {
    got.clear();
    if (n == 0) return 0;
    for (uint32_t i = 0; i < v.nclus && got.size() < n; i++) {
        uint32_t c = 2 + (v.hint - 2 + i) % v.nclus;
        if (fat_get(c) == 0) { got.push_back(c); fat_set(c, 0xFFF); }
    }
    if (got.size() < n) { for (uint32_t c : got) fat_set(c, 0); got.clear(); return 0; }
    for (size_t i = 0; i + 1 < got.size(); i++) fat_set(got[i], got[i + 1]);
    v.hint = got.back() + 1 > v.nclus + 1 ? 2 : got.back() + 1;
    return got[0];
}
// Write `len` bytes over the clusters (zero-padding the last one), merging runs.
bool write_clusters(const std::vector<uint32_t>& cl, const void* data, size_t len) {
    std::vector<uint8_t> buf(cl.size() * v.csize, 0);
    if (len) memcpy(buf.data(), data, len);
    for (size_t i = 0; i < cl.size();) {
        size_t j = i + 1;
        while (j < cl.size() && cl[j] == cl[j - 1] + 1) j++;
        if (!raw_write(clus_lba(cl[i]), (uint32_t)(j - i) * v.spc, &buf[i * v.csize])) return false;
        i = j;
    }
    return true;
}
bool read_clusters(const std::vector<uint32_t>& cl, size_t len, std::string& out) {
    std::vector<uint8_t> buf(cl.size() * v.csize);
    for (size_t i = 0; i < cl.size();) {
        size_t j = i + 1;
        while (j < cl.size() && cl[j] == cl[j - 1] + 1) j++;
        if (!raw_read(clus_lba(cl[i]), (uint32_t)(j - i) * v.spc, &buf[i * v.csize])) return false;
        i = j;
    }
    out.assign((const char*)buf.data(), len);
    return true;
}

// ---------------------------------------------------------------- time
void stamp(uint16_t& date, uint16_t& tm) {
    long t = fat_clock();
    long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    days += 719468;                                       // civil_from_days (H. Hinnant)
    long era = (days >= 0 ? days : days - 146096) / 146097;
    long doe = days - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y < 1980) { y = 1980; m = 1; d = 1; secs = 0; }
    if (y > 2107) y = 2107;
    date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
    tm = (uint16_t)((secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2);
}

// ---------------------------------------------------------------- directories
// A directory is its first cluster; 0 is the fixed-size root directory.
// Entries are addressed by index (16 per sector).
bool dir_lba(uint32_t d, uint32_t k, uint32_t& lba) {
    if (d == 0) { if (k >= v.root_secs) return false; lba = v.root_lba + k; return true; }
    uint32_t c = d;
    for (uint32_t i = k / v.spc; i; i--) { c = fat_get(c); if (!valid(c)) return false; }
    lba = clus_lba(c) + k % v.spc;
    return true;
}
Dirent* ent_get(uint32_t d, uint32_t idx) {
    uint32_t lba;
    if (!dir_lba(d, idx / 16, lba)) return nullptr;
    uint8_t* s = sec_get(lba);
    return s ? (Dirent*)(s + (idx % 16) * 32) : nullptr;
}
bool ent_put(uint32_t d, uint32_t idx, const void* e) {
    Dirent* p = ent_get(d, idx);
    if (!p) return false;
    memcpy(p, e, 32);
    cache_dirty = true;
    return true;
}

uint8_t lfn_sum(const uint8_t* n) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n[i]);
    return s;
}
std::string short_name(const Dirent& e) {
    std::string s;
    for (int i = 0; i < 8 && e.name[i] != ' '; i++) {
        char c = (char)(i == 0 && e.name[0] == 0x05 ? 0xE5 : e.name[i]);
        s += (e.ntres & NT_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e.name[8] != ' ') {
        s += '.';
        for (int i = 8; i < 11 && e.name[i] != ' '; i++) {
            char c = (char)e.name[i];
            s += (e.ntres & NT_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    return s;
}
uint32_t first_cluster(const Dirent& e) { return e.clus; }

struct Entry {
    Dirent e;
    std::string name;
    uint32_t idx;      // the 8.3 entry
    uint32_t first;    // first entry belonging to it (its first LFN entry, or idx)
};

// Visit every live entry of directory d (volume labels skipped). fn returns
// false to stop. Returns false only on a disk error.
template <class F> bool dir_walk(uint32_t d, F fn) {
    uint16_t lfn[20 * 13 + 1];
    int lfn_next = 0;                   // the next ordinal expected (0: none in progress)
    uint8_t lfn_csum = 0;
    uint32_t lfn_first = 0;
    for (uint32_t idx = 0;; idx++) {
        uint32_t lba;
        if (!dir_lba(d, idx / 16, lba)) return true;
        Dirent* p = ent_get(d, idx);
        if (!p) return false;
        Dirent e = *p;
        if (e.name[0] == 0x00) return true;
        if (e.name[0] == 0xE5) { lfn_next = 0; continue; }
        if (e.attr == A_LFN) {
            const LfnEnt& l = (const LfnEnt&)e;
            int ord = l.ord & 0x1F;
            if (l.ord & 0x40) {
                if (ord < 1 || ord > 20) { lfn_next = 0; continue; }
                for (auto& ch : lfn) ch = 0;
                lfn_next = ord; lfn_csum = l.sum; lfn_first = idx;
            } else if (ord != lfn_next || l.sum != lfn_csum) { lfn_next = 0; continue; }
            uint16_t* o = &lfn[(ord - 1) * 13];
            memcpy(o, l.n1, 10); memcpy(o + 5, l.n2, 12); memcpy(o + 11, l.n3, 4);
            lfn_next = ord - 1;
            if (lfn_next == 0) lfn_next = -1;            // complete; the 8.3 entry comes next
            continue;
        }
        if (e.attr & A_VOL) { lfn_next = 0; continue; }
        Entry en{e, {}, idx, idx};
        if (lfn_next == -1 && lfn_sum(e.name) == lfn_csum) {
            for (int i = 0; i < 20 * 13 && lfn[i] && lfn[i] != 0xFFFF; i++)
                en.name += lfn[i] < 0x80 ? (char)lfn[i] : '?';
            en.first = lfn_first;
        } else {
            en.name = short_name(e);
        }
        lfn_next = 0;
        if (!fn(en)) return true;
    }
}

bool same_name(const std::string& a, const char* b, size_t n) {
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
    }
    return true;
}
// 0 = not found, 1 = found, -1 = disk error.
int dir_find(uint32_t d, const char* name, size_t n, Entry& out) {
    bool hit = false;
    bool ok = dir_walk(d, [&](const Entry& en) {
        if (same_name(en.name, name, n)) { out = en; hit = true; return false; }
        return true;
    });
    return !ok ? -1 : hit ? 1 : 0;
}

// Split a path into directory (cluster) and final component. Missing
// directories fail unless `make` is set, in which case they're created.
bool make_dir_in(uint32_t parent, const std::string& name, uint32_t& made);
bool resolve_parent(const char* path, uint32_t& d, std::string& leaf, bool make = false) {
    d = 0;
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;
        const char* e = p;
        while (*e && *e != '/') e++;
        const char* rest = e;
        while (*rest == '/') rest++;
        if (!*rest) { leaf.assign(p, e - p); return true; }  // last component
        size_t n = e - p;
        if (n == 1 && p[0] == '.') { p = e; continue; }
        Entry en;
        int r = dir_find(d, p, n, en);
        if (r < 0) return false;
        if (r == 0) {
            if (!make || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
            uint32_t made;
            if (!make_dir_in(d, std::string(p, n), made)) return false;
            d = made;
        } else {
            if (!(en.e.attr & A_DIR)) return false;
            d = first_cluster(en.e);                     // ".." to the root is cluster 0
        }
        p = e;
    }
}
// Look up a whole path. The root itself is reported as a directory with d = 0.
int lookup(const char* path, uint32_t& parent, Entry& en, bool& is_root) {
    std::string leaf;
    is_root = false;
    if (!resolve_parent(path, parent, leaf)) return 0;
    if (leaf.empty() || leaf == ".") {
        if (parent == 0) { is_root = true; return 1; }
        // "dir/" or "dir/.": the "." entry names the directory but has no
        // parent link we can edit; callers needing that use the real name.
        return dir_find(parent, ".", 1, en);
    }
    return dir_find(parent, leaf.data(), leaf.size(), en);
}

// ---- names
bool sfn_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c & 0x80) || strchr("!#$%&'()-@^_`{}~", c);
}
bool valid_long(const std::string& s) {
    if (s.empty() || s.size() > 255 || s == "." || s == "..") return false;
    for (char c : s) if ((unsigned char)c < 0x20 || strchr("\"*/:<>?\\|", c)) return false;
    return s.back() != ' ' && s.back() != '.';
}
// The name fits in an 8.3 entry as is (using the lowercase flags for an
// all-lowercase base or extension).
bool exact_83(const std::string& s, uint8_t out[11], uint8_t& nt) {
    size_t dot = s.find('.');
    std::string base = s.substr(0, dot), ext = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return false;
    if (dot != std::string::npos && ext.empty()) return false;
    nt = 0;
    auto part = [&](const std::string& p, uint8_t* o, size_t w, uint8_t flag) {
        bool lo = false, up = false;
        for (size_t i = 0; i < w; i++) o[i] = ' ';
        for (size_t i = 0; i < p.size(); i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') { lo = true; c -= 32; } else if (c >= 'A' && c <= 'Z') up = true;
            if (!sfn_char(c)) return false;
            o[i] = (uint8_t)c;
        }
        if (lo && up) return false;
        if (lo) nt |= flag;
        return true;
    };
    if (!part(base, out, 8, NT_LOWER_BASE) || !part(ext, out + 8, 3, NT_LOWER_EXT)) return false;
    if (out[0] == 0xE5) out[0] = 0x05;
    return true;
}
bool sfn_taken(uint32_t d, const uint8_t n[11], bool& err) {
    bool taken = false;
    err = !dir_walk(d, [&](const Entry& en) {
        if (!memcmp(en.e.name, n, 11)) { taken = true; return false; }
        return true;
    });
    return taken;
}
// A unique 8.3 alias in the Windows style: NAME~1.EXT, NAME~2.EXT, ...
bool make_alias(uint32_t d, const std::string& s, uint8_t out[11]) {
    std::string base, ext;
    size_t dot = s.rfind('.');
    if (dot == 0) dot = std::string::npos;
    auto clean = [](char c) -> char {
        if (c >= 'a' && c <= 'z') return (char)(c - 32);
        return sfn_char(c) ? c : '_';
    };
    for (size_t i = 0; i < (dot == std::string::npos ? s.size() : dot); i++)
        if (s[i] != ' ' && s[i] != '.' && base.size() < 8) base += clean(s[i]);
    if (dot != std::string::npos)
        for (size_t i = dot + 1; i < s.size() && ext.size() < 3; i++)
            if (s[i] != ' ' && s[i] != '.') ext += clean(s[i]);
    if (base.empty()) base = "_";
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[9];
        int tl = 0;
        tail[tl++] = '~';
        char digits[8]; int dl = 0;
        for (uint32_t x = n; x; x /= 10) digits[dl++] = (char)('0' + x % 10);
        while (dl) tail[tl++] = digits[--dl];
        std::string b = base.substr(0, 8 - tl) + std::string(tail, tl);
        memset(out, ' ', 11);
        memcpy(out, b.data(), b.size());
        memcpy(out + 8, ext.data(), ext.size());
        if (out[0] == 0xE5) out[0] = 0x05;
        bool err;
        if (!sfn_taken(d, out, err)) return !err;
    }
    return false;
}

// Make room for `n` consecutive entries in directory d; returns the index.
bool find_slots(uint32_t d, uint32_t n, uint32_t& at) {
    uint32_t run = 0, start = 0;
    for (uint32_t idx = 0;; idx++) {
        Dirent* p = ent_get(d, idx);
        if (!p) {
            uint32_t lba;
            if (dir_lba(d, idx / 16, lba)) return false;  // disk error, not the end
            if (d == 0) return false;                    // the root directory is full
            // Grow the directory by one zeroed cluster.
            std::vector<uint32_t> cl, got;
            if (!chain(d, cl) || !alloc_chain(1, got)) return false;
            std::vector<uint8_t> z(v.csize, 0);
            if (!raw_write(clus_lba(got[0]), v.spc, z.data())) { fat_set(got[0], 0); return false; }
            fat_set(cl.back(), got[0]);
            if (!fat_flush()) return false;
            if (run == 0) start = idx;
            at = start;                                   // the run continues into the new cluster
            return true;
        }
        if (p->name[0] == 0x00 || p->name[0] == 0xE5) {
            if (run++ == 0) start = idx;
            if (run == n) { at = start; return true; }
        } else {
            run = 0;
        }
    }
}

// Add an entry named `name` to directory d, filled from `tmpl` (attr, cluster,
// size, times). Leaves the directory sector in the cache; caller flushes.
bool dir_add(uint32_t d, const std::string& name, const Dirent& tmpl, uint32_t* idx_out = nullptr) {
    if (!valid_long(name)) return false;
    Dirent e = tmpl;
    uint8_t nt = 0;
    uint32_t nlfn = 0;
    bool err = false;
    if (exact_83(name, e.name, nt) && !sfn_taken(d, e.name, err)) {
        e.ntres = nt;
    } else {
        if (err || !make_alias(d, name, e.name)) return false;
        e.ntres = 0;
        nlfn = (uint32_t)(name.size() + 12) / 13;
    }
    uint32_t at;
    if (!find_slots(d, nlfn + 1, at)) return false;
    uint8_t sum = lfn_sum(e.name);
    for (uint32_t i = 0; i < nlfn; i++) {
        uint32_t ord = nlfn - i;
        LfnEnt l;
        memset(&l, 0, sizeof l);
        l.ord = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        l.attr = A_LFN;
        l.sum = sum;
        uint16_t ch[13];
        for (int k = 0; k < 13; k++) {
            size_t pos = (ord - 1) * 13 + k;
            ch[k] = pos < name.size() ? (uint8_t)name[pos] : pos == name.size() ? 0x0000 : 0xFFFF;
        }
        memcpy(l.n1, ch, 10); memcpy(l.n2, ch + 5, 12); memcpy(l.n3, ch + 11, 4);
        if (!ent_put(d, at + i, &l)) return false;
    }
    if (!ent_put(d, at + nlfn, &e)) return false;
    if (idx_out) *idx_out = at + nlfn;
    return true;
}
bool dir_del(uint32_t d, const Entry& en) {
    for (uint32_t i = en.first; i <= en.idx; i++) {
        Dirent* p = ent_get(d, i);
        if (!p) return false;
        p->name[0] = 0xE5;
        cache_dirty = true;
    }
    return true;
}
Dirent new_dirent(uint8_t attr, uint32_t clus, uint32_t size) {
    Dirent e;
    memset(&e, 0, sizeof e);
    e.attr = attr;
    e.clus = (uint16_t)clus;
    e.size = size;
    uint16_t dt, tm;
    stamp(dt, tm);
    e.cdate = dt; e.ctime = tm;
    e.mdate = e.adate = e.cdate;
    e.mtime = e.ctime;
    return e;
}

bool make_dir_in(uint32_t parent, const std::string& name, uint32_t& made) {
    if (!valid_long(name)) return false;
    std::vector<uint32_t> got;
    if (!alloc_chain(1, got)) return false;
    uint32_t c = got[0];
    std::vector<uint8_t> buf(v.csize, 0);
    Dirent dot = new_dirent(A_DIR, c, 0), dotdot = new_dirent(A_DIR, parent, 0);
    memset(dot.name, ' ', 11); dot.name[0] = '.';
    memset(dotdot.name, ' ', 11); dotdot.name[0] = dotdot.name[1] = '.';
    memcpy(&buf[0], &dot, 32);
    memcpy(&buf[32], &dotdot, 32);
    if (!raw_write(clus_lba(c), v.spc, buf.data()) || !fat_flush()) { fat_set(c, 0); return false; }
    if (!dir_add(parent, name, new_dirent(A_DIR, c, 0)) || !cache_flush()) {
        fat_set(c, 0); fat_flush();
        return false;
    }
    made = c;
    return true;
}

bool dir_empty(uint32_t d, bool& empty) {
    empty = true;
    return dir_walk(d, [&](const Entry& en) {
        if (en.name == "." || en.name == "..") return true;
        empty = false;
        return false;
    });
}
// The ".." cluster of a subdirectory (0 for the root's children).
bool parent_of(uint32_t d, uint32_t& p) {
    Dirent* e = ent_get(d, 1);
    if (!e) return false;
    p = e->clus;
    return true;
}

struct Op { ~Op() { cache_flush(); idle(); } };   // end of every public call

} // namespace

// ---------------------------------------------------------------- public API
__attribute__((weak)) long fat_clock() { return (long)time(nullptr); }

bool fat_mount(const FatDisk& disk) {
    fat_unmount();
    v.disk = disk;
    Op op;
    uint8_t b[SEC];
    if (!v.disk.read(0, 1, b)) return false;
    auto u16 = [&](int o) { return (uint32_t)(b[o] | b[o + 1] << 8); };
    auto u32 = [&](int o) { return u16(o) | u16(o + 2) << 16; };
    uint32_t bps = u16(11), spc = b[13], rsv = u16(14), nfats = b[16], roots = u16(17);
    uint32_t total = u16(19) ? u16(19) : u32(32), fatsz = u16(22);
    if (bps != SEC || !spc || (spc & (spc - 1)) || !rsv || !nfats || !fatsz || !roots) return false;
    v.spc = spc; v.csize = spc * SEC;
    v.fat_lba = rsv; v.fat_secs = fatsz; v.nfats = nfats;
    v.root_lba = rsv + nfats * fatsz;
    v.root_secs = (roots * 32 + SEC - 1) / SEC;
    v.data_lba = v.root_lba + v.root_secs;
    if (total <= v.data_lba) return false;
    v.nclus = (total - v.data_lba) / spc;
    if (v.nclus < 1 || v.nclus >= 4085) return false;            // FAT16/32, not FAT12
    if ((uint64_t)fatsz * SEC * 2 / 3 < v.nclus + 2) return false;
    v.fat.assign(fatsz * SEC + 1, 0);                             // +1: odd last entry reads a byte past
    v.fat_dirty.assign(fatsz, 0);
    bool ok = false;
    for (uint32_t k = 0; k < nfats && !ok; k++) ok = v.disk.read(v.fat_lba + k * fatsz, fatsz, v.fat.data());
    if (!ok) return false;
    cache_lba = NONE; cache_dirty = false;
    v.hint = 2;
    v.mounted = true;
    return true;
}
bool fat_mounted() { return v.mounted; }
void fat_unmount() {
    if (v.mounted) { cache_flush(); fat_flush(); idle(); }
    v.mounted = false;
    cache_lba = NONE; cache_dirty = false;
}

bool fat_exists(const char* path, bool* is_dir, uint32_t* size) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1) return false;
    if (is_dir) *is_dir = root || (en.e.attr & A_DIR);
    if (size) *size = root ? 0 : en.e.size;
    return true;
}

bool fat_read(const char* path, std::string& out) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1 || root || (en.e.attr & A_DIR)) return false;
    std::vector<uint32_t> cl;
    if (!chain(first_cluster(en.e), cl)) return false;
    if ((uint64_t)cl.size() * v.csize < en.e.size) return false;
    return read_clusters(cl, en.e.size, out);
}

bool fat_write(const char* path, const void* data, size_t len) {
    if (!v.mounted || len > 0xFFFFFFFFu) return false;
    Op op;
    uint32_t d;
    std::string leaf;
    if (!resolve_parent(path, d, leaf)) return false;
    Entry en;
    int r = dir_find(d, leaf.data(), leaf.size(), en);
    if (r < 0 || (r == 1 && (en.e.attr & (A_DIR | A_RO)))) return false;
    if (r == 0 && !valid_long(leaf)) return false;

    // 1. Data into fresh clusters, then the FAT that links them.
    std::vector<uint32_t> cl;
    uint32_t n = (uint32_t)((len + v.csize - 1) / v.csize);
    uint32_t c = alloc_chain(n, cl);
    if (n && !c) return false;
    if (!write_clusters(cl, data, len) || !fat_flush()) { free_chain(c); fat_flush(); return false; }

    // 2. Point the directory entry at them.
    if (r == 1) {
        Dirent* p = ent_get(d, en.idx);
        if (!p) { free_chain(c); fat_flush(); return false; }
        uint32_t old = first_cluster(*p);
        p->clus = (uint16_t)c;
        p->size = (uint32_t)len;
        p->attr |= A_ARC;
        uint16_t dt, tm;
        stamp(dt, tm);
        p->mdate = dt; p->mtime = tm;
        p->adate = p->mdate;
        cache_dirty = true;
        if (!cache_flush()) return false;
        // 3. Only now release the old data.
        free_chain(old);
        return fat_flush();
    }
    if (!dir_add(d, leaf, new_dirent(A_ARC, c, (uint32_t)len)) || !cache_flush()) {
        free_chain(c); fat_flush();
        return false;
    }
    return true;
}

bool fat_append(const char* path, const void* data, size_t len) {
    std::string cur;
    bool dir;
    if (fat_exists(path, &dir) && (dir || !fat_read(path, cur))) return false;
    cur.append((const char*)data, len);
    return fat_write(path, cur.data(), cur.size());
}

bool fat_mkdir(const char* path) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d;
    std::string leaf;
    if (!resolve_parent(path, d, leaf, true)) return false;
    if (leaf.empty() || leaf == ".") return true;
    Entry en;
    int r = dir_find(d, leaf.data(), leaf.size(), en);
    if (r < 0) return false;
    if (r == 1) return (en.e.attr & A_DIR) != 0;
    uint32_t made;
    return make_dir_in(d, leaf, made);
}

bool fat_remove(const char* path) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1 || root || en.name == "." || en.name == "..") return false;
    if (en.e.attr & A_DIR) {
        bool empty;
        if (!dir_empty(first_cluster(en.e), empty) || !empty) return false;
    }
    if (!dir_del(d, en) || !cache_flush()) return false;
    free_chain(first_cluster(en.e));
    return fat_flush();
}

bool fat_rename(const char* from, const char* to) {
    if (!v.mounted) return false;
    Op op;
    uint32_t sd; Entry src; bool root;
    if (lookup(from, sd, src, root) != 1 || root || src.name == "." || src.name == "..") return false;
    uint32_t dd;
    std::string leaf;
    if (!resolve_parent(to, dd, leaf) || !valid_long(leaf)) return false;
    bool is_dir = src.e.attr & A_DIR;
    if (is_dir) {                                       // not into itself or a descendant
        for (uint32_t p = dd, n = 0; p != 0; n++) {
            if (p == first_cluster(src.e) || n > v.nclus) return false;
            if (!parent_of(p, p)) return false;
        }
    }
    Entry dst;
    int r = dir_find(dd, leaf.data(), leaf.size(), dst);
    if (r < 0) return false;
    bool same = r == 1 && dd == sd && dst.idx == src.idx;
    if (r == 1 && !same) {
        if (is_dir || (dst.e.attr & (A_DIR | A_RO))) return false;
        if (!dir_del(dd, dst) || !cache_flush()) return false;      // replace an existing file
        free_chain(first_cluster(dst.e));
        if (!fat_flush()) return false;
    }
    if (same) {                                         // only the case changes
        if (!dir_del(sd, src) || !dir_add(dd, leaf, src.e)) return false;
        return cache_flush();
    }
    if (!dir_add(dd, leaf, src.e) || !cache_flush()) return false;
    // The source entry may have moved if the directory grew; find it again.
    std::string sleaf;
    uint32_t sd2;
    Entry again;
    if (!resolve_parent(from, sd2, sleaf) || dir_find(sd2, sleaf.data(), sleaf.size(), again) != 1) return false;
    if (!dir_del(sd2, again)) return false;
    if (is_dir && sd != dd) {                           // fix the moved directory's ".."
        Dirent* p = ent_get(first_cluster(src.e), 1);
        if (!p) return false;
        p->clus = (uint16_t)dd;
        cache_dirty = true;
    }
    return cache_flush();
}

bool fat_list(const char* path, bool (*fn)(const char*, uint32_t, bool, void*), void* ctx) {
    if (!v.mounted) return false;
    Op op;
    uint32_t d; Entry en; bool root;
    if (lookup(path, d, en, root) != 1) return false;
    uint32_t dir = root ? 0 : first_cluster(en.e);
    if (!root && !(en.e.attr & A_DIR)) return false;
    std::vector<Entry> all;                          // collect first: fn may touch the disk
    if (!dir_walk(dir, [&](const Entry& e) {
            if (e.name != "." && e.name != "..") all.push_back(e);
            return true;
        }))
        return false;
    for (auto& e : all)
        if (!fn(e.name.c_str(), e.e.size, (e.e.attr & A_DIR) != 0, ctx)) break;
    return true;
}

uint32_t fat_free_bytes() {
    if (!v.mounted) return 0;
    uint32_t n = 0;
    for (uint32_t c = 2; c <= v.nclus + 1; c++) n += fat_get(c) == 0;
    return n * v.csize;
}
