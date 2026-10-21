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
    {   // s187: a CMYK picture's own separations, when the source kept them
        // Copied out FIRST: cairo_surface_flush lets go of a surface's mime data.
        const unsigned char* got = nullptr;
        unsigned long n = 0;
        cairo_surface_get_mime_data(pic, kCmykMime, &got, &n);
        const std::vector<std::uint8_t> ink(got, got ? got + n : got);
        const int w = cairo_image_surface_get_width(pic), h = cairo_image_surface_get_height(pic);
        if (!ink.empty() && w > 0 && h > 0 && n == static_cast<unsigned long>(w) * static_cast<unsigned long>(h) * 4) {
            cairo_surface_flush(pic);
            std::vector<std::uint8_t> alpha;
            if (cairo_image_surface_get_format(pic) == CAIRO_FORMAT_ARGB32) {
                alpha.resize(static_cast<std::size_t>(w) * h);
                const unsigned char* data = cairo_image_surface_get_data(pic);
                const int stride = cairo_image_surface_get_stride(pic);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        alpha[static_cast<std::size_t>(y) * w + x] =
                            static_cast<std::uint8_t>(reinterpret_cast<const std::uint32_t*>(data + static_cast<std::ptrdiff_t>(y) * stride)[x] >> 24);
            }
            return doc.add_image_cmyk(w, h, ink.data(), alpha.empty() ? nullptr : alpha.data());
        }
    }
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
    // s193 (G6): PDF/X-1a -- transparency flattened region by region.
    bool x1a = false;
    std::map<std::string, bool> see_through;   // asset -> its picture has see-through pixels
    std::vector<std::string> flattened;         // "page 3: a frame at 50% opacity"
};

// s193: does this asset's picture (for print) have see-through pixels?
bool asset_see_through(Ctx& c, const core::Asset& a, const core::Frame& f, const std::string& aid) {
    if (const auto it = c.see_through.find(aid); it != c.see_through.end()) return it->second;
    bool clear = false;
    if (cairo_surface_t* pic = c.images ? c.images(a, f.w, f.h) : nullptr) {
        if (cairo_surface_status(pic) == CAIRO_STATUS_SUCCESS && cairo_image_surface_get_format(pic) == CAIRO_FORMAT_ARGB32) {
            cairo_surface_flush(pic);
            const int w = cairo_image_surface_get_width(pic), h = cairo_image_surface_get_height(pic);
            const int stride = cairo_image_surface_get_stride(pic);
            const unsigned char* data = cairo_image_surface_get_data(pic);
            for (int y = 0; y < h && !clear; ++y)
                for (int x = 0; x < w; ++x)
                    if ((reinterpret_cast<const std::uint32_t*>(data + static_cast<std::ptrdiff_t>(y) * stride)[x] >> 24) < 255) { clear = true; break; }
        }
        cairo_surface_destroy(pic);
    }
    c.see_through[aid] = clear;
    return clear;
}

