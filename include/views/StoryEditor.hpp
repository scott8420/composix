#pragma once
// views::StoryEditor -- a story as plain running text, for writing (s033).
//
// InDesign's story editor: the words without the page. A TextView holds the
// story, one buffer line per paragraph; a gutter on the left names each
// paragraph's style; what no frame holds (overset) is tinted, under a rule.
// Every edit is core::doc::edit_text at the same TextPos ({line, byte in
// line}) -- one store, so the page follows as you type and undo is the
// document's. A change made elsewhere (the canvas, undo) reloads the text,
// keeping the cursor where it was.
#include "views/BackButton.hpp"
#include "core/Doc.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/textview.h>
#include <gtkmm/paned.h>
#include <gtkmm/togglebutton.h>

#include <sigc++/signal.h>

#include <string>

namespace composix::layout { class FlowCache; }

namespace composix::views {

class StoryEditor : public widgets::Box {
public:
    // s068: where back / forward before the title go ("Stories").
    void set_nav(const std::string& back_to, const std::string& fwd_to) { m_nav_btns.set(back_to, fwd_to); }
    // s119: the window's flows (the canvas's): the overset mark and the
    // notes' numbers as laid out read them, not a flow of their own.
    void share_flows(layout::FlowCache& f) { m_flowp = &f; }
    StoryEditor(core::Doc& d, core::DocJournal& j);
    ~StoryEditor() override;

    void show_story(const std::string& story);   // "" = none
    const std::string& story() const { return m_story; }
    // The document changed: reload unless it is our own edit; re-mark overset.
    void model_changed();
    void focus_text() { m_view.grab_focus(); }
    // s049: is the text being typed in? Ctrl+B / Ctrl+I (and Edit's Bold /
    // Italic) then act here: the selection, or the word at the cursor.
    bool text_has_focus() const { return m_view.has_focus(); }
    bool toggle_look(bool bold);

    sigc::signal<void()>& signal_edited() { return m_edited; }

private:
    layout::FlowCache* m_flowp = nullptr;   // s119
    int note_shown(const core::Story& st, int p, int k);   // s119: its number as laid out (by page / section), else in the story
    void load();
    void mark_overset();
    void show_looks();   // s049: bold / italic / character styles as tags
    // s050: styles from here -- the gutter's paragraph-style menu, the
    // header's character-style menu.
    void pick_para_style(int para, double y);
    void fill_char_menu();
    void apply_char_style(const std::string& id);
    void apply_para_style(int para, const std::string& id);
    void draw_gutter(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    // s112: footnotes as chips in the text -- each mark an anchor holding its number.
    void place_note_chips();
    void edit_note(int para, int k, Gtk::Widget& chip);
    // s127: the notes pane -- every footnote's words beside the text.
    void fill_notes_pane();
    void sync_notes_pane();          // texts from the document (structure the same), else refill
    bool focus_pane_note(int para, int k);   // the note's box, scrolled to and focused; false if the pane is shut
    void show_chip(int para, int k);         // the text scrolled to a note's chip

    core::Doc&           m_doc;
    core::DocJournal&    m_journal;
    widgets::Box         m_head;
    NavButtons         m_nav_btns;   // s068
    widgets::Label       m_title;
    widgets::Label       m_info;
    widgets::ScrolledWindow m_scroll;
    Gtk::TextView        m_view;
    widgets::DrawingArea m_gutter;
    Gtk::Popover         m_pick;        // s050: on the gutter (unparented in the destructor)
    Gtk::MenuButton      m_char_btn;    // s050: in the header
    Gtk::Popover         m_char_pop;
    Gtk::Popover         m_note_pop;    // s112: a footnote's words (unparented in the destructor)
    sigc::connection     m_note_conn;
    std::string          m_story;
    bool                 m_applying = false;   // our own edit: the reload would undo the cursor
    sigc::signal<void()> m_edited;
    // s127
    Gtk::Paned           m_split{Gtk::Orientation::HORIZONTAL};
    bool                 m_split_set = false;   // the divider placed once
    Gtk::ToggleButton    m_notes_btn;
    widgets::ScrolledWindow m_notes_scroll{"story.notes.scroll"};
    Gtk::Box             m_notes_box{Gtk::Orientation::VERTICAL, 10};
    struct NoteRow { int p = 0, k = 0; Gtk::Widget* row = nullptr; class NoteWords* words = nullptr; Gtk::Button* head = nullptr; };
    std::vector<NoteRow> m_note_rows;
};

}  // namespace composix::views
