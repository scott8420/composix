#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/box.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>

#include <cmath>

// Shell_notes_panel.cpp -- s121: Footnotes & Endnotes, one panel. Every
// setting the notes have (s111 numbering, s117 area, s118 look, s120
// placement) in one place, with a small picture of a page's foot drawn from
// them as they change. Not modal: the pages follow behind it. Each change is
// one step (the same verbs the Document settings rows use). Hide-on-close,
// built once (docs/resources-and-dialog-lifetime.md).

namespace composix {

namespace {
Gtk::Label* row_label(const char* text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(1.0f);
    l->add_css_class("dim-label");
    return l;
}
Gtk::Label* group_label(const char* text) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->add_css_class("heading");
    l->set_margin_top(6);
    return l;
}
}  // namespace

void Shell::open_notes_panel() {  // helper
    if (!m_np) {
        m_np = std::make_unique<Gtk::Window>();
        auto& w = *m_np;
        w.set_name("notes.panel");
        w.set_title("Footnotes & Endnotes");
        w.set_resizable(false);
        w.set_hide_on_close(true);
        w.set_transient_for(*this);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 18);
        box->add_css_class("cx-export");
        box->set_margin(18);

        // The picture: a page's foot as the settings make it.
        m_np_preview = Gtk::make_managed<Gtk::DrawingArea>();
        m_np_preview->set_name("notes.preview");
        m_np_preview->set_content_width(220);
        m_np_preview->set_content_height(300);
        m_np_preview->set_valign(Gtk::Align::START);
        m_np_preview->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int cw, int ch) { draw_notes_preview(cr, cw, ch); });
        box->append(*m_np_preview);

        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(6);
        grid->set_column_spacing(10);
        int r = 0;
        const auto add = [&](const char* label, Gtk::Widget& wd) {
            grid->attach(*row_label(label), 0, r);
            wd.set_hexpand(true);
            grid->attach(wd, 1, r++);
        };
        const auto drop = [](const char* name, std::vector<Glib::ustring> items) {
            auto d = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
            d->set_name(name);
            return d;
        };
        const auto spin = [](const char* name, double lo, double hi, double step, int digits) {
            auto s = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(lo, lo, hi, step, step * 4, 0), step, digits);
            s->set_name(name);
            s->set_numeric(true);
            return s;
        };
        auto title = Gtk::make_managed<Gtk::Label>("Footnotes & Endnotes");
        title->add_css_class("cx-export-title");
        title->set_xalign(0);
        grid->attach(*title, 0, r++, 2);

        grid->attach(*group_label("Where"), 0, r++, 2);
        m_np_at = drop("notes.at", {"Foot of the column", "End of the story", "End of each chapter"});   // s122
        add("Notes at", *m_np_at);
        m_np_area = spin("notes.area", 10, 100, 5, 0);
        add("Footnote area (%)", *m_np_area);
        m_np_title = Gtk::make_managed<Gtk::Entry>();
        m_np_title->set_name("notes.title");
        m_np_title->set_placeholder_text("No title");
        add("Endnotes title", *m_np_title);

        grid->attach(*group_label("Numbers"), 0, r++, 2);
        m_np_restart = drop("notes.restart", {"Through the story", "Each page", "Each section", "Each chapter"});   // s124
        add("Count", *m_np_restart);
        m_np_style = drop("notes.style", {"1 2 3", "i ii iii", "a b c", "* † ‡"});
        add("Numbers", *m_np_style);

        grid->attach(*group_label("Look"), 0, r++, 2);
        m_np_rule = spin("notes.rule", 0, 100, 5, 0);
        add("Rule (% of column)", *m_np_rule);
        m_np_rule_w = spin("notes.rule_w", 0.1, 6, 0.25, 2);
        add("Rule weight (pt)", *m_np_rule_w);
        m_np_space = spin("notes.space", 0, 36, 1, 1);
        add("Between notes (pt)", *m_np_space);
        m_np_cont = Gtk::make_managed<Gtk::Entry>();
        m_np_cont->set_name("notes.cont");
        m_np_cont->set_placeholder_text("e.g. (continued)");
        add("Continued line", *m_np_cont);
        m_np_note = Gtk::make_managed<Gtk::Label>();
        m_np_note->add_css_class("cx-export-note");
        m_np_note->set_xalign(0);
        m_np_note->set_wrap(true);
        m_np_note->set_max_width_chars(36);
        grid->attach(*m_np_note, 0, r++, 2);
        box->append(*grid);
        w.set_child(*box);

        // Each change: one step, through the document's verbs.
        const auto look = [this] {
            if (m_np_syncing) return;
            core::doc::set_note_look(m_journal, m_doc, static_cast<int>(std::lround(m_np_rule->get_value())), m_np_rule_w->get_value(),
                                     m_np_space->get_value(), m_np_cont->get_text());
        };
        const auto place = [this] {
            if (m_np_syncing) return;
            core::doc::set_note_place(m_journal, m_doc, static_cast<core::NoteAt>(std::min(2u, m_np_at->get_selected())), m_np_title->get_text());
        };
        m_np_at->property_selected().signal_changed().connect(place);
        m_np_area->signal_value_changed().connect([this] {
            if (!m_np_syncing) core::doc::set_note_area(m_journal, m_doc, static_cast<int>(std::lround(m_np_area->get_value())));
        });
        const auto numbering = [this] {
            if (m_np_syncing) return;
            core::doc::set_note_numbering(m_journal, m_doc, static_cast<core::NoteRestart>(std::min(3u, m_np_restart->get_selected())),
                                          static_cast<core::NoteStyle>(std::min(3u, m_np_style->get_selected())));
        };
        m_np_restart->property_selected().signal_changed().connect(numbering);
        m_np_style->property_selected().signal_changed().connect(numbering);
        m_np_rule->signal_value_changed().connect(look);
        m_np_rule_w->signal_value_changed().connect(look);
        m_np_space->signal_value_changed().connect(look);
        // Words: on Enter or leaving the field (not every keystroke a step).
        const auto on_leave = [](Gtk::Entry& e, const std::function<void()>& f) {
            e.signal_activate().connect(f);
            auto fc = Gtk::EventControllerFocus::create();
            fc->signal_leave().connect(f);
            e.add_controller(fc);
        };
        on_leave(*m_np_cont, look);
        on_leave(*m_np_title, place);
    }
    notes_panel_sync();
    m_np->present();
    if (auto lg = log::get(log::Area::Shell)) lg->info("notes panel: open");
}

