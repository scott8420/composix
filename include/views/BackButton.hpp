#pragma once
// s068 (Scott): back and forward at the start of a view's title row
// ("< > Front matter"). Back = up a level (win.go-up); Forward = back down
// into what you just came up from (win.go-forward). They run the window's
// actions, so each is enabled exactly when its action is; the shell names
// where they go (set_nav).
#include <gtkmm/box.h>
#include <gtkmm/button.h>

#include <string>

namespace composix::views {

struct NavButtons {
    Gtk::Box*    box  = nullptr;
    Gtk::Button* back = nullptr;
    Gtk::Button* fwd  = nullptr;

    // "" = nowhere (the button is greyed by its action anyway).
    void set(const std::string& back_to, const std::string& fwd_to) {
        if (back) back->set_tooltip_text(back_to.empty() ? "Back  (Alt+Left)" : "Back to " + back_to + "  (Alt+Left)");
        if (fwd)  fwd->set_tooltip_text(fwd_to.empty() ? "Forward  (Alt+Right)" : "Forward to " + fwd_to + "  (Alt+Right)");
    }
};

inline NavButtons nav_buttons(const std::string& prefix) {
    NavButtons n;
    n.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    n.box->set_name(prefix + ".nav");
    n.box->add_css_class("linked");
    n.box->set_valign(Gtk::Align::CENTER);
    const auto make = [&](const char* name, const char* icon, const char* action) {
        auto b = Gtk::make_managed<Gtk::Button>();
        b->set_name(prefix + "." + name);
        b->set_icon_name(icon);
        b->set_action_name(action);
        b->add_css_class("cx-chrome-button");
        b->add_css_class("cx-back");
        n.box->append(*b);
        return b;
    };
    n.back = make("back", "go-previous-symbolic", "win.go-up");
    n.fwd  = make("forward", "go-next-symbolic", "win.go-forward");
    n.set("", "");
    return n;
}

}  // namespace composix::views
