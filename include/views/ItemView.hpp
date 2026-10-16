#pragma once
// views::ItemView -- the opened item(s) (s002).
//
// Shown when the navigator picks ITEMS. Several pages selected -> side by side.
// s002 paints a PREVIEW (the same thumb painter, large, with margin guides);
// the editing canvas replaces this surface when the page model lands -- the
// shell's routing to it does not change.
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <vector>

namespace composix::views {

class ItemView : public widgets::Box {
public:
    ItemView();
    void show_items(const std::vector<const core::Item*>& items);
    // s068: where the back arrow before the title goes ("Document").
    void set_back(const std::string& to) { if (m_back) m_back->set_tooltip_text("Back to " + to + "  (Alt+Up)"); }

private:
    widgets::Box         m_head;
    Gtk::Button*         m_back = nullptr;   // s068
    widgets::Label       m_title;
    widgets::Label       m_count;
    widgets::DrawingArea m_area;
    std::vector<core::Thumb> m_thumbs;
};

}  // namespace composix::views
