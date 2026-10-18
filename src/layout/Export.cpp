#include "layout/Export.hpp"
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"

#include <cairo-pdf.h>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <cctype>
#include <set>

namespace composix::layout {

namespace {

constexpr double kMarkGap = 3;    // crop marks start this far outside the bleed
constexpr double kMarkLen = 15;   // and run this long
constexpr double kSlug = kMarkGap + kMarkLen + 3;

void draw_image(cairo_t* cr, const core::Doc& d, const core::Frame& f, const ImageSource& images,
                std::set<std::string>* missing, const PageDraw& how) {
    const auto* a = d.as<core::Asset>(f.asset);
    if (!a) return;
    cairo_surface_t* pic = images ? images(*a, f.w, f.h) : nullptr;
    if (!pic || cairo_surface_status(pic) != CAIRO_STATUS_SUCCESS) {
        if (pic) cairo_surface_destroy(pic);
        if (missing) missing->insert(a->name);
        if (how.placeholders) {   // s020: a soft plate where the picture would be
            cairo_set_source_rgb(cr, 0.80, 0.84, 0.76);
            cairo_rectangle(cr, f.x, f.y, f.w, f.h);
            cairo_fill(cr);
        }
        return;
    }
    const int pw = cairo_image_surface_get_width(pic), ph = cairo_image_surface_get_height(pic);
    // The picture's own proportions: the file's pixels (a raster), else the surface.
    const double iw = a->px_w > 0 ? a->px_w : pw, ih = a->px_h > 0 ? a->px_h : ph;
    const core::Rect r = core::picture_rect({f.x, f.y, f.w, f.h}, f, iw, ih);
    cairo_save(cr);
    cairo_rectangle(cr, f.x, f.y, f.w, f.h);
    cairo_clip(cr);
    cairo_translate(cr, r.x, r.y);
    cairo_scale(cr, r.w / std::max(1, pw), r.h / std::max(1, ph));
    cairo_set_source_surface(cr, pic, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(pic);
}

void draw_frames(cairo_t* cr, const core::Doc& d, const core::Doc::Id& owner, FlowCache& flows,
                 const ImageSource& images, std::set<std::string>* missing, const PageDraw& how,
                 const core::Doc::Id& on_page) {
    for (const auto& fid : d.frames_on(owner)) {
        if (owner != on_page && !core::master_item_on(d, fid, on_page)) continue;   // s025
        const auto& f = *d.as<core::Frame>(fid);
        // s021: the swatch at its tint; the frame's opacity on each part.
        const double op = std::clamp(f.opacity, 0.0, 1.0);
        const auto paint = [&](const std::string& sw, double tint) {
            const auto* s = d.as<core::Swatch>(sw);
            if (!s) return false;
            const auto c = core::tint_rgb(core::doc::screen_rgb(*s), tint);
            cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, op);
            return true;
        };
        if (paint(f.fill, f.fill_tint)) { cairo_rectangle(cr, f.x, f.y, f.w, f.h); cairo_fill(cr); }   // s016
        if (op < 1) cairo_push_group(cr);
        switch (f.kind) {
        case core::FrameKind::Text:
            if (f.story.empty()) break;
            cairo_save(cr);
            cairo_translate(cr, f.x, f.y);
            // s023: markers filled in -- a master's for the page it's drawn on.
            if (const auto& rs = flows.resolved(d, f.story, d.as<core::Master>(owner) ? on_page : std::string{}); rs.any)
                draw_frame_text(cr, d, f.story, rs.flow, fid, 0, 0, 0.0, &rs.text);
            else
                draw_frame_text(cr, d, f.story, flows.get(d, f.story), fid, 0, 0, 0.0);
            cairo_restore(cr);
            break;
        case core::FrameKind::Image:
            draw_image(cr, d, f, images, missing, how);
            break;
        case core::FrameKind::Shape:   // its fill / stroke are all it has
            break;
        }
        if (op < 1) { cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, op); }
        if (f.stroke_w > 0 && paint(f.stroke, f.stroke_tint)) {
            cairo_set_line_width(cr, f.stroke_w);
            cairo_rectangle(cr, f.x, f.y, f.w, f.h);
            cairo_stroke(cr);
        }
    }
}

void crop_marks(cairo_t* cr, double w, double h, double bleed) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 0.25);
    const double a = bleed + kMarkGap, b = a + kMarkLen;
    for (const double x : {0.0, w})
        for (const double y : {0.0, h}) {
            const double sx = x == 0 ? -1 : 1, sy = y == 0 ? -1 : 1;
            cairo_move_to(cr, x + sx * a, y);   // horizontal, level with the trim edge
            cairo_line_to(cr, x + sx * b, y);
            cairo_move_to(cr, x, y + sy * a);   // vertical
            cairo_line_to(cr, x, y + sy * b);
        }
    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace

void draw_page(cairo_t* cr, const core::Doc& d, const core::Doc::Id& id, FlowCache& flows, const ImageSource& images,
               std::set<std::string>* missing, const PageDraw& how) {
    const core::Doc::Id on_page = d.as<core::Page>(id) ? id : core::Doc::Id{};
    for (const auto& m : core::master_chain(d, id)) draw_frames(cr, d, m, flows, images, missing, how, on_page);
    draw_frames(cr, d, id, flows, images, missing, how, on_page);
}

