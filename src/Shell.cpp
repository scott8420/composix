#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto
#include "Log.hpp"
#include "views/Thumbs.hpp"

#include <filesystem>

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
    m_settings_file = (std::filesystem::path(m_recents_file).parent_path() / "settings.json").string();   // s052
    m_settings = core::load_settings(m_settings_file);
    bind_actions();    // bindings
    bind_views();      // bindings

    sync_inspector();  // the action exists now: toggle + View check follow the panel
    sync_nav();
    restore_window();  // s057: as it was left

    rebuild_recents_menu();

    // s020: page and master tiles draw the page itself.
    views::set_page_painter([this](const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id, double x, double y,
                                   double w, double h, bool large) {
        return m_page_thumbs.paint(cr, id, x, y, w, h, large);
    });

    // s019: the last project worked on, if it is still there; else the
    // built-in sample (loaded, not a step). The views see only its projection.
    bool opened = false;
    if (!m_recents.empty() && std::filesystem::is_directory(m_recents.front())) {
        core::Doc last;
        std::string err;
        if (core::load_project(last, m_recents.front(), &err)) {
            adopt(last, m_recents.front());
            opened = true;
            if (auto lg = log::get(log::Area::Io)) lg->info("reopened '{}'", m_recents.front());
        } else if (auto lg = log::get(log::Area::Io)) {
            lg->warn("reopen '{}': {} -- the sample instead", m_recents.front(), err);
        }
    }
    if (!opened) {
        core::Doc sample;
        core::load_sample(sample);
        adopt(sample, "");
    }

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} record(s), {} outline item(s)", m_doc.size(), m_outline.size());
}

// s057: the window's size and the side panels, kept between runs.
void Shell::restore_window() {  // helper
    const auto& st = m_settings;
    set_default_size(core::setting_int(st, "window.width", 1280, 640, 10000),
                     core::setting_int(st, "window.height", 820, 480, 10000));
    if (core::setting_int(st, "window.maximized", 0, 0, 1)) maximize();
    if (m_nav_handle) {
        m_nav_handle->set_width(core::setting_int(st, "nav.width", 250, 0, 10000));
        m_nav_handle->set_open(core::setting_int(st, "nav.open", 1, 0, 1) == 1);
    }
    if (m_insp_handle) {
        m_insp_handle->set_width(core::setting_int(st, "inspector.width", 290, 0, 10000));
        m_insp_handle->set_open(core::setting_int(st, "inspector.open", 1, 0, 1) == 1);
    }
}

void Shell::save_window() {  // helper
    int w = 0, h = 0;
    get_default_size(w, h);
    if (!is_maximized() && w > 0 && h > 0) {   // a maximized window keeps the size it comes back to
        m_settings["window.width"] = std::to_string(w);
        m_settings["window.height"] = std::to_string(h);
    }
    m_settings["window.maximized"] = is_maximized() ? "1" : "0";
    if (m_nav_handle) {
        m_settings["nav.width"] = std::to_string(m_nav_handle->width());
        m_settings["nav.open"] = m_nav_handle->open() ? "1" : "0";
    }
    if (m_insp_handle) {
        m_settings["inspector.width"] = std::to_string(m_insp_handle->width());
        m_settings["inspector.open"] = m_insp_handle->open() ? "1" : "0";
    }
    core::save_settings(m_settings_file, m_settings);
}

}  // namespace composix
