#include "layout/Flow.hpp"
#include "core/Hyphen.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace composix::layout {

namespace {

// One measuring context for the process: 72 dpi (1 unit = 1 pt), no hinting,
// so the same text breaks the same way at every zoom.
PangoContext* measure_context() {
    static PangoContext* ctx = [] {
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
    }();
    return ctx;
}

struct LayoutDel { void operator()(PangoLayout* l) const { g_object_unref(l); } };
using LayoutPtr = std::unique_ptr<PangoLayout, LayoutDel>;

// The style a paragraph is set in (Body 11/14 when it names none).
core::Style style_of(const core::Doc& d, const std::string& id) {
    if (const auto* s = d.as<core::Style>(id)) return *s;
    return core::Style{};
}

double leading_of(const core::Style& s) { return s.leading > 0 ? s.leading : std::max(1.0, s.size * 1.2); }

// s011: a paragraph's character styles, as Pango attributes over the part of
// its text this layout holds (bytes from `from`).
void apply_runs(PangoLayout* l, const core::Doc& d, const core::Para& p, int from) {
    if (p.runs.empty()) return;
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
            const auto c = core::tint_rgb(sw->preview, tint);   // s021: at its tint
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
    pango_layout_set_attributes(l, attrs);
    pango_attr_list_unref(attrs);
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
            const auto prev = style_of(d, s.paras[static_cast<std::size_t>(p - 1)].style);
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
            static const char* sign[] = {"\xEF\xBC\x83", "\xE2\xB8\xB9", "\xE3\x80\x8B", "\xE3\x80\x8A"};   // ＃ ⸹ 》 《
            shown.replace(i, 3, sign[k]);
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
struct Cursor { int p = 0; int start = 0; };

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
    const auto letter = [&](int i) { return i >= 0 && i < n && std::isalpha(static_cast<unsigned char>(t[static_cast<std::size_t>(i)])); };
    // s065: a discretionary hyphen (U+00AD, typed) is part of the word.
    const auto shy = [&](int i) { return i >= 0 && i + 1 < n && static_cast<unsigned char>(t[static_cast<std::size_t>(i)]) == 0xC2 &&
                                         static_cast<unsigned char>(t[static_cast<std::size_t>(i + 1)]) == 0xAD; };
    if (!(letter(a) || shy(a)) || t[static_cast<std::size_t>(a - 1)] != ' ') return 0;   // not a break before a word
    int b = a, letters = 0;
    std::vector<int> typed;   // after each discretionary hyphen (offsets in the word)
    while (letter(b) || shy(b)) {
        if (shy(b)) { b += 2; typed.push_back(b - a); }
        else { ++b; ++letters; }
    }
    if (b < n && static_cast<unsigned char>(t[static_cast<std::size_t>(b)]) >= 0x80) return 0;   // a word going on in other letters
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
        at = core::hyphen_points(std::string_view(t).substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a)));
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
    while (begin > 0 && L[begin - 1].col == col) --begin;
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
        const auto style = style_of(d, st.paras[static_cast<std::size_t>(q)].style);
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
        if (!style_of(d, st.paras[static_cast<std::size_t>(prev)].style).keep_next) break;
        send_on(a, prev, 0);
    }
}

