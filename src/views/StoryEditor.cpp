#include "views/StoryEditor.hpp"
#include "views/NoteWords.hpp"
#include "views/BackButton.hpp"
#include "layout/Flow.hpp"
#include "Log.hpp"

#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/textchildanchor.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textbuffer.h>
#include <tuple>
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
                t.replace(k, 3, core::marker_sign(m));   // s128: one table of signs
                k += 2;
            } else if (core::note_at(t, k)) {   // s112: a footnote -- where its chip goes (U+FFFC, an anchor)
                t.replace(k, 3, "\xEF\xBF\xBC");
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
    m_nav_btns = nav_buttons("story");   // s068
    m_head.append(*m_nav_btns.box);
    m_head.append(m_title);
    // s050: a character style for the selection (or the word at the cursor).
    m_char_btn.set_name("story.charstyle");
    m_char_btn.set_label("Character style");
    m_char_btn.set_tooltip_text("Set a character style on the selected words");
    m_char_btn.set_valign(Gtk::Align::CENTER);
    m_char_btn.add_css_class("flat");
    m_char_btn.set_popover(m_char_pop);
    m_head.append(m_char_btn);
    // s127: the notes pane, on / off.
    m_notes_btn.set_name("story.notes");
    m_notes_btn.set_label("Notes");
    m_notes_btn.set_tooltip_text("Show each footnote's words beside the text");
    m_notes_btn.set_valign(Gtk::Align::CENTER);
    m_notes_btn.add_css_class("flat");
    m_notes_btn.set_active(true);
    m_notes_btn.signal_toggled().connect([this] { fill_notes_pane(); });
    m_head.append(m_notes_btn);
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
    // s049: the words' own formatting.
    buf->create_tag("bold")->property_weight() = Pango::Weight::BOLD;
    buf->create_tag("italic")->property_style() = Pango::Style::ITALIC;
    {   // a character style: a faint underline (its name is in the inspector)
        auto t = buf->create_tag("cstyle");
        t->property_underline() = Pango::Underline::SINGLE;
        Gdk::RGBA c("rgba(91,127,166,0.75)");
        t->property_underline_rgba() = c;
    }

    // The gutter: each paragraph's style, beside its first line.
    m_gutter.set_content_width(kGutterW);
    m_gutter.set_draw_func(sigc::mem_fun(*this, &StoryEditor::draw_gutter));
    gtk_text_view_set_gutter(m_view.gobj(), GTK_TEXT_WINDOW_LEFT, GTK_WIDGET(m_gutter.gobj()));
    // s050: click a style's name -- a menu of paragraph styles for that paragraph.
    m_pick.set_parent(m_gutter);
    m_pick.set_has_arrow(true);
    m_pick.set_position(Gtk::PositionType::RIGHT);
    auto click = Gtk::GestureClick::create();
    click->signal_released().connect([this](int, double x, double y) {
        int bx = 0, by = 0;
        m_view.window_to_buffer_coords(Gtk::TextWindowType::LEFT, static_cast<int>(x), static_cast<int>(y), bx, by);
        Gtk::TextBuffer::iterator it;
        int top = 0;
        m_view.get_line_at_y(it, by, top);
        pick_para_style(it.get_line(), y);
    });
    m_gutter.add_controller(click);
    m_gutter.set_cursor(Gdk::Cursor::create("pointer"));

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
        // s049: bold / italic on the words here.
        if (ctrl && !shift && (kv == GDK_KEY_b || kv == GDK_KEY_i)) { toggle_look(kv == GDK_KEY_b); return true; }
        if (!ctrl || (kv != GDK_KEY_z && kv != GDK_KEY_Z && kv != GDK_KEY_y)) return false;
        activate_action((kv == GDK_KEY_y || shift) ? "win.redo" : "win.undo");
        return true;
    }, false);
    m_view.add_controller(keys);

    m_scroll.set_child(m_view);
    m_scroll.set_vexpand(true);
    m_scroll.set_hexpand(true);
    // s127: the text, and beside it the notes.
    m_notes_box.set_margin(12);
    m_notes_box.set_name("story.notes.box");
    m_notes_scroll.set_child(m_notes_box);
    m_notes_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_notes_scroll.set_size_request(260, -1);
    m_notes_scroll.add_css_class("cx-story-notes");
    m_split.set_start_child(m_scroll);
    m_split.set_end_child(m_notes_scroll);
    m_split.set_resize_start_child(true);
    m_split.set_resize_end_child(false);   // the text takes what's spare
    m_split.set_shrink_start_child(false);
    m_split.set_shrink_end_child(false);
    m_split.set_vexpand(true);
    m_scroll.set_size_request(240, -1);
    append(m_split);
    // The pane ~320 px wide at first; then where the divider is left.
    m_split.signal_map().connect([this] {
        Glib::signal_idle().connect_once([this] {
            if (m_split_set) return;
            const int w = m_split.get_width();
            if (w > 600) { m_split.set_position(w - 320); m_split_set = true; }
        });
    });
}

