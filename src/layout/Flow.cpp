#include "layout/Flow.hpp"
#include "core/Hyphen.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace composix::layout {

namespace {

// One measuring context a thread: 72 dpi (1 unit = 1 pt), no hinting, so
// the same text breaks the same way at every zoom. s102: per thread (was
// one for the process) -- Pango contexts aren't thread safe, and its default
// font map is already per thread; layout off the UI thread gets its own.
PangoContext* measure_context() {
    struct Held {   // let go when the thread ends (a worker's font map with it)
        PangoContext* c;
        ~Held() { if (c) g_object_unref(c); }
    };
    thread_local Held ctx{[] {
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        PangoContext* c = pango_font_map_create_context(fm);
        pango_cairo_context_set_resolution(c, 72.0);
        cairo_font_options_t* fo = cairo_font_options_create();
        cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        pango_cairo_context_set_font_options(c, fo);
        cairo_font_options_destroy(fo);
        return c;
    }()};
    return ctx.c;
}

struct LayoutDel { void operator()(PangoLayout* l) const { g_object_unref(l); } };
using LayoutPtr = std::unique_ptr<PangoLayout, LayoutDel>;

// s109: the story being flowed or drawn on this thread, so a paragraph's
// footnote marks know their numbers (counted through the story).
thread_local const core::Story* tl_story = nullptr;
thread_local const core::Doc* tl_doc = nullptr;   // s124: chapter numbering reads its starts
struct StoryScope {
    const core::Story* prev;
    const core::Doc* prev_d;
    StoryScope(const core::Doc& d, const core::Story* s) : prev(tl_story), prev_d(tl_doc) { tl_story = s; tl_doc = &d; }
    ~StoryScope() { tl_story = prev; tl_doc = prev_d; }
};
int first_note_of(const core::Para& p) {
    if (tl_story && !tl_story->paras.empty() && &p >= tl_story->paras.data() &&
        &p < tl_story->paras.data() + tl_story->paras.size())
        return tl_doc ? core::note_number(*tl_doc, *tl_story, static_cast<int>(&p - tl_story->paras.data()), 0)
                      : core::note_number(*tl_story, static_cast<int>(&p - tl_story->paras.data()), 0);
    return 1;
}
// s111: the number of the next footnote mark a layout meets, when the
// numbering starts again by page or section (the flow knows it; -1: count
// through the story).
thread_local int tl_note_first = -1;
struct NoteFirst {
    int prev;
    explicit NoteFirst(int n) : prev(tl_note_first) { tl_note_first = n; }
    ~NoteFirst() { tl_note_first = prev; }
};
// A footnote's number as it is set by its mark: smaller, raised.
constexpr double kRefSize = 0.62, kRefRise = 0.36;

// The style a paragraph is set in (Body 11/14 when it names none).
core::Style style_of(const core::Doc& d, const std::string& id) {
    if (const auto* s = d.as<core::Style>(id)) return *s;
    return core::Style{};
}

// s093: ... with the paragraph's own settings over it.
core::Style style_of(const core::Doc& d, const core::Para& p) { return core::doc::para_style(d, p); }

double leading_of(const core::Style& s) { return s.leading > 0 ? s.leading : std::max(1.0, s.size * 1.2); }

// s011: a paragraph's character styles, as Pango attributes over the part of
// its text this layout holds (bytes from `from`).
void add_mark_shapes(PangoLayout* l, PangoAttrList* attrs, const core::Doc& d, const core::Para& p, int from);

void apply_runs(PangoLayout* l, const core::Doc& d, const core::Para& p, int from) {
    // s109: a footnote's mark holds the room its number takes.
    const bool marks = p.text.find(core::kMarkNote, static_cast<std::size_t>(std::max(0, from))) != std::string::npos;
    if (p.runs.empty() && !marks) return;
    PangoAttrList* attrs = pango_attr_list_new();
    const int n = static_cast<int>(p.text.size());
    for (const auto& r : p.runs) {
        const int a = std::max(r.start, from), b = std::min(r.end, n);
        if (b <= a) continue;
        const auto* cs = d.as<core::Style>(r.style);
        const auto add = [&](PangoAttribute* at) {
            at->start_index = static_cast<guint>(a - from);
            at->end_index = static_cast<guint>(b - from);
            pango_attr_list_insert(attrs, at);
        };
        const auto colour = [&](const core::Swatch* sw, double tint) {
            const auto c = core::tint_rgb(core::doc::screen_rgb(*sw), tint);   // s021: at its tint
            add(pango_attr_foreground_new(static_cast<guint16>(((c >> 16) & 0xff) * 257),
                                          static_cast<guint16>(((c >> 8) & 0xff) * 257),
                                          static_cast<guint16>((c & 0xff) * 257)));
        };
        if (cs) {
            if (!cs->family.empty()) add(pango_attr_family_new(cs->family.c_str()));
            if (cs->size > 0) add(pango_attr_size_new(static_cast<int>(std::lround(cs->size * PANGO_SCALE))));
            if (cs->bold) add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            if (cs->italic) add(pango_attr_style_new(PANGO_STYLE_ITALIC));
            if (const auto* sw = d.as<core::Swatch>(cs->color)) colour(sw, cs->color_tint);   // s015: its colour
        }
        // s030: local formatting, over the styles (later attributes win).
        const auto& l = r.local;
        if (l.bold >= 0) add(pango_attr_weight_new(l.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL));
        if (l.italic >= 0) add(pango_attr_style_new(l.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
        if (l.size > 0) add(pango_attr_size_new(static_cast<int>(std::lround(l.size * PANGO_SCALE))));
        if (const auto* sw = d.as<core::Swatch>(l.color)) colour(sw, 1);
    }
    if (marks) add_mark_shapes(l, attrs, d, p, from);
    pango_layout_set_attributes(l, attrs);
    pango_attr_list_unref(attrs);
}

// s109: the number's layout -- the paragraph's face, smaller.
LayoutPtr ref_layout(const PangoFontDescription* base, const std::string& digits) {
    LayoutPtr l(pango_layout_new(measure_context()));
    PangoFontDescription* fd = base ? pango_font_description_copy(base) : pango_font_description_from_string("Serif 11");
    const double size = pango_font_description_get_size(fd) / static_cast<double>(PANGO_SCALE);
    pango_font_description_set_size(fd, static_cast<int>(std::lround(std::max(4.0, size * kRefSize) * PANGO_SCALE)));
    pango_layout_set_font_description(l.get(), fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l.get(), digits.c_str(), static_cast<int>(digits.size()));
    return l;
}

void add_mark_shapes(PangoLayout* l, PangoAttrList* attrs, const core::Doc& d, const core::Para& p, int from) {
    const PangoFontDescription* fd = pango_layout_get_font_description(l);
    const double size = fd ? pango_font_description_get_size(fd) / static_cast<double>(PANGO_SCALE) : 11;
    const int first = first_note_of(p);
    const auto& t = p.text;
    const auto fromz = static_cast<std::size_t>(std::max(0, from));
    for (std::size_t i = t.find(core::kMarkNote, fromz); i != std::string::npos; i = t.find(core::kMarkNote, i + 3)) {
        const int n = tl_note_first >= 0 ? tl_note_first + core::notes_in(t, fromz, i) : first + core::notes_in(t, 0, i);
        const std::string digits = core::note_label(n, d.settings().note_style);
        auto rl = ref_layout(fd, digits);
        PangoRectangle ext;
        pango_layout_get_extents(rl.get(), nullptr, &ext);
        // s114: above the baseline only -- reaching below it (deeper than the
        // font's descent) made the line a hair taller, and the lines after it
        // left the baseline grid. The number is raised; it needs no descent.
        PangoRectangle r{0, static_cast<int>(-size * 0.7 * PANGO_SCALE), ext.width + PANGO_SCALE / 4,
                         static_cast<int>(size * 0.7 * PANGO_SCALE)};
        PangoAttribute* a = pango_attr_shape_new(&r, &r);
        a->start_index = static_cast<guint>(static_cast<int>(i) - from);
        a->end_index = a->start_index + 3;
        pango_attr_list_insert(attrs, a);
    }
}

// s026: a paragraph's indents, where it is in its story (the first-line
// indent may be skipped: at the story's start / after another style).
struct Indents { double left = 0, right = 0, first = 0; };
Indents indents_of(const core::Doc& d, const core::Style& st, const core::Story& s, int p) {
    Indents in{std::max(0.0, st.left_indent), std::max(0.0, st.right_indent), st.first_indent};
    if (st.list.type != core::ListType::None) in.first = 0;   // s045: a list's first line starts with the rest (its marker hangs)
    if (st.skip_after_heading) {
        bool heading = p == 0;   // the story's start counts as one
        if (p > 0) {
            const auto prev = style_of(d, s.paras[static_cast<std::size_t>(p - 1)]);
            heading = (prev.size > 0 ? prev.size : 11) > (st.size > 0 ? st.size : 11) + 0.01;
        }
        if (heading) in.first = 0;
    }
    return in;
}

// Where a paragraph's layout sits in a column `cw` wide: its left offset,
// its width and Pango's indent. Pango hangs a NEGATIVE indent by
// indenting the other lines, so a hanging paragraph's layout starts at
// left + first (the first line's place) and Pango moves the rest in.
struct Geo { double dx, width, indent; };
Geo geo_of(const Indents& in, double cw, bool at_start) {
    const double first = at_start ? in.first : 0;
    const double dx = in.left + std::min(0.0, first);
    return {dx, std::max(1.0, cw - dx - in.right), first};
}

// s026: the style's tab stops on a layout (positions from its left edge).
void apply_tabs(PangoLayout* l, const core::Style& st) {
    if (st.tabs.empty()) return;
    PangoTabArray* tabs = pango_tab_array_new(static_cast<int>(st.tabs.size()), FALSE);
    for (std::size_t i = 0; i < st.tabs.size(); ++i) {
        const auto& t = st.tabs[i];
        PangoTabAlign a = PANGO_TAB_LEFT;
#if PANGO_VERSION_CHECK(1, 50, 0)
        switch (t.align) {
        case core::TabAlign::Left:    a = PANGO_TAB_LEFT; break;
        case core::TabAlign::Center:  a = PANGO_TAB_CENTER; break;
        case core::TabAlign::Right:   a = PANGO_TAB_RIGHT; break;
        case core::TabAlign::Decimal: a = PANGO_TAB_DECIMAL; break;
        }
#endif
        pango_tab_array_set_tab(tabs, static_cast<int>(i), a, static_cast<int>(std::lround(t.pos * PANGO_SCALE)));
#if PANGO_VERSION_CHECK(1, 50, 0)
        if (t.align == core::TabAlign::Decimal) pango_tab_array_set_decimal_point(tabs, static_cast<int>(i), '.');
#endif
    }
    pango_layout_set_tabs(l, tabs);
    pango_tab_array_free(tabs);
}

LayoutPtr make_layout(const core::Style& st, const char* text, int len, double width, bool justify_last,
                      double indent = 0) {
    LayoutPtr l(pango_layout_new(measure_context()));
    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, st.family.empty() ? "Serif" : st.family.c_str());
    pango_font_description_set_size(fd, static_cast<int>(std::lround((st.size > 0 ? st.size : 11) * PANGO_SCALE)));
    pango_font_description_set_weight(fd, st.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fd, st.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(l.get(), fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l.get(), static_cast<int>(std::max(1.0, width) * PANGO_SCALE));
    pango_layout_set_wrap(l.get(), PANGO_WRAP_WORD_CHAR);
    switch (st.align) {
    case core::Align::Left:    pango_layout_set_alignment(l.get(), PANGO_ALIGN_LEFT); break;
    case core::Align::Center:  pango_layout_set_alignment(l.get(), PANGO_ALIGN_CENTER); break;
    case core::Align::Right:   pango_layout_set_alignment(l.get(), PANGO_ALIGN_RIGHT); break;
    case core::Align::Justify:
        pango_layout_set_alignment(l.get(), PANGO_ALIGN_LEFT);
        pango_layout_set_justify(l.get(), TRUE);
#if PANGO_VERSION_CHECK(1, 50, 0)
        // A paragraph that continues in the next frame: its last line HERE is
        // not the paragraph's last line, so it is justified too.
        pango_layout_set_justify_last_line(l.get(), justify_last ? TRUE : FALSE);
#endif
        break;
    }
    (void)justify_last;
    if (indent != 0) pango_layout_set_indent(l.get(), static_cast<int>(std::lround(indent * PANGO_SCALE)));   // s026
    apply_tabs(l.get(), st);
    // s023: markers show as a sign of the same byte length, so every byte
    // place (caret, runs) holds.
    std::string shown(text, static_cast<std::size_t>(len));
    for (std::size_t i = 0; i + 3 <= shown.size(); ++i)
        if (const int k = core::marker_at(shown, i); k >= 0) {
            shown.replace(i, 3, core::marker_sign(k));   // ＃ ⸹ 》 《, s128: → and an unseen anchor
            i += 2;
        }
    pango_layout_set_text(l.get(), shown.c_str(), len);
    return l;
}

}  // namespace

std::vector<std::string> thread_frames(const core::Doc& d, const std::string& story) {
    std::vector<std::string> mine;
    for (const auto& id : d.ids_of<core::Frame>()) {
        const auto& f = *d.as<core::Frame>(id);
        if (f.kind == core::FrameKind::Text && f.story == story) mine.push_back(id);
    }
    std::string head;
    for (const auto& o : mine) {
        bool pointed = false;
        for (const auto& q : mine) pointed = pointed || d.as<core::Frame>(q)->next == o;
        if (!pointed) { head = o; break; }
    }
    std::vector<std::string> out;
    for (std::string k = head; !k.empty() && out.size() <= mine.size();) {
        const auto* f = d.as<core::Frame>(k);
        if (!f || std::find(out.begin(), out.end(), k) != out.end()) break;   // a broken or looping thread stops
        out.push_back(k);
        k = f->next;
    }
    return out;
}

namespace {
struct Cursor {
    int p = 0;
    int start = 0;
    std::vector<NoteRun> carry;   // s117: notes running on into what comes next
    bool ending = false;          // s120: the carry is the endnotes (set in the column, not the foot)
    int  end_from = 0;            // s122: the first paragraph whose notes aren't set as endnotes yet
};

// s018: the free spans of [x0, x1) across the band [y0, y1) -- what the
// wraps leave, narrow scraps dropped. `blocked` gets the lowest bottom of
// what covers the band (where to look next) when nothing is left.
struct Span { double x0, x1; };
std::vector<Span> free_spans(const std::vector<core::WrapRect>& wraps, double x0, double x1, double y0, double y1,
                             double* next_y) {
    std::vector<Span> spans{{x0, x1}};
    double lowest = 0;
    for (const auto& r : wraps) {
        if (r.y1 <= y0 || r.y0 >= y1 || r.x1 <= x0 || r.x0 >= x1) continue;
        lowest = lowest == 0 ? r.y1 : std::min(lowest, r.y1);
        const double cx0 = r.jump ? x0 : r.x0, cx1 = r.jump ? x1 : r.x1;
        std::vector<Span> cut;
        for (const auto& s : spans) {
            if (cx1 <= s.x0 || cx0 >= s.x1) { cut.push_back(s); continue; }
            if (cx0 > s.x0) cut.push_back({s.x0, cx0});
            if (cx1 < s.x1) cut.push_back({cx1, s.x1});
        }
        spans = std::move(cut);
    }
    const double least = std::min(x1 - x0, 18.0);   // a scrap narrower than this takes no text
    std::erase_if(spans, [&](const Span& s) { return s.x1 - s.x0 < least - 0.01; });
    if (spans.empty()) *next_y = std::max(lowest, y0 + 1);
    return spans;
}

// Where a body line's baseline sits in its line box `lead` tall (as
// walk_frame places it: the line's logical box centred in the leading).
// s038: a line set alone from `pos` took `took` bytes and stopped before a
// word that didn't fit. The most of that word that does fit with a hyphen,
// at one of its hyphenation points: the line's new length (0: none fits,
// or the line didn't stop at a word).
int hyphen_cut(const core::Doc& d, const core::Para& para, const core::Style& style, int pos, int took, double w,
               double indent) {
    const std::string& t = para.text;
    const int n = static_cast<int>(t.size());
    const int a = pos + took;   // where the next line would start
    if (took <= 0 || a >= n || a <= pos) return 0;
    // s190 (G4): letters in UTF-8 -- accented ones too (the patterns of more
    // languages than English); the bytes of the letter at i, 0: not a letter.
    const auto letter_len = [&](int i) -> int {
        if (i < 0 || i >= n) return 0;
        const auto c = static_cast<unsigned char>(t[static_cast<std::size_t>(i)]);
        if (c < 0x80) return std::isalpha(c) ? 1 : 0;
        const char* p = t.c_str() + i;
        const gunichar u = g_utf8_get_char_validated(p, n - i);
        if (u == static_cast<gunichar>(-1) || u == static_cast<gunichar>(-2) || !g_unichar_isalpha(u)) return 0;
        return static_cast<int>(g_utf8_next_char(p) - p);
    };
    const auto letter = [&](int i) { return letter_len(i) > 0; };
    // s065: a discretionary hyphen (U+00AD, typed) is part of the word.
    const auto shy = [&](int i) { return i >= 0 && i + 1 < n && static_cast<unsigned char>(t[static_cast<std::size_t>(i)]) == 0xC2 &&
                                         static_cast<unsigned char>(t[static_cast<std::size_t>(i + 1)]) == 0xAD; };
    if (!(letter(a) || shy(a)) || t[static_cast<std::size_t>(a - 1)] != ' ') return 0;   // not a break before a word
    int b = a, letters = 0;
    std::vector<int> typed;   // after each discretionary hyphen (offsets in the word)
    while (letter(b) || shy(b)) {
        if (shy(b)) { b += 2; typed.push_back(b - a); }
        else { b += letter_len(b); ++letters; }
    }
    // s190: a French word runs on past an apostrophe (l'eau, aujourd'hui).
    if (core::hyphen_language(style.lang).code == std::string_view("fr"))
        while (b < n && (t[static_cast<std::size_t>(b)] == '\'' || t.compare(static_cast<std::size_t>(b), 3, "\xE2\x80\x99") == 0) && letter(b + (t[static_cast<std::size_t>(b)] == '\'' ? 1 : 3))) {
            b += t[static_cast<std::size_t>(b)] == '\'' ? 1 : 3;
            while (letter(b)) { b += letter_len(b); ++letters; }
        }
    if (b < n && static_cast<unsigned char>(t[static_cast<std::size_t>(b)]) >= 0x80 && t.compare(static_cast<std::size_t>(b), 2, "\xC2\xAD") != 0 &&
        !std::isspace(static_cast<unsigned char>(t[static_cast<std::size_t>(b)])))
        return 0;   // a word going on in other characters (not a letter we know)
    std::vector<int> at;
    if (!typed.empty()) {
        // Typed hyphens are the only places it breaks; one at its start: never.
        if (typed.front() == 2) return 0;
        at = typed;
        at.erase(std::remove_if(at.begin(), at.end(), [&](int k) { return a + k >= b; }), at.end());   // not after its last letter
    } else {
        if (letters < std::max(1, style.hyph_shortest)) return 0;
        if (!style.hyph_last_word) {   // the paragraph's last word stays whole
            int e = b;
            while (e < n && !std::isalnum(static_cast<unsigned char>(t[static_cast<std::size_t>(e)]))) ++e;
            if (e >= n) return 0;
        }
        at = core::hyphen_points(std::string_view(t).substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a)), -1, -1,
                                 style.lang);   // s190: in its style's language
    }
    for (auto k = at.rbegin(); k != at.rend(); ++k) {
        std::string s = t.substr(static_cast<std::size_t>(pos), static_cast<std::size_t>(a + *k - pos)) + "-";
        auto lay = make_layout(style, s.c_str(), static_cast<int>(s.size()), 1e6, false, indent);
        apply_runs(lay.get(), d, para, pos);
        PangoRectangle ext;
        pango_layout_line_get_extents(pango_layout_get_line_readonly(lay.get(), 0), nullptr, &ext);
        if ((ext.x + ext.width) / static_cast<double>(PANGO_SCALE) + indent <= w + 0.01) return a + *k - pos;
    }
    return 0;
}

double baseline_in_box(const core::Style& style, double lead) {
    auto probe = make_layout(style, "X", 1, 1e6, false);
    PangoLayoutIter* pit = pango_layout_get_iter(probe.get());
    PangoRectangle pr;
    pango_layout_iter_get_line_extents(pit, nullptr, &pr);
    const double off = (lead - pr.height / static_cast<double>(PANGO_SCALE)) / 2 +
                       (pango_layout_iter_get_baseline(pit) - pr.y) / static_cast<double>(PANGO_SCALE);
    pango_layout_iter_free(pit);
    return off;
}

bool touches(const std::vector<core::WrapRect>& wraps, double x0, double x1) {
    return std::any_of(wraps.begin(), wraps.end(), [&](const core::WrapRect& r) { return r.x1 > x0 && r.x0 < x1; });
}

// s039: how many lines the rest of a paragraph (from byte `from`, not its
// first line) takes at width `w` -- as the flow would set it.
int lines_left(const core::Doc& d, const core::Para& para, const core::Style& style, int from, double w) {
    const int len = static_cast<int>(para.text.size());
    if (from >= len) return 0;
    const auto nb = core::no_break_spans(d, para);   // s075
    if (!style.hyphenate && nb.empty()) {
        auto lay = make_layout(style, para.text.c_str() + from, len - from, w, false);
        apply_runs(lay.get(), d, para, from);
        return pango_layout_get_line_count(lay.get());
    }
    int n = 0, run = 0;   // s065: hyphens in a row
    for (int pos = from; pos < len && n < 1000; ++n) {
        auto lay = make_layout(style, para.text.c_str() + pos, len - pos, w, false);
        apply_runs(lay.get(), d, para, pos);
        int took = pango_layout_get_line_readonly(lay.get(), 0)->length;
        const bool may = style.hyph_limit <= 0 || run < style.hyph_limit;
        int cut = style.hyphenate && may ? hyphen_cut(d, para, style, pos, took, w, 0) : 0;
        if (cut > 0 && core::keep_together(para.text, nb, pos, cut) != cut) cut = 0;   // s075: not inside a no-break span
        if (cut > 0) took = cut;
        else took = core::keep_together(para.text, nb, pos, took);
        run = cut > 0 ? run + 1 : 0;
        pos += std::max(1, took);
    }
    return n;
}

// s039: keeps, applied where column `col` broke before paragraph `c.p`
// (byte `c.start`). Lines are sent on to the next column -- never all of
// a column's lines (a rule that can't be kept here is let go).
//   * Keep lines N at a paragraph's start: fewer than N of a paragraph that
//     starts here (or fewer than its drop cap's lines) -> the whole of it
//     goes on. -1 (All together): any split of one that starts here.
//   * N at its end: fewer than N lines left for the next column -> lines go
//     on with them, if N stay here at its start (else the whole of it).
//   * Keep with next: a paragraph ending here whose next starts in the next
//     column goes on with it (and one before it that keeps with it, too).
void apply_keeps(const core::Doc& d, const core::Story& st, const core::Frame& fr, int col, Cursor& c, FrameText& ft) {
    const int paras = static_cast<int>(st.paras.size());
    if (c.p >= paras) return;
    auto& L = ft.lines;
    std::size_t begin = L.size();
    const int band = L.empty() ? 0 : L.back().band;   // s166: this band's column
    while (begin > 0 && L[begin - 1].col == col && L[begin - 1].band == band) --begin;
    const auto here_of = [&](int p) {   // the column's lines of paragraph p: [first, end)
        std::size_t a = L.size();
        while (a > begin && L[a - 1].para == p) --a;
        return a;
    };
    const auto send_on = [&](std::size_t from_line, int p, int start) {
        L.erase(L.begin() + static_cast<std::ptrdiff_t>(from_line), L.end());
        c.p = p;
        c.start = start;
    };
    // The paragraph split here.
    if (c.start > 0) {
        const int q = c.p;
        const auto style = style_of(d, st.paras[static_cast<std::size_t>(q)]);
        const std::size_t a = here_of(q);
        if (style.keep_lines != 0 && a < L.size()) {
            const bool starts_here = L[a].start == 0;
            int here = 0;
            for (std::size_t i = a; i < L.size(); ++i) here += L[i].cap <= 0;
            const int need = std::max(style.keep_lines < 0 ? here + 1 : style.keep_lines,
                                      starts_here && style.drop_lines >= 2 ? style.drop_lines : 0);
            const bool others = a > begin;
            if (starts_here && here < need) {
                if (others) send_on(a, q, 0);
            } else if (style.keep_lines > 0) {
                const Indents ind = indents_of(d, style, st, q);
                const double w = std::max(1.0, core::column_width(fr, col) - ind.left - ind.right);
                const int rest = lines_left(d, st.paras[static_cast<std::size_t>(q)], style, c.start, w);
                if (rest < style.keep_lines) {
                    const int pull = style.keep_lines - rest;
                    const int floor = starts_here ? need : 1;
                    if (here - pull >= floor) {
                        std::size_t cut = L.size();
                        for (int k = 0; k < pull; ++k) {
                            do --cut; while (cut > a && L[cut].cap > 0);
                        }
                        send_on(cut, q, L[cut].start);
                    } else if (starts_here && others) {
                        send_on(a, q, 0);
                    }
                }
            }
        }
    }
    // Keep with next: the paragraph before one that starts in the next column.
    while (c.start == 0 && c.p > 0) {
        const int prev = c.p - 1;
        const std::size_t a = here_of(prev);
        if (a == L.size() || L[a].start != 0 || a == begin) break;   // not wholly here, or all the column holds
        if (!style_of(d, st.paras[static_cast<std::size_t>(prev)]).keep_next) break;
        send_on(a, prev, 0);
    }
}

// s109: the style a paragraph's footnotes are set in -- a paragraph style
// named "Footnote" when there is one, else the paragraph's own, smaller.
core::Style note_style_of(const core::Doc& d, const core::Style& body) {
    for (const auto& id : d.ids_of<core::Style>()) {
        const auto* s = d.as<core::Style>(id);
        if (s->kind != core::StyleKind::Paragraph) continue;
        std::string n = s->name;
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (n == "footnote" || n == "footnotes") return *s;
    }
    core::Style n = body;
    n.size = std::max(6.0, (body.size > 0 ? body.size : 11) * 0.8);
    n.leading = n.size * 1.25;
    n.first_indent = n.left_indent = n.right_indent = n.space_before = n.space_after = 0;
    n.drop_lines = 0;
    n.align = core::Align::Left;
    n.list = {};
    n.tabs.clear();
    return n;
}

// s126: what comes before a note's words -- its number as the document
// writes it ("1", "1.", "1)", raised), then an en space.
std::string note_prefix(const core::Doc& d, int number) {
    std::string n = core::note_label(number, d.settings().note_style);
    switch (d.settings().note_num) {
    case core::Settings::NoteNum::Period: n += "."; break;
    case core::Settings::NoteNum::Paren:  n += ")"; break;
    default: break;
    }
    return n + "\u2002";
}

// s125: a chapter's head among endnotes -- the words of its first paragraph
// (p), markers and marks left out, tabs as spaces, 80 bytes at most.
std::string chapter_head_text(const core::Story& st, int p) {
    if (p < 0 || p >= static_cast<int>(st.paras.size())) return {};
    const auto& t = st.paras[static_cast<std::size_t>(p)].text;
    std::string out;
    for (std::size_t i = 0; i < t.size() && out.size() < 80;) {
        if (static_cast<unsigned char>(t[i]) == 0xEE && i + 2 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0x80) { i += 3; continue; }
        out += t[i] == '\t' ? ' ' : t[i];
        ++i;
    }
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();   // a cut character
    return out;
}

// The note's words, after its number, at width w. s117: a piece of them --
// [from, to) (to -1: the end); a piece from past the start has no number.
LayoutPtr note_layout(const core::Doc& d, const core::Story& st, int p, int k, int number, double w, int from = 0, int to = -1) {
    const auto& para = st.paras[static_cast<std::size_t>(p)];
    const auto ns = note_style_of(d, style_of(d, para));
    const core::Note* note = k >= 0 && k < static_cast<int>(para.notes.size()) ? &para.notes[static_cast<std::size_t>(k)] : nullptr;
    if (k == -2) {   // s125: a chapter's head among story endnotes -- its first paragraph's words, italic
        auto hs = ns;
        hs.italic = true;
        hs.bold = false;
        const auto t = chapter_head_text(st, p);
        return make_layout(hs, t.c_str(), static_cast<int>(t.size()), w, false);
    }
    if (k == -1) {   // s120: the endnotes' title -- the note style, bold, a little larger
        auto hs = ns;
        hs.bold = true;
        hs.size = (ns.size > 0 ? ns.size : 9) * 1.2;
        hs.leading = hs.size * 1.25;
        const auto& t = d.settings().note_title;
        return make_layout(hs, t.c_str(), static_cast<int>(t.size()), w, false);
    }
    const std::string words = note ? note->text : std::string{};
    const int n = static_cast<int>(words.size());
    from = std::clamp(from, 0, n);
    const int end = to < 0 ? n : std::clamp(to, from, n);
    const std::string prefix = from == 0 ? note_prefix(d, number) : std::string{};   // the number, an en space (s126: its look)
    core::Para np{para.style, prefix + words.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(end - from)), {}};
    if (to >= 0 && !d.settings().note_cont.empty()) np.text += "\n" + d.settings().note_cont;   // s118: cut -- the continued line
    const int shift = static_cast<int>(prefix.size()) - from;
    if (note)
        for (auto r : note->runs) {
            r.start = std::max(r.start, from);
            r.end = std::min(r.end, end);
            if (r.end <= r.start) continue;
            r.start += shift;
            r.end += shift;
            np.runs.push_back(r);
        }
    auto lay = make_layout(ns, np.text.c_str(), static_cast<int>(np.text.size()), w, false);
    apply_runs(lay.get(), d, np, 0);
    if (!prefix.empty() && d.settings().note_num == core::Settings::NoteNum::Raised) {   // s126: the number raised, smaller
        PangoAttrList* al = pango_layout_get_attributes(lay.get());
        PangoAttrList* mine = al ? pango_attr_list_copy(al) : pango_attr_list_new();
        const double size = ns.size > 0 ? ns.size : 9;
        const auto end = static_cast<guint>(prefix.size() - 3);   // the number, not the space
        PangoAttribute* sc = pango_attr_scale_new(0.7);
        sc->start_index = 0;
        sc->end_index = end;
        pango_attr_list_insert(mine, sc);
        PangoAttribute* up = pango_attr_rise_new(static_cast<int>(size * 0.35 * PANGO_SCALE));
        up->start_index = 0;
        up->end_index = end;
        pango_attr_list_insert(mine, up);
        pango_layout_set_attributes(lay.get(), mine);
        pango_attr_list_unref(mine);
    }
    return lay;
}

