// composix_selftest, part 7 of 8 (s196: split from src/selftest.cpp).
#include "Selftest.hpp"

namespace cxtest {
void part7() {

    {
        std::cout << "\n-- note links in the PDF (s123) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "On page 1.");
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
        L::FlowCache flows;
        auto links = L::note_links(d, flows);
        const auto page_of_note = [&](int p) -> std::string {
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes)
                    if (np.para == p && !np.cont) return d.as<core::Frame>(ft.frame)->owner;
            return {};
        };
        check("two links a note: to it, and back", links.size() == 4, std::to_string(links.size()));
        bool ok = links.size() == 4;
        for (std::size_t i = 0; ok && i < links.size(); i += 2) {
            const auto& to = links[i];
            const auto& back = links[i + 1];
            const auto* pg = d.as<core::Page>(to.page);
            ok = ok && pg && to.box.w > 0 && to.box.h > 0 && to.box.x >= 0 && to.box.r() <= pg->w && to.box.y >= 0 && to.box.b() <= pg->h &&
                 back.page == to.to && back.to == to.page;
        }
        check("each box on its page; back goes where the mark is", ok);
        check("the first note's mark on page 1 links to its own page's foot", !links.empty() && links[0].to == page_of_note(1) &&
                                                                              links[0].page == links[0].to);
        // As endnotes: the first note's link crosses to where the endnotes are.
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        links = L::note_links(d, flows);
        check("endnotes: the mark on page 1 links to the endnote's page", links.size() == 4 && links[0].to == page_of_note(1) &&
                                                                          links[0].to != links[0].page);
        // Written: the screen PDF and the print PDF carry them (plus the contents' two).
        const auto base = fs::temp_directory_path() / "composix_s123";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto contents = static_cast<int>(L::contents_links(d, flows).size());
        const auto pout = (base / "print.pdf").string();
        check("print PDF: the note links written", L::export_print_pdf(d, pout, o, src, &rep, &err) &&
                                                   count(slurp(pout), "/Subtype /Link") == contents + 4, err);
        const auto sout = (base / "screen.pdf").string();
        check("screen PDF: the note links written", L::export_pdf(d, sout, o, src, &rep, &err) &&
                                                    count(slurp(sout), "/Subtype /Link") == contents + 4, err);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- numbers by chapter (s124) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "One.");     // Chapter One
        D::insert_note(j, d, "story.1", {2, 6}, "Two.");     // Chapter One
        const auto* st = d.as<core::Story>("story.1");       // the sample's own: Chapter Two (paragraph 5)
        check("through the story: 1 2 3", core::note_number(d, *st, 1, 0) == 1 && core::note_number(d, *st, 2, 0) == 2 &&
                                          core::note_number(d, *st, 5, 0) == 3);
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        st = d.as<core::Story>("story.1");
        check("by chapter: 1 2, then 1 again", core::note_number(d, *st, 1, 0) == 1 && core::note_number(d, *st, 2, 0) == 2 &&
                                               core::note_number(d, *st, 5, 0) == 1);
        {
            core::Doc e;
            std::string err;
            check("json: by chapter kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_restart == core::NoteRestart::Chapter, err);
        }
        L::FlowCache flows;
        const auto nums = [&] {
            std::vector<int> v;
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes) if (np.k >= 0 && !np.cont) v.push_back(np.number);
            return v;
        };
        check("at the foot: 1 2 / 1", nums() == std::vector<int>{1, 2, 1});
        check("the marks: as set", L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 1 && L::laid_note_number(flows.get(d, "story.1"), 2, 0) == 2);
        D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes");
        check("chapter endnotes: 1 2 / 1", nums() == std::vector<int>{1, 2, 1});
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        check("story endnotes, by chapter: still each chapter's own", nums() == std::vector<int>{1, 2, 1});
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        {   // An edit -- re-flow == whole.
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size();
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size();
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k) same = re.frames[i].notes[k].number == wf.frames[i].notes[k].number;
            }
            check("re-flow by chapter == whole", same);
        }
        {   // A new chapter start (Heading 1 starts a right page) before the second note: re-flow == whole, renumbered.
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::set_para_style(j, d, "story.1", {2, 0}, {2, 0}, "style.2");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size();
            std::vector<int> a, b;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                for (const auto& np : re.frames[i].notes) if (!np.cont) a.push_back(np.number);
                for (const auto& np : wf.frames[i].notes) if (!np.cont) b.push_back(np.number);
            }
            check("a chapter start added: re-flow == whole, the notes after it from 1", same && a == b && !b.empty() && std::all_of(b.begin(), b.end(), [](int n) { return n == 1; }),
                  std::to_string(b.size()));
        }
    }

    {
        std::cout << "\n-- chapter heads among endnotes (s125) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "One.");
        D::insert_note(j, d, "story.1", {2, 6}, "Two.");
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        L::FlowCache flows;
        const auto order = [&] {
            std::vector<std::pair<int, int>> v;   // (k, para)
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes) if (!np.cont) v.push_back({np.k, np.para});
            return v;
        };
        using V = std::vector<std::pair<int, int>>;
        check("story endnotes by chapter: the title, each chapter's head, its notes",
              order() == V{{-1, 1}, {-2, 0}, {0, 1}, {0, 2}, {-2, 4}, {0, 5}});
        {
            std::vector<std::string> heads;
            const auto& f = flows.get(d, "story.1");
            for (const auto& ft : f.frames) {
                std::vector<double> ys;
                for (const auto& np : ft.notes) if (np.k == -2) ys.push_back(np.y);
                if (ys.empty()) continue;
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -4) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    const std::string line(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                    if (line.rfind("Chapter", 0) == 0) heads.push_back(line);
                });
            }
            check("the heads read the chapters' first words", heads == std::vector<std::string>{"Chapter One", "Chapter Two"},
                  std::to_string(heads.size()));
        }
        D::set_note_numbering(j, d, core::NoteRestart::Story, core::NoteStyle::Arabic);
        check("through the story: no heads", order() == V{{-1, 1}, {0, 1}, {0, 2}, {0, 5}});
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes");
        bool none = true;
        for (const auto& kp : order()) none = none && kp.first != -2;
        check("chapter endnotes: no heads (each group follows its chapter)", none);
    }

    {
        std::cout << "\n-- the number in the note (s126) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // its note: Chapter Two, page 3
        using NN = core::Settings::NoteNum;
        check("in the note: one step, the same again none", D::set_note_num(j, d, NN::Period) && !D::set_note_num(j, d, NN::Period));
        {
            core::Doc e;
            std::string err;
            check("json: kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_num == NN::Period, err);
        }
        // The note's first line, as drawn.
        const auto first_line = [&](bool* raised) {
            const auto f = L::flow_story(d, "story.1");
            std::string out;
            *raised = false;
            for (const auto& ft : f.frames) {
                if (ft.notes.empty()) continue;
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -4 || !out.empty()) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    out.assign(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                    if (PangoAttrList* al = pango_layout_get_attributes(v.line->layout)) {
                        PangoAttrIterator* it = pango_attr_list_get_iterator(al);
                        do {
                            if (pango_attr_iterator_get(it, PANGO_ATTR_RISE)) *raised = true;
                        } while (pango_attr_iterator_next(it));
                        pango_attr_iterator_destroy(it);
                    }
                });
            }
            return out;
        };
        bool raised = false;
        check("\"1.\" before the words", first_line(&raised).rfind("1. The foldout", 0) == 0 && !raised);
        L::FlowCache flows;
        const auto page = d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner;
        const auto k1 = L::page_key(d, flows, page);
        D::set_note_num(j, d, NN::Paren);
        check("\"1)\" before the words", first_line(&raised).rfind("1) The foldout", 0) == 0 && !raised);
        check("the page's key moves with it (its picture redrawn)", !(L::page_key(d, flows, page) == k1));
        D::set_note_num(j, d, NN::Raised);
        check("raised: \"1\" raised and smaller", first_line(&raised).rfind("1 The foldout", 0) == 0 && raised);
        D::set_note_num(j, d, NN::Plain);
        check("plain: \"1\", as before", first_line(&raised).rfind("1 The foldout", 0) == 0 && !raised);
    }

    {
        std::cout << "\n-- cross-references (s128) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        // A reference in Chapter One to Chapter Two's heading (paragraph 4).
        check("a reference to a heading: one step", D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::Page) && j.size() == 1);
        check("the heading got an anchor at its start, the reference its target",
              P(4).anchors.size() == 1 && P(4).text.rfind(core::kMarkAnchor, 0) == 0 && P(1).refs.size() == 1 &&
                  P(1).refs[0].target == P(4).anchors[0] && P(1).text.substr(4, 3) == core::kMarkRef);
        check("find_anchor: where it is", [&] {
            const auto a = D::find_anchor(d, P(4).anchors[0]);
            return a.story == "story.1" && a.at.para == 4 && a.at.byte == 0;
        }());
        D::insert_ref_to(j, d, "story.1", {2, 0}, "story.1", 4, core::RefShow::Text);
        check("a second reference: the same anchor", P(4).anchors.size() == 1 && P(2).refs.size() == 1 && P(2).refs[0].target == P(4).anchors[0]);
        // Shown on the page: the page's number, the words, both; the anchor unseen.
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        const auto folio3 = d.folio(d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner);
        check("shows: the heading's page", shown(1).substr(4, folio3.size()) == folio3, shown(1).substr(0, 20));
        check("shows: the heading's words", shown(2).rfind("Chapter Two", 0) == 0, shown(2).substr(0, 20));
        check("the anchor: nothing to see", shown(4).rfind("Chapter Two", 0) == 0);
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::TextPage);
        check("shows: both", shown(3).rfind("“Chapter Two” on page " + folio3, 0) == 0, shown(3).substr(0, 30));
        // Editing carries them.
        D::edit_text(j, d, "story.1", {1, 2}, {1, 2}, "\n", "Typing", "");
        check("a split: the reference goes with its mark", P(1).refs.empty() && P(2).refs.size() == 1);
        const int m = static_cast<int>(P(2).text.find(core::kMarkRef));
        D::edit_text(j, d, "story.1", {2, m}, {2, m + 3}, "", "Delete", "");
        check("its mark deleted: the reference goes", P(2).refs.empty());
        D::edit_text(j, d, "story.1", {0, 0}, {0, 0}, std::string(core::kMarkRef) + core::kMarkAnchor, "Paste", "");
        check("pasted marks: a reference with no target, an anchor with no id",
              P(0).refs.size() == 1 && P(0).refs[0].target.empty() && P(0).anchors.size() == 1 && P(0).anchors[0].empty());
        check("no target: \"??\"", shown(0).rfind("??", 0) == 0);
        D::set_char_style(j, d, "story.1", {5, 0}, {5, 4}, "style.5");
        check("a character style over the anchor keeps it", P(5).anchors.size() == 1 && !P(5).anchors[0].empty());
        {
            core::Doc e;
            std::string err;
            check("json: refs and anchors kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // The heading goes: references to it show "??".
        D::edit_text(j, d, "story.1", {5, 0}, {5, static_cast<int>(P(5).text.size())}, "", "Delete", "");
        check("the target gone: \"??\"", shown(4).find("??") != std::string::npos, shown(4).substr(0, 30));
        // The page's key follows a reference's value.
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::Text);
        L::FlowCache flows;
        const auto page1 = d.as<core::Frame>(L::flow_story(d, "story.1").frames.front().frame)->owner;
        std::string page_with_ref;
        for (const auto& ft : L::flow_story(d, "story.1").frames)
            for (const auto& l : ft.lines)
                if (l.para == 1 && l.start <= 4 && l.end > 4) page_with_ref = d.as<core::Frame>(ft.frame)->owner;
        (void)page1;
        const auto k1 = L::page_key(d, flows, page_with_ref);
        D::edit_text(j, d, "story.1", {4, 3}, {4, 3}, "The ", "Typing", "");   // the heading's words change
        check("the referring page's key moves when the heading's words do", !(L::page_key(d, flows, page_with_ref) == k1));
    }

    {
        std::cout << "\n-- cross-reference words; to notes (s129) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // its note: Chapter Two (paragraph 5)
        check("patterns: page, words, both, note", core::ref_pattern({"", core::RefShow::Page, {}}) == "#" &&
                                                   core::ref_pattern({"", core::RefShow::Text, {}}) == "%" &&
                                                   core::ref_pattern({"", core::RefShow::Note, {}}) == "*" &&
                                                   core::ref_pattern({"", core::RefShow::Custom, "see p. #"}) == "see p. #");
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("a reference to a note: one step", D::insert_ref_to_note(j, d, "story.1", {1, 4}, "story.1", 5, 0, core::RefShow::Note) && j.size() == 1);
        const int m = D::note_mark_byte(*d.as<core::Story>("story.1"), 5, 0);
        check("the anchor right before the note's mark", m >= 3 && P(5).text.compare(static_cast<std::size_t>(m - 3), 3, core::kMarkAnchor) == 0 &&
                                                           P(1).refs.size() == 1 && P(1).refs[0].target == P(5).anchors[0]);
        check("shows the note's number", shown(1).substr(4, 1) == "1", shown(1).substr(0, 12));
        D::insert_ref_to_note(j, d, "story.1", {2, 0}, "story.1", 5, 0, core::RefShow::Custom, "see note *, page #");
        const auto folio3 = d.folio(d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner);
        check("words of its own: \"see note 1, page 3\"", P(5).anchors.size() == 1 && shown(2).rfind("see note 1, page " + folio3, 0) == 0,
              shown(2).substr(0, 24));
        D::insert_note(j, d, "story.1", {3, 0}, "Earlier.");   // a note before it: it becomes 2
        check("the note renumbered: the reference follows", shown(1).substr(4, 1) == "2" && shown(2).rfind("see note 2", 0) == 0);
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Custom, "“%” (p. #)");
        check("words of its own, a heading: “Chapter Two” (p. 3)", shown(3).rfind("“Chapter Two” (p. " + folio3 + ")", 0) == 0,
              shown(3).substr(0, 30));
        {
            core::Doc e;
            std::string err;
            check("json: the pattern kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // Preflight: all well -- nothing; the heading gone -- a warning.
        L::FlowCache flows;
        const auto xrefs = [&] {
            int n = 0;
            for (const auto& i : L::preflight_issues(d, flows, {})) n += i.kind == "xref";
            return n;
        };
        check("preflight: references that resolve -- nothing", xrefs() == 0);
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(P(4).text.size())}, "", "Delete", "");
        check("preflight: the target gone -- a warning", xrefs() == 1, std::to_string(xrefs()));
        j.undo();
        // A target in a story not placed: a warning.
        const auto ded = D::insert_anchor(j, d, "story.4", {0, 0});
        D::insert_ref(j, d, "story.1", {2, 0}, ded, core::RefShow::Page);
        D::delete_frames(j, d, {[&] {
            for (const auto& id : d.ids_of<core::Frame>()) if (d.as<core::Frame>(id)->story == "story.4") return id;
            return std::string{};
        }()});
        check("preflight: the target not on a page -- a warning", xrefs() == 1, std::to_string(xrefs()));
    }

    {
        std::cout << "\n-- editing a cross-reference (s130) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        D::insert_ref_to(j, d, "story.1", {2, 0}, "story.1", 0, core::RefShow::Page);
        D::insert_ref_to(j, d, "story.1", {2, 3}, "story.1", 4, core::RefShow::Text);   // after the first: k 1
        check("found: the mark at the cursor", D::ref_near(S(), {2, 3}).k == 1 && D::ref_near(S(), {2, 3}).byte == 3);
        check("found: the mark just before it", D::ref_near(S(), {2, 6}).k == 1 && D::ref_near(S(), {2, 0}).k == 0);
        check("none away from a mark", D::ref_near(S(), {2, 9}).k < 0 && D::ref_near(S(), {1, 0}).k < 0);
        const auto n0 = j.size();
        check("what it shows changed: one step", D::set_ref(j, d, "story.1", 2, 1, core::RefShow::Custom, "see %") && j.size() == n0 + 1);
        check("  its words: \"see Chapter Two\"", shown(2).find("see Chapter Two") != std::string::npos, shown(2).substr(0, 40));
        check("the same again: nothing", !D::set_ref(j, d, "story.1", 2, 1, core::RefShow::Custom, "see %") && j.size() == n0 + 1);
        check("the other reference untouched", P(2).refs[0].show == core::RefShow::Page && P(2).refs[0].target == P(0).anchors[0]);
        j.undo();
        check("undone: as it was", P(2).refs[1].show == core::RefShow::Text && P(2).refs[1].pattern.empty());
        // Pointed elsewhere: at a heading with no anchor yet -- made, one step.
        const auto anchors_before = P(3).anchors.size();
        check("pointed elsewhere: one step", D::retarget_ref(j, d, "story.1", 2, 0, "story.1", 3, -1, core::RefShow::Text) && j.size() == n0 + 1);
        check("  the anchor made there; the reference points at it", P(3).anchors.size() == anchors_before + 1 && P(2).refs[0].target == P(3).anchors[0]);
        check("  the anchor where it was kept", !P(0).anchors.empty());
        check("  its words now", shown(2).rfind("By noon", 0) == 0, shown(2).substr(0, 20));
        check("the same target and showing: nothing", !D::retarget_ref(j, d, "story.1", 2, 0, "story.1", 3, -1, core::RefShow::Text));
        // In the same paragraph, before the reference: the reference is still k 0.
        check("pointed at its own paragraph", D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 2, -1, core::RefShow::Page) &&
                                              P(2).refs.size() == 2 && P(2).refs[1].target == P(2).anchors[0]);
        check("pointed at the note", D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 5, 0, core::RefShow::Note) &&
                                     shown(2).find("1") != std::string::npos);
        check("a note that isn't there: nothing", !D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 5, 3, core::RefShow::Note));
        check("no reference there: nothing", !D::set_ref(j, d, "story.1", 2, 5, core::RefShow::Page) &&
                                             !D::retarget_ref(j, d, "story.1", 1, 0, "story.1", 4, -1, core::RefShow::Page));
        {   // The hover's words.
            L::FlowCache fc;
            const auto w = L::ref_where(d, P(2).refs[1], fc, "story.1");
            check("hover: \"points at note 1, page \u2026\"", w.find("points at note 1, page ") != std::string::npos && w.find("(Chapters)") == std::string::npos, w);
            check("hover: from another story -- its name", L::ref_where(d, P(2).refs[1], fc, "story.2").find("(Chapters)") != std::string::npos);
            check("hover: a heading's words", L::ref_where(d, P(2).refs[0], fc, "story.1").find("\u201cBy noon") != std::string::npos);
            check("hover: gone", L::ref_where(d, core::XRef{"anchor.99", core::RefShow::Page, {}}, fc).find("gone") != std::string::npos);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no references, no anchors", P(2).refs.empty() && P(0).anchors.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- cross-reference links in the PDF (s131) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("no references: no links", L::xref_links(d, flows).empty());
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        // On page 1 (Chapter One's paragraph 1), to Chapter Two's heading.
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::TextPage);
        const auto& flow = flows.get(d, "story.1");
        const auto page_of = [&](int p, int b) -> std::string {
            const auto c = L::caret_at(d, "story.1", flow, {p, b});
            return c.ok ? d.as<core::Frame>(c.frame)->owner : std::string{};
        };
        const auto from_page = page_of(1, 4), head_page = page_of(4, 0);
        auto links = L::xref_links(d, flows);
        bool ok = !links.empty();
        for (const auto& l : links) {
            const auto* pg = d.as<core::Page>(l.page);
            ok = ok && pg && l.page == from_page && l.to == head_page && l.box.w > 0 && l.box.h > 0 && l.box.x >= 0 &&
                 l.box.r() <= pg->w + 0.5 && l.box.y >= 0 && l.box.b() <= pg->h;
        }
        check("a reference: a box on its page, to the heading's page", ok && from_page != head_page, std::to_string(links.size()));
        {   // The box is as wide as the words shown ("Chapter Two" on page 3 is wider than a bare number).
            double w = 0;
            for (const auto& l : links) w += l.box.w;
            D::set_ref(j, d, "story.1", 1, 0, core::RefShow::Page);
            double w2 = 0;
            for (const auto& l : L::xref_links(d, flows)) w2 += l.box.w;
            check("the box: the words it shows (page alone: narrower)", w2 > 0 && w2 < w, std::to_string(w) + " / " + std::to_string(w2));
        }
        // To a note: where the note is set.
        D::insert_ref_to_note(j, d, "story.1", {2, 0}, "story.1", 5, 0, core::RefShow::Note);
        std::string note_page;
        double note_y = 0;
        for (const auto& ft : flows.get(d, "story.1").frames)
            for (const auto& np : ft.notes)
                if (np.para == 5 && !np.cont && note_page.empty()) {
                    note_page = d.as<core::Frame>(ft.frame)->owner;
                    note_y = d.as<core::Frame>(ft.frame)->y + np.y;
                }
        links = L::xref_links(d, flows);
        bool to_note = false;
        for (const auto& l : links) to_note = to_note || (l.to == note_page && std::abs(l.to_y - (note_y - 4)) < 0.5);
        check("to a note: where the note is set", to_note && !note_page.empty());
        // Gone: no link.
        const auto n = links.size();
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(P(4).text.size())}, "", "Delete", "");
        check("its place gone: its link gone", L::xref_links(d, flows).size() < n);
        j.undo();
        // Written: in both PDFs, and counted on the Export sheet.
        const auto base = fs::temp_directory_path() / "composix_s131";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto count = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const auto hay = ss.str();
            int k = 0;
            for (auto at = hay.find("/Subtype /Link"); at != std::string::npos; at = hay.find("/Subtype /Link", at + 1)) ++k;
            return k;
        };
        const int want = static_cast<int>(L::contents_links(d, flows).size() + L::note_links(d, flows).size() + L::xref_links(d, flows).size());
        const auto pout = (base / "print.pdf").string(), sout = (base / "screen.pdf").string();
        check("print PDF: the cross-reference links written", L::export_print_pdf(d, pout, o, src, &rep, &err) && count(pout) == want, err);
        check("screen PDF: the cross-reference links written", L::export_pdf(d, sout, o, src, &rep, &err) && count(sout) == want, err);
        const auto sum = L::export_summary(d, flows, o, false, "");
        check("an anchored heading keeps its contents link", L::contents_links(d, flows).size() == 2 &&
                                                              L::bookmarks(d, flows).size() == 2 &&
                                                              L::bookmarks(d, flows)[1].title == "Chapter Two");
        check("the Export sheet counts them", sum.xref_links == static_cast<int>(L::xref_links(d, flows).size()) &&
                                              sum.nav.find("cross-reference link") != std::string::npos, sum.nav);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- copy and paste: notes, references, anchors (s132) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        // Paragraph 1: "The" + a reference to Chapter Two, a note, an anchor + the rest.
        D::insert_ref_to(j, d, "story.1", {1, 3}, "story.1", 4, core::RefShow::Text);
        D::insert_note(j, d, "story.1", {1, 6}, "A note.");
        const auto aid = D::insert_anchor(j, d, "story.1", {1, 9});
        D::insert_ref(j, d, "story.1", {3, 0}, aid, core::RefShow::Page);   // something points at that anchor
        const core::TextPos a{1, 0}, b{1, 12};
        const auto text = core::text_between(S(), a, b);
        const auto c = D::carried_between(S(), a, b);
        check("carried: the note's words, the reference, the anchor", c.notes.size() == 1 && c.notes[0].text == "A note." &&
                                                                       c.refs.size() == 1 && c.refs[0].target == P(4).anchors[0] &&
                                                                       c.anchors == std::vector<std::string>{aid});
        check("carried: none outside", D::carried_between(S(), {0, 0}, {0, 5}) == D::Carried{});
        // Copy, paste into paragraph 2: the note's words, the reference's target; the anchor a new id.
        const auto n0 = j.size();
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste", "", &c);
        check("paste: one step", j.size() == n0 + 1);
        check("paste: the note keeps its words", P(2).notes.size() == 1 && P(2).notes[0].text == "A note.");
        check("paste: the reference points where it did", P(2).refs.size() == 1 && P(2).refs[0] == P(1).refs[0] &&
                                                         shown(2).find("Chapter Two") != std::string::npos);
        check("paste of a copy: the anchor a fresh id", P(2).anchors.size() == 1 && !P(2).anchors[0].empty() &&
                                                        P(2).anchors[0] != aid && P(1).anchors[0] == aid);
        check("  the reference to the first still finds it", D::find_anchor(d, aid).at.para == 1);
        // Without what it carried (another app's text): blanks, as before.
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste");
        check("a plain paste: an empty note, a reference to nothing, no id",
              P(2).notes.size() == 2 && P(2).notes[0].text.empty() && P(2).refs[0].target.empty() && P(2).anchors[0].empty());
        j.undo();
        j.undo();
        // Cut, then paste elsewhere: the anchor keeps its id; what pointed at it still does.
        D::edit_text(j, d, "story.1", a, b, "", "Cut");
        check("cut: the anchor gone", D::find_anchor(d, aid).story.empty());
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste", "", &c);
        check("paste after a cut: the anchor keeps its id", D::find_anchor(d, aid).at.para == 2);
        check("  the reference to it still resolves", shown(3).rfind("??", 0) != 0, shown(3).substr(0, 10));
        // Pasted twice: the second a fresh id.
        D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, text, "Paste", "", &c);
        int with_aid = 0;
        for (const auto& p : S().paras) with_aid += static_cast<int>(std::count(p.anchors.begin(), p.anchors.end(), aid));
        check("pasted twice: one place has the id", with_aid == 1);
        // In another story: the id taken there too.
        D::edit_text(j, d, "story.2", {0, 0}, {0, 0}, text, "Paste", "", &c);
        const auto& cap = d.as<core::Story>("story.2")->paras[0];
        check("into another story: a fresh id there", cap.anchors.size() == 1 && cap.anchors[0] != aid && cap.notes.size() == 1);
        while (j.can_undo()) j.undo();
        check("undo all: as loaded", P(1).anchors.empty() && P(2).notes.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- the cross-references, listed (s133) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("none: an empty list", L::xref_list(d, flows).empty());
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Text);           // paragraph 3
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 5, core::RefShow::Page);           // paragraph 1 -- listed first
        D::insert_ref_to_note(j, d, "story.1", {1, 0}, "story.1", 5, 0, core::RefShow::Note);   // before it in paragraph 1
        D::insert_ref(j, d, "story.2", {0, 0}, "anchor.99", core::RefShow::Page);              // the caption's: points nowhere
        auto rows = L::xref_list(d, flows);
        check("four, in story then reading order", rows.size() == 4 && rows[0].story == "story.1" && rows[0].para == 1 && rows[0].k == 0 &&
                                                   rows[1].para == 1 && rows[1].k == 1 && rows[2].para == 3 && rows[3].story == "story.2",
              std::to_string(rows.size()));
        if (rows.size() == 4) {
            check("its page's folio", !rows[0].page.empty() && d.as<core::Page>(rows[0].page_id) != nullptr);
            check("what it shows; the words round it, marks left out",
                  rows[2].shows == "Chapter Two" && rows[2].context.find("Chapter Two") != std::string::npos &&
                  rows[2].context.find("\xEE\x80") == std::string::npos, rows[2].context);
            check("where it points", rows[0].where.find("points at note 1") != std::string::npos, rows[0].where);
            check("broken: the one pointing nowhere only", !rows[0].broken && !rows[1].broken && !rows[2].broken && rows[3].broken);
        }
        // The heading deleted: its reference broken too.
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(d.as<core::Story>("story.1")->paras[4].text.size())}, "", "Delete", "");
        rows = L::xref_list(d, flows);
        int broken = 0;
        for (const auto& r : rows) broken += r.broken;
        check("its place deleted: broken", broken == 2, std::to_string(broken));
    }

    {
        std::cout << "\n-- index marks (s134) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        L::FlowCache flows;
        check("none: an empty index", L::index_entries(d, flows).empty());
        const auto n0 = j.size();
        const auto after = D::insert_index_mark(j, d, "story.1", {1, 4}, "  harbor  ", "smells");
        check("a mark: one step, after it", j.size() == n0 + 1 && after == core::TextPos{1, 7});
        check("  its term and sub-entry, trimmed", P(1).index.size() == 1 && P(1).index[0] == core::IndexMark{"harbor", "smells"} &&
                                                   P(1).text.compare(4, 3, core::kMarkIndex) == 0);
        check("an empty term: nothing", D::insert_index_mark(j, d, "story.1", {1, 0}, "   ") == core::TextPos{1, 0} && j.size() == n0 + 1);
        check("found at / just before the cursor", D::index_near(S(), {1, 4}).k == 0 && D::index_near(S(), {1, 7}).k == 0 &&
                                                   D::index_near(S(), {1, 8}).k < 0);
        {   // Nothing shows on the page: the words as they were.
            const auto r = L::resolve_story(d, "story.1", "");
            check("on the page: nothing to see", r.any && r.text.paras[1].text == "The harbor smelled of diesel and oranges the morning the ship came in.");
        }
        // More: the same term on Chapter Two's page, another case; a term of its own; one in the caption.
        D::insert_index_mark(j, d, "story.1", {5, 0}, "Harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {4, 0}, "Maps");
        D::insert_index_mark(j, d, "story.1", {3, 0}, "Guild");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");
        auto ix = L::index_entries(d, flows);
        const auto folio = [&](int p) {
            const auto c = L::caret_at(d, "story.1", flows.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        check("entries sorted, case folded: guild, harbor, harbor/smells, maps",
              ix.size() == 4 && ix[0].term == "Guild" && ix[1].term == "harbor" && ix[1].sub.empty() && ix[2].sub == "smells" &&
                  ix[3].term == "Maps", std::to_string(ix.size()));
        if (ix.size() == 4) {
            check("one entry for both cases; its pages in order, each once",
                  ix[2].marks == 2 && ix[2].folios == std::vector<std::string>{folio(1), folio(5)} && folio(1) != folio(5));
            check("the term's own line: its own marks' pages", ix[1].marks == 1 && ix[1].folios == std::vector<std::string>{folio(2)});
        }
        {   // A sub-entry alone still heads under its term.
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            D::insert_index_mark(je, e, "story.1", {1, 0}, "Ships", "cargo");
            L::FlowCache fe;
            const auto ex = L::index_entries(e, fe);
            check("a sub-entry alone: its term heads it, with no pages", ex.size() == 2 && ex[0].term == "Ships" && ex[0].sub.empty() &&
                                                                          ex[0].pages.empty() && ex[0].marks == 0 && ex[1].sub == "cargo");
        }
        // Changed: one step; the same again nothing.
        check("changed: one step", D::set_index_mark(j, d, "story.1", 4, 0, "Charts", "folding"));
        check("the same again: nothing", !D::set_index_mark(j, d, "story.1", 4, 0, "Charts", "folding"));
        check("an empty term: nothing", !D::set_index_mark(j, d, "story.1", 4, 0, " "));
        // Kept through a character style and through copy / paste; saved.
        D::set_char_style(j, d, "story.1", {1, 0}, {1, 20}, "style.5");
        check("a character style keeps it", P(1).index.size() == 1 && P(1).index[0].term == "harbor");
        const auto c = D::carried_between(S(), {1, 0}, {1, 10});
        D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, core::text_between(S(), {1, 0}, {1, 10}), "Paste", "", &c);
        check("pasted: the mark keeps its term", P(3).index.size() == 2 && P(3).index[0] == core::IndexMark{"harbor", "smells"});
        {
            core::Doc e;
            std::string err;
            check("json: kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no marks", P(1).index.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- the index, built (s135) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto last = d.pages().back();
        check("a list on no page that isn't one: nothing", D::add_list(j, d, core::ListKind::Index, "Index", "frame.1").empty());
        const auto n0 = j.size();
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", last);
        check("a new index: one step, never built", !ix.empty() && j.size() == n0 + 1 && d.as<core::GenList>(ix)->built_entries < 0 &&
                                                    d.as<core::GenList>(ix)->kind == core::ListKind::Index);
        L::FlowCache fl;
        const auto stale_of = [&](const std::string& id) {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == id) return x.why;
            return std::string{};
        };
        check("not built yet: stale", stale_of(ix) == "Not built yet");
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {5, 0}, "Harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");
        D::insert_index_mark(j, d, "story.1", {3, 0}, "guild");
        D::insert_index_mark(j, d, "story.1", {4, 0}, "Maps");
        const auto en = L::list_entries(d, fl, ix);
        const auto f = [&](int p) {
            const auto c = L::caret_at(d, "story.1", fl.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        check("entries: a letter heading each initial; term, its sub-entries",
              en.size() == 7 && en[0].text == "G" && en[0].level == 0 && en[1].text == "guild" && en[2].text == "H" &&
                  en[3].text == "harbor" && en[3].level == 1 && en[4].text == "smells" && en[4].level == 2 &&
                  en[4].folio == f(1) + ", " + f(5) && en[5].text == "M" && en[6].text == "Maps",
              std::to_string(en.size()));
        check("built: one step", L::build_list(j, d, ix));
        const auto* l = d.as<core::GenList>(ix);
        const auto* st = d.as<core::Story>(l->story);
        check("written into a new story on its page", st && st->paras.size() == 7 && l->built_entries == 7);
        if (st && st->paras.size() == 7) {
            check("  \"harbor, n\" -- a comma, not a tab", st->paras[3].text == "harbor, " + f(2) && st->paras[4].text == "smells, " + f(1) + ", " + f(5));
            const auto* hs = d.as<core::Style>(st->paras[0].style);
            const auto* s1 = d.as<core::Style>(st->paras[1].style);
            const auto* s2 = d.as<core::Style>(st->paras[4].style);
            check("  letters in a \"Letter\" style, bold; sub-entries indented",
                  hs && s1 && s2 && hs->name == s1->name + " Letter" && hs->bold && s2->left_indent > s1->left_indent);
        }
        check("built: current", stale_of(ix).empty(), stale_of(ix));
        check("again: nothing", !L::build_list(j, d, ix));
        D::insert_index_mark(j, d, "story.1", {0, 0}, "anchors");
        check("a new mark: stale, saying which", stale_of(ix) == "\u201cA\u201d not listed yet", stale_of(ix));
        L::build_list(j, d, ix);
        D::edit_text(j, d, "story.1", {2, 0}, {2, 3}, "", "Delete", "");   // the harbor mark at {2,0} gone
        check("a mark gone: stale", !stale_of(ix).empty(), stale_of(ix));
        {   // A title written above the entries: kept by a rebuild.
            core::Story t = *d.as<core::Story>(d.as<core::GenList>(ix)->story);
            t.paras.insert(t.paras.begin(), core::Para{t.paras[1].style, "Index"});
            j.run("title", {d.as<core::GenList>(ix)->story}, [&] { d.put(d.as<core::GenList>(ix)->story, t); });
            L::build_list(j, d, ix);
            const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
            check("a title above: kept; the letters after it", w.size() > 2 && w[0].text == "Index" && w[1].text == "A");
        }
        {   // The inspector: Build offered; marks counted.
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            bool build = false, marks = false;
            for (const auto& it : o.items())
                if (it.id == ix)
                    for (const auto& fd : it.fields) {
                        build = build || (fd.edit == "genlist" && !fd.choices.empty());
                        marks = marks || (fd.label == "Marks" && fd.value == "5");
                    }
            check("the inspector: Build offered, the marks counted", build && marks);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no index", !d.as<core::GenList>(ix) && d.strays() == 0);
    }

    {
        std::cout << "\n-- the index: ranges, see, sorting (s136) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A frame with a story on pages 4, 5, 6 and 8 (numbered so): "x" with marks.
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        std::map<std::string, std::string> story_on;
        for (const char* f : {"4", "5", "6", "8"}) {
            const auto pg = page_numbered(f);
            const auto fr = D::add_frame(j, d, pg, core::FrameKind::Text, {72, 72, 200, 100});
            story_on[f] = d.as<core::Frame>(fr)->story;
            D::edit_text(j, d, story_on[f], {0, 0}, {0, 0}, "x");
        }
        for (const char* f : {"4", "5", "6", "8"}) D::insert_index_mark(j, d, story_on[f], {0, 1}, "Ships");
        for (const char* f : {"4", "5"}) D::insert_index_mark(j, d, story_on[f], {0, 1}, "Sails");
        L::FlowCache fl;
        const auto find = [&](const std::string& term, const std::string& sub = {}) {
            for (const auto& e : L::index_entries(d, fl)) if (e.term == term && e.sub == sub) return e;
            return L::IndexEntry{};
        };
        check("three pages in a row and more: \"4\u20136, 8\"", find("Ships").pages_text == "4\u20136, 8", find("Ships").pages_text);
        check("two in a row stay apart: \"4, 5\"", find("Sails").pages_text == "4, 5", find("Sails").pages_text);
        // See and see also.
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Charts", "", "Maps"});
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Ships", "", "Boats", true});
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Ships", "", "Anchors", true});
        check("a see: no page of its own", find("Charts").pages.empty() && find("Charts").see == std::vector<std::string>{"Maps"});
        check("see also: sorted, the pages kept", find("Ships").also == (std::vector<std::string>{"Anchors", "Boats"}) &&
                                                  find("Ships").pages.size() == 4);
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        const auto en = L::list_entries(d, fl, ix);
        std::map<std::string, std::string> line;
        for (const auto& e : en) line[e.text] = e.folio;
        check("written: \"Charts. See Maps\"", line.count("Charts. See Maps") && line["Charts. See Maps"].empty());
        check("written: \"Ships, 4\u20136, 8. See also Anchors; Boats\"", line["Ships"] == "4\u20136, 8. See also Anchors; Boats", line["Ships"]);
        L::build_list(j, d, ix);
        {
            const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
            bool ok = false;
            for (const auto& p : w) ok = ok || p.text == "Ships, 4\u20136, 8. See also Anchors; Boats";
            check("built: the line as written", ok);
            L::FlowCache f2;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f2)) stale = stale || x.list == ix;
            check("built: current", !stale);
        }
        // Sorting: an accented initial with its letter, after the plain word.
        D::insert_index_mark(j, d, "story.1", {2, 0}, "\u00c9clair");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "eagle");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "echo");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "eclair");
        std::vector<std::string> order;
        for (const auto& e : L::index_entries(d, fl)) if (e.sub.empty()) order.push_back(e.term);
        const auto at = [&](const std::string& t) { return std::find(order.begin(), order.end(), t) - order.begin(); };
        std::string ord;
        for (const auto& o : order) ord += o + " ";
        check("\u00c9clair with the e's: just after eclair, before Sails (not after z)",
              at("eagle") < at("echo") && at("echo") < at("eclair") && at("eclair") + 1 == at("\u00c9clair") && at("\u00c9clair") < at("Sails"), ord);
        bool e_head = false, accented_head = false;
        for (const auto& e : L::list_entries(d, fl, ix)) {
            e_head = e_head || (e.level == 0 && e.text == "E");
            accented_head = accented_head || (e.level == 0 && e.text == "\u00c9");
        }
        check("one letter heading E for both", e_head && !accented_head);
        // Kept: the see in the file; changed keeps it unless changed.
        {
            core::Doc e;
            std::string err;
            check("json: see and also kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        int k0 = 0;
        while (d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].term != "Charts") ++k0;
        check("renamed by term: its see kept", D::set_index_mark(j, d, "story.1", 1, k0, "Charts and plans") &&
                                               d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].see == "Maps");
        check("a see emptied: also off", D::set_index_mark(j, d, "story.1", 1, k0, core::IndexMark{"Charts", "", " ", true}) &&
                                         !d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].also);
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].index.empty());
    }

    {
        std::cout << "\n-- index links in the PDF (s137) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        std::map<std::string, std::string> story_on;
        for (const char* f : {"4", "5", "6", "8"}) {
            const auto fr = D::add_frame(j, d, page_numbered(f), core::FrameKind::Text, {72, 144, 200, 100});
            story_on[f] = d.as<core::Frame>(fr)->story;
            D::edit_text(j, d, story_on[f], {0, 0}, {0, 0}, "x");
            D::insert_index_mark(j, d, story_on[f], {0, 1}, "Ships");
        }
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor");
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Charts", "", "Maps"});
        L::FlowCache fl;
        check("no index built: no links", L::index_links(d, fl).empty());
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        L::build_list(j, d, ix);
        auto links = L::index_links(d, fl);
        // Ships: "4\u20136" and "8" -- two links; harbor: one; Charts (a see): none.
        int ships = 0, to4 = 0, to8 = 0, harbor = 0;
        for (const auto& l : links) {
            if (l.to == page_numbered("4")) { ++to4; ++ships; }
            if (l.to == page_numbered("8")) { ++to8; ++ships; }
            if (l.to == d.as<core::Frame>(L::flow_story(d, "story.1").frames.front().frame)->owner) ++harbor;
        }
        check("three links: a range to its first page, a number, harbor's", links.size() == 3 && to4 == 1 && to8 == 1 && harbor == 1,
              std::to_string(links.size()));
        bool ok = true;
        for (const auto& l : links) {
            const auto* pg = d.as<core::Page>(l.page);
            ok = ok && l.page == d.pages().back() && pg && l.box.w > 0 && l.box.h > 0 && l.box.r() <= pg->w + 0.5 && l.box.b() <= pg->h;
        }
        check("each box on the index's page", ok);
        {   // The range's box is wider than the single number's.
            double wr = 0, w8 = 0, y4 = -1;
            for (const auto& l : links) {
                if (l.to == page_numbered("4")) { wr = l.box.w; y4 = l.to_y; }
                if (l.to == page_numbered("8")) w8 = l.box.w;
            }
            check("the range's box: the whole \"4\u20136\"", wr > w8 * 1.5, std::to_string(wr) + " / " + std::to_string(w8));
            check("to the mark: down its page where the frame is", y4 > 100 && y4 < 200, std::to_string(y4));
        }
        // Stale: a mark gone -- that line no longer reads right, its link goes; the rest stay.
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");      // the same page: the line still reads "harbor, 1"
        D::insert_index_mark(j, d, story_on["8"], {0, 1}, "harbor");  // page 8: now "harbor, 1, 8" -- stale
        links = L::index_links(d, fl);
        check("stale: the line that changed has no link; the others keep theirs", links.size() == 2, std::to_string(links.size()));
        L::build_list(j, d, ix);
        check("rebuilt: harbor's two numbers linked", L::index_links(d, fl).size() == 4);
        // Written: both PDFs; the Export sheet counts them.
        const auto base = fs::temp_directory_path() / "composix_s137";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto count = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const auto hay = ss.str();
            int k = 0;
            for (auto at = hay.find("/Subtype /Link"); at != std::string::npos; at = hay.find("/Subtype /Link", at + 1)) ++k;
            return k;
        };
        const int want = static_cast<int>(L::contents_links(d, fl).size() + L::note_links(d, fl).size() + L::xref_links(d, fl).size() +
                                          L::index_links(d, fl).size());
        const auto pout = (base / "print.pdf").string(), sout = (base / "screen.pdf").string();
        check("print PDF: the index links written", L::export_print_pdf(d, pout, o, src, &rep, &err) && count(pout) == want, err);
        check("screen PDF: the index links written", L::export_pdf(d, sout, o, src, &rep, &err) && count(sout) == want, err);
        const auto sum = L::export_summary(d, fl, o, false, "");
        check("the Export sheet counts them", sum.index_links == 4 && sum.nav.find("4 index links") != std::string::npos, sum.nav);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- the index's styles (s138) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "guild");
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        L::build_list(j, d, ix);
        const auto rows = [&] {
            std::map<std::string, std::string> m;
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);   // (kept: items() is a reference into it)
            for (const auto& it : o.items())
                if (it.id == ix) for (const auto& f : it.fields) m[f.label] = f.value;
            return m;
        };
        auto r = rows();
        std::string seen;
        for (const auto& [k, v] : r) seen += k + "=" + v + "; ";
        check("the inspector: entries, sub-entries (the one Build made), page numbers; built current", r["Entries in"] == "Automatic" && r["Sub-entries in"] == "Body 2" &&
                                                                  r["Page numbers"] == "Yes" && r["Built"] == "Current", seen);
        const auto stale_of = [&] {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == ix) return x.why;
            return std::string{};
        };
        check("built: current", stale_of().empty(), stale_of());
        D::set_list_entry_style(j, d, ix, 1, "style.4");   // the Sources' style for entries
        check("a style picked: out of date, saying which", stale_of() == "Entries not in " + d.as<core::Style>("style.4")->name, stale_of());
        L::build_list(j, d, ix);
        const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
        bool in4 = false, letter_from4 = false;
        for (const auto& p : w) {
            in4 = in4 || (p.text.rfind("guild", 0) == 0 && p.style == "style.4");
            const auto* st = d.as<core::Style>(p.style);
            letter_from4 = letter_from4 || (p.text == "G" && st && st->based_on == "style.4");
        }
        check("rebuilt: the entries in it, the letters made from it", in4 && letter_from4 && stale_of().empty(), stale_of());
        D::set_list_numbers(j, d, ix, false);
        L::build_list(j, d, ix);
        bool bare = false;
        for (const auto& p : d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras) bare = bare || p.text == "guild";
        check("page numbers off: the words alone", bare && stale_of().empty());
        L::FlowCache f;
        check("page numbers off: no index links", L::index_links(d, f).empty());
    }

    {
        std::cout << "\n-- index marks that cover a stretch (s139) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& st = *d.as<core::Story>("story.1");   // 0 Chapter One, 1-3 body, 4 Chapter Two, 5 map
        check("a heading: a Contents list's style", D::heading_para(d, st.paras[4]) && !D::heading_para(d, st.paras[2]));
        check("span ends: point, paragraph, to heading, 2 paragraphs, story end",
              D::index_span_end(d, st, 1, {"x"}) == 1 && D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraph}) == 1 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::NextHeading}) == 3 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraphs, 2}) == 2 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraphs, 99}) == 5 &&
                  D::index_span_end(d, st, 5, {"x", "", "", false, core::IndexSpan::NextHeading}) == 5 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::StoryEnd}) == 5);
        // Two frames threaded on pages 4 and 5, the text long enough to run over both.
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        const auto f4 = D::add_frame(j, d, page_numbered("4"), core::FrameKind::Text, {72, 72, 200, 80});
        const auto f5 = D::add_frame(j, d, page_numbered("5"), core::FrameKind::Text, {72, 72, 200, 300});
        const auto f6 = D::add_frame(j, d, page_numbered("6"), core::FrameKind::Text, {72, 72, 200, 300});
        D::thread_frames(j, d, f4, f5);
        const auto run = d.as<core::Frame>(f4)->story;
        std::string body;
        for (int k = 0; k < 40; ++k) body += "Words enough to run on past the first frame and into the next one. ";
        D::edit_text(j, d, run, {0, 0}, {0, 0}, "Harbors\n" + body);
        D::edit_text(j, d, d.as<core::Frame>(f6)->story, {0, 0}, {0, 0}, "Six");
        L::FlowCache fl;
        const auto pages_of = [&](const std::string& term) {
            for (const auto& e : L::index_entries(d, fl)) if (e.term == term) return e.pages_text;
            return std::string{"(none)"};
        };
        D::insert_index_mark(j, d, run, {1, 0}, core::IndexMark{"Ports"});
        check("a point: its page only", pages_of("Ports") == "4", pages_of("Ports"));
        D::insert_index_mark(j, d, run, {1, 0}, core::IndexMark{"Harbors", "", "", false, core::IndexSpan::Paragraph});
        check("its paragraph over two pages in a row: \"4\u20135\"", pages_of("Harbors") == "4\u20135", pages_of("Harbors"));
        D::insert_index_mark(j, d, d.as<core::Frame>(f6)->story, {0, 0}, "Ports");
        D::insert_index_mark(j, d, run, {0, 0}, core::IndexMark{"Ports", "", "", false, core::IndexSpan::Point});
        check("points on 4 and 6: \"4, 6\"", pages_of("Ports") == "4, 6", pages_of("Ports"));
        {   // In the sample's own story: to the story end -- pages 1 and 3, not in a row.
            D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Voyage", "", "", false, core::IndexSpan::StoryEnd});
            std::set<std::string> want;
            for (const auto& ft : L::flow_story(d, "story.1").frames)
                if (!ft.lines.empty()) want.insert(d.folio(d.as<core::Frame>(ft.frame)->owner));
            check("to the story's end: every page its text is on", pages_of("Voyage").find(*want.begin()) != std::string::npos &&
                                                                   pages_of("Voyage").find(", ") != std::string::npos, pages_of("Voyage"));
        }
        check("a See covers nothing (its span dropped)",
              D::insert_index_mark(j, d, "story.1", {2, 0}, core::IndexMark{"Charts", "", "Maps", false, core::IndexSpan::StoryEnd}) == core::TextPos{2, 3} &&
                  d.as<core::Story>("story.1")->paras[2].index[0].span == core::IndexSpan::Point);
        D::insert_index_mark(j, d, "story.1", {3, 0}, core::IndexMark{"Guild", "", "", false, core::IndexSpan::Paragraphs, 2});
        {
            core::Doc e;
            std::string err;
            check("json: span and count kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].index.empty());
    }

    {
        std::cout << "\n-- the bibliography, built (s140) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // list.2: Bibliography from style.4 (the Sources story), on the last page
        L::FlowCache fl;
        const auto stale_of = [&] {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.2") return x.why;
            return std::string{};
        };
        check("never written: not out of date", stale_of().empty());
        // A source elsewhere too, one repeated, and one with "The".
        D::edit_text(j, d, "story.4", {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())},
                     {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())},
                     "\nThe Book of Tides.\nAlmanac of the Outer Ports, 3rd ed.\nBaltic Pilot.");
        auto en = L::list_entries(d, fl, "list.2");
        std::vector<std::string> w;
        for (const auto& e : en) w.push_back(e.text);
        check("the sources: sorted (\"The\" set aside), each once, no page numbers",
              w == (std::vector<std::string>{"Almanac of the Outer Ports, 3rd ed.", "Baltic Pilot.", "The Book of Tides.", "Harbor Registers, vols. II\u2013IV.",
                                             "Letters of the Surveyor General."}) &&
                  std::all_of(en.begin(), en.end(), [](const auto& e) { return e.folio.empty(); }),
              [&] { std::string x; for (const auto& t : w) x += t + " | "; return x; }());
        const auto frames_before = d.frames_on(d.as<core::GenList>("list.2")->placed_on).size();
        check("built: one step", L::build_list(j, d, "list.2"));
        const auto* l = d.as<core::GenList>("list.2");
        const auto* st = d.as<core::Story>(l->story);
        check("the Sources typed on its page: that story is it -- sorted in place, no frame on top",
              l->story == "story.4" && d.frames_on(l->placed_on).size() == frames_before);
        check("  sorted, each once, the words alone", st && st->paras.size() == 5 && st->paras[0].text == "Almanac of the Outer Ports, 3rd ed." &&
                                                     st->paras[2].text == "The Book of Tides.");
        check("  its italics kept (the Almanac's title in Emphasis)", st && !st->paras.empty() && !st->paras[0].runs.empty() &&
                                                                       st->paras[0].runs[0].style == "style.5");
        check("  in the sources' own style", st && std::all_of(st->paras.begin(), st->paras.end(), [](const core::Para& p) { return p.style == "style.4"; }));
        check("built: current", stale_of().empty(), stale_of());
        check("again: nothing", !L::build_list(j, d, "list.2"));
        D::edit_text(j, d, "story.4", {0, 0}, {0, 0}, "Zed's Charts.\n");
        check("one typed at the top: out of date", !stale_of().empty(), stale_of());
        L::build_list(j, d, "list.2");
        check("rebuilt: sorted to its place", d.as<core::Story>("story.4")->paras.back().text == "Zed's Charts." && stale_of().empty());
        {   // The inspector offers Build for it.
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            bool build = false;
            for (const auto& it : o.items())
                if (it.id == "list.2") for (const auto& f : it.fields) build = build || (f.edit == "genlist" && !f.choices.empty());
            check("the inspector: Rebuild offered", build);
        }
        {   // One on a page without its sources: a frame of its own, gathering them from where they are.
            const auto other = d.pages()[d.pages().size() - 2];
            const auto bl = D::add_list(j, d, core::ListKind::Bibliography, "Works Cited", other);
            {
                core::GenList g = *d.as<core::GenList>(bl);
                g.from_styles = {"style.4"};
                j.run("styles", {bl}, [&] { d.put(bl, g); });
            }
            check("elsewhere: built into a frame of its own", L::build_list(j, d, bl) && !d.as<core::GenList>(bl)->story.empty() &&
                                                            d.as<core::GenList>(bl)->story != "story.4");
            const auto& w2 = d.as<core::Story>(d.as<core::GenList>(bl)->story)->paras;
            check("  the same sources, each once (its own copies not doubled)", w2.size() == 6 && w2[0].text == "Almanac of the Outer Ports, 3rd ed.");
            check("  built again: nothing", !L::build_list(j, d, bl));
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::GenList>("list.2")->story.empty());
    }

    {
        std::cout << "\n-- citations (s141) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // story.4: Almanac, Harbor Registers, Letters -- Sources (list.2 gathers style.4)
        const auto& src = *d.as<core::Story>("story.4");
        check("sources numbered in their story", core::source_number(d, src, 0) == 1 && core::source_number(d, src, 2) == 3 &&
                                                 core::source_number(d, *d.as<core::Story>("story.1"), 1) == 0);
        check("a citation of what isn't a source: nothing", !D::insert_citation(j, d, "story.1", {1, 0}, "story.1", 2) && j.size() == 0);
        check("a citation: one step", D::insert_citation(j, d, "story.1", {1, 3}, "story.4", 2) && j.size() == 1);
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("  shows [3]", shown(1).rfind("The[3]", 0) == 0, shown(1).substr(0, 12));
        // A source typed at the top, the bibliography rebuilt: sorted, the cited one is 4th now.
        D::edit_text(j, d, "story.4", {0, 0}, {0, 0}, "Baltic Pilot.\n");
        L::build_list(j, d, "list.2");
        check("sorted again: the citation follows its source -- [4]", shown(1).rfind("The[4]", 0) == 0 &&
                                                                      d.as<core::Story>("story.4")->paras[3].text.find("Letters of the Surveyor General.") != std::string::npos,
              shown(1).substr(0, 12));
        check("  its anchor kept by the rebuild", !d.as<core::Story>("story.4")->paras[3].anchors.empty());
        {
            L::FlowCache f;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f)) stale = stale || x.list == "list.2";
            check("  a cited source (its anchor) doesn't make it out of date", !stale);
        }
        {   // A bibliography elsewhere: copies come without the anchor (one place, one id).
            const auto bl = D::add_list(j, d, core::ListKind::Bibliography, "Works Cited", d.pages()[d.pages().size() - 2]);
            core::GenList g = *d.as<core::GenList>(bl);
            g.from_styles = {"style.4"};
            j.run("styles", {bl}, [&] { d.put(bl, g); });
            L::build_list(j, d, bl);
            int with = 0;
            for (const auto& p : d.as<core::Story>(d.as<core::GenList>(bl)->story)->paras)
                for (const auto& a : p.anchors) with += !a.empty();
            check("a copy elsewhere: no anchor ids copied", with == 0);
        }
        // The source deleted: the citation broken, Preflight says so.
        D::edit_text(j, d, "story.4", {3, 0}, {3, static_cast<int>(d.as<core::Story>("story.4")->paras[3].text.size())}, "", "Delete", "");
        D::edit_text(j, d, "story.4", {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())}, {3, 0}, "", "Delete", "");
        L::FlowCache fl;
        int xr = 0;
        for (const auto& i : L::preflight_issues(d, fl, {})) xr += i.kind == "xref";
        check("its source gone: ??, and a Preflight warning", shown(1).rfind("The??", 0) == 0 && xr == 1, shown(1).substr(0, 12));
        {
            core::Doc e;
            std::string err;
            check("json: a citation kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].refs.empty());
    }

    {
        std::cout << "\n-- the bibliography in cited order (s142) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // Sources: Almanac (1), Harbor Registers (2), Letters (3)
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        // Letters cited first (paragraph 1), then Almanac (paragraph 2); Harbor never.
        D::insert_citation(j, d, "story.1", {1, 0}, "story.4", 2);
        D::insert_citation(j, d, "story.1", {2, 0}, "story.4", 0);
        check("alphabetical: [3] then [1]", shown(1).rfind("[3]", 0) == 0 && shown(2).rfind("[1]", 0) == 0, shown(1).substr(0, 5) + shown(2).substr(0, 5));
        const auto rows = [&] {
            std::map<std::string, std::string> m;
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            for (const auto& it : o.items()) if (it.id == "list.2") for (const auto& f : it.fields) m[f.label] = f.value;
            return m;
        };
        check("the inspector: Order Alphabetical", rows()["Order"] == "Alphabetical");
        L::build_list(j, d, "list.2");   // written alphabetical first
        check("first cited: one step", D::set_list_cite_order(j, d, "list.2", true) && rows()["Order"] == "First cited");
        {
            L::FlowCache f;
            std::string why;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.2") why = x.why;
            check("  out of date, saying why: the order", why == "Not in the order first cited", why);
        }
        check("  the same again: nothing", !D::set_list_cite_order(j, d, "list.2", true));
        L::build_list(j, d, "list.2");
        const auto& w = d.as<core::Story>("story.4")->paras;
        const auto has = [](const core::Para& p, const char* t) { return p.text.find(t) != std::string::npos; };
        check("rebuilt: Letters, Almanac, then the uncited Harbor Registers",
              w.size() == 3 && has(w[0], "Letters") && has(w[1], "Almanac") && has(w[2], "Harbor"));
        check("  the citations renumbered: [1] then [2]", shown(1).rfind("[1]", 0) == 0 && shown(2).rfind("[2]", 0) == 0,
              shown(1).substr(0, 5) + shown(2).substr(0, 5));
        {
            L::FlowCache f;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f)) stale = stale || x.list == "list.2";
            check("  current", !stale);
        }
        // A citation of Harbor added before the others: it moves to the front on a rebuild.
        D::insert_citation(j, d, "story.1", {0, 0}, "story.4", 2);
        L::build_list(j, d, "list.2");
        check("a new first citation: that source first", has(d.as<core::Story>("story.4")->paras[0], "Harbor") && shown(0).rfind("[1]", 0) == 0,
              shown(0).substr(0, 5));
        {
            core::Doc e;
            std::string err;
            check("json: the order kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && !d.as<core::GenList>("list.2")->cite_order);
    }

    {
        std::cout << "\n-- citations: locators, several sources (s143) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // Sources 1-3 in story.4
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("a locator: \"[2, p. 14]\" (no-break spaces)", D::insert_citation(j, d, "story.1", {1, 0}, {{"story.4", 1}}, " p. 14 ") &&
                                                         shown(1).rfind("[2,\u00a0p.\u00a014]", 0) == 0,
              shown(1).substr(0, 12));
        const auto n0 = j.size();
        check("several, out of order: \"[1, 3]\" -- one step", D::insert_citation(j, d, "story.1", {2, 0}, {{"story.4", 2}, {"story.4", 0}}) &&
                                                                   j.size() == n0 + 1 && shown(2).rfind("[1, 3]", 0) == 0, shown(2).substr(0, 10));
        check("none a source: nothing", !D::insert_citation(j, d, "story.1", {3, 0}, {{"story.1", 2}}) && j.size() == n0 + 1);
        check("one added to it: \"[1\u20133]\"", D::add_cite_source(j, d, "story.1", 2, 0, "story.4", 1) && shown(2).rfind("[1\u20133]", 0) == 0,
              shown(2).substr(0, 10));
        check("  the same again: nothing", !D::add_cite_source(j, d, "story.1", 2, 0, "story.4", 1));
        check("  to what isn't a citation: nothing", [&] {
            D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Page);
            return !D::add_cite_source(j, d, "story.1", 3, 0, "story.4", 1);
        }());
        check("the locator edited (Apply), its sources kept", D::set_ref(j, d, "story.1", 2, 0, core::RefShow::Cite, "ch. 3") &&
                                                              shown(2).rfind("[1\u20133,\u00a0ch.\u00a03]", 0) == 0, shown(2).substr(0, 16));
        {
            core::Doc e;
            std::string err;
            check("json: locator and sources kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // One of its sources gone: "??" with the rest, and a Preflight warning.
        D::edit_text(j, d, "story.4", {1, 0}, {2, 0}, "", "Delete", "");   // Harbor Registers (2) gone; Letters is 2 now
        L::FlowCache fl;
        int xr = 0;
        for (const auto& i : L::preflight_issues(d, fl, {})) xr += i.kind == "xref";
        check("a source gone: the rest renumbered, ?? for it, a warning", shown(2).rfind("[1, 2, ??,\u00a0ch.\u00a03]", 0) == 0 && xr >= 2,
              shown(2).substr(0, 20) + " / " + std::to_string(xr));
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[2].refs.empty());
    }

    {
        std::cout << "\n-- the sources, listed (s144) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache fl;
        auto rows = L::source_list(d, fl);
        check("the sample's three sources, numbered, none cited", rows.size() == 3 && rows[0].number == 1 && rows[2].number == 3 &&
                                                                   rows[0].words.rfind("Almanac", 0) == 0 &&
                                                                   std::all_of(rows.begin(), rows.end(), [](const auto& r) { return r.cites == 0; }));
        D::insert_citation(j, d, "story.1", {1, 0}, "story.4", 2);   // page 1
        D::insert_citation(j, d, "story.1", {5, 0}, {{"story.4", 2}, {"story.4", 0}});   // Chapter Two's page: both
        const auto folio = [&](int p) {
            const auto c = L::caret_at(d, "story.1", fl.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        rows = L::source_list(d, fl);
        check("Letters: cited twice, its pages in order", rows.size() == 3 && rows[2].cites == 2 &&
                                                          rows[2].pages == (std::vector<std::string>{folio(1), folio(5)}));
        check("Almanac: once (in a citation of two)", rows[0].cites == 1 && rows[0].pages == std::vector<std::string>{folio(5)});
        check("Harbor: not cited", rows[1].cites == 0 && rows[1].pages.empty());
        check("the words without the anchor's mark", rows[2].words == "Letters of the Surveyor General.");
    }
}
}  // namespace cxtest
