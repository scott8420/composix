// composix_selftest, part 5 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part5() {

    {
        std::cout << "\n-- indents + tabs --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        // Turn the wrapped picture off: plain lines to measure.
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.wrap = core::Wrap::None;
            d.put("frame.11", f);
        }
        const auto fl = L::flow_story(d, "story.1");
        const auto cx = [&](int p, int b) { return L::caret_at(d, "story.1", fl, {p, b}).x; };
        check("first indent: none after the heading", std::abs(cx(1, 0)) < 0.5);
        check("first indent: 18 pt after a body paragraph", std::abs(cx(2, 0) - 18) < 0.5);
        check("first indent: only the first line", [&] {
            for (const auto& ft : fl.frames)
                for (const auto& l : ft.lines)
                    if (l.para == 2 && l.start > 0) return std::abs(L::caret_at(d, "story.1", fl, {2, l.start}).x) < 0.5;
            return false;
        }());
        {   // s045: the sample's Sources are a numbered list now (their first line in line
            // with the rest, the number hung); without it, the negative first indent hangs.
            auto s4 = *d.as<core::Style>("style.4");
            s4.list = {};
            d.put("style.4", s4);
        }
        const auto src = L::flow_story(d, "story.4");
        bool hang = false;
        for (const auto& ft : src.frames)
            for (const auto& l : ft.lines)
                if (l.para == 0 && l.start > 0) hang = std::abs(L::caret_at(d, "story.4", src, {0, l.start}).x - l.x + l.x - 12) < 0.5 ||
                                                       std::abs(L::caret_at(d, "story.4", src, {0, l.start}).x - 12) < 0.5;
        check("hanging indent: the first line out, the rest in", std::abs(L::caret_at(d, "story.4", src, {0, 0}).x) < 0.5 && hang);
        // Contents: a right tab at the text's edge, with dots.
        const auto con = L::flow_story(d, "story.9");
        const double tw = core::text_width(d);
        const auto one = L::caret_at(d, "story.9", con, {1, 12});   // after "Chapter One\t"
        check("tab: right-aligned at the text's edge", one.ok && one.x > tw - 12 && one.x < tw);
        int leaders = 0;
        L::visit_lines(d, "story.9", con, "frame.16", [&](const L::LineVisit& v) { leaders += v.from < 0; });
        check("tab: a dot leader on each entry", leaders == 3);
        check("tabs: in words", core::tabs_text(d.as<core::Style>("style.8")->tabs) == "Right 4.5 in, dots" &&
                                core::tabs_text({}) == "None");
        check("tabs: presets measure the text width", core::tab_preset(d, "right.").front().pos == tw &&
                                                      core::tab_preset(d, "every").size() == 8);
        const auto o = core::project(d, "");
        bool fi = false, tb = false;
        for (const auto& f : o.find("style.1")->fields) {
            fi = fi || (f.edit == "findent" && f.value == "18 pt");
            tb = tb || (f.edit == "indent_after" && f.value == "Not after a heading");
        }
        check("inspector: First indent and when", fi && tb);
    }

    {
        std::cout << "\n-- spreads --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto sp = core::spreads(d);
        check("spreads: page i alone, then left + right pairs", sp.front() == std::vector<std::string>{"page.1"} &&
                                                                 sp[1] == (std::vector<std::string>{"page.2", "page.3"}));
        check("spreads: every page once", [&] {
            std::size_t n = 0;
            for (const auto& s : sp) n += s.size();
            return n == d.pages().size();
        }());
        check("spread_of: either page gives the pair", core::spread_of(d, "page.7") == (std::vector<std::string>{"page.6", "page.7"}) &&
                                                       core::spread_of(d, "page.6") == core::spread_of(d, "page.7"));
        check("spread_of: page 1 faces page iv (sections don't break spreads)",
              core::spread_of(d, "page.5") == (std::vector<std::string>{"page.4", "page.5"}));
        {   // single pages: each its own
            core::Doc e;
            core::load_sample(e);
            core::Doc::Loading load(e);
            auto st = e.settings();
            st.facing = false;
            e.put("doc", st);
            for (const auto& mid : e.ids_of<core::Master>()) {   // s147: facing is each master's
                auto m = *e.as<core::Master>(mid);
                m.facing = false;
                e.put(mid, m);
            }
            check("spreads: not facing, one page each", core::spreads(e).size() == e.pages().size());
        }
        // A frame dragged across the spine belongs to the other page.
        const auto pl = core::layout_pages(d, {"page.6", "page.7"});
        const auto box = core::doc::add_frame(j, d, "page.6", core::FrameKind::Shape, {300, 100, 100, 50});
        core::doc::frames_begin(j, d, {box}, "Move");
        core::doc::frames_set(j, d, box, {420, 100, 100, 50});   // centre at 470: page 7 (from 432)
        const int moved = core::doc::frames_rehome(j, d, pl, {box});
        core::doc::frames_end(j);
        const auto* f = d.as<core::Frame>(box);
        check("across the spine: the right page's now, same place", moved == 1 && f->owner == "page.7" &&
                                                                     std::abs(f->x - (420 - 432)) < 1e-9 && f->y == 100);
        j.undo();
        check("across the spine: one undo puts it back", d.as<core::Frame>(box)->owner == "page.6" &&
                                                        d.as<core::Frame>(box)->x == 300);
    }

    {
        std::cout << "\n-- drop caps --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto fl = L::flow_story(d, "story.1");
        const L::Line* cap = nullptr;
        double cap_x1 = 0;
        for (const auto& l : fl.frames.front().lines)
            if (l.para == 1 && l.cap > 0) { cap = &l; cap_x1 = l.x + l.w; }
        check("drop cap: the first letter, three lines tall", cap && cap->start == 0 && cap->end == 1 &&
                                                              std::abs(cap->h - 3 * 14) < 1e-9 && cap->cap > 30);
        bool beside = false;
        for (const auto& l : fl.frames.front().lines)
            if (cap && l.para == 1 && l.cap <= 0 && l.y < cap->y + cap->h) beside = beside || l.x > cap_x1;
        check("drop cap: the lines beside it start past it", beside);
        const auto c0 = L::caret_at(d, "story.1", fl, {1, 0});
        const auto c1 = L::caret_at(d, "story.1", fl, {1, 1});
        check("drop cap: the caret before it is as tall as it, after it beside", c0.ok && std::abs(c0.h - 42) < 1e-9 &&
                                                                                 c1.ok && c1.x > cap_x1);
        double next_y = 0;
        for (const auto& l : fl.frames.front().lines) if (l.para == 2) { next_y = l.y; break; }
        check("drop cap: a short paragraph still clears it", cap && next_y >= cap->y + cap->h - 0.01);
        check("indent: the paragraph after the drop cap's is indented (not a heading)",
              std::abs(L::caret_at(d, "story.1", fl, {2, 0}).x - 18) < 0.5);
        core::doc::edit_styles(j, d, {"style.10"}, "Drop letters", [](core::Style& s) { s.drop_chars = 2; });
        const auto f2 = L::flow_story(d, "story.1");
        bool two = false;
        for (const auto& l : f2.frames.front().lines) two = two || (l.para == 1 && l.cap > 0 && l.end == 2);
        check("drop cap: two letters", two);
        core::doc::edit_styles(j, d, {"style.10"}, "Drop cap", [](core::Style& s) { s.drop_lines = 0; });
        bool none = true;
        const auto f0 = L::flow_story(d, "story.1");   // (kept: .front() of a temporary dangles)
        for (const auto& l : f0.frames.front().lines) none = none && l.cap <= 0;
        check("drop cap: none", none);
        const auto o = core::project(d, "");
        bool field = false;
        for (const auto& f : o.find("style.10")->fields) field = field || (f.edit == "drop_lines" && f.value == "None");
        check("inspector: Drop cap", field);
    }

    {
        std::cout << "\n-- local formatting --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const core::TextPos a{1, 4}, b{1, 10};   // "harbor"
        const auto story = [&] { return *d.as<core::Story>("story.1"); };
        check("bold: not yet", !core::all_bold(d, story(), a, b));
        check("bold: one step", D::set_local(j, d, "story.1", a, b, "Bold", [](core::Local& l) { l.bold = 1; }) &&
                                core::all_bold(d, story(), a, b) && !core::all_bold(d, story(), {1, 0}, b));
        const auto s1 = story();
        const auto& runs = s1.paras[1].runs;
        check("bold: a run with local formatting, no style", runs.size() == 1 && runs[0].start == 4 && runs[0].end == 10 &&
                                                              runs[0].style.empty() && runs[0].local.bold == 1);
        const auto at = D::edit_text(j, d, "story.1", {1, 7}, {1, 7}, "x");
        check("typing inside: it's bold too", at == core::TextPos{1, 8} && story().paras[1].runs[0].end == 11);
        D::set_char_style(j, d, "story.1", {1, 4}, {1, 11}, "style.5");
        check("a character style on top keeps it", story().paras[1].runs[0].style == "style.5" &&
                                                    story().paras[1].runs[0].local.bold == 1);
        D::set_local(j, d, "story.1", {1, 4}, {1, 11}, "Colour", [](core::Local& l) { l.color = "swatch.3"; l.size = 14; });
        std::string err;
        core::Doc e;
        check("json: round-trips", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        const auto f = core::text_fields(d, "story.1", {1, 5}, {1, 5});
        bool sz = false, co = false, bt = false;
        for (const auto& x : f) {
            sz = sz || (x.edit == "lsize" && x.value == "14 pt");
            co = co || (x.edit == "lcolor" && x.value == "Guild Rust");
            bt = bt || (x.edit == "local" && x.buttons);
        }
        check("inspector: Size, Colour, Bold / Italic / Clear", sz && co && bt);
        D::set_local(j, d, "story.1", {1, 4}, {1, 11}, "Clear overrides", [](core::Local& l) { l = {}; });
        check("clear: back to the style alone", story().paras[1].runs.size() == 1 && story().paras[1].runs[0].local.empty());
        check("italic: the style's own counts", core::all_italic(d, story(), {1, 4}, {1, 11}));   // Emphasis is italic
        namespace L = composix::layout;
        // Drawn: a bold run sets wider than the same words plain.
        core::Doc w;
        load_plain(w);
        core::DocJournal wj{w};
        w.set_door([&wj] { return wj.open(); });
        const auto x0 = L::caret_at(w, "story.1", L::flow_story(w, "story.1"), {1, 10}).x;
        D::set_local(wj, w, "story.1", {1, 4}, {1, 10}, "Bold", [](core::Local& l) { l.bold = 1; });
        const auto x1 = L::caret_at(w, "story.1", L::flow_story(w, "story.1"), {1, 10}).x;
        check("drawn: bold words set wider", x1 > x0 + 0.5);
    }

    {
        std::cout << "\n-- export settings --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("defaults: print, bleed, no profile", d.settings().exp.print && d.settings().exp.bleed && d.settings().exp.profile.empty());
        core::ExportPrefs e{true, true, true, "GRACoL2013_CRPC6.icc", true, 1};
        check("kept: one step", core::doc::set_export(j, d, e) && j.size() == 1 && d.settings().exp == e);
        check("kept: the same again, no step", !core::doc::set_export(j, d, e) && j.size() == 1);
        std::string err;
        core::Doc f;
        check("json: with the document", core::from_json(f, core::to_json(d), &err) && f.settings().exp == e, err);
        L::FlowCache flows;
        L::PreflightOpts o;
        o.missing_profile = "Some Press Profile";
        const auto v = L::preflight_issues(d, flows, o);
        check("preflight: a profile not on this computer is named", std::any_of(v.begin(), v.end(), [](const L::Issue& i) {
                  return i.kind == "profile" && i.text.find("Some Press Profile") != std::string::npos; }));
        j.undo();
        check("undo: back to the defaults", d.settings().exp == core::ExportPrefs{});
    }

    {
        std::cout << "\n-- picture placement --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* f = d.as<core::Frame>("frame.4");   // harbor.psd, 3600 x 2700, filling 324 x 108
        const auto* a = d.as<core::Asset>("asset.2");
        const auto fill = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("fill: as before", std::abs(fill.w - 324) < 1e-6);
        check("scale: fill is a share of its own size", std::abs(core::picture_scale(*f, *a) - 324 / (3600 * 72.0 / 300) * 100) < 0.01);
        check("set scale: one step, by hand now", core::doc::set_picture_scale(j, d, {"frame.4"}, 50) && j.size() == 1 &&
                                                  d.as<core::Frame>("frame.4")->fit == core::ImageFit::Custom);
        f = d.as<core::Frame>("frame.4");
        const auto half = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("set scale: 50% of 12 x 9 in is 6 x 4.5 in, about the old centre",
              std::abs(half.w - 432) < 1e-6 && std::abs(half.h - 324) < 1e-6 &&
                  std::abs(half.x + half.w / 2 - (fill.x + fill.w / 2)) < 1e-6);
        check("effective ppi: 600 at 50%", std::abs(core::effective_ppi(*f, *a) - 600) < 0.5);
        // A move inside the frame (as Alt+drag does), within a gesture: one step.
        core::doc::frames_begin(j, d, {"frame.4"}, "Move picture");
        core::doc::edit_frames(j, d, {"frame.4"}, "Move picture", [](core::Frame& g) { g.pic_x -= 30; g.pic_y += 12; });
        core::doc::frames_end(j);
        f = d.as<core::Frame>("frame.4");
        const auto moved = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("moved: by the drag, the scale kept", std::abs(moved.x - (half.x - 30)) < 1e-6 && std::abs(moved.w - half.w) < 1e-6 &&
                                                    j.size() == 2);
        check("screen units: the same place at a zoom", [&] {
            const auto z = core::picture_rect({10, 20, f->w * 2, f->h * 2}, *f, a->px_w, a->px_h, 2);
            return std::abs(z.x - (10 + moved.x * 2)) < 1e-6 && std::abs(z.w - moved.w * 2) < 1e-6;
        }());
        std::string err;
        core::Doc e;
        check("json: kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        check("fill again: by hand gone", core::doc::set_fit(j, d, {"frame.4"}, core::ImageFit::Fill) &&
                                          std::abs(core::picture_rect({0, 0, f->w, f->h}, *d.as<core::Frame>("frame.4"), a->px_w, a->px_h).w - 324) < 1e-6);
    }

    {
        std::cout << "\n-- baseline grid --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto pages = d.pages();
        int with = 0;
        for (const auto& pid : pages) with += core::baseline_grid(d, pid).step == 14;
        check("sample: the body master has a 14 pt grid from its top margin",
              with > 0 && core::baseline_grid(d, "master.2").step == 14 &&
                  std::abs(core::baseline_grid(d, "master.2").start - d.as<core::Master>("master.2")->margins.top) < 1e-9);
        // Every line's top, story.1, in order.
        const auto tops = [&] {
            std::vector<double> out;
            for (const auto& ft : composix::layout::flow_story(d, "story.1").frames) for (const auto& l : ft.lines) out.push_back(l.y);
            return out;
        };
        const auto before = tops();
        // The baselines of the Body lines on gridded pages: off the grid by how much (worst).
        const auto worst = [&](int* counted) {
            double w = 0;
            *counted = 0;
            const auto* st = d.as<core::Story>("story.1");
            const auto flow = composix::layout::flow_story(d, "story.1");
            for (const auto& ft : flow.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                const auto g = core::baseline_grid(d, fr->owner);
                if (g.step <= 0) continue;
                composix::layout::visit_lines(d, "story.1", flow, ft.frame, [&](const composix::layout::LineVisit& v) {
                    if (v.from < 0) return;
                    const auto& style = st->paras[static_cast<std::size_t>(v.para)].style;
                    if (style != "style.1" && style != "style.10") return;
                    const double r = std::fmod(fr->y + v.baseline - g.start, g.step);
                    w = std::max(w, std::min(std::abs(r), std::abs(g.step - std::abs(r))));
                    ++*counted;
                });
            }
            return w;
        };
        int n0 = 0;
        const double off0 = worst(&n0);
        check("grid off: body baselines are where they were (off the grid)", n0 > 0 && off0 > 0.5, std::to_string(off0));
        check("align to grid: one step", core::doc::edit_styles(j, d, {"style.1", "style.10"}, "Align to grid",
                                                                [](core::Style& st) { st.align_grid = true; }) && j.size() == 1);
        int n1 = 0;
        const double off1 = worst(&n1);
        check("aligned: every body baseline on the grid (drop cap's too)", n1 > 0 && off1 < 0.01, std::to_string(off1) + " over " + std::to_string(n1));
        const auto after = tops();
        bool down = after.size() >= 1;
        // Lines only ever move down (never up past where they would be).
        for (std::size_t i = 0; i < std::min(before.size(), after.size()) && i < 3; ++i) down = down && after[i] >= before[i] - 1e-6;
        check("aligned: lines moved down, not up", down);
        check("master without a grid: nothing to align to", [&] {
            core::doc::edit_masters(j, d, {"master.2", "master.3"}, "Baseline grid", [](core::Master& m) { m.grid = 0; });
            const bool same = tops() == before;
            j.undo();
            return same;
        }());
        std::string err;
        core::Doc e;
        check("json: grid and align kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                               e.as<core::Style>("style.1")->align_grid && e.as<core::Master>("master.2")->grid == 14, err);
        const auto o = core::project(d, "");
        bool mf = false, sf = false;
        for (const auto& f : o.find("master.2")->fields) mf = mf || (f.edit == "mgrid" && f.value == "14 pt");
        for (const auto& f : o.find("style.1")->fields) sf = sf || (f.edit == "align_grid" && f.value == "Align to grid");
        check("inspector: the master's grid and the style's alignment", mf && sf);
        j.undo();
        check("undo: the flow as it was", tops() == before);
    }

    {
        std::cout << "\n-- styles on styles (s037) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&](const char* id) { return *d.as<core::Style>(id); };
        check("sample: Body First is based on Body, owning its indent and drop cap",
              S("style.10").based_on == "style.1" &&
                  S("style.10").own == std::vector<std::string>{"drop_lines", "first_indent"});
        // A change to Body ripples into Body First (what it doesn't set) -- one step.
        check("edit base: one step", core::doc::edit_styles(j, d, {"style.1"}, "Size",
                                                            [](core::Style& s) { s.size = 12; s.leading = 15; }) && j.size() == 1);
        check("edit base: the based-on style follows", S("style.10").size == 12 && S("style.10").leading == 15);
        check("edit base: its own settings stay", S("style.10").first_indent == 0 && S("style.10").drop_lines == 3);
        core::doc::edit_styles(j, d, {"style.1"}, "First indent", [](core::Style& s) { s.first_indent = 24; });
        check("edit base: an owned setting doesn't follow", S("style.10").first_indent == 0);
        j.undo();
        j.undo();
        check("undo: base and follower as they were", S("style.1").size == 11 && S("style.10").size == 11);
        // A change on the based-on style becomes its own.
        core::doc::edit_styles(j, d, {"style.10"}, "Leading", [](core::Style& s) { s.leading = 16; });
        check("edit follower: the setting is its own now",
              [&] { const auto own = S("style.10").own; return std::count(own.begin(), own.end(), "leading") == 1; }());
        core::doc::edit_styles(j, d, {"style.1"}, "Leading", [](core::Style& s) { s.leading = 13; });
        check("edit follower: then the base's change doesn't override it", S("style.10").leading == 16);
        check("reset to base: back to Body's", core::doc::reset_to_base(j, d, {"style.10"}) &&
                                                  S("style.10").leading == 13 && S("style.10").first_indent == 18 &&
                                                  S("style.10").drop_lines == 0 && S("style.10").own.empty());
        j.undo(); j.undo(); j.undo();
        // Chains: Pull Quote on Body First on Body -- Body's change goes two deep.
        check("base on: the look doesn't change", [&] {
            const auto before = S("style.6");
            core::doc::set_style_base(j, d, "style.6", "style.10");
            auto after = S("style.6");
            after.based_on.clear();
            after.own.clear();
            return after == before && S("style.6").based_on == "style.10" && !S("style.6").own.empty();
        }());
        core::doc::edit_styles(j, d, {"style.1"}, "Font", [](core::Style& s) { s.family = "Sans"; });
        check("chain: a change two levels up reaches the end", S("style.10").family == "Sans" && S("style.6").family == "Sans");
        check("chain: descendants in order",
              core::doc::style_descendants(d, "style.1") == std::vector<core::Doc::Id>{"style.12", "style.10", "style.6"});   // s114: Footnote on Body (by position)
        j.undo();
        check("no loops: Body can't be based on what's based on it",
              !core::doc::can_base_on(d, "style.1", "style.6") && !core::doc::can_base_on(d, "style.1", "style.1") &&
                  !core::doc::set_style_base(j, d, "style.1", "style.10"));
        check("no character bases", !core::doc::can_base_on(d, "style.1", "style.5"));
        // Deleting a base: those on it take its base and look the same.
        {
            const auto pq = S("style.6");
            core::doc::set_style_base(j, d, "style.6", "style.3");   // Caption is used; base on Pull Quote instead
            j.undo();
            (void)pq;
            core::doc::set_style_base(j, d, "style.3", "style.6");   // Caption on Pull Quote (unused)
            const auto cap = S("style.3");
            check("delete a base: one step", core::doc::delete_styles(j, d, {"style.6"}));
            auto now = S("style.3");
            check("delete a base: the style on it moves to its base, unchanged",
                  now.based_on == "style.10" && now.size == cap.size && now.italic == cap.italic && now.align == cap.align);
            j.undo();
            j.undo();
        }
        // Next style: Enter at a heading's end starts Body First; Enter mid-paragraph keeps the style.
        {
            const auto caret = core::doc::edit_text(j, d, "story.1", {0, 11}, {0, 11}, "\n");
            const auto& paras = d.as<core::Story>("story.1")->paras;
            check("next style: Enter after Chapter One starts Body First",
                  caret.para == 1 && paras[1].style == "style.10" && paras[0].style == "style.2");
            j.undo();
            core::doc::edit_text(j, d, "story.1", {0, 4}, {0, 4}, "\n");
            check("next style: Enter inside a heading splits it as Heading",
                  d.as<core::Story>("story.1")->paras[1].style == "style.2");
            j.undo();
            core::doc::edit_text(j, d, "story.1", {0, 11}, {0, 11}, "a\nb");
            // s081 (was "a paste keeps the style"): a paste at a heading's end
            // runs through Next style too -- the backlog's "Next style for a paste".
            check("next style: a paste at the heading's end takes its Next", d.as<core::Story>("story.1")->paras[1].style == "style.10");
            j.undo();
            const auto& b1 = d.as<core::Story>("story.1")->paras[1];
            core::doc::edit_text(j, d, "story.1", {1, static_cast<int>(b1.text.size())}, {1, static_cast<int>(b1.text.size())}, "\n");
            check("next style: Enter after Body First goes on in Body", d.as<core::Story>("story.1")->paras[2].style == "style.1");
            j.undo();
        }
        // Kept in the file; shown in the inspector.
        {
            core::doc::set_style_base(j, d, "style.6", "style.10");
            std::string err;
            core::Doc e;
            check("json: based on, own and next kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                                         e.as<core::Style>("style.6")->based_on == "style.10" &&
                                                         e.as<core::Style>("style.2")->next == "style.10", err);
            const auto o = core::project(d, "");
            bool base = false, own = false, nx = false, kids = false, reset = false;
            for (const auto& f : o.find("style.10")->fields) {
                base = base || (f.edit == "based_on" && f.value == "Body");
                own = own || (f.label == "Own settings" && f.value == "Drop cap, First indent");
                reset = reset || (f.edit == "style_base" && f.buttons);
                nx = nx || (f.edit == "next_style" && f.value == "Body");
                kids = kids || (f.label == "Styles on it" && f.value == "Pull Quote");
            }
            check("inspector: Based on, Own settings, Reset, Next style, Styles on it", base && own && reset && nx && kids);
            bool loopfree = true;
            for (const auto& f : o.find("style.1")->fields)
                if (f.edit == "based_on")
                    for (const auto& c : f.choices) loopfree = loopfree && c.id != "style.10" && c.id != "style.6" && c.id != "style.1";
            check("inspector: Body's Based on offers no style built on it", loopfree);
            j.undo();
        }
    }

    {
        std::cout << "\n-- hyphenation (s038) --\n";
        check("patterns: hy-phen-ation", core::hyphenated("hyphenation") == "hy-phen-ation", core::hyphenated("hyphenation"));
        check("patterns: an exception (ta-ble)", core::hyphenated("table") == "ta-ble", core::hyphenated("table"));
        check("patterns: equinox, cargo", core::hyphenated("equinox") == "equinox" || core::hyphenated("equinox") == "equi-nox",
              core::hyphenated("equinox"));
        check("patterns: capitals fold", core::hyphenated("Hyphenation") == "Hy-phen-ation");
        check("patterns: 2 before, 3 after", core::hyphen_points("oranges").empty() || core::hyphen_points("oranges").front() >= 2,
              core::hyphenated("oranges"));
        check("patterns: short words and non-letters don't break",
              core::hyphen_points("ship").empty() && core::hyphen_points("caf\xC3\xA9s").empty() && core::hyphen_points("x86-64").empty());
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A narrow justified frame of long words: hyphens are certain.
        std::string words;
        for (int k = 0; k < 12; ++k) words += "Uncharacteristically representational communication institutionalization ";
        words.pop_back();
        core::doc::edit_text(j, d, "story.1", {2, 0}, {2, static_cast<int>(d.as<core::Story>("story.1")->paras[2].text.size())}, words);
        const auto count = [&](int* lines = nullptr) {
            int h = 0, n = 0;
            for (const auto& f : L::flow_story(d, "story.1").frames)
                for (const auto& l : f.lines)
                    if (l.para == 2) { h += l.hyphen; ++n; }
            if (lines) *lines = n;
            return h;
        };
        int n_on = 0, n_off = 0;
        const int h = count(&n_on);
        check("flow: Body hyphenates the long words", h > 0, std::to_string(h) + " of " + std::to_string(n_on));
        bool mid = false, between = true;
        const auto& txt = d.as<core::Story>("story.1")->paras[2].text;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines)
                if (l.para == 2 && l.hyphen) {
                    mid = mid || std::isalpha(static_cast<unsigned char>(txt[static_cast<std::size_t>(l.end)]));
                    between = between && std::isalpha(static_cast<unsigned char>(txt[static_cast<std::size_t>(l.end - 1)]));
                }
        check("flow: a hyphenated line ends inside a word", mid && between);
        // Drawn: the hyphen is on the page (the line's glyphs end in '-'), the text unchanged.
        {
            const auto flow = L::flow_story(d, "story.1");
            int dashed = 0;
            for (const auto& f : flow.frames)
                L::visit_lines(d, "story.1", flow, f.frame, [&](const L::LineVisit& v) {
                    if (v.from < 0 || !v.line) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    const int e = v.line->start_index + v.line->length;
                    if (e > 0 && t[e - 1] == '-') ++dashed;
                });
            check("draw: the hyphenated lines end in a hyphen", dashed >= h, std::to_string(dashed) + " / " + std::to_string(h));
            check("text: no hyphen in the words", d.as<core::Story>("story.1")->paras[2].text == words);
        }
        // Caret at a hyphenated line's end belongs to the next line, as at any soft wrap.
        {
            const auto flow = L::flow_story(d, "story.1");
            const L::Line* hl = nullptr;
            for (const auto& f : flow.frames) for (const auto& l : f.lines) if (!hl && l.para == 2 && l.hyphen) hl = &l;
            const auto c1 = L::caret_at(d, "story.1", flow, {2, hl->end - 1});
            const auto c2 = L::caret_at(d, "story.1", flow, {2, hl->end});
            check("caret: before the break on the line, at it on the next", c1.ok && c2.ok && std::abs(c1.y - hl->y) < 0.5 && c2.y > c1.y + 1,
                  std::to_string(c1.y) + " " + std::to_string(c2.y));
        }
        check("style off: one step", core::doc::edit_styles(j, d, {"style.1"}, "Hyphenate",
                                                            [](core::Style& s) { s.hyphenate = false; }));
        check("style off: no hyphens (and Body First follows Body)", count(&n_off) == 0 && !d.as<core::Style>("style.10")->hyphenate);
        check("style off: hyphens save lines", n_off >= n_on, std::to_string(n_on) + " vs " + std::to_string(n_off));
        j.undo();
        check("undo: hyphens back", count() == h);
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find("style.1")->fields) row = row || (f.edit == "hyphenate" && f.value == "On");
        check("inspector: Hyphenate On", row);
        std::string err;
        core::Doc e;
        check("json: hyphenate kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>("style.1")->hyphenate, err);
    }

    {
        std::cout << "\n-- keeps (s039) --\n";
        namespace L = composix::layout;
        // Breaches of the keeps at every column break of story.1: a heading
        // left at a column's foot, a paragraph's first line alone at a foot
        // (orphan), its last alone at a head (widow). `checked`: column breaks seen.
        const auto breaches = [](const core::Doc& d, int* checked, int* splits) {
            const auto& st = *d.as<core::Story>("story.1");
            std::vector<std::vector<L::Line>> cols;
            for (const auto& f : L::flow_story(d, "story.1").frames) {
                int last = -1;
                for (const auto& l : f.lines) {
                    if (l.col != last) { cols.emplace_back(); last = l.col; }
                    cols.back().push_back(l);
                }
            }
            int bad = 0;
            for (std::size_t k = 0; k + 1 < cols.size(); ++k) {
                const auto& A = cols[k];
                const auto& B = cols[k + 1];
                if (A.empty() || B.empty()) continue;
                ++*checked;
                const auto& last = A.back();
                const auto& first = B.front();
                const auto sty = [&](int p) { return d.as<core::Style>(st.paras[static_cast<std::size_t>(p)].style); };
                const int plen = static_cast<int>(st.paras[static_cast<std::size_t>(last.para)].text.size());
                if (last.para == first.para) {   // split
                    ++*splits;
                    int na = 0, nb = 0;
                    bool starts = false, others = false;
                    for (const auto& l : A) if (l.para == last.para) { na += l.cap <= 0; starts = starts || l.start == 0; } else others = true;
                    bool ends = false;
                    for (const auto& l : B) if (l.para == last.para) { nb += l.cap <= 0; ends = ends || l.end == plen; }
                    if (starts && others && na < 2) ++bad;
                    if (ends && nb < 2 && na - (2 - nb) >= 2) ++bad;
                } else if (sty(last.para)->keep_next && first.start == 0) {
                    bool others = false;
                    for (const auto& l : A) others = others || l.para != last.para;
                    if (others) ++bad;
                }
            }
            return bad;
        };
        // Many layouts: n Body paragraphs of different lengths before the
        // second chapter's heading.
        const auto with_filler = [](core::Doc& d, int n, int words) {
            auto s = *d.as<core::Story>("story.1");
            std::vector<core::Para> extra;
            for (int k = 0; k < n; ++k) {
                std::string t;
                for (int w = 0; w < words + (k * 7) % 23; ++w) t += (w ? " " : "") + std::string(w % 3 ? "harbor" : "lanterns");
                extra.push_back({"style.1", t + "."});
            }
            s.paras.insert(s.paras.begin() + 4, extra.begin(), extra.end());
            d.put("story.1", s);
        };
        int bad_on = 0, bad_off = 0, seen = 0, splits = 0, seen_off = 0, splits_off = 0;
        // Wide enough that any fonts meet a breach with keeps off (Fedora's
        // found none in the first, 36-layout sweep).
        for (int n : {1, 3, 5})
            for (int words = 5; words <= 120; words += 3) {
                core::Doc d;
                core::load_sample(d);
                with_filler(d, n, words);
                bad_on += breaches(d, &seen, &splits);
                core::Doc e;
                core::load_sample(e);
                for (const char* id : {"style.1", "style.2", "style.10"}) {
                    auto s = *e.as<core::Style>(id);
                    s.keep_next = false;
                    s.keep_lines = 0;
                    e.put(id, s);
                }
                with_filler(e, n, words);
                bad_off += breaches(e, &seen_off, &splits_off);
            }
        check("keeps off: the sweep meets widows, orphans or stranded headings", bad_off > 0,
              std::to_string(bad_off) + " in " + std::to_string(seen_off) + " breaks");
        check("keeps on: none, across the sweep", bad_on == 0 && seen > 60 && splits > 20,
              std::to_string(bad_on) + " in " + std::to_string(seen) + " breaks, " + std::to_string(splits) + " splits");
        {   // The text is all still there (nothing lost to the moves).
            core::Doc d;
            core::load_sample(d);
            with_filler(d, 6, 41);
            const auto& st = *d.as<core::Story>("story.1");
            const auto flow = L::flow_story(d, "story.1");
            int next_para = 0, next_byte = 0;
            bool contiguous = true;
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines) {   // (a drop cap's line holds the first letters)
                    if (l.para != next_para) {
                        contiguous = contiguous && l.para == next_para + 1 && l.start == 0 &&
                                     next_byte == static_cast<int>(st.paras[static_cast<std::size_t>(next_para)].text.size());
                        next_para = l.para;
                    } else {
                        contiguous = contiguous && l.start == next_byte;
                    }
                    next_byte = l.end;
                }
            check("moves lose nothing: lines run on, paragraph after paragraph", contiguous);
        }
        {   // All together: a paragraph that starts in a column is never split from there.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            with_filler(d, 7, 41);
            check("all together: one step", core::doc::edit_styles(j, d, {"style.1"}, "Keep lines",
                                                                   [](core::Style& s) { s.keep_lines = -1; }));
            check("all together: Body First follows Body", d.as<core::Style>("style.10")->keep_lines == -1);
            int split_starts = 0, n = 0;
            std::vector<std::vector<L::Line>> cols;
            for (const auto& f : L::flow_story(d, "story.1").frames) {
                int last = -1;
                for (const auto& l : f.lines) { if (l.col != last) { cols.emplace_back(); last = l.col; } cols.back().push_back(l); }
            }
            for (std::size_t k = 0; k + 1 < cols.size(); ++k) {
                if (cols[k].empty() || cols[k + 1].empty()) continue;
                ++n;
                bool others = false, starts = false;
                for (const auto& l : cols[k]) {
                    if (l.para == cols[k].back().para) starts = starts || l.start == 0;
                    else others = true;
                }
                split_starts += cols[k].back().para == cols[k + 1].front().para && starts && others;
            }
            check("all together: no paragraph starts and splits in one column", split_starts == 0 && n > 0,
                  std::to_string(split_starts) + " of " + std::to_string(n));
            const auto o = core::project(d, "");
            bool kl = false, kn = false;
            for (const auto& f : o.find("style.1")->fields) kl = kl || (f.edit == "keep_lines" && f.value == "All together");
            for (const auto& f : o.find("style.2")->fields) kn = kn || (f.edit == "keep_next" && f.value == "1 line");   // s257: lines
            check("inspector: Keep lines, Keep with next", kl && kn);
            std::string err;
            core::Doc e;
            check("json: keeps kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>("style.1")->keep_lines == -1 &&
                                          e.as<core::Style>("style.2")->keep_next, err);
        }
    }

    {
        std::cout << "\n-- text insets (s040) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto lines_of = [&](const std::string& story, const std::string& frame) {
            for (const auto& f : L::flow_story(d, story).frames) if (f.frame == frame) return f.lines;
            return std::vector<L::Line>{};
        };
        const auto cap = lines_of("story.2", "frame.6");
        check("sample: the map caption is inset 6 pt", d.as<core::Frame>("frame.6")->inset == core::Sides{6, 6, 6, 6} &&
                                                           !cap.empty() && std::abs(cap[0].y - 6) < 0.01 && std::abs(cap[0].x - 6) < 0.01,
              cap.empty() ? "no lines" : std::to_string(cap[0].x) + "," + std::to_string(cap[0].y));
        check("columns: inside the insets", std::abs(core::column_x(*d.as<core::Frame>("frame.6"), 0) - 6) < 1e-9 &&
                                                std::abs(core::column_width(*d.as<core::Frame>("frame.6"), 0) - (d.as<core::Frame>("frame.6")->w - 12)) < 1e-9);
        // The opening text: an inset of 18 pt moves its first line in and down, and the
        // bottom inset ends the column sooner (more text left over).
        const auto& tf = *d.as<core::Frame>("frame.1");
        const auto before = lines_of("story.1", "frame.1");
        check("inset: one step", core::doc::edit_frames(j, d, {"frame.1"}, "Text inset",
                                                        [](core::Frame& f) { f.inset = {18, 18, 18, 18}; }) && j.size() == 1);
        const auto after = lines_of("story.1", "frame.1");
        check("inset: the first line starts 18 pt down", !after.empty() && std::abs(after[0].y - (before[0].y + 18)) < 0.01,
              std::to_string(before[0].y) + " -> " + std::to_string(after.empty() ? -1 : after[0].y));
        double maxb = 0;
        for (const auto& l : after) maxb = std::max(maxb, l.y + l.h);
        check("inset: no line below the bottom inset", maxb <= tf.h - 18 + 0.01, std::to_string(maxb));
        bool inside = true;
        for (const auto& l : after) inside = inside && l.x >= 18 - 0.01 && (l.w <= 0 || l.x + l.w <= tf.w - 18 + 0.01);
        check("inset: every line inside the left / right insets", inside);
        // Columns split what is left inside the insets.
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 2; });
        const auto& f2 = *d.as<core::Frame>("frame.1");
        check("inset + columns: the second column ends at the right inset",
              std::abs(core::column_x(f2, 1) + core::column_width(f2, 1) - (f2.w - 18)) < 1e-6 && std::abs(core::column_x(f2, 0) - 18) < 1e-9);
        j.undo();
        j.undo();
        check("undo: as it was", lines_of("story.1", "frame.1").size() == before.size() &&
                                     std::abs(lines_of("story.1", "frame.1")[0].y - before[0].y) < 1e-9);
        bool row = false;
        const auto items1 = core::frame_items(d, {"frame.6"});   // (kept: a range-for over a temporary's member dangles)
        for (const auto& f : items1.at(0).fields) row = row || (f.edit == "inset" && f.value == "6\t6\t6\t6");
        check("inspector: the caption's Inset", row);
        std::string err;
        core::Doc e;
        check("json: inset kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                      e.as<core::Frame>("frame.6")->inset == core::Sides{6, 6, 6, 6}, err);
    }

    {
        std::cout << "\n-- typed fields (s041) --\n";
        const auto is = [](const char* t, double unit, double want) {
            const auto v = core::parse_length(t, unit);
            return v && std::abs(*v - want) < 1e-9;
        };
        check("parse: points", is("12", 1, 12) && is("12.5 pt", 1, 12.5) && is(" 9PT ", 1, 9) && is("-12", 1, -12) && is(".5", 1, 0.5));
        check("parse: inches", is("1in", 1, 72) && is("1.5 inches", 1, 108) && is("2\"", 1, 144) && is("0.75", 72, 54));
        check("parse: picas", is("3p6", 1, 42) && is("6p", 1, 72) && is("p6", 1, 6) && is("2pc", 1, 24) && is("-1p6", 1, -18));
        check("parse: metric", is("25.4mm", 1, 72) && is("2.54 cm", 1, 72));
        check("parse: not lengths", !core::parse_length("", 1) && !core::parse_length("abc", 1) && !core::parse_length("12 furlongs", 1) &&
                                        !core::parse_length("1.2.3", 1) && !core::parse_length("Auto", 1) && !core::parse_length("p", 1));
        check("num_text: trims", core::num_text(12) == "12" && core::num_text(12.5) == "12.5" && core::num_text(1.0 / 3) == "0.333" &&
                                     core::num_text(-0.0001) == "0");
        core::Doc d;
        core::load_sample(d);
        const auto o = core::project(d, "");
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        const auto* ss = field(o.find("style.1")->fields, "ssize");
        check("style Size: typed in points, its own key (not the page's \"size\")",
              ss && ss->unit == 1 && ss->hi > 100 && !field(o.find("style.1")->fields, "size"));
        const auto* fi = field(o.find("style.1")->fields, "findent");
        check("First indent: typed, may hang (negative)", fi && fi->unit == 1 && fi->lo < 0);
        const auto* mt = field(o.find("master.2")->fields, "mtop");
        check("master margins: typed, a bare number in inches", mt && mt->unit == 72 && core::parse_length(mt->value, mt->unit));
        const auto fr = core::frame_items(d, {"frame.6"});
        const auto* b = field(fr.at(0).fields, "bounds");
        check("frame bounds: X Y W H typed, inches", b && b->unit == 72 && b->cols.size() == 4);
        check("frame inset: typed", field(fr.at(0).fields, "inset") && field(fr.at(0).fields, "inset")->unit == 1);
        const auto two = core::frame_items(d, {"frame.1", "frame.6"});
        std::vector<const core::Item*> sel{&two[0], &two[1]};
        const auto shared = core::shared_fields(sel);   // (kept: a pointer into a temporary dangles)
        const auto* sb = field(shared, "bounds");
        check("two frames: bounds still typed, Mixed where they differ", sb && sb->unit == 72 &&
                                                                              sb->value.find(core::kMixed) != std::string::npos);
        // Every typed field's shown value reads back as a length (or is a word preset: Auto, Inherit, Mixed).
        bool reads = true;
        std::string bad;
        for (const auto& it : o.items())
            for (const auto& f : it.fields)
                if (f.unit > 0 && f.cols.empty() && f.value != core::kMixed && !core::parse_length(f.value, f.unit) &&
                    std::none_of(f.choices.begin(), f.choices.end(), [&](const core::Choice& c) { return c.label == f.value; })) {
                    reads = false;   // (a word shown must be one of its presets: Auto, Inherit, None)
                    bad = f.label + "=" + f.value;
                }
        check("typed values read back as lengths", reads, bad);
    }

    {
        std::cout << "\n-- vertical justification (s042) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // The dedication's box, made tall (room to spare on any machine), three paragraphs.
        core::doc::edit_frames(j, d, {"frame.7"}, "Resize", [](core::Frame& f) { f.h = 400; f.inset = {10, 0, 20, 0}; });
        core::doc::edit_text(j, d, "story.3", {0, 3}, {0, 3}, "\n");
        core::doc::edit_text(j, d, "story.3", {1, 6}, {1, 6}, "\n");
        const auto lines = [&] {
            for (const auto& f : L::flow_story(d, "story.3").frames) if (f.frame == "frame.7") return f.lines;
            return std::vector<L::Line>{};
        };
        struct Span { double a, b; };   // (not std::pair: GCC's psabi note)
        const auto span = [](const std::vector<L::Line>& ls) {
            double a = 1e9, b = -1e9;
            for (const auto& l : ls) { a = std::min(a, l.y); b = std::max(b, l.y + l.h); }
            return Span{a, b};
        };
        check("sample: the dedication is centred", d.as<core::Frame>("frame.7")->valign == core::VAlign::Center);
        const auto set = [&](core::VAlign v) {
            core::doc::edit_frames(j, d, {"frame.7"}, "Vertical justification", [&](core::Frame& f) { f.valign = v; });
            return lines();
        };
        const auto top = set(core::VAlign::Top);
        const auto [t0, t1] = span(top);
        check("top: from the top inset", top.size() >= 3 && std::abs(t0 - 10) < 0.01, std::to_string(t0));
        const auto mid = set(core::VAlign::Center);
        const auto [c0, c1] = span(mid);
        check("centre: equal room above and below (inside the insets)", std::abs((c0 - 10) - (380 - c1)) < 0.01 && c0 > t0 + 1,
              std::to_string(c0) + " / " + std::to_string(c1));
        const auto low = set(core::VAlign::Bottom);
        const auto [b0, b1] = span(low);
        check("bottom: the last line ends at the bottom inset", std::abs(b1 - 380) < 0.01 && std::abs((b1 - b0) - (t1 - t0)) < 0.01);
        const auto just = set(core::VAlign::Justify);
        const auto [j0, j1] = span(just);
        bool kept = just.size() == top.size();
        for (std::size_t i = 0; kept && i < just.size(); ++i)
            kept = just[i].para == top[i].para && just[i].start == top[i].start && just[i].y >= top[i].y - 0.01;
        check("justify: fills from the top inset to the bottom one, the same lines", std::abs(j0 - 10) < 0.01 && std::abs(j1 - 380) < 0.01 && kept,
              std::to_string(j0) + " / " + std::to_string(j1));
        bool para_gaps = true;   // lines of one paragraph keep their spacing; the room goes between paragraphs
        for (std::size_t i = 1; i < just.size(); ++i)
            if (just[i].para == just[i - 1].para)
                para_gaps = para_gaps && std::abs((just[i].y - just[i - 1].y) - (top[i].y - top[i - 1].y)) < 0.01;
        check("justify: the room goes between paragraphs", para_gaps);
        // Caret follows the moved lines.
        const auto flow = L::flow_story(d, "story.3");
        const auto cr = L::caret_at(d, "story.3", flow, {2, 0});
        const L::Line* l2 = nullptr;
        for (const auto& l : just) if (!l2 && l.para == 2) l2 = &l;
        check("caret: on the moved line", cr.ok && l2 && std::abs(cr.y - l2->y) < l2->h, std::to_string(cr.y));
        // Text wrap leaves it at the top.
        core::doc::edit_frames(j, d, {"frame.7"}, "Vertical justification", [](core::Frame& f) { f.valign = core::VAlign::Bottom; });
        check("inspector: Vertical", [&] {
            const auto items2 = core::frame_items(d, {"frame.7"});   // (kept: a range-for over a temporary's member dangles)
            for (const auto& f : items2.at(0).fields) if (f.edit == "valign" && f.value == "Bottom") return true;
            return false;
        }());
        std::string err;
        core::Doc e;
        check("json: valign kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        const auto opening = [&](core::Doc& doc) {
            for (const auto& f : L::flow_story(doc, "story.1").frames) if (f.frame == "frame.1") return f.lines;
            return std::vector<L::Line>{};
        };
        const auto before = opening(d);
        core::doc::edit_frames(j, d, {"frame.1"}, "Vertical justification", [](core::Frame& f) { f.valign = core::VAlign::Bottom; });
        const auto after = opening(d);
        check("text wrap: the opening text (round a picture) stays at the top",
              !before.empty() && after.size() == before.size() && std::abs(after[0].y - before[0].y) < 1e-9);
    }

    {
        std::cout << "\n-- paragraph start (s043) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Where each paragraph of story.1 begins: frame, column, and that frame's page.
        const auto where = [&](int para) {
            const auto flow = L::flow_story(d, "story.1");
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines)
                    if (l.para == para && l.start == 0) return std::tuple{f.frame, l.col, d.as<core::Frame>(f.frame)->owner};
            return std::tuple{std::string{}, -1, std::string{}};
        };
        const auto first_line = [&](const std::string& frame) {
            for (const auto& f : L::flow_story(d, "story.1").frames)
                if (f.frame == frame && !f.lines.empty()) return f.lines.front().para;
            return -1;
        };
        check("sample: Heading 1 starts a right-hand page", d.as<core::Style>("style.2")->start == core::ParaStart::NextRightPage);
        const auto [f1, c1, p1] = where(0);
        check("story start: Chapter One where it is (nothing before it)", f1 == "frame.1");
        const auto [f4, c4, p4] = where(4);
        check("Chapter Two: on a right-hand page after Chapter One's",
              !f4.empty() && p4 != p1 && d.side_of(p4) != core::Side::Left && d.page_index(p4) > d.page_index(p1),
              f4 + " on " + p4);
        check("Chapter Two: first in its frame", first_line(f4) == 4);
        bool between_empty = true;   // frames on pages it passed hold nothing of it
        for (const auto& f : L::flow_story(d, "story.1").frames) {
            const auto pg = d.as<core::Frame>(f.frame)->owner;
            if (d.page_index(pg) > d.page_index(p1) && d.page_index(pg) < d.page_index(p4)) between_empty = between_empty && f.lines.empty();
        }
        check("the pages between: left empty", between_empty);
        // Next page (any side): the very next page with a frame of the thread.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextPage; });
        const auto [g4, gc, gp] = where(4);
        check("next page: a later page, first in its frame", d.page_index(gp) > d.page_index(p1) && first_line(g4) == 4);
        // Next frame: the next frame of the thread.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextFrame; });
        const auto [h4, hc, hp] = where(4);
        check("next frame: frame.2, first in it", h4 == "frame.2" && first_line("frame.2") == 4, h4);
        // Next column: two columns in frame.1 -- Chapter Two heads the second.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextColumn; });
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 2; });
        const auto [k4, kc, kp] = where(4);
        bool heads = true;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines)
                if (f.frame == k4 && l.col == kc && l.para < 4) heads = false;
        check("next column: a fresh column, nothing before it there", !k4.empty() && (kc > 0 || k4 != "frame.1") && heads,
              k4 + " col " + std::to_string(kc));
        // Anywhere: back on page 1 with the rest.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::Anywhere; });
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 1; });
        check("anywhere: Chapter Two follows on", std::get<0>(where(4)) == "frame.1");
        check("no text lost: nothing overset", !L::flow_story(d, "story.1").overset);
        j.undo(); j.undo();
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find("style.2")->fields) row = row || (f.edit == "pstart" && f.value == "Next column");
        check("inspector: Start", row);
        std::string err;
        core::Doc e;
        check("json: start kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- paragraph rules (s044) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto rules = [&] {
            const auto flow = L::flow_story(d, "story.1");
            return L::paragraph_rules(d, "story.1", flow, "frame.1");
        };
        // The baseline of a paragraph's first / last line in frame.1, as drawn.
        const auto base_of = [&](int para, bool last) {
            const auto flow = L::flow_story(d, "story.1");
            double b = -1;
            L::visit_lines(d, "story.1", flow, "frame.1", [&](const L::LineVisit& v) {
                if (v.para == para && v.from >= 0 && (b < 0 || last)) b = v.baseline;
            });
            return b;
        };
        const auto& fr = *d.as<core::Frame>("frame.1");
        const auto r0 = rules();
        check("sample: one rule under Chapter One (Guild Rust, 1 pt)", r0.size() == 1 && r0[0].color == "swatch.3" &&
                                                                         std::abs(r0[0].r.h - 1) < 1e-9);
        check("below: its top 4 pt under the heading's baseline", !r0.empty() && std::abs(r0[0].r.y - (base_of(0, true) + 4)) < 0.01,
              r0.empty() ? "" : std::to_string(r0[0].r.y) + " vs " + std::to_string(base_of(0, true)));
        check("below: across the column", !r0.empty() && std::abs(r0[0].r.x - core::column_x(fr, 0)) < 0.01 &&
                                             std::abs(r0[0].r.w - core::column_width(fr, 0)) < 0.01);
        // Above, on Body First (a drop cap: its first line is the one beside the cap), text width.
        check("above: one step", core::doc::edit_styles(j, d, {"style.10"}, "Rule", [](core::Style& s) {
            s.rule_above = {2, "", 0.5, 12, true};
        }) && j.size() == 1);
        const auto r1 = rules();
        const core::Rect* above = nullptr;
        for (const auto& m : r1) if (m.color.empty()) above = &m.r;
        const auto flow = L::flow_story(d, "story.1");
        double first_base = -1;   // the line beside the cap: the first non-cap line of para 1
        L::visit_lines(d, "story.1", flow, "frame.1", [&](const L::LineVisit& v) {
            if (v.para == 1 && v.from > 0 && first_base < 0) first_base = v.baseline;
        });
        check("above: its bottom 12 pt over the first line's baseline", above && std::abs(above->y + above->h - (first_base - 12)) < 0.01,
              above ? std::to_string(above->y + above->h) + " vs " + std::to_string(first_base - 12) : "none");
        check("above: text width -- inside the column", above && above->x >= core::column_x(fr, 0) - 0.01 &&
                                                         above->x + above->w <= core::column_x(fr, 0) + core::column_width(fr, 0) + 0.01);
        check("above: Body First's own (Body has none)", d.as<core::Style>("style.1")->rule_above.weight == 0 &&
                                                         std::count(d.as<core::Style>("style.10")->own.begin(),
                                                                    d.as<core::Style>("style.10")->own.end(), "rule_above") == 1);
        // Rules don't move text.
        j.undo();
        const auto before = L::flow_story(d, "story.1");
        core::doc::edit_styles(j, d, {"style.2"}, "Rule", [](core::Style& s) { s.rule_above = {6, "", 1, 30, false}; });
        const auto after = L::flow_story(d, "story.1");
        bool same = before.frames.size() == after.frames.size();
        for (std::size_t i = 0; same && i < before.frames.size(); ++i) {
            same = before.frames[i].lines.size() == after.frames[i].lines.size();
            for (std::size_t k = 0; same && k < before.frames[i].lines.size(); ++k)
                same = before.frames[i].lines[k].y == after.frames[i].lines[k].y && before.frames[i].lines[k].start == after.frames[i].lines[k].start;
        }
        check("rules take no room: the text where it was", same);
        j.undo();
        const auto o = core::project(d, "");
        bool w = false, c = false, ofs = false;
        for (const auto& f : o.find("style.2")->fields) {
            w = w || (f.edit == "rule_b_w" && f.value == "1 pt");
            c = c || (f.edit == "rule_b_c" && f.value == "Guild Rust");
            ofs = ofs || (f.edit == "rule_b_o" && f.value == "4 pt" && f.unit == 1);
        }
        check("inspector: Rule below, its colour and offset (typed)", w && c && ofs);
        std::string err;
        core::Doc e;
        check("json: rules kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                      e.as<core::Style>("style.2")->rule_below.weight == 1, err);
    }

    {
        std::cout << "\n-- bullets and numbering (s045) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& sources = *d.as<core::Story>("story.4");
        check("sample: the Sources are numbered 1. 2. 3.",
              core::doc::list_marker(d, sources, 0) == "1." && core::doc::list_marker(d, sources, 1) == "2." &&
                  core::doc::list_marker(d, sources, 2) == "3.");
        check("not a list: no marker", core::doc::list_marker(d, *d.as<core::Story>("story.1"), 2).empty());
        // The markers as drawn: one per paragraph, left of its first line, on its baseline.
        const auto marks = [&](const std::string& story) {
            std::vector<std::pair<std::string, int>> out;
            const auto flow = L::flow_story(d, story);
            for (const auto& f : flow.frames) {
                std::map<int, double> first_x, first_base, mark_x, mark_base;
                L::visit_lines(d, story, flow, f.frame, [&](const L::LineVisit& v) {
                    if (v.from == -2) {
                        out.push_back({pango_layout_get_text(v.line->layout), v.para});
                        mark_x[v.para] = v.x;
                        mark_base[v.para] = v.baseline;
                    } else if (v.from == 0 && !first_x.count(v.para)) {
                        first_x[v.para] = v.x;
                        first_base[v.para] = v.baseline;
                    }
                });
                for (const auto& [p, x] : mark_x)
                    if (!(x < first_x[p] && std::abs(mark_base[p] - first_base[p]) < 1e-9)) out.push_back({"BAD", p});
            }
            return out;
        };
        const auto m = marks("story.4");
        check("drawn: 1. 2. 3., each left of its text, on its first baseline",
              m.size() == 3 && m[0].first == "1." && m[1].first == "2." && m[2].first == "3.",
              std::to_string(m.size()) + (m.empty() ? "" : " first " + m[0].first));
        // Another style between restarts the count.
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {1, 0}, "style.1");
        check("an interruption restarts: 1. (Body) 1.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "1.");
        j.undo();
        // Bullets, letters, romans.
        core::doc::edit_styles(j, d, {"style.4"}, "Numbers", [](core::Style& s) { s.list.numbering = core::Numbering::RomanLower; });
        check("roman: i. ii. iii.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "iii.");
        core::doc::edit_styles(j, d, {"style.4"}, "Numbers", [](core::Style& s) { s.list.numbering = core::Numbering::AlphaUpper; });
        check("letters: A. B. C.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 1) == "B.");
        core::doc::edit_styles(j, d, {"style.4"}, "List", [](core::Style& s) { s.list.type = core::ListType::Bullet; });
        check("bullets: every paragraph a bullet", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "•");
        // The text's first line lines up with the rest (the marker hangs, not the words).
        {
            const auto flow = L::flow_story(d, "story.4");
            double x0 = -1, x1 = -1;
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines)
                    if (l.para == 0) { if (l.start == 0) x0 = l.x; else if (x1 < 0) x1 = l.x; }
            check("a list's first line starts with the rest", x0 >= 0 && (x1 < 0 || std::abs(x0 - x1) < 0.01));
        }
        // Off: no markers drawn.
        core::doc::edit_styles(j, d, {"style.4"}, "List", [](core::Style& s) { s.list.type = core::ListType::None; });
        check("off: no markers", marks("story.4").empty());
        j.undo(); j.undo(); j.undo(); j.undo();
        const auto o = core::project(d, "");
        bool lt = false, nm = false, hg = false;
        for (const auto& f : o.find("style.4")->fields) {
            lt = lt || (f.edit == "list_type" && f.value == "Numbers");
            nm = nm || (f.edit == "list_num" && f.value == "1. 2. 3.");
            hg = hg || (f.edit == "list_hang" && f.value == "12 pt" && f.unit == 1);
        }
        check("inspector: List, Numbers, Hang (typed)", lt && nm && hg);
        std::string err;
        core::Doc e;
        check("json: list kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                     e.as<core::Style>("style.4")->list.type == core::ListType::Number, err);
    }

    {
        std::cout << "\n-- character styles on character styles (s046) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&](const char* id) { return *d.as<core::Style>(id); };
        check("sample: Strong Emphasis on Emphasis, owning only its weight",
              S("style.11").based_on == "style.5" && S("style.11").own == std::vector<std::string>{"bold"} &&
                  S("style.11").bold && S("style.11").italic);
        check("kinds don't mix", !core::doc::can_base_on(d, "style.11", "style.1") && !core::doc::can_base_on(d, "style.1", "style.5") &&
                                     core::doc::can_base_on(d, "style.5", "style.11") == false);   // (a loop)
        core::doc::edit_styles(j, d, {"style.5"}, "Font", [](core::Style& s) { s.family = "Sans"; s.color = "swatch.3"; });
        check("edit the base: the font and colour follow", S("style.11").family == "Sans" && S("style.11").color == "swatch.3");
        core::doc::edit_styles(j, d, {"style.5"}, "Slant", [](core::Style& s) { s.italic = false; });
        check("edit the base: roman now, still bold (its own)", !S("style.11").italic && S("style.11").bold);
        j.undo(); j.undo();
        check("undo: as it was", S("style.11").family.empty() && S("style.11").italic);
        // Rebased onto None: it keeps its look.
        const auto before = S("style.11");
        check("based on None: one step, the look kept", core::doc::set_style_base(j, d, "style.11", "") && [&] {
            auto now = S("style.11");
            now.based_on = before.based_on;
            now.own = before.own;
            return now == before;
        }());
        j.undo();
        const auto o = core::project(d, "");
        bool base = false, next = false, own = false;
        for (const auto& f : o.find("style.11")->fields) {
            base = base || (f.edit == "based_on" && f.value == "Emphasis");
            next = next || f.edit == "next_style";
            own = own || (f.label == "Own settings" && f.value == "Weight");
        }
        check("inspector: Based on and Own settings for a character style (no Next style)", base && own && !next);
        bool para_only = true;   // a character style's Based on offers character styles only
        for (const auto& f : o.find("style.11")->fields)
            if (f.edit == "based_on")
                for (const auto& c : f.choices)
                    if (!c.id.empty()) para_only = para_only && d.as<core::Style>(c.id)->kind == core::StyleKind::Character;
        check("inspector: its Based on lists character styles only", para_only);
    }

    {
        std::cout << "\n-- list numbering options (s047) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto marks = [&] {
            std::string out;
            const auto& st = *d.as<core::Story>("story.4");
            for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                const auto m = core::doc::list_marker(d, st, p);
                out += (p ? " " : "") + (m.empty() ? std::string("-") : m);
            }
            return out;
        };
        check("sample: 1. 2. 3.", marks() == "1. 2. 3.", marks());
        // A Body paragraph in the middle.
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {1, 0}, "style.1");
        check("other paragraphs restart the count (as s045)", marks() == "1. - 1.", marks());
        core::doc::edit_styles(j, d, {"style.4"}, "Numbering", [](core::Style& s) { s.list.continues = true; });
        check("carries on: past the Body paragraph", marks() == "1. - 2.", marks());
        j.undo(); j.undo();
        // Five paragraphs, a second level between the first.
        const auto end2 = static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size());
        core::doc::edit_text(j, d, "story.4", {2, end2}, {2, end2}, "\nFour\nFive");
        const auto sub = core::doc::duplicate_style(j, d, "style.4");
        core::doc::edit_styles(j, d, {sub}, "Level", [](core::Style& s) {
            s.list.level = 2;
            s.list.numbering = core::Numbering::AlphaLower;
            s.left_indent = 24;
        });
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {2, 0}, sub);
        core::doc::set_para_style(j, d, "story.4", {4, 0}, {4, 0}, sub);
        check("level 2: a. b. under 1., a. again under 2.", marks() == "1. a. b. 2. a.", marks());
        core::doc::edit_styles(j, d, {"style.4"}, "Numbering", [](core::Style& s) { s.list.continues = true; });
        check("level 1 carrying on past its level-2 items", marks() == "1. a. b. 2. a.", marks());
        // Restart at, by hand.
        check("restart: one step", core::doc::set_list_restart(j, d, "story.4", {3, 0}, {3, 0}, 7));
        check("restart at 7: the next level-1 is 7.", marks() == "1. a. b. 7. a.", marks());
        core::doc::edit_text(j, d, "story.4", {3, 0}, {3, 0}, "The ");
        check("typing in it keeps the restart", d.as<core::Story>("story.4")->paras[3].restart == 7 && marks() == "1. a. b. 7. a.");
        const auto e3 = static_cast<int>(d.as<core::Story>("story.4")->paras[3].text.size());
        core::doc::edit_text(j, d, "story.4", {3, e3}, {3, e3}, "\n");
        check("Enter after it: the new paragraph counts on (8.)", d.as<core::Story>("story.4")->paras[4].restart == 0 &&
                                                                 core::doc::list_marker(d, *d.as<core::Story>("story.4"), 4) == "8.",
              marks());
        bool row = false;
        for (const auto& f : core::text_fields(d, "story.4", {3, 0}, {3, 0})) row = row || (f.edit == "list_from" && f.value == "Restart at 7");
        check("inspector (typing): Number -- Restart at 7", row);
        std::string err;
        core::Doc e;
        check("json: restart, carries on, level kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        core::doc::set_list_restart(j, d, "story.4", {3, 0}, {3, 0}, 0);
        check("counted again", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 3) == "2.", marks());
    }

    {
        std::cout << "\n-- auto-size frames (s048) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { return L::fit_frames(d); });   // as the app does
        core::load_sample(d);
        const auto bottom = [&](const char* story, const char* frame) {
            double b = -1;
            for (const auto& f : L::flow_story(d, story).frames)
                if (f.frame == frame) for (const auto& l : f.lines) b = std::max(b, l.y + l.h);
            return b;
        };
        const auto F = [&](const char* id) { return *d.as<core::Frame>(id); };
        const double h0 = F("frame.6").h;
        check("auto-size on: one step", core::doc::edit_frames(j, d, {"frame.6"}, "Auto-size", [](core::Frame& f) { f.autosize = true; }) &&
                                            j.size() == 1);
        check("the caption's height fits its text (+ its 6 pt bottom inset)",
              std::abs(F("frame.6").h - (bottom("story.2", "frame.6") + 6)) < 0.01, std::to_string(F("frame.6").h));
        const double fitted = F("frame.6").h;
        // Typing grows it -- in the same step as the typing.
        std::string more;
        for (int k = 0; k < 30; ++k) more += " The routes run east along the coast.";
        const auto end = static_cast<int>(d.as<core::Story>("story.2")->paras[0].text.size());
        core::doc::edit_text(j, d, "story.2", {0, end}, {0, end}, more);
        check("typing: the frame grows with it, one step", F("frame.6").h > fitted + 10 && j.size() == 2 &&
                                                               std::abs(F("frame.6").h - (bottom("story.2", "frame.6") + 6)) < 0.01);
        check("typing: nothing overset", !L::flow_story(d, "story.2").overset);
        j.undo();
        check("undo the typing: back to its fitted height", std::abs(F("frame.6").h - fitted) < 1e-9);
        j.undo();
        check("undo auto-size: its height as it was", std::abs(F("frame.6").h - h0) < 1e-9 && !F("frame.6").autosize);
        j.redo();
        check("redo: fitted again", std::abs(F("frame.6").h - fitted) < 1e-9);
        // A frame that threads on is left alone.
        const double h1 = F("frame.1").h;
        core::doc::edit_frames(j, d, {"frame.1"}, "Auto-size", [](core::Frame& f) { f.autosize = true; });
        check("a threaded frame: not resized", std::abs(F("frame.1").h - h1) < 1e-9);
        bool row = false;
        const auto items3 = core::frame_items(d, {"frame.1"});   // (kept: a range-for over a temporary's member dangles)
        for (const auto& f : items3.at(0).fields) row = row || (f.edit == "autosize" && f.value == "Height (not here)");
        check("inspector: says when it can't", row);
        std::string err;
        core::Doc e;
        check("json: autosize kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- story editor formatting (s049) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto looks = [&](const char* story, int p) { return core::word_looks(d, d.as<core::Story>(story)->paras[static_cast<std::size_t>(p)]); };
        const auto w0 = looks("story.4", 0);
        check("Sources: the title in Emphasis -- italic, its character style", w0.size() == 1 && w0[0].start == 0 && w0[0].end == 26 &&
                                                                               w0[0].italic && !w0[0].bold && w0[0].cstyle == "style.5");
        check("plain words: nothing", looks("story.4", 1).empty() && looks("story.1", 2).empty());
        check("a bold heading's own weight is the paragraph style's, not shown", looks("story.1", 0).empty());
        core::doc::set_local(j, d, "story.4", {1, 0}, {1, 6}, "Bold", [](core::Local& l) { l.bold = 1; });
        const auto w1 = looks("story.4", 1);
        check("local bold: its span, bold, no character style", w1.size() == 1 && w1[0].start == 0 && w1[0].end == 6 && w1[0].bold &&
                                                                 w1[0].cstyle.empty());
        core::doc::set_local(j, d, "story.4", {0, 0}, {0, 7}, "Italic", [](core::Local& l) { l.italic = 0; });
        const auto w2 = looks("story.4", 0);
        check("local roman over italic Emphasis: that part not italic, still Emphasis",
              w2.size() == 2 && !w2[0].italic && w2[0].cstyle == "style.5" && w2[1].italic, std::to_string(w2.size()));
        j.undo(); j.undo();
        check("undo: as it was", looks("story.4", 0).size() == 1 && looks("story.4", 1).empty());
    }

    {
        std::cout << "\n-- the long test book (s100) --\n";
        namespace L = composix::layout;
        // A short one (the full ~400 pages is composix_bench's): the rules
        // are the same at any length. No picture folder: missing links.
        core::Doc d;
        const auto info = L::load_big_book(d, {.pages = 20});
        const auto flow = L::flow_story(d, "story.1");
        check("big book: the long story fits (nothing overset)", !flow.overset);
        const auto body = d.pages_of(d.ids_of<core::Section>().back());
        const auto thread = L::thread_frames(d, "story.1");
        check("big book: one thread through every body page", thread.size() == body.size() && !body.empty(),
              std::to_string(thread.size()) + " frames, " + std::to_string(body.size()) + " pages");
        check("big book: about the length asked", info.pages >= 12 && info.pages <= 40, std::to_string(info.pages) + " pages");
        check("big book: the last page has text (no empty pages at the end)",
              !flow.frames.empty() && flow.frames.back().frame == thread.back() && !flow.frames.back().lines.empty());
        // Every chapter opens a right-hand page set on the chapter master,
        // with its picture in the sink; no other page is.
        const auto& paras = d.as<core::Story>("story.1")->paras;
        int heads = 0, opened = 0, bad = 0;
        for (const auto& p : paras) heads += p.style == "style.2";
        for (const auto& ft : flow.frames) {
            const auto* f = d.as<core::Frame>(ft.frame);
            const bool opens = !ft.lines.empty() && ft.lines.front().start == 0 &&
                               paras[static_cast<std::size_t>(ft.lines.front().para)].style == "style.2";
            const auto* pg = d.as<core::Page>(f->owner);
            int pics = 0;
            for (const auto& id : d.frames_on(f->owner)) pics += d.as<core::Frame>(id)->kind == core::FrameKind::Image;
            if (opens) {
                ++opened;
                if (d.side_of(f->owner) != core::Side::Right || pg->master != "master.3" || pics < 1) ++bad;
            } else if (pg->master == "master.3") {
                ++bad;
            }
        }
        check("big book: every chapter opens a right page on the chapter master, its picture over it",
              heads == info.chapters && opened == heads && bad == 0,
              std::to_string(heads) + " heads, " + std::to_string(opened) + " opened, " + std::to_string(bad) + " wrong");
        check("big book: section heads and pictures in it", info.sections > 0 && info.pictures > info.chapters);
        L::FlowCache flows;
        check("big book: its contents are current, one entry a chapter",
              L::stale_lists(d, flows).empty() && d.as<core::GenList>("list.1")->built_entries == info.chapters);
        core::Doc d2;
        (void)L::load_big_book(d2, {.pages = 20});
        check("big book: the same every time (seeded greeking)", d2.records() == d.records());
    }

    {
        std::cout << "\n-- page order, kept (s101) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto scan = [&] {   // what page_index was: a count, record by record
            bool ok = true;
            const auto ps = d.pages();
            for (std::size_t i = 0; i < ps.size(); ++i) ok = ok && d.page_index(ps[i]) == static_cast<int>(i);
            return ok && d.page_index("page.none") == -1;
        };
        check("page order: every page at its place", scan());
        const auto added = core::doc::add_pages(j, d, "page.2", 2, "master.1");
        check("page order: follows pages added in the middle", scan() && added.size() == 2 &&
                                                               d.page_index(added[0]) == 2 && d.page_index("page.3") == 4);
        core::doc::move_pages(j, d, {"page.1"}, "page.5");
        check("page order: follows a move", scan());
        j.undo(); j.undo();
        check("page order: and undo", scan() && d.page_index("page.3") == 2 && d.page_index(added[0]) == -1);
    }

    {
        std::cout << "\n-- layout off the UI thread (s102) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        const auto here = L::flow_story(d, "story.1");
        L::StoryFlow there;
        std::thread([&] { there = L::flow_story(d, "story.1"); }).join();   // its own Pango context
        bool same = here.frames.size() == there.frames.size() && here.overset == there.overset;
        for (std::size_t i = 0; same && i < here.frames.size(); ++i) {
            const auto& a = here.frames[i].lines;
            const auto& b = there.frames[i].lines;
            same = a.size() == b.size();
            for (std::size_t k = 0; same && k < a.size(); ++k)
                same = a[k].para == b[k].para && a[k].start == b[k].start && a[k].end == b[k].end && a[k].y == b[k].y;
        }
        check("a worker thread breaks the lines exactly as the UI thread", same && here.lines > 0);
        int reports = 0;
        double last = -1;
        bool rising = true;
        core::Doc b;
        (void)L::load_big_book(b, {.pages = 20, .progress = [&](double f, const std::string&) {
            ++reports;
            rising = rising && f >= last;
            last = f;
        }});
        check("big book: says how far along, rising", reports > 3 && rising && last > 0.9);
    }
}
}  // namespace cxtest
