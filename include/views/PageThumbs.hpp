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
#include "core/Doc.hpp"
#include "layout/Export.hpp"
#include "layout/Flow.hpp"

#include <cairomm/context.h>
#include <cairomm/surface.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace composix::views {

class PageThumbs {
public:
    // `resolve` turns an asset into the file to read (the project folder
    // is the shell's to know).
    PageThumbs(const core::Doc& d, std::function<std::string(const core::Asset&)> resolve);

    // Draw page / master `id` into the paper at x, y, w, h (user units of
    // `cr`). False if `id` isn't a page or master.
    bool paint(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id, double x, double y, double w, double h,
               bool large);
    // Another document (or folder): nothing kept carries over.
    void forget();
    // s101: flow through this cache (the shell's, shared with the canvas).
    void share_flows(layout::FlowCache& f) { m_flowp = &f; }
    int renders() const { return m_renders; }   // how many times a page was drawn (tests, logs)

private:
    struct Entry {
        layout::PageKey                    key;
        Cairo::RefPtr<Cairo::ImageSurface> surf;
        int                                pw = 0, ph = 0;
    };
    const layout::PageKey& key_of(const std::string& id);
    Cairo::RefPtr<Cairo::ImageSurface> render(const std::string& id, double pt_w, double pt_h, int pw, int ph, bool large);

    const core::Doc&                                 m_doc;
    std::function<std::string(const core::Asset&)>  m_resolve;
    layout::FlowCache                                m_own_flows;
    layout::FlowCache*                               m_flowp = &m_own_flows;   // s101: the shell's, shared
    std::map<std::string, Entry>                     m_cache;   // id ("|L" for the preview's size)
    std::map<std::string, layout::PageKey>           m_keys;    // this revision's
    std::uint64_t                                    m_keys_rev = ~0ull;
    int                                              m_renders = 0;
};

}  // namespace composix::views
