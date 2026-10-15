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
                out.frames[fi].lines.push_back({p, a, a + line->length, y});
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

void draw_frame_text(cairo_t* cr, const core::Doc& d, const std::string& story, const StoryFlow& flow,
                     const std::string& frame, double ox, double oy) {
    const auto* st = d.as<core::Story>(story);
    const auto* fr = d.as<core::Frame>(frame);
    if (!st || !fr) return;
    const FrameText* ft = nullptr;
    for (const auto& f : flow.frames) if (f.frame == frame) ft = &f;
    if (!ft || ft->lines.empty()) return;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.1);
    // Lines of one paragraph in this frame are drawn from ONE layout started
    // where this frame's part of the paragraph starts -- the same text at the
    // same width breaks exactly as the flow measured it.
    std::size_t i = 0;
    while (i < ft->lines.size()) {
        const int p = ft->lines[i].para;
        std::size_t j = i;
        while (j < ft->lines.size() && ft->lines[j].para == p) ++j;
        const auto& text = st->paras[static_cast<std::size_t>(p)].text;
        const auto style = style_of(d, st->paras[static_cast<std::size_t>(p)].style);
        const double lead = leading_of(style);
        const int from = ft->lines[i].start;
        const bool continues = ft->lines[j - 1].end < static_cast<int>(text.size());
        auto lay = make_layout(style, text.c_str() + from, static_cast<int>(text.size()) - from, fr->w, continues);
        PangoLayoutIter* it = pango_layout_get_iter(lay.get());
        for (std::size_t k = i; k < j; ++k) {
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double base = pango_layout_iter_get_baseline(it) / static_cast<double>(PANGO_SCALE);
            const double top = logical.y / static_cast<double>(PANGO_SCALE);
            const double h = logical.height / static_cast<double>(PANGO_SCALE);
            const double x = logical.x / static_cast<double>(PANGO_SCALE);
            cairo_move_to(cr, ox + x, oy + ft->lines[k].y + (lead - h) / 2 + (base - top));
            pango_cairo_show_layout_line(cr, pango_layout_iter_get_line_readonly(it));
            if (!pango_layout_iter_next_line(it)) break;
        }
        pango_layout_iter_free(it);
        i = j;
    }
    cairo_restore(cr);
}

const StoryFlow& FlowCache::get(const core::Doc& d, const std::string& story) {
    if (d.revision() != m_rev) { m_flows.clear(); m_rev = d.revision(); }
    auto it = m_flows.find(story);
    if (it == m_flows.end()) it = m_flows.emplace(story, flow_story(d, story)).first;
    return it->second;
}

}  // namespace composix::layout
