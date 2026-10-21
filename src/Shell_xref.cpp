#include "Shell.hpp"

#include <filesystem>
#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/window.h>
#include <gtkmm/listbox.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>

#include <map>

// Shell_xref.cpp -- s128: Edit > Insert Special Character > Cross-Reference...
// A popover at the text cursor: where the reference points (the document's
// headings -- paragraphs in a style named Heading..., or one a Contents list
// gathers -- and any paragraph holding an anchor), with where each is now;
// what it shows (page / words / both). A click: the reference at the cursor,
// one step (the heading's anchor made if it hasn't one). "Anchor Here" marks
// the cursor's place for references to point at.

namespace composix {

namespace {
bool heading_style(const core::Doc& d, const std::string& style_id, const std::vector<std::string>& listed) {
    if (std::find(listed.begin(), listed.end(), style_id) != listed.end()) return true;
    const auto* s = d.as<core::Style>(style_id);
    return s && s->name.rfind("Heading", 0) == 0;
}
}  // namespace

void Shell::on_insert_xref() {  // handler
    if (!m_canvas_view.canvas().editing()) {
        alert("Click into text first", "A cross-reference goes in at the text cursor: double-click a text frame, then insert.");
        return;
    }
    open_xref(-1, -1);
}

// s130: Edit Cross-Reference -- the one at (or just before) the cursor.
void Shell::on_edit_xref() {  // handler
    auto& cv = m_canvas_view.canvas();
    const auto* st = cv.editing() ? m_doc.as<core::Story>(cv.edit_story_id()) : nullptr;
    core::doc::RefAt r;
    if (st) {
        const auto [a, b] = cv.edit_range();
        r = core::doc::ref_near(*st, b);
        if (r.k < 0) r = core::doc::ref_near(*st, a);
    }
    if (r.k < 0) {
        alert("No cross-reference here", "Put the text cursor beside a cross-reference, then Edit Cross-Reference.");
        return;
    }
    open_xref(r.para, r.k);
}

void Shell::open_xref(int edit_para, int edit_k) {
    auto& cv = m_canvas_view.canvas();
    const std::string story = cv.edit_story_id();
    const auto at = cv.edit_range().second;
    // s130: changing one -- what it is now.
    const bool edit = edit_k >= 0;
    core::XRef cur;
    core::doc::AnchorAt cur_at;
    bool cur_note = false;
    if (edit) {
        const auto* st = m_doc.as<core::Story>(story);
        if (!st || edit_para >= static_cast<int>(st->paras.size()) ||
            edit_k >= static_cast<int>(st->paras[static_cast<std::size_t>(edit_para)].refs.size()))
            return;
        cur = st->paras[static_cast<std::size_t>(edit_para)].refs[static_cast<std::size_t>(edit_k)];
        m_xref_show = static_cast<int>(cur.show);
        if (cur.show == core::RefShow::Custom) m_xref_pattern = cur.pattern;
        if (cur.show == core::RefShow::Cite) m_xref_locator = cur.pattern;   // s143
        cur_at = core::doc::find_anchor(m_doc, cur.target);
        if (!cur_at.story.empty())
            cur_note = core::note_at(m_doc.as<core::Story>(cur_at.story)->paras[static_cast<std::size_t>(cur_at.at.para)].text,
                                     static_cast<std::size_t>(cur_at.at.byte) + 3);
    }
    // The places to point at.
    std::vector<std::string> listed;   // styles a Contents list gathers
    for (const auto& lid : m_doc.ids_of<core::GenList>()) {
        const auto& l = *m_doc.as<core::GenList>(lid);
        if (l.kind == core::ListKind::Contents) listed.insert(listed.end(), l.from_styles.begin(), l.from_styles.end());
    }
    struct Target { std::string story; int para; std::string label; int k = -1; };   // s129: k -- a footnote
    std::vector<Target> targets;
    std::map<std::string, layout::StoryFlow> flows;
    for (const auto& sid : m_doc.ids_of<core::Story>()) {
        const auto& st = *m_doc.as<core::Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            // s133: an anchor of its own (one set before a note's mark is the note's -- listed with the notes).
            bool anchored = false;
            for (auto i = para.text.find(core::kMarkAnchor); i != std::string::npos && !anchored; i = para.text.find(core::kMarkAnchor, i + 3))
                anchored = !core::note_at(para.text, i + 3);
            if (!anchored && !heading_style(m_doc, para.style, listed)) continue;
            if (core::source_number(m_doc, st, p) > 0) continue;   // s141: a source -- listed with the sources
            // Its words and where it is now (as a reference would show them).
            std::string words, page;
            {
                std::string t;
                for (std::size_t i = 0; i < para.text.size() && t.size() < 60 && para.text[i] != '\t';) {
                    if (static_cast<unsigned char>(para.text[i]) == 0xEE && i + 2 < para.text.size()) { i += 3; continue; }
                    t += para.text[i++];
                }
                words = t.empty() ? std::string("(no words)") : t;
            }
            {
                auto it = flows.find(sid);
                if (it == flows.end()) it = flows.emplace(sid, m_pf_flows.get(m_doc, sid)).first;
                for (const auto& ft : it->second.frames)
                    for (const auto& l : ft.lines)
                        if (l.para == p && l.start == 0)
                            if (const auto* f = m_doc.as<core::Frame>(ft.frame); f && m_doc.as<core::Page>(f->owner)) page = m_doc.folio(f->owner);
            }
            targets.push_back({sid, p, words + (page.empty() ? std::string("  — not placed") : "  — page " + page)});
        }
    }
    // s141: the bibliography's sources, each by its number and first words.
    for (const auto& sid : m_doc.ids_of<core::Story>()) {
        const auto& st = *m_doc.as<core::Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const int n = core::source_number(m_doc, st, p);
            if (n == 0) continue;
            std::string w;
            const auto& t = st.paras[static_cast<std::size_t>(p)].text;
            for (std::size_t i = 0; i < t.size() && w.size() < 48;) {
                if (static_cast<unsigned char>(t[i]) == 0xEE && i + 2 < t.size()) { i += 3; continue; }
                w += t[i++];
            }
            while (!w.empty() && (static_cast<unsigned char>(w.back()) & 0xC0) == 0x80) w.pop_back();
            targets.push_back({sid, p, "Source " + std::to_string(n) + ": " + w});
        }
    }
    // s129: the footnotes, each by its number and first words.
    for (const auto& sid : m_doc.ids_of<core::Story>()) {
        const auto& st = *m_doc.as<core::Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            for (int k = 0; k < static_cast<int>(para.notes.size()); ++k) {
                std::string w = para.notes[static_cast<std::size_t>(k)].text.substr(0, 40);
                std::replace(w.begin(), w.end(), '\n', ' ');
                while (!w.empty() && (static_cast<unsigned char>(w.back()) & 0xC0) == 0x80) w.pop_back();
                const int n = core::note_number(m_doc, st, p, k);
                targets.push_back({sid, p, "Note " + core::note_label(n, m_doc.settings().note_style) + ": " + (w.empty() ? std::string("(no words)") : w), k});
            }
        }
    }

    if (m_xref_pop) {
        m_xref_pop->popdown();
        m_xref_pop->unparent();
        m_xref_pop.reset();
    }
    m_xref_pop = std::make_unique<Gtk::Popover>();
    m_xref_pop->set_name("xref.popover");
    m_xref_pop->set_parent(cv);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto head = Gtk::make_managed<Gtk::Label>(edit ? "Edit Cross-Reference" : "Cross-Reference");
    head->set_xalign(0);
    head->add_css_class("heading");
    box->append(*head);
    auto showrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto showlab = Gtk::make_managed<Gtk::Label>("Shows");
    showlab->add_css_class("dim-label");
    // What it shows: one of five, side by side (a dropdown's own popover inside
    // this one closed both when an item was chosen -- seen under Xvfb).
    auto shows = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    shows->add_css_class("linked");
    shows->set_name("xref.show");
    // s129: a pattern of its own -- # the page, % the words, * the note's number.
    auto pattern = Gtk::make_managed<Gtk::Entry>();
    pattern->set_name("xref.pattern");
    if (m_xref_pattern.empty()) m_xref_pattern = "see page #";
    // s143: one entry, two uses -- Own...'s pattern, Cite's locator ("p. 14").
    const auto pattern_for = [this, pattern](int show) {
        m_xref_entry_fill = true;
        if (show == 5) {
            pattern->set_text(m_xref_locator);
            pattern->set_placeholder_text("a locator, as p. 14 (optional)");
            pattern->set_tooltip_text("Shown after the number: [2, p. 14]");
        } else {
            pattern->set_text(m_xref_pattern);
            pattern->set_placeholder_text("");
            pattern->set_tooltip_text("# the page \u00b7 % its words \u00b7 * the note's number \u00b7 ^ a source's number");
        }
        m_xref_entry_fill = false;
        pattern->set_visible(show == 4 || show == 5);
    };
    pattern->signal_changed().connect([this, pattern] {
        if (m_xref_entry_fill) return;
        (m_xref_show == 5 ? m_xref_locator : m_xref_pattern) = pattern->get_text();
    });
    pattern_for(m_xref_show);
    static const char* names[] = {"Page", "Words", "Both", "Note", "Own\u2026", "Cite"};
    static const char* tips[] = {"Its page number", "Its words", "Its words and page", "Its note's number", "Words of my own",
                                 "A source's number in the bibliography: [3]"};   // s141
    Gtk::ToggleButton* first = nullptr;
    for (int i = 0; i < 6; ++i) {
        auto t = Gtk::make_managed<Gtk::ToggleButton>(names[i]);
        t->set_tooltip_text(tips[i]);
        if (first) t->set_group(*first); else first = t;
        t->set_active(i == std::clamp(m_xref_show, 0, 5));
        t->signal_toggled().connect([this, t, i, pattern_for] {
            if (!t->get_active()) return;
            m_xref_show = i;
            pattern_for(i);
        });
        shows->append(*t);
    }
    showrow->append(*showlab);
    showrow->append(*shows);
    box->append(*showrow);
    box->append(*pattern);
    if (edit) {   // s130: the same place, shown another way
        auto apply = Gtk::make_managed<Gtk::Button>("Apply");
        apply->set_name("xref.apply");
        apply->set_tooltip_text("Keep where it points; show it as chosen above");
        apply->set_halign(Gtk::Align::END);
        apply->signal_clicked().connect([this, story, edit_para, edit_k] {
            const bool did = core::doc::set_ref(m_journal, m_doc, story, edit_para, edit_k,
                                                static_cast<core::RefShow>(std::clamp(m_xref_show, 0, 5)),
                                                m_xref_show == 5 ? m_xref_locator : m_xref_pattern);
            if (auto lg = log::get(log::Area::Shell)) lg->info("xref: edit {}:{} shows {} -> {}", edit_para, edit_k, m_xref_show, did ? "set" : "nothing");
            if (m_xref_pop) m_xref_pop->popdown();
            if (did) m_canvas_view.canvas().model_changed();
            Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
        });
        box->append(*apply);
        m_xref_add = false;
        if (cur.show == core::RefShow::Cite) {   // s143: several sources in one citation
            auto add = Gtk::make_managed<Gtk::CheckButton>("Add the source I click (\u201c[1, 3]\u201d)");
            add->set_name("xref.add");
            add->signal_toggled().connect([this, add] { m_xref_add = add->get_active(); });
            box->append(*add);
        }
        auto to = Gtk::make_managed<Gtk::Label>(cur_at.story.empty() ? "Points nowhere now -- its place is gone. Choose another:" : "Or point it elsewhere:");
        to->set_xalign(0);
        to->add_css_class("dim-label");
        box->append(*to);
    }
    auto list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    list->set_name("xref.targets");
    if (targets.empty()) {
        auto none = Gtk::make_managed<Gtk::Label>("No headings or anchors yet -- mark a place with Anchor Here.");
        none->add_css_class("dim-label");
        none->set_wrap(true);
        none->set_max_width_chars(40);
        list->append(*none);
    }
    for (const auto& t : targets) {
        // s130: where it points now, ticked.
        const bool here = edit && t.story == cur_at.story && t.para == cur_at.at.para &&
                          (t.k >= 0 ? cur_note && core::doc::note_mark_byte(*m_doc.as<core::Story>(t.story), t.para, t.k) == cur_at.at.byte + 3
                                    : !cur_note);
        auto b = Gtk::make_managed<Gtk::Button>((edit ? (here ? "\u2713 " : "    ") : "") + t.label);
        b->add_css_class("flat");
        if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) { l->set_xalign(0); l->set_ellipsize(Pango::EllipsizeMode::END); l->set_max_width_chars(48); }
        const auto ts = t.story;
        const int tp = t.para, tk = t.k;
        b->signal_clicked().connect([this, story, at, ts, tp, tk, edit, edit_para, edit_k] {
            const auto how = static_cast<core::RefShow>(std::clamp(m_xref_show, 0, 5));
            const std::string& words = how == core::RefShow::Cite ? m_xref_locator : m_xref_pattern;   // s143
            if (edit && m_xref_add && tk < 0) {   // s143: one more source in this citation
                const bool did = core::doc::add_cite_source(m_journal, m_doc, story, edit_para, edit_k, ts, tp);
                if (auto lg = log::get(log::Area::Shell)) lg->info("xref: cite {}:{} + {} para {} -> {}", edit_para, edit_k, ts, tp, did ? "set" : "nothing");
                if (m_xref_pop) m_xref_pop->popdown();
                if (did) m_canvas_view.canvas().model_changed();
                Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
                return;
            }
            if (edit) {   // s130: the reference pointed here instead
                const bool did = core::doc::retarget_ref(m_journal, m_doc, story, edit_para, edit_k, ts, tp, tk, how, words);
                if (auto lg = log::get(log::Area::Shell)) lg->info("xref: edit {}:{} to {} para {} note {} -> {}", edit_para, edit_k, ts, tp, tk, did ? "set" : "nothing");
                if (m_xref_pop) m_xref_pop->popdown();
                if (did) {
                    auto& c = m_canvas_view.canvas();
                    c.model_changed();
                    // After the reference's mark (an anchor may have gone in before it).
                    if (const auto* st = m_doc.as<core::Story>(story)) {
                        const auto& txt = st->paras[static_cast<std::size_t>(edit_para)].text;
                        std::size_t i = txt.find(core::kMarkRef);
                        for (int n = 0; n < edit_k && i != std::string::npos; ++n) i = txt.find(core::kMarkRef, i + 3);
                        if (i != std::string::npos) c.place_caret(core::clamp_pos(*st, {edit_para, static_cast<int>(i) + 3}));
                    }
                }
                Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
                return;
            }
            // s129: a note -- an anchor just before its mark (where it is, in the same paragraph, matters below).
            int mark = -1;
            if (tk >= 0)
                if (const auto* tst = m_doc.as<core::Story>(ts)) mark = core::doc::note_mark_byte(*tst, tp, tk);
            const bool did = tk >= 0 ? core::doc::insert_ref_to_note(m_journal, m_doc, story, at, ts, tp, tk, how, words)
                                     : core::doc::insert_ref_to(m_journal, m_doc, story, at, ts, tp, how, words);
            if (auto lg = log::get(log::Area::Shell)) lg->info("xref: to {} para {} ({}) -> {}", ts, tp, m_xref_show, did ? "set" : "nothing");
            if (m_xref_pop) m_xref_pop->popdown();
            if (did) {
                auto& c = m_canvas_view.canvas();
                c.model_changed();
                // After the reference (and after an anchor made before it in the same paragraph).
                const auto* st = m_doc.as<core::Story>(story);
                core::TextPos after{at.para, at.byte + 3};
                if (st && ts == story && tp == at.para && (tk < 0 || at.byte >= mark)) after.byte += 3;
                if (st) c.place_caret(core::clamp_pos(*st, after));
                Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
            }
        });
        list->append(*b);
    }
    auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
    sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sc->set_max_content_height(320);
    sc->set_propagate_natural_height(true);
    sc->set_child(*list);
    box->append(*sc);
    auto anchor = Gtk::make_managed<Gtk::Button>("Anchor Here");
    anchor->set_name("xref.anchor");
    anchor->set_tooltip_text("Mark the cursor's place for cross-references to point at");
    anchor->signal_clicked().connect([this, story, at] {
        const auto id = core::doc::insert_anchor(m_journal, m_doc, story, at);
        if (auto lg = log::get(log::Area::Shell)) lg->info("xref: anchor {} at {}:{}", id, at.para, at.byte);
        if (m_xref_pop) m_xref_pop->popdown();
        auto& c = m_canvas_view.canvas();
        c.model_changed();
        if (const auto* st = m_doc.as<core::Story>(story)) c.place_caret(core::clamp_pos(*st, {at.para, at.byte + 3}));
        Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
    });
    anchor->set_visible(!edit);
    box->append(*anchor);
    m_xref_pop->set_child(*box);
    const auto r = cv.caret_screen();
    if (r.w > 0)
        m_xref_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    m_xref_pop->popup();
}

