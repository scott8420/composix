#include "Shell.hpp"

#include "Log.hpp"
#include "layout/Flow.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
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

void Shell::on_edit_table() {  // handler
    auto& cv = m_canvas_view.canvas();
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

void Shell::open_table_editor(const std::string& story, int para) {  // helper
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
                core::doc::set_cell_words(work->rows[r].cells[c], typed((*entries)[r][c]->get_text()));
    };
    const auto fill = std::make_shared<std::function<void()>>();
    *fill = [work, entries, grid] {
        while (auto* ch = grid->get_first_child()) grid->remove(*ch);
        entries->clear();
        for (std::size_t r = 0; r < work->rows.size(); ++r) {
            auto lab = Gtk::make_managed<Gtk::Label>(static_cast<int>(r) < work->header_rows ? "H" : std::to_string(r + 1 - work->header_rows));
            lab->add_css_class("dim-label");
            lab->set_size_request(28, -1);
            grid->attach(*lab, 0, static_cast<int>(r));
            entries->emplace_back();
            for (std::size_t c = 0; c < work->widths.size(); ++c) {
                auto e = Gtk::make_managed<Gtk::Entry>();
                e->set_name("table.cell." + std::to_string(r) + "." + std::to_string(c));
                e->set_hexpand(true);
                e->set_width_chars(10);
                if (c < work->rows[r].cells.size()) e->set_text(shown(work->rows[r].cells[c]));
                if (static_cast<int>(r) < work->header_rows) e->add_css_class("cx-table-head");
                grid->attach(*e, static_cast<int>(c) + 1, static_cast<int>(r));
                entries->back().push_back(e);
            }
        }
    };
    const auto act = [&](const char* label, const char* name, std::function<void()> f) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->set_name(std::string("table.") + name);
        b->signal_clicked().connect([keep, fill, f] { keep(); f(); (*fill)(); });
        bar->append(*b);
    };
    act("+ Row", "add_row", [work] { core::doc::table_insert_row(*work, static_cast<int>(work->rows.size())); });
    act("− Row", "del_row", [work] { core::doc::table_delete_row(*work, static_cast<int>(work->rows.size()) - 1); });
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
    auto gl = Gtk::make_managed<Gtk::Label>("Grid");
    gl->set_margin_start(12);
    bar->append(*gl);
    auto weight = Gtk::make_managed<Gtk::SpinButton>(Gtk::Adjustment::create(work->rule_w, 0, 6, 0.25, 1), 0.25, 2);
    weight->set_name("table.rule_w");
    weight->set_tooltip_text("The grid's lines, points (0: none)");
    weight->signal_value_changed().connect([work, weight] { work->rule_w = weight->get_value(); });
    bar->append(*weight);
    outer->append(*bar);
    {   // the header's fill
        auto fb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto fl = Gtk::make_managed<Gtk::Label>("Header fill");
        fb->append(*fl);
        std::vector<Glib::ustring> names{"None"};
        std::vector<std::string> ids{""};
        guint now = 0;
        for (const auto& sw : m_doc.ids_of<core::Swatch>()) {
            names.push_back(m_doc.as<core::Swatch>(sw)->name);
            ids.push_back(sw);
            if (sw == work->header_fill) now = static_cast<guint>(ids.size() - 1);
        }
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
        dd->set_name("table.header_fill");
        dd->set_selected(now);
        dd->property_selected().signal_changed().connect([work, dd, ids] {
            const auto k = dd->get_selected();
            if (k < ids.size()) work->header_fill = ids[k];
        });
        fb->append(*dd);
        auto tip = Gtk::make_managed<Gtk::Label>("A cell's paragraphs: “a | b”");
        tip->add_css_class("dim-label");
        tip->set_margin_start(12);
        fb->append(*tip);
        outer->append(*fb);
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
}

}  // namespace composix
