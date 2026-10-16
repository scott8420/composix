// plate_test -- Plate's own headless test. Writes a PDF with Pango text,
// colour in three spaces, an image with alpha, page boxes and labels, then
// reads the file back for the structures it must hold.
#include "plate/pango.hpp"

#include <pango/pangocairo.h>

#include <cstdio>
#include <cstdlib>
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
    {   // s021: opacity -- an ExtGState per page, reused
        plate::Document ad;
        ad.set_compress(false);
        auto& pg = ad.add_page(100, 100);
        pg.save();
        pg.set_alpha(0.5, 0.25);
        pg.rect({0, 0, 10, 10});
        pg.fill();
        pg.set_alpha(0.5, 0.25);
        pg.restore();
        check("alpha: save", ad.save(out + ".alpha.pdf", &err));
        std::ifstream f(out + ".alpha.pdf", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto a = ss.str();
        check("alpha: one ExtGState, /ca + /CA, used by gs", a.find("/GS0 << /Type /ExtGState /ca 0.5 /CA 0.25 >>") != std::string::npos &&
                                                            a.find("/GS1") == std::string::npos && a.find("/GS0 gs") != std::string::npos);
    }
    // ── s016: colour management (needs a CMYK output profile: PLATE_ICC) ─────
    const char* icc = std::getenv("PLATE_ICC");
    if (!icc) {
        std::printf("  (skip colour tests: set PLATE_ICC to a CMYK output profile)\n");
    } else {
        const std::uint8_t rgb[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
        const auto read = [](const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        {   // convert: RGB becomes the press's CMYK
            plate::Document cd;
            cd.set_compress(false);
            check("profile: a CMYK output profile loads", cd.set_output_profile(icc, &err));
            check("profile: an RGB profile is refused", !cd.set_output_profile("/usr/share/color/icc/sRGB.icc", &err) || !cd.has_output_profile());
            cd.set_convert_rgb(true);
            const auto c = cd.prepare(plate::Color::rgb(1, 0, 0));
            check("convert: RGB red -> CMYK, mostly M + Y", c.space == plate::Color::Space::CMYK && c.v[1] > 0.7 && c.v[2] > 0.7 && c.v[0] < 0.2);
            auto& pg = cd.add_page(200, 200);
            pg.set_fill(plate::Color::rgb(1, 0, 0));
            pg.rect({10, 10, 50, 50});
            pg.fill();
            pg.set_fill(plate::Color::spot("PANTONE 7593 C", 1.0, 0, 0.7, 0.9, 0.2));
            pg.rect({70, 10, 50, 50});
            pg.fill();
            pg.set_fill(plate::Color::spot("PANTONE 7593 C", 0.4, 0, 0.7, 0.9, 0.2));
            pg.rect({130, 10, 50, 50});
            pg.fill();
            pg.image(cd.add_image_rgb(2, 2, rgb), {10, 80, 100, 100});
            check("convert: save", cd.save(out + ".cmyk.pdf", &err));
            const auto pdf = read(out + ".cmyk.pdf");
            const auto has = [&](const char* x) { return pdf.find(x) != std::string::npos; };
            check("convert: no RGB left in the content", !has(" rg\n") && has(" k\n"));
            check("convert: the image is DeviceCMYK", has("/ColorSpace /DeviceCMYK"));
            check("spot: a Separation with its CMYK alternate", has("[/Separation /PANTONE#207593#20C /DeviceCMYK") &&
                                                                  has("/C1 [0 0.7 0.9 0.2]"));
            check("spot: used by tint (one colour space, two tints)", has("/CS0 cs 1 scn") && has("/CS0 cs 0.4 scn"));
        }
        {   // keep: RGB stays RGB, tagged sRGB
            plate::Document kd;
            kd.set_compress(false);
            kd.set_output_profile(icc, &err);
            auto& pg = kd.add_page(100, 100);
            pg.image(kd.add_image_rgb(2, 2, rgb), {0, 0, 100, 100});
            check("keep: save", kd.save(out + ".keep.pdf", &err));
            const auto pdf = read(out + ".keep.pdf");
            check("keep: the image ICCBased (sRGB)", pdf.find("/ColorSpace [/ICCBased") != std::string::npos);
        }
        // ── s017: PDF/X ──────────────────────────────────────────────────────
        const std::uint8_t a4[] = {255, 128, 0, 255};
        for (auto st : {plate::Standard::X1a_2003, plate::Standard::X4}) {
            const bool x1 = st == plate::Standard::X1a_2003;
            const std::string tag = x1 ? "x1a: " : "x4: ";
            plate::Document xd;
            xd.set_compress(false);
            xd.set_standard(st);
            xd.set_title("Guild & <Map>");
            xd.set_date(1792094400);
            auto& pg = xd.add_page(100, 100);
            // no profile, no trim: refused, with reasons
            const auto probs = xd.preflight();
            check((tag + "preflight names the missing profile and trim").c_str(), probs.size() >= 2);
            check((tag + "save refuses").c_str(), !xd.save(out + ".x.pdf", &err) && err.find("TrimBox") != std::string::npos);
            xd.set_output_profile(icc, &err);
            pg.set_trim_box({0, 0, 100, 100});
            pg.set_fill(plate::Color::cmyk(0, 0, 0, 1));
            pg.rect({10, 10, 20, 20});
            pg.fill();
            pg.image(xd.add_image_rgb(2, 2, rgb, a4), {0, 50, 50, 50});
            check((tag + "save").c_str(), xd.save(out + (x1 ? ".x1a.pdf" : ".x4.pdf"), &err));
            const auto pdf = read(out + (x1 ? ".x1a.pdf" : ".x4.pdf"));
            const auto has = [&](const char* x) { return pdf.find(x) != std::string::npos; };
            check((tag + "version").c_str(), pdf.rfind(x1 ? "%PDF-1.4" : "%PDF-1.6", 0) == 0);
            check((tag + "output intent with the profile").c_str(), has("/S /GTS_PDFX") && has("/DestOutputProfile"));
            check((tag + "version keys").c_str(), x1 ? has("/GTS_PDFXConformance (PDF/X-1a:2003)") : has("/GTS_PDFXVersion (PDF/X-4)"));
            check((tag + "trapped, dates, ID").c_str(), has("/Trapped /False") && has("/CreationDate (D:") && has("/ID [<"));
            check((tag + "XMP metadata").c_str(), has("/Subtype /XML") && has("Guild &amp; &lt;Map&gt;"));
            if (x1) check("x1a: image flattened to CMYK, no SMask", !has("/SMask") && has("/ColorSpace /DeviceCMYK"));
            else check("x4: image keeps its alpha, ICC-tagged", has("/SMask") && has("/ICCBased"));
        }
        // X-1a with an OpenType CFF font: the bare CFF program (CIDFontType0C).
        const char* otf = "/usr/share/texmf/fonts/opentype/public/lm/lmroman10-italic.otf";
        if (std::ifstream(otf).good()) {
            plate::Document fd;
            fd.set_compress(false);
            fd.set_standard(plate::Standard::X1a_2003);
            fd.set_output_profile(icc, &err);
            hb_blob_t* blob = hb_blob_create_from_file(otf);
            hb_face_t* face = hb_face_create(blob, 0);
            auto& pg = fd.add_page(100, 100);
            pg.set_trim_box({0, 0, 100, 100});
            pg.glyphs(fd.add_font(face), 12, 10, 50, {{36, 7, 0, 0, "A"}, {37, 7, 0, 0, "B"}});
            check("x1a: CFF font saves", fd.save(out + ".x1acff.pdf", &err));
            const auto pdf = read(out + ".x1acff.pdf");
            check("x1a: CFF as CIDFontType0C in PDF 1.4", pdf.rfind("%PDF-1.4", 0) == 0 &&
                                                           pdf.find("/Subtype /CIDFontType0C") != std::string::npos &&
                                                           pdf.find("/Subtype /OpenType") == std::string::npos);
            hb_face_destroy(face);
            hb_blob_destroy(blob);
        }
        {   // X-1a: opacity is transparency -- refused, said why
            plate::Document td;
            td.set_standard(plate::Standard::X1a_2003);
            td.set_output_profile(icc, &err);
            auto& pg = td.add_page(10, 10);
            pg.set_trim_box({0, 0, 10, 10});
            pg.set_alpha(0.5, 1);
            check("x1a: opacity refused", !td.save(out + ".x1aalpha.pdf", &err) && err.find("opacity") != std::string::npos);
        }
        {   // an RGB colour drawn with no profile to convert it is named
            plate::Document rd;
            rd.set_standard(plate::Standard::X4);
            auto& pg = rd.add_page(10, 10);
            pg.set_fill(plate::Color::rgb(1, 0, 0));
            bool named = false;
            for (const auto& p : rd.preflight()) named |= p.find("RGB colour") != std::string::npos;
            check("x4: unprofiled RGB drawing is named", named);
        }
    }
    g_object_unref(ctx);
    std::printf("%d pass / %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
