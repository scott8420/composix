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

// A swatch as ink. Process colours stay in their own space; a spot colour is
// a Separation (s016) -- its own plate.
plate::Color ink_of(const core::Doc& d, const std::string& swatch, double tint = 1) {
    const auto* s = d.as<core::Swatch>(swatch);
    const double t = std::clamp(tint, 0.0, 1.0);
    if (!s) return plate::Color::cmyk(0, 0, 0, t);   // black text is 100 K, not four-colour
    switch (s->model) {
    // s021: a tint is less ink -- process values scaled, RGB toward white.
    case core::ColorModel::RGB:
        return plate::Color::rgb(1 - t * (1 - s->v[0] / 255), 1 - t * (1 - s->v[1] / 255), 1 - t * (1 - s->v[2] / 255));
    case core::ColorModel::Gray: return plate::Color::gray(1 - t * s->v[0] / 100);
    case core::ColorModel::CMYK:
        return plate::Color::cmyk(t * s->v[0] / 100, t * s->v[1] / 100, t * s->v[2] / 100, t * s->v[3] / 100);
    case core::ColorModel::Spot:   // s016: its own plate, named for its ink; its CMYK for proofs. s021: a tint of it
        return plate::Color::spot(s->spot.empty() ? s->name : s->spot, t, s->v[0] / 100, s->v[1] / 100,
                                  s->v[2] / 100, s->v[3] / 100);
    }
    return plate::Color::cmyk(0, 0, 0, t);
}

// An image for Plate: a JPEG's own bytes when the source kept them, else
// the surface's pixels (un-premultiplied) with their alpha.
plate::ImageId add_image(plate::Document& doc, cairo_surface_t* pic, bool* rgb_kept) {
    unsigned char* jpeg = nullptr;
    unsigned long jlen = 0;
    cairo_surface_get_mime_data(pic, CAIRO_MIME_TYPE_JPEG, const_cast<const unsigned char**>(&jpeg), &jlen);
    if (jpeg && jlen) {
        const std::string bytes(reinterpret_cast<const char*>(jpeg), jlen);
        int w = 0, h = 0, c = 0;
        // Converting to CMYK, an RGB JPEG goes through its pixels instead.
        if (plate::jpeg_info(bytes, &w, &h, &c) && !(c == 3 && doc.convert_rgb())) {
            if (c == 3) *rgb_kept = true;
            return doc.add_image_jpeg(bytes, w, h, c);
        }
    }
    if (!doc.convert_rgb()) *rgb_kept = true;
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
    std::set<std::string> rgb_images;   // placed and left RGB
};

void draw_text(Ctx& c, plate::Page& pg, const core::Frame& f, const std::string& fid, const std::string& on_page) {
    const auto* st = c.d.as<core::Story>(f.story);
    if (!st) return;
    // s023: markers filled in -- a master's for the page it's drawn on.
    const auto& rs = c.flows.resolved(c.d, f.story, c.d.as<core::Master>(f.owner) ? on_page : std::string{});
    if (rs.any) st = &rs.text;
    const auto& flow = rs.any ? rs.flow : c.flows.get(c.d, f.story);
    // s044: paragraph rules, under the words -- each in its swatch's ink.
    auto marks = paragraph_rules(c.d, f.story, flow, fid, rs.any ? &rs.text : nullptr);
    for (auto& m : column_rules(f)) marks.push_back(std::move(m));   // s055
    for (const auto& m : marks) {
        pg.set_fill(ink_of(c.d, m.color, m.tint));
        pg.rect({f.x + m.r.x, f.y + m.r.y, m.r.w, m.r.h});
        pg.fill();
    }
    visit_lines(c.d, f.story, flow, fid, [&](const LineVisit& v) {
        if (v.para >= static_cast<int>(st->paras.size())) return;
        const auto& para = st->paras[static_cast<std::size_t>(v.para)];
        const auto* ps = c.d.as<core::Style>(para.style);
        for (const auto& run : plate::pango::runs(v.line, f.x + v.x, f.y + v.baseline)) {
            // The run's colour: its character style's, else the paragraph's.
            std::string sw = ps ? ps->color : std::string{};
            double tint = ps ? ps->color_tint : 1;
            // (A tab's leader -- from -1 -- takes the paragraph's colour.)
            // s030: local colour over everything.
            if (v.from >= 0)
                if (const auto lc = core::local_at(para, v.from + run.start + 1).color; c.d.as<core::Swatch>(lc)) {
                    pg.set_fill(ink_of(c.d, lc, 1));
                    plate::pango::show(c.doc, pg, run);
                    continue;
                }
            if (const auto* cs = v.from < 0 ? nullptr : c.d.as<core::Style>(core::char_style_at(para, v.from + run.start + 1));
                cs && !cs->color.empty()) {
                sw = cs->color;
                tint = cs->color_tint;
            }
            pg.set_fill(ink_of(c.d, sw, tint));
            plate::pango::show(c.doc, pg, run);
        }
    }, rs.any ? &rs.text : nullptr);
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
    if (id < 0) {
        bool rgb = false;
        id = c.placed[f.asset] = add_image(c.doc, pic, &rgb);
        if (rgb) c.rgb_images.insert(a->name);
    }
    cairo_surface_destroy(pic);
    const double iw = a->px_w > 0 ? a->px_w : pw, ih = a->px_h > 0 ? a->px_h : ph;
    const core::Rect r = core::picture_rect({f.x, f.y, f.w, f.h}, f, iw, ih);
    pg.save();
    pg.clip_rect({f.x, f.y, f.w, f.h});
    pg.image(id, {r.x, r.y, r.w, r.h});
    pg.restore();
}

