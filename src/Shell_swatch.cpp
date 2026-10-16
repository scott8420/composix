#include "Shell.hpp"
#include "Log.hpp"
#include "layout/ColorReport.hpp"

#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/stringlist.h>
#include <glibmm/main.h>

#include <cmath>
#include <cstdio>

// Shell_swatch.cpp -- s058: the swatch editor. A sheet: the swatch's name,
// its model (CMYK / RGB / Gray / Spot), a slider and a typed value for each
// part, hex for RGB, the ink's name for a spot, and a big live chip. An RGB
// swatch also shows what it becomes on the press (plate::Proof through the
// export's profile, else the first found) and how far it moves. Save is
// one step (core::doc::add_swatch / edit_swatch); everything using the
// swatch follows.

namespace composix {

namespace {
std::string hex_of(std::uint32_t c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%06X", c & 0xFFFFFF);
    return b;
}
bool parse_hex(std::string t, std::uint32_t& out) {
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() == 3) t = {t[0], t[0], t[1], t[1], t[2], t[2]};
    if (t.size() != 6) return false;
    for (char c : t) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<std::uint32_t>(std::stoul(t, nullptr, 16));
    return true;
}
const char* const kParts[4][4] = {{"C", "M", "Y", "K"}, {"R", "G", "B", ""}, {"Gray", "", "", ""}, {"C", "M", "Y", "K"}};
int parts_of(core::ColorModel m) { return m == core::ColorModel::RGB ? 3 : m == core::ColorModel::Gray ? 1 : 4; }
// The dropdown's order.
const core::ColorModel kModels[] = {core::ColorModel::CMYK, core::ColorModel::RGB, core::ColorModel::Gray, core::ColorModel::Spot};
int model_index(core::ColorModel m) {
    for (int i = 0; i < 4; ++i) if (kModels[i] == m) return i;
    return 0;
}
}  // namespace

