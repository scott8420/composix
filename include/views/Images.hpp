#pragma once
// views::images -- the files behind image frames (s009). The GTK side of the
// seam: the core never opens a file; this probes one (for the verbs) and
// decodes it (for the canvas and the tiles), cached by path + file time.
//
// Decoding is gdk-pixbuf (PNG, JPEG, TIFF, GIF, BMP; SVG through its loader).
// What it can't read draws as the stand-in plate. A decoded picture is kept
// at most kMaxPx on its long side -- screen preview, not output.
#include "core/Doc.hpp"

#include <cairomm/context.h>
#include <cairomm/surface.h>

#include <optional>
#include <string>

namespace composix::views::images {

inline constexpr int kMaxPx = 2048;

// What a file is, as the verbs want it; nullopt if it isn't an image we read.
std::optional<core::ImageFile> probe(const std::string& path);

// The decoded picture as a cairo surface, ready to paint (null: missing /
// unreadable). Re-decoded when the file's time changes. `w` / `h` get the
// file's own size (pixels; a vector's own size).
Cairo::RefPtr<Cairo::ImageSurface> get(const std::string& path, std::int64_t stamp,
                                       int* w = nullptr, int* h = nullptr);

// Paint a picture stretched into x, y, w, h (the caller clips).
void paint(const Cairo::RefPtr<Cairo::Context>& cr, const Cairo::RefPtr<Cairo::ImageSurface>& pic,
           double x, double y, double w, double h, double alpha = 1.0);

// s013: the picture for PRINT -- a new cairo surface reference at the file's
// full resolution (a vector rendered for 300 ppi at want_w x want_h points);
// a JPEG carries its original bytes, so the PDF embeds it untouched. nullptr
// when it can't be read.
cairo_surface_t* print_surface(const std::string& path, double want_w, double want_h);

// The file patterns the Place dialog offers.
const std::vector<std::string>& patterns();

}  // namespace composix::views::images
