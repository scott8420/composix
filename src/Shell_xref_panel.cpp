#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>

// Shell_xref_panel.cpp -- s133: Cross-References, one panel. Every reference
// in the document: its page, the words round it as set, where it points;
// the broken ones (their place gone, or "??") marked, and a filter to show
// only those. A click on a row selects the reference on its page; Edit...
// opens its picker (s130). Not modal; follows the document (refresh()).
// Hide-on-close, built once.

namespace composix {

void Shell::open_xrefs_panel() {  // helper
    if (!m_xp) {
        m_xp = std::make_unique<Gtk::Window>();
        auto& w = *m_xp;
        w.set_name("xrefs.panel");
        w.set_title("Cross-References");
        w.set_default_size(560, 420);
        w.set_hide_on_close(true);
        w.set_transient_for(*this);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        box->set_margin(12);
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        m_xp_count = Gtk::make_managed<Gtk::Label>();
        m_xp_count->set_name("xrefs.count");
        m_xp_count->set_xalign(0);
        m_xp_count->set_hexpand(true);
        m_xp_count->add_css_class("heading");
        top->append(*m_xp_count);
        m_xp_broken = Gtk::make_managed<Gtk::ToggleButton>("Broken Only");
        m_xp_broken->set_name("xrefs.broken");
        m_xp_broken->set_tooltip_text("Show only references whose place is gone or not on a page");
        m_xp_broken->signal_toggled().connect([this] { xrefs_panel_sync(); });
        top->append(*m_xp_broken);
        box->append(*top);
        m_xp_list = Gtk::make_managed<Gtk::ListBox>();
        m_xp_list->set_name("xrefs.list");
        m_xp_list->add_css_class("boxed-list");
        m_xp_list->set_selection_mode(Gtk::SelectionMode::NONE);
        m_xp_list->set_activate_on_single_click(true);
        m_xp_list->signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
            const int i = row ? row->get_index() : -1;
            if (i < 0 || i >= static_cast<int>(m_xp_rows.size())) return;
            const auto [story, p, k] = m_xp_rows[static_cast<std::size_t>(i)];
            go_to_ref(story, p, k, false);
        });
        auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
        sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sc->set_vexpand(true);
        sc->set_child(*m_xp_list);
        box->append(*sc);
        auto hint = Gtk::make_managed<Gtk::Label>("Click a reference to see it on its page.");
        hint->add_css_class("dim-label");
        hint->set_xalign(0);
        box->append(*hint);
        w.set_child(*box);
    }
    xrefs_panel_sync();
    m_xp->present();
    if (auto lg = log::get(log::Area::Shell)) lg->info("xrefs panel: open");
}

void Shell::xrefs_panel_sync() {  // helper
    if (!m_xp || !m_xp_list) return;
    while (auto* c = m_xp_list->get_first_child()) m_xp_list->remove(*c);
    m_xp_rows.clear();
    const auto rows = layout::xref_list(m_doc, m_pf_flows);
    int broken = 0;
    for (const auto& r : rows) broken += r.broken;
    const int n = static_cast<int>(rows.size());
    m_xp_count->set_text(n == 0 ? std::string("No cross-references")
                                : std::to_string(n) + (n == 1 ? " cross-reference" : " cross-references") +
                                      (broken ? " · " + std::to_string(broken) + " broken" : std::string{}));
    const bool only = m_xp_broken->get_active();
    int shown = 0;
    for (const auto& r : rows) {
        if (only && !r.broken) continue;
        ++shown;
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        row->set_margin(6);
        auto page = Gtk::make_managed<Gtk::Label>(r.page.empty() ? std::string("—") : "p. " + r.page);
        page->add_css_class("dim-label");
        page->add_css_class("numeric");
        page->set_size_request(44, -1);
        page->set_xalign(1);
        page->set_valign(Gtk::Align::START);
        page->set_tooltip_text(r.page.empty() ? "Not on a page (overset, or its story isn't placed)" : "The page it is on");
        row->append(*page);
        auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        text->set_hexpand(true);
        auto ctx = Gtk::make_managed<Gtk::Label>(r.context.empty() ? std::string("(not set)") : r.context);
        ctx->set_xalign(0);
        ctx->set_ellipsize(Pango::EllipsizeMode::START);   // the reference is near the end: keep it
        ctx->set_max_width_chars(56);
        text->append(*ctx);
        std::string where = r.where;
        if (const auto c = where.find(": "); c != std::string::npos) where = where.substr(c + 2);   // "Cross-reference: " off
        where[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(where[0])));
        auto wl = Gtk::make_managed<Gtk::Label>((r.broken ? "⚠ " : "") + where);
        wl->set_xalign(0);
        wl->set_ellipsize(Pango::EllipsizeMode::END);
        wl->set_max_width_chars(56);
        wl->add_css_class(r.broken ? "error" : "dim-label");
        text->append(*wl);
        row->append(*text);
        auto edit = Gtk::make_managed<Gtk::Button>("Edit…");
        edit->set_valign(Gtk::Align::CENTER);
        edit->set_tooltip_text("Change what it shows, or where it points");
        const auto story = r.story;
        const int p = r.para, k = r.k;
        edit->signal_clicked().connect([this, story, p, k] { go_to_ref(story, p, k, true); });
        row->append(*edit);
        auto lr = Gtk::make_managed<Gtk::ListBoxRow>();
        lr->set_child(*row);
        lr->set_activatable(true);
        lr->set_name("xrefs.row");
        m_xp_rows.emplace_back(story, p, k);
        m_xp_list->append(*lr);
    }
    if (shown == 0) {
        auto none = Gtk::make_managed<Gtk::Label>(
            n == 0 ? "None yet. Put the text cursor in your text, then Edit › Insert Special Character › Cross-Reference…"
                   : "None broken.");
        none->set_wrap(true);
        none->add_css_class("dim-label");
        none->set_margin(18);
        m_xp_list->append(*none);
    }
}

void Shell::go_to_ref(const std::string& story, int p, int k, bool edit) {  // helper
    const auto* st = m_doc.as<core::Story>(story);
    if (!st || p < 0 || p >= static_cast<int>(st->paras.size())) return;
    const auto& t = st->paras[static_cast<std::size_t>(p)].text;
    std::size_t i = t.find(core::kMarkRef);
    for (int n = 0; n < k && i != std::string::npos; ++n) i = t.find(core::kMarkRef, i + 3);
    if (i == std::string::npos) return;
    const int b = static_cast<int>(i);
    const auto c = layout::caret_at(m_doc, story, m_pf_flows.get(m_doc, story), {p, b});
    if (!c.ok) {
        alert("This reference isn’t on a page", "Its text is overset or its story isn’t placed: make room, or place the story.");
        return;
    }
    const auto* f = m_doc.as<core::Frame>(c.frame);
    m_nav.select_items({f->owner});
    m_canvas_view.canvas().select_text(c.frame, {p, b}, {p, b + 3}, true);
    if (auto lg = log::get(log::Area::Shell)) lg->info("xrefs: to {} {}:{} (page {}){}", story, p, k, f->owner, edit ? " -- edit" : "");
    if (!edit) return;
    // As go_to_note (s120 fix): the picker only once the canvas shows the caret.
    auto tries = std::make_shared<int>(0);
    Glib::signal_timeout().connect([this, p, k, tries] {
        auto& cv = m_canvas_view.canvas();
        const bool ready = cv.get_mapped() && cv.get_width() > 0 && cv.caret_screen().w > 0;
        if (!ready && ++*tries < 40) return true;
        if (ready) open_xref(p, k);
        return false;
    }, 50);
}

}  // namespace composix
