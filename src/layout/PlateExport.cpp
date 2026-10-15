// The print PDF through Plate (s015). The same walk as Export.cpp (cairo),
// but every mark goes through libs/plate: text as glyph runs from the very
// Pango lines the canvas draws, colour from the swatches, page boxes.
#include "layout/Export.hpp"
#include "layout/Flow.hpp"

#include "plate/pango.hpp"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace composix::layout {

namespace {

constexpr double kMarkGap = 3, kMarkLen = 15, kSlug = kMarkGap + kMarkLen + 3;

// A swatch as ink. Process colours stay in their own space; a spot colour
// goes as its CMYK alternate until separations (s016).
plate::Color ink_of(const core::Doc& d, const std::string& swatch) {
    const auto* s = d.as<core::Swatch>(swatch);
    if (!s) return plate::Color::cmyk(0, 0, 0, 1);   // black text is 100 K, not four-colour
    switch (s->model) {
    case core::ColorModel::RGB:  return plate::Color::rgb(s->v[0] / 255, s->v[1] / 255, s->v[2] / 255);
    case core::ColorModel::Gray: return plate::Color::gray(1 - s->v[0] / 100);
    case core::ColorModel::CMYK:
    case core::ColorModel::Spot: return plate::Color::cmyk(s->v[0] / 100, s->v[1] / 100, s->v[2] / 100, s->v[3] / 100);
    }
    return plate::Color::cmyk(0, 0, 0, 1);
}

// An image for Plate: a JPEG's own bytes when the source kept them, else
// the surface's pixels (un-premultiplied) with their alpha.
plate::ImageId add_image(plate::Document& doc, cairo_surface_t* pic) {
    unsigned char* jpeg = nullptr;
    unsigned long jlen = 0;
    cairo_surface_get_mime_data(pic, CAIRO_MIME_TYPE_JPEG, const_cast<const unsigned char**>(&jpeg), &jlen);
    if (jpeg && jlen) {
        const std::string bytes(reinterpret_cast<const char*>(jpeg), jlen);
        int w = 0, h = 0, c = 0;
        if (plate::jpeg_info(bytes, &w, &h, &c)) return doc.add_image_jpeg(bytes, w, h, c);
    }
    cairo_surface_flush(pic);
    const int w = cairo_image_surface_get_width(pic), h = cairo_image_surface_get_height(pic);
    const int stride = cairo_image_surface_get_stride(pic);
    const bool has_alpha = cairo_image_surface_get_format(pic) == CAIRO_FORMAT_ARGB32;
    const unsigned char* data = cairo_image_surface_get_data(pic);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3), alpha(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(data + static_cast<std::ptrdiff_t>(y) * stride);
        for (int x = 0; x < w; ++x) {
            const std::uint32_t p = row[x];
            const unsigned a = has_alpha ? (p >> 24) & 0xff : 255;
            unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            rgb[i * 3] = static_cast<std::uint8_t>(std::min(255u, r));
            rgb[i * 3 + 1] = static_cast<std::uint8_t>(std::min(255u, g));
            rgb[i * 3 + 2] = static_cast<std::uint8_t>(std::min(255u, b));
            alpha[i] = static_cast<std::uint8_t>(a);
        }
    }
    return doc.add_image_rgb(w, h, rgb.data(), has_alpha ? alpha.data() : nullptr);
}

struct Ctx {
    const core::Doc&    d;
    plate::Document&    doc;
    const ImageSource&  images;
    FlowCache           flows;
    std::map<std::string, plate::ImageId> placed;   // asset -> image (one embedding per file)
    std::set<std::string> missing;
};

void draw_text(Ctx& c, plate::Page& pg, const core::Frame& f, const std::string& fid) {
    const auto* st = c.d.as<core::Story>(f.story);
    if (!st) return;
    const auto& flow = c.flows.get(c.d, f.story);
    visit_lines(c.d, f.story, flow, fid, [&](const LineVisit& v) {
        if (v.para >= static_cast<int>(st->paras.size())) return;
        const auto& para = st->paras[static_cast<std::size_t>(v.para)];
        const auto* ps = c.d.as<core::Style>(para.style);
        for (const auto& run : plate::pango::runs(v.line, f.x + v.x, f.y + v.baseline)) {
            // The run's colour: its character style's, else the paragraph's.
            std::string sw = ps ? ps->color : std::string{};
            if (const auto* cs = c.d.as<core::Style>(core::char_style_at(para, v.from + run.start + 1)); cs && !cs->color.empty())
                sw = cs->color;
            pg.set_fill(ink_of(c.d, sw));
            plate::pango::show(c.doc, pg, run);
        }
    });
}

