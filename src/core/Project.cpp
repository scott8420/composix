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
#include "core/Hyphen.hpp"   // s190

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

// s041: the fields whose values can be typed -- by key: the unit a bare
// number is in (points: 1, inches: 72) and the range it must fall in.
// s073: `doc_unit` -- points per the document's unit; the entries marked
// 72 (inches, before s073) take it.
void mark_typed(std::vector<Field>& fields, double doc_unit = kPt) {
    struct T { const char* key; double unit, lo, hi; };
    static const T typed[] = {
        {"ssize", 1, 0, 1296}, {"leading", 1, 0, 1296}, {"before", 1, 0, 1000}, {"after", 1, 0, 1000},
        {"lindent", 1, 0, 1000}, {"rindent", 1, 0, 1000}, {"findent", 1, -1000, 1000},
        {"mtop", 72, 0, 5000}, {"mbottom", 72, 0, 5000}, {"minside", 72, 0, 5000}, {"moutside", 72, 0, 5000},
        {"mgutter", 1, 0, 500}, {"gutter", 1, 0, 500}, {"inset", 1, 0, 500}, {"wrap_off", 1, 0, 500}, {"colw", 1, 1, 20000},
        {"lsize", 1, 0, 1296}, {"bounds", 72, -20000, 20000},
        {"list_hang", 1, 0, 144}, {"rule_a_w", 1, 0, 72}, {"rule_b_w", 1, 0, 72}, {"rule_a_o", 1, -500, 500}, {"rule_b_o", 1, -500, 500},
        {"col_rule", 1, 0, 36}, {"col_rule_inset", 1, 0, 500},   // s055
        {"dnote_rule_w", 1, 0.1, 6}, {"dnote_space", 1, 0, 36},   // s118
        {"dtrim", 72, 36, 5000}, {"dbleed", 72, 0, 72},   // s066
        {"msize", 72, 36, 5000}, {"mbleed", 72, 0, 72},   // s148
        {"mgrid", 1, 0, 144}, {"split_gutter", 1, 0, 500},   // s183   // s150 (Scott): a baseline grid of any measure, the presets beside
        {"pic_off", 72, -20000, 20000},   // s076
        {"plindent", 1, 0, 1000}, {"prindent", 1, 0, 1000}, {"pfindent", 1, -1000, 1000}, {"pbefore", 1, 0, 1000}, {"pafter", 1, 0, 1000},   // s093
        {"ptop", 72, 0, 5000}, {"pbottom", 72, 0, 5000}, {"pinside", 72, 0, 5000}, {"poutside", 72, 0, 5000}};   // s062
    for (auto& f : fields)
        for (const auto& t : typed)
            if (f.edit == t.key) { f.unit = t.unit == 72 ? doc_unit : t.unit; f.lo = t.lo; f.hi = t.hi; }
    // s071: tints and opacity -- percentages typed, the presets beside.
    struct P { const char* key; double lo; };
    static const P pct[] = {{"fill_tint", 1}, {"stroke_tint", 1}, {"color_tint", 1}, {"col_rule_tint", 1},
                            {"rule_a_t", 1}, {"rule_b_t", 1}, {"opacity", 0}};
    for (auto& f : fields)
        for (const auto& p : pct)
            if (f.edit == p.key) { f.percent = true; f.lo = p.lo; f.hi = 100; }
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

// s161 (Scott): a style's rows in groups the inspector folds -- Text,
// Paragraph, Flow, List, Rules, Style.
std::string style_group(const Field& f) {
    static const std::map<std::string, std::string> by_edit{
        {"font", "Text"}, {"ssize", "Text"}, {"leading", "Text"}, {"bold", "Text"}, {"italic", "Text"}, {"nobreak", "Text"},
        {"color", "Text"}, {"color_tint", "Text"},
        {"align", "Paragraph"}, {"before", "Paragraph"}, {"after", "Paragraph"}, {"lindent", "Paragraph"}, {"rindent", "Paragraph"},
        {"findent", "Paragraph"}, {"indent_after", "Paragraph"}, {"tabs", "Paragraph"}, {"drop_lines", "Paragraph"},
        {"drop_chars", "Paragraph"}, {"span", "Paragraph"}, {"split_gutter", "Paragraph"},
        {"align_grid", "Flow"}, {"hyphenate", "Flow"}, {"lang", "Flow"}, {"hyph_limit", "Flow"}, {"hyph_shortest", "Flow"}, {"hyph_last_word", "Flow"},
        {"keep_next", "Flow"}, {"keep_lines", "Flow"}, {"pstart", "Flow"},
        {"list_type", "List"}, {"list_bullet", "List"}, {"list_num", "List"}, {"list_cont", "List"}, {"list_level", "List"},
        {"list_hang", "List"},
        {"based_on", "Style"}, {"style_base", "Style"}, {"next_style", "Style"}};   // (Duplicate / Delete: under all, always in view)
    if (const auto it = by_edit.find(f.edit); it != by_edit.end()) return it->second;
    if (f.edit.rfind("rule_", 0) == 0) return "Rules";
    if (f.label == "Own settings" || f.label == "Styles on it" || f.label == "Used") return "Style";
    if (f.label == "Font") return "Text";   // (read-only when the fonts aren't known)
    return "";   // Kind: above the groups
}

// s073: a unit as the Units dropdown reads it.
std::string unit_label(Units u) {
    switch (u) {
    case Units::Millimetres: return "Millimetres";
    case Units::Picas:       return "Picas";
    case Units::Points:      return "Points";
    default:                 return "Inches";
    }
}

// s067: four sides as a typed compact row, T R B L, with the link.
Field sides_row(const std::string& label, const Sides& s, const std::string& key, std::vector<Choice> presets) {
    Field f{label, pts(s.top) + "\t" + pts(s.right) + "\t" + pts(s.bottom) + "\t" + pts(s.left), key, std::move(presets)};
    f.cols = {"T", "R", "B", "L"};
    f.link = true;
    return f;
}

}  // namespace

