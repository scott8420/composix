#include "views/Inspector.hpp"
#include <gtkmm/flowbox.h>
#include "core/Doc.hpp"

#include <memory>
#include <algorithm>
#include <glibmm/main.h>

#include <gtkmm/alertdialog.h>
#include <gtkmm/window.h>
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
#include <gtkmm/stringobject.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/listitem.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/separator.h>

namespace composix::views {

namespace {
// s152: Ctrl+Z in a typed field. The field took the window's Ctrl+Z for
// itself (a text box keeps its own), so after Enter the next Ctrl+Z undid
// nothing the user could see. Now: a field with an edit not yet sent puts
// back what it showed; one that matches what it shows hands Ctrl+Z (and
// Ctrl+Shift+Z / Ctrl+Y) to the document. Heard in the CAPTURE phase,
// before the entry's own text undo.
void undo_goes_to_document(Gtk::Entry* en, std::shared_ptr<std::string> shown) {
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([en, shown](guint kv, guint, Gdk::ModifierType state) {
        const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::ALT_MASK);
        const auto CTRL = Gdk::ModifierType::CONTROL_MASK, SHIFT = Gdk::ModifierType::SHIFT_MASK;
        const bool z = kv == GDK_KEY_z || kv == GDK_KEY_Z, y = kv == GDK_KEY_y || kv == GDK_KEY_Y;
        const bool undo = z && mods == CTRL;
        const bool redo = (z && mods == (CTRL | SHIFT)) || (y && mods == CTRL);
        if (!undo && !redo) return false;
        const std::string text = en->get_text();
        const bool edited = text != *shown;
        if (edited) {
            if (!undo) return false;           // redo inside an edit: the box's own
            en->set_text(*shown);              // the edit dropped, the value back
            en->remove_css_class("error");
            en->set_position(-1);
            return true;
        }
        en->activate_action(undo ? "win.undo" : "win.redo");
        return true;
    }, false);
    en->add_controller(keys);
}

// s152: a dropdown's labels. On the button, ellipsized at the end, so a long
// one (a foldout's Size) no longer widens the panel -- its whole text is the
// tooltip; in the list, whole.
Glib::RefPtr<Gtk::SignalListItemFactory> label_factory(bool ellipsize) {
    auto f = Gtk::SignalListItemFactory::create();
    f->signal_setup().connect([ellipsize](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto l = Gtk::make_managed<Gtk::Label>();
        l->set_xalign(0.0f);
        if (ellipsize) { l->set_ellipsize(Pango::EllipsizeMode::END); l->set_width_chars(1); }
        li->set_child(*l);
    });
    f->signal_bind().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
        auto* l = dynamic_cast<Gtk::Label*>(li->get_child());
        auto so = std::dynamic_pointer_cast<Gtk::StringObject>(li->get_item());
        if (l && so) l->set_text(so->get_string());
    });
    return f;
}
}  // namespace