PageKey page_key(const core::Doc& d, FlowCache& flows, const core::Doc::Id& id) {
    PageKey k;
    std::map<std::string, StoryFlow> ref_flows;   // s128: where references' targets are set
    const auto rec = [&](const std::string& rid) {
        if (const auto it = d.records().find(rid); it != d.records().end()) k.recs.emplace_back(rid, it->second);
    };
    auto owners = core::master_chain(d, id);
    owners.push_back(id);
    for (const auto& o : owners) {
        rec(o);
        for (const auto& fid : d.frames_on(o)) {
            if (o != id && !core::master_item_on(d, fid, id)) continue;   // s025: not shown here
            rec(fid);
            const auto& f = *d.as<core::Frame>(fid);
            if (!f.asset.empty()) rec(f.asset);
            if (f.kind != core::FrameKind::Text || f.story.empty()) continue;
            const auto* st = d.as<core::Story>(f.story);
            if (!st) continue;
            const auto& rs = flows.resolved(d, f.story, d.as<core::Master>(o) && d.as<core::Page>(id) ? id : std::string{});
            if (rs.any) st = &rs.text;   // s023: as drawn, numbers filled in
            for (const auto& ft : (rs.any ? rs.flow : flows.get(d, f.story)).frames) {
                if (ft.frame != fid) continue;
                for (const auto& l : ft.lines) {
                    const auto& p = st->paras[static_cast<std::size_t>(l.para)];
                    std::string line = fid + "|" + p.style + "|" + std::to_string(l.col) + "|" + std::to_string(l.x) + "|" +
                                       std::to_string(l.y) + "|" + std::to_string(l.w) + "|";
                    for (const auto& r : p.runs)
                        if (r.end > l.start && r.start < l.end)
                            line += std::to_string(r.start - l.start) + "-" + std::to_string(r.end - l.start) + r.style + ",";
                    line += "|" + p.text.substr(static_cast<std::size_t>(l.start), static_cast<std::size_t>(l.end - l.start));
                    // s109: a footnote mark's number (it moves when one is added before).
                    for (std::size_t i = p.text.find(core::kMarkNote, static_cast<std::size_t>(l.start));
                         i != std::string::npos && static_cast<int>(i) < l.end; i = p.text.find(core::kMarkNote, i + 3))
                        line += "#" + std::to_string(core::note_number(d, *st, l.para, core::notes_in(p.text, 0, i)));
                    // s128: a cross-reference's value (where its target is set) -- it moves with other pages.
                    for (std::size_t i = p.text.find(core::kMarkRef, static_cast<std::size_t>(l.start));
                         i != std::string::npos && static_cast<int>(i) < l.end; i = p.text.find(core::kMarkRef, i + 3)) {
                        const int idx = core::marks_in(p.text, core::kMarkRef, 0, i);
                        if (idx < static_cast<int>(p.refs.size())) line += "@" + ref_value(d, p.refs[static_cast<std::size_t>(idx)], ref_flows);
                    }
                    k.lines.push_back(std::move(line));
                }
                for (const auto& np : ft.notes) {   // s109: the notes at the foot
                    if (np.para < 0 || np.para >= static_cast<int>(st->paras.size())) continue;
                    const auto& p = st->paras[static_cast<std::size_t>(np.para)];
                    std::string line = fid + "|note|" + std::to_string(np.number) + "|" + std::to_string(np.col) + "|" +
                                       std::to_string(np.y) + "|" + p.style + "|" + std::to_string(np.from) + "-" +
                                       std::to_string(np.to) + (np.cont ? "c" : "") + (np.end ? "e" : "") + "|";   // s117: its piece; s120 an endnote
                    if (np.k >= 0 && np.k < static_cast<int>(p.notes.size())) {
                        const auto& n = p.notes[static_cast<std::size_t>(np.k)];
                        for (const auto& r : n.runs) line += std::to_string(r.start) + "-" + std::to_string(r.end) + r.style + ",";
                        line += "|" + n.text;
                    }
                    if (np.k == -1) line += "|" + d.settings().note_title;   // s120: the endnotes' title
                    {   // s126: the note settings that change how a note looks (not where it goes)
                        const auto& ds = d.settings();
                        line += "|" + std::to_string(static_cast<int>(ds.note_style)) + std::to_string(static_cast<int>(ds.note_num)) + "|" +
                                std::to_string(ds.note_rule) + "/" + std::to_string(ds.note_rule_w) + "|" + ds.note_cont;
                    }
                    if (np.k == -2) line += "|" + p.text;                     // s125: a chapter's head (its paragraph's words)
                    k.lines.push_back(std::move(line));
                }
            }
        }
    }
    for (const auto& sid : d.ids_of<core::Style>()) rec(sid);
    for (const auto& sid : d.ids_of<core::Swatch>()) rec(sid);
    return k;
}

std::vector<std::string> bookmark_lists(const core::Doc& d) {   // s082
    std::vector<std::string> out;
    for (const auto& lid : d.ids_of<core::GenList>())
        if (!d.as<core::GenList>(lid)->from_styles.empty()) out.push_back(lid);
    std::stable_sort(out.begin(), out.end(), [&d](const std::string& a, const std::string& b) {
        return d.as<core::GenList>(a)->pos < d.as<core::GenList>(b)->pos;
    });
    return out;
}

std::vector<std::string> bookmark_lists_used(const core::Doc& d, const std::vector<std::string>& picked) {   // s082
    std::vector<std::string> use;
    for (const auto& lid : bookmark_lists(d))
        if (std::find(picked.begin(), picked.end(), lid) != picked.end()) use.push_back(lid);
    if (use.empty())   // s054's default: the first Contents list
        for (const auto& lid : d.ids_of<core::GenList>())
            if (const auto* l = d.as<core::GenList>(lid); l->kind == core::ListKind::Contents && !l->from_styles.empty()) {
                use.push_back(lid);
                break;
            }
    return use;
}

