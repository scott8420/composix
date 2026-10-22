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
#include "layout/Export.hpp"   // s309: TextMark
#include "layout/Flow.hpp"
#include "layout/ThumbStore.hpp"
#include <plate/plate.hpp>
#include "widgets/Widgets.hpp"

#include <gdkmm/rgba.h>
#include <glibmm/dispatcher.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/popover.h>
#include <sigc++/signal.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace composix::views {

class Canvas : public widgets::DrawingArea {
public:
    // s101: flow through this cache (the shell's, shared with the thumbnails).
    void share_flows(layout::FlowCache& f) { m_flowp = &f; }
    // s008: what a drag on empty space does.
    // s186 (G2): Direct -- a picture inside its frame (InDesign's Direct
    // Selection, A): the whole picture shown, moved and scaled by hand.
    // s198 (Scott): the Eyedropper as a tool (I) -- a click takes the colour
    // on the page onto the picked frames' fill (Shift: stroke; Alt: just the
    // swatch). One Shape tool draws rectangles (M), ovals (L) or lines (\);
    // s199: the Pen (P) draws any other path.
    enum class Tool { Select, Direct, Text, Image, Shape, Pen, Eyedropper, Zoom };   // s241: Zoom
    enum class ShapeTool { Rect, Oval, Line, Polygon };   // s206: Polygon / star
    Canvas(core::Doc& d, core::DocJournal& j);
    const core::Doc& doc() const { return m_doc; }   // s073: the rulers read its unit
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
    const std::vector<core::Placed>& placed() const { return m_placed; }   // s154: the rulers read the pages' zeros
    // s154: the zero point dragged out of the ruler corner (canvas px):
    // crosshairs while it moves; dropped -> one step. reset_zero: back to
    // the origin (double-click the corner).
    void preview_zero(double x, double y);
    void drop_zero(double x, double y);
    void reset_zero();
    void pointer(double& x, double& y) const { x = m_in ? m_mx : -1; y = m_in ? m_my : -1; }
    void preview_guide(bool vertical, double at);          // a guide being pulled from a ruler (screen)
    void drop_guide(bool vertical, double x, double y);     // released: on a page -> one step
    void set_snap(bool on) { m_snap = on; }
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    void set_shape_tool(ShapeTool k) { m_shape = k; set_tool(Tool::Shape); }   // s198
    ShapeTool shape_tool() const { return m_shape; }
    // s206: the next polygon -- its sides (3..64) and star depth (1 = none,
    // below: the inner points that far out).
    void set_polygon(int sides, double inner) { m_poly_sides = std::clamp(sides, 3, 64); m_poly_inner = std::clamp(inner, 0.01, 1.0); }
    int polygon_sides() const { return m_poly_sides; }
    double polygon_inner() const { return m_poly_inner; }
    void delete_selection();
    void set_show_guides(bool on) { m_show_guides = on; queue_draw(); }
    // s153: guides locked -- not picked, moved or pulled off (new ones still
    // come from the rulers).
    void set_guides_locked(bool on) { m_guides_locked = on; m_hover_guide.clear(); queue_draw(); }
    bool guides_locked() const { return m_guides_locked; }
    void set_show_grid(bool on) { m_show_grid = on; queue_draw(); }   // s036: baseline grid
    bool show_grid() const { return m_show_grid; }
    void set_show_hidden(bool on) { m_show_hidden = on; queue_draw(); }   // s059: hidden characters
    // s309: misspelled words (a red wave) and links (a blue dotted line) marked in the words -- screen only.
    void set_marks(bool spelling, bool links) { m_mark_spell = spelling; m_mark_links = links; m_marks.clear(); queue_draw(); }
    void marks_changed() { m_marks.clear(); queue_draw(); }   // the dictionary / the user's words moved
    // s099: proof colours -- the pages drawn as this press profile will
    // print them (null: off).
    void set_proof(std::shared_ptr<const plate::Proof> p) { m_proof = std::move(p); ++m_proof_gen; queue_draw(); }
    bool proofing() const { return m_proof != nullptr; }
    std::size_t pages_pending() const { return m_pages.pending(); }   // s108: page images on the worker
    bool show_hidden() const { return m_show_hidden; }

