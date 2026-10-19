#include "views/Images.hpp"
#include "Log.hpp"
#include "layout/Psd.hpp"
#include "layout/Export.hpp"   // s187: kCmykMime

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdkmm/general.h>
#include <gdkmm/pixbuf.h>
#include <cairomm/context.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#ifdef COMPOSIX_HAVE_TIFF
#include <tiffio.h>
#endif
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
// s185 (G5b): a Photoshop file -- by its first bytes (gdk-pixbuf reads none).
std::string head_of(const std::string& path, std::size_t n) {
    std::ifstream in(path, std::ios::binary);
    std::string b(n, '\0');
    in.read(b.data(), static_cast<std::streamsize>(n));
    b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return b;
}
bool is_psd(const std::string& path) { return head_of(path, 4) == "8BPS"; }
std::string whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
// The file's own size in pixels, whatever reads it.
bool file_size(const std::string& path, int& w, int& h) {
    if (gdk_pixbuf_get_file_info(path.c_str(), &w, &h) && w > 0 && h > 0) return true;
    const auto i = layout::psd_info(head_of(path, 26));
    w = i.w;
    h = i.h;
    return i.ok;
}
// A PSD's merged picture as a cairo surface (`max_side` 0: full size).
Cairo::RefPtr<Cairo::ImageSurface> psd_surface(const std::string& path, int max_side) {
    int w = 0, h = 0;
    std::string why;
    const auto px = layout::psd_decode(whole_file(path), w, h, max_side, &why);
    if (px.empty()) {
        if (auto lg = log::get(log::Area::Io)) lg->warn("images: '{}' -- {}", path, why);
        return {};
    }
    auto s = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, w, h);
    s->flush();
    unsigned char* d = s->get_data();
    for (int y = 0; y < h; ++y)
        std::memcpy(d + static_cast<std::size_t>(y) * static_cast<std::size_t>(s->get_stride()),
                    px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w), static_cast<std::size_t>(w) * 4);
    s->mark_dirty();
    return s;
}
// s187: a picture's CMYK separations put on its print surface (layout::kCmykMime).
void attach_ink(cairo_surface_t* s, std::vector<std::uint8_t> ink) {
    auto* copy = new std::vector<std::uint8_t>(std::move(ink));
    cairo_surface_set_mime_data(s, layout::kCmykMime, copy->data(), copy->size(),
                                [](void* p) { delete static_cast<std::vector<std::uint8_t>*>(p); }, copy);
}
#ifdef COMPOSIX_HAVE_TIFF
// A CMYK TIFF's separations (8 or 16 bits, chunky), the file's own size; empty otherwise.
std::vector<std::uint8_t> tiff_ink(const std::string& path, int want_w, int want_h, int* got_w = nullptr, int* got_h = nullptr) {
    std::vector<std::uint8_t> out;
    TIFFSetWarningHandler(nullptr);
    TIFF* t = TIFFOpen(path.c_str(), "r");
    if (!t) return out;
    uint32_t w = 0, h = 0;
    uint16_t spp = 0, bps = 0, photo = 0, planar = PLANARCONFIG_CONTIG, inkset = INKSET_CMYK;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(t, TIFFTAG_INKSET, &inkset);
    if (photo == PHOTOMETRIC_SEPARATED && inkset == INKSET_CMYK && spp >= 4 && (bps == 8 || bps == 16) &&
        planar == PLANARCONFIG_CONTIG && (want_w <= 0 || (static_cast<int>(w) == want_w && static_cast<int>(h) == want_h))) {
        if (got_w) *got_w = static_cast<int>(w);
        if (got_h) *got_h = static_cast<int>(h);
        std::vector<std::uint8_t> line(static_cast<std::size_t>(TIFFScanlineSize(t)));
        out.resize(static_cast<std::size_t>(w) * h * 4);
        const std::size_t step = static_cast<std::size_t>(spp) * (bps / 8);
        for (uint32_t y = 0; y < h; ++y) {
            if (TIFFReadScanline(t, line.data(), y, 0) < 0) { out.clear(); break; }
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    const std::size_t at = x * step + static_cast<std::size_t>(c) * (bps / 8);
                    // 16-bit: the high byte (TIFF's own byte order is undone by libtiff)
                    out[(static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(c)] =
                        bps == 8 ? line[at] : static_cast<std::uint8_t>(reinterpret_cast<const uint16_t*>(line.data() + at)[0] >> 8);
                }
        }
    }
    TIFFClose(t);
    return out;
}
// s187: a CMYK TIFF gdk-pixbuf can't decode (16-bit): drawn from its own
// separations (plain ink arithmetic), every k-th pixel for the screen;
// the ink rides with it for print. Null when it isn't one.
cairo_surface_t* tiff_cmyk_surface(const std::string& path, int max_side, bool keep_ink) {
    int W = 0, H = 0;
    auto ink = tiff_ink(path, 0, 0, &W, &H);
    if (ink.empty()) return nullptr;
    const int k = max_side > 0 ? std::max(1, (std::max(W, H) + max_side - 1) / max_side) : 1;
    const int w = (W + k - 1) / k, h = (H + k - 1) / k;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_surface_flush(s);
    unsigned char* d = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &ink[(static_cast<std::size_t>(y * k) * W + static_cast<std::size_t>(x * k)) * 4];
            const unsigned kk = 255u - p[3];
            const unsigned r = (255u - p[0]) * kk / 255, g = (255u - p[1]) * kk / 255, b = (255u - p[2]) * kk / 255;
            reinterpret_cast<std::uint32_t*>(d + static_cast<std::ptrdiff_t>(y) * stride)[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    cairo_surface_mark_dirty(s);
    if (keep_ink && k == 1) attach_ink(s, std::move(ink));
    return s;
}
#endif
bool is_svg(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".svg" || ext == ".svgz";
}
}  // namespace

