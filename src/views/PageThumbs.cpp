#include "views/PageThumbs.hpp"
#include "views/Images.hpp"
#include "views/Thumbs.hpp"
#include "Log.hpp"

#include <glibmm/main.h>

#include <algorithm>
#include <cmath>

namespace composix::views {

PageThumbs::PageThumbs(const core::Doc& d, std::function<std::string()> project_dir)
    : m_doc(d), m_dir(std::move(project_dir)) {
    m_disp.connect([this] { on_worker(); });
    m_store.set_notify([this] {   // the worker: at most one wake-up in flight
        if (!m_signalled.exchange(true)) m_disp.emit();
    });
}

PageThumbs::~PageThumbs() {
    m_flush_conn.disconnect();
    m_collect_conn.disconnect();
    m_store.set_notify({});
}

layout::ImageSource PageThumbs::images() const {
    const std::string dir = m_dir ? m_dir() : std::string{};
    return [dir](const core::Asset& a, double, double) -> cairo_surface_t* {
        const auto pic = images::get(core::resolve_asset(a, dir), a.stamp);
        return pic ? cairo_surface_reference(pic->cobj()) : nullptr;
    };
}

void PageThumbs::on_worker() {
    m_signalled = false;
    // Gathered a moment, so a run of pages coming back is one redraw.
    if (m_collect_conn.connected()) return;
    m_collect_conn = Glib::signal_timeout().connect([this] {
        const auto ids = m_store.collect();
        m_run_n += static_cast<int>(ids.size());
        if (!ids.empty()) thumbs_ready().emit(ids);
        if (m_running && m_store.pending() == 0) {
            m_running = false;
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_run_t0).count();
            if (m_run_n >= 8 || ms >= 100)
                if (auto lg = log::get(log::Area::View)) lg->info("thumbs: {} drawn on the worker in {:.0f} ms", m_run_n, ms);
        }
        return false;
    }, 50);
}

void PageThumbs::forget() {
    m_store.forget();
    *m_flowp = {};
}

bool PageThumbs::paint(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id, double x, double y, double w,
                       double h, bool large) {
    if (id.rfind("mockup:", 0) == 0) {   // s149: a master's mock-up -- drawn here and now (a master is few frames)
        const auto mid = id.substr(7);
        if (!m_doc.as<core::Master>(mid) || w < 2 || h < 2) return false;
        layout::draw_master_mockup(cr->cobj(), m_doc, mid, *m_flowp, images(), x, y, w, h);
        return true;
    }
    if (!m_doc.as<core::Page>(id) && !m_doc.as<core::Master>(id)) return false;
    if (w < 1 || h < 1) return false;
    // At the screen's own pixels (2x on a HiDPI display), so it stays sharp.
    double sx = 1, sy = 1;
    cairo_surface_get_device_scale(cairo_get_target(cr->cobj()), &sx, &sy);
    const int pw = std::max(1, static_cast<int>(std::lround(w * sx)));
    const int ph = std::max(1, static_cast<int>(std::lround(h * sy)));
    const auto look = m_store.look(m_doc, *m_flowp, images(), large ? id + "|L" : id, id, pw, ph, large);
    if (m_store.asking() && !m_flush_conn.connected()) {
        // After this draw pass has asked for all it shows: one snapshot.
        m_flush_conn = Glib::signal_idle().connect([this] {
            const auto t0 = std::chrono::steady_clock::now();
            const int before = m_store.snapshots();
            if (m_store.flush(m_doc, *m_flowp, images()) && !m_running) {
                m_running = true;
                m_run_t0 = t0;
                m_run_n = 0;
            }
            if (m_store.snapshots() != before) {
                const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (ms >= 20)
                    if (auto lg = log::get(log::Area::View)) lg->info("thumbs snapshot {:.0f} ms", ms);
            }
            return false;
        });
    }
    if (!look.surf) return false;
    auto surf = Cairo::make_refptr_for_instance<Cairo::Surface>(
        new Cairo::Surface(cairo_surface_reference(look.surf.get()), true));
    const int iw = cairo_image_surface_get_width(look.surf.get());
    const int ih = cairo_image_surface_get_height(look.surf.get());
    cr->save();
    cr->rectangle(x, y, w, h);
    cr->clip();
    cr->translate(x, y);
    cr->scale(w / iw, h / ih);
    cr->set_source(surf, 0, 0);
    cr->paint();
    cr->restore();
    return true;
}

}  // namespace composix::views
