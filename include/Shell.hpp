#pragma once
#include "widgets/Widgets.hpp"
#include "core/Outline.hpp"
#include "core/Doc.hpp"      // s004: the document + its journal
#include "views/Canvas.hpp"
#include "views/Inspector.hpp"
#include "views/ItemView.hpp"
#include "views/Navigator.hpp"
#include "views/PageThumbs.hpp"
#include "views/StoryEditor.hpp"
#include "views/PanelHandle.hpp"
#include "views/TileView.hpp"
#include "core/Settings.hpp"

#include <gtkmm/popovermenu.h>
#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/revealer.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include "layout/Preflight.hpp"
#include <gtkmm/togglebutton.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/scale.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/flowboxchild.h>
#include <map>
#include <plate/plate.hpp>
#include <giomm/simpleaction.h>

#include <functional>
#include <memory>
#include <chrono>
#include <gtkmm/spinner.h>
#include <glibmm/dispatcher.h>
#include <thread>
#include <string>
#include <vector>
#include <tuple>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar (inherited from Cairn).
//
// Split across TUs by CATEGORY OF CODE, not by feature. The header IS the index:
// every method carries a trailing `// category: ...` tag naming its file.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: header, the three panes, menu.
//               File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: Gio actions, recents group, the window's
//               undo keys, the views' signals.  File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*).  File: src/Shell_handlers.cpp
//   Helpers  -- reusable sync / orchestration.  File: src/Shell_helpers.cpp
//   Glue     -- the ctor + build_ui().  File: src/Shell.cpp
//
// Layout (s002, REQUIREMENTS §10)
// -------------------------------
//   header:  logo | navigator toggle ........ title ........ inspector toggle | menu
//   body:    shell.body   = [ shell.nav.reveal (nav) | shell.nav.handle | shell.right ]
//            shell.right  = [ shell.main_over (shell.main + shell.inspector.tab)
//                             | shell.inspector.handle | shell.inspector.reveal (inspector) ]
//
// Finding things
// --------------
//   "What happens when I pick in the navigator?" -> Shell_handlers.cpp (on_nav_pick)
//   "What happens when I double-click a tile?"   -> Shell_handlers.cpp (on_tiles_open)
//   "Where are the panes built?"                 -> Shell_zones.cpp    (build_shell)
//   "Where does Ctrl+Z go?"                      -> Shell_bindings.cpp + on_undo/on_redo
// ─────────────────────────────────────────────────────────────────────────────
namespace composix {

class AboutWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: header + panes
    void build_welcome();                        // category: zone: the empty main view
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions, recents, undo keys
    void bind_views();                           // category: bindings: navigator / tiles -> handlers

