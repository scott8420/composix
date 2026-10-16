#include "views/StoryEditor.hpp"
#include "layout/Flow.hpp"
#include "Log.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/textbuffer.h>
#include <pangomm/layout.h>

namespace composix::views {

namespace {
constexpr int kGutterW = 120;

// The story as the editor shows it: a line per paragraph, markers (s023)
// as their signs (the same bytes long, so places map 1:1).
std::string shown_text(const core::Story& st) {
    std::string text;
    for (std::size_t i = 0; i < st.paras.size(); ++i) {
        if (i) text += "\n";
        std::string t = st.paras[i].text;
        for (std::size_t k = 0; k + 3 <= t.size(); ++k)
            if (const int m = core::marker_at(t, k); m >= 0) {
                static const char* sign[] = {"\xEF\xBC\x83", "\xE2\xB8\xB9", "\xE3\x80\x8B", "\xE3\x80\x8A"};
                t.replace(k, 3, sign[m]);
                k += 2;
            }
        text += t;
    }
    return text;
}
}  // namespace

StoryEditor::StoryEditor(core::Doc& d, core::DocJournal& j)
    : widgets::Box("story", Gtk::Orientation::VERTICAL),
      m_doc(d),
      m_journal(j),
      m_head("story.head", Gtk::Orientation::HORIZONTAL, 10),
      m_title("story.title"),
      m_info("story.info"),
      m_scroll("story.scroll"),
      m_gutter("story.gutter") {
    add_css_class("cx-story-editor");
    m_head.add_css_class("cx-view-head");
    m_title.add_css_class("cx-view-title");
    m_title.set_xalign(0.0f);
    m_title.set_hexpand(true);
    m_info.add_css_class("cx-count");
    m_info.set_valign(Gtk::Align::CENTER);
    m_head.append(m_title);
    m_head.append(m_info);
    append(m_head);

    m_view.set_name("story.text");
    m_view.add_css_class("cx-story-text");
    m_view.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_view.set_left_margin(14);
    m_view.set_right_margin(24);
    m_view.set_top_margin(12);
    m_view.set_bottom_margin(48);
    m_view.set_pixels_below_lines(6);
    m_view.get_buffer()->set_enable_undo(false);   // the document's undo, not the buffer's
    m_view.set_monospace(false);
    auto buf = m_view.get_buffer();
    buf->create_tag("overset")->property_background() = "rgba(214,69,69,0.16)";

    // The gutter: each paragraph's style, beside its first line.
    m_gutter.set_content_width(kGutterW);
    m_gutter.set_draw_func(sigc::mem_fun(*this, &StoryEditor::draw_gutter));
    gtk_text_view_set_gutter(m_view.gobj(), GTK_TEXT_WINDOW_LEFT, GTK_WIDGET(m_gutter.gobj()));

    // Edits go to the document before the buffer takes them (positions as they were).
    buf->signal_insert().connect([this](const Gtk::TextBuffer::iterator& at, const Glib::ustring& text, int) {
        if (m_applying || m_story.empty()) return;
        m_applying = true;
        const core::TextPos p{at.get_line(), at.get_line_index()};
        core::doc::edit_text(m_journal, m_doc, m_story, p, p, text.raw(), "Typing", "story-ed:" + m_story);
        m_applying = false;
        m_edited.emit();
    }, false);
    buf->signal_erase().connect([this](const Gtk::TextBuffer::iterator& a, const Gtk::TextBuffer::iterator& b) {
        if (m_applying || m_story.empty()) return;
        m_applying = true;
        core::doc::edit_text(m_journal, m_doc, m_story, {a.get_line(), a.get_line_index()},
                             {b.get_line(), b.get_line_index()}, "", "Delete", "story-del:" + m_story);
        m_applying = false;
        m_edited.emit();
    }, false);
    buf->signal_changed().connect([this] { m_gutter.queue_draw(); });

    // Ctrl+Z / Ctrl+Shift+Z here are the document's (the window leaves a
    // text view's keys alone).
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType st) {
        const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
        const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
        if (!ctrl || (kv != GDK_KEY_z && kv != GDK_KEY_Z && kv != GDK_KEY_y)) return false;
        activate_action((kv == GDK_KEY_y || shift) ? "win.redo" : "win.undo");
        return true;
    }, false);
    m_view.add_controller(keys);

    m_scroll.set_child(m_view);
    m_scroll.set_vexpand(true);
    m_scroll.set_hexpand(true);
    append(m_scroll);
}

