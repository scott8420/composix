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
#include "layout/Export.hpp"

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
            if (dir && !dir->get_path().empty()) {
                const std::string path = dir->get_path();
                confirm_discard([this, path] { open_project(path); });   // s019
            }
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
    // s063: one or several. A picked image frame takes the first; the rest
    // (or all) ride on the cursor, a click each.
    pick_images([this](std::vector<core::ImageFile> files) {
        if (files.empty()) return;
        auto& cv = m_canvas_view.canvas();
        const auto& sel = cv.selection();
        if (sel.size() == 1) {
            if (const auto* fr = m_doc.as<core::Frame>(sel.front()); fr && fr->kind == core::FrameKind::Image) {
                core::doc::place_image(m_journal, m_doc, sel.front(), files.front());
                files.erase(files.begin());
                if (files.empty()) return;
            }
        }
        if (m_main.get_visible_child_name() != "canvas") {
            alert("Open a page to place on", "Pick a page in the navigator, then Place again — or drop the files on it.");
            return;
        }
        cv.load_images(std::move(files));
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

// ── PDF (s013; regrouped s086) ─────────────────────────────────────────────
// The Export sheet: Print / Screen across the top; Pages and Navigation on
// the left, Colour on the right; a card saying what you'll get (pages ->
// sheets, the colour path, bookmarks and links), live; Export... names the
// file. A bad page range says why in the card and holds Export back.
namespace {
// A descendant by name (the sheet's widgets live in nested boxes).
Gtk::Widget* find_named(Gtk::Widget* w, const Glib::ustring& name) {
    if (!w) return nullptr;
    if (w->get_name() == name) return w;
    for (auto* c = w->get_first_child(); c; c = c->get_next_sibling())
        if (auto* f = find_named(c, name)) return f;
    return nullptr;
}
template <class W> W* named(Gtk::Widget* root, const char* name) { return dynamic_cast<W*>(find_named(root, name)); }

Gtk::Label* section_head(const char* text, const char* name) {
    auto* h = Gtk::make_managed<Gtk::Label>(text);
    h->set_name(name);
    h->add_css_class("cx-export-head");
    h->set_xalign(0);
    return h;
}
// A labelled row: the label in a fixed column, then the control.
Gtk::Box* field_row(const char* label, Gtk::Widget& w, const char* name) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_name(name);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->add_css_class("cx-field-label");
    l->set_xalign(0);
    l->set_size_request(64, -1);
    row->append(*l);
    w.set_hexpand(true);
    row->append(w);
    return row;
}
}  // namespace

void Shell::on_export_pdf() {  // handler
    if (!m_export) {
        m_export = std::make_unique<Gtk::Window>();
        auto& w = *m_export;
        w.set_name("export");
        w.set_title("Export PDF");
        w.set_modal(true);
        w.set_resizable(false);
        w.set_hide_on_close(true);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        box->set_name("export.box");
        box->add_css_class("cx-export");
        box->set_margin(20);
        box->set_size_request(580, -1);   // steady as the controls change
        // Title, and the writer: Print (Plate -- page boxes, CMYK, subset
        // fonts) or Screen (cairo -- RGB), as a switch (s015).
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto head = Gtk::make_managed<Gtk::Label>("Export PDF");
        head->set_name("export.title");
        head->add_css_class("cx-export-title");
        head->set_xalign(0);
        head->set_hexpand(true);
        top->append(*head);
        auto mode = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        mode->add_css_class("linked");
        auto print = Gtk::make_managed<Gtk::ToggleButton>("For print");
        print->set_name("export.print");
        print->set_tooltip_text("Trim and bleed boxes, CMYK colour, fonts subset -- for a printer");
        auto screen = Gtk::make_managed<Gtk::ToggleButton>("For screen");
        screen->set_name("export.screen");
        screen->set_tooltip_text("RGB, for reading on a screen and sharing");
        screen->set_group(*print);
        mode->append(*print);
        mode->append(*screen);
        top->append(*mode);
        box->append(*top);

        auto cols = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 28);
        cols->set_name("export.cols");
        cols->set_homogeneous(true);
        auto left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        auto right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        cols->append(*left);
        cols->append(*right);
        box->append(*cols);

        // ── Pages (s054 range, s084 spreads, s013 bleed / marks).
        left->append(*section_head("PAGES", "export.head.pages"));
        auto range = Gtk::make_managed<Gtk::Entry>();
        range->set_name("export.range");
        range->set_placeholder_text("All — or iii-7, 12");
        range->set_tooltip_text("Folios as the pages show them; commas between, a hyphen for a run");
        left->append(*field_row("Pages", *range, "export.range.row"));
        auto spreads = Gtk::make_managed<Gtk::CheckButton>("Facing pages as spreads");
        spreads->set_name("export.spreads");
        spreads->set_tooltip_text("Each spread one wide page, bleed on its outer edges; for viewing a book as it opens");
        left->append(*spreads);
        auto bleed = Gtk::make_managed<Gtk::CheckButton>();
        bleed->set_name("export.bleed");
        left->append(*bleed);
        auto marks = Gtk::make_managed<Gtk::CheckButton>("Crop marks");
        marks->set_name("export.marks");
        left->append(*marks);

        // ── Navigation (s054 / s082 bookmarks; s083 links are automatic).
        auto navh = section_head("NAVIGATION", "export.head.nav");
        navh->set_margin_top(8);
        left->append(*navh);
        auto marksb = Gtk::make_managed<Gtk::CheckButton>("Bookmarks from");
        marksb->set_name("export.bookmarks");
        marksb->set_tooltip_text("The entries these lists gather, as the PDF's outline -- the first list on top, each further one under its name");
        marksb->set_active(true);
        left->append(*marksb);
        auto lists = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);   // refilled on each open
        lists->set_name("export.lists");
        lists->set_margin_start(24);
        left->append(*lists);
        // s089: shown when a contents list is out of date.
        auto rebuild = Gtk::make_managed<Gtk::CheckButton>("Rebuild the contents first");
        rebuild->set_name("export.rebuild");
        rebuild->set_tooltip_text("A contents list no longer matches its headings -- rebuild it (one undo step) before writing");
        left->append(*rebuild);

        // ── Colour (s016 profile / convert, s085 intent, s017 standard).
        right->append(*section_head("COLOUR", "export.head.colour"));
        std::vector<Glib::ustring> names{"None (device CMYK)"};
        m_profiles.clear();
        m_profile_names.clear();
        for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) {
            m_profiles.push_back(p.path);
            m_profile_names.push_back(p.name);
            names.push_back(p.name);
        }
        auto prof = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
        prof->set_name("export.profile");
        {   // a long list: type to find one
            GtkExpression* ex = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
            gtk_drop_down_set_expression(prof->gobj(), ex);
            gtk_expression_unref(ex);
            gtk_drop_down_set_enable_search(prof->gobj(), TRUE);
        }
        prof->set_tooltip_text("The press's CMYK profile -- from ~/.local/share/icc and the system's colour folders");
        right->append(*field_row("Profile", *prof, "export.profile.row"));
        auto conv = Gtk::make_managed<Gtk::CheckButton>("Convert RGB to the press's CMYK");
        conv->set_name("export.convert");
        conv->set_sensitive(false);   // until a profile is picked
        right->append(*conv);
        auto intent = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Relative colorimetric", "Perceptual"}));
        intent->set_name("export.intent");
        intent->set_sensitive(false);   // until converting
        intent->set_tooltip_text("Relative colorimetric: colours the press can print stay exact, the rest clip to its edge.\n"
                                 "Perceptual: everything eases in together, keeping their relations.\nThe Colour Report shows either.");
        right->append(*field_row("Intent", *intent, "export.intent.row"));
        auto stdd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"None", "PDF/X-1a:2003", "PDF/X-4"}));
        stdd->set_name("export.standard");
        stdd->set_tooltip_text("PDF/X-1a: CMYK and spot only, no transparency.\nPDF/X-4: ICC-tagged RGB allowed.\nEither needs a profile.");
        right->append(*field_row("Standard", *stdd, "export.standard.row"));
        auto screen_note = Gtk::make_managed<Gtk::Label>("A screen PDF is RGB: no profile, no conversion.");
        screen_note->set_name("export.screen_note");
        screen_note->add_css_class("cx-export-note");
        screen_note->set_xalign(0);
        screen_note->set_wrap(true);
        right->append(*screen_note);

        // ── What you'll get.
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
        card->set_name("export.summary");
        card->add_css_class("cx-export-summary");
        for (const char* n : {"export.summary.size", "export.summary.colour", "export.summary.nav"}) {
            auto l = Gtk::make_managed<Gtk::Label>();
            l->set_name(n);
            l->set_xalign(0);
            l->set_wrap(true);
            card->append(*l);
        }
        static_cast<Gtk::Label*>(card->get_first_child())->add_css_class("cx-export-summary-lead");
        box->append(*card);

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

        // Which controls are live, and the card -- after any change.
        m_export_sync = [this, print, prof, conv, intent, stdd, screen_note, range, spreads, bleed, marksb, lists, rebuild, card, go] {
            const bool on = print->get_active(), pro = on && prof->get_selected() > 0;
            prof->set_sensitive(on);
            stdd->set_sensitive(pro);
            if (!pro && stdd->get_selected() != 0) stdd->set_selected(0);
            const bool x1a = pro && stdd->get_selected() == 1;
            if (x1a && !conv->get_active()) conv->set_active(true);
            conv->set_sensitive(pro && !x1a);
            intent->set_sensitive(pro && conv->get_active());   // s085: only a conversion has an intent
            screen_note->set_visible(!on);
            lists->set_sensitive(marksb->get_active());
            spreads->set_sensitive(core::any_facing(m_doc));
            // The card.
            auto* size = static_cast<Gtk::Label*>(card->get_first_child());
            auto* colour = static_cast<Gtk::Label*>(size->get_next_sibling());
            auto* nav = static_cast<Gtk::Label*>(colour->get_next_sibling());
            std::string why;
            const auto only = core::pages_in_range(m_doc, std::string(range->get_text()), &why);
            if (only.empty()) {
                range->add_css_class("error");
                size->set_text(why.empty() ? "No pages" : why);
                size->add_css_class("cx-export-bad");
                colour->set_visible(false);
                nav->set_visible(false);
                go->set_sensitive(false);
                return;
            }
            range->remove_css_class("error");
            size->remove_css_class("cx-export-bad");
            colour->set_visible(true);
            nav->set_visible(true);
            go->set_sensitive(true);
            layout::PdfOptions o;
            o.only = only.size() == m_doc.pages().size() ? std::vector<std::string>{} : only;
            o.spreads = spreads->get_active() && core::any_facing(m_doc);
            o.bleed = bleed->get_active();
            o.convert = conv->get_active();
            o.perceptual = intent->get_selected() == 1;
            o.standard = !pro ? layout::plate_standard::None
                       : stdd->get_selected() == 1 ? layout::plate_standard::X1a
                       : stdd->get_selected() == 2 ? layout::plate_standard::X4 : layout::plate_standard::None;
            o.bookmarks = marksb->get_active();
            bool any = false;
            for (auto* k = lists->get_first_child(); k; k = k->get_next_sibling())
                if (auto* cb = dynamic_cast<Gtk::CheckButton*>(k)) {
                    any = true;
                    if (cb->get_active()) o.mark_lists.push_back(std::string(k->get_name()).substr(std::string("export.list.").size()));
                }
            if (any && o.mark_lists.empty()) o.bookmarks = false;
            const auto i = prof->get_selected();
            const std::string pname = (pro && i - 1 < m_profile_names.size()) ? m_profile_names[i - 1] : std::string{};
            m_pf_flows.finish_now(m_doc);   // s106
            const auto s = layout::export_summary(m_doc, m_pf_flows, o, on, pname);
            size->set_text(s.size);
            colour->set_text(s.colour);
            nav->set_text(s.nav + (rebuild->get_visible() && rebuild->get_active() ? " \u00b7 contents rebuilt first" : ""));
        };
        const auto sync = [this] { if (m_export_sync) m_export_sync(); };
        for (auto* t : {static_cast<Gtk::CheckButton*>(spreads), bleed, conv, marksb, rebuild})
            t->signal_toggled().connect(sync);
        print->signal_toggled().connect(sync);
        for (auto* dd : {prof, intent, stdd}) dd->property_selected().signal_changed().connect(sync);
        range->signal_changed().connect(sync);

        cancel->signal_clicked().connect([this] { m_export->set_visible(false); });
        auto keys = Gtk::EventControllerKey::create();   // Esc closes the sheet
        keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
            if (kv != GDK_KEY_Escape) return false;
            m_export->set_visible(false);
            return true;
        }, false);
        w.add_controller(keys);
        w.set_default_widget(*go);
        range->signal_activate().connect([go] { if (go->get_sensitive()) go->activate(); });
        go->signal_clicked().connect([this, bleed, marks, spreads, print, prof, conv, stdd, intent, range, marksb, lists, rebuild] {
            // s054: the range first (the card has said why if it's bad).
            {
                const auto text = std::string(range->get_text());
                auto only = core::pages_in_range(m_doc, text);
                if (only.empty()) return;
                m_pdf_only = only.size() == m_doc.pages().size() ? std::vector<std::string>{} : only;
                m_pdf_bookmarks = marksb->get_active();
                {   // s082: the ticked lists; none ticked (with lists to tick) = no bookmarks
                    std::vector<std::string> picked;
                    bool any = false;
                    for (auto* k = lists->get_first_child(); k; k = k->get_next_sibling())
                        if (auto* cb = dynamic_cast<Gtk::CheckButton*>(k)) {
                            any = true;
                            if (cb->get_active()) picked.push_back(std::string(k->get_name()).substr(std::string("export.list.").size()));
                        }
                    if (any && picked.empty()) m_pdf_bookmarks = false;
                    if (!picked.empty()) m_pdf_mark_lists = picked;
                }
                m_pdf_range = text;   // s077: kept with the document below
            }
            m_pdf_bleed = bleed->get_active();
            m_pdf_marks = marks->get_active();
            m_pdf_spreads = spreads->get_active() && core::any_facing(m_doc);   // s084
            m_pdf_print = print->get_active();
            const auto i = prof->get_selected();
            m_pdf_profile = (i > 0 && i - 1 < m_profiles.size()) ? m_profiles[i - 1] : std::string{};
            update_screen_colours();   // s107
            m_pdf_convert = conv->get_active();
            m_pdf_perceptual = intent->get_selected() == 1;   // s085
            m_pdf_rebuild = rebuild->get_active();             // s089
            m_pdf_standard = (m_pdf_print && !m_pdf_profile.empty()) ? static_cast<int>(stdd->get_selected()) : 0;
            // s032: kept with the document, the profile by its name.
            {
                core::ExportPrefs e{m_pdf_print, m_pdf_bleed, m_pdf_marks, {}, m_pdf_convert, m_pdf_standard,
                                    m_pdf_range, m_pdf_bookmarks, m_pdf_mark_lists, m_pdf_spreads,
                                    m_pdf_perceptual, m_pdf_rebuild};
                for (std::size_t k = 0; k < m_profiles.size(); ++k)
                    if (m_profiles[k] == m_pdf_profile) e.profile = m_profile_names[k];
                m_profile_missing.clear();
                core::doc::set_export(m_journal, m_doc, e);
            }
            if (rebuild->get_visible() && m_pdf_rebuild) {   // s089: its own undo step, before the file
                bool any = false;
                m_pf_flows.finish_now(m_doc);   // s106
                for (const auto& s : layout::stale_lists(m_doc, m_pf_flows)) any = layout::build_list(m_journal, m_doc, s.list) || any;
                if (any) queue_refresh();
            }
            update_preflight();   // s024: the standard's checks follow
            if (m_proofing) apply_proof(true);   // s099: the proof follows the export's profile and intent
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
    // Refresh the sheet from the document each time it opens.
    auto* root = m_export->get_child();
    const auto saved = m_export_sync;
    m_export_sync = nullptr;   // quiet while filling
    if (auto* b = named<Gtk::CheckButton>(root, "export.bleed")) {
        double lo = 1e9, hi = 0;   // s147: each master's bleed
        for (const auto& p : m_doc.pages()) { const double v = core::bleed_of(m_doc, p); lo = std::min(lo, v); hi = std::max(hi, v); }
        b->set_label(lo == hi || m_doc.pages().empty() ? "Bleed (" + core::length_text(m_doc.pages().empty() ? m_doc.settings().bleed : hi, m_doc.settings().units) + ")"
                                                      : std::string("Bleed (each master's)"));
        b->set_active(m_pdf_bleed);
    }
    if (auto* b = named<Gtk::CheckButton>(root, "export.marks")) b->set_active(m_pdf_marks);
    if (auto* b = named<Gtk::ToggleButton>(root, "export.print")) b->set_active(m_pdf_print);
    if (auto* b = named<Gtk::ToggleButton>(root, "export.screen")) b->set_active(!m_pdf_print);
    if (auto* b = named<Gtk::CheckButton>(root, "export.spreads")) b->set_active(m_pdf_spreads && core::any_facing(m_doc));
    if (auto* b = named<Gtk::CheckButton>(root, "export.bookmarks")) b->set_active(m_pdf_bookmarks);
    if (auto* e = named<Gtk::Entry>(root, "export.range")) e->set_text(m_pdf_range);
    if (auto* dd = named<Gtk::DropDown>(root, "export.profile")) {
        guint at = 0;
        for (std::size_t n = 0; n < m_profiles.size(); ++n)
            if (m_profiles[n] == m_pdf_profile) at = static_cast<guint>(n + 1);
        dd->set_selected(at);
    }
    if (auto* b = named<Gtk::CheckButton>(root, "export.convert")) b->set_active(m_pdf_convert);
    if (auto* dd = named<Gtk::DropDown>(root, "export.standard")) dd->set_selected(static_cast<guint>(m_pdf_standard));
    if (auto* dd = named<Gtk::DropDown>(root, "export.intent")) dd->set_selected(m_pdf_perceptual ? 1u : 0u);   // s085
    if (auto* b = named<Gtk::CheckButton>(root, "export.rebuild")) {   // s089: only when something's out of date
        std::string names;
        m_pf_flows.finish_now(m_doc);   // s106
        for (const auto& s : layout::stale_lists(m_doc, m_pf_flows))
            names += (names.empty() ? "" : ", ") + m_doc.as<core::GenList>(s.list)->name;
        b->set_visible(!names.empty());
        b->set_label("Rebuild out-of-date contents first");
        b->set_tooltip_text("Out of date: " + names + ". Rebuilt (one undo step) before the file is written.");
        b->set_active(m_pdf_rebuild);
    }
    if (auto* lb = named<Gtk::Box>(root, "export.lists")) {   // s082: the document's lists, as they are now
        while (auto* k = lb->get_first_child()) lb->remove(*k);
        const auto all = layout::bookmark_lists(m_doc);
        const auto used = layout::bookmark_lists_used(m_doc, m_pdf_mark_lists);
        for (const auto& lid : all) {
            const auto* l = m_doc.as<core::GenList>(lid);
            auto* cb = Gtk::make_managed<Gtk::CheckButton>(l->name.empty() ? std::string("List") : l->name);
            cb->set_name("export.list." + lid);
            cb->set_active(std::find(used.begin(), used.end(), lid) != used.end());
            cb->signal_toggled().connect([this] { if (m_export_sync) m_export_sync(); });
            lb->append(*cb);
        }
        if (all.empty()) {
            auto* none = Gtk::make_managed<Gtk::Label>("No list gathers headings yet");
            none->set_name("export.lists.none");
            none->add_css_class("cx-export-note");
            none->set_xalign(0);
            lb->append(*none);
        }
    }
    m_export_sync = saved;
    if (m_export_sync) m_export_sync();
    m_export->set_transient_for(*this);
    m_export->present();
}

// Closing with unsaved changes asks first. "Save" on a never-saved document
// opens Save As and keeps the window; close again once it is saved.
bool Shell::on_close_request() {  // handler
    save_window();   // s057: whether or not the close goes ahead -- it's how the window is now
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
    confirm_discard([this, path] { open_project(path); });   // s019
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
    const auto keep = std::move(m_up_keep);   // s068: going up, the items just open stay picked
    m_up_keep.clear();
    if (!m_moving) { m_fwd_items.clear(); m_fwd_from.reset(); }   // s068: a pick of your own ends Forward
    show_pick(p, keep);
}

// s068: up a level -- the open items' section, as tiles, with them picked.
void Shell::on_go_up() {  // handler
    if (m_pick.segment || m_pick.items.empty()) return;
    const auto parent = core::parent_of(m_outline, m_pick.items);
    if (!parent) return;
    if (auto lg = log::get(log::Area::Shell)) lg->info("up: to {}", core::segment_info(*parent).key);
    m_up_keep = m_pick.items;
    m_fwd_items = m_pick.items;   // Forward goes back in
    m_fwd_from = parent;
    m_moving = true;
    m_nav.select_section(*parent);
    m_moving = false;
    m_up_keep.clear();
    sync_up();
}

// s070: the picked frames up / down the stack -- one step.
void Shell::on_arrange(core::doc::Arrange how) {  // handler
    auto& cv = m_canvas_view.canvas();
    if (m_main.get_visible_child_name() != "canvas" || cv.editing() || cv.selection().empty()) return;
    const bool did = core::doc::arrange(m_journal, m_doc, cv.selection(), how);
    if (auto lg = log::get(log::Area::Shell)) lg->info("arrange {} on {} frame(s): {}", static_cast<int>(how), cv.selection().size(), did ? "step" : "no change");
}

// ── s074: the canvas's right-click menu ─────────────────────────────────────
void Shell::bind_context_actions() {  // bindings
    auto picked = [this] { return m_canvas_view.canvas().selection(); };
    add_action("delete-frames", [this] { m_canvas_view.canvas().delete_selection(); });
    add_action_with_parameter("frame-wrap", Glib::Variant<Glib::ustring>::variant_type(), [this, picked](const Glib::VariantBase& p) {
        const int w = std::stoi(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(p).get());
        core::doc::edit_frames(m_journal, m_doc, picked(), "Text wrap", [&](core::Frame& f) {
            const auto to = static_cast<core::Wrap>(std::clamp(w, 0, 2));
            if (f.wrap == core::Wrap::None && to != core::Wrap::None && f.wrap_off == core::Sides{}) f.wrap_off = {9, 9, 9, 9};
            f.wrap = to;
        });
    });
    add_action("frame-ignore-wrap", [this, picked] {
        bool all = true;
        for (const auto& id : picked())
            if (const auto* f = m_doc.as<core::Frame>(id); f && f->kind == core::FrameKind::Text) all = all && f->ignore_wrap;
        core::doc::edit_frames(m_journal, m_doc, picked(), "Ignore text wrap", [&](core::Frame& f) {
            if (f.kind == core::FrameKind::Text) f.ignore_wrap = !all;
        });
    });
    add_action("remove-override", [this, picked] { core::doc::remove_overrides(m_journal, m_doc, picked()); });
    add_action("override-item", [this] {
        if (m_ctx.page.empty() || m_ctx.master_item.empty()) return;
        const auto made = core::doc::override_master_item(m_journal, m_doc, m_ctx.page, m_ctx.master_item);
        if (!made.empty()) {   // the copy, picked (as Ctrl+Shift+click)
            m_canvas_view.canvas().model_changed();
            m_canvas_view.canvas().set_selection({made});
            on_canvas_selection();
        }
    });
    add_action("override-page", [this] { if (!m_ctx.page.empty()) core::doc::override_all(m_journal, m_doc, {m_ctx.page}); });
    add_action("remove-page-overrides", [this] { if (!m_ctx.page.empty()) core::doc::remove_all_overrides(m_journal, m_doc, {m_ctx.page}); });
    add_action("page-add-after", [this] { if (!m_ctx.page.empty()) page_action("add", {m_ctx.page}); });
}

void Shell::on_canvas_context(const views::Canvas::ContextHit& h) {  // handler
    m_ctx = h;
    auto menu = Gio::Menu::create();
    if (!h.frame.empty()) {
        const auto& sel = m_canvas_view.canvas().selection();
        bool text = false, copy = false, ignoring = true;
        for (const auto& id : sel)
            if (const auto* f = m_doc.as<core::Frame>(id)) {
                if (f->kind == core::FrameKind::Text) { text = true; ignoring = ignoring && f->ignore_wrap; }
                copy = copy || !f->overrides.empty();
            }
        auto top = Gio::Menu::create();
        if (text) top->append("Story Editor", "win.story-editor");
        if (copy) top->append(sel.size() == 1 ? "Remove Override" : "Remove Overrides", "win.remove-override");
        if (top->get_n_items() > 0) menu->append_section(top);
        auto arrange = Gio::Menu::create();
        arrange->append_submenu("Arrange", m_arrange_menu);
        auto wrap = Gio::Menu::create();
        wrap->append("None", "win.frame-wrap::0");
        wrap->append("Wrap Around", "win.frame-wrap::1");
        wrap->append("Jump Over", "win.frame-wrap::2");
        arrange->append_submenu("Text Wrap", wrap);
        if (text) arrange->append(ignoring ? "Let Wrap Push the Text" : "Ignore Others' Wrap", "win.frame-ignore-wrap");
        menu->append_section(arrange);
        auto del = Gio::Menu::create();
        del->append(sel.size() == 1 ? "Delete Frame" : "Delete Frames", "win.delete-frames");
        menu->append_section(del);
    } else if (!h.page.empty() && m_doc.as<core::Page>(h.page)) {
        auto page = Gio::Menu::create();
        if (!h.master_item.empty()) page->append("Override This Master Item", "win.override-item");
        if (!core::doc::overridable_items(m_doc, h.page).empty()) page->append("Override All Master Items", "win.override-page");
        bool own = false;
        for (const auto& f : m_doc.frames_on(h.page)) own = own || !m_doc.as<core::Frame>(f)->overrides.empty();
        if (own) page->append("Remove All Overrides", "win.remove-page-overrides");
        if (page->get_n_items() > 0) menu->append_section(page);
        auto more = Gio::Menu::create();
        more->append("Add Page After", "win.page-add-after");
        more->append("Fit the Pages", "win.zoom-fit");
        menu->append_section(more);
    } else {
        auto more = Gio::Menu::create();
        more->append("Fit the Pages", "win.zoom-fit");
        menu->append_section(more);
    }
    auto& canvas = m_canvas_view.canvas();
    if (!m_ctx_menu) {
        m_ctx_menu = std::make_unique<Gtk::PopoverMenu>(menu, Gtk::PopoverMenu::Flags::NESTED);
        m_ctx_menu->set_name("canvas.context");
        m_ctx_menu->set_has_arrow(false);
        m_ctx_menu->set_parent(canvas);
    } else {
        m_ctx_menu->set_menu_model(menu);
    }
    m_ctx_menu->set_pointing_to(Gdk::Rectangle(static_cast<int>(h.x), static_cast<int>(h.y), 1, 1));
    // A hand-parented popover is given less than its natural height (seen
    // under Xvfb: content 103 px in a 90 px scroller, the last item cut), and
    // can't be measured before it's up (0). So: up, then measured, asked for
    // its natural height, presented again.
    m_ctx_menu->set_size_request(-1, -1);
    m_ctx_menu->popup();
    Glib::signal_idle().connect_once([this] {
        if (!m_ctx_menu || !m_ctx_menu->get_visible()) return;
        int mn = 0, nat = 0, b1 = 0, b2 = 0;
        m_ctx_menu->measure(Gtk::Orientation::VERTICAL, -1, mn, nat, b1, b2);
        if (nat > m_ctx_menu->get_height()) {
            m_ctx_menu->set_size_request(-1, nat);
            m_ctx_menu->present();
        }
    });
}

// s068: Forward -- back into what Back left, while its section is shown.
void Shell::on_go_forward() {  // handler
    if (m_fwd_items.empty() || !m_fwd_from || m_pick.segment != m_fwd_from) return;
    const auto items = m_fwd_items;
    if (auto lg = log::get(log::Area::Shell)) lg->info("forward: {} item(s)", items.size());
    m_fwd_items.clear();
    m_fwd_from.reset();
    m_moving = true;
    m_nav.select_items(items);
    m_moving = false;
    sync_up();
}

void Shell::on_tiles_selection(const std::vector<std::string>& ids) {  // handler
    m_inspected = ids;
    if (ids.empty() && m_segment) m_inspector.show_section(m_outline, *m_segment, section_fields(*m_segment));
    else                          m_inspector.show_items(resolve(ids));
}

// Opening tiles = selecting those items in the navigator, so there is ONE
// route to "open": the navigator's pick (the tile view never opens on its own).
void Shell::on_tiles_open(const std::vector<std::string>& ids) {  // handler
    // s058: a swatch opens in its editor.
    if (!ids.empty() && m_doc.as<core::Swatch>(ids.front())) { open_swatch_editor(ids.front()); return; }
    m_nav.select_items(ids);
}

// An inspector edit is ONE verb across everything the inspector shows -- one
// journal step, one Ctrl+Z (REQUIREMENTS §10). The views repaint from the
// document's change (queue_refresh), not from here.
// s099: the canvas through the press: the export's profile (else the first
// installed CMYK one) and its intent. Off: back to screen colours.
bool Shell::apply_proof(bool on) {  // helper
    m_proofing = false;
    if (!on) {
        m_canvas_view.canvas().set_proof(nullptr);
        m_canvas_view.set_proof_label("");
        return true;
    }
    std::string path = m_pdf_profile, name;
    for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) {
        if (path.empty()) path = p.path;
        if (p.path == path) { name = p.name; break; }
    }
    if (path.empty()) return false;
    auto proof = std::make_shared<plate::Proof>();
    if (!proof->load(path, m_pdf_perceptual, nullptr, m_proof_paper)) return false;   // s107: the paper too
    m_proofing = true;
    m_canvas_view.canvas().set_proof(proof);
    m_canvas_view.set_proof_label((name.empty() ? proof->name() : name) + (m_proof_paper ? " \u00b7 paper" : ""));
    return true;
}