    // s009: images. The project folder resolves relative / embedded files.
    void set_project_dir(const std::string& dir) { m_dir = dir; queue_draw(); }
    // "Loaded" with a file (Place): a click on a page places it at its own
    // size, a drag draws its frame, a click on an image frame fills that.
    // Esc puts it down. A file dropped from the desktop places the same way.
    void load_image(const core::ImageFile& f);
    void load_images(std::vector<core::ImageFile> files);   // s063: several, one click each
    bool loaded() const { return !m_load.empty(); }

    // s180: the eyedropper. The next click on a page takes the colour there
    // -- as it prints (layout::sample_page), three screen pixels averaged (at
    // most a point across, so type gives its ink zoomed out) --
    // and hands it to `done`; a click off the pages takes nothing and keeps
    // waiting. Esc, a right-click or another tool: `done(nullopt)`. While it
    // waits, a chip of the colour under the pointer rides beside it.
    void take_colour(std::function<void(std::optional<std::uint32_t>)> done);
    bool taking_colour() const { return static_cast<bool>(m_take); }
    void cancel_take();
    // The colour a click at (x, y) (widget pixels) would take; nullopt off the pages.
    std::optional<std::uint32_t> colour_at(double x, double y) const;

    // s010: editing text in place. Double-click a text frame (or click one
    // with the Text tool; a new text frame starts editing): a caret, typing
    // through the IM, Backspace / Delete / Enter, arrows (Shift extends, Ctrl
    // by word), Home / End, Ctrl+A, cut / copy / paste, drag to select,
    // double-click a word. Esc ends editing with the frame picked. Every
    // change is core::doc::edit_text -- a burst of typing is one step.
    bool editing() const { return !m_edit_story.empty(); }
    void end_edit();
    // s330: typing in a table's cell on the canvas -- double-click a cell;
    // Tab / Shift+Tab to the next / previous (Tab in the last adds a row),
    // the arrows, Home / End, Enter a paragraph in the cell, Esc out.
    bool cell_editing() const { return m_cell.para >= 0; }
    const layout::CellRef& typing_cell() const { return m_cell; }
    const std::string& cell_story() const { return m_cell_story; }
    // s333: the selection in the cell (reading order; equal = the caret), and
    // the cell's words as a story (one empty paragraph when empty).
    std::pair<core::TextPos, core::TextPos> cell_range() const { return {std::min(m_canchor, m_ccaret), std::max(m_canchor, m_ccaret)}; }
    core::Story typing_cell_words() const { return cell_words(); }
    // s335: type in cell `c` (in `frame`) with a .. b of its words selected
    // (a Find hit). It takes the keyboard (Find gives it back).
    void select_cell_text(const std::string& frame, const std::string& story, layout::CellRef c, core::TextPos a, core::TextPos b);
    void end_cell();
    // s022: edit `frame` with a..b selected (a Find hit). `focus`: take the
    // keyboard (else it stays where it is -- the find bar).
    void select_text(const std::string& frame, core::TextPos a, core::TextPos b, bool focus);
    // s023: put `text` (a marker) at the caret, replacing the selection --
    // one step. False when not editing.
    bool insert_special(const std::string& text, const std::string& label);
    bool insert_break(core::ParaStart kind);   // s055: column / frame / page break at the caret
    // s109: a footnote at the caret (its words come after, edit_note); false
    // when not editing. Its paragraph and index in p / k.
    bool insert_note(int& p, int& k);
    // The footnote whose mark is just before (or after) the caret.
    bool note_at_caret(int& p, int& k) const;
    const std::string& edit_story_id() const { return m_edit_story; }
    const std::string& edit_frame_id() const { return m_edit_frame; }   // s318
    core::Rect caret_screen() const;   // the caret, widget coordinates (w 0: not shown)
    // s061: the caret put at `p` (an undo's place), and the view follows it.
    void place_caret(core::TextPos p);
    // s061: the caret's frame is on a page not shown -- show that page (the
    // shell turns the navigator there); the zoom stays.
    sigc::signal<void(const std::string&)>& signal_follow() { return m_sig_follow; }
    // s062: a master dropped on a page: (master, {page}).
    sigc::signal<void(const std::string&, const std::vector<std::string>&)>& signal_master_drop() { return m_sig_master_drop; }
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
    // s074: a right-click -- what's under it (a frame -- now picked --, else a
    // master item showing on a page, else the page) and where (widget px).
    struct ContextHit {
        std::string frame, master_item, page;
        double x = 0, y = 0;
        bool zoom = false;   // s241: with the Zoom tool -- the zoom levels
        bool text = false;   // s274: typing, in the story being edited -- the text menu (the caret there)
    };
    // s241: zoom to `pct` about a point on screen (the pointer's place kept).
    void zoom_about(double pct, double sx, double sy);
    // s241: the view fitted to a rectangle (doc / layout coordinates).
    void zoom_to_rect(const core::Rect& r);
    sigc::signal<void(const ContextHit&)>& signal_context() { return m_sig_context; }
    // s323: a table double-clicked -- (story, its paragraph, the cell's row, column)
    sigc::signal<void(const std::string&, int, int, int)>& signal_table() { return m_sig_table; }
    sigc::signal<void(int, int)>& signal_note() { return m_sig_note; }   // s114: a note at the foot double-clicked -- its paragraph, index (the caret after its mark)

private:
    enum class Drag { None, Pan, Move, Resize, Marquee, Guide, Draw, Text, Gutter, Tab, Picture, PicScale, PenHandle, PathPoint, PointMarquee, PathStart, PathEnd, Rotate, Skew, Zoom, TableCol, CellText, TableRow };
    // s035: Alt+drag a picture inside its frame -- where it started.
    std::string m_pic_frame;
    core::Rect  m_pic0;
    // s186: the Direct tool -- the frame whose picture is on show, the
    // handle being dragged; its frame and picture (layout space) when it
    // has one we can show (an image frame, a picture, not turned).
    std::string  m_direct;
    core::Handle m_pic_handle = core::Handle::None;
    bool direct_pic(const std::string& frame, core::Rect& frame_l, core::Rect& pic_l) const;
    // s200: Direct on a shape -- its points shown, dragged, added, taken out.
    // A shape we can show (not turned, on a shown page); where its owner's
    // coordinates sit in the layout (dx, dy).
    bool direct_shape(const std::string& frame, double& dx, double& dy) const;
    int  m_pt = -1;                          // the point picked (-1: none) -- its handles show
    std::set<int> m_pts_sel;                 // s207: every point picked (Shift-click, a marquee); m_pt among them
    int  m_pt_part = 0;                      // dragging: 0 the anchor, 1 its handle in, 2 out
    std::vector<core::PathPoint> m_pts0;     // the points at the press (owner coordinates)
    bool m_pts_open = false;

