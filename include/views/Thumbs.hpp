#pragma once
// views::paint_thumb -- one painter for every thumbnail the shell draws: the
// tile grid, the item preview. The core says WHAT a thumb is (core::Thumb); this
// says how it looks. Paper is paper-white in both appearances (a page is a
// physical object); everything else takes the widget's foreground so it follows
// light / dark.
#include "core/Outline.hpp"

#include <cairomm/context.h>
#include <gtkmm/widget.h>

namespace composix::views {

// Paint `t` centred in a w x h box. `large` adds detail (guides, panel labels)
// for the preview; tiles leave it off.
void paint_thumb(const Cairo::RefPtr<Cairo::Context>& cr, Gtk::Widget& w,
                 int width, int height, const core::Thumb& t, bool large = false);


}  // namespace composix::views