StoryEditor::~StoryEditor() {
    m_pick.unparent();
    m_note_pop.unparent();   // s112
}

// s119: a note's number as the reader sees it -- the flow's when numbering
// starts again by page / section (the window's flows), else the story's.
int StoryEditor::note_shown(const core::Story& st, int p, int k) {
    if (m_doc.settings().note_restart != core::NoteRestart::Story && m_flowp)
        if (const int n = layout::laid_note_number(m_flowp->get(m_doc, m_story), p, k); n > 0) return n;
    return core::note_number(m_doc, st, p, k);   // s124: by chapter too
}

// s112: each footnote mark as a chip -- its number; its words on hover;
// click to edit them. The mark's place in the buffer is an anchor (U+FFFC,
// three bytes like the mark), so places still map byte for byte.
void StoryEditor::place_note_chips() {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return;
    auto buf = m_view.get_buffer();
    const bool was = m_applying;
    m_applying = true;
    const auto style = m_doc.settings().note_style;
    for (int p = 0; p < static_cast<int>(st->paras.size()) && p < buf->get_line_count(); ++p) {
        const auto& para = st->paras[static_cast<std::size_t>(p)];
        for (std::size_t i = para.text.find(core::kMarkNote); i != std::string::npos; i = para.text.find(core::kMarkNote, i + 3)) {
            const int k = core::notes_in(para.text, 0, i);
            auto it = buf->get_iter_at_line_index(p, static_cast<int>(i));
            Glib::RefPtr<Gtk::TextChildAnchor> anchor = it.get_child_anchor();
            if (!anchor) {   // the plain U+FFFC from the text: made an anchor
                auto next = it;
                next.forward_char();
                it = buf->erase(it, next);
                anchor = buf->create_child_anchor(it);
            }
            const std::string words = k < static_cast<int>(para.notes.size()) ? para.notes[static_cast<std::size_t>(k)].text : std::string{};
            const std::string label = core::note_label(note_shown(*st, p, k), style);
            const std::string tip = "Footnote " + label + (words.empty() ? std::string(" (no words yet)") : ": " + words) +
                                    "\nClick to edit its words";
            const auto widgets = anchor->get_widgets();
            if (!widgets.empty()) {   // already a chip: brought up to date
                if (auto* b = dynamic_cast<Gtk::Button*>(widgets.front())) {
                    b->set_label(label);
                    b->set_tooltip_text(tip);
                }
                continue;
            }
            auto chip = Gtk::make_managed<Gtk::Button>(label);
            chip->set_name("story.note");
            chip->add_css_class("cx-note-chip");
            chip->add_css_class("flat");
            chip->set_tooltip_text(tip);
            chip->set_focus_on_click(false);
            const int pp = p, kk = k;
            chip->signal_clicked().connect([this, pp, kk, chip] {
                if (!focus_pane_note(pp, kk)) edit_note(pp, kk, *chip);   // s127: the pane's box when it's open
            });
            m_view.add_child_at_anchor(*chip, anchor);
        }
    }
    m_applying = was;
}

void StoryEditor::edit_note(int para, int k, Gtk::Widget& chip) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st || para >= static_cast<int>(st->paras.size())) return;
    const auto& pr = st->paras[static_cast<std::size_t>(para)];
    const std::string words = k < static_cast<int>(pr.notes.size()) ? pr.notes[static_cast<std::size_t>(k)].text : std::string{};
    if (m_note_pop.get_parent()) m_note_pop.unparent();
    m_note_pop.set_parent(chip);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto head = Gtk::make_managed<Gtk::Label>("Footnote " + core::note_label(note_shown(*st, para, k), m_doc.settings().note_style));
    head->set_xalign(0);
    head->add_css_class("heading");
    auto entry = Gtk::make_managed<NoteWords>();   // s116: several paragraphs
    entry->view().set_name("story.note.words");
    entry->set_text(words);
    auto hint = Gtk::make_managed<Gtk::Label>("Enter: a new paragraph \u00b7 Ctrl+Enter or Esc to finish");
    hint->set_xalign(0);
    hint->add_css_class("dim-label");
    box->append(*head);
    box->append(*entry);
    box->append(*hint);
    m_note_pop.set_child(*box);
    entry->signal_done().connect([this] { m_note_pop.popdown(); });
    m_note_conn.disconnect();
    m_note_conn = m_note_pop.signal_closed().connect([this, para, k, entry] {
        if (core::doc::set_note_text(m_journal, m_doc, m_story, para, k, entry->get_text())) m_edited.emit();
    });
    m_note_pop.popup();
    entry->focus_end();   // the cursor after the words (not all of them selected)
}

