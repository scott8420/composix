// composix_selftest, part 4 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part4() {

    {
        std::cout << "\n-- stale contents (s089) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto why = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().list + ": " + v.front().why;
        };
        check("stale: the sample's extra line", why() == "list.1: \u201cSources\u201d listed, no such heading");
        L::build_list(j, d, "list.1");
        check("stale: built -- current", why() == "current");
        const auto toc = *d.as<core::Story>("story.9");
        const auto body = *d.as<core::Story>("story.1");
        {   // a written number that's wrong
            core::Doc::Loading load(d);
            auto st = toc;
            st.paras.at(2).text = "Chapter Two\t9";
            d.put("story.9", st);
        }
        check("stale: a page number moved", why() == "list.1: Chapter Two: page 9 → 3");
        {   // ... put back; a heading renamed
            core::Doc::Loading load(d);
            d.put("story.9", toc);
            auto st = body;
            for (auto& p : st.paras) if (p.text == "Chapter Two") p.text = "Chapter Deux";
            d.put("story.1", st);
        }
        check("stale: a heading renamed", why() == "list.1: \u201cChapter Deux\u201d not listed yet");
        {
            core::Doc::Loading load(d);
            d.put("story.1", body);
        }
        check("stale: put back -- current again", why() == "current");
        {
            auto l = *d.as<core::GenList>("list.1");
            l.built_entries = -1;
            core::Doc e = d;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            e.put("list.1", l);
            L::FlowCache f;
            const auto v = L::stale_lists(e, f);
            check("stale: never built", v.size() == 1 && v[0].why == "Not built yet");
        }
        // The outline and preflight say so.
        {
            core::Doc e;
            core::load_sample(e);
            L::FlowCache f;
            std::map<std::string, std::string> m;
            for (const auto& x : L::stale_lists(e, f)) m[x.list] = x.why;
            const auto o = core::project(e, "", nullptr, nullptr, &m);
            std::string built;
            for (const auto* it : o.in(core::Segment::Generated))
                if (it->id == "list.1")
                    for (const auto& fl : it->fields) if (fl.label == "Built") built = fl.value;
            check("outline: Built says what's out of date", built == "Out of date — \u201cSources\u201d listed, no such heading");
            const auto iss = L::preflight_issues(e, f, {});
            const auto it = std::find_if(iss.begin(), iss.end(), [](const L::Issue& i) { return i.kind == "stale_list"; });
            check("preflight: a warning at the contents frame, naming the list", it != iss.end() && it->level == L::Issue::Level::Warning &&
                                                                                it->item == "list.1" && it->frame == "frame.16" &&
                                                                                it->page == e.as<core::GenList>("list.1")->placed_on);
        }
        // Export's "rebuild first", kept.
        auto e = d.settings().exp;
        check("export: rebuild first by default", e.rebuild);
        e.rebuild = false;
        core::doc::set_export(j, d, e);
        std::string err;
        core::Doc f;
        check("json: rebuild first kept", core::from_json(f, core::to_json(d), &err) && !f.settings().exp.rebuild, err);
    }

    {
        std::cout << "\n-- list levels (s088) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto n0 = j.size();
        check("styles: Caption as level 2, one step", core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"}) &&
                                                      j.size() == n0 + 1 &&
                                                      d.as<core::GenList>("list.1")->from_styles == std::vector<std::string>{"style.2", "style.3"});
        check("styles: the same -- no step", !core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"}));
        check("styles: a character style or a repeat is dropped", core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.5", "style.2"}) &&
                                                                  d.as<core::GenList>("list.1")->from_styles == std::vector<std::string>{"style.2"});
        j.undo();
        L::FlowCache flows;
        const auto en = L::list_entries(d, flows, "list.1");
        bool fig = false;
        for (const auto& e : en) fig = fig || (e.level == 2 && e.text.find("Fig. 1") == 0);
        check("entries: the caption at level 2, in reading order", en.size() == 3 && en[0].level == 1 && en[1].level == 1 && fig &&
                                                                   en[2].level == 2);
        const auto entry1 = d.as<core::Story>("story.9")->paras.at(1).style;
        const auto styles0 = d.ids_of<core::Style>().size();
        check("build: one step, with a level-2 style made", L::build_list(j, d, "list.1") && d.ids_of<core::Style>().size() == styles0 + 1);
        const auto& ps = d.as<core::Story>("story.9")->paras;
        const auto* l = d.as<core::GenList>("list.1");
        const auto* s2 = l->entry_styles.size() == 2 ? d.as<core::Style>(l->entry_styles[1]) : nullptr;
        const auto* s1 = d.as<core::Style>(entry1);
        check("build: the new style -- named for level 2, based on level 1, indented", s2 && s2->name == s1->name + " 2" &&
              s2->based_on == entry1 && s2->left_indent == s1->left_indent + 12 && s2->own == std::vector<std::string>{"left_indent", "tabs"} &&
              !s2->tabs.empty() && s2->tabs.front().pos == s1->tabs.front().pos - 12);
        check("build: level 2's entry in it, level 1's as before", ps.size() == 4 && ps[1].style == entry1 && ps[3].style == l->entry_styles[1] &&
                                                                   ps[3].text.find("Fig. 1") == 0);
        const auto styles1 = d.ids_of<core::Style>().size();
        check("rebuild: the style found again, not made twice", !L::build_list(j, d, "list.1") && d.ids_of<core::Style>().size() == styles1);
        j.undo();
        check("undo: the made style goes too", d.ids_of<core::Style>().size() == styles0);
        j.redo();
        // Bookmarks nest the same way.
        L::FlowCache f2;
        const auto bm = L::bookmarks(d, f2, {"list.1"});
        check("bookmarks: the caption under its chapter", bm.size() == 3 && bm[2].level == 2);
        std::string err;
        core::Doc f;
        check("json: the levels' styles kept", core::from_json(f, core::to_json(d), &err) &&
                                               f.as<core::GenList>("list.1")->entry_styles == d.as<core::GenList>("list.1")->entry_styles, err);
    }

    {
        std::cout << "\n-- list build (s087) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const auto en = L::list_entries(d, flows, "list.1");
        check("entries: each heading and its folio", en == std::vector<core::doc::ListEntry>{{"Chapter One", "1"}, {"Chapter Two", "3"}});
        const auto before = d.as<core::Story>("story.9")->paras;
        const auto n0 = j.size();
        check("build: one step", L::build_list(j, d, "list.1") && j.size() == n0 + 1);
        const auto& ps = d.as<core::Story>("story.9")->paras;
        check("build: the title kept, an entry a heading, in the entry style",
              ps.size() == 3 && ps[0].text == "Contents" && ps[1].text == "Chapter One\t1" && ps[2].text == "Chapter Two\t3" &&
              ps[1].style == before[1].style && ps[2].style == before[1].style);
        check("build: counted -- not stale", d.as<core::GenList>("list.1")->built_entries == 2);
        check("build: again, no change -- no step", !L::build_list(j, d, "list.1") && j.size() == n0 + 1);
        L::FlowCache f2;
        check("build: the contents links still found", L::contents_links(d, f2).size() == 2);
        j.undo();
        check("build: undone -- as it was", d.as<core::Story>("story.9")->paras == before);
        j.redo();
        // A Contents list with no story: Build makes one, in a frame on its page.
        {
            auto l = *d.as<core::GenList>("list.1");
            l.story.clear();
            l.placed_on = d.pages()[0];
            core::Doc e = d;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            e.put("list.1", l);
            const auto frames0 = e.frames_on(l.placed_on).size();
            check("build, no story: made", L::build_list(je, e, "list.1") && je.size() == 1);
            const auto* nl = e.as<core::GenList>("list.1");
            const auto* ns = nl ? e.as<core::Story>(nl->story) : nullptr;
            check("build, no story: a story with the entries, in a new frame on its page",
                  ns && ns->paras.size() == 2 && ns->paras[0].text == "Chapter One\t1" &&
                  e.frames_on(l.placed_on).size() == frames0 + 1);
            const auto mr = core::margin_rect(e, l.placed_on);
            bool inside = false;
            for (const auto& fid : e.frames_on(l.placed_on))
                if (const auto* f = e.as<core::Frame>(fid); f->story == nl->story)
                    inside = mr.w < 1 || (f->x == mr.x && f->y == mr.y && f->w == mr.w);
            check("build, no story: the frame across the margins", inside);
            je.undo();
            check("build, no story: undone -- story and frame gone", e.as<core::GenList>("list.1")->story.empty() &&
                                                                    e.frames_on(l.placed_on).size() == frames0);
            l.placed_on.clear();
            e.put("list.1", l);
            check("build: no story, no page -- nothing", !L::build_list(je, e, "list.1"));
        }
        std::string err;
        core::Doc f;
        check("json: the list's story kept", core::from_json(f, core::to_json(d), &err) && f.as<core::GenList>("list.1")->story == "story.9", err);
    }

    {
        std::cout << "\n-- export summary (s086) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache flows;
        L::PdfOptions o;
        const auto has = [](const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; };
        auto s = L::export_summary(d, flows, o, true, "");
        check("summary: every page, its size and bleed", s.pages == 16 && s.sheets == 16 && has(s.size, "16 pages · ") &&
                                                         !has(s.size, "sheet") && has(s.size, "bleed"));
        check("summary: no profile -- device CMYK", has(s.colour, "Device CMYK"));
        check("summary: the bookmarks and links counted (s123: and the sample's note's two)",
              s.bookmarks == 2 && s.links == 2 && s.note_links == 2 && s.nav == "2 bookmarks · 2 contents links · 2 note links", s.nav);
        o.spreads = true;
        s = L::export_summary(d, flows, o, true, "");
        check("summary: spreads -- pages to sheets", s.sheets == 9 && has(s.size, "16 pages → 9 sheets"));
        o.only = core::pages_in_range(d, "ii-7");
        o.mark_lists = {"list.1", "list.2"};
        s = L::export_summary(d, flows, o, true, "");
        check("summary: a range -- its pages, its sheets, its bookmarks", s.pages == 10 && s.sheets == 5 && s.bookmarks == 2);
        o.only.clear();
        s = L::export_summary(d, flows, o, true, "");
        check("summary: both lists -- the group and its entries", s.bookmarks == 6);
        o.convert = true;
        o.perceptual = true;
        s = L::export_summary(d, flows, o, true, "Press");
        check("summary: converting, and how", s.colour == "CMYK for Press — RGB converted, perceptual");
        o.convert = false;
        o.standard = L::plate_standard::X4;
        s = L::export_summary(d, flows, o, true, "Press");
        check("summary: PDF/X-4 -- RGB kept, links left out", has(s.colour, "PDF/X-4 · ") && has(s.colour, "RGB kept") &&
                                                              s.links == 0 && has(s.nav, "left out"));
        o.standard = L::plate_standard::X1a;
        check("summary: X-1a always converts", has(L::export_summary(d, flows, o, true, "Press").colour, "RGB converted"));
        o.standard = L::plate_standard::None;
        o.bookmarks = false;
        s = L::export_summary(d, flows, o, false, "Press");
        check("summary: screen is RGB; bookmarks off -- none", s.colour == "RGB, for screen" && s.bookmarks == 0 &&
                                                              has(s.nav, "No bookmarks"));
    }

    {
        std::cout << "\n-- export intent (s085) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto e = d.settings().exp;
        check("intent: relative colorimetric by default", !e.perceptual);
        e.perceptual = true;
        const auto n0 = j.size();
        check("intent: kept with the document, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 && d.settings().exp.perceptual);
        j.undo();
        check("intent: undone", !d.settings().exp.perceptual);
        j.redo();
        std::string err;
        core::Doc f;
        check("json: the intent kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.perceptual, err);
        core::Doc g, h;
        core::load_sample(g);
        const auto js = core::to_json(g);
        check("json: relative -> no key, read back relative", js.find("\"perceptual\"") == std::string::npos &&
                                                              core::from_json(h, js, &err) && !h.settings().exp.perceptual, err);
        // The export says which intent it converted with (when a profile is installed).
        std::string icc;
        for (const auto& p : plate::find_cmyk_profiles({"/usr/share/color/icc", "/usr/share/ghostscript/iccprofiles",
                                                        "/usr/share/texlive/texmf-dist/tex/generic/colorprofiles",
                                                        "/usr/share/texmf/tex/generic/colorprofiles"}))
            if (icc.empty()) icc = p.path;
        if (icc.empty()) {
            std::cout << "  (skip intent export: no CMYK profile installed)\n";
        } else {
            namespace L = composix::layout;
            const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
            L::PdfOptions opt;
            opt.profile = icc;
            opt.convert = true;
            opt.perceptual = true;
            opt.only = core::pages_in_range(d, "iii");
            L::PdfReport rep;
            const auto out = (std::filesystem::temp_directory_path() / "composix_selftest_s085.pdf").string();
            const bool ok = L::export_print_pdf(d, out, opt, src, &rep, &err);
            check("export: the report names the intent", ok && !rep.warnings.empty() &&
                                                         rep.warnings.front().find("perceptual") != std::string::npos, err);
            std::error_code ec;
            std::filesystem::remove(out, ec);
        }
    }

    {
        std::cout << "\n-- PDF spreads (s084) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto pages = d.pages();
        const auto singles = L::pdf_sheets(d, pages, false);
        check("sheets: one a page without spreads", singles.size() == pages.size() && singles[1].label == d.folio(pages[1]) &&
                                                    singles[1].pages.size() == 1 && singles[1].x[0] == 0);
        const auto sp = L::pdf_sheets(d, pages, true);
        const auto pw = [&](const std::string& p) { return d.as<core::Page>(p)->w; };
        bool same = sp.size() == core::spreads(d).size();
        for (std::size_t k = 0; same && k < sp.size(); ++k) same = sp[k].pages == core::spreads(d)[k];
        check("sheets: one a spread, as the Pages view pairs them", same);
        check("sheets: the first page alone", !sp.empty() && sp[0].pages.size() == 1 && sp[0].label == "i");
        check("sheets: side by side, widths added, labelled", sp.size() > 1 && sp[1].pages.size() == 2 &&
              sp[1].x[1] == pw(sp[1].pages[0]) && sp[1].w == pw(sp[1].pages[0]) + pw(sp[1].pages[1]) &&
              sp[1].label == "ii–iii");
        bool wide = false;   // the foldout's spread is wider than two plain pages
        for (const auto& s : sp) wide = wide || s.w > 2 * pw(pages[0]) + 1;
        check("sheets: a foldout widens its spread", wide);
        const auto cut = L::pdf_sheets(d, core::pages_in_range(d, "iii-1"), true);
        check("sheets: a range keeps just its pages of each spread", cut.size() == 2 && cut[0].label == "iii" &&
                                                                     cut[1].label == "iv–1");
        const auto at = L::sheet_of(sp, core::pages_in_range(d, "1").front());
        check("sheet_of: page 1 -- the iv-1 sheet, right of iv", at.sheet >= 0 && sp[static_cast<std::size_t>(at.sheet)].label == "iv–1" &&
                                                                 at.x == pw(core::pages_in_range(d, "iv").front()));
        check("sheet_of: a page not written", L::sheet_of(cut, pages[0]).sheet == -1);
        {
            core::Doc e = d;
            auto st = e.settings();
            st.facing = false;
            e.put("doc", st);
            for (const auto& mid : e.ids_of<core::Master>()) {   // s147: facing is each master's
                auto m = *e.as<core::Master>(mid);
                m.facing = false;
                e.put(mid, m);
            }
            check("sheets: no facing pages -- one a page anyway", L::pdf_sheets(e, e.pages(), true).size() == e.pages().size());
        }
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s084";
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
            for (auto at2 = hay.find(n); at2 != std::string::npos; at2 = hay.find(n, at2 + 1)) ++k;
            return k;
        };
        L::PdfOptions opt;
        opt.compress = false;
        opt.spreads = true;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "spreads.pdf").string();
        const bool ok = L::export_print_pdf(d, out, opt, src, &rep, &err);
        const auto pdf = slurp(out);
        const double b = d.settings().bleed;
        char box[96];
        std::snprintf(box, sizeof box, "/MediaBox [0 0 %g %g]", 2 * pw(pages[1]) + 2 * b, d.as<core::Page>(pages[1])->h + 2 * b);
        check("print: one PDF page a spread, its pages still counted", ok && count(pdf, "/Type /Page /Parent") == static_cast<int>(sp.size()) &&
                                                                     rep.pages == static_cast<int>(pages.size()), err);
        check("print: a spread's media is both pages and the outer bleed", pdf.find(box) != std::string::npos);
        L::FlowCache flows;
        const auto lk = L::contents_links(d, flows);
        const auto iii = L::sheet_of(sp, lk.empty() ? std::string() : lk[0].page);
        char rect[48];
        std::snprintf(rect, sizeof rect, "/Rect [%g ", b + iii.x + (lk.empty() ? 0 : lk[0].box.x));
        check("print: the contents links moved with their page", !lk.empty() && iii.x > 0 &&
                                                                count(pdf, "/Subtype /Link") == 2 + static_cast<int>(L::note_links(d, flows).size()) &&
                                                                pdf.find(rect) != std::string::npos);
        check("print: the outline still there", pdf.find("(Chapter One)") != std::string::npos);
        L::PdfOptions so;
        so.spreads = true;
        const auto sout = (base / "spreads_screen.pdf").string();
        check("screen: spreads too", L::export_pdf(d, sout, so, src, &rep, &err) && rep.pages == static_cast<int>(pages.size()), err);
        fs::remove_all(base, ec);
        // Kept with the document.
        auto e = d.settings().exp;
        check("export: spreads off by default", !e.spreads);
        e.spreads = true;
        const auto n0 = j.size();
        check("export: spreads kept, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 && d.settings().exp.spreads);
        core::Doc f;
        check("json: spreads kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.spreads, err);
        core::Doc g, h;
        core::load_sample(g);
        const auto js = core::to_json(g);
        check("json: off -> no key, read back off", js.find("\"spreads\"") == std::string::npos && core::from_json(h, js, &err) &&
                                                    !h.settings().exp.spreads, err);
    }

    {
        std::cout << "\n-- contents links (s083) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache flows;
        const auto links = L::contents_links(d, flows);
        const auto toc = L::bookmarks(d, flows, {"list.1"});
        const auto* fr = d.as<core::Frame>("frame.16");
        check("links: the two chapter entries (Sources names no heading)", links.size() == 2 && toc.size() == 2);
        bool on_page = links.size() == 2, inside = on_page, to_heads = on_page;
        for (std::size_t k = 0; k < links.size(); ++k) {
            const auto& b = links[k].box;
            on_page = on_page && links[k].page == d.as<core::GenList>("list.1")->placed_on && d.folio(links[k].page) == "iii";
            inside = inside && b.w > 0 && b.h > 0 && b.x >= fr->x - 0.01 && b.r() <= fr->x + fr->w + 0.01 &&
                     b.y >= fr->y - 0.01 && b.b() <= fr->y + fr->h + 0.01;
            to_heads = to_heads && links[k].to == toc[k].page && links[k].to_y == toc[k].y;
        }
        check("links: on the contents page, inside its frame", on_page && inside);
        check("links: each to where its heading lands", to_heads && d.folio(links[0].to) == "1" && d.folio(links[1].to) == "3");
        check("links: in order down the page", links.size() == 2 && links[0].box.b() <= links[1].box.y + 0.01);
        {   // A second "Chapter One" entry: one heading, so one link.
            auto st = *d.as<core::Story>("story.9");
            st.paras.push_back(st.paras.at(1));
            core::Doc e = d;
            e.put("story.9", st);
            L::FlowCache f2;
            check("links: the k-th entry to the k-th heading -- no second target", L::contents_links(e, f2).size() == 2);
            auto l = *e.as<core::GenList>("list.1");
            l.placed_on.clear();
            e.put("list.1", l);
            L::FlowCache f3;
            check("links: a list not placed -- none", L::contents_links(e, f3).empty());
        }
        const auto base = fs::temp_directory_path() / "composix_selftest_s083";
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
        {   // Plate: an invisible /Link annotation, a missing page dropped.
            plate::Document pd;
            pd.set_compress(false);
            pd.add_page(100, 200);
            pd.add_page(100, 200);
            pd.add_link(0, 10, 20, 30, 12, 1, 50);
            pd.add_link(0, 10, 40, 30, 12, 7, 0);   // no such page
            const auto f = (base / "link.pdf").string();
            std::string err;
            const bool ok = pd.save(f, &err);
            const auto p = slurp(f);
            check("plate: a link -- its box flipped, going to the page", ok && count(p, "/Subtype /Link") == 1 &&
                  p.find("/Rect [10 168 40 180]") != std::string::npos && p.find("/XYZ 0 150 null") != std::string::npos &&
                  p.find("/Border [0 0 0]") != std::string::npos && p.find("/Annots [") != std::string::npos, err);
        }
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions opt;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "toc.pdf").string();
        check("print: the contents page's two links", L::export_print_pdf(d, out, opt, src, &rep, &err) &&
                                                      count(slurp(out), "/Subtype /Link") == 2 + [&] {   // s123: and the notes'
                                                          L::FlowCache nf;
                                                          return static_cast<int>(L::note_links(d, nf).size());
                                                      }(), err);
        opt.only = core::pages_in_range(d, "iii");
        check("print: the targets not written -- no links", L::export_print_pdf(d, out, opt, src, &rep, &err) &&
                                                            count(slurp(out), "/Subtype /Link") == 0, err);
        L::PdfOptions so;
        const auto sout = (base / "toc_screen.pdf").string();
        check("screen: the links too", L::export_pdf(d, sout, so, src, &rep, &err) && count(slurp(sout), "/Link") >= 2, err);
        fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- bookmarks from several lists (s082) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("lists: the ones gathering styles, in list order", L::bookmark_lists(d) == std::vector<std::string>{"list.1", "list.2"});
        const auto toc = L::bookmarks(d, flows, {"list.1"});
        check("default (none picked) = the contents, as s054", L::bookmarks(d, flows, {}) == L::bookmarks(d, flows) &&
                                                               L::bookmarks(d, flows, {}) == toc && !toc.empty());
        check("a list that's gone -> the default", L::bookmarks(d, flows, {"list.99"}) == toc);
        const auto bib = L::bookmarks(d, flows, {"list.2"});
        const bool bib_flat = !bib.empty() && std::all_of(bib.begin(), bib.end(), [](const L::Bookmark& b) { return b.level == 1; });
        bool chap = false;
        for (const auto& b : bib) chap = chap || b.title.find("Chapter") != std::string::npos;
        check("just the bibliography: its entries, at the top level", bib_flat && !chap && bib.size() == 3);
        check("two columns: in story order, not by height", bib.size() == 3 && bib[0].title.find("Almanac") == 0 &&
                                                            bib[1].title.find("Harbor") == 0 && bib[2].title.find("Letters") == 0);
        const auto both = L::bookmarks(d, flows, {"list.1", "list.2"});
        const bool grouped = both.size() == toc.size() + 1 + bib.size() &&
                             std::equal(toc.begin(), toc.end(), both.begin()) &&
                             both[toc.size()].title == "Bibliography" && both[toc.size()].level == 1 &&
                             both[toc.size()].page == bib.front().page && both[toc.size()].y == bib.front().y;
        bool nested = grouped;
        for (std::size_t k = 0; grouped && k < bib.size(); ++k)
            nested = nested && both[toc.size() + 1 + k].title == bib[k].title && both[toc.size() + 1 + k].level == 2;
        check("both: the contents on top, the bibliography under its name", grouped && nested);
        check("the lists' order, not the picking order", L::bookmarks(d, flows, {"list.2", "list.1"}) == both);
        d.put("list.3", core::GenList{"Index", core::ListKind::Index, 3, {}, "", -1});
        check("a list gathering no styles isn't offered", L::bookmark_lists(d).size() == 2);
        // A range without the bibliography's page: no empty group.
        std::vector<std::string> early;
        for (const auto& p : d.pages()) if (p != bib.front().page) early.push_back(p);
        const auto cut = L::bookmarks(d, flows, {"list.1", "list.2"}, early);
        bool no_bib = true;
        for (const auto& b : cut) no_bib = no_bib && b.page != bib.front().page;
        check("a range: entries off it left out, a group only where it has some", no_bib &&
              (cut.empty() || cut.back().title != "Bibliography"));
        // Into the PDFs.
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s082";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        L::PdfOptions opt;
        opt.compress = false;
        opt.mark_lists = {"list.1", "list.2"};
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "lists.pdf").string();
        const bool okp = L::export_print_pdf(d, out, opt, src, &rep, &err);
        const auto pdf = slurp(out);
        check("print: the group and its entries in the outline", okp && pdf.find("(Bibliography)") != std::string::npos &&
                                                                 pdf.find("(Chapter One)") != std::string::npos &&
                                                                 pdf.find("(Letters of the Surveyor General.)") != std::string::npos, err);
        L::PdfOptions so;
        so.mark_lists = {"list.2"};
        const auto sout = (base / "lists_screen.pdf").string();
        check("screen: exports from the bibliography alone", L::export_pdf(d, sout, so, src, &rep, &err) &&
                                                             slurp(sout).find("/Outlines") != std::string::npos, err);
        fs::remove_all(base, ec);
        // Kept with the document: one step, and in the file.
        auto e = d.settings().exp;
        check("export: no lists picked by default", e.mark_lists.empty());
        e.mark_lists = {"list.1", "list.2"};
        const auto n0 = j.size();
        check("export: the lists kept, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 &&
                                                  d.settings().exp.mark_lists == e.mark_lists);
        core::Doc f;
        check("json: the lists kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.mark_lists == e.mark_lists, err);
        core::Doc g;
        core::load_sample(g);
        const auto js = core::to_json(g);
        core::Doc h;
        check("json: none picked -> no key, read back as none", js.find("mark_lists") == std::string::npos &&
                                                                core::from_json(h, js, &err) && h.settings().exp.mark_lists.empty(), err);
    }

    {
        std::cout << "\n-- paste through Next style (s081) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Two paragraph styles: A -> next B, B -> next B (none set: itself).
        core::Doc::Id A, B;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Paragraph) { if (A.empty()) A = id; else if (B.empty() && id != A) B = id; }
        check("sample: two paragraph styles", !A.empty() && !B.empty());
        core::doc::edit_styles(j, d, {A}, "Next style", [&](core::Style& s) { s.next = B; });
        core::doc::edit_styles(j, d, {B}, "Next style", [&](core::Style& s) { s.next.clear(); });
        // A paragraph in A, at its end: paste three lines.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        const int end = static_cast<int>(d.as<core::Story>("story.1")->paras.at(1).text.size());
        const auto n0 = d.as<core::Story>("story.1")->paras.size();
        core::doc::edit_text(j, d, "story.1", {1, end}, {1, end}, "\nTitle two\nBody one\nBody two", "Paste");
        const auto& ps = d.as<core::Story>("story.1")->paras;
        check("paste: three new paragraphs", ps.size() == n0 + 3);
        check("paste at an end: A, then its Next (B), then B's (B)", ps.at(1).style == A && ps.at(2).style == B &&
                                                                     ps.at(3).style == B && ps.at(4).style == B);
        // Into the middle of an A paragraph: the last piece is that paragraph's rest -- it stays A.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        core::doc::edit_text(j, d, "story.1", {1, 3}, {1, 3}, "x\ny\nz", "Paste");
        const auto& p2 = d.as<core::Story>("story.1")->paras;
        check("paste mid-paragraph: the middle takes Next, the rest keeps A", p2.at(1).style == A && p2.at(2).style == B && p2.at(3).style == A);
        // Enter alone, as before: at the end -> Next; in the middle -> the same.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        const int e1 = static_cast<int>(d.as<core::Story>("story.1")->paras.at(1).text.size());
        core::doc::edit_text(j, d, "story.1", {1, e1}, {1, e1}, "\n");
        check("Enter at the end: Next style", d.as<core::Story>("story.1")->paras.at(2).style == B);
        core::doc::edit_text(j, d, "story.1", {1, 2}, {1, 2}, "\n");
        check("Enter in the middle: the same style", d.as<core::Story>("story.1")->paras.at(2).style == A);
    }

    {
        std::cout << "\n-- New sheet memory (s079) --\n";
        core::NewDoc n;
        n.trim_w = 5.5 * core::kPt;
        n.trim_h = 8.5 * core::kPt;
        n.pages = 48;
        n.facing = false;
        n.margins = {40, 50, 60, 70};
        n.columns = 2;
        n.gutter = 14;
        n.bleed = 0;
        n.text_frames = false;
        n.folios = false;
        n.units = core::Units::Millimetres;
        core::AppSettings st{{"window.width", "1400"}};
        core::new_to_settings(n, st);
        auto back = core::new_from_settings(st);
        back.title = n.title;
        check("new sheet: every choice comes back", back == n);
        check("new sheet: other settings left alone", st.at("window.width") == "1400");
        check("new sheet: nothing saved -> the defaults", core::new_from_settings({}) == core::NewDoc{});
        core::AppSettings bad{{"new.pages", "lots"}, {"new.trim_w", "1e9"}, {"new.units", "7"}, {"new.columns", "3.5x"}};
        const auto b = core::new_from_settings(bad);
        check("new sheet: unreadable keys keep defaults, numbers held in range",
              b.pages == core::NewDoc{}.pages && b.trim_w == 5000 && b.units == core::Units::Points && b.columns == core::NewDoc{}.columns);
        core::Doc d;
        check("new document: in the chosen unit", core::new_document(d, n) && d.settings().units == core::Units::Millimetres);
    }

    {
        std::cout << "\n-- saved queries (s078) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("sample: one saved query", d.settings().queries.size() == 1 && d.settings().queries[0].name == "Double spaces");
        core::SavedQuery q{"  Dashes ", "--", "\u2014"};
        q.mode = 1;
        check("save: one step, the name trimmed, kept by name", core::doc::save_query(j, d, q) && j.size() == 1 &&
                                                                 d.settings().queries.size() == 2 && d.settings().queries[0].name == "Dashes");
        check("save: the same again is no step", !core::doc::save_query(j, d, d.settings().queries[0]));
        q.name = "Dashes";
        q.with = "\u2013";
        check("save: the same name replaces", core::doc::save_query(j, d, q) && d.settings().queries.size() == 2 &&
                                              d.settings().queries[0].with == "\u2013");
        check("save: no name, no query", !core::doc::save_query(j, d, {"   ", "x"}));
        std::string err;
        core::Doc e;
        check("json: queries kept, every setting", core::from_json(e, core::to_json(d), &err) && e.settings().queries == d.settings().queries, err);
        check("delete: one step; unknown name none", core::doc::delete_query(j, d, "Dashes") && d.settings().queries.size() == 1 &&
                                                     !core::doc::delete_query(j, d, "Dashes"));
        j.undo();
        check("undo: back", d.settings().queries.size() == 2);
    }

    {
        std::cout << "\n-- export memory (s077) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("export: all pages and bookmarks by default", d.settings().exp.range.empty() && d.settings().exp.bookmarks);
        auto e = d.settings().exp;
        e.range = "iii-7, 12";
        e.bookmarks = false;
        check("export: the range and bookmarks kept, one step", core::doc::set_export(j, d, e) && j.size() == 1 &&
                                                                d.settings().exp.range == "iii-7, 12" && !d.settings().exp.bookmarks);
        check("export: the same again is no step", !core::doc::set_export(j, d, e));
        std::string err;
        core::Doc f;
        check("json: range and bookmarks kept", core::from_json(f, core::to_json(d), &err) &&
                                                f.settings().exp.range == "iii-7, 12" && !f.settings().exp.bookmarks, err);
        check("json: an older file (no keys) -> all pages, bookmarks on", [] {
            core::Doc g;
            core::load_sample(g);
            auto js = core::to_json(g);   // (keys are written sorted: cut the two new ones out)
            bool cut = false;
            for (const std::string k : {"\"bookmarks\": true,", "\"range\": \"\","})
                if (const auto at = js.find(k); at != std::string::npos) { js.erase(at, k.size()); cut = true; }
            core::Doc h;
            std::string err2;
            return cut && js.find("\"bookmarks\"") == std::string::npos && core::from_json(h, js, &err2) &&
                   h.settings().exp.range.empty() && h.settings().exp.bookmarks;
        }());
        check("export: the kept range still reads", !core::pages_in_range(d, d.settings().exp.range).empty());
    }

    {
        std::cout << "\n-- picture offsets (s076) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& f0 = *d.as<core::Frame>("frame.11");
        const auto& a = *d.as<core::Asset>(f0.asset);
        const double iw = a.px_w > 0 ? a.px_w : 1, ih = a.px_h > 0 ? a.px_h : 1;
        const auto r0 = core::picture_rect({0, 0, f0.w, f0.h}, f0, iw, ih);
        check("sample: the lighthouse is fitted, not by hand", f0.fit != core::ImageFit::Custom);
        check("offset X: one step, by hand, Y and size kept",
              core::doc::set_picture_offset(j, d, {"frame.11"}, 0, -6) && j.size() == 1);
        const auto& f1 = *d.as<core::Frame>("frame.11");
        const auto r1 = core::picture_rect({0, 0, f1.w, f1.h}, f1, iw, ih);
        check("offset X: there", f1.fit == core::ImageFit::Custom && std::abs(r1.x + 6) < 1e-9 && std::abs(r1.y - r0.y) < 1e-9 &&
                                 std::abs(r1.w - r0.w) < 1e-9);
        check("offset Y: there", core::doc::set_picture_offset(j, d, {"frame.11"}, 1, 4) &&
                                 std::abs(d.as<core::Frame>("frame.11")->pic_y - 4) < 1e-9);
        check("offset: the same again is no step", !core::doc::set_picture_offset(j, d, {"frame.11"}, 1, 4));
        check("offset: not on a text frame", !core::doc::set_picture_offset(j, d, {"frame.1"}, 0, 1));
        const auto items = core::frame_items(d, {"frame.11"});
        const core::Field* off = nullptr;
        for (const auto& f : items.at(0).fields) if (f.edit == "pic_off") off = &f;
        check("inspector: Offset X / Y typed, inches", off && off->cols.size() == 2 && off->unit == 72 && off->value == "-0.083\t0.056",
              off ? off->value : "");
        j.undo();
        j.undo();
        check("undo: fitted as before", d.as<core::Frame>("frame.11")->fit == f0.fit);
    }

    {
        std::cout << "\n-- no break (s075) --\n";
        namespace L = composix::layout;
        //            0123456789012345678901234
        const std::string t = "one two three four five";
        const std::vector<core::ByteSpan> sp{{8, 18}};   // "three four"
        check("keep together: a break inside goes back to the span's word start",
              core::keep_together(t, sp, 0, 14) == 8);
        check("keep together: a break outside stays", core::keep_together(t, sp, 0, 8) == 8 && core::keep_together(t, sp, 0, 19) == 19);
        check("keep together: a span starting the line breaks as usual", core::keep_together(t, sp, 8, 6) == 6);
        check("keep together: mid-word span start -> back to that word", core::keep_together("ab 12km x", {{5, 7}}, 0, 6) == 3);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A character style with No break, over a stretch of the opening paragraph.
        core::Doc::Id cs;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character) { cs = id; break; }
        check("sample: a character style", !cs.empty());
        check("no break: one step on the style", core::doc::edit_styles(j, d, {cs}, "No break", [](core::Style& st) { st.no_break = true; }));
        // The full column (the lighthouse's wrap off), so 30 bytes surely fit one line.
        core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap", [](core::Frame& f) { f.wrap = core::Wrap::None; });
        const auto& para = d.as<core::Story>("story.1")->paras.at(1);
        // Some 30 bytes from the 3rd word on, ending at a word's end.
        int a = 0, words = 0;
        while (a < static_cast<int>(para.text.size()) && words < 3) if (para.text[static_cast<std::size_t>(a++)] == ' ') ++words;
        int b = std::min(static_cast<int>(para.text.size()), a + 30);
        while (b < static_cast<int>(para.text.size()) && para.text[static_cast<std::size_t>(b)] != ' ') ++b;
        core::doc::set_char_style(j, d, "story.1", {1, a}, {1, b}, cs);
        const auto spans = core::no_break_spans(d, d.as<core::Story>("story.1")->paras.at(1));
        check("no break: the run is a span", !spans.empty() && spans[0].a <= a && spans[0].b >= b);
        bool never = !spans.empty(), whole = false;
        for (const auto& fr : L::flow_story(d, "story.1").frames)
            for (const auto& l : fr.lines) {
                if (l.para != 1) continue;
                for (const auto& s : spans) {
                    if (l.end > s.a && l.end < s.b && l.start < s.a) never = false;
                    whole = whole || (l.start <= s.a && l.end >= s.b);
                }
            }
        check("no break: no line ends inside it", never);
        check("no break: it sits whole on one line (it's narrower than the column)", whole);
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find(cs)->fields) row = row || (f.edit == "nobreak" && f.value == "On");
        check("inspector: a character style's No break", row);
        std::string err;
        core::Doc e;
        check("json: no break kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>(cs)->no_break, err);
    }

    {
        std::cout << "\n-- units (s073) --\n";
        using U = core::Units;
        check("units: points per unit", core::unit_pt(U::Inches) == 72 && core::unit_pt(U::Points) == 1 &&
                                        core::unit_pt(U::Picas) == 12 && std::abs(core::unit_pt(U::Millimetres) * 25.4 - 72) < 1e-9);
        check("units: lengths as text", core::length_text(54, U::Inches) == "0.75 in" && core::length_text(54, U::Points) == "54 pt" &&
                                        core::length_text(54, U::Picas) == "4p6" && core::length_text(72, U::Picas) == "6p" &&
                                        core::length_text(72, U::Millimetres) == "25.4 mm",
              core::length_text(54, U::Picas) + " / " + core::length_text(72, U::Millimetres));
        // Every unit reads back: what's shown, typed with that unit, is the same length.
        bool round = true;
        for (U u : {U::Inches, U::Millimetres, U::Picas, U::Points})
            for (double v : {0.0, 9.0, 54.0, 431.25, 648.0}) {
                const auto back = core::parse_length(core::length_in(v, u), core::unit_pt(u));
                round = round && back && std::abs(*back - v) < 0.01 * core::unit_pt(u) + 1e-9;
            }
        check("units: what's shown types back to the same length", round);
        check("units: size text", core::size_text(432, 648, 0, U::Millimetres) == "152.4 × 228.6 mm" &&
                                  core::size_text(432, 648, 0, U::Picas) == "36p × 54p");
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("units: one step; Millimetres", core::doc::set_units(j, d, U::Millimetres) && d.settings().units == U::Millimetres &&
                                              !core::doc::set_units(j, d, U::Millimetres));
        {   // s154: X / Y from the page's own corner here (Page origin)
            core::Doc::Loading load(d);
            auto st = d.settings();
            st.ruler_origin = core::RulerOrigin::Page;
            d.put("doc", st);
        }
        const auto items = core::frame_items(d, {"frame.1"});
        const core::Field* b = nullptr;
        for (const auto& f : items.at(0).fields) if (f.edit == "bounds") b = &f;
        check("inspector: Bounds in mm, a bare number in mm", b && b->label == "Bounds (mm)" && std::abs(b->unit - 72.0 / 25.4) < 1e-9 &&
                                                              b->value.rfind("19.05\t", 0) == 0, b ? b->value : "");
        const auto o = core::project(d, "");
        bool trim = false, units = false;
        for (const auto& f : o.find("doc")->fields) units = units || (f.edit == "dunits" && f.value == "Millimetres" && f.choices.size() == 4);
        for (const auto& f : o.find(d.ids_of<core::Master>().front())->fields)   // s148: the size is the master's
            trim = trim || (f.edit == "msize" && f.label == "Size (mm)" && f.value == "152.4\t228.6");
        check("inspector: a master's Size in mm, Units offered", trim && units);
        std::string err;
        core::Doc e;
        check("json: units kept", core::from_json(e, core::to_json(d), &err) && e.settings().units == U::Millimetres, err);
        j.undo();
        check("units: undo -- inches again", d.settings().units == U::Inches);
    }

    {
        std::cout << "\n-- override all (s072) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Two pages with master items showing (the sample's folios: one each).
        std::vector<core::Doc::Id> pages;
        for (const auto& p : d.pages())
            if (!core::doc::overridable_items(d, p).empty() && pages.size() < 2) pages.push_back(p);
        check("sample: two pages with master items", pages.size() == 2);
        if (pages.size() == 2) {
            const auto open0 = core::doc::overridable_items(d, pages[0]);
            const auto open1 = core::doc::overridable_items(d, pages[1]);
            const auto steps = j.size();
            const auto made = D::override_all(j, d, pages);
            check("override all: both pages, ONE step, a copy each", j.size() == steps + 1 && made.size() == open0.size() + open1.size());
            bool linked = made.size() == open0.size() + open1.size();
            for (std::size_t k = 0; linked && k < open0.size(); ++k)
                linked = d.as<core::Frame>(made[k])->overrides == open0[k] && d.as<core::Frame>(made[k])->owner == pages[0];
            check("override all: each copy on its page, overriding its item", linked);
            check("override all: none left to override", core::doc::overridable_items(d, pages[0]).empty() &&
                                                         core::doc::overridable_items(d, pages[1]).empty() && D::override_all(j, d, pages).empty());
            const auto o = core::project(d, "");
            bool row = false, back = false;
            for (const auto& f : o.find(pages[0])->fields) {
                row = row || (f.label == "Master items" && f.value.find("overridden") != std::string::npos);
                if (f.edit == "pover") for (const auto& c : f.choices) back = back || c.id == "none";
            }
            check("inspector: the page counts them, offers Remove overrides", row && back);
            check("remove all: one step, all back", D::remove_all_overrides(j, d, pages) &&
                                                    core::doc::overridable_items(d, pages[0]) == open0 && core::doc::overridable_items(d, pages[1]) == open1);
            j.undo();
            check("undo: overridden again", core::doc::overridable_items(d, pages[0]).empty());
            j.undo();
            check("undo: as it was", core::doc::overridable_items(d, pages[0]) == open0 && core::doc::overridable_items(d, pages[1]) == open1);
        }
    }

    {
        std::cout << "\n-- typed tints (s071) --\n";
        check("percent: 40, 40%, 40 %, 12.5%", core::parse_percent("40") == 40.0 && core::parse_percent("40%") == 40.0 &&
                                                core::parse_percent(" 40 % ") == 40.0 && core::parse_percent("12.5%") == 12.5);
        check("percent: not a percentage", !core::parse_percent("") && !core::parse_percent("%") && !core::parse_percent("4o") &&
                                           !core::parse_percent("-5") && !core::parse_percent("1.2.3") && !core::parse_percent("12pt"));
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        core::doc::edit_frames(j, d, {"frame.1"}, "Fill", [&](core::Frame& f) { f.fill = d.ids_of<core::Swatch>().front(); });
        const auto items = core::frame_items(d, {"frame.1"});
        const auto* ft = field(items.at(0).fields, "fill_tint");
        const auto* op = field(items.at(0).fields, "opacity");
        check("inspector: Fill tint typed, a percentage, presets kept", ft && ft->percent && ft->unit == 0 && ft->lo == 1 && ft->hi == 100 &&
                                                                      !ft->choices.empty() && core::parse_percent(ft->value));
        check("inspector: Opacity typed down to 0", op && op->percent && op->lo == 0);
        std::vector<const core::Item*> one{&items.at(0)};
        const auto sh = core::shared_fields(one);
        check("inspector: percent survives the shared view", field(sh, "fill_tint") && field(sh, "fill_tint")->percent);
        const auto o = core::project(d, "");
        bool style_tint = false;
        for (const auto& it : o.items())
            for (const auto& f : it.fields) style_tint = style_tint || (f.edit == "color_tint" && f.percent);
        check("inspector: a style's tint typed too", style_tint);
    }

    {
        std::cout << "\n-- arrange (s070) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto owner = d.as<core::Frame>("frame.1")->owner;
        const auto order = [&] { return d.frames_on(owner); };
        const auto at = [&](const std::string& id) {
            const auto o = order();
            return static_cast<int>(std::find(o.begin(), o.end(), id) - o.begin());
        };
        const auto n = static_cast<int>(order().size());
        check("sample: page 1 has several frames", n >= 3);
        check("to front: one step, on top", D::arrange(j, d, {"frame.1"}, D::Arrange::Front) && j.size() == 1 && at("frame.1") == n - 1);
        check("to front again: nothing moves", !D::arrange(j, d, {"frame.1"}, D::Arrange::Front));
        check("backward: one down", D::arrange(j, d, {"frame.1"}, D::Arrange::Backward) && at("frame.1") == n - 2);
        check("to back: at the bottom", D::arrange(j, d, {"frame.1"}, D::Arrange::Back) && at("frame.1") == 0);
        check("forward: one up", D::arrange(j, d, {"frame.1"}, D::Arrange::Forward) && at("frame.1") == 1);
        // Two picked keep their order among themselves.
        const auto o0 = order();
        const auto a = o0[0], b = o0[1];
        check("two to front: both on top, in order", D::arrange(j, d, {b, a}, D::Arrange::Front) && at(a) == n - 2 && at(b) == n - 1);
        bool z = true;
        const auto o1 = order();
        for (std::size_t k = 0; k < o1.size(); ++k) z = z && d.as<core::Frame>(o1[k])->z == static_cast<double>(k);
        check("z renumbered 0..n-1", z);
        // Stacking drives "text beneath" (s069): the lighthouse sent back stops pushing.
        D::set_wrap_beneath(j, d, true);
        D::arrange(j, d, {"frame.11"}, D::Arrange::Front);
        const bool pushes = !core::wrap_rects(d, "frame.1").empty();
        D::arrange(j, d, {"frame.11"}, D::Arrange::Back);
        check("beneath: in front pushes, at the back doesn't", pushes && core::wrap_rects(d, "frame.1").empty());
        const auto steps = j.size();
        j.undo();
        check("undo: the lighthouse back in front", at("frame.11") == n - 1 && j.size() == steps);
        bool keys = true;
        for (const char* act : {"win.bring-front", "win.bring-forward", "win.send-backward", "win.send-back"}) {
            bool found = false;
            for (const auto& r : core::shortcut_registry()) found = found || (r.action == act && !r.accels.empty());
            keys = keys && found;
        }
        check("arrange: four keys in the registry", keys);
    }

    {
        std::cout << "\n-- wrap options (s069) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        // The sample: the lighthouse (frame.11) sits above the opening text (frame.1) and wraps it.
        const auto& pic = *d.as<core::Frame>("frame.11");
        const auto& txt = *d.as<core::Frame>("frame.1");
        check("sample: the picture is above the text", pic.owner == txt.owner && pic.z > txt.z);
        check("wrap: it pushes the text", core::wrap_rects(d, "frame.1").size() == 1);
        // Ignore wrap: the text frame alone, one step; the lines run straight through.
        check("ignore wrap: one step", D::edit_frames(j, d, {"frame.1"}, "Ignore text wrap",
                                                      [](core::Frame& f) { f.ignore_wrap = true; }) && j.size() == 1);
        check("ignore wrap: nothing pushes it", core::wrap_rects(d, "frame.1").empty());
        // Ignoring wrap sets the text exactly as if the picture had no wrap.
        const auto lines_of = [&] {
            for (const auto& fr : L::flow_story(d, "story.1").frames) if (fr.frame == "frame.1") return fr.lines;
            return std::vector<L::Line>{};
        };
        const auto ignored = lines_of();
        D::edit_frames(j, d, {"frame.11"}, "Text wrap", [](core::Frame& f) { f.wrap = core::Wrap::None; });
        const auto none = lines_of();
        j.undo();
        bool same = !ignored.empty() && ignored.size() == none.size();
        for (std::size_t i = 0; same && i < none.size(); ++i)
            same = std::abs(ignored[i].x - none[i].x) < 1e-6 && std::abs(ignored[i].y - none[i].y) < 1e-6 &&
                   ignored[i].start == none[i].start && ignored[i].end == none[i].end;
        check("ignore wrap: the text sets as with no wrap at all", same);
        const auto items = core::frame_items(d, {"frame.1"});
        const auto* iw = field(items.at(0).fields, "ignore_wrap");
        check("inspector: a text frame's Others' wrap", iw && iw->value == "Ignored" && iw->choices.size() == 2);
        j.undo();
        check("ignore wrap: undo -- pushed again", core::wrap_rects(d, "frame.1").size() == 1);
        // Wrap only text beneath: the picture is above -> still pushes; put it below -> it doesn't.
        check("beneath: one step", D::set_wrap_beneath(j, d, true) && d.settings().wrap_beneath && !D::set_wrap_beneath(j, d, true));
        check("beneath: a frame above still pushes", core::wrap_rects(d, "frame.1").size() == 1);
        D::edit_frames(j, d, {"frame.11"}, "Arrange", [&](core::Frame& f) { f.z = txt.z - 1; });
        check("beneath: a frame below doesn't", core::wrap_rects(d, "frame.1").empty());
        D::set_wrap_beneath(j, d, false);
        check("all text: below pushes too", core::wrap_rects(d, "frame.1").size() == 1);
        D::set_wrap_beneath(j, d, true);
        // A master's wrapping item sits beneath every page frame.
        const auto chain = core::master_chain(d, txt.owner);
        check("beneath: the page has a master", !chain.empty());
        if (!chain.empty()) {
            const auto added = D::add_frame(j, d, chain.back(), core::FrameKind::Shape, {txt.x + 20, txt.y + 40, 60, 60});
            const bool made = !added.empty() && D::edit_frames(j, d, {added}, "Text wrap", [](core::Frame& f) {
                f.wrap = core::Wrap::Around;
                f.z = 1e6;   // top of its master -- still under the page
            });
            check("beneath: a master's wrap stays under the page's text", made && core::wrap_rects(d, "frame.1").empty());
            D::set_wrap_beneath(j, d, false);
            check("all text: the master's wrap pushes", !core::wrap_rects(d, "frame.1").empty());
        }
        const auto o = core::project(d, "");
        const auto* dw = field(o.find("doc")->fields, "dwrap");
        check("inspector: Document settings' Text wrap", dw && dw->value == "All text");
        D::set_wrap_beneath(j, d, true);
        D::edit_frames(j, d, {"frame.1"}, "Ignore text wrap", [](core::Frame& f) { f.ignore_wrap = true; });
        std::string err;
        core::Doc e;
        check("json: both kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                     e.settings().wrap_beneath && e.as<core::Frame>("frame.1")->ignore_wrap, err);
    }

    {
        std::cout << "\n-- up a level (s068) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto o = core::project(d, "");
        const auto first = [&](core::Segment s) { const auto in = o.in(s); return in.empty() ? std::string{} : in.front()->id; };
        using S = core::Segment;
        check("up: a page -> Pages & Spreads", core::parent_of(o, {d.pages().front()}) == S::Pages);
        check("up: a master -> Masters", core::parent_of(o, {first(S::Masters)}) == S::Masters);
        check("up: a story -> Stories", core::parent_of(o, {first(S::Stories)}) == S::Stories);
        check("up: a swatch -> Colors", core::parent_of(o, {first(S::Colors)}) == S::Colors);
        check("up: the first known item decides", core::parent_of(o, {"nothing.1", first(S::Masters)}) == S::Masters);
        check("up: nothing known -> no level", !core::parent_of(o, {}) && !core::parent_of(o, {"nothing.1"}));
        bool reg = false;
        for (const auto& r : core::shortcut_registry()) reg = reg || (r.action == "win.go-up" && !r.accels.empty());
        check("up: Alt+Up in the shortcut registry", reg);
        bool back = false, fwd = false;
        for (const auto& r : core::shortcut_registry()) {
            for (const auto& a : r.accels) {
                back = back || (r.action == "win.go-up" && a == "<Alt>Left");
                fwd = fwd || (r.action == "win.go-forward" && a == "<Alt>Right");
            }
        }
        check("back / forward: Alt+Left and Alt+Right in the registry", back && fwd);
    }

    {
        std::cout << "\n-- per-side insets, wrap offsets, typed column widths (s067) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto lines_of = [&](const std::string& story, const std::string& frame) {
            for (const auto& f : L::flow_story(d, story).frames) if (f.frame == frame) return f.lines;
            return std::vector<L::Line>{};
        };
        const auto in0 = d.as<core::Frame>("frame.1")->inset;
        const auto before = lines_of("story.1", "frame.1");
        // One side: the left inset alone.
        check("inset: the left side alone, one step", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 3, 30) && j.size() == 1);
        const auto& f1 = *d.as<core::Frame>("frame.1");
        check("inset: only the left changed", f1.inset.left == 30 && f1.inset.top == in0.top && f1.inset.right == in0.right &&
                                                  f1.inset.bottom == in0.bottom);
        const auto left = lines_of("story.1", "frame.1");
        bool inside = !left.empty();
        for (const auto& l : left) inside = inside && l.x >= 30 - 0.01;
        check("inset: every line right of the left inset", inside);
        check("inset: the first line's top unchanged", !left.empty() && !before.empty() && std::abs(left[0].y - before[0].y) < 0.01);
        // The top alone moves the first line down by the difference.
        D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 0, in0.top + 24);
        const auto top = lines_of("story.1", "frame.1");
        check("inset: the top alone moves the first line down", !top.empty() && std::abs(top[0].y - (before[0].y + 24)) < 0.01);
        check("inset: all four (same all round)", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, -1, 5) &&
                                                     d.as<core::Frame>("frame.1")->inset == core::Sides{5, 5, 5, 5});
        check("inset: held to 500 pt", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 2, 900) &&
                                          d.as<core::Frame>("frame.1")->inset.bottom == 500);
        check("inset: not on a picture", !D::set_side(j, d, {"frame.11"}, D::SideOf::Inset, 0, 12));
        check("inset: a side out of range refused", !D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 4, 12));
        const auto steps = j.size();
        j.undo();
        j.undo();
        j.undo();
        j.undo();
        check("inset: undo, side by side, back to the start", j.size() == steps && d.as<core::Frame>("frame.1")->inset == in0);
        // Wrap: the right side alone widens the band on the right only.
        const auto w0 = core::wrap_rects(d, "frame.1");
        check("wrap: the right side alone", D::set_side(j, d, {"frame.11"}, D::SideOf::Wrap, 1, 30));
        const auto w1 = core::wrap_rects(d, "frame.1");
        check("wrap: its band reaches further right, the left edge stays",
              w0.size() == 1 && w1.size() == 1 && std::abs(w1[0].x1 - (w0[0].x1 + 21)) < 1e-6 && std::abs(w1[0].x0 - w0[0].x0) < 1e-6);
        // Columns: one typed, the others share the rest in their proportions.
        const auto& src = *d.as<core::Frame>("frame.10");
        const double room = core::column_width(src, 0) + core::column_width(src, 1);
        check("column width: one step", D::set_column_width(j, d, {"frame.10"}, 0, 150));
        const auto& s1 = *d.as<core::Frame>("frame.10");
        check("column width: the first takes 150 pt, the second the rest",
              std::abs(core::column_width(s1, 0) - 150) < 0.02 && std::abs(core::column_width(s1, 0) + core::column_width(s1, 1) - room) < 0.02,
              std::to_string(core::column_width(s1, 0)) + " / " + std::to_string(core::column_width(s1, 1)));
        D::set_column_width(j, d, {"frame.10"}, 1, 1e6);
        check("column width: too wide leaves the other at least 1 pt", core::column_width(*d.as<core::Frame>("frame.10"), 0) >= 1 - 1e-9);
        D::edit_frames(j, d, {"frame.10"}, "Columns", [](core::Frame& f) { f.columns = 3; f.widths = {2, 1, 1}; });
        const auto& s3 = *d.as<core::Frame>("frame.10");
        const double room3 = core::column_width(s3, 0) + core::column_width(s3, 1) + core::column_width(s3, 2);
        D::set_column_width(j, d, {"frame.10"}, 0, room3 / 4);
        const auto& s4 = *d.as<core::Frame>("frame.10");
        check("column width: the others keep their proportions",
              std::abs(core::column_width(s4, 1) - core::column_width(s4, 2)) < 0.02 && std::abs(core::column_width(s4, 0) - room3 / 4) < 0.02);
        check("column width: not on a one-column frame", !D::set_column_width(j, d, {"frame.1"}, 0, 100));
        // The inspector: four typed sides with the link; widths typed, one per column.
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 3, 12);
        const auto items = core::frame_items(d, {"frame.1", "frame.10", "frame.11"});
        const auto* ins = field(items.at(0).fields, "inset");
        check("inspector: Inset is four typed sides with a link",
              ins && ins->link && ins->cols.size() == 4 && ins->unit == 1 && ins->value.find('\t') != std::string::npos &&
              ins->value.substr(ins->value.rfind('\t') + 1) == "12");
        const auto* cw = field(items.at(1).fields, "colw");
        check("inspector: Widths typed, one per column, presets beside",
              cw && cw->cols.size() == 3 && cw->unit == 1 && !cw->choices.empty() && cw->choices[0].id == "=");
        const auto* wo = field(items.at(2).fields, "wrap_off");
        check("inspector: Wrap offset is four typed sides", wo && wo->link && wo->cols.size() == 4 && wo->value == "9\t30\t9\t9");
        {   // what the inspector actually shows goes through shared_fields
            std::vector<const core::Item*> one{&items.at(0)};
            const auto sh = core::shared_fields(one);
            check("inspector: the link survives the shared view", field(sh, "inset") && field(sh, "inset")->link);
        }
        std::string err;
        core::Doc e;
        check("json: per-side insets and typed widths kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- page numbers --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const std::string t = std::string("p") + core::kMarkPage + "s" + core::kMarkSection;
        check("markers: found where they are", core::marker_at(t, 1) == 0 && core::marker_at(t, 5) == 1 &&
                                               core::marker_at(t, 0) == -1 && core::marker_at(t, 2) == -1);
        check("markers: a story with one", core::has_markers(*d.as<core::Story>("story.6")) &&
                                           !core::has_markers(*d.as<core::Story>("story.1")));
        // A master's folio, filled in for the page it's drawn on.
        const auto body2 = L::resolve_story(d, "story.6", "page.6");
        const auto front1 = L::resolve_story(d, "story.5", "page.1");
        check("folio: page 2 of the body reads 2", body2.any && body2.text.paras[0].text == "2");
        check("folio: the front matter's first page reads i", front1.text.paras[0].text == "i");
        check("folio: set on the page's own lines", !body2.flow.frames.empty());
        // On the master itself: the sign, the same number of bytes.
        const auto onm = L::resolve_story(d, "story.6", "");
        check("folio: on its master, the marker stays (a sign on screen)", onm.text.paras[0].text == core::kMarkPage);
        // Page frames: each marker takes the page its line is on; next /
        // previous are the thread's neighbours.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0},
                             std::string("[") + core::kMarkSection + " " + core::kMarkPrev + "/" + core::kMarkNext + "] ");
        const auto r1 = L::resolve_story(d, "story.1", "");
        check("page frame: section name, no previous, the next frame's page",
              r1.text.paras[1].text.rfind("[Body /2] ", 0) == 0, r1.text.paras[1].text.substr(0, 16));
        // A character style keeps covering the same words after the swap.
        core::doc::set_char_style(j, d, "story.1", {1, 18}, {1, 24}, "style.5");   // "harbor" (after the 14-byte prefix + "The ")
        const auto r2 = L::resolve_story(d, "story.1", "");
        const auto& p2 = r2.text.paras[1];
        bool styled = false;
        for (const auto& run : p2.runs) styled = styled || (p2.text.substr(run.start, run.end - run.start) == "harbor");
        check("page frame: character styles follow the words", styled);
        // The thumbnail key follows the folio: move page 2 of the body first.
        L::FlowCache flows;
        const auto k6 = L::page_key(d, flows, "page.6");
        bool two = false;
        for (const auto& l : k6.lines) two = two || (l.size() >= 2 && l.substr(l.size() - 2) == "|2");
        check("key: the folio as drawn", two);
        core::doc::move_pages(j, d, {"page.6"}, "page.4");
        check("key: a page that moves gets a new folio, a new key", !(L::page_key(d, flows, "page.6") == k6));
        // While editing (and in every flow), a marker is three bytes wherever it shows.
        const auto plain = L::flow_story(d, "story.6");
        check("display: the marker's three bytes on one line", !plain.frames.empty() && !plain.frames[0].lines.empty() &&
                                                               plain.frames[0].lines[0].end == 3);
    }

    {
        std::cout << "\n-- preflight --\n";
        namespace L = composix::layout;
        using Lv = L::Issue::Level;
        core::Doc d;
        load_plain(d);
        L::FlowCache flows;
        const auto has = [](const std::vector<L::Issue>& v, const std::string& kind) {
            return std::any_of(v.begin(), v.end(), [&](const L::Issue& i) { return i.kind == kind; });
        };
        auto v = L::preflight_issues(d, flows, {});
        // s089: the sample's contents is out of date on purpose (its Sources line names no heading).
        check("sample: the contents out of date, one warning", L::problem_count(v) == 1 && has(v, "stale_list"));
        {
            core::DocJournal pj{d};
            d.set_door([&pj] { return pj.open(); });
            L::build_list(pj, d, "list.1");
            d.set_door({});
        }
        v = L::preflight_issues(d, flows, {});
        check("clean sample: no problems, the spot plate noted", L::problem_count(v) == 0 && has(v, "spot"));
        {   // overset, at its last frame; a low-res picture; links as told
            core::Doc::Loading load(d);
            auto f1 = *d.as<core::Frame>("frame.1");
            f1.h = 40;
            f1.next.clear();
            d.put("frame.1", f1);
            auto a = *d.as<core::Asset>("asset.4");
            a.px_w = 200; a.px_h = 150;
            d.put("asset.4", a);
            auto f = *d.as<core::Frame>("frame.6");
            f.opacity = 0.5;
            d.put("frame.6", f);
        }
        L::PreflightOpts o;
        o.link = [](const core::Asset& a) { return a.name == "harbor.psd" ? core::LinkStatus::Missing : core::LinkStatus::OK; };
        v = L::preflight_issues(d, flows, o);
        const auto over = std::find_if(v.begin(), v.end(), [](const L::Issue& i) { return i.kind == "overset"; });
        check("overset: an error at its frame and page", over != v.end() && over->level == Lv::Error &&
                                                         over->frame == "frame.1" && over->page == "page.5");
        check("missing picture: an error, named", has(v, "missing"));
        check("low resolution: a warning", has(v, "lowres"));
        check("no standard: opacity is fine", !has(v, "transparency"));
        check("errors first", v.front().level == Lv::Error && v.back().level == Lv::Info);
        o.standard = 1;
        v = L::preflight_issues(d, flows, o);
        check("X-1a, no profile: the profile, RGB, transparency", has(v, "profile") && has(v, "rgb") && has(v, "transparency"));
        o.profile = true;
        v = L::preflight_issues(d, flows, o);
        check("X-1a with a profile: RGB converts (no error), transparency still", !has(v, "rgb") && has(v, "transparency"));
        check("X-1a: transparency is a warning now (flattened on export, s193)", std::any_of(v.begin(), v.end(), [](const L::Issue& i) {
                  return i.kind == "transparency" && i.level == Lv::Warning; }));
        L::PdfReport rep;
        L::preflight(d, {}, 16, &rep);
        check("export report: the same checks", std::any_of(rep.warnings.begin(), rep.warnings.end(), [](const std::string& w) {
                  return w.rfind("Overset text: Chapters", 0) == 0; }));
    }

    {
        std::cout << "\n-- master overrides --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("sides: the right folio on right pages only", !core::master_item_on(d, "frame.13", "page.6") &&
                                                            core::master_item_on(d, "frame.13", "page.7") &&
                                                            core::master_item_on(d, "frame.14", "page.6") &&
                                                            !core::master_item_on(d, "frame.14", "page.7"));
        check("override: the chapter opener's own folio hides the master's", !core::master_item_on(d, "frame.13", "page.5"));
        L::FlowCache flows;
        const auto k5 = L::page_key(d, flows, "page.5");
        const auto has_rec = [](const L::PageKey& k, const std::string& id) {
            return std::any_of(k.recs.begin(), k.recs.end(), [&](const auto& r) { return r.first == id; });
        };
        check("drawn: the page's copy, not the master's", has_rec(k5, "frame.15") && !has_rec(k5, "frame.13") && !has_rec(k5, "frame.14"));
        const auto steps = j.size();
        const auto made = core::doc::override_master_item(j, d, "page.7", "frame.13");
        const auto* mf = d.as<core::Frame>(made);
        check("override: a page copy, one step", mf && mf->owner == "page.7" && mf->overrides == "frame.13" && j.size() == steps + 1);
        check("override: its own story, the same words", mf && mf->story != "story.6" &&
                                                         d.as<core::Story>(mf->story)->paras[0].text == core::kMarkPage);
        check("override: the master's hidden there", !core::master_item_on(d, "frame.13", "page.7"));
        check("override: not twice, not where it doesn't show", core::doc::override_master_item(j, d, "page.7", "frame.13").empty() &&
                                                                core::doc::override_master_item(j, d, "page.6", "frame.13").empty());
        const std::string story = mf ? mf->story : "";
        check("remove override: back to the master's, its story gone", core::doc::remove_overrides(j, d, {made}) &&
                                                                       !d.as<core::Frame>(made) && !d.as<core::Story>(story) &&
                                                                       core::master_item_on(d, "frame.13", "page.7"));
        j.undo();
        check("remove override: undo, the copy back", d.as<core::Frame>(made) && !core::master_item_on(d, "frame.13", "page.7"));
        // Ctrl+Shift+click finds the master's item under the pointer on a page.
        const auto pl = core::layout_pages(d, {"page.9"});
        const auto* r = d.as<core::Frame>("frame.13");
        core::Doc::Id on;
        check("hit: a master item on a page", core::hit_master_item(d, pl, pl[0].r.x + r->x + 5, pl[0].r.y + r->y + 5, 1, &on) == "frame.13" &&
                                              on == "page.9");
        check("hit: not on the wrong side", core::hit_master_item(d, core::layout_pages(d, {"page.8"}),
                                                                  pl[0].r.x + r->x + 5, pl[0].r.y + r->y + 5, 1, &on).empty());
    }
}
}  // namespace cxtest