double note_height(const core::Doc& d, const core::Story& st, int p, int k, int number, double w, int from = 0, int to = -1) {
    auto lay = note_layout(d, st, p, k, number, w, from, to);
    PangoRectangle ext;
    pango_layout_get_extents(lay.get(), nullptr, &ext);
    return ext.height / static_cast<double>(PANGO_SCALE);
}

// s118: the continued line's height under a cut piece (0: none set).
double cont_extra(const core::Doc& d, const core::Story& st, int p, double w) {
    const auto& c = d.settings().note_cont;
    if (c.empty()) return 0;
    const auto ns = note_style_of(d, style_of(d, st.paras[static_cast<std::size_t>(p)]));
    auto lay = make_layout(ns, c.c_str(), static_cast<int>(c.size()), w, false);
    PangoRectangle ext;
    pango_layout_get_extents(lay.get(), nullptr, &ext);
    return ext.height / static_cast<double>(PANGO_SCALE);
}

// s117: where a note (from byte `from` of its words) may be cut: after each
// of its lines -- the byte the next line starts at (the last: the end) and
// the height down to there.
struct NoteCut { int at = 0; double h = 0; };
std::vector<NoteCut> note_cuts(const core::Doc& d, const core::Story& st, int p, int k, int number, double w, int from) {
    std::vector<NoteCut> out;
    auto lay = note_layout(d, st, p, k, number, w, from);
    const auto& para = st.paras[static_cast<std::size_t>(p)];
    const int n = k >= 0 && k < static_cast<int>(para.notes.size()) ? static_cast<int>(para.notes[static_cast<std::size_t>(k)].text.size())
                  : k == -1 ? static_cast<int>(d.settings().note_title.size())
                  : k == -2 ? static_cast<int>(chapter_head_text(st, p).size()) : 0;   // s125
    const int prefix = from == 0 && k >= 0 ? static_cast<int>(note_prefix(d, number).size()) : 0;   // s126
    std::vector<int> starts;
    PangoLayoutIter* it = pango_layout_get_iter(lay.get());
    do {
        int y0 = 0, y1 = 0;
        pango_layout_iter_get_line_yrange(it, &y0, &y1);
        starts.push_back(pango_layout_iter_get_line_readonly(it)->start_index);
        out.push_back({n, y1 / static_cast<double>(PANGO_SCALE)});
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    // Each cut: where the next line starts (a paragraph break stays with the line before).
    for (std::size_t i = 0; i + 1 < out.size(); ++i) out[i].at = std::clamp(starts[i + 1] - prefix + from, from, n);
    return out;
}

// The room above a column's first note (its rule sits in it).
double note_gap(const core::Doc& d, const core::Story& st, int p) {
    const auto ns = note_style_of(d, style_of(d, st.paras[static_cast<std::size_t>(p)]));
    return std::max(4.0, (ns.size > 0 ? ns.size : 9) * 0.9);
}

// s109: the column's notes -- those its lines (as kept) refer to, in order,
// stacked up from its foot.
// s111: notes numbered by page / section -- how many the page (section) has so far.
struct NoteCount {
    int next = 0;
};

// s117: what the fill decided about a column's notes, for place_notes: the
// pieces run on from before (set first), what is still to run on after
// them, and a note of this column's cut short (its mark's place, where).
struct ColNotes {
    std::vector<NotePlace> cont;     // run-on pieces, in order
    std::vector<NoteRun>   left;     // still running on after them
    bool split = false;
    int  sp = 0, sk = 0, scut = 0;   // the note cut: paragraph, index, where in its words
};

// s156 (Scott: footnotes are always page width): a frame of several
// columns sets its notes in ONE foot under all of them, the width of its
// text. The columns all stop above it; filled again with the room it took
// until that holds (fill_frame). `reserve`: the room kept in every column;
// `used`: the notes so far (each column's added as it ends); `pending`:
// them, stacked at the frame's end; a note cut short runs on to the next
// frame (`carry_out`) and nothing new comes after it here (`split`).
struct FootShare {
    double x = 0, w = 0;
    double floor = 0;   // s166: the frame's foot -- a band above it is filled to less, its notes still go here
    // s167 (Scott): a narrow frame's notes leave it for the PAGE's foot --
    // they take no room in it, are never cut, and are set at the margins' width.
    bool away = false;
    double reserve = 0, used = 0;
    std::vector<NotePlace> pending;
    std::vector<NoteRun>   carry_out;
    bool split = false;
};

// s109 / s117: the column's notes at its foot -- the run-on pieces first,
// then those its lines (as kept) refer to, in order; a note cut short sends
// its rest on (c.carry). Stacked up from `hmax`.
void place_notes(const core::Doc& d, const core::Story& st, const core::Frame& fr, int col, double cx, double cw,
                 double hmax, FrameText& ft, NoteCount* nc, const ColNotes& cn, Cursor& c, FootShare* fs = nullptr, int band = 0) {
    (void)fr;
    if (fs) { cx = fs->x; cw = fs->w; }   // s156: under all the columns
    std::vector<NotePlace> here = cn.cont;
    for (auto& x : here) { x.col = col; x.x = cx; x.w = cw; }
    std::vector<NoteRun> out = cn.left;
    const bool endnotes = d.settings().note_at != core::NoteAt::Foot;   // s120 / s122: none at the foot
    for (const auto& l : ft.lines) {
        if (endnotes) break;
        if (l.col != col || l.band != band || l.para < 0 || l.para >= static_cast<int>(st.paras.size())) continue;
        const auto& t = st.paras[static_cast<std::size_t>(l.para)].text;
        for (std::size_t i = t.find(core::kMarkNote, static_cast<std::size_t>(l.start)); i != std::string::npos &&
             static_cast<int>(i) < l.end; i = t.find(core::kMarkNote, i + 3)) {
            const int k = core::notes_in(t, 0, i);
            const int num = nc ? ++nc->next : core::note_number(d, st, l.para, k);
            NotePlace np{l.para, k, num, col, cx, 0, cw, 0};
            if (cn.split && l.para == cn.sp && k == cn.sk) {
                np.to = cn.scut;
                out.push_back({l.para, k, num, cn.scut});
            }
            np.h = note_height(d, st, l.para, k, num, cw, 0, np.to);
            here.push_back(np);
        }
    }
    if (fs) {   // s156: kept for the frame's end; a cut note runs on from the frame, not the column
        if (!out.empty()) fs->split = true;
        if (!c.ending) { fs->carry_out.insert(fs->carry_out.end(), out.begin(), out.end()); c.carry.clear(); }
        fs->pending.insert(fs->pending.end(), here.begin(), here.end());
        const double space = std::max(0.0, d.settings().note_space);
        double used = 0;
        for (const auto& n : fs->pending) used += n.h;
        if (!fs->pending.empty()) used += note_gap(d, st, fs->pending.front().para) + space * static_cast<double>(fs->pending.size() - 1);
        fs->used = used;
        return;
    }
    if (!c.ending) c.carry = std::move(out);   // s122: an endnotes carry isn't the foot's to change
    if (here.empty()) return;
    const double space = std::max(0.0, d.settings().note_space);   // s118: between notes
    double total = space * static_cast<double>(here.size() - 1);
    for (const auto& n : here) total += n.h;
    double y = hmax - total;
    for (auto& n : here) {
        n.y = y;
        y += n.h + space;
        ft.notes.push_back(n);
    }
}

// s156: one frame's columns (fill_frame, below); `fs` -- one foot under all
// of them, else each column's own.
bool fill_cols(const core::Doc& d, const core::Story& st, const core::Frame& fr, const std::vector<core::WrapRect>& wraps,
               Cursor& c, double hmax, FrameText& ft, NoteCount* nc, bool notes_only, FootShare* fs,
               double top0 = -1, int stop_para = -1, int band = 0) {   // s166: a band -- its top, the paragraph it stops before
    const int n = core::column_count(fr);
    const int paras = static_cast<int>(st.paras.size());
    // s036: the baseline grid of the page (or master) the frame is on.
    const core::BaselineGrid grid = fr.rot == 0 ? core::baseline_grid(d, fr.owner) : core::BaselineGrid{};
    for (int col = 0; col < n; ++col) {
        const double cw = core::column_width(fr, col), cx = core::column_x(fr, col);
        const double nx = fs ? fs->x : cx, nw = fs ? fs->w : cw;   // s156: where its notes go -- under all the columns
        const bool wrapped = touches(wraps, cx, cx + cw);
        const double top = top0 >= 0 ? top0 : std::max(0.0, fr.inset.top);   // s040: the text starts below the top inset (s166: a band's top)
        double y = top;
        // s109: footnotes -- the room the column's notes take from its foot.
        // s156: under all the columns -- what the columns before took too.
        double notes_h = fs ? fs->used : 0;
        int in_col = 0;     // lines set in this column
        int col_marks = 0;  // s111: footnote marks in them (numbers by page / section count on from these)
        // s117: the most the notes may take (the document's footnote area).
        const double area = std::max(1.0, (hmax - top) * std::clamp(d.settings().note_area, 10, 100) / 100.0);
        ColNotes cn;
        const double space = std::max(0.0, d.settings().note_space);   // s118: between notes
        int col_notes = 0;                                               // pieces at this foot so far
        if (!c.carry.empty() && !c.ending) {   // notes running on: first, as much as the area holds (a line at least -- on it goes)
            auto runs = std::move(c.carry);
            c.carry.clear();
            for (std::size_t i = 0; i < runs.size(); ++i) {
                const auto& r = runs[i];
                if (r.para < 0 || r.para >= static_cast<int>(st.paras.size())) continue;
                const double gap = notes_h <= 0 ? note_gap(d, st, r.para) : 0;
                const double sp = cn.cont.empty() ? 0 : space;   // s118
                const auto cuts = note_cuts(d, st, r.para, r.k, r.number, nw, r.from);
                const double extra = cont_extra(d, st, r.para, nw);
                const double room = area - notes_h - gap - sp;
                std::size_t take = 0;
                while (take < cuts.size() && cuts[take].h + (take + 1 < cuts.size() ? extra : 0) <= room + 0.01) ++take;
                if (take == 0 && cn.cont.empty()) take = 1;
                if (take == 0 || cuts.empty()) { cn.left.assign(runs.begin() + static_cast<std::ptrdiff_t>(i), runs.end()); break; }
                NotePlace np{r.para, r.k, r.number, col, nx, 0, nw, 0};
                np.from = r.from;
                np.cont = true;
                if (take < cuts.size()) np.to = cuts[take - 1].at;
                np.h = note_height(d, st, r.para, r.k, r.number, nw, np.from, np.to);
                notes_h += gap + sp + np.h;
                ++col_notes;
                if (take < cuts.size()) {
                    cn.cont.push_back(np);
                    cn.left.push_back({r.para, r.k, r.number, np.to});
                    cn.left.insert(cn.left.end(), runs.begin() + static_cast<std::ptrdiff_t>(i) + 1, runs.end());
                    break;
                }
                cn.cont.push_back(np);
            }
        }
        // The notes a line [a, b) of paragraph p brings, its bottom at `bottom`:
        // false -- they can't come (the line goes to the next column). Whole
        // when they fit; else the last one cut after the lines that do (s117),
        // the rest running on. Nothing new while a note runs on.
        const auto take_notes = [&](int p, int a, int b, double bottom) {
            const auto& t = st.paras[static_cast<std::size_t>(p)].text;
            if (t.find(core::kMarkNote, static_cast<std::size_t>(a)) == std::string::npos) return true;
            if (d.settings().note_at != core::NoteAt::Foot) return true;   // s120 / s122: endnotes -- nothing at the foot
            if (fs && fs->away) return true;   // s167: they go to the page's foot -- no room taken here
            struct M { int k, num; double h; };
            std::vector<M> ms;
            int j = 0;
            for (std::size_t i = t.find(core::kMarkNote, static_cast<std::size_t>(a)); i != std::string::npos && static_cast<int>(i) < b;
                 i = t.find(core::kMarkNote, i + 3), ++j) {
                const int k = core::notes_in(t, 0, i);
                const int num = nc ? nc->next + col_marks + j + 1 : core::note_number(d, st, p, k);
                ms.push_back({k, num, note_height(d, st, p, k, num, nw)});
            }
            if (ms.empty()) return true;
            if (!cn.left.empty() || cn.split || (fs && fs->split)) return false;
            const double gap = notes_h <= 0 ? note_gap(d, st, p) : 0;
            const double room = std::min(area, (fs ? fs->floor : hmax) - bottom) - notes_h - gap;
            const int n_m = static_cast<int>(ms.size());
            const double spaces = space * (col_notes > 0 ? n_m : n_m - 1);   // s118
            double sum = spaces;
            for (const auto& m : ms) sum += m.h;
            if (sum <= room + 0.01) { notes_h += gap + sum; col_notes += n_m; return true; }
            const double others = sum - ms.back().h;
            const auto cuts = note_cuts(d, st, p, ms.back().k, ms.back().num, nw, 0);
            const double extra = cont_extra(d, st, p, nw);
            std::size_t take = 0;
            while (take < cuts.size() && cuts[take].h + (take + 1 < cuts.size() ? extra : 0) <= room - others + 0.01) ++take;
            // Cut only with two of its lines on each side -- else, as before, the
            // line goes on with its note.
            if (take < cuts.size()) {
                if (cuts.size() - take < 2) take = cuts.size() >= 4 ? cuts.size() - 2 : 0;   // two to run on
                if (take < 2) take = 0;                                                      // two to stay
            }
            const bool first = in_col == 0;   // the column's first line always goes
            if (take == 0 && !first) return false;
            if (take == 0) take = 1;
            col_notes += n_m;
            if (take >= cuts.size()) { notes_h += gap + sum; return true; }
            cn.split = true;
            cn.sp = p;
            cn.sk = ms.back().k;
            cn.scut = cuts[take - 1].at;
            notes_h += gap + others + cuts[take - 1].h + extra;
            return true;
        };
        const auto lim = [&] {   // s156: the shared foot's room (s166: a band above the foot -- its own height)
            if (fs && fs->away) return hmax;   // s167: its notes are set at the page's foot, not here
            return fs ? std::min(hmax, fs->floor - std::max(notes_h, fs->reserve)) : hmax - notes_h;
        };
        // s111: the number the next mark a layout meets will take.
        const auto next_num = [&] { return nc ? nc->next + col_marks + 1 : -1; };
        const auto marks_in = [&](int p, int a, int b) {
            return core::notes_in(st.paras[static_cast<std::size_t>(p)].text, static_cast<std::size_t>(a), static_cast<std::size_t>(b));
        };
        // s120 / s122: endnotes -- the notes of paragraphs [c.end_from, upto) as
        // a carry (the title first), to be set in the column.
        const auto build_end = [&](int upto) {
            std::vector<NoteRun> runs;
            for (int p = std::max(0, c.end_from); p < upto && p < paras; ++p)
                for (int k = 0; k < static_cast<int>(st.paras[static_cast<std::size_t>(p)].notes.size()); ++k)
                    runs.push_back({p, k, core::note_number(d, st, p, k), 0});
            c.end_from = upto;
            if (runs.empty()) return false;
            c.carry.clear();
            if (!d.settings().note_title.empty()) c.carry.push_back({runs.front().para, -1, 0, 0});
            // s125: story endnotes numbered by chapter -- each chapter's group under
            // its head (the numbers start again; the head tells them apart).
            const bool heads = d.settings().note_at == core::NoteAt::StoryEnd && d.settings().note_restart == core::NoteRestart::Chapter;
            const auto chapter_of = [&](int p) {
                for (int i = p; i > 0; --i) {
                    const auto sa = core::doc::start_of(d, st.paras[static_cast<std::size_t>(i)]);
                    if (sa == core::ParaStart::NextPage || sa == core::ParaStart::NextRightPage) return i;
                }
                return 0;
            };
            int chap = -1;
            for (const auto& r : runs) {
                if (heads) {
                    const int ch = chapter_of(r.para);
                    if (ch != chap && !chapter_head_text(st, ch).empty()) c.carry.push_back({ch, -2, 0, 0});
                    chap = ch;
                }
                c.carry.push_back(r);
            }
            c.ending = true;
            return true;
        };
        // Set the endnotes carry from y down; true when all are set.
        const auto set_endnotes = [&] {
            const double space = std::max(0.0, d.settings().note_space);
            std::size_t i = 0;
            for (; i < c.carry.size(); ++i) {
                auto& r = c.carry[i];
                const auto cuts = note_cuts(d, st, r.para, r.k, r.number, cw, r.from);
                if (cuts.empty()) continue;
                const double room = hmax - y;
                std::size_t take = 0;
                while (take < cuts.size() && cuts[take].h <= room + 0.01) ++take;
                // The title goes with the first note's first lines.
                if (r.k < 0 && take == cuts.size() && i + 1 < c.carry.size()) {   // (s125: a chapter's head too)
                    const auto& nx = c.carry[i + 1];
                    const auto nc2 = note_cuts(d, st, nx.para, nx.k, nx.number, cw, nx.from);
                    const double first = nc2.empty() ? 0 : nc2[std::min<std::size_t>(1, nc2.size() - 1)].h;
                    if (cuts.back().h + space + first > room + 0.01 && y > top + 0.01) take = 0;
                }
                if (take < cuts.size() && take > 0 && take < 2 && cuts.size() > 2) take = 0;   // two lines here, or the next column
                if (take == 0 && y <= top + 0.01) take = 1;                                     // a line at least: on it goes
                if (take == 0) break;
                NotePlace np{r.para, r.k, r.number, col, cx, y, cw, 0};
                np.from = r.from;
                np.cont = r.from > 0;
                np.end = true;
                if (take < cuts.size()) np.to = cuts[take - 1].at;
                np.h = cuts[take - 1].h;
                ft.notes.push_back(np);
                y += np.h + space;
                if (take < cuts.size()) { r.from = np.to; break; }
            }
            c.carry.erase(c.carry.begin(), c.carry.begin() + static_cast<std::ptrdiff_t>(i));
            if (!c.carry.empty()) return false;
            c.ending = false;
            return true;
        };
        const auto col_has_end = [&] {
            return std::any_of(ft.notes.begin(), ft.notes.end(), [&](const NotePlace& n) { return n.end && n.col == col; });
        };
        // s122: endnotes running on from the column before: first.
        if (c.ending && !c.carry.empty()) {
            if (!set_endnotes()) continue;
            if (c.p >= paras) return true;
        }
        const auto chapters = d.settings().note_at == core::NoteAt::ChapterEnd;
        while (c.p < paras && !notes_only) {
            if (stop_para >= 0 && c.p >= stop_para && c.start == 0) {   // s166: the band ends before this paragraph
                place_notes(d, st, fr, col, cx, cw, hmax, ft, nc, cn, c, fs, band);
                return false;
            }
            const auto& para = st.paras[static_cast<std::size_t>(c.p)];
            const auto style = style_of(d, para);
            const double lead = leading_of(style);
            // s122: a paragraph that starts a new page ends a chapter -- its
            // notes first, set here.
            if (chapters && c.start == 0 && !c.ending) {
                const auto sa0 = core::doc::start_of(d, para);
                if ((sa0 == core::ParaStart::NextPage || sa0 == core::ParaStart::NextRightPage) && build_end(c.p)) {
                    if (y > top) y += 2 * note_gap(d, st, c.p - 1);
                    if (!set_endnotes()) break;   // on in the next column
                }
            }
            // s043: a paragraph that starts afresh -- not where something is set already.
            // s055: or a break typed before it (its own, over the style's).
            if (const auto sa = c.start == 0 ? core::doc::start_of(d, para) : core::ParaStart::Anywhere; sa != core::ParaStart::Anywhere) {
                if (sa == core::ParaStart::NextColumn && y > top) break;
                if (sa != core::ParaStart::NextColumn && (!ft.lines.empty() || col_has_end())) {   // flow_text finds the frame (s122: endnotes count)
                    place_notes(d, st, fr, col, cx, cw, hmax, ft, nc, cn, c, fs, band);   // s109
                    return false;
                }
            }
            // s011: space before a paragraph -- not at the top of a column.
            const double before = (c.start == 0 && y > top) ? style.space_before : 0;
            const int len = static_cast<int>(para.text.size());
            const Indents ind = indents_of(d, style, st, c.p);   // s026
            double yy = y + before;
            // s036: a line's top moved down so its baseline lands on the
            // next grid line (page coords: the frame's y + the line's).
            const bool on_grid = style.align_grid && grid.step > 0;
            const double boff = on_grid ? baseline_in_box(style, lead) : 0;
            const auto snap = [&](double ly) {
                if (!on_grid) return ly;
                const double k = std::max(0.0, std::ceil((fr.y + ly + boff - grid.start) / grid.step - 1e-6));
                return grid.start + k * grid.step - fr.y - boff;
            };
            // s029: a drop cap -- the first characters `drop_lines` lines tall.
            // Its box joins the wraps for this paragraph, so the lines beside
            // it are set line by line at the width it leaves (as a wrap's).
            std::vector<core::WrapRect> local;
            const bool drop = c.start == 0 && style.drop_lines >= 2 && len > 0;
            double cap_bottom = 0;   // a short paragraph still clears its cap
            if (drop) {
                const int n = std::min(style.drop_lines, 8);
                yy = snap(yy);
                if (yy + n * lead > lim() + 0.01 && y > top) break;   // no room for it here: the next column
                const char* t = para.text.c_str();
                int k = 0;
                for (int ch = 0; ch < std::max(1, style.drop_chars) && k < len; ++ch)
                    k = static_cast<int>(g_utf8_next_char(t + k) - t);
                const double body = style.size > 0 ? style.size : 11;
                core::Style cs = style;
                cs.size = (n - 1) * lead / 0.7 + body;   // its cap height spans the lines (caps ~0.7 em)
                cs.leading = 0;
                cs.tabs.clear();
                auto cl = make_layout(cs, t, k, 1e6, false);
                PangoRectangle ext;
                pango_layout_get_extents(cl.get(), nullptr, &ext);
                const double wc = std::max(1.0, ext.width / static_cast<double>(PANGO_SCALE));
                const double off = baseline_in_box(style, lead);   // where a body line's baseline sits
                const double capx = cx + ind.left, gap = std::max(3.0, body * 0.3);
                ft.lines.push_back({c.p, 0, k, yy, n * lead, col, capx, wc, cs.size, yy + (n - 1) * lead + off});
                ft.lines.back().band = band;
                ++in_col;
                local = wraps;
                local.push_back({cx - 1, yy, capx + wc + gap, yy + n * lead - 0.01, false});
                cap_bottom = yy + n * lead;
                c.start = k;
            }
            // s038: a hyphenating paragraph is set line by line too (each
            // line may end inside a word).
            const auto nobreak = core::no_break_spans(d, para);   // s075: kept on one line -- set line by line too
            if (wrapped || drop || style.hyphenate || !nobreak.empty()) {   // s018: line by line, each at the width the wraps leave
                const auto& wraps_here = drop ? local : wraps;
                int pos = c.start;
                bool ended = false, full = false, stop = false;
                int hy_run = 0;   // s065: hyphenated lines in a row
                while (!ended) {
                    yy = snap(yy);
                    if (yy + lead > lim() + 0.01) { full = true; break; }
                    double next = 0;
                    const auto spans = free_spans(wraps_here, cx, cx + cw, yy, yy + lead, &next);
                    if (spans.empty()) { yy = next; continue; }
                    for (const auto& sp : spans) {
                        // s026: the indents where the span meets the column's edges.
                        // (A line set alone is its layout's first line: a hanging
                        // first line just starts further left.)
                        const double hang = pos == 0 ? std::min(0.0, ind.first) : 0;
                        const double x0 = sp.x0 + (sp.x0 <= cx + 0.01 ? std::max(0.0, ind.left + hang) : 0);
                        const double x1 = sp.x1 - (sp.x1 >= cx + cw - 0.01 ? ind.right : 0);
                        const double w = std::max(1.0, x1 - x0);
                        const NoteFirst nf(next_num());   // s111
                        auto lay = make_layout(style, para.text.c_str() + pos, len - pos, w, false,
                                               pos == 0 ? std::max(0.0, ind.first) : 0);
                        apply_runs(lay.get(), d, para, pos);
                        int took = pango_layout_get_line_readonly(lay.get(), 0)->length;
                        const bool may = style.hyph_limit <= 0 || hy_run < style.hyph_limit;
                        int cut = style.hyphenate && may ? hyphen_cut(d, para, style, pos, took, w,
                                                                      pos == 0 ? std::max(0.0, ind.first) : 0) : 0;
                        if (cut > 0 && core::keep_together(para.text, nobreak, pos, cut) != cut) cut = 0;   // s075
                        if (cut > 0) took = cut;
                        else took = core::keep_together(para.text, nobreak, pos, took);
                        // s065: Pango broke at a typed hyphen -- draw it as a hyphen too.
                        const bool at_shy = took >= 3 && pos + took < len &&   // (after a letter: one at a word's start isn't a break)
                                            std::isalpha(static_cast<unsigned char>(para.text[static_cast<std::size_t>(pos + took - 3)])) &&
                                            static_cast<unsigned char>(para.text[static_cast<std::size_t>(pos + took - 2)]) == 0xC2 &&
                                            static_cast<unsigned char>(para.text[static_cast<std::size_t>(pos + took - 1)]) == 0xAD;
                        // s109: its notes must fit at the foot too (the column's first line always goes).
                        if (!take_notes(c.p, pos, pos + took, std::max(yy + lead, cap_bottom))) { stop = true; break; }   // s117 (below a drop cap)
                        ++in_col;
                        col_marks += marks_in(c.p, pos, pos + took);
                        hy_run = cut > 0 || at_shy ? hy_run + 1 : 0;
                        ft.lines.push_back({c.p, pos, pos + took, yy, lead, col, x0, w});
                        ft.lines.back().band = band;
                        ft.lines.back().hyphen = cut > 0 || at_shy;
                        pos += took;
                        if (pos >= len) { ended = true; break; }
                    }
                    if (stop) { full = true; break; }
                    yy += lead;
                }
                if (ended) {
                    y = std::max(yy, cap_bottom) + style.space_after;
                    ++c.p;
                    c.start = 0;
                    continue;
                }
                (void)full;
                c.start = pos;
                break;
            }
            const Geo g = geo_of(ind, cw, c.start == 0);
            const NoteFirst nf(next_num());   // s111
            auto lay = make_layout(style, para.text.c_str() + c.start, len - c.start, g.width, false, g.indent);
            apply_runs(lay.get(), d, para, c.start);
            const int nl = pango_layout_get_line_count(lay.get());
            int placed = 0;
            for (int i = 0; i < nl; ++i) {
                yy = snap(yy);
                if (yy + lead > lim() + 0.01) break;   // the column is full
                PangoLayoutLine* line = pango_layout_get_line_readonly(lay.get(), i);
                const int a = c.start + line->start_index;
                if (!take_notes(c.p, a, a + line->length, yy + lead)) break;   // s109 / s117
                ++in_col;
                col_marks += marks_in(c.p, a, a + line->length);
                ft.lines.push_back({c.p, a, a + line->length, yy, lead, col, cx + g.dx});
                ft.lines.back().band = band;
                yy += lead;
                ++placed;
            }
            if (placed == nl) {   // the whole (rest of the) paragraph fit
                y = yy + style.space_after;
                ++c.p;
                c.start = 0;
                continue;
            }
            // Continue in the next column from the first line that didn't fit.
            c.start += pango_layout_get_line_readonly(lay.get(), placed)->start_index;
            break;
        }
        if (c.p >= paras && !notes_only && d.settings().note_at != core::NoteAt::Foot) {   // s120: endnotes at the story's end
            if (!c.ending && build_end(paras) && y > top) y += 2 * note_gap(d, st, paras - 1);   // room above them
            if (c.ending && !set_endnotes()) continue;   // the rest in the next column
            return true;
        }
        if (c.p >= paras || notes_only) {
            place_notes(d, st, fr, col, cx, cw, hmax, ft, nc, cn, c, fs, band);   // s109
            if (notes_only) { if (c.carry.empty()) return false; continue; }
            if (c.carry.empty()) return true;
            continue;   // s117: the text has ended, a note runs on
        }
        if (!col_has_end()) apply_keeps(d, st, fr, col, c, ft);   // s039 (not under endnotes set after the lines)
        place_notes(d, st, fr, col, cx, cw, hmax, ft, nc, cn, c, fs, band);
    }
    return false;
}

// Fill one frame's columns from `c`, each column at most `hmax` tall. True
// when the story ended inside this frame.
// s117: `notes_only` -- the frame takes the notes running on and no text
// (the next paragraph starts on a later page).
// s156: several columns, footnotes at the foot: one foot under them all --
// filled, then again with the room the notes took kept in every column,
// until it holds (a few times at most; the last keeps the most it needed).
// s166: is paragraph p one that spans the columns?
bool spans(const core::Doc& d, const core::Story& st, int p) {
    return p >= 0 && p < static_cast<int>(st.paras.size()) && style_of(d, st.paras[static_cast<std::size_t>(p)]).span;
}

// s183 (G1): paragraph p's sub-columns when it splits (a one-column
// frame's; 0 = not split).
int splits(const core::Doc& d, const core::Story& st, const core::Frame& fr, int p) {
    if (core::column_count(fr) != 1 || p < 0 || p >= static_cast<int>(st.paras.size())) return 0;
    const int k = style_of(d, st.paras[static_cast<std::size_t>(p)]).split;
    return k >= 2 ? k : 0;
}
// A paragraph that starts a band of its own: spanning (2+ columns) or split (one).
bool own_band(const core::Doc& d, const core::Story& st, const core::Frame& fr, int p) {
    return core::column_count(fr) >= 2 ? spans(d, st, p) : splits(d, st, fr, p) > 0;
}

// s166 (Scott): a frame of columns with paragraphs spanning them -- in
// bands, top down: the columns up to the next spanning paragraph, balanced
// (the shortest height that still holds them); the spanning paragraph
// across all the columns below that; and so on. Each band's columns are
// their own (Line.band); the notes go to the frame's foot (fs) as before.
bool fill_bands(const core::Doc& d, const core::Story& st, const core::Frame& fr, const std::vector<core::WrapRect>& wraps,
                Cursor& c, double hmax, FrameText& ft, NoteCount* nc, bool notes_only, FootShare* fs) {
    const int paras = static_cast<int>(st.paras.size());
    const int n = core::column_count(fr);
    double top = std::max(0.0, fr.inset.top);
    int band = 0;
    for (const auto& l : ft.lines) band = std::max(band, l.band + 1);
    const auto bottom_of = [&](std::size_t from) {
        double b = top;
        for (std::size_t i = from; i < ft.lines.size(); ++i) b = std::max(b, ft.lines[i].y + ft.lines[i].h);
        return b;
    };
    core::Frame whole = fr;   // a spanning paragraph's frame: one column, the width of them all
    whole.columns = 1;
    whole.widths.clear();
    whole.w = fr.w;
    const double x0 = core::column_x(fr, 0), x1 = core::column_x(fr, n - 1) + core::column_width(fr, n - 1);
    whole.inset.left = x0;
    whole.inset.right = fr.w - x1;
    while (true) {
        if (notes_only || c.p >= paras) return fill_cols(d, st, fr, wraps, c, hmax, ft, nc, notes_only, fs, top, -1, band);
        if (const int k = splits(d, st, fr, c.p); k > 0) {   // s183: split -- its lines balanced over k sub-columns
            const auto style = style_of(d, st.paras[static_cast<std::size_t>(c.p)]);
            const double before = c.start == 0 && top > std::max(0.0, fr.inset.top) + 0.01 ? style.space_before : 0;
            core::Frame sub = fr;   // the frame's one column cut in k, the split gutter between
            sub.columns = k;
            sub.gutter = std::max(0.0, style.split_gutter);
            sub.widths.clear();
            sub.balance = false;
            const int p0 = c.p;
            const auto holds = [&](double h) {
                Cursor t = c;
                FrameText tmp = ft;
                NoteCount tc = nc ? *nc : NoteCount{};
                FootShare tf = fs ? *fs : FootShare{};
                fill_cols(d, st, sub, wraps, t, h, tmp, nc ? &tc : nullptr, false, fs ? &tf : nullptr, top + before, p0 + 1, band);
                return t.p > p0;
            };
            const std::size_t from = ft.lines.size();
            double h = hmax;
            if (holds(hmax)) {   // the shortest height that still holds it all: balanced
                double lo = top + before, hi = hmax;
                for (int it = 0; it < 20 && hi - lo > 0.5; ++it) {
                    const double mid = (lo + hi) / 2;
                    if (holds(mid)) hi = mid; else lo = mid;
                }
                h = hi;
            }
            fill_cols(d, st, sub, wraps, c, h, ft, nc, false, fs, top + before, p0 + 1, band);
            for (std::size_t i = from; i < ft.lines.size(); ++i) {
                ft.lines[i].cw = core::column_width(sub, ft.lines[i].col);
                ft.lines[i].band = band;
            }
            ++band;
            if (c.p >= paras) return true;
            if (c.p == p0) return false;   // it didn't all fit: the frame is full (on in the next)
            top = bottom_of(from) + style.space_after;
            continue;
        }
        if (c.start == 0 && spans(d, st, c.p)) {   // across them all
            const auto style = style_of(d, st.paras[static_cast<std::size_t>(c.p)]);
            const double before = top > std::max(0.0, fr.inset.top) + 0.01 ? style.space_before : 0;
            const std::size_t from = ft.lines.size();
            const int p0 = c.p;
            const bool ended = fill_cols(d, st, whole, wraps, c, hmax, ft, nc, false, fs, top + before, c.p + 1, band);
            for (std::size_t i = from; i < ft.lines.size(); ++i) { ft.lines[i].cw = x1 - x0; ft.lines[i].band = band; }
            ++band;
            if (ended) return true;
            if (c.p == p0) return false;   // it didn't all fit: the frame is full
            top = bottom_of(from) + style.space_after;
            continue;
        }
        int sp = -1;   // the next spanning (s183: or split) paragraph
        for (int p = c.p + 1; p < paras; ++p) if (own_band(d, st, fr, p)) { sp = p; break; }
        if (sp < 0) return fill_cols(d, st, fr, wraps, c, hmax, ft, nc, false, fs, top, -1, band);
        if (n == 1) {   // s183: one column -- nothing to balance; up to the split paragraph
            const std::size_t from = ft.lines.size();
            fill_cols(d, st, fr, wraps, c, hmax, ft, nc, false, fs, top, sp, band);
            if (c.p < sp) return false;   // the frame is full first
            ++band;
            top = bottom_of(from) + style_of(d, st.paras[static_cast<std::size_t>(sp - 1)]).space_after;
            continue;
        }
        // The columns up to it -- the shortest height that holds them all.
        const auto holds = [&](double h) {
            Cursor t = c;
            FrameText tmp = ft;
            NoteCount tc = nc ? *nc : NoteCount{};
            FootShare tf = fs ? *fs : FootShare{};
            fill_cols(d, st, fr, wraps, t, h, tmp, nc ? &tc : nullptr, false, fs ? &tf : nullptr, top, sp, band);
            return t.p >= sp && t.start == 0;
        };
        const std::size_t from = ft.lines.size();
        if (!holds(hmax)) return fill_cols(d, st, fr, wraps, c, hmax, ft, nc, false, fs, top, sp, band);   // the frame is full first
        double lo = top, hi = hmax;
        for (int k = 0; k < 20 && hi - lo > 0.5; ++k) {
            const double mid = (lo + hi) / 2;
            if (holds(mid)) hi = mid; else lo = mid;
        }
        fill_cols(d, st, fr, wraps, c, hi, ft, nc, false, fs, top, sp, band);
        ++band;
        top = bottom_of(from);
    }
}

// Fill one frame's columns from `c`, each column at most `hmax` tall. True
// when the story ended inside this frame.
// s117: `notes_only` -- the frame takes the notes running on and no text
// (the next paragraph starts on a later page).
// s156: several columns, footnotes at the foot: one foot under them all --
// filled, then again with the room the notes took kept in every column,
// until it holds (a few times at most; the last keeps the most it needed).
// s166: paragraphs spanning the columns -- in bands (fill_bands).
// s167: a narrow frame's notes, away to the page's foot: under the margins
// (x, w, frame coordinates), stacked up from `bottom`.
struct Away { double x = 0, w = 0, bottom = 0; };

bool fill_frame(const core::Doc& d, const core::Story& st, const core::Frame& fr, const std::vector<core::WrapRect>& wraps,
                Cursor& c, double hmax, FrameText& ft, NoteCount* nc = nullptr, bool notes_only = false,
                const Away* away = nullptr) {
    const int n = core::column_count(fr);
    bool banded = false;
    for (int p = std::max(0, c.p); p < static_cast<int>(st.paras.size()) && !banded; ++p) banded = own_band(d, st, fr, p);   // s183: a split too
    const auto fill_h = [&](FootShare* fs, double h) {
        return banded ? fill_bands(d, st, fr, wraps, c, h, ft, nc, notes_only, fs)
                      : fill_cols(d, st, fr, wraps, c, h, ft, nc, notes_only, fs);
    };
    const auto fill = [&](FootShare* fs) { return fill_h(fs, hmax); };
    if (away && d.settings().note_at == core::NoteAt::Foot) {   // s167: its notes at the page's foot
        // Its own text stops above them too, where it reaches that low:
        // filled again to just above them (a few times at most).
        const Cursor c0 = c;
        const FrameText ft0 = ft;
        const NoteCount nc0 = nc ? *nc : NoteCount{};
        const double space = std::max(0.0, d.settings().note_space);
        double h = hmax;
        FootShare fs;
        bool done = false;
        double total = 0;
        for (int pass = 0; pass < 4; ++pass) {
            fs = FootShare{};
            fs.x = away->x;
            fs.w = std::max(1.0, away->w);
            fs.floor = h;
            fs.away = true;
            done = fill_h(&fs, h);
            total = space * static_cast<double>(fs.pending.empty() ? 0 : fs.pending.size() - 1);
            for (const auto& x : fs.pending) total += x.h;
            if (!fs.pending.empty()) total += note_gap(d, st, fs.pending.front().para);
            const double room = away->bottom - total;
            double low = 0;
            for (const auto& l : ft.lines) low = std::max(low, l.y + l.h);
            if (fs.pending.empty() || low <= room + 0.01 || room >= h - 0.01) break;
            h = std::max(std::max(0.0, fr.inset.top), room);
            c = c0;
            ft = ft0;
            if (nc) *nc = nc0;
        }
        if (!fs.pending.empty()) total -= note_gap(d, st, fs.pending.front().para);
        double y = away->bottom - total;
        for (auto& x : fs.pending) {
            x.col = 0;
            x.y = y;
            y += x.h + space;
            ft.notes.push_back(x);
        }
        return done;
    }
    // s183: a one-column frame with a split paragraph too -- its notes at the frame's foot, not the band's.
    if ((n < 2 && !banded) || d.settings().note_at != core::NoteAt::Foot) return fill(nullptr);
    const Cursor c0 = c;
    const FrameText ft0 = ft;
    const NoteCount nc0 = nc ? *nc : NoteCount{};
    const double x0 = core::column_x(fr, 0);
    const double x1 = core::column_x(fr, n - 1) + core::column_width(fr, n - 1);
    double reserve = 0;
    for (int pass = 0;; ++pass) {
        FootShare fs;
        fs.x = x0;
        fs.w = std::max(1.0, x1 - x0);
        fs.reserve = reserve;
        fs.floor = hmax;
        const bool done = fill(&fs);
        if (fs.used <= reserve + 0.01 || pass >= 4) {
            // Stacked up from the foot, in order.
            const double space = std::max(0.0, d.settings().note_space);
            double total = space * static_cast<double>(fs.pending.empty() ? 0 : fs.pending.size() - 1);
            for (const auto& x : fs.pending) total += x.h;
            double y = hmax - total;
            for (auto& x : fs.pending) {
                x.col = 0;   // one foot (one rule over it)
                x.y = y;
                y += x.h + space;
                ft.notes.push_back(x);
            }
            if (!fs.carry_out.empty() && !c.ending) c.carry = fs.carry_out;
            return done && c.carry.empty();
        }
        reserve = std::max(reserve, fs.used);   // again, with that room kept
        c = c0;
        ft = ft0;
        if (nc) *nc = nc0;
    }
}
}  // namespace

