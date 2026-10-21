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
    void concat(double a, double b, double c, double d, double e, double f);   // s216: cm -- what follows turned / moved
    void set_line_cap(int cap);     // s204: 0 butt, 1 round, 2 square
    void set_line_join(int join);   // s204: 0 miter, 1 round, 2 bevel
    void set_dash(const std::vector<double>& on_off, double phase = 0);   // s204: empty -- solid
    // s021 (Composix): constant opacity for what's filled (and images) and
    // what's stroked, 0..1 -- an ExtGState (/ca, /CA). Within save() /
    // restore() to scope it. Below 1 is transparency: PDF/X-1a refuses it.
    void set_alpha(double fill, double stroke);
    void move_to(double x, double y);
    void line_to(double x, double y);
    void curve_to(double x1, double y1, double x2, double y2, double x, double y);   // s197: a cubic Bezier
    void rect(const Rect& r);
    void close_path();       // s189
    void clip_even_odd();    // s189: the path built so far clips what follows (even-odd), and is ended
    void fill();
    void fill_even_odd();   // s211: f* -- a path inside another is a hole
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
    // s021: this page's ExtGStates. A struct, not std::pair<double, double>
    // (GCC's psabi note on passing that pair by value).
    struct Alpha {
        double fill = 1, stroke = 1;
        bool operator==(const Alpha&) const = default;
    };
    std::vector<Alpha> m_alphas;
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
    // s187: 8-bit CMYK rows, as ink (0 none .. 255 full; 4 bytes a pixel) --
    // the picture's own separations, embedded as they are; `alpha` may be null.
    ImageId add_image_cmyk(int w, int h, const std::uint8_t* cmyk, const std::uint8_t* alpha = nullptr);
    // A JPEG's own bytes, embedded untouched (1 = gray, 3 = RGB, 4 = CMYK).
    ImageId add_image_jpeg(const std::string& bytes, int w, int h, int components);

    Page& add_page(double w, double h);
    std::size_t page_count() const { return m_pages.size(); }

    // s054: a bookmark (the reader's outline): its title, its level (1 = top;
    // a deeper level nests under the bookmark before it), and where it goes --
    // a page (0-based, as added) and a height on it (points from the top).
    // Written as the document's /Outlines, the reader opening with them shown.
    void add_bookmark(const std::string& title, int level, std::size_t page, double y);
    // s083: a link -- a box on a page (points, from its top-left) that goes
    // to another page (0-based) at a height. An invisible /Link annotation.
    // Not written under PDF/X (annotations inside the bleed are refused), nor
    // when either page isn't there.
    void add_link(std::size_t page, double x, double y, double w, double h, std::size_t to_page, double to_y);
    // s303: a link out of the document -- a box that opens a URI (a web
    // address, mailto:). Bytes past ASCII are %-escaped. Not under PDF/X.
    void add_uri(std::size_t page, double x, double y, double w, double h, const std::string& uri);

    bool save(const std::string& path, std::string* err = nullptr);

private:
    friend class Page;
    struct Font;
    struct Image;
    std::vector<std::unique_ptr<Page>>  m_pages;
    std::vector<std::unique_ptr<Font>>  m_fonts;
    std::vector<std::unique_ptr<Image>> m_images;
    std::string m_title, m_creator = "Plate", m_author;
    struct Mark { std::string title; int level; std::size_t page; double y; };
    std::vector<Mark> m_marks;   // s054
    struct Link { std::size_t page; double x, y, w, h; std::size_t to; double to_y; std::string uri; };   // s303: uri set -- out
    std::vector<Link> m_links;   // s083
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
    int       m_transparent = 0;       // s021: set_alpha calls below 1
};

// s016: the CMYK output (press) profiles in these folders (searched one level
// of subfolders deep), by their description -- for an app's profile picker.
struct ProfileInfo { std::string path, name; };
std::vector<ProfileInfo> find_cmyk_profiles(const std::vector<std::string>& dirs);

// s053: a soft proof -- what sRGB colours become on the press, through a
// CMYK output profile, the way the print export converts them (relative
// colorimetric with black point compensation, or perceptual). For each
// colour: its CMYK, the colour it will look like (back in sRGB, for the
// screen), and how far it moves -- CIE Delta E 2000 between the two in Lab.
// About 2: hard to see; 5: plain side by side; 10 and up: a different colour
// (out of the press's gamut).
class Proof {
public:
    Proof();
    ~Proof();
    Proof(const Proof&) = delete;
    Proof& operator=(const Proof&) = delete;
    // s107: `paper` -- simulate() shows the press's paper and black (the way
    // back absolute colorimetric): white comes out the paper's white.
    bool load(const std::string& icc_path, bool perceptual = false, std::string* err = nullptr, bool paper = false);
    bool ok() const;
    bool paper() const;
    // s107: what a CMYK ink mix (0..100 each) looks like on screen through
    // this press -- relative colorimetric (screen white is the paper). Any thread.
    std::uint32_t cmyk_to_screen(const double cmyk[4]) const;
    std::string name() const;   // the profile's description
    struct Result {
        double        cmyk[4] = {0, 0, 0, 0};   // 0..100
        std::uint8_t  after[3] = {0, 0, 0};     // sRGB, what it will look like
        double        delta_e = 0;
    };
    Result check(std::uint8_t r, std::uint8_t g, std::uint8_t b) const;
    // Pixels (RGB, 3 bytes each) proofed in place; the Delta E of each to `de`
    // when given (n entries).
    void proof_pixels(std::uint8_t* rgb, std::size_t n, std::vector<float>* de = nullptr) const;
    // s099: the same look, fast -- one 8-bit sRGB -> press -> sRGB transform
    // (lcms precomputes it), for a whole screen of pixels. No Delta E.
    // Any thread (one at a time).
    void simulate(std::uint8_t* rgb, std::size_t n) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Parse a JPEG's size and component count from its header (false: not one).
bool jpeg_info(const std::string& bytes, int* w, int* h, int* components);

}  // namespace plate
