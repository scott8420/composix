#pragma once
// core::Doc -- THE document (s004). GTK-free; the selftest drives it headless.
//
// One store, every record by id. The journal (core/Undo.hpp) asks only
// get(id) / put(id, value), so a record is a std::variant of the kinds below
// and the whole document is one map. Every user choice is a verb in
// core::doc:: that goes through Journal::run -- the ONE DOOR -- and is listed
// in doc::verbs(), which the selftest walks (the gate: a write with the door
// shut is a stray and fails the build's tests).
//
// The shape, after REQUIREMENTS:
//   Settings    one record ("doc"): title, trim, bleed, facing pages
//   Section     starts AT a page (InDesign's model): the run of pages from its
//               start page to the next section's. Numbering, start-at, side.
//   Page        place (pos), its OWN size -- a foldout is a wide page with
//               fold positions -- and its master
//   Master      a page template: prefix + name, size, columns, based-on
//   Frame       on a page or master: geometry in points, text / image / shape;
//               a text frame names its story and the NEXT frame in the thread
//   Story       text apart from frames -- paragraphs, each tagged with a style
//               (semantic: what EPUB reflow and TOC generation need)
//   Style       paragraph / character
//   Swatch      process CMYK / RGB / gray / spot
//   Asset       an image: linked (a path) or embedded (in the project folder)
//   GenList     a generated list: TOC / bibliography / index, from styles
//
// Units: POINTS (1/72 in) everywhere in the model. Inches are a display unit.
// A record's PLACE (pos, owner) is in its value, so undoing a delete puts it
// back exactly where it was, under the same id.
#include "core/Geometry.hpp"
#include "core/Undo.hpp"
#include "core/Settings.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <chrono>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace composix::core {

inline constexpr double kPt = 72.0;   // points per inch

enum class Numbering { Arabic, RomanLower, RomanUpper, AlphaLower, AlphaUpper };
enum class Side      { Any, Right, Left };
// s154 (Scott: as InDesign / Affinity): where the rulers' zero is -- the
// spread's top-left (each spread its own), each page's top-left, or the
// spine (left of it negative; fixed -- not dragged).
enum class RulerOrigin { Spread, Page, Spine };
enum class FrameKind { Text, Image, Shape };
// s197 (Scott: rect, oval, line, and a pen for every other path): a shape
// frame's outline. A line is a Path of two points, open.
enum class ShapeKind { Rect, Oval, Path };
enum class StyleKind { Paragraph, Character };
enum class Align     { Left, Center, Right, Justify };
enum class ColorModel{ CMYK, RGB, Gray, Spot };
enum class ListKind  { Contents, Bibliography, Index };
enum class ImageFit  { Fill, Fit, Custom };   // s009: cover the frame (cropped) / show it whole; s035: placed by hand
// s018: how a frame pushes text in OTHER frames aside. Around: text runs
// down both sides of it; Jump: text skips the band it covers, no lines
// beside it.
enum class Wrap      { None, Around, Jump, Shape };   // s173: Shape -- an image's own outline (its traced shape)
enum class VAlign    { Top, Center, Bottom, Justify };   // s042: a text frame's text, up and down
// s043: where a paragraph (in this style) may start.
enum class ParaStart { Anywhere, NextColumn, NextFrame, NextPage, NextRightPage, NextLeftPage };   // s252: left
// s252: a start that takes a new page (any, right-hand, left-hand).
inline bool page_start(ParaStart s) { return s == ParaStart::NextPage || s == ParaStart::NextRightPage || s == ParaStart::NextLeftPage; }

// Four distances, one per side (s018: a frame's text-wrap offset).
struct Sides {
    double top = 0, right = 0, bottom = 0, left = 0;
    bool operator==(const Sides&) const = default;
};

// s032: how the document exports, kept with it. The output profile by its
// NAME (its description) -- found again among the profiles on whatever
// computer opens the project, or reported missing.
struct ExportPrefs {
    bool        print = true;     // Plate (print) or cairo (screen)
    bool        bleed = true, marks = false;
    std::string profile;          // "" = none (device CMYK)
    bool        convert = false;
    int         standard = 0;     // 0 none, 1 PDF/X-1a:2003, 2 PDF/X-4
    std::string range;            // s077: the pages as typed ("" = all)
    bool        bookmarks = true; // s077: the contents' headings as the PDF's outline
    std::vector<std::string> mark_lists;   // s082: the lists the outline comes from (ids; empty = the first Contents)
    bool        spreads = false;  // s084: facing pages as one sheet each
    bool        perceptual = false;   // s085: the conversion's intent (else relative colorimetric + BPC)
    bool        rebuild = true;   // s089: rebuild out-of-date contents lists before writing
    bool operator==(const ExportPrefs&) const = default;
};

// s073: the unit lengths are shown in (rulers, the inspector, sizes).
// Typing any unit still works everywhere; a bare number is in this one.
enum class Units { Inches, Millimetres, Picas, Points };

// s078: a Find / Change query kept with the document, by name. Styles by
// id ("" = any / leave); mode 0 text, 1 wildcards, 2 regex; format 0 any,
// 1 bold, 2 italic, 3 both.
struct SavedQuery {
    std::string name, what, with;
    bool        match_case = false, whole_word = false;
    int         mode = 0, format = 0;
    std::string in_style, apply_style;
    bool operator==(const SavedQuery&) const = default;
};

// s111: how footnotes are numbered -- counting on through the story, or
// starting again on each page / in each section; and how a number looks.
enum class NoteRestart { Story, Page, Section, Chapter };   // s124: Chapter -- again after each paragraph that starts a new page
enum class NoteStyle   { Arabic, RomanLower, AlphaLower, Symbols };
// s120: where notes are set -- each column's foot, or all of them after the
// story's last paragraph (endnotes, numbered through the story).
enum class NoteAt { Foot, StoryEnd, ChapterEnd };   // s122: ChapterEnd -- before each paragraph that starts a new page, and at the story's end

struct Settings {
    std::string title  = "Untitled";
    double      trim_w = 6 * kPt, trim_h = 9 * kPt;
    double      bleed  = 0.125 * kPt;
    bool        facing = true;
    // s069: a frame's text wrap pushes only text frames beneath it in the
    // stacking order (masters below their pages); off: every text frame.
    bool        wrap_beneath = false;
    Units       units = Units::Inches;   // s073
    std::vector<SavedQuery> queries;     // s078: Find / Change, saved
    ExportPrefs exp;                     // s032
    NoteRestart note_restart = NoteRestart::Story;   // s111
    NoteStyle   note_style   = NoteStyle::Arabic;
    // s117: the most of a column its footnotes may take, percent (100: all
    // of it). A note that doesn't fit runs on to the next column's foot.
    int         note_area    = 100;
    // s118: how the foot looks -- the rule over a column's notes (percent of
    // the column; 0 none) and its weight, the space between notes, and a
    // line set under a note that runs on ("" none, e.g. "(continued)").
    int         note_rule    = 33;
    double      note_rule_w  = 0.5;
    double      note_space   = 0;
    std::string note_cont;
    NoteAt      note_at    = NoteAt::Foot;   // s120
    // s126: the number before a note's words -- "1", "1.", "1)", or raised.
    enum class NoteNum { Plain, Period, Paren, Raised };
    NoteNum     note_num   = NoteNum::Plain;
    std::string note_title = "Notes";        // s120: over the endnotes ("" none)
    std::string author;                      // s239 (H1): the document's author (a header's kMarkAuthor)
    // s240 (H2): the paragraph styles running heads take their words from --
    // the chapter's name (kMarkChapter), its subtitle (kMarkSubhead); "" =
    // found by name ("Heading 1"; "Subtitle", else "Heading 2").
    std::string run_head, run_sub;
    // s255: text reflow -- a story run past its last page frame adds pages
    // (its master, a frame in the margins, threaded on), as InDesign's
    // Smart Text Reflow.
    bool        reflow = false;
    // s256: and takes away the pages it added that the text no longer
    // reaches (InDesign's "Delete Empty Pages") -- a trailing page holding
    // only the thread's empty margin frame.
    bool        reflow_trim = false;
    // s154: the rulers' origin, and the zero point dragged from it (points,
    // from where the origin puts it; Spine: unused).
    RulerOrigin ruler_origin = RulerOrigin::Spread;
    double      zero_x = 0, zero_y = 0;
    // s307: the language its words are checked in ("en_US"; "" = the app's).
    std::string language;
    bool operator==(const Settings&) const = default;
};

struct Section {
    std::string name;
    std::string start_page;              // the page this section starts at
    Numbering   numbering = Numbering::Arabic;
    int         start_at  = 1;
    Side        starts    = Side::Any;   // where its first page wants to fall
    std::string master;                  // default master for new pages in it
    bool operator==(const Section&) const = default;
};

// Page margins (s014), points. Facing pages: inside = by the spine.
struct Margins {
    double top = 54, bottom = 54, inside = 54, outside = 54;
    bool operator==(const Margins&) const = default;
};

struct Page {
    double              pos = 0;         // order (ascending)
    double              w = 6 * kPt, h = 9 * kPt;
    std::vector<double> folds;           // x of each fold, from the left; empty = not a foldout
    std::string         master;          // "" = none
    // s062: the page's own margins, over its master's (none: the master's).
    std::optional<Margins> margins;
    bool operator==(const Page&) const = default;
};

struct Master {
    std::string         prefix;          // "B"
    std::string         name;            // "Body"
    double              pos = 0;
    double              w = 6 * kPt, h = 9 * kPt;
    std::vector<double> folds;
    int                 columns = 1;
    std::string         based_on;        // another master's id, or ""
    Margins             margins;         // s014
    double              gutter = 12;     // s014: between the page's columns
    double              grid = 0;        // s036: baseline grid increment (pt) from the top margin; 0 = none
    // s147 (Scott: "each master should have all the settings"): its pages'
    // bleed and whether they face (a cover single, the body facing). Its
    // size is w / h above. A file from before: the document's, on loading.
    double              bleed = 0.125 * kPt;
    bool                facing = true;
    bool operator==(const Master&) const = default;
};

// s055: a hairline between a text frame's columns -- `weight` 0 = none --
// in a swatch's ink ("" black), held `inset` points in from the text
// area's top and bottom.
struct ColumnRule {
    double      weight = 0;
    std::string color;
    double      tint = 1;
    double      inset = 0;
    bool operator==(const ColumnRule&) const = default;
};

// s197: one anchor of a path and the handles that bend the curves either
// side of it -- all in the FRAME's box, 0..1 across and down, so resizing
// the frame scales its path. A handle sitting on its anchor: that side is
// straight.
struct PathPoint {
    double x = 0, y = 0;     // the anchor
    double ix = 0, iy = 0;   // the handle in (the curve from the previous point)
    double ox = 0, oy = 0;   // the handle out (the curve to the next)
    bool   smooth = false;   // the handles kept in line (a pen drag; Direct keeps them so)
    bool operator==(const PathPoint&) const = default;
};

struct Frame {
    std::string owner;                   // page or master id
    FrameKind   kind = FrameKind::Text;
    double      x = 0, y = 0, w = 0, h = 0, rot = 0;   // s228: rot -- degrees, counter-clockwise (as InDesign), about its centre
    double      z = 0;
    std::string story;                   // text
    std::string next;                    // text: the next frame in the thread
    std::string asset;                   // image
    ImageFit    fit = ImageFit::Fill;    // image: how the picture sits in the frame (s009)
    double      pic_x = 0, pic_y = 0, pic_w = 0;   // s035: Custom -- the picture's left, top (from the frame's) and width, points
    int         columns = 1;             // text (s012): columns inside the frame
    double      gutter  = 12;            // text: points between them
    bool        balance = false;         // text: even out the columns where the story ends
    std::vector<double> widths;          // text: each column's relative weight (empty = equal)
    std::string fill, stroke;            // s016: swatch ids ("" = none) -- a frame's background / outline
    double      stroke_w = 1;            // points
    double      fill_tint = 1, stroke_tint = 1;   // s021: of the swatch, 0.05..1 (a spot prints as a tint of its ink)
    double      opacity = 1;             // s021: the frame's fill, content and stroke, 0..1 (below 1: transparency)
    // s025: a master's item: which pages it shows on (Any, or only Left /
    // Right ones -- a facing master's outside folios). A page's item: the
    // master item it OVERRIDES on that page ("" = its own), hiding that one.
    Side        side = Side::Any;
    std::string overrides;
    Wrap        wrap = Wrap::None;       // s018: pushes text in other frames aside
    Sides       wrap_off;                // points beyond the frame's edges
    Sides       inset;                   // s040: text -- held in from the frame's edges (points)
    VAlign      valign = VAlign::Top;    // s042: text -- where it sits in the frame's height
    bool        autosize = false;        // s048: text -- its height follows its text (top fixed; the
                                         // last frame of its thread, one column)
    // s254: what follows the text -- 0 height, 1 width (each paragraph one
    // line; its alignment holds the left / centre / right), 2 both; the edge
    // that stays put as the height changes -- 0 top, 1 centre, 2 bottom;
    // and a height it doesn't go under (0: none).
    int         autosize_dims = 0;
    int         autosize_from = 0;
    double      autosize_min = 0;
    ColumnRule  col_rule;                // s055: text -- a rule between its columns
    bool        ignore_wrap = false;     // s069: text -- other frames' wrap doesn't push this text
    int         role = 0;                // s275: a master's text frame -- 1 its header, 2 its footer (0 neither)
    // s155 (Scott): a facing master's item ACROSS THE SPINE -- one item, its
    // x from the left page's left edge; shown on both pages, each its part
    // (cut at the spine). `side` stays Any.
    bool        across = false;
    // s197: a shape frame's outline (kind Shape only -- text and image
    // frames stay rectangles). Path: its points, in the frame's box.
    ShapeKind   shape = ShapeKind::Rect;
    std::vector<PathPoint> path;
    bool        open = false;            // Path: not closed (a line, an open curve)
    // s204: the stroke's style -- its ends (0 butt, 1 round, 2 square), its
    // corners (0 miter, 1 round, 2 bevel), dashes (0 solid, 1 dashed, 2
    // dotted), and an open path's ends (0 none, 1 arrow, 2 circle, 3 bar).
    int         cap = 0, join = 0, dash = 0;
    int         end_a = 0, end_b = 0;    // its first point's end, its last's
    double      radius = 0;              // s205: rounded corners, points (any frame drawn as its box)
    // s211: a compound path -- where each further subpath starts in `path`
    // (ascending, 0 < k < size). Filled even-odd: a subpath inside another
    // is a hole.
    std::vector<int> subs;
    // s216: a text frame's words set ALONG its outline (an open path, a
    // line, an oval's edge...) -- not inside it: one line, its baseline on
    // the path, starting `path_from` points along it.
    bool        on_path = false;
    double      path_from = 0;
    // s218: the words on the path's other side (the path run the other way),
    // and where the line sits on it: 0 its baseline, 1 its middle, 2 its
    // top (the words hang below), 3 its bottom.
    bool        path_flip = false;
    int         path_align = 0;
    // s221: where the words stop, points along the path (as path_from is
    // measured); 0 = the path's end. Past it the text is overset.
    double      path_to = 0;
    // s223: how each glyph sits on the path (InDesign's effects): 0 Rainbow
    // (turned with it), 1 Skew (upright, slanted along it), 2 3D Ribbon
    // (level, leaning with it), 3 Stair Step (upright and level), s224:
    // 4 Gravity (its upright through the path's centre -- fanned).
    int         path_effect = 0;
    // s225: room round turns toward the words, points (about their height
    // at the turn): 0 none.
    double      path_spacing = 0;
    // s227 (G3): a picture frame's clipping path -- "" the picture's own
    // (the one Photoshop marked), "none" none, else one of its paths by
    // name (Asset::path_names).
    std::string clip_path;
    // s228: with `rot`, how the frame is turned -- `shear` degrees (its top
    // slid right, positive), `flip` mirrored left to right; all about its
    // box's centre: owner = C + R(rot) . Shear . Flip . (p - C). x, y, w, h
    // stay its box before the turn (frame_xf, frame_bounds).
    double      shear = 0;
    bool        flip = false;
    // s232: scaled WITH its contents -- the box drawn sx wide, sy tall
    // (1 = as set): text, stroke and picture with it. Innermost in the turn:
    // owner = C + R . Shear . Flip . Scale . (p - C).
    double      sx = 1, sy = 1;
    // (last, so the aggregate initialisers that fill the front keep working)
    std::string group;                   // s300: frames grouped share an id ("group.N"; "" none) -- picked and moved as one
    bool        locked = false;          // s301: not picked on the canvas, so not moved or changed by hand
    std::string layer;                   // s305: its layer's id ("" the bottom layer)
    bool operator==(const Frame&) const = default;
};

