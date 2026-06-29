#pragma once

// ============================================================================
// jf::JChart — line/area/bar/scatter chart built on JVectorCanvas.
//
// Anti-aliased GPU vector rendering with: nice-number axis ticks, optional log
// scales, a second (right) Y-axis, axis titles, a legend, multiple series of
// mixed type (line / area / bar / scatter, with optional point markers), and
// interaction (hover crosshair + value tooltip, zoom/pan helpers). Works for
// static XY plots (power/torque sweeps) and live strip charts (telemetry) via
// addPoint()/setXWindow().
//
//   JChart c;
//   c.setRect(20, 40, 600, 300);
//   int s = c.addSeries("RPM", rgb(80,200,255), 2.0f, /*area=*/true);
//   c.series(s).markers = true;                  // dots on the line
//   int b = c.addSeries("Boost", rgb(255,170,60));
//   c.series(b).secondary = true;                // right Y-axis
//   for (...) c.addPoint(s, x, y);
//   c.setHover(mouseX, mouseY);                  // optional: crosshair + tooltip
//   c.render(buffer);
// ============================================================================

#include <genesis/graphics/VectorGraphics.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <genesis/core/BaseWidgets.h>   // TextHelper
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>

inline namespace jf {

enum class JSeriesType : uint8_t { Line, Bar, Scatter };

struct JChartSeries {
    std::string            label;
    JColor                  color{rgb(80, 200, 255)};
    float                  width{2.0f};
    bool                   area{false};       // Line: fill under the curve
    JSeriesType             type{JSeriesType::Line};
    bool                   markers{false};    // Line: draw a dot at each point
    float                  markerSize{3.0f};
    bool                   secondary{false};  // plot against the right Y-axis
    std::vector<JVectorCanvas::JVec2> pts;       // data points (x,y in data space)
};

class JChart {
public:
    using JVec2 = JVectorCanvas::JVec2;

    // ---- Configuration ----
    void setRect(float x, float y, float w, float h) { m_x = x; m_y = y; m_w = w; m_h = h; }
    void setTitle(std::string t) { m_title = std::move(t); }
    void setBackground(JColor c) { m_bg = c; m_hasBg = true; }
    void setShowGrid(bool v)   { m_grid = v; }
    void setShowLegend(bool v) { m_legend = v; }
    void setShowAxes(bool v)   { m_axes = v; }

    void setXRange(double lo, double hi) { m_xLo = lo; m_xHi = hi; m_autoX = false; }
    void setYRange(double lo, double hi) { m_yLo = lo; m_yHi = hi; m_autoY = false; }
    void setY2Range(double lo, double hi){ m_y2Lo = lo; m_y2Hi = hi; m_autoY2 = false; }
    void setAutoX(bool v) { m_autoX = v; }
    void setAutoY(bool v) { m_autoY = v; }
    void setLogX(bool v) { m_xLog = v; }
    void setLogY(bool v) { m_yLog = v; }
    void setAxisTitles(std::string x, std::string y, std::string y2 = "") {
        m_xLabel = std::move(x); m_yLabel = std::move(y); m_y2Label = std::move(y2);
    }
    void setApproxTicks(int x, int y) { m_xTickN = x; m_yTickN = y; }
    // Keep only the most recent `span` x-units of every series (0 = unlimited).
    void setXWindow(double span) { m_xWindow = span; }

    // ---- Series ----
    int addSeries(std::string label, JColor color, float width = 2.0f, bool area = false) {
        JChartSeries s; s.label = std::move(label); s.color = color; s.width = width; s.area = area;
        m_series.push_back(std::move(s));
        return static_cast<int>(m_series.size()) - 1;
    }
    JChartSeries& series(int i) { return m_series[i]; }
    int seriesCount() const { return static_cast<int>(m_series.size()); }
    void clearSeries() { m_series.clear(); }
    void clearData() { for (auto& s : m_series) s.pts.clear(); }

    void addPoint(int s, double x, double y) {
        if (s < 0 || s >= (int)m_series.size()) return;
        auto& pts = m_series[s].pts;
        pts.push_back({static_cast<float>(x), static_cast<float>(y)});
        if (m_xWindow > 0 && !pts.empty()) {
            double cutoff = pts.back().x - m_xWindow;
            size_t drop = 0;
            while (drop < pts.size() && pts[drop].x < cutoff) ++drop;
            if (drop) pts.erase(pts.begin(), pts.begin() + drop);
        }
    }

    // ---- Interaction ----
    void setHover(float px, float py) { m_hoverPx = px; m_hoverPy = py; m_hasHover = true; }
    void clearHover() { m_hasHover = false; }
    // Zoom the X-axis by `factor` (<1 zooms in) about a data-space centre.
    void zoomX(double factor, double centerData) {
        double lo = m_curXLo, hi = m_curXHi;
        if (hi <= lo) return;
        m_xLo = centerData - (centerData - lo) * factor;
        m_xHi = centerData + (hi - centerData) * factor;
        m_autoX = false;
    }
    void panX(double deltaData) { m_xLo = m_curXLo + deltaData; m_xHi = m_curXHi + deltaData; m_autoX = false; }
    // Convert a pixel X to a data-space X using the last rendered layout.
    double pixelToDataX(float pxX) const {
        if (m_plotW <= 0) return 0;
        double t = (pxX - m_plotX) / m_plotW;
        if (m_xLog) {
            double a = std::log10(std::max(m_curXLo, 1e-12)), b = std::log10(std::max(m_curXHi, 1e-12));
            return std::pow(10.0, a + t * (b - a));
        }
        return m_curXLo + t * (m_curXHi - m_curXLo);
    }

    // ---- Render everything into a JPrimitiveBuffer ----
    void render(JPrimitiveBuffer& buf) {
        bool hasSecondary = false;
        for (auto& s : m_series) if (s.secondary) hasSecondary = true;

        double xLo, xHi, yLo, yHi, y2Lo, y2Hi;
        _range(/*x*/true, false, xLo, xHi);
        _range(/*y primary*/false, false, yLo, yHi);
        if (hasSecondary) _range(false, true, y2Lo, y2Hi); else { y2Lo = 0; y2Hi = 1; }

        std::vector<double> xticks = m_xLog ? _logTicks(xLo, xHi) : _niceTicks(xLo, xHi, m_xTickN);
        std::vector<double> yticks = m_yLog ? _logTicks(yLo, yHi) : _niceTicks(yLo, yHi, m_yTickN);
        std::vector<double> y2ticks = hasSecondary ? _niceTicks(y2Lo, y2Hi, m_yTickN) : std::vector<double>{};

        // Margins (room for labels / titles / legend).
        float ml = m_axes ? (m_yLabel.empty() ? 48.f : 62.f) : 6.f;
        float mr = (hasSecondary ? (m_y2Label.empty() ? 46.f : 60.f) : 12.f);
        float mb = m_axes ? (m_xLabel.empty() ? 22.f : 36.f) : 6.f;
        float mt = (!m_title.empty() ? 24.f : 8.f);
        float px = m_x + ml, py = m_y + mt;
        float pw = m_w - ml - mr, ph = m_h - mt - mb;
        if (pw < 4 || ph < 4) return;

        // Cache layout for interaction.
        m_plotX = px; m_plotY = py; m_plotW = pw; m_plotH = ph;
        m_curXLo = xLo; m_curXHi = xHi;

        auto mapX  = [&](double v) { return static_cast<float>(px + _t(v, xLo, xHi, m_xLog) * pw); };
        auto mapY  = [&](double v) { return static_cast<float>(py + ph - _t(v, yLo, yHi, m_yLog) * ph); };
        auto mapY2 = [&](double v) { return static_cast<float>(py + ph - _t(v, y2Lo, y2Hi, false) * ph); };

        JVectorCanvas vg;
        if (m_hasBg) {
            vg.fillRoundedRect(m_x, m_y, m_w, m_h, 8.f, JPaint::solid(m_bg));
            vg.strokeRoundedRect(m_x, m_y, m_w, m_h, 8.f, 1.f, JPaint::solid(_shade(m_bg, 1.5f)));
        }
        if (m_grid) {
            JColor gl = m_hasBg ? _shade(m_bg, 1.7f) : rgb(45, 48, 58);
            for (double tv : xticks) { float gx = mapX(tv); vg.drawLine(gx, py, gx, py + ph, 1.f, JPaint::solid(gl)); }
            for (double tv : yticks) { float gy = mapY(tv); vg.drawLine(px, gy, px + pw, gy, 1.f, JPaint::solid(gl)); }
        }
        vg.flush(buf);

        // ---- Series (clipped to plot) ----
        buf.pushClip(px, py, pw, ph);
        JVectorCanvas sg;
        int barSeries = 0; for (auto& s : m_series) if (s.type == JSeriesType::Bar) ++barSeries;
        int barIdx = 0;
        for (auto& s : m_series) {
            auto JMY = [&](double v) { return s.secondary ? mapY2(v) : mapY(v); };
            if (s.pts.empty()) continue;

            if (s.type == JSeriesType::Bar) {
                double baseV = s.secondary ? std::max(0.0, y2Lo) : std::max(0.0, yLo);
                float baseY = JMY(baseV);
                float slot = (s.pts.size() > 1)
                    ? (mapX(s.pts[1].x) - mapX(s.pts[0].x)) : (pw / 8.f);
                float bw = std::max(2.f, slot * 0.8f / std::max(1, barSeries));
                float off = (barSeries > 1) ? (barIdx - (barSeries - 1) * 0.5f) * bw : 0.f;
                for (auto& p : s.pts) {
                    float cx = mapX(p.x) + off, vy = JMY(p.y);
                    float top = std::min(vy, baseY), hgt = std::fabs(vy - baseY);
                    sg.fillRect(cx - bw * 0.5f, top, bw, hgt, JPaint::solid(withAlpha(s.color, 210)));
                }
                ++barIdx;
                continue;
            }

            if (s.type == JSeriesType::Scatter || s.markers) {
                for (auto& p : s.pts)
                    sg.fillCircle(mapX(p.x), JMY(p.y), s.markerSize, JPaint::solid(s.color));
                if (s.type == JSeriesType::Scatter) continue;
            }
            if (s.pts.size() < 2) continue;

            if (s.area) {
                double baseV = s.secondary ? std::max(0.0, y2Lo) : std::max(0.0, yLo);
                float baseY = JMY(baseV);
                JColor top = withAlpha(s.color, 90), bot = withAlpha(s.color, 12);
                for (size_t i = 0; i + 1 < s.pts.size(); ++i) {
                    JVec2 a{mapX(s.pts[i].x),   JMY(s.pts[i].y)};
                    JVec2 b{mapX(s.pts[i+1].x), JMY(s.pts[i+1].y)};
                    sg.fillConvex({{a.x, baseY}, a, b, {b.x, baseY}},
                                  JPaint::linear(a.x, py, top, a.x, baseY, bot));
                }
            }
            sg.beginPath();
            for (size_t i = 0; i < s.pts.size(); ++i) {
                float vx = mapX(s.pts[i].x), vy = JMY(s.pts[i].y);
                if (i == 0) sg.moveTo(vx, vy); else sg.lineTo(vx, vy);
            }
            sg.stroke(s.width, JPaint::solid(s.color), JLineCap::Round, JLineJoin::Round);
        }
        sg.flush(buf);
        buf.popClip();

        // ---- Hover crosshair + tooltip ----
        if (m_hasHover && m_hoverPx >= px && m_hoverPx <= px + pw &&
            m_hoverPy >= py && m_hoverPy <= py + ph) {
            _renderHover(buf, px, py, pw, ph, xLo, xHi, mapX, mapY, mapY2);
        }

        // ---- Text labels ----
        if (!JTextHelper::hasAtlas()) return;
        float lh = JTextHelper::lineHeight();
        if (!m_title.empty()) {
            uint8_t tc[4]{220, 222, 230, 235};
            JTextHelper::pushText(buf, m_x + 10.f, m_y + (mt - lh) * 0.5f, m_title, tc, m_w - 20.f);
        }
        if (m_axes) {
            uint8_t ac[4]{150, 154, 165, 220};
            for (double tv : yticks) {
                std::string l = _fmt(tv); float tw = JTextHelper::measureWidth(l);
                JTextHelper::pushText(buf, px - 6.f - tw, mapY(tv) - lh * 0.5f, l, ac);
            }
            for (double tv : xticks) {
                std::string l = _fmt(tv); float tw = JTextHelper::measureWidth(l);
                JTextHelper::pushText(buf, mapX(tv) - tw * 0.5f, py + ph + 4.f, l, ac);
            }
            if (hasSecondary) {
                uint8_t a2[4]{160, 150, 140, 220};
                for (double tv : y2ticks)
                    JTextHelper::pushText(buf, px + pw + 6.f, mapY2(tv) - lh * 0.5f, _fmt(tv), a2);
            }
            // Axis titles.
            if (!m_xLabel.empty()) {
                float tw = JTextHelper::measureWidth(m_xLabel);
                JTextHelper::pushText(buf, px + pw * 0.5f - tw * 0.5f, m_y + m_h - lh - 2.f, m_xLabel, ac);
            }
            if (!m_yLabel.empty())
                JTextHelper::pushText(buf, m_x + 6.f, py - lh - 2.f, m_yLabel, ac);
            if (hasSecondary && !m_y2Label.empty()) {
                float tw = JTextHelper::measureWidth(m_y2Label);
                JTextHelper::pushText(buf, px + pw - tw, py - lh - 2.f, m_y2Label, ac);
            }
        }
        if (m_legend) {
            float lx = px + 8.f, ly = py + 6.f;
            JVectorCanvas lg;
            for (auto& s : m_series) {
                lg.fillRect(lx, ly + lh * 0.5f - 2.f, 14.f, 4.f, JPaint::solid(s.color));
                uint8_t lc[4]{200, 204, 214, 230};
                JTextHelper::pushText(buf, lx + 20.f, ly, s.label, lc);
                ly += lh + 4.f;
            }
            lg.flush(buf);
        }
    }

private:
    float m_x{0}, m_y{0}, m_w{200}, m_h{120};
    std::string m_title, m_xLabel, m_yLabel, m_y2Label;
    JColor m_bg{rgb(22, 24, 31)};
    bool  m_hasBg{true}, m_grid{true}, m_legend{true}, m_axes{true};
    bool  m_autoX{true}, m_autoY{true}, m_autoY2{true};
    bool  m_xLog{false}, m_yLog{false};
    int   m_xTickN{6}, m_yTickN{5};
    double m_xLo{0}, m_xHi{1}, m_yLo{0}, m_yHi{1}, m_y2Lo{0}, m_y2Hi{1};
    double m_xWindow{0};
    std::vector<JChartSeries> m_series;

    // Interaction / cached layout.
    bool  m_hasHover{false};
    float m_hoverPx{0}, m_hoverPy{0};
    float m_plotX{0}, m_plotY{0}, m_plotW{0}, m_plotH{0};
    double m_curXLo{0}, m_curXHi{1};

    static double _t(double v, double lo, double hi, bool log) {
        if (log) {
            double l = std::log10(std::max(v, 1e-12));
            double a = std::log10(std::max(lo, 1e-12)), b = std::log10(std::max(hi, 1e-12));
            return (b > a) ? (l - a) / (b - a) : 0.0;
        }
        return (hi > lo) ? (v - lo) / (hi - lo) : 0.0;
    }
    static JColor _shade(JColor c, float f) {
        auto S = [&](uint8_t v) { int r = (int)(v * f); return (uint8_t)(r > 255 ? 255 : r); };
        return {S(c.r), S(c.g), S(c.b), c.a};
    }
    static std::string _fmt(double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.4g", v); return std::string(b);
    }

    void _range(bool xAxis, bool secondary, double& lo, double& hi) {
        if (xAxis && !m_autoX) { lo = m_xLo; hi = m_xHi; return; }
        if (!xAxis && !secondary && !m_autoY) { lo = m_yLo; hi = m_yHi; return; }
        if (!xAxis && secondary && !m_autoY2) { lo = m_y2Lo; hi = m_y2Hi; return; }
        lo = 1e300; hi = -1e300;
        for (auto& s : m_series) {
            if (!xAxis && s.secondary != secondary) continue;
            for (auto& p : s.pts) {
                double v = xAxis ? (double)p.x : (double)p.y;
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        }
        if (lo > hi) { lo = 0; hi = 1; }
        if (!xAxis) {  // pad Y a touch
            double pad = (hi - lo) * 0.08;
            if (pad <= 0) pad = (hi == 0 ? 1.0 : std::fabs(hi) * 0.1);
            lo -= pad; hi += pad;
        }
        if (hi - lo < 1e-9) hi = lo + 1;
    }

    static double _niceNum(double range, bool round) {
        double expv = std::floor(std::log10(range));
        double f = range / std::pow(10.0, expv);
        double nf;
        if (round) nf = (f < 1.5) ? 1 : (f < 3) ? 2 : (f < 7) ? 5 : 10;
        else       nf = (f <= 1)  ? 1 : (f <= 2) ? 2 : (f <= 5) ? 5 : 10;
        return nf * std::pow(10.0, expv);
    }
    static std::vector<double> _niceTicks(double& lo, double& hi, int approx) {
        std::vector<double> ticks;
        if (approx < 2) approx = 2;
        double range = _niceNum(hi - lo, false);
        if (range <= 0) range = 1;
        double d = _niceNum(range / (approx - 1), true);
        double gLo = std::floor(lo / d) * d;
        double gHi = std::ceil(hi / d) * d;
        for (double v = gLo; v <= gHi + d * 0.5; v += d) {
            // snap near-zero noise
            if (std::fabs(v) < d * 1e-6) v = 0;
            ticks.push_back(v);
        }
        lo = gLo; hi = gHi;   // expand axis to the nice bounds
        return ticks;
    }
    static std::vector<double> _logTicks(double& lo, double& hi) {
        if (lo <= 0) lo = (hi > 1) ? 1.0 : hi * 0.1;
        std::vector<double> ticks;
        int a = (int)std::floor(std::log10(std::max(lo, 1e-12)));
        int b = (int)std::ceil(std::log10(std::max(hi, 1e-12)));
        for (int k = a; k <= b; ++k) ticks.push_back(std::pow(10.0, k));
        lo = std::pow(10.0, a); hi = std::pow(10.0, b);
        return ticks;
    }

    template<class JMX, class JMY, class JMY2>
    void _renderHover(JPrimitiveBuffer& buf, float px, float py, float pw, float ph,
                      double xLo, double xHi, JMX mapX, JMY mapY, JMY2 mapY2) {
        double hx = pixelToDataX(m_hoverPx);
        JVectorCanvas hv;
        // vertical crosshair
        hv.drawLine(m_hoverPx, py, m_hoverPx, py + ph, 1.f, JPaint::solid(rgba(255, 255, 255, 70)));
        struct JHit { JColor c; std::string label; double y; float sx, sy; };
        std::vector<JHit> hits;
        for (auto& s : m_series) {
            if (s.pts.empty()) continue;
            // nearest point by x
            size_t best = 0; double bd = 1e300;
            for (size_t i = 0; i < s.pts.size(); ++i) {
                double d = std::fabs((double)s.pts[i].x - hx);
                if (d < bd) { bd = d; best = i; }
            }
            float sx = mapX(s.pts[best].x);
            float sy = s.secondary ? mapY2(s.pts[best].y) : mapY(s.pts[best].y);
            hv.fillCircle(sx, sy, 4.f, JPaint::solid(s.color));
            hits.push_back({s.color, s.label, s.pts[best].y, sx, sy});
        }
        // tooltip box
        if (!hits.empty() && JTextHelper::hasAtlas()) {
            float lh = JTextHelper::lineHeight();
            float boxW = 0;
            std::vector<std::string> lines;
            lines.push_back("x = " + _fmt(hx));
            for (auto& h : hits) lines.push_back(h.label + ": " + _fmt(h.y));
            for (auto& l : lines) boxW = std::max(boxW, JTextHelper::measureWidth(l));
            boxW += 28.f;
            float boxH = lines.size() * (lh + 2.f) + 8.f;
            float bx = m_hoverPx + 12.f, by = py + 8.f;
            if (bx + boxW > px + pw) bx = m_hoverPx - boxW - 12.f;
            hv.fillRoundedRect(bx, by, boxW, boxH, 5.f, JPaint::solid(rgba(20, 22, 30, 235)));
            hv.strokeRoundedRect(bx, by, boxW, boxH, 5.f, 1.f, JPaint::solid(rgba(90, 96, 110, 255)));
            hv.flush(buf);
            float ty = by + 5.f;
            uint8_t hc[4]{210, 214, 224, 235};
            JTextHelper::pushText(buf, bx + 8.f, ty, lines[0], hc);
            ty += lh + 2.f;
            JVectorCanvas sw;
            for (size_t i = 1; i < lines.size(); ++i) {
                sw.fillRect(bx + 8.f, ty + lh * 0.5f - 2.f, 10.f, 4.f, JPaint::solid(hits[i - 1].c));
                JTextHelper::pushText(buf, bx + 22.f, ty, lines[i], hc);
                ty += lh + 2.f;
            }
            sw.flush(buf);
        } else {
            hv.flush(buf);
        }
    }
};

}  // inline namespace jf
