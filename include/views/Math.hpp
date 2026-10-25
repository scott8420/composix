#pragma once
// s349 (Scott: "we need to be able to do math in the number fields"): a
// spin button that reads what's typed as a sum -- "12+6", "3p6*2", "1in -
// 6pt", "+6" (on the value it shows), "/2" -- through core::parse_length.
// `unit_pts`: the points one of its numbers stands for (1: points or a
// plain number; 72: inches). What doesn't read falls to GTK's own reading.
#include "core/Doc.hpp"

#include <gtkmm/spinbutton.h>

#include <string>

namespace composix::views {

inline void allow_math(Gtk::SpinButton& sb, double unit_pts = 1) {
    sb.signal_input().connect([&sb, unit_pts](double& v) -> int {
        std::string t = sb.get_text();
        while (!t.empty() && (t.back() == '%' || t.back() == ' ')) t.pop_back();   // a percentage's sign
        const auto r = core::parse_length(t, unit_pts, sb.get_value() * unit_pts);
        if (!r) return 0;   // GTK reads it (or says it can't)
        v = *r / unit_pts;
        return 1;
    }, false);
}

}  // namespace composix::views
