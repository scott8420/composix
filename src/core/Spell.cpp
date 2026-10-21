// core::spell -- see Spell.hpp (s306).
#include "core/Spell.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#ifdef COMPOSIX_HAVE_HUNSPELL
#include <hunspell/hunspell.h>
#endif

namespace composix::core::spell {

namespace {
namespace fs = std::filesystem;

struct State {
#ifdef COMPOSIX_HAVE_HUNSPELL
    Hunhandle* h = nullptr;
#endif
    std::string lang;
    bool        latin1 = false;   // the dictionary's words in ISO 8859-1 (else UTF-8)
    std::string user_file;
    std::set<std::string> user, ignored;
    ~State() {
#ifdef COMPOSIX_HAVE_HUNSPELL
        if (h) Hunspell_destroy(h);
#endif
    }
};
State& st() { static State s; return s; }

// One code point from UTF-8 at i (advanced past it); 0xFFFD on a bad byte.
char32_t next_cp(const std::string& t, std::size_t& i) {
    const auto c = static_cast<unsigned char>(t[i]);
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + static_cast<std::size_t>(n) > t.size()) n = 1;
    char32_t cp = n == 1 ? (c < 0x80 ? c : 0xFFFD) : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(t[i + static_cast<std::size_t>(k)]) & 0x3F);
    i += static_cast<std::size_t>(n);
    return cp;
}

bool letter(char32_t c) {
    if (c < 0x80) return std::isalnum(static_cast<int>(c)) != 0;
    if (c < 0xC0) return c == 0xAD;                       // a soft hyphen stays inside its word
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;         // punctuation, symbols, arrows, maths
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xE000 && c <= 0xF8FF) return false;         // the app's marks
    if (c >= 0xFE00 && c <= 0xFE0F) return false;
    if (c == 0xFEFF || c == 0xFFFD) return false;
    return true;
}
bool apostrophe(char32_t c) { return c == '\'' || c == 0x2019; }

// The word as the dictionary takes it: ’ as ', soft hyphens out.
std::string plain(const std::string& w) {
    std::string out;
    for (std::size_t i = 0; i < w.size();) {
        const std::size_t a = i;
        const char32_t c = next_cp(w, i);
        if (c == 0x2019) out += '\'';
        else if (c == 0xAD) continue;
        else out.append(w, a, i - a);
    }
    return out;
}

#ifdef COMPOSIX_HAVE_HUNSPELL
// To the dictionary's encoding; false when it can't say the word (then it's left alone).
bool to_dic(const std::string& w, std::string& out) {
    if (!st().latin1) { out = w; return true; }
    out.clear();
    for (std::size_t i = 0; i < w.size();) {
        const char32_t c = next_cp(w, i);
        if (c > 0xFF) return false;
        out += static_cast<char>(c);
    }
    return true;
}
std::string from_dic(const std::string& w) {
    if (!st().latin1) return w;
    std::string out;
    for (unsigned char c : w) {
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}
#endif

std::vector<std::string> search_dirs(const std::vector<std::string>& extra) {
    std::vector<std::string> out = extra;
    if (const char* home = std::getenv("HOME")) out.push_back(std::string(home) + "/.local/share/hunspell");
    for (const char* d : {"/usr/share/hunspell", "/usr/local/share/hunspell", "/usr/share/myspell", "/usr/share/myspell/dicts"}) out.push_back(d);
    return out;
}

void write_user() {
    auto& s = st();
    if (s.user_file.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(s.user_file).parent_path(), ec);
    std::ofstream out(s.user_file, std::ios::trunc);
    for (const auto& w : s.user) out << w << '\n';
}

// A whitespace-delimited chunk that reads as an address: its words left alone.
bool address(const std::string& chunk) {
    return chunk.find("://") != std::string::npos || chunk.find("www.") != std::string::npos ||
           (chunk.find('@') != std::string::npos && chunk.find('.') != std::string::npos);
}
}  // namespace

bool built_in() {
#ifdef COMPOSIX_HAVE_HUNSPELL
    return true;
#else
    return false;
#endif
}

bool load(const std::string& lang, std::string* err, const std::vector<std::string>& dirs) {
#ifdef COMPOSIX_HAVE_HUNSPELL
    for (const auto& d : search_dirs(dirs)) {
        const auto aff = d + "/" + lang + ".aff", dic = d + "/" + lang + ".dic";
        std::error_code ec;
        if (!fs::exists(aff, ec) || !fs::exists(dic, ec)) continue;
        Hunhandle* h = Hunspell_create(aff.c_str(), dic.c_str());
        if (!h) continue;
        auto& s = st();
        if (s.h) Hunspell_destroy(s.h);
        s.h = h;
        s.lang = lang;
        std::string enc = Hunspell_get_dic_encoding(h) ? Hunspell_get_dic_encoding(h) : "UTF-8";
        std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        s.latin1 = enc.find("8859-1") != std::string::npos || enc == "ISO8859-1" || enc == "LATIN1";
        for (const auto& w : s.user) { std::string dw; if (to_dic(w, dw)) Hunspell_add(h, dw.c_str()); }
        return true;
    }
    if (err) *err = "No " + lang + " dictionary found (install hunspell-" + lang + ")";
    return false;
#else
    (void)dirs;
    if (err) *err = "Spell check isn't built in (Hunspell wasn't found when the app was built) -- " + lang;
    return false;
#endif
}

bool loaded() {
#ifdef COMPOSIX_HAVE_HUNSPELL
    return st().h != nullptr;
#else
    return false;
#endif
}
std::string language() { return loaded() ? st().lang : std::string(); }

