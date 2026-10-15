#include "views/PanelHandle.hpp"

#include <gdkmm/cursor.h>
#include <gtkmm/gesturedrag.h>

#include <algorithm>
#include <cstdlib>

namespace composix::views {

PanelHandle::PanelHandle(std::string_view name, Side side, Gtk::Revealer& panel, int min_w, int max_w)
    : widgets::Box(name, Gtk::Orientation::VERTICAL), m_side(side), m_panel(panel), m_min(min_w), m_max(max_w) {
    add_css_class("cx-panel-handle");
    add_css_class(side == Side::Left ? "cx-left" : "cx-right");
    set_cursor(Gdk::Cursor::create("col-resize"));
    set_tooltip_text(side == Side::Left ? "Drag to size the navigator"
                                        : "Drag to size the inspector");
    add_drag(*this, false);
}

void PanelHandle::add_drag(Gtk::Widget& w, bool click_toggles) {
    // The handle (and the tab) move as the panel grows, so GTK's offsets (in
    // the widget's own coordinates) would undercount; measure in the parent's,
    // which does not move.
    auto drag = Gtk::GestureDrag::create();
    auto in_parent = [this, &w](Gtk::GestureDrag& g) {
        double sx = 0, sy = 0, ox = 0, oy = 0, px = 0, py = 0;
        g.get_start_point(sx, sy);
        g.get_offset(ox, oy);
        if (auto* body = get_parent()) w.translate_coordinates(*body, sx + ox, sy + oy, px, py);
        return px;
    };
    drag->signal_drag_begin().connect([this, d = drag.get(), in_parent](double, double) {
        m_w0 = m_width;
        m_open0 = open();
        m_moved = false;
        m_x0 = in_parent(*d);
    });
    drag->signal_drag_update().connect([this, dg = drag.get(), in_parent](double, double) {
        const double dx = in_parent(*dg) - m_x0;
        const int d = static_cast<int>(m_side == Side::Left ? dx : -dx);
        if (std::abs(d) < 4 && !m_moved) return;                         // a click, not a drag (yet)
        m_moved = true;
        if (m_open0) {
            const int nw = m_w0 + d;
            if (nw < m_min - 60) { if (open()) set_open(false); }       // dragged shut
            else { if (!open()) set_open(true); set_width(nw); }
        } else if (d > 24 && !open()) {                                  // pulled out
            set_open(true);
        }
    });
    drag->signal_drag_end().connect([this, click_toggles](double, double) {
        // Judged in the parent's coordinates (m_moved): the tab rides the
        // panel's edge, so in its OWN coordinates a real drag looks like none.
        const bool click = !m_moved;
        if (click && (click_toggles || !m_open0)) set_open(!m_open0);   // the tab toggles; a hidden edge opens
        else if (changed) changed();
    });
    w.add_controller(drag);
}

void PanelHandle::set_width(int w) {
    m_width = std::clamp(w, m_min, m_max);
    if (auto* c = m_panel.get_child()) c->set_size_request(m_width, -1);
}

void PanelHandle::set_open(bool on) {
    m_panel.set_reveal_child(on);
    if (on) { add_css_class("cx-open"); remove_css_class("cx-shut"); }
    else    { add_css_class("cx-shut"); remove_css_class("cx-open"); }
    if (changed) changed();
}

}  // namespace composix::views
