#pragma once
// views::PageThumbs -- the page and master tiles drawn for real (s020).
//
// Each thumbnail is the page as the screen PDF draws it (layout::draw_page:
// masters' items, fills, text, pictures, strokes), rendered once into an
// image at the tile's size and kept. It is drawn again only when what the
// page looks like changes -- its layout::PageKey (its records, its masters',
// its frames' and their pictures', the styles and swatches, and the lines
// of text set on it) -- so typing on page 3 redraws page 3 (and any page the
// text reflows onto), not the book.
//
// s103: drawn on a worker thread (layout::ThumbStore) from a snapshot of the
// document. Until a page's picture is ready, the tile shows its last one
// (scaled), or the stand-in type bars; when it comes back the tiles that
// show that page are drawn again (views::thumbs_ready).
#include "core/Doc.hpp"
#include "layout/Export.hpp"
#include "layout/Flow.hpp"
#include "layout/ThumbStore.hpp"

#include <map>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <glibmm/dispatcher.h>
#include <sigc++/connection.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace composix::views {

class PageThumbs {
public:
    // `project_dir` -- the folder assets resolve in (the shell's to know);
    // read on the UI thread, handed to the worker by value.
    PageThumbs(const core::Doc& d, std::function<std::string()> project_dir);
    ~PageThumbs();

    // Draw page / master `id` into the paper at x, y, w, h (user units of
    // `cr`). False if `id` isn't a page or master, or (threaded) nothing of
    // it is drawn yet -- the caller's stand-in shows.
    bool paint(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id, double x, double y, double w, double h,
               bool large);
    // Another document (or folder): nothing kept carries over.
    void forget();
    // s101: flow through this cache (the shell's, shared with the canvas).
    void share_flows(layout::FlowCache& f) { m_flowp = &f; }
    // s103: on the worker (the app); off, a miss is drawn on the spot.
    void set_threaded(bool on) { m_store.set_threaded(on); }
    int renders() const { return m_store.renders(); }   // pages drawn (tests, logs)
    const layout::ThumbStore& store() const { return m_store; }

private:
    layout::ImageSource images() const;
    void                on_worker();   // UI thread, from the dispatcher

    const core::Doc&             m_doc;
    std::function<std::string()> m_dir;
    layout::FlowCache            m_own_flows;
    layout::FlowCache*           m_flowp = &m_own_flows;   // s101: the shell's, shared
    Glib::Dispatcher             m_disp;                   // before m_store: the worker may emit until it stops
    std::atomic<bool>            m_signalled = false;
    // The log's line: a run of pictures, from the first ask to the last back.
    std::chrono::steady_clock::time_point m_run_t0{};
    int                          m_run_n = 0;
    bool                         m_running = false;
    sigc::connection             m_flush_conn, m_collect_conn;
    layout::ThumbStore           m_store;
    // s356: a page's last picture at any size -- shown, scaled, while a new size is drawn
    std::map<std::string, layout::Surface> m_last;
};

}  // namespace composix::views