namespace {
// One list's entries, where they land, in page-then-y order.
std::vector<Bookmark> list_marks(const core::Doc& d, FlowCache& flows, const core::GenList& toc,
                                 const std::vector<std::string>& only) {
    const auto pages = d.pages();
    // s082: on a page, a story's entries in story order (columns read down
    // then across); stories by where their first entry on the page lands.
    struct Keyed { std::size_t page; double y; std::size_t story; int para; double top = 0; Bookmark b; };
    std::vector<Keyed> all;
    std::size_t sn = 0;
    for (const auto& sid : d.ids_of<core::Story>()) {
        ++sn;
        const auto& st = *d.as<core::Story>(sid);
        const auto& flow = flows.get(d, sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            const auto lv = std::find(toc.from_styles.begin(), toc.from_styles.end(), para.style);
            if (lv == toc.from_styles.end()) continue;
            // s131: without its marks (an anchor a reference points at, a note's
            // number) -- a heading anchored no longer lost its contents link.
            std::string title;
            for (std::size_t i = 0; i < para.text.size();) {
                if (static_cast<unsigned char>(para.text[i]) == 0xEE && i + 2 < para.text.size() &&
                    static_cast<unsigned char>(para.text[i + 1]) == 0x80) { i += 3; continue; }
                title += para.text[i] == '\t' ? ' ' : para.text[i];
                ++i;
            }
            if (title.find_first_not_of(' ') == std::string::npos) continue;
            const auto c = caret_line(d, sid, flow, {p, 0});   // s105: (was caret_at: a frame laid out a heading)
            if (!c.ok) continue;
            const auto* f = d.as<core::Frame>(c.frame);
            if (!f) continue;
            const auto at = std::find(pages.begin(), pages.end(), f->owner);
            if (at == pages.end()) continue;   // on a master: not a place in the book
            if (!only.empty() && std::find(only.begin(), only.end(), f->owner) == only.end()) continue;
            const double y = f->y + c.y;
            all.push_back({static_cast<std::size_t>(at - pages.begin()), y, sn, p, y,
                           {title, static_cast<int>(lv - toc.from_styles.begin()) + 1, f->owner, y}});
        }
    }
    for (auto& k : all)
        for (const auto& o : all)
            if (o.page == k.page && o.story == k.story) k.top = std::min(k.top, o.y);
    std::stable_sort(all.begin(), all.end(), [](const Keyed& a, const Keyed& b) {
        if (a.page != b.page) return a.page < b.page;
        if (a.story != b.story) return a.top != b.top ? a.top < b.top : a.story < b.story;
        return a.para < b.para;
    });
    std::vector<Bookmark> out;
    for (auto& k : all) out.push_back(std::move(k.b));
    return out;
}
}  // namespace

std::vector<Bookmark> bookmarks(const core::Doc& d, FlowCache& flows, const std::vector<std::string>& lists,
                                const std::vector<std::string>& only) {
    const auto use = bookmark_lists_used(d, lists);
    std::vector<Bookmark> out;
    for (std::size_t n = 0; n < use.size(); ++n) {
        const auto& l = *d.as<core::GenList>(use[n]);
        auto marks = list_marks(d, flows, l, only);
        if (marks.empty()) continue;
        if (out.empty()) { out = std::move(marks); continue; }   // the first with entries: the top level
        out.push_back({l.name.empty() ? std::string("List") : l.name, 1, marks.front().page, marks.front().y});
        for (auto& m : marks) { ++m.level; out.push_back(std::move(m)); }
    }
    return out;
}

std::vector<Bookmark> bookmarks(const core::Doc& d, FlowCache& flows) { return bookmarks(d, flows, {}); }

namespace {
std::string trimmed(std::string t) {
    for (auto& c : t) if (c == '\t') c = ' ';
    const auto a = t.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    return t.substr(a, t.find_last_not_of(' ') - a + 1);
}
}  // namespace

std::vector<PdfLink> note_links(const core::Doc& d, FlowCache& flows) {   // s123
    std::vector<PdfLink> out;
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        bool any = false;
        for (const auto& p : st.paras) any = any || !p.notes.empty();
        if (!any) continue;
        const auto& flow = flows.get(d, sid);
        for (const auto& ft : flow.frames) {
            const auto* nf = d.as<core::Frame>(ft.frame);
            if (!nf || nf->rot != 0) continue;
            for (const auto& np : ft.notes) {
                if (np.cont || np.k < 0) continue;   // the note's first piece (where its number is)
                const int m = core::doc::note_mark_byte(st, np.para, np.k);
                if (m < 0) continue;
                const auto c = caret_at(d, sid, flow, {np.para, m});
                if (!c.ok) continue;
                const auto* mf = d.as<core::Frame>(c.frame);
                if (!mf || mf->rot != 0) continue;
                const auto rs = selection_rects(d, sid, flow, c.frame, {np.para, m}, {np.para, m + 3});
                if (rs.empty()) continue;
                const auto& r = rs.front();
                // The mark: a little room round a small number, so it can be hit.
                const double pad = 1.5;
                const core::Rect mark{mf->x + r.x - pad, mf->y + r.y - pad, std::max(4.0, r.w) + 2 * pad, r.h + 2 * pad};
                const double note_top = nf->y + np.y;
                out.push_back({mf->owner, mark, nf->owner, std::max(0.0, note_top - 4)});
                // Back: the note's number (its first line's start).
                const double back_w = std::min(np.w, 14.0), back_h = std::min(np.h, 12.0);
                out.push_back({nf->owner, {nf->x + np.x - pad, note_top - pad, back_w + 2 * pad, back_h + 2 * pad}, mf->owner,
                               std::max(0.0, mf->y + r.y - 24)});
            }
        }
    }
    return out;
}

namespace {
// s134: a string's simple case fold (core::fold_char over its code points), for sorting.
std::string folded(const std::string& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        const auto c = static_cast<unsigned char>(t[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + static_cast<std::size_t>(n) > t.size()) { out += t.substr(i); break; }
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int q = 1; q < n; ++q) cp = (cp << 6) | (static_cast<unsigned char>(t[i + static_cast<std::size_t>(q)]) & 0x3F);
        cp = core::doc::fold_char(cp);
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        i += static_cast<std::size_t>(n);
    }
    return out;
}
}  // namespace

