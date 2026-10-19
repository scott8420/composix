#pragma once
// layout::psd -- a Photoshop file's merged picture (s185, G5b). gdk-pixbuf
// has no PSD loader, so Composix reads the composite image Photoshop (and
// Krita, GIMP...) writes at the end of the file: raw, PackBits (RLE) or ZIP
// (with or without prediction); 8 or 16 bits; RGB, Gray, CMYK, Lab, indexed
// or bitmap. The layers aren't read -- only the flag saying the first extra
// channel is the merged picture's transparency. PSB (large documents) and
// 32-bit files aren't read.
//
// In the layout library (no GTK) so the selftest exercises it; the app hands
// it the file's bytes (views::images). CMYK and Lab come out as RGB for the
// screen and the plain PDF -- simple ink arithmetic, not a profile.
#include <cstdint>
#include <string>
#include <vector>

namespace composix::layout {

struct PsdInfo {
    bool ok = false;
    int  w = 0, h = 0;
    int  channels = 0, depth = 0;
    int  mode = 0;            // 0 bitmap, 1 gray, 2 indexed, 3 RGB, 4 CMYK, 7 multichannel, 8 duotone, 9 Lab
};
// From the header (the first 26 bytes are enough).
PsdInfo psd_info(const std::string& head);

// The merged picture, 0xAARRGGBB premultiplied (as cairo's ARGB32), rows
// packed. `max_side` > 0: taken at every k-th pixel so the long side is at
// most that (the screen's copy). Empty when it can't be read; `why` says.
// s187: `cmyk` (a CMYK file only): its own separations as ink, 4 bytes a
// pixel (0 none .. 255 full), the same w x h -- for print.
std::vector<std::uint32_t> psd_decode(const std::string& bytes, int& w, int& h, int max_side = 0,
                                      std::string* why = nullptr, std::vector<std::uint8_t>* cmyk = nullptr);

}  // namespace composix::layout
