#include "views/Thumbs.hpp"
#include "views/Images.hpp"

#include <gdkmm/rgba.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>

namespace composix::views {

namespace {

PagePainter& painter() {
    static PagePainter p;
    return p;
}

struct Rgb { double r, g, b; };
Rgb rgb(std::uint32_t c) {
    return {((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0};
}

void rounded(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h,
             double r) {
    r = std::min({r, w / 2, h / 2});
    cr->begin_new_sub_path();
    cr->arc(x + w - r, y + r, r, -M_PI / 2, 0);
    cr->arc(x + w - r, y + h - r, r, 0, M_PI / 2);
    cr->arc(x + r, y + h - r, r, M_PI / 2, M_PI);
    cr->arc(x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cr->close_path();
}

// A soft drop shadow: a few expanding translucent rounded rects.
void shadow(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h) {
    for (int i = 4; i >= 1; --i) {
        cr->set_source_rgba(0, 0, 0, 0.035);
        rounded(cr, x - i * 0.5, y + i * 0.9 - 0.5, w + i, h + i, 2 + i);
        cr->fill();
    }
}

// The largest w x h with aspect a that fits (W, H) with a margin.
void fit(double W, double H, double aspect, double pad, double& w, double& h) {
    W -= 2 * pad;
    H -= 2 * pad;
    w = W;
    h = w / aspect;
    if (h > H) { h = H; w = h * aspect; }
}

void text(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& wd, const std::string& s,
          const char* font, double x, double y, double width, int max_lines) {
    auto lay = wd.create_pango_layout(s);
    lay->set_font_description(Pango::FontDescription(font));
    lay->set_width(static_cast<int>(width * PANGO_SCALE));
    lay->set_wrap(Pango::WrapMode::WORD);
    lay->set_ellipsize(Pango::EllipsizeMode::END);
    lay->set_height(-max_lines);
    cr->move_to(x, y);
    lay->show_in_cairo_context(cr);
}

void paper(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h) {
    shadow(cr, x, y, w, h);
    cr->set_source_rgb(1, 1, 1);
    cr->rectangle(x, y, w, h);
    cr->fill();
    cr->set_source_rgba(0, 0, 0, 0.12);
    cr->set_line_width(1);
    cr->rectangle(x + 0.5, y + 0.5, w - 1, h - 1);
    cr->stroke();
}

// Grey bars standing in for set type, inside a margin box.
void type_bars(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h,
               double lead, unsigned seed) {
    // Snapped to whole pixels: fractional 1px bars at a 2px pitch alias into
    // grey blocks (a moire) at tile size.
    cr->set_source_rgba(0.2, 0.2, 0.25, 0.22);
    lead = std::max(3.0, std::round(lead));
    const double bar = std::max(1.0, std::round(lead * 0.42));
    const double x0 = std::round(x);
    int n = 0;
    for (double yy = std::round(y); yy + bar < y + h; yy += lead, ++n) {
        const bool last = ((n + seed) % 7) == 6;
        const double ww = std::round(last ? w * 0.55 : w);
        cr->rectangle(x0, yy, ww, bar);
    }
    cr->fill();
}

void page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Thumb& t, double W, double H,
          bool large) {
    double w, h;
    fit(W, H, t.w_in / t.h_in, large ? 24 : 8, w, h);
    const double x = std::round((W - w) / 2), y = std::round((H - h) / 2);
    paper(cr, x, y, w, h);

    const int panels = std::max(1, t.panels);
    const double pw = w / panels;
    const double m = pw * 0.12;
    // s020: the page itself, when there's a painter for it.
    const bool real = !t.page.empty() && painter() && painter()(cr, t.page, x, y, w, h, large);
    for (int p = 0; p < panels && !real; ++p) {
        const double px = x + p * pw;
        if (large) {   // margin guides
            cr->set_source_rgba(0.85, 0.25, 0.65, 0.55);
            cr->set_line_width(1);
            cr->rectangle(px + m + 0.5, y + m * 1.1 + 0.5, pw - 2 * m, h - 2.2 * m);
            cr->stroke();
        }
        if (t.flag && p == 1) {   // the foldout's map panel: an image block
            cr->set_source_rgba(0.56, 0.64, 0.48, 0.55);
            cr->rectangle(px + m, y + m * 1.1, pw - 2 * m, h * 0.62);
            cr->fill();
            type_bars(cr, px + m, y + m * 1.1 + h * 0.66, pw - 2 * m, h * 0.12,
                      std::max(2.0, h / 45), p);
        } else {
            type_bars(cr, px + m, y + m * 1.1, pw - 2 * m, h - 2.2 * m, std::max(2.0, h / 45), p);
        }
    }
    if (t.flag) {   // fold lines
        cr->set_source_rgba(0.2, 0.4, 0.9, 0.7);
        cr->set_line_width(1);
        std::vector<double> dash{4, 3};
        cr->set_dash(dash, 0);
        for (int p = 1; p < panels; ++p) {
            const double fx = std::round(x + p * pw) + 0.5;
            cr->move_to(fx, y);
            cr->line_to(fx, y + h);
        }
        cr->stroke();
        cr->unset_dash();
    }
}

}  // namespace

void set_page_painter(PagePainter p) { painter() = std::move(p); }

void paint_thumb(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& wd, int W, int H,
                 const core::Thumb& t, bool large) {
    using K = core::ThumbKind;
    const auto fg = wd.get_color();
    const auto c = rgb(t.rgb);
    cr->save();

    switch (t.kind) {
    case K::Page:
        page(cr, t, W, H, large);
        break;

    case K::Section: {   // a small stack of pages with the section's colour band
        double w, h;
        fit(W, H, 6.0 / 9.0, H * 0.14, w, h);
        const double x = (W - w) / 2, y = (H - h) / 2;
        for (int i = 2; i >= 0; --i) paper(cr, x + i * 5, y - i * 5, w, h);
        cr->set_source_rgb(c.r, c.g, c.b);
        cr->rectangle(x, y + h * 0.08, w, h * 0.1);
        cr->fill();
        type_bars(cr, x + w * 0.14, y + h * 0.3, w * 0.72, h * 0.58, std::max(2.0, h / 30), 1);
        break;
    }

    case K::Text: {   // a story: its opening words, set
        const double pad = W * 0.1;
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.05);
        rounded(cr, pad * 0.5, pad * 0.5, W - pad, H - pad, 8);
        cr->fill();
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.85);
        text(cr, wd, t.sample, large ? "Serif 18" : "Serif 10", pad, pad, W - 2 * pad, large ? 8 : 6);
        if (t.flag) {   // overset: the red plus
            cr->set_source_rgb(0.85, 0.2, 0.2);
            const double s = large ? 14 : 9, ox = W - pad - s, oy = H - pad - s;
            cr->rectangle(ox, oy, s, s);
            cr->fill();
            cr->set_source_rgb(1, 1, 1);
            cr->set_line_width(1.5);
            cr->move_to(ox + s / 2, oy + 2); cr->line_to(ox + s / 2, oy + s - 2);
            cr->move_to(ox + 2, oy + s / 2); cr->line_to(ox + s - 2, oy + s / 2);
            cr->stroke();
        }
        break;
    }

    case K::Image: {   // the picture itself (s009); else a tinted plate with a mountain-and-sun glyph
        int iw = 0, ih = 0;
        const auto pic = t.file.empty() ? Cairo::RefPtr<Cairo::ImageSurface>{} : images::get(t.file, t.stamp, &iw, &ih);
        double w, h;
        fit(W, H, pic && ih > 0 ? static_cast<double>(iw) / ih : t.w_in / t.h_in, large ? 24 : 10, w, h);
        const double x = std::round((W - w) / 2), y = std::round((H - h) / 2);
        if (pic) {
            shadow(cr, x, y, w, h);
            cr->save();
            rounded(cr, x, y, w, h, 3);
            cr->clip();
            images::paint(cr, pic, x, y, w, h);
            cr->restore();
            break;
        }
        shadow(cr, x, y, w, h);
        auto g = Cairo::LinearGradient::create(x, y, x, y + h);
        g->add_color_stop_rgb(0, std::min(1.0, c.r + 0.25), std::min(1.0, c.g + 0.25), std::min(1.0, c.b + 0.25));
        g->add_color_stop_rgb(1, c.r * 0.8, c.g * 0.8, c.b * 0.8);
        cr->set_source(g);
        rounded(cr, x, y, w, h, 3);
        cr->fill();
        cr->set_source_rgba(1, 1, 1, 0.55);
        cr->move_to(x + w * 0.1, y + h * 0.85);
        cr->line_to(x + w * 0.38, y + h * 0.45);
        cr->line_to(x + w * 0.55, y + h * 0.68);
        cr->line_to(x + w * 0.7, y + h * 0.52);
        cr->line_to(x + w * 0.9, y + h * 0.85);
        cr->close_path();
        cr->fill();
        cr->arc(x + w * 0.74, y + h * 0.28, std::min(w, h) * 0.08, 0, 2 * M_PI);
        cr->fill();
        if (t.flag) {   // missing: dimmed with a broken-link slash
            cr->set_source_rgba(0, 0, 0, 0.35);
            rounded(cr, x, y, w, h, 3);
            cr->fill();
            cr->set_source_rgb(0.9, 0.25, 0.25);
            cr->set_line_width(large ? 4 : 2.5);
            cr->move_to(x + w * 0.2, y + h * 0.8);
            cr->line_to(x + w * 0.8, y + h * 0.2);
            cr->stroke();
        }
        break;
    }

    case K::Swatch: {
        const double r = std::min(W, H) * 0.34;
        cr->arc(W / 2.0, H / 2.0 + 1.5, r + 1, 0, 2 * M_PI);
        cr->set_source_rgba(0, 0, 0, 0.12);
        cr->fill();
        cr->arc(W / 2.0, H / 2.0, r, 0, 2 * M_PI);
        cr->set_source_rgb(c.r, c.g, c.b);
        cr->fill_preserve();
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.2);
        cr->set_line_width(1);
        cr->stroke();
        break;
    }

