// composix_selftest, part 9 (s256: part8 had grown to 3800 lines).
#include "Selftest.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
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
    // ── s264: OpenType features ──────────────────────────────────────────────
    {
        std::cout << "--- s264 OpenType features ---\n";
        core::OtFeatures f;
        check("none set: no settings", core::ot_settings(f).empty() && !f.any());
        f.liga = 1;
        f.figures = 2;
        f.sset = 3;
        check("ligatures off, oldstyle figures, set 3", core::ot_settings(f) == "liga=0,clig=0,lnum=0,onum=1,ss03=1", core::ot_settings(f));
        core::OtFeatures g;
        g.frac = true;
        check("over a paragraph's: what it set is set back", core::ot_settings(g, f) == "liga=1,clig=1,lnum=0,onum=0,frac=1,ss03=0",
              core::ot_settings(g, f));
        const auto m = core::ot_over(f, g);
        check("a character style's over the paragraph's: both", m.liga == 1 && m.figures == 2 && m.frac && m.sset == 3);
        core::OtFeatures e;
        core::ot_edit(e, "ot_figsp", "2");
        core::ot_edit(e, "ot_ordn", "1");
        core::ot_edit(e, "ot_sset", "99");
        check("edits: tabular, ordinals, a set clamped to 20", e.fig_space == 2 && e.ordn && e.sset == 20);

        // Ligatures off: a glyph for every letter (whatever the font joins).
        const auto glyphs = [](const core::Doc& d, bool& feats) {
            const auto fl = L::flow_story(d, "story.1");
            int n = -1;
            feats = false;
            L::visit_lines(d, "story.1", fl, fl.frames.at(0).frame, [&](const L::LineVisit& v) {
                if (n >= 0 || v.from < 0) return;
                n = 0;
                for (GSList* l = v.line->runs; l; l = l->next) n += static_cast<PangoGlyphItem*>(l->data)->glyphs->num_glyphs;
                if (PangoAttrList* al = pango_layout_get_attributes(v.line->layout)) {
                    PangoAttrIterator* it = pango_attr_list_get_iterator(al);
                    do feats = feats || pango_attr_iterator_get(it, PANGO_ATTR_FONT_FEATURES);
                    while (pango_attr_iterator_next(it));
                    pango_attr_iterator_destroy(it);
                }
            });
            return n;
        };
        const auto make = [](core::Doc& d, const core::OtFeatures& ot) {
            core::NewDoc n;
            n.pages = 1;
            n.folios = false;
            core::new_document(d, n);
            core::Style ps;
            ps.kind = core::StyleKind::Paragraph;
            ps.family = "Serif";
            ps.size = 20;
            ps.leading = 24;
            ps.ot = ot;
            d.put("style.p", ps);
            core::Story st = *d.as<core::Story>("story.1");
            st.paras.clear();
            st.paras.push_back({"style.p", "officeffifl"});
            d.put("story.1", st);
        };
        core::Doc d0, d1;
        make(d0, {});
        core::OtFeatures off;
        off.liga = 1;
        make(d1, off);
        bool f0 = false, f1 = false;
        const int g0 = glyphs(d0, f0), g1 = glyphs(d1, f1);
        check("ligatures off: one glyph a letter; the features reach the layout", g1 == 11 && g0 <= g1 && f1 && !f0,
              std::to_string(g0) + " -> " + std::to_string(g1));
        std::string err;
        core::Doc back;
        check("kept in the file", core::from_json(back, core::to_json(d1), &err) && back.records() == d1.records(), err);
        core::Local own;
        own.ot = off;
        core::Doc d2;
        make(d2, {});
        core::Story st = *d2.as<core::Story>("story.1");
        st.paras[0].runs.push_back({0, 6, "", own});
        d2.put("story.1", st);
        bool f2 = false;
        check("the words' own: the first word's ligatures off", glyphs(d2, f2) >= g0 && f2);
        check("the words' own kept in the file", core::from_json(back, core::to_json(d2), &err) && back.records() == d2.records(), err);
        check("a style edit names the field", core::doc::style_key_of_edit("ot_frac") == "ot" && core::doc::style_edit_label("ot_fig") == "Figures");
    }
    // ── s265: manual kerning, the spacing keys ───────────────────────────────
    {
        std::cout << "--- s265 kerning ---\n";
        const auto make = [](core::Doc& d, const std::vector<core::Run>& runs, double tracking = 0) {
            core::NewDoc n;
            n.pages = 1;
            n.folios = false;
            core::new_document(d, n);
            core::Style ps;
            ps.kind = core::StyleKind::Paragraph;
            ps.family = "Serif";
            ps.size = 20;
            ps.leading = 24;
            ps.tracking = tracking;
            d.put("style.p", ps);
            core::Story st = *d.as<core::Story>("story.1");
            st.paras.clear();
            core::Para p{"style.p", "abcdef"};
            p.runs = runs;
            st.paras.push_back(p);
            d.put("story.1", st);
        };
        // Each glyph's ink left on the first line (pen + its offset), points.
        const auto inks = [](const core::Doc& d) {
            const auto fl = L::flow_story(d, "story.1");
            std::vector<double> out;
            L::visit_lines(d, "story.1", fl, fl.frames.at(0).frame, [&](const L::LineVisit& v) {
                if (!out.empty() || v.from < 0) return;
                double pen = 0;
                for (GSList* l = v.line->runs; l; l = l->next) {
                    auto* gi = static_cast<PangoGlyphItem*>(l->data);
                    for (int i = 0; i < gi->glyphs->num_glyphs; ++i) {
                        out.push_back((pen + gi->glyphs->glyphs[i].geometry.x_offset) / PANGO_SCALE);
                        pen += gi->glyphs->glyphs[i].geometry.width;
                    }
                }
            });
            return out;
        };
        core::Local k;
        k.kern = 500.0;   // 10 pt at 20 pt
        core::Doc d0, d1, d2, d3, d4;
        make(d0, {});
        make(d1, {{2, 3, "", k}});
        make(d2, {{0, 1, "", k}});
        const auto i0 = inks(d0), i1 = inks(d1), i2 = inks(d2);
        const auto near = [](double a, double b) { return std::abs(a - b) < 0.05; };
        check("a kern after c: c stays, d on moves 10 pt", i0.size() == 6 && i1.size() == 6 && near(i1[2], i0[2]) && near(i1[3], i0[3] + 10) &&
                                                          near(i1[5], i0[5] + 10) && near(i1[1], i0[1]),
              i0.size() == 6 && i1.size() == 6 ? std::to_string(i1[2] - i0[2]) + " / " + std::to_string(i1[3] - i0[3]) : "?");
        check("a kern on the line's first letter: it stays, the rest move", i2.size() == 6 && near(i2[0], i0[0]) && near(i2[1], i0[1] + 10),
              i2.size() == 6 ? std::to_string(i2[0] - i0[0]) + " / " + std::to_string(i2[1] - i0[1]) : "?");
        make(d3, {}, 50);
        make(d4, {{2, 3, "", k}}, 50);   // tracking 1 pt a letter under it
        const auto i3 = inks(d3), i4 = inks(d4);
        check("with tracking: the kern on top of it, c still in place", i3.size() == 6 && i4.size() == 6 && near(i4[2], i3[2]) && near(i4[3], i3[3] + 10),
              i3.size() == 6 && i4.size() == 6 ? std::to_string(i4[2] - i3[2]) + " / " + std::to_string(i4[3] - i3[3]) : "?");

        // The keys' verbs.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        make(d, {});
        const auto steps = j.size();
        core::doc::nudge_spacing(j, d, "story.1", {0, 3}, {0, 3}, 20);
        core::doc::nudge_spacing(j, d, "story.1", {0, 3}, {0, 3}, 20);
        const auto& p = d.as<core::Story>("story.1")->paras[0];
        check("Alt+Right twice at the caret after c: kern 40 on c, one step", core::local_at(p, 3).kern.value_or(0) == 40 && j.size() == steps + 1 &&
                                                                            j.undo_label() == "Kerning");
        core::doc::nudge_spacing(j, d, "story.1", {0, 3}, {0, 3}, -40);
        check("back to 0: the kern gone", !core::local_at(d.as<core::Story>("story.1")->paras[0], 3).kern);
        check("at a paragraph's start: nothing", !core::doc::nudge_spacing(j, d, "story.1", {0, 0}, {0, 0}, 20));
        core::doc::nudge_spacing(j, d, "story.1", {0, 1}, {0, 4}, -20);
        check("a selection: tracking -20 on it", core::local_at(d.as<core::Story>("story.1")->paras[0], 2).tracking.value_or(0) == -20);
        core::doc::nudge_shift(j, d, "story.1", {0, 2}, {0, 2}, 1);
        core::doc::nudge_shift(j, d, "story.1", {0, 2}, {0, 2}, 1);
        check("Alt+Shift+Up twice on the caret's word: shift 2 pt", core::local_at(d.as<core::Story>("story.1")->paras[0], 1).shift.value_or(0) == 2 &&
                                                                  j.undo_label() == "Baseline shift");
        // Typing after a kerned letter: the new text takes no kern.
        core::doc::nudge_spacing(j, d, "story.1", {0, 3}, {0, 3}, 100);
        core::doc::edit_text(j, d, "story.1", {0, 3}, {0, 3}, "x", "Type");
        const auto& q = d.as<core::Story>("story.1")->paras[0];
        check("typed after it: the kern stays on c, x has none", core::local_at(q, 3).kern.value_or(0) == 100 && !core::local_at(q, 4).kern,
              q.text);
        std::string err;
        core::Doc e;
        check("kept in the file", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }
    // ── s266: an ODT's font features ─────────────────────────────────────────
    {
        std::cout << "--- s266 an ODT's font features ---\n";
        std::string fam = "Linux Libertine G:onum&liga=0";
        check("split_features: the family and its features", core::odt::split_features(fam) == "onum&liga=0" && fam == "Linux Libertine G");
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f" xmlns:svg="g")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:font-face-decls>
 <style:font-face style:name="Libertine" svg:font-family="'Linux Libertine G:onum&amp;liga=0&amp;ss02'"/>
</office:font-face-decls><office:styles>
 <style:default-style style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties style:font-name="Libertine"/></style:style>
</office:styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="T1" style:family="text"><style:text-properties fo:font-family="'Linux Libertine G:smcp&amp;tnum'"/></style:style>
 <style:style style:name="T2" style:family="text"><style:text-properties style:letter-kerning="false"/></style:style>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">In 1923 the <text:span text:style-name="T1">NATO</text:span> line, <text:span text:style-name="T2">AV</text:span>.</text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: made", ok, err);
        if (ok) {
            const auto& st = *d.as<core::Story>(d.ids_of<core::Story>().front());
            const auto& p = st.paras.at(0);
            const auto ps = core::doc::para_style(d, p);
            check("the family without its features; oldstyle figures, ligatures off, set 2",
                  ps.family == "Linux Libertine G" && ps.ot.figures == 2 && ps.ot.liga == 1 && ps.ot.sset == 2, ps.family);
            const auto look = [&](const std::string& w) {
                const int b = static_cast<int>(p.text.find(w));
                const auto l = core::local_at(p, b + 1);
                return core::char_look(ps, d.as<core::Style>(core::char_style_at(p, b + 1)), &l);
            };
            check("a span's features: small caps, tabular figures", look("NATO").caps == 2 && look("NATO").ot.fig_space == 2);
            check("letter-kerning false: kerning off", look("AV").ot.kern == 1);
        }
    }
    // ── s267: the OpenType rows; the pair's kerning row ──────────────────────
    {
        std::cout << "--- s267 OpenType rows ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto ps = *d.as<core::Style>("style.1");
        ps.ot.figures = 2;
        ps.ot.sset = 4;
        d.put("style.1", ps);
        const auto o = core::project(d, "");
        std::map<std::string, std::pair<std::string, std::string>> rows;
        std::size_t sets = 0;
        for (const auto& f : o.find("style.1")->fields) {
            rows[f.edit] = {f.group, f.value};
            if (f.edit == "ot_sset") sets = f.choices.size();
        }
        bool all = true;
        for (const char* k : {"ot_liga", "ot_kern", "ot_fig", "ot_figsp", "ot_frac", "ot_ordn", "ot_dlig", "ot_swsh", "ot_sset"})
            all = all && rows.count(k) && rows.at(k).first == "OpenType";
        check("a style's OpenType group: nine rows", all);
        check("their values: Oldstyle, Set 4 (of None + 20), Ligatures On",
              rows["ot_fig"].second == "Oldstyle" && rows["ot_sset"].second == "Set 4" && sets == 21 && rows["ot_liga"].second == "On",
              rows["ot_fig"].second + " / " + rows["ot_sset"].second);
        // The words': "Style's" until set; the pair's kerning at a caret.
        auto st = *d.as<core::Story>("story.1");
        core::Local own;
        own.ot = core::OtFeatures{};
        own.ot->frac = true;
        own.kern = 30.0;
        st.paras[3].runs.push_back({0, 1, "", own});
        std::sort(st.paras[3].runs.begin(), st.paras[3].runs.end(), [](const core::Run& x, const core::Run& y) { return x.start < y.start; });
        d.put("story.1", st);
        std::map<std::string, std::string> v;
        for (const auto& x : core::text_fields(d, "story.1", {3, 1}, {3, 1})) v[x.edit] = x.value;
        check("the words' rows: fractions On (their own), the pair's kerning 30", v["lot_frac"] == "On" && v["lot_liga"] == "On" &&
                                                                                 v["lkern"] == "30 /1000 em", v["lot_frac"] + " / " + v["lkern"]);
        std::map<std::string, std::string> w;
        for (const auto& x : core::text_fields(d, "story.1", {4, 2}, {4, 2})) w[x.edit] = x.value;
        check("words with none of their own: Style's", w["lot_frac"] == "Style's" && w["lkern"] == "0");
    }
    // ── s268: an ODT's chapters on right-hand pages (a page style per side) ──
    {
        std::cout << "--- s268 an ODT's right-hand chapter starts ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:default-style style:family="paragraph"><style:text-properties fo:font-size="11pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1" style:master-page-name="Right_20_Page"><style:paragraph-properties fo:margin-top="0.5in"/></style:style>
 <style:style style:name="Coda" style:family="paragraph" style:master-page-name="Left_20_Page"/>
 <style:style style:name="Plate" style:family="paragraph" style:master-page-name="Blank"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1" style:page-usage="right"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
 <style:page-layout style:name="pm2" style:page-usage="left"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in"/></style:page-layout>
 <style:page-layout style:name="pm3" style:page-usage="all"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm3"/>
 <style:master-page style:name="Right_20_Page" style:display-name="Right Page" style:page-layout-name="pm1" style:next-style-name="Standard"/>
 <style:master-page style:name="Left_20_Page" style:display-name="Left Page" style:page-layout-name="pm2"/>
 <style:master-page style:name="Blank" style:page-layout-name="pm3"/>
</office:master-styles></office:document-styles>)";
        std::string body = R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter One</text:h>)"
                           R"(<text:p text:style-name="Standard">A short first chapter.</text:p>)"
                           R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter Two</text:h>)"
                           R"(<text:p text:style-name="Standard">Short again.</text:p>)"
                           R"(<text:p text:style-name="Plate">A plate.</text:p>)"
                           R"(<text:p text:style-name="Coda">The end.</text:p>)";
        const std::string content = "<office:document-content" + ns + "><office:body><office:text>" + body + "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: made", ok, err);
        if (ok) {
            check("a right-only page style: facing pages", d.settings().facing || core::any_facing(d));
            const auto sid = d.ids_of<core::Story>().front();
            const auto* st = d.as<core::Story>(sid);
            std::vector<std::string> sty;
            const auto start = [&](int i) { return core::doc::start_of(d, st->paras.at(static_cast<std::size_t>(i))); };
            check("Heading 1 (Right Page): next right-hand page; Coda (Left Page): left; Plate (Blank, any side): next page",
                  start(0) == core::ParaStart::NextRightPage && start(2) == core::ParaStart::NextRightPage &&
                      start(4) == core::ParaStart::NextPage && start(5) == core::ParaStart::NextLeftPage);
            // Where they land: chapter one on page 1 (right), page 2 left blank, chapter two on 3.
            const auto fl = L::flow_story(d, sid);
            std::map<int, int> page_of;   // paragraph -> page index
            std::set<int> blank;
            for (const auto& ft : fl.frames) {
                const int pi = d.page_index(d.as<core::Frame>(ft.frame)->owner);
                if (ft.lines.empty()) blank.insert(pi);
                for (const auto& l : ft.lines) if (!page_of.count(l.para)) page_of[l.para] = pi;
            }
            check("chapter one on page 1, page 2 blank, chapter two on page 3 (right-hand)",
                  page_of[0] == 0 && blank.count(1) && page_of[2] == 2 && !fl.overset &&
                      d.side_of(d.pages().at(2)) == core::Side::Right,
                  std::to_string(page_of[0]) + " / " + std::to_string(page_of[2]));
            check("the plate on the next page; the coda on a left-hand one after it",
                  page_of[4] == 3 && page_of[5] > page_of[4] && d.side_of(d.pages().at(static_cast<std::size_t>(page_of[5]))) == core::Side::Left,
                  std::to_string(page_of[4]) + " / " + std::to_string(page_of[5]));
        }
    }
    // ── s270: a master for each page style; their headers / footers ─────────
    {
        std::cout << "--- s270 an ODT's page styles as masters ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:default-style style:family="paragraph"><style:text-properties fo:font-size="11pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Title" style:family="paragraph" style:master-page-name="FrontMatter"/>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1" style:master-page-name="Right_20_Page"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1" style:page-usage="mirrored"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="1in"/></style:page-layout>
 <style:page-layout style:name="pm2" style:page-usage="right"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="2in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
 <style:page-layout style:name="pm3" style:page-usage="right"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1">
  <style:header><text:p text:style-name="Standard"><text:chapter text:display="name" text:outline-level="1">X</text:chapter></text:p></style:header>
  <style:header-left><text:p text:style-name="Standard">The Book</text:p></style:header-left>
  <style:footer><text:p text:style-name="Standard"><text:page-number>1</text:page-number></text:p></style:footer>
 </style:master-page>
 <style:master-page style:name="Right_20_Page" style:display-name="Right Page" style:page-layout-name="pm2" style:next-style-name="Standard">
  <style:footer><text:p text:style-name="Standard"><text:page-number>1</text:page-number></text:p></style:footer>
 </style:master-page>
 <style:master-page style:name="FrontMatter" style:page-layout-name="pm3" style:next-style-name="Standard">
  <style:header><text:p text:style-name="Standard"/></style:header>
 </style:master-page>
</office:master-styles></office:document-styles>)";
        std::string body = R"(<text:p text:style-name="Title">The Book</text:p>)";
        std::string lots;
        for (int i = 0; i < 70; ++i) lots += R"(<text:p text:style-name="Standard">The ship came in, and nobody on the quay expected her that morning, nor the next.</text:p>)";
        body += R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter One</text:h>)" + lots;
        body += R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter Two</text:h>)" + lots;
        const std::string content = "<office:document-content" + ns + "><office:body><office:text>" + body + "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: made", ok, err);
        if (ok) {
            std::map<std::string, std::string> by;   // master name -> id
            for (const auto& m : d.ids_of<core::Master>()) by[d.as<core::Master>(m)->name] = m;
            check("a master for each page style used, and one for inserted blank pages",
                  by.count("FrontMatter") && by.count("Right Page") && by.count("Standard") && by.count("Inserted blank") && by.size() == 4,
                  std::to_string(by.size()));
            const auto words_on = [&](const std::string& mid) {
                std::vector<std::string> w;
                for (const auto& f : d.frames_on(mid))
                    if (const auto* st = d.as<core::Story>(d.as<core::Frame>(f)->story)) w.push_back(st->paras.at(0).text);
                return w;
            };
            const auto std_w = words_on(by["Standard"]), right_w = words_on(by["Right Page"]);
            // (uneven margins: the one footer on each side, its own place -- 4 frames)
            check("Standard: a running head right, the book's title left, the page number below each side (4 frames)",
                  std_w.size() == 4 && std::count(std_w.begin(), std_w.end(), std::string("The Book")) == 1 &&
                      std::count(std_w.begin(), std_w.end(), std::string(core::kMarkChapter)) == 1 &&
                      std::count(std_w.begin(), std_w.end(), std::string(core::kMarkPage)) == 2,
                  std::to_string(std_w.size()));
            check("Right Page: its footer only; FrontMatter's empty header: no frame; inserted blank: nothing",
                  right_w.size() == 1 && right_w[0] == core::kMarkPage && words_on(by["FrontMatter"]).empty() && words_on(by["Inserted blank"]).empty(),
                  std::to_string(right_w.size()));
            {   // one story, one frame: the same words on both sides are two stories (one in two frames threads them)
                std::map<std::string, int> uses;
                for (const auto& m : d.ids_of<core::Master>())
                    for (const auto& f : d.frames_on(m))
                        if (const auto& sid = d.as<core::Frame>(f)->story; !sid.empty()) ++uses[sid];
                bool once = !uses.empty();
                for (const auto& [sid, n] : uses) once = once && n == 1;
                check("every header / footer story in one frame (the left page's footer shows its number)", once);
            }
            {   // s275: the import's heads are headers, its feet footers
                int heads = 0, feet = 0, none = 0;
                for (const auto& f : d.frames_on(by["Standard"])) {
                    const auto* fr = d.as<core::Frame>(f);
                    (fr->role == 1 ? heads : fr->role == 2 ? feet : none)++;
                }
                check("imported: two headers, two footers on Standard (their roles)", heads == 2 && feet == 2 && none == 0);
            }
            check("Right Page's own margins: 2 in at the top", std::abs(d.as<core::Master>(by["Right Page"])->margins.top - 144) < 0.01);
            // The pages: the title FrontMatter, chapters on Right Page, the rest Standard; blanks blank.
            const auto fl = L::flow_story(d, "story.1");
            const auto& st = *d.as<core::Story>("story.1");
            bool chapters_right = true, body_std = true, blanks_ok = true, frame_margins = true;
            int chapters = 0, blanks = 0;
            for (const auto& ft : fl.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                const auto& pg = *d.as<core::Page>(fr->owner);
                const auto mname = d.as<core::Master>(pg.master)->name;
                const auto r = core::margin_rect(d, fr->owner);
                frame_margins = frame_margins && std::abs(fr->y - r.y) < 0.01 && std::abs(fr->h - r.h) < 0.01;
                if (ft.lines.empty()) { ++blanks; blanks_ok = blanks_ok && mname == "Inserted blank"; continue; }
                const bool opens = ft.lines.front().start == 0 && st.paras[static_cast<std::size_t>(ft.lines.front().para)].text.rfind("Chapter", 0) == 0;
                if (opens) { ++chapters; chapters_right = chapters_right && mname == "Right Page" && d.side_of(fr->owner) == core::Side::Right; }
                else if (ft.lines.front().para > 0) body_std = body_std && mname == "Standard";
            }
            check("chapter pages on Right Page (right-hand), body pages on Standard", chapters == 2 && chapters_right && body_std,
                  std::to_string(chapters));
            check("inserted blank pages on the blank master; every text frame in its master's margins", blanks_ok && frame_margins && !fl.overset,
                  std::to_string(blanks) + " blank");
            check("the title page on FrontMatter", d.as<core::Master>(d.as<core::Page>(d.pages().front())->master)->name == "FrontMatter");
        }
    }
    // ── s271 / s272: an ODT's table of contents, a contents list ─────────────
    {
        std::cout << "--- s271 / s272 an ODT's table of contents ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:default-style style:family="paragraph"><style:text-properties fo:font-size="11pt"/></style:default-style>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Title" style:family="paragraph" style:master-page-name="FrontMatter"/>
 <style:style style:name="Contents_20_Heading" style:display-name="Contents Heading" style:family="paragraph"><style:text-properties fo:font-size="16pt"/></style:style>
 <style:style style:name="Contents_20_1" style:display-name="Contents 1" style:family="paragraph"><style:paragraph-properties><style:tab-stops><style:tab-stop style:position="4in" style:type="right" style:leader-text="."/></style:tab-stops></style:paragraph-properties></style:style>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1" style:master-page-name="Right_20_Page"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1" style:page-usage="mirrored"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
 <style:page-layout style:name="pm2" style:page-usage="right"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
 <style:master-page style:name="Right_20_Page" style:display-name="Right Page" style:page-layout-name="pm2" style:next-style-name="Standard"/>
 <style:master-page style:name="FrontMatter" style:page-layout-name="pm2" style:next-style-name="Standard"/>
</office:master-styles></office:document-styles>)";
        std::string lots;
        for (int i = 0; i < 40; ++i) lots += R"(<text:p text:style-name="Standard">The ship came in, and nobody on the quay expected her that morning, nor the next.</text:p>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="P4" style:family="paragraph" style:parent-style-name="Contents_20_Heading" style:master-page-name="FrontMatter"/>
 <style:style style:name="P5" style:family="paragraph" style:parent-style-name="Contents_20_1"/>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Title">The Book</text:p>)"
            R"(<text:table-of-content text:name="Table of Contents1"><text:table-of-content-source text:outline-level="1">)"
            R"(<text:index-title-template text:style-name="Contents_20_Heading">Table of Contents</text:index-title-template>)"
            R"(<text:table-of-content-entry-template text:outline-level="1" text:style-name="Contents_20_1"><text:index-entry-text/><text:index-entry-tab-stop style:type="right" style:leader-char="."/><text:index-entry-page-number/></text:table-of-content-entry-template></text:table-of-content-source>)"
            R"(<text:index-body><text:index-title><text:p text:style-name="P4">Contents</text:p></text:index-title>)"
            R"(<text:p text:style-name="P5">Chapter One<text:tab/>7</text:p><text:p text:style-name="P5">Chapter Two<text:tab/>9</text:p></text:index-body></text:table-of-content>)"
            R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter One</text:h>)" + lots +
            R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter Two</text:h>)" + lots +
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: one table of contents, after the title, level 1, its title and entry style",
              book && book->tocs.size() == 1 && book->tocs[0].at == 1 && book->tocs[0].levels == 1 && book->tocs[0].title == "Contents" &&
                  book->tocs[0].title_style == "P4" && book->tocs[0].entry_style == "Contents_20_1" && book->tocs[0].entries == 2 &&
                  book->contents == 0,
              err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        check("import: made, one contents list", ok && info.contents_lists == 1 && d.ids_of<core::GenList>().size() == 1, err);
        if (ok && d.ids_of<core::GenList>().size() == 1) {
            const auto& l = *d.as<core::GenList>(d.ids_of<core::GenList>().front());
            const auto& ls = *d.as<core::Story>(l.story);
            const auto* hs = d.as<core::Style>(l.from_styles.at(0));
            check("the list: Contents, gathering Heading 1, built", l.name == "Contents" && l.kind == core::ListKind::Contents && hs &&
                                                                     hs->name == "Heading 1" && l.built_entries == 2);
            const auto* es = ls.paras.size() == 3 ? d.as<core::Style>(ls.paras[1].style) : nullptr;
            check("its words: the title, then each chapter and its page, in Contents 1",
                  ls.paras.size() == 3 && ls.paras[0].text == "Contents" && ls.paras[1].text.rfind("Chapter One\t", 0) == 0 &&
                      ls.paras[2].text.rfind("Chapter Two\t", 0) == 0 && es && es->name == "Contents 1",
                  ls.paras.empty() ? "" : ls.paras[1].text);
            // Its page: the right-hand FrontMatter page after the title's; chapter one on a later page.
            const auto fl = L::flow_story(d, "story.1");
            std::map<std::string, int> page_of_text;
            const auto& st = *d.as<core::Story>("story.1");
            for (const auto& ft : fl.frames)
                for (const auto& line : ft.lines)
                    if (!page_of_text.count(st.paras[static_cast<std::size_t>(line.para)].text))
                        page_of_text[st.paras[static_cast<std::size_t>(line.para)].text] = d.page_index(d.as<core::Frame>(ft.frame)->owner);
            const int lp = d.page_index(l.placed_on);
            check("on its own right-hand page (FrontMatter), after the title's, before chapter one",
                  lp > page_of_text["The Book"] && lp < page_of_text["Chapter One"] && d.side_of(l.placed_on) == core::Side::Right &&
                      d.as<core::Master>(d.as<core::Page>(l.placed_on)->master)->name == "FrontMatter",
                  std::to_string(page_of_text["The Book"]) + " / " + std::to_string(lp) + " / " + std::to_string(page_of_text["Chapter One"]));
            check("Contents 1: a right tab at the frame's right edge, dot leader (the entry template's)",
                  es && es->tabs.size() == 1 && es->tabs[0].align == core::TabAlign::Right && es->tabs[0].leader == "." &&
                      std::abs(es->tabs[0].pos - core::margin_rect(d, l.placed_on).w) < 0.01,
                  es && !es->tabs.empty() ? std::to_string(es->tabs[0].pos) : "none");
            check("its page number for chapter one is this book's", ls.paras[1].text == "Chapter One\t" + d.folio(d.pages().at(static_cast<std::size_t>(page_of_text["Chapter One"]))),
                  ls.paras[1].text);
        }
    }
    // ── s273: New Index settled on a copy (a long book's way) ───────────────
    {
        std::cout << "--- s273 New Index settled apart ---\n";
        core::Doc a, b;
        core::load_sample(a);
        core::load_sample(b);
        const auto page = a.pages().back();
        core::DocJournal ja{a};
        a.set_door([&ja] { return ja.open(); });
        {   // index marks in both: harbor (twice), lanterns, map
            for (auto* doc : {&a, &b}) {
                core::DocJournal jx{*doc};
                doc->set_door([&jx] { return jx.open(); });
                core::doc::insert_index_mark(jx, *doc, "story.1", {3, 0}, "harbor");
                core::doc::insert_index_mark(jx, *doc, "story.1", {5, 0}, "harbor");
                core::doc::insert_index_mark(jx, *doc, "story.1", {4, 0}, "lanterns");
                core::doc::insert_index_mark(jx, *doc, "story.1", {6, 0}, "map");
            }
            a.set_door([&ja] { return ja.open(); });
        }
        const auto la = core::doc::add_list(ja, a, core::ListKind::Index, "Index", page);
        L::build_list(ja, a, la);
        // The worker's way: the copy holds the list-to-be under its own name.
        core::Doc scratch;
        {
            core::Doc::Loading load(scratch);
            for (const auto& [id, r] : b.records()) scratch.put(id, r);
            core::GenList l;
            l.name = "Index";
            l.kind = core::ListKind::Index;
            l.placed_on = page;
            l.built_entries = -1;
            scratch.put("list.new-index", l);
        }
        const auto built = L::settle_lists(scratch, {"list.new-index"});
        core::DocJournal jb{b};
        b.set_door([&jb] { return jb.open(); });
        const auto lb = core::doc::add_list(jb, b, core::ListKind::Index, "Index", page);
        const bool wrote = built.size() == 1 && core::doc::build_list(jb, b, lb, built.front().entries);
        const auto* sa = a.as<core::Story>(a.as<core::GenList>(la)->story);
        const auto* sb = b.as<core::Story>(b.as<core::GenList>(lb)->story);
        check("the same index as built here (its entries too)", wrote && sa && sb && sa->paras == sb->paras && sa->paras.size() >= 4,
              sa ? std::to_string(sa->paras.size()) + " paragraphs" : "no story");
    }
    // ── s275: a master's header / footer role ────────────────────────────────
    {
        std::cout << "--- s275 header / footer role ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("the sample's folios: footers", d.as<core::Frame>("frame.12")->role == 2 && d.as<core::Frame>("frame.13")->role == 2 &&
                                              d.as<core::Frame>("frame.14")->role == 2);
        core::NewDoc n;
        n.pages = 2;
        core::Doc e;
        core::new_document(e, n);
        int footers = 0;
        for (const auto& f : e.frames_on("master.1")) footers += e.as<core::Frame>(f)->role == 2;
        check("a new document's folio: its master's footer", footers == 1);
        const auto items = core::frame_items(d, {"frame.12"});
        std::string role;
        for (const auto& f : items.at(0).fields) if (f.edit == "frole") role = f.value;
        check("the inspector: Role -- Footer (a master's text frame)", role == "Footer", role);
        bool page_row = false;
        for (const auto& f : core::frame_items(d, {"frame.1"}).at(0).fields) page_row = page_row || f.edit == "frole";
        check("a page's frame: no Role row", !page_row);
        core::doc::edit_frames(j, d, {"frame.12"}, "Header / footer", [](core::Frame& f) { f.role = 1; });
        std::string err;
        core::Doc back;
        check("kept in the file", core::from_json(back, core::to_json(d), &err) && back.as<core::Frame>("frame.12")->role == 1, err);
    }
    // ── s276: header / footer presets for a new document ─────────────────────
    {
        std::cout << "--- s276 New: header / footer presets ---\n";
        const auto made = [](const core::NewDoc& n, core::Doc& d) {
            core::new_document(d, n);
            std::vector<const core::Frame*> out;
            for (const auto& f : d.frames_on("master.1")) out.push_back(d.as<core::Frame>(f));
            return out;
        };
        const auto first_text = [](const core::Doc& d, const core::Frame* f) { return d.as<core::Story>(f->story)->paras.at(0).text; };
        core::NewDoc n;
        n.pages = 2;
        n.facing = true;
        n.folio_outside = true;
        n.head = 1;
        core::Doc d;
        const auto fs = made(n, d);
        int foot_l = 0, foot_r = 0, head_l = 0, head_r = 0;
        for (const auto* f : fs) {
            const auto& p = d.as<core::Story>(f->story)->paras.at(0);
            if (f->role == 2 && f->side == core::Side::Left && p.over.align == core::Align::Left && p.text == core::kMarkPage) ++foot_l;
            if (f->role == 2 && f->side == core::Side::Right && p.over.align == core::Align::Right && p.text == core::kMarkPage) ++foot_r;
            if (f->role == 1 && f->side == core::Side::Left && p.text == core::kMarkTitle) ++head_l;
            if (f->role == 1 && f->side == core::Side::Right && p.text == core::kMarkChapter) ++head_r;
        }
        check("facing, outside corners: a folio a side (left on the left, right on the right)", foot_l == 1 && foot_r == 1);
        check("title left, chapter right: a running head a side, headers, in Running Head",
              head_l == 1 && head_r == 1 && fs.size() == 4 && d.as<core::Style>("style.7") && d.as<core::Style>("style.7")->name == "Running Head");
        bool in_margin = true;
        for (const auto* f : fs) in_margin = in_margin && (f->role == 1 ? f->y + f->h <= n.margins.top : f->y >= n.trim_h - n.margins.bottom);
        check("each in its margin (heads at the top, folios at the foot)", in_margin);
        core::NewDoc m;
        m.pages = 1;
        m.facing = false;
        m.head = 2;
        core::Doc e;
        const auto es = made(m, e);
        check("one-sided, chapter name, centred folio: one head, one folio",
              es.size() == 2 && std::count_if(es.begin(), es.end(), [&](const core::Frame* f) { return f->role == 1 && first_text(e, f) == core::kMarkChapter; }) == 1 &&
                  std::count_if(es.begin(), es.end(), [](const core::Frame* f) { return f->role == 2 && f->side == core::Side::Any; }) == 1);
        core::NewDoc plain;
        plain.pages = 1;
        core::Doc g;
        check("the defaults as before: one centred folio, no head, no Running Head style", made(plain, g).size() == 1 && !g.as<core::Style>("style.7"));
        core::AppSettings st;
        core::new_to_settings(n, st);
        const auto back = core::new_from_settings(st);
        check("the sheet remembers them", back.folio_outside && back.head == 1);
    }

    // -- s277: the navigator's spreads, as its card lays them out ----------
    {
        const std::vector<std::string> pages{"p1", "p2", "p3", "p4", "p5"};
        const std::vector<std::vector<std::string>> groups{{"p1"}, {"p2", "p3"}, {"p4", "p5"}};
        const auto left = [](const std::string& id) { return id == "p2" || id == "p4"; };
        const auto s = core::spread_slots(pages, groups, left);
        check("spreads: a lone right-hand first page gets an empty left", s.size() == 3 && s[0] == std::vector<std::string>{"", "p1"});
        check("spreads: pairs as they are", s[1] == std::vector<std::string>{"p2", "p3"} && s[2] == std::vector<std::string>{"p4", "p5"});
        const auto t = core::spread_slots({"p1", "p2", "p3", "p4"}, {{"p1"}, {"p2", "p3"}, {"p4"}}, left);
        check("spreads: a lone left-hand last page gets an empty right", t.size() == 3 && t[2] == std::vector<std::string>{"p4", ""});
        const auto u = core::spread_slots(pages, {}, left);
        check("spreads: not facing -- a page a row, no empty sides", u.size() == 5 && u[0] == std::vector<std::string>{"p1"});
    }

    // -- s277: Go to Page -- what a typed folio names ----------------------
    {
        core::Doc d;
        core::load_sample(d);
        const auto pages = d.pages();
        std::string p3, piv;
        for (const auto& p : pages) {
            if (d.folio(p) == "3") p3 = p;
            if (d.folio(p) == "iv") piv = p;
        }
        check("go to: a folio", !p3.empty() && core::go_to_target(d, "3") == p3);
        check("go to: a roman folio, any case, spaces ignored", !piv.empty() && core::go_to_target(d, " IV ") == piv);
        check("go to: +1 is the first page", core::go_to_target(d, "+1") == pages.front());
        check("go to: +N past the end, nothing", core::go_to_target(d, "+999").empty());
        const auto masters = d.ids_of<core::Master>();
        const auto* m0 = masters.empty() ? nullptr : d.as<core::Master>(masters.front());
        check("go to: a master by its prefix", m0 && core::go_to_target(d, m0->prefix) == masters.front());
        check("go to: a master by its label", m0 && core::go_to_target(d, core::master_label(d, masters.front())) == masters.front());
        check("go to: nothing matches, nothing", core::go_to_target(d, "zz9").empty() && core::go_to_target(d, "").empty());
    }

    // -- s278: a master dropped in a gap -- pages inserted there, one step --
    {
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.pages();
        const auto masters = d.ids_of<core::Master>();
        const auto made = core::doc::insert_pages(j, d, before.front(), 2, masters.front());
        const auto after = d.pages();
        check("insert: before the first page, two new pages lead", made.size() == 2 && after.size() == before.size() + 2 &&
                                                                         after[0] == made[0] && after[1] == made[1] && after[2] == before[0]);
        check("insert: on the master given", d.as<core::Page>(made[0])->master == masters.front());
        const auto mid = core::doc::insert_pages(j, d, before[3], 1, masters.front());
        const auto now = d.pages();
        const auto at = std::find(now.begin(), now.end(), before[3]);
        check("insert: before a page in the middle", mid.size() == 1 && at != now.begin() && *(at - 1) == mid[0]);
        j.undo();
        j.undo();
        check("insert: each one step (two undos, as before)", d.pages() == before);
    }

    // -- s280: the inspector's groups in a kept order ----------------------
    {
        const auto F = [](const char* label, const char* group) { core::Field f; f.label = label; f.group = group; return f; };
        const std::vector<core::Field> fs{F("Name", ""), F("Font", "Text"), F("Size", "Text"), F("Align", "Paragraph"),
                                          F("X", "Frame"), F("Y", "Frame"), F("", "")};
        const auto labels = [](const std::vector<core::Field>& v) { std::string s; for (const auto& f : v) s += f.label + std::string("|"); return s; };
        check("groups: no order kept -- as they came", labels(core::order_groups(fs, {})) == "Name|Font|Size|Align|X|Y||");
        check("groups: Frame first -- its rows together, the rest after in their order",
              labels(core::order_groups(fs, {"Frame"})) == "Name|X|Y|Font|Size|Align||");
        check("groups: a full order; ungrouped rows keep the top and the end",
              labels(core::order_groups(fs, {"Paragraph", "Frame", "Text"})) == "Name|Align|X|Y|Font|Size||");
        check("groups: names not shown are ignored", labels(core::order_groups(fs, {"Nope", "Text"})) == "Name|Font|Size|Align|X|Y||");
    }

    // -- s281 (X1): duplicating frames -- stories copied, threads kept -----
    {
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // The longest thread in the sample.
        std::map<std::string, std::vector<std::string>> by_story;
        for (const auto& id : d.ids_of<core::Frame>())
            if (const auto* f = d.as<core::Frame>(id); f->kind == core::FrameKind::Text && !f->story.empty()) by_story[f->story].push_back(id);
        std::string story;
        for (const auto& [s0, fs] : by_story) if (fs.size() > (story.empty() ? 1 : by_story[story].size())) story = s0;
        check("dup: the sample has a thread", !story.empty() && by_story[story].size() >= 2);
        // Its frames in thread order.
        std::vector<std::string> chain;
        {
            std::set<std::string> pointed;
            for (const auto& f : by_story[story]) if (!d.as<core::Frame>(f)->next.empty()) pointed.insert(d.as<core::Frame>(f)->next);
            for (const auto& f : by_story[story]) if (!pointed.count(f)) for (std::string at = f; !at.empty(); at = d.as<core::Frame>(at)->next) chain.push_back(at);
        }
        const auto frames_before = d.ids_of<core::Frame>().size();
        const auto stories_before = d.ids_of<core::Story>().size();
        const std::vector<std::string> two{chain[chain.size() - 1], chain[chain.size() - 2]};   // given out of order
        const auto made = core::doc::duplicate_frames(j, d, two, 18, 9);
        check("dup: two frames made, one story", made.size() == 2 && d.ids_of<core::Frame>().size() == frames_before + 2 &&
                                                 d.ids_of<core::Story>().size() == stories_before + 1);
        const auto* a = made.size() == 2 ? d.as<core::Frame>(made[1]) : nullptr;   // the copy of the earlier frame
        const auto* b = made.size() == 2 ? d.as<core::Frame>(made[0]) : nullptr;
        check("dup: threaded in the story's order", a && b && a->next == made[0] && b->next.empty());
        check("dup: a story of their own, the words the same", a && b && a->story == b->story && a->story != story &&
                                                               d.as<core::Story>(a->story)->paras.size() == d.as<core::Story>(story)->paras.size() &&
                                                               d.as<core::Story>(a->story)->paras.front().text == d.as<core::Story>(story)->paras.front().text);
        const auto* o = d.as<core::Frame>(chain[chain.size() - 2]);
        check("dup: moved by (18, 9), on its own page, on top", a && o && a->x == o->x + 18 && a->y == o->y + 9 && a->owner == o->owner && a->z > o->z);
        check("dup: the original untouched", o && o->story == story && o->next == chain.back());
        std::set<std::string> orig_anchors;
        for (const auto& p : d.as<core::Story>(story)->paras) for (const auto& x : p.anchors) orig_anchors.insert(x);
        bool shared = false;
        if (a) for (const auto& p : d.as<core::Story>(a->story)->paras) for (const auto& x : p.anchors) shared = shared || orig_anchors.count(x);
        check("dup: no anchor shared with the original", !shared);
        j.undo();
        check("dup: one step -- undone, all gone", d.ids_of<core::Frame>().size() == frames_before && d.ids_of<core::Story>().size() == stories_before);
        // Onto another page (a duplicated page's frames).
        const auto pages = d.pages();
        const auto* f0 = d.as<core::Frame>(chain.front());
        std::string other;
        for (const auto& p : pages) if (p != f0->owner) { other = p; break; }
        const auto moved = core::doc::duplicate_frames(j, d, {chain.front()}, 0, 0, {{f0->owner, other}});
        check("dup: onto the page the map names", moved.size() == 1 && d.as<core::Frame>(moved[0])->owner == other);
        check("dup: a lone frame of a thread -- the whole story, nowhere to run on", moved.size() == 1 && d.as<core::Frame>(moved[0])->next.empty());
    }

    // -- s282 (X1): cut, then paste -- the clip holds the frames by value ---
    {
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::string tf;
        for (const auto& id : d.ids_of<core::Frame>())
            if (const auto* f = d.as<core::Frame>(id); f->kind == core::FrameKind::Text && f->next.empty() && d.as<core::Page>(f->owner)) { tf = id; break; }
        const auto* f0 = d.as<core::Frame>(tf);
        const std::string words = f0 ? d.as<core::Story>(f0->story)->paras.front().text : std::string{};
        const auto clip = core::doc::clip_frames(d, {tf});
        check("clip: the frame and its story, its bounds", clip.order.size() == 1 && clip.stories.size() == 1 && f0 &&
                                                          clip.bounds.x == f0->x && clip.bounds.w == f0->w);
        core::doc::delete_frames(j, d, {tf});
        check("clip: cut -- the frame gone", !d.as<core::Frame>(tf));
        const auto pages = d.pages();
        const auto made = core::doc::paste_frames(j, d, clip, pages.back(), 0, 0);
        check("paste: onto the page given, the words with it", made.size() == 1 && d.as<core::Frame>(made[0])->owner == pages.back() &&
                                                               d.as<core::Story>(d.as<core::Frame>(made[0])->story)->paras.front().text == words);
        check("paste: onto nothing, nothing", core::doc::paste_frames(j, d, clip, "page.nope", 0, 0).empty());
        j.undo();
        const auto now = d.ids_of<core::Frame>();
        check("paste: one step", !made.empty() && std::find(now.begin(), now.end(), made[0]) == now.end());
    }

    // -- s283 (X1): duplicating pages -- a spread and what's on it ----------
    {
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.pages();
        // The pages with the most frames, side by side.
        std::size_t best = 0;
        for (std::size_t i = 0; i + 1 < before.size(); ++i)
            if (d.frames_on(before[i]).size() + d.frames_on(before[i + 1]).size() >
                d.frames_on(before[best]).size() + d.frames_on(before[best + 1]).size()) best = i;
        const std::vector<std::string> two{before[best], before[best + 1]};
        const auto made = core::doc::duplicate_pages(j, d, two);
        const auto after = d.pages();
        check("dup pages: two made, right after the two", made.size() == 2 && after.size() == before.size() + 2 &&
                                                          after[best + 2] == made[0] && after[best + 3] == made[1]);
        bool same = made.size() == 2;
        for (std::size_t k = 0; k < 2 && same; ++k)
            same = d.frames_on(made[k]).size() == d.frames_on(two[k]).size() &&
                   d.as<core::Page>(made[k])->master == d.as<core::Page>(two[k])->master &&
                   core::guides_on(d, made[k]).size() == core::guides_on(d, two[k]).size();
        check("dup pages: each its master, its guides, as many frames", same);
        bool own = true;
        for (const auto& p : made)
            for (const auto& f : d.frames_on(p))
                if (const auto* fr = d.as<core::Frame>(f); fr->kind == core::FrameKind::Text)
                    for (const auto& o : two) for (const auto& of : d.frames_on(o)) own = own && d.as<core::Frame>(of)->story != fr->story;
        check("dup pages: the copies' text is their own (no story shared)", own);
        j.undo();
        check("dup pages: one step", d.pages() == before);
    }

    {   // s283: a copy before the first page
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.pages();
        const auto made = core::doc::duplicate_pages(j, d, {before[3]}, {}, true);
        check("dup pages: at the front", made.size() == 1 && d.pages().front() == made[0] && d.pages()[1] == before[0]);
    }

    {   // s284: Step and Repeat
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::string sh;
        for (const auto& id : d.ids_of<core::Frame>()) if (d.as<core::Frame>(id)->kind != core::FrameKind::Text) { sh = id; break; }
        const auto n0 = d.ids_of<core::Frame>().size();
        const auto made = core::doc::step_repeat(j, d, {sh}, 4, 10, 5);
        const auto* f = d.as<core::Frame>(sh);
        check("step & repeat: four copies, stepped", made.size() == 4 && f && d.as<core::Frame>(made[3])->x == f->x + 40 &&
                                                     d.as<core::Frame>(made[3])->y == f->y + 20 && d.as<core::Frame>(made[0])->x == f->x + 10);
        j.undo();
        check("step & repeat: one step", d.ids_of<core::Frame>().size() == n0);
    }

    // -- s286: an ODT's alphabetical index and its marks -------------------
    {
        std::cout << "--- s286 an ODT's alphabetical index ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Index_20_Heading" style:display-name="Index Heading" style:family="paragraph"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">The <text:alphabetical-index-mark text:string-value="harbor"/>harbor at dawn.</text:p>)"
            R"(<text:p text:style-name="Standard">A <text:alphabetical-index-mark-start text:id="m1"/>night ferry<text:alphabetical-index-mark-end text:id="m1"/> came.</text:p>)"
            R"(<text:p text:style-name="Standard">Gulls<text:alphabetical-index-mark text:string-value="herring gull" text:key1="birds"/> cried.</text:p>)"
            R"(<text:alphabetical-index text:name="Index1"><text:alphabetical-index-source><text:index-title-template text:style-name="Index_20_Heading">Index</text:index-title-template></text:alphabetical-index-source>)"
            R"(<text:index-body><text:index-title><text:p text:style-name="Index_20_Heading">Index</text:p></text:index-title><text:p>harbor, 1</text:p></text:index-body></text:alphabetical-index>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: one index after the three paragraphs, its title; not counted as left out",
              book && book->indexes.size() == 1 && book->indexes[0].at == 3 && book->indexes[0].title == "Index" && book->contents == 0, err);
        check("read: a point mark, a marked stretch, a mark under key1",
              book && book->body.size() >= 3 && book->body[0].index.size() == 1 && book->body[0].index[0].term == "harbor" &&
                  book->body[1].index.size() == 1 && book->body[1].index[0].term == "night ferry" &&
                  book->body[2].index.size() == 1 && book->body[2].index[0].term == "birds" && book->body[2].index[0].sub == "herring gull");
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        check("import: an index, built from the three marks", ok && info.indexes == 1 && info.index_marks == 3, err);
        int marks = 0;
        if (ok) for (const auto& p : d.as<core::Story>("story.1")->paras) marks += static_cast<int>(p.index.size());
        check("import: the marks in the story", marks == 3);
        bool built = false;
        for (const auto& id : d.ids_of<core::GenList>())
            if (const auto* l = d.as<core::GenList>(id); l->kind == core::ListKind::Index && l->built_entries > 0 && d.as<core::Story>(l->story)) built = true;
        check("import: the index list built on its page", built);
    }

    // -- s288 / s289: an ODT page style's header / footer room, its columns --
    {
        std::cout << "--- s288 / s289 an ODT's header height, columns ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.5in" fo:margin-bottom="0.5in" fo:margin-left="0.75in" fo:margin-right="0.75in">
  <style:columns fo:column-count="2" fo:column-gap="0.25in"/></style:page-layout-properties>
  <style:header-style><style:header-footer-properties fo:min-height="0.5in" fo:margin-bottom="0.25in"/></style:header-style>
  <style:footer-style><style:header-footer-properties fo:min-height="0.25in" fo:margin-top="0.125in"/></style:footer-style>
 </style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1">
  <style:header><text:p text:style-name="Standard">THE BOOK</text:p></style:header>
  <style:footer><text:p text:style-name="Standard"><text:page-number/></text:p></style:footer>
 </style:master-page>
</office:master-styles></office:document-styles>)";
        std::string lots;
        for (int i = 0; i < 30; ++i) lots += R"(<text:p text:style-name="Standard">The ship came in, and nobody on the quay expected her that morning, nor the next.</text:p>)";
        const std::string content = "<office:document-content" + ns + "><office:body><office:text>" + lots + "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        const auto* lay = book && book->layouts.count("pm1") ? &book->layouts.at("pm1") : nullptr;
        check("read: the header's / footer's least heights and spacing, two columns",
              lay && lay->head && lay->foot && std::abs(lay->head_min - 36) < 0.01 && std::abs(lay->head_gap - 18) < 0.01 &&
                  std::abs(lay->foot_min - 18) < 0.01 && std::abs(lay->foot_gap - 9) < 0.01 && lay->columns == 2 && std::abs(lay->column_gap - 18) < 0.01, err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const auto* m = ok ? d.as<core::Master>("master.1") : nullptr;
        check("import: the body starts below margin + header + spacing; ends above footer + spacing",
              m && std::abs(m->margins.top - (36 + 36 + 18)) < 0.01 && std::abs(m->margins.bottom - (36 + 18 + 9)) < 0.01, err);
        check("import: the master's two columns, the gap", m && m->columns == 2 && std::abs(m->gutter - 18) < 0.01);
        const core::Frame* head = nullptr;
        const core::Frame* foot = nullptr;
        for (const auto& f : ok ? d.frames_on("master.1") : std::vector<std::string>{}) {
            const auto* fr = d.as<core::Frame>(f);
            if (fr->role == 1) head = fr;
            if (fr->role == 2) foot = fr;
        }
        check("import: the header at the page's margin, its least height; the footer above the bottom margin",
              head && foot && m && std::abs(head->y - 36) < 0.01 && std::abs(head->h - 36) < 0.01 &&
                  std::abs(foot->y + foot->h - (m->h - 36)) < 0.01, err);
        bool cols = ok;
        for (const auto& p : ok ? d.pages() : std::vector<std::string>{})
            for (const auto& f : d.frames_on(p))
                if (const auto* fr = d.as<core::Frame>(f); fr->story == "story.1") cols = cols && fr->columns == 2;
        check("import: the body's frames in two columns", cols);
    }

    // -- s290: a contents list longer than its page runs on ----------------
    {
        std::cout << "--- s290 a long contents list runs on ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="11pt"/></style:style>
 <style:style style:name="Contents_20_1" style:display-name="Contents 1" style:family="paragraph"/>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        std::string chapters;
        for (int i = 1; i <= 90; ++i)
            chapters += "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">Chapter " + std::to_string(i) +
                        "</text:h><text:p text:style-name=\"Standard\">A few words.</text:p>";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)"
            R"(<text:table-of-content text:name="T1"><text:table-of-content-source text:outline-level="1">)"
            R"(<text:table-of-content-entry-template text:outline-level="1" text:style-name="Contents_20_1"><text:index-entry-text/></text:table-of-content-entry-template></text:table-of-content-source>)"
            R"(<text:index-body><text:p>x</text:p></text:index-body></text:table-of-content>)" + chapters +
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        std::string story;
        for (const auto& id : ok ? d.ids_of<core::GenList>() : std::vector<std::string>{}) story = d.as<core::GenList>(id)->story;
        int frames = 0;
        for (const auto& f : d.ids_of<core::Frame>()) frames += d.as<core::Frame>(f)->story == story && !story.empty();
        check("import: the 90-entry contents ran on -- pages added, its frames threaded, nothing overset",
              ok && info.list_pages > 0 && frames > 1 && !L::flow_story(d, story).overset, std::to_string(info.list_pages) + " page(s)");
        check("import: the book's text not overset after", ok && !L::flow_story(d, "story.1").overset);
    }

    // -- s291: an ODT's bibliography -- sources, citations, the list --------
    {
        std::cout << "--- s291 an ODT's bibliography ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">As Smith showed <text:bibliography-mark text:identifier="Smith01" text:bibliography-type="book" text:author="Smith, J." text:title="Harbors" text:year="2001" text:publisher="Dock Press">[Smith01]</text:bibliography-mark>, and Jones <text:bibliography-mark text:identifier="Jones99" text:bibliography-type="article" text:author="Jones, A." text:title="Tides" text:journal="Sea Review" text:year="1999">[Jones99]</text:bibliography-mark>.</text:p>)"
            R"(<text:p text:style-name="Standard">Again <text:bibliography-mark text:identifier="Smith01" text:author="Smith, J.">[Smith01]</text:bibliography-mark>.</text:p>)"
            R"(<text:bibliography text:name="Bibliography1"><text:bibliography-source><text:index-title-template>Bibliography</text:index-title-template></text:bibliography-source>)"
            R"(<text:index-body><text:index-title><text:p>Bibliography</text:p></text:index-title><text:p>x</text:p></text:index-body></text:bibliography>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: two sources, in the order first cited; three citations; one bibliography; nothing left out",
              book && book->source_order == std::vector<std::string>{"Smith01", "Jones99"} && book->body.size() >= 2 &&
                  book->body[0].cites.size() == 2 && book->body[1].cites.size() == 1 && book->bibliographies.size() == 1 && book->contents == 0, err);
        check("read: a citation's shown words dropped (Composix numbers it)", book && book->body[0].text.find("[Smith01]") == std::string::npos);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        check("import: two sources, three citations, a bibliography", ok && info.sources == 2 && info.citations == 3 && info.bibliographies == 1, err);
        const core::GenList* bl = nullptr;
        for (const auto& id : ok ? d.ids_of<core::GenList>() : std::vector<std::string>{})
            if (d.as<core::GenList>(id)->kind == core::ListKind::Bibliography) bl = d.as<core::GenList>(id);
        check("import: the bibliography built on its page, the two sources in it", bl && bl->built_entries == 2 && d.as<core::Page>(bl->placed_on),
              bl ? std::to_string(bl->built_entries) + " / '" + bl->placed_on + "'" : std::string("none"));
        bool aimed = ok;
        int cites = 0;
        if (ok)
            for (const auto& p : d.as<core::Story>("story.1")->paras)
                for (const auto& r : p.refs) {
                    ++cites;
                    aimed = aimed && r.show == core::RefShow::Cite && !core::doc::find_anchor(d, r.target).story.empty();
                    err += r.target + ";";
                }
        check("import: each citation a Cite reference to its source's anchor", aimed && cites == 3, std::to_string(cites) + " " + err);
    }

    // -- s293: an ODT section in two columns -- its paragraphs split together --
    {
        std::cout << "--- s293 an ODT section's columns ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        std::string inner;
        for (int i = 0; i < 6; ++i) inner += R"(<text:p text:style-name="Standard">Gulls wheeled over the quay while the ferry came in slow against the tide, and nobody looked up.</text:p>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="Sect1" style:family="section"><style:section-properties><style:columns fo:column-count="2" fo:column-gap="0.2in"/></style:section-properties></style:style>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">Before the section.</text:p>)"
            R"(<text:section text:style-name="Sect1" text:name="Section1">)" + inner + R"(</text:section>)"
            R"(<text:p text:style-name="Standard">After the section.</text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: the section's six paragraphs in two columns, 0.2 in apart; the others not",
              book && book->body.size() == 8 && book->body[1].section_cols == 2 && std::abs(book->body[1].section_gap - 14.4) < 0.01 &&
                  book->body[0].section_cols == 0 && book->body[7].section_cols == 0, err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const auto& st = *d.as<core::Story>("story.1");
        const auto* ss = ok ? d.as<core::Style>(st.paras[1].style) : nullptr;
        check("import: a split style, two columns, together", ss && ss->split == 2 && ss->split_run && std::abs(ss->split_gutter - 14.4) < 0.01 &&
                                                             st.paras[6].style == st.paras[1].style && st.paras[0].style != st.paras[1].style, err);
        const auto fl = L::flow_story(d, "story.1");
        std::set<int> cols, bands;
        int first_right = -1;
        for (const auto& ft : fl.frames)
            for (const auto& l : ft.lines)
                if (l.para >= 1 && l.para <= 6) {
                    cols.insert(l.col);
                    bands.insert(l.band);
                    if (l.col == 1 && first_right < 0) first_right = l.para;
                }
        check("flow: the six balanced as ONE band over both columns (the right column takes a later paragraph)",
              cols.size() == 2 && bands.size() == 1 && first_right >= 3, std::to_string(first_right));
    }

    // -- s294: an ODT contents' deeper levels keep their own entry styles ----
    {
        std::cout << "--- s294 an ODT contents' level 2 ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"/>
 <style:style style:name="Contents_20_1" style:display-name="Contents 1" style:family="paragraph"/>
 <style:style style:name="Contents_20_2" style:display-name="Contents 2" style:family="paragraph"><style:paragraph-properties fo:margin-left="0.2in"/></style:style>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1"/>
 <style:style style:name="Heading_20_2" style:display-name="Heading 2" style:family="paragraph" style:default-outline-level="2"/>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)"
            R"(<text:table-of-content text:name="T1"><text:table-of-content-source text:outline-level="2">)"
            R"(<text:table-of-content-entry-template text:outline-level="1" text:style-name="Contents_20_1"><text:index-entry-text/></text:table-of-content-entry-template>)"
            R"(<text:table-of-content-entry-template text:outline-level="2" text:style-name="Contents_20_2"><text:index-entry-text/><text:index-entry-tab-stop style:type="right" style:leader-char="."/><text:index-entry-page-number/></text:table-of-content-entry-template>)"
            R"(</text:table-of-content-source><text:index-body><text:p>x</text:p></text:index-body></text:table-of-content>)"
            R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Part One</text:h>)"
            R"(<text:h text:style-name="Heading_20_2" text:outline-level="2">The Quay</text:h><text:p text:style-name="Standard">Words.</text:p>)"
            R"(<text:h text:style-name="Heading_20_2" text:outline-level="2">The Ferry</text:h><text:p text:style-name="Standard">Words.</text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: both levels' templates; level 2's right tab, its dots",
              book && book->tocs.size() == 1 && book->tocs[0].level_entries.size() == 2 && book->tocs[0].level_entries[1].style == "Contents_20_2" &&
                  book->tocs[0].level_entries[1].right_tab && book->tocs[0].level_entries[1].leader == ".", err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const core::GenList* gl = nullptr;
        for (const auto& id : ok ? d.ids_of<core::GenList>() : std::vector<std::string>{}) gl = d.as<core::GenList>(id);
        const auto* s2 = gl && gl->entry_styles.size() == 2 ? d.as<core::Style>(gl->entry_styles[1]) : nullptr;
        check("import: level 2's entries in Contents 2, its right tab with dots", s2 && s2->name == "Contents 2" && s2->tabs.size() == 1 &&
                                                                               s2->tabs[0].align == core::TabAlign::Right && s2->tabs[0].leader == ".", err);
        int in2 = 0;
        if (gl && d.as<core::Story>(gl->story))
            for (const auto& p : d.as<core::Story>(gl->story)->paras) in2 += p.style == gl->entry_styles[1];
        check("import: the two level-2 entries built in it", in2 == 2, std::to_string(in2));
    }

    // -- s295: "split together" in a frame of two columns too ---------------
    {
        std::cout << "--- s295 split together in a two-column frame ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="9pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="8.5in" fo:page-height="11in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in">
  <style:columns fo:column-count="2" fo:column-gap="0.25in"/></style:page-layout-properties></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        std::string inner;
        for (int i = 0; i < 4; ++i) inner += R"(<text:p text:style-name="Standard">Gulls over the quay, the ferry slow on the tide.</text:p>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="Sect1" style:family="section"><style:section-properties><style:columns fo:column-count="2" fo:column-gap="0.1in"/></style:section-properties></style:style>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">Before.</text:p><text:section text:style-name="Sect1" text:name="S1">)" + inner +
            R"(</text:section><text:p text:style-name="Standard">After.</text:p></office:text></office:body></office:document-content>)";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const auto fl = L::flow_story(d, "story.1");
        std::set<int> bands;
        std::set<double> xs;
        bool two_col_frame = false;
        for (const auto& ft : fl.frames)
            for (const auto& l : ft.lines)
                if (l.para >= 1 && l.para <= 4) {
                    bands.insert(l.band);
                    xs.insert(std::round(l.x));
                    two_col_frame = two_col_frame || core::column_count(*d.as<core::Frame>(ft.frame)) == 2;
                }
        check("in a two-column frame: the section's four paragraphs one band over two sub-columns",
              ok && two_col_frame && bands.size() == 1 && *bands.begin() >= 1000 && xs.size() == 2, err);
    }

    // -- s296: a baseline shift on a path -- the raised words off the path --
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto ln = D::add_path(j, d, page, {D::corner_point(100, 300), D::corner_point(400, 300)}, true);
        D::set_frame_content(j, d, {ln}, core::FrameKind::Text);
        D::edit_frames(j, d, {ln}, "On path", [](core::Frame& f) { f.on_path = true; });
        {
            core::Doc::Loading ld(d);
            core::Story st = *d.as<core::Story>(d.as<core::Frame>(ln)->story);
            st.paras[0].text = "low HIGH low";
            core::Run r;
            r.start = 4;
            r.end = 8;
            r.local.shift = 6;   // up 6 pt
            st.paras[0].runs = {r};
            d.put(d.as<core::Frame>(ln)->story, st);
        }
        namespace fs = std::filesystem;
        const auto base = fs::temp_directory_path() / "composix_selftest_rise";
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
        // each glyph: "a b c d x y Tm [" -- y the baseline off the path
        int at0 = 0, raised = 0;
        for (auto i = pdf.find(" Tm ["); i != std::string::npos; i = pdf.find(" Tm [", i + 1)) {
            const auto ls = pdf.rfind(' ', i - 1);
            const double y = std::atof(pdf.substr(ls + 1, i - ls - 1).c_str());
            if (std::abs(y) < 0.01) ++at0;
            else if (std::abs(y + 6) < 0.6 || std::abs(y - 6) < 0.6) ++raised;
        }
        check("on a path: the shifted word's glyphs 6 pt off the path, the rest on it", ok && raised == 4 && at0 >= 6,
              std::to_string(raised) + " raised, " + std::to_string(at0) + " on");
        fs::remove_all(base, ec);
    }

    // -- s297: words in and out through the system clipboard ---------------
    {
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.pages().back();
        const auto f = core::doc::paste_text_frame(j, d, page, {36, 72, 200, 36}, "First line\r\nSecond line\n\n");
        const auto* fr = d.as<core::Frame>(f);
        const auto* st = fr ? d.as<core::Story>(fr->story) : nullptr;
        check("paste text: a frame, its words a paragraph a line, auto-sized", fr && st && st->paras.size() == 2 && st->paras[0].text == "First line" &&
                                                                              st->paras[1].text == "Second line" && fr->autosize && fr->owner == page);
        check("paste text: nothing to paste, nothing made", core::doc::paste_text_frame(j, d, page, {36, 72, 200, 36}, " \n ").empty());
        check("frames' words: each story once, a line a paragraph", core::doc::frames_text(d, {f, f}) == "First line\nSecond line");
        j.undo();
        check("paste text: one step", !d.as<core::Frame>(f));
    }

    // -- s299 - s301: align, distribute, group, lock ------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.pages().back();
        const auto a = D::add_frame(j, d, page, core::FrameKind::Shape, {40, 40, 30, 20});
        const auto b = D::add_frame(j, d, page, core::FrameKind::Shape, {100, 90, 50, 40});
        const auto c = D::add_frame(j, d, page, core::FrameKind::Shape, {250, 200, 20, 20});
        const auto pl = core::layout_pages(d, {page});
        const auto F = [&](const std::string& id) { return *d.as<core::Frame>(id); };
        check("align: left edges to the selection's", D::align_frames(j, d, pl, {a, b, c}, 0, D::AlignTo::Selection) &&
                                                       F(a).x == 40 && F(b).x == 40 && F(c).x == 40);
        check("align: bottoms", D::align_frames(j, d, pl, {a, b, c}, 5, D::AlignTo::Selection) &&
                                 F(a).y + F(a).h == 220 && F(b).y + F(b).h == 220 && F(c).y == 200);
        j.undo();
        j.undo();
        check("align: one step each (undone)", F(a).x == 40 && F(b).x == 100 && F(c).x == 250 && F(b).y == 90);
        check("distribute: centres across -- the middle one midway", D::distribute_frames(j, d, pl, {a, b, c}, 0) &&
                                                                    std::abs((F(b).x + F(b).w / 2) - (55 + 260) / 2.0) < 1e-6 && F(a).x == 40 && F(c).x == 250);
        j.undo();
        check("distribute: even gaps across", D::distribute_frames(j, d, pl, {a, b, c}, 2) &&
                                              std::abs((F(b).x - (F(a).x + F(a).w)) - (F(c).x - (F(b).x + F(b).w))) < 1e-6);
        j.undo();
        check("distribute: two -- nothing", !D::distribute_frames(j, d, pl, {a, b}, 0));
        const auto m = core::margin_rect(d, page);
        check("align: to the margins' right", D::align_frames(j, d, pl, {a}, 2, D::AlignTo::Margins) && std::abs(F(a).x + F(a).w - (m.x + m.w)) < 1e-6);
        j.undo();
        check("group: two frames, one id", D::group_frames(j, d, {a, b}) && !F(a).group.empty() && F(a).group == F(b).group);
        const auto with = core::with_groups(d, {a});
        check("group: one picked brings the other", with.size() == 2 && std::find(with.begin(), with.end(), b) != with.end());
        check("group: aligned as one unit (its bounds), the single frame to it",
              D::align_frames(j, d, pl, {a, b, c}, 0, D::AlignTo::Selection) && F(a).x == 40 && F(b).x == 100 && F(c).x == 40);
        j.undo();
        check("ungroup", D::ungroup_frames(j, d, {a}) && F(a).group.empty() && F(b).group.empty());
        const auto picked_c = [&] { return core::hit_frame(d, pl, pl.front().r.x + 260, pl.front().r.y + 210, 0) == c; };
        const auto boxed_c = [&] { const auto in = core::frames_in(d, pl, {pl.front().r.x + 240, pl.front().r.y + 190, 50, 50});
                                   return std::find(in.begin(), in.end(), c) != in.end(); };
        check("lock: picked before it", picked_c() && boxed_c());
        check("lock: not picked on the canvas", D::lock_frames(j, d, {c}, true) && F(c).locked && !picked_c());
        check("lock: not in a marquee", !boxed_c());
        check("unlock all on the page", D::unlock_all(j, d, {page}) == 1 && !F(c).locked);
        core::Doc e;
        D::group_frames(j, d, {a, b});
        D::lock_frames(j, d, {c}, true);
        check("group / lock: saved", core::from_json(e, core::to_json(d)) && e.as<core::Frame>(a)->group == F(a).group && e.as<core::Frame>(c)->locked);
    }
}
}  // namespace cxtest
