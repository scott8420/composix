#pragma once
// core::Geometry -- the canvas's arithmetic (s005), GTK-free and selftested.
//
// Ideas from Curvz, rewritten clean (Curvz keeps all of this inline in a
// 30k-line Canvas; the map is in ARC s005):
//   * Viewport: screen = doc * scale + origin. Zoom about the cursor keeps the
//     doc point under it fixed (Curvz zoom_toward); fit at 90% (Curvz 85%);
//     pan is clamped so some of the content stays on screen (Curvz clamp_pan).
//   * Handles: 8 positions from a screen rect (Curvz
//     selection_handle_positions), hit-tested in SCREEN pixels (7 px), corners
//     before edges.
//   * Resize: recomputed from the PRESS snapshot every frame, never
//     incrementally (Curvz on_select_update) -- Shift keeps proportions, Alt
//     resizes about the centre.
#include <algorithm>
#include <array>
#include <cmath>

namespace composix::core {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double r() const { return x + w; }
    double b() const { return y + h; }
    double cx() const { return x + w / 2; }
    double cy() const { return y + h / 2; }
    bool contains(double px, double py) const { return px >= x && px <= r() && py >= y && py <= b(); }
    bool intersects(const Rect& o) const { return x <= o.r() && o.x <= r() && y <= o.b() && o.y <= b(); }
    bool operator==(const Rect&) const = default;
    static Rect from_corners(double x0, double y0, double x1, double y1) {
        return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
    }
    Rect unite(const Rect& o) const {
        if (w <= 0 && h <= 0) return o;
        return from_corners(std::min(x, o.x), std::min(y, o.y), std::max(r(), o.r()), std::max(b(), o.b()));
    }
};

// ── the viewport ───────────────────────────────────────────────────────────
// `scale` is screen pixels per POINT. "100%" is true size on a 96-dpi screen:
// 96/72 px per pt (what a CSS px is, and what GTK lays out in).
class Viewport {
public:
    static constexpr double kTrue = 96.0 / 72.0;   // px per pt at 100%
    static constexpr double kMin  = 0.02, kMax = 64.0;

    void set_widget(double w, double h) { m_w = w; m_h = h; }
    void set_content(const Rect& r) { m_content = r; }
    double widget_w() const { return m_w; }
    double widget_h() const { return m_h; }
    const Rect& content() const { return m_content; }

    double scale() const { return m_scale; }
    double percent() const { return m_scale / kTrue * 100.0; }

    void to_screen(double dx, double dy, double& sx, double& sy) const { sx = dx * m_scale + m_ox; sy = dy * m_scale + m_oy; }
    void to_doc(double sx, double sy, double& dx, double& dy) const { dx = (sx - m_ox) / m_scale; dy = (sy - m_oy) / m_scale; }
    Rect to_screen(const Rect& r) const { return {r.x * m_scale + m_ox, r.y * m_scale + m_oy, r.w * m_scale, r.h * m_scale}; }
    double ox() const { return m_ox; }
    double oy() const { return m_oy; }

    // Zoom by `factor` keeping the doc point under (sx, sy) where it is.
    void zoom_toward(double sx, double sy, double factor) { set_scale_about(m_scale * factor, sx, sy); }
    void set_scale_about(double scale, double sx, double sy) {
        double dx, dy;
        to_doc(sx, sy, dx, dy);
        m_scale = std::clamp(scale, kMin, kMax);
        m_ox = sx - dx * m_scale;
        m_oy = sy - dy * m_scale;
        clamp();
    }
    // Zoom to a percentage about the widget's centre.
    void set_percent(double pct) { set_scale_about(pct / 100.0 * kTrue, m_w / 2, m_h / 2); }

    // The whole content, centred, at 90% of the space.
    void fit() {
        if (m_content.w <= 0 || m_content.h <= 0 || m_w <= 0 || m_h <= 0) return;
        m_scale = std::clamp(std::min(m_w / m_content.w, m_h / m_content.h) * 0.9, kMin, kMax);
        m_ox = (m_w - m_content.w * m_scale) / 2 - m_content.x * m_scale;
        m_oy = (m_h - m_content.h * m_scale) / 2 - m_content.y * m_scale;
    }

    void pan_by(double dx_px, double dy_px) { m_ox += dx_px; m_oy += dy_px; clamp(); }
    // s061: pan the least that brings a doc rect `margin` px inside the
    // widget (its top / left first when it can't all fit). True if it moved.
    bool reveal(const Rect& r, double margin = 40) {
        if (m_w <= 0 || m_h <= 0) return false;
        const Rect s = to_screen(r);
        double dx = 0, dy = 0;
        if (s.r() > m_w - margin) dx = (m_w - margin) - s.r();
        if (s.x + dx < margin) dx = margin - s.x;
        if (s.b() > m_h - margin) dy = (m_h - margin) - s.b();
        if (s.y + dy < margin) dy = margin - s.y;
        if (dx == 0 && dy == 0) return false;
        m_ox += dx;
        m_oy += dy;
        return true;
    }
    void set_origin(double ox, double oy) { m_ox = ox; m_oy = oy; clamp(); }

