#pragma once
// plate_pango -- Pango lines as Plate glyph runs. Optional (links Pango).
//
// A PangoLayoutLine is a list of runs, each one font at one size; this turns
// them into plate::Glyph lists with their pen positions, so the PDF sets the
// text exactly as Pango laid it out (the same breaks, kerning, justification).
// Colour is the caller's: each run carries its byte range in the layout's
// text, which the app maps to its own styles.
#include "plate/plate.hpp"

#include <pango/pango.h>

#include <vector>

namespace plate::pango {

struct Run {
    hb_face_t*         face = nullptr;   // owned by Pango (Plate takes its own reference)
    double             size = 0;         // points
    double             x = 0, baseline = 0;
    int                start = 0, end = 0;   // byte range in the layout's text
    std::vector<Glyph> glyphs;
};

// The runs of `line`, its left at x and its baseline at `baseline` (points;
// the layout measured at 72 dpi, one Pango unit = 1/PANGO_SCALE point).
std::vector<Run> runs(PangoLayoutLine* line, double x, double baseline);

// Draw one run in the page's current fill colour.
void show(Document& doc, Page& page, const Run& r);

}  // namespace plate::pango
