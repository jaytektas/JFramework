#pragma once

// ============================================================================
// jf::JVectorCanvas — anti-aliased 2D vector drawing.
//
// A retained-immediate painter that tessellates high-level vector ops (lines,
// polylines, arcs, circles, ellipses, pies/rings, bezier curves, polygons,
// rounded rects) into per-vertex-coloured JRenderVertex triangles, then hands
// them to a JPrimitiveBuffer via pushGeometry(). The GPU "vector" pipeline draws
// them in one batched pass.
//
// Why it's good:
//   * Resolution-independent anti-aliasing via a 1px alpha "fringe" on every
//     edge — crisp at any DPI, no MSAA cost.
//   * Per-vertex colour, so linear/radial gradients are free (interpolated on
//     the GPU) — ideal for gauges, charts, and skinned widgets.
//   * Miter-joined strokes with butt/round caps; arcs/pies for instrument dials.
//   * Pure geometry, no GPU types — fully unit-testable on the CPU.
//
// Usage:
//   JVectorCanvas vg;
//   vg.fillCircle(120, 120, 80, JPaint::radial(120,120, 0, rgb(40,40,60),
//                                              80, rgb(10,10,20)));
//   vg.strokeArc(120, 120, 70, -2.4f, 0.9f, 10.f, rgb(0,200,120));   // gauge
//   vg.moveTo(20,200); vg.lineTo(80,140); vg.lineTo(160,180);        // chart line
//   vg.stroke(2.5f, rgb(220,220,40));
//   vg.flush(buffer);   // -> buffer.pushGeometry(...)
// ============================================================================

#include <j/graphics/RenderPrimitive.h>
#include <cstdint>
#include <cmath>
#include <vector>

