#include "views/WrapBox.hpp"

#include <algorithm>

namespace composix::views {

WrapBox::WrapBox(int hspacing, int vspacing) : m_hs(hspacing), m_vs(vspacing) {
    set_overflow(Gtk::Overflow::HIDDEN);
}

WrapBox::~WrapBox() { remove_all(); }

void WrapBox::append(Gtk::Widget& child) {
    child.set_parent(*this);
    m_kids.push_back(&child);
    queue_resize();
}

void WrapBox::remove_all() {
    for (auto* k : m_kids) k->unparent();
    m_kids.clear();
    queue_resize();
}

Gtk::SizeRequestMode WrapBox::get_request_mode_vfunc() const { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

namespace {
struct Size { int w, h; };
Size natural(const Gtk::Widget& k) {
    int mn = 0, nat = 0, b1 = -1, b2 = -1, mh = 0, nh = 0;
    k.measure(Gtk::Orientation::HORIZONTAL, -1, mn, nat, b1, b2);
    k.measure(Gtk::Orientation::VERTICAL, nat, mh, nh, b1, b2);
    return {nat, nh};
}
}  // namespace

int WrapBox::height_for(int width) const {
    int x = 0, line = 0, total = 0;
    for (const auto* k : m_kids) {
        if (!k->get_visible()) continue;
        const auto s = natural(*k);
        if (x > 0 && x + s.w > width) {   // a new line
            total += line + m_vs;
            x = 0;
            line = 0;
        }
        x += s.w + m_hs;
        line = std::max(line, s.h);
    }
    return total + line;
}

void WrapBox::measure_vfunc(Gtk::Orientation o, int for_size, int& minimum, int& natural_size, int& min_base, int& nat_base) const {
    min_base = nat_base = -1;
    if (o == Gtk::Orientation::HORIZONTAL) {
        int mn = 0, sum = 0;
        for (const auto* k : m_kids) {
            if (!k->get_visible()) continue;
            const auto s = natural(*k);
            mn = std::max(mn, s.w);
            sum += s.w + m_hs;
        }
        minimum = mn;
        natural_size = std::max(mn, sum - m_hs);
        return;
    }
    const int h = height_for(for_size < 0 ? 1 << 20 : for_size);
    minimum = natural_size = h;
}

void WrapBox::size_allocate_vfunc(int width, int, int) {
    int x = 0, y = 0, line = 0;
    for (auto* k : m_kids) {
        if (!k->get_visible()) continue;
        const auto s = natural(*k);
        if (x > 0 && x + s.w > width) {
            y += line + m_vs;
            x = 0;
            line = 0;
        }
        k->size_allocate(Gtk::Allocation(x, y, std::min(s.w, width), s.h), -1);
        x += s.w + m_hs;
        line = std::max(line, s.h);
    }
}

}  // namespace composix::views
