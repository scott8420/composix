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
#include <string>
#include <variant>
#include <vector>

namespace composix::core {

inline constexpr double kPt = 72.0;   // points per inch

enum class Numbering { Arabic, RomanLower, RomanUpper, AlphaLower, AlphaUpper };
enum class Side      { Any, Right, Left };
enum class FrameKind { Text, Image, Shape };
enum class StyleKind { Paragraph, Character };
enum class Align     { Left, Center, Right, Justify };
enum class ColorModel{ CMYK, RGB, Gray, Spot };
enum class ListKind  { Contents, Bibliography, Index };
enum class ImageFit  { Fill, Fit, Custom };   // s009: cover the frame (cropped) / show it whole; s035: placed by hand
// s018: how a frame pushes text in OTHER frames aside. Around: text runs
// down both sides of it; Jump: text skips the band it covers, no lines
// beside it.
enum class Wrap      { None, Around, Jump };
enum class VAlign    { Top, Center, Bottom, Justify };   // s042: a text frame's text, up and down
// s043: where a paragraph (in this style) may start.
enum class ParaStart { Anywhere, NextColumn, NextFrame, NextPage, NextRightPage };

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
enum class NoteRestart { Story, Page, Section };
enum class NoteStyle   { Arabic, RomanLower, AlphaLower, Symbols };

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

struct Frame {
    std::string owner;                   // page or master id
    FrameKind   kind = FrameKind::Text;
    double      x = 0, y = 0, w = 0, h = 0, rot = 0;
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
    ColumnRule  col_rule;                // s055: text -- a rule between its columns
    bool        ignore_wrap = false;     // s069: text -- other frames' wrap doesn't push this text
    bool operator==(const Frame&) const = default;
};

// A character style over bytes [start, end) of a paragraph (s011). A
// paragraph's runs are sorted, never overlap, never empty; text outside any
// run is in the paragraph style alone.
// s030: formatting set on words directly, over their styles: -1 / 0 /
// "" = leave it to the styles.
struct Local {
    int         bold = -1, italic = -1;   // -1 the styles', 0 off, 1 on
    double      size = 0;                 // points
    std::string color;                    // a swatch id
    bool empty() const { return bold < 0 && italic < 0 && size <= 0 && color.empty(); }
    bool operator==(const Local&) const = default;
};

struct Run {
    int         start = 0, end = 0;
    std::string style;                   // a character style id ("" with local formatting only)
    Local       local;                   // s030
    bool operator==(const Run&) const = default;
};

// s093: a paragraph's own settings over its style's -- each unset (the
// style's) or set. Kept when the style changes; cleared one by one.
struct ParaLocal {
    std::optional<Align>  align;
    std::optional<double> left_indent, right_indent, first_indent, space_before, space_after;
    std::optional<bool>   keep_next, hyphenate;   // s094
    std::optional<int>    keep_lines;             // s094: 0 off, 2 / 3, -1 all together
    bool empty() const {
        return !align && !left_indent && !right_indent && !first_indent && !space_before && !space_after && !keep_next &&
               !hyphenate && !keep_lines;
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
    bool operator==(const Para&) const = default;
};

struct Story {
    std::string       name;
    std::vector<Para> paras;
    bool operator==(const Story&) const = default;
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
    int         level = 1;
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
    // (headings). `keep_lines`: 0 none; N (2, 3): at least N lines at a
    // paragraph's start and at its end (no orphans / widows); -1 the whole
    // paragraph together.
    bool        keep_next = false;
    int         keep_lines = 0;
    // s043: start in the next column / frame / page / right-hand page when
    // something is already set there (a chapter's opening heading).
    ParaStart   start = ParaStart::Anywhere;
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
    bool operator==(const Style&) const = default;
};

struct Swatch {
    std::string           name;
    double                pos   = 0;
    ColorModel            model = ColorModel::CMYK;
    std::array<double, 4> v{};          // CMYK 0-100 / RGB 0-255 / gray 0-100
    std::string           spot;          // the ink's name, for Spot
    std::uint32_t         preview = 0x808080;   // what the screen shows (spot / CMYK approximation)
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

using Record = std::variant<Settings, Section, Page, Master, Frame, Story, Style, Swatch, Asset, GenList, Guide>;

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
    std::vector<Id> frames_on(const Id& owner) const;   // by z

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
};
// Where an asset's file is: an absolute path, or "" when it can't be known (a
// relative path in an unsaved project).
std::string  resolve_asset(const Asset& a, const std::string& project_dir);
std::int64_t file_stamp(const std::string& path);    // 0 = no such file
enum class LinkStatus { OK, Modified, Missing, Embedded, EmbeddedMissing };
LinkStatus   link_status(const Asset& a, const std::string& project_dir);
std::string  link_status_text(LinkStatus s);
// The picture's rect inside a frame (both in the same coordinates): Fill
// covers the frame (cropped by it), Fit shows it whole. iw / ih: its aspect.
Rect   fit_rect(const Rect& frame, double iw, double ih, ImageFit fit);
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

namespace doc {

// Size presets the inspector offers for a page: the trim, a 2-panel and a
// 3-panel foldout (each fold-in panel 1/8 in narrower, so it tucks inside).
struct SizePreset { std::string label; double w, h; std::vector<double> folds; };
std::vector<SizePreset> size_presets(const Settings& s);

std::vector<Doc::Id> add_pages(DocJournal& j, Doc& d, const Doc::Id& after, int count,
                               const Doc::Id& master);
bool remove_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
bool move_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& after);
bool set_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& master);
// s066: the document's own settings, one step each. The title (trimmed,
// not empty); the trim -- every page and master at the old trim height
// follows, its width (and a foldout's folds) in proportion; the bleed
// (points, >= 0); facing pages on / off.
bool set_title(DocJournal& j, Doc& d, const std::string& title);
bool set_trim(DocJournal& j, Doc& d, double w, double h);
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
// s073: the document's display unit. One step.
bool set_units(DocJournal& j, Doc& d, Units u);
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
Doc::Id add_frame(DocJournal& j, Doc& d, const Doc::Id& owner, FrameKind kind, const Rect& r,
                  int columns = 1, double gutter = 12);
// Delete frames. A text frame leaving a thread is spliced out (the frame
// before it points past it) so the story reflows through what is left. A
// story whose LAST frame goes goes with it, and so does an image whose last
// placement goes (s009, as InDesign). One step.
bool delete_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames);
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
bool edit_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, const std::string& label,
                 const std::function<void(Frame&)>& f);
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
TextPos edit_text(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& text,
                  const std::string& label = "Typing", const std::string& merge_key = {});
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
               const std::function<void(Local&)>& f);
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
// "hyphenate") cleared back to the style's. One step;
// no change -> no step.
bool set_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
                    const std::function<void(ParaLocal&)>& f);
