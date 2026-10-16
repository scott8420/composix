#include "Shell.hpp"
#include "Log.hpp"
#include "core/Recents.hpp"
#include "layout/Flow.hpp"
#include <tuple>

#include <giomm/simpleactiongroup.h>
#include <gtkmm/eventcontrollerkey.h>
#include <glibmm/variant.h>

#include <filesystem>
#include <functional>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions, the
// parameterised recents action group, and (where a keystroke must survive focus)
// accelerators. No slot bodies here -- those are handlers; this file only wires.

namespace composix {

void Shell::bind_actions() {  // bindings: actions + recents group
    // Plain win.* actions -> handler bodies (Shell_handlers.cpp).
    add_action("new",           sigc::mem_fun(*this, &Shell::on_new));      // s019
    add_action("find",          sigc::mem_fun(*this, &Shell::on_find));     // s022
    add_action("story-editor",  sigc::mem_fun(*this, &Shell::on_story_editor));   // s033
    // s030: bold / italic on the words being edited (the word at the caret).
    for (const bool bold : {true, false})
        add_action(bold ? "bold" : "italic", [this, bold] {
            // s049: typing in the story editor -- its words.
            if (m_story_ed.text_has_focus()) { m_story_ed.toggle_look(bold); return; }
            auto& cv = m_canvas_view.canvas();
            if (!cv.editing()) return;
            auto [a, b] = cv.edit_range();
            const auto story = cv.edit_story_id();
            const auto* st = m_doc.as<core::Story>(story);
            if (!st) return;
            if (a == b) std::tie(a, b) = core::word_at(*st, a);
            const bool on = !(bold ? core::all_bold(m_doc, *st, a, b) : core::all_italic(m_doc, *st, a, b));
            core::doc::set_local(m_journal, m_doc, story, a, b, bold ? "Bold" : "Italic",
                                 [&](core::Local& l) { (bold ? l.bold : l.italic) = on ? 1 : 0; });
            cv.grab_focus();
        });
    // s023: special characters, at the caret while editing text.
    for (const auto& [name, mark, label] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
             {"insert-page-number", core::kMarkPage, "Insert page number"},
             {"insert-section-name", core::kMarkSection, "Insert section name"},
             {"insert-next-page", core::kMarkNext, "Insert next page number"},
             {"insert-prev-page", core::kMarkPrev, "Insert previous page number"}}) {
        const std::string m = mark, l = label;
        add_action(name, [this, m, l] {
            if (!m_canvas_view.canvas().insert_special(m, l))
                alert("Click into text first", "Special characters go in at the text cursor: double-click a text frame, then insert.");
        });
    }
    // s055: breaks (also keypad Enter while typing: column; Shift: frame; Ctrl: page).
    for (const auto& [name, kind] : std::initializer_list<std::pair<const char*, core::ParaStart>>{
             {"insert-column-break", core::ParaStart::NextColumn},
             {"insert-frame-break", core::ParaStart::NextFrame},
             {"insert-page-break", core::ParaStart::NextPage}}) {
        const auto k = kind;
        add_action(name, [this, k] {
            if (!m_canvas_view.canvas().insert_break(k))
                alert("Click into text first", "A break goes in at the text cursor: double-click a text frame, then insert.");
        });
    }
    add_action("open-sample",   sigc::mem_fun(*this, &Shell::on_sample));   // s019
    add_action("open",          sigc::mem_fun(*this, &Shell::on_open));
    add_action("zoom-in",       [this] { m_canvas_view.canvas().zoom_step(+1); });
    add_action("zoom-out",      [this] { m_canvas_view.canvas().zoom_step(-1); });
    add_action("zoom-fit",      [this] { m_canvas_view.canvas().zoom_fit(); });
    add_action("zoom-100",      [this] { m_canvas_view.canvas().zoom_to(100); });
    // s006: what the canvas shows / does -- stateful, so the menu shows a check.
    const auto toggle = [this](const char* name, std::function<void(bool)> apply, bool initial = true) {
        add_action_bool(name, [this, name, apply] {
            auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action(name));
            bool on = false;
            a->get_state(on);
            a->change_state(!on);
            apply(!on);
        }, initial);
    };
    toggle("toggle-rulers", [this](bool on) { m_canvas_view.set_rulers(on); });
    toggle("toggle-guides", [this](bool on) { m_canvas_view.canvas().set_show_guides(on); });
    toggle("toggle-snap",   [this](bool on) { m_canvas_view.canvas().set_snap(on); });
    toggle("toggle-grid",   [this](bool on) { m_canvas_view.canvas().set_show_grid(on); }, false);   // s036
    add_action("save",          sigc::mem_fun(*this, &Shell::on_save));
    add_action("save-as",       sigc::mem_fun(*this, &Shell::on_save_as));
    add_action("place",         sigc::mem_fun(*this, &Shell::on_place));   // s009
    add_action("export-pdf",    sigc::mem_fun(*this, &Shell::on_export_pdf));   // s013
    add_action("color-report",  sigc::mem_fun(*this, &Shell::on_color_report));   // s053
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("about",         sigc::mem_fun(*this, &Shell::on_about));
    add_action("shortcuts",     sigc::mem_fun(*this, &Shell::on_shortcuts));
    add_action("quit",          [this]() { close(); });

    // Recents: a "recents" action group with a PARAMETERISED open verb --
    // recents.open(<path>) carries each entry's path as its target, so one
    // action serves every menu row (CANON/Curvz: the proven parameterised-action
    // shape). clear is held so rebuild_recents_menu can grey it when empty.
    auto group = Gio::SimpleActionGroup::create();
    group->add_action_with_parameter(
        "open", Glib::Variant<Glib::ustring>::variant_type(),
        [this](const Glib::VariantBase& param) {
            const std::string p =
                Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param).get();
            on_recent_open(p);
        });
    m_recents_clear = Gio::SimpleAction::create("clear");
    m_recents_clear->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_recent_clear(); });
    group->add_action(m_recents_clear);
    insert_action_group("recents", group);

    // Now that the group exists, load the persisted list (prunes stale folders).
    m_recents = core::load_recents(m_recents_file);

    // ── undo ────────────────────────────────────────────────────────────────
    // Undo / Redo are held so update_undo_ui can grey them. The journal's door
    // and selection source stay wired from day one (RULES: undo from the first
    // write); s002 writes nothing, so the journal is empty until s003.
    m_act_undo = add_action("undo", sigc::mem_fun(*this, &Shell::on_undo));
    m_act_redo = add_action("redo", sigc::mem_fun(*this, &Shell::on_redo));
    // The model <-> journal <-> UI wiring (as Cairn's stone): the document asks
    // the journal whether the door is open; the journal reads the selection
    // (what the inspector shows) and tells the UI when Undo / Redo change;
    // every change to the document re-projects, once, after the writes.
    m_doc.set_door([this] { return m_journal.open(); });
    m_doc.set_on_change([this] { queue_refresh(); });
    // s048: every step settles what follows from it -- auto-size frames fit their text.
    m_journal.set_settle([this] { return layout::fit_frames(m_doc); });
    m_journal.set_selection_source([this] { return m_inspected; });
    // s057: unsaved = not where it was saved (undo back there and it's clean).
    m_journal.set_on_change([this] { update_undo_ui(); set_dirty(m_journal.mark() != m_saved_mark); });

    // ── pane toggles: stateful, so the header buttons show the state ────────
    add_action_bool("toggle-nav", [this] {
        if (m_nav_handle) m_nav_handle->set_open(!m_nav_handle->open());   // sync_nav follows
    }, true);
    add_action_bool("toggle-inspector", [this] {
        if (m_insp_handle) m_insp_handle->set_open(!m_insp_handle->open());   // sync_inspector follows
    }, true);

    // ── Ctrl+Z from ANYWHERE but a text box ─────────────────────────────────
    // NOT an accelerator: a set_accels_for_action chord fires before the
    // focused widget, so it would take Ctrl+Z away from every Entry and
    // TextView (jot s016c met that with Ctrl+Delete). Instead the WINDOW
    // hears it in the BUBBLE phase -- only keys nothing else took -- and lets
    // a text box keep its own even when it had nothing to undo. Binding undo
    // to one widget's focus is the other trap: after a menu closes, or the
    // focused row is rebuilt away, the keyboard is nowhere and Ctrl+Z is dead
    // (jot s046a: "undo a delete" did nothing). The registry carries these as
    // doc-only rows for that reason.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) {
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK |
                                       Gdk::ModifierType::SHIFT_MASK |
                                       Gdk::ModifierType::ALT_MASK);
            const auto CTRL  = Gdk::ModifierType::CONTROL_MASK;
            const auto SHIFT = Gdk::ModifierType::SHIFT_MASK;
            const char* act = nullptr;
            if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == CTRL)               act = "win.undo";
            else if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == (CTRL | SHIFT)) act = "win.redo";
            else if ((keyval == GDK_KEY_y || keyval == GDK_KEY_Y) && mods == CTRL)          act = "win.redo";
            // s027: Page Up / Page Down -- the previous / next spread on the canvas.
            if (!act && mods == Gdk::ModifierType{} && (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) &&
                !focus_is_text() && m_main.get_visible_child_name() == "canvas" && !m_canvas_view.canvas().editing()) {
                const auto& now = m_canvas_view.canvas().shown();
                if (now.empty() || !m_doc.as<core::Page>(now.front())) return false;
                const auto all = core::spreads(m_doc);
                for (std::size_t i = 0; i < all.size(); ++i)
                    if (std::find(all[i].begin(), all[i].end(), now.front()) != all[i].end()) {
                        const std::size_t to = keyval == GDK_KEY_Page_Down ? std::min(i + 1, all.size() - 1) : (i ? i - 1 : 0);
                        if (to != i) m_nav.select_items({all[to].front()});
                        return true;
                    }
                return false;
            }
            if (!act || focus_is_text()) return false;
            if (auto lg = log::get(log::Area::Shell)) lg->info("window key -> {}", act);
            activate_action(act);   // the FULL name: "undo" alone would do nothing
            return true;
        }, false);
    add_controller(keys);
    update_undo_ui();
}

}  // namespace composix

