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
//   * s018: frames with a text wrap push lines aside. Where a column meets
//     one, each line is set on its own at the width left free (both sides of
//     an Around frame, left to right; nothing beside a Jump frame); a band
//     with no room drops to below what covers it.
#include "core/Doc.hpp"

#include <cairo.h>
#include <pango/pango.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    // s018: a line set on its own (text wrap): its width, from x. 0 = the
    // column's, set with the rest of its paragraph in the column.
    double w     = 0;
    // s029: a drop cap's line -- its font size (0: an ordinary line) and
    // its baseline (frame coordinates: the Nth line's).
    double cap   = 0;
    double base  = 0;
    // s038: the line ends inside a word, at a hyphenation point: a hyphen
    // is drawn after it (not in the text). Always a line set on its own.
    bool   hyphen = false;
};

// s109: a footnote set at the foot of a column: the k-th note of paragraph
// `para`, its number, and its box (frame coordinates).
struct NotePlace {
    int    para = 0, k = 0, number = 0, col = 0;
    double x = 0, y = 0, w = 0, h = 0;
    // s117: a piece of the note -- its words [from, to) (to -1: to the end).
    // `cont`: run on from a column before (no number; a full-width rule).
    int    from = 0, to = -1;
    bool   cont = false;
    // s120: an endnote -- set in the text's column after the story's end, not
    // at the foot. k -1: the endnotes' title.
    bool   end = false;
};

// s117: a note running on -- the rest of paragraph `para`'s k-th note, from
// byte `from` of its words, still to be set (at the next column's foot).
struct NoteRun {
    int para = 0, k = 0, number = 0, from = 0;
    bool operator==(const NoteRun&) const = default;
};

struct FrameText {
    std::string       frame;
    std::vector<Line> lines;
    std::vector<NotePlace> notes;   // s109: in column order, top to bottom
    // s105: where the flow stood when it came to this frame -- the paragraph
    // and byte (from_p -1: the story ended before it) and the page of the
    // last frame that took lines. A re-flow starts at a frame from these.
    int               from_p = -1, from_b = 0;
    std::string       page_before;
    int               note_from = 0;   // s111: notes numbered on its page (section) before it, when numbering restarts
    std::vector<NoteRun> carry_in;     // s117: notes running on into this frame
    bool              ending_in = false;   // s120: ... as endnotes (the story's text ended before it)
    int               end_from_in = 0;     // s122: notes from this paragraph on not yet set as endnotes
};

struct StoryFlow {
    std::vector<FrameText> frames;     // thread order
    bool                   overset = false;
    int                    lines   = 0;
    int                    filled  = 0;   // s105: frames set this time (the rest kept from the last flow)
    // s106: set only so far (a re-flow's budget ran out before the lines
    // resettled): frames from `tail_from` on are the last flow's, moved --
    // what was there, until the whole is worked out (FlowCache::complete).
    bool                   provisional = false;
    int                    tail_from   = -1;
};

// The story's frames in thread order.
std::vector<std::string> thread_frames(const core::Doc& d, const std::string& story);

StoryFlow flow_story(const core::Doc& d, const std::string& story);
// s102: the same, telling `tick` after each frame (done, of total).
using FlowTick = std::function<void(int done, int total)>;
StoryFlow flow_story(const core::Doc& d, const std::string& story, const FlowTick& tick);
// s105: the story re-flowed after an edit to its text: `old_flow` is the
// flow of `old_text` with everything else as now (frames, styles, pages).
// Frames up to the one where the first changed paragraph's predecessor
// starts are kept; from there it is set again until the flow comes to a
// frame just as it did before (past the changed paragraphs) -- the rest is
// the old flow, moved by the paragraphs added or taken away. The same
// lines a whole flow would give.
StoryFlow reflow_story(const core::Doc& d, const std::string& story, const core::Story& old_text,
                       const StoryFlow& old_flow, int budget = 0);
