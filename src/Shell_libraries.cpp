#include "Shell.hpp"
#include "Log.hpp"
#include "core/Swatches.hpp"
#include "views/Images.hpp"

#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/paned.h>
#include <tuple>
#include <gtkmm/popover.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <giomm/liststore.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

// Shell_libraries.cpp -- s172: the Colour Libraries window, on s171's core.
// Left, the libraries: This Document, My Library, the palettes other
// programs keep on the machine (by program), and the files imported (their
// paths in the "colors.imported" setting). Right, the one picked: its
// colours as chips, several pickable (Ctrl / Shift, Ctrl+A). Under them:
// Import... / Export... (the library on show -- its picked colours, else
// all -- as .ase or .gpl), and Remove (My Library's colours; an imported
// library from the list), Add to My Library, Add to Document (one undo
// step, each colour copied in, noting its library). Double-click a chip:
// that one into the document. Not modal; follows the document (refresh()).
// Hide-on-close, built once.

namespace composix {

namespace SW = core::swatches;

namespace {
// A row's key: "doc", "mine", "sys:<path>", "imp:<path>"; "" = a heading.
std::string path_of(const std::string& key) { return key.size() > 4 ? key.substr(4) : std::string{}; }
bool is_sys(const std::string& key) { return key.rfind("sys:", 0) == 0; }
bool is_imp(const std::string& key) { return key.rfind("imp:", 0) == 0; }

std::string value_text(const core::Swatch& s) {
    auto n = [](double v) { return std::to_string(std::lround(v)); };
    switch (s.model) {
    case core::ColorModel::CMYK: return "CMYK " + n(s.v[0]) + " / " + n(s.v[1]) + " / " + n(s.v[2]) + " / " + n(s.v[3]);
    case core::ColorModel::RGB:  return "RGB " + n(s.v[0]) + " / " + n(s.v[1]) + " / " + n(s.v[2]);
    case core::ColorModel::Gray: return "Gray " + n(s.v[0]) + "%";
    case core::ColorModel::Spot: return "Spot ink " + s.spot + " (CMYK " + n(s.v[0]) + " / " + n(s.v[1]) + " / " + n(s.v[2]) + " / " + n(s.v[3]) + ")";
    }
    return {};
}

std::string count_text(std::size_t n) { return n == 1 ? "1 colour" : std::to_string(n) + " colours"; }

std::string tilde(const std::string& p) {
    const std::string home = Glib::get_home_dir();
    return !home.empty() && p.rfind(home, 0) == 0 ? "~" + p.substr(home.size()) : p;
}
}  // namespace

std::string Shell::my_library_file() const {  // helper: XDG path (UI-side resolution)
    return SW::mine_file(Glib::get_user_data_dir());
}

// Colours into My Library (the inspector's button, the window's): merged by
// the document's rule, saved. How many went in; -1 when it couldn't save.
int Shell::add_to_my_library(const std::vector<core::Swatch>& colors) {  // helper
    auto mine = SW::load_mine(my_library_file());
    std::vector<core::Swatch> in = colors;
    for (auto& s : in) { s.pos = 0; s.from.clear(); }
    const int n = SW::merge_into(mine, in);
    if (n > 0 && !SW::save_mine(mine)) {
        alert("Can’t save My Library", "Composix couldn’t write “" + mine.path + "”.");
        return -1;
    }
    if (auto lg = log::get(log::Area::Shell)) lg->info("my library: +{} of {}", n, colors.size());
    return n;
}

void Shell::open_libraries(const std::string& key, const std::vector<std::string>& pick) {  // helper
    if (!m_lib) {
        m_lib = std::make_unique<Gtk::Window>();
        auto& w = *m_lib;
        w.set_name("libraries");
        w.set_title("Colour Libraries");
        w.set_default_size(820, 520);
        w.set_hide_on_close(true);
        w.set_transient_for(*this);

        auto paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
        paned->set_name("libraries.paned");
        paned->set_position(240);
        paned->set_shrink_start_child(false);
        paned->set_resize_start_child(false);

        // Left: the libraries.
        m_lib_list = Gtk::make_managed<Gtk::ListBox>();
        m_lib_list->set_name("libraries.list");
        m_lib_list->add_css_class("navigation-sidebar");
        m_lib_list->set_selection_mode(Gtk::SelectionMode::SINGLE);
        m_lib_list->signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
            if (m_lib_quiet || !row) return;
            const int i = row->get_index();
            if (i < 0 || i >= static_cast<int>(m_lib_keys.size()) || m_lib_keys[static_cast<std::size_t>(i)].empty()) return;
            libraries_show(m_lib_keys[static_cast<std::size_t>(i)]);
        });
        auto lsc = Gtk::make_managed<Gtk::ScrolledWindow>();
        lsc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        lsc->set_size_request(200, -1);
        lsc->set_child(*m_lib_list);
        paned->set_start_child(*lsc);

