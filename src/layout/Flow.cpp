#include "layout/Flow.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
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
        if (!cs) continue;
        const auto add = [&](PangoAttribute* at) {
            at->start_index = static_cast<guint>(a - from);
            at->end_index = static_cast<guint>(b - from);
            pango_attr_list_insert(attrs, at);
        };
        if (!cs->family.empty()) add(pango_attr_family_new(cs->family.c_str()));
        if (cs->size > 0) add(pango_attr_size_new(static_cast<int>(std::lround(cs->size * PANGO_SCALE))));
        if (cs->bold) add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        if (cs->italic) add(pango_attr_style_new(PANGO_STYLE_ITALIC));
    }
    pango_layout_set_attributes(l, attrs);
    pango_attr_list_unref(attrs);
}

LayoutPtr make_layout(const core::Style& st, const char* text, int len, double width, bool justify_last) {
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
    pango_layout_set_text(l.get(), text, len);
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

// Fill one frame's columns from `c`, each column at most `hmax` tall. True
// when the story ended inside this frame.
bool fill_frame(const core::Doc& d, const core::Story& st, const core::Frame& fr, Cursor& c, double hmax,
                FrameText& ft) {
    const int n = core::column_count(fr);
    const int paras = static_cast<int>(st.paras.size());
    for (int col = 0; col < n; ++col) {
        const double cw = core::column_width(fr, col), cx = core::column_x(fr, col);
        double y = 0;
        while (c.p < paras) {
            const auto& para = st.paras[static_cast<std::size_t>(c.p)];
            const auto style = style_of(d, para.style);
            const double lead = leading_of(style);
            // s011: space before a paragraph -- not at the top of a column.
            const double before = (c.start == 0 && y > 0) ? style.space_before : 0;
            auto lay = make_layout(style, para.text.c_str() + c.start, static_cast<int>(para.text.size()) - c.start, cw, false);
            apply_runs(lay.get(), d, para, c.start);
            const int nl = pango_layout_get_line_count(lay.get());
            int placed = 0;
            double yy = y + before;
            for (int i = 0; i < nl; ++i) {
                if (yy + lead > hmax + 0.01) break;   // the column is full
                PangoLayoutLine* line = pango_layout_get_line_readonly(lay.get(), i);
                const int a = c.start + line->start_index;
                ft.lines.push_back({c.p, a, a + line->length, yy, lead, col, cx});
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
    }
    return false;
}
}  // namespace

StoryFlow flow_story(const core::Doc& d, const std::string& story) {
    StoryFlow out;
    const auto* st = d.as<core::Story>(story);
    if (!st) return out;
    const auto chain = thread_frames(d, story);
    for (const auto& f : chain) out.frames.push_back({f, {}});
    if (chain.empty()) return out;   // not placed: nothing flows, nothing is "over"

    Cursor c;
    bool done = st->paras.empty();
    for (std::size_t fi = 0; fi < chain.size() && !done; ++fi) {
        const auto& fr = *d.as<core::Frame>(chain[fi]);
        const Cursor at = c;
        done = fill_frame(d, *st, fr, c, fr.h, out.frames[fi]);
        if (done && fr.balance && core::column_count(fr) > 1) {
            // The shortest column height that still holds the rest.
            double lo = 0, hi = fr.h;
            for (int k = 0; k < 24 && hi - lo > 0.25; ++k) {
                const double mid = (lo + hi) / 2;
                Cursor t = at;
                FrameText tmp{chain[fi], {}};
                if (fill_frame(d, *st, fr, t, mid, tmp)) hi = mid; else lo = mid;
            }
            c = at;
            out.frames[fi].lines.clear();
            fill_frame(d, *st, fr, c, hi, out.frames[fi]);
        }
    }
    for (const auto& ft : out.frames) out.lines += static_cast<int>(ft.lines.size());
    out.overset = !done;
    return out;
}

namespace {

// One placed line, as the drawing and the editing geometry both see it.
struct Placed {
    const Line*      line;
    PangoLayout*     lay;      // the paragraph's layout in this frame
    PangoLayoutLine* pl;
    int              from;     // the byte the layout starts at in the paragraph
    double           x;        // the line's left (alignment), frame coords
    double           base;     // its baseline, frame coords
};

const FrameText* frame_text(const StoryFlow& flow, const std::string& frame) {
    for (const auto& f : flow.frames) if (f.frame == frame) return &f;
    return nullptr;
}

// Walk a frame's lines. Lines of one paragraph in a frame come from ONE
// layout started where this frame's part of the paragraph starts -- the same
// text at the same width breaks exactly as the flow measured it.
template <class F>
void walk_frame(const core::Doc& d, const std::string& story, const StoryFlow& flow, const std::string& frame, F&& f) {
    const auto* st = d.as<core::Story>(story);
    const auto* fr = d.as<core::Frame>(frame);
    const FrameText* ft = frame_text(flow, frame);
    if (!st || !fr || !ft || ft->lines.empty()) return;
    std::size_t i = 0;
    while (i < ft->lines.size()) {
        const int p = ft->lines[i].para;
        const int col = ft->lines[i].col;
        std::size_t j = i;
        while (j < ft->lines.size() && ft->lines[j].para == p && ft->lines[j].col == col) ++j;
        if (p >= static_cast<int>(st->paras.size())) break;
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)].style);
        const int from = std::min(ft->lines[i].start, static_cast<int>(text.size()));
        const bool continues = ft->lines[j - 1].end < static_cast<int>(text.size());
        auto lay = make_layout(style, text.c_str() + from, static_cast<int>(text.size()) - from,
                               core::column_width(*fr, col), continues);
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
            f(Placed{&ln, lay.get(), pango_layout_iter_get_line_readonly(it), from, ln.x + x,
                     ln.y + (ln.h - h) / 2 + (bl - top)});
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

void draw_frame_text(cairo_t* cr, const core::Doc& d, const std::string& story, const StoryFlow& flow,
                     const std::string& frame, double ox, double oy, double ink) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, ink, ink, ink > 0 ? ink + 0.02 : 0);
    walk_frame(d, story, flow, frame, [&](const Placed& pl) {
        cairo_move_to(cr, ox + pl.x, oy + pl.base);
        pango_cairo_show_layout_line(cr, pl.pl);
    });
    cairo_restore(cr);
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
    const int to = cur + dir;
    if (to < 0) return {0, 0};                                                  // above the first line: the start
    if (to >= static_cast<int>(all.size())) return {all.back().second->para, all.back().second->end};   // the end
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
        double x0 = x_of(pl, s), x1 = x_of(pl, e);
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

const StoryFlow& FlowCache::get(const core::Doc& d, const std::string& story) {
    if (d.revision() != m_rev) { m_flows.clear(); m_rev = d.revision(); }
    auto it = m_flows.find(story);
    if (it == m_flows.end()) it = m_flows.emplace(story, flow_story(d, story)).first;
    return it->second;
}

}  // namespace composix::layout
