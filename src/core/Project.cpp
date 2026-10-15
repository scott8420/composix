// core::project -- the document as the navigator / tiles / inspector see it
// (s004). Replaces s002's fixture through the same seam: the views read an
// Outline and never the Doc.
//
// Everything shown is DERIVED, never stored twice: folios from sections, "used
// by" from pages, word counts from stories, link status from the disk,
// effective ppi from pixels over the placed frame's width, a contents list's
// staleness from its last build against today's headings.
#include "core/Doc.hpp"
#include "core/Outline.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace composix::core {

namespace fs = std::filesystem;

namespace {

int words(const std::string& s) {
    std::istringstream in(s);
    std::string w;
    int n = 0;
    while (in >> w) ++n;
    return n;
}

std::string with_commas(int n) {
    std::string s = std::to_string(n), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++c;
    }
    return out;
}

// A run of folios: "i–iv", "7", "" when empty.
std::string folio_range(const Doc& d, const std::vector<Doc::Id>& pages) {
    if (pages.empty()) return "—";
    if (pages.size() == 1) return d.folio(pages.front());
    return d.folio(pages.front()) + "–" + d.folio(pages.back());
}

std::string folios(const Doc& d, const std::vector<Doc::Id>& pages) {
    if (pages.empty()) return "—";
    std::string s = pages.size() == 1 ? "Page " : "Pages ";
    for (std::size_t i = 0; i < pages.size(); ++i) s += (i ? ", " : "") + d.folio(pages[i]);
    return s;
}

std::string style_name(const Doc& d, const Doc::Id& id) {
    const auto* s = d.as<Style>(id);
    return s ? s->name : "—";
}

// Tints for tiles that have no colour of their own (sections, images).
constexpr std::uint32_t kTints[] = {0x5b7fa6, 0x8fa37a, 0xc98f5a, 0x8a7fb0, 0x6b9e9a, 0x9a8f88};
std::uint32_t tint(std::size_t i) { return kTints[i % std::size(kTints)]; }

std::string swatch_value(const Swatch& s) {
    auto n = [](double v) { std::ostringstream o; o << std::lround(v); return o.str(); };
    switch (s.model) {
    case ColorModel::CMYK: return n(s.v[0]) + " / " + n(s.v[1]) + " / " + n(s.v[2]) + " / " + n(s.v[3]);
    case ColorModel::RGB:  return n(s.v[0]) + " / " + n(s.v[1]) + " / " + n(s.v[2]);
    case ColorModel::Gray: return n(s.v[0]) + "%";
    case ColorModel::Spot: return s.spot;
    }
    return {};
}

const char* model_text(ColorModel m) {
    switch (m) {
    case ColorModel::CMYK: return "CMYK";
    case ColorModel::RGB:  return "RGB";
    case ColorModel::Gray: return "Gray";
    case ColorModel::Spot: return "Spot";
    }
    return "?";
}

const char* align_text(Align a) {
    switch (a) {
    case Align::Left:    return "Left";
    case Align::Center:  return "Center";
    case Align::Right:   return "Right";
    case Align::Justify: return "Justified";
    }
    return "?";
}

std::string pts(double v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

}  // namespace

