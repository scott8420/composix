#pragma once
// s357 (WK1, Scott): composition problems -- what a typesetter fixes by hand.
//   * an ORPHAN: a paragraph's first line alone at the foot of a column (the
//     rest goes on in the next);
//   * a WIDOW: a paragraph's last line alone at the head of a column;
//   * a RUNT: a paragraph's last line a single word.
// Found from a story's flow (what's set, as set): a column is a frame's
// (band, column). Tables' rows and paragraphs of one line are passed by.
#include "core/Doc.hpp"
#include "layout/Flow.hpp"

#include <string>
#include <vector>

namespace composix::layout {

struct CompProblem {
    enum Kind { Widow, Orphan, Runt } kind = Widow;
    std::string frame;   // where the line is set
    int         para = 0;
    core::Rect  box;     // the line, frame coordinates
    bool operator==(const CompProblem&) const = default;
};

std::vector<CompProblem> composition_problems(const core::Doc& d, const std::string& story, const StoryFlow& f);
const char* comp_kind_name(CompProblem::Kind k);   // "Widow", "Orphan", "Runt"

}  // namespace composix::layout
