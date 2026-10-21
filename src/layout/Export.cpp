#include "layout/Export.hpp"
#include "core/Spell.hpp"
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"

#include <cairo-pdf.h>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <cstring>
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
    if (f.radius > 0 || core::doc::has_outline(f)) {   // s205: cut by its rounded corners (s209: or its shape)
        for (const auto& o : core::doc::outline_ops(f)) {
            if (o.op == 'M') cairo_move_to(cr, o.v[0], o.v[1]);
            else if (o.op == 'L') cairo_line_to(cr, o.v[0], o.v[1]);
            else if (o.op == 'C') cairo_curve_to(cr, o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
            else cairo_close_path(cr);
        }
    } else {
        cairo_rectangle(cr, f.x, f.y, f.w, f.h);
    }
    cairo_clip(cr);
    if (const auto polys = core::clip_polys(f, *a, r); !polys.empty()) {   // s189: its clipping path
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        for (const auto& poly : polys) {
            cairo_move_to(cr, poly[0][0], poly[0][1]);
            for (std::size_t i = 1; i < poly.size(); ++i) cairo_line_to(cr, poly[i][0], poly[i][1]);
            cairo_close_path(cr);
        }
        cairo_clip(cr);
    }
    cairo_translate(cr, r.x, r.y);
    cairo_scale(cr, r.w / std::max(1, pw), r.h / std::max(1, ph));
    cairo_set_source_surface(cr, pic, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(pic);
}

// True when it drew how.stop_after (and stopped there).
bool draw_frames(cairo_t* cr, const core::Doc& d, const core::Doc::Id& owner, FlowCache& flows,
                 const ImageSource& images, std::set<std::string>* missing, const PageDraw& how,
                 const core::Doc::Id& on_page) {
    for (const auto& fid : d.frames_on(owner)) {
        if (owner != on_page && !core::master_item_on(d, fid, on_page)) continue;   // s025
        if (!core::doc::frame_prints(d, *d.as<core::Frame>(fid))) continue;   // s305: its layer hidden or not printing
        // s155: a copy -- an item across the spine sits shifted on a right
        // page, and each page shows only its side of the spine.
        core::Frame f = *d.as<core::Frame>(fid);
        const core::Side sd = d.as<core::Page>(on_page) ? d.side_of(on_page) : core::Side::Any;
        const bool cut = f.across && owner != on_page && sd != core::Side::Any;
        if (cut) {
            f.x += core::across_dx(d, f, sd);
            const double pw = d.as<core::Page>(on_page)->w;
            cairo_save(cr);
            if (sd == core::Side::Left) cairo_rectangle(cr, -1e5, -1e5, 1e5 + pw, 2e5);
            else                        cairo_rectangle(cr, 0, -1e5, 1e5, 2e5);
            cairo_clip(cr);
        }
        // s228: turned -- everything it draws, through its turn.
        const bool turn = core::turned(f);
        if (turn) {
            const auto m = core::frame_xf(f);
            cairo_matrix_t cm;
            cairo_matrix_init(&cm, m.xx, m.yx, m.xy, m.yy, m.x0, m.y0);
            cairo_save(cr);
            cairo_transform(cr, &cm);
        }
        // s021: the swatch at its tint; the frame's opacity on each part.
        const double op = std::clamp(f.opacity, 0.0, 1.0);
        const auto paint = [&](const std::string& sw, double tint) {
            const auto* s = d.as<core::Swatch>(sw);
            if (!s) return false;
            const auto c = core::tint_rgb(core::doc::screen_rgb(*s), tint);
            cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, op);
            return true;
        };
        // s197: the frame's outline -- its box, or a shape's oval / path.
        const auto outline = [&] {
            if (!core::doc::has_outline(f) && f.radius <= 0) { cairo_rectangle(cr, f.x, f.y, f.w, f.h); return; }   // s205: or rounded
            cairo_new_path(cr);
            for (const auto& o : core::doc::outline_ops(f)) {
                if (o.op == 'M') cairo_move_to(cr, o.v[0], o.v[1]);
                else if (o.op == 'L') cairo_line_to(cr, o.v[0], o.v[1]);
                else if (o.op == 'C') cairo_curve_to(cr, o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
                else cairo_close_path(cr);
            }
        };
        if (paint(f.fill, f.fill_tint)) {   // s016
            outline();
            cairo_set_fill_rule(cr, f.subs.empty() ? CAIRO_FILL_RULE_WINDING : CAIRO_FILL_RULE_EVEN_ODD);   // s211: holes
            cairo_fill(cr);
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        }
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
            cairo_save(cr);
            cairo_set_line_width(cr, f.stroke_w);
            // s204: its ends, corners, dashes; under an arrow the line stops short.
            const auto dash = core::doc::dash_pattern(f);
            cairo_set_line_cap(cr, static_cast<cairo_line_cap_t>(f.dash == 2 ? 1 : std::clamp(f.cap, 0, 2)));
            cairo_set_line_join(cr, static_cast<cairo_line_join_t>(std::clamp(f.join, 0, 2)));
            if (!dash.empty()) cairo_set_dash(cr, dash.data(), static_cast<int>(dash.size()), 0);
            if (core::doc::has_outline(f)) {
                cairo_new_path(cr);
                for (const auto& o : core::doc::stroke_ops(f)) {
                    if (o.op == 'M') cairo_move_to(cr, o.v[0], o.v[1]);
                    else if (o.op == 'L') cairo_line_to(cr, o.v[0], o.v[1]);
                    else if (o.op == 'C') cairo_curve_to(cr, o.v[0], o.v[1], o.v[2], o.v[3], o.v[4], o.v[5]);
                    else cairo_close_path(cr);
                }
            } else {
                outline();
            }
            cairo_stroke(cr);
            for (const auto& poly : core::doc::end_shapes(f)) {
                for (std::size_t i = 0; i < poly.size(); ++i)
                    (i ? cairo_line_to : cairo_move_to)(cr, poly[i].x, poly[i].y);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        }
        if (turn) cairo_restore(cr);   // s228
        if (cut) cairo_restore(cr);   // s155
        if (!how.stop_after.empty() && fid == how.stop_after) return true;   // s193
    }
    return false;
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

MockupSize mockup_size(const core::Doc& d, const core::Doc::Id& master) {   // s149
    const auto* m = d.as<core::Master>(master);
    if (!m) return {};
    return {m->facing && m->folds.empty() ? 2 * m->w : m->w, m->h};   // a foldout: one sheet, its folds
}

void draw_master_mockup(cairo_t* cr, const core::Doc& d, const core::Doc::Id& master, FlowCache& flows,
                        const ImageSource& images, double x, double y, double w, double h) {   // s149
    const auto* m = d.as<core::Master>(master);
    if (!m || w < 2 || h < 2) return;
    const auto sz = mockup_size(d, master);
    const double s = std::min(w / sz.w, h / sz.h);
    const double ox = x + (w - sz.w * s) / 2, oy = y + (h - sz.h * s) / 2;
    const std::vector<core::Side> sides = m->facing && m->folds.empty() ? std::vector<core::Side>{core::Side::Left, core::Side::Right}
                                                                        : std::vector<core::Side>{core::Side::Any};
    // A page to draw it as, each side: the first that uses it on that side; else a copy of the
    // document with pages of it added (nothing kept), so its items and numbers show as they would.
    std::vector<core::Doc::Id> sample(sides.size());
    for (std::size_t k = 0; k < sides.size(); ++k)
        for (const auto& p : d.pages())
            if (d.as<core::Page>(p)->master == master && (sides[k] == core::Side::Any || d.side_of(p) == sides[k])) { sample[k] = p; break; }
    std::unique_ptr<core::Doc> scratch;
    std::unique_ptr<FlowCache> scratch_flows;
    if (std::any_of(sample.begin(), sample.end(), [](const auto& p) { return p.empty(); })) {
        scratch = std::make_unique<core::Doc>(d);
        scratch->set_on_change({});
        scratch->set_door({});
        core::Doc::Loading load(*scratch);
        double pos = 0;
        for (const auto& p : scratch->pages()) pos = std::max(pos, scratch->as<core::Page>(p)->pos);
        for (int n = 0; n < 3 && std::any_of(sample.begin(), sample.end(), [](const auto& p) { return p.empty(); }); ++n) {
            const std::string id = "page.mockup" + std::to_string(n);
            scratch->put(id, core::Page{pos + 1 + n, m->w, m->h, {}, master});
            for (std::size_t k = 0; k < sides.size(); ++k)
                if (sample[k].empty() && (sides[k] == core::Side::Any || scratch->side_of(id) == sides[k])) sample[k] = id;
        }
        scratch_flows = std::make_unique<FlowCache>();
    }
    const core::Doc& dd = scratch ? *scratch : d;
    FlowCache& ff = scratch ? *scratch_flows : flows;
    for (std::size_t k = 0; k < sides.size(); ++k) {
        const double px = ox + k * m->w * s;
        const bool left = sides[k] == core::Side::Left;
        // Its columns, faintly: where the text goes.
        const double ml = left ? m->margins.outside : m->margins.inside, mr = left ? m->margins.inside : m->margins.outside;
        const double tw = m->w - ml - mr, th = m->h - m->margins.top - m->margins.bottom;
        const int cols = std::max(1, m->columns);
        const double cw = (tw - m->gutter * (cols - 1)) / cols;
        cairo_save(cr);
        for (int c = 0; c < cols; ++c) {
            cairo_rectangle(cr, px + (ml + c * (cw + m->gutter)) * s, oy + m->margins.top * s, cw * s, th * s);
            cairo_set_source_rgba(cr, 0.45, 0.55, 0.95, 0.07);
            cairo_fill(cr);
        }
        // The baseline grid, when its lines are far enough apart to read.
        if (m->grid >= 1 && m->grid * s >= 2.5) {
            cairo_set_source_rgba(cr, 0.35, 0.6, 0.95, 0.22);
            cairo_set_line_width(cr, 0.5);
            for (double gy = m->margins.top + m->grid; gy <= m->h - m->margins.bottom + 0.01; gy += m->grid) {
                cairo_move_to(cr, px + ml * s, std::round(oy + gy * s) + 0.25);
                cairo_line_to(cr, px + (m->w - mr) * s, std::round(oy + gy * s) + 0.25);
            }
            cairo_stroke(cr);
        }
        // Its own items, as on that page (running heads, numbers filled in).
        cairo_save(cr);
        cairo_rectangle(cr, px, oy, m->w * s, m->h * s);
        cairo_clip(cr);
        cairo_translate(cr, px, oy);
        cairo_scale(cr, s, s);
        for (const auto& mc : core::master_chain(dd, sample[k])) draw_frames(cr, dd, mc, ff, images, nullptr, PageDraw{true}, sample[k]);
        cairo_restore(cr);
        // Its running heads, feet and numbers, readable at any size: each text frame outlined,
        // its words (as on that page) set no smaller than a label.
        cairo_save(cr);
        cairo_rectangle(cr, px, oy, m->w * s, m->h * s);
        cairo_clip(cr);
        for (const auto& mc : core::master_chain(dd, sample[k]))
            for (const auto& fid : dd.frames_on(mc)) {
                const auto& f = *dd.as<core::Frame>(fid);
                if (f.kind != core::FrameKind::Text || f.story.empty() || !core::master_item_on(dd, fid, sample[k])) continue;
                const auto* st = dd.as<core::Story>(f.story);
                if (!st || st->paras.empty()) continue;
                const auto& rs = ff.resolved(dd, f.story, sample[k]);
                const auto& para = (rs.any ? rs.text : *st).paras.front();
                std::string words;
                for (std::size_t i = 0; i < para.text.size();) {
                    if (static_cast<unsigned char>(para.text[i]) == 0xEE && i + 2 < para.text.size() &&
                        static_cast<unsigned char>(para.text[i + 1]) == 0x80) { i += 3; continue; }
                    words += para.text[i] == '\t' ? ' ' : para.text[i];
                    ++i;
                }
                const core::Rect r{px + (f.x + core::across_dx(dd, f, dd.side_of(sample[k]))) * s, oy + f.y * s, f.w * s, f.h * s};   // s155
                cairo_set_source_rgba(cr, 0.93, 0.47, 0.05, 0.75);   // the item: orange, dashed
                cairo_set_line_width(cr, 1);
                const double dash[] = {3, 2};
                cairo_set_dash(cr, dash, 2, 0);
                cairo_rectangle(cr, std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::max(2.0, std::round(r.w) - 1), std::max(2.0, std::round(r.h) - 1));
                cairo_stroke(cr);
                cairo_set_dash(cr, nullptr, 0, 0);
                if (words.find_first_not_of(' ') == std::string::npos) continue;
                const auto* sty = dd.as<core::Style>(para.style);
                const double size = std::max(8.0, (sty && sty->size > 0 ? sty->size : 10.0) * s);
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, size);
                cairo_text_extents_t te;
                cairo_text_extents(cr, words.c_str(), &te);
                const auto align = para.over.align ? *para.over.align : (sty ? sty->align : core::Align::Left);
                double tx = r.x + 1;
                if (align == core::Align::Center) tx = r.x + (r.w - te.x_advance) / 2;
                else if (align == core::Align::Right) tx = r.r() - te.x_advance - 1;
                const double ty = r.y + std::max(size, std::min(r.h, size * 1.15));
                cairo_set_source_rgba(cr, 0.75, 0.33, 0.0, 0.95);
                cairo_move_to(cr, tx, ty);
                cairo_show_text(cr, words.c_str());
            }
        cairo_restore(cr);
        // Margins (magenta), column edges (violet), guides (cyan) -- as the canvas draws them.
        cairo_set_line_width(cr, 1);
        cairo_set_source_rgba(cr, 0.85, 0.25, 0.65, 0.8);
        cairo_rectangle(cr, std::round(px + ml * s) + 0.5, std::round(oy + m->margins.top * s) + 0.5, std::round(tw * s), std::round(th * s));
        cairo_stroke(cr);
        if (cols > 1) {
            cairo_set_source_rgba(cr, 0.5, 0.35, 0.85, 0.7);
            for (int c = 1; c < cols; ++c) {
                for (const double gx : {ml + c * cw + (c - 1) * m->gutter, ml + c * (cw + m->gutter)}) {
                    cairo_move_to(cr, std::round(px + gx * s) + 0.5, oy + m->margins.top * s);
                    cairo_line_to(cr, std::round(px + gx * s) + 0.5, oy + (m->h - m->margins.bottom) * s);
                }
            }
            cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, 0.1, 0.7, 0.85, 0.7);
        for (const auto& gid : d.ids_of<core::Guide>()) {
            const auto& g = *d.as<core::Guide>(gid);
            if (g.owner != master) continue;
            if (g.vertical) { cairo_move_to(cr, std::round(px + g.pos * s) + 0.5, oy); cairo_line_to(cr, std::round(px + g.pos * s) + 0.5, oy + m->h * s); }
            else { cairo_move_to(cr, px, std::round(oy + g.pos * s) + 0.5); cairo_line_to(cr, px + m->w * s, std::round(oy + g.pos * s) + 0.5); }
        }
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (!m->folds.empty()) {   // a foldout's folds, dashed
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.2, 0.4, 0.9, 0.7);
        cairo_set_line_width(cr, 1);
        const double dash[] = {4, 3};
        cairo_set_dash(cr, dash, 2, 0);
        for (const double fx : m->folds) { cairo_move_to(cr, std::round(ox + fx * s) + 0.5, oy); cairo_line_to(cr, std::round(ox + fx * s) + 0.5, oy + m->h * s); }
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (sides.size() == 2) {   // the spine
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, std::round(ox + m->w * s) + 0.5, oy);
        cairo_line_to(cr, std::round(ox + m->w * s) + 0.5, oy + m->h * s);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

void draw_page(cairo_t* cr, const core::Doc& d, const core::Doc::Id& id, FlowCache& flows, const ImageSource& images,
               std::set<std::string>* missing, const PageDraw& how) {
    const core::Doc::Id on_page = d.as<core::Page>(id) ? id : core::Doc::Id{};
    for (const auto& m : core::master_chain(d, id))
        if (draw_frames(cr, d, m, flows, images, missing, how, on_page)) return;   // s193: stopped at its frame
    draw_frames(cr, d, id, flows, images, missing, how, on_page);
}

std::uint32_t sample_page(const core::Doc& d, const core::Doc::Id& id, FlowCache& flows, const ImageSource& images,
                          double x, double y, double across) {
    constexpr int N = 3;   // 3 x 3 samples across the square
    across = std::max(across, 1e-3);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, N, N);
    cairo_t* cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);   // paper
    cairo_paint(cr);
    const double k = N / across;
    cairo_scale(cr, k, k);
    cairo_translate(cr, -(x - across / 2), -(y - across / 2));
    draw_page(cr, d, id, flows, images, nullptr, {});
    cairo_destroy(cr);
    cairo_surface_flush(s);
    const unsigned char* data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    unsigned r = 0, g = 0, b = 0;
    for (int row = 0; row < N; ++row)
        for (int col = 0; col < N; ++col) {
            const std::uint32_t p = reinterpret_cast<const std::uint32_t*>(data + row * stride)[col];
            r += (p >> 16) & 0xff;
            g += (p >> 8) & 0xff;
            b += p & 0xff;
        }
    cairo_surface_destroy(s);
    const auto avg = [](unsigned t) { return static_cast<std::uint32_t>((t + N * N / 2) / (N * N)); };
    return (avg(r) << 16) | (avg(g) << 8) | avg(b);
}

PageKey page_key(const core::Doc& d, FlowCache& flows, const core::Doc::Id& id) {
    PageKey k;
    std::map<std::string, StoryFlow> ref_flows;   // s128: where references' targets are set
    const auto rec = [&](const std::string& rid) {
        if (const auto it = d.records().find(rid); it != d.records().end()) k.recs.emplace_back(rid, it->second);
    };
    for (const auto& l : d.ids_of<core::Layer>()) rec(l);   // s308: shown / hidden, its order -- what's drawn
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
std::string folded(const std::string& t) {   // s152: fully (a sharp s sorts as "ss")
    return core::doc::fold_text(t);
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
    std::map<Key, std::map<std::string, double>> top;   // s137: an entry's highest mark on each page
    std::map<Key, std::set<std::pair<std::string, std::string>>> linked;   // s139: page pairs one stretch runs across
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
                double y = 0;
                for (const auto& ft : flow.frames) {
                    for (const auto& l : ft.lines)
                        if (l.para == p && static_cast<int>(i) >= l.start && static_cast<int>(i) <= std::max(l.end, l.start + 1))
                            if (const auto* f = d.as<core::Frame>(ft.frame); f && d.as<core::Page>(f->owner)) {
                                page = f->owner;
                                y = f->y + l.y;
                                break;
                            }
                    if (!page.empty()) break;
                }
                // s139: a stretch -- every page its lines are set on, from the mark on.
                std::vector<std::string> span_pages;
                std::map<std::string, double> span_top;
                if (m.span != core::IndexSpan::Point && m.see.empty()) {
                    const int last = core::doc::index_span_end(d, st, p, m);
                    for (const auto& ft : flow.frames) {
                        const auto* f = d.as<core::Frame>(ft.frame);
                        if (!f || !d.as<core::Page>(f->owner)) continue;
                        for (const auto& l : ft.lines) {
                            if (l.para < p || l.para > last || (l.para == p && l.end < static_cast<int>(i))) continue;   // from the mark's line on
                            if (std::find(span_pages.begin(), span_pages.end(), f->owner) == span_pages.end()) span_pages.push_back(f->owner);
                            const double ly = f->y + l.y;
                            if (const auto it = span_top.find(f->owner); it == span_top.end() || ly < it->second) span_top[f->owner] = ly;
                        }
                    }
                    std::sort(span_pages.begin(), span_pages.end(), [&](const std::string& a, const std::string& b) { return d.page_index(a) < d.page_index(b); });
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
                if (!span_pages.empty()) {   // s139: the stretch's pages, linked in a row
                    auto& t = top[key(m.term, m.sub)];
                    for (std::size_t q = 0; q < span_pages.size(); ++q) {
                        const auto& pg = span_pages[q];
                        if (std::find(e.pages.begin(), e.pages.end(), pg) == e.pages.end()) e.pages.push_back(pg);
                        const double ty = pg == page ? y : span_top[pg];
                        if (const auto it = t.find(pg); it == t.end() || ty < it->second) t[pg] = ty;
                        if (q > 0 && d.page_index(pg) == d.page_index(span_pages[q - 1]) + 1)
                            linked[key(m.term, m.sub)].insert({span_pages[q - 1], pg});
                    }
                    continue;
                }
                if (!page.empty() && std::find(e.pages.begin(), e.pages.end(), page) == e.pages.end()) e.pages.push_back(page);
                if (!page.empty()) {   // s137
                    auto& t = top[key(m.term, m.sub)];
                    if (const auto it = t.find(page); it == t.end() || y < it->second) t[page] = y;
                }
            }
        }
    }
    std::vector<IndexEntry> out;
    for (auto& [kk, e] : by) {
        const auto& tops = top[kk];
        const auto& links = linked[kk];   // s139
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
            const auto run = [&](std::size_t q, std::string text) {   // s137
                const auto it = tops.find(e.pages[q]);
                e.runs.push_back({std::move(text), e.pages[q], it == tops.end() ? 0.0 : it->second});
            };
            // s139: two pages one stretch runs over are a range too ("4\u20135").
            const bool joined = b - a == 1 && links.count({e.pages[a], e.pages[b]}) > 0;
            if (b - a >= 2 || joined) run(a, e.folios[a] + "\u2013" + e.folios[b]);
            else for (std::size_t q = a; q <= b; ++q) run(q, e.folios[q]);
            a = b + 1;
        }
        for (const auto& r : e.runs) e.pages_text += (e.pages_text.empty() ? "" : ", ") + r.text;
        std::sort(e.see.begin(), e.see.end(), [](const std::string& x, const std::string& y) { return collated(x) < collated(y); });
        std::sort(e.also.begin(), e.also.end(), [](const std::string& x, const std::string& y) { return collated(x) < collated(y); });
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<PdfLink> index_links(const core::Doc& d, FlowCache& flows) {   // s137
    std::vector<PdfLink> out;
    std::vector<IndexEntry> all;
    bool have = false;
    for (const auto& lid : d.ids_of<core::GenList>()) {
        const auto& l = *d.as<core::GenList>(lid);
        if (l.kind != core::ListKind::Index || !l.numbers) continue;
        const auto* st = l.story.empty() ? nullptr : d.as<core::Story>(l.story);
        if (!st || l.built_entries < 0) continue;
        if (!have) { all = index_entries(d, flows); have = true; }
        const auto en = list_entries(d, flows, lid);
        const auto& flow = flows.get(d, l.story);
        // The written paragraphs after a title, matched in order: each entry line, its IndexEntry.
        auto p = static_cast<std::size_t>(core::doc::list_title_paras(*st, l.kind));
        std::size_t ie = 0;
        for (const auto& e : en) {
            if (p >= st->paras.size()) break;
            const auto& para = st->paras[p];
            const std::size_t at = p++;
            if (e.level == 0) continue;
            const auto& entry = all[ie++];
            if (e.folio.empty() || para.text != e.text + ", " + e.folio) continue;   // stale here: no links
            std::size_t pos = e.text.size() + 2;
            for (const auto& r : entry.runs) {
                if (para.text.compare(pos, r.text.size(), r.text) != 0) break;
                for (const auto& ft : flow.frames) {
                    const auto* f = d.as<core::Frame>(ft.frame);
                    if (!f || f->rot != 0 || !d.as<core::Page>(f->owner)) continue;
                    const core::TextPos a{static_cast<int>(at), static_cast<int>(pos)}, b{static_cast<int>(at), static_cast<int>(pos + r.text.size())};
                    for (const auto& rc : selection_rects(d, l.story, flow, ft.frame, a, b))
                        out.push_back({f->owner, {f->x + rc.x, f->y + rc.y, rc.w, rc.h}, r.page, std::max(0.0, r.y - 4)});
                }
                pos += r.text.size() + 2;
            }
        }
    }
    return out;
}

namespace {
// s131 / s303: where an anchor points -- its page and how far down; page "" none.
struct AnchorDest { std::string page; double y = 0; };
AnchorDest anchor_dest(const core::Doc& d, FlowCache& flows, const std::string& id) {
    AnchorDest dst;
    const auto at = core::doc::find_anchor(d, id);
    if (at.story.empty()) return dst;
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
    return dst;
}
}  // namespace

std::vector<PdfLink> hyperlink_links(const core::Doc& d, FlowCache& flows) {   // s303
    std::vector<PdfLink> out;
    std::map<std::string, AnchorDest> dests;
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto spans = core::doc::links_in(d, sid);
        if (spans.empty()) continue;
        const auto& rs = flows.resolved(d, sid, {});
        const core::Story* text = rs.any ? &rs.text : d.as<core::Story>(sid);
        const StoryFlow& flow = rs.any ? rs.flow : flows.get(d, sid);
        // the resolved words' own runs (a reference's words widen a paragraph) -- their links' spans
        std::vector<core::doc::LinkSpan> here;
        for (std::size_t p = 0; p < text->paras.size(); ++p)
            for (const auto& r : text->paras[p].runs) {
                if (r.local.link.empty() || r.end <= r.start) continue;
                if (!here.empty() && here.back().para == static_cast<int>(p) && here.back().to == r.start && here.back().target == r.local.link)
                    here.back().to = r.end;
                else
                    here.push_back({sid, static_cast<int>(p), r.start, r.end, r.local.link, {}, false});
            }
        for (const auto& span : here) {
            PdfLink proto;
            if (span.target[0] == '#') {
                const auto id = span.target.substr(1);
                auto it = dests.find(id);
                if (it == dests.end()) it = dests.emplace(id, anchor_dest(d, flows, id)).first;
                if (it->second.page.empty()) continue;
                proto.to = it->second.page;
                proto.to_y = it->second.y;
            } else {
                proto.uri = span.target;
            }
            for (const auto& ft : flow.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (!f || f->rot != 0 || !d.as<core::Page>(f->owner)) continue;
                const bool on = std::any_of(ft.lines.begin(), ft.lines.end(), [&](const Line& l) {
                    return l.para == span.para && l.start < span.to && l.end > span.from;
                });
                if (!on) continue;
                for (const auto& r : selection_rects(d, sid, flow, ft.frame, {span.para, span.from}, {span.para, span.to}, rs.any ? &rs.text : nullptr)) {
                    PdfLink l = proto;
                    l.page = f->owner;
                    l.box = {f->x + r.x, f->y + r.y, r.w, r.h};
                    out.push_back(std::move(l));
                }
            }
        }
    }
    return out;
}

