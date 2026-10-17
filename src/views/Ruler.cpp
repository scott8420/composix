#include "views/Ruler.hpp"
#include "views/Canvas.hpp"

#include <pangomm/layout.h>

#include <cmath>
#include <vector>
#include <cstdio>

namespace composix::views {

Ruler::Ruler(std::string_view name, bool horizontal, Canvas& canvas)
    : widgets::DrawingArea(name), m_horizontal(horizontal), m_canvas(canvas) {
    add_css_class("cx-ruler");
    if (horizontal) { set_content_height(20); set_hexpand(true); }
    else            { set_content_width(20);  set_vexpand(true); }
    set_draw_func(sigc::mem_fun(*this, &Ruler::draw));
    set_tooltip_text(horizontal ? "Drag down onto a page for a horizontal guide"
                                : "Drag right onto a page for a vertical guide");

    // Pull a guide: the press is on the ruler, the drop is on the canvas --
    // translate into the canvas's coordinates and let it preview / drop.
    m_drag = Gtk::GestureDrag::create();
    m_drag->signal_drag_begin().connect([this](double x, double y) { m_x0 = x; m_y0 = y; });
    m_drag->signal_drag_update().connect([this](double dx, double dy) {
        double cx = 0, cy = 0;
        translate_coordinates(m_canvas, m_x0 + dx, m_y0 + dy, cx, cy);
        m_canvas.preview_guide(!m_horizontal, m_horizontal ? cy : cx);
    });
    m_drag->signal_drag_end().connect([this](double dx, double dy) {
        double cx = 0, cy = 0;
        translate_coordinates(m_canvas, m_x0 + dx, m_y0 + dy, cx, cy);
        m_canvas.drop_guide(!m_horizontal, cx, cy);
    });
    add_controller(m_drag);
}

void Ruler::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const auto& vp = m_canvas.viewport();
    const double s = vp.scale();
    if (s <= 0) return;
    const auto fg = get_color();
    const double len = m_horizontal ? w : h;
    // This ruler's offset from the canvas (the corner box / the other ruler).
    double offx = 0, offy = 0;
    translate_coordinates(m_canvas, 0, 0, offx, offy);
    const double origin = m_horizontal ? vp.ox() - offx : vp.oy() - offy;   // screen px of doc 0

    // Major step: the smallest of these (in the document's unit -- s073) at
    // least 56 px apart; each split into as many of its parts as fit.
    const auto units = m_canvas.doc().settings().units;
    const double upt = core::unit_pt(units);
    std::vector<double> steps;
    std::vector<int> parts;
    switch (units) {
    case core::Units::Millimetres: steps = {1, 2, 5, 10, 20, 50, 100, 200, 500}; parts = {10, 5, 2, 1}; break;
    case core::Units::Picas:       steps = {1, 2, 3, 6, 12, 24, 48, 96};        parts = {12, 6, 3, 1}; break;
    case core::Units::Points:      steps = {6, 12, 18, 36, 72, 144, 288, 576};  parts = {12, 6, 2, 1}; break;
    default:                       steps = {0.125, 0.25, 0.5, 1, 2, 4, 6, 12, 24}; parts = {8, 4, 2, 1}; break;
    }
    double major = steps.back();
    for (double st : steps) if (st * upt * s >= 56) { major = st; break; }
    const double px_major = major * upt * s;
    int subs = 1;
    for (int p : parts) if (px_major / p >= 5) { subs = p; break; }

    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.55);
    cr->set_line_width(1);
    const double first = std::floor((0 - origin) / px_major) - 1;
    for (double k = first; origin + k * px_major < len + px_major; k += 1) {
        for (int i = 0; i < subs; ++i) {
            const double at = std::round(origin + (k + static_cast<double>(i) / subs) * px_major) + 0.5;
            if (at < 0 || at > len) continue;
            const double tick = i == 0 ? 20 : (subs % 2 == 0 && i == subs / 2 ? 9 : (subs >= 4 && subs % 4 == 0 && i % (subs / 4) == 0 ? 6 : 3));
            if (m_horizontal) { cr->move_to(at, 20); cr->line_to(at, 20 - tick); }
            else              { cr->move_to(20, at); cr->line_to(20 - tick, at); }
        }
    }
    cr->stroke();

    // Labels at the majors.
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.75);
    for (double k = first; origin + k * px_major < len + px_major; k += 1) {
        const double at = origin + k * px_major;
        if (at < -40 || at > len) continue;
        char buf[16];
        const double v = k * major;
        if (std::abs(v - std::round(v)) < 1e-9) std::snprintf(buf, sizeof buf, "%.0f", v);
        else std::snprintf(buf, sizeof buf, "%g", v);
        auto lay = create_pango_layout(buf);
        lay->set_font_description(Pango::FontDescription("Sans 7"));
        if (m_horizontal) {
            cr->move_to(at + 3, 1);
            lay->show_in_cairo_context(cr);
        } else {
            cr->save();
            cr->move_to(1, at + 3);
            cr->rotate(M_PI / 2);
            cr->rel_move_to(0, -12);
            lay->show_in_cairo_context(cr);
            cr->restore();
        }
    }

    // The pointer's marker.
    double px, py;
    m_canvas.pointer(px, py);
    if (px >= 0) {
        const double at = std::round((m_horizontal ? px - offx : py - offy)) + 0.5;
        cr->set_source_rgba(0.21, 0.52, 0.89, 0.9);
        if (m_horizontal) { cr->move_to(at, 0); cr->line_to(at, 20); }
        else              { cr->move_to(0, at); cr->line_to(20, at); }
        cr->stroke();
    }
    // The edge against the canvas.
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.15);
    if (m_horizontal) { cr->move_to(0, h - 0.5); cr->line_to(w, h - 0.5); }
    else              { cr->move_to(w - 0.5, 0); cr->line_to(w - 0.5, h); }
    cr->stroke();
}

}  // namespace composix::views
