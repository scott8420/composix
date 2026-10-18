#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>

// Shell_sources_panel.cpp -- s144: Sources, one panel. The bibliography's
// sources, each with its number, how often it is cited and on which pages
// (the uncited marked); then the citations that point nowhere, in red. A
// click on a source selects it on its page; on a broken citation, that
// citation. Cite puts a citation of the source at the text cursor. Not
// modal; follows the document (refresh()). Hide-on-close, built once.

namespace composix {

void Shell::open_sources_panel() {  // helper
    if (!m_sp) {
        m_sp = std::make_unique<Gtk::Window>();
        auto& w = *m_sp;
        w.set_name("sources.panel");
        w.set_title("Sources");
        w.set_default_size(560, 420);
        w.set_hide_on_close(true);
        w.set_transient_for(*this);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        box->set_margin(12);
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        m_sp_count = Gtk::make_managed<Gtk::Label>();
        m_sp_count->set_name("sources.count");
        m_sp_count->set_xalign(0);
        m_sp_count->set_hexpand(true);
        m_sp_count->add_css_class("heading");
        top->append(*m_sp_count);
        m_sp_uncited = Gtk::make_managed<Gtk::ToggleButton>("Uncited Only");
        m_sp_uncited->set_name("sources.uncited");
        m_sp_uncited->set_tooltip_text("Show only sources no citation names (and the broken citations)");
        m_sp_uncited->signal_toggled().connect([this] { sources_panel_sync(); });
        top->append(*m_sp_uncited);
        box->append(*top);
        m_sp_list = Gtk::make_managed<Gtk::ListBox>();
        m_sp_list->set_name("sources.list");
        m_sp_list->add_css_class("boxed-list");
        m_sp_list->set_selection_mode(Gtk::SelectionMode::NONE);
        m_sp_list->set_activate_on_single_click(true);
        m_sp_list->signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            const int i = row ? row->get_index() : -1;
            if (i < 0 || i >= static_cast<int>(m_sp_rows.size())) return;
            const auto [story, p, k] = m_sp_rows[static_cast<std::size_t>(i)];
            if (k >= 0) { go_to_ref(story, p, k, false); return; }   // a broken citation
            // A source: its paragraph selected on its page.
            const auto* st = m_doc.as<core::Story>(story);
            if (!st || p >= static_cast<int>(st->paras.size())) return;
            const auto c = layout::caret_at(m_doc, story, m_pf_flows.get(m_doc, story), {p, 0});
            if (!c.ok) {
                alert("This source isn’t on a page", "Its text is overset or its story isn’t placed.");
                return;
            }
            const auto* f = m_doc.as<core::Frame>(c.frame);
            m_nav.select_items({f->owner});
            m_canvas_view.canvas().select_text(c.frame, {p, 0}, {p, static_cast<int>(st->paras[static_cast<std::size_t>(p)].text.size())}, true);
            if (auto lg = log::get(log::Area::Shell)) lg->info("sources: to {} {} (page {})", story, p, f->owner);
        });
        auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
        sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sc->set_vexpand(true);
        sc->set_child(*m_sp_list);
        box->append(*sc);
        auto hint = Gtk::make_managed<Gtk::Label>("Click a source to see it; Cite puts it at the text cursor.");
        hint->add_css_class("dim-label");
        hint->set_xalign(0);
        box->append(*hint);
        w.set_child(*box);
    }
    sources_panel_sync();
    m_sp->present();
    if (auto lg = log::get(log::Area::Shell)) lg->info("sources panel: open");
}

