#pragma once
// views::Navigator -- the segmented navigator (s002, REQUIREMENTS §10).
//
// One ListBox, two kinds of row:
//   section rows  nav.section.<key>   -- a disclosure arrow, icon, title, count
//   item rows     nav.item.<id>       -- indented, hidden while the section is closed
// Selecting a SECTION row picks the whole category (the main view shows it as
// tiles); selecting ITEM rows picks those items (the main view opens them).
// Multi-selection: Ctrl / Shift click. If item rows are among the selection the
// pick is the items; only when no item is selected does a section row speak.
//
// The arrow is its own button, so disclosing never changes the selection.
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <sigc++/signal.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace composix::views {

struct Pick {
    std::optional<core::Section> section;   // set => a whole category
    std::vector<std::string>     items;     // set => these items (navigator order)
    bool empty() const { return !section && items.empty(); }
};

class Navigator : public widgets::Box {
public:
    Navigator();

    void set_outline(const core::Outline* o);   // rebuilds every row

    // Programmatic selection (a tile was opened; a section was chosen). Emits
    // signal_pick like a click would.
    void select_section(core::Section s);
    void select_items(const std::vector<std::string>& ids);

    void set_open(core::Section s, bool open);  // disclose / fold one section
    bool is_open(core::Section s) const;

    sigc::signal<void(const Pick&)>& signal_pick() { return m_pick; }

private:
    struct SecRow {
        core::Section                 section;
        Gtk::ListBoxRow*              row   = nullptr;
        Gtk::Button*                  arrow = nullptr;
        bool                          open  = false;
        std::vector<Gtk::ListBoxRow*> items;
    };

    void on_selection_changed();
    SecRow* sec(core::Section s);

    widgets::ScrolledWindow m_scroll;
    widgets::ListBox        m_list;

    const core::Outline*                    m_outline = nullptr;
    std::vector<SecRow>                     m_secs;
    std::map<const Gtk::ListBoxRow*, std::string>   m_item_of;   // row -> item id
    std::map<const Gtk::ListBoxRow*, core::Section> m_sec_of;    // section row -> section
    bool                                    m_quiet = false;     // programmatic: one emit at the end
    sigc::signal<void(const Pick&)>         m_pick;
};

}  // namespace composix::views
