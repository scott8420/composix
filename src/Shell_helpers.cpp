#include "Shell.hpp"
#include "views/Images.hpp"
#include "core/Recents.hpp"
#include "layout/Flow.hpp"
#include "Log.hpp"

#include <gtkmm/alertdialog.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <giomm/liststore.h>
#include <gtkmm/label.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <algorithm>
#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, and handlers: the recents seam (path resolution, persist, and the
// two-surface repaint).

namespace composix {

std::string Shell::recents_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md).
    return (std::filesystem::path(Glib::get_user_data_dir()) / "composix" / "recent.json")
        .string();
}

void Shell::note_recent(const std::string& path) {  // helper: push + persist + repaint
    if (path.empty()) return;
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();

    // Display name = folder basename; identical basenames get their parent dir
    // appended so they're distinguishable (Curvz's disambiguation).
    std::vector<std::string> names(m_recents.size());
    for (std::size_t i = 0; i < m_recents.size(); ++i)
        names[i] = std::filesystem::path(m_recents[i]).filename().string();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j]) {
                const std::string pi =
                    std::filesystem::path(m_recents[i]).parent_path().filename().string();
                const std::string pj =
                    std::filesystem::path(m_recents[j]).parent_path().filename().string();
                if (!pi.empty()) names[i] += "  (" + pi + ")";
                if (!pj.empty()) names[j] += "  (" + pj + ")";
            }

    for (std::size_t i = 0; i < m_recents.size(); ++i) {
        auto item = Gio::MenuItem::create(names[i], "");
        item->set_action_and_target(
            "recents.open", Glib::Variant<Glib::ustring>::create(m_recents[i]));
        m_recents_menu->append_item(item);
    }

    auto clear_section = Gio::Menu::create();
    clear_section->append("Clear recent", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// s004 -- the document <-> the views.
// ─────────────────────────────────────────────────────────────────────────────

// One re-projection per idle, AFTER the writes that caused it: a verb writes
// several records, and rebuilding the views per write would tear down a row
// or a dropdown mid-click (jot s045: "open UI after the rebuild you caused").
// The projection, with each story's overset measured by the layout library
// (core can't measure text; it is told).
core::Outline Shell::project_now() const {  // helper
    std::vector<std::string> over;
    for (const auto& sid : m_doc.ids_of<core::Story>())
        if (layout::flow_story(m_doc, sid).overset) over.push_back(sid);
    return core::project(m_doc, m_project_dir, &over, &layout::font_families());
}

void Shell::queue_refresh() {  // helper
    if (m_refresh_queued) return;
    m_refresh_queued = true;
    Glib::signal_idle().connect_once([this] {
        m_refresh_queued = false;
        refresh();
    });
}

// Re-project and repaint every view where it was: the navigator keeps what is
// disclosed and selected, the tiles keep their selection, an opened item stays
// open (unless it is gone), the inspector re-reads the same ids.
void Shell::refresh() {  // helper
    // Mid-drag the canvas paints the gesture itself; the rest of the views
    // catch up when it ends (signal_edited -> here again).
    if (m_journal.in_gesture()) { m_canvas_view.canvas().queue_draw(); return; }
    const auto keep = m_pick.segment ? m_tiles.selected() : std::vector<std::string>{};
    m_outline = project_now();
    m_nav.refresh(&m_outline);
    std::vector<std::string> still;
    for (const auto& id : m_pick.items)
        if (m_outline.find(id)) still.push_back(id);
    m_pick.items = still;
    show_pick(m_pick, keep);
    update_title();
}

void Shell::show_pick(const views::Pick& p, const std::vector<std::string>& keep_tiles) {  // helper
    if (p.segment) {
        m_segment = p.segment;
        m_tiles.show_section(m_outline, *p.segment, keep_tiles);
        m_main.set_visible_child("tiles");
        const auto kept = m_tiles.selected();
        m_inspected = kept;
        if (kept.empty()) m_inspector.show_section(m_outline, *p.segment);
        else              m_inspector.show_items(resolve(kept));
        return;
    }
    m_segment.reset();
    if (!p.items.empty()) {
        const auto items = resolve(p.items);
        // Pages and masters open on the canvas; anything else in the preview.
        const bool pages = std::all_of(p.items.begin(), p.items.end(), [this](const std::string& id) {
            return m_doc.as<core::Page>(id) || m_doc.as<core::Master>(id);
        });
        if (pages) {
            auto& cv = m_canvas_view.canvas();
            cv.show_pages(p.items);
            cv.model_changed();
            m_canvas_view.set_title(core::selection_title(items), items.size());
            m_main.set_visible_child("canvas");
            if (!cv.selection().empty()) { on_canvas_selection(); return; }
            m_inspected = p.items;
            m_inspector.show_items(items);
            return;
        }
        m_inspected = p.items;
        m_item.show_items(items);
        m_main.set_visible_child("item");
        m_inspector.show_items(items);
        return;
    }
    m_inspected.clear();
    m_main.set_visible_child("welcome");
    m_inspector.clear();
}

bool Shell::open_project(const std::string& dir) {  // helper
    core::Doc fresh;
    std::string err;
    if (!core::load_project(fresh, dir, &err)) {
        alert("Can\u2019t open that project", err);
        if (auto lg = log::get(log::Area::Io)) lg->warn("open '{}': {}", dir, err);
        return false;
    }
    // Swap the records in under a load: not a user choice, not undoable.
    {
        m_doc.reset();
        core::Doc::Loading load(m_doc);
        for (const auto& [id, r] : fresh.records()) m_doc.put(id, r);
    }
    m_project_dir = dir;
    m_canvas_view.canvas().set_project_dir(dir);
    m_journal.clear();       // no step carries over from another document
    set_dirty(false);
    note_recent(dir);
    m_pick = {};
    m_nav.set_outline(nullptr);
    m_outline = project_now();
    m_nav.set_outline(&m_outline);
    m_nav.set_open(core::Segment::Pages, true);
    m_nav.select_section(core::Segment::Pages);
    update_title();
    if (auto lg = log::get(log::Area::Io)) lg->info("opened '{}' ({} records)", dir, m_doc.size());
    return true;
}

bool Shell::save_to(const std::string& dir) {  // helper
    std::string err;
    // From the old folder: Save As carries the embedded files across (s009).
    if (!core::save_project(m_doc, dir, &err, m_project_dir)) {
        alert("Can\u2019t save the project", err);
        if (auto lg = log::get(log::Area::Io)) lg->warn("save '{}': {}", dir, err);
        return false;
    }
    m_project_dir = dir;
    m_canvas_view.canvas().set_project_dir(dir);
    set_dirty(false);
    note_recent(dir);
    queue_refresh();   // linked-asset paths may resolve now that there is a folder
    if (auto lg = log::get(log::Area::Io)) lg->info("saved '{}'", dir);
    return true;
}

void Shell::set_dirty(bool d) {  // helper
    if (m_dirty == d) return;
    m_dirty = d;
    update_title();
}

// Title: the document's name (a dot while unsaved). Subtitle: its trim and
// length, and where it lives.
void Shell::update_title() {  // helper
    const auto& s = m_doc.settings();
    m_title.set_text(s.title + (m_dirty ? "  \u2022" : ""));
    const auto n = m_doc.pages().size();
    std::string where = m_project_dir.empty() ? "sample \u2014 not saved" : m_project_dir;
    const std::string home = Glib::get_home_dir();
    if (!home.empty() && where.rfind(home, 0) == 0) where = "~" + where.substr(home.size());
    m_subtitle.set_text(core::size_text(s.trim_w, s.trim_h, 0) + " \u00b7 " + std::to_string(n) +
                        (n == 1 ? " page" : " pages") + " \u00b7 " + where);
    set_title(s.title + (m_dirty ? " \u2022" : "") + " \u2014 Composix");
}

void Shell::alert(const std::string& msg, const std::string& detail) {  // helper
    auto dialog = Gtk::AlertDialog::create(msg);
    dialog->set_detail(detail);
    dialog->show(*this);
}

std::vector<const core::Item*> Shell::resolve(const std::vector<std::string>& ids) const {  // helper
    std::vector<const core::Item*> out;
    for (const auto& id : ids)
        if (const auto* it = m_outline.find(id)) out.push_back(it);
    return out;
}

// Undo / Redo say what they would do, and grey when there is nothing: the
// actions (so buttons and menu items follow), the button tooltips, and the
// menu's labels -- "Undo Delete “Jade”" names the step before you take it.
void Shell::update_undo_ui() {  // helper
    if (m_act_undo) m_act_undo->set_enabled(m_journal.can_undo());
    if (m_act_redo) m_act_redo->set_enabled(m_journal.can_redo());
    const std::string u = m_journal.undo_label(), r = m_journal.redo_label();
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(u.empty() ? "Undo" : "Undo " + u, "win.undo");
        m_edit_menu->append(r.empty() ? "Redo" : "Redo " + r, "win.redo");
    }
}

