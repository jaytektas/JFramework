#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include "JTextHelper.h"
#include "JWidget.h"
#include "JControl.h"
#include "KeyEvent.h"
#include "Signal.h"
#include "../graphics/RenderPrimitive.h"
#include "../graphics/FontEngine.h"
#include "../graphics/GpuHal.h"

#if defined(_WIN32)
#include "../platforms/windows/WindowsPlatformWindow.h"
#else
#include "../platforms/linux/LinuxPlatformWindow.h"
#endif

inline namespace jf {

// ============================================================================
// JMenuShortcut — keyboard shortcut representation and matching
// ============================================================================
struct JMenuShortcut {
    JKeyEvent::JKey key{JKeyEvent::JKey::Unknown};
    bool ctrl{false};
    bool alt{false};
    bool shift{false};

    bool matches(const JKeyEvent& ke) const {
        return ke.key == key && ke.ctrl == ctrl && ke.alt == alt && ke.shift == shift;
    }

    std::string toString() const {
        if (key == JKeyEvent::JKey::Unknown) return "";
        std::string s;
        if (ctrl) s += "Ctrl+";
        if (alt) s += "Alt+";
        if (shift) s += "Shift+";
        using K = JKeyEvent::JKey;
        switch (key) {
            case K::Tab: s += "Tab"; break;
            case K::Return: s += "Enter"; break;
            case K::Space: s += "Space"; break;
            case K::Escape: s += "Esc"; break;
            case K::Backspace: s += "Backspace"; break;
            case K::Delete: s += "Del"; break;
            case K::Left: s += "Left"; break;
            case K::Right: s += "Right"; break;
            case K::Up: s += "Up"; break;
            case K::Down: s += "Down"; break;
            default:
                if (key >= K::A && key <= K::Z) {
                    s += static_cast<char>('A' + (static_cast<uint32_t>(key) - static_cast<uint32_t>(K::A)));
                } else if (key >= K::_0 && key <= K::_9) {
                    s += static_cast<char>('0' + (static_cast<uint32_t>(key) - static_cast<uint32_t>(K::_0)));
                } else {
                    s += "JKey";
                }
                break;
        }
        return s;
    }
};

class JMenu; // Forward decl

// ============================================================================
// JMenuItem — interactive menu entry with label, shortcut, icon or custom widget
// ============================================================================
class JMenuItem : public JControl {
public:
    jf::JSignal<> onTriggered;

    JMenuItem(JSceneGraph& graph, const std::string& label, JMenuShortcut shortcut = {}, JMenu* submenu = nullptr)
        : JControl(graph, "JMenuItem"), m_label(label), m_shortcut(shortcut), m_submenu(submenu)
    {
        auto& l = m_graph.getLayout(m_nodeId);
        l.minHeight = 28.f;
        l.minWidth = 160.f;
    }



    const std::string& label() const noexcept { return m_label; }
    const JMenuShortcut& shortcut() const noexcept { return m_shortcut; }
    JMenu* submenu() const noexcept { return m_submenu; }

    void setCheckable(bool c) noexcept { m_checkable = c; }
    bool isCheckable() const noexcept { return m_checkable; }
    void setChecked(bool c) noexcept { m_checked = c; }
    bool isChecked() const noexcept { return m_checked; }

    void setEmbeddedWidgetFactory(std::function<std::unique_ptr<JWidget>(JSceneGraph&)> factory) {
        m_embeddedFactory = std::move(factory);
        if (m_embeddedFactory) {
            m_embeddedOwned = m_embeddedFactory(m_graph);
            m_embedded = m_embeddedOwned.get();
            m_graph.addChild(m_nodeId, m_embedded->getNodeId());
        }
    }
    const std::function<std::unique_ptr<JWidget>(JSceneGraph&)>& embeddedWidgetFactory() const noexcept {
        return m_embeddedFactory;
    }
    JWidget* embeddedWidget() const noexcept { return m_embedded; }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        if (!m_visible) return;
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;

        // Draw hover backdrop
        if (m_state == JWidgetState::Hovered) {
            uint8_t hoverBg[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 50};
            buf.pushRectangle(bb.x, bb.y, bb.width, bb.height, hoverBg, 4.0f);
        }

        float textX = bb.x + 8.0f;
        if (m_checkable) {
            uint8_t boxColor[4] = {Colors::Surface3[0], Colors::Surface3[1], Colors::Surface3[2], 255};
            buf.pushRectangle(bb.x + 8.0f, bb.y + (bb.height - 12.0f) * 0.5f, 12.0f, 12.0f, boxColor, 2.0f);
            if (m_checked) {
                uint8_t checkColor[4] = {Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], 255};
                buf.pushRectangle(bb.x + 10.0f, bb.y + (bb.height - 8.0f) * 0.5f, 8.0f, 8.0f, checkColor, 1.0f);
            }
            textX += 20.0f;
        }

        if (m_embedded) {
            // Reposition embedded widget layout bounding box to fit inside JMenuItem
            auto& el = m_graph.getLayout(m_embedded->getNodeId());
            el.boundingBox.x = textX;
            el.boundingBox.y = bb.y + (bb.height - el.boundingBox.height) * 0.5f;
            el.boundingBox.width = bb.width - (textX - bb.x) - 8.f;
            m_embedded->populateRenderPrimitives(buf);
        } else {
            uint8_t textColor[4];
            if (m_state == JWidgetState::Disabled) {
                std::copy(Colors::TextSecondary, Colors::TextSecondary + 4, textColor);
            } else {
                std::copy(Colors::TextPrimary, Colors::TextPrimary + 4, textColor);
            }
            float labelY = bb.y + (bb.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, textX, labelY, m_label, textColor);

            if (m_shortcut.key != JKeyEvent::JKey::Unknown) {
                std::string scStr = m_shortcut.toString();
                float scW = JTextHelper::measureWidth(scStr);
                float scX = bb.x + bb.width - scW - 12.0f;
                uint8_t scColor[4] = {Colors::TextSecondary[0], Colors::TextSecondary[1], Colors::TextSecondary[2], 180};
                JTextHelper::pushText(buf, scX, labelY, scStr, scColor);
            }

            if (m_submenu) {
                uint8_t arrowColor[4] = {Colors::TextSecondary[0], Colors::TextSecondary[1], Colors::TextSecondary[2], 255};
                float arrowX = bb.x + bb.width - 16.0f;
                JTextHelper::pushText(buf, arrowX, labelY, ">", arrowColor);
            }
        }
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        if (m_embedded) {
            m_embedded->handleMouseMove(mx, my);
        }
    }

    void handleMousePress(float mx, float my) override {
        if (m_state == JWidgetState::Disabled) return;
        if (isPointInside(mx, my)) {
            if (m_embedded) {
                m_embedded->handleMousePress(mx, my);
            } else {
                setState(JWidgetState::Pressed);
                if (m_checkable) {
                    m_checked = !m_checked;
                }
                onTriggered.emit();
                onClicked.emit();
            }
        }
    }

    void handleMouseRelease(float mx, float my) override {
        JControl::handleMouseRelease(mx, my);
        if (m_embedded) {
            m_embedded->handleMouseRelease(mx, my);
        }
    }