void draw_image(Ctx& c, plate::Page& pg, const core::Frame& f) {
    const auto* a = c.d.as<core::Asset>(f.asset);
    if (!a) return;
    plate::ImageId id = -1;
    int pw = 0, ph = 0;
    if (auto it = c.placed.find(f.asset); it != c.placed.end()) {
        id = it->second;
    }
    cairo_surface_t* pic = c.images ? c.images(*a, f.w, f.h) : nullptr;
    if (!pic || cairo_surface_status(pic) != CAIRO_STATUS_SUCCESS) {
        if (pic) cairo_surface_destroy(pic);
        c.missing.insert(a->name);
        return;
    }
    pw = cairo_image_surface_get_width(pic);
    ph = cairo_image_surface_get_height(pic);
    if (id < 0) id = c.placed[f.asset] = add_image(c.doc, pic);
    cairo_surface_destroy(pic);
    const double iw = a->px_w > 0 ? a->px_w : pw, ih = a->px_h > 0 ? a->px_h : ph;
    const core::Rect r = core::fit_rect({f.x, f.y, f.w, f.h}, iw, ih, f.fit);
    pg.save();
    pg.clip_rect({f.x, f.y, f.w, f.h});
    pg.image(id, {r.x, r.y, r.w, r.h});
    pg.restore();
}

void draw_frames(Ctx& c, plate::Page& pg, const core::Doc::Id& owner) {
    for (const auto& fid : c.d.frames_on(owner)) {
        const auto& f = *c.d.as<core::Frame>(fid);
        if (f.kind == core::FrameKind::Text && !f.story.empty()) draw_text(c, pg, f, fid);
        else if (f.kind == core::FrameKind::Image) draw_image(c, pg, f);
    }
}

void crop_marks(plate::Page& pg, double w, double h, double bleed) {
    pg.set_stroke(plate::Color::cmyk(1, 1, 1, 1));   // registration: on every plate
    pg.set_line_width(0.25);
    const double a = bleed + kMarkGap, b = a + kMarkLen;
    for (const double x : {0.0, w})
        for (const double y : {0.0, h}) {
            const double sx = x == 0 ? -1 : 1, sy = y == 0 ? -1 : 1;
            pg.move_to(x + sx * a, y);
            pg.line_to(x + sx * b, y);
            pg.move_to(x, y + sy * a);
            pg.line_to(x, y + sy * b);
        }
    pg.stroke();
}

}  // namespace

bool export_print_pdf(const core::Doc& d, const std::string& path, const PdfOptions& opt, const ImageSource& images,
                      PdfReport* report, std::string* err) {
    const auto pages = d.pages();
    if (pages.empty()) { if (err) *err = "The document has no pages."; return false; }
    const double bleed = opt.bleed ? std::max(0.0, d.settings().bleed) : 0;
    const double off = bleed + (opt.marks ? kSlug : 0);

    plate::Document doc;
    doc.set_title(opt.title.empty() ? d.settings().title : opt.title);
    doc.set_creator("Composix");
    doc.set_compress(opt.compress);
    Ctx c{d, doc, images, {}, {}, {}};
    for (const auto& pid : pages) {
        const auto& p = *d.as<core::Page>(pid);
        auto& pg = doc.add_page(p.w + 2 * off, p.h + 2 * off);
        pg.set_trim_box({off, off, p.w, p.h});
        pg.set_bleed_box({off - bleed, off - bleed, p.w + 2 * bleed, p.h + 2 * bleed});
        pg.set_label(d.folio(pid));
        pg.save();
        pg.translate(off, off);
        pg.save();
        pg.clip_rect({-bleed, -bleed, p.w + 2 * bleed, p.h + 2 * bleed});   // nothing past the bleed
        for (const auto& m : core::master_chain(d, pid)) draw_frames(c, pg, m);
        draw_frames(c, pg, pid);
        pg.restore();
        if (opt.marks) crop_marks(pg, p.w, p.h, bleed);
        pg.restore();
    }
    if (!doc.save(path, err)) return false;
    preflight(d, std::vector<std::string>(c.missing.begin(), c.missing.end()), static_cast<int>(pages.size()), report);
    return true;
}

}  // namespace composix::layout