// s136: how an index sorts a word -- folded, with Latin accents set aside
// (\u00e9 with e, \u00f8 with o, \u00df with s): "\u00c9clair" between "eagle" and "echo".
std::string collated(const std::string& t) {
    static constexpr const char* base = "aaaaaaaceeeeiiiidnooooo0ouuuuytsaaaaaaaceeeeiiiidnooooo0ouuuuytyaaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii00jjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";   // U+00C0..U+017F, '0' none
    std::string f = folded(t), out;
    for (std::size_t i = 0; i < f.size();) {
        const auto c = static_cast<unsigned char>(f[i]);
        if ((c >> 5) == 6 && i + 1 < f.size()) {
            const char32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(f[i + 1]) & 0x3F);
            if (cp >= 0xC0 && cp < 0x180 && base[cp - 0xC0] != '0') { out += base[cp - 0xC0]; i += 2; continue; }
        }
        out += f[i++];
    }
    return out;
}

std::vector<IndexEntry> index_entries(const core::Doc& d, FlowCache& flows) {   // s134
    struct Key {
        std::string ct, cs, ft, fs;   // s136: collated first, then folded (a tie: \u00e9clair after eclair)
        bool operator<(const Key& o) const { return std::tie(ct, cs, ft, fs) < std::tie(o.ct, o.cs, o.ft, o.fs); }
    };
    const auto key = [](const std::string& term, const std::string& sub) {
        return Key{collated(term), collated(sub), folded(term), folded(sub)};
    };
    std::map<Key, IndexEntry> by;
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        if (std::none_of(st.paras.begin(), st.paras.end(), [](const core::Para& p) { return !p.index.empty(); })) continue;
        const auto& flow = flows.get(d, sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            int k = 0;
            for (std::size_t i = para.text.find(core::kMarkIndex); i != std::string::npos; i = para.text.find(core::kMarkIndex, i + 3), ++k) {
                if (k >= static_cast<int>(para.index.size())) break;
                const auto& m = para.index[static_cast<std::size_t>(k)];
                if (m.term.empty()) continue;
                std::string page;
                for (const auto& ft : flow.frames) {
                    for (const auto& l : ft.lines)
                        if (l.para == p && static_cast<int>(i) >= l.start && static_cast<int>(i) <= std::max(l.end, l.start + 1))
                            if (const auto* f = d.as<core::Frame>(ft.frame); f && d.as<core::Page>(f->owner)) { page = f->owner; break; }
                    if (!page.empty()) break;
                }
                // A sub-entry's term heads it (a line of its own, perhaps with no pages).
                if (!m.sub.empty()) {
                    auto& head = by[key(m.term, {})];
                    if (head.term.empty()) head.term = m.term;
                }
                auto& e = by[key(m.term, m.sub)];
                if (e.term.empty()) { e.term = m.term; e.sub = m.sub; }
                ++e.marks;
                if (!m.see.empty()) {   // s136: sends the reader on -- no page of its own
                    auto& to = m.also ? e.also : e.see;
                    if (std::find(to.begin(), to.end(), m.see) == to.end()) to.push_back(m.see);
                    continue;
                }
                if (!page.empty() && std::find(e.pages.begin(), e.pages.end(), page) == e.pages.end()) e.pages.push_back(page);
            }
        }
    }
    std::vector<IndexEntry> out;
    for (auto& [key, e] : by) {
        std::sort(e.pages.begin(), e.pages.end(), [&](const std::string& a, const std::string& b) { return d.page_index(a) < d.page_index(b); });
        for (const auto& pg : e.pages) e.folios.push_back(d.folio(pg));
        // s136: runs of three or more pages in a row as a range ("3\u20135"): the
        // pages next in the book and their numbers next too.
        const auto num = [](const std::string& f) {
            if (f.empty() || f.size() > 6 || !std::all_of(f.begin(), f.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
            return std::stoi(f);
        };
        for (std::size_t a = 0; a < e.pages.size();) {
            std::size_t b = a;
            while (b + 1 < e.pages.size() && d.page_index(e.pages[b + 1]) == d.page_index(e.pages[b]) + 1 && num(e.folios[b]) >= 0 &&
                   num(e.folios[b + 1]) == num(e.folios[b]) + 1)
                ++b;
            if (!e.pages_text.empty()) e.pages_text += ", ";
            if (b - a >= 2) e.pages_text += e.folios[a] + "\u2013" + e.folios[b];
            else for (std::size_t q = a; q <= b; ++q) e.pages_text += (q > a ? ", " : "") + e.folios[q];
            a = b + 1;
        }
        std::sort(e.see.begin(), e.see.end(), [](const std::string& x, const std::string& y) { return collated(x) < collated(y); });
        std::sort(e.also.begin(), e.also.end(), [](const std::string& x, const std::string& y) { return collated(x) < collated(y); });
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<PdfLink> xref_links(const core::Doc& d, FlowCache& flows) {   // s131
    std::vector<PdfLink> out;
    // Where an anchor points: its page and how far down; page "" none.
    struct Dest { std::string page; double y = 0; };
    std::map<std::string, Dest> dests;
    const auto dest_of = [&](const std::string& id) -> Dest {
        if (const auto it = dests.find(id); it != dests.end()) return it->second;
        Dest dst;
        const auto at = core::doc::find_anchor(d, id);
        if (!at.story.empty()) {
            const auto& st = *d.as<core::Story>(at.story);
            const auto& flow = flows.get(d, at.story);
            const auto& text = st.paras[static_cast<std::size_t>(at.at.para)].text;
            if (core::note_at(text, static_cast<std::size_t>(at.at.byte) + 3)) {   // a note: where it is set
                const int k = core::notes_in(text, 0, static_cast<std::size_t>(at.at.byte) + 3);
                for (const auto& ft : flow.frames)
                    for (const auto& np : ft.notes)
                        if (np.para == at.at.para && np.k == k && !np.cont && dst.page.empty())
                            if (const auto* nf = d.as<core::Frame>(ft.frame); nf && d.as<core::Page>(nf->owner))
                                dst = {nf->owner, std::max(0.0, nf->y + np.y - 4)};
            }
            if (dst.page.empty()) {
                const auto c = caret_at(d, at.story, flow, at.at);
                if (const auto* f = c.ok ? d.as<core::Frame>(c.frame) : nullptr; f && d.as<core::Page>(f->owner))
                    dst = {f->owner, std::max(0.0, f->y + c.y - 4)};
            }
        }
        dests.emplace(id, dst);
        return dst;
    };
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        if (std::none_of(st.paras.begin(), st.paras.end(), [](const core::Para& p) { return !p.refs.empty(); })) continue;
        const auto& rs = flows.resolved(d, sid, {});
        if (!rs.any) continue;
        for (const auto& span : rs.refs) {
            const auto& refs = st.paras[static_cast<std::size_t>(span.para)].refs;
            if (span.k >= static_cast<int>(refs.size()) || span.to <= span.from) continue;
            const auto dst = dest_of(refs[static_cast<std::size_t>(span.k)].target);
            if (dst.page.empty()) continue;
            for (const auto& ft : rs.flow.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (!f || f->rot != 0 || !d.as<core::Page>(f->owner)) continue;
                const bool here = std::any_of(ft.lines.begin(), ft.lines.end(), [&](const Line& l) {
                    return l.para == span.para && l.start < span.to && l.end > span.from;
                });
                if (!here) continue;
                for (const auto& r : selection_rects(d, sid, rs.flow, ft.frame, {span.para, span.from}, {span.para, span.to}, &rs.text))
                    out.push_back({f->owner, {f->x + r.x, f->y + r.y, r.w, r.h}, dst.page, dst.y});
            }
        }
    }
    return out;
}

std::vector<PdfLink> contents_links(const core::Doc& d, FlowCache& flows) {   // s083
    std::vector<PdfLink> out;
    for (const auto& lid : bookmark_lists(d)) {
        const auto& l = *d.as<core::GenList>(lid);
        if (l.kind != core::ListKind::Contents || l.placed_on.empty() || !d.as<core::Page>(l.placed_on)) continue;
        auto heads = list_marks(d, flows, l, {});
        std::vector<bool> used(heads.size(), false);
        for (const auto& sid : d.ids_of<core::Story>()) {
            const auto& st = *d.as<core::Story>(sid);
            const auto& flow = flows.get(d, sid);
            for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                const auto& para = st.paras[static_cast<std::size_t>(p)];
                if (std::find(l.from_styles.begin(), l.from_styles.end(), para.style) != l.from_styles.end()) continue;
                const auto title = trimmed(para.text.substr(0, para.text.find('\t')));
                if (title.empty()) continue;
                // s101: only paragraphs that start on the contents page (looked
                // up in the flow first -- caret_at for every paragraph of a long
                // book took minutes).
                bool on_page = false;
                for (const auto& ft : flow.frames) {
                    const auto* fr = d.as<core::Frame>(ft.frame);
                    if (!fr || fr->owner != l.placed_on) continue;
                    for (const auto& ln : ft.lines) on_page = on_page || (ln.para == p && ln.start == 0);
                }
                if (!on_page) continue;
                const auto c = caret_at(d, sid, flow, {p, 0});
                if (!c.ok) continue;
                const auto* f = d.as<core::Frame>(c.frame);
                if (!f || f->owner != l.placed_on || f->rot != 0) continue;
                std::size_t k = 0;
                while (k < heads.size() && (used[k] || trimmed(heads[k].title) != title)) ++k;
                if (k == heads.size()) continue;
                const auto rs = selection_rects(d, sid, flow, c.frame, {p, 0}, {p, static_cast<int>(para.text.size())});
                if (rs.empty()) continue;
                double x0 = rs.front().x, y0 = rs.front().y, x1 = rs.front().r(), y1 = rs.front().b();
                for (const auto& r : rs) {
                    x0 = std::min(x0, r.x); y0 = std::min(y0, r.y);
                    x1 = std::max(x1, r.r()); y1 = std::max(y1, r.b());
                }
                used[k] = true;
                out.push_back({f->owner, {f->x + x0, f->y + y0, x1 - x0, y1 - y0}, heads[k].page, heads[k].y});
            }
        }
    }
    return out;
}

std::vector<Sheet> pdf_sheets(const core::Doc& d, const std::vector<std::string>& pages, bool spreads) {   // s084
    std::vector<std::vector<std::string>> groups;
    if (spreads) {
        for (const auto& sp : core::spreads(d)) {
            std::vector<std::string> g;
            for (const auto& p : sp) if (std::find(pages.begin(), pages.end(), p) != pages.end()) g.push_back(p);
            if (!g.empty()) groups.push_back(std::move(g));
        }
    } else {
        for (const auto& p : pages) groups.push_back({p});
    }
    std::vector<Sheet> out;
    for (auto& g : groups) {
        Sheet sh;
        for (const auto& p : g) {
            const auto* pg = d.as<core::Page>(p);
            if (!pg) continue;
            sh.pages.push_back(p);
            sh.x.push_back(sh.w);
            sh.w += pg->w;
            sh.h = std::max(sh.h, pg->h);
            sh.label += (sh.label.empty() ? "" : "\u2013") + d.folio(p);
        }
        if (!sh.pages.empty()) out.push_back(std::move(sh));
    }
    return out;
}

SheetAt sheet_of(const std::vector<Sheet>& sheets, const std::string& page) {   // s084
    for (std::size_t s = 0; s < sheets.size(); ++s)
        for (std::size_t k = 0; k < sheets[s].pages.size(); ++k)
            if (sheets[s].pages[k] == page) return {static_cast<int>(s), sheets[s].x[k]};
    return {};
}

std::vector<core::doc::ListEntry> list_entries(const core::Doc& d, FlowCache& flows, const std::string& list) {   // s087
    std::vector<core::doc::ListEntry> out;
    const auto* l = d.as<core::GenList>(list);
    if (l && l->kind == core::ListKind::Index) {   // s135: the index marks' entries, a letter heading each initial
        std::string group;
        const auto joined = [](const std::vector<std::string>& v) {
            std::string t;
            for (const auto& x : v) t += (t.empty() ? "" : "; ") + x;
            return t;
        };
        for (const auto& e : index_entries(d, flows)) {
            // s136: its pages (runs joined), then where else to look.
            std::string folios = e.pages_text;
            std::string words = e.sub.empty() ? e.term : e.sub;
            if (!e.see.empty()) {
                if (folios.empty()) words += ". See " + joined(e.see);   // sent on, nothing here
                else folios += ". See also " + joined(e.see);
            }
            if (!e.also.empty()) {
                if (folios.empty()) words += ". See also " + joined(e.also);
                else folios += ". See also " + joined(e.also);
            }
            if (e.sub.empty()) {
                // The initial: its first character, folded; shown upper case (ASCII) or as written.
                const auto c = static_cast<unsigned char>(e.term[0]);
                const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
                const auto first = e.term.substr(0, std::min(n, e.term.size()));
                const auto key = collated(first);   // s136: \u00c9 with E
                if (key != group) {
                    group = key;
                    std::string shown = first;
                    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0])))
                        shown = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key[0]))));
                    out.push_back({shown, "", 0});
                }
            }
            out.push_back({words, folios, e.sub.empty() ? 1 : 2});
        }
        return out;
    }
    if (!l || l->from_styles.empty()) return out;
    for (const auto& m : list_marks(d, flows, *l, {})) out.push_back({trimmed(m.title), d.folio(m.page), m.level});
    return out;
}