void StoryEditor::show_story(const std::string& story) {
    m_story = story;
    load();
}

void StoryEditor::model_changed() {
    if (m_story.empty()) return;
    if (!m_doc.as<core::Story>(m_story)) { m_story.clear(); load(); return; }
    if (!m_applying) {
        // Changed elsewhere: reload if the words differ.
        if (m_view.get_buffer()->get_text(false).raw() != shown_text(*m_doc.as<core::Story>(m_story))) load();
    }
    mark_overset();
    m_gutter.queue_draw();
}

void StoryEditor::load() {
    auto buf = m_view.get_buffer();
    const int at = buf->get_insert()->get_iter().get_offset();
    m_applying = true;
    const auto* st = m_doc.as<core::Story>(m_story);
    const std::string text = st ? shown_text(*st) : std::string{};
    buf->set_text(text);
    buf->place_cursor(buf->get_iter_at_offset(std::min(at, buf->end().get_offset())));
    m_applying = false;
    m_title.set_text(st ? "Story — " + st->name : "Story");
    mark_overset();
    m_gutter.queue_draw();
}

// What no frame holds: tinted, and counted in the header.
void StoryEditor::mark_overset() {
    auto buf = m_view.get_buffer();
    buf->remove_tag_by_name("overset", buf->begin(), buf->end());
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) { m_info.set_text(""); return; }
    const auto flow = layout::flow_story(m_doc, m_story);
    std::size_t words = 0;
    for (const auto& p : st->paras) {
        bool in = false;
        for (unsigned char c : p.text) {
            const bool sp = c == ' ' || c == '\t';
            if (!sp && !in) ++words;
            in = !sp;
        }
    }
    std::string info = std::to_string(words) + (words == 1 ? " word" : " words");
    if (flow.overset) {
        core::TextPos end{0, 0};
        for (const auto& ft : flow.frames)
            for (const auto& l : ft.lines)
                if (end < core::TextPos{l.para, l.end}) end = {l.para, l.end};
        if (flow.frames.empty() || flow.lines == 0) end = {0, 0};
        auto from = buf->get_iter_at_line_index(end.para, end.byte);
        buf->apply_tag_by_name("overset", from, buf->end());
        info += " · overset";
    }
    if (flow.frames.empty()) info += " · not placed";
    m_info.set_text(info);
}

void StoryEditor::draw_gutter(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return;
    const auto fg = m_gutter.get_color();
    auto buf = m_view.get_buffer();
    Gdk::Rectangle vis;
    m_view.get_visible_rect(vis);
    for (int p = 0; p < static_cast<int>(st->paras.size()) && p < buf->get_line_count(); ++p) {
        int y = 0, h = 0;
        m_view.get_line_yrange(buf->get_iter_at_line(p), y, h);
        if (y + h < vis.get_y() || y > vis.get_y() + vis.get_height()) continue;
        int wx = 0, wy = 0;
        m_view.buffer_to_window_coords(Gtk::TextWindowType::LEFT, 0, y, wx, wy);
        const auto* s = m_doc.as<core::Style>(st->paras[static_cast<std::size_t>(p)].style);
        auto lay = m_gutter.create_pango_layout(s ? s->name : "—");
        lay->set_font_description(Pango::FontDescription("Sans 8.5"));
        lay->set_width((w - 14) * PANGO_SCALE);
        lay->set_ellipsize(Pango::EllipsizeMode::END);
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.55);
        cr->move_to(8, wy + 3);
        lay->show_in_cairo_context(cr);
    }
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.12);
    cr->rectangle(w - 1, 0, 1, 100000);
    cr->fill();
}

}  // namespace composix::views
