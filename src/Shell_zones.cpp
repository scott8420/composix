#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/progressbar.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gdkmm/cursor.h>
#include <glibmm/main.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the header, the
// three panes, the empty main view, and the menu model.

namespace composix {

void Shell::build_shell() {  // zone: header + panes
    set_title("Composix");
    set_default_size(1280, 820);
    add_css_class("cx-window");

    // ── header ──────────────────────────────────────────────────────────────
    auto header = Gtk::make_managed<Gtk::HeaderBar>();
    header->set_name("shell.header");
    header->add_css_class("cx-header");

    m_logo_button.set_icon_name("composix-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About Composix");
    m_logo_button.set_action_name("win.about");
    m_logo_button.add_css_class("cx-logo");
    header->pack_start(m_logo_button);

    m_nav_toggle.set_icon_name("sidebar-show-symbolic");
    m_nav_toggle.set_tooltip_text("Show / hide the navigator  (Ctrl+Alt+1)");
    m_nav_toggle.set_action_name("win.toggle-nav");
    m_nav_toggle.add_css_class("cx-chrome-button");
    header->pack_start(m_nav_toggle);

    // A two-line title, the document's name over its kind (web / Mac roots).
    m_title.set_text("Composix");
    m_title.add_css_class("cx-doc-title");
    m_subtitle.add_css_class("cx-doc-subtitle");
    m_title_box.set_valign(Gtk::Align::CENTER);
    m_title_box.append(m_title);
    m_title_box.append(m_subtitle);
    header->set_title_widget(m_title_box);

    // s279 (Scott): a menubar -- File, Edit, Type, Object, Layout, View, Help --
    // in place of the one menu button (one menu to keep, not two).
    m_menubar = Gtk::make_managed<Gtk::PopoverMenuBar>(build_menu());
    m_menubar->set_name("shell.menubar");
    m_menubar->add_css_class("cx-menubar");
    m_menubar->set_valign(Gtk::Align::CENTER);
    header->pack_start(*m_menubar);

    m_insp_toggle.set_icon_name("sidebar-show-right-symbolic");
    m_insp_toggle.set_tooltip_text("Show / hide the inspector  (Ctrl+Alt+2)");
    m_insp_toggle.set_action_name("win.toggle-inspector");
    m_insp_toggle.add_css_class("cx-chrome-button");
    header->pack_end(m_insp_toggle);
    build_preflight();   // s024
    header->pack_end(*m_preflight);
    // s108: work going on in the background, beside Preflight -- shown only
    // once it has gone on a moment, so quick work doesn't flicker it.
    m_work = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_work->set_name("shell.work");
    m_work->add_css_class("cx-work-pill");
    m_work->set_valign(Gtk::Align::CENTER);
    m_work_spin = Gtk::make_managed<Gtk::Spinner>();
    m_work_label = Gtk::make_managed<Gtk::Label>();
    m_work->append(*m_work_spin);
    m_work->append(*m_work_label);
    m_work->set_visible(false);
    header->pack_end(*m_work);
    m_work_poll = Glib::signal_timeout().connect([this] { poll_work(); return true; }, 200);
    set_titlebar(*header);

    // ── body: nav | (main | inspector) ─────────────────────────────────────
    m_main.add_css_class("cx-main");
    m_main.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_main.set_transition_duration(120);
    build_welcome();
    build_start();   // s064
    m_main.add(m_welcome, "welcome");
    m_main.add(m_tiles, "tiles");
    m_main.add(m_item, "item");
    m_main.add(m_canvas_view, "canvas");   // s005: pages and masters open here
    m_main.add(m_story_ed, "story");       // s033: a story opens as running text
    m_main.set_visible_child("welcome");

    // ── the inspector: a sliding panel, its handle, and the pull tab ───────
    // (s003, as Samson s013/s014.) The handle on the panel's inner edge drags
    // to size it, drags nearly shut to close it, and -- shut -- waits at the
    // window's edge to be pulled out. The PULL TAB floats over the main view's
    // right edge, halfway down, quiet until the pointer finds it: drag it to
    // size, click it to push the panel back or pull it out.
    m_insp_rev.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
    m_insp_rev.set_transition_duration(200);
    m_insp_rev.set_child(m_inspector);
    m_insp_rev.set_hexpand(false);
    m_insp_handle = std::make_unique<views::PanelHandle>(
        "shell.inspector.handle", views::PanelHandle::Side::Right, m_insp_rev, 240, 560);
    m_insp_handle->set_width(290);
    m_insp_handle->changed = [this] { sync_inspector(); };

    m_insp_tab.add_css_class("cx-pull-tab");
    m_insp_tab.set_halign(Gtk::Align::END);
    m_insp_tab.set_valign(Gtk::Align::CENTER);
    m_insp_tab_glyph.set_hexpand(true);
    m_insp_tab_glyph.set_vexpand(true);
    m_insp_tab.append(m_insp_tab_glyph);
    m_insp_tab.set_cursor(Gdk::Cursor::create("col-resize"));
    m_insp_handle->add_drag(m_insp_tab, true);
    // One "lit" for the edge and the tab: pointing at either lights the tab,
    // and it fades a moment after the pointer leaves both (no flicker between).
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_insp_tab),
                           static_cast<Gtk::Widget*>(m_insp_handle.get())}) {
        auto motion = Gtk::EventControllerMotion::create();
        motion->signal_enter().connect([this](double, double) { tab_hover(true); });
        motion->signal_leave().connect([this] { tab_hover(false); });
        w->add_controller(motion);
    }

    m_main.set_hexpand(true);
    m_main.set_vexpand(true);
    m_main_over.set_child(m_main);
    m_main_over.add_overlay(m_insp_tab);
    build_find_bar();   // s022
    {   // s356 (Scott: "a progress bar in a conspicuous place"): the book laid out in the background
        m_layout_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        m_layout_card->set_name("shell.layout_card");
        m_layout_card->add_css_class("cx-layout-card");
        m_layout_card->set_halign(Gtk::Align::CENTER);
        m_layout_card->set_valign(Gtk::Align::START);
        m_layout_card->set_can_target(false);   // the canvas under it still takes the mouse
        m_layout_label = Gtk::make_managed<Gtk::Label>();
        m_layout_label->add_css_class("cx-layout-label");
        m_layout_bar = Gtk::make_managed<Gtk::ProgressBar>();
        m_layout_bar->set_size_request(260, -1);
        m_layout_card->append(*m_layout_label);
        m_layout_card->append(*m_layout_bar);
        m_layout_card->set_visible(false);
        m_main_over.add_overlay(*m_layout_card);
    }
    m_main_over.set_hexpand(true);
    m_main_over.set_vexpand(true);
    m_right.append(m_main_over);
    m_right.append(*m_insp_handle);
    m_right.append(m_insp_rev);
    m_insp_handle->set_open(true);

    // The navigator slides like the inspector, with the same kind of handle
    // on its inner edge (drag to size / shut / pull out). Its grab area is
    // exactly its own strip, so the left ruler beside it keeps its drags.
    m_nav_rev.set_transition_type(Gtk::RevealerTransitionType::SLIDE_RIGHT);
    m_nav_rev.set_transition_duration(200);
    m_nav_rev.set_child(m_nav);
    m_nav_rev.set_hexpand(false);
    m_nav_handle = std::make_unique<views::PanelHandle>(
        "shell.nav.handle", views::PanelHandle::Side::Left, m_nav_rev, 180, 420);
    m_nav_handle->set_width(250);
    m_nav_handle->changed = [this] { sync_nav(); };
    // s176: the navigator's pull tab, as the inspector's: on the main view's
    // left edge, halfway down -- drag to size, click to push back / pull out.
    m_nav_tab.add_css_class("cx-pull-tab");
    m_nav_tab.add_css_class("cx-left");
    m_nav_tab.set_halign(Gtk::Align::START);
    m_nav_tab.set_valign(Gtk::Align::CENTER);
    m_nav_tab_glyph.set_hexpand(true);
    m_nav_tab_glyph.set_vexpand(true);
    m_nav_tab.append(m_nav_tab_glyph);
    m_nav_tab.set_cursor(Gdk::Cursor::create("col-resize"));
    m_nav_handle->add_drag(m_nav_tab, true);
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_nav_tab), static_cast<Gtk::Widget*>(m_nav_handle.get())}) {
        auto motion = Gtk::EventControllerMotion::create();
        motion->signal_enter().connect([this](double, double) { tab_hover(true, true); });
        motion->signal_leave().connect([this] { tab_hover(false, true); });
        w->add_controller(motion);
    }
    m_main_over.add_overlay(m_nav_tab);
    m_nav_handle->set_open(true);
    m_right.set_hexpand(true);
    m_body.append(m_nav_rev);
    m_body.append(*m_nav_handle);
    m_body.append(m_right);
    set_child(m_body);
}

