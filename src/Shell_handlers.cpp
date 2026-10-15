#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>

#include <filesystem>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace composix {

void Shell::on_pick_folder() {  // handler: folder chooser -> remember it
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Pick a folder to remember");
    dialog->select_folder(*this,
        [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto dir = dialog->select_folder_finish(result);
                if (!dir) return;
                const std::string folder = dir->get_path();
                if (!folder.empty()) note_recent(folder);
            } catch (const Glib::Error& e) {
                if (auto lg = log::get(log::Area::Shell))
                    lg->info("pick-folder: dialog dismissed ({})", e.what());
            }
        });
}

void Shell::on_recent_open(const std::string& path) {  // handler: open a recent
    if (path.empty()) return;
    // A real app would load the project here. The seed just proves the routing:
    // a vanished folder self-prunes; a live one is promoted back to the front.
    if (!std::filesystem::exists(path)) {
        core::recents_remove(m_recents, path);
        core::save_recents(m_recents_file, m_recents);
        rebuild_recents_menu();
        if (auto lg = log::get(log::Area::Recents))
            lg->warn("open '{}': gone -- pruned", path);
        return;
    }
    if (auto lg = log::get(log::Area::Recents)) lg->info("open '{}'", path);
    note_recent(path);   // move-to-front
}

void Shell::on_recent_clear() {  // handler: empty the recents list
    m_recents.clear();
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::on_dump_registry() {  // handler: print the live widget tree
    // The debuggability payoff, on demand. Same call works from a gdb prompt.
    registry::dump();
}

void Shell::on_shortcuts() {  // handler: open the keyboard reference
    // Same lifetime stone as About: build once, re-present. The window's CONTENT
    // is not written here -- it renders by walking core::shortcut_registry(),
    // the same list bind_accelerators() wires from, so the two can't drift.
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {  // handler: open the About window (dialog-lifetime exemplar)
    // Lazily build once, then re-present. Hide-on-close singleton (CANON:
    // "Lifetime shape is design") -- the X hides it, so one instance lives for
    // the session and its named children never re-register.
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

// ─────────────────────────────────────────────────────────────────────────────
// s002 -- the navigator / tiles / inspector routing.
// ─────────────────────────────────────────────────────────────────────────────

// A SECTION shows its whole category as tiles; ITEMS open. The inspector
// follows whichever: the category's summary, or the items' shared fields.
void Shell::on_nav_pick(const views::Pick& p) {  // handler
    if (p.section) {
        if (m_section != p.section) {
            m_tiles.show_section(m_outline, *p.section);
            m_section = p.section;
        }
        m_main.set_visible_child("tiles");
        m_inspector.show_section(m_outline, *p.section);
        return;
    }
    if (!p.items.empty()) {
        const auto items = resolve(p.items);
        m_item.show_items(items);
        m_main.set_visible_child("item");
        m_inspector.show_items(items);
        m_section.reset();   // the tile view rebuilds on the next section pick
        return;
    }
    m_main.set_visible_child("welcome");
    m_inspector.clear();
    m_section.reset();
}

void Shell::on_tiles_selection(const std::vector<std::string>& ids) {  // handler
    if (ids.empty() && m_section) m_inspector.show_section(m_outline, *m_section);
    else                          m_inspector.show_items(resolve(ids));
}

// Opening tiles = selecting those items in the navigator, so there is ONE
// route to "open": the navigator's pick (the tile view never opens on its own).
void Shell::on_tiles_open(const std::vector<std::string>& ids) {  // handler
    m_nav.select_items(ids);
}

void Shell::on_undo() {  // handler: take the last step back
    const auto r = m_journal.undo();
    if (!r.done) return;
    if (auto lg = log::get(log::Area::Shell)) lg->info("undo: {}", r.label);
}

void Shell::on_redo() {  // handler: put the undone step back
    const auto r = m_journal.redo();
    if (!r.done) return;
    if (auto lg = log::get(log::Area::Shell)) lg->info("redo: {}", r.label);
}

}  // namespace composix
