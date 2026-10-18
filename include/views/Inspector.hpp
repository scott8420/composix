#pragma once
// views::Inspector -- the right-hand pane (s002): what is selected, and its
// metadata. Multi-selection shows the fields the selection shares
// (core::shared_fields), "Mixed" where they differ.
//
// s004: a field that carries an `edit` key shows as a dropdown of its
// choices. Picking one emits signal_edit(key, choice id); the shell turns that
// into ONE verb -- one journal step -- across the whole selection
// (REQUIREMENTS §10; RULES: undo from the first write). A Mixed field shows
// no choice until one is picked.
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/entry.h>
#include <sigc++/signal.h>
#include <functional>

#include <string>
#include <vector>

#include <set>
#include <map>
#include <utility>

namespace composix::views {

class Inspector : public widgets::Box {
public:
    Inspector();

    void show_items(const std::vector<const core::Item*>& sel);
    void show_section(const core::Outline& o, core::Segment s,
                      const std::vector<core::Field>& extra = {});   // no tile selected (s097: plus the section's own rows)
    void clear();
    // Any fields under any title (s005: frames picked on the canvas).
    void show(const std::string& title, const std::string& sub, const std::vector<core::Field>& fields) {
        fill(title, sub, fields);
    }

    sigc::signal<void(const std::string& key, const std::string& choice)>& signal_edit() { return m_edit; }
    // s162 (Scott): a style's settings are STAGED -- changed rows wait until
    // Set (all of them, one step, on the styles they were made for); Reset
    // drops them. Leaving the styles with changes waiting: they're set.
    using Pending = std::vector<std::pair<std::string, std::string>>;
    sigc::signal<void(const std::vector<std::string>& ids, const Pending& edits)>& signal_apply() { return m_apply; }
    bool has_pending() const { return !m_pending.empty(); }
    // s164 (Scott): what's waiting, for a live specimen -- and a word when it changes.
    const Pending& pending() const { return m_pending; }
    const std::vector<std::string>& pending_ids() const { return m_stage_ids; }
    sigc::signal<void()>& signal_staged() { return m_staged_sig; }
    // s165 (Scott): leaving changes waiting -- a dialog: Accept sets them,
    // Cancel drops them; then `then` (none waiting: `then` at once).
    void settle_pending(std::function<void()> then);
    // s161: the groups shown open ("Text,Paragraph"), kept by the shell.
    void set_open_groups(const std::string& csv);
    sigc::signal<void(const std::string& csv)>& signal_groups() { return m_groups_sig; }

private:
    void fill(const std::string& title, const std::string& sub, const std::vector<core::Field>& fields);
    Gtk::MenuButton* presets_button(const std::string& key, const std::vector<core::Choice>& choices, Gtk::Entry* show_in = nullptr);
    void send(const std::string& key, const std::string& value);   // s162: an edit -- now, or staged
    void update_bar();
    void ask_pending(std::vector<std::string> ids, Pending pend, const std::string& name, std::function<void()> then);
    Gtk::Entry* typed_entry(const std::string& name, const std::string& value, double unit, double lo, double hi,
                            std::function<void(const std::string&)> send, bool percent = false);

    widgets::Label          m_title;
    widgets::Label          m_sub;
    widgets::ScrolledWindow m_scroll;
    widgets::Grid           m_fields;
    widgets::Label          m_empty;
    bool                    m_painting = false;   // filling the fields is not a user choice
    // What is shown: a refresh that changes nothing rebuilds nothing (s009 --
    // a rebuild destroys an OPEN dropdown, so its popup closed as it opened).
    std::string             m_shown_title, m_shown_sub;
    std::vector<core::Field> m_shown;
    bool                    m_has_shown = false;
    sigc::signal<void(const std::string&, const std::string&)> m_edit;
    // s161: groups
    std::set<std::string> m_open{"Text", "Paragraph"};
    sigc::signal<void(const std::string&)> m_groups_sig;
    // s162: staged edits
    bool m_staged = false, m_staged_next = false;
    std::vector<std::string> m_stage_ids, m_stage_ids_next;
    Pending m_pending;
    sigc::signal<void()> m_staged_sig;   // s164
    std::map<std::string, Gtk::Widget*> m_row_label;   // an edit key -> its row's label (marked when staged)
    sigc::signal<void(const std::vector<std::string>&, const Pending&)> m_apply;
    Gtk::Box*   m_bar = nullptr;
    Gtk::Label* m_bar_text = nullptr;
};

}  // namespace composix::views
