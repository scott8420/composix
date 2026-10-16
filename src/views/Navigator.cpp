#include "views/Navigator.hpp"
#include "Log.hpp"

#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/widgetpaintable.h>
#include <gdkmm/contentprovider.h>

#include <algorithm>
#include <glibmm/main.h>

namespace composix::views {

Navigator::Navigator()
    : widgets::Box("nav", Gtk::Orientation::VERTICAL),
      m_scroll("nav.scroll"),
      m_list("nav.list") {
    add_css_class("cx-nav");

    auto heading = Gtk::make_managed<Gtk::Label>("NAVIGATOR");
    heading->set_name("nav.heading");
    heading->set_xalign(0.0f);
    heading->add_css_class("cx-pane-heading");
    append(*heading);

    m_list.set_selection_mode(Gtk::SelectionMode::MULTIPLE);
    m_list.set_activate_on_single_click(false);
    m_list.add_css_class("cx-nav-list");
    m_list.signal_selected_rows_changed().connect(
        sigc::mem_fun(*this, &Navigator::on_selection_changed));

    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_list);
    append(m_scroll);
}

Navigator::SecRow* Navigator::sec(core::Segment s) {
    for (auto& r : m_secs)
        if (r.segment == s) return &r;
    return nullptr;
}

void Navigator::set_outline(const core::Outline* o) {
    m_quiet = true;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    m_secs.clear();
    m_item_of.clear();
    m_sec_of.clear();
    m_labels.clear();
    m_counts.clear();
    m_outline = o;

    if (o) {
        for (const auto& si : core::segments()) {
            const auto items = o->in(si.id);

            // ── the section row ──────────────────────────────────────────
            auto row = Gtk::make_managed<Gtk::ListBoxRow>();
            row->set_name("nav.segment." + si.key);
            row->add_css_class("cx-section-row");
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

            auto arrow = Gtk::make_managed<Gtk::Button>();
            arrow->set_name("nav.segment." + si.key + ".arrow");
            arrow->set_icon_name("pan-end-symbolic");
            arrow->set_has_frame(false);
            arrow->set_focus_on_click(false);
            arrow->add_css_class("cx-disclose");
            arrow->set_tooltip_text("Show / hide the items");
            const core::Segment s = si.id;
            arrow->signal_clicked().connect([this, s] { set_open(s, !is_open(s)); });

            auto icon = Gtk::make_managed<Gtk::Image>();
            icon->set_from_icon_name(si.icon);
            icon->add_css_class("cx-section-icon");
            auto title = Gtk::make_managed<Gtk::Label>(si.title);
            title->set_xalign(0.0f);
            title->set_hexpand(true);
            title->add_css_class("cx-section-title");
            auto count = Gtk::make_managed<Gtk::Label>(std::to_string(items.size()));
            count->add_css_class("cx-count");

            box->append(*arrow);
            box->append(*icon);
            box->append(*title);
            box->append(*count);
            m_counts[row] = count;
            row->set_child(*box);
            m_list.append(*row);

            SecRow sr{s, row, arrow, false, {}};
            m_sec_of[row] = s;

            // ── its item rows ────────────────────────────────────────────
            for (const auto* it : items) {
                auto ir = Gtk::make_managed<Gtk::ListBoxRow>();
                ir->set_name("nav.item." + it->id);
                ir->add_css_class("cx-item-row");
                auto ib = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
                auto il = Gtk::make_managed<Gtk::Label>(it->title);
                il->set_xalign(0.0f);
                il->set_hexpand(true);
                il->set_ellipsize(Pango::EllipsizeMode::END);
                ib->append(*il);
                // The badge always exists (hidden when empty) so a refresh
                // can update it in place.
                auto b = Gtk::make_managed<Gtk::Label>(it->badge);
                b->add_css_class("cx-badge");
                if (it->thumb.flag) b->add_css_class("cx-warn");
                b->set_visible(!it->badge.empty());
                ib->append(*b);
                m_labels[ir] = {il, b};
                ir->set_child(*ib);
                // s062: a master's row drags onto pages; a page's row takes one.
                if (s == core::Segment::Masters) {
                    auto src = Gtk::DragSource::create();
                    src->set_actions(Gdk::DragAction::COPY);
                    const std::string mid = it->id;
                    src->signal_prepare().connect([mid](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                        Glib::Value<Glib::ustring> v;
                        v.init(v.value_type());
                        v.set("composix-master:" + mid);
                        return Gdk::ContentProvider::create(v);
                    }, false);
                    // The row's name rides with the pointer, not the payload's text.
                    src->signal_drag_begin().connect([src, ib](const Glib::RefPtr<Gdk::Drag>&) {
                        src->set_icon(Gtk::WidgetPaintable::create(*ib), 12, 10);
                    });
                    ir->add_controller(src);
                } else if (s == core::Segment::Pages) {
                    auto drop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::COPY);
                    drop->signal_enter().connect([ir](double, double) { ir->add_css_class("cx-drop-on"); return Gdk::DragAction::COPY; }, false);
                    drop->signal_leave().connect([ir] { ir->remove_css_class("cx-drop-on"); });
                    const std::string pid = it->id;
                    drop->signal_drop().connect([this, ir, pid](const Glib::ValueBase& value, double, double) {
                        ir->remove_css_class("cx-drop-on");
                        Glib::Value<Glib::ustring> v;
                        v.init(value.gobj());
                        const std::string text = v.get(), tag = "composix-master:";
                        if (text.rfind(tag, 0) != 0) return false;
                        // The selected pages if this one is among them, else this one.
                        std::vector<std::string> pages;
                        if (ir->is_selected())
                            for (const auto& [row, id] : m_item_of) if (row->is_selected() && row->get_visible()) pages.push_back(id);
                        if (pages.empty()) pages = {pid};
                        const std::string master = text.substr(tag.size());
                        Glib::signal_idle().connect_once([this, master, pages] { m_master_drop.emit(master, pages); });
                        return true;
                    }, false);
                    ir->add_controller(drop);
                }
                ir->set_visible(false);
                m_list.append(*ir);
                m_item_of[ir] = it->id;
                sr.items.push_back(ir);
            }
            m_secs.push_back(sr);
        }
    }
    m_quiet = false;
    mark_spreads();
}

