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
//
// Each timing is the median of a few runs (the count in brackets).
#include "core/Doc.hpp"
#include "layout/BigBook.hpp"
#include "layout/Export.hpp"
#include "layout/Flow.hpp"
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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // each row as it's timed
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
        for (const auto& [label, para] : {std::pair{"near the start", early}, std::pair{"near the end", late}}) {
            std::vector<double> te, tf;
            for (int r = 0; r < 5; ++r) {
                L::FlowCache f;
                (void)f.get(d, "story.1");   // warm, as the canvas is
                te.push_back(ms_of([&] { C::doc::edit_text(j, d, "story.1", {para, 5}, {para, 5}, "x"); }));
                tf.push_back(ms_of([&] { (void)f.get(d, "story.1"); }));
            }
            row((std::string("keystroke ") + label + ": the edit").c_str(), median(te), 5, "journal + store");
            row((std::string("keystroke ") + label + ": reflow").c_str(), median(tf), 5, "the whole story, as now");
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
        // again) -- all on the one cache the change just emptied.
        const double t = ms_of([&] {
            L::FlowCache pf;   // s101: one cache for all three (the change emptied it)
            for (const auto& s : stories) (void)pf.get(d, s);
            (void)L::stale_lists(d, pf);
            (void)L::preflight_issues(d, pf, {});
        });
        row("the app's refresh after a change", t, 1, "project_now + update_preflight");
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
