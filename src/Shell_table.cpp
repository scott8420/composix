#include "views/Math.hpp"
#include "Shell.hpp"

#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

// Shell_table.cpp -- s318: tables in the text.
//   Type > Insert Table...  a popover at the caret: rows, columns, header rows;
//                           the table a paragraph of its own there, as wide
//                           as the column; then the editor.
//   Type > Edit Table...    the table editor: its cells as a grid (a cell's
//                           paragraphs shown with " | " between), rows and
//                           columns added / taken, header rows, the header's
//                           fill, the grid's weight. Apply: one step.

namespace composix {

namespace {
// A cell's paragraphs as one line ("a | b"), and back.
std::string shown(const core::TableCell& c) {
    std::string out;
    for (std::size_t i = 0; i < c.paras.size(); ++i) out += (i ? " | " : "") + c.paras[i].text;
    return out;
}
std::string typed(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s.compare(i, 3, " | ") == 0) { out += '\n'; i += 3; }
        else out += s[i++];
    }
    return out;
}
}  // namespace

void Shell::on_insert_table() {  // handler
    auto& cv = m_canvas_view.canvas();
    if (!cv.editing()) {
        alert("Click into text first", "A table goes in at the text cursor: double-click a text frame, then Insert Table.");
        return;
    }
    if (m_table_pop) { m_table_pop->popdown(); m_table_pop->unparent(); m_table_pop.reset(); }
    m_table_pop = std::make_unique<Gtk::Popover>();
    m_table_pop->set_name("table.insert");
    m_table_pop->set_parent(cv);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->set_margin(8);
    auto head = Gtk::make_managed<Gtk::Label>("Insert Table");
    head->set_xalign(0);
    head->add_css_class("heading");
    box->append(*head);
    const auto row = [&](const char* label, Gtk::SpinButton& sb) {
        auto r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto l = Gtk::make_managed<Gtk::Label>(label);
        l->set_xalign(0);
        l->set_size_request(100, -1);
        r->append(*l);
        r->append(sb);
        box->append(*r);
    };
    auto rows = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(4, 1, 500, 1, 5), 1, 0);
    views::allow_math(*rows);   // s349: math in the field
    auto cols = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(3, 1, 50, 1, 2), 1, 0);
    views::allow_math(*cols);   // s349: math in the field
    auto heads = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(1, 0, 20, 1, 1), 1, 0);
    views::allow_math(*heads);   // s349: math in the field
    rows->set_name("table.rows");
    cols->set_name("table.cols");
    heads->set_name("table.heads");
    row("Rows", *rows);
    row("Columns", *cols);
    row("Header rows", *heads);
    auto ok = Gtk::make_managed<Gtk::Button>("Insert");
    ok->set_name("table.ok");
    ok->add_css_class("suggested-action");
    ok->set_halign(Gtk::Align::END);
    box->append(*ok);
    m_table_pop->set_child(*box);
    ok->signal_clicked().connect([this, rows, cols, heads] {
        auto& c = m_canvas_view.canvas();
        const std::string story = c.edit_story_id();
        double width = 288;
        if (const auto* f = m_doc.as<core::Frame>(c.edit_frame_id())) width = core::column_width(*f, 0) - 1;
        auto t = core::doc::make_table(static_cast<int>(rows->get_value()), static_cast<int>(cols->get_value()), width);
        t.header_rows = std::min(static_cast<int>(heads->get_value()), static_cast<int>(t.rows.size()));
        for (const auto& sw : m_doc.ids_of<core::Swatch>())   // a light header, in the document's black
            if (m_doc.as<core::Swatch>(sw)->name == "Black") { t.header_fill = sw; t.header_tint = 0.12; break; }
        const int p = core::doc::insert_table(m_journal, m_doc, story, c.edit_range().first, t);
        if (auto lg = log::get(log::Area::Shell)) lg->info("table: {}x{} at {} para {}", t.rows.size(), t.widths.size(), story, p);
        if (m_table_pop) m_table_pop->popdown();
        c.model_changed();
        if (p >= 0) Glib::signal_idle().connect_once([this, story, p] { open_table_editor(story, p); });
    });
    const auto r = cv.caret_screen();
    if (r.w > 0)
        m_table_pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(r.x), static_cast<int>(r.y), 1, static_cast<int>(std::max(1.0, r.h))));
    m_table_pop->popup();
}

// s324: the light header Insert Table gives -- the document's Black
std::string Shell::table_header_black() const {  // helper
    for (const auto& sw : m_doc.ids_of<core::Swatch>())
        if (m_doc.as<core::Swatch>(sw)->name == "Black") return sw;
    return {};
}

