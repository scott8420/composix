#pragma once
// layout::color_report -- s053: the RGB colours a CMYK print will convert,
// and how well each survives. Every RGB swatch in use and every placed RGB
// picture, proofed through the press profile (plate::Proof): the CMYK it
// becomes, what it will look like, and how far it moves (Delta E 2000).
//
// GTK-free: the app hands in the pictures' pixels (a small sample of each).
#include "core/Doc.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace composix::layout {

// How far a colour moves, in words. Delta E 2000: under 2 it holds, under 5
// a slight shift, under 10 noticeable, 10 and up a large one (out of gamut).
enum class Shift { Holds, Slight, Noticeable, Large };
Shift       shift_of(double delta_e);
const char* shift_text(Shift s);   // "Holds", "Slight shift", ...

struct ColorRow {
    enum class Kind { Swatch, Image };
    Kind          kind = Kind::Swatch;
    std::string   id;            // the swatch or asset
    std::string   name;
    std::string   where;         // "4 places" / "unused"; a picture: "page 7"
    std::string   page;          // a picture: the first page it is on (to go there)
    bool          used = true;
    // A swatch: its colour before and after (0xRRGGBB), its CMYK.
    std::uint32_t before = 0, after = 0;
    double        cmyk[4] = {0, 0, 0, 0};
    // Both: how far it moves -- a swatch's own; a picture's average over its
    // pixels, its worst, and the share of pixels shifted noticeably (>= 5).
    double        delta_e = 0, worst = 0, share = 0;
    Shift         shift = Shift::Holds;
    // A picture: the sample, before and after (RGB, w * h * 3), for the eye.
    int           w = 0, h = 0;
    std::vector<std::uint8_t> px_before, px_after;
    bool          readable = true;   // a picture whose pixels couldn't be read
};

struct ColorReportResult {
    bool                  ok = false;
    std::string           err;           // the profile couldn't be read
    std::string           profile_name;
    std::vector<ColorRow> rows;          // swatches (by name), then pictures (page order)
    int worst_count = 0;                 // rows Noticeable or Large
};

// A picture's pixels for the proof: RGB bytes, w * h * 3, any size (the
// report samples what it gets). false: can't be read.
using PixelSource = std::function<bool(const core::Asset&, int& w, int& h, std::vector<std::uint8_t>& rgb)>;

ColorReportResult color_report(const core::Doc& d, const std::string& icc_path, bool perceptual,
                               const PixelSource& pixels);

}  // namespace composix::layout