// s031: the rows of a two-page spread bracketed together (a line down their
// left, the pair's ends rounded), and air before each spread.
void Navigator::mark_spreads() {
    const auto* r = sec(core::Segment::Pages);
    if (!r) return;
    const auto groups = m_grouping ? m_grouping() : std::vector<std::vector<std::string>>{};
    std::map<std::string, std::pair<std::size_t, std::size_t>> where;   // page -> (index in its spread, spread size)
    for (const auto& g : groups)
        for (std::size_t i = 0; i < g.size(); ++i) where[g[i]] = {i, g.size()};
    for (std::size_t k = 0; k < r->items.size(); ++k) {
        auto* row = r->items[k];
        for (const char* c : {"cx-spread-top", "cx-spread-bottom", "cx-spread-gap"}) row->remove_css_class(c);
        const auto it = where.find(m_item_of[row]);
        if (it == where.end()) continue;
        const auto [i, n] = it->second;
        if (n == 2) row->add_css_class(i == 0 ? "cx-spread-top" : "cx-spread-bottom");
        if (i == 0 && k > 0) row->add_css_class("cx-spread-gap");
    }
}

// The document changed. Most changes keep the same items in the same order
// (an edit, a move, a guide): update titles, badges and counts IN PLACE --
// never touching the rows keeps GTK's cursor and selection exactly as they
// were. Only when items come or go are the rows rebuilt, and then the picks
// are put back twice: now, and once GTK has settled (removing the focused
// row makes GtkListBox move its cursor later, which re-selects one row on its
// own -- s006, found driving a guide drop).
void Navigator::refresh(const core::Outline* o) {
    bool same = o && m_outline && !m_secs.empty();
    if (same)
        for (const auto& r : m_secs) {
            const auto items = o->in(r.segment);
            if (items.size() != r.items.size()) { same = false; break; }
            for (std::size_t i = 0; i < items.size() && same; ++i)
                same = m_item_of[r.items[i]] == items[i]->id;
            if (!same) break;
        }
    if (same) {
        m_outline = o;
        for (const auto& r : m_secs) {
            const auto items = o->in(r.segment);
            if (auto c = m_counts.find(r.row); c != m_counts.end()) c->second->set_text(std::to_string(items.size()));
            for (std::size_t i = 0; i < items.size(); ++i) {
                auto& [title, badge] = m_labels[r.items[i]];
                if (title->get_text().raw() != items[i]->title) title->set_text(items[i]->title);
                badge->set_text(items[i]->badge);
                badge->set_visible(!items[i]->badge.empty());
                if (items[i]->thumb.flag) badge->add_css_class("cx-warn");
                else                      badge->remove_css_class("cx-warn");
            }
        }
        mark_spreads();   // sides may have moved (a page added, a section)
        return;
    }

    std::vector<core::Segment> open, picked_segs;
    std::vector<std::string> picked;
    for (const auto& r : m_secs) {
        if (r.open) open.push_back(r.segment);
        if (r.row->is_selected()) picked_segs.push_back(r.segment);
        for (auto* ir : r.items)
            if (ir->is_selected()) picked.push_back(m_item_of[ir]);
    }
    set_outline(o);
    const auto restore = [this, open, picked_segs, picked] {
        m_quiet = true;
        m_list.unselect_all();
        for (auto& r : m_secs) {
            if (std::find(open.begin(), open.end(), r.segment) != open.end() && !r.open) {
                r.open = true;
                r.arrow->set_icon_name("pan-down-symbolic");
                r.row->add_css_class("cx-open");
                for (auto* ir : r.items) ir->set_visible(true);
            }
            if (std::find(picked_segs.begin(), picked_segs.end(), r.segment) != picked_segs.end())
                m_list.select_row(*r.row);
            for (auto* ir : r.items)
                if (std::find(picked.begin(), picked.end(), m_item_of[ir]) != picked.end()) m_list.select_row(*ir);
        }
        m_quiet = false;
    };
    restore();
    m_settling = true;
    Glib::signal_idle().connect_once([this, restore] {
        restore();
        m_settling = false;
    });
}

