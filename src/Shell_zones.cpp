#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gdkmm/cursor.h>
#include <glibmm/main.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the header, the
// three panes, the empty main view, and the menu model.

namespace composix {

void Shell::build_shell() {  // zone: header + panes
    set_title("Composix");
    set_default_size(1280, 820);
    add_css_class("cx-window");

    // ── header ──────────────────────────────────────────────────────────────
    auto header = Gtk::make_managed<Gtk::HeaderBar>();
    header->set_name("shell.header");
    header->add_css_class("cx-header");

    m_logo_button.set_icon_name("composix-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About Composix");
    m_logo_button.set_action_name("win.about");
    m_logo_button.add_css_class("cx-logo");
    header->pack_start(m_logo_button);

    m_nav_toggle.set_icon_name("sidebar-show-symbolic");
    m_nav_toggle.set_tooltip_text("Show / hide the navigator  (Ctrl+Alt+1)");
    m_nav_toggle.set_action_name("win.toggle-nav");
    m_nav_toggle.add_css_class("cx-chrome-button");
    header->pack_start(m_nav_toggle);

    // A two-line title, the document's name over its kind (web / Mac roots).
    m_title.set_text("Composix");
    m_title.add_css_class("cx-doc-title");
    m_subtitle.add_css_class("cx-doc-subtitle");
    m_title_box.set_valign(Gtk::Align::CENTER);
    m_title_box.append(m_title);
    m_title_box.append(m_subtitle);
    header->set_title_widget(m_title_box);

    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    m_menu_button.set_tooltip_text("Main menu");
    m_menu_button.add_css_class("cx-chrome-button");
    header->pack_end(m_menu_button);

    m_insp_toggle.set_icon_name("sidebar-show-right-symbolic");
    m_insp_toggle.set_tooltip_text("Show / hide the inspector  (Ctrl+Alt+2)");
    m_insp_toggle.set_action_name("win.toggle-inspector");
    m_insp_toggle.add_css_class("cx-chrome-button");
    header->pack_end(m_insp_toggle);
    set_titlebar(*header);

    // ── body: nav | (main | inspector) ─────────────────────────────────────
    m_main.add_css_class("cx-main");
    m_main.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_main.set_transition_duration(120);
    build_welcome();
    m_main.add(m_welcome, "welcome");
    m_main.add(m_tiles, "tiles");
    m_main.add(m_item, "item");
    m_main.add(m_canvas_view, "canvas");   // s005: pages and masters open here
    m_main.set_visible_child("welcome");

    // ── the inspector: a sliding panel, its handle, and the pull tab ───────
    // (s003, as Samson s013/s014.) The handle on the panel's inner edge drags
    // to size it, drags nearly shut to close it, and -- shut -- waits at the
    // window's edge to be pulled out. The PULL TAB floats over the main view's
    // right edge, halfway down, quiet until the pointer finds it: drag it to
    // size, click it to push the panel back or pull it out.
    m_insp_rev.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
    m_insp_rev.set_transition_duration(200);
    m_insp_rev.set_child(m_inspector);
    m_insp_rev.set_hexpand(false);
    m_insp_handle = std::make_unique<views::PanelHandle>(
        "shell.inspector.handle", views::PanelHandle::Side::Right, m_insp_rev, 240, 560);
    m_insp_handle->set_width(290);
    m_insp_handle->changed = [this] { sync_inspector(); };

    m_insp_tab.add_css_class("cx-pull-tab");
    m_insp_tab.set_halign(Gtk::Align::END);
    m_insp_tab.set_valign(Gtk::Align::CENTER);
    m_insp_tab_glyph.set_hexpand(true);
    m_insp_tab_glyph.set_vexpand(true);
    m_insp_tab.append(m_insp_tab_glyph);
    m_insp_tab.set_cursor(Gdk::Cursor::create("col-resize"));
    m_insp_handle->add_drag(m_insp_tab, true);
    // One "lit" for the edge and the tab: pointing at either lights the tab,
    // and it fades a moment after the pointer leaves both (no flicker between).
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_insp_tab),
                           static_cast<Gtk::Widget*>(m_insp_handle.get())}) {
        auto motion = Gtk::EventControllerMotion::create();
        motion->signal_enter().connect([this](double, double) { tab_hover(true); });
        motion->signal_leave().connect([this] { tab_hover(false); });
        w->add_controller(motion);
    }

    m_main.set_hexpand(true);
    m_main.set_vexpand(true);
    m_main_over.set_child(m_main);
    m_main_over.add_overlay(m_insp_tab);
    m_main_over.set_hexpand(true);
    m_main_over.set_vexpand(true);
    m_right.append(m_main_over);
    m_right.append(*m_insp_handle);
    m_right.append(m_insp_rev);
    m_insp_handle->set_open(true);

    m_panes.set_start_child(m_nav);
    m_panes.set_end_child(m_right);
    m_panes.set_resize_start_child(false);
    m_panes.set_resize_end_child(true);
    m_panes.set_shrink_start_child(false);
    m_panes.set_shrink_end_child(false);
    m_panes.set_position(250);
    m_panes.add_css_class("cx-panes");
    set_child(m_panes);
}

void Shell::build_welcome() {  // zone: the main view when nothing is picked
    m_welcome.add_css_class("cx-welcome");
    m_welcome.set_valign(Gtk::Align::CENTER);
    m_welcome.set_halign(Gtk::Align::CENTER);
    auto icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("composix-logo-symbolic");
    icon->set_pixel_size(64);
    icon->add_css_class("cx-welcome-icon");
    auto title = Gtk::make_managed<Gtk::Label>("Pick a section");
    title->add_css_class("cx-welcome-title");
    auto body = Gtk::make_managed<Gtk::Label>(
        "Choose a section in the navigator to see everything in it as tiles,\n"
        "or open its items to work on them.");
    body->set_justify(Gtk::Justification::CENTER);
    body->add_css_class("cx-welcome-body");
    m_welcome.append(*icon);
    m_welcome.append(*title);
    m_welcome.append(*body);
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto file = Gio::Menu::create();
    file->append("Open Project…", "win.open");
    m_recents_menu = Gio::Menu::create();
    file->append_submenu("Open Recent", m_recents_menu);
    file->append("Save", "win.save");
    file->append("Save As\u2026", "win.save-as");
    menu->append_section(file);

    m_edit_menu = Gio::Menu::create();
    m_edit_menu->append("Undo", "win.undo");
    m_edit_menu->append("Redo", "win.redo");
    menu->append_section(m_edit_menu);

    auto view = Gio::Menu::create();
    view->append("Navigator", "win.toggle-nav");
    view->append("Inspector", "win.toggle-inspector");
    auto canvas = Gio::Menu::create();   // s006
    canvas->append("Rulers", "win.toggle-rulers");
    canvas->append("Guides", "win.toggle-guides");
    canvas->append("Snap to Guides & Edges", "win.toggle-snap");
    view->append_section("", canvas);
    menu->append_section(view);

    auto diag = Gio::Menu::create();
    diag->append("Dump registry", "win.dump-registry");
    diag->append("Keyboard shortcuts", "win.shortcuts");
    menu->append_section(diag);

    auto app = Gio::Menu::create();
    app->append("About Composix", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace composix

namespace composix {

void Shell::tab_hover(bool in) {  // helper: the pull tab's shared hover
    m_tab_fade.disconnect();
    if (in) { m_insp_tab.add_css_class("cx-lit"); return; }
    m_tab_fade = Glib::signal_timeout().connect(
        [this] { m_insp_tab.remove_css_class("cx-lit"); return false; }, 250);
}

}  // namespace composix
