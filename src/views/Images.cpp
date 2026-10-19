#include "views/Images.hpp"
#include "Log.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdkmm/general.h>
#include <gdkmm/pixbuf.h>
#include <cairomm/context.h>

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <map>
#include <mutex>
#include <glibmm/main.h>
#include <chrono>
#include <thread>
#include <condition_variable>

namespace composix::views::images {

namespace {
struct Entry {
    std::int64_t              stamp = -1;
    Cairo::RefPtr<Cairo::ImageSurface> pix;
    int                       w = 0, h = 0;
    bool                      pending = false;   // s174: being decoded (by get or a peek's thread)
    int                       arrivals = 0;      // s174: decodes finished on their own thread
};
// s174: the cache's lock covers the map only -- a decode runs outside it, so
// a big file on its thread holds nobody else up. Never freed: a decode
// thread may outlive main().
struct Cache {
    std::mutex                    mx;
    std::condition_variable       done;
    std::map<std::string, Entry>  m;
    std::function<void()>         hook;
};
Cache& cache() {
    static auto* c = new Cache;
    return *c;
}
bool is_svg(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".svg" || ext == ".svgz";
}
}  // namespace

std::optional<core::ImageFile> probe(const std::string& path) {
    int w = 0, h = 0;
    GdkPixbufFormat* fmt = gdk_pixbuf_get_file_info(path.c_str(), &w, &h);
    if (!fmt || w <= 0 || h <= 0) return std::nullopt;
    core::ImageFile f;
    f.path = std::filesystem::absolute(path).lexically_normal().string();
    f.name = std::filesystem::path(path).filename().string();
    f.px_w = w;
    f.px_h = h;
    gchar* name = gdk_pixbuf_format_get_name(fmt);
    f.vector = is_svg(path) || (name && std::string(name) == "svg");
    g_free(name);
    f.color = "RGB";   // gdk-pixbuf reads everything as RGB(A); CMYK detection is output's job
    f.stamp = core::file_stamp(f.path);
    return f;
}

// Decode once into a surface: painting a pixbuf converts it on every draw.
Cairo::RefPtr<Cairo::ImageSurface> to_surface(const Glib::RefPtr<Gdk::Pixbuf>& pb) {
    if (!pb) return {};
    auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, pb->get_width(), pb->get_height());
    auto cr = Cairo::Context::create(surf);
    Gdk::Cairo::set_source_pixbuf(cr, pb, 0, 0);
    cr->paint();
    return surf;
}

namespace {
// The file, decoded for the screen (no lock held): its size, the surface.
Entry decode(const std::string& path, std::int64_t stamp) {
    Entry e;
    e.stamp = stamp;
    if (stamp == 0) return e;
    try {
        int fw = 0, fh = 0;
        if (gdk_pixbuf_get_file_info(path.c_str(), &fw, &fh) && fw > 0 && fh > 0) {
            e.w = fw;
            e.h = fh;
            // A vector renders at the preview size (crisp when zoomed);
            // a big raster shrinks on load; a small one loads as is.
            if (is_svg(path) || std::max(fw, fh) > kMaxPx) {
                const double sc = static_cast<double>(kMaxPx) / std::max(fw, fh);
                e.pix = to_surface(Gdk::Pixbuf::create_from_file(path, std::max(1, static_cast<int>(fw * sc)),
                                                                 std::max(1, static_cast<int>(fh * sc)), true));
            } else {
                e.pix = to_surface(Gdk::Pixbuf::create_from_file(path));
            }
        }
    } catch (const Glib::Error&) {   // a format it can't read: the stand-in
        e.pix.reset();
    }
    return e;
}

// Store a finished decode; wake whoever waits on it.
void store(const std::string& path, Entry e, bool threaded = false) {
    auto& c = cache();
    {
        std::lock_guard lk(c.mx);
        auto& slot = c.m[path];
        const int n = slot.arrivals + (threaded ? 1 : 0);
        if (slot.stamp == e.stamp) { e.pending = false; slot = std::move(e); }
        else slot.pending = false;   // the file changed meanwhile: the next ask decodes again
        slot.arrivals = n;
    }
    c.done.notify_all();
}
}  // namespace

Cairo::RefPtr<Cairo::ImageSurface> get(const std::string& path, std::int64_t stamp, int* w, int* h) {
    if (path.empty()) return {};
    auto& c = cache();
    {
        std::unique_lock lk(c.mx);
        // s174: one decoding it already (a peek's thread): wait for that one.
        c.done.wait(lk, [&] { const auto it = c.m.find(path); return it == c.m.end() || !it->second.pending; });
        auto& e = c.m[path];
        if (e.stamp == stamp) {
            if (w) *w = e.w;
            if (h) *h = e.h;
            return e.pix;
        }
        const int n = e.arrivals;
        e = Entry{};
        e.stamp = stamp;
        e.pending = true;
        e.arrivals = n;
    }
    Entry made = decode(path, stamp);
    if (w) *w = made.w;
    if (h) *h = made.h;
    auto pix = made.pix;
    store(path, std::move(made));
    return pix;
}

