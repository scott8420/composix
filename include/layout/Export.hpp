#pragma once
// layout::export_pdf -- the document as a PDF (s013).
//
// In the layout library (cairo + Pango, no GTK) so it is selftested headless.
// Every page at its own size (a foldout is a wide page), in page order, its
// masters' items under its own; text drawn by the SAME Pango layouts the
// canvas draws (layout::Flow), so the PDF breaks exactly as the screen does,
// with the fonts embedded (subset) by cairo; images at full resolution,
// cropped by their frames. Optionally the bleed, and crop marks outside it.
// Page labels are the folios ("iv", "7").
//
// The core never opens image files, and neither does this: the app hands an
// ImageSource that decodes one (gdk-pixbuf) -- the selftest hands a fake.
#include "core/Doc.hpp"
#include "layout/Flow.hpp"

#include <cairo.h>

#include <functional>
#include <string>
#include <set>
#include <vector>

namespace composix::layout {

enum class plate_standard { None, X1a, X4 };

struct PdfOptions {
    bool        bleed = true;    // extend the media by the document's bleed
    bool        marks = false;   // crop marks outside the bleed
    std::string title;           // the PDF's title ("" = the document's)
    bool        compress = true; // print writer: false leaves content streams readable (tests, debugging)
    // s016, print writer: the press's CMYK profile ("" = none: device CMYK,
    // RGB untouched); convert RGB images / colours to it; perceptual intent
    // (else relative colorimetric with black point compensation).
    std::string profile;
    bool        convert = false;
    bool        perceptual = false;
    // s017: PDF/X conformance (needs a profile). X-1a converts everything to
    // CMYK and flattens transparency; X-4 keeps RGB images ICC-tagged.
    plate_standard standard = plate_standard::None;
    // s054: just these pages (ids, document order; empty = all), and
    // bookmarks from the contents list.
    std::vector<std::string> only;
    bool bookmarks = true;
    // s082: the generated lists the outline comes from (ids; empty = the
    // first Contents list, as s054).
    std::vector<std::string> mark_lists;
    // s084: facing pages as one sheet each (a spread's written pages side by
    // side, tops aligned; bleed on the outer edges only, the trim around the
    // whole spread).
    bool spreads = false;
    // s102: how far along (0..1) and what it's doing, from the writer's
    // thread (the app exports on a worker, with a progress bar).
    std::function<void(double, const std::string&)> progress;
};

// s084: the PDF's sheets -- each one page, or (spreads) a spread's pages
// from `pages`, in order: each page's x on the sheet, the sheet's size
// (trim, no bleed), and its label ("ii\u2013iii").
struct Sheet {
    std::vector<std::string> pages;
    std::vector<double>      x;
    double                   w = 0, h = 0;
    std::string              label;
};
std::vector<Sheet> pdf_sheets(const core::Doc& d, const std::vector<std::string>& pages, bool spreads);
// Where a page went: its sheet (index) and x there; sheet -1 = not written.
struct SheetAt { int sheet = -1; double x = 0; };
SheetAt sheet_of(const std::vector<Sheet>& sheets, const std::string& page);

// s054: the reader's outline -- each paragraph in a contents list's styles
// (the first Contents list; its first style level 1, the next level 2 ...),
// where it lands: its page and how far down (points from the page's top).
// In reading order; overset or unplaced paragraphs are left out.
struct Bookmark {
    std::string title;
    int         level = 1;
    std::string page;
    double      y = 0;
    bool operator==(const Bookmark&) const = default;
};
std::vector<Bookmark> bookmarks(const core::Doc& d, FlowCache& flows);
// s082: from these lists (ids; empty or none still there = the first
// Contents list), in the document's list order. The first with entries gives the
// top level as above; each further list nests under one bookmark named for it
// (pointing where its first entry lands). `only`: just entries on these
// pages (empty = every page) -- so a group points at a page that's written.
std::vector<Bookmark> bookmarks(const core::Doc& d, FlowCache& flows, const std::vector<std::string>& lists,
                                const std::vector<std::string>& only = {});
// s082: the lists that can give bookmarks (they gather styles), in order.
std::vector<std::string> bookmark_lists(const core::Doc& d);
// s082: of those, the ones `picked` names -- or, when none, the default.
std::vector<std::string> bookmark_lists_used(const core::Doc& d, const std::vector<std::string>& picked);

// s083: the contents page's links. On the page a Contents list is placed
// on, each paragraph whose text before its first tab (trimmed) is the title
// of a heading the list gathers -- the k-th such entry to the k-th such
// heading -- is a box (its lines, on that page) going to where the heading
// lands. Paragraphs in the list's own heading styles are not entries.
struct PdfLink {
    std::string page;            // the entry's page
    core::Rect  box;             // on it, points from its top-left
    std::string to;              // the heading's page
    double      to_y = 0;        // ... how far down
    bool operator==(const PdfLink&) const = default;
};
std::vector<PdfLink> contents_links(const core::Doc& d, FlowCache& flows);

// s087: a generated list's entries -- each paragraph it gathers, in reading
// order: its text (tabs as spaces, trimmed) and the folio of the page it
// lands on. Overset / master paragraphs left out.
std::vector<core::doc::ListEntry> list_entries(const core::Doc& d, FlowCache& flows, const std::string& list);
// s087: Build the list (core::doc::build_list) with entries that hold once
// written -- worked out on a copy until the folios stop moving (a longer
// contents page can push the headings on), at most a few rounds. One step.
bool build_list(core::DocJournal& j, core::Doc& d, const std::string& list);

// s089: the Contents lists whose written entries no longer say what the
// headings do -- a heading renamed, moved to another page, added or gone --
// or that were never built. (The words and page numbers; not the styles.)
// `why`: the first difference, in words ("Chapter Two: page 3 \u2192 4",
// "Not built yet"). Lists with neither a story nor a page are left out.
struct StaleList {
    std::string list, why;
    bool operator==(const StaleList&) const = default;
};
std::vector<StaleList> stale_lists(const core::Doc& d, FlowCache& flows);

// s086: what an export will give, in words, for the Export sheet -- worked
// out from the options as they stand (no file written). `print`: Plate (else
// the screen writer); `profile_name`: the output profile's ("" = none).
struct ExportSummary {
    int pages = 0, sheets = 0, bookmarks = 0, links = 0;
    std::string size;     // "16 pages \u2192 9 sheets \u00b7 6 \u00d7 9 in + 0.125 in bleed"
    std::string colour;   // "CMYK for FOGRA39 \u2014 RGB converted, perceptual"
    std::string nav;      // "5 bookmarks \u00b7 2 contents links"
};
ExportSummary export_summary(const core::Doc& d, FlowCache& flows, const PdfOptions& opt, bool print,
                             const std::string& profile_name);

struct PdfReport {
    int                      pages = 0;
    std::vector<std::string> warnings;   // overset text, missing / low-res images
};

// An asset's picture for print: a NEW cairo surface reference (the exporter
// destroys it), or nullptr when it can't be read. `want_w` / `want_h`: the
// placed size in points (a vector renders to suit; a raster ignores it).
using ImageSource = std::function<cairo_surface_t*(const core::Asset& a, double want_w, double want_h)>;

// s020: one page (or master) as the cairo writer draws it -- its masters'
// items, then its own: fills, text, images, strokes -- in its own points
// from (0, 0). Shared by the screen PDF and the page thumbnails.
// `placeholders` draws a soft plate where a picture can't be had (thumbs).
struct PageDraw {
    bool placeholders = false;
};
void draw_page(cairo_t* cr, const core::Doc& d, const core::Doc::Id& page_or_master, FlowCache& flows,
               const ImageSource& images, std::set<std::string>* missing = nullptr, const PageDraw& how = {});

// s020: everything that decides how a page LOOKS, to compare: its record,
// its masters', the frames on them and their assets, every style and swatch,
// and the text lines set in its text frames (with their words). Two equal
// keys draw the same page -- a thumbnail is redrawn only when its key moves.
struct PageKey {
    std::vector<std::pair<std::string, core::Record>> recs;
    std::vector<std::string>                          lines;
    bool operator==(const PageKey&) const = default;
};
PageKey page_key(const core::Doc& d, FlowCache& flows, const core::Doc::Id& page_or_master);

bool export_pdf(const core::Doc& d, const std::string& path, const PdfOptions& opt, const ImageSource& images,
                PdfReport* report = nullptr, std::string* err = nullptr);

// s015: the PRINT PDF, written by Plate (libs/plate) instead of cairo: the
// same pages, plus a TrimBox / BleedBox on every page, colour as the swatches
// say (process CMYK stays CMYK; RGB stays RGB; black text is 100 K; crop marks
// in registration), fonts subset by HarfBuzz. Spot colours go as their CMYK
// for now (separations: s016).
bool export_print_pdf(const core::Doc& d, const std::string& path, const PdfOptions& opt, const ImageSource& images,
                      PdfReport* report = nullptr, std::string* err = nullptr);

// What's worth a look after an export (shared by both writers).
void preflight(const core::Doc& d, const std::vector<std::string>& missing, int pages, PdfReport* report);

}  // namespace composix::layout
