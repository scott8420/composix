#pragma once
// views::Inspector -- the right-hand pane (s002): what is selected, and its
// metadata. Multi-selection shows the fields the selection shares
// (core::shared_fields), "Mixed" where they differ.
//
// Read-only in s002: nothing here writes the model yet. When fields become
// editable, each edit is ONE journal step across the whole selection
// (REQUIREMENTS §10; RULES: undo from the first write).
#include "core/Outline.hpp"
#include "widgets/Widgets.hpp"

#include <string>
#include <vector>

namespace composix::views {

class Inspector : public widgets::Box {
public:
    Inspector();

    void show_items(const std::vector<const core::Item*>& sel);
    void show_section(const core::Outline& o, core::Section s);   // no tile selected
    void clear();

private:
    void fill(const std::string& title, const std::string& sub, const std::vector<core::Field>& fields);

    widgets::Label          m_title;
    widgets::Label          m_sub;
    widgets::ScrolledWindow m_scroll;
    widgets::Grid           m_fields;
    widgets::Label          m_empty;
};

}  // namespace composix::views
