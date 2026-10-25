// composix_selftest, part 9 (s256: part8 had grown to 3800 lines).
#include "Selftest.hpp"
#include "layout/Composition.hpp"
#include "core/Spell.hpp"
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
                    if (fr->body) continue;   // (s352: the body frame -- neither)
                    (fr->role == 1 ? heads : fr->role == 2 ? feet : none)++;
                }
                check("imported: two headers, two footers on Standard (their roles)", heads == 2 && feet == 2 && none == 0);
                // s352: the body's page frames follow a body frame on their master, kept to its margins
                int bodies = 0, follows = 0, body_frames = 0;
                for (const auto& f : d.frames_on(by["Standard"])) bodies += d.as<core::Frame>(f)->body && d.as<core::Frame>(f)->fit_margins;
                for (const auto& f : d.ids_of<core::Frame>())
                    if (d.as<core::Frame>(f)->story == "story.1") { ++body_frames; follows += d.as<core::Frame>(f)->follow_body; }
                check("imported: body frames on Standard, the body's frames following them", bodies >= 1 && follows == body_frames && follows > 0,
                      std::to_string(bodies) + " / " + std::to_string(follows) + " of " + std::to_string(body_frames));
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
    // -- s303 / s304: hyperlinks in text -------------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("link: www. made a web address", D::normal_link("  www.example.com ") == "https://www.example.com");
        check("link: an address made mailto:", D::normal_link("ann@example.com") == "mailto:ann@example.com");
        check("link: a scheme kept; #id kept", D::normal_link("ftp://x.org/a") == "ftp://x.org/a" && D::normal_link("#anchor.1") == "#anchor.1");
        check("link: set on the words (one step)", D::set_link(j, d, "story.1", {1, 4}, {1, 10}, "www.example.com"));
        auto ls = D::hyperlinks(d);
        check("link: listed -- its words, its target", ls.size() == 1 && ls[0].words == "harbor" && ls[0].target == "https://www.example.com" &&
                                                         ls[0].para == 1 && ls[0].from == 4 && ls[0].to == 10, ls.empty() ? "none" : ls[0].words);
        check("link: at a caret inside, over the words", D::link_at(d, "story.1", {1, 6}, {1, 6}) == "https://www.example.com" &&
                                                            D::link_at(d, "story.1", {1, 4}, {1, 10}) == "https://www.example.com");
        check("link: a selection past it -- mixed", D::link_at(d, "story.1", {1, 0}, {1, 10}).empty());
        D::edit_text(j, d, "story.1", {1, 10}, {1, 10}, "s");   // at its end: stays out
        D::edit_text(j, d, "story.1", {1, 6}, {1, 6}, "X");     // inside: joins it
        ls = D::hyperlinks(d);
        check("link: typed at its end stays out, typed inside joins", ls.size() == 1 && ls[0].words == "haXrbor", ls.empty() ? "none" : ls[0].words);
        {
            core::Doc e;
            check("link: saved", core::from_json(e, core::to_json(d)) && D::hyperlinks(e) == D::hyperlinks(d));
        }
        // s304: to an anchor in the book
        const auto anchor = D::insert_anchor(j, d, "story.1", {4, 0});
        check("link: to an anchor", !anchor.empty() && D::set_link(j, d, "story.1", {3, 0}, {3, 7}, "#" + anchor));
        ls = D::hyperlinks(d);
        check("link: to an anchor -- not broken", ls.size() == 2 && !ls[1].broken && ls[1].target == "#" + anchor);
        check("link: to a missing anchor -- broken", D::set_link(j, d, "story.2", {0, 0}, {0, 4}, "#anchor.none") && D::hyperlinks(d).back().broken);
        j.undo();
        {
            L::FlowCache flows;
            const auto pl = L::hyperlink_links(d, flows);
            const auto out = std::count_if(pl.begin(), pl.end(), [](const L::PdfLink& l) { return l.uri == "https://www.example.com"; });
            const auto in = std::find_if(pl.begin(), pl.end(), [](const L::PdfLink& l) { return l.uri.empty(); });
            const auto at = D::find_anchor(d, anchor);
            check("link: PDF boxes -- the web one, and the anchor's to its page", out >= 1 && in != pl.end() && !in->to.empty() &&
                                                                                 in->box.w > 0 && in->box.h > 0 && at.story == "story.1",
                  std::to_string(out) + " out");
        }
        namespace fs = std::filesystem;
        const auto base = fs::temp_directory_path() / "composix_selftest_links";
        std::error_code ec;
        fs::create_directories(base, ec);
        L::PdfOptions opt;
        opt.compress = false;
        std::string err;
        const auto read = [](const std::string& f) { std::ifstream in(f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
        const auto print = (base / "print.pdf").string(), screen = (base / "screen.pdf").string();
        L::PdfReport rep;
        const bool ok1 = L::export_print_pdf(d, print, opt, {}, &rep, &err);
        const bool ok2 = L::export_pdf(d, screen, opt, {}, nullptr, &err);
        const auto p1 = read(print), p2 = read(screen);
        check("link: in the print PDF, a /URI action", ok1 && p1.find("/URI (https://www.example.com)") != std::string::npos);
        check("link: in the screen PDF too", ok2 && p2.find("https://www.example.com") != std::string::npos);
        {
            L::FlowCache flows;
            const auto sum = L::export_summary(d, flows, opt, true, "");
            check("link: the summary counts them", sum.hyperlinks >= 2 && sum.nav.find("hyperlink") != std::string::npos, sum.nav);
        }
        check("link: taken off (one step)", D::set_link(j, d, "story.1", {1, 0}, {1, 20}, "") && D::hyperlinks(d).size() == 1);
        fs::remove_all(base, ec);
    }
    // -- s304: an ODT's hyperlinks and bookmarks -----------------------------
    {
        std::cout << "--- s304 an ODT's links ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f" xmlns:xlink="x")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:style>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:default-outline-level="1"><style:text-properties fo:font-size="16pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)"
            R"(<text:h text:style-name="Heading_20_1" text:outline-level="1">Harbors</text:h>)"
            R"(<text:p text:style-name="Standard">See <text:a xlink:type="simple" xlink:href="https://example.org/x">the site</text:a>, )"
            R"(<text:a xlink:href="#Harbors|outline">harbors</text:a>, <text:a xlink:href="#bm1">the mark</text:a> and <text:a xlink:href="#nowhere">lost</text:a>.</text:p>)"
            R"(<text:p text:style-name="Standard">Here <text:bookmark text:name="bm1"/>is the mark.</text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: four links, one bookmark", book && book->body.size() == 3 && book->body[1].links.size() == 4 &&
                                                book->body[2].bookmarks.size() == 1 && book->body[1].links[0].href == "https://example.org/x", err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const auto ls = core::doc::hyperlinks(d);
        std::string got;
        for (const auto& l : ls) got += l.words + "=" + l.target + (l.broken ? "!" : "") + "; ";
        const auto find = [&](const std::string& w) { return std::find_if(ls.begin(), ls.end(), [&](const auto& l) { return l.words == w; }); };
        check("import: counted -- 4 links, 1 bookmark, 1 not found (said)", ok && info.links == 4 && info.bookmarks == 1 && info.links_broken == 1 &&
                                                                             !info.notes_said.empty(), got + " ok=" + std::to_string(ok) + " links=" + std::to_string(info.links) + " " + err);
        check("import: the web link as written", find("the site") != ls.end() && find("the site")->target == "https://example.org/x", got);
        const auto bm = find("the mark");
        check("import: the bookmark's link to its anchor (in its paragraph)", bm != ls.end() && !bm->broken && bm->target.size() > 1 &&
                                                                               core::doc::find_anchor(d, bm->target.substr(1)).at.para == 2, got);
        const auto hd = find("harbors");
        check("import: the heading's link -- an anchor made at the heading", hd != ls.end() && !hd->broken &&
                                                                              core::doc::find_anchor(d, hd->target.substr(1)).at.para == 0 &&
                                                                              d.as<core::Story>("story.1")->paras[0].text.find("Harbors") == 3, got);
        check("import: a place not found -- kept, broken", find("lost") != ls.end() && find("lost")->broken, got);
    }
    // -- s305: layers --------------------------------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.pages().back();
        const auto F = [&](const std::string& id) { return *d.as<core::Frame>(id); };
        const auto a = D::add_frame(j, d, page, core::FrameKind::Shape, {40, 40, 100, 100});
        check("layers: none at first; a frame on no layer", D::layers(d).empty() && F(a).layer.empty() && d.active_layer().empty());
        const auto top = D::add_layer(j, d);
        const auto ls = D::layers(d);
        check("new layer: the first makes two -- Layer 1 (everything) under Layer 2, active",
              ls.size() == 2 && ls.back() == top && d.as<core::Layer>(ls[0])->name == "Layer 1" && d.as<core::Layer>(top)->name == "Layer 2" &&
                  d.active_layer() == top && D::frames_on_layer(d, ls[0]) == static_cast<int>(d.ids_of<core::Frame>().size()));
        const auto base = ls[0];
        j.undo();
        check("new layer: one step", D::layers(d).empty());
        j.redo();
        d.set_active_layer(top);
        const auto b = D::add_frame(j, d, page, core::FrameKind::Shape, {60, 60, 100, 100});
        check("a new frame: on the active layer", F(b).layer == top);
        // c drawn later, on the bottom layer: still under b
        d.set_active_layer(base);
        const auto c = D::add_frame(j, d, page, core::FrameKind::Shape, {80, 80, 100, 100});
        const auto on = d.frames_on(page);
        const auto at = [&](const std::string& id) { return std::find(on.begin(), on.end(), id) - on.begin(); };
        check("order: by layer, then z -- the top layer's frame over a later one below", F(c).z > F(b).z && at(c) < at(b) && at(a) < at(c));
        const auto pl = core::layout_pages(d, {page});
        const double px = pl.front().r.x + 100, py = pl.front().r.y + 100;
        check("picked: the top layer's first", core::hit_frame(d, pl, px, py, 0) == b);
        check("moved down a layer -- the later frame now on top", D::move_layer(j, d, top, 0) && core::hit_frame(d, pl, px, py, 0) == c);
        j.undo();
        check("lock a layer: its frames not picked (the one under is)", D::edit_layer(j, d, top, "Lock Layer", [](core::Layer& l) { l.locked = true; }) &&
                                                                        core::hit_frame(d, pl, px, py, 0) == c && D::frame_held(d, F(b)));
        j.undo();
        check("hide a layer: not shown, not picked, not printed", D::edit_layer(j, d, top, "Hide Layer", [](core::Layer& l) { l.visible = false; }) &&
                                                                     !D::frame_shown(d, F(b)) && core::hit_frame(d, pl, px, py, 0) == c && !D::frame_prints(d, F(b)));
        j.undo();
        check("a non-printing layer: shown, picked, not printed", D::edit_layer(j, d, top, "Layer Options", [](core::Layer& l) { l.print = false; }) &&
                                                                     D::frame_shown(d, F(b)) && core::hit_frame(d, pl, px, py, 0) == b && !D::frame_prints(d, F(b)));
        {   // the print PDF leaves it out
            namespace fs = std::filesystem;
            const auto dir = fs::temp_directory_path() / "composix_selftest_layers";
            std::error_code ec;
            fs::create_directories(dir, ec);
            L::PdfOptions opt;
            opt.compress = false;
            opt.only = {page};
            std::string err;
            const auto read = [](const std::string& f) { std::ifstream in(f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
            // b gets a fill only it has: its colour in the PDF tells whether it was drawn
            D::edit_frames(j, d, {b}, "Fill", [&](core::Frame& f) { f.fill = d.ids_of<core::Swatch>().back(); });
            const auto f1 = (dir / "a.pdf").string(), f2 = (dir / "b.pdf").string();
            L::export_print_pdf(d, f1, opt, {}, nullptr, &err);
            D::edit_layer(j, d, top, "Layer Options", [](core::Layer& l) { l.print = true; });
            L::export_print_pdf(d, f2, opt, {}, nullptr, &err);
            const auto p1 = read(f1), p2 = read(f2);
            check("a non-printing layer: its frame out of the PDF (the printing one's in)", !p1.empty() && p2.size() > p1.size(),
                  std::to_string(p1.size()) + " vs " + std::to_string(p2.size()));
            fs::remove_all(dir, ec);
        }
        check("move to a layer (one step)", D::move_to_layer(j, d, {a}, top) == 1 && F(a).layer == top);
        {
            core::Doc e;
            check("layers: saved", core::from_json(e, core::to_json(d)) && D::layers(e) == D::layers(d) && e.as<core::Frame>(a)->layer == top &&
                                       *e.as<core::Layer>(top) == *d.as<core::Layer>(top));
        }
        const auto n = d.ids_of<core::Frame>().size();
        check("delete a layer, keeping its frames -- to the one under", D::delete_layer(j, d, top, true) && d.as<core::Frame>(a) &&
                                                                         F(a).layer == base && d.ids_of<core::Frame>().size() == n);
        j.undo();
        check("delete a layer -- its frames with it (one step)", D::delete_layer(j, d, top) && !d.as<core::Frame>(a) && !d.as<core::Frame>(b) &&
                                                                  d.as<core::Frame>(c) && !d.as<core::Layer>(top));
        j.undo();
        check("... undone: the layer and its frames back", d.as<core::Layer>(top) && d.as<core::Frame>(a) && d.as<core::Frame>(b));
        check("the last layer can't go", D::delete_layer(j, d, top) && !D::delete_layer(j, d, base));
    }
    // -- s306: spell check ---------------------------------------------------
    {
        namespace S = core::spell;
        std::string got;
        for (const auto& w : S::words_of("Don't stop—the harbor’s 3rd ferry at www.x.org, e-mail ann@x.org", 2)) got += w.text + "|";
        check("words: apostrophes inside, dashes / hyphens part them, digits and addresses left alone",
              got == "Don't|stop|the|harbor’s|ferry|at|e|mail|", got);
        const auto ws = S::words_of("ab" + std::string(core::kMarkAnchor) + "cd 'quoted'", 0);
        check("words: a mark parts them; quotes round a word not in it", ws.size() == 3 && ws[0].text == "ab" && ws[1].text == "cd" && ws[2].text == "quoted" &&
                                                                      ws[1].from == 5 && ws[1].to == 7);
        std::string err;
        if (!S::built_in() || !S::load("en_US", &err)) {
            check("spell: no en_US dictionary here -- the rest skipped (every word reads fine)", S::good("harbr"), err);
        } else {
            check("spell: en_US loaded", S::loaded() && S::language() == "en_US");
            check("spell: right and wrong", S::good("harbor") && S::good("harbor’s") && S::good("Don't") && !S::good("harbr") && !S::good("smeled"));
            const auto sg = S::suggest("harbr");
            check("spell: suggestions", std::find(sg.begin(), sg.end(), "harbor") != sg.end(), sg.empty() ? "none" : sg.front());
            core::Story st{"t", {{"s", "The harbr smelled of oranges."}, {"s", "Nothing wrong here."}, {"s", "Composix sets tipe."}}};
            auto ms = S::misses(st);
            got.clear();
            for (const auto& m : ms) got += m.text + "@" + std::to_string(m.para) + ":" + std::to_string(m.from) + "|";
            check("spell: a story's misses, in order, where they are", got == "harbr@0:4|Composix@2:0|tipe@2:14|", got);
            namespace fs = std::filesystem;
            const auto dir = fs::temp_directory_path() / "composix_selftest_spell";
            std::error_code ec;
            fs::remove_all(dir, ec);
            S::set_user_file((dir / "words.txt").string());
            check("spell: a word added -- good now, kept in the user's file", S::add_word("Composix") && S::good("Composix") && fs::exists(dir / "words.txt"));
            S::ignore("tipe");
            check("spell: Ignore All", S::misses(st).size() == 1);
            S::clear_ignored();
            const auto nm = S::next_miss(st, {0, 6});
            const auto wrapped = S::next_miss(st, {2, 19});
            check("spell: the next miss after the caret; past the end, round to the start", nm.text == "tipe" && wrapped.text == "harbr", nm.text + " / " + wrapped.text);
            check("spell: the word removed again", S::remove_word("Composix") && !S::good("Composix"));
            S::set_user_file({});
            fs::remove_all(dir, ec);
        }
    }
    // -- s307: spelling put right ---------------------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("language: set, saved", D::set_language(j, d, "en_GB") && d.settings().language == "en_GB" &&
                                          [&] { core::Doc e; return core::from_json(e, core::to_json(d)) && e.settings().language == "en_GB"; }());
        check("replace a word -- only while it still reads so", !D::replace_word(j, d, "story.1", 1, 4, 10, "harbr", "harbor") &&
                                                                 D::replace_word(j, d, "story.1", 1, 4, 10, "harbor", "harbour") &&
                                                                 d.as<core::Story>("story.1")->paras[1].text.rfind("The harbour smelled", 0) == 0);
        j.undo();
        check("replace: one step", d.as<core::Story>("story.1")->paras[1].text.rfind("The harbor smelled", 0) == 0);
        int before = 0;
        for (const auto& sid : d.ids_of<core::Story>())
            for (const auto& p : d.as<core::Story>(sid)->paras)
                for (const auto& w : core::spell::words_of(p.text)) before += w.text == "the" ? 1 : 0;
        const int n = D::change_all_words(j, d, "the", "thee");
        int after = 0, other = 0;
        for (const auto& sid : d.ids_of<core::Story>())
            for (const auto& p : d.as<core::Story>(sid)->paras)
                for (const auto& w : core::spell::words_of(p.text)) { after += w.text == "thee" ? 1 : 0; other += w.text == "The" ? 1 : 0; }
        check("Change All: every whole word, case as typed (The kept), one step", n == before && n > 2 && after == n && other > 0 &&
                                                                                   d.as<core::Story>("story.1")->paras[1].text.find("thee") != std::string::npos,
              std::to_string(n) + " of " + std::to_string(before));
        j.undo();
        int back = 0;
        for (const auto& sid : d.ids_of<core::Story>())
            for (const auto& p : d.as<core::Story>(sid)->paras)
                for (const auto& w : core::spell::words_of(p.text)) back += w.text == "thee" ? 1 : 0;
        check("Change All: undone in one", back == 0);
        if (core::spell::loaded()) {
            const auto g = core::spell::generation();
            const auto m1 = core::spell::para_misses("A harbr and a harbr.", 3);
            const auto m2 = core::spell::para_misses("A harbr and a harbr.", 5);
            check("para misses: remembered by the words, its paragraph on each", m1.size() == 2 && m2.size() == 2 && m1[0].para == 3 && m2[0].para == 5 &&
                                                                                 core::spell::generation() == g);
            core::spell::ignore("harbr");
            check("para misses: Ignore All moves the generation and forgets", core::spell::generation() != g &&
                                                                               core::spell::para_misses("A harbr and a harbr.", 0).empty());
            core::spell::clear_ignored();
        }
    }
    // -- s308: the Layers card's items, a frame's Layer row ------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("layers card: none until one is made", core::project(d, "").in(core::Segment::Layers).empty());
        const auto top = D::add_layer(j, d, "Notes");
        const auto ls = D::layers(d);
        D::edit_layer(j, d, ls.front(), "Hide Layer", [](core::Layer& l) { l.visible = false; });
        const auto o = core::project(d, "");
        const auto items = o.in(core::Segment::Layers);
        const auto fv = [](const core::Item& it, const std::string& label) {
            for (const auto& f : it.fields) if (f.label == label) return f.value;
            return std::string("?");
        };
        check("layers card: top first; the active one marked; hidden said", items.size() == 2 && items[0]->id == top && items[0]->badge == "Active" &&
                                                                             items[1]->badge == "Hidden" && fv(*items[1], "Visible") == "Hidden" &&
                                                                             fv(*items[0], "Prints") == "Yes",
              items.empty() ? "none" : items[0]->badge + "/" + items.back()->badge);
        const auto fi = core::frame_items(d, {"frame.1"}, "");
        const auto row = std::find_if(fi.front().fields.begin(), fi.front().fields.end(), [](const core::Field& f) { return f.edit == "flayer"; });
        check("a frame's Layer row -- its layer, the others to move it to (top first)", row != fi.front().fields.end() &&
                                                                                       row->value == "Layer 1" && row->choices.size() == 2 &&
                                                                                       row->choices.front().id == top);
    }
    // -- s309: the screen's marks -- misspelled words, links ------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::replace_word(j, d, "story.1", 1, 11, 18, "smelled", "smeled");
        D::set_link(j, d, "story.1", {1, 4}, {1, 10}, "www.example.com");
        L::FlowCache flows;
        const auto ms = L::text_marks(d, flows, "frame.1");
        int miss = 0, link = 0;
        for (const auto& m : ms) {
            if (m.kind == L::TextMark::Link && m.para == 1 && m.from == 4 && m.to == 10 && m.box.w > 0) ++link;
            if (m.kind == L::TextMark::Misspelled && m.para == 1 && m.from == 11 && m.to == 17) ++miss;
        }
        check("marks: the link's words boxed", link >= 1, std::to_string(ms.size()) + " marks");
        check("marks: the misspelled word boxed (with a dictionary)", !core::spell::loaded() || miss >= 1);
        check("marks: none asked for, none given", L::text_marks(d, flows, "frame.1", false, false).empty());
        check("marks: not a text frame -- none", L::text_marks(d, flows, "frame.4", true, true).empty() || d.as<core::Frame>("frame.4")->kind == core::FrameKind::Text);
    }
    // -- s310: Check Spelling through the book --------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto order = D::stories_in_order(d);
        check("stories in order: every story once, the book's text first", order.size() == d.ids_of<core::Story>().size() && !order.empty());
        if (core::spell::loaded()) {
            for (const auto& sid : d.ids_of<core::Story>()) core::spell::misses(*d.as<core::Story>(sid));   // (the sample may have its own odd words)
            const auto c0 = D::misspellings(d);
            D::replace_word(j, d, "story.1", 1, 11, 18, "smelled", "smeled");
            const auto st2 = order.size() > 1 ? order[1] : order[0];
            D::edit_text(j, d, st2, {0, 0}, {0, 0}, "Qwzx ");
            const auto c1 = D::misspellings(d);
            check("count: two more words", c1.words == c0.words + 2, std::to_string(c0.words) + " -> " + std::to_string(c1.words));
            // walk from the start of the first story: every miss once, then round to the start
            std::vector<std::string> seen;
            auto h = D::next_misspelling(d, order.front(), {0, 0});
            const auto first = h;
            for (int guard = 0; !h.story.empty() && guard < 200; ++guard) {
                seen.push_back(h.word);
                h = D::next_misspelling(d, h.story, {h.para, h.to});
                if (h == first) break;
            }
            check("walk: each miss once, round to the first", static_cast<int>(seen.size()) == c1.words &&
                                                                std::find(seen.begin(), seen.end(), "smeled") != seen.end() &&
                                                                std::find(seen.begin(), seen.end(), "Qwzx") != seen.end(),
                  std::to_string(seen.size()) + " of " + std::to_string(c1.words));
            const auto in = D::next_misspelling(d, "story.1", {1, 13});   // the caret inside "smeled": past it
            check("walk: the word the caret is in skipped", in.word != "smeled", in.word);
        }
    }
    // -- s311: links to headings and pages ------------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto places = D::link_places(d);
        const auto two = std::find_if(places.begin(), places.end(), [](const D::LinkPlace& p) { return p.words == "Chapter Two"; });
        check("places: the headings, in reading order", two != places.end() && places.front().words == "Chapter One",
              places.empty() ? "none" : places.front().words);
        const auto before = d.as<core::Story>("story.1")->paras[two->para].text;
        check("link to a heading: an anchor made at its start, one step", D::link_to_paragraph(j, d, "story.1", {1, 4}, {1, 10}, "story.1", two->para) &&
                                                                          d.as<core::Story>("story.1")->paras[two->para].text.rfind(core::kMarkAnchor, 0) == 0);
        auto ls = D::hyperlinks(d);
        check("... the words link to it, not broken; where: its words", ls.size() == 1 && ls[0].words == "harbor" && !ls[0].broken &&
                                                                        D::link_where(d, ls[0].target) == "“Chapter Two”",
              ls.empty() ? "none" : D::link_where(d, ls[0].target));
        check("a second link to it: the same anchor", D::link_to_paragraph(j, d, "story.1", {2, 0}, {2, 6}, "story.1", two->para) &&
                                                      D::hyperlinks(d).size() == 2 && D::hyperlinks(d)[0].target == D::hyperlinks(d)[1].target);
        j.undo();
        j.undo();
        check("undone: the anchor gone with it", d.as<core::Story>("story.1")->paras[two->para].text == before && D::hyperlinks(d).empty());
        const auto pg = d.pages().back();
        check("link to a page", D::set_link(j, d, "story.1", {1, 4}, {1, 10}, D::page_link(pg)) && D::hyperlinks(d)[0].target == "page:" + pg &&
                                    D::link_where(d, "page:" + pg) == "Page " + d.folio(pg));
        {
            L::FlowCache flows;
            const auto pl = L::hyperlink_links(d, flows);
            check("... in the PDF: to that page's top", !pl.empty() && pl.front().to == pg && pl.front().uri.empty() && pl.front().to_y == 0);
        }
        D::remove_pages(j, d, {pg});
        check("... the page gone: broken", D::hyperlinks(d)[0].broken && D::link_where(d, "page:" + pg).rfind("Gone", 0) == 0);
    }
    // -- s312: preflight -- broken links, layers left out, misspellings -------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const auto kinds = [&](const std::string& k) {
            int n = 0;
            for (const auto& i : L::preflight_issues(d, flows, {})) n += i.kind == k ? 1 : 0;
            return n;
        };
        check("preflight: none of these at first", kinds("link") == 0 && kinds("layer") == 0);
        D::set_link(j, d, "story.1", {1, 4}, {1, 10}, "#anchor.none");
        const auto all = L::preflight_issues(d, flows, {});
        const auto li = std::find_if(all.begin(), all.end(), [](const L::Issue& i) { return i.kind == "link"; });
        check("preflight: a link going nowhere -- a warning at its page", li != all.end() && li->level == L::Issue::Level::Warning && !li->page.empty() &&
                                                                         li->text.find("harbor") != std::string::npos,
              li == all.end() ? "none" : li->text);
        const auto ly = D::add_layer(j, d);
        const auto f = D::add_frame(j, d, d.pages().back(), core::FrameKind::Image, {40, 40, 100, 100});   // an empty picture frame: Info
        const int empty_before = kinds("empty");
        D::edit_layer(j, d, ly, "Hide Layer", [](core::Layer& l) { l.visible = false; });
        check("preflight: a hidden layer said; its frames not checked", kinds("layer") == 1 && kinds("empty") == empty_before - 1 && d.as<core::Frame>(f));
        if (core::spell::loaded()) {
            D::replace_word(j, d, "story.1", 1, 11, 18, "smelled", "smeled");
            check("preflight: misspelled words counted (info)", kinds("spelling") == 1);
        }
    }
    // -- s314: tables -- the model --------------------------------------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto n0 = d.as<core::Story>("story.1")->paras.size();
        auto t = D::make_table(3, 4, 288);
        check("make: 3 rows of 4, equal columns", t.rows.size() == 3 && D::table_cols(t) == 4 && t.widths[0] == 72 && t.rows[2].cells.size() == 4);
        const int p = D::insert_table(j, d, "story.1", {1, 10}, t);   // mid-paragraph: split round it
        const auto& st = *d.as<core::Story>("story.1");
        check("insert mid-paragraph: its own paragraph, the words either side kept", p == 2 && st.paras.size() == n0 + 2 &&
                                                                                    st.paras[2].text == core::kMarkTable && st.paras[1].text == "The harbor" &&
                                                                                    st.paras[3].text.rfind(" smelled", 0) == 0 && D::table_at(d, "story.1", 2));
        j.undo();
        check("insert: one step", d.as<core::Story>("story.1")->paras.size() == n0);
        const int pe = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, t);
        check("insert at a paragraph's end: no empty one left over", pe == 2 && d.as<core::Story>("story.1")->paras.size() == n0 + 1);
        check("cell text: lines a paragraph each", D::set_cell_text(j, d, "story.1", pe, 1, 2, "Harbor fees\nper ton") &&
                                                   D::table_at(d, "story.1", pe)->rows[1].cells[2].paras.size() == 2 &&
                                                   D::cell_text(D::table_at(d, "story.1", pe)->rows[1].cells[2]) == "Harbor fees\nper ton");
        check("rows / columns added and taken (one step each), the width kept", D::edit_table(j, d, "story.1", pe, "Insert Column", [](core::Table& x) {
                                                                                   core::doc::table_insert_col(x, 1);
                                                                                   core::doc::table_insert_row(x, 0);
                                                                               }) &&
                                                                               D::table_cols(*D::table_at(d, "story.1", pe)) == 5 &&
                                                                               D::table_at(d, "story.1", pe)->rows.size() == 4 &&
                                                                               std::abs([&] { double w = 0; for (double x : D::table_at(d, "story.1", pe)->widths) w += x; return w; }() - 288) < 1e-6 &&
                                                                               D::cell_text(D::table_at(d, "story.1", pe)->rows[2].cells[3]) == "Harbor fees\nper ton");
        {
            core::Doc e;
            check("table: saved", core::from_json(e, core::to_json(d)) && *D::table_at(e, "story.1", pe) == *D::table_at(d, "story.1", pe));
        }
        // typing beside it, deleting it
        const auto copy = *D::table_at(d, "story.1", pe);
        D::edit_text(j, d, "story.1", {1, 0}, {1, 3}, "A");   // words before it change -- the table rides along
        check("edits round it: the table kept", D::table_at(d, "story.1", pe) && *D::table_at(d, "story.1", pe) == copy);
        D::edit_text(j, d, "story.1", {pe, 0}, {pe, 3}, "");
        check("its mark deleted: the table gone", !D::table_at(d, "story.1", pe));
        j.undo();
        check("... undone: back", D::table_at(d, "story.1", pe) && *D::table_at(d, "story.1", pe) == copy);
    }
    // -- s315 / s316: a table flowed, broken between rows, drawn ---------------
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto t = D::make_table(40, 3, 288);
        t.header_rows = 1;
        D::set_cell_words(t.rows[0].cells[0], "Port");
        D::set_cell_words(t.rows[0].cells[1], "Fee");
        D::set_cell_words(t.rows[5].cells[2], "A long note in a narrow cell that runs over several lines of words");
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, t);
        const auto geo = L::table_geo(d, d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)], 200);
        check("geo: narrowed to fit, a taller row for the long words", std::abs(geo.width - 200) < 0.01 && geo.row_h.size() == 40 &&
                                                                       geo.row_h[5] > geo.row_h[4] * 1.5 && geo.row_h[4] > 8,
              std::to_string(geo.row_h[4]) + " / " + std::to_string(geo.row_h[5]));
        const auto fl = L::flow_story(d, "story.1");
        std::vector<std::pair<std::string, int>> rows;   // frame, row
        for (const auto& ft : fl.frames)
            for (const auto& l : ft.lines)
                if (l.para == p) rows.emplace_back(ft.frame, l.row);
        std::set<std::string> frames;
        for (const auto& r : rows) frames.insert(r.first);
        int body = 0, heads = 0;
        for (const auto& r : rows) { body += r.second > 0; heads += r.second == 0; }
        check("flow: its rows as lines, each row once (headers again), on into the next frame", body == 39 && frames.size() >= 2 &&
                                                                                                 heads == static_cast<int>(frames.size()) &&
                                                                                                 std::all_of(rows.begin(), rows.end(), [](const auto& r) { return r.second >= 0; }),
              std::to_string(body) + " body, " + std::to_string(heads) + " heads, " + std::to_string(frames.size()) + " frames");
        // the words after it follow on after its last row
        int after_frame = -1, last_row_frame = -1;
        for (int fi = 0; fi < static_cast<int>(fl.frames.size()); ++fi)
            for (const auto& l : fl.frames[static_cast<std::size_t>(fi)].lines) {
                if (l.para == p && l.row == 39) last_row_frame = fi;
                if (l.para == p + 1 && after_frame < 0) after_frame = fi;
            }
        check("flow: the next paragraph after the table's last row", after_frame >= last_row_frame && last_row_frame >= 0);
        L::FlowCache flows;
        int fills = 0, texts = 0, rules = 0;
        std::string fr;
        for (const auto& r : rows) if (r.second == 5) fr = r.first;
        L::visit_tables(d, "story.1", flows.get(d, "story.1"), fr, [&](const L::TablePiece& tp) {
            fills += tp.kind == L::TablePiece::Fill;
            texts += tp.kind == L::TablePiece::Text;
            rules += tp.kind == L::TablePiece::Rule;
        });
        check("draw: the words line by line, the grid", texts >= 4 && rules > 10 && fills == 0, std::to_string(texts) + " lines, " + std::to_string(rules) + " rules");
        D::edit_table(j, d, "story.1", p, "Header Fill", [&](core::Table& x) { x.header_fill = d.ids_of<core::Swatch>().front(); });
        fills = 0;
        L::visit_tables(d, "story.1", flows.get(d, "story.1"), fr, [&](const L::TablePiece& tp) { fills += tp.kind == L::TablePiece::Fill; });
        check("draw: the header row filled (each frame's)", fills == 3);
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "composix_selftest_table";
        std::error_code ec;
        fs::create_directories(dir, ec);
        L::PdfOptions opt;
        opt.compress = false;
        std::string err;
        const bool ok = L::export_print_pdf(d, (dir / "t.pdf").string(), opt, {}, nullptr, &err) &&
                        L::export_pdf(d, (dir / "s.pdf").string(), opt, {}, nullptr, &err);
        check("PDF: both writers take a table", ok, err);
        if (std::getenv("COMPOSIX_KEEP_TABLE_PDF")) fs::copy_file(dir / "t.pdf", "/tmp/table_print.pdf", fs::copy_options::overwrite_existing, ec);
        fs::remove_all(dir, ec);
    }
    // -- s317: an ODT's tables come in ------------------------------------------
    {
        std::cout << "--- s317 an ODT's table ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f" xmlns:table="tb")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:text-properties fo:font-size="10pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="6in" fo:page-height="9in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        const std::string content = "<office:document-content" + ns + R"(><office:automatic-styles>
 <style:style style:name="T1.A" style:family="table-column"><style:table-column-properties style:column-width="1.5in"/></style:style>
 <style:style style:name="T1.B" style:family="table-column"><style:table-column-properties style:column-width="1in"/></style:style>
 <style:style style:name="T1.H" style:family="table-cell"><style:table-cell-properties fo:background-color="#cccccc"/></style:style>
