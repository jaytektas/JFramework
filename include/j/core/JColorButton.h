#pragma once

// JColorButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JColorButton — the property-inspector colour field. The whole control IS the swatch: it fills with
// the chosen colour and prints its hex in a contrast-picked text colour (black on light, white on dark).
// Unset ("") means "inherit the scheme" and shows "(scheme)" on a plain surface. Click opens a picker
// (the app wires onOpenPickerHook → a JColorPicker popup); when inheritable, an inline ✕ clears to
// scheme without opening the picker. Emits onColorChanged("#rrggbb" | "") whenever the value changes.
// Dumb-by-design, exactly like JComboBox: it holds the value + requests the popup, the app owns it.
// ============================================================================

class JColorButton : public JControl {
public:
    static inline std::function<void(JColorButton*)> onOpenPickerHook;
    jf::JSignal<std::string> onColorChanged;

    JColorButton(JSceneGraph& graph, float w = 200.0f, float h = 0.0f) : JControl(graph, "JColorButton") {
        auto& l = m_graph.getLayout(m_nodeId); l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
    }
    void setInheritable(bool v) { m_inheritable = v; }
    bool inheritable() const    { return m_inheritable; }

    void setColorHex(const std::string& hex) { if (m_hex != hex) { m_hex = hex; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    const std::string& colorHex() const { return m_hex; }
    // Programmatic set that fires the change signal (used by the picker + clear button).
    void pick(const std::string& hex) { if (m_hex != hex) { m_hex = hex; m_graph.invalidateNode(m_nodeId, DirtySelf); onColorChanged.emit(hex); } }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        if (m_inheritable && !m_hex.empty()) {   // inline ✕ hit region on the right clears to scheme
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (mx >= b.x + b.width - b.height) { pick(""); return; }
        }
        if (onOpenPickerHook) onOpenPickerHook(this);
    }
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const bool focused = isFocused();
        uint8_t c[4];
        if (_parse(m_hex, c)) {
            buf.pushRectangle(b.x, b.y, b.width, b.height, c, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4] = {0, 0, 0, 230};
                if (_luma(c) < 0.5f) { tc[0] = tc[1] = tc[2] = 240; }   // white text on dark fills
                JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, m_hex, tc, b.width - 16.0f);
            }
        } else {
            buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
            if (JTextHelper::hasAtlas()) {
                uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
                JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "(scheme)", tc, b.width - 16.0f);
            }
        }
        // Inline clear affordance (only when inheritable + a colour is set).
        if (m_inheritable && !m_hex.empty() && JTextHelper::hasAtlas()) {
            uint8_t xc[4] = {0, 0, 0, 200};
            if (_parse(m_hex, c) && _luma(c) < 0.5f) { xc[0] = xc[1] = xc[2] = 235; }
            JTextHelper::pushText(buf, b.x + b.width - b.height + 6.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "x", xc, b.height);
        }
    }

private:
    static bool _parse(const std::string& s, uint8_t o[4]) {
        if (s.size() != 7 || s[0] != '#') return false;
        auto hx = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
        int v[6]; for (int i = 0; i < 6; ++i) { v[i] = hx(s[i + 1]); if (v[i] < 0) return false; }
        o[0] = uint8_t(v[0] * 16 + v[1]); o[1] = uint8_t(v[2] * 16 + v[3]); o[2] = uint8_t(v[4] * 16 + v[5]); o[3] = 255; return true;
    }
    static float _luma(const uint8_t c[4]) { return (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255.0f; }
    std::string m_hex;
    bool        m_inheritable{false};
};

} // inline namespace jf