namespace {
// s042: a frame's lines moved down its height, column by column: Centre
// and Bottom by the room left below them; Justify shares that room out
// between paragraphs (between lines, with one paragraph). Not where it
// would undo what placed them: text wrap, balanced columns, baseline grid.
void vertical_justify(const core::Doc& d, const core::Story& st, const core::Frame& fr,
                      const std::vector<core::WrapRect>& wraps, FrameText& ft) {
    if (fr.valign == core::VAlign::Top || ft.lines.empty() || fr.kind != core::FrameKind::Text) return;
    if (!ft.notes.empty()) return;   // s109: the notes hold the foot
    if (std::any_of(ft.lines.begin(), ft.lines.end(), [](const Line& l) { return l.band > 0; })) return;   // s166: in bands
    if (!wraps.empty() || (fr.balance && core::column_count(fr) > 1)) return;
    if (fr.rot == 0 && core::baseline_grid(d, fr.owner).step > 0)
        for (const auto& l : ft.lines)
            if (l.para < static_cast<int>(st.paras.size()) &&
                style_of(d, st.paras[static_cast<std::size_t>(l.para)]).align_grid) return;
    const double bottom = fr.h - std::max(0.0, fr.inset.bottom);
    for (int col = 0; col < core::column_count(fr); ++col) {
        std::vector<Line*> ls;
        double last = 0;
        bool cap = false;
        for (auto& l : ft.lines)
            if (l.col == col) { ls.push_back(&l); last = std::max(last, l.y + l.h); cap = cap || l.cap > 0; }
        const double room = bottom - last;
        if (ls.empty() || room <= 0.01) continue;
        const auto shift = [](Line* l, double dy) { l->y += dy; if (l->cap > 0) l->base += dy; };
        if (fr.valign != core::VAlign::Justify) {
            const double dy = fr.valign == core::VAlign::Center ? room / 2 : room;
            for (auto* l : ls) shift(l, dy);
            continue;
        }
        // Justify: the gaps -- before each paragraph that starts after the
        // column's first line; else (one paragraph) before each line.
        int gaps = 0;
        for (std::size_t i = 1; i < ls.size(); ++i) gaps += ls[i]->para != ls[i - 1]->para;
        if (gaps > 0) {
            int k = 0;
            for (std::size_t i = 0; i < ls.size(); ++i) {
                if (i > 0 && ls[i]->para != ls[i - 1]->para) ++k;
                shift(ls[i], room * k / gaps);
            }
        } else if (!cap && ls.size() > 1) {
            for (std::size_t i = 0; i < ls.size(); ++i) shift(ls[i], room * static_cast<double>(i) / static_cast<double>(ls.size() - 1));
        }
    }
}
}  // namespace