// A character style over bytes [start, end) of a paragraph (s011). A
// paragraph's runs are sorted, never overlap, never empty; text outside any
// run is in the paragraph style alone.
// s030: formatting set on words directly, over their styles: -1 / 0 /
// "" = leave it to the styles.
// s264: OpenType features (InDesign's Character panel menu). 0 / false: the
// font's default (a character style's: its paragraph's). `liga` 1 off;
// `kern` 1 none (metrics off); `figures` 1 lining, 2 oldstyle; `fig_space` 1
// proportional, 2 tabular; `sset` a stylistic set 1 - 20 (ss01 ...).
struct OtFeatures {
    int  liga = 0, kern = 0, figures = 0, fig_space = 0, sset = 0;
    bool dlig = false, frac = false, ordn = false, swsh = false;
    bool any() const { return liga || kern || figures || fig_space || sset || dlig || frac || ordn || swsh; }
    bool operator==(const OtFeatures&) const = default;
};
// s264: `over` on `under`: what `over` sets (non-default) wins.
OtFeatures ot_over(const OtFeatures& under, const OtFeatures& over);
// s264: as HarfBuzz feature settings ("liga=0,onum=1"); `base`: what's set
// under it -- what it set and this doesn't is set back ("liga=1"). "" none.
std::string ot_settings(const OtFeatures& f, const OtFeatures& base = {});
// s264: one feature set from an edit key (ot_liga, ot_kern, ot_fig, ot_figsp,
// ot_sset, ot_dlig, ot_frac, ot_ordn, ot_swsh) and its choice.
void ot_edit(OtFeatures& f, const std::string& key, const std::string& choice);

struct Local {
    int         bold = -1, italic = -1;   // -1 the styles', 0 off, 1 on
    double      size = 0;                 // points
    std::string color;                    // a swatch id
    int         no_break = -1;            // s159: -1 the styles', 0 may break, 1 kept on one line
    // s260: character attributes (unset / -1: the styles') -- see Style.
    std::optional<double> tracking, shift;
    int         position = -1, caps = -1, underline = -1, strike = -1;
    std::optional<OtFeatures> ot;         // s264: the words' own features (all of them, over the styles')
    std::optional<double> kern;           // s265: manual kerning after this character (1/1000 em)
    // s303: a hyperlink over these words -- a web address / mailto: (out of
    // the document), or "#" and an anchor's id (s304: to that place). "" none.
    std::string link;
    bool empty() const {
        return bold < 0 && italic < 0 && size <= 0 && color.empty() && no_break < 0 && !tracking && !shift && position < 0 &&
               caps < 0 && underline < 0 && strike < 0 && !ot && !kern && link.empty();
    }
    bool operator==(const Local&) const = default;
};

struct Run {
    int         start = 0, end = 0;
    std::string style;                   // a character style id ("" with local formatting only)
    Local       local;                   // s030
    bool operator==(const Run&) const = default;
};

// s026: a tab stop -- where (points from the text's left edge), how the
// text after it lines up, and what fills the gap ("" none, "." dots).
enum class TabAlign { Left, Center, Right, Decimal };
struct TabStop {
    double      pos = 0;
    TabAlign    align = TabAlign::Left;
    std::string leader;
    bool operator==(const TabStop&) const = default;
};

// s093: a paragraph's own settings over its style's -- each unset (the
// style's) or set. Kept when the style changes; cleared one by one.
struct ParaLocal {
    std::optional<Align>  align;
    std::optional<double> left_indent, right_indent, first_indent, space_before, space_after;
    std::optional<int>    keep_next;              // s094; s257: lines (0 off, 1 - 5)
    std::optional<bool>   hyphenate;              // s094
    std::optional<int>    keep_lines;             // s094: 0 off, 2 / 3, -1 all together
    // s157 (Scott): the style holds the tab stops; a paragraph may have its
    // own (the tab ruler edits these), "Reset to style" drops them.
    std::optional<std::vector<TabStop>> tabs;
    bool empty() const {
        return !align && !left_indent && !right_indent && !first_indent && !space_before && !space_after && !keep_next &&
               !hyphenate && !keep_lines && !tabs;
    }
    bool operator==(const ParaLocal&) const = default;
};

// s109: a footnote -- its words, kept with the paragraph its reference
// mark is in (kMarkNote in the text: the k-th mark is notes[k]). Numbered
// through the story, in order.
struct Note {
    std::string      text;
    std::vector<Run> runs;               // character styles in the note, as a paragraph's
    bool operator==(const Note&) const = default;
};

// s128: a cross-reference -- a mark in the text (kMarkRef) that shows where
// an anchor (kMarkAnchor, by its id) is: its page's number, its paragraph's
// words, or both. Kept with the paragraph its mark is in (the k-th mark is
// refs[k]), as notes are.
enum class RefShow { Page, Text, TextPage, Note, Custom, Cite };   // s129: Note -- the note's number; Custom -- its own pattern; s141: Cite -- "[3]"
struct XRef {
    std::string target;                  // an anchor's id ("" or gone: shows "??")
    RefShow     show = RefShow::Page;
    std::string pattern;                 // s129: Custom -- # the page, % the words, * the note's number; s141: ^ the source's number;
                                         // s143: Cite -- its locator ("p. 14": "[2, p. 14]")
    std::vector<std::string> more;       // s143: Cite -- other sources' anchors ("[1, 3]")
    bool operator==(const XRef&) const = default;
};
// s143: a reference shown this way keeps words of its own (Custom: its pattern; Cite: its locator).
inline bool keeps_pattern(RefShow s) { return s == RefShow::Custom || s == RefShow::Cite; }
// s134: an index entry marked in the text (kMarkIndex): its term, and a
// sub-entry under it if any ("Harbors" / "fees"). Kept with its paragraph
// as notes and references are (the k-th mark is index[k]).
// s136: `see` -- the entry sends the reader to another ("Charts. See
// Maps"): the mark adds no page. With `also`, after the entry's pages
// ("Harbor, 1, 3. See also Ports").
// s139: what stretch of text a mark covers -- its own place (the page it
// is on), the rest of its paragraph, on to the next heading (a paragraph a
// Contents list gathers, or in a style named Heading...), `count`
// paragraphs from its own, or to the story's end. Every page the stretch
// is set on is the entry's; a stretch over pages in a row is a range.
enum class IndexSpan { Point, Paragraph, NextHeading, Paragraphs, StoryEnd };
struct IndexMark {
    std::string term;
    std::string sub;
    std::string see;                     // s136
    bool        also = false;            // s136
    IndexSpan   span = IndexSpan::Point; // s139
    int         count = 1;               // s139: Paragraphs -- how many (its own the first)
    bool operator==(const IndexMark&) const = default;
};
// s129: the pattern a reference shows by (Custom: its own).
std::string ref_pattern(const XRef& r);

struct Para {
    std::string      style;              // a paragraph style id
    std::string      text;
    std::vector<Run> runs;               // s011
    int              restart = 0;        // s047: a numbered list starts again here, at this number (0: counts on)
    // s055: a break typed before it (column / frame / page break): it starts
    // as a style's Start would -- this paragraph only. Anywhere = none.
    ParaStart        brk = ParaStart::Anywhere;
    ParaLocal        over;               // s093: its own settings over the style's
    std::vector<Note> notes;             // s109: one for each kMarkNote in text, in order
    std::vector<XRef> refs;              // s128: one for each kMarkRef in text, in order
    std::vector<std::string> anchors;    // s128: one id for each kMarkAnchor in text, in order
    std::vector<IndexMark> index;        // s134: one for each kMarkIndex in text, in order
    // s201 (W1): a bibliography's copy of a source from another story -- that
    // story's id ("" = a paragraph of its own). A rebuild gathers the
    // original, not the copy, so a copy whose original is gone drops out.
    std::string      copy_of;
    bool operator==(const Para&) const = default;
};

struct Story {
    std::string       name;
    std::vector<Para> paras;
    bool operator==(const Story&) const = default;
};

// s044: a rule (a line) above a paragraph's first line or below its last.
// `weight` 0 = none. Above: its bottom edge `offset` above the first
// line's baseline; below: its top edge `offset` below the last line's.
// Across the column (inside the indents), or just the line's text.
struct ParaRule {
    double      weight = 0;          // points
    std::string color;               // a swatch id; "" = black
    double      tint = 1;
    double      offset = 0;          // points
    bool        text_width = false;
    bool operator==(const ParaRule&) const = default;
};

// s045: bullets and numbering. A marker set before a paragraph's first
// line, `hang` points left of its text (in the left indent); drawn, not
// stored. Numbers count on through consecutive paragraphs of the style.
enum class ListType { None, Bullet, Number };
struct ListSpec {
    ListType    type = ListType::None;
    std::string bullet = "\u2022";   // •
    Numbering   numbering = Numbering::Arabic;
    std::string after = ".";         // after a number
    double      hang = 18;           // points
    // s047: numbers carry on past other paragraphs (else another style
    // restarts them); `level` 2 restarts after each level-1 list paragraph.
    bool        continues = false;
    int         level = 1;               // 1..9 (s251: was 1..2)
    // s251: "1.2." -- the levels above's numbers first (each its own form, a
    // "." after), as InDesign's ^1.^2; and the marker set in a character style.
    bool        with_parent = false;
    std::string marker_style;
    bool operator==(const ListSpec&) const = default;
};

struct Style {
    std::string name;
    StyleKind   kind   = StyleKind::Paragraph;
    double      pos    = 0;
    std::string family = "Serif";
    double      size   = 11, leading = 14;   // points; 0 = inherit (character styles)
    bool        bold   = false, italic = false;
    Align       align  = Align::Left;
    double      space_before = 0, space_after = 0;   // s011: points, paragraph styles
    std::string color;                   // s015: a swatch id; "" = black (a character style: inherit)
    double      color_tint = 1;          // s021: of that swatch, 0.05..1
    // s026: paragraph indents (points) and tab stops. A negative first
    // indent hangs the first line out. `skip_after_heading`: no first-line
    // indent at a story's start or after a heading -- a paragraph set
    // larger than this one (as novels are set; s029 -- was "only after the
    // same style", which also skipped it after a chapter's first paragraph).
    double      left_indent = 0, right_indent = 0, first_indent = 0;
    bool        skip_after_heading = false;
    std::vector<TabStop> tabs;
    // s029: a drop cap -- the first `drop_chars` characters set `drop_lines`
    // lines tall (0 = none), the paragraph's first lines beside them.
    int         drop_lines = 0, drop_chars = 1;
    // s036: each line's baseline on its page's baseline grid (when the
    // page's master has one), the lines moved down to the next grid line.
    bool        align_grid = false;
    // s038: break words at the line's end (TeX US English patterns), so
    // justified lines space evenly. Words of 5+ letters, 2 before and 3 after.
    bool        hyphenate = false;
    // s065: steering it -- at most `hyph_limit` hyphenated lines in a row
    // (0: no limit), words of at least `hyph_shortest` letters, and the
    // paragraph's last word left whole unless `hyph_last_word`.
    int         hyph_limit = 2;
    int         hyph_shortest = 5;
    bool        hyph_last_word = false;
    // s039: keeps, where a column breaks. `keep_next`: this paragraph's
    // last line stays in the same column as the next paragraph's first
    // (headings) -- s257: as many of the next's first lines (1 - 5; 0 off),
    // InDesign's "Keep with Next: N lines". `keep_lines`: 0 none; N (2, 3):
    // at least N lines at a paragraph's start and at its end (no orphans /
    // widows); -1 the whole paragraph together.
    int         keep_next = 0;
    int         keep_lines = 0;
    // s043: start in the next column / frame / page / right-hand page when
    // something is already set there (a chapter's opening heading).
    ParaStart   start = ParaStart::Anywhere;
    // s242 (T2c): its space before kept even at a column's / frame's top (a
    // chapter heading sunk down its new page, as LibreOffice sets it). Off:
    // dropped there, as before.
    bool        top_space = false;
    ParaRule    rule_above, rule_below;   // s044
    ListSpec    list;                     // s045
    // s037: styles built on styles (paragraph styles). `based_on`: a style
    // whose settings this one takes, except those it sets itself (`own`,
    // keys from style_field_keys(), sorted). The inherited values are kept
    // here too, written through whenever the base changes, so every reader
    // sees plain settings. `next`: the style Enter at a paragraph's end
    // starts ("" = this one again).
    std::string based_on;
    std::string next;
    std::vector<std::string> own;
    bool        no_break = false;        // s075: character style -- its words stay on one line
    // s260: character attributes -- a paragraph style's for all its text, a
    // character style's over it (0 / false there: the paragraph's).
    // `tracking`: thousandths of an em after each character; `shift`: the
    // baseline moved up (points, down < 0); `position`: 0 normal, 1
    // superscript, 2 subscript; `caps`: 0 as typed, 1 all caps, 2 small caps.
    double      tracking = 0, shift = 0;
    int         position = 0, caps = 0;
    bool        underline = false, strike = false;
    OtFeatures  ot;                      // s264
    // s166 (Scott): a paragraph style across ALL the frame's columns -- the
    // text before it balanced over the columns above it, the text after
    // going on in columns below it.
    bool        span = false;
    // s183 (G1): a paragraph SPLIT into sub-columns inside a one-column
    // frame (InDesign's Split Column): its lines balanced over `split`
    // columns (0 / 1: not split), `split_gutter` points between them.
    int         split = 0;
    double      split_gutter = 12;
    // s293: the split's paragraphs taken TOGETHER -- consecutive paragraphs
    // split alike (k, gutter, run) balanced as one band, as a word
    // processor's section in columns; off: each paragraph its own band.
    bool        split_run = false;
    // s190 (G4): the language its words hyphenate by (core::hyphen_languages:
    // "en-US", "de", "fr"...; "" = US English). s226: a CHARACTER style's
    // too -- its words in that language; "" = the paragraph's, "none" = No
    // language (they don't hyphenate; typed hyphens still do).
    std::string lang;
    bool operator==(const Style&) const = default;
};

struct Swatch {
    std::string           name;
    double                pos   = 0;
    ColorModel            model = ColorModel::CMYK;
    std::array<double, 4> v{};          // CMYK 0-100 / RGB 0-255 / gray 0-100
    std::string           spot;          // the ink's name, for Spot
    std::uint32_t         preview = 0x808080;   // what the screen shows (spot / CMYK approximation)
    std::string           from;          // s171: the library it was taken from ("" = made here)
    bool operator==(const Swatch&) const = default;
};

