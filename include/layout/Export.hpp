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

#include <cairo.h>

#include <functional>
#include <string>
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
};

struct PdfReport {
    int                      pages = 0;
    std::vector<std::string> warnings;   // overset text, missing / low-res images
};

// An asset's picture for print: a NEW cairo surface reference (the exporter
// destroys it), or nullptr when it can't be read. `want_w` / `want_h`: the
// placed size in points (a vector renders to suit; a raster ignores it).
using ImageSource = std::function<cairo_surface_t*(const core::Asset& a, double want_w, double want_h)>;

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
