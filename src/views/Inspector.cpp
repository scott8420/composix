#include "views/Inspector.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/separator.h>

namespace composix::views {

Inspector::Inspector()
    : widgets::Box("inspector", Gtk::Orientation::VERTICAL),
      m_title("inspector.title"),
      m_sub("inspector.sub"),
      m_scroll("inspector.scroll"),
      m_fields("inspector.fields"),
      m_empty("inspector.empty") {
    add_css_class("cx-inspector");

    auto heading = Gtk::make_managed<Gtk::Label>("INSPECTOR");
    heading->set_name("inspector.heading");
    heading->set_xalign(0.0f);
    heading->add_css_class("cx-pane-heading");
    append(*heading);

    m_title.add_css_class("cx-insp-title");
    m_title.set_xalign(0.0f);
    m_title.set_wrap(true);
    m_sub.add_css_class("cx-insp-sub");
    m_sub.set_xalign(0.0f);
    append(m_title);
    append(m_sub);

    m_fields.add_css_class("cx-insp-fields");
    m_fields.set_column_spacing(12);
    m_fields.set_row_spacing(2);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_fields);
    append(m_scroll);

    m_empty.add_css_class("cx-insp-empty");
    m_empty.set_text("Select something in the navigator or the tiles.");
    m_empty.set_wrap(true);
    m_empty.set_vexpand(true);
    m_empty.set_valign(Gtk::Align::START);
    append(m_empty);
    clear();
}

void Inspector::fill(const std::string& title, const std::string& sub,
                     const std::vector<core::Field>& fields) {
    if (m_has_shown && title == m_shown_title && sub == m_shown_sub && fields == m_shown) return;
    m_has_shown = true;
    m_shown_title = title;
    m_shown_sub = sub;
    m_shown = fields;
    while (auto* c = m_fields.get_first_child()) m_fields.remove(*c);
    m_title.set_text(title);
    m_sub.set_text(sub);
    m_title.set_visible(true);
    m_sub.set_visible(!sub.empty());
    m_empty.set_visible(false);
    m_scroll.set_visible(true);
    m_painting = true;
    int row = 0;
    for (const auto& f : fields) {
        if (f.buttons) {   // s009: actions -- a row of buttons under the fields
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            box->set_name("inspector.actions." + f.edit);
            box->add_css_class("cx-insp-actions");
            for (const auto& c : f.choices) {
                auto b = Gtk::make_managed<Gtk::Button>(c.label);
                b->set_name("inspector.act." + f.edit + "." + c.id);
                b->add_css_class("cx-insp-action");
                const std::string key = f.edit, id = c.id;
                b->signal_clicked().connect([this, key, id] { m_edit.emit(key, id); });
                box->append(*b);
            }
            m_fields.attach(*box, 0, row, 2, 1);
            ++row;
            continue;
        }
        if (!f.cols.empty()) {   // a compact row: headers over values, under the label
            auto l = Gtk::make_managed<Gtk::Label>(f.label);
            l->add_css_class("cx-field-label");
            l->set_xalign(1.0f);
            l->set_valign(Gtk::Align::END);
            m_fields.attach(*l, 0, row);
            auto g = Gtk::make_managed<Gtk::Grid>();
            g->set_name("inspector.cols");
            g->add_css_class("cx-insp-cols");
            g->set_column_homogeneous(true);
            g->set_hexpand(true);
            std::vector<std::string> vals(1);
            for (char c : f.value) { if (c == '\t') vals.emplace_back(); else vals.back() += c; }
            for (std::size_t k = 0; k < f.cols.size(); ++k) {
                auto h = Gtk::make_managed<Gtk::Label>(f.cols[k]);
                h->add_css_class("cx-col-head");
                h->set_xalign(0.0f);
                g->attach(*h, static_cast<int>(k), 0);
                auto v = Gtk::make_managed<Gtk::Label>(k < vals.size() ? vals[k] : "");
                v->add_css_class("cx-field-value");
                if (k < vals.size() && vals[k] == core::kMixed) v->add_css_class("cx-mixed");
                v->set_xalign(0.0f);
                v->set_can_focus(false);
                g->attach(*v, static_cast<int>(k), 1);
            }
            m_fields.attach(*g, 1, row);
            ++row;
            continue;
        }
        auto l = Gtk::make_managed<Gtk::Label>(f.label);
        l->add_css_class("cx-field-label");
        l->set_xalign(1.0f);
        l->set_valign(f.edit.empty() ? Gtk::Align::START : Gtk::Align::CENTER);
        m_fields.attach(*l, 0, row);

        if (!f.edit.empty()) {
            // An editable field: a dropdown of its choices.
            std::vector<Glib::ustring> labels;
            guint at = GTK_INVALID_LIST_POSITION;
            for (std::size_t i = 0; i < f.choices.size(); ++i) {
                labels.push_back(f.choices[i].label);
                // The shown value is the chosen label (or the size it reads as).
                if (f.choices[i].label == f.value || f.choices[i].label.find("(" + f.value + ")") != std::string::npos)
                    at = static_cast<guint>(i);
            }
            // No choice matches (Mixed, or a size that isn't a preset): show
            // the value itself as a first entry, so GTK doesn't print "(None)".
            // Picking it does nothing; every real choice is one down.
            guint offset = 0;
            if (at == GTK_INVALID_LIST_POSITION) {
                labels.insert(labels.begin(), f.value);
                at = 0;
                offset = 1;
            }
            auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(labels));
            dd->set_name("inspector.edit." + f.edit);
            dd->add_css_class("cx-field-edit");
            dd->set_hexpand(true);
            dd->set_selected(at);
            if (f.value == core::kMixed) {
                dd->add_css_class("cx-mixed");
                dd->set_tooltip_text("Mixed \u2014 pick one to set all of them");
            }
            const std::string key = f.edit;
            const auto choices = f.choices;
            dd->property_selected().signal_changed().connect([this, dd, key, choices, offset] {
                if (m_painting) return;
                const guint i = dd->get_selected();
                if (i < offset || i == GTK_INVALID_LIST_POSITION) return;
                if (i - offset < choices.size()) m_edit.emit(key, choices[i - offset].id);
            });
            m_fields.attach(*dd, 1, row);
        } else {
            auto v = Gtk::make_managed<Gtk::Label>(f.value);
            v->add_css_class("cx-field-value");
            if (f.value == core::kMixed) v->add_css_class("cx-mixed");
            v->set_xalign(0.0f);
            v->set_hexpand(true);
            v->set_wrap(true);
            // Not selectable: a selectable label takes the focus when the
            // dropdown that had it is rebuilt away, and shows up highlighted.
            v->set_can_focus(false);
            m_fields.attach(*v, 1, row);
        }
        ++row;
    }
    m_painting = false;
}

void Inspector::show_items(const std::vector<const core::Item*>& sel) {
    if (sel.empty()) { clear(); return; }
    std::string sub;
    if (sel.size() == 1) sub = core::segment_info(sel.front()->segment).title;
    else                 sub = "Shared fields — Mixed where they differ";
    fill(core::selection_title(sel), sub, core::shared_fields(sel));
}

void Inspector::show_section(const core::Outline& o, core::Segment s) {
    const auto n = o.in(s).size();
    fill(core::segment_info(s).title, std::to_string(n) + (n == 1 ? " item" : " items"), {});
}

void Inspector::clear() {
    m_has_shown = false;
    while (auto* c = m_fields.get_first_child()) m_fields.remove(*c);
    m_title.set_visible(false);
    m_sub.set_visible(false);
    m_scroll.set_visible(false);
    m_empty.set_visible(true);
}

}  // namespace composix::views
