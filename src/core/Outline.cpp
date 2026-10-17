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

std::optional<Segment> parent_of(const Outline& o, const std::vector<std::string>& ids) {
    for (const auto& id : ids)
        if (const auto* it = o.find(id)) return it->segment;
    return std::nullopt;
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
        if (f.buttons) {   // actions: the ones every item offers (s014)
            Field g{f.label, "", f.edit, {}};
            g.buttons = true;
            for (const auto& c : f.choices) {
                bool all = true;
                for (std::size_t i = 1; i < sel.size() && all; ++i) {
                    const auto& fs = sel[i]->fields;
                    const auto hit = std::find_if(fs.begin(), fs.end(), [&](const Field& h) { return h.buttons && h.edit == f.edit; });
                    all = hit != fs.end() && std::find(hit->choices.begin(), hit->choices.end(), c) != hit->choices.end();
                }
                if (all) g.choices.push_back(c);
            }
            if (!g.choices.empty()) out.push_back(std::move(g));
            continue;
        }
        Field g{f.label, same ? f.value : std::string(kMixed), {}, {}};
        if (!f.cols.empty()) {   // per column: the value where all agree, Mixed where not
            const auto split = [](const std::string& v) {
                std::vector<std::string> out(1);
                for (char c : v) { if (c == '\t') out.emplace_back(); else out.back() += c; }
                return out;
            };
            auto parts = split(f.value);
            for (std::size_t i = 1; i < sel.size(); ++i)
                for (const auto& h : sel[i]->fields)
                    if (h.label == f.label) {
                        const auto other = split(h.value);
                        for (std::size_t k = 0; k < parts.size(); ++k)
                            if (k >= other.size() || other[k] != parts[k]) parts[k] = kMixed;
                    }
            g.value.clear();
            for (std::size_t k = 0; k < parts.size(); ++k) g.value += (k ? "\t" : "") + parts[k];
            g.cols = f.cols;
        }
        if (editable) { g.edit = f.edit; g.choices = f.choices; g.buttons = f.buttons; g.unit = f.unit; g.lo = f.lo; g.hi = f.hi; g.text = f.text; g.link = f.link; g.percent = f.percent; }
        if (!f.own.empty()) {   // s092: marked when it's every picked style's own
            bool all = true;
            for (std::size_t i = 1; i < sel.size() && all; ++i) {
                const auto& fs = sel[i]->fields;
                all = std::any_of(fs.begin(), fs.end(), [&](const Field& h) { return h.label == f.label && h.own == f.own; });
            }
            if (all) g.own = f.own;
        }
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
