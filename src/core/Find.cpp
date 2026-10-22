// core::doc::find_all / change_hits -- Find / Change (s022).
#include "core/Doc.hpp"
#include <array>
#include "core/Spell.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string_view>
#include <tuple>

namespace composix::core::doc {

namespace {


bool word_byte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

// The order stories read in: by the page their thread starts on.
std::vector<Doc::Id> reading_order(const Doc& d) {
    std::map<Doc::Id, double> page_at;
    const auto pages = d.pages();
    for (std::size_t i = 0; i < pages.size(); ++i) page_at[pages[i]] = static_cast<double>(i);
    std::vector<std::pair<double, Doc::Id>> keyed;
    for (const auto& sid : d.ids_of<Story>()) {
        double at = 1e9;   // unplaced: last
        for (const auto& fid : d.ids_of<Frame>()) {
            const auto& f = *d.as<Frame>(fid);
            if (f.kind != FrameKind::Text || f.story != sid) continue;
            const auto it = page_at.find(f.owner);
            at = std::min(at, it != page_at.end() ? it->second : 1e8);   // a master's: after the pages
        }
        keyed.emplace_back(at, sid);
    }
    std::stable_sort(keyed.begin(), keyed.end());
    std::vector<Doc::Id> out;
    for (auto& [k, id] : keyed) out.push_back(id);
    return out;
}

// s051: the stretches of a paragraph set bold / italic as asked (local
// over the character style), runs side by side joined.
struct Stretch { int a = 0, b = 0; };
std::vector<Stretch> format_stretches(const Doc& d, const Para& para, const FindOpts& f) {
    std::vector<Stretch> out;
    if (f.para_counts) {   // s160: the paragraph style's bold / italic too -- every byte, runs or not
        const Style ps = para_style(d, para);
        const int len = static_cast<int>(para.text.size());
        const auto add = [&](int a, int b) {
            if (b <= a) return;
            if (!out.empty() && out.back().b == a) out.back().b = b;
            else out.push_back({a, b});
        };
        const auto look = [&](int a, int b, const Run* r) {
            if (!f.cstyle.empty() && (!r || r->style != f.cstyle)) return;
            if (!f.skip_cstyle.empty() && r && r->style == f.skip_cstyle) return;
            const auto* cs = r ? d.as<Style>(r->style) : nullptr;
            const bool bo = r && r->local.bold >= 0 ? r->local.bold == 1 : ((cs && cs->bold) || ps.bold);
            const bool it = r && r->local.italic >= 0 ? r->local.italic == 1 : ((cs && cs->italic) || ps.italic);
            if ((f.bold && !bo) || (f.italic && !it)) return;
            add(a, b);
        };
        int at = 0;
        for (const auto& r : para.runs) {
            if (r.start > at) look(at, r.start, nullptr);
            look(std::max(at, r.start), r.end, &r);
            at = std::max(at, r.end);
        }
        if (at < len) look(at, len, nullptr);
        return out;
    }
    for (const auto& r : para.runs) {
        if (!f.cstyle.empty() && r.style != f.cstyle) continue;
        if (!f.skip_cstyle.empty() && r.style == f.skip_cstyle) continue;
        const auto* cs = d.as<Style>(r.style);
        const bool b = r.local.bold >= 0 ? r.local.bold == 1 : (cs && cs->bold);
        const bool i = r.local.italic >= 0 ? r.local.italic == 1 : (cs && cs->italic);
        if ((f.bold && !b) || (f.italic && !i) || r.start >= r.end) continue;
        if (!out.empty() && out.back().b == r.start) out.back().b = r.end;
        else out.push_back({r.start, r.end});
    }
    return out;
}

}  // namespace

char32_t fold_char(char32_t c) {   // s091
    const auto pair_even = [](char32_t x) { return (x % 2 == 0) ? x + 1 : x; };   // upper even, lower odd
    const auto pair_odd = [](char32_t x) { return (x % 2 == 1) ? x + 1 : x; };    // upper odd, lower even
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if (c == 0x17F) return 's';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return pair_odd(c);
        if (c == 0x131 || c == 0x138 || c == 0x149) return c;
        return pair_even(c);
    }
    if ((c >= 0x1CD && c <= 0x1DC)) return pair_odd(c);
    if ((c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) || (c >= 0x222 && c <= 0x233) || (c >= 0x246 && c <= 0x24F))
        return pair_even(c);
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c == 0x3C2) return 0x3C3;   // final sigma
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return pair_even(c);
    if (c == 0x4C0) return 0x4CF;
    if (c >= 0x4C1 && c <= 0x4CE) return pair_odd(c);
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c == 0x1E9E) return 0xDF;   // capital sharp s
    if (c >= 0x1E00 && c <= 0x1EFF && !(c >= 0x1E96 && c <= 0x1E9F)) return pair_even(c);
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}

