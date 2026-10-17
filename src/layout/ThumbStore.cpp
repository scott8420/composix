#include "layout/ThumbStore.hpp"

#include <algorithm>

namespace composix::layout {

Surface adopt_surface(cairo_surface_t* s) {
    if (!s) return {};
    return Surface(s, [](cairo_surface_t* p) { cairo_surface_destroy(p); });
}

Surface render_thumb(const core::Doc& d, FlowCache& flows, const std::string& id, int pw, int ph, bool guides,
                     const ImageSource& images) {
    double pt_w = 0, pt_h = 0;
    if (const auto* p = d.as<core::Page>(id)) { pt_w = p->w; pt_h = p->h; }
    else if (const auto* m = d.as<core::Master>(id)) { pt_w = m->w; pt_h = m->h; }
    if (pt_w <= 0 || pt_h <= 0 || pw < 1 || ph < 1) return {};
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, pw / pt_w, ph / pt_h);
    draw_page(cr, d, id, flows, images, nullptr, {.placeholders = true});
    // The margins (and a master's columns) as the canvas shows them: on the
    // preview, and on every master -- a master's layout IS its guides.
    if (guides || d.as<core::Master>(id)) {
        const core::Rect r = core::margin_rect(d, id);
        if (r.w > 0) {
            cairo_set_line_width(cr, pt_w / pw);   // one device pixel
            cairo_set_source_rgba(cr, 0.85, 0.25, 0.65, 0.55);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_stroke(cr);
            const auto cols = core::page_columns(d, id);
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
    cairo_surface_flush(surf);
    return adopt_surface(surf);
}

namespace {
std::size_t size_of(const Surface& s) {
    if (!s) return 0;
    return static_cast<std::size_t>(cairo_image_surface_get_stride(s.get())) *
           static_cast<std::size_t>(cairo_image_surface_get_height(s.get()));
}
}  // namespace

ThumbStore::ThumbStore(std::size_t cap_bytes) : m_cap(cap_bytes) {}

ThumbStore::~ThumbStore() {
    {
        std::lock_guard lk(m_mx);
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void ThumbStore::set_notify(std::function<void()> f) {
    std::lock_guard lk(m_mx);
    m_notify = std::move(f);
}

int ThumbStore::renders() const {
    std::lock_guard lk(m_mx);
    return m_sync_renders + m_worker_renders;
}

std::size_t ThumbStore::pending() const {
    std::lock_guard lk(m_mx);
    return m_queue.size() + (m_busy ? 1 : 0) + m_done.size();
}

const PageKey& ThumbStore::key_of(const core::Doc& d, FlowCache& flows, const std::string& id) {
    if (d.revision() != m_keys_rev) {
        m_keys.clear();
        m_keys_rev = d.revision();
    }
    auto it = m_keys.find(id);
    if (it == m_keys.end()) it = m_keys.emplace(id, page_key(d, flows, id)).first;
    return it->second;
}

void ThumbStore::store(Entry& e, Surface s, const PageKey& key, const std::string& variant, int pw, int ph) {
    m_bytes -= size_of(e.surf);
    e.surf = std::move(s);
    e.key = key;
    e.variant = variant;
    e.pw = pw;
    e.ph = ph;
    m_bytes += size_of(e.surf);
}

ThumbStore::Look ThumbStore::look(const core::Doc& d, FlowCache& flows, const ImageSource& images,
                                  const std::string& slot, const std::string& id, int pw, int ph, bool guides) {
    const Render r = [id, pw, ph, guides](const core::Doc& doc, FlowCache& fl, const ImageSource& im) {
        return render_thumb(doc, fl, id, pw, ph, guides, im);
    };
    // s107: how CMYK is shown on screen is part of the picture.
    return look(d, flows, images, slot, id, "s" + std::to_string(core::doc::screen_gen()), pw, ph, r,
                m_threaded ? Miss::Ask : Miss::Now);
}

ThumbStore::Look ThumbStore::look(const core::Doc& d, FlowCache& flows, const ImageSource& images,
                                  const std::string& slot, const std::string& id, const std::string& variant, int pw,
                                  int ph, const Render& render, Miss miss) {
    // Asks made for an earlier revision are dropped, not snapshotted with a
    // later one (the picture would not be what its key says).
    if (d.revision() != m_asks_rev) {
        for (const auto& a : m_asks)
            if (auto it = m_entries.find(a.slot); it != m_entries.end()) it->second.asked = false;
        m_asks.clear();
        m_asks_rev = d.revision();
    }
    const PageKey& key = key_of(d, flows, id);
    Entry& e = m_entries[slot];
    e.used = ++m_tick;
    if (e.surf && e.pw == pw && e.ph == ph && e.variant == variant && e.key == key) return {e.surf, true};
    if (miss == Miss::Now || (miss == Miss::NowIfNone && !e.surf)) {
        store(e, render(d, flows, images), key, variant, pw, ph);
        e.drawn_at = m_tick;
        e.asked = false;
        std::erase_if(m_asks, [&](const Job& j) { return j.slot == slot; });
        {
            std::lock_guard lk(m_mx);
            ++m_sync_renders;
        }
        Surface s = e.surf;   // trim() may let the entry go
        trim();
        return {s, s != nullptr};
    }
    if (!(e.asked && e.want_pw == pw && e.want_ph == ph && e.want_variant == variant && e.want_key == key)) {
        e.asked = true;
        e.want_key = key;
        e.want_variant = variant;
        e.want_pw = pw;
        e.want_ph = ph;
        std::erase_if(m_asks, [&](const Job& j) { return j.slot == slot; });
        m_asks.push_back(Job{slot, id, key, variant, pw, ph, render, nullptr, m_tick});
    }
    return {e.surf, false};
}

bool ThumbStore::flush(const core::Doc& d, const FlowCache& flows, const ImageSource& images) {
    if (m_asks.empty()) return false;
    if (d.revision() != m_asks_rev) {   // the document moved since: ask again at the next draw
        for (const auto& a : m_asks)
            if (auto it = m_entries.find(a.slot); it != m_entries.end()) it->second.asked = false;
        m_asks.clear();
        return false;
    }
    if (!m_snap || m_snap_rev != d.revision() || m_snap->gen != m_gen) {
        auto s = std::make_shared<Snap>();
        {
            core::Doc::Loading load(s->doc);
            for (const auto& [id, r] : d.records()) s->doc.put(id, r);
        }
        // The shell's layout, if it is this revision's, is the copy's too:
        // the worker draws without flowing again.
        if (flows.fresh_for(d)) {
            s->flows = flows;
            s->flows.rebase(s->doc);
        }
        s->images = images;
        s->gen = m_gen;
        m_snap = std::move(s);
        m_snap_rev = d.revision();
        ++m_snapshots;
    }
    {
        std::lock_guard lk(m_mx);
        for (auto& a : m_asks) {
            a.snap = m_snap;
            std::erase_if(m_queue, [&](const Job& q) { return q.slot == a.slot; });   // the newest ask wins
            m_queue.push_back(std::move(a));
        }
        if (!m_worker.joinable()) m_worker = std::thread([this] { run(); });
    }
    m_asks.clear();
    m_cv.notify_all();
    return true;
}

void ThumbStore::run() {
    std::unique_lock lk(m_mx);
    for (;;) {
        m_cv.wait(lk, [this] { return m_stop || (!m_held && !m_queue.empty()); });
        if (m_stop) return;
        Job j = std::move(m_queue.front());
        m_queue.pop_front();
        m_busy = true;
        lk.unlock();
        Surface s = j.render ? j.render(j.snap->doc, j.snap->flows, j.snap->images) : Surface{};
        const std::uint64_t gen = j.snap->gen;
        j.snap.reset();   // the last job of a revision lets its snapshot go here
        lk.lock();
        ++m_worker_renders;
        j.render = {};   // what it captured goes on this thread
        m_done.push_back(Done{std::move(j.slot), std::move(j.id), std::move(j.key), std::move(j.variant), j.pw, j.ph,
                              std::move(s), gen, j.asked_at});
        m_busy = false;
        auto notify = m_notify;
        const bool idle = m_queue.empty();
        lk.unlock();
        if (idle) m_idle_cv.notify_all();
        if (notify) notify();
        lk.lock();
    }
}

std::set<std::string> ThumbStore::collect() {
    std::vector<Done> done;
    {
        std::lock_guard lk(m_mx);
        done.swap(m_done);
    }
    std::set<std::string> ids;
    for (auto& r : done) {
        if (r.gen != m_gen || !r.surf) continue;   // another document's
        Entry& e = m_entries[r.slot];
        if (e.surf && r.asked_at < e.drawn_at) continue;   // something newer is already there
        store(e, std::move(r.surf), r.key, r.variant, r.pw, r.ph);
        e.drawn_at = r.asked_at;
        if (e.asked && e.want_key == r.key && e.want_variant == r.variant && e.want_pw == r.pw && e.want_ph == r.ph)
            e.asked = false;
        ids.insert(r.id);
    }
    if (!done.empty()) trim();
    return ids;
}

void ThumbStore::trim() {
    if (m_bytes <= m_cap) return;
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto& [slot, e] : m_entries)
        if (e.surf) order.emplace_back(e.used, slot);
    std::sort(order.begin(), order.end());
    for (const auto& [used, slot] : order) {
        if (m_bytes <= m_cap) break;
        Entry& e = m_entries[slot];
        store(e, nullptr, e.key, {}, 0, 0);
        if (!e.asked) m_entries.erase(slot);
    }
}

void ThumbStore::forget() {
    {
        std::lock_guard lk(m_mx);
        m_queue.clear();
        m_done.clear();
    }
    ++m_gen;
    m_entries.clear();
    m_keys.clear();
    m_keys_rev = ~0ull;
    m_asks.clear();
    m_asks_rev = ~0ull;
    m_snap.reset();
    m_snap_rev = ~0ull;
    m_bytes = 0;
}

void ThumbStore::hold(bool on) {
    {
        std::lock_guard lk(m_mx);
        m_held = on;
    }
    m_cv.notify_all();
}

void ThumbStore::wait_idle() {
    std::unique_lock lk(m_mx);
    m_idle_cv.wait(lk, [this] { return m_stop || (!m_busy && (m_queue.empty() || m_held)); });
}

}  // namespace composix::layout
