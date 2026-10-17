#include "views/TileView.hpp"
#include "views/Thumbs.hpp"
#include "Log.hpp"

#include <gtkmm/button.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/widgetpaintable.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/togglebutton.h>
#include <gdkmm/contentprovider.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/overlay.h>

#include <glibmm/main.h>

#include <algorithm>
#include <tuple>

namespace composix::views {

namespace {
constexpr int kThumbH = 150;
constexpr int kCardW  = 176;   // every card the same width: the grid stays a grid
constexpr int kFieldsOnTile = 3;
constexpr int kSpreadW = 272;   // s028: two pages a card, and still two cards a row in the usual width
// s052: the small tiles and the list's chip.
constexpr int kSmallW = 96, kSmallH = 64;
constexpr int kChip   = 28;
constexpr int kListFields = 3;
}

TileView::TileView()
    : widgets::Box("tiles", Gtk::Orientation::VERTICAL),
      m_head("tiles.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("tiles.title"),
      m_count("tiles.count"),
      m_scroll("tiles.scroll"),
      m_body("tiles.body", Gtk::Orientation::VERTICAL),
      m_grid("tiles.grid"),
      m_groups("tiles.groups", Gtk::Orientation::VERTICAL),
      m_sizes("tiles.sizes", Gtk::Orientation::HORIZONTAL) {
    add_css_class("cx-tiles");

    m_head.add_css_class("cx-view-head");
    m_title.add_css_class("cx-view-title");
    m_title.set_xalign(0.0f);
    m_count.add_css_class("cx-count");
    m_count.set_valign(Gtk::Align::CENTER);
    m_nav_btns = nav_buttons("tiles");   // s068
    m_head.append(*m_nav_btns.box);
    m_head.append(m_title);
    m_head.append(m_count);

    // s052: Large / Small / List (s080: every section).
    m_sizes.add_css_class("linked");
    m_sizes.set_hexpand(true);
    m_sizes.set_halign(Gtk::Align::END);
    m_sizes.set_valign(Gtk::Align::CENTER);
    Gtk::ToggleButton* first = nullptr;
    for (const auto& [z, key, label, tip] : {std::tuple{TileSize::Large, "large", "Large", "Large tiles"},
                                             std::tuple{TileSize::Small, "small", "Small", "Small tiles"},
                                             std::tuple{TileSize::List, "list", "List", "One a line: a small picture, the name, its first details"}}) {
        auto b = Gtk::make_managed<Gtk::ToggleButton>(label);
        b->set_name(std::string("tiles.size.") + key);
        b->set_tooltip_text(tip);
        if (first) b->set_group(*first); else first = b;
        b->signal_toggled().connect([this, b, z = z] {
            if (!b->get_active() || m_quiet || z == tile_size(m_seg)) return;
            m_size_of[m_seg] = z;
            if (auto lg = log::get(log::Area::View))
                lg->info("tiles: {} size {}", core::segment_info(m_seg).key, static_cast<int>(z));
            if (m_shown) show_section(m_last, m_seg, selected());
            m_size_sig.emit(m_seg, z);
        });
        m_sizes.append(*b);
    }
    m_sizes.set_visible(false);
    m_head.append(m_sizes);
    {   // s058: a new swatch, from the Colors view.
        auto nb = Gtk::make_managed<Gtk::Button>("New Swatch\u2026");
        nb->set_name("tiles.new");
        nb->set_valign(Gtk::Align::CENTER);
        nb->set_visible(false);
        nb->signal_clicked().connect([this] { m_new_sig.emit(); });
        m_head.append(*nb);
        m_new_btn = nb;
    }
    append(m_head);

    setup_grid(m_grid);
    m_grid.set_name("tiles.grid");

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

    m_groups.set_visible(false);
    m_body.append(m_grid);
    m_body.append(m_groups);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_body);
    append(m_scroll);
    sync_size_buttons();
}

TileSize TileView::tile_size(core::Segment s) const {
    const auto it = m_size_of.find(s);
    return it == m_size_of.end() ? TileSize::Large : it->second;
}

void TileView::set_tile_size(core::Segment s, TileSize z) {
    m_size_of[s] = z;
    sync_size_buttons();
}

void TileView::sync_size_buttons() {
    m_quiet = true;
    int i = 0;
    for (auto* c = m_sizes.get_first_child(); c; c = c->get_next_sibling(), ++i)
        if (auto* b = dynamic_cast<Gtk::ToggleButton*>(c)) b->set_active(i == static_cast<int>(tile_size(m_seg)));
    m_quiet = false;
}

// A grid of tiles: multi-select, double-click opens. s052: several grids
// (one per group) act as one -- a plain click in one clears the others.
void TileView::setup_grid(Gtk::FlowBox& g) {
    g.set_selection_mode(Gtk::SelectionMode::MULTIPLE);
    g.set_activate_on_single_click(false);
    g.set_homogeneous(true);
    g.set_valign(Gtk::Align::START);
    g.add_css_class("cx-tile-grid");
    g.signal_selected_children_changed().connect([this] {
        if (m_quiet) return;
        m_sel.emit(selected());
    });
    auto click = Gtk::GestureClick::create();
    click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    auto* cp = click.get();
    click->signal_pressed().connect([this, &g, cp](int, double, double) {
        const auto st = cp->get_current_event_state();
        if ((st & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK)) != Gdk::ModifierType{}) return;
        for (auto* o : m_grids) if (o != &g) o->unselect_all();
    });
    g.add_controller(click);
    g.signal_child_activated().connect([this](Gtk::FlowBoxChild* c) {
        auto ids = selected();
        const auto it = m_id_of.find(c);
        if (it == m_id_of.end()) return;
        if (std::find(ids.begin(), ids.end(), it->second.front()) == ids.end()) ids = it->second;
        if (auto lg = log::get(log::Area::View)) lg->info("tiles open: {}", ids.size());
        m_open.emit(ids);
    });
}

