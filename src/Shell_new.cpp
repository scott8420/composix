#include "Shell.hpp"
#include "Log.hpp"
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <cmath>
#include <memory>

// Shell_new.cpp -- HANDLERS + HELPERS for s019: File > New (the sheet that
// sets up a blank document) and Help > Open Sample Book.

namespace composix {

void Shell::on_new() {  // handler
    confirm_discard([this] { show_new_sheet(); });
}

void Shell::on_sample() {  // handler
    confirm_discard([this] {
        core::Doc fresh;
        core::load_sample(fresh);
        adopt(fresh, "");
        if (auto lg = log::get(log::Area::Shell)) lg->info("opened the sample book");
    });
}

namespace {
// The sheet's widgets, held while it lives (the window owns them).
struct NewForm {
    Gtk::Entry*       title = nullptr;
    Gtk::DropDown*    size = nullptr;
    Gtk::SpinButton*  w = nullptr;
    Gtk::SpinButton*  h = nullptr;
    Gtk::SpinButton*  pages = nullptr;
    Gtk::CheckButton* facing = nullptr;
    Gtk::SpinButton*  top = nullptr;
    Gtk::SpinButton*  bottom = nullptr;
    Gtk::SpinButton*  inside = nullptr;
    Gtk::SpinButton*  outside = nullptr;
    Gtk::Label*       inside_lab = nullptr;
    Gtk::Label*       outside_lab = nullptr;
    Gtk::SpinButton*  columns = nullptr;
    Gtk::SpinButton*  gutter = nullptr;
    Gtk::SpinButton*  bleed = nullptr;
    Gtk::CheckButton* frames = nullptr;
    Gtk::CheckButton* folios = nullptr;
    Gtk::DropDown*    units = nullptr;   // s079: the sheet's (and the document's) unit
    core::Units       u = core::Units::Inches;
    Gtk::Label*       wh_unit = nullptr;
    Gtk::Label*       bleed_lab = nullptr;
    Gtk::Label*       note = nullptr;
    Gtk::Button*      go = nullptr;
    bool              syncing = false;

    core::NewDoc read() const {
        const double in = core::unit_pt(u);   // s079: the boxes are in the sheet's unit
        core::NewDoc n;
        n.title = title->get_text();
        n.trim_w = w->get_value() * in;
        n.trim_h = h->get_value() * in;
        n.pages = pages->get_value_as_int();
        n.facing = facing->get_active();
        n.margins = {top->get_value() * in, bottom->get_value() * in, inside->get_value() * in, outside->get_value() * in};
        n.columns = columns->get_value_as_int();
        n.gutter = gutter->get_value();
        n.bleed = bleed->get_value() * in;
        n.text_frames = frames->get_active();
        n.folios = folios->get_active();
        n.units = u;
        return n;
    }
    // s079: the length boxes in unit `to` -- values kept as lengths, ranges,
    // steps and decimals to suit it.
    void set_unit(core::Units to) {
        const double from_pt = core::unit_pt(u), to_pt = core::unit_pt(to);
        struct B { Gtk::SpinButton* s; double lo_in, hi_in; };
        const B boxes[] = {{w, 1, 50}, {h, 1, 50}, {top, 0, 10}, {bottom, 0, 10}, {inside, 0, 10}, {outside, 0, 10}, {bleed, 0, 1}};
        double step = 0.125;
        int digits = 3;
        switch (to) {
        case core::Units::Millimetres: step = 1; digits = 2; break;
        case core::Units::Picas:       step = 1; digits = 2; break;
        case core::Units::Points:      step = 1; digits = 1; break;
        default: break;
        }
        for (const auto& b : boxes) {
            const double pts = b.s->get_value() * from_pt;
            const double st = (b.s == bleed && to == core::Units::Inches) ? 0.0625 : step;
            b.s->set_digits(static_cast<guint>(b.s == bleed && to == core::Units::Inches ? 4 : digits));
            b.s->set_adjustment(Gtk::Adjustment::create(pts / to_pt, b.lo_in * core::kPt / to_pt, b.hi_in * core::kPt / to_pt, st, st * 4));
        }
        u = to;
        if (wh_unit) wh_unit->set_text(core::unit_name(to));
        if (bleed_lab) bleed_lab->set_text("Bleed (" + core::unit_name(to) + ")");
    }
};

Gtk::SpinButton* spin(const std::string& name, double lo, double hi, double step, int digits, double v) {
    auto s = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(v, lo, hi, step, step * 4), step, digits);
    s->set_name(name);
    s->set_numeric(true);
    s->set_width_chars(6);
    return s;
}

Gtk::Label* label(const std::string& text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->add_css_class("cx-field-label");
    l->set_xalign(1);
    return l;
}
}  // namespace

