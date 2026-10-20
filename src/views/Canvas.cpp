#include "views/Canvas.hpp"
#include "views/BackButton.hpp"
#include "views/Images.hpp"
#include "views/PagePaint.hpp"
#include "views/Ruler.hpp"
#include "layout/Export.hpp"   // s180: sample_page
#include "layout/Flow.hpp"
#include "Log.hpp"

#include <gdkmm/cursor.h>
#include <glibmm/main.h>
#include <gtkmm/grid.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/image.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/overlay.h>
#include <gtkmm/popover.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/root.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/tooltip.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/togglebutton.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace composix::views {

using core::Rect;

namespace {

// The accent for selection (Adwaita's blue; the theme's own is not reachable
// from a draw function without the deprecated style-context lookups).
constexpr double kAcc[3] = {0.21, 0.52, 0.89};
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
    images::set_ready_hook([this] { queue_draw(); });   // s174: a big picture decoded: its pages again
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
    // s074: right-click -- the thing under the pointer picked, then the shell's menu.
    {
        auto right = Gtk::GestureClick::create();
        right->set_button(GDK_BUTTON_SECONDARY);
        right->signal_pressed().connect([this](int, double x, double y) {
            if (m_take) { cancel_take(); return; }   // s180: the eyedropper put down
            if (editing() || m_drag != Drag::None) return;
            grab_focus();
            double lx = 0, ly = 0;
            m_vp.to_doc(x, y, lx, ly);
            front_panel(lx, ly);   // s150
            ContextHit h;
            h.x = x;
            h.y = y;
            h.frame = core::hit_frame(m_doc, m_placed, lx, ly, 3 / m_vp.scale());
            if (!h.frame.empty()) {
                if (std::find(m_sel.begin(), m_sel.end(), h.frame) == m_sel.end()) pick({h.frame});
            } else {
                h.master_item = core::hit_master_item(m_doc, m_placed, lx, ly, 3 / m_vp.scale(), &h.page);
                if (h.page.empty()) h.page = core::page_at(m_placed, lx, ly);
                if (!m_sel.empty()) pick({});
            }
            if (h.page.empty() && !h.frame.empty())
                if (const auto* f = m_doc.as<core::Frame>(h.frame)) h.page = f->owner;
            if (auto lg = log::get(log::Area::View))
                lg->info("context: frame '{}' master item '{}' page '{}'", h.frame, h.master_item, h.page);
            m_sig_context.emit(h);
        });
        add_controller(right);
    }
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
    // s130: a cross-reference under the pointer -- where it points.
    set_has_tooltip(true);
    signal_query_tooltip().connect([this](int x, int y, bool kbd, const Glib::RefPtr<Gtk::Tooltip>& tip) {
        if (kbd || m_drag != Drag::None) return false;
        const auto t = ref_tip(x, y);
        if (t.empty()) return false;
        tip->set_text(t);
        return true;
    }, false);

    // s009: image files dropped from the desktop.
    auto drop = Gtk::DropTarget::create(GDK_TYPE_FILE_LIST, Gdk::DragAction::COPY);
    drop->signal_drop().connect(sigc::mem_fun(*this, &Canvas::on_drop), false);
    add_controller(drop);
    // s062: a master dropped on a page.
    auto mdrop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::COPY);
    mdrop->signal_drop().connect([this](const Glib::ValueBase& value, double x, double y) {
        Glib::Value<Glib::ustring> v;
        v.init(value.gobj());
        const std::string text = v.get(), tag = "composix-master:";
        if (text.rfind(tag, 0) != 0) return false;
        double dx = 0, dy = 0;
        m_vp.to_doc(x, y, dx, dy);
        const auto page = core::page_at(m_placed, dx, dy);
        if (page.empty() || !m_doc.as<core::Page>(page)) return false;
        const std::string master = text.substr(tag.size());
        Glib::signal_idle().connect_once([this, master, page] { m_sig_master_drop.emit(master, std::vector<std::string>{page}); });
        return true;
    }, false);
    add_controller(mdrop);

    m_keys = Gtk::EventControllerKey::create();
    m_keys->signal_key_pressed().connect(
        [this](guint kv, guint, Gdk::ModifierType st) { return on_key(kv, st); }, false);
    m_keys->signal_key_released().connect([this](guint kv, guint, Gdk::ModifierType) { on_key_up(kv); });
    add_controller(m_keys);

    // s010: typed text arrives through an input method (dead keys, compose,
    // non-Latin input) -- attached to the key controller only while editing,
    // so V / T / F / M stay tools otherwise.
    m_im = gtk_im_multicontext_new();
    // s158: what's being composed (a dead key's accent, a compose sequence,
    // an input method's word) shown at the caret as it's typed.
    gtk_im_context_set_use_preedit(m_im, TRUE);
    g_signal_connect(m_im, "commit", G_CALLBACK(+[](GtkIMContext*, const char* t, gpointer self) {
        auto* c = static_cast<Canvas*>(self);
        c->m_preedit.clear();
        if (c->editing() && t) c->edit_insert(t, "type", "Typing");
        c->queue_draw();
    }), this);
    g_signal_connect(m_im, "preedit-changed", G_CALLBACK(+[](GtkIMContext* im, gpointer self) {
        auto* c = static_cast<Canvas*>(self);
        gchar* str = nullptr;
        gint cur = 0;
        gtk_im_context_get_preedit_string(im, &str, nullptr, &cur);
        c->m_preedit = str ? str : "";
        c->m_preedit_cur = cur;
        g_free(str);
        c->queue_draw();
    }), this);
    g_signal_connect(m_im, "preedit-end", G_CALLBACK(+[](GtkIMContext*, gpointer self) {
        auto* c = static_cast<Canvas*>(self);
        c->m_preedit.clear();
        c->queue_draw();
    }), this);

    // s104: page images back from the worker -> drawn again (gathered 30 ms).
    m_pages.set_threaded(true);
    m_pg_disp.connect([this] {
        m_pg_signalled = false;
        if (m_pg_collect.connected()) return;
        m_pg_collect = Glib::signal_timeout().connect([this] {
            if (!m_pages.collect().empty()) queue_draw();
            return false;
        }, 30);
    });
    m_pages.set_notify([this] {
        if (!m_pg_signalled.exchange(true)) m_pg_disp.emit();
    });
}

Canvas::~Canvas() {
    m_pg_flush.disconnect();
    m_pg_collect.disconnect();
    m_pages.set_notify({});
    m_blink.disconnect();
    if (m_im) {
        g_signal_handlers_disconnect_by_data(m_im, this);
        g_object_unref(m_im);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// what is shown
// ─────────────────────────────────────────────────────────────────────────────
const std::vector<std::string>& Canvas::shown() const { return m_ids; }

void Canvas::show_pages(const std::vector<std::string>& ids) {
    const bool same = ids == m_ids;
    m_ids = ids;
    relayout();
    if (!same) {
        m_sel.clear();
        m_fit_pending = !m_follow_keep_zoom;   // s061: following the caret keeps the zoom
    }
    queue_draw();
}

void Canvas::model_changed() {
    relayout();
    // s010: editing survives the change (an undo) if its story and frame do.
    if (editing()) {
        const auto* fr = m_doc.as<core::Frame>(m_edit_frame);
        if (!edit_story() || !fr || fr->story != m_edit_story) end_edit();
        else {
            m_caret = core::clamp_pos(*edit_story(), m_caret);
            m_anchor = core::clamp_pos(*edit_story(), m_anchor);
        }
    }
    // Drop picks that are gone or no longer on a shown page.
    std::vector<std::string> keep;
    for (const auto& id : m_sel)
        if (core::frame_in_layout(m_doc, m_placed, id).w > 0) keep.push_back(id);
    m_sel = keep;
    queue_draw();
}

void Canvas::order_panels() {
    // Each facing master's two panels sit side by side in m_placed; the
    // front one first (their rects don't change, only which is asked first).
    for (std::size_t i = 0; i + 1 < m_placed.size(); ++i)
        if (m_placed[i].id == m_placed[i + 1].id && m_placed[i].side != m_front && m_placed[i + 1].side == m_front)
            std::swap(m_placed[i], m_placed[i + 1]);
}

void Canvas::front_panel(double x, double y) {
    const auto* p = core::panel_at(m_placed, x, y);
    if (!p || p->side == core::Side::Any || p->side == m_front) return;
    m_front = p->side;
    order_panels();
    queue_draw();
}

void Canvas::relayout() {
    m_placed = core::layout_pages(m_doc, m_ids);
    order_panels();
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
    // s100: a slow draw (100 ms+) says so (View area): what the performance arc is
    // working down. The reflow a keystroke causes is paid here, too.
    struct Timer {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::size_t pages;
        ~Timer() {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms >= 100)
                if (auto lg = log::get(log::Area::View)) lg->info("canvas draw {:.0f} ms ({} pages laid out)", ms, pages);
        }
    } timer{.pages = m_placed.size()};
    m_vp.set_widget(w, h);
    if (m_fit_pending && w > 0 && h > 0 && !m_placed.empty()) {
        m_fit_pending = false;
        m_vp.fit();
        Glib::signal_idle().connect_once([this] { m_sig_zoom.emit(); });
    }
    // s104: each page an image (proofed in it when Proof Colours is on).
    for (const auto& p : m_placed) draw_page(cr, p);
    if (m_show_grid)
        for (const auto& p : m_placed) draw_grid(cr, p);
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

    // s154: the zero point being dragged -- crosshairs.
    if (m_zero_preview) {
        std::vector<double> dash{6, 4};
        cr->set_dash(dash, 0);
        cr->set_source_rgba(0.0, 0.65, 0.8, 0.9);
        double sx = 0, sy = 0;
        m_vp.to_screen(m_zero_x, m_zero_y, sx, sy);
        cr->move_to(std::round(sx) + 0.5, 0); cr->line_to(std::round(sx) + 0.5, h);
        cr->move_to(0, std::round(sy) + 0.5); cr->line_to(w, std::round(sy) + 0.5);
        cr->stroke();
        cr->unset_dash();
    }
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
    // s014: the margins (magenta) and the page's columns (violet), from its master.
    if (const Rect mr = core::margin_rect(m_doc, p.id, p.side); mr.w > 0) {   // s150: a master's panel mirrors
        const Rect smr = m_vp.to_screen(Rect{p.r.x + mr.x, p.r.y + mr.y, mr.w, mr.h});
        cr->set_line_width(1);
        cr->set_source_rgba(0.85, 0.25, 0.65, 0.7);
        cr->rectangle(std::round(smr.x) + 0.5, std::round(smr.y) + 0.5, std::round(smr.w), std::round(smr.h));
        cr->stroke();
        const auto cols = core::page_columns(m_doc, p.id, p.side);
        cr->set_source_rgba(0.5, 0.35, 0.9, 0.6);
        for (std::size_t c = 0; c + 1 < cols.size(); ++c)
            for (double x : {cols[c].x1, cols[c + 1].x0}) {
                const double sx = std::round(sr.x + x * s) + 0.5;
                cr->move_to(sx, smr.y);
                cr->line_to(sx, smr.b());
            }
        cr->stroke();
    }
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

// s036: the baseline grid -- light blue rules across the margins, every
// step from the top margin to the bottom one (none when too dense to read).
void Canvas::draw_grid(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p) {
    const auto g = core::baseline_grid(m_doc, p.id);
    const Rect mr = core::margin_rect(m_doc, p.id, p.side);
    const double s = m_vp.scale();
    if (g.step <= 0 || mr.w <= 0 || g.step * s < 4) return;
    const Rect sr = m_vp.to_screen(p.r);
    cr->set_line_width(1);
    cr->set_source_rgba(0.35, 0.6, 0.95, 0.45);
    const double x0 = std::round(sr.x + mr.x * s), x1 = std::round(sr.x + mr.r() * s);
    for (double y = g.start; y <= mr.b() + 0.01; y += g.step) {
        const double sy = std::round(sr.y + y * s) + 0.5;
        cr->move_to(x0, sy);
        cr->line_to(x1, sy);
    }
    cr->stroke();
}

void Canvas::preview_guide(bool vertical, double at) {
    if (!m_preview) m_targets = core::snap_targets(m_doc, m_placed, {});   // s153: gathered as it comes out
    m_preview = true;
    m_preview_v = vertical;
    m_preview_at = at;
    // s153: snapped as it goes, like a dragged one.
    double dx = 0, dy = 0;
    m_vp.to_doc(vertical ? at : 0, vertical ? 0 : at, dx, dy);
    m_preview_doc = vertical ? dx : dy;
    m_snap_x = m_snap_y = NAN;
    if (m_snap) {
        const auto h = core::snap_any({m_preview_doc}, vertical ? m_targets.xs : m_targets.ys, 6 / m_vp.scale());
        if (h.hit()) {
            m_preview_doc = h.line;
            (vertical ? m_snap_x : m_snap_y) = h.line;
            double sx = 0, sy = 0;
            m_vp.to_screen(vertical ? h.line : 0, vertical ? 0 : h.line, sx, sy);
            m_preview_at = vertical ? sx : sy;
        }
    }
    queue_draw();
}

// s154: the zero point, dragged from the ruler corner -- snapped like a
// guide, crosshairs across the canvas; released over the canvas, one step.
void Canvas::preview_zero(double x, double y) {
    if (!m_zero_preview) m_targets = core::snap_targets(m_doc, m_placed, {});
    m_zero_preview = true;
    m_vp.to_doc(x, y, m_zero_x, m_zero_y);
    m_snap_x = m_snap_y = NAN;
    if (m_snap) {
        const double tol = 6 / m_vp.scale();
        const auto hx = core::snap_any({m_zero_x}, m_targets.xs, tol);
        const auto hy = core::snap_any({m_zero_y}, m_targets.ys, tol);
        if (hx.hit()) { m_zero_x = hx.line; m_snap_x = hx.line; }
        if (hy.hit()) { m_zero_y = hy.line; m_snap_y = hy.line; }
    }
    queue_draw();
}

void Canvas::drop_zero(double x, double y) {
    const bool was = m_zero_preview;
    m_zero_preview = false;
    m_snap_x = m_snap_y = NAN;
    queue_draw();
    if (!was || x < 0 || y < 0 || x > get_width() || y > get_height()) return;   // back on the rulers: nothing
    const auto z = core::zero_at(m_doc, m_placed, m_zero_x, m_zero_y);
    if (core::doc::set_ruler_zero(m_journal, m_doc, z.x, z.y)) m_sig_edited.emit();
    m_sig_view.emit();
}

void Canvas::reset_zero() {
    if (core::doc::set_ruler_zero(m_journal, m_doc, 0, 0)) m_sig_edited.emit();
    m_sig_view.emit();
}

// Released from a ruler: over a page, a guide at that spot (one step);
// over the pasteboard or back on the ruler, nothing.
void Canvas::drop_guide(bool vertical, double x, double y) {
    m_preview = false;
    m_snap_x = m_snap_y = NAN;
    queue_draw();
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    if (!std::isnan(m_preview_doc)) (vertical ? dx : dy) = m_preview_doc;   // s153: where it snapped
    m_preview_doc = NAN;
    front_panel(dx, dy);   // s150
    const auto page = core::page_at(m_placed, dx, dy);
    if (page.empty() || x < 0 || y < 0) return;
    for (const auto& p : m_placed)
        if (p.id == page) {
            core::doc::add_guide(m_journal, m_doc, page, vertical, vertical ? dx - p.r.x : dy - p.r.y);
            m_sig_edited.emit();
            return;
        }
}

// s104: the page as an image at this zoom (views::render_canvas_page), kept
// in m_pages. A change elsewhere -- another page edited, a zoom, Proof
// Colours -- is drawn on the page worker from a snapshot while the last
// image stands in (scaled to the zoom); the page being edited, or dragged
// on, is drawn on the spot. A page bigger than the view (zoomed in) is
// drawn on the spot, just the part in view. Off screen: not drawn at all.
void Canvas::draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p) {
    double ox = 0, oy = 0;
    m_vp.to_screen(0, 0, ox, oy);
    CanvasParams v;
    v.s = m_vp.scale();
    v.ox = ox;
    v.oy = oy;
    v.dir = m_dir;
    v.show_hidden = m_show_hidden;
    v.live_story = m_edit_story;
    for (const auto& q : m_placed) {   // s027: in a spread the bleed runs round it -- none at the spine
        if (q.id == p.id && q.side == p.side) continue;
        if ((q.master || p.master) && !(q.master && p.master && q.id == p.id)) continue;   // s150: a master's own spread
        v.spine_left = v.spine_left || std::abs(q.r.x + q.r.w - p.r.x) < 0.01;
        v.spine_right = v.spine_right || std::abs(p.r.x + p.r.w - q.r.x) < 0.01;
    }
    const PixBox full = canvas_page_box(m_doc, p, v);
    const int W = static_cast<int>(std::ceil(m_vp.widget_w())), H = static_cast<int>(std::ceil(m_vp.widget_h()));
    if (full.x >= W || full.y >= H || full.x + full.w <= 0 || full.y + full.h <= 0) return;   // off screen

    double ds = 1, dsy = 1;
    cairo_surface_get_device_scale(cairo_get_target(cr->cobj()), &ds, &dsy);
    // Whole when it's not much more than the view; else the part in view.
    PixBox box = full;
    const bool clipped = static_cast<double>(full.w) * full.h > 4.0 * std::max(1, W) * std::max(1, H);
    if (clipped) {
        const int x1 = std::min(full.x + full.w, W), y1 = std::min(full.y + full.h, H);
        box.x = std::max(full.x, 0);
        box.y = std::max(full.y, 0);
        box.w = x1 - box.x;
        box.h = y1 - box.y;
    }
    // What decides the image besides the page's key: the look, where the
    // page falls on the pixel grid, the thread ports' state, the proof.
    std::string var;
    {
        char buf[200];
        if (clipped)
            std::snprintf(buf, sizeof buf, "c%.9g|%.4f|%.4f|%d|%d|%g", v.s, box.x - ox, box.y - oy, box.w, box.h, ds);
        else
            std::snprintf(buf, sizeof buf, "w%.9g|%.4f|%.4f|%g", v.s, ox - std::floor(ox), oy - std::floor(oy), ds);
        var = buf;
        var += "|" + v.dir + "|" + v.live_story + (v.show_hidden ? "|h" : "|-") + (v.spine_left ? "L" : "-") +
               (v.spine_right ? "R" : "-") + "|p" + std::to_string(m_proof ? m_proof_gen : 0) +
               "|s" + std::to_string(core::doc::screen_gen());   // s107: CMYK through the press profile
        for (const auto& fid : m_doc.frames_on(p.id)) {
            const auto* f = m_doc.as<core::Frame>(fid);
            if (!f || f->kind != core::FrameKind::Text || f->story.empty()) continue;
            const auto [at, total] = core::thread_place(m_doc, fid);
            var += "|" + std::to_string(at) + "/" + std::to_string(total) + (flows().get(m_doc, f->story).overset ? "+" : "");
        }
        // s174: a picture still decoding shows as "Loading"; once it's in, the page is drawn again.
        auto owners = core::master_chain(m_doc, p.id);
        owners.push_back(p.id);
        for (const auto& o : owners)
            for (const auto& fid : m_doc.frames_on(o))
                if (const auto* f = m_doc.as<core::Frame>(fid); f && f->kind == core::FrameKind::Image)
                    if (const auto* a = m_doc.as<core::Asset>(f->asset)) {
                        const std::string file = core::resolve_asset(*a, m_dir);
                        if (const int n = file.empty() ? 0 : images::arrivals(file)) var += "|i" + std::to_string(n);
                    }
    }
    bool live = m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::Gutter || m_drag == Drag::Tab ||
                m_drag == Drag::Picture || m_drag == Drag::PicScale || clipped;
    if (!live && editing())
        for (const auto& fid : m_doc.frames_on(p.id))
            if (const auto* f = m_doc.as<core::Frame>(fid); f && f->story == m_edit_story) { live = true; break; }
    const std::shared_ptr<const plate::Proof> proof = m_proof;
    const core::Placed placed = p;
    const layout::ThumbStore::Render render = [v, placed, box, ds, proof](const core::Doc& d, layout::FlowCache& fl,
                                                                         const layout::ImageSource&) {
        return render_canvas_page(d, fl, v, placed, box, ds, proof.get());
    };
    const int pw = static_cast<int>(std::ceil(box.w * ds)), ph = static_cast<int>(std::ceil(box.h * ds));
    const std::string slot = p.side == core::Side::Left ? p.id + "#L" : p.side == core::Side::Right ? p.id + "#R" : p.id;   // s150
    const auto look = m_pages.look(m_doc, flows(), {}, slot, p.id, var, pw, ph, render,
                                   live ? layout::ThumbStore::Miss::Now : layout::ThumbStore::Miss::NowIfNone);
    if (m_pages.asking() && !m_pg_flush.connected())
        m_pg_flush = Glib::signal_idle().connect([this] {
            m_pages.flush(m_doc, flows(), {});
            return false;
        });
    if (const ImagePlace* at = canvas_image_place(look.surf)) {
        // Where the image's layout origin falls now: scaled from its zoom to this one.
        const double k = v.s / at->s;
        cr->save();
        cr->translate(ox + (at->x - at->ox) * k, oy + (at->y - at->oy) * k);
        cr->scale(k, k);
        cr->set_source(Cairo::make_refptr_for_instance<Cairo::Surface>(
                           new Cairo::Surface(cairo_surface_reference(look.surf.get()), true)), 0, 0);
        if (k != 1.0) cairo_pattern_set_filter(cairo_get_source(cr->cobj()), CAIRO_FILTER_BILINEAR);
        if (clipped) {   // a part, drawn for this view: only what it covers
            cr->rectangle(0, 0, cairo_image_surface_get_width(look.surf.get()) / ds,
                          cairo_image_surface_get_height(look.surf.get()) / ds);
            cr->clip();
        }
        cr->paint();
        cr->restore();
    }

    // The page's name, under it.
    const double s = m_vp.scale();
    const Rect sr = m_vp.to_screen(p.r);
    const auto fg = get_color();
    std::string name;
    if (p.master) name = core::master_label(m_doc, p.id) +
                         (p.side == core::Side::Left ? " \u2014 Left" : p.side == core::Side::Right ? " \u2014 Right" : "");   // s150
    else          name = "Page " + m_doc.folio(p.id);
    auto lay = create_pango_layout(name);
    lay->set_font_description(Pango::FontDescription("Sans 9"));
    int lw, lh;
    lay->get_pixel_size(lw, lh);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
    cr->move_to(sr.x + (sr.w - lw) / 2, sr.b() + 8);
    lay->show_in_cairo_context(cr);
    (void)s;
}

// A picked text frame shows its whole thread: out-port to in-port, in order.
void Canvas::draw_threads(const Cairo::RefPtr<Cairo::Context>& cr) {
    std::vector<std::string> stories;
    for (const auto& id : m_sel)
        if (const auto* f = m_doc.as<core::Frame>(id); f && f->kind == core::FrameKind::Text && !f->story.empty() &&
                                                       std::find(stories.begin(), stories.end(), f->story) == stories.end())
            stories.push_back(f->story);
    cr->set_line_width(1.5);
    cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.85);
    for (const auto& st : stories) {
        const auto chain = layout::thread_frames(m_doc, st);
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            const Rect a = port_rect(chain[i], true), b = port_rect(chain[i + 1], false);
            if (a.w <= 0 || b.w <= 0) continue;
            cr->move_to(a.cx(), a.cy());
            cr->line_to(b.cx(), b.cy());
        }
    }
    cr->stroke();
    cr->set_line_width(1);
}

