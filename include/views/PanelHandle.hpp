#pragma once
// views::PanelHandle -- a side panel's handle (s003, ported from Samson s013/
// s014, itself after Folio). Lives on the panel's inner edge: drag to size the
// panel, drag it nearly shut and it closes; with the panel shut it waits at
// the window's edge to be pulled out again. add_drag() gives the same
// behaviour to another widget -- the inspector's PULL TAB, where a click also
// toggles. The panel slides (Gtk::Revealer).
#include "widgets/Widgets.hpp"

#include <gtkmm/revealer.h>

#include <functional>
#include <string_view>

namespace composix::views {

class PanelHandle : public widgets::Box {
public:
    enum class Side { Left, Right };   // Right: the inspector's (dragging left widens)
    PanelHandle(std::string_view name, Side side, Gtk::Revealer& panel, int min_w, int max_w);

    int  width() const { return m_width; }
    void set_width(int w);                 // clamped
    bool open() const { return m_panel.get_reveal_child(); }
    void set_open(bool on);
    std::function<void()> changed;         // opened, closed or resized
    // The same drag on another widget: drag sizes, drag nearly shut closes;
    // with click_toggles a CLICK opens or shuts.
    void add_drag(Gtk::Widget& w, bool click_toggles);

private:
    Side           m_side;
    Gtk::Revealer& m_panel;
    int            m_min, m_max;
    int            m_width = 0;
    int            m_w0 = 0;
    bool           m_open0 = false;
    double         m_x0 = 0;               // the drag's start, in the parent's coordinates
    bool           m_moved = false;        // this drag has moved (a click if not)
};

}  // namespace composix::views
