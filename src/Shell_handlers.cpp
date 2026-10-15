#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"

#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include "plate/plate.hpp"
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>
#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <tuple>

#include "views/Images.hpp"

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace composix {

// ── the project folder (s004) ──────────────────────────────────────────────
void Shell::on_open() {  // handler: pick a project folder -> load it
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open a Composix project folder");
    dialog->select_folder(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto dir = dialog->select_folder_finish(result);
            if (dir && !dir->get_path().empty()) open_project(dir->get_path());
        } catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Io)) lg->info("open: dismissed ({})", e.what());
        }
    });
}

void Shell::on_save() {  // handler: save (Save As the first time)
    if (m_project_dir.empty()) { on_save_as(); return; }
    save_to(m_project_dir);
}

// A project is a FOLDER: the name typed here becomes the folder, holding
// document.json and assets/ (REQUIREMENTS §8).
void Shell::on_save_as() {  // handler: name a new project folder -> save
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save the project as a folder");
    dialog->set_initial_name(m_doc.settings().title);
    dialog->save(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto f = dialog->save_finish(result);
            if (!f || f->get_path().empty()) return;
            const std::string dir = f->get_path();
            std::error_code ec;
            if (std::filesystem::exists(dir, ec) && !std::filesystem::is_directory(dir, ec)) {
                alert("Can't save there", "\u201c" + dir + "\u201d is a file, and a Composix project is a folder.");
                return;
            }
            save_to(dir);
        } catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Io)) lg->info("save-as: dismissed ({})", e.what());
        }
    });
}

// ── images (s009) ──────────────────────────────────────────────────────────
// Place: one image frame picked -> into it; else the canvas is loaded with the
// file (click on a page / drag a frame / click an image frame; Esc puts it down).
void Shell::on_place() {  // handler
    pick_image([this](const core::ImageFile& f) {
        auto& cv = m_canvas_view.canvas();
        const auto& sel = cv.selection();
        if (sel.size() == 1) {
            if (const auto* fr = m_doc.as<core::Frame>(sel.front()); fr && fr->kind == core::FrameKind::Image) {
                core::doc::place_image(m_journal, m_doc, sel.front(), f);
                return;
            }
        }
        if (m_main.get_visible_child_name() != "canvas") {
            alert("Open a page to place on", "Pick a page in the navigator, then Place again — or drop the file on it.");
            return;
        }
        cv.load_image(f);
    });
}

void Shell::on_asset_action(const std::string& act) {  // handler
    std::vector<std::string> assets;
    for (const auto& id : m_inspected) if (m_doc.as<core::Asset>(id)) assets.push_back(id);
    if (assets.empty()) return;
    if (act == "relink") {
        if (assets.size() > 1) {
            alert("Relink one at a time", "Pick a single image to point it at another file.");
            return;
        }
        const auto id = assets.front();
        pick_image([this, id](const core::ImageFile& f) { core::doc::relink_assets(m_journal, m_doc, {{id, f}}); });
    } else if (act == "update") {   // the same files, read again
        std::vector<std::pair<core::Doc::Id, core::ImageFile>> files;
        for (const auto& id : assets)
            if (auto f = views::images::probe(core::resolve_asset(*m_doc.as<core::Asset>(id), m_project_dir)))
                files.push_back({id, *f});
        core::doc::relink_assets(m_journal, m_doc, files);
    } else if (act == "embed") {
        if (m_project_dir.empty()) {
            alert("Save the project first", "An embedded image is copied into the project’s folder, "
                                            "and this document doesn’t have one yet.");
            return;
        }
        std::vector<std::pair<core::Doc::Id, std::string>> rels;
        for (const auto& id : assets) {
            std::string err;
            const auto rel = embed_copy(*m_doc.as<core::Asset>(id), &err);
            if (rel.empty()) { alert("Can’t embed " + m_doc.as<core::Asset>(id)->name, err); return; }
            rels.push_back({id, rel});
        }
        core::doc::embed_assets(m_journal, m_doc, rels);
    } else if (act == "unembed") {
        core::doc::unembed_assets(m_journal, m_doc, assets);
    }
}