Cairo::RefPtr<Cairo::ImageSurface> peek(const std::string& path, std::int64_t stamp, int* w, int* h, bool* pending) {
    if (pending) *pending = false;
    if (path.empty()) return {};
    auto& c = cache();
    {
        std::lock_guard lk(c.mx);
        auto& e = c.m[path];
        if (e.stamp == stamp && !e.pending) {
            if (w) *w = e.w;
            if (h) *h = e.h;
            return e.pix;
        }
        if (e.stamp == stamp && e.pending) {   // on its way
            if (pending) *pending = true;
            return {};
        }
    }
    // Small: here and now. Big: on a thread of its own.
    int fw = 0, fh = 0;
    const bool big = stamp != 0 && gdk_pixbuf_get_file_info(path.c_str(), &fw, &fh) &&
                     static_cast<long>(fw) * static_cast<long>(fh) > kInlinePx && !is_svg(path);
    if (!big) return get(path, stamp, w, h);
    {
        std::lock_guard lk(c.mx);
        auto& e = c.m[path];
        if (e.pending) { if (pending) *pending = true; return {}; }   // someone got there first
        const int n = e.arrivals;
        e = Entry{};
        e.stamp = stamp;
        e.pending = true;
        e.arrivals = n;
    }
    if (pending) *pending = true;
    std::thread([path, stamp] {
        auto t0 = std::chrono::steady_clock::now();
        store(path, decode(path, stamp), true);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Io)) lg->info("images: decoded '{}' on its own thread, {} ms", path, ms);
        Glib::MainContext::get_default()->invoke([] {
            std::function<void()> f;
            {
                std::lock_guard lk(cache().mx);
                f = cache().hook;
            }
            if (f) f();
            return false;
        });
    }).detach();
    return {};
}

int arrivals(const std::string& path) {
    auto& c = cache();
    std::lock_guard lk(c.mx);
    const auto it = c.m.find(path);
    return it == c.m.end() ? 0 : it->second.arrivals;
}

void set_ready_hook(std::function<void()> f) {
    std::lock_guard lk(cache().mx);
    cache().hook = std::move(f);
}

std::vector<std::uint32_t> pixels(const std::string& path, std::int64_t stamp, int& w, int& h) {
    w = h = 0;
    auto pic = get(path, stamp);
    if (!pic) return {};
    pic->flush();
    w = pic->get_width();
    h = pic->get_height();
    const int stride = pic->get_stride();
    const auto fmt = pic->get_format();
    if (w <= 0 || h <= 0 || (fmt != Cairo::Surface::Format::ARGB32 && fmt != Cairo::Surface::Format::RGB24)) { w = h = 0; return {}; }
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const unsigned char* data = pic->get_data();
    for (int y = 0; y < h; ++y) {
        std::memcpy(&px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)], data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    static_cast<std::size_t>(w) * 4);
        if (fmt == Cairo::Surface::Format::RGB24)   // its top byte is unused: opaque
            for (int x = 0; x < w; ++x) px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] |= 0xff000000u;
    }
    return px;
}

std::vector<float> trace(const std::string& path, std::int64_t stamp) {
    int w = 0, h = 0;
    const auto px = pixels(path, stamp, w, h);
    if (px.empty()) return {};
    auto s = core::trace_shape(px.data(), w, h, w);
    if (auto lg = log::get(log::Area::Io)) lg->info("trace: '{}' {}x{} -> {}", path, w, h, s.empty() ? "nothing to trace" : std::to_string((s.size() - 1) / 2) + " bands");
    return s;
}

void paint(const Cairo::RefPtr<Cairo::Context>& cr, const Cairo::RefPtr<Cairo::ImageSurface>& pic,
           double x, double y, double w, double h, double alpha) {
    if (!pic || w <= 0 || h <= 0) return;
    cr->save();
    cr->translate(x, y);
    cr->scale(w / pic->get_width(), h / pic->get_height());
    cr->set_source(pic, 0, 0);
    cr->paint_with_alpha(alpha);
    cr->restore();
}

cairo_surface_t* print_surface(const std::string& path, double want_w, double want_h) {
    if (path.empty()) return nullptr;
    try {
        Glib::RefPtr<Gdk::Pixbuf> pb;
        if (is_svg(path)) {
            const int w = std::max(1, static_cast<int>(want_w / 72.0 * 300)), h = std::max(1, static_cast<int>(want_h / 72.0 * 300));
            pb = Gdk::Pixbuf::create_from_file(path, w, h, true);
        } else {
            pb = Gdk::Pixbuf::create_from_file(path);
        }
        auto surf = to_surface(pb);
        if (!surf) return nullptr;
        cairo_surface_t* raw = cairo_surface_reference(surf->cobj());
        auto ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".jpg" || ext == ".jpeg") {   // the original JPEG goes into the PDF as is
            gchar* bytes = nullptr;
            gsize len = 0;
            if (g_file_get_contents(path.c_str(), &bytes, &len, nullptr))
                cairo_surface_set_mime_data(raw, CAIRO_MIME_TYPE_JPEG, reinterpret_cast<unsigned char*>(bytes), len,
                                            g_free, bytes);
        }
        return raw;
    } catch (const Glib::Error&) {
        return nullptr;
    }
}

const std::vector<std::string>& patterns() {
    static const std::vector<std::string> p{"*.png", "*.jpg", "*.jpeg", "*.tif", "*.tiff", "*.gif",
                                            "*.bmp", "*.svg", "*.webp"};
    return p;
}

}  // namespace composix::views::images
