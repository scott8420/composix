#include "views/Canvas.hpp"
#include "views/Ruler.hpp"
#include "Log.hpp"

#include <gdkmm/cursor.h>
#include <glibmm/main.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace composix::views {

using core::Rect;

namespace {

// The accent for selection (Adwaita's blue; the theme's own is not reachable
// from a draw function without the deprecated style-context lookups).
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

const char* cursor_for(core::Handle h) {
    switch (h) {
    case core::Handle::NW: case core::Handle::SE: return "nwse-resize";
    case core::Handle::NE: case core::Handle::SW: return "nesw-resize";
    case core::Handle::N:  case core::Handle::S:  return "ns-resize";
    case core::Handle::E:  case core::Handle::W:  return "ew-resize";
    default: return "default";
    }
}

}  // namespace

Canvas::Canvas(core::Doc& d, core::DocJournal& j)
    : widgets::DrawingArea("canvas"), m_doc(d), m_journal(j) {
    add_css_class("cx-canvas");
    set_hexpand(true);
    set_vexpand(true);
    set_focusable(true);
    set_draw_func(sigc::mem_fun(*this, &Canvas::draw));

    // Left button: pick / move / resize / marquee (or pan with Space held).
    m_gdrag = Gtk::GestureDrag::create();
    m_gdrag->set_button(GDK_BUTTON_PRIMARY);
    m_gdrag->signal_drag_begin().connect(sigc::mem_fun(*this, &Canvas::on_drag_begin));
    m_gdrag->signal_drag_update().connect(sigc::mem_fun(*this, &Canvas::on_drag_update));
    m_gdrag->signal_drag_end().connect(sigc::mem_fun(*this, &Canvas::on_drag_end));
    add_controller(m_gdrag);

    // Middle button: pan.
    auto mid = Gtk::GestureDrag::create();
    mid->set_button(GDK_BUTTON_MIDDLE);
    mid->signal_drag_begin().connect([this](double, double) {
        m_drag = Drag::Pan;
        m_ox0 = m_vp.ox();
        m_oy0 = m_vp.oy();
        set_cursor(Gdk::Cursor::create("grabbing"));
    });
    mid->signal_drag_update().connect([this](double dx, double dy) {
        if (m_drag != Drag::Pan) return;
        m_vp.set_origin(m_ox0 + dx, m_oy0 + dy);
        queue_draw();
    });
    mid->signal_drag_end().connect([this](double, double) { m_drag = Drag::None; update_cursor(); });
    add_controller(mid);

    m_gscroll = Gtk::EventControllerScroll::create();
    m_gscroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    m_gscroll->signal_scroll().connect(sigc::mem_fun(*this, &Canvas::on_scroll), false);
    add_controller(m_gscroll);

    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_motion().connect(sigc::mem_fun(*this, &Canvas::on_motion));
    motion->signal_leave().connect([this] { m_in = false; m_sig_view.emit(); });
    add_controller(motion);

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint kv, guint, Gdk::ModifierType st) { return on_key(kv, st); }, false);
    keys->signal_key_released().connect([this](guint kv, guint, Gdk::ModifierType) { on_key_up(kv); });
    add_controller(keys);
}

// ─────────────────────────────────────────────────────────────────────────────
// what is shown
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::show_pages(const std::vector<std::string>& ids) {
    const bool same = ids == m_ids;
    m_ids = ids;
    relayout();
    if (!same) {
        m_sel.clear();
        m_fit_pending = true;
    }
    queue_draw();
}

void Canvas::model_changed() {
    relayout();
    // Drop picks that are gone or no longer on a shown page.
    std::vector<std::string> keep;
    for (const auto& id : m_sel)
        if (core::frame_in_layout(m_doc, m_placed, id).w > 0) keep.push_back(id);
    m_sel = keep;
    queue_draw();
}

void Canvas::relayout() {
    m_placed = core::layout_pages(m_doc, m_ids);
    Rect all;
    for (const auto& p : m_placed) all = all.unite(p.r);
    // Content = the pages plus a margin of pasteboard, so frames hanging off
    // a page can still be reached.
    m_vp.set_content({all.x - 72, all.y - 72, all.w + 144, all.h + 144});
}

