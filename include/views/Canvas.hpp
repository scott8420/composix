#pragma once
#include "views/BackButton.hpp"
#include "core/PlaceQueue.hpp"
// views::Canvas -- the page canvas (s005).
//
// The opened pages (or masters) at true size, left to right on a pasteboard:
// paper, bleed, folds, master items under the page's own, frames from the
// model. The Select tool: click to pick (topmost first), Shift adds, a drag on
// empty space is a marquee (touching selects); 8 handles resize; a drag moves.
// Every drag is ONE journal gesture (core::doc::frames_begin .. _end), Escape
// cancels it, arrows nudge (merged into one step). Space- or middle-drag pans,
// Ctrl+scroll zooms about the pointer, scroll pans.
//
// The one view that reads the Doc directly -- it needs geometry, not the
// Outline's summaries -- and it writes ONLY through core::doc verbs.
// Ideas from Curvz (viewport, handles, gesture shape), code rewritten: ARC s005.
#include "core/Doc.hpp"
#include "layout/Flow.hpp"
#include "widgets/Widgets.hpp"

#include <gdkmm/rgba.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <sigc++/signal.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace composix::views {

class Canvas : public widgets::DrawingArea {
public:
    // s008: what a drag on empty space does.
    enum class Tool { Select, Text, Image, Shape };
    Canvas(core::Doc& d, core::DocJournal& j);
    ~Canvas() override;

    // Show these pages / masters. A different set re-fits; the same set (a
    // refresh) keeps the zoom, the scroll and the selection.
    void show_pages(const std::vector<std::string>& ids);
    const std::vector<std::string>& shown() const;   // s027: the pages / masters on show
    void model_changed();                       // re-layout, drop vanished picks, repaint

    const std::vector<std::string>& selection() const { return m_sel; }
    void set_selection(const std::vector<std::string>& ids);   // no signal (undo / redo)
    bool dragging() const { return m_drag != Drag::None; }

    void   zoom_fit();
    void   zoom_to(double percent);
    void   zoom_step(int dir);                  // +1 in, -1 out (about the centre)
    double zoom_percent() const { return m_vp.percent(); }

    // s006: rulers, guides, snapping.
    const core::Viewport& viewport() const { return m_vp; }
    void pointer(double& x, double& y) const { x = m_in ? m_mx : -1; y = m_in ? m_my : -1; }
    void preview_guide(bool vertical, double at);          // a guide being pulled from a ruler (screen)
    void drop_guide(bool vertical, double x, double y);     // released: on a page -> one step
    void set_snap(bool on) { m_snap = on; }
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    void delete_selection();
    void set_show_guides(bool on) { m_show_guides = on; queue_draw(); }
    void set_show_grid(bool on) { m_show_grid = on; queue_draw(); }   // s036: baseline grid
    bool show_grid() const { return m_show_grid; }
    void set_show_hidden(bool on) { m_show_hidden = on; queue_draw(); }   // s059: hidden characters
    bool show_hidden() const { return m_show_hidden; }

    // s009: images. The project folder resolves relative / embedded files.
    void set_project_dir(const std::string& dir) { m_dir = dir; queue_draw(); }
    // "Loaded" with a file (Place): a click on a page places it at its own
    // size, a drag draws its frame, a click on an image frame fills that.
    // Esc puts it down. A file dropped from the desktop places the same way.
    void load_image(const core::ImageFile& f);
    void load_images(std::vector<core::ImageFile> files);   // s063: several, one click each
    bool loaded() const { return !m_load.empty(); }

    // s010: editing text in place. Double-click a text frame (or click one
    // with the Text tool; a new text frame starts editing): a caret, typing
    // through the IM, Backspace / Delete / Enter, arrows (Shift extends, Ctrl
    // by word), Home / End, Ctrl+A, cut / copy / paste, drag to select,
    // double-click a word. Esc ends editing with the frame picked. Every
    // change is core::doc::edit_text -- a burst of typing is one step.
    bool editing() const { return !m_edit_story.empty(); }
    void end_edit();
    // s022: edit `frame` with a..b selected (a Find hit). `focus`: take the
    // keyboard (else it stays where it is -- the find bar).
    void select_text(const std::string& frame, core::TextPos a, core::TextPos b, bool focus);
    // s023: put `text` (a marker) at the caret, replacing the selection --
    // one step. False when not editing.
    bool insert_special(const std::string& text, const std::string& label);
    bool insert_break(core::ParaStart kind);   // s055: column / frame / page break at the caret
    // s061: the caret put at `p` (an undo's place), and the view follows it.
    void place_caret(core::TextPos p);
    // s061: the caret's frame is on a page not shown -- show that page (the
    // shell turns the navigator there); the zoom stays.
    sigc::signal<void(const std::string&)>& signal_follow() { return m_sig_follow; }
    // s062: a master dropped on a page: (master, {page}).
    sigc::signal<void(const std::string&, const std::vector<std::string>&)>& signal_master_drop() { return m_sig_master_drop; }
    const std::string& edit_story_id() const { return m_edit_story; }
    // The selection, in reading order (equal = just the caret).
    std::pair<core::TextPos, core::TextPos> edit_range() const {
        return {std::min(m_anchor, m_caret), std::max(m_anchor, m_caret)};
    }
    sigc::signal<void()>& signal_caret() { return m_sig_caret; }   // s011: caret / selection / editing changed