// s041: an entry for a typed value. Enter (or leaving it) sends what was
// typed, in points, through `send`; what doesn't read as a length in
// [lo, hi] is marked and not sent.
Gtk::Entry* Inspector::typed_entry(const std::string& name, const std::string& value, double unit, double lo, double hi,
                                   std::function<void(const std::string&)> send, bool percent) {
    auto en = Gtk::make_managed<Gtk::Entry>();
    en->set_name(name);
    en->add_css_class("cx-field-typed");
    en->set_hexpand(true);
    en->set_width_chars(5);
    if (value == core::kMixed) { en->set_placeholder_text(core::kMixed); en->add_css_class("cx-mixed"); }
    else en->set_text(value);
    // What it last showed or sent: Enter, then leaving, sends once.
    auto shown = std::make_shared<std::string>(value == core::kMixed ? std::string{} : value);
    auto commit = [this, en, shown, unit, lo, hi, send, percent] {
        if (m_painting) return;
        const std::string text = en->get_text();
        if (text == *shown || text.empty()) { en->remove_css_class("error"); return; }
        const auto v = percent ? core::parse_percent(text) : core::parse_length(text, unit);
        if (!v || *v < lo - 1e-9 || *v > hi + 1e-9) {
            en->add_css_class("error");
            en->set_tooltip_text(percent ? "Type a percentage -- " + core::num_text(lo) + " to " + core::num_text(hi) + ", as 40 or 40%"
                                         : std::string("Type a length -- 12, 12 pt, 1.5 in, 3p6, 10 mm"));
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
    undo_goes_to_document(en, shown);   // s152
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

    // s162: Set / Reset for a style's waiting changes -- under the rows, always in view.
    m_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_bar->set_name("inspector.stage_bar");
    m_bar->add_css_class("cx-stage-bar");
    m_bar_text = Gtk::make_managed<Gtk::Label>();
    m_bar_text->add_css_class("cx-stage-text");
    m_bar_text->set_hexpand(true);
    m_bar_text->set_xalign(0.0f);
    auto reset = Gtk::make_managed<Gtk::Button>("Reset");
    reset->set_name("inspector.stage_reset");
    reset->set_tooltip_text("Drop the changes waiting");
    auto set = Gtk::make_managed<Gtk::Button>("Set");
    set->set_name("inspector.stage_set");
    set->add_css_class("suggested-action");
    set->set_tooltip_text("Set the changes -- one step");
    reset->signal_clicked().connect([this] {
        m_pending.clear();
        update_bar();
        m_staged_sig.emit();   // s164
        m_has_shown = false;   // the rows as they are in the document again
        const auto t = m_shown_title, sb = m_shown_sub;
        const auto fs = m_shown;
        m_staged_next = m_staged;
        m_stage_ids_next = m_stage_ids;
        fill(t, sb, fs);
    });
    set->signal_clicked().connect([this] {
        auto ids = m_stage_ids;
        auto pend = m_pending;
        m_pending.clear();
        update_bar();
        m_has_shown = false;
        m_apply.emit(ids, pend);
    });
    m_bar->append(*m_bar_text);
    m_bar->append(*reset);
    m_bar->append(*set);
    m_bar->set_visible(false);
    append(*m_bar);

    m_empty.add_css_class("cx-insp-empty");
    m_empty.set_text("Select something in the navigator or the tiles.");
    m_empty.set_wrap(true);
    m_empty.set_vexpand(true);
    m_empty.set_valign(Gtk::Align::START);
    append(m_empty);
    clear();
}

// s041: a typed value's presets, in a menu (s067: shared with compact rows).
Gtk::MenuButton* Inspector::presets_button(const std::string& key, const std::vector<core::Choice>& choices, Gtk::Entry* show_in) {
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
        b->signal_clicked().connect([this, pop, key, id, show_in, lbl = c.label] {
            pop->popdown();
            if (show_in && m_staged) show_in->set_text(lbl);   // s162: what's waiting, shown
            send(key, id);
        });
        list->append(*b);
    }
    pop->set_child(*list);
    mb->set_popover(*pop);
    return mb;
}

void Inspector::fill(const std::string& title, const std::string& sub,
                     const std::vector<core::Field>& fields) {
    const bool same_subject = m_has_shown && title == m_shown_title && sub == m_shown_sub;
    // s162: changes waiting -- the same subject keeps them (and its rows as
    // they are); another subject sets them first, on the styles they were for.
    if (!m_pending.empty()) {
        if (same_subject && m_staged_next) { m_staged_next = false; m_stage_ids_next.clear(); return; }
        auto ids = m_stage_ids;
        auto pend = m_pending;
        m_pending.clear();
        ask_pending(ids, pend, m_shown_title, {});   // s165: Accept / Cancel
        m_has_shown = false;
    }
    m_staged = m_staged_next;
    m_stage_ids = m_stage_ids_next;
    m_staged_next = false;
    m_stage_ids_next.clear();
    update_bar();
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
    m_row_label.clear();
    int row = 0;
    // s161: rows under group headings, folded or open.
    std::string cur_group;
    std::map<int, std::string> row_group;                 // a row -> its group
    std::map<std::string, std::vector<const core::Field*>> group_fields;
    std::vector<std::pair<std::string, Gtk::Button*>> headers;
    for (const auto& f : fields) {
        if (f.group != cur_group) {
            cur_group = f.group;
            if (!cur_group.empty()) {
                auto hb = Gtk::make_managed<Gtk::Button>();
                hb->set_name("inspector.group." + cur_group);
                hb->add_css_class("flat");
                hb->add_css_class("cx-insp-group");
                hb->set_hexpand(true);
                m_fields.attach(*hb, 0, row, 2, 1);
                headers.push_back({cur_group, hb});
                ++row;
            }
        }
        if (!f.group.empty()) group_fields[f.group].push_back(&f);
        // Its rows (however the row ends -- the `continue`s too) under its group.
        struct Rows {
            int row0; const int& row; std::map<int, std::string>& out; const std::string& g;
            ~Rows() { for (int r = row0; r < row; ++r) out[r] = g; }
        } rows_mark{row, row, row_group, f.group};
        if (f.buttons) {   // s009: actions -- a row of buttons under the fields
            // s179: a FlowBox -- a row too long for the panel wraps (a swatch's
            // four buttons cut the last off in s172).
            auto box = Gtk::make_managed<Gtk::FlowBox>();
            box->set_selection_mode(Gtk::SelectionMode::NONE);
            box->set_homogeneous(false);
            box->set_column_spacing(6);
            box->set_row_spacing(6);
            box->set_max_children_per_line(8);
            box->set_valign(Gtk::Align::START);
            box->set_name("inspector.actions." + f.edit);
            box->add_css_class("cx-insp-actions");
            for (const auto& c : f.choices) {
                auto b = Gtk::make_managed<Gtk::Button>(c.label);
                b->set_name("inspector.act." + f.edit + "." + c.id);
                b->add_css_class("cx-insp-action");
                const std::string key = f.edit, id = c.id;
                b->signal_clicked().connect([this, key, id] { m_edit.emit(key, id); });
                box->append(*b);
                if (auto* ch = b->get_parent()) ch->set_focusable(false);   // the button takes focus, not its slot
            }
            m_fields.attach(*box, 0, row, 2, 1);
            ++row;
            continue;
        }
        if (!f.cols.empty()) {   // a compact row: headers over values, under the label
            auto l = Gtk::make_managed<Gtk::Label>(f.label);
            l->add_css_class("cx-field-label");
            if (!f.own.empty()) l->add_css_class("cx-field-own");   // s092: marked (Reset to base clears it)
            if (!f.edit.empty()) m_row_label[f.edit] = l;   // s162
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
                                              send(key, (*linked ? std::string("a") : idx) + ":" + v);
                                          });
                    // Room for the link / presets beside them without widening the panel.
                    en->set_width_chars(1);   // s096: share the column -- a wider minimum widened the panel
                    en->add_css_class("cx-insp-col-entry");
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
                    if (*linked && !even && first != core::kMixed && !first.empty()) send(key, "a:" + first);
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
        if (!f.edit.empty()) m_row_label[f.edit] = l;   // s162
        if (!f.own.empty()) {   // s092: the style's own setting -- marked, with a way back to the base's
            l->add_css_class("cx-field-own");
            l->set_tooltip_text("Its own setting \u2014 not from the style it's based on");
            auto lb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
            lb->set_halign(Gtk::Align::END);
            lb->set_valign(Gtk::Align::CENTER);
            auto rb = Gtk::make_managed<Gtk::Button>("\u21ba");   // a glyph, not a theme icon: shows on any desktop
            rb->set_name("inspector.own_reset." + f.own);
            rb->add_css_class("flat");
            rb->add_css_class("cx-own-reset");
            rb->set_tooltip_text("Take " + f.label + " from the base style again");
            const std::string k = f.own;
            rb->signal_clicked().connect([this, k] { send("style_reset", k); });
            lb->append(*rb);
            lb->append(*l);
            m_fields.attach(*lb, 0, row);
        } else {
            m_fields.attach(*l, 0, row);
        }

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
                send(key, text);
            };
            en->signal_activate().connect(commit);
            auto focus = Gtk::EventControllerFocus::create();
            focus->signal_leave().connect(commit);
            en->add_controller(focus);
            undo_goes_to_document(en, shown);   // s152
            m_fields.attach(*en, 1, row);
            ++row;
            continue;
        }
        if (!f.edit.empty() && (f.unit > 0 || f.percent)) {
            // s041: a typed value, its presets in a menu beside it (s071: or a percentage).
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            box->add_css_class("linked");
            box->set_hexpand(true);
            const std::string key = f.edit;
            auto* te = typed_entry("inspector.edit." + key, f.value, f.unit, f.lo, f.hi,
                                   [this, key](const std::string& v) { send(key, v); }, f.percent);
            box->append(*te);
            if (!f.choices.empty()) box->append(*presets_button(key, f.choices, te));
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
            dd->set_factory(label_factory(true));        // s152: the button's label ellipsized
            dd->set_list_factory(label_factory(false));  // ... the list's whole
            dd->set_selected(at);
            if (at < labels.size()) dd->set_tooltip_text(labels[at]);
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
                if (i - offset < choices.size()) send(key, choices[i - offset].id);
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
    // s161: each heading -- an arrow, its name, and (folded) a line of its values;
    // its rows shown or hidden.
    std::map<std::string, std::vector<Gtk::Widget*>> members;
    for (auto* c = m_fields.get_first_child(); c; c = c->get_next_sibling()) {
        int col = 0, r = 0, w = 0, h = 0;
        m_fields.query_child(*c, col, r, w, h);
        if (const auto it = row_group.find(r); it != row_group.end() && !it->second.empty()) members[it->second].push_back(c);
    }
    for (const auto& [name, hb] : headers) {
        std::string summary;
        int n = 0;
        for (const auto* f : group_fields[name]) {
            if (f->buttons || f->label.empty() || f->value.empty() || n >= 4) continue;
            std::string v = f->value;
            for (auto& ch : v) if (ch == '\t') ch = ' ';
            // A bare On / Off / None says little alone: with its label.
            if (v == "On" || v == "Off" || v == "None" || v == "Inherit") {
                std::string lv = v;
                lv[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lv[0])));
                v = f->label + " " + lv;
            }
            summary += (summary.empty() ? "" : " \u00b7 ") + v;
            ++n;
        }
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto arrow = Gtk::make_managed<Gtk::Label>();
        arrow->add_css_class("cx-group-arrow");
        auto nl = Gtk::make_managed<Gtk::Label>(name);
        nl->add_css_class("cx-group-name");
        auto sl = Gtk::make_managed<Gtk::Label>(summary);
        sl->add_css_class("cx-group-summary");
        sl->set_ellipsize(Pango::EllipsizeMode::END);
        sl->set_width_chars(1);
        sl->set_hexpand(true);
        sl->set_xalign(0.0f);
        box->append(*arrow);
        box->append(*nl);
        box->append(*sl);
        hb->set_child(*box);
        hb->set_tooltip_text(summary);
        auto rows = members[name];
        const auto apply = [this, name, arrow, sl, rows] {
            const bool open = m_open.count(name) > 0;
            arrow->set_text(open ? "\u25BE" : "\u25B8");
            sl->set_visible(!open);
            for (auto* w : rows) w->set_visible(open);
        };
        apply();
        hb->signal_clicked().connect([this, name, apply] {
            if (m_open.count(name)) m_open.erase(name); else m_open.insert(name);
            apply();
            std::string csv;
            for (const auto& g : m_open) csv += (csv.empty() ? "" : ",") + g;
            m_groups_sig.emit(csv);
        });
    }
    m_painting = false;
}

