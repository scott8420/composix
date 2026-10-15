#include "views/Images.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdkmm/general.h>
#include <gdkmm/pixbuf.h>
#include <cairomm/context.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace composix::views::images {

namespace {
struct Entry {
    std::int64_t              stamp = -1;
    Cairo::RefPtr<Cairo::ImageSurface> pix;
    int                       w = 0, h = 0;
};
std::map<std::string, Entry>& cache() {
    static std::map<std::string, Entry> c;
    return c;
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

Cairo::RefPtr<Cairo::ImageSurface> get(const std::string& path, std::int64_t stamp, int* w, int* h) {
    if (path.empty()) return {};
    auto& e = cache()[path];
    if (e.stamp != stamp) {
        e = Entry{};
        e.stamp = stamp;
        if (stamp != 0) {
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
        }
    }
    if (w) *w = e.w;
    if (h) *h = e.h;
    return e.pix;
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