void Canvas::set_selection(const std::vector<std::string>& ids) {
    m_sel.clear();
    for (const auto& id : ids)
        if (core::frame_in_layout(m_doc, m_placed, id).w > 0) m_sel.push_back(id);
    queue_draw();
}

void Canvas::pick(const std::vector<std::string>& ids, bool notify) {
    if (ids == m_sel) return;
    m_sel = ids;
    queue_draw();
    if (notify) m_sig_sel.emit();
}

Rect Canvas::selection_box() const {
    Rect box;
    for (const auto& id : m_sel) box = box.unite(core::frame_in_layout(m_doc, m_placed, id));
    return box;
}

// ─────────────────────────────────────────────────────────────────────────────
// zoom
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::zoom_fit() {
    m_vp.fit();
    queue_draw();
    m_sig_zoom.emit();
}

void Canvas::zoom_to(double pct) {
    m_vp.set_percent(pct);
    queue_draw();
    m_sig_zoom.emit();
}

void Canvas::zoom_step(int dir) {
    m_vp.zoom_toward(m_vp.widget_w() / 2, m_vp.widget_h() / 2, dir > 0 ? 1.25 : 1 / 1.25);
    queue_draw();
    m_sig_zoom.emit();
}

// ─────────────────────────────────────────────────────────────────────────────
// drawing: pasteboard (CSS) -> pages -> master items -> own frames -> overlays
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    m_vp.set_widget(w, h);
    if (m_fit_pending && w > 0 && h > 0 && !m_placed.empty()) {
        m_fit_pending = false;
        m_vp.fit();
        Glib::signal_idle().connect_once([this] { m_sig_zoom.emit(); });
    }
    for (const auto& p : m_placed) draw_page(cr, p);
    if (m_show_guides)
        for (const auto& p : m_placed) draw_guides(cr, p);

    // Snap lines while dragging (magenta, across the whole view).
    cr->set_line_width(1);
    cr->set_source_rgba(0.9, 0.1, 0.6, 0.85);
    if (!std::isnan(m_snap_x)) {
        double sx, sy;
        m_vp.to_screen(m_snap_x, 0, sx, sy);
        cr->move_to(std::round(sx) + 0.5, 0);
        cr->line_to(std::round(sx) + 0.5, h);
    }
    if (!std::isnan(m_snap_y)) {
        double sx, sy;
        m_vp.to_screen(0, m_snap_y, sx, sy);
        cr->move_to(0, std::round(sy) + 0.5);
        cr->line_to(w, std::round(sy) + 0.5);
    }
    cr->stroke();

    // A guide being pulled out of a ruler.
    if (m_preview) {
        std::vector<double> dash{6, 4};
        cr->set_dash(dash, 0);
        cr->set_source_rgba(0.0, 0.65, 0.8, 0.9);
        const double at = std::round(m_preview_at) + 0.5;
        if (m_preview_v) { cr->move_to(at, 0); cr->line_to(at, h); }
        else             { cr->move_to(0, at); cr->line_to(w, at); }
        cr->stroke();
        cr->unset_dash();
    }
    draw_overlays(cr);
    m_sig_view.emit();
}

// Guides: a master's (dimmed) and the page's own, across the page.
void Canvas::draw_guides(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p) {
    const Rect sr = m_vp.to_screen(p.r);
    const double s = m_vp.scale();
    auto owners = p.master ? std::vector<std::string>{} : core::master_chain(m_doc, p.id);
    owners.push_back(p.id);
    cr->set_line_width(1);
    for (const auto& o : owners) {
        const bool own = o == p.id;
        for (const auto& gid : core::guides_on(m_doc, o)) {
            const auto& g = *m_doc.as<core::Guide>(gid);
            const bool lit = gid == m_hover_guide || gid == m_guide;
            if (gid == m_guide && m_guide_off) cr->set_source_rgba(0.85, 0.2, 0.2, 0.9);   // will be removed
            else cr->set_source_rgba(0.0, 0.65, 0.8, own ? (lit ? 1.0 : 0.75) : 0.4);
            if (g.vertical) {
                const double x = std::round(sr.x + g.pos * s) + 0.5;
                cr->move_to(x, sr.y);
                cr->line_to(x, sr.b());
            } else {
                const double y = std::round(sr.y + g.pos * s) + 0.5;
                cr->move_to(sr.x, y);
                cr->line_to(sr.r(), y);
            }
            cr->stroke();
        }
    }
}

