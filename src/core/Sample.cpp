// core::load_sample -- the built-in sample book (s004). What Composix opens to
// before a project is opened, and the selftest's fixture. It is LOADED (not a
// user choice, not undoable): every write is inside a Doc::Loading scope.
//
// Varied on purpose, so every navigator section and tile state is on screen:
// three sections with different numbering, a chapter master, a 3-panel
// foldout, a threaded story, an embedded / a missing / a vector asset,
// process / spot / RGB colours, a stale contents list.
#include "core/Doc.hpp"

namespace composix::core {

void load_sample(Doc& d) {
    d.reset();
    Doc::Loading load(d);
    const double in = kPt;

    Settings s;
    s.title = "Sample Book";
    s.queries = {{"Double spaces", "  ", " "}};   // s078: a saved Find / Change
    d.put("doc", s);

    // ── masters ─────────────────────────────────────────────────────────────
    d.put("master.1", Master{"A", "Front", 1, s.trim_w, s.trim_h, {}, 1, ""});
    {
        Master b{"B", "Body", 2, s.trim_w, s.trim_h, {}, 1, ""};
        b.grid = 14;   // s036: a baseline grid at the body's leading
        d.put("master.2", b);
    }
    {   // A chapter opener sinks its text: a deep top margin.
        Master c{"C", "Chapter", 3, s.trim_w, s.trim_h, {}, 1, "master.2"};
        c.margins.top = 2.5 * in;
        c.grid = 14;   // s036: its grid starts at its own (deep) top margin
        d.put("master.3", c);
    }
    const auto fold3 = doc::size_presets(s)[2];
    {   // The foldout: narrow margins, a column per panel.
        Master f{"D", "Foldout", 4, fold3.w, fold3.h, fold3.folds, 3, ""};
        f.margins = {0.5 * in, 0.5 * in, 0.25 * in, 0.25 * in};
        f.gutter = 0.25 * in;
        d.put("master.4", f);
    }

    // ── pages: 4 front, 10 body (one a foldout), 2 back ────────────────────
    std::vector<Doc::Id> pg;
    for (int i = 1; i <= 16; ++i) {
        const Doc::Id id = "page." + std::to_string(i);
        Page p;
        p.pos = i;
        p.w = s.trim_w;
        p.h = s.trim_h;
        p.master = i <= 4 ? "master.1" : (i <= 14 ? "master.2" : "master.1");
        if (i == 5) p.master = "master.3";
        if (i == 11) { p.w = fold3.w; p.folds = fold3.folds; p.master = "master.4"; }
        d.put(id, p);
        pg.push_back(id);
    }

    // ── sections ────────────────────────────────────────────────────────────
    d.put("section.1", Section{"Front matter", pg[0], Numbering::RomanLower, 1, Side::Right, "master.1"});
    d.put("section.2", Section{"Body", pg[4], Numbering::Arabic, 1, Side::Right, "master.2"});
    d.put("section.3", Section{"Back matter", pg[14], Numbering::Arabic, 11, Side::Any, "master.1"});

    // ── styles ──────────────────────────────────────────────────────────────
    {   // s026: body text indents its first lines -- not after a heading (as novels do).
        Style b{"Body", StyleKind::Paragraph, 1, "Serif", 11, 14, false, false, Align::Justify};
        b.first_indent = 18;
        b.skip_after_heading = true;
        b.hyphenate = true;   // s038: justified text hyphenates (Body First too)
        b.keep_lines = 2;     // s039: no widows or orphans
        d.put("style.1", b);
        // s029: a chapter's first paragraph opens with a three-line drop cap.
        Style first = b;
        first.name = "Body First";
        first.pos = 10;
        first.first_indent = 0;
        first.drop_lines = 3;
        // s037: built on Body (it sets only its indent and drop cap), and
        // Enter after it goes on in Body.
        first.based_on = "style.1";
        first.own = doc::style_differences(first, b);
        first.next = "style.1";
        d.put("style.10", first);
    }
    {   // s037: Enter after a chapter heading starts its first paragraph.
        Style h{"Heading 1", StyleKind::Paragraph, 2, "Serif", 24, 28, true, false, Align::Left, 12, 6, "swatch.3"};
        h.next = "style.10";
        h.keep_next = 1;   // s039: never alone at a column's foot
        h.start = ParaStart::NextRightPage;   // s043: each chapter opens a right-hand page
        h.rule_below = {1, "swatch.3", 1, 4, false};   // s044: a Guild Rust line under the heading
        d.put("style.2", h);
    }
    d.put("style.3", Style{"Caption", StyleKind::Paragraph, 3, "Serif", 9, 11, false, true, Align::Left});
    {   // s114: the style footnotes are set in (the flow finds it by its name), built on Body.
        Style fn{"Footnote", StyleKind::Paragraph, 3.5, "Serif", 8.5, 10.5, false, false, Align::Left};
        fn.based_on = "style.1";
        fn.own = doc::style_differences(fn, *d.as<Style>("style.1"));
        d.put("style.12", fn);
    }
    {   // s026: sources hang: the first line out, the rest in.
        Style src{"Source", StyleKind::Paragraph, 4, "Serif", 10, 12, false, false, Align::Left};
        src.left_indent = 12;
        src.first_indent = -12;
        src.list = {ListType::Number, "\u2022", Numbering::Arabic, ".", 12};   // s045: numbered, in the hang
        d.put("style.4", src);
    }
    // Not used yet: a style a designer keeps ready (and the one Delete is offered on).
    d.put("style.6", Style{"Pull Quote", StyleKind::Paragraph, 6, "Serif", 14, 18, false, true, Align::Center, 6, 6});
    d.put("style.5", Style{"Emphasis", StyleKind::Character, 5, "", 0, 0, false, true, Align::Left});
    {   // s046: a character style built on Emphasis -- bold too (unused: ready to apply).
        Style strong{"Strong Emphasis", StyleKind::Character, 5.5, "", 0, 0, true, true, Align::Left};
        strong.based_on = "style.5";
        strong.own = doc::style_differences(strong, *d.as<Style>("style.5"));
        d.put("style.11", strong);
    }

    // ── stories ─────────────────────────────────────────────────────────────
    Story ch;
    ch.name = "Chapters";
    ch.paras = {
        {"style.2", "Chapter One"},
        {"style.10", "The harbor smelled of diesel and oranges the morning the ship came in."},
        {"style.1", "Nobody on the quay had expected her before the equinox, and fewer still "
                    "had expected the cargo she carried."},
        {"style.1", "By noon the guild had posted a notice, and by evening the notice had "
                    "been torn down twice."},
        {"style.2", "Chapter Two"},
        {"style.10", "The map came out of its tube in three panels, each a little narrower "
                    "than the last, so it would fold inside the book." + std::string(kMarkNote)},
    };
    // s114: a footnote on the map (its mark at the paragraph's end).
    ch.paras.back().notes = {Note{"The foldout opens to the right, past the page\u2019s edge.", {}}};
    d.put("story.1", ch);
    d.put("story.2", Story{"Map caption",
                           {{"style.3", "Fig. 1 — The trade routes, as surveyed in the third year."}}});
    {   // s093: the dedication centred on its own (Body, with its own alignment)
        Story ded{"Dedication", {{"style.1", "For everyone who ever finished a manuscript."}}};
        ded.paras.front().over.align = Align::Center;
        d.put("story.3", ded);
    }
    d.put("story.4", Story{"Sources",
                           {{"style.4", "Almanac of the Outer Ports, 3rd ed.", {{0, 26, "style.5"}}},   // the title in Emphasis
                            {"style.4", "Harbor Registers, vols. II–IV."},
                            {"style.4", "Letters of the Surveyor General."}}});

    // ── swatches ────────────────────────────────────────────────────────────
    d.put("swatch.1", Swatch{"Black", 1, ColorModel::CMYK, {0, 0, 0, 100}, "", 0x1a1a1a});
    d.put("swatch.2", Swatch{"Paper", 2, ColorModel::CMYK, {0, 1, 5, 0}, "", 0xfbf8f1});
    d.put("swatch.3", Swatch{"Guild Rust", 3, ColorModel::Spot, {0, 70, 90, 20}, "PMS 7593 C", 0xb5512b});
    d.put("swatch.4", Swatch{"Sky", 4, ColorModel::RGB, {111, 168, 220, 0}, "", 0x6fa8dc});

    // ── assets ──────────────────────────────────────────────────────────────
    d.put("asset.1", Asset{"guild-map.tif", 1, "assets/guild-map.tif", true, "CMYK", 7300, 3700, "Maps/guild-map.tif"});
    d.put("asset.2", Asset{"harbor.psd", 2, "Pictures/harbor.psd", false, "RGB", 3600, 2700});
    d.put("asset.3", Asset{"fleuron.svg", 3, "assets/fleuron.svg", true, "Gray", 0, 0});
    d.put("asset.4", Asset{"lighthouse.jpg", 4, "Pictures/lighthouse.jpg", false, "RGB", 2400, 1800});

    // ── frames ──────────────────────────────────────────────────────────────
    const double m = 0.75 * in, tw = s.trim_w - 2 * m, th = s.trim_h - 2 * m;
    // The chapter story threads pages 1-3 of the body.
    d.put("frame.1", Frame{pg[4], FrameKind::Text, m, 2.5 * in, tw, th - 1.75 * in, 0, 1, "story.1", "frame.2", ""});
    d.put("frame.2", Frame{pg[5], FrameKind::Text, m, m, tw, th, 0, 1, "story.1", "frame.3", ""});
    d.put("frame.3", Frame{pg[6], FrameKind::Text, m, m, tw, th, 0, 1, "story.1", "", ""});
    // The chapter opener's picture (its link is missing).
    d.put("frame.4", Frame{pg[4], FrameKind::Image, m, m, tw, 1.5 * in, 0, 2, "", "", "asset.2"});
    // The foldout: the map across the panels, its caption under it.
    d.put("frame.5", Frame{pg[10], FrameKind::Image, 0.25 * in, 0.5 * in, fold3.w - 0.5 * in, 6.5 * in, 0, 1, "", "", "asset.1"});
    {   // The caption, ruled off in Guild Rust (a spot colour: its own plate).
        Frame cap{pg[10], FrameKind::Text, 0.25 * in, 7.25 * in, 6 * in, 0.75 * in, 0, 2, "story.2", "", ""};
        cap.stroke = "swatch.3";
        cap.stroke_w = 0.5;
        cap.fill = "swatch.3";     // s021: a 15% tint of the spot ink behind it
        cap.fill_tint = 0.15;
        cap.inset = {6, 6, 6, 6};  // s040: the words clear of the rule
        d.put("frame.6", cap);
    }
    // Dedication, ornaments, sources.
    {   // The dedication, centred in its box (s042).
        Frame ded{pg[1], FrameKind::Text, m, 3 * in, tw, 1 * in, 0, 1, "story.3", "", ""};
        ded.valign = VAlign::Center;
        d.put("frame.7", ded);
    }
    d.put("frame.8", Frame{pg[1], FrameKind::Image, 2.75 * in, 4.25 * in, 0.5 * in, 0.5 * in, 0, 2, "", "", "asset.3"});
    d.put("frame.9", Frame{pg[15], FrameKind::Image, 2.75 * in, 7.5 * in, 0.5 * in, 0.5 * in, 0, 2, "", "", "asset.3"});
    {   // Sources in two balanced columns (s012).
        Frame src{pg[15], FrameKind::Text, m, m, tw, 5 * in, 0, 1, "story.4", "", ""};
        src.columns = 2;
        src.balance = true;
        d.put("frame.10", src);
    }
    {   // s018: a lighthouse set into the chapter's opening text, wrapped (both sides).
        Frame pic{pg[4], FrameKind::Image, m + tw / 2 - 0.8 * in, 3.4 * in, 1.6 * in, 1.1 * in, 0, 3, "", "", "asset.4"};
        pic.fit = ImageFit::Fit;
        pic.wrap = Wrap::Around;
        pic.wrap_off = {9, 9, 9, 9};
        d.put("frame.11", pic);
    }

    // s023: folios -- the page number at the foot of every page, set on the
    // front and body masters (roman in front, arabic in the body: each
    // page's own section numbering).
    d.put("style.7", Style{"Folio", StyleKind::Paragraph, 7, "Serif", 9, 11, false, false, Align::Center});
    d.put("story.5", Story{"Folio (front)", {{"style.7", kMarkPage}}});
    d.put("story.6", Story{"Folio (body)", {{"style.7", kMarkPage}}});
    d.put("frame.12", Frame{"master.1", FrameKind::Text, m, s.trim_h - 0.5 * in, tw, 0.25 * in, 0, 1, "story.5", "", ""});
    {   // s025: the body master is a facing one -- folios at the outside
        // corners: the right-hand pages' at the right, the left-hand pages'
        // at the left (each frame shows on its side's pages only).
        Frame r{"master.2", FrameKind::Text, m + tw - 0.5 * in, s.trim_h - 0.5 * in, 0.5 * in, 0.25 * in, 0, 1, "story.6", "", ""};
        r.side = Side::Right;
        d.put("frame.13", r);
        Frame l{"master.2", FrameKind::Text, m, s.trim_h - 0.5 * in, 0.5 * in, 0.25 * in, 0, 2, "story.7", "", ""};
        l.side = Side::Left;
        d.put("story.7", Story{"Folio (body, left)", {{"style.7", kMarkPage}}});
        d.put("frame.14", l);
        // The chapter opener's folio, overridden on its page: centred there.
        Frame o = r;
        o.owner = pg[4];
        o.overrides = "frame.13";
        o.side = Side::Any;
        o.x = m;
        o.w = tw;
        o.story = "story.8";
        d.put("story.8", Story{"Folio (page 1)", {{"style.7", kMarkPage}}});
        d.put("frame.15", o);
    }

    {   // s026: a contents page -- entries with a right tab at the text's
        // edge and a dot leader.
        Style c{"Contents Entry", StyleKind::Paragraph, 8, "Serif", 11, 18, false, false, Align::Left};
        c.tabs = {{tw, TabAlign::Right, "."}};
        d.put("style.8", c);
        d.put("style.9", Style{"Contents Title", StyleKind::Paragraph, 9, "Serif", 18, 22, false, false, Align::Left, 0, 12});
        d.put("story.9", Story{"Contents", {{"style.9", "Contents"},
                                            {"style.8", "Chapter One\t1"},
                                            {"style.8", "Chapter Two\t3"},   // s043: it opens page 3 now
                                            {"style.8", "Sources\t12"}}});
        d.put("frame.16", Frame{pg[2], FrameKind::Text, m, m, tw, 3 * in, 0, 1, "story.9", "", ""});
    }

    // ── guides on the body master: the margins (s006) ──────────────────────
    d.put("guide.1", Guide{"master.2", true, m});
    d.put("guide.2", Guide{"master.2", true, s.trim_w - m});
    d.put("guide.3", Guide{"master.2", false, m});
    d.put("guide.4", Guide{"master.2", false, s.trim_h - m});

    // ── generated lists ─────────────────────────────────────────────────────
    // The contents list was built before Chapter Two existed: stale.
    d.put("list.1", GenList{"Table of Contents", ListKind::Contents, 1, {"style.2"}, pg[2], 1, "story.9"});
    d.put("list.2", GenList{"Bibliography", ListKind::Bibliography, 2, {"style.4"}, pg[15], 3});
}

}  // namespace composix::core
