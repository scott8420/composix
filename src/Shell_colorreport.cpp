#include "Shell.hpp"
#include "Log.hpp"
#include "layout/ColorReport.hpp"
#include "views/Images.hpp"

#include <gtkmm/box.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <plate/plate.hpp>

#include <cstdio>
#include <cstring>

// Shell_colorreport.cpp -- s053: the Colour Report. Every RGB colour a CMYK
// print converts -- the RGB swatches and the placed RGB pictures -- shown
// before and after the press profile, with how far each moves (Delta E 2000)
// in words. layout::color_report does the proofing; this draws it.

namespace composix {

namespace {

// A picture's pixels for the proof: the screen decode, over white.
bool asset_pixels(const core::Asset& a, const std::string& dir, int& w, int& h, std::vector<std::uint8_t>& rgb) {
    auto pic = views::images::get(core::resolve_asset(a, dir), a.stamp);
    if (!pic) return false;
    pic->flush();
    w = pic->get_width();
    h = pic->get_height();
    const int stride = pic->get_stride();
    const auto fmt = pic->get_format();
    if (w <= 0 || h <= 0 || (fmt != Cairo::Surface::Format::ARGB32 && fmt != Cairo::Surface::Format::RGB24)) return false;
    const unsigned char* data = pic->get_data();
    rgb.resize(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint32_t px;
            std::memcpy(&px, data + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4, 4);
            const int A = fmt == Cairo::Surface::Format::ARGB32 ? static_cast<int>(px >> 24) : 255;
            auto* o = &rgb[(static_cast<std::size_t>(y) * w + x) * 3];
            const int R = (px >> 16) & 0xff, G = (px >> 8) & 0xff, B = px & 0xff;   // premultiplied
            o[0] = static_cast<std::uint8_t>(std::min(255, R + 255 - A));
            o[1] = static_cast<std::uint8_t>(std::min(255, G + 255 - A));
            o[2] = static_cast<std::uint8_t>(std::min(255, B + 255 - A));
        }
    return true;
}

// A chip (a swatch) or a little picture (RGB bytes), drawn to fit.
Gtk::DrawingArea* swatch_chip(std::uint32_t rgb, const char* name) {
    auto a = Gtk::make_managed<Gtk::DrawingArea>();
    a->set_name(name);
    a->set_content_width(40);
    a->set_content_height(40);
    a->set_valign(Gtk::Align::CENTER);
    a->set_draw_func([rgb](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->set_source_rgb(((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
        cr->fill_preserve();
        cr->set_source_rgba(0, 0, 0, 0.18);
        cr->set_line_width(1);
        cr->stroke();
    });
    return a;
}

Gtk::DrawingArea* picture_chip(const std::vector<std::uint8_t>& px, int pw, int ph, const char* name) {
    auto a = Gtk::make_managed<Gtk::DrawingArea>();
    a->set_name(name);
    a->set_content_width(56);
    a->set_content_height(40);
    a->set_valign(Gtk::Align::CENTER);
    auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24, std::max(1, pw), std::max(1, ph));
    if (pw > 0 && ph > 0 && px.size() >= static_cast<std::size_t>(pw) * ph * 3) {
        surf->flush();
        auto* d = surf->get_data();
        const int st = surf->get_stride();
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const auto* s = &px[(static_cast<std::size_t>(y) * pw + x) * 3];
                const std::uint32_t v = (std::uint32_t{s[0]} << 16) | (std::uint32_t{s[1]} << 8) | s[2];
                std::memcpy(d + static_cast<std::size_t>(y) * st + static_cast<std::size_t>(x) * 4, &v, 4);
            }
        surf->mark_dirty();
    }
    const bool have = pw > 0 && ph > 0;
    a->set_draw_func([surf, pw, ph, have](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        if (!have) {   // unreadable: a soft plate with a slash
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->set_source_rgba(0.5, 0.5, 0.5, 0.15);
            cr->fill();
            cr->set_source_rgba(0.6, 0.3, 0.3, 0.7);
            cr->move_to(w * 0.25, h * 0.75);
            cr->line_to(w * 0.75, h * 0.25);
            cr->stroke();
            return;
        }
        const double s = std::min(static_cast<double>(w) / pw, static_cast<double>(h) / ph);
        cr->save();
        cr->translate((w - pw * s) / 2, (h - ph * s) / 2);
        cr->scale(s, s);
        cr->set_source(surf, 0, 0);
        cr->paint();
        cr->restore();
    });
    return a;
}

std::string fmt1(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }
std::string cmyk_text(const double c[4]) {
    char b[64];
    std::snprintf(b, sizeof b, "C%.0f M%.0f Y%.0f K%.0f", c[0], c[1], c[2], c[3]);
    return b;
}

}  // namespace

void Shell::on_color_report() {  // handler
    if (!m_report) {
        m_report = std::make_unique<Gtk::Window>();
        auto& w = *m_report;
        w.set_name("report");
        w.set_title("Colour Report");
        w.set_default_size(720, 560);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        box->set_name("report.box");
        box->add_css_class("cx-export");
        box->set_margin(18);
        auto head = Gtk::make_managed<Gtk::Label>("Colour Report");
        head->add_css_class("cx-export-title");
        head->set_xalign(0);
        box->append(*head);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto lab = Gtk::make_managed<Gtk::Label>("Press profile");
        lab->add_css_class("cx-field-label");
        m_report_profile = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({}));
        m_report_profile->set_name("report.profile");
        m_report_profile->set_hexpand(true);
        {
            GtkExpression* ex = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
            gtk_drop_down_set_expression(m_report_profile->gobj(), ex);
            gtk_expression_unref(ex);
            gtk_drop_down_set_enable_search(m_report_profile->gobj(), TRUE);
        }
        row->append(*lab);
        row->append(*m_report_profile);
        box->append(*row);
        m_report_note = Gtk::make_managed<Gtk::Label>();
        m_report_note->set_name("report.note");
        m_report_note->add_css_class("cx-export-note");
        m_report_note->set_xalign(0);
        m_report_note->set_wrap(true);
        box->append(*m_report_note);
        auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll->set_vexpand(true);
        m_report_list = Gtk::make_managed<Gtk::ListBox>();
        m_report_list->set_name("report.list");
        m_report_list->add_css_class("cx-report-list");
        m_report_list->set_selection_mode(Gtk::SelectionMode::NONE);
        m_report_list->set_activate_on_single_click(true);
        m_report_list->signal_row_activated().connect([this](Gtk::ListBoxRow* r) {
            const int i = r->get_index();
            if (i < 0 || i >= static_cast<int>(m_report_pages.size()) || m_report_pages[static_cast<std::size_t>(i)].empty()) return;
            go_to_issue({layout::Issue::Level::Info, "shift", "", m_report_pages[static_cast<std::size_t>(i)], m_report_frames[static_cast<std::size_t>(i)]});
        });
        scroll->set_child(*m_report_list);
        box->append(*scroll);
        auto legend = Gtk::make_managed<Gtk::Label>(
            "Before → after: as Export converts (relative colorimetric, black point compensation). "
            "ΔE 2000 under 2 holds; 5 is plain side by side; 10 and up is out of the press’s gamut. "
            "A picture is judged on a sample of its pixels.");
        legend->add_css_class("cx-export-note");
        legend->set_wrap(true);
        legend->set_xalign(0);
        box->append(*legend);
        w.set_child(*box);
        m_report_profile->property_selected().signal_changed().connect([this] {
            if (!m_report_quiet) report_refresh();
        });
    }
    // The profiles, fresh; the export's picked, else the first.
    m_report_quiet = true;
    m_report_paths.clear();
    std::vector<Glib::ustring> names;
    guint at = 0;
    for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) {
        if (p.path == m_pdf_profile) at = static_cast<guint>(m_report_paths.size());
        m_report_paths.push_back(p.path);
        names.push_back(p.name);
    }
    if (names.empty()) names.push_back("No CMYK press profile on this computer");
    m_report_profile->set_model(Gtk::StringList::create(names));
    m_report_profile->set_selected(at);
    m_report_profile->set_sensitive(!m_report_paths.empty());
    m_report_quiet = false;
    report_refresh();
    m_report->set_transient_for(*this);
    m_report->present();
}

