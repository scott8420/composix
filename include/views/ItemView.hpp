#pragma once
// views::ItemView -- the opened item(s) (s002).
//
// Shown when the navigator picks ITEMS. Several pages selected -> side by side.
// s002 paints a PREVIEW (the same thumb painter, large, with margin guides);
// the editing canvas replaces this surface when the page model lands -- the
// shell's routing to it does not change.
#include "views/BackButton.hpp"
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <vector>
#include <functional>

namespace composix::views {

class ItemView : public widgets::Box {
public:
    ItemView();
    void show_items(const std::vector<const core::Item*>& items);
    // s163: something drawn in place of the thumbs (a style's specimen); cleared by show_items.
    using Painter = std::function<void(const Cairo::RefPtr<Cairo::Context>&, int, int)>;
    void set_painter(Painter p) { m_painter = std::move(p); m_area.queue_draw(); }
    void redraw() { m_area.queue_draw(); }   // s164
    // s068: where back / forward before the title go ("Document").
    void set_nav(const std::string& back_to, const std::string& fwd_to) { m_nav_btns.set(back_to, fwd_to); }

private:
    widgets::Box         m_head;
    NavButtons         m_nav_btns;   // s068
    widgets::Label       m_title;
    widgets::Label       m_count;
    widgets::DrawingArea m_area;
    std::vector<core::Thumb> m_thumbs;
    Painter m_painter;   // s163
};

}  // namespace composix::views