StoryFlow flow_story(const core::Doc& d, const std::string& story) {
    const auto* st = d.as<core::Story>(story);
    if (!st) return {};
    return flow_text(d, story, *st);
}

namespace {
struct Prev {   // s105
    const core::Story*     text;
    const StoryFlow*       flow;
    int                    budget = 0;   // s106: frames to set at most (0: all it takes)
};
// ── s167 (Scott): a narrow frame's footnotes at the PAGE's foot ─────────────
// A text frame on a page narrower than nine tenths of the margins (a
// sidebar): its notes leave it -- set under the margins, at their width, at
// the page's foot. Every other text frame on that page stops above them.
// What a narrow frame's notes take is found from its story's flow made
// PLAIN (no other frame's notes reserved -- no story waits on another's).
namespace {
thread_local bool tl_foot_plain = false;
struct FootCache { const core::Doc* d = nullptr; std::uint64_t rev = ~0ull; std::map<std::string, StoryFlow> flows; };
thread_local FootCache tl_foot;

bool notes_away(const core::Doc& d, const core::Frame& f) {
    if (f.kind != core::FrameKind::Text || f.story.empty() || !d.as<core::Page>(f.owner)) return false;
    const core::Rect mr = core::margin_rect(d, f.owner);
    return mr.w > 0 && f.w < 0.9 * mr.w;
}

bool story_has_notes(const core::Doc& d, const std::string& story) {
    const auto* st = d.as<core::Story>(story);
    if (!st) return false;
    for (const auto& p : st->paras) if (!p.notes.empty()) return true;
    return false;
}

// The room the notes of narrow frame `fid` take at the page's foot (its plain flow).
double away_height(const core::Doc& d, const std::string& fid) {
    const auto* f = d.as<core::Frame>(fid);
    if (!f) return 0;
    if (tl_foot.d != &d || tl_foot.rev != d.revision()) { tl_foot = {}; tl_foot.d = &d; tl_foot.rev = d.revision(); }
    auto it = tl_foot.flows.find(f->story);
    if (it == tl_foot.flows.end()) {
        const bool was = tl_foot_plain;
        tl_foot_plain = true;
        StoryFlow fl = flow_story(d, f->story);
        tl_foot_plain = was;
        it = tl_foot.flows.emplace(f->story, std::move(fl)).first;
    }
    const auto* st = d.as<core::Story>(f->story);
    for (const auto& ft : it->second.frames) {
        if (ft.frame != fid) continue;
        double h = 0;
        int n = 0;
        for (const auto& np : ft.notes) if (!np.end) { h += np.h; ++n; }
        if (n == 0) return 0;
        const double space = std::max(0.0, d.settings().note_space);
        int first = -1;
        for (const auto& np : ft.notes) if (!np.end) { first = np.para; break; }
        return h + space * (n - 1) + (st && first >= 0 && first < static_cast<int>(st->paras.size()) ? note_gap(d, *st, first) : 0);
    }
    return 0;
}

// For frame `fid`: away (its notes to the page's foot -- where) or the
// floor the page's narrow frames' notes leave it (frame coordinates; < 0 none).
struct PageFoot { bool away = false; Away at; double limit = -1; };
PageFoot page_foot(const core::Doc& d, const std::string& fid) {
    PageFoot pf;
    if (d.settings().note_at != core::NoteAt::Foot) return pf;
    const auto* fr = d.as<core::Frame>(fid);
    if (!fr || fr->kind != core::FrameKind::Text || !d.as<core::Page>(fr->owner)) return pf;
    const core::Rect mr = core::margin_rect(d, fr->owner);
    if (mr.w <= 0) return pf;
    std::vector<std::string> narrow;
    for (const auto& g : d.frames_on(fr->owner))
        if (const auto* gf = d.as<core::Frame>(g); gf && notes_away(d, *gf) && story_has_notes(d, gf->story)) narrow.push_back(g);
    const bool me = notes_away(d, *fr) && story_has_notes(d, fr->story);   // (none: nothing to send away)
    if (me) {
        pf.away = true;
        pf.at.x = mr.x - fr->x;
        pf.at.w = mr.w;
        double below = 0;   // the narrow frames after it stack under it
        if (!tl_foot_plain) {
            bool after = false;
            for (const auto& g : narrow) {
                if (after) below += away_height(d, g);
                if (g == fid) after = true;
            }
        }
        pf.at.bottom = mr.b() - below - fr->y;
        return pf;
    }
    if (narrow.empty() || tl_foot_plain) return pf;
    double extra = 0;
    for (const auto& g : narrow) extra += away_height(d, g);
    if (extra > 0) pf.limit = mr.b() - extra - fr->y;
    return pf;
}
}  // namespace

// s167: stories whose flow depends on `story`'s notes at a page's foot --
// the other text frames on the pages where its narrow frames are.
std::vector<std::string> foot_neighbours_of(const core::Doc& d, const std::string& story) {
    std::vector<std::string> out;
    if (d.settings().note_at != core::NoteAt::Foot) return out;
    for (const auto& fid : d.ids_of<core::Frame>()) {
        const auto* f = d.as<core::Frame>(fid);
        if (!f || f->story != story || !notes_away(d, *f)) continue;
        for (const auto& g : d.frames_on(f->owner)) {
            const auto* gf = d.as<core::Frame>(g);
            if (gf && gf->kind == core::FrameKind::Text && !gf->story.empty() && gf->story != story &&
                std::find(out.begin(), out.end(), gf->story) == out.end())
                out.push_back(gf->story);
        }
    }
    return out;
}

StoryFlow flow_impl(const core::Doc& d, const std::string& story, const core::Story& text,
                    const std::map<std::string, double>* heights, const FlowTick* tick = nullptr,
                    const Prev* prev = nullptr);
}

