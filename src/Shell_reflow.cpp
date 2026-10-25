#include "Shell.hpp"
#include "Log.hpp"

#include <atomic>
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
    // s356: whole stories (a master, a style changed) and pages to add
    bool                                       whole = false;   // any whole story or plan: the card shows
    std::vector<std::string>                   plans;
    std::vector<std::pair<std::string, int>>   added;   // story, pages reflow adds
    std::atomic<int>                           tick{0}, total{0};   // frames set, of all (the bar)
    std::atomic<bool>                          planning{false};     // working out the pages to add
};

void Shell::finish_flows() {  // helper
    if (m_flow_job || (!m_pf_flows.any_unfinished() && m_reflow_plans.empty())) return;
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
            const bool moved_on = job->rev != m_doc.revision();
            // s356: the pages a long story needs, added as one step (only if nothing changed meanwhile)
            int added = 0;
            if (job->rev == m_doc.revision() && m_doc.settings().reflow && !m_journal.can_redo())
                for (const auto& [sid, n] : job->added) added += layout::add_reflow_pages(m_journal, m_doc, sid, n);
            if (added > 0)
                if (auto lg = log::get(log::Area::Shell)) lg->info("text reflow: {} page(s) added (worked out on the worker)", added);
            if (job->whole && added == 0) check_reflow();   // the whole flows in: empty end pages, overset
            if (auto lg = log::get(log::Area::Shell); lg && !job->done.empty())
                lg->info("reflow: the rest of {} {} set on a worker ({} frames, {:.0f} ms){}", job->done.size(),
                         job->done.size() == 1 ? "story" : "stories", job->frames, job->ms,
                         moved_on ? " -- changed meanwhile" : "");
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
    job->plans.assign(m_reflow_plans.begin(), m_reflow_plans.end());
    m_reflow_plans.clear();
    int total = 0;
    for (const auto& u : job->todo)
        if (u.whole) total += static_cast<int>(layout::thread_frames(job->doc, u.story).size());
    for (const auto& s : job->plans) total += static_cast<int>(layout::thread_frames(job->doc, s).size());
    job->total = std::max(1, total);
    job->whole = total > 0;
    m_flow_job = job;
    m_flow_thread = std::thread([this, job] {
        const auto t0 = std::chrono::steady_clock::now();
        int before = 0;
        for (const auto& u : job->todo) {
            if (u.whole) {   // s356: a master / style change -- the story whole, the bar told
                const int n = static_cast<int>(layout::thread_frames(job->doc, u.story).size());
                auto f = layout::flow_story(job->doc, u.story, [&](int done, int) { job->tick = before + done; });
                before += n;
                job->frames += static_cast<int>(f.frames.size());
                job->done.emplace_back(u.story, std::move(f));
                continue;
            }
            auto f = layout::reflow_story(job->doc, u.story, u.basis_text, u.basis);
            job->frames += f.filled;
            job->done.emplace_back(u.story, std::move(f));
        }
        job->planning = !job->plans.empty();
        for (const auto& s : job->plans) {   // s356: the pages it needs, worked out on a copy
            const int n = layout::reflow_pages_needed(job->doc, s);
            before += static_cast<int>(layout::thread_frames(job->doc, s).size());
            job->tick = before;
            if (n > 0) job->added.emplace_back(s, n);
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
    // s356: the whole book laid out on the worker -- the card over the canvas, after 150 ms
    {
        const bool whole = flowing && m_flow_job->whole;
        static std::chrono::steady_clock::time_point since{};
        static bool was = false;
        const auto t = std::chrono::steady_clock::now();
        if (whole && !was) since = t;
        was = whole;
        const bool card = whole && t - since >= std::chrono::milliseconds(150);
        if (card && m_layout_card) {
            const int total = std::max(1, m_flow_job->total.load()), done = std::min(m_flow_job->tick.load(), total);
            if (m_flow_job->planning) {   // no count to tell: the bar pulses
                m_layout_label->set_text("Adding pages for the text\u2026");
                m_layout_bar->pulse();
            } else {
                m_layout_label->set_text("Laying out the pages \u00b7 " + std::to_string(done) + " of " + std::to_string(total));
                m_layout_bar->set_fraction(static_cast<double>(done) / total);
            }
        }
        if (m_layout_card && card != m_layout_card->get_visible()) m_layout_card->set_visible(card);
    }
    const bool busy = (flowing && !m_flow_job->whole) || tiles > 0 || pages > 0;   // s356: the card says the whole
    const auto now = std::chrono::steady_clock::now();
    if (busy && !m_work_busy) m_work_since = now;
    m_work_busy = busy;
    const bool show = busy && now - m_work_since >= std::chrono::milliseconds(400);
    if (show) {
        std::string what, tip;
        if (flowing && !m_flow_job->whole) {
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