void StoryEditor::pick_para_style(int para, double y) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st || para < 0 || para >= static_cast<int>(st->paras.size())) return;
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto head = Gtk::make_managed<Gtk::Label>("Paragraph style");
    head->add_css_class("dim-label");
    head->set_xalign(0.0f);
    head->set_margin(4);
    box->append(*head);
    for (const auto& id : m_doc.ids_of<core::Style>()) {
        const auto* s = m_doc.as<core::Style>(id);
        if (s->kind != core::StyleKind::Paragraph) continue;
        auto b = Gtk::make_managed<Gtk::Button>((id == st->paras[static_cast<std::size_t>(para)].style ? "\u2713 " : "    ") + s->name);
        b->add_css_class("flat");
        if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) l->set_xalign(0.0f);
        b->set_name("story.pick." + id);
        b->signal_clicked().connect([this, para, id] { m_pick.popdown(); apply_para_style(para, id); });
        box->append(*b);
    }
    auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
    sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sc->set_max_content_height(360);
    sc->set_propagate_natural_height(true);
    sc->set_child(*box);
    m_pick.set_child(*sc);
    Gdk::Rectangle at(8, static_cast<int>(y), 1, 1);
    m_pick.set_pointing_to(at);
    m_pick.popup();
}

// The paragraph clicked -- or, when the selection takes it in, every
// paragraph of the selection.
void StoryEditor::apply_para_style(int para, const std::string& id) {
    auto buf = m_view.get_buffer();
    Gtk::TextBuffer::iterator ia, ib;
    buf->get_selection_bounds(ia, ib);
    core::TextPos a{para, 0}, b{para, 0};
    if (ia.get_line() <= para && para <= ib.get_line() && ia != ib) { a = {ia.get_line(), 0}; b = {ib.get_line(), 0}; }
    if (core::doc::set_para_style(m_journal, m_doc, m_story, a, b, id)) m_edited.emit();
    m_gutter.queue_draw();
}

void StoryEditor::fill_char_menu() {
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    const auto add = [&](const std::string& id, const std::string& name) {
        auto b = Gtk::make_managed<Gtk::Button>(name);
        b->add_css_class("flat");
        if (auto* l = dynamic_cast<Gtk::Label*>(b->get_child())) l->set_xalign(0.0f);
        b->set_name("story.char." + (id.empty() ? std::string("none") : id));
        b->signal_clicked().connect([this, id] { m_char_pop.popdown(); apply_char_style(id); });
        box->append(*b);
    };
    add("", "None");
    for (const auto& id : m_doc.ids_of<core::Style>())
        if (const auto* s = m_doc.as<core::Style>(id); s->kind == core::StyleKind::Character) add(id, s->name);
    m_char_pop.set_child(*box);
}

void StoryEditor::apply_char_style(const std::string& id) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return;
    auto buf = m_view.get_buffer();
    Gtk::TextBuffer::iterator ia, ib;
    buf->get_selection_bounds(ia, ib);
    core::TextPos a{ia.get_line(), ia.get_line_index()}, b{ib.get_line(), ib.get_line_index()};
    if (a == b) std::tie(a, b) = core::word_at(*st, a);
    if (a != b && core::doc::set_char_style(m_journal, m_doc, m_story, a, b, id)) m_edited.emit();
    m_view.grab_focus();
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
        auto b = m_view.get_buffer();   // (the slice: chips' anchors included)
        if (b->get_slice(b->begin(), b->end(), true).raw() != shown_text(*m_doc.as<core::Story>(m_story))) load();
        else place_note_chips();   // a note's words changed: its chip's tooltip
    } else if (m_doc.settings().note_restart != core::NoteRestart::Story) {
        place_note_chips();   // s119: typed here -- numbers by page may have moved
    }
    sync_notes_pane();   // s127
    mark_overset();
    show_looks();
    if (!m_char_pop.get_visible()) fill_char_menu();   // s050: styles added / renamed
    m_gutter.queue_draw();
}