    // ── handlers ───────────────────────────────────────────────────────────
    void on_nav_pick(const views::Pick& p);      // category: handler: section -> tiles, items -> open
    void on_tiles_selection(const std::vector<std::string>& ids);  // category: handler: -> inspector
    void update_screen_colours();                 // category: helper: s107 -- CMYK swatches on screen through the press profile
    bool apply_proof(bool on);   // s099: proof colours with the export's profile and intent; false = no profile
    std::vector<core::Field> section_fields(core::Segment s) const;   // s097: a section's own Inspector rows
    void on_tiles_open(const std::vector<std::string>& ids);       // category: handler: -> navigator selects them
    void on_inspector_edit(const std::string& key, const std::string& choice);  // category: handler: -> one verb
    void on_canvas_selection();                  // category: handler: frames picked -> inspector
    void on_open();                              // category: handler: pick a project folder -> load it
    void on_new();                               // category: handler: s019 -- the New sheet -> a blank document
    void on_sample();                            // category: handler: s019 -- open the built-in sample book
    void on_big_book();                          // category: handler: s100 -- the generated ~400-page test book
    void on_find();                              // category: handler: s022 -- Ctrl+F: show the find bar
    void on_story_editor();                      // category: handler: s033 -- Ctrl+Y: the story being edited / picked
    void on_save();                              // category: handler: save (Save As the first time)
    void on_save_as();                           // category: handler: name a new project folder -> save
    void on_place();                             // category: handler: pick an image file -> into a frame / loaded on the canvas
    void pick_images(std::function<void(std::vector<core::ImageFile>)> then);   // category: helper: s063 -- Place: several files
    void build_start();                          // category: zone: s064 -- the start screen
    void show_start();                           // category: helper: s064 -- its recent projects, shown
    void restore_window();                       // category: helper: s057 -- size + panels from settings.json
    void save_window();                          // category: helper: s057 -- and back, at close
    void open_swatch_editor(const std::string& id);   // category: helper: s058 -- edit a swatch ("" = a new one)
    void swatch_editor_sync();                   // category: helper: s058 -- the sheet's fields from m_sw
    void on_color_report();                      // category: handler: s053 -- the Colour Report window
    void report_refresh();                       // category: helper: s053 -- proof again (profile picked / shown)
    void on_export_pdf();                        // category: handler: the Export PDF sheet -> a file -> the report
    void on_tiles_move(const std::vector<std::string>& ids, const std::string& target, bool before);  // category: handler: pages dragged
    bool page_action(const std::string& act, const std::vector<std::string>& ids);  // category: helper: add / delete / section
    void on_asset_action(const std::string& act);   // category: handler: relink / update / embed / unembed (inspector)
    bool on_close_request() override;            // category: handler: unsaved changes? ask first
    void on_recent_open(const std::string& p);   // category: handler: open a recent project (parameterised)
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: the About window
    void on_shortcuts();                         // category: handler: keyboard reference
    void on_go_up();                             // category: handler: s068 -- Alt+Up / the header's Up: the section's tiles
    void on_arrange(core::doc::Arrange how);
    void on_canvas_context(const views::Canvas::ContextHit& h);   // category: handler: s074 -- the canvas's right-click menu
    void bind_context_actions();                 // category: bindings: s074 -- what that menu runs     // category: handler: s070 -- the canvas's picked frames, up / down the stack
    void on_go_forward();                        // category: handler: s068 -- Alt+Right / Forward: back into what Back left
    void sync_up();                              // category: helper: s068 -- the Up button: shown, named, enabled
    void on_undo();                              // category: handler: Ctrl+Z / menu
    void on_redo();                              // category: handler: Ctrl+Shift+Z, Ctrl+Y / menu

