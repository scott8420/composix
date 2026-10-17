#pragma once
// layout::ThumbStore -- page thumbnails drawn on a worker thread (s103).
// GTK-free, so the selftest drives it headless; views::PageThumbs is its
// thin GTK side (a dispatcher to come back to the UI thread, the redraw).
//
// How a tile gets its picture:
//   * look() (UI thread) works out the page's layout::PageKey -- what it
//     looks like -- and returns the picture kept for it: CURRENT (same key,
//     same size: paint it), or the last one drawn (stale: paint it scaled
//     until the new one comes), or none (paint a placeholder). A miss is
//     ASKED for -- once per key and size.
//   * flush() (UI thread, at idle -- after a draw pass has asked for what
//     it needs) hands the asks to the worker with a SNAPSHOT of the document:
//     a copy of its records and of the shell's warm flow cache, made once per
//     revision. The worker never reads the live document.
//   * The worker draws each page from that snapshot, newest ask for a tile
//     winning (an older one still queued is dropped), and calls `notify`.
//   * collect() (UI thread) keeps what's finished -- each picture under the
//     key it was drawn for, so a result that is already stale is a stale
//     picture, never a current one -- and says which pages to redraw.
//   * An LRU cap on the bytes kept: the least recently looked-at go first.
//
// Unthreaded (set_threaded(false), the default), look() draws a miss on the
// spot, as before s103.
#include "core/Doc.hpp"
#include "layout/Export.hpp"
#include "layout/Flow.hpp"

#include <cairo.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace composix::layout {

using Surface = std::shared_ptr<cairo_surface_t>;   // owns one reference
Surface adopt_surface(cairo_surface_t* s);

// One page / master drawn into a pw x ph RGB image, white paper; `guides`
// adds the margins and columns (the preview; every master). Any thread, given
// a document and a flow cache no other thread is using.
Surface render_thumb(const core::Doc& d, FlowCache& flows, const std::string& id, int pw, int ph, bool guides,
                     const ImageSource& images);

class ThumbStore {
public:
    // s104: what draws a picture -- any thread, from the document (and flow
    // cache) it is handed: the live one, or a snapshot on the worker.
    using Render = std::function<Surface(const core::Doc& d, FlowCache& flows, const ImageSource& images)>;
    // What look() does with a miss: ask the worker; draw it now (and keep
    // it); draw it now only if there is no picture at all (else the stale one
    // stands in while the worker draws the new).
    enum class Miss { Ask, Now, NowIfNone };

    struct Look {
        Surface surf;            // null: nothing yet -- the placeholder
        bool    current = false; // drawn for what the page looks like now, at this size
    };

    explicit ThumbStore(std::size_t cap_bytes = std::size_t{96} << 20);
    ~ThumbStore();
    ThumbStore(const ThumbStore&) = delete;
    ThumbStore& operator=(const ThumbStore&) = delete;

    void set_threaded(bool on) { m_threaded = on; }
    bool threaded() const { return m_threaded; }
    // Called ON THE WORKER after each finished picture: hop to the UI thread.
    void set_notify(std::function<void()> f);

    // ── UI thread ──
    // `slot` names the tile's picture (the page id, or id + "|L" for the
    // preview's size); `id` the page or master.
    Look look(const core::Doc& d, FlowCache& flows, const ImageSource& images, const std::string& slot,
              const std::string& id, int pw, int ph, bool guides);
    // s104: the general form. `variant` -- whatever else decides the picture
    // besides the page's key and size (a zoom, a mode); `render` draws it.
    Look look(const core::Doc& d, FlowCache& flows, const ImageSource& images, const std::string& slot,
              const std::string& id, const std::string& variant, int pw, int ph, const Render& render, Miss miss);
    // Hand the asks to the worker. False: nothing asked.
    bool flush(const core::Doc& d, const FlowCache& flows, const ImageSource& images);
    bool asking() const { return !m_asks.empty(); }
    // Keep what's finished; the pages (ids) that now have a newer picture.
    std::set<std::string> collect();
    // Another document: nothing kept, asked or queued carries over.
    void forget();

    // ── counts (tests, logs) ──
    int         renders() const;          // pictures drawn, either thread
    int         snapshots() const { return m_snapshots; }
    std::size_t bytes() const { return m_bytes; }
    std::size_t kept() const { return m_entries.size(); }
    std::size_t cap() const { return m_cap; }
    // Asks still with the worker (queued or being drawn).
    std::size_t pending() const;
    void        set_cap(std::size_t b) { m_cap = b; }
    // Tests: hold the worker (asks queue up); wait until it has nothing to do.
    void hold(bool on);
    void wait_idle();

private:
    struct Snap {
        core::Doc     doc;
        FlowCache     flows;
        ImageSource   images;
        std::uint64_t gen = 0;
    };
    struct Job {
        std::string           slot, id;
        PageKey               key;
        std::string           variant;
        int                   pw = 0, ph = 0;
        Render                render;
        std::shared_ptr<Snap> snap;
        std::uint64_t         asked_at = 0;   // m_tick when asked
    };
    struct Done {
        std::string   slot, id;
        PageKey       key;
        std::string   variant;
        int           pw = 0, ph = 0;
        Surface       surf;
        std::uint64_t gen = 0;
        std::uint64_t asked_at = 0;
    };
    struct Entry {
        PageKey       key;        // what `surf` was drawn for
        std::string   variant;
        Surface       surf;
        int           pw = 0, ph = 0;
        bool          asked = false;   // an ask for want_key at want_pw x want_ph is out
        PageKey       want_key;
        std::string   want_variant;
        int           want_pw = 0, want_ph = 0;
        std::uint64_t used = 0;   // LRU tick
        std::uint64_t drawn_at = 0;   // when what `surf` shows was asked for (a later answer wins)
    };

    const PageKey& key_of(const core::Doc& d, FlowCache& flows, const std::string& id);
    void           store(Entry& e, Surface s, const PageKey& key, const std::string& variant, int pw, int ph);
    void           trim();
    void           run();   // the worker

    bool                                   m_threaded = false;
    std::size_t                            m_cap;
    std::size_t                            m_bytes = 0;
    std::map<std::string, Entry>           m_entries;   // by slot
    std::map<std::string, PageKey>         m_keys;      // this revision's, by id
    std::uint64_t                          m_keys_rev = ~0ull;
    std::uint64_t                          m_tick = 0;
    std::vector<Job>                       m_asks;      // not yet handed over
    std::uint64_t                          m_asks_rev = ~0ull;
    std::shared_ptr<Snap>                  m_snap;      // the last one made
    std::uint64_t                          m_snap_rev = ~0ull;
    std::uint64_t                          m_gen = 1;   // forget() moves it
    int                                    m_snapshots = 0;
    int                                    m_sync_renders = 0;

    // ── shared with the worker (under m_mx) ──
    mutable std::mutex                     m_mx;
    std::condition_variable                m_cv, m_idle_cv;
    std::deque<Job>                        m_queue;
    std::vector<Done>                      m_done;
    std::function<void()>                  m_notify;
    bool                                   m_stop = false, m_held = false, m_busy = false;
    int                                    m_worker_renders = 0;
    std::thread                            m_worker;
};

}  // namespace composix::layout