    case K::Style: {   // a specimen
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
        auto lay = wd.create_pango_layout(t.sample.empty() ? "Aa" : t.sample);
        // The style's own font (s011), by API: a family name may hold spaces / digits.
        Pango::FontDescription fd;
        fd.set_family(t.family.empty() ? "Serif" : t.family);
        fd.set_weight(t.bold ? Pango::Weight::BOLD : Pango::Weight::NORMAL);
        fd.set_style(t.italic ? Pango::Style::ITALIC : Pango::Style::NORMAL);
        fd.set_size((large ? 96 : 44) * PANGO_SCALE);
        lay->set_font_description(fd);
        int lw, lh;
        lay->get_pixel_size(lw, lh);
        cr->move_to((W - lw) / 2.0, (H - lh) / 2.0);
        lay->show_in_cairo_context(cr);
        break;
    }

    case K::Generated: {   // a page of entries with dot leaders
        double w, h;
        fit(W, H, 6.0 / 9.0, large ? 24 : 10, w, h);
        const double x = (W - w) / 2, y = (H - h) / 2;
        paper(cr, x, y, w, h);
        const double m = w * 0.14, lead = std::max(4.0, h / 14);
        cr->set_source_rgba(0.2, 0.2, 0.25, 0.4);
        cr->rectangle(x + m, y + m, w * 0.4, lead * 0.5);
        cr->fill();
        cr->set_source_rgba(0.2, 0.2, 0.25, 0.25);
        for (double yy = y + m + lead * 1.6; yy < y + h - m; yy += lead) {
            cr->rectangle(x + m, yy, w * 0.35, lead * 0.35);
            for (double dx = x + m + w * 0.4; dx < x + w - m - w * 0.1; dx += 3)
                cr->rectangle(dx, yy + lead * 0.25, 1, 1);
            cr->rectangle(x + w - m - w * 0.06, yy, w * 0.06, lead * 0.35);
        }
        cr->fill();
        if (t.flag) {   // stale: an amber dot
            cr->set_source_rgb(0.95, 0.65, 0.1);
            cr->arc(x + w - 6, y + 6, large ? 7 : 4, 0, 2 * M_PI);
            cr->fill();
        }
        break;
    }
    }
    cr->restore();
}

}  // namespace composix::views