    // ── helpers ────────────────────────────────────────────────────────────
    std::vector<const core::Item*> resolve(const std::vector<std::string>& ids) const;  // category: helper
    void queue_refresh();                         // category: helper: one re-projection per idle, after the writes
    core::Outline project_now() const;            // category: helper: the projection, with overset measured
    void refresh();                               // category: helper: re-project; every view repaints, keeping its place
    void show_pick(const views::Pick& p, const std::vector<std::string>& keep_tiles);  // category: helper
    void show_pick_now(const views::Pick& p, const std::vector<std::string>& keep_tiles);  // category: helper: s068 -- show_pick, before the Up button
    bool open_project(const std::string& dir);    // category: helper: load + reset the journal + views
    // s102: a progress dialog in front of long work (Shell_busy.cpp).
    using Report = std::function<void(double fraction, const std::string& line)>;
    void busy(const std::string& title, const std::string& what, std::function<void()> work);   // category: helper: UI-thread work, dialog drawn first
    void busy_thread(const std::string& title, const std::string& what, std::function<void(const Report&)> job,
                     const std::string& then_what, std::function<void()> done);   // category: helper: a worker job with a bar, then UI work
    static constexpr std::size_t kBusyPages = 100;
    void export_pdf_now(const std::string& path);   // category: helper: s102 -- export_pdf_to's work (it may put up the dialog first)   // s102: books this long get the dialog
    void adopt(const core::Doc& fresh, const std::string& dir,
               layout::FlowCache* laid = nullptr);  // category: helper: s019 -- swap in a whole document; s102: `laid` -- its flows, made on a worker
    void confirm_discard(std::function<void()> then);  // category: helper: s019 -- unsaved? ask, then go on
    void show_new_sheet();                        // category: helper: s019 -- the New sheet
    void build_find_bar();                        // category: zone: s022 -- Find / Change, over the main area
    Glib::RefPtr<Gio::Menu> special_menu();       // category: zone: s023 -- Insert Special Character
    void build_preflight();                       // category: zone: s024 -- the header's Preflight button + list
    void update_preflight();                      // category: helper: s024 -- the checks again; the count, the list
    void finish_flows();
    void poll_work();
    void edit_note(const std::string& story, int p, int k, int sel_a = -1, int sel_b = -1, bool take_focus = true);   // s113: a Find hit's words selected, the keyboard left where it is
    void go_to_note(const std::string& id);    // category: helper: s115 -- a footnote's page, its words open
    void on_insert_xref();                      // category: handler: s128 -- a cross-reference at the cursor
    void on_edit_xref();                        // category: handler: s130 -- the one beside the cursor, changed
    void open_xref(int edit_para, int edit_k);  // s130: the picker (edit_k >= 0: changing that reference)
    void open_xrefs_panel();                    // category: helper: s133 -- every cross-reference, one panel
    void open_sources_panel();                  // category: helper: s144 -- the bibliography's sources, one panel
    void sources_panel_sync();                  // category: helper: s144 -- the panel from the document
    void on_index_mark();                       // category: handler: s138 -- Index Entry... at the selection
    void on_edit_index();                       // category: handler: s138 -- the mark beside the cursor
    void open_index_pop(int edit_para, int edit_k);   // category: helper: s138 -- the index entry popover
    void on_new_index();                        // category: handler: s138 -- an Index on the page on show, built
    void xrefs_panel_sync();                    // category: helper: s133 -- the panel from the document
    void go_to_ref(const std::string& story, int para, int k, bool edit);   // category: helper: s133 -- a reference selected (and its picker)
    void open_notes_panel();                     // category: helper: s121 -- Footnotes & Endnotes, one panel
    void notes_panel_sync();                     // category: helper: s121 -- the panel from the document
    void draw_notes_preview(const Cairo::RefPtr<Cairo::Context>& cr, int cw, int ch);   // category: helper: s121
    void close_find_note();                     // category: helper: s113 -- the note Find opened, closed   // category: helper: s110 -- a footnote's words, in a popover at the caret                             // category: helper: s108 -- the header's "working" pill                          // category: helper: s106 -- a provisional flow's whole, on a worker
    void go_to_issue(const layout::Issue& i);     // category: helper: s024 -- its page, its frame picked
    void find_refresh();                          // category: helper: s022 -- the hits again, from the bar's settings
    void find_go(int dir);                        // category: helper: s022 -- next / previous hit, shown on the canvas
    void find_show(int i, bool focus);            // category: helper: s022 -- one hit, on its page, selected
    void find_change(bool all);                   // category: helper: s022 -- Change / Change All (one step)
    void find_status(const std::string& extra = {});
    core::SavedQuery find_capture() const;        // category: helper: s078 -- the bar's settings, as a query
    void find_load(const core::SavedQuery& q);    // category: helper: s078 -- a saved query into the bar
    void find_saved_fill();                       // category: helper: s078 -- the Saved popover's list, afresh  // category: helper: s022 -- "3 of 12"
    bool save_to(const std::string& dir);         // category: helper: write + remember + clean
    void set_dirty(bool d);                       // category: helper
    void update_title();                          // category: helper: the header's two lines
    void alert(const std::string& msg, const std::string& detail);  // category: helper
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                  // category: helper: repaint the submenu
    std::string recents_file() const;             // category: helper: the XDG path (UI-side resolution)
    void update_undo_ui();                        // category: helper: Undo / Redo labels + greying
    bool focus_is_text() const;                   // category: helper: the keyboard is in a text box
    void sync_inspector();                        // category: helper: toggle + tab follow the panel
    void sync_nav();                              // category: helper: the toggle follows the navigator
    void tab_hover(bool in);                      // category: helper: the pull tab's shared hover
    void pick_image(std::function<void(const core::ImageFile&)> then);  // category: helper: file dialog + probe
    std::string embed_copy(const core::Asset& a, std::string* err);     // category: helper: copy into <project>/assets

    // Header.
    widgets::Button       m_logo_button;
    widgets::ToggleButton m_nav_toggle;
    std::vector<std::string> m_up_keep;     // s068: the items to select among the tiles, going up
    Glib::RefPtr<Gio::SimpleAction> m_act_up;
    // s068: Forward -- the items left by going up, and the section it went
    // to; good while that section is still what's shown.
    std::vector<std::string>         m_fwd_items;
    std::optional<core::Segment>     m_fwd_from;
    bool                             m_moving = false;   // a pick made by Back / Forward
    Glib::RefPtr<Gio::SimpleAction> m_act_fwd;
    widgets::Box          m_title_box;
    widgets::Label        m_title;
    widgets::Label        m_subtitle;
    widgets::ToggleButton m_insp_toggle;
    widgets::MenuButton   m_menu_button;