    // s034: the tab ruler -- a strip over the column being typed in, its
    // zero at the text's left edge; the stops of the caret's paragraph style.
    struct TabStrip {
        bool        ok = false;
        core::Rect  screen;        // the strip, screen coordinates
        double      x0 = 0;        // screen x of the text's left edge (tab 0)
        double      scale = 1;
        double      width = 0;     // points: the column less the indents
        std::string style;         // the paragraph style whose tabs these are
        // s157 (Scott): the stops shown are the caret paragraph's -- its own
        // when it has them (the strip edits those), else its style's.
        std::vector<core::TabStop> tabs;
        bool        own = false;
        core::Rect  reset;         // "Reset to style" (screen; only with own stops)
    };
    TabStrip tab_strip() const;
public:   // s158: the input method's callbacks write these
    std::string m_preedit;    // what's being composed (shown at the caret)
    int         m_preedit_cur = 0;
private:
    void draw_tab_strip(const Cairo::RefPtr<Cairo::Context>& cr);
    int  tab_hit(const TabStrip& t, double x) const;   // the stop under screen x, -1 none
    int    m_tab = -1;        // the stop being dragged (-1: a new one)
    double m_tab_x = 0;       // where it is now (screen x)
    bool   m_tab_off = false; // dragged off the strip: let go removes it

    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_page(const Cairo::RefPtr<Cairo::Context>& cr, const core::Placed& p);
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
    std::string ref_tip(double x, double y) const;
    // s132: the last copy / cut -- its text and what its marks carried; a
    // paste of the same text gives them back.
    std::string           m_clip_text;
    core::doc::Carried    m_clip;   // s130: the tooltip over a cross-reference ("" none)

