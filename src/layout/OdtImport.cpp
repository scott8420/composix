// layout::import_odt -- see the header.
#include "layout/OdtImport.hpp"
#include "layout/Flow.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace composix::layout {
namespace {
using core::Doc;
namespace odt = core::odt;

// A line break (\n) ends a paragraph: the pieces, each its own spans and notes.
std::vector<odt::Para> split_lines(const odt::Para& p) {
    std::vector<odt::Para> out;
    std::size_t a = 0;
    std::size_t note = 0;
    for (;;) {
        const auto e = p.text.find('\n', a);
        const std::size_t b = e == std::string::npos ? p.text.size() : e;
        odt::Para q;
        q.style = p.style;
        q.outline = p.outline;
        q.text = p.text.substr(a, b - a);
        for (const auto& s : p.spans) {
            const int s0 = std::max(s.start, static_cast<int>(a)), s1 = std::min(s.end, static_cast<int>(b));
            if (s1 > s0) q.spans.push_back({s0 - static_cast<int>(a), s1 - static_cast<int>(a), s.style});
        }
        for (std::size_t i = q.text.find(core::kMarkNote); i != std::string::npos; i = q.text.find(core::kMarkNote, i + 3))
            if (note < p.notes.size()) q.notes.push_back(p.notes[note++]);
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
    double pos = 10;
    std::string body_id;                              // the default paragraph style's

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
        if (pr.page_break == 1) s.start = core::ParaStart::NextPage;
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
        const auto id = d.fresh_id("style");
        char_style[named] = id;
        d.put(id, s);
        ++info.styles;
        return id;
    }
    // What an automatic style changes, as local formatting (over the paragraph style `ps`).
    core::Local local_of(const odt::Props& p) {
        core::Local l;
        if (p.bold >= 0) l.bold = p.bold;
        if (p.italic >= 0) l.italic = p.italic;
        if (p.size > 0) l.size = p.size;
        l.color = swatch_for(p.color);
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
    core::Para para_of(const odt::Para& p) {
        core::Para q;
        q.style = para_for(p.style);
        q.text = p.text;
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
        }
        q.runs = runs_of(p.text, p.spans, base);
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
    for (const auto& p : b.body) {
        const bool ch = p.outline == 1 || [&] { const auto it = b.styles.find(bl.named_of(p.style)); return it != b.styles.end() && it->second.outline == 1; }();
        if (ch) ++info.chapters;
        bool first = true;
        for (const auto& piece : split_lines(p)) {
            heading1.push_back(ch && first);   // the heading's first line is the chapter's start
            first = false;
            st.paras.push_back(bl.para_of(piece));
            for (const auto& nn : piece.notes) (nn.endnote ? any_end : any_foot) = true;
        }
    }
    if (st.paras.empty()) st.paras.push_back({bl.para_for(""), ""});
    info.paragraphs = static_cast<int>(st.paras.size());
    d.put("story.1", st);
    if (any_end && !any_foot) {
        auto s = d.settings();
        s.note_at = core::NoteAt::StoryEnd;
        d.put("doc", s);
        info.notes_said.push_back("its notes are endnotes: set at the story's end");
    } else if (any_end) {
        info.notes_said.push_back("its endnotes came in as footnotes");
    }
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
    for (int round = 1; round <= 40; ++round) {
        report(std::min(0.9, 0.15 + 0.7 * round / 12.0), "Fitting the pages (" + std::to_string(pages.size()) + ")…");
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
        while (static_cast<int>(pages.size()) > std::max(1, last + 1)) {
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
    info.pages = static_cast<int>(pages.size());
    // s237: the first master page's header and footer -- each a story in a frame on the
    // master, in the top / bottom margin; facing with uneven margins: one a side (the
    // left pages' own words when the file has them).
    report(0.92, "Headers and footers…");
    const odt::MasterPage* mp = nullptr;
    {
        std::string want;
        if (!b.body.empty()) want = b.resolved(b.body.front().style).master;
        for (const auto& m : b.masters) if (m.name == (want.empty() ? std::string("Standard") : want)) mp = &m;
        if (!mp && !b.masters.empty()) mp = &b.masters.front();
    }
    if (mp) {
        const auto& ms = *d.as<core::Master>("master.1");
        const auto place = [&](const std::vector<odt::Para>& right, const std::vector<odt::Para>& left, bool top, const char* name) {
            if (right.empty() && left.empty()) return;
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
            const auto frame = [&](const std::string& story, double h, core::Side side) {
                const core::Rect r = core::margin_rect(d, "master.1", side == core::Side::Any ? core::Side::Right : side);
                const double y = top ? std::max(0.0, (ms.margins.top - h) / 2) : n.trim_h - ms.margins.bottom + std::max(0.0, (ms.margins.bottom - h) / 2);
                core::Frame f{"master.1", core::FrameKind::Text, r.x, y, r.w, h, 0, 1, story, "", ""};
                f.side = side;
                d.put(d.fresh_id("frame"), f);
            };
            const auto [rs, rh] = story_of(right.empty() ? left : right, name);
            if (!two) { frame(rs, rh, core::Side::Any); return; }
            frame(rs, rh, core::Side::Right);
            if (left.empty()) { frame(rs, rh, core::Side::Left); return; }   // the same words, its own place
            const auto [ls, lh] = story_of(left, std::string(name) + " (left)");
            frame(ls, lh, core::Side::Left);
        };
        place(mp->header, mp->header_left, true, "Header");
        place(mp->footer, mp->footer_left, false, "Footer");
        if (!mp->header.empty() || !mp->header_left.empty()) ++info.headers;
        if (!mp->footer.empty() || !mp->footer_left.empty()) ++info.headers;
    }
    // s237: chapters / body -- the words before the first chapter (Heading 1) are the
    // front matter (its own section, roman numbers); the body begins on that chapter's page.
    {
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
                d.put("section.1", core::Section{"Front matter", pages.front(), core::Numbering::RomanLower, 1, core::Side::Any, "master.1"});
                d.put("section.2", core::Section{"Body", page, core::Numbering::Arabic, 1, core::Side::Any, "master.1"});
                info.front_matter = true;
            }
        }
    }
    info.tables = b.tables;
    info.pictures = b.pictures;
    info.contents = b.contents;
    if (b.tables) info.notes_said.push_back(std::to_string(b.tables) + (b.tables == 1 ? " table" : " tables") + " left out (tables come later)");
    if (b.pictures) info.notes_said.push_back(std::to_string(b.pictures) + (b.pictures == 1 ? " picture" : " pictures") + " left out -- place them");
    if (b.contents) info.notes_said.push_back("its contents / index left out -- build one from the styles");
    report(1.0, "Done");
    return true;
}

}  // namespace composix::layout