namespace {
// s152: the characters whose full case fold is more than one letter
// (Unicode CaseFolding, status F): sharp s, the Latin and Armenian
// ligatures, n-apostrophe. nullptr: one for one (fold_char).
const wchar_t* fold_expansion(char32_t c) {
    switch (c) {
        case 0xDF:   case 0x1E9E: return L"ss";
        case 0x149:  return L"\u02BCn";
        case 0x587:  return L"\u0565\u0582";
        case 0xFB00: return L"ff";
        case 0xFB01: return L"fi";
        case 0xFB02: return L"fl";
        case 0xFB03: return L"ffi";
        case 0xFB04: return L"ffl";
        case 0xFB05: case 0xFB06: return L"st";
        case 0xFB13: return L"\u0574\u0576";
        case 0xFB14: return L"\u0574\u0565";
        case 0xFB15: return L"\u0574\u056B";
        case 0xFB16: return L"\u057E\u0576";
        case 0xFB17: return L"\u0574\u056D";
        default: return nullptr;
    }
}

// s152: a wide string fully folded, and where each folded character came
// from: src[i] is the index in the input of folded character i, src[size]
// the input's length. Folding off: the text as it is, indices one for one.
struct Folded {
    std::wstring w;
    std::vector<std::size_t> src;
    // A match [a, b) in the folded text, in the input -- or false when it
    // starts or ends inside one character's expansion ("s" in a sharp s).
    bool back(std::size_t a, std::size_t b, std::size_t& ia, std::size_t& ib) const {
        if (a > 0 && a < w.size() && src[a] == src[a - 1]) return false;
        if (b > 0 && b < w.size() && src[b] == src[b - 1]) return false;
        ia = src[a];
        ib = b < w.size() ? src[b] : src.back();
        return true;
    }
};
Folded fold_map(const std::wstring& in, bool fold = true) {
    Folded f;
    f.w.reserve(in.size());
    f.src.reserve(in.size() + 1);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto c = static_cast<char32_t>(in[i]);
        if (const wchar_t* x = fold ? fold_expansion(c) : nullptr) {
            for (; *x; ++x) { f.w.push_back(*x); f.src.push_back(i); }
        } else {
            f.w.push_back(fold ? static_cast<wchar_t>(fold_char(c)) : in[i]);
            f.src.push_back(i);
        }
    }
    f.src.push_back(in.size());
    return f;
}
// s091: a wide string folded (s152: fully -- a sharp s is "ss").
std::wstring folded(const std::wstring& w) { return fold_map(w).w; }
// ... a pattern folded, leaving what follows an escape (\D, \W, \S mean
// something else in upper case) -- `esc`: the escape character. s152: an
// expansion is wrapped as a group, so "ß+" stays one thing repeated.
// In a regex's [class] an expansion can't be a group -- there it folds one
// for one; in Wildcards (`regex` false) it is plain letters.
std::wstring folded_pattern(const std::wstring& w, wchar_t esc, bool regex) {
    std::wstring r;
    bool in_class = false;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (i > 0 && w[i - 1] == esc && (i < 2 || w[i - 2] != esc)) { r += w[i]; continue; }
        if (regex && w[i] == L'[') in_class = true;
        else if (regex && w[i] == L']') in_class = false;
        const auto c = static_cast<char32_t>(w[i]);
        const wchar_t* x = in_class ? nullptr : fold_expansion(c);
        if (x && regex) { r += L"(?:"; r += x; r += L")"; }
        else if (x) r += x;
        else r += static_cast<wchar_t>(fold_char(c));
    }
    return r;
}
}  // namespace

namespace {
// s056: patterns run on characters, not bytes -- the text as wide
// characters (32-bit on Linux), each one's byte offset kept to map back.
std::wstring widen(const std::string& s, std::vector<int>* at = nullptr) {
    std::wstring w;
    if (at) at->clear();
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + static_cast<std::size_t>(len) > s.size()) len = 1;
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        if (at) at->push_back(static_cast<int>(i));
        w.push_back(static_cast<wchar_t>(cp));
        i += static_cast<std::size_t>(len);
    }
    if (at) at->push_back(static_cast<int>(s.size()));
    return w;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t wc : w) {
        const auto c = static_cast<char32_t>(wc);
        if (c < 0x80) s += static_cast<char>(c);
        else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return s;
}

// Wildcards as a regular expression; the rest escaped.
std::wstring wild_to_regex(const std::wstring& w) {
    std::wstring r;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (c == L'^' && i + 1 < w.size()) {
            const wchar_t k = w[++i];
            if (k == L'9') r += L"[0-9]";
            else if (k == L'$') r += L"[A-Za-zÀ-ÖØ-öø-ɏͰ-ϿЀ-ӿ]";
            else if (k == L'w') r += L"[ \t  -​ ]";
            else if (k == L'?') r += L".";
            else if (k == L'p') r += L"\n";   // s095: a paragraph break
            else if (k == L'^') r += L"\\^";
            else { r += L"\\^"; --i; }
            continue;
        }
        if (std::wstring_view(L"\\.^$|()[]{}*+?").find(c) != std::wstring_view::npos) r += L'\\';
        r += c;
    }
    return r;
}

