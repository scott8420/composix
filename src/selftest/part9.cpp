// composix_selftest, part 9 (s256: part8 had grown to 3800 lines).
#include "Selftest.hpp"

namespace cxtest {
namespace {
namespace L = composix::layout;

// A new two-page facing document with threaded margin frames, reflow on, its
// story long enough to need `repeat` sentences' worth of pages, reflowed.
void long_doc(core::Doc& d, core::DocJournal& j, int repeat) {
    core::NewDoc n;
    n.pages = 2;
    n.text_frames = true;
    n.facing = true;
    core::new_document(d, n);
    std::string words;
    for (int k = 0; k < repeat; ++k) words += "The routes ran east along the coast, and the ships came in with the morning. ";
    core::Story st = *d.as<core::Story>("story.1");
    st.paras[0].text = words;
    j.run("Type", {"story.1"}, [&] { d.put("story.1", st); });
    L::reflow_pages(j, d, "story.1");
}

void set_text(core::Doc& d, core::DocJournal& j, const std::string& text) {
    core::Story st = *d.as<core::Story>("story.1");
    st.paras.resize(1);
    st.paras[0].text = text;
    j.run("Type", {"story.1"}, [&] { d.put("story.1", st); });
}
}  // namespace

void part9() {
    // ── s256: text reflow takes away the pages the text no longer reaches ────
    {
        std::cout << "--- s256 reflow removes empty pages ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        long_doc(d, j, 400);
        const auto grown = d.pages().size();
        check("grown by reflow", grown > 4 && !L::flow_story(d, "story.1").overset, std::to_string(grown) + " pages");
        check("nothing empty yet", L::empty_reflow_pages(d, "story.1", L::flow_story(d, "story.1")).empty());
        check("off by default (adds pages only)", d.settings().reflow && !d.settings().reflow_trim);

        check("set_reflow_mode 2: adds and removes, one step",
              core::doc::set_reflow_mode(j, d, 2) && d.settings().reflow && d.settings().reflow_trim &&
                  j.undo_label() == "Text reflow: adds and removes pages");
        check("set_reflow_mode 2 again: no step", !core::doc::set_reflow_mode(j, d, 2));

        // Cut the text to a page and a bit: the frames past it are empty.
        std::string some;
        for (int k = 0; k < 40; ++k) some += "The routes ran east along the coast, and the ships came in with the morning. ";
        set_text(d, j, some);
        const auto fl = L::flow_story(d, "story.1");
        int used = 0;
        for (const auto& ft : fl.frames) used += !ft.lines.empty();
        const auto empty = L::empty_reflow_pages(d, "story.1", fl);
        check("the empty pages: every page past the text", used >= 1 && static_cast<int>(empty.size()) == static_cast<int>(grown) - used,
              std::to_string(used) + " used, " + std::to_string(empty.size()) + " empty of " + std::to_string(grown));

        const auto steps = j.size();
        const int removed = L::trim_reflow_pages(j, d, "story.1");
        const auto chain = L::thread_frames(d, "story.1");
        const auto after = L::flow_story(d, "story.1");
        bool all_used = true;
        for (const auto& ft : after.frames) all_used = all_used && !ft.lines.empty();
        check("trim: the empty pages gone, one step \"Text reflow\"",
              removed == static_cast<int>(empty.size()) && static_cast<int>(d.pages().size()) == used && j.size() == steps + 1 &&
                  j.undo_label() == "Text reflow",
              std::to_string(removed) + " removed, " + std::to_string(d.pages().size()) + " pages");
        check("trim: every frame left holds text, the last threads on to nothing, not overset",
              all_used && !after.overset && static_cast<int>(chain.size()) == used && d.as<core::Frame>(chain.back())->next.empty());
        check("trim: nothing more to take", L::trim_reflow_pages(j, d, "story.1") == 0);
        bool no_orphans = true;
        for (const auto& f : d.ids_of<core::Frame>()) {
            const auto& o = d.as<core::Frame>(f)->owner;
            no_orphans = no_orphans && (d.as<core::Page>(o) || d.as<core::Master>(o));
        }
        check("trim: no frame left without its page", no_orphans);
        j.undo();
        check("undo: the pages back, threaded as before", d.pages().size() == grown &&
                                                         L::thread_frames(d, "story.1").size() == grown);
        j.redo();
        check("redo: taken again", static_cast<int>(d.pages().size()) == used);

        // Grow again, then shrink: a page holding something else stays -- and
        // so do the pages before it.
        set_text(d, j, std::string(some).append(some).append(some).append(some));
        L::reflow_pages(j, d, "story.1");
        const auto chain2 = L::thread_frames(d, "story.1");
        const auto n2 = d.pages().size();
        check("grown again", n2 > static_cast<std::size_t>(used) + 1, std::to_string(n2));
        const auto mid_page = d.as<core::Frame>(chain2[chain2.size() - 2])->owner;   // the next-to-last page
        core::doc::add_frame(j, d, mid_page, core::FrameKind::Shape, {20, 20, 40, 40});
        set_text(d, j, "A line.");
        const auto e2 = L::empty_reflow_pages(d, "story.1", L::flow_story(d, "story.1"));
        check("a page with a shape on it: it and the pages before it kept", e2.size() == 1 &&
                                                                         e2[0] == d.as<core::Frame>(chain2.back())->owner,
              std::to_string(e2.size()));
        check("trim: the one page after it", L::trim_reflow_pages(j, d, "story.1") == 1 && d.pages().size() == n2 - 1);

        // The first frame is never taken, even empty.
        set_text(d, j, "");
        core::doc::remove_pages(j, d, [&] {
            std::vector<core::Doc::Id> v;
            for (const auto& p : d.pages())
                if (p != d.pages().front()) v.push_back(p);
            return v;
        }());
        check("an empty story on one page: nothing taken", L::trim_reflow_pages(j, d, "story.1") == 0 && d.pages().size() == 1);

        // Off and on in the file and the Document rows.
        std::string err;
        core::Doc e;
        check("adds and removes: kept in the file", core::from_json(e, core::to_json(d), &err) && e.settings().reflow_trim, err);
        const auto row = [&] {
            const auto o = core::project(d, "");   // kept: items() of a temporary would dangle (GCC 13)
            for (const auto& it : o.items())
                for (const auto& f : it.fields)
                    if (f.edit == "dreflow") return f.value + "|" + std::to_string(f.choices.size());
            return std::string{};
        };
        check("Document: Text reflow -- Adds and removes pages (three choices)", row() == "Adds and removes pages|3", row());
        core::doc::set_reflow_mode(j, d, 1);
        check("mode 1: adds pages only", d.settings().reflow && !d.settings().reflow_trim && row() == "Adds pages|3" &&
                                         j.undo_label() == "Text reflow: adds pages");
        core::doc::set_reflow_mode(j, d, 0);
        check("mode 0: off", !d.settings().reflow && !d.settings().reflow_trim && row() == "Off|3");
    }
    {
        std::cout << "--- s256 a page's guides go with it ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::NewDoc n;
        n.pages = 3;
        n.text_frames = false;
        core::new_document(d, n);
        const auto p2 = d.pages()[1];
        const auto g = core::doc::add_guide(j, d, p2, true, 100);
        const auto g1 = core::doc::add_guide(j, d, d.pages()[0], false, 100);
        check("guides placed", d.as<core::Guide>(g) && d.as<core::Guide>(g1));
        check("delete page 2: its guide gone, page 1's kept", core::doc::remove_pages(j, d, {p2}) && !d.as<core::Guide>(g) &&
                                                             d.as<core::Guide>(g1));
        j.undo();
        check("undo: the guide back", d.as<core::Guide>(g) != nullptr);
    }
    // ── s257: keep with next N lines ─────────────────────────────────────────
    {
        std::cout << "--- s257 keep with next N lines ---\n";
        // k one-line paragraphs, a one-line heading keeping with `n` lines, a
        // long body paragraph: as k grows the heading meets every line of a
        // column's foot.
        const auto make = [](core::Doc& d, int k, int n) {
            core::NewDoc nd;
            nd.pages = 3;
            nd.folios = false;
            core::new_document(d, nd);
            core::Style base;
            base.kind = core::StyleKind::Paragraph;
            base.family = "Serif";
            base.size = 11;
            base.leading = 14;
            base.keep_lines = 0;
            core::Style h = base;
            h.name = "H";
            h.bold = true;
            h.keep_next = n;
            core::Style b = base;
            b.name = "B";
            d.put("style.h", h);
            d.put("style.b", b);
            core::Story st = *d.as<core::Story>("story.1");
            st.paras.clear();
            for (int i = 0; i < k; ++i) st.paras.push_back({"style.b", "Filler."});
            st.paras.push_back({"style.h", "Heading"});
            std::string w;
            for (int i = 0; i < 160; ++i) w += "harbor lanterns ";
            st.paras.push_back({"style.b", w + "end."});
            d.put("story.1", st);
        };
        // At each column break: the heading's column holds fewer of the body's
        // lines than min(n, its lines) -> a breach. `moved`: the heading heads
        // a column (it went on). `lost`: the body's lines don't add up.
        struct Tally { int breaches = 0, moved = 0, cols = 0; bool lost = false; };
        const auto tally = [](const core::Doc& d, int k, int n, Tally& t) {
            const auto fl = L::flow_story(d, "story.1");
            std::vector<std::vector<L::Line>> cols;
            for (const auto& f : fl.frames) {
                int last = -1;
                for (const auto& l : f.lines) {
                    if (l.col != last || cols.empty()) { cols.emplace_back(); last = l.col; }
                    cols.back().push_back(l);
                }
            }
            int body_total = 0;
            for (const auto& c : cols) for (const auto& l : c) body_total += l.para == k + 1 && l.cap <= 0;
            for (const auto& c : cols) {
                if (c.empty()) continue;
                ++t.cols;
                bool head = false;
                int body = 0;
                for (const auto& l : c) { head = head || l.para == k; body += l.para == k + 1 && l.cap <= 0; }
                if (!head) continue;
                if (c.front().para == k && k > 0) ++t.moved;
                if (body < std::min(n, body_total)) ++t.breaches;
            }
            t.lost = t.lost || fl.overset || body_total < 10;
        };
        Tally one, three;
        for (int k = 0; k <= 50; ++k) {
            core::Doc d1, d3;
            make(d1, k, 1);
            make(d3, k, 3);
            tally(d1, k, 3, one);   // measured against 3: keep 1 lets 1 - 2 lines be
            tally(d3, k, 3, three);
        }
        check("keep with next 1: the sweep meets a heading over 1 - 2 lines at a foot", one.breaches > 0,
              std::to_string(one.breaches) + " in 51 layouts");
        check("keep with next 3: none -- the heading goes on with them", three.breaches == 0 && three.moved > one.moved && !three.lost,
              std::to_string(three.breaches) + " breaches, moved " + std::to_string(three.moved) + " (keep 1: " + std::to_string(one.moved) + ")");

        // The style row, the paragraph's own, the file.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        make(d, 3, 3);
        const auto o = core::project(d, "");
        std::string row;
        std::size_t nch = 0;
        for (const auto& f : o.find("style.h")->fields)
            if (f.edit == "keep_next") { row = f.value; nch = f.choices.size(); }
        check("style row: Keep with next -- 3 lines (Off, 1 - 5)", row == "3 lines" && nch == 6, row);
        check("set from the row: 5 lines, clamped", core::doc::edit_styles(j, d, {"style.h"}, "Keep with next", [](core::Style& s) {
                  s.keep_next = 5; }) && d.as<core::Style>("style.h")->keep_next == 5);
        core::doc::set_para_local(j, d, "story.1", {4, 0}, {4, 0}, "Keep with next", [](core::ParaLocal& lo) { lo.keep_next = 2; });
        const auto tf = core::text_fields(d, "story.1", {4, 0}, {4, 0});
        std::string prow;
        for (const auto& x : tf) if (x.edit == "pkeep_next") prow = x.value;
        check("a paragraph's own: 2 lines", prow == "2 lines" && core::doc::para_style(d, d.as<core::Story>("story.1")->paras[4]).keep_next == 2, prow);
        std::string err;
        core::Doc e;
        check("lines kept in the file", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>("style.h")->keep_next == 5 &&
                                            e.as<core::Story>("story.1")->paras[4].over.keep_next == 2, err);
        // A file from before s257: keep_next true / false.
        auto js = core::to_json(d);
        const auto swap = [&](const std::string& from, const std::string& to) {
            const auto at = js.find(from);
            if (at != std::string::npos) js.replace(at, from.size(), to);
            return at != std::string::npos;
        };
        const bool swapped = swap("\"keep_next\": 5", "\"keep_next\": true") && swap("\"keep_next\": 2", "\"keep_next\": false");
        core::Doc g;
        check("an older file: true reads as 1 line, false as off", swapped && core::from_json(g, js, &err) &&
                                                                  g.as<core::Style>("style.h")->keep_next == 1 &&
                                                                  g.as<core::Story>("story.1")->paras[4].over.keep_next == 0,
              err);
    }
    {
        std::cout << "--- s257 an ODT's keeps ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:default-style style:family="paragraph"><style:paragraph-properties fo:orphans="2" fo:widows="2"/><style:text-properties fo:font-size="11pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Loose" style:family="paragraph"><style:paragraph-properties fo:orphans="0" fo:widows="0"/></style:style>
 <style:style style:name="Heading_20_2" style:display-name="Heading 2" style:family="paragraph"><style:paragraph-properties fo:keep-with-next="always" fo:widows="5"/></style:style>
</office:styles></office:document-styles>)";
        const std::string body = R"(<text:h text:style-name="Heading_20_2">A head</text:h><text:p text:style-name="Standard">Body.</text:p>)"
                                 R"(<text:p text:style-name="P1">Kept with the next.</text:p><text:p text:style-name="Loose">Loose.</text:p>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="P1" style:family="paragraph" style:parent-style-name="Standard"><style:paragraph-properties fo:keep-with-next="always"/></style:style>
</office:automatic-styles><office:body><office:text>)" + body + "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: made", ok, err);
        if (ok) {
            const auto& st = *d.as<core::Story>(d.ids_of<core::Story>().front());
            const auto sty = [&](int i) { return core::doc::para_style(d, st.paras.at(static_cast<std::size_t>(i))); };
            check("keep-with-next always: 1 line; widows 5: keep lines 3", st.paras.size() == 4 && sty(0).keep_next == 1 && sty(0).keep_lines == 3,
                  std::to_string(sty(0).keep_next) + " / " + std::to_string(sty(0).keep_lines));
            check("orphans / widows 2 (the default): keep lines 2; no keep with next", sty(1).keep_next == 0 && sty(1).keep_lines == 2);
            check("a paragraph's own keep-with-next", st.paras[2].over.keep_next == 1 && sty(2).keep_next == 1);
            check("orphans / widows 0: off", sty(3).keep_lines == 0);
        }
    }
    // ── s258: a right- / left-hand start past the thread's end ───────────────
    {
        std::cout << "--- s258 a blank page for a right- / left-hand start ---\n";
        for (const auto start : {core::ParaStart::NextRightPage, core::ParaStart::NextLeftPage}) {
            const bool right = start == core::ParaStart::NextRightPage;
            const std::string what = right ? "right" : "left";
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::NewDoc n;
            n.pages = 1;
            n.folios = false;
            core::new_document(d, n);
            // "right": A (~13 lines) fits page 1, a right-hand page -- B skips page 2.
            // "left":  A (~53 lines) runs onto page 2, a left-hand page -- B skips page 3.
            std::string words;
            for (int k = 0; k < (right ? 10 : 40); ++k) words += "The routes ran east along the coast, and the ships came in with the morning. ";
            core::Story st = *d.as<core::Story>("story.1");
            const std::string sty0 = st.paras.at(0).style;
            st.paras.clear();
            st.paras.push_back({sty0, words});
            core::Para b = st.paras[0];
            b.text = "Chapter Two begins here.";
            b.brk = start;
            st.paras.push_back(b);
            j.run("Type", {"story.1"}, [&] { d.put("story.1", st); });
            check(what + ": overset before reflow (no " + what + "-hand frame after A)", L::flow_story(d, "story.1").overset);
            L::reflow_pages(j, d, "story.1");
            const auto fl = L::flow_story(d, "story.1");
            // Where A ends and B starts.
            std::string a_page, b_page;
            std::vector<std::string> empty_pages;
            for (const auto& ft : fl.frames) {
                const auto pg = d.as<core::Frame>(ft.frame)->owner;
                if (ft.lines.empty()) empty_pages.push_back(pg);
                for (const auto& l : ft.lines) {
                    if (l.para == 0) a_page = pg;
                    if (l.para == 1 && b_page.empty()) b_page = pg;
                }
            }
            const auto idx = [&](const std::string& p) { return d.page_index(p); };
            check(what + ": B on a " + what + "-hand page, a blank one between, not overset",
                  !fl.overset && !b_page.empty() && d.side_of(b_page) == (right ? core::Side::Right : core::Side::Left) &&
                      empty_pages.size() == 1 && idx(empty_pages[0]) == idx(a_page) + 1 && idx(b_page) == idx(a_page) + 2,
                  "A on " + std::to_string(idx(a_page)) + ", B on " + std::to_string(b_page.empty() ? -1 : idx(b_page)) + ", " +
                      std::to_string(empty_pages.size()) + " blank");
            // With removal on, the blank page stays (it isn't at the end).
            core::doc::set_reflow_mode(j, d, 2);
            check(what + ": the blank page isn't taken away", L::trim_reflow_pages(j, d, "story.1") == 0);
            // B gone: the blank page and B's page are the empty end -- taken.
            const auto pages_with_b = d.pages().size();
            core::Story s2 = *d.as<core::Story>("story.1");
            s2.paras.pop_back();
            j.run("Delete", {"story.1"}, [&] { d.put("story.1", s2); });
            check(what + ": B deleted -- its page and the blank one taken", L::trim_reflow_pages(j, d, "story.1") == 2 &&
                                                                       d.pages().size() == pages_with_b - 2);
        }
    }
    // ── s258: the lists settled apart (a worker's half) ──────────────────────
    {
        std::cout << "--- s258 lists settled on a copy, on a thread ---\n";
        core::Doc a, b;
        core::load_sample(a);
        core::load_sample(b);
        const auto lists = a.ids_of<core::GenList>();
        check("the sample has lists", !lists.empty(), std::to_string(lists.size()));
        // One by one, as before.
        core::DocJournal ja{a};
        a.set_door([&ja] { return ja.open(); });
        for (const auto& l : lists) L::build_list(ja, a, l);
        // Settled on a records-only copy, on a thread, then written.
        core::Doc scratch;
        {
            core::Doc::Loading load(scratch);
            for (const auto& [id, r] : b.records()) scratch.put(id, r);
        }
        std::vector<double> fr;
        std::vector<std::string> lines;
        std::vector<L::ListBuild> built;
        std::thread t([&] { built = L::settle_lists(scratch, lists, [&](double f, const std::string& line) { fr.push_back(f); lines.push_back(line); }); });
        t.join();
        {
            core::Doc c;
            core::load_sample(c);
            check("settling leaves the document itself alone", b.records() == c.records() && scratch.records() != c.records());
        }
        core::DocJournal jb{b};
        b.set_door([&jb] { return jb.open(); });
        const auto steps = jb.size();
        for (const auto& lb : built) core::doc::build_list(jb, b, lb.list, lb.entries);
        bool same = built.size() == lists.size();
        for (const auto& l : lists) {
            const auto* la = a.as<core::GenList>(l);
            const auto* lb = b.as<core::GenList>(l);
            same = same && la && lb && la->built_entries == lb->built_entries && !la->story.empty() &&
                   a.as<core::Story>(la->story)->paras == b.as<core::Story>(lb->story)->paras;
        }
        check("the same lists as built one by one, each its own step", same && jb.size() == steps + lists.size());
        bool rising = !fr.empty() && fr.back() == 1.0;
        for (std::size_t i = 1; i < fr.size(); ++i) rising = rising && fr[i] >= fr[i - 1];
        check("progress: rising to 1, said in words", rising && !lines.empty() && lines.front().rfind("Gathering", 0) == 0 &&
                                                      lines.back() == "Writing\u2026",
              std::to_string(fr.size()) + " reports");
        check("a list gone meanwhile: left out", L::settle_lists(scratch, {"list.none"}).empty());
    }
}
}  // namespace cxtest