StoryFlow reflow_story(const core::Doc& d, const std::string& story, const core::Story& old_text,
                       const StoryFlow& old_flow, int budget) {   // s105
    const auto* st = d.as<core::Story>(story);
    if (!st) return {};
    const Prev prev{&old_text, &old_flow, budget};
    return flow_impl(d, story, *st, nullptr, nullptr, &prev);
}

StoryFlow flow_story(const core::Doc& d, const std::string& story, const FlowTick& tick) {   // s102
    const auto* st = d.as<core::Story>(story);
    if (!st) return {};
    return flow_impl(d, story, *st, nullptr, tick ? &tick : nullptr);
}

StoryFlow flow_text(const core::Doc& d, const std::string& story, const core::Story& text) {
    return flow_impl(d, story, text, nullptr);
}

std::vector<core::DocJournal::Settled> fit_frames(const core::Doc& d) {
    std::vector<core::DocJournal::Settled> out;
    for (const auto& id : d.ids_of<core::Frame>()) {
        const auto* f = d.as<core::Frame>(id);
        if (!f->autosize || f->kind != core::FrameKind::Text || !f->next.empty() || core::column_count(*f) != 1) continue;
        const auto* st = d.as<core::Story>(f->story);
        if (!st) continue;
        const std::map<std::string, double> room{{id, 1e6}};
        const auto flow = flow_impl(d, f->story, *st, &room);
        double bottom = -1;
        for (const auto& ft : flow.frames)
            if (ft.frame == id)
                for (const auto& l : ft.lines) bottom = std::max(bottom, l.y + l.h);
        if (bottom < 0) continue;   // no text here: leave it as it is
        const double h = std::max(1.0, bottom + std::max(0.0, f->inset.bottom));
        if (std::abs(h - f->h) < 0.01) continue;
        core::Frame g = *f;
        g.h = h;
        out.push_back({id, g});
    }
    return out;
}

namespace {
StoryFlow flow_impl(const core::Doc& d, const std::string& story, const core::Story& text,
                    const std::map<std::string, double>* heights, const FlowTick* tick, const Prev* prev) {
    StoryFlow out;
    const auto* st = &text;
    const StoryScope scope(d, st);   // s109: notes numbered through this text
    const auto chain = thread_frames(d, story);
    for (const auto& f : chain) out.frames.push_back({f, {}});
    if (chain.empty()) return out;   // not placed: nothing flows, nothing is "over"

    Cursor c;
    bool done = st->paras.empty();
    std::string last_page;   // s043: the page of the last frame that took lines
    std::size_t first = 0;

    // s105: from an earlier flow of this story -- the paragraphs changed are
    // [k0, k1) now (they were [k0, k1 - delta)); the frames before the one
    // where paragraph k0 - 1 starts are kept as they were.
    int k1 = 0, delta = 0, note_delta = 0;
    bool resume = false;
    if (prev && !heights && !done && prev->flow->frames.size() == chain.size()) {
        bool same_chain = true;
        for (std::size_t i = 0; i < chain.size() && same_chain; ++i) same_chain = prev->flow->frames[i].frame == chain[i];
        const auto& op = prev->text->paras;
        const auto& np = st->paras;
        if (same_chain && !op.empty()) {
            const std::size_t lim = std::min(op.size(), np.size());
            std::size_t k0 = 0;
            while (k0 < lim && op[k0] == np[k0]) ++k0;
            std::size_t suf = 0;
            while (suf < lim - k0 && op[op.size() - 1 - suf] == np[np.size() - 1 - suf]) ++suf;
            k1 = static_cast<int>(np.size() - suf);
            delta = static_cast<int>(np.size()) - static_cast<int>(op.size());
            // s109: footnotes added or taken away renumber every note after --
            // the old frames can't be taken as they were (their numbers, and
            // the room a number takes in its line, may differ).
            for (std::size_t i = k0; i < np.size() - suf; ++i) note_delta += static_cast<int>(np[i].notes.size());
            for (std::size_t i = k0; i < op.size() - suf; ++i) note_delta -= static_cast<int>(op[i].notes.size());
            // s124: numbered by chapter -- a chapter start added, taken away or
            // changed renumbers the notes after it: nothing after is taken as it was.
            if (d.settings().note_restart == core::NoteRestart::Chapter) {
                const auto starts = [&](const core::Para& pa) {
                    const auto sa = core::doc::start_of(d, pa);
                    return sa == core::ParaStart::NextPage || sa == core::ParaStart::NextRightPage;
                };
                bool brk = false;
                for (std::size_t i = k0; i < np.size() - suf; ++i) brk = brk || starts(np[i]);
                for (std::size_t i = k0; i < op.size() - suf; ++i) brk = brk || starts(op[i]);
                if (brk) note_delta += 1 << 20;
            }
            const int back = k0 > 0 ? static_cast<int>(k0) - 1 : 0;
            // The last frame the old flow came to at or before paragraph `back`'s start.
            std::size_t fi0 = 0;
            bool found = false;
            for (std::size_t i = 0; i < chain.size(); ++i) {
                const auto& ft = prev->flow->frames[i];
                if (ft.from_p < 0) break;
                if (ft.from_p < back || (ft.from_p == back && ft.from_b == 0)) { fi0 = i; found = true; }
                else break;
            }
            if (found) {
                for (std::size_t i = 0; i < fi0; ++i) out.frames[i] = prev->flow->frames[i];
                const auto& at = prev->flow->frames[fi0];
                c = Cursor{at.from_p, at.from_b, at.carry_in, at.ending_in, at.end_from_in};   // s117 / s120 / s122: and what ran on into it
                last_page = at.page_before;
                first = fi0;
                resume = true;
            }
        }
    }

    for (std::size_t fi = first; fi < chain.size() && !done; ++fi) {
        const auto& fr = *d.as<core::Frame>(chain[fi]);
        out.frames[fi].from_p = c.p;   // s105
        out.frames[fi].from_b = c.start;
        out.frames[fi].carry_in = c.carry;   // s117
        out.frames[fi].ending_in = c.ending;   // s120
        out.frames[fi].end_from_in = c.end_from;   // s122
        out.frames[fi].page_before = last_page;
        // s111: numbering by page / section -- the notes before this frame on the same one.
        // s120: endnotes count through the story.
        const auto restart = (d.settings().note_at != core::NoteAt::Foot || d.settings().note_restart == core::NoteRestart::Chapter)
                                 ? core::NoteRestart::Story : d.settings().note_restart;   // s124: by chapter -- the text's count
        const auto key_of = [&](const std::string& fid) -> std::string {
            const auto* f = d.as<core::Frame>(fid);
            if (!f) return {};
            if (restart == core::NoteRestart::Page || !d.as<core::Page>(f->owner)) return f->owner;
            return "s:" + d.section_of(f->owner);
        };
        int note_from = 0;
        if (restart != core::NoteRestart::Story) {
            const std::string key = key_of(chain[fi]);
            for (std::size_t i = 0; i < fi; ++i)
                if (!out.frames[i].notes.empty() && key_of(chain[i]) == key)
                    for (const auto& np : out.frames[i].notes) note_from += np.cont ? 0 : 1;   // s117: a run-on piece isn't a new note
        }
        out.frames[fi].note_from = note_from;
        // s105: come to this frame just as before, past what changed: the
        // rest is the old flow, its paragraphs moved by delta.
        if (resume && fi > first && c.p > k1 && note_delta == 0) {
            const auto& old = prev->flow->frames[fi];
            const auto moved = [&](std::vector<NoteRun> v) { for (auto& r : v) r.para += delta; return v; };   // s117
            if (old.from_p >= 0 && old.from_p + delta == c.p && old.from_b == c.start && old.page_before == last_page &&
                old.note_from == note_from && moved(old.carry_in) == c.carry && old.ending_in == c.ending &&
                (old.end_from_in >= k1 - delta ? old.end_from_in + delta : old.end_from_in) == c.end_from) {
                for (std::size_t i = fi; i < chain.size(); ++i) {
                    FrameText ft = prev->flow->frames[i];
                    for (auto& l : ft.lines) l.para += delta;
                    for (auto& nt : ft.notes) nt.para += delta;   // s109 (numbers: the same notes come before)
                    for (auto& r : ft.carry_in) r.para += delta;   // s117
                    if (ft.end_from_in >= k1 - delta) ft.end_from_in += delta;   // s122
                    if (ft.from_p >= 0) ft.from_p += delta;
                    out.frames[i] = std::move(ft);
                }
                for (const auto& ft : out.frames) out.lines += static_cast<int>(ft.lines.size());
                out.overset = prev->flow->overset;
                return out;
            }
        }
        // s106: the budget spent and not resettled: the old frames from here
        // stand in -- their lines of paragraphs that didn't change, moved.
        if (resume && prev->budget > 0 && out.filled >= prev->budget) {
            const int keep_from = k1 - delta;   // in the old numbering: the paragraphs after the change
            for (std::size_t i = fi; i < chain.size(); ++i) {
                FrameText ft = prev->flow->frames[i];
                std::erase_if(ft.lines, [&](const Line& l) { return l.para < keep_from; });
                std::erase_if(ft.notes, [&](const NotePlace& nt) { return nt.para < keep_from; });
                std::erase_if(ft.carry_in, [&](const NoteRun& r) { return r.para < keep_from; });   // s117
                for (auto& r : ft.carry_in) r.para += delta;
                for (auto& l : ft.lines) l.para += delta;
                for (auto& nt : ft.notes) {
                    nt.para += delta;
                    nt.number += note_delta;   // (the stand-in's numbers, as they will be)
                }
                if (ft.from_p >= 0) ft.from_p += delta;
                out.frames[i] = std::move(ft);
            }
            for (const auto& ft : out.frames) out.lines += static_cast<int>(ft.lines.size());
            out.overset = prev->flow->overset;
            out.provisional = true;
            out.tail_from = static_cast<int>(fi);
            return out;
        }
        // s117: the frame's floor -- its height (or the one it's measured at), less the bottom inset.
        const auto fh_of = [&](const core::Frame& f) {
            const auto h = heights ? heights->find(chain[fi]) : decltype(heights->end()){};
            return ((heights && h != heights->end()) ? h->second : f.h) - std::max(0.0, f.inset.bottom);
        };
        // s043: a paragraph to start on a new (right-hand) page passes the
        // frames on the page it would follow on, and left-hand pages.
        if (c.start == 0 && c.p < static_cast<int>(st->paras.size()) && !last_page.empty()) {
            const auto start = core::doc::start_of(d, st->paras[static_cast<std::size_t>(c.p)]);   // s055: a break too
            if ((start == core::ParaStart::NextPage || start == core::ParaStart::NextRightPage) &&
                (fr.owner == last_page || (start == core::ParaStart::NextRightPage && d.side_of(fr.owner) == core::Side::Left))) {
                // s117: a note running on still takes this frame's foot (no text).
                if (!c.carry.empty()) {
                    ++out.filled;
                    out.frames[fi].lines.clear();
                    out.frames[fi].notes.clear();
                    NoteCount count{note_from};
                    const PageFoot pf = page_foot(d, chain[fi]);   // s167
                    double fl = fh_of(fr);
                    if (pf.limit >= 0) fl = std::min(fl, std::max(0.0, pf.limit));
                    fill_frame(d, *st, fr, core::wrap_rects(d, chain[fi]), c, fl, out.frames[fi],
                               restart != core::NoteRestart::Story ? &count : nullptr, true, pf.away ? &pf.at : nullptr);
                }
                continue;
            }
        }
        ++out.filled;
        const Cursor at = c;
        const auto wraps = core::wrap_rects(d, chain[fi]);
        // s048: a frame being measured for its auto-size gets room to spare (and no vertical justification).
        const auto hit = heights ? heights->find(chain[fi]) : decltype(heights->end()){};
        const bool measuring = heights && hit != heights->end();
        const double fh = measuring ? hit->second : fr.h;
        double floor = fh - std::max(0.0, fr.inset.bottom);   // s040: above the bottom inset
        // s167: a narrow frame's notes go to the page's foot; the other frames stop above them.
        const PageFoot pf = measuring ? PageFoot{} : page_foot(d, chain[fi]);
        if (pf.limit >= 0) floor = std::min(floor, std::max(std::max(0.0, fr.inset.top), pf.limit));
        const Away* away = pf.away ? &pf.at : nullptr;
        out.frames[fi].lines.clear();
        out.frames[fi].notes.clear();
        NoteCount count{note_from};
        NoteCount* nc = restart != core::NoteRestart::Story ? &count : nullptr;
        done = fill_frame(d, *st, fr, wraps, c, floor, out.frames[fi], nc, false, away);
        if (done && fr.balance && core::column_count(fr) > 1) {
            // The shortest column height that still holds the rest.
            double lo = std::max(0.0, fr.inset.top), hi = floor;
            for (int k = 0; k < 24 && hi - lo > 0.25; ++k) {
                const double mid = (lo + hi) / 2;
                Cursor t = at;
                FrameText tmp{chain[fi], {}};
                NoteCount tc{note_from};
                if (fill_frame(d, *st, fr, wraps, t, mid, tmp, nc ? &tc : nullptr, false, away)) hi = mid; else lo = mid;
            }
            c = at;
            out.frames[fi].lines.clear();
        out.frames[fi].notes.clear();
            count = NoteCount{note_from};
            fill_frame(d, *st, fr, wraps, c, hi, out.frames[fi], nc, false, away);
        }
        if (!measuring) vertical_justify(d, *st, fr, wraps, out.frames[fi]);   // s042
        if (!out.frames[fi].lines.empty() || !out.frames[fi].notes.empty()) last_page = fr.owner;   // s122: endnotes take the page too
        if (tick) (*tick)(static_cast<int>(fi + 1), static_cast<int>(chain.size()));   // s102
    }
    // Frames the story never came to (it ended before them): as a whole flow leaves them.
    if (resume)
        for (std::size_t i = 0; i < chain.size(); ++i)
            if (i >= first && out.frames[i].from_p < 0) out.frames[i].lines.clear();
    for (const auto& ft : out.frames) out.lines += static_cast<int>(ft.lines.size());
    out.overset = !done;
    return out;
}
}  // namespace