std::optional<core::ImageFile> probe(const std::string& path) {
    int w = 0, h = 0;
    GdkPixbufFormat* fmt = gdk_pixbuf_get_file_info(path.c_str(), &w, &h);
    if (!fmt || w <= 0 || h <= 0) {   // s185: a PSD -- its header, if its picture can be read
        const auto i = layout::psd_info(head_of(path, 26));
        if (!i.ok || i.mode == 7 || (i.depth != 1 && i.depth != 8 && i.depth != 16)) return std::nullopt;
        w = i.w;
        h = i.h;
        fmt = nullptr;
    }
    core::ImageFile f;
    f.path = std::filesystem::absolute(path).lexically_normal().string();
    f.name = std::filesystem::path(path).filename().string();
    f.px_w = w;
    f.px_h = h;
    gchar* name = fmt ? gdk_pixbuf_format_get_name(fmt) : nullptr;
    f.vector = is_svg(path) || (name && std::string(name) == "svg");
    g_free(name);
    // s184 (G5): its colour space from the file's own header (gdk-pixbuf
    // reads everything as RGB); "RGB" when the header says nothing we read.
    {
        std::ifstream in(f.path, std::ios::binary);
        const auto c = core::picture_color([&in](std::uint64_t off, std::size_t n) {
            std::string b(n, '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(b.data(), static_cast<std::streamsize>(n));
            b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
            return b;
        });
        f.color = c.empty() || f.vector ? "RGB" : c;
        // s189: its clipping path, if Photoshop gave it one
        f.clip = core::pack_clip(core::clipping_path([&in](std::uint64_t off, std::size_t n) {
            std::string b(n, '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(b.data(), static_cast<std::streamsize>(n));
            b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
            return b;
        }));
    }
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
        if (is_psd(path)) {   // s185: read here (shrunk on the way for the screen)
            if (file_size(path, fw, fh)) { e.w = fw; e.h = fh; e.pix = psd_surface(path, kMaxPx); }
        } else if (gdk_pixbuf_get_file_info(path.c_str(), &fw, &fh) && fw > 0 && fh > 0) {
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
#ifdef COMPOSIX_HAVE_TIFF
    if (!e.pix)   // s187: a CMYK TIFF gdk-pixbuf can't decode (16-bit)
        if (cairo_surface_t* t = tiff_cmyk_surface(path, kMaxPx, false))
            e.pix = Cairo::RefPtr<Cairo::ImageSurface>(new Cairo::ImageSurface(t, true));
#endif
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
    const bool big = stamp != 0 && file_size(path, fw, fh) &&   // s185: a PSD's too
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
    {   // s189 (G3): its clipping path, when it has one -- what the designer drew wins over its pixels
        std::ifstream in(path, std::ios::binary);
        const auto clip = core::clipping_path([&in](std::uint64_t off, std::size_t n) {
            std::string b(n, '\0');
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(b.data(), static_cast<std::streamsize>(n));
            b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
            return b;
        });
        int fw = 0, fh = 0;
        if (!clip.polys.empty() && file_size(path, fw, fh)) {
            auto s = core::clip_shape(clip, fw, fh);
            if (auto lg = log::get(log::Area::Io))
                lg->info("trace: '{}' clipping path '{}' ({} subpaths) -> {}", path, clip.name, clip.polys.size(),
                         s.empty() ? "nothing" : std::to_string((s.size() - 1) / 2) + " bands");
            if (!s.empty()) return s;
        }
    }
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
    if (is_psd(path)) {   // s185: full size (s187: a CMYK one's separations with it)
        int w = 0, h = 0;
        std::string why;
        std::vector<std::uint8_t> ink;
        const auto px = layout::psd_decode(whole_file(path), w, h, 0, &why, &ink);
        if (px.empty()) return nullptr;
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_surface_flush(s);
        unsigned char* d = cairo_image_surface_get_data(s);
        for (int y = 0; y < h; ++y)
            std::memcpy(d + static_cast<std::size_t>(y) * static_cast<std::size_t>(cairo_image_surface_get_stride(s)),
                        px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w), static_cast<std::size_t>(w) * 4);
        cairo_surface_mark_dirty(s);
        if (!ink.empty()) attach_ink(s, std::move(ink));
        return s;
    }
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
#ifdef COMPOSIX_HAVE_TIFF
        if (ext == ".tif" || ext == ".tiff") {   // s187: a CMYK TIFF's own separations
            auto ink = tiff_ink(path, cairo_image_surface_get_width(raw), cairo_image_surface_get_height(raw));
            if (!ink.empty()) attach_ink(raw, std::move(ink));
        }
#endif
        if (ext == ".jpg" || ext == ".jpeg") {   // the original JPEG goes into the PDF as is
            gchar* bytes = nullptr;
            gsize len = 0;
            if (g_file_get_contents(path.c_str(), &bytes, &len, nullptr))
                cairo_surface_set_mime_data(raw, CAIRO_MIME_TYPE_JPEG, reinterpret_cast<unsigned char*>(bytes), len,
                                            g_free, bytes);
        }
        return raw;
    } catch (const Glib::Error&) {
#ifdef COMPOSIX_HAVE_TIFF
        return tiff_cmyk_surface(path, 0, true);   // s187: a 16-bit CMYK TIFF, from its own separations
#else
        return nullptr;
#endif
    }
}

const std::vector<std::string>& patterns() {
    static const std::vector<std::string> p{"*.png", "*.jpg", "*.jpeg", "*.tif", "*.tiff", "*.gif",
                                            "*.bmp", "*.svg", "*.webp", "*.psd"};   // s185
    return p;
}

}  // namespace composix::views::images
