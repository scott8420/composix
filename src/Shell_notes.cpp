#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/eventcontrollerkey.h>

// Shell_notes.cpp -- HELPERS for s110: footnotes in the text.
//   * Insert Footnote (Ctrl+Alt+F, Edit > Insert Special Character): the mark
//     at the caret, then its words typed in a popover there.
//   * Edit Footnote: the caret beside a mark -- the same popover, with the
//     note's words.
// The words go in when the popover closes (Enter, Esc, a click away): one
// step; nothing changed, no step.
//   * s113: Find / Change's hit in a note -- the same popover, the hit's
//     words selected, the keyboard left in the bar (Enter there: the next
//     hit). Click into it to edit.

namespace composix {

void Shell::edit_note(const std::string& story, int p, int k, int sel_a, int sel_b, bool take_focus) {  // helper
    const auto* st = m_doc.as<core::Story>(story);
    if (!st || p < 0 || p >= static_cast<int>(st->paras.size())) return;
    const auto& para = st->paras[static_cast<std::size_t>(p)];
    const std::string words = k >= 0 && k < static_cast<int>(para.notes.size()) ? para.notes[static_cast<std::size_t>(k)].text
                                                                                : std::string{};
    const int number = core::note_number(*st, p, k);
    auto& canvas = m_canvas_view.canvas();
    if (m_note_pop) {
        m_note_pop->popdown();
        m_note_pop->unparent();
        m_note_pop.reset();
    }
    m_note_pop_find = !take_focus;
    m_note_pop = std::make_unique<Gtk::Popover>();
    m_note_pop->set_name("note.popover");
    m_note_pop->set_parent(canvas);
    if (!take_focus) m_note_pop->set_autohide(false);   // s113: the keyboard stays in the Find bar
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto head = Gtk::make_managed<Gtk::Label>("Footnote " + std::to_string(number));
    head->set_xalign(0);
    head->add_css_class("heading");
    auto entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_name("note.words");
    entry->set_text(words);
    entry->set_placeholder_text("The note's words");
    entry->set_width_chars(48);
    auto hint = Gtk::make_managed<Gtk::Label>(take_focus ? "Enter to finish" : "Found in the note · click to edit");
    hint->set_xalign(0);
    hint->add_css_class("dim-label");
    box->append(*head);
    box->append(*entry);
    box->append(*hint);
    m_note_pop->set_child(*box);
    const auto r = canvas.caret_screen();
    if (r.w > 0)
        m_note_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    entry->signal_activate().connect([this] { if (m_note_pop) m_note_pop->popdown(); });
    auto keys = Gtk::EventControllerKey::create();   // s113: Esc closes it (a Find one doesn't hide by itself)
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) {
        if (kv != GDK_KEY_Escape || !m_note_pop) return false;
        m_note_pop->popdown();
        return true;
    }, false);
    entry->add_controller(keys);
    m_note_pop->signal_closed().connect([this, story, p, k, entry, words, take_focus] {
        const std::string t = entry->get_text();
        // s113: only what was typed here goes in -- not the words as they were
        // when it opened over a change made since (Find's Change).
        if (t != words && core::doc::set_note_text(m_journal, m_doc, story, p, k, t))
            if (auto lg = log::get(log::Area::Shell)) lg->info("footnote {} of paragraph {}: {} bytes", k, p, t.size());
        // Back to the text, where it was.
        if (take_focus) Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
    });
    m_note_pop->popup();
    if (sel_a >= 0 && sel_b >= sel_a) {   // bytes -> characters
        const auto chars = [&](int b) {
            return static_cast<int>(g_utf8_strlen(words.c_str(), std::clamp(b, 0, static_cast<int>(words.size()))));
        };
        entry->select_region(chars(sel_a), chars(sel_b));
    }
    if (take_focus) {
        entry->grab_focus();
        if (sel_a < 0) entry->set_position(-1);   // the cursor after the words (not all of them selected)
    }
}

void Shell::close_find_note() {  // helper
    if (!m_note_pop || !m_note_pop_find) return;
    m_note_pop->popdown();
    m_note_pop->unparent();
    m_note_pop.reset();
    m_note_pop_find = false;
}

}  // namespace composix
