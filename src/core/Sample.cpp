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
    d.put("doc", s);

    // ── masters ─────────────────────────────────────────────────────────────
    d.put("master.1", Master{"A", "Front", 1, s.trim_w, s.trim_h, {}, 1, ""});
    d.put("master.2", Master{"B", "Body", 2, s.trim_w, s.trim_h, {}, 1, ""});
    d.put("master.3", Master{"C", "Chapter", 3, s.trim_w, s.trim_h, {}, 1, "master.2"});
    const auto fold3 = doc::size_presets(s)[2];
    d.put("master.4", Master{"D", "Foldout", 4, fold3.w, fold3.h, fold3.folds, 3, ""});

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
    d.put("style.1", Style{"Body", StyleKind::Paragraph, 1, "Serif", 11, 14, false, false, Align::Justify});
    d.put("style.2", Style{"Heading 1", StyleKind::Paragraph, 2, "Serif", 24, 28, true, false, Align::Left});
    d.put("style.3", Style{"Caption", StyleKind::Paragraph, 3, "Serif", 9, 11, false, true, Align::Left});
    d.put("style.4", Style{"Source", StyleKind::Paragraph, 4, "Serif", 10, 12, false, false, Align::Left});
    d.put("style.5", Style{"Emphasis", StyleKind::Character, 5, "Serif", 0, 0, false, true, Align::Left});

    // ── stories ─────────────────────────────────────────────────────────────
    Story ch;
    ch.name = "Chapters";
    ch.paras = {
        {"style.2", "Chapter One"},
        {"style.1", "The harbor smelled of diesel and oranges the morning the ship came in."},
        {"style.1", "Nobody on the quay had expected her before the equinox, and fewer still "
                    "had expected the cargo she carried."},
        {"style.1", "By noon the guild had posted a notice, and by evening the notice had "
                    "been torn down twice."},
        {"style.2", "Chapter Two"},
        {"style.1", "The map came out of its tube in three panels, each a little narrower "
                    "than the last, so it would fold inside the book."},
    };
    d.put("story.1", ch);
    d.put("story.2", Story{"Map caption",
                           {{"style.3", "Fig. 1 — The trade routes, as surveyed in the third year."}}});
    d.put("story.3", Story{"Dedication", {{"style.1", "For everyone who ever finished a manuscript."}}});
    d.put("story.4", Story{"Sources",
                           {{"style.4", "Almanac of the Outer Ports, 3rd ed."},
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
    d.put("frame.6", Frame{pg[10], FrameKind::Text, 0.25 * in, 7.25 * in, 6 * in, 0.75 * in, 0, 2, "story.2", "", ""});
    // Dedication, ornaments, sources.
    d.put("frame.7", Frame{pg[1], FrameKind::Text, m, 3 * in, tw, 1 * in, 0, 1, "story.3", "", ""});
    d.put("frame.8", Frame{pg[1], FrameKind::Image, 2.75 * in, 4.25 * in, 0.5 * in, 0.5 * in, 0, 2, "", "", "asset.3"});
    d.put("frame.9", Frame{pg[15], FrameKind::Image, 2.75 * in, 7.5 * in, 0.5 * in, 0.5 * in, 0, 2, "", "", "asset.3"});
    d.put("frame.10", Frame{pg[15], FrameKind::Text, m, m, tw, 5 * in, 0, 1, "story.4", "", ""});

    // ── guides on the body master: the margins (s006) ──────────────────────
    d.put("guide.1", Guide{"master.2", true, m});
    d.put("guide.2", Guide{"master.2", true, s.trim_w - m});
    d.put("guide.3", Guide{"master.2", false, m});
    d.put("guide.4", Guide{"master.2", false, s.trim_h - m});

    // ── generated lists ─────────────────────────────────────────────────────
    // The contents list was built before Chapter Two existed: stale.
    d.put("list.1", GenList{"Table of Contents", ListKind::Contents, 1, {"style.2"}, pg[2], 1});
    d.put("list.2", GenList{"Bibliography", ListKind::Bibliography, 2, {"style.4"}, pg[15], 3});
}

}  // namespace composix::core