// s324: Type > Convert Text to Table -- the paragraphs the selection touches
// (else the caret's), a row each, tabs between the cells; a first row
// over others is the header.
void Shell::on_text_to_table() {  // handler
    auto& cv = m_canvas_view.canvas();
    if (!cv.editing()) {
        alert("Select some text first", "Convert Text to Table makes a row of each paragraph selected, a cell of each run of words between tabs.");
        return;
    }
    const std::string story = cv.edit_story_id();
    auto [a, b] = cv.edit_range();
    if (b < a) std::swap(a, b);
    int p1 = b.para;
    if (b.byte == 0 && b.para > a.para) --p1;   // a selection ending at a paragraph's start leaves that one out
    double width = 288;
    if (const auto* f = m_doc.as<core::Frame>(cv.edit_frame_id())) width = core::column_width(*f, 0) - 1;
    cv.end_edit();
    const int p = core::doc::text_to_table(m_journal, m_doc, story, a.para, p1, width, p1 > a.para ? 1 : 0, table_header_black());
    if (p == -2) {
        alert("Can't make a table of these", "One of the paragraphs holds a footnote, cross-reference, anchor, index entry or table -- they can't go into a cell.");
        return;
    }
    if (p < 0) return;
    if (auto lg = log::get(log::Area::Shell)) lg->info("text to table: {} paras {}..{} -> para {}", story, a.para, p1, p);
    cv.model_changed();
    Glib::signal_idle().connect_once([this, story, p] { open_table_editor(story, p); });
}

// s324: Type > Convert Table to Text -- the table at (or after) the caret, or
// the picked frame's first.
void Shell::on_table_to_text() {  // handler
    auto& cv = m_canvas_view.canvas();
    std::string story;
    int from = 0;
    if (cv.editing()) { story = cv.edit_story_id(); from = cv.edit_range().first.para; }
    else if (cv.cell_editing()) { story = cv.cell_story(); from = cv.typing_cell().para; }   // s330
    else
        for (const auto& id : cv.selection())
            if (const auto* f = m_doc.as<core::Frame>(id); f && f->kind == core::FrameKind::Text && !f->story.empty()) { story = f->story; break; }
    const auto* st = m_doc.as<core::Story>(story);
    int para = -1;
    if (st) {
        for (int p = from; p < static_cast<int>(st->paras.size()) && para < 0; ++p) if (!st->paras[static_cast<std::size_t>(p)].tables.empty()) para = p;
        for (int p = 0; p < from && para < 0; ++p) if (!st->paras[static_cast<std::size_t>(p)].tables.empty()) para = p;
    }
    if (para < 0) {
        alert("No table here", "Pick a text frame holding a table (or click into its text), then Convert Table to Text.");
        return;
    }
    if (m_table_win) m_table_win->hide();   // its table is going
    if (cv.editing()) cv.end_edit();
    cv.end_cell();   // s330
    const int rows = core::doc::table_to_text(m_journal, m_doc, story, para);
    if (auto lg = log::get(log::Area::Shell)) lg->info("table to text: {} para {} -> {} paragraphs", story, para, rows);
    cv.model_changed();
}

void Shell::on_edit_table() {  // handler
    auto& cv = m_canvas_view.canvas();
    if (cv.cell_editing()) {   // s330: typing in a cell -- its table, that cell
        const auto c = cv.typing_cell();
        open_table_editor(cv.cell_story(), c.para, c.row, c.col);
        return;
    }
    std::string story;
    int from = 0;
    if (cv.editing()) { story = cv.edit_story_id(); from = cv.edit_range().first.para; }
    else
        for (const auto& id : cv.selection())
            if (const auto* f = m_doc.as<core::Frame>(id); f && f->kind == core::FrameKind::Text && !f->story.empty()) { story = f->story; break; }
    const auto* st = m_doc.as<core::Story>(story);
    int para = -1;
    if (st) {   // the table at or after the caret, else the story's first
        for (int p = from; p < static_cast<int>(st->paras.size()) && para < 0; ++p) if (!st->paras[static_cast<std::size_t>(p)].tables.empty()) para = p;
        for (int p = 0; p < from && para < 0; ++p) if (!st->paras[static_cast<std::size_t>(p)].tables.empty()) para = p;
    }
    if (para < 0) {
        alert("No table here", "Pick a text frame holding a table (or click into its text), then Edit Table.");
        return;
    }
    open_table_editor(story, para);
}

