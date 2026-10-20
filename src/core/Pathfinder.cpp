#include "core/Pathfinder.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace composix::core::doc {

namespace {
// ── small vector arithmetic ────────────────────────────────────────────────
XY operator+(XY a, XY b) { return {a.x + b.x, a.y + b.y}; }
XY operator-(XY a, XY b) { return {a.x - b.x, a.y - b.y}; }
XY operator*(XY a, double s) { return {a.x * s, a.y * s}; }
double dot(XY a, XY b) { return a.x * b.x + a.y * b.y; }
double len(XY a) { return std::hypot(a.x, a.y); }
XY unit(XY a) {
    const double l = len(a);
    return l > 1e-12 ? XY{a.x / l, a.y / l} : XY{0, 0};
}

// ── Schneider's curve fit ("An Algorithm for Automatically Fitting Digitized
// Curves", Graphics Gems, 1990) ───────────────────────────────────────────
struct Bez { XY p[4]; };
XY at(const Bez& b, double t) {
    const double u = 1 - t;
    return b.p[0] * (u * u * u) + b.p[1] * (3 * u * u * t) + b.p[2] * (3 * u * t * t) + b.p[3] * (t * t * t);
}
double B0(double u) { return (1 - u) * (1 - u) * (1 - u); }
double B1(double u) { return 3 * u * (1 - u) * (1 - u); }
double B2(double u) { return 3 * u * u * (1 - u); }
double B3(double u) { return u * u * u; }

Bez generate(const std::vector<XY>& d, int first, int last, const std::vector<double>& u, XY t1, XY t2) {
    double C[2][2] = {{0, 0}, {0, 0}}, X[2] = {0, 0};
    const XY p0 = d[static_cast<std::size_t>(first)], p3 = d[static_cast<std::size_t>(last)];
    for (std::size_t i = 0; i < u.size(); ++i) {
        const XY a0 = t1 * B1(u[i]), a1 = t2 * B2(u[i]);
        C[0][0] += dot(a0, a0);
        C[0][1] += dot(a0, a1);
        C[1][1] += dot(a1, a1);
        const XY tmp = d[static_cast<std::size_t>(first) + i] - (p0 * (B0(u[i]) + B1(u[i])) + p3 * (B2(u[i]) + B3(u[i])));
        X[0] += dot(a0, tmp);
        X[1] += dot(a1, tmp);
    }
    C[1][0] = C[0][1];
    const double det = C[0][0] * C[1][1] - C[1][0] * C[0][1];
    const double al = det == 0 ? 0 : (X[0] * C[1][1] - X[1] * C[0][1]) / det;
    const double ar = det == 0 ? 0 : (C[0][0] * X[1] - C[1][0] * X[0]) / det;
    const double seg = len(p3 - p0), eps = 1e-6 * seg;
    if (al < eps || ar < eps || al > seg * 4 || ar > seg * 4) {   // the heuristic: a third of the chord
        const double dd = seg / 3;
        return {{p0, p0 + t1 * dd, p3 + t2 * dd, p3}};
    }
    return {{p0, p0 + t1 * al, p3 + t2 * ar, p3}};
}

double max_error(const std::vector<XY>& d, int first, int last, const Bez& b, const std::vector<double>& u, int& split) {
    double worst = 0;
    split = (first + last) / 2;
    for (int i = first + 1; i < last; ++i) {
        const double e = len(at(b, u[static_cast<std::size_t>(i - first)]) - d[static_cast<std::size_t>(i)]);
        if (e >= worst) { worst = e; split = i; }
    }
    return worst;
}

std::vector<double> chord_params(const std::vector<XY>& d, int first, int last) {
    std::vector<double> u{0};
    for (int i = first + 1; i <= last; ++i) u.push_back(u.back() + len(d[static_cast<std::size_t>(i)] - d[static_cast<std::size_t>(i - 1)]));
    const double total = u.back();
    for (auto& v : u) v = total > 0 ? v / total : 0;
    return u;
}

double newton(const Bez& b, XY p, double u) {   // one Newton step toward the nearest parameter
    const XY d1[3] = {(b.p[1] - b.p[0]) * 3, (b.p[2] - b.p[1]) * 3, (b.p[3] - b.p[2]) * 3};
    const XY d2[2] = {(d1[1] - d1[0]) * 2, (d1[2] - d1[1]) * 2};
    const double v = 1 - u;
    const XY q = at(b, u);
    const XY q1 = d1[0] * (v * v) + d1[1] * (2 * u * v) + d1[2] * (u * u);
    const XY q2 = d2[0] * v + d2[1] * u;
    const double num = dot(q - p, q1), den = dot(q1, q1) + dot(q - p, q2);
    return den == 0 ? u : u - num / den;
}

void fit(const std::vector<XY>& d, int first, int last, XY t1, XY t2, double err, std::vector<Bez>& out) {
    if (last - first == 1) {
        const double dd = len(d[static_cast<std::size_t>(last)] - d[static_cast<std::size_t>(first)]) / 3;
        out.push_back({{d[static_cast<std::size_t>(first)], d[static_cast<std::size_t>(first)] + t1 * dd,
                        d[static_cast<std::size_t>(last)] + t2 * dd, d[static_cast<std::size_t>(last)]}});
        return;
    }
    auto u = chord_params(d, first, last);
    Bez b = generate(d, first, last, u, t1, t2);
    int split = 0;
    double e = max_error(d, first, last, b, u, split);
    if (e < err) { out.push_back(b); return; }
    if (e < err * 4) {
        for (int k = 0; k < 4; ++k) {
            for (std::size_t i = 0; i < u.size(); ++i) u[i] = std::clamp(newton(b, d[static_cast<std::size_t>(first) + i], u[i]), 0.0, 1.0);
            b = generate(d, first, last, u, t1, t2);
            e = max_error(d, first, last, b, u, split);
            if (e < err) { out.push_back(b); return; }
        }
    }
    split = std::clamp(split, first + 1, last - 1);
    XY tc = unit(d[static_cast<std::size_t>(split - 1)] - d[static_cast<std::size_t>(split + 1)]);
    if (len(tc) == 0) tc = unit(d[static_cast<std::size_t>(split - 1)] - d[static_cast<std::size_t>(split)]);
    fit(d, first, split, t1, tc, err, out);
    fit(d, split, last, tc * -1, t2, err, out);
}

// A cubic that is a straight line (its handles on the chord, between its
// ends): its handles put on its anchors, so it's drawn and kept as a line.
bool straight_bez(const Bez& b) {
    const XY c = b.p[3] - b.p[0];
    const double l = len(c);
    if (l < 1e-9) return true;
    for (int k : {1, 2}) {
        const XY v = b.p[k] - b.p[0];
        if (std::abs(v.x * c.y - v.y * c.x) / l > 0.02) return false;
        const double t = dot(v, c) / (l * l);
        if (t < -0.01 || t > 1.01) return false;
    }
    return true;
}

// One closed polygon -> its path points.
std::vector<PathPoint> fit_loop(std::vector<XY> P, double tol) {
    // Duplicates and the closing repeat out.
    std::vector<XY> q;
    for (const auto& p : P)
        if (q.empty() || len(p - q.back()) > 0.005) q.push_back(p);
    while (q.size() > 2 && len(q.front() - q.back()) <= 0.005) q.pop_back();
    const int n = static_cast<int>(q.size());
    if (n < 3) return {};
    // Corners: a turn sharper than ~35 degrees.
    std::vector<bool> corner(static_cast<std::size_t>(n), false);
    const double lim = std::cos(35.0 * M_PI / 180);
    for (int i = 0; i < n; ++i) {
        const XY a = unit(q[static_cast<std::size_t>(i)] - q[static_cast<std::size_t>((i + n - 1) % n)]);
        const XY b = unit(q[static_cast<std::size_t>((i + 1) % n)] - q[static_cast<std::size_t>(i)]);
        corner[static_cast<std::size_t>(i)] = dot(a, b) < lim;
    }
    int start = -1;
    for (int i = 0; i < n; ++i) if (corner[static_cast<std::size_t>(i)]) { start = i; break; }
    std::vector<XY> d;   // the loop from its first corner (or point 0), round to it again
    const int s0 = start < 0 ? 0 : start;
    for (int i = 0; i <= n; ++i) d.push_back(q[static_cast<std::size_t>((s0 + i) % n)]);
    std::vector<bool> cd;
    for (int i = 0; i <= n; ++i) cd.push_back(corner[static_cast<std::size_t>((s0 + i) % n)]);
    std::vector<Bez> curves;
    std::vector<int> run_starts;   // which curves begin at a corner
    if (start < 0) {   // smooth all round: one run, its ends' tangents matched
        const XY t = unit(d[1] - d[static_cast<std::size_t>(n - 1)]);
        fit(d, 0, n, t, t * -1, tol, curves);
    } else {
        int a = 0;
        for (int b = 1; b <= n; ++b) {
            if (!cd[static_cast<std::size_t>(b)] && b != n) continue;
            const XY t1 = unit(d[static_cast<std::size_t>(a + 1)] - d[static_cast<std::size_t>(a)]);
            const XY t2 = unit(d[static_cast<std::size_t>(b - 1)] - d[static_cast<std::size_t>(b)]);
            fit(d, a, b, t1, t2, tol, curves);
            a = b;
        }
    }
    // Curves -> points: each curve's start an anchor, its handles set.
    std::vector<PathPoint> out;
    for (const auto& c : curves) {
        PathPoint p = corner_point(c.p[0].x, c.p[0].y);
        if (!out.empty() && len(XY{out.back().x, out.back().y} - c.p[0]) < 1e-6) {
            // the same anchor as the last curve's end: just its out handle
        } else {
            out.push_back(p);
        }
        auto& a = out.back();
        if (!straight_bez(c)) { a.ox = c.p[1].x; a.oy = c.p[1].y; }
        // the next anchor (made by the next curve, or the first again) gets the in handle
        PathPoint e = corner_point(c.p[3].x, c.p[3].y);
        if (!straight_bez(c)) { e.ix = c.p[2].x; e.iy = c.p[2].y; }
        out.push_back(e);
    }
    // The last anchor is the first again: its in handle goes to the first.
    if (out.size() >= 2 && len(XY{out.back().x, out.back().y} - XY{out.front().x, out.front().y}) < 1e-6) {
        out.front().ix = out.back().ix;
        out.front().iy = out.back().iy;
        out.pop_back();
    }
    // A point is smooth when its handles are in line (not a corner).
    for (auto& p : out) {
        const XY hi = XY{p.ix, p.iy} - XY{p.x, p.y}, ho = XY{p.ox, p.oy} - XY{p.x, p.y};
        p.smooth = len(hi) > 1e-6 && len(ho) > 1e-6 && dot(unit(hi), unit(ho)) < -0.999;
    }
    return out;
}

// ── to and from Clipper ───────────────────────────────────────────────────
Clipper2Lib::PathsD to_clip(const std::vector<std::vector<XY>>& polys) {
    Clipper2Lib::PathsD out;
    for (const auto& poly : polys) {
        Clipper2Lib::PathD p;
        for (const auto& q : poly) p.push_back({q.x, q.y});
        if (p.size() >= 2) out.push_back(std::move(p));
    }
    return out;
}
std::vector<std::vector<XY>> from_clip(const Clipper2Lib::PathsD& paths) {
    std::vector<std::vector<XY>> out;
    for (const auto& p : paths) {
        std::vector<XY> poly;
        for (const auto& q : p) poly.push_back({q.x, q.y});
        if (poly.size() >= 3) out.push_back(std::move(poly));
    }
    return out;
}
constexpr int kPrecision = 3;   // a thousandth of a point
// A frame's outline as a Clipper region (its holes even-odd).
Clipper2Lib::PathsD region_of(const Frame& f) {
    return Clipper2Lib::Union(to_clip(outline_polys(f, 0.05)), Clipper2Lib::FillRule::EvenOdd, kPrecision);
}
bool closed_shape(const Frame& f) {
    if (f.kind == FrameKind::Text || f.rot != 0) return false;
    return !(f.shape == ShapeKind::Path && f.open);
}
}  // namespace