</office:automatic-styles><office:body><office:text>)"
            R"(<text:p text:style-name="Standard">Before.</text:p>)"
            R"(<table:table table:name="T1"><table:table-column table:style-name="T1.A"/><table:table-column table:style-name="T1.B" table:number-columns-repeated="2"/>)"
            R"(<table:table-header-rows><table:table-row><table:table-cell table:style-name="T1.H"><text:p text:style-name="Standard">Port</text:p></table:table-cell>)"
            R"(<table:table-cell table:style-name="T1.H"><text:p text:style-name="Standard">Fee</text:p></table:table-cell><table:table-cell table:style-name="T1.H"><text:p text:style-name="Standard">Days</text:p></table:table-cell></table:table-row></table:table-header-rows>)"
            R"(<table:table-row><table:table-cell><text:p text:style-name="Standard">Harbor</text:p><text:p text:style-name="Standard">(north)</text:p></table:table-cell>)"
            R"(<table:table-cell><text:p text:style-name="Standard">12</text:p></table:table-cell><table:table-cell><text:p text:style-name="Standard">3</text:p></table:table-cell></table:table-row>)"
            R"(</table:table>)"
            R"(<text:p text:style-name="Standard">After.</text:p>)"
            "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        check("read: the table between the paragraphs -- 3 columns (widths), a header row, 2 rows", book && book->body.size() == 3 &&
                                                                                                    book->body[1].tables.size() == 1 &&
                                                                                                    book->body[1].tables[0].widths.size() == 3 &&
                                                                                                    std::abs(book->body[1].tables[0].widths[0] - 108) < 0.01 &&
                                                                                                    std::abs(book->body[1].tables[0].widths[2] - 72) < 0.01 &&
                                                                                                    book->body[1].tables[0].header_rows == 1 &&
                                                                                                    book->body[1].tables[0].rows.size() == 2, err);
        core::Doc d;
        L::OdtInfo info;
        const bool ok = book && L::import_odt(d, *book, &info, {}, &err);
        const auto* t = ok ? core::doc::table_at(d, "story.1", 1) : nullptr;
        check("import: a table -- its words, its header's fill, nothing said left out", t && t->rows.size() == 2 && t->header_rows == 1 &&
                                                                                       core::doc::cell_text(t->rows[1].cells[0]) == "Harbor\n(north)" &&
                                                                                       core::doc::cell_text(t->rows[0].cells[2]) == "Days" &&
                                                                                       d.as<core::Swatch>(t->rows[0].cells[0].fill) && info.tables_kept == 1 &&
                                                                                       std::none_of(info.notes_said.begin(), info.notes_said.end(), [](const std::string& x) { return x.find("table") != std::string::npos; }),
              err);
        const auto fl = L::flow_story(d, "story.1");
        int rows = 0;
        for (const auto& ft : fl.frames) for (const auto& l : ft.lines) rows += l.row >= 0;
        check("import: its rows flowed", rows == 2);
    }
    // -- s319: merged cells ------------------------------------------------------
    {
        namespace D = core::doc;
        auto t = D::make_table(4, 3, 240);
        D::set_cell_words(t.rows[1].cells[1], "kept");
        D::set_cell_words(t.rows[1].cells[2], "joined");
        check("merge: 2 across -- the covered cell's words joined in", D::table_merge(t, 1, 1, 2, 1) && t.rows[1].cells[1].span_cols == 2 &&
                                                                       D::cell_text(t.rows[1].cells[1]) == "kept\njoined" && t.rows[1].cells[2].paras.empty());
        check("owner: a covered cell's is the merge's", D::owner_of(t, 1, 2) == std::pair<int, int>{1, 1} && D::owner_of(t, 1, 0) == std::pair<int, int>{1, 0});
        check("merge down over it: the old merge undone first", D::table_merge(t, 0, 2, 1, 3) && t.rows[1].cells[1].span_cols == 1 &&
                                                                 D::owner_of(t, 2, 2) == std::pair<int, int>{0, 2});
        check("unmerge", D::table_unmerge(t, 0, 2) && D::owner_of(t, 2, 2) == std::pair<int, int>{2, 2});
        check("a merge kept inside the table when a column goes", D::table_merge(t, 0, 1, 2, 1) && [&] { D::table_delete_col(t, 2); return t.rows[0].cells[1].span_cols == 1; }());
        // flowed and drawn: a cell over 3 rows with many words -- its last row taller, no break inside it
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto u = D::make_table(30, 3, 240);
        D::set_cell_words(u.rows[2].cells[0], "Many words in a merged cell that runs down three rows and needs more room than they give it");
        D::table_merge(u, 2, 0, 1, 3);
        D::table_merge(u, 6, 1, 2, 1);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, u);
        const auto geo = L::table_geo(d, d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)], 240);
        check("geo: the merged rows together as tall as its words; no break inside", geo.row_h[4] > geo.row_h[3] + 1 &&
                                                                                    !geo.break_before[3] && !geo.break_before[4] && geo.break_before[5],
              std::to_string(geo.row_h[3]) + " / " + std::to_string(geo.row_h[4]));
        L::FlowCache flows;
        const auto& fl = flows.get(d, "story.1");
        std::string fr6;
        bool split_merge = false;
        std::map<int, std::string> at;
        for (const auto& ft : fl.frames) for (const auto& l : ft.lines) if (l.para == p && l.row >= 0) at[l.row] = ft.frame;
        split_merge = at[2] != at[3] || at[3] != at[4];
        check("flow: the merged rows in one frame", !split_merge);
        int verticals = 0, wide_fill = 0;
        L::visit_tables(d, "story.1", fl, at[6], [&](const L::TablePiece& tp) {
            if (tp.row == 6 && tp.kind == L::TablePiece::Rule && tp.r.w < 2) ++verticals;
        });
        (void)wide_fill;
        check("draw: no grid line inside a merge across (row 6: 3 edges, not 4)", verticals == 3, std::to_string(verticals));
        int words = 0;
        double top = 1e9;
        L::visit_tables(d, "story.1", fl, at[2], [&](const L::TablePiece& tp) {
            if (tp.kind == L::TablePiece::Text && tp.row == 2 && tp.col == 0) { ++words; top = std::min(top, tp.baseline); }
        });
        check("draw: the merged cell's words over its rows", words >= 4, std::to_string(words));
    }
    // -- s320: a table's cell found by a point; column widths --------------------
    {
        namespace D = core::doc;
        auto t = D::make_table(3, 3, 240);
        check("width: the next column gives -- the table as wide", D::table_set_width(t, 0, 100) && t.widths[0] == 100 && std::abs(t.widths[1] - 60) < 1e-6 &&
                                                                   std::abs(t.widths[0] + t.widths[1] + t.widths[2] - 240) < 1e-6);
        check("width: at least 12, the neighbour too", D::table_set_width(t, 0, 500) && std::abs(t.widths[1] - 12) < 1e-6 && !D::table_set_width(t, 7, 50));
        check("width: the last column -- the table grows", D::table_set_width(t, 2, 120) && t.widths[2] == 120);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto u = D::make_table(3, 3, 240);
        D::table_merge(u, 1, 1, 2, 1);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, u);
        L::FlowCache flows;
        const auto& fl = flows.get(d, "story.1");
        const L::Line* row1 = nullptr;
        std::string fr;
        for (const auto& ft : fl.frames) for (const auto& l : ft.lines) if (l.para == p && l.row == 1) { row1 = &l; fr = ft.frame; }
        const auto h = row1 ? L::table_hit(d, "story.1", fl, fr, row1->x + 200, row1->y + row1->h / 2, 2) : L::TableHit{};
        check("hit: in the merge -- its own cell, its width", h.ok() && h.para == p && h.row == 1 && h.col == 1 && std::abs(h.cell.w - 160) < 0.5,
              std::to_string(h.row) + "," + std::to_string(h.col) + " w " + std::to_string(h.cell.w));
        const auto e = row1 ? L::table_hit(d, "story.1", fl, fr, row1->x + 80.5, row1->y + 2, 2) : L::TableHit{};
        check("hit: an edge near", e.ok() && e.edge == 1);
        check("hit: off the table -- none", row1 && !L::table_hit(d, "story.1", fl, fr, row1->x + 120, row1->y - 50, 2).ok());
    }
    // -- s321: a cell's words top / middle / bottom; the table left / centre / right
    {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto u = D::make_table(2, 2, 160);
        D::set_cell_words(u.rows[0].cells[0], "A tall cell of words that need several lines here");
        D::set_cell_words(u.rows[0].cells[1], "Short");
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, u);
        const auto base = [&](int valign) {
            D::edit_table(j, d, "story.1", p, "Align", [&](core::Table& t) { t.rows[0].cells[1].valign = valign; });
            L::FlowCache flows;
            const auto& fl = flows.get(d, "story.1");
            double b = -1;
            for (const auto& ft : fl.frames)
                L::visit_tables(d, "story.1", fl, ft.frame, [&](const L::TablePiece& tp) {
                    if (tp.kind == L::TablePiece::Text && tp.row == 0 && tp.col == 1) b = tp.baseline;
                });
            return b;
        };
        const double top = base(0), mid = base(1), bot = base(2);
        check("valign: top < middle < bottom", top > 0 && mid > top + 1 && bot > mid + 1,
              std::to_string(top) + " / " + std::to_string(mid) + " / " + std::to_string(bot));
        const auto left_of = [&](int align) {
            D::edit_table(j, d, "story.1", p, "Align", [&](core::Table& t) { t.align = align; });
            const auto fl = L::flow_story(d, "story.1");
            for (const auto& ft : fl.frames) for (const auto& l : ft.lines) if (l.para == p && l.row == 0) return l.x;
            return -1.0;
        };
        const double l0 = left_of(0), l1 = left_of(1), l2 = left_of(2);
        check("align: the table centred, then right, in its column", l1 > l0 + 1 && l2 > l1 + 1 && std::abs((l2 - l0) - 2 * (l1 - l0)) < 0.5,
              std::to_string(l0) + " / " + std::to_string(l1) + " / " + std::to_string(l2));
        core::Doc e;
        check("valign / align: saved", core::from_json(e, core::to_json(d)) && D::table_at(e, "story.1", p)->align == 2 &&
                                           D::table_at(e, "story.1", p)->rows[0].cells[1].valign == 2);
    }
    // -- s322: spelling in a table's cells -----------------------------------------
    if (core::spell::loaded()) {
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto u = D::make_table(2, 2, 160);
        D::set_cell_words(u.rows[0].cells[1], "Harbr fees");
        D::set_cell_words(u.rows[1].cells[0], "per tonn | and qwzx");
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, u);
        D::set_cell_text(j, d, "story.1", p, 1, 0, "per tonn\nand qwzx");
        const auto c = D::misspellings(d);
        auto h = D::next_misspelling(d, "story.1", {p, 0});
        check("cells: counted; the walk comes to the first cell's word", c.words >= 3 && h.row == 0 && h.col == 1 && h.word == "Harbr", h.word);
        const auto h2 = D::next_misspelling(d, "story.1", {p, 0}, &h);
        check("cells: on to the next cell's (its paragraph)", h2.row == 1 && h2.col == 0 && h2.word == "tonn", h2.word);
        const auto h3 = D::next_misspelling(d, "story.1", {p, 0}, &h2);
        check("cells: a cell's second paragraph", h3.row == 1 && h3.cpara == 1 && h3.word == "qwzx", h3.word);
        check("cells: put right, one step", D::replace_misspelling(j, d, h, "Harbor") &&
                                                D::cell_text(D::table_at(d, "story.1", p)->rows[0].cells[1]) == "Harbor fees");
        check("cells: Change All reaches them", D::change_all_words(j, d, "tonn", "ton") == 1 &&
                                                    D::cell_text(D::table_at(d, "story.1", p)->rows[1].cells[0]) == "per ton\nand qwzx");
        j.undo();
        check("cells: Change All undone", D::cell_text(D::table_at(d, "story.1", p)->rows[1].cells[0]) == "per tonn\nand qwzx");
    }
    // -- s324: text to a table, a table to text -----------------------------------
    {
        std::cout << "--- s324 text <-> table ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // three paragraphs of tab-separated words after paragraph 1
        const int at = static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size());
        D::edit_text(j, d, "story.1", {1, at}, {1, at}, "\nPort\tFee\tDays\nDover\t12\nCalais\t9\t3");
        const auto n0 = d.as<core::Story>("story.1")->paras.size();
        {   // bold on "Fee"
            core::Story s = *d.as<core::Story>("story.1");
            core::Run r;
            r.start = 5; r.end = 8; r.local.bold = 1;
            s.paras[2].runs = {r};
            j.run("bold", {"story.1"}, [&] { d.put("story.1", s); });
        }
        const auto before = core::to_json(d);
        const int p = D::text_to_table(j, d, "story.1", 2, 4, 240, 1);
        const auto* t = D::table_at(d, "story.1", p);
        check("to table: three paragraphs one, in their place", p == 2 && t && d.as<core::Story>("story.1")->paras.size() == n0 - 2);
        check("to table: a row each, the widest row's columns, the width shared", t && t->rows.size() == 3 && D::table_cols(*t) == 3 &&
                                                                                    std::abs(t->widths[0] - 80) < 1e-9 && t->header_rows == 1);
        check("to table: the tabs between cells; a short row's last cell empty", t && D::cell_text(t->rows[0].cells[1]) == "Fee" &&
                                                                                     D::cell_text(t->rows[1].cells[0]) == "Dover" &&
                                                                                     t->rows[1].cells[2].paras.empty() && D::cell_text(t->rows[2].cells[2]) == "3");
        check("to table: the words' formatting kept", t && t->rows[0].cells[1].paras.size() == 1 && t->rows[0].cells[1].paras[0].runs.size() == 1 &&
                                                          t->rows[0].cells[1].paras[0].runs[0].start == 0 && t->rows[0].cells[1].paras[0].runs[0].end == 3 &&
                                                          t->rows[0].cells[1].paras[0].runs[0].local.bold == 1);
        j.undo();
        check("to table: one step", core::to_json(d) == before);
        j.redo();
        // and back
        const auto as_table = core::to_json(d);
        D::edit_table(j, d, "story.1", p, "merge", [](core::Table& x) { D::table_merge(x, 1, 1, 2, 1); });   // "12" across two
        const int rows = D::table_to_text(j, d, "story.1", p);
        const auto& st = *d.as<core::Story>("story.1");
        check("to text: a paragraph a row, tabs between", rows == 3 && st.paras[2].text == "Port\tFee\tDays" && st.paras[3].text == "Dover\t12\t" &&
                                                            st.paras[4].text == "Calais\t9\t3" && st.paras.size() == n0);
        check("to text: the formatting kept", st.paras[2].runs.size() == 1 && st.paras[2].runs[0].start == 5 && st.paras[2].runs[0].end == 8);
        check("to text: no table left", !D::table_at(d, "story.1", 2) && !D::table_at(d, "story.1", 3));
        j.undo();
        j.undo();
        check("to text: one step", core::to_json(d) == as_table);
        // refused: a paragraph holding a note
        core::Doc e;
        core::DocJournal je{e};
        e.set_door([&je] { return je.open(); });
        core::load_sample(e);
        int with_mark = -1;
        const auto& es = *e.as<core::Story>("story.1");
        for (int k = 0; k < static_cast<int>(es.paras.size()) && with_mark < 0; ++k)
            if (!es.paras[static_cast<std::size_t>(k)].notes.empty() || !es.paras[static_cast<std::size_t>(k)].anchors.empty() ||
                !es.paras[static_cast<std::size_t>(k)].index.empty() || !es.paras[static_cast<std::size_t>(k)].refs.empty()) with_mark = k;
        if (with_mark < 0) {   // none in the sample: give one a note
            D::edit_text(je, e, "story.1", {1, 0}, {1, 0}, "x");
            core::Story s = *e.as<core::Story>("story.1");
            s.paras[1].text = std::string(core::kMarkNote) + s.paras[1].text;
            s.paras[1].notes.resize(1);
            je.run("note", {"story.1"}, [&] { e.put("story.1", s); });
            with_mark = 1;
        }
        const auto je0 = je.size();
        check("to table: refused over a note / reference / anchor / index entry", D::text_to_table(je, e, "story.1", with_mark, with_mark, 200) == -2 &&
                                                                                    je.size() == je0);
    }
    // -- s325: alternating rows; a table's colours are in use ------------------------
    {
        std::cout << "--- s325 bands ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto t = D::make_table(7, 2, 200);
        t.header_rows = 1;
        t.header_fill = "swatch.1";
        t.band_fill = "swatch.2";
        t.band_tint = 0.25;
        double k = 0;
        check("bands: a header row takes the header's", D::table_cell_fill(t, 0, 0, &k) == "swatch.1");
        check("bands: the body's first row plain, its second filled", D::table_cell_fill(t, 1, 0).empty() && D::table_cell_fill(t, 2, 1, &k) == "swatch.2" &&
                                                                         k == 0.25 && D::table_cell_fill(t, 3, 0).empty() && D::table_cell_fill(t, 4, 0) == "swatch.2");
        t.band_every = 2;
        check("bands: two rows a band", D::table_cell_fill(t, 1, 0).empty() && D::table_cell_fill(t, 2, 0).empty() &&
                                            D::table_cell_fill(t, 3, 0) == "swatch.2" && D::table_cell_fill(t, 4, 0) == "swatch.2" && D::table_cell_fill(t, 5, 0).empty());
        t.rows[3].cells[1].fill = "swatch.3";
        t.rows[3].cells[1].fill_tint = 0.5;
        check("bands: a cell's own fill over them", D::table_cell_fill(t, 3, 1, &k) == "swatch.3" && k == 0.5 && D::table_cell_fill(t, 3, 0) == "swatch.2");
        check("bands: off the table none", D::table_cell_fill(t, 9, 0).empty() && D::table_cell_fill(t, -1, 0).empty());
        {
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            const int p = D::insert_table(je, e, "story.1", {1, static_cast<int>(e.as<core::Story>("story.1")->paras[1].text.size())}, t);
            core::Doc f;
            check("bands: saved", core::from_json(f, core::to_json(e)) && *D::table_at(f, "story.1", p) == *D::table_at(e, "story.1", p) &&
                                      D::table_at(f, "story.1", p)->band_every == 2 && D::table_at(f, "story.1", p)->band_fill == "swatch.2");
        }
        // a swatch only a table uses is in use -- not "unused", not deleted
        core::Swatch only;
        only.name = "Table only";
        const auto sw = D::add_swatch(j, d, only);
        check("in use: a new swatch unused", D::swatch_uses(d, sw) == 0);
        auto u = D::make_table(2, 2, 160);
        u.band_fill = sw;
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, u);
        check("in use: a table's bands", D::swatch_uses(d, sw) == 1);
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& x) { x.band_fill.clear(); x.rows[1].cells[0].fill = sw; x.rule_color = sw; });
        check("in use: a cell's fill, the grid", D::swatch_uses(d, sw) == 2);
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& x) {
            x.rows[1].cells[0].fill.clear();
            x.rule_color.clear();
            core::Run r;
            r.start = 0; r.end = 1; r.local.color = sw;
            D::set_cell_words(x.rows[0].cells[0], "Ab");
            x.rows[0].cells[0].paras[0].runs = {r};
        });
        check("in use: a cell's coloured words", D::swatch_uses(d, sw) == 1);
        const auto un = D::unused_swatches(d);
        check("in use: not offered as unused, not deleted", std::find(un.begin(), un.end(), sw) == un.end() && !D::delete_swatch(j, d, sw));
    }
    // -- s326: footer rows -- at the foot of each piece ------------------------------
    {
        std::cout << "--- s326 footer rows ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto t = D::make_table(42, 3, 288);   // 1 header, 39 body, 2 footer
        t.header_rows = 1;
        t.footer_rows = 2;
        t.band_fill = "swatch.3";
        D::set_cell_words(t.rows[40].cells[0], "Carried");
        D::set_cell_words(t.rows[41].cells[0], "Total");
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, t);
        const auto fl = L::flow_story(d, "story.1");
        std::map<std::string, std::vector<int>> by_frame;   // in order
        std::vector<std::string> order;
        for (const auto& ft : fl.frames)
            for (const auto& l : ft.lines)
                if (l.para == p) {
                    if (!by_frame.count(ft.frame)) order.push_back(ft.frame);
                    by_frame[ft.frame].push_back(l.row);
                }
        bool tails = !order.empty(), heads = true;
        int body = 0;
        for (const auto& fr : order) {
            const auto& v = by_frame[fr];
            tails = tails && v.size() >= 4 && v[v.size() - 2] == 40 && v.back() == 41;
            heads = heads && v.front() == 0;
            for (int r : v) body += r >= 1 && r <= 39;
        }
        check("footer: broken over frames", order.size() >= 2, std::to_string(order.size()) + " frames");
        check("footer: each piece ends with the footer rows, starts with the header", tails && heads);
        check("footer: each body row once", body == 39, std::to_string(body));
        check("footer: not banded", D::table_cell_fill(*D::table_at(d, "story.1", p), 41, 0).empty() &&
                                        D::table_cell_fill(*D::table_at(d, "story.1", p), 40, 0).empty() &&
                                        !D::table_cell_fill(*D::table_at(d, "story.1", p), 39 - (39 % 2 == 0 ? 0 : 1), 0).empty());
        // rows added and taken round the footer
        auto u = *D::table_at(d, "story.1", p);
        D::table_insert_row(u, 41);   // inside it
        check("footer: a row put inside it joins it", u.footer_rows == 3 && u.rows.size() == 43);
        D::table_insert_row(u, 40);   // at its start: the body's
        check("footer: a row at its start is the body's", u.footer_rows == 3 && u.rows.size() == 44);
        D::table_delete_row(u, 43);
        check("footer: a footer row taken", u.footer_rows == 2);
        {
            core::Doc e;
            check("footer: saved", core::from_json(e, core::to_json(d)) && D::table_at(e, "story.1", p)->footer_rows == 2);
        }
        // none when it would leave no body row
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& x) { x.rows.resize(3); x.header_rows = 1; x.footer_rows = 2; });
        int n = 0;
        for (const auto& ft : L::flow_story(d, "story.1").frames) for (const auto& l : ft.lines) n += l.para == p;
        check("footer: a table all header and footer -- each row once", n == 3, std::to_string(n));
    }
    // -- s327: a column's edge dragged -- the gesture one step ---------------------
    {
        std::cout << "--- s327 column edge dragged ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(3, 3, 180));
        const auto before = core::to_json(d);
        const auto n0 = j.size();
        const auto w0 = D::table_at(d, "story.1", p)->widths;
        // as the canvas does: begin, the widths set again from the start at each move, commit
        j.begin("Column width", {"story.1"});
        for (double w : {70.0, 80.0, 90.0})
            D::edit_table(j, d, "story.1", p, "Column width", [&](core::Table& t) { t.widths = w0; D::table_set_width(t, 0, w); });
        check("drag: one step at the end", j.commit() && j.size() == n0 + 1 && d.strays() == 0);
        const auto* t = D::table_at(d, "story.1", p);
        check("drag: the column as dropped, its neighbour the difference, the width kept", t && t->widths[0] == 90 && t->widths[1] == 30 && t->widths[2] == 60);
        j.undo();
        check("drag: undone", core::to_json(d) == before);
        j.begin("Column width", {"story.1"});
        D::edit_table(j, d, "story.1", p, "Column width", [&](core::Table& x) { x.widths = w0; D::table_set_width(x, 2, 100); });
        j.cancel();
        check("drag: Esc -- back, no step", core::to_json(d) == before && j.size() == n0 + 1 && j.can_redo());   // the undone drag still to redo
        // the edge found where it's drawn (the table narrowed to its column: the hit's at the drawn edge)
        D::edit_table(j, d, "story.1", p, "wide", [](core::Table& x) { x.widths = {400, 400, 400}; });
        const auto fl = L::flow_story(d, "story.1");
        const L::Line* row0 = nullptr;
        std::string fr;
        for (const auto& ft : fl.frames)
            for (const auto& l : ft.lines)
                if (l.para == p && l.row == 0 && !row0) { row0 = &l; fr = ft.frame; }
        if (row0) {
            const double third = row0->w / 3;
            const auto h = L::table_hit(d, "story.1", fl, fr, row0->x + third + 1, row0->y + row0->h / 2, 2);
            check("drag: a narrowed table's edge at its drawn place", h.edge == 1 && row0->w < 1200, std::to_string(row0->w));
        } else {
            check("drag: a narrowed table's edge at its drawn place", false, "no row");
        }
    }
    // -- s328: typing in a cell ------------------------------------------------------
    {
        std::cout << "--- s328 typing in a cell ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto t0 = D::make_table(3, 3, 240);
        t0.header_rows = 1;
        t0.footer_rows = 1;
        D::table_merge(t0, 1, 1, 2, 1);   // (1,1) across (1,2)
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, t0);
        const auto cell = [&](int r, int c) { return D::cell_text(D::table_at(d, "story.1", p)->rows[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)]); };
        const auto n0 = j.size();
        // a burst: one step
        core::TextPos at{0, 0};
        for (const char* ch : {"F", "e", "e"}) at = D::edit_cell(j, d, "story.1", p, 0, 1, at, at, ch, "Typing", "typing:cell:1");
        check("cell: typed into an empty cell", cell(0, 1) == "Fee" && at == core::TextPos{0, 3});
        check("cell: a burst one step", j.size() == n0 + 1, std::to_string(j.size() - n0));
        at = D::edit_cell(j, d, "story.1", p, 0, 1, at, at, "\nper ton");
        check("cell: a new paragraph in the cell", cell(0, 1) == "Fee\nper ton" && at == core::TextPos{1, 7} &&
                                                       D::cell_end(D::table_at(d, "story.1", p)->rows[0].cells[1]) == core::TextPos{1, 7});
        at = D::edit_cell(j, d, "story.1", p, 0, 1, {0, 3}, {1, 0}, "");   // the break taken out
        check("cell: paragraphs joined", cell(0, 1) == "Feeper ton" && at == core::TextPos{0, 3});
        // bold kept round an insertion
        D::edit_table(j, d, "story.1", p, "bold", [](core::Table& x) {
            core::Run r;
            r.start = 0; r.end = 3; r.local.bold = 1;
            x.rows[0].cells[1].paras[0].runs = {r};
        });
        D::edit_cell(j, d, "story.1", p, 0, 1, {0, 1}, {0, 1}, "X");
        const auto& cp = D::table_at(d, "story.1", p)->rows[0].cells[1].paras[0];
        check("cell: typed inside bold words -- bold", cp.text == "FXeeper ton" && cp.runs.size() == 1 && cp.runs[0].end == 4 && cp.runs[0].local.bold == 1);
        D::edit_cell(j, d, "story.1", p, 0, 1, {0, 0}, D::cell_end(D::table_at(d, "story.1", p)->rows[0].cells[1]), "");
        check("cell: all its words out -- an empty cell again", D::table_at(d, "story.1", p)->rows[0].cells[1].paras.empty());
        check("cell: a covered cell refused", D::edit_cell(j, d, "story.1", p, 1, 2, {0, 0}, {0, 0}, "z") == core::TextPos{-1, -1});
        // undo
        const auto before = core::to_json(d);
        D::edit_cell(j, d, "story.1", p, 2, 0, {0, 0}, {0, 0}, "Total");
        j.undo();
        check("cell: undone exactly", core::to_json(d) == before);
        // Tab's order: across, down, a merge passed over
        const auto& t = *D::table_at(d, "story.1", p);
        check("tab: across", D::next_cell(t, 0, 0, 1) == std::pair<int, int>{0, 1});
        check("tab: on to the next row", D::next_cell(t, 0, 2, 1) == std::pair<int, int>{1, 0});
        check("tab: over a merge's covered cell", D::next_cell(t, 1, 1, 1) == std::pair<int, int>{2, 0} &&
                                                     D::next_cell(t, 1, 2, 1) == std::pair<int, int>{2, 0});
        check("shift+tab: back, to a merge's own cell", D::next_cell(t, 2, 0, -1) == std::pair<int, int>{1, 1});
        check("tab: past the ends -- none", D::next_cell(t, 2, 2, 1).first < 0 && D::next_cell(t, 0, 0, -1).first < 0);
        const int nr = D::table_tab_row(j, d, "story.1", p);
        check("tab in the last cell: a row before the footer", nr == 2 && D::table_at(d, "story.1", p)->rows.size() == 4 &&
                                                                 D::table_at(d, "story.1", p)->footer_rows == 1);
    }
    // -- s329: a cell's caret, hits, line moves, selection -------------------------
    {
        std::cout << "--- s329 cell geometry ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto t0 = D::make_table(2, 3, 240);
        D::set_cell_words(t0.rows[0].cells[1], "Harbor fees are charged per ton of cargo landed on the quay\nSecond");
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, t0);
        const auto fl = L::flow_story(d, "story.1");
        const L::CellRef c{p, 0, 1};
        const std::string fr = L::cell_frame(fl, c);
        const auto& words = D::table_at(d, "story.1", p)->rows[0].cells[1].paras[0].text;
        check("cell geo: its frame found", !fr.empty());
        const auto c0 = L::cell_caret(d, "story.1", fl, fr, c, {0, 0});
        const auto c5 = L::cell_caret(d, "story.1", fl, fr, c, {0, 6});
        // the cell's box, from the hit test at the caret
        const auto th = L::table_hit(d, "story.1", fl, fr, c0.x + 1, c0.y + c0.h / 2);
        check("cell geo: the caret inside its cell", c0.ok && th.row == 0 && th.col == 1 && c0.x >= th.cell.x && c0.x < th.cell.x + th.cell.w &&
                                                        c0.y >= th.cell.y - 0.01 && c0.h > 2);
        check("cell geo: further along, further right (same line)", c5.ok && c5.x > c0.x + 1 && std::abs(c5.y - c0.y) < 0.01);
        bool round = true;
        for (int b : {0, 3, 6, 12}) {
            const auto cc = L::cell_caret(d, "story.1", fl, fr, c, {0, b});
            const auto back = L::cell_hit(d, "story.1", fl, fr, c, cc.x + 0.01, cc.y + cc.h / 2);
            round = round && back == core::TextPos{0, b};
        }
        check("cell geo: a click at the caret comes back to it", round);
        const auto down = L::cell_line_move(d, "story.1", fl, fr, c, {0, 0}, 1, c0.x);
        check("cell geo: down a line -- the wrapped words' next line", down.para == 0 && down.byte > 6 && down.byte < static_cast<int>(words.size()),
              std::to_string(down.byte));
        check("cell geo: up from the first line leaves the cell", L::cell_line_move(d, "story.1", fl, fr, c, {0, 0}, -1, c0.x).para < 0);
        const auto end0 = L::cell_line_edge(d, "story.1", fl, fr, c, {0, 2}, true);
        const auto beg1 = L::cell_line_edge(d, "story.1", fl, fr, c, down, false);
        check("cell geo: a wrapped line's end before the next's start", end0.para == 0 && end0.byte > 2 && end0.byte < beg1.byte && beg1.byte <= down.byte);
        auto lastp = core::TextPos{1, 6};
        const auto cl = L::cell_caret(d, "story.1", fl, fr, c, lastp);
        check("cell geo: the second paragraph below the first", cl.ok && cl.y > c0.y + c0.h);
        check("cell geo: down past the last line leaves the cell", L::cell_line_move(d, "story.1", fl, fr, c, lastp, 1, cl.x).para < 0);
        const auto rs = L::cell_selection_rects(d, "story.1", fl, fr, c, {0, 3}, {1, 3});
        check("cell geo: a selection over the lines it takes", rs.size() >= 3 && std::all_of(rs.begin(), rs.end(), [](const core::Rect& r) { return r.w > 0; }),
              std::to_string(rs.size()));
        const auto ce = L::cell_caret(d, "story.1", fl, fr, L::CellRef{p, 1, 2}, {0, 0});
        check("cell geo: an empty cell has a caret", ce.ok && ce.h > 2);
        check("cell geo: a covered / missing cell none", !L::cell_caret(d, "story.1", fl, fr, L::CellRef{p, 5, 0}, {0, 0}).ok);
    }
    // -- s330: a page's picture knows its tables' cells ---------------------------
    {
        std::cout << "--- s330 page key ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        L::FlowCache flows;
        const std::string fr = L::cell_frame(flows.get(d, "story.1"), L::CellRef{p, 0, 0});
        const std::string page = d.as<core::Frame>(fr)->owner;
        const auto k0 = L::page_key(d, flows, page);
        D::edit_cell(j, d, "story.1", p, 0, 0, {0, 0}, {0, 0}, "Dover");
        const auto k1 = L::page_key(d, flows, page);
        check("page key: a cell's words change it", !(k0 == k1));
        D::edit_table(j, d, "story.1", p, "bold", [](core::Table& t) {
            core::Run r;
            r.start = 0; r.end = 5; r.local.bold = 1;
            t.rows[0].cells[0].paras[0].runs = {r};
        });
        check("page key: a cell's bold changes it", !(L::page_key(d, flows, page) == k1));
    }
    // -- s332: a row's height -- at least, never cutting its words; its edge dragged --
    {
        std::cout << "--- s332 row heights ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(3, 2, 180));
        {
            core::Table t = D::make_table(3, 2, 180);
            check("row height: set, half points", D::table_set_row_height(t, 1, 40.3) && t.rows[1].min_h == 40.5);
            check("row height: the same again -- no change", !D::table_set_row_height(t, 1, 40.5));
            check("row height: off the table refused", !D::table_set_row_height(t, 3, 20) && !D::table_set_row_height(t, -1, 20));
            check("row height: never below 0", D::table_set_row_height(t, 0, -10) == false && t.rows[0].min_h == 0);
        }
        auto row_line = [&](int r, std::string* frame = nullptr) -> L::Line {
            const auto fl = L::flow_story(d, "story.1");
            for (const auto& ft : fl.frames)
                for (const auto& l : ft.lines)
                    if (l.para == p && l.row == r) { if (frame) *frame = ft.frame; return l; }
            return {};
        };
        const double h0 = row_line(1).h;
        check("row height: as its words need at first", h0 > 0 && h0 < 60, std::to_string(h0));
        const auto before = core::to_json(d);
        const auto n0 = j.size();
        // as the canvas does: begin, the height from the start at each move, commit
        j.begin("Row height", {"story.1"});
        for (double h : {h0 + 10, h0 + 30, h0 + 50})
            D::edit_table(j, d, "story.1", p, "Row height", [&](core::Table& t) { D::table_set_row_height(t, 1, h); });
        check("row drag: one step at the end", j.commit() && j.size() == n0 + 1 && d.strays() == 0);
        const double h1 = row_line(1).h;
        check("row drag: the row as tall as dropped", std::abs(h1 - (h0 + 50)) < 0.6, std::to_string(h1));
        check("row drag: the rows around it as they were", std::abs(row_line(0).h - h0) < 0.6 && std::abs(row_line(2).h - h0) < 0.6);
        {
            core::Doc e;
            check("row height: saved", core::from_json(e, core::to_json(d)) && std::abs(D::table_at(e, "story.1", p)->rows[1].min_h - (h0 + 50)) < 0.6);
        }
        // its bottom edge found where it's drawn; not mid-row
        std::string fr;
        const auto ln = row_line(1, &fr);
        const auto fl = L::flow_story(d, "story.1");
        const auto e = L::table_row_edge(d, "story.1", fl, fr, ln.x + ln.w / 2, ln.y + ln.h + 1, 2);
        check("row edge: found at the row's foot", e.ok() && e.para == p && e.row == 1 && std::abs(e.height - ln.h) < 1e-6);
        check("row edge: none mid-row", !L::table_row_edge(d, "story.1", fl, fr, ln.x + ln.w / 2, ln.y + ln.h / 2, 2).ok());
        // smaller than its words: the words win (never cut)
        D::edit_table(j, d, "story.1", p, "Row height", [&](core::Table& t) { D::table_set_row_height(t, 1, 1); });
        check("row height: less than the words need -- as tall as they need", std::abs(row_line(1).h - h0) < 0.6, std::to_string(row_line(1).h));
        j.undo();
        j.undo();
        check("row drag: undone", core::to_json(d) == before);
    }
    // -- s333: formatting while typing in a cell ------------------------------------
    {
        std::cout << "--- s333 cell formatting ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 0, 0, {0, 0}, {0, 0}, "Dover Sole\nCrab");
        const auto before = core::to_json(d);
        const auto n0 = j.size();
        auto words = [&](int r, int c) {
            core::Story s;
            s.paras = D::table_at(d, "story.1", p)->rows[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)].paras;
            return s;
        };
        check("cell bold: one step", D::set_cell_local(j, d, "story.1", p, 0, 0, {0, 0}, {0, 5}, "Bold", [](core::Local& l) { l.bold = 1; }) &&
                                        j.size() == n0 + 1 && d.strays() == 0);
        {
            const auto w = words(0, 0);
            check("cell bold: those words, not the rest", core::all_bold(d, w, {0, 0}, {0, 5}) && !core::all_bold(d, w, {0, 6}, {0, 10}) &&
                                                         w.paras[0].text == "Dover Sole" && w.paras.size() == 2);
        }
        check("cell bold: again the same -- no step", !D::set_cell_local(j, d, "story.1", p, 0, 0, {0, 0}, {0, 5}, "Bold", [](core::Local& l) { l.bold = 1; }) &&
                                                     j.size() == n0 + 1);
        // across its paragraphs; an attribute through all_look
        D::set_cell_local(j, d, "story.1", p, 0, 0, {0, 6}, {1, 4}, "Underline", [](core::Local& l) { l.underline = 1; });
        {
            const auto w = words(0, 0);
            check("cell underline: across the cell's paragraphs",
                  core::all_look(d, w, {0, 6}, {1, 4}, [](const core::CharLook& k) { return k.underline; }) &&
                  !core::all_look(d, w, {0, 0}, {0, 5}, [](const core::CharLook& k) { return k.underline; }));
        }
        check("cell format: other cells untouched", words(0, 1).paras.size() <= 1 && words(1, 0).paras.size() <= 1 &&
                                                     (words(0, 1).paras.empty() || words(0, 1).paras[0].runs.empty()));
        // a covered cell refused
        D::edit_table(j, d, "story.1", p, "Merge", [](core::Table& t) { D::table_merge(t, 0, 0, 2, 1); });
        check("cell format: a covered cell refused", !D::set_cell_local(j, d, "story.1", p, 0, 1, {0, 0}, {0, 1}, "Bold", [](core::Local& l) { l.bold = 1; }));
        {
            L::FlowCache flows;
            const std::string fr = L::cell_frame(flows.get(d, "story.1"), L::CellRef{p, 0, 0});
            const std::string page = d.as<core::Frame>(fr)->owner;
            const auto k0 = L::page_key(d, flows, page);
            D::set_cell_local(j, d, "story.1", p, 0, 0, {1, 0}, {1, 4}, "Italic", [](core::Local& l) { l.italic = 1; });
            check("cell italic: the page's picture redrawn", !(L::page_key(d, flows, page) == k0));
        }
        {
            core::Doc e;
            check("cell format: saved", core::from_json(e, core::to_json(d)) && core::to_json(e) == core::to_json(d));
        }
        for (auto k = j.size(); k > n0; --k) j.undo();
        check("cell format: undone", core::to_json(d) == before);
    }
    // -- s334: a cell's own borders ------------------------------------------------
    {
        std::cout << "--- s334 cell borders ---\n";
        namespace D = core::doc;
        {
            core::Table t = D::make_table(3, 3, 180);
            t.rule_w = 0.5;
            const auto e0 = D::table_edge_look(t, {1, 1}, core::kTop, {0, 1}, core::kBottom);
            check("border: none set -- the grid's", e0.w == 0.5 && e0.color.empty());
            core::CellBorder b;
            b.w = 2;
            b.color = "swatch.x";
            check("border: set on the middle cell's top", D::table_set_border(t, 1, 1, 1, b));
            check("border: the cell above takes it on its bottom (one line)",
                  t.rows[0].cells[1].borders[core::kBottom] == t.rows[1].cells[1].borders[core::kTop]);
            const auto e1 = D::table_edge_look(t, {0, 1}, core::kBottom, {1, 1}, core::kTop);
            check("border: the shared edge as set, from either side", e1.w == 2 && e1.color == "swatch.x" &&
                                                                      D::table_edge_look(t, {1, 1}, core::kTop, {0, 1}, core::kBottom).w == 2);
            check("border: the other sides still the grid's", D::table_edge_look(t, {1, 1}, core::kLeft, {1, 0}, core::kRight).w == 0.5);
            check("border: the same again -- no change", !D::table_set_border(t, 1, 1, 1, b));
            core::CellBorder none;
            none.w = 0;
            D::table_set_border(t, 1, 1, 2 | 8, none);
            check("border: 0 -- no line on those sides", D::table_edge_look(t, {1, 1}, core::kRight, {1, 2}, core::kLeft).w == 0 &&
                                                         D::table_edge_look(t, {1, 0}, core::kRight, {1, 1}, core::kLeft).w == 0);
            check("border: back to the grid", D::table_set_border(t, 1, 1, 15, core::CellBorder{}) &&
                                              D::table_edge_look(t, {0, 1}, core::kBottom, {1, 1}, core::kTop).w == 0.5 &&
                                              t.rows[0].cells[1].borders[core::kBottom].w < 0);
            // the outside: the table's own edge takes the cell's alone
            D::table_set_border(t, 0, 0, 1 | 8, b);
            check("border: the table's outside edge", D::table_edge_look(t, {0, 0}, core::kTop, {-1, -1}, core::kBottom).w == 2);
            // a merge: the cells across all its length take it
            D::table_merge(t, 1, 0, 2, 1);
            D::table_set_border(t, 1, 0, 4, b);
            check("border: a merge's bottom -- each cell under it", t.rows[2].cells[0].borders[core::kTop].w == 2 && t.rows[2].cells[1].borders[core::kTop].w == 2);
            check("border: a covered cell refers to its merge", D::table_set_border(t, 1, 1, 1, b) && t.rows[1].cells[0].borders[core::kTop].w == 2);
        }
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        const std::string sw = d.ids_of<core::Swatch>().back();
        L::FlowCache flows;
        const std::string fr = L::cell_frame(flows.get(d, "story.1"), L::CellRef{p, 0, 0});
        auto rules = [&](double w, const std::string& col) {
            int n = 0;
            L::visit_tables(d, "story.1", flows.get(d, "story.1"), fr, [&](const L::TablePiece& tp) {
                if (tp.kind != L::TablePiece::Rule) return;
                const double thin = std::min(tp.r.w, tp.r.h);
                n += std::abs(thin - w) < 1e-6 && tp.color == col;
            });
            return n;
        };
        const int grid0 = rules(D::table_at(d, "story.1", p)->rule_w, D::table_at(d, "story.1", p)->rule_color);
        const auto page = d.as<core::Frame>(fr)->owner;
        const auto k0 = L::page_key(d, flows, page);
        core::CellBorder b;
        b.w = 3;
        b.color = sw;
        check("border: one step", D::edit_table(j, d, "story.1", p, "Cell border", [&](core::Table& t) { D::table_set_border(t, 0, 0, 15, b); }) &&
                                  d.strays() == 0);
        check("border: drawn -- its four sides heavy, in its colour", rules(3, sw) >= 4, std::to_string(rules(3, sw)));
        check("border: the rest still the grid", rules(D::table_at(d, "story.1", p)->rule_w, D::table_at(d, "story.1", p)->rule_color) > 0 &&
                                                 rules(D::table_at(d, "story.1", p)->rule_w, D::table_at(d, "story.1", p)->rule_color) < grid0);
        check("border: the page's picture redrawn", !(L::page_key(d, flows, page) == k0));
        check("border: its colour in use", core::doc::swatch_uses(d, sw) > 0);
        {
            core::Doc e;
            check("border: saved", core::from_json(e, core::to_json(d)) && D::table_at(e, "story.1", p)->rows[0].cells[0].borders[core::kRight].w == 3 &&
                                   D::table_at(e, "story.1", p)->rows[0].cells[1].borders[core::kLeft].color == sw &&
                                   D::table_at(e, "story.1", p)->rows[1].cells[1].borders[core::kTop].w < 0);
        }
        // the grid off: a cell's own border still drawn
        D::edit_table(j, d, "story.1", p, "Grid", [](core::Table& t) { t.rule_w = 0; });
        check("border: drawn with the grid off", rules(3, sw) >= 4 && rules(0, "") == 0);
    }
    // -- s335: Find / Change in a table's cells -------------------------------------
    {
        std::cout << "--- s335 find in cells ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 0, 0, {0, 0}, {0, 0}, "zebrine one\nzebrine two");
        D::edit_cell(j, d, "story.1", p, 1, 1, {0, 0}, {0, 0}, "a Zebrine");
        D::edit_table(j, d, "story.1", p, "Merge", [](core::Table& t) { D::table_merge(t, 0, 0, 2, 1); });
        D::FindOpts f;
        f.what = "zebrine";
        auto hits = D::find_all(d, f);
        int in_cells = 0;
        for (const auto& h : hits) in_cells += h.table == p;
        check("find: the cells' words found (a merge once, every paragraph)", in_cells == 3, std::to_string(in_cells));
        bool placed = true;
        for (const auto& h : hits)
            if (h.table == p) placed = placed && h.story == "story.1" && h.a.byte >= 0 && h.b.byte - h.a.byte == 7;
        check("find: each hit its cell and its place in the cell's words", placed);
        {
            int k = -1, last = -1;   // read where the table is: after the table paragraph's own, before the next paragraph's
            bool order = true;
            for (const auto& h : hits) {
                ++k;
                if (h.story != "story.1") continue;
                const int at = h.table >= 0 ? h.table : h.a.para;
                order = order && at >= last;
                last = at;
            }
            check("find: read in order, where the table is", order);
        }
        f.match_case = true;
        int cased = 0;
        for (const auto& h : D::find_all(d, f)) cased += h.table == p;
        check("find: match case in cells", cased == 2, std::to_string(cased));
        f.match_case = false;
        const auto before = core::to_json(d);
        const auto n0 = j.size();
        D::ChangeOpts c;
        c.set_text = true;
        c.with = "okapi";
        std::vector<D::Hit> cell_hits;
        for (const auto& h : hits) if (h.table == p) cell_hits.push_back(h);
        check("change all: in the cells, one step", D::change_hits(j, d, cell_hits, c, "Change All") == 3 && j.size() == n0 + 1 && d.strays() == 0);
        {
            const auto* t = D::table_at(d, "story.1", p);
            check("change all: the words changed, the rest kept",
                  D::cell_text(t->rows[0].cells[0]) == "okapi one\nokapi two" && D::cell_text(t->rows[1].cells[1]) == "a okapi");
        }
        j.undo();
        check("change all: undone", core::to_json(d) == before);
        // in the text and in a cell together: both, the table kept in its place
        f.what = "the";
        auto both = D::find_all(d, f);
        D::edit_cell(j, d, "story.1", p, 1, 0, {0, 0}, {0, 0}, "the end");
        both = D::find_all(d, f);
        int nc = 0;
        for (const auto& h : both) nc += h.table == p;
        c.with = "THE";
        D::change_hits(j, d, both, c, "Change All");
        check("change all: text and cells together", nc == 1 && D::cell_text(D::table_at(d, "story.1", p)->rows[1].cells[0]) == "THE end");
        check("change all: nothing left in either", D::find_all(d, [] { D::FindOpts o; o.what = "the"; o.match_case = true; return o; }()).empty());
    }
    // -- s336: the inspector while typing in a cell; a character style in cells --------
    {
        std::cout << "--- s336 cell inspector ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 1, 0, {0, 0}, {0, 0}, "Dover Sole\nCrab");
        const auto* cs = d.as<core::Style>("style.5");
        check("fixture: style.5 a character style", cs && cs->kind == core::StyleKind::Character);
        auto row = [](const std::vector<core::Field>& fs, const std::string& edit) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == edit) return &f;
            return nullptr;
        };
        auto fs = core::cell_text_fields(d, "story.1", p, 1, 0, {0, 0}, {0, 5});
        check("cell fields: the word rows, no paragraph rows",
              row(fs, "lsize") && row(fs, "lcolor") && row(fs, "luline") && row(fs, "local") && row(fs, "cstyle") && !row(fs, "pstyle"));
        bool sel5 = false;
        for (const auto& f : fs) sel5 = sel5 || (f.label == "Selection" && f.value == "5 characters");
        check("cell fields: the selection counted in the cell's words", sel5);
        check("cell fields: a covered / missing cell -- none", core::cell_text_fields(d, "story.1", p, 7, 0, {0, 0}, {0, 0}).empty());
        D::set_cell_local(j, d, "story.1", p, 1, 0, {0, 0}, {0, 5}, "Size", [](core::Local& l) { l.size = 14; });
        fs = core::cell_text_fields(d, "story.1", p, 1, 0, {0, 0}, {0, 5});
        check("cell fields: its own size shown", row(fs, "lsize") && row(fs, "lsize")->value.find("14") != std::string::npos, row(fs, "lsize") ? row(fs, "lsize")->value : "");
        const auto n0 = j.size();
        check("cell char style: one step", D::set_cell_char_style(j, d, "story.1", p, 1, 0, {0, 6}, {1, 2}, "style.5") && j.size() == n0 + 1);
        fs = core::cell_text_fields(d, "story.1", p, 1, 0, {0, 6}, {1, 2});
        check("cell char style: shown", row(fs, "cstyle") && row(fs, "cstyle")->value == cs->name, row(fs, "cstyle") ? row(fs, "cstyle")->value : "");
        check("cell char style: a paragraph style refused", !D::set_cell_char_style(j, d, "story.1", p, 1, 0, {0, 0}, {0, 2}, "style.3"));
        check("cell char style: its size kept", row(core::cell_text_fields(d, "story.1", p, 1, 0, {0, 0}, {0, 5}), "lsize")->value.find("14") != std::string::npos);
        // Change: a character style onto hits in cells
        D::FindOpts f;
        f.what = "Crab";
        D::ChangeOpts c;
        c.set_text = false;
        c.cstyle = "style.5";
        const auto before = core::to_json(d);
        f.match_case = true;
        {
            const auto hs = D::find_all(d, f);
            int nc = 0;
            for (const auto& h : hs) nc += h.table == p;
            const int n = D::change_hits(j, d, hs, c, "Change All");
            check("change: a character style in a cell", nc == 1 && n == static_cast<int>(hs.size()), std::to_string(nc) + "/" + std::to_string(n) + "/" + std::to_string(hs.size()));
        }
        f.match_case = false;
        f.what = "";
        f.cstyle = "style.5";
        int in_cell = 0;
        for (const auto& h : D::find_all(d, f)) in_cell += h.table == p;
        check("change: found again by the style", in_cell >= 2, std::to_string(in_cell));
        j.undo();
        check("change: undone", core::to_json(d) == before);
    }
    // -- s337: the inspector's Cell group while typing in a cell ---------------------
    {
        std::cout << "--- s337 cell group ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(3, 3, 180));
        auto row = [](const std::vector<core::Field>& fs, const std::string& edit) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == edit) return &f;
            return nullptr;
        };
        auto fs = core::cell_text_fields(d, "story.1", p, 1, 1, {0, 0}, {0, 0});
        bool grouped = true;
        for (const auto& f : fs) if (f.edit.rfind("cell_", 0) == 0) grouped = grouped && f.group == "Cell";
        check("cell group: its rows, under Cell", row(fs, "cell_fill") && row(fs, "cell_valign") && row(fs, "cell_rowh") && row(fs, "cell_border") &&
                                                  row(fs, "cell_border_color") && grouped);
        check("cell group: rows / columns / merge as buttons", row(fs, "cell_rows") && row(fs, "cell_rows")->buttons && row(fs, "cell_cols") &&
                                                               row(fs, "cell_merge") && row(fs, "cell_merge")->choices.size() == 3);
        check("cell group: no fill -- no tint row; as the grid; as its words",
              !row(fs, "cell_tint") && row(fs, "cell_border")->value == "As the grid" && row(fs, "cell_rowh")->value == "As its words");
        check("cell group: the row height typed in points", row(fs, "cell_rowh")->unit == 1 && row(fs, "cell_rowh")->hi >= 2000 &&
                                                            row(fs, "cell_border")->unit == 1);
        const std::string sw = d.ids_of<core::Swatch>().back();
        core::CellBorder b;
        b.w = 2;
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) {
            t.rows[1].cells[1].fill = sw;
            t.rows[1].cells[1].fill_tint = 0.5;
            t.rows[1].cells[1].valign = 2;
            D::table_set_row_height(t, 1, 40);
            D::table_set_border(t, 1, 1, 1, b);
        });
        fs = core::cell_text_fields(d, "story.1", p, 1, 1, {0, 0}, {0, 0});
        check("cell group: the fill, its tint, the words' place, the row",
              row(fs, "cell_fill")->value == d.as<core::Swatch>(sw)->name && row(fs, "cell_tint") && row(fs, "cell_tint")->value == "50%" &&
              row(fs, "cell_tint")->percent && row(fs, "cell_valign")->value == "Bottom" && row(fs, "cell_rowh")->value.find("40") != std::string::npos);
        check("cell group: sides that differ -- Mixed", row(fs, "cell_border")->value == core::kMixed && row(fs, "cell_border_color")->value == core::kMixed);
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) { D::table_set_border(t, 1, 1, 15, b); D::table_merge(t, 1, 1, 2, 2); });
        fs = core::cell_text_fields(d, "story.1", p, 1, 1, {0, 0}, {0, 0});
        bool says = false;
        for (const auto& f : fs) says = says || (f.label == "Cell" && f.value.find("2 × 2") != std::string::npos);
        check("cell group: all four agree -- the weight; a merge says its size", row(fs, "cell_border")->value.find("2") != std::string::npos && says);
    }
    // -- s338: a layer's colour on its frames ----------------------------------------
    {
        std::cout << "--- s338 layer colours ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("layer colours: InDesign's list, distinct", D::layer_colors().size() >= 20 &&
              std::set<std::string>([] { std::set<std::string> x; for (const auto& c : D::layer_colors()) x.insert(c.hex); return x; }()).size() ==
                  D::layer_colors().size());
        const auto l2 = D::add_layer(j, d);
        const auto ls = D::layers(d);
        check("new layer: the first gets Light Blue, the new one the next", ls.size() == 2 && d.as<core::Layer>(ls[0])->color == D::layer_colors()[0].hex &&
                                                                            d.as<core::Layer>(l2)->color == D::layer_colors()[1].hex);
        const auto l3 = D::add_layer(j, d);
        D::edit_layer(j, d, l2, "Layer Colour", [](core::Layer& l) { l.color = "#2fae4f"; });
        const auto l4 = D::add_layer(j, d);
        check("new layer: a colour not yet taken", d.as<core::Layer>(l3)->color == D::layer_colors()[2].hex &&
                                                   d.as<core::Layer>(l4)->color == D::layer_colors()[1].hex, d.as<core::Layer>(l4)->color);
        check("layer colour: named", D::layer_color_name("#e63c3c") == "Red" && D::layer_color_name("") == "Usual" && D::layer_color_name("#123456") == "Custom");
        core::Frame f;
        f.layer = l2;
        double c[3] = {0, 0, 0};
        check("layer rgb: its frames'", D::layer_rgb(d, f, c) && std::abs(c[0] - 0x2f / 255.0) < 1e-9 && std::abs(c[1] - 0xae / 255.0) < 1e-9);
        D::edit_layer(j, d, l2, "Layer Colour", [](core::Layer& l) { l.color = ""; });
        check("layer rgb: none -- the usual", !D::layer_rgb(d, f, c));
        D::edit_layer(j, d, l2, "Layer Colour", [](core::Layer& l) { l.color = "#zz0000"; });
        check("layer rgb: a bad one -- the usual", !D::layer_rgb(d, f, c));
        {
            core::Doc e;
            check("layer colour: saved", core::from_json(e, core::to_json(d)) && e.as<core::Layer>(l3)->color == D::layer_colors()[2].hex);
        }
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& it : o.items())
            if (it.id == l3)
                for (const auto& fl : it.fields) row = row || (fl.edit == "layer_color" && fl.value == D::layer_colors()[2].name);
        check("layers card: its Colour row", row);
    }
    // -- s339: the Hyperlinks list; a link's target changed everywhere ------------------
    {
        std::cout << "--- s339 hyperlinks ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto base = D::link_uses(d).size();
        D::set_link(j, d, "story.1", {1, 0}, {1, 4}, "example.com");
        D::set_link(j, d, "story.1", {2, 0}, {2, 3}, "https://example.com");
        D::set_link(j, d, "story.1", {3, 0}, {3, 2}, "a@b.org");
        const int p = D::insert_table(j, d, "story.1", {4, 0}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 1, 1, {0, 0}, {0, 0}, "see the site");
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& t) {
            core::Run r;
            r.start = 4; r.end = 12; r.local.link = "https://example.com";
            t.rows[1].cells[1].paras[0].runs = {r};
        });
        const auto all = D::hyperlinks(d);
        int cells = 0;
        for (const auto& l : all) if (l.row == 1 && l.col == 1 && l.cpara == 0 && l.words == "the site") ++cells;
        check("links: a cell's found, its words and place", cells == 1);
        const auto us = D::link_uses(d);
        const D::LinkUse* ex = nullptr;
        for (const auto& u : us) if (u.target == "https://example.com") ex = &u;
        check("link uses: each target once, its uses in reading order", us.size() == base + 2 && ex && ex->uses.size() == 3 &&
                                                                         ex->uses[0].para == 1 && ex->uses[1].para == 2 && ex->uses[2].row == 1);
        const auto o = core::project(d, "");
        bool item = false;
        for (const auto& it : o.items())
            if (it.id == "link:https://example.com" && it.segment == core::Segment::Links && it.badge == "3 uses") item = true;
        check("outline: a Hyperlinks item per target", item);
        const auto before = core::to_json(d);
        const auto n0 = j.size();
        check("retarget: every use, one step", D::retarget_links(j, d, "https://example.com", "www.example.org") == 3 && j.size() == n0 + 1 && d.strays() == 0);
        bool moved = true;
        for (const auto& l : D::hyperlinks(d)) moved = moved && l.target != "https://example.com";
        int now = 0;
        for (const auto& l : D::hyperlinks(d)) now += l.target == "https://www.example.org";
        check("retarget: none left at the old, all at the new (normalised), the cell's too", moved && now == 3);
        check("retarget: the same -- nothing", D::retarget_links(j, d, "https://www.example.org", "https://www.example.org") == 0);
        check("remove: the link taken off, the words kept", D::retarget_links(j, d, "mailto:a@b.org", "") == 1 &&
                                                            D::link_uses(d).size() == base + 1);
        j.undo();
        j.undo();
        check("retarget: undone", core::to_json(d) == before);
        // the segment once, in order
        int seen = 0;
        for (const auto& si : core::segments()) seen += si.id == core::Segment::Links;
        check("segments: Hyperlinks once", seen == 1);
    }
    // -- s340: a cell's links in the PDF; its links and misspelled words marked ----------
    {
        std::cout << "--- s340 cell links and marks ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 0, 1, {0, 0}, {0, 0}, "see the site");
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& t) {
            core::Run r;
            r.start = 4; r.end = 12; r.local.link = "https://example.com";
            t.rows[0].cells[1].paras[0].runs = {r};
        });
        L::FlowCache flows;
        const std::string fr = L::cell_frame(flows.get(d, "story.1"), L::CellRef{p, 0, 1});
        int marked = 0;
        for (const auto& m : L::text_marks(d, flows, fr, false, true))
            marked += m.kind == L::TextMark::Link && m.row == 0 && m.col == 1 && m.cpara == 0 && m.from == 4 && m.to == 12 && m.box.w > 0;
        check("marks: a cell's link marked on screen", marked >= 1, std::to_string(marked));
        int pdf = 0;
        const auto page = d.as<core::Frame>(fr)->owner;
        for (const auto& l : L::hyperlink_links(d, flows)) pdf += l.uri == "https://example.com" && l.page == page && l.box.w > 0;
        check("pdf: a cell's link a live box on its page", pdf >= 1, std::to_string(pdf));
        if (core::spell::loaded()) {
            D::edit_cell(j, d, "story.1", p, 1, 0, {0, 0}, {0, 0}, "qwzxv words");
            int waved = 0;
            for (const auto& m : L::text_marks(d, flows, fr, true, false)) waved += m.kind == L::TextMark::Misspelled && m.row == 1 && m.col == 0 && m.from == 0 && m.to == 5;
            check("marks: a cell's misspelled word waved", waved >= 1, std::to_string(waved));
        }
    }
    // -- s340: Ctrl+K in a cell -------------------------------------------------------
    {
        std::cout << "--- s340 links typed in a cell ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        D::edit_cell(j, d, "story.1", p, 1, 1, {0, 0}, {0, 0}, "see the site");
        const auto n0 = j.size();
        check("cell link: one step", D::set_cell_link(j, d, "story.1", p, 1, 1, {0, 4}, {0, 12}, "example.com") && j.size() == n0 + 1);
        int found = 0;
        for (const auto& l : D::hyperlinks(d)) found += l.row == 1 && l.col == 1 && l.target == "https://example.com" && l.words == "the site";
        check("cell link: normalised, found", found == 1);
        check("cell link: off again", D::set_cell_link(j, d, "story.1", p, 1, 1, {0, 4}, {0, 12}, ""));
        // to a paragraph (one with no anchor yet): the anchor made, the link not broken
        const int to = 3;
        check("cell link to a paragraph: one step", D::cell_link_to_paragraph(j, d, "story.1", p, 1, 1, {0, 0}, {0, 3}, "story.1", to + (to >= p ? 1 : 0)));
        bool ok = false;
        for (const auto& l : D::hyperlinks(d)) if (l.row == 1 && l.col == 1) ok = l.target.size() > 1 && l.target[0] == '#' && !l.broken;
        check("cell link to a paragraph: its anchor made, not broken", ok);
        check("cell link to a paragraph: the table's own refused", !D::cell_link_to_paragraph(j, d, "story.1", p, 1, 1, {0, 0}, {0, 3}, "story.1", p));
        check("cell link: the table still where it was", D::table_at(d, "story.1", p) != nullptr);
    }
    // -- s341: a layer dragged into another place; its colour on its row -----------------
    {
        std::cout << "--- s341 layer rows ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto b = D::add_layer(j, d);   // the first named too: [first, b]
        const auto c = D::add_layer(j, d);   // [first, b, c]
        const auto a = D::layers(d).front();
        check("drop: c below a -- the bottom", D::move_layer_by(j, d, c, a, false) && D::layers(d) == std::vector<std::string>{c, a, b});
        check("drop: c above b -- the top", D::move_layer_by(j, d, c, b, true) && D::layers(d) == std::vector<std::string>{a, b, c});
        check("drop: where it already is -- nothing", !D::move_layer_by(j, d, c, b, true) && !D::move_layer_by(j, d, c, c, true));
        check("drop: a between", D::move_layer_by(j, d, a, c, false) && D::layers(d) == std::vector<std::string>{b, a, c});
        j.undo();
        check("drop: undone", D::layers(d) == std::vector<std::string>{a, b, c});
        const auto o = core::project(d, "");
        bool rgb = false;
        for (const auto& it : o.items())
            if (it.id == b) rgb = it.thumb.rgb == 0xe63c3c;   // the second layer: Red
        check("layers card: a row carries its colour", rgb);
    }
    // -- s342: table and cell styles (TB2, as InDesign) ---------------------------------
    {
        std::cout << "--- s342 table / cell styles ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const std::string sw = d.ids_of<core::Swatch>().back();
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(4, 2, 160));
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& t) { t.header_rows = 1; t.footer_rows = 1; });
        auto T = [&] { return *D::table_at(d, "story.1", p); };
        // a cell style from a look
        core::TableCell look;
        look.fill = sw;
        look.fill_tint = 0.5;
        look.valign = 1;
        const auto head = D::add_cell_style(j, d, "Head", &look);
        const auto* hs = d.as<core::CellStyle>(head);
        check("cell style: made from a cell's look", hs && hs->name == "Head" && hs->fill && *hs->fill == sw && hs->valign && *hs->valign == 1);
        check("cell style: a second of the name numbered", d.as<core::CellStyle>(D::add_cell_style(j, d, "Head"))->name == "Head 2");
        const auto body = D::add_cell_style(j, d, "Body");
        D::edit_cell_style(j, d, body, "x", [](core::CellStyle& c) { c.valign = 2; });
        const auto ts = D::add_table_style(j, d, "Grid");
        D::edit_table_style(j, d, ts, "x", [&](core::TableStyle& t) { t.rule_w = 1; t.header_cell = head; t.body_cell = body; });
        // applied
        const auto n0 = j.size();
        check("apply table style: one step", D::edit_table(j, d, "story.1", p, "Table Style", [&](core::Table& t) { D::apply_table_style(d, t, ts); }) && j.size() == n0 + 1);
        auto t = T();
        check("apply: the table's settings, its style named", t.style == ts && t.rule_w == 1);
        check("apply: each region's cells its cell style (the footer none)", t.rows[0].cells[0].style == head && t.rows[0].cells[0].fill == sw &&
                                                                             t.rows[1].cells[1].style == body && t.rows[1].cells[1].valign == 2 &&
                                                                             t.rows[3].cells[0].style.empty());
        // a cell given its own style keeps it; a cell's own change kept through an edit
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& x) { D::apply_cell_style(d, x.rows[2].cells[0], head); x.rows[1].cells[0].valign = 0; });
        D::edit_cell_style(j, d, body, "Body", [](core::CellStyle& c) { c.valign = 1; c.fill = ""; });
        t = T();
        check("edit cell style: its cells follow", t.rows[1].cells[1].valign == 1 && t.rows[2].cells[1].valign == 1);
        check("edit cell style: a cell's own change kept", t.rows[1].cells[0].valign == 0);
        check("edit cell style: another style's cell untouched", t.rows[2].cells[0].style == head && t.rows[2].cells[0].fill == sw);
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& x) { x.inset = 9; });
        D::edit_table_style(j, d, ts, "Grid", [](core::TableStyle& x) { x.rule_w = 2; x.inset = 6; x.footer_cell = ""; x.body_cell = ""; });
        t = T();
        check("edit table style: the table follows, its own inset kept", t.rule_w == 2 && t.inset == 9);
        check("edit table style: the body's cells lose the region's style, the own one kept",
              t.rows[1].cells[1].style.empty() && t.rows[2].cells[0].style == head);
        check("uses counted", D::table_style_uses(d, ts) == 1 && D::cell_style_uses(d, head) >= 3, std::to_string(D::cell_style_uses(d, head)));
        check("swatch: a style's colour in use", D::swatch_uses(d, sw) > 0);
        {
            core::Doc e;
            check("styles: saved", core::from_json(e, core::to_json(d)) && core::to_json(e) == core::to_json(d) && e.as<core::TableStyle>(ts) &&
                                   e.as<core::CellStyle>(head) && D::table_at(e, "story.1", p)->style == ts);
        }
        const auto before = core::to_json(d);
        check("delete cell style: its cells keep the look, the style cleared", D::delete_cell_style(j, d, head) && !d.as<core::CellStyle>(head) &&
                                                                               T().rows[0].cells[0].style.empty() && T().rows[0].cells[0].fill == sw &&
                                                                               d.as<core::TableStyle>(ts)->header_cell.empty());
        check("delete table style: the table keeps its look", D::delete_table_style(j, d, ts) && T().style.empty() && T().rule_w == 2);
        j.undo();
        j.undo();
        check("deletes undone", core::to_json(d) == before);
    }
    // -- s343: table / cell styles in the Styles section; the Cell group's style rows -----
    {
        std::cout << "--- s343 styles in the inspector ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto cs = D::add_cell_style(j, d, "Head");
        const auto ts = D::add_table_style(j, d, "Grid");
        D::edit_table_style(j, d, ts, "x", [&](core::TableStyle& t) { t.header_cell = cs; t.band_fill = d.ids_of<core::Swatch>().back(); });
        auto field = [](const core::Item& it, const std::string& edit) -> const core::Field* {
            for (const auto& f : it.fields) if (f.edit == edit) return &f;
            return nullptr;
        };
        const auto o = core::project(d, "");
        const core::Item *ti = nullptr, *ci = nullptr;
        for (const auto& it : o.items()) {
            if (it.id == ts) ti = &it;
            if (it.id == cs) ci = &it;
        }
        check("styles section: a table style and a cell style listed", ti && ci && ti->segment == core::Segment::Styles && ci->segment == core::Segment::Styles);
        check("table style item: its rows, grouped", ti && field(*ti, "ts_rule_w") && field(*ti, "ts_header_cell") && field(*ti, "ts_header_cell")->value == "Head" &&
                                                     field(*ti, "ts_band_tint") && field(*ti, "ts_act") && field(*ti, "ts_rule_w")->group == "Table style");
        check("cell style item: unset shown as Not set", ci && field(*ci, "cs_fill")->value == "Not set" && field(*ci, "cs_valign")->value == "Not set" &&
                                                         field(*ci, "cs_border")->value == "Not set" && !field(*ci, "cs_tint"));
        check("unused badged", ti && ti->badge.find("Unused") != std::string::npos);
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(2, 2, 160));
        auto fs = core::cell_text_fields(d, "story.1", p, 0, 0, {0, 0}, {0, 0});
        const core::Field* tr = nullptr;
        for (const auto& f : fs) if (f.edit == "cell_tstyle") tr = &f;
        check("cell group: table style / cell style rows, New style buttons", tr && tr->choices.size() == 2 && tr->value == "None");
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) { t.header_rows = 1; D::apply_table_style(d, t, ts); });
        fs = core::cell_text_fields(d, "story.1", p, 0, 0, {0, 0}, {0, 0});
        std::string tv, cv;
        for (const auto& f : fs) { if (f.edit == "cell_tstyle") tv = f.value; if (f.edit == "cell_cstyle") cv = f.value; }
        check("cell group: shows the styles in use", tv == "Grid" && cv == "Head");
    }
    // -- s344: Alt+drag copies (as the canvas does it: the move put back, a copy made) --
    {
        std::cout << "--- s344 alt+drag copies ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = core::to_json(d);
        const auto f0 = *d.as<core::Frame>("frame.2");
        const auto n0 = j.size();
        D::frames_begin(j, d, {"frame.2"}, "Move frame");
        D::frames_set(j, d, "frame.2", {f0.x + 30, f0.y + 20, f0.w, f0.h});
        const auto* f1 = d.as<core::Frame>("frame.2");
        const double dx = f1->x - f0.x, dy = f1->y - f0.y;
        D::frames_cancel(j);
        const auto made = D::duplicate_frames(j, d, {"frame.2"}, dx, dy);
        check("alt+drag: one step, the original where it was", j.size() == n0 + 1 && made.size() == 1 && *d.as<core::Frame>("frame.2") == f0);
        const auto* c = made.empty() ? nullptr : d.as<core::Frame>(made.front());
        check("alt+drag: the copy where it was dropped", c && std::abs(c->x - (f0.x + 30)) < 1e-9 && std::abs(c->y - (f0.y + 20)) < 1e-9 && c->owner == f0.owner);
        j.undo();
        check("alt+drag: undone in one", core::to_json(d) == before);
    }
    // -- s345: table / cell styles' specimens -------------------------------------------
    {
        std::cout << "--- s345 style specimens ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const std::string sw = d.ids_of<core::Swatch>().back();
        const auto ts = D::add_table_style(j, d, "Grid");
        D::edit_table_style(j, d, ts, "x", [&](core::TableStyle& t) { t.header_fill = sw; t.header_tint = 1; t.rule_w = 2; });
        const auto cs = D::add_cell_style(j, d, "Hot");
        D::edit_cell_style(j, d, cs, "x", [&](core::CellStyle& c) { c.fill = sw; c.borders[0].w = 2; });
        const auto o = core::project(d, "");
        const core::Thumb *tt = nullptr, *ct = nullptr;
        for (const auto& it : o.items()) { if (it.id == ts) tt = &it.thumb; if (it.id == cs) ct = &it.thumb; }
        const auto rgb = core::doc::screen_rgb(*d.as<core::Swatch>(sw));
        check("specimen: a table style -- its header's colour, no bands, its grid", tt && tt->kind == core::ThumbKind::Table && tt->panels == 0 &&
                                                                                   tt->rgb == rgb && tt->rgb2 == core::Thumb::kNoFill && tt->rule == 2);
        check("specimen: a cell style -- one cell, its fill, heavy borders", ct && ct->kind == core::ThumbKind::Table && ct->panels == 1 && ct->rgb == rgb && ct->bold);
    }
    // -- s346: a table style's left / right column cell styles ------------------------
    {
        std::cout << "--- s346 column cell styles ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto head = D::add_cell_style(j, d, "Head");
        const auto left = D::add_cell_style(j, d, "Left");
        const auto right = D::add_cell_style(j, d, "Right");
        const auto body = D::add_cell_style(j, d, "Body");
        const auto ts = D::add_table_style(j, d, "Grid");
        D::edit_table_style(j, d, ts, "x", [&](core::TableStyle& t) { t.header_cell = head; t.body_cell = body; t.left_cell = left; t.right_cell = right; });
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(3, 3, 180));
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) { t.header_rows = 1; D::table_merge(t, 2, 1, 2, 1); D::apply_table_style(d, t, ts); });
        auto T = [&] { return *D::table_at(d, "story.1", p); };
        auto t = T();
        check("columns: the header row its own, not the columns'", t.rows[0].cells[0].style == head && t.rows[0].cells[2].style == head);
        check("columns: the body's first / last column, the middle the body's", t.rows[1].cells[0].style == left && t.rows[1].cells[2].style == right &&
                                                                               t.rows[1].cells[1].style == body);
        check("columns: a merge reaching the last column takes the right's", t.rows[2].cells[1].style == right);
        D::edit_table_style(j, d, ts, "x", [](core::TableStyle& x) { x.left_cell.clear(); });
        check("columns: the left one cleared -- those cells the body's", T().rows[1].cells[0].style == body && T().rows[1].cells[2].style == right);
        {
            core::Doc e;
            check("columns: saved", core::from_json(e, core::to_json(d)) && e.as<core::TableStyle>(ts)->right_cell == right);
        }
        D::delete_cell_style(j, d, right);
        check("columns: a deleted cell style leaves the table style", d.as<core::TableStyle>(ts)->right_cell.empty());
    }
    // -- s347: Redefine Style; Clear Overrides ---------------------------------------
    {
        std::cout << "--- s347 redefine / clear ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const std::string sw = d.ids_of<core::Swatch>().back();
        const auto body = D::add_cell_style(j, d, "Body");
        D::edit_cell_style(j, d, body, "x", [](core::CellStyle& c) { c.valign = 0; });
        const auto ts = D::add_table_style(j, d, "Grid");
        D::edit_table_style(j, d, ts, "x", [&](core::TableStyle& t) { t.body_cell = body; });
        const int p = D::insert_table(j, d, "story.1", {1, static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size())}, D::make_table(3, 2, 160));
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) { D::apply_table_style(d, t, ts); });
        auto T = [&] { return *D::table_at(d, "story.1", p); };
        // a cell changed by hand, then the style redefined from it: every body cell follows
        D::edit_table(j, d, "story.1", p, "x", [&](core::Table& t) { t.rows[1].cells[1].fill = sw; t.rows[1].cells[1].valign = 1; });
        const auto n0 = j.size();
        check("redefine cell style: one step", D::redefine_cell_style(j, d, body, T().rows[1].cells[1]) && j.size() == n0 + 1);
        check("redefine cell style: the style took the look", d.as<core::CellStyle>(body)->fill && *d.as<core::CellStyle>(body)->fill == sw &&
                                                              *d.as<core::CellStyle>(body)->valign == 1);
        check("redefine cell style: its other cells follow", T().rows[2].cells[0].fill == sw && T().rows[0].cells[0].valign == 1);
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& t) { t.rule_w = 3; });
        check("redefine table style: from the table", D::redefine_table_style(j, d, ts, T()) && d.as<core::TableStyle>(ts)->rule_w == 3);
        // overrides cleared
        D::edit_table(j, d, "story.1", p, "x", [](core::Table& t) { t.rule_w = 1; t.rows[2].cells[1].valign = 2; t.rows[0].cells[1].valign = 2; });
        auto t1 = T();
        D::clear_overrides(d, t1, 2, 1);
        check("clear: one cell back to its style, the rest kept", t1.rows[2].cells[1].valign == 1 && t1.rows[0].cells[1].valign == 2 && t1.rule_w == 1);
        auto t2 = T();
        D::clear_overrides(d, t2);
        check("clear: the whole table back to its style", t2.rule_w == 3 && t2.rows[0].cells[1].valign == 1 && t2.rows[2].cells[1].valign == 1);
    }
    // -- s348: lines set alone from a slice -- the same lines, quicker ---------------------
    {
        std::cout << "--- s348 line slicing ---\n";
        core::Doc d;
        (void)L::load_big_book(d, {.pages = 20});
        // every paragraph hyphenated (the line-by-line path), some long ones, a no-break span
        for (const auto& id : d.ids_of<core::Style>()) {
            core::Style s = *d.as<core::Style>(id);
            if (s.kind == core::StyleKind::Paragraph) { s.hyphenate = true; d.put(id, s); }
        }
        L::set_line_slicing(false);
        const auto whole = L::flow_story(d, "story.1");
        L::set_line_slicing(true);
        const auto sliced = L::flow_story(d, "story.1");
        bool same = whole.frames.size() == sliced.frames.size() && whole.overset == sliced.overset;
        int lines = 0, hy = 0;
        for (std::size_t k = 0; same && k < whole.frames.size(); ++k) {
            const auto& a = whole.frames[k].lines;
            const auto& b = sliced.frames[k].lines;
            same = a.size() == b.size();
            for (std::size_t i = 0; same && i < a.size(); ++i) {
                same = a[i].para == b[i].para && a[i].start == b[i].start && a[i].end == b[i].end && a[i].hyphen == b[i].hyphen &&
                       std::abs(a[i].y - b[i].y) < 1e-9;
                ++lines;
                hy += a[i].hyphen;
            }
        }
        check("slicing: every line the same as laid out from the whole rest", same && lines > 500 && hy > 0,
              std::to_string(lines) + " lines, " + std::to_string(hy) + " hyphenated");
    }
    // -- s348: a change keeps the flows it can't touch -- and they equal a fresh flow ------
    {
        std::cout << "--- s348 flows kept by their surroundings ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        (void)L::load_big_book(d, {.pages = 20});
        L::FlowCache fc;
        const auto stories = d.ids_of<core::Story>();
        auto all_get = [&] { for (const auto& s : stories) (void)fc.get(d, s); };
        auto same_as_fresh = [&](std::string& why) {
            for (const auto& s : stories) {
                const auto& a = fc.get(d, s);
                const auto b = L::flow_story(d, s);
                bool ok = a.frames.size() == b.frames.size() && a.overset == b.overset;
                for (std::size_t k = 0; ok && k < a.frames.size(); ++k) {
                    ok = a.frames[k].frame == b.frames[k].frame && a.frames[k].lines.size() == b.frames[k].lines.size();
                    for (std::size_t i = 0; ok && i < a.frames[k].lines.size(); ++i)
                        ok = a.frames[k].lines[i].para == b.frames[k].lines[i].para && a.frames[k].lines[i].start == b.frames[k].lines[i].start &&
                             a.frames[k].lines[i].end == b.frames[k].lines[i].end && std::abs(a.frames[k].lines[i].y - b.frames[k].lines[i].y) < 1e-9 &&
                             std::abs(a.frames[k].lines[i].x - b.frames[k].lines[i].x) < 1e-9;
                }
                if (!ok) { why = s; return false; }
            }
            return true;
        };
        all_get();
        const auto ms = d.ids_of<core::Master>();
        // a master's margins (no grid, no footnotes): the body kept, not laid out again
        D::edit_masters(j, d, {ms.front()}, "Top margin", [](core::Master& m) { m.margins.top += 9; });
        (void)fc.get(d, "story.1");
        check("kept: a master's margin -- the long story's flow kept", fc.last_how() == L::FlowCache::How::Kept);
        std::string why;
        check("kept: ... and it is what a fresh flow makes", same_as_fresh(why), why);
        // its baseline grid: laid out again
        D::edit_masters(j, d, ms, "Baseline grid", [](core::Master& m) { m.grid = 13; });
        (void)fc.get(d, "story.1");
        check("whole: a baseline grid -- laid out again", fc.last_how() == L::FlowCache::How::Whole);
        check("whole: ... the same as fresh", same_as_fresh(why), why);
        D::edit_masters(j, d, ms, "Baseline grid", [](core::Master& m) { m.grid = 0; });
        all_get();
        // a frame of the story moved / narrowed: laid out again, the same as fresh
        const auto chain = L::thread_frames(d, "story.1");
        D::edit_frames(j, d, {chain[3]}, "Narrow", [](core::Frame& f) { f.w -= 40; });
        (void)fc.get(d, "story.1");
        check("whole: a frame of its thread narrowed", fc.last_how() == L::FlowCache::How::Whole);
        check("whole: ... the same as fresh", same_as_fresh(why), why);
        // a frame with wrap dropped on one of its pages: laid out again
        const auto page = d.as<core::Frame>(chain[5])->owner;
        const auto pic = D::add_frame(j, d, page, core::FrameKind::Shape, {60, 120, 120, 90}, 1, 12, false, core::ShapeKind::Rect);
        D::edit_frames(j, d, {pic}, "Wrap", [](core::Frame& f) { f.wrap = core::Wrap::Around; });
        check("whole: a wrapping frame on its page -- the same as fresh", same_as_fresh(why), why);
        // a swatch, a guide: nothing laid out
        all_get();
        if (!d.ids_of<core::Swatch>().empty()) {
            const auto id = d.ids_of<core::Swatch>().front();
            auto sw = *d.as<core::Swatch>(id);
            sw.name += " 2";
            D::edit_swatch(j, d, id, sw);
            (void)fc.get(d, "story.1");
            check("kept: a swatch renamed", fc.last_how() == L::FlowCache::How::Kept);
        }
        // a style: everything again
        const auto sid = d.ids_of<core::Style>().front();
        D::edit_styles(j, d, {sid}, "Size", [](core::Style& st) { st.size += 1; });
        (void)fc.get(d, "story.1");
        check("whole: a style changed -- all laid out again", fc.last_how() == L::FlowCache::How::Whole);
        // undo it all, step by step: always the same as fresh
        bool undo_ok = true;
        while (j.can_undo() && undo_ok) { j.undo(); undo_ok = same_as_fresh(why); }
        check("undo, step by step: every flow as fresh", undo_ok, why);
    }
    // -- s348 (Scott's Fedora): a cache keyed by a Doc's address met a new Doc there --------
    {
        std::cout << "--- s348 a new Doc where an old one was ---\n";
        std::optional<core::Doc> slot;
        const auto make = [&](const char* story) {
            slot.emplace();
            core::Doc::Loading load(*slot);
            core::Frame f;
            f.owner = "page.1";
            f.kind = core::FrameKind::Text;
            f.story = story;
            slot->put("frame.1", f);
        };
        make("story.a");
        const auto a = L::thread_frames(*slot, "story.a");
        const auto rev = slot->revision();
        make("story.b");   // the same storage, the same revision, the same count -- another document
        check("new doc: the same address and revision", slot->revision() == rev);
        const auto b = L::thread_frames(*slot, "story.b");
        check("new doc: its own threads, not the old one's", a.size() == 1 && b.size() == 1 && L::thread_frames(*slot, "story.a").empty());
        core::Doc c = *slot;
        check("new doc: a copy is another document", c.serial() != slot->serial());
    }
    // -- s349: math in the number fields ---------------------------------------------
    {
        std::cout << "--- s349 math in fields ---\n";
        const auto near = [](std::optional<double> v, double want) { return v && std::abs(*v - want) < 1e-6; };
        check("math: plain lengths as before", near(core::parse_length("12", 1), 12) && near(core::parse_length("1in", 1), 72) &&
                                               near(core::parse_length("3p6", 1), 42) && near(core::parse_length("-3p6", 1), -42) &&
                                               near(core::parse_length("2", 72), 144) && near(core::parse_length("1\"", 1), 72));
        check("math: a sum of units", near(core::parse_length("1in + 6pt", 1), 78) && near(core::parse_length("10mm+1cm", 1), 20 * 72 / 25.4));
        check("math: a bare number beside a length is in the field's unit", near(core::parse_length("1in+1", 72), 144) &&
                                                                            near(core::parse_length("6pt + 1", 72), 78));
        check("math: times / divided -- a plain factor", near(core::parse_length("3p6*2", 1), 84) && near(core::parse_length("12pt/4", 1), 3) &&
                                                         near(core::parse_length("2 x 6pt", 1), 12) && near(core::parse_length("6*2", 72), 864));
        check("math: precedence, brackets", near(core::parse_length("1+2*3", 1), 7) && near(core::parse_length("(1+2)*3", 1), 9) &&
                                            near(core::parse_length("(210mm - 20mm) / 2", 1), 95 * 72 / 25.4));
        check("math: on the field's value -- +6, *2, /3; -6 stays minus six", near(core::parse_length("+6", 1, 12.0), 18) &&
                                                                              near(core::parse_length("*2", 1, 12.0), 24) &&
                                                                              near(core::parse_length("/3", 1, 12.0), 4) &&
                                                                              near(core::parse_length("-6", 1, 12.0), -6) &&
                                                                              near(core::parse_length("+1", 72, 72.0), 144));
        check("math: not lengths", !core::parse_length("1in*1in", 1) && !core::parse_length("6/0", 1) && !core::parse_length("2/1in", 1) &&
                                   !core::parse_length("(1+2", 1) && !core::parse_length("1++", 1) && !core::parse_length("abc", 1) &&
                                   !core::parse_length("12 ft", 1));
        check("math: percentages", near(core::parse_percent("50+10"), 60) && near(core::parse_percent("75/3%"), 25) &&
                                   near(core::parse_percent("+10", 40.0), 50) && !core::parse_percent("50-60") && !core::parse_percent("40pt"));
    }
    // -- s350: the baseline grid's start; a frame's Vertical over the grid ----------------
    {
        std::cout << "--- s350 grid start, Vertical over the grid ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.pages()[3];
        const auto mid = d.as<core::Page>(page)->master;
        const double top = core::margin_rect(d, page).y;
        D::edit_masters(j, d, {mid}, "Baseline grid", [](core::Master& m) { m.grid = 12; });
        auto g = core::baseline_grid(d, page);
        check("grid: from the top margin by default", std::abs(g.start - top) < 1e-9 && g.step == 12);
        D::edit_masters(j, d, {mid}, "Grid start", [](core::Master& m) { m.grid_start = 6; });
        g = core::baseline_grid(d, page);
        check("grid: its start below the margin", std::abs(g.start - (top + 6)) < 1e-9);
        D::edit_masters(j, d, {mid}, "Grid from", [](core::Master& m) { m.grid_from_page = true; m.grid_start = 30; });
        g = core::baseline_grid(d, page);
        check("grid: from the page's top", std::abs(g.start - 30) < 1e-9);
        {
            core::Doc e;
            check("grid: saved", core::from_json(e, core::to_json(d)) && e.as<core::Master>(mid)->grid_start == 30 && e.as<core::Master>(mid)->grid_from_page);
        }
        // a small frame of grid-aligned words, set Centre: its lines move off the grid now
        const auto fid = D::add_frame(j, d, page, core::FrameKind::Text, {100, 100, 200, 120});
        const auto sid = d.as<core::Frame>(fid)->story;
        D::edit_text(j, d, sid, {0, 0}, {0, 0}, "Running head");
        const auto ps = d.as<core::Story>(sid)->paras[0].style;
        if (!ps.empty()) D::edit_styles(j, d, {ps}, "Grid", [](core::Style& st) { st.align_grid = true; });
        const auto top_y = L::flow_story(d, sid).frames[0].lines.at(0).y;
        D::edit_frames(j, d, {fid}, "Vertical", [](core::Frame& f) { f.valign = core::VAlign::Center; });
        const auto mid_y = L::flow_story(d, sid).frames[0].lines.at(0).y;
        check("vertical: a grid-aligned line centred by its frame (was: the grid won)", mid_y > top_y + 20, std::to_string(top_y) + " -> " + std::to_string(mid_y));
    }
    // -- s351: a master's body frame, page frames following it ---------------------------
    {
        std::cout << "--- s351 body frames ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { auto out = D::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        (void)L::load_big_book(d, {.pages = 20});
        const auto chain = L::thread_frames(d, "story.1");
        // one frame set apart (a chapter opener, lower)
        D::edit_frames(j, d, {chain[2]}, "Opener", [](core::Frame& f) { f.y += 72; f.h -= 72; });
        const auto opener = *d.as<core::Frame>(chain[2]);
        const auto before = core::to_json(d);
        const int n = D::make_body_frames(j, d, "story.1");
        int bodies = 0;
        for (const auto& id : d.ids_of<core::Frame>()) bodies += d.as<core::Frame>(id)->body;
        check("make: body frames on the masters, the frames placed alike following", n >= static_cast<int>(chain.size()) - 3 && bodies >= 1,
              std::to_string(n) + " of " + std::to_string(chain.size()) + ", " + std::to_string(bodies) + " body");
        check("make: the opener left its own", !d.as<core::Frame>(chain[2])->follow_body && *d.as<core::Frame>(chain[2]) == opener);
        // the body frame narrowed: every follower narrows, the opener doesn't
        const auto page5 = d.as<core::Frame>(chain[5])->owner;
        const auto bid = D::body_frame_for(d, page5);
        check("body frame for a page", !bid.empty() && d.as<core::Frame>(bid)->body);
        const double w0 = d.as<core::Frame>(bid)->w;
        D::edit_frames(j, d, {bid}, "Narrow", [](core::Frame& f) { f.w -= 30; f.columns = 2; });
        bool all = true;
        int followed = 0;
        for (const auto& fid : L::thread_frames(d, "story.1")) {
            const auto& f = *d.as<core::Frame>(fid);
            if (!f.follow_body || D::body_frame_for(d, f.owner) != bid) continue;
            ++followed;
            all = all && std::abs(f.w - (w0 - 30)) < 1e-6 && f.columns == 2;
        }
        check("follow: the body frame narrowed, two columns -- its pages' frames with it, in the same step", all && followed > 3,
              std::to_string(followed));
        check("follow: the opener stays", d.as<core::Frame>(chain[2])->w == opener.w);
        j.undo();
        bool back = true;
        for (const auto& fid : L::thread_frames(d, "story.1"))
            if (d.as<core::Frame>(fid)->follow_body && D::body_frame_for(d, d.as<core::Frame>(fid)->owner) == bid) back = back && d.as<core::Frame>(fid)->columns == 1;
        check("follow: undone in one", back);
        {
            core::Doc e;
            check("body: saved", core::from_json(e, core::to_json(d)) && e.as<core::Frame>(bid)->body && e.as<core::Frame>(chain[5])->follow_body);
        }
        j.undo();
        check("make: undone", core::to_json(d) == before);
    }
    // -- s352: body frames keep to the margins; an import makes them -------------------
    {
        std::cout << "--- s352 body frames and margins ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { auto out = D::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        (void)L::load_big_book(d, {.pages = 20});
        D::make_body_frames(j, d, "story.1");
        const auto chain = L::thread_frames(d, "story.1");
        std::string fol;
        for (const auto& f : chain) if (d.as<core::Frame>(f)->follow_body) { fol = f; break; }
        const auto page = d.as<core::Frame>(fol)->owner;
        const auto bid = D::body_frame_for(d, page);
        const auto mid = d.as<core::Page>(page)->master;
        check("margins: a body frame made on the margins keeps to them", d.as<core::Frame>(bid)->fit_margins);
        const double y0 = d.as<core::Frame>(fol)->y;
        D::edit_masters(j, d, {mid}, "Top margin", [](core::Master& m) { m.margins.top += 18; });
        check("margins: the top margin down 18 -- the body frame and its pages' frames with it", std::abs(d.as<core::Frame>(fol)->y - (y0 + 18)) < 1e-6 &&
                                                                                           std::abs(d.as<core::Frame>(bid)->y - (y0 + 18)) < 1e-6,
              std::to_string(d.as<core::Frame>(fol)->y));
        D::edit_frames(j, d, {bid}, "By hand", [](core::Frame& f) { f.h -= 40; });
        check("margins: moved by hand -- it stops keeping to them", !d.as<core::Frame>(bid)->fit_margins);
        const double h1 = d.as<core::Frame>(fol)->h;
        D::edit_masters(j, d, {mid}, "Top margin", [](core::Master& m) { m.margins.top -= 18; });
        check("margins: ... a later margin leaves it (and its pages) be", std::abs(d.as<core::Frame>(fol)->h - h1) < 1e-6);
    }
    // -- s353: the grid made findable -- its rows on pages and frames, colour, threshold --
    {
        std::cout << "--- s353 grid findable ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.pages()[3];
        const auto mid = d.as<core::Page>(page)->master;
        std::string tf;
        for (const auto& f : d.frames_on(page)) if (!d.as<core::Frame>(f)->story.empty()) { tf = f; break; }
        check("grid: a page's and a frame's master found", core::master_id_of(d, page) == mid && core::master_id_of(d, mid) == mid &&
                                                           (tf.empty() || core::master_id_of(d, tf) == mid) && core::master_id_of(d, "nope").empty());
        const auto rows = [&](const std::string& id) {
            const auto o = core::project(d, "");
            std::vector<core::Field> out;
            if (const auto* it = o.find(id)) for (const auto& f : it->fields) if (f.edit.rfind("mgrid", 0) == 0 || f.label == "Snaps") out.push_back(f);
            return out;
        };
        auto pr = rows(page);
        check("grid: a page shows its master's grid row (none yet: the step only)", pr.size() == 1 && pr[0].edit == "mgrid" && pr[0].group == "Baseline Grid");
        D::edit_masters(j, d, {mid}, "Baseline grid", [](core::Master& m) { m.grid = 13; });
        pr = rows(page);
        bool colour = false, snaps = false;
        for (const auto& f : pr) { colour = colour || (f.edit == "mgrid_color" && f.value == "Light Blue"); snaps = snaps || f.label == "Snaps"; }
        check("grid: set -- start, from, colour and what snaps show on the page too", pr.size() == 5 && colour && snaps);
        check("grid: the colours", core::grid_colour("magenta").label == "Magenta" && core::grid_colour("").label == "Light Blue" &&
                                   core::grid_colour("plaid").label == "Light Blue" && core::grid_colours().size() >= 6);
        D::edit_masters(j, d, {mid}, "Grid colour", [](core::Master& m) { m.grid_color = "green"; });
        check("grid: its colour, on the page's rows", [&] { for (const auto& f : rows(page)) if (f.edit == "mgrid_color") return f.value == "Green"; return false; }());
        check("grid: the view threshold", D::set_grid_view(j, d, 75) && d.settings().grid_view == 75 && !D::set_grid_view(j, d, 75));
        {
            core::Doc e;
            check("grid: colour and threshold saved", core::from_json(e, core::to_json(d)) && e.as<core::Master>(mid)->grid_color == "green" &&
                                                     e.settings().grid_view == 75);
        }
        j.undo();
        check("grid: threshold undone", d.settings().grid_view == 0);
        const double lead = core::body_leading(d, "story.1");
        check("grid: the body's leading is its most-used style's", lead > 0, std::to_string(lead));
        check("grid: no story, no leading", core::body_leading(d, "story.nope") == 0);
    }
    // -- s354: the page size in one place; a page added takes the body's frame ---------
    {
        std::cout << "--- s354 book page size, pages added threaded ---\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { auto out = D::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        (void)L::load_big_book(d, {.pages = 20});
        const auto [w0, h0] = D::book_page_size(d);
        check("size: the book's page size is its masters'", w0 >= 36 && h0 >= 36, std::to_string(w0) + " x " + std::to_string(h0));
        // a cover master of its own size keeps it
        const auto ms = d.ids_of<core::Master>();
        std::string cover;
        if (ms.size() > 1) {
            cover = ms.back();
            D::set_master_size(j, d, cover, w0 + 36, h0 + 36);
        }
        const auto before = core::to_json(d);
        D::make_body_frames(j, d, "story.1");
        check("size: set in one step", D::set_book_page_size(j, d, 396, 612) && !D::set_book_page_size(j, d, 396, 612));
        bool all = true;
        for (const auto& p : d.pages()) {
            const auto* pg = d.as<core::Page>(p);
            if (pg->master == cover) continue;
            all = all && std::abs(pg->w - 396) < 1e-6 && std::abs(pg->h - 612) < 1e-6;
        }
        check("size: every page at the old size follows", all);
        check("size: a master of its own size keeps it", cover.empty() || std::abs(d.as<core::Master>(cover)->w - (w0 + 36)) < 1e-6);
        check("size: the trim follows", std::abs(d.settings().trim_w - 396) < 1e-6 && D::book_page_size(d).first == 396);
        {   // the body frames kept to the margins refit
            bool fits = true;
            for (const auto& f : d.ids_of<core::Frame>()) {
                const auto* x = d.as<core::Frame>(f);
                if (!x->body || !x->fit_margins) continue;
                const auto m = core::margin_rect(d, x->owner, x->side);
                fits = fits && std::abs(x->w - m.w) < 0.01;
            }
            check("size: the body frames refit to the new margins", fits);
        }
        check("size: book trims offered", D::book_trims().size() >= 6 && D::book_trims()[3].w == 432);
        j.undo();
        check("size: undone", D::book_page_size(d).first == w0);
        // a page added after one in the body's flow takes a frame, threaded in
        const auto chain = L::thread_frames(d, "story.1");
        std::string at;
        for (const auto& f : chain) if (d.as<core::Frame>(f)->follow_body) { at = f; }
        check("add page: a frame following the body to add after", !at.empty(), std::to_string(chain.size()) + " frames");
        if (at.empty()) return;
        const auto page = d.as<core::Frame>(at)->owner;
        const auto after = d.as<core::Frame>(at)->next;
        const auto snap = core::to_json(d);
        const auto made = D::add_pages(j, d, page, 1, d.as<core::Page>(page)->master);
        std::string nf;
        for (const auto& f : d.frames_on(made.front())) if (d.as<core::Frame>(f)->owner == made.front()) nf = f;
        check("add page: a frame from the body frame", !nf.empty() && d.as<core::Frame>(nf)->follow_body && d.as<core::Frame>(nf)->story == "story.1");
        check("add page: threaded in", !nf.empty() && d.as<core::Frame>(at)->next == nf && d.as<core::Frame>(nf)->next == after);
        check("add page: in the body frame's place", !nf.empty() && [&] {
            const auto* b = d.as<core::Frame>(D::body_frame_for(d, made.front()));
            const auto* x = d.as<core::Frame>(nf);
            return b && std::abs(b->x - x->x) < 0.01 && std::abs(b->w - x->w) < 0.01; }());
        check("add page: the story flows through it", L::thread_frames(d, "story.1").size() == chain.size() + 1);
        j.undo();
        {
            const auto now = core::to_json(d);
            std::string diff;
            core::Doc was;
            (void)core::from_json(was, snap);
            for (const auto& [id, r] : d.records()) if (core::record_json(d, id) != core::record_json(was, id)) diff += id + " ";
            for (const auto& [id, r] : was.records()) if (core::record_json(d, id).empty()) diff += "-" + id + " ";
            check("add page: undone in one", now == snap, diff);
        }
        // a page added where no body frame is followed: just the page
        core::Doc e;
        core::DocJournal je{e};
        e.set_door([&je] { return je.open(); });
        core::load_sample(e);
        const auto n0 = e.ids_of<core::Frame>().size();
        D::add_pages(je, e, e.pages().back(), 1, e.as<core::Page>(e.pages().back())->master);
        check("add page: no body frames, no frame made", e.ids_of<core::Frame>().size() == n0);
        (void)before;
    }
    // -- s355: an import's empty end pages (Scott's Hollow Oak: 168 - 216 blank after THE END) --
    {
        std::cout << "--- s355 an import takes back the pages its text gave up ---\n";
        const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
        const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:paragraph-properties fo:line-height="14pt"/><style:text-properties fo:font-size="11pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1"><style:page-layout-properties fo:page-width="5.5in" fo:page-height="8.5in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.75in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"/>
</office:master-styles></office:document-styles>)";
        std::string paras;
        for (int i = 0; i < 40; ++i)
            paras += "<text:p text:style-name=\"Standard\">The oak stood hollow at the edge of the wood, and the children went to it every evening after supper.</text:p>";
        paras += "<text:p text:style-name=\"Standard\">THE END</text:p>";
        const std::string content = "<office:document-content" + ns + R"(><office:body><office:text>)" + paras +
                                    "</office:text></office:body></office:document-content>";
        const auto odt = core::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
        std::string err;
        const auto book = core::odt::read(odt, &err);
        core::Doc d;
        const bool ok = book && L::import_odt(d, *book, nullptr, {}, &err);
        check("import: reflow adds and removes pages", ok && d.settings().reflow && d.settings().reflow_trim, err);
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { auto out = core::doc::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        const auto n0 = d.pages().size();
        // The text grows (as Scott's did with the grid double-spacing it), the reflow adds pages...
        core::Story st = *d.as<core::Story>("story.1");
        const auto was = st;
        const auto end = st.paras.back();
        st.paras.pop_back();
        for (int k = 0; k < 4; ++k) for (std::size_t i = 0; i < was.paras.size() - 1; ++i) st.paras.push_back(was.paras[i]);
        st.paras.push_back(end);
        j.run("Type", {"story.1"}, [&] { d.put("story.1", st); });
        for (int r = 0; r < 6 && L::flow_story(d, "story.1").overset; ++r) L::reflow_pages(j, d, "story.1");
        const auto n1 = d.pages().size();
        check("grown: pages added, not overset", ok && n1 > n0 + 2 && !L::flow_story(d, "story.1").overset,
              std::to_string(n0) + " -> " + std::to_string(n1));
        // ...then shrinks back: the pages after THE END are empty, and found
        j.run("Delete", {"story.1"}, [&] { d.put("story.1", was); });
        const auto empty = L::empty_reflow_pages(d, "story.1", L::flow_story(d, "story.1"));
        check("shrunk: the empty end pages are found (body frames, a grid)", ok && !empty.empty() && empty.size() >= n1 - n0 - 1,
              std::to_string(empty.size()) + " of " + std::to_string(n1 - n0));
        const int took = L::trim_reflow_pages(j, d, "story.1");
        const auto fl = L::flow_story(d, "story.1");
        check("trim: taken in one step, the last page holds THE END", took == static_cast<int>(empty.size()) && !fl.overset &&
                                                                         !fl.frames.empty() && !fl.frames.back().lines.empty(),
              std::to_string(took) + " taken, " + std::to_string(d.pages().size()) + " pages");
        j.undo();
        check("trim: undone in one", d.pages().size() == n1);
    }
    // -- s356: a long story's whole flow on the worker; a stand-in meanwhile --
    {
        std::cout << "--- s356 a master change: a stand-in, the whole from the worker ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        long_doc(d, j, 400);
        const auto chain = L::thread_frames(d, "story.1");
        check("long: more frames than the budget", chain.size() > 6, std::to_string(chain.size()));
        L::FlowCache fc;
        fc.set_reflow_budget(4);
        const auto before = fc.get(d, "story.1");
        check("first flow whole (nothing to stand in)", !before.provisional);
        const auto narrow = [&](core::Doc& dd, core::DocJournal& jj, double by) {   // every frame of the story (as a body frame's change does)
            core::doc::edit_frames(jj, dd, L::thread_frames(dd, "story.1"), "Width", [by](core::Frame& f) { f.w -= by; });
        };
        narrow(d, j, 18);
        const auto& si = fc.get(d, "story.1");
        check("a margin changed: the last flow stands in, provisional", si.provisional && si.frames.size() == L::thread_frames(d, "story.1").size());
        const auto un = fc.unfinished();
        check("unfinished: the story, whole", un.size() == 1 && un[0].whole && un[0].story == "story.1");
        L::FlowCache plain;   // no budget: whole here and now, as before
        const auto want = plain.get(d, "story.1");
        check("no budget: no stand-in", !want.provisional);
        fc.complete(d, "story.1", d.revision(), L::flow_story(d, "story.1"), *d.as<core::Story>("story.1"));
        const auto& got = fc.get(d, "story.1");
        bool same = !got.provisional && got.frames.size() == want.frames.size() && got.overset == want.overset;
        for (std::size_t i = 0; same && i < got.frames.size(); ++i) same = got.frames[i].lines.size() == want.frames[i].lines.size();
        check("handed in: the whole, as flowed here", same);
        // typed on while a stand-in shows: flowed whole (never re-flowed from the stand-in)
        narrow(d, j, -18);
        check("stand-in again", fc.get(d, "story.1").provisional);
        core::doc::edit_text(j, d, "story.1", {0, 3}, {0, 3}, "x");
        const auto& typed = fc.get(d, "story.1");
        L::FlowCache p2;
        check("typed over a stand-in: whole, not provisional", !typed.provisional && typed.frames.size() == p2.get(d, "story.1").frames.size());
        // a short story: whole here, no stand-in
        core::Doc e;
        core::DocJournal je{e};
        e.set_door([&je] { return je.open(); });
        long_doc(e, je, 10);
        L::FlowCache fe;
        fe.set_reflow_budget(4);
        (void)fe.get(e, "story.1");
        narrow(e, je, 6);
        check("a short story: flowed whole here", !fe.get(e, "story.1").provisional);
    }
    {
        std::cout << "--- s356 the pages reflow adds, worked out on a copy ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        long_doc(d, j, 300);
        {
            std::string more;
            for (int k = 0; k < 600; ++k) more += "The routes ran east along the coast, and the ships came in with the morning. ";
            set_text(d, j, more);
        }
        check("more text: overset", L::flow_story(d, "story.1").overset);
        const auto n0 = d.pages().size();
        const int need = L::reflow_pages_needed(d, "story.1");
        check("needed, the doc untouched", need > 0 && d.pages().size() == n0, std::to_string(need));
        const int added = L::add_reflow_pages(j, d, "story.1", need);
        check("added in one step: not overset", added == need && !L::flow_story(d, "story.1").overset &&
                                                    d.pages().size() == n0 + static_cast<std::size_t>(need));
        j.undo();
        check("undone in one", d.pages().size() == n0);
    }
    // -- s357: widows, orphans, runts found from a flow (set by hand: no fonts in it) --
    {
        std::cout << "--- s357 composition problems ---\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        long_doc(d, j, 60);
        const auto ch = L::thread_frames(d, "story.1");
        check("two frames to set in", ch.size() >= 2);
        core::Story st = *d.as<core::Story>("story.1");
        const std::string sty = st.paras[0].style;
        st.paras.clear();
        st.paras.push_back({sty, "Alpha beta gamma delta epsilon zeta eta theta."});      // 0: two lines, one column
        st.paras.push_back({sty, "Iota kappa lambda mu nu xi omicron pi rho sigma."});    // 1: first line alone at frame 0's foot
        st.paras.push_back({sty, "Tau upsilon phi chi psi omega alpha beta gamma end"});  // 2: last line one word ("end")
        st.paras.push_back({sty, "One two three four five six seven eight nine ten."});   // 3: last line alone, column 1's head
        st.paras.push_back({sty, "Short."});                                                // 4: one line -- never
        j.run("Type", {"story.1"}, [&] { d.put("story.1", st); });
        L::StoryFlow f;
        const auto line = [](int para, int a, int b, double y, int col = 0) { L::Line l; l.para = para; l.start = a; l.end = b; l.y = y; l.h = 12; l.col = col; return l; };
        L::FrameText f0, f1;
        f0.frame = ch[0];
        f1.frame = ch[1];
        f0.lines = {line(0, 0, 24, 0), line(0, 24, 46, 12), line(1, 0, 25, 24)};
        f1.lines = {line(1, 25, 40, 0), line(1, 40, 49, 12), line(2, 0, 26, 24), line(2, 26, 47, 36), line(2, 47, 50, 48),
                    line(3, 0, 17, 60), line(3, 17, 34, 72), line(3, 34, 50, 0, 1), line(4, 0, 6, 12, 1)};
        f.frames = {f0, f1};
        const auto ps = L::composition_problems(d, "story.1", f);
        const auto has = [&](L::CompProblem::Kind k, int para, const std::string& fr) {
            return std::any_of(ps.begin(), ps.end(), [&](const L::CompProblem& p) { return p.kind == k && p.para == para && p.frame == fr; });
        };
        std::string got;
        for (const auto& p : ps) got += std::string(L::comp_kind_name(p.kind)) + " " + std::to_string(p.para) + "; ";
        check("orphan: paragraph 1's first line at frame 0's foot", has(L::CompProblem::Orphan, 1, ch[0]), got);
        check("runt: paragraph 2's last line a word", has(L::CompProblem::Runt, 2, ch[1]), got);
        check("widow: paragraph 3's last line alone at column 1's head", has(L::CompProblem::Widow, 3, ch[1]), got);
        check("nothing else (a whole paragraph, a one-line one)", ps.size() == 3, got);
        const auto w = std::find_if(ps.begin(), ps.end(), [](const L::CompProblem& p) { return p.kind == L::CompProblem::Widow; });
        check("the widow's box: its line (y, height, a width)", w != ps.end() && w->box.y == 0 && w->box.h == 12 && w->box.w > 0);
    }
}
}  // namespace cxtest