std::vector<TextMark> text_marks(const core::Doc& d, FlowCache& flows, const std::string& frame, bool spelling, bool links) {   // s309
    std::vector<TextMark> out;
    const auto* f = d.as<core::Frame>(frame);
    if (!f || f->kind != core::FrameKind::Text || f->story.empty() || !d.as<core::Story>(f->story)) return out;
    const auto& sid = f->story;
    const auto& rs = flows.resolved(d, sid, {});
    const core::Story* text = rs.any ? &rs.text : d.as<core::Story>(sid);
    const StoryFlow& flow = rs.any ? rs.flow : flows.get(d, sid);
    const FrameText* ft = nullptr;
    for (const auto& x : flow.frames) if (x.frame == frame) { ft = &x; break; }
    if (!ft || ft->lines.empty()) return out;
    std::set<int> paras;
    for (const auto& l : ft->lines) paras.insert(l.para);
    const auto add = [&](TextMark::Kind k, int p, int a, int b) {
        for (const auto& r : selection_rects(d, sid, flow, frame, {p, a}, {p, b}, rs.any ? &rs.text : nullptr))
            if (r.w > 0) out.push_back({k, r, p, a, b});
    };
    for (int p : paras) {
        if (p < 0 || p >= static_cast<int>(text->paras.size())) continue;
        const auto& para = text->paras[static_cast<std::size_t>(p)];
        if (spelling && core::spell::loaded())
            for (const auto& w : core::spell::para_misses(para.text, p)) add(TextMark::Misspelled, p, w.from, w.to);
        if (links) {
            int a = -1, b = -1;
            std::string at;
            for (const auto& r : para.runs) {
                if (r.local.link.empty()) continue;
                if (a >= 0 && r.start == b && r.local.link == at) { b = r.end; continue; }
                if (a >= 0) add(TextMark::Link, p, a, b);
                a = r.start; b = r.end; at = r.local.link;
            }
            if (a >= 0) add(TextMark::Link, p, a, b);
        }
    }
    return out;
}

