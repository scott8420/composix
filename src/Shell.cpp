#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto
#include "Log.hpp"

// Shell.cpp -- GLUE. The ctor and the ordered build_ui() pass live here; every
// other method body lives in a category file (see the index in Shell.hpp).

namespace composix {

Shell::Shell()
    : m_logo_button("shell.logo_button"),
      m_nav_toggle("shell.nav_toggle"),
      m_title_box("shell.title_box", Gtk::Orientation::VERTICAL),
      m_title("shell.title"),
      m_subtitle("shell.subtitle"),
      m_insp_toggle("shell.inspector_toggle"),
      m_menu_button("shell.menu_button"),
      m_panes("shell.panes", Gtk::Orientation::HORIZONTAL),
      m_right("shell.right", Gtk::Orientation::HORIZONTAL),
      m_main_over("shell.main_over"),
      m_insp_rev("shell.inspector.reveal"),
      m_insp_tab("shell.inspector.tab", Gtk::Orientation::VERTICAL),
      m_main("shell.main"),
      m_welcome("shell.welcome", Gtk::Orientation::VERTICAL, 8) {
    set_name("shell.window");
}

Shell::~Shell() = default;

// construct the shell -> bind actions -> bind the views -> load -> paint.
void Shell::build_ui() {
    build_shell();     // zone
    bind_actions();    // bindings
    bind_views();      // bindings

    sync_inspector();  // the action exists now: toggle + View check follow the panel

    m_recents_file = recents_file();
    rebuild_recents_menu();

    // s002: the fixture, through the real seam (core::Outline).
    m_outline = core::fixture_outline();
    m_nav.set_outline(&m_outline);
    m_nav.set_open(core::Section::Pages, true);
    m_nav.select_section(core::Section::Pages);   // land on the page tiles

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} outline item(s)", m_outline.size());
}

}  // namespace composix