void Canvas::preview_guide(bool vertical, double at) {
    m_preview = true;
    m_preview_v = vertical;
    m_preview_at = at;
    queue_draw();
}

// Released from a ruler: over a page, a guide at that spot (one step);
// over the pasteboard or back on the ruler, nothing.
void Canvas::drop_guide(bool vertical, double x, double y) {
    m_preview = false;
    queue_draw();
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    const auto page = core::page_at(m_placed, dx, dy);
    if (page.empty() || x < 0 || y < 0) return;
    for (const auto& p : m_placed)
        if (p.id == page) {
            core::doc::add_guide(m_journal, m_doc, page, vertical, vertical ? dx - p.r.x : dy - p.r.y);
            m_sig_edited.emit();
            return;
        }
}

void Canvas::draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p) {
    const double s = m_vp.scale();
    const Rect sr = m_vp.to_screen(p.r);
    const auto fg = get_color();

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
    const double bleed = m_doc.settings().bleed * s;
    if (bleed > 1) {
        cr->set_source_rgba(0.85, 0.2, 0.2, 0.55);
        cr->set_line_width(1);
        cr->rectangle(std::round(sr.x - bleed) + 0.5, std::round(sr.y - bleed) + 0.5,
                      std::round(sr.w + 2 * bleed), std::round(sr.h + 2 * bleed));
        cr->stroke();
    }

    // Folds: dashed, from the page's own fold positions.
    const std::vector<double>* folds = nullptr;
    if (const auto* pg = m_doc.as<core::Page>(p.id)) folds = &pg->folds;
    else if (const auto* m = m_doc.as<core::Master>(p.id)) folds = &m->folds;
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
    if (!p.master)
        for (const auto& m : core::master_chain(m_doc, p.id))
            for (const auto& fid : m_doc.frames_on(m))
                draw_frame(cr, *m_doc.as<core::Frame>(fid), p.r.x, p.r.y, true, fid);
    for (const auto& fid : m_doc.frames_on(p.id))
        draw_frame(cr, *m_doc.as<core::Frame>(fid), p.r.x, p.r.y, false, fid);

    // The page's name, under it.
    std::string name;
    if (p.master) name = core::master_label(m_doc, p.id);
    else          name = "Page " + m_doc.folio(p.id);
    auto lay = create_pango_layout(name);
    lay->set_font_description(Pango::FontDescription("Sans 9"));
    int lw, lh;
    lay->get_pixel_size(lw, lh);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
    cr->move_to(sr.x + (sr.w - lw) / 2, sr.b() + 8);
    lay->show_in_cairo_context(cr);
}