// s304: Type > Hyperlink... (Ctrl+K) -- the selected words made a link (a
// web address, mailto:, or #anchor); at a caret inside a link, that link
// edited or removed.
void Shell::on_hyperlink() {  // handler
    auto& cv = m_canvas_view.canvas();
    if (!cv.editing()) {
        alert("Select words first", "A hyperlink goes on words: double-click a text frame and select them, then Hyperlink.");
        return;
    }
    const std::string story = cv.edit_story_id();
    auto [a, b] = cv.edit_range();
    if (b < a) std::swap(a, b);
    std::string cur = core::doc::link_at(m_doc, story, a, b);
    if (a == b) {   // a caret: the whole link it stands in
        for (const auto& l : core::doc::links_in(m_doc, story))
            if (l.para == a.para && a.byte >= l.from && a.byte <= l.to && l.target == cur) { a = {l.para, l.from}; b = {l.para, l.to}; break; }
        if (a == b) {
            alert("Select words first", "Select the words the link goes on, then Hyperlink.");
            return;
        }
    }
    if (m_xref_pop) {
        m_xref_pop->popdown();
        m_xref_pop->unparent();
        m_xref_pop.reset();
    }
    m_xref_pop = std::make_unique<Gtk::Popover>();
    m_xref_pop->set_name("link.popover");
    m_xref_pop->set_parent(cv);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto head = Gtk::make_managed<Gtk::Label>(cur.empty() ? "Hyperlink" : "Edit Hyperlink");
    head->set_xalign(0);
    head->add_css_class("heading");
    box->append(*head);
    auto entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_name("link.target");
    entry->set_width_chars(36);
    entry->set_placeholder_text("www.example.com, name@example.com, #anchor");
    entry->set_text(cur.rfind("page:", 0) == 0 ? "page " + m_doc.folio(cur.substr(5)) : cur);
    box->append(*entry);
    // s313: or a heading / an anchored paragraph in the book
    const auto places = core::doc::link_places(m_doc);
    std::vector<Glib::ustring> names{"\u2014 or go to a heading \u2014"};
    guint now = 0;
    for (const auto& pl : places) {
        names.push_back(pl.words);
        const auto& para = m_doc.as<core::Story>(pl.story)->paras[static_cast<std::size_t>(pl.para)];
        if (cur.size() > 1 && cur[0] == '#' && !para.anchors.empty() && para.anchors.front() == cur.substr(1)) now = static_cast<guint>(names.size() - 1);
    }
    auto to = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
    to->set_name("link.place");
    to->set_selected(now);
    if (now > 0) entry->set_text("");
    to->set_sensitive(!places.empty());
    box->append(*to);
    auto hint = Gtk::make_managed<Gtk::Label>("A page: \u201cpage 7\u201d");
    hint->set_xalign(0);
    hint->add_css_class("dim-label");
    box->append(*hint);
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto remove = Gtk::make_managed<Gtk::Button>("Remove");
    remove->set_name("link.remove");
    remove->set_sensitive(!cur.empty());
    auto ok = Gtk::make_managed<Gtk::Button>(cur.empty() ? "Link" : "Change");
    ok->set_name("link.ok");
    ok->add_css_class("suggested-action");
    buttons->append(*remove);
    buttons->append(*ok);
    box->append(*buttons);
    m_xref_pop->set_child(*box);
    const auto apply = [this, story, a, b](const std::string& to) {
        const bool did = core::doc::set_link(m_journal, m_doc, story, a, b, to);
        if (auto lg = log::get(log::Area::Shell)) lg->info("hyperlink: {}:{}-{}:{} -> '{}' ({})", a.para, a.byte, b.para, b.byte, core::doc::normal_link(to), did);
        if (m_xref_pop) m_xref_pop->popdown();
        auto& c = m_canvas_view.canvas();
        if (did) c.model_changed();
        Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
    };
    // s313: a heading picked -- linked to it; "page 7" -- to that page; else as typed
    const auto go = [this, apply, entry, to, places, story, a, b] {
        const guint k = to->get_selected();
        if (k > 0 && k <= places.size()) {
            const auto& pl = places[k - 1];
            const bool did = core::doc::link_to_paragraph(m_journal, m_doc, story, a, b, pl.story, pl.para);
            if (auto lg = log::get(log::Area::Shell)) lg->info("hyperlink: to \u201c{}\u201d ({})", pl.words, did);
            if (m_xref_pop) m_xref_pop->popdown();
            if (did) m_canvas_view.canvas().model_changed();
            Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
            return;
        }
        std::string t = entry->get_text();
        std::string low = t;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* pre : {"page ", "p. ", "p "})
            if (low.rfind(pre, 0) == 0) {
                const auto folio = t.substr(std::string(pre).size());
                for (const auto& pid : m_doc.pages())
                    if (m_doc.folio(pid) == folio) { apply(core::doc::page_link(pid)); return; }
            }
        apply(t);
    };
    ok->signal_clicked().connect(go);
    entry->signal_activate().connect(go);
    remove->signal_clicked().connect([apply] { apply(""); });
    const auto r = cv.caret_screen();
    if (r.w > 0)
        m_xref_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    m_xref_pop->popup();
    entry->grab_focus();
}