struct Asset {
    std::string name;                    // display name, "guild-map.tif"
    double      pos = 0;
    std::string path;                    // linked: absolute or project-relative; embedded: assets/<file>
    bool        embedded = false;
    std::string color = "RGB";           // "CMYK" / "RGB" / "Gray"
    int         px_w = 0, px_h = 0;      // pixels; 0 = vector
    std::string source;                  // embedded (s009): the file it was copied from
    std::int64_t stamp = 0;              // linked (s009): the file's time when placed / updated; 0 = unknown
    // s173: the picture's outline for a Shape wrap, traced from its pixels
    // (see trace_shape): its aspect (w / h), then each row band's solid
    // span as x0, x1 (0..1 of its width; x0 > x1 = nothing in that band).
    // Empty = not traced (or nothing to trace: a Shape wrap then wraps the frame).
    std::vector<float> shape;
    // s189 (G3): its Photoshop clipping path, read when placed: each closed
    // subpath as its point count then x, y pairs (0..1 of the picture).
    // The picture is drawn clipped to it (even-odd); empty = none.
    std::vector<float> clip;
    // s227 (G3): ALL its Photoshop paths -- their names, each packed as
    // `clip`, and each traced as `shape` for a Shape wrap -- for a frame
    // to pick from (Frame::clip_path). Empty before s227 (Update reads them).
    std::vector<std::string>        path_names;
    std::vector<std::vector<float>> path_clips, path_shapes;
    double      vec_w = 0, vec_h = 0;    // s188: a vector's own size, points (0: not known -- placed before s188)
    bool operator==(const Asset&) const = default;
};

struct GenList {
    std::string              name;
    ListKind                 kind = ListKind::Contents;
    double                   pos  = 0;
    std::vector<std::string> from_styles;   // paragraph style ids gathered
    std::string              placed_on;     // page id, or ""
    int                      built_entries = -1;   // entries when last built; -1 = never
    std::string              story;         // s087: the story Build writes into ("" = none yet)
    std::vector<std::string> entry_styles;  // s088: the style each level's entries take (by level; "" / missing = found or made)
    bool                     numbers = true;   // s090: entries end with a tab and the page number
    bool                     cite_order = false;   // s142: a bibliography -- in the order first cited (else alphabetical)
    bool operator==(const GenList&) const = default;
};

// A ruler guide (s006): on a page or master, at `pos` points from its left
// (vertical) or top (horizontal) edge, spanning it. A master's guides show on
// every page that uses it.
struct Guide {
    std::string owner;
    bool        vertical = true;
    double      pos = 0;
    bool operator==(const Guide&) const = default;
};

// s305: a layer -- the whole document's (as InDesign), bottom first by pos.
// Frames on a layer are drawn above those on the layers under it; a hidden
// layer's frames aren't drawn or picked, a locked one's aren't picked, a
// non-printing one's are drawn on screen but left out of exports. A frame
// on no layer ("" or one gone) is on the bottom layer.
struct Layer {
    std::string name;
    double      pos = 0;
    bool        visible = true, locked = false, print = true;
    std::string color;                   // its frames' selection colour ("" the usual)
    bool operator==(const Layer&) const = default;
};

using Record = std::variant<Settings, Section, Page, Master, Frame, Story, Style, Swatch, Asset, GenList, Guide, Layer>;

class Doc {
public:
    using Id    = std::string;
    using Value = Record;

    // ── the journal's contract ──────────────────────────────────────────────
    std::optional<Record> get(const Id& id) const;
    void put(const Id& id, const std::optional<Record>& v);

    // ── typed reading ───────────────────────────────────────────────────────
    template <class T> const T* as(const Id& id) const {
        const auto it = m_recs.find(id);
        return it == m_recs.end() ? nullptr : std::get_if<T>(&it->second);
    }
    // Every id of one kind, in its natural order (pos where it has one; for
    // sections, the order of their start pages).
    template <class T> std::vector<Id> ids_of() const;

    const Settings& settings() const;
    std::vector<Id> pages() const { return ids_of<Page>(); }
    std::size_t     size() const { return m_recs.size(); }
    const std::map<Id, Record>& records() const { return m_recs; }

    // Pages: where they fall.
    int         page_index(const Id& page) const;       // 0-based in page order; -1 absent
    Id          section_of(const Id& page) const;       // "" = before any section (implicit)
    std::string folio(const Id& page) const;            // "iv", "7"
    Side        side_of(const Id& page) const;          // Right / Left (facing), Any (single)
    std::vector<Id> pages_of(const Id& section) const;
    std::vector<Id> frames_on(const Id& owner) const;   // by z (s305: by layer, then z)

    // s305: the layer new frames go on ("" or one gone: the top layer). Not
    // a document change -- not saved, not an undo step.
    void set_active_layer(const Id& layer) { m_active_layer = layer; }
    Id   active_layer() const;

    // A fresh id with a prefix: "page" -> "page.13". Never reused in a session.
    Id fresh_id(const std::string& prefix);
    // A place just after `after` among T (append when "" / absent / empty).
    template <class T> double pos_after(const Id& after) const;

    // ── the one-door guard (as Cairn's Stones) ──────────────────────────────
    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    struct Loading {   // a load is not a user choice and is not undoable
        explicit Loading(Doc& d) : m_d(d) { ++m_d.m_loading; }
        ~Loading() { --m_d.m_loading; }
        Doc& m_d;
    };
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

    // Start over (a new / opened document). Not a user step.
    void reset();

    // Bumped by every put / reset (s007): caches keyed on it (text flow) know
    // when to recompute.
    std::uint64_t revision() const { return m_rev; }
    // s105: the ids written since revision `rev`, oldest first (repeats
    // kept) -- false when that's no longer known (long ago, or a reset).
    bool changes_since(std::uint64_t rev, std::vector<Id>& ids) const;

private:
    std::map<Id, Record>       m_recs;
    std::uint64_t              m_rev = 0;
    std::deque<std::pair<std::uint64_t, Id>> m_log;   // s105: (revision after, id), the last few thousand writes
    std::uint64_t              m_log_floor = 0;        // changes since a revision below this are not known
    std::map<std::string, int> m_next;   // fresh_id counters per prefix
    // s101: page order, rebuilt when the revision moves (page_index was a
    // scan of every record per call -- per page, per projection).
    mutable std::map<Id, int>  m_page_at;
    mutable std::uint64_t      m_page_at_rev = ~0ull;
    // s105: more of the same, each rebuilt when the revision moves -- the
    // projection asked these per page, each a scan (and a sort) of every record.
    mutable std::array<std::vector<Id>, std::variant_size_v<Record>> m_ids;   // ids_of, by kind
    mutable std::array<std::uint64_t, std::variant_size_v<Record>>   m_ids_rev = [] {
        std::array<std::uint64_t, std::variant_size_v<Record>> a{};
        a.fill(~0ull);
        return a;
    }();
    mutable std::map<Id, std::vector<Id>> m_frames_on;   // owner -> its frames, by z
    mutable std::uint64_t      m_frames_on_rev = ~0ull;
    mutable std::map<Id, Id>   m_section_at;             // page -> its section
    mutable std::uint64_t      m_section_at_rev = ~0ull;
    int                        m_strays = 0;
    Id                         m_active_layer;          // s305
    int                        m_loading = 0;
    std::function<bool()>      m_door;
    std::function<void()>      m_changed;
};

using DocJournal = Journal<Doc>;

// Display helpers.
std::string format_folio(int n, Numbering how);
// s111: a footnote's number as shown: 1, i, a -- or * \u2020 \u2021 \u00a7 \u2016 \u00b6, then doubled ...
std::string note_label(int n, NoteStyle how);
std::string inches(double pt);                          // 432 -> "6", 423 -> "5.875"
// s041: a typed length in points: "12", "12.5 pt", "1in", "1\"", "3p6" (3
// picas 6 points), "6p", "10 mm", "1.5cm". A bare number is in `unit`
// points (1: points, 72: inches). Nothing (nullopt) if it isn't a length.
std::optional<double> parse_length(std::string_view text, double unit = 1);
// s075: where a paragraph's words must stay on one line -- its runs in a
// character style with No Break (or one it's based on), merged. Bytes.
struct ByteSpan { int a = 0, b = 0; };
std::vector<ByteSpan> no_break_spans(const Doc& d, const Para& p);
// s260: a run's character attributes, resolved -- the paragraph's style, a
// character style's over it, the words' own over both.
struct CharLook {
    double tracking = 0, shift = 0;
    int    position = 0, caps = 0;
    bool   underline = false, strike = false;
    OtFeatures ot;   // s264
    bool operator==(const CharLook&) const = default;
};
CharLook char_look(const Style& para, const Style* cs = nullptr, const Local* own = nullptr);
// The superscript / subscript set: this much of the size, raised (or
// lowered) this much of it (InDesign's 58.3% / 33.3%).
constexpr double kScriptSize = 0.583, kSuperRise = 0.333, kSubDrop = 0.333;
// s075: a line from `pos` that Pango ends after `took` bytes: pulled back
// to before a no-break span it would split (to the last space before the
// span, else the span's start) -- unless the span starts the line (too
// long for any line: it breaks as usual). The line's new length.
int keep_together(const std::string& text, const std::vector<ByteSpan>& spans, int pos, int took);
// s071: a typed percentage: "40", "40%", "40 %", "12.5%". Nothing if it
// isn't one.
std::optional<double> parse_percent(std::string_view text);
// A number for a verb: 12 -> "12", 12.5 -> "12.5" (to 1/1000).
std::string num_text(double v);
std::string size_text(double w, double h, std::size_t folds, Units u = Units::Inches);   // "17.75 × 9 in (3-panel)"
// s073: lengths in a unit. unit_pt: points per unit (72, 72 / 25.4, 12, 1).
// unit_name: "in", "mm", "picas", "pt". length_in: the number alone
// ("0.75", "19.05", "4p6", "54"); length_text: with its unit ("0.75 in",
// "19.05 mm", "4p6", "54 pt").
double      unit_pt(Units u);
std::string unit_name(Units u);
std::string length_in(double pt, Units u);
std::string length_text(double pt, Units u);
std::string numbering_text(Numbering n);
std::string side_text(Side s);
std::string master_label(const Doc& d, const Doc::Id& master);  // "B-Body", "None"
// s277: Go to Page -- what the typed text names: a page by its folio ("8",
// "iv"; letters' case ignored), "+N" the Nth page from the front, or a master
// by its prefix or label ("B", "B-Body"). "" when nothing matches.
Doc::Id go_to_target(const Doc& d, const std::string& typed);

// ── text positions (s010) ───────────────────────────────────────────────────
// A place in a story: paragraph index, BYTE offset into its UTF-8 text (always
// on a character boundary). Ordered as the text reads.
struct TextPos {
    int para = 0;
    int byte = 0;
    auto operator<=>(const TextPos&) const = default;
};
// Inside the story's text (a paragraph that exists, a byte on a boundary).
TextPos clamp_pos(const Story& s, TextPos p);
TextPos next_char(const Story& s, TextPos p);   // across a paragraph end, too
TextPos prev_char(const Story& s, TextPos p);
TextPos next_word(const Story& s, TextPos p);   // to the start of the next word
TextPos prev_word(const Story& s, TextPos p);   // to the start of this / the previous word
TextPos story_end(const Story& s);
// The text between two places, paragraphs joined by '\n'.
std::string text_between(const Story& s, TextPos a, TextPos b);
// s061: where a change to a story ends, in the story after it -- for the
// caret after an undo / redo (the end of what came back, or where what
// went was). The start when nothing differs.
TextPos changed_end(const Story& before, const Story& after);
// The word around a place (double-click): [first, last).
std::pair<TextPos, TextPos> word_at(const Story& s, TextPos p);
// The character style at a place: the character before it carries on (as
// typing continues it); at a paragraph's start, the first character's.
std::string char_style_at(const Para& p, int byte);
// s030: the local formatting at a place (as char_style_at: the character before it).
Local local_at(const Para& p, int byte);
// Is every character of [a, b) bold / italic, all told (local, then the
// character style, then the paragraph style)? What Ctrl+B / Ctrl+I toggle.
// s049: how a paragraph's words are set, apart from its paragraph style --
// the spans with a character style or local formatting, and whether each
// reads bold / italic (local over the character style). For the story
// editor, which shows the words' own formatting, not the paragraph's.
struct WordLook {
    int         start = 0, end = 0;   // bytes in the paragraph
    bool        bold = false, italic = false;
    std::string cstyle;               // a character style id, or ""
};
std::vector<WordLook> word_looks(const Doc& d, const Para& p);
bool all_bold(const Doc& d, const Story& s, TextPos a, TextPos b);
// s263: every character in [a, b) has the look `f` asks for (a toggle's test).
bool all_look(const Doc& d, const Story& s, TextPos a, TextPos b, const std::function<bool(const CharLook&)>& f);
bool all_italic(const Doc& d, const Story& s, TextPos a, TextPos b);

// ── images (s009) ───────────────────────────────────────────────────────────
// A file as the UI found it on disk. The core never decodes: the app probes
// the file (views/Images) and hands the verbs what it learned.
struct ImageFile {
    std::string  path;                   // absolute
    std::string  name;                   // its file name
    int          px_w = 0, px_h = 0;     // pixels (a vector's own size, in points)
    bool         vector = false;
    std::string  color = "RGB";
    std::int64_t stamp = 0;
    std::vector<float> shape;            // s173: traced (Asset::shape), when the UI traced it
    std::vector<float> clip;             // s189: its clipping path (Asset::clip), when it has one
    std::vector<std::string>        path_names;               // s227: all its paths (Asset::path_*)
    std::vector<std::vector<float>> path_clips, path_shapes;
};
// s184 (G5): a picture's colour space from its file's own header -- "CMYK",
// "Gray", "RGB" or "Lab"; "" when the bytes aren't a format read here. JPEG:
// its frame's components (1 / 3 / 4); PNG: IHDR's colour type; TIFF:
// PhotometricInterpretation (5 = separated, CMYK); PSD: its colour mode;
// GIF / BMP / WebP: RGB. `read(offset, n)`: up to n bytes from offset
// (fewer at the end) -- the app reads the file; the core never opens one.
using ReadAt = std::function<std::string(std::uint64_t offset, std::size_t n)>;
std::string picture_color(const ReadAt& read);
std::string picture_color(const std::string& bytes);   // a file's bytes, whole or its start

// Where an asset's file is: an absolute path, or "" when it can't be known (a
// relative path in an unsaved project).
std::string  resolve_asset(const Asset& a, const std::string& project_dir);
std::int64_t file_stamp(const std::string& path);    // 0 = no such file
enum class LinkStatus { OK, Modified, Missing, Embedded, EmbeddedMissing };
LinkStatus   link_status(const Asset& a, const std::string& project_dir);
std::string  link_status_text(LinkStatus s);
// s247: the pictures linked (not embedded) whose files are there -- what Embed All takes.
std::vector<Doc::Id> linked_assets(const Doc& d, const std::string& project_dir);
// The picture's rect inside a frame (both in the same coordinates): Fill
// covers the frame (cropped by it), Fit shows it whole. iw / ih: its aspect.
Rect   fit_rect(const Rect& frame, double iw, double ih, ImageFit fit);
// s188: a picture's shape for placing it (iw x ih) -- its pixels, a
// vector's own size, else 1 x 1 (a vector placed before s188).
struct Aspect { double w = 1, h = 1; };
Aspect picture_aspect(const Asset& a);
// s035: its scale now, percent of its own size.
double picture_scale(const Frame& f, const Asset& a);
// s035: where a frame's picture (iw x ih) goes in `frame` (any units, `s` of
// them a point): Fill / Fit as fit_rect, Custom from the frame's own pic_*.
Rect   picture_rect(const Rect& frame, const Frame& f, double iw, double ih, double s = 1);
// Pixels per inch as placed (lower = softer); 0 for a vector or unknown.
double effective_ppi(const Frame& f, const Asset& a);
inline constexpr double kLowPpi = 150;   // under this, "Low res"
// The size a placed file takes when no frame was drawn for it: 300 ppi (a
// vector at its own size), shrunk to fit max_w x max_h.
struct Size2 { double w = 0, h = 0; };   // a plain aggregate: std::pair trips GCC's -Wpsabi note
Size2 natural_size(const ImageFile& f, double max_w, double max_h);
// s035: a placed picture's size at its own resolution (300 ppi).
Size2 picture_natural(const Asset& a);