void draw_frames(Ctx& c, plate::Page& pg, const core::Doc::Id& owner, const core::Doc::Id& on_page) {
    for (const auto& fid : c.d.frames_on(owner)) {
        if (owner != on_page && !core::master_item_on(c.d, fid, on_page)) continue;   // s025
        const auto& f = *c.d.as<core::Frame>(fid);
        pg.save();
        if (f.opacity < 1) pg.set_alpha(f.opacity, f.opacity);   // s021: fill, content and stroke alike
        if (c.d.as<core::Swatch>(f.fill)) {   // s016: the frame's background
            pg.set_fill(ink_of(c.d, f.fill, f.fill_tint));
            pg.rect({f.x, f.y, f.w, f.h});
            pg.fill();
        }
        if (f.kind == core::FrameKind::Text && !f.story.empty()) draw_text(c, pg, f, fid, on_page);
        else if (f.kind == core::FrameKind::Image) draw_image(c, pg, f);
        if (c.d.as<core::Swatch>(f.stroke) && f.stroke_w > 0) {
            pg.set_stroke(ink_of(c.d, f.stroke, f.stroke_tint));
            pg.set_line_width(f.stroke_w);
            pg.rect({f.x, f.y, f.w, f.h});
            pg.stroke();
        }
        pg.restore();
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
    auto pages = d.pages();
    if (!opt.only.empty())   // s054: a page range
        std::erase_if(pages, [&](const std::string& p) { return std::find(opt.only.begin(), opt.only.end(), p) == opt.only.end(); });
    if (pages.empty()) { if (err) *err = "The document has no pages."; return false; }
    const double bleed = opt.bleed ? std::max(0.0, d.settings().bleed) : 0;
    const double off = bleed + (opt.marks ? kSlug : 0);

    plate::Document doc;
    doc.set_title(opt.title.empty() ? d.settings().title : opt.title);
    doc.set_creator("Composix");
    doc.set_compress(opt.compress);
    // s017: set before anything is drawn (the writer converts as it goes).
    if (opt.standard != plate_standard::None) {
        if (opt.profile.empty()) { if (err) *err = "PDF/X needs an output profile."; return false; }
        if (opt.standard == plate_standard::X1a) {   // s021: no transparency -- say which frames
            std::string seen;
            for (const auto& pid : pages)
                for (const auto& owner : [&] { auto o = core::master_chain(d, pid); o.push_back(pid); return o; }())
                    for (const auto& fid : d.frames_on(owner))
                        if (const auto* f = d.as<core::Frame>(fid); f && f->opacity < 1) {
                            const std::string where = d.as<core::Page>(owner) ? "page " + d.folio(owner) : "a master";
                            const std::string line = "\n\u2022 a frame on " + where + " at " + core::tint_text(f->opacity) + " opacity";
                            if (seen.find(line) == std::string::npos) seen += line;
                        }
            if (!seen.empty()) {
                if (err) *err = "PDF/X-1a allows no transparency:" + seen + "\nSet them to 100% or export PDF/X-4.";
                return false;
            }
        }
        doc.set_standard(opt.standard == plate_standard::X1a ? plate::Standard::X1a_2003 : plate::Standard::X4);
    }
    // s016: the press's profile; RGB converted to its CMYK, or kept (tagged sRGB).
    if (!opt.profile.empty()) {
        std::string perr;
        if (!doc.set_output_profile(opt.profile, &perr)) { if (err) *err = "Output profile: " + perr; return false; }
        doc.set_intent(opt.perceptual ? plate::Intent::Perceptual : plate::Intent::RelativeColorimetric);
        doc.set_convert_rgb(opt.convert);
    }
    Ctx c{d, doc, images, {}, {}, {}};
    const auto sheets = pdf_sheets(d, pages, opt.spreads);   // s084
    for (const auto& sh : sheets) {
        auto& pg = doc.add_page(sh.w + 2 * off, sh.h + 2 * off);
        pg.set_trim_box({off, off, sh.w, sh.h});
        pg.set_bleed_box({off - bleed, off - bleed, sh.w + 2 * bleed, sh.h + 2 * bleed});
        pg.set_label(sh.label);
        for (std::size_t k = 0; k < sh.pages.size(); ++k) {
            const auto& pid = sh.pages[k];
            const auto& p = *d.as<core::Page>(pid);
            // s084: the bleed on the spread's outer edges only.
            const double x0 = k == 0 ? -bleed : 0, x1 = k + 1 == sh.pages.size() ? p.w + bleed : p.w;
            pg.save();
            pg.translate(off + sh.x[k], off);
            pg.clip_rect({x0, -bleed, x1 - x0, p.h + 2 * bleed});   // nothing past the bleed
            for (const auto& m : core::master_chain(d, pid)) draw_frames(c, pg, m, pid);
            draw_frames(c, pg, pid, pid);
            pg.restore();
        }
        if (opt.marks) {
            pg.save();
            pg.translate(off, off);
            crop_marks(pg, sh.w, sh.h, bleed);
            pg.restore();
        }
    }
    {   // s083: the contents page's entries, clickable (Plate leaves them out under PDF/X)
        FlowCache flows;
        for (const auto& l : contents_links(d, flows)) {
            const auto at = sheet_of(sheets, l.page), to = sheet_of(sheets, l.to);   // s084: by sheet
            if (at.sheet >= 0 && to.sheet >= 0)
                doc.add_link(static_cast<std::size_t>(at.sheet), off + at.x + l.box.x, off + l.box.y, l.box.w, l.box.h,
                             static_cast<std::size_t>(to.sheet), off + l.to_y);
        }
    }
    if (opt.bookmarks) {   // s054
        FlowCache flows;
        for (const auto& b : bookmarks(d, flows, opt.mark_lists, pages))
            if (const auto at = sheet_of(sheets, b.page); at.sheet >= 0)
                doc.add_bookmark(b.title, b.level, static_cast<std::size_t>(at.sheet), off + b.y);
    }
    if (!doc.save(path, err)) return false;
    preflight(d, std::vector<std::string>(c.missing.begin(), c.missing.end()), static_cast<int>(pages.size()), report);
    if (report && !c.rgb_images.empty()) {
        std::string names;
        for (const auto& n : c.rgb_images) names += (names.empty() ? "" : ", ") + n;
        report->warnings.push_back(std::string(doc.has_output_profile() ? "RGB images (tagged sRGB, not converted): "
                                                                        : "RGB images (no output profile): ") + names);
    }
    if (report && doc.has_output_profile())
        report->warnings.insert(report->warnings.begin(), "Output profile: " + doc.output_profile_name() +
                                                              (doc.convert_rgb() ? std::string(" (RGB converted, ") + (opt.perceptual ? "perceptual)" : "relative colorimetric)")
                                                                                 : std::string()));   // s085
    if (report && doc.standard() != plate::Standard::None)
        report->warnings.insert(report->warnings.begin(), std::string("Standard: ") +
                                                              plate::Document::standard_name(doc.standard()) +
                                                              (doc.flattened() ? " (" + std::to_string(doc.flattened()) +
                                                                                     " image(s) flattened on white)" : ""));
    return true;
}

}  // namespace composix::layout
