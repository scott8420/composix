// core::new_document -- a blank document from File > New (s019). Like the
// sample it is LOADED: every write inside a Doc::Loading scope, no step.
#include "core/Doc.hpp"

#include <algorithm>
#include <cmath>

namespace composix::core {

const std::vector<TrimPreset>& trim_presets() {
    static const std::vector<TrimPreset> list = [] {
        const double in = kPt;
        std::vector<TrimPreset> v;
        const auto add = [&](const char* name, double w, double h) {
            v.push_back({std::string(name) + " (" + size_text(w, h, 0) + ")", w, h});
        };
        add("Trade", 6 * in, 9 * in);
        add("Digest", 5.5 * in, 8.5 * in);
        add("Novel", 5 * in, 8 * in);
        add("Royal", 6.14 * in, 9.21 * in);
        add("Crown quarto", 7.44 * in, 9.69 * in);
        add("Large", 7 * in, 10 * in);
        add("Letter", 8.5 * in, 11 * in);
        v.push_back({"A5 (148 \u00d7 210 mm)", 148 / 25.4 * in, 210 / 25.4 * in});   // ISO sizes read in mm
        v.push_back({"A4 (210 \u00d7 297 mm)", 210 / 25.4 * in, 297 / 25.4 * in});
        add("Square", 8.5 * in, 8.5 * in);
        return v;
    }();
    return list;
}

std::string check_new(const NewDoc& n) {
    const double in = kPt;
    if (n.trim_w < 1 * in || n.trim_h < 1 * in) return "The page is too small (1 inch at least).";
    if (n.trim_w > 50 * in || n.trim_h > 50 * in) return "The page is too large (50 inches at most).";
    if (n.pages < 1 || n.pages > 2000) return "Pages: 1 to 2000.";
    const auto& m = n.margins;
    if (m.top < 0 || m.bottom < 0 || m.inside < 0 || m.outside < 0) return "Margins can't be negative.";
    const double lw = n.trim_w - m.inside - m.outside, lh = n.trim_h - m.top - m.bottom;
    if (lw < 0.5 * in || lh < 0.5 * in) return "The margins leave less than half an inch to work in.";
    if (n.columns < 1 || n.columns > 12) return "Columns: 1 to 12.";
    if (n.gutter < 0) return "The gutter can't be negative.";
    if (n.columns > 1 && (lw - n.gutter * (n.columns - 1)) / n.columns < 18)
        return "The columns would be narrower than a quarter inch.";
    if (n.bleed < 0 || n.bleed > 1 * in) return "Bleed: 0 to 1 inch.";
    return {};
}

void new_to_settings(const NewDoc& n, AppSettings& s) {   // s079
    s["new.trim_w"] = num_text(n.trim_w);
    s["new.trim_h"] = num_text(n.trim_h);
    s["new.pages"] = std::to_string(n.pages);
    s["new.facing"] = n.facing ? "1" : "0";
    s["new.m_top"] = num_text(n.margins.top);
    s["new.m_bottom"] = num_text(n.margins.bottom);
    s["new.m_inside"] = num_text(n.margins.inside);
    s["new.m_outside"] = num_text(n.margins.outside);
    s["new.columns"] = std::to_string(n.columns);
    s["new.gutter"] = num_text(n.gutter);
    s["new.bleed"] = num_text(n.bleed);
    s["new.frames"] = n.text_frames ? "1" : "0";
    s["new.folios"] = n.folios ? "1" : "0";
    s["new.units"] = std::to_string(static_cast<int>(n.units));
}

NewDoc new_from_settings(const AppSettings& s) {   // s079
    NewDoc n;
    const auto num = [&](const char* key, double fallback, double lo, double hi) {
        const auto it = s.find(key);
        if (it == s.end()) return fallback;
        try {
            std::size_t used = 0;
            const double v = std::stod(it->second, &used);
            if (used != it->second.size() || !std::isfinite(v)) return fallback;
            return std::clamp(v, lo, hi);
        } catch (...) { return fallback; }
    };
    const auto flag = [&](const char* key, bool fallback) {
        const auto it = s.find(key);
        return it == s.end() ? fallback : it->second == "1";
    };
    n.trim_w = num("new.trim_w", n.trim_w, 36, 5000);
    n.trim_h = num("new.trim_h", n.trim_h, 36, 5000);
    n.pages = static_cast<int>(num("new.pages", n.pages, 1, 2000));
    n.facing = flag("new.facing", n.facing);
    n.margins.top = num("new.m_top", n.margins.top, 0, 2500);
    n.margins.bottom = num("new.m_bottom", n.margins.bottom, 0, 2500);
    n.margins.inside = num("new.m_inside", n.margins.inside, 0, 2500);
    n.margins.outside = num("new.m_outside", n.margins.outside, 0, 2500);
    n.columns = static_cast<int>(num("new.columns", n.columns, 1, 12));
    n.gutter = num("new.gutter", n.gutter, 0, 72);
    n.bleed = num("new.bleed", n.bleed, 0, 72);
    n.text_frames = flag("new.frames", n.text_frames);
    n.folios = flag("new.folios", n.folios);
    n.units = static_cast<Units>(static_cast<int>(num("new.units", 0, 0, 3)));
    return n;
}

bool new_document(Doc& d, const NewDoc& n) {
    if (!check_new(n).empty()) return false;
    d.reset();
    Doc::Loading load(d);

    Settings s;
    s.title = n.title.empty() ? "Untitled" : n.title;
    s.trim_w = n.trim_w;
    s.trim_h = n.trim_h;
    s.bleed = n.bleed;
    s.facing = n.facing;
    s.units = n.units;   // s079
    d.put("doc", s);

    Master m{"A", "Master", 1, n.trim_w, n.trim_h, {}, n.columns, ""};
    m.bleed = n.bleed;     // s147: the master's own
    m.facing = n.facing;
    m.margins = n.margins;
    m.gutter = n.gutter;
    d.put("master.1", m);

    std::vector<Doc::Id> pages;
    for (int i = 1; i <= n.pages; ++i) {
        Page p;
        p.pos = i;
        p.w = n.trim_w;
        p.h = n.trim_h;
        p.master = "master.1";
        pages.push_back("page." + std::to_string(i));
        d.put(pages.back(), p);
    }
    // A book starts on a right-hand page.
    d.put("section.1", Section{"Body", pages.front(), Numbering::Arabic, 1, n.facing ? Side::Right : Side::Any, "master.1"});

    d.put("swatch.1", Swatch{"Black", 1, ColorModel::CMYK, {0, 0, 0, 100}, "", 0x1a1a1a});
    d.put("swatch.2", Swatch{"Paper", 2, ColorModel::CMYK, {0, 0, 0, 0}, "", 0xffffff});

    d.put("style.1", Style{"Body", StyleKind::Paragraph, 1, "Serif", 11, 14, false, false, Align::Left});
    {   // s037: Enter after a heading goes on in Body; a caption is Body made small and italic.
        Style h{"Heading 1", StyleKind::Paragraph, 2, "Serif", 20, 24, true, false, Align::Left, 12, 6};
        h.next = "style.1";
        d.put("style.2", h);
        Style c{"Caption", StyleKind::Paragraph, 3, "Serif", 9, 11, false, true, Align::Left};
        c.based_on = "style.1";
        c.own = doc::style_differences(c, *d.as<Style>("style.1"));
        d.put("style.3", c);
        // s114: footnotes are set in this (the flow finds it by its name).
        Style fn{"Footnote", StyleKind::Paragraph, 3.5, "Serif", 9, 11, false, false, Align::Left};
        fn.based_on = "style.1";
        fn.own = doc::style_differences(fn, *d.as<Style>("style.1"));
        d.put("style.6", fn);
    }
    d.put("style.4", Style{"Emphasis", StyleKind::Character, 4, "", 0, 0, false, true, Align::Left});
    d.put("style.5", Style{"Folio", StyleKind::Paragraph, 5, "Serif", 9, 11, false, false, Align::Center});

    if (n.folios) {   // s023: centred in the bottom margin, across the text width
        const double h = 14, y = n.trim_h - n.margins.bottom / 2 - h / 2;
        d.put("story.2", Story{"Folio", {{"style.5", kMarkPage}}});
        Frame f{"master.1", FrameKind::Text, n.margins.inside, y, n.trim_w - n.margins.inside - n.margins.outside, h, 0, 1,
                "story.2", "", ""};
        d.put("frame." + std::to_string(n.pages + 1), f);   // after the page frames
    }

    if (n.text_frames) {
        d.put("story.1", Story{"Main", {{"style.1", ""}}});
        // A frame in each page's margins (its own side's), columns as the
        // master's, threaded page to page.
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const Rect r = margin_rect(d, pages[i]);
            Frame f{pages[i], FrameKind::Text, r.x, r.y, r.w, r.h, 0, 1, "story.1",
                    i + 1 < pages.size() ? "frame." + std::to_string(i + 2) : "", ""};
            f.columns = n.columns;
            f.gutter = n.gutter;
            d.put("frame." + std::to_string(i + 1), f);
        }
    }
    return true;
}

}  // namespace composix::core
