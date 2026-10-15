#include "views/Navigator.hpp"
#include "Log.hpp"

#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>

#include <algorithm>
#include <glibmm/main.h>

namespace composix::views {

Navigator::Navigator()
    : widgets::Box("nav", Gtk::Orientation::VERTICAL),
      m_scroll("nav.scroll"),
      m_list("nav.list") {
    add_css_class("cx-nav");
    set_size_request(232, -1);

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
                ir->set_visible(false);
                m_list.append(*ir);
                m_item_of[ir] = it->id;
                sr.items.push_back(ir);
            }
            m_secs.push_back(sr);
        }
    }
    m_quiet = false;
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
