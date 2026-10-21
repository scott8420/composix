#include "views/Navigator.hpp"
#include "views/Thumbs.hpp"
#include "views/WrapBox.hpp"
#include "Log.hpp"

#include <gtkmm/drawingarea.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/overlay.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widgetpaintable.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/drop.h>
#include <glibmm/main.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace composix::views {

namespace {

// The field `label`'s value on an item ("" when it hasn't one).
std::string field(const core::Item& it, const char* label) {
    for (const auto& f : it.fields)
        if (f.label == label) return f.value;
    return {};
}

// "A-Body" -> "A"; "None" -> "".
std::string prefix_of(const std::string& master) {
    if (master.empty() || master == "None" || master == "—") return {};
    const auto dash = master.find('-');
    return dash == std::string::npos ? master : master.substr(0, dash);
}

// s292: a story's first words as the card shows them -- its marks as words
// ("#" a page number), the rest of the private-use marks left out.
std::string readable(const std::string& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        const auto c = static_cast<unsigned char>(t[i]);
        if (c == 0xEE && i + 2 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0x80) {
            const auto k = static_cast<unsigned char>(t[i + 2]);
            if (k == 0x80 || k == 0x82 || k == 0x83) out += "#";   // a page number (this page's, a jump line's)
            else if (k == 0x81) out += "\u00a7";                  // the section's name
            i += 3;
            continue;
        }
        out += t[i++];
    }
    return out;
}

bool inside_button(Gtk::Widget* w, Gtk::Widget* stop) {
    for (; w && w != stop; w = w->get_parent())
        if (dynamic_cast<Gtk::Button*>(w)) return true;
    return false;
}

}  // namespace

Navigator::Navigator()
    : widgets::Box("nav", Gtk::Orientation::VERTICAL),
      m_scroll("nav.scroll"),
      m_cards("nav.cards", Gtk::Orientation::VERTICAL, 8) {
    add_css_class("cx-nav");
    m_cards.add_css_class("cx-cards");
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_cards);
    // s298: a filter over every card -- what's typed narrows each to the items whose names have it
    m_filter_entry = Gtk::make_managed<Gtk::SearchEntry>();
    m_filter_entry->set_name("nav.filter");
    m_filter_entry->set_placeholder_text("Filter pages, styles, colours\u2026");
    m_filter_entry->add_css_class("cx-nav-filter");
    m_filter_entry->signal_search_changed().connect([this] {
        std::string t = m_filter_entry->get_text();
        for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        if (t == m_filter) return;
        m_filter = t;
        apply_filter();
    });
    m_filter_entry->signal_stop_search().connect([this] { m_filter_entry->set_text(""); });
    append(*m_filter_entry);
    append(m_scroll);
}

Navigator::~Navigator() {
    for (auto& c : m_cardv) if (c.flow) c.flow->remove_all();
    for (auto& [seg, rs] : m_chip_rows) for (auto* r : rs) r->remove_all();
}

Navigator::Card* Navigator::card(core::Segment s) {
    for (auto& c : m_cardv)
        if (c.segment == s) return &c;
    return nullptr;
}
const Navigator::Card* Navigator::card(core::Segment s) const {
    for (const auto& c : m_cardv)
        if (c.segment == s) return &c;
    return nullptr;
}
Navigator::Card* Navigator::card_of(const std::string& id) {
    for (auto& c : m_cardv)
        if (std::find(c.ids.begin(), c.ids.end(), id) != c.ids.end()) return &c;
    return nullptr;
}

// ── building ─────────────────────────────────────────────────────────────────
void Navigator::set_outline(const core::Outline* o) {
    m_quiet = true;
    for (auto& c : m_cardv) if (c.flow) c.flow->remove_all();   // s285: a WrapBox lets its children go before it goes
    for (auto& [seg, rs] : m_chip_rows) for (auto* r : rs) r->remove_all();   // s287
    m_chip_rows.clear();
    while (auto* c = m_cards.get_first_child()) m_cards.remove(*c);
    m_cardv.clear();
    m_outline = o;
    if (o) {
        m_cardv.reserve(core::segments().size());   // cards are found by pointer while building
        for (const auto seg : m_order.empty() ? std::vector<core::Segment>{} : m_order) build_card(core::segment_info(seg), o->in(seg));
        for (const auto& si : core::segments())   // any not in the saved order, after
            if (std::find(m_order.begin(), m_order.end(), si.id) == m_order.end()) build_card(si, o->in(si.id));
    }
    // What was picked and is still here stays picked.
    std::vector<std::string> keep;
    for (const auto& id : m_picked)
        if (card_of(id)) keep.push_back(id);
    m_picked = keep;
    m_quiet = false;
    show_picks();
    if (!m_filter.empty()) apply_filter();   // s298
}

