#include "layout/Composition.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace composix::layout {

const char* comp_kind_name(CompProblem::Kind k) {
    switch (k) {
    case CompProblem::Widow:  return "Widow";
    case CompProblem::Orphan: return "Orphan";
    case CompProblem::Runt:   return "Runt";
    }
    return "";
}

namespace {
struct At {
    std::size_t frame;   // index in the flow
    int         band, col;
    const Line* line;
    bool same_column(const At& o) const { return frame == o.frame && band == o.band && col == o.col; }
};

// The line's words, trimmed -- a single word when there's no space inside.
bool one_word(const std::string& text, int start, int end) {
    start = std::clamp(start, 0, static_cast<int>(text.size()));
    end = std::clamp(end, start, static_cast<int>(text.size()));
    int a = start, b = end;
    const auto space = [&](int i) { return std::isspace(static_cast<unsigned char>(text[static_cast<std::size_t>(i)])) != 0; };
    while (a < b && space(a)) ++a;
    while (b > a && space(b - 1)) --b;
    if (a >= b) return false;   // nothing on it
    for (int i = a; i < b; ++i)
        if (space(i)) return false;
    return true;
}
}  // namespace

std::vector<CompProblem> composition_problems(const core::Doc& d, const std::string& story, const StoryFlow& f) {
    std::vector<CompProblem> out;
    const auto* st = d.as<core::Story>(story);
    if (!st) return out;
    std::map<int, std::vector<At>> by_para;   // each paragraph's lines, in flow order
    for (std::size_t i = 0; i < f.frames.size(); ++i)
        for (const auto& l : f.frames[i].lines) {
            if (l.row >= 0) continue;   // a table's
            by_para[l.para].push_back({i, l.band, l.col, &l});
        }
    const auto box_of = [&](const At& a) {
        const auto* fr = d.as<core::Frame>(f.frames[a.frame].frame);
        const double w = a.line->w > 0 ? a.line->w : (fr ? line_width_of(*fr, *a.line) : 0);
        return core::Rect{a.line->x, a.line->y, w, a.line->h};
    };
    for (const auto& [para, ls] : by_para) {
        if (ls.size() < 2) continue;   // one line: none of these
        if (!ls[0].same_column(ls[1]))
            out.push_back({CompProblem::Orphan, f.frames[ls[0].frame].frame, para, box_of(ls[0])});
        const auto& last = ls.back();
        if (!last.same_column(ls[ls.size() - 2])) {
            out.push_back({CompProblem::Widow, f.frames[last.frame].frame, para, box_of(last)});
            continue;   // a widow's the bigger fault
        }
        if (para >= 0 && para < static_cast<int>(st->paras.size()) &&
            one_word(st->paras[static_cast<std::size_t>(para)].text, last.line->start, last.line->end))
            out.push_back({CompProblem::Runt, f.frames[last.frame].frame, para, box_of(last)});
    }
    return out;
}

}  // namespace composix::layout
