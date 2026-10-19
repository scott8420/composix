#include "AboutWindow.hpp"
#include "widgets/Widgets.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/enums.h>
#include <gtkmm/expander.h>
#include <gtk/gtk.h>
#include <pango/pango.h>
#include <cairo.h>
#include <hb.h>
#include <lcms2.h>
#include <cstdio>
#include <string>

namespace composix {

AboutWindow::AboutWindow(Gtk::Window& parent) {
    set_name("shell.about");
    registry::add("shell.about", this);

    set_title("About Composix");
    set_modal(true);
    set_resizable(false);
    set_default_size(380, 340);
    set_transient_for(parent);
    set_hide_on_close(true);   // singleton: X hides, never destructs mid-session

    // Raw Gtk::HeaderBar, matching Shell's headerbar (Cairn doesn't wrap it --
    // widen on demand, and nothing addresses it).
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    set_titlebar(*hb);

    auto* page = Gtk::make_managed<widgets::Box>(
        widgets::unregistered, "shell.about.page", Gtk::Orientation::VERTICAL);
    page->set_spacing(10);
    page->set_margin(24);
    page->set_halign(Gtk::Align::CENTER);

    // Hero logo: symbolic icon from the gresource bundle, recoloured to the
    // theme foreground by GTK. Proof the resource pipeline resolved.
    auto* logo = Gtk::make_managed<widgets::Image>(widgets::unregistered, "shell.about.logo");
    logo->set_from_icon_name("composix-logo-symbolic");
    logo->set_pixel_size(88);
    page->append(*logo);

    auto* name = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.name");
    name->set_markup("<span size='xx-large' weight='bold'>Composix</span>");
    page->append(*name);

    auto* version = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.version",
                                                      "Version " COMPOSIX_VERSION "  \u00b7  build " COMPOSIX_BUILD);
    version->add_css_class("dim-label");
    page->append(*version);

    auto* tagline = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.tagline");
    tagline->set_markup("<i>Books and pages, set with care.</i>");
    tagline->set_margin_top(4);
    page->append(*tagline);

    auto* note = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.note");
    note->set_wrap(true);
    note->set_justify(Gtk::Justification::CENTER);
    note->set_max_width_chars(46);
    note->set_margin_top(4);
    note->set_text("Page layout for the GNOME desktop: threaded text and styles, "
                   "pictures and colour, notes, contents and indexes \u2014 out to PDF/X for the press.");
    page->append(*note);

    // Credits: what it's built on, and whose words it hyphenates by.
    auto* credits = Gtk::make_managed<Gtk::Expander>("Credits");
    credits->set_margin_top(8);
    auto* cbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    cbox->set_margin_top(6);
    const auto line = [&](const char* text) {
        auto* l = Gtk::make_managed<Gtk::Label>();
        l->set_markup(text);
        l->set_xalign(0);
        l->set_wrap(true);
        l->set_max_width_chars(52);
        l->add_css_class("dim-label");
        cbox->append(*l);
    };
    line("<b>Built with</b> gtkmm and GTK, Pango, cairo, HarfBuzz, LittleCMS, zlib, "
         "gdk-pixbuf, librsvg, libtiff, spdlog and nlohmann/json.");
    line("<b>Hyphenation</b> by the TeX patterns of hyph-utf8 \u2014 English (Gerard D. C. Kuiken; "
         "Dominik Wujastyk, Graham Toal), German (Trennmustermannschaft), French (Daniel Flipo, "
         "Bernard Gaulle, Arthur Reutenauer), Spanish (Javier Bezos), Italian (Claudio Beccari), "
         "Dutch (Piet Tutelaers), Portuguese (Pedro J. de Rezende and others). MIT / BSD licences; "
         "the notices travel with the source.");
    line("<b>Logo</b> by Scott.");
    credits->set_child(*cbox);
    page->append(*credits);

    // What it's running on -- with Copy Details, for a bug report.
    char buf[512];
    std::snprintf(buf, sizeof buf, "GTK %u.%u.%u  \u00b7  Pango %s  \u00b7  cairo %s\nHarfBuzz %s  \u00b7  LittleCMS %d.%d%s",
                  gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(), pango_version_string(),
                  cairo_version_string(), hb_version_string(), cmsGetEncodedCMMversion() / 1000,
                  (cmsGetEncodedCMMversion() % 1000) / 10,
#ifdef COMPOSIX_HAVE_TIFF
                  "  \u00b7  libtiff: yes"
#else
                  "  \u00b7  libtiff: no (CMYK TIFFs print through RGB)"
#endif
    );
    const std::string details = std::string("Composix " COMPOSIX_VERSION " (build " COMPOSIX_BUILD ")\n") + buf;
    auto* sys = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.system", buf);
    sys->add_css_class("dim-label");
    sys->add_css_class("caption");
    sys->set_justify(Gtk::Justification::CENTER);
    sys->set_selectable(true);
    sys->set_margin_top(8);
    page->append(*sys);

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_halign(Gtk::Align::CENTER);
    row->set_margin_top(8);
    auto* copy = Gtk::make_managed<widgets::Button>(widgets::unregistered, "shell.about.copy", "Copy Details");
    copy->set_tooltip_text("The version and what it runs on, for a bug report");
    copy->signal_clicked().connect([this, details, copy] {
        get_clipboard()->set_text(details);
        copy->set_label("Copied");
    });
    row->append(*copy);
    page->append(*row);
    signal_show().connect([copy] { copy->set_label("Copy Details"); });   // a singleton: fresh each time it opens

    auto* close_btn = Gtk::make_managed<widgets::Button>(
        widgets::unregistered, "shell.about.close", "Close");
    close_btn->add_css_class("suggested-action");
    close_btn->set_halign(Gtk::Align::CENTER);
    close_btn->signal_clicked().connect([this]() { close(); });
    row->append(*close_btn);

    set_child(*page);
}

AboutWindow::~AboutWindow() {
    registry::remove(this);
}

}  // namespace composix