private:
    std::string m_label;
    JMenuShortcut m_shortcut;
    JMenu* m_submenu{nullptr};
    bool m_checkable{false};
    bool m_checked{false};
    std::function<std::unique_ptr<JWidget>(JSceneGraph&)> m_embeddedFactory;
    std::unique_ptr<JWidget> m_embeddedOwned;
    JWidget* m_embedded{nullptr};
};

// ============================================================================
// JMenuSeparator — thin divider widget
// ============================================================================
class JMenuSeparator : public JWidget {
public:
    JMenuSeparator(JSceneGraph& graph) : JWidget(graph, "JMenuSeparator") {
        auto& l = m_graph.getLayout(m_nodeId);
        l.minHeight = 6.0f;
        l.minWidth = 150.0f;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        uint8_t lineC[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], 120};
        buf.pushRectangle(bb.x + 6.0f, bb.y + 2.0f, bb.width - 12.0f, 1.0f, lineC);
    }

};

// ============================================================================
// JTearOffHandle — handles drag/click events to detatch menu into free window
// ============================================================================
class JTearOffHandle : public JControl {
public:
    jf::JSignal<> onTornOff;

    JTearOffHandle(JSceneGraph& graph) : JControl(graph, "JTearOffHandle") {
        auto& l = m_graph.getLayout(m_nodeId);
        l.minHeight = 12.f;
        l.minWidth = 150.f;
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        uint8_t lineC[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], 180};
        buf.pushRectangle(bb.x + 15.f, bb.y + 4.f, bb.width - 30.f, 1.f, lineC);
        buf.pushRectangle(bb.x + 15.f, bb.y + 7.f, bb.width - 30.f, 1.f, lineC);
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            onTornOff.emit();
        }
    }

};

