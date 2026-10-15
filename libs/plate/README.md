# Plate

A small PDF writer for print, in C++23. Born in Composix (s015); standalone so
other apps can use it.

- **Text** as positioned glyph runs: fonts embedded as **subsets** (HarfBuzz's
  subsetter, glyph ids kept), Identity-H with a ToUnicode map, so the text can
  be copied and searched. TrueType -> FontFile2 (PDF 1.4); CFF / OpenType ->
  FontFile3 (PDF 1.6).
- **Colour** in Gray, RGB, CMYK or **spot** (`plate::Color::spot(ink, tint,
  c, m, y, k)` -> a Separation with its CMYK alternate; tints share one
  colour space).
- **Colour management** (LittleCMS): `set_output_profile(press.icc)`; then
  RGB is either **converted** to the press's CMYK (`set_convert_rgb(true)`;
  images and colours, from sRGB; relative colorimetric + black point
  compensation, or perceptual) or **kept** and tagged sRGB (ICCBased) for
  the RIP. `find_cmyk_profiles(dirs)` lists the press profiles on a system.
  PDF/X output intents follow (Composix s017).
- **Images**: 8-bit RGB with an optional alpha (soft mask), or a JPEG's own
  bytes (DCTDecode, untouched).
- **Pages**: any size each, Media / Bleed / Trim boxes, page labels.
- **Paths**: rectangles, lines, fill / stroke, rectangle clips, save /
  restore, translate.
- Coordinates in points from the **top-left**, y down.

Depends on HarfBuzz (`harfbuzz`, `harfbuzz-subset`), LittleCMS (`lcms2`) and
zlib (Fedora: `harfbuzz-devel lcms2-devel zlib-devel`). The optional
`plate_pango` adapter (`plate/pango.hpp`) turns a `PangoLayoutLine` into glyph
runs, so anything laid out with Pango prints exactly as it was laid out.

```cpp
plate::Document doc;
doc.set_title("Example");
auto& pg = doc.add_page(612, 792);
pg.set_trim_box({0, 0, 612, 792});
pg.set_fill(plate::Color::cmyk(0, 0, 0, 1));
for (const auto& run : plate::pango::runs(line, 72, 100))   // a PangoLayoutLine
    plate::pango::show(doc, pg, run);
doc.save("example.pdf");
```

Build on its own: `cmake -S libs/plate -B build-plate && cmake --build build-plate`,
then `build-plate/plate_test out.pdf` (writes a PDF and checks its structure;
set `PLATE_ICC=/path/to/press.icc` to test the colour management too).
In an app: `add_subdirectory(libs/plate)` and link `plate` or `plate_pango`.
