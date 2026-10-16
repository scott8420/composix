#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/stringlist.h>
#include <algorithm>

// Shell_find.cpp -- s022: Find / Change. A bar over the main area (Ctrl+F):
// what to find, what to change it to, Aa (case) and Word, a style to look
// in and a style to apply; the hits in reading order, each shown on its
// page with the words selected. Change and Change All are one step each.

namespace composix {

void Shell::build_find_bar() {  // zone
    m_find_rev = Gtk::make_managed<Gtk::Revealer>();
    m_find_rev->set_name("find.reveal");
    m_find_rev->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
    m_find_rev->set_halign(Gtk::Align::END);
    m_find_rev->set_valign(Gtk::Align::START);
    m_find_rev->set_margin_end(28);
    m_find_rev->set_margin_top(52);   // below the canvas's title row

    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    bar->set_name("find.bar");
    bar->add_css_class("cx-find");
    const auto row = [](const char* name) {
        auto r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        r->set_name(name);
        return r;
    };
    const auto button = [](const char* name, const char* label, const char* tip) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->set_name(name);
        b->set_tooltip_text(tip);
        return b;
    };

    auto r1 = row("find.row1");
    m_find_what = Gtk::make_managed<Gtk::Entry>();
    m_find_what->set_name("find.what");
    m_find_what->set_placeholder_text("Find");
    m_find_what->set_width_chars(22);
    m_find_case = Gtk::make_managed<Gtk::ToggleButton>("Aa");
    m_find_case->set_name("find.case");
    m_find_case->set_tooltip_text("Match case");
    m_find_word = Gtk::make_managed<Gtk::ToggleButton>("Word");
    m_find_word->set_name("find.word");
    m_find_word->set_tooltip_text("Whole word");
    m_find_count = Gtk::make_managed<Gtk::Label>();
    m_find_count->set_name("find.count");
    m_find_count->add_css_class("cx-export-note");
    m_find_count->set_width_chars(12);
    auto prev = button("find.prev", "↑", "Previous (Shift+Enter)");
    auto next = button("find.next", "↓", "Next (Enter)");
    auto close = button("find.close", "✕", "Close (Esc)");
    close->add_css_class("flat");
    for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{m_find_what, m_find_case, m_find_word, m_find_count, prev, next, close})
        r1->append(*w);

    auto r2 = row("find.row2");
    m_find_with = Gtk::make_managed<Gtk::Entry>();
    m_find_with->set_name("find.with");
    m_find_with->set_placeholder_text("Change to (empty: delete)");
    m_find_with->set_width_chars(22);
    auto change = button("find.change", "Change", "Change this one, then find the next");
    auto all = button("find.all", "Change All", "Change every match (one undo)");
    r2->append(*m_find_with);
    r2->append(*change);
    r2->append(*all);

    auto r3 = row("find.row3");
    auto inl = Gtk::make_managed<Gtk::Label>("In");
    inl->add_css_class("cx-field-label");
    m_find_in = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Any style"}));
    m_find_in->set_name("find.in");
    auto apl = Gtk::make_managed<Gtk::Label>("Apply");
    apl->add_css_class("cx-field-label");
    m_find_apply = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"No style change"}));
    m_find_apply->set_name("find.apply");
    r3->append(*inl);
    r3->append(*m_find_in);
    r3->append(*apl);
    r3->append(*m_find_apply);

    bar->append(*r1);
    bar->append(*r2);
    bar->append(*r3);
    m_find_rev->set_child(*bar);
    m_main_over.add_overlay(*m_find_rev);

    // Typing or a toggle: the hits again, the first one shown (keyboard stays here).
    const auto again = [this] {
        m_hits_rev = ~0ull;
        find_refresh();
        if (!m_hits.empty()) find_show(0, false);
    };
    m_find_what->signal_changed().connect(again);
    m_find_case->signal_toggled().connect(again);
    m_find_word->signal_toggled().connect(again);
    m_find_in->property_selected().signal_changed().connect(again);
    m_find_what->signal_activate().connect([this] { find_go(+1); });
    m_find_with->signal_activate().connect([this] { find_change(false); });
    next->signal_clicked().connect([this] { find_go(+1); });
    prev->signal_clicked().connect([this] { find_go(-1); });
    change->signal_clicked().connect([this] { find_change(false); });
    all->signal_clicked().connect([this] { find_change(true); });
    close->signal_clicked().connect([this] { m_find_rev->set_reveal_child(false); });
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType st) {
        if (kv == GDK_KEY_Escape) { m_find_rev->set_reveal_child(false); return true; }
        if ((kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) && (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK) {
            find_go(-1);
            return true;
        }
        return false;
    }, false);
    bar->add_controller(keys);
}

void Shell::on_find() {  // handler
    // The style lists, fresh: paragraph styles, then character styles.
    m_find_styles.clear();
    std::vector<Glib::ustring> in{"Any style"}, apply{"No style change"};
    for (const auto kind : {core::StyleKind::Paragraph, core::StyleKind::Character})
        for (const auto& id : m_doc.ids_of<core::Style>()) {
            const auto& s = *m_doc.as<core::Style>(id);
            if (s.kind != kind) continue;
            m_find_styles.push_back(id);
            const std::string label = s.name + (kind == core::StyleKind::Paragraph ? " ¶" : " (a)");
            in.push_back(label);
            apply.push_back(label);
        }
    m_find_in->set_model(Gtk::StringList::create(in));
    m_find_apply->set_model(Gtk::StringList::create(apply));
    // Words selected on the canvas start the search.
    auto& cv = m_canvas_view.canvas();
    if (cv.editing()) {
        const auto [a, b] = cv.edit_range();
        if (a.para == b.para && a != b)
            if (const auto* st = m_doc.as<core::Story>(cv.edit_story_id()))
                m_find_what->set_text(st->paras[static_cast<std::size_t>(a.para)].text.substr(
                    static_cast<std::size_t>(a.byte), static_cast<std::size_t>(b.byte - a.byte)));
    }
    m_find_rev->set_reveal_child(true);
    m_find_what->grab_focus();
    m_hits_rev = ~0ull;
    find_refresh();
}

