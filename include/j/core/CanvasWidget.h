#pragma once

#include <cmath>
#include <algorithm>
#include <string>
#include "BaseWidgets.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline namespace jf {

// ============================================================================
// JCanvas — immediate-mode drawing API passed to JCanvasWidget::draw().
// All coordinates are in the widget's local space (0,0 = top-left of widget).
// Internally maps to JPrimitiveBuffer calls with the widget's screen offset.
// ============================================================================
class JCanvas {
public:
    JCanvas(JPrimitiveBuffer& buf, float offsetX, float offsetY)
        : m_buf(buf), m_ox(offsetX), m_oy(offsetY) {}

    // --- Rectangles ---

    void fillRect(float x, float y, float w, float h,
                  const uint8_t color[4], float radius = 0.f) {
        static const uint8_t none[4] = {0, 0, 0, 0};
        m_buf.pushRectangle(m_ox + x, m_oy + y, w, h, color, radius, 0.f, none);
    }

    void strokeRect(float x, float y, float w, float h,
                    const uint8_t color[4], float lineWidth = 1.f, float radius = 0.f) {
        static const uint8_t none[4] = {0, 0, 0, 0};
        m_buf.pushRectangle(m_ox + x, m_oy + y, w, h, none, radius, lineWidth, color);
    }

    void rect(float x, float y, float w, float h,
              const uint8_t fill[4], float radius = 0.f,
              float borderWidth = 0.f, const uint8_t border[4] = nullptr) {
        static const uint8_t none[4] = {0, 0, 0, 0};
        m_buf.pushRectangle(m_ox + x, m_oy + y, w, h,
                            fill, radius, borderWidth, border ? border : none);
    }

    // --- Circles ---

    void fillCircle(float cx, float cy, float r, const uint8_t color[4]) {
        float d = r * 2.f;
        fillRect(cx - r, cy - r, d, d, color, r);
    }

    void strokeCircle(float cx, float cy, float r,
                      const uint8_t color[4], float lineWidth = 1.f) {
        float d = r * 2.f;
        strokeRect(cx - r, cy - r, d, d, color, lineWidth, r);
    }

    // --- Arc / Donut (for gauge tracks and needles) ---
    // Angles in degrees; 0° = right, positive = clockwise (screen coords).
    // Approximated as small filled circles along the arc path.
    // strokeWidth controls the track thickness.

    void arc(float cx, float cy, float r,
             float startDeg, float endDeg,
             const uint8_t color[4], float strokeWidth = 4.f) {
        float span  = endDeg - startDeg;
        // Number of dots: enough that gaps are smaller than dot radius
        int   steps = std::max(4, static_cast<int>(std::abs(span) * r * M_PI / 180.f / (strokeWidth * 0.6f)));
        float hw    = strokeWidth * 0.5f;
        float s0    = startDeg * static_cast<float>(M_PI) / 180.f;
        float s1    = endDeg   * static_cast<float>(M_PI) / 180.f;

        for (int i = 0; i <= steps; ++i) {
            float a = s0 + (s1 - s0) * (static_cast<float>(i) / steps);
            float x = cx + std::cos(a) * r;
            float y = cy + std::sin(a) * r;
            fillRect(x - hw, y - hw, strokeWidth, strokeWidth, color, hw);
        }
    }

    // --- Line ---
    // Approximate as a thin rectangle; for near-horizontal/vertical lines
    // this is exact; for diagonal lines it renders as an axis-aligned band.
    // Good enough for tick marks and simple rulers.
    void line(float x1, float y1, float x2, float y2,
              const uint8_t color[4], float width = 1.f) {
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx*dx + dy*dy);
        if (len < 0.5f) return;
        // Place a series of dots along the line (same technique as arc)
        int steps = std::max(1, static_cast<int>(len / (width * 0.5f)));
        float hw  = width * 0.5f;
        for (int i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps;
            float x = x1 + dx * t, y = y1 + dy * t;
            fillRect(x - hw, y - hw, width, width, color, hw);
        }
    }

    // --- Text ---

    void text(float x, float y, const std::string& str,
              const uint8_t color[4], float maxWidth = 0.f) {
        JTextHelper::pushText(m_buf, m_ox + x, m_oy + y, str, color, maxWidth);
    }

    void textCentered(float cx, float cy, const std::string& str, const uint8_t color[4]) {
        float tw = JTextHelper::measureWidth(str);
        float th = JTextHelper::lineHeight();
        JTextHelper::pushText(m_buf, m_ox + cx - tw * 0.5f, m_oy + cy - th * 0.5f, str, color);
    }

    // --- Convenience color constructors ---
    static void rgba(uint8_t out[4], uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        out[0] = r; out[1] = g; out[2] = b; out[3] = a;
    }

    // --- Coordinate helpers ---
    float offsetX() const { return m_ox; }
    float offsetY() const { return m_oy; }

private:
    JPrimitiveBuffer& m_buf;
    float m_ox, m_oy;
};

// ============================================================================
// JCanvasWidget — base class for custom-painted widgets.
//
// Subclass and override draw(). Properties are plain C++ members — no macros,
// no getters/setters required. Call invalidate() after changing a property to
// schedule a repaint.
//
// Example:
//   class JDialWidget : public JCanvasWidget {
//   public:
//       double value{0}, minValue{0}, maxValue{100};
//
//       void draw(JCanvas& c, float w, float h) override {
//           float r   = std::min(w, h) * 0.4f;
//           float pct = (value - minValue) / (maxValue - minValue);
//           c.arc(w/2, h/2, r, -220.f, -220.f + pct*260.f, Colors::Accent, 6.f);
//       }
//   };
// ============================================================================
class JCanvasWidget : public JWidget {
public:
    JCanvasWidget(JSceneGraph& graph, const std::string& debugName = "JCanvasWidget")
        : JWidget(graph, debugName) {}

    // Override this in your subclass.
    // w, h = current widget size in pixels (same as boundingBox.width/height).
    virtual void draw(JCanvas& canvas, float w, float h) = 0;

    void populateRenderPrimitives(JPrimitiveBuffer& buf) final {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        JCanvas c(buf, bb.x, bb.y);
        draw(c, bb.width, bb.height);
        drawFocusRing(buf);
    }

    // Schedule a repaint (call after mutating a property).
    void invalidate() {
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

};

} // inline namespace jf
