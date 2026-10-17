#pragma once
// layout::load_big_book -- the generated long test book (s100).
//
// The performance arc measures before it changes anything, so it needs a
// book the size real ones are: ~400 pages of greeked text (deterministic
// lorem ipsum -- the same words every run) set like the sample: front
// matter (title, dedication, a contents list built for real), then ONE
// story threaded through every body page -- the long story the reflow work
// is about -- with chapter openers on right-hand pages (the chapter master,
// a sunk opening, a picture over it), section heads inside the chapters,
// some words in Emphasis, and a wrapped picture every few pages.
//
// In the layout library, not core: the pages are fitted to the text by
// flowing it -- chapter by chapter (each starts a fresh right-hand page, so
// it breaks alone as it does in the whole story), then one whole-story flow
// to check -- and the contents are written from where that flow put the
// headings. Loaded, not a user step (Doc::Loading).
//
// Pictures: with `picture_dir` set, a handful of plates are drawn (cairo,
// PNG) into it and linked -- real files, so thumbnails and the canvas pay
// for decoding. Empty: the assets are left missing (placeholders).
#include "core/Doc.hpp"

#include <string>

namespace composix::layout {

struct BigBookOpts {
    int         pages = 400;       // roughly: the body is fitted to its text
    std::string picture_dir;       // "" = no files (missing links)
    unsigned    seed = 100;        // the greeking's (same seed, same book)
};

struct BigBookInfo {
    int chapters = 0, sections = 0, pictures = 0, pages = 0;
    int rounds = 0;                // chapter flows while fitting
    int whole = 0;                 // whole-story flows (1 = the check agreed)
    std::size_t bytes = 0;         // the long story's text
};

BigBookInfo load_big_book(core::Doc& d, const BigBookOpts& o = {});

}  // namespace composix::layout