std::vector<PdfLink> xref_links(const core::Doc& d, FlowCache& flows) {   // s131
    std::vector<PdfLink> out;
    std::map<std::string, AnchorDest> dests;
    const auto dest_of = [&](const std::string& id) -> AnchorDest {
        if (const auto it = dests.find(id); it != dests.end()) return it->second;
        return dests.emplace(id, anchor_dest(d, flows, id)).first->second;
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
    if (l && l->kind == core::ListKind::Bibliography) {   // s140: the sources -- each paragraph in its styles (its own story's too), once, sorted
        std::map<std::string, core::doc::ListEntry> by;   // the sort key -> the entry
        // s141: its own story first -- a source there keeps its anchors (citations point at
        // them); a copy from elsewhere comes without (one place, one id).
        const auto own = core::doc::list_story(d, list);
        std::vector<std::string> order;
        if (!own.empty()) order.push_back(own);
        for (const auto& sid : d.ids_of<core::Story>()) if (sid != own) order.push_back(sid);
        for (const auto& sid : order) {
            for (auto para : d.as<core::Story>(sid)->paras) {
                if (std::find(l->from_styles.begin(), l->from_styles.end(), para.style) == l->from_styles.end()) continue;
                // s201 (W1): a copy built here before -- its original (if still there) is gathered instead.
                if (sid == own && !para.copy_of.empty()) continue;
                if (sid != own) {
                    for (auto& a : para.anchors) a.clear();
                    para.copy_of = sid;
                }
                std::string w;
                for (std::size_t i = 0; i < para.text.size();) {
                    if (static_cast<unsigned char>(para.text[i]) == 0xEE && i + 2 < para.text.size() &&
                        static_cast<unsigned char>(para.text[i + 1]) == 0x80) { i += 3; continue; }
                    w += para.text[i] == '\t' ? ' ' : para.text[i];
                    ++i;
                }
                w = trimmed(w);
                if (w.empty()) continue;
                // Sorted as a reader looks one up: an opening "The", "A" or "An" set aside.
                std::string k = collated(w);
                for (const char* art : {"the ", "a ", "an "})
                    if (k.rfind(art, 0) == 0 && k.size() > std::strlen(art)) { k = k.substr(std::strlen(art)); break; }
                by.emplace(k + '\x01' + folded(w), core::doc::ListEntry{w, "", 1, para});   // the same words twice: once (the first, whole)
            }
        }
        for (auto& [k, e] : by) out.push_back(std::move(e));
        if (l->cite_order) {   // s142: as first cited -- cited sources by their first citation in reading order, the rest after (alphabetical)
            struct At { int page, story, para, byte; bool operator<(const At& o) const { return std::tie(page, story, para, byte) < std::tie(o.page, o.story, o.para, o.byte); } };
            std::map<std::string, At> first;   // anchor id -> its first citation
            int sn = 0;
            for (const auto& sid : d.ids_of<core::Story>()) {
                ++sn;
                const auto& st = *d.as<core::Story>(sid);
                if (std::none_of(st.paras.begin(), st.paras.end(), [](const core::Para& p) { return !p.refs.empty(); })) continue;
                const auto& flow = flows.get(d, sid);
                for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                    const auto& para = st.paras[static_cast<std::size_t>(p)];
                    int k = 0;
                    for (auto i = para.text.find(core::kMarkRef); i != std::string::npos && k < static_cast<int>(para.refs.size());
                         i = para.text.find(core::kMarkRef, i + 3), ++k) {
                        const auto& r = para.refs[static_cast<std::size_t>(k)];
                        if (r.show != core::RefShow::Cite || r.target.empty()) continue;
                        int page = 1 << 30;   // not on a page: after those that are
                        for (const auto& ft : flow.frames)
                            for (const auto& ln : ft.lines)
                                if (ln.para == p && static_cast<int>(i) >= ln.start && static_cast<int>(i) <= std::max(ln.end, ln.start + 1))
                                    if (const auto* f = d.as<core::Frame>(ft.frame); f && d.as<core::Page>(f->owner))
                                        page = std::min(page, d.page_index(f->owner));
                        const At at{page, sn, p, static_cast<int>(i)};
                        std::vector<std::string> ids{r.target};   // s143: and its other sources
                        ids.insert(ids.end(), r.more.begin(), r.more.end());
                        for (const auto& id : ids)
                            if (const auto it = first.find(id); !id.empty() && (it == first.end() || at < it->second)) first[id] = at;
                    }
                }
            }
            const auto rank = [&](const core::doc::ListEntry& e) {
                At best{1 << 30, 1 << 30, 0, 0};
                bool cited = false;
                if (e.para)
                    for (const auto& id : e.para->anchors)
                        if (const auto it = first.find(id); !id.empty() && it != first.end()) { cited = true; best = std::min(best, it->second); }
                return std::make_tuple(!cited, best.page, best.story, best.para, best.byte);
            };
            std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
        }
        return out;
    }
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

namespace {
// s087: the entries that hold once written -- written on a copy of `d`, looked
// at again, until the folios stop moving (at most a few rounds).
std::vector<core::doc::ListEntry> settled_entries(const core::Doc& d, const std::string& list, const std::function<void(int round)>& tick) {
    std::vector<core::doc::ListEntry> entries;
    {
        FlowCache f0;
        entries = list_entries(d, f0, list);
    }
    for (int round = 0; round < 4; ++round) {   // settle: write on a copy, look again
        if (tick) tick(round + 1);
        core::Doc scratch;
        {
            core::Doc::Loading load(scratch);   // s258: records only (no door, no change hook of d's)
            for (const auto& [id, r] : d.records()) scratch.put(id, r);
        }
        core::DocJournal sj{scratch};
        scratch.set_door([&sj] { return sj.open(); });
        core::doc::build_list(sj, scratch, list, entries);
        FlowCache fs;
        auto again = list_entries(scratch, fs, list);
        if (again == entries) break;
        entries = std::move(again);
    }
    return entries;
}
}  // namespace

bool build_list(core::DocJournal& j, core::Doc& d, const std::string& list) {   // s087
    if (!d.as<core::GenList>(list)) return false;
    return core::doc::build_list(j, d, list, settled_entries(d, list, {}));
}

std::vector<ListBuild> settle_lists(core::Doc& scratch, const std::vector<std::string>& lists, const ListProgress& progress) {   // s258
    std::vector<ListBuild> out;
    core::DocJournal sj{scratch};
    scratch.set_door([&sj] { return sj.open(); });
    const double n = static_cast<double>(std::max<std::size_t>(1, lists.size()));
    for (std::size_t i = 0; i < lists.size(); ++i) {
        const auto* gl = scratch.as<core::GenList>(lists[i]);
        if (!gl) continue;
        const std::string name = gl->name.empty() ? "the list" : "\u201c" + gl->name + "\u201d";
        if (progress) progress(static_cast<double>(i) / n, "Gathering " + name + "\u2026");
        auto entries = settled_entries(scratch, lists[i], [&](int round) {
            if (progress) progress((static_cast<double>(i) + round / 5.0) / n, "Placing " + name + " (pass " + std::to_string(round) + ")\u2026");
        });
        core::doc::build_list(sj, scratch, lists[i], entries);   // the next list sees this one written
        out.push_back({lists[i], std::move(entries)});
    }
    scratch.set_door({});
    if (progress) progress(1.0, "Writing\u2026");
    return out;
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
            // s170: an AUTOMATIC deeper level -- build_list sets it in the style
            // named for level 1's ("Contents Entry 2"), found by name, else made.
            // Written in another, or none of that name yet: out of date.
            const core::Style* base = nullptr;
            for (std::size_t i = 0; i < en.size() && !base; ++i)
                if (en[i].level <= 1 && t0 + i < st->paras.size()) base = d.as<core::Style>(st->paras[t0 + i].style);
            for (std::size_t i = 0; i < en.size() && why.empty() && base; ++i) {
                const int lv = en[i].level;
                if (lv < 2 || t0 + i >= st->paras.size()) continue;
                if (static_cast<std::size_t>(lv) <= l.entry_styles.size() && !l.entry_styles[static_cast<std::size_t>(lv - 1)].empty()) continue;
                const std::string name = base->name + " " + std::to_string(lv);
                core::Doc::Id found;
                for (const auto& sid : d.ids_of<core::Style>())
                    if (const auto* x = d.as<core::Style>(sid); x->kind == core::StyleKind::Paragraph && x->name == name) { found = sid; break; }
                if (found.empty()) why = "No " + name + " style yet";
                else if (st->paras[t0 + i].style != found) why = "Entries not in " + name;
            }
        }
        if (!why.empty()) out.push_back({lid, why});
    }
    // s135: an index -- its written paragraphs against what the marks make now.
    for (const auto& lid : d.ids_of<core::GenList>()) {
        const auto& l = *d.as<core::GenList>(lid);
        if (l.kind != core::ListKind::Index && l.kind != core::ListKind::Bibliography) continue;   // s140: a bibliography too
        const auto* st = l.story.empty() ? nullptr : d.as<core::Story>(l.story);
        if (!st && l.placed_on.empty()) continue;
        // s140: a bibliography never built is not out of date (the inspector says Never) -- typed by hand till then.
        if (l.kind == core::ListKind::Bibliography && (!st || l.built_entries < 0)) continue;
        if (!st || l.built_entries < 0) { out.push_back({lid, "Not built yet"}); continue; }
        const auto en = list_entries(d, flows, lid);
        std::vector<std::string> want, had;
        for (const auto& e : en) want.push_back(e.level == 0 || !l.numbers || e.folio.empty() ? e.text : e.text + ", " + e.folio);
        for (auto k = static_cast<std::size_t>(core::doc::list_title_paras(*st, l.kind)); k < st->paras.size(); ++k) {
            // s141: as read -- the marks out (a cited source holds an anchor), tabs as spaces, trimmed as the entries are.
            std::string t;
            const auto& raw = st->paras[k].text;
            for (std::size_t i = 0; i < raw.size();) {
                if (static_cast<unsigned char>(raw[i]) == 0xEE && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80) { i += 3; continue; }
                t += raw[i];
                ++i;
            }
            had.push_back(l.kind == core::ListKind::Bibliography ? trimmed(t) : t);
        }
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
        if (!why.empty() && l.kind == core::ListKind::Bibliography) {   // s142: the same sources, another order
            auto a1 = want, b1 = had;
            std::sort(a1.begin(), a1.end());
            std::sort(b1.begin(), b1.end());
            if (a1 == b1) why = l.cite_order ? "Not in the order first cited" : "Not in alphabetical order";
        }
        if (why.empty()) {   // s138: a level whose style was picked -- written in it?
            const auto t0 = static_cast<std::size_t>(core::doc::list_title_paras(*st, l.kind));
            for (std::size_t i = 0; i < en.size() && why.empty(); ++i) {
                const auto lv = static_cast<std::size_t>(en[i].level);
                if (lv == 0 || lv > l.entry_styles.size() || l.entry_styles[lv - 1].empty()) continue;
                const auto* want_style = d.as<core::Style>(l.entry_styles[lv - 1]);
                if (want_style && t0 + i < st->paras.size() && st->paras[t0 + i].style != l.entry_styles[lv - 1])
                    why = (lv == 1 ? "Entries not in " : "Sub-entries not in ") + want_style->name;
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
        if (opt.bleed) {   // s147: each master its own -- one figure when they agree
        double lo = 1e9, hi = 0;
        for (const auto& p : pages) { const double b = core::bleed_of(d, p); lo = std::min(lo, b); hi = std::max(hi, b); }
        if (hi > 0) s.size += lo == hi ? " + " + core::length_text(hi, units) + " bleed" : std::string(" + bleed by master");
    }
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
        for (const auto& l : hyperlink_links(d, flows))   // s303
            if (sheet_of(sheets, l.page).sheet >= 0 && (!l.uri.empty() || sheet_of(sheets, l.to).sheet >= 0)) ++s.hyperlinks;
        for (const auto& l : index_links(d, flows))   // s137
            if (sheet_of(sheets, l.page).sheet >= 0 && sheet_of(sheets, l.to).sheet >= 0) ++s.index_links;
    }
    const auto n = [](int k, const char* one, const char* many) {
        return (k == 0 ? std::string("No") : std::to_string(k)) + " " + (k == 1 ? one : many);
    };
    s.nav = n(s.bookmarks, "bookmark", "bookmarks") + " \u00b7 " +
            (x ? std::string("contents links left out (PDF/X)") : n(s.links, "contents link", "contents links"));
    if (!x && s.note_links > 0) s.nav += " \u00b7 " + n(s.note_links, "note link", "note links");   // s123
    if (!x && s.xref_links > 0) s.nav += " \u00b7 " + n(s.xref_links, "cross-reference link", "cross-reference links");   // s131
    if (!x && s.hyperlinks > 0) s.nav += " \u00b7 " + n(s.hyperlinks, "hyperlink", "hyperlinks");   // s303
    if (!x && s.index_links > 0) s.nav += " \u00b7 " + n(s.index_links, "index link", "index links");   // s137
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
    const auto sheets = pdf_sheets(d, pages, opt.spreads);   // s084
    // s147: each sheet's bleed -- its pages' masters' (the most of them).
    std::vector<double> sbleed;
    for (const auto& sh : sheets) {
        double b = 0;
        if (opt.bleed) for (const auto& p : sh.pages) b = std::max(b, core::bleed_of(d, p));
        sbleed.push_back(b);
    }
    const auto off_of = [&](int i) { return sbleed[static_cast<std::size_t>(std::max(0, i))] + (opt.marks ? kSlug : 0); };
    cairo_surface_t* surf = cairo_pdf_surface_create(path.c_str(), sheets.front().w + 2 * off_of(0), sheets.front().h + 2 * off_of(0));
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
    for (auto& l : hyperlink_links(d, flows)) links.push_back(std::move(l));   // s303: and the words' own links
    for (auto& l : index_links(d, flows)) links.push_back(std::move(l));   // s137: and the index'
    std::set<std::string> missing;
    int n = 0;
    for (const auto& sh : sheets) {
        const int si = static_cast<int>(&sh - sheets.data());
        const double bleed = sbleed[static_cast<std::size_t>(si)], off = off_of(si);   // s147: this sheet's
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
                if (!l.uri.empty()) {   // s303: out of the document
                    std::string u;
                    for (char ch : l.uri) { if (ch == '\'' || ch == '\\') u += '\\'; u += ch; }
                    char box[120];
                    std::snprintf(box, sizeof box, "rect=[%g %g %g %g] ", off + sh.x[k] + l.box.x, off + l.box.y, l.box.w, l.box.h);
                    const std::string attr = std::string(box) + "uri='" + u + "'";
                    cairo_tag_begin(cr, CAIRO_TAG_LINK, attr.c_str());
                    cairo_tag_end(cr, CAIRO_TAG_LINK);
                    continue;
                }
                const auto to = sheet_of(sheets, l.to);
                if (to.sheet < 0) continue;
                char attr[160];
                std::snprintf(attr, sizeof attr, "rect=[%g %g %g %g] page=%d pos=[%g %g]", off + sh.x[k] + l.box.x, off + l.box.y,
                              l.box.w, l.box.h, to.sheet + 1, off_of(to.sheet) + to.x, off_of(to.sheet) + l.to_y);
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
            std::snprintf(dest, sizeof dest, "page=%d pos=[%g %g]", at.sheet + 1, off_of(at.sheet) + at.x, off_of(at.sheet) + b.y);
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


// ── s163: a style, greeked ──────────────────────────────────────────────────
namespace {
std::string greek(int words, int from = 0) {
    static const char* lorem[] = {"lorem", "ipsum", "dolor", "sit", "amet,", "consectetur", "adipiscing", "elit,", "sed", "do",
                                  "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua.", "Ut",
                                  "enim", "ad", "minim", "veniam,", "quis", "nostrud", "exercitation", "ullamco", "laboris",
                                  "nisi", "ut", "aliquip", "ex", "ea", "commodo", "consequat.", "Duis", "aute", "irure", "dolor",
                                  "in", "reprehenderit", "in", "voluptate", "velit", "esse", "cillum", "dolore", "eu", "fugiat",
                                  "nulla", "pariatur."};
    const int n = static_cast<int>(sizeof lorem / sizeof lorem[0]);
    std::string out;
    for (int i = 0; i < words; ++i) {
        std::string w = lorem[(from + i) % n];
        if (i == 0 || (!out.empty() && out.back() == '.')) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
        out += (i ? " " : "") + w;
    }
    if (!out.empty() && out.back() != '.') {
        if (out.back() == ',') out.pop_back();
        out += '.';
    }
    return out;
}
}  // namespace

core::Doc style_specimen_doc(const core::Doc& d, const core::Doc::Id& style, bool in_use, double w, double h,
                             const StyleOver* over) {
    core::Doc s;
    core::Doc::Loading load(s);
    core::Settings st = d.settings();
    s.put("doc", st);
    for (const auto& id : d.ids_of<core::Style>()) {
        core::Style x = *d.as<core::Style>(id);
        if (over) if (const auto it = over->find(id); it != over->end()) x = it->second;   // s164: as it would be
        x.start = core::ParaStart::Anywhere;   // one page: a "next page" start would send it nowhere
        s.put(id, x);
    }
    for (const auto& id : d.ids_of<core::Swatch>()) s.put(id, *d.as<core::Swatch>(id));
    core::Page pg;
    pg.pos = 1;
    pg.w = w;
    pg.h = h;
    s.put("page.1", pg);
    const auto* me = d.as<core::Style>(style);
    // The document's own: its body (the paragraph style used most) and its
    // heading (the biggest paragraph style), neither the one shown.
    core::Doc::Id body, head;
    double biggest = -1;
    {   // the body: the most TEXT set in it (a folio is on every page, but short), not a list
        std::map<core::Doc::Id, std::size_t> bytes;
        for (const auto& sid : d.ids_of<core::Story>())
            for (const auto& pp : d.as<core::Story>(sid)->paras) bytes[pp.style] += pp.text.size();
        std::size_t most = 0;
        for (const auto& id : d.ids_of<core::Style>()) {
            const auto* x = d.as<core::Style>(id);
            if (x->kind != core::StyleKind::Paragraph || id == style || x->list.type != core::ListType::None) continue;
            if (body.empty() || bytes[id] > most) { most = bytes[id]; body = id; }
        }
    }
    for (const auto& id : d.ids_of<core::Style>()) {
        const auto* x = d.as<core::Style>(id);
        if (x->kind != core::StyleKind::Paragraph || id == style || id == body) continue;
        if (x->size > biggest) { biggest = x->size; head = id; }
    }
    if (head.empty()) head = body;
    core::Story story{"Specimen", {}};
    const auto para = [&](const core::Doc::Id& sid, std::string text) {
        core::Para p;
        p.style = sid;
        p.text = std::move(text);
        story.paras.push_back(std::move(p));
    };
    const bool is_char = me && me->kind == core::StyleKind::Character;
    const auto char_para = [&](int words, int from) {   // body words, some in the character style
        const std::string a = greek(words / 3, from), b = greek(words / 3, from + 9), c = greek(words / 3, from + 17);
        core::Para p;
        p.style = body;
        p.text = a.substr(0, a.size() - 1) + " ";
        const int s0 = static_cast<int>(p.text.size());
        std::string mid = b.substr(0, b.size() - 1);
        mid[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(mid[0])));
        p.text += mid;
        const int s1 = static_cast<int>(p.text.size());
        std::string rest = c;
        rest[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
        p.text += " " + rest;
        p.runs.push_back({s0, s1, style, {}});
        story.paras.push_back(std::move(p));
    };
    const auto* bs = d.as<core::Style>(body);
    const bool heading_like = me && bs && me->kind == core::StyleKind::Paragraph && me->size > bs->size + 1.5;
    if (!in_use) {
        if (is_char) { char_para(48, 0); char_para(36, 20); }
        else if (heading_like) { para(style, greek(3, 0)); para(style, greek(5, 12)); para(style, greek(4, 30)); }
        else { para(style, greek(44, 0)); para(style, greek(34, 12)); para(style, greek(26, 30)); }
    } else if (is_char) {
        para(head, greek(3, 0));
        para(body, greek(40, 4));
        char_para(45, 22);
        para(body, greek(38, 40));
    } else {
        const auto* b = d.as<core::Style>(body);
        const bool heading = me && b && me->size > b->size + 1.5;
        const core::Doc::Id after = me && d.as<core::Style>(me->next) ? me->next : body;
        if (heading) {
            para(body, greek(30, 33));
            para(style, greek(3, 5));
            para(after, greek(42, 9));
            para(body, greek(36, 21));
            para(body, greek(30, 2));
        } else {
            para(head, greek(3, 0));
            para(style, greek(40, 4));
            para(style == body ? style : after, greek(36, 19));
            para(style, greek(30, 37));
            para(body.empty() ? style : body, greek(24, 11));
        }
    }
    s.put("story.1", story);
    core::Frame f;
    f.owner = "page.1";
    f.kind = core::FrameKind::Text;
    const double m = std::min(w, h) * 0.09;
    f.x = m;
    f.y = m;
    f.w = w - 2 * m;
    f.h = h - 2 * m;
    f.story = "story.1";
    // s166: a style spanning the columns shows it -- in use, two columns.
    if (const auto* x = s.as<core::Style>(style); in_use && x && x->span) f.columns = 2;
    s.put("frame.1", f);
    return s;
}

void draw_style_specimen(cairo_t* cr, const core::Doc& d, const core::Doc::Id& style, double W, double H,
                         const StyleOver* over) {
    if (!d.as<core::Style>(style) || W < 40 || H < 40) return;
    // "On its own": a half page; "In use": a page at the document's trim.
    const double tw = d.settings().trim_w > 0 ? d.settings().trim_w : 432, th = d.settings().trim_h > 0 ? d.settings().trim_h : 648;
    struct Panel { const char* label; double w, h; bool use; };
    const Panel panels[] = {{"On its own", tw, th * 0.5, false}, {"In use", tw, th, true}};
    const double gap = 36, pad = 28, top = 26;
    const double sc = std::min((W - 2 * pad - gap) / (panels[0].w + panels[1].w), (H - 2 * pad - top) / th);
    if (sc <= 0) return;
    double x = (W - (panels[0].w + panels[1].w) * sc - gap) / 2;
    const double y0 = (H - th * sc - top) / 2 + top;
    for (const auto& p : panels) {
        const core::Doc sd = style_specimen_doc(d, style, p.use, p.w, p.h, over);
        FlowCache flows;
        const double y = p.use ? y0 : y0;
        // the label over it
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.45, 0.45, 0.5, 1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, x, y - 9);
        cairo_show_text(cr, p.label);
        // paper, a soft shadow
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
        cairo_rectangle(cr, x + 2, y + 3, p.w * sc, p.h * sc);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, x, y, p.w * sc, p.h * sc);
        cairo_fill(cr);
        cairo_rectangle(cr, x, y, p.w * sc, p.h * sc);
        cairo_clip(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, sc, sc);
        draw_page(cr, sd, "page.1", flows, {}, nullptr, {});
        // In use: the style's own paragraphs marked beside them.
        if (p.use) {
            const auto& fl = flows.get(sd, "story.1");
            const auto* st = sd.as<core::Story>("story.1");
            const auto* fr = sd.as<core::Frame>("frame.1");
            const bool is_char = d.as<core::Style>(style)->kind == core::StyleKind::Character;
            cairo_set_source_rgba(cr, 0.85, 0.5, 0.1, 0.9);
            for (const auto& ft : fl.frames)
                for (const auto& l : ft.lines) {
                    if (l.para < 0 || l.para >= static_cast<int>(st->paras.size())) continue;
                    const auto& pp = st->paras[static_cast<std::size_t>(l.para)];
                    bool mine = pp.style == style;
                    if (is_char)
                        for (const auto& r : pp.runs) mine = mine || (r.style == style && r.start < l.end && r.end > l.start);
                    if (!mine) continue;
                    cairo_rectangle(cr, fr->x - 9, fr->y + l.y, 2.5, l.h);
                    cairo_fill(cr);
                }
        }
        cairo_restore(cr);
        x += p.w * sc + gap;
    }
}

}  // namespace composix::layout
