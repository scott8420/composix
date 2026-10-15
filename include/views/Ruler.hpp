#pragma once
// views::Ruler -- a ruler beside the canvas (s006).
//
// Reads the canvas's viewport every time it paints, so it follows zoom and pan
// without a state copy (Curvz pushed a RulerState struct and re-derived the
// transform; one source is enough). Zero is the first opened page's top-left.
// Inches, with ticks that thin out as you zoom out. A marker follows the
// pointer. Drag OUT of a ruler onto a page to pull a guide: the top ruler makes
// horizontal guides, the left one vertical guides (one step each).
#include "widgets/Widgets.hpp"

#include <gtkmm/gesturedrag.h>

namespace composix::views {

class Canvas;

class Ruler : public widgets::DrawingArea {
public:
    Ruler(std::string_view name, bool horizontal, Canvas& canvas);

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);

    bool    m_horizontal;
    Canvas& m_canvas;
    Glib::RefPtr<Gtk::GestureDrag> m_drag;
    double  m_x0 = 0, m_y0 = 0;
};

}  // namespace composix::views