// s107: CMYK swatches (and spots' alternates) drawn through the press
// profile -- the export's, else the first installed; the simple ink model
// when there is none. The drawn pages' caches key on core::screen_gen().
void Shell::update_screen_colours() {  // helper
    std::string path = m_pdf_profile;
    if (path.empty())
        for (const auto& p : plate::find_cmyk_profiles(profile_dirs())) { path = p.path; break; }
    if (path == m_screen_profile) return;
    m_screen_profile = path;
    auto pr = std::make_shared<plate::Proof>();
    if (!path.empty() && pr->load(path)) {
        core::doc::set_screen_cmyk([pr](const double cmyk[4]) { return pr->cmyk_to_screen(cmyk); });
        if (auto lg = log::get(log::Area::Shell)) lg->info("CMYK on screen through '{}'", pr->name());
    } else {
        core::doc::set_screen_cmyk({});
    }
    queue_refresh();
    m_canvas_view.canvas().queue_draw();
}

// s097: Links & Assets, nothing picked -- the files in assets/ nothing uses
// (not the document, not what undo could bring back), and a way to clear
// them: the desktop's Trash, so a mistake can be fetched back.
std::vector<core::Field> Shell::section_fields(core::Segment s) const {  // helper
    std::vector<core::Field> out;
    if (s == core::Segment::Notes) {   // s121: the settings, in their panel
        core::Field act{"", "", "notes_panel", {{"open", "Footnotes & Endnotes\u2026"}}};
        act.buttons = true;
        out.push_back(act);
        return out;
    }
    if (s != core::Segment::Assets || m_project_dir.empty()) return out;
    std::vector<std::string> held;
    m_journal.each_value([&](const core::Record& r) {
        if (const auto* a = std::get_if<core::Asset>(&r)) held.push_back(a->path);
    });
    const auto files = core::unused_asset_files(m_doc, m_project_dir, held);
    if (files.empty()) {
        out.push_back({"Unused files", "None in assets/"});
        return out;
    }
    std::uintmax_t total = 0;
    std::string names;
    for (std::size_t k = 0; k < files.size(); ++k) {
        total += files[k].bytes;
        if (k < 6) names += (names.empty() ? "" : "\n") + std::filesystem::path(files[k].path).filename().string();
    }
    if (files.size() > 6) names += "\n\u2026 and " + std::to_string(files.size() - 6) + " more";
    out.push_back({"Unused files", std::to_string(files.size()) + " \u2014 " + core::bytes_text(total)});
    out.push_back({"In assets/", names});
    core::Field act{"", "", "assets_unused", {{"trash", "Move to Trash"}}};
    act.buttons = true;
    out.push_back(act);
    return out;
}

