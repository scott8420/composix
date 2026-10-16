#pragma once
// s068 (Scott): the way back up a level -- a back arrow at the start of a
// view's title row ("< Front matter"). It runs win.go-up, so it is enabled
// exactly when the window's action is; the shell names where it goes.
#include <gtkmm/button.h>

#include <string>

namespace composix::views {

inline Gtk::Button* back_button(const std::string& name) {
    auto b = Gtk::make_managed<Gtk::Button>();
    b->set_name(name);
    b->set_icon_name("go-previous-symbolic");
    b->set_action_name("win.go-up");
    b->add_css_class("cx-chrome-button");
    b->add_css_class("cx-back");
    b->set_valign(Gtk::Align::CENTER);
    b->set_tooltip_text("Back up a level  (Alt+Up)");
    return b;
}

}  // namespace composix::views
