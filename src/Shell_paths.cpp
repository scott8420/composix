// Shell_paths.cpp -- s214: the Pathfinder (unite / subtract / intersect /
// exclude the picked shapes); s215: Offset Path (a window: how far, which
// corners, keep the original) and Outline Stroke (a line's stroke made a
// filled shape). The work is core::doc (core/Pathfinder.hpp).
#include "Shell.hpp"
#include "Log.hpp"
#include "core/Pathfinder.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

namespace composix {

void Shell::bind_path_actions() {  // bindings
    auto picked = [this] { return m_canvas_view.canvas().selection(); };
    const auto show = [this](const std::vector<std::string>& ids) {
        m_canvas_view.canvas().model_changed();
        m_canvas_view.canvas().set_selection(ids);
        on_canvas_selection();
    };
    add_action_with_parameter("path-op", Glib::Variant<Glib::ustring>::variant_type(), [this, picked, show](const Glib::VariantBase& p) {
        const int k = std::stoi(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get());
        const auto id = core::doc::path_op(m_journal, m_doc, picked(), static_cast<core::doc::PathOp>(std::clamp(k, 0, 3)));
        if (!id.empty()) show({id});   // two closed shapes on one page, overlapping as the op needs -- else nothing
    });
    add_action("offset-path", [this] { on_offset_path(); });
    add_action("step-repeat", [this] { on_step_repeat(); });   // s284
    add_action("outline-stroke", [this, picked, show] {   // the open paths among the picks: their stroke, as a shape
        std::vector<std::string> lines;
        for (const auto& id : picked())
            if (const auto* f = m_doc.as<core::Frame>(id);
                f && f->shape == core::ShapeKind::Path && f->open && m_doc.as<core::Swatch>(f->stroke) && f->stroke_w > 0)
                lines.push_back(id);
        std::vector<std::string> made;
        for (const auto& id : lines) {   // half the weight out each side, its ends as its caps
            const double w = m_doc.as<core::Frame>(id)->stroke_w;
            const int join = m_doc.as<core::Frame>(id)->join;
            const auto r = core::doc::offset_paths(m_journal, m_doc, {id}, w / 2, join, false);
            made.insert(made.end(), r.begin(), r.end());
        }
        if (!made.empty()) show(made);
    });
}

void Shell::on_offset_path() {  // handler
    if (!m_offset) {
        m_offset = std::make_unique<Gtk::Window>();
        auto& w = *m_offset;
        w.set_name("offset");
        w.set_title("Offset Path");
        w.set_modal(true);
        w.set_resizable(false);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        box->set_margin(18);
        auto row = [](const char* label, Gtk::Widget& wd) {
            auto r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            auto l = Gtk::make_managed<Gtk::Label>(label);
            l->add_css_class("cx-field-label");
            l->set_xalign(0);
            l->set_size_request(90, -1);
            r->append(*l);
            wd.set_hexpand(true);
            r->append(wd);
            return r;
        };
        auto dist = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(6, -500, 500, 0.5, 6), 1, 2);
        dist->set_name("offset.distance");
        dist->set_tooltip_text("Points out from the outline (below 0: in)");
        auto joins = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Mitre", "Round", "Bevel"}));
        joins->set_name("offset.joins");
        joins->set_selected(1);
        auto keep = Gtk::make_managed<Gtk::CheckButton>("Keep the original");
        keep->set_name("offset.keep");
        keep->set_active(true);
        box->append(*row("Offset (pt)", *dist));
        box->append(*row("Corners", *joins));
        box->append(*keep);
        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);
        buttons->set_margin_top(6);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        auto ok = Gtk::make_managed<Gtk::Button>("Offset");
        ok->set_name("offset.ok");
        ok->add_css_class("suggested-action");
        buttons->append(*cancel);
        buttons->append(*ok);
        box->append(*buttons);
        w.set_child(*box);
        cancel->signal_clicked().connect([this] { m_offset->set_visible(false); });
        ok->signal_clicked().connect([this, dist, joins, keep] {
            static const int kJoin[] = {0, 1, 2};
            const auto made = core::doc::offset_paths(m_journal, m_doc, m_canvas_view.canvas().selection(), dist->get_value(),
                                                      kJoin[std::min(2u, joins->get_selected())], keep->get_active());
            m_offset->set_visible(false);
            if (made.empty()) return;
            m_canvas_view.canvas().model_changed();
            m_canvas_view.canvas().set_selection(made);
            on_canvas_selection();
        });
    }
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) m_offset->set_transient_for(*top);
    m_offset->present();
}

