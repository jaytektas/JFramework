#pragma once

// JStatusBar — a framework-owned status strip along the window bottom. It shows a permanent text
// (e.g. an app/connection state) and can overlay a transient message that auto-reverts after a
// timeout. It can also host widgets, right-aligned (a live indicator, a progress control, …).
// JAppWindow owns one and drives its layout/render/input.

#include <j/core/JTextHelper.h>
#include <j/core/JWidget.h>
#include <j/core/Timer.h>                // transient-message auto-revert
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

inline namespace jf {

class JStatusBar {
public:
    JStatusBar() {
        // When the transient message's timer elapses, revert to the permanent text (the dispatcher
        // drain that delivered this tick arms a redraw, so the bar updates on screen).
        m_timer.onTick.connect([this]{ m_hasMessage = false; });
        // Live readout: each tick re-asks the provider for the right-aligned text (same dispatcher
        // redraw). Connected once here so repeated setLiveText() calls don't stack slots.
        m_liveTimer.onTick.connect([this]{ if (m_liveProvider) m_liveText = m_liveProvider(); });
    }

    // Permanent text — shown whenever no transient message is active.
    void setText(std::string s) { m_permanent = std::move(s); }

    // Transient message. ms > 0 auto-reverts to the permanent text after that many milliseconds;
    // ms == 0 shows it until the next setText/showMessage/clearMessage.
    void showMessage(std::string s, int ms = 0) {
        m_message = std::move(s);
        m_hasMessage = true;
        if (ms > 0) m_timer.start(std::chrono::milliseconds(ms), JTimer::JMode::SingleShot);
        else        m_timer.stop();
    }
    void clearMessage() { m_hasMessage = false; m_timer.stop(); }

    const std::string& text() const { return m_hasMessage ? m_message : m_permanent; }

    // Host a widget, laid out from the RIGHT edge (permanent). Parity with the toolbar; the widget
    // is non-owning and hit-tests itself.
    void addWidget(JWidget* w, float width) { m_widgets.push_back({ w, width, 0.f }); }

    // Live readout: a right-aligned text the bar refreshes itself, on an owned timer. `provider`
    // returns the current text (return "" to show nothing); the bar re-asks it every `intervalMs`
    // and only the changed text repaints. This is the built-in "permanent live label" — the app
    // supplies the string, the bar owns the timer/refresh. Pass a null provider to clear it.
    void setLiveText(std::function<std::string()> provider, int intervalMs = 250) {
        m_liveProvider = std::move(provider);
        m_liveText = m_liveProvider ? m_liveProvider() : std::string();   // seed now, don't wait a tick
        m_liveTimer.stop();
        if (m_liveProvider && intervalMs > 0)
            m_liveTimer.start(std::chrono::milliseconds(intervalMs));
    }

    void setRect(JRect r) { m_rect = r; }

    // Route input to hosted widgets. Returns true if the cursor is within the bar.
    bool handleMouse(float mx, float my, bool pressed, bool released) {
        layout();
        for (auto& it : m_widgets) {
            it.w->handleMouseMove(mx, my);
            if (pressed)  it.w->handleMousePress(mx, my);
            if (released) it.w->handleMouseRelease(mx, my);
        }
        return (mx >= m_rect.x && mx < m_rect.x + m_rect.width &&
                my >= m_rect.y && my < m_rect.y + m_rect.height);
    }

    void render(JPrimitiveBuffer& buf) {
        layout();
        const float h = m_rect.height;
        // Message / permanent text on the left.
        const std::string& t = text();
        if (JTextHelper::hasAtlas() && !t.empty()) {
            uint8_t tc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, tc);
            JTextHelper::pushText(buf, m_rect.x + 10.f,
                                  m_rect.y + (h - JTextHelper::lineHeight()) * 0.5f, t, tc);
        }
        // Live readout on the right (refreshed by m_liveTimer), then widgets to its left.
        if (JTextHelper::hasAtlas() && !m_liveText.empty()) {
            uint8_t lc[4]; std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, lc);
            JTextHelper::pushText(buf, m_liveX,
                                  m_rect.y + (h - JTextHelper::lineHeight()) * 0.5f, m_liveText, lc);
        }
        const float pad = 4.f;
        for (auto& it : m_widgets) {
            it.w->setBounds({ it.x, m_rect.y + pad, it.width, h - 2.f * pad });
            it.w->populateRenderPrimitives(buf);
        }
    }

private:
    void layout() {
        // Right-align from the bar's right edge inward: the live readout sits furthest right,
        // then widgets pack to its left.
        float x = m_rect.x + m_rect.width - kPad;
        if (JTextHelper::hasAtlas() && !m_liveText.empty()) {
            x -= JTextHelper::measureWidth(m_liveText);
            m_liveX = x;
            x -= kGap;
        }
        for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it) {
            x -= it->width;
            it->x = x;
            x -= kGap;
        }
    }

    struct WItem { JWidget* w; float width, x; };

    std::string          m_permanent, m_message;
    bool                 m_hasMessage{false};
    JTimer               m_timer;
    std::vector<WItem>   m_widgets;
    JRect                m_rect{};
    // Live readout (right-aligned, self-refreshing on m_liveTimer).
    std::function<std::string()> m_liveProvider;
    JTimer               m_liveTimer;
    std::string          m_liveText;
    float                m_liveX{0.f};
    static constexpr float kPad = 8.f, kGap = 6.f;
};

} // inline namespace jf
