#pragma once
// views::paint_thumb -- one painter for every thumbnail the shell draws: the
// tile grid, the item preview. The core says WHAT a thumb is (core::Thumb); this
// says how it looks. Paper is paper-white in both appearances (a page is a
// physical object); everything else takes the widget's foreground so it follows
// light / dark.
#include "core/Outline.hpp"

#include <cairomm/context.h>
#include <gtkmm/widget.h>

#include <sigc++/signal.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace composix::views {

// Paint `t` centred in a w x h box. `large` adds detail (guides, panel labels)
// for the preview; tiles leave it off.
void paint_thumb(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& w,
                 int width, int height, const core::Thumb& t, bool large = false);

// s028: a spread -- its pages side by side at the spine, fitted together.
void paint_spread(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& w, int width, int height,
                  const std::vector<core::Thumb>& pages);

// s020: who draws a page's real contents into its paper (x, y, w, h -- the
// paper, device units). The shell sets one (PageThumbs); false = not drawn,
// so the stand-in type bars show. `large`: the preview (margin guides too).
using PagePainter = std::function<bool(const Cairo::RefPtr<Cairo::Context>& cr, const std::string& id,
                                       double x, double y, double w, double h, bool large)>;
void set_page_painter(PagePainter p);

// s103: the pages whose pictures just came back from the worker; every widget
// that painted one of them (paint_thumb / paint_spread) is drawn again.
using ThumbsReady = sigc::signal<void(const std::set<std::string>&)>;
ThumbsReady& thumbs_ready();
// Draw `wd` again when page `id` gets a newer picture (paint_thumb does it).
void watch_page(Gtk::Widget& wd, const std::string& id);


}  // namespace composix::views