// ── the verbs: every user choice on the document ───────────────────────────
// Each goes through the journal; the UI calls these and nothing else writes.
struct Placed;   // s027: frames_rehome takes the canvas layout (below)
struct MasterItemOn { Doc::Id page, item; };   // s169: a master's item as it shows on a page

namespace doc {

// Size presets the inspector offers for a page: the trim, a 2-panel and a
// 3-panel foldout (each fold-in panel 1/8 in narrower, so it tucks inside).
struct SizePreset { std::string label; double w, h; std::vector<double> folds; };
std::vector<SizePreset> size_presets(const Settings& s);
// s148: for a page -- its master's size the trim (no master: the document's).
std::vector<SizePreset> size_presets(const Doc& d, const Doc::Id& page);

std::vector<Doc::Id> add_pages(DocJournal& j, Doc& d, const Doc::Id& after, int count,
                               const Doc::Id& master);
bool remove_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
// s277: `count` new pages on `master` placed BEFORE page `before` ("" or not
// a page: at the end) -- one step. The navigator's master dropped in a gap.
std::vector<Doc::Id> insert_pages(DocJournal& j, Doc& d, const Doc::Id& before, int count, const Doc::Id& master);
// s256: remove_pages' parts, for a step of one's own -- the ids it touches
// (pages, their frames and guides, the sections) and the removal itself
// (call inside the step; no check that a page is left).
std::vector<Doc::Id> page_removal_ids(const Doc& d, const std::vector<Doc::Id>& pages);
void drop_pages(Doc& d, const std::vector<Doc::Id>& pages);
bool move_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& after);
// s283 (X1): copies of pages -- each its size, master, own margins, guides
// and frames (duplicate_frames' way: a story with frames among them copied
// once, threads inside the copy kept) -- placed after `after` ("": after the
// last of them), in their order. One step; the pages made.
std::vector<Doc::Id> duplicate_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& after = {},
                                     bool at_front = false);   // at_front: before the first page (after is ignored)
bool set_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& master);
// s066: the document's own settings, one step each. The title (trimmed,
// not empty); the trim -- every page and master at the old trim height
// follows, its width (and a foldout's folds) in proportion; the bleed
// (points, >= 0); facing pages on / off.
bool set_title(DocJournal& j, Doc& d, const std::string& title);
// s239: the document's author (may be empty). One step.
bool set_author(DocJournal& j, Doc& d, const std::string& author);
bool set_reflow(DocJournal& j, Doc& d, bool on);   // s255
// s256: 0 off, 1 adds pages, 2 adds and removes pages. One step.
bool set_reflow_mode(DocJournal& j, Doc& d, int mode);
// s240: the style a running head follows (slot 0 chapter, 1 subtitle; "" by name). One step.
bool set_running(DocJournal& j, Doc& d, int slot, const std::string& style);
bool set_trim(DocJournal& j, Doc& d, double w, double h);
// s147: a master's own page settings. Its size: the pages using it at its
// old size follow (a page of its own size keeps it). One step each; false
// if nothing changed or out of range.
bool set_master_size(DocJournal& j, Doc& d, const Doc::Id& master, double w, double h);
bool set_master_bleed(DocJournal& j, Doc& d, const Doc::Id& master, double bleed);
bool set_master_facing(DocJournal& j, Doc& d, const Doc::Id& master, bool facing);
bool set_bleed(DocJournal& j, Doc& d, double bleed);
bool set_facing(DocJournal& j, Doc& d, bool facing);
// s069: wrap pushes only text beneath (Settings::wrap_beneath). One step.
bool set_wrap_beneath(DocJournal& j, Doc& d, bool on);
// s111: footnote numbering (restart, look). One step each; false if unchanged.
bool set_note_numbering(DocJournal& j, Doc& d, NoteRestart restart, NoteStyle style);
// s117: the footnote area, percent of the column (10 .. 100). One step.
bool set_note_area(DocJournal& j, Doc& d, int percent);
// s118: the foot's look -- rule (percent 0..100), its weight (0.1..6 pt),
// space between notes (0..36 pt), the continued line. One step.
bool set_note_look(DocJournal& j, Doc& d, int rule, double rule_w, double space, const std::string& cont);
// s120: where notes go, and the endnotes' title. One step.
bool set_note_place(DocJournal& j, Doc& d, NoteAt at, const std::string& title);
// s126: how the number before a note's words looks. One step.
bool set_note_num(DocJournal& j, Doc& d, Settings::NoteNum how);
// s073: the document's display unit. One step.
bool set_units(DocJournal& j, Doc& d, Units u);
// s154: the rulers' origin (the zero point back to it) -- one step; the
// zero point dragged (points from the origin's own) -- one step. Spine
// keeps it fixed: set_ruler_zero does nothing.
bool set_ruler_origin(DocJournal& j, Doc& d, RulerOrigin o);
bool set_ruler_zero(DocJournal& j, Doc& d, double x, double y);
// s078: keep a query under its name (trimmed, not empty; the same name is
// replaced, else it's added, the list kept by name); delete one. One step.
bool save_query(DocJournal& j, Doc& d, SavedQuery q);
bool delete_query(DocJournal& j, Doc& d, const std::string& name);
// s062: pages' own margins -- `f` edits them (starting from what they use
// now); `reset` drops them (back to the master's). One step.
bool edit_page_margins(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const std::string& label,
                       const std::function<void(Margins&)>& f);
bool reset_page_margins(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
bool set_page_size(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const SizePreset& sz);

Doc::Id add_section(DocJournal& j, Doc& d, const Doc::Id& start_page, const std::string& name,
                    Numbering n, int start_at);
bool set_numbering(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, Numbering n);
bool set_starts(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, Side s);
bool remove_section(DocJournal& j, Doc& d, const Doc::Id& section);
// s060: a section's name (what Section Name markers show), the number its
// first page takes, and the master new pages in it get. One step each.
bool rename_section(DocJournal& j, Doc& d, const Doc::Id& section, const std::string& name);
bool set_section_start_at(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, int start_at);
bool set_section_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, const Doc::Id& master);

// The neighbour that takes the selection when `pages` go (next, else previous).
Doc::Id neighbour_page(const Doc& d, const std::vector<Doc::Id>& pages);

// ── frames (s005: the canvas) ───────────────────────────────────────────────
// Move by (dx, dy) points -- the arrow keys. With a merge key, presses inside
// the journal's window fold into ONE step.
bool move_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double dx, double dy,
                 const std::string& merge_key = {});
// A drag is ONE step: begin at press (the frames it may touch), set as the
// pointer moves (each write inside the open gesture), end at release. Cancel
// (Escape) puts everything back and keeps nothing.
void frames_begin(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, const std::string& label);
void frames_set(DocJournal& j, Doc& d, const Doc::Id& frame, const Rect& r);   // r in the owner's coords
bool frames_end(DocJournal& j);
// s027: frames dragged onto another shown page (their centre over it)
// belong to that page now -- same place on the spread. Inside the drag's
// gesture: part of its one step. Returns how many moved page.
int frames_rehome(DocJournal& j, Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& ids);
void frames_cancel(DocJournal& j);

// ── frame tools (s008) ──────────────────────────────────────────────────────
// Draw a frame on a page / master (rect in its coordinates). A new TEXT frame
// gets a story of its own (one empty paragraph in the first paragraph style).
// s014: `columns` / `gutter` for a text frame (one drawn across the margins
// takes the page's columns).
// ── s197: shapes -- rectangles, ovals, paths ──────────────────────────────
// A point in the owner's coordinates (a path's anchor / handle, absolute).
struct XY { double x = 0, y = 0; bool operator==(const XY&) const = default; };
// One step of an outline: 'M' move (v[0..1]), 'L' line, 'C' curve (two
// handles, then the point), 'Z' close. Screen, PDF and print all draw these.
struct OutlineOp { char op = 'M'; double v[6] = {}; };
// The frame's outline in its owner's coordinates (moved by dx, dy): its box
// for a text / image frame or a Rect shape; four quarter-curves for an Oval;
// the points for a Path (a straight side drawn as a line).
std::vector<OutlineOp> outline_ops(const Frame& f, double dx = 0, double dy = 0);
// Drawn other than as its box (an oval or a path).
bool has_outline(const Frame& f);
// A path's points with the frame's box applied (owner coordinates).
std::vector<PathPoint> path_points(const Frame& f);
// The outline flattened to polygons (owner coordinates), each segment cut
// to within `tol` points. A closed shape's polygon closes itself.
std::vector<std::vector<XY>> outline_polys(const Frame& f, double tol = 0.25);
// The curves' own bounds (not their handles') of points in owner coordinates.
Rect path_bounds(const std::vector<PathPoint>& abs, bool open);
// Make f a Path through `abs` (owner coordinates): its box becomes the
// curves' bounds and the points are stored within it.
void set_path(Frame& f, const std::vector<PathPoint>& abs, bool open);
// A straight point (both handles on the anchor).
PathPoint corner_point(double x, double y);
// Draw a path on a page or master: one undo step ("Draw line" for two
// straight points, open; "Draw path" otherwise). Outlined in Black, like
// any new shape. Fewer than two points, or no size at all: nothing.
Doc::Id add_path(DocJournal& j, Doc& d, const Doc::Id& owner, const std::vector<PathPoint>& abs, bool open,
                 bool across = false, const std::string& label = {});
// s206 (as Curvz's): a polygon's or star's corners around (cx, cy) --
// `sides` points `r` out, the first at `angle` (radians; -pi/2 = straight
// up, the page's y down); `inner` below 1 makes a star: a point between
// each pair at r * inner (perfect_star gives straight-sided stars).
std::vector<PathPoint> polygon_points(double cx, double cy, double r, int sides, double inner, double angle);
// The star depth a drawn one snaps to: straight sides (5 points or more), else -1.
double perfect_star(int sides);
// Draw one: "Draw polygon" / "Draw star". A closed path (Direct edits its points).
Doc::Id add_polygon(DocJournal& j, Doc& d, const Doc::Id& owner, double cx, double cy, double r, int sides, double inner,
                    double angle);
// s204: what a stroke draws -- the outline with an open path's ends drawn
// back under their arrowheads (so the line's end doesn't show past the
// tip); the dash pattern in points (empty: solid); the ends' shapes, filled
// in the stroke's colour (owner coordinates).
std::vector<OutlineOp> stroke_ops(const Frame& f, double dx = 0, double dy = 0);
std::vector<double> dash_pattern(const Frame& f);
std::vector<std::vector<XY>> end_shapes(const Frame& f, double dx = 0, double dy = 0);
// s216: text on a path. The path as a track to measure along (owner
// coordinates): its first subpath, flattened; a closed one returns to its
// start. at(s): the point s points along and the direction there (radians).
struct PathTrack {
    std::vector<XY>     pts;
    std::vector<double> cum;   // the length to each point
    double length() const { return cum.empty() ? 0 : cum.back(); }
    void at(double s, double& x, double& y, double& angle) const;
    double nearest(double x, double y, double* dist = nullptr) const;   // how far along the closest point is
    // s225: corner spacing -- the text measured `h` above the path, on its
    // side: each turn TOWARD the words takes h x its angle off the text's
    // length there (turns away are left alone). wa[i]: the text distance
    // leaving point i; empty = none (text distance = path distance).
    std::vector<double> wa;
    double to_w(double s) const;   // path distance -> text distance
    double to_s(double t) const;   // and back (round an inside turn: just past it, the room left)
    double text_s(double from, double bx) const { return to_s(to_w(from) + bx); }   // bx along the text from `from`
};
PathTrack path_track(const Frame& f);
// s218: the text box's coordinates (bx along, by down; `base` its first
// baseline) to the owner's, and back -- `shift`: how far the baseline sits
// below the path (path_shift).
void path_point(const PathTrack& tr, const Frame& f, double bx, double by, double base, double shift, double& x, double& y);
void path_unpoint(const PathTrack& tr, const Frame& f, double x, double y, double base, double shift, double& bx, double& by);
// The baseline's distance below the path for the frame's alignment, from the
// line's ascent and descent (points).
double path_shift(const Frame& f, double ascent, double descent);
// s221: where along the track the words stop -- path_to, or the track's end
// (and never before path_from).
double path_end(const Frame& f, const PathTrack& tr);
// s221: the path_to to store for an end at `s` along the track: at least a
// point past path_from; within half a point of the track's end, 0 (the end
// itself, which follows the path's length); to 0.1 pt.
double path_to_for(const Frame& f, const PathTrack& tr, double s);
// s223: a glyph's axes at a place on the path heading `angle` -- where its
// x (along) and y (down) go, for the frame's effect: x' = xx x + xy y,
// y' = yx x + yy y (cairo's / PDF's cm order: xx yx xy yy). Never mirrored:
// running leftward, Skew and 3D Ribbon turn the glyph over as Rainbow does.
// s224: (x, y) the place on the path (owner coordinates) -- Gravity turns
// the glyph's upright to point away from the frame's centre through it.
struct PathAxes { double xx = 1, yx = 0, xy = 0, yy = 1; };
PathAxes path_axes(const Frame& f, double angle, double x, double y);
// The box the frame's text is flowed in: itself -- or, for text on a path,
// one long line as long as the path (from path_from), its shape a box.
Frame text_box(const Frame& f);
// s203: is (x, y) (owner coordinates) on this shape -- inside a closed
// one, or within `tol` (and half its stroke) of its outline; an open path
// (a line) only near its stroke. Frames without an outline: their box.
bool shape_hit(const Frame& f, double x, double y, double tol);
// s210: what frames hold -- Text (a new empty story), Image (no picture
// yet: Place fills it) or Shape (none). Its outline, fill and stroke stay.
// A text frame gives its text up only when it has none and no thread; a
// picture frame drops its picture (the asset stays while others use it).
// One step ("Content: Text" / "Content: Picture" / "Content: None").
bool set_frame_content(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, FrameKind k);
// s211: a path's subpaths as [a, b) ranges of its points (one when not compound).
struct SubRange { int a = 0, b = 0; };
std::vector<SubRange> sub_ranges(int n, const std::vector<int>& subs);
// Shapes (closed: rectangles, ovals, closed paths) made one compound path --
// the first (in the order given) keeps its style, the rest go; holes where
// they overlap (even-odd). One step "Make compound path"; its id.
Doc::Id make_compound(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames);
// Its subpaths each a path of its own, styled alike. One step; the new ids
// (the first is the frame itself).
std::vector<Doc::Id> release_compound(DocJournal& j, Doc& d, const Doc::Id& frame);
// s217: text set along the frames' outlines (on) -- a shape gets an empty
// story first -- or back inside them (off). `from` >= 0: where along the
// path it starts. One step ("Text on path" / "Text in frame").
bool set_text_on_path(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, bool on, double from = -1);
// Change shapes between Rect and Oval (a Path keeps its points). One step.
bool set_shape_kind(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, ShapeKind k);
// s200: Direct on a shape -- its points to edit (owner coordinates): a
// path's own; an oval's four, a rectangle's corners (the first edit makes
// either a path). `open` says whether the outline is closed.
std::vector<PathPoint> shape_points(const Frame& f, bool& open);
// Where on a shape's outline a point is nearest: the segment (from point
// k to k + 1) and how far along it (0..1, of the curve's own parameter),
// and the distance. k < 0: no outline.
struct OutlineHit { int k = -1; double t = 0, dist = 1e300; };
OutlineHit outline_nearest(const Frame& f, double x, double y);
// The point on segment k at t, its curve split there (de Casteljau) -- the
// outline's look unchanged. One step: "Add point". A rectangle or oval is
// made a path first.
bool path_add_point(DocJournal& j, Doc& d, const Doc::Id& frame, int k, double t);
// s208: a shape's points replaced (owner coordinates) -- the Pen carrying on
// from an open path's end ("Extend path"), or closing it ("Close path").
bool set_path_points(DocJournal& j, Doc& d, const Doc::Id& frame, const std::vector<PathPoint>& abs, bool open,
                     const std::string& label);