        // Right: the library on show.
        auto right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        right->set_margin(12);
        m_lib_title = Gtk::make_managed<Gtk::Label>();
        m_lib_title->set_name("libraries.title");
        m_lib_title->add_css_class("title-3");
        m_lib_title->set_xalign(0);
        m_lib_title->set_ellipsize(Pango::EllipsizeMode::END);
        right->append(*m_lib_title);
        m_lib_sub = Gtk::make_managed<Gtk::Label>();
        m_lib_sub->set_name("libraries.sub");
        m_lib_sub->add_css_class("dim-label");
        m_lib_sub->set_xalign(0);
        m_lib_sub->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        right->append(*m_lib_sub);

        m_lib_chips = Gtk::make_managed<Gtk::FlowBox>();
        m_lib_chips->set_name("libraries.chips");
        m_lib_chips->set_selection_mode(Gtk::SelectionMode::MULTIPLE);
        m_lib_chips->set_activate_on_single_click(false);
        m_lib_chips->set_homogeneous(true);
        m_lib_chips->set_valign(Gtk::Align::START);
        m_lib_chips->set_max_children_per_line(40);
        m_lib_chips->set_column_spacing(6);
        m_lib_chips->set_row_spacing(6);
        m_lib_chips->signal_selected_children_changed().connect([this] {
            if (m_lib_quiet) return;
            m_lib_note_is_count = true;   // a new pick: the count again, over any message
            libraries_buttons_sync();
        });
        m_lib_chips->signal_child_activated().connect([this](Gtk::FlowBoxChild* c) {
            // Double-click: that colour into the document.
            if (!c || m_lib_on == "doc") return;
            const int i = c->get_index();
            if (i < 0 || i >= static_cast<int>(m_lib_cur.colors.size())) return;
            // After the click's own selection change, so its message stays.
            Glib::signal_idle().connect_once([this, i] { libraries_add_to_document({static_cast<std::size_t>(i)}); });
        });
        auto csc = Gtk::make_managed<Gtk::ScrolledWindow>();
        csc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        csc->set_vexpand(true);
        csc->set_child(*m_lib_chips);
        right->append(*csc);

        m_lib_note = Gtk::make_managed<Gtk::Label>();
        m_lib_note->set_name("libraries.note");
        m_lib_note->add_css_class("dim-label");
        m_lib_note->set_xalign(0);
        m_lib_note->set_wrap(true);
        right->append(*m_lib_note);

