#pragma once
// views::paint_canvas_page -- a page as the CANVAS shows it (s104; was
// Canvas::draw_page / draw_frame): shadow, paper, bleed, folds, master items
// dimmed, the page's frames -- fills, text (hidden characters), pictures,
// strokes, frame edges, columns, insets, thread ports, stand-ins. Not the
// page's name, guides, grid, selection or caret (the canvas draws those live).
//
// A free function over an explicit look (no widget, no live document), so it
// runs on any thread: the canvas draws each page into an image at the current
// zoom -- on the page worker from a snapshot, or on the spot -- and paints
// the image. render_canvas_page is that image, proofed when Proof Colours is on.
#include "core/Doc.hpp"
#include "core/Geometry.hpp"
#include "core/Outline.hpp"
#include "layout/Flow.hpp"
#include "layout/ThumbStore.hpp"

#include <cairomm/context.h>

#include <memory>
#include <string>

namespace plate { class Proof; }

namespace composix::views {

// Everything besides the document that decides how a page looks on the canvas.
struct CanvasParams {
    double      s = 1, ox = 0, oy = 0;   // layout point -> screen: x * s + ox
    std::string dir;                     // the project folder (links)
    std::string live_story;              // being edited: markers as signs
    bool        show_hidden = false;
    bool        spine_left = false, spine_right = false;   // a page meets it at the spine
    core::Rect  to_screen(const core::Rect& r) const { return {r.x * s + ox, r.y * s + oy, r.w * s, r.h * s}; }
};

struct CanvasLook : CanvasParams {
    CanvasLook(const CanvasParams& p, const core::Doc& d, layout::FlowCache& f) : CanvasParams(p), doc(d), flows(f) {}
    const core::Doc&   doc;
    layout::FlowCache& flows;
};

void paint_canvas_page(const Cairo::RefPtr<Cairo::Context>& cr, const CanvasLook& look, const core::Placed& p);

// The screen box (whole pixels) the page's drawing can reach: paper, shadow,
// bleed, frames hanging off it, their ports and strokes.
struct PixBox {
    int x = 0, y = 0, w = 0, h = 0;
};
PixBox canvas_page_box(const core::Doc& d, const core::Placed& p, const CanvasParams& v);

// The page drawn into an ARGB image covering `box` (screen pixels; `ds` the
// display's scale), the screen box at (box.x, box.y). Proofed through
// `proof` when given (its opaque pixels: the paper and what's on it).
// The image remembers where it was drawn (canvas_image_place) so a stale one
// can be painted scaled to the current zoom until the new one comes.
layout::Surface render_canvas_page(const core::Doc& d, layout::FlowCache& flows, const CanvasParams& v,
                                   const core::Placed& p, const PixBox& box, double ds, const plate::Proof* proof);

struct ImagePlace {
    double s = 1, ox = 0, oy = 0;   // the look it was drawn with
    int    x = 0, y = 0;            // its top-left, screen pixels
};
const ImagePlace* canvas_image_place(const layout::Surface& img);

}  // namespace composix::views