namespace {

// One placed line, as the drawing and the editing geometry both see it.
// s026: a tab's gap to be filled with its stop's leader (frame coords).
struct LeaderGap {
    double      x0 = 0, x1 = 0;
    std::string text;
};

struct Placed {
    const Line*      line;
    PangoLayout*     lay;      // the paragraph's layout in this frame
    PangoLayoutLine* pl;
    int              from;     // the byte the layout starts at in the paragraph
    double           x;        // the line's left (alignment), frame coords
    double           base;     // its baseline, frame coords
    std::vector<LeaderGap> leaders = {};
    const core::Style*     style = nullptr;
    std::string            marker = {};    // s045: a list's bullet / number, before this (first) line
    double                 marker_x = 0;   // its left, frame coords
};

// s026: the tabs on a placed line whose stop has a leader: their gaps.
std::vector<LeaderGap> leader_gaps(PangoLayout* lay, PangoLayoutLine* pl, const core::Style& st, double line_x) {
    std::vector<LeaderGap> out;
    if (st.tabs.empty()) return out;
    bool any = false;
    for (const auto& t : st.tabs) any = any || !t.leader.empty();
    if (!any) return out;
    const char* text = pango_layout_get_text(lay);
    for (int i = pl->start_index; i < pl->start_index + pl->length; ++i) {
        if (text[i] != '\t') continue;
        PangoRectangle pos;
        pango_layout_index_to_pos(lay, i, &pos);
        const double a = pos.x / static_cast<double>(PANGO_SCALE), b = (pos.x + pos.width) / static_cast<double>(PANGO_SCALE);
        // The stop this tab goes to: the first past where it starts.
        const core::TabStop* stop = nullptr;
        for (const auto& t : st.tabs) if (t.pos > a + 0.5 && (!stop || t.pos < stop->pos)) stop = &t;
        if (!stop || stop->leader.empty() || b - a < 1) continue;
        out.push_back({line_x + a, line_x + b, stop->leader});
    }
    return out;
}

// A leader's glyphs: the unit repeated on a grid (so dots line up from
// line to line), clear of the words either side. `f(line, x)` draws it.
template <class F>
void leader_lines(const core::Style& st, const LeaderGap& g, F&& f) {
    auto unit = make_layout(st, g.text.c_str(), static_cast<int>(g.text.size()), 1e6, false);
    PangoRectangle ext;
    pango_layout_get_extents(unit.get(), nullptr, &ext);
    const double w = ext.width / static_cast<double>(PANGO_SCALE);
    if (w <= 0.1) return;
    const double pad = std::max(2.0, w * 0.5);
    const double start = std::ceil((g.x0 + pad) / w) * w;
    const int n = static_cast<int>(std::floor((g.x1 - pad - start) / w));
    if (n <= 0) return;
    std::string rep;
    for (int k = 0; k < n; ++k) rep += g.text;
    auto lay = make_layout(st, rep.c_str(), static_cast<int>(rep.size()), 1e6, false);
    f(pango_layout_get_line_readonly(lay.get(), 0), start);
}

const FrameText* frame_text(const StoryFlow& flow, const std::string& frame) {
    for (const auto& f : flow.frames) if (f.frame == frame) return &f;
    return nullptr;
}

// Walk a frame's lines. Lines of one paragraph in a frame come from ONE
// layout started where this frame's part of the paragraph starts -- the same
// text at the same width breaks exactly as the flow measured it.
template <class F>
void walk_frame(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame, F&& f,
                const core::Story* text = nullptr) {
    const auto* st = text ? text : d.as<core::Story>(story);
    const auto* fr = d.as<core::Frame>(frame);
    const FrameText* ft = frame_text(flow, frame);
    if (!st || !fr || !ft || ft->lines.empty()) return;
    const StoryScope scope(d, st);   // s109
    std::size_t i = 0;
    while (i < ft->lines.size()) {
        const int p = ft->lines[i].para;
        const int col = ft->lines[i].col;
        std::size_t j = i + 1;
        // s018: a line set on its own (w > 0) is laid out alone, as the flow set it.
        if (ft->lines[i].w <= 0)
            while (j < ft->lines.size() && ft->lines[j].para == p && ft->lines[j].col == col && ft->lines[j].band == ft->lines[i].band &&
                   ft->lines[j].w <= 0) ++j;
        if (p >= static_cast<int>(st->paras.size())) break;
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)]);
        const int from = std::min(ft->lines[i].start, static_cast<int>(text.size()));
        const bool solo = ft->lines[i].w > 0;
        const bool continues = !solo && ft->lines[j - 1].end < static_cast<int>(text.size());
        const Indents ind = indents_of(d, style, *st, p);   // s026: as the flow set it
        const Geo g = geo_of(ind, line_width_of(*fr, ft->lines[i]), from == 0);   // s166: a spanning paragraph's own width
        // s029: a drop cap's line: its characters alone, at its size (the
        // lines beside it start past them: no first indent).
        const double cap = ft->lines[i].cap;
        core::Style cs = style;
        if (cap > 0) { cs.size = cap; cs.leading = 0; cs.tabs.clear(); }
        // s038: a hyphenated line: its words to the break, then the hyphen
        // (justified: it is not the paragraph's last line).
        const std::string hy = ft->lines[i].hyphen ? text.substr(static_cast<std::size_t>(from),
                                                                 static_cast<std::size_t>(ft->lines[i].end - from)) + "-"
                                                   : std::string{};
        auto lay = cap > 0 ? make_layout(cs, text.c_str() + from, ft->lines[i].end - from, 1e6, false)
                 : !hy.empty() ? make_layout(style, hy.c_str(), static_cast<int>(hy.size()), ft->lines[i].w, true,
                                             from == 0 ? std::max(0.0, ind.first) : 0)
                           : make_layout(style, text.c_str() + from, static_cast<int>(text.size()) - from,
                                         solo ? ft->lines[i].w : g.width, continues,
                                         solo ? (from == 0 ? std::max(0.0, ind.first) : 0) : g.indent);
        int first_num = -1;   // s111: the number this frame gave the first mark here (as the flow set its room)
        if (const auto mi = text.find(core::kMarkNote, static_cast<std::size_t>(from)); mi != std::string::npos) {
            const int mk = core::notes_in(text, 0, mi);
            for (const auto& np : ft->notes)
                if (np.para == p && np.k == mk) { first_num = np.number; break; }
        }
        const NoteFirst nf(first_num);
        apply_runs(lay.get(), d, st->paras[static_cast<std::size_t>(p)], from);
        PangoLayoutIter* it = pango_layout_get_iter(lay.get());
        for (std::size_t k = i; k < j; ++k) {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double bl = pango_layout_iter_get_baseline(it) / static_cast<double>(PANGO_SCALE);
            const double top = logical.y / static_cast<double>(PANGO_SCALE);
            const double h = logical.height / static_cast<double>(PANGO_SCALE);
            const double x = logical.x / static_cast<double>(PANGO_SCALE);
            const auto& ln = ft->lines[k];
            PangoLayoutLine* pll = pango_layout_iter_get_line_readonly(it);
            // s045: a list paragraph's first line carries its marker, hung left of
            // the text (not past the column's edge).
            std::string mk;
            double mx = 0;
            if (ln.start == 0 && ln.cap <= 0 && style.list.type != core::ListType::None) {
                mk = core::doc::list_marker(d, *st, p);
                mx = std::max(ln.x - std::max(0.0, style.list.hang), ln.x - std::max(0.0, ind.left));
            }
            f(Placed{&ln, lay.get(), pll, from, ln.x + x, ln.cap > 0 ? ln.base : ln.y + (ln.h - h) / 2 + (bl - top),
                     leader_gaps(lay.get(), pll, style, ln.x), &style, mk, mx});
            if (!pango_layout_iter_next_line(it)) break;
        }
        pango_layout_iter_free(it);
        i = j;
    }
}

// s109: a placed line's footnote numbers -- each where its mark is, raised.
template <class F>
void each_ref(const core::Doc& d, const core::Story& st, const FrameText* ft, const Placed& pl, F&& f) {
    const Line& ln = *pl.line;
    if (ln.cap > 0 || ln.para < 0 || ln.para >= static_cast<int>(st.paras.size())) return;
    const auto& t = st.paras[static_cast<std::size_t>(ln.para)].text;
    const PangoFontDescription* fd = pango_layout_get_font_description(pl.lay);
    const double size = fd ? pango_font_description_get_size(fd) / static_cast<double>(PANGO_SCALE) : 11;
    for (std::size_t i = t.find(core::kMarkNote, static_cast<std::size_t>(ln.start)); i != std::string::npos &&
         static_cast<int>(i) < ln.end; i = t.find(core::kMarkNote, i + 3)) {
        PangoRectangle r;
        pango_layout_index_to_pos(pl.lay, static_cast<int>(i) - pl.from, &r);
        const int k = core::notes_in(t, 0, i);
        int n = core::note_number(d, st, ln.para, k);
        if (ft)   // s111: as the flow numbered it (by page / section)
            for (const auto& np : ft->notes)
                if (np.para == ln.para && np.k == k) { n = np.number; break; }
        auto rl = ref_layout(fd, core::note_label(n, d.settings().note_style));
        f(rl.get(), ln.x + r.x / static_cast<double>(PANGO_SCALE), pl.base - size * kRefRise);
    }
}

// s109: a frame's notes, line by line: (line, x, baseline, the note).
template <class F>
void each_note_line(const core::Doc& d, const core::Story& st, const FrameText& ft, F&& f) {
    for (const auto& np : ft.notes) {
        if (np.para < 0 || np.para >= static_cast<int>(st.paras.size())) continue;
        auto lay = note_layout(d, st, np.para, np.k, np.number, np.w, np.from, np.to);   // s117: its piece
        PangoLayoutIter* it = pango_layout_get_iter(lay.get());
        do {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double base = np.y + pango_layout_iter_get_baseline(it) / static_cast<double>(PANGO_SCALE);
            f(pango_layout_iter_get_line_readonly(it), np.x + logical.x / static_cast<double>(PANGO_SCALE), base, np);
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
    }
}

int para_len(const core::Story& s, int p) {
    return p >= 0 && p < static_cast<int>(s.paras.size()) ? static_cast<int>(s.paras[static_cast<std::size_t>(p)].text.size()) : 0;
}

// Is `p` on this line? At a soft wrap the place belongs to the NEXT line
// (where typing there would appear) -- except at the paragraph's very end.
bool on_line(const core::Story& s, const Line& l, core::TextPos p) {
    if (l.para != p.para) return false;
    if (p.byte < l.start) return false;
    if (p.byte < l.end) return true;
    return p.byte == l.end && l.end == para_len(s, l.para);
}

// x of a byte on a placed line, frame coords.
double x_of(const Placed& pl, int byte) {
    PangoRectangle pos;
    pango_layout_index_to_pos(pl.lay, byte - pl.from, &pos);
    return pl.line->x + pos.x / static_cast<double>(PANGO_SCALE);
}

// s061: the right edge of what ends at `byte` on this line -- at a line's
// end, x_of(end) is the NEXT line's first character (back at the left), so
// a selection's middle lines came out as slivers. The trailing edge of the
// character before it instead.
double x_end_of(const Placed& pl, int byte) {
    if (byte <= pl.line->start || byte < pl.line->end) return x_of(pl, byte);
    const char* text = pango_layout_get_text(pl.lay);
    const char* at = text + (byte - pl.from);
    const char* prev = g_utf8_find_prev_char(text, at);
    if (!prev) return x_of(pl, byte);
    PangoRectangle pos;
    pango_layout_index_to_pos(pl.lay, static_cast<int>(prev - text), &pos);
    return pl.line->x + (pos.x + std::max(0, pos.width)) / static_cast<double>(PANGO_SCALE);
}

// The byte nearest x on a placed line.
int byte_at_x(const Placed& pl, double x) {
    int index = 0, trailing = 0;
    pango_layout_line_x_to_index(pl.pl, static_cast<int>(std::lround((x - pl.x) * PANGO_SCALE)), &index, &trailing);
    const char* text = pango_layout_get_text(pl.lay);
    const char* at = text + index;
    for (int t = 0; t < trailing && *at; ++t) at = g_utf8_next_char(at);
    int b = pl.from + static_cast<int>(at - text);
    // Never past the line's own end (a wrapped line's trailing space).
    return std::clamp(b, pl.line->start, pl.line->end);
}

}  // namespace

double line_width_of(const core::Frame& fr, const Line& l) {   // s166
    return l.cw > 0 ? l.cw : core::column_width(fr, l.col);
}

std::vector<RuleMark> column_rules(const core::Frame& f, const FrameText* ft) {
    std::vector<RuleMark> out;
    const int n = core::column_count(f);
    const auto& r = f.col_rule;
    if (f.kind != core::FrameKind::Text || n < 2 || r.weight <= 0) return out;
    const double y0 = std::max(0.0, f.inset.top) + std::max(0.0, r.inset);
    const double y1 = f.h - std::max(0.0, f.inset.bottom) - std::max(0.0, r.inset);
    if (y1 <= y0) return out;
    // s168: the stretches a spanning paragraph takes (its lines, with a
    // rule's inset round them) -- the rules leave them out.
    struct Gap { double a, b; };
    std::vector<Gap> gaps;
    if (ft)
        for (const auto& l : ft->lines) {
            if (l.cw <= 0) continue;
            const double a = l.y - std::max(0.0, r.inset), b = l.y + l.h + std::max(0.0, r.inset);
            if (!gaps.empty() && a <= gaps.back().b + 0.01) gaps.back().b = std::max(gaps.back().b, b);
            else gaps.push_back({a, b});
        }
    for (int c = 1; c < n; ++c) {
        const double x = (core::column_x(f, c - 1) + core::column_width(f, c - 1) + core::column_x(f, c)) / 2;
        double from = y0;
        for (const auto& g : gaps) {
            if (g.b <= from) continue;
            if (g.a > from && g.a < y1) out.push_back({{x - r.weight / 2, from, r.weight, std::min(g.a, y1) - from}, r.color, r.tint});
            from = std::max(from, g.b);
            if (from >= y1) break;
        }
        if (from < y1) out.push_back({{x - r.weight / 2, from, r.weight, y1 - from}, r.color, r.tint});
    }
    return out;
}

const char* break_name(int brk) {   // s098
    switch (brk) {
        case 1: return "Column break";
        case 2: return "Frame break";
        case 3: return "Page break";
        case 4: return "Right-page break";
        default: return "Break";
    }
}

std::vector<HiddenMark> hidden_marks(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                     const std::string& frame, const core::Story* text) {
    std::vector<HiddenMark> out;
    const auto* st = text ? text : d.as<core::Story>(story);
    if (!st) return out;
    const int paras = static_cast<int>(st->paras.size());
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        const auto& ln = *pl.line;
        if (ln.para < 0 || ln.para >= paras) return;
        const auto& para = st->paras[static_cast<std::size_t>(ln.para)];
        const auto& t = para.text;
        for (int i = ln.start; i < ln.end && i < static_cast<int>(t.size()); ++i) {
            const char c = t[static_cast<std::size_t>(i)];
            if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < static_cast<int>(t.size()) &&
                static_cast<unsigned char>(t[static_cast<std::size_t>(i + 1)]) == 0xAD) {   // s065
                out.push_back({HiddenMark::Kind::Hyphen, x_of(pl, i), x_of(pl, i), pl.base, ln.h, 0});
                continue;
            }
            if (c != ' ' && c != '\t') continue;
            out.push_back({c == ' ' ? HiddenMark::Kind::Space : HiddenMark::Kind::Tab, x_of(pl, i), x_end_of(pl, i + 1), pl.base, ln.h, 0});
        }
        if (ln.end < static_cast<int>(t.size())) return;   // not the paragraph's last line
        // Its end: the story's end, a break before the next, or a plain paragraph end.
        HiddenMark m{HiddenMark::Kind::Para, x_end_of(pl, static_cast<int>(t.size())), 0, pl.base, ln.h, 0};
        if (ln.para + 1 >= paras) m.kind = HiddenMark::Kind::End;
        else if (const auto b = st->paras[static_cast<std::size_t>(ln.para + 1)].brk; b != core::ParaStart::Anywhere) {
            m.kind = HiddenMark::Kind::Break;
            m.brk = static_cast<int>(b);
            if (const auto* fr = d.as<core::Frame>(frame)) {   // s098: the rule runs to the column's edge
                const int col = core::column_at(*fr, m.x0);
                m.edge = core::column_x(*fr, col) + core::column_width(*fr, col);
            }
        }
        m.x1 = m.x0;
        out.push_back(m);
    }, text);
    return out;
}