inline namespace jf {

// ---- Colour -----------------------------------------------------------------
struct JColor {
    uint8_t r{0}, g{0}, b{0}, a{255};
    bool operator==(const JColor& o) const { return r==o.r && g==o.g && b==o.b && a==o.a; }
    bool operator!=(const JColor& o) const { return !(*this == o); }
    // Contiguous RGBA8 view — the 4 packed bytes are a valid `const uint8_t[4]` for the
    // render primitives (pushRectangle et al.).
    const uint8_t* data() const noexcept { return &r; }
    // Build from the legacy `uint8_t[4]` colour arrays the theme stores.
    static JColor fromArray(const uint8_t c[4]) { return JColor{c[0], c[1], c[2], c[3]}; }
};
inline JColor rgb (uint8_t r, uint8_t g, uint8_t b)            { return {r, g, b, 255}; }
inline JColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return {r, g, b, a}; }
inline JColor withAlpha(JColor c, uint8_t a) { c.a = a; return c; }
inline JColor lerp(JColor a, JColor b, float t) {
    if (t < 0) t = 0; else if (t > 1) t = 1;
    auto L = [&](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {L(a.r, b.r), L(a.g, b.g), L(a.b, b.b), L(a.a, b.a)};
}

// ---- JPaint (solid / linear / radial) ---------------------------------------
struct JPaint {
    enum class JKind : uint8_t { Solid, Linear, Radial } kind{JKind::Solid};
    JColor c0{}, c1{};
    float x0{0}, y0{0}, x1{0}, y1{0};   // linear: endpoints; radial: center + (r0,r1) in x1,y1
    float r0{0}, r1{0};

    JPaint() = default;
    JPaint(JColor c) : kind(JKind::Solid), c0(c) {}

    static JPaint solid(JColor c) { return JPaint(c); }
    static JPaint linear(float x0, float y0, JColor c0, float x1, float y1, JColor c1) {
        JPaint p; p.kind = JKind::Linear; p.x0 = x0; p.y0 = y0; p.c0 = c0;
        p.x1 = x1; p.y1 = y1; p.c1 = c1; return p;
    }
    static JPaint radial(float cx, float cy, float rInner, JColor cInner,
                        float rOuter, JColor cOuter) {
        JPaint p; p.kind = JKind::Radial; p.x0 = cx; p.y0 = cy;
        p.r0 = rInner; p.c0 = cInner; p.r1 = rOuter; p.c1 = cOuter; return p;
    }

    JColor eval(float x, float y) const {
        switch (kind) {
            case JKind::Linear: {
                float dx = x1 - x0, dy = y1 - y0;
                float len2 = dx * dx + dy * dy;
                float t = len2 > 1e-6f ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0.f;
                return lerp(c0, c1, t);
            }
            case JKind::Radial: {
                float d = std::sqrt((x - x0) * (x - x0) + (y - y0) * (y - y0));
                float span = r1 - r0;
                float t = span > 1e-6f ? (d - r0) / span : (d > r0 ? 1.f : 0.f);
                return lerp(c0, c1, t);
            }
            default: return c0;
        }
    }
};

enum class JLineCap  : uint8_t { Butt, Round, Square };
enum class JLineJoin : uint8_t { Miter, Bevel, Round };

// ---- JCanvas -----------------------------------------------------------------
class JVectorCanvas {
public:
    struct JVec2 { float x, y; };

    explicit JVectorCanvas(float aaWidth = 1.0f) : m_aa(aaWidth) {}

    void clear() { m_verts.clear(); m_subpaths.clear(); m_cur.clear(); }
    const std::vector<JRenderVertex>& geometry() const { return m_verts; }
    bool empty() const { return m_verts.empty(); }

    // Emit accumulated geometry into a JPrimitiveBuffer (does not clear).
    void flush(JPrimitiveBuffer& buf) const {
        if (m_verts.size() >= 3) buf.pushGeometry(m_verts);
    }

    void setAntiAlias(float px) { m_aa = px; }
    float antiAlias() const { return m_aa; }

    // ===================== Path API =====================
    void beginPath() { m_subpaths.clear(); m_cur.clear(); }
    void moveTo(float x, float y) { _flushSub(); m_cur.push_back({x, y}); }
    void lineTo(float x, float y) { if (m_cur.empty()) m_cur.push_back({x, y}); else m_cur.push_back({x, y}); }
    void close() { m_closed = true; }

    void quadTo(float cx, float cy, float x, float y) {
        if (m_cur.empty()) m_cur.push_back({0, 0});
        JVec2 p0 = m_cur.back();
        int n = _bezierSegments(p0, {cx, cy}, {cx, cy}, {x, y});
        for (int i = 1; i <= n; ++i) {
            float t = static_cast<float>(i) / n, u = 1 - t;
            float a = u * u, b = 2 * u * t, c = t * t;
            m_cur.push_back({a * p0.x + b * cx + c * x, a * p0.y + b * cy + c * y});
        }
    }
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        if (m_cur.empty()) m_cur.push_back({0, 0});
        JVec2 p0 = m_cur.back();
        int n = _bezierSegments(p0, {c1x, c1y}, {c2x, c2y}, {x, y});
        for (int i = 1; i <= n; ++i) {
            float t = static_cast<float>(i) / n, u = 1 - t;
            float a = u*u*u, b = 3*u*u*t, c = 3*u*t*t, d = t*t*t;
            m_cur.push_back({a*p0.x + b*c1x + c*c2x + d*x, a*p0.y + b*c1y + c*c2y + d*y});
        }
    }
    void arcTo(float cx, float cy, float r, float a0, float a1, int segs = 0) {
        auto pts = arcPoints(cx, cy, r, a0, a1, segs);
        for (auto& p : pts) m_cur.push_back(p);
    }

    // Fill / stroke the current path's subpaths.
    void fill(const JPaint& paint) {
        _flushSub();
        for (auto& sp : m_subpaths) fillConvex(sp, paint);
    }
    void stroke(float width, const JPaint& paint,
                JLineCap cap = JLineCap::Butt, JLineJoin join = JLineJoin::Miter) {
        _flushSub();
        for (auto& sp : m_subpaths) strokePolyline(sp, width, paint, m_closed, cap, join);
    }

    // ===================== Convenience shapes =====================
    void drawLine(float x0, float y0, float x1, float y1, float width,
                  const JPaint& paint, JLineCap cap = JLineCap::Butt) {
        std::vector<JVec2> pts{{x0, y0}, {x1, y1}};
        strokePolyline(pts, width, paint, false, cap, JLineJoin::Miter);
    }
    void strokePolyline(const std::vector<JVec2>& pts, float width, const JPaint& paint,
                        bool closed = false, JLineCap cap = JLineCap::Butt,
                        JLineJoin join = JLineJoin::Miter) {
        _strokePolyline(pts, width, paint, closed, cap, join);
    }
    void fillConvex(const std::vector<JVec2>& pts, const JPaint& paint) {
        _fillConvexAA(pts, paint);
    }

    void fillRect(float x, float y, float w, float h, const JPaint& paint) {
        fillConvex({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, paint);
    }
    void strokeRect(float x, float y, float w, float h, float width, const JPaint& paint) {
        strokePolyline({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, width, paint, true);
    }
    void fillRoundedRect(float x, float y, float w, float h, float r, const JPaint& paint) {
        fillConvex(roundedRectPoints(x, y, w, h, r), paint);
    }
    void strokeRoundedRect(float x, float y, float w, float h, float r, float width, const JPaint& paint) {
        strokePolyline(roundedRectPoints(x, y, w, h, r), width, paint, true);
    }
    void fillCircle(float cx, float cy, float r, const JPaint& paint) {
        fillConvex(arcPoints(cx, cy, r, 0.f, 2.f * kPi, 0), paint);
    }
    void strokeCircle(float cx, float cy, float r, float width, const JPaint& paint) {
        strokePolyline(arcPoints(cx, cy, r, 0.f, 2.f * kPi, 0), width, paint, true);
    }
    void fillEllipse(float cx, float cy, float rx, float ry, const JPaint& paint) {
        fillConvex(ellipsePoints(cx, cy, rx, ry, 0), paint);
    }
    // Stroked arc band (e.g. a gauge sweep): from a0 to a1 (radians), thickness = width.
    void strokeArc(float cx, float cy, float r, float a0, float a1, float width,
                   const JPaint& paint, JLineCap cap = JLineCap::Butt) {
        strokePolyline(arcPoints(cx, cy, r, a0, a1, 0), width, paint, false, cap);
    }
    // Filled pie wedge (center + arc).
    void fillPie(float cx, float cy, float r, float a0, float a1, const JPaint& paint) {
        std::vector<JVec2> pts;
        pts.push_back({cx, cy});
        auto a = arcPoints(cx, cy, r, a0, a1, 0);
        pts.insert(pts.end(), a.begin(), a.end());
        _fillConvexAA(pts, paint, /*forceConvex=*/true);
    }
    // Filled annulus (ring) between rInner and rOuter over [a0,a1].
    void fillRing(float cx, float cy, float rInner, float rOuter, float a0, float a1,
                  const JPaint& paint) {
        auto outer = arcPoints(cx, cy, rOuter, a0, a1, 0);
        auto inner = arcPoints(cx, cy, rInner, a1, a0, 0);  // reversed
        std::vector<JVec2> poly = outer;
        poly.insert(poly.end(), inner.begin(), inner.end());
        // Ring is non-convex as a whole; tessellate as a quad strip between rings.
        size_t n = outer.size();
        auto innerFwd = arcPoints(cx, cy, rInner, a0, a1, 0);
        for (size_t i = 0; i + 1 < n; ++i) {
            _quad({outer[i], paint}, {outer[i + 1], paint},
                  {innerFwd[i + 1], paint}, {innerFwd[i], paint});
        }
    }

    // ===================== Geometry generators =====================
    static std::vector<JVec2> arcPoints(float cx, float cy, float r, float a0, float a1, int segs) {
        std::vector<JVec2> pts;
        float sweep = a1 - a0;
        if (segs <= 0) {
            float n = std::ceil(std::fabs(sweep) / (kPi / 24.f));  // ~7.5° per segment
            segs = static_cast<int>(n < 2 ? 2 : n);
        }
        pts.reserve(segs + 1);
        for (int i = 0; i <= segs; ++i) {
            float a = a0 + sweep * (static_cast<float>(i) / segs);
            pts.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r});
        }
        return pts;
    }
    static std::vector<JVec2> ellipsePoints(float cx, float cy, float rx, float ry, int segs) {
        if (segs <= 0) segs = static_cast<int>(std::ceil(2.f * kPi / (kPi / 24.f)));
        std::vector<JVec2> pts; pts.reserve(segs);
        for (int i = 0; i < segs; ++i) {
            float a = 2.f * kPi * (static_cast<float>(i) / segs);
            pts.push_back({cx + std::cos(a) * rx, cy + std::sin(a) * ry});
        }
        return pts;
    }
    static std::vector<JVec2> roundedRectPoints(float x, float y, float w, float h, float r) {
        float rr = r;
        rr = std::min(rr, std::min(w, h) * 0.5f);
        if (rr <= 0.5f) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        std::vector<JVec2> pts;
        auto corner = [&](float ccx, float ccy, float a0, float a1) {
            auto a = arcPoints(ccx, ccy, rr, a0, a1, 0);
            pts.insert(pts.end(), a.begin(), a.end());
        };
        corner(x + w - rr, y + rr,     -kPi * 0.5f, 0.f);          // top-right
        corner(x + w - rr, y + h - rr,  0.f,        kPi * 0.5f);   // bottom-right
        corner(x + rr,     y + h - rr,  kPi * 0.5f, kPi);          // bottom-left
        corner(x + rr,     y + rr,      kPi,        kPi * 1.5f);   // top-left
        return pts;
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    struct JCV { JVec2 p; JPaint paint; };  // a vertex carrying its paint for colour eval

    float m_aa{1.0f};
    bool  m_closed{false};
    std::vector<JRenderVertex> m_verts;
    std::vector<std::vector<JVec2>> m_subpaths;
    std::vector<JVec2> m_cur;

    void _flushSub() {
        if (!m_cur.empty()) { m_subpaths.push_back(m_cur); m_cur.clear(); }
    }

    static JVec2 _sub(JVec2 a, JVec2 b) { return {a.x - b.x, a.y - b.y}; }
    static float _len(JVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
    static JVec2 _norm(JVec2 a) { float l = _len(a); return l > 1e-6f ? JVec2{a.x / l, a.y / l} : JVec2{0, 0}; }
    static JVec2 _perp(JVec2 d) { return {-d.y, d.x}; }  // left normal of a direction

    JRenderVertex _mk(JVec2 p, JColor c) {
        JRenderVertex v;
        v.position[0] = p.x; v.position[1] = p.y;
        v.texCoord[0] = 0;   v.texCoord[1] = 0;
        v.color[0] = c.r; v.color[1] = c.g; v.color[2] = c.b; v.color[3] = c.a;
        return v;
    }
    void _tri(JRenderVertex a, JRenderVertex b, JRenderVertex c) {
        m_verts.push_back(a); m_verts.push_back(b); m_verts.push_back(c);
    }
    // Quad a-b-c-d (CCW or CW) carrying per-corner paint, evaluated at each corner.
    void _quad(JCV a, JCV b, JCV c, JCV d) {
        JRenderVertex va = _mk(a.p, a.paint.eval(a.p.x, a.p.y));
        JRenderVertex vb = _mk(b.p, b.paint.eval(b.p.x, b.p.y));
        JRenderVertex vc = _mk(c.p, c.paint.eval(c.p.x, c.p.y));
        JRenderVertex vd = _mk(d.p, d.paint.eval(d.p.x, d.p.y));
        _tri(va, vb, vc); _tri(va, vc, vd);
    }
    // Quad with explicit colours (used for fringe alpha fades).
    void _quadC(JVec2 a, JColor ca, JVec2 b, JColor cb, JVec2 c, JColor cc, JVec2 d, JColor cd) {
        _tri(_mk(a, ca), _mk(b, cb), _mk(c, cc));
        _tri(_mk(a, ca), _mk(c, cc), _mk(d, cd));
    }

    static JVec2 _centroid(const std::vector<JVec2>& pts) {
        JVec2 c{0, 0};
        for (auto& p : pts) { c.x += p.x; c.y += p.y; }
        float n = static_cast<float>(pts.size());
        if (n > 0) { c.x /= n; c.y /= n; }
        return c;
    }

    // Fan-fill a convex polygon + anti-aliased outward fringe.
    void _fillConvexAA(const std::vector<JVec2>& pts, const JPaint& paint, bool forceConvex = false) {
        (void)forceConvex;
        if (pts.size() < 3) return;
        // Interior fan.
        JRenderVertex v0 = _mk(pts[0], paint.eval(pts[0].x, pts[0].y));
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            _tri(v0,
                 _mk(pts[i],     paint.eval(pts[i].x, pts[i].y)),
                 _mk(pts[i + 1], paint.eval(pts[i + 1].x, pts[i + 1].y)));
        }
        // Outward fringe: push each edge outward by m_aa, fading alpha to 0.
        if (m_aa <= 0.f) return;
        JVec2 ctr = _centroid(pts);
        size_t n = pts.size();
        for (size_t i = 0; i < n; ++i) {
            JVec2 a = pts[i], b = pts[(i + 1) % n];
            JVec2 dir = _norm(_sub(b, a));
            JVec2 nrm = _perp(dir);
            // Ensure normal points away from centroid (outward).
            JVec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            if ((nrm.x * (mid.x - ctr.x) + nrm.y * (mid.y - ctr.y)) < 0) { nrm.x = -nrm.x; nrm.y = -nrm.y; }
            JVec2 ao{a.x + nrm.x * m_aa, a.y + nrm.y * m_aa};
            JVec2 bo{b.x + nrm.x * m_aa, b.y + nrm.y * m_aa};
            JColor ca = paint.eval(a.x, a.y), cb = paint.eval(b.x, b.y);
            _quadC(a, ca, b, cb, bo, withAlpha(cb, 0), ao, withAlpha(ca, 0));
        }
    }

    // Stroke a polyline with miter joins + caps + outward fringe on both sides.
    void _strokePolyline(const std::vector<JVec2>& inPts, float width, const JPaint& paint,
                         bool closed, JLineCap cap, JLineJoin join) {
        (void)join;
        std::vector<JVec2> pts = inPts;
        if (pts.size() < 2) return;
        if (closed && pts.size() >= 2) {
            // ensure first != last
            if (std::fabs(pts.front().x - pts.back().x) < 1e-4f &&
                std::fabs(pts.front().y - pts.back().y) < 1e-4f) pts.pop_back();
            pts.push_back(pts.front());
        }
        float hw = width * 0.5f;
        size_t n = pts.size();

        // Per-vertex left/right offset points (miter where interior, perpendicular at ends).
        std::vector<JVec2> left(n), right(n);
        for (size_t i = 0; i < n; ++i) {
            JVec2 dPrev{0, 0}, dNext{0, 0};
            if (i > 0)       dPrev = _norm(_sub(pts[i], pts[i - 1]));
            if (i + 1 < n)   dNext = _norm(_sub(pts[i + 1], pts[i]));
            if (i == 0)      dPrev = dNext;
            if (i + 1 == n)  dNext = dPrev;
            JVec2 nPrev = _perp(dPrev), nNext = _perp(dNext);
            JVec2 m{nPrev.x + nNext.x, nPrev.y + nNext.y};
            float ml = _len(m);
            JVec2 mn = ml > 1e-4f ? JVec2{m.x / ml, m.y / ml} : nNext;
            float denom = mn.x * nNext.x + mn.y * nNext.y;   // cos(theta/2)
            float scale = denom > 0.1f ? (hw / denom) : hw;  // clamp miter spikes
            if (scale > hw * 4.f) scale = hw * 4.f;
            left[i]  = {pts[i].x + mn.x * scale, pts[i].y + mn.y * scale};
            right[i] = {pts[i].x - mn.x * scale, pts[i].y - mn.y * scale};
        }

        // Core quad strip.
        for (size_t i = 0; i + 1 < n; ++i) {
            _quad({left[i], paint}, {left[i + 1], paint},
                  {right[i + 1], paint}, {right[i], paint});
        }
        // Outward fringe on both boundaries.
        if (m_aa > 0.f) {
            for (size_t i = 0; i + 1 < n; ++i) {
                JVec2 ld = _norm(_perp(_norm(_sub(left[i + 1], left[i]))));  // outward-ish on left
                // left boundary outward = away from centerline (use left-right direction)
                JVec2 outL = _norm(_sub(left[i], pts[i]));
                JVec2 outL2 = _norm(_sub(left[i + 1], pts[i + 1]));
                (void)ld;
                JColor c0 = paint.eval(left[i].x, left[i].y);
                JColor c1 = paint.eval(left[i + 1].x, left[i + 1].y);
                JVec2 lo0{left[i].x + outL.x * m_aa, left[i].y + outL.y * m_aa};
                JVec2 lo1{left[i + 1].x + outL2.x * m_aa, left[i + 1].y + outL2.y * m_aa};
                _quadC(left[i], c0, left[i + 1], c1, lo1, withAlpha(c1, 0), lo0, withAlpha(c0, 0));

                JVec2 outR = _norm(_sub(right[i], pts[i]));
                JVec2 outR2 = _norm(_sub(right[i + 1], pts[i + 1]));
                JColor r0 = paint.eval(right[i].x, right[i].y);
                JColor r1 = paint.eval(right[i + 1].x, right[i + 1].y);
                JVec2 ro0{right[i].x + outR.x * m_aa, right[i].y + outR.y * m_aa};
                JVec2 ro1{right[i + 1].x + outR2.x * m_aa, right[i + 1].y + outR2.y * m_aa};
                _quadC(right[i], r0, right[i + 1], r1, ro1, withAlpha(r1, 0), ro0, withAlpha(r0, 0));
            }
        }
        // Round caps: filled half-discs at the open ends.
        if (!closed && cap == JLineCap::Round) {
            JVec2 d0 = _norm(_sub(pts[1], pts[0]));
            float a0 = std::atan2(d0.y, d0.x);
            fillConvex(arcPoints(pts[0].x, pts[0].y, hw, a0 + kPi * 0.5f, a0 + kPi * 1.5f, 0), paint);
            JVec2 d1 = _norm(_sub(pts[n - 1], pts[n - 2]));
            float a1 = std::atan2(d1.y, d1.x);
            fillConvex(arcPoints(pts[n - 1].x, pts[n - 1].y, hw, a1 - kPi * 0.5f, a1 + kPi * 0.5f, 0), paint);
        } else if (!closed && cap == JLineCap::Square) {
            // extend ends by hw — handled implicitly by butt here; left simple for v1.
        }
    }

    // Adaptive subdivision count for a cubic/quadratic span by chord length.
    static int _bezierSegments(JVec2 p0, JVec2 c1, JVec2 c2, JVec2 p3) {
        float approx = _len(_sub(c1, p0)) + _len(_sub(c2, c1)) + _len(_sub(p3, c2));
        int n = static_cast<int>(approx / 6.f);   // ~6px per segment
        if (n < 4)  n = 4;
        if (n > 96) n = 96;
        return n;
    }
};

}  // inline namespace jf
