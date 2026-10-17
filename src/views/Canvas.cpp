#include "views/Canvas.hpp"
#include "views/BackButton.hpp"
#include "views/Images.hpp"
#include "views/Ruler.hpp"
#include "layout/Flow.hpp"
#include "Log.hpp"

#include <gdkmm/cursor.h>
#include <glibmm/main.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/root.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/togglebutton.h>
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
    gtk_im_context_set_use_preedit(m_im, FALSE);
    g_signal_connect(m_im, "commit", G_CALLBACK(+[](GtkIMContext*, const char* t, gpointer self) {
        auto* c = static_cast<Canvas*>(self);
        if (c->editing() && t) c->edit_insert(t, "type", "Typing");
    }), this);
}

Canvas::~Canvas() {
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
    if (const Rect mr = core::margin_rect(m_doc, p.id); mr.w > 0) {
        const Rect smr = m_vp.to_screen(Rect{p.r.x + mr.x, p.r.y + mr.y, mr.w, mr.h});
        cr->set_line_width(1);
        cr->set_source_rgba(0.85, 0.25, 0.65, 0.7);
        cr->rectangle(std::round(smr.x) + 0.5, std::round(smr.y) + 0.5, std::round(smr.w), std::round(smr.h));
        cr->stroke();
        const auto cols = core::page_columns(m_doc, p.id);
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
    const Rect mr = core::margin_rect(m_doc, p.id);
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
    // s027: in a spread the bleed runs round the spread -- none at the spine.
    const double bleed = m_doc.settings().bleed * s;
    if (bleed > 1) {
        bool at_left = false, at_right = false;   // a page touching this one at the spine
        for (const auto& q : m_placed) {
            if (q.id == p.id || q.master || p.master) continue;
            at_left = at_left || std::abs(q.r.x + q.r.w - p.r.x) < 0.01;
            at_right = at_right || std::abs(p.r.x + p.r.w - q.r.x) < 0.01;
        }
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
                if (core::master_item_on(m_doc, fid, p.id))   // s025: its side, not overridden here
                    draw_frame(cr, *m_doc.as<core::Frame>(fid), p.r.x, p.r.y, true, fid, p.id);
    for (const auto& fid : m_doc.frames_on(p.id))
        draw_frame(cr, *m_doc.as<core::Frame>(fid), p.r.x, p.r.y, false, fid, p.id);

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
                        bool from_master, const std::string& id, const std::string& on_page) {
    const Rect r = m_vp.to_screen(Rect{ox + f.x, oy + f.y, f.w, f.h});
    const double s = m_vp.scale();
    const double dim = from_master ? 0.45 : 1.0;
    const double op = std::clamp(f.opacity, 0.0, 1.0);   // s021: on fill, content and stroke alike
    // s016: the frame's fill, behind its content (and its stroke, after).
    if (const auto* fs = m_doc.as<core::Swatch>(f.fill)) {
        const auto c = core::tint_rgb(fs->preview, f.fill_tint);   // s021
        cr->save();
        cr->set_source_rgba(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, dim * op);
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->fill();
        cr->restore();
    }
    struct StrokeLater {
        const Cairo::RefPtr<Cairo::Context>& cr; const core::Doc& d; const core::Frame& f; Rect r; double s, dim;
        ~StrokeLater() {
            const auto* ss = d.as<core::Swatch>(f.stroke);
            if (!ss || f.stroke_w <= 0) return;
            const auto c = core::tint_rgb(ss->preview, f.stroke_tint);   // s021
            cr->save();
            cr->set_source_rgba(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0,
                                dim * std::clamp(f.opacity, 0.0, 1.0));
            cr->set_line_width(std::max(0.5, f.stroke_w * s));
            cr->rectangle(r.x, r.y, r.w, r.h);   // centred on the frame's edge, as it prints
            cr->stroke();
            cr->restore();
        }
    } stroke_later{cr, m_doc, f, r, s, dim};
    cr->save();

    switch (f.kind) {
    case core::FrameKind::Text: {
        // s007: the story's text, flowed through the thread (layout::Flow),
        // drawn at true size and scaled -- breaks never change with zoom.
        const auto& flow = m_flows.get(m_doc, f.story);
        cr->save();
        cr->rectangle(r.x, r.y, r.w, r.h);
        cr->clip();
        cr->translate(r.x, r.y);
        cr->scale(s, s);
        if (from_master || op < 1) cr->push_group();
        // s023: page numbers and the like filled in -- except in the story
        // being edited, whose markers show as signs (every byte where it was).
        const bool live = editing() && m_edit_story == f.story;
        const std::string ctx = from_master && m_doc.as<core::Page>(on_page) ? on_page : std::string{};
        const layout::Resolved* rs = live ? nullptr : &m_flows.resolved(m_doc, f.story, ctx);
        if (rs && rs->any) layout::draw_frame_text(cr->cobj(), m_doc, f.story, rs->flow, id, 0, 0, 0.08, &rs->text);
        else layout::draw_frame_text(cr->cobj(), m_doc, f.story, flow, id, 0, 0);
        // s059: hidden characters, over the words, in the selection's blue (never printed).
        if (m_show_hidden && !from_master) {
            const auto marks = (rs && rs->any) ? layout::hidden_marks(m_doc, f.story, rs->flow, id, &rs->text)
                                               : layout::hidden_marks(m_doc, f.story, flow, id);
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
        const auto [at, total] = core::thread_place(m_doc, id);
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
            const Rect in = port_rect(id, false), out = port_rect(id, true);
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
        const auto* asset = m_doc.as<core::Asset>(f.asset);
        const auto status = asset ? core::link_status(*asset, m_dir) : core::LinkStatus::Missing;
        const bool missing = status == core::LinkStatus::Missing || status == core::LinkStatus::EmbeddedMissing;
        // s009: the real picture, cropped by its frame.
        if (asset && !missing) {
            const std::string file = core::resolve_asset(*asset, m_dir);
            int iw = 0, ih = 0;
            if (auto pic = images::get(file, core::file_stamp(file), &iw, &ih)) {
                cr->save();
                cr->rectangle(r.x, r.y, r.w, r.h);
                cr->clip();
                const Rect ir = core::picture_rect(r, f, iw, ih, s);   // s035: Custom placement too
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
        if (!f.fill.empty() || !f.stroke.empty()) break;   // s016: it has its own look
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
            if (m_doc.as<core::Frame>(id)->kind != core::FrameKind::Text) continue;
            const Rect pr = port_rect(id, true);
            if (pr.w > 0 && Rect{pr.x - 3, pr.y - 3, pr.w + 6, pr.h + 6}.contains(sx, sy)) return id;
        }
    return {};
}

void Canvas::draw_overlays(const Cairo::RefPtr<Cairo::Context>& cr) {
    draw_threads(cr);
    cr->set_line_width(1);
    if (m_drag == Drag::Draw && m_draw.w > 0) {   // the frame being drawn
        const Rect r = m_vp.to_screen(m_draw);
        std::vector<double> dash{5, 3};
        cr->set_dash(dash, 0);
        cr->set_source_rgba(kAcc[0], kAcc[1], kAcc[2], 0.9);
        cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
        cr->stroke();
        cr->unset_dash();
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
        if (f && f->wrap != core::Wrap::None) {
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
    if (!m_sel.empty() && !editing()) {
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

    double px0, py0;
    m_vp.to_doc(x, y, px0, py0);

    // s010: clicks counted by hand (double-click = edit / pick a word).
    const gint64 now = g_get_monotonic_time();
    if (now - m_last_press < 400000 && std::hypot(x - m_last_px, y - m_last_py) < 5) ++m_clicks;
    else m_clicks = 1;
    m_last_press = now;
    m_last_px = x;
    m_last_py = y;
    // s034: on the tab ruler -- a stop: drag it (double-click: its kind,
    // Shift+double-click: its leader); elsewhere: a new one.
    if (const auto t = tab_strip(); t.ok && t.screen.contains(x, y)) {
        const int hit = tab_hit(t, x);
        if (hit >= 0 && m_clicks >= 2) {
            const bool leader = m_mods_shift;
            core::doc::edit_styles(m_journal, m_doc, {t.style}, leader ? "Tab leader" : "Tab alignment", [&](core::Style& s) {
                if (hit >= static_cast<int>(s.tabs.size())) return;
                auto& stop = s.tabs[static_cast<std::size_t>(hit)];
                if (leader) stop.leader = stop.leader.empty() ? "." : "";
                else stop.align = static_cast<core::TabAlign>((static_cast<int>(stop.align) + 1) % 4);
            });
            m_sig_edited.emit();
            m_drag = Drag::None;
            queue_draw();
            return;
        }
        m_tab = hit;
        m_tab_x = hit >= 0 ? t.x0 + m_doc.as<core::Style>(t.style)->tabs[static_cast<std::size_t>(hit)].pos * t.scale : x;
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
        if (m_thread_from.empty() && m_load.empty() && text_frame && !m_mods_alt && out_port_hit(x, y).empty() &&
            ((m_tool == Tool::Select && m_clicks >= 2) || m_tool == Tool::Text)) {
            begin_edit(under, hit_story(under, px0, py0));
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
    m_mods_ctrl  = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
}

void Canvas::on_drag_update(double ox, double oy) {
    read_mods();
    const double s = m_vp.scale();
    switch (m_drag) {
    case Drag::None:
        return;
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
        if (m_mods_shift) {   // Shift: square
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
    if (was == Drag::Text) { queue_draw(); return; }
    if (was == Drag::Picture) {   // s035: the move, one step
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
            if (!(off && k < 0))
                core::doc::edit_styles(m_journal, m_doc, {t.style}, label, [&](core::Style& s) {
                    if (k >= 0 && k < static_cast<int>(s.tabs.size())) {
                        if (off) s.tabs.erase(s.tabs.begin() + k);
                        else s.tabs[static_cast<std::size_t>(k)].pos = pos;
                    } else if (!off) {
                        s.tabs.push_back({pos, core::TabAlign::Left, ""});
                    }
                    std::sort(s.tabs.begin(), s.tabs.end(), [](const core::TabStop& a, const core::TabStop& b) { return a.pos < b.pos; });
                });
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
        const bool drawn = m_draw.w * m_vp.scale() >= 4 && m_draw.h * m_vp.scale() >= 4;
        // s014: a Text-tool CLICK inside the margins makes a frame filling the
        // page's column there.
        if (!drawn && m_tool == Tool::Text) {
            for (const auto& p : m_placed) {
                if (p.id != m_draw_page) continue;
                const Rect mr = core::margin_rect(m_doc, p.id);
                const double lx = m_dx0 - p.r.x, ly = m_dy0 - p.r.y;
                if (mr.w <= 0 || ly < mr.y || ly > mr.b()) break;
                for (const auto& c : core::page_columns(m_doc, p.id))
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
                    static const core::FrameKind kinds[] = {core::FrameKind::Text, core::FrameKind::Text,
                                                            core::FrameKind::Image, core::FrameKind::Shape};
                    // s014: a text frame drawn across the margins takes the page's columns.
                    const Rect own{m_draw.x - p.r.x, m_draw.y - p.r.y, m_draw.w, m_draw.h};
                    const Rect mr = core::margin_rect(m_doc, p.id);
                    auto [cols, gut] = core::page_column_setup(m_doc, p.id);
                    if (!(mr.w > 0 && std::abs(own.x - mr.x) < 1 && std::abs(own.r() - mr.r()) < 1)) cols = 1;
                    const auto id = core::doc::add_frame(m_journal, m_doc, p.id, kinds[static_cast<int>(m_tool)],
                                                         own, cols, gut);
                    if (!id.empty()) {
                        relayout();
                        pick({id});
                        m_sig_edited.emit();
                        // s010: a new text frame is ready to type in (as InDesign).
                        if (m_tool == Tool::Text) begin_edit(id, {0, 0});
                    }
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
    if ((m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::Guide || m_drag == Drag::Gutter) && m_begun) {
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
    if (kv == GDK_KEY_Escape) {
        if (!m_thread_from.empty()) { m_thread_from.clear(); update_cursor(); queue_draw(); return true; }
        if (m_drag != Drag::None) { cancel_drag(); return true; }
        if (!m_load.empty()) { m_load.clear(); update_cursor(); queue_draw(); return true; }
        if (!m_sel.empty()) { pick({}); return true; }
        return false;
    }
    const bool alt = (st & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK;
    if (!ctrl && !alt && m_drag == Drag::None) {   // s008: tools by letter (as InDesign), Delete
        switch (kv) {
        case GDK_KEY_v: case GDK_KEY_V: set_tool(Tool::Select); m_sig_tool.emit(); return true;
        case GDK_KEY_t: case GDK_KEY_T: set_tool(Tool::Text);   m_sig_tool.emit(); return true;
        case GDK_KEY_f: case GDK_KEY_F: set_tool(Tool::Image);  m_sig_tool.emit(); return true;
        case GDK_KEY_m: case GDK_KEY_M: set_tool(Tool::Shape);  m_sig_tool.emit(); return true;
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

void Canvas::on_motion(double x, double y) {
    m_mx = x;
    m_my = y;
    m_in = true;
    m_sig_view.emit();
    if (!m_load.empty()) queue_draw();   // the file's name rides with the pointer
    if (m_drag != Drag::None) return;
    double dx, dy;
    m_vp.to_doc(x, y, dx, dy);
    const auto hg = m_show_guides ? core::hit_guide(m_doc, m_placed, dx, dy, 4 / m_vp.scale()) : std::string{};
    if (hg != m_hover_guide) { m_hover_guide = hg; queue_draw(); }
    const auto h = core::hit_frame(m_doc, m_placed, dx, dy, 3 / m_vp.scale());
    if (h != m_hover) { m_hover = h; queue_draw(); }
    m_hover_gutter = gutter_hit(x, y);
    update_cursor();
}

void Canvas::set_tool(Tool t) {
    end_edit();
    m_tool = t;
    m_thread_from.clear();
    m_load.clear();
    update_cursor();
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

core::TextPos Canvas::hit_story(const std::string& frame, double lx, double ly) {
    const Rect lr = core::frame_in_layout(m_doc, m_placed, frame);
    const auto& flow = m_flows.get(m_doc, m_edit_story.empty() ? m_doc.as<core::Frame>(frame)->story : m_edit_story);
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
    const auto& flow = m_flows.get(m_doc, m_edit_story);
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

void Canvas::edit_insert(const std::string& text, const std::string& kind, const std::string& label) {
    if (!edit_story()) return;
    if (kind.empty() || kind != m_burst_kind) { ++m_burst; m_burst_kind = kind; }
    const std::string key = kind.empty() ? std::string{} : kind + ":" + m_edit_story + ":" + std::to_string(m_burst);
    m_caret = core::doc::edit_text(m_journal, m_doc, m_edit_story, m_anchor, m_caret, text, label, key);
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
    const auto& flow = m_flows.get(m_doc, m_edit_story);
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
            get_clipboard()->set_text(core::text_between(*story, lo, hi));
            if (kv == GDK_KEY_x || kv == GDK_KEY_X) edit_insert("", "", "Cut");
            return true;
        case GDK_KEY_v: case GDK_KEY_V:
            get_clipboard()->read_text_async([this](const Glib::RefPtr<Gio::AsyncResult>& r) {
                try {
                    const auto t = get_clipboard()->read_text_finish(r);
                    if (editing() && !t.empty()) edit_insert(t.raw(), "", "Paste");
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
    const auto* ps = m_doc.as<core::Style>(para.style);
    if (!ps) return t;
    const auto& flow = const_cast<layout::FlowCache&>(m_flows).get(m_doc, m_edit_story);
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
    t.screen = {sr.x - 6, sr.y - 30, sr.w + 12, 22};
    t.style = para.style;
    return t;
}

int Canvas::tab_hit(const TabStrip& t, double x) const {
    const auto* ps = m_doc.as<core::Style>(t.style);
    if (!ps) return -1;
    for (int i = 0; i < static_cast<int>(ps->tabs.size()); ++i)
        if (std::abs(t.x0 + ps->tabs[static_cast<std::size_t>(i)].pos * t.scale - x) <= 6) return i;
    return -1;
}

void Canvas::draw_tab_strip(const Cairo::RefPtr<Cairo::Context>& cr) {
    const auto t = tab_strip();
    if (!t.ok) return;
    const auto* ps = m_doc.as<core::Style>(t.style);
    const auto& r = t.screen;
    cr->save();
    // The strip: a card, ticks every 6 pt (longer each half inch), inches marked.
    cr->set_source_rgba(1, 1, 1, 0.96);
    cr->rectangle(r.x, r.y, r.w, r.h);
    cr->fill();
    cr->set_source_rgba(0.45, 0.55, 0.7, 0.8);
    cr->set_line_width(1);
    cr->rectangle(std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w) - 1, std::round(r.h) - 1);
    cr->stroke();
    cr->set_source_rgba(0.2, 0.25, 0.35, 0.55);
    for (double p = 0; p <= t.width + 0.01; p += 6) {
        const double x = std::round(t.x0 + p * t.scale) + 0.5;
        const bool inch = std::fmod(p, 72) < 0.01, half = std::fmod(p, 36) < 0.01;
        if (6 * t.scale < 3 && !half) continue;
        cr->move_to(x, r.b() - 1);
        cr->line_to(x, r.b() - (inch ? 9 : half ? 6 : 3));
    }
    cr->stroke();
    // The stops: a mark at each, its kind in a letter, dots for a leader.
    std::vector<core::TabStop> tabs = ps ? ps->tabs : std::vector<core::TabStop>{};
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
        cr->move_to(x - 3, r.y + 1);
        lay->show_in_cairo_context(cr);
    }
    cr->restore();
}

void Canvas::draw_edit(const Cairo::RefPtr<Cairo::Context>& cr) {
    const auto& flow = m_flows.get(m_doc, m_edit_story);
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
    cr->restore();
}

// ── s009: placing images ────────────────────────────────────────────────────
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
    const core::ImageFile file = m_load.current();
    std::string made;
    if (!drawn) {
        const auto hit = core::hit_frame(m_doc, m_placed, lx, ly, 3 / m_vp.scale());
        if (const auto* f = m_doc.as<core::Frame>(hit); f && f->kind == core::FrameKind::Image) {
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
    if (!m_thread_from.empty() || !m_load.empty()) { set_cursor(Gdk::Cursor::create("copy")); return; }   // loaded
    if (!out_port_hit(m_mx, m_my).empty() && !editing()) { set_cursor(Gdk::Cursor::create("pointer")); return; }
    if (editing() || m_tool == Tool::Text) {   // s010: the I-beam over text you can type in
        const auto* f = m_doc.as<core::Frame>(m_hover);
        if (f && f->kind == core::FrameKind::Text && !f->story.empty() && (!editing() || f->story == m_edit_story)) {
            set_cursor(Gdk::Cursor::create("text"));
            return;
        }
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
        {"insert-text-symbolic",          "canvas_view.tool.text",   "Text frame  (T) \u2014 drag on a page", Canvas::Tool::Text},
        {"image-x-generic-symbolic",      "canvas_view.tool.image",  "Image frame  (F) \u2014 drag on a page", Canvas::Tool::Image},
        {"composix-tool-shape-symbolic",  "canvas_view.tool.shape",  "Shape  (M) \u2014 drag on a page; Shift for a square", Canvas::Tool::Shape},
    };
    Gtk::ToggleButton* first = nullptr;
    for (const auto& t : tools) {
        auto b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_icon_name(t.icon);
        b->set_name(t.name);
        b->set_tooltip_text(t.tip);
        b->add_css_class("cx-tool");
        if (first) b->set_group(*first); else first = b;
        const auto tool = t.tool;
        b->signal_toggled().connect([this, b, tool] { if (b->get_active()) m_canvas.set_tool(tool); });
        m_tools.append(*b);
        m_tool_buttons.push_back(b);
    }
    first->set_active(true);
    m_canvas.signal_tool().connect([this] {
        const int i = static_cast<int>(m_canvas.tool());
        if (i < static_cast<int>(m_tool_buttons.size())) m_tool_buttons[static_cast<std::size_t>(i)]->set_active(true);
    });
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

CanvasView::~CanvasView() = default;

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
