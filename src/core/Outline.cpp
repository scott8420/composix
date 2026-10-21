#include "core/Outline.hpp"

#include <algorithm>
#include <set>
#include <map>
#include <stdexcept>

namespace composix::core {

const std::vector<SegmentInfo>& segments() {
    static const std::vector<SegmentInfo> k = {
        {Segment::Document,  "document",  "Document",        "x-office-document-symbolic"},
        {Segment::Masters,   "masters",   "Masters",         "view-dual-symbolic"},   // s150 (Scott): masters before pages
        {Segment::Pages,     "pages",     "Pages & Spreads", "view-grid-symbolic"},
        {Segment::Stories,   "stories",   "Stories",         "format-justify-left-symbolic"},
        {Segment::Notes,     "notes",     "Footnotes",       "composix-footnote-symbolic"},   // s115 (bundled)
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

std::string note_item_id(const std::string& story, int para, int k) {   // s115
    return "note:" + story + ":" + std::to_string(para) + ":" + std::to_string(k);
}

bool parse_note_item_id(const std::string& id, std::string& story, int& para, int& k) {
    if (id.rfind("note:", 0) != 0) return false;
    const auto c2 = id.rfind(':');
    if (c2 == std::string::npos || c2 < 5) return false;
    const auto c1 = id.rfind(':', c2 - 1);
    if (c1 == std::string::npos || c1 < 5) return false;
    try {
        std::size_t used = 0;
        para = std::stoi(id.substr(c1 + 1, c2 - c1 - 1), &used);
        if (used != c2 - c1 - 1) return false;
        k = std::stoi(id.substr(c2 + 1), &used);
        if (used != id.size() - c2 - 1) return false;
    } catch (...) {
        return false;
    }
    story = id.substr(5, c1 - 5);
    return !story.empty() && para >= 0 && k >= 0;
}

std::vector<const Item*> Outline::in(Segment s) const {
    std::vector<const Item*> out;
    for (const auto& it : m_items)
        if (it.segment == s) out.push_back(&it);
    return out;
}

std::vector<std::vector<std::string>> spread_slots(const std::vector<std::string>& pages,
                                                   const std::vector<std::vector<std::string>>& groups,
                                                   const std::function<bool(const std::string&)>& is_left) {
    std::vector<std::vector<std::string>> out;
    std::map<std::string, std::size_t> group_of;
    for (std::size_t g = 0; g < groups.size(); ++g)
        for (const auto& id : groups[g]) group_of[id] = g;
    std::set<std::size_t> done;
    for (const auto& id : pages) {
        const auto it = group_of.find(id);
        if (it == group_of.end()) { out.push_back({id}); continue; }
        if (!done.insert(it->second).second) continue;
        std::vector<std::string> row;
        for (const auto& p : groups[it->second])
            if (std::find(pages.begin(), pages.end(), p) != pages.end()) row.push_back(p);
        if (row.size() == 1) {
            if (is_left && is_left(row.front())) row.push_back("");
            else                                 row.insert(row.begin(), "");
        }
        out.push_back(row);
    }
    return out;
}

std::vector<Field> order_groups(const std::vector<Field>& fields, const std::vector<std::string>& order) {
    std::vector<Field> head, tail;
    std::vector<std::string> names;
    std::map<std::string, std::vector<Field>> blocks;
    bool seen_group = false;
    for (const auto& f : fields) {
        if (f.group.empty()) { (seen_group ? tail : head).push_back(f); continue; }
        seen_group = true;
        if (!blocks.count(f.group)) names.push_back(f.group);
        blocks[f.group].push_back(f);
    }
    if (names.empty()) return fields;
    std::stable_sort(names.begin(), names.end(), [&order](const std::string& a, const std::string& b) {
        const auto ia = std::find(order.begin(), order.end(), a), ib = std::find(order.begin(), order.end(), b);
        return ia < ib;   // not named: order.end() -- after the named, as they came
    });
    std::vector<Field> out = head;
    for (const auto& n : names) out.insert(out.end(), blocks[n].begin(), blocks[n].end());
    out.insert(out.end(), tail.begin(), tail.end());
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
            g.group = f.group;   // s161
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
        g.group = f.group;   // s161
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
                                 "styles", "assets", "colors", "generated lists", "footnotes"};
    return std::to_string(sel.size()) + " " + noun[static_cast<int>(s)];
}

}  // namespace composix::core