void draw_frame_text(cairo_t* cr, const core::Doc& d, const std::string& story, const StoryFlow& flow,
                     const std::string& frame, double ox, double oy, double ink, const core::Story* text) {
    cairo_save(cr);
    const auto* st = text ? text : d.as<core::Story>(story);
    // s044: paragraph rules, under the words; s055: column rules with them.
    auto marks = paragraph_rules(d, story, flow, frame, text);
    if (const auto* fr = d.as<core::Frame>(frame))
        for (auto& m : column_rules(*fr, frame_text(flow, frame))) marks.push_back(std::move(m));   // s168: not through a spanning paragraph
    for (const auto& m : marks) {
        if (const auto* sw = d.as<core::Swatch>(m.color)) {
            const auto c = core::tint_rgb(core::doc::screen_rgb(*sw), m.tint);
            cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
        } else {
            const double g = 1 - m.tint * (1 - ink);
            cairo_set_source_rgb(cr, g, g, g);
        }
        cairo_rectangle(cr, ox + m.r.x, oy + m.r.y, m.r.w, m.r.h);
        cairo_fill(cr);
    }
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        // The paragraph style's colour (s015), else the ink.
        const core::Swatch* sw = nullptr;
        double tint = 1;
        if (st && pl.line->para < static_cast<int>(st->paras.size()))
            if (const auto* ps = d.as<core::Style>(st->paras[static_cast<std::size_t>(pl.line->para)].style)) {
                sw = d.as<core::Swatch>(ps->color);
                tint = ps->color_tint;
            }
        if (sw) {
            const auto c = core::tint_rgb(core::doc::screen_rgb(*sw), tint);   // s021
            cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
        }
        else if (tint < 1) cairo_set_source_rgb(cr, 1 - tint * (1 - ink), 1 - tint * (1 - ink), 1 - tint * (1 - ink));   // black, tinted: grey
        else cairo_set_source_rgb(cr, ink, ink, ink > 0 ? ink + 0.02 : 0);
        cairo_move_to(cr, ox + pl.x, oy + pl.base);
        pango_cairo_show_layout_line(cr, pl.pl);
        for (const auto& g : pl.leaders)   // s026: dots to the tab stop
            leader_lines(*pl.style, g, [&](PangoLayoutLine* l, double x) {
                cairo_move_to(cr, ox + x, oy + pl.base);
                pango_cairo_show_layout_line(cr, l);
            });
        if (!pl.marker.empty()) {   // s045: the bullet / number, in the paragraph's colour
            auto ml = make_layout(*pl.style, pl.marker.c_str(), static_cast<int>(pl.marker.size()), 1e6, false);
            cairo_move_to(cr, ox + pl.marker_x, oy + pl.base);
            pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(ml.get(), 0));
        }
        if (st)   // s109: footnote numbers at their marks
            each_ref(d, *st, frame_text(flow, frame), pl, [&](PangoLayout* rl, double x, double base) {
                cairo_move_to(cr, ox + x, oy + base);
                pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(rl, 0));
            });
    }, text);
    // s109: the footnotes at the columns' feet, in the ink.
    if (const FrameText* ft = frame_text(flow, frame); st && ft && !ft->notes.empty()) {
        cairo_set_source_rgb(cr, ink, ink, ink > 0 ? ink + 0.02 : 0);
        each_note_line(d, *st, *ft, [&](PangoLayoutLine* l, double x, double base, const NotePlace&) {
            cairo_move_to(cr, ox + x, oy + base);
            pango_cairo_show_layout_line(cr, l);
        });
    }
    cairo_restore(cr);
}

std::vector<RuleMark> paragraph_rules(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                      const std::string& frame, const core::Story* text) {
    std::vector<RuleMark> out;
    const auto* st = text ? text : d.as<core::Story>(story);
    const auto* fr = d.as<core::Frame>(frame);
    if (!st || !fr) return out;
    int after_cap = -1;   // a drop cap's paragraph: its first line is the one after the cap's
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        const auto& ln = *pl.line;
        if (ln.para >= static_cast<int>(st->paras.size())) return;
        const auto& para = st->paras[static_cast<std::size_t>(ln.para)];
        const auto style = style_of(d, para);
        if (ln.cap > 0) { after_cap = ln.para; return; }
        const bool first = (ln.start == 0) || after_cap == ln.para;
        after_cap = -1;
        const bool last = ln.end >= static_cast<int>(para.text.size());
        struct Span { double x0, x1; };   // (not std::pair: GCC's psabi note)
        const auto span = [&](const core::ParaRule& r) {
            if (r.text_width) {
                PangoRectangle e;
                pango_layout_line_get_extents(pl.pl, nullptr, &e);
                return Span{pl.x, pl.x + std::max(1.0, e.width / static_cast<double>(PANGO_SCALE))};
            }
            const Indents ind = indents_of(d, style, *st, ln.para);
            const double cx = ln.cw > 0 ? ln.x : core::column_x(*fr, ln.col), cw = line_width_of(*fr, ln);   // s166
            return Span{cx + std::max(0.0, ind.left), cx + cw - std::max(0.0, ind.right)};
        };
        if (first && style.rule_above.weight > 0) {
            const auto [x0, x1] = span(style.rule_above);
            const double b = pl.base - style.rule_above.offset;
            out.push_back({{x0, b - style.rule_above.weight, x1 - x0, style.rule_above.weight}, style.rule_above.color, style.rule_above.tint});
        }
        if (last && style.rule_below.weight > 0) {
            const auto [x0, x1] = span(style.rule_below);
            const double t = pl.base + style.rule_below.offset;
            out.push_back({{x0, t, x1 - x0, style.rule_below.weight}, style.rule_below.color, style.rule_below.tint});
        }
    }, text);
    // s109: a short rule above each column's footnotes.
    if (const FrameText* ft = frame_text(flow, frame); st && ft) {
        std::map<int, const NotePlace*> first;
        for (const auto& np : ft->notes)
            if (!np.end && (!first.count(np.col) || np.y < first[np.col]->y)) first[np.col] = &np;   // s120: not over endnotes
        for (const auto& [col, np] : first) {
            if (np->para < 0 || np->para >= static_cast<int>(st->paras.size())) continue;
            const double gap = note_gap(d, *st, np->para);
            // s117: a note run on from before: the rule across the column.
            // s118: else the document's length (none at 0); its weight.
            const auto& ds = d.settings();
            const double len = np->cont ? np->w : np->w * std::clamp(ds.note_rule, 0, 100) / 100.0;
            const double rw = std::clamp(ds.note_rule_w, 0.1, 6.0);
            if (len > 0) out.push_back({{np->x, np->y - gap / 2 - rw / 2, len, rw}, std::string{}, 1});
        }
    }
    return out;
}

void visit_lines(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame,
                 const std::function<void(const LineVisit&)>& f, const core::Story* text) {
    const auto* st = text ? text : d.as<core::Story>(story);
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        f(LineVisit{pl.pl, pl.x, pl.base, pl.line->para, pl.from});
        for (const auto& g : pl.leaders)   // s026: each leader as a line of its own (from = -1)
            leader_lines(*pl.style, g, [&](PangoLayoutLine* l, double x) {
                f(LineVisit{l, x, pl.base, pl.line->para, -1});
            });
        if (!pl.marker.empty()) {   // s045: a list's marker (from = -2)
            auto ml = make_layout(*pl.style, pl.marker.c_str(), static_cast<int>(pl.marker.size()), 1e6, false);
            f(LineVisit{pango_layout_get_line_readonly(ml.get(), 0), pl.marker_x, pl.base, pl.line->para, -2});
        }
        if (st)   // s109: a footnote's number at its mark (from = -3)
            each_ref(d, *st, frame_text(flow, frame), pl, [&](PangoLayout* rl, double x, double base) {
                f(LineVisit{pango_layout_get_line_readonly(rl, 0), x, base, pl.line->para, -3});
            });
    }, text);
    // s109: the footnotes' lines (from = -4).
    if (const FrameText* ft = frame_text(flow, frame); st && ft)
        each_note_line(d, *st, *ft, [&](PangoLayoutLine* l, double x, double base, const NotePlace& np) {
            f(LineVisit{l, x, base, np.para, -4});
        });
}

// ── s023: markers filled in ─────────────────────────────────────────────────
std::string marker_value(const core::Doc& d, int k, const std::string& page, const std::string& frame) {
    if (!d.as<core::Page>(page)) return {};
    switch (k) {
    case 0: return d.folio(page);
    case 1: {
        const auto* sec = d.as<core::Section>(d.section_of(page));
        return sec ? sec->name : std::string{};
    }
    case 2: case 3: {
        // A page frame: the page of the next / previous frame in its thread.
        // A master's (or none): the next / previous page of the document.
        const auto* f = d.as<core::Frame>(frame);
        if (f && d.as<core::Page>(f->owner) && !f->story.empty()) {
            const auto chain = thread_frames(d, f->story);
            const auto it = std::find(chain.begin(), chain.end(), frame);
            if (it == chain.end()) return {};
            if (k == 2 && it + 1 != chain.end()) return marker_value(d, 0, d.as<core::Frame>(*(it + 1))->owner, {});
            if (k == 3 && it != chain.begin()) return marker_value(d, 0, d.as<core::Frame>(*(it - 1))->owner, {});
            return {};
        }
        const auto pages = d.pages();
        const auto it = std::find(pages.begin(), pages.end(), page);
        if (it == pages.end()) return {};
        if (k == 2 && it + 1 != pages.end()) return d.folio(*(it + 1));
        if (k == 3 && it != pages.begin()) return d.folio(*(it - 1));
        return {};
    }
    }
    return {};
}

// s128: what a cross-reference shows -- where its anchor is set (the target
// story's flow, from `flows` -- one each, made as needed): the page's number,
// its paragraph's words (to the first tab, 60 bytes), or both. "??" when the
// anchor is gone or not set.
namespace {
std::string ref_value_by(const core::Doc& d, const core::XRef& r, const std::function<const StoryFlow&(const std::string&)>& flow_of) {
    const auto at = core::doc::find_anchor(d, r.target);
    if (at.story.empty()) return "??";
    const auto& st = *d.as<core::Story>(at.story);
    const auto& text = st.paras[static_cast<std::size_t>(at.at.para)].text;
    const auto flow = [&]() -> const StoryFlow& { return flow_of(at.story); };
    // s129: the pattern's pieces, each worked out only when asked for.
    const auto words = [&] {
        std::string w;
        for (std::size_t i = 0; i < text.size() && w.size() < 60 && text[i] != '\t';) {
            if (static_cast<unsigned char>(text[i]) == 0xEE && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80) { i += 3; continue; }
            w += text[i++];
        }
        while (!w.empty() && (static_cast<unsigned char>(w.back()) & 0xC0) == 0x80) w.pop_back();
        while (!w.empty() && w.back() == ' ') w.pop_back();
        return w.empty() ? std::string("??") : w;
    };
    const auto page = [&] {
        for (const auto& ft : flow().frames)
            for (const auto& l : ft.lines)
                if (l.para == at.at.para && at.at.byte >= l.start && at.at.byte <= std::max(l.end, l.start + 1))
                    if (const auto* f = d.as<core::Frame>(ft.frame); f && d.as<core::Page>(f->owner)) return d.folio(f->owner);
        return std::string("??");
    };
    const auto note = [&] {   // the note whose mark follows the anchor
        const auto b = static_cast<std::size_t>(at.at.byte) + 3;
        if (!core::note_at(text, b)) return std::string("??");
        const int k = core::notes_in(text, 0, b);
        int n = laid_note_number(flow(), at.at.para, k);
        if (n <= 0) n = core::note_number(d, st, at.at.para, k);
        return core::note_label(n, d.settings().note_style);
    };
    const auto pat = core::ref_pattern(r);
    std::string out;
    for (const char c : pat) {
        if (c == '#') out += page();
        else if (c == '%') out += words();
        else if (c == '*') out += note();
        else if (c == '^') {   // s141: the source's number; s143: every source's, in order, runs of 3+ joined
            std::vector<int> ns;
            bool gone = false;
            const auto add = [&](const std::string& id) {
                const auto a = core::doc::find_anchor(d, id);
                const int n = a.story.empty() ? 0 : core::source_number(d, *d.as<core::Story>(a.story), a.at.para);
                if (n > 0) ns.push_back(n); else gone = true;
            };
            add(r.target);
            for (const auto& id : r.more) add(id);
            std::sort(ns.begin(), ns.end());
            ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
            std::string t;
            for (std::size_t a = 0; a < ns.size();) {
                std::size_t b = a;
                while (b + 1 < ns.size() && ns[b + 1] == ns[b] + 1) ++b;
                if (!t.empty()) t += ", ";
                if (b - a >= 2) t += std::to_string(ns[a]) + "\u2013" + std::to_string(ns[b]);
                else for (std::size_t q = a; q <= b; ++q) t += (q > a ? ", " : "") + std::to_string(ns[q]);
                a = b + 1;
            }
            if (gone) t += t.empty() ? "??" : ", ??";
            out += t;
        }
        else out += c;
    }
    return out;
}
}  // namespace

std::string ref_value(const core::Doc& d, const core::XRef& r, std::map<std::string, StoryFlow>& flows) {   // s128
    return ref_value_by(d, r, [&](const std::string& story) -> const StoryFlow& {
        auto it = flows.find(story);
        if (it == flows.end()) it = flows.emplace(story, flow_story(d, story)).first;
        return it->second;
    });
}

std::string ref_value(const core::Doc& d, const core::XRef& r, FlowCache& flows) {   // s130
    return ref_value_by(d, r, [&](const std::string& story) -> const StoryFlow& { return flows.get(d, story); });
}

std::vector<SourceRow> source_list(const core::Doc& d, FlowCache& flows) {   // s144
    std::vector<SourceRow> out;
    // Where each anchor is cited: (page index, folio) per citation.
    std::map<std::string, std::vector<std::pair<int, std::string>>> cited;
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        if (std::none_of(st.paras.begin(), st.paras.end(), [](const core::Para& p) { return !p.refs.empty(); })) continue;
        const auto& flow = flows.get(d, sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            int k = 0;
            for (auto i = para.text.find(core::kMarkRef); i != std::string::npos && k < static_cast<int>(para.refs.size());
                 i = para.text.find(core::kMarkRef, i + 3), ++k) {
                const auto& r = para.refs[static_cast<std::size_t>(k)];
                if (r.show != core::RefShow::Cite) continue;
                int pi = -1;
                std::string folio;
                const auto c = caret_at(d, sid, flow, {p, static_cast<int>(i)});
                if (const auto* f = c.ok ? d.as<core::Frame>(c.frame) : nullptr; f && d.as<core::Page>(f->owner)) {
                    pi = d.page_index(f->owner);
                    folio = d.folio(f->owner);
                }
                std::vector<std::string> ids{r.target};
                ids.insert(ids.end(), r.more.begin(), r.more.end());
                for (const auto& id : ids) if (!id.empty()) cited[id].push_back({pi, folio});
            }
        }
    }
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const int n = core::source_number(d, st, p);
            if (n == 0) continue;
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            SourceRow row{sid, p, n};
            for (std::size_t i = 0; i < para.text.size();) {
                if (static_cast<unsigned char>(para.text[i]) == 0xEE && i + 2 < para.text.size() &&
                    static_cast<unsigned char>(para.text[i + 1]) == 0x80) { i += 3; continue; }
                row.words += para.text[i] == '\t' ? ' ' : para.text[i];
                ++i;
            }
            std::vector<std::pair<int, std::string>> at;
            for (const auto& id : para.anchors)
                if (const auto it = cited.find(id); !id.empty() && it != cited.end()) at.insert(at.end(), it->second.begin(), it->second.end());
            row.cites = static_cast<int>(at.size());
            std::sort(at.begin(), at.end());
            for (const auto& [pi, folio] : at)
                if (pi >= 0 && std::find(row.pages.begin(), row.pages.end(), folio) == row.pages.end()) row.pages.push_back(folio);
            out.push_back(std::move(row));
        }
    }
    return out;
}

std::vector<XRefRow> xref_list(const core::Doc& d, FlowCache& flows) {   // s133
    std::vector<XRefRow> out;
    // UTF-8: back / on to a character's start.
    const auto lead = [](const std::string& t, std::size_t i) {
        while (i > 0 && i < t.size() && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) --i;
        return i;
    };
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& st = *d.as<core::Story>(sid);
        if (std::none_of(st.paras.begin(), st.paras.end(), [](const core::Para& p) { return !p.refs.empty(); })) continue;
        const auto& rs = flows.resolved(d, sid, {});
        const auto& flow = flows.get(d, sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            int k = 0;
            for (std::size_t i = para.text.find(core::kMarkRef); i != std::string::npos; i = para.text.find(core::kMarkRef, i + 3), ++k) {
                if (k >= static_cast<int>(para.refs.size())) break;
                const auto& r = para.refs[static_cast<std::size_t>(k)];
                XRefRow row{sid, p, k, static_cast<int>(i)};
                for (const auto& sp : rs.refs) {
                    if (sp.para != p || sp.k != k) continue;
                    const auto& t = rs.text.paras[static_cast<std::size_t>(p)].text;
                    row.shows = t.substr(static_cast<std::size_t>(sp.from), static_cast<std::size_t>(sp.to - sp.from));
                    const auto a = lead(t, sp.from > 40 ? static_cast<std::size_t>(sp.from - 40) : 0);
                    const auto b = lead(t, std::min(t.size(), static_cast<std::size_t>(sp.to) + 16));
                    row.context = (a > 0 ? "\u2026" : "") + t.substr(a, b - a) + (b < t.size() ? "\u2026" : "");
                    std::string clean;   // marks out (a note's number shows as itself on the page, not here)
                    for (std::size_t q = 0; q < row.context.size();) {
                        if (static_cast<unsigned char>(row.context[q]) == 0xEE && q + 2 < row.context.size() &&
                            static_cast<unsigned char>(row.context[q + 1]) == 0x80) { q += 3; continue; }
                        clean += row.context[q] == '\t' ? ' ' : row.context[q];
                        ++q;
                    }
                    row.context = std::move(clean);
                }
                const auto c = caret_at(d, sid, flow, {p, static_cast<int>(i)});
                if (const auto* f = c.ok ? d.as<core::Frame>(c.frame) : nullptr; f && d.as<core::Page>(f->owner)) {
                    row.page = d.folio(f->owner);
                    row.page_id = f->owner;
                }
                row.where = ref_where(d, r, flows, sid);
                row.broken = core::doc::find_anchor(d, r.target).story.empty() || row.shows.find("??") != std::string::npos;
                out.push_back(std::move(row));
            }
        }
    }
    return out;
}

