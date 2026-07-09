#pragma once

// JColorPicker — the custom-colour editor page of the colour dialog (the "+" popup): a vertical hue
// bar, a large saturation/value square, a top row of eyedropper + live preview + editable hex, and an
// alpha slider over a checkerboard. Drag the square/bar/alpha or type a hex; the value emits live via
// onColorChanged. The SV square is one bilinear quad (white→pure-hue across, →black down) — exactly
// HSV, so the GPU interpolates it for free; the hue bar is six gradient segments.
//
// Note: values serialise as "#rrggbb" (alpha is edited for preview but not encoded into the hex).

#include <j/core/JTextHelper.h>
#include <j/core/JWidget.h>
#include <j/graphics/RenderPrimitive.h>  // JPrimitiveBuffer, JRenderVertex, pushGeometry
#include <j/graphics/VectorGraphics.h>   // JVectorCanvas (eyedropper glyph)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

inline namespace jf {

class JColorPicker : public JWidget {
public:
    jf::JSignal<std::string> onColorChanged;   // "#rrggbb"
    jf::JSignal<>            onEyedropper;      // eyedropper button pressed (host wires screen-pick)

    JColorPicker(JSceneGraph& graph, float w = kW, float h = kH) : JWidget(graph, "JColorPicker") {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w; l.boundingBox.height = h;
        l.minWidth = w; l.minHeight = h;
    }

    void setColorHex(const std::string& hex) {
        uint8_t c[4];
        if (_parse(hex, c)) { _rgbToHsv(c[0], c[1], c[2]); m_a = 1.f; _syncHex(); }
        else                { m_h = 0.f; m_s = 0.f; m_v = 1.f; m_a = 1.f; _syncHex(); }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    const std::string& colorHex() const { return m_hex; }

    void handleMousePress(float mx, float my) override {
        const auto r = _regions();
        if      (_in(r.sv, mx, my))    { m_dragSV = true;  _setSV(mx, my, r); }
        else if (_in(r.hue, mx, my))   { m_dragHue = true; _setHue(my, r); }
        else if (_in(r.alpha, mx, my)) { m_dragA = true;   _setAlpha(mx, r); }
        else if (_in(r.eye, mx, my))   { onEyedropper.emit(); }
        m_hexFocus = _in(r.hex, mx, my);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    void handleMouseMove(float mx, float my) override {
        if (!m_dragSV && !m_dragHue && !m_dragA) return;
        const auto r = _regions();
        if (m_dragSV)  _setSV(mx, my, r);
        if (m_dragHue) _setHue(my, r);
        if (m_dragA)   _setAlpha(mx, r);
    }
    void handleMouseRelease(float, float) override { m_dragSV = m_dragHue = m_dragA = false; }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!m_hexFocus || !ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Backspace) { if (!m_hex.empty()) m_hex.pop_back(); _tryCommitHex(); return true; }
        if (ke.key == K::Return)    { _tryCommitHex(true); return true; }
        const char ch = ke.utf8[0];
        const bool hexCh = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == '#';
        if (hexCh && m_hex.size() < 7) { m_hex += ch; _tryCommitHex(); return true; }
        return false;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto r = _regions();
        uint8_t cur[4]; _hsvToRgb(m_h, m_s, m_v, cur);

        // ---- Top row: eyedropper button, preview swatch, hex field ----
        buf.pushRectangle(r.eye.x, r.eye.y, r.eye.w, r.eye.h, Colors::Surface2, 5.f, 1.f, Colors::Border);
        _drawEyedropper(buf, r.eye);
        buf.pushRectangle(r.preview.x, r.preview.y, r.preview.w, r.preview.h, cur, 5.f, 1.f, Colors::Border);
        buf.pushRectangle(r.hex.x, r.hex.y, r.hex.w, r.hex.h, Colors::Surface1, 5.f, m_hexFocus ? 1.5f : 1.f,
                          m_hexFocus ? Colors::Accent : Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4] = {225, 225, 232, 230};
            JTextHelper::pushText(buf, r.hex.x + 10.f, r.hex.y + (r.hex.h - JTextHelper::lineHeight()) * 0.5f,
                                  m_hex.empty() ? "#______" : m_hex, tc, r.hex.w - 14.f);
        }

        // ---- Hue bar: six vertical gradient segments ----
        static const uint8_t stops[7][4] = {
            {255,0,0,255},{255,255,0,255},{0,255,0,255},{0,255,255,255},{0,0,255,255},{255,0,255,255},{255,0,0,255} };
        const float segH = r.hue.h / 6.f;
        for (int i = 0; i < 6; ++i)
            _quad(buf, {r.hue.x, r.hue.y + i * segH, r.hue.w, segH}, stops[i], stops[i + 1], stops[i + 1], stops[i]);
        buf.pushRectangle(r.hue.x, r.hue.y, r.hue.w, r.hue.h, kNoFill, 3.f, 1.f, Colors::Border);
        _handleMark(buf, r.hue.x - 2.f, r.hue.y + m_h * r.hue.h, r.hue.w + 4.f);

        // ---- SV square: one bilinear quad, white/hue across → black down ----
        uint8_t hue[4]; _hsvToRgb(m_h, 1.f, 1.f, hue);
        uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
        _quad(buf, r.sv, white, hue, black, black);
        buf.pushRectangle(r.sv.x, r.sv.y, r.sv.w, r.sv.h, kNoFill, 3.f, 1.f, Colors::Border);
        const float cxp = r.sv.x + m_s * r.sv.w, cyp = r.sv.y + (1.f - m_v) * r.sv.h;
        uint8_t ring[4] = {255, 255, 255, 255};
        buf.pushRectangle(cxp - 6.f, cyp - 6.f, 12.f, 12.f, kNoFill, 6.f, 2.f, ring);

        // ---- Alpha slider: checkerboard, then transparent→opaque gradient, then handle ----
        _checker(buf, r.alpha);
        uint8_t a0[4] = {cur[0], cur[1], cur[2], 0}, a1[4] = {cur[0], cur[1], cur[2], 255};
        _quad(buf, r.alpha, a0, a1, a1, a0);
        buf.pushRectangle(r.alpha.x, r.alpha.y, r.alpha.w, r.alpha.h, kNoFill, 4.f, 1.f, Colors::Border);
        _handleDot(buf, r.alpha.x + m_a * r.alpha.w, r.alpha.y + r.alpha.h * 0.5f);
    }


    static constexpr float kW = 328.f, kH = 356.f;

    // ---- Public colour helpers (shared with the palette dialog) ----
    static std::string hexFromHsv(float h, float s, float v) {
        uint8_t c[4]; _hsvToRgb(h, s, v, c);
        char t[8]; std::snprintf(t, sizeof(t), "#%02x%02x%02x", c[0], c[1], c[2]);
        return t;
    }
    static bool parseHex(const std::string& s, uint8_t out[4]) { return _parse(s, out); }

private:
    struct Rc { float x, y, w, h; };
    struct R { Rc eye, preview, hex, hue, sv, alpha; };
    R _regions() const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float pad = 14.f, topH = 34.f, gap = 12.f, barW = 26.f, alphaH = 26.f;
        const float bodyY = b.y + pad + topH + gap;
        const float svH   = b.height - pad - topH - gap - alphaH - gap - pad;
        R r;
        r.eye     = { b.x + pad,                        b.y + pad, topH, topH };
        r.preview = { r.eye.x + topH + 8.f,             b.y + pad, 60.f, topH };
        r.hex     = { r.preview.x + 60.f + 8.f,         b.y + pad, b.x + b.width - (r.preview.x + 60.f + 8.f) - pad, topH };
        r.hue     = { b.x + pad,                        bodyY, barW, svH };
        r.sv      = { r.hue.x + barW + gap,             bodyY, b.x + b.width - (r.hue.x + barW + gap) - pad, svH };
        r.alpha   = { b.x + pad,                        bodyY + svH + gap, b.width - pad * 2.f, alphaH };
        return r;
    }
    static bool _in(const Rc& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

    void _setSV(float mx, float my, const R& r) {
        m_s = std::clamp((mx - r.sv.x) / r.sv.w, 0.f, 1.f);
        m_v = std::clamp(1.f - (my - r.sv.y) / r.sv.h, 0.f, 1.f);
        _emit();
    }
    void _setHue(float my, const R& r)  { m_h = std::clamp((my - r.hue.y) / r.hue.h, 0.f, 1.f); _emit(); }
    void _setAlpha(float mx, const R& r){ m_a = std::clamp((mx - r.alpha.x) / r.alpha.w, 0.f, 1.f); _emit(); }
    void _emit() { _syncHex(); m_graph.invalidateNode(m_nodeId, DirtySelf); onColorChanged.emit(m_hex); }
    void _syncHex() { m_hex = hexFromHsv(m_h, m_s, m_v); }
    void _tryCommitHex(bool force = false) {
        uint8_t c[4];
        if (_parse(m_hex, c)) { _rgbToHsv(c[0], c[1], c[2]); onColorChanged.emit(m_hex); }
        else if (force)       { _syncHex(); }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // ---- primitive helpers ----
    static void _quad(JPrimitiveBuffer& buf, const Rc& r,
                      const uint8_t tl[4], const uint8_t tr[4], const uint8_t br[4], const uint8_t bl[4]) {
        auto v = [](float x, float y, const uint8_t c[4]) {
            JRenderVertex vx{}; vx.position[0] = x; vx.position[1] = y;
            vx.color[0] = c[0]; vx.color[1] = c[1]; vx.color[2] = c[2]; vx.color[3] = c[3]; return vx; };
        const float ax = r.x, ay = r.y, bx = r.x + r.w, by = r.y + r.h;
        std::vector<JRenderVertex> vs = {
            v(ax, ay, tl), v(bx, ay, tr), v(bx, by, br),
            v(ax, ay, tl), v(bx, by, br), v(ax, by, bl) };
        buf.pushGeometry(std::move(vs));
    }
    static void _checker(JPrimitiveBuffer& buf, const Rc& r) {
        buf.pushClip(r.x, r.y, r.w, r.h);
        const float cs = 7.f;
        uint8_t light[4] = {150, 150, 156, 255}, dark[4] = {90, 90, 96, 255};
        for (int row = 0; row * cs < r.h; ++row)
            for (int col = 0; col * cs < r.w; ++col)
                buf.pushRectangle(r.x + col * cs, r.y + row * cs, cs, cs, ((row + col) & 1) ? dark : light, 0.f);
        buf.popClip();
    }
    static void _handleMark(JPrimitiveBuffer& buf, float x, float y, float w) {
        uint8_t c[4] = {255, 255, 255, 255};
        buf.pushRectangle(x, y - 1.5f, w, 3.f, c, 1.5f, 1.f, Colors::Border);
    }
    static void _handleDot(JPrimitiveBuffer& buf, float cx, float cy) {
        uint8_t c[4] = {255, 255, 255, 255};
        buf.pushRectangle(cx - 7.f, cy - 9.f, 14.f, 18.f, c, 7.f, 1.f, Colors::Border);
    }
    static void _drawEyedropper(JPrimitiveBuffer& buf, const Rc& r) {
        JVectorCanvas vg;
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        const JColor ink = jf::rgb(210, 210, 218);
        vg.strokePolyline({ {cx + 5.f, cy - 6.f}, {cx - 4.f, cy + 5.f} }, 2.5f, JPaint::solid(ink));  // stem
        vg.fillCircle(cx + 6.f, cy - 6.f, 3.2f, JPaint::solid(ink));                                   // bulb
        vg.flush(buf);
    }

    // ---- colour maths ----
    static bool _parse(const std::string& s, uint8_t o[4]) {
        if (s.size() != 7 || s[0] != '#') return false;
        auto hx = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
        int v[6]; for (int i = 0; i < 6; ++i) { v[i] = hx(s[i + 1]); if (v[i] < 0) return false; }
        o[0] = uint8_t(v[0] * 16 + v[1]); o[1] = uint8_t(v[2] * 16 + v[3]); o[2] = uint8_t(v[4] * 16 + v[5]); o[3] = 255; return true;
    }
    static void _hsvToRgb(float h, float s, float v, uint8_t out[4]) {
        float r = v, g = v, b = v;
        if (s > 0.f) {
            const float hh = (h >= 1.f ? 0.f : h) * 6.f;
            const int   i  = static_cast<int>(hh);
            const float f  = hh - i;
            const float p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
            switch (i) {
                case 0: r = v; g = t; b = p; break;
                case 1: r = q; g = v; b = p; break;
                case 2: r = p; g = v; b = t; break;
                case 3: r = p; g = q; b = v; break;
                case 4: r = t; g = p; b = v; break;
                default: r = v; g = p; b = q; break;
            }
        }
        out[0] = uint8_t(std::lround(r * 255.f)); out[1] = uint8_t(std::lround(g * 255.f));
        out[2] = uint8_t(std::lround(b * 255.f)); out[3] = 255;
    }
    void _rgbToHsv(uint8_t R, uint8_t G, uint8_t B) {
        const float r = R / 255.f, g = G / 255.f, b = B / 255.f;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        m_v = mx;
        m_s = mx <= 0.f ? 0.f : d / mx;
        if (d <= 0.f) { m_h = 0.f; return; }
        float h;
        if (mx == r)      h = (g - b) / d + (g < b ? 6.f : 0.f);
        else if (mx == g) h = (b - r) / d + 2.f;
        else              h = (r - g) / d + 4.f;
        m_h = h / 6.f;
    }

    static inline const uint8_t kNoFill[4] = {0, 0, 0, 0};

    float m_h{0.f}, m_s{0.f}, m_v{1.f}, m_a{1.f};
    std::string m_hex;
    bool m_dragSV{false}, m_dragHue{false}, m_dragA{false}, m_hexFocus{false};
};

} // inline namespace jf
