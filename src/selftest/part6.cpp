// composix_selftest, part 6 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part6() {

    {
        std::cout << "\n-- progress you can see (s102) --\n";
        namespace L = composix::layout;
        core::Doc d;
        (void)L::load_big_book(d, {.pages = 20});
        const auto dir = std::filesystem::temp_directory_path();
        for (const bool print : {false, true}) {
            L::PdfOptions o;
            int calls = 0;
            double last = -1;
            bool rising = true, wrote = false;
            o.progress = [&](double f, const std::string& line) {
                ++calls;
                rising = rising && f >= last;
                last = f;
                wrote = wrote || line.rfind("Writing page", 0) == 0;
            };
            std::string err;
            const auto path = (dir / (print ? "cx_progress_print.pdf" : "cx_progress_screen.pdf")).string();
            const bool ok = print ? L::export_print_pdf(d, path, o, {}, nullptr, &err) : L::export_pdf(d, path, o, {}, nullptr, &err);
            check(std::string(print ? "print" : "screen") + " PDF: progress page by page, rising, near the end",
                  ok && calls > static_cast<int>(d.pages().size()) && rising && wrote && last > 0.85, err);
            std::filesystem::remove(path);
        }
        // A layout made from a copy, handed to the document it was copied from.
        core::Doc copy;
        {
            core::Doc::Loading load(copy);
            for (const auto& [id, r] : d.records()) copy.put(id, r);
        }
        L::FlowCache made;
        int ticks = 0, total = 0;
        L::flow_all(copy, made, [&](int done, int t) { ticks = done; total = t; });
        check("flow_all: counts every frame of every story", total > 0 && ticks == total);
        const auto* before = &made.get(copy, "story.1");
        const int nlines = before->lines;
        made.rebase(d);
        const auto* after = &made.get(d, "story.1");
        check("a handed-over layout is the document's own (kept, not flowed again)", after == before && after->lines == nlines && nlines > 0);
    }


    {
        std::cout << "\n-- thumbnails on a worker (s103) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const L::ImageSource none;
        const auto same_pixels = [](const L::Surface& a, const L::Surface& b) {
            if (!a || !b) return false;
            const int w = cairo_image_surface_get_width(a.get()), h = cairo_image_surface_get_height(a.get());
            if (w != cairo_image_surface_get_width(b.get()) || h != cairo_image_surface_get_height(b.get())) return false;
            const int st = cairo_image_surface_get_stride(a.get());
            return std::memcmp(cairo_image_surface_get_data(a.get()), cairo_image_surface_get_data(b.get()),
                               static_cast<std::size_t>(st) * h) == 0;
        };
        // What the UI thread would draw, page 5, 120 x 180.
        const auto here = L::render_thumb(d, flows, "page.5", 120, 180, false, none);

        L::ThumbStore st;
        st.set_threaded(true);
        auto lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: a miss is asked for, nothing to paint yet", !lk.surf && !lk.current && st.asking());
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.look(d, flows, none, "page.6", "page.6", 120, 180, false);
        check("worker: asked once per page", st.flush(d, flows, none) && !st.asking());
        st.wait_idle();
        auto ids = st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: back, and current", ids.count("page.5") && ids.count("page.6") && lk.current && st.renders() == 2);
        check("worker: draws the page exactly as the UI thread", same_pixels(lk.surf, here));
        check("worker: one snapshot for the revision", st.snapshots() == 1);

        // A change: the page's last picture shows (stale) until the new one.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then ", "Typing", "");
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: after a change the old picture stands in", lk.surf && !lk.current);
        // The worker held: two asks for the page, the document changed between.
        st.hold(true);
        (void)st.flush(d, flows, none);
        const auto old_key = L::page_key(d, flows, "page.5");
        const auto at_old = L::render_thumb(d, flows, "page.5", 120, 180, false, none);
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "And ", "Typing", "");
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.flush(d, flows, none);
        const int before = st.renders();
        st.hold(false);
        st.wait_idle();
        ids = st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: the newest ask wins, the older is dropped", st.renders() == before + 1 && lk.current);
        check("worker: drawn from the newest revision", !same_pixels(lk.surf, at_old) &&
                                                       same_pixels(lk.surf, L::render_thumb(d, flows, "page.5", 120, 180, false, none)));
        // The snapshot is the document as it was: changes after it don't reach it.
        st.hold(true);
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "So ", "Typing", "");
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.flush(d, flows, none);
        const auto snap_pic = L::render_thumb(d, flows, "page.5", 120, 180, false, none);
        j.undo();   // the live document moves on while the worker waits
        st.hold(false);
        st.wait_idle();
        (void)st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: reads its snapshot, not the live document", !lk.current && lk.surf && same_pixels(lk.surf, snap_pic));
        (void)old_key;
        // Another document: what was asked doesn't come back.
        st.hold(true);
        (void)st.flush(d, flows, none);
        st.forget();
        st.hold(false);
        st.wait_idle();
        check("worker: forget drops what's queued and finished", st.collect().empty() && st.kept() == 0 && st.bytes() == 0);

        // The cap: the least recently looked at go first.
        L::ThumbStore lru(3 * 120 * 180 * 4);
        for (const char* p : {"page.1", "page.2", "page.3", "page.4"})
            (void)lru.look(d, flows, none, p, p, 120, 180, false);
        check("cap: kept within its bytes", lru.bytes() <= lru.cap() && lru.kept() == 3);
        check("cap: the oldest let go, the newest kept",
              lru.look(d, flows, none, "page.4", "page.4", 120, 180, false).current && lru.renders() == 4);
        (void)lru.look(d, flows, none, "page.1", "page.1", 120, 180, false);
        check("cap: a page let go is drawn again when looked at", lru.renders() == 5);
    }

    {
        std::cout << "\n-- the store's general form: a variant, a renderer, a mode (s104) --\n";
        namespace L = composix::layout;
        using Miss = L::ThumbStore::Miss;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        int drawn = 0;
        const auto at = [&](int w) {
            return L::ThumbStore::Render([&drawn, w](const core::Doc& doc, L::FlowCache& fl, const L::ImageSource& im) {
                ++drawn;
                return L::render_thumb(doc, fl, "page.5", w, w * 3 / 2, false, im);
            });
        };
        L::ThumbStore st;
        auto lk = st.look(d, flows, {}, "p5", "page.5", "z1", 100, 150, at(100), Miss::NowIfNone);
        check("NowIfNone: nothing yet -> drawn on the spot, current", lk.current && lk.surf && drawn == 1 && !st.asking());
        lk = st.look(d, flows, {}, "p5", "page.5", "z2", 120, 180, at(120), Miss::NowIfNone);
        check("NowIfNone: another variant -> the last stands in, the new is asked",
              !lk.current && lk.surf && cairo_image_surface_get_width(lk.surf.get()) == 100 && st.asking() && drawn == 1);
        st.hold(true);
        (void)st.flush(d, flows, {});
        // Meanwhile the page is drawn on the spot (an edit, say) for z2: the
        // worker's answer, asked before, must not replace it.
        lk = st.look(d, flows, {}, "p5", "page.5", "z3", 140, 210, at(140), Miss::Now);
        check("Now: drawn on the spot", lk.current && drawn == 2);
        st.hold(false);
        st.wait_idle();
        (void)st.collect();
        lk = st.look(d, flows, {}, "p5", "page.5", "z3", 140, 210, at(140), Miss::NowIfNone);
        check("an older answer doesn't replace a newer picture", lk.current && drawn == 3 && st.renders() == 3);
        lk = st.look(d, flows, {}, "p5", "page.5", "z1", 100, 150, at(100), Miss::Ask);
        check("Ask: a variant back again is asked, not drawn here", !lk.current && st.asking() && drawn == 3);
    }

    {
        std::cout << "\n-- a long book's tiles off the UI thread (s103) --\n";
        namespace L = composix::layout;
        core::Doc d;
        (void)L::load_big_book(d, {.pages = 40});
        L::FlowCache flows;
        L::flow_all(d, flows, {});
        L::ThumbStore st;
        st.set_threaded(true);
        const auto pages = d.pages();
        for (const auto& p : pages) (void)st.look(d, flows, {}, p, p, 80, 120, false);
        (void)st.flush(d, flows, {});
        st.wait_idle();
        const auto ids = st.collect();
        bool all = ids.size() == pages.size();
        for (const auto& p : pages) all = all && st.look(d, flows, {}, p, p, 80, 120, false).current;
        check("every page of a 40-page book drawn on the worker, current", all && st.renders() == static_cast<int>(pages.size()));
    }
    {
        std::cout << "\n-- CMYK drawn through the press profile (s107) --\n";
        core::Swatch cm{"Ink", 1, core::ColorModel::CMYK, {10, 20, 30, 40}, "", 0x123456};
        core::Swatch rgb{"Sky", 2, core::ColorModel::RGB, {10, 20, 30, 0}, "", 0x0a141e};
        const auto g0 = core::doc::screen_gen();
        check("no profile: the preview", core::doc::screen_rgb(cm) == 0x123456 && core::doc::screen_rgb(rgb) == 0x0a141e);
        core::doc::set_screen_cmyk([](const double v[4]) { return static_cast<std::uint32_t>(v[0] * 1000 + v[3]); });
        check("a profile: CMYK through it, RGB as it is, and caches told",
              core::doc::screen_rgb(cm) == 10040 && core::doc::screen_rgb(rgb) == 0x0a141e && core::doc::screen_gen() > g0);
        core::Swatch spot = cm;
        spot.model = core::ColorModel::Spot;
        check("a spot: its CMYK alternate through it", core::doc::screen_rgb(spot) == 10040);
        core::doc::set_screen_cmyk({});
        check("back to the ink model", core::doc::screen_rgb(cm) == 0x123456);
    }

    {
        std::cout << "\n-- a keystroke re-flows from the edit (s105) --\n";
        namespace L = composix::layout;
        const auto same_flow = [](const L::StoryFlow& a, const L::StoryFlow& b, std::string& why) {
            if (a.overset != b.overset) { why = "overset"; return false; }
            if (a.frames.size() != b.frames.size()) { why = "frames"; return false; }
            for (std::size_t i = 0; i < a.frames.size(); ++i) {
                const auto& x = a.frames[i].lines;
                const auto& y = b.frames[i].lines;
                if (x.size() != y.size()) { why = "frame " + std::to_string(i) + " lines " + std::to_string(x.size()) + " vs " + std::to_string(y.size()); return false; }
                for (std::size_t k = 0; k < x.size(); ++k) {
                    const auto& l = x[k];
                    const auto& m = y[k];
                    if (l.para != m.para || l.start != m.start || l.end != m.end || std::abs(l.y - m.y) > 1e-9 ||
                        l.h != m.h || l.col != m.col || std::abs(l.x - m.x) > 1e-9 || std::abs(l.w - m.w) > 1e-9 ||
                        l.cap != m.cap || std::abs(l.base - m.base) > 1e-9 || l.hyphen != m.hyphen) {
                        why = "frame " + std::to_string(i) + " line " + std::to_string(k);
                        return false;
                    }
                }
                if (a.frames[i].from_p != b.frames[i].from_p || a.frames[i].from_b != b.frames[i].from_b) {
                    why = "frame " + std::to_string(i) + " start";
                    return false;
                }
                const auto& na = a.frames[i].notes;   // s109
                const auto& nb = b.frames[i].notes;
                bool notes_same = na.size() == nb.size();
                for (std::size_t k = 0; notes_same && k < na.size(); ++k)
                    notes_same = na[k].para == nb[k].para && na[k].k == nb[k].k && na[k].number == nb[k].number &&
                                 na[k].col == nb[k].col && std::abs(na[k].y - nb[k].y) < 1e-9 && std::abs(na[k].h - nb[k].h) < 1e-9;
                if (!notes_same) {
                    why = "frame " + std::to_string(i) + " notes";
                    return false;
                }
            }
            return true;
        };
        {
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            (void)L::load_big_book(d, {.pages = 24});
            L::FlowCache cache;
            (void)cache.get(d, "story.1");
            std::vector<std::string> ids;
            core::doc::edit_text(j, d, "story.1", {5, 0}, {5, 0}, "x", "Typing", "");
            check("a keystroke writes only its story", d.changes_since(d.revision() - 1, ids) && ids.size() == 1 && ids[0] == "story.1");
            // Edits of every kind, at places through the book: each re-flowed
            // flow must be the whole flow, line for line.
            std::uint32_t seed = 7;
            const auto rnd = [&](std::uint32_t n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % std::max(1u, n); };
            const char* inserts[] = {"x", "Lorem ipsum dolor sit amet, ", "\n", "a much longer run of words to push a line or two on, and more words still to make it so ", core::kMarkNote, "\nA new paragraph.\n"};
            int bad = 0, reflowed = 0, kept = 0, partial = 0;
            std::string why;
            for (int n = 0; n < 40; ++n) {
                const auto* st = d.as<core::Story>("story.1");
                const int np = static_cast<int>(st->paras.size());
                const int p = static_cast<int>(rnd(static_cast<std::uint32_t>(np)));
                const auto& t = st->paras[static_cast<std::size_t>(p)].text;
                // A word boundary (ASCII greeking: any space).
                int at = 0;
                for (int tries = 0; tries < 8; ++tries) {
                    const int k = static_cast<int>(rnd(static_cast<std::uint32_t>(t.size() + 1)));
                    if (k == 0 || k == static_cast<int>(t.size()) || t[static_cast<std::size_t>(k - 1)] == ' ') { at = k; break; }
                }
                core::TextPos a{p, at}, b = a;
                const int kind = static_cast<int>(rnd(6));
                if (n % 5 == 3 && p + 1 < np) b = {p + 1, 0};                                   // join with the next
                else if (n % 7 == 4 && at + 5 <= static_cast<int>(t.size()) && t.find(' ', static_cast<std::size_t>(at)) != std::string::npos)
                    b = {p, static_cast<int>(t.find(' ', static_cast<std::size_t>(at)))};       // a word taken out
                core::doc::edit_text(j, d, "story.1", a, b, inserts[kind], "Typing", "");
                const auto& inc = cache.get(d, "story.1");
                reflowed += cache.last_how() == L::FlowCache::How::Reflowed;
                kept += cache.last_how() == L::FlowCache::How::Kept;   // an edit that changed nothing
                partial += inc.filled < static_cast<int>(inc.frames.size()) / 2;
                const auto whole = L::flow_story(d, "story.1");
                if (!same_flow(inc, whole, why)) {
                    if (bad++ == 0) check("re-flow == whole flow (first difference)", false, "edit " + std::to_string(n) + ": " + why);
                }
            }
            check("40 edits through a 24-page book: every re-flow is the whole flow", bad == 0, std::to_string(bad) + " differ");
            check("each was re-flowed, not flowed whole", reflowed + kept == 40 && reflowed >= 30,
                  std::to_string(reflowed) + " re-flowed, " + std::to_string(kept) + " no change");
            check("most set fewer than half the frames", partial >= 10, std::to_string(partial) + " of 40");
            // Undo and redo are edits too.
            j.undo();
            const bool u = same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why);
            j.redo();
            const bool r = same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why);
            check("undo / redo: re-flowed as the whole", u && r, why);
            // Another story's flow is kept through an edit of this one.
            const auto* title = &cache.get(d, "story.2");
            core::doc::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "y", "Typing", "");
            (void)cache.get(d, "story.1");
            check("an edit leaves other stories' flows as they were", &cache.get(d, "story.2") == title &&
                                                                     cache.last_how() == L::FlowCache::How::Kept);
            // Anything but text: the whole again.
            core::doc::edit_frames(j, d, {d.as<core::Story>("story.1") ? L::thread_frames(d, "story.1").front() : ""}, "Gutter",
                                   [](core::Frame& f) { f.gutter += 1; });
            (void)cache.get(d, "story.1");
            check("a frame changed: flowed whole", cache.last_how() == L::FlowCache::How::Whole);
        }
        {   // s106: a budget -- the far end provisional, then the whole handed in.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            (void)L::load_big_book(d, {.pages = 24});
            L::FlowCache cache;
            cache.set_reflow_budget(2);
            (void)cache.get(d, "story.1");
            const auto valid = [&](const L::StoryFlow& f) {   // every line's text is there
                const auto* st = d.as<core::Story>("story.1");
                for (const auto& ft : f.frames)
                    for (const auto& l : ft.lines)
                        if (l.para < 0 || l.para >= static_cast<int>(st->paras.size()) || l.start < 0 ||
                            l.end > static_cast<int>(st->paras[static_cast<std::size_t>(l.para)].text.size()) || l.start > l.end)
                            return false;
                return true;
            };
            const auto head_same = [&](const L::StoryFlow& f, const L::StoryFlow& whole) {
                if (f.tail_from < 0 || f.tail_from > static_cast<int>(f.frames.size())) return false;
                std::string w;
                L::StoryFlow a = f, b = whole;
                a.frames.resize(static_cast<std::size_t>(f.tail_from));
                b.frames.resize(static_cast<std::size_t>(f.tail_from));
                a.overset = b.overset;
                return same_flow(a, b, w);
            };
            // Pages of text pasted in: no 2-frame re-flow can resettle after
            // that, whatever the fonts (a chapter's last page has < 1 page free).
            std::string paste;
            for (int k = 0; k < 40; ++k)
                paste += "\nPasted paragraph, long enough to take several lines in a column of this book, so forty of them run on for pages and pages.";
            core::doc::edit_text(j, d, "story.1", {5, 3}, {5, 3}, paste, "Paste", "");
            const auto rev1 = d.revision();
            const auto p1 = cache.get(d, "story.1");
            const auto whole1 = L::flow_story(d, "story.1");
            check("budget: the re-flow stops, provisional", p1.provisional && p1.tail_from > 0 && p1.filled <= 2 &&
                                                          cache.any_unfinished(), std::to_string(p1.filled) + " set");
            check("budget: the frames it set are the whole flow's", p1.provisional && head_same(p1, whole1));
            check("budget: the stand-in's lines are all real text", valid(p1));
            std::string why;
            {   // the worker's part, then handed in
                const auto u = cache.unfinished();
                const bool one = u.size() == 1 && u[0].story == "story.1";
                const auto full = one ? L::reflow_story(d, "story.1", u[0].basis_text, u[0].basis) : L::flow_story(d, "story.1");
                cache.complete(d, "story.1", rev1, full, *d.as<core::Story>("story.1"));
                const auto& got = cache.get(d, "story.1");
                check("complete: the whole taken, no longer provisional", one && !got.provisional && !cache.any_unfinished() &&
                                                                          same_flow(got, whole1, why), why);
            }
            // Typed on while the worker works: its answer becomes the basis.
            core::doc::edit_text(j, d, "story.1", {7, 2}, {7, 2}, paste, "Paste", "");
            const auto rev2 = d.revision();
            (void)cache.get(d, "story.1");
            const auto u2 = cache.unfinished();
            const auto text2 = *d.as<core::Story>("story.1");
            const auto full2 = u2.empty() ? L::StoryFlow{} : L::reflow_story(d, "story.1", u2[0].basis_text, u2[0].basis);
            core::doc::edit_text(j, d, "story.1", {9, 2}, {9, 2}, "and more \n", "Typing", "");
            const auto& p3 = cache.get(d, "story.1");
            const bool p3ok = valid(p3) && (!p3.provisional || head_same(p3, L::flow_story(d, "story.1")));
            if (!u2.empty()) cache.complete(d, "story.1", rev2, full2, text2);   // late: the document moved on
            const auto& p4 = cache.get(d, "story.1");
            check("typed on meanwhile: re-flowed from the late answer", !u2.empty() && p3ok && valid(p4) &&
                                                                     cache.last_how() == L::FlowCache::How::Reflowed);
            cache.finish_now(d);
            check("finish_now: the whole, here and now", same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why) &&
                                                         !cache.any_unfinished(), why);
            // Undo through it all, unfinished or not, ends at the whole.
            j.undo(); j.undo(); j.undo(); j.undo();
            (void)cache.get(d, "story.1");
            cache.finish_now(d);
            check("undo: the whole again", same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why), why);
        }
        {   // The sample: keeps, drop caps, wraps, columns, balance, breaks.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            L::FlowCache cache;
            int bad = 0;
            std::string why, first;
            for (const auto& sid : d.ids_of<core::Story>()) {
                (void)cache.get(d, sid);
                const auto* st = d.as<core::Story>(sid);
                for (int p = 0; p < static_cast<int>(st->paras.size()); ++p) {
                    for (const char* ins : {"Then ", "\n", "some words to push the paragraph a line further on, it may be, here "}) {
                        core::doc::edit_text(j, d, sid, {p, 0}, {p, 0}, ins, "Typing", "");
                        if (!same_flow(cache.get(d, sid), L::flow_story(d, sid), why) && bad++ == 0) first = sid + " para " + std::to_string(p) + ": " + why;
                        j.undo();
                        if (!same_flow(cache.get(d, sid), L::flow_story(d, sid), why) && bad++ == 0) first = sid + " para " + std::to_string(p) + " (undone): " + why;
                    }
                }
            }
            check("the sample: every paragraph of every story edited -- re-flow == whole", bad == 0, first);
        }
    }

    {
        std::cout << "\n-- footnotes: the model (s109) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        const auto p1 = d.as<core::Story>("story.1")->paras[1].text;
        const auto after = core::doc::insert_note(j, d, "story.1", {1, 4}, "A first note.");
        const auto* st = d.as<core::Story>("story.1");
        const auto& para = st->paras[1];
        check("insert: the mark in the text, its note with it",
              core::note_at(para.text, 4) && para.notes.size() == 1 && para.notes[0].text == "A first note." &&
              after.para == 1 && after.byte == 7 && para.text.size() == p1.size() + 3);
        check("insert: one step", j.can_undo() && d.strays() == 0);
        j.undo();
        check("insert: undone exactly", d.records() == before);
        j.redo();
        core::doc::insert_note(j, d, "story.1", {3, 0}, "A later note.");
        core::doc::insert_note(j, d, "story.1", {1, 0}, "An earlier one.");
        st = d.as<core::Story>("story.1");
        check("numbered through the story, in order",
              core::note_number(*st, 1, 0) == 1 && core::note_number(*st, 1, 1) == 2 && core::note_number(*st, 3, 0) == 3 &&
              st->paras[1].notes[0].text == "An earlier one." && st->paras[1].notes[1].text == "A first note.");
        check("set_note_text: changes the words",
              core::doc::set_note_text(j, d, "story.1", 1, 1, "Changed.") && d.as<core::Story>("story.1")->paras[1].notes[1].text == "Changed.");
        // Enter before the second mark: that note goes with its mark to the new paragraph.
        core::doc::edit_text(j, d, "story.1", {1, 5}, {1, 5}, "\n", "Typing", "");
        st = d.as<core::Story>("story.1");
        check("a paragraph split: each note goes with its mark",
              st->paras[1].notes.size() == 1 && st->paras[1].notes[0].text == "An earlier one." &&
              st->paras[2].notes.size() == 1 && st->paras[2].notes[0].text == "Changed.");
        // Deleting the mark deletes its note; the others keep theirs.
        const int at = static_cast<int>(st->paras[2].text.find(core::kMarkNote));
        core::doc::edit_text(j, d, "story.1", {2, at}, {2, at + 3}, "", "Delete", "");
        st = d.as<core::Story>("story.1");
        check("a mark deleted: its note goes", st->paras[2].notes.empty() && st->paras[1].notes.size() == 1 &&
                                                core::note_number(*st, 4, 0) == 2);
        // A mark pasted in: an empty note.
        core::doc::edit_text(j, d, "story.1", {0, 0}, {0, 0}, std::string("x") + core::kMarkNote, "Paste", "");
        st = d.as<core::Story>("story.1");
        check("a pasted mark: an empty note, numbered first", st->paras[0].notes.size() == 1 && st->paras[0].notes[0].text.empty() &&
                                                              core::note_number(*st, 1, 0) == 2);
        // Saved and read back.
        core::Doc e;
        std::string err;
        check("json: notes round-trip", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        // Every step undone: the document as it was.
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
        core::Para lone{"", std::string("a") + core::kMarkNote + "b" + core::kMarkNote, {}};
        core::fit_notes(lone);
        check("fit_notes: a note for each mark", lone.notes.size() == 2);
    }

    {
        std::cout << "\n-- footnotes: set at the column's foot (s109) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::insert_note(j, d, "story.1", {1, 4}, "The harbour master's log for that year is lost.");
        core::doc::insert_note(j, d, "story.1", {2, 6}, "See the chart of 1871, in the second drawer.");
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        int placed = 0;
        bool clear = true, inside = true, numbered = true;
        std::string why;
        for (const auto& ft : f.frames) {
            const auto* fr = d.as<core::Frame>(ft.frame);
            for (const auto& np : ft.notes) {
                ++placed;
                numbered = numbered && np.number == placed;
                inside = inside && np.y >= 0 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01 && np.h > 0;
                // No line of the column runs into the notes (or their rule).
                for (const auto& l : ft.lines)
                    if (l.col == np.col && l.y + l.h > np.y + 0.01) { clear = false; why = ft.frame; }
            }
        }
        check("two notes placed (and the sample's), numbered 1, 2, 3", placed == 3 && numbered, std::to_string(placed));
        check("each inside its frame, at the foot", inside);
        check("no line runs into the notes", clear, why);
        check("a note's reference is in its column",
              [&] {
                  for (const auto& ft : f.frames)
                      for (const auto& np : ft.notes) {
                          bool found = false;
                          for (const auto& l : ft.lines) {
                              if (l.para != np.para || l.col != np.col) continue;
                              const auto& t = d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(l.para)].text;
                              for (std::size_t i = t.find(core::kMarkNote); i != std::string::npos; i = t.find(core::kMarkNote, i + 3))
                                  if (core::notes_in(t, 0, i) == np.k && static_cast<int>(i) >= l.start && static_cast<int>(i) < l.end) found = true;
                          }
                          if (!found) return false;
                      }
                  return true;
              }());
        // A note too long for the room left: its line goes on (to where it fits), never under the notes.
        std::string longer;
        for (int k = 0; k < 30; ++k) longer += "A long note, set in the smaller size, running on for many lines. ";
        core::doc::set_note_text(j, d, "story.1", 2, 0, longer);
        const auto& g = flows.get(d, "story.1");
        bool ok = true;
        for (const auto& ft : g.frames)
            for (const auto& np : ft.notes)
                for (const auto& l : ft.lines)
                    if (l.col == np.col && l.y + l.h > np.y + 0.01) {
                        // allowed only when the line is the column's first (it always goes)
                        bool first = true;
                        for (const auto& m : ft.lines) if (m.col == l.col && m.y < l.y) first = false;
                        ok = ok && first;
                    }
        check("a long note: the lines stay above it", ok);
        {   // The page's key moves with its note's words (its picture is drawn again).
            std::string page;
            for (const auto& ft : g.frames) if (!ft.notes.empty()) { page = d.as<core::Frame>(ft.frame)->owner; break; }
            const auto k0 = L::page_key(d, flows, page);
            core::doc::set_note_text(j, d, "story.1", 1, 0, "Reworded.");
            check("page key: a note's words change it", !page.empty() && !(L::page_key(d, flows, page) == k0));
        }
        // Re-flowed after an edit near a note: the whole flow, notes and all.
        L::FlowCache inc;
        (void)inc.get(d, "story.1");
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then, ", "Typing", "");
        const auto& r = inc.get(d, "story.1");
        const auto w = L::flow_story(d, "story.1");
        bool same = r.frames.size() == w.frames.size();
        for (std::size_t i = 0; same && i < r.frames.size(); ++i) {
            same = r.frames[i].notes.size() == w.frames[i].notes.size() && r.frames[i].lines.size() == w.frames[i].lines.size();
            for (std::size_t k = 0; same && k < r.frames[i].notes.size(); ++k)
                same = r.frames[i].notes[k].para == w.frames[i].notes[k].para && r.frames[i].notes[k].number == w.frames[i].notes[k].number &&
                       std::abs(r.frames[i].notes[k].y - w.frames[i].notes[k].y) < 1e-9;
        }
        check("re-flow with notes == whole flow", same);
        // Drawn and exported: the notes' lines are visited; both PDFs write.
        int note_lines = 0, refs = 0;
        for (const auto& ft : w.frames)
            L::visit_lines(d, "story.1", w, ft.frame, [&](const L::LineVisit& v) {
                note_lines += v.from == -4;
                refs += v.from == -3;
            });
        check("visited: the numbers at their marks, the notes' lines", refs == 3 && note_lines >= 4,
              std::to_string(refs) + " / " + std::to_string(note_lines));
        const auto dir = std::filesystem::temp_directory_path();
        for (const bool print : {false, true}) {
            std::string err;
            const auto path = (dir / (print ? "cx_notes_print.pdf" : "cx_notes_screen.pdf")).string();
            const bool done = print ? L::export_print_pdf(d, path, {}, {}, nullptr, &err) : L::export_pdf(d, path, {}, {}, nullptr, &err);
            check(std::string(print ? "print" : "screen") + " PDF with footnotes: written",
                  done && std::filesystem::file_size(path) > 1000, err);
            std::filesystem::remove(path);
        }
    }

    {
        std::cout << "\n-- footnote numbering (s111) --\n";
        namespace L = composix::layout;
        using NS = core::NoteStyle;
        check("labels: 1, iv, c, *, †, **", core::note_label(1, NS::Arabic) == "1" && core::note_label(4, NS::RomanLower) == "iv" &&
                                                 core::note_label(3, NS::AlphaLower) == "c" && core::note_label(1, NS::Symbols) == "*" &&
                                                 core::note_label(2, NS::Symbols) == "†" && core::note_label(7, NS::Symbols) == "**");
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        (void)L::load_big_book(d, {.pages = 24});
        // Notes in several paragraphs, two in one, far apart.
        for (const int p : {40, 12, 12, 5})
            core::doc::insert_note(j, d, "story.1", {p, 0}, "A note in paragraph " + std::to_string(p) + ".");
        const auto per_page = [&](const L::StoryFlow& f) {   // page -> its notes' numbers in order
            std::map<std::string, std::vector<int>> m;
            for (const auto& ft : f.frames)
                for (const auto& np : ft.notes) m[d.as<core::Frame>(ft.frame)->owner].push_back(np.number);
            return m;
        };
        L::FlowCache flows;
        auto whole = per_page(flows.get(d, "story.1"));
        std::vector<int> all;
        for (const auto& pg : d.pages()) for (int n : whole[pg]) all.push_back(n);
        std::size_t pages_with = 0;
        for (const auto& [pg, ns] : whole) pages_with += !ns.empty();
        check("through the story: 1, 2, 3, 4 in page order, on several pages", all == std::vector<int>{1, 2, 3, 4} && pages_with >= 2,
              std::to_string(pages_with) + " pages");
        check("numbering: one step", core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Arabic) &&
                                     !core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Arabic));
        const auto& fp = flows.get(d, "story.1");
        bool restarts = true;
        int ones = 0;
        for (const auto& [pg, ns] : per_page(fp))
            for (std::size_t i = 0; i < ns.size(); ++i) {
                restarts = restarts && ns[i] == static_cast<int>(i) + 1;
                ones += ns[i] == 1;
            }
        check("each page: its notes 1, 2, ...", restarts && ones >= 2);
        // Re-flowed under page numbering: still the whole flow.
        L::FlowCache inc;
        (void)inc.get(d, "story.1");
        bool same = true;
        for (const auto& [p, b] : std::vector<std::pair<int, int>>{{3, 0}, {12, 5}, {20, 0}}) {
            core::doc::edit_text(j, d, "story.1", {p, b}, {p, b}, "some words here to move a line on ", "Typing", "");
            const auto a = per_page(inc.get(d, "story.1"));
            same = same && a == per_page(L::flow_story(d, "story.1"));
        }
        check("re-flowed by page: the same numbers as a whole flow", same);
        {   // By section: numbers count on across pages, starting again in each section.
            core::doc::set_note_numbering(j, d, core::NoteRestart::Section, NS::Arabic);
            const auto& fsec = flows.get(d, "story.1");
            std::map<std::string, std::vector<int>> by;
            for (const auto& pg : d.pages())
                for (const auto& ft : fsec.frames)
                    if (d.as<core::Frame>(ft.frame)->owner == pg)
                        for (const auto& np : ft.notes) by[d.section_of(pg)].push_back(np.number);
            bool ok = !by.empty();
            for (const auto& [sec, ns] : by)
                for (std::size_t i = 0; i < ns.size(); ++i) ok = ok && ns[i] == static_cast<int>(i) + 1;
            check("each section: its notes 1, 2, ... across its pages", ok);
        }
        // The look: symbols in the notes and at the marks.
        core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Symbols);
        const auto& fs = flows.get(d, "story.1");
        int stars = 0;
        for (const auto& ft : fs.frames)
            if (!ft.notes.empty())
                L::visit_lines(d, "story.1", fs, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -3 && v.from != -4) return;
                    const int n = pango_layout_line_get_length(v.line) > 0 ? 1 : 0;
                    stars += n;
                });
        check("symbols: the numbers are drawn", stars >= 8);
        core::Doc e;
        std::string err;
        check("json: numbering kept", core::from_json(e, core::to_json(d), &err) &&
                                      e.settings().note_restart == core::NoteRestart::Page && e.settings().note_style == NS::Symbols, err);
    }

    {
        std::cout << "\n-- find / change in footnotes (s113) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        // "qqx" at the paragraph's start, in a note at byte 4+, and at its end.
        D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "qqx ", "Typing", "");
        D::insert_note(j, d, "story.1", {1, 8}, "Logged by the zebrafinch qqx master.");
        const int end1 = static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size());
        D::edit_text(j, d, "story.1", {1, end1}, {1, end1}, " qqx", "Typing", "");
        const auto* st = d.as<core::Story>("story.1");
        check("note_mark_byte: where the mark is", core::doc::note_mark_byte(*st, 1, 0) == 8 && core::doc::note_mark_byte(*st, 1, 1) < 0);
        const auto zh = D::find_all(d, {"zebrafinch"});
        const auto& nt = st->paras[1].notes[0].text;
        check("find: a word only in a note -- one hit, in it",
              zh.size() == 1 && zh[0].note == 0 && zh[0].a.para == 1 &&
                  nt.substr(static_cast<std::size_t>(zh[0].a.byte), static_cast<std::size_t>(zh[0].b.byte - zh[0].a.byte)) == "zebrafinch");
        const auto qh = D::find_all(d, {"qqx"});
        check("find: text, note, text -- in reading order at the mark",
              qh.size() == 3 && qh[0].note < 0 && qh[0].a.byte == 0 && qh[1].note == 0 && qh[2].note < 0 && qh[2].a.byte > 8);
        check("find: case and whole word reach notes", D::find_all(d, {"ZEBRAFINCH", true}).empty() &&
                                                       D::find_all(d, {"zebra", false, true}).empty() && D::find_all(d, {"ZEBRA"}).size() == 1);
        D::FindOpts rx{"zebra(\\w+)"};
        rx.mode = D::FindOpts::Mode::Regex;
        const auto rh = D::find_all(d, rx);
        check("find: a pattern in a note, its groups", rh.size() == 1 && rh[0].note == 0 && rh[0].groups.size() == 2 && rh[0].groups[1] == "finch");
        check("find: a paragraph style asked (not Footnote) -- notes not looked in",
              D::find_all(d, {"zebrafinch", false, false, "style.2"}).empty());
        D::FindOpts in{"zebrafinch"};
        in.story = "story.1";
        in.in_range = true;
        in.from = {1, 0};
        in.to = {1, 8};
        check("find: a range short of the mark -- not its note", D::find_all(d, in).empty());
        in.to = {1, 11};
        check("find: a range holding the mark -- its note", D::find_all(d, in).size() == 1);
        // Change one, in the note.
        const auto pre = d.records();
        check("change: in a note, one hit", D::change_hits(j, d, zh, {true, "heron"}, "Change") == 1 &&
                                            d.as<core::Story>("story.1")->paras[1].notes[0].text == "Logged by the heron qqx master.");
        j.undo();
        check("change: one step, undone exactly", d.records() == pre);
        // Change All across text and notes: one step; the note kept with its mark.
        check("change all: text and note, one step",
              D::change_hits(j, d, qh, {true, "yy"}, "Change All (3)") == 3 && D::find_all(d, {"qqx"}).empty() &&
                  D::find_all(d, {"yy", false, true}).size() == 3 && d.as<core::Story>("story.1")->paras[1].notes.size() == 1);
        j.undo();
        check("change all: undone in one", d.records() == pre);
        check("change: a line break in a note is a new paragraph (s116)",
              D::change_hits(j, d, zh, {true, "big\r\nheron"}, "Change") == 1 &&
                  d.as<core::Story>("story.1")->paras[1].notes[0].text == "Logged by the big\nheron qqx master.");
        j.undo();
        // A character style onto a note's words.
        D::ChangeOpts cs{false, "", "", "style.5"};
        st = d.as<core::Story>("story.1");
        const auto& note = st->paras[1].notes[0];
        check("change: a character style onto words in a note",
              D::change_hits(j, d, zh, cs, "Change") == 1 && d.as<core::Story>("story.1")->paras[1].notes[0].text == note.text &&
                  [&] {
                      const auto& r = d.as<core::Story>("story.1")->paras[1].notes[0].runs;
                      return r.size() == 1 && r[0].style == "style.5" && r[0].start == zh[0].a.byte && r[0].end == zh[0].b.byte;
                  }());
        check("find: in a character style -- in the note", D::find_all(d, {"zebrafinch", false, false, "", "style.5"}).size() == 1);
        j.undo();
        // s113 fix: a character style (or bold) over text with a note kept the note's words.
        const auto words = d.as<core::Story>("story.1")->paras[1].notes[0].text;
        D::set_char_style(j, d, "story.1", {1, 0}, {1, 12}, "style.5");
        check("fix: a character style over a footnote's mark keeps its words",
              d.as<core::Story>("story.1")->paras[1].notes.size() == 1 && d.as<core::Story>("story.1")->paras[1].notes[0].text == words);
        D::set_local(j, d, "story.1", {1, 0}, {1, 12}, "Bold", [](core::Local& l) { l.bold = 1; });
        check("fix: bold over a footnote's mark keeps its words", d.as<core::Story>("story.1")->paras[1].notes[0].text == words);
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
    }

    {
        std::cout << "\n-- footnote defaults (s114) --\n";
        namespace L = composix::layout;
        core::Doc n;
        core::new_document(n, core::NewDoc{});
        bool has = false;
        for (const auto& id : n.ids_of<core::Style>()) {
            const auto* s = n.as<core::Style>(id);
            has = has || (s->name == "Footnote" && s->kind == core::StyleKind::Paragraph && s->based_on == "style.1");
        }
        check("a new document has a Footnote style (on Body)", has);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* st = d.as<core::Story>("story.1");
        check("the sample has a note (Chapter Two's map)", st->paras[5].notes.size() == 1 && !st->paras[5].notes[0].text.empty() &&
                                                         core::doc::note_mark_byte(*st, 5, 0) >= 0);
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        const L::NotePlace* np = nullptr;
        std::string fr;
        for (const auto& ft : f.frames)
            for (const auto& x : ft.notes) { np = &x; fr = ft.frame; }
        check("the sample's note is set at a foot", np != nullptr);
        if (np) {
            const auto* hit = L::note_hit(f, fr, np->x + np->w / 2, np->y + np->h / 2);
            check("note_hit: on the note -- it", hit && hit->para == 5 && hit->k == 0);
            check("note_hit: above it -- none", L::note_hit(f, fr, np->x + np->w / 2, np->y - 2) == nullptr);
            // Set in the Footnote style: its size, not the paragraph's.
            int lines = 0;
            double h1 = 0, body = 0;
            for (const auto& ft : f.frames)
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    PangoRectangle r;
                    pango_layout_line_get_extents(v.line, nullptr, &r);
                    const double h = r.height / static_cast<double>(PANGO_SCALE);
                    if (v.from == -4) { ++lines; h1 = std::max(h1, h); }
                    else if (v.from >= 0 && v.para == 2) body = std::max(body, h);   // Body, 11 pt
                });
            check("the note's lines drawn, smaller than Body's", lines >= 1 && h1 > 0 && body > 0 && h1 < body * 0.9,
                  std::to_string(h1) + " / " + std::to_string(body));
        }
    }

    {
        std::cout << "\n-- the Footnotes section (s115) --\n";
        std::string st;
        int p = -1, k = -1;
        const auto id = core::note_item_id("story.1", 5, 0);
        check("note item id: made and read back", core::parse_note_item_id(id, st, p, k) && st == "story.1" && p == 5 && k == 0);
        check("note item id: not one -- refused", !core::parse_note_item_id("story.1", st, p, k) &&
                                                  !core::parse_note_item_id("note:story.1:x:0", st, p, k) &&
                                                  !core::parse_note_item_id("note::1:0", st, p, k) &&
                                                  !core::parse_note_item_id("note:story.1:1:2z", st, p, k));
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto o = core::project(d, "");
        auto notes = o.in(core::Segment::Notes);
        const auto words = [](const core::Item* it) {
            for (const auto& f : it->fields) if (f.edit == "note_words") return f.value;
            return std::string("?");
        };
        check("the sample's note listed, its words editable",
              notes.size() == 1 && notes[0]->id == id && words(notes[0]) == d.as<core::Story>("story.1")->paras[5].notes[0].text &&
                  notes[0]->thumb.sample == words(notes[0]));
        core::doc::insert_note(j, d, "story.1", {1, 4}, "");
        core::doc::insert_note(j, d, "story.2", {0, 0}, "Surveyed again in 1872.");
        o = core::project(d, "");
        notes = o.in(core::Segment::Notes);
        check("each note, in story order, numbered in its story",
              notes.size() == 3 && notes[0]->id == core::note_item_id("story.1", 1, 0) && notes[0]->title.rfind("Note 1", 0) == 0 &&
                  notes[1]->id == id && notes[1]->title.rfind("Note 2", 0) == 0 && notes[2]->title.rfind("Note 1", 0) == 0);
        check("an empty note is badged", notes[0]->badge == "Empty" && notes[1]->badge.empty());
        bool show = false;
        for (const auto& f : notes[1]->fields) show = show || (f.buttons && f.edit == "note" && !f.choices.empty());
        check("Show in the Text offered", show);
        check("ids unique across the outline", [&] {
            std::set<std::string> ids;
            for (const auto& it : o.items()) if (!ids.insert(it.id).second) return false;
            return true;
        }());
    }

    {
        std::cout << "\n-- footnotes in paragraphs (s116) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto note = [&] { return d.as<core::Story>("story.1")->paras[5].notes[0].text; };
        const auto foot = [&] {   // the sample note's place: its height, its lines drawn
            L::FlowCache flows;
            const auto& f = flows.get(d, "story.1");
            double h = 0;
            int lines = 0;
            for (const auto& ft : f.frames) {
                for (const auto& np : ft.notes) h += np.h;
                if (!ft.notes.empty())
                    L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) { lines += v.from == -4; });
            }
            return std::pair<double, int>{h, lines};
        };
        const auto one = foot();
        check("set_note_text: paragraphs kept, \\r\\n read as one break",
              D::set_note_text(j, d, "story.1", 5, 0, "First, the foldout.\r\nSecond, the key.\rThird, the scale.") &&
                  note() == "First, the foldout.\nSecond, the key.\nThird, the scale.");
        const auto three = foot();
        check("three paragraphs: taller at the foot, each its own line(s)", three.first > one.first + 1 && three.second >= 3,
              std::to_string(one.first) + " -> " + std::to_string(three.first) + ", " + std::to_string(three.second) + " lines");
        core::Doc e;
        std::string err;
        check("json: a note's paragraphs kept", core::from_json(e, core::to_json(d), &err) &&
                                                e.as<core::Story>("story.1")->paras[5].notes[0].text == note(), err);
        const auto hits = D::find_all(d, {"key.^pThird"});
        check("find ^p: across a note's paragraphs", hits.size() == 1 && hits[0].note == 0 && hits[0].a.para == 5);
        D::FindOpts rx{"\\n"};
        rx.mode = D::FindOpts::Mode::Regex;
        const auto in_note = [](std::vector<D::Hit> h) { std::erase_if(h, [](const D::Hit& x) { return x.note < 0; }); return h; };
        check("find \\n (regex): each break in the note", in_note(D::find_all(d, rx)).size() == 2);
        check("change ^p in a note: joined, one step",
              D::change_hits(j, d, in_note(D::find_all(d, {"^p"})), {true, " "}, "Change All") == 2 &&
                  note() == "First, the foldout. Second, the key. Third, the scale.");
        j.undo();
        check("undo: the paragraphs back", note() == "First, the foldout.\nSecond, the key.\nThird, the scale.");
        const auto o = core::project(d, "");
        const auto ns = o.in(core::Segment::Notes);
        bool ro = false;
        for (const auto& f : ns.front()->fields) ro = ro || (f.label.rfind("Words (", 0) == 0 && f.edit.empty());
        check("the Footnotes section: paragraphs shown, not typed in one line", ns.size() == 1 && ro);
    }

    {
        std::cout << "\n-- footnotes run on; the footnote area (s117) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("footnote area: one step, kept", D::set_note_area(j, d, 50) && !D::set_note_area(j, d, 50) &&
                                               d.settings().note_area == 50 && D::set_note_area(j, d, 100));
        {
            core::Doc e;
            std::string err;
            D::set_note_area(j, d, 33);
            check("json: the area kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_area == 33, err);
            D::set_note_area(j, d, 100);
        }
        // A note far longer than a column, on Chapter One's first paragraph.
        std::string longw;
        for (int i = 1; i <= 60; ++i)
            longw += "Clause " + std::to_string(i) + " of the harbour ordinance, as amended, applies to every vessel entering the port.";
        longw += "\nA second paragraph closes the note.";
        D::insert_note(j, d, "story.1", {1, 4}, longw);
        const auto* st = d.as<core::Story>("story.1");
        // Every piece of the note, in thread order, and the checks on each frame.
        struct Piece { std::size_t frame; int from, to; bool cont; double y, h; };
        const auto pieces = [&](const L::StoryFlow& f, bool* inside, bool* clear, double* most) {
            std::vector<Piece> out;
            *inside = *clear = true;
            *most = 0;
            for (std::size_t i = 0; i < f.frames.size(); ++i) {
                const auto& ft = f.frames[i];
                const auto* fr = d.as<core::Frame>(ft.frame);
                std::map<int, double> per_col;
                for (const auto& np : ft.notes) {
                    if (np.para == 1 && np.k == 0) out.push_back({i, np.from, np.to, np.cont, np.y, np.h});
                    *inside = *inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                    per_col[np.col] += np.h;
                    for (const auto& l : ft.lines)
                        if (l.col == np.col && l.y + l.h > np.y + 0.01) *clear = false;
                }
                const double colh = fr->h - std::max(0.0, fr->inset.bottom) - std::max(0.0, fr->inset.top);
                for (const auto& [c, h] : per_col) *most = std::max(*most, h / colh);
            }
            return out;
        };
        const auto joined = [&](const std::vector<Piece>& ps, bool overset = false) {   // the pieces cover the words, in order, once
            if (ps.empty() || ps.front().from != 0 || ps.front().cont || (ps.back().to != -1 && !overset)) return false;
            for (std::size_t i = 1; i < ps.size(); ++i)
                if (!ps[i].cont || ps[i].from != ps[i - 1].to || ps[i].frame <= ps[i - 1].frame) return false;
            return true;
        };
        bool inside = false, clear = false;
        double most = 0;
        L::FlowCache flows;
        const auto& whole = flows.get(d, "story.1");
        auto ps = pieces(whole, &inside, &clear, &most);
        check("a note longer than a column: cut, run on", ps.size() >= 2 && joined(ps), std::to_string(ps.size()) + " pieces");
        check("each piece inside its frame; no line runs into the notes", inside && clear);
        const auto n100 = ps.size();
        // Its mark's line stays where it was: the note is cut instead (not the line sent on).
        bool mark_on_first = false;
        for (const auto& l : whole.frames[ps.front().frame].lines)
            mark_on_first = mark_on_first || (l.para == 1 && l.start <= 4 && l.end > 4);
        check("the mark's line keeps its frame, with the note's first piece", mark_on_first);
        // The rule over a run-on piece spans the column.
        bool wide = false;
        for (const auto& r : L::paragraph_rules(d, "story.1", whole, whole.frames[ps[1].frame].frame))
            for (const auto& np : whole.frames[ps[1].frame].notes)
                if (np.cont && std::abs(r.r.w - np.w) < 0.01) wide = true;
        check("a run-on piece: the rule across the column", wide);
        int cont_lines = 0;
        L::visit_lines(d, "story.1", whole, whole.frames[ps[1].frame].frame, [&](const L::LineVisit& v) { cont_lines += v.from == -4; });
        check("a run-on piece is drawn", cont_lines >= 1);
        {   // Cut with two lines of the note or more on each side.
            bool two = true;
            for (const auto& ft : whole.frames)
                for (const auto& np : ft.notes)
                    if (np.para == 1 && np.k == 0) {
                        int lines = 0;
                        L::visit_lines(d, "story.1", whole, ft.frame, [&](const L::LineVisit& v) { lines += v.from == -4; });
                        two = two && lines >= 2;
                    }
            check("each piece two lines or more", two);
        }
        // The area at 50%: no column's notes past half of it; more pieces.
        D::set_note_area(j, d, 50);
        const auto& half = flows.get(d, "story.1");
        ps = pieces(half, &inside, &clear, &most);
        check("area 50%: the notes keep to half the column", most <= 0.5 + 0.01, std::to_string(most));
        check("area 50%: the note runs on further (past the last frame: overset)", joined(ps, half.overset) && ps.size() > n100, std::to_string(n100) + " -> " + std::to_string(ps.size()));
        check("area 50%: inside, clear", inside && clear);
        check("a frame the text passes (a chapter starts right) still takes the note running on", [&] {
            for (const auto& ft : half.frames)
                if (ft.lines.empty() && !ft.notes.empty() && ft.notes.front().cont) return true;
            return false;
        }());
        // Numbered by page: a run-on piece takes no number -- the next page's own note is 1.
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);
        {
            const auto& fp = flows.get(d, "story.1");
            const auto pp = pieces(fp, &inside, &clear, &most);
            bool ok = !pp.empty();
            for (const auto& ft : fp.frames) {
                int own = 0;
                for (const auto& np : ft.notes)
                    if (!np.cont) ok = ok && np.number == ++own;
            }
            check("by page: run-on pieces take no number", ok);
        }
        D::set_note_numbering(j, d, core::NoteRestart::Story, core::NoteStyle::Arabic);
        // A re-flow after an edit past the note: the same as a whole flow.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                const auto& a = re.frames[i].notes;
                const auto& b = wf.frames[i].notes;
                same = a.size() == b.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() && re.frames[i].carry_in == wf.frames[i].carry_in;
                for (std::size_t k = 0; same && k < a.size(); ++k)
                    same = a[k].para == b[k].para && a[k].k == b[k].k && a[k].from == b[k].from && a[k].to == b[k].to && a[k].cont == b[k].cont &&
                           std::abs(a[k].y - b[k].y) < 1e-9;
            }
            check("re-flow with a note running on == whole flow", same);
            // And an edit before the note (its line moves): still the same.
            const core::Story before2 = *d.as<core::Story>("story.1");
            const auto old2 = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "At first light ", "Typing", "");
            const auto re2 = L::reflow_story(d, "story.1", before2, old2);
            const auto wf2 = L::flow_story(d, "story.1");
            bool same2 = re2.frames.size() == wf2.frames.size();
            for (std::size_t i = 0; same2 && i < re2.frames.size(); ++i)
                same2 = re2.frames[i].notes.size() == wf2.frames[i].notes.size() && re2.frames[i].carry_in == wf2.frames[i].carry_in &&
                        re2.frames[i].lines.size() == wf2.frames[i].lines.size();
            check("re-flow, an edit before the note == whole flow", same2);
        }
        while (j.can_undo()) j.undo();
        // The text ends, a note still runs on: into the next frame.
        {
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            // The Sources story is short: a long note on its last paragraph.
            const auto& src = *e.as<core::Story>("story.4");
            const int last = static_cast<int>(src.paras.size()) - 1;
            D::insert_note(je, e, "story.4", {last, static_cast<int>(src.paras[static_cast<std::size_t>(last)].text.size())}, longw);
            const auto f = L::flow_story(e, "story.4");
            int with_notes = 0;
            for (const auto& ft : f.frames) with_notes += ft.notes.empty() ? 0 : 1;
            check("the text ended, the note ran on (or is overset): never lost",
                  f.overset || with_notes >= 2 || f.frames.size() == 1, std::to_string(with_notes) + " frames with notes, overset " + std::to_string(f.overset));
        }
    }

    {
        std::cout << "\n-- the foot's look (s118) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("footnote look: one step, the same again none",
              D::set_note_look(j, d, 50, 1, 3, "(continued)") && !D::set_note_look(j, d, 50, 1, 3, "(continued)") && j.size() >= 1);
        {
            core::Doc e;
            std::string err;
            check("json: the look kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_rule == 50 &&
                                         e.settings().note_rule_w == 1 && e.settings().note_space == 3 && e.settings().note_cont == "(continued)", err);
        }
        // Two short notes in Chapter One's first paragraph, a long one in its second.
        D::insert_note(j, d, "story.1", {1, 4}, "The first note.");
        D::insert_note(j, d, "story.1", {1, 20}, "The second note.");
        std::string longw;
        for (int i = 1; i <= 40; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance, as amended, applies to every vessel. ";
        D::insert_note(j, d, "story.1", {2, 6}, longw);
        D::set_note_area(j, d, 50);
        const auto f = L::flow_story(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& x : f.frames) if (x.notes.size() >= 2 && !x.notes.front().cont) { ft = &x; break; }
        check("two notes at a foot", ft != nullptr);
        if (ft) {
            const auto& a = ft->notes[0];
            const auto& b = ft->notes[1];
            check("3 pt between notes", std::abs(b.y - (a.y + a.h) - 3) < 0.01, std::to_string(b.y - (a.y + a.h)));
            bool rule = false;
            for (const auto& r : L::paragraph_rules(d, "story.1", f, ft->frame))
                if (r.r.y < a.y && r.r.y > a.y - 20 && std::abs(r.r.w - a.w * 0.5) < 0.01 && std::abs(r.r.h - 1) < 0.01) rule = true;
            check("the rule: half the column, 1 pt", rule);
            D::set_note_look(j, d, 0, 1, 3, "(continued)");
            const auto f0 = L::flow_story(d, "story.1");
            bool none = true;
            for (const auto& x : f0.frames)
                if (x.frame == ft->frame)
                    for (const auto& r : L::paragraph_rules(d, "story.1", f0, x.frame))
                        if (r.r.y < x.notes.front().y && r.r.y > x.notes.front().y - 20) none = false;
            check("rule None: no rule over the notes", none);
            D::set_note_look(j, d, 50, 1, 3, "(continued)");
        }
        // The continued line: the last line of a cut piece, and nothing overlaps.
        bool cut_seen = false, cont_line = false, inside = true, clear = true;
        for (const auto& x : f.frames) {
            const auto* fr = d.as<core::Frame>(x.frame);
            for (const auto& np : x.notes) {
                inside = inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                for (const auto& l : x.lines) if (l.col == np.col && l.y + l.h > np.y + 0.01) clear = false;
                if (np.to >= 0) cut_seen = true;
            }
            std::string last;
            L::visit_lines(d, "story.1", f, x.frame, [&](const L::LineVisit& v) {
                if (v.from != -4) return;
                const char* t = pango_layout_get_text(v.line->layout);
                last = std::string(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                if (last == "(continued)") cont_line = true;
            });
        }
        check("a cut note ends in the continued line", cut_seen && cont_line);
        check("with the look: inside the frames, no line into the notes", inside && clear);
        D::set_note_look(j, d, 33, 0.5, 0, "");
        const auto g = L::flow_story(d, "story.1");
        bool any = false;
        for (const auto& x : g.frames)
            L::visit_lines(d, "story.1", g, x.frame, [&](const L::LineVisit& v) {
                if (v.from != -4) return;
                const char* t = pango_layout_get_text(v.line->layout);
                if (std::string(t + v.line->start_index, static_cast<std::size_t>(v.line->length)) == "(continued)") any = true;
            });
        check("no continued line set: none drawn", !any);
    }

    {
        std::cout << "\n-- the Story Editor's numbers (s119) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "On page 1.");   // the sample's own is on Chapter Two's page
        L::FlowCache flows;
        check("through the story: 1 and 2", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                            L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 2);
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);
        check("by page: each the first on its page -- 1 and 1", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                                               L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 1);
        check("a note not there: 0", L::laid_note_number(flows.get(d, "story.1"), 1, 3) == 0);
        {
            const auto o = core::project(d, "");
            const auto* it = o.find("story.1");
            check("the story's tile: no footnote mark in its words", it && it->thumb.sample.find(core::kMarkNote) == std::string::npos &&
                                                                     it->thumb.sample.find("The harbor") != std::string::npos);
        }
        // A note running on: its run-on piece isn't another number.
        std::string longw;
        for (int i = 1; i <= 40; ++i) longw += "Clause " + std::to_string(i) + " applies to every vessel entering the port. ";
        D::set_note_text(j, d, "story.1", 1, 0, longw);
        D::set_note_area(j, d, 25);
        int pieces = 0;
        for (const auto& ft : flows.get(d, "story.1").frames)
            for (const auto& n : ft.notes) pieces += n.para == 1 && n.k == 0;
        check("running on: the number of its first piece", pieces >= 2 && L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1);
    }

    {
        std::cout << "\n-- endnotes (s120) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "The first, on page 1.");   // the sample's own (Chapter Two) is second
        check("notes at: one step, kept", D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes") &&
                                          !D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes"));
        {
            core::Doc e;
            std::string err;
            D::set_note_place(j, d, core::NoteAt::StoryEnd, "Endnotes");
            check("json: endnotes and their title kept", core::from_json(e, core::to_json(d), &err) &&
                                                         e.settings().note_at == core::NoteAt::StoryEnd && e.settings().note_title == "Endnotes", err);
            D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        }
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);   // endnotes count through the story anyway
        const auto check_flow = [&](const L::StoryFlow& f, std::vector<const L::NotePlace*>* all, bool* inside, bool* after, bool* clear) {
            *inside = *after = *clear = true;
            for (const auto& ft : f.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                double last = 0;
                for (const auto& l : ft.lines) last = std::max(last, l.y + l.h);
                for (const auto& np : ft.notes) {
                    all->push_back(&np);
                    *inside = *inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                    *after = *after && np.y >= last - 0.01;
                    for (const auto& l : ft.lines) if (l.col == np.col && l.y + l.h > np.y + 0.01 && l.y < np.y + np.h - 0.01) *clear = false;
                }
            }
        };
        L::FlowCache flows;
        std::vector<const L::NotePlace*> all;
        bool inside = false, after = false, clear = false;
        check_flow(flows.get(d, "story.1"), &all, &inside, &after, &clear);
        check("endnotes: the title, then each note in story order, numbered 1, 2",
              all.size() == 3 && all[0]->k == -1 && all[1]->para == 1 && all[1]->number == 1 && all[2]->para == 5 && all[2]->number == 2 &&
                  std::all_of(all.begin(), all.end(), [](const L::NotePlace* n) { return n->end; }));
        check("endnotes: after the text, inside the frame, clear of it", inside && after && clear);
        check("the marks: numbered through the story", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                                       L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 2);
        {
            int drawn = 0;
            const auto& f = flows.get(d, "story.1");
            for (const auto& ft : f.frames) L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) { drawn += v.from == -4; });
            check("endnotes drawn (title and notes)", drawn >= 3);
            bool foot_rule = false;
            for (const auto& ft : f.frames)
                for (const auto& r : L::paragraph_rules(d, "story.1", f, ft.frame))
                    for (const auto& np : ft.notes) if (r.r.y < np.y && r.r.y > np.y - 12 && r.r.h <= 0.6) foot_rule = true;
            check("no footnote rule over endnotes", !foot_rule);
        }
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "");
        all.clear();
        check_flow(flows.get(d, "story.1"), &all, &inside, &after, &clear);
        check("no title set: none", all.size() == 2 && all[0]->k == 0);
        // A long endnote runs on: its pieces chained, in order (overset past the last frame).
        std::string longw;
        for (int i = 1; i <= 60; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance applies to every vessel. ";
        D::set_note_text(j, d, "story.1", 5, 0, longw);
        all.clear();
        const auto& lf = flows.get(d, "story.1");
        check_flow(lf, &all, &inside, &after, &clear);
        std::vector<const L::NotePlace*> pcs;
        for (auto* n : all) if (n->para == 5 && n->k == 0) pcs.push_back(n);
        bool chain = !pcs.empty() && pcs.front()->from == 0;
        for (std::size_t i = 1; i < pcs.size(); ++i) chain = chain && pcs[i]->from == pcs[i - 1]->to && pcs[i]->cont;
        check("a long endnote: cut where the last frame ends, the rest overset (chained)",
              !pcs.empty() && chain && pcs.back()->to > 0 && lf.overset, std::to_string(pcs.size()) + " pieces");
        check("long: inside, after the text, clear", inside && after && clear);
        {   // The last frame in two columns: the endnote runs on into the second.
            const auto last = L::thread_frames(d, "story.1").back();
            core::doc::edit_frames(j, d, {last}, "Columns", [](core::Frame& f) { f.columns = 2; });
            std::vector<const L::NotePlace*> two;
            bool in2 = false, af2 = false, cl2 = false;
            const auto& f2 = flows.get(d, "story.1");
            check_flow(f2, &two, &in2, &af2, &cl2);
            std::vector<const L::NotePlace*> p2;
            for (auto* n : two) if (n->para == 5 && n->k == 0) p2.push_back(n);
            bool ch = !p2.empty() && p2.front()->from == 0;
            for (std::size_t i = 1; i < p2.size(); ++i) ch = ch && p2[i]->from == p2[i - 1]->to && p2[i]->cont && p2[i]->col > p2[i - 1]->col;
            check("two columns: the endnote runs on into the next column", p2.size() >= 2 && ch && in2 && cl2, std::to_string(p2.size()));
            j.undo();
        }
        // Re-flow after an edit == whole.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() &&
                       re.frames[i].ending_in == wf.frames[i].ending_in;
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k)
                    same = re.frames[i].notes[k].from == wf.frames[i].notes[k].from && std::abs(re.frames[i].notes[k].y - wf.frames[i].notes[k].y) < 1e-9;
            }
            check("re-flow with endnotes == whole flow", same);
        }
        // Back to the foot: no endnotes left behind.
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        bool any_end = false;
        for (const auto& ft : flows.get(d, "story.1").frames) for (const auto& np : ft.notes) any_end = any_end || np.end;
        check("back to the foot: none set as endnotes", !any_end);
        // A story with no notes: no title.
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        bool none = true;
        for (const auto& ft : flows.get(d, "story.2").frames) none = none && ft.notes.empty();
        check("no notes: no endnotes title", none);
    }

    {
        std::cout << "\n-- chapter endnotes (s122) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "Chapter One's note.");   // the sample's own is in Chapter Two
        check("notes at: End of each chapter, one step", D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes"));
        {
            core::Doc e;
            std::string err;
            check("json: chapters kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_at == core::NoteAt::ChapterEnd, err);
        }
        // Where each piece is: the frame index; and where each paragraph's lines are.
        struct At { std::size_t fi; const L::NotePlace* np; };
        const auto places = [&](const L::StoryFlow& f) {
            std::vector<At> out;
            for (std::size_t i = 0; i < f.frames.size(); ++i)
                for (const auto& np : f.frames[i].notes) out.push_back({i, &np});
            return out;
        };
        const auto frame_of_para = [&](const L::StoryFlow& f, int p) {
            for (std::size_t i = 0; i < f.frames.size(); ++i)
                for (const auto& l : f.frames[i].lines) if (l.para == p) return static_cast<int>(i);
            return -1;
        };
        L::FlowCache flows;
        {
            const auto& f = flows.get(d, "story.1");
            const auto ps = places(f);
            const int ch2 = frame_of_para(f, 4), ch1end = frame_of_para(f, 3);
            bool ok = ps.size() == 4 && ps[0].np->k == -1 && ps[1].np->para == 1 && ps[1].np->number == 1 && ps[2].np->k == -1 &&
                      ps[3].np->para == 5 && ps[3].np->number == 2;
            check("each chapter: its title and notes; numbered through the story", ok, std::to_string(ps.size()));
            if (ok) {
                check("Chapter One's notes before Chapter Two, after its text",
                      static_cast<int>(ps[1].fi) >= ch1end && static_cast<int>(ps[1].fi) < ch2);
                check("Chapter Two's at the story's end", static_cast<int>(ps[3].fi) >= frame_of_para(f, 5));
                bool clear = true;
                for (const auto& a : ps)
                    for (const auto& l : f.frames[a.fi].lines)
                        if (l.col == a.np->col && l.y + l.h > a.np->y + 0.01 && l.y < a.np->y + a.np->h - 0.01) clear = false;
                check("chapter endnotes clear of the text", clear);
            }
        }
        // A long note in Chapter One: runs on into the page the chapter break passes (page 2), Chapter Two still on its right page.
        std::string longw;
        for (int i = 1; i <= 45; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance applies to every vessel. ";
        D::set_note_text(j, d, "story.1", 1, 0, longw);
        {
            const auto& f = flows.get(d, "story.1");
            std::vector<std::size_t> fis;
            for (const auto& a : places(f)) if (a.np->para == 1 && a.np->k == 0) fis.push_back(a.fi);
            const int ch2 = frame_of_para(f, 4);
            bool before = !fis.empty();
            for (auto fi : fis) before = before && static_cast<int>(fi) < ch2;
            const auto* p4 = ch2 >= 0 ? d.as<core::Frame>(f.frames[static_cast<std::size_t>(ch2)].frame) : nullptr;
            check("a long chapter note runs on, all of it before Chapter Two", fis.size() >= 2 && before, std::to_string(fis.size()) + " pieces");
            check("Chapter Two still opens a right-hand page", p4 && d.side_of(p4->owner) == core::Side::Right);
        }
        // Re-flow after an edit in Chapter One == whole.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, "Even so, ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() &&
                       re.frames[i].end_from_in == wf.frames[i].end_from_in;
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k)
                    same = re.frames[i].notes[k].from == wf.frames[i].notes[k].from && std::abs(re.frames[i].notes[k].y - wf.frames[i].notes[k].y) < 1e-9;
            }
            check("re-flow with chapter endnotes == whole flow", same);
        }
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        bool any = false;
        for (const auto& ft : flows.get(d, "story.1").frames) for (const auto& np : ft.notes) any = any || np.end;
        check("back to the foot: no endnotes", !any);
    }
}
}  // namespace cxtest
