#include "views/TileView.hpp"
#include "views/Thumbs.hpp"
#include "Log.hpp"

#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/widgetpaintable.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gdkmm/contentprovider.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/overlay.h>

#include <glibmm/main.h>

#include <algorithm>

namespace composix::views {

namespace {
constexpr int kThumbH = 150;
constexpr int kCardW  = 176;   // every card the same width: the grid stays a grid
constexpr int kFieldsOnTile = 3;
}

TileView::TileView()
    : widgets::Box("tiles", Gtk::Orientation::VERTICAL),
      m_head("tiles.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("tiles.title"),
      m_count("tiles.count"),
      m_scroll("tiles.scroll"),
      m_grid("tiles.grid") {
    add_css_class("cx-tiles");

    m_head.add_css_class("cx-view-head");
    m_title.add_css_class("cx-view-title");
    m_title.set_xalign(0.0f);
    m_count.add_css_class("cx-count");
    m_count.set_valign(Gtk::Align::CENTER);
    m_head.append(m_title);
    m_head.append(m_count);
    append(m_head);

    m_grid.set_selection_mode(Gtk::SelectionMode::MULTIPLE);
    m_grid.set_activate_on_single_click(false);
    m_grid.set_homogeneous(true);
    m_grid.set_valign(Gtk::Align::START);
    m_grid.set_max_children_per_line(40);
    m_grid.set_column_spacing(18);
    m_grid.set_row_spacing(18);
    m_grid.add_css_class("cx-tile-grid");
    m_grid.signal_selected_children_changed().connect([this] {
        if (m_quiet) return;
        m_sel.emit(selected());
    });
    m_grid.signal_child_activated().connect([this](Gtk::FlowBoxChild* c) {
        auto ids = selected();
        const auto it = m_id_of.find(c);
        if (it == m_id_of.end()) return;
        if (std::find(ids.begin(), ids.end(), it->second) == ids.end()) ids = {it->second};
        if (auto lg = log::get(log::Area::View)) lg->info("tiles open: {}", ids.size());
        m_open.emit(ids);
    });

    // s014: Delete / Backspace on the selected tiles.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
        if (kv != GDK_KEY_Delete && kv != GDK_KEY_BackSpace && kv != GDK_KEY_KP_Delete) return false;
        const auto ids = selected();
        if (!m_reorder || ids.empty()) return false;
        m_del.emit(ids);
        return true;
    }, false);
    m_grid.add_controller(keys);

    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_grid);
    append(m_scroll);
}