void Shell::on_inspector_edit(const std::string& key, const std::string& choice) {  // handler
    if (key == "assets_unused" && choice == "trash") {   // s097: not a document edit -- files on disk
        std::vector<std::string> held;
        m_journal.each_value([&](const core::Record& r) {
            if (const auto* a = std::get_if<core::Asset>(&r)) held.push_back(a->path);
        });
        int moved = 0;
        for (const auto& f : core::unused_asset_files(m_doc, m_project_dir, held)) {
            try {
                if (Gio::File::create_for_path(f.path)->trash()) ++moved;
            } catch (const Glib::Error& e) {
                if (auto lg = log::get(log::Area::Io)) lg->warn("assets: can't trash {} ({})", f.path, e.what());
            }
        }
        if (auto lg = log::get(log::Area::Io)) lg->info("assets: {} unused file(s) to the Trash", moved);
        on_tiles_selection(m_inspected);
        return;
    }
    if (key == "notes_panel") { open_notes_panel(); return; }   // s121
    if (m_inspected.empty()) return;
    if (key == "note" || key == "note_words") {   // s115: a footnote picked in Footnotes
        std::string story;
        int p = 0, k = 0;
        if (key == "note") {
            if (choice == "show" && core::parse_note_item_id(m_inspected.front(), story, p, k)) go_to_note(m_inspected.front());
            return;
        }
        int n = 0;
        for (const auto& id : m_inspected)
            if (core::parse_note_item_id(id, story, p, k) && core::doc::set_note_text(m_journal, m_doc, story, p, k, choice)) ++n;
        if (auto lg = log::get(log::Area::Shell)) lg->info("footnotes: words set on {}", n);
        return;
    }
    bool did = false;
    try {
        if (const auto label = core::doc::style_edit_label(key); !label.empty()) {   // s164: a style's setting (one code path, the preview's too)
            did = core::doc::edit_styles(m_journal, m_doc, m_inspected, label, [&](core::Style& st) { core::doc::style_edit(m_doc, st, key, choice); });
        } else if (key == "master") {
            did = core::doc::set_master(m_journal, m_doc, m_inspected, choice);
        } else if (key == "size") {
            // s148: a page's presets are its master's size -- pages grouped by master, each its own.
            const auto i = static_cast<std::size_t>(std::stoi(choice));
            std::map<std::string, std::vector<std::string>> by;
            for (const auto& id : m_inspected) if (const auto* p = m_doc.as<core::Page>(id)) by[p->master].push_back(id);
            for (const auto& [m, ids] : by) {
                const auto presets = core::doc::size_presets(m_doc, ids.front());
                if (i < presets.size()) did = core::doc::set_page_size(m_journal, m_doc, ids, presets[i]) || did;
            }
        } else if (key == "numbering") {
            did = core::doc::set_numbering(m_journal, m_doc, m_inspected,
                                           static_cast<core::Numbering>(std::stoi(choice)));
        } else if (key == "dtitle") {   // s066: the document's settings
            did = core::doc::set_title(m_journal, m_doc, choice);
        } else if (key == "dtrim") {   // "0:<points>" (W) or "1:<points>" (H)
            const auto colon = choice.find(':');
            const int k = std::stoi(choice.substr(0, colon));
            const double v = std::stod(choice.substr(colon + 1));
            const auto& st = m_doc.settings();
            did = core::doc::set_trim(m_journal, m_doc, k == 0 ? v : st.trim_w, k == 1 ? v : st.trim_h);
        } else if (key == "dbleed") {
            did = core::doc::set_bleed(m_journal, m_doc, std::stod(choice));
        } else if (key == "dunits") {   // s073
            did = core::doc::set_units(m_journal, m_doc, static_cast<core::Units>(std::clamp(std::stoi(choice), 0, 3)));
        } else if (key == "dorigin") {   // s154
            did = core::doc::set_ruler_origin(m_journal, m_doc, static_cast<core::RulerOrigin>(std::clamp(std::stoi(choice), 0, 2)));
        } else if (key == "dwrap") {   // s069
            did = core::doc::set_wrap_beneath(m_journal, m_doc, choice == "1");
        } else if (key == "ignore_wrap") {   // s069
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, "Ignore text wrap", [&](core::Frame& f) {
                if (f.kind == core::FrameKind::Text) f.ignore_wrap = choice == "1";
            });
        } else if (key == "dnote_restart" || key == "dnote_style") {   // s111
            const int v = std::clamp(std::stoi(choice), 0, 3);
            auto r = m_doc.settings().note_restart;
            auto st = m_doc.settings().note_style;
            if (key == "dnote_restart") r = static_cast<core::NoteRestart>(std::min(v, 3));   // s124: chapter
            else st = static_cast<core::NoteStyle>(v);
            did = core::doc::set_note_numbering(m_journal, m_doc, r, st);
        } else if (key == "dnote_rule" || key == "dnote_rule_w" || key == "dnote_space" || key == "dnote_cont") {   // s118
            const auto& o = m_doc.settings();
            int rule = o.note_rule;
            double w = o.note_rule_w, sp = o.note_space;
            std::string cont = o.note_cont;
            if (key == "dnote_rule") rule = static_cast<int>(std::lround(std::stod(choice)));
            else if (key == "dnote_rule_w") w = std::stod(choice);
            else if (key == "dnote_space") sp = std::stod(choice);
            else cont = choice;
            did = core::doc::set_note_look(m_journal, m_doc, rule, w, sp, cont);
        } else if (key == "dnote_at" || key == "dnote_title") {   // s120
            auto at = m_doc.settings().note_at;
            auto title = m_doc.settings().note_title;
            if (key == "dnote_at") at = static_cast<core::NoteAt>(std::clamp(std::stoi(choice), 0, 2));   // s122: chapters
            else title = choice;
            did = core::doc::set_note_place(m_journal, m_doc, at, title);
        } else if (key == "dnote_num") {   // s126
            did = core::doc::set_note_num(m_journal, m_doc, static_cast<core::Settings::NoteNum>(std::clamp(std::stoi(choice), 0, 3)));
        } else if (key == "dnote_area") {   // s117
            did = core::doc::set_note_area(m_journal, m_doc, static_cast<int>(std::lround(std::stod(choice))));
        } else if (key == "dfacing") {
            did = core::doc::set_facing(m_journal, m_doc, choice == "1");
        } else if (key == "pmargins") {   // s062: a page's own margins, or back to the master's
            if (choice == "own") did = core::doc::edit_page_margins(m_journal, m_doc, m_inspected, "Page's own margins", [](core::Margins&) {});
            else did = core::doc::reset_page_margins(m_journal, m_doc, m_inspected);
        } else if (key == "ptop" || key == "pbottom" || key == "pinside" || key == "poutside") {
            const double v = std::stod(choice);
            did = core::doc::edit_page_margins(m_journal, m_doc, m_inspected, "Page margin", [&](core::Margins& m) {
                if (key == "ptop") m.top = v;
                else if (key == "pbottom") m.bottom = v;
                else if (key == "pinside") m.inside = v;
                else m.outside = v;
            });
        } else if (key == "sname") {   // s060: a section's name
            if (m_inspected.size() == 1) did = core::doc::rename_section(m_journal, m_doc, m_inspected.front(), choice);
        } else if (key == "sstart_at") {   // s060: its first page's number
            int n = 0;
            try { std::size_t used = 0; n = std::stoi(choice, &used); if (used != choice.size()) n = 0; } catch (const std::exception&) { n = 0; }
            if (n < 1 || n > 99999) { alert("Not a page number", "Starts at takes a whole number from 1 -- \u201c" + choice + "\u201d isn\u2019t one."); return; }
            did = core::doc::set_section_start_at(m_journal, m_doc, m_inspected, n);
        } else if (key == "smaster") {   // s060: the master its new pages take
            did = core::doc::set_section_master(m_journal, m_doc, m_inspected, choice);
        } else if (key == "section") {   // s060: Remove section
            if (choice == "remove")
                for (const auto& id : m_inspected) did = core::doc::remove_section(m_journal, m_doc, id) || did;
        } else if (key == "starts") {
            did = core::doc::set_starts(m_journal, m_doc, m_inspected,
                                        static_cast<core::Side>(std::stoi(choice)));
        } else if (key == "local" || key == "lsize" || key == "lcolor" || key == "lnobreak") {   // s030: formatting on the words
            if (key == "local" && (choice == "bold" || choice == "italic")) { activate_action(choice == "bold" ? "win.bold" : "win.italic"); return; }
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            auto [a, b] = cv.edit_range();
            const auto story = cv.edit_story_id();
            if (a == b) if (const auto* st = m_doc.as<core::Story>(story)) std::tie(a, b) = core::word_at(*st, a);
            if (key == "local")
                did = core::doc::set_local(m_journal, m_doc, story, a, b, "Clear overrides", [](core::Local& l) { l = {}; });
            else if (key == "lsize")
                did = core::doc::set_local(m_journal, m_doc, story, a, b, "Size", [&](core::Local& l) { l.size = std::stod(choice); });
            else if (key == "lnobreak")   // s159
                did = core::doc::set_local(m_journal, m_doc, story, a, b, "No break", [&](core::Local& l) { l.no_break = std::clamp(std::stoi(choice), -1, 1); });
            else
                did = core::doc::set_local(m_journal, m_doc, story, a, b, "Colour", [&](core::Local& l) { l.color = choice; });
            cv.grab_focus();
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
        } else if (key == "fill_tint" || key == "stroke_tint" || key == "opacity") {   // s021
            static const std::map<std::string, std::string> labels{
                {"fill_tint", "Fill tint"}, {"stroke_tint", "Stroke tint"}, {"opacity", "Opacity"}};
            const double v = std::stod(choice) / 100;
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Frame& f) {
                if (key == "fill_tint") f.fill_tint = v;
                else if (key == "stroke_tint") f.stroke_tint = v;
                else f.opacity = v;
            });
        } else if (key == "bounds") {   // s041: a frame's X / Y / W / H typed ("k:points")
            const auto colon = choice.find(':');
            const int k = std::stoi(choice.substr(0, colon));
            const double v = std::stod(choice.substr(colon + 1));
            static const char* labels[] = {"Move", "Move", "Resize", "Resize"};
            if (k >= 0 && k < 4)
                did = core::doc::edit_frames(m_journal, m_doc, m_inspected, labels[k], [&](core::Frame& f) {
                    const core::Zero z = core::ruler_zero(m_doc, f.owner, f.side);   // s154: typed from the rulers' zero
                    if (k == 0) f.x = v + z.x;
                    else if (k == 1) f.y = v + z.y;
                    else if (k == 2) f.w = std::max(1.0, v);
                    else f.h = std::max(1.0, v);
                });
        } else if (key == "style") {   // s011: Duplicate / Delete
            if (choice == "duplicate" && !m_inspected.empty()) {
                const auto made = core::doc::duplicate_style(m_journal, m_doc, m_inspected.front());
                did = !made.empty();
                if (did) Glib::signal_idle().connect_once([this, made] { m_tiles.show_section(m_outline, core::Segment::Styles, {made}); on_tiles_selection({made}); });
            } else if (choice == "delete") {
                did = core::doc::delete_styles(m_journal, m_doc, m_inspected);
            }
        } else if (key == "swatch") {   // s058: Edit... / Duplicate / Delete
            if (choice == "edit" && !m_inspected.empty()) {
                open_swatch_editor(m_inspected.front());
                return;
            }
            if (choice == "duplicate" && !m_inspected.empty()) {
                const auto made = core::doc::duplicate_swatch(m_journal, m_doc, m_inspected.front());
                did = !made.empty();
                if (did) Glib::signal_idle().connect_once([this, made] { m_tiles.show_section(m_outline, core::Segment::Colors, {made}); on_tiles_selection({made}); });
            } else if (choice == "delete") {
                for (const auto& id : m_inspected) did = core::doc::delete_swatch(m_journal, m_doc, id) || did;
            }
        } else if (key == "list_add" || key == "list_remove") {   // s088: the styles a list gathers
            for (const auto& id : m_inspected)
                if (const auto* l = m_doc.as<core::GenList>(id)) {
                    auto v = l->from_styles;
                    if (key == "list_add") v.push_back(choice);
                    else std::erase(v, choice);
                    did = core::doc::set_list_styles(m_journal, m_doc, id, v) || did;
                }
        } else if (key == "list_entry") {   // s090: "level:style"
            const auto colon = choice.find(':');
            if (colon != std::string::npos)
                for (const auto& id : m_inspected)
                    did = core::doc::set_list_entry_style(m_journal, m_doc, id, std::stoi(choice.substr(0, colon)), choice.substr(colon + 1)) || did;
        } else if (key == "list_cite_order") {   // s142
            for (const auto& id : m_inspected) did = core::doc::set_list_cite_order(m_journal, m_doc, id, choice == "1") || did;
        } else if (key == "list_numbers") {   // s090
            for (const auto& id : m_inspected) did = core::doc::set_list_numbers(m_journal, m_doc, id, choice == "1") || did;
        } else if (key == "genlist") {   // s087: Build / Rebuild
            if (choice == "build")
                for (const auto& id : m_inspected) did = layout::build_list(m_journal, m_doc, id) || did;
        } else if (key == "based_on") {   // s037
            if (m_inspected.size() == 1) did = core::doc::set_style_base(m_journal, m_doc, m_inspected.front(), choice);
        } else if (key == "style_reset" && choice.rfind("para.", 0) == 0) {   // s093: a paragraph's own setting off
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            const auto [a, b] = cv.edit_range();
            did = core::doc::clear_para_local(m_journal, m_doc, cv.edit_story_id(), a, b, {choice.substr(5)});
            cv.grab_focus();
        } else if (key == "pkeep_next" || key == "pkeep_lines" || key == "phyphenate") {   // s094
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            const auto [a, b] = cv.edit_range();
            const int v = std::stoi(choice);
            did = core::doc::set_para_local(m_journal, m_doc, cv.edit_story_id(), a, b,
                                            key == "pkeep_next" ? "Keep with next" : key == "pkeep_lines" ? "Keep lines" : "Hyphenate",
                                            [&](core::ParaLocal& o) {
                                                if (key == "pkeep_next") o.keep_next = v != 0;
                                                else if (key == "phyphenate") o.hyphenate = v != 0;
                                                else o.keep_lines = v;
                                            });
            cv.grab_focus();
        } else if (key == "palign" || key == "plindent" || key == "prindent" || key == "pfindent" || key == "pbefore" ||
                   key == "pafter") {   // s093: the paragraph's own settings
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            const auto [a, b] = cv.edit_range();
            static const std::map<std::string, std::string> labels{{"palign", "Align"}, {"plindent", "Left indent"},
                {"prindent", "Right indent"}, {"pfindent", "First indent"}, {"pbefore", "Space before"}, {"pafter", "Space after"}};
            const double v = std::stod(choice);
            did = core::doc::set_para_local(m_journal, m_doc, cv.edit_story_id(), a, b, labels.at(key), [&](core::ParaLocal& o) {
                if (key == "palign") o.align = static_cast<core::Align>(std::clamp(static_cast<int>(v), 0, 3));
                else if (key == "plindent") o.left_indent = v;
                else if (key == "prindent") o.right_indent = v;
                else if (key == "pfindent") o.first_indent = v;
                else if (key == "pbefore") o.space_before = v;
                else o.space_after = v;
            });
            cv.grab_focus();
        } else if (key == "style_reset") {   // s092: one setting back to the base's
            did = core::doc::reset_style_fields(m_journal, m_doc, m_inspected, {choice});
        } else if (key == "style_base") {   // s037: Reset to base
            did = core::doc::reset_to_base(m_journal, m_doc, m_inspected);
        } else if (key == "list_from") {   // s047: the paragraph(s) being typed in
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            const auto [a, b] = cv.edit_range();
            did = core::doc::set_list_restart(m_journal, m_doc, cv.edit_story_id(), a, b, std::stoi(choice));
            cv.grab_focus();
        } else if (key == "mname") {   // s148: a master's own settings
            did = core::doc::edit_masters(m_journal, m_doc, m_inspected, "Master name", [&](core::Master& m) { if (!choice.empty()) m.name = choice; });
        } else if (key == "msize") {   // "0:<points>" (W) or "1:<points>" (H)
            const auto colon = choice.find(':');
            const int k = std::stoi(choice.substr(0, colon));
            const double v = std::stod(choice.substr(colon + 1));
            for (const auto& id : m_inspected)
                if (const auto* m = m_doc.as<core::Master>(id))
                    did = core::doc::set_master_size(m_journal, m_doc, id, k == 0 ? v : m->w, k == 1 ? v : m->h) || did;
        } else if (key == "mbleed") {
            for (const auto& id : m_inspected) did = core::doc::set_master_bleed(m_journal, m_doc, id, std::stod(choice)) || did;
        } else if (key == "mfacing") {
            for (const auto& id : m_inspected) did = core::doc::set_master_facing(m_journal, m_doc, id, choice == "1") || did;
        } else if (key == "mcolumns" || key == "mgutter" || key == "mtop" || key == "mbottom" ||
                   key == "minside" || key == "moutside" || key == "mgrid") {   // s014: a master's margins / columns
            static const std::map<std::string, std::string> labels{
                {"mgrid", "Baseline grid"},
                {"mcolumns", "Page columns"}, {"mgutter", "Page gutter"}, {"mtop", "Top margin"},
                {"mbottom", "Bottom margin"}, {"minside", "Inside margin"}, {"moutside", "Outside margin"}};
            const double v = std::stod(choice);
            did = core::doc::edit_masters(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Master& m) {
                if (key == "mcolumns") m.columns = static_cast<int>(v);
                else if (key == "mgutter") m.gutter = v;
                else if (key == "mgrid") m.grid = v;
                else if (key == "mtop") m.margins.top = v;
                else if (key == "mbottom") m.margins.bottom = v;
                else if (key == "minside") m.margins.inside = v;
                else m.margins.outside = v;
            });
        } else if (key == "pover") {   // s072: every master item on the pages -- overridden / back
            if (choice == "all") did = !core::doc::override_all(m_journal, m_doc, m_inspected).empty();
            else did = core::doc::remove_all_overrides(m_journal, m_doc, m_inspected);
        } else if (key == "page") {   // s014: Add page after / Delete / Start section
            did = page_action(choice, m_inspected);
        } else if (key == "fill" || key == "stroke" || key == "stroke_w") {   // s016: frame fill / stroke
            static const std::map<std::string, std::string> labels{{"fill", "Fill"}, {"stroke", "Stroke"}, {"stroke_w", "Stroke weight"}};
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Frame& f) {
                if (key == "fill") f.fill = choice;
                else if (key == "stroke") f.stroke = choice;
                else f.stroke_w = std::stod(choice);
            });
        } else if (key == "side") {   // s025: which pages a master item shows on
            did = core::doc::set_shows_on(m_journal, m_doc, m_inspected, std::stoi(choice));   // s155: keeps each in place
        } else if (key == "override") {   // s025: back to the master's
            did = core::doc::remove_overrides(m_journal, m_doc, m_inspected);
        } else if (key == "autosize") {   // s048 (the journal's settle fits the height in the same step)
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, "Auto-size", [&](core::Frame& f) {
                if (f.kind == core::FrameKind::Text) f.autosize = choice == "1";
            });
        } else if (key == "valign") {   // s042
            const auto v = static_cast<core::VAlign>(std::clamp(std::stoi(choice), 0, 3));
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, "Vertical justification", [&](core::Frame& f) {
                if (f.kind == core::FrameKind::Text) f.valign = v;
            });
        } else if (key == "col_rule" || key == "col_rule_color" || key == "col_rule_tint" || key == "col_rule_inset") {   // s055
            static const std::map<std::string, std::string> labels{{"col_rule", "Column rule"}, {"col_rule_color", "Column rule colour"},
                                                                   {"col_rule_tint", "Column rule tint"}, {"col_rule_inset", "Column rule inset"}};
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, labels.at(key), [&](core::Frame& f) {
                if (f.kind != core::FrameKind::Text) return;
                if (key == "col_rule") f.col_rule.weight = std::stod(choice);
                else if (key == "col_rule_color") f.col_rule.color = choice;
                else if (key == "col_rule_tint") f.col_rule.tint = std::stod(choice) / 100;
                else f.col_rule.inset = std::stod(choice);
            });
        } else if (key == "inset" || key == "wrap_off") {   // s040 / s018; s067: per side
            // "<points>" (a preset) or "a:<points>" -- all four; "k:<points>" -- side k (T R B L).
            const auto colon = choice.find(':');
            int side = -1;
            if (colon != std::string::npos && choice.compare(0, colon, "a") != 0) side = std::stoi(choice.substr(0, colon));
            const double v = std::stod(colon == std::string::npos ? choice : choice.substr(colon + 1));
            did = core::doc::set_side(m_journal, m_doc, m_inspected,
                                      key == "inset" ? core::doc::SideOf::Inset : core::doc::SideOf::Wrap, side, v);
        } else if (key == "wrap") {   // s018: text wrap
            did = core::doc::edit_frames(m_journal, m_doc, m_inspected, "Text wrap", [&](core::Frame& f) {
                const auto w = static_cast<core::Wrap>(std::stoi(choice));
                // Turned on with no offset: a little air (9 pt) to start.
                if (f.wrap == core::Wrap::None && w != core::Wrap::None && f.wrap_off == core::Sides{}) f.wrap_off = {9, 9, 9, 9};
                f.wrap = w;
            });
        } else if (key == "colw" && !choice.empty() && choice[0] != '=') {   // s067: "k:<points>" -- column k's width
            const auto colon = choice.find(':');
            did = core::doc::set_column_width(m_journal, m_doc, m_inspected, std::stoi(choice.substr(0, colon)),
                                              std::stod(choice.substr(colon + 1)));
        } else if (key == "columns" || key == "gutter" || key == "widths" || key == "balance" || key == "colw") {   // s012
            static const std::map<std::string, std::string> labels{
                {"columns", "Columns"}, {"gutter", "Gutter"}, {"widths", "Column widths"}, {"colw", "Column widths"},
                {"balance", "Balance columns"}};
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
                } else {   // "2:1:1" -> weights; "" -> equal (s067: "=2:1" / "=" from the Widths row's presets)
                    std::vector<double> w;
                    std::size_t at = (!choice.empty() && choice[0] == '=') ? 1 : 0;
                    while (!choice.empty() && at < choice.size()) {
                        const auto c = choice.find(':', at);
                        w.push_back(std::stod(choice.substr(at, c == std::string::npos ? std::string::npos : c - at)));
                        if (c == std::string::npos) break;
                        at = c + 1;
                    }
                    f.widths = w;
                }
            });
        } else if (key == "pic_off") {   // s076: "0:<points>" (X) / "1:<points>" (Y)
            const auto colon = choice.find(':');
            did = core::doc::set_picture_offset(m_journal, m_doc, m_inspected, std::stoi(choice.substr(0, colon)),
                                                std::stod(choice.substr(colon + 1)));
        } else if (key == "pic_scale") {   // s035
            did = core::doc::set_picture_scale(m_journal, m_doc, m_inspected, std::stod(choice));
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
    // s061: typing, the caret goes where the text came back (or went).
    auto& cv = m_canvas_view.canvas();
    std::optional<core::Story> before;
    if (cv.editing()) if (const auto* st = m_doc.as<core::Story>(cv.edit_story_id())) before = *st;
    const auto r = m_journal.undo();
    if (!r.done) return;
    if (before && cv.editing())
        if (const auto* st = m_doc.as<core::Story>(cv.edit_story_id()); st && !(*st == *before))
            cv.place_caret(core::changed_end(*before, *st));
    if (all_frames(m_doc, r.selection)) m_canvas_view.canvas().set_selection(r.selection);
    if (auto lg = log::get(log::Area::Shell)) lg->info("undo: {}", r.label);
}