namespace composix {

void Shell::bind_views() {  // bindings: navigator / tiles -> handlers
    m_nav.signal_pick().connect(sigc::mem_fun(*this, &Shell::on_nav_pick));
    m_tiles.signal_selection().connect(sigc::mem_fun(*this, &Shell::on_tiles_selection));
    m_tiles.signal_open().connect(sigc::mem_fun(*this, &Shell::on_tiles_open));
    // s052: the Colors view's size, kept between runs.
    {
        const auto it = m_settings.find("colors.view");   // (find: reading adds no empty key)
        const std::string v = it == m_settings.end() ? std::string{} : it->second;
        m_tiles.set_tile_size(v == "small" ? views::TileSize::Small : v == "list" ? views::TileSize::List : views::TileSize::Large);
        m_tiles.signal_tile_size().connect([this](views::TileSize z) {
            m_settings["colors.view"] = z == views::TileSize::Small ? "small" : z == views::TileSize::List ? "list" : "large";
            core::save_settings(m_settings_file, m_settings);
        });
    }
    // s033: the story editor's edits refresh the other views.
    m_story_ed.signal_edited().connect([this] { set_dirty(m_journal.mark() != m_saved_mark); queue_refresh(); });
    // s031: the navigator brackets them too.
    m_nav.set_grouping([this] {
        return m_doc.settings().facing ? core::spreads(m_doc) : std::vector<std::vector<std::string>>{};
    });
    // s028: facing pages tile by spread.
    m_tiles.set_grouping([this] {
        return m_doc.settings().facing ? core::spreads(m_doc) : std::vector<std::vector<std::string>>{};
    });
    m_inspector.signal_edit().connect(sigc::mem_fun(*this, &Shell::on_inspector_edit));

    // s005: the canvas. Its picks drive the inspector; a finished drag or a
    // nudge refreshes the other views (mid-drag only the canvas repaints).
    auto& cv = m_canvas_view.canvas();
    m_tiles.signal_move().connect(sigc::mem_fun(*this, &Shell::on_tiles_move));   // s014
    m_tiles.signal_delete().connect([this](const std::vector<std::string>& ids) { page_action("delete", ids); });
    cv.signal_selection().connect(sigc::mem_fun(*this, &Shell::on_canvas_selection));
    cv.signal_caret().connect(sigc::mem_fun(*this, &Shell::on_canvas_selection));   // s011: the text inspector
    cv.signal_edited().connect([this] { queue_refresh(); });
    // s009: files change behind our back (an image edited in another app):
    // coming back to the window re-reads link status and redraws the pictures.
    property_is_active().signal_changed().connect([this] {
        if (!is_active()) return;
        if (auto lg = log::get(log::Area::Shell)) lg->info("window active again: re-checking linked files");
        queue_refresh();
        m_canvas_view.canvas().queue_draw();
    });
}

}  // namespace composix
