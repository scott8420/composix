#pragma once
#include "widgets/Widgets.hpp"
#include "core/Outline.hpp"
#include "core/Stones.hpp"   // the journal's store until the page model replaces it (s003)
#include "views/Inspector.hpp"
#include "views/ItemView.hpp"
#include "views/Navigator.hpp"
#include "views/PanelHandle.hpp"
#include "views/TileView.hpp"

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>

#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar (inherited from Cairn).
//
// Split across TUs by CATEGORY OF CODE, not by feature. The header IS the index:
// every method carries a trailing `// category: ...` tag naming its file.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: header, the three panes, menu.
//               File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: Gio actions, recents group, the window's
//               undo keys, the views' signals.  File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*).  File: src/Shell_handlers.cpp
//   Helpers  -- reusable sync / orchestration.  File: src/Shell_helpers.cpp
//   Glue     -- the ctor + build_ui().  File: src/Shell.cpp
//
// Layout (s002, REQUIREMENTS §10)
// -------------------------------
//   header:  logo | navigator toggle ........ title ........ inspector toggle | menu
//   body:    shell.panes  = [ nav | shell.right ]
//            shell.right  = [ shell.main_over (shell.main + shell.inspector.tab)
//                             | shell.inspector.handle | shell.inspector.reveal (inspector) ]
//
// Finding things
// --------------
//   "What happens when I pick in the navigator?" -> Shell_handlers.cpp (on_nav_pick)
//   "What happens when I double-click a tile?"   -> Shell_handlers.cpp (on_tiles_open)
//   "Where are the panes built?"                 -> Shell_zones.cpp    (build_shell)
//   "Where does Ctrl+Z go?"                      -> Shell_bindings.cpp + on_undo/on_redo
// ─────────────────────────────────────────────────────────────────────────────
namespace composix {

class AboutWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: header + panes
    void build_welcome();                        // category: zone: the empty main view
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions, recents, undo keys
    void bind_views();                           // category: bindings: navigator / tiles -> handlers

    // ── handlers ───────────────────────────────────────────────────────────
    void on_nav_pick(const views::Pick& p);      // category: handler: section -> tiles, items -> open
    void on_tiles_selection(const std::vector<std::string>& ids);  // category: handler: -> inspector
    void on_tiles_open(const std::vector<std::string>& ids);       // category: handler: -> navigator selects them
    void on_pick_folder();                       // category: handler: folder chooser -> remember it
    void on_recent_open(const std::string& p);   // category: handler: open a recent (parameterised)
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: the About window
    void on_shortcuts();                         // category: handler: keyboard reference
    void on_undo();                              // category: handler: Ctrl+Z / menu
    void on_redo();                              // category: handler: Ctrl+Shift+Z, Ctrl+Y / menu

    // ── helpers ────────────────────────────────────────────────────────────
    std::vector<const core::Item*> resolve(const std::vector<std::string>& ids) const;  // category: helper
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                  // category: helper: repaint the submenu
    std::string recents_file() const;             // category: helper: the XDG path (UI-side resolution)
    void update_undo_ui();                        // category: helper: Undo / Redo labels + greying
    bool focus_is_text() const;                   // category: helper: the keyboard is in a text box
    void sync_inspector();                        // category: helper: toggle + tab follow the panel
    void tab_hover(bool in);                      // category: helper: the pull tab's shared hover

    // Header.
    widgets::Button       m_logo_button;
    widgets::ToggleButton m_nav_toggle;
    widgets::Box          m_title_box;
    widgets::Label        m_title;
    widgets::Label        m_subtitle;
    widgets::ToggleButton m_insp_toggle;
    widgets::MenuButton   m_menu_button;

    // Body.
    widgets::Paned  m_panes;   // nav | inner
    widgets::Box     m_right;      // [main + pull tab][handle][inspector] (s003)
    widgets::Overlay m_main_over;  // the main view, the pull tab floating on its right edge
    widgets::Revealer m_insp_rev;  // the inspector slides
    widgets::Box     m_insp_tab;   // the pull tab -- IS the inspector's handle (as Samson)
    Gtk::Label       m_insp_tab_glyph;
    std::unique_ptr<views::PanelHandle> m_insp_handle;   // the inspector's inner edge
    sigc::connection m_tab_fade;   // "lit" ends a moment after the pointer leaves
    widgets::Stack  m_main;    // "welcome" / "tiles" / "item"
    widgets::Box    m_welcome;
    views::Navigator m_nav;
    views::TileView  m_tiles;
    views::ItemView  m_item;
    views::Inspector m_inspector;

    // What the views read (s002: the fixture; s003: projected from the model).
    core::Outline   m_outline;
    std::optional<core::Section> m_section;   // the category on the tile view

    // Recents state (core data; UI holds it + resolves the path -- the seam).
    std::vector<std::string>        m_recents;
    std::string                     m_recents_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;

    // Undo. The journal and its door stay wired from day one; s002 writes
    // nothing, so it is empty. s003 swaps core::Stones for the page model.
    core::Stones       m_stones;
    core::StoneJournal m_journal{m_stones};
    Glib::RefPtr<Gio::Menu>         m_edit_menu;
    Glib::RefPtr<Gio::SimpleAction> m_act_undo, m_act_redo;

    std::unique_ptr<AboutWindow>     m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;
};

}  // namespace composix