// s284 (X1): Step and Repeat -- how many, and how far each step goes (the
// document's unit; across, down), as InDesign's.
void Shell::on_step_repeat() {  // handler
    if (picked_frames().empty()) return;
    const double U = core::unit_pt(m_doc.settings().units);
    const std::string un = core::unit_name(m_doc.settings().units);
    if (!m_step) {
        m_step = std::make_unique<Gtk::Window>();
        auto& w = *m_step;
        w.set_name("step");
        w.set_title("Step and Repeat");
        w.set_modal(true);
        w.set_resizable(false);
        w.set_hide_on_close(true);
    }
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(18);
    auto row = [](const std::string& label, Gtk::Widget& wd) {
        auto r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto l = Gtk::make_managed<Gtk::Label>(label);
        l->add_css_class("cx-field-label");
        l->set_xalign(0);
        l->set_size_request(120, -1);
        r->append(*l);
        wd.set_hexpand(true);
        r->append(wd);
        return r;
    };
    const auto it = m_settings.find("step.count");
    const int count0 = it == m_settings.end() ? 3 : core::setting_int(m_settings, "step.count", 3, 1, 500);
    const double across0 = m_settings.count("step.dx") ? std::atof(m_settings["step.dx"].c_str()) : 0.25 * 72;
    const double down0 = m_settings.count("step.dy") ? std::atof(m_settings["step.dy"].c_str()) : 0.25 * 72;
    auto count = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(count0, 1, 500, 1, 5), 1, 0);
    count->set_name("step.count");
    const int digits = U >= 72 ? 3 : U >= 12 ? 2 : 1;
    auto across = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(across0 / U, -5000 / U, 5000 / U, 1.0 / (U >= 72 ? 16 : 1), 1), 1, digits);
    across->set_name("step.across");
    auto down = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(down0 / U, -5000 / U, 5000 / U, 1.0 / (U >= 72 ? 16 : 1), 1), 1, digits);
    down->set_name("step.down");
    box->append(*row("Copies", *count));
    box->append(*row("Across (" + un + ")", *across));
    box->append(*row("Down (" + un + ")", *down));
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    buttons->set_margin_top(6);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto ok = Gtk::make_managed<Gtk::Button>("Repeat");
    ok->set_name("step.ok");
    ok->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*ok);
    box->append(*buttons);
    m_step->set_child(*box);
    cancel->signal_clicked().connect([this] { m_step->set_visible(false); });
    ok->signal_clicked().connect([this, count, across, down, U] {
        const int n = static_cast<int>(count->get_value());
        const double dx = across->get_value() * U, dy = down->get_value() * U;
        m_settings["step.count"] = std::to_string(n);
        m_settings["step.dx"] = std::to_string(dx);
        m_settings["step.dy"] = std::to_string(dy);
        core::save_settings(m_settings_file, m_settings);
        const auto made = core::doc::step_repeat(m_journal, m_doc, picked_frames(), n, dx, dy);
        m_step->set_visible(false);
        if (auto lg = log::get(log::Area::Shell)) lg->info("step and repeat: {} x ({}, {}) -> {} frame(s)", n, dx, dy, made.size());
        if (made.empty()) return;
        m_canvas_view.canvas().model_changed();
        m_canvas_view.canvas().set_selection(made);
        on_canvas_selection();
    });
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) m_step->set_transient_for(*top);
    m_step->present();
}

}  // namespace composix