void Navigator::build_card(const core::SegmentInfo& si, const std::vector<const core::Item*>& items) {
    m_cardv.push_back({});
    Card& c = m_cardv.back();
    c.segment = si.id;
    const core::Segment s = si.id;

    c.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    c.box->set_name("nav.segment." + si.key);
    c.box->add_css_class("cx-card");

    c.head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    c.head->set_name("nav.segment." + si.key + ".head");
    c.head->add_css_class("cx-card-head");
    c.arrow = Gtk::make_managed<Gtk::Button>();
    c.arrow->set_name("nav.segment." + si.key + ".arrow");
    c.arrow->set_icon_name("pan-end-symbolic");
    c.arrow->set_has_frame(false);
    c.arrow->set_focus_on_click(false);
    c.arrow->add_css_class("cx-disclose");
    c.arrow->set_tooltip_text("Open / close");
    c.arrow->signal_clicked().connect([this, s] { set_open(s, !is_open(s)); });
    auto icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(si.icon);
    icon->add_css_class("cx-section-icon");
    auto title = Gtk::make_managed<Gtk::Label>(si.title);
    title->set_xalign(0.0f);
    title->add_css_class("cx-section-title");
    c.count = Gtk::make_managed<Gtk::Label>(std::to_string(items.size()));
    c.count->add_css_class("cx-count");
    c.count->set_valign(Gtk::Align::CENTER);
    auto fill = Gtk::make_managed<Gtk::Box>();
    fill->set_hexpand(true);
    c.head->append(*c.arrow);
    c.head->append(*icon);
    c.head->append(*title);
    c.head->append(*c.count);
    c.head->append(*fill);
    c.head->set_tooltip_text("Click: everything in " + si.title + " as tiles");

    if (s == core::Segment::Pages) {   // s277: the pictures' size
        auto seg = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        seg->set_name("nav.segment.pages.size");
        seg->add_css_class("cx-size-seg");
        seg->set_valign(Gtk::Align::CENTER);
        Gtk::ToggleButton* first = nullptr;
        struct Z { const char* key; const char* tip; int px; };
        static const Z zs[] = {{"S", "Small pictures", kSmall}, {"M", "Medium pictures", kMedium}, {"L", "Large pictures", kLarge}};
        for (const auto& z : zs) {
            auto b = Gtk::make_managed<Gtk::ToggleButton>(z.key);
            b->set_name(std::string("nav.segment.pages.size.") + z.key);
            b->set_tooltip_text(z.tip);
            b->set_focus_on_click(false);
            if (first) b->set_group(*first); else first = b;
            b->set_active(m_thumb_h == z.px);
            const int px = z.px;
            b->signal_toggled().connect([this, b, px] {
                if (m_quiet || !b->get_active() || px == m_thumb_h) return;
                set_thumb_height(px);
                m_thumb_sig.emit(px);
            });
            c.sizes[z.key] = b;
            seg->append(*b);
        }
        c.head->append(*seg);
    }

    // The header picks the section (not when the click was one of its buttons).
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    Gtk::Box* head = c.head;
    click->signal_released().connect([this, s, head](int, double x, double y) {
        if (inside_button(head->pick(x, y), head)) return;
        select_section(s);
    });
    c.head->add_controller(click);
    wire_context(*c.head, [s] { Pick p; p.segment = s; return p; });   // s279
    c.box->append(*c.head);

    c.body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    c.body->set_name("nav.segment." + si.key + ".body");
    c.body->add_css_class("cx-card-body");
    c.box->append(*c.body);

    for (const auto* it : items) c.ids.push_back(it->id);
    if (s == core::Segment::Pages || s == core::Segment::Masters) {
        build_pictures(c, items);
    } else if (s == core::Segment::Colors) {   // s287: the swatches as chips, a heading over each group
        build_chips(c, items);
    } else if (s == core::Segment::Assets) {   // s292: the pictures as small tiles
        build_assets(c, items);
    } else {
        c.list = Gtk::make_managed<Gtk::ListBox>();
        c.list->set_selection_mode(Gtk::SelectionMode::MULTIPLE);
        c.list->set_activate_on_single_click(false);
        c.list->add_css_class("cx-nav-list");
        for (const auto* it : items) {
            auto row = Gtk::make_managed<Gtk::ListBoxRow>();
            row->set_name("nav.item." + it->id);
            row->add_css_class("cx-item-row");
            auto ib = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto il = Gtk::make_managed<Gtk::Label>(it->title);
            il->set_xalign(0.0f);
            il->set_hexpand(true);
            il->set_ellipsize(Pango::EllipsizeMode::END);
            if (s == core::Segment::Styles) specimen(*il, *it);   // s287: the name set in the style
            if (s != core::Segment::Stories) ib->append(*il);
            if (s == core::Segment::Stories) {   // s292: its first words under its name
                auto col2 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
                col2->set_hexpand(true);
                col2->append(*il);   // (placed once: a managed label taken out of a box is gone)
                auto sm = Gtk::make_managed<Gtk::Label>(readable(it->thumb.sample));
                sm->set_xalign(0.0f);
                sm->set_ellipsize(Pango::EllipsizeMode::END);
                sm->set_width_chars(1);
                sm->add_css_class("cx-story-sample");
                sm->set_visible(!readable(it->thumb.sample).empty());
                col2->append(*sm);
                ib->append(*col2);
                c.samples[it->id] = sm;
            }
            auto b = Gtk::make_managed<Gtk::Label>(it->badge);   // always there, hidden when empty
            b->add_css_class("cx-badge");
            if (it->thumb.flag) b->add_css_class("cx-warn");
            b->set_visible(!it->badge.empty());
            ib->append(*b);
            row->set_child(*ib);
            c.list->append(*row);
            c.rows[it->id] = row;
            c.labels[it->id] = {il, b};
        }
        c.list->signal_selected_rows_changed().connect([this, s] {
            if (auto* cc = card(s)) on_list_changed(*cc);
        });
        c.body->append(*c.list);
    }
    if (s == core::Segment::Pages) wire_page_drop(c);
    wire_card_drag(c);
    show_open(c);
    m_cards.append(*c.box);
}

