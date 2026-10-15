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
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_nav_rev("shell.nav.reveal"),
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
    // The recents path BEFORE bind_actions, which loads the list from it.
    // (Cairn set it after, so the list loaded from "" and came up empty on
    // every launch -- found in s004.)
    m_recents_file = recents_file();
    bind_actions();    // bindings
    bind_views();      // bindings

    sync_inspector();  // the action exists now: toggle + View check follow the panel
    sync_nav();

    rebuild_recents_menu();

    // s004: the document. Until a project is opened, the built-in sample
    // (loaded, not a step). The views see only its projection.
    core::load_sample(m_doc);
    m_outline = project_now();
    m_nav.set_outline(&m_outline);
    update_title();
    m_nav.set_open(core::Segment::Pages, true);
    m_nav.select_section(core::Segment::Pages);   // land on the page tiles

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} record(s), {} outline item(s)", m_doc.size(), m_outline.size());
}

}  // namespace composix