    void relayout();
    // s150: a facing master's panel at (x, y) (layout) handled first -- an
    // item on both sides is picked, dragged, edited where it was clicked.
    void front_panel(double x, double y);
    void order_panels();
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
    void edit_insert(const std::string& text, const std::string& kind, const std::string& label,
                     const core::doc::Carried* with = nullptr);
    void set_caret(core::TextPos p, bool extend, bool keep_goal = false);
    bool note_click(const std::string& frame, double px, double py);   // s114: a double-click on a note at the foot
    core::TextPos hit_story(const std::string& frame, double lx, double ly);   // layout coords
    void draw_edit(const Cairo::RefPtr<Cairo::Context>& cr);
    void blink_restart();
    // s012: a picked text frame's gutter under the pointer (screen): the
    // column to its left, or -1.
    int gutter_hit(double sx, double sy) const;
    // s327: a picked text frame's table -- a column's right edge under the
    // pointer (screen), what dragging it moves.
    struct TableEdge {
        std::string frame, story;
        int para = -1, col = -1;
        double ratio = 1;             // drawn width / the table's own (narrowed to fit)
        std::vector<double> widths0;  // the table's widths when the drag began
        bool ok() const { return para >= 0; }
    };
    TableEdge table_edge_hit(double sx, double sy) const;
    // s332: a picked text frame's table -- a row's bottom edge under the
    // pointer (screen), the row whose least height dragging it sets.
    struct RowEdgeHit {
        std::string frame, story;
        int para = -1, row = -1;
        double h0 = 0;                // the row's height as drawn when the drag began
        bool ok() const { return para >= 0; }
    };
    RowEdgeHit table_row_hit(double sx, double sy) const;
    const core::Story* edit_story() const { return m_doc.as<core::Story>(m_edit_story); }
    // s330: typing in a cell
    void begin_cell(const std::string& frame, const std::string& story, layout::CellRef c, core::TextPos at, bool select_all = false);
    bool cell_ok() const;                         // the cell still there (an undo may take it)
    core::Story cell_words() const;               // its paragraphs as a story (one empty when empty)
    bool cell_key(guint kv, Gdk::ModifierType st);
    void cell_insert(const std::string& text, const std::string& kind, const std::string& label);
    void cell_set_caret(core::TextPos p, bool extend, bool keep_goal = false);
    void cell_go(int r, int c, bool select_all, bool at_end);   // to another cell of the table
    void cell_follow();
    void draw_cell_edit(const Cairo::RefPtr<Cairo::Context>& cr);
    std::string     m_cell_story, m_cell_frame;
    layout::CellRef m_cell;
    core::TextPos   m_ccaret, m_canchor;
    double          m_cgoal = NAN;
    std::string     m_cburst_kind;
    int             m_cburst = 0;

    core::Doc&        m_doc;
    core::DocJournal& m_journal;

    std::vector<std::string>  m_ids;
    std::vector<core::Placed> m_placed;
    core::Side                m_front = core::Side::Left;   // s150: the master panel last clicked
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
    bool        m_override_marquee = false;                // s169: Ctrl+Shift+drag -- overrides what it touches
    std::vector<core::MasterItemOn> m_override_hits;       //  ... what it touches now
    Glib::RefPtr<Gtk::GestureDrag> m_gdrag;
    Glib::RefPtr<Gtk::EventControllerScroll> m_gscroll;
    double      m_mx = 0, m_my = 0;            // the pointer, screen