// ============================================================================
// JMenu — structural model containing items & submenus
// ============================================================================
class JMenu {
public:
    JMenu(const std::string& title) : m_title(title) {}

    const std::string& title() const noexcept { return m_title; }

    void add(std::unique_ptr<JMenuItem> item) {
        m_items.push_back(std::move(item));
    }

    JMenuItem* add(JSceneGraph& graph, const std::string& label, JMenuShortcut shortcut = {}, JMenu* submenu = nullptr) {
        auto item = std::make_unique<JMenuItem>(graph, label, shortcut, submenu);
        JMenuItem* ptr = item.get();
        m_items.push_back(std::move(item));
        return ptr;
    }

    void addSeparator(JSceneGraph& graph) {
        m_items.push_back(std::make_unique<JMenuSeparator>(graph));
    }

    const std::vector<std::unique_ptr<JWidget>>& items() const noexcept { return m_items; }

    void setTearOffEnabled(bool enable) noexcept { m_tearOffEnabled = enable; }
    bool isTearOffEnabled() const noexcept { return m_tearOffEnabled; }

private:
    std::string m_title;
    std::vector<std::unique_ptr<JWidget>> m_items;
    bool m_tearOffEnabled{true};
};

// ============================================================================
// JMenuManager — tracks registered shortcuts, context actions & tear-off windows
// ============================================================================
class JMenuManager {
public:
    static JMenuManager& instance() {
        static JMenuManager inst;
        return inst;
    }

    std::function<void(JMenu* menu, int sx, int sy, bool parentTorn)> onOpenMenu;
    std::function<void(JMenu* menu, int sx, int sy)> onTearOffMenu;

    struct JShortcutReg {
        JMenuShortcut shortcut;
        std::function<void()> callback;
    };

    void registerShortcut(const JMenuShortcut& sc, std::function<void()> callback) {
        if (sc.key != JKeyEvent::JKey::Unknown) {
            m_shortcuts.push_back({sc, std::move(callback)});
        }
    }

    bool processAccelerator(const JKeyEvent& ke) {
        for (const auto& reg : m_shortcuts) {
            if (reg.shortcut.matches(ke)) {
                reg.callback();
                return true;
            }
        }
        return false;
    }

    void clearShortcuts() { m_shortcuts.clear(); }

    // Global tearoff switch — overrides per-menu setting when false.
    void setTearOffEnabled(bool v) noexcept { m_globalTearOff = v; }
    bool isTearOffEnabled() const noexcept  { return m_globalTearOff; }

    // Fired when a widget requests its context menu (local widget coords).
    // The receiver (main loop) should offset by window screen position.
    std::function<void(JMenu*, float localX, float localY)> onContextMenuRequested;

private:
    JMenuManager() = default;
    std::vector<JShortcutReg> m_shortcuts;
    bool m_globalTearOff{true};
};

// ============================================================================
// JMenuBar — top level horizontal strip widget
// ============================================================================
class JMenuBar : public JControl {
public:
    std::function<std::pair<int, int>(float, float)> onQueryScreenPos;

    JMenuBar(JSceneGraph& graph) : JControl(graph, "JMenuBar") {
        auto& l = m_graph.getLayout(m_nodeId);
        l.direction = JFlexDirection::JRow;
        l.minHeight = 32.f;
        l.flexGrow = 0.0f;
    }



    struct JMenuEntry {
        std::string title;
        JMenu* menu;
        NodeId btnId;
    };

