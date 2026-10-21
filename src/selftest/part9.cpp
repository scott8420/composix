// composix_selftest, part 9 (s256: part8 had grown to 3800 lines).
#include "Selftest.hpp"
#include <plate/pango.hpp>

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
    // ── s260: character attributes ───────────────────────────────────────────
    {
        std::cout << "--- s260 character attributes ---\n";
        // A one-frame document; paragraph 0 set as given; the first line's visit.
        const auto doc_with = [](core::Doc& d, const std::string& text, const core::Style& ps, const std::vector<core::Run>& runs) {
            core::NewDoc n;
            n.pages = 1;
            n.folios = false;
            core::new_document(d, n);
            d.put("style.p", ps);
            core::Story st = *d.as<core::Story>("story.1");
            st.paras.clear();
            core::Para p{"style.p", text};
            p.runs = runs;
            st.paras.push_back(p);
            d.put("story.1", st);
        };
        // The first line, measured while its layout lives: baseline, width,
        // each run's (offset, rise, size), the print PDF's run baselines, and
        // whether underline / strikethrough are set.
        struct RunInfo { int offset = 0; double rise = 0, size = 0; };
        struct Probe { double baseline = 0, width = 0; std::vector<RunInfo> runs; std::vector<double> plate_bases; bool u = false, s = false; };
        const auto first_line = [](const core::Doc& d) {
            const auto fl = L::flow_story(d, "story.1");
            Probe out;
            bool got = false;
            L::visit_lines(d, "story.1", fl, fl.frames.at(0).frame, [&](const L::LineVisit& v) {
                if (got || v.from < 0) return;
                got = true;
                out.baseline = v.baseline;
                PangoRectangle r;
                pango_layout_line_get_extents(v.line, nullptr, &r);
                out.width = r.width / static_cast<double>(PANGO_SCALE);
                for (GSList* l = v.line->runs; l; l = l->next) {
                    auto* gi = static_cast<PangoGlyphItem*>(l->data);
                    RunInfo ri;
                    ri.offset = gi->item->offset;
#if PANGO_VERSION_CHECK(1, 50, 0)
                    ri.rise = gi->y_offset / static_cast<double>(PANGO_SCALE);
#endif
                    PangoFontDescription* fd = pango_font_describe_with_absolute_size(gi->item->analysis.font);
                    ri.size = pango_font_description_get_size(fd) / static_cast<double>(PANGO_SCALE);
                    pango_font_description_free(fd);
                    out.runs.push_back(ri);
                }
                for (const auto& pr : plate::pango::runs(v.line, 0, 100)) out.plate_bases.push_back(pr.baseline);
                if (PangoAttrList* al = pango_layout_get_attributes(v.line->layout)) {
                    PangoAttrIterator* it = pango_attr_list_get_iterator(al);
                    do {
                        out.u = out.u || pango_attr_iterator_get(it, PANGO_ATTR_UNDERLINE);
                        out.s = out.s || pango_attr_iterator_get(it, PANGO_ATTR_STRIKETHROUGH);
                    } while (pango_attr_iterator_next(it));
                    pango_attr_iterator_destroy(it);
                }
            });
            return out;
        };
        const auto width = [](const Probe& v) { return v.width; };
        core::Style base;
        base.kind = core::StyleKind::Paragraph;
        base.family = "Serif";
        base.size = 20;
        base.leading = 24;
        const std::string word = "abcdefghij";

        // Resolving: paragraph, character style over it, the words' own over both.
        core::Style ps = base;
        ps.tracking = 50;
        ps.underline = true;
        core::Style cs;
        cs.kind = core::StyleKind::Character;
        cs.tracking = 100;
        cs.position = 1;
        core::Local own;
        own.tracking = 0.0;
        own.underline = 0;
        const auto k1 = core::char_look(ps, &cs);
        const auto k2 = core::char_look(ps, &cs, &own);
        check("char_look: a character style over the paragraph's", k1.tracking == 100 && k1.position == 1 && k1.underline);
        check("char_look: the words' own over both (0 tracking, no underline)", k2.tracking == 0 && k2.position == 1 && !k2.underline);

        // Tracking widens by about (letters) x tracking/1000 em.
        core::Doc d0, d1;
        doc_with(d0, word, base, {});
        core::Style tr = base;
        tr.tracking = 100;   // 2 pt at 20 pt
        doc_with(d1, word, tr, {});
        const double w0 = width(first_line(d0)), w1 = width(first_line(d1));
        check("tracking 100: ten letters about 18 - 20 pt wider", w1 - w0 > 17.5 && w1 - w0 < 20.5,
              std::to_string(w0) + " -> " + std::to_string(w1));

        // A superscript: raised, smaller, and its line's baseline where it was.
        core::Local sup;
        sup.position = 1;
        core::Doc d2;
        doc_with(d2, word, base, {{5, 10, "", sup}});
        const auto v0 = first_line(d0), v2 = first_line(d2);
        double up = 0, small = 0;
        for (const auto& ri : v2.runs)
            if (ri.offset >= 5) { up = ri.rise; small = ri.size; }
        check("superscript: raised a third of the size, set at 58.3%", std::abs(up - 20 * core::kSuperRise) < 0.1 &&
                                                                     std::abs(small - 20 * core::kScriptSize) < 0.6,
              std::to_string(up) + " up, " + std::to_string(small) + " pt");
        check("superscript: its line's baseline unmoved", std::abs(v2.baseline - v0.baseline) < 0.01,
              std::to_string(v0.baseline) + " / " + std::to_string(v2.baseline));
        {   // The print PDF's runs: the raised one's baseline up by as much.
            double lo = 100, hi = 100;
            for (const double pb : v2.plate_bases) { lo = std::min(lo, pb); hi = std::max(hi, pb); }
            check("print PDF: the superscript's glyphs raised with it", v2.plate_bases.size() >= 2 && std::abs((hi - lo) - 20 * core::kSuperRise) < 0.1 && hi == 100,
                  std::to_string(lo) + " .. " + std::to_string(hi));
        }
        // A subscript and a baseline shift.
        core::Local sub, sh;
        sub.position = 2;
        sh.shift = -3.0;
        core::Doc d3;
        doc_with(d3, word, base, {{0, 3, "", sub}, {6, 9, "", sh}});
        double down = 0, shifted = 0;
        for (const auto& ri : first_line(d3).runs) {
            if (ri.offset < 3) down = ri.rise;
            if (ri.offset >= 6 && ri.offset < 9) shifted = ri.rise;
        }
        check("subscript lowered; a baseline shift of -3 pt lowered 3", down < 0 && std::abs(shifted + 3) < 0.05,
              std::to_string(down) + " / " + std::to_string(shifted));
        check("their line's baseline unmoved too", std::abs(first_line(d3).baseline - v0.baseline) < 0.01);

        // Caps: all caps and small caps set other glyphs than as typed.
        core::Style ac = base, sc = base;
        ac.caps = 1;
        sc.caps = 2;
        core::Doc d4, d5;
        doc_with(d4, word, ac, {});
        doc_with(d5, word, sc, {});
        const double w4 = width(first_line(d4)), w5 = width(first_line(d5));
        check("all caps: wider than as typed; small caps: between", w4 > w0 + 1 && w5 > w0 - 0.01 && w5 < w4 - 0.5,
              std::to_string(w0) + " / " + std::to_string(w4) + " / " + std::to_string(w5));

        // Underline and strikethrough reach the layout.
        core::Local ul;
        ul.underline = 1;
        ul.strike = 1;
        core::Doc d6;
        doc_with(d6, word, base, {{2, 4, "", ul}});
        const auto v6 = first_line(d6);
        const bool has_u = v6.u, has_s = v6.s;
        check("underline and strikethrough set on the words", has_u && has_s);

        // Kept in the file: a style's and the words' own.
        core::Style all = base;
        all.tracking = -20;
        all.shift = 1.5;
        all.position = 2;
        all.caps = 2;
        all.underline = all.strike = true;
        core::Local mine;
        mine.tracking = 0.0;
        mine.shift = 2.0;
        mine.position = 0;
        mine.caps = 1;
        mine.underline = 0;
        mine.strike = 1;
        core::Doc d7;
        doc_with(d7, word, all, {{1, 3, "", mine}});
        std::string err;
        core::Doc e;
        check("kept in the file", core::from_json(e, core::to_json(d7), &err) && e.records() == d7.records(), err);
        check("Local::empty knows them", !mine.empty() && core::Local{}.empty());

        // The style editor's keys.
        core::Style ed = base;
        core::doc::style_edit(d7, ed, "track", "75");
        core::doc::style_edit(d7, ed, "bshift", "-2");
        core::doc::style_edit(d7, ed, "cpos", "1");
        core::doc::style_edit(d7, ed, "ccase", "2");
        core::doc::style_edit(d7, ed, "uline", "1");
        core::doc::style_edit(d7, ed, "strike", "1");
        check("style edits: tracking, shift, position, case, underline, strike",
              ed.tracking == 75 && ed.shift == -2 && ed.position == 1 && ed.caps == 2 && ed.underline && ed.strike);
        check("their keys name the style's fields", core::doc::style_key_of_edit("track") == "tracking" &&
                                                     core::doc::style_key_of_edit("ccase") == "caps");
    }
    // ── s261: underline / strikethrough in the print PDF ─────────────────────
    {
        std::cout << "--- s261 underline and strikethrough drawn ---\n";
        namespace fs = std::filesystem;
        const auto make = [](core::Doc& d, const std::vector<core::Run>& runs) {
            core::NewDoc n;
            n.pages = 1;
            n.folios = false;
            core::new_document(d, n);
            core::Style ps;
            ps.kind = core::StyleKind::Paragraph;
            ps.family = "Serif";
            ps.size = 20;
            ps.leading = 24;
            d.put("style.p", ps);
            core::Story st = *d.as<core::Story>("story.1");
            st.paras.clear();
            core::Para p{"style.p", "abcdefghij klmnop"};
            p.runs = runs;
            st.paras.push_back(p);
            d.put("story.1", st);
        };
        core::Local ul, sk, both_up;
        ul.underline = 1;
        sk.strike = 1;
        both_up.underline = 1;
        both_up.position = 1;
        core::Doc d;
        make(d, {{0, 4, "", ul}, {5, 8, "", sk}, {11, 15, "", both_up}});
        const auto fl = L::flow_story(d, "story.1");
        std::vector<L::TextDeco> decos;
        double base = 0;
        L::visit_lines(d, "story.1", fl, fl.frames.at(0).frame, [&](const L::LineVisit& v) {
            if (v.from < 0 || !decos.empty()) return;
            base = v.baseline;
            decos = L::text_decorations(v.line, v.x, v.baseline);
        });
        const auto at = [&](int start) {
            for (const auto& t : decos) if (t.start == start) return t;
            return L::TextDeco{-1};
        };
        const auto u = at(0), k = at(5), up = at(11);
        check("three runs marked: underline, strikethrough, a raised underline", decos.size() == 3 && u.under && !u.strike &&
                                                                                  k.strike && !k.under && up.under,
              std::to_string(decos.size()));
        check("underline under the baseline, strikethrough over it, both with thickness",
              u.under_y > base && u.under_y < base + 6 && k.strike_y < base && k.strike_y > base - 15 && u.under_t > 0 && k.strike_t > 0,
              std::to_string(base) + ": " + std::to_string(u.under_y) + " / " + std::to_string(k.strike_y));
        check("a run's width: four letters wider than three", u.x1 - u.x0 > k.x1 - k.x0 && u.x0 >= 0);
        check("a superscript's underline raised with it", up.under_y < u.under_y - 4, std::to_string(up.under_y));

        // The print PDF: two more rectangles than without (the strokes), uncompressed.
        const auto rects = [](const core::Doc& doc, const std::string& name) {
            const auto dir = fs::temp_directory_path() / "composix_selftest_s261";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const auto out = (dir / name).string();
            L::PdfOptions opt;
            opt.compress = false;
            std::string err;
            if (!L::export_print_pdf(doc, out, opt, {}, nullptr, &err)) return -1;
            std::ifstream f(out, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string pdf = ss.str();
            int n = 0;
            for (std::size_t i = pdf.find(" re\n"); i != std::string::npos; i = pdf.find(" re\n", i + 1)) ++n;
            return n;
        };
        core::Doc plain;
        make(plain, {});
        core::Doc two;
        make(two, {{0, 4, "", ul}, {5, 8, "", sk}});
        const int r0 = rects(plain, "plain.pdf"), r2 = rects(two, "two.pdf");
        check("print PDF: an underline and a strikethrough drawn (2 more rectangles)", r0 >= 0 && r2 == r0 + 2,
              std::to_string(r0) + " -> " + std::to_string(r2));
    }
    // ── s262: an ODT's character attributes ──────────────────────────────────
    {
        std::cout << "--- s262 an ODT's character attributes ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:default-style style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Spaced" style:family="paragraph"><style:text-properties fo:letter-spacing="1pt" fo:text-transform="uppercase"/></style:style>
 <style:style style:name="Smallcaps" style:family="text"><style:text-properties fo:font-variant="small-caps" style:text-underline-style="solid"/></style:style>
</office:styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="T1" style:family="text"><style:text-properties style:text-position="super 58%"/></style:style>
 <style:style style:name="T2" style:family="text"><style:text-properties style:text-line-through-style="solid"/></style:style>
 <style:style style:name="T3" style:family="text"><style:text-properties style:text-position="20% 100%"/></style:style>
 <style:style style:name="T4" style:family="text"><style:text-properties style:text-position="sub 58%" fo:letter-spacing="normal"/></style:style>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Spaced">Heading</text:p>)"
            R"(<text:p text:style-name="Standard">E=mc<text:span text:style-name="T1">2</text:span> <text:span text:style-name="T2">gone</text:span> )"
            R"(<text:span text:style-name="T3">up</text:span> H<text:span text:style-name="T4">2</text:span>O <text:span text:style-name="Smallcaps">Names</text:span></text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: made", ok, err);
        if (ok) {
            const auto& st = *d.as<core::Story>(d.ids_of<core::Story>().front());
            const auto h = core::doc::para_style(d, st.paras.at(0));
            check("a paragraph style's letter spacing 1 pt at 10 pt: tracking 100; uppercase: all caps", h.tracking == 100 && h.caps == 1,
                  std::to_string(h.tracking) + " / " + std::to_string(h.caps));
            const auto& p = st.paras.at(1);
            const auto look_at = [&](int byte) {
                for (const auto& r : p.runs)
                    if (r.start <= byte && byte < r.end) return core::char_look(core::doc::para_style(d, p), d.as<core::Style>(r.style), &r.local);
                return core::char_look(core::doc::para_style(d, p));
            };
            const auto at = [&](const std::string& w) { return static_cast<int>(p.text.find(w)); };
            check("super 58%: a superscript", look_at(at("2 ")).position == 1);
            check("line-through: struck", look_at(at("gone")).strike && !look_at(at("E=")).strike);
            check("20% 100%: a baseline shift of 2 pt", std::abs(look_at(at("up")).shift - 2) < 0.01, std::to_string(look_at(at("up")).shift));
            check("sub 58%: a subscript; normal spacing: tracking 0", look_at(at("2O") ).position == 2 && look_at(at("2O")).tracking == 0);
            const auto nm = look_at(at("Names"));
            check("a text style: small caps, underlined -- a character style", nm.caps == 2 && nm.underline, std::to_string(nm.caps));
        }
    }
    // ── s263: the rows, the toggles' test, the shortcuts ─────────────────────
    {
        std::cout << "--- s263 type style rows and shortcuts ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto o = core::project(d, "");
        const auto rows_of = [&](core::StyleKind kind) {
            std::map<std::string, std::pair<std::string, std::string>> m;   // edit -> (group, value)
            for (const auto* it : o.in(core::Segment::Styles))
                if (const auto* st = d.as<core::Style>(it->id); st && st->kind == kind) {
                    for (const auto& f : it->fields) m[f.edit] = {f.group, f.value};
                    break;
                }
            return m;
        };
        const auto pr = rows_of(core::StyleKind::Paragraph), cr = rows_of(core::StyleKind::Character);
        bool grouped = true;
        for (const char* k : {"track", "bshift", "cpos", "ccase", "uline", "strike"})
            grouped = grouped && pr.count(k) && cr.count(k) && pr.at(k).first == "Character" && cr.at(k).first == "Character";
        check("style rows: Tracking .. Strikethrough, a Character group, both kinds", grouped);
        check("a paragraph style says None / Normal / As typed / Off; a character style Inherit",
              pr.at("track").second == "None" && pr.at("cpos").second == "Normal" && pr.at("ccase").second == "As typed" &&
                  pr.at("uline").second == "Off" && cr.at("track").second == "Inherit" && cr.at("uline").second == "Inherit");
        check("the labels name the steps", core::doc::style_edit_label("track") == "Tracking" && core::doc::style_edit_label("strike") == "Strikethrough");

        // The words' own rows, and a toggle's test.
        core::Story st = *d.as<core::Story>("story.1");
        const int p = 3;
        core::Local mine;
        mine.underline = 1;
        mine.tracking = 25.0;
        const int n = static_cast<int>(st.paras[p].text.size());
        core::doc::set_local(j, d, "story.1", {p, 0}, {p, n}, "Underline", [&](core::Local& l) { l.underline = 1; l.tracking = 25.0; });
        const auto f = core::text_fields(d, "story.1", {p, 0}, {p, 2});
        std::map<std::string, std::string> v;
        for (const auto& x : f) v[x.edit] = x.value;
        check("the words' rows: tracking 25, underline On, the rest the style's",
              v["ltrack"] == "25 /1000 em" && v["luline"] == "On" && v["lpos"] == "Style's" && v["lstrike"] == "Style's", v["ltrack"]);
        const auto& sp = *d.as<core::Story>("story.1");
        const auto ul = [](const core::CharLook& k) { return k.underline; };
        check("all_look: the paragraph underlined throughout; the next not", core::all_look(d, sp, {p, 0}, {p, n}, ul) &&
                                                                             !core::all_look(d, sp, {p + 1, 0}, {p + 1, 2}, ul));
        // Shortcuts: wired, and no chord claimed twice.
        std::set<std::string> acts;
        for (const auto& sc : core::shortcut_registry()) acts.insert(sc.action);
        bool all = true;
        for (const char* a : {"win.underline", "win.strikethrough", "win.superscript", "win.subscript", "win.all-caps", "win.small-caps"})
            all = all && acts.count(a);
        check("shortcuts: the six toggles listed", all);
        const auto col = core::find_accel_collisions();
        check("no chord claimed twice", col.empty(), col.empty() ? "" : col.front());
    }
}
}  // namespace cxtest
