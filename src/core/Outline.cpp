#include "core/Outline.hpp"

#include <algorithm>
#include <stdexcept>

namespace composix::core {

const std::vector<SectionInfo>& sections() {
    static const std::vector<SectionInfo> k = {
        {Section::Document,  "document",  "Document",        "x-office-document-symbolic"},
        {Section::Pages,     "pages",     "Pages & Spreads", "view-grid-symbolic"},
        {Section::Masters,   "masters",   "Masters",         "view-dual-symbolic"},
        {Section::Stories,   "stories",   "Stories",         "format-justify-left-symbolic"},
        {Section::Styles,    "styles",    "Styles",          "format-text-bold-symbolic"},
        {Section::Assets,    "assets",    "Links & Assets",  "image-x-generic-symbolic"},
        {Section::Colors,    "colors",    "Colors",          "applications-graphics-symbolic"},
        {Section::Generated, "generated", "Contents & Index","view-list-bullet-symbolic"},
    };
    return k;
}

const SectionInfo& section_info(Section s) {
    for (const auto& i : sections())
        if (i.id == s) return i;
    throw std::logic_error("section_info: unknown section");
}

std::vector<const Item*> Outline::in(Section s) const {
    std::vector<const Item*> out;
    for (const auto& it : m_items)
        if (it.section == s) out.push_back(&it);
    return out;
}

const Item* Outline::find(const std::string& id) const {
    for (const auto& it : m_items)
        if (it.id == id) return &it;
    return nullptr;
}

std::vector<Field> shared_fields(const std::vector<const Item*>& sel) {
    std::vector<Field> out;
    if (sel.empty()) return out;
    for (const auto& f : sel.front()->fields) {
        bool everywhere = true, same = true;
        for (std::size_t i = 1; i < sel.size() && everywhere; ++i) {
            const auto& fs = sel[i]->fields;
            const auto hit = std::find_if(fs.begin(), fs.end(),
                                          [&](const Field& g) { return g.label == f.label; });
            if (hit == fs.end()) everywhere = false;
            else if (hit->value != f.value) same = false;
        }
        if (everywhere) out.push_back({f.label, same ? f.value : std::string(kMixed)});
    }
    return out;
}

std::string selection_title(const std::vector<const Item*>& sel) {
    if (sel.empty()) return "Nothing selected";
    if (sel.size() == 1) return sel.front()->title;
    const Section s = sel.front()->section;
    const bool one_kind = std::all_of(sel.begin(), sel.end(),
                                      [&](const Item* i) { return i->section == s; });
    if (!one_kind) return std::to_string(sel.size()) + " items";
    static const char* noun[] = {"sections", "pages", "masters", "stories",
                                 "styles", "assets", "colors", "generated lists"};
    return std::to_string(sel.size()) + " " + noun[static_cast<int>(s)];
}

// ─────────────────────────────────────────────────────────────────────────────
// The fixture. Plausible, varied, and small: every tile kind, every badge, at
// least one foldout, one missing link, one stale list -- so the shell's states
// are all on screen at once (CANON: a milestone ends at a surface).
// ─────────────────────────────────────────────────────────────────────────────
Outline fixture_outline() {
    Outline o;
    using S = Section;
    using K = ThumbKind;

    // Document -- sections of the book.
    o.add({"sec.front", S::Document, "Front matter", "", {K::Section, 6, 9, 0x8a8f98},
           {{"Pages", "i–vi"}, {"Numbering", "Roman (i, ii, iii)"}, {"Starts", "Right"},
            {"Master", "A-Front"}}});
    o.add({"sec.body", S::Document, "Body", "", {K::Section, 6, 9, 0x5b7fa6},
           {{"Pages", "1–12"}, {"Numbering", "Arabic (1, 2, 3)"}, {"Starts", "Right"},
            {"Master", "B-Body"}}});
    o.add({"sec.back", S::Document, "Back matter", "", {K::Section, 6, 9, 0x8a8f98},
           {{"Pages", "13–16"}, {"Numbering", "Arabic (1, 2, 3)"}, {"Starts", "Any"},
            {"Master", "A-Front"}}});

    // Pages -- 6x9 trade, one 3-panel foldout (17.75in wide) on page 7.
    const auto page = [&](int n, const char* master, const char* sec, bool foldout = false) {
        const std::string num = std::to_string(n);
        Item it{"page." + num, S::Pages, "Page " + num, foldout ? "Foldout" : "",
                {K::Page, foldout ? 17.75 : 6.0, 9.0, 0xffffff, foldout, ""},
                {{"Folio", num},
                 {"Size", foldout ? "17.75 × 9 in (3-panel)" : "6 × 9 in"},
                 {"Master", master},
                 {"Section", sec},
                 {"Side", n % 2 ? "Right" : "Left"}}};
        o.add(std::move(it));
    };
    for (int n = 1; n <= 12; ++n)
        page(n, n == 1 ? "C-Chapter" : (n == 7 ? "D-Foldout" : "B-Body"), "Body", n == 7);

    // Masters.
    o.add({"master.A", S::Masters, "A-Front", "", {K::Page, 6, 9, 0xffffff},
           {{"Size", "6 × 9 in"}, {"Columns", "1"}, {"Used by", "Front & back matter"}}});
    o.add({"master.B", S::Masters, "B-Body", "", {K::Page, 6, 9, 0xffffff},
           {{"Size", "6 × 9 in"}, {"Columns", "1"}, {"Used by", "10 pages"}}});
    o.add({"master.C", S::Masters, "C-Chapter", "", {K::Page, 6, 9, 0xffffff},
           {{"Size", "6 × 9 in"}, {"Columns", "1"}, {"Used by", "1 page"}}});
    o.add({"master.D", S::Masters, "D-Foldout", "Foldout", {K::Page, 17.75, 9, 0xffffff, true},
           {{"Size", "17.75 × 9 in (3-panel)"}, {"Columns", "3"}, {"Used by", "1 page"}}});

    // Stories.
    o.add({"story.ch1", S::Stories, "Chapter One", "", {K::Text, 6, 9, 0, false,
           "The harbor smelled of diesel and oranges the morning the ship came in."},
           {{"Words", "4,812"}, {"Frames", "11, threaded"}, {"Style", "Body"},
            {"Overset", "No"}}});
    o.add({"story.cap", S::Stories, "Map caption", "Overset", {K::Text, 6, 9, 0, true,
           "Fig. 1 — The Guild trade routes, as surveyed in the third year."},
           {{"Words", "64"}, {"Frames", "1"}, {"Style", "Caption"}, {"Overset", "Yes — 2 lines"}}});
    o.add({"story.ded", S::Stories, "Dedication", "", {K::Text, 6, 9, 0, false,
           "For everyone who ever finished a manuscript."},
           {{"Words", "7"}, {"Frames", "1"}, {"Style", "Body"}, {"Overset", "No"}}});

    // Styles.
    o.add({"style.body", S::Styles, "Body", "", {K::Style, 6, 9, 0x222222, false, "Aa"},
           {{"Kind", "Paragraph"}, {"Font", "Granjon 11/14"}, {"Align", "Justified"},
            {"Used", "38×"}}});
    o.add({"style.h1", S::Styles, "Heading 1", "", {K::Style, 6, 9, 0x222222, false, "Aa"},
           {{"Kind", "Paragraph"}, {"Font", "Granjon Bold 24/28"}, {"Align", "Left"},
            {"Used", "1×"}}});
    o.add({"style.cap", S::Styles, "Caption", "", {K::Style, 6, 9, 0x555555, false, "Aa"},
           {{"Kind", "Paragraph"}, {"Font", "Granjon Italic 9/11"}, {"Align", "Left"},
            {"Used", "1×"}}});
    o.add({"style.em", S::Styles, "Emphasis", "", {K::Style, 6, 9, 0x222222, false, "Aa"},
           {{"Kind", "Character"}, {"Font", "Granjon Italic"}, {"Used", "12×"}}});

    // Links & assets.
    o.add({"asset.map", S::Assets, "guild-map.tif", "", {K::Image, 17.75, 9, 0x8fa37a},
           {{"Link", "Linked"}, {"Status", "OK"}, {"Color", "CMYK"}, {"Effective ppi", "412"},
            {"Placed", "Page 7"}}});
    o.add({"asset.cover", S::Assets, "harbor.psd", "Missing", {K::Image, 4, 3, 0xc98f5a, true},
           {{"Link", "Linked"}, {"Status", "Missing"}, {"Color", "RGB"}, {"Effective ppi", "—"},
            {"Placed", "Page 1"}}});
    o.add({"asset.orn", S::Assets, "fleuron.svg", "", {K::Image, 1, 1, 0x6b6b6b},
           {{"Link", "Embedded"}, {"Status", "OK"}, {"Color", "Gray"}, {"Effective ppi", "Vector"},
            {"Placed", "Pages 1, 12"}}});

    // Colors.
    o.add({"color.black", S::Colors, "Black", "", {K::Swatch, 1, 1, 0x1a1a1a},
           {{"Model", "CMYK"}, {"Value", "0 / 0 / 0 / 100"}, {"Type", "Process"}, {"Used", "41×"}}});
    o.add({"color.paper", S::Colors, "Paper", "", {K::Swatch, 1, 1, 0xfbf8f1},
           {{"Model", "CMYK"}, {"Value", "0 / 1 / 5 / 0"}, {"Type", "Process"}, {"Used", "—"}}});
    o.add({"color.rust", S::Colors, "Guild Rust", "Spot", {K::Swatch, 1, 1, 0xb5512b},
           {{"Model", "Spot"}, {"Value", "PMS 7593 C"}, {"Type", "Spot"}, {"Used", "6×"}}});
    o.add({"color.sky", S::Colors, "Sky", "RGB", {K::Swatch, 1, 1, 0x6fa8dc},
           {{"Model", "RGB"}, {"Value", "111 / 168 / 220"}, {"Type", "Process (converts at output)"},
            {"Used", "2×"}}});

    // Generated lists.
    o.add({"gen.toc", S::Generated, "Table of Contents", "Stale", {K::Generated, 6, 9, 0, true},
           {{"From", "Heading 1, Heading 2"}, {"Entries", "9"}, {"Built", "Before 2 edits"},
            {"Placed", "Page v"}}});
    o.add({"gen.bib", S::Generated, "Bibliography", "", {K::Generated, 6, 9, 0, false},
           {{"From", "Tagged sources"}, {"Entries", "14"}, {"Built", "Current"},
            {"Placed", "Page 14"}}});

    return o;
}

}  // namespace composix::core