Rect Canvas::port_rect(const std::string& frame, bool out) const {
    const Rect r = m_vp.to_screen(core::frame_in_layout(m_doc, m_placed, frame));
    if (r.w <= 24 || r.h <= 24) return {};
    const double ps = 7;
    // s060: outside the frame -- the in-port on the left edge near the top,
    // the out-port on the right edge near the bottom (clear of the text and
    // of the corner handles).
    return out ? Rect{std::round(r.r()) + 3.5, std::round(r.b()) - ps - 10.5, ps, ps}
               : Rect{std::round(r.x) - ps - 3.5, std::round(r.y) + 10.5, ps, ps};
}

std::string Canvas::out_port_hit(double sx, double sy) const {
    for (const auto& p : m_placed)
        for (const auto& id : m_doc.frames_on(p.id)) {
            if (!core::on_panel(m_doc, p, id)) continue;   // s150
            if (m_doc.as<core::Frame>(id)->kind != core::FrameKind::Text) continue;
            const Rect pr = port_rect(id, true);
            if (pr.w > 0 && Rect{pr.x - 3, pr.y - 3, pr.w + 6, pr.h + 6}.contains(sx, sy)) return id;
        }
    return {};
}

void Canvas::draw_overlays(const Cairo::RefPtr<Cairo::Context>& cr) {
    draw_threads(cr);
    cr->set_line_width(1);
    if (m_drag == Drag::Draw && (m_draw.w > 0 || m_draw.h > 0)) {   // the frame being drawn
        const Rect r = m_vp.to_screen(m_draw);
        std::vector<double> dash{5, 3};
        cr->set_dash(dash, 0);
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.9);
        const bool shape = m_tool == Tool::Shape && m_load.empty();
        if (shape && m_shape == ShapeTool::Line) {   // s198
            double ax, ay, bx, by;
            m_vp.to_screen(m_dx0, m_dy0, ax, ay);
            m_vp.to_screen(m_lx1, m_ly1, bx, by);
            cr->move_to(ax, ay);
            cr->line_to(bx, by);
        } else if (shape && m_shape == ShapeTool::Polygon) {   // s206
            double inner = 1;
            const auto P = drawn_polygon(inner);
            for (std::size_t i = 0; i < P.size(); ++i) {
                double sx, sy;
                m_vp.to_screen(P[i].x, P[i].y, sx, sy);
                if (i) cr->line_to(sx, sy); else cr->move_to(sx, sy);
            }
            if (!P.empty()) cr->close_path();
        } else if (shape && m_shape == ShapeTool::Oval && r.w > 0 && r.h > 0) {
            cr->save();
            cr->translate(r.x + r.w / 2, r.y + r.h / 2);
            cr->scale(r.w / 2, r.h / 2);
            cr->arc(0, 0, 1, 0, 2 * M_PI);
            cr->restore();
        } else {
            cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        }
        cr->stroke();
        cr->unset_dash();
    }
    // s199: the Pen's path so far -- its curves, its points, the next
    // segment to the pointer; on the first point, a ring: a click closes.
    if (m_tool == Tool::Pen && !m_pen.empty()) {
        const auto S = [&](double x, double y) { double sx, sy; m_vp.to_screen(x, y, sx, sy); return std::array<double, 2>{sx, sy}; };
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
        cr->set_line_width(1.5);
        auto p0 = S(m_pen[0].x, m_pen[0].y);
        cr->move_to(p0[0], p0[1]);
        for (std::size_t i = 1; i < m_pen.size(); ++i) {
            const auto a = S(m_pen[i - 1].ox, m_pen[i - 1].oy), b = S(m_pen[i].ix, m_pen[i].iy), c = S(m_pen[i].x, m_pen[i].y);
            cr->curve_to(a[0], a[1], b[0], b[1], c[0], c[1]);
        }
        if (m_drag != Drag::PenHandle && m_in) {   // the next segment, to the pointer
            const auto& l = m_pen.back();
            const auto a = S(l.ox, l.oy);
            const bool closing = m_pen.size() >= 2 && pen_near_first(m_mx, m_my);
            const auto e = closing ? p0 : std::array<double, 2>{m_mx, m_my};
            const auto b = closing ? S(m_pen[0].ix, m_pen[0].iy) : e;
            cr->curve_to(a[0], a[1], b[0], b[1], e[0], e[1]);
            cr->stroke();
            if (closing) {
                cr->arc(p0[0], p0[1], 6, 0, 2 * M_PI);
                cr->stroke();
            }
        } else {
            cr->stroke();
        }
        cr->set_line_width(1);
        for (std::size_t i = 0; i < m_pen.size(); ++i) {
            const auto& q = m_pen[i];
            const auto c = S(q.x, q.y);
            const bool last = i + 1 == m_pen.size();
            if (q.smooth && (last || i + 2 == m_pen.size())) {   // the newest point's handles
                const auto hi = S(q.ix, q.iy), ho = S(q.ox, q.oy);
                cr->move_to(hi[0], hi[1]);
                cr->line_to(ho[0], ho[1]);
                cr->stroke();
                for (const auto& h : {hi, ho}) { cr->arc(h[0], h[1], 2.5, 0, 2 * M_PI); cr->fill(); }
            }
            cr->rectangle(std::round(c[0]) - 3, std::round(c[1]) - 3, 6, 6);
            if (last) cr->fill();
            else {
                cr->set_source_rgba(1, 1, 1, 1);
                cr->fill_preserve();
                cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
                cr->stroke();
            }
        }
    }
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
        // s018: a wrapping frame shows its wrap edge (the offsets), dotted.
        const auto* f = m_doc.as<core::Frame>(id);
        const auto* fa = f && f->wrap == core::Wrap::Shape ? m_doc.as<core::Asset>(f->asset) : nullptr;
        const auto bands = fa ? core::shape_bands(*f, *fa)
                         : f && f->wrap == core::Wrap::Shape && core::doc::has_outline(*f) ? core::outline_bands(*f, m_doc)   // s200
                                                                                            : std::vector<core::Band>{};
        if (!bands.empty()) {   // s173: the outline it wraps to, dotted -- each run of touching bands one shape
            const Rect fr = core::frame_in_layout(m_doc, m_placed, id);
            const auto& o = f->wrap_off;
            std::vector<double> dash{2, 3};
            cr->set_dash(dash, 0);
            cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.7);
            std::size_t a = 0;
            while (a < bands.size()) {
                std::size_t b = a + 1;
                while (b < bands.size() && bands[b].y0 <= bands[b - 1].y1 + 0.01) ++b;
                const auto pt = [&](double x, double y) {
                    const Rect s = m_vp.to_screen({fr.x + x, fr.y + y, 0, 0});
                    return std::array<double, 2>{std::round(s.x) + 0.5, std::round(s.y) + 0.5};
                };
                std::vector<std::array<double, 2>> poly;
                for (std::size_t k = a; k < b; ++k) {   // down the left
                    const double y0 = k == a ? bands[k].y0 - o.top : bands[k].y0, y1 = k + 1 == b ? bands[k].y1 + o.bottom : bands[k].y1;
                    poly.push_back(pt(bands[k].x0 - o.left, y0));
                    poly.push_back(pt(bands[k].x0 - o.left, y1));
                }
                for (std::size_t k = b; k-- > a;) {   // up the right
                    const double y0 = k == a ? bands[k].y0 - o.top : bands[k].y0, y1 = k + 1 == b ? bands[k].y1 + o.bottom : bands[k].y1;
                    poly.push_back(pt(bands[k].x1 + o.right, y1));
                    poly.push_back(pt(bands[k].x1 + o.right, y0));
                }
                cr->move_to(poly[0][0], poly[0][1]);
                for (std::size_t k = 1; k < poly.size(); ++k) cr->line_to(poly[k][0], poly[k][1]);
                cr->close_path();
                cr->stroke();
                a = b;
            }
            cr->unset_dash();
        } else if (f && f->wrap != core::Wrap::None) {
            const Rect fr = core::frame_in_layout(m_doc, m_placed, id);
            const auto& o = f->wrap_off;
            const Rect w = m_vp.to_screen({fr.x - o.left, fr.y - o.top, fr.w + o.left + o.right, fr.h + o.top + o.bottom});
            std::vector<double> dash{2, 3};
            cr->set_dash(dash, 0);
            cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.7);
            cr->rectangle(std::round(w.x) + 0.5, std::round(w.y) + 0.5, std::round(w.w), std::round(w.h));
            cr->stroke();
            cr->unset_dash();
        }
    }
    if (editing()) { draw_edit(cr); draw_tab_strip(cr); }   // s034
    // s186: the Direct tool -- the whole picture: the part its frame crops
    // ghosted, its edge and handles in brown (InDesign's colour for content).
    Rect dfl, dpl;
    const bool direct = m_tool == Tool::Direct && !m_direct.empty() && !editing() && direct_pic(m_direct, dfl, dpl);
    if (direct) {
        const Rect fs = m_vp.to_screen(dfl), ps = m_vp.to_screen(dpl);
        const auto* f = m_doc.as<core::Frame>(m_direct);
        const auto* a = f ? m_doc.as<core::Asset>(f->asset) : nullptr;
        if (auto pic = a ? images::get(core::resolve_asset(*a, m_dir), a->stamp) : Cairo::RefPtr<Cairo::ImageSurface>{}) {
            cr->save();
            cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);   // outside the frame only
            cr->rectangle(ps.x, ps.y, ps.w, ps.h);
            cr->rectangle(fs.x, fs.y, fs.w, fs.h);
            cr->clip();
            images::paint(cr, pic, ps.x, ps.y, ps.w, ps.h, 0.35);
            cr->restore();
        }
        constexpr double kBrown[3] = {0.62, 0.38, 0.13};
        cr->set_line_width(1);
        cr->set_source_rgba(kBrown[0], kBrown[1], kBrown[2], 0.95);
        cr->rectangle(std::round(ps.x) + 0.5, std::round(ps.y) + 0.5, std::round(ps.w), std::round(ps.h));
        cr->stroke();
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.6);   // the frame, thin, no handles
        cr->rectangle(std::round(fs.x) + 0.5, std::round(fs.y) + 0.5, std::round(fs.w), std::round(fs.h));
        cr->stroke();
        for (const auto& p : core::handle_points(ps)) {
            const double x = std::round(p[0]) - 3.5, y = std::round(p[1]) - 3.5;
            cr->set_source_rgb(1, 1, 1);
            cr->rectangle(x, y, 7, 7);
            cr->fill_preserve();
            cr->set_source_rgba(kBrown[0], kBrown[1], kBrown[2], 1);
            cr->stroke();
        }
    }
    double sdx = 0, sdy = 0;   // s200: a shape's points on show -- they, not the box's handles
    const bool direct_shp = m_tool == Tool::Direct && !m_direct.empty() && !editing() && direct_shape(m_direct, sdx, sdy);
    if (!m_sel.empty() && !editing() && !direct && !direct_shp) {
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
    // s200: Direct on a shape -- its outline, its points (squares corners,
    // rounds smooth; the picked one filled) and the picked one's handles.
    if (double odx = 0, ody = 0; m_tool == Tool::Direct && !m_direct.empty() && direct_shape(m_direct, odx, ody)) {
        const auto* f = m_doc.as<core::Frame>(m_direct);
        bool open = false;
        const auto P = core::doc::shape_points(*f, open);
        const auto S = [&](double x, double y) { double sx, sy; m_vp.to_screen(x + odx, y + ody, sx, sy); return std::array<double, 2>{sx, sy}; };
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
        cr->set_line_width(1);
        for (const auto& o : core::doc::outline_ops(*f, 0, 0)) {
            if (o.op == 'M') { const auto a = S(o.v[0], o.v[1]); cr->move_to(a[0], a[1]); }
            else if (o.op == 'L') { const auto a = S(o.v[0], o.v[1]); cr->line_to(a[0], a[1]); }
            else if (o.op == 'C') {
                const auto a = S(o.v[0], o.v[1]), b = S(o.v[2], o.v[3]), c = S(o.v[4], o.v[5]);
                cr->curve_to(a[0], a[1], b[0], b[1], c[0], c[1]);
            } else cr->close_path();
        }
        cr->stroke();
        if (m_pt >= 0 && m_pt < static_cast<int>(P.size())) {
            const auto& p = P[static_cast<std::size_t>(m_pt)];
            const auto c = S(p.x, p.y);
            for (const auto& h : {S(p.ix, p.iy), S(p.ox, p.oy)}) {
                if (std::hypot(h[0] - c[0], h[1] - c[1]) < 0.5) continue;
                cr->move_to(c[0], c[1]);
                cr->line_to(h[0], h[1]);
                cr->stroke();
                cr->arc(h[0], h[1], 3, 0, 2 * M_PI);
                cr->fill();
            }
        }
        for (int i = 0; i < static_cast<int>(P.size()); ++i) {
            const auto& p = P[static_cast<std::size_t>(i)];
            const auto c = S(p.x, p.y);
            if (p.smooth) cr->arc(c[0], c[1], 3.5, 0, 2 * M_PI);
            else cr->rectangle(std::round(c[0]) - 3.5, std::round(c[1]) - 3.5, 7, 7);
            if (i == m_pt || m_pts_sel.count(i)) cr->fill();   // s207: every picked one
            else {
                cr->set_source_rgba(1, 1, 1, 1);
                cr->fill_preserve();
                cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
                cr->stroke();
            }
        }
    }
    // Loaded (s009): the file's name beside the pointer.
    if (!m_load.empty() && m_in) {
        // s063: and how many more wait; the next one's picture beside it.
        const std::string more = m_load.size() > 1 ? "  +" + std::to_string(m_load.size() - 1) + " more" : std::string{};
        auto lay = create_pango_layout(m_load.current().name + more);
        lay->set_font_description(Pango::FontDescription("Sans 9"));
        int lw, lh;
        lay->get_pixel_size(lw, lh);
        const double x = std::round(m_mx + 16), y = std::round(m_my + 14);
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.92);
        cr->rectangle(x - 5, y - 2, lw + 10, lh + 4);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
        cr->move_to(x, y);
        lay->show_in_cairo_context(cr);
        // s063: a small copy of the picture a click will put down.
        const auto& cur = m_load.current();
        if (auto pic = images::get(cur.path, cur.stamp)) {
            const double pw = pic->get_width(), ph = pic->get_height();
            if (pw > 0 && ph > 0) {
                const double k = std::min(64.0 / pw, 48.0 / ph);
                const double tw = pw * k, th = ph * k, ty = y + lh + 6;
                cr->set_source_rgba(0, 0, 0, 0.25);
                cr->rectangle(x - 5 + 1, ty + 1, tw + 4, th + 4);
                cr->fill();
                cr->set_source_rgb(1, 1, 1);
                cr->rectangle(x - 5, ty, tw + 4, th + 4);
                cr->fill();
                images::paint(cr, pic, x - 3, ty + 2, tw, th);
            }
        }
    }
    // s180: the eyedropper -- the colour under the pointer, its hex, and how to stop.
    if ((m_take || m_tool == Tool::Eyedropper) && m_in) {   // s198: the tool too
        char hx[64];
        if (m_take_under) std::snprintf(hx, sizeof hx, "#%06X", *m_take_under & 0xFFFFFFu);
        else std::snprintf(hx, sizeof hx, "%s", "Over a page");
        const char* how = m_take ? "  \u00b7  click to take, Esc to stop"
                        : m_sel.empty() ? "  \u00b7  click: a swatch"
                                        : "  \u00b7  click: fill, Shift: stroke, Alt: a swatch";
        auto lay = create_pango_layout(std::string(hx) + how);
        lay->set_font_description(Pango::FontDescription("Sans 9"));
        int lw, lh;
        lay->get_pixel_size(lw, lh);
        const double cs = 26, x = std::round(m_mx + 16), y = std::round(m_my + 14);
        cr->set_source_rgba(0, 0, 0, 0.25);   // the chip, with a shadow and a two-tone edge
        cr->rectangle(x + 1, y + 1, cs, cs);
        cr->fill();
        if (m_take_under) {
            const auto c = *m_take_under;
            cr->set_source_rgb(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
        } else {
            cr->set_source_rgb(0.85, 0.85, 0.85);
        }
        cr->rectangle(x, y, cs, cs);
        cr->fill();
        cr->set_line_width(1);
        cr->set_source_rgb(1, 1, 1);
        cr->rectangle(x + 0.5, y + 0.5, cs - 1, cs - 1);
        cr->stroke();
        cr->set_source_rgba(0, 0, 0, 0.6);
        cr->rectangle(x - 0.5, y - 0.5, cs + 1, cs + 1);
        cr->stroke();
        const double tx = x + cs + 8, ty = y + (cs - lh) / 2;
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.92);
        cr->rectangle(tx - 5, ty - 2, lw + 10, lh + 4);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
        cr->move_to(tx, ty);
        lay->show_in_cairo_context(cr);
    }
    if (m_drag == Drag::Marquee || m_drag == Drag::PointMarquee) {   // s207: the points' too
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.12);
        cr->rectangle(m_marquee.x, m_marquee.y, m_marquee.w, m_marquee.h);
        cr->fill_preserve();
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.8);
        cr->stroke();
        // s169: the master items it would override -- outlined in orange.
        if (m_override_marquee) {
            cr->set_source_rgba(0.93, 0.47, 0.05, 0.95);
            cr->set_line_width(1.5);
            for (const auto& h : m_override_hits)
                for (const auto& p : m_placed) {
                    if (p.id != h.page) continue;
                    const auto* f = m_doc.as<core::Frame>(h.item);
                    if (!f) continue;
                    const Rect r = m_vp.to_screen(Rect{p.r.x + f->x + core::across_dx(m_doc, *f, m_doc.side_of(p.id)), p.r.y + f->y, f->w, f->h});
                    cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
                }
            cr->stroke();
        }
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
    if (m_take) {   // s180: the eyedropper -- a click on a page takes its colour
        m_drag = Drag::None;
        const auto c = colour_at(x, y);
        if (!c) return;   // off the pages: still waiting
        auto done = std::move(m_take);
        m_take = nullptr;
        m_take_under.reset();
        update_cursor();
        queue_draw();
        done(c);
        return;
    }
    read_mods();

    double px0, py0;
    m_vp.to_doc(x, y, px0, py0);
    front_panel(px0, py0);   // s150

    // s010: clicks counted by hand (double-click = edit / pick a word).
    const gint64 now = g_get_monotonic_time();
    if (now - m_last_press < 400000 && std::hypot(x - m_last_px, y - m_last_py) < 5) ++m_clicks;
    else m_clicks = 1;
    m_last_press = now;
    m_last_px = x;
    m_last_py = y;
    // s198: the Eyedropper tool -- the colour here onto the picks' fill
    // (Shift: stroke); Alt, or nothing picked: just the swatch.
    if (m_tool == Tool::Eyedropper) {
        m_drag = Drag::None;
        const auto c = colour_at(x, y);
        if (!c) return;
        core::doc::take_colour_onto(m_journal, m_doc, m_mods_alt ? std::vector<std::string>{} : m_sel, *c, m_mods_shift);
        m_sig_edited.emit();
        queue_draw();
        return;
    }
    // s199: the Pen -- a click puts down a corner, a drag pulls out its
    // handles; on the first point it closes, a double-click ends it open.
    if (m_tool == Tool::Pen) {
        m_drag = Drag::None;
        if (m_pen.size() >= 2 && pen_near_first(x, y)) { pen_finish(true, true); return; }
        if (!m_pen.empty() && m_clicks >= 2) { pen_finish(true); return; }
        if (m_pen.empty()) {   // s208: on an open path's end -- carry on from it
            m_pen_cont.clear();
            m_pen_rev = false;
            for (const auto& p : m_placed) {
                if (!m_pen_cont.empty()) break;
                for (const auto& fid : m_doc.frames_on(p.id)) {
                    const auto* f = m_doc.as<core::Frame>(fid);
                    if (!f || f->kind != core::FrameKind::Shape || f->shape != core::ShapeKind::Path || !f->open || f->across ||
                        f->path.size() < 2 || !core::on_panel(m_doc, p, fid))
                        continue;
                    auto P = core::doc::path_points(*f);
                    for (auto& q : P) { q.x += p.r.x; q.ix += p.r.x; q.ox += p.r.x; q.y += p.r.y; q.iy += p.r.y; q.oy += p.r.y; }
                    const auto near = [&](const core::PathPoint& q) {
                        double sx, sy;
                        m_vp.to_screen(q.x, q.y, sx, sy);
                        return std::hypot(sx - x, sy - y) <= 6;
                    };
                    if (!near(P.back()) && !near(P.front())) continue;
                    m_pen_rev = !near(P.back());
                    m_pen = m_pen_rev ? core::doc::reversed_path(P) : P;
                    m_pen_cont = fid;
                    m_pen_owner = p.id;
                    m_pen_ox = p.r.x;
                    m_pen_oy = p.r.y;
                    break;
                }
            }
            if (!m_pen_cont.empty()) {   // a drag from the end pulls its handle out
                m_dx0 = m_pen.back().x;
                m_dy0 = m_pen.back().y;
                m_drag = Drag::PenHandle;
                pick({m_pen_cont});
                queue_draw();
                return;
            }
        }
        if (m_pen.empty()) {
            const core::Placed* at = nullptr;
            for (const auto& p : m_placed)
                if (p.r.contains(px0, py0)) { at = &p; break; }
            if (!at) return;   // off the pages
            m_pen_owner = at->id;
            m_pen_ox = at->r.x;
            m_pen_oy = at->r.y;
        }
        double qx = px0, qy = py0;
        if (m_mods_shift && !m_pen.empty()) {   // Shift: at 45-degree steps from the last point
            const auto& l = m_pen.back();
            const double a = std::round(std::atan2(qy - l.y, qx - l.x) / (M_PI / 4)) * (M_PI / 4);
            const double len = std::hypot(qx - l.x, qy - l.y);
            qx = l.x + len * std::cos(a);
            qy = l.y + len * std::sin(a);
        }
        m_pen.push_back(core::doc::corner_point(qx, qy));
        m_dx0 = qx;
        m_dy0 = qy;
        m_drag = Drag::PenHandle;
        queue_draw();
        return;
    }
    // s034: on the tab ruler -- a stop: drag it (double-click: its kind,
    // Shift+double-click: its leader); elsewhere: a new one.
    // s157: "Reset to style" -- the paragraphs' own stops dropped.
    if (const auto t = tab_strip(); t.ok && t.own && t.reset.contains(x, y)) {
        const auto [a, b] = edit_range();
        core::doc::clear_para_local(m_journal, m_doc, m_edit_story, a, b, {"tabs"});
        m_sig_edited.emit();
        m_drag = Drag::None;
        queue_draw();
        return;
    }
    if (const auto t = tab_strip(); t.ok && t.screen.contains(x, y)) {
        const int hit = tab_hit(t, x);
        if (hit >= 0 && m_clicks >= 2) {   // s157: the paragraph's own stops
            const bool leader = m_mods_shift;
            auto tabs = t.tabs;
            auto& stop = tabs[static_cast<std::size_t>(hit)];
            if (leader) stop.leader = stop.leader.empty() ? "." : "";
            else stop.align = static_cast<core::TabAlign>((static_cast<int>(stop.align) + 1) % 4);
            const auto [a, b] = edit_range();
            core::doc::set_para_tabs(m_journal, m_doc, m_edit_story, a, b, leader ? "Tab leader" : "Tab alignment", tabs);
            m_sig_edited.emit();
            m_drag = Drag::None;
            queue_draw();
            return;
        }
        m_tab = hit;
        m_tab_x = hit >= 0 ? t.x0 + t.tabs[static_cast<std::size_t>(hit)].pos * t.scale : x;
        m_tab_off = false;
        m_drag = Drag::Tab;
        queue_draw();
        return;
    }
    {
        const auto under = core::hit_frame(m_doc, m_placed, px0, py0, 3 / m_vp.scale());
        const auto* uf = m_doc.as<core::Frame>(under);
        const bool text_frame = uf && uf->kind == core::FrameKind::Text && !uf->story.empty();
        if (editing()) {
            if (text_frame && uf->story == m_edit_story) {   // a click in the story: the caret
                const auto at = hit_story(under, px0, py0);
                if (m_clicks >= 3) {   // s061: triple-click -- the paragraph
                    const auto& para = edit_story()->paras[static_cast<std::size_t>(at.para)];
                    m_anchor = {at.para, 0};
                    set_caret({at.para, static_cast<int>(para.text.size())}, true);
                } else if (m_clicks >= 2) {
                    if (note_click(under, px0, py0)) return;   // s114
                    const auto [a, b] = core::word_at(*edit_story(), at);
                    m_anchor = a;
                    set_caret(b, true);
                } else {
                    set_caret(at, m_mods_shift);
                }
                m_edit_frame = under;
                m_drag = Drag::Text;
                return;
            }
            end_edit();   // a click anywhere else ends editing, then does its own thing
        }
        // s217: the Text tool on a shape's outline (not a text frame's) -- its words set along it, from there.
        if (m_tool == Tool::Text && m_load.empty() && uf && uf->kind != core::FrameKind::Text && uf->kind != core::FrameKind::Image &&
            (core::doc::has_outline(*uf) || uf->kind == core::FrameKind::Shape)) {
            const Rect fl = core::frame_in_layout(m_doc, m_placed, under);
            const auto tr = core::doc::path_track(*uf);
            double dd = 0;
            const double s = tr.nearest(px0 - (fl.x - uf->x), py0 - (fl.y - uf->y), &dd);
            if (dd * m_vp.scale() <= 8 && core::doc::set_text_on_path(m_journal, m_doc, {under}, true, s)) {
                m_drag = Drag::None;
                relayout();
                pick({under});
                m_sig_edited.emit();
                Glib::signal_timeout().connect_once([this] { open_story_editor(); }, 150);   // its words typed there, once the navigator has its story (the canvas edits text in boxes)
                return;
            }
        }
        // s217: text on a path is typed in the Story Editor (double-click / the Text tool open it).
        if (text_frame && uf->on_path && m_load.empty() && ((m_tool == Tool::Select && m_clicks >= 2) || m_tool == Tool::Text)) {
            m_drag = Drag::None;
            pick({under});
            Glib::signal_timeout().connect_once([this] { open_story_editor(); }, 150);
            return;
        }
        if (m_thread_from.empty() && m_load.empty() && text_frame && !m_mods_alt && out_port_hit(x, y).empty() &&
            ((m_tool == Tool::Select && m_clicks >= 2) || m_tool == Tool::Text)) {
            begin_edit(under, hit_story(under, px0, py0));
            if (m_clicks >= 2 && note_click(under, px0, py0)) return;   // s114
            m_drag = Drag::Text;
            return;
        }
    }
    // Loaded with a thread: this click picks where the text continues.
    if (!m_thread_from.empty()) {
        const auto to = core::hit_frame(m_doc, m_placed, px0, py0, 3 / m_vp.scale());
        const auto from = m_thread_from;
        m_thread_from.clear();
        m_drag = Drag::None;
        if (!to.empty() && core::doc::thread_frames(m_journal, m_doc, from, to)) {
            pick({from, to});
            m_sig_edited.emit();
        }
        update_cursor();
        queue_draw();
        return;
    }
    // A text frame's out-port: click to load the thread; Alt-click to cut it.
    if (const auto port = out_port_hit(x, y); !port.empty()) {
        m_drag = Drag::None;
        if (m_mods_alt) {
            if (core::doc::unthread_after(m_journal, m_doc, port)) m_sig_edited.emit();
        } else {
            m_thread_from = port;
        }
        update_cursor();
        queue_draw();
        return;
    }
    // s186 (G2): the Direct tool -- the picture inside its frame.
    if (m_tool == Tool::Direct && m_load.empty()) {
        m_drag = Drag::None;
        Rect fl, pl;
        // s200: the shape on show -- a handle of its picked point, a point,
        // or (double-click) its outline.
        double odx = 0, ody = 0;
        if (!m_direct.empty() && direct_shape(m_direct, odx, ody)) {
            const auto* f = m_doc.as<core::Frame>(m_direct);
            bool open = false;
            const auto P = core::doc::shape_points(*f, open);
            const auto near = [&](double ax, double ay) {
                double sx, sy;
                m_vp.to_screen(ax + odx, ay + ody, sx, sy);
                return std::hypot(sx - x, sy - y) <= 6;
            };
            int part = -1, at = -1;
            if (m_pt >= 0 && m_pt < static_cast<int>(P.size())) {
                const auto& p = P[static_cast<std::size_t>(m_pt)];
                if (!(p.ix == p.x && p.iy == p.y) && near(p.ix, p.iy)) { part = 1; at = m_pt; }
                else if (!(p.ox == p.x && p.oy == p.y) && near(p.ox, p.oy)) { part = 2; at = m_pt; }
            }
            for (int i = 0; part < 0 && i < static_cast<int>(P.size()); ++i)
                if (near(P[static_cast<std::size_t>(i)].x, P[static_cast<std::size_t>(i)].y)) { part = 0; at = i; }
            if (part >= 0) {
                if (part == 0 && m_mods_shift && !m_mods_alt) {   // s207: Shift-click -- one more point picked (or one less)
                    if (m_pts_sel.erase(at)) {
                        if (m_pt == at) m_pt = m_pts_sel.empty() ? -1 : *m_pts_sel.begin();
                        queue_draw();
                        return;
                    }
                    m_pts_sel.insert(at);
                } else if (part == 0 && !m_pts_sel.count(at)) {
                    m_pts_sel = {at};
                }
                m_pt = at;
                if (part == 0 && m_mods_alt) {   // Alt-click a point: smooth <-> corner
                    if (core::doc::path_toggle_point(m_journal, m_doc, m_direct, at)) m_sig_edited.emit();
                    queue_draw();
                    return;
                }
                m_pt_part = part;
                m_pts0 = P;
                m_pts_open = open;
                m_dx0 = px0;
                m_dy0 = py0;
                core::doc::frames_begin(m_journal, m_doc, {m_direct}, part == 0 ? "Move point" : "Move handle");
                m_begun = true;
                m_drag = Drag::PathPoint;
                queue_draw();
                return;
            }
            if (m_clicks >= 2) {   // a double-click on the outline: a point there
                const auto h = core::doc::outline_nearest(*f, px0 - odx, py0 - ody);
                if (h.k >= 0 && h.dist * m_vp.scale() <= 6 && core::doc::path_add_point(m_journal, m_doc, m_direct, h.k, h.t)) {
                    m_pt = h.k + 1;
                    m_pts_sel = {m_pt};
                    m_sig_edited.emit();
                    queue_draw();
                    return;
                }
            }
        }
        {   // a shape: its points on show
            const auto hit = core::hit_frame(m_doc, m_placed, px0, py0, 3 / m_vp.scale());
            // s207: on the shown shape (or nothing) -- a marquee picks its points
            // (a text frame under it doesn't stop it -- only another shape or a picture is picked instead)
            Rect hfl, hpl;
            double hdx = 0, hdy = 0;
            const auto* hf = m_doc.as<core::Frame>(hit);
            const bool plain_text = hf && hf->kind == core::FrameKind::Text && !core::doc::has_outline(*hf);   // s210: a column's box
            const bool other = !hit.empty() && hit != m_direct && !plain_text && (direct_shape(hit, hdx, hdy) || direct_pic(hit, hfl, hpl));
            if (!m_direct.empty() && !other && direct_shape(m_direct, odx, ody) && m_clicks < 2) {
                if (!m_mods_shift) { m_pts_sel.clear(); m_pt = -1; }
                m_pts0.clear();
                for (const int k : m_pts_sel) m_pts0.push_back(core::PathPoint{static_cast<double>(k)});   // the picks before it
                m_marquee = {x, y, 0, 0};
                m_drag = Drag::PointMarquee;
                queue_draw();
                return;
            }
            if (!hit.empty() && direct_shape(hit, odx, ody)) {
                if (hit != m_direct) { m_pt = -1; m_pts_sel.clear(); }
                m_direct = hit;
                pick({hit});
                queue_draw();
                return;
            }
        }
        const auto start = [&](const std::string& fid, Drag kind, const char* label) {
            m_pic0 = {pl.x - fl.x, pl.y - fl.y, pl.w, pl.h};   // the frame's own coordinates
            m_pic_frame = fid;
            core::doc::frames_begin(m_journal, m_doc, {fid}, label);
            m_begun = true;
            m_drag = kind;
        };
        // A handle of the picture on show: scale it.
        if (!m_direct.empty() && direct_pic(m_direct, fl, pl)) {
            const auto h = core::handle_hit(m_vp.to_screen(pl), x, y);
            if (h != core::Handle::None) {
                m_pic_handle = h;
                start(m_direct, Drag::PicScale, "Scale picture");
                return;
            }
        }
        // In a picture's frame -- or the part of the shown one its frame crops: move it.
        const auto hit = core::hit_frame(m_doc, m_placed, px0, py0, 3 / m_vp.scale());
        std::string target = hit;
        if (!m_direct.empty() && (hit.empty() || hit == m_direct) && direct_pic(m_direct, fl, pl) && pl.contains(px0, py0))
            target = m_direct;
        if (!target.empty() && direct_pic(target, fl, pl)) {
            m_direct = target;
            pick({target});
            start(target, Drag::Picture, "Move picture");
            return;
        }
        // Anything else: picked as a frame (nothing to show), or nothing.
        m_direct.clear();
        pick(hit.empty() ? std::vector<std::string>{} : std::vector<std::string>{hit});
        queue_draw();
        return;
    }
    // A drawing tool -- or a loaded image (s009) -- drag out a frame on a page.
    if (m_tool != Tool::Select || !m_load.empty()) {
        m_draw_page = core::page_at(m_placed, px0, py0);
        m_drag = m_draw_page.empty() ? Drag::None : Drag::Draw;
        m_dx0 = px0;
        m_dy0 = py0;
        m_draw = {};
        m_targets = core::snap_targets(m_doc, m_placed, {});
        return;
    }

    // s012: a picked text frame's gutter -- drag it to share the width
    // differently between the columns either side.
    if (const int k = gutter_hit(x, y); k >= 0) {
        const auto& f = *m_doc.as<core::Frame>(m_sel.front());
        m_col0.clear();
        for (int c = 0; c < core::column_count(f); ++c) m_col0.push_back(core::column_width(f, c));
        m_gutter = k;
        m_drag = Drag::Gutter;
        return;
    }
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
    if (m_show_guides && !m_guides_locked) {   // s153: a locked guide isn't picked
        const auto g = core::hit_guide(m_doc, m_placed, dx, dy, 4 / m_vp.scale());
        if (!g.empty()) {
            m_drag = Drag::Guide;
            m_guide = g;
            const auto& gd = *m_doc.as<core::Guide>(g);
            m_guide0 = gd.pos;
            m_guide_off = false;
            // s153: it snaps as it goes -- to everything but itself.
            m_guide_base = 0;
            for (const auto& p : m_placed)
                if (p.id == gd.owner) { m_guide_base = gd.vertical ? p.r.x : p.r.y; break; }
            m_targets = core::snap_targets(m_doc, m_placed, {g});
            return;
        }
    }
    // s035: Alt+drag on a picture moves it inside its frame (a crop).
    if (m_mods_alt && !m_mods_ctrl && m_tool == Tool::Select) {
        const auto hit = core::hit_frame(m_doc, m_placed, dx, dy, 0);
        const auto* f = m_doc.as<core::Frame>(hit);
        const auto* a = f ? m_doc.as<core::Asset>(f->asset) : nullptr;
        if (f && a && f->kind == core::FrameKind::Image) {
            const auto nat = core::picture_natural(*a);
            const double iw = a->px_w > 0 ? a->px_w : nat.w, ih = a->px_h > 0 ? a->px_h : nat.h;
            m_pic0 = core::picture_rect({0, 0, f->w, f->h}, *f, iw, ih);
            m_pic_frame = hit;
            pick({hit});
            core::doc::frames_begin(m_journal, m_doc, {hit}, "Move picture");
            m_begun = true;
            m_drag = Drag::Picture;
            return;
        }
    }
    // s025: Ctrl+Shift+click a master's item on a page: override it there --
    // the page's own copy, picked (InDesign's gesture).
    if (m_mods_ctrl && m_mods_shift) {
        core::Doc::Id page;
        const auto mf = core::hit_master_item(m_doc, m_placed, dx, dy, 3 / m_vp.scale(), &page);
        if (!mf.empty() && core::hit_frame(m_doc, m_placed, dx, dy, 0).empty()) {
            const auto made = core::doc::override_master_item(m_journal, m_doc, page, mf);
            if (!made.empty()) {
                relayout();
                pick({made});
                m_sig_edited.emit();
                if (auto lg = log::get(log::Area::View)) lg->info("override {} on {} -> {}", mf, page, made);
            }
            m_drag = Drag::None;
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
    // s169: Ctrl+Shift+drag -- the master items it touches overridden, at once.
    m_override_marquee = m_mods_ctrl && m_mods_shift;
    m_sel0 = adding && !m_override_marquee ? m_sel : std::vector<std::string>{};
    m_marquee = {x, y, 0, 0};
    if (!adding || m_override_marquee) pick({});
}

// Shift / Alt from the gesture's own event, at press and on every update. Not
// at release: some platforms clear Alt before the release arrives (Curvz).
void Canvas::read_mods() {
    const auto st = m_gdrag->get_current_event_state();
    m_mods_shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
    m_mods_alt   = (st & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK;
    m_mods_ctrl  = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
}

void Canvas::on_drag_update(double ox, double oy) {
    read_mods();
    const double s = m_vp.scale();
    switch (m_drag) {
    case Drag::None:
        return;
    case Drag::PathPoint: {   // s200: a shape's point (with its handles), or one handle
        if (m_pt < 0 || m_pt >= static_cast<int>(m_pts0.size())) return;
        auto P = m_pts0;
        auto& p = P[static_cast<std::size_t>(m_pt)];
        const double ddx = ox / s, ddy = oy / s;
        if (m_pt_part == 0) {   // s207: every picked point moves with it
            std::set<int> move = m_pts_sel;
            move.insert(m_pt);
            for (const int k : move) {
                if (k < 0 || k >= static_cast<int>(P.size())) continue;
                auto& q = P[static_cast<std::size_t>(k)];
                q.x += ddx; q.ix += ddx; q.ox += ddx;
                q.y += ddy; q.iy += ddy; q.oy += ddy;
            }
        } else {
            double& hx = m_pt_part == 1 ? p.ix : p.ox;
            double& hy = m_pt_part == 1 ? p.iy : p.oy;
            double& kx = m_pt_part == 1 ? p.ox : p.ix;
            double& ky = m_pt_part == 1 ? p.oy : p.iy;
            hx += ddx;
            hy += ddy;
            if (m_mods_alt) p.smooth = false;   // Alt: the handles part ways
            else if (p.smooth) {   // the other handle stays in line, its own length
                const double other = std::hypot(kx - p.x, ky - p.y), len = std::hypot(hx - p.x, hy - p.y);
                if (len > 1e-9) { kx = p.x - (hx - p.x) / len * other; ky = p.y - (hy - p.y) / len * other; }
            }
        }
        const bool open = m_pts_open;
        core::doc::edit_frames(m_journal, m_doc, {m_direct}, "Edit path", [&](core::Frame& f) { core::doc::set_path(f, P, open); });
        queue_draw();
        return;
    }
    case Drag::PointMarquee: {   // s207: the shape's points inside it picked (with those before, Shift)
        m_marquee = Rect::from_corners(m_px, m_py, m_px + ox, m_py + oy);
        double odx = 0, ody = 0;
        if (!direct_shape(m_direct, odx, ody)) return;
        bool open = false;
        const auto P = core::doc::shape_points(*m_doc.as<core::Frame>(m_direct), open);
        m_pts_sel.clear();
        for (const auto& q : m_pts0) m_pts_sel.insert(static_cast<int>(q.x));
        for (int i = 0; i < static_cast<int>(P.size()); ++i) {
            double sx, sy;
            m_vp.to_screen(P[static_cast<std::size_t>(i)].x + odx, P[static_cast<std::size_t>(i)].y + ody, sx, sy);
            if (m_marquee.contains(sx, sy)) m_pts_sel.insert(i);
        }
        m_pt = m_pts_sel.size() == 1 ? *m_pts_sel.begin() : -1;
        queue_draw();
        return;
    }
    case Drag::PenHandle: {   // s199: the handles out of the point just put down (and in, mirrored)
        if (m_pen.empty() || std::hypot(ox, oy) < 3) return;
        auto& p = m_pen.back();
        p.ox = m_dx0 + ox / s;
        p.oy = m_dy0 + oy / s;
        if (m_mods_alt) {   // s208 (as Curvz): Alt -- the out handle alone, a sharp turn here
            p.smooth = false;
        } else {
            p.ix = 2 * p.x - p.ox;
            p.iy = 2 * p.y - p.oy;
            p.smooth = true;
        }
        queue_draw();
        return;
    }
    case Drag::PicScale: {   // s186: the picture scaled by a handle -- in proportion (Alt: about its centre)
        const Rect r = core::resize_rect(m_pic0, m_pic_handle, ox / s, oy / s, true, m_mods_alt, 2);
        core::doc::edit_frames(m_journal, m_doc, {m_pic_frame}, "Scale picture", [&](core::Frame& f) {
            f.fit = core::ImageFit::Custom;
            f.pic_w = r.w;
            f.pic_x = r.x;
            f.pic_y = r.y;
        });
        queue_draw();
        return;
    }
    case Drag::Picture: {   // s035: the picture follows inside its frame
        const Rect p0 = m_pic0;
        core::doc::edit_frames(m_journal, m_doc, {m_pic_frame}, "Move picture", [&](core::Frame& f) {
            f.fit = core::ImageFit::Custom;
            f.pic_w = p0.w;
            f.pic_x = p0.x + ox / s;
            f.pic_y = p0.y + oy / s;
        });
        queue_draw();
        return;
    }
    case Drag::Tab: {   // s034: the stop follows; well off the strip: it goes
        const auto t = tab_strip();
        m_tab_x = std::clamp(m_px + ox, t.x0, t.x0 + t.width * t.scale);
        m_tab_off = std::abs(oy) > 28;
        queue_draw();
        return;
    }
    case Drag::Gutter: {
        if (!m_begun) {
            if (std::abs(ox) < 2) return;
            core::doc::frames_begin(m_journal, m_doc, {m_sel.front()}, "Column widths");
            m_begun = true;
        }
        const auto k = static_cast<std::size_t>(m_gutter);
        const double total = m_col0[k] + m_col0[k + 1], least = std::min(18.0, total / 2);
        auto w = m_col0;
        w[k] = std::clamp(m_col0[k] + ox / s, least, total - least);
        w[k + 1] = total - w[k];
        for (auto& v : w) v = std::round(v * 2) / 2;   // half points: tidy weights
        core::doc::edit_frames(m_journal, m_doc, {m_sel.front()}, "Column widths",
                               [&](core::Frame& f) { f.widths = w; });
        queue_draw();
        return;
    }
    case Drag::Text: {   // s010: drag-select, across the story's frames
        double lx, ly;
        m_vp.to_doc(m_px + ox, m_py + oy, lx, ly);
        auto f = core::hit_frame(m_doc, m_placed, lx, ly, 3 / s);
        const auto* fr = m_doc.as<core::Frame>(f);
        if (!fr || fr->story != m_edit_story) f = m_edit_frame;
        set_caret(hit_story(f, lx, ly), true);
        return;
    }
    case Drag::Pan:
        m_vp.set_origin(m_ox0 + ox, m_oy0 + oy);
        queue_draw();
        return;
    case Drag::Marquee: {
        m_marquee = Rect::from_corners(m_px, m_py, m_px + ox, m_py + oy);
        double x0, y0, x1, y1;
        m_vp.to_doc(m_marquee.x, m_marquee.y, x0, y0);
        m_vp.to_doc(m_marquee.r(), m_marquee.b(), x1, y1);
        if (m_override_marquee) {   // s169: what it would override, outlined (draw_overlays)
            m_override_hits = core::master_items_in(m_doc, m_placed, Rect::from_corners(x0, y0, x1, y1));
            queue_draw();
            return;
        }
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
    case Drag::Draw: {
        double x1 = m_dx0 + ox / s, y1 = m_dy0 + oy / s;
        const bool line = m_tool == Tool::Shape && m_shape == ShapeTool::Line && m_load.empty();
        const bool poly = m_tool == Tool::Shape && m_shape == ShapeTool::Polygon && m_load.empty();   // s206
        if (m_mods_shift && poly) {   // Shift: its turn at 15-degree steps
            const double a = std::round(std::atan2(y1 - m_dy0, x1 - m_dx0) / (M_PI / 12)) * (M_PI / 12);
            const double len = std::hypot(x1 - m_dx0, y1 - m_dy0);
            x1 = m_dx0 + len * std::cos(a);
            y1 = m_dy0 + len * std::sin(a);
        } else if (m_mods_shift && line) {   // s198: Shift -- a line at 45-degree steps
            const double a = std::round(std::atan2(y1 - m_dy0, x1 - m_dx0) / (M_PI / 4)) * (M_PI / 4);
            const double len = std::hypot(x1 - m_dx0, y1 - m_dy0);
            x1 = m_dx0 + len * std::cos(a);
            y1 = m_dy0 + len * std::sin(a);
        } else if (m_mods_shift) {   // Shift: square
            const double side = std::max(std::abs(x1 - m_dx0), std::abs(y1 - m_dy0));
            x1 = m_dx0 + std::copysign(side, x1 - m_dx0);
            y1 = m_dy0 + std::copysign(side, y1 - m_dy0);
        }
        m_snap_x = m_snap_y = NAN;
        if (m_snap) {
            const double tol = 6 / s;
            const auto hx = core::snap_any({x1}, m_targets.xs, tol);
            if (hx.hit()) { x1 += hx.delta; m_snap_x = hx.line; }
            const auto hy = core::snap_any({y1}, m_targets.ys, tol);
            if (hy.hit()) { y1 += hy.delta; m_snap_y = hy.line; }
        }
        m_draw = Rect::from_corners(m_dx0, m_dy0, x1, y1);
        m_lx1 = x1;
        m_ly1 = y1;
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
        double at = m_guide0 + (g->vertical ? ox : oy) / s;
        m_snap_x = m_snap_y = NAN;
        if (m_snap) {   // s153: to the edges, margins, columns, other guides, frames
            const auto h = core::snap_any({m_guide_base + at}, g->vertical ? m_targets.xs : m_targets.ys, 6 / s);
            if (h.hit()) { at += h.delta; (g->vertical ? m_snap_x : m_snap_y) = h.line; }
        }
        core::doc::guide_set(m_journal, m_doc, m_guide, at);
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
    if (was == Drag::Marquee && m_override_marquee) {   // s169: one step, the copies picked
        m_override_marquee = false;
        const auto hits = std::move(m_override_hits);
        m_override_hits.clear();
        m_marquee = {};
        const auto made = core::doc::override_master_items(m_journal, m_doc, hits);
        if (!made.empty()) {
            relayout();
            pick(made);
            m_sig_edited.emit();
            if (auto lg = log::get(log::Area::View)) lg->info("override {} master item(s) by marquee", made.size());
        }
        queue_draw();
        return;
    }
    if (was == Drag::Text) { queue_draw(); return; }
    if (was == Drag::Picture || was == Drag::PicScale || was == Drag::PathPoint) {   // s035: the move (s186: or the scale; s200: a point), one step
        m_begun = false;
        core::doc::frames_end(m_journal);
        m_sig_edited.emit();
        m_pic_frame.clear();
        queue_draw();
        return;
    }
    if (was == Drag::Tab) {   // s034: one step -- added, moved or removed
        const auto t = tab_strip();
        if (t.ok) {
            const double pos = std::max(0.0, std::round((m_tab_x - t.x0) / t.scale));
            const int k = m_tab;
            const bool off = m_tab_off;
            const char* label = off ? "Remove tab" : k >= 0 ? "Move tab" : "Add tab";
            if (!(off && k < 0)) {   // s157: the paragraph's own stops (the selection's paragraphs)
                auto tabs = t.tabs;
                if (k >= 0 && k < static_cast<int>(tabs.size())) {
                    if (off) tabs.erase(tabs.begin() + k);
                    else tabs[static_cast<std::size_t>(k)].pos = pos;
                } else if (!off) {
                    tabs.push_back({pos, core::TabAlign::Left, ""});
                }
                const auto [a, b] = edit_range();
                core::doc::set_para_tabs(m_journal, m_doc, m_edit_story, a, b, label, tabs);
            }
            m_sig_edited.emit();
        }
        m_tab = -1;
        m_tab_off = false;
        queue_draw();
        return;
    }
    if (was == Drag::Draw && !m_load.empty()) {   // s009: a click places, a drag draws its frame
        const bool drawn = m_draw.w * m_vp.scale() >= 4 && m_draw.h * m_vp.scale() >= 4;
        place_loaded(m_dx0, m_dy0, drawn ? &m_draw : nullptr);
        m_draw = {};
        m_snap_x = m_snap_y = NAN;
        queue_draw();
        return;
    }
    if (was == Drag::Draw) {
        // Big enough to mean it (4 px on screen): a frame on the page it was
        // started on, in that page's coordinates.
        const bool line = m_tool == Tool::Shape && m_shape == ShapeTool::Line;   // s198
        const bool drawn = line ? std::hypot(m_lx1 - m_dx0, m_ly1 - m_dy0) * m_vp.scale() >= 4
                                : m_draw.w * m_vp.scale() >= 4 && m_draw.h * m_vp.scale() >= 4;
        if (m_tool == Tool::Shape && m_shape == ShapeTool::Polygon) {   // s206: from its centre
            const double rr = std::hypot(m_lx1 - m_dx0, m_ly1 - m_dy0);
            if (rr * m_vp.scale() >= 4)
                for (const auto& p : m_placed)
                    if (p.id == m_draw_page && p.r.contains(m_dx0, m_dy0)) {
                        double inner = 1;
                        (void)drawn_polygon(inner);
                        const auto id = core::doc::add_polygon(m_journal, m_doc, p.id, m_dx0 - p.r.x, m_dy0 - p.r.y, rr, m_poly_sides, inner,
                                                               std::atan2(m_ly1 - m_dy0, m_lx1 - m_dx0));
                        if (!id.empty()) { relayout(); pick({id}); m_sig_edited.emit(); }
                        break;
                    }
            m_draw = {};
            m_snap_x = m_snap_y = NAN;
            queue_draw();
            return;
        }
        if (drawn && line) {
            for (const auto& p : m_placed)
                if (p.id == m_draw_page && p.r.contains(m_dx0, m_dy0)) {
                    const auto id = core::doc::add_path(m_journal, m_doc, p.id,
                                                        {core::doc::corner_point(m_dx0 - p.r.x, m_dy0 - p.r.y),
                                                         core::doc::corner_point(m_lx1 - p.r.x, m_ly1 - p.r.y)}, true);
                    if (!id.empty()) { relayout(); pick({id}); m_sig_edited.emit(); }
                    break;
                }
            m_draw = {};
            m_snap_x = m_snap_y = NAN;
            queue_draw();
            return;
        }
        // s014: a Text-tool CLICK inside the margins makes a frame filling the
        // page's column there.
        if (!drawn && m_tool == Tool::Text) {
            for (const auto& p : m_placed) {
                if (p.id != m_draw_page) continue;
                const Rect mr = core::margin_rect(m_doc, p.id, p.side);
                const double lx = m_dx0 - p.r.x, ly = m_dy0 - p.r.y;
                if (mr.w <= 0 || ly < mr.y || ly > mr.b()) break;
                for (const auto& c : core::page_columns(m_doc, p.id, p.side))
                    if (lx >= c.x0 && lx <= c.x1) {
                        const auto id = core::doc::add_frame(m_journal, m_doc, p.id, core::FrameKind::Text,
                                                             {c.x0, mr.y, c.x1 - c.x0, mr.h});
                        if (!id.empty()) {
                            relayout();
                            pick({id});
                            m_sig_edited.emit();
                            begin_edit(id, {0, 0});
                        }
                    }
            }
        }
        if (drawn) {
            for (const auto& p : m_placed)
                if (p.id == m_draw_page) {
                    const core::FrameKind kind = m_tool == Tool::Text    ? core::FrameKind::Text
                                               : m_tool == Tool::Image   ? core::FrameKind::Image
                                                                         : core::FrameKind::Shape;
                    // s155: on a facing master's spread, drawn across the spine: one
                    // item across it, placed from the left page.
                    const core::Placed* left = nullptr;
                    bool across = false;
                    if (p.master && p.side != core::Side::Any)
                        for (const auto& q : m_placed)
                            if (q.id == p.id && q.side == core::Side::Left) left = &q;
                    if (left) across = m_draw.x < left->r.r() - 0.5 && m_draw.r() > left->r.r() + 0.5;
                    const core::Placed& at = across ? *left : p;
                    // s014: a text frame drawn across the margins takes the page's columns.
                    const Rect own{m_draw.x - at.r.x, m_draw.y - at.r.y, m_draw.w, m_draw.h};
                    const Rect mr = core::margin_rect(m_doc, at.id, at.side);
                    auto [cols, gut] = core::page_column_setup(m_doc, at.id);
                    if (across || !(mr.w > 0 && std::abs(own.x - mr.x) < 1 && std::abs(own.r() - mr.r()) < 1)) cols = 1;
                    const auto id = core::doc::add_frame(m_journal, m_doc, at.id, kind, own, cols, gut, across,
                                                         m_shape == ShapeTool::Oval ? core::ShapeKind::Oval : core::ShapeKind::Rect);
                    if (!id.empty()) {
                        relayout();
                        pick({id});
                        m_sig_edited.emit();
                        // s010: a new text frame is ready to type in (as InDesign).
                        if (m_tool == Tool::Text) begin_edit(id, {0, 0});
                    }
                    break;   // s155: one frame (a master's spread is two panels of one id)
                }
        }
        m_draw = {};
        m_snap_x = m_snap_y = NAN;
        queue_draw();
        return;
    }
    if (was == Drag::Guide && m_begun && m_guide_off)
        core::doc::guide_remove_in_gesture(m_journal, m_doc, m_guide);
    if ((was == Drag::Move || was == Drag::Resize || was == Drag::Guide || was == Drag::Gutter) && m_begun) {
        m_begun = false;
        // s027: dragged across the spine (onto another shown page): that
        // page's now -- part of the drag's one step.
        if (was == Drag::Move && core::doc::frames_rehome(m_journal, m_doc, m_placed, m_sel) > 0)
            if (auto lg = log::get(log::Area::View)) lg->info("move: to another page");
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
    if ((m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::Guide || m_drag == Drag::Gutter ||
         m_drag == Drag::Picture || m_drag == Drag::PicScale || m_drag == Drag::PathPoint) && m_begun) {   // s200: a picture's / a point's drag too
        core::doc::frames_cancel(m_journal);
        m_sig_edited.emit();
    }
    m_guide.clear();
    m_guide_off = false;
    m_snap_x = m_snap_y = NAN;
    if (m_drag == Drag::Marquee) pick(m_sel0);
    if (m_drag == Drag::PenHandle) pen_finish(false);   // s199: Esc mid-drag -- the path dropped
    m_drag = Drag::None;
    m_begun = false;
    m_pic_frame.clear();
    queue_draw();
    update_cursor();
}

bool Canvas::on_key(guint kv, Gdk::ModifierType st) {
    if (m_take && kv == GDK_KEY_Escape) { cancel_take(); return true; }   // s180
    if (editing()) return edit_key(kv, st);
    const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
    const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
    if (kv == GDK_KEY_space && !ctrl) {
        if (!m_space) { m_space = true; update_cursor(); }
        return true;
    }
    // s063: loaded with several -- the arrows turn through them.
    if (m_load.size() > 1 && !editing() &&
        (kv == GDK_KEY_Left || kv == GDK_KEY_Right || kv == GDK_KEY_Up || kv == GDK_KEY_Down)) {
        m_load.cycle(kv == GDK_KEY_Right || kv == GDK_KEY_Down ? +1 : -1);
        queue_draw();
        return true;
    }
    if (kv == GDK_KEY_Escape && m_tool == Tool::Pen && !m_pen.empty()) { pen_finish(true); return true; }   // s199
    if (kv == GDK_KEY_Escape) {
        if (!m_thread_from.empty()) { m_thread_from.clear(); update_cursor(); queue_draw(); return true; }
        if (m_drag != Drag::None) { cancel_drag(); return true; }
        if (!m_load.empty()) { m_load.clear(); update_cursor(); queue_draw(); return true; }
        if (!m_sel.empty()) { pick({}); return true; }
        return false;
    }
    const bool alt = (st & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK;
    // s200: Direct -- Delete takes out the shape's picked point.
    if (m_tool == Tool::Direct && (m_pt >= 0 || !m_pts_sel.empty()) && !m_direct.empty() &&
        (kv == GDK_KEY_Delete || kv == GDK_KEY_BackSpace)) {   // s207: every picked point
        auto ks = m_pts_sel;
        if (m_pt >= 0) ks.insert(m_pt);
        if (core::doc::path_delete_points(m_journal, m_doc, m_direct, ks)) m_sig_edited.emit();
        m_pt = -1;
    m_pts_sel.clear();
        m_pts_sel.clear();
        queue_draw();
        return true;
    }
    // s207: Direct -- the arrows nudge the picked points (Shift: 10 pt), one step while they're pressed.
    if (m_tool == Tool::Direct && !m_pts_sel.empty() && !m_direct.empty() &&
        (kv == GDK_KEY_Left || kv == GDK_KEY_Right || kv == GDK_KEY_Up || kv == GDK_KEY_Down)) {
        const double step = shift ? 10 : 1;
        const double ddx = kv == GDK_KEY_Left ? -step : kv == GDK_KEY_Right ? step : 0;
        const double ddy = kv == GDK_KEY_Up ? -step : kv == GDK_KEY_Down ? step : 0;
        if (core::doc::path_move_points(m_journal, m_doc, m_direct, m_pts_sel, ddx, ddy, "nudge-points:" + m_direct)) m_sig_edited.emit();
        queue_draw();
        return true;
    }
    // s199: the Pen's path -- Enter ends it (open), Backspace takes back its last point.
    if (m_tool == Tool::Pen && !m_pen.empty()) {
        if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) { pen_finish(true); return true; }
        if (kv == GDK_KEY_BackSpace || kv == GDK_KEY_Delete) {
            m_pen.pop_back();
            queue_draw();
            return true;
        }
    }
    if (!ctrl && !alt && m_drag == Drag::None) {   // s008: tools by letter (as InDesign), Delete
        switch (kv) {
        case GDK_KEY_v: case GDK_KEY_V: set_tool(Tool::Select); m_sig_tool.emit(); return true;
        case GDK_KEY_a: case GDK_KEY_A: set_tool(Tool::Direct); m_sig_tool.emit(); return true;   // s186
        case GDK_KEY_t: case GDK_KEY_T: set_tool(Tool::Text);   m_sig_tool.emit(); return true;
        case GDK_KEY_f: case GDK_KEY_F: set_tool(Tool::Image);  m_sig_tool.emit(); return true;
        case GDK_KEY_m: case GDK_KEY_M: set_shape_tool(ShapeTool::Rect); m_sig_tool.emit(); return true;
        case GDK_KEY_l: case GDK_KEY_L: set_shape_tool(ShapeTool::Oval); m_sig_tool.emit(); return true;   // s198
        case GDK_KEY_backslash:         set_shape_tool(ShapeTool::Line); m_sig_tool.emit(); return true;
        case GDK_KEY_p: case GDK_KEY_P: set_tool(Tool::Pen);        m_sig_tool.emit(); return true;   // s199
        case GDK_KEY_i: case GDK_KEY_I: set_tool(Tool::Eyedropper); m_sig_tool.emit(); return true;   // s198
        case GDK_KEY_Delete: case GDK_KEY_BackSpace:
            if (!m_sel.empty()) { delete_selection(); return true; }
            return false;
        default: break;
        }
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

std::string Canvas::ref_tip(double x, double y) const {   // s130
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    const auto frame = core::hit_frame(m_doc, m_placed, dx, dy, 0);
    const auto* f = frame.empty() ? nullptr : m_doc.as<core::Frame>(frame);
    const auto* st = f && !f->story.empty() ? m_doc.as<core::Story>(f->story) : nullptr;
    if (!st) return {};
    if (std::none_of(st->paras.begin(), st->paras.end(), [](const core::Para& p) { return !p.refs.empty() || !p.index.empty(); })) return {};
    const Rect lr = core::frame_in_layout(m_doc, m_placed, frame);
    const auto pos = layout::hit_text(m_doc, f->story, flows().get(m_doc, f->story), frame, dx - lr.x, dy - lr.y);
    if (const auto ix = core::doc::index_near(*st, pos); ix.k >= 0) {   // s138: an index mark
        const auto& m = st->paras[static_cast<std::size_t>(ix.para)].index[static_cast<std::size_t>(ix.k)];
        std::string t = "Index: " + m.term + (m.sub.empty() ? "" : " \u203a " + m.sub);
        if (!m.see.empty()) t += (m.also ? ". See also " : ". See ") + m.see;
        switch (m.span) {   // s139
        case core::IndexSpan::Point: break;
        case core::IndexSpan::Paragraph: t += " (its paragraph)"; break;
        case core::IndexSpan::NextHeading: t += " (to the next heading)"; break;
        case core::IndexSpan::Paragraphs: t += " (" + std::to_string(m.count) + " paragraphs)"; break;
        case core::IndexSpan::StoryEnd: t += " (to the story's end)"; break;
        }
        return t;
    }
    const auto r = core::doc::ref_near(*st, pos);
    if (r.k < 0) return {};
    return layout::ref_where(m_doc, st->paras[static_cast<std::size_t>(r.para)].refs[static_cast<std::size_t>(r.k)], flows(), f->story);
}

void Canvas::on_motion(double x, double y) {
    m_mx = x;
    m_my = y;
    m_in = true;
    m_sig_view.emit();
    if (!m_load.empty()) queue_draw();   // the file's name rides with the pointer
    if (m_take || m_tool == Tool::Eyedropper) {   // s180: the colour under the pointer rides with it
        m_take_under = colour_at(x, y);
        queue_draw();
        return;
    }
    if (m_tool == Tool::Pen && !m_pen.empty()) queue_draw();   // s199: the next segment follows
    if (m_drag != Drag::None) return;
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    const auto hg = m_show_guides && !m_guides_locked ? core::hit_guide(m_doc, m_placed, dx, dy, 4 / m_vp.scale()) : std::string{};
    if (hg != m_hover_guide) { m_hover_guide = hg; queue_draw(); }
    const auto h = core::hit_frame(m_doc, m_placed, dx, dy, 3 / m_vp.scale());
    if (h != m_hover) { m_hover = h; queue_draw(); }
    m_hover_gutter = gutter_hit(x, y);
    update_cursor();
}

void Canvas::set_tool(Tool t) {
    if (t != Tool::Pen) pen_finish(true);   // s199: another tool -- the path so far is drawn
    cancel_take();   // s180
    end_edit();
    if (t != Tool::Direct) m_direct.clear();   // s186
    else if (m_sel.size() == 1) {   // s186: a picked picture shows at once (s200: or a shape's points)
        Rect fl, pl;
        double ox = 0, oy = 0;
        if (direct_pic(m_sel.front(), fl, pl) || direct_shape(m_sel.front(), ox, oy)) m_direct = m_sel.front();
    }
    m_pt = -1;
    queue_draw();
    m_tool = t;
    m_take_under.reset();
    m_thread_from.clear();
    m_load.clear();
    update_cursor();
}

std::vector<core::PathPoint> Canvas::drawn_polygon(double& inner) const {   // s206
    inner = m_poly_inner;
    if (const double perfect = core::doc::perfect_star(m_poly_sides); perfect > 0 && std::abs(inner - perfect) < 0.04) inner = perfect;
    if (inner > 0.985) inner = 1;
    const double rr = std::hypot(m_lx1 - m_dx0, m_ly1 - m_dy0);
    if (rr <= 0) return {};
    return core::doc::polygon_points(m_dx0, m_dy0, rr, m_poly_sides, inner, std::atan2(m_ly1 - m_dy0, m_lx1 - m_dx0));
}

void Canvas::open_story_editor() {   // s217: the window's Story Editor action, for text on a path
    auto* root = dynamic_cast<Gtk::Widget*>(get_root());
    const bool ok = root && root->activate_action("win.story-editor");
    if (auto lg = log::get(log::Area::View)) lg->info("story editor for text on a path: {}", ok ? "opened" : "no action");
}

bool Canvas::pen_near_first(double sx, double sy) const {   // s199
    if (m_pen.empty()) return false;
    double fx, fy;
    m_vp.to_screen(m_pen.front().x, m_pen.front().y, fx, fy);
    return std::hypot(sx - fx, sy - fy) <= 6;
}

void Canvas::pen_finish(bool keep, bool closed) {   // s199
    if (m_pen.empty()) return;
    auto pts = std::move(m_pen);
    m_pen.clear();
    if (m_drag == Drag::PenHandle) m_drag = Drag::None;
    queue_draw();
    const auto cont = std::move(m_pen_cont);
    m_pen_cont.clear();
    if (!keep || pts.size() < 2) return;
    for (auto& p : pts) {   // into the page's own coordinates
        p.x -= m_pen_ox; p.ix -= m_pen_ox; p.ox -= m_pen_ox;
        p.y -= m_pen_oy; p.iy -= m_pen_oy; p.oy -= m_pen_oy;
    }
    if (!cont.empty()) {   // s208: the path carried on (or closed) -- one step
        if (m_pen_rev) pts = core::doc::reversed_path(pts);
        if (core::doc::set_path_points(m_journal, m_doc, cont, pts, !closed, closed ? "Close path" : "Extend path")) {
            relayout();
            pick({cont});
            m_sig_edited.emit();
        }
        return;
    }
    const auto id = core::doc::add_path(m_journal, m_doc, m_pen_owner, pts, !closed);
    if (id.empty()) return;
    relayout();
    pick({id});
    m_sig_edited.emit();
}

void Canvas::delete_selection() {
    if (m_sel.empty()) return;
    const auto gone = m_sel;
    if (core::doc::delete_frames(m_journal, m_doc, gone)) {
        pick({});
        m_sig_edited.emit();
    }
}

// ── s012: gutters ───────────────────────────────────────────────────────────
int Canvas::gutter_hit(double sx, double sy) const {
    if (m_sel.size() != 1 || editing() || m_tool != Tool::Select) return -1;
    const auto* f = m_doc.as<core::Frame>(m_sel.front());
    if (!f || f->kind != core::FrameKind::Text || core::column_count(*f) < 2) return -1;
    const Rect lr = core::frame_in_layout(m_doc, m_placed, m_sel.front());
    if (lr.w <= 0) return -1;
    const Rect r = m_vp.to_screen(lr);
    if (sy < r.y || sy > r.b()) return -1;
    const double s = m_vp.scale();
    for (int k = 0; k + 1 < core::column_count(*f); ++k) {
        const double mid = (core::column_x(*f, k) + core::column_width(*f, k) + core::column_x(*f, k + 1)) / 2;
        const double half = std::max(4.0, (core::column_x(*f, k + 1) - core::column_x(*f, k) - core::column_width(*f, k)) * s / 2);
        if (std::abs(sx - (r.x + mid * s)) <= half) return k;
    }
    return -1;
}

// ── s010: editing text ──────────────────────────────────────────────────────
void Canvas::begin_edit(const std::string& frame, core::TextPos at) {
    const auto* f = m_doc.as<core::Frame>(frame);
    if (!f || f->kind != core::FrameKind::Text || f->story.empty()) return;
    m_edit_story = f->story;
    m_edit_frame = frame;
    m_caret = m_anchor = core::clamp_pos(*edit_story(), at);
    m_goal_x = NAN;
    m_burst_kind.clear();
    ++m_burst;
    gtk_im_context_set_client_widget(m_im, GTK_WIDGET(gobj()));
    gtk_event_controller_key_set_im_context(m_keys->gobj(), m_im);
    gtk_im_context_focus_in(m_im);
    grab_focus();
    pick({frame});
    blink_restart();
    update_cursor();
    m_sig_caret.emit();
    if (auto lg = log::get(log::Area::View)) lg->info("edit: {} in {}", m_edit_story, frame);
}

void Canvas::select_text(const std::string& frame, core::TextPos a, core::TextPos b, bool focus) {
    Gtk::Widget* had = nullptr;
    if (!focus) if (auto* root = get_root()) had = root->get_focus();
    begin_edit(frame, a);
    if (!editing()) return;
    m_anchor = core::clamp_pos(*edit_story(), a);
    m_caret = core::clamp_pos(*edit_story(), b);
    if (had) had->grab_focus();
    queue_draw();
    m_sig_caret.emit();
}

bool Canvas::insert_special(const std::string& text, const std::string& label) {
    if (!editing()) return false;
    edit_insert(text, "", label);
    grab_focus();
    return true;
}

// s055: a column / frame / page break at the caret (what's selected goes first).
bool Canvas::insert_break(core::ParaStart kind) {
    if (!editing() || !edit_story()) return false;
    if (m_anchor != m_caret) edit_insert("", "", "Delete text");
    m_caret = core::doc::insert_break(m_journal, m_doc, m_edit_story, m_caret, kind);
    m_anchor = m_caret;
    m_goal_x = NAN;
    m_burst_kind.clear();
    m_sig_edited.emit();
    follow_caret();   // s061
    blink_restart();
    m_sig_caret.emit();
    grab_focus();
    return true;
}

bool Canvas::insert_note(int& p, int& k) {   // s109
    if (!editing() || !edit_story()) return false;
    // s113: words selected -- the mark goes after them (as Word), not over them.
    const core::TextPos at = std::max(m_anchor, m_caret);
    m_caret = core::doc::insert_note(m_journal, m_doc, m_edit_story, at, "");
    m_anchor = m_caret;
    m_goal_x = NAN;
    m_burst_kind.clear();
    p = m_caret.para;
    k = m_caret.byte >= 3 ? core::doc::note_index_at(*edit_story(), {m_caret.para, m_caret.byte - 3}) : -1;
    m_sig_edited.emit();
    follow_caret();
    blink_restart();
    m_sig_caret.emit();
    return k >= 0;
}

bool Canvas::note_at_caret(int& p, int& k) const {   // s109
    if (!editing() || !edit_story()) return false;
    const auto& s = *edit_story();
    for (const int b : {m_caret.byte - 3, m_caret.byte}) {
        const int i = core::doc::note_index_at(s, {m_caret.para, b});
        if (i >= 0) { p = m_caret.para; k = i; return true; }
    }
    return false;
}

Rect Canvas::caret_screen() const {   // s109
    if (!editing()) return {};
    const auto& flow = flows().get(m_doc, m_edit_story);
    const auto c = layout::caret_at(m_doc, m_edit_story, flow, m_caret);
    if (!c.ok) return {};
    const Rect fr = core::frame_in_layout(m_doc, m_placed, c.frame);
    if (fr.w <= 0) return {};
    return m_vp.to_screen(Rect{fr.x + c.x, fr.y + c.y, 1, c.h});
}

void Canvas::end_edit() {
    if (!editing()) return;
    gtk_im_context_focus_out(m_im);
    gtk_im_context_reset(m_im);
    gtk_event_controller_key_set_im_context(m_keys->gobj(), nullptr);
    gtk_im_context_set_client_widget(m_im, nullptr);
    m_blink.disconnect();
    m_edit_story.clear();
    m_drag = m_drag == Drag::Text ? Drag::None : m_drag;
    queue_draw();
    update_cursor();
    m_sig_caret.emit();
}

void Canvas::blink_restart() {
    m_caret_on = true;
    m_blink.disconnect();
    m_blink = Glib::signal_timeout().connect([this] {
        m_caret_on = !m_caret_on;
        queue_draw();
        return editing();
    }, 530);
    queue_draw();
}

// s114: (px, py) on a note set at the column's foot -- the caret after its
// mark, and the Shell opens its words. False: not on a note.
bool Canvas::note_click(const std::string& frame, double px, double py) {
    if (!editing() || !edit_story()) return false;
    const Rect lr = core::frame_in_layout(m_doc, m_placed, frame);
    const auto* np = layout::note_hit(flows().get(m_doc, m_edit_story), frame, px - lr.x, py - lr.y);
    if (!np) return false;
    const int m = core::doc::note_mark_byte(*edit_story(), np->para, np->k);
    if (m < 0) return false;
    m_edit_frame = frame;
    set_caret({np->para, m + 3}, false);
    m_drag = Drag::None;
    const int p = np->para, k = np->k;
    m_sig_note.emit(p, k);
    return true;
}

core::TextPos Canvas::hit_story(const std::string& frame, double lx, double ly) {
    const Rect lr = core::frame_in_layout(m_doc, m_placed, frame);
    const auto& flow = flows().get(m_doc, m_edit_story.empty() ? m_doc.as<core::Frame>(frame)->story : m_edit_story);
    const std::string story = m_edit_story.empty() ? m_doc.as<core::Frame>(frame)->story : m_edit_story;
    return layout::hit_text(m_doc, story, flow, frame, lx - lr.x, ly - lr.y);
}

void Canvas::set_caret(core::TextPos p, bool extend, bool keep_goal) {
    const auto* st = edit_story();
    if (!st) return;
    m_caret = core::clamp_pos(*st, p);
    if (!extend) m_anchor = m_caret;
    if (!keep_goal) m_goal_x = NAN;
    m_burst_kind.clear();   // a move ends the typing burst: the next keys are a new step
    follow_caret();         // s061
    blink_restart();
    m_sig_caret.emit();
}

void Canvas::place_caret(core::TextPos p) {
    if (!editing()) return;
    set_caret(p, false);
}

// s061: keep the caret in view -- onto another spread when the text has
// taken it there, then panned so it sits inside the canvas.
void Canvas::follow_caret() {
    if (!editing()) return;
    const auto& flow = flows().get(m_doc, m_edit_story);
    const auto c = layout::caret_at(m_doc, m_edit_story, flow, m_caret);
    if (!c.ok) return;   // overset: nowhere to show it
    m_edit_frame = c.frame;
    Rect fr = core::frame_in_layout(m_doc, m_placed, c.frame);
    if (fr.w <= 0) {
        const auto* f = m_doc.as<core::Frame>(c.frame);
        if (!f || !m_doc.as<core::Page>(f->owner)) return;
        m_follow_keep_zoom = true;
        m_sig_follow.emit(f->owner);
        m_follow_keep_zoom = false;
        fr = core::frame_in_layout(m_doc, m_placed, c.frame);
        if (fr.w <= 0) return;
        if (auto lg = log::get(log::Area::View)) lg->info("caret: followed onto {}", f->owner);
    }
    if (m_vp.reveal({fr.x + c.x - 2, fr.y + c.y, 4, c.h}, 48)) queue_draw();
}

void Canvas::edit_insert(const std::string& text, const std::string& kind, const std::string& label,
                         const core::doc::Carried* with) {
    if (!edit_story()) return;
    if (kind.empty() || kind != m_burst_kind) { ++m_burst; m_burst_kind = kind; }
    const std::string key = kind.empty() ? std::string{} : kind + ":" + m_edit_story + ":" + std::to_string(m_burst);
    m_caret = core::doc::edit_text(m_journal, m_doc, m_edit_story, m_anchor, m_caret, text, label, key, with);
    m_anchor = m_caret;
    m_goal_x = NAN;
    if (kind.empty()) m_burst_kind.clear();
    m_sig_edited.emit();
    follow_caret();   // s061: typing onto the next page takes the view along
    blink_restart();
    m_sig_caret.emit();
}

bool Canvas::edit_key(guint kv, Gdk::ModifierType st) {
    const auto* story = edit_story();
    if (!story) { end_edit(); return false; }
    const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
    const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
    const bool sel = m_anchor != m_caret;
    const auto lo = std::min(m_anchor, m_caret), hi = std::max(m_anchor, m_caret);
    const auto& flow = flows().get(m_doc, m_edit_story);
    switch (kv) {
    case GDK_KEY_Escape: {
        const auto f = m_edit_frame;
        end_edit();
        pick({f});
        return true;
    }
    case GDK_KEY_BackSpace:
        if (!sel) m_anchor = ctrl ? core::prev_word(*story, m_caret) : core::prev_char(*story, m_caret);
        if (m_anchor != m_caret) edit_insert("", "erase", "Delete text");
        return true;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
        if (!sel) m_caret = ctrl ? core::next_word(*story, m_caret) : core::next_char(*story, m_caret);
        if (m_anchor != m_caret) edit_insert("", "erase", "Delete text");
        return true;
    case GDK_KEY_underscore: case GDK_KEY_minus:   // s065: Ctrl+Shift+- -- a discretionary hyphen
        if (!ctrl || (kv == GDK_KEY_minus && !shift)) return false;
        edit_insert("\u00AD", "", "Discretionary hyphen");
        return true;
    case GDK_KEY_KP_Enter:   // s055: keypad Enter breaks -- column; Shift: frame; Ctrl: page
        insert_break(ctrl ? core::ParaStart::NextPage : shift ? core::ParaStart::NextFrame : core::ParaStart::NextColumn);
        return true;
    case GDK_KEY_Return:
        edit_insert("\n", "type", "Typing");
        return true;
    case GDK_KEY_Tab:
        edit_insert("\t", "type", "Typing");
        return true;
    case GDK_KEY_Left: case GDK_KEY_KP_Left:
        if (sel && !shift) set_caret(lo, false);
        else set_caret(ctrl ? core::prev_word(*story, m_caret) : core::prev_char(*story, m_caret), shift);
        return true;
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
        if (sel && !shift) set_caret(hi, false);
        else set_caret(ctrl ? core::next_word(*story, m_caret) : core::next_char(*story, m_caret), shift);
        return true;
    case GDK_KEY_Up: case GDK_KEY_Down: case GDK_KEY_KP_Up: case GDK_KEY_KP_Down: {
        const int dir = (kv == GDK_KEY_Up || kv == GDK_KEY_KP_Up) ? -1 : 1;
        if (std::isnan(m_goal_x)) m_goal_x = layout::caret_at(m_doc, m_edit_story, flow, m_caret).x;
        set_caret(layout::line_move(m_doc, m_edit_story, flow, m_caret, dir, m_goal_x), shift, true);
        return true;
    }
    case GDK_KEY_Home: case GDK_KEY_KP_Home:
        set_caret(ctrl ? core::TextPos{} : layout::line_edge(m_doc, m_edit_story, flow, m_caret, false), shift);
        return true;
    case GDK_KEY_End: case GDK_KEY_KP_End:
        set_caret(ctrl ? core::story_end(*story) : layout::line_edge(m_doc, m_edit_story, flow, m_caret, true), shift);
        return true;
    default: break;
    }
    if (ctrl) {
        switch (kv) {
        case GDK_KEY_a: case GDK_KEY_A:
            m_anchor = {};
            set_caret(core::story_end(*story), true);
            return true;
        case GDK_KEY_c: case GDK_KEY_C: case GDK_KEY_x: case GDK_KEY_X:
            if (!sel) return true;
            m_clip_text = core::text_between(*story, lo, hi);
            m_clip = core::doc::carried_between(*story, lo, hi);   // s132
            get_clipboard()->set_text(m_clip_text);
            if (kv == GDK_KEY_x || kv == GDK_KEY_X) edit_insert("", "", "Cut");
            return true;
        case GDK_KEY_v: case GDK_KEY_V:
            get_clipboard()->read_text_async([this](const Glib::RefPtr<Gio::AsyncResult>& r) {
                try {
                    const auto t = get_clipboard()->read_text_finish(r);
                    // s132: our own copy: its notes, references and anchors with it.
                    if (editing() && !t.empty()) edit_insert(t.raw(), "", "Paste", t.raw() == m_clip_text ? &m_clip : nullptr);
                } catch (const Glib::Error&) {}
            });
            return true;
        default:
            return false;   // Ctrl+Z, Ctrl+S ... belong to the window
        }
    }
    return true;   // anything else typed is the input method's (or nothing)
}

// ── s034: the tab ruler ─────────────────────────────────────────────────────
Canvas::TabStrip Canvas::tab_strip() const {
    TabStrip t;
    if (!editing()) return t;
    const auto* st = m_doc.as<core::Story>(m_edit_story);
    if (!st || st->paras.empty()) return t;
    const auto& para = st->paras[static_cast<std::size_t>(std::min<int>(m_caret.para, static_cast<int>(st->paras.size()) - 1))];
    if (!m_doc.as<core::Style>(para.style)) return t;
    const core::Style eff = core::doc::para_style(m_doc, para);   // s094: the paragraph's own indents count
    const auto* ps = &eff;
    const auto& flow = flows().get(m_doc, m_edit_story);
    const auto c = layout::caret_at(m_doc, m_edit_story, flow, m_caret);
    const auto* f = m_doc.as<core::Frame>(c.ok ? c.frame : m_edit_frame);
    if (!f) return t;
    const std::string fid = c.ok ? c.frame : m_edit_frame;
    const int col = c.ok ? core::column_at(*f, c.x) : 0;
    const Rect fr = core::frame_in_layout(m_doc, m_placed, fid);
    if (fr.w <= 0) return t;
    const double left = fr.x + core::column_x(*f, col) + std::max(0.0, ps->left_indent);
    const double width = core::column_width(*f, col) - std::max(0.0, ps->left_indent) - std::max(0.0, ps->right_indent);
    const Rect sr = m_vp.to_screen(Rect{left, fr.y, std::max(1.0, width), 1});
    t.ok = true;
    t.scale = m_vp.scale();
    t.x0 = sr.x;
    t.width = width;
    t.screen = {sr.x - 6, sr.y - 34, sr.w + 12, 26};   // s160 fix: room for the numbers
    t.style = para.style;
    t.tabs = eff.tabs;                  // s157: the paragraph's own, else its style's
    t.own = para.over.tabs.has_value();
    if (t.own) t.reset = {t.screen.r() + 4, t.screen.y, 92, t.screen.h};
    return t;
}

int Canvas::tab_hit(const TabStrip& t, double x) const {
    for (int i = 0; i < static_cast<int>(t.tabs.size()); ++i)
        if (std::abs(t.x0 + t.tabs[static_cast<std::size_t>(i)].pos * t.scale - x) <= 6) return i;
    return -1;
}

void Canvas::draw_tab_strip(const Cairo::RefPtr<Cairo::Context>& cr) {
    const auto t = tab_strip();
    if (!t.ok) return;
    const auto& r = t.screen;
    cr->save();
    // The strip: a card, ticks every 6 pt (longer each half inch), inches marked.
    // s157: tinted when the stops are the paragraph's own; "Reset to style" beside it.
    if (t.own) cr->set_source_rgba(1.0, 0.97, 0.90, 0.97);
    else       cr->set_source_rgba(1, 1, 1, 0.96);
    cr->rectangle(r.x, r.y, r.w, r.h);
    cr->fill();
    if (t.own) {
        const auto& b = t.reset;
        cr->set_source_rgba(1, 1, 1, 0.96);
        cr->rectangle(b.x, b.y, b.w, b.h);
        cr->fill_preserve();
        cr->set_source_rgba(0.85, 0.5, 0.1, 0.9);
        cr->set_line_width(1);
        cr->stroke();
        auto lb = create_pango_layout("\u21BA Reset to style");
        lb->set_font_description(Pango::FontDescription("Sans 7.5"));
        int lw = 0, lh = 0;
        lb->get_pixel_size(lw, lh);
        cr->move_to(b.x + (b.w - lw) / 2, b.y + (b.h - lh) / 2);
        lb->show_in_cairo_context(cr);
        auto tag = create_pango_layout("this paragraph");
        tag->set_font_description(Pango::FontDescription("Sans Italic 7"));
        int tw = 0, th = 0;
        tag->get_pixel_size(tw, th);
        cr->set_source_rgba(0.6, 0.35, 0.05, 0.9);
        cr->move_to(r.r() - tw - 4, r.y + 1);
        tag->show_in_cairo_context(cr);
    }
    cr->set_source_rgba(0.3, 0.4, 0.55, 0.95);
    cr->set_line_width(1);
    cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w) - 1, std::round(r.h) - 1);
    cr->stroke();
    // s160 fix (Scott: too faint, no measures): ticks in the document's unit,
    // as the rulers -- majors numbered, halves and parts shorter -- drawn dark.
    {
        const auto units = m_doc.settings().units;
        const double upt = core::unit_pt(units);
        std::vector<double> steps;
        std::vector<int> parts;
        switch (units) {
        case core::Units::Millimetres: steps = {5, 10, 20, 50, 100}; parts = {10, 5, 2, 1}; break;
        case core::Units::Picas:       steps = {1, 2, 3, 6, 12};      parts = {12, 6, 3, 1}; break;
        case core::Units::Points:      steps = {12, 18, 36, 72, 144}; parts = {12, 6, 2, 1}; break;
        default:                       steps = {0.25, 0.5, 1, 2};     parts = {8, 4, 2, 1}; break;
        }
        double major = steps.back();
        for (double st : steps) if (st * upt * t.scale >= 44) { major = st; break; }
        const double px_major = major * upt * t.scale;
        int subs = 1;
        for (int q : parts) if (px_major / q >= 5) { subs = q; break; }
        cr->set_source_rgba(0.12, 0.15, 0.22, 0.9);
        cr->set_line_width(1);
        const double right = t.x0 + t.width * t.scale + 0.5;
        for (int k = 0; t.x0 + k * px_major <= right; ++k)
            for (int i = 0; i < subs; ++i) {
                const double x = std::round(t.x0 + (k + static_cast<double>(i) / subs) * px_major) + 0.5;
                if (x > right) break;
                const double len = i == 0 ? 11 : (subs % 2 == 0 && i == subs / 2 ? 7 : 4);
                cr->move_to(x, r.b() - 1);
                cr->line_to(x, r.b() - 1 - len);
            }
        cr->stroke();
        auto font = Pango::FontDescription("Sans 7");
        for (int k = 0; t.x0 + k * px_major <= right - 8; ++k) {
            const double v = k * major;
            char buf[16];
            if (std::abs(v - std::round(v)) < 1e-9) std::snprintf(buf, sizeof buf, "%.0f", v);
            else std::snprintf(buf, sizeof buf, "%g", v);
            auto lay = create_pango_layout(buf);
            lay->set_font_description(font);
            cr->move_to(std::round(t.x0 + k * px_major) + 2, r.b() - 21);
            lay->show_in_cairo_context(cr);
        }
    }
    // The stops: a mark at each, its kind in a letter, dots for a leader.
    std::vector<core::TabStop> tabs = t.tabs;
    if (m_drag == Drag::Tab) {   // the one in hand, where it is
        const double pos = std::max(0.0, std::round((m_tab_x - t.x0) / t.scale));
        if (m_tab >= 0 && m_tab < static_cast<int>(tabs.size())) {
            if (m_tab_off) tabs.erase(tabs.begin() + m_tab);
            else tabs[static_cast<std::size_t>(m_tab)].pos = pos;
        } else if (!m_tab_off) {
            tabs.push_back({pos, core::TabAlign::Left, ""});
        }
    }
    static const char* letter[] = {"L", "C", "R", "D"};
    for (const auto& s : tabs) {
        const double x = t.x0 + s.pos * t.scale;
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 1);
        cr->move_to(x, r.b() - 1);
        cr->line_to(x - 5, r.b() - 9);
        cr->line_to(x + 5, r.b() - 9);
        cr->close_path();
        cr->fill();
        auto lay = create_pango_layout(std::string(letter[static_cast<int>(s.align)]) + (s.leader.empty() ? "" : "…"));
        lay->set_font_description(Pango::FontDescription("Sans Bold 7"));
        int lw = 0, lh = 0;   // s160 fix: on a chip, over the numbers
        lay->get_pixel_size(lw, lh);
        cr->rectangle(x - 4, r.y + 1, lw + 2, lh);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
        cr->move_to(x - 3, r.y + 1);
        lay->show_in_cairo_context(cr);
    }
    cr->restore();
}

void Canvas::draw_edit(const Cairo::RefPtr<Cairo::Context>& cr) {
    const auto& flow = flows().get(m_doc, m_edit_story);
    const auto lo = std::min(m_anchor, m_caret), hi = std::max(m_anchor, m_caret);
    cr->save();
    for (const auto& ft : flow.frames) {
        const Rect lr = core::frame_in_layout(m_doc, m_placed, ft.frame);
        if (lr.w <= 0) continue;
        // The frames being typed in: a quiet accent outline.
        const Rect fr = m_vp.to_screen(lr);
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.6);
        cr->set_line_width(1);
        cr->rectangle(std::round(fr.x) + 0.5, std::round(fr.y) + 0.5, std::round(fr.w), std::round(fr.h));
        cr->stroke();
        for (const auto& r : layout::selection_rects(m_doc, m_edit_story, flow, ft.frame, lo, hi)) {
            const Rect sr = m_vp.to_screen(Rect{lr.x + r.x, lr.y + r.y, r.w, r.h});
            cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.28);
            cr->rectangle(sr.x, sr.y, sr.w, sr.h);
            cr->fill();
        }
    }
    // s138: index marks -- unseen on the page, shown while typing: a small
    // orange flag at each (its words in the tooltip).
    if (const auto* st = edit_story())
        for (int p = 0; p < static_cast<int>(st->paras.size()); ++p) {
            const auto& t = st->paras[static_cast<std::size_t>(p)].text;
            for (auto i = t.find(core::kMarkIndex); i != std::string::npos; i = t.find(core::kMarkIndex, i + 3)) {
                const auto c = layout::caret_at(m_doc, m_edit_story, flow, {p, static_cast<int>(i)});
                const Rect lr = c.ok ? core::frame_in_layout(m_doc, m_placed, c.frame) : Rect{};
                if (!c.ok || lr.w <= 0) continue;
                const Rect sr = m_vp.to_screen(Rect{lr.x + c.x, lr.y + c.y, 0, c.h});
                const double x = std::round(sr.x) + 0.5, s = std::clamp(sr.h * 0.22, 2.5, 6.0);
                cr->set_source_rgba(0.93, 0.47, 0.05, 0.95);
                cr->set_line_width(1);
                cr->move_to(x, sr.y + sr.h * 0.15);
                cr->line_to(x, sr.b());
                cr->stroke();
                cr->move_to(x, sr.y + sr.h * 0.15);
                cr->line_to(x + s * 1.4, sr.y + sr.h * 0.15 + s * 0.6);
                cr->line_to(x, sr.y + sr.h * 0.15 + s * 1.2);
                cr->close_path();
                cr->fill();
            }
        }
    if (m_caret_on && lo == hi) {
        const auto c = layout::caret_at(m_doc, m_edit_story, flow, m_caret);
        const Rect lr = c.ok ? core::frame_in_layout(m_doc, m_placed, c.frame) : Rect{};
        if (c.ok && lr.w > 0) {
            const Rect sr = m_vp.to_screen(Rect{lr.x + c.x, lr.y + c.y, 0, c.h});
            cr->set_source_rgb(0.05, 0.05, 0.08);
            cr->set_line_width(std::max(1.0, std::min(2.0, m_vp.scale())));
            cr->move_to(std::round(sr.x) + 0.5, sr.y);
            cr->line_to(std::round(sr.x) + 0.5, sr.b());
            cr->stroke();
        }
    }
    // s158: the composing text at the caret -- set in the paragraph's face at
    // the zoom, on paper, underlined; the input method's window told where.
    if (!m_preedit.empty() && lo == hi) {
        const auto c = layout::caret_at(m_doc, m_edit_story, flow, m_caret);
        if (c.ok) {
            const Rect fr = core::frame_in_layout(m_doc, m_placed, c.frame);
            const Rect sr = m_vp.to_screen(Rect{fr.x + c.x, fr.y + c.y, 1, c.h});
            const auto* st = edit_story();
            core::Style ps;
            if (st && m_caret.para < static_cast<int>(st->paras.size()))
                ps = core::doc::para_style(m_doc, st->paras[static_cast<std::size_t>(m_caret.para)]);
            auto lay = create_pango_layout(m_preedit);
            Pango::FontDescription fd(ps.family.empty() ? std::string("Serif") : ps.family);
            fd.set_absolute_size(std::max(4.0, (ps.size > 0 ? ps.size : 11) * m_vp.scale()) * PANGO_SCALE);
            lay->set_font_description(fd);
            int lw = 0, lh = 0;
            lay->get_pixel_size(lw, lh);
            const double ty = sr.b() - lh;
            cr->set_source_rgba(1, 1, 1, 0.95);
            cr->rectangle(sr.x, ty, lw + 2, lh);
            cr->fill();
            cr->set_source_rgb(0.05, 0.05, 0.08);
            cr->move_to(sr.x + 1, ty);
            lay->show_in_cairo_context(cr);
            cr->set_line_width(1);
            cr->move_to(sr.x + 1, std::round(sr.b()) - 0.5);
            cr->line_to(sr.x + 1 + lw, std::round(sr.b()) - 0.5);
            cr->stroke();
            GdkRectangle area{static_cast<int>(sr.x), static_cast<int>(sr.y), 1, static_cast<int>(sr.h)};
            gtk_im_context_set_cursor_location(m_im, &area);
        }
    }
    cr->restore();
}

// ── s009: placing images ────────────────────────────────────────────────────
bool Canvas::direct_shape(const std::string& fid, double& dx, double& dy) const {   // s200
    const auto* f = m_doc.as<core::Frame>(fid);
    // s210: a shape, or a text frame (its box's corners, or its shape) -- a picture frame's Direct is its picture
    if (!f || f->kind == core::FrameKind::Image || f->rot != 0) return false;
    if (std::none_of(m_placed.begin(), m_placed.end(), [&](const core::Placed& p) { return p.id == f->owner; })) return false;
    const Rect fl = core::frame_in_layout(m_doc, m_placed, fid);
    dx = fl.x - f->x;
    dy = fl.y - f->y;
    return true;
}

bool Canvas::direct_pic(const std::string& fid, Rect& fl, Rect& pl) const {   // s186
    const auto* f = m_doc.as<core::Frame>(fid);
    if (!f || f->kind != core::FrameKind::Image || f->rot != 0) return false;
    const auto* a = m_doc.as<core::Asset>(f->asset);
    if (!a) return false;
    if (std::none_of(m_placed.begin(), m_placed.end(), [&](const core::Placed& p) { return p.id == f->owner; })) return false;
    fl = core::frame_in_layout(m_doc, m_placed, fid);
    const auto nat = core::picture_natural(*a);
    const double iw = a->px_w > 0 ? a->px_w : nat.w, ih = a->px_h > 0 ? a->px_h : nat.h;
    pl = core::picture_rect(fl, *f, iw, ih);
    return pl.w > 0 && pl.h > 0;
}

void Canvas::take_colour(std::function<void(std::optional<std::uint32_t>)> done) {   // s180
    cancel_take();
    end_edit();
    if (m_drag != Drag::None) cancel_drag();
    m_thread_from.clear();
    m_load.clear();
    m_take = std::move(done);
    m_take_under = m_in ? colour_at(m_mx, m_my) : std::nullopt;
    grab_focus();
    update_cursor();
    queue_draw();
}

void Canvas::cancel_take() {   // s180
    if (!m_take) return;
    auto done = std::move(m_take);
    m_take = nullptr;
    m_take_under.reset();
    update_cursor();
    queue_draw();
    done(std::nullopt);
}

std::optional<std::uint32_t> Canvas::colour_at(double x, double y) const {   // s180
    double lx = 0, ly = 0;
    m_vp.to_doc(x, y, lx, ly);
    for (const auto& p : m_placed) {
        if (!p.r.contains(lx, ly)) continue;
        const std::string dir = m_dir;
        const layout::ImageSource pics = [dir](const core::Asset& a, double, double) -> cairo_surface_t* {
            auto pic = images::get(core::resolve_asset(a, dir), a.stamp);
            return pic ? cairo_surface_reference(pic->cobj()) : nullptr;
        };
        return layout::sample_page(m_doc, p.id, flows(), pics, lx - p.r.x, ly - p.r.y,
                                   std::min(1.0, 3 / m_vp.scale()));   // at most a point: type gives its ink at any zoom
    }
    return std::nullopt;
}

void Canvas::load_image(const core::ImageFile& f) { load_images({f}); }

void Canvas::load_images(std::vector<core::ImageFile> files) {
    m_load.load(std::move(files));
    if (m_load.empty()) return;
    m_thread_from.clear();
    grab_focus();
    update_cursor();
    queue_draw();
}

// Put the loaded file down at (lx, ly) (layout): into the image frame there,
// or a new frame -- the one drawn, else the file's own size, kept on the page.
void Canvas::place_loaded(double lx, double ly, const Rect* drawn) {
    if (m_load.empty()) return;
    core::ImageFile file = m_load.current();
    std::string made;
    front_panel(lx, ly);   // s150
    if (!drawn) {
        const auto hit = core::hit_frame(m_doc, m_placed, lx, ly, 3 / m_vp.scale());
        if (const auto* f = m_doc.as<core::Frame>(hit); f && (f->kind == core::FrameKind::Image || f->kind == core::FrameKind::Shape)) {   // s210: or into a shape
            if (f->wrap == core::Wrap::Shape) file.shape = images::trace(file.path, file.stamp);   // s175: its outline, as Replace
            core::doc::place_image(m_journal, m_doc, hit, file);
            made = hit;
        }
    }
    if (made.empty()) {
        const auto page = core::page_at(m_placed, lx, ly);
        for (const auto& p : m_placed) {
            if (p.id != page) continue;
            Rect r;
            if (drawn) {
                r = {drawn->x - p.r.x, drawn->y - p.r.y, drawn->w, drawn->h};
            } else {
                const auto [w, h] = core::natural_size(file, p.r.w, p.r.h);
                r = {std::clamp(lx - p.r.x, 0.0, std::max(0.0, p.r.w - w)),
                     std::clamp(ly - p.r.y, 0.0, std::max(0.0, p.r.h - h)), w, h};
            }
            made = core::doc::place_image_new(m_journal, m_doc, p.id, r, file);
        }
    }
    if (made.empty()) return;   // the pasteboard: still loaded, try again
    m_load.placed();             // s063: the next one comes up (if any)
    relayout();
    pick({made});
    m_sig_edited.emit();
    update_cursor();
}

// Files dropped from the desktop: the first readable image places where it
// lands (the rest are ignored for now -- one at a time, as Place).
bool Canvas::on_drop(const Glib::ValueBase& v, double x, double y) {
    if (!G_VALUE_HOLDS(v.gobj(), GDK_TYPE_FILE_LIST)) return false;
    auto* list = static_cast<GdkFileList*>(g_value_get_boxed(v.gobj()));
    if (!list) return false;
    GSList* files = gdk_file_list_get_files(list);
    // s063: every readable picture -- the first placed where it lands, the
    // rest left on the cursor for a click each.
    std::vector<core::ImageFile> found;
    for (GSList* l = files; l; l = l->next) {
        char* path = g_file_get_path(G_FILE(l->data));
        if (path) if (auto f = images::probe(path)) found.push_back(*f);
        g_free(path);
    }
    g_slist_free(files);
    if (found.empty()) return false;
    double lx, ly;
    m_vp.to_doc(x, y, lx, ly);
    const std::size_t n = found.size();
    m_load.load(std::move(found));
    place_loaded(lx, ly, nullptr);
    if (m_load.size() == n) { m_load.clear(); update_cursor(); return false; }   // dropped on the pasteboard
    grab_focus();
    update_cursor();
    queue_draw();
    return true;
}

void Canvas::update_cursor() {
    if (m_space) { set_cursor(Gdk::Cursor::create(m_drag == Drag::Pan ? "grabbing" : "grab")); return; }
    if (m_take) { set_cursor(Gdk::Cursor::create("crosshair")); return; }   // s180: the eyedropper
    if (!m_thread_from.empty() || !m_load.empty()) { set_cursor(Gdk::Cursor::create("copy")); return; }   // loaded
    if (!out_port_hit(m_mx, m_my).empty() && !editing()) { set_cursor(Gdk::Cursor::create("pointer")); return; }
    if (editing() || m_tool == Tool::Text) {   // s010: the I-beam over text you can type in
        const auto* f = m_doc.as<core::Frame>(m_hover);
        if (f && f->kind == core::FrameKind::Text && !f->story.empty() && (!editing() || f->story == m_edit_story)) {
            set_cursor(Gdk::Cursor::create("text"));
            return;
        }
    }
    if (m_tool == Tool::Direct) {   // s186: a picture's handle, else a hand over pictures
        Rect fl, pl;
        if (!m_direct.empty() && direct_pic(m_direct, fl, pl)) {
            const auto h = core::handle_hit(m_vp.to_screen(pl), m_mx, m_my);
            if (h != core::Handle::None) { set_cursor(Gdk::Cursor::create(cursor_for(h))); return; }
        }
        double lx = 0, ly = 0;
        m_vp.to_doc(m_mx, m_my, lx, ly);
        const bool over = (direct_pic(m_hover, fl, pl)) || (!m_direct.empty() && direct_pic(m_direct, fl, pl) && pl.contains(lx, ly));
        set_cursor(Gdk::Cursor::create(m_drag == Drag::Picture ? "grabbing" : over ? "grab" : "default"));
        return;
    }
    if (m_tool != Tool::Select) { set_cursor(Gdk::Cursor::create("crosshair")); return; }
    if (!m_sel.empty()) {
        const auto h = core::handle_hit(m_vp.to_screen(selection_box()), m_mx, m_my);
        if (h != core::Handle::None) { set_cursor(Gdk::Cursor::create(cursor_for(h))); return; }
    }
    if (m_hover_gutter >= 0 || m_drag == Drag::Gutter) { set_cursor(Gdk::Cursor::create("col-resize")); return; }
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
void CanvasView::set_proof_label(const std::string& profile) {   // s099
    if (!m_proof_pill) return;
    m_proof_pill->set_visible(!profile.empty());
    m_proof_pill->set_text("Proof: " + profile);
    m_proof_pill->set_tooltip_text("Colours as " + profile + " will print them (View \u203a Proof Colours)");
}

CanvasView::CanvasView(core::Doc& d, core::DocJournal& j)
    : widgets::Box("canvas_view", Gtk::Orientation::VERTICAL),
      m_head("canvas_view.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("canvas_view.title"),
      m_count("canvas_view.count"),
      m_tools("canvas_view.tools", Gtk::Orientation::VERTICAL),
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
    m_nav_btns = nav_buttons("canvas_view");   // s068
    m_head.append(*m_nav_btns.box);
    m_head.append(m_title);
    m_head.append(m_count);
    // s008: the tools -- a vertical strip down the canvas's left edge
    // (Scott: object tools in a vertical panel, standard icons). Text and
    // Image are the theme's icons; Adwaita has no pointer or shape icon, so
    // those two ship in the resource bundle, drawn in the same symbolic style.
    m_tools.add_css_class("cx-toolstrip");
    m_tools.set_vexpand(true);
    struct T { const char* icon; const char* name; const char* tip; Canvas::Tool tool; };
    static const T tools[] = {
        {"composix-tool-select-symbolic", "canvas_view.tool.select", "Select  (V)", Canvas::Tool::Select},
        {"composix-tool-direct-symbolic", "canvas_view.tool.direct", "Direct  (A) — a picture inside its frame: drag to move it, a corner to scale it",
         Canvas::Tool::Direct},   // s186
        {"insert-text-symbolic",          "canvas_view.tool.text",   "Text frame  (T) — drag on a page", Canvas::Tool::Text},
        {"image-x-generic-symbolic",      "canvas_view.tool.image",  "Image frame  (F) — drag on a page", Canvas::Tool::Image},
        {"composix-tool-rect-symbolic",   "canvas_view.tool.shape",  "", Canvas::Tool::Shape},   // s198: one button, four tools
        {"composix-tool-eyedropper-symbolic", "canvas_view.tool.eyedropper",
         "Eyedropper  (I) — click the page: its colour onto the picked frames' fill; Shift: their stroke; Alt: just a swatch",
         Canvas::Tool::Eyedropper},
    };
    // s198 (Scott): the shapes share a button -- it draws what was picked
    // last; right-click (or hold) for the others. A small mark in its
    // corner says there's more.
    struct S { const char* icon; const char* label; const char* key; Canvas::Tool tool; Canvas::ShapeTool shape; const char* tip; };
    static const S shapes[] = {
        {"composix-tool-rect-symbolic", "Rectangle", "M", Canvas::Tool::Shape, Canvas::ShapeTool::Rect, "Rectangle  (M) — drag on a page; Shift for a square"},
        {"composix-tool-oval-symbolic", "Oval", "L", Canvas::Tool::Shape, Canvas::ShapeTool::Oval, "Oval  (L) — drag on a page; Shift for a circle"},
        {"composix-tool-line-symbolic", "Line", "\\", Canvas::Tool::Shape, Canvas::ShapeTool::Line, "Line  (\\) — drag on a page; Shift at 45°"},
        {"composix-tool-polygon-symbolic", "Polygon", "", Canvas::Tool::Shape, Canvas::ShapeTool::Polygon,
         "Polygon / star \u2014 drag from its centre; Shift turns it in 15\u00b0 steps (sides and star depth in this menu)"},   // s206
        {"composix-tool-pen-symbolic", "Pen", "P", Canvas::Tool::Pen, Canvas::ShapeTool::Rect,
         "Pen  (P) — click for corners, drag for curves; click the first point to close, Enter to end"},
    };
    const auto shape_index = [this] {
        if (m_canvas.tool() == Canvas::Tool::Pen) return 4;
        return static_cast<int>(m_canvas.shape_tool());
    };
    Gtk::ToggleButton* first = nullptr;
    auto shape_btn_p = std::make_shared<Gtk::ToggleButton*>(nullptr);
    auto last_shape = std::make_shared<int>(0);   // what the button draws
    const auto show_shape = [shape_btn_p, last_shape](int k) {
        *last_shape = k;
        (*shape_btn_p)->set_icon_name(shapes[k].icon);
        (*shape_btn_p)->set_tooltip_text(std::string(shapes[k].tip) + "\nClick again (or right-click, or hold): rectangle, oval, line, polygon, pen");
    };
    const auto use_shape = [this, last_shape](int k) {
        if (shapes[k].tool == Canvas::Tool::Pen) m_canvas.set_tool(Canvas::Tool::Pen);
        else m_canvas.set_shape_tool(shapes[k].shape);
        (void)last_shape;
    };
    for (const auto& t : tools) {
        auto b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_icon_name(t.icon);
        b->set_name(t.name);
        b->set_tooltip_text(t.tip);
        b->add_css_class("cx-tool");
        if (first) b->set_group(*first); else first = b;
        const auto tool = t.tool;
        m_tool_buttons.push_back(b);
        if (tool != Canvas::Tool::Shape) {
            b->signal_toggled().connect([this, b, tool] { if (b->get_active()) m_canvas.set_tool(tool); });
            m_tools.append(*b);
            continue;
        }
        *shape_btn_p = b;
        b->signal_toggled().connect([b, use_shape, last_shape, this, shape_index] {
            if (!b->get_active()) return;
            const bool already = (m_canvas.tool() == Canvas::Tool::Shape || m_canvas.tool() == Canvas::Tool::Pen) &&
                                 shape_index() == *last_shape;
            if (!already) use_shape(*last_shape);
        });
        // The menu of the four.
        auto pop = Gtk::make_managed<Gtk::Popover>();
        pop->set_name("canvas_view.tool.shapes");
        pop->set_position(Gtk::PositionType::RIGHT);
        pop->set_has_arrow(true);
        pop->set_autohide(true);
        auto list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        for (int k = 0; k < 5; ++k) {
            auto row = Gtk::make_managed<Gtk::Button>();
            row->add_css_class("flat");
            row->set_name(std::string("canvas_view.tool.shapes.") + shapes[k].label);
            auto hb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto icon = Gtk::make_managed<Gtk::Image>();
            icon->set_from_icon_name(shapes[k].icon);
            hb->append(*icon);
            auto name = Gtk::make_managed<Gtk::Label>(shapes[k].label);
            name->set_xalign(0);
            name->set_hexpand(true);
            hb->append(*name);
            auto key = Gtk::make_managed<Gtk::Label>(shapes[k].key);
            key->add_css_class("dim-label");
            hb->append(*key);
            row->set_child(*hb);
            row->signal_clicked().connect([pop, k, show_shape, use_shape, b] {
                pop->popdown();
                show_shape(k);
                use_shape(k);
                b->set_active(true);
            });
            list->append(*row);
        }
        {   // s206: the polygon's sides and star depth (the next one drawn)
            auto grid = Gtk::make_managed<Gtk::Grid>();
            grid->set_row_spacing(4);
            grid->set_column_spacing(8);
            grid->set_margin_top(6);
            grid->set_margin_start(8);
            grid->set_margin_end(8);
            grid->set_margin_bottom(4);
            const auto lab = [](const char* t) {
                auto l = Gtk::make_managed<Gtk::Label>(t);
                l->set_xalign(1);
                l->add_css_class("dim-label");
                return l;
            };
            auto sides = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(m_canvas.polygon_sides(), 3, 64, 1, 5), 1, 0);
            sides->set_name("canvas_view.tool.polygon.sides");
            auto star = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(m_canvas.polygon_inner() * 100, 1, 100, 1, 10), 1, 0);
            star->set_name("canvas_view.tool.polygon.star");
            star->set_tooltip_text("Star depth: 100% a plain polygon; lower, the inner points come in (a five-point star snaps to straight sides near 38%)");
            const auto apply = [this, sides, star] { m_canvas.set_polygon(sides->get_value_as_int(), star->get_value() / 100); };
            sides->signal_value_changed().connect(apply);
            star->signal_value_changed().connect(apply);
            grid->attach(*lab("Sides"), 0, 0);
            grid->attach(*sides, 1, 0);
            grid->attach(*lab("Star depth %"), 0, 1);
            grid->attach(*star, 1, 1);
            list->append(*Gtk::make_managed<Gtk::Separator>());
            list->append(*grid);
        }
        pop->set_child(*list);
        auto ov = Gtk::make_managed<Gtk::Overlay>();
        ov->set_child(*b);
        auto mark = Gtk::make_managed<Gtk::DrawingArea>();   // the corner mark
        mark->set_content_width(5);
        mark->set_content_height(5);
        mark->set_halign(Gtk::Align::END);
        mark->set_valign(Gtk::Align::END);
        mark->set_margin_end(3);
        mark->set_margin_bottom(3);
        mark->set_can_target(false);
        mark->set_draw_func([mark](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            const auto c = mark->get_color();
            cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), 0.8);
            cr->move_to(w, 0);
            cr->line_to(w, h);
            cr->line_to(0, h);
            cr->close_path();
            cr->fill();
        });
        ov->add_overlay(*mark);
        pop->set_parent(*ov);
        m_shape_pop = pop;
        // s201 (Scott: the menu didn't show): every way in, ahead of the
        // button's own handling (the capture phase) -- a click on the button
        // when it's already the tool, a right-click, a hold.
        auto click = Gtk::GestureClick::create();
        click->set_button(0);   // any button
        click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        auto was_active = std::make_shared<bool>(false);
        click->signal_pressed().connect([click, b, pop, was_active](int, double, double) {
            *was_active = b->get_active();
            if (click->get_current_button() == GDK_BUTTON_SECONDARY) {
                click->set_state(Gtk::EventSequenceState::CLAIMED);
                pop->popup();
            }
        });
        click->signal_released().connect([click, pop, was_active](int, double, double) {
            if (click->get_current_button() == GDK_BUTTON_PRIMARY && *was_active) pop->popup();
        });
        ov->add_controller(click);
        auto hold = Gtk::GestureLongPress::create();
        hold->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        hold->signal_pressed().connect([pop](double, double) { pop->popup(); });
        ov->add_controller(hold);
        m_tools.append(*ov);
    }
    show_shape(0);
    first->set_active(true);
    m_canvas.signal_tool().connect([this, show_shape, shape_index] {
        const auto t = m_canvas.tool();
        int i = 0;
        switch (t) {
        case Canvas::Tool::Select: i = 0; break;
        case Canvas::Tool::Direct: i = 1; break;
        case Canvas::Tool::Text: i = 2; break;
        case Canvas::Tool::Image: i = 3; break;
        case Canvas::Tool::Shape: case Canvas::Tool::Pen: i = 4; show_shape(shape_index()); break;
        case Canvas::Tool::Eyedropper: i = 5; break;
        }
        m_tool_buttons[static_cast<std::size_t>(i)]->set_active(true);
    });
    m_proof_pill = Gtk::make_managed<Gtk::Label>();   // s099: shown while proofing
    m_proof_pill->set_name("canvas_view.proof");
    m_proof_pill->add_css_class("cx-count");
    m_proof_pill->add_css_class("cx-proof-pill");
    m_proof_pill->set_valign(Gtk::Align::CENTER);
    m_proof_pill->set_visible(false);
    m_head.append(*m_proof_pill);
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
    // s154: the zero point -- dragged out of the corner onto the canvas;
    // double-click puts it back. Spine: fixed.
    corner->set_tooltip_text("Drag onto the page to move the rulers' zero point; double-click to put it back");
    {
        auto drag = Gtk::GestureDrag::create();
        auto x0 = std::make_shared<double>(0), y0 = std::make_shared<double>(0);
        drag->signal_drag_begin().connect([x0, y0](double x, double y) { *x0 = x; *y0 = y; });
        drag->signal_drag_update().connect([this, corner, x0, y0](double dx, double dy) {
            if (m_canvas.doc().settings().ruler_origin == core::RulerOrigin::Spine) return;
            double cx = 0, cy = 0;
            corner->translate_coordinates(m_canvas, *x0 + dx, *y0 + dy, cx, cy);
            m_canvas.preview_zero(cx, cy);
        });
        drag->signal_drag_end().connect([this, corner, x0, y0](double dx, double dy) {
            double cx = 0, cy = 0;
            corner->translate_coordinates(m_canvas, *x0 + dx, *y0 + dy, cx, cy);
            m_canvas.drop_zero(cx, cy);
        });
        corner->add_controller(drag);
        auto click = Gtk::GestureClick::create();
        click->signal_pressed().connect([this](int n, double, double) { if (n == 2) m_canvas.reset_zero(); });
        corner->add_controller(click);
    }
    m_grid.attach(m_tools, 0, 0, 1, 2);   // the tool strip, the full height
    m_grid.attach(*corner, 1, 0);
    m_grid.attach(*m_top, 2, 0);
    m_grid.attach(*m_left, 1, 1);
    m_grid.attach(m_canvas, 2, 1);
    m_grid.set_vexpand(true);
    append(m_grid);
    m_canvas.signal_zoom().connect(sigc::mem_fun(*this, &CanvasView::sync_zoom));
    m_canvas.signal_view().connect([this] { m_top->queue_draw(); m_left->queue_draw(); });
}

CanvasView::~CanvasView() {
    if (m_shape_pop && m_shape_pop->get_parent()) m_shape_pop->unparent();   // s198
}

void CanvasView::set_rulers(bool on) {
    m_top->set_visible(on);
    m_left->set_visible(on);
    if (auto* c = m_grid.get_child_at(1, 0)) c->set_visible(on);
}

void CanvasView::queue_rulers() {   // s073
    if (m_top) m_top->queue_draw();
    if (m_left) m_left->queue_draw();
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