// Pages: the spreads, a FlowBox child each. Masters: each master's mock-up.
void Navigator::build_pictures(Card& c, const std::vector<const core::Item*>& items) {
    if (c.flow) { c.flow->remove_all(); c.body->remove(*c.flow); }   // s285: its children let go first
    c.cells.clear();
    c.spreads.clear();
    c.flow = Gtk::make_managed<WrapBox>(2, 4);
    c.flow->add_css_class("cx-card-pictures");
    // Each picture in a slot (its hover; the spacing round it).
    const auto slot = [&c](Gtk::Widget& w) {
        auto s = Gtk::make_managed<Gtk::Box>();
        s->add_css_class("cx-pic-slot");
        s->append(w);
        c.flow->append(*s);
    };
    std::map<std::string, const core::Item*> by_id;
    for (const auto* it : items) by_id[it->id] = it;

    if (c.segment == core::Segment::Masters) {
        for (const auto* it : items) {
            slot(*make_cell(c, *it, false, false));
        }
    } else {
        const auto groups = m_grouping ? m_grouping() : std::vector<std::vector<std::string>>{};
        c.spreads = core::spread_slots(c.ids, groups, [&](const std::string& id) {
            const auto f = by_id.find(id);
            return f != by_id.end() && field(*f->second, "Side") == "Left";
        });
        for (const auto& row : c.spreads) {
            // s278: the pages side by side, their folios in a row under them --
            // the folio row is the spread's own (click: the spread; drag: it moves).
            auto slot_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            auto spread = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            spread->add_css_class("cx-thumb-spread");
            auto folios = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
            folios->add_css_class("cx-spread-folios");
            std::vector<std::string> ids;
            double a = 2.0 / 3.0;
            for (const auto& o : row)
                if (auto g = by_id.find(o); g != by_id.end() && g->second->thumb.h_in > 0)
                    a = g->second->thumb.w_in / g->second->thumb.h_in;
            for (std::size_t k = 0; k < row.size(); ++k) {
                const auto f = by_id.find(row[k]);
                if (f == by_id.end()) {   // the empty side: a page's room, nothing drawn
                    const int w = static_cast<int>(std::lround(m_thumb_h * a));
                    auto gap = Gtk::make_managed<Gtk::Box>();
                    gap->set_size_request(w, m_thumb_h);
                    gap->set_valign(Gtk::Align::START);
                    spread->append(*gap);
                    auto none = Gtk::make_managed<Gtk::Label>();
                    none->set_size_request(w, -1);
                    folios->append(*none);
                    continue;
                }
                spread->append(*make_cell(c, *f->second, k == 0 && row.size() > 1, k + 1 == row.size() && row.size() > 1));
                auto& cell = c.cells[row[k]];
                folios->append(*cell.under);
                ids.push_back(row[k]);
            }
            folios->set_tooltip_text(ids.size() > 1 ? "Click: the spread  \u00b7  drag: move it" : "Click: the page  \u00b7  drag: move it");
            {   // click: the spread picked (Ctrl: added to / taken from the pick)
                auto click = Gtk::GestureClick::create();
                click->set_button(GDK_BUTTON_PRIMARY);
                auto* clk = click.get();
                click->signal_pressed().connect([this, ids, clk](int, double, double) {
                    auto* cc = card(core::Segment::Pages);
                    if (!cc || ids.empty()) return;
                    const bool ctrl = (clk->get_current_event_state() & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
                    std::vector<std::string> next = ctrl ? m_picked : std::vector<std::string>{};
                    const bool all_in = std::all_of(ids.begin(), ids.end(), [&](const std::string& i) { return std::find(next.begin(), next.end(), i) != next.end(); });
                    for (const auto& i : ids) {
                        auto f = std::find(next.begin(), next.end(), i);
                        if (ctrl && all_in) { if (f != next.end()) next.erase(f); }
                        else if (f == next.end()) next.push_back(i);
                    }
                    std::vector<std::string> ordered;
                    for (const auto& x : cc->ids)
                        if (std::find(next.begin(), next.end(), x) != next.end()) ordered.push_back(x);
                    m_picked = ordered;
                    m_anchor = ids.front();
                    m_seg_pick.reset();
                    show_picks();
                    emit_pick();
                });
                folios->add_controller(click);
            }
            wire_context(*folios, [this, ids] {   // s279: the spread
                bool in = !ids.empty();
                for (const auto& i : ids) in = in && std::find(m_picked.begin(), m_picked.end(), i) != m_picked.end();
                if (!in && !ids.empty()) {
                    m_picked = ids;
                    m_anchor = ids.front();
                    m_seg_pick.reset();
                    show_picks();
                    emit_pick();
                }
                Pick p;
                p.items = m_picked;
                return p;
            });
            {   // drag: the spread (s283: Ctrl -- a copy of it)
                auto src = Gtk::DragSource::create();
                src->set_actions(Gdk::DragAction::MOVE | Gdk::DragAction::COPY);
                std::string payload = "composix-pages:";
                for (std::size_t k = 0; k < ids.size(); ++k) payload += (k ? "," : "") + ids[k];
                src->signal_prepare().connect([payload](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                    Glib::Value<Glib::ustring> v;
                    v.init(v.value_type());
                    v.set(payload);
                    return Gdk::ContentProvider::create(v);
                }, false);
                auto* srcp = src.get();
                src->signal_drag_begin().connect([srcp, spread](const Glib::RefPtr<Gdk::Drag>&) {
                    srcp->set_icon(Gtk::WidgetPaintable::create(*spread), 12, 10);
                });
                folios->add_controller(src);
            }
            slot_box->append(*spread);
            slot_box->append(*folios);
            slot(*slot_box);
        }
    }
    c.body->append(*c.flow);
}

Gtk::Box* Navigator::make_cell(Card& c, const core::Item& it, bool left, bool right) {
    const bool page = c.segment == core::Segment::Pages;
    Cell cell;
    cell.id = it.id;
    cell.col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    cell.col->set_name("nav.item." + it.id);
    cell.col->add_css_class(page ? "cx-thumb-page" : "cx-thumb-master");
    if (left) cell.col->add_css_class("cx-left");
    if (right) cell.col->add_css_class("cx-right");
    cell.col->set_valign(Gtk::Align::START);
    auto over = Gtk::make_managed<Gtk::Overlay>();
    over->add_css_class("cx-thumb-paper");
    over->set_halign(Gtk::Align::CENTER);
    cell.da = Gtk::make_managed<Gtk::DrawingArea>();
    over->set_child(*cell.da);
    cell.badge = Gtk::make_managed<Gtk::Label>();
    cell.badge->add_css_class("cx-master-badge");
    cell.badge->set_valign(Gtk::Align::START);
    cell.badge->set_halign(left ? Gtk::Align::START : Gtk::Align::END);   // the outside corner
    cell.badge->set_can_target(false);
    over->add_overlay(*cell.badge);
    cell.col->append(*over);
    cell.under = Gtk::make_managed<Gtk::Label>();
    cell.under->add_css_class(page ? "cx-folio" : "cx-master-name");
    cell.under->set_ellipsize(Pango::EllipsizeMode::END);
    if (!page) cell.col->append(*cell.under);   // a page's folio goes under its spread (s278)

    Gtk::DrawingArea* da = cell.da;
    const std::string id = it.id;
    const core::Segment seg = c.segment;
    da->set_draw_func([this, da, id, seg](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        auto* cc = card(seg);
        if (!cc) return;
        const auto f = cc->cells.find(id);
        if (f == cc->cells.end()) return;
        paint_page_exact(cr, *da, w, h, f->second.thumb);
    });

    // A click picks it; Ctrl adds or takes away; Shift takes the run.
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    auto* clk = click.get();
    click->signal_pressed().connect([this, seg, id, clk](int, double, double) {
        const auto st = clk->get_current_event_state();
        const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
        const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
        if (auto* cc = card(seg)) click_item(*cc, id, ctrl, shift);
    });
    cell.col->add_controller(click);
    // s279: right-click -- a page is picked first (unless it's in the pick); a master stays as it is.
    wire_context(*cell.col, [this, seg, id, page] {
        Pick p;
        if (page && std::find(m_picked.begin(), m_picked.end(), id) == m_picked.end())
            if (auto* cc = card(seg)) click_item(*cc, id, false, false);
        p.items = page ? m_picked : std::vector<std::string>{id};
        return p;
    });

    if (!page) {   // s062: a master drags onto pages
        auto src = Gtk::DragSource::create();
        src->set_actions(Gdk::DragAction::COPY);
        src->signal_prepare().connect([id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
            Glib::Value<Glib::ustring> v;
            v.init(v.value_type());
            v.set("composix-master:" + id);
            return Gdk::ContentProvider::create(v);
        }, false);
        auto* srcp = src.get();
        src->signal_drag_begin().connect([srcp, over](const Glib::RefPtr<Gdk::Drag>&) {
            srcp->set_icon(Gtk::WidgetPaintable::create(*over), 12, 10);
        });
        cell.col->add_controller(src);
    } else {   // s278: a page drags -- the pick if it's among it, else itself (s283: Ctrl -- copies)
        auto src = Gtk::DragSource::create();
        src->set_actions(Gdk::DragAction::MOVE | Gdk::DragAction::COPY);
        src->signal_prepare().connect([this, id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
            const auto ids = drag_ids(id);
            std::string payload = "composix-pages:";
            for (std::size_t k = 0; k < ids.size(); ++k) payload += (k ? "," : "") + ids[k];
            Glib::Value<Glib::ustring> v;
            v.init(v.value_type());
            v.set(payload);
            return Gdk::ContentProvider::create(v);
        }, false);
        auto* srcp = src.get();
        src->signal_drag_begin().connect([srcp, over](const Glib::RefPtr<Gdk::Drag>&) {
            srcp->set_icon(Gtk::WidgetPaintable::create(*over), 12, 10);
        });
        cell.col->add_controller(src);
    }

    apply_cell(cell, it, page);
    size_cell(cell);
    Gtk::Box* col = cell.col;
    c.cells[it.id] = cell;
    return col;
}

void Navigator::apply_cell(Cell& cell, const core::Item& it, bool page) {
    cell.thumb = it.thumb;
    cell.aspect = it.thumb.h_in > 0 ? it.thumb.w_in / it.thumb.h_in : 2.0 / 3.0;
    if (page) {
        const std::string folio = field(it, "Folio");
        cell.under->set_text(folio);
        const std::string pre = prefix_of(field(it, "Master"));
        cell.badge->set_text(pre);
        cell.badge->set_visible(!pre.empty());
        cell.col->set_tooltip_text(it.title + (field(it, "Master").empty() ? "" : "  ·  " + field(it, "Master")) +
                                   (it.badge.empty() ? "" : "  ·  " + it.badge));
    } else {
        cell.under->set_text(it.title);
        cell.badge->set_visible(false);
        const std::string used = field(it, "Used by");
        cell.col->set_tooltip_text(it.title + (used.empty() ? "" : "  ·  used by " + used) +
                                   "\nDrag onto pages to apply it");
    }
    if (cell.da) cell.da->queue_draw();
}

void Navigator::size_cell(Cell& cell) {
    const int h = m_thumb_h;
    const int w = std::max(8, static_cast<int>(std::lround(h * cell.aspect)));
    cell.da->set_content_width(w);
    cell.da->set_content_height(h);
    cell.under->set_size_request(w, -1);
}

void Navigator::set_thumb_height(int px) {
    px = std::clamp(px, 32, 200);
    if (px == m_thumb_h) return;
    m_thumb_h = px;
    m_quiet = true;
    for (auto& c : m_cardv) {
        for (auto& [k, b] : c.sizes)
            b->set_active((k == "S" && px == kSmall) || (k == "M" && px == kMedium) || (k == "L" && px == kLarge));
        if (c.flow && m_outline && (c.segment == core::Segment::Pages || c.segment == core::Segment::Masters))
            build_pictures(c, m_outline->in(c.segment));
    }
    m_quiet = false;
    show_picks();
}

// ── open / closed ────────────────────────────────────────────────────────────
void Navigator::show_open(Card& c) {
    const bool open = m_open.count(c.segment) > 0;
    c.arrow->set_icon_name(open ? "pan-down-symbolic" : "pan-end-symbolic");
    c.body->set_visible(open && !c.ids.empty());
    if (open) c.box->add_css_class("cx-open");
    else      c.box->remove_css_class("cx-open");
}

bool Navigator::is_open(core::Segment s) const { return m_open.count(s) > 0; }

void Navigator::set_open(core::Segment s, bool open) {
    if (is_open(s) == open) return;
    if (open) m_open.insert(s);
    else      m_open.erase(s);
    if (auto* c = card(s)) show_open(*c);
    m_open_sig.emit();
}

std::string Navigator::open_keys() const {
    std::string out;
    for (const auto& si : core::segments())
        if (m_open.count(si.id)) out += (out.empty() ? "" : ",") + si.key;
    return out;
}

void Navigator::set_open_keys(const std::string& csv) {
    m_open.clear();
    std::stringstream ss(csv);
    std::string k;
    while (std::getline(ss, k, ','))
        for (const auto& si : core::segments())
            if (si.key == k) m_open.insert(si.id);
    for (auto& c : m_cardv) show_open(c);
}

// ── picking ──────────────────────────────────────────────────────────────────
void Navigator::show_picks() {
    const bool was = m_quiet;
    m_quiet = true;
    for (auto& c : m_cardv) {
        if (m_seg_pick && *m_seg_pick == c.segment && m_picked.empty()) c.head->add_css_class("cx-picked");
        else                                                           c.head->remove_css_class("cx-picked");
        if (c.list) {
            c.list->unselect_all();
            for (const auto& id : m_picked)
                if (auto r = c.rows.find(id); r != c.rows.end()) c.list->select_row(*r->second);
        }
        for (auto& [id, cell] : c.cells) {
            const bool on = std::find(m_picked.begin(), m_picked.end(), id) != m_picked.end();
            if (on) cell.col->add_css_class("cx-picked");
            else    cell.col->remove_css_class("cx-picked");
        }
    }
    m_quiet = was;
}

void Navigator::emit_pick() {
    Pick p;
    p.items = m_picked;
    if (p.items.empty()) p.segment = m_seg_pick;
    if (auto lg = log::get(log::Area::View))
        lg->info("nav pick: {} {}", p.segment ? core::segment_info(*p.segment).key : "-", p.items.size());
    m_pick.emit(p);
}

void Navigator::click_item(Card& c, const std::string& id, bool ctrl, bool shift) {
    // A pick lives in one card: starting in another card starts afresh.
    const bool same = !m_picked.empty() && card_of(m_picked.front()) == &c;
    std::vector<std::string> next = same ? m_picked : std::vector<std::string>{};
    if (shift && same && !m_anchor.empty() && card_of(m_anchor) == &c) {
        auto a = std::find(c.ids.begin(), c.ids.end(), m_anchor);
        auto b = std::find(c.ids.begin(), c.ids.end(), id);
        if (a > b) std::swap(a, b);
        next.assign(a, b + 1);
    } else if (ctrl) {
        auto f = std::find(next.begin(), next.end(), id);
        if (f != next.end()) next.erase(f);
        else                 next.push_back(id);
        m_anchor = id;
    } else {
        next = {id};
        m_anchor = id;
    }
    // In the card's order, so a pick reads top to bottom.
    std::vector<std::string> ordered;
    for (const auto& x : c.ids)
        if (std::find(next.begin(), next.end(), x) != next.end()) ordered.push_back(x);
    m_picked = ordered;
    m_seg_pick.reset();
    show_picks();
    emit_pick();
}

void Navigator::on_list_changed(Card& c) {
    if (m_quiet) return;
    std::vector<std::string> next;
    for (const auto& id : c.ids)
        if (auto r = c.rows.find(id); r != c.rows.end() && r->second->is_selected()) next.push_back(id);
    m_picked = next;
    m_seg_pick.reset();
    show_picks();   // the other cards let go
    emit_pick();
}

void Navigator::select_section(core::Segment s) {
    m_picked.clear();
    m_seg_pick = s;
    show_picks();
    emit_pick();
}

void Navigator::select_items(const std::vector<std::string>& ids) {
    std::vector<std::string> next;
    std::string first;
    for (auto& c : m_cardv) {
        bool any = false;
        for (const auto& id : c.ids)
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
                next.push_back(id);
                any = true;
                if (first.empty()) first = id;
            }
        if (any && !is_open(c.segment)) {   // open the card so the pick shows
            m_open.insert(c.segment);
            show_open(c);
            m_open_sig.emit();
        }
    }
    m_picked = next;
    m_seg_pick.reset();
    if (!first.empty()) m_anchor = first;
    show_picks();
    if (!first.empty()) {
        Glib::signal_timeout().connect_once([this, first] {
            auto* c = card_of(first);
            if (!c) return;
            if (auto f = c->cells.find(first); f != c->cells.end()) scroll_to(*f->second.col);
            else if (auto r = c->rows.find(first); r != c->rows.end()) scroll_to(*r->second);
        }, 60);
    }
    emit_pick();
}