void Shell::open_table_editor(const std::string& story, int para, int focus_row, int focus_col) {  // helper
    const auto* t0 = core::doc::table_at(m_doc, story, para);
    if (!t0) return;
    auto work = std::make_shared<core::Table>(*t0);
    if (!m_table_win) {
        m_table_win = std::make_unique<Gtk::Window>();
        m_table_win->set_name("table.editor");
        m_table_win->set_title("Table");
        m_table_win->set_hide_on_close(true);
        m_table_win->set_default_size(640, 420);
        auto keys = Gtk::EventControllerKey::create();
        keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
            if (k != GDK_KEY_Escape) return false;
            m_table_win->set_visible(false);
            return true;
        }, false);
        m_table_win->add_controller(keys);
    }
    if (auto* top = dynamic_cast<Gtk::Window*>(get_root())) m_table_win->set_transient_for(*top);
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    outer->set_margin(12);
    // the structure
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_name("table.grid");
    grid->set_row_spacing(2);
    grid->set_column_spacing(2);
    auto entries = std::make_shared<std::vector<std::vector<Gtk::Entry*>>>();
    // the grid of cells from `work` (what's typed kept first)
    const auto keep = [work, entries] {
        for (std::size_t r = 0; r < entries->size() && r < work->rows.size(); ++r)
            for (std::size_t c = 0; c < (*entries)[r].size() && c < work->rows[r].cells.size(); ++c)
                if ((*entries)[r][c]) core::doc::set_cell_words(work->rows[r].cells[c], typed((*entries)[r][c]->get_text()));
    };
    // s323: the cell the tools act on (the one last typed in / focused)
    auto cur = std::make_shared<std::pair<int, int>>(std::max(0, focus_row), std::max(0, focus_col));
    const auto picked = std::make_shared<std::function<void()>>([] {});   // s325: the picked cell's rows follow it
    const auto fill = std::make_shared<std::function<void()>>();
    *fill = [work, entries, grid, cur, picked] {
        while (auto* ch = grid->get_first_child()) grid->remove(*ch);
        entries->clear();
        const int R = static_cast<int>(work->rows.size()), C = core::doc::table_cols(*work);
        for (int r = 0; r < R; ++r) {
            const bool foot = work->footer_rows > 0 && r >= R - work->footer_rows && r >= work->header_rows;   // s326
            auto lab = Gtk::make_managed<Gtk::Label>(r < work->header_rows ? "H" : foot ? "F" : std::to_string(r + 1 - work->header_rows));
            lab->add_css_class("dim-label");
            lab->set_size_request(28, -1);
            grid->attach(*lab, 0, r + 1);
            entries->emplace_back(static_cast<std::size_t>(C), nullptr);
            for (int c = 0; c < C; ++c) {
                if (core::doc::owner_of(*work, r, c) != std::pair<int, int>{r, c}) continue;   // s323: covered -- the merge's entry spans it
                const auto& cell = work->rows[static_cast<std::size_t>(r)].cells[static_cast<std::size_t>(c)];
                auto e = Gtk::make_managed<Gtk::Entry>();
                e->set_name("table.cell." + std::to_string(r) + "." + std::to_string(c));
                e->set_hexpand(true);
                e->set_width_chars(8);
                e->set_text(shown(cell));
                if (r < work->header_rows || foot) e->add_css_class("cx-table-head");
                if (cell.span_cols > 1 || cell.span_rows > 1) e->add_css_class("cx-table-merged");
                auto focus = Gtk::EventControllerFocus::create();
                focus->signal_enter().connect([cur, picked, r, c] { *cur = {r, c}; (*picked)(); });
                e->add_controller(focus);
                grid->attach(*e, c + 1, r + 1, std::max(1, cell.span_cols), std::max(1, cell.span_rows));
                (*entries)[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = e;
            }
        }
        // the columns' widths over them (points)
        for (int c = 0; c < C; ++c) {
            auto sb = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->widths[static_cast<std::size_t>(c)], 12, 2000, 1, 9), 1, 1);
            views::allow_math(*sb);   // s349: math in the field
            sb->set_name("table.width." + std::to_string(c));
            sb->set_tooltip_text("This column's width, points (the next column gives the difference)");
            sb->signal_value_changed().connect([work, sb, c, grid] {
                if (core::doc::table_set_width(*work, c, sb->get_value()))   // the neighbour's box follows
                    if (auto* nb = dynamic_cast<Gtk::SpinButton*>(grid->get_child_at(c + 2, 0)); nb && c + 1 < core::doc::table_cols(*work))
                        nb->set_value(work->widths[static_cast<std::size_t>(c) + 1]);
            });
            grid->attach(*sb, c + 1, 0);
        }
    };
    const auto act = [&](const char* label, const char* name, std::function<void()> f) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->set_name(std::string("table.") + name);
        b->signal_clicked().connect([keep, fill, f] { keep(); f(); (*fill)(); });
        bar->append(*b);
    };
    // s326: a row added / taken at the body's end (before the footer)
    act("+ Row", "add_row", [work] { core::doc::table_insert_row(*work, static_cast<int>(work->rows.size()) - std::max(0, work->footer_rows)); });
    act("− Row", "del_row", [work] {
        const int R = static_cast<int>(work->rows.size()), body_last = R - 1 - std::max(0, work->footer_rows);
        core::doc::table_delete_row(*work, body_last >= work->header_rows ? body_last : R - 1);
    });
    act("+ Column", "add_col", [work] { core::doc::table_insert_col(*work, core::doc::table_cols(*work)); });
    act("− Column", "del_col", [work] { core::doc::table_delete_col(*work, core::doc::table_cols(*work) - 1); });
    auto hl = Gtk::make_managed<Gtk::Label>("Header rows");
    hl->set_margin_start(12);
    bar->append(*hl);
    auto heads = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->header_rows, 0, 20, 1, 1), 1, 0);
    views::allow_math(*heads);   // s349: math in the field
    heads->set_name("table.heads");
    heads->signal_value_changed().connect([keep, fill, work, heads] {
        keep();
        work->header_rows = std::clamp(static_cast<int>(heads->get_value()), 0, static_cast<int>(work->rows.size()));
        (*fill)();
    });
    bar->append(*heads);
    auto fl2 = Gtk::make_managed<Gtk::Label>("Footer rows");   // s326
    fl2->set_margin_start(12);
    bar->append(*fl2);
    auto feet = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->footer_rows, 0, 20, 1, 1), 1, 0);
    views::allow_math(*feet);   // s349: math in the field
    feet->set_name("table.feet");
    feet->set_tooltip_text("The table's last rows, set again at the foot of each column it breaks across");
    feet->signal_value_changed().connect([keep, fill, work, feet] {
        keep();
        work->footer_rows = std::clamp(static_cast<int>(feet->get_value()), 0, std::max(0, static_cast<int>(work->rows.size()) - work->header_rows));
        (*fill)();
    });
    bar->append(*feet);
    auto gl = Gtk::make_managed<Gtk::Label>("Grid");
    gl->set_margin_start(12);
    bar->append(*gl);
    auto weight = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->rule_w, 0, 6, 0.25, 1), 0.25, 2);
    views::allow_math(*weight);   // s349: math in the field
    weight->set_name("table.rule_w");
    weight->set_tooltip_text("The grid's lines, points (0: none)");
    weight->signal_value_changed().connect([work, weight] { work->rule_w = weight->get_value(); });
    bar->append(*weight);
    outer->append(*bar);
    // the swatches, "None" first (s325: the fills' dropdowns)
    std::vector<Glib::ustring> sw_names{"None"};
    std::vector<std::string> sw_ids{""};
    for (const auto& sw : m_doc.ids_of<core::Swatch>()) {
        sw_names.push_back(m_doc.as<core::Swatch>(sw)->name);
        sw_ids.push_back(sw);
    }
    const auto sw_index = [sw_ids](const std::string& id) {
        for (std::size_t k = 0; k < sw_ids.size(); ++k) if (sw_ids[k] == id) return static_cast<guint>(k);
        return 0u;
    };
    {   // the header's fill; s325: the rows' bands
        auto fb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto fl = Gtk::make_managed<Gtk::Label>("Header fill");
        fb->append(*fl);
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(sw_names));
        dd->set_name("table.header_fill");
        dd->set_selected(sw_index(work->header_fill));
        dd->property_selected().signal_changed().connect([work, dd, sw_ids] {
            const auto k = dd->get_selected();
            if (k < sw_ids.size()) work->header_fill = sw_ids[k];
        });
        fb->append(*dd);
        auto bl = Gtk::make_managed<Gtk::Label>("Alternate rows");
        bl->set_margin_start(12);
        fb->append(*bl);
        auto bd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(sw_names));
        bd->set_name("table.band_fill");
        bd->set_tooltip_text("Every second run of body rows filled with this (a cell's own fill over it)");
        bd->set_selected(sw_index(work->band_fill));
        fb->append(*bd);
        auto bt = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->band_tint * 100, 5, 100, 5, 10), 5, 0);
        views::allow_math(*bt);   // s349: math in the field
        bt->set_name("table.band_tint");
        bt->set_tooltip_text("The bands' tint, percent");
        fb->append(*bt);
        auto el = Gtk::make_managed<Gtk::Label>("% every");
        fb->append(*el);
        auto be = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->band_every, 1, 9, 1, 1), 1, 0);
        views::allow_math(*be);   // s349: math in the field
        be->set_name("table.band_every");
        be->set_tooltip_text("Rows in a band (1: every other row)");
        fb->append(*be);
        auto rl = Gtk::make_managed<Gtk::Label>("rows");
        fb->append(*rl);
        auto tip = Gtk::make_managed<Gtk::Label>("A cell's paragraphs: “a | b”");
        tip->add_css_class("dim-label");
        tip->set_margin_start(12);
        fb->append(*tip);
        bd->property_selected().signal_changed().connect([work, bd, sw_ids] {
            const auto k = bd->get_selected();
            if (k < sw_ids.size()) work->band_fill = sw_ids[k];
        });
        bt->signal_value_changed().connect([work, bt] { work->band_tint = bt->get_value() / 100.0; });
        be->signal_value_changed().connect([work, be] { work->band_every = static_cast<int>(be->get_value()); });
        outer->append(*fb);
    }
    {   // s323: the picked cell -- merged across / down, unmerged; its words top / middle / bottom; the table in its column
        auto cb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        const auto tool = [&](const char* label, const char* name, std::function<void()> f) {
            auto b = Gtk::make_managed<Gtk::Button>(label);
            b->set_name(std::string("table.") + name);
            b->signal_clicked().connect([keep, fill, f] { keep(); f(); (*fill)(); });
            cb->append(*b);
        };
        tool("Merge \u2192", "merge_across", [work, cur] {
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first < 0) return;
            const auto& x = work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)];
            core::doc::table_merge(*work, o.first, o.second, x.span_cols + 1, x.span_rows);
        });
        tool("Merge \u2193", "merge_down", [work, cur] {
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first < 0) return;
            const auto& x = work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)];
            core::doc::table_merge(*work, o.first, o.second, x.span_cols, x.span_rows + 1);
        });
        tool("Unmerge", "unmerge", [work, cur] {
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first >= 0) core::doc::table_unmerge(*work, o.first, o.second);
        });
        auto vl = Gtk::make_managed<Gtk::Label>("Words");
        vl->set_margin_start(12);
        cb->append(*vl);
        auto va = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Top", "Middle", "Bottom"}));
        va->set_name("table.valign");
        va->set_tooltip_text("The picked cell's words: at its top, middle or bottom");
        va->property_selected().signal_changed().connect([work, cur, va] {
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first >= 0) work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)].valign = static_cast<int>(va->get_selected());
        });
        cb->append(*va);
        auto rl = Gtk::make_managed<Gtk::Label>("Row at least");   // s331: the picked cell's row
        rl->set_margin_start(12);
        cb->append(*rl);
        auto rh = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(0, 0, 2000, 1, 12), 1, 1);
        views::allow_math(*rh);   // s349: math in the field
        rh->set_name("table.row_h");
        rh->set_tooltip_text("The picked cell's row at least this tall, points (0: as its words need)");
        cb->append(*rh);
        auto cl = Gtk::make_managed<Gtk::Label>("Cell fill");   // s325: the picked cell's own
        cl->set_margin_start(12);
        cb->append(*cl);
        auto cf = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(sw_names));
        cf->set_name("table.cell_fill");
        cf->set_tooltip_text("The picked cell's own fill (None: the header's or the rows')");
        cb->append(*cf);
        auto ct = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(100, 5, 100, 5, 10), 5, 0);
        views::allow_math(*ct);   // s349: math in the field
        ct->set_name("table.cell_tint");
        ct->set_tooltip_text("The cell fill's tint, percent");
        cb->append(*ct);
        auto syncing = std::make_shared<bool>(false);
        cf->property_selected().signal_changed().connect([work, cur, cf, ct, sw_ids, syncing] {
            if (*syncing) return;
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            const auto k = cf->get_selected();
            if (o.first < 0 || k >= sw_ids.size()) return;
            auto& x = work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)];
            x.fill = sw_ids[k];
            x.fill_tint = ct->get_value() / 100.0;
        });
        rh->signal_value_changed().connect([work, cur, rh, syncing] {   // s331
            if (*syncing) return;
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first >= 0) core::doc::table_set_row_height(*work, o.first, rh->get_value());
        });
        ct->signal_value_changed().connect([work, cur, ct, syncing] {
            if (*syncing) return;
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first >= 0) work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)].fill_tint = ct->get_value() / 100.0;
        });
        *picked = [work, cur, va, cf, ct, rh, syncing, sw_index] {   // s325: the rows show the picked cell's settings
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first < 0) return;
            const auto& x = work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)];
            *syncing = true;
            va->set_selected(static_cast<guint>(std::clamp(x.valign, 0, 2)));
            cf->set_selected(sw_index(x.fill));
            ct->set_value(x.fill.empty() ? 100 : x.fill_tint * 100);
            rh->set_value(work->rows[static_cast<std::size_t>(o.first)].min_h);
            *syncing = false;
        };
        (*picked)();
        auto al = Gtk::make_managed<Gtk::Label>("Table");
        al->set_margin_start(12);
        cb->append(*al);
        auto ta = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Left", "Centre", "Right"}));
        ta->set_name("table.align");
        ta->set_selected(static_cast<guint>(std::clamp(work->align, 0, 2)));
        ta->property_selected().signal_changed().connect([work, ta] { work->align = static_cast<int>(ta->get_selected()); });
        cb->append(*ta);
        outer->append(*cb);
    }
    {   // s334: the picked cell's own borders -- which sides, how heavy, what colour; Grid puts them back
        auto bb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto bl = Gtk::make_managed<Gtk::Label>("Cell border");
        bb->append(*bl);
        static const int masks[] = {15, 1, 4, 8, 2, 5, 10};
        auto sides = Gtk::make_managed<Gtk::DropDown>(
            Gtk::StringList::create({"All sides", "Top", "Bottom", "Left", "Right", "Top & bottom", "Left & right"}));
        sides->set_name("table.border_sides");
        sides->set_tooltip_text("Which of the picked cell's sides (an edge it shares is one line: the cell across it takes the same)");
        bb->append(*sides);
        auto bw = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->rule_w, 0, 12, 0.25, 1), 0.25, 2);
        views::allow_math(*bw);   // s349: math in the field
        bw->set_name("table.border_w");
        bw->set_tooltip_text("The border's weight, points (0: none on those sides)");
        bb->append(*bw);
        auto pl = Gtk::make_managed<Gtk::Label>("pt");
        bb->append(*pl);
        std::vector<Glib::ustring> bnames = sw_names;
        bnames[0] = "The grid's colour";
        auto bc = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(bnames));
        bc->set_name("table.border_color");
        bc->set_tooltip_text("The border's colour");
        bb->append(*bc);
        auto set = Gtk::make_managed<Gtk::Button>("Set");
        set->set_name("table.border_set");
        set->set_tooltip_text("The picked cell's chosen sides take this weight and colour");
        bb->append(*set);
        auto grid_b = Gtk::make_managed<Gtk::Button>("As the grid");
        grid_b->set_name("table.border_grid");
        grid_b->set_tooltip_text("The picked cell's chosen sides back to the table's grid");
        bb->append(*grid_b);
        auto state = Gtk::make_managed<Gtk::Label>("");
        state->set_name("table.border_state");
        state->add_css_class("dim-label");
        state->set_margin_start(6);
        bb->append(*state);
        const auto mask = [sides] { return masks[std::min<guint>(sides->get_selected(), 6)]; };
        // what the picked cell's first chosen side has now
        const auto show = std::make_shared<std::function<void()>>();
        *show = [work, cur, mask, bw, bc, state, sw_index] {
            const auto o = core::doc::owner_of(*work, cur->first, cur->second);
            if (o.first < 0) return;
            const auto& x = work->rows[static_cast<std::size_t>(o.first)].cells[static_cast<std::size_t>(o.second)];
            int side = 0;
            while (side < 3 && !(mask() & (1 << side))) ++side;
            const auto& b = x.borders[static_cast<std::size_t>(side)];
            if (b.w >= 0) {
                bw->set_value(b.w);
                bc->set_selected(sw_index(b.color));
                state->set_text("its own");
            } else {
                bw->set_value(work->rule_w);
                bc->set_selected(0);
                state->set_text("as the grid");
            }
        };
        sides->property_selected().signal_changed().connect([show] { (*show)(); });
        set->signal_clicked().connect([keep, fill, work, cur, mask, bw, bc, sw_ids] {
            keep();
            core::CellBorder b;
            b.w = bw->get_value();
            const auto k = bc->get_selected();
            b.color = k < sw_ids.size() ? sw_ids[k] : std::string{};
            core::doc::table_set_border(*work, cur->first, cur->second, mask(), b);
            (*fill)();
        });
        grid_b->signal_clicked().connect([keep, fill, work, cur, mask] {
            keep();
            core::doc::table_set_border(*work, cur->first, cur->second, mask(), core::CellBorder{});
            (*fill)();
        });
        const auto prev = *picked;
        *picked = [prev, show] { prev(); (*show)(); };
        (*show)();
        outer->append(*bb);
    }
    auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
    sc->set_vexpand(true);
    sc->set_child(*grid);
    outer->append(*sc);
    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_halign(Gtk::Align::END);
    auto cancel = Gtk::make_managed<Gtk::Button>("Close");
    auto apply = Gtk::make_managed<Gtk::Button>("Apply");
    apply->set_name("table.apply");
    apply->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*apply);
    outer->append(*buttons);
    cancel->signal_clicked().connect([this] { m_table_win->set_visible(false); });
    apply->signal_clicked().connect([this, keep, work, story, para] {
        keep();
        const bool did = core::doc::edit_table(m_journal, m_doc, story, para, "Edit Table", [&](core::Table& t) { t = *work; });
        if (auto lg = log::get(log::Area::Shell)) lg->info("table: {} para {} {}x{} -> {}", story, para, work->rows.size(), work->widths.size(), did ? "step" : "no change");
        m_canvas_view.canvas().model_changed();
        m_table_win->set_visible(false);
    });
    (*fill)();
    m_table_win->set_child(*outer);
    m_table_win->present();
    if (focus_row >= 0 && focus_col >= 0 && focus_row < static_cast<int>(entries->size()))   // s323: the cell double-clicked
        if (auto* e = (*entries)[static_cast<std::size_t>(focus_row)][static_cast<std::size_t>(std::max(0, focus_col))]) e->grab_focus();
}