bool make_regex(const FindOpts& f, std::wregex& re, std::string* err) {
    try {
        const auto flags = std::regex::ECMAScript;   // s091: case by folding both sides, not icase (ASCII only)
        auto w = widen(f.what);
        if (!f.match_case) w = folded_pattern(w, f.mode == FindOpts::Mode::Regex ? L'\\' : L'^', f.mode == FindOpts::Mode::Regex);
        re = std::wregex(f.mode == FindOpts::Mode::Regex ? w : wild_to_regex(w), flags);
        return true;
    } catch (const std::regex_error& e) {
        if (err) *err = e.code() == std::regex_constants::error_paren     ? "unbalanced ( )"
                      : e.code() == std::regex_constants::error_brack     ? "unbalanced [ ]"
                      : e.code() == std::regex_constants::error_brace     ? "unbalanced { }"
                      : e.code() == std::regex_constants::error_badrepeat ? "* + ? with nothing before it"
                      : e.code() == std::regex_constants::error_escape    ? "a bad \\ escape"
                                                                           : "not a pattern that reads";
        return false;
    }
}
}  // namespace

bool find_pattern_ok(const FindOpts& f, std::string* err) {
    if (f.mode == FindOpts::Mode::Text || f.what.empty()) return true;
    std::wregex re;
    return make_regex(f, re, err);
}

std::string expand_groups(const std::string& with, const std::vector<std::string>& g) {
    std::string out;
    for (std::size_t i = 0; i < with.size(); ++i) {
        if (with[i] == '$' && i + 1 < with.size()) {
            const char k = with[i + 1];
            if (k == '$') { out += '$'; ++i; continue; }
            if (k == '&' && !g.empty()) { out += g[0]; ++i; continue; }
            if (k >= '1' && k <= '9' && static_cast<std::size_t>(k - '0') < g.size()) { out += g[static_cast<std::size_t>(k - '0')]; ++i; continue; }
        }
        out += with[i];
    }
    return out;
}

namespace {
// s113: the paragraph style a story's notes are set in, for "Any style /
// this style" -- a paragraph style named Footnote (as the flow finds it).
Doc::Id note_style_id(const Doc& d) {
    for (const auto& id : d.ids_of<Style>()) {
        const auto* s = d.as<Style>(id);
        if (s->kind != StyleKind::Paragraph) continue;
        std::string n = s->name;
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (n == "footnote" || n == "footnotes") return id;
    }
    return {};
}
}  // namespace

