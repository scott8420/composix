#include "views/Inspector.hpp"
#include "core/Doc.hpp"

#include <memory>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/separator.h>

namespace composix::views {

// s041: an entry for a typed value. Enter (or leaving it) sends what was
// typed, in points, through `send`; what doesn't read as a length in
// [lo, hi] is marked and not sent.
Gtk::Entry* Inspector::typed_entry(const std::string& name, const std::string& value, double unit, double lo, double hi,
                                   std::function<void(const std::string&)> send) {
    auto en = Gtk::make_managed<Gtk::Entry>();
    en->set_name(name);
    en->add_css_class("cx-field-typed");
    en->set_hexpand(true);
    en->set_width_chars(5);
    if (value == core::kMixed) { en->set_placeholder_text(core::kMixed); en->add_css_class("cx-mixed"); }
    else en->set_text(value);
    // What it last showed or sent: Enter, then leaving, sends once.
    auto shown = std::make_shared<std::string>(value == core::kMixed ? std::string{} : value);
    auto commit = [this, en, shown, unit, lo, hi, send] {
        if (m_painting) return;
        const std::string text = en->get_text();
        if (text == *shown || text.empty()) { en->remove_css_class("error"); return; }
        const auto v = core::parse_length(text, unit);
        if (!v || *v < lo - 1e-9 || *v > hi + 1e-9) {
            en->add_css_class("error");
            en->set_tooltip_text("Type a length -- 12, 12 pt, 1.5 in, 3p6, 10 mm");
            return;
        }
        en->remove_css_class("error");
        *shown = text;
        send(core::num_text(*v));
    };
    en->signal_activate().connect(commit);
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect(commit);
    en->add_controller(focus);
    return en;
}

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

// s041: a typed value's presets, in a menu (s067: shared with compact rows).
Gtk::MenuButton* Inspector::presets_button(const std::string& key, const std::vector<core::Choice>& choices) {
    auto mb = Gtk::make_managed<Gtk::MenuButton>();
    mb->set_name("inspector.presets." + key);
    mb->set_icon_name("pan-down-symbolic");
    mb->set_tooltip_text("Presets");
    auto pop = Gtk::make_managed<Gtk::Popover>();
    auto list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    for (const auto& c : choices) {
        auto b = Gtk::make_managed<Gtk::Button>(c.label);
        b->add_css_class("flat");
        if (auto* lab = dynamic_cast<Gtk::Label*>(b->get_child())) lab->set_xalign(0.0f);
        const std::string id = c.id;
        b->signal_clicked().connect([this, pop, key, id] { pop->popdown(); m_edit.emit(key, id); });
        list->append(*b);
    }
    pop->set_child(*list);
    mb->set_popover(*pop);
    return mb;
}

void Inspector::fill(const std::string& title, const std::string& sub,
                     const std::vector<core::Field>& fields) {
    if (m_has_shown && title == m_shown_title && sub == m_shown_sub && fields == m_shown) return;
    m_has_shown = true;
    m_shown_title = title;
    m_shown_sub = sub;
    m_shown = fields;
    m_painting = true;   // s041: a typed entry losing focus as it goes is not an edit
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
            // s067: four sides with a link -- linked when they all agree.
            auto linked = std::make_shared<bool>(false);
            if (f.link) {
                *linked = vals.size() == f.cols.size() && vals[0] != core::kMixed;
                for (const auto& v : vals) *linked = *linked && v == vals[0];
            }
            static const char* side_names[] = {"Top", "Right", "Bottom", "Left"};
            for (std::size_t k = 0; k < f.cols.size(); ++k) {
                auto h = Gtk::make_managed<Gtk::Label>(f.cols[k]);
                h->add_css_class("cx-col-head");
                h->set_xalign(0.0f);
                g->attach(*h, static_cast<int>(k), 0);
                if (!f.edit.empty() && f.unit > 0) {   // s041: each column typed
                    const std::string key = f.edit, idx = std::to_string(k);
                    auto en = typed_entry("inspector.edit." + key + "." + idx, k < vals.size() ? vals[k] : "", f.unit, f.lo, f.hi,
                                          [this, key, idx, linked](const std::string& v) {
                                              m_edit.emit(key, (*linked ? std::string("a") : idx) + ":" + v);
                                          });
                    // Room for the link / presets beside them without widening the panel.
                    en->set_width_chars(f.link || !f.choices.empty() ? 3 : 4);
                    if (f.link && k < 4) en->set_tooltip_text(side_names[k]);
                    g->attach(*en, static_cast<int>(k), 1);
                    continue;
                }
                auto v = Gtk::make_managed<Gtk::Label>(k < vals.size() ? vals[k] : "");
                v->add_css_class("cx-field-value");
                if (k < vals.size() && vals[k] == core::kMixed) v->add_css_class("cx-mixed");
                v->set_xalign(0.0f);
                v->set_can_focus(false);
                g->attach(*v, static_cast<int>(k), 1);
            }
            if (!f.link && f.choices.empty()) {
                m_fields.attach(*g, 1, row);
                ++row;
                continue;
            }
            // s067: the link and / or the presets, beside the values.
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            box->set_hexpand(true);
            box->append(*g);
            const std::string key = f.edit;
            if (f.link) {
                auto tb = Gtk::make_managed<Gtk::ToggleButton>();
                tb->set_name("inspector.link." + key);
                tb->set_icon_name("insert-link-symbolic");
                tb->add_css_class("flat");
                tb->set_valign(Gtk::Align::END);
                tb->set_tooltip_text("Same all round");
                tb->add_css_class("cx-insp-mini");
                tb->set_active(*linked);
                const std::string first = vals[0];
                const bool even = *linked;
                tb->signal_toggled().connect([this, tb, linked, key, first, even] {
                    if (m_painting) return;
                    *linked = tb->get_active();
                    // Linking sides that differ makes all four the first (top).
                    if (*linked && !even && first != core::kMixed && !first.empty()) m_edit.emit(key, "a:" + first);
                });
                box->append(*tb);
            }
            if (!f.choices.empty() && !f.link) {   // a linked row: typing in one column IS the preset
                auto mb = presets_button(key, f.choices);
                mb->set_valign(Gtk::Align::END);
                mb->add_css_class("cx-insp-mini");
                box->append(*mb);
            }
            m_fields.attach(*box, 1, row);
            ++row;
            continue;
        }
        auto l = Gtk::make_managed<Gtk::Label>(f.label);
        l->add_css_class("cx-field-label");
        l->set_xalign(1.0f);
        l->set_valign(f.edit.empty() ? Gtk::Align::START : Gtk::Align::CENTER);
        m_fields.attach(*l, 0, row);