    void addMenu(JMenu* menu) {
        NodeId btnId = m_graph.createNode("MenuBarBtn");
        auto& l = m_graph.getLayout(btnId);
        l.minWidth = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(menu->title()) + 20.f : 70.f;
        l.minHeight = 32.f;
        m_graph.addChild(m_nodeId, btnId);
        m_entries.push_back({menu->title(), menu, btnId});
    }

    void populateRenderPrimitives(JPrimitiveBuffer& buf) override {
        const auto& bb = m_graph.getLayoutConst(m_nodeId).boundingBox;
        uint8_t barBg[4] = {Colors::Surface1[0], Colors::Surface1[1], Colors::Surface1[2], 255};
        buf.pushRectangle(bb.x, bb.y, bb.width, bb.height, barBg, 0.0f);

        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& entry = m_entries[i];
            const auto& btnBB = m_graph.getLayoutConst(entry.btnId).boundingBox;

            bool isHovered = (m_activeIdx == static_cast<int>(i)) || (m_hoveredIdx == static_cast<int>(i));
            if (isHovered) {
                uint8_t hoverBg[4] = {Colors::Surface3[0], Colors::Surface3[1], Colors::Surface3[2], 255};
                buf.pushRectangle(btnBB.x + 2.f, btnBB.y + 2.f, btnBB.width - 4.f, btnBB.height - 4.f, hoverBg, 4.0f);
            }

            uint8_t tc[4] = {Colors::TextPrimary[0], Colors::TextPrimary[1], Colors::TextPrimary[2], 255};
            float ty = btnBB.y + (btnBB.height - JTextHelper::lineHeight()) * 0.5f;
            JTextHelper::pushText(buf, btnBB.x + 10.f, ty, entry.title, tc);
        }
    }

    void handleMouseMove(float mx, float my) override {
        JControl::handleMouseMove(mx, my);
        m_hoveredIdx = -1;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& btnBB = m_graph.getLayoutConst(m_entries[i].btnId).boundingBox;
            if (mx >= btnBB.x && mx <= btnBB.x + btnBB.width && my >= btnBB.y && my <= btnBB.y + btnBB.height) {
                m_hoveredIdx = static_cast<int>(i);
                break;
            }
        }

        if (m_hoveredIdx != -1 && m_activeIdx != -1 && m_hoveredIdx != m_activeIdx) {
            openMenu(m_hoveredIdx);
        }
    }

    void handleMousePress(float mx, float my) override {
        if (isPointInside(mx, my)) {
            for (size_t i = 0; i < m_entries.size(); ++i) {
                const auto& btnBB = m_graph.getLayoutConst(m_entries[i].btnId).boundingBox;
                if (mx >= btnBB.x && mx <= btnBB.x + btnBB.width && my >= btnBB.y && my <= btnBB.y + btnBB.height) {
                    if (m_activeIdx == static_cast<int>(i)) {
                        m_activeIdx = -1;
                        if (JMenuManager::instance().onOpenMenu) {
                            JMenuManager::instance().onOpenMenu(nullptr, 0, 0, false);
                        }
                    } else {
                        openMenu(static_cast<int>(i));
                    }
                    m_graph.invalidateNode(m_nodeId, DirtySelf);
                    break;
                }
            }
        }
    }

    void closeMenu() noexcept {
        m_activeIdx  = -1;
        m_hoveredIdx = -1;
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    const std::vector<JMenuEntry>& entries() const noexcept { return m_entries; }
    int activeIndex() const noexcept { return m_activeIdx; }

private:
    void openMenu(int index) {
        m_activeIdx = index;
        if (JMenuManager::instance().onOpenMenu && onQueryScreenPos) {
            const auto& entry = m_entries[index];
            const auto& btnBB = m_graph.getLayoutConst(entry.btnId).boundingBox;
            auto [sx, sy] = onQueryScreenPos(btnBB.x, btnBB.y + btnBB.height);
            JMenuManager::instance().onOpenMenu(entry.menu, sx, sy, false);
        }
        m_graph.invalidateNode(m_nodeId, DirtySelf);
    }

    std::vector<JMenuEntry> m_entries;
    int m_activeIdx{-1};
    int m_hoveredIdx{-1};
};

} // inline namespace jf
