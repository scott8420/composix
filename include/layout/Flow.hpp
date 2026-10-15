#pragma once
// layout::Flow -- real text, flowed through threaded frames (s007).
//
// Its own library (composix_layout): Pango + cairo, NO gtk -- so the flow is
// measured and tested headless, like the core, and the canvas only draws it.
//
// The rules:
//   * Measured at 72 dpi with hinting OFF, so one point is one unit and line
//     breaks do not change with zoom (the canvas scales the drawing, never the
//     measurement).
//   * A story's paragraphs flow through its frames in thread order (the head
//     is the frame nobody points to; `next` from there).
//   * Paragraph by paragraph: each is laid out at the width of the frame it is
//     in; a paragraph that crosses into the next frame is re-broken at that
//     frame's width from where it left off.
//   * Each line advances by its style's leading; the baseline sits centred in
//     the leading.
//   * Text left over after the last frame is OVERSET.
//   * s012: a frame is filled column by column (each at its own width); a
//     BALANCED frame where the story ends gets the shortest column height
//     that still holds the rest, so its columns come out even.
#include "core/Doc.hpp"

#include <cairo.h>
#include <pango/pango.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace composix::layout {

struct Line {
    int    para  = 0;    // paragraph index in the story
    int    start = 0;    // byte range in that paragraph's text
    int    end   = 0;
    double y     = 0;    // top of the line box, frame coordinates (points)
    double h     = 0;    // its leading (s010: the caret's height)
    int    col   = 0;    // s012: the column it is in
    double x     = 0;    // that column's left edge, frame coordinates
};

struct FrameText {
    std::string       frame;
    std::vector<Line> lines;
};

struct StoryFlow {
    std::vector<FrameText> frames;     // thread order
    bool                   overset = false;
    int                    lines   = 0;
};

// The story's frames in thread order.
std::vector<std::string> thread_frames(const core::Doc& d, const std::string& story);

StoryFlow flow_story(const core::Doc& d, const std::string& story);

// Draw what `flow` put in `frame`, with the frame's top-left at (ox, oy) in the
// cairo context's current user space (points). The caller sets the scale.
// `ink`: the text's grey (screen: near-black; print, s013: 0 = pure black).
void draw_frame_text(cairo_t* cr, const core::Doc& d, const std::string& story, const StoryFlow& flow,
                     const std::string& frame, double ox, double oy, double ink = 0.08);

// s015: the lines `flow` put in `frame`, exactly as drawn: the Pango line, its
// left and baseline (frame coordinates), the paragraph, and the byte in it
// where the line's layout starts (a run's layout byte + from = a byte in the
// paragraph). The print exporter sets these through Plate.
struct LineVisit {
    PangoLayoutLine* line = nullptr;
    double           x = 0, baseline = 0;
    int              para = 0, from = 0;
};
void visit_lines(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame,
                 const std::function<void(const LineVisit&)>& f);

// ── editing geometry (s010) ─────────────────────────────────────────────────
// All in FRAME coordinates (points), from the same layouts the drawing uses,
// so the caret sits exactly where the glyphs are.
struct Caret {
    bool        ok = false;   // false: the place is overset (in no frame)
    std::string frame;
    double      x = 0, y = 0, h = 0;   // the caret: x, top, height
};
Caret caret_at(const core::Doc& d, const std::string& story, const StoryFlow& flow, core::TextPos p);
// The place nearest (x, y) in a frame of the story (clamped to its lines).
core::TextPos hit_text(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                       const std::string& frame, double x, double y);
// Up / down one line (dir -1 / +1) through the whole thread, keeping to
// `goal_x` (the x the caret had when the run of up / downs began).
core::TextPos line_move(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                        core::TextPos p, int dir, double goal_x);
// The start / end of the line the place is on.
core::TextPos line_edge(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                        core::TextPos p, bool end);
// The selection [a, b) as rectangles in one frame.
std::vector<core::Rect> selection_rects(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                        const std::string& frame, core::TextPos a, core::TextPos b);

// The installed font families (Pango / fontconfig), sorted, with the generic
// Serif / Sans / Monospace first. Read once.
const std::vector<std::string>& font_families();

// Flows, recomputed only when the document changes (Doc::revision()).
class FlowCache {
public:
    const StoryFlow& get(const core::Doc& d, const std::string& story);

private:
    std::uint64_t                    m_rev = ~0ull;
    std::map<std::string, StoryFlow> m_flows;
};

}  // namespace composix::layout
