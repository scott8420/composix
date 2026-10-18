#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
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
            const bool anchored = para.text.find(core::kMarkAnchor) != std::string::npos;
            if (!anchored && !heading_style(m_doc, para.style, listed)) continue;
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
    pattern->set_text(m_xref_pattern.empty() ? std::string("see page #") : m_xref_pattern);
    pattern->set_tooltip_text("# the page \u00b7 % its words \u00b7 * the note's number");
    pattern->set_visible(m_xref_show == 4);
    pattern->signal_changed().connect([this, pattern] { m_xref_pattern = pattern->get_text(); });
    m_xref_pattern = pattern->get_text();
    static const char* names[] = {"Page", "Words", "Both", "Note", "Own\u2026"};
    static const char* tips[] = {"Its page number", "Its words", "Its words and page", "Its note's number", "Words of my own"};
    Gtk::ToggleButton* first = nullptr;
    for (int i = 0; i < 5; ++i) {
        auto t = Gtk::make_managed<Gtk::ToggleButton>(names[i]);
        t->set_tooltip_text(tips[i]);
        if (first) t->set_group(*first); else first = t;
        t->set_active(i == std::clamp(m_xref_show, 0, 4));
        t->signal_toggled().connect([this, t, i, pattern] {
            if (!t->get_active()) return;
            m_xref_show = i;
            pattern->set_visible(i == 4);
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
                                                static_cast<core::RefShow>(std::clamp(m_xref_show, 0, 4)), m_xref_pattern);
            if (auto lg = log::get(log::Area::Shell)) lg->info("xref: edit {}:{} shows {} -> {}", edit_para, edit_k, m_xref_show, did ? "set" : "nothing");
            if (m_xref_pop) m_xref_pop->popdown();
            if (did) m_canvas_view.canvas().model_changed();
            Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
        });
        box->append(*apply);
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
            const auto how = static_cast<core::RefShow>(std::clamp(m_xref_show, 0, 4));
            if (edit) {   // s130: the reference pointed here instead
                const bool did = core::doc::retarget_ref(m_journal, m_doc, story, edit_para, edit_k, ts, tp, tk, how, m_xref_pattern);
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
            const bool did = tk >= 0 ? core::doc::insert_ref_to_note(m_journal, m_doc, story, at, ts, tp, tk, how, m_xref_pattern)
                                     : core::doc::insert_ref_to(m_journal, m_doc, story, at, ts, tp, how, m_xref_pattern);
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

}  // namespace composix
