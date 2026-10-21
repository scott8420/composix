#pragma once
// layout::import_odt -- an OpenDocument text (core::odt::Book) made a
// Composix document (s236, T2). In the layout library: its pages are fitted
// to its text by flowing it (as BigBook's are).
//
// The document: its first page layout's size and margins (mirrored: facing
// pages), one master, the text in one story threaded through a frame on
// every page. Styles: each named paragraph / text style a Composix style
// (flattened: its chain resolved); a paragraph's automatic style is its
// named parent plus the paragraph's own settings (ParaLocal) and local
// formatting; a span's automatic style is local formatting (bold, italic,
// size, colour), a named text style a character style. Colours: swatches.
// A line break starts a new paragraph (as Composix keeps them). Footnotes:
// notes (only endnotes in the file: set at the story's end). A style that
// breaks to a new page starts each paragraph on one (chapters).
// Headers / footers and the chapters' sections: s237.
#include "core/Doc.hpp"
#include "core/Odt.hpp"

#include <functional>
#include <string>
#include <vector>

namespace composix::layout {

struct OdtInfo {
    int pages = 0, paragraphs = 0, styles = 0, notes = 0, chapters = 0;
    int headers = 0;                               // s237: a header, a footer (each counted once)
    int lists = 0;                                 // s243: list styles made (a paragraph style at a list's level)
    int placed = 0;                                // s245: pictures placed
    int header_pictures = 0;                       // s248: of them, on the master (a header's / footer's)
    bool front_matter = false;                     // s237: words before the first chapter -> their own section
    int masters = 1;                               // s270: a master per page style the book uses
    int contents_lists = 0;                        // s272: its tables of contents, as contents lists (built)
    int indexes = 0;                               // s286: its alphabetical indexes, as indexes (built)
    int index_marks = 0;                           // s286: the index marks in its text
    int tables = 0, pictures = 0, contents = 0;   // in the file, not brought in
    std::vector<std::string> notes_said;           // what didn't come over as it was, in words
};

struct OdtOptions {
    std::function<void(double, const std::string&)> progress;   // 0..1 and what it's doing
    bool        sections = true;   // s247: the front matter its own section (roman numbers), the body from 1
    std::string picture_dir;   // s245: where its pictures are written (and linked from); "" = left out
};

// Replace `d` with the book (loaded: not a step). False with `err` when
// there's nothing to make a document of.
bool import_odt(core::Doc& d, const core::odt::Book& b, OdtInfo* info = nullptr, const OdtOptions& opt = {},
                std::string* err = nullptr);

}  // namespace composix::layout
