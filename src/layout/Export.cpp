#include "layout/Export.hpp"
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"

#include <cairo-pdf.h>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <map>
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
            std::string title = para.text;
            for (auto& c : title) if (c == '\t') c = ' ';
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
    if (!x)
        for (const auto& l : contents_links(d, flows))
            if (sheet_of(sheets, l.page).sheet >= 0 && sheet_of(sheets, l.to).sheet >= 0) ++s.links;
    const auto n = [](int k, const char* one, const char* many) {
        return (k == 0 ? std::string("No") : std::to_string(k)) + " " + (k == 1 ? one : many);
    };
    s.nav = n(s.bookmarks, "bookmark", "bookmarks") + " \u00b7 " +
            (x ? std::string("contents links left out (PDF/X)") : n(s.links, "contents link", "contents links"));
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
    const auto links = contents_links(d, flows);   // s083
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