bool StoryEditor::toggle_look(bool bold) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return false;
    auto buf = m_view.get_buffer();
    Gtk::TextBuffer::iterator ia, ib;
    buf->get_selection_bounds(ia, ib);
    core::TextPos a{ia.get_line(), ia.get_line_index()}, b{ib.get_line(), ib.get_line_index()};
    if (a == b) std::tie(a, b) = core::word_at(*st, a);
    if (a == b) return false;
    const bool on = !(bold ? core::all_bold(m_doc, *st, a, b) : core::all_italic(m_doc, *st, a, b));
    const bool did = core::doc::set_local(m_journal, m_doc, m_story, a, b, bold ? "Bold" : "Italic",
                                          [&](core::Local& l) { (bold ? l.bold : l.italic) = on ? 1 : 0; });
    if (did) m_edited.emit();
    return did;
}

void StoryEditor::show_looks() {
    auto buf = m_view.get_buffer();
    for (const char* t : {"bold", "italic", "cstyle"}) buf->remove_tag_by_name(t, buf->begin(), buf->end());
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return;
    const int lines = buf->get_line_count();
    for (int p = 0; p < static_cast<int>(st->paras.size()) && p < lines; ++p)
        for (const auto& w : core::word_looks(m_doc, st->paras[static_cast<std::size_t>(p)])) {
            const auto a = buf->get_iter_at_line_index(p, w.start), b = buf->get_iter_at_line_index(p, w.end);
            if (w.bold) buf->apply_tag_by_name("bold", a, b);
            if (w.italic) buf->apply_tag_by_name("italic", a, b);
            if (!w.cstyle.empty()) buf->apply_tag_by_name("cstyle", a, b);
        }
}

void StoryEditor::load() {
    auto buf = m_view.get_buffer();
    const int at = buf->get_insert()->get_iter().get_offset();
    m_applying = true;
    const auto* st = m_doc.as<core::Story>(m_story);
    const std::string text = st ? shown_text(*st) : std::string{};
    buf->set_text(text);
    place_note_chips();   // s112
    fill_notes_pane();    // s127
    buf->place_cursor(buf->get_iter_at_offset(std::min(at, buf->end().get_offset())));
    m_applying = false;
    m_title.set_text(st ? "Story — " + st->name : "Story");
    mark_overset();
    show_looks();
    fill_char_menu();   // s050
    m_gutter.queue_draw();
}

// What no frame holds: tinted, and counted in the header.
void StoryEditor::mark_overset() {
    auto buf = m_view.get_buffer();
    buf->remove_tag_by_name("overset", buf->begin(), buf->end());
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) { m_info.set_text(""); return; }
    // s119: the window's flows when shared (one flow per story, not one per change).
    const layout::StoryFlow own = m_flowp ? layout::StoryFlow{} : layout::flow_story(m_doc, m_story);
    const layout::StoryFlow& flow = m_flowp ? m_flowp->get(m_doc, m_story) : own;
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
        const auto& para = st->paras[static_cast<std::size_t>(p)];
        const auto* s = m_doc.as<core::Style>(para.style);
        // s055: a break typed before it -- named, with a dashed line over the paragraph.
        static const char* brk[] = {"", "\u2913 Column \u00b7 ", "\u2913 Frame \u00b7 ", "\u2913 Page \u00b7 ", "\u2913 Right page \u00b7 ", "\u2913 Left page \u00b7 "};
        const std::string tag = brk[static_cast<int>(para.brk)];
        if (!tag.empty()) {
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.45);
            cr->set_line_width(1);
            const std::vector<double> dash{3, 3};
            cr->set_dash(dash, 0);
            cr->move_to(4, wy + 0.5);
            cr->line_to(w - 4, wy + 0.5);
            cr->stroke();
            cr->unset_dash();
        }
        auto lay = m_gutter.create_pango_layout(tag + (s ? s->name : "—"));
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