// ── pages (s014) ───────────────────────────────────────────────────────────
bool Shell::page_action(const std::string& act, const std::vector<std::string>& ids) {  // helper
    std::vector<std::string> pages;
    for (const auto& id : ids) if (m_doc.as<core::Page>(id)) pages.push_back(id);
    if (pages.empty()) return false;
    if (act == "add") {
        const auto last = pages.back();
        const auto made = core::doc::add_pages(m_journal, m_doc, last, 1, m_doc.as<core::Page>(last)->master);
        if (made.empty()) return false;
        Glib::signal_idle().connect_once([this, made] { m_tiles.show_section(m_outline, core::Segment::Pages, made); on_tiles_selection(made); });
        return true;
    }
    if (act == "delete") {
        if (pages.size() >= m_doc.pages().size()) {
            alert("A document keeps at least one page", "Add a page first, then delete these.");
            return false;
        }
        const auto next = core::doc::neighbour_page(m_doc, pages);
        const bool ok = core::doc::remove_pages(m_journal, m_doc, pages);
        if (ok && !next.empty())
            Glib::signal_idle().connect_once([this, next] { m_tiles.show_section(m_outline, core::Segment::Pages, {next}); on_tiles_selection({next}); });
        return ok;
    }
    if (act == "section") {
        const int n = static_cast<int>(m_doc.ids_of<core::Section>().size()) + 1;
        return !core::doc::add_section(m_journal, m_doc, pages.front(), "Section " + std::to_string(n),
                                       core::Numbering::Arabic, 1).empty();
    }
    return false;
}

void Shell::on_tiles_move(const std::vector<std::string>& ids, const std::string& target, bool before) {  // handler
    if (std::find(ids.begin(), ids.end(), target) != ids.end()) return;   // onto itself
    std::vector<std::string> rest;
    for (const auto& p : m_doc.pages())
        if (std::find(ids.begin(), ids.end(), p) == ids.end()) rest.push_back(p);
    const auto it = std::find(rest.begin(), rest.end(), target);
    if (it == rest.end()) return;
    const std::string after = before ? (it == rest.begin() ? std::string{} : *(it - 1)) : target;
    if (core::doc::move_pages(m_journal, m_doc, ids, after)) {
        Glib::signal_idle().connect_once([this, ids] {
            m_tiles.show_section(m_outline, core::Segment::Pages, ids);
            on_tiles_selection(ids);
        });
        if (auto lg = log::get(log::Area::Shell)) lg->info("moved {} page(s) {} {}", ids.size(), before ? "before" : "after", target);
    }
}

// s016: where CMYK output profiles live (Fedora / Debian system folders, the
// user's own).
std::vector<std::string> Shell::profile_dirs() const {  // helper
    std::vector<std::string> dirs{"/usr/share/color/icc", "/usr/local/share/color/icc"};
    if (const char* home = std::getenv("HOME")) {
        dirs.push_back(std::string(home) + "/.local/share/icc");
        dirs.push_back(std::string(home) + "/.color/icc");
    }
    return dirs;
}

