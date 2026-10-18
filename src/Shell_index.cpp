#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Export.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/togglebutton.h>

#include <set>

// Shell_index.cpp -- s138: the index in the app. Index Entry... (Ctrl+Alt+U)
// marks the selection's start with a term (the selected words, to start),
// a sub-entry, and where else to look (See / See also); Edit Index Entry...
// beside a mark changes or removes it. New Index puts an Index on the page
// on show (or the last) and builds it. Each one step.

namespace composix {

namespace {
// The words selected, as a term: marks out, lines joined, trimmed, not too long.
std::string term_of(const std::string& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        if (static_cast<unsigned char>(t[i]) == 0xEE && i + 2 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0x80) { i += 3; continue; }
        out += (t[i] == '\n' || t[i] == '\t') ? ' ' : t[i];
        ++i;
    }
    const auto a = out.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    out = out.substr(a, out.find_last_not_of(' ') - a + 1);
    if (out.size() > 80) {
        out.resize(80);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty()) out.pop_back();
    }
    return out;
}
}  // namespace

void Shell::on_index_mark() {  // handler
    if (!m_canvas_view.canvas().editing()) {
        alert("Click into text first", "An index entry is marked in the text: select the words (or put the cursor where it belongs), then Index Entry…");
        return;
    }
    open_index_pop(-1, -1);
}

void Shell::on_edit_index() {  // handler
    auto& cv = m_canvas_view.canvas();
    const auto* st = cv.editing() ? m_doc.as<core::Story>(cv.edit_story_id()) : nullptr;
    core::doc::IndexAt at;
    if (st) {
        const auto [a, b] = cv.edit_range();
        at = core::doc::index_near(*st, b);
        if (at.k < 0) at = core::doc::index_near(*st, a);
    }
    if (at.k < 0) {
        alert("No index entry here", "Put the text cursor beside an index mark (the small orange flag), then Edit Index Entry…");
        return;
    }
    open_index_pop(at.para, at.k);
}

