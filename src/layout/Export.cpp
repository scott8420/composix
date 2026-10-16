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
            const auto c = core::tint_rgb(s->preview, tint);
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

std::vector<Bookmark> bookmarks(const core::Doc& d, FlowCache& flows) {
    std::vector<Bookmark> out;
    const core::GenList* toc = nullptr;
    for (const auto& lid : d.ids_of<core::GenList>())
        if (const auto* l = d.as<core::GenList>(lid); l->kind == core::ListKind::Contents && !l->from_styles.empty()) { toc = l; break; }
    if (!toc) return out;
    const auto pages = d.pages();
    struct Keyed { std::size_t page; double y; Bookmark b; };
    std::vector<Keyed> all;
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        const auto& flow = flows.get(d, sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            const auto lv = std::find(toc->from_styles.begin(), toc->from_styles.end(), para.style);
            if (lv == toc->from_styles.end()) continue;
            std::string title = para.text;
            for (auto& c : title) if (c == '\t') c = ' ';
            if (title.find_first_not_of(' ') == std::string::npos) continue;
            const auto c = caret_at(d, sid, flow, {p, 0});
            if (!c.ok) continue;
            const auto* f = d.as<core::Frame>(c.frame);
            if (!f) continue;
            const auto at = std::find(pages.begin(), pages.end(), f->owner);
            if (at == pages.end()) continue;   // on a master: not a place in the book
            const double y = f->y + c.y;
            all.push_back({static_cast<std::size_t>(at - pages.begin()), y,
                           {title, static_cast<int>(lv - toc->from_styles.begin()) + 1, f->owner, y}});
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Keyed& a, const Keyed& b) { return a.page != b.page ? a.page < b.page : a.y < b.y; });
    for (auto& k : all) out.push_back(std::move(k.b));
    return out;
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

    const auto* first = d.as<core::Page>(pages.front());
    cairo_surface_t* surf = cairo_pdf_surface_create(path.c_str(), first->w + 2 * off, first->h + 2 * off);
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
    std::set<std::string> missing;
    int n = 0;
    for (const auto& pid : pages) {
        const auto& pg = *d.as<core::Page>(pid);
        cairo_pdf_surface_set_size(surf, pg.w + 2 * off, pg.h + 2 * off);   // before anything is drawn on it
        cairo_pdf_surface_set_page_label(surf, d.folio(pid).c_str());
        cairo_save(cr);
        cairo_translate(cr, off, off);
        cairo_save(cr);
        cairo_rectangle(cr, -bleed, -bleed, pg.w + 2 * bleed, pg.h + 2 * bleed);   // nothing past the bleed
        cairo_clip(cr);
        draw_page(cr, d, pid, flows, images, &missing);
        cairo_restore(cr);
        if (opt.marks) crop_marks(cr, pg.w, pg.h, bleed);
        cairo_restore(cr);
        cairo_show_page(cr);
        ++n;
    }
    // s054: bookmarks, each nested under the last one a level up.
    if (opt.bookmarks) {
        std::vector<int> parent_at{CAIRO_PDF_OUTLINE_ROOT};   // [level-1] -> the id to nest under
        for (const auto& b : bookmarks(d, flows)) {
            const auto at = std::find(pages.begin(), pages.end(), b.page);
            if (at == pages.end()) continue;
            const int lv = std::clamp(b.level, 1, static_cast<int>(parent_at.size()));
            parent_at.resize(static_cast<std::size_t>(lv));
            char dest[96];
            std::snprintf(dest, sizeof dest, "page=%d pos=[%g %g]", static_cast<int>(at - pages.begin()) + 1, off, off + b.y);
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
