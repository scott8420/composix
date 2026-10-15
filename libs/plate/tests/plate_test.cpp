// plate_test -- Plate's own headless test. Writes a PDF with Pango text,
// colour in three spaces, an image with alpha, page boxes and labels, then
// reads the file back for the structures it must hold.
#include "plate/pango.hpp"

#include <pango/pangocairo.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
int pass = 0, fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? pass : fail)++;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "/tmp/plate_test.pdf";
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    PangoContext* ctx = pango_font_map_create_context(fm);
    pango_cairo_context_set_resolution(ctx, 72.0);

    plate::Document doc;
    doc.set_title("Plate test \xE2\x80\x94 caf\xC3\xA9");
    doc.set_creator("plate_test");
    for (int n = 0; n < 2; ++n) {
        auto& pg = doc.add_page(432 + 18, 648 + 18);
        pg.set_bleed_box({0, 0, 450, 666});
        pg.set_trim_box({9, 9, 432, 648});
        pg.set_label(n == 0 ? "i" : "1");

        pg.set_fill(plate::Color::cmyk(0, 0.7, 0.9, 0.2));
        pg.rect({9, 9, 432, 60});
        pg.fill();

        PangoLayout* lay = pango_layout_new(ctx);
        PangoFontDescription* fd = pango_font_description_from_string(n == 0 ? "Serif 14" : "Sans Bold 12");
        pango_layout_set_font_description(lay, fd);
        pango_font_description_free(fd);
        pango_layout_set_width(lay, 360 * PANGO_SCALE);
        pango_layout_set_text(lay, "Plate sets text exactly as Pango laid it out \xE2\x80\x94 caf\xC3\xA9, fi ligatures, AV kerning.", -1);
        PangoLayoutIter* it = pango_layout_get_iter(lay);
        pg.set_fill(plate::Color::gray(0));
        do {
            const double base = pango_layout_iter_get_baseline(it) / static_cast<double>(PANGO_SCALE);
            PangoRectangle lr;
            pango_layout_iter_get_line_extents(it, nullptr, &lr);
            for (const auto& r : plate::pango::runs(pango_layout_iter_get_line_readonly(it), 45 + lr.x / static_cast<double>(PANGO_SCALE), 120 + base))
                plate::pango::show(doc, pg, r);
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        g_object_unref(lay);

        // A 4 x 2 RGB image, half transparent on the right.
        const std::uint8_t rgb[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0,
                                    0, 0, 0, 128, 128, 128, 255, 255, 255, 0, 255, 255};
        const std::uint8_t alpha[] = {255, 255, 128, 0, 255, 255, 128, 0};
        const auto im = doc.add_image_rgb(4, 2, rgb, alpha);
        pg.save();
        pg.clip_rect({45, 300, 200, 100});
        pg.image(im, {45, 300, 240, 120});
        pg.restore();

        pg.set_stroke(plate::Color::rgb(0, 0.4, 0.8));
        pg.set_line_width(0.25);
        pg.move_to(0, 9);
        pg.line_to(5, 9);
        pg.stroke();
    }
    std::string err;
    doc.set_compress(false);
    check("save", doc.save(out, &err));
    std::ifstream f(out, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string pdf = ss.str();
    const auto has = [&](const char* s) { return pdf.find(s) != std::string::npos; };
    check("a PDF", pdf.rfind("%PDF-1.", 0) == 0);
    check("two pages", has("/Count 2"));
    check("fonts embedded as subsets", has("/FontFile2") || has("/FontFile3"));
    check("Identity-H with ToUnicode", has("/Identity-H") && has("/ToUnicode"));
    check("CMYK fill", has(" k\n"));
    check("image with a soft mask", has("/SMask"));
    check("trim and bleed boxes", has("/TrimBox [9 9 441 657]") && has("/BleedBox"));
    check("page labels", has("/PageLabels") && has("/P (i)"));
    check("unicode title", has("/Title <FEFF"));
    int w = 0, h = 0, c = 0;
    const std::string jpg("\xFF\xD8\xFF\xC0\x00\x11\x08\x00\x20\x00\x40\x03", 12);
    check("jpeg_info reads a frame header", plate::jpeg_info(jpg + std::string(8, '\0'), &w, &h, &c) && w == 64 && h == 32 && c == 3);
    g_object_unref(ctx);
    std::printf("%d pass / %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