std::string ref_where(const core::Doc& d, const core::XRef& r, FlowCache& flows, const std::string& from_story) {   // s130
    const auto at = core::doc::find_anchor(d, r.target);
    if (at.story.empty()) return "Cross-reference: its place is gone \u2014 it shows question marks";
    const auto& text = d.as<core::Story>(at.story)->paras[static_cast<std::size_t>(at.at.para)].text;
    const bool note = core::note_at(text, static_cast<std::size_t>(at.at.byte) + 3);
    std::string w = ref_value(d, core::XRef{r.target, core::RefShow::Custom, note ? "note *, page #" : "\u201c%\u201d, page #"}, flows);
    if (const auto* st = d.as<core::Story>(at.story); st && at.story != from_story) w += " (" + st->name + ")";
    return "Cross-reference: points at " + w;
}

Resolved resolve_story(const core::Doc& d, const std::string& story, const std::string& on_page) {
    Resolved r;
    const auto* st = d.as<core::Story>(story);
    if (!st || !core::has_markers(*st)) return r;
    r.any = true;
    r.text = *st;
    // Where each marker is set (page frames): from the plain flow.
    const StoryFlow plain = on_page.empty() ? flow_story(d, story) : StoryFlow{};
    std::map<std::string, StoryFlow> targets;   // s128: the stories references point into
    if (on_page.empty()) targets.emplace(story, plain);
    const auto frame_of = [&](int para, int byte) -> std::string {
        for (const auto& ft : plain.frames)
            for (const auto& l : ft.lines)
                if (l.para == para && byte >= l.start && byte < std::max(l.end, l.start + 1)) return ft.frame;
        return {};
    };
    for (int p = 0; p < static_cast<int>(r.text.paras.size()); ++p) {
        auto& para = r.text.paras[static_cast<std::size_t>(p)];
        const std::string src = st->paras[static_cast<std::size_t>(p)].text;   // positions as the plain flow knows them
        std::string out;
        std::vector<std::pair<int, int>> shifts;   // (at byte in src, delta)
        for (std::size_t i = 0; i < src.size();) {
            const int k = core::marker_at(src, i);
            if (k < 0) { out += src[i++]; continue; }
            std::string page = on_page, frame;
            if (page.empty()) {
                frame = frame_of(p, static_cast<int>(i));
                if (const auto* f = d.as<core::Frame>(frame)) page = f->owner;
            }
            std::string v;
            if (k == 5) {   // s128: a cross-reference -- where its anchor is
                const auto& refs = st->paras[static_cast<std::size_t>(p)].refs;
                const int idx = core::marks_in(src, core::kMarkRef, 0, i);
                v = idx < static_cast<int>(refs.size()) ? ref_value(d, refs[static_cast<std::size_t>(idx)], targets) : std::string("??");
                r.refs.push_back({p, idx, static_cast<int>(out.size()), static_cast<int>(out.size() + v.size())});   // s131
            } else if (k == 6 || k == 7) {   // s134: an index entry -- nothing to see either
                v.clear();   // an anchor: nothing to see
            } else {
                v = d.as<core::Page>(page) ? marker_value(d, k, page, frame) : src.substr(i, 3);
            }
            shifts.emplace_back(static_cast<int>(i), static_cast<int>(v.size()) - 3);
            out += v;
            i += 3;
        }
        if (shifts.empty()) continue;
        // Character styles keep covering the same characters.
        const auto moved = [&](int b) {
            int delta = 0;
            for (const auto& [at, dl] : shifts) if (at < b) delta += dl;
            return b + delta;
        };
        for (auto& run : para.runs) { run.start = moved(run.start); run.end = moved(run.end); }
        std::erase_if(para.runs, [](const core::Run& x) { return x.end <= x.start; });
        para.text = std::move(out);
    }
    r.flow = flow_text(d, story, r.text);
    return r;
}

Caret caret_at(const core::Doc& d, const std::string& story, const StoryFlow& flow, core::TextPos p) {
    Caret c;
    const auto* st = d.as<core::Story>(story);
    if (!st) return c;
    for (const auto& ft : flow.frames) {
        // s101: lay out only the frame that holds the place -- walking every
        // frame before it cost ~0.6 s a call in a 440-page story.
        if (std::none_of(ft.lines.begin(), ft.lines.end(), [&](const Line& l) { return on_line(*st, l, p); })) continue;
        walk_frame(d, story, flow, ft.frame, [&](const Placed& pl) {
            if (c.ok || !on_line(*st, *pl.line, p)) return;
            c = {true, ft.frame, x_of(pl, p.byte), pl.line->y, pl.line->h};
        });
        if (c.ok) break;
    }
    return c;
}

Caret caret_line(const core::Doc& d, const std::string& story, const StoryFlow& flow, core::TextPos p) {   // s105
    Caret c;
    const auto* st = d.as<core::Story>(story);
    if (!st) return c;
    for (const auto& ft : flow.frames)
        for (const auto& l : ft.lines)
            if (on_line(*st, l, p)) return {true, ft.frame, 0, l.y, l.h};
    return c;
}

int laid_note_number(const StoryFlow& flow, int p, int k) {   // s119
    for (const auto& ft : flow.frames)
        for (const auto& n : ft.notes)
            if (n.para == p && n.k == k && !n.cont) return n.number;
    return 0;
}

const NotePlace* note_hit(const StoryFlow& flow, const std::string& frame, double x, double y) {   // s114
    const FrameText* ft = frame_text(flow, frame);
    if (!ft) return nullptr;
    for (const auto& n : ft->notes)
        if (x >= n.x && x <= n.x + n.w && y >= n.y && y <= n.y + n.h) return &n;
    return nullptr;
}

core::TextPos hit_text(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                       const std::string& frame, double x, double y) {
    const FrameText* ft = frame_text(flow, frame);
    if (!ft || ft->lines.empty()) {
        // An empty frame of the story: the end of the text before it, else the start.
        const auto* st = d.as<core::Story>(story);
        if (!st) return {};
        for (auto it = flow.frames.rbegin(); it != flow.frames.rend(); ++it)
            if (!it->lines.empty()) return {it->lines.back().para, it->lines.back().end};
        return {};
    }
    // The column under x (s012: the nearest that holds text), then the line
    // under y in it (above the first: the first; below the last: the last).
    const auto* fr = d.as<core::Frame>(frame);
    int col = fr ? core::column_at(*fr, x) : 0;
    while (col > 0 && std::none_of(ft->lines.begin(), ft->lines.end(), [&](const Line& l) { return l.col == col; })) --col;
    const Line* want = nullptr;
    // s166: in bands -- the band under y first (a spanning paragraph is all
    // the columns'), then its column under x.
    int band = 0;
    if (fr && std::any_of(ft->lines.begin(), ft->lines.end(), [](const Line& l) { return l.band > 0; })) {
        for (const auto& l : ft->lines) if (y >= l.y) band = l.band;
        const bool across = std::any_of(ft->lines.begin(), ft->lines.end(), [&](const Line& l) { return l.band == band && l.cw > 0; });
        if (across) col = 0;
        else while (col > 0 && std::none_of(ft->lines.begin(), ft->lines.end(), [&](const Line& l) { return l.band == band && l.col == col; })) --col;
    }
    for (const auto& l : ft->lines)
        if (l.band == band && l.col == col && (!want || y >= l.y)) want = &l;
    if (!want) want = &ft->lines.front();
    // s018: lines side by side (a wrap between them): the rightmost that
    // starts left of x, else the leftmost.
    if (want->w > 0) {
        const double row = want->y;
        const Line* left = nullptr;
        const Line* best = nullptr;
        for (const auto& l : ft->lines) {
            if (l.col != col || l.band != band || l.y != row || l.w <= 0) continue;
            if (!left || l.x < left->x) left = &l;
            if (l.x <= x && (!best || l.x > best->x)) best = &l;
        }
        want = best ? best : left ? left : want;
    }
    core::TextPos out{want->para, want->start};
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        if (pl.line == want) out = {want->para, byte_at_x(pl, x)};
    });
    return out;
}

core::TextPos line_move(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                        core::TextPos p, int dir, double goal_x) {
    const auto* st = d.as<core::Story>(story);
    if (!st) return p;
    // Every line of the thread, in order.
    std::vector<std::pair<const FrameText*, const Line*>> all;
    for (const auto& ft : flow.frames) for (const auto& l : ft.lines) all.push_back({&ft, &l});
    int cur = -1;
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
        if (on_line(*st, *all[static_cast<std::size_t>(i)].second, p)) { cur = i; break; }
    if (cur < 0) return p;
    // s018: a ROW is the lines side by side (same frame, column and y); move
    // to the next row, then to the line in it under goal_x.
    const auto same_row = [&](int a, int b) {
        const auto& [fa, la] = all[static_cast<std::size_t>(a)];
        const auto& [fb, lb] = all[static_cast<std::size_t>(b)];
        return fa == fb && la->col == lb->col && la->y == lb->y;
    };
    int to = cur;
    while (to >= 0 && to < static_cast<int>(all.size()) && same_row(to, cur)) to += dir;
    if (to < 0) return {0, 0};                                                  // above the first line: the start
    if (to >= static_cast<int>(all.size())) return {all.back().second->para, all.back().second->end};   // the end
    {
        int a = to, b = to;
        while (a > 0 && same_row(a - 1, to)) --a;
        while (b + 1 < static_cast<int>(all.size()) && same_row(b + 1, to)) ++b;
        int pick = a;
        for (int k = a; k <= b; ++k)
            if (all[static_cast<std::size_t>(k)].second->x <= goal_x) pick = k;
        to = pick;
    }
    const auto [ft, line] = all[static_cast<std::size_t>(to)];
    core::TextPos out{line->para, line->start};
    walk_frame(d, story, flow, ft->frame, [&](const Placed& pl) {
        if (pl.line == line) out = {line->para, byte_at_x(pl, goal_x)};
    });
    return out;
}

core::TextPos line_edge(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                        core::TextPos p, bool end) {
    const auto* st = d.as<core::Story>(story);
    if (!st) return p;
    for (const auto& ft : flow.frames)
        for (const auto& l : ft.lines)
            if (on_line(*st, l, p)) {
                if (!end) return {l.para, l.start};
                // The end of a wrapped line is before its trailing space.
                int e = l.end;
                const auto& t = st->paras[static_cast<std::size_t>(l.para)].text;
                if (e < static_cast<int>(t.size()))
                    while (e > l.start && t[static_cast<std::size_t>(e - 1)] == ' ') --e;
                return {l.para, e};
            }
    return p;
}

std::vector<core::Rect> selection_rects(const core::Doc& d, const std::string& story, const StoryFlow& flow,
                                        const std::string& frame, core::TextPos a, core::TextPos b, const core::Story* text) {
    std::vector<core::Rect> out;
    if (b < a) std::swap(a, b);
    if (a == b) return out;
    const auto* st = text ? text : d.as<core::Story>(story);
    if (!st) return out;
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        const Line& l = *pl.line;
        if (l.para < a.para || l.para > b.para) return;
        const int s = l.para == a.para ? std::max(a.byte, l.start) : l.start;
        const int e = l.para == b.para ? std::min(b.byte, l.end) : l.end;
        const bool para_end_selected = l.end == para_len(*st, l.para) && l.para < b.para;
        if (s > e || (s == e && !para_end_selected)) return;
        if (l.para == a.para && a.byte > l.end) return;
        double x0 = x_of(pl, s), x1 = x_end_of(pl, e);
        if (x1 < x0) std::swap(x0, x1);
        if (para_end_selected) x1 += 4;   // the paragraph break, shown as a sliver
        out.push_back({x0, l.y, x1 - x0, l.h});
    }, text);
    return out;
}

const std::vector<std::string>& font_families() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out{"Serif", "Sans", "Monospace"};   // fontconfig's generics first
        PangoFontFamily** fams = nullptr;
        int n = 0;
        pango_font_map_list_families(pango_cairo_font_map_get_default(), &fams, &n);
        std::vector<std::string> named;
        for (int i = 0; i < n; ++i) named.emplace_back(pango_font_family_get_name(fams[i]));
        g_free(fams);
        std::sort(named.begin(), named.end(), [](const std::string& a, const std::string& b) {
            return g_ascii_strcasecmp(a.c_str(), b.c_str()) < 0;
        });
        named.erase(std::unique(named.begin(), named.end()), named.end());
        for (auto& f : named)
            if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(std::move(f));
        return out;
    }();
    return list;
}

void FlowCache::check(const core::Doc& d) {
    if (d.revision() == m_rev) return;
    // s105: when only stories' text changed since, the other stories' flows
    // still hold, and an edited one is re-flowed from its last flow.
    std::vector<core::Doc::Id> ids;
    bool text_only = m_rev != ~0ull && d.changes_since(m_rev, ids);
    for (std::size_t i = 0; text_only && i < ids.size(); ++i) text_only = d.as<core::Story>(ids[i]) != nullptr;
    if (!text_only) {
        m_flows.clear();
        m_prev.clear();
    } else {
        // s167: a story whose notes sit at a page's foot -- the other stories on
        // those pages stop above them: flowed again, whole.
        std::vector<core::Doc::Id> neighbours;
        for (const auto& id : ids)
            for (const auto& nb : foot_neighbours(d, id))
                if (std::find(ids.begin(), ids.end(), nb) == ids.end()) neighbours.push_back(nb);
        for (const auto& nb : neighbours) { m_flows.erase(nb); m_prev.erase(nb); }
        for (const auto& id : ids)
            if (auto it = m_flows.find(id); it != m_flows.end()) {
                // (an older prev of it is as good: either is a whole flow of its text.)
                // s106: a provisional flow isn't -- its basis is.
                if (it->second.flow.provisional && it->second.basis && it->second.basis_text)
                    m_prev[id] = Entry{*it->second.basis, *it->second.basis_text, nullptr, nullptr};
                else
                    m_prev[id] = std::move(it->second);
                m_flows.erase(it);
            }
    }
    m_resolved.clear();
    m_rev = d.revision();
}

const Resolved& FlowCache::resolved(const core::Doc& d, const std::string& story, const std::string& on_page) {
    check(d);
    const std::string key = story + "|" + on_page;
    auto it = m_resolved.find(key);
    if (it == m_resolved.end()) it = m_resolved.emplace(key, resolve_story(d, story, on_page)).first;
    return it->second;
}

void FlowCache::put(const core::Doc& d, const std::string& story, StoryFlow f) {   // s102
    check(d);
    const auto* st = d.as<core::Story>(story);
    m_flows[story] = Entry{std::move(f), st ? *st : core::Story{}};
    m_prev.erase(story);
}

std::vector<FlowCache::Unfinished> FlowCache::unfinished() const {   // s106
    std::vector<Unfinished> out;
    for (const auto& [id, e] : m_flows)
        if (e.flow.provisional && e.basis && e.basis_text) out.push_back({id, *e.basis, *e.basis_text});
    return out;
}

bool FlowCache::any_unfinished() const {
    for (const auto& [id, e] : m_flows)
        if (e.flow.provisional) return true;
    return false;
}

void FlowCache::complete(const core::Doc& d, const std::string& story, std::uint64_t rev, StoryFlow flow,
                         core::Story text) {   // s106
    if (flow.provisional) return;
    if (rev == d.revision() && m_rev == rev) {
        m_flows[story] = Entry{std::move(flow), std::move(text), nullptr, nullptr};
        m_prev.erase(story);
        m_resolved.clear();
        return;
    }
    // The document moved on: if only text changed since `rev`, the whole
    // flow of `text` is a better basis than the one the next re-flow has.
    std::vector<core::Doc::Id> ids;
    if (!d.changes_since(rev, ids)) return;
    for (const auto& id : ids)
        if (!d.as<core::Story>(id)) return;
    check(d);
    auto basis = std::make_shared<const StoryFlow>(std::move(flow));
    auto btext = std::make_shared<const core::Story>(std::move(text));
    if (auto it = m_flows.find(story); it != m_flows.end()) {
        if (it->second.flow.provisional) {   // re-flow the current text from it now, next get()
            m_prev[story] = Entry{*basis, *btext, nullptr, nullptr};
            m_flows.erase(it);
        }
        return;   // a whole flow already: nothing better to do
    }
    m_prev[story] = Entry{*basis, *btext, nullptr, nullptr};
}

void FlowCache::finish_now(const core::Doc& d) {   // s106
    check(d);
    for (auto& u : unfinished()) {
        const auto* st = d.as<core::Story>(u.story);
        if (!st) continue;
        complete(d, u.story, d.revision(), reflow_story(d, u.story, u.basis_text, u.basis), *st);
    }
}

void flow_all(const core::Doc& d, FlowCache& cache, const std::function<void(int, int)>& tick) {   // s102
    const auto stories = d.ids_of<core::Story>();
    std::vector<std::size_t> sizes;
    int total = 0;
    for (const auto& s : stories) {
        sizes.push_back(thread_frames(d, s).size());
        total += static_cast<int>(sizes.back());
    }
    int before = 0;
    for (std::size_t i = 0; i < stories.size(); ++i) {
        const FlowTick t = [&](int done, int) { if (tick) tick(before + done, total); };
        cache.put(d, stories[i], flow_story(d, stories[i], t));
        before += static_cast<int>(sizes[i]);
    }
}

const StoryFlow& FlowCache::get(const core::Doc& d, const std::string& story) {
    check(d);
    auto it = m_flows.find(story);
    if (it != m_flows.end()) {
        m_how = How::Kept;
        return it->second.flow;
    }
    const auto* st = d.as<core::Story>(story);
    Entry e{{}, st ? *st : core::Story{}};
    if (auto pv = m_prev.find(story); pv != m_prev.end() && st) {
        e.flow = reflow_story(d, story, pv->second.text, pv->second.flow, m_budget);
        if (e.flow.provisional) {   // s106: keep what the whole is worked out from
            e.basis = std::make_shared<const StoryFlow>(std::move(pv->second.flow));
            e.basis_text = std::make_shared<const core::Story>(std::move(pv->second.text));
        }
        m_prev.erase(pv);
        m_how = How::Reflowed;
    } else {
        e.flow = flow_story(d, story);
        m_how = How::Whole;
    }
    return m_flows.emplace(story, std::move(e)).first->second.flow;
}

std::vector<std::string> foot_neighbours(const core::Doc& d, const std::string& story) {   // s167
    return foot_neighbours_of(d, story);
}

}  // namespace composix::layout