void draw_text(Ctx& c, plate::Page& pg, const core::Frame& f, const std::string& fid, const std::string& on_page) {
    const auto* st = c.d.as<core::Story>(f.story);
    if (!st) return;
    // s023: markers filled in -- a master's for the page it's drawn on.
    const auto& rs = c.flows.resolved(c.d, f.story, c.d.as<core::Master>(f.owner) ? on_page : std::string{});
    if (rs.any) st = &rs.text;
    const auto& flow = rs.any ? rs.flow : c.flows.get(c.d, f.story);
    // s044: paragraph rules, under the words -- each in its swatch's ink.
    auto marks = paragraph_rules(c.d, f.story, flow, fid, rs.any ? &rs.text : nullptr);
    const FrameText* ftx = nullptr;   // s168: rules not through a spanning paragraph
    for (const auto& x : flow.frames) if (x.frame == fid) ftx = &x;
    for (auto& m : column_rules(f, ftx)) marks.push_back(std::move(m));   // s055
    for (const auto& m : marks) {
        pg.set_fill(ink_of(c.d, m.color, m.tint));
        pg.rect({f.x + m.r.x, f.y + m.r.y, m.r.w, m.r.h});
        pg.fill();
    }
    // s216: text on a path -- each glyph turned to the path at its middle (one line).
    const bool on_path = f.on_path && f.kind == core::FrameKind::Text;
    const core::doc::PathTrack tr = on_path ? core::doc::path_track(f) : core::doc::PathTrack{};
    double first_base = -1, shift = 0, line_base = 0;   // s296: line_base -- where the line's runs sit unraised
    const auto show = [&](const plate::pango::Run& run) {
        if (!on_path) { plate::pango::show(c.doc, pg, run); return; }
        double pen = run.x;
        for (const auto& g : run.glyphs) {
            const double mid = pen - f.x + g.advance / 2;
            pen += g.advance;
            const double s = tr.text_s(std::max(0.0, f.path_from), mid);   // s225
            if (s < 0 || s > tr.length()) continue;
            double px, py, ang;
            tr.at(s, px, py, ang);
            plate::pango::Run one = run;
            one.x = -g.advance / 2;
            one.baseline = shift + (run.baseline - line_base);   // s218: below the path by the alignment; s296: a raised run by its rise
            one.glyphs = {g};
            pg.save();
            const auto m = core::doc::path_axes(f, ang, px, py);   // s223: the effect
            pg.concat(m.xx, m.yx, m.xy, m.yy, px, py);
            plate::pango::show(c.doc, pg, one);
            pg.restore();
        }
    };
    visit_lines(c.d, f.story, flow, fid, [&](const LineVisit& v) {
        if (v.para >= static_cast<int>(st->paras.size())) return;
        if (on_path) {   // its first line only
            if (first_base < 0) {
                first_base = v.baseline;
                shift = core::doc::path_shift(f, line_ascent(v.line), line_descent(v.line));
            }
            if (v.baseline > first_base + 0.01) return;
        }
        const auto& para = st->paras[static_cast<std::size_t>(v.para)];
        const auto* ps = c.d.as<core::Style>(para.style);
        // s261: underlines / strikethroughs, each in its run's ink, after its glyphs.
        const auto decos = on_path ? std::vector<TextDeco>{} : text_decorations(v.line, f.x + v.x, f.y + v.baseline);
        const auto strokes = [&](const plate::pango::Run& run) {
            for (const auto& t : decos) {
                if (t.start != run.start) continue;
                if (t.under) { pg.rect({t.x0, t.under_y, t.x1 - t.x0, t.under_t}); pg.fill(); }
                if (t.strike) { pg.rect({t.x0, t.strike_y, t.x1 - t.x0, t.strike_t}); pg.fill(); }
            }
        };
        line_base = f.y + v.baseline;   // s296
        for (const auto& run : plate::pango::runs(v.line, f.x + v.x, f.y + v.baseline)) {
            // The run's colour: its character style's, else the paragraph's.
            std::string sw = ps ? ps->color : std::string{};
            double tint = ps ? ps->color_tint : 1;
            // (A tab's leader -- from -1 -- takes the paragraph's colour.)
            // s030: local colour over everything.
            if (v.from >= 0)
                if (const auto lc = core::local_at(para, v.from + run.start + 1).color; c.d.as<core::Swatch>(lc)) {
                    pg.set_fill(ink_of(c.d, lc, 1));
                    show(run);
                    strokes(run);
                    continue;
                }
            if (const auto* cs = v.from < 0 ? nullptr : c.d.as<core::Style>(core::char_style_at(para, v.from + run.start + 1));
                cs && !cs->color.empty()) {
                sw = cs->color;
                tint = cs->color_tint;
            }
            pg.set_fill(ink_of(c.d, sw, tint));
            show(run);
            strokes(run);
        }
    }, rs.any ? &rs.text : nullptr);
    if (on_path) return;
    // s315: the tables' rows -- fills, words, the grid, each in its ink
    visit_tables(c.d, f.story, flow, fid, [&](const TablePiece& tp) {
        pg.set_fill(ink_of(c.d, tp.color, tp.tint));
        if (tp.kind == TablePiece::Text) {
            for (const auto& run : plate::pango::runs(tp.line, f.x + tp.x, f.y + tp.baseline)) plate::pango::show(c.doc, pg, run);
        } else {
            pg.rect({f.x + tp.r.x, f.y + tp.r.y, tp.r.w, tp.r.h});
            pg.fill();
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
    if (f.radius > 0 || core::doc::has_outline(f)) {   // s205: cut by its rounded corners (s209: or its shape)
        for (const auto& o : core::doc::outline_ops(f)) {
            if (o.op == 'M') pg.move_to(o.v[0], o.v[1]);
            else if (o.op == 'L') pg.line_to(o.v[0], o.v[1]);
            else if (o.op == 'C') pg.curve_to(o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
            else pg.close_path();
        }
        pg.clip_even_odd();
    }
    if (const auto polys = core::clip_polys(f, *a, r); !polys.empty()) {   // s189: its clipping path
        for (const auto& poly : polys) {
            pg.move_to(poly[0][0], poly[0][1]);
            for (std::size_t i = 1; i < poly.size(); ++i) pg.line_to(poly[i][0], poly[i][1]);
            pg.close_path();
        }
        pg.clip_even_odd();
    }
    pg.image(id, {r.x, r.y, r.w, r.h});
    pg.restore();
}

void draw_frames(Ctx& c, plate::Page& pg, const core::Doc::Id& owner, const core::Doc::Id& on_page) {
    for (const auto& fid : c.d.frames_on(owner)) {
        if (owner != on_page && !core::master_item_on(c.d, fid, on_page)) continue;   // s025
        if (!core::doc::frame_prints(c.d, *c.d.as<core::Frame>(fid))) continue;   // s305: its layer hidden or not printing
        // s155: a copy -- an item across the spine shifted on a right page,
        // cut at the spine.
        core::Frame f = *c.d.as<core::Frame>(fid);
        const auto* page = c.d.as<core::Page>(on_page);
        const core::Side sd = page ? c.d.side_of(on_page) : core::Side::Any;
        pg.save();
        if (f.across && owner != on_page && sd != core::Side::Any) {
            f.x += core::across_dx(c.d, f, sd);
            if (sd == core::Side::Left) pg.clip_rect({-1e4, -1e4, 1e4 + page->w, 2e4});
            else                        pg.clip_rect({0, -1e4, 1e4, 2e4});
        }
        // s193 (G6): PDF/X-1a has no transparency -- a see-through frame (its
        // opacity, or a picture with see-through pixels; a clipping path is
        // not transparency) goes in FLATTENED: its
        // box, with everything under it, drawn as one picture at 300 ppi
        // (to the press's CMYK, as any RGB picture); what's above stays vector.
        if (c.x1a) {
            const auto* a = f.kind == core::FrameKind::Image ? c.d.as<core::Asset>(f.asset) : nullptr;
            const bool clear = f.opacity < 1 || (a && asset_see_through(c, *a, f, f.asset));
            if (clear) {
                const double grow = (c.d.as<core::Swatch>(f.stroke) ? (f.end_a || f.end_b ? f.stroke_w * 4.5 + 2 : f.stroke_w / 2) : 0) + 0.5;   // s204: arrows
                const core::Rect fb = core::frame_bounds(f);   // s228: where it reaches, turned
                core::Rect box{fb.x - grow, fb.y - grow, fb.w + 2 * grow, fb.h + 2 * grow};
                const double bl = core::bleed_of(c.d, on_page);
                const double pw = page ? page->w : box.r(), ph = page ? page->h : box.b();
                const double x0 = std::max(box.x, -bl), y0 = std::max(box.y, -bl), x1 = std::min(box.r(), pw + bl), y1 = std::min(box.b(), ph + bl);
                if (x1 > x0 && y1 > y0) {
                    constexpr double kPpi = 300;
                    const double k = std::min(kPpi / 72.0, 8000.0 / std::max(x1 - x0, y1 - y0));   // at most 8000 px a side
                    const int W = std::max(1, static_cast<int>(std::ceil((x1 - x0) * k))), H = std::max(1, static_cast<int>(std::ceil((y1 - y0) * k)));
                    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
                    cairo_t* cr = cairo_create(s);
                    cairo_set_source_rgb(cr, 1, 1, 1);   // paper
                    cairo_paint(cr);
                    cairo_scale(cr, k, k);
                    cairo_translate(cr, -x0, -y0);
                    PageDraw how;
                    how.stop_after = fid;
                    draw_page(cr, c.d, on_page.empty() ? owner : on_page, c.flows, c.images, nullptr, how);
                    cairo_destroy(cr);
                    cairo_surface_flush(s);
                    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(W) * H * 3);
                    const unsigned char* data = cairo_image_surface_get_data(s);
                    const int stride = cairo_image_surface_get_stride(s);
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const std::uint32_t p = reinterpret_cast<const std::uint32_t*>(data + static_cast<std::ptrdiff_t>(y) * stride)[x];
                            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 3;
                            rgb[i] = (p >> 16) & 0xff;
                            rgb[i + 1] = (p >> 8) & 0xff;
                            rgb[i + 2] = p & 0xff;
                        }
                    cairo_surface_destroy(s);
                    const auto id = c.doc.add_image_rgb(W, H, rgb.data());
                    pg.image(id, {x0, y0, x1 - x0, y1 - y0});
                    const std::string where = page ? "page " + c.d.folio(on_page) : "a master";
                    c.flattened.push_back(where + ": " + (f.opacity < 1 ? "a frame at " + core::tint_text(f.opacity) + " opacity"
                                                                        : "a see-through picture" + (a ? " (" + a->name + ")" : std::string{})));
                }
                pg.restore();
                continue;
            }
        }
        if (core::turned(f)) {   // s228: turned -- everything it draws, through its turn
            const auto m = core::frame_xf(f);
            pg.concat(m.xx, m.yx, m.xy, m.yy, m.x0, m.y0);
        }
        if (f.opacity < 1) pg.set_alpha(f.opacity, f.opacity);   // s021: fill, content and stroke alike
        // s197: the frame's outline -- its box, or a shape's oval / path.
        const auto outline = [&] {
            if (!core::doc::has_outline(f) && f.radius <= 0) { pg.rect({f.x, f.y, f.w, f.h}); return; }   // s205: or rounded
            for (const auto& o : core::doc::outline_ops(f)) {
                if (o.op == 'M') pg.move_to(o.v[0], o.v[1]);
                else if (o.op == 'L') pg.line_to(o.v[0], o.v[1]);
                else if (o.op == 'C') pg.curve_to(o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
                else pg.close_path();
            }
        };
        if (c.d.as<core::Swatch>(f.fill)) {   // s016: the frame's background
            pg.set_fill(ink_of(c.d, f.fill, f.fill_tint));
            outline();
            if (f.subs.empty()) pg.fill(); else pg.fill_even_odd();   // s211: holes
        }
        if (f.kind == core::FrameKind::Text && !f.story.empty()) draw_text(c, pg, f, fid, on_page);
        else if (f.kind == core::FrameKind::Image) draw_image(c, pg, f);
        if (c.d.as<core::Swatch>(f.stroke) && f.stroke_w > 0) {
            pg.set_stroke(ink_of(c.d, f.stroke, f.stroke_tint));
            pg.set_line_width(f.stroke_w);
            // s204: its ends, corners, dashes; under an arrow the line stops short.
            const auto dash = core::doc::dash_pattern(f);
            pg.save();
            pg.set_line_cap(f.dash == 2 ? 1 : f.cap);
            pg.set_line_join(f.join);
            if (!dash.empty()) pg.set_dash(dash);
            if (core::doc::has_outline(f)) {
                for (const auto& o : core::doc::stroke_ops(f)) {
                    if (o.op == 'M') pg.move_to(o.v[0], o.v[1]);
                    else if (o.op == 'L') pg.line_to(o.v[0], o.v[1]);
                    else if (o.op == 'C') pg.curve_to(o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
                    else pg.close_path();
                }
            } else {
                outline();
            }
            pg.stroke();
            pg.restore();
            const auto ends = core::doc::end_shapes(f);
            if (!ends.empty()) {   // the ends, filled in the stroke's ink
                pg.set_fill(ink_of(c.d, f.stroke, f.stroke_tint));
                for (const auto& poly : ends) {
                    for (std::size_t i = 0; i < poly.size(); ++i)
                        if (i) pg.line_to(poly[i].x, poly[i].y); else pg.move_to(poly[i].x, poly[i].y);
                    pg.close_path();
                }
                pg.fill();
            }
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

    plate::Document doc;
    doc.set_title(opt.title.empty() ? d.settings().title : opt.title);
    doc.set_creator("Composix");
    doc.set_compress(opt.compress);
    // s017: set before anything is drawn (the writer converts as it goes).
    if (opt.standard != plate_standard::None) {
        if (opt.profile.empty()) { if (err) *err = "PDF/X needs an output profile."; return false; }
        // s021 refused a frame with transparency under PDF/X-1a; s193 flattens it (draw_frames).
        doc.set_standard(opt.standard == plate_standard::X1a ? plate::Standard::X1a_2003 : plate::Standard::X4);
    }
    // s016: the press's profile; RGB converted to its CMYK, or kept (tagged sRGB).
    if (!opt.profile.empty()) {
        std::string perr;
        if (!doc.set_output_profile(opt.profile, &perr)) { if (err) *err = "Output profile: " + perr; return false; }
        doc.set_intent(opt.perceptual ? plate::Intent::Perceptual : plate::Intent::RelativeColorimetric);
        doc.set_convert_rgb(opt.convert);
    }
    Ctx c{d, doc, images, {}, {}, {}, {}};
    c.x1a = opt.standard == plate_standard::X1a;   // s193
    const std::size_t npages = pages.size();
    if (opt.progress)   // s102: lay it all out first, so the bar can say so
        flow_all(d, c.flows, [&](int done, int total) {
            if (total > 0)
                opt.progress(0.3 * done / total, "Laying out page " + std::to_string(std::max<std::size_t>(1, npages * done / total)) +
                                                     " of " + std::to_string(npages) + "\u2026");
        });
    std::size_t written = 0;
    const auto sheets = pdf_sheets(d, pages, opt.spreads);   // s084
    // s147: each sheet's bleed -- its pages' masters' (the most of them).
    std::vector<double> sbleed;
    for (const auto& sh : sheets) {
        double b = 0;
        if (opt.bleed) for (const auto& p : sh.pages) b = std::max(b, core::bleed_of(d, p));
        sbleed.push_back(b);
    }
    const auto off_of = [&](int i) { return sbleed[static_cast<std::size_t>(std::max(0, i))] + (opt.marks ? kSlug : 0); };
    for (const auto& sh : sheets) {
        const int si = static_cast<int>(&sh - sheets.data());
        const double bleed = sbleed[static_cast<std::size_t>(si)], off = off_of(si);   // s147: this sheet's
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
            if (opt.progress && ++written <= npages)   // s102
                opt.progress(0.3 + 0.6 * written / npages,
                             "Writing page " + std::to_string(written) + " of " + std::to_string(npages) + "\u2026");
        }
        if (opt.marks) {
            pg.save();
            pg.translate(off, off);
            crop_marks(pg, sh.w, sh.h, bleed);
            pg.restore();
        }
    }
    {   // s083: the contents page's entries, clickable (Plate leaves them out under PDF/X)
        FlowCache& flows = c.flows;   // s102: the pages' own flows (was a fresh layout of the book)
        auto links = contents_links(d, flows);
        for (auto& l : note_links(d, flows)) links.push_back(std::move(l));   // s123: and the notes'
        for (auto& l : xref_links(d, flows)) links.push_back(std::move(l));   // s131: and the cross-references'
        for (auto& l : hyperlink_links(d, flows)) links.push_back(std::move(l));   // s303: and the words' own links
        for (auto& l : index_links(d, flows)) links.push_back(std::move(l));   // s137: and the index'
        for (const auto& l : links) {
            if (!l.uri.empty()) {   // s303: out of the document
                if (const auto at = sheet_of(sheets, l.page); at.sheet >= 0)
                    doc.add_uri(static_cast<std::size_t>(at.sheet), off_of(at.sheet) + at.x + l.box.x, off_of(at.sheet) + l.box.y, l.box.w, l.box.h, l.uri);
                continue;
            }
            const auto at = sheet_of(sheets, l.page), to = sheet_of(sheets, l.to);   // s084: by sheet
            if (at.sheet >= 0 && to.sheet >= 0)
                doc.add_link(static_cast<std::size_t>(at.sheet), off_of(at.sheet) + at.x + l.box.x, off_of(at.sheet) + l.box.y, l.box.w, l.box.h,
                             static_cast<std::size_t>(to.sheet), off_of(to.sheet) + l.to_y);
        }
    }
    if (opt.bookmarks) {   // s054
        FlowCache& flows = c.flows;   // s102: as above
        for (const auto& b : bookmarks(d, flows, opt.mark_lists, pages))
            if (const auto at = sheet_of(sheets, b.page); at.sheet >= 0)
                doc.add_bookmark(b.title, b.level, static_cast<std::size_t>(at.sheet), off_of(at.sheet) + b.y);
    }
    if (opt.progress) opt.progress(0.92, "Saving the file\u2026");   // s102: fonts subset, images compressed
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
    if (report && !c.flattened.empty()) {   // s193: say what went in flattened, and what that costs
        std::string list;
        for (const auto& f : c.flattened) if (list.find(f) == std::string::npos) list += (list.empty() ? "" : "; ") + f;
        report->warnings.push_back("Transparency flattened for PDF/X-1a (300 ppi, process colour -- spot inks and black text "
                                   "under it print as CMYK there): " + list);
    }
    if (report && doc.standard() != plate::Standard::None)
        report->warnings.insert(report->warnings.begin(), std::string("Standard: ") +
                                                              plate::Document::standard_name(doc.standard()) +
                                                              (doc.flattened() ? " (" + std::to_string(doc.flattened()) +
                                                                                     " image(s) flattened on white)" : ""));
    return true;
}

}  // namespace composix::layout