    // Keep at least `keep` px of the content on screen (or all of it, if smaller).
    void clamp(double keep = 48) {
        if (m_content.w <= 0 || m_w <= 0) return;
        const Rect s = to_screen(m_content);
        const double kx = std::min(keep, s.w), ky = std::min(keep, s.h);
        if (s.r() < kx)        m_ox += kx - s.r();
        if (s.x > m_w - kx)    m_ox -= s.x - (m_w - kx);
        if (s.b() < ky)        m_oy += ky - s.b();
        if (s.y > m_h - ky)    m_oy -= s.y - (m_h - ky);
    }

private:
    double m_w = 0, m_h = 0;
    double m_scale = 1, m_ox = 0, m_oy = 0;
    Rect   m_content;
};

// ── handles ────────────────────────────────────────────────────────────────
enum class Handle { None, NW, N, NE, E, SE, S, SW, W };

// The 8 handle centres of a (screen) rect, in Handle order NW..W.
inline std::array<std::array<double, 2>, 8> handle_points(const Rect& s) {
    return {{{s.x, s.y}, {s.cx(), s.y}, {s.r(), s.y}, {s.r(), s.cy()},
             {s.r(), s.b()}, {s.cx(), s.b()}, {s.x, s.b()}, {s.x, s.cy()}}};
}

// Which handle (screen px), corners first so a small rect still resizes from
// its corners. `half` is the grab half-size.
inline Handle handle_hit(const Rect& s, double px, double py, double half = 7) {
    const auto pts = handle_points(s);
    static constexpr Handle order[] = {Handle::NW, Handle::NE, Handle::SE, Handle::SW,
                                       Handle::N, Handle::E, Handle::S, Handle::W};
    for (Handle h : order) {
        const auto& p = pts[static_cast<int>(h) - 1];
        if (std::abs(px - p[0]) <= half && std::abs(py - p[1]) <= half) return h;
    }
    return Handle::None;
}

// The press rect, resized by dragging handle `h` by (dx, dy) doc units.
// Shift: keep proportions. Alt: about the centre. Never thinner than `min`.
inline Rect resize_rect(const Rect& r0, Handle h, double dx, double dy, bool shift, bool alt, double min = 1) {
    if (h == Handle::None) return r0;
    const bool L = h == Handle::NW || h == Handle::W || h == Handle::SW;
    const bool R = h == Handle::NE || h == Handle::E || h == Handle::SE;
    const bool T = h == Handle::NW || h == Handle::N || h == Handle::NE;
    const bool B = h == Handle::SW || h == Handle::S || h == Handle::SE;
    double x0 = r0.x, x1 = r0.r(), y0 = r0.y, y1 = r0.b();
    if (L) x0 += dx;
    if (R) x1 += dx;
    if (T) y0 += dy;
    if (B) y1 += dy;
    if (alt) {   // about the centre: the opposite side mirrors the move
        if (L) x1 -= dx;
        if (R) x0 -= dx;
        if (T) y1 -= dy;
        if (B) y0 -= dy;
    }
    double w = std::max(min, x1 - x0), hh = std::max(min, y1 - y0);
    if (shift && r0.w > 0 && r0.h > 0) {
        const double a = r0.w / r0.h;
        if ((L || R) && (T || B)) {   // a corner: the larger change wins
            if (w / r0.w > hh / r0.h) hh = w / a; else w = hh * a;
        } else if (L || R) hh = w / a;
        else               w = hh * a;
    }
    // Anchor on the opposite side; an axis the handle doesn't touch stays
    // centred (it only changes under Shift); Alt anchors on the centre.
    const double nx = alt ? r0.cx() - w / 2 : L ? r0.r() - w : R ? r0.x : r0.cx() - w / 2;
    const double ny = alt ? r0.cy() - hh / 2 : T ? r0.b() - hh : B ? r0.y : r0.cy() - hh / 2;
    return {nx, ny, w, hh};
}

// Map `r` from the frame `from` to the frame `to` (a multi-selection resized
// as one box: each member keeps its place in the box).
inline Rect map_rect(const Rect& r, const Rect& from, const Rect& to) {
    const double sx = from.w > 0 ? to.w / from.w : 1, sy = from.h > 0 ? to.h / from.h : 1;
    return {to.x + (r.x - from.x) * sx, to.y + (r.y - from.y) * sy, r.w * sx, r.h * sy};
}

}  // namespace composix::core
