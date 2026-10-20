// composix_selftest, part 8 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part8() {

    {
        std::cout << "\n-- the bare document Close Project leaves (s146) --\n";
        core::Doc d;
        core::NewDoc n;
        n.title = "Untitled";
        n.pages = 1;
        n.text_frames = false;
        n.folios = false;
        check("made", core::new_document(d, n));
        check("one page, one master, one section", d.pages().size() == 1 && d.ids_of<core::Master>().size() == 1 &&
                                                   d.ids_of<core::Section>().size() == 1);
        check("no frames, no stories", d.ids_of<core::Frame>().empty() && d.ids_of<core::Story>().empty());
        check("the colours and the base styles", d.ids_of<core::Swatch>().size() >= 2 && !d.ids_of<core::Style>().empty());
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        check("a page can be added after it", !core::doc::add_pages(j, d, d.pages().front(), 1, d.ids_of<core::Master>().front()).empty() &&
                                              d.pages().size() == 2);
    }

    {
        std::cout << "\n-- masters own their page settings (s147) --\n";
        using core::kPt;
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::NewDoc n;
        n.pages = 6;
        n.text_frames = false;
        core::new_document(d, n);
        const auto body = d.ids_of<core::Master>().front();
        // A cover master: single, its own size and bleed, on page 1.
        core::Master cover = *d.as<core::Master>(body);
        cover.prefix = "C";
        cover.name = "Cover";
        cover.pos = 0.5;
        j.run("cover", {}, [&]() -> std::vector<core::Doc::Id> { d.put("master.cover", cover); return {"master.cover"}; });
        {
            auto pg = *d.as<core::Page>(d.pages().front());
            pg.master = "master.cover";
            j.run("p1", {d.pages().front()}, [&] { d.put(d.pages().front(), pg); });
        }
        check("its size: one step, its pages follow", D::set_master_size(j, d, "master.cover", 6.25 * kPt, 9.25 * kPt) &&
                                                       d.as<core::Page>(d.pages().front())->w == 6.25 * kPt &&
                                                       d.as<core::Page>(d.pages()[1])->w == 6 * kPt);
        check("  the same again: nothing", !D::set_master_size(j, d, "master.cover", 6.25 * kPt, 9.25 * kPt));
        {   // A page of its own size (a foldout) keeps it.
            auto pg = *d.as<core::Page>(d.pages()[3]);
            pg.w = 11.75 * kPt;
            j.run("fold", {d.pages()[3]}, [&] { d.put(d.pages()[3], pg); });
            D::set_master_size(j, d, body, 5.5 * kPt, 8.5 * kPt);
            check("  a page of its own size keeps it", d.as<core::Page>(d.pages()[3])->w == 11.75 * kPt &&
                                                       d.as<core::Page>(d.pages()[2])->w == 5.5 * kPt);
        }
        check("single cover, facing body: page 1 alone, then pairs", D::set_master_facing(j, d, "master.cover", false) &&
                                                                    d.side_of(d.pages().front()) == core::Side::Any &&
                                                                    core::spreads(d).front().size() == 1 && core::spreads(d)[1].size() == 2);
        check("bleed: each master's", D::set_master_bleed(j, d, "master.cover", 18) && core::bleed_of(d, d.pages().front()) == 18 &&
                                      core::bleed_of(d, d.pages()[1]) == n.bleed);
        {   // The PDF: the cover's sheet the larger bleed.
            const auto base = fs::temp_directory_path() / "composix_s147";
            fs::create_directories(base);
            const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
            L::PdfOptions o;
            o.compress = false;
            o.bleed = true;
            L::PdfReport rep;
            std::string err;
            const auto out = (base / "p.pdf").string();
            const bool ok = L::export_print_pdf(d, out, o, src, &rep, &err);
            std::ifstream in(out, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            char want[64];
            std::snprintf(want, sizeof want, "/BleedBox [0 0 %g %g]", 6.25 * kPt + 36, 9.25 * kPt + 36);
            check("  the print PDF: the cover sheet bled 1/4 in", ok && ss.str().find(want) != std::string::npos, err);
            std::error_code ec;
            if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
        }
        {   // A file from before: masters without bleed / facing take the document's.
            // (every "bleed" / "facing" key out -- never an object's first -- the document's too, so defaults then)
            const std::string js = std::regex_replace(core::to_json(d), std::regex(R"re(,\s*"(bleed|facing)"\s*:\s*[^,}\]]+)re"), "");
            core::Doc e;
            std::string err;
            check("old file: loads", core::from_json(e, js, &err), err);
            const auto* m = e.as<core::Master>("master.cover");
            check("  its masters: the document's bleed and facing", m && m->bleed == e.settings().bleed && m->facing == e.settings().facing);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && !d.as<core::Master>("master.cover"));
    }

    {
        std::cout << "\n-- masters before pages; a grid of any measure (s150) --\n";
        namespace D = core::doc;
        int im = -1, ip = -1, i = 0;
        for (const auto& s : core::segments()) {
            if (s.id == core::Segment::Masters) im = i;
            if (s.id == core::Segment::Pages) ip = i;
            ++i;
        }
        check("the navigator: Masters before Pages", im >= 0 && ip >= 0 && im < ip);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto mid = d.ids_of<core::Master>().front();
        auto grid_field = [&]() -> std::optional<core::Field> {
            const auto o = core::project(d, "");
            for (const auto& f : o.find(mid)->fields)
                if (f.edit == "mgrid") return f;
            return std::nullopt;
        };
        const auto g0 = grid_field();
        check("the grid: typed, its presets beside", g0 && g0->unit > 0 && g0->hi >= 72 && !g0->choices.empty());
        D::edit_masters(j, d, {mid}, "Baseline grid", [](core::Master& m) { m.grid = 13.5; });
        const auto g1 = grid_field();
        check("a measure not in the presets: kept and shown", d.as<core::Master>(mid)->grid == 13.5 && g1 && g1->value == "13.5 pt");
        j.undo();
        check("undo: the grid back", d.as<core::Master>(mid)->grid != 13.5);
    }

    {
        std::cout << "\n-- a facing master opens as its left / right spread (s150) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::Doc::Id fm, sm;
        for (const auto& m : d.ids_of<core::Master>()) {
            const auto* M = d.as<core::Master>(m);
            if (M->facing && M->folds.empty() && fm.empty()) fm = m;
        }
        check("the sample has a facing master", !fm.empty());
        const auto pl = core::layout_pages(d, {fm});
        check("two panels: Left then Right, meeting at the spine",
              pl.size() == 2 && pl[0].id == fm && pl[1].id == fm && pl[0].side == core::Side::Left &&
                  pl[1].side == core::Side::Right && std::abs(pl[0].r.r() - pl[1].r.x) < 1e-9 && pl[0].master && pl[1].master);
        D::set_master_facing(j, d, fm, false);
        check("single: one panel", core::layout_pages(d, {fm}).size() == 1 && core::layout_pages(d, {fm})[0].side == core::Side::Any);
        j.undo();
        // Its items: one for each side, one for both.
        const auto& M = *d.as<core::Master>(fm);
        const auto mk = [&](core::Side side, double x) {
            core::Frame f;
            f.owner = fm;
            f.kind = core::FrameKind::Shape;
            f.x = x; f.y = 20; f.w = 30; f.h = 30;
            f.side = side;
            const auto id = "frame.s150." + std::to_string(static_cast<int>(side));
            j.run("item", {}, [&]() -> std::vector<core::Doc::Id> { d.put(id, f); return {id}; });
            return id;
        };
        const auto fl = mk(core::Side::Left, 40), fr = mk(core::Side::Right, M.w - 70), fa = mk(core::Side::Any, M.w / 2 - 15);
        const auto pl2 = core::layout_pages(d, {fm});
        check("on the left panel: left and both, not right",
              core::on_panel(d, pl2[0], fl) && core::on_panel(d, pl2[0], fa) && !core::on_panel(d, pl2[0], fr));
        check("on the right panel: right and both, not left",
              core::on_panel(d, pl2[1], fr) && core::on_panel(d, pl2[1], fa) && !core::on_panel(d, pl2[1], fl));
        const double cy = 35;
        check("a both-sides item picked on either panel",
              core::hit_frame(d, pl2, pl2[0].r.x + M.w / 2, cy, 0) == fa && core::hit_frame(d, pl2, pl2[1].r.x + M.w / 2, cy, 0) == fa);
        check("a right item not picked where it would be on the left",
              core::hit_frame(d, pl2, pl2[0].r.x + M.w - 55, cy, 0).empty() && core::hit_frame(d, pl2, pl2[1].r.x + M.w - 55, cy, 0) == fr);
        const auto all = core::frames_in(d, pl2, pl2[0].r.unite(pl2[1].r));
        check("a marquee over the spread: each item once",
              std::count(all.begin(), all.end(), fa) == 1 && std::count(all.begin(), all.end(), fl) == 1 &&
                  std::count(all.begin(), all.end(), fr) == 1);
        check("the right item laid out on the right panel", core::frame_in_layout(d, pl2, fr).x == pl2[1].r.x + M.w - 70);
        const auto ml = core::margin_rect(d, fm, core::Side::Left), mr = core::margin_rect(d, fm, core::Side::Right);
        check("margins mirrored: the inside by the spine on both",
              std::abs(ml.x - M.margins.outside) < 1e-9 && std::abs(mr.x - M.margins.inside) < 1e-9 &&
                  std::abs((M.w - ml.r()) - M.margins.inside) < 1e-9);
        check("the master's own margins, no side: as before (a right page)",
              std::abs(core::margin_rect(d, fm).x - M.margins.inside) < 1e-9);
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0);
    }

    {
        std::cout << "\n-- a master's inspector holds its page settings (s148) --\n";
        namespace D = core::doc;
        using core::kPt;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto mid = d.ids_of<core::Master>().front();
        const auto o = core::project(d, "");
        std::map<std::string, std::string> mf, df;
        for (const auto& f : o.find(mid)->fields) mf[f.edit.empty() ? f.label : f.edit] = f.value;
        for (const auto& f : o.find("doc")->fields) df[f.edit.empty() ? f.label : f.edit] = f.value;
        check("the master: name, size, bleed, pages, columns, margins", mf.count("mname") && mf.count("msize") && mf.count("mbleed") &&
                                                                         mf.count("mfacing") && mf.count("mcolumns") && mf.count("mtop"));
        check("the document: no trim, bleed or pages; title, units, footnotes stay",
              !df.count("dtrim") && !df.count("dbleed") && !df.count("dfacing") && df.count("dtitle") && df.count("dunits") &&
                  df.count("dnote_restart"));
        check("the document item: \"Document\"", o.find("doc")->title == "Document");
        // A cover master 8 x 10: its page's size presets start from it; a page added with it takes its size.
        core::Master cover = *d.as<core::Master>(mid);
        cover.w = 8 * kPt;
        cover.h = 10 * kPt;
        j.run("cover", {}, [&]() -> std::vector<core::Doc::Id> { d.put("master.cover", cover); return {"master.cover"}; });
        const auto made = D::add_pages(j, d, d.pages().front(), 1, "master.cover");
        check("a page added with it: its size", made.size() == 1 && d.as<core::Page>(made[0])->w == 8 * kPt && d.as<core::Page>(made[0])->h == 10 * kPt);
        const auto pr = D::size_presets(d, made[0]);
        check("its size presets: from its master (8 x 10, foldouts of it)", pr.size() == 3 && pr[0].w == 8 * kPt && pr[1].folds.front() == 8 * kPt);
        check("rename: one step", D::edit_masters(j, d, {"master.cover"}, "Master name", [](core::Master& m) { m.name = "Cover"; }) &&
                                  d.as<core::Master>("master.cover")->name == "Cover");
    }

    {
        std::cout << "\n-- a master's mock-up (s149) --\n";
        namespace L = composix::layout;
        using core::kPt;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache fl;
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto body = std::string("master.2");
        check("its size: a facing master is a spread", std::abs(L::mockup_size(d, body).w - 2 * d.as<core::Master>(body)->w) < 1e-9);
        check("a foldout master: one sheet", [&] {
            for (const auto& mid : d.ids_of<core::Master>())
                if (!d.as<core::Master>(mid)->folds.empty()) return std::abs(L::mockup_size(d, mid).w - d.as<core::Master>(mid)->w) < 1e-9;
            return true;
        }());
        // Drawn: the margin's magenta where the right page's top margin is (A-Front: no guides over it).
        const int W = 432, H = 324;   // 2 x 6 in by 9 in at 36 px an inch: exact fit
        cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t* cr = cairo_create(img);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        const std::string front = "master.1";
        L::draw_master_mockup(cr, d, front, fl, src, 0, 0, W, H);
        cairo_surface_flush(img);
        const auto* m = d.as<core::Master>(front);
        const double sc = W / L::mockup_size(d, front).w;
        const int mx = static_cast<int>(W / 2 + (m->margins.inside + 20) * sc), my = static_cast<int>(std::round(m->margins.top * sc));
        const auto px = [&](int x, int y) {
            const auto* row = cairo_image_surface_get_data(img) + y * cairo_image_surface_get_stride(img);
            const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(row)[x];
            return v;
        };
        bool magenta = false;
        for (int dy = -1; dy <= 1; ++dy) {
            const auto v = px(mx, my + dy);
            const int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
            magenta = magenta || (r > 180 && g < 150 && b > 150);
        }
        check("drawn: the right page's top margin in magenta", magenta);
        cairo_destroy(cr);
        cairo_surface_destroy(img);
        // A master no page uses on one side (the bare document's left): drawn all the same.
        core::Doc bare;
        core::NewDoc n;
        n.pages = 1;
        n.text_frames = false;
        core::new_document(bare, n);
        cairo_surface_t* img2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 80);
        cairo_t* cr2 = cairo_create(img2);
        L::FlowCache f2;
        L::draw_master_mockup(cr2, bare, bare.ids_of<core::Master>().front(), f2, src, 0, 0, 100, 80);
        check("a side no page uses: drawn from a page made for it, nothing kept", bare.pages().size() == 1 && cairo_status(cr2) == CAIRO_STATUS_SUCCESS);
        cairo_destroy(cr2);
        cairo_surface_destroy(img2);
        {   // The tiles: masters, sections and the document show the mock-up.
            const auto o = core::project(d, "");
            check("tiles: a master's, a section's, the document's are its master's mock-up",
                  o.find(body)->thumb.page == "mockup:" + body && o.find("doc")->thumb.page.rfind("mockup:", 0) == 0 &&
                      o.find(d.ids_of<core::Section>().front())->thumb.page.rfind("mockup:", 0) == 0);
        }
    }


    // ── s171: colour libraries -- the formats, My Library, the system's palettes ──
    {
        std::cout << "--- s171 colour libraries ---\n";
        namespace SW = core::swatches;
        namespace D = core::doc;
        using CM = core::ColorModel;
        const auto near = [](std::uint32_t a, std::uint32_t b, int tol) {
            for (int sh : {16, 8, 0})
                if (std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF)) > tol) return false;
            return true;
        };
        check("Lab: white, black, mid grey", SW::lab_to_rgb(100, 0, 0) == 0xFFFFFF && SW::lab_to_rgb(0, 0, 0) == 0 &&
                                               near(SW::lab_to_rgb(50, 0, 0), 0x777777, 2));
        // ASE: written, read back -- CMYK, RGB, Gray, a spot, a name past ASCII.
        std::vector<core::Swatch> four{
            {"Teal", 0, CM::CMYK, {80, 0, 30, 10}},
            {"Brick", 0, CM::RGB, {170, 60, 40, 0}},
            {"Half", 0, CM::Gray, {50, 0, 0, 0}},
            {"Café Ü", 0, CM::Spot, {0, 60, 100, 0}, "Café Ü"}};
        for (auto& c : four) c.preview = D::swatch_preview(c);
        const std::string ase = SW::to_ase(four);
        check("ASE: sniffed", SW::format_of("x.bin", ase) == SW::Format::ASE);
        const auto a = SW::parse(ase, SW::Format::ASE);
        bool ase_ok = a && a->colors.size() == 4;
        for (std::size_t i = 0; ase_ok && i < 4; ++i) {
            const auto& g = a->colors[i];
            ase_ok = g.name == four[i].name && g.model == four[i].model;
            for (int k = 0; k < 4; ++k) ase_ok = ase_ok && std::abs(g.v[static_cast<std::size_t>(k)] - four[i].v[static_cast<std::size_t>(k)]) < 0.01;
        }
        check("ASE: CMYK, RGB, Gray and a spot back as they went", ase_ok);
        check("ASE: the spot keeps its ink", ase_ok && a->colors[3].spot == "Café Ü");
        check("ASE: cut short -- refused or what was whole", !SW::parse("ASEF", SW::Format::ASE) || SW::parse("ASEF", SW::Format::ASE)->colors.empty());
        {   // An ASE colour in Lab, by hand: L 0.5 (50), a 0, b 0.
            std::string b = "ASEF";
            const auto put = [&](std::uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) b += static_cast<char>((v >> (8 * i)) & 0xFF); };
            const auto f32 = [&](float f) { std::uint32_t v; std::memcpy(&v, &f, 4); put(v, 4); };
            put(1, 2); put(0, 2); put(3, 4);
            put(0xC001, 2); put(8, 4); put(3, 2); put('G', 2); put('1', 2); put(0, 2);   // a group
            put(1, 2); put(2 + 4 + 4 + 12 + 2, 4); put(2, 2); put('L', 2); put(0, 2);
            b += "LAB "; f32(0.5f); f32(0.f); f32(0.f); put(2, 2);
            put(0xC002, 2); put(0, 4);
            const auto lab = SW::parse(b, SW::Format::ASE);
            check("ASE: a Lab colour inside a group -- RGB, its look", lab && lab->colors.size() == 1 && lab->colors[0].model == CM::RGB &&
                                                                    lab->colors[0].name == "L" && near(lab->colors[0].preview, 0x777777, 2));
        }
        // GPL.
        const std::string gpl = "GIMP Palette\nName: Earth\nColumns: 4\n# a comment\n255   0   0\tRed\n 0 128 0 Leaf Green\n10 20 30\n";
        const auto g = SW::parse(gpl, SW::Format::GPL);
        check("GPL: sniffed, named, three colours", SW::format_of("p.txt", gpl) == SW::Format::GPL && g && g->name == "Earth" &&
                                                   g->colors.size() == 3 && g->colors[1].name == "Leaf Green" && g->colors[1].v[1] == 128 &&
                                                   g->colors[2].name == "#0A141E");
        {
            const auto back = SW::parse(SW::to_gpl("Mine", four), SW::Format::GPL);
            check("GPL: written -- the screen colours, named", back && back->name == "Mine" && back->colors.size() == 4 &&
                                                               back->colors[0].preview == four[0].preview && back->colors[3].name == four[3].name);
        }
        // LibreOffice .soc.
        const std::string soc = R"(<?xml version="1.0" encoding="UTF-8"?>
