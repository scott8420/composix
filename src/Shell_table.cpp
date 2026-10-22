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
    auto cols = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(3, 1, 50, 1, 2), 1, 0);
    auto heads = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(1, 0, 20, 1, 1), 1, 0);
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
        bt->set_name("table.band_tint");
        bt->set_tooltip_text("The bands' tint, percent");
        fb->append(*bt);
        auto el = Gtk::make_managed<Gtk::Label>("% every");
        fb->append(*el);
        auto be = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->band_every, 1, 9, 1, 1), 1, 0);
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

}  // namespace composix
