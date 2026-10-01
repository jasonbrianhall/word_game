// Tiny stand-ins for <fstream> and <sstream>.
//
// The real ones need libstdc++'s locale and file machinery, which a bare-metal
// kernel doesn't have. letterlock.cpp keeps its statistics in two small text
// files; here they live in RAM, backed by the boot floppy when there is one
// (storage.cpp: platform_file_load/store), and are read back with just
// the `>>` extraction and `<<` insertion the game uses. These are explicit
// specializations of the <iosfwd> class templates for char; the primary
// templates are never defined.
#pragma once
#include <iosfwd>
#include <string>
#include <map>

// storage.cpp: persistent copies of the files, when a disk is available.
bool platform_file_load(const char* path, std::string& out);
void platform_file_store(const char* path, const std::string& all, size_t from, bool append);

namespace std _GLIBCXX_VISIBILITY(default) {

inline map<string, string>& __ramfs() { static map<string, string> files; return files; }

template<> class basic_ios<char, char_traits<char>> {
public:
    enum openmode_ { in = 1, out = 2, binary = 4, trunc = 8, app = 16, ate = 32 };
    enum seekdir_ { beg = 0, cur = 1, end = 2 };
};

template<> class basic_ifstream<char, char_traits<char>> {
public:
    basic_ifstream() {}
    explicit basic_ifstream(const char* p, int = 0) { open(p); }
    explicit basic_ifstream(const string& p, int = 0) { open(p.c_str()); }
    void open(const char* p) {
        auto it = __ramfs().find(p);
        if (it == __ramfs().end()) {
            string s;
            if (!platform_file_load(p, s)) { fail_ = true; return; }
            it = __ramfs().emplace(p, std::move(s)).first;
        }
        buf_ = it->second; pos_ = 0; fail_ = false; open_ = true;
    }
    bool good() const { return open_ && !fail_; }
    bool is_open() const { return open_; }
    explicit operator bool() const { return !fail_; }
    bool operator!() const { return fail_; }
    void close() { open_ = false; }

    basic_ifstream& operator>>(string& s) {
        skip_ws();
        if (pos_ >= buf_.size()) { fail_ = true; return *this; }
        size_t b = pos_;
        while (pos_ < buf_.size() && !ws(buf_[pos_])) pos_++;
        s.assign(buf_, b, pos_ - b);
        return *this;
    }
    basic_ifstream& operator>>(int& v) {
        skip_ws();
        bool neg = pos_ < buf_.size() && buf_[pos_] == '-';
        if (neg || (pos_ < buf_.size() && buf_[pos_] == '+')) pos_++;
        if (pos_ >= buf_.size() || buf_[pos_] < '0' || buf_[pos_] > '9') { fail_ = true; return *this; }
        long n = 0;
        while (pos_ < buf_.size() && buf_[pos_] >= '0' && buf_[pos_] <= '9') n = n * 10 + (buf_[pos_++] - '0');
        v = (int)(neg ? -n : n);
        return *this;
    }
    basic_ifstream& ignore(long n = 1, int delim = -1) {
        while (n-- > 0 && pos_ < buf_.size())
            if ((unsigned char)buf_[pos_++] == delim) break;
        return *this;
    }
private:
    static bool ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
    void skip_ws() { while (pos_ < buf_.size() && ws(buf_[pos_])) pos_++; }
    string buf_;
    size_t pos_ = 0;
    bool fail_ = true, open_ = false;
};

template<> class basic_ofstream<char, char_traits<char>> {
public:
    basic_ofstream() {}
    explicit basic_ofstream(const char* p, int mode = 0) { open(p, mode); }
    explicit basic_ofstream(const string& p, int mode = 0) { open(p.c_str(), mode); }
    basic_ofstream(const basic_ofstream&) = delete;
    ~basic_ofstream() { close(); }
    // Written to disk on close (or destruction): appended bytes only for app.
    void open(const char* p, int mode = 0) {
        close();
        auto& fs = __ramfs();
        path_ = p;
        append_ = mode & basic_ios<char>::app;
        if (append_ && !fs.count(p)) {
            string s;
            if (platform_file_load(p, s)) fs.emplace(p, std::move(s));
        }
        file_ = &fs[p];
        if (!append_) file_->clear();
        from_ = file_->size();
    }
    bool good() const { return file_ != nullptr; }
    bool is_open() const { return file_ != nullptr; }
    explicit operator bool() const { return file_ != nullptr; }
    void close() {
        if (!file_) return;
        platform_file_store(path_.c_str(), *file_, from_, append_);
        file_ = nullptr;
    }
    basic_ofstream& operator<<(const string& s) { if (file_) *file_ += s; return *this; }
    basic_ofstream& operator<<(const char* s) { if (file_) *file_ += s; return *this; }
    basic_ofstream& operator<<(char c) { if (file_) *file_ += c; return *this; }
    basic_ofstream& operator<<(int v) { if (file_) *file_ += to_string(v); return *this; }
    basic_ofstream& operator<<(long v) { if (file_) *file_ += to_string(v); return *this; }
    basic_ofstream& operator<<(unsigned v) { if (file_) *file_ += to_string(v); return *this; }
private:
    string* file_ = nullptr;
    string path_;
    size_t from_ = 0;
    bool append_ = false;
};

} // namespace std
