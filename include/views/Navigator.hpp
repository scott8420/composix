#pragma once
// views::Navigator -- the navigator as a column of cards (s277; was one
// source-list ListBox, s002).
//
// One card per section (core::segments() order): a header -- disclosure
// arrow, icon, title, count -- and a body shown while the card is open. Any
// number of cards may be open at once (Scott, s277).
//   Pages    the spreads as small pictures, a folio under each page and its
//            master's prefix in the page's outside corner; S / M / L sizes.
//   Masters  each master's mock-up (s149) at the same size, its name under.
//   others   rows, as before.
// Clicking a card's HEADER picks the whole section (the main view shows it as
// tiles); clicking items picks those items (the main view opens them).
// Ctrl+click adds / removes, Shift+click takes the run from the last click.
// A pick lives in one card at a time.
//
// Names: nav, nav.scroll, nav.cards; per card nav.segment.<key> (+ .arrow,
// .head, .body); items nav.item.<id>.
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <sigc++/signal.h>
#include <gtkmm/drawingarea.h>
#include <memory>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Gtk { class DrawingArea; class ListBox; class ListBoxRow; class SearchEntry; class ToggleButton; }

namespace composix::views {

class WrapBox;

struct Pick {
    std::optional<core::Segment> segment;   // set => a whole category
    std::vector<std::string>     items;     // set => these items (navigator order)
    bool empty() const { return !segment && items.empty(); }
};

class Navigator : public widgets::Box {
public:
    Navigator();
    ~Navigator() override;   // s285: the cards' WrapBoxes let their children go

    void set_outline(const core::Outline* o);   // rebuilds every card
    // s004: the document changed -- update in place when the items are the
    // same (s006), else rebuild keeping what is open and picked. Emits
    // nothing: the shell repaints its own views after.
    void refresh(const core::Outline* o);

    // Programmatic selection (a tile was opened; a section was chosen). Emits
    // signal_pick like a click would.
    void select_section(core::Segment s);
    void select_items(const std::vector<std::string>& ids);

    void set_open(core::Segment s, bool open);  // open / close one card
    bool is_open(core::Segment s) const;

    // s277: the page pictures' height in px (Small 56, Medium 80, Large 112).
    static constexpr int kSmall = 56, kMedium = 80, kLarge = 112;
    void set_thumb_height(int px);
    int  thumb_height() const { return m_thumb_h; }

    // s277: which cards are open, as section keys "pages,masters" (kept
    // between runs by the shell).
    std::string open_keys() const;
    void        set_open_keys(const std::string& csv);

    sigc::signal<void(const Pick&)>& signal_pick() { return m_pick; }
    // s062: a master dropped on pages: (master, pages).
    sigc::signal<void(const std::string&, const std::vector<std::string>&)>& signal_master_drop() { return m_master_drop; }
    // s278: pages dragged in the Pages card -- (pages, onto page, before it?, s283: copied -- Ctrl held?).
    sigc::signal<void(const std::vector<std::string>&, const std::string&, bool, bool)>& signal_move_pages() { return m_move_sig; }
    // s278: a master dropped in the gap before / after a page -- (master, page, before?).
    sigc::signal<void(const std::string&, const std::string&, bool)>& signal_master_insert() { return m_insert_sig; }
    // s278: the cards' order, dragged by their headers ("masters,pages,...").
    std::string order_keys() const;
    void        set_order_keys(const std::string& csv);
    sigc::signal<void()>&            signal_order_changed() { return m_order_sig; }
    // s341: a layer's row dropped above / below another's -- (layer, onto, above?).
    sigc::signal<void(const std::string&, const std::string&, bool)>& signal_layer_move() { return m_layer_move_sig; }
    // s279: a right-click -- on pictures (pages: picked first, as a click
    // would; a master: as it is), or a card's header (the section). x, y in
    // the navigator's own coordinates.
    sigc::signal<void(const Pick&, double, double)>& signal_context() { return m_ctx_sig; }
    // s279: every card open / closed; the cards back in their first order.
    void open_all(bool open);
    void reset_order();
    sigc::signal<void(int)>&         signal_thumb_height() { return m_thumb_sig; }
    // s308: a Layers row's eye / padlock / printer pressed -- (layer, "lvis" /
    // "llock" / "lprint", "1" / "0"), as the inspector's rows send them.
    sigc::signal<void(const std::string&, const std::string&, const std::string&)>& signal_layer_toggle() { return m_layer_sig; }
    sigc::signal<void()>&            signal_open_changed() { return m_open_sig; }

    // s031: how the pages group into spreads. Empty groups: one page each.
    void set_grouping(std::function<std::vector<std::vector<std::string>>()> g) { m_grouping = std::move(g); }

