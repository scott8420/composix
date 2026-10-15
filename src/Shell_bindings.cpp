#include "Shell.hpp"
#include "Log.hpp"
#include "core/Recents.hpp"

#include <giomm/simpleactiongroup.h>
#include <gtkmm/eventcontrollerkey.h>
#include <glibmm/variant.h>

#include <filesystem>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions, the
// parameterised recents action group, and (where a keystroke must survive focus)
// accelerators. No slot bodies here -- those are handlers; this file only wires.

namespace composix {

void Shell::bind_actions() {  // bindings: actions + recents group
    // Plain win.* actions -> handler bodies (Shell_handlers.cpp).
    add_action("open",          sigc::mem_fun(*this, &Shell::on_open));
    add_action("save",          sigc::mem_fun(*this, &Shell::on_save));
    add_action("save-as",       sigc::mem_fun(*this, &Shell::on_save_as));
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
    m_journal.set_selection_source([this] { return m_inspected; });
    m_journal.set_on_change([this] { update_undo_ui(); set_dirty(true); });

    // ── pane toggles: stateful, so the header buttons show the state ────────
    add_action_bool("toggle-nav", [this] {
        const bool show = !m_nav.get_visible();
        m_nav.set_visible(show);
        if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(lookup_action("toggle-nav")))
            a->change_state(show);
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
    m_inspector.signal_edit().connect(sigc::mem_fun(*this, &Shell::on_inspector_edit));
}

}  // namespace composix