// ── s127: the notes pane ───────────────────────────────────────────────────
// Every footnote of the story, in order: its number (click: the text at its
// mark) and its words in a box -- Enter a new paragraph; they go in on
// leaving the box or Ctrl+Enter (one step). Shown when the story has notes
// and Notes is on.
void StoryEditor::fill_notes_pane() {
    while (auto* c = m_notes_box.get_first_child()) m_notes_box.remove(*c);
    m_note_rows.clear();
    const auto* st = m_doc.as<core::Story>(m_story);
    int count = 0;
    if (st)
        for (const auto& p : st->paras) count += static_cast<int>(p.notes.size());
    m_notes_btn.set_sensitive(count > 0);
    m_notes_btn.set_tooltip_text(count > 0 ? "Show each footnote's words beside the text" : "This story has no footnotes");
    m_notes_scroll.set_visible(count > 0 && m_notes_btn.get_active());
    if (!st || count == 0) return;
    const auto style = m_doc.settings().note_style;
    auto title = Gtk::make_managed<Gtk::Label>(std::to_string(count) + (count == 1 ? " footnote" : " footnotes"));
    title->set_xalign(0);
    title->add_css_class("heading");
    m_notes_box.append(*title);
    for (int p = 0; p < static_cast<int>(st->paras.size()); ++p) {
        const auto& para = st->paras[static_cast<std::size_t>(p)];
        for (int k = 0; k < static_cast<int>(para.notes.size()); ++k) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            row->set_name("story.note.row");
            auto head = Gtk::make_managed<Gtk::Button>("Footnote " + core::note_label(note_shown(*st, p, k), style));
            head->add_css_class("flat");
            head->set_halign(Gtk::Align::START);
            head->set_tooltip_text("Show its mark in the text");
            head->signal_clicked().connect([this, p, k] { show_chip(p, k); });
            auto words = Gtk::make_managed<NoteWords>();
            words->view().set_name("story.note.pane.words");
            words->set_size_request(200, -1);
            words->set_text(para.notes[static_cast<std::size_t>(k)].text);
            // In on leaving the box, or Ctrl+Enter / Esc.
            const auto commit = [this, p, k, words] {
                const auto* s2 = m_doc.as<core::Story>(m_story);
                if (!s2 || p >= static_cast<int>(s2->paras.size()) || k >= static_cast<int>(s2->paras[static_cast<std::size_t>(p)].notes.size())) return;
                const auto t = words->get_text();
                if (t == s2->paras[static_cast<std::size_t>(p)].notes[static_cast<std::size_t>(k)].text) return;
                if (core::doc::set_note_text(m_journal, m_doc, m_story, p, k, t)) m_edited.emit();
            };
            words->signal_done().connect([this, commit] { commit(); m_view.grab_focus(); });
            auto fc = Gtk::EventControllerFocus::create();
            fc->signal_leave().connect(commit);
            words->view().add_controller(fc);
            row->append(*head);
            row->append(*words);
            m_notes_box.append(*row);
            m_note_rows.push_back({p, k, row, words, head});
        }
    }
}

void StoryEditor::sync_notes_pane() {
    const auto* st = m_doc.as<core::Story>(m_story);
    std::vector<std::pair<int, int>> now;
    if (st)
        for (int p = 0; p < static_cast<int>(st->paras.size()); ++p)
            for (int k = 0; k < static_cast<int>(st->paras[static_cast<std::size_t>(p)].notes.size()); ++k) now.push_back({p, k});
    bool same = now.size() == m_note_rows.size();
    for (std::size_t i = 0; same && i < now.size(); ++i) same = now[i].first == m_note_rows[i].p && now[i].second == m_note_rows[i].k;
    if (!same) { fill_notes_pane(); return; }
    const auto style = m_doc.settings().note_style;
    for (auto& r : m_note_rows) {   // the words (not the one being typed in), the numbers
        const auto& t = st->paras[static_cast<std::size_t>(r.p)].notes[static_cast<std::size_t>(r.k)].text;
        if (!r.words->view().has_focus() && r.words->get_text() != t) r.words->set_text(t);
        r.head->set_label("Footnote " + core::note_label(note_shown(*st, r.p, r.k), style));
    }
}

bool StoryEditor::focus_pane_note(int para, int k) {
    if (!m_notes_scroll.get_visible()) return false;
    for (auto& r : m_note_rows)
        if (r.p == para && r.k == k) {
            r.words->focus_end();
            // Scrolled to: the row's place in the pane.
            double x = 0, y = 0;
            if (r.row->translate_coordinates(m_notes_box, 0, 0, x, y)) m_notes_scroll.get_vadjustment()->set_value(std::max(0.0, y - 12));
            return true;
        }
    return false;
}

void StoryEditor::show_chip(int para, int k) {
    const auto* st = m_doc.as<core::Story>(m_story);
    if (!st) return;
    const int m = core::doc::note_mark_byte(*st, para, k);
    if (m < 0) return;
    auto buf = m_view.get_buffer();
    auto it = buf->get_iter_at_line_index(para, m);
    buf->place_cursor(it);
    m_view.scroll_to(it, 0.2);
    m_view.grab_focus();
}

}  // namespace composix::views
