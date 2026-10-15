#include "core/Outline.hpp"

#include <algorithm>
#include <stdexcept>

namespace composix::core {

const std::vector<SegmentInfo>& segments() {
    static const std::vector<SegmentInfo> k = {
        {Segment::Document,  "document",  "Document",        "x-office-document-symbolic"},
        {Segment::Pages,     "pages",     "Pages & Spreads", "view-grid-symbolic"},
        {Segment::Masters,   "masters",   "Masters",         "view-dual-symbolic"},
        {Segment::Stories,   "stories",   "Stories",         "format-justify-left-symbolic"},
        {Segment::Styles,    "styles",    "Styles",          "format-text-bold-symbolic"},
        {Segment::Assets,    "assets",    "Links & Assets",  "image-x-generic-symbolic"},
        {Segment::Colors,    "colors",    "Colors",          "applications-graphics-symbolic"},
        {Segment::Generated, "generated", "Contents & Index","view-list-bullet-symbolic"},
    };
    return k;
}

const SegmentInfo& segment_info(Segment s) {
    for (const auto& i : segments())
        if (i.id == s) return i;
    throw std::logic_error("segment_info: unknown section");
}

std::vector<const Item*> Outline::in(Segment s) const {
    std::vector<const Item*> out;
    for (const auto& it : m_items)
        if (it.segment == s) out.push_back(&it);
    return out;
}

const Item* Outline::find(const std::string& id) const {
    for (const auto& it : m_items)
        if (it.id == id) return &it;
    return nullptr;
}

std::vector<Field> shared_fields(const std::vector<const Item*>& sel) {
    std::vector<Field> out;
    if (sel.empty()) return out;
    for (const auto& f : sel.front()->fields) {
        bool everywhere = true, same = true, editable = !f.edit.empty();
        for (std::size_t i = 1; i < sel.size() && everywhere; ++i) {
            const auto& fs = sel[i]->fields;
            const auto hit = std::find_if(fs.begin(), fs.end(),
                                          [&](const Field& g) { return g.label == f.label; });
            if (hit == fs.end()) { everywhere = false; break; }
            if (hit->value != f.value) same = false;
            if (hit->edit != f.edit || hit->choices != f.choices) editable = false;
        }
        if (!everywhere) continue;
        Field g{f.label, same ? f.value : std::string(kMixed), {}, {}};
        if (editable) { g.edit = f.edit; g.choices = f.choices; }
        out.push_back(std::move(g));
    }
    return out;
}

std::string selection_title(const std::vector<const Item*>& sel) {
    if (sel.empty()) return "Nothing selected";
    if (sel.size() == 1) return sel.front()->title;
    const Segment s = sel.front()->segment;
    const bool one_kind = std::all_of(sel.begin(), sel.end(),
                                      [&](const Item* i) { return i->segment == s; });
    if (!one_kind) return std::to_string(sel.size()) + " items";
    static const char* noun[] = {"sections", "pages", "masters", "stories",
                                 "styles", "assets", "colors", "generated lists"};
    return std::to_string(sel.size()) + " " + noun[static_cast<int>(s)];
}

}  // namespace composix::core