bool build_list(core::DocJournal& j, core::Doc& d, const std::string& list) {   // s087
    if (!d.as<core::GenList>(list)) return false;
    std::vector<core::doc::ListEntry> entries;
    {
        FlowCache f0;
        entries = list_entries(d, f0, list);
    }
    for (int round = 0; round < 4; ++round) {   // settle: write on a copy, look again
        core::Doc scratch = d;
        core::DocJournal sj{scratch};
        scratch.set_door([&sj] { return sj.open(); });
        core::doc::build_list(sj, scratch, list, entries);
        FlowCache fs;
        auto again = list_entries(scratch, fs, list);
        if (again == entries) break;
        entries = std::move(again);
    }
    return core::doc::build_list(j, d, list, entries);
}

std::vector<StaleList> stale_lists(const core::Doc& d, FlowCache& flows) {   // s089
    std::vector<StaleList> out;
    for (const auto& lid : bookmark_lists(d)) {
        const auto& l = *d.as<core::GenList>(lid);
        if (l.kind != core::ListKind::Contents) continue;
        const auto* st = l.story.empty() ? nullptr : d.as<core::Story>(l.story);
        if (!st && l.placed_on.empty()) continue;
        if (!st || l.built_entries < 0) { out.push_back({lid, "Not built yet"}); continue; }
        auto en = list_entries(d, flows, lid);
        if (!l.numbers) for (auto& e : en) e.folio.clear();   // s090: words alone
        // What's written: the entries after a title (text, no tab).
        std::size_t k = static_cast<std::size_t>(core::doc::list_title_paras(*st));   // s090
        std::vector<core::doc::ListEntry> had;
        for (; k < st->paras.size(); ++k) {
            const auto& t = st->paras[k].text;
            const auto tab = l.numbers ? t.rfind('\t') : std::string::npos;
            had.push_back({tab == std::string::npos ? t : t.substr(0, tab), tab == std::string::npos ? "" : t.substr(tab + 1)});
        }
        std::string why;
        const std::size_t n = std::min(en.size(), had.size());
        for (std::size_t i = 0; i < n && why.empty(); ++i) {
            if (en[i].text == had[i].text && had[i].folio.empty() && !en[i].folio.empty())
                why = "Page numbers not written";   // s090: switched back on
            else if (en[i].text == had[i].text && en[i].folio != had[i].folio)
                why = en[i].text + ": page " + had[i].folio + " \u2192 " + en[i].folio;
            else if (en[i].text != had[i].text)
                why = std::find_if(had.begin(), had.end(), [&](const auto& h) { return h.text == en[i].text; }) == had.end()
                          ? "\u201c" + en[i].text + "\u201d not listed yet"
                          : "\u201c" + had[i].text + "\u201d listed, no such heading";
        }
        if (why.empty() && en.size() > had.size()) why = "\u201c" + en[n].text + "\u201d not listed yet";
        if (why.empty() && had.size() > en.size()) why = "\u201c" + had[n].text + "\u201d listed, no such heading";
        if (why.empty()) {   // s096: a level whose style was picked (s090) -- written in it?
            const std::size_t t0 = static_cast<std::size_t>(core::doc::list_title_paras(*st));
            for (std::size_t i = 0; i < en.size() && why.empty(); ++i) {
                const auto lv = static_cast<std::size_t>(std::max(1, en[i].level));
                if (lv > l.entry_styles.size() || l.entry_styles[lv - 1].empty()) continue;
                const auto* want = d.as<core::Style>(l.entry_styles[lv - 1]);
                if (want && t0 + i < st->paras.size() && st->paras[t0 + i].style != l.entry_styles[lv - 1])
                    why = "Entries not in " + want->name;
            }
        }
        if (!why.empty()) out.push_back({lid, why});
    }
    // s135: an index -- its written paragraphs against what the marks make now.
    for (const auto& lid : d.ids_of<core::GenList>()) {
        const auto& l = *d.as<core::GenList>(lid);
        if (l.kind != core::ListKind::Index) continue;
        const auto* st = l.story.empty() ? nullptr : d.as<core::Story>(l.story);
        if (!st && l.placed_on.empty()) continue;
        if (!st || l.built_entries < 0) { out.push_back({lid, "Not built yet"}); continue; }
        const auto en = list_entries(d, flows, lid);
        std::vector<std::string> want, had;
        for (const auto& e : en) want.push_back(e.level == 0 || !l.numbers || e.folio.empty() ? e.text : e.text + ", " + e.folio);
        for (auto k = static_cast<std::size_t>(core::doc::list_title_paras(*st, l.kind)); k < st->paras.size(); ++k) had.push_back(st->paras[k].text);
        if (had.size() == 1 && had[0].empty()) had.clear();   // built with no entries
        std::string why;
        const auto name = [](const core::doc::ListEntry& e) { return "\u201c" + e.text + "\u201d"; };
        for (std::size_t i = 0; i < std::min(want.size(), had.size()) && why.empty(); ++i) {
            if (want[i] == had[i]) continue;
            const auto& e = en[i];
            if (e.level > 0 && had[i].rfind(e.text, 0) == 0) why = name(e) + ": pages now " + (e.folio.empty() ? "none" : e.folio);
            else why = name(e) + " not listed yet";
        }
        if (why.empty() && want.size() > had.size()) why = name(en[had.size()]) + " not listed yet";
        if (why.empty() && had.size() > want.size()) why = "\u201c" + had[want.size()] + "\u201d listed, no such entry";
        if (!why.empty()) out.push_back({lid, why});
    }
    return out;
}