void Inspector::set_open_groups(const std::string& csv) {   // s161
    m_open.clear();
    std::string cur;
    for (char c : csv + ",") {
        if (c == ',') { if (!cur.empty()) m_open.insert(cur); cur.clear(); }
        else cur += c;
    }
}

// s162: an edit -- to the shell now, or (a style's) waiting for Set.
void Inspector::send(const std::string& key, const std::string& value) {
    if (!m_staged) { m_edit.emit(key, value); return; }
    const auto colon = value.find(':');
    const std::string slot = key + "|" + (colon == std::string::npos ? std::string{} : value.substr(0, colon));
    bool found = false;
    for (auto& [k, v] : m_pending) {
        const auto c2 = v.find(':');
        if (k + "|" + (c2 == std::string::npos ? std::string{} : v.substr(0, c2)) == slot) { v = value; found = true; }
    }
    if (!found) m_pending.emplace_back(key, value);
    if (const auto it = m_row_label.find(key); it != m_row_label.end() && it->second) it->second->add_css_class("cx-field-pending");
    update_bar();
    m_staged_sig.emit();   // s164: the specimen shows it now
}

// s165: the changes waiting, as the user leaves them -- Accept sets them (on
// the styles they were made for), Cancel drops them.
void Inspector::ask_pending(std::vector<std::string> ids, Pending pend, const std::string& name, std::function<void()> then) {
    update_bar();
    m_staged_sig.emit();
    auto* win = dynamic_cast<Gtk::Window*>(get_root());
    const auto n = pend.size();
    if (!win) { m_apply.emit(ids, pend); if (then) then(); return; }
    auto dialog = Gtk::AlertDialog::create("Accept the new settings for \u201c" + name + "\u201d?");
    dialog->set_detail(std::to_string(n) + (n == 1 ? " change is" : " changes are") + " waiting. Cancel drops "
                       + (n == 1 ? "it" : "them") + "; Accept sets " + (n == 1 ? "it" : "them") + " (one step).");
    dialog->set_buttons({"Cancel", "Accept"});
    dialog->set_cancel_button(0);
    dialog->set_default_button(1);
    dialog->choose(*win, [this, dialog, ids, pend, then](const Glib::RefPtr<Gio::AsyncResult>& result) {
        int choice = 0;
        try { choice = dialog->choose_finish(result); } catch (const Glib::Error&) { choice = 0; }
        // On idle: the dialog is still transient for the window here (s004).
        Glib::signal_idle().connect_once([this, choice, ids, pend, then] {
            if (choice == 1) m_apply.emit(ids, pend);
            if (then) then();
        });
    });
}

