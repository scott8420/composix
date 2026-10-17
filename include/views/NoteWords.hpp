#pragma once
// s116: a footnote's words, edited -- a small multi-line text box (a note
// may run to several paragraphs). Enter: a new paragraph; Ctrl+Enter or Esc:
// done (signal_done). Used by the canvas's note popover and the Story
// Editor's.

#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textview.h>
#include <gtkmm/eventcontrollerkey.h>
#include <glibmm/main.h>

#include <string>

namespace composix::views {

class NoteWords : public Gtk::ScrolledWindow {
public:
    NoteWords() {
        set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        set_min_content_height(72);
        set_max_content_height(260);
        set_propagate_natural_height(true);
        set_size_request(420, -1);
        add_css_class("frame");
        m_view.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        m_view.set_accepts_tab(false);
        m_view.set_left_margin(6);
        m_view.set_right_margin(6);
        m_view.set_top_margin(4);
        m_view.set_bottom_margin(4);
        m_view.set_name("note.words");
        set_child(m_view);
        auto keys = Gtk::EventControllerKey::create();
        keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType st) {
            const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
            if (kv == GDK_KEY_Escape || ((kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) && ctrl)) {
                m_done.emit();
                return true;
            }
            return false;
        }, false);
        m_view.add_controller(keys);
    }

    void set_text(const std::string& t) { m_view.get_buffer()->set_text(t); }
    std::string get_text() const { return m_view.get_buffer()->get_text(true); }
    // Characters a .. b selected (the keyboard not taken).
    void select(int a, int b) {
        auto buf = m_view.get_buffer();
        buf->select_range(buf->get_iter_at_offset(a), buf->get_iter_at_offset(b));
        auto mark = buf->create_mark(buf->get_iter_at_offset(a));
        Glib::signal_idle().connect_once([this, mark] { m_view.scroll_to(mark); });
    }
    // The keyboard here, the cursor after the words.
    void focus_end() {
        m_view.grab_focus();
        auto buf = m_view.get_buffer();
        buf->place_cursor(buf->end());
    }
    Gtk::TextView& view() { return m_view; }
    sigc::signal<void()>& signal_done() { return m_done; }

private:
    Gtk::TextView m_view;
    sigc::signal<void()> m_done;
};

}  // namespace composix::views