bool clear_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::vector<std::string>& keys);
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
struct ListEntry {
    std::string text, folio;
    int         level = 1;
    bool operator==(const ListEntry&) const = default;
};
bool build_list(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<ListEntry>& entries);
// s090: how many paragraphs open a list's story as its title (0 or 1): the
// first, when it has words and no tab and either stands alone or some later
// paragraph is in another style. (Entries may have no tab -- page numbers off.)
int list_title_paras(const Story& st);
// s088: the paragraph styles a list gathers, in level order (others dropped,
// each once). One step; the same -> no step.
bool set_list_styles(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<Doc::Id>& styles);
// s090: a list's settings -- a level's entry style ("" = automatic),
// page numbers on / off. One step; no change -> no step.
bool set_list_entry_style(DocJournal& j, Doc& d, const Doc::Id& list, int level, const Doc::Id& style);
bool set_list_numbers(DocJournal& j, Doc& d, const Doc::Id& list, bool on);

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
bool    edit_swatch(DocJournal& j, Doc& d, const Doc::Id& id, Swatch s);
Doc::Id duplicate_swatch(DocJournal& j, Doc& d, const Doc::Id& id);
bool    delete_swatch(DocJournal& j, Doc& d, const Doc::Id& id);

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
};
std::vector<Placed> layout_pages(const Doc& d, const std::vector<Doc::Id>& ids, double gap = 36);
inline Rect frame_rect(const Frame& f) { return {f.x, f.y, f.w, f.h}; }
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
// s062: the margins a page (or master) uses -- its own, else its master's.
// Null with neither.
const Margins* margins_of(const Doc& d, const Doc::Id& page_or_master);
struct ColumnSpan { double x0, x1; };
std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& page_or_master);
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
bool note_at(const std::string& text, std::size_t i);
int  notes_in(const std::string& text, std::size_t from = 0, std::size_t to = std::string::npos);   // the marks in [from, to)
// The number the k-th note of paragraph p takes (1-based, through the story).
int  note_number(const Story& s, int p, int k);
// The paragraph's notes made to match its marks (missing ones empty, extras dropped).
void fit_notes(Para& p);
// Which marker starts at text[i] (0 page, 1 section, 2 next, 3 previous;
// -1 none).
int marker_at(const std::string& text, std::size_t i);
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
// s027: spreads -- facing pages pair a left page with the right one after
// it (a book's first page stands alone on the right); without facing
// pages each page is its own. In page order.
std::vector<std::vector<Doc::Id>> spreads(const Doc& d);
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
// The topmost master item showing on a placed PAGE under (x, y) -- what
// Ctrl+Shift+click overrides. "" if none.
Doc::Id hit_master_item(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol, Doc::Id* page);
// Every own frame touched by `r` (touching selects -- Curvz's marquee rule).
std::vector<Doc::Id> frames_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r);
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