    // Body.
    // [nav (slides)][nav handle][right] -- s007 fix: was a Gtk::Paned, whose
    // extended grab zone reached into the left ruler and stole guide drags.
    widgets::Box      m_body;
    widgets::Revealer m_nav_rev;
    std::unique_ptr<views::PanelHandle> m_nav_handle;
    widgets::Box     m_right;      // [main + pull tab][handle][inspector] (s003)
    widgets::Overlay m_main_over;  // the main view, the pull tab floating on its right edge
    widgets::Revealer m_insp_rev;  // the inspector slides
    widgets::Box     m_insp_tab;   // the pull tab -- IS the inspector's handle (as Samson)
    Gtk::Label       m_insp_tab_glyph;
    std::unique_ptr<views::PanelHandle> m_insp_handle;   // the inspector's inner edge
    sigc::connection m_tab_fade;   // "lit" ends a moment after the pointer leaves
    widgets::Stack  m_main;    // "welcome" / "tiles" / "item"
    widgets::Box    m_welcome;
    views::Navigator m_nav;
    views::TileView  m_tiles;
    views::ItemView  m_item;
    views::Inspector m_inspector;

    // What the views read: projected from the document (s004).
    core::Outline   m_outline;
    std::optional<core::Segment> m_segment;   // the category on the tile view
    views::Pick              m_pick;          // what the navigator picked
    std::vector<std::string> m_inspected;     // the ids the inspector shows (an edit's target)
    bool                     m_refresh_queued = false;

    // Recents state (core data; UI holds it + resolves the path -- the seam).
    std::vector<std::string>        m_recents;
    std::string                     m_recents_file;
    // s052: the app's own choices between runs (settings.json beside recent.json).
    core::AppSettings               m_settings;
    std::string                     m_settings_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;

    // The document and its journal (s004). Every write goes through
    // m_journal (core::doc verbs); the door is checked in Doc::put.
    // Declaration order matters: the store before the journal that holds it.
    core::Doc          m_doc;
    core::DocJournal   m_journal{m_doc};
    std::string        m_project_dir;    // "" = the unsaved sample / never saved
    bool               m_dirty = false;
    std::uint64_t      m_saved_mark = 0;   // s057: the journal's mark when last saved / opened
    bool               m_closing_ok = false;   // the user chose; let the close through

    // s089: Preflight's flows; s101: THE flow cache -- the canvas and the
    // thumbnails share it (declared before them: it outlives them).
    mutable layout::FlowCache        m_pf_flows;
    std::string                      m_screen_profile = "\x01";   // s107: the profile CMYK is shown through ("" none)
    bool                             m_proof_paper = true;          // s107: Proof Colours simulates the paper
    static constexpr int             kReflowBudget = 24;   // s106: frames a re-flow sets on the spot
    static int reflow_budget();   // kReflowBudget, or COMPOSIX_REFLOW_BUDGET (a test knob)
    // s106: the whole of a provisional flow, worked out on a worker.
    struct FlowJob;
    std::shared_ptr<FlowJob>         m_flow_job;
    std::thread                      m_flow_thread;
    Glib::Dispatcher                 m_flow_disp;
    bool                             m_flow_disp_bound = false;
    mutable std::map<std::string, std::string> m_stale_last;   // the contents check while a flow is provisional

    // s020: the page / master tiles drawn for real, kept per page.
    views::PageThumbs  m_page_thumbs{m_doc, [this] { return m_project_dir; }};

    // s033: the story editor -- words without the page.
    views::StoryEditor m_story_ed{m_doc, m_journal};

    // s005: the page canvas -- after the document and journal it edits.
    views::CanvasView  m_canvas_view{m_doc, m_journal};
    Glib::RefPtr<Gio::Menu>         m_edit_menu;
    Glib::RefPtr<Gio::Menu>         m_arrange_menu;
    // s074: the canvas's right-click menu, and what it was opened on.
    std::unique_ptr<Gtk::PopoverMenu> m_ctx_menu;
    bool                              m_note_pop_find = false;   // s113: the popover is Find's (shows a hit)
    std::unique_ptr<Gtk::Popover>     m_xref_pop;   // s128: the cross-reference picker
    std::unique_ptr<Gtk::Popover>     m_index_pop;  // s138: the index entry popover
    int                               m_index_span = 0, m_index_count = 1;   // s139: what the mark in the popover covers
    int                               m_xref_show = 0;   // s128: what the last reference showed (page / words / both / note / own)
    std::string                       m_xref_pattern;    // s129: the last pattern of its own
    std::string                       m_xref_locator;    // s143: the last citation's locator ("p. 14")
    bool                              m_xref_entry_fill = false;   // s143: the entry being filled, not typed in
    bool                              m_xref_add = false;          // s143: edit: a source clicked joins the citation
    std::unique_ptr<Gtk::Popover>     m_note_pop;   // s110: a footnote's words, edited at the caret
    views::Canvas::ContextHit         m_ctx;   // s070: kept, so the Edit section's rebuild keeps it
    Glib::RefPtr<Gio::SimpleAction> m_act_undo, m_act_redo;

