// composix_bench -- timings on the generated long book (s100).
//
// Numbers before changes: the performance arc (s101..s104) moves rendering
// and reflow off the keystroke's path, and this is what it's measured
// against. Headless (core + layout, no GTK), so it runs anywhere the
// selftest does. NOT part of the selftest -- the selftest gates launch and
// must stay quick; run this by hand:
//
//   ./build/composix_bench                 # ~400 pages, pictures in /tmp
//   ./build/composix_bench --pages 200     # another length
//   ./build/composix_bench --no-pictures   # missing links (placeholders)
//   ./build/composix_bench --pdf           # also time a screen PDF export
//   ./build/composix_bench --make-odt-book DIR   # s355: a 300-page ODT import, kept as a project
//
// Each timing is the median of a few runs (the count in brackets).
#include "core/Doc.hpp"
#include "core/Outline.hpp"
#include "layout/BigBook.hpp"
#include "layout/Export.hpp"
#include "layout/Flow.hpp"
#include "layout/OdtImport.hpp"
#include "core/Odt.hpp"
#include "core/Zip.hpp"
#include "layout/Preflight.hpp"
#include "layout/ThumbStore.hpp"

#include <cairo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace L = composix::layout;
namespace C = composix::core;

namespace {

double ms_of(const std::function<void()>& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

void row(const char* what, double ms, int runs, const std::string& note = {}) {
    std::printf("  %-34s %10.1f ms  [%d]%s%s\n", what, ms, runs, note.empty() ? "" : "  ", note.c_str());
}

// Pictures decoded once and kept (the app's images:: cache does the same).
struct Pictures {
    std::map<std::string, cairo_surface_t*> kept;
    int decoded = 0;
    ~Pictures() {
        for (auto& [k, s] : kept) cairo_surface_destroy(s);
    }
    L::ImageSource source() {
        return [this](const C::Asset& a, double, double) -> cairo_surface_t* {
            auto it = kept.find(a.path);
            if (it == kept.end()) {
                cairo_surface_t* s = nullptr;
                if (std::filesystem::exists(a.path)) {
                    s = cairo_image_surface_create_from_png(a.path.c_str());
                    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
                        cairo_surface_destroy(s);
                        s = nullptr;
                    } else {
                        ++decoded;
                    }
                }
                it = kept.emplace(a.path, s).first;
            }
            return it->second ? cairo_surface_reference(it->second) : nullptr;
        };
    }
};

// One page drawn as the thumbnails do: white, scaled, layout::draw_page.
void draw_one(const C::Doc& d, L::FlowCache& flows, const L::ImageSource& img, const std::string& page, double px_per_pt) {
    const auto* p = d.as<C::Page>(page);
    const int w = std::max(1, static_cast<int>(p->w * px_per_pt)), h = std::max(1, static_cast<int>(p->h * px_per_pt));
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, px_per_pt, px_per_pt);
    L::draw_page(cr, d, page, flows, img, nullptr, {.placeholders = true});
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
}

}  // namespace