void TileView::show_section(const core::Outline& o, core::Segment s, const std::vector<std::string>& keep) {
    m_quiet = true;
    while (auto* c = m_grid.get_first_child()) m_grid.remove(*c);
    m_id_of.clear();

    const auto items = o.in(s);
    m_reorder = s == core::Segment::Pages;
    m_title.set_text(core::segment_info(s).title);
    m_count.set_text(std::to_string(items.size()));

    for (const auto* it : items) {
        auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_name("tiles.tile." + it->id);
        child->add_css_class("cx-tile");
        if (it->thumb.flag) child->add_css_class("cx-flagged");

        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("cx-tile-card");

        // Thumbnail, with the badge overlaid top-right.
        const int tw = kCardW;   // the painter fits the thumb inside; a foldout is wide and short
        auto overlay = Gtk::make_managed<Gtk::Overlay>();
        auto area = Gtk::make_managed<Gtk::DrawingArea>();
        area->set_name("tiles.tile." + it->id + ".thumb");
        area->add_css_class("cx-thumb");
        area->set_content_width(tw);
        area->set_content_height(kThumbH);
        const core::Thumb th = it->thumb;
        area->set_draw_func([area, th](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            paint_thumb(cr, *area, w, h, th);
        });
        overlay->set_child(*area);
        if (!it->badge.empty()) {
            auto b = Gtk::make_managed<Gtk::Label>(it->badge);
            b->add_css_class("cx-badge");
            if (it->thumb.flag) b->add_css_class("cx-warn");
            b->set_halign(Gtk::Align::END);
            b->set_valign(Gtk::Align::START);
            b->set_margin(6);
            overlay->add_overlay(*b);
        }
        card->append(*overlay);

        auto title = Gtk::make_managed<Gtk::Label>(it->title);
        title->add_css_class("cx-tile-title");
        title->set_xalign(0.0f);
        title->set_ellipsize(Pango::EllipsizeMode::END);
        title->set_max_width_chars(1);   // let the card's width decide
        title->set_hexpand(true);
        card->append(*title);

        auto meta = Gtk::make_managed<Gtk::Grid>();
        meta->add_css_class("cx-tile-meta");
        meta->set_column_spacing(8);
        int row = 0;
        for (const auto& f : it->fields) {
            if (row == kFieldsOnTile) break;
            auto l = Gtk::make_managed<Gtk::Label>(f.label);
            l->add_css_class("cx-field-label");
            l->set_xalign(0.0f);
            auto v = Gtk::make_managed<Gtk::Label>(f.value);
            v->add_css_class("cx-field-value");
            v->set_xalign(0.0f);
            v->set_hexpand(true);
            v->set_ellipsize(Pango::EllipsizeMode::END);
            v->set_max_width_chars(1);
            meta->attach(*l, 0, row);
            meta->attach(*v, 1, row);
            ++row;
        }
        card->append(*meta);
        card->set_size_request(kCardW, -1);

        child->set_child(*card);
        if (m_reorder) {
            // s014: drag pages to reorder -- the selection if this tile is in it.
            const std::string id = it->id;
            auto src = Gtk::DragSource::create();
            src->set_actions(Gdk::DragAction::MOVE);
            src->signal_prepare().connect([this, id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                auto ids = selected();
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids = {id};
                m_drag_ids = ids;
                Glib::Value<Glib::ustring> v;
                v.init(v.value_type());
                v.set("composix-pages");
                return Gdk::ContentProvider::create(v);
            }, false);
            // The tile itself rides with the pointer (not the payload's text).
            src->signal_drag_begin().connect([src, card](const Glib::RefPtr<Gdk::Drag>&) {
                src->set_icon(Gtk::WidgetPaintable::create(*card), card->get_width() / 2, 30);
            });
            child->add_controller(src);
            auto drop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::MOVE);
            const auto mark = [child](double x) {
                const bool before = x < child->get_width() / 2.0;
                child->remove_css_class(before ? "cx-drop-after" : "cx-drop-before");
                child->add_css_class(before ? "cx-drop-before" : "cx-drop-after");
            };
            drop->signal_enter().connect([mark](double x, double) { mark(x); return Gdk::DragAction::MOVE; }, false);
            drop->signal_motion().connect([mark](double x, double) { mark(x); return Gdk::DragAction::MOVE; }, false);
            drop->signal_leave().connect([child] {
                child->remove_css_class("cx-drop-before");
                child->remove_css_class("cx-drop-after");
            });
            drop->signal_drop().connect([this, child, id](const Glib::ValueBase&, double x, double) {
                child->remove_css_class("cx-drop-before");
                child->remove_css_class("cx-drop-after");
                if (m_drag_ids.empty()) return false;
                const auto ids = m_drag_ids;
                m_drag_ids.clear();
                const bool before = x < child->get_width() / 2.0;
                // After the drop's own handling: the move rebuilds these tiles.
                Glib::signal_idle().connect_once([this, ids, id, before] { m_move.emit(ids, id, before); });
                return true;
            }, false);
            child->add_controller(drop);
        }
        m_grid.append(*child);
        m_id_of[child] = it->id;
        if (std::find(keep.begin(), keep.end(), it->id) != keep.end()) m_grid.select_child(*child);
    }
    m_quiet = false;
    if (auto lg = log::get(log::Area::View))
        lg->info("tiles: {} ({})", core::segment_info(s).key, items.size());
}

std::vector<std::string> TileView::selected() const {
    std::vector<std::string> out;
    for (auto* c = m_grid.get_first_child(); c; c = c->get_next_sibling()) {
        auto* fc = dynamic_cast<const Gtk::FlowBoxChild*>(c);
        if (!fc || !fc->is_selected()) continue;
        if (auto it = m_id_of.find(fc); it != m_id_of.end()) out.push_back(it->second);
    }
    return out;
}

}  // namespace composix::views