void Canvas::draw_frame(const Cairo::RefPtr<Cairo::Context>& cr, const core::Frame& f, double ox, double oy,
                        bool from_master, const std::string& id) {
    const Rect r = m_vp.to_screen(Rect{ox + f.x, oy + f.y, f.w, f.h});
    const double s = m_vp.scale();
    const double dim = from_master ? 0.45 : 1.0;
    cr->save();

    switch (f.kind) {
    case core::FrameKind::Text: {
        // s005 draws the frame and stand-in lines; real text flow through the
        // thread is s006. The story's name says what will flow here.
        const auto* st = m_doc.as<core::Story>(f.story);
        double lead = 14;
        if (st && !st->paras.empty())
            if (const auto* sty = m_doc.as<core::Style>(st->paras.back().style)) lead = std::max(6.0, sty->leading);
        const double inset = 4 * s, step = lead * s, bar = std::max(1.0, step * 0.42);
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->clip();
        cr->set_source_rgba(0.2, 0.2, 0.25, 0.16 * dim);
        int n = 0;
        if (step >= 2)
            for (double y = r.y + inset + step * 0.6; y + bar < r.b() - inset; y += step, ++n) {
                const double w = (n % 6 == 5) ? (r.w - 2 * inset) * 0.6 : r.w - 2 * inset;
                cr->rectangle(r.x + inset, std::round(y), w, bar);
            }
        cr->fill();
        cr->reset_clip();
        cr->set_source_rgba(0.45, 0.55, 0.7, 0.7 * dim);
        cr->set_line_width(1);
        cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        cr->stroke();
        // Thread ports: in (top-left), out (bottom-right); filled = linked.
        const auto [at, total] = core::thread_place(m_doc, id);
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
            port(std::round(r.x) + 3.5, std::round(r.y) + 3.5, at > 1);
            port(std::round(r.r()) - ps - 3.5, std::round(r.b()) - ps - 3.5, total > 0 && at < total);
        }
        if (st && r.w > 60 && r.h > 20 && !from_master) {
            auto lay = create_pango_layout(st->name + (total > 1 ? "  " + std::to_string(at) + "/" + std::to_string(total) : ""));
            lay->set_font_description(Pango::FontDescription("Sans 8"));
            cr->set_source_rgba(0.35, 0.45, 0.6, 0.9);
            cr->move_to(r.x + 14, r.y + 2);
            lay->show_in_cairo_context(cr);
        }
        break;
    }
    case core::FrameKind::Image: {
        const bool missing = m_missing && m_missing(f.asset);
        rgb(cr, tint_for(f.asset), 0.55 * dim);
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->fill();
        // A mountain-and-sun stand-in until images are decoded.
        cr->set_source_rgba(1, 1, 1, 0.5 * dim);
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
        if (const auto* a = m_doc.as<core::Asset>(f.asset); a && r.w > 60 && r.h > 20) {
            auto lay = create_pango_layout(a->name + (missing ? "  — missing" : ""));
            lay->set_font_description(Pango::FontDescription("Sans 8"));
            cr->set_source_rgba(missing ? 0.75 : 0.15, 0.15, 0.15, 0.9 * dim);
            cr->move_to(r.x + 4, r.y + 2);
            lay->show_in_cairo_context(cr);
        }
        break;
    }
    case core::FrameKind::Shape:
        cr->set_source_rgba(0.5, 0.5, 0.55, 0.12 * dim);
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->fill_preserve();
        cr->set_source_rgba(0.4, 0.4, 0.45, 0.8 * dim);
        cr->set_line_width(1);
        cr->stroke();
        break;
    }
    cr->restore();
}

void Canvas::draw_overlays(const Cairo::RefPtr<Cairo::Context>& cr) {
    cr->set_line_width(1);
    // Hover: a quiet outline on the frame under the pointer.
    if (!m_hover.empty() && std::find(m_sel.begin(), m_sel.end(), m_hover) == m_sel.end()) {
        const Rect r = m_vp.to_screen(core::frame_in_layout(m_doc, m_placed, m_hover));
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.55);
        cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        cr->stroke();
    }
    // Each pick outlined; one box with 8 handles around them all.
    for (const auto& id : m_sel) {
        const Rect r = m_vp.to_screen(core::frame_in_layout(m_doc, m_placed, id));
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
        cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        cr->stroke();
    }
    if (!m_sel.empty()) {
        const Rect box = m_vp.to_screen(selection_box());
        if (m_sel.size() > 1) {
            std::vector<double> dash{4, 3};
            cr->set_dash(dash, 0);
            cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.8);
            cr->rectangle(std::round(box.x) + 0.5, std::round(box.y) + 0.5, std::round(box.w), std::round(box.h));
            cr->stroke();
            cr->unset_dash();
        }
        for (const auto& p : core::handle_points(box)) {
            const double x = std::round(p[0]) - 3.5, y = std::round(p[1]) - 3.5;
            cr->set_source_rgb(1, 1, 1);
            cr->rectangle(x, y, 7, 7);
            cr->fill_preserve();
            cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
            cr->stroke();
        }
    }
    if (m_drag == Drag::Marquee) {
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.12);
        cr->rectangle(m_marquee.x, m_marquee.y, m_marquee.w, m_marquee.h);
        cr->fill_preserve();
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.8);
        cr->stroke();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// the Select tool