// The panel from the document (after an edit here, an undo, another window).
void Shell::notes_panel_sync() {  // helper
    if (!m_np) return;
    const auto& s = m_doc.settings();
    m_np_syncing = true;
    const bool end = s.note_at != core::NoteAt::Foot;
    m_np_at->set_selected(static_cast<guint>(s.note_at));
    m_np_area->set_value(s.note_area);
    if (!m_np_title->has_focus()) m_np_title->set_text(s.note_title);
    m_np_restart->set_selected(static_cast<guint>(s.note_restart));
    m_np_style->set_selected(static_cast<guint>(s.note_style));
    m_np_rule->set_value(s.note_rule);
    m_np_rule_w->set_value(s.note_rule_w);
    m_np_space->set_value(s.note_space);
    if (!m_np_cont->has_focus()) m_np_cont->set_text(s.note_cont);
    // What doesn't apply, greyed: endnotes have no area, no rule, no restart.
    m_np_area->set_sensitive(!end);
    m_np_restart->set_sensitive(true);   // s124: endnotes count through the story or by chapter (page / section read as the story)
    m_np_rule->set_sensitive(!end);
    m_np_rule_w->set_sensitive(!end);
    m_np_cont->set_sensitive(!end);
    m_np_title->set_sensitive(end);
    int notes = 0;
    for (const auto& id : m_doc.ids_of<core::Story>())
        for (const auto& p : m_doc.as<core::Story>(id)->paras) notes += static_cast<int>(p.notes.size());
    m_np_note->set_text(std::to_string(notes) + (notes == 1 ? " note" : " notes") + " in the document." +
                        (s.note_at == core::NoteAt::ChapterEnd ? " Each chapter's notes come before the next paragraph that starts a new page (and at the story's end), numbered through the story."
                         : end ? " Endnotes follow each story's last paragraph, numbered through the story."
                             : " A note that doesn't fit in the area runs on to the next column."));
    m_np_syncing = false;
    m_np_preview->queue_draw();
}

