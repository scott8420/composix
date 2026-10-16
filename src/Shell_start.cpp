#include "Shell.hpp"
#include "Log.hpp"
#include "layout/Export.hpp"
#include "views/Images.hpp"
#include "core/Recents.hpp"
#include <glibmm/main.h>
#include <gtkmm/overlay.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

#include <chrono>

// Shell_start.cpp -- s064: the start screen. New, Open, the Sample Book,
// and the recent projects as cards: the first page drawn, the title, its
// trim and length, when it was last saved. Shown at launch when there's no
// project to reopen, and from File > Start Screen.

namespace composix {

namespace {
constexpr int kCardH = 168;   // the page's height on a card (px)

// A project's first page (with anything on it), drawn as the thumbnails are (a soft plate for a
// picture that can't be had).
Cairo::RefPtr<Cairo::ImageSurface> first_page(const core::Doc& d, const std::string& dir) {
    const auto pages = d.pages();
    if (pages.empty()) return {};
    // The first page with something of its own on it (a book's first pages
    // are often blank), else the first.
    std::string show = pages.front();
    for (const auto& p : pages)
        if (!d.frames_on(p).empty()) { show = p; break; }
    const auto* pg = d.as<core::Page>(show);
    if (!pg || pg->w <= 0 || pg->h <= 0) return {};
    const double k = kCardH / pg->h;
    auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, std::max(1, static_cast<int>(pg->w * k)), kCardH);
    auto cr = Cairo::Context::create(surf);
    cr->set_source_rgb(1, 1, 1);
    cr->paint();
    cr->scale(k, k);
    layout::FlowCache flows;
    const layout::ImageSource images = [dir](const core::Asset& a, double, double) -> cairo_surface_t* {
        auto pic = views::images::get(core::resolve_asset(a, dir), a.stamp);
        return pic ? cairo_surface_reference(pic->cobj()) : nullptr;
    };
    layout::PageDraw how;
    how.placeholders = true;
    layout::draw_page(cr->cobj(), d, show, flows, images, nullptr, how);
    return surf;
}
}  // namespace

void Shell::build_start() {  // zone
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_name("start");
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    box->add_css_class("cx-start");
    box->set_margin_start(40);
    box->set_margin_end(40);
    box->set_margin_top(36);
    box->set_margin_bottom(36);
    auto title = Gtk::make_managed<Gtk::Label>("Composix");
    title->add_css_class("cx-start-title");
    title->set_xalign(0);
    auto sub = Gtk::make_managed<Gtk::Label>("Books and pages, set with care.");
    sub->add_css_class("cx-start-sub");
    sub->set_xalign(0);
    box->append(*title);
    box->append(*sub);
    auto acts = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    for (const auto& [label, action, suggested] : {std::tuple{"New Document…", "win.new", true},
                                                   std::tuple{"Open Project…", "win.open", false},
                                                   std::tuple{"Sample Book", "win.open-sample", false}}) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->set_name(std::string("start.") + (action + 4));
        b->set_action_name(action);
        if (suggested) b->add_css_class("suggested-action");
        acts->append(*b);
    }
    box->append(*acts);
    auto recent = Gtk::make_managed<Gtk::Label>("Recent projects");
    recent->add_css_class("cx-tile-group");
    recent->set_xalign(0);
    recent->set_margin_top(10);
    box->append(*recent);
    m_start_empty = Gtk::make_managed<Gtk::Label>("Nothing yet — projects you open or save show up here.");
    m_start_empty->add_css_class("cx-export-note");
    m_start_empty->set_xalign(0);
    box->append(*m_start_empty);
    m_start_grid = Gtk::make_managed<Gtk::FlowBox>();
    m_start_grid->set_name("start.recent");
    m_start_grid->add_css_class("cx-tile-grid");
    m_start_grid->set_selection_mode(Gtk::SelectionMode::NONE);
    m_start_grid->set_activate_on_single_click(true);
    m_start_grid->set_homogeneous(true);
    m_start_grid->set_max_children_per_line(8);
    m_start_grid->set_column_spacing(18);
    m_start_grid->set_row_spacing(18);
    m_start_grid->set_valign(Gtk::Align::START);
    m_start_grid->signal_child_activated().connect([this](Gtk::FlowBoxChild* c) {
        const auto it = m_start_paths.find(c);
        if (it == m_start_paths.end()) return;
        const std::string path = it->second;
        confirm_discard([this, path] { open_project(path); });
    });
    box->append(*m_start_grid);
    scroll->set_child(*box);
    m_main.add(*scroll, "start");
}

