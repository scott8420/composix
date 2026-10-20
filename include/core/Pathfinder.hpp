#pragma once
// core::Pathfinder (s213) -- shapes combined: united, one cut out of
// another, their overlap, or all but it; and (s215) a shape's outline
// offset out or in. The outlines go to Clipper2 as polygons (each curve
// flattened finely); what comes back is fitted with curves again
// (Schneider's least-squares fit, corners kept where the outline turns
// sharply), so a circle comes back a few curves, not a hundred lines.
// GTK-free; the selftest walks it.
#include "core/Doc.hpp"

#include <string>
#include <vector>

namespace composix::core::doc {

enum class PathOp { Unite, Subtract, Intersect, Exclude };
const char* path_op_name(PathOp op);   // "Unite", "Subtract", ...

// Closed polygons (owner coordinates) back into path points: a corner where
// the outline turns more than ~35 degrees, curves fitted within `tol`
// points between, straight runs kept straight. Each polygon one subpath;
// `subs` gets where each further one starts.
std::vector<PathPoint> fit_outline(const std::vector<std::vector<XY>>& polys, double tol, std::vector<int>& subs);

// The op on the picked shapes (two or more, closed, not text frames, on one
// page). Subtract: the backmost minus all the rest, in its style; the others
// keep the frontmost one's style. The result replaces that frame (a compound
// path when it comes in parts); the rest go. One step ("Unite shapes" ...).
// Nothing left (two shapes that don't overlap, intersected): no step, "".
Doc::Id path_op(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, PathOp op);

// s215: each picked shape's outline moved out (`dist` > 0) or in (< 0)
// points -- round, mitre or bevel corners (join 1 / 0 / 2, as a stroke's).
// A new shape on top, styled as its source (`keep`: the source stays; else
// it's replaced). One step "Offset path"; the new ids.
std::vector<Doc::Id> offset_paths(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double dist, int join, bool keep);

}  // namespace composix::core::doc
