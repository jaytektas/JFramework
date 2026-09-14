// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once
// JPickerField — a field whose value is chosen somewhere else.
//
// NOT A COMBO BOX, and that is the whole point. A combo's chevron promises a list that drops down under
// it; this control opens a searchable dialog, because its list is the several hundred channels of a
// signal catalogue or the 167 maps of a table registry. Dressing that up as a combo made the control
// lie about what pressing it does — and left nowhere to put a CLEAR, because a combo's only affordance
// was already spoken for.
//
// So it says what it is: an ellipsis well (…, "there is more, elsewhere") instead of a chevron, the
// chosen value in the field, and an ✕ to unchoose. Three regions, three unambiguous meanings.
//
//   onOpenRequested — the field or the … was pressed; the host opens whatever picker it owns
//   onCleared       — the ✕ was pressed; the host unsets its value
//
// The host owns the value: this draws the text it is given and asks to be changed. It never holds a
// list, which is why it does not care how long one is.

#include "JControl.h"
#include "JTextHelper.h"
#include "KeyEvent.h"
#include <string>
#include <utility>

namespace jf {

class JPickerField : public JControl {
public:
    JSignal<> onOpenRequested;
    JSignal<> onCleared;

    JPickerField(JSceneGraph& graph, float w = 200.0f, float h = 0.0f)
        : JControl(graph, "JPickerField") {
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width = w;
        l.boundingBox.height = (h > 0.0f) ? h : JStyle::current().controlHeight;
        l.minHeight = l.boundingBox.height;
    }

    // What is currently chosen. Empty means nothing is, and the placeholder shows instead.
    void setText(std::string t) {
        if (m_text == t) return;
        m_text = std::move(t);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
        notifyAccessibility();
    }
    const std::string& text() const { return m_text; }

    // What to show when nothing is chosen. "Not assigned" reads better than an empty box, which is
    // indistinguishable from a control that has failed to load.
    void setPlaceholder(std::string t) {
        if (m_placeholder == t) return;
        m_placeholder = std::move(t);
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    // Offer the ✕ at all. It then draws only while there is something to clear, so an empty field never
    // shows a control that would do nothing.
    void setClearable(bool on) {
        if (m_clearable == on) return;
        m_clearable = on;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }
    bool clearable() const { return m_clearable; }

    void handleMousePress(float mx, float my) override {
        if (!isPointInside(mx, my)) return;
        if (acceptsClickFocus()) requestFocus();
        // THE ✕ FIRST. It sits inside the field, so anywhere-opens-the-picker would swallow it — which is
        // exactly what happened when this was a combo and its host intercepted the press above it.
        if (_clearShown() && _inClear(mx, my)) { onCleared.emit(); return; }
        onOpenRequested.emit();
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        const bool over = _clearShown() && _inClear(mx, my);
        if (over != m_hoverClear) { m_hoverClear = over; m_graph.invalidateNode(m_nodeId, DirtySelf); }
    }

    bool handleKeyEvent(const JKeyEvent& ke) override {
        if (!ke.pressed) return false;
        using K = JKeyEvent::JKey;
        if (ke.key == K::Space || ke.key == K::Return) { onOpenRequested.emit(); return true; }
        // Delete unsets, which is what Delete means in every field that can be empty.
        if (m_clearable && !m_text.empty() && (ke.key == K::Delete || ke.key == K::Backspace)) {
            onCleared.emit();
            return true;
        }
        return JControl::handleKeyEvent(ke);
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const bool focused = isFocused();
        const JStyleOption o = jstyle::option(m_state, focused);
        const float wellW = _wellW(b);
        const float inset = JStyle::current().hint(JStyleHint::ControlInsetY);

        // The body is a FIELD, recessed like every other input — a spin box, a line edit, a combo — so a
        // column of settings reads as one column and not as a row of buttons.
        buf.pushRectangle(b.x, b.y + inset, b.width, b.height - 2.0f * inset,
                          jstyle::fieldFill(o).data(), JStyle::current().hint(JStyleHint::ControlRadius),
                          jstyle::borderW(focused), jstyle::border(o).data());
        // …and the well that opens the picker is raised, the way a combo's arrow well is.
        buf.pushRectangle(b.x + b.width - wellW, b.y + 1.0f, wellW - 1.0f, b.height - 2.0f,
                          jstyle::role(JColorRole::ToolTipBase, o).data(), 5.0f);

        // AN ELLIPSIS, NOT A CHEVRON. Three dots is the universal "opens something else"; a chevron would
        // promise a list that drops down, which is the lie this control exists to stop telling.
        const float ex = b.x + b.width - wellW * 0.5f, ey = b.y + b.height * 0.5f - 1.0f;
        const uint8_t dc[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], 230};
        for (int k = -1; k <= 1; ++k)
            buf.pushRectangle(ex + static_cast<float>(k) * 5.0f - 1.0f, ey, 2.0f, 2.0f, dc, 1.0f);

        // The ✕, in its own square left of the well. Brighter under the pointer, so it reads as its own
        // target rather than as decoration on the field.
        if (_clearShown()) {
            const float cx = _clearX(b) + wellW * 0.5f, cy = b.y + b.height * 0.5f;
            const uint8_t a = m_hoverClear ? 255 : 170;
            const uint8_t xc[4] = {Colors::MutedText[0], Colors::MutedText[1], Colors::MutedText[2], a};
            for (int k = -3; k <= 3; ++k) {
                const float d = static_cast<float>(k);
                buf.pushRectangle(cx + d - 1.0f, cy + d - 1.0f, 2.0f, 2.0f, xc, 0.5f);
                buf.pushRectangle(cx + d - 1.0f, cy - d - 1.0f, 2.0f, 2.0f, xc, 0.5f);
            }
        }

        if (!JTextHelper::hasAtlas()) return;
        const float avail = b.width - wellW - textPadding() - 6.0f - (_clearShown() ? wellW : 0.0f);
        const bool  empty = m_text.empty();
        const std::string shown = empty ? m_placeholder : m_text;
        if (shown.empty()) return;
        // The placeholder is muted: "nothing chosen" must not read as a value.
        const uint8_t* base = empty ? Colors::MutedText : Colors::FieldText;
        const uint8_t tc[4] = {base[0], base[1], base[2], static_cast<uint8_t>(empty ? 170 : 220)};
        JTextHelper::pushText(buf, b.x + textPadding(),
                              b.y + (b.height - JTextHelper::lineHeight()) * 0.5f, shown, tc, avail);
    }

private:
    bool  _clearShown() const { return m_clearable && !m_text.empty(); }
    float _wellW(const JRect& b) const { return b.height * 0.75f; }
    float _clearX(const JRect& b) const { return b.x + b.width - _wellW(b) * 2.0f; }
    bool  _inClear(float mx, float my) const {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float x = _clearX(b);
        return mx >= x && mx < x + _wellW(b) && my >= b.y && my < b.y + b.height;
    }

    std::string m_text, m_placeholder;
    bool m_clearable  = false;
    bool m_hoverClear = false;
};

}   // namespace jf