void Shell::sources_panel_sync() {  // helper
    if (!m_sp || !m_sp_list) return;
    while (auto* c = m_sp_list->get_first_child()) m_sp_list->remove(*c);
    m_sp_rows.clear();
    const auto rows = layout::source_list(m_doc, m_pf_flows);
    std::vector<layout::XRefRow> broken;
    for (auto& r : layout::xref_list(m_doc, m_pf_flows)) {
        const auto* st = m_doc.as<core::Story>(r.story);
        if (r.broken && st && st->paras[static_cast<std::size_t>(r.para)].refs[static_cast<std::size_t>(r.k)].show == core::RefShow::Cite)
            broken.push_back(std::move(r));
    }
    int uncited = 0;
    for (const auto& r : rows) uncited += r.cites == 0;
    const int n = static_cast<int>(rows.size());
    std::string head = n == 0 ? std::string("No sources") : std::to_string(n) + (n == 1 ? " source" : " sources");
    if (uncited) head += " · " + std::to_string(uncited) + " uncited";
    if (!broken.empty()) head += " · " + std::to_string(broken.size()) + (broken.size() == 1 ? " broken citation" : " broken citations");
    m_sp_count->set_text(head);
    const bool only = m_sp_uncited->get_active();
    for (const auto& r : rows) {
        if (only && r.cites > 0) continue;
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        row->set_margin(6);
        auto num = Gtk::make_managed<Gtk::Label>("[" + std::to_string(r.number) + "]");
        num->add_css_class("numeric");
        num->set_size_request(36, -1);
        num->set_xalign(1);
        num->set_valign(Gtk::Align::START);
        row->append(*num);
        auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        text->set_hexpand(true);
        auto w = Gtk::make_managed<Gtk::Label>(r.words.empty() ? std::string("(no words)") : r.words);
        w->set_xalign(0);
        w->set_ellipsize(Pango::EllipsizeMode::END);
        w->set_max_width_chars(52);
        text->append(*w);
        std::string pages;
        for (const auto& f : r.pages) pages += (pages.empty() ? "" : ", ") + f;
        auto how = Gtk::make_managed<Gtk::Label>(r.cites == 0 ? std::string("Not cited")
                                                              : "Cited " + std::to_string(r.cites) + (r.cites == 1 ? " time" : " times") +
                                                                    (pages.empty() ? std::string{} : (r.pages.size() == 1 ? " · page " : " · pages ") + pages));
        how->set_xalign(0);
        how->add_css_class(r.cites == 0 ? "warning" : "dim-label");
        text->append(*how);
        row->append(*text);
        auto cite = Gtk::make_managed<Gtk::Button>("Cite");
        cite->set_valign(Gtk::Align::CENTER);
        cite->set_tooltip_text("A citation of it at the text cursor");
        const auto story = r.story;
        const int p = r.para;
        cite->signal_clicked().connect([this, story, p] {
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) {
                alert("Click into text first", "A citation goes in at the text cursor: double-click a text frame where it belongs, then Cite.");
                return;
            }
            const auto at = cv.edit_range().second;
            const bool did = core::doc::insert_citation(m_journal, m_doc, cv.edit_story_id(), at, story, p);
            if (auto lg = log::get(log::Area::Shell)) lg->info("sources: cite {} {} -> {}", story, p, did ? "set" : "nothing");
            if (!did) return;
            cv.model_changed();
            // After the citation (and after an anchor made before it in the same paragraph).
            const auto* st = m_doc.as<core::Story>(cv.edit_story_id());
            core::TextPos after{at.para, at.byte + 3};
            if (st && cv.edit_story_id() == story && at.para == p) after.byte += 3;
            if (st) cv.place_caret(core::clamp_pos(*st, after));
            present();
            Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
        });
        row->append(*cite);
        auto lr = Gtk::make_managed<Gtk::ListBoxRow>();
        lr->set_child(*row);
        lr->set_name("sources.row");
        m_sp_list->append(*lr);
        m_sp_rows.emplace_back(story, p, -1);
    }
    for (const auto& b : broken) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        row->set_margin(6);
        auto w = Gtk::make_managed<Gtk::Label>("⚠ A citation whose source is gone" + (b.page.empty() ? std::string{} : " · page " + b.page));
        w->set_xalign(0);
        w->add_css_class("error");
        row->append(*w);
        auto ctx = Gtk::make_managed<Gtk::Label>(b.context);
        ctx->set_xalign(0);
        ctx->set_ellipsize(Pango::EllipsizeMode::START);
        ctx->set_max_width_chars(56);
        ctx->add_css_class("dim-label");
        row->append(*ctx);
        auto lr = Gtk::make_managed<Gtk::ListBoxRow>();
        lr->set_child(*row);
        m_sp_list->append(*lr);
        m_sp_rows.emplace_back(b.story, b.para, b.k);
    }
    if (m_sp_rows.empty()) {
        auto none = Gtk::make_managed<Gtk::Label>(n == 0 ? "No sources yet. A Bibliography list gathers paragraphs in its style -- type them, then Build it."
                                                         : "Every source is cited.");
        none->set_wrap(true);
        none->add_css_class("dim-label");
        none->set_margin(18);
        m_sp_list->append(*none);
    }
}

}  // namespace composix