namespace {
// s095: does the search hold a paragraph break? "^p" (Text, Wildcards) or
// "\n" (Regex). Not with character-level filters (a character style, bold
// / italic), which belong to one paragraph.
bool spans_breaks(const FindOpts& f) {
    if (!f.cstyle.empty() || f.bold || f.italic || f.what.empty()) return false;
    return f.mode == FindOpts::Mode::Regex ? f.what.find("\\n") != std::string::npos : f.what.find("^p") != std::string::npos;
}

// s095: each story as one text, its paragraphs joined by '\n'; every
// character's place (paragraph, byte) kept, and one past the end.
std::vector<Hit> find_across(const Doc& d, const FindOpts& f, const std::vector<Doc::Id>& order) {
    std::vector<Hit> out;
    const Doc::Id nstyle = note_style_id(d);   // s116
    std::wregex re;
    const bool pattern = f.mode != FindOpts::Mode::Text;
    if (pattern && !make_regex(f, re, nullptr)) return out;
    std::wstring what;
    if (!pattern) {
        std::string w = f.what;
        for (std::size_t at = w.find("^p"); at != std::string::npos; at = w.find("^p", at + 1)) w.replace(at, 2, "\n");
        what = f.match_case ? widen(w) : folded(widen(w));
    }
    TextPos lo = f.from, hi = f.to;
    if (hi < lo) std::swap(lo, hi);
    for (const auto& sid : order) {
        if (!f.story.empty() && sid != f.story) continue;
        const auto& st = *d.as<Story>(sid);
        std::wstring all;
        std::vector<TextPos> place;
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            std::vector<int> at;
            const auto w = widen(st.paras[static_cast<std::size_t>(p)].text, &at);
            for (std::size_t k = 0; k < w.size(); ++k) { all.push_back(w[k]); place.push_back({p, at[k]}); }
            if (p + 1 < static_cast<int>(st.paras.size())) { all.push_back(L'\n'); place.push_back({p, at.back()}); }
            else place.push_back({p, at.back()});   // one past the end
        }
        struct M { std::size_t a, b; std::vector<std::string> g; };
        const auto matches = [&](const std::wstring& all) {
        const Folded fm = fold_map(all, !f.match_case);   // s152: fully folded, mapped back
        const auto& look = fm.w;
        std::vector<M> ms;
        if (pattern) {
            for (auto it = std::wsregex_iterator(look.begin(), look.end(), re); it != std::wsregex_iterator(); ++it) {
                const auto& m = *it;
                if (m.length(0) == 0) continue;
                std::size_t a = 0, b = 0;
                if (!fm.back(static_cast<std::size_t>(m.position(0)), static_cast<std::size_t>(m.position(0) + m.length(0)), a, b)) continue;
                M x{a, b, {}};
                for (std::size_t k = 0; k < m.size(); ++k)
                    x.g.push_back(m[k].matched ? narrow(all.substr(fm.src[static_cast<std::size_t>(m.position(k))],
                                                                   fm.src[static_cast<std::size_t>(m.position(k) + m.length(k))] -
                                                                   fm.src[static_cast<std::size_t>(m.position(k))]))
                                               : std::string{});
                ms.push_back(std::move(x));
            }
        } else if (!what.empty()) {
            for (std::size_t at = look.find(what); at != std::wstring::npos; at = look.find(what, at + 1)) {
                std::size_t a = 0, b = 0;
                if (!fm.back(at, at + what.size(), a, b)) continue;
                ms.push_back({a, b, {}});
                at += what.size() - 1;
            }
        }
        const auto wordc = [](wchar_t c) { return c != L'\n' && word_byte(c < 0x80 ? static_cast<unsigned char>(c) : 0x80); };
        std::erase_if(ms, [&](const M& m) {
            return f.whole_word && ((m.a > 0 && wordc(all[m.a - 1])) || (m.b < all.size() && wordc(all[m.b])));
        });
        return ms;
        };
        std::vector<std::pair<TextPos, Hit>> keyed;   // s116: the text's and the notes' hits, read at their place
        for (auto& m : matches(all)) {
            const TextPos a = place[m.a], b = place[m.b];
            if (!f.pstyle.empty() && st.paras[static_cast<std::size_t>(a.para)].style != f.pstyle) continue;
            if (f.in_range && (a < lo || hi < b)) continue;
            keyed.emplace_back(a, Hit{sid, a, b, std::move(m.g)});
        }
        // s116: a note's paragraph breaks -- each note's words searched as one
        // text (notes when the paragraph style asked is none, or Footnote).
        if (f.pstyle.empty() || f.pstyle == nstyle)
            for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                const auto& para = st.paras[static_cast<std::size_t>(p)];
                for (int k = 0; k < static_cast<int>(para.notes.size()); ++k) {
                    const int mark = note_mark_byte(st, p, k);
                    if (mark < 0) continue;
                    if (f.in_range && (TextPos{p, mark} < lo || hi < TextPos{p, mark + 3})) continue;
                    std::vector<int> at;
                    const auto w = widen(para.notes[static_cast<std::size_t>(k)].text, &at);
                    for (auto& m : matches(w)) {
                        Hit h{sid, {p, at[m.a]}, {p, at[m.b]}, std::move(m.g)};
                        h.note = k;
                        keyed.emplace_back(TextPos{p, mark + 1}, std::move(h));
                    }
                }
            }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        for (auto& [key, h] : keyed) out.push_back(std::move(h));
    }
    return out;
}
}  // namespace