// ─────────────────────────────────────────────────────────────────────────────
void Canvas::on_drag_begin(double x, double y) {
    grab_focus();
    m_px = x;
    m_py = y;
    m_begun = false;
    m_handle = core::Handle::None;

    if (m_space) {   // Space held: the hand
        m_drag = Drag::Pan;
        m_ox0 = m_vp.ox();
        m_oy0 = m_vp.oy();
        set_cursor(Gdk::Cursor::create("grabbing"));
        return;
    }
    read_mods();

    // Handles first (on the whole selection), then a frame, then empty space.
    if (!m_sel.empty()) {
        m_handle = core::handle_hit(m_vp.to_screen(selection_box()), x, y);
        if (m_handle != core::Handle::None) {
            m_drag = Drag::Resize;
            m_box0 = selection_box();
            m_own0.clear();
            m_lay0.clear();
            for (const auto& id : m_sel) {
                m_own0[id] = core::frame_rect(*m_doc.as<core::Frame>(id));
                m_lay0[id] = core::frame_in_layout(m_doc, m_placed, id);
            }
            m_targets = core::snap_targets(m_doc, m_placed, m_sel);
            return;
        }
    }
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    // A guide, before frames (it lies on top of them).
    if (m_show_guides) {
        const auto g = core::hit_guide(m_doc, m_placed, dx, dy, 4 / m_vp.scale());
        if (!g.empty()) {
            m_drag = Drag::Guide;
            m_guide = g;
            m_guide0 = m_doc.as<core::Guide>(g)->pos;
            m_guide_off = false;
            return;
        }
    }
    const auto hit = core::hit_frame(m_doc, m_placed, dx, dy, 3 / m_vp.scale());
    const bool adding = m_mods_shift;
    if (!hit.empty()) {
        if (adding) {   // Shift-click: toggle, no move
            auto s = m_sel;
            const auto it = std::find(s.begin(), s.end(), hit);
            if (it == s.end()) s.push_back(hit); else s.erase(it);
            pick(s);
            m_drag = Drag::None;
            return;
        }
        if (std::find(m_sel.begin(), m_sel.end(), hit) == m_sel.end()) pick({hit});
        m_drag = Drag::Move;
        m_own0.clear();
        for (const auto& id : m_sel) m_own0[id] = core::frame_rect(*m_doc.as<core::Frame>(id));
        m_box0 = selection_box();
        m_targets = core::snap_targets(m_doc, m_placed, m_sel);
        return;
    }
    m_drag = Drag::Marquee;
    m_sel0 = adding ? m_sel : std::vector<std::string>{};
    m_marquee = {x, y, 0, 0};
    if (!adding) pick({});
}