// ── PDF (s013) ─────────────────────────────────────────────────────────────
// A small sheet: Bleed, Crop marks, then Export... names the file.
void Shell::on_export_pdf() {  // handler
    if (!m_export) {
        m_export = std::make_unique<Gtk::Window>();
        auto& w = *m_export;
        w.set_name("export");
        w.set_title("Export PDF");
        w.set_modal(true);
        w.set_resizable(false);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        box->set_name("export.box");
        box->add_css_class("cx-export");
        box->set_margin(18);
        auto head = Gtk::make_managed<Gtk::Label>("Export PDF");
        head->set_name("export.title");
        head->add_css_class("cx-export-title");
        head->set_xalign(0);
        box->append(*head);
        auto bleed = Gtk::make_managed<Gtk::CheckButton>();
        bleed->set_name("export.bleed");
        auto marks = Gtk::make_managed<Gtk::CheckButton>("Crop marks");
        marks->set_name("export.marks");
        // s015: which writer. Print = Plate (page boxes, CMYK, subset fonts);
        // Screen = cairo (RGB, for viewing and sharing).
        auto print = Gtk::make_managed<Gtk::CheckButton>("For print \u2014 trim / bleed boxes, CMYK colour");
        print->set_name("export.print");
        auto screen = Gtk::make_managed<Gtk::CheckButton>("For screen \u2014 RGB");
        screen->set_name("export.screen");
        screen->set_group(*print);
        box->append(*print);
        box->append(*screen);
        box->append(*bleed);
        box->append(*marks);
        // s016: the press's profile (CMYK output profiles found on this
        // computer), and whether RGB is converted to it.
        auto prow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto plab = Gtk::make_managed<Gtk::Label>("Output profile");
        plab->add_css_class("cx-field-label");
        std::vector<Glib::ustring> names{"None (device CMYK)"};
        m_profiles.clear();
        for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) {
            m_profiles.push_back(p.path);
            names.push_back(p.name);
        }
        auto prof = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
        prof->set_name("export.profile");
        prof->set_hexpand(true);
        {   // a long list: type to find one
            GtkExpression* ex = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
            gtk_drop_down_set_expression(prof->gobj(), ex);
            gtk_expression_unref(ex);
            gtk_drop_down_set_enable_search(prof->gobj(), TRUE);
        }
        prow->append(*plab);
        prow->append(*prof);
        box->append(*prow);
        auto conv = Gtk::make_managed<Gtk::CheckButton>("Convert RGB images and colours to CMYK");
        conv->set_name("export.convert");
        conv->set_sensitive(false);   // until a profile is picked
        box->append(*conv);
        // s017: PDF/X conformance. Needs the press's profile (the output
        // intent); X-1a converts all RGB, so the convert box is forced on.
        auto srow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        srow->set_name("export.standard.row");
        auto slab = Gtk::make_managed<Gtk::Label>("Standard");
        slab->add_css_class("cx-field-label");
        auto stdd = Gtk::make_managed<Gtk::DropDown>(
            Gtk::StringList::create({"None", "PDF/X-1a:2003 \u2014 CMYK + spot only", "PDF/X-4 \u2014 ICC RGB allowed"}));
        stdd->set_name("export.standard");
        stdd->set_hexpand(true);
        stdd->set_tooltip_text("PDF/X needs an output profile");
        srow->append(*slab);
        srow->append(*stdd);
        box->append(*srow);
        // Only the print writer manages colour.
        const auto sync = [print, prof, conv, stdd] {
            const bool on = print->get_active(), pro = on && prof->get_selected() > 0;
            prof->set_sensitive(on);
            stdd->set_sensitive(pro);
            if (!pro && stdd->get_selected() != 0) stdd->set_selected(0);
            const bool x1a = pro && stdd->get_selected() == 1;
            if (x1a) conv->set_active(true);
            conv->set_sensitive(pro && !x1a);
        };
        print->signal_toggled().connect(sync);
        prof->property_selected().signal_changed().connect(sync);
        stdd->property_selected().signal_changed().connect(sync);
        auto note = Gtk::make_managed<Gtk::Label>("Every page at its own size, fonts embedded,\nimages at full resolution.");
        note->set_name("export.note");
        note->add_css_class("cx-export-note");
        note->set_xalign(0);
        box->append(*note);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->set_name("export.cancel");
        auto go = Gtk::make_managed<Gtk::Button>("Export…");
        go->set_name("export.go");
        go->add_css_class("suggested-action");
        row->append(*cancel);
        row->append(*go);
        box->append(*row);
        w.set_child(*box);
        cancel->signal_clicked().connect([this] { m_export->set_visible(false); });
        auto keys = Gtk::EventControllerKey::create();   // Esc closes the sheet
        keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
            if (kv != GDK_KEY_Escape) return false;
            m_export->set_visible(false);
            return true;
        }, false);
        w.add_controller(keys);
        w.set_default_widget(*go);
        go->signal_clicked().connect([this, bleed, marks, print, prof, conv, stdd] {
            m_pdf_bleed = bleed->get_active();
            m_pdf_marks = marks->get_active();
            m_pdf_print = print->get_active();
            const auto i = prof->get_selected();
            m_pdf_profile = (i > 0 && i - 1 < m_profiles.size()) ? m_profiles[i - 1] : std::string{};
            m_pdf_convert = conv->get_active();
            m_pdf_standard = (m_pdf_print && !m_pdf_profile.empty()) ? static_cast<int>(stdd->get_selected()) : 0;
            m_export->set_visible(false);
            auto dialog = Gtk::FileDialog::create();
            dialog->set_title("Export PDF");
            dialog->set_initial_name(m_doc.settings().title + ".pdf");
            dialog->save(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
                try {
                    auto f = dialog->save_finish(result);
                    if (f && !f->get_path().empty()) export_pdf_to(f->get_path());
                } catch (const Glib::Error& e) {
                    if (auto lg = log::get(log::Area::Io)) lg->info("export: dismissed ({})", e.what());
                }
            });
        });
    }
    // Refresh the sheet's state each time (the bleed comes from the document).
    auto* box = static_cast<Gtk::Box*>(m_export->get_child());
    for (auto* c = box->get_first_child(); c; c = c->get_next_sibling()) {
        if (c->get_name() == "export.bleed") {
            auto* b = static_cast<Gtk::CheckButton*>(c);
            b->set_label("Bleed (" + core::inches(m_doc.settings().bleed) + " in)");
            b->set_active(m_pdf_bleed);
        } else if (c->get_name() == "export.marks") {
            static_cast<Gtk::CheckButton*>(c)->set_active(m_pdf_marks);
        } else if (c->get_name() == "export.print") {
            static_cast<Gtk::CheckButton*>(c)->set_active(m_pdf_print);
        } else if (c->get_name() == "export.screen") {
            static_cast<Gtk::CheckButton*>(c)->set_active(!m_pdf_print);
        } else if (c->get_name() == "export.convert") {
            static_cast<Gtk::CheckButton*>(c)->set_active(m_pdf_convert);
        } else if (auto* row = dynamic_cast<Gtk::Box*>(c)) {   // the profile row: re-pick the session's profile
            for (auto* k = row->get_first_child(); k; k = k->get_next_sibling())
                if (k->get_name() == "export.profile") {
                    auto* dd = static_cast<Gtk::DropDown*>(k);
                    guint at = 0;
                    for (std::size_t n = 0; n < m_profiles.size(); ++n)
                        if (m_profiles[n] == m_pdf_profile) at = static_cast<guint>(n + 1);
                    dd->set_selected(at);
                } else if (k->get_name() == "export.standard") {
                    static_cast<Gtk::DropDown*>(k)->set_selected(static_cast<guint>(m_pdf_standard));
                }
        }
    }
    m_export->set_transient_for(*this);
    m_export->present();
}