void Shell::open_index_pop(int edit_para, int edit_k) {  // helper
    auto& cv = m_canvas_view.canvas();
    const std::string story = cv.edit_story_id();
    const auto* st = m_doc.as<core::Story>(story);
    if (!st) return;
    const auto [lo, hi] = cv.edit_range();
    const bool edit = edit_k >= 0;
    core::IndexMark cur;
    if (edit) cur = st->paras[static_cast<std::size_t>(edit_para)].index[static_cast<std::size_t>(edit_k)];
    else cur.term = term_of(core::text_between(*st, lo, hi));
    // s139: a selection over several paragraphs covers them, to start.
    const int sel_paras = hi.para - lo.para + 1;
    if (!edit && sel_paras > 1) { cur.span = core::IndexSpan::Paragraphs; cur.count = sel_paras; }
    m_index_span = static_cast<int>(cur.span);
    m_index_count = cur.count;

    if (m_index_pop) {
        m_index_pop->popdown();
        m_index_pop->unparent();
        m_index_pop.reset();
    }
    m_index_pop = std::make_unique<Gtk::Popover>();
    m_index_pop->set_name("index.popover");
    m_index_pop->set_parent(cv);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(10);
    auto head = Gtk::make_managed<Gtk::Label>(edit ? "Edit Index Entry" : "Index Entry");
    head->set_xalign(0);
    head->add_css_class("heading");
    box->append(*head);
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    const auto row = [&](int r, const char* label, const char* name, const std::string& value, const char* hint) {
        auto l = Gtk::make_managed<Gtk::Label>(label);
        l->set_xalign(1);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, r);
        auto e = Gtk::make_managed<Gtk::Entry>();
        e->set_name(name);
        e->set_text(value);
        e->set_placeholder_text(hint);
        e->set_width_chars(28);
        e->set_hexpand(true);
        grid->attach(*e, 1, r);
        return e;
    };
    auto term = row(0, "Term", "index.term", cur.term, "the word or phrase looked up");
    auto sub = row(1, "Under it", "index.sub", cur.sub, "a sub-entry (optional)");
    auto see = row(2, "See", "index.see", cur.see, "another entry (optional)");
    auto also = Gtk::make_managed<Gtk::CheckButton>("See also -- after this entry's pages");
    also->set_name("index.also");
    also->set_active(cur.also);
    grid->attach(*also, 1, 3);
    // s139: what it covers -- side by side (a dropdown in a popover closes both, s129).
    {
        auto l = Gtk::make_managed<Gtk::Label>("Covers");
        l->set_xalign(1);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, 4);
        auto spans = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        spans->add_css_class("linked");
        spans->set_name("index.span");
        struct Opt { int span; std::string label, tip; };
        std::vector<Opt> opts{{0, "This Place", "The page the mark is on"},
                              {1, "Paragraph", "Every page its paragraph runs over"},
                              {2, "To Heading", "On to the next heading"},
                              {4, "To End", "To the story's end"}};
        if (m_index_span == 3 || sel_paras > 1)
            opts.insert(opts.begin() + 2, {3, std::to_string(m_index_count) + " Paragraphs", "These paragraphs, its own the first"});
        Gtk::ToggleButton* first = nullptr;
        for (const auto& o : opts) {
            auto t = Gtk::make_managed<Gtk::ToggleButton>(o.label);
            t->set_tooltip_text(o.tip);
            if (first) t->set_group(*first); else first = t;
            t->set_active(o.span == m_index_span);
            const int sp = o.span;
            t->signal_toggled().connect([this, t, sp] { if (t->get_active()) m_index_span = sp; });
            spans->append(*t);
        }
        grid->attach(*spans, 1, 4);
    }
    box->append(*grid);
    auto hint = Gtk::make_managed<Gtk::Label>("With See alone, the entry sends the reader there and adds no page.");
    hint->add_css_class("dim-label");
    hint->set_wrap(true);
    hint->set_max_width_chars(40);
    hint->set_xalign(0);
    box->append(*hint);
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    buttons->set_halign(Gtk::Align::END);
    auto ok = Gtk::make_managed<Gtk::Button>(edit ? "Apply" : "Mark");
    ok->set_name("index.ok");
    ok->add_css_class("suggested-action");
    if (edit) {
        auto rm = Gtk::make_managed<Gtk::Button>("Remove");
        rm->set_name("index.remove");
        rm->add_css_class("destructive-action");
        rm->signal_clicked().connect([this, story, edit_para, edit_k] {
            const auto* s = m_doc.as<core::Story>(story);
            if (!s) return;
            const auto& t = s->paras[static_cast<std::size_t>(edit_para)].text;
            std::size_t i = t.find(core::kMarkIndex);
            for (int n = 0; n < edit_k && i != std::string::npos; ++n) i = t.find(core::kMarkIndex, i + 3);
            if (m_index_pop) m_index_pop->popdown();
            if (i == std::string::npos) return;
            const int b = static_cast<int>(i);
            auto& c = m_canvas_view.canvas();
            const auto at = core::doc::edit_text(m_journal, m_doc, story, {edit_para, b}, {edit_para, b + 3}, "", "Remove Index Entry");
            if (auto lg = log::get(log::Area::Shell)) lg->info("index: removed {}:{}", edit_para, edit_k);
            c.model_changed();
            c.place_caret(at);
            Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
        });
        buttons->append(*rm);
    }
    buttons->append(*ok);
    box->append(*buttons);
    const auto go = [this, story, edit, edit_para, edit_k, lo, term, sub, see, also] {
        const core::IndexMark m{term->get_text(), sub->get_text(), see->get_text(), also->get_active(),
                                static_cast<core::IndexSpan>(std::clamp(m_index_span, 0, 4)), m_index_count};
        auto& c = m_canvas_view.canvas();
        bool did = false;
        if (edit) {
            did = core::doc::set_index_mark(m_journal, m_doc, story, edit_para, edit_k, m);
        } else {
            const auto after = core::doc::insert_index_mark(m_journal, m_doc, story, lo, m);
            did = !(after == lo);
            if (did) { c.model_changed(); c.place_caret(after); }
        }
        if (auto lg = log::get(log::Area::Shell))
            lg->info("index: {} \"{}\" / \"{}\" see \"{}\"{} span {} ({}) -> {}", edit ? "edit" : "mark", m.term, m.sub, m.see,
                     m.also ? " also" : "", static_cast<int>(m.span), m.count, did ? "set" : "nothing");
        if (did && edit) c.model_changed();
        if (!did && !edit && m.term.find_first_not_of(' ') == std::string::npos) {   // no term: say so, stay open
            term->grab_focus();
            return;
        }
        if (m_index_pop) m_index_pop->popdown();
        Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
    };
    ok->signal_clicked().connect(go);
    for (auto* e : {term, sub, see}) e->signal_activate().connect(go);
    m_index_pop->set_child(*box);
    const auto r = cv.caret_screen();
    if (r.w > 0)
        m_index_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    m_index_pop->popup();
    Glib::signal_idle().connect_once([term] { term->grab_focus(); });
}

// New Index: on the page on show (else the last), built at once -- one step.
void Shell::on_new_index() {  // handler
    std::string page;
    for (const auto& id : m_canvas_view.canvas().shown())
        if (m_doc.as<core::Page>(id)) { page = id; break; }
    if (page.empty() && !m_doc.pages().empty()) page = m_doc.pages().back();
    if (page.empty()) return;
    std::string id;
    // One step: the list, then its story, frame and styles. Runs inside a run
    // just write (Undo.hpp), so the step names what it made: every record new since.
    std::set<core::Doc::Id> had;
    for (const auto& [rid, rec] : m_doc.records()) had.insert(rid);
    m_journal.run("New Index", {}, [&]() -> std::vector<core::Doc::Id> {
        id = core::doc::add_list(m_journal, m_doc, core::ListKind::Index, "Index", page);
        if (!id.empty()) layout::build_list(m_journal, m_doc, id);
        std::vector<core::Doc::Id> made;
        for (const auto& [rid, rec] : m_doc.records()) if (!had.count(rid)) made.push_back(rid);
        return made;
    });
    if (auto lg = log::get(log::Area::Shell)) lg->info("index: new {} on {}", id, page);
    if (id.empty()) return;
    queue_refresh();
    m_nav.select_items({page});
}

}  // namespace composix