// The same points the other way round (each point's handles swap sides).
std::vector<PathPoint> reversed_path(std::vector<PathPoint> P);
// Point k taken out (a closed path keeps three, an open one two). "Delete point".
bool path_delete_point(DocJournal& j, Doc& d, const Doc::Id& frame, int k);
// s207: several at once ("Delete points"); none if too few would be left.
bool path_delete_points(DocJournal& j, Doc& d, const Doc::Id& frame, const std::set<int>& ks);
// s207: points moved by (dx, dy) with their handles -- the arrow keys
// (presses within the journal's window merge on `merge_key`).
bool path_move_points(DocJournal& j, Doc& d, const Doc::Id& frame, const std::set<int>& ks, double dx, double dy,
                      const std::string& merge_key = {},
                      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
// Point k smooth <-> corner: a corner gets handles along its neighbours'
// line, a third of the way to each; a smooth point loses them. One step.
bool path_toggle_point(DocJournal& j, Doc& d, const Doc::Id& frame, int k);

Doc::Id add_frame(DocJournal& j, Doc& d, const Doc::Id& owner, FrameKind kind, const Rect& r,
                  int columns = 1, double gutter = 12, bool across = false,
                  ShapeKind shape = ShapeKind::Rect);   // s197: a shape's outline (Rect / Oval)
// s155: where master items show -- 0 every page, 1 right pages, 2 left
// pages, 3 across the spine (a facing master's only). Each item keeps its
// place on the spread: into Across a right-side item's x gains the page
// width; out of it to Right, loses it. One step.
bool set_shows_on(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, int choice);
// Delete frames. A text frame leaving a thread is spliced out (the frame
// before it points past it) so the story reflows through what is left. A
// story whose LAST frame goes goes with it, and so does an image whose last
// placement goes (s009, as InDesign). One step.
bool delete_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames);
// s282 (X1): frames as the clipboard holds them -- their records and their
// stories (whole), by value, so a cut's frames can be pasted after they've
// gone. `threads`: each story's frames among them, in the story's order;
// `bounds`: round the frames on the first one's owner (its coordinates).
struct FrameClip {
    std::vector<Doc::Id>                      order;
    std::map<Doc::Id, Frame>                  frames;
    std::map<Doc::Id, Story>                  stories;
    std::map<Doc::Id, std::vector<Doc::Id>>   threads;
    Doc::Id                                   owner;
    Rect                                      bounds;
    bool empty() const { return order.empty(); }
};
FrameClip clip_frames(const Doc& d, const std::vector<Doc::Id>& frames);
// Inside a step: the clip's copies (dx, dy) from where they were -- all onto
// `onto` when given, else each on owners[its owner] or its own. As
// duplicate_frames does it (stories copied, threads kept, anchors new).
std::vector<Doc::Id> paste_clip_into(Doc& d, const FrameClip& clip, double dx, double dy,
                                     const std::map<Doc::Id, Doc::Id>& owners, const Doc::Id& onto, std::vector<Doc::Id>& made);
// Paste: the clip onto page / master `onto`, moved (dx, dy). One step; the frames made.
std::vector<Doc::Id> paste_frames(DocJournal& j, Doc& d, const FrameClip& clip, const Doc::Id& onto, double dx, double dy);
// s299: Align -- each unit (a group's frames together, else a frame) moved so
// its `edge` (0 left, 1 centre across, 2 right, 3 top, 4 middle, 5 bottom)
// meets the target's: the selection's bounds (2+ units), the margins or the
// page of the first frame's page (layout coordinates via `pl`). One step.
enum class AlignTo { Selection, Margins, Page };
bool align_frames(DocJournal& j, Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& frames, int edge, AlignTo to);
// s299: Distribute (3+ units) -- 0 centres evenly across, 1 down; 2 even gaps
// across, 3 down -- the outermost two stay put. One step.
bool distribute_frames(DocJournal& j, Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& frames, int how);
// s300: Group (2+ frames: one new group; a grouped frame among them leaves its
// old group) / Ungroup (every group among them). One step each.
bool group_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames);
bool ungroup_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames);
// s305: layers. layers(): bottom first. layer_rank: its place (0 the bottom;
// a frame on no layer 0). frame_shown -- its layer visible; frame_held -- it
// or its layer locked (or hidden): not picked; frame_prints -- shown and its
// layer printing.
std::vector<Doc::Id> layers(const Doc& d);
int  layer_rank(const Doc& d, const Doc::Id& layer);
const Layer* layer_of(const Doc& d, const Frame& f);
bool frame_shown(const Doc& d, const Frame& f);
bool frame_held(const Doc& d, const Frame& f);
bool frame_prints(const Doc& d, const Frame& f);
// s305: a new layer on top ("Layer N" when `name` is ""), made active. The
// first layer made in a document without layers is "Layer 1" holding every
// frame, the new one "Layer 2" above it. One step; its id.
Doc::Id add_layer(DocJournal& j, Doc& d, const std::string& name = {});
// A layer's settings (`f` changes it; the label names the step). One step.
bool edit_layer(DocJournal& j, Doc& d, const Doc::Id& layer, const std::string& label, const std::function<void(Layer&)>& f);
// A layer moved to place `index` (0 the bottom). One step.
bool move_layer(DocJournal& j, Doc& d, const Doc::Id& layer, int index);
// A layer gone, its frames deleted with it (InDesign) -- or, `keep`, moved to
// the layer under it (the one above when it was the bottom). The last layer
// can't go. One step.
bool delete_layer(DocJournal& j, Doc& d, const Doc::Id& layer, bool keep = false);
// Frames moved to a layer (each group whole). One step; how many moved.
int  move_to_layer(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, const Doc::Id& layer);
// How many frames each layer holds (a frame on no layer: the bottom's).
int  frames_on_layer(const Doc& d, const Doc::Id& layer);
// s301: Lock / Unlock frames; Unlock every frame on `owners`. One step each.
bool lock_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, bool lock);
int  unlock_all(DocJournal& j, Doc& d, const std::vector<Doc::Id>& owners);
// s297: words from another program pasted onto a page -- a text frame at
// `r` (its height then follows the text: auto-size), the words its story's
// paragraphs (a line break a new paragraph), in the first paragraph style.
// One step; the frame ("" when nothing to paste or no page).
Doc::Id paste_text_frame(DocJournal& j, Doc& d, const Doc::Id& page, const Rect& r, const std::string& text);
// s297: the words of frames as a plain-text clipboard holds them -- each
// story once (in the frames' order), its paragraphs a line each, a blank line
// between stories; its marks (page numbers, notes ...) left out.
std::string frames_text(const Doc& d, const std::vector<Doc::Id>& frames);
// s284: Step and Repeat -- `count` copies of the frames, the k-th (k dx, k dy)
// from them (points), each as duplicate_frames makes one. One step; every
// frame made, copy by copy.
std::vector<Doc::Id> step_repeat(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, int count, double dx, double dy);
// s281 (X1): copies of frames. Each copy sits (dx, dy) from its frame, on
// `owners[its owner]` when the map names one (a page duplicated), else on its
// own page / master; on top of what's there, in their own stacking order.
// Text: each story with frames among them is copied once, whole (its anchors
// new, references to them following), and those frames' copies are threaded
// through the copy in the story's order -- a thread inside what was copied
// stays a thread. Not an override (the copy is its own). A step of its own
// (or part of the one it runs in). Returns the frames made, in the order given.
std::vector<Doc::Id> duplicate_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double dx, double dy,
                                      const std::map<Doc::Id, Doc::Id>& owners = {});
// The copying itself, inside a step the caller runs: the frames made, then
// the stories made, into `made` (both ids, for the step to know).
std::vector<Doc::Id> copy_frames_into(Doc& d, const std::vector<Doc::Id>& frames, double dx, double dy,
                                      const std::map<Doc::Id, Doc::Id>& owners, std::vector<Doc::Id>& made);
// s032: the export settings, kept with the document (one step; none when
// unchanged).
bool set_export(DocJournal& j, Doc& d, const ExportPrefs& e);
// s025: a master's item on `page` becomes the page's own -- a copy (a text
// frame gets its own copy of the story) that hides the master's there.
// "" if it isn't a master item shown on that page, or is already overridden.
Doc::Id override_master_item(DocJournal& j, Doc& d, const Doc::Id& page, const Doc::Id& master_frame);
// s072: every master item showing on each of `pages` (not yet overridden)
// overridden at once -- ONE step; the copies made, in order.
std::vector<Doc::Id> override_all(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
// s169: these master items overridden on their pages (Ctrl+Shift+drag) --
// each page's own copy; one step. The copies made.
std::vector<Doc::Id> override_master_items(DocJournal& j, Doc& d, const std::vector<MasterItemOn>& items);
// s072: every override on each of `pages` back to the master's -- one step.
bool remove_all_overrides(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
// s072: the master items showing on `page` that it could override (its
// masters' items for its side, not yet overridden), bottom up.
std::vector<Doc::Id> overridable_items(const Doc& d, const Doc::Id& page);
// Back to the master's: the page copies go (their stories with them).
bool remove_overrides(DocJournal& j, Doc& d, const std::vector<Doc::Id>& page_frames);
// Thread `from` -> `to` (`to` goes into the thread right after `from`). `to`
// must be a text frame that is not already threaded; its own text is appended
// to `from`'s story and its old story goes if nothing else uses it. One step.
bool thread_frames(DocJournal& j, Doc& d, const Doc::Id& from, const Doc::Id& to);
// Cut the thread after `from`: the text stays in the frames up to `from`; the
// frames after it are left empty (no story). One step.
bool unthread_after(DocJournal& j, Doc& d, const Doc::Id& from);

// ── images (s009) ──────────────────────────────────────────────────────────
// Put a file into an image frame. The same linked file is one asset however
// often it is placed; the frame's old asset goes if nothing else shows it.
bool    place_image(DocJournal& j, Doc& d, const Doc::Id& frame, const ImageFile& f);
// A new image frame holding the file, at `r` on a page / master. One step.
Doc::Id place_image_new(DocJournal& j, Doc& d, const Doc::Id& owner, const Rect& r, const ImageFile& f);
// Point assets at files (a missing link found, or Update: the same path
// re-read). An embedded asset becomes linked. One step for all of them.
bool    relink_assets(DocJournal& j, Doc& d, const std::vector<std::pair<Doc::Id, ImageFile>>& files);
// Embed: the app has copied each file to <project>/<rel>; the asset now reads
// from there and remembers where it came from. Unembed: linked to that again
// (an embedded asset with no known source can't be). One step each call.
bool    embed_assets(DocJournal& j, Doc& d, const std::vector<std::pair<Doc::Id, std::string>>& rels);
bool    unembed_assets(DocJournal& j, Doc& d, const std::vector<Doc::Id>& assets);
// s035: a picture at `percent` of its own size (300 ppi), about its centre:
// the fit becomes Custom. One step.
bool    set_picture_scale(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, double percent);
// s076: the picture's left (axis 0) or top (1), points from the frame's;
// the fit becomes Custom, starting from where it sits now. One step.
bool    set_picture_offset(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, int axis, double pts);
bool    set_fit(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, ImageFit fit);

// ── masters (s014) ──────────────────────────────────────────────────────────
bool edit_masters(DocJournal& j, Doc& d, const std::vector<Doc::Id>& masters, const std::string& label,
                  const std::function<void(Master&)>& f);

// ── frame settings (s012) ───────────────────────────────────────────────────
// Change frames' settings (columns, gutter, balance ...): `f` edits each;
// one step for all.
// s173: image frames wrap text to their picture's outline -- one step: each
// picked image frame's wrap set to Shape (9 pt air when it had none), and
// the traced outlines (asset -> Asset::shape, from trace_shape) kept on
// their assets. Frames that aren't images are left. False: nothing changed.
bool set_wrap_shape(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames,
                    const std::map<Doc::Id, std::vector<float>>& traced);
bool edit_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, const std::string& label,
                 const std::function<void(Frame&)>& f);
// s228: turning frames. One of the nine places on a frame's turned box
// (0 top left, 1 top, 2 top right, 3 left, 4 centre ... 8 bottom right), on
// its owner.
XY ref_point(const Frame& f, int ref);
// The frames' bounds together (frame_bounds), and their centre: a group's pivot.
Rect frames_bounds(const Doc& d, const std::vector<Doc::Id>& frames);
// s231: a frame as it would be turned by `deg` about (px, py) (rotate_frames' sum).
Frame rotated(const Frame& f, double deg, double px, double py);
// Rotate by `deg` (counter-clockwise) about (px, py) on their owner -- a
// group turns as one, each frame's centre round the pivot. One step "Rotate".
bool rotate_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double deg, double px, double py,
                   const std::string& label = "Rotate");
// Mirror about the vertical (or, `vertical`, horizontal) line through
// (px, py). One step "Flip Horizontal" / "Flip Vertical".
bool flip_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, bool vertical, double px, double py);
// Each frame's rotation and / or shear SET (NAN: leave it), its reference
// place `ref` kept where it is. One step `label`.
bool set_turn(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double rot, double shear, int ref,
              const std::string& label);
// s232: scale WITH contents, by kx / ky along the owner's axes about
// (px, py) -- a group as one. One step `label`.
bool scale_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double kx, double ky, double px, double py,
                  const std::string& label = "Scale");
// Each frame's scale SET (fractions, NAN: leave it), its reference place held.
bool set_scale(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double sx, double sy, int ref,
               const std::string& label = "Scale");
// Degrees in (-180, 180], to 0.01.
double norm_deg(double a);
// s070: stacking order -- the frames among the others on their page (or
// master): to the front / one forward / one backward / to the back. The
// picked keep their order among themselves; forward / backward step past
// ONE frame that isn't picked. z is renumbered 0..n-1 on each owner
// touched. One step; false when nothing moves.
enum class Arrange { Front, Forward, Backward, Back };
bool arrange(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, Arrange how);
// s067: one side of a frame's text inset (text frames) or wrap offset --
// side 0..3 = top / right / bottom / left, -1 = all four ("same all
// round"). Points, 0..500. One step for all the frames.
enum class SideOf { Inset, Wrap };
bool set_side(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, SideOf which, int side, double pts);
// s067: a column's width typed (points): that column takes it, the others
// share what's left in their proportions (text frames with more than `col`
// columns). At least 1 pt each. One step.
bool set_column_width(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, int col, double pts);

// ── text (s010) ─────────────────────────────────────────────────────────────
// THE text verb: replace [a, b) in a story with `text` ('\n' splits a
// paragraph; new paragraphs take the style of the one split -- s037 / s081:
// at a paragraph's end, or within a paste, each new paragraph the Next style
// of the one before). Typing,
// Backspace, Delete, Enter, paste, cut -- all this one verb. A merge key folds
// a burst of keystrokes into ONE step. Returns where the caret goes (just
// after the inserted text); `a` (clamped) when nothing changed.
// s132: what a stretch of text's marks carry -- its notes' words, its
// references' targets, its anchors' ids -- in mark order: copied with the
// text, given back to the marks of a paste (`with`). A pasted anchor keeps
// its id while nothing else has it (a cut, then a paste: what points at it
// still does); else it gets a fresh one (a copy: one place, one id).
struct Carried {
    std::vector<Note> notes;
    std::vector<XRef> refs;
    std::vector<std::string> anchors;
    std::vector<IndexMark> index;   // s134
    bool operator==(const Carried&) const = default;
};
Carried carried_between(const Story& s, TextPos a, TextPos b);
TextPos edit_text(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& text,
                  const std::string& label = "Typing", const std::string& merge_key = {}, const Carried* with = nullptr);