        auto brow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        const auto button = [&](const char* label, const char* name, const char* tip) {
            auto b = Gtk::make_managed<Gtk::Button>(label);
            b->set_name(std::string("libraries.") + name);
            b->set_tooltip_text(tip);
            brow->append(*b);
            return b;
        };
        auto imp = button("Import…", "import", "Open palette files (.ase, .gpl, .soc, .aco, Scribus .xml) as libraries");
        {   // s175: the format picked here, not by a filter the save dialog can't report
            auto mb = Gtk::make_managed<Gtk::MenuButton>();
            mb->set_label("Export…");
            mb->set_name("libraries.export");
            mb->set_tooltip_text("Save this library -- its picked colours, else all");
            auto pop = Gtk::make_managed<Gtk::Popover>();
            auto pb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            for (const auto& [label, fmt, key] : {std::tuple{"Adobe Swatch Exchange (.ase) — CMYK, spot and RGB", SW::Format::ASE, "ase"},
                                                  std::tuple{"GIMP / Inkscape palette (.gpl) — screen colours only", SW::Format::GPL, "gpl"}}) {
                auto b = Gtk::make_managed<Gtk::Button>(label);
                b->set_name(std::string("libraries.export.") + key);
                b->add_css_class("flat");
                if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) l->set_xalign(0);
                b->signal_clicked().connect([this, pop, fmt = fmt] { pop->popdown(); libraries_export(fmt); });
                pb->append(*b);
            }
            pop->set_child(*pb);
            mb->set_popover(*pop);
            brow->append(*mb);
            m_lib_export = mb;
        }
        auto gap = Gtk::make_managed<Gtk::Box>();
        gap->set_hexpand(true);
        brow->append(*gap);
        m_lib_remove = button("Remove", "remove", "");
        m_lib_add_mine = button("Add to My Library", "add_mine", "Copy the picked colours into My Library, for every document");
        m_lib_add_doc = button("Add to Document", "add_doc", "Copy the picked colours into this document (one step)");
        m_lib_add_doc->add_css_class("suggested-action");
        right->append(*brow);
        paned->set_end_child(*right);
        w.set_child(*paned);
        {   // Escape closes it, as a panel.
            auto keys = Gtk::EventControllerKey::create();
            keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
                if (kv != GDK_KEY_Escape) return false;
                m_lib->set_visible(false);
                return true;
            }, false);
            w.add_controller(keys);
        }

        imp->signal_clicked().connect([this] { libraries_import(); });
        m_lib_add_doc->signal_clicked().connect([this] { libraries_add_to_document(libraries_picked()); });
        m_lib_add_mine->signal_clicked().connect([this] {
            const auto at = libraries_picked();
            std::vector<core::Swatch> cs;
            for (auto i : at) cs.push_back(m_lib_cur.colors[i]);
            const int n = add_to_my_library(cs);
            if (n < 0) return;
            const auto skipped = static_cast<int>(cs.size()) - n;
            m_lib_note->set_text(n == 0 ? "Already in My Library."
                                        : count_text(static_cast<std::size_t>(n)) + " added to My Library" +
                                              (skipped > 0 ? " (" + std::to_string(skipped) + " there already)." : "."));
            libraries_list_sync();
        });
        m_lib_remove->signal_clicked().connect([this] { libraries_remove(); });
    }
    libraries_list_sync();
    std::string want = key.empty() ? (m_lib_on.empty() ? std::string("mine") : m_lib_on) : key;
    if (std::find(m_lib_keys.begin(), m_lib_keys.end(), want) == m_lib_keys.end()) want = "mine";
    libraries_show(want, pick);
    m_lib->present();
    if (auto lg = log::get(log::Area::Shell)) lg->info("libraries: open on {}", want);
}

// The left list: This Document, My Library, the system's (headed by
// program), the imported -- each with its count where it's cheap to know.
// s181: an SVG by its name (.svg / .svgz).
static bool is_svg_name(const std::string& n) {
    const auto dot = n.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = n.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == "svg" || e == "svgz";
}