        if (!f.edit.empty() && f.text) {
            // s060: typed text -- sent as typed on Enter or leaving it.
            auto en = Gtk::make_managed<Gtk::Entry>();
            const std::string key = f.edit;
            en->set_name("inspector.edit." + key);
            en->add_css_class("cx-field-typed");
            en->set_hexpand(true);
            en->set_width_chars(8);
            if (f.value == core::kMixed) { en->set_placeholder_text(core::kMixed); en->add_css_class("cx-mixed"); }
            else en->set_text(f.value);
            auto shown = std::make_shared<std::string>(f.value == core::kMixed ? std::string{} : f.value);
            auto commit = [this, en, shown, key] {
                if (m_painting) return;
                const std::string text = en->get_text();
                if (text == *shown || text.empty()) return;
                *shown = text;
                m_edit.emit(key, text);
            };
            en->signal_activate().connect(commit);
            auto focus = Gtk::EventControllerFocus::create();
            focus->signal_leave().connect(commit);
            en->add_controller(focus);
            m_fields.attach(*en, 1, row);
            ++row;
            continue;
        }
        if (!f.edit.empty() && f.unit > 0) {
            // s041: a typed value, its presets in a menu beside it.
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            box->add_css_class("linked");
            box->set_hexpand(true);
            const std::string key = f.edit;
            box->append(*typed_entry("inspector.edit." + key, f.value, f.unit, f.lo, f.hi,
                                     [this, key](const std::string& v) { m_edit.emit(key, v); }));
            if (!f.choices.empty()) box->append(*presets_button(key, f.choices));
            m_fields.attach(*box, 1, row);
        } else if (!f.edit.empty()) {
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
            // A long list (s011: the installed fonts) can be searched by typing.
            if (labels.size() > 24) {
                GtkExpression* ex = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
                gtk_drop_down_set_expression(dd->gobj(), ex);
                gtk_expression_unref(ex);
                gtk_drop_down_set_enable_search(dd->gobj(), TRUE);
            }
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
    m_painting = true;
    while (auto* c = m_fields.get_first_child()) m_fields.remove(*c);
    m_painting = false;
    m_title.set_visible(false);
    m_sub.set_visible(false);
    m_scroll.set_visible(false);
    m_empty.set_visible(true);
}

}  // namespace composix::views