void Shell::sync_nav() {  // helper: the header toggle + View check follow the navigator
    if (!m_nav_handle) return;
    if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action("toggle-nav")))
        a->change_state(m_nav_handle->open());
}

// The header toggle, the View menu's check and the pull tab all follow the
// panel -- whichever of them (or the handle, or a drag) moved it.
void Shell::sync_inspector() {  // helper
    if (!m_insp_handle) return;
    const bool open = m_insp_handle->open();
    if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action("toggle-inspector")))
        a->change_state(open);
    m_insp_tab_glyph.set_text(open ? "\u203A" : "\u2039");   // > push back  /  < pull out
    m_insp_tab.set_tooltip_text(open ? "Drag to size the inspector; click to push it back  (Ctrl+Alt+2)"
                                     : "Click or drag to pull out the inspector  (Ctrl+Alt+2)");
    if (open) m_insp_tab.add_css_class("cx-open");
    else      m_insp_tab.remove_css_class("cx-open");
}

// Is the keyboard in something that types? Then Ctrl+Z is that box's own,
// whether or not it had anything to undo.
bool Shell::focus_is_text() const {  // helper
    for (const Gtk::Widget* w = get_focus(); w; w = w->get_parent())
        if (dynamic_cast<const Gtk::TextView*>(w) || dynamic_cast<const Gtk::Editable*>(w)) return true;
    return false;
}

}  // namespace composix

