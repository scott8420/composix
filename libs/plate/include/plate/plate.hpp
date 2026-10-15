#pragma once
// Plate -- a small PDF writer for print (s015, Composix; reusable by any app).
//
// What it is for: pages built from glyph runs, images and simple paths, with
// colour written as the printer needs it (Gray / RGB / CMYK now; spot colours
// and PDF/X output intents next), fonts embedded as SUBSETS, page boxes
// (Media / Bleed / Trim) and page labels. No GTK, no cairo: HarfBuzz (faces +
// the subsetter) and zlib. An optional adapter (plate/pango.hpp, library
// plate_pango) turns Pango lines into glyph runs.
//
// Coordinates: points, origin at the page's TOP-LEFT, y down -- as layout
// code thinks. Plate flips to PDF's bottom-up space itself.
//
//   plate::Document doc;
//   auto font = doc.add_font(hb_face);                 // any HarfBuzz face
//   auto& pg = doc.add_page(612, 792);
//   pg.set_fill(plate::Color::cmyk(0, 0, 0, 1));
//   pg.glyphs(font, 11, 72, 100, run);                  // positioned glyphs
//   doc.save("out.pdf");
#include <hb.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plate {

struct Color {
    enum class Space { Gray, RGB, CMYK, Spot };
    Space  space = Space::Gray;
    double v[4] = {0, 0, 0, 0};   // 0..1 each; Gray uses v[0] (0 = black); Spot: v[0] = the tint
    // s016: a spot colour -- its ink's name and its CMYK look (for proofs and
    // for devices without that ink). Written as a Separation colour space.
    std::string name;
    double      alt[4] = {0, 0, 0, 0};
    static Color gray(double g) { Color c; c.space = Space::Gray; c.v[0] = g; return c; }
    static Color rgb(double r, double g, double b) { Color c; c.space = Space::RGB; c.v[0] = r; c.v[1] = g; c.v[2] = b; return c; }
    static Color cmyk(double cc, double m, double y, double k) {
        Color c; c.space = Space::CMYK; c.v[0] = cc; c.v[1] = m; c.v[2] = y; c.v[3] = k; return c;
    }
    static Color spot(const std::string& ink, double tint, double c, double m, double y, double k) {
        Color s; s.space = Space::Spot; s.name = ink; s.v[0] = tint;
        s.alt[0] = c; s.alt[1] = m; s.alt[2] = y; s.alt[3] = k; return s;
    }
    bool operator==(const Color&) const = default;
};

// s016: how colour is made ready for the press.
enum class Intent { Perceptual, RelativeColorimetric, Saturation, AbsoluteColorimetric };

// s017: the PDF/X standard a document is written to.
//   X1a_2003 -- CMYK and spot only (RGB is converted), no transparency
//               (alpha flattened against white), PDF 1.4.
//   X4       -- RGB allowed when ICC-tagged, transparency allowed, PDF 1.6.
// Both: an output profile (embedded as the output intent), a TrimBox on
// every page, fonts embedded, Trapped stated, XMP metadata, a document ID.
enum class Standard { None, X1a_2003, X4 };

struct Rect { double x = 0, y = 0, w = 0, h = 0; };

// One positioned glyph. `advance`: the pen's move after it (points);
// x_off / y_off: its offset from the pen (points, y down). `text`: the
// characters it stands for (UTF-8), so the PDF's text can be copied /
// searched -- the first glyph of a cluster carries the cluster's text.
struct Glyph {
    std::uint32_t gid = 0;
    double        advance = 0, x_off = 0, y_off = 0;
    std::string   text;
};

using FontId  = int;
using ImageId = int;

class Document;

class Page {
public:
    // Boxes (top-left coordinates). Unset = the MediaBox.
    void set_bleed_box(const Rect& r) { m_bleed = r; m_has_bleed = true; }
    void set_trim_box(const Rect& r) { m_trim = r; m_has_trim = true; }
    void set_label(const std::string& l) { m_label = l; }   // "iv", "7"

    void save();
    void restore();
    void clip_rect(const Rect& r);
    void translate(double x, double y);   // within save() / restore()
    void set_fill(const Color& c);
    void set_stroke(const Color& c);
    void set_line_width(double w);
    void move_to(double x, double y);
    void line_to(double x, double y);
    void rect(const Rect& r);
    void fill();
    void stroke();
    // An image stretched into r.
    void image(ImageId id, const Rect& r);
    // Glyphs of one font at one size, the first glyph's pen at (x, baseline).
    void glyphs(FontId font, double size, double x, double baseline, const std::vector<Glyph>& g);