void Shell::on_redo() {  // handler: put the undone step back
    auto& cv = m_canvas_view.canvas();
    std::optional<core::Story> before;   // s061: as undo
    if (cv.editing()) if (const auto* st = m_doc.as<core::Story>(cv.edit_story_id())) before = *st;
    const auto r = m_journal.redo();
    if (!r.done) return;
    if (before && cv.editing())
        if (const auto* st = m_doc.as<core::Story>(cv.edit_story_id()); st && !(*st == *before))
            cv.place_caret(core::changed_end(*before, *st));
    if (all_frames(m_doc, r.selection)) m_canvas_view.canvas().set_selection(r.selection);
    if (auto lg = log::get(log::Area::Shell)) lg->info("redo: {}", r.label);
}

// s162 (Scott): a style's waiting changes, Set -- on the styles they were
// made for, each through the usual handler, all of it one step.
void Shell::on_inspector_apply(const std::vector<std::string>& ids,
                               const std::vector<std::pair<std::string, std::string>>& edits) {  // handler
    if (ids.empty() || edits.empty()) return;
    const auto keep = m_inspected;
    m_inspected = ids;
    const bool did = core::doc::styles_as_one(m_journal, m_doc, edits.size() == 1 ? std::string("Style setting") : "Style settings", [&] {
        for (const auto& [k, v] : edits) on_inspector_edit(k, v);
    });
    m_inspected = keep;
    if (auto lg = log::get(log::Area::Shell)) lg->info("set {} style change(s) on {} style(s): {}", edits.size(), ids.size(), did ? "step" : "no change");
}

}  // namespace composix
