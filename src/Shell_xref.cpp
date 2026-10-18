#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>

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
    auto& cv = m_canvas_view.canvas();
    if (!cv.editing()) {
        alert("Click into text first", "A cross-reference goes in at the text cursor: double-click a text frame, then insert.");
        return;
    }
    const std::string story = cv.edit_story_id();
    const auto at = cv.edit_range().second;
    // The places to point at.
    std::vector<std::string> listed;   // styles a Contents list gathers
    for (const auto& lid : m_doc.ids_of<core::GenList>()) {
        const auto& l = *m_doc.as<core::GenList>(lid);
        if (l.kind == core::ListKind::Contents) listed.insert(listed.end(), l.from_styles.begin(), l.from_styles.end());
    }
    struct Target { std::string story; int para; std::string label; };
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
    auto head = Gtk::make_managed<Gtk::Label>("Cross-Reference");
    head->set_xalign(0);
    head->add_css_class("heading");
    box->append(*head);
    auto showrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto showlab = Gtk::make_managed<Gtk::Label>("Shows");
    showlab->add_css_class("dim-label");
    auto show = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Its page number", "Its words", "Its words and page"}));
    show->set_name("xref.show");
    show->set_selected(static_cast<guint>(m_xref_show));
    show->property_selected().signal_changed().connect([this, show] { m_xref_show = static_cast<int>(show->get_selected()); });
    showrow->append(*showlab);
    showrow->append(*show);
    box->append(*showrow);
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
        auto b = Gtk::make_managed<Gtk::Button>(t.label);
        b->add_css_class("flat");
        if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) { l->set_xalign(0); l->set_ellipsize(Pango::EllipsizeMode::END); l->set_max_width_chars(48); }
        const auto ts = t.story;
        const int tp = t.para;
        b->signal_clicked().connect([this, story, at, ts, tp] {
            const auto how = static_cast<core::RefShow>(std::clamp(m_xref_show, 0, 2));
            const bool did = core::doc::insert_ref_to(m_journal, m_doc, story, at, ts, tp, how);
            if (auto lg = log::get(log::Area::Shell)) lg->info("xref: to {} para {} ({}) -> {}", ts, tp, m_xref_show, did ? "set" : "nothing");
            if (m_xref_pop) m_xref_pop->popdown();
            if (did) {
                auto& c = m_canvas_view.canvas();
                c.model_changed();
                // After the reference (and after an anchor made before it in the same paragraph).
                const auto* st = m_doc.as<core::Story>(story);
                core::TextPos after{at.para, at.byte + 3};
                if (st && ts == story && tp == at.para) after.byte += 3;
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
    box->append(*anchor);
    m_xref_pop->set_child(*box);
    const auto r = cv.caret_screen();
    if (r.w > 0)
        m_xref_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    m_xref_pop->popup();
}

}  // namespace composix