// s109: a footnote at `at` -- its mark in the text, its words `text`. One
// step. Returns the place just after the mark.
TextPos insert_note(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& text);
// s116: a note's words may run to several paragraphs, '\n' between them
// (\r\n and \r read as \n).
std::string note_breaks(const std::string& text);
// The words of paragraph p's k-th note. A merge key folds a burst of typing.
bool set_note_text(DocJournal& j, Doc& d, const Doc::Id& story, int p, int k, const std::string& text,
                   const std::string& merge_key = {});
// The note at mark `at` (a mark's first byte): its paragraph and index; -1 none.
int note_index_at(const Story& s, TextPos at);
// s128: cross-references. An anchor at `at` (a fresh id) -- one step; the
// id. A reference at `at` to anchor `target`, shown as `show` -- one step.
// The anchor at paragraph p's start (made there if it has none) and a
// reference to it at `at` -- one step; false if nothing changed.
std::string insert_anchor(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at);
TextPos     insert_ref(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& target, RefShow show,
                       const std::string& pattern = {});
bool        insert_ref_to(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para, RefShow show,
                          const std::string& pattern = {});
// s129: a reference to paragraph p's k-th note -- an anchor just before its
// mark (made if none is there), the reference at `at` to it. One step.
bool insert_ref_to_note(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para, int k,
                        RefShow show, const std::string& pattern = {});
// s141: a citation of source paragraph to_para (an anchor at its start, made if
// none) at `at`, shown "[n]". One step; false if it isn't a source.
bool insert_citation(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para);
// s143: a citation of several sources at once ("[1, 3]"), with a locator
// ("p. 14"; "" none). One step; false if none of them is a source.
struct SourceAt { Doc::Id story; int para = 0; };
bool insert_citation(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::vector<SourceAt>& sources,
                     const std::string& locator = {});
// s143: one more source in the citation at (para, k) -- one step; false if it
// isn't a citation, isn't a source, or has it already.
bool add_cite_source(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const Doc::Id& to_story, int to_para);
// s130: the reference whose mark is at `at` or ends there: its paragraph,
// index in Para::refs and mark's byte; k -1 none.
struct RefAt { int para = -1, k = -1, byte = -1; };
RefAt ref_near(const Story& s, TextPos at);
// s130: change a reference -- what it shows (and its own words) -- one step;
// false if nothing changed. Point it at paragraph to_para (note_k >= 0: that
// paragraph's note) instead, the anchor made if none -- one step.
bool set_ref(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, RefShow show, const std::string& pattern = {});
bool retarget_ref(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const Doc::Id& to_story, int to_para,
                  int note_k, RefShow show, const std::string& pattern = {});
// s139: the last paragraph a mark in paragraph p covers (p itself for a
// point or its paragraph).
int index_span_end(const Doc& d, const Story& s, int p, const IndexMark& m);
// s139: a heading, as the index and cross-references see one: a paragraph a
// Contents list gathers, or in a style whose name starts "Heading".
bool heading_para(const Doc& d, const Para& p);
// s134: an index entry at `at` -- its mark in the text, its term (trimmed;
// empty: nothing) and sub-entry. One step; the place after the mark
// (`at` when nothing changed).
TextPos insert_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& term,
                          const std::string& sub = {});
TextPos insert_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const IndexMark& m);   // s136
// s134: the index mark at `at` or ending there: its paragraph and index; k -1 none.
struct IndexAt { int para = -1, k = -1, byte = -1; };
IndexAt index_near(const Story& s, TextPos at);
// s134: a mark's term and sub-entry changed (term empty: nothing). One step;
// false if nothing changed.
bool set_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const std::string& term,
                    const std::string& sub = {});
bool set_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const IndexMark& m);   // s136
// Where an anchor is: its story, paragraph and byte; story "" when none.
struct AnchorAt { Doc::Id story; TextPos at; };
AnchorAt find_anchor(const Doc& d, const std::string& id);
// s113: where paragraph p's k-th note's mark is (its first byte); -1 none.
int note_mark_byte(const Story& s, int p, int k);
// s113: a stretch [a, b) of paragraph p's k-th note's words -- replaced by
// `text` (when `set_text`; s116: a line break is a new paragraph; it takes the style
// of the first character replaced), then given character style `cstyle`
// (when not empty; `fold` then runs over the stretch's local formatting).
// One step (or part of the one it runs in). Returns where the stretch ends
// now; -1 when nothing changed.
int change_note_words(DocJournal& j, Doc& d, const Doc::Id& story, int p, int k, int a, int b, bool set_text,
                      const std::string& text, const Doc::Id& cstyle, const std::string& label,
                      const std::function<void(Local&)>& fold = {});

// ── styles (s011) ───────────────────────────────────────────────────────────
// The paragraphs [a.para .. b.para] take a paragraph style. One step.
bool set_para_style(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const Doc::Id& style);
// [a, b) takes a character style ("" = none). One step.
// s030: formatting on the words of [a, b) directly -- `f` changes each
// piece's Local (Clear overrides: f = reset). One step.
bool set_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
               const std::function<void(Local&)>& f, const std::string& merge_key = {});
// s307: the document's language for spell check ("" the app's). One step.
bool set_language(DocJournal& j, Doc& d, const std::string& lang);
// s307: a misspelled word put right -- [from, to) of paragraph `para`, if it
// still reads `was`, becomes `with`. One step ("Spelling").
bool replace_word(DocJournal& j, Doc& d, const Doc::Id& story, int para, int from, int to, const std::string& was,
                  const std::string& with);
// s307: Change All -- every word `was` (whole words, as the checker reads
// them; case as typed) in every story becomes `with`. One step; how many.
int  change_all_words(DocJournal& j, Doc& d, const std::string& was, const std::string& with);
// s303: hyperlinks. [a, b) made a link to `target` ("" takes the link off);
// the target as normal_link gives it. One step ("Hyperlink").
bool set_link(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& target);
// s303: what a typed target means -- trimmed; "www.x" -> "https://www.x";
// "a@b.c" (no scheme) -> "mailto:a@b.c"; "#id" kept (s304: an anchor).
std::string normal_link(const std::string& typed);
// s303: every link's words, story by story, paragraph by paragraph (runs
// side by side with the same target as one); `broken`: a "#" target whose
// anchor is gone (s304).
struct LinkSpan {
    Doc::Id story;
    int para = 0, from = 0, to = 0;   // bytes in that paragraph
    std::string target, words;
    bool broken = false;
    bool operator==(const LinkSpan&) const = default;
};
std::vector<LinkSpan> links_in(const Doc& d, const Doc::Id& story);
std::vector<LinkSpan> hyperlinks(const Doc& d);
// s311: a link to a paragraph -- its anchor (the first at its start, else
// one made there) -- over [a, b). One step.
bool link_to_paragraph(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const Doc::Id& to_story, int to_para);
// s311: a link to a page: "page:" and its id (to its top). links_in calls it
// broken when the page is gone.
std::string page_link(const Doc::Id& page);
// s311: the places a link can go to, in reading order -- headings
// (paragraphs in a style named Heading..., or one a Contents list gathers)
// and paragraphs holding an anchor of their own; their first words.
struct LinkPlace {
    Doc::Id story;
    int para = 0;
    std::string words;
    bool operator==(const LinkPlace&) const = default;
};
std::vector<LinkPlace> link_places(const Doc& d);
// s311: where a link goes, in words: "https://...", "Page 7", "\u201cChapter
// Two\u201d" (an anchor's paragraph), "Gone".
std::string link_where(const Doc& d, const std::string& target);
// s303: the link at a caret / over a whole selection ("" none or mixed).
std::string link_at(const Doc& d, const Doc::Id& story, TextPos a, TextPos b);
// s265: Alt+Left / Right while typing (InDesign's): at a caret, the kern of
// the pair it stands in (on the character before it; not at a paragraph's
// start); a selection, its tracking -- `by` thousandths of an em, presses
// in a row one step. 0 again: the kern goes.
bool nudge_spacing(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, double by);
// s265: Alt+Shift+Up / Down: the selection's (or the caret's word's)
// baseline shift by `by` points, presses in a row one step.
bool nudge_shift(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, double by);
bool set_char_style(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const Doc::Id& style);
// Change styles' settings: `f` edits each; one step for all of them.
// s037: what `f` changes on a based-on style becomes its own, and the
// styles based on the edited ones follow (in the same step).
bool edit_styles(DocJournal& j, Doc& d, const std::vector<Doc::Id>& styles, const std::string& label,
                 const std::function<void(Style&)>& f);
// s037: Based on / Next style.
// The settings a style can take from its base, as keys ("size", "tabs").
const std::vector<std::string>& style_field_keys();
std::string style_field_label(const std::string& key);   // "size" -> "Size"
// The keys where two styles' settings differ.
std::vector<std::string> style_differences(const Style& a, const Style& b);
// `child` takes every setting of `base` it doesn't set itself.
void inherit_from(Style& child, const Style& base);
// The styles based on this one, and on those, ... (each after its base).
std::vector<Doc::Id> style_descendants(const Doc& d, const Doc::Id& style);
// Could `style` be based on `base`? (Both paragraph styles or both
// character styles (s046); not itself, not one based on it.) "" (None)
// always can.
bool can_base_on(const Doc& d, const Doc::Id& style, const Doc::Id& base);
// Base a style on another ("" = none). Its look doesn't change: what
// differs from the new base becomes its own; the rest now follows it. One step.
bool set_style_base(DocJournal& j, Doc& d, const Doc::Id& style, const Doc::Id& base);
// Styles' own settings dropped: all of it from the base again. One step.
bool reset_to_base(DocJournal& j, Doc& d, const std::vector<Doc::Id>& styles);
// s092: just these settings back to the base's (keys from style_field_keys);
// styles that don't own them are left alone. One step; nothing -> no step.
bool reset_style_fields(DocJournal& j, Doc& d, const std::vector<Doc::Id>& styles, const std::vector<std::string>& keys);
// s093: a paragraph's style with its own settings over it (a missing style:
// the default Style). What the layout sets it in.
Style para_style(const Doc& d, const Para& p);
// s093: the paragraphs a..b's own settings -- `f` edits each; or the named
// ones ("align", "left_indent", "right_indent", "first_indent",
// "space_before", "space_after"; s094: "keep_next", "keep_lines",
// "hyphenate"; s157: "tabs") cleared back to the style's. One step;
// no change -> no step.
bool set_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
                    const std::function<void(ParaLocal&)>& f);
bool clear_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::vector<std::string>& keys);
// s157: paragraphs a..b's tab stops -- `stops` (sorted) as their own; the
// same as a paragraph's style's: its own dropped (it follows the style
// again). `label` the step's. One step; no change -> no step.
bool set_para_tabs(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
                   std::vector<TabStop> stops);
// s162: several style edits as ONE step (the inspector's Set) -- `body`
// runs the edits (each a verb as usual); every style is in the step, so
// styles based on the edited ones come back on undo too. True if anything
// changed.
bool styles_as_one(DocJournal& j, Doc& d, const std::string& label, const std::function<void()>& body);
// s164: a style's setting as the inspector's row sends it (key, choice):
// its step's label ("" -- not a plain style setting: based on, resets,
// Duplicate ...), and the change made to one style. The inspector's Set
// and its live specimen (changes waiting, shown before they're set) both
// go through it. A paragraph-only setting leaves a character style alone.
std::string style_edit_label(const std::string& key);
void style_edit(const Doc& d, Style& st, const std::string& key, const std::string& choice);
// s092: the style setting an inspector row edits ("ssize" -> "size",
// "rule_a_w" -> "rule_above"); "" when it isn't one.
std::string style_key_of_edit(const std::string& edit);
// s045: a list paragraph's number (1 + the paragraphs of the same style
// just before it), and its marker ("•", "3.", "iv."; "" not a list).
// s047: paragraphs of other styles: with the list's `continues`, the count
// carries on past them, except a list paragraph of a LOWER level (1 under
// a level-2 list), which restarts it. A list always counts on past its own
// sub-items (a higher level). A paragraph's `restart` sets its number.
int list_number(const Doc& d, const Story& s, int para);
std::string list_marker(const Doc& d, const Story& s, int para);
// s251: the nearest paragraph before `para` in a numbered list a level up (-1 none).
int list_parent(const Doc& d, const Story& s, int para);
// s047: the paragraphs [a.para .. b.para] restart their list at `n` (0:
// count on). One step.
bool set_list_restart(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, int n);
// s055: a column / frame / page break at `at` -- the paragraph split there
// (at a paragraph's start: that paragraph), the one after it starting in the
// next column / frame / page. Anywhere takes a break off the paragraph at
// `at`. One step; returns where the caret goes (the start of the broken
// paragraph).
TextPos insert_break(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, ParaStart kind);
// The Start a paragraph takes: its own break, else its style's.
ParaStart start_of(const Doc& d, const Para& p);
// The style a new paragraph after one in `style` takes (Enter at its end).
Doc::Id next_style_of(const Doc& d, const Doc::Id& style);
// A copy of a style, just after it ("Body copy"). Its id.
Doc::Id duplicate_style(DocJournal& j, Doc& d, const Doc::Id& style);
// Delete styles nothing uses (paragraphs, runs, generated lists). One step.
bool delete_styles(DocJournal& j, Doc& d, const std::vector<Doc::Id>& styles);
// How many paragraphs (paragraph style) / runs (character style) use it.
int style_uses(const Doc& d, const Doc::Id& style);

// ── guides (s006) ───────────────────────────────────────────────────────────
// Pulled out of a ruler onto a page: one step. Dragged: one gesture (the same
// begin / set / end shape as frames); dragged off its page: removed, in the
// same step.
Doc::Id add_guide(DocJournal& j, Doc& d, const Doc::Id& owner, bool vertical, double pos);
void    guide_begin(DocJournal& j, Doc& d, const Doc::Id& guide);
void    guide_set(DocJournal& j, Doc& d, const Doc::Id& guide, double pos);
void    guide_remove_in_gesture(DocJournal& j, Doc& d, const Doc::Id& guide);
bool    remove_guide(DocJournal& j, Doc& d, const Doc::Id& guide);

