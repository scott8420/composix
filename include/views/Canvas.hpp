#pragma once
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

    // Show these pages / masters. A different set re-fits; the same set (a
    // refresh) keeps the zoom, the scroll and the selection.
    void show_pages(const std::vector<std::string>& ids);
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

    // s009: images. The project folder resolves relative / embedded files.
    void set_project_dir(const std::string& dir) { m_dir = dir; queue_draw(); }
    // "Loaded" with a file (Place): a click on a page places it at its own
    // size, a drag draws its frame, a click on an image frame fills that.
    // Esc puts it down. A file dropped from the desktop places the same way.
    void load_image(const core::ImageFile& f);
    bool loaded() const { return m_loaded.has_value(); }

    sigc::signal<void()>& signal_selection() { return m_sig_sel; }    // the picks changed (by the user)
    sigc::signal<void()>& signal_zoom()      { return m_sig_zoom; }
    sigc::signal<void()>& signal_edited()    { return m_sig_edited; } // a drag / nudge ended: refresh the rest
    sigc::signal<void()>& signal_view()      { return m_sig_view; }   // zoom / pan / pointer moved: rulers repaint
    sigc::signal<void()>& signal_tool()      { return m_sig_tool; }   // the tool changed (a key)

private:
    enum class Drag { None, Pan, Move, Resize, Marquee, Guide, Draw };

    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);
    void draw_frame(const Cairo::RefPtr<Cairo::Context>& cr, const core::Frame& f, double ox, double oy,
                    bool from_master, const std::string& id);
    void draw_overlays(const Cairo::RefPtr<Cairo::Context>& cr);
    void draw_guides(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);
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
    Glib::RefPtr<Gtk::GestureDrag> m_gdrag;
    Glib::RefPtr<Gtk::EventControllerScroll> m_gscroll;
    double      m_mx = 0, m_my = 0;            // the pointer, screen

    // s006
    bool              m_in = false;                // the pointer is over the canvas
    bool              m_snap = true, m_show_guides = true;
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
    std::optional<core::ImageFile> m_loaded;   // Place: the file waiting for a click
    sigc::signal<void()> m_sig_sel, m_sig_zoom, m_sig_edited, m_sig_view, m_sig_tool;
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

private:
    void sync_zoom();
    widgets::Box    m_head;
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
