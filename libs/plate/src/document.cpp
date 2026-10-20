#include "internal.hpp"

#include <hb-ot.h>
#include <hb-subset.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ctime>
#include <map>

namespace plate {

using detail::num;

// ── fonts and images, as the document keeps them ────────────────────────────
struct Document::Font {
    hb_face_t* face = nullptr;
    std::map<std::uint32_t, std::string> used;   // glyph id -> its text (first seen)
    ~Font() { if (face) hb_face_destroy(face); }
};

struct Document::Image {
    int w = 0, h = 0, comps = 3;   // 3 = RGB pixels, 4 = CMYK (converted, or the picture's own -- s187)
    std::string pix, alpha, jpeg;
};

// ── the page: a content stream in top-left coordinates ──────────────────────
Page::Page(Document& d, double w, double h) : m_doc(d), m_w(w), m_h(h) {
    // Flip once: from here on, y runs down from the top-left corner.
    m_ops = "1 0 0 -1 0 " + num(h) + " cm\n";
}

void Page::save() { m_ops += "q\n"; }
void Page::restore() { m_ops += "Q\n"; }
void Page::translate(double x, double y) { m_ops += "1 0 0 1 " + num(x) + " " + num(y) + " cm\n"; }
void Page::clip_rect(const Rect& r) {
    m_ops += num(r.x) + " " + num(r.y) + " " + num(r.w) + " " + num(r.h) + " re W n\n";
}

namespace {
std::string color_op(const Color& c, bool stroke) {
    switch (c.space) {
    case Color::Space::Spot: return {};   // a page op: Page::colour
    case Color::Space::Gray: return num(c.v[0]) + (stroke ? " G\n" : " g\n");
    case Color::Space::RGB:  return num(c.v[0]) + " " + num(c.v[1]) + " " + num(c.v[2]) + (stroke ? " RG\n" : " rg\n");
    case Color::Space::CMYK:
        return num(c.v[0]) + " " + num(c.v[1]) + " " + num(c.v[2]) + " " + num(c.v[3]) + (stroke ? " K\n" : " k\n");
    }
    return {};
}
}  // namespace

// Spot colours need the page's colour space resources; process colours don't.
static std::string spot_op(int idx, double tint, bool stroke) {
    return "/CS" + std::to_string(idx) + (stroke ? " CS " : " cs ") + num(tint) + (stroke ? " SCN\n" : " scn\n");
}
void Page::set_alpha(double fill, double stroke) {
    fill = std::clamp(fill, 0.0, 1.0);
    stroke = std::clamp(stroke, 0.0, 1.0);
    if (fill < 1 || stroke < 1) ++m_doc.m_transparent;
    const Alpha want{fill, stroke};
    std::size_t i = 0;
    while (i < m_alphas.size() && !(m_alphas[i] == want)) ++i;
    if (i == m_alphas.size()) m_alphas.push_back(want);
    m_ops += "/GS" + std::to_string(i) + " gs\n";
}

void Page::set_fill(const Color& c0) {
    const Color c = m_doc.prepare(c0);
    if (c.space == Color::Space::Spot) {
        const int i = m_doc.spot_index(c);
        if (std::find(m_spots_used.begin(), m_spots_used.end(), i) == m_spots_used.end()) m_spots_used.push_back(i);
        m_ops += spot_op(i, c.v[0], false);
        return;
    }
    if (c.space == Color::Space::RGB) m_doc.m_rgb_written = true;
    m_ops += color_op(c, false);
}
void Page::set_stroke(const Color& c0) {
    const Color c = m_doc.prepare(c0);
    if (c.space == Color::Space::Spot) {
        const int i = m_doc.spot_index(c);
        if (std::find(m_spots_used.begin(), m_spots_used.end(), i) == m_spots_used.end()) m_spots_used.push_back(i);
        m_ops += spot_op(i, c.v[0], true);
        return;
    }
    if (c.space == Color::Space::RGB) m_doc.m_rgb_written = true;
    m_ops += color_op(c, true);
}
void Page::set_line_width(double w) { m_ops += num(w) + " w\n"; }
void Page::set_line_cap(int cap) { m_ops += std::to_string(std::clamp(cap, 0, 2)) + " J\n"; }     // s204
void Page::set_line_join(int join) { m_ops += std::to_string(std::clamp(join, 0, 2)) + " j\n"; }
void Page::set_dash(const std::vector<double>& on_off, double phase) {
    m_ops += "[";
    for (std::size_t i = 0; i < on_off.size(); ++i) m_ops += (i ? " " : "") + num(on_off[i]);
    m_ops += "] " + num(phase) + " d\n";
}
void Page::move_to(double x, double y) { m_ops += num(x) + " " + num(y) + " m\n"; }
void Page::line_to(double x, double y) { m_ops += num(x) + " " + num(y) + " l\n"; }
void Page::curve_to(double x1, double y1, double x2, double y2, double x, double y) {   // s197
    m_ops += num(x1) + " " + num(y1) + " " + num(x2) + " " + num(y2) + " " + num(x) + " " + num(y) + " c\n";
}
void Page::rect(const Rect& r) { m_ops += num(r.x) + " " + num(r.y) + " " + num(r.w) + " " + num(r.h) + " re\n"; }
void Page::fill() { m_ops += "f\n"; }
void Page::fill_even_odd() { m_ops += "f*\n"; }   // s211
void Page::close_path() { m_ops += "h\n"; }            // s189
void Page::clip_even_odd() { m_ops += "W* n\n"; }      // s189
void Page::stroke() { m_ops += "S\n"; }

void Page::image(ImageId id, const Rect& r) {
    if (id < 0 || id >= static_cast<int>(m_doc.m_images.size())) return;
    if (std::find(m_images_used.begin(), m_images_used.end(), id) == m_images_used.end()) m_images_used.push_back(id);
    // The image's unit square, its top row at the top: y is already flipped.
    m_ops += "q " + num(r.w) + " 0 0 " + num(-r.h) + " " + num(r.x) + " " + num(r.y + r.h) + " cm /Im" +
             std::to_string(id) + " Do Q\n";
}

void Page::glyphs(FontId font, double size, double x, double baseline, const std::vector<Glyph>& g) {
    if (g.empty() || font < 0 || font >= static_cast<int>(m_doc.m_fonts.size()) || size <= 0) return;
    auto& f = *m_doc.m_fonts[static_cast<std::size_t>(font)];
    if (std::find(m_fonts_used.begin(), m_fonts_used.end(), font) == m_fonts_used.end()) m_fonts_used.push_back(font);
    hb_font_t* hf = hb_font_create(f.face);
    const double upem = hb_face_get_upem(f.face);
    const auto width = [&](std::uint32_t gid) { return hb_font_get_glyph_h_advance(hf, gid) / upem * size; };

    m_ops += "BT /F" + std::to_string(font) + " " + num(size) + " Tf\n";
    // Where each glyph must sit; the PDF's own advance (the font's width)
    // differs from Pango's (kerning, justification): TJ adjusts between them.
    double pen = x;
    bool open = false;
    double at = 0, cur_y = 0;   // the PDF pen after the last glyph shown
    for (const auto& gl : g) {
        const double gx = pen + gl.x_off, gy = baseline + gl.y_off;
        if (!open || std::abs(gy - cur_y) > 1e-6) {
            if (open) m_ops += "] TJ\n";
            m_ops += "1 0 0 -1 " + num(gx) + " " + num(gy) + " Tm [";
            open = true;
            at = gx;
            cur_y = gy;
        } else if (std::abs(at - gx) > 1e-6) {
            m_ops += num((at - gx) * 1000 / size);
        }
        char hex[8];
        std::snprintf(hex, sizeof hex, "<%04X>", gl.gid & 0xFFFF);
        m_ops += hex;
        if (!f.used.count(gl.gid) || f.used[gl.gid].empty()) f.used[gl.gid] = gl.text;
        at = gx + width(gl.gid);
        pen += gl.advance;
    }
    if (open) m_ops += "] TJ\n";
    m_ops += "ET\n";
    hb_font_destroy(hf);
}

std::string Page::flip_box(const Rect& r) const {
    return "[" + num(r.x) + " " + num(m_h - r.y - r.h) + " " + num(r.x + r.w) + " " + num(m_h - r.y) + "]";
}

// ── the document ────────────────────────────────────────────────────────────
Document::Document() = default;
Document::~Document() = default;

FontId Document::add_font(hb_face_t* face) {
    for (std::size_t i = 0; i < m_fonts.size(); ++i)
        if (m_fonts[i]->face == face) return static_cast<FontId>(i);
    auto f = std::make_unique<Font>();
    f->face = hb_face_reference(face);
    f->used[0] = "";   // .notdef is always there
    m_fonts.push_back(std::move(f));
    return static_cast<FontId>(m_fonts.size() - 1);
}

ImageId Document::add_image_rgb(int w, int h, const std::uint8_t* rgb, const std::uint8_t* alpha) {
    auto im = std::make_unique<Image>();
    im->w = w;
    im->h = h;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    // s017: PDF/X-1a allows no transparency -- the alpha is flattened against
    // white (paper) here, and the image goes in opaque.
    std::string flat;
    if (alpha && m_standard == Standard::X1a_2003) {
        flat.assign(reinterpret_cast<const char*>(rgb), n * 3);
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                const unsigned v = static_cast<unsigned char>(flat[i * 3 + k]), a = alpha[i];
                flat[i * 3 + k] = static_cast<char>((v * a + 255 * (255 - a)) / 255);
            }
        rgb = reinterpret_cast<const std::uint8_t*>(flat.data());
        alpha = nullptr;
        ++m_flattened;
    }
    if (convert_rgb()) {   // s016: to the press's CMYK, now
        im->comps = 4;
        im->pix.resize(n * 4);
        cmsDoTransform(m_cms->img, rgb, im->pix.data(), static_cast<cmsUInt32Number>(n));
    } else {
        im->pix.assign(reinterpret_cast<const char*>(rgb), n * 3);
    }
    if (alpha) {
        bool opaque = true;
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h && opaque; ++i) opaque = alpha[i] == 255;
        if (!opaque) im->alpha.assign(reinterpret_cast<const char*>(alpha), static_cast<std::size_t>(w) * h);
    }
    m_images.push_back(std::move(im));
    return static_cast<ImageId>(m_images.size() - 1);
}