    double width() const { return m_w; }
    double height() const { return m_h; }

private:
    friend class Document;
    Page(Document& d, double w, double h);
    std::string flip_box(const Rect& r) const;

    Document&   m_doc;
    double      m_w, m_h;
    std::string m_ops;                 // the content stream
    Rect        m_bleed, m_trim;
    bool        m_has_bleed = false, m_has_trim = false;
    std::string m_label;
    std::vector<FontId>  m_fonts_used;
    std::vector<ImageId> m_images_used;
    std::vector<int>     m_spots_used;   // s016: indexes into the document's spot list
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    void set_title(const std::string& t) { m_title = t; }
    void set_creator(const std::string& c) { m_creator = c; }
    void set_author(const std::string& a) { m_author = a; }
    // Content streams uncompressed -- for reading the PDF by eye (debugging).
    void set_compress(bool on) { m_compress = on; }

    // ── colour management (s016, LittleCMS) ─────────────────────────────────
    // The press's profile (a CMYK output ICC). With it, RGB can be CONVERTED
    // to the press's CMYK (images and colours, from sRGB), or KEPT as RGB and
    // tagged sRGB (ICCBased) so a RIP converts it. Set before adding images.
    bool set_output_profile(const std::string& icc_path, std::string* err = nullptr);
    bool set_output_profile_bytes(const std::string& icc, std::string* err = nullptr);
    bool has_output_profile() const;
    std::string output_profile_name() const;   // its description ("GRACoL2013_CRPC6")
    void set_convert_rgb(bool on) { m_convert = on; }
    // X-1a converts whether asked or not: it allows no RGB.
    bool convert_rgb() const { return (m_convert || m_standard == Standard::X1a_2003) && has_output_profile(); }
    void set_intent(Intent i);
    // A colour as it will be written (RGB converted when converting).
    Color prepare(const Color& c) const;

    // ── PDF/X (s017) ────────────────────────────────────────────────────────
    // Set BEFORE adding images (X-1a flattens their alpha and converts RGB).
    void set_standard(Standard s) { m_standard = s; }
    Standard standard() const { return m_standard; }
    static const char* standard_name(Standard s);   // "PDF/X-1a:2003", "PDF/X-4"
    void set_date(long long unix_seconds) { m_date = unix_seconds; }   // default: now
    // What would make the file fail its standard (empty = conforms). save()
    // refuses a non-conforming file and returns these as its error.
    std::vector<std::string> preflight() const;
    int flattened() const { return m_flattened; }   // X-1a: images whose alpha went to white

    // The same face (by HarfBuzz identity) is the same font. Plate keeps a
    // reference; glyphs are collected as pages use them and the face is
    // SUBSET on save (glyph ids kept, so content streams stay valid).
    FontId add_font(hb_face_t* face);
    // 8-bit RGB rows (3 bytes a pixel, w*3 a row); `alpha` (w*h) may be null.
    ImageId add_image_rgb(int w, int h, const std::uint8_t* rgb, const std::uint8_t* alpha = nullptr);
    // A JPEG's own bytes, embedded untouched (1 = gray, 3 = RGB, 4 = CMYK).
    ImageId add_image_jpeg(const std::string& bytes, int w, int h, int components);

    Page& add_page(double w, double h);
    std::size_t page_count() const { return m_pages.size(); }

    bool save(const std::string& path, std::string* err = nullptr);

private:
    friend class Page;
    struct Font;
    struct Image;
    std::vector<std::unique_ptr<Page>>  m_pages;
    std::vector<std::unique_ptr<Font>>  m_fonts;
    std::vector<std::unique_ptr<Image>> m_images;
    std::string m_title, m_creator = "Plate", m_author;
    bool m_compress = true;

    // s016
    struct Cms;
    std::unique_ptr<Cms> m_cms;
    bool m_convert = false;
    struct Spot { std::string name; double alt[4]; };
    std::vector<Spot> m_spots;
    int spot_index(const Color& c);

    // s017
    Standard  m_standard = Standard::None;
    long long m_date = 0;
    int       m_flattened = 0;    // images whose alpha X-1a flattened
    bool      m_rgb_written = false;   // an RGB colour reached a content stream
};

// s016: the CMYK output (press) profiles in these folders (searched one level
// of subfolders deep), by their description -- for an app's profile picker.
struct ProfileInfo { std::string path, name; };
std::vector<ProfileInfo> find_cmyk_profiles(const std::vector<std::string>& dirs);

// Parse a JPEG's size and component count from its header (false: not one).
bool jpeg_info(const std::string& bytes, int* w, int* h, int* components);

}  // namespace plate
