#include "views/Navigator.hpp"
#include "Log.hpp"

#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>

#include <algorithm>

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

Navigator::SecRow* Navigator::sec(core::Section s) {
    for (auto& r : m_secs)
        if (r.section == s) return &r;
    return nullptr;
}

void Navigator::set_outline(const core::Outline* o) {
    m_quiet = true;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    m_secs.clear();
    m_item_of.clear();
    m_sec_of.clear();
    m_outline = o;

    if (o) {
        for (const auto& si : core::sections()) {
            const auto items = o->in(si.id);

            // ── the section row ──────────────────────────────────────────
            auto row = Gtk::make_managed<Gtk::ListBoxRow>();
            row->set_name("nav.section." + si.key);
            row->add_css_class("cx-section-row");
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

            auto arrow = Gtk::make_managed<Gtk::Button>();
            arrow->set_name("nav.section." + si.key + ".arrow");
            arrow->set_icon_name("pan-end-symbolic");
            arrow->set_has_frame(false);
            arrow->set_focus_on_click(false);
            arrow->add_css_class("cx-disclose");
            arrow->set_tooltip_text("Show / hide the items");
            const core::Section s = si.id;
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
                if (!it->badge.empty()) {
                    auto b = Gtk::make_managed<Gtk::Label>(it->badge);
                    b->add_css_class("cx-badge");
                    if (it->thumb.flag) b->add_css_class("cx-warn");
                    ib->append(*b);
                }
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

bool Navigator::is_open(core::Section s) const {
    for (const auto& r : m_secs)
        if (r.section == s) return r.open;
    return false;
}

void Navigator::set_open(core::Section s, bool open) {
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

void Navigator::select_section(core::Section s) {
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
    if (m_quiet) return;
    Pick p;
    std::optional<core::Section> first_sec;
    // Walk in list order, not selection order, so a pick reads top-to-bottom.
    for (auto* c = m_list.get_first_child(); c; c = c->get_next_sibling()) {
        auto* row = dynamic_cast<Gtk::ListBoxRow*>(c);
        if (!row || !row->is_selected()) continue;
        if (auto it = m_item_of.find(row); it != m_item_of.end()) p.items.push_back(it->second);
        else if (auto s = m_sec_of.find(row); s != m_sec_of.end() && !first_sec) first_sec = s->second;
    }
    if (p.items.empty()) p.section = first_sec;
    if (auto lg = log::get(log::Area::View))
        lg->info("nav pick: {} {}", p.section ? core::section_info(*p.section).key : "-",
                 p.items.size());
    m_pick.emit(p);
}

}  // namespace composix::views