void Shell::report_refresh() {  // helper
    if (!m_report) return;
    while (auto* c = m_report_list->get_first_child()) m_report_list->remove(*c);
    m_report_pages.clear();
    m_report_frames.clear();
    const auto i = m_report_profile->get_selected();
    if (m_report_paths.empty() || i >= m_report_paths.size()) {
        m_report_note->set_text("Install a CMYK press profile (your printer's, or GRACoL / FOGRA) in ~/.local/share/icc to see "
                                "what your RGB colours become.");
        return;
    }
    const std::string dir = m_project_dir;
    const auto rep = layout::color_report(m_doc, m_report_paths[i], false,
                                          [dir](const core::Asset& a, int& w, int& h, std::vector<std::uint8_t>& rgb) {
                                              return asset_pixels(a, dir, w, h, rgb);
                                          });
    if (!rep.ok) {
        m_report_note->set_text("Can’t use that profile: " + rep.err);
        return;
    }
    if (rep.rows.empty()) {
        m_report_note->set_text("Nothing in RGB — every colour and picture is already CMYK, spot or grey. Nothing will convert.");
        return;
    }
    const int n = static_cast<int>(rep.rows.size());
    m_report_note->set_text(std::to_string(n) + (n == 1 ? " RGB colour or picture" : " RGB colours and pictures") +
                            " through " + rep.profile_name + " — " +
                            (rep.worst_count == 0 ? std::string("all convert well.")
                                                  : std::to_string(rep.worst_count) + " will shift noticeably."));
    int k = 0;
    for (const auto& r : rep.rows) {
        const std::string base = "report.row." + std::to_string(k++);
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        line->set_name(base);
        line->add_css_class("cx-report-row");
        if (!r.used) line->set_opacity(0.55);
        const bool pic = r.kind == layout::ColorRow::Kind::Image;
        if (pic) {
            line->append(*picture_chip(r.px_before, r.w, r.h, "report.before"));
            auto arrow = Gtk::make_managed<Gtk::Label>("→");
            arrow->add_css_class("cx-export-note");
            line->append(*arrow);
            line->append(*picture_chip(r.px_after, r.w, r.h, "report.after"));
        } else {
            line->append(*swatch_chip(r.before, "report.before"));
            auto arrow = Gtk::make_managed<Gtk::Label>("→");
            arrow->add_css_class("cx-export-note");
            line->append(*arrow);
            line->append(*swatch_chip(r.after, "report.after"));
        }
        auto names = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        names->set_hexpand(true);
        names->set_valign(Gtk::Align::CENTER);
        auto title = Gtk::make_managed<Gtk::Label>(r.name);
        title->add_css_class("cx-tile-title");
        title->set_xalign(0);
        title->set_ellipsize(Pango::EllipsizeMode::END);
        auto sub = Gtk::make_managed<Gtk::Label>((pic ? "Picture · " : "Swatch · ") + r.where);
        sub->add_css_class("cx-export-note");
        sub->set_xalign(0);
        names->append(*title);
        names->append(*sub);
        line->append(*names);
        auto detail = Gtk::make_managed<Gtk::Label>();
        detail->add_css_class("cx-export-note");
        detail->set_xalign(1);
        if (pic && !r.readable) {
            const auto* a = m_doc.as<core::Asset>(r.id);
            const auto st = a ? core::link_status(*a, dir) : core::LinkStatus::Missing;
            detail->set_text(st == core::LinkStatus::Missing || st == core::LinkStatus::EmbeddedMissing
                                 ? "file missing" : "can’t read its pixels");
        }
        else if (pic) detail->set_text("ΔE avg " + fmt1(r.delta_e) + " · worst " + fmt1(r.worst) + "\n" +
                                       std::to_string(static_cast<int>(std::lround(r.share * 100))) + "% shifts noticeably");
        else detail->set_text(cmyk_text(r.cmyk) + "\nΔE " + fmt1(r.delta_e));
        detail->set_justify(Gtk::Justification::RIGHT);
        line->append(*detail);
        auto verdict = Gtk::make_managed<Gtk::Label>(pic && !r.readable ? "Unknown" : layout::shift_text(r.shift));
        verdict->set_name(base + ".verdict");
        verdict->add_css_class("cx-badge");
        verdict->set_valign(Gtk::Align::CENTER);
        verdict->set_width_chars(16);
        if (!(pic && !r.readable))
            verdict->add_css_class(r.shift == layout::Shift::Holds ? "cx-good" : r.shift == layout::Shift::Slight ? "cx-mild"
                                   : r.shift == layout::Shift::Noticeable ? "cx-warn" : "cx-bad");
        line->append(*verdict);
        m_report_list->append(*line);
        m_report_pages.push_back(r.page);
        // A picture: its first frame on that page, to pick.
        std::string frame;
        if (pic)
            for (const auto& fid : m_doc.ids_of<core::Frame>()) {
                const auto& f = *m_doc.as<core::Frame>(fid);
                if (f.asset == r.id && f.owner == r.page) { frame = fid; break; }
            }
        m_report_frames.push_back(frame);
    }
    if (auto lg = log::get(log::Area::Shell)) lg->info("colour report: {} rows, {} shifting ({})", n, rep.worst_count, rep.profile_name);
}

}  // namespace composix