std::vector<Hit> find_all(const Doc& d, const FindOpts& f) {
    if (spans_breaks(f)) return find_across(d, f, reading_order(d));   // s095
    std::vector<Hit> out;
    const bool fmt = f.bold || f.italic;
    if (f.what.empty() && f.pstyle.empty() && f.cstyle.empty() && !fmt) return out;
    const bool pattern = f.mode != FindOpts::Mode::Text && !f.what.empty();
    std::wregex re;
    if (pattern && !make_regex(f, re, nullptr)) return out;
    const std::wstring what = f.match_case ? widen(f.what) : folded(widen(f.what));   // s091: by characters
    TextPos lo = f.from, hi = f.to;
    if (hi < lo) std::swap(lo, hi);
    // s113: notes are looked in with no paragraph style asked for, or the
    // one they're set in (Footnote).
    const Doc::Id nstyle = note_style_id(d);
    const bool in_notes = f.pstyle.empty() || (!nstyle.empty() && f.pstyle == nstyle);

    // The hits in one paragraph's text (or a note's words, `note` >= 0),
    // in [lo_b, hi_b), paragraph p.
    const auto search = [&](const Doc::Id& sid, const Para& para, int p, int note, int lo_b, int hi_b, std::vector<Hit>& into) {
        const int n = static_cast<int>(para.text.size());
        const auto inside = [&](int a, int b) { return a >= lo_b && b <= hi_b; };
        const auto hit = [&](int a, int b, std::vector<std::string> g = {}) {
            Hit h{sid, {p, a}, {p, b}, std::move(g)};
            h.note = note;
            into.push_back(std::move(h));
        };
        std::vector<Stretch> fs;
        if (fmt) {
            fs = format_stretches(d, para, f);
            if (fs.empty()) return;
        }
        if (what.empty() && fmt) {   // each stretch so set
            for (const auto& x : fs) {
                const int a = std::max(x.a, lo_b), b = std::min(x.b, hi_b);
                if (b > a) hit(a, b);
            }
            return;
        }
        if (what.empty()) {
            if (!f.cstyle.empty()) {   // each run of the character style
                for (const auto& r : para.runs) {
                    const int a = std::max(r.start, lo_b), b = std::min(r.end, hi_b);
                    if (r.style == f.cstyle && b > a) hit(a, b);
                }
            } else if (hi_b > lo_b) {  // the whole paragraph (its part in the range)
                hit(lo_b, hi_b);
            }
            return;
        }
        // The matches: [a, b) and their groups.
        struct M { int a, b; std::vector<std::string> g; };
        std::vector<M> ms;
        const std::string& hay = para.text;
        if (pattern) {
            std::vector<int> at;
            const auto wide = widen(para.text, &at);
            const Folded fm = fold_map(wide, !f.match_case);   // s152: fully folded, mapped back
            const auto& look = fm.w;
            for (auto it = std::wsregex_iterator(look.begin(), look.end(), re); it != std::wsregex_iterator(); ++it) {
                const auto& m = *it;
                if (m.length(0) == 0) continue;   // an empty match finds nothing
                std::size_t p0 = 0, p1 = 0;
                if (!fm.back(static_cast<std::size_t>(m.position(0)), static_cast<std::size_t>(m.position(0) + m.length(0)), p0, p1)) continue;
                M x{at[p0], at[p1], {}};
                for (std::size_t k = 0; k < m.size(); ++k) {   // the groups as written, not folded
                    const std::size_t g0 = fm.src[static_cast<std::size_t>(m.position(k))];
                    const std::size_t g1 = fm.src[static_cast<std::size_t>(m.position(k) + m.length(k))];
                    x.g.push_back(m[k].matched ? narrow(wide.substr(g0, g1 - g0)) : std::string{});
                }
                ms.push_back(std::move(x));
            }
        } else {
            // s091: by characters, folded both sides; the byte offsets mapped back.
            std::vector<int> bat;
            const Folded fm = fold_map(widen(hay, &bat), !f.match_case);   // s152: fully
            const auto& wh = fm.w;
            const std::size_t w = what.size();
            for (std::size_t at = wh.find(what); at != std::wstring::npos; at = wh.find(what, at + 1)) {
                std::size_t c0 = 0, c1 = 0;
                if (!fm.back(at, at + w, c0, c1)) continue;
                const int a = bat[c0], b = bat[c1];
                if (f.whole_word && ((a > 0 && word_byte(static_cast<unsigned char>(hay[static_cast<std::size_t>(a - 1)]))) ||
                                     (b < n && word_byte(static_cast<unsigned char>(hay[static_cast<std::size_t>(b)])))))
                    continue;
                ms.push_back({a, b, {}});
                at += w - 1;   // no overlapping hits
            }
        }
        for (auto& m : ms) {
            const int a = m.a, b = m.b;
            if (pattern && f.whole_word && ((a > 0 && word_byte(static_cast<unsigned char>(para.text[a - 1]))) ||
                                            (b < n && word_byte(static_cast<unsigned char>(para.text[b])))))
                continue;
            if (!inside(a, b)) continue;
            if (!f.cstyle.empty()) {   // wholly inside a run of it
                bool in = false;
                for (const auto& r : para.runs) in = in || (r.style == f.cstyle && r.start <= a && r.end >= b);
                if (!in) continue;
            }
            if (fmt && std::none_of(fs.begin(), fs.end(), [&](const Stretch& x) { return x.a <= a && x.b >= b; }))
                continue;
            hit(a, b, std::move(m.g));
        }
    };

    for (const auto& sid : reading_order(d)) {
        if (!f.story.empty() && sid != f.story) continue;   // s056
        const auto& st = *d.as<Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            const int n = static_cast<int>(para.text.size());
            // s056: the stretch searched -- the range's part of this paragraph.
            int lo_b = 0, hi_b = n;
            if (f.in_range) {
                if (p < lo.para || p > hi.para) continue;
                if (p == lo.para) lo_b = std::clamp(lo.byte, 0, n);
                if (p == hi.para) hi_b = std::clamp(hi.byte, 0, n);
                if (hi_b <= lo_b) continue;
            }
            std::vector<Hit> here;
            if (f.pstyle.empty() || para.style == f.pstyle) search(sid, para, p, -1, lo_b, hi_b, here);
            // s113: its notes' words -- each read at its mark (a note in the
            // range when its mark is).
            if (in_notes && !para.notes.empty()) {
                std::vector<std::pair<int, Hit>> keyed;   // (the mark's byte, the hit)
                for (const auto& h : here) keyed.emplace_back(h.a.byte, h);
                int k = 0;
                for (std::size_t i = para.text.find(kMarkNote); i != std::string::npos && k < static_cast<int>(para.notes.size());
                     i = para.text.find(kMarkNote, i + 3), ++k) {
                    const int m = static_cast<int>(i);
                    if (m < lo_b || m + 3 > hi_b) continue;
                    const auto& note = para.notes[static_cast<std::size_t>(k)];
                    Para np{para.style, note.text, note.runs};
                    std::vector<Hit> in;
                    search(sid, np, p, k, 0, static_cast<int>(note.text.size()), in);
                    for (auto& h : in) keyed.emplace_back(m + 1, std::move(h));   // after a hit beginning at the mark
                }
                std::stable_sort(keyed.begin(), keyed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                here.clear();
                for (auto& [key, h] : keyed) here.push_back(std::move(h));
            }
            for (auto& h : here) out.push_back(std::move(h));
            // s335: a table's cells, row by row (a merge once) -- read where its
            // paragraph is; with a paragraph style asked for, only cell
            // paragraphs set in it.
            for (const auto& t : para.tables)
                for (int r = 0; r < static_cast<int>(t.rows.size()); ++r)
                    for (int c = 0; c < static_cast<int>(t.rows[static_cast<std::size_t>(r)].cells.size()); ++c) {
                        if (owner_of(t, r, c) != std::pair<int, int>{r, c}) continue;
                        const auto& cell = t.rows[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)];
                        for (int q = 0; q < static_cast<int>(cell.paras.size()); ++q) {
                            const auto& cp = cell.paras[static_cast<std::size_t>(q)];
                            if (!f.pstyle.empty() && cp.style != f.pstyle) continue;
                            std::vector<Hit> in;
                            search(sid, cp, q, -1, 0, static_cast<int>(cp.text.size()), in);
                            for (auto& h : in) {
                                h.table = p;
                                h.row = r;
                                h.col = c;
                                out.push_back(std::move(h));
                            }
                        }
                    }
        }
    }
    return out;
}