ImageId Document::add_image_cmyk(int w, int h, const std::uint8_t* cmyk, const std::uint8_t* alpha) {   // s187
    auto im = std::make_unique<Image>();
    im->w = w;
    im->h = h;
    im->comps = 4;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    im->pix.assign(reinterpret_cast<const char*>(cmyk), n * 4);
    if (alpha && m_standard == Standard::X1a_2003) {   // as add_image_rgb: flattened on paper (no ink where see-through)
        for (std::size_t i = 0; i < n; ++i)
            for (int k = 0; k < 4; ++k) {
                const unsigned v = static_cast<unsigned char>(im->pix[i * 4 + k]);
                im->pix[i * 4 + k] = static_cast<char>(v * alpha[i] / 255);
            }
        alpha = nullptr;
        ++m_flattened;
    }
    if (alpha) {
        bool opaque = true;
        for (std::size_t i = 0; i < n && opaque; ++i) opaque = alpha[i] == 255;
        if (!opaque) im->alpha.assign(reinterpret_cast<const char*>(alpha), n);
    }
    m_images.push_back(std::move(im));
    return static_cast<ImageId>(m_images.size() - 1);
}

ImageId Document::add_image_jpeg(const std::string& bytes, int w, int h, int components) {
    auto im = std::make_unique<Image>();
    im->w = w;
    im->h = h;
    im->comps = components;
    im->jpeg = bytes;
    m_images.push_back(std::move(im));
    return static_cast<ImageId>(m_images.size() - 1);
}