    sigc::signal<void()>& signal_selection() { return m_sig_sel; }    // the picks changed (by the user)
    sigc::signal<void()>& signal_zoom()      { return m_sig_zoom; }
    sigc::signal<void()>& signal_edited()    { return m_sig_edited; } // a drag / nudge ended: refresh the rest
    sigc::signal<void()>& signal_view()      { return m_sig_view; }   // zoom / pan / pointer moved: rulers repaint
    sigc::signal<void()>& signal_tool()      { return m_sig_tool; }   // the tool changed (a key)

private:
    enum class Drag { None, Pan, Move, Resize, Marquee, Guide, Draw, Text, Gutter, Tab, Picture };
    // s035: Alt+drag a picture inside its frame -- where it started.
    std::string m_pic_frame;
    core::Rect  m_pic0;

    // s034: the tab ruler -- a strip over the column being typed in, its
    // zero at the text's left edge; the stops of the caret's paragraph style.
    struct TabStrip {
        bool        ok = false;
        core::Rect  screen;        // the strip, screen coordinates
        double      x0 = 0;        // screen x of the text's left edge (tab 0)
        double      scale = 1;
        double      width = 0;     // points: the column less the indents
        std::string style;         // the paragraph style whose tabs these are
    };
    TabStrip tab_strip() const;
    void draw_tab_strip(const Cairo::RefPtr<Cairo::Context>& cr);
    int  tab_hit(const TabStrip& t, double x) const;   // the stop under screen x, -1 none
    int    m_tab = -1;        // the stop being dragged (-1: a new one)
    double m_tab_x = 0;       // where it is now (screen x)
    bool   m_tab_off = false; // dragged off the strip: let go removes it

    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);
    void draw_frame(const Cairo::RefPtr<Cairo::Context>& cr, const core::Frame& f, double ox, double oy,
                    bool from_master, const std::string& id, const std::string& on_page = {});
    void draw_overlays(const Cairo::RefPtr<Cairo::Context>& cr);
    void draw_guides(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);
    void draw_grid(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);   // s036
    void draw_threads(const Cairo::RefPtr<Cairo::Context>& cr);
    std::string out_port_hit(double sx, double sy) const;   // a text frame's out-port under the pointer
    core::Rect port_rect(const std::string& frame, bool out) const;   // screen

    void read_mods();
    void on_drag_begin(double x, double y);
    void on_drag_update(double dx, double dy);
    void on_drag_end(double dx, double dy);
    bool on_key(guint keyval, Gdk::ModifierType state);
    void on_key_up(guint keyval);
    bool on_scroll(double dx, double dy);
    void on_motion(double x, double y);

    void relayout();
    core::Rect selection_box() const;           // union of the picks, layout (doc) space
    void pick(const std::vector<std::string>& ids, bool notify = true);
    void cancel_drag();
    void update_cursor();
    void place_loaded(double lx, double ly, const core::Rect* drawn);   // s009: layout coords
    bool on_drop(const Glib::ValueBase& v, double x, double y);
    // s010: editing
    void begin_edit(const std::string& frame, core::TextPos at);
    bool edit_key(guint kv, Gdk::ModifierType st);
    // kind: "type" / "erase" merge into a burst; "" is a step of its own.
    void edit_insert(const std::string& text, const std::string& kind, const std::string& label);
    void set_caret(core::TextPos p, bool extend, bool keep_goal = false);
    core::TextPos hit_story(const std::string& frame, double lx, double ly);   // layout coords
    void draw_edit(const Cairo::RefPtr<Cairo::Context>& cr);
    void blink_restart();
    // s012: a picked text frame's gutter under the pointer (screen): the
    // column to its left, or -1.
    int gutter_hit(double sx, double sy) const;
    const core::Story* edit_story() const { return m_doc.as<core::Story>(m_edit_story); }

    core::Doc&        m_doc;
    core::DocJournal& m_journal;

    std::vector<std::string>  m_ids;
    std::vector<core::Placed> m_placed;
    core::Viewport            m_vp;
    bool                      m_fit_pending = true;

    std::vector<std::string> m_sel;
    std::string              m_hover;

    // The drag in progress: everything is recomputed from the PRESS snapshot.
    Drag        m_drag = Drag::None;
    bool        m_begun = false;               // the journal gesture is open
    double      m_px = 0, m_py = 0;            // press, screen
    double      m_ox0 = 0, m_oy0 = 0;          // viewport origin at press (pan)
    core::Handle m_handle = core::Handle::None;
    core::Rect  m_box0;                        // the selection box at press (layout)
    std::map<std::string, core::Rect> m_own0;  // each pick, owner coords, at press
    std::map<std::string, core::Rect> m_lay0;  // each pick, layout coords, at press
    std::vector<std::string> m_sel0;           // the picks at press (Shift-marquee adds)
    core::Rect  m_marquee;                     // screen
    bool        m_space = false;
    bool        m_mods_shift = false, m_mods_alt = false;   // read from the drag's events
    bool        m_mods_ctrl = false;                       // s025: Ctrl+Shift+click overrides a master item
    Glib::RefPtr<Gtk::GestureDrag> m_gdrag;
    Glib::RefPtr<Gtk::EventControllerScroll> m_gscroll;
    double      m_mx = 0, m_my = 0;            // the pointer, screen

    // s006
    bool              m_in = false;                // the pointer is over the canvas
    bool              m_snap = true, m_show_guides = true, m_show_grid = false;
    bool              m_show_hidden = false;   // s059
    bool              m_follow_keep_zoom = false;   // s061: a follow shows another spread without fitting
    sigc::signal<void(const std::string&)> m_sig_follow;
    sigc::signal<void(const std::string&, const std::vector<std::string>&)> m_sig_master_drop;
    void follow_caret();                        // s061
    core::SnapTargets m_targets;                   // gathered at press
    double            m_snap_x = NAN, m_snap_y = NAN;   // the lines snapped to (layout), drawn mid-drag
    std::string       m_guide;                     // the guide being dragged
    double            m_guide0 = 0;                // its position at press
    bool              m_guide_off = false;         // dragged off its page: it will go
    std::string       m_hover_guide;
    bool              m_preview = false, m_preview_v = false;
    double            m_preview_at = 0;

    layout::FlowCache m_flows;   // s007: text, recomputed when the document changes

    // s008
    Tool        m_tool = Tool::Select;
    std::string m_draw_page;                 // the page a frame is being drawn on
    double      m_dx0 = 0, m_dy0 = 0;        // its first corner (layout)
    core::Rect  m_draw;                      // the frame being drawn (layout)
    std::string m_thread_from;               // "loaded": the next click threads into a frame

    // s009
    std::string                    m_dir;      // the project folder ("" = unsaved)
    core::PlaceQueue  m_load;   // Place (s063: several): the files waiting for a click each

    // s010: editing
    std::string   m_edit_story, m_edit_frame;   // editing when the story is set
    core::TextPos m_caret, m_anchor;            // the selection is [anchor, caret)
    double        m_goal_x = NAN;               // up / down keep to this x (frame coords)
    bool          m_caret_on = true;
    sigc::connection m_blink;
    int           m_burst = 0;                  // a new number = a new undo step
    std::string   m_burst_kind;
    GtkIMContext*                         m_im = nullptr;   // C API: gtkmm 4.10 has no wrapper
    Glib::RefPtr<Gtk::EventControllerKey> m_keys;
    gint64        m_last_press = 0;             // double-click, by hand (the drag gesture eats the click)
    double        m_last_px = 0, m_last_py = 0;
    int           m_clicks = 0;

    // s012: dragging a gutter changes the widths of the columns either side
    int                 m_gutter = -1;          // the column left of the gutter being dragged
    int                 m_hover_gutter = -1;
    std::vector<double> m_col0;                 // every column's width (points) at press
    sigc::signal<void()> m_sig_sel, m_sig_zoom, m_sig_edited, m_sig_view, m_sig_tool, m_sig_caret;
};

class Ruler;

// The canvas with its head (the title, a count, the zoom) and its rulers.
class CanvasView : public widgets::Box {
public:
    CanvasView(core::Doc& d, core::DocJournal& j);
    ~CanvasView() override;
    Canvas& canvas() { return m_canvas; }
    void set_rulers(bool on);
    void set_title(const std::string& t, std::size_t n);
    // s068: where back / forward before the title go ("Document").
    void set_nav(const std::string& back_to, const std::string& fwd_to) { m_nav_btns.set(back_to, fwd_to); }

private:
    void sync_zoom();
    widgets::Box    m_head;
    NavButtons    m_nav_btns;   // s068
    widgets::Label  m_title;
    widgets::Label  m_count;
    widgets::Box    m_tools;    // s008: Select / Text / Image / Shape
    std::vector<Gtk::ToggleButton*> m_tool_buttons;
    widgets::Button m_fit;
    widgets::Label  m_zoom;
    widgets::Grid   m_grid;     // [corner][top ruler] / [left ruler][canvas]
    Canvas          m_canvas;
    std::unique_ptr<Ruler> m_top, m_left;
};

}  // namespace composix::views