Outline project(const Doc& d, const std::string& dir, const std::vector<std::string>* overset,
                const std::vector<std::string>* fonts, const std::map<std::string, std::string>* stale_lists) {
    Outline o;
    using G = Segment;
    using K = ThumbKind;
    const auto pages = d.pages();

    // ── choices the inspector offers (shared by every item that edits them) ──
    std::vector<Choice> master_choices{{"", "None"}};
    for (const auto& m : d.ids_of<Master>()) master_choices.push_back({m, master_label(d, m)});
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

    // ── Document: its settings (s066), then the sections ────────────────────
    {
        const auto& st = d.settings();
        Field title{"Title", st.title, "dtitle"};
        title.text = true;
        const auto U = st.units;   // s073
        // s148 (Scott): the page settings -- trim, bleed, facing -- are each master's now (Masters).
        std::vector<Field> f{title,
                             {"Units", unit_label(U), "dunits",   // s073
                              {{"0", unit_label(Units::Inches)}, {"1", unit_label(Units::Millimetres)},
                               {"2", unit_label(Units::Picas)}, {"3", unit_label(Units::Points)}}},
                             {"Text wrap", st.wrap_beneath ? "Text beneath" : "All text", "dwrap",   // s069
                              {{"0", "All text"}, {"1", "Text beneath"}}},
                             {"Ruler origin", st.ruler_origin == RulerOrigin::Page ? "Page"   // s154
                                              : st.ruler_origin == RulerOrigin::Spine ? "Spine" : "Spread",
                              "dorigin", {{"0", "Spread"}, {"1", "Page"}, {"2", "Spine"}}},
                             {"Footnotes", st.note_restart == NoteRestart::Page ? "Each page"   // s111
                                           : st.note_restart == NoteRestart::Section ? "Each section"
                                           : st.note_restart == NoteRestart::Chapter ? "Each chapter" : "Through the story",   // s124
                              "dnote_restart", {{"0", "Through the story"}, {"1", "Each page"}, {"2", "Each section"}, {"3", "Each chapter"}}},
                             {"In the note", st.note_num == Settings::NoteNum::Period ? "1." : st.note_num == Settings::NoteNum::Paren ? "1)"   // s126
                                             : st.note_num == Settings::NoteNum::Raised ? "Raised" : "1",
                              "dnote_num", {{"0", "1"}, {"1", "1."}, {"2", "1)"}, {"3", "Raised"}}},
                             {"Numbers", st.note_style == NoteStyle::RomanLower ? "i ii iii"
                                         : st.note_style == NoteStyle::AlphaLower ? "a b c"
                                         : st.note_style == NoteStyle::Symbols ? "* \u2020 \u2021" : "1 2 3",
                              "dnote_style", {{"0", "1 2 3"}, {"1", "i ii iii"}, {"2", "a b c"}, {"3", "* \u2020 \u2021"}}},
                             [&] {   // s117: the most of a column the notes may take
                                 Field a{"Footnote area", st.note_area >= 100 ? "Whole column" : std::to_string(st.note_area) + "%",
                                         "dnote_area", {{"100", "Whole column"}, {"75", "75%"}, {"50", "50%"}, {"33", "33%"}, {"25", "25%"}}};
                                 a.percent = true;
                                 a.lo = 10;
                                 a.hi = 100;
                                 return a;
                             }(),
                             [&] {   // s118: the rule over the notes
                                 Field r{"Footnote rule", st.note_rule <= 0 ? "None" : st.note_rule >= 100 ? "Full width"
                                                                                  : std::to_string(st.note_rule) + "%",
                                         "dnote_rule", {{"0", "None"}, {"25", "25%"}, {"33", "33%"}, {"50", "50%"}, {"100", "Full width"}}};
                                 r.percent = true;
                                 r.lo = 0;
                                 r.hi = 100;
                                 return r;
                             }(),
                             {"Rule weight", pts(st.note_rule_w) + " pt", "dnote_rule_w",
                              {{"0.25", "0.25 pt"}, {"0.5", "0.5 pt"}, {"0.75", "0.75 pt"}, {"1", "1 pt"}, {"2", "2 pt"}}},
                             {"Between notes", pts(st.note_space) + " pt", "dnote_space",
                              {{"0", "0 pt"}, {"2", "2 pt"}, {"3", "3 pt"}, {"4", "4 pt"}, {"6", "6 pt"}}},
                             [&] {
                                 Field c{"Continued line", st.note_cont, "dnote_cont", {}};
                                 c.text = true;
                                 return c;
                             }(),
                             {"Notes at", st.note_at == NoteAt::StoryEnd ? "End of the story"   // s120
                                          : st.note_at == NoteAt::ChapterEnd ? "End of each chapter" : "Foot of the column",   // s122
                              "dnote_at", {{"0", "Foot of the column"}, {"1", "End of the story"}, {"2", "End of each chapter"}}},
                             [&] {
                                 Field t{"Endnotes title", st.note_title, "dnote_title", {}};
                                 t.text = true;
                                 return t;
                             }(),
                             {"Length", std::to_string(d.pages().size()) + (d.pages().size() == 1 ? " page" : " pages")}};
        mark_typed(f, unit_pt(d.settings().units));
        Thumb th{K::Section, 6, 9, 0x9a8f88};
        if (!d.pages().empty())   // s149: the first page's master, as a mock-up (else the stock picture)
            if (const auto* m = d.as<Master>(d.as<Page>(d.pages().front())->master)) {
                th.w_in = (m->facing && m->folds.empty() ? 2 * m->w : m->w) / kPt;
                th.h_in = m->h / kPt;
                th.page = "mockup:" + d.as<Page>(d.pages().front())->master;
            }
        o.add({"doc", G::Document, "Document", "", th, f});   // s148: was "Document settings"
    }
    {
        std::size_t i = 0;
        for (const auto& sid : d.ids_of<Section>()) {
            const auto& s = *d.as<Section>(sid);
            // s060: name and first number typed; the master chosen; Remove.
            Field name{"Name", s.name, "sname"};
            name.text = true;
            Field at{"Starts at", std::to_string(s.start_at), "sstart_at"};
            at.text = true;
            std::vector<Choice> mc{{"", "None"}};
            for (const auto& mid : d.ids_of<Master>()) mc.push_back({mid, master_label(d, mid)});
            std::vector<Field> f{name,
                                 {"Pages", folio_range(d, d.pages_of(sid))},
                                 {"Numbering", numbering_text(s.numbering), "numbering", numbering_choices},
                                 at,
                                 {"Starts", side_text(s.starts), "starts", side_choices},
                                 {"Master", master_label(d, s.master), "smaster", mc}};
            const auto pages_all = d.pages();
            if (pages_all.empty() || s.start_page != pages_all.front()) {   // the first section stays
                Field act{"", "", "section", {{"remove", "Remove section"}}};
                act.buttons = true;
                f.push_back(act);
            }
            Thumb th{K::Section, 6, 9, tint(i++)};
            {   // s149: its master as a mock-up (its own, else its first page's)
                std::string mid = s.master;
                if (!d.as<Master>(mid))
                    if (const auto* p = d.as<Page>(s.start_page)) mid = p->master;
                if (const auto* m = d.as<Master>(mid)) {
                    th.w_in = (m->facing && m->folds.empty() ? 2 * m->w : m->w) / kPt;
                    th.h_in = m->h / kPt;
                    th.page = "mockup:" + mid;
                }
            }
            o.add({sid, G::Document, s.name, "", th, f});
        }
    }

    // ── Pages ───────────────────────────────────────────────────────────────
    for (const auto& pid : pages) {
        const auto& p = *d.as<Page>(pid);
        const std::string sec = d.section_of(pid);
        Thumb th{K::Page, p.w / kPt, p.h / kPt, 0xffffff, !p.folds.empty()};
        th.panels = static_cast<int>(p.folds.size()) + 1;
        th.page = pid;   // s020: drawn for real
        // s014: what can be done to pages, as buttons.
        bool starts = false;
        for (const auto& sid : d.ids_of<Section>()) starts = starts || d.as<Section>(sid)->start_page == pid;
        Field acts{"", "", "page", {{"add", "Add page after"}, {"delete", "Delete"}}};
        if (!starts) acts.choices.push_back({"section", "Start section"});
        acts.buttons = true;
        std::vector<Field> pf{{"Folio", d.folio(pid)},
                              {"Size", size_text(p.w, p.h, p.folds.size(), d.settings().units), "size", [&] {   // s148: its master's size the trim
                                   std::vector<Choice> c;
                                   const auto pr = doc::size_presets(d, pid);
                                   for (std::size_t i = 0; i < pr.size(); ++i) c.push_back({std::to_string(i), pr[i].label});
                                   return c;
                               }()},
                              {"Master", master_label(d, p.master), "master", master_choices},
                              {"Section", sec.empty() ? "—" : d.as<Section>(sec)->name},
                              {"Side", side_text(d.side_of(pid))}};
        {   // s062: its own margins, or the master's.
            pf.push_back({"Margins", p.margins ? "This page's own" : "Master's", "pmargins",
                          {{"master", "Master's"}, {"own", "This page's own"}}});
            if (const auto* mg = margins_of(d, pid); mg && p.margins) {
                static const double ms[] = {0, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1, 1.25, 1.5, 2};
                std::vector<Choice> mc;
                const auto U = d.settings().units;   // s073
                for (double v : ms) mc.push_back({pts(v * kPt), length_text(v * kPt, U)});
                pf.push_back({"Top", length_text(mg->top, U), "ptop", mc});
                pf.push_back({"Bottom", length_text(mg->bottom, U), "pbottom", mc});
                pf.push_back({"Inside", length_text(mg->inside, U), "pinside", mc});
                pf.push_back({"Outside", length_text(mg->outside, U), "poutside", mc});
            }
        }
        {   // s072: its master's items -- how many show, how many it has made its own.
            const auto open = doc::overridable_items(d, pid);
            int own = 0;
            for (const auto& f : d.frames_on(pid)) own += d.as<Frame>(f)->overrides.empty() ? 0 : 1;
            if (!open.empty() || own > 0) {
                std::string v = open.empty() ? std::string{} : std::to_string(open.size()) + " from the master";
                if (own) v += (v.empty() ? "" : ", ") + std::to_string(own) + " overridden";
                pf.push_back({"Master items", v});
                Field mi{"", "", "pover", {}};
                if (!open.empty()) mi.choices.push_back({"all", "Override all"});
                if (own > 0) mi.choices.push_back({"none", "Remove overrides"});
                mi.buttons = true;
                pf.push_back(mi);
            }
        }
        pf.push_back(acts);
        mark_typed(pf, unit_pt(d.settings().units));
        o.add({pid, G::Pages, "Page " + d.folio(pid), p.folds.empty() ? "" : "Foldout", th, pf});
    }

    // ── Masters ─────────────────────────────────────────────────────────────
    for (const auto& mid : d.ids_of<Master>()) {
        const auto& m = *d.as<Master>(mid);
        int n = 0;
        for (const auto& pid : pages) n += d.as<Page>(pid)->master == mid;
        // s149 (Scott): a master's tile is its mock-up -- its spread when it faces, the
        // margins, columns, grid and guides over its own items, numbers filled in.
        Thumb th{K::Page, (m.facing && m.folds.empty() ? 2 * m.w : m.w) / kPt, m.h / kPt, 0xffffff, false};
        th.page = "mockup:" + mid;
        // s014: margins and columns, editable.
        static const double margins[] = {0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1, 1.25, 1.5, 2, 2.5, 3};
        std::vector<Choice> mc;
        const auto U = d.settings().units;   // s073
        for (double v : margins) mc.push_back({pts(v * kPt), length_text(v * kPt, U)});
        std::vector<Choice> cc, gc;
        for (int k = 1; k <= 6; ++k) cc.push_back({std::to_string(k), std::to_string(k)});
        for (double g : {6.0, 9.0, 12.0, 14.0, 18.0, 24.0, 36.0}) gc.push_back({pts(g), pts(g) + " pt"});
        const auto in = [U](double v) { return length_text(v, U); };
        // s148 (Scott): every page setting is the master's -- its name, size, bleed, facing, then the rest.
        Field name{"Name", m.name, "mname"};
        name.text = true;
        Field size{"Size (" + unit_name(U) + ")", length_in(m.w, U) + "\t" + length_in(m.h, U)};
        size.cols = {"W", "H"};
        size.edit = "msize";
        std::vector<Choice> bc;
        for (double v : {0.0, 0.0625, 0.125, 0.25}) bc.push_back({pts(v * kPt), length_text(v * kPt, U)});
        std::vector<Field> f{name, size,
                             {"Bleed", length_text(m.bleed, U), "mbleed", bc},
                             {"Pages", m.facing ? "Facing" : "Single", "mfacing", {{"1", "Facing"}, {"0", "Single"}}},
                             {"Columns", std::to_string(m.columns), "mcolumns", cc}};
        if (m.columns > 1) f.push_back({"Gutter", pts(m.gutter) + " pt", "mgutter", gc});
        {   // s036: the baseline grid, from the top margin.
            std::vector<Choice> grc{{"0", "None"}};
            for (double g : {9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 18.0, 24.0}) grc.push_back({pts(g), pts(g) + " pt"});
            f.push_back({"Baseline grid", m.grid >= 1 ? pts(m.grid) + " pt" : "None", "mgrid", grc});
        }
        f.push_back({"Top", in(m.margins.top), "mtop", mc});
        f.push_back({"Bottom", in(m.margins.bottom), "mbottom", mc});
        f.push_back({"Inside", in(m.margins.inside), "minside", mc});
        f.push_back({"Outside", in(m.margins.outside), "moutside", mc});
        f.push_back({"Based on", m.based_on.empty() ? "—" : master_label(d, m.based_on)});
        f.push_back({"Used by", n == 1 ? "1 page" : std::to_string(n) + " pages"});
        mark_typed(f, unit_pt(d.settings().units));   // s041
        o.add({mid, G::Masters, master_label(d, mid), m.folds.empty() ? "" : "Foldout", th, f});
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
            if (sample.size() < 60) {
                std::string t = p.text;   // s119: footnote marks left out (a private-use glyph on the tile)
                for (std::size_t i = t.find(kMarkNote); i != std::string::npos; i = t.find(kMarkNote, i)) t.erase(i, 3);
                sample += (sample.empty() ? "" : " ") + t;
            }
        }
        std::string top;
        int best = 0;
        for (const auto& [st, n] : styles) if (n > best) { best = n; top = st; }
        int frames = 0;
        for (const auto& fid : d.ids_of<Frame>()) frames += d.as<Frame>(fid)->story == sid;
        const bool over = overset && std::find(overset->begin(), overset->end(), sid) != overset->end();
        std::vector<Field> fields{{"Words", with_commas(w)},
                                  {"Paragraphs", std::to_string(s.paras.size())},
                                  {"Frames", frames > 1 ? std::to_string(frames) + ", threaded" : std::to_string(frames)},
                                  {"Style", style_name(d, top)}};
        if (overset) fields.push_back({"Overset", over ? "Yes" : (frames ? "No" : "Not placed")});
        o.add({sid, G::Stories, s.name, over ? "Overset" : "", {K::Text, 6, 9, 0, over, sample}, fields});
    }

    // ── Styles ──────────────────────────────────────────────────────────────
    // s011: every setting editable (dropdowns), plus Duplicate / Delete.
    {
        static const double sizes[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 36, 42, 48, 60, 72};
        static const double spaces[] = {0, 2, 3, 4, 6, 8, 9, 12, 18, 24, 36};
        const auto pt = [](double v) { return pts(v) + " pt"; };
        std::vector<Choice> size_ch, lead_ch{{"0", "Auto"}}, space_ch;
        for (double v : sizes) { size_ch.push_back({pts(v), pt(v)}); lead_ch.push_back({pts(v), pt(v)}); }
        for (double v : spaces) space_ch.push_back({pts(v), pt(v)});
        std::vector<Choice> font_ch;
        if (fonts) for (const auto& f : *fonts) font_ch.push_back({f, f});
        std::vector<Choice> align_ch;
        for (int i = 0; i < 4; ++i) align_ch.push_back({std::to_string(i), align_text(static_cast<Align>(i))});
        for (const auto& sid : d.ids_of<Style>()) {
            const auto& s = *d.as<Style>(sid);
            const bool para = s.kind == StyleKind::Paragraph;
            Thumb th{K::Style, 6, 9, 0x222222, false, "Aa"};
            th.bold = s.bold;
            th.italic = s.italic;
            th.family = s.family;
            const int uses = doc::style_uses(d, sid);
            std::vector<Field> f{{"Kind", para ? "Paragraph" : "Character"}};
            // A character style leaves unset what it doesn't change ("Inherit").
            auto fc = font_ch, sc = size_ch;
            if (!para) { fc.insert(fc.begin(), {"", "Inherit"}); sc.insert(sc.begin(), {"0", "Inherit"}); }
            if (!fonts) fc.clear();
            f.push_back({"Font", s.family.empty() ? "Inherit" : s.family, fc.empty() ? "" : "font", fc});
            // s041: "ssize" -- "size" is the page's (its handler took this one too).
            f.push_back({"Size", s.size > 0 ? pt(s.size) : "Inherit", "ssize", sc});
            if (para) f.push_back({"Leading", s.leading > 0 ? pt(s.leading) : "Auto", "leading", lead_ch});
            f.push_back({"Weight", s.bold ? "Bold" : "Regular", "bold", {{"0", "Regular"}, {"1", "Bold"}}});
            f.push_back({"Slant", s.italic ? "Italic" : "Roman", "italic", {{"0", "Roman"}, {"1", "Italic"}}});
            if (!para) f.push_back({"No break", s.no_break ? "On" : "Off", "nobreak", {{"0", "Off"}, {"1", "On"}}});   // s075
            // s015: its colour, a swatch.
            {
                std::vector<Choice> col{{"", para ? "Black" : "Inherit"}};
                for (const auto& sw : d.ids_of<Swatch>()) col.push_back({sw, d.as<Swatch>(sw)->name});
                const auto* cur = d.as<Swatch>(s.color);
                f.push_back({"Color", cur ? cur->name : (para ? "Black" : "Inherit"), "color", col});
                if (cur || para) f.push_back({"Tint", tint_text(s.color_tint), "color_tint", tint_choices()});   // s021
            }
            if (para) {
                f.push_back({"Align", align_text(s.align), "align", align_ch});
                f.push_back({"Space before", pt(s.space_before), "before", space_ch});
                f.push_back({"Space after", pt(s.space_after), "after", space_ch});
                // s026: indents and tabs.
                std::vector<Choice> ind_ch, first_ch;
                for (double v : {0.0, 6.0, 9.0, 12.0, 18.0, 24.0, 36.0, 54.0, 72.0}) ind_ch.push_back({pts(v), pt(v)});
                for (double v : {0.0, 9.0, 12.0, 14.0, 18.0, 24.0, 36.0, -9.0, -12.0, -18.0}) first_ch.push_back({pts(v), pt(v)});
                f.push_back({"Left indent", pt(s.left_indent), "lindent", ind_ch});
                f.push_back({"Right indent", pt(s.right_indent), "rindent", ind_ch});
                f.push_back({"First indent", pt(s.first_indent), "findent", first_ch});
                if (s.first_indent != 0)
                    f.push_back({"Indent when", s.skip_after_heading ? "Not after a heading" : "Always", "indent_after",
                                 {{"0", "Always"}, {"1", "Not after a heading"}}});
                std::vector<Choice> tab_ch;
                for (const auto& [id, label] : tab_presets(d)) tab_ch.push_back({id, label});
                f.push_back({"Tabs", tabs_text(s.tabs), "tabs", tab_ch});
                // s029: drop cap.
                f.push_back({"Drop cap", s.drop_lines >= 2 ? std::to_string(s.drop_lines) + " lines" : "None", "drop_lines",
                             {{"0", "None"}, {"2", "2 lines"}, {"3", "3 lines"}, {"4", "4 lines"}, {"5", "5 lines"}}});
                if (s.drop_lines >= 2)
                    f.push_back({"Drop letters", std::to_string(s.drop_chars), "drop_chars", {{"1", "1"}, {"2", "2"}, {"3", "3"}}});
                // s166: across all the frame's columns.
                // s183: or split into sub-columns (a one-column frame's).
                f.push_back({"Span columns",
                             s.span ? "All columns" : s.split >= 2 ? "Split into " + std::to_string(s.split) : "Off", "span",
                             {{"0", "Off"}, {"1", "All columns"}, {"s2", "Split into 2"}, {"s3", "Split into 3"}, {"s4", "Split into 4"}}});
                if (s.split >= 2) {
                    std::vector<Choice> g_ch;
                    for (double v : {6.0, 9.0, 12.0, 18.0, 24.0}) g_ch.push_back({pts(v), pt(v)});
                    f.push_back({"Split gutter", pt(s.split_gutter), "split_gutter", g_ch});
                }
                // s036: lines on the page's baseline grid.
                f.push_back({"Baseline grid", s.align_grid ? "Align to grid" : "Off", "align_grid",
                             {{"0", "Off"}, {"1", "Align to grid"}}});
                // s038: hyphenation.
                f.push_back({"Hyphenate", s.hyphenate ? "On" : "Off", "hyphenate", {{"0", "Off"}, {"1", "On"}}});
                if (s.hyphenate) {   // s065: how
                    std::vector<Choice> langs;   // s190: in which language
                    for (const auto& l : hyphen_languages()) langs.push_back({l.code, l.name});
                    f.push_back({"Language", hyphen_language(s.lang).name, "lang", langs});
                    std::vector<Choice> lim{{"0", "No limit"}};
                    for (int k = 1; k <= 5; ++k) lim.push_back({std::to_string(k), std::to_string(k)});
                    f.push_back({"Hyphens in a row", s.hyph_limit > 0 ? std::to_string(s.hyph_limit) : "No limit", "hyph_limit", lim});
                    std::vector<Choice> sh;
                    for (int k = 5; k <= 10; ++k) sh.push_back({std::to_string(k), std::to_string(k) + " letters"});
                    f.push_back({"Shortest word", std::to_string(s.hyph_shortest) + " letters", "hyph_shortest", sh});
                    f.push_back({"Last word", s.hyph_last_word ? "May break" : "Whole", "hyph_last_word",
                                 {{"0", "Whole"}, {"1", "May break"}}});
                }
                // s039: keeps.
                f.push_back({"Keep with next", s.keep_next ? "On" : "Off", "keep_next", {{"0", "Off"}, {"1", "On"}}});
                static const std::vector<Choice> keeps{{"0", "Off"}, {"2", "2 at start and end"}, {"3", "3 at start and end"},
                                                       {"-1", "All together"}};
                std::string kl = "Off";
                for (const auto& k : keeps) if (k.id == std::to_string(s.keep_lines)) kl = k.label;
                f.push_back({"Keep lines", kl, "keep_lines", keeps});
                // s043: where it starts.
                static const std::vector<Choice> starts{{"0", "Anywhere"}, {"1", "Next column"}, {"2", "Next frame"},
                                                        {"3", "Next page"}, {"4", "Next right-hand page"}};
                f.push_back({"Start", starts[static_cast<std::size_t>(s.start)].label, "pstart", starts});
                // s045: bullets and numbering.
                static const std::vector<Choice> lt{{"0", "None"}, {"1", "Bullets"}, {"2", "Numbers"}};
                f.push_back({"List", lt[static_cast<std::size_t>(s.list.type)].label, "list_type", lt});
                if (s.list.type == ListType::Bullet) {
                    static const std::vector<Choice> bl{{"\u2022", "\u2022  Bullet"}, {"\u2013", "\u2013  Dash"}, {"\u25AA", "\u25AA  Square"},
                                                        {"\u25E6", "\u25E6  Circle"}, {"\u2713", "\u2713  Check"}, {"\u2605", "\u2605  Star"}};
                    std::string shown = s.list.bullet;
                    for (const auto& c : bl) if (c.id == s.list.bullet) shown = c.label;
                    f.push_back({"Bullet", shown, "list_bullet", bl});
                }
                if (s.list.type == ListType::Number) {
                    static const std::vector<Choice> nl{{"0", "1. 2. 3."}, {"1", "i. ii. iii."}, {"2", "I. II. III."},
                                                        {"3", "a. b. c."}, {"4", "A. B. C."}};
                    f.push_back({"Numbers", nl[static_cast<std::size_t>(s.list.numbering)].label, "list_num", nl});
                    // s047: past other paragraphs; a second level.
                    f.push_back({"Numbering", s.list.continues ? "Carries on" : "Restarts after others", "list_cont",
                                 {{"0", "Restarts after others"}, {"1", "Carries on"}}});
                    f.push_back({"Level", std::to_string(s.list.level), "list_level", {{"1", "1"}, {"2", "2"}}});
                }
                if (s.list.type != ListType::None) {
                    std::vector<Choice> hc;
                    for (double v : {9.0, 12.0, 14.0, 18.0, 24.0, 36.0}) hc.push_back({pts(v), pt(v)});
                    f.push_back({"Hang", pt(s.list.hang), "list_hang", hc});
                }
                // s044: rules above / below.
                for (const auto& [name, r, k] : {std::tuple{"above", &s.rule_above, "rule_a"}, std::tuple{"below", &s.rule_below, "rule_b"}}) {
                    const std::string key = k, label = std::string(name) == "above" ? "Rule above" : "Rule below";
                    std::vector<Choice> wc{{"0", "None"}};
                    for (double w : {0.25, 0.5, 1.0, 2.0, 4.0, 6.0, 12.0}) wc.push_back({pts(w), pt(w)});
                    f.push_back({label, r->weight > 0 ? pt(r->weight) : "None", key + "_w", wc});
                    if (r->weight <= 0) continue;
                    const std::string cap = std::string(name) == "above" ? "Above" : "Below";
                    std::vector<Choice> col{{"", "Black"}};
                    for (const auto& sw : d.ids_of<Swatch>()) col.push_back({sw, d.as<Swatch>(sw)->name});
                    const auto* cur = d.as<Swatch>(r->color);
                    f.push_back({cap + " colour", cur ? cur->name : "Black", key + "_c", col});
                    f.push_back({cap + " tint", tint_text(r->tint), key + "_t", tint_choices()});
                    std::vector<Choice> oc;
                    for (double v : {0.0, 2.0, 4.0, 6.0, 9.0, 12.0, 18.0, 24.0}) oc.push_back({pts(v), pt(v)});
                    f.push_back({cap + " offset", pt(r->offset), key + "_o", oc});
                    f.push_back({cap + " width", r->text_width ? "Text" : "Column", key + "_x", {{"0", "Column"}, {"1", "Text"}}});
                }
            }
            {   // s037 / s046: built on another style (of its kind); what Enter starts next (paragraphs)
                std::vector<Choice> base_ch{{"", "None"}}, next_ch{{"", "Same style"}};
                for (const auto& o : d.ids_of<Style>()) {
                    const auto* os = d.as<Style>(o);
                    if (doc::can_base_on(d, sid, o)) base_ch.push_back({o, os->name});
                    if (para && os->kind == StyleKind::Paragraph && o != sid) next_ch.push_back({o, os->name});
                }
                const auto* base = d.as<Style>(s.based_on);
                const auto* nx = d.as<Style>(s.next);
                f.push_back({"Based on", base ? base->name : "None", "based_on", base_ch});
                if (base) {
                    std::string mine;
                    for (const auto& k : s.own) mine += (mine.empty() ? "" : ", ") + doc::style_field_label(k);
                    f.push_back({"Own settings", mine.empty() ? "None (all from " + base->name + ")" : mine});
                    if (!s.own.empty()) {
                        Field back{"", "", "style_base", {{"reset", "Reset to " + base->name}}};
                        back.buttons = true;
                        f.push_back(back);
                    }
                }
                if (para) f.push_back({"Next style", nx ? nx->name : "Same style", "next_style", next_ch});
                const auto kids = doc::style_descendants(d, sid);
                if (!kids.empty()) {
                    std::string names;
                    for (const auto& k : kids) names += (names.empty() ? "" : ", ") + d.as<Style>(k)->name;
                    f.push_back({"Styles on it", names});
                }
            }
            f.push_back({"Used", uses ? std::to_string(uses) + "×" : "Not used"});
            Field act{"", "", "style", {{"duplicate", "Duplicate"}}};
            if (!uses) act.choices.push_back({"delete", "Delete"});
            act.buttons = true;
            f.push_back(act);
            mark_typed(f, unit_pt(d.settings().units));   // s041
            for (auto& fl : f) fl.group = style_group(fl);   // s161
            if (d.as<Style>(s.based_on))   // s092: mark the rows that are its own
                for (auto& fl : f)
                    if (const auto k = doc::style_key_of_edit(fl.edit); !k.empty() && std::find(s.own.begin(), s.own.end(), k) != s.own.end())
                        fl.own = k;
            o.add({sid, G::Styles, s.name, uses ? "" : "Unused", th, f});
        }
    }

    // ── Links & assets ──────────────────────────────────────────────────────
    {
        std::size_t i = 0;
        for (const auto& aid : d.ids_of<Asset>()) {
            const auto& a = *d.as<Asset>(aid);
            // Where it's placed, and how sharp it lands there: the SOFTEST
            // placement's effective ppi (s009: per frame, by its fit).
            std::vector<Doc::Id> on;
            double ppi_low = 0;
            for (const auto& fid : d.ids_of<Frame>()) {
                const auto& f = *d.as<Frame>(fid);
                if (f.asset != aid) continue;
                if (d.as<Page>(f.owner) && std::find(on.begin(), on.end(), f.owner) == on.end())
                    on.push_back(f.owner);
                const double p = effective_ppi(f, a);
                if (p > 0 && (ppi_low == 0 || p < ppi_low)) ppi_low = p;
            }
            std::sort(on.begin(), on.end(),
                      [&](const Doc::Id& x, const Doc::Id& y) { return d.page_index(x) < d.page_index(y); });
            std::string ppi = "—";
            if (a.px_w == 0) ppi = "Vector";
            else if (ppi_low > 0) ppi = std::to_string(static_cast<int>(std::lround(ppi_low)));
            const bool low = ppi_low > 0 && ppi_low < kLowPpi;

            const LinkStatus st = link_status(a, dir);
            const bool bad = st == LinkStatus::Missing || st == LinkStatus::EmbeddedMissing;
            std::string badge = bad ? "Missing" : st == LinkStatus::Modified ? "Modified" : low ? "Low res" : "";

            // What can be done to it (buttons in the inspector).
            std::vector<Choice> acts{{"relink", "Relink…"}};
            if (st == LinkStatus::Modified) acts.push_back({"update", "Update"});
            if (!a.embedded && !bad) acts.push_back({"embed", "Embed"});
            if (a.embedded && !a.source.empty()) acts.push_back({"unembed", "Unembed"});

            const double aw = a.px_w ? a.px_w : 1, ah = a.px_h ? a.px_h : 1;
            Thumb th{K::Image, aw, ah, tint(i++), bad};
            if (!bad) { th.file = resolve_asset(a, dir); th.stamp = file_stamp(th.file); }
            std::vector<Field> fields{
                {"Link", a.embedded ? "Embedded" : "Linked"},
                {"Status", link_status_text(st)},
                {"File", a.path},
                {"Color", a.color},
                {"Pixels", a.px_w ? std::to_string(a.px_w) + " × " + std::to_string(a.px_h) : "Vector"},
                {"Effective ppi", ppi + (low ? "  (low)" : "")},
                {"Placed", folios(d, on)}};
            if (a.embedded && !a.source.empty()) fields.insert(fields.begin() + 3, Field{"From", a.source});
            Field act{"", "", "asset", acts};
            act.buttons = true;
            fields.push_back(act);
            o.add({aid, G::Assets, a.name, badge, th, fields});
        }
    }

    // ── Colors ──────────────────────────────────────────────────────────────
    for (const auto& sid : d.ids_of<Swatch>()) {
        const auto& s = *d.as<Swatch>(sid);
        std::string badge = s.model == ColorModel::Spot ? "Spot" : (s.model == ColorModel::RGB ? "RGB" : "");
        std::string type = s.model == ColorModel::Spot ? "Spot"
                         : s.model == ColorModel::RGB  ? "Process (converts at output)" : "Process";
        // s052: how much it's used, and a heading by kind -- spot inks
        // apart, RGB apart (it converts at output: the CMYK report's list).
        const int uses = doc::swatch_uses(d, sid);
        std::string used = uses == 0 ? "Unused" : uses == 1 ? "1 place" : std::to_string(uses) + " places";
        // Black is what text with no colour prints in, Paper the page itself.
        if (s.name == "Black") used = uses == 0 ? "Text (default)" : "Text, " + used;
        if (s.name == "Paper" && uses == 0) used = "The page";
        const std::string group = s.model == ColorModel::Spot ? "Spot inks"
                                : s.model == ColorModel::RGB  ? "RGB \u2014 converts at output"
                                : s.model == ColorModel::Gray ? "Gray" : "Process (CMYK)";
        // s058: edit it, copy it, delete it (when nothing uses it).
        Field act{"", "", "swatch", {{"edit", "Edit\u2026"}, {"duplicate", "Duplicate"}, {"mine", "Add to My Library"}}};   // s172: mine
        if (uses == 0 && s.name != "Black" && s.name != "Paper") act.choices.push_back({"delete", "Delete"});
        act.buttons = true;
        std::vector<Field> fields{{"Model", model_text(s.model)}, {"Value", swatch_value(s)}, {"Used", used}, {"Type", type}};
        if (!s.from.empty()) fields.push_back({"From", s.from});   // s171: the library it was taken from
        fields.push_back(act);
        o.add({sid, G::Colors, s.name, badge, {K::Swatch, 1, 1, doc::screen_rgb(s)}, fields, group});
    }

    // ── Footnotes (s115): each one, story by story, in order ────────────────
    for (const auto& sid : d.ids_of<Story>()) {
        const auto& st = *d.as<Story>(sid);
        const std::string sname = st.name.empty() ? sid : st.name;
        int n = 0;
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            for (int k = 0; k < static_cast<int>(para.notes.size()); ++k) {
                ++n;
                const auto& words = para.notes[static_cast<std::size_t>(k)].text;
                // Its paragraph's opening words, marks left out (context: where it is).
                std::string in;
                for (std::size_t i = 0; i < para.text.size() && in.size() < 48;) {
                    const auto c = static_cast<unsigned char>(para.text[i]);
                    if (c == 0xEE) { i += 3; continue; }   // a marker / a mark (U+E0xx)
                    in += para.text[i++];
                }
                if (in.size() < para.text.size()) {
                    while (!in.empty() && (static_cast<unsigned char>(in.back()) & 0xC0) == 0x80) in.pop_back();   // whole characters
                    if (!in.empty()) in.pop_back();
                    in += "\u2026";
                }
                // s116: one paragraph -- typed here; several -- shown, edited in the text.
                const bool paras = words.find('\n') != std::string::npos;
                Field w{paras ? "Words (\u00b6 " + std::to_string(std::count(words.begin(), words.end(), '\n') + 1) + ")" : "Words",
                        words, paras ? "" : "note_words"};
                w.text = !paras;
                Field go{"", "", "note", {{"show", "Show in the Text"}}};
                go.buttons = true;
                Thumb th{K::Text, 6, 9};
                th.sample = words.empty() ? std::string("(no words yet)") : words;
                o.add({note_item_id(sid, p, k), G::Notes, "Note " + std::to_string(n) + " \u00b7 " + sname,
                       words.empty() ? "Empty" : "", th,
                       {w, {"Story", sname}, {"Number", std::to_string(n) + " in the story"}, {"In", in.empty() ? "\u2014" : in}, go}});
            }
        }
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
        if (l.kind == ListKind::Index) {   // s135: an index counts its marks
            now = 0;
            for (const auto& sid : d.ids_of<Story>())
                for (const auto& p : d.as<Story>(sid)->paras) now += static_cast<int>(p.index.size());
        }
        const bool never = l.built_entries < 0;
        bool stale = !never && l.kind == ListKind::Contents && l.built_entries != now;   // s140: a bibliography's lines aren't its paragraphs either   // s138: an index counts entries, not marks -- its staleness is stale_lists' alone
        std::string built = never ? "Never" : stale ? "Stale — built with " + std::to_string(l.built_entries)
                                                    : "Current";
        if (stale_lists && !never) {   // s135: an index too; s140: and a bibliography   // s089: what Build would change
            const auto it = stale_lists->find(lid);
            stale = it != stale_lists->end();
            built = stale ? "Out of date — " + it->second : "Current";
        }
        // s088: the styles it gathers, by level -- add one (the next level), take one out.
        std::string levels;
        for (std::size_t k = 0; k < l.from_styles.size(); ++k)
            levels += (levels.empty() ? "" : "  ·  ") + (l.from_styles.size() > 1 ? std::to_string(k + 1) + " " : std::string()) +
                      style_name(d, l.from_styles[k]);
        std::vector<Choice> add_ch;
        for (const auto& s : d.ids_of<Style>())
            if (d.as<Style>(s)->kind == StyleKind::Paragraph &&
                std::find(l.from_styles.begin(), l.from_styles.end(), s) == l.from_styles.end())
                add_ch.push_back({s, d.as<Style>(s)->name});
        Field add{l.kind == ListKind::Bibliography ? "Add style" : "Add level", "Choose a style…", "list_add", add_ch};   // s140: a bibliography gathers styles, not levels
        Field drop{"", "", "list_remove", {}};
        drop.buttons = true;
        for (const auto& s : l.from_styles) drop.choices.push_back({s, "− " + style_name(d, s)});
        // s090: each level's entry style (Automatic = found or made by Build), and page numbers.
        std::vector<Field> entry_rows;
        if (l.kind == ListKind::Contents) {
            for (std::size_t k = 0; k < l.from_styles.size(); ++k) {
                const std::string lv = std::to_string(k + 1);
                std::vector<Choice> ch{{lv + ":", "Automatic"}};
                for (const auto& s : d.ids_of<Style>())
                    if (d.as<Style>(s)->kind == StyleKind::Paragraph) ch.push_back({lv + ":" + s, d.as<Style>(s)->name});
                const std::string cur = k < l.entry_styles.size() ? l.entry_styles[k] : std::string{};
                const auto* cs = d.as<Style>(cur);
                entry_rows.push_back({l.from_styles.size() > 1 ? "Level " + lv + " in" : "Entries in", cs ? cs->name : "Automatic",
                                      "list_entry", ch});
            }
            entry_rows.push_back({"Page numbers", l.numbers ? "Yes" : "No", "list_numbers", {{"1", "Yes"}, {"0", "No"}}});
        }
        if (l.kind == ListKind::Bibliography)   // s142: its order
            entry_rows.push_back({"Order", l.cite_order ? "First cited" : "Alphabetical", "list_cite_order", {{"0", "Alphabetical"}, {"1", "First cited"}}});
        if (l.kind == ListKind::Index) {   // s138: an index's two levels -- entries and the sub-entries under them
            for (int k = 0; k < 2; ++k) {
                const std::string lv = std::to_string(k + 1);
                std::vector<Choice> ch{{lv + ":", "Automatic"}};
                for (const auto& s2 : d.ids_of<Style>())
                    if (d.as<Style>(s2)->kind == StyleKind::Paragraph) ch.push_back({lv + ":" + s2, d.as<Style>(s2)->name});
                const std::string cur = static_cast<std::size_t>(k) < l.entry_styles.size() ? l.entry_styles[static_cast<std::size_t>(k)] : std::string{};
                const auto* cs = d.as<Style>(cur);
                entry_rows.push_back({k == 0 ? "Entries in" : "Sub-entries in", cs ? cs->name : "Automatic", "list_entry", ch});
            }
            entry_rows.push_back({"Page numbers", l.numbers ? "Yes" : "No", "list_numbers", {{"1", "Yes"}, {"0", "No"}}});
        }
        Item it{lid, G::Generated, l.name, stale || never ? "Stale" : "", {K::Generated, 6, 9, 0, stale || never},
               {{"From", from.empty() ? "—" : levels},
                {"Entries", std::to_string(now)},
                {"Built", built},
                {"Placed", l.placed_on.empty() ? "—" : "Page " + d.folio(l.placed_on)},
                add,
                drop,
                [&] {   // s087: Build -- writes the entries, with their folios, into the list's story
                    Field act{"", "", "genlist", {}};
                    if (!l.story.empty() || !l.placed_on.empty())   // s140: every kind builds
                        act.choices.push_back({"build", never ? "Build" : "Rebuild"});
                    act.buttons = true;
                    return act;
                }()}};
        // s090: the entry rows before the Build action
        it.fields.insert(it.fields.end() - 1, entry_rows.begin(), entry_rows.end());
        if (l.kind == ListKind::Index) {   // s135: made from index marks, not styles -- no levels to pick
            std::erase_if(it.fields, [](const Field& f) { return f.edit == "list_add" || f.edit == "list_remove"; });
            for (auto& f : it.fields)
                if (f.label == "From") f.value = "Index marks";
                else if (f.label == "Entries") f.label = "Marks";
        }
        o.add(std::move(it));
    }

    return o;
}