const char* path_op_name(PathOp op) {
    switch (op) {
    case PathOp::Unite: return "Unite";
    case PathOp::Subtract: return "Subtract";
    case PathOp::Intersect: return "Intersect";
    case PathOp::Exclude: return "Exclude";
    }
    return "";
}

std::vector<PathPoint> fit_outline(const std::vector<std::vector<XY>>& polys, double tol, std::vector<int>& subs) {
    subs.clear();
    std::vector<PathPoint> all;
    for (const auto& poly : polys) {
        auto pts = fit_loop(poly, tol);
        if (pts.size() < 2) continue;
        if (!all.empty()) subs.push_back(static_cast<int>(all.size()));
        all.insert(all.end(), pts.begin(), pts.end());
    }
    return all;
}

Doc::Id path_op(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, PathOp op) {
    std::vector<Doc::Id> use;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (!f || !closed_shape(*f)) continue;
        if (!use.empty() && f->owner != d.as<Frame>(use.front())->owner) continue;
        use.push_back(id);
    }
    if (use.size() < 2) return {};
    std::stable_sort(use.begin(), use.end(), [&](const Doc::Id& a, const Doc::Id& b) { return d.as<Frame>(a)->z < d.as<Frame>(b)->z; });
    using namespace Clipper2Lib;
    const auto nz = FillRule::NonZero;
    PathsD result;
    if (op == PathOp::Subtract) {   // the backmost, minus the rest
        PathsD rest;
        for (std::size_t i = 1; i < use.size(); ++i) {
            const auto r = region_of(*d.as<Frame>(use[i]));
            rest.insert(rest.end(), r.begin(), r.end());
        }
        result = Difference(region_of(*d.as<Frame>(use.front())), Union(rest, nz, kPrecision), nz, kPrecision);
    } else {
        result = region_of(*d.as<Frame>(use.front()));
        for (std::size_t i = 1; i < use.size(); ++i) {
            const auto r = region_of(*d.as<Frame>(use[i]));
            result = op == PathOp::Unite     ? Union(result, r, nz, kPrecision)
                   : op == PathOp::Intersect ? Intersect(result, r, nz, kPrecision)
                                             : Xor(result, r, nz, kPrecision);
        }
    }
    std::vector<int> subs;
    const auto pts = fit_outline(from_clip(result), 0.2, subs);
    if (pts.size() < 3) return {};   // nothing left
    const Doc::Id keep = op == PathOp::Subtract ? use.front() : use.back();
    j.run(std::string(path_op_name(op)) + " shapes", use, [&] {
        Frame g = *d.as<Frame>(keep);
        g.kind = FrameKind::Shape;
        g.story.clear();
        g.next.clear();
        g.asset.clear();
        g.radius = 0;
        g.end_a = g.end_b = 0;
        set_path(g, pts, false);
        g.subs = subs;
        g.z = d.as<Frame>(use.back())->z;   // where the frontmost was
        d.put(keep, g);
        for (const auto& id : use) if (id != keep) d.put(id, std::nullopt);
    });
    return keep;
}

