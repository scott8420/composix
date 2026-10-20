#include <functional>
#include "views/PagePaint.hpp"
#include "views/Images.hpp"

#include <plate/plate.hpp>

#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// PagePaint.cpp -- the canvas's page drawing, moved out of Canvas (s104) so
// it can run on the page worker. The body is Canvas::draw_page / draw_frame
// as they were, reading the look (CanvasLook) instead of the widget.

namespace composix::views {

using core::Rect;

namespace {

constexpr double kAcc[3] = {0.21, 0.52, 0.89};
constexpr std::uint32_t kTints[] = {0x8fa37a, 0xc98f5a, 0x8a7fb0, 0x6b9e9a, 0x5b7fa6, 0x9a8f88};

void rgb(const Cairo::RefPtr<Cairo::Context>& cr, std::uint32_t c, double a = 1) {
    cr->set_source_rgba(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, a);
}

std::uint32_t tint_for(const std::string& id) {
    std::size_t h = 0;
    for (char c : id) h = h * 31 + static_cast<unsigned char>(c);
    return kTints[h % std::size(kTints)];
}

// As Canvas::port_rect, from the frame's screen rectangle.
Rect port_rect(const Rect& r, bool out) {
    if (r.w <= 24 || r.h <= 24) return {};
    const double ps = 7;
    return out ? Rect{std::round(r.r()) + 3.5, std::round(r.b()) - ps - 10.5, ps, ps}
               : Rect{std::round(r.x) - ps - 3.5, std::round(r.y) + 10.5, ps, ps};
}

void paint_frame(const Cairo::RefPtr<Cairo::Context>& cr, const CanvasLook& P, const core::Frame& f, double ox, double oy,
                 bool from_master, const std::string& id, const std::string& on_page);

}  // namespace

void paint_canvas_page(const Cairo::RefPtr<Cairo::Context>& cr, const CanvasLook& P, const core::Placed& p) {
    const double s = P.s;
    const Rect sr = P.to_screen(p.r);

    // Shadow and paper (paper is paper-white in both appearances).
    for (int i = 4; i >= 1; --i) {
        cr->set_source_rgba(0, 0, 0, 0.04);
        cr->rectangle(sr.x - i * 0.5, sr.y + i, sr.w + i, sr.h + i);
        cr->fill();
    }
    cr->set_source_rgb(1, 1, 1);
    cr->rectangle(sr.x, sr.y, sr.w, sr.h);
    cr->fill();

    // Bleed: a thin red line outside the trim.
    // s027: in a spread the bleed runs round the spread -- none at the spine.
    const double bleed = core::bleed_of(P.doc, p.id) * s;   // s147: its master's
    if (bleed > 1) {
        const bool at_left = P.spine_left, at_right = P.spine_right;   // a page touching this one at the spine
        const double x0 = std::round(sr.x - (at_left ? 0 : bleed)) + 0.5, x1 = std::round(sr.r() + (at_right ? 0 : bleed)) + 0.5;
        const double y0 = std::round(sr.y - bleed) + 0.5, y1 = std::round(sr.b() + bleed) + 0.5;
        cr->set_source_rgba(0.85, 0.2, 0.2, 0.55);
        cr->set_line_width(1);
        cr->move_to(x0, y0); cr->line_to(x1, y0);
        cr->move_to(x0, y1); cr->line_to(x1, y1);
        if (!at_left) { cr->move_to(x0, y0); cr->line_to(x0, y1); }
        if (!at_right) { cr->move_to(x1, y0); cr->line_to(x1, y1); }
        cr->stroke();
        if (at_right) {   // the spine: a quiet line where the pages meet
            cr->set_source_rgba(0, 0, 0, 0.18);
            cr->move_to(std::round(sr.r()) + 0.5, sr.y);
            cr->line_to(std::round(sr.r()) + 0.5, sr.b());
            cr->stroke();
        }
    }

    // Folds: dashed, from the page's own fold positions.
    const std::vector<double>* folds = nullptr;
    if (const auto* pg = P.doc.as<core::Page>(p.id)) folds = &pg->folds;
    else if (const auto* m = P.doc.as<core::Master>(p.id)) folds = &m->folds;
    if (folds && !folds->empty()) {
        cr->set_source_rgba(0.2, 0.4, 0.9, 0.75);
        cr->set_line_width(1);
        std::vector<double> dash{5, 4};
        cr->set_dash(dash, 0);
        for (double fx : *folds) {
            const double x = std::round(sr.x + fx * s) + 0.5;
            cr->move_to(x, sr.y);
            cr->line_to(x, sr.b());
        }
        cr->stroke();
        cr->unset_dash();
    }

    // Master items first (dimmed: they are not picked on a page), then the
    // page's own frames, back to front.
    // s155: an item across the spine -- shifted for a right page, cut at the spine.
    const core::Side sd = core::side_for(P.doc, p.id, p.side);
    const auto across = [&](const core::Frame& f, bool from_master, const std::string& fid) {
        const double dx = core::across_dx(P.doc, f, sd);
        if (!f.across || sd == core::Side::Any) { paint_frame(cr, P, f, p.r.x, p.r.y, from_master, fid, p.id); return; }
        const Rect page = P.to_screen(p.r);
        cr->save();
        if (sd == core::Side::Left) cr->rectangle(page.x - 1e5, page.y - 1e5, 1e5 + page.w, 2e5);
        else                        cr->rectangle(page.x, page.y - 1e5, 1e5, 2e5);
        cr->clip();
        paint_frame(cr, P, f, p.r.x + dx, p.r.y, from_master, fid, p.id);
        cr->restore();
    };
    if (!p.master)
        for (const auto& m : core::master_chain(P.doc, p.id))
            for (const auto& fid : P.doc.frames_on(m))
                if (core::master_item_on(P.doc, fid, p.id))   // s025: its side, not overridden here
                    across(*P.doc.as<core::Frame>(fid), true, fid);
    for (const auto& fid : P.doc.frames_on(p.id))
        if (core::on_panel(P.doc, p, fid))   // s150: a master's panel -- its side's items and both sides'
            across(*P.doc.as<core::Frame>(fid), false, fid);

}

namespace {

void paint_frame(const Cairo::RefPtr<Cairo::Context>& cr, const CanvasLook& P, const core::Frame& f, double ox, double oy,
                 bool from_master, const std::string& id, const std::string& on_page) {
    const Rect r = P.to_screen(Rect{ox + f.x, oy + f.y, f.w, f.h});
    const double s = P.s;
    const double dim = from_master ? 0.45 : 1.0;
    const double op = std::clamp(f.opacity, 0.0, 1.0);   // s021: on fill, content and stroke alike
    // s197: the frame's outline on screen -- its box, or a shape's oval / path.
    const auto outline = [&cr, &f, r, s] {
        if (!core::doc::has_outline(f)) { cr->rectangle(r.x, r.y, r.w, r.h); return; }
        cr->begin_new_path();
        const auto X = [&](double v) { return r.x + (v - f.x) * s; };
        const auto Y = [&](double v) { return r.y + (v - f.y) * s; };
        for (const auto& o : core::doc::outline_ops(f)) {
            if (o.op == 'M') cr->move_to(X(o.v[0]), Y(o.v[1]));
            else if (o.op == 'L') cr->line_to(X(o.v[0]), Y(o.v[1]));
            else if (o.op == 'C') cr->curve_to(X(o.v[0]), Y(o.v[1]), X(o.v[2]), Y(o.v[3]), X(o.v[4]), Y(o.v[5]));
            else cr->close_path();
        }
    };
    // s016: the frame's fill, behind its content (and its stroke, after).
    if (const auto* fs = P.doc.as<core::Swatch>(f.fill)) {
        const auto c = core::tint_rgb(core::doc::screen_rgb(*fs), f.fill_tint);   // s021
        cr->save();
        cr->set_source_rgba(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, dim * op);
        outline();
        cr->fill();
        cr->restore();
    }
    struct StrokeLater {
        const Cairo::RefPtr<Cairo::Context>& cr; const core::Doc& d; const core::Frame& f; Rect r; double s, dim;
        std::function<void()> outline;   // s197
        ~StrokeLater() {
            const auto* ss = d.as<core::Swatch>(f.stroke);
            if (!ss || f.stroke_w <= 0) return;
            const auto c = core::tint_rgb(core::doc::screen_rgb(*ss), f.stroke_tint);   // s021
            cr->save();
            cr->set_source_rgba(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0,
                                dim * std::clamp(f.opacity, 0.0, 1.0));
            cr->set_line_width(std::max(0.5, f.stroke_w * s));
            // s204: its ends, corners, dashes (in screen units); under an arrow the line stops short.
            auto dash = core::doc::dash_pattern(f);
            for (auto& v : dash) v *= s;
            cr->set_line_cap(static_cast<Cairo::Context::LineCap>(f.dash == 2 ? 1 : std::clamp(f.cap, 0, 2)));
            cr->set_line_join(static_cast<Cairo::Context::LineJoin>(std::clamp(f.join, 0, 2)));
            if (!dash.empty()) cr->set_dash(dash, 0);
            if (core::doc::has_outline(f)) {
                cr->begin_new_path();
                const auto X = [&](double v) { return r.x + (v - f.x) * s; };
                const auto Y = [&](double v) { return r.y + (v - f.y) * s; };
                for (const auto& o : core::doc::stroke_ops(f)) {
                    if (o.op == 'M') cr->move_to(X(o.v[0]), Y(o.v[1]));
                    else if (o.op == 'L') cr->line_to(X(o.v[0]), Y(o.v[1]));
                    else if (o.op == 'C') cr->curve_to(X(o.v[0]), Y(o.v[1]), X(o.v[2]), Y(o.v[3]), X(o.v[4]), Y(o.v[5]));
                    else cr->close_path();
                }
                cr->stroke();
                cr->unset_dash();
                for (const auto& poly : core::doc::end_shapes(f)) {
                    for (std::size_t i = 0; i < poly.size(); ++i)
                        if (i) cr->line_to(X(poly[i].x), Y(poly[i].y)); else cr->move_to(X(poly[i].x), Y(poly[i].y));
                    cr->close_path();
                    cr->fill();
                }
            } else {
                outline();   // centred on the frame's edge, as it prints
                cr->stroke();
            }
            cr->restore();
        }
    } stroke_later{cr, P.doc, f, r, s, dim, outline};
    cr->save();

    switch (f.kind) {
    case core::FrameKind::Text: {
        // s007: the story's text, flowed through the thread (layout::Flow),
        // drawn at true size and scaled -- breaks never change with zoom.
        const auto& flow = P.flows.get(P.doc, f.story);
        cr->save();
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->clip();
        cr->translate(r.x, r.y);
        cr->scale(s, s);
        if (from_master || op < 1) cr->push_group();
        // s023: page numbers and the like filled in -- except in the story
        // being edited, whose markers show as signs (every byte where it was).
        const bool live = !P.live_story.empty() && P.live_story == f.story;
        const std::string ctx = from_master && P.doc.as<core::Page>(on_page) ? on_page : std::string{};
        const layout::Resolved* rs = live ? nullptr : &P.flows.resolved(P.doc, f.story, ctx);
        if (rs && rs->any) layout::draw_frame_text(cr->cobj(), P.doc, f.story, rs->flow, id, 0, 0, 0.08, &rs->text);
        else layout::draw_frame_text(cr->cobj(), P.doc, f.story, flow, id, 0, 0);
        // s059: hidden characters, over the words, in the selection's blue (never printed).
        if (P.show_hidden && !from_master) {
            const auto marks = (rs && rs->any) ? layout::hidden_marks(P.doc, f.story, rs->flow, id, &rs->text)
                                               : layout::hidden_marks(P.doc, f.story, flow, id);
            cr->save();
            cr->set_source_rgba(0.22, 0.45, 0.85, 0.8);
            cr->set_line_width(0.6);
            cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::NORMAL);
            for (const auto& m : marks) {
                const double mid = m.base - m.h * 0.28;
                using K = layout::HiddenMark::Kind;
                if (m.kind == K::Space) {
                    cr->arc((m.x0 + m.x1) / 2, mid, std::max(0.45, m.h * 0.045), 0, 2 * M_PI);
                    cr->fill();
                } else if (m.kind == K::Tab) {   // an arrow across the tab's room
                    const double a = m.x0 + 1, b = std::max(a + 2, m.x1 - 1), hh = m.h * 0.12;
                    cr->move_to(a, mid);
                    cr->line_to(b, mid);
                    cr->move_to(b - hh, mid - hh);
                    cr->line_to(b, mid);
                    cr->line_to(b - hh, mid + hh);
                    cr->stroke();
                } else if (m.kind == K::Hyphen) {   // s065: a typed hyphen -- a short dash, raised
                    cr->move_to(m.x0 - m.h * 0.1, mid - m.h * 0.12);
                    cr->line_to(m.x0 + m.h * 0.1, mid - m.h * 0.12);
                    cr->stroke();
                } else if (m.kind == K::Para || m.kind == K::End) {
                    cr->set_font_size(m.h * 0.62);
                    cr->move_to(m.x0 + 1, m.base);
                    cr->show_text(m.kind == K::Para ? "\u00b6" : "#");
                } else {   // a break: a down arrow -- a bar under it for a frame, two for a page
                    const double x = m.x0 + 1 + m.h * 0.2, top = m.base - m.h * 0.62, bot = m.base - m.h * 0.12, w = m.h * 0.18;
                    cr->move_to(x, top);
                    cr->line_to(x, bot);
                    cr->move_to(x - w, bot - w);
                    cr->line_to(x, bot);
                    cr->line_to(x + w, bot - w);
                    for (int k = 1; k < std::min(m.brk, 3); ++k) {
                        cr->move_to(x - w, bot + k * m.h * 0.08);
                        cr->line_to(x + w, bot + k * m.h * 0.08);
                    }
                    cr->stroke();
                    // s098: a dashed rule on to the column's edge, the break named at its end.
                    const double from = x + w + m.h * 0.25, to = std::max(from, m.edge - 1);
                    const double size = std::max(3.0, m.h * 0.42);
                    cr->set_font_size(size);
                    Cairo::TextExtents te;
                    const char* name = layout::break_name(m.brk);
                    cr->get_text_extents(name, te);
                    const double label_x = to - te.x_advance;
                    if (label_x > from + m.h * 0.5) {
                        const std::vector<double> dash{m.h * 0.18, m.h * 0.12};
                        cr->save();
                        cr->set_dash(dash, 0);
                        cr->move_to(from, mid);
                        cr->line_to(label_x - m.h * 0.2, mid);
                        cr->stroke();
                        cr->restore();
                        cr->move_to(label_x, mid + te.height / 2);
                        cr->show_text(name);
                    } else {   // no room on the line: the name alone, after the arrow
                        cr->move_to(from, mid + te.height / 2);
                        cr->show_text(name);
                    }
                }
            }
            cr->restore();
        }
        if (from_master || op < 1) { cr->pop_group_to_source(); cr->paint_with_alpha(dim * op); }
        cr->restore();
        cr->set_source_rgba(0.45, 0.55, 0.7, 0.55 * dim);
        cr->set_line_width(1);
        cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        cr->stroke();
        // s012: the columns' edges, either side of each gutter.
        if (const int n = core::column_count(f); n > 1) {
            cr->save();
            std::vector<double> dash{3, 3};
            cr->set_dash(dash, 0);
            cr->set_source_rgba(0.45, 0.55, 0.7, 0.4 * dim);
            for (int k = 0; k + 1 < n; ++k) {
                for (double fx : {core::column_x(f, k) + core::column_width(f, k), core::column_x(f, k + 1)}) {
                    const double x = std::round(r.x + fx * s) + 0.5;
                    cr->move_to(x, r.y);
                    cr->line_to(x, r.b());
                }
            }
            cr->stroke();
            cr->restore();
        }
        // s040: the inset -- where the text area sits inside the frame.
        if (const auto& in = f.inset; !(in == core::Sides{})) {
            cr->save();
            std::vector<double> dash{2, 3};
            cr->set_dash(dash, 0);
            cr->set_source_rgba(0.45, 0.55, 0.7, 0.4 * dim);
            const double x0 = std::round(r.x + std::max(0.0, in.left) * s) + 0.5, y0 = std::round(r.y + std::max(0.0, in.top) * s) + 0.5;
            const double x1 = std::round(r.x + (f.w - std::max(0.0, in.right)) * s) + 0.5;
            const double y1 = std::round(r.y + (f.h - std::max(0.0, in.bottom)) * s) + 0.5;
            if (x1 > x0 && y1 > y0) { cr->rectangle(x0, y0, x1 - x0, y1 - y0); cr->stroke(); }
            cr->restore();
        }
        // Thread ports: in (top-left), out (bottom-right); filled = linked.
        // The last frame of an overset story shows the red + instead.
        const auto [at, total] = core::thread_place(P.doc, id);
        const bool over_here = flow.overset && total > 0 && at == total;
        const double ps = 7;
        const auto port = [&](double x, double y, bool linked) {
            cr->set_source_rgb(1, 1, 1);
            cr->rectangle(x, y, ps, ps);
            cr->fill_preserve();
            cr->set_source_rgba(0.45, 0.55, 0.7, dim);
            cr->stroke();
            if (linked) {
                cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], dim);
                cr->rectangle(x + 2, y + 2, ps - 4, ps - 4);
                cr->fill();
            }
        };
        if (!from_master && r.w > 24 && r.h > 24) {
            // s060: just outside the frame's edges (they sat over its first glyph).
            const Rect in = port_rect(r, false), out = port_rect(r, true);
            port(in.x, in.y, at > 1);
            const double px = out.x, py = out.y;
            if (over_here) {
                cr->set_source_rgb(0.85, 0.15, 0.15);
                cr->rectangle(px - 1, py - 1, ps + 2, ps + 2);
                cr->fill();
                cr->set_source_rgb(1, 1, 1);
                cr->set_line_width(1.5);
                cr->move_to(px + ps / 2, py + 1); cr->line_to(px + ps / 2, py + ps - 1);
                cr->move_to(px + 1, py + ps / 2); cr->line_to(px + ps - 1, py + ps / 2);
                cr->stroke();
                cr->set_line_width(1);
            } else {
                port(px, py, total > 0 && at < total);
            }
        }
        break;
    }
    case core::FrameKind::Image: {
        if (f.asset.empty()) {   // an empty graphic frame: the classic X
            cr->set_source_rgba(0.5, 0.5, 0.55, 0.8 * dim);
            cr->set_line_width(1);
            cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
            cr->move_to(r.x, r.y); cr->line_to(r.r(), r.b());
            cr->move_to(r.r(), r.y); cr->line_to(r.x, r.b());
            cr->stroke();
            break;
        }
        const auto* asset = P.doc.as<core::Asset>(f.asset);
        const auto status = asset ? core::link_status(*asset, P.dir) : core::LinkStatus::Missing;
        const bool missing = status == core::LinkStatus::Missing || status == core::LinkStatus::EmbeddedMissing;
        // s009: the real picture, cropped by its frame.
        bool pending = false;   // s174: a big picture decodes on its own thread; meanwhile, a quiet plate
        if (asset && !missing) {
            const std::string file = core::resolve_asset(*asset, P.dir);
            int iw = 0, ih = 0;
            if (auto pic = images::peek(file, core::file_stamp(file), &iw, &ih, &pending)) {
                cr->save();
                cr->rectangle(r.x, r.y, r.w, r.h);
                cr->clip();
                const Rect ir = core::picture_rect(r, f, iw, ih, s);   // s035: Custom placement too
                if (const auto polys = core::clip_polys(*asset, ir); !polys.empty()) {   // s189: its clipping path
                    cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
                    for (const auto& poly : polys) {
                        cr->move_to(poly[0][0], poly[0][1]);
                        for (std::size_t i = 1; i < poly.size(); ++i) cr->line_to(poly[i][0], poly[i][1]);
                        cr->close_path();
                    }
                    cr->clip();
                }
                images::paint(cr, pic, ir.x, ir.y, ir.w, ir.h, dim * op);   // s021: its opacity
                cr->restore();
                if (status == core::LinkStatus::Modified) {   // a quiet corner flag
                    cr->set_source_rgba(0.85, 0.51, 0.17, 0.95);
                    cr->move_to(r.r(), r.y); cr->line_to(r.r() - 12, r.y); cr->line_to(r.r(), r.y + 12);
                    cr->close_path();
                    cr->fill();
                }
                break;
            }
        }
        if (pending) {   // s174: still decoding -- say so
            {
                cr->set_source_rgba(0.5, 0.5, 0.55, 0.12 * dim * op);
                cr->rectangle(r.x, r.y, r.w, r.h);
                cr->fill();
                cr->set_source_rgba(0.35, 0.35, 0.4, 0.8 * dim * op);
                cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::NORMAL);
                cr->set_font_size(std::clamp(r.h * 0.08, 7.0, 13.0));
                Cairo::TextExtents te;
                const std::string t = "Loading " + asset->name + "\u2026";
                cr->get_text_extents(t, te);
                if (te.width < r.w - 8) {
                    cr->move_to(r.x + (r.w - te.width) / 2 - te.x_bearing, r.y + r.h / 2 - te.height / 2 - te.y_bearing);
                    cr->show_text(t);
                }
                break;
            }
        }
        rgb(cr, tint_for(f.asset), 0.55 * dim * op);   // s021: the stand-in at its opacity too
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->fill();
        // The stand-in: a file it can't read here (or the unsaved sample's).
        cr->set_source_rgba(1, 1, 1, 0.5 * dim * op);
        cr->move_to(r.x + r.w * 0.1, r.y + r.h * 0.85);
        cr->line_to(r.x + r.w * 0.38, r.y + r.h * 0.45);
        cr->line_to(r.x + r.w * 0.55, r.y + r.h * 0.68);
        cr->line_to(r.x + r.w * 0.7, r.y + r.h * 0.52);
        cr->line_to(r.x + r.w * 0.9, r.y + r.h * 0.85);
        cr->close_path();
        cr->fill();
        if (missing) {
            cr->set_source_rgba(0.85, 0.2, 0.2, 0.85 * dim);
            cr->set_line_width(2);
            cr->move_to(r.x, r.y); cr->line_to(r.r(), r.b());
            cr->move_to(r.r(), r.y); cr->line_to(r.x, r.b());
            cr->stroke();
        }
        if (const auto* a = P.doc.as<core::Asset>(f.asset); a && r.w > 60 && r.h > 20) {
            auto lay = Pango::Layout::create(cr);
            lay->set_text(a->name + (missing ? "  — missing" : ""));
            lay->set_font_description(Pango::FontDescription("Sans 8"));
            cr->set_source_rgba(missing ? 0.75 : 0.15, 0.15, 0.15, 0.9 * dim);
            cr->move_to(r.x + 4, r.y + 2);
            lay->show_in_cairo_context(cr);
        }
        break;
    }
    case core::FrameKind::Shape:
        if (!f.fill.empty() || !f.stroke.empty()) break;   // s016: it has its own look
        cr->set_source_rgba(0.5, 0.5, 0.55, 0.12 * dim);
        outline();
        cr->fill_preserve();
        cr->set_source_rgba(0.4, 0.4, 0.45, 0.8 * dim);
        cr->set_line_width(1);
        cr->stroke();
        break;
    }
    cr->restore();
}


}  // namespace