// s021: tints / opacities, 100% down (opacity also 0%).
std::vector<Choice> tint_choices(int least) {
    std::vector<Choice> c;
    for (int t : tint_steps()) c.push_back({std::to_string(t), std::to_string(t) + "%"});
    if (least == 0) c.push_back({"0", "0%"});
    return c;
}

std::vector<Item> frame_items(const Doc& d, const std::vector<std::string>& ids, const std::string& dir) {
    std::vector<Item> out;
    for (const auto& id : ids) {
        const auto* f = d.as<Frame>(id);
        if (!f) continue;
        const char* kind = f->kind == FrameKind::Text ? "Text" : f->kind == FrameKind::Image ? "Image" : "Shape";
        std::string on = d.as<Page>(f->owner) ? "Page " + d.folio(f->owner) : master_label(d, f->owner);
        std::vector<Field> fields{{"Kind", kind}, {"On", on}};
        // s210: what it holds -- a shape made a text or picture frame, and back
        fields.push_back({"Content", f->on_path && f->kind == FrameKind::Text ? "Text on path"   // s217
                                     : f->kind == FrameKind::Text ? "Text" : f->kind == FrameKind::Image ? "Picture" : "None", "content",
                          {{"0", "Text"}, {"3", "Text on path"}, {"1", "Picture"}, {"2", "None"}}});
        if (f->on_path && f->kind == FrameKind::Text) {   // s217: where along the path it starts
            std::vector<Choice> sc;
            for (double v : {0.0, 6.0, 12.0, 18.0, 24.0, 36.0, 48.0, 72.0, 96.0, 144.0}) sc.push_back({pts(v), pts(v) + " pt"});
            fields.push_back({"Path start", pts(f->path_from) + " pt", "path_from", sc});
        }
        {   // s197: its outline -- a rectangle and an oval trade places (s210: any frame's)
            if (f->shape == ShapeKind::Path) {
                const bool line = f->open && f->path.size() == 2;
                fields.push_back({"Shape", line ? std::string("Line")
                                       : !f->subs.empty() ? "Compound path, " + std::to_string(f->subs.size() + 1) + " parts"   // s212
                                                : (f->open ? "Open path, " : "Closed path, ") + std::to_string(f->path.size()) + " points"});
            } else {
                fields.push_back({"Shape", f->shape == ShapeKind::Oval ? "Oval" : "Rectangle", "shape", {{"0", "Rectangle"}, {"1", "Oval"}}});
            }
        }
        // One compact row (Scott): X Y W H over their values, inches.
        const auto U = d.settings().units;   // s073
        // s154: X / Y from the rulers' zero (the origin and the dragged zero point).
        const Zero z = ruler_zero(d, f->owner, f->side);
        Field box{"Bounds (" + unit_name(U) + ")", length_in(f->x - z.x, U) + "\t" + length_in(f->y - z.y, U) + "\t" + length_in(f->w, U) + "\t" +
                                                       length_in(f->h, U)};
        box.cols = {"X", "Y", "W", "H"};
        box.edit = "bounds";   // s041: each typed
        fields.push_back(box);
        std::string title = std::string(kind) + " frame";
        if (f->kind == FrameKind::Shape && f->shape != ShapeKind::Rect)   // s197
            title = f->shape == ShapeKind::Oval ? "Oval" : f->open && f->path.size() == 2 ? "Line" : "Path";
        // s016: fill and stroke, from the swatches.
        {
            std::vector<Choice> sw{{"", "None"}};
            for (const auto& id : d.ids_of<Swatch>()) sw.push_back({id, d.as<Swatch>(id)->name});
            const auto name = [&](const std::string& id) { const auto* x = d.as<Swatch>(id); return x ? x->name : std::string("None"); };
            fields.push_back({"Fill", name(f->fill), "fill", sw});
            if (!f->fill.empty()) fields.push_back({"Fill tint", tint_text(f->fill_tint), "fill_tint", tint_choices()});   // s021
            fields.push_back({"Stroke", name(f->stroke), "stroke", sw});
            if (!f->stroke.empty()) fields.push_back({"Stroke tint", tint_text(f->stroke_tint), "stroke_tint", tint_choices()});
            if (!f->stroke.empty()) {
                std::vector<Choice> wc;
                for (double w : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0}) wc.push_back({pts(w), pts(w) + " pt"});
                fields.push_back({"Weight", pts(f->stroke_w) + " pt", "stroke_w", wc});
                // s204: the stroke's style
                static const char* dashes[] = {"Solid", "Dashed", "Dotted"};
                fields.push_back({"Dash", dashes[std::clamp(f->dash, 0, 2)], "dash", {{"0", "Solid"}, {"1", "Dashed"}, {"2", "Dotted"}}});
                static const char* caps[] = {"Butt", "Round", "Square"};
                if (f->dash != 2) fields.push_back({"Line ends", caps[std::clamp(f->cap, 0, 2)], "cap", {{"0", "Butt"}, {"1", "Round"}, {"2", "Square"}}});
                static const char* joins[] = {"Mitre", "Round", "Bevel"};
                const bool open = f->kind == FrameKind::Shape && f->shape == ShapeKind::Path && f->open;
                if (!(open && f->path.size() == 2))
                    fields.push_back({"Corners", joins[std::clamp(f->join, 0, 2)], "join", {{"0", "Mitre"}, {"1", "Round"}, {"2", "Bevel"}}});
                if (open) {   // an open path's two ends
                    static const char* ends[] = {"None", "Arrow", "Circle", "Bar"};
                    const std::vector<Choice> ec{{"0", "None"}, {"1", "Arrow"}, {"2", "Circle"}, {"3", "Bar"}};
                    fields.push_back({"Start", ends[std::clamp(f->end_a, 0, 3)], "end_a", ec});
                    fields.push_back({"End", ends[std::clamp(f->end_b, 0, 3)], "end_b", ec});
                }
            }
        }
        if (!(f->kind == FrameKind::Shape && f->shape != ShapeKind::Rect)) {   // s205: rounded corners (a box's)
            std::vector<Choice> rc;
            for (double v : {0.0, 3.0, 6.0, 9.0, 12.0, 18.0, 24.0, 36.0}) rc.push_back({pts(v), v > 0 ? pts(v) + " pt" : std::string("Square")});
            fields.push_back({"Corner radius", f->radius > 0 ? pts(f->radius) + " pt" : std::string("Square"), "radius", rc});
        }
        fields.push_back({"Opacity", tint_text(f->opacity), "opacity", tint_choices(0)});   // s021
        // s025: a master's item shows on both sides, or one; a page's copy
        // of one says so and can go back.
        if (const auto* m = d.as<Master>(f->owner)) {
            static const char* sides[] = {"Every page", "Right pages", "Left pages"};
            std::vector<Choice> ch{{"0", "Every page"}, {"1", "Right pages"}, {"2", "Left pages"}};
            if (m->facing && m->folds.empty()) ch.push_back({"3", "Across the spine"});   // s155
            fields.push_back({"Shows on", f->across ? "Across the spine" : sides[static_cast<int>(f->side)], "side", ch});
        }
        {   // s018: text wrap -- how this frame pushes OTHER frames' text aside.
            static const char* names[] = {"None", "Around", "Jump", "Shape"};
            std::vector<Choice> wc{{"0", "None"}, {"1", "Around"}, {"2", "Jump"}};
            if (f->kind == FrameKind::Image || doc::has_outline(*f)) wc.push_back({"3", "Shape"});   // s173: the picture's own outline; s200: a shape's
            fields.push_back({"Wrap", names[static_cast<int>(f->wrap)], "wrap", wc});
            if (f->wrap == Wrap::Shape && f->kind == FrameKind::Image) {
                const auto* a = d.as<Asset>(f->asset);
                if (!a || a->shape.empty())
                    fields.push_back({"Shape", "Nothing to trace \u2014 wraps the frame"});
            }
            if (f->wrap != Wrap::None) {
                std::vector<Choice> oc;
                for (double v : {0.0, 3.0, 6.0, 9.0, 12.0, 18.0, 24.0, 36.0}) oc.push_back({pts(v), pts(v) + " pt"});
                fields.push_back(sides_row("Wrap offset (pt)", f->wrap_off, "wrap_off", oc));   // s067: per side
            }
        }
        if (f->kind == FrameKind::Text) {
            const auto* st = d.as<Story>(f->story);
            fields.push_back({"Story", st ? st->name : "\u2014"});
            const auto [at, n] = thread_place(d, id);
            fields.push_back({"Thread", n > 1 ? std::to_string(at) + " of " + std::to_string(n) : "Not threaded"});
            // s012: columns.
            const int cols = column_count(*f);
            std::vector<Choice> cc;
            for (int k = 1; k <= 6; ++k) cc.push_back({std::to_string(k), std::to_string(k)});
            fields.push_back({"Columns", std::to_string(cols), "columns", cc});
            if (cols > 1) {
                std::vector<Choice> gc;
                for (double g : {6.0, 9.0, 12.0, 14.0, 18.0, 24.0, 36.0}) gc.push_back({pts(g), pts(g) + " pt"});
                fields.push_back({"Gutter", pts(f->gutter) + " pt", "gutter", gc});
                // s067: Widths -- each column's width typed (points), one
                // column each; the ratio presets ("=2:1") in a menu beside.
                std::vector<Choice> wc{{"=", "Equal"}};
                if (cols == 2) for (const char* p : {"2:1", "1:2", "3:2", "2:3", "3:1", "1:3"}) wc.push_back({std::string("=") + p, std::string(p).replace(1, 1, " : ")});
                if (cols == 3) for (const char* p : {"2:1:1", "1:2:1", "1:1:2"}) {
                    std::string l = p;
                    for (std::size_t i = 0; (i = l.find(':', i)) != std::string::npos; i += 3) l.replace(i, 1, " : ");
                    wc.push_back({std::string("=") + p, l});
                }
                Field widths{"Widths (pt)", "", "colw", wc};
                for (int c = 0; c < cols; ++c) {
                    widths.value += (c ? "\t" : "") + pts(std::round(column_width(*f, c) * 10) / 10);
                    widths.cols.push_back(std::to_string(c + 1));
                }
                fields.push_back(widths);
                fields.push_back({"Balance", f->balance ? "On" : "Off", "balance", {{"0", "Off"}, {"1", "On"}}});
                // s055: a rule between the columns -- its weight, then (when on) its ink and inset.
                {
                    const auto& r = f->col_rule;
                    std::vector<Choice> wc{{"0", "None"}};
                    for (double v : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0}) wc.push_back({pts(v), pts(v) + " pt"});
                    fields.push_back({"Column rule", r.weight > 0 ? pts(r.weight) + " pt" : "None", "col_rule", wc});
                    if (r.weight > 0) {
                        std::vector<Choice> sw{{"", "Black"}};
                        for (const auto& sid : d.ids_of<Swatch>()) sw.push_back({sid, d.as<Swatch>(sid)->name});
                        const auto* cur = d.as<Swatch>(r.color);
                        fields.push_back({"Rule colour", cur ? cur->name : "Black", "col_rule_color", sw});
                        fields.push_back({"Rule tint", tint_text(r.tint), "col_rule_tint", tint_choices()});
                        std::vector<Choice> ic;
                        for (double v : {0.0, 3.0, 6.0, 9.0, 12.0, 18.0, 24.0}) ic.push_back({pts(v), pts(v) + " pt"});
                        fields.push_back({"Rule inset", pts(r.inset) + " pt", "col_rule_inset", ic});
                    }
                }
            }
            // s069: other frames' wrap -- pushes this text, or not.
            fields.push_back({"Others' wrap", f->ignore_wrap ? "Ignored" : "Pushes the text", "ignore_wrap",
                              {{"0", "Pushes the text"}, {"1", "Ignored"}}});
            {   // s048: the height follows the text (the last frame of a thread, one column).
                const bool can = f->next.empty() && column_count(*f) == 1;
                fields.push_back({"Auto-size", f->autosize ? (can ? "Height" : "Height (not here)") : "Off", "autosize",
                                  {{"0", "Off"}, {"1", "Height"}}});
            }
            {   // s042: where the text sits, up and down.
                static const std::vector<Choice> va{{"0", "Top"}, {"1", "Centre"}, {"2", "Bottom"}, {"3", "Justify"}};
                fields.push_back({"Vertical", va[static_cast<std::size_t>(f->valign)].label, "valign", va});
            }
            {   // s040: the text held in from the frame's edges.
                std::vector<Choice> ic;
                for (double v : {0.0, 3.0, 4.0, 6.0, 9.0, 12.0, 18.0, 24.0}) ic.push_back({pts(v), pts(v) + " pt"});
                fields.push_back(sides_row("Inset (pt)", f->inset, "inset", ic));   // s067: per side
            }
            if (st) title += " \u2014 " + st->name;
        } else if (f->kind == FrameKind::Image) {
            const auto* a = d.as<Asset>(f->asset);
            fields.push_back({"Image", a ? a->name : "Empty"});
            if (a) {
                title += " \u2014 " + a->name;
                static const std::vector<Choice> fits{{"0", "Fill frame"}, {"1", "Fit in frame"}};
                fields.push_back({"Fit", f->fit == ImageFit::Custom ? "By hand" : fits[static_cast<int>(f->fit)].label, "fit", fits});
                // s035: its scale (of its own size at 300 ppi); Alt+drag moves it in the frame.
                std::vector<Choice> sc;
                for (int v : {10, 25, 50, 75, 100, 125, 150, 200, 300, 400}) sc.push_back({std::to_string(v), std::to_string(v) + "%"});
                fields.push_back({"Scale", std::to_string(static_cast<int>(std::lround(picture_scale(*f, *a)))) + "%", "pic_scale", sc});
                {   // s076: where it sits in the frame, typed (the document's unit).
                    const Rect r = picture_rect({0, 0, f->w, f->h}, *f, picture_aspect(*a).w, picture_aspect(*a).h);
                    Field off{"Offset (" + unit_name(U) + ")", length_in(r.x, U) + "\t" + length_in(r.y, U)};
                    off.cols = {"X", "Y"};
                    off.edit = "pic_off";
                    fields.push_back(off);
                }
                const double p = effective_ppi(*f, *a);
                fields.push_back({"Effective ppi", a->px_w == 0 ? "Vector"
                                  : std::to_string(static_cast<int>(std::lround(p))) + (p < kLowPpi ? "  (low)" : "")});
                fields.push_back({"Status", link_status_text(link_status(*a, dir))});
            }
            Field act{"", "", "frame_image", {{"place", a ? "Replace\u2026" : "Place\u2026"}}};
            if (a && link_status(*a, dir) == LinkStatus::Modified) act.choices.push_back({"update", "Update"});
            act.buttons = true;
            fields.push_back(act);
        }
        // s025: last -- what it overrides, and the way back.
        if (!f->overrides.empty()) {
            const auto* mf = d.as<Frame>(f->overrides);
            fields.push_back({"Overrides", mf ? master_label(d, mf->owner) + " item" : "a removed master item"});
            Field back{"", "", "override", {{"remove", "Remove override"}}};
            back.buttons = true;
            fields.push_back(back);
        }
        mark_typed(fields, unit_pt(d.settings().units));   // s041
        out.push_back({id, Segment::Pages, title, "", {}, fields});
    }
    return out;
}