void TileView::show_section(const core::Outline& o, core::Segment s, const std::vector<std::string>& keep) {
    m_quiet = true;
    if (&o != &m_last) m_last = o;
    m_seg = s;
    m_shown = true;
    while (auto* c = m_grid.get_first_child()) m_grid.remove(*c);
    while (auto* c = m_groups.get_first_child()) m_groups.remove(*c);
    m_id_of.clear();
    m_grids.clear();

    const auto items = o.in(s);
    // s052: Colors comes Large, Small or as a List, under its groups'
    // headings; s080: every section has the three, each remembered.
    const TileSize z = tile_size(s);
    m_sizes.set_visible(true);
    sync_size_buttons();
    if (m_new_btn) m_new_btn->set_visible(s == core::Segment::Colors);
    bool grouped = false;
    for (const auto* it : items) grouped = grouped || (s == core::Segment::Colors && !it->group.empty());
    m_grid.set_visible(!grouped);
    m_groups.set_visible(grouped);
    const auto fit = [z](Gtk::FlowBox& g) {
        g.set_max_children_per_line(z == TileSize::List ? 1 : 40);
        g.set_column_spacing(z == TileSize::Large ? 18 : 12);
        g.set_row_spacing(z == TileSize::Large ? 18 : z == TileSize::Small ? 12 : 4);
        if (z == TileSize::List) g.add_css_class("cx-tile-list"); else g.remove_css_class("cx-tile-list");
    };
    std::map<std::string, Gtk::FlowBox*> grid_of;
    const auto grid_for = [&](const std::string& gname) -> Gtk::FlowBox& {
        if (!grouped) return m_grid;
        if (auto f = grid_of.find(gname); f != grid_of.end()) return *f->second;
        auto head = Gtk::make_managed<Gtk::Label>(gname.empty() ? std::string("Other") : gname);
        head->set_name("tiles.group." + std::to_string(grid_of.size()) + ".head");
        head->add_css_class("cx-tile-group");
        head->set_xalign(0.0f);
        m_groups.append(*head);
        auto g = Gtk::make_managed<Gtk::FlowBox>();
        g->set_name("tiles.group." + std::to_string(grid_of.size()));
        setup_grid(*g);
        fit(*g);
        m_groups.append(*g);
        m_grids.push_back(g);
        grid_of[gname] = g;
        return *g;
    };
    fit(m_grid);
    if (!grouped) m_grids.push_back(&m_grid);
    m_reorder = s == core::Segment::Pages;
    m_title.set_text(core::segment_info(s).title);
    m_count.set_text(std::to_string(items.size()));

    // s028: facing pages -- a tile per SPREAD (its pages side by side); else
    // a tile per item.
    std::vector<std::vector<const core::Item*>> tiles;
    const auto groups = (s == core::Segment::Pages && m_grouping) ? m_grouping() : std::vector<std::vector<std::string>>{};
    if (!groups.empty()) {
        for (const auto& g : groups) {
            std::vector<const core::Item*> t;
            for (const auto& id : g)
                for (const auto* it : items) if (it->id == id) t.push_back(it);
            if (!t.empty()) tiles.push_back(t);
        }
    } else {
        for (const auto* it : items) tiles.push_back({it});
    }
    const bool spreads = !groups.empty();

    for (const auto& group : tiles) {
        const auto* it = group.front();
        std::vector<std::string> gids;
        for (const auto* g : group) gids.push_back(g->id);
        const std::string tile_id = gids.size() == 1 ? it->id : gids.front() + "+" + gids.back();
        std::string title_text = it->title, badge = it->badge;
        bool flag = false;
        std::vector<core::Thumb> thumbs;
        for (const auto* g : group) {
            thumbs.push_back(g->thumb);
            flag = flag || g->thumb.flag;
            if (badge.empty()) badge = g->badge;
        }
        if (group.size() == 2) {   // "Pages 2–3"
            const auto folio = [](const core::Item* x) { for (const auto& f : x->fields) if (f.label == "Folio") return f.value; return x->title; };
            title_text = "Pages " + folio(group[0]) + "\u2013" + folio(group[1]);
        }
        auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_name("tiles.tile." + tile_id);
        child->add_css_class("cx-tile");
        if (flag) child->add_css_class("cx-flagged");

        Gtk::Box* card = nullptr;
        if (z == TileSize::List) {   // s052: chip, name, its leading values, badge
            card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            card->add_css_class("cx-tile-card");
            card->add_css_class("cx-tile-row");
            auto area = Gtk::make_managed<Gtk::DrawingArea>();
            area->set_name("tiles.tile." + tile_id + ".thumb");
            area->set_content_width(kChip);
            area->set_content_height(kChip);
            area->set_valign(Gtk::Align::CENTER);
            area->set_draw_func([area, thumbs](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                paint_thumb(cr, *area, w, h, thumbs.front());
            });
            card->append(*area);
            auto title = Gtk::make_managed<Gtk::Label>(title_text);
            title->add_css_class("cx-tile-title");
            title->set_xalign(0.0f);
            title->set_hexpand(true);
            title->set_ellipsize(Pango::EllipsizeMode::END);
            title->set_width_chars(14);
            card->append(*title);
            int k = 0;
            for (const auto& f : it->fields) {
                if (k++ == kListFields) break;
                auto v = Gtk::make_managed<Gtk::Label>(f.value);
                v->add_css_class("cx-field-value");
                v->add_css_class("cx-tile-meta");
                v->set_tooltip_text(f.label);
                v->set_xalign(0.0f);
                v->set_width_chars(k == 2 ? 18 : 10);
                v->set_max_width_chars(k == 2 ? 18 : 10);
                v->set_ellipsize(Pango::EllipsizeMode::END);
                card->append(*v);
            }
            auto b = Gtk::make_managed<Gtk::Label>(badge);
            b->add_css_class("cx-badge");
            if (flag) b->add_css_class("cx-warn");
            b->set_valign(Gtk::Align::CENTER);
            b->set_width_chars(9);       // s080: "Foldout", "Missing" -- a fixed slot, so the columns line up
            b->set_max_width_chars(9);
            b->set_ellipsize(Pango::EllipsizeMode::END);
            b->set_opacity(badge.empty() ? 0 : 1);
            card->append(*b);
        } else if (z == TileSize::Small) {   // s052: the thumbnail and the name
            card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            card->add_css_class("cx-tile-card");
            card->add_css_class("cx-tile-small");
            auto overlay = Gtk::make_managed<Gtk::Overlay>();
            auto area = Gtk::make_managed<Gtk::DrawingArea>();
            area->set_name("tiles.tile." + tile_id + ".thumb");
            area->add_css_class("cx-thumb");
            area->set_content_width(spreads ? kSmallW * 2 : kSmallW);   // s080: a spread, both pages
            area->set_content_height(kSmallH);
            area->set_draw_func([area, thumbs](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                if (thumbs.size() == 1) paint_thumb(cr, *area, w, h, thumbs.front());
                else paint_spread(cr, *area, w, h, thumbs);
            });
            overlay->set_child(*area);
            if (!badge.empty()) {
                auto b = Gtk::make_managed<Gtk::Label>(badge);
                b->add_css_class("cx-badge");
                b->set_halign(Gtk::Align::END);
                b->set_valign(Gtk::Align::START);
                overlay->add_overlay(*b);
            }
            card->append(*overlay);
            auto title = Gtk::make_managed<Gtk::Label>(title_text);
            title->add_css_class("cx-tile-title");
            title->set_xalign(0.0f);
            title->set_ellipsize(Pango::EllipsizeMode::END);
            title->set_max_width_chars(1);
            card->append(*title);
            card->set_size_request(spreads ? kSmallW * 2 : kSmallW, -1);
        } else {
            card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            card->add_css_class("cx-tile-card");

            // Thumbnail, with the badge overlaid top-right.
            const int tw = spreads ? kSpreadW : kCardW;   // a spread's card holds two pages
            auto overlay = Gtk::make_managed<Gtk::Overlay>();
            auto area = Gtk::make_managed<Gtk::DrawingArea>();
            area->set_name("tiles.tile." + tile_id + ".thumb");
            area->add_css_class("cx-thumb");
            area->set_content_width(tw);
            area->set_content_height(kThumbH);
            area->set_draw_func([area, thumbs](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                if (thumbs.size() == 1) paint_thumb(cr, *area, w, h, thumbs.front());
                else paint_spread(cr, *area, w, h, thumbs);
            });
            overlay->set_child(*area);
            if (!badge.empty()) {
                auto b = Gtk::make_managed<Gtk::Label>(badge);
                b->add_css_class("cx-badge");
                if (flag) b->add_css_class("cx-warn");
                b->set_halign(Gtk::Align::END);
                b->set_valign(Gtk::Align::START);
                b->set_margin(6);
                overlay->add_overlay(*b);
            }
            card->append(*overlay);

            auto title = Gtk::make_managed<Gtk::Label>(title_text);
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
                // A spread: each page's value, when they differ ("B-Body / C-Chapter").
                std::string value = f.value;
                for (std::size_t at; (at = value.find('\t')) != std::string::npos;) value.replace(at, 1, " \u00d7 ");   // s066: a compact row ("6 x 9")
                for (std::size_t k = 1; k < group.size(); ++k)
                    for (const auto& g : group[k]->fields)
                        if (g.label == f.label && g.value != f.value) value += " / " + g.value;
                auto l = Gtk::make_managed<Gtk::Label>(f.label);
                l->add_css_class("cx-field-label");
                l->set_xalign(0.0f);
                auto v = Gtk::make_managed<Gtk::Label>(value);
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
            card->set_size_request(tw, -1);
        }

        child->set_child(*card);
        if (m_reorder) {
            // s014: drag pages to reorder -- the selection if this tile is in it.
            // s028: a spread tile drags (and takes drops for) both its pages.
            auto src = Gtk::DragSource::create();
            src->set_actions(Gdk::DragAction::MOVE);
            src->signal_prepare().connect([this, gids](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                auto ids = selected();
                if (std::find(ids.begin(), ids.end(), gids.front()) == ids.end()) ids = gids;
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
            // s062: also a master dropped on it (COPY): the page(s) take it.
            auto drop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::MOVE | Gdk::DragAction::COPY);
            const auto mark = [this, child](double x) {
                if (m_drag_ids.empty()) {   // not pages being moved: a master, onto this tile
                    child->add_css_class("cx-drop-on");
                    return Gdk::DragAction::COPY;
                }
                const bool before = x < child->get_width() / 2.0;
                child->remove_css_class(before ? "cx-drop-after" : "cx-drop-before");
                child->add_css_class(before ? "cx-drop-before" : "cx-drop-after");
                return Gdk::DragAction::MOVE;
            };
            drop->signal_enter().connect([mark](double x, double) { return mark(x); }, false);
            drop->signal_motion().connect([mark](double x, double) { return mark(x); }, false);
            drop->signal_leave().connect([child] {
                child->remove_css_class("cx-drop-before");
                child->remove_css_class("cx-drop-after");
                child->remove_css_class("cx-drop-on");
            });
            drop->signal_drop().connect([this, child, gids](const Glib::ValueBase& value, double x, double) {
                child->remove_css_class("cx-drop-before");
                child->remove_css_class("cx-drop-after");
                child->remove_css_class("cx-drop-on");
                if (m_drag_ids.empty()) {   // s062
                    Glib::Value<Glib::ustring> v;
                    v.init(value.gobj());
                    const std::string text = v.get();
                    const std::string tag = "composix-master:";
                    if (text.rfind(tag, 0) != 0) return false;
                    const std::string master = text.substr(tag.size());
                    auto pages = selected();
                    if (std::find(pages.begin(), pages.end(), gids.front()) == pages.end()) pages = gids;
                    Glib::signal_idle().connect_once([this, master, pages] { m_master_drop.emit(master, pages); });
                    return true;
                }
                const auto ids = m_drag_ids;
                m_drag_ids.clear();
                const bool before = x < child->get_width() / 2.0;
                const std::string target = before ? gids.front() : gids.back();
                // After the drop's own handling: the move rebuilds these tiles.
                Glib::signal_idle().connect_once([this, ids, target, before] { m_move.emit(ids, target, before); });
                return true;
            }, false);
            child->add_controller(drop);
        }
        if (s == core::Segment::Masters) {   // s062: drag a master onto pages
            auto src = Gtk::DragSource::create();
            src->set_actions(Gdk::DragAction::COPY);
            const std::string mid = gids.front();
            src->signal_prepare().connect([mid](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                Glib::Value<Glib::ustring> v;
                v.init(v.value_type());
                v.set("composix-master:" + mid);
                return Gdk::ContentProvider::create(v);
            }, false);
            src->signal_drag_begin().connect([src, card](const Glib::RefPtr<Gdk::Drag>&) {
                src->set_icon(Gtk::WidgetPaintable::create(*card), card->get_width() / 2, 30);
            });
            child->add_controller(src);
        }
        auto& grid = grid_for(it->group);
        grid.append(*child);
        m_id_of[child] = gids;
        bool keep_it = false;
        for (const auto& g : gids) keep_it = keep_it || std::find(keep.begin(), keep.end(), g) != keep.end();
        if (keep_it) grid.select_child(*child);
    }
    m_quiet = false;
    if (auto lg = log::get(log::Area::View))
        lg->info("tiles: {} ({})", core::segment_info(s).key, items.size());
}

std::vector<std::string> TileView::selected() const {
    std::vector<std::string> out;
    for (const auto* g : m_grids)
        for (auto* c = g->get_first_child(); c; c = c->get_next_sibling()) {
            auto* fc = dynamic_cast<const Gtk::FlowBoxChild*>(c);
            if (!fc || !fc->is_selected()) continue;
            if (auto it = m_id_of.find(fc); it != m_id_of.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        }
    return out;
}

}  // namespace composix::views
