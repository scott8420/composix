#include "Shell.hpp"
#include "Log.hpp"
#include <gtkmm/box.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>

// Shell_preflight.cpp -- s024: Preflight, live. A button in the header shows
// the count of problems (green when none); its list names each one and a
// click goes to it. The checks are layout::preflight_issues -- the same ones
// Export's report gives.

namespace composix {

void Shell::build_preflight() {  // zone
    m_preflight = Gtk::make_managed<Gtk::MenuButton>();
    m_preflight->set_name("shell.preflight");
    m_preflight->add_css_class("cx-preflight");
    m_preflight->set_tooltip_text("Preflight: what would trouble a printer");
    m_preflight_label = Gtk::make_managed<Gtk::Label>("✓");
    m_preflight_label->set_name("shell.preflight.label");
    m_preflight->set_child(*m_preflight_label);

    auto pop = Gtk::make_managed<Gtk::Popover>();
    pop->set_name("preflight.popover");
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(6);
    auto head = Gtk::make_managed<Gtk::Label>("Preflight");
    head->add_css_class("cx-export-title");
    head->set_xalign(0);
    box->append(*head);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_max_content_height(420);
    scroll->set_propagate_natural_height(true);
    scroll->set_min_content_width(380);
    scroll->set_propagate_natural_width(true);
    scroll->set_size_request(400, -1);
    m_preflight_list = Gtk::make_managed<Gtk::ListBox>();
    m_preflight_list->set_name("preflight.list");
    m_preflight_list->add_css_class("cx-preflight-list");
    m_preflight_list->set_selection_mode(Gtk::SelectionMode::NONE);
    m_preflight_list->set_activate_on_single_click(true);
    m_preflight_list->signal_row_activated().connect([this, pop](Gtk::ListBoxRow* row) {
        const int i = row->get_index();
        if (i < 0 || i >= static_cast<int>(m_issues.size())) return;
        pop->popdown();
        go_to_issue(m_issues[static_cast<std::size_t>(i)]);
    });
    scroll->set_child(*m_preflight_list);
    box->append(*scroll);
    pop->set_child(*box);
    m_preflight->set_popover(*pop);
}

void Shell::update_preflight() {  // helper
    if (!m_preflight) return;
    layout::PreflightOpts o;
    o.standard = m_pdf_print ? m_pdf_standard : 0;
    o.profile = !m_pdf_profile.empty();
    o.missing_profile = m_profile_missing;   // s032
    o.profile_path = m_pdf_print ? m_pdf_profile : std::string{};   // s053
    o.convert = m_pdf_convert || o.standard == 1;
    o.perceptual = m_pdf_perceptual;   // s085
    const std::string dir = m_project_dir;
    o.link = [dir](const core::Asset& a) { return core::link_status(a, dir); };
    m_issues = layout::preflight_issues(m_doc, m_pf_flows, o);
    const int n = layout::problem_count(m_issues);
    bool err = false;
    for (const auto& i : m_issues) err = err || i.level == layout::Issue::Level::Error;
    m_preflight_label->set_text(n == 0 ? "✓ Preflight" : "● " + std::to_string(n));
    for (const char* c : {"cx-ok", "cx-warn", "cx-bad"}) m_preflight->remove_css_class(c);
    m_preflight->add_css_class(n == 0 ? "cx-ok" : err ? "cx-bad" : "cx-warn");
    m_preflight->set_tooltip_text(n == 0 ? "Preflight: no problems" :
                                  std::to_string(n) + (n == 1 ? " problem" : " problems") + " \u2014 click to see");

    while (auto* c = m_preflight_list->get_first_child()) m_preflight_list->remove(*c);
    if (m_issues.empty()) {
        auto l = Gtk::make_managed<Gtk::Label>("Nothing to fix.");
        l->add_css_class("cx-export-note");
        l->set_margin(8);
        m_preflight_list->append(*l);
        return;
    }
    for (std::size_t k = 0; k < m_issues.size(); ++k) {
        const auto& i = m_issues[k];
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_name("preflight.issue." + std::to_string(k));
        row->set_margin(4);
        auto dot = Gtk::make_managed<Gtk::Label>("●");
        dot->add_css_class(i.level == layout::Issue::Level::Error ? "cx-dot-bad"
                           : i.level == layout::Issue::Level::Warning ? "cx-dot-warn" : "cx-dot-info");
        auto text = Gtk::make_managed<Gtk::Label>(i.text);
        text->set_xalign(0);
        text->set_wrap(true);
        text->set_max_width_chars(48);
        text->set_width_chars(40);
        text->set_hexpand(true);
        row->append(*dot);
        row->append(*text);
        m_preflight_list->append(*row);
    }
}

void Shell::go_to_issue(const layout::Issue& i) {  // helper
    if (i.page.empty()) {   // nowhere in particular: Export is where it's fixed
        if (i.kind == "profile") on_export_pdf();
        return;
    }
    m_nav.select_items({i.page});
    auto& cv = m_canvas_view.canvas();
    if (const auto* f = m_doc.as<core::Frame>(i.frame); f && f->owner == i.page) {
        cv.set_selection({i.frame});
        on_canvas_selection();
    }
    if (auto lg = log::get(log::Area::Shell)) lg->info("preflight: go to {} {}", i.page, i.frame);
}

}  // namespace composix
