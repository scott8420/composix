#include "Shell.hpp"
#include "Log.hpp"

#include <chrono>
#include <cstdlib>

// Shell_reflow.cpp -- HELPERS for s106: the far end of a long story, set on
// a worker. A keystroke re-flows at most a few frames on the spot (the
// shell's FlowCache budget); when the lines haven't resettled by then the
// flow is provisional -- the old far end moved along -- and the whole is
// worked out here from a copy of the document, then handed in
// (FlowCache::complete) and the views drawn again. More typing meanwhile:
// the next run starts from what this one worked out.

namespace composix {

int Shell::reflow_budget() {  // helper
    static const int b = [] {
        if (const char* e = std::getenv("COMPOSIX_REFLOW_BUDGET")) {
            const int v = std::atoi(e);
            if (v > 0) return v;
        }
        return kReflowBudget;
    }();
    return b;
}

struct Shell::FlowJob {
    core::Doc                                  doc;   // records only, as of `rev`
    std::uint64_t                              rev = 0;
    std::vector<layout::FlowCache::Unfinished> todo;
    std::vector<std::pair<std::string, layout::StoryFlow>> done;
    double                                     ms = 0;
    int                                        frames = 0;
};

void Shell::finish_flows() {  // helper
    if (m_flow_job || !m_pf_flows.any_unfinished()) return;
    if (!m_flow_disp_bound) {
        m_flow_disp_bound = true;
        m_flow_disp.connect([this] {
            if (m_flow_thread.joinable()) m_flow_thread.join();
            auto job = std::move(m_flow_job);
            if (!job) return;
            for (auto& [sid, flow] : job->done) {
                const auto* st = job->doc.as<core::Story>(sid);
                if (st) m_pf_flows.complete(m_doc, sid, job->rev, std::move(flow), *st);
            }
            if (auto lg = log::get(log::Area::Shell))
                lg->info("reflow: the rest of {} {} set on a worker ({} frames, {:.0f} ms){}", job->done.size(),
                         job->done.size() == 1 ? "story" : "stories", job->frames, job->ms,
                         job->rev == m_doc.revision() ? "" : " -- typed on meanwhile");
            queue_refresh();
            m_canvas_view.canvas().queue_draw();
        });
    }
    auto job = std::make_shared<FlowJob>();
    {
        core::Doc::Loading load(job->doc);
        for (const auto& [id, r] : m_doc.records()) job->doc.put(id, r);
    }
    job->rev = m_doc.revision();
    job->todo = m_pf_flows.unfinished();
    m_flow_job = job;
    m_flow_thread = std::thread([this, job] {
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& u : job->todo) {
            auto f = layout::reflow_story(job->doc, u.story, u.basis_text, u.basis);
            job->frames += f.filled;
            job->done.emplace_back(u.story, std::move(f));
        }
        job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_flow_disp.emit();
    });
}

// s108: what's going on in the background, in the header: the rest of a
// story being set, page tiles, canvas pages. After 400 ms of it (quick work
// never shows); gone when it's done.
void Shell::poll_work() {  // helper
    if (!m_work) return;
    const std::size_t tiles = m_page_thumbs.store().pending();
    const std::size_t pages = m_canvas_view.canvas().pages_pending();
    const bool flowing = m_flow_job != nullptr;
    const bool busy = flowing || tiles > 0 || pages > 0;
    const auto now = std::chrono::steady_clock::now();
    if (busy && !m_work_busy) m_work_since = now;
    m_work_busy = busy;
    const bool show = busy && now - m_work_since >= std::chrono::milliseconds(400);
    if (show) {
        std::string what, tip;
        if (flowing) {
            what = "Setting the rest of the text\u2026";
            tip = "The pages near your edit are set; the rest of the story follows on in the background.";
        } else if (tiles > 0) {
            what = "Drawing pages \u00b7 " + std::to_string(tiles) + " to go";
            tip = "The page tiles are drawn in the background; each shows as it's ready.";
        } else {
            what = "Drawing pages\u2026";
            tip = "The canvas's pages are drawn again in the background (a zoom, Proof Colours).";
        }
        m_work_label->set_text(what);
        m_work->set_tooltip_text(tip);
    }
    if (show != m_work->get_visible()) {
        m_work->set_visible(show);
        if (show) m_work_spin->start(); else m_work_spin->stop();
    }
}

}  // namespace composix