int swatch_uses(const Doc& d, const Doc::Id& sw) {
    if (sw.empty()) return 0;
    int n = 0;
    for (const auto& id : d.ids_of<Frame>()) {
        const auto& f = *d.as<Frame>(id);
        n += (f.fill == sw) + (f.stroke == sw);
    }
    for (const auto& id : d.ids_of<Style>()) {
        const auto& s = *d.as<Style>(id);
        n += (s.color == sw) + (s.rule_above.color == sw) + (s.rule_below.color == sw);
    }
    for (const auto& id : d.ids_of<Frame>()) n += d.as<Frame>(id)->col_rule.color == sw;   // s325: a column rule's
    for (const auto& id : d.ids_of<TableStyle>()) {   // s342: table and cell styles' colours
        const auto& t = *d.as<TableStyle>(id);
        n += (t.rule_color == sw) + (t.header_fill == sw) + (t.band_fill == sw);
    }
    for (const auto& id : d.ids_of<CellStyle>()) {
        const auto& c = *d.as<CellStyle>(id);
        n += c.fill && *c.fill == sw;
        for (const auto& b : c.borders) n += b.w >= 0 && b.color == sw;
    }
    const auto in_runs = [&](const std::vector<Run>& runs) { for (const auto& r : runs) n += r.local.color == sw; };
    for (const auto& id : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(id)->paras) {
            in_runs(p.runs);
            for (const auto& note : p.notes) in_runs(note.runs);   // s325: a note's words
            for (const auto& t : p.tables) {   // s325: a table's fills, grid, words
                n += (t.header_fill == sw) + (t.band_fill == sw) + (t.rule_color == sw);
                for (const auto& row : t.rows)
                    for (const auto& c : row.cells) {
                        n += c.fill == sw;
                        for (const auto& b : c.borders) n += b.w >= 0 && b.color == sw;   // s334: its own border
                        for (const auto& cp : c.paras) in_runs(cp.runs);
                    }
            }
        }
    return n;
}

std::string change_text(const ChangeOpts& c, const Hit& h) {   // s095
    std::string w = c.expand ? expand_groups(c.with, h.groups) : c.with;
    for (std::size_t at = w.find("^p"); at != std::string::npos; at = w.find("^p", at + 1)) w.replace(at, 2, "\n");
    return w;
}