    // s006
    bool              m_in = false;                // the pointer is over the canvas
    bool              m_snap = true, m_show_guides = true, m_show_grid = false;
    bool              m_guides_locked = false;   // s153
    bool              m_show_hidden = false;   // s059
    bool              m_mark_spell = true, m_mark_links = true;   // s309
    std::map<std::string, std::vector<layout::TextMark>> m_marks;   // s309: a frame's, until the document moves
    std::uint64_t     m_marks_rev = 0, m_marks_gen = 0;
    std::shared_ptr<const plate::Proof> m_proof;   // s099
    std::uint64_t     m_proof_gen = 0;             // s104: a new proof, new page images
    bool              m_follow_keep_zoom = false;   // s061: a follow shows another spread without fitting
    sigc::signal<void(const std::string&)> m_sig_follow;
    sigc::signal<void(const std::string&, const std::vector<std::string>&)> m_sig_master_drop;
    void follow_caret();                        // s061
    core::SnapTargets m_targets;                   // gathered at press
    double            m_snap_x = NAN, m_snap_y = NAN;   // the lines snapped to (layout), drawn mid-drag
    std::string       m_guide;                     // the guide being dragged
    double            m_guide0 = 0;                // its position at press
    double            m_guide_base = 0;            // s153: its page's left / top (layout), for snapping
    bool              m_guide_off = false;         // dragged off its page: it will go
    std::string       m_hover_guide;
    bool              m_preview = false, m_preview_v = false;
    double            m_preview_at = 0;
    double            m_preview_doc = NAN;         // s153: where it would drop (layout, snapped)
    bool              m_zero_preview = false;      // s154: the zero point being dragged
    double            m_zero_x = 0, m_zero_y = 0;  // ... where (layout, snapped)

    // s007: text, recomputed when the document changes. s101: the shell's
    // cache (share_flows), so a change is flowed once for every view.
    layout::FlowCache  m_own_flows;
    layout::FlowCache* m_flowp = &m_own_flows;
    layout::FlowCache& flows() const { return *m_flowp; }

    // s008
    Tool        m_tool = Tool::Select;
    std::function<void(std::optional<std::uint32_t>)> m_take;   // s180: the eyedropper, waiting
    std::optional<std::uint32_t>                      m_take_under;   // what's under the pointer
    std::string m_draw_page;                 // the page a frame is being drawn on
    double      m_dx0 = 0, m_dy0 = 0;        // its first corner (layout)
    core::Rect  m_draw;                      // the frame being drawn (layout)
    ShapeTool   m_shape = ShapeTool::Rect;   // s198: what the Shape tool draws
    double      m_lx1 = 0, m_ly1 = 0;        // s198: a line's far end (layout)
    int         m_poly_sides = 6;            // s206
    double      m_poly_inner = 1.0;
    // s206: a polygon being drawn from its centre (m_dx0, m_dy0) to a tip at
    // (m_lx1, m_ly1): its corners (layout), the depth snapped as drawn.
    std::vector<core::PathPoint> drawn_polygon(double& inner) const;
    // s199: the Pen -- the path so far (layout), on the page it was started
    // on (its owner, and where that panel sits in the layout).
    std::vector<core::PathPoint> m_pen;
    std::string m_pen_owner;
    std::string m_pen_cont;                  // s208: carrying on from this open path's end ("" a new path)
    bool        m_pen_rev = false;           // ... from its first point (the points held reversed)
    double      m_pen_ox = 0, m_pen_oy = 0;
    void pen_finish(bool keep, bool closed = false);   // Enter / Esc / another tool: drawn (two points or more)
    bool pen_near_first(double sx, double sy) const;
    void open_story_editor();                // s217
    // s219: a point in a frame's text box (frame coordinates, as the flow
    // has it) in the layout -- text on a path follows its path. And back.
    void box_to_layout(const std::string& frame, double bx, double by, double& lx, double& ly) const;
    void layout_to_box(const std::string& frame, double lx, double ly, double& bx, double& by) const;
    bool text_on_path(const std::string& frame) const;
    bool mapped(const std::string& frame) const;   // s230: on a path or turned
    // s220: the picked text on a path's start mark (layout coordinates); false: none to show.
    bool path_start_mark(double& x, double& y, double& angle) const;
    bool path_end_mark(double& x, double& y, double& angle) const;     // s221: where it stops
    // s229: turned frames -- a frame's bounds in the layout, one turned
    // pick's handles (screen), the handle under the pointer either way.
    core::Rect bounds_in_layout(const std::string& id) const;
    bool turned_pick(std::array<std::array<double, 2>, 8>& hs) const;
    core::Handle pick_handle(double x, double y) const;
    std::map<std::string, core::Frame> m_turn0;   // turned frames as a resize began (s231: every pick as a rotate began)
    // s231: rotating by a drag just outside a corner -- the pivot (layout)
    // and the pointer's angle at the press; the angle so far (degrees).
    bool   rotate_zone(double x, double y) const;
    double m_rot_px = 0, m_rot_py = 0, m_rot_a0 = 0, m_rot_deg = 0;
    bool   m_rot_hover = false;
    // s233: a corner handle scales (shapes, pictures; text with Ctrl); just
    // outside an edge's middle a drag skews. The picks as the drag began.
    bool   m_scaling = false;
    core::Handle m_skew_h = core::Handle::None;
    std::map<std::string, core::Frame> m_orig0;
    core::Handle skew_zone(double x, double y) const;
    bool   m_skew_hover = false;
    bool   scale_corner(double x, double y) const;   // s234
    bool   m_scale_hover = false;
public:
    double zoom_click_x() const { return m_zoom_x; }   // s241: where the zoom levels were asked for
    double zoom_click_y() const { return m_zoom_y; }
private:
    double m_zoom_x = 0, m_zoom_y = 0;   // the pointer on the first point (screen): it closes
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
    TableEdge           m_tedge;                // s327: the table column edge being dragged
    bool                m_hover_tedge = false;
    RowEdgeHit          m_redge;                // s332: the table row edge being dragged
    bool                m_hover_redge = false;
    std::vector<double> m_col0;                 // every column's width (points) at press
    sigc::signal<void()> m_sig_sel, m_sig_zoom, m_sig_edited, m_sig_view, m_sig_tool, m_sig_caret;
    sigc::signal<void(const ContextHit&)> m_sig_context;   // s074
    sigc::signal<void(const std::string&, int, int, int)> m_sig_table;   // s323
    sigc::signal<void(int, int)> m_sig_note;   // s114

