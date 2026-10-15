#include "views/Inspector.hpp"

#include <gtkmm/label.h>
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
    while (auto* c = m_fields.get_first_child()) m_fields.remove(*c);
    m_title.set_text(title);
    m_sub.set_text(sub);
    m_title.set_visible(true);
    m_sub.set_visible(!sub.empty());
    m_empty.set_visible(false);
    m_scroll.set_visible(true);
    int row = 0;
    for (const auto& f : fields) {
        auto l = Gtk::make_managed<Gtk::Label>(f.label);
        l->add_css_class("cx-field-label");
        l->set_xalign(1.0f);
        l->set_valign(Gtk::Align::START);
        auto v = Gtk::make_managed<Gtk::Label>(f.value);
        v->add_css_class("cx-field-value");
        if (f.value == core::kMixed) v->add_css_class("cx-mixed");
        v->set_xalign(0.0f);
        v->set_hexpand(true);
        v->set_wrap(true);
        v->set_selectable(true);
        m_fields.attach(*l, 0, row);
        m_fields.attach(*v, 1, row);
        ++row;
    }
}

void Inspector::show_items(const std::vector<const core::Item*>& sel) {
    if (sel.empty()) { clear(); return; }
    std::string sub;
    if (sel.size() == 1) sub = core::section_info(sel.front()->section).title;
    else                 sub = "Shared fields — Mixed where they differ";
    fill(core::selection_title(sel), sub, core::shared_fields(sel));
}

void Inspector::show_section(const core::Outline& o, core::Section s) {
    const auto n = o.in(s).size();
    fill(core::section_info(s).title, std::to_string(n) + (n == 1 ? " item" : " items"), {});
}

void Inspector::clear() {
    while (auto* c = m_fields.get_first_child()) m_fields.remove(*c);
    m_title.set_visible(false);
    m_sub.set_visible(false);
    m_scroll.set_visible(false);
    m_empty.set_visible(true);
}

}  // namespace composix::views