void Shell::libraries_list_sync() {  // helper
    if (!m_lib_list) return;
    m_lib_quiet = true;
    while (auto* c = m_lib_list->get_first_child()) m_lib_list->remove(*c);
    m_lib_keys.clear();
    const auto heading = [&](const std::string& t) {
        auto l = Gtk::make_managed<Gtk::Label>(t);
        l->add_css_class("heading");
        l->add_css_class("dim-label");
        l->set_xalign(0);
        l->set_margin_top(m_lib_keys.empty() ? 4 : 12);
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*l);
        row->set_selectable(false);
        row->set_activatable(false);
        m_lib_list->append(*row);
        m_lib_keys.push_back({});
    };
    const auto entry = [&](const std::string& key, const std::string& name, const std::string& extra, bool dim, const std::string& tip) {
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto l = Gtk::make_managed<Gtk::Label>(name);
        l->set_xalign(0);
        l->set_hexpand(true);
        l->set_ellipsize(Pango::EllipsizeMode::END);
        if (dim) l->add_css_class("dim-label");
        box->append(*l);
        if (!extra.empty()) {
            auto e = Gtk::make_managed<Gtk::Label>(extra);
            e->add_css_class("dim-label");
            e->add_css_class("numeric");
            box->append(*e);
        }
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_name("libraries.row." + std::to_string(m_lib_keys.size()));
        row->set_child(*box);
        if (!tip.empty()) row->set_tooltip_text(tip);
        m_lib_list->append(*row);
        m_lib_keys.push_back(key);
    };
    entry("doc", "This Document", std::to_string(m_doc.ids_of<core::Swatch>().size()), false, "The document’s own swatches");
    const auto mine = SW::load_mine(my_library_file());
    entry("mine", "My Library", std::to_string(mine.colors.size()), false, "Your own colours, for every document — " + tilde(mine.path));
    // The system's: found once a window (a rescan when it opens again).
    if (!m_lib_found_once) { m_lib_found = SW::system_palettes(); m_lib_found_once = true; }
    std::string src;
    for (const auto& f : m_lib_found) {
        if (f.source != src) { heading(f.source); src = f.source; }
        entry("sys:" + f.path, std::filesystem::path(f.path).stem().string(), "", false, tilde(f.path));
    }
    if (m_lib_found.empty()) {
        heading("System");
        auto l = Gtk::make_managed<Gtk::Label>("No palettes found from GIMP, Inkscape, LibreOffice, Scribus or Krita.");
        l->add_css_class("dim-label");
        l->set_wrap(true);
        l->set_xalign(0);
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*l);
        row->set_selectable(false);
        row->set_activatable(false);
        m_lib_list->append(*row);
        m_lib_keys.push_back({});
    }
    // s177: the document's pictures, each a library of its main colours.
    {
        bool any = false;
        for (const auto& id : m_doc.ids_of<core::Asset>()) {
            const auto* a = m_doc.as<core::Asset>(id);
            if (!a) continue;
            // s181 (C1b): a vector (SVG) too -- the colours it's drawn in, as written.
            const bool vector = a->px_w <= 0;
            if (vector && !is_svg_name(a->name) && !is_svg_name(a->path)) continue;   // a vector we can't read
            if (!any) { heading("Pictures"); any = true; }
            const auto st = core::link_status(*a, m_project_dir);
            const bool there = st != core::LinkStatus::Missing && st != core::LinkStatus::EmbeddedMissing;
            entry("pic:" + id, a->name, there ? (vector ? "vector" : "") : "missing", !there,
                  (vector ? "The colours " : "The main colours in ") + a->name + (vector ? " is drawn in" : ""));
        }
    }
    const auto imp = SW::imported(m_settings);
    heading("Imported");
    for (const auto& p : imp) {
        const bool there = std::filesystem::exists(p);
        entry("imp:" + p, std::filesystem::path(p).stem().string(), there ? "" : "missing", !there, tilde(p));
    }
    if (imp.empty()) {
        auto l = Gtk::make_managed<Gtk::Label>("Import… opens palette files.");
        l->add_css_class("dim-label");
        l->set_xalign(0);
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*l);
        row->set_selectable(false);
        row->set_activatable(false);
        m_lib_list->append(*row);
        m_lib_keys.push_back({});
    }
    // Keep the one on show selected.
    for (std::size_t i = 0; i < m_lib_keys.size(); ++i)
        if (!m_lib_on.empty() && m_lib_keys[i] == m_lib_on)
            if (auto* row = m_lib_list->get_row_at_index(static_cast<int>(i))) m_lib_list->select_row(*row);
    m_lib_quiet = false;
}