Page& Document::add_page(double w, double h) {
    m_pages.push_back(std::unique_ptr<Page>(new Page(*this, w, h)));
    return *m_pages.back();
}

namespace {

std::string tag_for(std::size_t i) {   // the subset prefix: six capitals, unique per font
    std::string t = "AAAAAA";
    for (int k = 5; k >= 0 && i; --k) { t[static_cast<std::size_t>(k)] = static_cast<char>('A' + i % 26); i /= 26; }
    return t;
}

std::string ps_name(hb_face_t* face) {
    char buf[128];
    unsigned len = sizeof buf;
    std::string out;
    if (hb_ot_name_get_utf8(face, HB_OT_NAME_ID_POSTSCRIPT_NAME, HB_LANGUAGE_INVALID, &len, buf) > 0) out = buf;
    std::string clean;
    for (char c : out)
        if (c > ' ' && c < 127 && c != '/' && c != '(' && c != ')' && c != '[' && c != ']' && c != '<' && c != '>')
            clean += c;
    return clean.empty() ? "Font" : clean;
}

int be16(const char* p) { return static_cast<std::int16_t>((static_cast<unsigned char>(p[0]) << 8) | static_cast<unsigned char>(p[1])); }

std::string to_unicode_cmap(const std::map<std::uint32_t, std::string>& used) {
    std::vector<std::pair<std::uint32_t, std::string>> map;
    for (const auto& [gid, text] : used)
        if (!text.empty()) map.push_back({gid, detail::hex_utf16be(text)});
    std::string s =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    for (std::size_t i = 0; i < map.size(); i += 100) {
        const std::size_t n = std::min<std::size_t>(100, map.size() - i);
        s += std::to_string(n) + " beginbfchar\n";
        char hex[8];
        for (std::size_t k = i; k < i + n; ++k) {
            std::snprintf(hex, sizeof hex, "%04X", map[k].first & 0xFFFF);
            s += "<" + std::string(hex) + "> <" + map[k].second + ">\n";
        }
        s += "endbfchar\n";
    }
    return s + "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
}

}  // namespace