void Shell::show_start() {  // helper
    while (auto* c = m_start_grid->get_first_child()) m_start_grid->remove(*c);
    m_start_paths.clear();
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    int n = 0;
    for (const auto& path : m_recents) {
        core::Doc d;
        const auto sum = core::summarize_project(path, &d);
        if (!sum.ok) continue;
        auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_name("start.card." + std::to_string(n++));
        child->add_css_class("cx-tile");
        child->set_tooltip_text(path);
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("cx-tile-card");
        auto pic = first_page(d, path);
        auto area = Gtk::make_managed<Gtk::DrawingArea>();
        area->set_content_width(150);
        area->set_content_height(kCardH);
        area->set_draw_func([pic](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            if (!pic) return;
            const double x = (w - pic->get_width()) / 2.0, y = (h - pic->get_height()) / 2.0;
            cr->set_source_rgba(0, 0, 0, 0.12);
            cr->rectangle(x + 1, y + 2, pic->get_width(), pic->get_height());
            cr->fill();
            cr->set_source(pic, x, y);
            cr->paint();
            cr->rectangle(x + 0.5, y + 0.5, pic->get_width() - 1, pic->get_height() - 1);   // the page's edge
            cr->set_source_rgba(0, 0, 0, 0.18);
            cr->set_line_width(1);
            cr->stroke();
        });
        card->append(*area);
        auto t = Gtk::make_managed<Gtk::Label>(sum.title);
        t->add_css_class("cx-tile-title");
        t->set_xalign(0);
        t->set_ellipsize(Pango::EllipsizeMode::END);
        t->set_max_width_chars(1);
        t->set_hexpand(true);
        card->append(*t);
        auto meta = Gtk::make_managed<Gtk::Label>(core::size_text(sum.trim_w, sum.trim_h, 0) + " · " + std::to_string(sum.pages) +
                                                  (sum.pages == 1 ? " page" : " pages") +
                                                  (sum.saved ? "\nsaved " + core::ago_text(now, sum.saved) : std::string{}));
        meta->add_css_class("cx-field-value");
        meta->add_css_class("cx-tile-meta");
        meta->set_xalign(0);
        card->append(*meta);
        // Off the list (the project itself stays where it is).
        auto rm = Gtk::make_managed<Gtk::Button>();
        rm->set_icon_name("window-close-symbolic");
        rm->add_css_class("flat");
        rm->add_css_class("circular");
        rm->set_tooltip_text("Remove from the list");
        rm->set_halign(Gtk::Align::END);
        rm->set_valign(Gtk::Align::START);
        rm->signal_clicked().connect([this, path] {
            core::recents_remove(m_recents, path);
            core::save_recents(m_recents_file, m_recents);
            rebuild_recents_menu();
            Glib::signal_idle().connect_once([this] { show_start(); });
        });
        auto over = Gtk::make_managed<Gtk::Overlay>();
        over->set_child(*card);
        over->add_overlay(*rm);
        card->set_size_request(176, -1);
        child->set_child(*over);
        m_start_grid->append(*child);
        m_start_paths[child] = path;
    }
    m_start_empty->set_visible(n == 0);
    m_start_grid->set_visible(n > 0);
    m_main.set_visible_child("start");
    sync_up();   // s068
    m_canvas_view.set_title("", 0);
    if (auto lg = log::get(log::Area::Shell)) lg->info("start screen: {} recent project(s)", n);
}

}  // namespace composix