PixBox canvas_page_box(const core::Doc& d, const core::Placed& p, const CanvasParams& v) {
    Rect b = p.r;
    const double bleed = core::bleed_of(d, p.id);   // s147
    b = Rect{b.x - bleed, b.y - bleed, b.w + 2 * bleed, b.h + 2 * bleed};
    double stroke = 0;
    const core::Side sd = core::side_for(d, p.id, p.side);   // s155
    const auto add = [&](const std::string& fid) {
        const auto* f = d.as<core::Frame>(fid);
        if (!f) return;
        Rect fr{p.r.x + f->x + core::across_dx(d, *f, sd), p.r.y + f->y, f->w, f->h};
        if (f->across && sd == core::Side::Left)  fr.w = std::max(0.0, std::min(fr.r(), p.r.r()) - fr.x);   // cut at the spine
        if (f->across && sd == core::Side::Right) { const double r = fr.r(); fr.x = std::max(fr.x, p.r.x); fr.w = std::max(0.0, r - fr.x); }
        b = b.unite(fr);
        stroke = std::max(stroke, f->stroke_w);
        if (f->end_a || f->end_b) stroke = std::max(stroke, f->stroke_w * 4 + 2);   // s204: an arrow is wider than its line
    };
    if (!p.master)
        for (const auto& m : core::master_chain(d, p.id))
            for (const auto& fid : d.frames_on(m))
                if (core::master_item_on(d, fid, p.id)) add(fid);
    for (const auto& fid : d.frames_on(p.id))
        if (core::on_panel(d, p, fid)) add(fid);
    const Rect s = v.to_screen(b);
    // Shadow (4 px), ports (7 px, 3.5 px out), half a stroke, a pixel's rounding.
    const double m = 14 + stroke * v.s / 2;
    PixBox out;
    out.x = static_cast<int>(std::floor(s.x - m));
    out.y = static_cast<int>(std::floor(s.y - m));
    out.w = static_cast<int>(std::ceil(s.r() + m)) - out.x;
    out.h = static_cast<int>(std::ceil(s.b() + m)) - out.y;
    return out;
}

