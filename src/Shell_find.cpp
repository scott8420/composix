#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"
#include <gtkmm/checkbutton.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/entry.h>
#include <algorithm>

// Shell_find.cpp -- s022: Find / Change. A bar over the main area (Ctrl+F):
// what to find, what to change it to, Aa (case) and Word, a style to look
// in and a style to apply; the hits in reading order, each shown on its
// page with the words selected. Change and Change All are one step each.

namespace composix {

// s023: Edit > Insert Special Character.
// s274 (Scott: "special chars are hard to remember and codes are even
// harder"): the fields by name -- what each shows, and the sign it wears on
// a master -- for the text menu (right-click while typing).
Glib::RefPtr<Gio::Menu> Shell::field_menu() {  // zone
    auto m = Gio::Menu::create();
    auto pages = Gio::Menu::create();
    pages->append("Page Number  \uFF03", "win.insert-page-number");
    pages->append("Next Page Number  \u300B", "win.insert-next-page");
    pages->append("Previous Page Number  \u300A", "win.insert-prev-page");
    pages->append("Section Name  \u2E39", "win.insert-section-name");
    m->append_section(pages);
    auto book = Gio::Menu::create();
    book->append("Document Title  \u24E3", "win.insert-doc-title");
    book->append("Author  \u24B6", "win.insert-doc-author");
    book->append("Chapter Name  \u24B8", "win.insert-chapter-name");
    book->append("Chapter Subtitle  \u24C8", "win.insert-chapter-sub");
    m->append_section(book);
    return m;
}

// s263: the words' type style -- a toggle each (the inspector's rows too).
Glib::RefPtr<Gio::Menu> Shell::type_style_menu() {  // zone
    auto ts = Gio::Menu::create();
    auto look = Gio::Menu::create();
    look->append("Bold", "win.bold");
    look->append("Italic", "win.italic");
    look->append("Underline", "win.underline");
    look->append("Strikethrough", "win.strikethrough");
    ts->append_section(look);
    auto pos = Gio::Menu::create();
    pos->append("Superscript", "win.superscript");
    pos->append("Subscript", "win.subscript");
    ts->append_section(pos);
    auto cas = Gio::Menu::create();
    cas->append("All Caps", "win.all-caps");
    cas->append("Small Caps", "win.small-caps");
    ts->append_section(cas);
    return ts;
}

Glib::RefPtr<Gio::Menu> Shell::special_menu() {  // zone
    auto m = Gio::Menu::create();
    m->append("Page Number", "win.insert-page-number");
    m->append("Section Name", "win.insert-section-name");
    m->append("Document Title", "win.insert-doc-title");     // s239
    m->append("Author", "win.insert-doc-author");
    m->append("Chapter Name", "win.insert-chapter-name");       // s240
    m->append("Chapter Subtitle", "win.insert-chapter-sub");
    m->append("Next Page Number", "win.insert-next-page");
    m->append("Previous Page Number", "win.insert-prev-page");
    m->append("Discretionary Hyphen", "win.insert-shy");   // s065
    auto notes = Gio::Menu::create();   // s110
    notes->append("Footnote", "win.insert-footnote");
    notes->append("Edit Footnote", "win.edit-footnote");
    notes->append("Footnotes & Endnotes\u2026", "win.notes-panel");   // s121
    notes->append("Cross-Reference\u2026", "win.insert-xref");   // s128
    notes->append("Edit Cross-Reference\u2026", "win.edit-xref");   // s130
    notes->append("All Cross-References\u2026", "win.xrefs-panel");   // s133
    notes->append("Sources & Citations\u2026", "win.sources-panel");   // s144
    m->append_section(notes);
    auto index = Gio::Menu::create();   // s138
    index->append("Index Entry\u2026", "win.index-mark");
    index->append("Edit Index Entry\u2026", "win.edit-index");
    index->append("New Index on This Page", "win.new-index");
    notes = index;
    m->append_section(notes);
    auto breaks = Gio::Menu::create();   // s055
    breaks->append("Column Break", "win.insert-column-break");
    breaks->append("Frame Break", "win.insert-frame-break");
    breaks->append("Page Break", "win.insert-page-break");
    breaks->append("Right-Hand Page Break", "win.insert-right-page-break");   // s252
    breaks->append("Left-Hand Page Break", "win.insert-left-page-break");
    m->append_section(breaks);
    return m;
}

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
    // s051: find by formatting -- words set bold / italic, locally or by
    // their character style.
    auto fml = Gtk::make_managed<Gtk::Label>("Set");
    fml->add_css_class("cx-field-label");
    m_find_format = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Any", "Bold", "Italic", "Bold Italic"}));
    m_find_format->set_name("find.format");
    m_find_format->set_tooltip_text("Only words set bold / italic (by hand or by character style)");
    auto apl = Gtk::make_managed<Gtk::Label>("Apply");
    apl->add_css_class("cx-field-label");
    m_find_apply = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"No style change"}));
    m_find_apply->set_name("find.apply");
    r3->append(*inl);
    r3->append(*m_find_in);
    r3->append(*fml);
    r3->append(*m_find_format);
    // s160: ... or by the paragraph style too.
    m_find_by_para = Gtk::make_managed<Gtk::CheckButton>("by paragraph style too");
    m_find_by_para->set_name("find.by_para");
    m_find_by_para->set_tooltip_text("Count words a bold / italic paragraph style sets (a heading's)");
    m_find_by_para->set_sensitive(false);
    r3->append(*m_find_by_para);
    r3->append(*apl);
    r3->append(*m_find_apply);

    // s056: how Find reads, and where it looks.
    auto r4 = row("find.row4");
    auto mdl = Gtk::make_managed<Gtk::Label>("As");
    mdl->add_css_class("cx-field-label");
    m_find_mode = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Text", "Wildcards", "Regex"}));
    m_find_mode->set_name("find.mode");
    m_find_mode->set_tooltip_text("Wildcards: ^9 digit, ^$ letter, ^w space, ^? any character.\n"
                                  "Regex: a regular expression; Change to takes $1, $2 ... and $& (the whole match).");
    auto scl = Gtk::make_managed<Gtk::Label>("Search");
    scl->add_css_class("cx-field-label");
    m_find_scope = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Document", "This story", "Selection"}));
    m_find_scope->set_name("find.scope");
    m_find_scope->set_tooltip_text("This story / Selection: the text you were in (and had selected) when you opened Find");
    r4->append(*mdl);
    r4->append(*m_find_mode);
    r4->append(*scl);
    r4->append(*m_find_scope);
    {   // s078: saved queries -- load one; save this one under a name.
        auto mb = Gtk::make_managed<Gtk::MenuButton>();
        mb->set_name("find.saved");
        mb->set_label("Saved");
        mb->set_tooltip_text("Saved queries -- kept with the document");
        m_find_saved_pop = Gtk::make_managed<Gtk::Popover>();
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        box->set_margin(6);
        m_find_saved_list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        m_find_saved_list->set_name("find.saved.list");
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        m_find_saved_name = Gtk::make_managed<Gtk::Entry>();
        m_find_saved_name->set_name("find.saved.name");
        m_find_saved_name->set_placeholder_text("Name this query");
        auto save = Gtk::make_managed<Gtk::Button>("Save");
        save->set_name("find.saved.save");
        const auto do_save = [this] {
            auto q = find_capture();
            q.name = m_find_saved_name->get_text();
            if (core::doc::save_query(m_journal, m_doc, q)) {
                if (auto lg = log::get(log::Area::Shell)) lg->info("find: saved query '{}'", q.name);
                m_find_saved_name->set_text("");
                find_saved_fill();
            }
        };
        save->signal_clicked().connect(do_save);
        m_find_saved_name->signal_activate().connect(do_save);
        row->append(*m_find_saved_name);
        row->append(*save);
        box->append(*m_find_saved_list);
        box->append(*row);
        m_find_saved_pop->set_child(*box);
        m_find_saved_pop->signal_show().connect([this] { find_saved_fill(); });
        mb->set_popover(*m_find_saved_pop);
        r4->append(*mb);
    }

    bar->append(*r1);
    bar->append(*r2);
    bar->append(*r3);
    bar->append(*r4);
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
    m_find_format->property_selected().signal_changed().connect(again);
    m_find_format->property_selected().signal_changed().connect([this] { m_find_by_para->set_sensitive(m_find_format->get_selected() > 0); });
    m_find_by_para->signal_toggled().connect(again);   // s160
    m_find_mode->property_selected().signal_changed().connect(again);    // s056
    m_find_scope->property_selected().signal_changed().connect(again);
    m_find_apply->property_selected().signal_changed().connect(again);   // s051: what's already in it drops out
    m_find_what->signal_activate().connect([this] { find_go(+1); });
    m_find_with->signal_activate().connect([this] { find_change(false); });
    next->signal_clicked().connect([this] { find_go(+1); });
    prev->signal_clicked().connect([this] { find_go(-1); });
    change->signal_clicked().connect([this] { find_change(false); });
    all->signal_clicked().connect([this] { find_change(true); });
    close->signal_clicked().connect([this] { m_find_rev->set_reveal_child(false); });
    m_find_rev->property_reveal_child().signal_changed().connect([this] {   // s113: the bar goes, so does its note
        if (!m_find_rev->get_reveal_child()) close_find_note();
    });
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType st) {
        if (kv == GDK_KEY_Escape) {
            // s078: Esc in the Saved popover closes just that (its keys reach the bar too).
            if (m_find_saved_pop && m_find_saved_pop->get_visible()) { m_find_saved_pop->popdown(); return true; }
            m_find_rev->set_reveal_child(false);
            return true;
        }
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
    // Words selected on the canvas start the search. s056: the story and
    // selection are kept for This story / Selection; a selection across
    // paragraphs is a place to look, not a thing to find.
    auto& cv = m_canvas_view.canvas();
    m_find_story.clear();
    m_find_from = m_find_to = {};
    if (!cv.editing() && cv.cell_editing()) m_find_story = cv.cell_story();   // s335: typing in a cell -- This story is the table's
    if (cv.editing()) {
        const auto [a, b] = cv.edit_range();
        m_find_story = cv.edit_story_id();
        m_find_from = a;
        m_find_to = b;
        if (a.para != b.para) m_find_scope->set_selected(2);
        else if (m_find_scope->get_selected() == 2 && a == b) m_find_scope->set_selected(1);
        if (a.para == b.para && a != b && m_find_scope->get_selected() != 2)
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
    // s056: how it reads, where it looks.
    f.mode = static_cast<core::doc::FindOpts::Mode>(std::min(2u, m_find_mode->get_selected()));
    const auto scope = m_find_scope->get_selected();
    if (scope >= 1) f.story = m_find_story.empty() ? std::string("-") : m_find_story;   // "-": nothing to search
    if (scope == 2) {
        f.in_range = true;
        f.from = m_find_from;
        f.to = m_find_to;
    }
    {
        std::string why;
        const bool ok = core::doc::find_pattern_ok(f, &why);
        if (ok) m_find_what->remove_css_class("error"); else m_find_what->add_css_class("error");
        m_find_bad = ok ? std::string{} : "Pattern: " + why;
        if (scope >= 1 && m_find_story.empty()) m_find_bad = "Click into text, then Ctrl+F";
        else if (scope == 2 && m_find_from == m_find_to) m_find_bad = "Select text, then Ctrl+F";
    }
    const auto fm = m_find_format->get_selected();   // s051
    f.bold = fm == 1 || fm == 3;
    f.italic = fm == 2 || fm == 3;
    f.para_counts = (f.bold || f.italic) && m_find_by_para->get_active();   // s160
    if (f.bold || f.italic)
        if (const auto i = m_find_apply->get_selected(); i > 0 && i - 1 < m_find_styles.size())
            if (m_doc.as<core::Style>(m_find_styles[i - 1])->kind == core::StyleKind::Character) f.skip_cstyle = m_find_styles[i - 1];
    m_hits = core::doc::find_all(m_doc, f);
    m_hits_rev = m_doc.revision();
    if (m_hit >= static_cast<int>(m_hits.size())) m_hit = m_hits.empty() ? -1 : 0;
    find_status();
}

void Shell::find_status(const std::string& extra) {  // helper
    std::string t;
    if (m_find_what->get_text().empty() && m_find_in->get_selected() == 0 && m_find_format->get_selected() == 0) t = "";
    else if (m_hits.empty()) t = "No matches";
    else if (m_hit < 0) t = std::to_string(m_hits.size()) + (m_hits.size() == 1 ? " match" : " matches");
    else t = std::to_string(m_hit + 1) + " of " + std::to_string(m_hits.size());
    if (!m_find_bad.empty() && m_hits.empty() && !t.empty()) t = m_find_bad;   // s056
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
    // s113: a hit in a note -- its mark, on the page; its words then open.
    core::TextPos a = h.a, b = h.b;
    if (h.note >= 0) {
        const auto* st = m_doc.as<core::Story>(h.story);
        const int m = st ? core::doc::note_mark_byte(*st, h.a.para, h.note) : -1;
        if (m < 0) return;
        a = {h.a.para, m};
        b = {h.a.para, m + 3};
    } else {
        close_find_note();
    }
    if (h.table >= 0) {   // s335: in a table's cell -- typing in it, the words selected
        close_find_note();
        auto& cv = m_canvas_view.canvas();
        const layout::CellRef ref{h.table, h.row, h.col};
        const std::string fr = layout::cell_frame(layout::flow_story(m_doc, h.story), ref);
        const auto* f = m_doc.as<core::Frame>(fr);
        if (!f) {
            find_status(std::to_string(i + 1) + " of " + std::to_string(m_hits.size()) + " (in a table, overset)");
            return;
        }
        m_nav.select_items({f->owner});
        cv.select_cell_text(fr, h.story, ref, h.a, h.b);
        if (had) {
            if (had == m_find_what || had->is_ancestor(*m_find_what)) m_find_what->grab_focus_without_selecting();
            else if (had == m_find_with || had->is_ancestor(*m_find_with)) m_find_with->grab_focus_without_selecting();
            else had->grab_focus();
        }
        find_status(std::to_string(i + 1) + " of " + std::to_string(m_hits.size()) + " \u00b7 in a table");
        return;
    }
    const auto flow = layout::flow_story(m_doc, h.story);
    const auto c = layout::caret_at(m_doc, h.story, flow, a);
    if (!c.ok) {
        close_find_note();
        find_status(std::to_string(i + 1) + " of " + std::to_string(m_hits.size()) +
                    (flow.frames.empty() ? " (not placed)" : " (overset)"));
        return;
    }
    const auto* f = m_doc.as<core::Frame>(c.frame);
    auto& cv = m_canvas_view.canvas();
    m_nav.select_items({f->owner});   // its page (or master) on the canvas
    cv.select_text(c.frame, a, b, focus);
    if (h.note >= 0) edit_note(h.story, h.a.para, h.note, h.a.byte, h.b.byte, focus);
    if (had) {   // an entry: back WITHOUT selecting all (typing would replace it)
        if (had == m_find_what || had->is_ancestor(*m_find_what)) m_find_what->grab_focus_without_selecting();
        else if (had == m_find_with || had->is_ancestor(*m_find_with)) m_find_with->grab_focus_without_selecting();
        else had->grab_focus();
    }
    find_status(h.note >= 0 ? std::to_string(i + 1) + " of " + std::to_string(m_hits.size()) + " \u00b7 in a note" : std::string{});
}

void Shell::find_go(int dir) {  // helper
    find_refresh();
    if (m_hits.empty()) return;
    const int n = static_cast<int>(m_hits.size());
    find_show(m_hit < 0 ? (dir > 0 ? 0 : n - 1) : ((m_hit + dir) % n + n) % n, false);
}

void Shell::find_change(bool all) {  // helper
    close_find_note();   // s113: what was typed in it goes in first
    find_refresh();
    if (m_hits.empty()) return;
    core::doc::ChangeOpts c;
    c.with = m_find_with->get_text();
    if (const auto i = m_find_apply->get_selected(); i > 0 && i - 1 < m_find_styles.size()) {
        const auto& id = m_find_styles[i - 1];
        (m_doc.as<core::Style>(id)->kind == core::StyleKind::Paragraph ? c.pstyle : c.cstyle) = id;
    }
    c.expand = m_find_mode->get_selected() != 0;   // s056: $1 ... from the pattern
    // s051: an empty "Change to" with a style to apply keeps the words (it
    // deleted them before -- the style then had nothing to go on).
    c.set_text = !c.with.empty() || (c.pstyle.empty() && c.cstyle.empty());
    auto& cv = m_canvas_view.canvas();
    if (all) {
        const int n = core::doc::change_hits(m_journal, m_doc, m_hits,
                                             c, "Change All (" + std::to_string(m_hits.size()) + ")");
        cv.end_edit();
        cv.end_cell();   // s335: a cell's words may have changed under its caret
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
    if (h.table >= 0) {   // s335: in a cell -- on to the next hit after what was put there
        const int end = c.set_text ? h.a.byte + static_cast<int>(core::doc::change_text(c, h).size()) : h.b.byte;
        const int end_para = h.a.para;
        cv.end_cell();
        core::doc::change_hits(m_journal, m_doc, {h}, c, "Change");
        m_hits_rev = ~0ull;
        find_refresh();
        if (m_hits.empty()) { m_hit = -1; find_status("Done"); return; }
        int next = -1;
        for (int i = 0; i < static_cast<int>(m_hits.size()) && next < 0; ++i) {
            const auto& x = m_hits[static_cast<std::size_t>(i)];
            if (x.story == h.story && x.table == h.table && x.row == h.row && x.col == h.col &&
                (x.a.para > end_para || (x.a.para == end_para && x.a.byte >= end)))
                next = i;
        }
        if (next < 0) next = std::min(m_hit, static_cast<int>(m_hits.size()) - 1);
        if (next >= 0 && m_hits[static_cast<std::size_t>(next)] == h) next = (next + 1) % static_cast<int>(m_hits.size());
        find_show(next, false);
        return;
    }
    if (h.note >= 0) {   // s113: in a note -- on to the next hit after what was put there
        const int end = c.set_text ? h.a.byte + static_cast<int>(core::doc::change_text(c, h).size()) : h.b.byte;
        core::doc::change_hits(m_journal, m_doc, {h}, c, "Change");
        m_hits_rev = ~0ull;
        find_refresh();
        if (m_hits.empty()) { cv.end_edit(); m_hit = -1; find_status("Done"); return; }
        // The hits read in order, the note's at its mark: the first one past
        // this note's (from `end`), else the one that came after it.
        int next = -1;
        for (int i = 0; i < static_cast<int>(m_hits.size()) && next < 0; ++i) {
            const auto& x = m_hits[static_cast<std::size_t>(i)];
            if (x.story == h.story && x.a.para == h.a.para && x.note == h.note && x.a.byte >= end) next = i;
        }
        if (next < 0) next = std::min(m_hit, static_cast<int>(m_hits.size()) - 1);
        // ... still the same one (the text put back matched): one on.
        if (next >= 0 && m_hits[static_cast<std::size_t>(next)] == h) next = (next + 1) % static_cast<int>(m_hits.size());
        find_show(next, false);
        return;
    }
    core::doc::change_hits(m_journal, m_doc, {h}, c, "Change");
    // On to the next one after what was put there.
    const auto put = core::doc::change_text(c, h);   // s056 groups; s095 ^p breaks
    const auto nl = put.rfind('\n');
    const int put_paras = static_cast<int>(std::count(put.begin(), put.end(), '\n'));
    const int end = c.set_text ? (nl == std::string::npos ? h.a.byte + static_cast<int>(put.size()) : static_cast<int>(put.size() - nl - 1))
                               : (h.b.para == h.a.para ? h.b.byte : 0);
    const int end_para = c.set_text ? h.a.para + put_paras : h.b.para;
    m_hits_rev = ~0ull;
    find_refresh();
    int next = -1, last_of_story = -1;
    for (int i = 0; i < static_cast<int>(m_hits.size()); ++i) {
        const auto& x = m_hits[static_cast<std::size_t>(i)];
        if (x.story != h.story) continue;
        last_of_story = i;
        int xb = x.a.byte;   // s113: a note's hit reads at its mark
        if (x.note >= 0)
            if (const auto* st = m_doc.as<core::Story>(x.story)) xb = core::doc::note_mark_byte(*st, x.a.para, x.note) + 1;
        if (next < 0 && (x.a.para > end_para || (x.a.para == end_para && xb >= end))) next = i;
    }
    if (next < 0) next = last_of_story + 1;
    if (m_hits.empty()) { cv.end_edit(); m_hit = -1; find_status("Done"); return; }
    find_show(next % static_cast<int>(m_hits.size()), false);
}

// s339: a hyperlink's words shown -- its page, the words selected.
void Shell::show_link_use(const core::doc::LinkSpan& l) {  // helper
    auto& cv = m_canvas_view.canvas();
    const auto flow = layout::flow_story(m_doc, l.story);
    if (l.row >= 0) {
        const layout::CellRef ref{l.para, l.row, l.col};
        const std::string fr = layout::cell_frame(flow, ref);
        const auto* f = m_doc.as<core::Frame>(fr);
        if (!f) { alert("Not on a page", "That link is in a table cell that doesn't fit its frames (overset)."); return; }
        m_nav.select_items({f->owner});
        cv.select_cell_text(fr, l.story, ref, {l.cpara, l.from}, {l.cpara, l.to});
        return;
    }
    const core::TextPos a{l.para, l.from}, b{l.para, l.to};
    const auto c = layout::caret_at(m_doc, l.story, flow, a);
    if (!c.ok) { alert("Not on a page", flow.frames.empty() ? "That link's story isn't placed." : "That link's words are overset."); return; }
    const auto* f = m_doc.as<core::Frame>(c.frame);
    m_nav.select_items({f->owner});
    cv.select_text(c.frame, a, b, true);
}

}  // namespace composix

namespace composix {

// s033: the story editor -- the story being typed in, else the picked text
// frame's, else the inspected story (navigator / tiles).
void Shell::on_story_editor() {  // handler
    std::string story;
    auto& cv = m_canvas_view.canvas();
    if (cv.editing()) story = cv.edit_story_id();
    if (story.empty())
        for (const auto& id : cv.selection())
            if (const auto* f = m_doc.as<core::Frame>(id); f && !f->story.empty()) { story = f->story; break; }
    if (story.empty())
        for (const auto& id : m_inspected)
            if (m_doc.as<core::Story>(id)) { story = id; break; }
    if (story.empty()) {
        alert("No story to open", "Pick a text frame (or a story in the navigator), then open the story editor.");
        return;
    }
    cv.end_edit();
    m_nav.set_open(core::Segment::Stories, true);
    m_nav.select_items({story});
    m_story_ed.focus_text();
}

}  // namespace composix

namespace composix {

// s078: the bar's settings as a query (styles by id, "" = any / leave).
core::SavedQuery Shell::find_capture() const {  // helper
    core::SavedQuery q;
    q.what = m_find_what->get_text();
    q.with = m_find_with->get_text();
    q.match_case = m_find_case->get_active();
    q.whole_word = m_find_word->get_active();
    q.mode = static_cast<int>(std::min(2u, m_find_mode->get_selected()));
    q.format = static_cast<int>(std::min(3u, m_find_format->get_selected())) | (m_find_by_para->get_active() ? 4 : 0);   // s160: +4 the paragraph style counts
    if (const auto i = m_find_in->get_selected(); i > 0 && i - 1 < m_find_styles.size()) q.in_style = m_find_styles[i - 1];
    if (const auto i = m_find_apply->get_selected(); i > 0 && i - 1 < m_find_styles.size()) q.apply_style = m_find_styles[i - 1];
    return q;
}

// s078: a saved query into the bar -- a style that's gone reads as Any / Leave.
void Shell::find_load(const core::SavedQuery& q) {  // helper
    const auto at = [this](const std::string& id) -> guint {
        for (std::size_t k = 0; k < m_find_styles.size(); ++k) if (m_find_styles[k] == id) return static_cast<guint>(k + 1);
        return 0;
    };
    m_find_what->set_text(q.what);
    m_find_with->set_text(q.with);
    m_find_case->set_active(q.match_case);
    m_find_word->set_active(q.whole_word);
    m_find_mode->set_selected(static_cast<guint>(std::clamp(q.mode, 0, 2)));
    m_find_format->set_selected(static_cast<guint>(q.format & 3));
    m_find_by_para->set_active((q.format & 4) != 0);   // s160
    m_find_in->set_selected(q.in_style.empty() ? 0 : at(q.in_style));
    m_find_apply->set_selected(q.apply_style.empty() ? 0 : at(q.apply_style));
    m_hits_rev = ~0ull;
    find_refresh();
    if (!m_hits.empty()) find_show(0, false);
    if (auto lg = log::get(log::Area::Shell)) lg->info("find: loaded query '{}' -> {} hit(s)", q.name, m_hits.size());
}

// s078: the Saved popover's rows -- a button to load each, an x to delete it.
void Shell::find_saved_fill() {  // helper
    if (!m_find_saved_list) return;
    while (auto* c = m_find_saved_list->get_first_child()) m_find_saved_list->remove(*c);
    const auto& qs = m_doc.settings().queries;
    if (qs.empty()) {
        auto none = Gtk::make_managed<Gtk::Label>("None saved yet");
        none->add_css_class("cx-export-note");
        m_find_saved_list->append(*none);
        return;
    }
    for (const auto& q : qs) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
        auto load = Gtk::make_managed<Gtk::Button>(q.name);
        load->add_css_class("flat");
        load->set_hexpand(true);
        if (auto* lab = dynamic_cast<Gtk::Label*>(load->get_child())) lab->set_xalign(0.0f);
        load->set_tooltip_text(q.what.empty() ? "(formatting only)" : "Find: " + q.what);
        const auto name = q.name;
        load->signal_clicked().connect([this, name] {
            for (const auto& x : m_doc.settings().queries)
                if (x.name == name) { const auto copy = x; m_find_saved_pop->popdown(); find_load(copy); break; }
        });
        auto del = Gtk::make_managed<Gtk::Button>();
        del->set_icon_name("window-close-symbolic");
        del->add_css_class("flat");
        del->set_tooltip_text("Delete this saved query");
        del->signal_clicked().connect([this, name] {
            if (core::doc::delete_query(m_journal, m_doc, name)) find_saved_fill();
        });
        row->append(*load);
        row->append(*del);
        m_find_saved_list->append(*row);
    }
}

}  // namespace composix