// s087: Build a generated list -- its entries (title, folio), worked out by
// the layout (where each gathered paragraph lands), written into the list's
// story: the story's first paragraph stays when it's a title (text, no
// tab); then one paragraph per entry, "title<TAB>folio", in the entry style
// (the story's first paragraph with a tab, else its first). A list with no
// story gets one, in a new text frame across its page's margins (no page:
// nothing). Sets built_entries. One step; no change -> no step.
// s088: each entry has its level (1 = the list's first style); a level's
// style is the list's entry_styles[level-1] when it's a paragraph style,
// else level 1's as found, else (deeper levels) a style "<level 1's name>
// <n>" -- found by name or made: based on level 1's, indented 12 pt a level
// (its tab stops pulled in as much, so they stay put on the page).
// s135: an index's entries: page numbers after ", " (not a tab), and level 0
// a letter heading ("H") -- no number, in "<level 1's name> Letter" (found
// by name or made: based on level 1's, bold, 6 pt more space before).
struct ListEntry {
    std::string text, folio;
    int         level = 1;
    std::optional<Para> para;   // s140: a bibliography's source, whole -- its italics, notes, marks kept (its style: the entry style)
    bool operator==(const ListEntry&) const = default;
};
bool build_list(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<ListEntry>& entries);
// s141: the story a list is written into -- its own, or (a bibliography not
// yet built) a story of sources only already on its page; "" none yet.
Doc::Id list_story(const Doc& d, const Doc::Id& list);
// s090: how many paragraphs open a list's story as its title (0 or 1): the
// first, when it has words and no tab and either stands alone or some later
// paragraph is in another style. (Entries may have no tab -- page numbers off.)
int list_title_paras(const Story& st);
// s135: the same for an index -- a first paragraph of one character is its
// first letter heading, not a title.
int list_title_paras(const Story& st, ListKind kind);
// s088: the paragraph styles a list gathers, in level order (others dropped,
// each once). One step; the same -> no step.
bool set_list_styles(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<Doc::Id>& styles);
// s090: a list's settings -- a level's entry style ("" = automatic),
// page numbers on / off. One step; no change -> no step.
bool set_list_entry_style(DocJournal& j, Doc& d, const Doc::Id& list, int level, const Doc::Id& style);
bool set_list_numbers(DocJournal& j, Doc& d, const Doc::Id& list, bool on);
// s142: a bibliography's order -- as first cited in the text, or alphabetical. One step.
bool set_list_cite_order(DocJournal& j, Doc& d, const Doc::Id& list, bool on);
// s135: a new generated list of `kind`, named, placed on page `on` ("" none):
// one step; its id ("" when `on` isn't a page and isn't ""). Built later.
Doc::Id add_list(DocJournal& j, Doc& d, ListKind kind, const std::string& name, const Doc::Id& on);

// The verb table the selftest walks: every verb, runnable on the sample.
struct Verb {
    std::string name;
    std::function<void(DocJournal&, Doc&)> run;
};
const std::vector<Verb>& verbs();

// s052: how many places use a swatch -- frames' fill / stroke, styles'
// colour and rules, words' local colour (each frame, style or run once
// per use). 0 = unused.
int swatch_uses(const Doc& d, const Doc::Id& swatch);

// s058: swatches made and changed. The screen colour (`preview`) is worked
// out from the values: RGB as is, Gray as grey, CMYK and a spot's
// alternate by a simple ink model (the print conversion is the profile's).
std::uint32_t swatch_preview(const Swatch& s);
// s107: the colour a swatch is DRAWN in on screen (canvas, thumbnails, the
// screen PDF, the panels' chips): its preview -- except a CMYK swatch (or
// a spot's CMYK alternate) when the app has a press profile, then the
// profile's look of those inks (set_screen_cmyk). Any thread.
std::uint32_t screen_rgb(const Swatch& s);
using ScreenCmyk = std::function<std::uint32_t(const double cmyk[4])>;
void set_screen_cmyk(ScreenCmyk f);   // {} = the simple ink model (the preview)
std::uint64_t screen_gen();          // moves with each set_screen_cmyk: caches of drawn pages key on it
// The values as 0xRRGGBB and back (for switching a swatch's model).
void swatch_from_rgb(Swatch& s, std::uint32_t rgb);
// New (after the last swatch), changed (its preview made from its values),
// duplicated ("<name> copy"); each one step. Delete: only swatches nothing
// uses (false if any is used, nothing changed).
Doc::Id add_swatch(DocJournal& j, Doc& d, Swatch s);
// s198: the Eyedropper tool -- a colour taken from the page (0xRRGGBB, as
// it prints) onto frames' fill, or their stroke: the swatch already showing
// it (within a step of each channel), else a new RGB swatch named by its
// hex. No frames: just the swatch (found or made). One step (none when
// nothing changes). Returns the swatch.
Doc::Id take_colour_onto(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, std::uint32_t rgb, bool stroke);
bool    edit_swatch(DocJournal& j, Doc& d, const Doc::Id& id, Swatch s);
Doc::Id duplicate_swatch(DocJournal& j, Doc& d, const Doc::Id& id);
bool    delete_swatch(DocJournal& j, Doc& d, const Doc::Id& id);
// s179: the swatches nothing uses -- Black (text's default) and Paper (the
// page) never count -- in the Colors order; and all of them gone in one
// step. How many went.
std::vector<Doc::Id> unused_swatches(const Doc& d);
int     delete_unused_swatches(DocJournal& j, Doc& d);

// ── Find / Change (s022) ────────────────────────────────────────────────────
// What to look for: text (in one paragraph), optionally only in a
// paragraph style and / or wholly in a character style. No text and a
// style: each paragraph in that style (or each run of that character
// style) is a hit. s091: case folds beyond ASCII -- fold_char's letters
// (Latin, Greek, Cyrillic, Armenian, fullwidth), one for one.
// s091: a character's simple case fold (lower case, one for one): ASCII,
// Latin-1, Latin Extended-A / B / Additional, Greek (final sigma as sigma),
// Cyrillic, Armenian, fullwidth A-Z. Anything else is itself.
char32_t fold_char(char32_t c);
// s152: full case folding -- a whole string as fold_char, and the letters
// whose fold is more than one: sharp s (both cases) "ss", the ligatures
// ff fi fl ffi ffl st as their letters, n-apostrophe, the Armenian
// ligatures. Find and Change match through it ("STRASSE" finds "Straße"),
// a hit never starting or ending inside one character.
std::string fold_text(const std::string& utf8);

struct FindOpts {
    std::string what;
    bool        match_case = false;
    bool        whole_word = false;
    Doc::Id     pstyle;   // "" = any
    Doc::Id     cstyle;   // "" = any
    // s051: only words set bold / italic (or both) -- by local formatting
    // or their character style, not the paragraph style. No text: each
    // stretch of such words is a hit. `skip_cstyle`: leave out words
    // already in this character style (what Apply is about to put on).
    bool        bold = false, italic = false;
    Doc::Id     skip_cstyle;
    // s160: bold / italic set by the paragraph style count too (words in a
    // bold heading style are "bold"); off: only by hand or character style.
    bool        para_counts = false;
    // s056: how `what` reads -- plain text; Wildcards (^9 a digit, ^$ a
    // letter, ^w white space, ^? any character, ^^ a caret; the rest
    // plain); or a regular expression (ECMAScript; "." is one character;
    // s091: case folded as fold_char, both sides). A pattern finds within
    // one paragraph.
    enum class Mode { Text, Wildcards, Regex };
    Mode        mode = Mode::Text;
    // s056: where -- every story (""), one story, or a stretch of it
    // (`from` .. `to`, when `in_range`).
    Doc::Id     story;
    bool        in_range = false;
    TextPos     from, to;
};
struct Hit {
    Doc::Id story;
    TextPos a, b;
    // s056: a pattern's groups ($0 the whole match, $1 ...), for Change to.
    std::vector<std::string> groups;
    // s113: a hit in a footnote's words -- paragraph a.para's note-th note,
    // a.byte .. b.byte in its words. -1: in the text.
    int note = -1;
    bool operator==(const Hit&) const = default;
};
// s056: is `f.what` a pattern that reads (Wildcards / Regex)? `err` says
// what's wrong with it. Plain text always reads.
bool find_pattern_ok(const FindOpts& f, std::string* err = nullptr);
// s056: `with` with a hit's groups put in: $1..$9, $& (the whole match),
// $$ (a dollar). A group the pattern hasn't is left as written.
std::string expand_groups(const std::string& with, const std::vector<std::string>& groups);
// Every hit, in reading order: stories by the first page their thread
// starts on (masters', then unplaced, after), then by position.
std::vector<Hit> find_all(const Doc& d, const FindOpts& f);
// s310: the stories in the order they read -- by the page their thread
// starts on (masters', then unplaced, after).
std::vector<Doc::Id> stories_in_order(const Doc& d);
// s310: Check Spelling through the book. The next misspelled word after
// (story, from) -- the rest of that story, then the stories after it in
// reading order, then round from the first to where it began; story ""
// none (or no dictionary). `from` past the word the caret is in.
struct SpellHit {
    Doc::Id story;
    int para = -1, from = 0, to = 0;
    std::string word;
    bool operator==(const SpellHit&) const = default;
};
SpellHit next_misspelling(const Doc& d, const Doc::Id& story, TextPos from);
// s310: how many misspelled words, and in how many stories.
struct SpellCount { int words = 0, stories = 0; };
SpellCount misspellings(const Doc& d);
// What to do to a hit: put `with` in its place (when set_text), and / or
// apply a paragraph or character style to what's there afterwards.
struct ChangeOpts {
    bool        set_text = true;
    std::string with;
    Doc::Id     pstyle;   // "" = leave
    Doc::Id     cstyle;   // "" = leave
    // s051: with a character style applied, local bold / italic that says
    // what the style says goes, so the style alone carries it (a later
    // change to the style is seen on those words too).
    bool        fold_local = true;
    // s056: `with` takes the hit's groups ($1 ...) -- a pattern's Change to.
    bool        expand = false;
};
// s095: what a hit is changed to -- `with`, its groups put in (expand),
// "^p" as a paragraph break.
std::string change_text(const ChangeOpts& c, const Hit& h);
// Change the hits -- ONE step ("Change" / "Change All (n)"). The hits are
// done last to first, so the earlier ones keep their places. Returns how
// many changed.
int change_hits(DocJournal& j, Doc& d, std::vector<Hit> hits, const ChangeOpts& c, const std::string& label);

}  // namespace doc