std::vector<Field> text_fields(const Doc& d, const std::string& story, TextPos a, TextPos b) {
    std::vector<Field> out;
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return out;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    std::vector<Choice> pc, cc{{"", "None"}};
    for (const auto& id : d.ids_of<Style>()) {
        const auto* s = d.as<Style>(id);
        (s->kind == StyleKind::Paragraph ? pc : cc).push_back({id, s->name});
    }
    // The paragraph style: one, or Mixed.
    std::string ps = st->paras[static_cast<std::size_t>(a.para)].style;
    for (int p = a.para; p <= b.para; ++p)
        if (st->paras[static_cast<std::size_t>(p)].style != ps) { ps = "\x01"; break; }
    // The character style over the selection (at the caret: what typing gets).
    std::string cs;
    if (a == b) {
        cs = char_style_at(st->paras[static_cast<std::size_t>(a.para)], a.byte);
    } else {
        bool first = true;
        for (TextPos q = a; q < b; q = next_char(*st, q)) {
            const auto& para = st->paras[static_cast<std::size_t>(q.para)];
            if (q.byte >= static_cast<int>(para.text.size())) continue;   // a paragraph break
            const std::string here = char_style_at(para, q.byte + 1);
            if (first) { cs = here; first = false; }
            else if (here != cs) { cs = "\x01"; break; }
        }
    }
    const auto name = [&](const std::string& id, const char* none) {
        if (id == "\x01") return std::string(kMixed);
        const auto* s = d.as<Style>(id);
        return s ? s->name : std::string(none);
    };
    out.push_back({"Paragraph style", name(ps, "None"), "pstyle", pc});
    // s047: a numbered paragraph's number -- counted, or set by hand.
    {
        const auto& para = st->paras[static_cast<std::size_t>(a.para)];
        const auto* pst = d.as<Style>(para.style);
        if (pst && pst->list.type == ListType::Number) {
            std::vector<Choice> nc{{"0", "Counted"}};
            for (int k = 1; k <= 20; ++k) nc.push_back({std::to_string(k), "Restart at " + std::to_string(k)});
            out.push_back({"Number", para.restart > 0 ? "Restart at " + std::to_string(para.restart)
                                                      : "Counted (" + doc::list_marker(d, *st, a.para) + ")",
                           "list_from", nc});
        }
    }
    // s093: the paragraph's own settings over its style -- the effective
    // value (Mixed across a selection that differs), marked when it's the
    // paragraph's own (the first paragraph's), with a reset.
    {
        const auto ptx = [](double v) { return pts(v) + " pt"; };
        std::vector<Choice> al;
        for (int i = 0; i < 4; ++i) al.push_back({std::to_string(i), align_text(static_cast<Align>(i))});
        std::vector<Choice> len;
        for (double v : {0.0, 6.0, 12.0, 18.0, 24.0, 36.0}) len.push_back({pts(v), ptx(v)});
        struct Row { const char* label; const char* edit; const char* key; };
        static const Row rows[] = {{"Align", "palign", "align"}, {"Left indent", "plindent", "left_indent"},
                                   {"Right indent", "prindent", "right_indent"}, {"First indent", "pfindent", "first_indent"},
                                   {"Space before", "pbefore", "space_before"}, {"Space after", "pafter", "space_after"}};
        const auto value = [&](const Para& p, const std::string& key) -> std::string {
            const Style e = doc::para_style(d, p);
            if (key == "align") return align_text(e.align);
            return ptx(key == "left_indent" ? e.left_indent : key == "right_indent" ? e.right_indent
                     : key == "first_indent" ? e.first_indent : key == "space_before" ? e.space_before : e.space_after);
        };
        const auto has = [](const ParaLocal& o, const std::string& key) {
            return key == "align" ? o.align.has_value() : key == "left_indent" ? o.left_indent.has_value()
                 : key == "right_indent" ? o.right_indent.has_value() : key == "first_indent" ? o.first_indent.has_value()
                 : key == "space_before" ? o.space_before.has_value() : o.space_after.has_value();
        };
        const auto& p0 = st->paras[static_cast<std::size_t>(a.para)];
        // s094: keeps and hyphenation too (choices, not lengths).
        static const std::vector<Choice> onoff{{"0", "Off"}, {"1", "On"}};
        static const std::vector<Choice> keeps{{"0", "Off"}, {"2", "2 at start and end"}, {"3", "3 at start and end"}, {"-1", "All together"}};
        const auto choice_text = [](const std::vector<Choice>& ch, const std::string& id) {
            for (const auto& c : ch) if (c.id == id) return c.label;
            return id;
        };
        struct Row2 { const char* label; const char* edit; const char* key; const std::vector<Choice>* ch; };
        const Row2 rows2[] = {{"Keep with next", "pkeep_next", "keep_next", &onoff}, {"Keep lines", "pkeep_lines", "keep_lines", &keeps},
                              {"Hyphenate", "phyphenate", "hyphenate", &onoff}};
        const auto value2 = [&](const Para& p, const std::string& key, const std::vector<Choice>& ch) {
            const Style e = doc::para_style(d, p);
            return choice_text(ch, key == "keep_next" ? std::to_string(e.keep_next ? 1 : 0)
                                 : key == "hyphenate" ? std::to_string(e.hyphenate ? 1 : 0) : std::to_string(e.keep_lines));
        };
        const auto has2 = [](const ParaLocal& o, const std::string& key) {
            return key == "keep_next" ? o.keep_next.has_value() : key == "hyphenate" ? o.hyphenate.has_value() : o.keep_lines.has_value();
        };
        std::vector<Field> extra;
        for (const auto& r : rows2) {
            std::string v = value2(p0, r.key, *r.ch);
            for (int p = a.para + 1; p <= b.para; ++p)
                if (value2(st->paras[static_cast<std::size_t>(p)], r.key, *r.ch) != v) { v = kMixed; break; }
            Field f{r.label, v, r.edit, *r.ch};
            if (has2(p0.over, r.key)) f.own = std::string("para.") + r.key;
            extra.push_back(f);
        }
        for (const auto& r : rows) {
            std::string v = value(p0, r.key);
            for (int p = a.para + 1; p <= b.para; ++p)
                if (value(st->paras[static_cast<std::size_t>(p)], r.key) != v) { v = kMixed; break; }
            Field f{r.label, v, r.edit, std::string(r.key) == "align" ? al : len};
            if (has(p0.over, r.key)) f.own = std::string("para.") + r.key;
            out.push_back(f);
        }
        out.insert(out.end(), extra.begin(), extra.end());
    }
    out.push_back({"Character style", name(cs, "None"), "cstyle", cc});
    const std::string sel = text_between(*st, a, b);
    std::size_t chars = 0;
    for (unsigned char c : sel) chars += (c & 0xC0) != 0x80;
    out.push_back({"Selection", a == b ? "Caret" : std::to_string(chars) + (chars == 1 ? " character" : " characters")});
    // s030: formatting on the words themselves (over the styles).
    {
        const auto& para = st->paras[static_cast<std::size_t>(a.para)];
        const Local l = local_at(para, a == b ? a.byte : a.byte + 1);
        const auto pt = [](double v) { return pts(v) + " pt"; };
        std::vector<Choice> sz{{"0", "Style's"}};
        for (double v : {8.0, 9.0, 10.0, 11.0, 12.0, 14.0, 18.0, 24.0, 36.0}) sz.push_back({pts(v), pt(v)});
        std::vector<Choice> co{{"", "Style's"}};
        for (const auto& id : d.ids_of<Swatch>()) co.push_back({id, d.as<Swatch>(id)->name});
        const auto* sw = d.as<Swatch>(l.color);
        out.push_back({"Size", l.size > 0 ? pt(l.size) : "Style's", "lsize", sz});
        out.push_back({"Colour", sw ? sw->name : "Style's", "lcolor", co});
        // s159: No break on the words themselves (a character style's before).
        out.push_back({"No break", l.no_break < 0 ? "Style's" : l.no_break ? "On" : "Off", "lnobreak",
                       {{"-1", "Style's"}, {"1", "On"}, {"0", "Off"}}});
        mark_typed(out, unit_pt(d.settings().units));   // s041: Size typed
        Field fx{"", "", "local", {{"bold", "Bold"}, {"italic", "Italic"}, {"clear", "Clear overrides"}}};
        fx.buttons = true;
        out.push_back(fx);
    }
    return out;
}

std::string frames_title(const std::vector<Item>& items) {
    if (items.empty()) return "Nothing selected";
    if (items.size() == 1) return items.front().title;
    return std::to_string(items.size()) + " frames";
}

}  // namespace composix::core
