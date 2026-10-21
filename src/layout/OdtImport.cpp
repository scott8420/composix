// layout::import_odt -- see the header.
#include "layout/OdtImport.hpp"
#include "layout/Flow.hpp"
#include "layout/Export.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <filesystem>
#include <fstream>

namespace composix::layout {
namespace {
using core::Doc;
namespace odt = core::odt;

// A line break (\n) ends a paragraph: the pieces, each its own spans and notes.
std::vector<odt::Para> split_lines(const odt::Para& p) {
    std::vector<odt::Para> out;
    std::size_t a = 0;
    std::size_t note = 0, index = 0, cite = 0;
    for (;;) {
        const auto e = p.text.find('\n', a);
        const std::size_t b = e == std::string::npos ? p.text.size() : e;
        odt::Para q;
        q.style = p.style;
        q.outline = p.outline;
        q.list = p.list;   // s243: its list goes with each piece (the number on the first)
        q.list_level = p.list_level;
        q.start = out.empty() ? p.start : 0;
        q.text = p.text.substr(a, b - a);
        for (const auto& s : p.spans) {
            const int s0 = std::max(s.start, static_cast<int>(a)), s1 = std::min(s.end, static_cast<int>(b));
            if (s1 > s0) q.spans.push_back({s0 - static_cast<int>(a), s1 - static_cast<int>(a), s.style});
        }
        for (std::size_t i = q.text.find(core::kMarkNote); i != std::string::npos; i = q.text.find(core::kMarkNote, i + 3))
            if (note < p.notes.size()) q.notes.push_back(p.notes[note++]);
        for (std::size_t i = q.text.find(core::kMarkIndex); i != std::string::npos; i = q.text.find(core::kMarkIndex, i + 3))   // s286
            if (index < p.index.size()) q.index.push_back(p.index[index++]);
        for (std::size_t i = q.text.find(core::kMarkRef); i != std::string::npos; i = q.text.find(core::kMarkRef, i + 3))   // s291
            if (cite < p.cites.size()) q.cites.push_back(p.cites[cite++]);
        out.push_back(std::move(q));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return out;
}

struct Builder {
    Doc& d;
    const odt::Book& b;
    OdtInfo& info;
    std::map<std::string, std::string> para_style;   // ODT named paragraph style -> Composix id
    std::map<std::string, std::string> char_style;   // ODT named text style -> Composix id
    std::map<std::string, std::string> swatch;       // "#rrggbb" -> id
    std::map<std::string, std::string> list_style;   // s243: "base|list|level" -> a list paragraph style
    double pos = 10;
    std::string body_id;                              // the default paragraph style's
    std::map<std::string, std::string> cite_anchor;   // s291: a source's identifier -> its anchor

    std::string swatch_for(const std::string& hex) {
        if (hex.size() != 7 || hex == "#000000") return {};
        if (const auto it = swatch.find(hex); it != swatch.end()) return it->second;
        const auto v = static_cast<std::uint32_t>(std::stoul(hex.substr(1), nullptr, 16));
        core::Swatch s{hex, 10 + static_cast<double>(swatch.size()), core::ColorModel::RGB,
                       {static_cast<double>((v >> 16) & 0xff), static_cast<double>((v >> 8) & 0xff), static_cast<double>(v & 0xff), 0}, "", v};
        const auto id = d.fresh_id("swatch");
        d.put(id, s);
        return swatch[hex] = id;
    }
    // The named style an automatic one stands on (itself when named).
    std::string named_of(const std::string& st) const {
        std::string at = st;
        for (int g = 0; g < 32; ++g) {
            const auto it = b.styles.find(at);
            if (it == b.styles.end() || !it->second.automatic) return at;
            at = it->second.parent;
        }
        return at;
    }
    // s268: a paragraph naming a page style starts a new page with it (ODF);
    // a page style for right (left) pages only: on the next right- (left-)
    // hand page -- LibreOffice's "Right Page" chapter openings.
    core::ParaStart start_for_master(const std::string& master) const {
        if (master.empty()) return core::ParaStart::Anywhere;
        for (const auto& m : b.masters) {
            if (m.name != master) continue;
            const auto it = b.layouts.find(m.layout);
            if (it == b.layouts.end()) break;
            if (it->second.side == 1) return core::ParaStart::NextRightPage;
            if (it->second.side == 2) return core::ParaStart::NextLeftPage;
            break;
        }
        return core::ParaStart::NextPage;
    }
    std::string para_for(const std::string& odt_style) {
        const std::string named = named_of(odt_style);
        if (const auto it = para_style.find(named); it != para_style.end()) return it->second;
        const auto pr = b.resolved(named);
        core::Style s;
        const auto sit = b.styles.find(named);
        s.name = sit != b.styles.end() ? sit->second.display : (named.empty() ? "Default" : named);
        s.kind = core::StyleKind::Paragraph;
        s.pos = pos++;
        s.family = pr.family.empty() ? "Serif" : pr.family;
        s.size = pr.size > 0 ? pr.size : 12;
        s.leading = pr.leading > 0 ? pr.leading : std::round(s.size * 1.2 * (pr.leading_pct > 0 ? pr.leading_pct / 100 : 1) * 2) / 2;
        s.bold = pr.bold == 1;
        s.italic = pr.italic == 1;
        s.align = pr.align < 0 ? core::Align::Left : static_cast<core::Align>(std::clamp(pr.align, 0, 3));
        const auto set = [](double v, double& into) { if (v > -1e8) into = v; };
        set(pr.left, s.left_indent);
        set(pr.right, s.right_indent);
        set(pr.indent, s.first_indent);
        set(pr.before, s.space_before);
        set(pr.after, s.space_after);
        s.color = swatch_for(pr.color);
        onto(s, char_set(pr, s.size));   // s262: a paragraph style's character attributes
        if (pr.page_break == 1) s.start = core::ParaStart::NextPage;
        if (pr.page_break == 1 && s.space_before > 0) s.top_space = true;   // s242: a chapter heading sunk down its page, as in the file
        if (!pr.master.empty()) {   // s268: its page style -- a new page, on its side
            s.start = start_for_master(pr.master);
            if (s.space_before > 0) s.top_space = true;
        }
        // s257: keeps -- keep with next (a line), orphans / widows as Keep lines
        // (the larger; 1 or none is Off, more than 3 is kept at 3).
        if (pr.keep_next == 1) s.keep_next = 1;
        if (const int kl = std::max(pr.orphans, pr.widows); kl >= 2) s.keep_lines = std::min(kl, 3);
        const auto id = d.fresh_id("style");
        para_style[named] = id;
        d.put(id, s);
        ++info.styles;
        if (sit != b.styles.end() && !sit->second.next.empty() && sit->second.next != named) {   // Enter after it: its next
            const auto nx = para_for(sit->second.next);
            auto t = *d.as<core::Style>(id);
            t.next = nx;
            d.put(id, t);
        }
        return id;
    }
    std::string char_for(const std::string& named) {
        if (const auto it = char_style.find(named); it != char_style.end()) return it->second;
        const auto sit = b.styles.find(named);
        const auto& pp = sit != b.styles.end() ? sit->second.props : odt::Props{};
        core::Style s{sit != b.styles.end() ? sit->second.display : named, core::StyleKind::Character, pos++, pp.family, pp.size > 0 ? pp.size : 0, 0,
                      pp.bold == 1, pp.italic == 1, core::Align::Left};
        s.color = swatch_for(pp.color);
        onto(s, char_set(pp, pp.size > 0 ? pp.size : cur_size));   // s262
        const auto id = d.fresh_id("style");
        char_style[named] = id;
        d.put(id, s);
        ++info.styles;
        return id;
    }
    // s262: an ODT's character attributes at `size` points -- letter spacing
    // as tracking (thousandths of an em), a raised / lowered smaller run as a
    // superscript / subscript, a raised one at full size as a baseline shift.
    struct CharSet {
        std::optional<double> tracking, shift;
        int position = -1, caps = -1, underline = -1, strike = -1;
        std::optional<core::OtFeatures> ot;   // s266
    };
    static CharSet char_set(const odt::Props& p, double size) {
        CharSet c;
        if (size <= 0) size = 12;
        if (p.letter > -1e8) c.tracking = std::round(p.letter / size * 1000);
        if (p.rise > -1e8) {
            if (p.rise == 0) { c.position = 0; c.shift = 0.0; }
            else if (p.scale > 0 && p.scale < 100) c.position = p.rise > 0 ? 1 : 2;
            else c.shift = std::round(p.rise / 100 * size * 100) / 100;
        }
        c.caps = p.caps;
        c.underline = p.underline;
        c.strike = p.strike;
        // s266: font features ("onum&liga=0&-kern&ss02") and pair kerning off.
        core::OtFeatures f;
        bool any = false;
        std::size_t at = 0;
        const std::string& fs = p.features;
        while (at < fs.size()) {
            std::size_t e = fs.find('&', at);
            if (e == std::string::npos) e = fs.size();
            std::string t = fs.substr(at, e - at);
            at = e + 1;
            bool on = true;
            if (!t.empty() && (t[0] == '-' || t[0] == '+')) { on = t[0] == '+'; t.erase(0, 1); }
            if (const auto eq = t.find('='); eq != std::string::npos) { on = std::atoi(t.c_str() + eq + 1) != 0; t.erase(eq); }
            if (t == "liga") { f.liga = on ? 0 : 1; any = any || !on; }
            else if (t == "kern") { f.kern = on ? 0 : 1; any = any || !on; }
            else if (t == "lnum" && on) { f.figures = 1; any = true; }
            else if (t == "onum" && on) { f.figures = 2; any = true; }
            else if (t == "pnum" && on) { f.fig_space = 1; any = true; }
            else if (t == "tnum" && on) { f.fig_space = 2; any = true; }
            else if (t == "dlig" && on) { f.dlig = true; any = true; }
            else if (t == "frac" && on) { f.frac = true; any = true; }
            else if (t == "ordn" && on) { f.ordn = true; any = true; }
            else if (t == "swsh" && on) { f.swsh = true; any = true; }
            else if (t == "smcp" && on) c.caps = 2;
            else if (t.size() == 4 && t[0] == 's' && t[1] == 's' && on) {
                const int n = std::atoi(t.c_str() + 2);
                if (n >= 1 && n <= 20) { f.sset = n; any = true; }
            }
        }
        if (p.kerning == 0) { f.kern = 1; any = true; }
        if (any) c.ot = f;
        return c;
    }
    static void onto(core::Style& s, const CharSet& c) {
        if (c.tracking) s.tracking = *c.tracking;
        if (c.shift) s.shift = *c.shift;
        if (c.position >= 0) s.position = c.position;
        if (c.caps >= 0) s.caps = c.caps;
        if (c.underline >= 0) s.underline = c.underline == 1;
        if (c.strike >= 0) s.strike = c.strike == 1;
        if (c.ot) s.ot = *c.ot;   // s266
    }
    static void onto(core::Local& l, const CharSet& c) {
        if (c.tracking) l.tracking = c.tracking;
        if (c.shift) l.shift = c.shift;
        if (c.position >= 0) l.position = c.position;
        if (c.caps >= 0) l.caps = c.caps;
        if (c.underline >= 0) l.underline = c.underline;
        if (c.strike >= 0) l.strike = c.strike;
        if (c.ot) l.ot = c.ot;   // s266
    }
    double cur_size = 12;   // s262: the paragraph's size, for an automatic style's letter spacing
    // What an automatic style changes, as local formatting (over the paragraph style `ps`).
    core::Local local_of(const odt::Props& p) {
        core::Local l;
        if (p.bold >= 0) l.bold = p.bold;
        if (p.italic >= 0) l.italic = p.italic;
        if (p.size > 0) l.size = p.size;
        l.color = swatch_for(p.color);
        onto(l, char_set(p, p.size > 0 ? p.size : cur_size));   // s262
        return l;
    }
    // Spans (nested, overlapping) -> sorted runs that don't overlap; `base` over the whole text.
    std::vector<core::Run> runs_of(const std::string& text, const std::vector<odt::Span>& spans, const core::Local& base) {
        std::set<int> cuts{0, static_cast<int>(text.size())};
        for (const auto& s : spans) { cuts.insert(s.start); cuts.insert(s.end); }
        std::vector<core::Run> out;
        for (auto it = cuts.begin(); std::next(it) != cuts.end(); ++it) {
            const int a = *it, e = *std::next(it);
            if (e <= a) continue;
            core::Run r{a, e, "", base};
            // the covering spans, widest first, so the innermost wins
            std::vector<const odt::Span*> cover;
            for (const auto& s : spans) if (s.start <= a && s.end >= e) cover.push_back(&s);
            std::sort(cover.begin(), cover.end(), [](const odt::Span* x, const odt::Span* y) { return (x->end - x->start) > (y->end - y->start); });
            for (const auto* s : cover) {
                const auto st = b.styles.find(s->style);
                if (st == b.styles.end()) continue;
                if (st->second.automatic) {
                    const auto l = local_of(st->second.props);
                    if (l.bold >= 0) r.local.bold = l.bold;
                    if (l.italic >= 0) r.local.italic = l.italic;
                    if (l.size > 0) r.local.size = l.size;
                    if (!l.color.empty()) r.local.color = l.color;
                    if (l.tracking) r.local.tracking = l.tracking;   // s262
                    if (l.shift) r.local.shift = l.shift;
                    if (l.position >= 0) r.local.position = l.position;
                    if (l.caps >= 0) r.local.caps = l.caps;
                    if (l.underline >= 0) r.local.underline = l.underline;
                    if (l.strike >= 0) r.local.strike = l.strike;
                    if (l.ot) r.local.ot = l.ot;   // s266
                    if (!st->second.parent.empty() && b.styles.count(st->second.parent)) r.style = char_for(st->second.parent);
                } else {
                    r.style = char_for(s->style);
                }
            }
            if (r.style.empty() && r.local.empty()) continue;
            if (!out.empty() && out.back().end == a && out.back().style == r.style && out.back().local == r.local) out.back().end = e;
            else out.push_back(r);
        }
        return out;
    }
    // s243: a paragraph style made a list's at a level -- the base's look, the list's marker.
    std::string list_for(const std::string& base, const std::string& list, int level) {
        const auto key = base + "|" + list + "|" + std::to_string(level);
        if (const auto it = list_style.find(key); it != list_style.end()) return it->second;
        const auto lit = b.lists.find(list);
        if (lit == b.lists.end()) return base;
        const auto& lv = lit->second.levels;
        const odt::ListLevel L = level - 1 < static_cast<int>(lv.size()) ? lv[static_cast<std::size_t>(level - 1)] : (lv.empty() ? odt::ListLevel{} : lv.back());
        core::Style s = *d.as<core::Style>(base);
        s.list.type = L.number ? core::ListType::Number : core::ListType::Bullet;
        s.list.bullet = L.bullet.empty() ? "\u2022" : L.bullet;
        const auto& f = L.format;
        s.list.numbering = f == "i" ? core::Numbering::RomanLower : f == "I" ? core::Numbering::RomanUpper
                         : f == "a" ? core::Numbering::AlphaLower : f == "A" ? core::Numbering::AlphaUpper : core::Numbering::Arabic;
        s.list.after = L.suffix;
        s.list.hang = L.label > 0 ? L.label : 18;
        s.list.level = std::clamp(level, 1, 9);   // s251: as deep as it goes
        s.list.with_parent = L.number && L.display > 1 && level > 1;
        if (!L.text_style.empty() && b.styles.count(L.text_style)) s.list.marker_style = char_for(L.text_style);
        s.left_indent = L.indent >= 0 ? L.indent : s.list.hang * level;
        if (s.left_indent < s.list.hang) s.left_indent = s.list.hang;
        s.first_indent = 0;
        s.name += std::string(L.number ? " (numbered" : " (bullets") + (level > 1 ? ", level " + std::to_string(level) : std::string{}) + ")";
        s.pos = pos++;
        s.based_on.clear();
        s.own.clear();
        const auto id = d.fresh_id("style");
        d.put(id, s);
        ++info.styles;
        ++info.lists;
        return list_style[key] = id;
    }
    core::Para para_of(const odt::Para& p) {
        core::Para q;
        q.style = para_for(p.style);
        if (!p.list.empty() && p.list_level > 0) {   // s243: a list's item
            q.style = list_for(q.style, p.list, p.list_level);
            q.restart = std::max(0, p.start);
        }
        q.text = p.text;
        if (const auto* qs = d.as<core::Style>(q.style)) cur_size = qs->size > 0 ? qs->size : 12;   // s262
        core::Local base;
        if (const auto it = b.styles.find(p.style); it != b.styles.end() && it->second.automatic) {   // its own look
            const auto& own = it->second.props;
            base = local_of(own);
            if (own.align >= 0) q.over.align = static_cast<core::Align>(std::clamp(own.align, 0, 3));
            if (own.left > -1e8) q.over.left_indent = own.left;
            if (own.right > -1e8) q.over.right_indent = own.right;
            if (own.indent > -1e8) q.over.first_indent = own.indent;
            if (own.before > -1e8) q.over.space_before = own.before;
            if (own.after > -1e8) q.over.space_after = own.after;
            if (own.page_break == 1) q.brk = core::ParaStart::NextPage;
            if (!own.master.empty()) q.brk = start_for_master(own.master);   // s268
            if (own.keep_next >= 0) q.over.keep_next = own.keep_next;   // s257
        }
        q.runs = runs_of(p.text, p.spans, base);
        for (const auto& c : p.cites) {   // s291: a citation -- a reference to its source's anchor, shown as its number
            core::XRef r;
            const auto it = cite_anchor.find(c);
            r.target = it == cite_anchor.end() ? std::string{} : it->second;
            r.show = core::RefShow::Cite;
            q.refs.push_back(std::move(r));
            ++info.citations;
        }
        for (const auto& m : p.index) {   // s286: an index's marks
            core::IndexMark im;
            im.term = m.term.empty() ? std::string("?") : m.term;
            im.sub = m.sub;
            q.index.push_back(std::move(im));
            ++info.index_marks;
        }
        for (const auto& n : p.notes) {
            core::Note cn;
            cn.text = n.text;
            cn.runs = runs_of(n.text, n.spans, {});
            q.notes.push_back(std::move(cn));
            ++info.notes;
        }
        return q;
    }
};
}  // namespace

bool import_odt(Doc& d, const odt::Book& b, OdtInfo* out, const OdtOptions& opt, std::string* err) {
    OdtInfo local;
    OdtInfo& info = out ? *out : local;
    info = {};
    const auto report = [&](double f, const std::string& what) { if (opt.progress) opt.progress(f, what); };
    report(0.02, "Reading the styles…");
    // The page: the first master page's layout (the body's first paragraph's master, else "Standard", else any).
    const odt::PageLayout* L = nullptr;
    {
        std::string want;
        if (!b.body.empty()) want = b.resolved(b.body.front().style).master;
        for (const auto& m : b.masters)
            if ((want.empty() ? m.name == "Standard" : m.name == want) && b.layouts.count(m.layout)) L = &b.layouts.at(m.layout);
        if (!L && !b.masters.empty() && b.layouts.count(b.masters.front().layout)) L = &b.layouts.at(b.masters.front().layout);
        if (!L && !b.layouts.empty()) L = &b.layouts.begin()->second;
    }
    core::NewDoc n;
    n.title = b.title.empty() ? "Imported" : b.title;
    if (L && L->w > 36 && L->h > 36) { n.trim_w = L->w; n.trim_h = L->h; }
    else { n.trim_w = 8.5 * 72; n.trim_h = 11 * 72; }
    if (L) {
        const auto m = [](double v, double fb) { return v >= 0 ? v : fb; };
        n.margins = {m(L->top, 54), m(L->bottom, 54), m(L->left, 54), m(L->right, 54)};
    }
    n.pages = 1;
    n.facing = L && L->mirrored;
    // s268: a page style for one side only (a front matter's "right"), or a
    // mirrored body ("Standard"), or any side-only page style a paragraph
    // asks for: the book has sides -- facing pages.
    if (L && L->side != 0) n.facing = true;
    for (const auto& m : b.masters) {
        const auto it = b.layouts.find(m.layout);
        if (it == b.layouts.end()) continue;
        if (m.name == "Standard" && it->second.mirrored) n.facing = true;
    }
    for (const auto& [name, st] : b.styles) {
        if (st.props.master.empty()) continue;
        for (const auto& m : b.masters)
            if (m.name == st.props.master && b.layouts.count(m.layout) && b.layouts.at(m.layout).side != 0) n.facing = true;
    }
    n.text_frames = false;
    n.folios = false;
    if (!core::check_new(n).empty()) {
        if (err) *err = "its page won't make a document: " + core::check_new(n);
        return false;
    }
    core::new_document(d, n);
    Doc::Loading ld(d);
    d.put("page.1", std::nullopt);   // pages come as the text needs them
    for (const char* s : {"style.1", "style.2", "style.3", "style.4"}) d.put(s, std::nullopt);   // the blank document's own; the file brings its
    if (const auto* fn = d.as<core::Style>("style.6")) {   // Footnote stays (the flow sets notes in it), standing alone
        auto t = *fn;
        t.based_on.clear();
        t.own.clear();
        d.put("style.6", t);
    }
    Builder bl{d, b, info};
    // The story.
    report(0.08, "Setting the text in styles…");
    core::Story st{"Main", {}};
    bool any_foot = false, any_end = false;
    std::vector<char> heading1;   // s237: which paragraphs are chapters (level-1 headings, or a style at outline 1)
    std::vector<int> first_of;   // s245: each body paragraph's first story paragraph (a picture's anchor)
    // s272: a table of contents holds its place in the text -- an empty
    // paragraph starting its page (as its title's style does: the locket's
    // front matter's right-hand page); the text after it starts afresh. The
    // list itself goes on that page once the pages are set.
    std::vector<int> toc_para(b.tocs.size(), -1);
    std::vector<int> index_para(b.indexes.size(), -1);   // s286: an index holds its place as a contents list does
    std::vector<int> bib_para(b.bibliographies.size(), -1);   // s291: and a bibliography
    // s291: the sources first -- each a paragraph in a Source style, in a story of
    // their own (not placed), an anchor at its start, so the body's citations point at them.
    std::string source_style, source_story;
    if (!b.source_order.empty()) {
        core::Style ss = *d.as<core::Style>(bl.para_for(""));
        ss.name = "Source";
        ss.pos = bl.pos++;
        ss.left_indent = 18;
        ss.first_indent = -18;
        ss.space_after = std::max(ss.space_after, 4.0);
        ss.start = core::ParaStart::Anywhere;
        source_style = d.fresh_id("style");
        d.put(source_style, ss);
        core::Story srcs{"Sources", {}};
        int n = 0;
        for (const auto& id : b.source_order) {
            const auto& src = b.sources.at(id);
            std::string words;
            const auto add = [&](const std::string& w, const char* after) {
                if (w.empty()) return;
                words += w;
                if (words.back() != '.' || std::string(after) != ".") words += after;
                words += ' ';
            };
            add(src.author, ".");
            add(src.title, ".");
            add(src.journal, src.volume.empty() ? "." : "");
            if (!src.volume.empty()) add(src.volume, ".");
            if (!src.publisher.empty() && !src.year.empty()) { words += src.publisher + ", " + src.year + ". "; }
            else { add(src.publisher, "."); add(src.year, "."); }
            if (!src.pages.empty()) add("pp. " + src.pages, ".");
            add(src.url, "");
            while (!words.empty() && words.back() == ' ') words.pop_back();
            if (words.empty()) words = id;
            core::Para q;
            q.style = source_style;
            const std::string anchor = "anchor." + std::to_string(++n);
            q.text = std::string(core::kMarkAnchor) + words;
            q.anchors = {anchor};
            srcs.paras.push_back(std::move(q));
            bl.cite_anchor[id] = anchor;
        }
        // (not fresh_id: "story.1" is the book's text, put later under that name)
        for (int k = 2; source_story.empty() || d.as<core::Story>(source_story); ++k) source_story = "story." + std::to_string(k);
        d.put(source_story, srcs);
        info.sources = n;
    }
    bool after_toc = false;
    const auto put_tocs = [&](int at) {
        const auto hold = [&](const std::string& title_style, int& slot) {
            core::Para q;
            q.style = bl.para_for(title_style);
            q.brk = bl.start_for_master(b.resolved(title_style).master);
            if (q.brk == core::ParaStart::Anywhere) q.brk = core::ParaStart::NextPage;
            slot = static_cast<int>(st.paras.size());
            st.paras.push_back(q);
            heading1.push_back(false);
            after_toc = true;
        };
        for (std::size_t t = 0; t < b.tocs.size(); ++t)
            if (b.tocs[t].at == at) hold(b.tocs[t].title_style, toc_para[t]);
        for (std::size_t t = 0; t < b.indexes.size(); ++t)
            if (b.indexes[t].at == at) hold(b.indexes[t].title_style, index_para[t]);
        for (std::size_t t = 0; t < b.bibliographies.size(); ++t)   // s291
            if (b.bibliographies[t].at == at) hold(b.bibliographies[t].title_style, bib_para[t]);
    };
    for (const auto& p : b.body) {
        put_tocs(static_cast<int>(first_of.size()));
        first_of.push_back(static_cast<int>(st.paras.size()));
        const bool ch = p.outline == 1 || [&] { const auto it = b.styles.find(bl.named_of(p.style)); return it != b.styles.end() && it->second.outline == 1; }();
        if (ch) ++info.chapters;
        bool first = true;
        for (const auto& piece : split_lines(p)) {
            heading1.push_back(ch && first);   // the heading's first line is the chapter's start
            first = false;
            st.paras.push_back(bl.para_of(piece));
            if (after_toc) {   // s272: what follows the contents starts a page of its own
                auto& q = st.paras.back();
                if (q.brk == core::ParaStart::Anywhere && core::doc::start_of(d, q) == core::ParaStart::Anywhere) q.brk = core::ParaStart::NextPage;
                after_toc = false;
            }
            for (const auto& nn : piece.notes) (nn.endnote ? any_end : any_foot) = true;
        }
    }
    put_tocs(static_cast<int>(b.body.size()));
    if (st.paras.empty()) st.paras.push_back({bl.para_for(""), ""});
    info.paragraphs = static_cast<int>(st.paras.size());
    d.put("story.1", st);
    {   // s255: as a word processor's -- the text adds pages as it runs on
        auto s = d.settings();
        s.reflow = true;
        d.put("doc", s);
    }
    if (!b.author.empty()) {   // s239: its author, for a header's author field
        auto s = d.settings();
        s.author = b.author;
        d.put("doc", s);
    }
    if (any_end && !any_foot) {
        auto s = d.settings();
        s.note_at = core::NoteAt::StoryEnd;
        d.put("doc", s);
        info.notes_said.push_back("its notes are endnotes: set at the story's end");
    } else if (any_end) {
        info.notes_said.push_back("its endnotes came in as footnotes");
    }
    // s270: the page styles the book uses -- each a master of its own (its
    // size, margins, headers / footers). The first paragraph's (else
    // "Standard") is master.1; then every one a paragraph names, and where
    // each goes next ("Right Page" -> "Standard").
    const auto mp_of = [&](const std::string& nm) -> const odt::MasterPage* {
        for (const auto& m : b.masters) if (m.name == nm) return &m;
        return nullptr;
    };
    std::vector<std::string> used;
    const auto use = [&](const std::string& nm) {
        if (!nm.empty() && mp_of(nm) && std::find(used.begin(), used.end(), nm) == used.end()) used.push_back(nm);
    };
    std::string first_style = b.body.empty() ? std::string{} : b.resolved(b.body.front().style).master;
    if (!mp_of(first_style)) first_style = mp_of("Standard") ? "Standard" : b.masters.empty() ? std::string{} : b.masters.front().name;
    use(first_style);
    for (const auto& p : b.body) use(b.resolved(p.style).master);
    for (std::size_t i = 0; i < used.size(); ++i) use(mp_of(used[i])->next);
    std::vector<std::string> para_style_page;   // each story paragraph: the page style it names ("" none)
    const auto toc_pages = [&](int at) {   // s272: a contents placeholder -- its title's page style (s286: an index's too)
        for (const auto& t : b.tocs)
            if (t.at == at) {
                const std::string want = b.resolved(t.title_style).master;
                para_style_page.push_back(mp_of(want) ? want : std::string{});
            }
        for (const auto& t : b.indexes)
            if (t.at == at) {
                const std::string want = b.resolved(t.title_style).master;
                para_style_page.push_back(mp_of(want) ? want : std::string{});
            }
        for (const auto& t : b.bibliographies)   // s291
            if (t.at == at) {
                const std::string want = b.resolved(t.title_style).master;
                para_style_page.push_back(mp_of(want) ? want : std::string{});
            }
    };
    int body_i = 0;
    for (const auto& p : b.body) {
        toc_pages(body_i++);
        const std::string want = b.resolved(p.style).master;
        bool first = true;
        for (const auto& piece : split_lines(p)) {
            (void)piece;
            para_style_page.push_back(first && mp_of(want) ? want : std::string{});
            first = false;
        }
    }
    toc_pages(static_cast<int>(b.body.size()));
    // s288: a header's / footer's height -- its words' lines (each paragraph
    // its style's leading), or the file's least height when that's more.
    const auto head_height = [&](const std::vector<odt::Para>& right, const std::vector<odt::Para>& left, double least) {
        double h = 0;
        for (const auto* side : {&right, &left}) {
            double one = 0;
            for (const auto& p : *side)
                for (const auto& piece : split_lines(p)) {
                    const auto* ps = d.as<core::Style>(bl.para_for(piece.style));
                    one += ps ? ps->leading : 14;
                }
            h = std::max(h, one);
        }
        return std::max(least, h > 0 ? h + 2 : 0.0);
    };
    std::map<std::string, std::string> master_id;   // page style -> master
    std::string blank_master;   // s270: an inserted blank page's (LibreOffice prints them blank)
    {
        const core::Master base = *d.as<core::Master>("master.1");
        for (std::size_t i = 0; i < used.size(); ++i) {
            const auto* mp = mp_of(used[i]);
            core::Master m = base;
            m.prefix = std::string(1, static_cast<char>('A' + std::min<std::size_t>(i, 24)));
            m.name = mp->display.empty() ? mp->name : mp->display;
            m.pos = static_cast<double>(i + 1);
            if (const auto it = b.layouts.find(mp->layout); it != b.layouts.end()) {
                const auto& lay = it->second;
                if (lay.w > 36 && lay.h > 36) { m.w = lay.w; m.h = lay.h; }
                const auto mg = [](double v, double fb) { return v >= 0 ? v : fb; };
                m.margins = {mg(lay.top, base.margins.top), mg(lay.bottom, base.margins.bottom), mg(lay.left, base.margins.inside),
                             mg(lay.right, base.margins.outside)};
                // s288: a header / footer switched on takes its height and spacing from the body's room
                if (lay.head) m.margins.top += head_height(mp->header, mp->header_left, lay.head_min) + lay.head_gap;
                if (lay.foot) m.margins.bottom += head_height(mp->footer, mp->footer_left, lay.foot_min) + lay.foot_gap;
                m.columns = lay.columns;   // s289
                if (lay.columns > 1) m.gutter = lay.column_gap > 0 ? lay.column_gap : 12;
            }
            const std::string id = i == 0 ? std::string("master.1") : d.fresh_id("master");
            d.put(id, m);
            master_id[used[i]] = id;
        }
        if (used.empty()) master_id[""] = "master.1";
    }
    const auto blank_for = [&](const std::string& like) {   // a blank page's master: the page style's size and margins, nothing on it
        if (blank_master.empty()) {
            core::Master m = *d.as<core::Master>(like);
            m.prefix = std::string(1, static_cast<char>('A' + std::min<std::size_t>(used.size(), 24)));
            m.name = "Inserted blank";
            m.pos = static_cast<double>(used.size() + 1);
            blank_master = d.fresh_id("master");
            d.put(blank_master, m);
        }
        return blank_master;
    };

    // Pages, as many as the text takes.
    std::vector<std::string> pages, frames;
    const auto add_page = [&] {
        core::Page p;
        p.pos = static_cast<double>(pages.size() + 1);
        p.w = n.trim_w;
        p.h = n.trim_h;
        p.master = "master.1";
        const auto pid = d.fresh_id("page");
        d.put(pid, p);
        const core::Rect r = core::margin_rect(d, pid);
        const auto fid = d.fresh_id("frame");
        d.put(fid, core::Frame{pid, core::FrameKind::Text, r.x, r.y, r.w, r.h, 0, 1, "story.1", "", ""});
        if (!frames.empty()) {
            auto prev = *d.as<core::Frame>(frames.back());
            prev.next = fid;
            d.put(frames.back(), prev);
        }
        pages.push_back(pid);
        frames.push_back(fid);
    };
    std::size_t bytes = 0;
    for (const auto& p : st.paras) bytes += p.text.size() + 1;
    const core::Rect area = core::margin_rect(d, "master.1");
    const double body = d.as<core::Style>(st.paras.front().style)->size;
    const double per_page = std::max(200.0, area.w * area.h / (body * body * 0.55 * 1.3));   // a rough first guess
    for (int i = 0, k = std::max(1, static_cast<int>(std::ceil(static_cast<double>(bytes) / per_page))); i < k; ++i) add_page();
    if (!pages.empty()) d.put("section.1", core::Section{"Body", pages.front(), core::Numbering::Arabic, 1, n.facing ? core::Side::Right : core::Side::Any, "master.1"});
    int keep = 1;   // s245: pages kept however little text (a picture placed on page N)
    const auto fit = [&](double lo, double hi) {   // s245: as many pages as it takes (again, after the pictures)
    for (int round = 1; round <= 40; ++round) {
        report(std::min(hi, lo + (hi - lo) * round / 12.0), "Fitting the pages (" + std::to_string(pages.size()) + ")\u2026");
        const StoryFlow f = flow_story(d, "story.1");
        std::size_t placed = 0;
        int last = -1;
        std::map<std::string, int> at;
        for (std::size_t i = 0; i < frames.size(); ++i) at[frames[i]] = static_cast<int>(i);
        for (const auto& ft : f.frames) {
            if (ft.lines.empty()) continue;
            for (const auto& l : ft.lines) placed += static_cast<std::size_t>(l.end - l.start);
            if (const auto it = at.find(ft.frame); it != at.end()) last = std::max(last, it->second);
        }
        if (f.overset) {
            const double rest = static_cast<double>(bytes > placed ? bytes - placed : 0);
            const int more = std::max(1, static_cast<int>(std::ceil(rest / per_page * 1.1)));
            for (int i = 0; i < more; ++i) add_page();
            continue;
        }
        // empty pages off the end
        while (static_cast<int>(pages.size()) > std::max(keep, last + 1)) {
            d.put(frames.back(), std::nullopt);
            d.put(pages.back(), std::nullopt);
            frames.pop_back();
            pages.pop_back();
            auto prev = *d.as<core::Frame>(frames.back());
            prev.next.clear();
            d.put(frames.back(), prev);
        }
        break;
    }
    };
    fit(0.15, 0.6);
    // s270: each page its page style's master, as LibreOffice turns them --
    // a paragraph naming one starts it on its page, and each page after takes
    // the one before's "next"; an inserted blank page (a right-hand start's)
    // is blank. The page's text frame in its master's margins. True when
    // anything changed (the text then set again).
    const auto assign_masters = [&]() {
        if (used.empty()) return false;
        const StoryFlow f = flow_story(d, "story.1");
        std::map<std::string, std::string> starts;
        std::set<std::string> empty;
        for (const auto& ft : f.frames) {
            const auto* tf = d.as<core::Frame>(ft.frame);
            if (!tf) continue;
            if (ft.lines.empty()) empty.insert(tf->owner);
            for (const auto& l : ft.lines)
                if (l.start == 0 && l.para < static_cast<int>(para_style_page.size()) && !para_style_page[static_cast<std::size_t>(l.para)].empty() &&
                    !starts.count(tf->owner))
                    starts[tf->owner] = para_style_page[static_cast<std::size_t>(l.para)];
        }
        bool changed = false;
        std::string cur = first_style;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const auto& pid = pages[i];
            if (const auto it = starts.find(pid); it != starts.end()) cur = it->second;
            const bool blank = empty.count(pid) && !starts.count(pid) && i > 0 && i + 1 < pages.size() &&
                               d.frames_on(pid).size() == 1;   // (a page with a picture on it isn't blank)
            const std::string want = blank ? blank_for(master_id[cur]) : master_id[cur];
            core::Page pg = *d.as<core::Page>(pid);
            const auto* wm = d.as<core::Master>(want);
            if (pg.master != want || (wm && (pg.w != wm->w || pg.h != wm->h))) {
                pg.master = want;
                if (wm) { pg.w = wm->w; pg.h = wm->h; }
                d.put(pid, pg);
                changed = true;
            }
            const core::Rect r = core::margin_rect(d, pid);
            core::Frame fr = *d.as<core::Frame>(frames[i]);
            const auto* pm = d.as<core::Master>(d.as<core::Page>(pid)->master);   // s289: its page style's columns
            const int cols = pm ? std::max(1, pm->columns) : 1;
            const double gut = pm ? pm->gutter : 12;
            if (r.w > 1 && (std::abs(fr.x - r.x) > 0.01 || std::abs(fr.y - r.y) > 0.01 || std::abs(fr.w - r.w) > 0.01 || std::abs(fr.h - r.h) > 0.01 ||
                            fr.columns != cols || (cols > 1 && std::abs(fr.gutter - gut) > 0.01))) {
                fr.x = r.x;
                fr.y = r.y;
                fr.w = r.w;
                fr.h = r.h;
                fr.columns = cols;
                fr.gutter = gut;
                d.put(frames[i], fr);
                changed = true;
            }
            if (const auto* m = mp_of(cur); m && !m->next.empty() && master_id.count(m->next)) cur = m->next;
        }
        return changed;
    };
    for (int round = 0; round < 4 && assign_masters(); ++round) fit(0.55, 0.6);
    // s245 / s248: one file, one link -- however many frames show it (the body's and the headers')
    std::map<std::string, std::string> asset_of;   // href -> asset
    const auto asset_for = [&](const odt::Picture& pic, std::size_t k) -> std::string {
        std::string& aid = asset_of[pic.href.empty() ? pic.name : pic.href];
        if (aid.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(opt.picture_dir, ec);
            int pw = 0, ph = 0;
            const bool sized = odt::pixel_size(pic.bytes, pw, ph);
            const std::string ext = pic.bytes.compare(0, 4, "\x89PNG") == 0 ? ".png" : pic.bytes.compare(0, 3, "GIF") == 0 ? ".gif"
                                  : pic.bytes.compare(0, 2, "\xFF\xD8") == 0 ? ".jpg"
                                  : std::filesystem::path(pic.href).extension().string();
            std::string base = std::filesystem::path(pic.href).stem().string();   // the file's own name
            if (base.empty()) base = pic.name.empty() ? "picture-" + std::to_string(k + 1) : pic.name;
            for (auto& c : base) if (c == '/' || c == '\\' || c == ':') c = '_';
            for (int n = 2; std::filesystem::exists(std::filesystem::path(opt.picture_dir) / (base + ext)) && n < 10000; ++n)
                base = std::filesystem::path(pic.href).stem().string() + "-" + std::to_string(n);
            const auto path = (std::filesystem::path(opt.picture_dir) / (base + ext)).string();
            { std::ofstream out(path, std::ios::binary); out << pic.bytes; }
            core::Asset a;
            a.name = base + ext;
            a.pos = static_cast<double>(k + 1);
            a.path = path;
            a.color = core::picture_color(pic.bytes);
            if (a.color.empty()) a.color = "RGB";
            a.px_w = sized ? pw : 0;
            a.px_h = sized ? ph : 0;
            a.stamp = core::file_stamp(path);
            aid = d.fresh_id("asset");
            d.put(aid, a);
        }
        return aid;
    };
    // s245: the pictures -- written out (opt.picture_dir) and linked, placed by their anchors;
    // then the pages fitted again, the anchored ones moved to where their paragraphs went.
    if (!opt.picture_dir.empty() && !b.images.empty()) {
        report(0.62, "Placing the pictures\u2026");
        std::error_code ec;
        std::filesystem::create_directories(opt.picture_dir, ec);
        for (const auto& pic : b.images)
            if (pic.anchor == "page" && !pic.bytes.empty()) keep = std::max(keep, std::min(pic.page, 999));
        while (static_cast<int>(pages.size()) < keep) add_page();
        struct Placed1 { std::size_t k; std::string frame; };
        std::vector<Placed1> placed;
        const auto where = [&](const odt::Picture& pic, std::string& page, double& x, double& y) {   // on which page, at what place
            const core::Rect page_area = core::margin_rect(d, "master.1");
            if (pic.anchor == "page" || pic.para < 0 || pic.para >= static_cast<int>(first_of.size())) {
                page = pages[static_cast<std::size_t>(std::clamp(pic.page, 1, static_cast<int>(pages.size())) - 1)];
                x = pic.x;
                y = pic.y;
                return true;
            }
            const int sp = first_of[static_cast<std::size_t>(pic.para)];
            const StoryFlow f = flow_story(d, "story.1");
            for (const auto& ft : f.frames)
                for (const auto& l : ft.lines) {
                    if (l.para != sp) continue;
                    if (pic.anchor == "as-char" && l.end < pic.at && l.end < static_cast<int>(st.paras[static_cast<std::size_t>(sp)].text.size())) continue;
                    const auto* tf = d.as<core::Frame>(ft.frame);
                    page = tf->owner;
                    const double top = tf->y + l.y;
                    const auto* ps = d.as<core::Style>(st.paras[static_cast<std::size_t>(sp)].style);
                    const auto& q = st.paras[static_cast<std::size_t>(sp)];
                    const bool centred = q.over.align ? *q.over.align == core::Align::Center : ps && ps->align == core::Align::Center;
                    const int hp = pic.anchor == "as-char" ? (centred ? 2 : 1) : pic.hpos;
                    const double ax = pic.whole_page ? 0 : tf->x, aw = pic.whole_page ? n.trim_w : tf->w;
                    x = hp == 1 ? ax : hp == 2 ? ax + (aw - pic.w) / 2 : hp == 3 ? ax + aw - pic.w : ax + pic.x;
                    y = pic.anchor == "as-char" ? top : top + (pic.vpos == 0 ? pic.y : 0);
                    (void)page_area;
                    return true;
                }
            return false;
        };
        for (std::size_t k = 0; k < b.images.size(); ++k) {
            const auto& pic = b.images[k];
            if (pic.bytes.empty()) continue;
            std::string page;
            double x = 0, y = 0;
            if (!where(pic, page, x, y)) continue;
            const std::string aid = asset_for(pic, k);
            core::Frame fr{page, core::FrameKind::Image, x, y, pic.w, pic.h, 0, 1e6 + static_cast<double>(k), "", "", aid};
            fr.fit = core::ImageFit::Fit;
            fr.wrap = pic.wrap == 0 ? core::Wrap::None : pic.wrap == 1 ? core::Wrap::Around : core::Wrap::Jump;
            if (pic.wrap != 0) fr.wrap_off = pic.anchor == "as-char" ? core::Sides{0, 0, 0, 0} : pic.anchor == "page" ? core::Sides{6, 6, 6, 6} : core::Sides{0, 6, 6, 6};
            const auto fid = d.fresh_id("frame");
            d.put(fid, fr);
            placed.push_back({k, fid});
            ++info.placed;
        }
        fit(0.66, 0.8);
        for (int pass = 0; pass < 2; ++pass) {   // the paragraphs moved: their pictures after them
            bool moved = false;
            for (const auto& pl : placed) {
                const auto& pic = b.images[pl.k];
                if (pic.anchor == "page") continue;
                std::string page;
                double x = 0, y = 0;
                auto fr = *d.as<core::Frame>(pl.frame);
                // (its own wrap moves its paragraph: look with it out of the way)
                d.put(pl.frame, std::nullopt);
                const bool ok = where(pic, page, x, y);
                if (ok && (page != fr.owner || std::abs(x - fr.x) > 0.5 || std::abs(y - fr.y) > 0.5)) {
                    fr.owner = page;
                    fr.x = x;
                    fr.y = y;
                    moved = true;
                }
                d.put(pl.frame, fr);
            }
            if (!moved) break;
            fit(0.8, 0.9);
        }
    }
    for (int round = 0; round < 3 && assign_masters(); ++round) fit(0.9, 0.92);   // s270: after the pictures moved things
    info.pages = static_cast<int>(pages.size());
    // s237: the first master page's header and footer -- each a story in a frame on the
    // master, in the top / bottom margin; facing with uneven margins: one a side (the
    // left pages' own words when the file has them).
    // s270: new pages in the body take the body's page style ("Standard"), else the first.
    const std::string body_master = master_id.count("Standard") ? master_id["Standard"] : std::string("master.1");
    if (auto* s1 = d.as<core::Section>("section.1"); s1 && !opt.sections) {
        auto s = *s1;
        s.master = body_master;
        d.put("section.1", s);
    }
    report(0.92, "Headers and footers…");
    // s270: every page style's, on its own master (was the first's alone).
    std::vector<std::pair<const odt::MasterPage*, std::string>> heads;
    for (const auto& nm : used) heads.push_back({mp_of(nm), master_id[nm]});
    if (heads.empty() && !b.masters.empty()) heads.push_back({&b.masters.front(), "master.1"});
    for (const auto& [mp, mid] : heads) {
        const auto& ms = *d.as<core::Master>(mid);
        const std::string tag = heads.size() > 1 ? " (" + ms.name + ")" : std::string{};
        const auto place = [&](const std::vector<odt::Para>& right, const std::vector<odt::Para>& left, bool top, const char* name) {
            if (right.empty() && left.empty()) return;
            {   // s270: a header with no words (a page style's, switched on but empty): no frame
                bool words = false;
                for (const auto* side : {&right, &left})
                    for (const auto& p : *side) words = words || !p.text.empty() || !p.pictures.empty();
                if (!words) return;
            }
            const auto story_of = [&](const std::vector<odt::Para>& ps, const std::string& nm) {
                core::Story s{nm, {}};
                for (const auto& p : ps)
                    for (const auto& piece : split_lines(p)) s.paras.push_back(bl.para_of(piece));
                const auto id = d.fresh_id("story");
                d.put(id, s);
                double h = 0;
                for (const auto& p : s.paras) h += d.as<core::Style>(p.style)->leading;
                return std::pair{id, std::max(12.0, h + 2)};
            };
            const bool two = n.facing && (std::abs(ms.margins.inside - ms.margins.outside) > 0.01 || !left.empty());
            // s288: the page style's own header / footer: from the page's margin, its height (else: centred in the margin)
            const odt::PageLayout* lay = nullptr;
            if (const auto it = b.layouts.find(mp->layout); it != b.layouts.end() && (top ? it->second.head : it->second.foot)) lay = &it->second;
            const auto frame = [&](const std::string& story, double h, core::Side side, const std::vector<odt::Para>& ps) {   // (h: s288 may grow it to the file's height)
                const core::Rect r = core::margin_rect(d, mid, side == core::Side::Any ? core::Side::Right : side);
                double y = top ? std::max(0.0, (ms.margins.top - h) / 2) : ms.h - ms.margins.bottom + std::max(0.0, (ms.margins.bottom - h) / 2);
                if (lay) {
                    const double room = top ? head_height(mp->header, mp->header_left, lay->head_min) : head_height(mp->footer, mp->footer_left, lay->foot_min);
                    h = std::max(h, room);
                    y = top ? std::max(0.0, lay->top) : ms.h - std::max(0.0, lay->bottom) - h;
                }
                core::Frame f{mid, core::FrameKind::Text, r.x, y, r.w, h, 0, 1, story, "", ""};
                f.side = side;
                f.role = top ? 1 : 2;   // s275
                d.put(d.fresh_id("frame"), f);
                // s248: its pictures (a logo, a rule) -- by the header's frame, over it, no wrap
                if (opt.picture_dir.empty()) return;
                std::size_t k = 0;
                for (const auto& p : ps)
                    for (const auto& pic : p.pictures) {
                        ++k;
                        if (pic.bytes.empty()) continue;
                        const bool centred = b.resolved(p.style).align == 1;
                        const int hp = pic.anchor == "as-char" ? (centred ? 2 : 1) : pic.hpos;
                        const double px = hp == 1 ? r.x : hp == 2 ? r.x + (r.w - pic.w) / 2 : hp == 3 ? r.x + r.w - pic.w : r.x + pic.x;
                        const double py = pic.anchor == "page" ? pic.y : pic.anchor == "as-char" || pic.vpos != 0 ? y : y + pic.y;
                        core::Frame g{mid, core::FrameKind::Image, px, py, pic.w, pic.h, 0, 2 + static_cast<double>(k), "", "",
                                      asset_for(pic, 100000 + k)};
                        g.fit = core::ImageFit::Fit;
                        g.wrap = core::Wrap::None;
                        g.side = side;
                        d.put(d.fresh_id("frame"), g);
                        if (side != core::Side::Left || &ps == &left) {   // (once for a picture shown on both sides)
                            ++info.placed;
                            ++info.header_pictures;
                        }
                    }
            };
            const auto [rs, rh] = story_of(right.empty() ? left : right, name);
            const auto& rp = right.empty() ? left : right;
            if (!two) { frame(rs, rh, core::Side::Any, rp); return; }
            frame(rs, rh, core::Side::Right, rp);
            if (left.empty()) {   // the same words, its own place -- s270: its own story (one story in two frames threads them: the second got nothing)
                const auto [cs, ch] = story_of(rp, std::string(name) + " (left)");
                frame(cs, ch, core::Side::Left, rp);
                return;
            }
            const auto [ls, lh] = story_of(left, std::string(name) + " (left)");
            frame(ls, lh, core::Side::Left, left);
        };
        place(mp->header, mp->header_left, true, ("Header" + tag).c_str());
        place(mp->footer, mp->footer_left, false, ("Footer" + tag).c_str());
        if (!mp->header.empty() || !mp->header_left.empty()) ++info.headers;
        if (!mp->footer.empty() || !mp->footer_left.empty()) ++info.headers;
    }
    // s237: chapters / body -- the words before the first chapter (Heading 1) are the
    // front matter (its own section, roman numbers); the body begins on that chapter's page.
    if (opt.sections) {   // s247: unless asked not to
        int first_ch = -1;
        for (std::size_t i = 0; i < st.paras.size() && first_ch < 0; ++i)
            if (i < heading1.size() && heading1[i]) first_ch = static_cast<int>(i);
        if (first_ch > 0) {
            const StoryFlow f = flow_story(d, "story.1");
            std::string page;
            for (const auto& ft : f.frames) {
                for (const auto& l : ft.lines)
                    if (l.para == first_ch) { page = d.as<core::Frame>(ft.frame)->owner; break; }
                if (!page.empty()) break;
            }
            if (!page.empty() && page != pages.front()) {
                d.put("section.1", core::Section{"Front matter", pages.front(), core::Numbering::RomanLower, 1, core::Side::Any,
                                                 d.as<core::Page>(pages.front())->master});
                d.put("section.2", core::Section{"Body", page, core::Numbering::Arabic, 1, core::Side::Any, body_master});   // s270
                info.front_matter = true;
            }
        }
    }
    // s272: each table of contents a contents list -- on the page its place
    // holds, its title and level 1's entry style from the file; gathering one
    // paragraph style per outline level (those the book's headings are in);
    // built, so its page numbers are this book's.
    if (!b.tocs.empty()) {
        report(0.96, "The contents…");
        core::DocJournal j{d};
        const StoryFlow f = flow_story(d, "story.1");
        for (std::size_t t = 0; t < b.tocs.size(); ++t) {
            const auto& toc = b.tocs[t];
            if (toc_para[t] < 0) continue;
            std::string page;
            for (const auto& ft : f.frames) {
                for (const auto& l : ft.lines)
                    if (l.para == toc_para[t]) { page = d.as<core::Frame>(ft.frame)->owner; break; }
                if (!page.empty()) break;
            }
            if (page.empty()) continue;
            std::vector<std::string> by_level(static_cast<std::size_t>(toc.levels));
            for (std::size_t i = 0; i < b.body.size() && i < first_of.size(); ++i) {
                const auto& p = b.body[i];
                int lv = p.outline;
                if (lv <= 0) if (const auto it = b.styles.find(bl.named_of(p.style)); it != b.styles.end()) lv = it->second.outline;
                if (lv < 1 || lv > toc.levels) continue;
                auto& slot = by_level[static_cast<std::size_t>(lv - 1)];
                if (slot.empty()) slot = st.paras[static_cast<std::size_t>(first_of[i])].style;
            }
            std::vector<std::string> from;
            for (const auto& s : by_level) if (!s.empty() && std::find(from.begin(), from.end(), s) == from.end()) from.push_back(s);
            if (from.empty()) continue;
            const std::string title_style = bl.para_for(toc.title_style);
            const std::string entry_style = toc.entry_style.empty() ? title_style : bl.para_for(toc.entry_style);
            const std::string name = toc.title.empty() ? std::string("Contents") : toc.title;
            const auto sid = d.fresh_id("story");
            core::Story ls{name, {}};
            if (!toc.title.empty()) ls.paras.push_back({title_style, toc.title});
            ls.paras.push_back({entry_style, ""});
            d.put(sid, ls);
            const core::Rect r = core::margin_rect(d, page);
            if (toc.right_tab && r.w > 1) {   // the page number at the right edge, the leader up to it -- as the file's entry template
                auto es = *d.as<core::Style>(entry_style);
                const double at = std::max(12.0, r.w - std::max(0.0, es.left_indent) - std::max(0.0, es.right_indent));
                es.tabs = {core::TabStop{at, core::TabAlign::Right, toc.leader}};
                d.put(entry_style, es);
            }
            double z = 0;
            for (const auto& o : d.frames_on(page)) z = std::max(z, d.as<core::Frame>(o)->z);
            core::Frame fr{page, core::FrameKind::Text, r.x, r.y, r.w, r.h, 0, z + 1, sid, "", ""};
            d.put(d.fresh_id("frame"), fr);
            double pos = 0;
            for (const auto& id : d.ids_of<core::GenList>()) pos = std::max(pos, d.as<core::GenList>(id)->pos);
            core::GenList gl{name, core::ListKind::Contents, pos + 1, from, page, -1, sid};
            gl.entry_styles = {entry_style};
            const auto lid = d.fresh_id("list");
            d.put(lid, gl);
            build_list(j, d, lid);
            ++info.contents_lists;
        }
    }
    // s286: each alphabetical index an index of this book's marks, on the page its place holds.
    if (!b.indexes.empty()) {
        report(0.98, "The index\u2026");
        core::DocJournal j{d};
        const StoryFlow f = flow_story(d, "story.1");
        for (std::size_t t = 0; t < b.indexes.size(); ++t) {
            if (index_para[t] < 0) continue;
            std::string page;
            for (const auto& ft : f.frames) {
                for (const auto& l : ft.lines)
                    if (l.para == index_para[t]) { page = d.as<core::Frame>(ft.frame)->owner; break; }
                if (!page.empty()) break;
            }
            if (page.empty()) continue;
            double pos = 0;
            for (const auto& id : d.ids_of<core::GenList>()) pos = std::max(pos, d.as<core::GenList>(id)->pos);
            core::GenList gl;
            gl.name = b.indexes[t].title.empty() ? std::string("Index") : b.indexes[t].title;
            gl.kind = core::ListKind::Index;
            gl.pos = pos + 1;
            gl.placed_on = page;
            gl.built_entries = -1;
            const auto lid = d.fresh_id("list");
            d.put(lid, gl);
            build_list(j, d, lid);
            ++info.indexes;
        }
    }
    // s291: the sources' bibliography -- on the page its place holds (or kept, not placed,
    // so the citations still count: [1], [2] ...), built from the Source paragraphs.
    if (!source_style.empty()) {
        core::DocJournal j{d};
        const StoryFlow f = flow_story(d, "story.1");
        const auto make = [&](const std::string& name, const std::string& page) {
            double pos = 0;
            for (const auto& id : d.ids_of<core::GenList>()) pos = std::max(pos, d.as<core::GenList>(id)->pos);
            core::GenList gl;
            gl.name = name;
            gl.kind = core::ListKind::Bibliography;
            gl.pos = pos + 1;
            gl.from_styles = {source_style};
            gl.placed_on = page;
            gl.built_entries = -1;
            gl.cite_order = true;   // LibreOffice numbers them as cited
            const auto lid = d.fresh_id("list");
            d.put(lid, gl);
            if (!page.empty()) build_list(j, d, lid);
            ++info.bibliographies;
        };
        bool any = false;
        for (std::size_t t = 0; t < b.bibliographies.size(); ++t) {
            if (bib_para[t] < 0) continue;
            std::string page;
            for (const auto& ft : f.frames) {
                for (const auto& l : ft.lines)
                    if (l.para == bib_para[t]) { page = d.as<core::Frame>(ft.frame)->owner; break; }
                if (!page.empty()) break;
            }
            if (page.empty()) continue;
            make(b.bibliographies[t].title.empty() ? std::string("Bibliography") : b.bibliographies[t].title, page);
            any = true;
        }
        if (!any) make("Bibliography", "");
    }
    // s290: a contents list or index longer than its page runs on -- pages added
    // after its page (its master), its frame threaded on, as text reflow does;
    // then the book's text set again (a chapter that now falls on the wrong
    // side takes its blank page).
    {
        core::DocJournal j{d};
        int more = 0;
        for (const auto& lid : d.ids_of<core::GenList>()) {
            const auto* l = d.as<core::GenList>(lid);
            if (!l || !d.as<core::Story>(l->story)) continue;
            if (layout::reflows(d, l->story) && flow_story(d, l->story).overset) {
                const int n1 = layout::reflow_pages(j, d, l->story);
                more += n1;
                info.list_pages += n1;
            }
        }
        if (more > 0) {
            fit(0.99, 0.995);
            for (int round = 0; round < 3 && assign_masters(); ++round) fit(0.995, 0.997);
            info.pages = static_cast<int>(d.pages().size());
        }
    }
    info.masters = std::max<int>(1, static_cast<int>(used.size()) + (blank_master.empty() ? 0 : 1));
    info.tables = b.tables;
    info.pictures = b.pictures;
    info.contents = b.contents;
    if (b.tables) info.notes_said.push_back(std::to_string(b.tables) + (b.tables == 1 ? " table" : " tables") + " left out (tables come later)");
    if (b.pictures > info.placed)
        info.notes_said.push_back(std::to_string(b.pictures - info.placed) + (b.pictures - info.placed == 1 ? " picture" : " pictures") + " left out -- place them");
    if (b.contents) info.notes_said.push_back("its contents / index left out -- build one from the styles");
    report(1.0, "Done");
    return true;
}

}  // namespace composix::layout