void Shell::open_swatch_editor(const std::string& id) {  // helper
    if (!m_swed) {
        m_swed = std::make_unique<Gtk::Window>();
        auto& w = *m_swed;
        w.set_name("swatch");
        w.set_title("Swatch");
        w.set_modal(true);
        w.set_resizable(false);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        box->set_name("swatch.box");
        box->add_css_class("cx-export");
        box->set_margin(18);
        m_sw_title = Gtk::make_managed<Gtk::Label>("Swatch");
        m_sw_title->add_css_class("cx-export-title");
        m_sw_title->set_xalign(0);
        box->append(*m_sw_title);

        // The chip: the colour, big -- and for RGB, the press's version beside it.
        m_sw_chip = Gtk::make_managed<Gtk::DrawingArea>();
        m_sw_chip->set_name("swatch.chip");
        m_sw_chip->set_content_width(320);
        m_sw_chip->set_content_height(96);
        m_sw_chip->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int cw, int ch) {
            const auto fill = [&](std::uint32_t c, double x, double wd) {
                cr->rectangle(x + 0.5, 0.5, wd - 1, ch - 1);
                cr->set_source_rgb(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
                cr->fill_preserve();
                cr->set_source_rgba(0, 0, 0, 0.18);
                cr->set_line_width(1);
                cr->stroke();
            };
            const bool press = m_sw.model == core::ColorModel::RGB && m_sw_proof;
            if (!press) { fill(m_sw.preview, 0, cw); return; }
            fill(m_sw.preview, 0, cw / 2.0);
            fill(m_sw_after, cw / 2.0, cw / 2.0);
        });
        box->append(*m_sw_chip);
        m_sw_press = Gtk::make_managed<Gtk::Label>();
        m_sw_press->set_name("swatch.press");
        m_sw_press->add_css_class("cx-export-note");
        m_sw_press->set_xalign(0);
        m_sw_press->set_wrap(true);
        m_sw_press->set_max_width_chars(52);   // wraps; the sheet keeps its width
        box->append(*m_sw_press);

        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(6);
        grid->set_column_spacing(10);
        int row = 0;
        const auto label = [&](const char* t) {
            auto l = Gtk::make_managed<Gtk::Label>(t);
            l->add_css_class("cx-field-label");
            l->set_xalign(1);
            return l;
        };
        m_sw_name = Gtk::make_managed<Gtk::Entry>();
        m_sw_name->set_name("swatch.name");
        m_sw_name->set_hexpand(true);
        grid->attach(*label("Name"), 0, row);
        grid->attach(*m_sw_name, 1, row++, 2);
        m_sw_model = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"CMYK", "RGB", "Gray", "Spot"}));
        m_sw_model->set_name("swatch.model");
        grid->attach(*label("Model"), 0, row);
        grid->attach(*m_sw_model, 1, row++, 2);
        // Spot: the ink's name (its alternate is the CMYK below).
        {
            auto l = label("Ink");
            m_sw_ink = Gtk::make_managed<Gtk::Entry>();
            m_sw_ink->set_name("swatch.ink");
            m_sw_ink->set_placeholder_text("PMS 7593 C");
            grid->attach(*l, 0, row);
            grid->attach(*m_sw_ink, 1, row++, 2);
            m_sw_ink_row = l;
        }
        for (int k = 0; k < 4; ++k) {
            auto l = label("");
            auto sc = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
            sc->set_name("swatch.scale." + std::to_string(k));
            sc->set_range(0, 100);
            sc->set_increments(1, 10);
            sc->set_draw_value(false);
            sc->set_hexpand(true);
            sc->set_size_request(200, -1);
            auto sp = Gtk::make_managed<Gtk::SpinButton>();
            sp->set_name("swatch.value." + std::to_string(k));
            sp->set_range(0, 100);
            sp->set_increments(1, 10);
            sp->set_digits(0);
            sp->set_width_chars(4);
            grid->attach(*l, 0, row);
            grid->attach(*sc, 1, row);
            grid->attach(*sp, 2, row++);
            m_sw_labels.push_back(l);
            m_sw_scales.push_back(sc);
            m_sw_spins.push_back(sp);
            m_sw_rows.push_back(l);
            const auto set = [this, k](double v) {
                if (m_sw_quiet) return;
                m_sw.v[static_cast<std::size_t>(k)] = v;
                swatch_editor_sync();
            };
            sc->signal_value_changed().connect([sc, set] { set(std::round(sc->get_value())); });
            sp->signal_value_changed().connect([sp, set] { set(sp->get_value()); });
        }
        {
            auto l = label("Hex");
            m_sw_hex = Gtk::make_managed<Gtk::Entry>();
            m_sw_hex->set_name("swatch.hex");
            m_sw_hex->set_width_chars(9);
            grid->attach(*l, 0, row);
            grid->attach(*m_sw_hex, 1, row++, 2);
            m_sw_hex_row = l;
            m_sw_hex->signal_changed().connect([this] {
                if (m_sw_quiet) return;
                std::uint32_t c = 0;
                if (!parse_hex(m_sw_hex->get_text(), c)) { m_sw_hex->add_css_class("error"); return; }
                m_sw_hex->remove_css_class("error");
                core::doc::swatch_from_rgb(m_sw, c);
                m_sw_hex_typing = true;   // what's being typed stays as typed
                swatch_editor_sync();
                m_sw_hex_typing = false;
            });
        }
        box->append(*grid);
        m_sw_model->property_selected().signal_changed().connect([this] {
            if (m_sw_quiet) return;
            // Another model: the same colour, as near as that model has it.
            const auto was = core::doc::swatch_preview(m_sw);
            m_sw.model = kModels[std::min(3u, m_sw_model->get_selected())];
            core::doc::swatch_from_rgb(m_sw, was);
            if (m_sw.model == core::ColorModel::Spot && m_sw.spot.empty()) m_sw.spot = m_sw.name;
            swatch_editor_sync();
        });
        m_sw_name->signal_changed().connect([this] { if (!m_sw_quiet) m_sw.name = m_sw_name->get_text(); });
        m_sw_ink->signal_changed().connect([this] { if (!m_sw_quiet) m_sw.spot = m_sw_ink->get_text(); });

        auto brow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        brow->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->set_name("swatch.cancel");
        m_sw_save = Gtk::make_managed<Gtk::Button>("Save");
        m_sw_save->set_name("swatch.save");
        m_sw_save->add_css_class("suggested-action");
        brow->append(*cancel);
        brow->append(*m_sw_save);
        box->append(*brow);
        w.set_child(*box);
        w.set_default_widget(*m_sw_save);
        cancel->signal_clicked().connect([this] { m_swed->set_visible(false); });
        m_sw_save->signal_clicked().connect([this] {
            if (m_sw.name.empty()) { m_sw_name->add_css_class("error"); return; }
            if (m_sw_id.empty()) {
                const auto made = core::doc::add_swatch(m_journal, m_doc, m_sw);
                if (!made.empty())
                    Glib::signal_idle().connect_once([this, made] { m_tiles.show_section(m_outline, core::Segment::Colors, {made}); on_tiles_selection({made}); });
            } else {
                core::doc::edit_swatch(m_journal, m_doc, m_sw_id, m_sw);
            }
            if (auto lg = log::get(log::Area::Shell)) lg->info("swatch: {} '{}'", m_sw_id.empty() ? "new" : "edited", m_sw.name);
            m_swed->set_visible(false);
        });
    }
    // The swatch, or a new one (a mid grey-blue in CMYK to start).
    m_sw_id = id;
    if (const auto* s = m_doc.as<core::Swatch>(id)) m_sw = *s;
    else { m_sw_id.clear(); m_sw = core::Swatch{"New Swatch", 0, core::ColorModel::CMYK, {60, 30, 0, 10}}; }
    // The press: the export's profile, else the first CMYK one found.
    m_sw_proof.reset();
    {
        std::string path = m_pdf_profile;
        if (path.empty())
            for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) { path = p.path; break; }
        if (!path.empty()) {
            auto pr = std::make_unique<plate::Proof>();
            if (pr->load(path)) m_sw_proof = std::move(pr);
        }
    }
    m_sw_title->set_text(m_sw_id.empty() ? "New Swatch" : "Swatch — " + m_sw.name);
    m_sw_save->set_label(m_sw_id.empty() ? "Create" : "Save");
    m_sw_name->remove_css_class("error");
    swatch_editor_sync();
    m_swed->set_transient_for(*this);
    m_swed->present();
    m_sw_name->grab_focus();
}