    // s104: the pages as images at the current zoom, drawn on a worker (last:
    // it stops its worker first, while the dispatcher is still there).
    Glib::Dispatcher   m_pg_disp;
    std::atomic<bool>  m_pg_signalled = false;
    sigc::connection   m_pg_flush, m_pg_collect;
    layout::ThumbStore m_pages{std::size_t{160} << 20};
};

class Ruler;

// The canvas with its head (the title, a count, the zoom) and its rulers.
class CanvasView : public widgets::Box {
public:
    CanvasView(core::Doc& d, core::DocJournal& j);
    ~CanvasView() override;
    Canvas& canvas() { return m_canvas; }
    void set_rulers(bool on);
    void queue_rulers();   // s073: redraw them (the unit changed)
    void set_title(const std::string& t, std::size_t n);
    void set_proof_label(const std::string& profile);   // s099: "" hides the header's Proof pill
    // s068: where back / forward before the title go ("Document").
    void set_nav(const std::string& back_to, const std::string& fwd_to) { m_nav_btns.set(back_to, fwd_to); }
    // s277: Go to Page in the head -- [<][ folio ][>]. The shell fills in what
    // is shown and hears what is typed (Enter).
    void set_goto_text(const std::string& t);
    void focus_goto();
    void flag_goto_miss();   // nothing matched: the field says so a moment
    sigc::signal<void(const std::string&)>& signal_goto() { return m_goto_sig; }

private:
    void sync_zoom();
    widgets::Box    m_head;
    NavButtons    m_nav_btns;   // s068
    widgets::Label  m_title;
    widgets::Label  m_count;
    widgets::Box    m_tools;    // s008: Select / Text / Image / Shape
    std::vector<Gtk::ToggleButton*> m_tool_buttons;
    Gtk::Popover*   m_shape_pop = nullptr;   // s198: rectangle / oval / line / pen (unparented when we go)
    widgets::Button m_fit;
    widgets::Label  m_zoom;
    Gtk::Label*     m_proof_pill = nullptr;   // s099
    Gtk::Entry*     m_goto = nullptr;         // s277
    std::string     m_goto_shown;
    sigc::signal<void(const std::string&)> m_goto_sig;
    widgets::Grid   m_grid;     // [corner][top ruler] / [left ruler][canvas]
    Canvas          m_canvas;
    std::unique_ptr<Ruler> m_top, m_left;
};

}  // namespace composix::views