// The sheet: name, size (book trims or custom), pages, facing, margins,
// columns, bleed, a text frame on every page. Create makes it; the choices
// stay for the session.
void Shell::show_new_sheet() {  // helper
    const double in = core::kPt;
    const auto& presets = core::trim_presets();
    m_new = std::make_unique<Gtk::Window>();
    auto& win = *m_new;
    win.set_name("new");
    win.set_title("New Document");
    win.set_modal(true);
    win.set_resizable(false);
    win.set_hide_on_close(true);
    auto f = std::make_shared<NewForm>();

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_name("new.box");
    box->add_css_class("cx-export");
    box->set_margin(18);
    auto head = Gtk::make_managed<Gtk::Label>("New Document");
    head->set_name("new.title_label");
    head->add_css_class("cx-export-title");
    head->set_xalign(0);
    box->append(*head);

    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_name("new.grid");
    grid->set_row_spacing(8);
    grid->set_column_spacing(8);
    int row = 0;
    const auto put = [&](const std::string& text, Gtk::Widget& w, int span = 3) {
        grid->attach(*label(text), 0, row);
        grid->attach(w, 1, row, span);
        ++row;
    };

    f->title = Gtk::make_managed<Gtk::Entry>();
    f->title->set_name("new.name");
    f->title->set_text(m_newdoc.title);
    f->title->set_hexpand(true);
    put("Name", *f->title);

    std::vector<Glib::ustring> names;
    for (const auto& p : presets) names.push_back(p.label);
    names.push_back("Custom");
    f->size = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
    f->size->set_name("new.size");
    put("Size", *f->size);

    f->w = spin("new.width", 1, 50, 0.125, 3, m_newdoc.trim_w / in);
    f->h = spin("new.height", 1, 50, 0.125, 3, m_newdoc.trim_h / in);
    {
        auto wh = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        wh->set_name("new.wh");
        auto x = Gtk::make_managed<Gtk::Label>("×");
        auto unit = Gtk::make_managed<Gtk::Label>("in");
        unit->add_css_class("cx-field-label");
        f->wh_unit = unit;
        wh->append(*f->w);
        wh->append(*x);
        wh->append(*f->h);
        wh->append(*unit);
        put("W × H", *wh);
    }

    f->pages = spin("new.pages", 1, 2000, 1, 0, m_newdoc.pages);
    f->facing = Gtk::make_managed<Gtk::CheckButton>("Facing pages");
    f->facing->set_name("new.facing");
    f->facing->set_active(m_newdoc.facing);
    {
        auto pr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        pr->append(*f->pages);
        pr->append(*f->facing);
        put("Pages", *pr);
    }

    // Margins: two rows of two; inside / outside read left / right unfaced.
    const auto& mg = m_newdoc.margins;
    f->top = spin("new.top", 0, 10, 0.125, 3, mg.top / in);
    f->bottom = spin("new.bottom", 0, 10, 0.125, 3, mg.bottom / in);
    f->inside = spin("new.inside", 0, 10, 0.125, 3, mg.inside / in);
    f->outside = spin("new.outside", 0, 10, 0.125, 3, mg.outside / in);
    f->inside_lab = label("Inside");
    f->outside_lab = label("Outside");
    grid->attach(*label("Top"), 0, row);
    grid->attach(*f->top, 1, row);
    grid->attach(*label("Bottom"), 2, row);
    grid->attach(*f->bottom, 3, row);
    ++row;
    grid->attach(*f->inside_lab, 0, row);
    grid->attach(*f->inside, 1, row);
    grid->attach(*f->outside_lab, 2, row);
    grid->attach(*f->outside, 3, row);
    ++row;

    f->columns = spin("new.columns", 1, 12, 1, 0, m_newdoc.columns);
    f->gutter = spin("new.gutter", 0, 72, 1, 1, m_newdoc.gutter);
    grid->attach(*label("Columns"), 0, row);
    grid->attach(*f->columns, 1, row);
    grid->attach(*label("Gutter (pt)"), 2, row);
    grid->attach(*f->gutter, 3, row);
    ++row;

    f->bleed = spin("new.bleed", 0, 1, 0.0625, 4, m_newdoc.bleed / in);
    f->bleed_lab = label("Bleed (in)");
    grid->attach(*f->bleed_lab, 0, row);
    grid->attach(*f->bleed, 1, row);
    {   // s079: the unit -- the boxes above follow it, and so does the document.
        f->units = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Inches", "Millimetres", "Picas", "Points"}));
        f->units->set_name("new.units");
        grid->attach(*label("Units"), 2, row);
        grid->attach(*f->units, 3, row);
    }
    ++row;

    f->frames = Gtk::make_managed<Gtk::CheckButton>("A text frame on every page, threaded");
    f->frames->set_name("new.frames");
    f->frames->set_active(m_newdoc.text_frames);
    grid->attach(*f->frames, 1, row, 3);
    ++row;
    f->folios = Gtk::make_managed<Gtk::CheckButton>("Page numbers at the foot (on the master)");
    f->folios->set_name("new.folios");
    f->folios->set_active(m_newdoc.folios);
    grid->attach(*f->folios, 1, row, 3);
    ++row;
    box->append(*grid);

    f->note = Gtk::make_managed<Gtk::Label>();
    f->note->set_name("new.note");
    f->note->add_css_class("cx-export-note");
    f->note->set_xalign(0);
    f->note->set_wrap(true);
    f->note->set_max_width_chars(44);
    box->append(*f->note);

    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->set_name("new.cancel");
    f->go = Gtk::make_managed<Gtk::Button>("Create");
    f->go->set_name("new.create");
    f->go->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*f->go);
    box->append(*buttons);
    win.set_child(*box);
    win.set_default_widget(*f->go);

    // Live: the preset follows W x H (Custom when none matches); the note
    // says what the document will be, or why it can't be.
    const auto check = [f] {
        const auto n = f->read();
        const auto why = core::check_new(n);
        f->go->set_sensitive(why.empty());
        if (!why.empty()) { f->note->set_text(why); return; }
        const double lw = n.trim_w - n.margins.inside - n.margins.outside;
        const double lh = n.trim_h - n.margins.top - n.margins.bottom;
        f->note->set_text(std::to_string(n.pages) + (n.pages == 1 ? " page, " : " pages, ") +
                          core::size_text(n.trim_w, n.trim_h, 0, n.units) + "; text area " + core::size_text(lw, lh, 0, n.units) + ".");
        f->inside_lab->set_text(n.facing ? "Inside" : "Left");
        f->outside_lab->set_text(n.facing ? "Outside" : "Right");
    };
    const auto match_preset = [f, presets, check] {
        if (f->syncing) return;
        f->syncing = true;
        guint at = static_cast<guint>(presets.size());   // Custom
        for (std::size_t i = 0; i < presets.size(); ++i)
            if (std::abs(presets[i].w - f->w->get_value() * core::unit_pt(f->u)) < 0.15 &&
                std::abs(presets[i].h - f->h->get_value() * core::unit_pt(f->u)) < 0.15) at = static_cast<guint>(i);
        f->size->set_selected(at);
        f->syncing = false;
        check();
    };
    f->size->property_selected().signal_changed().connect([f, presets, check] {
        if (f->syncing) return;
        const auto i = f->size->get_selected();
        if (i < presets.size()) {
            f->syncing = true;
            f->w->set_value(presets[i].w / core::unit_pt(f->u));
            f->h->set_value(presets[i].h / core::unit_pt(f->u));
            f->syncing = false;
        }
        check();
    });
    // (Reconnected when the unit changes: set_adjustment keeps the button's
    // own value-changed signal, so these hold.)
    f->w->signal_value_changed().connect(match_preset);
    f->h->signal_value_changed().connect(match_preset);
    for (auto* s : {f->pages, f->top, f->bottom, f->inside, f->outside, f->columns, f->gutter, f->bleed})
        s->signal_value_changed().connect(check);
    // s079: the remembered unit, then any change of it converts the boxes.
    f->set_unit(m_newdoc.units);
    f->units->set_selected(static_cast<guint>(m_newdoc.units));
    f->units->property_selected().signal_changed().connect([f, match_preset] {
        f->syncing = true;
        f->set_unit(static_cast<core::Units>(std::min(3u, f->units->get_selected())));
        f->syncing = false;
        match_preset();
    });
    f->facing->signal_toggled().connect(check);
    f->title->signal_changed().connect(check);
    match_preset();

    cancel->signal_clicked().connect([this] { m_new->set_visible(false); });
    auto keys = Gtk::EventControllerKey::create();   // Esc closes the sheet
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
        if (kv != GDK_KEY_Escape) return false;
        m_new->set_visible(false);
        return true;
    }, false);
    win.add_controller(keys);
    f->go->signal_clicked().connect([this, f] {
        const auto n = f->read();
        core::Doc fresh;
        if (!core::new_document(fresh, n)) return;   // the note already says why
        m_newdoc = n;
        // s079: the choices kept for next time (not the name).
        core::new_to_settings(n, m_settings);
        if (!m_settings_file.empty()) core::save_settings(m_settings_file, m_settings);
        m_new->set_visible(false);
        adopt(fresh, "");
        if (auto lg = log::get(log::Area::Shell))
            lg->info("new document '{}': {} page(s), {} x {} pt", n.title, n.pages, n.trim_w, n.trim_h);
    });
    win.set_transient_for(*this);
    win.present();
    f->title->grab_focus();
}

}  // namespace composix