void Shell::build_welcome() {  // zone: the main view when nothing is picked
    m_welcome.add_css_class("cx-welcome");
    m_welcome.set_valign(Gtk::Align::CENTER);
    m_welcome.set_halign(Gtk::Align::CENTER);
    auto icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("composix-logo-symbolic");
    icon->set_pixel_size(64);
    icon->add_css_class("cx-welcome-icon");
    auto title = Gtk::make_managed<Gtk::Label>("Pick a section");
    title->add_css_class("cx-welcome-title");
    auto body = Gtk::make_managed<Gtk::Label>(
        "Choose a section in the navigator to see everything in it as tiles,\n"
        "or open its items to work on them.");
    body->set_justify(Gtk::Justification::CENTER);
    body->add_css_class("cx-welcome-body");
    m_welcome.append(*icon);
    m_welcome.append(*title);
    m_welcome.append(*body);
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: the menubar's model (s279; was the hamburger's, s101)
    auto bar = Gio::Menu::create();
    const auto section = [](const Glib::RefPtr<Gio::Menu>& into, std::initializer_list<std::pair<const char*, const char*>> items) {
        auto sec = Gio::Menu::create();
        for (const auto& [label, action] : items) sec->append(label, action);
        into->append_section(sec);
        return sec;
    };

    // ── File ──
    auto file = Gio::Menu::create();
    {
        auto docs = section(file, {{"New\u2026", "win.new"}, {"Open Project\u2026", "win.open"}});
        m_recents_menu = Gio::Menu::create();
        docs->append_submenu("Open Recent", m_recents_menu);
        docs->append("Close Project", "win.close-project");   // s145; s146: under Open (Scott)
        section(file, {{"Save", "win.save"}, {"Save As\u2026", "win.save-as"}});
        section(file, {{"Place Image\u2026", "win.place"}, {"Import ODT\u2026", "win.import-odt"}});
        section(file, {{"Export PDF\u2026", "win.export-pdf"}, {"Colour Report\u2026", "win.color-report"}});
        section(file, {{"Quit", "win.quit"}});
    }
    bar->append_submenu("_File", file);

    // ── Edit ── (the Undo / Redo section is rebuilt after each step: their labels)
    auto edit = Gio::Menu::create();
    m_edit_menu = Gio::Menu::create();
    m_edit_menu->append("Undo", "win.undo");
    m_edit_menu->append("Redo", "win.redo");
    edit->append_section(m_edit_menu);
    section(edit, {{"Cut", "win.cut-frames"}, {"Copy", "win.copy-frames"}, {"Paste", "win.paste-frames"},
                   {"Paste in Place", "win.paste-in-place"}, {"Duplicate", "win.duplicate-frames"},
                   {"Step and Repeat\u2026", "win.step-repeat"}});   // s282; s284
    section(edit, {{"Find / Change\u2026", "win.find"}, {"Check Spelling\u2026", "win.check-spelling"}});   // s313
    section(edit, {{"Delete Frames", "win.delete-frames"}});
    bar->append_submenu("_Edit", edit);

    // ── Type ──
    auto type = Gio::Menu::create();
    {
        auto ins = Gio::Menu::create();
        ins->append_submenu("Insert Special Character", special_menu());   // s023
        ins->append_submenu("Insert Field", field_menu());                 // s274
        ins->append_submenu("Type Style", type_style_menu());              // s263
        type->append_section(ins);
        section(type, {{"Footnote", "win.insert-footnote"}, {"Index Entry\u2026", "win.index-mark"}, {"Cross-Reference\u2026", "win.insert-xref"}, {"Hyperlink\u2026", "win.hyperlink"}});
        section(type, {{"Insert Table\u2026", "win.insert-table"}, {"Edit Table\u2026", "win.edit-table"},
                       {"Convert Text to Table", "win.text-to-table"}, {"Convert Table to Text", "win.table-to-text"}});   // s318; s324
        section(type, {{"Story Editor", "win.story-editor"}});
        section(type, {{"Footnotes\u2026", "win.notes-panel"}, {"Sources\u2026", "win.sources-panel"}, {"Cross-References\u2026", "win.xrefs-panel"}});
        section(type, {{"New Index\u2026", "win.new-index"}});
    }
    bar->append_submenu("_Type", type);

    // ── Object ── (also the canvas's right-click Arrange)
    auto object = Gio::Menu::create();
    {   // s070
        m_arrange_menu = Gio::Menu::create();
        m_arrange_menu->append("Bring to Front", "win.bring-front");
        m_arrange_menu->append("Bring Forward", "win.bring-forward");
        m_arrange_menu->append("Send Backward", "win.send-backward");
        m_arrange_menu->append("Send to Back", "win.send-back");
        auto turn = Gio::Menu::create();   // s231
        turn->append("Rotate 90\u00B0 Counter-clockwise", "win.turn::ccw");
        turn->append("Rotate 90\u00B0 Clockwise", "win.turn::cw");
        turn->append("Flip Horizontal", "win.turn::fliph");
        turn->append("Flip Vertical", "win.turn::flipv");
        m_arrange_menu->append_section(turn);
        auto paths = Gio::Menu::create();   // s212
        paths->append("Make Compound Path", "win.make-compound");
        paths->append("Release Compound Path", "win.release-compound");
        m_arrange_menu->append_section(paths);
        auto finder = Gio::Menu::create();   // s214: the Pathfinder -- a section (a nested submenu warned in GTK's popover stack)
        finder->append("Unite Shapes", "win.path-op::0");
        finder->append("Subtract (Back Minus Front)", "win.path-op::1");
        finder->append("Intersect Shapes", "win.path-op::2");
        finder->append("Exclude Overlap", "win.path-op::3");
        m_arrange_menu->append_section(finder);
        auto outline = Gio::Menu::create();   // s215
        outline->append("Offset Path\u2026", "win.offset-path");
        outline->append("Outline Stroke", "win.outline-stroke");
        m_arrange_menu->append_section(outline);
        // The menubar's Object: the same items, laid out flat.
        object->append_section(m_arrange_menu);
        {   // s299: align and distribute
            auto al = Gio::Menu::create();
            static const char* edges[] = {"Left Edges", "Horizontal Centres", "Right Edges", "Top Edges", "Vertical Centres", "Bottom Edges"};
            for (int e = 0; e < 6; ++e) al->append(edges[e], "win.align::" + std::to_string(e) + ":s");
            auto dist = Gio::Menu::create();
            dist->append("Horizontal Centres", "win.distribute::0");
            dist->append("Vertical Centres", "win.distribute::1");
            dist->append("Horizontal Space", "win.distribute::2");
            dist->append("Vertical Space", "win.distribute::3");
            auto am = Gio::Menu::create();
            am->append_submenu("Align", al);
            am->append_submenu("Distribute", dist);
            object->append_section(am);
            auto gl = Gio::Menu::create();   // s300 / s301
            gl->append("Group", "win.group");
            gl->append("Ungroup", "win.ungroup");
            gl->append("Lock", "win.lock");
            gl->append("Unlock All on Spread", "win.unlock-spread");
            object->append_section(gl);
        }
        auto wrap = Gio::Menu::create();
        wrap->append("None", "win.frame-wrap::0");
        wrap->append("Wrap Around", "win.frame-wrap::1");
        wrap->append("Jump Over", "win.frame-wrap::2");
        wrap->append("Wrap to Shape", "win.frame-wrap::3");
        auto w = Gio::Menu::create();
        w->append_submenu("Text Wrap", wrap);
        w->append("Ignore Others' Wrap", "win.frame-ignore-wrap");
        object->append_section(w);
        section(object, {{"Remove Override", "win.remove-override"}});
    }
    bar->append_submenu("_Object", object);

    // ── Layout ── (s277: Go to Page; s279: the page verbs for the spread shown)
    auto layout = Gio::Menu::create();
    section(layout, {{"Go to Page\u2026", "win.go-to-page"}, {"Previous Spread", "win.prev-spread"}, {"Next Spread", "win.next-spread"}});
    section(layout, {{"Back", "win.go-up"}, {"Forward", "win.go-forward"}});
    section(layout, {{"Add Page After", "win.layout-add-page"}, {"Duplicate Spread", "win.layout-duplicate"},
                     {"Delete Pages", "win.layout-delete-pages"}});
    section(layout, {{"Override All Master Items", "win.layout-override-all"}, {"Remove All Overrides", "win.layout-remove-overrides"}});
    bar->append_submenu("_Layout", layout);

    // ── View ──
    auto view = Gio::Menu::create();
    section(view, {{"Navigator", "win.toggle-nav"}, {"Inspector", "win.toggle-inspector"}});
    section(view, {{"Zoom In", "win.zoom-in"}, {"Zoom Out", "win.zoom-out"}, {"Fit the Pages", "win.zoom-fit"},
                   {"Actual Size", "win.zoom-100"}, {"200%", "win.zoom-200"}, {"300%", "win.zoom-300"}});
    section(view, {{"Rulers", "win.toggle-rulers"}, {"Guides", "win.toggle-guides"}, {"Lock Guides", "win.toggle-guide-lock"},
                   {"Baseline Grid", "win.toggle-grid"}, {"Hidden Characters", "win.toggle-hidden"}, {"Snap to Guides & Edges", "win.toggle-snap"}});
    section(view, {{"Misspelled Words", "win.toggle-spelling"}, {"Hyperlinks", "win.toggle-links"},   // s309
                   {"Composition Problems", "win.toggle-composition"}});   // s357: widows, orphans, runts
    section(view, {{"Proof Colours", "win.toggle-proof"}, {"Simulate Paper", "win.toggle-paper"}});
    bar->append_submenu("_View", view);

    // ── Help ──
    auto help = Gio::Menu::create();
    section(help, {{"Start Screen", "win.start"}, {"Keyboard Shortcuts", "win.shortcuts"}});
    section(help, {{"Open Sample Book", "win.open-sample"}, {"Open Big Test Book", "win.open-big-book"}});
    section(help, {{"Dump Registry", "win.dump-registry"}, {"About Composix", "win.about"}});
    bar->append_submenu("_Help", help);
    return bar;
}

}  // namespace composix

namespace composix {

void Shell::tab_hover(bool in, bool nav) {  // helper: a pull tab's shared hover
    auto& tab = nav ? m_nav_tab : m_insp_tab;
    auto& fade = nav ? m_nav_tab_fade : m_tab_fade;
    fade.disconnect();
    if (in) { tab.add_css_class("cx-lit"); return; }
    fade = Glib::signal_timeout().connect([&tab] { tab.remove_css_class("cx-lit"); return false; }, 250);
}

}  // namespace composix
