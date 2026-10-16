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
#include "views/BackButton.hpp"
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <sigc++/signal.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace composix::views {

// s052: how big the tiles are -- Colors offers all three (others: Large).
enum class TileSize { Large, Small, List };

class TileView : public widgets::Box {
public:
    // s068: where back / forward before the title go.
    void set_nav(const std::string& back_to, const std::string& fwd_to) { m_nav_btns.set(back_to, fwd_to); }
    TileView();

    TileSize tile_size() const { return m_size; }
    void     set_tile_size(TileSize z);   // redraws the section on show (quietly: no signal)
    sigc::signal<void(TileSize)>& signal_tile_size() { return m_size_sig; }
    sigc::signal<void()>&         signal_new()       { return m_new_sig; }   // s058: Colors' New Swatch
    // s062: a master dropped on page tiles: (master, pages).
    sigc::signal<void(const std::string&, const std::vector<std::string>&)>& signal_master_drop() { return m_master_drop; }

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
    NavButtons              m_nav_btns;   // s068
    widgets::Label          m_title;
    widgets::Label          m_count;
    widgets::ScrolledWindow m_scroll;
    widgets::Box            m_body;      // s052: m_grid, or the grouped grids
    widgets::FlowBox        m_grid;
    widgets::Box            m_groups;    // s052: a heading + a grid per item group
    widgets::Box            m_sizes;     // s052: Large / Small / List
    std::vector<Gtk::FlowBox*> m_grids;  // the grids on show
    TileSize                m_size = TileSize::Large;
    sigc::signal<void(TileSize)> m_size_sig;
    sigc::signal<void()>    m_new_sig;
    sigc::signal<void(const std::string&, const std::vector<std::string>&)> m_master_drop;
    Gtk::Widget*            m_new_btn = nullptr;
    core::Outline           m_last;      // the section on show, to redraw at another size
    core::Segment           m_seg = core::Segment::Document;
    bool                    m_shown = false;

    void setup_grid(Gtk::FlowBox& g);
    void sync_size_buttons();

    std::map<const Gtk::FlowBoxChild*, std::vector<std::string>> m_id_of;   // s028: a spread tile: its pages
    std::function<std::vector<std::vector<std::string>>()> m_grouping;
    bool m_quiet = false;
    sigc::signal<void(const std::vector<std::string>&)> m_sel, m_open, m_del;
    sigc::signal<void(const std::vector<std::string>&, const std::string&, bool)> m_move;
    std::vector<std::string> m_drag_ids;   // the tiles being dragged
    bool m_reorder = false;                // the pages segment
};

}  // namespace composix::views
