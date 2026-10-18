#include "Shell.hpp"
#include "Log.hpp"
#include "core/Recents.hpp"
#include "layout/Flow.hpp"
#include <tuple>

#include <giomm/simpleactiongroup.h>
#include <gtkmm/gestureclick.h>
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
             {"insert-prev-page", core::kMarkPrev, "Insert previous page number"},
             {"insert-shy", "\u00AD", "Discretionary hyphen"}}) {   // s065
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
    // s110: footnotes -- the mark at the caret, its words in a popover there.
    add_action("insert-footnote", [this] {
        int p = 0, k = 0;
        auto& c = m_canvas_view.canvas();
        if (!c.insert_note(p, k)) {
            alert("Click into text first", "A footnote goes in at the text cursor: double-click a text frame, then insert.");
            return;
        }
        edit_note(c.edit_story_id(), p, k);
    });
    add_action("notes-panel", [this] { open_notes_panel(); });   // s121
    add_action("insert-xref", [this] { on_insert_xref(); });   // s128
    add_action("edit-xref", [this] { on_edit_xref(); });       // s130
    add_action("xrefs-panel", [this] { open_xrefs_panel(); }); // s133
    add_action("edit-footnote", [this] {
        int p = 0, k = 0;
        auto& c = m_canvas_view.canvas();
        if (!c.note_at_caret(p, k)) {
            alert("No footnote here", "Put the text cursor beside a footnote's number in the text, then Edit Footnote.");
            return;
        }
        edit_note(c.edit_story_id(), p, k);
    });
    add_action("open-sample",   sigc::mem_fun(*this, &Shell::on_sample));   // s019
    add_action("open-big-book", sigc::mem_fun(*this, &Shell::on_big_book)); // s100
    add_action("open",          sigc::mem_fun(*this, &Shell::on_open));
    add_action("start",         [this] { show_start(); });   // s064
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
    {   // s059: hidden characters -- kept between runs.
        const bool on = core::setting_int(m_settings, "view.hidden", 0, 0, 1) == 1;
        m_canvas_view.canvas().set_show_hidden(on);
        toggle("toggle-hidden", [this](bool v) {
            m_canvas_view.canvas().set_show_hidden(v);
            m_settings["view.hidden"] = v ? "1" : "0";
            core::save_settings(m_settings_file, m_settings);
        }, on);
    }
    // s099: proof colours -- with the export's press profile (else the first installed) and intent.
    {
        auto a = Gio::SimpleAction::create_bool("toggle-proof", false);
        a->signal_activate().connect([this, a](const Glib::VariantBase&) {
            bool on = false;
            a->get_state(on);
            if (!on && !apply_proof(true)) {
                alert("No press profile", "Proof Colours shows the pages through a CMYK press profile. Install one (your printer's, "
                                          "or GRACoL / FOGRA) in ~/.local/share/icc, or pick one in Export PDF.");
                return;
            }
            if (on) apply_proof(false);
            a->change_state(!on);
        });
        add_action(a);
    }
    {   // s107: the proof simulates the paper (its white, the ink's black) -- kept between runs.
        m_proof_paper = core::setting_int(m_settings, "view.paper", 1, 0, 1) == 1;
        toggle("toggle-paper", [this](bool v) {
            m_proof_paper = v;
            m_settings["view.paper"] = v ? "1" : "0";
            core::save_settings(m_settings_file, m_settings);
            if (m_proofing) apply_proof(true);
        }, m_proof_paper);
    }
    add_action("save",          sigc::mem_fun(*this, &Shell::on_save));
    add_action("save-as",       sigc::mem_fun(*this, &Shell::on_save_as));
    add_action("place",         sigc::mem_fun(*this, &Shell::on_place));   // s009
    // s070: stacking order of the frames picked on the canvas.
    add_action("bring-front",   [this] { on_arrange(core::doc::Arrange::Front); });
    add_action("bring-forward", [this] { on_arrange(core::doc::Arrange::Forward); });
    add_action("send-backward", [this] { on_arrange(core::doc::Arrange::Backward); });
    add_action("send-back",     [this] { on_arrange(core::doc::Arrange::Back); });
    bind_context_actions();   // s074
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
    m_act_up = add_action("go-up", sigc::mem_fun(*this, &Shell::on_go_up));   // s068
    m_act_up->set_enabled(false);
    m_act_fwd = add_action("go-forward", sigc::mem_fun(*this, &Shell::on_go_forward));   // s068
    m_act_fwd->set_enabled(false);
    // s068: the mouse's back / forward (thumb) buttons, anywhere in the window.
    {
        auto thumbs = Gtk::GestureClick::create();
        thumbs->set_button(0);
        thumbs->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        thumbs->signal_pressed().connect([this, g = thumbs.get()](int, double, double) {
            const auto b = g->get_current_button();
            if (b == 8) activate_action("win.go-up");
            else if (b == 9) activate_action("win.go-forward");
        });
        add_controller(thumbs);
    }
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
    m_tiles.signal_new().connect([this] { open_swatch_editor({}); });   // s058
    // s062: a master dropped on pages (tiles, navigator rows, the canvas).
    const auto master_drop = [this](const std::string& master, const std::vector<std::string>& pages) {
        const bool did = core::doc::set_master(m_journal, m_doc, pages, master);
        if (auto lg = log::get(log::Area::Shell)) lg->info("master drop: {} on {} page(s): {}", master, pages.size(), did ? "step" : "no change");
    };
    m_tiles.signal_master_drop().connect(master_drop);
    m_nav.signal_master_drop().connect(master_drop);
    m_canvas_view.canvas().signal_master_drop().connect(master_drop);
    // s061: the caret's text went onto a page not shown -- show it (its spread).
    // Showing the page moves the keyboard to the navigator: give it back to the text.
    m_canvas_view.canvas().signal_follow().connect([this](const std::string& page) {
        m_nav.select_items({page});
        m_canvas_view.canvas().grab_focus();
    });
    // s052: the Colors view's size, kept between runs; s080: every
    // section's, under "<section>.view" ("colors.view" as before).
    {
        for (const auto& si : core::segments()) {
            const auto it = m_settings.find(si.key + ".view");   // (find: reading adds no empty key)
            const std::string v = it == m_settings.end() ? std::string{} : it->second;
            m_tiles.set_tile_size(si.id, v == "small" ? views::TileSize::Small : v == "list" ? views::TileSize::List
                                                                                              : views::TileSize::Large);
        }
        m_tiles.signal_tile_size().connect([this](core::Segment s, views::TileSize z) {
            m_settings[core::segment_info(s).key + ".view"] =
                z == views::TileSize::Small ? "small" : z == views::TileSize::List ? "list" : "large";
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
    cv.signal_context().connect(sigc::mem_fun(*this, &Shell::on_canvas_context));   // s074
    cv.signal_note().connect([this](int p, int k) {   // s114: a note at the foot double-clicked
        Glib::signal_idle().connect_once([this, p, k] { edit_note(m_canvas_view.canvas().edit_story_id(), p, k); });
    });
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