// ── the canvas's view of the document (s005) ───────────────────────────────
// Opened pages (or masters) laid out left to right at true size, tops aligned,
// `gap` points apart. Doc space = this layout; a frame's own x / y are in its
// owner's coordinates.
struct Placed {
    Doc::Id id;
    Rect    r;
    bool    master = false;
    // s150 (Scott): a facing master opens as its spread -- a Left and a
    // Right panel, the same master, meeting at the spine. Any: a page, or
    // a single master.
    Side    side = Side::Any;
};
// s150: does frame `fid` show on placed `p`? A page / single master: its
// own frames all do; a facing master's panel: the items for its side and
// those on both (Any).
bool on_panel(const Doc& d, const Placed& p, const Doc::Id& fid);
// s150: the placed page / panel under (x, y); null over the pasteboard.
const Placed* panel_at(const std::vector<Placed>& pl, double x, double y);
std::vector<Placed> layout_pages(const Doc& d, const std::vector<Doc::Id>& ids, double gap = 36);
inline Rect frame_rect(const Frame& f) { return {f.x, f.y, f.w, f.h}; }
// s228: a frame's turn -- an affine map (cairo's / PDF's cm order: x' = xx x
// + xy y + x0, y' = yx x + yy y + y0) from its box's coordinates (owner,
// before the turn) to the owner's.
struct Xf {
    double xx = 1, yx = 0, xy = 0, yy = 1, x0 = 0, y0 = 0;
    void apply(double x, double y, double& ox, double& oy) const { ox = xx * x + xy * y + x0; oy = yx * x + yy * y + y0; }
    Xf inverse() const;
    Xf then(const Xf& b) const;   // this, then b
    bool identity() const { return xx == 1 && yx == 0 && xy == 0 && yy == 1 && x0 == 0 && y0 == 0; }
};
bool turned(const Frame& f);   // rotated, sheared, flipped or scaled (s232)
// s232: the frame put through an affine map of its owner `a` -- its centre
// moved by it, its turn's linear part taken by it, split back into rotation,
// shear, flip and scale (any map with an inverse is one). Its box (w, h)
// stays; only its place and its turn change.
Frame transformed(const Frame& f, const Xf& a);
// A map of the owner: about (px, py), by the linear part (xx yx xy yy).
Xf about(double px, double py, double xx, double yx, double xy, double yy);
Xf   frame_xf(const Frame& f);  // identity when not turned
// Owner coordinates -> the frame's box coordinates (before the turn), and back.
void to_box(const Frame& f, double& x, double& y);
void from_box(const Frame& f, double& x, double& y);
// Where the frame reaches on its owner, turned: its box's corners (or a
// shape's outline) mapped, their bounds. frame_rect when not turned.
Rect frame_bounds(const Frame& f);
// s229: resizing a turned frame by one of its handles -- (dx, dy) the
// pointer's move on the owner, taken along the frame's own axes; the
// opposite handle (Alt: its centre) stays put on the page. The new box.
Rect turned_resize(const Frame& f0, int handle, double dx, double dy, bool shift, bool alt);
// s233: a frame SCALED by one of its corner handles (contents and all) --
// the pointer's move (dx, dy, owner) taken along its axes; the opposite
// corner held (Alt: the centre); Shift keeps its proportions.
Frame scaled_by_handle(const Frame& f0, int handle, double dx, double dy, bool shift, bool alt);
// s233: a frame SKEWED by dragging just outside one of its edges' middles
// (handle N / E / S / W): that edge slides along itself by the pointer's
// move, the opposite edge held.
Frame skewed_by_edge(const Frame& f0, int handle, double dx, double dy);
// The 8 handle places of a frame's turned box on its owner (Handle order NW..W).
std::array<std::array<double, 2>, 8> turned_handles(const Frame& f);
// s012: a text frame's columns -- how many, each one's width and left edge
// (frame coordinates), and which column an x falls in (the nearest). Widths
// are the frame's width less the gutters, shared by the columns' weights
// (equal when no weights are set), so they keep their proportions on resize.
int    column_count(const Frame& f);
double column_width(const Frame& f, int col);
double column_x(const Frame& f, int col);
int    column_at(const Frame& f, double x);
// The weights for `n` columns from `f` (its own when they fit, else equal).
std::vector<double> column_weights(const Frame& f, int n);
// s014: a page's (or master's) margins and columns, from its master; in its
// own coordinates. A left-hand page mirrors inside / outside. Empty when it
// has no master (no margins to show).
Rect margin_rect(const Doc& d, const Doc::Id& page_or_master);
// s150: a master's panel -- `side` mirrors it as a page there would be.
Rect margin_rect(const Doc& d, const Doc::Id& page_or_master, Side side);
// s062: the margins a page (or master) uses -- its own, else its master's.
// Null with neither.
const Margins* margins_of(const Doc& d, const Doc::Id& page_or_master);
struct ColumnSpan { double x0, x1; };
std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& page_or_master);
std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& page_or_master, Side side);   // s150
// s036: the baseline grid on a page (or master), its own coordinates:
// lines at start + k * step (k >= 0). step 0 = none (no master, or none set).
struct BaselineGrid { double start = 0, step = 0; };
BaselineGrid baseline_grid(const Doc& d, const Doc::Id& page_or_master);
// The page's column count and gutter (1 / 12 with no master).
std::pair<int, double> page_column_setup(const Doc& d, const Doc::Id& page_or_master);
// s023: special characters in text, standing for what each page fills in.
// Private-use code points (3 bytes of UTF-8 each), so they travel in the
// text like any character. While editing (and on a master) they show as a
// sign of the same byte length; drawn on a page they become the value.
inline constexpr const char* kMarkPage    = "\xEE\x80\x80";   // U+E000: this page's number
inline constexpr const char* kMarkSection = "\xEE\x80\x81";   // U+E001: this page's section name
inline constexpr const char* kMarkNext    = "\xEE\x80\x82";   // U+E002: the next frame's page number (jump lines)
inline constexpr const char* kMarkPrev    = "\xEE\x80\x83";   // U+E003: the previous frame's page number
// s109: a footnote's reference: drawn as its number, raised; the note is the
// paragraph's notes[k] for the k-th of these in its text.
inline constexpr const char* kMarkNote    = "\xEE\x80\x84";   // U+E004
inline constexpr const char* kMarkRef     = "\xEE\x80\x85";   // U+E005: s128, a cross-reference (Para::refs)
inline constexpr const char* kMarkAnchor  = "\xEE\x80\x86";   // U+E006: s128, a place references point at (Para::anchors)
inline constexpr const char* kMarkIndex   = "\xEE\x80\x87";   // U+E007: s134, an index entry (Para::index)
// s239 (H1): the document's metadata, filled in where it's drawn.
inline constexpr const char* kMarkTitle   = "\xEE\x80\x88";   // U+E008: the document's title
inline constexpr const char* kMarkAuthor  = "\xEE\x80\x89";   // U+E009: its author
// s240 (H2): running heads -- the words of the chapter's heading (and its
// subtitle) in force on the page: the first on the page, else the last before it.
inline constexpr const char* kMarkChapter = "\xEE\x80\x8A";   // U+E00A: the chapter's name
inline constexpr const char* kMarkSubhead = "\xEE\x80\x8B";   // U+E00B: the chapter's subtitle
// s240: the paragraph style a running head (0 chapter, 1 subtitle) follows -- the
// document's choice, else by name; "" when there's none.
std::string running_style(const Doc& d, int slot);
bool note_at(const std::string& text, std::size_t i);
int  notes_in(const std::string& text, std::size_t from = 0, std::size_t to = std::string::npos);   // the marks in [from, to)
// The number the k-th note of paragraph p takes (1-based, through the story).
int  note_number(const Story& s, int p, int k);
// s124: the same, as the document numbers it -- counting again from each
// chapter's start (a paragraph that starts a new page) when the footnotes
// count by chapter; else through the story.
int  note_number(const Doc& d, const Story& s, int p, int k);
// s141: a source's number -- paragraph p of story s counted among the
// story's sources (paragraphs in a style a Bibliography list gathers), from
// 1; 0 when it isn't one. A citation (RefShow::Cite) shows it: "[3]".
int source_number(const Doc& d, const Story& s, int p);
// The paragraph's notes made to match its marks (missing ones empty, extras dropped).
void fit_notes(Para& p);
// Which marker starts at text[i] (0 page, 1 section, 2 next, 3 previous;
// s128: 5 a cross-reference, 6 an anchor; s134: 7 an index entry; -1 none -- a footnote's mark is
// note_at's).
int marker_at(const std::string& text, std::size_t i);
// s128: how a marker shows where its value isn't filled in (the editors, a
// flow for places): a sign of the same three bytes. An anchor: a word
// joiner (nothing to see).
const char* marker_sign(int k);
// s128: how many of `mark` (a three-byte marker) are in text[from, to).
int marks_in(const std::string& text, const char* mark, std::size_t from = 0, std::size_t to = std::string::npos);
// s128: a paragraph's refs and anchors (s134: and index marks) made to match its marks.
void fit_marks(Para& p);
bool has_markers(const Story& s);
// s026: the width text is usually set at -- the trim less the first
// master's inside and outside margins (tab presets measure from it).
double text_width(const Doc& d);
// A style's tab stops in a few words ("None", "Right 4.5 in, dots", "9 stops").
std::string tabs_text(const std::vector<TabStop>& tabs);
// The Tabs presets the inspector offers: {id, label}, and the stops for an id.
std::vector<std::pair<std::string, std::string>> tab_presets(const Doc& d);
std::vector<TabStop> tab_preset(const Doc& d, const std::string& id);
// s021: a swatch's screen colour at a tint (mixed toward paper white).
std::uint32_t tint_rgb(std::uint32_t rgb, double tint);
// The tints offered (percent), and how a tint reads ("40%").
const std::vector<int>& tint_steps();
std::string tint_text(double tint);
// s018: what pushes a text frame's lines aside -- every OTHER frame with a
// wrap on its owner and on the owner's masters, grown by its offsets, in the
// text frame's coordinates. Stacking doesn't matter (InDesign's default)
// unless the document says wrap pushes only text beneath (s069: then only
// frames ABOVE the text frame -- masters below their pages, then by z).
// A text frame set to ignore wrap gets none (s069).
struct WrapRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool   jump = false;
};
std::vector<WrapRect> wrap_rects(const Doc& d, const Doc::Id& text_frame);
// s173: a picture's outline from its pixels -- 32-bit ARGB (cairo's
// ARGB32: 0xAARRGGBB, premultiplied is fine), `stride` pixels a row. Solid
// is: with any see-through pixel, alpha at least half; else, when its four
// corners are one colour (a product shot on white), whatever differs from
// that colour; else nothing to trace (empty). Up to `bands` row bands
// (fewer for a short picture). The Asset::shape layout.
std::vector<float> trace_shape(const std::uint32_t* px, int w, int h, int stride, int bands = 96);
// s189 (G3): a picture's Photoshop CLIPPING PATH -- the named path its
// resources (8BIM blocks) mark as the clipping path (resource 2999), from a
// PSD's image resources, a JPEG's APP13 ("Photoshop 3.0") segments or TIFF
// tag 34377. Each closed subpath's Bezier knots flattened to points, as
// fractions of the picture (x of its width, y of its height). Empty when
// it has none. `read` as picture_color's.
struct ClipPath {
    std::string name;
    std::vector<std::vector<std::array<double, 2>>> polys;   // closed, (x, y) 0..1
};
ClipPath clipping_path(const ReadAt& read);
// s227 (G3): every path its resources hold (2000 - 2997), in their order,
// named -- the clipping path among them.
std::vector<ClipPath> picture_paths(const ReadAt& read);
// The clipping path as a Shape wrap's outline (the Asset::shape layout):
// filled even-odd into a mask of the picture's proportions (w x h, any
// unit), then traced as trace_shape traces. Empty when there's no path.
std::vector<float> clip_shape(const ClipPath& p, double w, double h, int bands = 96);
// s189: a clipping path packed for Asset::clip, and back -- as placed, in
// the picture's rect `pr` (whatever its units).
std::vector<float> pack_clip(const ClipPath& p);
std::vector<std::vector<std::array<double, 2>>> clip_polys(const Asset& a, const Rect& pr);
// s227: the frame's choice of path -- its packed points (empty: none), its
// wrap shape (Asset::shape's layout; empty: none), and the polygons placed.
const std::vector<float>& frame_clip(const Frame& f, const Asset& a);
const std::vector<float>& frame_shape(const Frame& f, const Asset& a);
std::vector<std::vector<std::array<double, 2>>> clip_polys(const Frame& f, const Asset& a, const Rect& pr);
// s173: a frame's traced picture as placed in it (fit / crop), in the
// frame's coordinates, cut to the frame: each band's solid span. Empty when
// the asset has no shape.
struct Band { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
std::vector<Band> shape_bands(const Frame& f, const Asset& a);
// s200: a shape's outline as wrap bands (frame coordinates), each `step`
// points tall, grown by half its stroke -- for a Shape wrap.
std::vector<Band> outline_bands(const Frame& f, const Doc& d, double step = 3);
// s027: spreads -- facing pages pair a left page with the right one after
// it (a book's first page stands alone on the right); without facing
// pages each page is its own. In page order.
std::vector<std::vector<Doc::Id>> spreads(const Doc& d);
// s147: a page's (or master's) bleed -- its master's; a page with none: the document's.
double bleed_of(const Doc& d, const Doc::Id& owner);
// s147: any master with facing pages (the views group by spread only then).
bool any_facing(const Doc& d);
std::vector<Doc::Id> spread_of(const Doc& d, const Doc::Id& page);
// The masters a page shows, most basic first (a master's based-on chain).
std::vector<Doc::Id> master_chain(const Doc& d, const Doc::Id& page_or_master);
// s054: a page range as typed -- folios as the pages show them, commas
// between, a hyphen (or en dash) for a run: "iii-7, 12". Empty or "all":
// every page. The pages in document order, each once; an unknown folio or a
// run that goes backwards is an error (empty result, `err` says which).
std::vector<Doc::Id> pages_in_range(const Doc& d, const std::string& text, std::string* err = nullptr);
// A frame's rect in layout (doc) space; empty if its owner isn't placed.
Rect frame_in_layout(const Doc& d, const std::vector<Placed>& pl, const Doc::Id& frame);
// The topmost of a placed page's OWN frames under (x, y), within `tol`
// points; "" if none. Master items are not picked on a page (as InDesign).
Doc::Id hit_frame(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol);
// s025: does master item `fid` show on `page`? (Its side matches the page's,
// and the page hasn't overridden it.) Any non-page: true.
bool master_item_on(const Doc& d, const Doc::Id& fid, const Doc::Id& page);
// s155: an item across the spine on a page / panel of side `side`: how far
// it sits from its own x (a right page: minus the master's width), and the
// spine it is cut at -- Left: its right edge, Right: its left (`cut_left`
// true). 0 / false for anything else.
double across_dx(const Doc& d, const Frame& f, Side side);
// The side an item is drawn for on a page (its side) or a master panel.
Side side_for(const Doc& d, const Doc::Id& page_or_master, Side panel);
// The topmost master item showing on a placed PAGE under (x, y) -- what
// Ctrl+Shift+click overrides. "" if none.
Doc::Id hit_master_item(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol, Doc::Id* page);
// s169: the master items showing on placed pages that `r` touches (an item
// across the spine: on each page its part), each with its page.
std::vector<MasterItemOn> master_items_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r);
// Every own frame touched by `r` (touching selects -- Curvz's marquee rule).
std::vector<Doc::Id> frames_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r);
// s300: `ids` with every frame grouped with any of them (in the document's order, the given first).
std::vector<Doc::Id> with_groups(const Doc& d, const std::vector<Doc::Id>& ids);
// Guides on a page or master (its own; master_chain gives the inherited).
std::vector<Doc::Id> guides_on(const Doc& d, const Doc::Id& owner);
// The guide of a placed page / master (its OWN, not a master's) within `tol`
// points of (x, y); "" if none.
Doc::Id hit_guide(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol);
// Which placed page / master contains (x, y); "" over the pasteboard.
Doc::Id page_at(const std::vector<Placed>& pl, double x, double y);

// ── snapping (s006) ─────────────────────────────────────────────────────────
// Candidate lines (layout coords), gathered ONCE at press (Curvz's
// gather-at-drag-start): page edges + centres, folds, guides (own and
// master's), and the edges + centres of every frame not being dragged.
// s153: `exclude` leaves out guides too (a dragged guide snaps to the rest).
struct SnapTargets {
    std::vector<double> xs, ys;
};
SnapTargets snap_targets(const Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& exclude);
// The nearest target to any of `vs` within `tol`: the correction to add, and
// the line it snapped to. {0, nan} when nothing is close enough.
struct SnapHit {
    double delta = 0;
    double line  = std::numeric_limits<double>::quiet_NaN();
    bool   hit() const { return !std::isnan(line); }
};
SnapHit snap_any(const std::vector<double>& vs, const std::vector<double>& targets, double tol);

// ── the rulers' zero (s154) ─────────────────────────────────────────────────
// Where the zero is, from a page's (or a master panel's) top-left, points:
// the origin (Spread: the spread's first page; Page: the page; Spine: the
// spine -- a left page's right edge, a right page's left edge) plus the
// dragged zero point. The inspector's X / Y and the rulers read from it.
struct Zero { double x = 0, y = 0; };
Zero ruler_zero(const Doc& d, const Doc::Id& owner, Side side = Side::Any);
// A horizontal ruler in pieces: each placed page measures from its zero
// (layout x); neighbours sharing a zero (a spread) are one piece. `lo` /
// `hi` tile the line (the first from -inf, the last to +inf, a boundary
// halfway across a gap).
struct RulerSpan { double lo, hi, zero; };
std::vector<RulerSpan> ruler_spans(const Doc& d, const std::vector<Placed>& pl);
// The zero point a press at (x, y) (layout) would set -- for set_ruler_zero.
Zero zero_at(const Doc& d, const std::vector<Placed>& pl, double x, double y);

// A thread's position: (1-based index, length); (0, 0) for non-text frames.
std::pair<int, int> thread_place(const Doc& d, const Doc::Id& frame);

// The built-in sample (a small illustrated book) -- what the app opens to
// before a project is opened, and the selftest's fixture. Loaded, not undoable.
void load_sample(Doc& d);

// ── a new document (s019) ───────────────────────────────────────────────────
// What File > New asks for. Points throughout.
struct NewDoc {
    std::string title   = "Untitled";
    double      trim_w  = 6 * kPt, trim_h = 9 * kPt;
    int         pages   = 16;
    bool        facing  = true;
    Margins     margins{0.75 * kPt, 0.75 * kPt, 0.75 * kPt, 0.75 * kPt};
    int         columns = 1;
    double      gutter  = 12;
    double      bleed   = 0.125 * kPt;
    bool        text_frames = true;   // one story threaded through a frame in every page's margins
    bool        folios = true;        // s023: the page number at the foot, on the master
    Units       units = Units::Inches; // s079: the new document's (and the sheet's) unit
    // s276: header / footer presets on the master -- the folio at the outside
    // corners (else centred); a running head: 0 none, 1 the title on left
    // pages and the chapter on right (one-sided: the title), 2 the chapter's
    // name, 3 the title.
    bool        folio_outside = false;
    int         head = 0;
    bool operator==(const NewDoc&) const = default;
};
// s079: the New sheet's choices between runs, in the app's settings
// ("new.trim_w" ... points; the name isn't kept). Reading is tolerant: a
// missing or unreadable key keeps NewDoc's default, numbers held to sane
// ranges.
void   new_to_settings(const NewDoc& n, AppSettings& s);
NewDoc new_from_settings(const AppSettings& s);
// Common book trims (the first is the default).
struct TrimPreset { std::string label; double w, h; };
const std::vector<TrimPreset>& trim_presets();
// "" when `n` makes a usable document, else why not (for the sheet to say).
std::string check_new(const NewDoc& n);
// Replace `d` with a blank document (loaded: not a step): settings, one
// master with the margins and columns, the pages, one section, Black and
// Paper, Body / Heading / Caption / Emphasis, and -- with text_frames -- an
// empty story threaded through every page. False (d untouched) if
// check_new refuses.
bool new_document(Doc& d, const NewDoc& n);

// ── the project folder: <name>/document.json + assets/ ─────────────────────
// `from_dir`: the folder it was saved in before (Save As): its embedded files
// are copied across so the new folder is whole.
bool        save_project(const Doc& d, const std::string& dir, std::string* err = nullptr,
                         const std::string& from_dir = {});
bool        load_project(Doc& d, const std::string& dir, std::string* err = nullptr);
// s064: what the start screen says about a project folder -- its title,
// trim, page count, when document.json was last written (Unix seconds),
// and its first page (for drawing). ok false: no readable document there.
struct ProjectSummary {
    bool        ok = false;
    std::string title;
    double      trim_w = 0, trim_h = 0;
    int         pages = 0;
    long long   saved = 0;
    Units       units = Units::Inches;   // s096: the document's own (its card says its size in them)
};
ProjectSummary summarize_project(const std::string& dir, Doc* keep = nullptr);
// s097: files in a project's assets/ folder no asset uses -- not the
// document's, nor any path in `also_used` (what undo could bring back) --
// by path ("<dir>/assets/x.png") and size, sorted. Folders inside are left
// alone. No folder: none.
struct UnusedFile {
    std::string   path;
    std::uintmax_t bytes = 0;
    bool operator==(const UnusedFile&) const = default;
};
std::vector<UnusedFile> unused_asset_files(const Doc& d, const std::string& dir, const std::vector<std::string>& also_used = {});
// "512 bytes", "12 KB", "2.4 MB".
std::string bytes_text(std::uintmax_t n);
// "just now", "12 minutes ago", "3 hours ago", "yesterday", "4 days ago",
// then the date ("Oct 3, 2026"). Local time for the date.
std::string ago_text(long long now, long long then);
std::string to_json(const Doc& d);                       // the document.json text
bool        from_json(Doc& d, const std::string& text, std::string* err = nullptr);

}  // namespace composix::core