// s309: the dictionary the document's words are checked in -- its own
// language, else the app's ("spell.lang", en_US), else the first installed.
// The user's words kept beside the settings. Cheap when nothing changed.
void Shell::ensure_spell() {  // helper
    static bool user = false;
    if (!user) {
        user = true;
        core::spell::set_user_file((std::filesystem::path(m_settings_file).parent_path() / "words.txt").string());
    }
    std::string want = m_doc.settings().language;
    if (want.empty()) want = m_settings.count("spell.lang") ? m_settings["spell.lang"] : std::string("en_US");
    if (core::spell::language() == want) return;
    static std::string failed;   // asked once a language: not again each refresh
    if (failed == want) return;
    std::string err;
    if (!core::spell::load(want, &err)) {
        failed = want;
        if (!core::spell::loaded()) {
            const auto ls = core::spell::languages();
            if (!ls.empty()) core::spell::load(ls.front());
        }
        if (auto lg = log::get(log::Area::Shell)) lg->warn("spelling: {} -- using {}", err, core::spell::language().empty() ? "none" : core::spell::language());
    } else if (auto lg = log::get(log::Area::Shell)) {
        lg->info("spelling: {} loaded", want);
    }
    m_canvas_view.canvas().marks_changed();
}

// s313: Edit > Check Spelling... (F7) -- the book's misspelled words one by
// one, from the text cursor (else the first story's start): each shown in
// place, what it might be, Change / Change All / Skip / Ignore All / Add.
void Shell::on_check_spelling() {  // handler
    ensure_spell();
    if (!core::spell::loaded()) {
        alert("No dictionary", core::spell::built_in() ? "No spelling dictionary was found -- install one (hunspell-en-US)."
                                                       : "Spell check isn't built in: install hunspell-devel and hunspell-en-US, then build again.");
        return;
    }
    if (!m_spell_win) {
        m_spell_win = std::make_unique<Gtk::Window>();
        auto& w = *m_spell_win;
        w.set_name("spell");
        w.set_title("Check Spelling");
        w.set_resizable(false);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
        box->set_margin(16);
        auto left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        auto cap = Gtk::make_managed<Gtk::Label>("Not in the dictionary");
        cap->set_xalign(0);
        cap->add_css_class("cx-field-label");
        left->append(*cap);
        m_spell_word = Gtk::make_managed<Gtk::Label>();
        m_spell_word->set_name("spell.word");
        m_spell_word->set_xalign(0);
        m_spell_word->add_css_class("title-3");
        left->append(*m_spell_word);
        m_spell_where = Gtk::make_managed<Gtk::Label>();
        m_spell_where->set_name("spell.where");
        m_spell_where->set_xalign(0);
        m_spell_where->set_ellipsize(Pango::EllipsizeMode::END);
        m_spell_where->set_max_width_chars(40);
        m_spell_where->add_css_class("dim-label");
        left->append(*m_spell_where);
        auto cap2 = Gtk::make_managed<Gtk::Label>("Change to");
        cap2->set_xalign(0);
        cap2->set_margin_top(6);
        cap2->add_css_class("cx-field-label");
        left->append(*cap2);
        m_spell_to = Gtk::make_managed<Gtk::Entry>();
        m_spell_to->set_name("spell.to");
        left->append(*m_spell_to);
        m_spell_list = Gtk::make_managed<Gtk::ListBox>();
        m_spell_list->set_name("spell.suggestions");
        auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
        sc->set_size_request(260, 150);
        sc->set_child(*m_spell_list);
        left->append(*sc);
        m_spell_list->signal_row_selected().connect([this](Gtk::ListBoxRow* r) {
            if (!r) return;
            if (auto* l = dynamic_cast<Gtk::Label*>(r->get_child())) m_spell_to->set_text(l->get_text());
        });
        m_spell_list->signal_row_activated().connect([this](Gtk::ListBoxRow*) { m_spell_btns[0]->activate(); });
        box->append(*left);
        auto right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        const char* names[] = {"Change", "Change All", "Skip", "Ignore All", "Add", "Start Again"};
        const char* ids[] = {"change", "change_all", "skip", "ignore", "add", "start"};
        m_spell_btns.clear();
        for (int k = 0; k < 6; ++k) {
            auto b = Gtk::make_managed<Gtk::Button>(names[k]);
            b->set_name(std::string("spell.") + ids[k]);
            if (k == 5) b->set_margin_top(10);
            right->append(*b);
            m_spell_btns.push_back(b);
        }
        m_spell_btns[0]->add_css_class("suggested-action");
        box->append(*right);
        w.set_child(*box);
        {   // Escape closes it
            auto keys = Gtk::EventControllerKey::create();
            keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
                if (k != GDK_KEY_Escape) return false;
                m_spell_win->set_visible(false);
                return true;
            }, false);
            w.add_controller(keys);
        }
        m_spell_to->signal_activate().connect([this] { m_spell_btns[0]->activate(); });
        const auto here = [this] { return core::TextPos{m_spell_hit.para, m_spell_hit.to}; };
        m_spell_btns[0]->signal_clicked().connect([this] {   // Change
            const auto h = m_spell_hit;
            const std::string with = m_spell_to->get_text();
            if (h.story.empty() || with.empty()) return;
            core::doc::replace_word(m_journal, m_doc, h.story, h.para, h.from, h.to, h.word, with);
            m_canvas_view.canvas().model_changed();
            spell_next(h.story, {h.para, h.from + static_cast<int>(with.size())});
        });
        m_spell_btns[1]->signal_clicked().connect([this] {   // Change All
            const auto h = m_spell_hit;
            const std::string with = m_spell_to->get_text();
            if (h.story.empty() || with.empty()) return;
            const int n = core::doc::change_all_words(m_journal, m_doc, h.word, with);
            if (auto lg = log::get(log::Area::Shell)) lg->info("spelling: '{}' -> '{}' x{}", h.word, with, n);
            m_canvas_view.canvas().model_changed();
            spell_next(h.story, {h.para, h.from});
        });
        m_spell_btns[2]->signal_clicked().connect([this, here] { if (!m_spell_hit.story.empty()) spell_next(m_spell_hit.story, here()); });
        m_spell_btns[3]->signal_clicked().connect([this, here] {   // Ignore All
            if (m_spell_hit.story.empty()) return;
            core::spell::ignore(m_spell_hit.word);
            m_canvas_view.canvas().marks_changed();
            spell_next(m_spell_hit.story, here());
        });
        m_spell_btns[4]->signal_clicked().connect([this, here] {   // Add
            if (m_spell_hit.story.empty()) return;
            core::spell::add_word(m_spell_hit.word);
            m_canvas_view.canvas().marks_changed();
            spell_next(m_spell_hit.story, here());
        });
        m_spell_btns[5]->signal_clicked().connect([this] {   // Start Again: the book's first story
            const auto order = core::doc::stories_in_order(m_doc);
            if (!order.empty()) spell_next(order.front(), {0, 0});
        });
    }
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) m_spell_win->set_transient_for(*top);
    auto& cv = m_canvas_view.canvas();
    if (cv.editing()) spell_next(cv.edit_story_id(), cv.edit_range().first);
    else {
        const auto order = core::doc::stories_in_order(m_doc);
        spell_next(order.empty() ? std::string() : order.front(), {0, 0});
    }
    m_spell_win->present();
}

