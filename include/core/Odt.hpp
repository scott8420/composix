#pragma once
// core/Odt -- an OpenDocument text file (.odt), read (s235, T2). What the
// file says, in its own terms: its styles (named and automatic, each with
// its parent), its body as paragraphs with their spans and footnotes, its
// page layouts and master pages (with their headers and footers). Building
// a Composix document from it is layout::import_odt's (it flows the text).
//
// Kept: paragraphs and headings (lists' and sections' too, as paragraphs),
// spans, spaces / tabs / line breaks, footnotes and endnotes (as notes),
// page breaks, page numbers in headers / footers. Counted, not kept:
// tables, pictures, tables of contents.
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace composix::core::odt {

// Properties a style sets; unset: NaN / -1 / "" (its parent's, then the default).
struct Props {
    std::string family;                 // a font family
    double      size = -1;              // points (from a percentage: of the parent's, resolved on read when it can be)
    int         bold = -1, italic = -1;
    std::string color;                  // "#rrggbb"
    int         align = -1;             // 0 left, 1 centre, 2 right, 3 justified
    double      left = -1e9, right = -1e9, indent = -1e9, before = -1e9, after = -1e9;   // points; -1e9 unset
    double      leading = -1;           // points (fo:line-height absolute); -1 unset
    double      leading_pct = -1;       // fo:line-height as a percentage (100 = single)
    int         page_break = -1;        // 1: starts a page (fo:break-before="page")
    int         keep_next = -1;         // s257: 1 fo:keep-with-next="always", 0 "auto"
    int         orphans = -1, widows = -1;   // s257: fo:orphans / fo:widows (lines)
    // s262: character attributes. letter: fo:letter-spacing (points; 0
    // "normal"); rise / scale: style:text-position (percent of the size --
    // "super" 33 / "sub" -33, the size 58 unless given); caps: 1 uppercase
    // (fo:text-transform), 2 small caps (fo:font-variant), 0 neither;
    // underline / strike: style:text-underline-style / -line-through-style.
    double      letter = -1e9, rise = -1e9, scale = -1;
    int         caps = -1, underline = -1, strike = -1;
    // s266: font features -- LibreOffice writes them into the font's name
    // ("Linux Libertine G:onum&liga=0"), the family is what's before the
    // colon; `kerning` 0: style:letter-kerning="false" (pair kerning off).
    std::string features;
    int         kerning = -1;
    std::string master;                 // style:master-page-name (a new page with that master)
};

struct Style {
    std::string name, display, family;  // family: "paragraph" / "text"
    std::string parent, next;
    Props       props;
    int         outline = 0;            // style:default-outline-level (headings)
    bool        automatic = false;      // from office:automatic-styles (a paragraph's own look)
};

struct Span { int start = 0, end = 0; std::string style; };   // bytes of the paragraph's text
// s304: a hyperlink (text:a) over bytes [start, end): its xlink:href as written
// ("https://...", "#bookmark", "#Heading|outline").
struct Link { int start = 0, end = 0; std::string href; };

// s243 (T2b): a list style's level -- a bullet, or a number in a format.
struct ListLevel {
    bool        number = false;
    std::string bullet = "\u2022";
    std::string format = "1";       // style:num-format: 1 i I a A
    std::string prefix, suffix = ".";
    double      indent = -1;        // points: where its text sits (fo:margin-left / text:space-before + min-label-width)
    double      label = -1;         // the label's room (min-label-width / -text-indent)
    int         display = 1;        // s251: text:display-levels -- 2: "1.2."
    std::string text_style;         // s251: the label's character style (text:style-name)
};
struct ListStyle { std::string name; std::vector<ListLevel> levels; };   // levels[0] is level 1

struct Note {
    std::string       text;             // its paragraphs, '\n' between (as core::Note)
    std::vector<Span> spans;
    bool              endnote = false;
};

// s244 (T2b): a picture -- where it's anchored and how it sits.
struct Picture {
    std::string name, href;             // its frame's name; Pictures/... in the zip, or a link
    std::string bytes;                  // its file (embedded: from the zip, or a flat file's base64)
    std::string anchor = "paragraph";   // as-char / char / paragraph / page / frame
    int         para = -1;              // the body paragraph it's anchored to (not page)
    int         at = 0;                 // as-char / char: the byte in that paragraph's text
    int         page = 1;               // page-anchored: text:anchor-page-number
    double      x = 0, y = 0, w = 0, h = 0;   // points: svg:x / y (from the anchor), width, height
    int         wrap = 2;               // 0 none (text runs through), 1 around, 2 jump (text above and below)
    int         hpos = 0;               // 0 at x, 1 left, 2 centre, 3 right (of the paragraph's / page's text area)
    int         vpos = 0;               // 0 at y, 1 top, 2 middle, 3 bottom
    bool        whole_page = false;     // horizontal-rel / vertical-rel the page itself (else its text area)
};

// s286: an alphabetical index's mark in the text (core's kMarkIndex): its
// entry, under key1 when the file gives one (key2 joins the sub-entry).
struct IndexMark { std::string term, sub; };

// s317: a table (table:table) -- its columns' widths (points, from their
// styles; 0 unknown), header rows, and each row's cells (each cell's
// paragraphs; covered cells of a merge kept as empty cells). In the body it
// stands as a paragraph whose text is core's kMarkTable.
struct Para;
struct TableCell {
    std::vector<Para> paras;
    std::string       bg;               // fo:background-color ("#rrggbb"; "" none)
    int               span_cols = 1, span_rows = 1;   // s319: table:number-columns- / rows-spanned
    int               valign = 0;       // s321: style:vertical-align top / middle / bottom
};
struct Table {
    std::vector<double> widths;
    int header_rows = 0;
    std::vector<std::vector<TableCell>> rows;
};

struct Para {
    std::string       style;            // the paragraph's style (often an automatic one)
    std::string       text;             // UTF-8; a note's place marked with core's kMarkNote, a page number kMarkPage
    std::vector<Span> spans;
    std::vector<Note> notes;            // one for each note mark, in order
    int               outline = 0;      // a heading's level (text:h), 0 a paragraph
    std::string       list;             // s243: its list's style ("" not in a list)
    int               list_level = 0;   // 1.. (nested lists deeper)
    int               start = 0;        // text:start-value on its item (0: counts on)
    std::vector<Picture> pictures;      // s244: those anchored in it (Book::images takes them)
    std::vector<IndexMark> index;       // s286: one for each kMarkIndex in text, in order
    std::vector<std::string> cites;     // s291: one for each kMarkRef in text -- the source's identifier
    std::vector<Link> links;            // s304: its hyperlinks
    std::vector<std::string> bookmarks; // s304: one for each kMarkAnchor in text -- the bookmark's name
    std::vector<Table> tables;          // s317: a table's paragraph (text kMarkTable)
    int               section_cols = 0;  // s293: in a section of 2+ columns: how many, and the gap
    double            section_gap = 0;
};

struct PageLayout {
    double w = 0, h = 0;                // points
    double top = -1, right = -1, bottom = -1, left = -1;
    bool   mirrored = false;            // s236: style:page-usage="mirrored" -- facing pages
    int    side = 0;                    // s268: style:page-usage "right" 1, "left" 2 -- a page style only for that side
    // s288: its header / footer when switched on (style:header-style's
    // header-footer-properties): the least height, and the space between it
    // and the body. The page's margin runs to the header's top; the body
    // starts below header + spacing (as LibreOffice lays it).
    bool   head = false, foot = false;
    double head_min = 0, head_gap = 0, foot_min = 0, foot_gap = 0;
    // s289: the body's columns (style:columns).
    int    columns = 1;
    double column_gap = 0;
};

struct MasterPage {
    std::string       name, display, layout, next;
    std::vector<Para> header, footer, header_left, footer_left;   // _left: the left pages' own (empty: as the right)
};

// s271: a table of contents in the body -- where it stands (before body
// paragraph `at`), its title (words and paragraph style, often an automatic
// one naming a page style), the levels it gathers (outline 1..levels), and
// level 1's entry style.
struct Toc {
    int         at = 0;
    std::string title, title_style;
    std::string entry_style;
    int         levels = 10;
    int         entries = 0;            // as LibreOffice last wrote it
    bool        right_tab = false;      // s272: level 1's page number at the right edge (its entry template's tab)
    std::string leader;                 //        what fills up to it ("." dots; "" none)
    // s294: every level's own entry template -- its style, its right tab and leader
    // (index 0 is level 1; a level the file doesn't give: "" / none).
    struct Level { std::string style; bool right_tab = false; std::string leader; };
    std::vector<Level> level_entries;
};

struct Book {
    std::map<std::string, Style>      styles;      // by name (named and automatic)
    std::map<std::string, Props>      defaults;    // style:default-style, by family
    std::map<std::string, std::string> fonts;      // style:font-face name -> family
    std::map<std::string, PageLayout> layouts;
    std::map<std::string, ListStyle>  lists;       // s243: list styles by name
    std::map<std::string, Picture>    graphics;    // s244: graphic styles -- their wrap / position (in a Picture's fields)
    std::vector<Picture>              images;      // s244: the body's pictures, in order
    std::vector<MasterPage>           masters;
    std::vector<Para>                 body;
    std::string                       title, author;
    std::vector<Toc>                  tocs;        // s271: the body's tables of contents
    // s286: the body's alphabetical indexes -- where each stands (before body
    // paragraph `at`) and its title (words, paragraph style).
    struct Index { int at = 0; std::string title, title_style; };
    std::vector<Index>                indexes;
    // s291: the sources its bibliography marks name (text:bibliography-mark),
    // by identifier, in the order first cited; and where a bibliography stands.
    struct Source { std::string type, author, title, year, publisher, journal, volume, pages, url; };
    std::map<std::string, Source>     sources;
    std::vector<std::string>          source_order;
    std::vector<Index>                bibliographies;
    // s293: a section style's columns (style:section-properties) -- count, gap.
    std::map<std::string, std::pair<int, double>> section_columns;
    // s317: table column styles' widths, table cell styles' backgrounds.
    std::map<std::string, double>      column_widths;
    std::map<std::string, std::string> cell_backgrounds;
    std::map<std::string, int>         cell_valign;   // s321
    int tables = 0, pictures = 0, contents = 0;   // what wasn't kept (s271: contents -- an index / bibliography)
    // A style's property, its chain followed (named parents; the family's default last).
    Props resolved(const std::string& style) const;
};

// The .odt's bytes (a zip: content.xml, styles.xml, meta.xml) -- or a flat
// .fodt (one XML). nullopt with `err` when it isn't one.
std::optional<Book> read(const std::string& bytes, std::string* err = nullptr);
// "2.54cm", "1in", "12pt", "10mm", "1pc" -> points; `fallback` when it isn't a length.
double length(const std::string& v, double fallback = -1);
// s266: "Family:feat&feat=1" -> "Family", the features ("" none).
std::string split_features(std::string& family);
// s244: a picture's size in pixels from its file's header (PNG, JPEG, GIF); false if not one.
bool pixel_size(const std::string& bytes, int& w, int& h);
// s244: base64 (a flat file's office:binary-data), white space ignored.
std::string base64(const std::string& text);

}  // namespace composix::core::odt
