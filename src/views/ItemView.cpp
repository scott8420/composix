#include "views/ItemView.hpp"
#include "views/BackButton.hpp"
#include "views/Thumbs.hpp"

#include <algorithm>

namespace composix::views {

ItemView::ItemView()
    : widgets::Box("item", Gtk::Orientation::VERTICAL),
      m_head("item.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("item.title"),
      m_count("item.count"),
      m_area("item.preview") {
    add_css_class("cx-item-view");
    m_head.add_css_class("cx-view-head");
    m_title.add_css_class("cx-view-title");
    m_title.set_xalign(0.0f);
    m_title.set_ellipsize(Pango::EllipsizeMode::END);
    m_count.add_css_class("cx-count");
    m_count.set_valign(Gtk::Align::CENTER);
    m_nav_btns = nav_buttons("item");   // s068
    m_head.append(*m_nav_btns.box);
    m_head.append(m_title);
    m_head.append(m_count);
    append(m_head);

    m_area.add_css_class("cx-pasteboard");
    m_area.set_vexpand(true);
    m_area.set_hexpand(true);
    m_area.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int W, int H) {
        if (m_painter) { m_painter(cr, W, H); return; }   // s163
        if (m_thumbs.empty()) return;
        // Lay the items out left to right, each slot as wide as its proportions
        // want, all scaled together to fit the surface.
        const double gap = 28, pad = 24;
        std::vector<double> want;
        double total = 0;
        for (const auto& t : m_thumbs) {
            const bool shaped = t.kind == core::ThumbKind::Page || t.kind == core::ThumbKind::Image;
            const double a = shaped ? t.w_in / std::max(0.1, t.h_in) : 1.0;
            want.push_back(a);
            total += a;
        }
        const double avail_w = W - 2 * pad - gap * (m_thumbs.size() - 1);
        const double h = std::min(H - 2 * pad, avail_w / total);
        double x = (W - (h * total + gap * (m_thumbs.size() - 1))) / 2;
        const double y = (H - h) / 2;
        for (std::size_t i = 0; i < m_thumbs.size(); ++i) {
            const double w = h * want[i];
            cr->save();
            cr->translate(x, y);
            paint_thumb(cr, m_area, static_cast<int>(w), static_cast<int>(h), m_thumbs[i], true);
            cr->restore();
            x += w + gap;
        }
    });
    append(m_area);
}

void ItemView::show_items(const std::vector<const core::Item*>& items) {
    m_thumbs.clear();
    m_painter = {};
    for (const auto* it : items) m_thumbs.push_back(it->thumb);
    m_title.set_text(core::selection_title(items));
    m_count.set_visible(items.size() > 1);
    m_count.set_text(std::to_string(items.size()));
    m_area.queue_draw();
}

}  // namespace composix::views
