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
};

struct PageLayout {
    double w = 0, h = 0;                // points
    double top = -1, right = -1, bottom = -1, left = -1;
    bool   mirrored = false;            // s236: style:page-usage="mirrored" -- facing pages
};

struct MasterPage {
    std::string       name, display, layout, next;
    std::vector<Para> header, footer, header_left, footer_left;   // _left: the left pages' own (empty: as the right)
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
    int tables = 0, pictures = 0, contents = 0;   // what wasn't kept
    // A style's property, its chain followed (named parents; the family's default last).
    Props resolved(const std::string& style) const;
};

// The .odt's bytes (a zip: content.xml, styles.xml, meta.xml) -- or a flat
// .fodt (one XML). nullopt with `err` when it isn't one.
std::optional<Book> read(const std::string& bytes, std::string* err = nullptr);
// "2.54cm", "1in", "12pt", "10mm", "1pc" -> points; `fallback` when it isn't a length.
double length(const std::string& v, double fallback = -1);
// s244: a picture's size in pixels from its file's header (PNG, JPEG, GIF); false if not one.
bool pixel_size(const std::string& bytes, int& w, int& h);
// s244: base64 (a flat file's office:binary-data), white space ignored.
std::string base64(const std::string& text);

}  // namespace composix::core::odt