ExportSummary export_summary(const core::Doc& d, FlowCache& flows, const PdfOptions& opt, bool print,
                             const std::string& profile_name) {   // s086
    ExportSummary s;
    std::vector<std::string> pages;
    for (const auto& p : d.pages())
        if (opt.only.empty() || std::find(opt.only.begin(), opt.only.end(), p) != opt.only.end()) pages.push_back(p);
    const auto sheets = pdf_sheets(d, pages, opt.spreads);
    s.pages = static_cast<int>(pages.size());
    s.sheets = static_cast<int>(sheets.size());
    const auto units = d.settings().units;
    s.size = std::to_string(s.pages) + (s.pages == 1 ? " page" : " pages");
    if (s.sheets != s.pages) s.size += " \u2192 " + std::to_string(s.sheets) + (s.sheets == 1 ? " sheet" : " sheets");
    if (!pages.empty()) {
        const auto* pg = d.as<core::Page>(pages.front());
        s.size += " \u00b7 " + core::size_text(pg->w, pg->h, 0, units);
        if (opt.bleed && d.settings().bleed > 0) s.size += " + " + core::length_text(d.settings().bleed, units) + " bleed";
    }
    const bool x = print && opt.standard != plate_standard::None;
    if (!print) {
        s.colour = "RGB, for screen";
    } else if (profile_name.empty()) {
        s.colour = "Device CMYK \u2014 RGB left as it is";
    } else {
        const bool conv = opt.convert || opt.standard == plate_standard::X1a;
        s.colour = std::string(x ? (opt.standard == plate_standard::X1a ? "PDF/X-1a \u00b7 " : "PDF/X-4 \u00b7 ") : "") +
                   "CMYK for " + profile_name + " \u2014 " +
                   (conv ? std::string("RGB converted, ") + (opt.perceptual ? "perceptual" : "relative colorimetric")
                         : std::string("RGB kept, tagged"));
    }
    if (opt.bookmarks && !pages.empty()) s.bookmarks = static_cast<int>(bookmarks(d, flows, opt.mark_lists, pages).size());
    if (!x) {
        for (const auto& l : contents_links(d, flows))
            if (sheet_of(sheets, l.page).sheet >= 0 && sheet_of(sheets, l.to).sheet >= 0) ++s.links;
        for (const auto& l : note_links(d, flows))   // s123
            if (sheet_of(sheets, l.page).sheet >= 0 && sheet_of(sheets, l.to).sheet >= 0) ++s.note_links;
        for (const auto& l : xref_links(d, flows))   // s131
            if (sheet_of(sheets, l.page).sheet >= 0 && sheet_of(sheets, l.to).sheet >= 0) ++s.xref_links;
    }
    const auto n = [](int k, const char* one, const char* many) {
        return (k == 0 ? std::string("No") : std::to_string(k)) + " " + (k == 1 ? one : many);
    };
    s.nav = n(s.bookmarks, "bookmark", "bookmarks") + " \u00b7 " +
            (x ? std::string("contents links left out (PDF/X)") : n(s.links, "contents link", "contents links"));
    if (!x && s.note_links > 0) s.nav += " \u00b7 " + n(s.note_links, "note link", "note links");   // s123
    if (!x && s.xref_links > 0) s.nav += " \u00b7 " + n(s.xref_links, "cross-reference link", "cross-reference links");   // s131
    return s;
}