// Closing with unsaved changes asks first. "Save" on a never-saved document
// opens Save As and keeps the window; close again once it is saved.
bool Shell::on_close_request() {  // handler
    if (!m_dirty || m_closing_ok) return Gtk::ApplicationWindow::on_close_request();
    auto dialog = Gtk::AlertDialog::create("Save changes to \u201c" + m_doc.settings().title + "\u201d?");
    dialog->set_detail("Your changes will be lost if you don\u2019t save them.");
    dialog->set_buttons({"Cancel", "Don\u2019t Save", "Save"});
    dialog->set_cancel_button(0);
    dialog->set_default_button(2);
    dialog->choose(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        int choice = 0;
        try { choice = dialog->choose_finish(result); } catch (const Glib::Error&) { return; }
        // Close on IDLE, not here: inside this callback the dialog's window
        // is still transient for us, and closing now finalizes it against a
        // dead parent (GLib CRITICAL in gtk_window_set_transient_for -- s004).
        const auto close_later = [this] {
            m_closing_ok = true;
            Glib::signal_idle().connect_once([this] { close(); });
        };
        if (choice == 1) close_later();
        else if (choice == 2) {
            if (m_project_dir.empty()) { on_save_as(); return; }
            if (save_to(m_project_dir)) close_later();
        }
    });
    return true;   // handled: stay open until the user chooses
}