void Inspector::settle_pending(std::function<void()> then) {
    if (m_pending.empty()) { if (then) then(); return; }
    auto ids = m_stage_ids;
    auto pend = m_pending;
    m_pending.clear();
    // The rows as they are in the document again.
    m_has_shown = false;
    const auto t = m_shown_title, sb = m_shown_sub;
    const auto fs = m_shown;
    m_staged_next = m_staged;
    m_stage_ids_next = m_stage_ids;
    fill(t, sb, fs);
    ask_pending(ids, pend, t, std::move(then));
}

void Inspector::update_bar() {
    if (!m_bar) return;
    m_bar->set_visible(m_staged && !m_pending.empty());
    const auto n = m_pending.size();
    m_bar_text->set_text(std::to_string(n) + (n == 1 ? " change waiting" : " changes waiting"));
}

void Inspector::show_items(const std::vector<const core::Item*>& sel) {
    if (sel.empty()) { clear(); return; }
    // s162: styles' settings wait for Set.
    m_staged_next = std::all_of(sel.begin(), sel.end(), [](const core::Item* it) { return it->segment == core::Segment::Styles; });
    m_stage_ids_next.clear();
    if (m_staged_next) for (const auto* it : sel) m_stage_ids_next.push_back(it->id);
    std::string sub;
    if (sel.size() == 1) sub = core::segment_info(sel.front()->segment).title;
    else                 sub = "Shared fields — Mixed where they differ";
    fill(core::selection_title(sel), sub, core::shared_fields(sel));
}

void Inspector::show_section(const core::Outline& o, core::Segment s, const std::vector<core::Field>& extra) {
    const auto n = o.in(s).size();
    fill(core::segment_info(s).title, std::to_string(n) + (n == 1 ? " item" : " items"), extra);
}

void Inspector::clear() {
    if (!m_pending.empty()) {   // s162 / s165: leaving with changes waiting -- ask
        auto ids = m_stage_ids;
        auto pend = m_pending;
        m_pending.clear();
        ask_pending(ids, pend, m_shown_title, {});
    }
    m_staged = false;
    update_bar();
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