Outline project(const Doc& d, const std::string& dir) {
    Outline o;
    using G = Segment;
    using K = ThumbKind;
    const auto pages = d.pages();

    // ── choices the inspector offers (shared by every item that edits them) ──
    std::vector<Choice> master_choices{{"", "None"}};
    for (const auto& m : d.ids_of<Master>()) master_choices.push_back({m, master_label(d, m)});
    std::vector<Choice> size_choices;
    const auto presets = doc::size_presets(d.settings());
    for (std::size_t i = 0; i < presets.size(); ++i) size_choices.push_back({std::to_string(i), presets[i].label});
    std::vector<Choice> numbering_choices;
    for (int i = 0; i <= static_cast<int>(Numbering::AlphaUpper); ++i)
        numbering_choices.push_back({std::to_string(i), numbering_text(static_cast<Numbering>(i))});
    std::vector<Choice> side_choices;
    for (int i = 0; i <= static_cast<int>(Side::Left); ++i)
        side_choices.push_back({std::to_string(i), side_text(static_cast<Side>(i))});

    // Paragraph counts per style, across every story (styles' "Used", TOC entries).
    std::map<Doc::Id, int> used;
    for (const auto& sid : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(sid)->paras) ++used[p.style];

    // ── Document: the sections ──────────────────────────────────────────────
    {
        std::size_t i = 0;
        for (const auto& sid : d.ids_of<Section>()) {
            const auto& s = *d.as<Section>(sid);
            o.add({sid, G::Document, s.name, "", {K::Section, 6, 9, tint(i++)},
                   {{"Pages", folio_range(d, d.pages_of(sid))},
                    {"Numbering", numbering_text(s.numbering), "numbering", numbering_choices},
                    {"Starts", side_text(s.starts), "starts", side_choices},
                    {"Master", master_label(d, s.master)}}});
        }
    }

    // ── Pages ───────────────────────────────────────────────────────────────
    for (const auto& pid : pages) {
        const auto& p = *d.as<Page>(pid);
        const std::string sec = d.section_of(pid);
        Thumb th{K::Page, p.w / kPt, p.h / kPt, 0xffffff, !p.folds.empty()};
        th.panels = static_cast<int>(p.folds.size()) + 1;
        o.add({pid, G::Pages, "Page " + d.folio(pid), p.folds.empty() ? "" : "Foldout", th,
               {{"Folio", d.folio(pid)},
                {"Size", size_text(p.w, p.h, p.folds.size()), "size", size_choices},
                {"Master", master_label(d, p.master), "master", master_choices},
                {"Section", sec.empty() ? "—" : d.as<Section>(sec)->name},
                {"Side", side_text(d.side_of(pid))}}});
    }

    // ── Masters ─────────────────────────────────────────────────────────────
    for (const auto& mid : d.ids_of<Master>()) {
        const auto& m = *d.as<Master>(mid);
        int n = 0;
        for (const auto& pid : pages) n += d.as<Page>(pid)->master == mid;
        Thumb th{K::Page, m.w / kPt, m.h / kPt, 0xffffff, !m.folds.empty()};
        th.panels = static_cast<int>(m.folds.size()) + 1;
        o.add({mid, G::Masters, master_label(d, mid), m.folds.empty() ? "" : "Foldout", th,
               {{"Size", size_text(m.w, m.h, m.folds.size())},
                {"Columns", std::to_string(m.columns)},
                {"Based on", m.based_on.empty() ? "—" : master_label(d, m.based_on)},
                {"Used by", n == 1 ? "1 page" : std::to_string(n) + " pages"}}});
    }

    // ── Stories ─────────────────────────────────────────────────────────────
    for (const auto& sid : d.ids_of<Story>()) {
        const auto& s = *d.as<Story>(sid);
        int w = 0;
        std::map<std::string, int> styles;
        std::string sample;
        for (const auto& p : s.paras) {
            w += words(p.text);
            ++styles[p.style];
            if (sample.size() < 60) sample += (sample.empty() ? "" : " ") + p.text;
        }
        std::string top;
        int best = 0;
        for (const auto& [st, n] : styles) if (n > best) { best = n; top = st; }
        int frames = 0;
        for (const auto& fid : d.ids_of<Frame>()) frames += d.as<Frame>(fid)->story == sid;
        o.add({sid, G::Stories, s.name, "", {K::Text, 6, 9, 0, false, sample},
               {{"Words", with_commas(w)},
                {"Paragraphs", std::to_string(s.paras.size())},
                {"Frames", frames > 1 ? std::to_string(frames) + ", threaded" : std::to_string(frames)},
                {"Style", style_name(d, top)}}});
    }

    // ── Styles ──────────────────────────────────────────────────────────────
    for (const auto& sid : d.ids_of<Style>()) {
        const auto& s = *d.as<Style>(sid);
        std::string font = s.family;
        if (s.bold) font += " Bold";
        if (s.italic) font += " Italic";
        if (s.size > 0) font += " " + pts(s.size) + "/" + pts(s.leading);
        Thumb th{K::Style, 6, 9, 0x222222, false, "Aa"};
        th.bold = s.bold;
        th.italic = s.italic;
        std::vector<Field> f{{"Kind", s.kind == StyleKind::Paragraph ? "Paragraph" : "Character"},
                             {"Font", font}};
        if (s.kind == StyleKind::Paragraph) f.push_back({"Align", align_text(s.align)});
        f.push_back({"Used", s.kind == StyleKind::Paragraph ? std::to_string(used[sid]) + "×" : "—"});
        o.add({sid, G::Styles, s.name, "", th, f});
    }

    // ── Links & assets ──────────────────────────────────────────────────────
    {
        std::size_t i = 0;
        for (const auto& aid : d.ids_of<Asset>()) {
            const auto& a = *d.as<Asset>(aid);
            // Where it's placed, and how sharp it lands there (pixels over the
            // widest placement's width in inches).
            std::vector<Doc::Id> on;
            double widest = 0;
            for (const auto& fid : d.ids_of<Frame>()) {
                const auto& f = *d.as<Frame>(fid);
                if (f.asset != aid) continue;
                if (d.as<Page>(f.owner) && std::find(on.begin(), on.end(), f.owner) == on.end())
                    on.push_back(f.owner);
                widest = std::max(widest, f.w);
            }
            std::sort(on.begin(), on.end(),
                      [&](const Doc::Id& x, const Doc::Id& y) { return d.page_index(x) < d.page_index(y); });
            std::string ppi = "—";
            if (a.px_w == 0) ppi = "Vector";
            else if (widest > 0) ppi = std::to_string(static_cast<int>(std::lround(a.px_w / (widest / kPt))));

            // Status: a linked file is looked for where it says; relative paths
            // need a saved project to be relative TO.
            bool found = false;
            const fs::path p(a.path);
            std::error_code ec;
            if (p.is_absolute()) found = fs::exists(p, ec);
            else if (!dir.empty()) found = fs::exists(fs::path(dir) / p, ec);
            std::string status;
            if (a.embedded) status = (dir.empty() || found) ? "Embedded" : "Missing from project";
            else            status = found ? "OK" : "Missing";
            const bool bad = status.rfind("Missing", 0) == 0;

            const double aw = a.px_w ? a.px_w : 1, ah = a.px_h ? a.px_h : 1;
            o.add({aid, G::Assets, a.name, bad ? "Missing" : "", {K::Image, aw, ah, tint(i++), bad},
                   {{"Link", a.embedded ? "Embedded" : "Linked"},
                    {"Status", status},
                    {"Color", a.color},
                    {"Pixels", a.px_w ? std::to_string(a.px_w) + " × " + std::to_string(a.px_h) : "Vector"},
                    {"Effective ppi", ppi},
                    {"Placed", folios(d, on)}}});
        }
    }

    // ── Colors ──────────────────────────────────────────────────────────────
    for (const auto& sid : d.ids_of<Swatch>()) {
        const auto& s = *d.as<Swatch>(sid);
        std::string badge = s.model == ColorModel::Spot ? "Spot" : (s.model == ColorModel::RGB ? "RGB" : "");
        std::string type = s.model == ColorModel::Spot ? "Spot"
                         : s.model == ColorModel::RGB  ? "Process (converts at output)" : "Process";
        o.add({sid, G::Colors, s.name, badge, {K::Swatch, 1, 1, s.preview},
               {{"Model", model_text(s.model)}, {"Value", swatch_value(s)}, {"Type", type}}});
    }

    // ── Generated lists ─────────────────────────────────────────────────────
    for (const auto& lid : d.ids_of<GenList>()) {
        const auto& l = *d.as<GenList>(lid);
        int now = 0;
        std::string from;
        for (const auto& st : l.from_styles) {
            now += used[st];
            from += (from.empty() ? "" : ", ") + style_name(d, st);
        }
        const bool never = l.built_entries < 0;
        const bool stale = !never && l.built_entries != now;
        std::string built = never ? "Never" : stale ? "Stale — built with " + std::to_string(l.built_entries)
                                                    : "Current";
        o.add({lid, G::Generated, l.name, stale || never ? "Stale" : "", {K::Generated, 6, 9, 0, stale || never},
               {{"From", from.empty() ? "—" : from},
                {"Entries", std::to_string(now)},
                {"Built", built},
                {"Placed", l.placed_on.empty() ? "—" : "Page " + d.folio(l.placed_on)}}});
    }

    return o;
}

}  // namespace composix::core
