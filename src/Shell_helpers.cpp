#include "Shell.hpp"
#include "core/Recents.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, and handlers: the recents seam (path resolution, persist, and the
// two-surface repaint).

namespace composix {

std::string Shell::recents_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md).
    return (std::filesystem::path(Glib::get_user_data_dir()) / "composix" / "recent.json")
        .string();
}

void Shell::note_recent(const std::string& path) {  // helper: push + persist + repaint
    if (path.empty()) return;
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();

    // Display name = folder basename; identical basenames get their parent dir
    // appended so they're distinguishable (Curvz's disambiguation).
    std::vector<std::string> names(m_recents.size());
    for (std::size_t i = 0; i < m_recents.size(); ++i)
        names[i] = std::filesystem::path(m_recents[i]).filename().string();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j]) {
                const std::string pi =
                    std::filesystem::path(m_recents[i]).parent_path().filename().string();
                const std::string pj =
                    std::filesystem::path(m_recents[j]).parent_path().filename().string();
                if (!pi.empty()) names[i] += "  (" + pi + ")";
                if (!pj.empty()) names[j] += "  (" + pj + ")";
            }

    for (std::size_t i = 0; i < m_recents.size(); ++i) {
        auto item = Gio::MenuItem::create(names[i], "");
        item->set_action_and_target(
            "recents.open", Glib::Variant<Glib::ustring>::create(m_recents[i]));
        m_recents_menu->append_item(item);
    }

    auto clear_section = Gio::Menu::create();
    clear_section->append("Clear recent", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

std::vector<const core::Item*> Shell::resolve(const std::vector<std::string>& ids) const {  // helper
    std::vector<const core::Item*> out;
    for (const auto& id : ids)
        if (const auto* it = m_outline.find(id)) out.push_back(it);
    return out;
}

// Undo / Redo say what they would do, and grey when there is nothing: the
// actions (so buttons and menu items follow), the button tooltips, and the
// menu's labels -- "Undo Delete “Jade”" names the step before you take it.
void Shell::update_undo_ui() {  // helper
    if (m_act_undo) m_act_undo->set_enabled(m_journal.can_undo());
    if (m_act_redo) m_act_redo->set_enabled(m_journal.can_redo());
    const std::string u = m_journal.undo_label(), r = m_journal.redo_label();
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(u.empty() ? "Undo" : "Undo " + u, "win.undo");
        m_edit_menu->append(r.empty() ? "Redo" : "Redo " + r, "win.redo");
    }
}

// The header toggle, the View menu's check and the pull tab all follow the
// panel -- whichever of them (or the handle, or a drag) moved it.
void Shell::sync_inspector() {  // helper
    if (!m_insp_handle) return;
    const bool open = m_insp_handle->open();
    if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action("toggle-inspector")))
        a->change_state(open);
    m_insp_tab_glyph.set_text(open ? "\u203A" : "\u2039");   // > push back  /  < pull out
    m_insp_tab.set_tooltip_text(open ? "Drag to size the inspector; click to push it back  (Ctrl+Alt+2)"
                                     : "Click or drag to pull out the inspector  (Ctrl+Alt+2)");
    if (open) m_insp_tab.add_css_class("cx-open");
    else      m_insp_tab.remove_css_class("cx-open");
}

// Is the keyboard in something that types? Then Ctrl+Z is that box's own,
// whether or not it had anything to undo.
bool Shell::focus_is_text() const {  // helper
    for (const Gtk::Widget* w = get_focus(); w; w = w->get_parent())
        if (dynamic_cast<const Gtk::TextView*>(w) || dynamic_cast<const Gtk::Editable*>(w)) return true;
    return false;
}

}  // namespace composix