// Shift / Alt from the gesture's own event, at press and on every update. Not
// at release: some platforms clear Alt before the release arrives (Curvz).
void Canvas::read_mods() {
    const auto st = m_gdrag->get_current_event_state();
    m_mods_shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
    m_mods_alt   = (st & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK;
}

void Canvas::on_drag_update(double ox, double oy) {
    read_mods();
    const double s = m_vp.scale();
    switch (m_drag) {
    case Drag::None:
        return;
    case Drag::Pan:
        m_vp.set_origin(m_ox0 + ox, m_oy0 + oy);
        queue_draw();
        return;
    case Drag::Marquee: {
        m_marquee = Rect::from_corners(m_px, m_py, m_px + ox, m_py + oy);
        double x0, y0, x1, y1;
        m_vp.to_doc(m_marquee.x, m_marquee.y, x0, y0);
        m_vp.to_doc(m_marquee.r(), m_marquee.b(), x1, y1);
        auto s2 = m_sel0;
        for (const auto& id : core::frames_in(m_doc, m_placed, Rect::from_corners(x0, y0, x1, y1)))
            if (std::find(s2.begin(), s2.end(), id) == s2.end()) s2.push_back(id);
        pick(s2);
        queue_draw();
        return;
    }
    case Drag::Move: {
        if (!m_begun) {
            if (std::hypot(ox, oy) < 3) return;   // a click, not a drag (yet)
            core::doc::frames_begin(m_journal, m_doc, m_sel,
                                    m_sel.size() == 1 ? "Move frame" : "Move " + std::to_string(m_sel.size()) + " frames");
            m_begun = true;
        }
        double dx = ox / s, dy = oy / s;
        bool lock_x = false, lock_y = false;
        if (m_mods_shift) {   // Shift: one axis
            if (std::abs(dx) > std::abs(dy)) { dy = 0; lock_y = true; } else { dx = 0; lock_x = true; }
        }
        m_snap_x = m_snap_y = NAN;
        if (m_snap) {   // edges and centre of the moving box, to the targets gathered at press
            const double tol = 6 / s;
            if (!lock_x) {
                const auto hx = core::snap_any({m_box0.x + dx, m_box0.cx() + dx, m_box0.r() + dx}, m_targets.xs, tol);
                if (hx.hit()) { dx += hx.delta; m_snap_x = hx.line; }
            }
            if (!lock_y) {
                const auto hy = core::snap_any({m_box0.y + dy, m_box0.cy() + dy, m_box0.b() + dy}, m_targets.ys, tol);
                if (hy.hit()) { dy += hy.delta; m_snap_y = hy.line; }
            }
        }
        for (const auto& [id, r] : m_own0)
            core::doc::frames_set(m_journal, m_doc, id, {r.x + dx, r.y + dy, r.w, r.h});
        queue_draw();
        return;
    }
    case Drag::Guide: {
        if (!m_begun) {
            if (std::hypot(ox, oy) < 2) return;
            core::doc::guide_begin(m_journal, m_doc, m_guide);
            m_begun = true;
        }
        const auto* g = m_doc.as<core::Guide>(m_guide);
        if (!g) return;
        core::doc::guide_set(m_journal, m_doc, m_guide, m_guide0 + (g->vertical ? ox : oy) / s);
        // Off its page (into the pasteboard or another page): it will be removed.
        double dx, dy;
        m_vp.to_doc(m_px + ox, m_py + oy, dx, dy);
        m_guide_off = core::page_at(m_placed, dx, dy) != g->owner;
        queue_draw();
        return;
    }
    case Drag::Resize: {
        if (!m_begun) {
            if (std::hypot(ox, oy) < 2) return;
            core::doc::frames_begin(m_journal, m_doc, m_sel,
                                    m_sel.size() == 1 ? "Resize frame" : "Resize " + std::to_string(m_sel.size()) + " frames");
            m_begun = true;
        }
        double dx = ox / s, dy = oy / s;
        m_snap_x = m_snap_y = NAN;
        if (m_snap) {   // the edge(s) the handle moves, to the targets
            const auto h = m_handle;
            const bool L = h == core::Handle::NW || h == core::Handle::W || h == core::Handle::SW;
            const bool R = h == core::Handle::NE || h == core::Handle::E || h == core::Handle::SE;
            const bool T = h == core::Handle::NW || h == core::Handle::N || h == core::Handle::NE;
            const bool B = h == core::Handle::SW || h == core::Handle::S || h == core::Handle::SE;
            const double tol = 6 / s;
            if (L || R) {
                const auto hx = core::snap_any({(L ? m_box0.x : m_box0.r()) + dx}, m_targets.xs, tol);
                if (hx.hit()) { dx += hx.delta; m_snap_x = hx.line; }
            }
            if (T || B) {
                const auto hy = core::snap_any({(T ? m_box0.y : m_box0.b()) + dy}, m_targets.ys, tol);
                if (hy.hit()) { dy += hy.delta; m_snap_y = hy.line; }
            }
        }
        const Rect box = core::resize_rect(m_box0, m_handle, dx, dy, m_mods_shift, m_mods_alt);
        for (const auto& [id, lay] : m_lay0) {
            const Rect nl = core::map_rect(lay, m_box0, box);
            const Rect& own = m_own0[id];
            // Back to the owner's coordinates: the same offset as at press.
            core::doc::frames_set(m_journal, m_doc, id,
                                  {nl.x - (lay.x - own.x), nl.y - (lay.y - own.y), nl.w, nl.h});
        }
        queue_draw();
        return;
    }
    }
}

void Canvas::on_drag_end(double, double) {
    const Drag was = m_drag;
    m_drag = Drag::None;
    if (was == Drag::Guide && m_begun && m_guide_off)
        core::doc::guide_remove_in_gesture(m_journal, m_doc, m_guide);
    if ((was == Drag::Move || was == Drag::Resize || was == Drag::Guide) && m_begun) {
        m_begun = false;
        core::doc::frames_end(m_journal);
        m_sig_edited.emit();
    }
    m_begun = false;
    m_guide.clear();
    m_guide_off = false;
    m_snap_x = m_snap_y = NAN;
    queue_draw();
    update_cursor();
}

// Escape mid-drag: everything back where it was, no step.
void Canvas::cancel_drag() {
    if ((m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::Guide) && m_begun) {
        core::doc::frames_cancel(m_journal);
        m_sig_edited.emit();
    }
    m_guide.clear();
    m_guide_off = false;
    m_snap_x = m_snap_y = NAN;
    if (m_drag == Drag::Marquee) pick(m_sel0);
    m_drag = Drag::None;
    m_begun = false;
    queue_draw();
    update_cursor();
}

bool Canvas::on_key(guint kv, Gdk::ModifierType st) {
    const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
    const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
    if (kv == GDK_KEY_space && !ctrl) {
        if (!m_space) { m_space = true; update_cursor(); }
        return true;
    }
    if (kv == GDK_KEY_Escape) {
        if (m_drag != Drag::None) { cancel_drag(); return true; }
        if (!m_sel.empty()) { pick({}); return true; }
        return false;
    }
    double dx = 0, dy = 0;
    const double step = shift ? 10 : 1;   // points
    switch (kv) {
    case GDK_KEY_Left:  dx = -step; break;
    case GDK_KEY_Right: dx = step; break;
    case GDK_KEY_Up:    dy = -step; break;
    case GDK_KEY_Down:  dy = step; break;
    default: return false;
    }
    if (m_sel.empty() || ctrl || m_drag != Drag::None) return false;
    std::string key = "nudge:";
    for (const auto& id : m_sel) key += id + ",";
    if (core::doc::move_frames(m_journal, m_doc, m_sel, dx, dy, key)) m_sig_edited.emit();
    queue_draw();
    return true;
}

void Canvas::on_key_up(guint kv) {
    if (kv == GDK_KEY_space) { m_space = false; update_cursor(); }
}

bool Canvas::on_scroll(double dx, double dy) {
    const auto st = m_gscroll->get_current_event_state();
    if ((st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK) {
        if (dy != 0) {
            m_vp.zoom_toward(m_mx, m_my, dy < 0 ? 1.1 : 1 / 1.1);
            m_sig_zoom.emit();
        }
    } else {
        m_vp.pan_by(-dx * 40, -dy * 40);
    }
    queue_draw();
    return true;
}

void Canvas::on_motion(double x, double y) {
    m_mx = x;
    m_my = y;
    m_in = true;
    m_sig_view.emit();
    if (m_drag != Drag::None) return;
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    const auto hg = m_show_guides ? core::hit_guide(m_doc, m_placed, dx, dy, 4 / m_vp.scale()) : std::string{};
    if (hg != m_hover_guide) { m_hover_guide = hg; queue_draw(); }
    const auto h = core::hit_frame(m_doc, m_placed, dx, dy, 3 / m_vp.scale());
    if (h != m_hover) { m_hover = h; queue_draw(); }
    update_cursor();
}

void Canvas::update_cursor() {
    if (m_space) { set_cursor(Gdk::Cursor::create(m_drag == Drag::Pan ? "grabbing" : "grab")); return; }
    if (!m_sel.empty()) {
        const auto h = core::handle_hit(m_vp.to_screen(selection_box()), m_mx, m_my);
        if (h != core::Handle::None) { set_cursor(Gdk::Cursor::create(cursor_for(h))); return; }
    }
    if (!m_hover_guide.empty()) {
        const auto* g = m_doc.as<core::Guide>(m_hover_guide);
        set_cursor(Gdk::Cursor::create(g && g->vertical ? "col-resize" : "row-resize"));
        return;
    }
    set_cursor(Gdk::Cursor::create(m_hover.empty() ? "default" : "move"));
}

// ─────────────────────────────────────────────────────────────────────────────
// CanvasView
// ─────────────────────────────────────────────────────────────────────────────
CanvasView::CanvasView(core::Doc& d, core::DocJournal& j)
    : widgets::Box("canvas_view", Gtk::Orientation::VERTICAL),
      m_head("canvas_view.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("canvas_view.title"),
      m_count("canvas_view.count"),
      m_fit("canvas_view.fit"),
      m_zoom("canvas_view.zoom"),
      m_grid("canvas_view.grid"),
      m_canvas(d, j) {
    add_css_class("cx-canvas-view");
    m_head.add_css_class("cx-view-head");
    m_title.add_css_class("cx-view-title");
    m_title.set_xalign(0.0f);
    m_title.set_ellipsize(Pango::EllipsizeMode::END);
    m_title.set_hexpand(true);
    m_count.add_css_class("cx-count");
    m_count.set_valign(Gtk::Align::CENTER);
    m_fit.set_icon_name("zoom-fit-best-symbolic");
    m_fit.set_tooltip_text("Fit  (Ctrl+0)");
    m_fit.set_action_name("win.zoom-fit");
    m_fit.add_css_class("cx-chrome-button");
    m_fit.set_valign(Gtk::Align::CENTER);
    m_zoom.add_css_class("cx-count");
    m_zoom.add_css_class("cx-zoom");
    m_zoom.set_valign(Gtk::Align::CENTER);
    m_zoom.set_tooltip_text("Zoom -- Ctrl+scroll, Ctrl+= / Ctrl+-, Ctrl+1 for 100%");
    m_head.append(m_title);
    m_head.append(m_count);
    m_head.append(m_fit);
    m_head.append(m_zoom);
    append(m_head);
    m_canvas.add_css_class("cx-pasteboard");
    // s006: rulers on the top and left, a corner box where they meet.
    m_top  = std::make_unique<Ruler>("canvas_view.ruler_top", true, m_canvas);
    m_left = std::make_unique<Ruler>("canvas_view.ruler_left", false, m_canvas);
    auto corner = Gtk::make_managed<Gtk::Box>();
    corner->set_name("canvas_view.ruler_corner");
    corner->add_css_class("cx-ruler");
    corner->set_size_request(20, 20);
    m_grid.attach(*corner, 0, 0);
    m_grid.attach(*m_top, 1, 0);
    m_grid.attach(*m_left, 0, 1);
    m_grid.attach(m_canvas, 1, 1);
    m_grid.set_vexpand(true);
    append(m_grid);
    m_canvas.signal_zoom().connect(sigc::mem_fun(*this, &CanvasView::sync_zoom));
    m_canvas.signal_view().connect([this] { m_top->queue_draw(); m_left->queue_draw(); });
}

CanvasView::~CanvasView() = default;

void CanvasView::set_rulers(bool on) {
    m_top->set_visible(on);
    m_left->set_visible(on);
    if (auto* c = m_grid.get_child_at(0, 0)) c->set_visible(on);
}

void CanvasView::set_title(const std::string& t, std::size_t n) {
    m_title.set_text(t);
    m_count.set_text(std::to_string(n));
    m_count.set_visible(n > 1);
}

void CanvasView::sync_zoom() {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f%%", m_canvas.zoom_percent());
    m_zoom.set_text(buf);
}

}  // namespace composix::views
