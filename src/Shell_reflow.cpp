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

}  // namespace composix
