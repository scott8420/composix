// composix_selftest, part 3 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part3() {

    {
        std::cout << "\n-- place several (s063) --\n";
        core::PlaceQueue q;
        check("queue: empty at first", q.empty() && q.size() == 0);
        const auto file = [](const char* n) { core::ImageFile f; f.name = n; f.path = std::string("/x/") + n; return f; };
        q.load({file("a.png"), file("b.png"), file("c.png")});
        check("queue: the first is the one a click puts down", q.size() == 3 && q.current().name == "a.png" && q.next()->name == "b.png");
        q.cycle(+1);
        check("queue: the arrows turn -- next", q.current().name == "b.png" && q.size() == 3);
        q.cycle(-1);
        q.cycle(-1);
        check("queue: and back (the last to the front)", q.current().name == "c.png");
        q.placed();
        check("queue: placed -- it goes, the next comes up", q.size() == 2 && q.current().name == "a.png");
        q.placed();
        q.placed();
        check("queue: the last placed -- nothing loaded", q.empty() && !q.next());
        q.load({file("x.png")});
        q.cycle(+1);
        check("queue: one alone doesn't turn; Esc clears", q.current().name == "x.png" && (q.clear(), q.empty()));
    }

    {
        std::cout << "\n-- start screen (s064) --\n";
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        const auto dir = (fs::temp_directory_path() / "composix_s064" / "Sample Book").string();
        std::error_code ec;
        fs::remove_all(fs::path(dir).parent_path(), ec);
        fs::create_directories(dir, ec);
        std::string err;
        check("fixture: the sample saved", core::save_project(d, dir, &err), err);
        core::Doc kept;
        const auto sum = core::summarize_project(dir, &kept);
        const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        check("summary: title, trim, pages", sum.ok && sum.title == d.settings().title && sum.pages == static_cast<int>(d.pages().size()) &&
                                             sum.trim_w == d.settings().trim_w && sum.trim_h == d.settings().trim_h);
        check("summary: saved just now, the document kept", std::llabs(now - sum.saved) < 120 && kept.pages().size() == d.pages().size());
        check("summary: no document there -- not ok", !core::summarize_project((fs::path(dir).parent_path() / "none").string()).ok);
        fs::remove_all(fs::path(dir).parent_path(), ec);
        const long long t = 1'760'000'000;
        check("ago: minutes, hours, yesterday, days", core::ago_text(t + 30, t) == "just now" && core::ago_text(t + 60, t) == "1 minute ago" &&
                                                     core::ago_text(t + 3 * 3600, t) == "3 hours ago" &&
                                                     core::ago_text(t + 30 * 3600, t) == "yesterday" &&
                                                     core::ago_text(t + 4 * 86400, t) == "4 days ago");
        const auto date = core::ago_text(t + 30 * 86400, t);
        check("ago: a month on -- the date", date.find("2025") != std::string::npos && date.find("  ") == std::string::npos, date);
    }

    {
        std::cout << "\n-- hyphen control (s065) --\n";
        namespace L = composix::layout;
        core::Doc d;
        load_plain(d);
        const std::string text =
            "Considerable international correspondence concerning extraordinary navigational responsibilities "
            "overwhelmed administrative establishments, notwithstanding unquestionably comprehensive "
            "documentation characterizing transcontinental shipping circumstances, unconditionally.";
        const auto setup = [&](int limit, int shortest, bool last, const std::string& body, double width = 110) {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Style>("style.1");
            st.hyphenate = true;
            st.align = core::Align::Justify;
            st.hyph_limit = limit;
            st.hyph_shortest = shortest;
            st.hyph_last_word = last;
            st.drop_lines = 0;
            d.put("style.h", st);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = width; f.h = 2000;
            f.story = "story.y";
            d.put("frame.y", f);
            core::Story sy;
            sy.name = "Hy";
            sy.paras = {{"style.h", body, {}}};
            d.put("story.y", sy);
        };
        const auto lines = [&] {
            const auto fl = L::flow_story(d, "story.y");
            return fl.frames.empty() ? std::vector<L::Line>{} : fl.frames.front().lines;
        };
        const auto max_run = [&] {
            int run = 0, best = 0, total = 0;
            for (const auto& l : lines()) { run = l.hyphen ? run + 1 : 0; best = std::max(best, run); total += l.hyphen; }
            return std::pair{best, total};
        };
        setup(0, 5, true, text);
        const auto [free_run, free_total] = max_run();
        // (How many it hyphenates is the fonts' business -- a note, not a test.)
        std::cout << "  (narrow column: " << free_total << " hyphenated lines, at most " << free_run << " in a row)\n";
        setup(2, 5, true, text);
        check("limit 2: never more than two in a row", max_run().first <= 2);
        setup(1, 5, true, text);
        check("limit 1: never two in a row", max_run().first <= 1);
        setup(0, 30, true, text);
        check("shortest word 30 letters: nothing hyphenates", max_run().second == 0);
        // The last word stays whole.
        setup(0, 5, false, text);
        const auto last_at = static_cast<int>(text.rfind(' ')) + 1;
        bool whole = true;
        for (const auto& l : lines()) whole = whole && !(l.hyphen && l.end > last_at);
        check("last word: never broken (unless asked)", whole);
        // Typed hyphens: the word breaks only there; one at its start -- never.
        const std::string shy = "\u00AD";
        const std::string word = "trans" + shy + "continental" + shy + "ism";
        setup(0, 5, true, "Unquestionably " + word + " " + word + " " + word + " notwithstanding.");
        const auto& tx = d.as<core::Story>("story.y")->paras[0].text;
        bool only_typed = true;
        int at_typed = 0;
        for (const auto& l : lines()) {
            if (!l.hyphen) continue;
            const bool after_shy = l.end >= 2 && static_cast<unsigned char>(tx[static_cast<std::size_t>(l.end - 2)]) == 0xC2;
            const bool in_word = tx.substr(0, static_cast<std::size_t>(l.end)).rfind("trans") != std::string::npos &&
                                 static_cast<int>(tx.substr(0, static_cast<std::size_t>(l.end)).rfind(' ')) <
                                     static_cast<int>(tx.substr(0, static_cast<std::size_t>(l.end)).rfind("trans"));
            if (in_word && !after_shy) only_typed = false;
            at_typed += after_shy;
        }
        // The rule, not the layout: wherever a marked word breaks, it's at a
        // typed hyphen (whether one lands at a line's end is the fonts' doing).
        check("typed hyphens: a marked word breaks only at them", only_typed, std::to_string(at_typed));
        // (a column the words fit across: too narrow and Pango breaks a word anywhere)
        setup(0, 5, true, "Unquestionably " + shy + "transcontinentalism " + shy + "transcontinentalism notwithstanding.", 240);
        const auto& tx2 = d.as<core::Story>("story.y")->paras[0].text;
        std::vector<std::pair<int, int>> spans;   // each marked word, its typed hyphen to its end
        for (std::size_t at = tx2.find(shy); at != std::string::npos; at = tx2.find(shy, at + 1))
            spans.push_back({static_cast<int>(at) + 2, static_cast<int>(tx2.find(' ', at))});
        bool never = spans.size() == 2;
        for (const auto& l : lines())
            for (const auto& [s0, s1] : spans) never = never && !(l.end > s0 && l.end < s1);
        if (!never) for (const auto& l : lines()) std::cout << "    line " << l.start << ".." << l.end << (l.hyphen ? " -" : "") << "\n";
        check("typed hyphen at a word's start: that word never breaks", never);
    }

    {
        std::cout << "\n-- document settings (s066) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto st0 = d.settings();
        check("title: trimmed, one step; empty refused", D::set_title(j, d, "  The Outer Ports ") && d.settings().title == "The Outer Ports" &&
                                                         !D::set_title(j, d, "  "));
        // Trim: pages at the trim follow, a foldout in proportion; one undo.
        std::string plain, fold;
        for (const auto& p : d.pages()) {
            const auto* pg = d.as<core::Page>(p);
            if (pg->folds.empty() && plain.empty()) plain = p;
            if (!pg->folds.empty() && fold.empty()) fold = p;
        }
        const auto fold0 = *d.as<core::Page>(fold);
        const auto steps = j.size();
        const double W = 5.5 * core::kPt, H = 8.5 * core::kPt;
        check("trim: one step", D::set_trim(j, d, W, H) && j.size() == steps + 1);
        check("trim: a page takes it", std::abs(d.as<core::Page>(plain)->w - W) < 1e-6 && std::abs(d.as<core::Page>(plain)->h - H) < 1e-6);
        const auto* f1 = d.as<core::Page>(fold);
        check("trim: a foldout keeps its panels, in proportion",
              std::abs(f1->w - fold0.w * W / st0.trim_w) < 1e-6 && f1->folds.size() == fold0.folds.size() &&
              std::abs(f1->folds[0] - fold0.folds[0] * W / st0.trim_w) < 1e-6 && std::abs(f1->h - H) < 1e-6);
        bool masters = true;
        for (const auto& m : d.ids_of<core::Master>()) masters = masters && std::abs(d.as<core::Master>(m)->h - H) < 1e-6;
        check("trim: the masters too", masters);
        check("trim: too small refused", !D::set_trim(j, d, 10, 10));
        j.undo();
        check("trim: undo, every page back", d.as<core::Page>(fold)->w == fold0.w && d.settings().trim_w == st0.trim_w);
        check("bleed and facing", D::set_bleed(j, d, 18) && d.settings().bleed == 18 && !D::set_bleed(j, d, -1) &&
                                  D::set_facing(j, d, !st0.facing) && d.settings().facing == !st0.facing);
        const auto o = core::project(d, "");
        const auto* it = o.find("doc");
        check("inspector: the document's settings in Document", it && it->segment == core::Segment::Document && !it->fields.empty() &&
                                                               it->fields[0].text && it->fields[0].value == "The Outer Ports");
    }

    {
        std::cout << "\n-- break marks (s098) --\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_break(j, d, "story.1", {2, 0}, core::ParaStart::NextPage);
        L::FlowCache flows;
        const auto& flow = flows.get(d, "story.1");
        const L::HiddenMark* brk = nullptr;
        std::vector<L::HiddenMark> all;
        for (const auto& fid : d.ids_of<core::Frame>())
            if (d.as<core::Frame>(fid)->story == "story.1")
                for (const auto& m : L::hidden_marks(d, "story.1", flow, fid)) all.push_back(m);
        for (const auto& m : all) if (m.kind == L::HiddenMark::Kind::Break) brk = &m;
        const auto* f1 = d.as<core::Frame>("frame.1");
        check("mark: the page break, its rule to the column's edge", brk && brk->brk == 3 && brk->edge > brk->x0 &&
                                                                     std::abs(brk->edge - core::column_width(*f1, 0) - core::column_x(*f1, 0)) < 0.01);
        check("names", std::string(L::break_name(1)) == "Column break" && std::string(L::break_name(3)) == "Page break" &&
                       std::string(L::break_name(4)) == "Right-page break");
    }

    {
        std::cout << "\n-- unused asset files (s097) --\n";
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto dir = (fs::temp_directory_path() / "composix_selftest_s097").string();
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string err;
        core::save_project(d, dir, &err);
        const auto put = [&](const std::string& name, std::size_t n) {
            std::ofstream o(fs::path(dir) / "assets" / name, std::ios::binary);
            o << std::string(n, 'x');
        };
        put("guild-map.tif", 10);      // the sample's embedded files
        put("fleuron.svg", 10);
        put("old-scan.png", 2000);     // nothing uses these
        put("draft.jpg", 3000);
        put("undone.png", 100);        // an asset only undo could bring back
        fs::create_directories(fs::path(dir) / "assets" / "sub", ec);   // a folder: left alone
        const auto held = std::vector<std::string>{"assets/undone.png"};
        const auto u = core::unused_asset_files(d, dir, held);
        check("unused: what nothing uses, sorted, with sizes", u.size() == 2 && fs::path(u[0].path).filename() == "draft.jpg" &&
                                                              u[0].bytes == 3000 && fs::path(u[1].path).filename() == "old-scan.png");
        check("unused: without the undo history, the undone one too", core::unused_asset_files(d, dir).size() == 3);
        check("unused: an unsaved document -- none", core::unused_asset_files(d, "").empty());
        // The journal: what undo could bring back.
        core::doc::embed_assets(j, d, {{"asset.2", "assets/harbor-copy.psd"}});
        j.undo();
        std::vector<std::string> paths;
        j.each_value([&](const core::Record& r) { if (const auto* a = std::get_if<core::Asset>(&r)) paths.push_back(a->path); });
        check("journal: the undone embed's path is still held", std::find(paths.begin(), paths.end(), "assets/harbor-copy.psd") != paths.end());
        check("bytes: in words", core::bytes_text(512) == "512 bytes" && core::bytes_text(12 * 1024) == "12 KB" &&
                                 core::bytes_text(2516582) == "2.4 MB");
        fs::remove_all(dir, ec);
    }

    {
        std::cout << "\n-- small fixes (s096) --\n";
        namespace fs = std::filesystem;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::set_units(j, d, core::Units::Millimetres);
        const auto dir = (fs::temp_directory_path() / "composix_selftest_s096").string();
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string err;
        const bool saved = core::save_project(d, dir, &err);
        const auto sum = core::summarize_project(dir);
        check("start screen: a card knows its document's units", saved && sum.ok && sum.units == core::Units::Millimetres &&
                                                                core::size_text(sum.trim_w, sum.trim_h, 0, sum.units).find("mm") != std::string::npos, err);
        fs::remove_all(dir, ec);
        // A level's picked entry style, not yet written: stale.
        L::build_list(j, d, "list.1");
        const auto why = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().why;
        };
        check("stale: built -- current", why() == "current");
        core::doc::set_list_entry_style(j, d, "list.1", 1, "style.3");
        check("stale: a picked entry style not yet written", why() == "Entries not in Caption");
        L::build_list(j, d, "list.1");
        check("stale: rebuilt in it -- current", why() == "current");
    }

    {
        std::cout << "\n-- find across paragraphs (s095) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // three short paragraphs to search
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras = {{"style.1", "alpha end"}, {"style.1", "Begin beta end"}, {"style.4", "begin gamma"}};
            d.put("story.3", st);
        }
        const auto find = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        auto h = find("end^pbegin");
        check("text ^p: over each break, case folded", h.size() == 2 && h[0].a == core::TextPos{0, 6} && h[0].b == core::TextPos{1, 5} &&
                                                       h[1].a == core::TextPos{1, 11} && h[1].b == core::TextPos{2, 5});
        check("wildcards ^p: three letters, the break, a b (either case)", find("^$^$^$^pB", D::FindOpts::Mode::Wildcards).size() == 2);
        h = find("(\\w+)\\n(\\w+)", D::FindOpts::Mode::Regex);
        check("regex \\n: across, groups as written", h.size() == 2 && h[0].groups.size() == 3 && h[0].groups[1] == "end" &&
                                                     h[0].groups[2] == "Begin");
        {
            D::FindOpts f;
            f.what = "end^pbegin";
            f.story = "story.3";
            f.pstyle = "style.4";
            check("a paragraph style: the one the hit starts in", D::find_all(d, f).empty());
        }
        // Change across: join the paragraphs, or break one.
        D::ChangeOpts c;
        c.with = "end / begin";
        const auto n = D::change_hits(j, d, find("end^pbegin"), c, "Change All");
        const auto& ps = d.as<core::Story>("story.3")->paras;
        check("change: the breaks taken out, one step", n == 2 && ps.size() == 1 && ps[0].text == "alpha end / begin beta end / begin gamma");
        c.with = "^p";
        D::change_hits(j, d, find(" / "), c, "Change All");
        check("change to ^p: broken back into paragraphs", d.as<core::Story>("story.3")->paras.size() == 3);
        check("plain text without ^p: within a paragraph, as before", find("beta").size() == 1);
    }

    {
        std::cout << "\n-- a paragraph's keeps, hyphenation (s094) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto body = *d.as<core::Style>("style.1");
        check("set: keep with next, keep lines, hyphenate -- one step",
              core::doc::set_para_local(j, d, "story.1", {3, 0}, {3, 0}, "Keeps", [&](core::ParaLocal& o) {
                  o.keep_next = body.keep_next ? 0 : 1; o.keep_lines = -1; o.hyphenate = !body.hyphenate; }));
        const auto& p3 = d.as<core::Story>("story.1")->paras.at(3);
        const auto e = core::doc::para_style(d, p3);
        check("para_style: its own keeps and hyphenation", e.keep_next == (body.keep_next ? 0 : 1) && e.keep_lines == -1 && e.hyphenate == !body.hyphenate);
        const auto f = core::text_fields(d, "story.1", {3, 0}, {3, 0});
        std::string kl, kl_own, hy_own = "?";
        for (const auto& x : f) {
            if (x.edit == "pkeep_lines") { kl = x.value; kl_own = x.own; }
            if (x.edit == "phyphenate") hy_own = x.own;
        }
        check("inspector: Keep lines All together, its own; Hyphenate its own", kl == "All together" && kl_own == "para.keep_lines" &&
                                                                               hy_own == "para.hyphenate");
        check("clear: keep lines back to the style's", core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"keep_lines"}) &&
                                                      !d.as<core::Story>("story.1")->paras.at(3).over.keep_lines &&
                                                      d.as<core::Story>("story.1")->paras.at(3).over.keep_next.has_value());
        std::string err;
        core::Doc g;
        check("json: keeps and hyphenation kept", core::from_json(g, core::to_json(d), &err) &&
                                                 g.as<core::Story>("story.1")->paras.at(3).over == d.as<core::Story>("story.1")->paras.at(3).over, err);
    }

    {
        std::cout << "\n-- a paragraph's own settings (s093) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& ded = d.as<core::Story>("story.3")->paras.at(0);
        check("sample: the dedication -- Body, centred on its own", ded.style == "style.1" && ded.over.align == core::Align::Center &&
                                                                    core::doc::para_style(d, ded).align == core::Align::Center &&
                                                                    d.as<core::Style>("style.1")->align != core::Align::Center);
        const auto x_of = [&](int para) {
            L::FlowCache f;
            return L::caret_at(d, "story.1", f.get(d, "story.1"), {para, 0}).x;
        };
        const double x0 = x_of(3);
        const auto n0 = j.size();
        check("set: one step over a range", core::doc::set_para_local(j, d, "story.1", {3, 0}, {4, 0}, "Left indent",
                                                                      [](core::ParaLocal& o) { o.left_indent = 36; }) &&
                                            j.size() == n0 + 1 && d.as<core::Story>("story.1")->paras.at(4).over.left_indent == 36.0);
        check("set: the same again -- no step", !core::doc::set_para_local(j, d, "story.1", {3, 0}, {4, 0}, "Left indent",
                                                                           [](core::ParaLocal& o) { o.left_indent = 36; }));
        check("layout: the paragraph set in 36 pt", std::abs(x_of(3) - x0 - 36) < 0.5);
        // Enter in the middle: both halves keep it; Enter at the end: the next paragraph (Next style) doesn't.
        core::doc::edit_text(j, d, "story.1", {3, 4}, {3, 4}, "\n");
        const auto& ps = d.as<core::Story>("story.1")->paras;
        check("Enter mid-paragraph: both halves keep their own settings", ps.at(3).over.left_indent == 36.0 && ps.at(4).over.left_indent == 36.0);
        const int e = static_cast<int>(ps.at(4).text.size());
        core::doc::edit_text(j, d, "story.1", {4, e}, {4, e}, "\n");
        check("Enter at the end: a fresh paragraph, its style's", d.as<core::Story>("story.1")->paras.at(5).over.empty());
        check("clear: one setting back to the style's", core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"left_indent"}) &&
                                                       !d.as<core::Story>("story.1")->paras.at(3).over.left_indent &&
                                                       d.as<core::Story>("story.1")->paras.at(4).over.left_indent == 36.0);
        check("clear: nothing set -- no step", !core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"left_indent"}));
        // The inspector while typing: effective values, the own ones marked.
        {
            const auto f = core::text_fields(d, "story.3", {0, 0}, {0, 0});
            std::string align_own, align_val, lind_own = "?";
            for (const auto& x : f) {
                if (x.edit == "palign") { align_own = x.own; align_val = x.value; }
                if (x.edit == "plindent") lind_own = x.own;
            }
            check("inspector: Align Center, its own; Left indent the style's", align_val == "Center" && align_own == "para.align" &&
                                                                              lind_own.empty());
        }
        std::string err;
        core::Doc g;
        check("json: a paragraph's own settings kept", core::from_json(g, core::to_json(d), &err) &&
                                                      g.as<core::Story>("story.1")->paras.at(4).over == d.as<core::Story>("story.1")->paras.at(4).over &&
                                                      g.as<core::Story>("story.3")->paras.at(0).over.align == core::Align::Center, err);
    }

    {
        std::cout << "\n-- own settings, row by row (s092) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto rows = [&](const std::string& id) {
            std::map<std::string, std::string> m;   // label -> own key
            const auto o = core::project(d, "");
            for (const auto* it : o.in(core::Segment::Styles))
                if (it->id == id)
                    for (const auto& f : it->fields) if (!f.own.empty()) m[f.label] = f.own;
            return m;
        };
        auto m = rows("style.10");
        check("rows: Body First's own -- First indent and Drop cap, marked", m.size() == 2 && m["First indent"] == "first_indent" &&
                                                                            m["Drop cap"] == "drop_lines");
        check("rows: a style on no base -- nothing marked", rows("style.1").empty());
        check("edit keys -> style keys", core::doc::style_key_of_edit("ssize") == "size" && core::doc::style_key_of_edit("rule_a_w") == "rule_above" &&
                                         core::doc::style_key_of_edit("list_hang") == "list" && core::doc::style_key_of_edit("pstart") == "start" &&
                                         core::doc::style_key_of_edit("zoom").empty());
        const auto base = *d.as<core::Style>("style.1");
        const auto n0 = j.size();
        check("reset one: one step", core::doc::reset_style_fields(j, d, {"style.10"}, {"drop_lines"}) && j.size() == n0 + 1);
        const auto& bf = *d.as<core::Style>("style.10");
        check("reset one: the base's value, no longer its own; the other kept", bf.drop_lines == base.drop_lines &&
              bf.own == std::vector<std::string>{"first_indent"});
        check("reset one: not owned -- no step", !core::doc::reset_style_fields(j, d, {"style.10"}, {"drop_lines"}) &&
                                                 !core::doc::reset_style_fields(j, d, {"style.1"}, {"size"}));
        m = rows("style.10");
        check("rows: the reset row unmarked", m.size() == 1 && m.count("First indent") == 1);
        j.undo();
        check("reset one: undone", d.as<core::Style>("style.10")->own.size() == 2 && d.as<core::Style>("style.10")->drop_lines >= 2);
        // Several picked: marked only where every one owns it.
        {
            const auto o = core::project(d, "");
            std::vector<const core::Item*> sel;
            for (const auto* it : o.in(core::Segment::Styles)) if (it->id == "style.10" || it->id == "style.1") sel.push_back(it);
            bool any = false;
            for (const auto& f : core::shared_fields(sel)) any = any || !f.own.empty();
            check("several picked: not marked unless all own it", sel.size() == 2 && !any);
        }
    }

    {
        std::cout << "\n-- find: Unicode case (s091) --\n";
        namespace D = core::doc;
        check("fold: Latin-1, Extended-A, Greek, Cyrillic, Armenian, fullwidth",
              D::fold_char(U'É') == U'é' && D::fold_char(U'Ž') == U'ž' && D::fold_char(U'Ł') == U'ł' && D::fold_char(U'Σ') == U'σ' &&
              D::fold_char(U'ς') == U'σ' && D::fold_char(U'Ά') == U'ά' && D::fold_char(U'Ж') == U'ж' && D::fold_char(U'Ё') == U'ё' &&
              D::fold_char(U'Ա') == U'ա' && D::fold_char(U'Ａ') == U'ａ' && D::fold_char(U'Ẁ') == U'ẁ' && D::fold_char(U'×') == U'×' &&
              D::fold_char(U'é') == U'é' && D::fold_char(U'7') == U'7');
        core::Doc d;
        core::load_sample(d);
        {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras.at(0).text = "ÉTÉ à Москва; ΟΔΟΣ and οδος; Łódź";
            d.put("story.3", st);
        }
        const auto hits = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text, bool mc = false, bool ww = false) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.match_case = mc;
            f.whole_word = ww;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        const auto& txt = d.as<core::Story>("story.3")->paras.at(0).text;
        const auto h1 = hits("été");
        check("text: été finds ÉTÉ, its bytes", h1.size() == 1 && txt.substr(static_cast<std::size_t>(h1[0].a.byte),
                                                                           static_cast<std::size_t>(h1[0].b.byte - h1[0].a.byte)) == "ÉTÉ");
        check("text: Cyrillic and Greek either way", hits("МОСКВА").size() == 1 && hits("οδοσ").size() == 2 && hits("ŁÓDŹ").size() == 1);
        check("text: match case still matches case", hits("été", D::FindOpts::Mode::Text, true).empty() && hits("ÉTÉ", D::FindOpts::Mode::Text, true).size() == 1);
        check("text: whole word with letters past ASCII", hits("ét", D::FindOpts::Mode::Text, false, true).empty() &&
                                                         hits("été", D::FindOpts::Mode::Text, false, true).size() == 1);
        const auto h2 = hits("(é)t(é)", D::FindOpts::Mode::Regex);
        check("regex: folded both sides, the groups as written", h2.size() == 1 && h2[0].groups.size() == 3 && h2[0].groups[1] == "É");
        check("regex: \\W still means not-a-word", hits("\\W\\W", D::FindOpts::Mode::Regex).size() >= 1 &&
                                                  hits("Москва\\W", D::FindOpts::Mode::Regex).size() == 1);
        check("wildcards: ^$ and folded letters", hits("Ł^$dź", D::FindOpts::Mode::Wildcards).size() == 1);
    }

    {
        std::cout << "\n-- contents out of date by an automatic level's style (s170) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"});
        L::FlowCache flows;
        L::build_list(j, d, "list.1");
        const auto made = d.as<core::GenList>("list.1")->entry_styles.at(1);
        const auto stale = [&]() -> std::string {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.1") return x.why;
            return "current";
        };
        check("built: current", stale() == "current", stale());
        check("level 2 automatic again: found by name -- current", core::doc::set_list_entry_style(j, d, "list.1", 2, "") && stale() == "current", stale());
        const auto name = d.as<core::Style>(made)->name;
        core::doc::edit_styles(j, d, {made}, "Name", [](core::Style& st) { st.name = "Figure Entry"; });
        check("its style renamed: none of the name -- out of date", stale() == "No " + name + " style yet", stale());
        j.undo();
        check("named again: current", stale() == "current", stale());
        {   // another style takes the name; the entries are still in the old one
            core::doc::edit_styles(j, d, {made}, "Name", [](core::Style& st) { st.name = "Old Entry"; });
            const auto other = core::doc::duplicate_style(j, d, made);
            core::doc::edit_styles(j, d, {other}, "Name", [&](core::Style& st) { st.name = name; });
            check("another style of the name: entries not in it", stale() == "Entries not in " + name, stale());
        }
    }

    {
        std::cout << "\n-- override several master items with a marquee (s169) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::vector<std::string> sp;
        for (const auto& x : core::spreads(d)) if (x.size() == 2 && !D::overridable_items(d, x[0]).empty() && !D::overridable_items(d, x[1]).empty()) { sp = x; break; }
        check("fixture: a spread whose pages show master items", !sp.empty());
        const auto pl = core::layout_pages(d, sp);
        const auto all = core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4});
        check("all of both pages: each page's items", all.size() == D::overridable_items(d, sp[0]).size() + D::overridable_items(d, sp[1]).size());
        const auto right = core::master_items_in(d, pl, {pl[1].r.x + 1, -1e4, 1e4, 2e4});
        check("the right page alone", !right.empty() && std::all_of(right.begin(), right.end(), [&](const core::MasterItemOn& m) { return m.page == sp[1]; }));
        const auto made = D::override_master_items(j, d, all);
        check("overridden: one step, a copy each", made.size() == all.size() && j.size() == 1 &&
              std::all_of(made.begin(), made.end(), [&](const auto& id) { return d.as<core::Frame>(id) && !d.as<core::Frame>(id)->overrides.empty(); }));
        check("now none to touch there", core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4}).empty());
        check("again: nothing, no step", D::override_master_items(j, d, all).empty() && j.size() == 1);
        j.undo();
        check("undo: the master's again", core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4}).size() == all.size());
        check("marquee overrides: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- column rules stop at a spanning paragraph (s168) --\n";
        namespace L = composix::layout;
        core::Frame f;
        f.kind = core::FrameKind::Text;
        f.w = 300;
        f.h = 400;
        f.columns = 2;
        f.col_rule.weight = 1;
        f.col_rule.inset = 4;
        check("no flow: one rule down the gutter", L::column_rules(f).size() == 1);
        L::FrameText ft;
        L::Line a{0, 0, 10, 20, 14, 0, 0};
        L::Line s1{1, 0, 10, 100, 28, 0, 0};
        s1.cw = 300;
        s1.band = 1;
        L::Line s2 = s1;
        s2.y = 128;
        L::Line b{2, 0, 10, 170, 14, 0, 0};
        b.band = 2;
        ft.lines = {a, s1, s2, b};
        const auto r = L::column_rules(f, &ft);
        check("a spanning paragraph: the rule above it and under it", r.size() == 2 &&
              std::abs((r[0].r.y + r[0].r.h) - (100 - 4)) < 0.01 && std::abs(r[1].r.y - (156 + 4)) < 0.01 &&
              std::abs(r[1].r.y + r[1].r.h - (400 - 4)) < 0.01);
        L::Line top = s1;
        top.y = 0;
        ft.lines = {top, b};
        const auto r2 = L::column_rules(f, &ft);
        check("at the top: only under it", r2.size() == 1 && r2[0].r.y > 28);
    }

    {
        std::cout << "\n-- a sidebar's footnotes at the page's foot (s167) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const std::string page = d.as<core::Frame>("frame.1")->owner;
        const core::Rect mr = core::margin_rect(d, page);
        core::Doc::Id body;
        for (const auto& id : d.ids_of<core::Style>()) if (d.as<core::Style>(id)->name == "Body") body = id;
        {
            core::Doc::Loading load(d);
            core::Story side{"Sidebar", {}};
            core::Para pp;
            pp.style = body;
            pp.text = "A sidebar's words, with a note of its own.";
            side.paras.push_back(pp);
            d.put("story.90", side);
            core::Frame f;
            f.owner = page;
            f.kind = core::FrameKind::Text;
            f.x = mr.x;
            f.y = mr.y + 40;
            f.w = mr.w * 0.4;
            f.h = 120;
            f.z = 50;
            f.story = "story.90";
            d.put("frame.90", f);
        }
        D::insert_note(j, d, "story.90", {0, 10}, "The sidebar's note -- at the page's foot, under the main text.");
        L::FlowCache fl;
        const auto& sf = fl.get(d, "story.90");
        const auto* fr = d.as<core::Frame>("frame.90");
        const L::NotePlace* np = nullptr;
        for (const auto& ft : sf.frames) if (ft.frame == "frame.90" && !ft.notes.empty()) np = &ft.notes.front();
        check("the sidebar's note is set", np != nullptr);
        if (np) {
            check("at the margins' width", std::abs(np->x - (mr.x - fr->x)) < 0.01 && std::abs(np->w - mr.w) < 0.01);
            check("at the page's foot", std::abs(fr->y + np->y + np->h - mr.b()) < 0.5, std::to_string(fr->y + np->y + np->h) + " vs " + std::to_string(mr.b()));
            const double top_of_notes = fr->y + np->y;
            const auto& mf = fl.get(d, "story.1");
            bool clear = true;
            for (const auto& ft : mf.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (f->owner != page) continue;
                for (const auto& l : ft.lines) if (f->y + l.y + l.h > top_of_notes + 0.5) clear = false;
                for (const auto& n : ft.notes) if (f->y + n.y + n.h > top_of_notes + 0.5) clear = false;
            }
            check("the main text (and its notes) stop above it", clear);
            // the sidebar's words change (text only): the main story is flowed again around more notes
            D::insert_note(j, d, "story.90", {0, 30}, "A second note, longer than the first, so the foot it takes grows by a line or two at least.");
            const auto& sf2 = fl.get(d, "story.90");
            double top2 = 1e9;
            for (const auto& ft : sf2.frames) if (ft.frame == "frame.90") for (const auto& n : ft.notes) top2 = std::min(top2, fr->y + n.y);
            const auto& mf2 = fl.get(d, "story.1");
            bool clear2 = true;
            for (const auto& ft : mf2.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (f->owner != page) continue;
                for (const auto& l : ft.lines) if (f->y + l.y + l.h > top2 + 0.5) clear2 = false;
            }
            check("a note added to the sidebar: the main text stops higher", top2 < top_of_notes && clear2);
        }
        check("a wide frame's notes stay its own (the main story's)", [&] {
            const auto& mf = fl.get(d, "story.1");
            for (const auto& ft : mf.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                for (const auto& n : ft.notes) if (std::abs(n.w - core::column_width(*f, 0)) > 0.01 && core::column_count(*f) == 1) return false;
            }
            return true;
        }());
        {   // a sidebar reaching down to the foot: its own words stop above its notes
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.90");
            f.h = mr.b() - f.y;
            d.put("frame.90", f);
            auto st = *d.as<core::Story>("story.90");
            std::string more;
            for (int k = 0; k < 40; ++k) more += " More words of the sidebar, running down the page.";
            st.paras.at(0).text += more;
            d.put("story.90", st);
        }
        {
            L::FlowCache f2;
            const auto& sf3 = f2.get(d, "story.90");
            const auto* f9 = d.as<core::Frame>("frame.90");
            double ntop = 1e9, lbot = 0;
            for (const auto& ft : sf3.frames) if (ft.frame == "frame.90") {
                for (const auto& n : ft.notes) ntop = std::min(ntop, n.y);
                for (const auto& l : ft.lines) lbot = std::max(lbot, l.y + l.h);
            }
            check("a sidebar down to the foot: its words stop above its notes", ntop < 1e9 && lbot <= ntop + 0.5,
                  std::to_string(lbot) + " / " + std::to_string(ntop) + " (frame h " + std::to_string(f9->h) + ")");
        }
        check("foot neighbours: the main story waits on the sidebar's", [&] {
            const auto nb = L::foot_neighbours(d, "story.90");
            return std::find(nb.begin(), nb.end(), "story.1") != nb.end();
        }());
    }

    {
        std::cout << "\n-- a paragraph spanning the columns: bands (s166) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // the chapters in two columns
            core::Doc::Loading load(d);
            for (const auto& fid : d.ids_of<core::Frame>()) {
                auto f = *d.as<core::Frame>(fid);
                if (f.story != "story.1") continue;
                f.columns = 2;
                d.put(fid, f);
            }
        }
        core::Doc::Id head;
        for (const auto& id : d.ids_of<core::Style>()) if (d.as<core::Style>(id)->name == "Heading 1") head = id;
        const auto sp = D::duplicate_style(j, d, head);
        check("Span columns: a style setting", D::style_edit_label("span") == "Span columns" &&
              D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "1"); st.start = core::ParaStart::Anywhere; }) &&
              d.as<core::Style>(sp)->span);
        // a spanning head part way through the opening chapter
        const int P = 3;
        D::set_para_style(j, d, "story.1", {P, 0}, {P, 0}, sp);
        L::FlowCache fl;
        const auto& f = fl.get(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& x : f.frames) for (const auto& l : x.lines) if (l.para == P) ft = &x;
        check("the spanning paragraph is set", ft != nullptr);
        if (ft) {
            const auto* fr = d.as<core::Frame>(ft->frame);
            const double x0 = core::column_x(*fr, 0), x1 = core::column_x(*fr, 1) + core::column_width(*fr, 1);
            double s_top = 1e9, s_bot = -1;
            bool wide = true;
            for (const auto& l : ft->lines)
                if (l.para == P) {
                    s_top = std::min(s_top, l.y);
                    s_bot = std::max(s_bot, l.y + l.h);
                    wide = wide && std::abs(l.cw - (x1 - x0)) < 0.01 && l.x >= x0 - 0.01 && l.col == 0 && (l.w > 0 || std::abs(l.x - x0) < 0.01);
                }
            std::string dbg;
            for (const auto& l : ft->lines) if (l.para == P) dbg += "[x" + std::to_string(l.x) + " cw" + std::to_string(l.cw) + " c" + std::to_string(l.col) + " b" + std::to_string(l.band) + " w" + std::to_string(l.w) + "]";
            check("across both columns, its own width", wide, dbg + " x0 " + std::to_string(x0) + " x1 " + std::to_string(x1));
            bool above_ok = true, below_ok = true, two_above = false, two_below = false;
            std::set<int> cols_above, cols_below;
            for (const auto& l : ft->lines) {
                if (l.para < P) { above_ok = above_ok && l.y + l.h <= s_top + 0.01; cols_above.insert(l.col); }
                if (l.para > P) { below_ok = below_ok && l.y >= s_bot - 0.01; cols_below.insert(l.col); }
            }
            two_above = cols_above.size() == 2 || cols_above.empty();
            two_below = cols_below.size() == 2 || cols_below.size() <= 1;
            check("the text before it: above it", above_ok);
            check("the text before it: balanced over both columns", two_above);
            check("the text after it: below it, in columns", below_ok && two_below);
            int bands = 0;
            for (const auto& l : ft->lines) bands = std::max(bands, l.band + 1);
            check("bands: before, across, after", bands >= 1 + (cols_above.empty() ? 0 : 1) + (cols_below.empty() ? 0 : 1),
                  std::to_string(bands));
            // balanced: the two columns above end within a line or so of each other
            double b0 = 0, b1 = 0;
            for (const auto& l : ft->lines) if (l.para < P) (l.col == 0 ? b0 : b1) = std::max(l.col == 0 ? b0 : b1, l.y + l.h);
            const double lead = 14;
            check("balanced: the columns above end together", cols_above.size() < 2 || std::abs(b0 - b1) <= 2 * lead + 0.01,
                  std::to_string(b0) + " / " + std::to_string(b1));
        }
        check("no spanning paragraph: as before (one band)", [&] {
            core::Doc e;
            core::load_sample(e);
            L::FlowCache fe;
            for (const auto& x : fe.get(e, "story.1").frames) for (const auto& l : x.lines) if (l.band != 0 || l.cw != 0) return false;
            return true;
        }());
        {
            core::Doc e;
            check("json: span round-trips", core::from_json(e, core::to_json(d)) && e.as<core::Style>(sp)->span);
        }
        check("span: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- a style's setting, one code path; the specimen shows what's waiting (s164) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        core::Doc::Id body, emph;
        for (const auto& id : d.ids_of<core::Style>()) {
            if (d.as<core::Style>(id)->name == "Body") body = id;
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character && emph.empty()) emph = id;
        }
        core::Style b = *d.as<core::Style>(body);
        check("labels: a plain setting has one, the others none", D::style_edit_label("ssize") == "Size" &&
              D::style_edit_label("rule_b_w") == "Rule" && D::style_edit_label("based_on").empty() && D::style_edit_label("style").empty());
        const double lead0 = b.leading, size0 = b.size;
        D::style_edit(d, b, "ssize", "13");
        check("size: set; leading follows when it was the usual ratio",
              b.size == 13 && (std::abs(lead0 - size0 * 1.2) < 1.5 ? b.leading == std::round(13 * 1.2 * 2) / 2 : b.leading == lead0));
        D::style_edit(d, b, "align", "1");
        D::style_edit(d, b, "rule_b_w", "2");
        check("align and a rule below (offset clear of the text)", b.align == core::Align::Center && b.rule_below.weight == 2 &&
                                                                  b.rule_below.offset > 0);
        D::style_edit(d, b, "list_type", "1");
        check("a list makes room for its marker", b.list.type == core::ListType::Bullet && b.left_indent >= b.list.hang);
        core::Style e = *d.as<core::Style>(emph);
        const auto e0 = e;
        D::style_edit(d, e, "lindent", "36");
        check("a paragraph setting leaves a character style alone", e == e0);
        L::StyleOver over{{body, b}};
        const auto sd = L::style_specimen_doc(d, body, false, 432, 324, &over);
        check("specimen: the waiting change shown", sd.as<core::Style>(body)->size == 13 && sd.as<core::Style>(body)->align == core::Align::Center);
        check("...the document's style as it was", d.as<core::Style>(body)->size == size0);
    }

    {
        std::cout << "\n-- a style, greeked: on its own and in use (s163) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        core::Doc::Id body, head, emph;
        for (const auto& id : d.ids_of<core::Style>()) {
            const auto* s = d.as<core::Style>(id);
            if (s->name == "Body") body = id;
            if (s->name == "Heading 1") head = id;
            if (s->kind == core::StyleKind::Character && emph.empty()) emph = id;
        }
        check("fixture: Body, Heading 1, a character style", !body.empty() && !head.empty() && !emph.empty());
        const auto alone = L::style_specimen_doc(d, body, false, 432, 324);
        const auto* st = alone.as<core::Story>("story.1");
        check("on its own: paragraphs all in it", st && st->paras.size() >= 2 &&
              std::all_of(st->paras.begin(), st->paras.end(), [&](const core::Para& p) { return p.style == body; }));
        const auto use = L::style_specimen_doc(d, head, true, 432, 648);
        const auto* su = use.as<core::Story>("story.1");
        int mine = 0, other = 0;
        for (const auto& p : su->paras) (p.style == head ? mine : other)++;
        check("in use: it among others", mine >= 1 && other >= 2);
        const auto use_styles = use.ids_of<core::Style>();
        check("in use: a next-page start set where it falls (one page)",
              std::all_of(use_styles.begin(), use_styles.end(),
                          [&](const auto& id) { return use.as<core::Style>(id)->start == core::ParaStart::Anywhere; }));
        L::FlowCache fl;
        check("in use: it all fits the page", !fl.get(use, "story.1").overset);
        const auto ch = L::style_specimen_doc(d, emph, true, 432, 648);
        bool run = false;
        for (const auto& p : ch.as<core::Story>("story.1")->paras) for (const auto& r : p.runs) run = run || r.style == emph;
        check("a character style: on some words of the body", run);
        check("the document untouched", d.strays() == 0);
        auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 800, 600);
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_paint(cr);
        L::draw_style_specimen(cr, d, head, 800, 600);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        int white = 0;
        const auto* data = cairo_image_surface_get_data(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < 600; y += 4)
            for (int x = 0; x < 800; x += 4) white += (reinterpret_cast<const std::uint32_t*>(data + y * stride)[x] & 0xffffff) == 0xffffff;
        cairo_surface_destroy(surf);
        check("drawn: paper on the pasteboard", white > 1000);
    }

    {
        std::cout << "\n-- a style's rows in groups; Set as one step (s161 / s162) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto o = core::project(d, "");
        const core::Item* para = nullptr;
        for (const auto* it : o.in(core::Segment::Styles))
            if (const auto* st = d.as<core::Style>(it->id); st && st->kind == core::StyleKind::Paragraph) { para = it; break; }
        check("a paragraph style item", para != nullptr);
        std::set<std::string> groups;
        bool ok = true;
        std::string why;
        for (const auto& f : para->fields) {
            if (f.label == "Kind" || f.buttons) { ok = ok && (f.group.empty() || f.edit == "style_base"); continue; }   // Kind above, actions below
            if (f.group.empty()) { ok = false; why = f.label; }
            groups.insert(f.group);
        }
        check("every row but Kind in a group", ok, why);
        check("shared fields keep their groups", [&] {
            const auto sh = core::shared_fields({para});
            return std::all_of(sh.begin(), sh.end(), [](const core::Field& f) { return f.label == "Kind" || f.buttons || !f.group.empty(); });
        }());
        check("groups: Text, Paragraph, Flow, List, Rules, Style",
              groups.count("Text") && groups.count("Paragraph") && groups.count("Flow") && groups.count("List") && groups.count("Rules") &&
              groups.count("Style"));
        {   // each group's rows together (one heading each)
            std::vector<std::string> seq;
            for (const auto& f : para->fields) if (!f.group.empty() && (seq.empty() || seq.back() != f.group)) seq.push_back(f.group);
            check("each group's rows together", std::set<std::string>(seq.begin(), seq.end()).size() == seq.size());
        }
        const auto sid = para->id;
        const auto before = *d.as<core::Style>(sid);
        const auto n0 = j.size();
        const bool did = D::styles_as_one(j, d, "Style settings", [&] {
            D::edit_styles(j, d, {sid}, "Size", [](core::Style& s) { s.size = 15; });
            D::edit_styles(j, d, {sid}, "Space after", [](core::Style& s) { s.space_after = 9; });
        });
        check("Set: one step for both", did && j.size() == n0 + 1 && j.undo_label() == "Style settings" &&
                                        d.as<core::Style>(sid)->size == 15 && d.as<core::Style>(sid)->space_after == 9);
        j.undo();
        check("undo: both back", *d.as<core::Style>(sid) == before);
        check("Set with nothing changed: no step", !D::styles_as_one(j, d, "Style settings", [] {}));
        check("groups: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- find bold / italic by the paragraph style too (s160) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // a bold paragraph style on paragraph 0, a word made italic by hand in paragraph 1
        const auto& st = *d.as<core::Story>("story.1");
        const auto ps0 = st.paras.at(0).style;
        D::edit_styles(j, d, {ps0}, "Weight", [](core::Style& s) { s.bold = true; });
        const bool other_bold = d.as<core::Style>(st.paras.at(1).style)->bold;
        D::set_local(j, d, "story.1", {1, 0}, {1, 3}, "Bold", [](core::Local& l) { l.bold = 1; });
        D::FindOpts f;
        f.bold = true;
        f.story = "story.1";
        const auto by_hand = D::find_all(d, f);
        check("bold, by hand only: not the bold style's paragraph",
              std::none_of(by_hand.begin(), by_hand.end(), [](const D::Hit& h) { return h.a.para == 0; }) &&
              std::any_of(by_hand.begin(), by_hand.end(), [](const D::Hit& h) { return h.a.para == 1 && h.a.byte == 0 && h.b.byte == 3; }));
        f.para_counts = true;
        const auto with = D::find_all(d, f);
        const int len0 = static_cast<int>(d.as<core::Story>("story.1")->paras.at(0).text.size());
        check("by the paragraph style too: the whole bold paragraph",
              std::any_of(with.begin(), with.end(), [&](const D::Hit& h) { return h.a.para == 0 && h.a.byte == 0 && h.b.byte == len0; }));
        if (!other_bold)
            check("...and still the word set by hand", std::any_of(with.begin(), with.end(), [](const D::Hit& h) { return h.a.para == 1 && h.b.byte == 3; }));
        D::set_local(j, d, "story.1", {0, 0}, {0, 2}, "Bold", [](core::Local& l) { l.bold = 0; });
        const auto with2 = D::find_all(d, f);
        check("words set not-bold by hand in a bold style: left out",
              std::any_of(with2.begin(), with2.end(), [&](const D::Hit& h) { return h.a.para == 0 && h.a.byte == 2 && h.b.byte == len0; }));
        f.what = "a";
        check("with words too", !D::find_all(d, f).empty() || len0 == 0);
    }

    {
        std::cout << "\n-- No break on the words themselves (s159) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& p1 = [&]() -> const core::Para& { return d.as<core::Story>("story.1")->paras.at(1); };
        check("none to start", core::no_break_spans(d, p1()).empty());
        check("No break on: one step", D::set_local(j, d, "story.1", {1, 4}, {1, 20}, "No break", [](core::Local& l) { l.no_break = 1; }) &&
                                      j.undo_label() == "No break");
        const auto sp = core::no_break_spans(d, p1());
        check("its words kept together", sp.size() == 1 && sp[0].a == 4 && sp[0].b == 20);
        {
            core::Doc e;
            check("json: round-trips", core::from_json(e, core::to_json(d)) && core::no_break_spans(e, e.as<core::Story>("story.1")->paras.at(1)).size() == 1);
        }
        // a character style with No break, turned off on some of its words
        core::Doc::Id cs;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character) { cs = id; break; }
        D::edit_styles(j, d, {cs}, "No break", [](core::Style& st) { st.no_break = true; });
        D::set_local(j, d, "story.1", {1, 0}, {1, 20}, "No break", [](core::Local& l) { l.no_break = -1; });
        D::set_char_style(j, d, "story.1", {1, 30}, {1, 50}, cs);
        check("the character style's", core::no_break_spans(d, p1()).size() == 1);
        D::set_local(j, d, "story.1", {1, 30}, {1, 40}, "No break", [](core::Local& l) { l.no_break = 0; });
        const auto sp2 = core::no_break_spans(d, p1());
        check("Off on the words beats the style's", sp2.size() == 1 && sp2[0].a == 40 && sp2[0].b == 50);
        check("keep_together breaks before them", core::keep_together(p1().text, {{4, 20}}, 0, 10) <= 4);
        check("no break: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- a paragraph's own tab stops (s157) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& st0 = *d.as<core::Story>("story.1");
        const std::string ps = st0.paras.at(1).style;
        const auto style_tabs = d.as<core::Style>(ps)->tabs;
        std::vector<core::TabStop> mine{{144, core::TabAlign::Right, "."}, {72, core::TabAlign::Left, ""}};
        check("own stops: one step, sorted", core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Add tab", mine) &&
                                             j.undo_label() == "Add tab" && d.as<core::Story>("story.1")->paras[1].over.tabs &&
                                             d.as<core::Story>("story.1")->paras[1].over.tabs->front().pos == 72);
        check("the style keeps its own", d.as<core::Style>(ps)->tabs == style_tabs);
        check("the layout sets it in them", core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs.size() == 2 &&
                                            core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs[1].leader == ".");
        check("only that paragraph", !d.as<core::Story>("story.1")->paras[2].over.tabs);
        check("the same again: no step", !core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Add tab", mine));
        {
            core::Doc e;
            check("json: own stops round-trip", core::from_json(e, core::to_json(d)) && e.as<core::Story>("story.1")->paras[1].over.tabs &&
                                                *e.as<core::Story>("story.1")->paras[1].over.tabs == *d.as<core::Story>("story.1")->paras[1].over.tabs);
        }
        check("Reset to style: one step, the style's again",
              core::doc::clear_para_local(j, d, "story.1", {1, 0}, {1, 0}, {"tabs"}) && !d.as<core::Story>("story.1")->paras[1].over.tabs &&
              core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs == style_tabs);
        j.undo();
        check("undo: its own again", d.as<core::Story>("story.1")->paras[1].over.tabs.has_value());
        check("set to the style's: follows the style (none of its own)",
              core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Move tab", style_tabs) && !d.as<core::Story>("story.1")->paras[1].over.tabs);
        check("a range: each paragraph", core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {3, 0}, "Add tab", mine) &&
                                         d.as<core::Story>("story.1")->paras[2].over.tabs && d.as<core::Story>("story.1")->paras[3].over.tabs);
        {   // a style change later doesn't reach a paragraph with its own
            core::doc::edit_styles(j, d, {ps}, "Tabs", [](core::Style& s) { s.tabs = {{36, core::TabAlign::Left, ""}}; });
            check("its own stops hold over a style change", core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs.size() == 2 &&
                                                            d.as<core::Style>(ps)->tabs.size() == 1);
        }
        L::FlowCache f;
        check("still flows", !f.get(d, "story.1").frames.empty());
        check("own tabs: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- footnotes page width: one foot under all the columns (s156) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // the chapters in two columns
            core::Doc::Loading load(d);
            for (const auto& fid : d.ids_of<core::Frame>()) {
                auto f = *d.as<core::Frame>(fid);
                if (f.story != "story.1") continue;
                f.columns = 2;
                d.put(fid, f);
            }
        }
        const int np = static_cast<int>(d.as<core::Story>("story.1")->paras.size());
        for (int p = 1; p < std::min(np, 7); ++p)
            core::doc::insert_note(j, d, "story.1", {p, 2}, "A note for paragraph " + std::to_string(p) + ", a line or two long in the note size.");
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        int placed = 0, last = 0;
        bool wide = true, clear = true, ordered = true, inside = true;
        std::string why;
        for (const auto& ft : f.frames) {
            const auto* fr = d.as<core::Frame>(ft.frame);
            const int n = core::column_count(*fr);
            const double x0 = core::column_x(*fr, 0), x1 = core::column_x(*fr, n - 1) + core::column_width(*fr, n - 1);
            double top = 1e9, prev_y = -1;
            for (const auto& note : ft.notes) {
                ++placed;
                ordered = ordered && note.number > last && note.y > prev_y;
                last = note.number;
                prev_y = note.y;
                top = std::min(top, note.y);
                if (n > 1 && (std::abs(note.x - x0) > 0.01 || std::abs(note.w - (x1 - x0)) > 0.01)) { wide = false; why = ft.frame; }
                inside = inside && note.y + note.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
            }
            for (const auto& l : ft.lines)   // no line in ANY column runs into the foot
                if (l.y + l.h > top + 0.01) {
                    bool first = true;
                    for (const auto& m : ft.lines) if (m.col == l.col && m.y < l.y) first = false;
                    if (!first) { clear = false; why = ft.frame + " col " + std::to_string(l.col); }
                }
        }
        check("notes placed, numbered in order, stacked in order", placed >= 6 && ordered, std::to_string(placed));
        check("each note the width of all the columns", wide, why);
        check("no line in any column runs into the foot", clear, why);
        check("the foot inside its frame", inside);
        {   // one rule over the foot, across the first note's width share
            int rules = 0;
            for (const auto& ft : f.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                if (ft.notes.empty() || core::column_count(*fr) < 2) continue;
                std::set<int> cols;
                for (const auto& note : ft.notes) cols.insert(note.col);
                rules += static_cast<int>(cols.size());
                check("one foot: one rule over it (" + ft.frame + ")", cols.size() == 1);
            }
            check("a frame with notes in two columns", rules >= 1);
        }
        {   // one column: as before -- the column's width
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            core::doc::insert_note(je, e, "story.1", {1, 4}, "One note.");
            L::FlowCache fe;
            bool same = false;
            for (const auto& ft : fe.get(e, "story.1").frames)
                for (const auto& note : ft.notes) {
                    const auto* fr = e.as<core::Frame>(ft.frame);
                    if (core::column_count(*fr) == 1) same = std::abs(note.w - core::column_width(*fr, 0)) < 0.01;
                }
            check("one column: the note its width", same);
        }
    }

    {
        std::cout << "\n-- master items across the spine (s155) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // a facing master, a spread of pages using it
        std::string M, Lp, Rp;
        for (const auto& sp : core::spreads(d)) {
            if (sp.size() != 2) continue;
            const auto* a = d.as<core::Page>(sp[0]);
            const auto* b = d.as<core::Page>(sp[1]);
            if (!a || !b || a->master.empty() || a->master != b->master) continue;
            const auto* m = d.as<core::Master>(a->master);
            if (m && m->facing && m->folds.empty() && a->w == m->w && b->w == m->w) { M = a->master; Lp = sp[0]; Rp = sp[1]; break; }
        }
        check("fixture: a facing master's spread", !M.empty());
        const double w = d.as<core::Master>(M)->w;
        const auto id = core::doc::add_frame(j, d, M, core::FrameKind::Shape, {w - 100, 100, 160, 50}, 1, 12, true);
        const auto& f = *d.as<core::Frame>(id);
        check("drawn across: one item, across, both sides", !id.empty() && f.across && f.side == core::Side::Any && j.size() == 1);
        check("dx: none on the left, a page width back on the right",
              core::across_dx(d, f, core::Side::Left) == 0 && core::across_dx(d, f, core::Side::Right) == -w);
        check("shows on both pages", core::master_item_on(d, id, Lp) && core::master_item_on(d, id, Rp));
        const auto mpl = core::layout_pages(d, {M});
        check("master spread: on both panels, one place", mpl.size() == 2 && core::on_panel(d, mpl[0], id) && core::on_panel(d, mpl[1], id) &&
                                                          core::frame_in_layout(d, mpl, id).x == mpl[0].r.x + w - 100);
        const auto pl = core::layout_pages(d, {Lp, Rp});
        core::Doc::Id on;
        check("hit: its right part on the right page", core::hit_master_item(d, pl, pl[1].r.x + 30, 120, 0, &on) == id && on == Rp);
        check("hit: its left part on the left page", core::hit_master_item(d, pl, pl[0].r.x + w - 50, 120, 0, &on) == id && on == Lp);
        const auto t = core::snap_targets(d, pl, {});
        check("snap: its right edge on the right page", std::any_of(t.xs.begin(), t.xs.end(), [&](double x) { return std::abs(x - (pl[1].r.x + 60)) < 1e-6; }));
        {   // drawn: each page its part, cut at the spine
            core::Doc::Loading load(d);
            auto g = f;
            for (const auto& sw : d.ids_of<core::Swatch>()) if (d.as<core::Swatch>(sw)->name == "Black") g.fill = sw;
            d.put(id, g);
        }
        L::FlowCache flows;
        const auto ink_at = [&](const std::string& page, int x, int y) {
            const auto* pg = d.as<core::Page>(page);
            auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(pg->w), static_cast<int>(pg->h));
            cairo_t* cr = cairo_create(surf);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
            L::draw_page(cr, d, page, flows, {}, nullptr, {});
            cairo_destroy(cr);
            cairo_surface_flush(surf);
            const auto* row = cairo_image_surface_get_data(surf) + y * cairo_image_surface_get_stride(surf);
            const bool ink = (reinterpret_cast<const std::uint32_t*>(row)[x] & 0xffffff) != 0xffffff;
            cairo_surface_destroy(surf);
            return ink;
        };
        check("drawn: the left page its left part", ink_at(Lp, static_cast<int>(w) - 50, 120) && !ink_at(Lp, static_cast<int>(w) - 120, 120));
        check("drawn: the right page its right part", ink_at(Rp, 30, 120) && !ink_at(Rp, 80, 120));
        check("json: across round-trips", [&] { core::Doc e; return core::from_json(e, core::to_json(d)) && e.as<core::Frame>(id)->across; }());
        check("shows on Right pages: off the spine, its place kept", core::doc::set_shows_on(j, d, {id}, 1) && !d.as<core::Frame>(id)->across &&
                                                                    d.as<core::Frame>(id)->side == core::Side::Right && d.as<core::Frame>(id)->x == -100);
        check("...and back across", core::doc::set_shows_on(j, d, {id}, 3) && d.as<core::Frame>(id)->across && d.as<core::Frame>(id)->x == w - 100);
        j.undo();
        check("undo: Right again", !d.as<core::Frame>(id)->across && d.as<core::Frame>(id)->side == core::Side::Right);
        j.redo();
        const auto copy = core::doc::override_master_item(j, d, Rp, id);
        check("override on the right page: its copy where it showed -- cut at the spine (s182)",
              !copy.empty() && !d.as<core::Frame>(copy)->across && d.as<core::Frame>(copy)->x == 0 &&
              d.as<core::Frame>(copy)->w == 60);
        {   // s182 (W2): the left page's copy -- its part up to the spine; a picture stays put; text whole
            const auto lcopy = core::doc::override_master_item(j, d, Lp, id);
            const auto* lc = d.as<core::Frame>(lcopy);
            check("override on the left page: its part up to the spine", lc && lc->x == w - 100 && lc->w == 100);
            const auto pic = core::doc::add_frame(j, d, M, core::FrameKind::Image, {w - 120, 300, 200, 100}, 1, 12, true);
            {
                core::Doc::Loading load(d);
                core::Asset a;
                a.name = "spread.jpg";
                a.path = "/x/spread.jpg";
                a.px_w = 4000;
                a.px_h = 1000;   // 4:1 -- Fill covers 200 x 100 at 400 x 100, centred
                d.put("asset.s182", a);
                auto g = *d.as<core::Frame>(pic);
                g.asset = "asset.s182";
                d.put(pic, g);
            }
            const auto before = core::picture_rect({w - 120, 300, 200, 100}, *d.as<core::Frame>(pic), 4000, 1000);
            const auto rp = core::doc::override_master_item(j, d, Rp, pic);
            const auto* rf = d.as<core::Frame>(rp);
            const auto after = rf ? core::picture_rect({rf->x + w, rf->y, rf->w, rf->h}, *rf, 4000, 1000) : core::Rect{};
            const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
            check("override a picture on the right: cut at the spine, the picture where it was",
                  rf && near(rf->x, 0) && near(rf->w, 80) && rf->fit == core::ImageFit::Custom &&
                  near(after.x, before.x) && near(after.y, before.y) && near(after.w, before.w) && near(after.h, before.h));
            const auto tx = core::doc::add_frame(j, d, M, core::FrameKind::Text, {w - 50, 500, 100, 40}, 1, 12, true);
            const auto tc = core::doc::override_master_item(j, d, Lp, tx);
            check("override a text frame across: kept whole (its words would reflow)",
                  d.as<core::Frame>(tc) && d.as<core::Frame>(tc)->w == 100 && d.as<core::Frame>(tc)->x == w - 50);
            j.undo();
            j.undo();
            j.undo();
            j.undo();
            j.undo();
            check("overrides undone", !d.as<core::Frame>(lcopy) && !d.as<core::Frame>(rp) && !d.as<core::Frame>(tc));
        }
        // a single (not facing) master has no spine
        std::string single;
        for (const auto& m : d.ids_of<core::Master>()) if (!d.as<core::Master>(m)->facing && !d.frames_on(m).empty()) single = m;
        if (!single.empty()) check("single master: no across", !core::doc::set_shows_on(j, d, {d.frames_on(single).front()}, 3));
        check("across: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- ruler origin (s154) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::vector<std::string> sp;
        for (const auto& x : core::spreads(d)) if (x.size() == 2) { sp = x; break; }
        const std::string L = sp.at(0), R = sp.at(1);
        const double wl = d.as<core::Page>(L)->w;
        using O = core::RulerOrigin;
        const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
        check("origin: Spread by default -- the right page's zero its left page's corner",
              d.settings().ruler_origin == O::Spread && near(core::ruler_zero(d, L).x, 0) && near(core::ruler_zero(d, R).x, -wl));
        const auto pl = core::layout_pages(d, {L, R});
        const auto s1 = core::ruler_spans(d, pl);
        check("spans: a spread is one piece from its corner", s1.size() == 1 && near(s1[0].zero, 0));
        // a frame on the right page: X from the spread's corner
        std::string fr;
        for (const auto& f : d.frames_on(R)) { fr = f; break; }
        if (fr.empty()) {
            core::Doc::Loading load(d);
            d.put("frame.95", core::Frame{R, core::FrameKind::Shape, 54, 54, 72, 72, 0, 9});
            fr = "frame.95";
        }
        const auto bx = [&]() -> std::string {
            const auto items = core::frame_items(d, {fr});
            for (const auto& f : items.at(0).fields) if (f.edit == "bounds") return f.value.substr(0, f.value.find('\t'));
            return "";
        };
        const double fx = d.as<core::Frame>(fr)->x;
        check("inspector: X from the spread's corner", bx() == core::length_in(fx + wl, d.settings().units), bx());
        check("origin: one step, the zero point back", core::doc::set_ruler_origin(j, d, O::Page) && d.settings().ruler_origin == O::Page &&
                                                     j.undo_label() == "Ruler origin" && !core::doc::set_ruler_origin(j, d, O::Page));
        check("Page: each page its own zero", near(core::ruler_zero(d, R).x, 0) && bx() == core::length_in(fx, d.settings().units));
        const auto s2 = core::ruler_spans(d, pl);
        check("spans: Page -- two pieces, cut at the spine", s2.size() == 2 && near(s2[0].hi, wl) && near(s2[1].lo, wl) && near(s2[1].zero, wl));
        core::doc::set_ruler_origin(j, d, O::Spine);
        check("Spine: the left page's right edge, the right page's left", near(core::ruler_zero(d, L).x, wl) && near(core::ruler_zero(d, R).x, 0));
        const auto s3 = core::ruler_spans(d, pl);
        check("spans: Spine -- one piece, zero at the spine", s3.size() == 1 && near(s3[0].zero, wl));
        check("Spine: the zero point can't move", !core::doc::set_ruler_zero(j, d, 10, 10));
        core::doc::set_ruler_origin(j, d, O::Spread);
        const auto z = core::zero_at(d, pl, wl + 10, 20);
        check("zero_at: from the spread's corner", near(z.x, wl + 10) && near(z.y, 20));
        check("zero point: one step", core::doc::set_ruler_zero(j, d, z.x, z.y) && j.undo_label() == "Zero point");
        check("zero point: both pages read from it", near(core::ruler_zero(d, R).x, 10) && near(core::ruler_zero(d, L).x, wl + 10) &&
                                                    near(core::ruler_zero(d, R).y, 20));
        check("inspector: X / Y from the zero point", bx() == core::length_in(fx - 10, d.settings().units), bx() + " vs " + core::length_in(fx - 10, d.settings().units));
        {
            core::Doc e;
            check("json: origin and zero point round-trip", core::from_json(e, core::to_json(d)) && e.settings().ruler_origin == O::Spread &&
                                                            near(e.settings().zero_x, wl + 10) && near(e.settings().zero_y, 20));
        }
        check("origin again: the zero point back", core::doc::set_ruler_origin(j, d, O::Spread) && d.settings().zero_x == 0);
        j.undo();
        check("undo: the zero point returns", near(d.settings().zero_x, wl + 10));
        check("ruler origin: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- find: full case folding (s152) --\n";
        namespace D = core::doc;
        check("fold_text: sharp s (both cases) ss, ligatures their letters",
              D::fold_text("Straße") == "strasse" && D::fold_text("STRAẞE") == "strasse" &&
              D::fold_text("\uFB01ne \uFB04") == "fine ffl" && D::fold_text("ÉTÉ") == "été");
        core::Doc d;
        core::load_sample(d);
        {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras.at(0).text = "Die Straße, the STRASSE; a \uFB01ne line; ß alone";
            d.put("story.3", st);
        }
        const auto hits = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text, bool mc = false, bool ww = false) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.match_case = mc;
            f.whole_word = ww;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        const auto& txt = d.as<core::Story>("story.3")->paras.at(0).text;
        const auto word = [&](const D::Hit& h) { return txt.substr(static_cast<std::size_t>(h.a.byte), static_cast<std::size_t>(h.b.byte - h.a.byte)); };
        const auto h1 = hits("strasse");
        check("text: strasse finds Straße and STRASSE, their bytes", h1.size() == 2 && word(h1[0]) == "Straße" && word(h1[1]) == "STRASSE");
        const auto h2 = hits("STRAẞE");
        check("text: capital sharp s finds both", h2.size() == 2);
        const auto h3 = hits("fine");
        check("text: a ligature found by its letters", h3.size() == 1 && word(h3[0]) == "\uFB01ne");
        check("text: never half a sharp s", hits("s alone").empty() && hits("ss alone").size() == 1 && hits("ss").size() == 3);
        check("text: match case does not fold", hits("strasse", D::FindOpts::Mode::Text, true).empty() &&
                                               hits("Straße", D::FindOpts::Mode::Text, true).size() == 1);
        check("text: whole word through the fold", hits("strasse", D::FindOpts::Mode::Text, false, true).size() == 2);
        const auto h4 = hits("stra(ss)e", D::FindOpts::Mode::Regex);
        check("regex: folded, the groups as written", h4.size() == 2 && h4[0].groups.size() == 2 && h4[0].groups[1] == "ß" && h4[1].groups[1] == "SS");
        check("regex: sharp s in the pattern; in a class one for one", hits("straße", D::FindOpts::Mode::Regex).size() == 2 &&
                                                                     hits("ß+ alone", D::FindOpts::Mode::Regex).size() == 1);
        check("wildcards: sharp s in the pattern", hits("^$traße", D::FindOpts::Mode::Wildcards).size() == 2);
    }

    {
        std::cout << "\n-- level styles, page numbers (s090) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto stale = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().why;
        };
        L::build_list(j, d, "list.1");
        const auto texts = [&] {
            std::string t;
            for (const auto& p : d.as<core::Story>("story.9")->paras) t += p.text + "|";
            return t;
        };
        check("numbers off: one step", core::doc::set_list_numbers(j, d, "list.1", false) && !d.as<core::GenList>("list.1")->numbers &&
                                       !core::doc::set_list_numbers(j, d, "list.1", false));
        check("numbers off: the list is out of date", stale() != "current");
        L::build_list(j, d, "list.1");
        check("numbers off: built -- the words alone, the title kept", texts() == "Contents|Chapter One|Chapter Two|");
        check("numbers off: current, and a rebuild changes nothing", stale() == "current" && !L::build_list(j, d, "list.1"));
        core::doc::set_list_numbers(j, d, "list.1", true);
        check("numbers on again: says so", stale() == "Page numbers not written");
        L::build_list(j, d, "list.1");
        check("numbers on again: built", texts() == "Contents|Chapter One\t1|Chapter Two\t3|");
        // Level 1 in a picked style.
        check("entry style: picked, one step", core::doc::set_list_entry_style(j, d, "list.1", 1, "style.3") &&
                                               d.as<core::GenList>("list.1")->entry_styles == std::vector<std::string>{"style.3"});
        check("entry style: a character style refused", !core::doc::set_list_entry_style(j, d, "list.1", 1, "style.5"));
        L::build_list(j, d, "list.1");
        const auto& ps = d.as<core::Story>("story.9")->paras;
        check("entry style: built in it, the title kept", ps.size() == 3 && ps[0].text == "Contents" && ps[1].style == "style.3" &&
                                                          ps[2].style == "style.3");
        check("entry style: back to Automatic -- the list forgets it", core::doc::set_list_entry_style(j, d, "list.1", 1, "") &&
                                                                       d.as<core::GenList>("list.1")->entry_styles.empty());
        // The title rule.
        core::Story st{"t", {{"style.9", "Contents"}, {"style.8", "A"}, {"style.8", "B"}}};
        const bool titled = core::doc::list_title_paras(st) == 1;
        core::Story same{"t", {{"style.8", "A"}, {"style.8", "B"}}};
        core::Story alone{"t", {{"style.9", "Contents"}}};
        core::Story tabbed{"t", {{"style.8", "A\t1"}, {"style.9", "B"}}};
        check("title: another style below -- a title; all alike -- none; alone -- a title; a tab -- none",
              titled && core::doc::list_title_paras(same) == 0 && core::doc::list_title_paras(alone) == 1 &&
              core::doc::list_title_paras(tabbed) == 0);
        std::string err;
        core::Doc f;
        core::doc::set_list_numbers(j, d, "list.1", false);
        check("json: page numbers off kept", core::from_json(f, core::to_json(d), &err) && !f.as<core::GenList>("list.1")->numbers, err);
    }
}
}  // namespace cxtest