int change_hits(DocJournal& j, Doc& d, std::vector<Hit> hits, const ChangeOpts& c, const std::string& label) {
    std::erase_if(hits, [&](const Hit& h) { return !d.as<Story>(h.story); });
    if (hits.empty()) return 0;
    if (!c.set_text && c.pstyle.empty() && c.cstyle.empty()) return 0;
    // Last to first within each story: earlier places stay put. s113: the
    // notes' hits first (a change in the text can't then move a note out
    // from under its hit), each note's last to first.
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        if (x.story != y.story) return x.story < y.story;
        // s335: the cells' hits first of all (nothing in the text then moves a
        // table out from under them), each cell's last to first
        if ((x.table >= 0) != (y.table >= 0)) return x.table >= 0;
        if (x.table >= 0) {
            if (x.table != y.table || x.row != y.row || x.col != y.col)
                return std::tie(x.table, x.row, x.col) < std::tie(y.table, y.row, y.col);
            return y.a < x.a;
        }
        if ((x.note >= 0) != (y.note >= 0)) return x.note >= 0;
        if (x.note >= 0 && (x.a.para != y.a.para || x.note != y.note))
            return x.a.para != y.a.para ? y.a.para < x.a.para : y.note < x.note;
        return y.a < x.a;
    });
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    std::vector<Doc::Id> stories;
    for (const auto& h : hits)
        if (std::find(stories.begin(), stories.end(), h.story) == stories.end()) stories.push_back(h.story);
    int n = 0;
    // An unchanged run leaves no step (the journal drops it): 0 then.
    const bool did = j.run(label, stories, [&] {
        for (const auto& h : hits) {
            if (h.table >= 0) {   // s335: in a cell's words -- the text; s336: a character style too (a paragraph style: not in cells)
                if (!c.set_text && c.cstyle.empty()) continue;
                TextPos a = h.a, b = h.b;
                bool any = false;
                if (c.set_text) {
                    b = edit_cell(j, d, h.story, h.table, h.row, h.col, a, b, change_text(c, h), label);
                    if (b.para < 0) continue;
                    any = true;
                }
                if (!c.cstyle.empty() && a != b) {
                    set_cell_char_style(j, d, h.story, h.table, h.row, h.col, a, b, c.cstyle);   // inside this step: just writes (no "changed" back)
                    any = true;
                    if (const auto* sty = d.as<Style>(c.cstyle); sty && c.fold_local)   // s051, as in the text
                        set_cell_local(j, d, h.story, h.table, h.row, h.col, a, b, label, [&](Local& l) {
                            if (l.bold >= 0 && (l.bold == 1) == sty->bold) l.bold = -1;
                            if (l.italic >= 0 && (l.italic == 1) == sty->italic) l.italic = -1;
                        });
                }
                if (any) ++n;
                continue;
            }
            if (h.note >= 0) {   // s113: in a note's words -- the text and / or a character style
                if (!c.set_text && c.cstyle.empty()) continue;
                const auto* sty = c.cstyle.empty() ? nullptr : d.as<Style>(c.cstyle);
                const std::function<void(Local&)> fold = [&](Local& l) {
                    if (!sty || !c.fold_local) return;
                    if (l.bold >= 0 && (l.bold == 1) == sty->bold) l.bold = -1;
                    if (l.italic >= 0 && (l.italic == 1) == sty->italic) l.italic = -1;
                };
                if (change_note_words(j, d, h.story, h.a.para, h.note, h.a.byte, h.b.byte, c.set_text, change_text(c, h), c.cstyle,
                                      label, fold) >= 0)
                    ++n;
                continue;
            }
            TextPos a = h.a, b = h.b;
            if (c.set_text) b = edit_text(j, d, h.story, a, b, change_text(c, h));   // inside this step: just writes
            if (!c.pstyle.empty()) set_para_style(j, d, h.story, a, b, c.pstyle);
            if (!c.cstyle.empty() && a != b) {
                set_char_style(j, d, h.story, a, b, c.cstyle);
                if (const auto* sty = d.as<Style>(c.cstyle); sty && c.fold_local)   // s051
                    set_local(j, d, h.story, a, b, label, [&](Local& l) {
                        if (l.bold >= 0 && (l.bold == 1) == sty->bold) l.bold = -1;
                        if (l.italic >= 0 && (l.italic == 1) == sty->italic) l.italic = -1;
                    });
            }
            ++n;
        }
    });
    if (!did) return 0;
    return n;
}

// s152: a string's full case fold (fold_char, a sharp s "ss", the ligatures
// as their letters) -- for Find and the index's sorting.
std::string fold_text(const std::string& utf8) { return narrow(folded(widen(utf8))); }

std::vector<Doc::Id> stories_in_order(const Doc& d) { return reading_order(d); }   // s310

