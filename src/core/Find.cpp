// core::doc::find_all / change_hits -- Find / Change (s022).
#include "core/Doc.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <string_view>

namespace composix::core::doc {

namespace {

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

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
        auto flags = std::regex::ECMAScript;
        if (!f.match_case) flags |= std::regex::icase;
        const auto w = widen(f.what);
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

std::vector<Hit> find_all(const Doc& d, const FindOpts& f) {
    std::vector<Hit> out;
    const bool fmt = f.bold || f.italic;
    if (f.what.empty() && f.pstyle.empty() && f.cstyle.empty() && !fmt) return out;
    const bool pattern = f.mode != FindOpts::Mode::Text && !f.what.empty();
    std::wregex re;
    if (pattern && !make_regex(f, re, nullptr)) return out;
    std::string what = f.what;
    if (!f.match_case) for (auto& c : what) c = fold(c);
    TextPos lo = f.from, hi = f.to;
    if (hi < lo) std::swap(lo, hi);
    for (const auto& sid : reading_order(d)) {
        if (!f.story.empty() && sid != f.story) continue;   // s056
        const auto& st = *d.as<Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            if (!f.pstyle.empty() && para.style != f.pstyle) continue;
            const int n = static_cast<int>(para.text.size());
            // s056: the stretch searched -- the range's part of this paragraph.
            int lo_b = 0, hi_b = n;
            if (f.in_range) {
                if (p < lo.para || p > hi.para) continue;
                if (p == lo.para) lo_b = std::clamp(lo.byte, 0, n);
                if (p == hi.para) hi_b = std::clamp(hi.byte, 0, n);
                if (hi_b <= lo_b) continue;
            }
            const auto inside = [&](int a, int b) { return a >= lo_b && b <= hi_b; };
            std::vector<Stretch> fs;
            if (fmt) {
                fs = format_stretches(d, para, f);
                if (fs.empty()) continue;
            }
            if (what.empty() && fmt) {   // each stretch so set
                for (const auto& x : fs) {
                    const int a = std::max(x.a, lo_b), b = std::min(x.b, hi_b);
                    if (b > a) out.push_back({sid, {p, a}, {p, b}});
                }
                continue;
            }
            if (what.empty()) {
                if (!f.cstyle.empty()) {   // each run of the character style
                    for (const auto& r : para.runs) {
                        const int a = std::max(r.start, lo_b), b = std::min(r.end, hi_b);
                        if (r.style == f.cstyle && b > a) out.push_back({sid, {p, a}, {p, b}});
                    }
                } else if (hi_b > lo_b) {  // the whole paragraph (its part in the range)
                    out.push_back({sid, {p, lo_b}, {p, hi_b}});
                }
                continue;
            }
            // The matches in this paragraph: [a, b) and their groups.
            struct M { int a, b; std::vector<std::string> g; };
            std::vector<M> ms;
            std::string hay = para.text;
            if (pattern) {
                std::vector<int> at;
                const auto wide = widen(para.text, &at);
                for (auto it = std::wsregex_iterator(wide.begin(), wide.end(), re); it != std::wsregex_iterator(); ++it) {
                    const auto& m = *it;
                    if (m.length(0) == 0) continue;   // an empty match finds nothing
                    const auto p0 = static_cast<std::size_t>(m.position(0)), p1 = p0 + static_cast<std::size_t>(m.length(0));
                    M x{at[p0], at[p1], {}};
                    for (std::size_t k = 0; k < m.size(); ++k) x.g.push_back(m[k].matched ? narrow(m[k].str()) : std::string{});
                    ms.push_back(std::move(x));
                }
            } else {
                if (!f.match_case) for (auto& c : hay) c = fold(c);
                const int w = static_cast<int>(what.size());
                for (std::size_t at = hay.find(what); at != std::string::npos; at = hay.find(what, at + 1)) {
                    const int a = static_cast<int>(at), b = a + w;
                    if (f.whole_word && ((a > 0 && word_byte(static_cast<unsigned char>(hay[a - 1]))) ||
                                         (b < n && word_byte(static_cast<unsigned char>(hay[b])))))
                        continue;
                    ms.push_back({a, b, {}});
                    at += static_cast<std::size_t>(w) - 1;   // no overlapping hits
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
                out.push_back({sid, {p, a}, {p, b}, std::move(m.g)});
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
    for (const auto& id : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(id)->paras)
            for (const auto& r : p.runs) n += r.local.color == sw;
    return n;
}

int change_hits(DocJournal& j, Doc& d, std::vector<Hit> hits, const ChangeOpts& c, const std::string& label) {
    std::erase_if(hits, [&](const Hit& h) { return !d.as<Story>(h.story); });
    if (hits.empty()) return 0;
    if (!c.set_text && c.pstyle.empty() && c.cstyle.empty()) return 0;
    // Last to first within each story: earlier places stay put.
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        return x.story != y.story ? x.story < y.story : y.a < x.a;
    });
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    std::vector<Doc::Id> stories;
    for (const auto& h : hits)
        if (std::find(stories.begin(), stories.end(), h.story) == stories.end()) stories.push_back(h.story);
    int n = 0;
    // An unchanged run leaves no step (the journal drops it): 0 then.
    const bool did = j.run(label, stories, [&] {
        for (const auto& h : hits) {
            TextPos a = h.a, b = h.b;
            if (c.set_text) b = edit_text(j, d, h.story, a, b, c.expand ? expand_groups(c.with, h.groups) : c.with);   // inside this step: just writes
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