void Navigator::scroll_to(Gtk::Widget& w) {
    graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(w.gobj(), GTK_WIDGET(m_cards.gobj()), &in, &out)) return;
    auto adj = m_scroll.get_vadjustment();
    const double top = out.y, bottom = out.y + w.get_height() + 24;   // + the folios under it
    const double v = adj->get_value(), page = adj->get_page_size();
    if (top < v || bottom > v + page)   // out of view (even in part): brought to the middle
        adj->set_value(std::clamp(top - (page - (bottom - top)) / 2, 0.0, std::max(0.0, adj->get_upper() - page)));
}

// ── s278: drag and drop ─────────────────────────────────────────────────────
std::vector<std::string> Navigator::drag_ids(const std::string& id) {
    auto* c = card(core::Segment::Pages);
    if (c && std::find(m_picked.begin(), m_picked.end(), id) != m_picked.end()) {
        std::vector<std::string> out;
        for (const auto& p : m_picked) if (c->cells.count(p)) out.push_back(p);
        if (!out.empty()) return out;
    }
    return {id};
}

// The page nearest (x, y) in the Pages card's pictures, and where: a master
// takes the middle half (onto the page) and the outer quarters (a gap); pages
// take the halves (before / after).
Navigator::Hit Navigator::hit_page(Card& c, double x, double y, bool master) {
    Hit best;
    double bd = 1e18;
    for (auto& [id, cell] : c.cells) {
        graphene_rect_t r;
        auto* paper = cell.da->get_parent();
        if (!paper || !gtk_widget_compute_bounds(paper->gobj(), GTK_WIDGET(c.flow->gobj()), &r)) continue;
        const double dx = x < r.origin.x ? r.origin.x - x : x > r.origin.x + r.size.width ? x - (r.origin.x + r.size.width) : 0;
        const double dy = y < r.origin.y ? r.origin.y - y : y > r.origin.y + r.size.height + 18 ? y - (r.origin.y + r.size.height + 18) : 0;
        const double d = dx * dx + dy * dy * 4;   // lines count more than columns
        if (d < bd) {
            bd = d;
            const double fx = r.size.width > 0 ? (x - r.origin.x) / r.size.width : 0.5;
            best.page = id;
            if (master) best.zone = fx < 0.25 ? -1 : fx > 0.75 ? 1 : 0;
            else        best.zone = fx < 0.5 ? -1 : 1;
        }
    }
    return best;
}