    // s277: for the selftest and logs -- the card pictures' layout as text:
    // "[-|1] [2|3] ..." (spreads in order; '-' an empty side).
    std::string spreads_text() const;

private:
    struct Cell {   // one picture: a page, or a master's mock-up
        std::string       id;
        Gtk::Box*         col   = nullptr;   // what is ringed when picked
        Gtk::DrawingArea* da    = nullptr;
        Gtk::Label*       under = nullptr;   // folio (under the spread, s278) / master name
        Gtk::Label*       badge = nullptr;   // master prefix (pages)
        core::Thumb       thumb;
        double            aspect = 2.0 / 3.0;
    };
    struct Card {
        core::Segment                 segment;
        Gtk::Box*                     box   = nullptr;
        Gtk::Box*                     head  = nullptr;
        Gtk::Button*                  arrow = nullptr;
        Gtk::Label*                   count = nullptr;
        Gtk::Box*                     body  = nullptr;
        Gtk::ListBox*                 list  = nullptr;   // rows
        WrapBox*                      flow  = nullptr;   // pictures (pages, masters)
        std::vector<std::string>      ids;               // its items, in order
        std::map<std::string, Gtk::ListBoxRow*> rows;
        std::map<std::string, std::pair<Gtk::Label*, Gtk::Label*>> labels;   // row -> title, badge
        std::map<std::string, Cell>   cells;
        std::vector<std::vector<std::string>> spreads;   // pages: as shown
        std::map<std::string, Gtk::ToggleButton*> sizes; // pages: S / M / L
        Gtk::Box*                     chips = nullptr;   // s287: Colors -- the chips' column (its first row is `flow`)
        std::map<std::string, Gtk::Label*> samples;      // s292: Stories -- each one's first words
        std::map<std::string, std::pair<Gtk::DrawingArea*, std::shared_ptr<std::uint32_t>>> layer_chips;   // s341: Layers -- each one's colour
        std::vector<std::pair<Gtk::Widget*, WrapBox*>> chip_heads;   // s298: Colors -- a group's heading, its row
        std::map<std::string, std::array<Gtk::ToggleButton*, 3>> layer_btns;   // s308: Layers -- eye, padlock, printer
    };

    Card*       card(core::Segment s);
    const Card* card(core::Segment s) const;
    Card*       card_of(const std::string& id);
    void        build_card(const core::SegmentInfo& si, const std::vector<const core::Item*>& items);
    void        build_pictures(Card& c, const std::vector<const core::Item*>& items);
    Gtk::Box*   make_cell(Card& c, const core::Item& it, bool left, bool right);
    void        apply_cell(Cell& cell, const core::Item& it, bool page);
    void        size_cell(Cell& cell);
    void        show_open(Card& c);
    void        show_picks();             // rows, rings, headers -- no emit
    void        emit_pick();
    void        click_item(Card& c, const std::string& id, bool ctrl, bool shift);
    void        on_list_changed(Card& c);
    void        scroll_to(Gtk::Widget& w);
    void        build_chips(Card& c, const std::vector<const core::Item*>& items);   // s287
    void        build_assets(Card& c, const std::vector<const core::Item*>& items);  // s292
    static void specimen(Gtk::Label& l, const core::Item& it);                         // s287
    // s278: drag and drop
    struct Hit { std::string page; int zone = 0; };   // zone: -1 before, 0 onto, +1 after
    Hit         hit_page(Card& c, double x, double y, bool master);
    void        mark_drop(Card& c, const Hit& h);
    void        wire_page_drop(Card& c);
    void        wire_card_drag(Card& c);
    std::vector<std::string> drag_ids(const std::string& id);
    void        wire_context(Gtk::Widget& w, std::function<Pick()> what);   // s279

    widgets::ScrolledWindow m_scroll;
    widgets::Box            m_cards;
    Gtk::SearchEntry*       m_filter_entry = nullptr;   // s298
    std::string             m_filter;                   // s298: lower-cased; "" = all
    void        apply_filter();                         // s298: each card's items shown that match; cards with none hidden

    const core::Outline*         m_outline = nullptr;
    std::vector<Card>            m_cardv;
    std::set<core::Segment>      m_open{core::Segment::Pages};
    std::vector<core::Segment>   m_order;    // s278: the cards top to bottom (empty: core::segments())
    std::optional<core::Segment> m_seg_pick;
    std::vector<std::string>     m_picked;
    std::string                  m_anchor;   // Shift+click runs from here
    int                          m_thumb_h = kMedium;
    bool                         m_quiet = false;
    std::map<core::Segment, std::vector<WrapBox*>> m_chip_rows;   // s287: Colors' rows after its first

    sigc::signal<void(const Pick&)> m_pick;
    sigc::signal<void(const std::string&, const std::vector<std::string>&)> m_master_drop;
    sigc::signal<void(int)>         m_thumb_sig;
    sigc::signal<void()>            m_open_sig;
    sigc::signal<void(const std::vector<std::string>&, const std::string&, bool, bool)> m_move_sig;
    sigc::signal<void(const std::string&, const std::string&, bool)> m_insert_sig;
    sigc::signal<void()>            m_order_sig;
    sigc::signal<void(const std::string&, const std::string&, bool)> m_layer_move_sig;   // s341
    sigc::signal<void(const Pick&, double, double)> m_ctx_sig;
    sigc::signal<void(const std::string&, const std::string&, const std::string&)> m_layer_sig;   // s308
    void        layer_toggles(Card& c, const core::Item& it, Gtk::Box& row);                          // s308
    void        layer_row_drag(Gtk::ListBoxRow& row, const std::string& id);                        // s341
    void        set_layer_toggles(Card& c, const core::Item& it);
    std::function<std::vector<std::vector<std::string>>()> m_grouping;
};

}  // namespace composix::views
