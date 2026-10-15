#pragma once
// views::TileView -- a whole category as tiles with metadata (s002).
//
// Shown when the navigator picks a SECTION. Each tile: a thumbnail (pages keep
// their proportions, so a foldout tile is wide), the title, a badge, and the
// item's leading fields. Multi-select (Ctrl / Shift / rubber band); the
// selection feeds the inspector. Double-click or Enter OPENS: the selected
// tiles if the activated one is among them, else just that one.
//
// Tiles are rebuilt per section and named tiles.tile.<id> -- named for the
// Inspector, not registered (transient content: CANON's unregistered_t rule).
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <sigc++/signal.h>

#include <map>
#include <string>
#include <vector>

namespace composix::views {

class TileView : public widgets::Box {
public:
    TileView();

    void show_section(const core::Outline& o, core::Section s);
    std::vector<std::string> selected() const;   // grid order

    sigc::signal<void(const std::vector<std::string>&)>& signal_selection() { return m_sel; }
    sigc::signal<void(const std::vector<std::string>&)>& signal_open()      { return m_open; }

private:
    widgets::Box            m_head;
    widgets::Label          m_title;
    widgets::Label          m_count;
    widgets::ScrolledWindow m_scroll;
    widgets::FlowBox        m_grid;

    std::map<const Gtk::FlowBoxChild*, std::string> m_id_of;
    bool m_quiet = false;
    sigc::signal<void(const std::vector<std::string>&)> m_sel, m_open;
};

}  // namespace composix::views