void Shell::swatch_editor_sync() {  // helper
    m_sw_quiet = true;
    // The screen colour: the swatch's own until its values change.
    {
        const auto* cur = m_doc.as<core::Swatch>(m_sw_id);
        m_sw.preview = (cur && cur->model == m_sw.model && cur->v == m_sw.v) ? cur->preview : core::doc::swatch_preview(m_sw);
    }
    const int mi = model_index(m_sw.model);
    if (m_sw_model->get_selected() != static_cast<guint>(mi)) m_sw_model->set_selected(static_cast<guint>(mi));
    if (std::string(m_sw_name->get_text()) != m_sw.name) m_sw_name->set_text(m_sw.name);
    const bool spot = m_sw.model == core::ColorModel::Spot, rgb = m_sw.model == core::ColorModel::RGB;
    m_sw_ink_row->set_visible(spot);
    m_sw_ink->set_visible(spot);
    if (spot && std::string(m_sw_ink->get_text()) != m_sw.spot) m_sw_ink->set_text(m_sw.spot);
    m_sw_hex_row->set_visible(rgb);
    m_sw_hex->set_visible(rgb);
    if (rgb && !m_sw_hex_typing) m_sw_hex->set_text(hex_of(m_sw.preview));
    const int n = parts_of(m_sw.model);
    const double top = rgb ? 255 : 100;
    for (int k = 0; k < 4; ++k) {
        const bool on = k < n;
        m_sw_labels[static_cast<std::size_t>(k)]->set_visible(on);
        m_sw_scales[static_cast<std::size_t>(k)]->set_visible(on);
        m_sw_spins[static_cast<std::size_t>(k)]->set_visible(on);
        if (!on) continue;
        m_sw_labels[static_cast<std::size_t>(k)]->set_text(kParts[mi][k]);
        auto* sc = m_sw_scales[static_cast<std::size_t>(k)];
        auto* sp = m_sw_spins[static_cast<std::size_t>(k)];
        sc->set_range(0, top);
        sp->set_range(0, top);
        const double v = std::clamp(m_sw.v[static_cast<std::size_t>(k)], 0.0, top);
        if (sc->get_value() != v) sc->set_value(v);
        if (sp->get_value() != v) sp->set_value(v);
    }
    // RGB: what the press makes of it.
    if (rgb && m_sw_proof) {
        const auto c = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
        const auto r = m_sw_proof->check(c(m_sw.v[0]), c(m_sw.v[1]), c(m_sw.v[2]));
        m_sw_after = (std::uint32_t{r.after[0]} << 16) | (std::uint32_t{r.after[1]} << 8) | r.after[2];
        char t[200];
        std::snprintf(t, sizeof t, "Left: on screen. Right: on the press (%s) — C%.0f M%.0f Y%.0f K%.0f, %s (ΔE %.1f).",
                      m_sw_proof->name().c_str(), r.cmyk[0], r.cmyk[1], r.cmyk[2], r.cmyk[3],
                      layout::shift_text(layout::shift_of(r.delta_e)), r.delta_e);
        m_sw_press->set_text(t);
        m_sw_press->set_visible(true);
    } else if (rgb) {
        m_sw_press->set_text("RGB converts at output. Install a CMYK press profile to see what it becomes.");
        m_sw_press->set_visible(true);
    } else if (spot) {
        m_sw_press->set_text("A spot colour prints on its own plate; the CMYK below is how it shows and proofs.");
        m_sw_press->set_visible(true);
    } else {
        m_sw_press->set_visible(false);
    }
    m_sw_chip->queue_draw();
    m_sw_quiet = false;
}

}  // namespace composix
