// core::doc::find_all / change_hits -- Find / Change (s022).
#include "core/Doc.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string_view>

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
// s091: a wide string folded, char for char (indices unchanged).
std::wstring folded(std::wstring w) {
    for (auto& c : w) c = static_cast<wchar_t>(fold_char(static_cast<char32_t>(c)));
    return w;
}
// ... a pattern folded, leaving what follows an escape (\D, \W, \S mean
// something else in upper case) -- `esc`: the escape character.
std::wstring folded_pattern(const std::wstring& w, wchar_t esc) {
    std::wstring r;
    for (std::size_t i = 0; i < w.size(); ++i) {
        r += (i > 0 && w[i - 1] == esc && (i < 2 || w[i - 2] != esc)) ? w[i] : static_cast<wchar_t>(fold_char(static_cast<char32_t>(w[i])));
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
        if (!f.match_case) w = folded_pattern(w, f.mode == FindOpts::Mode::Regex ? L'\\' : L'^');
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
        const auto look = f.match_case ? all : folded(all);
        std::vector<M> ms;
        if (pattern) {
            for (auto it = std::wsregex_iterator(look.begin(), look.end(), re); it != std::wsregex_iterator(); ++it) {
                const auto& m = *it;
                if (m.length(0) == 0) continue;
                M x{static_cast<std::size_t>(m.position(0)), static_cast<std::size_t>(m.position(0) + m.length(0)), {}};
                for (std::size_t k = 0; k < m.size(); ++k)
                    x.g.push_back(m[k].matched ? narrow(all.substr(static_cast<std::size_t>(m.position(k)), static_cast<std::size_t>(m.length(k))))
                                               : std::string{});
                ms.push_back(std::move(x));
            }
        } else if (!what.empty()) {
            for (std::size_t at = look.find(what); at != std::wstring::npos; at = look.find(what, at + 1)) {
                ms.push_back({at, at + what.size(), {}});
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
            const auto look = f.match_case ? wide : folded(wide);   // s091: same indices
            for (auto it = std::wsregex_iterator(look.begin(), look.end(), re); it != std::wsregex_iterator(); ++it) {
                const auto& m = *it;
                if (m.length(0) == 0) continue;   // an empty match finds nothing
                const auto p0 = static_cast<std::size_t>(m.position(0)), p1 = p0 + static_cast<std::size_t>(m.length(0));
                M x{at[p0], at[p1], {}};
                for (std::size_t k = 0; k < m.size(); ++k)   // the groups as written, not folded
                    x.g.push_back(m[k].matched ? narrow(wide.substr(static_cast<std::size_t>(m.position(k)),
                                                                    static_cast<std::size_t>(m.length(k))))
                                               : std::string{});
                ms.push_back(std::move(x));
            }
        } else {
            // s091: by characters, folded both sides; the byte offsets mapped back.
            std::vector<int> bat;
            auto wh = widen(hay, &bat);
            if (!f.match_case) wh = folded(wh);
            const std::size_t w = what.size();
            for (std::size_t at = wh.find(what); at != std::wstring::npos; at = wh.find(what, at + 1)) {
                const int a = bat[at], b = bat[at + w];
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
    for (const auto& id : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(id)->paras)
            for (const auto& r : p.runs) n += r.local.color == sw;
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

}  // namespace composix::core::doc
