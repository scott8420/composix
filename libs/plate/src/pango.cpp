#include "plate/pango.hpp"

#include <pango/pangocairo.h>

namespace plate::pango {

std::vector<Run> runs(PangoLayoutLine* line, double x, double baseline) {
    std::vector<Run> out;
    if (!line) return out;
    const char* text = pango_layout_get_text(line->layout);
    double pen = x;
    for (GSList* l = line->runs; l; l = l->next) {
        auto* gi = static_cast<PangoGlyphItem*>(l->data);
        PangoItem* item = gi->item;
        PangoGlyphString* gs = gi->glyphs;
        Run r;
        r.x = pen;
        r.baseline = baseline;
        r.start = item->offset;
        r.end = item->offset + item->length;
        if (hb_font_t* hf = pango_font_get_hb_font(item->analysis.font)) r.face = hb_font_get_face(hf);
        PangoFontDescription* fd = pango_font_describe_with_absolute_size(item->analysis.font);
        r.size = pango_font_description_get_size(fd) / static_cast<double>(PANGO_SCALE);
        pango_font_description_free(fd);
        // Each cluster's text rides on its first glyph (for copy / search).
        for (int i = 0; i < gs->num_glyphs; ++i) {
            const PangoGlyphInfo& g = gs->glyphs[i];
            const double adv = g.geometry.width / static_cast<double>(PANGO_SCALE);
            if (g.glyph == PANGO_GLYPH_EMPTY || (g.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) { pen += adv; if (!r.glyphs.empty()) r.glyphs.back().advance += adv; else r.x += adv; continue; }
            Glyph pg;
            pg.gid = g.glyph;
            pg.advance = adv;
            pg.x_off = g.geometry.x_offset / static_cast<double>(PANGO_SCALE);
            pg.y_off = g.geometry.y_offset / static_cast<double>(PANGO_SCALE);
            if (g.attr.is_cluster_start) {
                const int a = item->offset + gs->log_clusters[i];
                int b = item->offset + item->length;
                for (int k = i + 1; k < gs->num_glyphs; ++k)
                    if (gs->glyphs[k].attr.is_cluster_start && gs->log_clusters[k] != gs->log_clusters[i]) {
                        b = item->offset + gs->log_clusters[k];
                        break;
                    }
                if (b > a) pg.text.assign(text + a, static_cast<std::size_t>(b - a));
            }
            r.glyphs.push_back(std::move(pg));
            pen += adv;
        }
        if (r.face && r.size > 0) out.push_back(std::move(r));
    }
    return out;
}

void show(Document& doc, Page& page, const Run& r) {
    if (!r.face || r.glyphs.empty()) return;
    page.glyphs(doc.add_font(r.face), r.size, r.x, r.baseline, r.glyphs);
}

}  // namespace plate::pango