void Document::add_bookmark(const std::string& title, int level, std::size_t page, double y) {
    m_marks.push_back({title, level, page, y});
}

void Document::add_link(std::size_t page, double x, double y, double w, double h, std::size_t to_page, double to_y) {
    m_links.push_back({page, x, y, w, h, to_page, to_y});
}

bool Document::save(const std::string& path, std::string* err) {
    if (m_pages.empty()) { if (err) *err = "no pages"; return false; }
    if (m_standard != Standard::None) {
        const auto problems = preflight();
        if (!problems.empty()) {
            if (err) {
                *err = std::string(standard_name(m_standard)) + ":";
                for (const auto& p : problems) *err += "\n\u2022 " + p;
            }
            return false;
        }
    }
    detail::Objects o;
    const int catalog = o.reserve(), pages = o.reserve(), info = o.reserve();
    bool cff = false;

    // Fonts: subset (glyph ids kept), embed, describe.
    std::vector<int> font_obj(m_fonts.size(), 0);
    for (std::size_t i = 0; i < m_fonts.size(); ++i) {
        auto& f = *m_fonts[i];
        bool used_any = false;
        for (const auto& pg : m_pages)
            used_any = used_any || std::find(pg->m_fonts_used.begin(), pg->m_fonts_used.end(), static_cast<int>(i)) != pg->m_fonts_used.end();
        if (!used_any) continue;
        hb_subset_input_t* in = hb_subset_input_create_or_fail();
        if (!in) { if (err) *err = "font subsetter unavailable"; return false; }
        hb_set_t* gl = hb_subset_input_glyph_set(in);
        for (const auto& [gid, t] : f.used) hb_set_add(gl, gid);
        hb_subset_input_set_flags(in, HB_SUBSET_FLAGS_RETAIN_GIDS);
        hb_face_t* sub = hb_subset_or_fail(f.face, in);
        hb_subset_input_destroy(in);
        if (!sub) { if (err) *err = "couldn't subset " + ps_name(f.face); return false; }
        hb_blob_t* blob = hb_face_reference_blob(sub);
        unsigned blen = 0;
        const char* bdata = hb_blob_get_data(blob, &blen);
        const std::string fontdata(bdata, blen);
        hb_blob_destroy(blob);
        hb_face_destroy(sub);

        hb_blob_t* cfft = hb_face_reference_table(f.face, HB_TAG('C', 'F', 'F', ' '));
        const bool is_cff = hb_blob_get_length(cfft) > 0;
        hb_blob_destroy(cfft);
        cff = cff || is_cff;

        const double upem = hb_face_get_upem(f.face);
        const auto k = [&](double v) { return num(v * 1000.0 / upem); };
        hb_font_t* hf = hb_font_create(f.face);
        hb_position_t asc = 0, desc = 0, cap = 0;
        hb_ot_metrics_get_position_with_fallback(hf, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &asc);
        hb_ot_metrics_get_position_with_fallback(hf, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &desc);
        hb_ot_metrics_get_position_with_fallback(hf, HB_OT_METRICS_TAG_CAP_HEIGHT, &cap);
        std::string bbox = "[0 " + k(desc) + " 1000 " + k(asc) + "]";
        hb_blob_t* head = hb_face_reference_table(f.face, HB_TAG('h', 'e', 'a', 'd'));
        unsigned hl = 0;
        const char* hd = hb_blob_get_data(head, &hl);
        if (hl >= 44)
            bbox = "[" + k(be16(hd + 36)) + " " + k(be16(hd + 38)) + " " + k(be16(hd + 40)) + " " + k(be16(hd + 42)) + "]";
        hb_blob_destroy(head);
        double italic = 0;
        hb_blob_t* post = hb_face_reference_table(f.face, HB_TAG('p', 'o', 's', 't'));
        unsigned pl = 0;
        const char* pd = hb_blob_get_data(post, &pl);
        if (pl >= 8) italic = be16(pd + 4) + (static_cast<unsigned char>(pd[6]) * 256 + static_cast<unsigned char>(pd[7])) / 65536.0;
        hb_blob_destroy(post);

        std::string widths = "[";
        for (const auto& [gid, t] : f.used)
            widths += std::to_string(gid) + " [" + k(hb_font_get_glyph_h_advance(hf, gid)) + "] ";
        widths += "]";
        hb_font_destroy(hf);

        const std::string name = tag_for(i + 1) + "+" + ps_name(f.face);
        // CFF: PDF 1.6 takes the whole OpenType file; PDF/X-1a (PDF 1.4) only
        // the bare CFF program (CIDFontType0C), cut from the subset.
        int file = 0;
        if (is_cff && m_standard == Standard::X1a_2003) {
            hb_face_t* sf = hb_face_create(hb_blob_create(fontdata.data(), static_cast<unsigned>(fontdata.size()),
                                                          HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr), 0);
            hb_blob_t* cffb = hb_face_reference_table(sf, HB_TAG('C', 'F', 'F', ' '));
            unsigned cl = 0;
            const char* cd = hb_blob_get_data(cffb, &cl);
            file = o.stream("/Subtype /CIDFontType0C", std::string(cd ? cd : "", cl));
            hb_blob_destroy(cffb);
            hb_face_destroy(sf);
        } else if (is_cff) {
            file = o.stream("/Subtype /OpenType", fontdata);
        } else {
            file = o.stream("/Length1 " + std::to_string(fontdata.size()), fontdata);
        }
        const int desc_obj = o.add("<< /Type /FontDescriptor /FontName /" + name + " /Flags " +
                                   std::string(italic != 0 ? "68" : "4") + " /FontBBox " + bbox + " /ItalicAngle " +
                                   num(italic) + " /Ascent " + k(asc) + " /Descent " + k(desc) + " /CapHeight " +
                                   k(cap ? cap : asc) + " /StemV 80 " + (is_cff ? "/FontFile3 " : "/FontFile2 ") +
                                   std::to_string(file) + " 0 R >>");
        const int cid = o.add("<< /Type /Font /Subtype /" + std::string(is_cff ? "CIDFontType0" : "CIDFontType2") +
                              " /BaseFont /" + name +
                              " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
                              " /FontDescriptor " + std::to_string(desc_obj) + " 0 R /DW 1000 /W " + widths +
                              (is_cff ? "" : " /CIDToGIDMap /Identity") + " >>");
        const int tou = o.stream("", to_unicode_cmap(f.used));
        font_obj[i] = o.add("<< /Type /Font /Subtype /Type0 /BaseFont /" + name +
                            " /Encoding /Identity-H /DescendantFonts [" + std::to_string(cid) + " 0 R] /ToUnicode " +
                            std::to_string(tou) + " 0 R >>");
    }

    // s016: sRGB (for tagged RGB images) and the spot colours' Separations.
    int srgb_obj = 0;
    if (m_cms) srgb_obj = o.stream("/N 3 /Alternate /DeviceRGB", m_cms->srgb_bytes);
    std::vector<int> spot_obj(m_spots.size(), 0);
    for (std::size_t i = 0; i < m_spots.size(); ++i) {
        const auto& sp = m_spots[i];
        const int fn = o.add("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [" + num(sp.alt[0]) + " " +
                             num(sp.alt[1]) + " " + num(sp.alt[2]) + " " + num(sp.alt[3]) + "] /N 1 >>");
        spot_obj[i] = o.add("[/Separation " + detail::pdf_name(sp.name) + " /DeviceCMYK " + std::to_string(fn) + " 0 R]");
    }

    // Images.
    std::vector<int> image_obj(m_images.size(), 0);
    for (std::size_t i = 0; i < m_images.size(); ++i) {
        const auto& im = *m_images[i];
        const std::string size = "/Type /XObject /Subtype /Image /Width " + std::to_string(im.w) + " /Height " +
                                 std::to_string(im.h) + " /BitsPerComponent 8 /Interpolate true";
        if (!im.jpeg.empty()) {
            std::string cs = im.comps == 1 ? "/DeviceGray" : im.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB";
            if (im.comps == 3 && srgb_obj) cs = "[/ICCBased " + std::to_string(srgb_obj) + " 0 R]";
            // Adobe's CMYK JPEGs are stored inverted.
            const std::string decode = im.comps == 4 ? " /Decode [1 0 1 0 1 0 1 0]" : "";
            image_obj[i] = o.stream(size + " /ColorSpace " + cs + decode + " /Filter /DCTDecode", im.jpeg, false);
            continue;
        }
        std::string smask;
        if (!im.alpha.empty())
            smask = " /SMask " + std::to_string(o.stream("/Type /XObject /Subtype /Image /Width " + std::to_string(im.w) +
                                                         " /Height " + std::to_string(im.h) +
                                                         " /BitsPerComponent 8 /ColorSpace /DeviceGray", im.alpha)) + " 0 R";
        std::string cs = im.comps == 4 ? "/DeviceCMYK" : "/DeviceRGB";
        if (im.comps == 3 && srgb_obj) cs = "[/ICCBased " + std::to_string(srgb_obj) + " 0 R]";   // tagged: the RIP converts
        image_obj[i] = o.stream(size + " /ColorSpace " + cs + smask, im.pix);
    }

    // Pages.
    std::string kids, labels;
    std::vector<int> page_obj;   // s054: for bookmarks' destinations
    for (std::size_t i = 0; i < m_pages.size(); ++i) page_obj.push_back(o.reserve());   // s083: links point ahead
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
        const auto& pg = *m_pages[i];
        std::string res = "<< /ProcSet [/PDF /Text /ImageB /ImageC]";
        if (!pg.m_fonts_used.empty()) {
            res += " /Font <<";
            for (int f : pg.m_fonts_used) res += " /F" + std::to_string(f) + " " + std::to_string(font_obj[static_cast<std::size_t>(f)]) + " 0 R";
            res += " >>";
        }
        if (!pg.m_images_used.empty()) {
            res += " /XObject <<";
            for (int im : pg.m_images_used) res += " /Im" + std::to_string(im) + " " + std::to_string(image_obj[static_cast<std::size_t>(im)]) + " 0 R";
            res += " >>";
        }
        if (!pg.m_alphas.empty()) {   // s021
            res += " /ExtGState <<";
            for (std::size_t a = 0; a < pg.m_alphas.size(); ++a)
                res += " /GS" + std::to_string(a) + " << /Type /ExtGState /ca " + num(pg.m_alphas[a].fill) + " /CA " +
                       num(pg.m_alphas[a].stroke) + " >>";
            res += " >>";
        }
        if (!pg.m_spots_used.empty()) {
            res += " /ColorSpace <<";
            for (int sp : pg.m_spots_used) res += " /CS" + std::to_string(sp) + " " + std::to_string(spot_obj[static_cast<std::size_t>(sp)]) + " 0 R";
            res += " >>";
        }
        res += " >>";
        const int content = o.stream("", pg.m_ops, m_compress);
        std::string boxes = " /MediaBox [0 0 " + num(pg.m_w) + " " + num(pg.m_h) + "]";
        if (pg.m_has_bleed) boxes += " /BleedBox " + pg.flip_box(pg.m_bleed);
        if (pg.m_has_trim) boxes += " /TrimBox " + pg.flip_box(pg.m_trim);
        // s083: links -- invisible boxes going to a page (never under PDF/X).
        std::string annots;
        if (m_standard == Standard::None)
            for (const auto& l : m_links) {
                if (l.page != i || l.to >= m_pages.size() || l.w <= 0 || l.h <= 0) continue;
                const auto& to = *m_pages[l.to];
                const int a = o.add("<< /Type /Annot /Subtype /Link /Rect [" + num(l.x) + " " + num(pg.m_h - l.y - l.h) + " " +
                                    num(l.x + l.w) + " " + num(pg.m_h - l.y) + "] /Border [0 0 0] /P " +
                                    std::to_string(page_obj[i]) + " 0 R /Dest [" + std::to_string(page_obj[l.to]) +
                                    " 0 R /XYZ 0 " + num(std::clamp(to.m_h - l.to_y, 0.0, to.m_h)) + " null] >>");
                annots += std::to_string(a) + " 0 R ";
            }
        if (!annots.empty()) annots = " /Annots [" + annots.substr(0, annots.size() - 1) + "]";
        const int id = page_obj[i];
        o.set(id, "<< /Type /Page /Parent " + std::to_string(pages) + " 0 R" + boxes + " /Resources " + res +
                      " /Contents " + std::to_string(content) + " 0 R" + annots + " >>");
        kids += std::to_string(id) + " 0 R ";
        if (!pg.m_label.empty()) labels += std::to_string(i) + " << /P " + detail::text_string(pg.m_label) + " >> ";
    }
    o.set(pages, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(m_pages.size()) + " >>");

    // s017: dates, the document ID, and -- for PDF/X -- the output intent,
    // the version keys and XMP metadata.
    const long long when = m_date ? m_date : static_cast<long long>(std::time(nullptr));
    const std::string pdf_date = detail::pdf_date(when), xmp_date = detail::xmp_date(when);
    const std::string id = detail::doc_id(m_title + "|" + std::to_string(when) + "|" + std::to_string(m_pages.size()));
    std::string extra;
    if (m_standard != Standard::None) {
        const int icc = o.stream("/N 4", m_cms->out_bytes);
        const std::string cond = detail::text_string(m_cms->name);
        const int intent = o.add("<< /Type /OutputIntent /S /GTS_PDFX /OutputConditionIdentifier " + cond +
                                 " /OutputCondition " + cond + " /Info " + cond +
                                 " /RegistryName (http://www.color.org) /DestOutputProfile " + std::to_string(icc) + " 0 R >>");
        const int xmp = o.stream("/Type /Metadata /Subtype /XML",
                                 detail::xmp(m_standard, m_title, m_creator, xmp_date, id), false);
        extra = " /OutputIntents [" + std::to_string(intent) + " 0 R] /Metadata " + std::to_string(xmp) + " 0 R";
    }
    // s054: the outline -- a tree from the levels (a level deeper than the one
    // before nests under it), every item open.
    std::vector<Mark> marks;
    for (const auto& m : m_marks) if (m.page < page_obj.size()) marks.push_back(m);
    if (!marks.empty()) {
        const int root = o.reserve();
        const std::size_t n = marks.size();
        std::vector<int> id(n), parent(n, -1), lvl(n);
        for (std::size_t k = 0; k < n; ++k) id[k] = o.reserve();
        std::vector<int> stack;   // indices of the open ancestors
        for (std::size_t k = 0; k < n; ++k) {
            const int want = std::max(1, marks[k].level);
            while (!stack.empty() && lvl[static_cast<std::size_t>(stack.back())] >= want) stack.pop_back();
            lvl[k] = stack.empty() ? 1 : std::min(want, lvl[static_cast<std::size_t>(stack.back())] + 1);
            parent[k] = stack.empty() ? -1 : stack.back();
            stack.push_back(static_cast<int>(k));
        }
        const auto kids_of = [&](int p) {
            std::vector<std::size_t> v;
            for (std::size_t k = 0; k < n; ++k) if (parent[k] == p) v.push_back(k);
            return v;
        };
        std::vector<int> count(n, 0);   // descendants (all open)
        for (std::size_t k = n; k-- > 0;) if (parent[k] >= 0) count[static_cast<std::size_t>(parent[k])] += 1 + count[k];
        const auto ref = [&](int obj) { return std::to_string(obj) + " 0 R"; };
        for (std::size_t k = 0; k < n; ++k) {
            const auto sib = kids_of(parent[k]);
            const auto at = static_cast<std::size_t>(std::find(sib.begin(), sib.end(), k) - sib.begin());
            const auto ch = kids_of(static_cast<int>(k));
            const auto& pg = *m_pages[marks[k].page];
            std::string e = "<< /Title " + detail::text_string(marks[k].title) + " /Parent " +
                            ref(parent[k] < 0 ? root : id[static_cast<std::size_t>(parent[k])]);
            if (at > 0) e += " /Prev " + ref(id[sib[at - 1]]);
            if (at + 1 < sib.size()) e += " /Next " + ref(id[sib[at + 1]]);
            if (!ch.empty()) e += " /First " + ref(id[ch.front()]) + " /Last " + ref(id[ch.back()]) + " /Count " + std::to_string(count[k]);
            e += " /Dest [" + ref(page_obj[marks[k].page]) + " /XYZ 0 " + num(std::clamp(pg.m_h - marks[k].y, 0.0, pg.m_h)) + " null] >>";
            o.set(id[k], e);
        }
        const auto top = kids_of(-1);
        o.set(root, "<< /Type /Outlines /First " + ref(id[top.front()]) + " /Last " + ref(id[top.back()]) +
                        " /Count " + std::to_string(n) + " >>");
        extra += " /Outlines " + ref(root) + " /PageMode /UseOutlines";
    }
    o.set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R" +
                       (labels.empty() ? "" : " /PageLabels << /Nums [" + labels + "] >>") + extra + " >>");
    std::string inf = "<< /Producer (Plate) /CreationDate (" + pdf_date + ") /ModDate (" + pdf_date + ")";
    if (!m_title.empty()) inf += " /Title " + detail::text_string(m_title);
    if (!m_author.empty()) inf += " /Author " + detail::text_string(m_author);
    if (!m_creator.empty()) inf += " /Creator " + detail::text_string(m_creator);
    if (m_standard == Standard::X1a_2003) inf += " /GTS_PDFXVersion (PDF/X-1:2003) /GTS_PDFXConformance (PDF/X-1a:2003) /Trapped /False";
    if (m_standard == Standard::X4) inf += " /GTS_PDFXVersion (PDF/X-4) /Trapped /False";
    o.set(info, inf + " >>");

    // The version: X-1a is PDF 1.4; X-4 (and OpenType CFF fonts) PDF 1.6.
    std::string version = cff ? "1.6" : "1.4";
    if (m_standard == Standard::X1a_2003) version = "1.4";
    if (m_standard == Standard::X4) version = "1.6";
    const std::string bytes = o.write(catalog, info, version, "/ID [<" + id + "> <" + id + ">]");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { if (err) *err = "can't write " + path; return false; }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) { if (err) *err = "write failed: " + path; return false; }
    return true;
}