// s333: formatting on the words typed in a cell -- the selection, else the
// word at the caret (as on a story).
void Shell::cell_local(const std::string& label, const std::function<bool(const core::Story&, core::TextPos, core::TextPos)>& all_on,
                       const std::function<void(core::Local&, bool)>& set) {
    auto& cv = m_canvas_view.canvas();
    if (!cv.cell_editing()) return;
    const auto w = cv.typing_cell_words();
    auto [a, b] = cv.cell_range();
    if (a == b) std::tie(a, b) = core::word_at(w, a);
    if (a == b) return;
    const bool on = !all_on(w, a, b);
    const auto& c = cv.typing_cell();
    core::doc::set_cell_local(m_journal, m_doc, cv.cell_story(), c.para, c.row, c.col, a, b, label, [&](core::Local& l) { set(l, on); });
    cv.grab_focus();
}

// s337: the Cell group's rows.
bool Shell::cell_edit(const std::string& key, const std::string& choice) {
    auto& cv = m_canvas_view.canvas();
    if (!cv.cell_editing()) return false;
    const auto ref = cv.typing_cell();
    const auto story = cv.cell_story();
    const auto* t0 = core::doc::table_at(m_doc, story, ref.para);
    if (!t0) return false;
    const int R = static_cast<int>(t0->rows.size()), C = core::doc::table_cols(*t0);
    const auto& here = t0->rows[static_cast<std::size_t>(ref.row)].cells[static_cast<std::size_t>(ref.col)];
    const int span_c = here.span_cols, span_r = here.span_rows;
    const double grid_w = t0->rule_w;
    const auto num = [&] { try { return std::stod(choice); } catch (...) { return 0.0; } };
    const auto edit = [&](const std::string& label, const std::function<void(core::Table&)>& f) {
        return core::doc::edit_table(m_journal, m_doc, story, ref.para, label, f);
    };
    const auto cell = [&](core::Table& t) -> core::TableCell& { return t.rows[static_cast<std::size_t>(ref.row)].cells[static_cast<std::size_t>(ref.col)]; };
    // where the typing goes after rows / columns move: (row, col), or {-1, -1} out
    std::pair<int, int> go{ref.row, ref.col};
    bool did = false;
    if (key == "cell_fill")
        did = edit("Cell fill", [&](core::Table& t) { cell(t).fill = choice; });
    else if (key == "cell_tint")
        did = edit("Cell fill tint", [&](core::Table& t) { cell(t).fill_tint = std::clamp(num() / 100, 0.05, 1.0); });
    else if (key == "cell_valign")
        did = edit("Cell words", [&](core::Table& t) { cell(t).valign = std::clamp(static_cast<int>(num()), 0, 2); });
    else if (key == "cell_rowh")
        did = edit("Row height", [&](core::Table& t) { core::doc::table_set_row_height(t, ref.row + span_r - 1, num()); });
    else if (key == "cell_border") {
        core::CellBorder b;
        b.w = num();
        const auto& b0 = here.borders[0];
        if (b.w >= 0 && b0.w >= 0) { b.color = b0.color; b.tint = b0.tint; }   // keep its colour
        did = edit("Cell border", [&](core::Table& t) { core::doc::table_set_border(t, ref.row, ref.col, 15, b); });
    } else if (key == "cell_border_color") {
        did = edit("Cell border colour", [&](core::Table& t) {
            for (int side = 0; side < 4; ++side) {   // each side keeps its weight (the grid's where it had none of its own)
                core::CellBorder b = cell(t).borders[static_cast<std::size_t>(side)];
                if (b.w < 0) b.w = grid_w;
                b.color = choice;
                core::doc::table_set_border(t, ref.row, ref.col, 1 << side, b);
            }
        });
    } else if (key == "cell_tstyle") {   // s343
        did = edit("Table Style", [&](core::Table& t) { core::doc::apply_table_style(m_doc, t, choice); });
    } else if (key == "cell_cstyle") {
        did = edit("Cell Style", [&](core::Table& t) {
            if (choice.empty()) cell(t).style.clear();
            else core::doc::apply_cell_style(m_doc, cell(t), choice);
        });
    } else if (key == "cell_newstyle") {   // a style from this table / cell, the table / cell then using it
        const core::Table tnow = *t0;   // copies: adding a style changes the document
        const core::TableCell cnow = here;
        if (choice == "table") {
            const auto id = core::doc::add_table_style(m_journal, m_doc, "Table Style", &tnow);
            did = !id.empty() && edit("Table Style", [&](core::Table& t) { t.style = id; });
        } else {
            const auto id = core::doc::add_cell_style(m_journal, m_doc, "Cell Style", &cnow);
            did = !id.empty() && edit("Cell Style", [&](core::Table& t) { cell(t).style = id; });
        }
    } else if (key == "cell_restyle") {   // s347
        const core::Table tnow = *t0;
        const core::TableCell cnow = here;
        if (choice == "redefine_table" && !tnow.style.empty()) did = core::doc::redefine_table_style(m_journal, m_doc, tnow.style, tnow);
        else if (choice == "redefine_cell" && !cnow.style.empty()) did = core::doc::redefine_cell_style(m_journal, m_doc, cnow.style, cnow);
        else if (choice == "clear")   // the table back to its style (its cells to theirs), else just this cell
            did = edit("Clear Overrides", [&](core::Table& t) {
                if (!t.style.empty()) core::doc::clear_overrides(m_doc, t);
                else core::doc::clear_overrides(m_doc, t, ref.row, ref.col);
            });
    } else if (key == "cell_rows") {
        if (choice == "above") { did = edit("Insert row", [&](core::Table& t) { core::doc::table_insert_row(t, ref.row); }); go.first += 1; }
        else if (choice == "below") did = edit("Insert row", [&](core::Table& t) { core::doc::table_insert_row(t, ref.row + span_r); });
        else if (choice == "delete" && R > 1) { did = edit("Delete row", [&](core::Table& t) { core::doc::table_delete_row(t, ref.row); }); go = {std::min(ref.row, R - 2), ref.col}; }
    } else if (key == "cell_cols") {
        if (choice == "left") { did = edit("Insert column", [&](core::Table& t) { core::doc::table_insert_col(t, ref.col); }); go.second += 1; }
        else if (choice == "right") did = edit("Insert column", [&](core::Table& t) { core::doc::table_insert_col(t, ref.col + span_c); });
        else if (choice == "delete" && C > 1) { did = edit("Delete column", [&](core::Table& t) { core::doc::table_delete_col(t, ref.col); }); go = {ref.row, std::min(ref.col, C - 2)}; }
    } else if (key == "cell_merge") {
        if (choice == "across" && ref.col + span_c < C) did = edit("Merge cells", [&](core::Table& t) { core::doc::table_merge(t, ref.row, ref.col, span_c + 1, span_r); });
        else if (choice == "down" && ref.row + span_r < R) did = edit("Merge cells", [&](core::Table& t) { core::doc::table_merge(t, ref.row, ref.col, span_c, span_r + 1); });
        else if (choice == "unmerge") did = edit("Unmerge cells", [&](core::Table& t) { core::doc::table_unmerge(t, ref.row, ref.col); });
    }
    // typing stays in the cell (moved, or the one now there) -- at its start when it moved
    const auto still = [&] {
        const auto* t = core::doc::table_at(m_doc, story, ref.para);
        return t && core::doc::owner_of(*t, ref.row, ref.col) == std::pair<int, int>{ref.row, ref.col};
    };
    if (did && (go.first != ref.row || go.second != ref.col || !still())) {
        cv.end_cell();
        if (const auto* t = core::doc::table_at(m_doc, story, ref.para)) {
            const auto o = core::doc::owner_of(*t, go.first, go.second);
            if (o.first >= 0) {
                const layout::CellRef to{ref.para, o.first, o.second};
                const std::string fr = layout::cell_frame(layout::flow_story(m_doc, story), to);
                if (!fr.empty()) cv.select_cell_text(fr, story, to, {0, 0}, {0, 0});
            }
        }
    }
    cv.grab_focus();
    return did;
}