void Shell::on_recent_open(const std::string& path) {  // handler: open a recent project
    if (path.empty()) return;
    // A vanished folder self-prunes; a live one opens (and moves to the front).
    if (!std::filesystem::exists(path)) {
        core::recents_remove(m_recents, path);
        core::save_recents(m_recents_file, m_recents);
        rebuild_recents_menu();
        if (auto lg = log::get(log::Area::Recents)) lg->warn("open '{}': gone -- pruned", path);
        alert("That project is gone", "\u201c" + path + "\u201d no longer exists; it was removed from Open Recent.");
        return;
    }
    open_project(path);
}

void Shell::on_recent_clear() {  // handler: empty the recents list
    m_recents.clear();
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::on_dump_registry() {  // handler: print the live widget tree
    // The debuggability payoff, on demand. Same call works from a gdb prompt.
    registry::dump();
}

void Shell::on_shortcuts() {  // handler: open the keyboard reference
    // Same lifetime stone as About: build once, re-present. The window's CONTENT
    // is not written here -- it renders by walking core::shortcut_registry(),
    // the same list bind_accelerators() wires from, so the two can't drift.
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {  // handler: open the About window (dialog-lifetime exemplar)
    // Lazily build once, then re-present. Hide-on-close singleton (CANON:
    // "Lifetime shape is design") -- the X hides it, so one instance lives for
    // the session and its named children never re-register.
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

// ─────────────────────────────────────────────────────────────────────────────
// s002 -- the navigator / tiles / inspector routing.
// ─────────────────────────────────────────────────────────────────────────────

// A SEGMENT shows its whole category as tiles; ITEMS open. The inspector
// follows whichever: the category's summary, or the items' shared fields.
void Shell::on_nav_pick(const views::Pick& p) {  // handler
    m_pick = p;
    show_pick(p, {});
}

void Shell::on_tiles_selection(const std::vector<std::string>& ids) {  // handler
    m_inspected = ids;
    if (ids.empty() && m_segment) m_inspector.show_section(m_outline, *m_segment);
    else                          m_inspector.show_items(resolve(ids));
}

// Opening tiles = selecting those items in the navigator, so there is ONE
// route to "open": the navigator's pick (the tile view never opens on its own).
void Shell::on_tiles_open(const std::vector<std::string>& ids) {  // handler
    m_nav.select_items(ids);
}

// An inspector edit is ONE verb across everything the inspector shows -- one
// journal step, one Ctrl+Z (REQUIREMENTS §10). The views repaint from the
// document's change (queue_refresh), not from here.
void Shell::on_inspector_edit(const std::string& key, const std::string& choice) {  // handler
    if (m_inspected.empty()) return;
    bool did = false;
    try {
        if (key == "master") {
            did = core::doc::set_master(m_journal, m_doc, m_inspected, choice);
        } else if (key == "size") {
            const auto presets = core::doc::size_presets(m_doc.settings());
            const auto i = static_cast<std::size_t>(std::stoi(choice));
            if (i < presets.size()) did = core::doc::set_page_size(m_journal, m_doc, m_inspected, presets[i]);
        } else if (key == "numbering") {
            did = core::doc::set_numbering(m_journal, m_doc, m_inspected,
                                           static_cast<core::Numbering>(std::stoi(choice)));
        } else if (key == "starts") {
            did = core::doc::set_starts(m_journal, m_doc, m_inspected,
                                        static_cast<core::Side>(std::stoi(choice)));
        } else if (key == "pstyle" || key == "cstyle") {   // s011: the text being edited
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            auto [a, b] = cv.edit_range();
            const auto story = cv.edit_story_id();
            if (key == "pstyle") {
                did = core::doc::set_para_style(m_journal, m_doc, story, a, b, choice);
            } else {
                // Just a caret: the word it is in (as a word processor does).
                if (a == b) if (const auto* st = m_doc.as<core::Story>(story)) std::tie(a, b) = core::word_at(*st, a);
                did = core::doc::set_char_style(m_journal, m_doc, story, a, b, choice);
            }
            cv.grab_focus();   // back to typing
        } else if (key == "color") {   // s015: a style's swatch
            did = core::doc::edit_styles(m_journal, m_doc, m_inspected, "Color",
                                         [&](core::Style& st) { st.color = choice; });
        } else if (key == "font" || key == "size" || key == "leading" || key == "bold" || key == "italic" ||
                   key == "align" || key == "before" || key == "after") {   // s011: style settings
            static const std::map<std::string, std::string> labels{
                {"font", "Font"}, {"size", "Size"}, {"leading", "Leading"}, {"bold", "Weight"},
                {"italic", "Slant"}, {"align", "Align"}, {"before", "Space before"}, {"after", "Space after"}};
            const double v = (key == "font") ? 0 : std::stod(choice);
            did = core::doc::edit_styles(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Style& s) {
                if (key == "font") s.family = choice;
                else if (key == "size") {
                    // Leading follows the size when it was the usual ratio (or auto).
                    if (s.size > 0 && s.leading > 0 && std::abs(s.leading - s.size * 1.2) < 1.5) s.leading = std::round(v * 1.2 * 2) / 2;
                    s.size = v;
                }
                else if (key == "leading") s.leading = v;
                else if (key == "bold") s.bold = v != 0;
                else if (key == "italic") s.italic = v != 0;
                else if (key == "align") s.align = static_cast<core::Align>(static_cast<int>(v));
                else if (key == "before") s.space_before = v;
                else if (key == "after") s.space_after = v;
            });
        } else if (key == "style") {   // s011: Duplicate / Delete
            if (choice == "duplicate" && !m_inspected.empty()) {
                const auto made = core::doc::duplicate_style(m_journal, m_doc, m_inspected.front());
                did = !made.empty();
                if (did) Glib::signal_idle().connect_once([this, made] { m_tiles.show_section(m_outline, core::Segment::Styles, {made}); on_tiles_selection({made}); });
            } else if (choice == "delete") {
                did = core::doc::delete_styles(m_journal, m_doc, m_inspected);
            }
        } else if (key == "mcolumns" || key == "mgutter" || key == "mtop" || key == "mbottom" ||
                   key == "minside" || key == "moutside") {   // s014: a master's margins / columns
            static const std::map<std::string, std::string> labels{
                {"mcolumns", "Page columns"}, {"mgutter", "Page gutter"}, {"mtop", "Top margin"},
                {"mbottom", "Bottom margin"}, {"minside", "Inside margin"}, {"moutside", "Outside margin"}};
            const double v = std::stod(choice);
            did = core::doc::edit_masters(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Master& m) {
                if (key == "mcolumns") m.columns = static_cast<int>(v);
                else if (key == "mgutter") m.gutter = v;
                else if (key == "mtop") m.margins.top = v;
                else if (key == "mbottom") m.margins.bottom = v;
                else if (key == "minside") m.margins.inside = v;
                else m.margins.outside = v;
            });
        } else if (key == "page") {   // s014: Add page after / Delete / Start section
            did = page_action(choice, m_inspected);
        } else if (key == "fill" || key == "stroke" || key == "stroke_w") {   // s016: frame fill / stroke
            static const std::map<std::string, std::string> labels{{"fill", "Fill"}, {"stroke", "Stroke"}, {"stroke_w", "Stroke weight"}};
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Frame& f) {
                if (key == "fill") f.fill = choice;
                else if (key == "stroke") f.stroke = choice;
                else f.stroke_w = std::stod(choice);
            });
        } else if (key == "columns" || key == "gutter" || key == "widths" || key == "balance") {   // s012
            static const std::map<std::string, std::string> labels{
                {"columns", "Columns"}, {"gutter", "Gutter"}, {"widths", "Column widths"}, {"balance", "Balance columns"}};
            std::vector<std::string> text;
            for (const auto& id : m_inspected)
                if (const auto* f = m_doc.as<core::Frame>(id); f && f->kind == core::FrameKind::Text) text.push_back(id);
            did = core::doc::edit_frames(m_journal, m_doc, text, labels.at(key), [&](core::Frame& f) {
                if (key == "columns") {
                    const int n = std::stoi(choice);
                    if (n != f.columns) f.widths.clear();   // a new count starts even
                    f.columns = n;
                } else if (key == "gutter") {
                    f.gutter = std::stod(choice);
                } else if (key == "balance") {
                    f.balance = choice == "1";
                } else {   // "2:1:1" -> weights; "" -> equal
                    std::vector<double> w;
                    std::size_t at = 0;
                    while (!choice.empty() && at <= choice.size()) {
                        const auto c = choice.find(':', at);
                        w.push_back(std::stod(choice.substr(at, c == std::string::npos ? std::string::npos : c - at)));
                        if (c == std::string::npos) break;
                        at = c + 1;
                    }
                    f.widths = w;
                }
            });
        } else if (key == "fit") {   // s009
            did = core::doc::set_fit(m_journal, m_doc, m_inspected, static_cast<core::ImageFit>(std::stoi(choice)));
        } else if (key == "frame_image") {
            if (choice == "update") {   // the frames' files, read again
                std::vector<std::pair<core::Doc::Id, core::ImageFile>> files;
                for (const auto& id : m_inspected)
                    if (const auto* f = m_doc.as<core::Frame>(id))
                        if (const auto* a = m_doc.as<core::Asset>(f->asset))
                            if (auto img = views::images::probe(core::resolve_asset(*a, m_project_dir)))
                                files.push_back({f->asset, *img});
                did = core::doc::relink_assets(m_journal, m_doc, files);
            } else {
                on_place();
                return;
            }
        } else if (key == "asset") {
            on_asset_action(choice);
            return;
        }
    } catch (const std::exception& e) {
        if (auto lg = log::get(log::Area::Shell)) lg->warn("edit {}={}: {}", key, choice, e.what());
    }
    if (auto lg = log::get(log::Area::Shell))
        lg->info("edit {} = {} on {} item(s): {}", key, choice, m_inspected.size(), did ? "step" : "no change");
}