// A library on the right: read (the document's from the model), its chips.
// `pick`: colours (by name) to show picked.
void Shell::libraries_show(const std::string& key, const std::vector<std::string>& pick) {  // helper
    if (!m_lib_chips) return;
    m_lib_quiet = true;
    const bool same = key == m_lib_on;
    m_lib_on = key;
    std::string err, sub;
    m_lib_cur = {};
    bool bad = false;
    if (key == "doc") {
        m_lib_cur.name = m_doc.settings().title.empty() ? "This Document" : m_doc.settings().title;
        m_lib_cur.colors = SW::document_colors(m_doc);
        sub = "This document’s swatches";
    } else if (key == "mine") {
        m_lib_cur = SW::load_mine(my_library_file());
        sub = "Yours, for every document · " + tilde(m_lib_cur.path);
    } else if (key.rfind("pic:", 0) == 0) {   // s177: a placed picture's colours
        const std::string id = key.substr(4);
        if (const auto* a = m_doc.as<core::Asset>(id); a && a->px_w <= 0) {   // s181: a vector -- read as written
            const std::string file = core::resolve_asset(*a, m_project_dir);
            std::string text;
            if (!file.empty()) {
                std::ifstream in(file, std::ios::binary);
                if (in) text.assign(std::istreambuf_iterator<char>(in), {});
            }
            const bool gz = text.size() > 2 && static_cast<unsigned char>(text[0]) == 0x1f && static_cast<unsigned char>(text[1]) == 0x8b;
            if (gz) text.clear();   // .svgz: compressed, not read here
            m_lib_cur = SW::vector_library(a->name, SW::svg_colors(text));
            sub = "The colours this drawing is drawn in, most used first";
            if (text.empty()) {
                bad = true;
                err = gz ? "a compressed SVG (.svgz) isn\u2019t read here -- save it as plain .svg." : "the drawing is missing or can\u2019t be read.";
            } else if (m_lib_cur.colors.empty()) {
                sub = "No colours named in this drawing (it may take them from its text, or be all black)";
            }
        } else if (const auto* a = m_doc.as<core::Asset>(id)) {
            const std::string file = core::resolve_asset(*a, m_project_dir);
            int w = 0, h = 0;
            const auto px = file.empty() ? std::vector<std::uint32_t>{} : views::images::pixels(file, a->stamp, w, h);
            m_lib_cur = SW::picture_library(a->name, SW::picture_colors(px.data(), w, h, w));
            sub = "The main colours in this picture, most first" + (px.empty() ? std::string(" -- it can\u2019t be read here") : std::string{});
            bad = px.empty();
            if (bad) err = "the picture is missing or can\u2019t be read.";
        }
    } else {
        const std::string p = path_of(key);
        if (auto got = SW::read_file(p, &err)) {
            m_lib_cur = *got;
            if (m_lib_cur.name.empty()) m_lib_cur.name = std::filesystem::path(p).stem().string();
            std::string who = "File";
            for (const auto& f : m_lib_found) if (f.path == p) who = f.source;
            sub = (is_sys(key) ? who + " · " : std::string("Imported · ")) + SW::format_name(m_lib_cur.format) + " · " + tilde(p);
        } else {
            bad = true;
            m_lib_cur.name = std::filesystem::path(p).stem().string();
            sub = tilde(p);
        }
        m_lib_cur.path = p;
        m_lib_cur.kind = is_sys(key) ? SW::LibKind::System : SW::LibKind::Imported;
    }
    // Its row picked in the list (quietly) -- else the list picks its first on focus.
    for (std::size_t i = 0; i < m_lib_keys.size(); ++i)
        if (m_lib_keys[i] == key)
            if (auto* row = m_lib_list->get_row_at_index(static_cast<int>(i))) m_lib_list->select_row(*row);
    m_lib_title->set_text(m_lib_cur.name + "  —  " + count_text(m_lib_cur.colors.size()));
    m_lib_sub->set_text(sub);

    // Keep what was picked when it's the same library again (a refresh).
    std::vector<std::string> keep = pick;
    if (keep.empty() && same)
        for (auto* c : m_lib_chips->get_selected_children()) {
            const int i = c->get_index();
            if (i >= 0 && i < static_cast<int>(m_lib_names.size())) keep.push_back(m_lib_names[static_cast<std::size_t>(i)]);
        }
    while (auto* c = m_lib_chips->get_first_child()) m_lib_chips->remove(*c);
    m_lib_names.clear();
    int k = 0;
    for (const auto& s : m_lib_cur.colors) {
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
        auto chip = Gtk::make_managed<Gtk::DrawingArea>();
        chip->set_content_width(64);
        chip->set_content_height(40);
        const std::uint32_t c = core::doc::screen_rgb(s);
        const bool spot = s.model == core::ColorModel::Spot;
        chip->set_draw_func([c, spot](const Cairo::RefPtr<Cairo::Context>& cr, int cw, int ch) {
            cr->rectangle(0.5, 0.5, cw - 1, ch - 1);
            cr->set_source_rgb(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
            cr->fill_preserve();
            cr->set_source_rgba(0, 0, 0, 0.25);
            cr->set_line_width(1);
            cr->stroke();
            if (spot) {   // a spot ink: a white dot, ringed, in the corner
                cr->arc(cw - 8, 8, 4, 0, 2 * M_PI);
                cr->set_source_rgb(1, 1, 1);
                cr->fill_preserve();
                cr->set_source_rgba(0, 0, 0, 0.6);
                cr->stroke();
            }
        });
        box->append(*chip);
        auto l = Gtk::make_managed<Gtk::Label>(s.name);
        l->add_css_class("caption");
        l->set_ellipsize(Pango::EllipsizeMode::END);
        l->set_max_width_chars(9);
        l->set_width_chars(9);
        box->append(*l);
        auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_name("libraries.chip." + std::to_string(k++));
        child->set_child(*box);
        child->set_tooltip_text(s.name + "\n" + value_text(s) + (s.from.empty() ? "" : "\nFrom " + s.from));
        m_lib_chips->append(*child);
        m_lib_names.push_back(s.name);
    }
    for (std::size_t i = 0; i < m_lib_names.size(); ++i)
        if (std::find(keep.begin(), keep.end(), m_lib_names[i]) != keep.end())
            if (auto* c = m_lib_chips->get_child_at_index(static_cast<int>(i))) m_lib_chips->select_child(*c);
    if (bad) m_lib_note->set_text("Can’t read it: " + (err.empty() ? std::string("missing or not a palette.") : err));
    else if (m_lib_cur.colors.empty()) m_lib_note->set_text(key == "mine" ? "Empty so far. Add to My Library from any library, or from a swatch’s inspector." : "No colours.");
    else m_lib_note->set_text({});
    m_lib_quiet = false;
    libraries_buttons_sync();
    if (auto lg = log::get(log::Area::Shell)) lg->info("libraries: show {} ({} colours{})", key, m_lib_cur.colors.size(), bad ? ", unreadable" : "");
}

std::vector<std::size_t> Shell::libraries_picked() const {  // helper
    std::vector<std::size_t> out;
    if (!m_lib_chips) return out;
    for (auto* c : m_lib_chips->get_selected_children()) {
        const int i = c->get_index();
        if (i >= 0 && i < static_cast<int>(m_lib_cur.colors.size())) out.push_back(static_cast<std::size_t>(i));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Shell::libraries_buttons_sync() {  // helper
    if (!m_lib_add_doc) return;
    const auto n = libraries_picked().size();
    const bool doc = m_lib_on == "doc", mine = m_lib_on == "mine", imp = is_imp(m_lib_on);
    m_lib_add_doc->set_visible(!doc);
    m_lib_add_doc->set_sensitive(n > 0);
    m_lib_add_doc->set_label(n > 1 ? "Add " + std::to_string(n) + " to Document" : "Add to Document");
    m_lib_add_mine->set_visible(!mine);
    m_lib_add_mine->set_sensitive(n > 0);
    m_lib_remove->set_visible(mine || imp);
    if (mine) {
        m_lib_remove->set_label("Remove");
        m_lib_remove->set_tooltip_text("Take the picked colours out of My Library");
        m_lib_remove->set_sensitive(n > 0);
    } else if (imp) {
        m_lib_remove->set_label("Remove from List");
        m_lib_remove->set_tooltip_text("Forget this imported library (its file stays where it is)");
        m_lib_remove->set_sensitive(true);
    }
    m_lib_export->set_sensitive(!m_lib_cur.colors.empty());
    if (m_lib_note->get_text().empty() || m_lib_note_is_count) {
        const auto all = m_lib_cur.colors.size();
        if (all > 0) {
            m_lib_note->set_text(n == 0 ? "Click a colour to pick it; Ctrl or Shift for more, Ctrl+A for all. Double-click puts one in the document."
                                        : std::to_string(n) + " of " + std::to_string(all) + " picked.");
            m_lib_note_is_count = true;
        }
    }
}

void Shell::libraries_add_to_document(const std::vector<std::size_t>& at) {  // helper
    if (at.empty() || m_lib_on == "doc") return;
    std::vector<core::Swatch> cs;
    for (auto i : at) if (i < m_lib_cur.colors.size()) cs.push_back(m_lib_cur.colors[i]);
    const auto before = m_doc.ids_of<core::Swatch>().size();
    const auto ids = core::doc::add_library_swatches(m_journal, m_doc, cs, m_lib_cur.name);
    const auto added = m_doc.ids_of<core::Swatch>().size() - before;
    m_lib_note_is_count = false;
    m_lib_note->set_text(added == 0 ? std::string("Already in the document.")
                                    : count_text(added) + " added to the document" +
                                          (added < cs.size() ? " (" + std::to_string(cs.size() - added) + " there already)." : "."));
    if (auto lg = log::get(log::Area::Shell)) lg->info("libraries: {} of {} into the document from '{}'", added, cs.size(), m_lib_cur.name);
    libraries_list_sync();   // This Document's count
}

void Shell::libraries_remove() {  // helper
    if (m_lib_on == "mine") {
        const auto at = libraries_picked();
        if (at.empty()) return;
        auto dialog = Gtk::AlertDialog::create(at.size() == 1 ? "Remove “" + m_lib_cur.colors[at.front()].name + "” from My Library?"
                                                              : "Remove " + count_text(at.size()) + " from My Library?");
        dialog->set_detail("Documents keep the copies they have. This can’t be undone.");
        dialog->set_buttons({"Cancel", "Remove"});
        dialog->set_cancel_button(0);
        dialog->set_default_button(0);
        dialog->choose(*m_lib, [this, dialog, at](const Glib::RefPtr<Gio::AsyncResult>& r) {
            int b = 0;
            try { b = dialog->choose_finish(r); } catch (const Glib::Error&) { return; }
            if (b != 1) return;
            auto mine = SW::load_mine(my_library_file());
            std::vector<std::size_t> idx;
            for (auto i : at)   // by name: the file is the truth, not the chips
                for (std::size_t k = 0; k < mine.colors.size(); ++k)
                    if (i < m_lib_cur.colors.size() && mine.colors[k].name == m_lib_cur.colors[i].name) idx.push_back(k);
            const int n = SW::remove_at(mine, idx);
            if (n > 0 && !SW::save_mine(mine)) { alert("Can’t save My Library", "Composix couldn’t write “" + mine.path + "”."); return; }
            if (auto lg = log::get(log::Area::Shell)) lg->info("my library: -{}", n);
            libraries_list_sync();
            libraries_show("mine");
            m_lib_note_is_count = false;
            m_lib_note->set_text(count_text(static_cast<std::size_t>(n)) + " removed from My Library.");
        });
    } else if (is_imp(m_lib_on)) {
        const auto p = path_of(m_lib_on);
        if (SW::forget_imported(m_settings, p)) core::save_settings(m_settings_file, m_settings);
        if (auto lg = log::get(log::Area::Shell)) lg->info("libraries: forgot '{}'", p);
        m_lib_on.clear();
        libraries_list_sync();
        libraries_show("mine");
    }
}

void Shell::libraries_import() {  // helper
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import Colour Libraries");
    auto filter = Gtk::FileFilter::create();
    filter->set_name("Palettes (.ase, .gpl, .soc, .aco, .xml, .json)");
    for (const char* p : {"*.ase", "*.ASE", "*.gpl", "*.GPL", "*.soc", "*.SOC", "*.aco", "*.ACO", "*.xml", "*.XML", "*.json", "*.sbz", "*.SBZ", "*.colors"}) filter->add_pattern(p);
    auto any = Gtk::FileFilter::create();
    any->set_name("All files");
    any->add_pattern("*");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    filters->append(any);
    dialog->set_filters(filters);
    dialog->set_default_filter(filter);
    dialog->open_multiple(*m_lib, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        std::vector<Glib::RefPtr<Gio::File>> chosen;
        try { chosen = dialog->open_multiple_finish(result); }
        catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Io)) lg->info("libraries import: dismissed ({})", e.what());
            return;
        }
        std::string bad, last;
        int ok = 0;
        for (const auto& f : chosen) {
            if (!f || f->get_path().empty()) continue;
            std::string err;
            if (!SW::read_file(f->get_path(), &err)) {
                bad += (bad.empty() ? "" : ", ") + std::string("“") + f->get_basename() + "”";
                continue;
            }
            SW::remember_imported(m_settings, f->get_path());
            last = f->get_path();
            ++ok;
        }
        if (ok) core::save_settings(m_settings_file, m_settings);
        if (auto lg = log::get(log::Area::Io)) lg->info("libraries import: {} read, {}", ok, bad.empty() ? "none bad" : bad);
        if (!bad.empty())
            alert("Can’t read some of those", bad + " — not palettes Composix can read (.ase, .gpl, .soc, .aco, Scribus .xml).");
        libraries_list_sync();
        if (!last.empty()) libraries_show("imp:" + last);
    });
}

