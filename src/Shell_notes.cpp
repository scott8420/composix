#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>

// Shell_notes.cpp -- HELPERS for s110: footnotes in the text.
//   * Insert Footnote (Ctrl+Alt+F, Edit > Insert Special Character): the mark
//     at the caret, then its words typed in a popover there.
//   * Edit Footnote: the caret beside a mark -- the same popover, with the
//     note's words.
// The words go in when the popover closes (Enter, Esc, a click away): one
// step; nothing changed, no step.

namespace composix {

void Shell::edit_note(const std::string& story, int p, int k) {  // helper
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
    m_note_pop = std::make_unique<Gtk::Popover>();
    m_note_pop->set_name("note.popover");
    m_note_pop->set_parent(canvas);
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
    auto hint = Gtk::make_managed<Gtk::Label>("Enter to finish");
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
    m_note_pop->signal_closed().connect([this, story, p, k, entry] {
        const std::string t = entry->get_text();
        if (core::doc::set_note_text(m_journal, m_doc, story, p, k, t))
            if (auto lg = log::get(log::Area::Shell)) lg->info("footnote {} of paragraph {}: {} bytes", k, p, t.size());
        // Back to the text, where it was.
        Glib::signal_idle().connect_once([this] { m_canvas_view.canvas().grab_focus(); });
    });
    m_note_pop->popup();
    entry->grab_focus();
    entry->set_position(-1);   // the cursor after the words (not all of them selected)
}

}  // namespace composix