namespace {
const cairo_user_data_key_t kPlaceKey{};
}  // namespace

const ImagePlace* canvas_image_place(const layout::Surface& img) {
    if (!img) return nullptr;
    return static_cast<const ImagePlace*>(cairo_surface_get_user_data(img.get(), &kPlaceKey));
}

layout::Surface render_canvas_page(const core::Doc& d, layout::FlowCache& flows, const CanvasParams& v,
                                   const core::Placed& p, const PixBox& box, double ds, const plate::Proof* proof) {
    if (box.w <= 0 || box.h <= 0) return {};
    const int pw = std::max(1, static_cast<int>(std::ceil(box.w * ds)));
    const int ph = std::max(1, static_cast<int>(std::ceil(box.h * ds)));
    auto img = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, pw, ph);
    cairo_surface_set_device_scale(img->cobj(), ds, ds);
    {
        auto cr = Cairo::Context::create(img);
        cr->translate(-box.x, -box.y);
        paint_canvas_page(cr, CanvasLook(v, d, flows), p);
    }
    img->flush();
    if (proof) {   // s099, per page now: the opaque pixels (the paper and what's on it)
        unsigned char* data = img->get_data();
        const int stride = img->get_stride();
        std::vector<std::uint8_t> px;
        px.reserve(static_cast<std::size_t>(pw) * 3);
        for (int y = 0; y < ph; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(data + static_cast<std::size_t>(y) * stride);
            px.clear();
            for (int x = 0; x < pw; ++x)
                if ((row[x] >> 24) == 0xFF) {
                    px.push_back(static_cast<std::uint8_t>(row[x] >> 16));
                    px.push_back(static_cast<std::uint8_t>(row[x] >> 8));
                    px.push_back(static_cast<std::uint8_t>(row[x]));
                }
            proof->simulate(px.data(), px.size() / 3);
            std::size_t k = 0;
            for (int x = 0; x < pw; ++x)
                if ((row[x] >> 24) == 0xFF) {
                    row[x] = 0xFF000000u | (std::uint32_t(px[k]) << 16) | (std::uint32_t(px[k + 1]) << 8) | px[k + 2];
                    k += 3;
                }
        }
        img->mark_dirty();
    }
    auto* place = new ImagePlace{v.s, v.ox, v.oy, box.x, box.y};
    cairo_surface_set_user_data(img->cobj(), &kPlaceKey, place, [](void* q) { delete static_cast<ImagePlace*>(q); });
    return layout::adopt_surface(cairo_surface_reference(img->cobj()));
}

}  // namespace composix::views