    // s013: the Export PDF sheet, and its choices for the session.
    std::unique_ptr<Gtk::Window>     m_export;
    std::function<void()>            m_export_sync;   // s086: the sheet's live state and summary card
    // s058: the swatch editor -- the swatch being made / changed, its sheet's widgets.
    // s064: the start screen's recent-project cards and their folders.
    Gtk::FlowBox*                    m_start_grid = nullptr;
    Gtk::Label*                      m_start_empty = nullptr;
    std::map<const Gtk::FlowBoxChild*, std::string> m_start_paths;
    std::unique_ptr<Gtk::Window>     m_swed;
    // s144: the Sources panel.
    std::unique_ptr<Gtk::Window>     m_sp;
    Gtk::ListBox*                    m_sp_list = nullptr;
    Gtk::Label*                      m_sp_count = nullptr;
    Gtk::ToggleButton*               m_sp_uncited = nullptr;
    std::vector<std::tuple<std::string, int, int>> m_sp_rows;   // a source (k -1) or a broken citation (its k)
    // s133: the Cross-References panel.
    std::unique_ptr<Gtk::Window>     m_xp;
    Gtk::ListBox*                    m_xp_list = nullptr;
    Gtk::Label*                      m_xp_count = nullptr;
    Gtk::ToggleButton*               m_xp_broken = nullptr;
    std::vector<std::tuple<std::string, int, int>> m_xp_rows;   // each row's reference: story, paragraph, index
    // s121: the Footnotes & Endnotes panel.
    std::unique_ptr<Gtk::Window>     m_np;
    Gtk::DrawingArea*                m_np_preview = nullptr;
    Gtk::DropDown*                   m_np_at = nullptr;
    Gtk::DropDown*                   m_np_restart = nullptr;
    Gtk::DropDown*                   m_np_style = nullptr;
    Gtk::DropDown*                   m_np_num = nullptr;   // s126
    Gtk::SpinButton*                 m_np_area = nullptr;
    Gtk::SpinButton*                 m_np_rule = nullptr;
    Gtk::SpinButton*                 m_np_rule_w = nullptr;
    Gtk::SpinButton*                 m_np_space = nullptr;
    Gtk::Entry*                      m_np_cont = nullptr;
    Gtk::Entry*                      m_np_title = nullptr;
    Gtk::Label*                      m_np_note = nullptr;
    bool                             m_np_syncing = false;
    core::Swatch                     m_sw;
    std::string                      m_sw_id;                 // "" = new
    bool                             m_sw_quiet = false;
    bool                             m_sw_hex_typing = false;
    Gtk::Entry*                      m_sw_name = nullptr;
    Gtk::DropDown*                   m_sw_model = nullptr;
    Gtk::Entry*                      m_sw_hex = nullptr;
    Gtk::Entry*                      m_sw_ink = nullptr;
    Gtk::Label*                      m_sw_title = nullptr;
    Gtk::Label*                      m_sw_press = nullptr;
    Gtk::Button*                     m_sw_save = nullptr;
    Gtk::DrawingArea*                m_sw_chip = nullptr;
    std::vector<Gtk::Widget*>        m_sw_rows;                // 4 component rows
    std::vector<Gtk::Label*>         m_sw_labels;
    std::vector<Gtk::Scale*>         m_sw_scales;
    std::vector<Gtk::SpinButton*>    m_sw_spins;
    Gtk::Widget*                     m_sw_hex_row = nullptr;
    Gtk::Widget*                     m_sw_ink_row = nullptr;
    std::unique_ptr<plate::Proof>    m_sw_proof;              // the press, for an RGB swatch
    std::uint32_t                    m_sw_after = 0;
    // s053: the Colour Report -- its window, profile choice, list, and each row's page / frame.
    std::unique_ptr<Gtk::Window>     m_report;
    Gtk::DropDown*                   m_report_profile = nullptr;
    Gtk::Label*                      m_report_note = nullptr;
    Gtk::DropDown*                   m_report_intent = nullptr;   // s085: the report's intent (starts as Export's)
    Gtk::Label*                      m_report_legend = nullptr;
    Gtk::ListBox*                    m_report_list = nullptr;
    std::vector<std::string>         m_report_paths, m_report_pages, m_report_frames;
    bool                             m_report_quiet = false;
    bool                             m_pdf_bleed = true, m_pdf_marks = false;
    bool                             m_pdf_print = true;   // s015: Plate (print) or cairo (screen)
    std::string                      m_pdf_profile;        // s016: the press's ICC ("" = none)
    bool                             m_pdf_convert = false;
    std::vector<std::string>         m_pdf_only;            // s054: the range's pages (empty = all)
    bool                             m_pdf_bookmarks = true;
    std::vector<std::string>         m_pdf_mark_lists;      // s082: the lists the outline comes from (empty = default)
    bool                             m_pdf_spreads = false; // s084: facing pages as one sheet
    bool                             m_pdf_perceptual = false;   // s085: the conversion's intent
    bool                             m_pdf_rebuild = true;       // s089: rebuild stale contents first
    bool                             m_proofing = false;         // s099: View > Proof Colours
    std::string                      m_pdf_range;   // s077: as typed, "" = all
    int                              m_pdf_standard = 0;   // s017: 0 none, 1 PDF/X-1a, 2 PDF/X-4
    std::vector<std::string>         m_profiles;           // the sheet's list, as paths
    std::vector<std::string>         m_profile_names;      // s032: ... and their names (what the document keeps)
    std::string                      m_profile_missing;    // s032: the document's profile, not on this computer
    void load_export_prefs();                     // category: helper: s032 -- the document's export settings -> the sheet's
    std::vector<std::string> profile_dirs() const;          // category: helper: where ICC profiles live
    void export_pdf_to(const std::string& path);  // category: helper: write + report

