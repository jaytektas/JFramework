#pragma once

// JFontButton.

#include "JControl.h"
#include "JTextHelper.h"

inline namespace jf {

// ============================================================================
// JFontButton — the property-inspector font field (mirrors JColorButton). Shows the current font
// spec ("Family 12 Bold") or "(scheme)" when unset; click opens a picker via onOpenPickerHook (the app
// wires it to a font dialog). Inheritable → inline ✕ clears to scheme. Emits onFontChanged(spec).
// The spec is a compact string "family|size|b|i" (b/i = 1/0); label() renders it human-readably.
// ============================================================================
class JFontButton : public JControl {
public:
    static inline std::function<void(JFontButton*)> onOpenPickerHook;
    jf::JSignal<std::string> onFontChanged;

    JFontButton(JSceneGraph& graph, float w = 200.0f, float h = 0.0f) : JControl(graph, "JFontButton") {
        auto& l = m_graph.getLayout(m_nodeId); l.boundingBox.width = w; l.boundingBox.height = (h > 0.0f) ? h : JTheme::current().controlHeight;
    }
    void setInheritable(bool v) { m_inheritable = v; }
    void setFontSpec(const std::string& s) { if (m_spec != s) { m_spec = s; m_graph.invalidateNode(m_nodeId, DirtySelf); } }
    const std::string& fontSpec() const { return m_spec; }
    void pick(const std::string& s) { if (m_spec != s) { m_spec = s; m_graph.invalidateNode(m_nodeId, DirtySelf); onFontChanged.emit(s); } }

    // Human-readable label from the "family|size|b|i" spec.
    static std::string prettify(const std::string& spec) {
        if (spec.empty()) return "(scheme)";
        std::string out; size_t p = 0; int field = 0; std::string fam, sz; bool b = false, i = false;
        while (p <= spec.size()) { const size_t c = spec.find('|', p); const std::string t = spec.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (field == 0) fam = t; else if (field == 1) sz = t; else if (field == 2) b = (t == "1"); else if (field == 3) i = (t == "1");
            ++field; if (c == std::string::npos) break; p = c + 1; }
        out = fam.empty() ? "Default" : fam; if (!sz.empty()) out += " " + sz; if (b) out += " Bold"; if (i) out += " Italic";
        return out;
    }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        onClicked.emit();
        if (m_inheritable && !m_spec.empty()) {
            const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
            if (mx >= b.x + b.width - b.height) { pick(""); return; }
        }
        if (onOpenPickerHook) onOpenPickerHook(this);
    }
    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const bool focused = isFocused();
        buf.pushRectangle(b.x, b.y, b.width, b.height, Colors::Surface2, 4.0f, focused ? 1.5f : 1.0f, focused ? Colors::Accent : Colors::Border);
        if (JTextHelper::hasAtlas()) {
            uint8_t tc[4]; std::copy(m_spec.empty() ? Colors::TextSecondary : Colors::TextPrimary, (m_spec.empty() ? Colors::TextSecondary : Colors::TextPrimary) + 4, tc);
            JTextHelper::pushText(buf, b.x + 8.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, prettify(m_spec), tc, b.width - 16.0f);
            if (m_inheritable && !m_spec.empty()) {
                uint8_t xc[4] = {Colors::LabelText[0], Colors::LabelText[1], Colors::LabelText[2], 200};
                JTextHelper::pushText(buf, b.x + b.width - b.height + 6.0f, b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, "x", xc, b.height);
            }
        }
    }

private:
    std::string m_spec;
    bool        m_inheritable{false};
};

} // inline namespace jf
