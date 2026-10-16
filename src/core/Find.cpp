// core::doc::find_all / change_hits -- Find / Change (s022).
#include "core/Doc.hpp"

#include <algorithm>
#include <map>

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

std::vector<Hit> find_all(const Doc& d, const FindOpts& f) {
    std::vector<Hit> out;
    const bool fmt = f.bold || f.italic;
    if (f.what.empty() && f.pstyle.empty() && f.cstyle.empty() && !fmt) return out;
    std::string what = f.what;
    if (!f.match_case) for (auto& c : what) c = fold(c);
    for (const auto& sid : reading_order(d)) {
        const auto& st = *d.as<Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            if (!f.pstyle.empty() && para.style != f.pstyle) continue;
            const int n = static_cast<int>(para.text.size());
            std::vector<Stretch> fs;
            if (fmt) {
                fs = format_stretches(d, para, f);
                if (fs.empty()) continue;
            }
            if (what.empty() && fmt) {   // each stretch so set
                for (const auto& x : fs) out.push_back({sid, {p, x.a}, {p, x.b}});
                continue;
            }
            if (what.empty()) {
                if (!f.cstyle.empty()) {   // each run of the character style
                    for (const auto& r : para.runs)
                        if (r.style == f.cstyle) out.push_back({sid, {p, r.start}, {p, r.end}});
                } else if (n > 0) {        // the whole paragraph
                    out.push_back({sid, {p, 0}, {p, n}});
                }
                continue;
            }
            std::string hay = para.text;
            if (!f.match_case) for (auto& c : hay) c = fold(c);
            const int w = static_cast<int>(what.size());
            for (std::size_t at = hay.find(what); at != std::string::npos; at = hay.find(what, at + 1)) {
                const int a = static_cast<int>(at), b = a + w;
                if (f.whole_word && ((a > 0 && word_byte(static_cast<unsigned char>(hay[a - 1]))) ||
                                     (b < n && word_byte(static_cast<unsigned char>(hay[b])))))
                    continue;
                if (!f.cstyle.empty()) {   // wholly inside a run of it
                    bool inside = false;
                    for (const auto& r : para.runs) inside = inside || (r.style == f.cstyle && r.start <= a && r.end >= b);
                    if (!inside) continue;
                }
                if (fmt && std::none_of(fs.begin(), fs.end(), [&](const Stretch& x) { return x.a <= a && x.b >= b; }))
                    continue;
                out.push_back({sid, {p, a}, {p, b}});
                at += static_cast<std::size_t>(w) - 1;   // no overlapping hits
            }
        }
    }
    return out;
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
            if (c.set_text) b = edit_text(j, d, h.story, a, b, c.with);   // inside this step: just writes
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