// A page's foot, schematic: text lines, the rule, two notes -- the second cut
// with the continued line -- or, as endnotes, the title and the notes after
// the text. Grey bars for words; the numbers drawn.
void Shell::draw_notes_preview(const Cairo::RefPtr<Cairo::Context>& cr, int cw, int ch) {  // helper
    const auto& s = m_doc.settings();
    // The page.
    cr->set_source_rgb(1, 1, 1);
    cr->rectangle(0.5, 0.5, cw - 1, ch - 1);
    cr->fill_preserve();
    cr->set_source_rgba(0, 0, 0, 0.25);
    cr->set_line_width(1);
    cr->stroke();
    const double m = 16, x0 = m, w = cw - 2 * m, top = m, bottom = ch - m;
    const auto bar = [&](double y, double len, double h, double a, double in = 0) {   // `in`: past a note's number
        cr->set_source_rgba(0, 0, 0, a);
        cr->rectangle(x0 + in, y, std::max(4.0, len - in), h);
        cr->fill();
    };
    const double ni = 9;   // a note's words start past its number
    const auto number = [&](double x, double y, const std::string& t, double size) {
        cr->set_source_rgba(0, 0, 0, 0.85);
        cr->select_font_face("Serif", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::NORMAL);
        cr->set_font_size(size);
        cr->move_to(x, y);
        cr->show_text(t);
    };
    const double lead = 9, nlead = 7;
    const auto label = [&](int n) { return core::note_label(n, s.note_style); };
    if (s.note_at != core::NoteAt::Foot) {
        double y = top;
        for (int i = 0; i < 9; ++i, y += lead) bar(y, i == 8 ? w * 0.55 : w, 4, 0.22);
        y += 12;
        if (!s.note_title.empty()) {
            cr->set_source_rgba(0, 0, 0, 0.9);
            cr->select_font_face("Serif", Cairo::ToyFontFace::Slant::NORMAL, Cairo::ToyFontFace::Weight::BOLD);
            cr->set_font_size(10);
            cr->move_to(x0, y + 8);
            cr->show_text(s.note_title.substr(0, 24));
            y += 14;
        }
        for (int n = 1; n <= 3 && y < bottom - 10; ++n) {
            number(x0, y + 6, label(n), 7);
            for (int l = 0; l < (n == 2 ? 3 : 2) && y < bottom - 4; ++l, y += nlead) bar(y + 2, l == 0 ? w : w * 0.7, 3, 0.35, l == 0 ? ni : 0);
            y += std::min(12.0, s.note_space);
        }
        return;
    }
    // Notes at the foot: the area marked, the rule, two notes, the second cut.
    const double colh = bottom - top;
    const double area = colh * std::clamp(s.note_area, 10, 100) / 100.0;
    const double sp = std::min(12.0, s.note_space);
    const bool cut = !s.note_cont.empty();
    const int n1 = 2, n2 = cut ? 4 : 3;
    const double notes_h = (n1 + n2 + (cut ? 1 : 0)) * nlead + sp;
    const double nh = std::min(area - 8, notes_h);
    const double ny = bottom - nh;
    for (double y = top; y + lead < ny - 8; y += lead) bar(y, w, 4, 0.22);
    if (s.note_area < 100) {   // the area's top, dashed
        cr->set_source_rgba(0.2, 0.45, 0.85, 0.6);
        cr->set_line_width(0.75);
        std::vector<double> dash{3, 2};
        cr->set_dash(dash, 0);
        cr->move_to(x0 - 6, bottom - area);
        cr->line_to(x0 + w + 6, bottom - area);
        cr->stroke();
        cr->unset_dash();
    }
    if (s.note_rule > 0) {
        cr->set_source_rgba(0, 0, 0, 0.85);
        cr->rectangle(x0, ny - 5 - s.note_rule_w / 2, w * s.note_rule / 100.0, std::max(0.4, s.note_rule_w));
        cr->fill();
    }
    double y = ny;
    number(x0, y + 6, label(1), 7);
    for (int l = 0; l < n1; ++l, y += nlead) bar(y + 2, l == n1 - 1 ? w * 0.6 : w, 3, 0.35, l == 0 ? ni : 0);
    y += sp;
    number(x0, y + 6, label(2), 7);
    for (int l = 0; l < n2 && y < bottom - 2; ++l, y += nlead) bar(y + 2, w, 3, 0.35, l == 0 ? ni : 0);
    if (cut && y < bottom) {
        cr->set_source_rgba(0, 0, 0, 0.75);
        cr->select_font_face("Serif", Cairo::ToyFontFace::Slant::ITALIC, Cairo::ToyFontFace::Weight::NORMAL);
        cr->set_font_size(7);
        cr->move_to(x0, std::min(y + 6, bottom - 1));
        cr->show_text(s.note_cont.substr(0, 30));
    }
}

}  // namespace composix