std::vector<std::string> languages(const std::vector<std::string>& dirs) {
    std::set<std::string> out;
    for (const auto& d : search_dirs(dirs)) {
        std::error_code ec;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            const auto p = it->path();
            if (p.extension() != ".dic") continue;
            auto aff = p;
            aff.replace_extension(".aff");
            if (fs::exists(aff, ec)) out.insert(p.stem().string());
        }
    }
    return {out.begin(), out.end()};
}

void set_user_file(const std::string& path) {
    auto& s = st();
    s.user_file = path;
    if (path.empty()) return;
    std::ifstream in(path);
    for (std::string w; std::getline(in, w);) {
        while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
        if (w.empty()) continue;
        s.user.insert(w);
#ifdef COMPOSIX_HAVE_HUNSPELL
        if (s.h) { std::string dw; if (to_dic(w, dw)) Hunspell_add(s.h, dw.c_str()); }
#endif
    }
}

bool add_word(const std::string& word) {
    const auto w = plain(word);
    if (w.empty() || !st().user.insert(w).second) return false;
#ifdef COMPOSIX_HAVE_HUNSPELL
    if (st().h) { std::string dw; if (to_dic(w, dw)) Hunspell_add(st().h, dw.c_str()); }
#endif
    write_user();
    return true;
}
bool remove_word(const std::string& word) {
    const auto w = plain(word);
    if (!st().user.erase(w)) return false;
#ifdef COMPOSIX_HAVE_HUNSPELL
    if (st().h) { std::string dw; if (to_dic(w, dw)) Hunspell_remove(st().h, dw.c_str()); }
#endif
    write_user();
    return true;
}
std::vector<std::string> user_words() { return {st().user.begin(), st().user.end()}; }
void ignore(const std::string& word) { st().ignored.insert(plain(word)); }
void clear_ignored() { st().ignored.clear(); }

bool good(const std::string& word) {
    const auto w = plain(word);
    if (w.empty() || st().user.count(w) || st().ignored.count(w)) return true;
#ifdef COMPOSIX_HAVE_HUNSPELL
    if (!st().h) return true;
    std::string dw;
    if (!to_dic(w, dw)) return true;
    return Hunspell_spell(st().h, dw.c_str()) != 0;
#else
    return true;
#endif
}

std::vector<std::string> suggest(const std::string& word, int max) {
    std::vector<std::string> out;
#ifdef COMPOSIX_HAVE_HUNSPELL
    std::string dw;
    if (!st().h || !to_dic(plain(word), dw)) return out;
    char** list = nullptr;
    const int n = Hunspell_suggest(st().h, &list, dw.c_str());
    for (int k = 0; k < n && static_cast<int>(out.size()) < max; ++k) out.push_back(from_dic(list[k]));
    if (list) Hunspell_free_list(st().h, &list, n);
    // the typed word's curly apostrophe back on the suggestions
    if (word.find("’") != std::string::npos)
        for (auto& s : out)
            for (std::size_t p = s.find('\''); p != std::string::npos; p = s.find('\'', p + 3)) s.replace(p, 1, "’");
#else
    (void)word;
    (void)max;
#endif
    return out;
}

std::vector<Word> words_of(const std::string& text, int para) {
    std::vector<Word> out;
    // whitespace chunks first: an address's words are left alone
    std::vector<std::pair<std::size_t, std::size_t>> skip;
    for (std::size_t a = 0; a < text.size();) {
        while (a < text.size() && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
        std::size_t b = a;
        while (b < text.size() && !std::isspace(static_cast<unsigned char>(text[b]))) ++b;
        if (b > a && address(text.substr(a, b - a))) skip.push_back({a, b});
        a = b;
    }
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t at = i;
        char32_t c = next_cp(text, i);
        if (!letter(c)) continue;
        const std::size_t from = at;
        std::size_t end = i;
        bool digit = c < 0x80 && std::isdigit(static_cast<int>(c));
        while (i < text.size()) {
            at = i;
            c = next_cp(text, i);
            if (letter(c)) { end = i; digit = digit || (c < 0x80 && std::isdigit(static_cast<int>(c))); continue; }
            if (apostrophe(c) && i < text.size()) {   // inside a word only: a letter after it
                std::size_t j = i;
                if (letter(next_cp(text, j))) { end = j; i = j; continue; }
            }
            i = at;
            break;
        }
        i = std::max(i, end);
        const bool skipped = std::any_of(skip.begin(), skip.end(), [&](const auto& s) { return from >= s.first && from < s.second; });
        if (!digit && !skipped)
            out.push_back({para, static_cast<int>(from), static_cast<int>(end), text.substr(from, end - from)});
    }
    return out;
}

std::vector<Word> misses(const Story& s) {
    std::vector<Word> out;
    for (std::size_t p = 0; p < s.paras.size(); ++p)
        for (auto& w : words_of(s.paras[p].text, static_cast<int>(p)))
            if (!good(w.text)) out.push_back(std::move(w));
    return out;
}

Word next_miss(const Story& s, TextPos from, bool wrap) {
    for (int pass = 0; pass < (wrap ? 2 : 1); ++pass)
        for (std::size_t p = pass == 0 ? static_cast<std::size_t>(std::max(0, from.para)) : 0; p < s.paras.size(); ++p) {
            if (pass == 1 && static_cast<int>(p) > from.para) break;
            for (auto& w : words_of(s.paras[p].text, static_cast<int>(p))) {
                if (pass == 0 && static_cast<int>(p) == from.para && w.to <= from.byte) continue;
                if (pass == 0 && static_cast<int>(p) == from.para && w.from < from.byte) continue;   // the word the caret is in: past it
                if (pass == 1 && static_cast<int>(p) == from.para && w.from >= from.byte) return {-1, 0, 0, {}};
                if (!good(w.text)) return w;
            }
        }
    return {-1, 0, 0, {}};
}

}  // namespace composix::core::spell