// s106: `budget` > 0 -- set at most that many frames; if the lines haven't
// resettled by then the flow is provisional (see StoryFlow).
// s048: auto-size frames -- each (the last of its thread, one column)
// measured with room to spare and given the height its text needs (top
// fixed; at least 1 pt). What changes, for DocJournal::set_settle.
std::vector<core::DocJournal::Settled> fit_frames(const core::Doc& d);
// s023: flow `text` (a version of the story's words -- markers filled in)
// through that story's thread.
StoryFlow flow_text(const core::Doc& d, const std::string& story, const core::Story& text);

// s023: a story as a page shows it -- its markers (page number, section
// name, next / previous page) filled in, and flowed. `on_page`: the page a
// MASTER's frame is being drawn on ("" -- the story sits in page frames:
// each marker takes the page its line is set on). `any` false: no
// markers; use the plain flow.
struct Resolved {
    bool        any = false;
    core::Story text;
    StoryFlow   flow;
};
Resolved resolve_story(const core::Doc& d, const std::string& story, const std::string& on_page);
// The value a marker takes on `page` (k as core::marker_at). `frame`: the
// frame it sits in, for next / previous (its thread's neighbours).
std::string marker_value(const core::Doc& d, int k, const std::string& page, const std::string& frame);

// Draw what `flow` put in `frame`, with the frame's top-left at (ox, oy) in the
// cairo context's current user space (points). The caller sets the scale.
// `ink`: the text's grey (screen: near-black; print, s013: 0 = pure black).
// `text`: the words `flow` was made from when not the story's own (s023).
void draw_frame_text(cairo_t* cr, const core::Doc& d, const std::string& story, const StoryFlow& flow,
                     const std::string& frame, double ox, double oy, double ink = 0.08,
                     const core::Story* text = nullptr);

// s015: the lines `flow` put in `frame`, exactly as drawn: the Pango line, its
// left and baseline (frame coordinates), the paragraph, and the byte in it
// where the line's layout starts (a run's layout byte + from = a byte in the
// paragraph). The print exporter sets these through Plate.
// s044: the paragraph rules `flow` puts in `frame` -- rectangles in frame
// coordinates, with their swatch ("" black) and tint. Drawn under the text.
struct RuleMark {
    core::Rect  r;
    std::string color;
    double      tint = 1;
};
std::vector<RuleMark> paragraph_rules(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                      const std::string& frame, const core::Story* text = nullptr);
// s055: a text frame's column rules -- one down the middle of each gutter,
// the text area's height less the rule's inset (frame coordinates).
std::vector<RuleMark> column_rules(const core::Frame& f);

// s059: hidden characters -- where the marks that steer the text sit on a
// frame's lines (frame coordinates): spaces, tabs, paragraph ends, a break
// typed before the next paragraph (its kind in `brk`), the story's end.
// For the canvas only: never printed or exported.
struct HiddenMark {
    enum class Kind { Space, Tab, Para, Break, End, Hyphen };   // s065: Hyphen -- a typed (discretionary) one
    Kind   kind = Kind::Para;
    double x0 = 0, x1 = 0;   // the character's span (a paragraph end: x0 only)
    double base = 0, h = 0;  // its line's baseline and leading
    int    brk = 0;          // Break: 1 column, 2 frame, 3 page, 4 right-hand page
    double edge = 0;         // s098: Break -- the column's right edge (frame coords), where its rule ends
};
// s098: a break's name, as the canvas labels it ("Page break").
const char* break_name(int brk);
std::vector<HiddenMark> hidden_marks(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                     const std::string& frame, const core::Story* text = nullptr);

struct LineVisit {
    PangoLayoutLine* line = nullptr;
    double           x = 0, baseline = 0;
    int              para = 0, from = 0;   // s026: from -1 = a tab's leader (dots), not the paragraph's words; s045: -2 a list's marker
};
void visit_lines(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame,
                 const std::function<void(const LineVisit&)>& f, const core::Story* text = nullptr);

