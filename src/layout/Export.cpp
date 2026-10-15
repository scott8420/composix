#include "layout/Export.hpp"
#include "layout/Flow.hpp"

#include <cairo-pdf.h>

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
                std::set<std::string>& missing) {
    const auto* a = d.as<core::Asset>(f.asset);
    if (!a) return;
    cairo_surface_t* pic = images ? images(*a, f.w, f.h) : nullptr;
    if (!pic || cairo_surface_status(pic) != CAIRO_STATUS_SUCCESS) {
        if (pic) cairo_surface_destroy(pic);
        missing.insert(a->name);
        return;
    }
    const int pw = cairo_image_surface_get_width(pic), ph = cairo_image_surface_get_height(pic);
    // The picture's own proportions: the file's pixels (a raster), else the surface.
    const double iw = a->px_w > 0 ? a->px_w : pw, ih = a->px_h > 0 ? a->px_h : ph;
    const core::Rect r = core::fit_rect({f.x, f.y, f.w, f.h}, iw, ih, f.fit);
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
                 const ImageSource& images, std::set<std::string>& missing) {
    for (const auto& fid : d.frames_on(owner)) {
        const auto& f = *d.as<core::Frame>(fid);
        switch (f.kind) {
        case core::FrameKind::Text:
            if (f.story.empty()) break;
            cairo_save(cr);
            cairo_translate(cr, f.x, f.y);
            draw_frame_text(cr, d, f.story, flows.get(d, f.story), fid, 0, 0, 0.0);
            cairo_restore(cr);
            break;
        case core::FrameKind::Image:
            draw_image(cr, d, f, images, missing);
            break;
        case core::FrameKind::Shape:   // no fill / stroke in the model yet: nothing prints
            break;
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

bool export_pdf(const core::Doc& d, const std::string& path, const PdfOptions& opt, const ImageSource& images,
                PdfReport* report, std::string* err) {
    const auto pages = d.pages();
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
        for (const auto& m : core::master_chain(d, pid)) draw_frames(cr, d, m, flows, images, missing);
        draw_frames(cr, d, pid, flows, images, missing);
        cairo_restore(cr);
        if (opt.marks) crop_marks(cr, pg.w, pg.h, bleed);
        cairo_restore(cr);
        cairo_show_page(cr);
        ++n;
    }
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    const auto status = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (status != CAIRO_STATUS_SUCCESS) {
        if (err) *err = std::string("Writing the PDF failed: ") + cairo_status_to_string(status);
        return false;
    }

    if (report) {
        report->pages = n;
        report->warnings.clear();
        for (const auto& sid : d.ids_of<core::Story>()) {
            const auto& f = flows.get(d, sid);
            if (f.overset) report->warnings.push_back("Overset text: " + d.as<core::Story>(sid)->name);
        }
        for (const auto& m : missing) report->warnings.push_back("Missing image: " + m);
        std::set<std::string> low;
        for (const auto& fid : d.ids_of<core::Frame>()) {
            const auto& f = *d.as<core::Frame>(fid);
            const auto* a = d.as<core::Asset>(f.asset);
            if (!a || !d.as<core::Page>(f.owner) || missing.count(a->name)) continue;
            const double ppi = core::effective_ppi(f, *a);
            if (ppi > 0 && ppi < core::kLowPpi)
                low.insert(a->name + " (" + std::to_string(static_cast<int>(std::lround(ppi))) + " ppi)");
        }
        for (const auto& l : low) report->warnings.push_back("Low resolution: " + l);
    }
    return true;
}

}  // namespace composix::layout
