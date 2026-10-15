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

StoryFlow flow_story(const core::Doc& d, const std::string& story) {
    StoryFlow out;
    const auto* st = d.as<core::Story>(story);
    if (!st) return out;
    const auto chain = thread_frames(d, story);
    for (const auto& f : chain) out.frames.push_back({f, {}});
    if (chain.empty()) return out;   // not placed: nothing flows, nothing is "over"

    std::size_t fi = 0;
    double y = 0;
    for (int p = 0; p < static_cast<int>(st->paras.size()); ++p) {
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)].style);
        const double lead = leading_of(style);
        int start = 0;
        while (true) {
            if (fi >= chain.size()) { out.overset = true; return out; }
            const auto& fr = *d.as<core::Frame>(chain[fi]);
            auto lay = make_layout(style, text.c_str() + start, static_cast<int>(text.size()) - start, fr.w, false);
            const int n = pango_layout_get_line_count(lay.get());
            int placed = 0;
            for (int i = 0; i < n; ++i) {
                if (y + lead > fr.h + 0.01) break;   // the frame is full
                PangoLayoutLine* line = pango_layout_get_line_readonly(lay.get(), i);
                const int a = start + line->start_index;
                out.frames[fi].lines.push_back({p, a, a + line->length, y, lead});
                y += lead;
                ++placed;
                ++out.lines;
            }
            if (placed == n) break;   // the whole (rest of the) paragraph fit
            // Continue in the next frame from the first line that didn't fit.
            start += pango_layout_get_line_readonly(lay.get(), placed)->start_index;
            ++fi;
            y = 0;
        }
    }
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
        std::size_t j = i;
        while (j < ft->lines.size() && ft->lines[j].para == p) ++j;
        if (p >= static_cast<int>(st->paras.size())) break;
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)].style);
        const int from = std::min(ft->lines[i].start, static_cast<int>(text.size()));
        const bool continues = ft->lines[j - 1].end < static_cast<int>(text.size());
        auto lay = make_layout(style, text.c_str() + from, static_cast<int>(text.size()) - from, fr->w, continues);
        PangoLayoutIter* it = pango_layout_get_iter(lay.get());
        for (std::size_t k = i; k < j; ++k) {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double bl = pango_layout_iter_get_baseline(it) / static_cast<double>(PANGO_SCALE);
            const double top = logical.y / static_cast<double>(PANGO_SCALE);
            const double h = logical.height / static_cast<double>(PANGO_SCALE);
            const double x = logical.x / static_cast<double>(PANGO_SCALE);
            const auto& ln = ft->lines[k];
            f(Placed{&ln, lay.get(), pango_layout_iter_get_line_readonly(it), from, x,
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
    return pos.x / static_cast<double>(PANGO_SCALE);
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
                     const std::string& frame, double ox, double oy) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.1);
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
    // The line under y (above the first: the first; below the last: the last).
    const Line* want = &ft->lines.front();
    for (const auto& l : ft->lines) if (y >= l.y) want = &l;
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

const StoryFlow& FlowCache::get(const core::Doc& d, const std::string& story) {
    if (d.revision() != m_rev) { m_flows.clear(); m_rev = d.revision(); }
    auto it = m_flows.find(story);
    if (it == m_flows.end()) it = m_flows.emplace(story, flow_story(d, story)).first;
    return it->second;
}

}  // namespace composix::layout