bool Navigator::is_open(core::Segment s) const {
    for (const auto& r : m_secs)
        if (r.segment == s) return r.open;
    return false;
}

void Navigator::set_open(core::Segment s, bool open) {
    auto* r = sec(s);
    if (!r || r->open == open) return;
    r->open = open;
    r->arrow->set_icon_name(open ? "pan-down-symbolic" : "pan-end-symbolic");
    if (open) r->row->add_css_class("cx-open");
    else      r->row->remove_css_class("cx-open");
    m_quiet = true;
    bool dropped = false;
    for (auto* ir : r->items) {
        if (!open && ir->is_selected()) { m_list.unselect_row(ir); dropped = true; }
        ir->set_visible(open);
    }
    m_quiet = false;
    if (dropped) on_selection_changed();   // a folded-away item is no longer picked
}

void Navigator::select_section(core::Segment s) {
    auto* r = sec(s);
    if (!r) return;
    m_quiet = true;
    m_list.unselect_all();
    m_list.select_row(*r->row);
    m_quiet = false;
    on_selection_changed();
}

void Navigator::select_items(const std::vector<std::string>& ids) {
    m_quiet = true;
    m_list.unselect_all();
    Gtk::ListBoxRow* first = nullptr;
    for (auto& r : m_secs)
        for (auto* ir : r.items)
            if (std::find(ids.begin(), ids.end(), m_item_of[ir]) != ids.end()) {
                if (!r.open) {   // disclose the section so the selection is visible
                    m_quiet = true;
                    r.open = true;
                    r.arrow->set_icon_name("pan-down-symbolic");
                    r.row->add_css_class("cx-open");
                    for (auto* x : r.items) x->set_visible(true);
                }
                m_list.select_row(*ir);
                if (!first) first = ir;
            }
    m_quiet = false;
    if (first) first->grab_focus();
    on_selection_changed();
}

void Navigator::on_selection_changed() {
    if (m_quiet || m_settling) return;   // ours, or GTK settling after a rebuild
    Pick p;
    std::optional<core::Segment> first_seg;
    // Walk in list order, not selection order, so a pick reads top-to-bottom.
    for (auto* c = m_list.get_first_child(); c; c = c->get_next_sibling()) {
        auto* row = dynamic_cast<Gtk::ListBoxRow*>(c);
        if (!row || !row->is_selected()) continue;
        if (auto it = m_item_of.find(row); it != m_item_of.end()) p.items.push_back(it->second);
        else if (auto s = m_sec_of.find(row); s != m_sec_of.end() && !first_seg) first_seg = s->second;
    }
    if (p.items.empty()) p.segment = first_seg;
    if (auto lg = log::get(log::Area::View))
        lg->info("nav pick: {} {}", p.segment ? core::segment_info(*p.segment).key : "-",
                 p.items.size());
    m_pick.emit(p);
}

}  // namespace composix::views