// ── editing geometry (s010) ─────────────────────────────────────────────────
// All in FRAME coordinates (points), from the same layouts the drawing uses,
// so the caret sits exactly where the glyphs are.
struct Caret {
    bool        ok = false;   // false: the place is overset (in no frame)
    std::string frame;
    double      x = 0, y = 0, h = 0;   // the caret: x, top, height
};
Caret caret_at(const core::Doc& d, const std::string& story, const StoryFlow& flow, core::TextPos p);
// s105: the same frame, top and height, without the x -- read off the flow's
// lines, no text laid out (the contents check asks it per heading).
Caret caret_line(const core::Doc& d, const std::string& story, const StoryFlow& flow, core::TextPos p);
// s119: the number paragraph p's k-th note was given where it's set (by
// page / section numbering, the flow's); 0: not set (overset, not placed).
int laid_note_number(const StoryFlow& flow, int p, int k);
// s114: the footnote set at (x, y) in a frame (frame coordinates, as
// hit_text); nullptr: none there.
const NotePlace* note_hit(const StoryFlow& flow, const std::string& frame, double x, double y);
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
class FlowCache;
// s102: every story of `d` flowed into `cache`, `tick` told across all
// their frames (done, of total) -- a long book's layout with a progress bar.
void flow_all(const core::Doc& d, FlowCache& cache, const std::function<void(int done, int total)>& tick);

class FlowCache {
public:
    const StoryFlow& get(const core::Doc& d, const std::string& story);

    // s023: a story with its markers filled in for a page (see resolve_story).
    const Resolved& resolved(const core::Doc& d, const std::string& story, const std::string& on_page);

    // s102: a flow worked out elsewhere (a worker) for this revision of `d`.
    void put(const core::Doc& d, const std::string& story, StoryFlow f);
    // s102: flows made from a copy with the same records as `d` (loaded on a
    // worker, then copied in): keep them, now as `d`'s.
    void rebase(const core::Doc& d) { m_rev = d.revision(); }
    // s103: what it holds was laid out for this revision of `d`.
    bool fresh_for(const core::Doc& d) const { return m_rev == d.revision(); }

    // s105: how the last get() was had -- for tests and the log.
    enum class How { Kept, Whole, Reflowed };
    How last_how() const { return m_how; }

    // s106: re-flows set at most `frames` frames (0: no limit, the default);
    // past that a flow is provisional until complete() hands in the whole.
    void set_reflow_budget(int frames) { m_budget = frames; }
    // The provisional flows, each with what the whole is worked out from:
    // the last whole flow of the story and the text it was made from.
    struct Unfinished {
        std::string story;
        StoryFlow   basis;
        core::Story basis_text;
    };
    std::vector<Unfinished> unfinished() const;
    bool any_unfinished() const;
    // The whole flow of `story`, worked out (on a worker) for revision `rev`
    // of `d` from `text`. Taken as the flow when `d` is still at `rev`; else,
    // if only text changed since, as the basis the next re-flow starts from.
    void complete(const core::Doc& d, const std::string& story, std::uint64_t rev, StoryFlow flow, core::Story text);
    // Work out every provisional flow here and now (before anything that
    // must not read a stand-in: rebuilding a contents list, the export sheet).
    void finish_now(const core::Doc& d);

private:
    void check(const core::Doc& d);
    struct Entry {
        StoryFlow   flow;
        core::Story text;   // s105: what it was flowed from -- an edit re-flows from it
        // s106: a provisional flow's basis -- the last whole flow and its text.
        std::shared_ptr<const StoryFlow>   basis;
        std::shared_ptr<const core::Story> basis_text;
    };
    std::uint64_t                    m_rev = ~0ull;
    std::map<std::string, Entry>     m_flows;
    std::map<std::string, Entry>     m_prev;       // s105: flows of stories edited since -- re-flowed from
    std::map<std::string, Resolved>  m_resolved;   // story "|" page
    How                              m_how = How::Whole;
    int                              m_budget = 0;
};

}  // namespace composix::layout