void Shell::libraries_export(core::swatches::Format want) {  // helper
    if (m_lib_cur.colors.empty()) return;
    std::vector<core::Swatch> cs;
    const auto at = libraries_picked();
    if (at.empty()) cs = m_lib_cur.colors;
    else for (auto i : at) cs.push_back(m_lib_cur.colors[i]);
    const std::string name = m_lib_cur.name;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title(at.empty() ? "Export “" + name + "”" : "Export " + count_text(cs.size()) + " from “" + name + "”");
    const bool gpl = want == SW::Format::GPL;
    auto filter = Gtk::FileFilter::create();
    filter->set_name(gpl ? "GIMP / Inkscape palette (.gpl)" : "Adobe Swatch Exchange (.ase)");
    filter->add_pattern(gpl ? "*.gpl" : "*.ase");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(filter);
    dialog->set_initial_name(name + (gpl ? ".gpl" : ".ase"));
    dialog->save(*m_lib, [this, dialog, cs, name, want](const Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> f;
        try { f = dialog->save_finish(result); }
        catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Io)) lg->info("libraries export: dismissed ({})", e.what());
            return;
        }
        if (!f || f->get_path().empty()) return;
        // The format picked; a name typed with the other one's extension wins.
        const auto ext = std::filesystem::path(f->get_path()).extension().string();
        auto fmt = (ext == ".ase" || ext == ".ASE" || ext == ".gpl" || ext == ".GPL") ? SW::save_format(f->get_path()) : want;
        const auto path = SW::with_extension(f->get_path(), fmt);
        std::string err;
        if (!SW::write_file(path, name, cs, fmt, &err)) {
            alert("Can’t export the colours", "Composix couldn’t write “" + path + "”" + (err.empty() ? "." : ": " + err));
            return;
        }
        m_lib_note_is_count = false;
        m_lib_note->set_text(count_text(cs.size()) + " exported to " + tilde(path) + " (" + SW::format_name(fmt) + ").");
        if (auto lg = log::get(log::Area::Io)) lg->info("libraries export: {} to '{}'", cs.size(), path);
    });
}

}  // namespace composix