void Shell::spell_next(const std::string& story, core::TextPos from) {  // helper
    m_spell_hit = story.empty() ? core::doc::SpellHit{} : core::doc::next_misspelling(m_doc, story, from);
    spell_show();
}

void Shell::spell_show() {  // helper
    const auto& h = m_spell_hit;
    while (auto* r = m_spell_list->get_row_at_index(0)) m_spell_list->remove(*r);
    for (int k = 0; k < 5; ++k) m_spell_btns[static_cast<std::size_t>(k)]->set_sensitive(!h.story.empty());
    if (h.story.empty()) {
        m_spell_word->set_text("No more misspelled words");
        m_spell_where->set_text("");
        m_spell_to->set_text("");
        return;
    }
    m_spell_word->set_text(h.word);
    const auto sg = core::spell::suggest(h.word, 10);
    for (const auto& g : sg) {
        auto l = Gtk::make_managed<Gtk::Label>(g);
        l->set_xalign(0);
        l->set_margin_start(6);
        m_spell_list->append(*l);
    }
    m_spell_to->set_text(sg.empty() ? h.word : sg.front());
    if (!sg.empty()) m_spell_list->select_row(*m_spell_list->get_row_at_index(0));
    // where: its story, its words round it; on the canvas, the word selected
    const auto* st = m_doc.as<core::Story>(h.story);
    std::string where = st ? st->name : std::string();
    const auto flow = layout::flow_story(m_doc, h.story);
    const auto c = layout::caret_at(m_doc, h.story, flow, {h.para, h.from});
    if (c.ok) {
        const auto* f = m_doc.as<core::Frame>(c.frame);
        if (f && m_doc.as<core::Page>(f->owner)) where += " \u00b7 page " + m_doc.folio(f->owner);
        Gtk::Widget* had = m_spell_win ? m_spell_win->get_focus() : nullptr;
        if (f) m_nav.select_items({f->owner});
        m_canvas_view.canvas().select_text(c.frame, {h.para, h.from}, {h.para, h.to}, false);
        if (had) had->grab_focus();
    } else {
        where += flow.frames.empty() ? " \u00b7 not placed" : " \u00b7 overset";
    }
    m_spell_where->set_text(where);
}

}  // namespace composix