// s343: table and cell styles -- their rows in the inspector, New.
bool Shell::table_style_edit(const std::string& key, const std::string& choice) {
    namespace D = core::doc;
    const auto num = [&] { try { return std::stod(choice); } catch (...) { return 0.0; } };
    const auto show = [this](const std::string& id) {
        Glib::signal_idle().connect_once([this, id] {
            m_tiles.show_section(m_outline, core::Segment::Styles, {id});
            on_tiles_selection({id});
        });
    };
    if (key == "tstyles_new") {
        const auto id = choice == "cell" ? D::add_cell_style(m_journal, m_doc, "Cell Style") : D::add_table_style(m_journal, m_doc, "Table Style");
        if (!id.empty()) show(id);
        return !id.empty();
    }
    bool did = false;
    for (const auto& id : m_inspected) {
        if (key.rfind("ts_", 0) == 0 && m_doc.as<core::TableStyle>(id)) {
            if (key == "ts_act") {
                if (choice == "delete") did = D::delete_table_style(m_journal, m_doc, id) || did;
                else if (choice == "duplicate") {
                    const auto src = *m_doc.as<core::TableStyle>(id);
                    const auto made = D::add_table_style(m_journal, m_doc, src.name + " copy");
                    D::edit_table_style(m_journal, m_doc, made, "Duplicate Table Style", [&](core::TableStyle& t) {
                        const auto nm = t.name;
                        t = src;
                        t.name = nm;
                    });
                    show(made);
                    did = true;
                }
                continue;
            }
            const std::string k = key.substr(3);
            did = D::edit_table_style(m_journal, m_doc, id, "Table Style", [&](core::TableStyle& t) {
                if (k == "name") { if (!choice.empty()) t.name = choice; }
                else if (k == "rule_w") t.rule_w = std::clamp(num(), 0.0, 72.0);
                else if (k == "rule_color") t.rule_color = choice;
                else if (k == "inset") t.inset = std::clamp(num(), 0.0, 500.0);
                else if (k == "header_fill") t.header_fill = choice;
                else if (k == "header_tint") t.header_tint = std::clamp(num() / 100, 0.01, 1.0);
                else if (k == "band_fill") t.band_fill = choice;
                else if (k == "band_tint") t.band_tint = std::clamp(num() / 100, 0.01, 1.0);
                else if (k == "band_every") t.band_every = std::clamp(static_cast<int>(num()), 1, 9);
                else if (k == "header_cell") t.header_cell = choice;
                else if (k == "body_cell") t.body_cell = choice;
                else if (k == "footer_cell") t.footer_cell = choice;
                else if (k == "left_cell") t.left_cell = choice;     // s346
                else if (k == "right_cell") t.right_cell = choice;
            }) || did;
        } else if (key.rfind("cs_", 0) == 0 && m_doc.as<core::CellStyle>(id)) {
            if (key == "cs_act") {
                if (choice == "delete") did = D::delete_cell_style(m_journal, m_doc, id) || did;
                else if (choice == "duplicate") {
                    const auto src = *m_doc.as<core::CellStyle>(id);
                    const auto made = D::add_cell_style(m_journal, m_doc, src.name + " copy");
                    D::edit_cell_style(m_journal, m_doc, made, "Duplicate Cell Style", [&](core::CellStyle& c) {
                        const auto nm = c.name;
                        c = src;
                        c.name = nm;
                    });
                    show(made);
                    did = true;
                }
                continue;
            }
            const std::string k = key.substr(3);
            did = D::edit_cell_style(m_journal, m_doc, id, "Cell Style", [&](core::CellStyle& c) {
                if (k == "name") { if (!choice.empty()) c.name = choice; }
                else if (k == "fill") { if (choice == "-") c.fill.reset(); else c.fill = choice; }
                else if (k == "tint") c.fill_tint = std::clamp(num() / 100, 0.01, 1.0);
                else if (k == "valign") { const int v = static_cast<int>(num()); if (v < 0) c.valign.reset(); else c.valign = std::clamp(v, 0, 2); }
                else if (k == "border") {
                    const double w = num();
                    for (auto& b : c.borders) {
                        if (w < 0) { b = core::CellBorder{}; continue; }
                        b.w = std::clamp(std::round(w * 4) / 4, 0.0, 72.0);
                    }
                } else if (k == "border_color") {
                    for (auto& b : c.borders) if (b.w >= 0) b.color = choice;
                } else if (k == "para") c.para_style = choice;
            }) || did;
        }
    }
    if (did) m_canvas_view.canvas().model_changed();
    return did;
}

}  // namespace composix