void Navigator::mark_drop(Card& c, const Hit& h) {
    for (auto& [id, cell] : c.cells) {
        for (const char* k : {"cx-drop-on", "cx-drop-before", "cx-drop-after"}) cell.col->remove_css_class(k);
        if (id != h.page) continue;
        cell.col->add_css_class(h.zone < 0 ? "cx-drop-before" : h.zone > 0 ? "cx-drop-after" : "cx-drop-on");
    }
}

void Navigator::wire_page_drop(Card& c) {
    auto drop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::MOVE | Gdk::DragAction::COPY);
    drop->set_preload(true);
    auto* dp = drop.get();
    const auto text_of = [dp]() -> std::string {
        const auto& v = dp->get_value();
        if (!G_VALUE_HOLDS_STRING(v.gobj())) return {};
        const char* t = g_value_get_string(v.gobj());
        return t ? t : "";
    };
    drop->signal_motion().connect([this, text_of, dp](double x, double y) {
        auto* cc = card(core::Segment::Pages);
        if (!cc || !cc->flow) return Gdk::DragAction{};
        const std::string t = text_of();
        const bool master = t.rfind("composix-master:", 0) == 0;
        if (!master && t.rfind("composix-pages:", 0) != 0) { mark_drop(*cc, {}); return Gdk::DragAction{}; }
        auto h = hit_page(*cc, x, y, master);
        mark_drop(*cc, h);
        const bool ctrl = (dp->get_current_event_state() & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
        return master || ctrl ? Gdk::DragAction::COPY : Gdk::DragAction::MOVE;
    }, false);
    drop->signal_leave().connect([this] { if (auto* cc = card(core::Segment::Pages)) mark_drop(*cc, {}); });
    drop->signal_drop().connect([this, dp](const Glib::ValueBase& value, double x, double y) {
        // s283: Ctrl held -- the drag offers a copy only (GDK's choice from the keys), or the state says so.
        bool copy = (dp->get_current_event_state() & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
        if (auto drp = dp->get_current_drop()) {
            const auto acts = drp->get_actions();
            if ((acts & Gdk::DragAction::COPY) == Gdk::DragAction::COPY && (acts & Gdk::DragAction::MOVE) != Gdk::DragAction::MOVE) copy = true;
            if (auto lg = log::get(log::Area::View)) lg->info("nav drop: actions {} -> {}", static_cast<int>(acts), copy ? "copy" : "move");
        }
        auto* cc = card(core::Segment::Pages);
        if (!cc || !cc->flow) return false;
        mark_drop(*cc, {});
        Glib::Value<Glib::ustring> v;
        v.init(value.gobj());
        const std::string text = v.get();
        const std::string mt = "composix-master:", pt = "composix-pages:";
        if (text.rfind(mt, 0) == 0) {
            const auto h = hit_page(*cc, x, y, true);
            if (h.page.empty()) return false;
            const std::string master = text.substr(mt.size());
            if (h.zone == 0) {   // onto the page: the pick if it's among it (s062)
                const auto pages = drag_ids(h.page);
                Glib::signal_idle().connect_once([this, master, pages] { m_master_drop.emit(master, pages); });
            } else {
                const std::string page = h.page;
                const bool before = h.zone < 0;
                Glib::signal_idle().connect_once([this, master, page, before] { m_insert_sig.emit(master, page, before); });
            }
            return true;
        }
        if (text.rfind(pt, 0) != 0) return false;
        std::vector<std::string> ids;
        std::stringstream ss(text.substr(pt.size()));
        std::string one;
        while (std::getline(ss, one, ',')) if (!one.empty()) ids.push_back(one);
        const auto h = hit_page(*cc, x, y, false);
        if (h.page.empty() || ids.empty() || (!copy && std::find(ids.begin(), ids.end(), h.page) != ids.end())) return false;
        const std::string page = h.page;
        const bool before = h.zone < 0;
        Glib::signal_idle().connect_once([this, ids, page, before, copy] { m_move_sig.emit(ids, page, before, copy); });
        return true;
    }, false);
    c.body->add_controller(drop);
}

// A card's header drags it up or down the column.
void Navigator::wire_card_drag(Card& c) {
    const std::string key = core::segment_info(c.segment).key;
    auto src = Gtk::DragSource::create();
    src->set_actions(Gdk::DragAction::MOVE);
    src->signal_prepare().connect([key](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
        Glib::Value<Glib::ustring> v;
        v.init(v.value_type());
        v.set("composix-card:" + key);
        return Gdk::ContentProvider::create(v);
    }, false);
    auto* srcp = src.get();
    Gtk::Box* head = c.head;
    src->signal_drag_begin().connect([srcp, head](const Glib::RefPtr<Gdk::Drag>&) {
        srcp->set_icon(Gtk::WidgetPaintable::create(*head), 20, 14);
    });
    c.head->add_controller(src);

    auto drop = Gtk::DropTarget::create(G_TYPE_STRING, Gdk::DragAction::MOVE);
    drop->set_preload(true);
    auto* dp = drop.get();
    Gtk::Box* box = c.box;
    const auto is_card = [dp] {
        const auto& v = dp->get_value();
        if (!G_VALUE_HOLDS_STRING(v.gobj())) return false;
        const char* t = g_value_get_string(v.gobj());
        return t && std::string(t).rfind("composix-card:", 0) == 0;
    };
    drop->signal_motion().connect([box, is_card](double, double y) {
        if (!is_card()) return Gdk::DragAction{};
        const bool above = y < box->get_height() / 2.0;
        box->remove_css_class(above ? "cx-card-drop-after" : "cx-card-drop-before");
        box->add_css_class(above ? "cx-card-drop-before" : "cx-card-drop-after");
        return Gdk::DragAction::MOVE;
    }, false);
    drop->signal_leave().connect([box] { box->remove_css_class("cx-card-drop-before"); box->remove_css_class("cx-card-drop-after"); });
    const core::Segment target = c.segment;
    drop->signal_drop().connect([this, box, target](const Glib::ValueBase& value, double, double y) {
        box->remove_css_class("cx-card-drop-before");
        box->remove_css_class("cx-card-drop-after");
        Glib::Value<Glib::ustring> v;
        v.init(value.gobj());
        const std::string t = v.get(), tag = "composix-card:";
        if (t.rfind(tag, 0) != 0) return false;
        const std::string key = t.substr(tag.size());
        std::optional<core::Segment> moving;
        for (const auto& si : core::segments()) if (si.key == key) moving = si.id;
        if (!moving || *moving == target) return false;
        const bool above = y < box->get_height() / 2.0;
        // The order as shown, then the card taken out and put back by the target.
        std::vector<core::Segment> order;
        for (const auto& cc : m_cardv) order.push_back(cc.segment);
        order.erase(std::remove(order.begin(), order.end(), *moving), order.end());
        auto at = std::find(order.begin(), order.end(), target);
        if (!above && at != order.end()) ++at;
        order.insert(at, *moving);
        const core::Segment mv = *moving;
        Glib::signal_idle().connect_once([this, order, mv] {
            m_order = order;
            // Move the card's box in place (no rebuild: what's open and drawn stays).
            Gtk::Widget* prev = nullptr;
            for (const auto seg : m_order) {
                if (auto* cc = card(seg)) {
                    if (seg == mv) {
                        if (prev) m_cards.reorder_child_after(*cc->box, *prev);
                        else      m_cards.reorder_child_at_start(*cc->box);
                    }
                    prev = cc->box;
                }
            }
            // Keep m_cardv in the shown order too.
            std::stable_sort(m_cardv.begin(), m_cardv.end(), [this](const Card& a, const Card& b) {
                return std::find(m_order.begin(), m_order.end(), a.segment) < std::find(m_order.begin(), m_order.end(), b.segment);
            });
            if (auto lg = log::get(log::Area::View)) lg->info("nav: cards {}", order_keys());
            m_order_sig.emit();
        });
        return true;
    }, false);
    c.box->add_controller(drop);
}

// s287: a style's name set in its own face -- family, weight, slant, its size
// (kept between 9 and 16 pt, so the card stays a list).
void Navigator::specimen(Gtk::Label& l, const core::Item& it) {
    if (it.thumb.kind != core::ThumbKind::Style) return;
    Pango::FontDescription fd;
    if (!it.thumb.family.empty()) fd.set_family(it.thumb.family);
    if (it.thumb.bold) fd.set_weight(Pango::Weight::BOLD);
    if (it.thumb.italic) fd.set_style(Pango::Style::ITALIC);
    double pt = 0;
    for (const auto& f : it.fields) if (f.label == "Size") pt = std::atof(f.value.c_str());
    if (pt > 0) fd.set_size(static_cast<int>(std::clamp(pt, 9.0, 16.0) * PANGO_SCALE));
    auto attrs = Pango::AttrList();
    auto a = Pango::Attribute::create_attr_font_desc(fd);
    attrs.insert(a);
    l.set_attributes(attrs);
    l.add_css_class("cx-specimen");
}

// s292: Links & Assets -- each picture a small tile (the picture itself, as the
// tile view draws it), its name under, a Missing / Modified word in warning ink.
void Navigator::build_assets(Card& c, const std::vector<const core::Item*>& items) {
    if (c.flow) { c.flow->remove_all(); c.body->remove(*c.flow); }
    c.cells.clear();
    c.flow = Gtk::make_managed<WrapBox>(6, 8);
    c.flow->add_css_class("cx-asset-tiles");
    for (const auto* it : items) {
        Cell cell;
        cell.id = it->id;
        cell.thumb = it->thumb;
        cell.col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        cell.col->set_name("nav.item." + it->id);
        cell.col->add_css_class("cx-asset-tile");
        cell.da = Gtk::make_managed<Gtk::DrawingArea>();
        cell.da->set_content_width(76);
        cell.da->set_content_height(56);
        Gtk::DrawingArea* da = cell.da;
        const std::string id = it->id;
        const core::Segment seg = c.segment;
        da->set_draw_func([this, da, id, seg](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            auto* cc = card(seg);
            if (!cc) return;
            if (auto f = cc->cells.find(id); f != cc->cells.end()) paint_thumb(cr, *da, w, h, f->second.thumb, false);
        });
        cell.col->append(*da);
        cell.under = Gtk::make_managed<Gtk::Label>(it->title);
        cell.under->add_css_class("cx-master-name");
        cell.under->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        cell.under->set_size_request(76, -1);
        cell.under->set_max_width_chars(1);
        cell.col->append(*cell.under);
        cell.badge = Gtk::make_managed<Gtk::Label>(it->badge);
        cell.badge->add_css_class("cx-asset-badge");
        cell.badge->set_visible(!it->badge.empty());
        cell.col->append(*cell.badge);
        cell.col->set_tooltip_text(it->title + (it->badge.empty() ? "" : "  \u00b7  " + it->badge));
        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_PRIMARY);
        auto* clk = click.get();
        click->signal_pressed().connect([this, seg, id, clk](int, double, double) {
            const auto st = clk->get_current_event_state();
            const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
            const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
            if (auto* cc = card(seg)) click_item(*cc, id, ctrl, shift);
        });
        cell.col->add_controller(click);
        c.flow->append(*cell.col);
        c.cells[it->id] = cell;
    }
    c.body->append(*c.flow);
}