namespace composix {

// ── images (s009) ──────────────────────────────────────────────────────────
void Shell::pick_image(std::function<void(const core::ImageFile&)> then) {  // helper
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Place an image");
    auto filter = Gtk::FileFilter::create();
    filter->set_name("Images");
    for (const auto& p : views::images::patterns()) filter->add_pattern(p);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    dialog->set_filters(filters);
    dialog->set_default_filter(filter);
    dialog->open(*this, [this, dialog, then](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = dialog->open_finish(result);
            if (!file || file->get_path().empty()) return;
            const auto f = views::images::probe(file->get_path());
            if (!f) {
                alert("Can’t read that image", "“" + file->get_basename() + "” isn’t a picture "
                      "Composix can open (PNG, JPEG, TIFF, GIF, BMP, WebP, SVG).");
                return;
            }
            if (auto lg = log::get(log::Area::Io))
                lg->info("place: '{}' {}x{}{}", f->path, f->px_w, f->px_h, f->vector ? " (vector)" : "");
            then(*f);
        } catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Io)) lg->info("place: dismissed ({})", e.what());
        }
    });
}

// Copy an asset's file into <project>/assets/ under a name not taken there;
// the project-relative path, or "" (and why).
std::string Shell::embed_copy(const core::Asset& a, std::string* err) {  // helper
    namespace fs = std::filesystem;
    const std::string src = core::resolve_asset(a, m_project_dir);
    std::error_code ec;
    if (src.empty() || !fs::exists(src, ec)) { if (err) *err = "Its file is missing — relink it first."; return {}; }
    const fs::path dir = fs::path(m_project_dir) / "assets";
    fs::create_directories(dir, ec);
    const fs::path name = fs::path(src).filename();
    fs::path dst = dir / name;
    for (int n = 2; fs::exists(dst, ec); ++n)
        dst = dir / (name.stem().string() + "-" + std::to_string(n) + name.extension().string());
    fs::copy_file(src, dst, ec);
    if (ec) { if (err) *err = ec.message(); return {}; }
    return (fs::path("assets") / dst.filename()).string();
}

}  // namespace composix

#include "layout/Export.hpp"

namespace composix {

// Write the PDF, then say what was written and what wants a look (s013).
void Shell::export_pdf_to(const std::string& path) {  // helper
    layout::PdfOptions opt;
    opt.bleed = m_pdf_bleed;
    opt.marks = m_pdf_marks;
    const std::string dir = m_project_dir;
    const layout::ImageSource images = [dir](const core::Asset& a, double w, double h) {
        return views::images::print_surface(core::resolve_asset(a, dir), w, h);
    };
    layout::PdfReport rep;
    std::string err;
    if (!layout::export_pdf(m_doc, path, opt, images, &rep, &err)) {
        alert("Can’t export the PDF", err);
        if (auto lg = log::get(log::Area::Io)) lg->warn("export '{}': {}", path, err);
        return;
    }
    if (auto lg = log::get(log::Area::Io))
        lg->info("exported '{}': {} page(s), {} warning(s)", path, rep.pages, rep.warnings.size());
    std::string detail = std::filesystem::path(path).filename().string();
    if (rep.warnings.empty()) detail += "\n\nNothing to check.";
    else {
        detail += "\n\nWorth a look:";
        for (const auto& w : rep.warnings) detail += "\n• " + w;
    }
    alert("Exported " + std::to_string(rep.pages) + (rep.pages == 1 ? " page" : " pages"), detail);
}

}  // namespace composix
