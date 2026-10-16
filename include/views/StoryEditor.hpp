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
#include "core/Doc.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/textview.h>

#include <sigc++/signal.h>

#include <string>

namespace composix::views {

class StoryEditor : public widgets::Box {
public:
    StoryEditor(core::Doc& d, core::DocJournal& j);

    void show_story(const std::string& story);   // "" = none
    const std::string& story() const { return m_story; }
    // The document changed: reload unless it is our own edit; re-mark overset.
    void model_changed();
    void focus_text() { m_view.grab_focus(); }

    sigc::signal<void()>& signal_edited() { return m_edited; }

private:
    void load();
    void mark_overset();
    void draw_gutter(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);

    core::Doc&           m_doc;
    core::DocJournal&    m_journal;
    widgets::Box         m_head;
    widgets::Label       m_title;
    widgets::Label       m_info;
    widgets::ScrolledWindow m_scroll;
    Gtk::TextView        m_view;
    widgets::DrawingArea m_gutter;
    std::string          m_story;
    bool                 m_applying = false;   // our own edit: the reload would undo the cursor
    sigc::signal<void()> m_edited;
};

}  // namespace composix::views