namespace {
// s322: a story's misspelled words in reading order -- its paragraphs' and,
// at a table's paragraph, its cells' (row by row, each merge once).
using SpellKey = std::array<int, 5>;   // para, row + 1 (0: the paragraph's own words), col, cell paragraph, byte
SpellKey key_of(const SpellHit& h) { return {h.para, h.row + 1, std::max(0, h.col), h.cpara, h.from}; }
std::vector<SpellHit> story_misses(const Doc& d, const Doc::Id& sid) {
    std::vector<SpellHit> out;
    const auto& st = *d.as<Story>(sid);
    for (std::size_t p = 0; p < st.paras.size(); ++p) {
        const auto& para = st.paras[p];
        for (const auto& w : spell::para_misses(para.text, static_cast<int>(p))) out.push_back({sid, w.para, w.from, w.to, w.text});
        for (const auto& t : para.tables)
            for (int r = 0; r < static_cast<int>(t.rows.size()); ++r)
                for (int c = 0; c < static_cast<int>(t.rows[static_cast<std::size_t>(r)].cells.size()); ++c) {
                    if (owner_of(t, r, c) != std::pair<int, int>{r, c}) continue;
                    const auto& cell = t.rows[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)];
                    for (int q = 0; q < static_cast<int>(cell.paras.size()); ++q)
                        for (const auto& w : spell::para_misses(cell.paras[static_cast<std::size_t>(q)].text, static_cast<int>(p))) {
                            SpellHit h{sid, static_cast<int>(p), w.from, w.to, w.text};
                            h.row = r;
                            h.col = c;
                            h.cpara = q;
                            out.push_back(h);
                        }
                }
    }
    return out;
}
}  // namespace

SpellHit next_misspelling(const Doc& d, const Doc::Id& story, TextPos from, const SpellHit* after) {   // s310; s322: in tables too
    if (!spell::loaded()) return {};
    const auto order = reading_order(d);
    if (order.empty()) return {};
    const auto at = std::find(order.begin(), order.end(), story);
    const std::size_t k0 = at == order.end() ? 0 : static_cast<std::size_t>(at - order.begin());
    // where it starts: past `after` (that word), else the caret (the word it's in skipped)
    SpellKey start{from.para, 0, 0, 0, from.byte};
    bool strict = false;
    if (after && !after->story.empty() && after->story == story) { start = key_of(*after); strict = true; }
    for (std::size_t n = 0; n <= order.size(); ++n) {
        const auto& sid = order[(k0 + n) % order.size()];
        const bool first = n == 0 && at != order.end(), last = n == order.size();
        if (last && at == order.end()) break;
        for (const auto& h : story_misses(d, sid)) {
            const auto k = key_of(h);
            if (first) {
                if (strict ? !(start < k) : k < start) continue;
                if (!strict && h.row < 0 && h.para == from.para && h.from < from.byte) continue;   // the word the caret is in
            }
            if (last && !(k < start)) return {};   // round to where it began
            return h;
        }
    }
    return {};
}

SpellCount misspellings(const Doc& d) {   // s310; s322: tables' cells too
    SpellCount c;
    if (!spell::loaded()) return c;
    for (const auto& sid : d.ids_of<Story>()) {
        const auto m = story_misses(d, sid);
        c.words += static_cast<int>(m.size());
        c.stories += m.empty() ? 0 : 1;
    }
    return c;
}

bool replace_misspelling(DocJournal& j, Doc& d, const SpellHit& h, const std::string& with) {   // s322
    if (h.story.empty() || with.empty()) return false;
    if (h.row < 0) return replace_word(j, d, h.story, h.para, h.from, h.to, h.word, with);
    const auto* t = table_at(d, h.story, h.para);
    if (!t || h.row >= static_cast<int>(t->rows.size()) || h.col >= static_cast<int>(t->rows[static_cast<std::size_t>(h.row)].cells.size())) return false;
    const auto& cell = t->rows[static_cast<std::size_t>(h.row)].cells[static_cast<std::size_t>(h.col)];
    if (h.cpara < 0 || h.cpara >= static_cast<int>(cell.paras.size())) return false;
    const auto& txt = cell.paras[static_cast<std::size_t>(h.cpara)].text;
    if (h.to > static_cast<int>(txt.size()) || txt.compare(static_cast<std::size_t>(h.from), static_cast<std::size_t>(h.to - h.from), h.word) != 0) return false;
    return edit_table(j, d, h.story, h.para, "Spelling", [&](Table& x) {
        auto& q = x.rows[static_cast<std::size_t>(h.row)].cells[static_cast<std::size_t>(h.col)].paras[static_cast<std::size_t>(h.cpara)];
        q.text.replace(static_cast<std::size_t>(h.from), static_cast<std::size_t>(h.to - h.from), with);
        // its runs: those after the word moved by the difference (the word's own run takes it)
        const int delta = static_cast<int>(with.size()) - (h.to - h.from);
        for (auto& r : q.runs) {
            if (r.start >= h.to) r.start += delta;
            if (r.end >= h.to) r.end += delta;
        }
    });
}

}  // namespace composix::core::doc
