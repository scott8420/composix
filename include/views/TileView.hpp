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

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace composix::views {

class TileView : public widgets::Box {
public:
    TileView();

    // `keep`: tiles to leave selected (a refresh after an edit), silently.
    void show_section(const core::Outline& o, core::Segment s, const std::vector<std::string>& keep = {});
    std::vector<std::string> selected() const;   // grid order
    // s028: how the Pages section groups into tiles (spreads); empty groups
    // = a tile per page.
    void set_grouping(std::function<std::vector<std::vector<std::string>>()> g) { m_grouping = std::move(g); }

    sigc::signal<void(const std::vector<std::string>&)>& signal_selection() { return m_sel; }
    sigc::signal<void(const std::vector<std::string>&)>& signal_open()      { return m_open; }
    // s014: Delete / Backspace on the tiles; tiles dragged onto another
    // (before it when dropped on its left half). Only where reorderable.
    sigc::signal<void(const std::vector<std::string>&)>& signal_delete()    { return m_del; }
    sigc::signal<void(const std::vector<std::string>&, const std::string& target, bool before)>& signal_move() {
        return m_move;
    }

private:
    widgets::Box            m_head;
    widgets::Label          m_title;
    widgets::Label          m_count;
    widgets::ScrolledWindow m_scroll;
    widgets::FlowBox        m_grid;

    std::map<const Gtk::FlowBoxChild*, std::vector<std::string>> m_id_of;   // s028: a spread tile: its pages
    std::function<std::vector<std::vector<std::string>>()> m_grouping;
    bool m_quiet = false;
    sigc::signal<void(const std::vector<std::string>&)> m_sel, m_open, m_del;
    sigc::signal<void(const std::vector<std::string>&, const std::string&, bool)> m_move;
    std::vector<std::string> m_drag_ids;   // the tiles being dragged
    bool m_reorder = false;                // the pages segment
};

}  // namespace composix::views