// Frames picked on the canvas: the inspector shows them (shared fields across
// several); nothing picked: back to the opened pages.
void Shell::on_canvas_selection() {  // handler
    auto& cv = m_canvas_view.canvas();
    // s011: typing in a frame -- the inspector is about the text.
    if (cv.editing()) {
        const auto* st = m_doc.as<core::Story>(cv.edit_story_id());
        const auto [a, b] = cv.edit_range();
        m_inspected = {cv.edit_story_id()};
        m_inspector.show("Text \u2014 " + (st ? st->name : std::string("story")), "Editing",
                         core::text_fields(m_doc, cv.edit_story_id(), a, b));
        return;
    }
    const auto& sel = cv.selection();
    if (sel.empty()) {
        m_inspected = m_pick.items;
        m_inspector.show_items(resolve(m_pick.items));
        return;
    }
    m_inspected = sel;
    const auto items = core::frame_items(m_doc, sel, m_project_dir);
    std::vector<const core::Item*> ptrs;
    for (const auto& it : items) ptrs.push_back(&it);
    m_inspector.show(core::frames_title(items),
                     items.size() == 1 ? m_doc.as<core::Frame>(sel.front())->owner.rfind("master", 0) == 0
                                             ? "On a master" : "Frame"
                                       : "Shared fields \u2014 Mixed where they differ",
                     core::shared_fields(ptrs));
}

// After undo / redo, frames the step touched come back selected on the canvas.
static bool all_frames(const core::Doc& d, const std::vector<std::string>& ids) {
    return !ids.empty() && std::all_of(ids.begin(), ids.end(),
                                       [&](const std::string& id) { return d.as<core::Frame>(id) != nullptr; });
}

void Shell::on_undo() {  // handler: take the last step back
    const auto r = m_journal.undo();
    if (!r.done) return;
    if (all_frames(m_doc, r.selection)) m_canvas_view.canvas().set_selection(r.selection);
    if (auto lg = log::get(log::Area::Shell)) lg->info("undo: {}", r.label);
}

void Shell::on_redo() {  // handler: put the undone step back
    const auto r = m_journal.redo();
    if (!r.done) return;
    if (all_frames(m_doc, r.selection)) m_canvas_view.canvas().set_selection(r.selection);
    if (auto lg = log::get(log::Area::Shell)) lg->info("redo: {}", r.label);
}

}  // namespace composix
