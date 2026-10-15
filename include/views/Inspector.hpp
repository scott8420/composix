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

#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace composix::views {

class Inspector : public widgets::Box {
public:
    Inspector();

    void show_items(const std::vector<const core::Item*>& sel);
    void show_section(const core::Outline& o, core::Segment s);   // no tile selected
    void clear();
    // Any fields under any title (s005: frames picked on the canvas).
    void show(const std::string& title, const std::string& sub, const std::vector<core::Field>& fields) {
        fill(title, sub, fields);
    }

    sigc::signal<void(const std::string& key, const std::string& choice)>& signal_edit() { return m_edit; }

private:
    void fill(const std::string& title, const std::string& sub, const std::vector<core::Field>& fields);

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
};

}  // namespace composix::views
