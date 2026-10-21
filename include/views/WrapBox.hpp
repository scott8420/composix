#pragma once
// views::WrapBox -- children left to right, wrapping to a new line when the
// width runs out; each line as tall as its tallest child, children aligned to
// the line's top (s277: the navigator's spreads -- GtkFlowBox lines its
// children up in columns, so one wide foldout made every line one spread).
#include <gtkmm/widget.h>

#include <vector>

namespace composix::views {

class WrapBox : public Gtk::Widget {
public:
    explicit WrapBox(int hspacing = 2, int vspacing = 4);
    ~WrapBox() override;

    void append(Gtk::Widget& child);
    void remove_all();
    const std::vector<Gtk::Widget*>& children() const { return m_kids; }

protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation o, int for_size, int& minimum, int& natural, int& min_base, int& nat_base) const override;
    void size_allocate_vfunc(int width, int height, int baseline) override;

private:
    int height_for(int width) const;

    std::vector<Gtk::Widget*> m_kids;
    int m_hs, m_vs;
};

}  // namespace composix::views