// s348: a real project -- what a master edit costs on it (Scott's book).
int bench_project(const std::string& dir) {
    C::Doc d;
    std::string err;
    if (!C::load_project(d, dir, &err)) { std::printf("can't load %s: %s\n", dir.c_str(), err.c_str()); return 2; }
    const auto stories = d.ids_of<C::Story>();
    std::printf("composix_bench -- %s\n  %zu pages, %zu stories\n\n", dir.c_str(), d.pages().size(), stories.size());
    L::FlowCache pf;
    row("flow every story (cold)", ms_of([&] { for (const auto& s : stories) (void)pf.get(d, s); }), 1);
    if (std::getenv("COMPOSIX_BENCH_KEYS")) {   // s348: keys only, over and over (to profile)
        C::DocJournal jk{d};
        for (int r = 0; r < 30; ++r) {
            if (std::string(std::getenv("COMPOSIX_BENCH_KEYS")) == "margin")
                C::doc::edit_masters(jk, d, {d.ids_of<C::Master>().front()}, "m", [](C::Master& m) { m.margins.top += 1; });
            else
                C::doc::edit_text(jk, d, stories.front(), {3, 2}, {3, 2}, "x");
            const auto pages = d.pages();
            row("keys", ms_of([&] { for (const auto& p : pages) (void)L::page_key(d, pf, p); }), 1);
        }
        return 0;
    }
    if (std::getenv("COMPOSIX_BENCH_BODY")) {   // s351: body frames made on a real book, then one narrowed
        C::DocJournal jb{d};
        jb.set_settle([&d] { auto out = C::doc::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        std::string longest;
        std::size_t most = 0;
        for (const auto& s : stories) if (d.as<C::Story>(s)->paras.size() > most) { most = d.as<C::Story>(s)->paras.size(); longest = s; }
        int n = 0;
        row("make body frames", ms_of([&] { n = C::doc::make_body_frames(jb, d, longest); }), 1, std::to_string(n) + " frames follow");
        int bodies = 0, alone = 0;
        for (const auto& f : d.ids_of<C::Frame>()) {
            const auto* x = d.as<C::Frame>(f);
            bodies += x->body;
            alone += x->story == longest && !x->follow_body;
        }
        std::printf("  %d body frames; %d of the story's frames left their own\n", bodies, alone);
        for (const auto& s : stories) (void)pf.get(d, s);
        std::string b;
        for (const auto& f : d.ids_of<C::Frame>()) if (d.as<C::Frame>(f)->body) { b = f; break; }
        row("a body frame narrowed (step + settle)", ms_of([&] { C::doc::edit_frames(jb, d, {b}, "w", [](C::Frame& f) { f.w -= 12; }); }), 1);
        int whole = 0;
        row("  reflow", ms_of([&] { for (const auto& s : stories) { (void)pf.get(d, s); whole += pf.last_how() == L::FlowCache::How::Whole; } }), 1,
            std::to_string(whole) + " whole");
        return 0;
    }
    if (std::getenv("COMPOSIX_BENCH_GRID")) {   // s353: Scott's crash -- body frames as the import makes them, a grid, the reflow
        C::DocJournal jg{d};
        jg.set_settle([&d] { auto out = C::doc::follow_body_frames(d); auto fit = L::fit_frames(d); out.insert(out.end(), fit.begin(), fit.end()); return out; });
        if (d.as<C::Story>("story.1")) { std::vector<C::Doc::Id> made; C::doc::body_frames_into(d, "story.1", made); }
        C::doc::edit_masters(jg, d, {d.ids_of<C::Master>().front()}, "step", [](C::Master& m) { m.grid = 13; });
        for (int round = 0; round < 4; ++round) {
            int added = 0;
            for (const auto& s : d.ids_of<C::Story>())
                if (L::reflows(d, s) && pf.get(d, s).overset) added += L::reflow_pages(jg, d, s);
            std::printf("  round %d: %d page(s) added, %zu pages\n", round, added, d.pages().size());
            for (const auto& p : d.pages()) (void)L::page_key(d, pf, p);
            (void)C::project(d, "", nullptr, nullptr, nullptr);
            (void)L::preflight_issues(d, pf, {});
            if (!added) break;
        }
        return 0;
    }
    C::DocJournal j{d};
    const auto ms = d.ids_of<C::Master>();
    const auto try_edit = [&](const char* label, const std::function<void()>& edit) {
        for (const auto& s : stories) (void)pf.get(d, s);
        const double te = ms_of(edit);
        int whole = 0;
        const double tf = ms_of([&] {
            for (const auto& s : stories) { (void)pf.get(d, s); whole += pf.last_how() == L::FlowCache::How::Whole; }
        });
        const auto pages = d.pages();
        const double tk = ms_of([&] { for (const auto& p : pages) (void)L::page_key(d, pf, p); });
        const double tp = ms_of([&] { (void)C::project(d, "", nullptr, nullptr, nullptr); });
        const double tpre = ms_of([&] { (void)L::stale_lists(d, pf); (void)L::preflight_issues(d, pf, {}); });
        std::printf("  %-28s edit %6.1f  reflow %7.1f (%d whole)  keys %6.1f  project %6.1f  preflight %6.1f  = %7.1f ms\n", label, te, tf, whole, tk,
                    tp, tpre, te + tf + tk + tp + tpre);
    };
    try_edit("master top margin", [&] { C::doc::edit_masters(j, d, {ms.front()}, "Top margin", [](C::Master& m) { m.margins.top += 6; }); });
    std::string hdr;
    for (const auto& f : d.ids_of<C::Frame>()) if (d.as<C::Master>(d.as<C::Frame>(f)->owner)) { hdr = f; break; }
    if (!hdr.empty())
        try_edit("a master's header frame", [&] { C::doc::edit_frames(j, d, {hdr}, "Move", [](C::Frame& f) { f.y += 3; }); });
    try_edit("a swatch", [&] { if (!d.ids_of<C::Swatch>().empty()) { const auto id = d.ids_of<C::Swatch>().front(); auto sw = *d.as<C::Swatch>(id); sw.name += "x"; C::doc::edit_swatch(j, d, id, sw); } });
    try_edit("typing in the main story", [&] { C::doc::edit_text(j, d, stories.front(), {3, 2}, {3, 2}, "x"); });
    return 0;
}

// s355: a book as an ODT import makes it (Scott's Hollow Oak: ~300 pages,
// chapters on right-hand pages, body frames, a grid), kept as a project --
// to open in the app and see what the navigator costs on it.
int make_odt_book(const std::string& dir) {
    const std::string ns = R"( xmlns:office="o" xmlns:style="s" xmlns:text="t" xmlns:fo="f")";
    const std::string styles = "<office:document-styles" + ns + R"(><office:styles>
 <style:style style:name="Standard" style:family="paragraph"><style:paragraph-properties fo:line-height="13pt" fo:text-indent="0.25in"/><style:text-properties fo:font-size="11pt"/></style:style>
 <style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:master-page-name="Right_20_Page"><style:text-properties fo:font-size="18pt"/></style:style>
</office:styles><office:automatic-styles>
 <style:page-layout style:name="pm1" style:page-usage="mirrored"><style:page-layout-properties fo:page-width="5.5in" fo:page-height="8.5in" fo:margin-top="0.75in" fo:margin-bottom="0.75in" fo:margin-left="0.75in" fo:margin-right="0.6in"/></style:page-layout>
</office:automatic-styles><office:master-styles>
 <style:master-page style:name="Standard" style:page-layout-name="pm1"><style:footer><text:p><text:page-number/></text:p></style:footer></style:master-page>
 <style:master-page style:name="Right_20_Page" style:display-name="Right Page" style:page-layout-name="pm1" style:next-style-name="Standard"><style:footer><text:p><text:page-number/></text:p></style:footer></style:master-page>
</office:master-styles></office:document-styles>)";
    std::string body;
    const char* words = "The oak stood hollow at the edge of the wood, and the children went to it every evening after supper, "
                        "listening for the sound that had come from it the night the storm broke over the valley. ";
    for (int c = 1; c <= 34; ++c) {
        body += "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">Chapter " + std::to_string(c) + "</text:h>";
        for (int p = 0; p < 40; ++p) body += std::string("<text:p text:style-name=\"Standard\">") + words + words + "</text:p>";
    }
    body += "<text:p text:style-name=\"Standard\">THE END</text:p>";
    const std::string content = "<office:document-content" + ns + "><office:body><office:text>" + body + "</office:text></office:body></office:document-content>";
    const auto odt = C::zip::make_stored({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", content}, {"styles.xml", styles}});
    std::string err;
    const auto book = C::odt::read(odt, &err);
    C::Doc d;
    L::OdtInfo info;
    double ms = 0;
    bool ok = false;
    ms = ms_of([&] { ok = book && L::import_odt(d, *book, &info, {}, &err); });
    if (!ok) { std::printf("import failed: %s\n", err.c_str()); return 2; }
    std::printf("imported: %zu pages, %d paragraphs in %.0f ms; overset %d\n", d.pages().size(), info.paragraphs, ms,
                static_cast<int>(L::flow_story(d, "story.1").overset));
    if (!C::save_project(d, dir, &err)) { std::printf("not saved: %s\n", err.c_str()); return 2; }
    std::printf("kept: %s\n", dir.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // each row as it's timed
    if (argc == 3 && std::string(argv[1]) == "--make-odt-book") return make_odt_book(argv[2]);   // s355
    if (argc == 3 && std::string(argv[1]) == "--project") return bench_project(argv[2]);   // s348
    L::BigBookOpts o;
    o.picture_dir = (std::filesystem::temp_directory_path() / "composix-bench").string();
    bool pdf = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pages" && i + 1 < argc) o.pages = std::atoi(argv[++i]);
        else if (a == "--no-pictures") o.picture_dir.clear();
        else if (a == "--pdf") pdf = true;
        else {
            std::printf("usage: composix_bench [--pages N] [--no-pictures] [--pdf]\n");
            return 2;
        }
    }

    C::Doc d;
    L::BigBookInfo info;
    const double t_build = ms_of([&] { info = L::load_big_book(d, o); });
    const auto pages = d.pages();
    std::printf("composix_bench -- the long book\n");
    std::printf("  %d pages, %d chapters, %d section heads, %d pictures placed, %.0f KB of text in one story\n",
                info.pages, info.chapters, info.sections, info.pictures, info.bytes / 1024.0);
    std::printf("  %zu paragraphs; fitted in %d chapter flows + %d whole; pictures: %s\n\n", d.as<C::Story>("story.1")->paras.size(),
                info.rounds, info.whole, o.picture_dir.empty() ? "none (missing links)" : o.picture_dir.c_str());
    row("generate + fit the book", t_build, 1);

    // ── the flow ────────────────────────────────────────────────────────────
    const auto stories = d.ids_of<C::Story>();
    {
        std::vector<double> t;
        for (int r = 0; r < 3; ++r) t.push_back(ms_of([&] { (void)L::flow_story(d, "story.1"); }));
        row("flow the long story", median(t), 3);
        std::vector<double> t2;
        for (int r = 0; r < 3; ++r)
            t2.push_back(ms_of([&] {
                L::FlowCache f;
                for (const auto& s : stories) (void)f.get(d, s);
            }));
        row("flow every story (a cold cache)", median(t2), 3, std::to_string(stories.size()) + " stories");
    }
    const auto flow = L::flow_story(d, "story.1");
    bool overset = flow.overset;

    // ── a keystroke: the edit, then the reflow the next draw pays for ──────
    {
        C::DocJournal j{d};
        const auto& body = d.as<C::Story>("story.1")->paras;
        const int early = 2, late = static_cast<int>(body.size()) - 3;
        struct Case { const char* label; int para; const char* text; };
        for (const auto& [label, para, text] : {Case{"keystroke near the start", early, "x"}, Case{"keystroke near the end", late, "x"},
                                                Case{"Enter near the start", early, "\n"}}) {
            std::vector<double> te, tf;
            int filled = 0;
            for (int r = 0; r < 5; ++r) {
                L::FlowCache f;
                (void)f.get(d, "story.1");   // warm, as the canvas is
                te.push_back(ms_of([&] { C::doc::edit_text(j, d, "story.1", {para, 5}, {para, 5}, text); }));
                tf.push_back(ms_of([&] { filled = f.get(d, "story.1").filled; }));
            }
            row((std::string(label) + ": the edit").c_str(), median(te), 5, "journal + store");
            // s105: re-flowed from the edit until the lines resettle.
            row((std::string(label) + ": reflow").c_str(), median(tf), 5,
                std::to_string(filled) + " of " + std::to_string(flow.frames.size()) + " frames set");
        }
        while (j.can_undo()) j.undo();
    }

    // ── what a page looks like (the thumbnails' key) ───────────────────────
    L::FlowCache flows;
    {
        (void)flows.get(d, "story.1");
        std::vector<double> t;
        for (int r = 0; r < 3; ++r)
            t.push_back(ms_of([&] {
                for (const auto& p : pages) (void)L::page_key(d, flows, p);
            }));
        row("page keys, every page (warm flow)", median(t), 3);
    }

    // ── drawing pages ───────────────────────────────────────────────────────
    Pictures pics;
    const auto img = pics.source();
    {
        // A body page with text only, and one with a picture: at 100% (the
        // canvas's 96 px / inch) and 200%.
        std::string plain, picture;
        for (std::size_t k = 8; k < pages.size() && (plain.empty() || picture.empty()); ++k) {
            const auto on = d.frames_on(pages[k]);
            const bool has_pic = std::any_of(on.begin(), on.end(), [&](const auto& f) { return d.as<C::Frame>(f)->kind == C::FrameKind::Image; });
            if (has_pic && picture.empty()) picture = pages[k];
            if (!has_pic && plain.empty() && on.size() == 1) plain = pages[k];
        }
        draw_one(d, flows, img, picture.empty() ? pages.back() : picture, 96.0 / 72);   // decode its picture first
        for (const auto& [label, page, zoom] :
             {std::tuple{"draw a text page at 100%", plain, 96.0 / 72}, std::tuple{"draw a text page at 200%", plain, 2 * 96.0 / 72},
              std::tuple{"draw a picture page at 100%", picture, 96.0 / 72}}) {
            if (page.empty()) continue;
            std::vector<double> t;
            for (int r = 0; r < 7; ++r) t.push_back(ms_of([&] { draw_one(d, flows, img, page, zoom); }));
            row(label, median(t), 7, "page " + d.folio(page));
        }
    }
    {
        // Every page as a thumbnail, 120 px tall (the Pages tiles' size).
        const double z = 120.0 / d.settings().trim_h;
        const int before = pics.decoded;
        const double cold = ms_of([&] {
            Pictures fresh;
            const auto im = fresh.source();
            for (const auto& p : pages) draw_one(d, flows, im, p, z);
        });
        std::vector<double> t;
        for (int r = 0; r < 3; ++r)
            t.push_back(ms_of([&] {
                for (const auto& p : pages) draw_one(d, flows, img, p, z);
            }));
        (void)before;
        row("thumbnails, every page (decoding)", cold, 1, std::to_string(pages.size()) + " pages, pictures read");
        row("thumbnails, every page (decoded)", median(t), 3,
            std::to_string(median(t) / std::max<std::size_t>(1, pages.size())).substr(0, 4) + " ms a page");
    }

    {
        // s103: the same tiles on the worker, as the app draws them. The UI
        // thread's share: the keys (look) and one snapshot (flush).
        const int th = 120, tw = std::max(1, static_cast<int>(120 * d.settings().trim_w / d.settings().trim_h));
        std::vector<double> ui, snap, worker;
        for (int r = 0; r < 3; ++r) {
            L::ThumbStore st;
            st.set_threaded(true);
            double tf = 0;
            const double tl = ms_of([&] {
                for (const auto& p : pages) (void)st.look(d, flows, img, p, p, tw, th, false);
            });
            const double tw0 = ms_of([&] {
                tf = ms_of([&] { (void)st.flush(d, flows, img); });
                st.wait_idle();
            });
            (void)st.collect();
            ui.push_back(tl + tf);
            snap.push_back(tf);
            worker.push_back(tw0 - tf);
        }
        row("thumbnails on the worker: UI thread", median(ui), 3, "keys + one snapshot");
        row("  the snapshot (records + layout)", median(snap), 3, "once a revision");
        row("  the worker, every page", median(worker), 3, "off the UI thread");
    }

    // ── what runs on every change in the app ───────────────────────────────
    {
        // Slow at length (they lay out copies): timed once.
        bool current = false;
        const double t2 = ms_of([&] { current = L::stale_lists(d, flows).empty(); });
        row("stale contents check (warm flow)", t2, 1, current ? "current" : "STALE");
        const double t = ms_of([&] { (void)L::preflight_issues(d, flows, {}); });
        row("preflight (warm flow)", t, 1);
    }
    {
        // What Shell::refresh does on the idle after ANY change (a keystroke):
        // project_now flows every story for the overset badges and runs the
        // stale check; update_preflight runs preflight (the stale check
        // again). s105: a keystroke no longer empties the cache -- the edited
        // story is re-flowed from the edit, the others kept.
        L::FlowCache pf;
        for (const auto& s : stories) (void)pf.get(d, s);
        C::DocJournal j{d};
        C::doc::edit_text(j, d, "story.1", {2, 5}, {2, 5}, "x");
        const double t = ms_of([&] {
            for (const auto& s : stories) (void)pf.get(d, s);
            (void)L::stale_lists(d, pf);
            (void)L::preflight_issues(d, pf, {});
        });
        j.undo();
        row("the app's refresh after a keystroke", t, 1, "project_now + update_preflight");
        std::vector<double> tp;
        for (int r = 0; r < 3; ++r) tp.push_back(ms_of([&] { (void)C::project(d, "", nullptr, nullptr, nullptr); }));
        row("  the projection (core::project)", median(tp), 3, "the navigator's / tiles' model");
    }
    {
        // s348 (Scott: "sluggish with 305 pages ... setting margins"): a
        // master's margin changed, then what the app pays before it answers.
        L::FlowCache pf;
        for (const auto& s : stories) (void)pf.get(d, s);
        C::DocJournal j{d};
        std::vector<std::string> ms = d.ids_of<C::Master>();
        double te = 0, tf = 0, tk = 0, tpj = 0, tst = 0, tpre = 0;
        te = ms_of([&] { C::doc::edit_masters(j, d, {ms.front()}, "Top margin", [](C::Master& m) { m.margins.top += 6; }); });
        tf = ms_of([&] { for (const auto& s : stories) (void)pf.get(d, s); });
        tk = ms_of([&] { for (const auto& p : pages) (void)L::page_key(d, pf, p); });
        tpj = ms_of([&] { (void)C::project(d, "", nullptr, nullptr, nullptr); });
        tst = ms_of([&] { (void)L::stale_lists(d, pf); });
        tpre = ms_of([&] { (void)L::preflight_issues(d, pf, {}); });
        row("master margin: the edit", te, 1, "journal + settle");
        row("  reflow every story", tf, 1);
        row("  page keys, every page", tk, 1, "the thumbnails' look");
        row("  the projection", tpj, 1);
        row("  stale contents check", tst, 1);
        row("  preflight", tpre, 1);
        j.undo();
    }
    {
        C::DocJournal j{d};
        const double t = ms_of([&] { (void)L::build_list(j, d, "list.1"); });
        row("rebuild the contents", t, 1, "settled on copies");
    }
    if (pdf) {
        const std::string out = (std::filesystem::temp_directory_path() / "composix-bench.pdf").string();
        L::PdfOptions po;
        std::string err;
        bool ok = false;
        const double t = ms_of([&] { ok = L::export_pdf(d, out, po, img, nullptr, &err); });
        row("export the screen PDF", t, 1, ok ? out : "FAILED: " + err);
    }
    std::printf("\n%s\n", overset ? "WARNING: the long story is overset" : "nothing overset");
    return overset ? 1 : 0;
}