    // s022: Find / Change -- the bar (an overlay over the main area) and
    // its hits, recomputed when the document has moved since.
    Gtk::Revealer*                   m_find_rev = nullptr;
    Gtk::Entry*                      m_find_what = nullptr;
    Gtk::Entry*                      m_find_with = nullptr;
    Gtk::ToggleButton*               m_find_case = nullptr;
    Gtk::ToggleButton*               m_find_word = nullptr;
    Gtk::DropDown*                   m_find_in = nullptr;
    Gtk::DropDown*                   m_find_apply = nullptr;
    Gtk::DropDown*                   m_find_format = nullptr;   // s051: Any / Bold / Italic / Bold Italic
    Gtk::DropDown*                   m_find_mode = nullptr;     // s056: Text / Wildcards / Regex
    Gtk::DropDown*                   m_find_scope = nullptr;    // s056: Document / This story / Selection
    std::string                      m_find_story;              // s056: the story and selection when Find opened
    core::TextPos                    m_find_from, m_find_to;
    std::string                      m_find_bad;                // s056: why nothing can be found (a bad pattern ...)
    Gtk::Label*                      m_find_count = nullptr;
    Gtk::Popover*                    m_find_saved_pop = nullptr;   // s078: saved queries
    Gtk::Box*                        m_find_saved_list = nullptr;
    Gtk::Entry*                      m_find_saved_name = nullptr;
    std::vector<std::string>         m_find_styles;   // the dropdowns' ids after "Any" / "Leave"
    std::vector<core::doc::Hit>      m_hits;
    int                              m_hit = -1;
    std::uint64_t                    m_hits_rev = ~0ull;

    // s024: Preflight -- the header button (a count), its list, the checks' flows.
    Gtk::MenuButton*                 m_preflight = nullptr;
    // s108: background work you can see -- a spinner and what it's doing.
    Gtk::Box*                        m_work = nullptr;
    Gtk::Spinner*                    m_work_spin = nullptr;
    Gtk::Label*                      m_work_label = nullptr;
    std::chrono::steady_clock::time_point m_work_since{};
    bool                             m_work_busy = false;
    sigc::connection                 m_work_poll;
    Gtk::Label*                      m_preflight_label = nullptr;
    Gtk::ListBox*                    m_preflight_list = nullptr;
    std::vector<layout::Issue>       m_issues;

    // s019: the New sheet, and what it was last set to (for the session).
    std::unique_ptr<Gtk::Window>     m_new;
    core::NewDoc                     m_newdoc;

    std::unique_ptr<AboutWindow>     m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;
};

}  // namespace composix