const char* Document::standard_name(Standard s) {
    switch (s) {
    case Standard::X1a_2003: return "PDF/X-1a:2003";
    case Standard::X4:       return "PDF/X-4";
    case Standard::None:     break;
    }
    return "PDF";
}

std::vector<std::string> Document::preflight() const {
    std::vector<std::string> p;
    if (m_standard == Standard::None) return p;
    if (!m_cms) p.push_back("needs an output profile (the press's CMYK ICC)");
    for (std::size_t i = 0; i < m_pages.size(); ++i)
        if (!m_pages[i]->m_has_trim) p.push_back("page " + std::to_string(i + 1) + " has no TrimBox");
    if (m_rgb_written) p.push_back("an RGB colour was drawn (set the standard before drawing)");
    if (m_standard == Standard::X1a_2003) {
        int rgb = 0, alpha = 0;
        for (const auto& im : m_images) {
            rgb += im->comps == 3;
            alpha += !im->alpha.empty();
        }
        if (rgb) p.push_back(std::to_string(rgb) + " RGB image(s) -- X-1a allows CMYK and gray only");
        if (alpha) p.push_back(std::to_string(alpha) + " image(s) with transparency -- X-1a allows none");
        if (m_transparent) p.push_back("opacity below 100% (" + std::to_string(m_transparent) + "x) -- X-1a allows no transparency");
    }
    return p;
}

bool jpeg_info(const std::string& b, int* w, int* h, int* comps) {
    if (b.size() < 4 || static_cast<unsigned char>(b[0]) != 0xFF || static_cast<unsigned char>(b[1]) != 0xD8) return false;
    std::size_t i = 2;
    while (i + 9 < b.size()) {
        if (static_cast<unsigned char>(b[i]) != 0xFF) return false;
        const unsigned m = static_cast<unsigned char>(b[i + 1]);
        const std::size_t len = (static_cast<unsigned char>(b[i + 2]) << 8) | static_cast<unsigned char>(b[i + 3]);
        // A frame header (SOF0..SOF15, but not DHT / JPG / DAC).
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (h) *h = (static_cast<unsigned char>(b[i + 5]) << 8) | static_cast<unsigned char>(b[i + 6]);
            if (w) *w = (static_cast<unsigned char>(b[i + 7]) << 8) | static_cast<unsigned char>(b[i + 8]);
            if (comps) *comps = static_cast<unsigned char>(b[i + 9]);
            return true;
        }
        i += 2 + len;
    }
    return false;
}

}  // namespace plate