// Fill one frame's columns from `c`, each column at most `hmax` tall. True
// when the story ended inside this frame.
bool fill_frame(const core::Doc& d, const core::Story& st, const core::Frame& fr, const std::vector<core::WrapRect>& wraps,
                Cursor& c, double hmax, FrameText& ft) {
    const int n = core::column_count(fr);
    const int paras = static_cast<int>(st.paras.size());
    // s036: the baseline grid of the page (or master) the frame is on.
    const core::BaselineGrid grid = fr.rot == 0 ? core::baseline_grid(d, fr.owner) : core::BaselineGrid{};
    for (int col = 0; col < n; ++col) {
        const double cw = core::column_width(fr, col), cx = core::column_x(fr, col);
        const bool wrapped = touches(wraps, cx, cx + cw);
        const double top = std::max(0.0, fr.inset.top);   // s040: the text starts below the top inset
        double y = top;
        while (c.p < paras) {
            const auto& para = st.paras[static_cast<std::size_t>(c.p)];
            const auto style = style_of(d, para.style);
            const double lead = leading_of(style);
            // s043: a paragraph that starts afresh -- not where something is set already.
            // s055: or a break typed before it (its own, over the style's).
            if (const auto sa = c.start == 0 ? core::doc::start_of(d, para) : core::ParaStart::Anywhere; sa != core::ParaStart::Anywhere) {
                if (sa == core::ParaStart::NextColumn && y > top) break;
                if (sa != core::ParaStart::NextColumn && !ft.lines.empty()) return false;   // flow_text finds the frame
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
                if (yy + n * lead > hmax + 0.01 && y > top) break;   // no room for it here: the next column
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
                bool ended = false, full = false;
                int hy_run = 0;   // s065: hyphenated lines in a row
                while (!ended) {
                    yy = snap(yy);
                    if (yy + lead > hmax + 0.01) { full = true; break; }
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
                        hy_run = cut > 0 || at_shy ? hy_run + 1 : 0;
                        ft.lines.push_back({c.p, pos, pos + took, yy, lead, col, x0, w});
                        ft.lines.back().hyphen = cut > 0 || at_shy;
                        pos += took;
                        if (pos >= len) { ended = true; break; }
                    }
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
            auto lay = make_layout(style, para.text.c_str() + c.start, len - c.start, g.width, false, g.indent);
            apply_runs(lay.get(), d, para, c.start);
            const int nl = pango_layout_get_line_count(lay.get());
            int placed = 0;
            for (int i = 0; i < nl; ++i) {
                yy = snap(yy);
                if (yy + lead > hmax + 0.01) break;   // the column is full
                PangoLayoutLine* line = pango_layout_get_line_readonly(lay.get(), i);
                const int a = c.start + line->start_index;
                ft.lines.push_back({c.p, a, a + line->length, yy, lead, col, cx + g.dx});
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
        if (c.p >= paras) return true;
        apply_keeps(d, st, fr, col, c, ft);   // s039
    }
    return false;
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
    if (!wraps.empty() || (fr.balance && core::column_count(fr) > 1)) return;
    if (fr.rot == 0 && core::baseline_grid(d, fr.owner).step > 0)
        for (const auto& l : ft.lines)
            if (l.para < static_cast<int>(st.paras.size()) &&
                style_of(d, st.paras[static_cast<std::size_t>(l.para)].style).align_grid) return;
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
StoryFlow flow_impl(const core::Doc& d, const std::string& story, const core::Story& text,
                    const std::map<std::string, double>* heights);
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
                    const std::map<std::string, double>* heights) {
    StoryFlow out;
    const auto* st = &text;
    const auto chain = thread_frames(d, story);
    for (const auto& f : chain) out.frames.push_back({f, {}});
    if (chain.empty()) return out;   // not placed: nothing flows, nothing is "over"

    Cursor c;
    bool done = st->paras.empty();
    std::string last_page;   // s043: the page of the last frame that took lines
    for (std::size_t fi = 0; fi < chain.size() && !done; ++fi) {
        const auto& fr = *d.as<core::Frame>(chain[fi]);
        // s043: a paragraph to start on a new (right-hand) page passes the
        // frames on the page it would follow on, and left-hand pages.
        if (c.start == 0 && c.p < static_cast<int>(st->paras.size()) && !last_page.empty()) {
            const auto start = core::doc::start_of(d, st->paras[static_cast<std::size_t>(c.p)]);   // s055: a break too
            if ((start == core::ParaStart::NextPage || start == core::ParaStart::NextRightPage) &&
                (fr.owner == last_page || (start == core::ParaStart::NextRightPage && d.side_of(fr.owner) == core::Side::Left)))
                continue;
        }
        const Cursor at = c;
        const auto wraps = core::wrap_rects(d, chain[fi]);
        // s048: a frame being measured for its auto-size gets room to spare (and no vertical justification).
        const auto hit = heights ? heights->find(chain[fi]) : decltype(heights->end()){};
        const bool measuring = heights && hit != heights->end();
        const double fh = measuring ? hit->second : fr.h;
        const double floor = fh - std::max(0.0, fr.inset.bottom);   // s040: above the bottom inset
        done = fill_frame(d, *st, fr, wraps, c, floor, out.frames[fi]);
        if (done && fr.balance && core::column_count(fr) > 1) {
            // The shortest column height that still holds the rest.
            double lo = std::max(0.0, fr.inset.top), hi = floor;
            for (int k = 0; k < 24 && hi - lo > 0.25; ++k) {
                const double mid = (lo + hi) / 2;
                Cursor t = at;
                FrameText tmp{chain[fi], {}};
                if (fill_frame(d, *st, fr, wraps, t, mid, tmp)) hi = mid; else lo = mid;
            }
            c = at;
            out.frames[fi].lines.clear();
            fill_frame(d, *st, fr, wraps, c, hi, out.frames[fi]);
        }
        if (!measuring) vertical_justify(d, *st, fr, wraps, out.frames[fi]);   // s042
        if (!out.frames[fi].lines.empty()) last_page = fr.owner;
    }
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
    std::size_t i = 0;
    while (i < ft->lines.size()) {
        const int p = ft->lines[i].para;
        const int col = ft->lines[i].col;
        std::size_t j = i + 1;
        // s018: a line set on its own (w > 0) is laid out alone, as the flow set it.
        if (ft->lines[i].w <= 0)
            while (j < ft->lines.size() && ft->lines[j].para == p && ft->lines[j].col == col && ft->lines[j].w <= 0) ++j;
        if (p >= static_cast<int>(st->paras.size())) break;
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)].style);
        const int from = std::min(ft->lines[i].start, static_cast<int>(text.size()));
        const bool solo = ft->lines[i].w > 0;
        const bool continues = !solo && ft->lines[j - 1].end < static_cast<int>(text.size());
        const Indents ind = indents_of(d, style, *st, p);   // s026: as the flow set it
        const Geo g = geo_of(ind, core::column_width(*fr, col), from == 0);
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

std::vector<RuleMark> column_rules(const core::Frame& f) {
    std::vector<RuleMark> out;
    const int n = core::column_count(f);
    const auto& r = f.col_rule;
    if (f.kind != core::FrameKind::Text || n < 2 || r.weight <= 0) return out;
    const double y0 = std::max(0.0, f.inset.top) + std::max(0.0, r.inset);
    const double y1 = f.h - std::max(0.0, f.inset.bottom) - std::max(0.0, r.inset);
    if (y1 <= y0) return out;
    for (int c = 1; c < n; ++c) {
        const double x = (core::column_x(f, c - 1) + core::column_width(f, c - 1) + core::column_x(f, c)) / 2;
        out.push_back({{x - r.weight / 2, y0, r.weight, y1 - y0}, r.color, r.tint});
    }
    return out;
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
        for (auto& m : column_rules(*fr)) marks.push_back(std::move(m));
    for (const auto& m : marks) {
        if (const auto* sw = d.as<core::Swatch>(m.color)) {
            const auto c = core::tint_rgb(sw->preview, m.tint);
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
            const auto c = core::tint_rgb(sw->preview, tint);   // s021
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
    }, text);
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
        const auto style = style_of(d, para.style);
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
            const double cx = core::column_x(*fr, ln.col), cw = core::column_width(*fr, ln.col);
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
    return out;
}

void visit_lines(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame,
                 const std::function<void(const LineVisit&)>& f, const core::Story* text) {
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
    }, text);
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

Resolved resolve_story(const core::Doc& d, const std::string& story, const std::string& on_page) {
    Resolved r;
    const auto* st = d.as<core::Story>(story);
    if (!st || !core::has_markers(*st)) return r;
    r.any = true;
    r.text = *st;
    // Where each marker is set (page frames): from the plain flow.
    const StoryFlow plain = on_page.empty() ? flow_story(d, story) : StoryFlow{};
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
            std::string v = d.as<core::Page>(page) ? marker_value(d, k, page, frame) : src.substr(i, 3);
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
        walk_frame(d, story, flow, ft.frame, [&](const Placed& pl) {
            if (c.ok || !on_line(*st, *pl.line, p)) return;
            c = {true, ft.frame, x_of(pl, p.byte), pl.line->y, pl.line->h};
        });
        if (c.ok) break;
    }
    return c;
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
    for (const auto& l : ft->lines)
        if (l.col == col && (!want || y >= l.y)) want = &l;
    if (!want) want = &ft->lines.front();
    // s018: lines side by side (a wrap between them): the rightmost that
    // starts left of x, else the leftmost.
    if (want->w > 0) {
        const double row = want->y;
        const Line* left = nullptr;
        const Line* best = nullptr;
        for (const auto& l : ft->lines) {
            if (l.col != col || l.y != row || l.w <= 0) continue;
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
                                        const std::string& frame, core::TextPos a, core::TextPos b) {
    std::vector<core::Rect> out;
    if (b < a) std::swap(a, b);
    if (a == b) return out;
    const auto* st = d.as<core::Story>(story);
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
    });
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
    if (d.revision() != m_rev) { m_flows.clear(); m_resolved.clear(); m_rev = d.revision(); }
}

const Resolved& FlowCache::resolved(const core::Doc& d, const std::string& story, const std::string& on_page) {
    check(d);
    const std::string key = story + "|" + on_page;
    auto it = m_resolved.find(key);
    if (it == m_resolved.end()) it = m_resolved.emplace(key, resolve_story(d, story, on_page)).first;
    return it->second;
}

const StoryFlow& FlowCache::get(const core::Doc& d, const std::string& story) {
    check(d);
    auto it = m_flows.find(story);
    if (it == m_flows.end()) it = m_flows.emplace(story, flow_story(d, story)).first;
    return it->second;
}

}  // namespace composix::layout