namespace {
std::vector<std::string> pages_to_export(const core::Doc& d, const PdfOptions& opt) {
    if (opt.only.empty()) return d.pages();
    std::vector<std::string> out;
    for (const auto& p : d.pages())
        if (std::find(opt.only.begin(), opt.only.end(), p) != opt.only.end()) out.push_back(p);
    return out;
}
}  // namespace

bool export_pdf(const core::Doc& d, const std::string& path, const PdfOptions& opt, const ImageSource& images,
                PdfReport* report, std::string* err) {
    const auto pages = pages_to_export(d, opt);
    if (pages.empty()) { if (err) *err = "The document has no pages."; return false; }
    const double bleed = opt.bleed ? std::max(0.0, d.settings().bleed) : 0;
    const double off = bleed + (opt.marks ? kSlug : 0);

    const auto sheets = pdf_sheets(d, pages, opt.spreads);   // s084
    cairo_surface_t* surf = cairo_pdf_surface_create(path.c_str(), sheets.front().w + 2 * off, sheets.front().h + 2 * off);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (err) *err = std::string("Can't write the PDF: ") + cairo_status_to_string(cairo_surface_status(surf));
        cairo_surface_destroy(surf);
        return false;
    }
    // PDF 1.4: the widest-read version, and the base PDF/X builds on (it also
    // keeps the page dictionaries out of compressed object streams).
    cairo_pdf_surface_restrict_to_version(surf, CAIRO_PDF_VERSION_1_4);
    const std::string title = opt.title.empty() ? d.settings().title : opt.title;
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_TITLE, title.c_str());
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATOR, "Composix");

    cairo_t* cr = cairo_create(surf);
    FlowCache flows;
    const std::size_t npages = pages.size();
    auto report_layout = [&](int done, int total) {   // s102
        if (opt.progress && total > 0)
            opt.progress(0.3 * done / total, "Laying out page " + std::to_string(std::max<std::size_t>(1, npages * done / total)) +
                                                 " of " + std::to_string(npages) + "\u2026");
    };
    if (opt.progress) flow_all(d, flows, report_layout);
    auto links = contents_links(d, flows);   // s083
    for (auto& l : note_links(d, flows)) links.push_back(std::move(l));   // s123: and the notes'
    for (auto& l : xref_links(d, flows)) links.push_back(std::move(l));   // s131: and the cross-references'
    std::set<std::string> missing;
    int n = 0;
    for (const auto& sh : sheets) {
        cairo_pdf_surface_set_size(surf, sh.w + 2 * off, sh.h + 2 * off);   // before anything is drawn on it
        cairo_pdf_surface_set_page_label(surf, sh.label.c_str());
        for (std::size_t k = 0; k < sh.pages.size(); ++k) {
            const auto& pid = sh.pages[k];
            const auto& pg = *d.as<core::Page>(pid);
            // s084: the bleed on the spread's outer edges only.
            const double x0 = k == 0 ? -bleed : 0, x1 = k + 1 == sh.pages.size() ? pg.w + bleed : pg.w;
            cairo_save(cr);
            cairo_translate(cr, off + sh.x[k], off);
            cairo_rectangle(cr, x0, -bleed, x1 - x0, pg.h + 2 * bleed);   // nothing past the bleed
            cairo_clip(cr);
            draw_page(cr, d, pid, flows, images, &missing);
            cairo_restore(cr);
            ++n;
            if (opt.progress)   // s102
                opt.progress(0.3 + 0.67 * n / npages, "Writing page " + std::to_string(n) + " of " + std::to_string(npages) + "\u2026");
            for (const auto& l : links) {   // s083: the contents page's entries, clickable
                if (l.page != pid) continue;
                const auto to = sheet_of(sheets, l.to);
                if (to.sheet < 0) continue;
                char attr[160];
                std::snprintf(attr, sizeof attr, "rect=[%g %g %g %g] page=%d pos=[%g %g]", off + sh.x[k] + l.box.x, off + l.box.y,
                              l.box.w, l.box.h, to.sheet + 1, off + to.x, off + l.to_y);
                cairo_tag_begin(cr, CAIRO_TAG_LINK, attr);
                cairo_tag_end(cr, CAIRO_TAG_LINK);
            }
        }
        if (opt.marks) {
            cairo_save(cr);
            cairo_translate(cr, off, off);
            crop_marks(cr, sh.w, sh.h, bleed);
            cairo_restore(cr);
        }
        cairo_show_page(cr);
    }
    // s054: bookmarks, each nested under the last one a level up.
    if (opt.bookmarks) {
        std::vector<int> parent_at{CAIRO_PDF_OUTLINE_ROOT};   // [level-1] -> the id to nest under
        for (const auto& b : bookmarks(d, flows, opt.mark_lists, pages)) {
            const auto at = sheet_of(sheets, b.page);   // s084: its sheet
            if (at.sheet < 0) continue;
            const int lv = std::clamp(b.level, 1, static_cast<int>(parent_at.size()));
            parent_at.resize(static_cast<std::size_t>(lv));
            char dest[96];
            std::snprintf(dest, sizeof dest, "page=%d pos=[%g %g]", at.sheet + 1, off + at.x, off + b.y);
            parent_at.push_back(cairo_pdf_surface_add_outline(surf, parent_at[static_cast<std::size_t>(lv - 1)], b.title.c_str(), dest,
                                                              CAIRO_PDF_OUTLINE_FLAG_OPEN));
        }
    }
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    const auto status = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (status != CAIRO_STATUS_SUCCESS) {
        if (err) *err = std::string("Writing the PDF failed: ") + cairo_status_to_string(status);
        return false;
    }

    if (report) preflight(d, std::vector<std::string>(missing.begin(), missing.end()), n, report);
    return true;
}

// s024: the same checks as the Preflight list (errors and warnings), plus
// the pictures this export couldn't read.
void preflight(const core::Doc& d, const std::vector<std::string>& missing, int pages, PdfReport* report) {
    if (!report) return;
    report->pages = pages;
    report->warnings.clear();
    FlowCache flows;
    for (const auto& i : preflight_issues(d, flows, {}))
        if (i.level != Issue::Level::Info) report->warnings.push_back(i.text);
    for (const auto& m : missing) report->warnings.push_back("Missing image: " + m);
}

}  // namespace composix::layout