std::vector<Doc::Id> offset_paths(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, double dist, int join, bool keep) {
    using namespace Clipper2Lib;
    struct Made { Doc::Id from; Frame f; };
    std::vector<Made> plan;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (!f || f->kind == FrameKind::Text || f->rot != 0 || dist == 0) continue;
        const bool open = f->shape == ShapeKind::Path && f->open;
        if (open && dist <= 0) continue;   // a line has no inside
        const JoinType jt = join == 1 ? JoinType::Round : join == 2 ? JoinType::Bevel : JoinType::Miter;
        const EndType et = !open ? EndType::Polygon : f->cap == 1 ? EndType::Round : f->cap == 2 ? EndType::Square : EndType::Butt;
        PathsD src = open ? to_clip(outline_polys(*f, 0.05)) : region_of(*f);
        const PathsD res = InflatePaths(src, dist, jt, et, 4.0, kPrecision, 0.05);
        std::vector<int> subs;
        const auto pts = fit_outline(from_clip(res), 0.2, subs);
        if (pts.size() < 3) continue;
        Frame g = *f;
        g.kind = FrameKind::Shape;
        g.story.clear();
        g.next.clear();
        g.asset.clear();
        g.radius = 0;
        g.end_a = g.end_b = 0;
        g.dash = 0;
        if (open) {   // a line's outline: filled in its stroke's colour (Outline Stroke)
            g.fill = f->stroke;
            g.fill_tint = f->stroke_tint;
            g.stroke.clear();
        }
        set_path(g, pts, false);
        g.subs = subs;
        g.z = f->z + 0.5;
        plan.push_back({id, g});
    }
    std::vector<Doc::Id> out;
    if (plan.empty()) return out;
    std::vector<Doc::Id> touch;
    if (!keep) for (const auto& m : plan) touch.push_back(m.from);
    j.run(keep ? "Offset path" : "Outline", touch, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        for (auto& m : plan) {
            if (!keep) {
                m.f.z = d.as<Frame>(m.from)->z;
                d.put(m.from, m.f);
                out.push_back(m.from);
                continue;
            }
            const Doc::Id nid = d.fresh_id("frame");
            d.put(nid, m.f);
            made.push_back(nid);
            out.push_back(nid);
        }
        return made;
    });
    return out;
}

}  // namespace composix::core::doc