void Navigator::build_chips(Card& c, const std::vector<const core::Item*>& items) {
    if (c.flow) c.flow->remove_all();
    for (auto* r : m_chip_rows[c.segment]) r->remove_all();
    if (c.chips) c.body->remove(*c.chips);
    c.chips = nullptr;
    c.flow = nullptr;
    c.cells.clear();
    c.chip_heads.clear();
    auto col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    col->add_css_class("cx-chips");
    c.flow = Gtk::make_managed<WrapBox>(5, 5);
    std::string group = "\x01";
    std::vector<std::pair<std::string, WrapBox*>> rows;
    WrapBox* row = nullptr;
    for (const auto* it : items) {
        if (!row || it->group != group) {   // a group's heading, then its chips
            group = it->group;
            if (!group.empty()) {
                auto h = Gtk::make_managed<Gtk::Label>(group);
                h->set_xalign(0.0f);
                h->add_css_class("cx-chip-group");
                col->append(*h);
            }
            row = rows.empty() ? c.flow : Gtk::make_managed<WrapBox>(5, 5);
            rows.push_back({group, row});
            col->append(*row);
            if (!group.empty()) c.chip_heads.push_back({col->get_last_child()->get_prev_sibling(), row});   // s298
        }
        Cell cell;
        cell.id = it->id;
        cell.thumb = it->thumb;
        cell.col = Gtk::make_managed<Gtk::Box>();
        cell.col->set_name("nav.item." + it->id);
        cell.col->add_css_class("cx-chip");
        cell.da = Gtk::make_managed<Gtk::DrawingArea>();
        cell.da->set_content_width(22);
        cell.da->set_content_height(22);
        const std::uint32_t rgb = it->thumb.rgb;
        const bool flag = it->thumb.flag || field(*it, "Type") == "Spot";   // a spot ink: its corner turned
        cell.da->set_draw_func([rgb, flag](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            const double r = 5;
            cr->begin_new_sub_path();
            cr->arc(w - r, r, r, -M_PI / 2, 0);
            cr->arc(w - r, h - r, r, 0, M_PI / 2);
            cr->arc(r, h - r, r, M_PI / 2, M_PI);
            cr->arc(r, r, r, M_PI, 3 * M_PI / 2);
            cr->close_path();
            cr->set_source_rgb(((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
            cr->fill_preserve();
            cr->set_source_rgba(0, 0, 0, 0.18);
            cr->set_line_width(1);
            cr->stroke();
            if (flag) {   // a spot colour: a corner turned down
                cr->move_to(w - 8, h);
                cr->line_to(w, h - 8);
                cr->line_to(w, h);
                cr->close_path();
                cr->set_source_rgba(1, 1, 1, 0.85);
                cr->fill();
            }
        });
        cell.col->append(*cell.da);
        cell.under = Gtk::make_managed<Gtk::Label>();
        cell.badge = Gtk::make_managed<Gtk::Label>();
        cell.col->set_tooltip_text(it->title + (it->badge.empty() ? "" : "  \u00b7  " + it->badge));
        const std::string id = it->id;
        const core::Segment seg = c.segment;
        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_PRIMARY);
        auto* clk = click.get();
        click->signal_pressed().connect([this, seg, id, clk](int, double, double) {
            const auto st = clk->get_current_event_state();
            const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;
            const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) == Gdk::ModifierType::SHIFT_MASK;
            if (auto* cc = card(seg)) click_item(*cc, id, ctrl, shift);
        });
        cell.col->add_controller(click);
        row->append(*cell.col);
        c.cells[it->id] = cell;
    }
    // The extra rows live in `col`; the card keeps the first as its flow (let go with the rest).
    m_chip_rows[c.segment].clear();
    for (std::size_t k = 1; k < rows.size(); ++k) m_chip_rows[c.segment].push_back(rows[k].second);
    c.chips = col;
    c.body->append(*col);
}

// s298: the filter -- an item shows when its name (a page: its folio and
// master too; a swatch: its group) has what's typed; a spread when either
// page does. A card with nothing showing goes; one with something opens
// (while filtering -- what the user left open or shut is kept).
void Navigator::apply_filter() {
    const auto lower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const auto matches = [&](const std::string& id) {
        if (m_filter.empty()) return true;
        const auto* it = m_outline ? m_outline->find(id) : nullptr;
        if (!it) return false;
        std::string hay = it->title + " " + it->badge + " " + it->group;
        if (it->segment == core::Segment::Pages) hay += " " + field(*it, "Folio") + " " + field(*it, "Master");
        if (it->segment == core::Segment::Stories) hay += " " + it->thumb.sample;
        return lower(hay).find(m_filter) != std::string::npos;
    };
    for (auto& c : m_cardv) {
        int shown = 0;
        for (const auto& [id, row] : c.rows) {
            const bool on = matches(id);
            row->set_visible(on);
            shown += on;
        }
        // pictures / chips / tiles: the slot holding the cell (a spread: either page)
        std::map<Gtk::Widget*, bool> slot_on;
        for (auto& [id, cell] : c.cells) {
            Gtk::Widget* w = cell.col;
            while (w && w->get_parent() && w->get_parent() != c.flow &&
                   std::find_if(m_chip_rows[c.segment].begin(), m_chip_rows[c.segment].end(), [&](WrapBox* r) { return r == w->get_parent(); }) ==
                       m_chip_rows[c.segment].end())
                w = w->get_parent();
            const bool on = matches(id);
            slot_on[w] = slot_on[w] || on;
            shown += on;
        }
        for (auto& [w, on] : slot_on) w->set_visible(on);
        for (auto& [head, row] : c.chip_heads) {   // s298: a group's heading goes with its last chip
            bool any = false;
            for (auto* k : row->children()) any = any || k->get_visible();
            head->set_visible(any);
            row->set_visible(any);
        }
        const bool filtering = !m_filter.empty();
        c.box->set_visible(!filtering || shown > 0);
        if (filtering) {
            c.body->set_visible(shown > 0);
            c.arrow->set_icon_name(shown > 0 ? "pan-down-symbolic" : "pan-end-symbolic");   // shown open while filtering
            c.count->set_text(std::to_string(shown) + " of " + std::to_string(c.ids.size()));
        } else {
            c.count->set_text(std::to_string(c.ids.size()));
            show_open(c);
        }
    }
}

void Navigator::wire_context(Gtk::Widget& w, std::function<Pick()> what) {
    auto rc = Gtk::GestureClick::create();
    rc->set_button(GDK_BUTTON_SECONDARY);
    Gtk::Widget* wp = &w;
    rc->signal_pressed().connect([this, wp, what](int, double x, double y) {
        const Pick p = what();
        graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
        if (!gtk_widget_compute_point(wp->gobj(), GTK_WIDGET(gobj()), &in, &out)) return;
        m_ctx_sig.emit(p, out.x, out.y);
    });
    w.add_controller(rc);
}

void Navigator::open_all(bool open) {
    m_open.clear();
    if (open) for (const auto& si : core::segments()) m_open.insert(si.id);
    for (auto& c : m_cardv) show_open(c);
    m_open_sig.emit();
}

void Navigator::reset_order() {
    m_order.clear();
    if (m_outline) set_outline(m_outline);
    m_order_sig.emit();
}

std::string Navigator::order_keys() const {
    std::string out;
    for (const auto& c : m_cardv) out += (out.empty() ? "" : ",") + core::segment_info(c.segment).key;
    return out;
}

void Navigator::set_order_keys(const std::string& csv) {
    m_order.clear();
    std::stringstream ss(csv);
    std::string k;
    while (std::getline(ss, k, ','))
        for (const auto& si : core::segments())
            if (si.key == k && std::find(m_order.begin(), m_order.end(), si.id) == m_order.end()) m_order.push_back(si.id);
    if (m_outline) set_outline(m_outline);
}

// ── the document changed ─────────────────────────────────────────────────────
void Navigator::refresh(const core::Outline* o) {
    bool same = o && m_outline && !m_cardv.empty();
    if (same)
        for (const auto& c : m_cardv) {
            const auto items = o->in(c.segment);
            if (items.size() != c.ids.size()) { same = false; break; }
            for (std::size_t i = 0; i < items.size() && same; ++i) same = c.ids[i] == items[i]->id;
            if (!same) break;
        }
    if (!same) {
        set_outline(o);
        return;
    }
    m_outline = o;
    for (auto& c : m_cardv) {
        const auto items = o->in(c.segment);
        c.count->set_text(std::to_string(items.size()));
        if (c.segment == core::Segment::Colors) {   // s287: chips drawn again (a colour, a group, may have changed)
            build_chips(c, items);
            continue;
        }
        if (c.segment == core::Segment::Assets) {   // s292: the tiles drawn again (a picture relinked, a badge)
            build_assets(c, items);
            continue;
        }
        if (c.segment == core::Segment::Pages) {
            // Sides may have moved (a page added before, a section): the spreads again.
            const auto groups = m_grouping ? m_grouping() : std::vector<std::vector<std::string>>{};
            std::map<std::string, const core::Item*> by_id;
            for (const auto* it : items) by_id[it->id] = it;
            const auto slots = core::spread_slots(c.ids, groups, [&](const std::string& id) {
                const auto f = by_id.find(id);
                return f != by_id.end() && field(*f->second, "Side") == "Left";
            });
            if (slots != c.spreads) {
                build_pictures(c, items);
                continue;
            }
        }
        for (const auto* it : items) {
            if (auto f = c.cells.find(it->id); f != c.cells.end()) {
                const double was = f->second.aspect;
                apply_cell(f->second, *it, c.segment == core::Segment::Pages);
                if (std::abs(was - f->second.aspect) > 1e-6) size_cell(f->second);
            }
            if (auto l = c.labels.find(it->id); l != c.labels.end()) {
                auto& [title, badge] = l->second;
                if (title->get_text().raw() != it->title) title->set_text(it->title);
                if (c.segment == core::Segment::Styles) specimen(*title, *it);   // s287: its face may have changed
                if (auto sm = c.samples.find(it->id); sm != c.samples.end()) {   // s292
                    sm->second->set_text(readable(it->thumb.sample));
                    sm->second->set_visible(!readable(it->thumb.sample).empty());
                }
                badge->set_text(it->badge);
                badge->set_visible(!it->badge.empty());
                if (it->thumb.flag) badge->add_css_class("cx-warn");
                else                badge->remove_css_class("cx-warn");
            }
        }
    }
    show_picks();
    if (!m_filter.empty()) apply_filter();   // s298
}

std::string Navigator::spreads_text() const {
    std::string out;
    if (const auto* c = card(core::Segment::Pages))
        for (const auto& row : c->spreads) {
            if (!out.empty()) out += ' ';
            out += '[';
            for (std::size_t k = 0; k < row.size(); ++k) out += (k ? "|" : "") + (row[k].empty() ? std::string("-") : row[k]);
            out += ']';
        }
    return out;
}

}  // namespace composix::views