<ooo:color-table xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0">
<draw:color draw:name="Dark Red 2" draw:color="#a51d2d"/>
<draw:color draw:name="Salt &amp; Pepper" draw:color="#808080"/>
</ooo:color-table>)";
        const auto so = SW::parse(soc, SW::format_of("standard.soc", soc));
        check("SOC: two colours, entities decoded", so && so->colors.size() == 2 && so->colors[0].preview == 0xA51D2D &&
                                                     so->colors[1].name == "Salt & Pepper");
        // Scribus: old (CMYK="#..") and new (Space=) attributes, a spot.
        const std::string scx = R"(<?xml version="1.0"?><SCRIBUSCOLORS Name="Press">
<COLOR NAME="Cyan" CMYK="#ff000000" Spot="0"/>
<COLOR NAME="Web" RGB="#336699"/>
<COLOR Space="CMYK" NAME="Gold Ink" C="0" M="20" Y="80" K="10" Spot="1" Register="0"/>
</SCRIBUSCOLORS>)";
        const auto sc = SW::parse(scx, SW::format_of("press.xml", scx));
        check("Scribus: named, CMYK by hex and by parts, RGB, a spot", sc && sc->name == "Press" && sc->colors.size() == 3 &&
                                                                        sc->colors[0].model == CM::CMYK && std::abs(sc->colors[0].v[0] - 100) < 0.01 &&
                                                                        sc->colors[1].preview == 0x336699 && sc->colors[2].model == CM::Spot &&
                                                                        sc->colors[2].spot == "Gold Ink" && sc->colors[2].v[2] == 80);
        // Photoshop .aco: version 1 then version 2 (named).
        {
            std::string b;
            const auto put = [&](std::uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) b += static_cast<char>((v >> (8 * i)) & 0xFF); };
            const auto colours = [&] {
                put(0, 2); put(65535, 2); put(0, 2); put(0, 2); put(0, 2);           // RGB red
                put(2, 2); put(0, 2); put(65535, 2); put(65535, 2); put(65535, 2);   // CMYK: 100 C (inverted)
            };
            put(1, 2); put(2, 2); colours();
            put(2, 2); put(2, 2);
            put(0, 2); put(65535, 2); put(0, 2); put(0, 2); put(0, 2); put(4, 4); for (char c : std::string("Red")) put(static_cast<unsigned char>(c), 2); put(0, 2);
            put(2, 2); put(0, 2); put(65535, 2); put(65535, 2); put(65535, 2); put(5, 4); for (char c : std::string("Cyan")) put(static_cast<unsigned char>(c), 2); put(0, 2);
            const auto ac = SW::parse(b, SW::format_of("old.aco", b));
            check("ACO: version 2's names, RGB and inverted CMYK", ac && ac->colors.size() == 2 && ac->colors[0].name == "Red" &&
                                                                   ac->colors[0].preview == 0xFF0000 && ac->colors[1].name == "Cyan" &&
                                                                   std::abs(ac->colors[1].v[0] - 100) < 0.01 && ac->colors[1].v[1] < 0.01);
        }
        check("unknown bytes: refused", !SW::parse("hello", SW::format_of("x.txt", "hello")));
        // Files, and My Library (lossless).
        const auto tmp = std::filesystem::temp_directory_path() / ("cx_swatch_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(tmp);
        {
            const std::string f = (tmp / "out" / "set.ase").string();
            check("write ASE, read it: named by the file", SW::write_file(f, "Set", four, SW::Format::ASE) &&
                                                          SW::read_file(f) && SW::read_file(f)->name == "set" && SW::read_file(f)->colors.size() == 4);
            check("SOC isn't written", !SW::write_file((tmp / "x.soc").string(), "x", four, SW::Format::SOC) && !SW::writable(SW::Format::SOC));
            auto mine = SW::load_mine((tmp / "lib" / "my-library.json").string());
            check("My Library: missing is empty, its kind", mine.colors.empty() && mine.kind == SW::LibKind::Mine && mine.name == "My Library");
            auto tagged = four;
            tagged[0].from = "Earth";
            check("merge: four in", SW::merge_into(mine, tagged) == 4);
            auto other = four[1];
            other.v = {10, 10, 10, 0};
            check("merge: the same skipped, a taken name numbered", SW::merge_into(mine, {four[0], other}) == 1 &&
                                                                     mine.colors.back().name == "Brick 2");
            check("My Library: saved", SW::save_mine(mine));
            const auto again = SW::load_mine(mine.path);
            check("My Library: back as it was, where it came from too", again.colors == mine.colors && again.colors[0].from == "Earth");
        }
        {   // The system's palettes: GIMP 2 and 3 shipping one file once, Inkscape's, Scribus's (a folder down), a stray XML left out.
            const auto data = tmp / "share", home = tmp / "home";
            const auto mk = [](const std::filesystem::path& p, const std::string& text) {
                std::filesystem::create_directories(p.parent_path());
                std::ofstream(p) << text;
            };
            mk(data / "gimp" / "2.0" / "palettes" / "Web.gpl", gpl);
            mk(data / "gimp" / "3.0" / "palettes" / "Web.gpl", gpl);
            mk(data / "inkscape" / "palettes" / "Tango.gpl", gpl);
            mk(data / "scribus" / "swatches" / "Press" / "press.xml", scx);
            mk(data / "scribus" / "swatches" / "readme.xml", "<html/>");
            mk(home / ".config" / "GIMP" / "3.0" / "palettes" / "Mine.gpl", gpl);
            const auto found = SW::system_palettes(home.string(), {data.string()});
            std::vector<std::string> got;
            for (const auto& f : found) got.push_back(f.source + ":" + std::filesystem::path(f.path).filename().string());
            check("system palettes: each once, by source, palettes only",
                  got == std::vector<std::string>{"GIMP:Mine.gpl", "GIMP:Web.gpl", "Inkscape:Tango.gpl", "Scribus:press.xml"});
        }
        std::filesystem::remove_all(tmp);
        // Into the document: one step, from noted; again is nothing; a taken name numbered; undo.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::NewDoc n;
        n.pages = 1;
        core::new_document(d, n);
        const auto before = d.ids_of<core::Swatch>().size();
        const auto steps = j.size();
        const auto ids = D::add_library_swatches(j, d, {four[0], four[3], four[0]}, "Earth");
        check("library into the document: one step, two new, the repeat is the first",
              j.size() == steps + 1 && d.ids_of<core::Swatch>().size() == before + 2 && ids.size() == 3 && ids[0] == ids[2] &&
                  d.as<core::Swatch>(ids[0]) && d.as<core::Swatch>(ids[0])->from == "Earth" && d.as<core::Swatch>(ids[1])->model == CM::Spot);
        const auto again = D::add_library_swatches(j, d, {four[0]}, "Earth");
        check("library into the document: there already -- no step, its id", j.size() == steps + 1 && again[0] == ids[0]);
        auto teal2 = four[0];
        teal2.v = {0, 0, 0, 100};
        const auto renamed = D::add_library_swatches(j, d, {teal2}, "Other");
        check("library into the document: a taken name numbered", d.as<core::Swatch>(renamed[0])->name == "Teal 2" &&
                                                                  d.as<core::Swatch>(renamed[0])->from == "Other");
        {
            core::Doc e;
            check("from: saved with the document", core::from_json(e, core::to_json(d)) && e.as<core::Swatch>(ids[0]) &&
                                                   e.as<core::Swatch>(ids[0])->from == "Earth");
        }
        j.undo();
        j.undo();
        check("library into the document: undone", d.ids_of<core::Swatch>().size() == before);
        // s179: unused swatches, all at once (Black and Paper stay).
        {
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            const auto before = e.ids_of<core::Swatch>().size();
            const std::size_t steps = je.size();
            D::add_library_swatches(je, e, {four[0], four[1]}, "Earth");   // two new, unused
            const auto un = D::unused_swatches(e);
            bool fine = un.size() >= 2;
            for (const auto& id : un) {
                const auto* s = e.as<core::Swatch>(id);
                fine = fine && s->name != "Black" && s->name != "Paper" && D::swatch_uses(e, id) == 0;
            }
            check("unused swatches: found, never Black or Paper", fine);
            const int n = D::delete_unused_swatches(je, e);
            check("unused swatches: gone in one step", n == static_cast<int>(un.size()) && je.size() == steps + 2 &&
                  D::unused_swatches(e).empty() && e.ids_of<core::Swatch>().size() == before + 2 - un.size());
            check("unused swatches: none -- no step", D::delete_unused_swatches(je, e) == 0 && je.size() == steps + 2);
            je.undo();
            check("unused swatches: undone", D::unused_swatches(e).size() == un.size());
        }
        // s177: a picture's colours.
        {
            const int W = 100, H = 100;
            std::vector<std::uint32_t> px(W * H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)   // 60% sky (two near blues), 30% sand, 10% see-through
                    px[static_cast<std::size_t>(y * W + x)] = y < 60 ? (x % 2 ? 0xff3366ccu : 0xff3a6ad0u)
                                                             : y < 90 ? 0xffe0c080u : 0x00000000u;
            const auto pc = SW::picture_colors(px.data(), W, H, W);
            check("picture colours: the main ones, most first; near ones pooled; see-through not counted",
                  pc.size() == 2 && std::abs(pc[0].share - 60.0 / 90) < 0.01 && std::abs(pc[1].share - 30.0 / 90) < 0.01 &&
                  pc[1].rgb == 0xe0c080u && ((pc[0].rgb >> 16) & 0xff) >= 0x33 && ((pc[0].rgb >> 16) & 0xff) <= 0x3a);
            px[5] = 0xff00ff00u;   // one green pixel: under 1%
            check("picture colours: a speck left out", SW::picture_colors(px.data(), W, H, W).size() == 2);
            check("picture colours: at most asked", SW::picture_colors(px.data(), W, H, W, 1).size() == 1);
            std::vector<std::uint32_t> clear(16, 0u);
            check("picture colours: see-through all over -- none", SW::picture_colors(clear.data(), 4, 4, 4).empty());
            const auto pl = SW::picture_library("cove.png", pc);
            check("picture colours: a library of RGB swatches named by hex", pl.name == "cove.png" && pl.colors.size() == 2 &&
                  pl.colors[1].name == "#E0C080" && pl.colors[1].model == CM::RGB && pl.colors[1].preview == 0xe0c080u);
        }
        // s172: the Libraries window's shelf.
        {
            check("shelf: My Library's file", SW::mine_file("/d") == "/d/composix/swatches/my-library.json");
            core::AppSettings st;
            const bool a = SW::remember_imported(st, "/x/a.ase"), b = SW::remember_imported(st, "/x/b.gpl"),
                       again = SW::remember_imported(st, "/x/a.ase");
            check("shelf: imported remembered in order, once", a && b && !again &&
                  SW::imported(st) == std::vector<std::string>{"/x/a.ase", "/x/b.gpl"});
            check("shelf: forgotten", SW::forget_imported(st, "/x/a.ase") && !SW::forget_imported(st, "/x/a.ase") &&
                  SW::imported(st) == std::vector<std::string>{"/x/b.gpl"});
            SW::forget_imported(st, "/x/b.gpl");
            check("shelf: none left -- the setting goes", !st.count("colors.imported") && SW::imported(st).empty());
            check("shelf: save format by extension", SW::save_format("p.GPL") == SW::Format::GPL &&
                  SW::save_format("p.ase") == SW::Format::ASE && SW::save_format("p") == SW::Format::ASE &&
                  SW::save_format("p.json") == SW::Format::Composix);
            check("shelf: extension added when missing", SW::with_extension("/t/Earth", SW::Format::GPL) == "/t/Earth.gpl" &&
                  SW::with_extension("/t/Earth.ase", SW::Format::GPL) == "/t/Earth.ase" &&
                  SW::with_extension("/t/E.v2", SW::Format::ASE) == "/t/E.v2.ase");
            SW::Library lib;
            lib.colors = {four[0], four[1], four[2], four[3]};
            const int gone = SW::remove_at(lib, {3, 0, 3, 9});
            check("shelf: removed by place (repeats and strays ignored)", gone == 2 && lib.colors.size() == 2 &&
                  lib.colors[0].name == four[1].name && lib.colors[1].name == four[2].name);
            const auto dc = SW::document_colors(d);
            bool ordered = dc.size() == d.ids_of<core::Swatch>().size();
            for (std::size_t i = 1; i < dc.size(); ++i) ordered = ordered && dc[i - 1].pos <= dc[i].pos;
            check("shelf: the document's colours, in order", ordered && !dc.empty());
            // Out and back: the document's colours to .ase, then read as a library.
            const auto tmp2 = std::filesystem::temp_directory_path() / "cx_s172_shelf";
            std::filesystem::create_directories(tmp2);
            const auto out = SW::with_extension((tmp2 / "Doc").string(), SW::Format::ASE);
            const bool wrote = SW::write_file(out, "Doc", dc, SW::save_format(out));
            const auto back = SW::read_file(out);
            check("shelf: the document exported and read back", wrote && back && back->colors.size() == dc.size());
            std::filesystem::remove_all(tmp2);
        }
    }
    // ── s180: the eyedropper -- a colour taken from the page as it prints ──
    {
        std::cout << "--- s180 eyedropper ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        const auto box = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 20, 40, 30});
        D::edit_frames(j, d, {box}, "Fill", [&](core::Frame& f) { f.fill = sw; f.stroke.clear(); f.z = 1e6; });
        L::FlowCache flows;
        const auto want = d.as<core::Swatch>(sw)->preview;
        const auto near = [](std::uint32_t a, std::uint32_t b, int tol) {
            for (int sh : {16, 8, 0})
                if (std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF)) > tol) return false;
            return true;
        };
        const auto in = L::sample_page(d, page, flows, {}, 320, 35, 1);
        char hx[16];
        std::snprintf(hx, sizeof hx, "#%06X", in);
        check("eyedropper: inside a filled frame -- its colour", near(in, want, 2), hx);
        const auto tinted = [&] {
            D::edit_frames(j, d, {box}, "Fill tint", [](core::Frame& f) { f.fill_tint = 0.5; });
            const auto c = L::sample_page(d, page, flows, {}, 320, 35, 1);
            j.undo();
            return c;
        }();
        const auto half = [](std::uint32_t c, int sh) { return (static_cast<int>((c >> sh) & 0xFF) + 255) / 2; };
        check("eyedropper: a tint -- half way to paper",
              std::abs(static_cast<int>((tinted >> 16) & 0xFF) - half(want, 16)) <= 3 &&
              std::abs(static_cast<int>(tinted & 0xFF) - half(want, 0)) <= 3);
        const auto edge = L::sample_page(d, page, flows, {}, 300, 35, 2);   // half on the frame, half off
        check("eyedropper: across an edge -- the two averaged",
              ((edge >> 16) & 0xFF) > ((want >> 16) & 0xFF) && (edge & 0xFF) > (want & 0xFF) && (edge & 0xFF) < 0xF0);
        {   // a spot clear of every item on the page and its masters (either side of a spread)
            const auto* pg = d.as<core::Page>(page);
            auto owners = core::master_chain(d, page);
            owners.push_back(page);
            const auto clear = [&](double x, double y) {
                for (const auto& o : owners)
                    for (const auto& fid : d.frames_on(o)) {
                        const auto r = core::frame_rect(*d.as<core::Frame>(fid));
                        for (const double dx : {0.0, -pg->w, pg->w})
                            if (x > r.x + dx - 4 && x < r.r() + dx + 4 && y > r.y - 4 && y < r.b() + 4) return false;
                    }
                return true;
            };
            double cx = -1, cy = -1;
            for (double y = 2; pg && y < pg->h - 2 && cx < 0; y += 7)
                for (double x = 2; x < pg->w - 2; x += 7)
                    if (clear(x, y)) { cx = x; cy = y; break; }
            check("eyedropper: where nothing is -- paper", cx >= 0 && L::sample_page(d, page, flows, {}, cx, cy, 0.5) == 0xFFFFFFu);
        }
        check("eyedropper: nothing changed, no step", d.as<core::Frame>(box)->fill == sw);
    }
    // ── s189 (G3): a picture's clipping path ──
    {
        std::cout << "--- s189 clipping paths ---\n";
        using B = std::string;
        const auto be16 = [](unsigned v) { return B{static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; };
        const auto be32 = [&](unsigned v) { return be16(v >> 16) + be16(v & 0xFFFF); };
        const auto fx = [&](double v) { return be32(static_cast<unsigned>(static_cast<std::int32_t>(std::lround(v * 16777216.0)))); };
        // A closed subpath of straight corners (controls on the anchors).
        const auto subpath = [&](std::initializer_list<std::array<double, 2>> pts) {
            B s = be16(0) + be16(static_cast<unsigned>(pts.size())) + B(22, '\0');
            for (const auto& p : pts) {
                const B yx = fx(p[1]) + fx(p[0]);   // vertical, then horizontal
                s += be16(2) + yx + yx + yx;
            }
            return s;
        };
        const auto block = [&](unsigned id, const B& name, const B& data) {
            B s = B("8BIM") + be16(id) + static_cast<char>(name.size()) + name;
            if ((name.size() + 1) % 2) s += '\0';
            s += be32(static_cast<unsigned>(data.size())) + data;
            if (data.size() % 2) s += '\0';
            return s;
        };
        const auto pascal = [](const B& n) { return static_cast<char>(n.size()) + n; };
        const B tri = subpath({{0.5, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
        const B res = block(1005, "", B(16, '\0')) + block(2000, "Outline", be16(6) + B(24, '\0') + tri) +
                      block(2001, "Other", subpath({{0, 0}, {0.1, 0}, {0.1, 0.1}})) + block(2999, "", pascal("Outline") + be16(0x0100));
        const auto reader = [](const B& bytes) {
            return core::ReadAt([bytes](std::uint64_t off, std::size_t n) { return off >= bytes.size() ? B{} : bytes.substr(static_cast<std::size_t>(off), n); });
        };
        // JPEG: APP0, APP13 "Photoshop 3.0", a frame.
        const B app13 = B("Photoshop 3.0") + B(1, '\0') + res;
        const B jpeg = B{'\xFF', '\xD8', '\xFF', '\xE0'} + be16(16) + "JFIF" + B(10, '\0') + B{'\xFF', '\xED'} +
                       be16(static_cast<unsigned>(app13.size() + 2)) + app13 + B{'\xFF', '\xC0'} + be16(17) + B(15, '\0') + B{'\xFF', '\xD9'};
        const auto cj = core::clipping_path(reader(jpeg));
        bool tri_ok = cj.name == "Outline" && cj.polys.size() == 1 && !cj.polys[0].empty();
        if (tri_ok) {   // its points: the triangle's, flattened (the anchors among them)
            double top = 1, maxx = 0;
            for (const auto& p : cj.polys[0]) { top = std::min(top, p[1]); maxx = std::max(maxx, p[0]); }
            tri_ok = std::abs(top) < 1e-6 && std::abs(maxx - 1) < 1e-6;
        }
        check("clip path: from a JPEG's APP13 -- the one named as clipping, not the others", tri_ok);
        // PSD: its image resources.
        const B psd = B("8BPS") + be16(1) + B(6, '\0') + be16(3) + be32(10) + be32(10) + be16(8) + be16(3) + be32(0) +
                      be32(static_cast<unsigned>(res.size())) + res + be32(0) + be16(0) + B(300, '\0');
        check("clip path: from a PSD's resources", core::clipping_path(reader(psd)).polys.size() == 1);
        // TIFF (little-endian): IFD0 with tag 34377 pointing at the resources.
        const auto le16 = [](unsigned v) { return B{static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)}; };
        const auto le32 = [&](unsigned v) { return le16(v & 0xFFFF) + le16(v >> 16); };
        const B tiff = B("II") + le16(42) + le32(8) + le16(1) + le16(34377) + le16(7) + le32(static_cast<unsigned>(res.size())) + le32(26) + le32(0) + res;
        check("clip path: from a TIFF's tag 34377", core::clipping_path(reader(tiff)).name == "Outline");
        const B none = block(2000, "Outline", tri);   // a path, none marked for clipping
        check("clip path: paths but none for clipping -- none", core::clipping_path(reader(B{'\xFF', '\xD8', '\xFF', '\xED'} +
                                                       be16(static_cast<unsigned>(14 + none.size() + 2)) + "Photoshop 3.0" + B(1, '\0') + none +
                                                       B{'\xFF', '\xD9'})).polys.empty() &&
                                                   core::clipping_path(reader(B("not a picture"))).polys.empty());
        // The outline: narrow at the apex, the width at the foot; the picture's aspect.
        const auto sh = core::clip_shape(cj, 300, 200, 20);
        bool shape_ok = sh.size() == 1 + 2 * 20 && std::abs(sh[0] - 1.5f) < 1e-4;
        if (shape_ok) {
            const float top_w = sh[2] - sh[1], foot_w = sh[40] - sh[39];
            shape_ok = top_w < 0.1f && foot_w > 0.9f && std::abs((sh[1] + sh[2]) / 2 - 0.5f) < 0.03f;
        }
        check("clip path: its outline for the wrap -- the triangle, band by band", shape_ok);
        // A hole: a square in a square, even-odd -- the band through it spans the outer square.
        const B holed = block(2000, "Ring", be16(6) + B(24, '\0') + subpath({{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.1, 0.9}}) +
                                               subpath({{0.4, 0.4}, {0.6, 0.4}, {0.6, 0.6}, {0.4, 0.6}})) +
                        block(2999, "", pascal("Ring") + be16(0x0100));
        const auto ring = core::clipping_path(reader(B("8BPS") + be16(1) + B(6, '\0') + be16(3) + be32(10) + be32(10) + be16(8) + be16(3) +
                                                     be32(0) + be32(static_cast<unsigned>(holed.size())) + holed));
        const auto rs = core::clip_shape(ring, 100, 100, 10);
        check("clip path: two subpaths read; the outline spans the outer one",
              ring.polys.size() == 2 && rs.size() == 21 && std::abs(rs[11] - 0.1f) < 0.02f && std::abs(rs[12] - 0.9f) < 0.02f);
    }
    // ── s193 (G6): PDF/X-1a -- transparency flattened ──
    {
        std::cout << "--- s193 X-1a flattening ---\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        namespace fs = std::filesystem;
        std::string profile;
        for (const char* p : {"/usr/share/color/icc/GRACoL2013_CRPC6.icc", "/usr/share/color/icc/ghostscript/default_cmyk.icc",
                              "/usr/share/texlive/texmf-dist/tex/generic/colorprofiles/FOGRA39L_coated.icc"})
            if (fs::exists(p)) { profile = p; break; }
        if (profile.empty()) {
            std::cout << "  (no CMYK profile here -- the X-1a export checks skipped)\n";
        } else {
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            const auto page = d.as<core::Frame>("frame.1")->owner;
            const auto half = D::add_frame(j, d, page, core::FrameKind::Shape, {200, 40, 60, 60});
            D::edit_frames(j, d, {half}, "Half", [&](core::Frame& f) {
                for (const auto& sw : d.ids_of<core::Swatch>()) if (d.as<core::Swatch>(sw)->name == "Black") f.fill = sw;
                f.opacity = 0.5;
                f.z = 1e6;
            });
            const L::ImageSource src = [](const core::Asset& a, double, double) -> cairo_surface_t* {
                if (a.name == "harbor.psd") return nullptr;
                cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
                cairo_t* c = cairo_create(s);
                cairo_set_source_rgba(c, 0.2, 0.4, 0.6, a.name == "lighthouse.jpg" ? 0.5 : 1.0);   // the lighthouse: see-through
                cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
                cairo_paint(c);
                cairo_destroy(c);
                return s;
            };
            const auto base = fs::temp_directory_path() / "composix_selftest_s193";
            std::error_code ec;
            fs::create_directories(base, ec);
            const auto out = (base / "x1a.pdf").string();
            L::PdfOptions opt;
            opt.compress = false;
            opt.profile = profile;
            opt.convert = true;
            opt.standard = L::plate_standard::X1a;
            L::PdfReport rep;
            std::string err;
            const bool ok = L::export_print_pdf(d, out, opt, src, &rep, &err);
            const auto read = [&] { std::ifstream in(out, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
            const std::string pdf = read();
            check("X-1a: exports with transparency in it (was refused)", ok, err);
            check("X-1a: nothing see-through left in the file -- no ExtGState alpha, no SMask",
                  pdf.find("/ca ") == std::string::npos && pdf.find("/SMask") == std::string::npos);
            std::string flat;
            for (const auto& w : rep.warnings) if (w.find("flattened for PDF/X-1a") != std::string::npos) flat = w;
            check("X-1a: the report names what was flattened", flat.find("a frame at 50% opacity") != std::string::npos &&
                  flat.find("a see-through picture (lighthouse.jpg)") != std::string::npos, flat);
            check("X-1a: the flattened regions are pictures (DeviceCMYK, converted)", pdf.find("/ColorSpace /DeviceCMYK") != std::string::npos);
            opt.standard = L::plate_standard::X4;
            L::PdfReport r4;
            const bool ok4 = L::export_print_pdf(d, out, opt, src, &r4, &err);
            check("X-4: transparency kept live (nothing flattened)", ok4 && read().find("/ca 0.5") != std::string::npos &&
                  std::none_of(r4.warnings.begin(), r4.warnings.end(), [](const std::string& w) { return w.find("flattened") != std::string::npos; }), err);
            fs::remove_all(base, ec);
        }
    }
    // ── s190 (G4): hyphenation in more languages ──
    {
        std::cout << "--- s190 hyphenation languages ---\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        const auto hy = [](const char* w, const char* l) { return core::hyphenated(w, l); };
        check("de: Sil-ben-tren-nung; ß and umlauts are letters",
              hy("Silbentrennung", "de") == "Sil-ben-tren-nung" && hy("Stra\xC3\x9F" "enbahnhaltestelle", "de") == "Stra-\xC3\x9F" "en-bahn-hal-te-stel-le" &&
              hy("Fu\xC3\x9Fg\xC3\xA4ngerin", "de").find('-') != std::string::npos, hy("Stra\xC3\x9F" "enbahnhaltestelle", "de"));
        check("fr / es / it / nl / pt: their own patterns",
              hy("hyph\xC3\xA9nation", "fr") == "hy-ph\xC3\xA9-na-tion" && hy("murci\xC3\xA9lago", "es") == "mur-ci\xC3\xA9-la-go" &&
              hy("ipotenusa", "it") == "ipo-te-nu-sa" && hy("ziekenhuis", "nl") == "zie-ken-huis" && hy("computador", "pt") == "com-pu-ta-dor");
        check("en-GB and en-US differ (hy-phen-a-tion / hy-phen-ation)",
              hy("hyphenation", "en-GB") == "hy-phen-a-tion" && hy("hyphenation", "en-US") == "hy-phen-ation" && hy("hyphenation", "") == "hy-phen-ation");
        {   // break points are byte offsets at letter starts, never inside a UTF-8 letter
            const std::string w = "Fu\xC3\x9Fg\xC3\xA4nger\xC3\xBC" "berg\xC3\xA4nge";
            bool clean = true;
            for (int k : core::hyphen_points(w, -1, -1, "de")) clean = clean && (static_cast<unsigned char>(w[static_cast<std::size_t>(k)]) & 0xC0) != 0x80;
            check("de: breaks fall between letters, not inside one's bytes", clean && !core::hyphen_points(w, -1, -1, "de").empty());
        }
        check("fr: an apostrophe within the word (l'\xC3\xA9la-bo-ra-tion)", hy("l'\xC3\xA9laboration", "fr").find("bo-ra-tion") != std::string::npos);
        check("minimums: Portuguese 2 / 3, German 2 / 2", core::hyphen_language("pt").right == 3 && core::hyphen_language("de").right == 2 &&
                                                          std::string(core::hyphen_language("xx").code) == "en-US");
        check("not letters: no breaks", core::hyphen_points("x86-64", -1, -1, "de").empty() && core::hyphen_points("\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94", -1, -1, "fr").empty());
        // A style's Language, through the flow: a narrow frame of German compounds.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::string words;
        for (int k = 0; k < 10; ++k) words += "Stra\xC3\x9F" "enbahnhaltestelle Donaudampfschifffahrtsgesellschaft Fu\xC3\x9Fg\xC3\xA4nger\xC3\xBC" "berg\xC3\xA4nge ";
        words.pop_back();
        D::edit_text(j, d, "story.1", {2, 0}, {2, static_cast<int>(d.as<core::Story>("story.1")->paras[2].text.size())}, words);
        const auto sid = d.as<core::Story>("story.1")->paras[2].style;
        check("Language: a style setting (in the inspector under Hyphenate)", D::style_edit_label("lang") == "Language" &&
              D::edit_styles(j, d, {sid}, "Language", [&](core::Style& st) { D::style_edit(d, st, "lang", "de"); st.hyphenate = true; }) &&
              d.as<core::Style>(sid)->lang == "de");
        bool german_ok = true;
        int hyph = 0;
        const auto& txt = d.as<core::Story>("story.1")->paras[2].text;
        const auto flow = L::flow_story(d, "story.1");
        for (const auto& f : flow.frames)
            for (const auto& l : f.lines)
                if (l.para == 2 && l.hyphen) {
                    ++hyph;
                    // the break: one of the word's German points
                    int s = l.end;
                    while (s > 0 && txt[static_cast<std::size_t>(s - 1)] != ' ') --s;
                    int e = l.end;
                    while (e < static_cast<int>(txt.size()) && txt[static_cast<std::size_t>(e)] != ' ') ++e;
                    const auto pts = core::hyphen_points(std::string_view(txt).substr(static_cast<std::size_t>(s), static_cast<std::size_t>(e - s)), -1, -1, "de");
                    german_ok = german_ok && std::find(pts.begin(), pts.end(), l.end - s) != pts.end();
                }
        check("flow: German words break at German points (umlauts and ß inside them)", hyph > 0 && german_ok, std::to_string(hyph));
        core::Doc e;
        check("Language: saved with the style", core::from_json(e, core::to_json(d)) && e.as<core::Style>(sid)->lang == "de");
    }
    // ── s189: a picture drawn clipped to its path ──
    {
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::ClipPath diamond;
        diamond.polys.push_back({{0.5, 0.0}, {1.0, 0.5}, {0.5, 1.0}, {0.0, 0.5}});
        core::ImageFile f;
        f.path = "/x/clipped.jpg";
        f.name = "clipped.jpg";
        f.px_w = 400;
        f.px_h = 400;
        f.clip = core::pack_clip(diamond);
        const auto frame = D::add_frame(j, d, d.as<core::Frame>("frame.1")->owner, core::FrameKind::Image, {300, 20, 100, 100});
        D::edit_frames(j, d, {frame}, "Top", [](core::Frame& g) { g.z = 1e6; });
        check("clipped: placed, its path kept on the asset", D::place_image(j, d, frame, f) &&
              d.as<core::Asset>(d.as<core::Frame>(frame)->asset)->clip == f.clip);
        {
            core::Doc e;
            check("clipped: the path saved with the document", core::from_json(e, core::to_json(d)) &&
                  e.as<core::Asset>(d.as<core::Frame>(frame)->asset)->clip.size() == f.clip.size());
        }
        const L::ImageSource red = [](const core::Asset&, double, double) -> cairo_surface_t* {
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 4, 4);
            cairo_t* c = cairo_create(s);
            cairo_set_source_rgb(c, 1, 0, 0);
            cairo_paint(c);
            cairo_destroy(c);
            return s;
        };
        L::FlowCache flows;
        const auto page = d.as<core::Frame>(frame)->owner;
        const auto centre = L::sample_page(d, page, flows, red, 350, 70, 1), corner = L::sample_page(d, page, flows, red, 303, 23, 1);
        check("clipped: drawn inside its path, not in the frame's corners", (centre & 0xFFFFFF) == 0xFF0000u && (corner & 0xFFFFFF) == 0xFFFFFFu);
        const auto base = fs::temp_directory_path() / "composix_selftest_s189";
        std::error_code ec;
        fs::create_directories(base, ec);
        L::PdfOptions opt;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "clip.pdf").string();
        const bool ok = L::export_print_pdf(d, out, opt, red, &rep, &err);
        std::ifstream in(out, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        check("clipped: the print PDF clips it (even-odd) before the image", ok && ss.str().find(" l\nh\nW* n\n") != std::string::npos);
        fs::remove_all(base, ec);
    }
    // ── s188 (G2b): a vector picture's own size ──
    {
        std::cout << "--- s188 vector sizes ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::Doc::Id frame;
        for (const auto& fid : d.ids_of<core::Frame>())
            if (const auto* f = d.as<core::Frame>(fid); f->kind == core::FrameKind::Image && d.as<core::Page>(f->owner)) { frame = fid; break; }
        core::ImageFile f;
        f.path = "/x/map.svg";
        f.name = "map.svg";
        f.vector = true;
        f.px_w = 300;   // CSS pixels
        f.px_h = 200;
        check("vector: placed", !frame.empty() && D::place_image(j, d, frame, f));
        const auto* a = d.as<core::Asset>(d.as<core::Frame>(frame)->asset);
        check("vector: its own size kept, as points (96 px to the inch)",
              a && a->px_w == 0 && std::abs(a->vec_w - 225) < 1e-9 && std::abs(a->vec_h - 150) < 1e-9);
        const auto nat = a ? core::picture_natural(*a) : core::Size2{};
        check("vector: its natural size -- 3 : 2, not a square", std::abs(nat.w - 225) < 1e-9 && std::abs(nat.h - 150) < 1e-9);
        {   // Fit in a square frame: as wide as the frame, two thirds as tall
            auto g = *d.as<core::Frame>(frame);
            g.fit = core::ImageFit::Fit;
            const auto r = core::picture_rect({0, 0, 120, 120}, g, nat.w, nat.h);
            check("vector: Fit keeps its shape", std::abs(r.w - 120) < 1e-6 && std::abs(r.h - 80) < 1e-6);
        }
        core::Doc e;
        check("vector: its size saved", a && core::from_json(e, core::to_json(d)) && e.as<core::Asset>(d.as<core::Frame>(frame)->asset)->vec_w == a->vec_w);
        {   // placed at its own size: 100%
            const auto ns = core::natural_size(f, 0, 0);
            check("vector: placed at its own size (CSS px to points)", std::abs(ns.w - 225) < 1e-9 && std::abs(ns.h - 150) < 1e-9);
        }
        check("vector: its shape where pictures are placed (scale, offsets, the inspector)",
              a && core::picture_aspect(*a).w == a->vec_w && core::picture_aspect(*a).h == a->vec_h);
        core::Asset old;
        check("vector: placed before s188 -- square as it was", core::picture_natural(old).w == core::picture_natural(old).h &&
                                                               core::picture_aspect(old).w == 1 && core::picture_aspect(old).h == 1);
    }
    // ── s187 (G5c): a CMYK picture's own separations into the print PDF ──
    {
        std::cout << "--- s187 CMYK pictures to print ---\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        {   // the PSD decoder hands its separations (stored 255 - ink) as ink
            using B = std::string;
            const auto be16 = [](unsigned v) { return B{static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; };
            const auto be32 = [&](unsigned v) { return be16(v >> 16) + be16(v & 0xFFFF); };
            const B psd = B("8BPS") + be16(1) + B(6, '\0') + be16(4) + be32(1) + be32(1) + be16(8) + be16(4) + be32(0) + be32(0) +
                          be32(0) + be16(0) + B{'\x00', '\x7F', '\xFF', '\xE6'};   // C 100%, M ~50%, Y 0, K 10%
            int w = 0, h = 0;
            std::vector<std::uint8_t> ink;
            const auto px = L::psd_decode(psd, w, h, 0, nullptr, &ink);
            check("PSD: CMYK separations as ink", px.size() == 1 && ink == std::vector<std::uint8_t>{255, 128, 0, 25});
            std::vector<std::uint8_t> none{1};
            L::psd_decode(psd.substr(0, 24) + be16(3) + psd.substr(26, psd.size() - 26 - 4) + B{'\x10', '\x20', '\x30'}, w, h, 0, nullptr, &none);
            check("PSD: an RGB file gives none", none.empty());
        }
        core::Doc d;
        core::load_sample(d);
        const auto make = [](bool ink) {
            return L::ImageSource([ink](const core::Asset&, double, double) -> cairo_surface_t* {
                cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
                cairo_t* c = cairo_create(s);
                cairo_set_source_rgb(c, 0, 0.6, 0.9);
                cairo_paint(c);
                cairo_destroy(c);
                if (ink) {
                    auto* v = new std::vector<std::uint8_t>{200, 10, 0, 0, 200, 10, 0, 0, 200, 10, 0, 0, 200, 10, 0, 0};
                    cairo_surface_set_mime_data(s, L::kCmykMime, v->data(), v->size(),
                                                [](void* p) { delete static_cast<std::vector<std::uint8_t>*>(p); }, v);
                }
                return s;
            });
        };
        const auto base = fs::temp_directory_path() / "composix_selftest_s187";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto out = (base / "cmyk.pdf").string();
        L::PdfOptions opt;
        opt.compress = false;
        const auto run = [&](bool ink, std::string& pdf, L::PdfReport& rep) {
            std::string err;
            const auto file = ink ? out : out + ".rgb.pdf";
            const bool ok = L::export_print_pdf(d, file, opt, make(ink), &rep, &err);
            std::ifstream f(file, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            pdf = ss.str();
            return ok;
        };
        std::string pdf, pdf_rgb;
        L::PdfReport rep, rep_rgb;
        const bool ok = run(true, pdf, rep), ok2 = run(false, pdf_rgb, rep_rgb);
        const auto rgb_warned = [](const L::PdfReport& r) {
            return std::any_of(r.warnings.begin(), r.warnings.end(), [](const std::string& w) { return w.find("RGB images") != std::string::npos; });
        };
        const std::string pix{'\xC8', '\x0A', '\x00', '\x00', '\xC8', '\x0A', '\x00', '\x00'};
        const auto ink_back = [&] {   // the image's stream, inflated
            const auto at = pdf.find("/Width 2 /Height 2 /BitsPerComponent 8 /Interpolate true /ColorSpace /DeviceCMYK /Filter /FlateDecode");
            if (at == std::string::npos) return std::string{};
            const auto s0 = pdf.find("stream\n", at), s1 = pdf.find("\nendstream", at);
            if (s0 == std::string::npos || s1 == std::string::npos) return std::string{};
            std::string z = pdf.substr(s0 + 7, s1 - s0 - 7), raw(64, '\0');
            uLongf n = static_cast<uLongf>(raw.size());
            if (uncompress(reinterpret_cast<Bytef*>(raw.data()), &n, reinterpret_cast<const Bytef*>(z.data()), static_cast<uLong>(z.size())) != Z_OK) return std::string{};
            raw.resize(n);
            return raw;
        }();
        check("print: a picture's own CMYK goes in as DeviceCMYK, its ink as it was",
              ok && ink_back.size() == 16 && ink_back.compare(0, 8, pix) == 0, [&] { std::string h; char b[4]; for (unsigned char c : ink_back) { std::snprintf(b, sizeof b, "%02X ", c); h += b; } return h; }());
        check("print: no RGB warning for it (without the ink: RGB, warned)", !rgb_warned(rep) && ok2 && rgb_warned(rep_rgb));
        fs::remove_all(base, ec);
    }
    // ── s185 (G5b): a PSD's merged picture ──
    {
        std::cout << "--- s185 PSD ---\n";
        namespace L = composix::layout;
        using B = std::string;
        const auto be16 = [](unsigned v) { return B{static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; };
        const auto be32 = [&](unsigned v) { return be16(v >> 16) + be16(v & 0xFFFF); };
        // A PSD: header, colour mode data, no resources, the layer section (a
        // negative count when `alpha`), then the image data as given.
        const auto psd = [&](int ch, int w, int h, int depth, int mode, const B& cm, bool alpha, int comp, const B& data) {
            B s = B("8BPS") + be16(1) + B(6, '\0') + be16(static_cast<unsigned>(ch)) + be32(static_cast<unsigned>(h)) +
                  be32(static_cast<unsigned>(w)) + be16(static_cast<unsigned>(depth)) + be16(static_cast<unsigned>(mode));
            s += be32(static_cast<unsigned>(cm.size())) + cm + be32(0);
            if (alpha) s += be32(6) + be32(2) + be16(0xFFFF);   // layer info: count -1
            else s += be32(0);
            return s + be16(static_cast<unsigned>(comp)) + data;
        };
        const auto at = [](const std::vector<std::uint32_t>& px, int w, int x, int y) { return px[static_cast<std::size_t>(y * w + x)]; };
        int w = 0, h = 0;
        {   // RGB, raw, 2 x 1: red, blue
            const B data = B{'\xFF', '\x00'} + B{'\x00', '\x00'} + B{'\x00', '\xFF'};
            const auto px = L::psd_decode(psd(3, 2, 1, 8, 3, "", false, 0, data), w, h);
            check("PSD: RGB raw", w == 2 && h == 1 && px.size() == 2 && at(px, 2, 0, 0) == 0xFFFF0000u && at(px, 2, 1, 0) == 0xFF0000FFu);
        }
        {   // RGB + transparency, PackBits, 4 x 1: each channel's row a run of 4 (counts first)
            B counts, rows;
            const unsigned char v[4] = {0x40, 0x80, 0xC0, 0x80};   // R G B A
            for (int c = 0; c < 4; ++c) { counts += be16(2); rows += B{static_cast<char>(-3), static_cast<char>(v[c])}; }
            const auto px = L::psd_decode(psd(4, 4, 1, 8, 3, "", true, 1, counts + rows), w, h);
            const auto p = px.empty() ? 0u : px[3];
            check("PSD: PackBits, the merged picture's transparency (premultiplied)",
                  w == 4 && px.size() == 4 && (p >> 24) == 0x80 && ((p >> 16) & 0xFF) == 0x40 * 0x80 / 255);
            const auto opaque = L::psd_decode(psd(4, 4, 1, 8, 3, "", false, 1, counts + rows), w, h);
            check("PSD: an extra channel without the flag -- not transparency", !opaque.empty() && (opaque[0] >> 24) == 0xFF);
        }
        {   // CMYK, raw, 1 x 1: stored as 255 - ink -- 100% cyan
            const auto px = L::psd_decode(psd(4, 1, 1, 8, 4, "", false, 0, B{'\x00', '\xFF', '\xFF', '\xFF'}), w, h);
            check("PSD: CMYK (inverted ink) -- cyan on screen", px.size() == 1 && px[0] == 0xFF00FFFFu);
        }
        {   // Gray 16-bit, ZIP with prediction, 3 x 1: 0x1000, +0x1000, +0x1000
            const B raw = be16(0x1000) + be16(0x1000) + be16(0x1000);
            uLongf n = compressBound(static_cast<uLong>(raw.size()));
            B z(n, '\0');
            compress(reinterpret_cast<Bytef*>(z.data()), &n, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()));
            z.resize(n);
            const auto px = L::psd_decode(psd(1, 3, 1, 16, 1, "", false, 3, z), w, h);
            check("PSD: 16-bit gray, ZIP with prediction", px.size() == 3 && (px[0] & 0xFF) == 0x10 && (px[1] & 0xFF) == 0x20 && (px[2] & 0xFF) == 0x30);
        }
        {   // Indexed: index 1 -> the palette's second colour
            B pal(768, '\0');
            pal[1] = '\x11';
            pal[256 + 1] = '\x22';
            pal[512 + 1] = '\x33';
            const auto px = L::psd_decode(psd(1, 1, 1, 8, 2, pal, false, 0, B{'\x01'}), w, h);
            check("PSD: indexed, by its palette", px.size() == 1 && px[0] == 0xFF112233u);
        }
        {   // Bitmap, 8 x 1 in one byte: 1 = black
            const auto px = L::psd_decode(psd(1, 8, 1, 1, 0, "", false, 0, B{'\xF0'}), w, h);
            check("PSD: bitmap", px.size() == 8 && px[0] == 0xFF000000u && px[7] == 0xFFFFFFFFu);
        }
        {   // shrunk for the screen: 8 x 4 at most 2 a side -> every 4th pixel
            B data(8 * 4 * 3, '\x7F');
            const auto px = L::psd_decode(psd(3, 8, 4, 8, 3, "", false, 0, data), w, h, 2);
            check("PSD: shrunk on the way", w == 2 && h == 1 && px.size() == 2);
        }
        std::string why;
        check("PSD: header read", L::psd_info(psd(3, 640, 480, 8, 3, "", false, 0, "")).ok &&
                                  L::psd_info(psd(3, 640, 480, 8, 3, "", false, 0, "")).w == 640);
        check("PSD: refused, saying why -- PSB, 32-bit, cut short",
              L::psd_decode(B("8BPS") + be16(2) + B(40, '\0'), w, h, 0, &why).empty() && why.find("PSB") != B::npos &&
              L::psd_decode(psd(3, 1, 1, 32, 3, "", false, 0, B(12, '\0')), w, h, 0, &why).empty() && why.find("32") != B::npos &&
              L::psd_decode(psd(3, 4, 4, 8, 3, "", false, 0, B(5, '\0')), w, h, 0, &why).empty() && why == "cut short");
        check("PSD: its colour space from the header", core::picture_color(psd(4, 1, 1, 8, 4, "", false, 0, B(4, '\0'))) == "CMYK");
    }
    // ── s184 (G5): a picture's colour space from its header ──
    {
        std::cout << "--- s184 picture colour spaces ---\n";
        namespace D = core::doc;
        using B = std::string;
        const auto bytes = [](std::initializer_list<int> v) { B s; for (int c : v) s += static_cast<char>(c); return s; };
        const auto jpeg = [&](int sof, int comps, std::size_t app2) {
            B s = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10}) + "JFIF" + B(10, '\0');
            if (app2) {   // a big APP2 (an ICC profile) before the frame
                s += bytes({0xFF, 0xE2, static_cast<int>((app2 + 2) >> 8), static_cast<int>((app2 + 2) & 0xFF)});
                s += B(app2, 'x');
            }
            s += bytes({0xFF, 0xC4, 0x00, 0x04, 0x00, 0x00});   // a DHT first (not a frame)
            s += bytes({0xFF, sof, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, comps});
            return s + B(20, '\0');
        };
        check("JPEG: 4 components -- CMYK (past a big ICC segment and a DHT)", core::picture_color(jpeg(0xC0, 4, 60000)) == "CMYK");
        check("JPEG: 3 -- RGB; 1 (progressive) -- Gray",
              core::picture_color(jpeg(0xC0, 3, 0)) == "RGB" && core::picture_color(jpeg(0xC2, 1, 0)) == "Gray");
        const auto png = [&](int ct) { return bytes({0x89}) + "PNG" + bytes({13, 10, 26, 10, 0, 0, 0, 13}) + "IHDR" + B(8, '\1') + bytes({8, ct}) + B(10, '\0'); };
        check("PNG: grey / grey+alpha -- Gray; truecolour -- RGB",
              core::picture_color(png(0)) == "Gray" && core::picture_color(png(4)) == "Gray" && core::picture_color(png(6)) == "RGB");
        const auto psd = [&](int mode) { return B("8BPS") + bytes({0, 1}) + B(6, '\0') + bytes({0, 4, 0, 0, 1, 0, 0, 0, 1, 0, 0, 8, 0, mode}) + B(8, '\0'); };
        check("PSD: CMYK / Gray / Lab / RGB modes", core::picture_color(psd(4)) == "CMYK" && core::picture_color(psd(1)) == "Gray" &&
                                                    core::picture_color(psd(9)) == "Lab" && core::picture_color(psd(3)) == "RGB");
        const auto tiff = [&](bool le, int photometric) {
            const auto s16 = [&](int v) { return le ? bytes({v & 0xFF, v >> 8}) : bytes({v >> 8, v & 0xFF}); };
            const auto s32 = [&](int v) { return le ? bytes({v & 0xFF, (v >> 8) & 0xFF, 0, 0}) : bytes({0, 0, (v >> 8) & 0xFF, v & 0xFF}); };
            B s = le ? B("II") + s16(42) : B("MM") + s16(42);
            s += s32(8) + s16(2);   // IFD at 8, two entries
            s += s16(256) + s16(3) + s32(1) + s16(64) + s16(0);           // ImageWidth
            s += s16(262) + s16(3) + s32(1) + s16(photometric) + s16(0);  // PhotometricInterpretation
            return s + s32(0);
        };
        check("TIFF: separated -- CMYK, either byte order", core::picture_color(tiff(true, 5)) == "CMYK" && core::picture_color(tiff(false, 5)) == "CMYK");
        check("TIFF: RGB / black-is-zero", core::picture_color(tiff(true, 2)) == "RGB" && core::picture_color(tiff(false, 1)) == "Gray");
        check("GIF / BMP / WebP: RGB; not a picture: none",
              core::picture_color(B("GIF89a") + B(10, '\0')) == "RGB" && core::picture_color(B("BM") + B(30, '\0')) == "RGB" &&
              core::picture_color(B("RIFF") + B(4, '\0') + "WEBP" + B(8, '\0')) == "RGB" &&
              core::picture_color(B("hello, world")).empty() && core::picture_color(B{}).empty() &&
              core::picture_color(bytes({0xFF, 0xD8, 0xFF, 0xDA, 0, 2})).empty());
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::Doc::Id frame;
        for (const auto& fid : d.ids_of<core::Frame>())
            if (const auto* f = d.as<core::Frame>(fid); f->kind == core::FrameKind::Image && d.as<core::Page>(f->owner)) { frame = fid; break; }
        core::ImageFile f;
        f.path = "/x/press-ready.tif";
        f.name = "press-ready.tif";
        f.px_w = 3000;
        f.px_h = 2000;
        f.color = "CMYK";
        check("placed: the asset keeps CMYK", !frame.empty() && D::place_image(j, d, frame, f) &&
              d.as<core::Asset>(d.as<core::Frame>(frame)->asset)->color == "CMYK");
    }
    // ── s183 (G1): a paragraph split into sub-columns in a one-column frame ──
    {
        std::cout << "--- s183 split columns ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int P = 3;
        {   // a long plain paragraph P (many lines in any font)
            core::Doc::Loading load(d);
            auto s = *d.as<core::Story>("story.1");
            auto& para = s.paras.at(P);
            para.text.clear();
            for (int i = 0; i < 14; ++i) para.text += "The tide came in over the stones and went out again. ";
            para.runs.clear();
            para.notes.clear();
            para.refs.clear();
            para.anchors.clear();
            para.index.clear();
            para.brk = core::ParaStart::Anywhere;
            d.put("story.1", s);
        }
        const auto body = d.as<core::Story>("story.1")->paras.at(P).style;
        const auto sp = D::duplicate_style(j, d, body);
        check("split: a style setting -- Span columns: Split into 2",
              D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "s2"); st.start = core::ParaStart::Anywhere; }) &&
              d.as<core::Style>(sp)->split == 2 && !d.as<core::Style>(sp)->span);
        D::set_para_style(j, d, "story.1", {P, 0}, {P, 0}, sp);
        L::FlowCache fl;
        const L::FrameText* ft = nullptr;
        for (const auto& x : fl.get(d, "story.1").frames) for (const auto& l : x.lines) if (l.para == P) ft = &x;
        const auto* fr = ft ? d.as<core::Frame>(ft->frame) : nullptr;
        check("split: set, in a one-column frame", fr && core::column_count(*fr) == 1);
        if (fr) {
            const double x0 = core::column_x(*fr, 0), W = core::column_width(*fr, 0), g = d.as<core::Style>(sp)->split_gutter;
            const double sw = (W - g) / 2;
            int n0 = 0, n1 = 0;
            double top = 1e9, bot = -1, b0 = -1, b1 = -1;
            bool geo = true;
            int band = -1;
            for (const auto& l : ft->lines) {
                if (l.para != P) continue;
                top = std::min(top, l.y);
                bot = std::max(bot, l.y + l.h);
                (l.col == 0 ? n0 : n1)++;
                (l.col == 0 ? b0 : b1) = std::max(l.col == 0 ? b0 : b1, l.y + l.h);
                const double cx = x0 + l.col * (sw + g);
                geo = geo && l.col <= 1 && std::abs(l.cw - sw) < 0.01 && l.x >= cx - 0.01 && l.x < cx + sw;
                band = l.band;
            }
            check("split: two sub-columns, each half the column less the gutter", geo && n0 > 0 && n1 > 0);
            check("split: balanced -- the two within a line of each other", std::abs(n0 - n1) <= 1 && std::abs(b0 - b1) < 20);
            bool before = true, after = true, other_band = true;
            for (const auto& l : ft->lines) {
                if (l.para < P) before = before && l.y + l.h <= top + 0.01 && l.col == 0;
                if (l.para > P) after = after && l.y >= bot - 0.01 && l.col == 0;
                if (l.para != P) other_band = other_band && l.band != band;
            }
            check("split: the text before above it, the text after below it, one column", before && after && other_band);
            D::edit_styles(j, d, {sp}, "Split gutter", [&](core::Style& st) { D::style_edit(d, st, "split_gutter", "24"); });
            L::FlowCache fl2;
            double x1 = -1;
            for (const auto& x : fl2.get(d, "story.1").frames)
                for (const auto& l : x.lines) if (l.para == P && l.col == 1 && (x1 < 0 || l.x < x1)) x1 = l.x;
            check("split: the gutter widens", x1 >= x0 + (W - 24) / 2 + 24 - 0.01);
        }
        {   // a split paragraph's footnote: at the frame's foot, the column's full width
            std::string ns;
            int np = -1;
            for (const auto& sid : d.ids_of<core::Story>()) {
                const auto& ps = d.as<core::Story>(sid)->paras;
                for (int i = 0; i < static_cast<int>(ps.size()) && np < 0; ++i)
                    if (!ps[static_cast<std::size_t>(i)].notes.empty()) { ns = sid; np = i; }
                if (np >= 0) break;
            }
            bool ok = np >= 0;
            if (ok) {
                D::set_para_style(j, d, ns, {np, 0}, {np, 0}, sp);
                L::FlowCache fn;
                bool found = false;
                for (const auto& x : fn.get(d, ns).frames)
                    for (const auto& n : x.notes) {
                        if (n.para != np) continue;
                        const auto* f = d.as<core::Frame>(x.frame);
                        double low = 0;
                        for (const auto& l : x.lines) if (l.para == np) low = std::max(low, l.y + l.h);
                        found = true;
                        ok = ok && core::column_count(*f) == 1 && std::abs(n.w - core::column_width(*f, 0)) < 0.01 && n.y > low;
                    }
                ok = ok && found;
                j.undo();
            }
            check("split: its footnote at the frame's foot, the column's width", ok);
        }
        {
            core::Doc e;
            check("split: saved with the style",core::from_json(e, core::to_json(d)) && e.as<core::Style>(sp)->split == 2 &&
                                                 e.as<core::Style>(sp)->split_gutter == 24);
        }
        {   // a style based on it takes a change to the split; back Off clears it
            const auto kid = D::duplicate_style(j, d, sp);   // split like it: nothing its own
            const bool based = D::set_style_base(j, d, kid, sp);
            D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "s3"); });
            check("split: Split into 3 -- reaches the style based on it", based && d.as<core::Style>(kid)->based_on == sp &&
                                                                         d.as<core::Style>(kid)->split == 3);
            D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "0"); });
            check("split: Off", d.as<core::Style>(sp)->split == 0 && !d.as<core::Style>(sp)->span);
        }
        {   // the inspector offers it
            const auto o = core::project(d, "");
            bool offered = false;
            for (const auto& it : o.items())
                for (const auto& f : it.fields)
                    if (f.edit == "span")
                        for (const auto& ch : f.choices) offered = offered || ch.id == "s2";
            check("split: offered in the style's Span columns", offered);
        }
        {   // s191 (G1b): a two-column frame -- split WITHIN its column (no
            // picture wrapping into it, a paragraph short enough to balance)
            core::Doc::Loading load(d);
            for (const auto& fid : d.ids_of<core::Frame>()) {
                auto f = *d.as<core::Frame>(fid);
                if (f.wrap != core::Wrap::None) { f.wrap = core::Wrap::None; d.put(fid, f); continue; }
                if (f.story != "story.1") continue;
                f.columns = 2;
                d.put(fid, f);
            }
            auto s1 = *d.as<core::Story>("story.1");
            s1.paras.at(P).text.clear();
            for (int i = 0; i < 4; ++i) s1.paras.at(P).text += "The tide came in over the stones and went out again. ";
            d.put("story.1", s1);
        }
        D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "s2"); });
        L::FlowCache fl3;
        const auto& flow3 = fl3.get(d, "story.1");
        const L::FrameText* f3 = nullptr;
        for (const auto& x : flow3.frames) for (const auto& l : x.lines) if (l.para == P && !f3) f3 = &x;
        const auto* fr3 = f3 ? d.as<core::Frame>(f3->frame) : nullptr;
        check("in-column split: set, in a frame of two columns", fr3 && core::column_count(*fr3) == 2);
        if (fr3) {
            const double g = d.as<core::Style>(sp)->split_gutter;
            // which column it's in: the one holding its first line
            int home = -1;
            double top = 1e9, bot = -1;
            int n0 = 0, n1 = 0;
            for (const auto& l : f3->lines)
                if (l.para == P) {
                    top = std::min(top, l.y);
                    bot = std::max(bot, l.y + l.h);
                    (l.col == 0 ? n0 : n1)++;
                    for (int c = 0; c < 2 && home < 0; ++c)
                        if (l.x >= core::column_x(*fr3, c) - 0.01 && l.x < core::column_x(*fr3, c) + core::column_width(*fr3, c)) home = c;
                }
            const double cx = home >= 0 ? core::column_x(*fr3, home) : 0, cw = home >= 0 ? core::column_width(*fr3, home) : 0;
            const double sw = (cw - g) / 2;
            bool inside = home >= 0;
            for (const auto& l : f3->lines)
                if (l.para == P) inside = inside && std::abs(l.cw - sw) < 0.01 && l.x >= cx - 0.01 && l.x + 0.01 < cx + cw && l.band >= 1000;
            check("in-column split: two sub-columns inside its column, each half of it less the gutter", inside && n0 > 0 && n1 > 0);
            check("in-column split: balanced", std::abs(n0 - n1) <= 1, std::to_string(n0) + " / " + std::to_string(n1));
            bool above = true, below = true, below_any = false;
            for (const auto& l : f3->lines) {
                if (l.para == P || l.band >= 1000) continue;
                const bool same_col = std::abs(core::column_x(*fr3, l.col) - cx) < 0.01;
                if (!same_col) continue;
                if (l.para < P) above = above && l.y + l.h <= top + 0.01;
                if (l.para > P) { below = below && l.y >= bot - 0.01; below_any = true; }
            }
            check("in-column split: in its column the text before is above it, the text after below it", above && below);
            (void)below_any;
            // A click in its second sub-column lands in it (column-first hit-testing).
            const L::Line* second = nullptr;
            for (const auto& l : f3->lines) if (l.para == P && l.col == 1 && !second) second = &l;
            if (second) {
                const auto at = L::hit_text(d, "story.1", flow3, f3->frame, second->x + 4, second->y + second->h / 2);
                check("in-column split: a click in its second sub-column lands there", at.para == P && at.byte >= second->start && at.byte <= second->end,
                      std::to_string(at.para) + ":" + std::to_string(at.byte));
            }
        }
    }
    // ── s181 (C1b): a vector picture's colours, as written ──
    {
        std::cout << "--- s181 SVG colours ---\n";
        namespace SW = core::swatches;
        check("css: #rgb, #rrggbb, #rrggbbaa", SW::css_color("#f80") == 0xFF8800u && SW::css_color(" #1A2b3C ") == 0x1A2B3Cu &&
                                                SW::css_color("#11223380") == 0x112233u && SW::css_color("#f808") == 0xFF8800u);
        check("css: rgb() numbers and percentages, rgba()", SW::css_color("rgb(255, 0, 10)") == 0xFF000Au &&
              SW::css_color("rgb(100%,50%,0%)") == 0xFF8000u && SW::css_color("rgba(0 128 255 / 0.5)") == 0x0080FFu);
        check("css: hsl()", SW::css_color("hsl(120, 100%, 25%)") == 0x008000u && SW::css_color("hsl(0,0%,100%)") == 0xFFFFFFu);
        check("css: names, any case", SW::css_color("Teal") == 0x008080u && SW::css_color("rebeccapurple") == 0x663399u &&
                                      SW::css_color("aliceblue") == 0xF0F8FFu && SW::css_color("yellowgreen") == 0x9ACD32u);
        check("css: not colours", !SW::css_color("none") && !SW::css_color("currentColor") && !SW::css_color("url(#g1)") &&
                                  !SW::css_color("transparent") && !SW::css_color("inherit") && !SW::css_color("#12") &&
                                  !SW::css_color("#ggg") && !SW::css_color(""));
        const std::string svg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <style>.sea { fill: #1E6091; stroke:none } .sand{fill:rgb(233,196,106)!important}</style>
  <defs><linearGradient id="g1"><stop offset="0" stop-color="#1E6091"/><stop offset="1" style="stop-color:white;stop-opacity:1"/></linearGradient></defs>
  <rect class="sea" width="100" height="60"/>
  <rect fill="url(#g1)" fill-opacity="0.5" data-fill="#ABCDEF" width="10" height="10"/>
  <path fill="#1e6091" d="M0 0"/>
  <circle style="fill:#e63946 icc-color(FOGRA39, 0, 0.85, 0.7, 0.05);stroke:#000" r="5"/>
  <text fill="none" stroke="currentColor" color="crimson">fill: lots</text>
</svg>)svg";
        const auto vc = SW::svg_colors(svg);
        const auto find = [&](std::uint32_t rgb, bool cmyk) -> const SW::VectorColor* {
            for (const auto& c : vc) if (c.rgb == rgb && c.cmyk == cmyk) return &c;
            return nullptr;
        };
        const auto* sea = find(0x1E6091u, false);
        check("svg: attributes, style=, <style> and stops all read; the most used first",
              sea && sea->uses == 3 && &vc.front() == sea && find(0xE9C46Au, false) && find(0xFFFFFFu, false) && find(0x000000u, false) &&
              find(0xDC143Cu, false));
        const auto* red = find(0xE63946u, true);
        check("svg: an icc-color gives its CMYK (the fallback for the screen)",
              red && red->v[0] == 0 && red->v[1] == 85 && red->v[2] == 70 && red->v[3] == 5 && !find(0xE63946u, false));
        check("svg: none / url() / currentColor / data-fill / fill-opacity / text not taken",
              !find(0xABCDEFu, false) && vc.size() == 6);
        check("svg: at most asked", SW::svg_colors(svg, 2).size() == 2);
        check("svg: nothing drawn in colour -- none", SW::svg_colors("<svg><rect fill=\"none\"/></svg>").empty());
        const auto lib = SW::vector_library("map.svg", vc);
        bool named = lib.name == "map.svg" && lib.colors.size() == vc.size();
        for (const auto& s : lib.colors)
            named = named && (s.model == core::ColorModel::CMYK ? s.name == "C0 M85 Y70 K5" : s.name.size() == 7 && s.name[0] == '#');
        check("svg: a library -- RGB by hex, CMYK by its values", named && lib.colors.front().name == "#1E6091");
    }
    // ── s197: shapes -- rectangle, oval, line, path ──────────────────────
    {
        std::cout << "--- s197 shapes ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto ops_str = [](const core::Frame& f) {
            std::string s;
            for (const auto& o : D::outline_ops(f)) s += o.op;
            return s;
        };
        const std::string before = core::to_json(d);
        const auto oval = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 20, 40, 30}, 1, 12, false, core::ShapeKind::Oval);
        const auto* of = d.as<core::Frame>(oval);
        check("oval: drawn, one step", of && of->shape == core::ShapeKind::Oval && j.undo_label() == "Draw oval");
        check("oval: four curves, closed", of && ops_str(*of) == "MCCCCZ" && D::has_outline(*of));
        {
            const auto ops = D::outline_ops(*of);
            check("oval: its top and right on the box", ops[0].v[0] == 320 && ops[0].v[1] == 20 && ops[1].v[4] == 340 && ops[1].v[5] == 35);
        }
        core::Frame rect;
        rect.kind = core::FrameKind::Shape;
        rect.x = 1; rect.y = 2; rect.w = 3; rect.h = 4;
        check("rect: its box, four lines", ops_str(rect) == "MLLLZ" && !D::has_outline(rect));
        // A level line: no height at all.
        const auto line = D::add_path(j, d, page, {D::corner_point(100, 300), D::corner_point(250, 300)}, true);
        const auto* lf = d.as<core::Frame>(line);
        check("line: drawn though it has no height", lf && lf->h == 0 && lf->w == 150 && lf->open && j.undo_label() == "Draw line");
        check("line: a move and a line, open", lf && ops_str(*lf) == "ML" && !lf->stroke.empty());
        // A curve: (0,0) to (100,0), both handles 50 up -- its top is 3/4 of that.
        std::vector<core::PathPoint> arc{{200, 400, 200, 400, 200, 350, false}, {300, 400, 300, 350, 300, 400, false}};
        const auto b = D::path_bounds(arc, true);
        check("path: bounds are the curve's, not its handles'", std::abs(b.y - 362.5) < 0.1 && b.h > 37.4 && b.h < 37.6 && b.w == 100);
        const auto cur = D::add_path(j, d, page, arc, true);
        const auto* cf = d.as<core::Frame>(cur);
        check("path: one step, 'Draw path'", cf && j.undo_label() == "Draw path" && ops_str(*cf) == "MC");
        {
            const auto back = D::path_points(*cf);
            bool same = back.size() == 2;
            for (std::size_t i = 0; same && i < 2; ++i)
                same = std::abs(back[i].x - arc[i].x) < 1e-9 && std::abs(back[i].y - arc[i].y) < 1e-9 &&
                       std::abs(back[i].ox - arc[i].ox) < 1e-9 && std::abs(back[i].oy - arc[i].oy) < 1e-9 &&
                       std::abs(back[i].iy - arc[i].iy) < 1e-9;
            check("path: its points come back where they were drawn", same);
        }
        // Resized: the path scales with its frame.
        D::edit_frames(j, d, {cur}, "Size", [](core::Frame& f) { f.w *= 2; });
        check("path: resizing the frame scales it", std::abs(D::path_points(*d.as<core::Frame>(cur))[1].x - 400) < 1e-9);
        j.undo();
        check("path: too few points, or no size -- nothing",
              D::add_path(j, d, page, {D::corner_point(5, 5)}, true).empty() &&
              D::add_path(j, d, page, {D::corner_point(5, 5), D::corner_point(5.2, 5.2)}, true).empty());
        // Rect <-> oval.
        const auto box = D::add_frame(j, d, page, core::FrameKind::Shape, {60, 500, 50, 50});
        check("shape kind: a rectangle made an oval", D::set_shape_kind(j, d, {box, cur}, core::ShapeKind::Oval) &&
              d.as<core::Frame>(box)->shape == core::ShapeKind::Oval && d.as<core::Frame>(cur)->shape == core::ShapeKind::Path &&
              j.undo_label() == "Make oval");
        check("shape kind: a path isn't chosen", !D::set_shape_kind(j, d, {box}, core::ShapeKind::Path));
        // JSON.
        {
            core::Doc e;
            std::string err;
            const auto text = core::to_json(d);
            check("shapes: through JSON exactly", core::from_json(e, text, &err) && core::to_json(e) == text &&
                  *e.as<core::Frame>(cur) == *d.as<core::Frame>(cur), err);
        }
        // Drawn as it prints: an oval's corner is paper, its middle its colour.
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        D::edit_frames(j, d, {oval}, "Fill", [&](core::Frame& f) { f.fill = sw; f.stroke.clear(); f.z = 1e6; });
        L::FlowCache flows;
        const auto mid = L::sample_page(d, page, flows, {}, 320, 35, 1);
        const auto corner = L::sample_page(d, page, flows, {}, 301.5, 21.5, 1);
        check("oval: drawn round -- its middle filled, its corner not", mid == d.as<core::Swatch>(sw)->preview && corner != mid);
        // Print: curves in the PDF.
        {
            namespace fs = std::filesystem;
            const auto base = fs::temp_directory_path() / "composix_selftest_shapes";
            std::error_code ec;
            fs::create_directories(base, ec);
            const auto out = (base / "shapes.pdf").string();
            L::PdfOptions opt;
            opt.compress = false;
            L::PdfReport rep;
            std::string err;
            const bool ok = L::export_print_pdf(d, out, opt, {}, &rep, &err);
            std::string pdf;
            { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); pdf = ss.str(); }
            check("print: the oval and the curve as curves", ok && pdf.find(" c\n") != std::string::npos, err);
            fs::remove_all(base, ec);
        }
        while (j.can_undo()) j.undo();
        check("shapes: all undone exactly", core::to_json(d) == before);
    }
    // ── s198: the Eyedropper tool -- a colour onto fills / strokes ────────
    {
        std::cout << "--- s198 eyedropper tool ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto a = D::add_frame(j, d, page, core::FrameKind::Shape, {60, 60, 40, 40});
        const auto b = D::add_frame(j, d, page, core::FrameKind::Shape, {120, 60, 40, 40}, 1, 12, false, core::ShapeKind::Oval);
        const auto nsw = d.ids_of<core::Swatch>().size();
        const auto sw = D::take_colour_onto(j, d, {a, b}, 0x3A7D44u, false);
        check("eyedropper: a new colour -- a swatch by its hex, both fills, one step",
              d.ids_of<core::Swatch>().size() == nsw + 1 && d.as<core::Swatch>(sw)->name == "#3A7D44" &&
              d.as<core::Frame>(a)->fill == sw && d.as<core::Frame>(b)->fill == sw && j.undo_label() == "Fill from page");
        const auto sw2 = D::take_colour_onto(j, d, {a}, 0x3B7D44u, true);
        check("eyedropper: a step away -- the same swatch, onto the stroke",
              sw2 == sw && d.as<core::Frame>(a)->stroke == sw && d.ids_of<core::Swatch>().size() == nsw + 1 &&
              j.undo_label() == "Stroke from page");
        const auto black = d.as<core::Frame>(b)->stroke;   // a new shape's
        const auto bc = D::screen_rgb(*d.as<core::Swatch>(black));
        const auto steps = j.undo_label();
        check("eyedropper: an existing swatch's colour -- that swatch, no new one",
              D::take_colour_onto(j, d, {}, bc, false) == black && j.undo_label() == steps);
        const auto sw3 = D::take_colour_onto(j, d, {}, 0x112233u, false);
        check("eyedropper: nothing picked -- just the swatch", !sw3.empty() && j.undo_label() == "New swatch #112233" &&
              d.as<core::Frame>(a)->fill == sw);
        check("eyedropper: already that colour -- no step", D::take_colour_onto(j, d, {a, b}, 0x3A7D44u, false) == sw &&
              j.undo_label() == "New swatch #112233");
        while (j.can_undo()) j.undo();
        check("eyedropper: all undone exactly", core::to_json(d) == before);
    }
    // ── s200: Direct on a shape's points; a shape's own wrap ─────────────
    {
        std::cout << "--- s200 path points ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto box = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 100, 80, 60});
        bool open = true;
        auto P = D::shape_points(*d.as<core::Frame>(box), open);
        check("points: a rectangle's four corners, closed", P.size() == 4 && !open && P[2].x == 180 && P[2].y == 160);
        // A point added on the top edge, halfway: still the same outline.
        const auto h = D::outline_nearest(*d.as<core::Frame>(box), 140, 99);
        check("points: the nearest place on the outline", h.k == 0 && std::abs(h.t - 0.5) < 0.02 && std::abs(h.dist - 1) < 0.01);
        check("points: added -- the rectangle a path of five, same box",
              D::path_add_point(j, d, box, h.k, 0.5) && j.undo_label() == "Add point" &&
              d.as<core::Frame>(box)->shape == core::ShapeKind::Path && d.as<core::Frame>(box)->path.size() == 5 &&
              core::frame_rect(*d.as<core::Frame>(box)) == core::Rect{100, 100, 80, 60});
        // Dragged up 20: the box grows to hold it.
        P = D::shape_points(*d.as<core::Frame>(box), open);
        P[1].y -= 20; P[1].iy -= 20; P[1].oy -= 20;
        D::frames_begin(j, d, {box}, "Move point");
        D::edit_frames(j, d, {box}, "Edit path", [&](core::Frame& f) { D::set_path(f, P, open); });
        D::frames_end(j);
        check("points: a point moved -- one step, the box follows", j.undo_label() == "Move point" &&
              d.as<core::Frame>(box)->y == 80 && d.as<core::Frame>(box)->h == 80);
        check("points: smooth -- handles along its neighbours' line", D::path_toggle_point(j, d, box, 1) &&
              j.undo_label() == "Smooth point" && [&] {
                  const auto Q = D::path_points(*d.as<core::Frame>(box));
                  return Q[1].smooth && std::abs(Q[1].iy - 80) < 1e-6 && Q[1].ix < 140 && Q[1].ox > 140;
              }());
        check("points: back to a corner", D::path_toggle_point(j, d, box, 1) && j.undo_label() == "Corner point" &&
              !D::path_points(*d.as<core::Frame>(box))[1].smooth);
        check("points: one taken out", D::path_delete_point(j, d, box, 1) && d.as<core::Frame>(box)->path.size() == 4 &&
              j.undo_label() == "Delete point" && d.as<core::Frame>(box)->y == 100);
        D::path_delete_point(j, d, box, 0);
        check("points: a closed path keeps three", !D::path_delete_point(j, d, box, 0) && d.as<core::Frame>(box)->path.size() == 3);
        // An oval split on a curve keeps its look.
        const auto ov = D::add_frame(j, d, page, core::FrameKind::Shape, {200, 300, 100, 60}, 1, 12, false, core::ShapeKind::Oval);
        const auto polys0 = D::outline_polys(*d.as<core::Frame>(ov));
        D::path_add_point(j, d, ov, 0, 0.3);
        const auto* of = d.as<core::Frame>(ov);
        bool same = of->path.size() == 5 && std::abs(of->w - 100) < 0.01 && std::abs(of->h - 60) < 0.01;
        const auto polys1 = D::outline_polys(*of);
        for (const auto& q : polys1.front()) {   // every point still on the ellipse
            const double ex = (q.x - 250) / 50, ey = (q.y - 330) / 30;
            same = same && std::abs(std::hypot(ex, ey) - 1) < 0.01;
        }
        check("points: an oval's curve split -- the same ellipse", same && !polys0.empty());
        // Wrap to the shape: an oval pushes text by its own outline.
        {
            core::Frame o;
            o.kind = core::FrameKind::Shape;
            o.shape = core::ShapeKind::Oval;
            o.x = 0; o.y = 0; o.w = 100; o.h = 60;
            const auto bands = D::has_outline(o) ? core::outline_bands(o, d, 3) : std::vector<core::Band>{};
            bool narrow_top = !bands.empty() && bands.front().x0 > 20 && bands.front().x1 < 80;
            bool wide_mid = false;
            for (const auto& b : bands) if (b.y0 <= 30 && b.y1 >= 30) wide_mid = b.x0 < 1 && b.x1 > 99;
            check("wrap: an oval's bands -- narrow at its top, full at its middle", narrow_top && wide_mid && bands.size() == 20);
        }
        {   // ... and in place: over a text frame, set to Shape, it pushes by bands
            const auto* t = d.as<core::Frame>("frame.1");
            const auto o2 = D::add_frame(j, d, t->owner, core::FrameKind::Shape, {t->x + 20, t->y + 20, 80, 60}, 1, 12, false,
                                         core::ShapeKind::Oval);
            D::edit_frames(j, d, {o2}, "Text wrap", [](core::Frame& f) { f.wrap = core::Wrap::Shape; });
            const auto rs = core::wrap_rects(d, "frame.1");
            check("wrap: a shape set to Shape pushes text by its outline", rs.size() > 10, std::to_string(rs.size()));
        }
        while (j.can_undo()) j.undo();
        check("points: all undone exactly", core::to_json(d) == before);
    }
    // ── s202 (C5): real swatch files -- written by Illustrator, Photoshop, KDE, SwatchBooker ──
    {
        std::cout << "--- s202 real swatch files ---\n";
        namespace SW = core::swatches;
        const std::string dir = std::string(COMPOSIX_SOURCE_DIR) + "/third_party/swatch-samples/";
        const auto rd = [&](const char* f) { return SW::read_file(dir + f); };
        const auto has = [](const SW::Library& l, const char* name, std::uint32_t rgb) {
            for (const auto& c : l.colors) if (c.name == name && (c.preview & 0xFFFFFFu) == rgb) return true;
            return false;
        };
        const auto sampler = rd("nsfmc-sampler.ase");
        check("ase (Illustrator): RGB and Gray in a group, all five", sampler && sampler->colors.size() == 5 &&
              has(*sampler, "lablab", 0xC80032) && has(*sampler, "orange", 0xF1A321) && has(*sampler, "grey", 0xF1F1F1));
        const auto sol = rd("nsfmc-solarized.ase");
        bool spots = sol && sol->colors.size() == 26;
        if (sol) for (const auto& c : sol->colors) spots = spots && c.model == core::ColorModel::Spot && c.spot == c.name;
        check("ase (Illustrator): Lab spot colours -- Solarized's own hex", spots && has(*sol, "Base 03", 0x002B36) &&
              has(*sol, "Base 1", 0x93A1A1));
        const auto herbs = rd("hrbrmstr-herbs_and_spice.ase");
        check("ase (Adobe Color): unnamed colours named by their hex, whole numbers",
              herbs && herbs->colors.size() == 5 && herbs->colors[0].name == "#5A1F00" && herbs->colors[0].v[1] == 31);
        const auto tne = rd("hrbrmstr-tomorrow_night_eighties.aco");
        check("aco (Photoshop): v1 + v2, the names from v2, whole numbers", tne && tne->colors.size() == 12 &&
              has(*tne, "Tomorrow-Night-Eighties-Red", 0xF2777A) && tne->colors[0].v[0] == 45);
        const auto ps = rd("szydlovski-swatches.aco");
        check("aco (Photoshop): the three as the author's test has them", ps && ps->colors.size() == 3 &&
              has(*ps, "Hearty Red", 0xD41E2A) && has(*ps, "Luscious Pink", 0xFF8CCB) && has(*ps, "Deep Navy", 0x285094));
        const auto kde = rd("hrbrmstr-fourty.colors");
        check("kde palette: forty colours, blank lines between groups", kde && kde->colors.size() == 40 &&
              kde->format == SW::Format::KDE && has(*kde, "Almost black", 0x303030));
        const auto lab = rd("dtpstudio-CIE-LAB.sbz");
        check("sbz (SwatchBooker, Scribus ships it): unzipped, 986 Lab colours in the book's order, its title",
              lab && lab->colors.size() == 986 && lab->name == "CIE-LAB" && lab->colors[0].name == "Lab 95 00 20" &&
              has(*lab, "Lab 95 00 00", 0xF1F1F1));
        // Scribus as its own files write it (Shades of K, the Registration colour, HKS spots).
        const std::string scribus = R"(<?xml version="1.0" encoding="UTF-8"?>
<SCRIBUSCOLORS Name="Shades">
    <COLOR NAME="Registration" SPACE="CMYK" CMYK="#ffffffff" Register="1"/>
    <COLOR NAME="1 %" SPACE="CMYK" CMYK="#00000003" K="1"/>
    <COLOR RGB="#ffb600" NAME="HKS 5 K" Spot="1" />
</SCRIBUSCOLORS>)";
        const auto sc = SW::parse(scribus, SW::format_of("x.xml", scribus));
        check("scribus: Registration left out; K=\"1\" exact (not 3/255); a spot",
              sc && sc->colors.size() == 2 && sc->colors[0].v[3] == 1 && sc->colors[1].model == core::ColorModel::Spot);
    }
    // ── s203: a line or a path picked by its outline, not its box ─────────
    {
        std::cout << "--- s203 shape hit ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto under = D::add_frame(j, d, page, core::FrameKind::Shape, {250, 100, 60, 60});
        const auto line = D::add_path(j, d, page, {D::corner_point(100, 100), D::corner_point(300, 300)}, true);
        const auto ov = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 400, 100, 60}, 1, 12, false, core::ShapeKind::Oval);
        const auto pl = core::layout_pages(d, {page});
        const double ox = pl.front().r.x, oy = pl.front().r.y;
        const auto hit = [&](double x, double y) { return core::hit_frame(d, pl, ox + x, oy + y, 2); };
        check("hit: on the diagonal line -- the line", hit(200, 201) == line);
        check("hit: in its box, off its stroke -- the frame under it", hit(280, 120) == under);
        check("hit: an oval's middle -- the oval; its box's corner -- not", hit(150, 430) == ov && hit(101, 401) != ov);
        check("hit: a level line (no height) -- by its stroke", [&] {
            const auto lv = D::add_path(j, d, page, {D::corner_point(60, 600), D::corner_point(200, 600)}, true);
            return hit(130, 601) == lv && hit(130, 606) != lv;
        }());
        while (j.can_undo()) j.undo();
    }
    // ── s204: the stroke's style -- ends, corners, dashes, arrowheads ────
    {
        std::cout << "--- s204 stroke style ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto ln = D::add_path(j, d, page, {D::corner_point(100, 40), D::corner_point(300, 40)}, true);
        D::edit_frames(j, d, {ln}, "End", [](core::Frame& f) { f.end_b = 1; f.end_a = 2; f.stroke_w = 2; f.z = 1e6; });
        const auto* f = d.as<core::Frame>(ln);
        const auto ops = D::stroke_ops(*f);
        check("arrow: the line stops short of the tip", ops.size() == 2 && ops[1].v[0] < 300 && ops[1].v[0] > 290 && ops[0].v[0] == 100);
        const auto ends = D::end_shapes(*f);
        check("ends: a circle at the start, an arrow (tip on the end) at the end",
              ends.size() == 2 && ends[0].size() == 32 && ends[1].size() == 3 && ends[1][0].x == 300 && ends[1][1].x < 300);
        check("dash: dashed 3:2 of the weight, dotted 0:2", [&] {
            core::Frame g = *f;
            g.dash = 1;
            const auto a = D::dash_pattern(g);
            g.dash = 2;
            const auto b = D::dash_pattern(g);
            return a == std::vector<double>{6, 4} && b == std::vector<double>{0, 4} && D::dash_pattern(*f).empty();
        }());
        check("ends: none on a closed shape", [&] {
            core::Frame g = *f;
            g.open = false;
            return D::end_shapes(g).empty();
        }());
        // As it prints: the arrow's middle in the stroke's colour (black), past where the line stops.
        L::FlowCache flows;
        const auto at = L::sample_page(d, page, flows, {}, 297, 40, 0.5);
        check("arrow: drawn (its tip area dark)", ((at >> 16) & 0xFF) < 80, std::to_string(at));
        {
            D::edit_frames(j, d, {ln}, "Dash", [](core::Frame& g) { g.dash = 1; g.cap = 1; g.join = 2; });
            core::Doc e;
            std::string err;
            const auto text = core::to_json(d);
            check("stroke style: through JSON", core::from_json(e, text, &err) && core::to_json(e) == text && e.as<core::Frame>(ln)->dash == 1 &&
                  e.as<core::Frame>(ln)->end_b == 1, err);
            namespace fs = std::filesystem;
            const auto base = fs::temp_directory_path() / "composix_selftest_strokes";
            std::error_code ec;
            fs::create_directories(base, ec);
            const auto out = (base / "s.pdf").string();
            L::PdfOptions opt;
            opt.compress = false;
            L::PdfReport rep;
            const bool ok = L::export_print_pdf(d, out, opt, {}, &rep, &err);
            std::string pdf;
            { std::ifstream in(out, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); pdf = ss.str(); }
            check("print: round ends, bevel corners, a 6 4 dash", ok && pdf.find("1 J\n2 j\n[6 4] 0 d\n") != std::string::npos, err);
            fs::remove_all(base, ec);
        }
        while (j.can_undo()) j.undo();
    }
    // ── s205: rounded corners; a saved web page isn't a swatch file ────────
    {
        std::cout << "--- s205 rounded corners ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace SW = core::swatches;
        std::string err;
        check("swatches: a saved web page -- said so", !SW::parse("<!DOCTYPE html><html lang=\"en\">", SW::Format::Unknown, &err) &&
              err.find("web page") != std::string::npos, err);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto box = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 20, 60, 40});
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        D::edit_frames(j, d, {box}, "Round", [&](core::Frame& f) { f.radius = 12; f.fill = sw; f.stroke.clear(); f.z = 1e6; });
        const auto* f = d.as<core::Frame>(box);
        std::string ops;
        for (const auto& o : D::outline_ops(*f)) ops += o.op;
        check("round: four lines, four quarter-curves", ops == "MLCLCLCLCZ" && !D::has_outline(*f));
        core::Frame big = *f;
        big.radius = 500;
        check("round: the radius held to half the short side", D::outline_ops(big)[0].v[0] == 320);
        L::FlowCache flows;
        const auto mid = L::sample_page(d, page, flows, {}, 330, 40, 1), corner = L::sample_page(d, page, flows, {}, 301, 21, 1);
        check("round: drawn -- the middle filled, the corner paper", mid == d.as<core::Swatch>(sw)->preview && corner != mid);
        core::Doc e;
        const auto text = core::to_json(d);
        check("round: through JSON", core::from_json(e, text, &err) && e.as<core::Frame>(box)->radius == 12 && core::to_json(e) == text, err);
        while (j.can_undo()) j.undo();
    }
    // ── s206: polygon / star (as Curvz's) ─────────────────────────────────
    {
        std::cout << "--- s206 polygon ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto hex = D::polygon_points(100, 100, 50, 6, 1, -M_PI / 2);
        check("polygon: six corners, the first straight up", hex.size() == 6 && std::abs(hex[0].x - 100) < 1e-9 && std::abs(hex[0].y - 50) < 1e-9 &&
              std::abs(hex[3].y - 150) < 1e-9);
        const double ps = D::perfect_star(5);
        check("star: a five-point star's straight-sided depth (0.382)", std::abs(ps - 0.381966) < 1e-5 && D::perfect_star(4) < 0);
        const auto star = D::polygon_points(0, 0, 10, 5, ps, -M_PI / 2);
        check("star: ten points, inner ones at the depth", star.size() == 10 && std::abs(std::hypot(star[1].x, star[1].y) - 10 * ps) < 1e-9);
        const auto id = D::add_polygon(j, d, page, 200, 200, 40, 5, ps, -M_PI / 2);
        const auto* f = d.as<core::Frame>(id);
        check("star: drawn -- a closed path of ten, one step", f && f->shape == core::ShapeKind::Path && !f->open && f->path.size() == 10 &&
              j.undo_label() == "Draw star");
        check("polygon: drawn -- 'Draw polygon'", !D::add_polygon(j, d, page, 300, 300, 30, 8, 1, 0).empty() && j.undo_label() == "Draw polygon");
        check("polygon: too small -- nothing", D::add_polygon(j, d, page, 300, 300, 0.5, 8, 1, 0).empty());
        while (j.can_undo()) j.undo();
    }
    // ── s207: Direct on several points ────────────────────────────────────
    {
        std::cout << "--- s207 several points ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto hex = D::add_polygon(j, d, page, 200, 200, 50, 6, 1, -M_PI / 2);
        const auto before = D::path_points(*d.as<core::Frame>(hex));
        const auto t0 = std::chrono::steady_clock::now();
        D::path_move_points(j, d, hex, {0, 3}, 0, -10, "nudge:t", t0);
        D::path_move_points(j, d, hex, {0, 3}, 0, -10, "nudge:t", t0 + std::chrono::milliseconds(100));
        const auto after = D::path_points(*d.as<core::Frame>(hex));
        check("points: two nudged together, twice -- one step", std::abs(after[0].y - (before[0].y - 20)) < 1e-9 &&
              std::abs(after[3].y - (before[3].y - 20)) < 1e-9 && std::abs(after[1].y - before[1].y) < 1e-9 && j.undo_label() == "Move points");
        j.undo();
        check("points: undone in one", std::abs(D::path_points(*d.as<core::Frame>(hex))[0].y - before[0].y) < 1e-9);
        check("points: three taken out together", D::path_delete_points(j, d, hex, {1, 2, 4}) && d.as<core::Frame>(hex)->path.size() == 3 &&
              j.undo_label() == "Delete points");
        check("points: not if fewer than three would be left", !D::path_delete_points(j, d, hex, {0, 1}));
        while (j.can_undo()) j.undo();
    }
    // ── s208: the Pen carries on from an open path's end ──────────────────
    {
        std::cout << "--- s208 extend path ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto ln = D::add_path(j, d, page, {{100, 100, 100, 100, 130, 80, true}, D::corner_point(200, 100)}, true);
        auto P = D::path_points(*d.as<core::Frame>(ln));
        const auto R = D::reversed_path(P);
        check("reversed: the order and each point's handles swapped", R.size() == 2 && R[1].x == 100 && R[1].ix == 130 && R[1].iy == 80 &&
              D::reversed_path(R) == P);
        P.push_back(D::corner_point(200, 200));
        check("extend: a third point, one step", D::set_path_points(j, d, ln, P, true, "Extend path") &&
              d.as<core::Frame>(ln)->path.size() == 3 && d.as<core::Frame>(ln)->open && j.undo_label() == "Extend path");
        check("close: the same points, closed", D::set_path_points(j, d, ln, P, false, "Close path") && !d.as<core::Frame>(ln)->open &&
              j.undo_label() == "Close path");
        check("unchanged: no step", !D::set_path_points(j, d, ln, P, false, "Close path"));
        while (j.can_undo()) j.undo();
    }
    // ── s209: any frame shaped -- text kept inside its outline ────────────
    {
        std::cout << "--- s209 shaped frames ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        check("shaped: a text frame is a rectangle to start", !D::has_outline(*d.as<core::Frame>("frame.1")));
        D::edit_frames(j, d, {"frame.1"}, "Oval", [](core::Frame& f) { f.shape = core::ShapeKind::Oval; });
        const auto* t = d.as<core::Frame>("frame.1");
        const auto rs = core::wrap_rects(d, "frame.1");
        int left = 0, right = 0, gaps = 0;
        for (const auto& r : rs) { left += r.x0 < 0 && !r.jump; right += r.x1 > t->w && !r.jump; gaps += r.jump; }
        check("shaped: an oval text frame -- outside its outline wrapped, both sides",
              D::has_outline(*d.as<core::Frame>("frame.1")) && left > 10 && right > 10, std::to_string(left) + " " + std::to_string(right) + " " + std::to_string(gaps));
        L::FlowCache flows;
        const auto& fl = flows.get(d, d.as<core::Frame>("frame.1")->story);
        double widest = 0, first_w = 0;
        bool first = true;
        for (const auto& ft : fl.frames)
            for (const auto& ln : ft.lines) {
                const double w = ln.w > 0 ? ln.w : L::line_width_of(*d.as<core::Frame>(ft.frame), ln);
                if (first) { first_w = w; first = false; }
                widest = std::max(widest, w);
            }
        check("shaped: its first line narrower than its widest", !first && first_w < widest - 20,
              std::to_string(first_w) + " / " + std::to_string(widest));
        // A picture frame made a star keeps its kind (set_path no longer makes it a shape).
        core::Frame pic = *d.as<core::Frame>("frame.11");
        D::set_path(pic, D::polygon_points(pic.x + pic.w / 2, pic.y + pic.h / 2, 40, 5, D::perfect_star(5), -M_PI / 2), false);
        check("shaped: a picture frame stays a picture frame, with an outline", pic.kind == core::FrameKind::Image && D::has_outline(pic));
        while (j.can_undo()) j.undo();
    }
    // ── s210: Content -- a shape made a text or picture frame, and back ──
    {
        std::cout << "--- s210 content ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto star = D::add_polygon(j, d, page, 200, 200, 60, 5, D::perfect_star(5), -M_PI / 2);
        check("content: a star made a text frame -- its story made, its outline kept",
              D::set_frame_content(j, d, {star}, core::FrameKind::Text) && d.as<core::Frame>(star)->kind == core::FrameKind::Text &&
              d.as<core::Story>(d.as<core::Frame>(star)->story) && d.as<core::Frame>(star)->path.size() == 10 &&
              j.undo_label() == "Content: Text");
        check("content: an empty text frame back to none -- its story gone", [&] {
            const auto st = d.as<core::Frame>(star)->story;
            return D::set_frame_content(j, d, {star}, core::FrameKind::Shape) && !d.as<core::Story>(st) &&
                   d.as<core::Frame>(star)->story.empty();
        }());
        check("content: a text frame with words keeps them (refused)", !D::set_frame_content(j, d, {"frame.1"}, core::FrameKind::Shape));
        check("content: a shape made a picture frame", D::set_frame_content(j, d, {star}, core::FrameKind::Image) &&
              d.as<core::Frame>(star)->kind == core::FrameKind::Image && j.undo_label() == "Content: Picture");
        check("shape kind: a text frame made an oval", D::set_shape_kind(j, d, {"frame.1"}, core::ShapeKind::Oval) &&
              d.as<core::Frame>("frame.1")->shape == core::ShapeKind::Oval && d.as<core::Frame>("frame.1")->kind == core::FrameKind::Text);
        bool open = true;
        check("points: a text frame's corners for Direct", D::shape_points(*d.as<core::Frame>("frame.2"), open).size() == 4 && !open);
        while (j.can_undo()) j.undo();
        check("content: all undone exactly", core::to_json(d) == before);
    }
    // ── s211: compound paths -- holes ────────────────────────────────────
    {
        std::cout << "--- s211 compound paths ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto outer = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 20, 60, 60});
        const auto hole = D::add_frame(j, d, page, core::FrameKind::Shape, {315, 35, 30, 30}, 1, 12, false, core::ShapeKind::Oval);
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        D::edit_frames(j, d, {outer}, "Fill", [&](core::Frame& f) { f.fill = sw; f.stroke.clear(); f.z = 1e6; });
        const auto c = D::make_compound(j, d, {outer, hole});
        const auto* f = d.as<core::Frame>(c);
        check("compound: one path of two subpaths (4 corners + 4 oval points), the other frame gone",
              c == outer && f->path.size() == 8 && f->subs == std::vector<int>{4} && !d.as<core::Frame>(hole) &&
              j.undo_label() == "Make compound path");
        std::string ops;
        for (const auto& o : D::outline_ops(*f)) ops += o.op;
        check("compound: two closed outlines", ops == "MLLLLZMCCCCZ", ops);
        L::FlowCache flows;
        const auto ring = L::sample_page(d, page, flows, {}, 305, 25, 1), mid = L::sample_page(d, page, flows, {}, 330, 50, 1);
        check("compound: the ring filled, the hole paper", ring == d.as<core::Swatch>(sw)->preview && mid != ring);
        const auto pl = core::layout_pages(d, {page});
        check("compound: the hole isn't the shape (hit)", core::hit_frame(d, pl, pl.front().r.x + 330, pl.front().r.y + 50, 0.5) != c);
        // Direct in a compound: a point added in the hole stays in the hole.
        const auto h = D::outline_nearest(*d.as<core::Frame>(c), 340.6, 39.4);
        check("compound: nearest -- on the hole's subpath", h.k >= 4 && h.dist < 1, std::to_string(h.k));
        D::path_add_point(j, d, c, h.k, h.t);
        check("compound: a point added there -- the hole has five", d.as<core::Frame>(c)->path.size() == 9 && d.as<core::Frame>(c)->subs == std::vector<int>{4},
              std::to_string(d.as<core::Frame>(c)->path.size()) + " k=" + std::to_string(h.k) + " t=" + std::to_string(h.t));
        check("compound: the hole's points taken out -- it goes, the square stays",
              D::path_delete_points(j, d, c, {4, 5, 6}) && d.as<core::Frame>(c)->path.size() == 4 && d.as<core::Frame>(c)->subs.empty());
        j.undo();
        const auto parts = D::release_compound(j, d, c);
        check("release: two paths again, styled alike", parts.size() == 2 && d.as<core::Frame>(parts[1])->fill == sw &&
              d.as<core::Frame>(parts[0])->subs.empty() && d.as<core::Frame>(parts[1])->path.size() == 5 && j.undo_label() == "Release compound path");
        {
            j.undo();
            core::Doc e;
            std::string err;
            const auto text = core::to_json(d);
            check("compound: through JSON", core::from_json(e, text, &err) && e.as<core::Frame>(c)->subs == std::vector<int>{4} && core::to_json(e) == text, err);
        }
        while (j.can_undo()) j.undo();
        check("compound: all undone exactly", core::to_json(d) == before);
    }
    // ── s213: shapes combined (Clipper2), curves fitted back ──────────────
    {
        std::cout << "--- s213 pathfinder ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto pl = core::layout_pages(d, {page});
        const double ox = pl.front().r.x, oy = pl.front().r.y;
        const auto inside = [&](const std::string& id, double x, double y) { return D::shape_hit(*d.as<core::Frame>(id), x, y, 0); };
        (void)ox; (void)oy;
        // Two squares overlapping: one L-ish outline of eight corners.
        auto a = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 100, 60, 60});
        auto b = D::add_frame(j, d, page, core::FrameKind::Shape, {130, 130, 60, 60});
        const auto u = D::path_op(j, d, {a, b}, D::PathOp::Unite);
        const auto* uf = d.as<core::Frame>(u);
        check("unite: two squares -- one path of 8 corners, the other gone, one step", uf && uf->path.size() == 8 && uf->subs.empty() &&
              !d.as<core::Frame>(u == a ? b : a) && j.undo_label() == "Unite shapes" &&
              core::frame_rect(*uf) == core::Rect{100, 100, 90, 90}, uf ? std::to_string(uf->path.size()) : "none");
        check("unite: its corners straight lines, corners", uf && std::all_of(uf->path.begin(), uf->path.end(), [](const core::PathPoint& p) {
                  return !p.smooth && p.ix == p.x && p.ox == p.x; }));
        j.undo();
        // A circle's outline comes back a handful of curves, on the circle.
        const auto c1 = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 300, 100, 100}, 1, 12, false, core::ShapeKind::Oval);
        const auto c2 = D::add_frame(j, d, page, core::FrameKind::Shape, {360, 300, 100, 100}, 1, 12, false, core::ShapeKind::Oval);
        const auto cu = D::path_op(j, d, {c1, c2}, D::PathOp::Unite);
        {
            const auto* f = d.as<core::Frame>(cu);
            bool on = f != nullptr;
            const auto polys = f ? D::outline_polys(*f) : std::vector<std::vector<D::XY>>{};
            for (const auto& poly : polys)
                for (const auto& q : poly) {
                    const double r1 = std::hypot(q.x - 350, q.y - 350), r2 = std::hypot(q.x - 410, q.y - 350);
                    on = on && std::min(std::abs(r1 - 50), std::abs(r2 - 50)) < 0.6;
                }
            check("unite: two circles -- a few curves (not a hundred lines), every point on one circle or the other",
                  on && f->path.size() <= 16 && f->path.size() >= 4, f ? std::to_string(f->path.size()) : "none");
        }
        j.undo();
        j.undo();
        j.undo();
        // A square minus a circle inside it: a hole.
        const auto sq = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 300, 100, 100});
        const auto ci = D::add_frame(j, d, page, core::FrameKind::Shape, {125, 325, 50, 50}, 1, 12, false, core::ShapeKind::Oval);
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        D::edit_frames(j, d, {sq}, "Fill", [&](core::Frame& f) { f.fill = sw; });
        const auto s = D::path_op(j, d, {ci, sq}, D::PathOp::Subtract);
        check("subtract: the backmost (the square) minus the circle -- a compound path with a hole, the square's fill",
              s == sq && !d.as<core::Frame>(ci) && d.as<core::Frame>(s)->subs.size() == 1 && d.as<core::Frame>(s)->fill == sw &&
              inside(s, 105, 305) && !inside(s, 150, 350) && j.undo_label() == "Subtract shapes");
        j.undo();
        const auto in = D::path_op(j, d, {sq, ci}, D::PathOp::Intersect);
        check("intersect: the overlap -- the circle's area", !in.empty() && inside(in, 150, 350) && !inside(in, 105, 305));
        j.undo();
        const auto x = D::path_op(j, d, {sq, ci}, D::PathOp::Exclude);
        check("exclude: all but the overlap", !x.empty() && inside(x, 105, 305) && !inside(x, 150, 350));
        j.undo();
        const auto far = D::add_frame(j, d, page, core::FrameKind::Shape, {400, 500, 20, 20});
        check("intersect: shapes that don't meet -- nothing, no step", D::path_op(j, d, {sq, far}, D::PathOp::Intersect).empty() &&
              d.as<core::Frame>(far));
        check("pathfinder: a text frame isn't a shape here", D::path_op(j, d, {sq, "frame.1"}, D::PathOp::Unite).empty());
        while (j.can_undo()) j.undo();
        check("pathfinder: all undone exactly", core::to_json(d) == before);
    }
    // ── s215: Offset Path, Outline Stroke ─────────────────────────────────
    {
        std::cout << "--- s215 offset ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto sq = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 100, 100, 100});
        const auto out = D::offset_paths(j, d, {sq}, 10, 0, true);
        check("offset: a square out 10 (mitre) -- a new 120-wide square on top, the original kept",
              out.size() == 1 && out[0] != sq && d.as<core::Frame>(sq) &&
              std::abs(d.as<core::Frame>(out[0])->w - 120) < 0.01 && d.as<core::Frame>(out[0])->z > d.as<core::Frame>(sq)->z &&
              j.undo_label() == "Offset path");
        const auto in = D::offset_paths(j, d, {sq}, -20, 1, true);
        check("offset: in 20 -- 60 wide", in.size() == 1 && std::abs(d.as<core::Frame>(in[0])->w - 60) < 0.01);
        const auto rnd = D::offset_paths(j, d, {sq}, 10, 1, true);
        check("offset: round corners -- curves at them", rnd.size() == 1 && std::any_of(d.as<core::Frame>(rnd[0])->path.begin(),
              d.as<core::Frame>(rnd[0])->path.end(), [](const core::PathPoint& p) { return p.ix != p.x || p.ox != p.x; }));
        const auto ln = D::add_path(j, d, page, {D::corner_point(100, 400), D::corner_point(300, 400)}, true);
        D::edit_frames(j, d, {ln}, "Weight", [](core::Frame& f) { f.stroke_w = 8; });
        const auto stroke = d.as<core::Frame>(ln)->stroke;
        const auto ol = D::offset_paths(j, d, {ln}, 4, 0, false);
        const auto* of = d.as<core::Frame>(ln);
        check("outline stroke: the line made a 200 x 8 shape, filled with its stroke's colour, in its place",
              ol.size() == 1 && ol[0] == ln && !of->open && std::abs(of->w - 200) < 0.01 && std::abs(of->h - 8) < 0.01 &&
              of->fill == stroke && of->stroke.empty() && j.undo_label() == "Outline");
        while (j.can_undo()) j.undo();
    }
    // ── s216: text on a path ──────────────────────────────────────────────
    {
        std::cout << "--- s216 text on a path ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto ln = D::add_path(j, d, page, {D::corner_point(100, 300), D::corner_point(300, 300)}, true);
        const auto tr = D::path_track(*d.as<core::Frame>(ln));
        double x, y, a;
        tr.at(50, x, y, a);
        check("track: a 200 pt line, 50 along it at (150, 300), heading right", std::abs(tr.length() - 200) < 1e-6 &&
              std::abs(x - 150) < 1e-6 && std::abs(y - 300) < 1e-6 && std::abs(a) < 1e-9);
        double dist = 0;
        check("track: the nearest place to a point -- how far along", std::abs(tr.nearest(220, 310, &dist) - 120) < 1e-6 && std::abs(dist - 10) < 1e-6);
        D::set_frame_content(j, d, {ln}, core::FrameKind::Text);
        D::edit_frames(j, d, {ln}, "On path", [](core::Frame& f) { f.on_path = true; f.path_from = 20; });
        {
            core::Doc::Loading ld(d);
            core::Story st = *d.as<core::Story>(d.as<core::Frame>(ln)->story);
            st.paras[0].text = "Along the line, and on past its end until the words run out of room entirely here";
            d.put(d.as<core::Frame>(ln)->story, st);
        }
        const auto box = D::text_box(*d.as<core::Frame>(ln));
        check("text box: the path's length less where it starts, one column, no shape", std::abs(box.w - 180) < 1e-6 && box.columns == 1 &&
              box.shape == core::ShapeKind::Rect);
        check("text on a path: nothing wraps it", core::wrap_rects(d, ln).empty());
        L::FlowCache flows;
        const auto& fl = flows.get(d, d.as<core::Frame>(ln)->story);
        double w0 = 0;
        for (const auto& ft : fl.frames) if (ft.frame == ln && !ft.lines.empty()) w0 = ft.lines.front().w > 0 ? ft.lines.front().w : 180;
        check("text on a path: flowed at the path's length", w0 > 0 && w0 <= 180.01, std::to_string(w0));
        {   // as it prints: each glyph turned (a cm before it)
            namespace fs = std::filesystem;
            const auto base = fs::temp_directory_path() / "composix_selftest_onpath";
            std::error_code ec;
            fs::create_directories(base, ec);
            const auto out = (base / "p.pdf").string();
            L::PdfOptions opt;
            opt.compress = false;
            L::PdfReport rep;
            std::string err;
            const bool ok = L::export_print_pdf(d, out, opt, {}, &rep, &err);
            std::string pdf;
            { std::ifstream in(out, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); pdf = ss.str(); }
            int cms = 0;
            for (auto i = pdf.find(" 0 Tm ["); i != std::string::npos; i = pdf.find(" 0 Tm [", i + 1)) ++cms;   // each glyph at its own origin
            check("print: the glyphs set one by one along it", ok && cms >= 20, std::to_string(cms));
            fs::remove_all(base, ec);
        }
        core::Doc e;
        std::string err;
        const auto text = core::to_json(d);
        check("text on a path: through JSON", core::from_json(e, text, &err) && e.as<core::Frame>(ln)->on_path &&
              e.as<core::Frame>(ln)->path_from == 20 && core::to_json(e) == text, err);
        while (j.can_undo()) j.undo();
    }
    // ── s217: making text on a path ───────────────────────────────────────
    {
        std::cout << "--- s217 text on path verbs ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const std::string before = core::to_json(d);
        const auto ov = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 100, 120, 80}, 1, 12, false, core::ShapeKind::Oval);
        check("on path: a shape gets a story, its words along it, from where it was clicked -- one step",
              D::set_text_on_path(j, d, {ov}, true, 33) && d.as<core::Frame>(ov)->kind == core::FrameKind::Text &&
              d.as<core::Frame>(ov)->on_path && d.as<core::Frame>(ov)->path_from == 33 && d.as<core::Story>(d.as<core::Frame>(ov)->story) &&
              j.undo_label() == "Text on path");
        check("on path: already so -- no step", !D::set_text_on_path(j, d, {ov}, true, 33));
        check("on path: back inside", D::set_text_on_path(j, d, {ov}, false) && !d.as<core::Frame>(ov)->on_path &&
              j.undo_label() == "Text in frame");
        check("on path: a picture frame isn't", !D::set_text_on_path(j, d, {"frame.11"}, true));
        D::set_text_on_path(j, d, {ov}, true, 0);
        const auto pl = core::layout_pages(d, {page});
        check("on path: picked by its words above the path (16 pt)", core::hit_frame(d, pl, pl.front().r.x + 160, pl.front().r.y + 88, 1) == ov);
        while (j.can_undo()) j.undo();
        check("on path: all undone exactly", core::to_json(d) == before);
    }
}
}  // namespace cxtest