void Shell::find_refresh() {  // helper
    if (m_hits_rev == m_doc.revision()) return;
    core::doc::FindOpts f;
    f.what = m_find_what->get_text();
    f.match_case = m_find_case->get_active();
    f.whole_word = m_find_word->get_active();
    if (const auto i = m_find_in->get_selected(); i > 0 && i - 1 < m_find_styles.size()) {
        const auto& id = m_find_styles[i - 1];
        (m_doc.as<core::Style>(id)->kind == core::StyleKind::Paragraph ? f.pstyle : f.cstyle) = id;
    }
    m_hits = core::doc::find_all(m_doc, f);
    m_hits_rev = m_doc.revision();
    if (m_hit >= static_cast<int>(m_hits.size())) m_hit = m_hits.empty() ? -1 : 0;
    find_status();
}

void Shell::find_status(const std::string& extra) {  // helper
    std::string t;
    if (m_find_what->get_text().empty() && m_find_in->get_selected() == 0) t = "";
    else if (m_hits.empty()) t = "No matches";
    else if (m_hit < 0) t = std::to_string(m_hits.size()) + (m_hits.size() == 1 ? " match" : " matches");
    else t = std::to_string(m_hit + 1) + " of " + std::to_string(m_hits.size());
    if (!extra.empty()) t = extra;
    m_find_count->set_text(t);
}

// A hit: its page on the canvas, the words selected. One not set in any
// frame (overset, or a story not placed) says so instead.
void Shell::find_show(int i, bool focus) {  // helper
    if (i < 0 || i >= static_cast<int>(m_hits.size())) return;
    m_hit = i;
    const auto& h = m_hits[static_cast<std::size_t>(i)];
    // Showing the page moves the keyboard (the navigator, the canvas); put
    // it back where it was unless asked to take it.
    Gtk::Widget* had = focus ? nullptr : get_focus();
    const auto flow = layout::flow_story(m_doc, h.story);
    const auto c = layout::caret_at(m_doc, h.story, flow, h.a);
    if (!c.ok) {
        find_status(std::to_string(i + 1) + " of " + std::to_string(m_hits.size()) +
                    (flow.frames.empty() ? " (not placed)" : " (overset)"));
        return;
    }
    const auto* f = m_doc.as<core::Frame>(c.frame);
    auto& cv = m_canvas_view.canvas();
    m_nav.select_items({f->owner});   // its page (or master) on the canvas
    cv.select_text(c.frame, h.a, h.b, focus);
    if (had) {   // an entry: back WITHOUT selecting all (typing would replace it)
        if (had == m_find_what || had->is_ancestor(*m_find_what)) m_find_what->grab_focus_without_selecting();
        else if (had == m_find_with || had->is_ancestor(*m_find_with)) m_find_with->grab_focus_without_selecting();
        else had->grab_focus();
    }
    find_status();
}

void Shell::find_go(int dir) {  // helper
    find_refresh();
    if (m_hits.empty()) return;
    const int n = static_cast<int>(m_hits.size());
    find_show(m_hit < 0 ? (dir > 0 ? 0 : n - 1) : ((m_hit + dir) % n + n) % n, false);
}

void Shell::find_change(bool all) {  // helper
    find_refresh();
    if (m_hits.empty()) return;
    core::doc::ChangeOpts c;
    c.with = m_find_with->get_text();
    if (const auto i = m_find_apply->get_selected(); i > 0 && i - 1 < m_find_styles.size()) {
        const auto& id = m_find_styles[i - 1];
        (m_doc.as<core::Style>(id)->kind == core::StyleKind::Paragraph ? c.pstyle : c.cstyle) = id;
    }
    auto& cv = m_canvas_view.canvas();
    if (all) {
        const int n = core::doc::change_hits(m_journal, m_doc, m_hits,
                                             c, "Change All (" + std::to_string(m_hits.size()) + ")");
        cv.end_edit();
        m_hit = -1;
        m_hits_rev = ~0ull;
        find_refresh();
        find_status(n ? "Changed " + std::to_string(n) : "Nothing changed");
        if (auto lg = log::get(log::Area::Shell)) lg->info("find: change all -> {}", n);
        return;
    }
    // Change: the hit on show (else the first one, shown -- look before changing).
    if (m_hit < 0) { find_show(0, false); return; }
    const auto h = m_hits[static_cast<std::size_t>(m_hit)];
    core::doc::change_hits(m_journal, m_doc, {h}, c, "Change");
    // On to the next one after what was put there.
    const int end = h.a.byte + static_cast<int>(c.with.size());
    m_hits_rev = ~0ull;
    find_refresh();
    int next = -1, last_of_story = -1;
    for (int i = 0; i < static_cast<int>(m_hits.size()); ++i) {
        const auto& x = m_hits[static_cast<std::size_t>(i)];
        if (x.story != h.story) continue;
        last_of_story = i;
        if (next < 0 && (x.a.para > h.a.para || (x.a.para == h.a.para && x.a.byte >= end))) next = i;
    }
    if (next < 0) next = last_of_story + 1;
    if (m_hits.empty()) { cv.end_edit(); m_hit = -1; find_status("Done"); return; }
    find_show(next % static_cast<int>(m_hits.size()), false);
}

}  // namespace composix
