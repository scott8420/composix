#include "views/PageThumbs.hpp"
#include "views/Images.hpp"

#include <algorithm>
#include <cmath>

namespace composix::views {

PageThumbs::PageThumbs(const core::Doc& d, std::function<std::string(const core::Asset&)> resolve)
    : m_doc(d), m_resolve(std::move(resolve)) {}

void PageThumbs::forget() {
    m_cache.clear();
    m_keys.clear();
    m_keys_rev = ~0ull;
    m_flows = {};
}

const layout::PageKey& PageThumbs::key_of(const std::string& id) {
    if (m_doc.revision() != m_keys_rev) {
        m_keys.clear();
        m_keys_rev = m_doc.revision();
    }
    auto it = m_keys.find(id);
    if (it == m_keys.end()) it = m_keys.emplace(id, layout::page_key(m_doc, m_flows, id)).first;
    return it->second;
}

Cairo::RefPtr<Cairo::ImageSurface> PageThumbs::render(const std::string& id, double pt_w, double pt_h, int pw, int ph,
                                                      bool large) {
    auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24, pw, ph);
    cairo_t* cr = cairo_create(surf->cobj());
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, pw / pt_w, ph / pt_h);
    const layout::ImageSource pictures = [this](const core::Asset& a, double, double) -> cairo_surface_t* {
        const auto pic = images::get(m_resolve(a), a.stamp);
        return pic ? cairo_surface_reference(pic->cobj()) : nullptr;
    };
    layout::draw_page(cr, m_doc, id, m_flows, pictures, nullptr, {.placeholders = true});
    // The margins (and a master's columns) as the canvas shows them: on the
    // preview, and on every master -- a master's layout IS its guides.
    if (large || m_doc.as<core::Master>(id)) {
        const core::Rect r = core::margin_rect(m_doc, id);
        if (r.w > 0) {
            cairo_set_line_width(cr, pt_w / pw);   // one device pixel
            cairo_set_source_rgba(cr, 0.85, 0.25, 0.65, 0.55);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_stroke(cr);
            const auto cols = core::page_columns(m_doc, id);
            cairo_set_source_rgba(cr, 0.45, 0.35, 0.9, 0.5);
            for (std::size_t c = 0; c + 1 < cols.size(); ++c) {
                for (const double x : {cols[c].x1, cols[c + 1].x0}) {
                    cairo_move_to(cr, x, r.y);
                    cairo_line_to(cr, x, r.y + r.h);
                }
            }
            cairo_stroke(cr);
        }
    }
    cairo_destroy(cr);
    ++m_renders;
    return surf;
}

bool PageThumbs::paint(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id, double x, double y, double w,
                       double h, bool large) {
    double pt_w = 0, pt_h = 0;
    if (const auto* p = m_doc.as<core::Page>(id)) { pt_w = p->w; pt_h = p->h; }
    else if (const auto* m = m_doc.as<core::Master>(id)) { pt_w = m->w; pt_h = m->h; }
    else return false;
    if (w < 1 || h < 1) return false;
    // At the screen's own pixels (2x on a HiDPI display), so it stays sharp.
    double sx = 1, sy = 1;
    cairo_surface_get_device_scale(cairo_get_target(cr->cobj()), &sx, &sy);
    const int pw = std::max(1, static_cast<int>(std::lround(w * sx)));
    const int ph = std::max(1, static_cast<int>(std::lround(h * sy)));
    const auto& key = key_of(id);
    auto& e = m_cache[large ? id + "|L" : id];
    if (!e.surf || e.pw != pw || e.ph != ph || !(e.key == key)) {
        e.surf = render(id, pt_w, pt_h, pw, ph, large);
        e.key = key;
        e.pw = pw;
        e.ph = ph;
    }
    cr->save();
    cr->rectangle(x, y, w, h);
    cr->clip();
    cr->translate(x, y);
    cr->scale(w / pw, h / ph);
    cr->set_source(e.surf, 0, 0);
    cr->paint();
    cr->restore();
    return true;
}

}  // namespace composix::views
