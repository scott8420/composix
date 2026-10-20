// composix_selftest, part 2 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part2() {

    std::cout << "-----------------------------------------------\n";

    // -- Recents: list ops + JSON round-trip with prune -----------------------
    {
        std::vector<std::string> l;
        core::recents_add(l, "/a");
        core::recents_add(l, "/b");
        core::recents_add(l, "/c");
        check("recents: newest first", l.size() == 3 && l.front() == "/c");
        core::recents_add(l, "/a");
        check("recents: re-add moves to front (no dup)",
              l.size() == 3 && l.front() == "/a");
        core::recents_add(l, "/x/");
        check("recents: trailing slash normalised",
              std::find(l.begin(), l.end(), "/x") != l.end() &&
              std::find(l.begin(), l.end(), "/x/") == l.end());
        std::vector<std::string> cap;
        for (int i = 0; i < 20; ++i)
            core::recents_add(cap, "/p" + std::to_string(i), 8);
        check("recents: trim to max (8)", cap.size() == 8 && cap.front() == "/p19");

        const std::string base =
            (std::filesystem::temp_directory_path() / "composix_selftest_recents").string();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        const std::string live = base + "/live";
        std::filesystem::create_directories(live, ec);
        const std::string file = base + "/recent.json";

        std::vector<std::string> to_save = {live, base + "/GONE"};
        check("recents: save ok", core::save_recents(file, to_save));
        auto loaded = core::load_recents(file);
        check("recents: load prunes the missing path",
              loaded.size() == 1 && loaded.front() == live);
        check("recents: missing file -> empty",
              core::load_recents(base + "/nope.json").empty());
    }

    // -- TextMap: forward/inverse round-trip ----------------------------------
    {
        // A scene as a viewer flattens it:
        //   0 "The Cellar"     title  (non-prose)
        //   1 ""               gap    (non-prose)
        //   2 "She opened the" prose, para_start   <- para 1, wrap 1
        //   3 "old iron door." prose               <- para 1, wrap 2
        //   4 "Caf\u00e9 dust." prose, para_start  <- para 2 (UTF-8: é)
        std::vector<core::FlatLine> flat = {
            {"The Cellar",     false, false},
            {"",               false, false},
            {"She opened the", true,  true },
            {"old iron door.", true,  false},
            {"Caf\xC3\xA9 dust.", true, true },
        };
        roundtrip(flat, 2, 0, 2, 14, "single line");
        roundtrip(flat, 2, 4, 3, 8,  "multi-line (soft wrap)");
        roundtrip(flat, 2, 0, 4, 10, "cross-paragraph");
        roundtrip(flat, 4, 0, 4, 6,  "UTF-8 span (accented)");

        // Chrome is never landed on: a range covering all prose starts on line 2.
        std::vector<int> off;
        (void)core::visible_text(flat, off);
        core::LineCol s, e;
        core::map_range(flat, 0, 3, s, e);
        check("textmap: start snaps to prose, never chrome", s.line == 2);

        // Degenerate / empty inputs fail cleanly.
        check("textmap: empty range rejected", !core::map_range(flat, 5, 5, s, e));
        std::vector<core::FlatLine> no_prose = {{"Title", false, false}};
        check("textmap: no prose -> false", !core::map_range(no_prose, 0, 3, s, e));
    }
    // -- Shortcuts: the registry paradigm, exercised ---------------------------
    // A seed's claim isn't a fact until a consumer exercises it. The registry's
    // whole promise is that a key and its advertised row can't drift, because
    // both read one list -- so the guard that matters is pure and lives here:
    // no two chords may collide. (App wires from this same list; ShortcutsDialog
    // renders from it.)
    {
        namespace sc = composix::core;

        check("shortcuts: format_accel renders modifiers + key",
              sc::format_accel("<Ctrl><Shift>n") == "Ctrl+Shift+N",
              sc::format_accel("<Ctrl><Shift>n"));
        check("shortcuts: format_accel maps a named key to its glyph",
              sc::format_accel("<Ctrl>question") == "Ctrl+?",
              sc::format_accel("<Ctrl>question"));

        {
            sc::ShortcutSpec derived; derived.accels = {"<Ctrl>question", "<Ctrl>slash"};
            check("shortcuts: display_keys derives + joins accels",
                  derived.display_keys() == "Ctrl+?  /  Ctrl+/",
                  derived.display_keys());
            sc::ShortcutSpec doc; doc.keys = "Click a recent";
            check("shortcuts: display_keys prefers an explicit key string",
                  doc.display_keys() == "Click a recent");
        }

        check("shortcuts: registry is populated",
              !sc::shortcut_registry().empty(),
              std::to_string(sc::shortcut_registry().size()) + " specs");
        check("shortcuts: registry is accel-collision-free",
              sc::find_accel_collisions().empty(),
              sc::find_accel_collisions().empty()
                  ? "clean" : sc::find_accel_collisions().front());

        // Sections authored A-Z + contiguous: the dialog walks linearly and
        // starts a heading on change, so a stray out-of-order row would split a
        // section into two headings.
        {
            std::vector<std::string> secs;
            for (const auto& r : sc::shortcut_registry())
                if (secs.empty() || secs.back() != r.section) secs.push_back(r.section);
            std::vector<std::string> sorted = secs;
            std::sort(sorted.begin(), sorted.end());
            check("shortcuts: sections are contiguous and A-Z ordered", secs == sorted);
        }

        // The doc-only half of the paradigm: a gesture row documents without
        // binding (no accels), but must still show keys.
        {
            bool ok = true, saw_doc_only = false;
            for (const auto& r : sc::shortcut_registry())
                if (r.action.empty()) {
                    saw_doc_only = true;
                    if (!r.accels.empty() || r.keys.empty()) ok = false;
                }
            check("shortcuts: a doc-only row binds nothing but still displays keys",
                  ok && saw_doc_only);
        }
    }



    {
        std::cout << "\n-- text wrap --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* pic = d.as<core::Frame>("frame.11");
        const auto* txt = d.as<core::Frame>("frame.1");
        // The wrap edge, in frame.2's coordinates: the picture plus 9 pt a side.
        const auto wr = core::wrap_rects(d, "frame.1");
        check("wrap: the picture pushes the opening text", wr.size() == 1 && !wr[0].jump &&
                                                          std::abs(wr[0].x0 - (pic->x - 9 - txt->x)) < 1e-9 &&
                                                          std::abs(wr[0].y1 - (pic->y + pic->h + 9 - txt->y)) < 1e-9);
        check("wrap: not its own frame, not other pages", core::wrap_rects(d, "frame.11").empty() &&
                                                             core::wrap_rects(d, "frame.2").empty());
        const auto flow = L::flow_story(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& f : flow.frames) if (f.frame == "frame.1") ft = &f;
        bool clear = ft != nullptr, both = false;
        int beside = 0;
        if (ft && !wr.empty())
            for (const auto& l : ft->lines) {
                const double right = l.x + (l.w > 0 ? l.w : txt->w);
                const bool in_band = l.y + l.h > wr[0].y0 && l.y < wr[0].y1;
                if (in_band) {
                    ++beside;
                    clear = clear && (right <= wr[0].x0 + 0.01 || l.x >= wr[0].x1 - 0.01);
                    both = both || l.x >= wr[0].x1 - 0.01;
                }
            }
        check("wrap: lines beside it keep clear of the edge", clear && beside > 0);
        check("wrap: Around sets text on both sides", both);
        // A row of two: the click picks the side, arrows move by rows.
        if (ft) {
            const L::Line* left = nullptr;
            const L::Line* right = nullptr;
            for (std::size_t k = 0; k + 1 < ft->lines.size() && !right; ++k)
                if (ft->lines[k + 1].y == ft->lines[k].y && ft->lines[k + 1].x > ft->lines[k].x &&
                    ft->lines[k].cap <= 0 && ft->lines[k + 1].cap <= 0) {   // (not a drop cap and its line -- s029)
                    left = &ft->lines[k];
                    right = &ft->lines[k + 1];
                }
            check("wrap: two lines side by side", left && right);
            if (left && right) {
                const auto hl = L::hit_text(d, "story.1", flow, "frame.1", left->x + 5, left->y + 2);
                const auto hr = L::hit_text(d, "story.1", flow, "frame.1", right->x + 5, right->y + 2);
                check("wrap: a click on each side lands on its line",
                      hl.byte >= left->start && hl.byte <= left->end && hr.byte >= right->start && hr.byte <= right->end);
                const auto down = L::line_move(d, "story.1", flow, {left->para, left->start + 1}, 1, left->x + 5);
                const auto cd = L::caret_at(d, "story.1", flow, down);
                check("wrap: down from the left goes down, not across", cd.ok && cd.y > left->y && cd.x < right->x);
            }
        }
        // Jump: nothing beside it.
        check("wrap: set to Jump", core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap",
                                                          [](core::Frame& f) { f.wrap = core::Wrap::Jump; }));
        const auto jf = L::flow_story(d, "story.1");
        bool none = true;
        const auto jr = core::wrap_rects(d, "frame.1");
        for (const auto& f : jf.frames)
            if (f.frame == "frame.1" && !jr.empty())
                for (const auto& l : f.lines) none = none && (l.cap > 0 || !(l.y + l.h > jr[0].y0 && l.y < jr[0].y1));
        check("wrap: Jump leaves its band empty", none && !jr.empty() && jr[0].jump);
        j.undo();
        check("wrap: undo", d.as<core::Frame>("frame.11")->wrap == core::Wrap::Around);
        // None: the opening text as before s018 (no line set on its own).
        check("wrap: off", core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap",
                                                  [](core::Frame& f) { f.wrap = core::Wrap::None; }));
        bool plain = true;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines) {   // (a drop cap's lines are set on their own too -- s029; a hyphenating style's, s038)
                const auto& ps = d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(l.para)].style;
                plain = plain && (l.w <= 0 || ps == "style.10" || d.as<core::Style>(ps)->hyphenate);
            }
        check("wrap: none sets every line with its paragraph", plain);
        // The inspector: Wrap on every frame, Offset when it wraps.
        j.undo();
        const auto items = core::frame_items(d, {"frame.11"}, "");
        bool wf = false, of = false;
        for (const auto& f : items.front().fields) {
            wf = wf || (f.edit == "wrap" && f.value == "Around");
            of = of || (f.edit == "wrap_off" && f.value == "9\t9\t9\t9");
        }
        check("wrap: inspector shows Wrap + offset", wf && of);
    }

    {
        std::cout << "\n-- s173: wrap to an image's shape --\n";
        namespace L = composix::layout;
        // Tracing: a see-through picture -- a triangle, its point up.
        const int W = 40, H = 40;
        std::vector<std::uint32_t> px(W * H, 0x00000000u);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (std::abs(x - 20) * 2 <= y) px[static_cast<std::size_t>(y * W + x)] = 0xff336699u;
        const auto tri = core::trace_shape(px.data(), W, H, W, 4);
        check("trace: aspect, then a span a band", tri.size() == 9 && std::abs(tri[0] - 1.f) < 1e-6);
        check("trace: narrow at the top, wide at the foot", tri.size() == 9 && (tri[2] - tri[1]) < (tri[8] - tri[7]) &&
              tri[7] <= 0.05f && tri[8] >= 0.95f && tri[1] > 0.3f && tri[2] < 0.7f);
        {   // No alpha: a product on white -- the corners' colour is the ground.
            std::vector<std::uint32_t> q(W * H, 0xfffefefeu);
            for (int y = 10; y < 30; ++y) for (int x = 15; x < 25; ++x) q[static_cast<std::size_t>(y * W + x)] = 0xff202020u;
            const auto sq = core::trace_shape(q.data(), W, H, W, 4);
            check("trace: on a plain ground -- the object, the empty bands marked",
                  sq.size() == 9 && sq[1] > sq[2] && std::abs(sq[3] - 15.f / 40) < 1e-6 && std::abs(sq[4] - 25.f / 40) < 1e-6 && sq[7] > sq[8]);
            q[0] = 0xff00ff00u;   // a corner of another colour: a photo to its edges
            check("trace: a photo to its edges -- nothing", core::trace_shape(q.data(), W, H, W, 4).empty());
            std::vector<std::uint32_t> none(W * H, 0x00000000u);
            check("trace: see-through all over -- nothing", core::trace_shape(none.data(), W, H, W, 4).empty());
        }
        // As placed: Fit in a frame twice as wide -- the picture centred.
        {
            core::Frame f;
            f.kind = core::FrameKind::Image;
            f.w = 200; f.h = 100;
            f.fit = core::ImageFit::Fit;
            core::Asset a;
            a.shape = tri;
            const auto b = core::shape_bands(f, a);
            check("shape bands: in the frame, the picture's place", b.size() == 4 && b[0].x0 > 50 && b[3].x0 >= 50 - 1e-9 &&
                  b[3].x1 <= 150 + 1e-9 && std::abs(b[0].y0) < 1e-9 && std::abs(b[3].y1 - 100) < 1e-9);
            a.shape.clear();
            check("shape bands: untraced -- none", core::shape_bands(f, a).empty());
        }
        // The sample's lighthouse (Around) -> Shape, traced as the triangle.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto around = core::wrap_rects(d, "frame.1");
        const auto aid = d.as<core::Frame>("frame.11")->asset;
        const auto steps = j.size();
        check("wrap shape: one step", core::doc::set_wrap_shape(j, d, {"frame.11", "frame.1"}, {{aid, tri}}) && j.size() == steps + 1 &&
              d.as<core::Frame>("frame.11")->wrap == core::Wrap::Shape && d.as<core::Asset>(aid)->shape == tri &&
              d.as<core::Frame>("frame.1")->wrap == core::Wrap::None);
        check("wrap shape: again -- nothing", !core::doc::set_wrap_shape(j, d, {"frame.11"}, {{aid, tri}}) && j.size() == steps + 1);
        const auto shaped = core::wrap_rects(d, "frame.1");
        bool inside = around.size() == 1 && shaped.size() == 4;
        for (const auto& r : shaped)
            inside = inside && r.x0 >= around[0].x0 - 1e-9 && r.x1 <= around[0].x1 + 1e-9 && r.y0 >= around[0].y0 - 1e-9 && r.y1 <= around[0].y1 + 1e-9;
        check("wrap shape: a rect a band, within the frame's wrap", inside && shaped[0].x1 - shaped[0].x0 < shaped[3].x1 - shaped[3].x0);
        {   // The text comes into the frame's old box, and keeps clear of every band.
            const auto flow = L::flow_story(d, "story.1");
            bool clear = true, closer = false;
            for (const auto& ft : flow.frames) {
                if (ft.frame != "frame.1") continue;
                const double fw = d.as<core::Frame>("frame.1")->w;
                for (const auto& l : ft.lines) {
                    const double x1 = l.x + (l.w > 0 ? l.w : fw);
                    for (const auto& r : shaped)
                        if (l.y + l.h > r.y0 && l.y < r.y1) clear = clear && (x1 <= r.x0 + 0.01 || l.x >= r.x1 - 0.01);
                    if (!around.empty() && l.y + l.h > around[0].y0 && l.y < around[0].y1 && l.x > around[0].x0 + 1 && l.x < around[0].x1 - 1)
                        closer = true;
                }
            }
            check("wrap shape: lines keep clear of the outline", clear);
            check("wrap shape: text comes in closer than the frame's box", closer);
        }
        {
            core::Doc e;
            check("wrap shape: saved with the document (outline rounded)", core::from_json(e, core::to_json(d)) &&
                  e.as<core::Frame>("frame.11")->wrap == core::Wrap::Shape && e.as<core::Asset>(aid)->shape.size() == tri.size() &&
                  std::abs(e.as<core::Asset>(aid)->shape[2] - tri[2]) < 1e-4);
        }
        {
            const auto items = core::frame_items(d, {"frame.11"}, "");
            bool sh = false;
            for (const auto& f : items.front().fields)
                if (f.edit == "wrap") for (const auto& c : f.choices) sh = sh || (c.id == "3" && f.value == "Shape");
            const auto titems = core::frame_items(d, {"frame.1"}, "");
            bool tsh = false;
            for (const auto& f : titems.front().fields)
                if (f.edit == "wrap") for (const auto& c : f.choices) tsh = tsh || c.id == "3";
            check("wrap shape: the inspector offers Shape on pictures only", sh && !tsh);
        }
        // A relink keeps a traced outline when the new file brings one; untraced wraps the frame.
        core::ImageFile nf{"/pics/new.png", "new.png", 400, 400, false, "RGB", 5};
        check("wrap shape: relinked without an outline -- the frame's box", core::doc::relink_assets(j, d, {{aid, nf}}) &&
              d.as<core::Asset>(aid)->shape.empty() && core::wrap_rects(d, "frame.1").size() == 1);
        nf.shape = tri;
        nf.stamp = 6;
        check("wrap shape: relinked with one -- kept", core::doc::relink_assets(j, d, {{aid, nf}}) &&
              d.as<core::Asset>(aid)->shape == tri && core::wrap_rects(d, "frame.1").size() == 4);
        {   // s175: placed into a Shape frame with its outline (Replace, a drop): kept -- a new asset or one already there.
            core::ImageFile pf{"/pics/other.png", "other.png", 300, 300, false, "RGB", 3};
            pf.shape = tri;
            const bool placed = core::doc::place_image(j, d, "frame.11", pf);
            const auto* na = d.as<core::Asset>(d.as<core::Frame>("frame.11")->asset);
            check("wrap shape: placed with an outline -- kept, wraps to it", placed && na && na->shape == tri &&
                  core::wrap_rects(d, "frame.1").size() == 4);
            j.undo();
            core::ImageFile again{"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7};
            const bool p1 = core::doc::place_image(j, d, "frame.4", again);   // cove.png now an asset, untraced
            again.shape = tri;
            const bool p2 = core::doc::place_image(j, d, "frame.11", again);
            const auto* ca = d.as<core::Asset>(d.as<core::Frame>("frame.11")->asset);
            check("wrap shape: an asset already there gains the outline", p1 && p2 && ca && ca->shape == tri);
            j.undo(); j.undo();
        }
        j.undo(); j.undo(); j.undo();
        check("wrap shape: undone -- Around, untraced", d.as<core::Frame>("frame.11")->wrap == core::Wrap::Around &&
              d.as<core::Asset>(aid)->shape.empty() && core::wrap_rects(d, "frame.1").size() == 1);
    }

    {
        std::cout << "\n-- new document --\n";
        namespace L = composix::layout;
        const double in = core::kPt;
        check("presets: Trade 6 x 9 first", !core::trim_presets().empty() &&
                                            core::trim_presets().front().w == 6 * in && core::trim_presets().front().h == 9 * in);
        core::NewDoc n;
        check("defaults are usable", core::check_new(n).empty(), core::check_new(n));
        {
            auto b = n; b.pages = 0;
            check("refused: no pages", !core::check_new(b).empty());
            b = n; b.margins.inside = b.margins.outside = 3 * in;
            check("refused: margins eat the page", core::check_new(b).find("half an inch") != std::string::npos);
            b = n; b.columns = 12; b.gutter = 24;
            check("refused: columns too narrow", core::check_new(b).find("narrower") != std::string::npos);
            core::Doc keep;
            core::load_sample(keep);
            const auto before = keep.size();
            b = n; b.trim_w = 0.5 * in;
            check("refused: the document is left alone", !core::new_document(keep, b) && keep.size() == before);
        }
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        n.title = "Field Notes";
        n.pages = 8;
        n.margins = {0.75 * in, 1 * in, 0.875 * in, 0.5 * in};
        n.columns = 2;
        n.gutter = 18;
        check("made", core::new_document(d, n));
        check("settings: title, trim, bleed, facing", d.settings().title == "Field Notes" && d.settings().trim_w == 6 * in &&
                                                      d.settings().bleed == 0.125 * in && d.settings().facing);
        check("eight pages on the master", d.pages().size() == 8 && d.as<core::Page>("page.8")->master == "master.1");
        const auto* m = d.as<core::Master>("master.1");
        check("the master carries margins + columns", m && m->margins == n.margins && m->columns == 2 && m->gutter == 18);
        check("starts on a right-hand page", d.side_of("page.1") == core::Side::Right && d.side_of("page.2") == core::Side::Left);
        const auto* f1 = d.as<core::Frame>("frame.1");
        const auto* f2 = d.as<core::Frame>("frame.2");
        check("a frame in each page's margins (left pages mirrored)",
              f1 && f2 && f1->x == 0.875 * in && f2->x == 0.5 * in && f1->y == 0.75 * in &&
                  std::abs(f1->h - (9 * in - 1.75 * in)) < 1e-9 && f1->columns == 2 && f1->gutter == 18);
        check("threaded page to page", L::thread_frames(d, "story.1").size() == 8 && d.as<core::Frame>("frame.8")->next.empty());
        check("the story flows, nothing over", !L::flow_story(d, "story.1").overset);
        check("styles + swatches to start with", d.ids_of<core::Style>().size() == 6 && d.ids_of<core::Swatch>().size() == 2);
        check("a folio on the master: the page number", d.as<core::Frame>("frame.9") &&
                                                        d.as<core::Frame>("frame.9")->owner == "master.1" &&
                                                        d.as<core::Story>("story.2")->paras[0].text == core::kMarkPage);
        check("not a step: nothing to undo", !j.can_undo());
        check("the next frame gets a fresh id", d.fresh_id("frame") == "frame.10");
        std::string err;
        core::Doc e;
        check("round-trips through JSON", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        n.facing = false;
        n.text_frames = false;
        n.folios = false;
        core::Doc u;
        core::new_document(u, n);
        check("unfaced: no sides, no frames", u.side_of("page.2") != core::Side::Left && u.ids_of<core::Frame>().empty() &&
                                              !u.as<core::Story>("story.1"));
        const auto o = core::project(d, "");
        check("the projection shows it", o.size() > 0);
    }

    {
        std::cout << "\n-- page thumbnails --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const auto k5 = L::page_key(d, flows, "page.5");
        const auto k16 = L::page_key(d, flows, "page.16");
        check("key: a page with text carries its lines",
              std::any_of(k5.lines.begin(), k5.lines.end(), [](const std::string& l) { return l.find("Chapter One") != std::string::npos; }));
        check("key: the same page, the same key", L::page_key(d, flows, "page.5") == k5);
        // A change on page 16 (sources) leaves page 1 of the body alone.
        core::doc::edit_frames(j, d, {"frame.10"}, "Gutter", [](core::Frame& f) { f.gutter = 20; });
        check("key: another page's change leaves it", L::page_key(d, flows, "page.5") == k5 &&
                                                      !(L::page_key(d, flows, "page.16") == k16));
        // Typing in the chapter changes the page it's set on.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then ", "Typing", "");
        check("key: its text changes it", !(L::page_key(d, flows, "page.5") == k5));
        // The master's items are part of every page that shows it.
        const auto k2 = L::page_key(d, flows, "page.6");
        core::doc::edit_masters(j, d, {"master.2"}, "Columns", [](core::Master& m) { m.columns = 2; });
        check("key: a master change reaches its pages", !(L::page_key(d, flows, "page.6") == k2));
        // Drawn: ink where the text is, a soft plate where a picture is missing.
        auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 432, 648);
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        std::set<std::string> missing;
        L::draw_page(cr, d, "page.5", flows, {}, &missing, {.placeholders = true});
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        const auto px = [&](int x, int y) {
            const auto* row = cairo_image_surface_get_data(surf) + y * cairo_image_surface_get_stride(surf);
            return reinterpret_cast<const std::uint32_t*>(row)[x] & 0xffffff;
        };
        int ink = 0;
        for (int y = 180; y < 260; ++y) for (int x = 54; x < 378; ++x) ink += px(x, y) != 0xffffff;
        const auto* pic = d.as<core::Frame>("frame.4");
        check("drawn: the text inks the page", ink > 200);
        check("drawn: a missing picture as a plate", px(static_cast<int>(pic->x + pic->w / 2), static_cast<int>(pic->y + 5)) != 0xffffff &&
                                                     missing.count("harbor.psd") == 1);
        cairo_surface_destroy(surf);
    }

    {
        std::cout << "\n-- tints + opacity --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        check("tint: half of black is mid grey, 100% is itself", core::tint_rgb(0x000000, 0.5) == 0x808080 &&
                                                                core::tint_rgb(0xb5512b, 1) == 0xb5512b &&
                                                                core::tint_rgb(0xb5512b, 0) == 0xffffff);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A Black box at 40% on page 2, and the caption at half opacity.
        const auto box = core::doc::add_frame(j, d, "page.6", core::FrameKind::Shape, {54, 54, 100, 50});
        core::doc::edit_frames(j, d, {box}, "Fill", [](core::Frame& f) { f.fill = "swatch.1"; f.fill_tint = 0.4; f.stroke.clear(); });
        const auto items = core::frame_items(d, {"frame.6"}, "");
        bool ft = false, st = false, op = false;
        for (const auto& f : items.front().fields) {
            ft = ft || (f.edit == "fill_tint" && f.value == "15%");
            st = st || (f.edit == "stroke_tint" && f.value == "100%");
            op = op || (f.edit == "opacity" && f.value == "100%");
        }
        check("inspector: Fill tint, Stroke tint, Opacity", ft && st && op);
        const L::ImageSource none = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto out = (fs::temp_directory_path() / "composix_selftest_tint.pdf").string();
        L::PdfOptions opt;
        opt.compress = false;
        std::string err;
        const auto read = [&] { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
        check("print: ok", L::export_print_pdf(d, out, opt, none, nullptr, &err), err);
        auto pdf = read();
        check("print: a process tint is less ink (40% K)", pdf.find("0 0 0 0.4 k\n") != std::string::npos);
        check("print: a spot tint prints on the ink's plate (15%)", pdf.find(" cs 0.15 scn") != std::string::npos);
        check("print: no transparency when nothing is see-through", pdf.find("/ExtGState") == std::string::npos);
        core::doc::edit_frames(j, d, {"frame.6"}, "Opacity", [](core::Frame& f) { f.opacity = 0.5; });
        check("print: opacity as an ExtGState", L::export_print_pdf(d, out, opt, none, nullptr, &err) &&
                                                (pdf = read(), pdf.find("/ca 0.5 /CA 0.5") != std::string::npos));
        const char* gracol = "/usr/share/color/icc/GRACoL2013_CRPC6.icc";
        if (fs::exists(gracol)) {
            opt.profile = gracol;
            opt.standard = L::plate_standard::X1a;
            L::PdfReport xr;   // s193: flattened now, and said so (was: refused)
            check("x1a: opacity flattened, the frame named",
                  L::export_print_pdf(d, out, opt, none, &xr, &err) && (pdf = read(), pdf.find("/ExtGState") == std::string::npos) &&
                      std::any_of(xr.warnings.begin(), xr.warnings.end(), [](const std::string& w) {
                          return w.find("flattened for PDF/X-1a") != std::string::npos && w.find("page 7: a frame at 50% opacity") != std::string::npos; }),
                  err);
            opt.standard = L::plate_standard::X4;
            check("x4: opacity kept", L::export_print_pdf(d, out, opt, none, nullptr, &err), err);
        }
        std::string json = core::to_json(d);
        core::Doc e;
        check("json: tints + opacity round-trip", core::from_json(e, json, &err) && e.records() == d.records() &&
                                                  e.as<core::Frame>("frame.6")->opacity == 0.5);
    }

    {
        std::cout << "\n-- find / change --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const auto any = D::find_all(d, {"the"});
        const auto cased = D::find_all(d, {"The", true});
        check("find: in reading order, the chapter first", !any.empty() && any.front().story == "story.1" &&
                                                           any.front().a == core::TextPos{1, 0} && any.front().b == core::TextPos{1, 3});
        check("find: match case finds fewer", !cased.empty() && cased.size() < any.size());
        check("find: whole word", !D::find_all(d, {"harb"}).empty() && D::find_all(d, {"harb", false, true}).empty() &&
                                  !D::find_all(d, {"harbor", false, true}).empty());
        check("find: a paragraph style alone -- each paragraph in it", D::find_all(d, {"", false, false, "style.2"}).size() == 2);
        check("find: a character style alone -- each run of it", D::find_all(d, {"", false, false, "", "style.5"}).size() == 1);
        check("find: text wholly in a character style", D::find_all(d, {"almanac", false, false, "", "style.5"}).size() == 1 &&
                                                      D::find_all(d, {"harbor", false, false, "", "style.5"}).empty());
        check("find: nothing asked, nothing found", D::find_all(d, {}).empty());
        // Change All: one step, every one; undo puts them all back.
        const auto before = *d.as<core::Story>("story.1");
        const auto steps = j.size();
        const int n = D::change_hits(j, d, any, {true, "a"}, "Change All");
        check("change all: every hit, one step", n == static_cast<int>(any.size()) && j.size() == steps + 1 &&
                                                 D::find_all(d, {"the", false, true}).empty());
        check("change all: later hits kept their places", d.as<core::Story>("story.1")->paras[1].text.rfind("a harbor smelled", 0) == 0);
        j.undo();
        check("change all: undo, all back", *d.as<core::Story>("story.1") == before);
        // Change one, applying a character style to what's put there.
        const auto h = D::find_all(d, {"diesel"});
        check("change: one, styled", D::change_hits(j, d, h, {true, "coal", "", "style.5"}, "Change") == 1 &&
                                     core::char_style_at(d.as<core::Story>("story.1")->paras[1], h.front().a.byte + 1) == "style.5" &&
                                     d.as<core::Story>("story.1")->paras[1].text.find("coal") != std::string::npos);
        // Style only: a paragraph style on every heading's paragraph.
        check("change: style only, text kept", D::change_hits(j, d, D::find_all(d, {"chapter", false, false, "style.2"}), {false, "", "style.6"}, "Change All") == 2 &&
                                               d.as<core::Story>("story.1")->paras[0].style == "style.6" &&
                                               d.as<core::Story>("story.1")->paras[0].text == "Chapter One");
        const auto at = j.size();
        check("change: to the same thing, no step", D::change_hits(j, d, D::find_all(d, {"coal"}), {true, "coal"}, "Change") == 0 &&
                                                    j.size() == at);
    }

    {
        std::cout << "\n-- find by formatting (s051) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const std::string S = "story.1";
        const auto& p1 = [&]() -> const core::Para& { return d.as<core::Story>(S)->paras[1]; };
        bool ascii = p1().text.size() > 40 && p1().runs.empty();
        for (int i = 0; ascii && i < 40; ++i) ascii = static_cast<unsigned char>(p1().text[static_cast<std::size_t>(i)]) < 0x80;
        check("find fmt: fixture -- a plain opening paragraph", ascii);
        // [0,5) italic by hand, [5,10) bold italic by hand, [20,25) bold by hand, [30,35) in Emphasis.
        D::set_local(j, d, S, {1, 0}, {1, 5}, "i", [](core::Local& l) { l.italic = 1; });
        D::set_local(j, d, S, {1, 5}, {1, 10}, "bi", [](core::Local& l) { l.italic = 1; l.bold = 1; });
        D::set_local(j, d, S, {1, 20}, {1, 25}, "b", [](core::Local& l) { l.bold = 1; });
        D::set_char_style(j, d, S, {1, 30}, {1, 35}, "style.5");
        const auto in1 = [&](const std::vector<core::doc::Hit>& hs) {
            std::vector<std::pair<int, int>> out;
            for (const auto& h : hs) if (h.story == S && h.a.para == 1) out.push_back({h.a.byte, h.b.byte});
            return out;
        };
        using V = std::vector<std::pair<int, int>>;
        check("find fmt: italic -- by hand (side by side, joined) and by character style",
              in1(D::find_all(d, D::FindOpts{.italic = true})) == V{{0, 10}, {30, 35}});
        check("find fmt: bold", in1(D::find_all(d, D::FindOpts{.bold = true})) == V{{5, 10}, {20, 25}});
        check("find fmt: bold italic", in1(D::find_all(d, D::FindOpts{.bold = true, .italic = true})) == V{{5, 10}});
        check("find fmt: words already in the style to apply left out",
              in1(D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"})) == V{{0, 10}});
        check("find fmt: in a character style and italic", in1(D::find_all(d, D::FindOpts{.cstyle = "style.5", .italic = true})) == V{{30, 35}});
        const std::string w1 = p1().text.substr(1, 3), w2 = p1().text.substr(21, 3);
        {
            bool at1 = false, at21 = false;
            for (const auto& [a, b] : in1(D::find_all(d, D::FindOpts{.what = w1, .match_case = true, .italic = true}))) at1 = at1 || a == 1;
            for (const auto& [a, b] : in1(D::find_all(d, D::FindOpts{.what = w2, .match_case = true, .italic = true}))) at21 = at21 || a == 21;
            check("find fmt: text only where it is so set", at1 && !at21);
        }
        // Change All: the hand-made italics to Emphasis -- one step; the
        // local italic goes (the style carries it), the local bold stays.
        const auto before = *d.as<core::Story>(S);
        const auto steps = j.size();
        const auto hits = D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"});
        const int n = D::change_hits(j, d, hits, {false, "", "", "style.5"}, "Change All");
        check("change fmt: to Emphasis, one step", n == static_cast<int>(hits.size()) && n > 0 && j.size() == steps + 1);
        check("change fmt: words kept, now Emphasis", p1().text == before.paras[1].text &&
                                                     core::char_style_at(p1(), 1) == "style.5" && core::char_style_at(p1(), 6) == "style.5");
        check("change fmt: the hand-made italic folded into the style",
              core::local_at(p1(), 1).italic == -1 && core::local_at(p1(), 6).italic == -1 && core::local_at(p1(), 6).bold == 1);
        check("change fmt: nothing left to change", in1(D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"})).empty());
        j.undo();
        check("change fmt: undo, all back", *d.as<core::Story>(S) == before);
        D::ChangeOpts keep{false, "", "", "style.5"};
        keep.fold_local = false;
        D::change_hits(j, d, D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"}), keep, "Change All");
        check("change fmt: fold off keeps the local italic", core::local_at(p1(), 1).italic == 1 && core::char_style_at(p1(), 1) == "style.5");
    }

    {
        std::cout << "\n-- swatches: use, groups, view setting (s052) --\n";
        namespace D = core::doc;
        core::Doc d;
        load_plain(d);
        core::Swatch ink{"Test Ink", 99, core::ColorModel::Spot, {0, 0, 0, 0}, "PMS Test", 0x336699};
        core::Swatch rgb{"Test RGB", 99.5, core::ColorModel::RGB, {10, 20, 30, 0}, "", 0x0a141e};
        {
            core::Doc::Loading load(d);
            d.put("swatch.t1", ink);
            d.put("swatch.t2", rgb);
        }
        check("swatch: a new one is unused", D::swatch_uses(d, "swatch.t1") == 0 && D::swatch_uses(d, "") == 0);
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.fill = "swatch.t1";
            f.stroke = "swatch.t1";
            d.put("frame.11", f);
            auto st = *d.as<core::Style>("style.1");
            st.rule_below.color = "swatch.t1";
            d.put("style.1", st);
            auto story = *d.as<core::Story>("story.1");
            story.paras[1].runs = {{0, 3, "", core::Local{-1, -1, 0, "swatch.t1"}}};
            d.put("story.1", story);
        }
        check("swatch: fill, stroke, a rule and a word's colour all count", D::swatch_uses(d, "swatch.t1") == 4);
        const auto o = core::project(d, "");
        const auto field = [&](const char* id, const char* label) {
            const auto* it = o.find(id);
            if (!it) return std::string("?");
            for (const auto& f : it->fields) if (f.label == label) return f.value;
            return std::string("-");
        };
        check("swatch: the tile says where it's used", field("swatch.t1", "Used") == "4 places" && field("swatch.t2", "Used") == "Unused");
        check("swatch: grouped by kind -- spot inks, RGB apart",
              o.find("swatch.t1") && o.find("swatch.t1")->group == "Spot inks" &&
              o.find("swatch.t2") && o.find("swatch.t2")->group.rfind("RGB", 0) == 0);
        bool all_grouped = true;
        for (const auto* it : o.in(core::Segment::Colors)) all_grouped = all_grouped && !it->group.empty();
        check("swatch: every colour has a group", all_grouped);
        const auto dir = std::filesystem::temp_directory_path() / "composix_s052";
        const auto file = (dir / "settings.json").string();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check("settings: first run is empty", core::load_settings(file).empty());
        core::AppSettings a{{"colors.view", "list"}, {"other", "x"}};
        check("settings: round-trip", core::save_settings(file, a) && core::load_settings(file) == a);
        { std::ofstream bad(file); bad << "{not json"; }
        check("settings: unreadable -> empty, no throw", core::load_settings(file).empty());
        std::filesystem::remove_all(dir, ec);
    }

    {
        std::cout << "\n-- colour report (s053) --\n";
        namespace L = composix::layout;
        check("shift: words by Delta E", L::shift_of(1) == L::Shift::Holds && L::shift_of(3) == L::Shift::Slight &&
                                         L::shift_of(7) == L::Shift::Noticeable && L::shift_of(12) == L::Shift::Large);
        {
            plate::Proof bad;
            check("proof: no such file -> not ok", !bad.load("/nonexistent/x.icc") && !bad.ok());
        }
        // Any CMYK press profile on this computer (Fedora, Ubuntu, TeX Live's).
        std::string icc;
        for (const auto& p : plate::find_cmyk_profiles({"/usr/share/color/icc", "/usr/share/ghostscript/iccprofiles",
                                                        "/usr/share/texlive/texmf-dist/tex/generic/colorprofiles",
                                                        "/usr/share/texmf/tex/generic/colorprofiles"}))
            if (icc.empty()) icc = p.path;
        if (icc.empty()) {
            std::cout << "  (skip colour report: no CMYK profile installed)\n";
        } else {
            std::cout << "  (profile: " << icc << ")\n";
            plate::Proof pr;
            check("proof: a press profile loads, named", pr.load(icc) && pr.ok() && !pr.name().empty());
            const auto gray = pr.check(128, 128, 128), blue = pr.check(0, 0, 255);
            check("proof: a mid grey survives (< 5)", gray.delta_e < 5);
            check("proof: sRGB's pure blue is out of any press's gamut (>= 10)", blue.delta_e >= 10);
            check("proof: CMYK in range, blue mostly C + M", blue.cmyk[0] > 50 && blue.cmyk[1] > 50 && blue.cmyk[2] < 30 &&
                                                              blue.cmyk[0] <= 100 && blue.cmyk[3] <= 100);
            std::vector<std::uint8_t> px{0, 0, 255, 128, 128, 128};
            std::vector<float> de;
            pr.proof_pixels(px.data(), 2, &de);
            check("proof: pixels proofed in place, as check says", de.size() == 2 && px[0] == blue.after[0] &&
                                                                   px[2] == blue.after[2] && px[3] == gray.after[0] &&
                                                                   std::abs(de[0] - blue.delta_e) < 0.01);

            core::Doc d;
            load_plain(d);
            {   // Sky made pure blue and used; a placed RGB picture.
                core::Doc::Loading load(d);
                auto sky = *d.as<core::Swatch>("swatch.4");
                sky.v = {0, 0, 255, 0};
                d.put("swatch.4", sky);
                d.put("asset.t", core::Asset{"blue.png", 99, "/nowhere/blue.png", false, "RGB", 4, 4});
                core::Frame f;
                f.owner = d.pages().back();
                f.kind = core::FrameKind::Image;
                f.asset = "asset.t";
                f.w = f.h = 72;
                d.put("frame.t", f);
                auto cap = *d.as<core::Frame>("frame.11");
                cap.fill = "swatch.4";
                d.put("frame.11", cap);
            }
            const auto blue_px = [](const core::Asset&, int& w, int& h, std::vector<std::uint8_t>& rgb) {
                w = h = 4;
                rgb.assign(48, 0);
                for (int i = 0; i < 16; ++i) rgb[static_cast<std::size_t>(i) * 3 + 2] = 255;
                return true;
            };
            const auto rep = L::color_report(d, icc, false, blue_px);
            const auto row = [&](const std::string& id) -> const L::ColorRow* {
                for (const auto& r : rep.rows) if (r.id == id) return &r;
                return nullptr;
            };
            check("report: profile read, named", rep.ok && rep.profile_name == pr.name());
            const auto* sky = row("swatch.4");
            check("report: the RGB swatch, used, before and after", sky && sky->kind == L::ColorRow::Kind::Swatch && sky->used &&
                                                                   sky->before == 0x0000ff && sky->after != sky->before &&
                                                                   sky->shift == L::Shift::Large);
            check("report: CMYK and spot swatches left out", !row("swatch.1") && !row("swatch.3"));
            const auto* pic = row("asset.t");
            check("report: the placed RGB picture, where it is", pic && pic->kind == L::ColorRow::Kind::Image &&
                                                                 pic->page == d.pages().back() && pic->readable);
            check("report: the picture judged on its pixels", pic && pic->w == 4 && pic->h == 4 && pic->share == 1 &&
                                                              pic->shift == L::Shift::Large && pic->px_after.size() == 48 &&
                                                              pic->px_after != pic->px_before);
            check("report: the worst counted", rep.worst_count >= 2);
            const auto unread = L::color_report(d, icc, false, [](const core::Asset&, int&, int&, std::vector<std::uint8_t>&) { return false; });
            check("report: a picture that can't be read says so", [&] {
                for (const auto& r : unread.rows) if (r.id == "asset.t") return !r.readable;
                return false;
            }());
            check("report: a bad profile -> not ok, why", !L::color_report(d, "/nonexistent.icc", false, blue_px).ok);
            {   // s099: the fast screen path agrees with check, colour by colour (both intents).
                bool same = true;
                for (bool per : {false, true}) {
                    plate::Proof pf;
                    pf.load(icc, per);
                    const std::uint8_t cs[][3] = {{0, 0, 255}, {255, 0, 0}, {30, 200, 90}, {255, 255, 255}, {12, 12, 12}, {180, 120, 60}};
                    for (const auto& c : cs) {
                        std::uint8_t px[3] = {c[0], c[1], c[2]};
                        pf.simulate(px, 1);
                        const auto r = pf.check(c[0], c[1], c[2]);
                        for (int k = 0; k < 3; ++k) same = same && std::abs(int(px[k]) - int(r.after[k])) <= 2;
                    }
                }
                check("proof: the screen's fast path looks as check says (within 2 levels)", same);
            }
            {   // s107: the paper, and CMYK inks on screen through the press.
                plate::Proof plain, paper;
                plain.load(icc);
                paper.load(icc, false, nullptr, true);
                std::uint8_t w0[3] = {255, 255, 255}, w1[3] = {255, 255, 255};
                plain.simulate(w0, 1);
                paper.simulate(w1, 1);
                check("proof: without the paper, white stays white", w0[0] >= 250 && w0[1] >= 250 && w0[2] >= 250 && !plain.paper());
                check("proof: with the paper, white is the paper's (not screen white)",
                      paper.paper() && (w1[0] < 253 || w1[1] < 253 || w1[2] < 253) && w1[0] > 180 && w1[1] > 180 && w1[2] > 180,
                      std::to_string(w1[0]) + "," + std::to_string(w1[1]) + "," + std::to_string(w1[2]));
                const double none[4] = {0, 0, 0, 0}, k[4] = {0, 0, 0, 100}, c[4] = {100, 0, 0, 0};
                const auto wn = plain.cmyk_to_screen(none), kk = plain.cmyk_to_screen(k), cc = plain.cmyk_to_screen(c);
                check("CMYK on screen: no ink is white, K is dark, C is cyan",
                      (wn >> 16 & 0xff) >= 245 && (wn & 0xff) >= 245 && (kk >> 16 & 0xff) < 100 &&
                      (cc >> 16 & 0xff) < 120 && (cc & 0xff) > 150);
            }
            {   // s085: the intent asked for, and the proof made with it.
                const auto per = L::color_report(d, icc, true, blue_px);
                const L::ColorRow* psky = nullptr;
                for (const auto& r : per.rows) if (r.id == "swatch.4") psky = &r;
                check("report: proofed perceptually when asked, saying so", per.ok && per.perceptual && !rep.perceptual && psky &&
                                                                            psky->before == 0x0000ff);
            }
            // Preflight: converting, the shifting colour is a warning; not converting, nothing.
            L::FlowCache flows;
            L::PreflightOpts o;
            o.profile = true;
            o.profile_path = icc;
            const auto has_shift = [&] {
                for (const auto& i : L::preflight_issues(d, flows, o)) if (i.kind == "shift") return true;
                return false;
            };
            check("preflight: not converting, no shift warning", !has_shift());
            o.convert = true;
            check("preflight: converting, the out-of-gamut RGB colour warned", has_shift());
        }
    }

    {
        std::cout << "\n-- page range + bookmarks (s054) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        const auto pages = d.pages();
        const auto folios = [&](const std::vector<std::string>& v) {
            std::string out;
            for (const auto& p : v) out += (out.empty() ? "" : ",") + d.folio(p);
            return out;
        };
        std::string why;
        check("range: empty or All -- every page", core::pages_in_range(d, "").size() == pages.size() &&
                                                   core::pages_in_range(d, " all ").size() == pages.size());
        check("range: a run across the sections", folios(core::pages_in_range(d, "iii-1")) == "iii,iv,1");
        check("range: in document order, each once", folios(core::pages_in_range(d, "2, ii, 2")) == "ii,2");
        check("range: an en dash reads as a hyphen", folios(core::pages_in_range(d, "1\u20133")) == "1,2,3");
        check("range: unknown folio refused, named", core::pages_in_range(d, "99", &why).empty() && why.find("99") != std::string::npos);
        check("range: backwards refused", core::pages_in_range(d, "7-2", &why).empty() && why.find("backwards") != std::string::npos);

        L::FlowCache flows;
        const auto bm = L::bookmarks(d, flows);
        const auto find_bm = [&](const std::string& t) -> const L::Bookmark* {
            for (const auto& b : bm) if (b.title.find(t) != std::string::npos) return &b;
            return nullptr;
        };
        const auto* one = find_bm("Chapter One");
        const auto* two = find_bm("Chapter Two");
        check("bookmarks: the chapters, from the contents list's styles", one && two && one->level == 1 && two->level == 1);
        check("bookmarks: where they land, in reading order", one && two && d.folio(one->page) == "1" &&
                                                              d.folio(two->page) == "3" && one < two &&
                                                              one->y > 0 && one->y < d.as<core::Page>(one->page)->h);

        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s054";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto count = [](const std::string& hay, const std::string& n) {
            int k = 0;
            for (auto at = hay.find(n); at != std::string::npos; at = hay.find(n, at + 1)) ++k;
            return k;
        };
        L::PdfOptions opt;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "marks.pdf").string();
        check("print: exports with bookmarks", L::export_print_pdf(d, out, opt, src, &rep, &err), err);
        auto pdf = slurp(out);
        check("print: an outline, opened with it shown", pdf.find("/Outlines") != std::string::npos &&
                                                        pdf.find("/PageMode /UseOutlines") != std::string::npos &&
                                                        pdf.find("/Type /Outlines") != std::string::npos);
        check("print: the chapters in it, each going to its page", pdf.find("(Chapter One)") != std::string::npos &&
                                                                   pdf.find("(Chapter Two)") != std::string::npos &&
                                                                   count(pdf, "/XYZ 0 ") - count(pdf, "/Subtype /Link") == static_cast<int>(bm.size()));   // s083: less the links' own
        opt.only = core::pages_in_range(d, "1-3");
        check("print: a range -- just those pages", L::export_print_pdf(d, out, opt, src, &rep, &err) && rep.pages == 3 &&
                                                    count(slurp(out), "/Type /Page /Parent") == 3, err);
        opt.only = core::pages_in_range(d, "iv");
        L::export_print_pdf(d, out, opt, src, &rep, &err);
        check("print: no chapter in the range, no outline", slurp(out).find("/Outlines") == std::string::npos);
        opt.only.clear();
        opt.bookmarks = false;
        L::export_print_pdf(d, out, opt, src, &rep, &err);
        check("print: bookmarks off, no outline", slurp(out).find("/Outlines") == std::string::npos);
        // Nesting: level 2 under the level 1 before it.
        {
            plate::Document pd;
            pd.set_compress(false);
            pd.add_page(100, 100);
            pd.add_page(100, 100);
            pd.add_bookmark("A", 1, 0, 10);
            pd.add_bookmark("A1", 2, 0, 50);
            pd.add_bookmark("A2", 2, 1, 0);
            pd.add_bookmark("B", 1, 1, 60);
            pd.add_bookmark("Gone", 1, 9, 0);   // no such page: left out
            const auto f = (base / "nest.pdf").string();
            const bool ok = pd.save(f, &err);
            const auto p2 = slurp(f);
            check("plate: bookmarks nest by level, a missing page dropped",
                  ok && p2.find("/Count 4") != std::string::npos && p2.find("/Count 2") != std::string::npos &&
                  p2.find("(Gone)") == std::string::npos && p2.find("/XYZ 0 90 null") != std::string::npos, err);
        }
        // Screen (cairo): the outline too.
        L::PdfOptions so;
        const auto sout = (base / "screen.pdf").string();
        check("screen: exports with bookmarks", L::export_pdf(d, sout, so, src, &rep, &err), err);
        check("screen: an outline in it", slurp(sout).find("/Outlines") != std::string::npos);
        so.only = core::pages_in_range(d, "ii-iii");
        check("screen: a range -- just those pages", L::export_pdf(d, sout, so, src, &rep, &err) && rep.pages == 2, err);
        fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- column rules + breaks (s055) --\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        // A two-column frame of its own, two short paragraphs.
        {
            core::Doc::Loading load(d);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = 360; f.h = 300;
            f.columns = 2;
            f.gutter = 18;
            f.story = "story.t";
            d.put("frame.t", f);
            core::Frame g = f;
            g.y = 360; g.h = 200; g.columns = 1; g.story = "story.t";
            f.next = "frame.t2";
            d.put("frame.t", f);
            d.put("frame.t2", g);
            core::Story st;
            st.name = "Test";
            st.paras = {{"style.1", "First paragraph.", {}}, {"style.1", "Second paragraph.", {}}, {"style.1", "Third.", {}}};
            d.put("story.t", st);
        }
        const auto where = [&](int para) {   // {frame, column} of a paragraph's first line
            const auto fl = L::flow_story(d, "story.t");
            for (const auto& ft : fl.frames)
                for (const auto& ln : ft.lines)
                    if (ln.para == para && ln.start == 0) return ft.frame + ":" + std::to_string(ln.col);
            return std::string("none");
        };
        check("break: none -- all in the first column", where(1) == "frame.t:0" && where(2) == "frame.t:0");
        const auto before = *d.as<core::Story>("story.t");
        const auto steps = j.size();
        const auto c = D::insert_break(j, d, "story.t", {1, 7}, core::ParaStart::NextColumn);   // "Second |paragraph."
        const auto& st = *d.as<core::Story>("story.t");
        check("break: splits the paragraph, one step", st.paras.size() == 4 && j.size() == steps + 1 &&
                                                       st.paras[1].text == "Second " && st.paras[2].text == "paragraph." &&
                                                       c == core::TextPos{2, 0});
        check("break: the rest keeps the style, carries the break", st.paras[2].style == "style.1" &&
                                                                     st.paras[2].brk == core::ParaStart::NextColumn &&
                                                                     st.paras[1].brk == core::ParaStart::Anywhere);
        check("break: column -- what follows starts the next column", where(1) == "frame.t:0" && where(2) == "frame.t:1");
        j.undo();
        check("break: undo, all back", *d.as<core::Story>("story.t") == before);
        D::insert_break(j, d, "story.t", {2, 0}, core::ParaStart::NextFrame);
        check("break: at a paragraph's start -- no split, that one moves on", d.as<core::Story>("story.t")->paras.size() == 3 &&
                                                                               where(2) == "frame.t2:0" && where(1) == "frame.t:0");
        D::insert_break(j, d, "story.t", {2, 0}, core::ParaStart::Anywhere);
        check("break: taken off again", where(2) == "frame.t:0");
        // Typing keeps a break where it was; Enter's new paragraph has none.
        D::insert_break(j, d, "story.t", {1, 0}, core::ParaStart::NextColumn);
        D::edit_text(j, d, "story.t", {1, 6}, {1, 6}, "X");
        D::edit_text(j, d, "story.t", {1, 18}, {1, 18}, "\n");
        check("break: survives typing; Enter's paragraph has none", d.as<core::Story>("story.t")->paras[1].brk == core::ParaStart::NextColumn &&
                                                                    d.as<core::Story>("story.t")->paras[2].brk == core::ParaStart::Anywhere);
        // Column rules.
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.t");
            f.columns = 3;
            f.col_rule = {1, "swatch.3", 0.5, 6};
            f.inset = {10, 0, 4, 0};
            d.put("frame.t", f);
        }
        const auto& fr = *d.as<core::Frame>("frame.t");
        const auto rules = L::column_rules(fr);
        bool mid = rules.size() == 2;
        for (int k = 0; mid && k < 2; ++k) {
            const double gap0 = core::column_x(fr, k) + core::column_width(fr, k), gap1 = core::column_x(fr, k + 1);
            const auto& r = rules[static_cast<std::size_t>(k)].r;
            mid = std::abs(r.x + r.w / 2 - (gap0 + gap1) / 2) < 1e-6 && r.w == 1 && r.y == 16 && std::abs(r.y + r.h - (300 - 4 - 6)) < 1e-6;
        }
        check("rules: one down each gutter, inset top and bottom", mid && rules[0].color == "swatch.3" && rules[0].tint == 0.5);
        {
            auto one = fr;
            one.columns = 1;
            auto none = fr;
            none.col_rule.weight = 0;
            check("rules: none for one column, or no weight", L::column_rules(one).empty() && L::column_rules(none).empty());
        }
        // Kept in the file.
        const auto json = core::to_json(d);
        core::Doc e;
        std::string err;
        check("json: column rule and break round-trip", core::from_json(e, json, &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- find: patterns + scope (s056) --\n";
        namespace D = core::doc;
        using M = D::FindOpts::Mode;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        {
            core::Doc::Loading load(d);
            core::Story st;
            st.name = "Patterns";
            st.paras = {{"style.1", "Call 555-1234 or 555-9876.", {}},
                        {"style.1", "Caf\u00e9 cr\u00e8me, na\u00efve r\u00e9sum\u00e9.", {}},
                        {"style.1", "Chapter 12 and chapter 3.", {}}};
            d.put("story.f", st);
        }
        const auto find = [&](D::FindOpts f) { f.story = f.story.empty() ? "story.f" : f.story; return D::find_all(d, f); };
        auto h = find({.what = "^9^9^9-^9^9^9^9", .mode = M::Wildcards});
        check("wildcards: ^9 a digit", h.size() == 2 && h[0].a == core::TextPos{0, 5} && h[0].b == core::TextPos{0, 13});
        h = find({.what = "^$^$^$^$", .whole_word = true, .mode = M::Wildcards});
        check("wildcards: ^$ a letter -- accented ones too, by character",
              std::any_of(h.begin(), h.end(), [](const D::Hit& x) { return x.a == core::TextPos{1, 0} && x.b == core::TextPos{1, 5}; }) &&
              h.front().a == core::TextPos{0, 0});   // "Call" too
        h = find({.what = "Caf^?", .match_case = true, .mode = M::Wildcards});
        check("wildcards: ^? one character (two bytes here)", h.size() == 1 && h[0].b == core::TextPos{1, 5});
        check("wildcards: the rest is plain (a dot is a dot)", find({.what = "^9.", .mode = M::Wildcards}).size() == 2 &&
                                                               find({.what = "^9", .mode = M::Text}).empty());
        h = find({.what = "chapter (\\d+)", .mode = M::Regex});
        check("regex: matches, case folded, groups kept", h.size() == 2 && h[0].groups.size() == 2 && h[0].groups[1] == "12" &&
                                                           h[1].groups[1] == "3");
        check("regex: match case", find({.what = "chapter (\\d+)", .match_case = true, .mode = M::Regex}).size() == 1);
        std::string why;
        check("regex: a bad pattern says why, finds nothing",
              !D::find_pattern_ok({.what = "(abc", .mode = M::Regex}, &why) && why == "unbalanced ( )" &&
              find({.what = "(abc", .mode = M::Regex}).empty() && D::find_pattern_ok({.what = "(abc"}));
        check("regex: an empty match finds nothing", find({.what = "x*", .mode = M::Regex}).empty());
        check("groups: $1 $& $$, unknown left", D::expand_groups("$$1 $& $9 $2", {"ab", "a"}) == "$1 ab $9 $2");
        const auto steps = j.size();
        const int n = D::change_hits(j, d, h, {.with = "Ch. $1", .expand = true}, "Change All");
        check("change: groups put in, one step", n == 2 && j.size() == steps + 1 &&
                                                 d.as<core::Story>("story.f")->paras[2].text == "Ch. 12 and Ch. 3.");
        j.undo();
        // Scope.
        check("scope: one story", find({.what = "the", .story = "story.1"}).size() > 0 && find({.what = "the"}).empty());
        check("scope: every story", D::find_all(d, {.what = "555"}).size() == 2);
        h = find({.what = "^9", .mode = M::Wildcards, .in_range = true, .from = {0, 5}, .to = {0, 8}});
        check("scope: a selection -- only inside it", h.size() == 3 && h.back().b == core::TextPos{0, 8});
        h = find({.what = "chapter", .in_range = true, .from = {1, 0}, .to = {2, 7}});
        check("scope: a selection across paragraphs", h.size() == 1 && h[0].a == core::TextPos{2, 0});
        h = find({.pstyle = "style.1", .in_range = true, .from = {0, 20}, .to = {1, 4}});
        check("scope: a style alone -- each paragraph's part in it", h.size() == 2 && h[0].a == core::TextPos{0, 20} &&
                                                                     h[1].b == core::TextPos{1, 4});
    }

    {
        std::cout << "\n-- saved state (s057) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        j.clear();
        const auto saved = j.mark();
        D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "A", "Typing", "k");
        const auto one = j.mark();
        check("mark: a step moves it", one != saved);
        j.undo();
        check("mark: undo back to the save -- the same mark (nothing to save)", j.mark() == saved);
        j.redo();
        check("mark: redo -- the step's own mark again", j.mark() == one);
        D::edit_text(j, d, "story.1", {1, 1}, {1, 1}, "B", "Typing", "k");   // merges into the same step
        check("mark: a merge into the last step is a new state", j.size() == 1 && j.mark() != one && j.mark() != saved);
        const auto two = j.mark();
        j.undo();
        check("mark: undo of the merged step -- back to the save", j.mark() == saved);
        j.redo();
        check("mark: and forward", j.mark() == two);
        j.clear();
        check("mark: a cleared journal is a fresh start", j.mark() != saved && j.mark() != two);
        core::AppSettings st{{"w", "1400"}, {"bad", "12px"}, {"neg", "-5"}};
        check("settings: a number, clamped; missing or not one -> the fallback",
              core::setting_int(st, "w", 1, 0, 2000) == 1400 && core::setting_int(st, "w", 1, 0, 1000) == 1000 &&
              core::setting_int(st, "bad", 7, 0, 99) == 7 && core::setting_int(st, "none", 3, 0, 9) == 3 &&
              core::setting_int(st, "neg", 3, 0, 9) == 0);
    }

    {
        std::cout << "\n-- swatch editor (s058) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        using CM = core::ColorModel;
        check("preview: RGB as is, grey, CMYK by the ink model",
              D::swatch_preview({"a", 0, CM::RGB, {255, 128, 0, 0}}) == 0xff8000 &&
              D::swatch_preview({"g", 0, CM::Gray, {100, 0, 0, 0}}) == 0x000000 &&
              D::swatch_preview({"g", 0, CM::Gray, {0, 0, 0, 0}}) == 0xffffff &&
              D::swatch_preview({"c", 0, CM::CMYK, {100, 0, 0, 0}}) == 0x00ffff &&
              D::swatch_preview({"k", 0, CM::CMYK, {0, 0, 0, 100}}) == 0x000000);
        {
            core::Swatch s{"x", 0, CM::CMYK};
            D::swatch_from_rgb(s, 0xff0000);
            core::Swatch g{"y", 0, CM::Gray};
            D::swatch_from_rgb(g, 0x808080);
            check("from RGB: red is M+Y, grey is a percent", s.v[0] == 0 && s.v[1] == 100 && s.v[2] == 100 && s.v[3] == 0 &&
                                                             g.v[0] >= 49 && g.v[0] <= 50);
        }
        const auto steps = j.size();
        const auto made = D::add_swatch(j, d, {"Teal", 0, CM::CMYK, {80, 0, 30, 10}});
        const auto* t = d.as<core::Swatch>(made);
        double last = 0;
        for (const auto& id : d.ids_of<core::Swatch>()) if (id != made) last = std::max(last, d.as<core::Swatch>(id)->pos);
        check("add: one step, after the others, its preview made", t && j.size() == steps + 1 && t->pos > last &&
                                                                   t->preview == D::swatch_preview(*t) && t->name == "Teal");
        auto e = *t;
        e.v = {0, 0, 0, 50};
        e.preview = 0x123456;   // ignored: made from the values
        check("edit: one step, preview follows", D::edit_swatch(j, d, made, e) && d.as<core::Swatch>(made)->preview == 0x808080);
        check("edit: the same again -- no step", !D::edit_swatch(j, d, made, *d.as<core::Swatch>(made)));
        {   // a swatch's own preview (the sample's tuned rust) survives an edit that leaves its values.
            auto rust = *d.as<core::Swatch>("swatch.3");
            const auto was = rust.preview;
            rust.name = "Rust";
            check("edit: the name only -- its own preview kept", D::edit_swatch(j, d, "swatch.3", rust) &&
                                                                 d.as<core::Swatch>("swatch.3")->preview == was);
            j.undo();
        }
        const auto copy = D::duplicate_swatch(j, d, "swatch.3");
        check("duplicate: \"<name> copy\", same ink", d.as<core::Swatch>(copy) && d.as<core::Swatch>(copy)->name == "Guild Rust copy" &&
                                                      d.as<core::Swatch>(copy)->spot == d.as<core::Swatch>("swatch.3")->spot);
        check("delete: a used one stays", !D::delete_swatch(j, d, "swatch.3") && d.as<core::Swatch>("swatch.3"));
        check("delete: an unused one goes, one step; undo brings it back",
              D::delete_swatch(j, d, made) && !d.as<core::Swatch>(made) && (j.undo(), d.as<core::Swatch>(made) != nullptr));
        // A frame using it follows the edit (the same id, new values).
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.fill = made;
            d.put("frame.11", f);
        }
        auto e2 = *d.as<core::Swatch>(made);
        e2.model = CM::RGB;
        e2.v = {10, 200, 30, 0};
        D::edit_swatch(j, d, made, e2);
        check("edit: what uses it follows (same swatch)", d.as<core::Frame>("frame.11")->fill == made &&
                                                         d.as<core::Swatch>(made)->preview == 0x0ac81e);
    }

    {
        std::cout << "\n-- hidden characters (s059) --\n";
        namespace L = composix::layout;
        core::Doc d;
        load_plain(d);
        {
            core::Doc::Loading load(d);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = 360; f.h = 300;
            f.columns = 2;
            f.story = "story.h";
            d.put("frame.h", f);
            core::Story st;
            st.name = "Hidden";
            st.paras = {{"style.1", "A b\tc", {}}, {"style.1", "D", {}}, {"style.1", "", {}}};
            st.paras[1].brk = core::ParaStart::NextColumn;
            d.put("story.h", st);
        }
        const auto flow = L::flow_story(d, "story.h");
        const auto marks = L::hidden_marks(d, "story.h", flow, "frame.h");
        using K = L::HiddenMark::Kind;
        const auto count = [&](K k) { return std::count_if(marks.begin(), marks.end(), [&](const L::HiddenMark& m) { return m.kind == k; }); };
        check("hidden: a space, a tab, a break, a paragraph end, the story's end",
              count(K::Space) == 1 && count(K::Tab) == 1 && count(K::Break) == 1 && count(K::Para) == 1 && count(K::End) == 1);
        const auto first = [&](K k) { return *std::find_if(marks.begin(), marks.end(), [&](const L::HiddenMark& m) { return m.kind == k; }); };
        check("hidden: each where it is -- the space before the tab, a span each",
              first(K::Space).x1 > first(K::Space).x0 && first(K::Tab).x1 > first(K::Tab).x0 && first(K::Tab).x0 >= first(K::Space).x1 &&
              first(K::Break).brk == 1 && first(K::Break).x0 >= first(K::Tab).x1);
        check("hidden: the break's paragraph in the next column -- its end further right",
              first(K::Para).x0 > static_cast<double>(core::column_x(*d.as<core::Frame>("frame.h"), 1)));
        check("hidden: none for a frame that doesn't hold the story", L::hidden_marks(d, "story.h", flow, "frame.11").empty());
    }

    {
        std::cout << "\n-- section editing (s060) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto secs = d.ids_of<core::Section>();
        check("fixture: two sections (front matter, body)", secs.size() >= 2);
        const auto body = secs.back();
        const auto first_page = d.as<core::Section>(body)->start_page;
        const auto steps = j.size();
        check("rename: trimmed, one step", D::rename_section(j, d, body, "  Part One ") && j.size() == steps + 1 &&
                                           d.as<core::Section>(body)->name == "Part One");
        check("rename: empty refused", !D::rename_section(j, d, body, "   ") && d.as<core::Section>(body)->name == "Part One");
        check("rename: Section Name markers read it", L::marker_value(d, 1, first_page, "") == "Part One");
        const auto was = d.folio(first_page);
        check("starts at: the first page takes it", D::set_section_start_at(j, d, {body}, 5) && d.folio(first_page) == "5");
        check("starts at: 0 refused", !D::set_section_start_at(j, d, {body}, 0));
        j.undo();
        check("starts at: undo", d.folio(first_page) == was);
        std::string m0;   // a master the section hasn't got
        for (const auto& m : d.ids_of<core::Master>()) if (m != d.as<core::Section>(body)->master) { m0 = m; break; }
        check("master: set, or a missing one refused", D::set_section_master(j, d, {body}, m0) &&
                                                       d.as<core::Section>(body)->master == m0 &&
                                                       !D::set_section_master(j, d, {body}, "master.none"));
        const auto o = core::project(d, "");
        const auto has_remove = [&](const std::string& id) {
            const auto* it = o.find(id);
            if (!it) return false;
            for (const auto& f : it->fields) if (f.edit == "section") return true;
            return false;
        };
        const auto name_field = [&](const std::string& id) {
            for (const auto& f : o.find(id)->fields) if (f.edit == "sname") return f.text && f.value == d.as<core::Section>(id)->name;
            return false;
        };
        check("inspector: Name typed; Remove offered except for the first section", name_field(body) && has_remove(body) &&
                                                                                     !has_remove(secs.front()));
    }

    {
        std::cout << "\n-- caret follow (s061) --\n";
        core::Viewport vp;
        vp.set_widget(800, 600);
        vp.set_content({0, 0, 5000, 5000});
        vp.set_origin(0, 0);
        check("reveal: already in view -- no move", !vp.reveal({100, 100, 4, 12}, 40) && vp.ox() == 0 && vp.oy() == 0);
        check("reveal: below -- pans up just enough", vp.reveal({100, 700, 4, 12}, 40) &&
                                                      std::abs(vp.oy() - (600 - 40 - 712)) < 1e-9 && vp.ox() == 0);
        vp.set_origin(0, 0);
        check("reveal: to the right and above", (vp.set_origin(0, -500), vp.reveal({1000, 100, 4, 12}, 40)) &&
                                                std::abs(vp.ox() - (800 - 40 - 1004)) < 1e-9 && std::abs(vp.oy() - (40 - 100)) < 1e-9);
        // Where a change ends, in the text after it.
        core::Story a, b;
        a.paras = {{"s", "The harbor", {}}, {"s", "smelled", {}}};
        b = a;
        b.paras[0].text = "The old harbor";
        check("changed end: text came back -- after it", core::changed_end(a, b) == core::TextPos{0, 8});
        check("changed end: text went -- where it was", core::changed_end(b, a) == core::TextPos{0, 4});
        b = a;
        b.paras = {{"s", "The harbor", {}}, {"s", "", {}}, {"s", "smelled", {}}};
        check("changed end: a paragraph came back -- across the break", core::changed_end(a, b) == core::TextPos{1, 0} ||
                                                                         core::changed_end(a, b) == core::TextPos{2, 0});
        check("changed end: the same -- the start", core::changed_end(a, a) == core::TextPos{0, 0});
        b = a;
        b.paras[1].text = "smelled \u00e9";
        check("changed end: never inside a character", core::changed_end(a, b) == core::TextPos{1, 10});
        // A whole paragraph selected (triple-click): every line lit across, not
        // just its first and last (a line's end read as the next line's start).
        {
            namespace L = composix::layout;
            core::Doc d;
            load_plain(d);
            {
                core::Doc::Loading load(d);
                core::Frame f;
                f.owner = d.pages().back();
                f.kind = core::FrameKind::Text;
                f.x = 36; f.y = 36; f.w = 200; f.h = 400;
                f.story = "story.s";
                d.put("frame.s", f);
                core::Story st;
                st.name = "Sel";
                st.paras = {{"style.1", "The harbor smelled of diesel and oranges the morning the ship came in, and nobody on "
                                        "the quay had expected her before the equinox and fewer still the cargo she carried.", {}}};
                d.put("story.s", st);
            }
            const auto fl = L::flow_story(d, "story.s");
            const int n = static_cast<int>(d.as<core::Story>("story.s")->paras[0].text.size());
            const auto rects = L::selection_rects(d, "story.s", fl, "frame.s", {0, 0}, {0, n});
            bool wide = rects.size() >= 3;
            for (std::size_t k = 0; wide && k + 1 < rects.size(); ++k) wide = rects[k].w > 100;
            check("selection: a paragraph's middle lines lit across", wide);
        }
    }

    {
        std::cout << "\n-- page margins + master drop (s062) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::string pg, other;   // two pages on the same master
        for (const auto& p : d.pages()) {
            if (d.as<core::Page>(p)->master.empty()) continue;
            if (pg.empty()) pg = p;
            else if (d.as<core::Page>(p)->master == d.as<core::Page>(pg)->master) { other = p; break; }
        }
        check("fixture: two pages on one master", !pg.empty() && !other.empty());
        const auto before = core::margin_rect(d, pg), other_before = core::margin_rect(d, other);
        const auto steps = j.size();
        check("own margins: one step, from the master's", D::edit_page_margins(j, d, {pg}, "Top margin", [](core::Margins& m) { m.top = 100; }) &&
                                                         j.size() == steps + 1 && d.as<core::Page>(pg)->margins &&
                                                         d.as<core::Page>(pg)->margins->bottom == core::margins_of(d, other)->bottom);
        const auto after = core::margin_rect(d, pg);
        check("own margins: this page's text area moves; its neighbour's doesn't",
              std::abs(after.y - 100) < 1e-9 && std::abs(after.h - (before.h - (100 - before.y))) < 1e-9 &&
              core::margin_rect(d, other) == other_before);
        const auto json = core::to_json(d);
        core::Doc e;
        std::string err;
        check("own margins: kept in the file", core::from_json(e, json, &err) && e.records() == d.records(), err);
        check("reset: back to the master's, one step", D::reset_page_margins(j, d, {pg}) && !d.as<core::Page>(pg)->margins &&
                                                       core::margin_rect(d, pg) == before);
        // A master dropped on pages is set_master: one step, those pages.
        std::string m2;
        for (const auto& m : d.ids_of<core::Master>()) if (m != d.as<core::Page>(pg)->master) { m2 = m; break; }
        const auto s2 = j.size();
        check("master drop: the pages take it, one step", D::set_master(j, d, {pg, other}, m2) && j.size() == s2 + 1 &&
                                                         d.as<core::Page>(pg)->master == m2 && d.as<core::Page>(other)->master == m2);
    }
}
}  // namespace cxtest
